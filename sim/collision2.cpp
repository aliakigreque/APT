// Per tick collision checks around the player: portals, orbs, pads, solids and hazards.

#include <array>
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#endif
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <cstdio>
#include <string>
#include "gd.hpp"
#include "timeline.hpp"
#include "triggers.hpp"

#include <algorithm>
#include <cmath>

namespace ap::sim {
    bool g_trace = false;
    int g_rngLogFrom = -1;
    int g_rngLogTo = -1;

    // prints an rng draw with its call site, relative to this function
    void rngLogDraw(int tick, void const* site) {
        std::fprintf(stderr, "[draw] tick %d site %p (rngLogDraw%+lld)\n", tick, site,
                     static_cast<long long>(reinterpret_cast<char const*>(site) -
                                            reinterpret_cast<char const*>(reinterpret_cast<void const*>(&rngLogDraw))));
    }

    static thread_local std::vector<int> tl_solids;
    static thread_local std::vector<int> tl_hazards;
    static thread_local std::vector<int> tl_dyn;
    static thread_local std::vector<int> tl_merged;
    static thread_local std::vector<int> tl_filtered;
    static constexpr float kFilterMargin = 24.f;
    static thread_local float tl_winLo = -1e30f;
    static thread_local float tl_winHi = 1e30f;
    static thread_local bool tl_needFull = false;

    namespace {
        // mixes a value into a hash
        inline uint64_t mixKey(uint64_t hash, uint64_t value) {
            hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
            hash *= 0xbf58476d1ce4e5b9ull;
            return hash ^ (hash >> 29);
        }

        struct CellCache {
            uint64_t key = 0;
            World::CellIndex ix;
        };
        thread_local std::vector<CellCache> tl_cellCache;
        thread_local int tl_cellCacheTick = -1;
        thread_local WKey tl_cellCacheWorld;
        thread_local size_t tl_cellCacheUsed = 0;
        thread_local long g_cellCacheBuilds = 0;

        // builds the vertical band index of a list at the current object positions
        void buildDynamicIndex(Game& g, std::vector<int> const& list, World::CellIndex& index) {
            World const& world = *g.world;
            for (auto& band : index.bands) band.clear();
            index.mov.clear();
            index.teleporter = false;
            index.byPos = false;
            for (size_t pos = 0; pos < list.size(); ++pos) {
                int const idx = list[pos];
                Obj const& o = world.objs[static_cast<size_t>(idx)];
                bool const isTeleporter = o.type == TeleportPortal || o.id == ObjId::TeleportTrigger;
                if (world.mayHaveDyn[static_cast<size_t>(idx)] ||
                    (!world.areaTarget.empty() && world.areaTarget[static_cast<size_t>(idx)])) {
                    World::MovEntry entry;
                    entry.idx = idx;
                    entry.uid = static_cast<unsigned>(o.uid);
                    entry.minY = 0.f;
                    entry.maxY = 0.f;
                    entry.dyn = true;
                    entry.teleporter = isTeleporter;
                    entry.ngroups = 0;
                    index.mov.push_back(entry);
                    continue;
                }
                Rect const box = g.obj(idx).getObjectRect();
                float minY = box.y;
                float maxY = box.y + box.h;
                if (o.radius > 0.f) {
                    float const radius = g.obj(idx).circleRadius();
                    float const centerY = g.obj(idx).getRealPosition().y;
                    float const reach = std::fabs(radius) + 1.f;
                    minY = std::min(minY, centerY - reach);
                    maxY = std::max(maxY, centerY + reach);
                }
                if (o.type == Slope) {
                    if (o.rectGen && o.sws == 1.f && o.shs == 1.f && box.w > 0.f && box.h > 0.f) {
                        minY = minY - box.h * 0.5f - 0.5f;
                        maxY = maxY + box.h * 0.5f + 0.5f;
                    } else {
                        Rect const doubledBox = g.obj(idx).getObjectRectWH(2.f, 2.f);
                        minY = std::min(minY - box.h * 0.5f, doubledBox.y);
                        maxY = std::max(maxY + box.h * 0.5f, doubledBox.y + doubledBox.h);
                    }
                }
                if (isTeleporter) index.teleporter = true;
                int firstBand = static_cast<int>(std::floor(minY / World::CellIndex::kBandH));
                int lastBand = static_cast<int>(std::floor(maxY / World::CellIndex::kBandH));
                if (firstBand < 0) firstBand = 0;
                if (lastBand < firstBand) lastBand = firstBand;
                if (lastBand > 4095) lastBand = 4095;
                if (static_cast<size_t>(lastBand) >= index.bands.size()) {
                    index.bands.resize(static_cast<size_t>(lastBand) + 1);
                }
                for (int band = firstBand; band <= lastBand; ++band) {
                    index.bands[static_cast<size_t>(band)].push_back(
                            World::BandEntry{idx, static_cast<unsigned>(o.uid), minY, maxY});
                }
            }
            index.computeSorted();
        }

        struct ListGroups {
            std::vector<short> groups;
        };
        thread_local std::unordered_map<uint64_t, ListGroups> tl_listGroups;
        thread_local WKey tl_lgWorld;

        // groups of a list (cached), also writes the list content hash
        ListGroups const& groupsOfList(Game const& g, std::vector<int> const& list, uint64_t& content) {
            World const& world = *g.world;
            if (tl_lgWorld != &world) {
                tl_lgWorld = &world;
                tl_listGroups.clear();
            }
            content = mixKey(0x51ed27ull, list.size());
            for (int idx : list) content = mixKey(content, static_cast<uint64_t>(static_cast<uint32_t>(idx)));
            auto it = tl_listGroups.find(content);
            if (it != tl_listGroups.end()) return it->second;
            if (tl_listGroups.size() >= 65536) tl_listGroups.clear();
            ListGroups result;
            for (int idx : list) {
                if (world.mayHaveDyn[static_cast<size_t>(idx)] ||
                    (!world.areaTarget.empty() && world.areaTarget[static_cast<size_t>(idx)]))
                    continue;
                Obj const& o = world.objs[static_cast<size_t>(idx)];
                for (short group : o.groups) {
                    if (group >= 0) result.groups.push_back(group);
                }
            }
            std::sort(result.groups.begin(), result.groups.end());
            result.groups.erase(std::unique(result.groups.begin(), result.groups.end()), result.groups.end());
            return tl_listGroups.emplace(content, std::move(result)).first->second;
        }

        // cache key of a cell: list content, cell, half step and offsets of its groups
        uint64_t cellDepKey(Game const& g, int cellKey, std::vector<int> const& list) {
            uint64_t content = 0;
            ListGroups const& listGroups = groupsOfList(g, list, content);
            uint64_t hash = mixKey(mixKey(content, static_cast<uint64_t>(static_cast<uint32_t>(cellKey))),
                                   g.isBetweenSteps ? 7u : 3u);
            auto const& groupOffsets = g.trig.r().M().groupOff;
            for (short group : listGroups.groups) {
                if (auto const* offset = groupOffsets.find(group)) {
                    uint64_t bitsX;
                    uint64_t bitsY;
                    std::memcpy(&bitsX, &offset->x, 8);
                    std::memcpy(&bitsY, &offset->y, 8);
                    hash = mixKey(mixKey(mixKey(hash, static_cast<uint64_t>(static_cast<uint16_t>(group))), bitsX),
                                  bitsY);
                }
            }
            return hash;
        }

        // vertical index of a cell at current positions, from the cache or built, nullptr if not possible
        World::CellIndex const* dynamicIndex(Game& g, int cellKey, std::vector<int> const& list) {
            if (g_noShare & 2) return nullptr;
            if (tl_cellCacheTick != g.tick || tl_cellCacheWorld != g.world) {
                tl_cellCacheTick = g.tick;
                tl_cellCacheWorld = g.world;
                tl_cellCacheUsed = 0;
            }
            struct Fast {
                uint64_t ver;
                void const* list;
                bool bs;
                int slot;
            };
            static thread_local std::vector<Fast> tl_fast;
            static thread_local int tl_fastTick = -1;
            if (tl_fastTick != g.tick || tl_cellCacheUsed == 0) {
                tl_fastTick = g.tick;
                tl_fast.clear();
            }
            uint64_t const version = g.trig.r().M().ver;
            for (auto const& fast : tl_fast) {
                if (fast.ver == version && fast.list == &list && fast.bs == g.isBetweenSteps) {
                    return &tl_cellCache[static_cast<size_t>(fast.slot)].ix;
                }
            }
            uint64_t const key = cellDepKey(g, cellKey, list);
            for (size_t i = 0; i < tl_cellCacheUsed; ++i) {
                if (tl_cellCache[i].key == key) {
                    if (tl_fast.size() < 256) {
                        tl_fast.push_back(Fast{version, &list, g.isBetweenSteps, static_cast<int>(i)});
                    }
                    return &tl_cellCache[i].ix;
                }
            }
            if (tl_cellCacheUsed >= 48) return nullptr;
            if (tl_cellCache.size() <= tl_cellCacheUsed) tl_cellCache.emplace_back();
            CellCache& slot = tl_cellCache[tl_cellCacheUsed++];
            slot.key = key;
            if (tl_fast.size() < 256) {
                tl_fast.push_back(Fast{version, &list, g.isBetweenSteps, static_cast<int>(tl_cellCacheUsed - 1)});
            }
            buildDynamicIndex(g, list, slot.ix);
            ++g_cellCacheBuilds;
            ++g_trigCounters[4];
            return &slot.ix;
        }
    }

    // number of indexes built by the cell cache
    long cellCacheBuilds() { return g_cellCacheBuilds; }

    // force applied by a force block to the player
    Point calculateForceToTarget(GObj object, Player const& player) {
        Obj const& o = *object.o;
        Rect const rect = object.getObjectRect();
        float radius = o.radius;
        if (radius <= 0.f)
            radius = std::max(rect.w, rect.h) * 0.5f;
        else
            radius = object.circleRadius();
        Point const objectPos = object.getRealPosition();
        float const dx = objectPos.x - player.x;
        float const dy = objectPos.y - player.y;
        float angle;
        bool const flipY = object.flipYEff();
        if (!o.forceRelative)
            angle = ((flipY ? 180.f : 0.f) + (90.f - object.nodeRotX())) * 0.017453292f;
        else
            angle = static_cast<float>(static_cast<double>(std::atan2(dy, dx)) + (flipY ? 0.0 : 3.141592653589793));
        float magnitude = o.force;
        if (o.forceRange) {
            float factor = 0.f;
            if (!o.forceRelative) {
                float const dirX = gcos(angle);
                float const dirY = gsin(angle);
                factor = ((dirX * dx + dirY * dy + radius) / radius) * 0.5f;
            } else {
                float const dist = std::sqrt(dx * dx + dy * dy);
                if (dist < radius) factor = 1.f - dist / radius;
            }
            factor = std::clamp(factor, 0.f, 1.f);
            magnitude = o.forceMin + (o.forceMax - o.forceMin) * factor;
        }
        return Point{gcos(angle) * magnitude, gsin(angle) * magnitude};
    }

    // builds the box corners from a center, a size and an angle in radians
    void OBB::calculateWithCenter(Point center, float width, float height, float rotationAngle) {
        float const sinA = gsin(rotationAngle);
        float const cosA = gcos(rotationAngle);
        Point axisX{cosA, sinA};
        Point axisY{-sinA, cosA};
        float const halfWidth = width * 0.5f;
        float const halfHeight = height * 0.5f;
        axisX = Point{axisX.x * halfWidth, axisX.y * halfWidth};
        axisY = Point{axisY.x * halfHeight, axisY.y * halfHeight};
        corner[0] = Point{center.x - axisX.x - axisY.x, center.y - axisX.y - axisY.y};
        corner[1] = Point{center.x + axisX.x - axisY.x, center.y + axisX.y - axisY.y};
        corner[2] = Point{center.x + axisX.x + axisY.x, center.y + axisX.y + axisY.y};
        corner[3] = Point{center.x - axisX.x + axisY.x, center.y - axisX.y + axisY.y};
        finish();
    }

    // computes the axes and origins from the corners
    void OBB::finish() {
        axis[0] = Point{corner[1].x - corner[0].x, corner[1].y - corner[0].y};
        axis[1] = Point{corner[3].x - corner[0].x, corner[3].y - corner[0].y};
        for (int a = 0; a < 2; ++a) {
            float const invLengthSq = 1.0f / (axis[a].x * axis[a].x + axis[a].y * axis[a].y);
            axis[a] = Point{axis[a].x * invLengthSq, axis[a].y * invLengthSq};
            origin[a] = static_cast<double>(corner[0].y * axis[a].y + corner[0].x * axis[a].x);
        }
    }

    // biggest gap between the two boxes on their axes, negative if they overlap
    float OBB::separation(OBB const& other) const {
        float best = -1e9f;
        auto axisOf = [](OBB const& box, int i) {
            Point edge = i == 0 ? Point{box.corner[1].x - box.corner[0].x, box.corner[1].y - box.corner[0].y}
                                : Point{box.corner[3].x - box.corner[0].x, box.corner[3].y - box.corner[0].y};
            float const len = std::sqrt(edge.x * edge.x + edge.y * edge.y);
            return len > 0.f ? Point{edge.x / len, edge.y / len} : Point{1.f, 0.f};
        };
        for (int k = 0; k < 4; ++k) {
            Point const dir = axisOf(k < 2 ? *this : other, k & 1);
            float aMin = 1e9f;
            float aMax = -1e9f;
            float bMin = 1e9f;
            float bMax = -1e9f;
            for (int i = 0; i < 4; ++i) {
                float const projA = corner[i].x * dir.x + corner[i].y * dir.y;
                float const projB = other.corner[i].x * dir.x + other.corner[i].y * dir.y;
                aMin = std::min(aMin, projA);
                aMax = std::max(aMax, projA);
                bMin = std::min(bMin, projB);
                bMax = std::max(bMax, projB);
            }
            float const sep = std::max(bMin - aMax, aMin - bMax);
            best = std::max(best, sep);
        }
        return best;
    }

    // true if other overlaps this box on this box's two axes
    bool OBB::overlaps1Way(OBB const& other) const {
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
        {
            __m128 const corners01 = _mm_loadu_ps(&other.corner[0].x);
            __m128 const corners23 = _mm_loadu_ps(&other.corner[2].x);
            __m128 const xs = _mm_shuffle_ps(corners01, corners23, _MM_SHUFFLE(2, 0, 2, 0));
            __m128 const ys = _mm_shuffle_ps(corners01, corners23, _MM_SHUFFLE(3, 1, 3, 1));
            __m128 const proj0 =
                    _mm_add_ps(_mm_mul_ps(_mm_set1_ps(axis[0].x), xs), _mm_mul_ps(_mm_set1_ps(axis[0].y), ys));
            __m128 const proj1 =
                    _mm_add_ps(_mm_mul_ps(_mm_set1_ps(axis[1].x), xs), _mm_mul_ps(_mm_set1_ps(axis[1].y), ys));
            if (!_mm_movemask_ps(_mm_or_ps(_mm_cmpunord_ps(proj0, proj0), _mm_cmpunord_ps(proj1, proj1)))) {
                __m128 lo = _mm_min_ps(_mm_unpacklo_ps(proj0, proj1), _mm_unpackhi_ps(proj0, proj1));
                __m128 hi = _mm_max_ps(_mm_unpacklo_ps(proj0, proj1), _mm_unpackhi_ps(proj0, proj1));
                lo = _mm_min_ps(lo, _mm_movehl_ps(lo, lo));
                hi = _mm_max_ps(hi, _mm_movehl_ps(hi, hi));
                float minProj[4];
                float maxProj[4];
                _mm_storeu_ps(minProj, lo);
                _mm_storeu_ps(maxProj, hi);
                for (int a = 0; a < 2; ++a) {
                    if (origin[a] + 1.0 < static_cast<double>(minProj[a]) ||
                        static_cast<double>(maxProj[a]) < origin[a])
                        return false;
                }
                return true;
            }
        }
#endif
        for (int a = 0; a < 2; ++a) {
            float const ax = axis[a].x;
            float const ay = axis[a].y;
            double projMin;
            double projMax;
            {
                double const proj0 = static_cast<double>(ax * other.corner[0].x + ay * other.corner[0].y);
                double const proj1 = static_cast<double>(ay * other.corner[1].y + ax * other.corner[1].x);
                projMin = proj0;
                projMax = proj1;
                if (proj0 <= proj1) {
                    projMin = proj0;
                    projMax = proj1;
                } else {
                    projMin = proj1;
                    projMax = proj0;
                }
            }
            {
                double const proj2 = static_cast<double>(ay * other.corner[2].y + ax * other.corner[2].x);
                if (proj2 < projMin) projMin = proj2;
                if (projMax < proj2) projMax = proj2;
            }
            {
                double const proj3 = static_cast<double>(ay * other.corner[3].y + ax * other.corner[3].x);
                if (proj3 < projMin) projMin = proj3;
                if (projMax < proj3) projMax = proj3;
            }
            if (origin[a] + 1.0 < projMin || projMax < origin[a]) return false;
        }
        return true;
    }

    // oriented box of an object (rotated, scaled, moved or sideways)
    OBB objectObb(GObj o) {
        if (o.g && o.g->rotOvN) {
            if (auto* rotOv = o.g->rotOvFind(o.index)) {
                if (o.o->obbGen) {
                    Point const rotatedPos = o.getRealPosition();
                    float scaleX = o.o->scaleX;
                    float scaleY = o.o->scaleY;
                    if (o.trig()) trigScaleAbs(*o.g, o.index, scaleX, scaleY);
                    OBB box;
                    box.calculateWithCenter(Point{rotatedPos.x + o.o->boxOffX, rotatedPos.y + o.o->boxOffY},
                                            o.o->sws * o.o->width * scaleX, o.o->shs * o.o->height * scaleY,
                                            -o.rotation() * 0.017453292f);
                    return box;
                }
                int const savedCount = o.g->rotOvN;
                o.g->rotOvN = 0;
                OBB box = objectObb(o);
                Point const basePos = o.getRealPosition();
                o.g->rotOvN = savedCount;
                (void)rotOv;
                Point const rotatedPos = o.getRealPosition();
                float const offX = o.o->boxOffX;
                float const offY = o.o->boxOffY;
                for (auto& corner : box.corner) {
                    float const vx = corner.x - (basePos.x + offX);
                    float const vy = corner.y - (basePos.y + offY);
                    corner = Point{rotatedPos.x + offX - vy, rotatedPos.y + offY + vx};
                }
                box.finish();
                return box;
            }
        }
        OBB box;
        float dx = 0.f;
        float dy = 0.f;
        if (o.trig()) {
            Point const pos = o.getRealPosition();
            float scaleX;
            float scaleY;
            bool const scaled = trigScaleAbs(*o.g, o.index, scaleX, scaleY);
            if (trigRotated(*o.g, o.index) || scaled || !o.o->hasObb) {
                float const rot = o.o->rotX + trigRotation(*o.g, o.index);
                box.calculateWithCenter(Point{pos.x + o.o->boxOffX, pos.y + o.o->boxOffY},
                                        o.o->sws * o.o->width * scaleX, o.o->shs * o.o->height * scaleY,
                                        -rot * 0.017453292f);
                return box;
            }
            if (o.o->obbGen) {
                box.fromHalfAxes(Point{pos.x + o.o->boxOffX, pos.y + o.o->boxOffY}, o.o->obbHx, o.o->obbHy, o.o->obbVx,
                                 o.o->obbVy);
                return box;
            }
            dx = pos.x - static_cast<float>(o.o->posX);
            dy = pos.y - static_cast<float>(o.o->posY);
            for (int i = 0; i < 4; ++i) box.corner[i] = Point{o.o->obb[i * 2] + dx, o.o->obb[i * 2 + 1] + dy};
            box.finish();
            return box;
        }
        if (auto* key = o.cur()) {
            if (o.o->obbGen && key->rot == o.o->rot &&
                (key->x != static_cast<float>(o.o->posX) || key->y != static_cast<float>(o.o->posY))) {
                box.fromHalfAxes(Point{key->x + o.o->boxOffX, key->y + o.o->boxOffY}, o.o->obbHx, o.o->obbHy,
                                 o.o->obbVx, o.o->obbVy);
                return box;
            }
            dx = key->x - static_cast<float>(o.o->posX);
            dy = key->y - static_cast<float>(o.o->posY);
            if (key->rot != o.o->rot) {
                float const width = std::hypot(o.o->obb[2] - o.o->obb[0], o.o->obb[3] - o.o->obb[1]);
                float const height = std::hypot(o.o->obb[6] - o.o->obb[0], o.o->obb[7] - o.o->obb[1]);
                box.calculateWithCenter(Point{key->x + o.o->boxOffX, key->y + o.o->boxOffY}, width, height,
                                        -key->rot * 0.017453292f);
                return box;
            }
        }
        for (int i = 0; i < 4; ++i) box.corner[i] = Point{o.o->obb[i * 2] + dx, o.o->obb[i * 2 + 1] + dy};
        box.finish();
        return box;
    }

    // oriented box of the player, uses the cached size like the game
    static OBB playerObb(Player const& p) {
        OBB box;
        if (p.obbDirty) {
            p.obbScaleW = p.spriteWidthScale;
            p.obbScaleH = p.spriteHeightScale;
            p.obbDirty = false;
        }
        box.calculateWithCenter(Point{p.x, p.y}, p.obbScaleW * p.width * p.scaleX, p.obbScaleH * p.height * p.scaleY,
                                -p.rotation * 0.017453292f);
        return box;
    }

    // true if the hit result would change within the rotation uncertainty
    static bool obbRotAmbiguous(Player const& p, OBB const& objectBox, bool hit) {
        if (!p.rotUncertain || p.rotUnc <= 0.f) return false;
        float const width = p.obbScaleW * p.width * p.scaleX;
        float const height = p.obbScaleH * p.height * p.scaleY;
        for (float side : {-1.f, 1.f}) {
            OBB box;
            box.calculateWithCenter(Point{p.x, p.y}, width, height, -(p.rotation + side * p.rotUnc) * 0.017453292f);
            if ((objectBox.overlaps1Way(box) && box.overlaps1Way(objectBox)) != hit) return true;
        }
        return false;
    }

    // true if the object points down (gravity portals)
    bool GObj::isFacingDown() const {
        int const rot = static_cast<int>(rotation());
        bool const flipY = flipYEff();
        if (rot != (rot / 90) * 90) {
            bool facing = (static_cast<unsigned>(rot - 0x5b) < 0xb3u) || (static_cast<unsigned>(rot + 0x10d) < 0xb3u);
            return flipY ? !facing : facing;
        }
        int const absRot = rot < 0 ? -rot : rot;
        return flipY ? (absRot != 180) : (absRot == 180);
    }

    // true if the object points left (gravity portals when sideways)
    bool GObj::isFacingLeft() const {
        int const rot = static_cast<int>(rotation());
        bool const flipY = flipYEff();
        if (rot != (rot / 90) * 90) {
            bool facing = (static_cast<unsigned>(rot - 0x5b) < 0xb3u) || (static_cast<unsigned>(rot + 0x10d) < 0xb3u);
            return flipY ? !facing : facing;
        }
        int const positiveRot = rot < 0 ? rot + 360 : rot;
        return (positiveRot == 270 && !flipY) || (positiveRot == 90 && flipY);
    }

    // slope direction, recomputed for an object turned by rotateGameplayObject
    int GObj::slopeDir() const {
        if (!(g && g->rotOvN && g->rotOvFind(index))) return o->slopeDir;
        int angle = static_cast<int>(rotation()) % 360;
        if (angle < 0) angle += 360;
        if (angle % 90 != 0) return o->slopeDir;
        static int const kTab[2][2][4] = {{{0, 4, 3, 5}, {1, 7, 2, 6}}, {{2, 6, 1, 7}, {3, 5, 0, 4}}};
        return kTab[o->flipX ? 1 : 0][o->flipY ? 1 : 0][angle / 90];
    }

    // true if the slope goes up, recomputed for a turned object
    bool GObj::slopeUphill() const {
        if (!(g && g->rotOvN && g->rotOvFind(index))) return o->slopeUphill;
        {
            int angle = static_cast<int>(rotation()) % 360;
            if (angle < 0) angle += 360;
            if (angle % 90 != 0) return false;
        }
        int const dir = slopeDir();
        return dir == 0 || dir == 3 || dir == 6 || dir == 7;
    }

    // player box vs round object, or player circle with the circle option
    bool playerCircleCollision(Game& g, Player& p, GObj object) {
        float const radius = object.circleRadius();
        Point const center = object.getRealPosition();
        if (!g.circleOption) {
            Rect const box = playerRect(p);
            float const maxX = getMaxX(box);
            float const minX = getMinX(box);
            float const maxY = getMaxY(box);
            float const minY = getMinY(box);
            if (center.x >= minX && center.x <= maxX && center.y >= minY && center.y <= maxY) return true;
            auto dist = [&](float x, float y) {
                return std::sqrt((x - center.x) * (x - center.x) + (y - center.y) * (y - center.y));
            };
            if (std::fabs(dist(maxX, maxY)) < radius) return true;
            if (std::fabs(dist(maxX, minY)) < radius) return true;
            if (std::fabs(dist(minX, minY)) < radius) return true;
            if (std::fabs(dist(minX, maxY)) < radius) return true;
            return false;
        }
        float const distance = std::sqrt((p.x - center.x) * (p.x - center.x) + (p.y - center.y) * (p.y - center.y));
        Rect const box = playerRect(p);
        return distance <= box.w * 0.5f + radius;
    }

    // floor of the ship / ufo / wave corridor
    float getMinPortalY(Game const& g) {
        if (g.camKey && g.camKey->staticMode) return g.camKey->minPortalY;
        if (!g.camKey && cameraStaticBounds(g)) return cameraStaticMinY(g);
        float const minY = (g.portalCenterY - (g.lastModeHeight * 0.5f) / g.cameraZoom) + g.cameraOffsetY;
        return 90.f <= minY ? minY : 90.f;
    }

    // ceiling of the corridor
    float getMaxPortalY(Game const& g) {
        if (g.camKey && g.camKey->staticMode) return g.camKey->maxPortalY;
        if (!g.camKey && cameraStaticBounds(g)) return cameraStaticMaxY(g);
        return getMinPortalY(g) + g.lastModeHeight / g.cameraZoom;
    }

    // true while a zoom trigger is changing the camera zoom
    static bool cameraZoomChanging(Game const& g) { return g.cameraZoom != g.targetCameraZoom; }

    void collisionCheckObjects(Game& g, Player& player, std::vector<int> const& objects, float dt);
    static int collisionCheckObjectsWin(Game& g, Player& player, std::vector<int> const& objects, float dt, float winLo,
                                        float winHi, size_t from = 0);
    static thread_local std::vector<std::pair<unsigned, int>> tl_cand;
    thread_local long g_filtStatScanned = 0;
    thread_local long g_filtStatMov = 0;
    thread_local long g_filtStatCalls = 0;
    thread_local long g_filtStatKept = 0;
    thread_local long g_filtStatMovDeco = 0;

    // moving decoration entries seen by the filter
    long filterStatMovDeco() { return g_filtStatMovDeco; }

    void filterStats(long& scanned, long& mov, long& calls, long& kept) {
        scanned = g_filtStatScanned;
        mov = g_filtStatMov;
        calls = g_filtStatCalls;
        kept = g_filtStatKept;
    }

    // collisionCheckObjects on the objects close to the player on y only
    static void collisionCheckObjectsFiltered(Game& g, Player& player, std::vector<int> const& objects,
                                              World::CellIndex const& index, float dt) {
        Rect const playerBox = playerRect(player);
        float const lo = getMinY(playerBox) - kFilterMargin;
        float const hi = getMaxY(playerBox) + kFilterMargin;
        tl_winLo = std::max(tl_winLo, lo);
        tl_winHi = std::min(tl_winHi, hi);
        tl_cand.clear();
        int firstBand = static_cast<int>(std::floor(lo / World::CellIndex::kBandH));
        int lastBand = static_cast<int>(std::floor(hi / World::CellIndex::kBandH));
        if (firstBand < 0) firstBand = 0;
        int const bandCount = static_cast<int>(index.bands.size());
        if (lastBand >= bandCount) lastBand = bandCount - 1;
        size_t runEnd[16];
        int nRuns = 0;
        for (int band = firstBand; band <= lastBand; ++band) {
            g_filtStatScanned += index.bands[static_cast<size_t>(band)].size();
            if (band == firstBand) {
                for (auto const& entry : index.bands[static_cast<size_t>(band)]) {
                    if (entry.maxY < lo || entry.minY > hi) continue;
                    tl_cand.emplace_back(entry.uid, entry.idx);
                }
            } else {
                float const bandF = static_cast<float>(band);
                for (auto const& entry : index.bands[static_cast<size_t>(band)]) {
                    if (entry.maxY < lo || entry.minY > hi) continue;
                    if (!(entry.minY / World::CellIndex::kBandH >= bandF)) continue;
                    tl_cand.emplace_back(entry.uid, entry.idx);
                }
            }
            if (nRuns < 16) runEnd[nRuns++] = tl_cand.size();
        }
        g_filtStatMov += index.mov.size();
        ++g_filtStatCalls;
        {
            if (debugOptions().stats) {
                for (auto const& entry : index.mov) {
                    if (g.world->decoLikeAt(entry.idx)) ++g_filtStatMovDeco;
                }
            }
        }
        if (!index.mov.empty()) {
            bool const trigOn = g.trig.on;
            uint64_t const* movedBits = nullptr;
            size_t movedWords = 0;
            if (trigOn) {
                auto const& movedGroups = g.trig.r().M().movedGroups;
                movedBits = movedGroups.data();
                movedWords = movedGroups.size();
            }
            World const& world = *g.world;
            for (auto const& entry : index.mov) {
                bool keep = entry.dyn || (entry.maxY >= lo && entry.minY <= hi);
                if (!keep) {
                    if (!trigOn)
                        keep = true;
                    else {
                        for (uint8_t k = 0; k < entry.ngroups; ++k) {
                            int const group = entry.groups[k];
                            if (group < 0) continue;
                            size_t const word = static_cast<size_t>(group) >> 6;
                            if (word < movedWords && ((movedBits[word] >> (group & 63)) & 1u)) {
                                keep = true;
                                break;
                            }
                        }
                    }
                    if (!keep && trigOn) {
                        size_t const i = static_cast<size_t>(entry.idx);
                        if ((!world.areaTarget.empty() && world.areaTarget[i] && trigAreaOff(g, entry.idx)) ||
                            (world.mayHaveDyn[i] && trigHasDyn(g, entry.idx)))
                            keep = true;
                    }
                }
                if (keep) tl_cand.emplace_back(entry.uid, entry.idx);
            }
            if (nRuns < 16) runEnd[nRuns++] = tl_cand.size();
        }
        if (tl_cand.empty()) return;
        g_filtStatKept += static_cast<long>(tl_cand.size());
        if (index.sortedRuns && nRuns < 16) {
            static thread_local std::vector<std::pair<unsigned, int>> tl_mrg;
            size_t runStart = 0;
            int run = 0;
            while (run < nRuns && runEnd[run] == 0) ++run;
            if (run < nRuns) runStart = runEnd[run++];
            for (; run < nRuns; ++run) {
                size_t const runStop = runEnd[run];
                if (runStop == runStart) continue;
                if (!(tl_cand[runStart] < tl_cand[runStart - 1])) {
                    runStart = runStop;
                    continue;
                }
                tl_mrg.resize(runStop);
                std::merge(tl_cand.begin(), tl_cand.begin() + static_cast<long>(runStart),
                           tl_cand.begin() + static_cast<long>(runStart), tl_cand.begin() + static_cast<long>(runStop),
                           tl_mrg.begin());
                std::copy(tl_mrg.begin(), tl_mrg.begin() + static_cast<long>(runStop), tl_cand.begin());
                runStart = runStop;
            }
        } else
            std::sort(tl_cand.begin(), tl_cand.end());
        tl_filtered.clear();
        for (auto const& cand : tl_cand) tl_filtered.push_back(cand.second);
        size_t const solidCount = tl_solids.size();
        size_t const hazardCount = tl_hazards.size();
        int const stopIndex = collisionCheckObjectsWin(g, player, tl_filtered, dt, lo, hi);
        if (stopIndex >= 0) {
            unsigned const lastKey = tl_cand[static_cast<size_t>(stopIndex)].first;
            tl_solids.resize(solidCount);
            tl_hazards.resize(hazardCount);
            size_t from = 0;
            for (; from < objects.size(); ++from) {
                int const idx = objects[from];
                unsigned const key = index.byPos ? static_cast<unsigned>(from)
                                                 : static_cast<unsigned>(g.world->objs[static_cast<size_t>(idx)].uid);
                if (key > lastKey) break;
                GObj object = g.obj(idx);
                int const type = object.getType();
                if (type == Decoration || object.disabled()) continue;
                if (type == Breakable && object.st().destroyed) continue;
                if (type == Solid || type == Breakable)
                    tl_solids.push_back(idx);
                else if (type == Hazard || type == AnimatedHazard)
                    tl_hazards.push_back(idx);
            }
            collisionCheckObjectsWin(g, player, objects, dt, -1e30f, 1e30f, from);
        } else {
            Rect const newBox = playerRect(player);
            if (getMinY(newBox) < lo || getMaxY(newBox) > hi) {
                tl_needFull = true;
                if (g_trace)
                    std::printf("      [trace] player left the filter window (%.2f..%.2f): full lists\n", lo, hi);
            }
        }
    }

    // one pass on a list of objects with no y window
    void collisionCheckObjects(Game& g, Player& player, std::vector<int> const& objects, float dt) {
        collisionCheckObjectsWin(g, player, objects, dt, -1e30f, 1e30f);
    }

    // applies portals, orbs, pads and items, keeps solids and hazards for later
    static int collisionCheckObjectsWin(Game& g, Player& player, std::vector<int> const& objects, float dt, float winLo,
                                        float winHi, size_t from) {
        Rect playerBox = playerRect(player);
        float playerMaxX = getMaxX(playerBox);
        float playerMinX = getMinX(playerBox);
        float playerMaxY = getMaxY(playerBox);
        float playerMinY = getMinY(playerBox);
        for (size_t listPos = from; listPos < objects.size(); ++listPos) {
            int const idx = objects[listPos];
            GObj object = g.obj(idx);
            int const type = object.getType();
            if (type == Decoration || object.disabled()) continue;
            if ((type == Breakable || type == SecretCoin || type == Collectible || type == UserCoin) &&
                object.st().destroyed)
                continue;
            if (type == Solid || type == Breakable) {
                tl_solids.push_back(idx);
                continue;
            }
            if (type == Hazard || type == AnimatedHazard) {
                tl_hazards.push_back(idx);
                continue;
            }
            if (type == CollisionObject) continue;
            if (hasBeenActivatedByPlayer(player, object)) continue;
            Rect objRect = (type == Slope) ? object.getObjectRectWH(2.f, 2.f) : object.getObjectRect();
            bool touching;
            if (object.o->radius <= 0.f) {
                touching = getMinX(objRect) <= playerMaxX && playerMinX <= getMaxX(objRect) &&
                           getMinY(objRect) <= playerMaxY && playerMinY <= getMaxY(objRect);
            } else {
                touching = playerCircleCollision(g, player, object);
            }
            if (!touching) {
                if (g_trace && std::fabs(objRect.x + objRect.w * 0.5f - player.x) < 120.f &&
                    std::fabs(objRect.y + objRect.h * 0.5f - player.y) < 120.f)
                    std::printf(
                            "      [trace] uid %d id %d type %d : rect [%.2f %.2f %.2f %.2f] not touched (player [%.2f "
                            "%.2f %.2f %.2f])\n",
                            object.uid(), object.o->id, type, objRect.x, objRect.y, objRect.w, objRect.h, playerBox.x,
                            playerBox.y, playerBox.w, playerBox.h);
                continue;
            }
            bool obbHit = true;
            if (object.hasOrientedBox() && (!g.circleOption || object.o->radius <= 0.f)) {
                OBB const objectOBB = objectObb(object);
                OBB const playerOBB = playerObb(player);
                obbHit = objectOBB.overlaps1Way(playerOBB) && playerOBB.overlaps1Way(objectOBB);
                g.obbGap = std::min(g.obbGap, std::fabs(objectOBB.separation(playerOBB)));
                if (!g.rotAmb && obbRotAmbiguous(player, objectOBB, obbHit)) g.rotAmb = true;
                if (g_trace)
                    std::printf(
                            "      [trace] uid %d id %d type %d : rect touched, OBB %s (player rot %.2f size "
                            "%.1fx%.1f, corners (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f))\n",
                            object.uid(), object.o->id, type, obbHit ? "yes" : "NO", player.rotation,
                            player.spriteWidthScale * player.width * player.scaleX,
                            player.spriteHeightScale * player.height * player.scaleY, playerOBB.corner[0].x,
                            playerOBB.corner[0].y, playerOBB.corner[1].x, playerOBB.corner[1].y, playerOBB.corner[2].x,
                            playerOBB.corner[2].y, playerOBB.corner[3].x, playerOBB.corner[3].y);
            } else if (g_trace)
                std::printf("      [trace] uid %d id %d type %d : rect touched\n", object.uid(), object.o->id, type);
            if (!obbHit) continue;
            if (g.currentChannel != 0 && object.o->cls == 1 && object.o->channel != 0 &&
                object.o->channel != g.currentChannel)
                continue;

            auto setPortal = [&]() {
                Point const pos = object.getPosition();
                player.lastPortalX = pos.x;
                player.lastPortalY = pos.y;
                player.lastActivatedPortal = object.uid();
            };
            auto claimPortal1 = [&]() {
                if (g.isDualMode && g.lastActivatedPortal2)
                    g.lastActivatedPortal1 = g.lastActivatedPortal2;
                else
                    g.lastActivatedPortal1 = object.uid();
            };
            auto portal1NoEffects = [&]() {
                GObj const portal1 = g.lastActivatedPortal1 ? g.objByUid(g.lastActivatedPortal1) : GObj{};
                return portal1 ? portal1.o->hasNoEffects : object.o->hasNoEffects;
            };
            bool refreshRect = false;
            switch (type) {
            case InverseGravityPortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    setPortal();
                    layerFlipGravity(g, player, true, object.o->hasNoEffects || object.o->hide);
                    activatedByPlayer(player, object);
                    trigGameEvent(g, GameEvent::PortalGravityFlip);
                }
                break;
            case NormalGravityPortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    setPortal();
                    layerFlipGravity(g, player, false, object.o->hasNoEffects || object.o->hide);
                    activatedByPlayer(player, object);
                    trigGameEvent(g, GameEvent::PortalGravityNormal);
                }
                break;
            case ShipPortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    playerWillSwitchMode(g, player, object);
                    switchedToMode(player, 5);
                    setPortal();
                    claimPortal1();
                    if (toggleFlyMode(player, true, portal1NoEffects())) updateDashArt(g, player);
                    activatedByPlayer(player, object);
                }
                break;
            case CubePortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    playerWillSwitchMode(g, player, object);
                    setPortal();
                    toggleFlyMode(player, false, false);
                    toggleBirdMode(player, false, false);
                    toggleRollMode(player, false, false);
                    toggleDartMode(player, false, false);
                    toggleRobotMode(player, false, false);
                    toggleSpiderMode(player, false, false);
                    toggleSwingMode(player, false, false);
                    updateDashArt(g, player);
                    activatedByPlayer(player, object);
                }
                break;
            case YellowJumpPad:
            case PinkJumpPad:
            case RedJumpPad:
            case SpiderPad:
                layerBumpPlayer(g, player, object);
                break;
            case GravityPad: {
                bool const newUpsideDown = !(player.isSideways ? object.isFacingLeft() : object.isFacingDown());
                if (player.isUpsideDown != newUpsideDown && canBeActivatedByPlayer(g, player, object)) {
                    if (object.o->reverse) reversePlayer(g, player, object);
                    Point const pos = object.getPosition();
                    player.lastPortalX = pos.x;
                    player.lastPortalY = pos.y - 10.f;
                    activatedByPlayer(player, object);
                    player.lastActivatedPortal = object.uid();
                    propellPlayer(player, 0.8f, object.o->hasNoEffects, 10);
                    layerFlipGravity(g, player, newUpsideDown, true);
                    player.padRingRelated = true;
                    trigGameEvent(g, GameEvent::GravityPad);
                }
                break;
            }
            case YellowJumpRing:
            case PinkJumpRing:
            case GravityRing:
            case GreenRing:
            case DropRing:
            case RedJumpRing:
            case CustomRing:
            case DashRing:
            case GravityDashRing:
            case SpiderOrb:
                playerTouchedRing(g, player, object);
                break;
            case InverseMirrorPortal:
            case NormalMirrorPortal:
                if (!g.isEditor && canBeActivatedByPlayer(g, player, object)) {
                    setPortal();
                    toggleFlipped(g, type == InverseMirrorPortal, object.o->hasNoEffects);
                    triggerActivated(object);
                    trigGameEvent(g, type == InverseMirrorPortal ? GameEvent::PortalFlip : GameEvent::PortalUnFlip);
                }
                break;
            case BallPortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    playerWillSwitchMode(g, player, object);
                    toggleFlyMode(player, false, false);
                    toggleBirdMode(player, false, false);
                    toggleDartMode(player, false, false);
                    toggleRobotMode(player, false, false);
                    toggleSpiderMode(player, false, false);
                    toggleSwingMode(player, false, false);
                    setPortal();
                    claimPortal1();
                    if (toggleRollMode(player, true, portal1NoEffects())) updateDashArt(g, player);
                    activatedByPlayer(player, object);
                }
                break;
            case RegularSizePortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    setPortal();
                    togglePlayerScale(player, false, object.o->hasNoEffects);
                    activatedByPlayer(player, object);
                    trigGameEvent(g, GameEvent::PortalNormalScale);
                }
                break;
            case MiniSizePortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    setPortal();
                    togglePlayerScale(player, true, object.o->hasNoEffects);
                    activatedByPlayer(player, object);
                    trigGameEvent(g, GameEvent::PortalMiniScale);
                }
                break;
            case UfoPortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    playerWillSwitchMode(g, player, object);
                    switchedToMode(player, 0x13);
                    setPortal();
                    claimPortal1();
                    if (toggleBirdMode(player, true, portal1NoEffects())) updateDashArt(g, player);
                    activatedByPlayer(player, object);
                }
                break;
            case Modifier:
            case EnterEffectObject:
                playerTouchedTrigger(g, player, object);
                break;
            case SecretCoin:
            case Collectible:
            case UserCoin: {
                if ((type == SecretCoin || type == UserCoin) && g.isPracticeMode) break;
                bool fire = true;
                if (type != SecretCoin) {
                    auto const key = std::make_pair(object.uid(), 0);
                    fire = !g.triggeredIDs.count(key);
                    if (fire) g.triggeredIDs.insert(key);
                }
                if (fire && g.trig.on) {
                    if (type == Collectible)
                        trigCollected(g, player, idx);
                    else
                        trigCoin(g, idx);
                }
                object.mut().destroyed = true;
                if (fire) trigGameEvent(g, type == Collectible ? GameEvent::PickupItem : GameEvent::UserCoin);
                break;
            }
            case DualPortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    setPortal();
                    toggleDualMode(g, object, true, player, false);
                    triggerActivated(object);
                    trigGameEvent(g, GameEvent::PortalDualOn);
                }
                break;
            case SoloPortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    setPortal();
                    toggleDualMode(g, object, false, player, false);
                    triggerActivated(object);
                    trigGameEvent(g, GameEvent::PortalDualOff);
                }
                break;
            case Slope:
                if (!player.isSideways)
                    collidedWithSlopeInternal(g, player, dt, object, false);
                else
                    handleRotatedCollisionInternal(g, player, dt, object, zeroRect(), false, false, true);
                refreshRect = true;
                break;
            case WavePortal:
                if (!g.isPlatformer && canBeActivatedByPlayer(g, player, object)) {
                    playerWillSwitchMode(g, player, object);
                    switchedToMode(player, 0x1a);
                    setPortal();
                    claimPortal1();
                    if (toggleDartMode(player, true, portal1NoEffects())) updateDashArt(g, player);
                    activatedByPlayer(player, object);
                }
                break;
            case RobotPortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    playerWillSwitchMode(g, player, object);
                    toggleFlyMode(player, false, false);
                    toggleBirdMode(player, false, false);
                    toggleRollMode(player, false, false);
                    toggleDartMode(player, false, false);
                    toggleSpiderMode(player, false, false);
                    toggleSwingMode(player, false, false);
                    setPortal();
                    claimPortal1();
                    if (toggleRobotMode(player, true, portal1NoEffects())) updateDashArt(g, player);
                    activatedByPlayer(player, object);
                }
                break;
            case TeleportPortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    trigGameEvent(g, GameEvent::PortalTeleport);
                    teleportPlayer(g, object, player);
                    refreshRect = true;
                }
                break;
            case SpiderPortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    playerWillSwitchMode(g, player, object);
                    toggleFlyMode(player, false, false);
                    toggleBirdMode(player, false, false);
                    toggleRollMode(player, false, false);
                    toggleDartMode(player, false, false);
                    toggleRobotMode(player, false, false);
                    toggleSwingMode(player, false, false);
                    setPortal();
                    claimPortal1();
                    if (toggleSpiderMode(player, true, portal1NoEffects())) updateDashArt(g, player);
                    activatedByPlayer(player, object);
                }
                break;
            case Special: {
                int const id = object.o->id;
                if (id < 0x744) {
                    if (id == 0x743)
                        player.stateHitHead = 2;
                    else if (id == 0x6db)
                        player.stateDartSlide = 2;
                    else if (id == 0x715)
                        player.stateNoAutoJump = 2;
                    else if (id == 0x725 && player.isDashing) {
                        stopDashing(g, player);
                        player.jumpBuffered = false;
                    }
                } else if (id == 0x815 || id == 0xe3d) {
                    player.stateForce = 2;
                    bool skip = false;
                    int const key = object.o->forceKey;
                    if (key > 0) {
                        for (int i = 0; i < player.nForceKeys; ++i) {
                            if (player.forceKeys[i] == key) {
                                skip = true;
                                break;
                            }
                        }
                        if (!skip && player.nForceKeys < 4) player.forceKeys[player.nForceKeys++] = key;
                    }
                    if (!skip) {
                        Point const force = calculateForceToTarget(object, player);
                        player.stateForceX += force.x;
                        player.stateForceY += force.y;
                        float const lengthSq =
                                player.stateForceX * player.stateForceX + player.stateForceY * player.stateForceY;
                        float const len = std::sqrt(lengthSq);
                        if (len > 9999.f) {
                            float const scale = 9999.f / len;
                            player.stateForceX *= scale;
                            player.stateForceY *= scale;
                        }
                    }
                } else if (id == 0xb32) {
                    player.stateFlipGravity = 2;
                }
                break;
            }
            case SwingPortal:
                if (!g.isPlatformer && canBeActivatedByPlayer(g, player, object)) {
                    playerWillSwitchMode(g, player, object);
                    switchedToMode(player, 0x29);
                    setPortal();
                    claimPortal1();
                    if (toggleSwingMode(player, true, portal1NoEffects())) updateDashArt(g, player);
                    activatedByPlayer(player, object);
                }
                break;
            case GravityTogglePortal:
                if (canBeActivatedByPlayer(g, player, object)) {
                    setPortal();
                    layerFlipGravity(g, player, !player.isUpsideDown, object.o->hasNoEffects || object.o->hide);
                    activatedByPlayer(player, object);
                    trigGameEvent(g, GameEvent::PortalGravityInvert);
                }
                break;
            case TeleportOrb:
                playerTouchedRing(g, player, object);
                refreshRect = true;
                break;
            default:
                break;
            }
            if (refreshRect) {
                playerBox = playerRect(player);
                playerMaxX = getMaxX(playerBox);
                playerMinX = getMinX(playerBox);
                playerMaxY = getMaxY(playerBox);
                playerMinY = getMinY(playerBox);
                if (playerMinY < winLo || playerMaxY > winHi) return static_cast<int>(listPos);
            }
        }
        return -1;
    }

    // text for the trace of a round object
    static std::string fmt_circle(Game& g, GObj o) {
        Point const center = o.getRealPosition();
        char buf[160];
        std::snprintf(buf, sizeof buf, " circle center (%.2f, %.2f) radius %.2f option %d", center.x, center.y,
                      o.circleRadius(), g.circleOption ? 1 : 0);
        return buf;
    }

    namespace {
        struct RectCacheScope {
            bool prev;
            bool prevPos;

            // turns on the rect and position caches for this scope
            RectCacheScope() : prev(tl_rectCacheOn) {
                tl_rectCacheOn = !(g_noShare & 2);
                trigPosCacheScopeBegin(prevPos);
            }

            // puts the caches back as they were
            ~RectCacheScope() {
                tl_rectCacheOn = prev;
                trigPosCacheScopeEnd(prevPos);
            }
        };
    }

    static int checkCollisionsImpl(Game& g, Player& player, float dt, bool ignoreDamage);

    // all collisions of the tick, returns 1 if the player died
    int checkCollisions(Game& g, Player& player, float dt, bool ignoreDamage) {
        RectCacheScope scope;
        return checkCollisionsImpl(g, player, dt, ignoreDamage);
    }

    // same as GJBaseGameLayer::checkCollisions
    static int checkCollisionsImpl(Game& g, Player& player, float dt, bool ignoreDamage) {
        bool const wasTeleported = player.wasTeleported;
        player.wasTeleported = false;
        player.ringJumpRelated = false;
        player.collidedTopMinY = 0;
        player.collidedBottomMaxY = 0;
        player.collidedLeftMaxX = 0;
        player.collidedRightMinX = 0;
        player.wasOnSlope = player.isOnSlope;
        player.isOnSlope = false;
        player.isOnGround4 = player.isOnGround2;
        if (!player.unk669)
            player.unk669 = true;
        else
            player.currentPotentialSlope = 0;
        player.potentialSlopes.clear();
        if (player.currentPotentialSlope) player.addPotentialSlope(player.currentPotentialSlope);
        if (player.currentSlope) player.addPotentialSlope(player.currentSlope);

        float sizeOffset = 0.f;
        float const hitboxSize = player.hitboxSize;
        if (player.vehicleSize != 1.f) sizeOffset = (1.f - player.vehicleSize) * hitboxSize * 0.5f;
        float const halfHitbox = hitboxSize * 0.5f;
        float limitY = (halfHitbox + 90.f) - sizeOffset;
        bool const corridorMode = !g.isFreeMode && (player.isShip || player.isBird || player.isDart || player.isSwing ||
                                                    player.isBall || player.isSpider || g.isDualMode);
        bool const wasOutOfBounds = player.isOutOfBounds;
        player.isOutOfBounds = false;
        bool outOfBounds = false;
        if (!g.isPlatformer) {
            if (player.isGoingLeft && player.x < -30.f) outOfBounds = true;
        }
        if (limitY <= player.y || corridorMode) {
            if (sizeOffset + g.maxGameplayY < player.y) outOfBounds = true;
        } else if (!player.isUpsideDown) {
            if (!player.maybeIsBoosted) {
                playerMoved(player);
                player.y = limitY;
                hitGround(g, player, GObj{}, false);
                updateCollide(player, 1, GObj{});
            }
        } else {
            if (player.lastFlipTime != 0.0 && player.totalTime - player.lastFlipTime < 0.10000000149011612) {
                playerMoved(player);
                player.y = limitY;
                hitGround(g, player, GObj{}, true);
                updateCollide(player, 0, GObj{});
                player.isOnGround2 = false;
                return 0;
            }
            outOfBounds = true;
        }
        if (corridorMode && !wasTeleported) {
            limitY = getMaxPortalY(g);
            float const minPortalY = getMinPortalY(g);
            float const maxPortalYDelta = limitY - g.lastMaxPortalY;
            bool const shipHolding = player.isShip && player.jumpBuffered;
            limitY = (limitY - halfHitbox) + sizeOffset;
            sizeOffset = (halfHitbox + minPortalY) - sizeOffset;
            bool const flyLike = player.isShip || player.isBird || player.isDart || player.isSwing || player.isBall ||
                                 player.isSpider;
            if (g_trace)
                std::printf(
                        "      [trace] corridor : y=%.3f floor=%.3f ceiling=%.3f (min %.2f max %.2f lastMax %.2f) "
                        "ud=%d yv=%.4f\n",
                        player.y, sizeOffset, limitY, minPortalY, getMaxPortalY(g), g.lastMaxPortalY,
                        player.isUpsideDown, player.yVelocity);
            if (player.y <= limitY) {
                if (player.y < sizeOffset) {
                    if (!player.wasOnSlope || (sizeOffset - hitboxSize * 0.5f) <= player.y) {
                        bool keepVelocity;
                        bool stopVelocity;
                        float savedYVelocity;
                        bool die = false;
                        if (!flyLike && player.stateHitHead < 1 && player.isUpsideDown) {
                            if (player.lastFlipTime == 0.0 ||
                                0.20000000298023224 <= player.totalTime - player.lastFlipTime) {
                                if (player.gameModeChangedTime == 0.0)
                                    die = true;
                                else if (0.20000000298023224 <= player.totalTime - player.gameModeChangedTime) {
                                    die = true;
                                }
                            }
                            savedYVelocity = static_cast<float>(player.yVelocity);
                            stopVelocity = true;
                            keepVelocity = false;
                        } else {
                            savedYVelocity = static_cast<float>(player.yVelocity);
                            stopVelocity = false;
                            keepVelocity = !(player.isUpsideDown ||
                                             ((player.isShip || player.isDart) && player.stateRingJump) ||
                                             maxPortalYDelta == 0.f);
                        }
                        if (die) {
                            outOfBounds = true;
                        } else {
                            float offset = 0.f;
                            if (!stopVelocity && (player.isShip || player.isDart) && player.stateRingJump &&
                                cameraZoomChanging(g))
                                offset = 1.f;
                            playerMoved(player);
                            player.y = offset + sizeOffset;
                            if (stopVelocity) {
                                player.yVelocity = 0;
                                player.isOnGround2 = false;
                                player.isOnGround = false;
                            } else if ((!player.isShip && !player.isSwing) || player.yVelocity < 0.0) {
                                hitGround(g, player, GObj{}, player.isUpsideDown);
                            }
                            updateCollide(player, player.isUpsideDown ? 0 : 1, GObj{});
                            if ((keepVelocity && static_cast<double>(savedYVelocity) < player.yVelocity) ||
                                (player.isUpsideDown && shipHolding && maxPortalYDelta < 0.f)) {
                                setYVelocity(player, static_cast<double>(savedYVelocity));
                            }
                            player.wasOnSlope = false;
                        }
                    } else {
                        outOfBounds = true;
                    }
                }
            } else {
                bool skip = false;
                if (player.wasOnSlope && halfHitbox + limitY < player.y) {
                    outOfBounds = true;
                    skip = true;
                }
                if (!skip) {
                    bool keepVelocity;
                    bool stopVelocity;
                    float savedYVelocity;
                    bool die = false;
                    if (!flyLike && player.stateHitHead < 1 && !player.isUpsideDown) {
                        if ((player.lastFlipTime == 0.0 ||
                             0.20000000298023224 <= player.totalTime - player.lastFlipTime) &&
                            (player.gameModeChangedTime == 0.0 ||
                             0.20000000298023224 <= player.totalTime - player.gameModeChangedTime))
                            die = true;
                        savedYVelocity = static_cast<float>(player.yVelocity);
                        stopVelocity = true;
                        keepVelocity = false;
                    } else {
                        savedYVelocity = static_cast<float>(player.yVelocity);
                        stopVelocity = false;
                        keepVelocity =
                                !(!player.isUpsideDown || ((player.isShip || player.isDart) && player.stateRingJump) ||
                                  maxPortalYDelta == 0.f);
                    }
                    if (die) {
                        outOfBounds = true;
                    } else {
                        playerMoved(player);
                        player.y = limitY;
                        if (stopVelocity) {
                            player.yVelocity = 0;
                            player.isOnGround2 = false;
                            player.isOnGround = false;
                        } else if (0.0 < player.yVelocity) {
                            hitGround(g, player, GObj{}, !player.isUpsideDown);
                        }
                        updateCollide(player, player.isUpsideDown ? 1 : 0, GObj{});
                        if ((keepVelocity && player.yVelocity < static_cast<double>(savedYVelocity)) ||
                            (!player.isUpsideDown && shipHolding && 0.f < maxPortalYDelta)) {
                            player.yVelocity = gdRound3(static_cast<double>(savedYVelocity));
                        }
                        player.wasOnSlope = false;
                    }
                }
            }
        }
        if (wasOutOfBounds && outOfBounds) {
            if (ignoreDamage) return 1;
            destroyPlayer(g, player, GObj{});
            return 1;
        }
        player.isOutOfBounds = outOfBounds;

        int const cellX = World::cellOfF(player.x);
        int const cellY = g.world->cellYOfF(player.y);
        tl_solids.clear();
        tl_hazards.clear();
        tl_winLo = -1e30f;
        tl_winHi = 1e30f;
        tl_needFull = false;
        auto const& cells = g.world->cells;
        Timeline const* timeline = g.trig.on ? nullptr : g.world->timeline;
        static thread_local std::vector<std::array<int, 5>> tl_areaCells;
        tl_areaCells.clear();
        auto const& areaMoved = g.trig.areaMoved;
        if (g.trig.on && !areaMoved.empty()) {
            trigAreaCellsList(g, tl_areaCells, std::max(cellX - 1, 0), cellX + 1, std::max(cellY - 1, 0), cellY + 1);
        }
        static thread_local std::vector<uint32_t> tl_amMark;
        static thread_local uint32_t tl_amStamp = 0;
        bool amBuilt = false;
        auto isAreaMoved = [&](int idx) {
            if (!amBuilt) {
                amBuilt = true;
                size_t const nObj = g.world->objs.size();
                if (tl_amMark.size() != nObj) {
                    tl_amMark.assign(nObj, 0u);
                    tl_amStamp = 0;
                }
                if (++tl_amStamp == 0) {
                    std::fill(tl_amMark.begin(), tl_amMark.end(), 0u);
                    tl_amStamp = 1;
                }
                for (int i : areaMoved) tl_amMark[static_cast<size_t>(i)] = tl_amStamp;
            }
            return tl_amMark[static_cast<size_t>(idx)] == tl_amStamp;
        };
        if (!cells.empty() || timeline) {
            int const x0 = std::max(cellX - 1, 0);
            int const x1 = cellX + 1;
            int const y0 = std::max(cellY - 1, 0);
            int const y1 = cellY + 1;
            for (int cx = x0; cx <= x1; ++cx) {
                for (int cy = y0; cy <= y1; ++cy) {
                    std::vector<int> const* staticList = nullptr;
                    bool overridden = false;
                    if (g.trig.on) {
                        staticList = trigCell(g, cx, cy);
                        overridden = staticList != nullptr;
                    }
                    if (!staticList && static_cast<size_t>(cx) < cells.size()) {
                        auto const& column = cells[static_cast<size_t>(cx)];
                        if (static_cast<size_t>(cy) < column.size()) staticList = &column[static_cast<size_t>(cy)];
                    }
                    tl_dyn.clear();
                    if (timeline && static_cast<size_t>(cx) < timeline->byColumn.size()) {
                        for (int dynIndex : timeline->byColumn[static_cast<size_t>(cx)]) {
                            TimelineKey const& key = timeline->at(dynIndex, g.tlTick);
                            if (World::cellOf(static_cast<double>(key.x)) != cx ||
                                g.world->cellYOf(static_cast<double>(key.y)) != cy)
                                continue;
                            tl_dyn.push_back(timeline->objs[static_cast<size_t>(dynIndex)].objIndex);
                        }
                    }
                    bool areaCell = false;
                    for (auto const& entry : tl_areaCells) {
                        if (entry[3] == cx && entry[4] == cy) {
                            tl_dyn.push_back(entry[0]);
                            areaCell = true;
                        } else if (entry[1] == cx && entry[2] == cy)
                            areaCell = true;
                    }
                    if (areaCell) {
                        auto const& objs = g.world->objs;
                        std::sort(tl_dyn.begin(), tl_dyn.end(), [&](int a, int b) {
                            return static_cast<unsigned>(objs[static_cast<size_t>(a)].uid) <
                                   static_cast<unsigned>(objs[static_cast<size_t>(b)].uid);
                        });
                        tl_merged.clear();
                        size_t staticPos = 0;
                        size_t dynPos = 0;
                        size_t const staticCount = staticList ? staticList->size() : 0;
                        while (staticPos < staticCount || dynPos < tl_dyn.size()) {
                            if (staticPos < staticCount && isAreaMoved((*staticList)[staticPos])) {
                                ++staticPos;
                                continue;
                            }
                            if (dynPos >= tl_dyn.size() ||
                                (staticPos < staticCount &&
                                 static_cast<unsigned>(objs[static_cast<size_t>((*staticList)[staticPos])].uid) <
                                         static_cast<unsigned>(objs[static_cast<size_t>(tl_dyn[dynPos])].uid)))
                                tl_merged.push_back((*staticList)[staticPos++]);
                            else
                                tl_merged.push_back(tl_dyn[dynPos++]);
                        }
                        collisionCheckObjects(g, player, tl_merged, dt);
                    } else if (tl_dyn.empty()) {
                        if (staticList && !staticList->empty()) {
                            World::CellIndex const* index = nullptr;
                            if (!overridden) {
                                if (static_cast<size_t>(cx) < g.world->cellIndex.size() &&
                                    static_cast<size_t>(cy) < g.world->cellIndex[static_cast<size_t>(cx)].size())
                                    index = &g.world->cellIndex[static_cast<size_t>(cx)][static_cast<size_t>(cy)];
                                if (index && g.trig.on && !index->mov.empty() && g.trig.anyMotion()) {
                                    bool stale = false;
                                    for (auto const& entry : index->mov) {
                                        if (trigObjMoved(g, g.world->objs[static_cast<size_t>(entry.idx)])) {
                                            stale = true;
                                            break;
                                        }
                                    }
                                    if (stale) index = dynamicIndex(g, World::cellKey(cx, cy), *staticList);
                                }
                            } else if (g.trig.on)
                                index = dynamicIndex(g, World::cellKey(cx, cy), *staticList);
                            std::vector<int> const* list = staticList;
                            if (index)
                                collisionCheckObjectsFiltered(g, player, *list, *index, dt);
                            else
                                collisionCheckObjects(g, player, *list, dt);
                        }
                    } else {
                        tl_merged.clear();
                        auto const& objs = g.world->objs;
                        size_t staticPos = 0;
                        size_t dynPos = 0;
                        size_t const staticCount = staticList ? staticList->size() : 0;
                        while (staticPos < staticCount || dynPos < tl_dyn.size()) {
                            if (dynPos >= tl_dyn.size() ||
                                (staticPos < staticCount &&
                                 static_cast<unsigned>(objs[static_cast<size_t>((*staticList)[staticPos])].uid) <
                                         static_cast<unsigned>(objs[static_cast<size_t>(tl_dyn[dynPos])].uid)))
                                tl_merged.push_back((*staticList)[staticPos++]);
                            else
                                tl_merged.push_back(tl_dyn[dynPos++]);
                        }
                        collisionCheckObjects(g, player, tl_merged, dt);
                    }
                }
            }
        }
        if (!g.world->extended.empty()) {
            collisionCheckObjectsFiltered(g, player, g.world->extended, g.world->extendedIndex, dt);
        }

        auto gatherFull = [&](std::vector<int>& solids, std::vector<int>& hazards) {
            solids.clear();
            hazards.clear();
            auto take = [&](std::vector<int> const& list) {
                for (int idx : list) {
                    GObj object = g.obj(idx);
                    int const type = object.getType();
                    if (type == Decoration || object.disabled()) continue;
                    if ((type == Breakable || type == SecretCoin || type == Collectible || type == UserCoin) &&
                        object.st().destroyed) {
                        continue;
                    }
                    if (type == Solid || type == Breakable)
                        solids.push_back(idx);
                    else if (type == Hazard || type == AnimatedHazard)
                        hazards.push_back(idx);
                }
            };
            if (!cells.empty()) {
                for (int cx = std::max(cellX - 1, 0); cx <= cellX + 1; ++cx)
                    for (int cy = std::max(cellY - 1, 0); cy <= cellY + 1; ++cy) {
                        std::vector<int> const* staticList = nullptr;
                        if (g.trig.on) staticList = trigCell(g, cx, cy);
                        if (!staticList && static_cast<size_t>(cx) < cells.size()) {
                            auto const& column = cells[static_cast<size_t>(cx)];
                            if (static_cast<size_t>(cy) < column.size()) staticList = &column[static_cast<size_t>(cy)];
                        }
                        if (!tl_areaCells.empty()) {
                            static thread_local std::vector<int> tmp;
                            static thread_local std::vector<int> add;
                            tmp.clear();
                            add.clear();
                            bool areaCell = false;
                            for (auto const& entry : tl_areaCells) {
                                if (entry[3] == cx && entry[4] == cy) {
                                    add.push_back(entry[0]);
                                    areaCell = true;
                                } else if (entry[1] == cx && entry[2] == cy)
                                    areaCell = true;
                            }
                            if (areaCell) {
                                auto const& objs = g.world->objs;
                                auto uidOf = [&](int i) {
                                    return static_cast<unsigned>(objs[static_cast<size_t>(i)].uid);
                                };
                                if (staticList) {
                                    for (int i : *staticList) {
                                        if (!isAreaMoved(i)) tmp.push_back(i);
                                    }
                                }
                                for (int i : add) tmp.push_back(i);
                                std::stable_sort(tmp.begin(), tmp.end(),
                                                 [&](int a, int b) { return uidOf(a) < uidOf(b); });
                                take(tmp);
                                continue;
                            }
                        }
                        if (staticList) {
                            static thread_local std::vector<int> srt;
                            srt.assign(staticList->begin(), staticList->end());
                            auto const& objs = g.world->objs;
                            std::stable_sort(srt.begin(), srt.end(), [&](int a, int b) {
                                return static_cast<unsigned>(objs[static_cast<size_t>(a)].uid) <
                                       static_cast<unsigned>(objs[static_cast<size_t>(b)].uid);
                            });
                            take(srt);
                        }
                    }
            }
            take(g.world->extended);
        };
        static thread_local std::vector<int> tl_fullS;
        static thread_local std::vector<int> tl_fullH;
        if (tl_needFull && timeline == nullptr) {
            gatherFull(tl_fullS, tl_fullH);
            tl_solids.swap(tl_fullS);
            tl_hazards.swap(tl_fullH);
            tl_winLo = -1e30f;
            tl_winHi = 1e30f;
        }
        bool const anyFiltered = tl_winLo > -1e29f || tl_winHi < 1e29f;
        bool rebuilt = timeline != nullptr || !anyFiltered;
        Rect playerBox = playerRect(player);
        float playerMaxX = getMaxX(playerBox);
        float playerMinX = getMinX(playerBox);
        float playerMaxY = getMaxY(playerBox);
        float playerMinY = getMinY(playerBox);
        bool stick = player.lastGroundObject != 0 && player.isCollidingWithSlope;
        Rect const innerBox = playerInnerRect(player);
        if (!rebuilt && (playerMinY < tl_winLo || playerMaxY > tl_winHi)) {
            rebuilt = true;
            gatherFull(tl_fullS, tl_fullH);
            tl_solids.swap(tl_fullS);
            tl_hazards.swap(tl_fullH);
            if (g_trace)
                std::printf(
                        "      [trace] player outside the filter window before the solids (%.2f..%.2f): full lists\n",
                        tl_winLo, tl_winHi);
        }
        for (int i = static_cast<int>(tl_solids.size()) - 1; i >= 0; --i) {
            GObj o = g.obj(tl_solids[static_cast<size_t>(i)]);
            if (o.disabled() || (o.o->type == Breakable && o.st().destroyed)) continue;
            Rect const box = o.getObjectRect();
            g.tickGap = std::min(g.tickGap, rectGap(innerBox, box));
            if (player.isDart) g.hazGap = std::min(g.hazGap, rectGap(playerBox, box));
            if (stick && !o.o->groups.empty()) {
                Point const last = o.lastPosition();
                Point const real = o.getRealPosition();
                if (last.x != real.x || last.y != real.y) {
                    bool const down = !player.isUpsideDown ? real.y < last.y : last.y < real.y;
                    if (down) {
                        Rect stickBox = box;
                        stickBox.h += dt * 5.f;
                        if (player.isUpsideDown) stickBox.y -= dt * 5.f;
                        if (intersectsRect(playerBox, stickBox)) {
                            player.collidedObject = o.uid();
                            stick = false;
                        }
                    }
                }
            }
            if (g_trace && std::fabs(box.x + box.w * 0.5f - player.x) < 90.f &&
                std::fabs(box.y + box.h * 0.5f - player.y) < 90.f)
                std::printf(
                        "      [trace] solid uid %d id %d rect [%.2f %.2f %.2f %.2f] %s (player [%.2f %.2f %.2f "
                        "%.2f])\n",
                        o.uid(), o.o->id, box.x, box.y, box.w, box.h,
                        (getMinX(box) <= playerMaxX && playerMinX <= getMaxX(box) && getMinY(box) <= playerMaxY &&
                         playerMinY <= getMaxY(box))
                                ? "TOUCHED"
                                : "not touched",
                        playerMinX, playerMinY, playerMaxX - playerMinX, playerMaxY - playerMinY);
            if (getMinX(box) <= playerMaxX && playerMinX <= getMaxX(box) && getMinY(box) <= playerMaxY &&
                playerMinY <= getMaxY(box)) {
                collidedWithObject(g, player, dt, o, zeroRect(), false);
                playerBox = playerRect(player);
                playerMaxX = getMaxX(playerBox);
                playerMinX = getMinX(playerBox);
                playerMaxY = getMaxY(playerBox);
                playerMinY = getMinY(playerBox);
                if (!rebuilt && (playerMinY < tl_winLo || playerMaxY > tl_winHi)) {
                    rebuilt = true;
                    int const currentIdx = tl_solids[static_cast<size_t>(i)];
                    gatherFull(tl_fullS, tl_fullH);
                    int j = -1;
                    for (int k = static_cast<int>(tl_fullS.size()) - 1; k >= 0; --k) {
                        if (tl_fullS[static_cast<size_t>(k)] == currentIdx) {
                            j = k;
                            break;
                        }
                    }
                    if (j >= 0) {
                        tl_solids.swap(tl_fullS);
                        tl_hazards.swap(tl_fullH);
                        i = j;
                    }
                }
            }
        }
        if (g.died) return 1;
        playerBox = playerRect(player);
        playerMaxX = getMaxX(playerBox);
        playerMinX = getMinX(playerBox);
        playerMaxY = getMaxY(playerBox);
        playerMinY = getMinY(playerBox);
        bool spikeTodo = g.spikeOn && std::fabs(player.x - g.spikeX) < 60.f && std::fabs(player.y - g.spikeY) < 60.f;
        int const spikeCellX = spikeTodo ? World::cellOfF(g.spikeX) : 0;
        int const spikeCellY = spikeTodo ? g.world->cellYOfF(g.spikeY) : 0;
        auto spikeHit = [&]() -> bool {
            spikeTodo = false;
            Rect const spikeBox{g.spikeX - 3.f, g.spikeY - 6.f, 6.f, 12.f};
            if (!(getMinX(spikeBox) <= playerMaxX && playerMinX <= getMaxX(spikeBox) &&
                  getMinY(spikeBox) <= playerMaxY && playerMinY <= getMaxY(spikeBox)))
                return false;
            g.spikeVerified = true;
            return true;
        };
        for (size_t i = 0; i < tl_hazards.size(); ++i) {
            GObj o = g.obj(tl_hazards[i]);
            if (spikeTodo) {
                bool after = o.o->extendedCollision;
                if (!after) {
                    Rect const hazardBox = o.getObjectRect();
                    int const hazardCellX = World::cellOfF(hazardBox.x + hazardBox.w * 0.5f);
                    int const hazardCellY = g.world->cellYOfF(hazardBox.y + hazardBox.h * 0.5f);
                    after = hazardCellX > spikeCellX || (hazardCellX == spikeCellX && hazardCellY > spikeCellY);
                }
                if (after && spikeHit()) return 1;
            }
            if (o.disabled()) continue;
            bool hit = false;
            if (o.o->radius <= 0.f) {
                Rect const& box = o.getObjectRect();
                hit = getMinX(box) <= playerMaxX && playerMinX <= getMaxX(box) && getMinY(box) <= playerMaxY &&
                      playerMinY <= getMaxY(box);
                g.tickGap = std::min(g.tickGap, rectGap(playerBox, box));
                g.hazGap = std::min(g.hazGap, rectGap(playerBox, box));
            } else {
                hit = playerCircleCollision(g, player, o);
                Point const center = o.getPosition();
                float const distance = std::sqrt((player.x - center.x) * (player.x - center.x) +
                                                 (player.y - center.y) * (player.y - center.y));
                float const radius = o.o->radius;
                g.tickGap = std::min(g.tickGap, distance - (playerBox.w * 0.5f + radius));
                g.hazGap = std::min(g.hazGap, distance - (playerBox.w * 0.5f + radius));
            }
            if (g_trace)
                std::printf(
                        "      [trace] hazard uid %d id %d rect [%.2f %.2f %.2f %.2f] %s (player [%.2f %.2f %.2f "
                        "%.2f])%s\n",
                        o.uid(), o.o->id, o.getObjectRect().x, o.getObjectRect().y, o.getObjectRect().w,
                        o.getObjectRect().h, hit ? "TOUCHED" : "not touched", playerMinX, playerMinY,
                        playerMaxX - playerMinX, playerMaxY - playerMinY,
                        o.o->radius > 0.f ? fmt_circle(g, o).c_str() : "");
            if (!hit) continue;
            if (!o.hasOrientedBox() || (g.circleOption && 0.f < o.o->radius)) {
                if (ignoreDamage) return 1;
                destroyPlayer(g, player, o);
                return 1;
            }
            OBB const objectOBB = objectObb(o);
            OBB const playerOBB = playerObb(player);
            bool const obbHit = objectOBB.overlaps1Way(playerOBB) && playerOBB.overlaps1Way(objectOBB);
            if (!g.rotAmb && obbRotAmbiguous(player, objectOBB, obbHit)) g.rotAmb = true;
            if (obbHit) {
                if (ignoreDamage) return 1;
                destroyPlayer(g, player, o);
                return 1;
            }
        }
        if (spikeTodo && spikeHit()) return 1;
        postCollision(g, player, dt, g.isBetweenSteps);
        return 0;
    }

}
