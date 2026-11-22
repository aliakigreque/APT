// Types, level maps and helpers shared by the search files (search*.cpp), and the SearchRun class whose stages they
// define. Only those files include it; the rest of the simulator goes through search.hpp.

#pragma once
#include "search.hpp"
#include "triggers.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <chrono>
#include <cmath>
#include <condition_variable>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <map>
#include <array>
#include <set>
#include <optional>
#include <functional>

namespace ap::sim::search_impl {
    // 4 seconds of play at the game's 240 steps per second: how far back a restart or a revisit kill still
    // counts as recent
    constexpr int kFourSeconds = 4 * 240;

    struct Key {
        int32_t x;
        int32_t y;
        int32_t v;
        uint32_t flags;
        uint32_t extra;
        // true if both search keys are equal
        bool operator==(Key const& other) const {
            return x == other.x && y == other.y && v == other.v && flags == other.flags && extra == other.extra;
        }
    };

    struct KeyHash {
        // splitmix64 mixer
        static uint64_t splitmix(uint64_t z) {
            z += 0x9e3779b97f4a7c15ull;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            return z ^ (z >> 31);
        }

        // hash of a search key
        size_t operator()(Key const& k) const {
            uint64_t h =
                    splitmix((static_cast<uint64_t>(static_cast<uint32_t>(k.x)) << 32) | static_cast<uint32_t>(k.y));
            h ^= splitmix((static_cast<uint64_t>(static_cast<uint32_t>(k.v)) << 32) | k.flags);
            h = splitmix(h ^ k.extra);
            return static_cast<size_t>(h);
        }
    };

    template <class V>
    class KeyTable {
    public:
        // empties the table
        void clear() {
            ++m_gen;
            if (m_gen == 0) {
                for (auto& e : m_slots) e.gen = 0;
                m_gen = 1;
            }
            m_count = 0;
        }

        // finds the key or inserts it with value, returns (value pointer, was already there)
        std::pair<V*, bool> findOrInsert(Key const& k, V value) {
            if ((m_count + 1) * 2 > m_slots.size()) grow();
            size_t const mask = m_slots.size() - 1;
            size_t i = KeyHash{}(k)&mask;
            for (;;) {
                Slot& s = m_slots[i];
                if (s.gen != m_gen) {
                    s.gen = m_gen;
                    s.key = k;
                    s.value = value;
                    ++m_count;
                    return {&s.value, false};
                }
                if (s.key == k) return {&s.value, true};
                i = (i + 1) & mask;
            }
        }

    private:
        struct Slot {
            Key key;
            V value;
            uint32_t gen = 0;
        };
        std::vector<Slot> m_slots = std::vector<Slot>(1024);
        uint32_t m_gen = 1;
        size_t m_count = 0;

        // doubles the size and puts back the current keys
        void grow() {
            std::vector<Slot> old = std::move(m_slots);
            m_slots.assign(old.size() * 2, Slot{});
            m_count = 0;
            size_t const mask = m_slots.size() - 1;
            for (auto const& e : old) {
                if (e.gen != m_gen) continue;
                size_t i = KeyHash{}(e.key) & mask;
                while (m_slots[i].gen == m_gen) i = (i + 1) & mask;
                m_slots[i] = e;
                ++m_count;
            }
        }
    };

    Key makeKey(Game const& g, float quantY, float quantV);

    struct Band {
        float lo = 0;
        float hi = 0;
        int n = 0;
    };

    constexpr float kBandW = 300.f;

    constexpr float kBandMargin = 210.f;

    struct EmptyGrid {
        static constexpr float kCell = 30.f;
        float x0 = 0;
        float y0 = 0;
        int nx = 0;
        int ny = 0;
        std::vector<uint8_t> d;

        // distance at a point, 255 outside the grid
        int at(float x, float y) const {
            int const cx = static_cast<int>(std::floor((x - x0) / kCell));
            int const cy = static_cast<int>(std::floor((y - y0) / kCell));
            if (cx < 0 || cy < 0 || cx >= nx || cy >= ny) return 255;
            return d[static_cast<size_t>(cy) * static_cast<size_t>(nx) + static_cast<size_t>(cx)];
        }
    };

    constexpr int kEmptyCells = 5;

    struct Extent {
        float x0 = 0;
        float groundY = 105.f;
        std::vector<float> lo;
        std::vector<float> hi;
    };

    void buildExtent(World const& w, Extent& extent, float groundY);

    // returns true if a player is above or below the level extent by more than margin (half margin when flying)
    inline bool outsideExtent(Game const& g, Extent const& extent, float margin) {
        if (extent.lo.empty() || g.p1.isSideways) return false;
        auto out = [&](Player const& p) {
            float const localMargin = (p.isShip || p.isBird || p.isDart || p.isSwing) ? margin * 0.5f : margin;
            long const col = static_cast<long>((p.x - extent.x0) / 30.f);
            if (col < 0 || col >= static_cast<long>(extent.lo.size())) return false;
            if (extent.hi[static_cast<size_t>(col)] < extent.lo[static_cast<size_t>(col)]) return false;
            if (p.y < extent.groundY + margin) return false;
            return p.y > extent.hi[static_cast<size_t>(col)] + localMargin ||
                   p.y < extent.lo[static_cast<size_t>(col)] - localMargin;
        };
        return out(g.p1) || (g.isDualMode && out(g.p2));
    }

    // returns true if a player outside the extent still has trigger moved gameplay on its side
    inline bool movedGameplayNear(Game& g, Extent const& extent, float margin) {
        if (extent.lo.empty() || !g.trig.on || g.p1.isSideways) return false;
        auto side = [&](Player const& p) -> int {
            float const localMargin = (p.isShip || p.isBird || p.isDart || p.isSwing) ? margin * 0.5f : margin;
            long const col = static_cast<long>((p.x - extent.x0) / 30.f);
            if (col < 0 || col >= static_cast<long>(extent.lo.size())) return 0;
            if (extent.hi[static_cast<size_t>(col)] < extent.lo[static_cast<size_t>(col)]) return 0;
            if (p.y < extent.groundY + margin) return 0;
            return p.y > extent.hi[static_cast<size_t>(col)] + localMargin
                           ? 1
                           : (p.y < extent.lo[static_cast<size_t>(col)] - localMargin ? -1 : 0);
        };
        auto nearOne = [&](Player const& p, int dir) {
            float const localMargin = (p.isShip || p.isBird || p.isDart || p.isSwing) ? margin * 0.5f : margin;
            return dir > 0 ? trigMovedGameplayIn(g, p.x - 150.f, p.x + 150.f, p.y - localMargin, 1e7f)
                           : trigMovedGameplayIn(g, p.x - 150.f, p.x + 150.f, -1e7f, p.y + localMargin);
        };
        int const side1 = side(g.p1);
        int const side2 = g.isDualMode ? side(g.p2) : 0;
        if (side1 == 0 && side2 == 0) return false;
        return (side1 == 0 || nearOne(g.p1, side1)) && (side2 == 0 || nearOne(g.p2, side2));
    }

    struct VoidHull {
        static constexpr float kCol = 30.f;
        static constexpr float kAhead = 3000.f;
        static constexpr float kMargin = 300.f;
        float x0 = 0;
        std::vector<float> top;
        std::vector<float> bot;

        // returns true if the player can never land again
        bool doomed(Player const& p) const {
            if (top.empty() || p.isShip || p.isBird || p.isDart || p.isSwing || p.isSideways) return false;
            int const col = static_cast<int>(std::floor((p.x - x0) / kCol));
            if (col < 0 || static_cast<size_t>(col) >= top.size()) return false;
            if (!p.isUpsideDown) return p.y < bot[static_cast<size_t>(col)] - kMargin && p.yVelocity <= 0.0;
            return p.y > top[static_cast<size_t>(col)] + kMargin && p.yVelocity >= 0.0;
        }
    };

    void buildVoidHull(World const& w, VoidHull& hull, std::vector<std::array<float, 4>> const& excluded = {});

    struct LevelPresence {
        static constexpr float kBin = 50.f;
        float yBase = 0.f;
        int nBins = 0;
        int nWords = 0;
        int nCols = 0;
        bool ySec = false;
        std::vector<uint64_t> col;
        std::vector<uint8_t> geoCol;
        std::vector<uint8_t> game;

        // true if the presence map was built
        bool ok() const { return nCols > 0 && nWords > 0; }

        // returns true if the object counts for presence
        static bool isGame(Obj const& o) {
            if (o.isSpeedPortal()) return true;
            switch (o.type) {
            case Decoration:
            case Modifier:
            case CollisionObject:
            case EnterEffectObject:
                return false;
            default:
                break;
            }
            return !(o.rect.w > 3000.f || o.rect.h > 3000.f);
        }

        // height bin of y
        int binOf(float y) const { return static_cast<int>(std::floor((y - yBase) / kBin)); }

        // sets bits first to last
        static void setRange(uint64_t* bits, int first, int last) {
            for (int b = first; b <= last; ++b) bits[static_cast<size_t>(b) >> 6] |= 1ull << (b & 63);
        }

        // returns true if any bit from first to last is set, one word at a time
        static bool anyRange(uint64_t const* bits, int first, int last) {
            for (int b = first; b <= last;) {
                int const word = b >> 6;
                int const lo = b & 63;
                int const hi = std::min(63, last - (word << 6));
                uint64_t const mask = (hi == 63 ? ~0ull : ((1ull << (hi + 1)) - 1)) & ~((1ull << lo) - 1);
                if (bits[word] & mask) return true;
                b = (word + 1) << 6;
            }
            return false;
        }

        // fills the map from the level objects at their start position
        void build(World const& w) {
            game.assign(w.objs.size(), 0);
            float lo = 1e30f;
            float hi = -1e30f;
            float maxX = 0.f;
            for (size_t i = 0; i < w.objs.size(); ++i) {
                Obj const& o = w.objs[i];
                if (!isGame(o)) continue;
                game[i] = 1;
                lo = std::min(lo, o.rect.y);
                hi = std::max(hi, o.rect.maxY());
                maxX = std::max(maxX, o.rect.maxX());
            }
            if (!(lo < hi)) return;
            ySec = w.ySection;
            yBase = std::floor((lo - 3000.f) / kBin) * kBin;
            nBins = std::min(static_cast<int>((hi + 3000.f - yBase) / kBin) + 1, 64 * 64);
            nWords = (nBins + 63) / 64;
            nCols = World::cellOfF(maxX) + 2;
            col.assign(static_cast<size_t>(nCols) * static_cast<size_t>(nWords), 0ull);
            geoCol.assign(static_cast<size_t>(nCols), 0);
            for (size_t i = 0; i < w.objs.size(); ++i) {
                if (!game[i]) continue;
                Rect const& r = w.objs[i].rect;
                int const c0 = World::cellOfF(r.x);
                int const c1 = std::min(std::min(World::cellOfF(r.maxX()), c0 + 30), nCols - 1);
                int const b0 = std::clamp(binOf(r.y), 0, nBins - 1);
                int const b1 = std::clamp(binOf(r.maxY()), 0, nBins - 1);
                for (int c = c0; c <= c1; ++c)
                    setRange(&col[static_cast<size_t>(c) * static_cast<size_t>(nWords)], b0, b1);
                if (isGeometry(w.objs[i]))
                    for (int c = std::max(c0, 0); c <= c1; ++c) geoCol[static_cast<size_t>(c)] = 1;
            }
        }

        // solid blocks, hazards, slopes and breakables (no portals or orbs)
        static bool isGeometry(Obj const& o) {
            switch (o.type) {
            case Solid:
            case Hazard:
            case Slope:
            case AnimatedHazard:
            case Breakable:
                break;
            default:
                return false;
            }
            return !(o.rect.w > 3000.f || o.rect.h > 3000.f);
        }

        // height bins used by the objects of a moved cell at their current position, cached per tick
        uint64_t const* dynBits(Game const& g, std::vector<int> const& cellObjs) const {
            struct E {
                void const* lst;
                uint64_t mv;
                uint64_t dv;
                std::vector<uint64_t> bits;
            };
            static thread_local std::vector<E> cache;
            static thread_local int cacheTick = -1;
            static thread_local WKey cacheWorld;
            if (cacheTick != g.tick || cacheWorld != g.world || cache.size() > 4096) {
                cache.clear();
                cacheTick = g.tick;
                cacheWorld = g.world;
            }
            uint64_t const motionVer = g.trig.r().M().ver;
            uint64_t const dynVer = g.trig.d ? g.trig.rd().ver : 0;
            for (auto const& e : cache) {
                if (e.lst == &cellObjs && e.mv == motionVer && e.dv == dynVer) return e.bits.data();
            }
            E e{&cellObjs, motionVer, dynVer, std::vector<uint64_t>(static_cast<size_t>(nWords), 0ull)};
            World const& w = *g.world;
            for (int idx : cellObjs) {
                if (idx < 0 || static_cast<size_t>(idx) >= game.size() || !game[static_cast<size_t>(idx)]) continue;
                Obj const& o = w.objs[static_cast<size_t>(idx)];
                double x;
                double y;
                trigSharedPos(g, idx, x, y);
                float const dy = static_cast<float>(y - o.posY);
                int const b0 = std::clamp(binOf(o.rect.y + dy), 0, nBins - 1);
                int const b1 = std::clamp(binOf(o.rect.maxY() + dy), 0, nBins - 1);
                setRange(e.bits.data(), b0, b1);
            }
            cache.push_back(std::move(e));
            return cache.back().bits.data();
        }

        // gameplay object in the box x +- rx, y +- ry ? 0 no, 1 yes, 2 no and the stretch has no geometry at all
        int any(Game const& g, float x, float y, float rx, float ry) const {
            int const c0 = World::cellOfF(x - rx);
            int const c1 = World::cellOfF(x + rx);
            int b0 = binOf(y - ry);
            int b1 = binOf(y + ry);
            if (b1 < 0 || b0 >= nBins) return 0;
            b0 = std::max(b0, 0);
            b1 = std::min(b1, nBins - 1);
            for (int c = c0; c <= std::min(c1, nCols - 1); ++c) {
                if (anyRange(&col[static_cast<size_t>(c) * static_cast<size_t>(nWords)], b0, b1)) {
                    return 1;
                }
            }
            if (!g.trig.on || !g.trig.h) return 0;
            if (TrigMotion const& motion = g.trig.r().M(); motion.pset && !motion.pset->collObjs.empty()) {
                for (int idx : motion.pset->collObjs) {
                    if (idx < 0 || static_cast<size_t>(idx) >= game.size() || !game[static_cast<size_t>(idx)]) continue;
                    if (!isGeometry(g.world->objs[static_cast<size_t>(idx)])) continue;
                    Rect const r = trigRect(g, idx);
                    if (r.maxX() >= x - rx && r.x <= x + rx && r.maxY() >= y - ry && r.y <= y + ry) {
                        return 1;
                    }
                }
            }
            auto const& overrides = g.trig.r().M().cellOv;
            if (overrides.items.empty()) return 0;
            for (int c = c0; c <= c1; ++c) {
                auto it = overrides.lower(World::cellKey(c, 0));
                for (; it != overrides.items.end() && it->first < World::cellKey(c + 1, 0); ++it) {
                    if (!it->second.v) continue;
                    if (ySec) {
                        float const cellY = static_cast<float>(it->first - World::cellKey(c, 0)) * 100.f;
                        if (cellY + 300.f < y - ry || cellY - 200.f > y + ry) continue;
                    }
                    if (anyRange(dynBits(g, *it->second.v), b0, b1)) {
                        return 1;
                    }
                }
            }
            {
                bool const empty = emptyStretch(g, c0, c1);
                return empty ? 2 : 0;
            }
        }

        // returns true if cell columns c0 to c1 have no geometry (static, moved or in the player set)
        bool emptyStretch(Game const& g, int c0, int c1) const {
            if (c0 < 0) return false;
            c1 = std::min(c1, nCols - 1);
            if (c0 > c1) return true;
            for (int c = c0; c <= c1; ++c)
                if (geoCol[static_cast<size_t>(c)]) return false;
            if (g.trig.on && g.trig.h) {
                World const& w = *g.world;
                auto const& overrides = g.trig.r().M().cellOv;
                auto it = overrides.lower(World::cellKey(c0, 0));
                for (; it != overrides.items.end() && it->first < World::cellKey(c1 + 1, 0); ++it)
                    if (it->second.v)
                        for (int idx : *it->second.v)
                            if (idx >= 0 && static_cast<size_t>(idx) < w.objs.size() &&
                                isGeometry(w.objs[static_cast<size_t>(idx)]))
                                return false;
                if (TrigMotion const& motion = g.trig.r().M(); motion.pset)
                    for (int idx : motion.pset->collObjs) {
                        if (idx < 0 || static_cast<size_t>(idx) >= w.objs.size() ||
                            !isGeometry(w.objs[static_cast<size_t>(idx)]))
                            continue;
                        Rect const r = trigRect(g, idx);
                        if (World::cellOfF(r.maxX()) >= c0 && World::cellOfF(r.x) <= c1) return false;
                    }
            }
            return true;
        }
    };

    struct Attract {
        float x;
        float yLo;
        float yHi;
    };

    struct StructureMap {
        float x0 = 0;
        std::vector<std::vector<std::pair<float, float>>> cols;

        // true if the structure map has no column
        bool empty() const { return cols.empty(); }

        // fills the map from the level geometry
        void build(World const& w) {
            float minX = 1e30f;
            float maxX = -1e30f;
            auto geo = [](Obj const& o) {
                switch (o.type) {
                case Solid:
                case Hazard:
                case Slope:
                case AnimatedHazard:
                case Breakable:
                    break;
                default:
                    return false;
                }
                return !(o.rect.w > 3000.f || o.rect.h > 3000.f);
            };
            for (auto const& o : w.objs)
                if (geo(o)) {
                    minX = std::min(minX, o.rect.x);
                    maxX = std::max(maxX, o.rect.x + o.rect.w);
                }
            if (!(minX < maxX) || maxX - minX > 3.0e6f) return;
            x0 = std::floor(minX / 30.f) * 30.f;
            size_t const n = static_cast<size_t>((maxX - x0) / 30.f) + 2;
            cols.assign(n, {});
            for (auto const& o : w.objs) {
                if (!geo(o)) continue;
                size_t const firstCol = static_cast<size_t>(std::max(0.f, (o.rect.x - x0) / 30.f));
                size_t const lastCol =
                        std::min(n - 1, static_cast<size_t>(std::max(0.f, (o.rect.x + o.rect.w - x0) / 30.f)));
                for (size_t c = firstCol; c <= lastCol; ++c) cols[c].emplace_back(o.rect.y, o.rect.y + o.rect.h);
            }
            for (auto& v : cols) {
                std::sort(v.begin(), v.end());
                v.erase(std::unique(v.begin(), v.end()), v.end());
            }
        }

        // returns true if geometry separates height y from [lo, hi] in the column of x (another floor)
        bool separated(float x, float y, float lo, float hi) const {
            long const c = static_cast<long>(std::floor((x - x0) / 30.f));
            if (c < 0 || c >= static_cast<long>(cols.size())) return false;
            for (auto const& e : cols[static_cast<size_t>(c)]) {
                if (y > hi && e.first >= hi && e.second <= y - 15.f) return true;
                if (y < lo && e.second <= lo && e.first >= y + 15.f) return true;
            }
            return false;
        }

        // free interval around height y in the column of x, +-1e30 when there is no geometry on that side
        std::pair<float, float> freeAt(float x, float y) const {
            float lo = -1e30f;
            float hi = 1e30f;
            long const c = static_cast<long>(std::floor((x - x0) / 30.f));
            if (c < 0 || c >= static_cast<long>(cols.size())) return {lo, hi};
            for (auto const& e : cols[static_cast<size_t>(c)]) {
                if (e.second <= y + 1.f)
                    lo = std::max(lo, e.second);
                else if (e.first >= y - 1.f)
                    hi = std::min(hi, e.first);
            }
            return {lo, hi};
        }

        // 1 on the roof of the structure, 2 under its bottom, 0 inside (or nothing around)
        int escaped(float x, float y, float groundY) const {
            long const c = static_cast<long>(std::floor((x - x0) / 30.f));
            if (c < 1 || c + 1 >= static_cast<long>(cols.size())) return 0;
            static thread_local std::vector<std::pair<float, float>> intervals;
            intervals.clear();
            for (long k = c - 1; k <= c + 1; ++k)
                intervals.insert(intervals.end(), cols[static_cast<size_t>(k)].begin(),
                                 cols[static_cast<size_t>(k)].end());
            if (intervals.empty()) return 0;
            bool above = false;
            bool below = false;
            for (auto const& e : intervals) {
                if (e.first > y + 15.f) above = true;
                if (e.second < y - 15.f) below = true;
            }
            if (above == below) return 0;
            std::sort(intervals.begin(), intervals.end());
            int numLayers = 0;
            float top = -1e30f;
            for (auto const& e : intervals) {
                bool const side = above ? e.first > y + 15.f : e.second < y - 15.f;
                if (!side) continue;
                if (numLayers == 0 || e.first > top + 60.f) {
                    ++numLayers;
                    top = e.second;
                } else
                    top = std::max(top, e.second);
                if (numLayers >= 2) break;
            }
            if (numLayers < 2) return 0;
            if (below) return 1;
            return y > groundY + 600.f ? 2 : 0;
        }
    };

    constexpr float kAttractAhead = 2400.f;

    constexpr float kAttractTol = 90.f;

    constexpr int kRingGap = 6;

    constexpr int kVariants = 2;

    struct Trap {
        float x0;
        float x1;
        int idx;
    };

    struct Cues {
        std::vector<Trap> traps;
        std::vector<World::PortalCluster> const* clusters = nullptr;
        std::vector<World::PortalCluster> extra;
        std::vector<std::pair<float, int>> passX;
        std::vector<std::pair<float, int>> approachX;
        std::vector<Band> bands;
        EmptyGrid empty;
        Extent extent;
        VoidHull voidHull;
        std::vector<Attract> attract;
        LevelPresence presence;
        StructureMap structure;
        float endZoneX = std::numeric_limits<float>::infinity();
    };

    // lowest x of an end trigger that fires by position, infinity if none
    inline float endActivationX(World const& w) {
        std::unordered_map<int, std::vector<int>> spawners;
        for (size_t i = 0; i < w.objs.size(); ++i) {
            Obj const& o = w.objs[i];
            if (o.cls == 1 && o.id == ObjId::SpawnTrigger && o.targetGroup > 0)
                spawners[o.targetGroup].push_back(static_cast<int>(i));
        }
        float best = std::numeric_limits<float>::infinity();
        for (size_t i = 0; i < w.objs.size(); ++i) {
            if (!(w.objs[i].cls == 1 && w.objs[i].id == ObjId::EndTrigger)) continue;
            std::vector<int> frontier{static_cast<int>(i)};
            std::vector<int> seen;
            for (int depth = 0; depth < 6 && !frontier.empty(); ++depth) {
                std::vector<int> next;
                for (int k : frontier) {
                    if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
                    seen.push_back(k);
                    Obj const& o = w.objs[static_cast<size_t>(k)];
                    if (!o.spawnTriggered && !o.touchTriggered) {
                        best = std::min(best, o.x);
                        continue;
                    }
                    if (o.spawnTriggered)
                        for (short group : o.groups) {
                            auto it = spawners.find(group);
                            if (it != spawners.end()) next.insert(next.end(), it->second.begin(), it->second.end());
                        }
                }
                frontier.swap(next);
            }
        }
        return best;
    }

    // 1 if a learned trigger ahead wants the player at another height, 0 otherwise
    inline int attractCost(Game const& g, Cues const& cues) {
        Player const& p = g.p1;
        if (p.isSideways) return 0;
        bool any = false;
        for (auto const& a : cues.attract) {
            float const ahead = a.x - p.x;
            if (!(ahead > 0.f && ahead < kAttractAhead)) continue;
            float const dy = p.y < a.yLo ? a.yLo - p.y : (p.y > a.yHi ? p.y - a.yHi : 0.f);
            if (dy <= kAttractTol) return 0;
            any = true;
        }
        return any ? 1 : 0;
    }

    void buildEmpty(World const& w, EmptyGrid& grid);

    // returns true if the players are at least kEmptyCells cells from any gameplay object
    inline bool farFromLevel(Game const& g, Cues const& cues) {
        if (cues.empty.nx == 0) return false;
        Player const& p = g.p1;
        if (cues.empty.at(p.x, p.y) < kEmptyCells) return false;
        if (g.isDualMode && cues.empty.at(g.p2.x, g.p2.y) < kEmptyCells) return false;
        return true;
    }

    void buildBands(World const& w, std::vector<Band>& out);

    // returns true if player 1 is more than kBandMargin away from the play band of its slice
    inline bool outOfBand(Game const& g, Cues const& cues) {
        Player const& p = g.p1;
        if (p.isSideways || cues.bands.empty()) return false;
        int const b = static_cast<int>(std::floor(p.x / kBandW));
        if (b < 0 || static_cast<size_t>(b) >= cues.bands.size()) return false;
        Band const& band = cues.bands[static_cast<size_t>(b)];
        if (band.n == 0) return false;
        return p.y < band.lo - kBandMargin || p.y > band.hi + kBandMargin;
    }

    int lateSpeedCost(Game const& g, float prevX, Cues const& cues, SearchOptions const& opt);

    char modeCharOf(Player const& p);

    uint8_t modeBitOf(Player const& p);

    World::PortalCluster const& clusterOf(Cues const& cues, int k);

    // returns true if the portal would change nothing for player 1 as it is now
    inline bool portalNoOp(Game const& g, Obj const& o) {
        Player const& p = g.p1;
        if (o.cls != 0 && o.cls != 1) return false;
        switch (o.type) {
        case NormalGravityPortal:
            return !p.isUpsideDown;
        case InverseGravityPortal:
            return p.isUpsideDown;
        case RegularSizePortal:
            return !(p.vehicleSize < 1.f);
        case MiniSizePortal:
            return p.vehicleSize < 1.f;
        case ShipPortal:
            return p.isShip;
        case UfoPortal:
            return p.isBird;
        case BallPortal:
            return p.isBall;
        case WavePortal:
            return p.isDart;
        case RobotPortal:
            return p.isRobot;
        case SpiderPortal:
            return p.isSpider;
        case SwingPortal:
            return p.isSwing;
        case CubePortal:
            return !(p.isShip || p.isBird || p.isBall || p.isDart || p.isRobot || p.isSpider || p.isSwing);
        default:
            return false;
        }
    }

    bool prepaid(World::PortalCluster const& c);

    bool portalTakenBy(Game const& g, int idx);

    bool isGameModePortal(int type);

    bool clusterTaken(Game const& g, World::PortalCluster const& c);

    float clusterDy(Game const& g, World::PortalCluster const& c);

    inline std::atomic<float> g_waveExitReach{0.f};

    // vertical reach of a cluster: 160 when flying, 90 on the ground, more for a wave exit portal
    inline float portalReach(Player const& p, World::PortalCluster const& c) {
        if (p.isDart && c.modeOut) {
            float const reach = g_waveExitReach.load(std::memory_order_relaxed);
            if (reach > 160.f) return reach;
        }
        return (p.isShip || p.isBird || p.isDart || p.isSwing) ? 160.f : 90.f;
    }

    bool clusterOwed(Game const& g, World::PortalCluster const& c);

    // returns true if a portal of this type gets the player out of its flying mode
    inline bool flyExitType(Player const& p, int type) {
        bool const ground = type == CubePortal || type == BallPortal || type == RobotPortal || type == SpiderPortal;
        if (p.isDart) return ground || type == ShipPortal || type == UfoPortal || type == SwingPortal;
        return ground;
    }

    int flyExitCost(Game const& g, Cues const& cues, SearchOptions const& opt);

    void openClusters(Game const& g, float x, Cues const& cues, SmallVec<std::pair<int, bool>, 8>& out);

    int prepaidAt(Game const& g, Cues const& cues, SearchOptions const& opt);

    int portalPrepay(Game const& g, float prevX, Cues const& cues, SearchOptions const& opt,
                     SmallVec<std::pair<int, bool>, 8> const& open);

    inline thread_local int g_portalMisses = 0;

    inline thread_local float g_lastMissX = 0;

    int missedPortalsCost(Game const& g, float prevX, Cues const& cues, SearchOptions const& opt, int& speedMisses,
                          bool outside = false);

    int flyExitMisses(Game const& g, float prevX, Cues const& cues, bool outside, bool waveOnly = false,
                      float maxReach = 1e30f);

    int missedOrbsCost(Game const& g, RingList const& prevRings1, RingList const& prevRings2, SearchOptions const& opt);

    struct Span {
        float x0 = 0;
        float x1 = 0;
        float y0 = 0;
        float y1 = 0;
        int tick = -1;
        int esc = 0;
        int escR = 0;

        // advances the escape counter (+2 out, -1 in), returns true past kill ticks
        bool escStep(bool out, int kill) {
            if (out)
                esc += 2;
            else if (esc > 0)
                --esc;
            return esc > 2 * kill;
        }

        // same counter for the roof
        void roofStep(bool roof) {
            if (roof)
                escR += 2;
            else if (escR > 0)
                --escR;
        }
        static constexpr int kTpEsc = 2;
        uint16_t tp1 = 0;
        uint16_t tp2 = 0;
        bool tpInit = false;
        int tpAt1 = -1000000;
        int tpAt2 = -1000000;

        // remembers the tick of the last teleport of each player
        void tpNote(Game const& g) {
            if (!tpInit) {
                tp1 = g.p1.tpCount;
                tp2 = g.p2.tpCount;
                tpInit = true;
                return;
            }
            if (g.p1.tpCount != tp1) {
                tp1 = g.p1.tpCount;
                tpAt1 = g.tick;
            }
            if (g.p2.tpCount != tp2) {
                tp2 = g.p2.tpCount;
                tpAt2 = g.tick;
            }
        }

        // returns true if the player was teleported a few ticks before t
        bool tpRecent(bool second, int t) const { return t - (second ? tpAt2 : tpAt1) <= kTpEsc; }
        static constexpr int kHeldMax = 28800;
        float px = 0;
        float py = 0;
        int held = 0;
        static constexpr int kTouchMax = 16;
        uint8_t touchResets = 0;
        bool touchGrace = false;
        uint64_t touchSig = 0;

        // a new set of active touch triggers restarts the stuck timer (at most kTouchMax times)
        void touchCheck(uint64_t sig, int t) {
            if (sig == touchSig) return;
            bool const first = touchSig == 0;
            touchSig = sig;
            if (!first && tick >= 0 && touchResets < kTouchMax) {
                ++touchResets;
                tick = t;
                touchGrace = true;
            }
        }
        static constexpr float kCell = 120.f;
        static constexpr int kRecent = 8;
        static constexpr float kCellBack = 3000.f;
        int16_t rcx[kRecent] = {};
        int16_t rcy[kRecent] = {};
        uint8_t rn = 0;
        uint8_t rpos = 0;

        // returns true if the cell of (x, y) is not one of the recent cells, and adds it
        bool newCell(float x, float y) {
            int16_t const cx = static_cast<int16_t>(std::clamp(std::floor(x / kCell), -32000.f, 32000.f));
            int16_t const cy = static_cast<int16_t>(std::clamp(std::floor(y / kCell), -32000.f, 32000.f));
            for (int i = 0; i < rn; ++i)
                if (rcx[i] == cx && rcy[i] == cy) return false;
            rcx[rpos] = cx;
            rcy[rpos] = cy;
            rpos = static_cast<uint8_t>((rpos + 1) % kRecent);
            if (rn < kRecent) ++rn;
            return true;
        }

        // advances one tick, returns true if the lineage hasn't grown its area (by margin) for kill ticks
        bool step(float x, float y, int t, int kill, float margin) {
            if (tick < 0) {
                x0 = x1 = x;
                y0 = y1 = y;
                tick = t;
                px = x;
                py = y;
                newCell(x, y);
                return false;
            }
            bool const still = x == px && y == py;
            px = x;
            py = y;
            if (still && held < kHeldMax) {
                ++held;
                tick = t;
                return false;
            }
            if (!still) held = 0;
            bool grew = false;
            if (x > x1 + margin) {
                x1 = x;
                grew = true;
            }
            if (x < x0 - margin) {
                x0 = x;
                grew = true;
            }
            if (y > y1 + margin) {
                y1 = y;
                grew = true;
            }
            if (y < y0 - margin) {
                y0 = y;
                grew = true;
            }
            if (x >= x1 - kCellBack && newCell(x, y)) grew = true;
            if (grew) {
                tick = t;
                touchGrace = false;
                touchResets = 0;
                return false;
            }
            return t - tick > (touchGrace ? kill * 5 / 2 : kill);
        }
    };

    struct Arena {
        std::deque<Game> slots;
        std::vector<Game*> freeList;
        std::vector<std::vector<Game*>> incoming;

        // gives a free slot, or a new one
        Game* alloc() {
            if (!freeList.empty()) {
                Game* g = freeList.back();
                freeList.pop_back();
                return g;
            }
            slots.emplace_back();
            return &slots.back();
        }

        // gives a game state back to the arena
        void release(Game* g) {
            if (g) freeList.push_back(g);
        }

        // slot given back by another worker, collected later
        void releaseFrom(int worker, Game* g) {
            if (g) incoming[static_cast<size_t>(worker)].push_back(g);
        }

        // moves the slots given back by other workers into the free list
        void collect() {
            for (auto& v : incoming) {
                freeList.insert(freeList.end(), v.begin(), v.end());
                v.clear();
            }
        }
    };

    struct Node {
        Game* g = nullptr;
        uint8_t owner = 0;
        int parent = -1;
        uint8_t action = 0;
        int cost = 0;
        int frag = 0;
        int debt = 0;
        int pdebt = 0;
        float debtX0 = 0;
        float debtX1 = 0;
        int outTicks = 0;
        int boundTicks = 0;
        int away = -1;
        Span span;
        bool outJust = false;
        uint8_t flyMiss = 0;
        float backX = -1e30f;
        uint64_t backSig = 0;
        uint64_t backSigP = 0;
        uint8_t backN = 0;
        int backTick0 = 0;
        int lastPress = -1000000;
        int lastRelease = -1000000;
        bool ringSincePress = false;
        Key key{};
        uint32_t cls = 0;
        uint32_t lin0 = 0;
        uint32_t lin1 = 0;
        bool dead = false;
        bool kept = false;
        uint8_t dbgHold = 0;
        bool trigHOk = false;
        uint32_t trigH = 0;
        int sameNext = -1;
        Node* sameNextP = nullptr;
    };

    uint64_t loopSig(Game const& g, bool withTriggers);

    // trigger state hash of a node, computed once
    inline uint32_t trigOf(Node& n) {
        if (!n.trigHOk) {
            n.trigH = n.g->trig.on ? trigHashMain(*n.g) : 0u;
            n.trigHOk = true;
        }
        return n.trigH;
    }

    // returns true if touching this trigger changes something for the path (not a decoration only toggle or move)
    inline bool touchMatters(World const& w, Obj const& o) {
        int const group = o.targetGroup;
        switch (o.id) {
        case 1049:
            return group > 0 && group < static_cast<int>(w.groupObjs.size()) &&
                   !w.groupObjs[static_cast<size_t>(group)].empty();
        case 901:
        case 1346:
        case 1347:
        case 1814:
            return w.relevantGroup(group);
        default:
            return true;
        }
    }

    void offAheadOf(World const& w, int group, bool viaToggle, float x0, int depth, std::vector<char>& seenGroups,
                    int& n, float& lo, float& hi);

    bool turnsOnWallFarAhead(World const& w, Obj const& o);

    bool isTrapTouchItself(World const& w, Obj const& o);

    std::vector<Rect> trapRectsOf(World const& w);

    bool isTrapTouch(World const& w, Obj const& o);

    int trapsTouchedNear(Game const& g, Cues const& cues, float x);

    bool ringEffectMatters(World const& w, int group, int depth = 0);

    struct CauseIndex {
        World const* w = nullptr;
        std::vector<std::vector<int>> byTarget;

        // builds it once per world
        void build(World const& world) {
            if (w == &world) return;
            w = &world;
            byTarget.assign(10000, {});
            for (size_t i = 0; i < world.objs.size(); ++i) {
                Obj const& o = world.objs[i];
                if ((o.cls == 1 || o.id == ObjId::ToggleOrb || o.id == ObjId::ToggleBlock) && o.targetGroup > 0 &&
                    o.targetGroup < 10000)
                    byTarget[static_cast<size_t>(o.targetGroup)].push_back(static_cast<int>(i));
            }
        }
    };

    struct CauseTake {
        short group;
        std::vector<int> orbs;
    };

    struct CauseResult {
        std::vector<CauseTake> take;
        std::vector<int> avoid;
    };

    // returns true if the object can turn on its target group
    inline bool causeActivates(Obj const& a) {
        switch (a.id) {
        case 1049:
        case 1611:
        case 1811:
            return a.tf(Obj::TF_ActivateGroup);
        case 1594:
        case 3643:
            return !a.ringSpawnOnly && a.tf(Obj::TF_ActivateGroup);
        case 1595:
            return a.touchToggleMode != 2;
        default:
            return false;
        }
    }

    // returns true if the object can turn off its target group
    inline bool causeDeactivates(Obj const& a) {
        switch (a.id) {
        case 1049:
        case 1611:
        case 1811:
            return !a.tf(Obj::TF_ActivateGroup);
        case 1594:
        case 3643:
            return !a.ringSpawnOnly && !a.tf(Obj::TF_ActivateGroup);
        case 1595:
            return a.touchToggleMode != 1;
        default:
            return false;
        }
    }

    // returns true if the object can start the triggers of its target group
    inline bool causeSpawns(Obj const& a) {
        return a.id == ObjId::SpawnTrigger ||
               ((a.id == ObjId::ToggleOrb || a.id == ObjId::ToggleBlock) && a.ringSpawnOnly) || causeActivates(a);
    }

    struct CauseWalk {
        World const& w;
        Game const& bg;
        CauseIndex const& ix;
        float fx;
        CauseResult& out;
        std::vector<int> seen;

        // true if the trigger was activated in the base game
        bool fired(int i) const { return trigActivatedBit(bg, i); }

        // true if the object is behind the dead end
        bool passed(Obj const& o) const { return o.x < fx; }

        // returns false if already visited
        bool visit(int i) {
            if (std::find(seen.begin(), seen.end(), i) != seen.end()) return false;
            seen.push_back(i);
            return true;
        }

        // avoids the passed toggle orbs that would turn this group on
        void avoidOrbsEnabling(int group) {
            for (int a : ix.byTarget[static_cast<size_t>(group)]) {
                Obj const& o = w.objs[static_cast<size_t>(a)];
                if ((o.id == ObjId::ToggleOrb || o.id == ObjId::ToggleBlock) && o.x < fx && causeActivates(o) &&
                    std::find(out.avoid.begin(), out.avoid.end(), a) == out.avoid.end())
                    out.avoid.push_back(a);
            }
        }

        // trigger that shouldn't have fired: turn its groups off, or don't spawn it
        void block(int trigger, int depth) {
            if (depth > 5 || !visit(trigger)) return;
            Obj const& o = w.objs[static_cast<size_t>(trigger)];
            for (short group : o.groups) {
                if (group <= 0) continue;
                avoidOrbsEnabling(group);
                if (!trigGroupOn(bg, group)) continue;
                std::vector<int> take;
                for (int a : ix.byTarget[static_cast<size_t>(group)]) {
                    Obj const& t = w.objs[static_cast<size_t>(a)];
                    if ((t.id == ObjId::ToggleOrb || t.id == ObjId::ToggleBlock) && t.x < fx && causeDeactivates(t))
                        take.push_back(a);
                }
                if (!take.empty()) out.take.push_back(CauseTake{group, std::move(take)});
            }
            if (o.spawnTriggered)
                for (short group : o.groups) {
                    if (group <= 0) continue;
                    for (int s : ix.byTarget[static_cast<size_t>(group)]) {
                        Obj const& t = w.objs[static_cast<size_t>(s)];
                        if (!causeSpawns(t)) continue;
                        if (t.id == ObjId::ToggleOrb || t.id == ObjId::ToggleBlock) {
                            if (std::find(out.avoid.begin(), out.avoid.end(), s) == out.avoid.end() && t.x < fx)
                                out.avoid.push_back(s);
                        } else if (fired(s))
                            block(s, depth + 1);
                    }
                }
        }

        // trigger that should have fired: who spawns it and why not, or who turned its group off
        void whyNot(int trigger, int depth) {
            if (depth > 5 || !visit(trigger)) return;
            Obj const& o = w.objs[static_cast<size_t>(trigger)];
            if (o.spawnTriggered) {
                for (short group : o.groups) {
                    if (group <= 0) continue;
                    for (int s : ix.byTarget[static_cast<size_t>(group)]) {
                        Obj const& a = w.objs[static_cast<size_t>(s)];
                        if (!causeSpawns(a) || a.id == ObjId::ToggleOrb || a.id == ObjId::ToggleBlock) continue;
                        if (!fired(s)) whyNot(s, depth + 1);
                    }
                }
                return;
            }
            if (o.touchTriggered || !passed(o) || fired(trigger)) return;
            for (short group : o.groups) {
                if (group <= 0 || trigGroupOn(bg, group)) continue;
                for (int a : ix.byTarget[static_cast<size_t>(group)]) {
                    Obj const& d = w.objs[static_cast<size_t>(a)];
                    if (d.id == ObjId::ToggleOrb || d.id == ObjId::ToggleBlock || !causeDeactivates(d)) continue;
                    block(a, depth + 1);
                }
            }
        }
    };

    CauseResult impasseCauses(World const& w, Game const& bg, CauseIndex& ix, std::vector<short> const& killerGroups,
                              float fx);

    struct ItemTarget {
        int idx = -1;
        int item = 0;
        int count = 0;
        int mode = 0;
        short group = 0;
        float x = 0;

        // how far value v is from meeting the condition (0 when met)
        int dist(int v) const {
            switch (mode) {
            case 1:
                return std::max(0, count + 1 - v);
            case 2:
                return std::max(0, v - (count - 1));
            case 3:
                return count >= 0 ? std::max(0, count - v) : std::max(0, v - count);
            default:
                return std::abs(v - count);
            }
        }
    };

    std::vector<ItemTarget> itemTargetsFor(World const& w, Game const& bg, CauseIndex& ix,
                                           std::vector<short> const& killerGroups, float fx);

    // returns true if both nodes have the same trigger state
    inline bool sameTrig(Node& a, Node& b) {
        if (trigOf(a) != trigOf(b)) return false;
        if (a.g->trig.on && b.g->trig.on && !trigPvEqual(a.g->trig, b.g->trig)) return false;
        TrigDyn const* dynA = a.g->trig.on ? a.g->trig.d.get() : nullptr;
        TrigDyn const* dynB = b.g->trig.on ? b.g->trig.d.get() : nullptr;
        if (dynA == dynB) return true;
        if (!dynA || !dynB) return false;
        return trigDynEqual(*dynA, *dynB);
    }

    inline bool g_debtFirst = true;

    // returns true if a is better than b (debt, cost, fragility, then a stable tie break)
    inline bool nodeLess(Node const& a, Node const& b) {
        if (g_debtFirst && a.debt != b.debt) return a.debt < b.debt;
        if (a.cost != b.cost) return a.cost < b.cost;
        if (a.frag != b.frag) return a.frag < b.frag;
        if (a.lastPress != b.lastPress) return a.lastPress < b.lastPress;
        if (a.key.x != b.key.x) return a.key.x < b.key.x;
        if (a.key.y != b.key.y) return a.key.y < b.key.y;
        if (a.key.v != b.key.v) return a.key.v < b.key.v;
        if (a.key.flags != b.key.flags) return a.key.flags < b.key.flags;
        if (a.key.extra != b.key.extra) return a.key.extra < b.key.extra;
        if (a.parent != b.parent) return a.parent < b.parent;
        return a.action < b.action;
    }

    // class of a state for the beam quotas: mode, gravity, size, dual, y/10, x/60, y velocity
    inline uint32_t classOfState(Key const& k, Game const& g) {
        uint32_t const flags = k.flags;
        float const vAxis = g.p1.isSideways ? g.p1.x : g.p1.y;
        float const fAxis = g.p1.isSideways ? g.p1.y : g.p1.x;
        uint32_t const yBits =
                static_cast<uint32_t>(std::max(0, static_cast<int>(std::floor(vAxis / 10.f)) + 64)) & 0x3ffu;
        uint32_t const xBits = static_cast<uint32_t>(std::max(0, static_cast<int>(std::floor(fAxis / 60.f)))) & 0x3ffu;
        uint32_t const vBits =
                static_cast<uint32_t>(std::clamp(static_cast<int>(std::floor(g.p1.yVelocity / 2.0)) + 16, 0, 31));
        return (flags & 0xffu) | (((flags >> 16) & 1u) << 8) | (((flags >> 21) & 1u) << 9) | (yBits << 10) |
               (xBits << 20) | ((vBits & 31u) << 27);
    }

    class CountTable {
    public:
        // empties the table (new generation)
        void clear() {
            ++m_gen;
            if (m_gen == 0) {
                for (auto& e : m_slots) e.gen = 0;
                m_gen = 1;
            }
            m_count = 0;
        }

        // counter of k, inserted at 0 if missing
        int& operator[](uint32_t k) {
            if ((m_count + 1) * 2 > m_slots.size()) grow();
            size_t const mask = m_slots.size() - 1;
            size_t i = static_cast<size_t>(KeyHash::splitmix(k)) & mask;
            for (;;) {
                Slot& s = m_slots[i];
                if (s.gen != m_gen) {
                    s.gen = m_gen;
                    s.key = k;
                    s.value = 0;
                    ++m_count;
                    return s.value;
                }
                if (s.key == k) return s.value;
                i = (i + 1) & mask;
            }
        }

        // number of keys in the table
        size_t size() const { return m_count; }

    private:
        struct Slot {
            uint32_t key = 0;
            int value = 0;
            uint32_t gen = 0;
        };
        std::vector<Slot> m_slots = std::vector<Slot>(256);
        uint32_t m_gen = 1;
        size_t m_count = 0;

        // doubles the size and puts back the current keys
        void grow() {
            std::vector<Slot> old = std::move(m_slots);
            m_slots.assign(old.size() * 2, Slot{});
            m_count = 0;
            for (auto const& e : old)
                if (e.gen == m_gen) (*this)[e.key] = e.value;
        }
    };

    struct Checkpoint {
        Game g;
        int tick = 0;
        size_t inputsLen = 0;
        Span span{};
    };

    // one busy wait step (x86 pause instruction)
    inline void cpuRelax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        _mm_pause();
#endif
    }

    constexpr long kSpinUs = 2000;

    class Pool {
    public:
        // starts the worker threads
        explicit Pool(int threads, std::function<void()> init = {})
            : m_T(std::max(1, threads)), m_init(std::move(init)), m_cvs(static_cast<size_t>(std::max(1, threads))) {
            for (int worker = 1; worker < m_T; ++worker) m_workers.emplace_back([this, worker] { loop(worker); });
        }

        // stops and joins all the workers
        ~Pool() {
            {
                std::lock_guard<std::mutex> lock(m_mx);
                m_quit = true;
                m_gen.fetch_add(1);
            }
            for (auto& cv : m_cvs) cv.notify_all();
            for (auto& t : m_workers) t.join();
        }

        double waitSeconds = 0;
        long slowWaits = 0;
        long lateStarts = 0;

        // runs job(worker) on workers 0 to use - 1, the job must take its work by stealing chunks
        void run(std::function<void(int)> const& job, int use) {
            use = std::max(1, std::min(use, m_T));
            if (use == 1) {
                job(0);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(m_mx);
                m_job = &job;
                m_use = use;
                m_gen.fetch_add(1);
                m_open = true;
            }
            for (int worker = 1; worker < use; ++worker) m_cvs[static_cast<size_t>(worker)].notify_one();
            job(0);
            auto const waitStart = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(m_mx);
                m_open = false;
            }
            for (uint32_t spin = 0; m_active.load(std::memory_order_acquire) != 0; ++spin) {
                cpuRelax();
                if ((spin & 63u) == 63u &&
                    std::chrono::steady_clock::now() - waitStart > std::chrono::microseconds(kSpinUs))
                    break;
            }
            if (m_active.load(std::memory_order_acquire) != 0) {
                ++slowWaits;
                std::unique_lock<std::mutex> lock(m_mx);
                m_done.wait(lock, [this] { return m_active.load(std::memory_order_acquire) == 0; });
            }
            waitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - waitStart).count();
            std::lock_guard<std::mutex> lock(m_mx);
            m_job = nullptr;
        }

        // runs the job on every worker
        void run(std::function<void(int)> const& job) { run(job, m_T); }

    private:
        // worker loop, until the pool is destroyed
        void loop(int worker) {
            if (m_init) m_init();
            uint64_t seen = 0;
            bool spinNext = false;
            while (true) {
                bool got = false;
                if (spinNext) {
                    auto const spinStart = std::chrono::steady_clock::now();
                    for (uint32_t spin = 0;; ++spin) {
                        if (m_gen.load(std::memory_order_acquire) != seen) {
                            got = true;
                            break;
                        }
                        cpuRelax();
                        if ((spin & 63u) == 63u &&
                            std::chrono::steady_clock::now() - spinStart > std::chrono::microseconds(kSpinUs))
                            break;
                    }
                }
                std::function<void(int)> const* job = nullptr;
                {
                    std::unique_lock<std::mutex> lock(m_mx);
                    if (!got) {
                        ++lateStarts;
                        m_cvs[static_cast<size_t>(worker)].wait(lock, [&] {
                            return m_quit || (m_gen.load(std::memory_order_acquire) != seen && worker < m_use);
                        });
                    }
                    if (m_quit) return;
                    uint64_t const gen = m_gen.load(std::memory_order_acquire);
                    seen = gen;
                    if (m_open && worker < m_use && m_job) {
                        job = m_job;
                        m_active.fetch_add(1, std::memory_order_acq_rel);
                    }
                }
                spinNext = job != nullptr;
                if (!job) continue;
                (*job)(worker);
                {
                    std::lock_guard<std::mutex> lock(m_mx);
                    if (m_active.fetch_sub(1, std::memory_order_acq_rel) == 1) m_done.notify_one();
                }
            }
        }
        int m_T;
        std::function<void()> m_init;
        std::vector<std::thread> m_workers;
        std::mutex m_mx;
        std::vector<std::condition_variable> m_cvs;
        std::condition_variable m_done;
        std::function<void(int)> const* m_job = nullptr;
        std::atomic<int> m_active{0};
        std::atomic<uint64_t> m_gen{0};
        int m_use = 1;
        bool m_open = false;
        bool m_quit = false;
    };

    struct SoftState {
        Game g;
        std::vector<uint8_t> tail;
        int debt = 0;
        int pdebt = 0;
        int away = -1;
        Span span{};
    };

    struct Soft {
        int tick = 0;
        std::vector<SoftState> states;
    };

    constexpr size_t kSoftStates = 96;

    constexpr int kLineagePeriod = 120;

    constexpr int kSoftRoom = 240;

    struct Rung {
        int back;
        int mul;
        int q;
        int gapDiv;
        bool anchor;
    };

    constexpr Rung kLadder[] = {{240, 4, 0, 1, false},  {720, 4, 0, 2, true},   {720, 4, 0, 2, false},
                                {240, 16, 0, 2, false}, {1440, 16, 0, 1, true}, {1440, 16, 0, 4, false},
                                {240, 16, 1, 4, false}, {240, 16, 2, 4, false}, {2400, 16, 1, 1, false}};

    constexpr int kNLadder = static_cast<int>(sizeof(kLadder) / sizeof(kLadder[0]));

    struct Tracker {
        bool on = false;
        std::map<int, Game> snaps;
        bool alive = true;
        int rankPre = -1;
        int costPre = 0;
        int nPre = 0;

        // replays the macro from start and keeps its states between the from and to ticks
        void init(Game const& start) {
            char const* path = debugOptions().track;
            if (!path) return;
            FILE* file = std::fopen(path, "r");
            if (!file) return;
            std::vector<uint8_t> macro;
            int ch;
            while ((ch = std::fgetc(file)) != EOF)
                if (ch >= '0' && ch <= '3') macro.push_back(static_cast<uint8_t>(ch - '0'));
            std::fclose(file);
            int const from = debugOptions().trackFrom >= 0 ? debugOptions().trackFrom : start.tick;
            int const to = debugOptions().trackTo >= 0 ? debugOptions().trackTo : start.tick + 3000;
            Game g = start;
            for (int t = start.tick; t <= to && t < static_cast<int>(macro.size()) && !g.died; ++t) {
                if (t >= from) snaps[t] = g;
                stepTick(g, macro[static_cast<size_t>(t)]);
            }
            on = true;
            std::fprintf(stderr, "[track] %zu states of the tracked path (ticks %d..%d)\n", snaps.size(), from, to);
        }

        // tracked state at tick t, or nullptr
        Game const* at(int t) const {
            auto it = snaps.find(t);
            return it == snaps.end() ? nullptr : &it->second;
        }
    };

    int menuStartValue(World const& w, int item);

    enum class Flow { Next, Continue, Break, Return };

    // One run of searchPath. The search keeps a lot of state across ticks (beam width, kill zones, restart points...),
    // so its locals are members here and each stage of a tick is a member function.
    struct SearchRun {
        SearchRun(Game const& start, SearchOptions const& opt, std::atomic<bool> const* cancel,
                  std::function<void(int, int)> const& progress)
            : start(start), opt(opt), cancel(cancel), progress(progress) {}

        // arguments of searchPath
        Game const& start;
        SearchOptions const& opt;
        std::atomic<bool> const* cancel;
        std::function<void(int, int)> const& progress;
        // what run() returns, set by the stage that ends the search
        SearchResult finalResult;

        std::chrono::steady_clock::time_point startTime;
        SearchResult res;
        int endTick;
        Cues cues;
        std::vector<float> dynSpeedX;
        Tracker track;
        uint64_t tieSalt;
        struct CountTarget {
            int item;
            int count;
            float x;
            bool instant;
        };
        std::vector<CountTarget> countTargets;
        struct ClickZone {
            float x0;
            float x1;
            int item;
            int count;
        };
        std::vector<ClickZone> clickZones;
        float itemRewardUntilX;
        struct PaceItem {
            int item;
            std::vector<std::pair<float, int>> th;
        };
        std::vector<PaceItem> paceItems;
        int escapeSide(Game const& g, Span const& span);
        bool escapedNow(Game const& g, Span const& span);
        void spanReplay(Span& span, Game const& g);
        std::vector<uint8_t> inputs;
        std::vector<Checkpoint> hard;
        std::deque<Soft> soft;
        std::vector<std::vector<uint8_t>> rootTails;
        std::vector<Game> rootGamesDbg;
        float lastLiveX;
        int rootTick;
        static size_t commonPrefixLen(std::vector<uint8_t> const& a, std::vector<uint8_t> const& b);
        int beam;
        int widenedAtTick;
        int gapNow;
        Rung const* ladder;
        int gapForRung(int rung);
        int ladderRoot;
        int capLiftedAt;
        int lastReducedTick;
        int noReduceUntil;
        std::vector<int> nominalSpots;
        std::vector<int> liveRetrySpots;
        bool dbgHoldTrack;
        std::vector<std::vector<int>> killersOf;
        static constexpr int kAwayKiller = -7;
        std::vector<std::vector<float>> awayKillsOf;
        static constexpr int kStuckKiller = -9;
        std::vector<std::vector<float>> stuckKillsOf;
        static constexpr int kEscapeKiller = -10;
        static constexpr int kMenuKiller = -12;
        static constexpr int kFlyKiller = -13;
        static constexpr int kTrapKiller = -14;
        std::vector<std::vector<float>> escapeKillsOf;
        std::vector<std::pair<float, float>> awayZones;
        std::vector<std::pair<float, float>> flyOkZones;
        int lastFlyKillTick;
        float lastFlyKillX;
        struct MustTake {
            int cluster;
            float passX;
            float x1;
        };
        std::vector<MustTake> mustTakes;
        std::vector<float> flyTryFails;
        bool flyTriesExhausted(float x);
        static constexpr int kMustKiller = -16;
        std::vector<std::pair<float, float>> escOkZones;
        int lastEscKillTick;
        float lastEscKillX;
        int lastAwayKillTick;
        float lastAwayKillX;
        static constexpr int kRevisitKiller = -8;
        static constexpr float kRevisitJump = 900.f;
        static constexpr float kRevisitBin = 32.f;
        static constexpr float kRevisitDy = 150.f;
        static constexpr int kRevisitAge = 600;
        struct Visit {
            float y;
            int tick;
        };
        std::vector<std::vector<Visit>> visitBins;
        std::vector<int> visitOrder;
        std::vector<std::vector<float>> revisitKillsOf;
        std::vector<std::pair<float, float>> revisitOk;
        int lastRevisitKillTick;
        float lastRevisitX;
        bool revisitOn;
        int visitBinOf(float x);
        void visitPurge(int fromTick);
        void visitAddXY(float x, float y, int atTick);
        void visitAdd(Game const& g);
        bool revisited(Game const& g);
        std::vector<std::vector<int>> loopStartsOf;
        struct LoopZone {
            float x0;
            float x1;
            float y;
        };
        std::vector<std::vector<LoopZone>> loopZonesOf;
        std::vector<LoopZone> loopZones;
        struct TrapCorr {
            float x0 = 0;
            float xStart = 0;
            std::vector<std::array<float, 2>> band;
            std::vector<uint8_t> modes;
            int startTick = 0;
            bool off = false;
        };
        struct DeadPortal {
            int uid = 0;
            int idx = -1;
            float x = 0;
            int tick = 0;
            bool off = false;
            int offAt = 0;
        };
        std::vector<DeadPortal> deadPortals;
        int deadLearns;
        static constexpr int kMaxDeadLearns = 32;
        static constexpr int kDeadPortalKiller = -17;
        std::vector<TrapCorr> trapCorr;
        static constexpr float kTrapFloorChange = 250.f;
        static constexpr float kTrapBand = 150.f;
        int popBeforeTick;
        int trapSoftFrom;
        std::vector<std::pair<float, float>> trapWin;
        std::vector<int> trapLearned;
        CauseIndex causeIx;
        std::vector<int> avoidUids;
        std::vector<short> needOff;
        std::vector<ItemTarget> itemTargets;
        struct GroupGoal {
            short group;
            float checkX;
            std::vector<int> acts;
        };
        std::vector<GroupGoal> groupGoals;
        std::vector<std::pair<float, float>> fineZones;
        float qY;
        float qV;
        struct Passed {
            float x;
            int beam;
            float qY;
            float qV;
            int gap;
        };
        std::vector<Passed> passed;
        float spotX;
        float holdX;
        int spotTick;
        static constexpr float kZoneGap = 2000.f;
        static constexpr float kZoneLen = 2000.f;
        float zoneX0;
        float zoneX1;
        Passed zoneSet;
        void applyZone(float x);
        void applyPassed(float rootX);
        int nThreads;
        std::optional<Pool> pool;
        std::vector<std::vector<Node>> curBuckets;
        std::vector<std::vector<Node>> nextBuckets;
        std::vector<Arena> arenas;
        std::vector<Node*> live;
        std::vector<Node*> liveNext;
        void releaseUnkept(std::vector<std::vector<Node>>& buckets);
        std::vector<KeyTable<int>> localSeen;
        KeyTable<Node*> seen;
        std::vector<std::pair<uint64_t, Node*>> sortBuf;
        std::vector<uint64_t> cellsBuf;
        int adaptWidenSeen;
        std::vector<std::pair<float, float>> noAdaptZones;
        std::vector<double> busy;
        std::vector<long> nodesDone;
        double nodeCostUs;
        double busyPrev;
        long nodesPrev;
        std::vector<TrigLocalCtx> localCtx;
        std::vector<std::vector<size_t>> part;
        std::vector<std::atomic<size_t>> partCursor;
        std::atomic<long> simulated{0};
        long debtTickMark;
        std::vector<std::vector<std::pair<int32_t, uint8_t>>> history;
        struct Root {
            Game const* g;
            std::vector<uint8_t> const* tail;
            int debt;
            int pdebt = 0;
            int away = -1;
            Span span{};
        };
        void restartFromMany(std::vector<Root> const& roots, int atTick);
        void restartFrom(Game const& g, int atTick, std::vector<uint8_t> tail, Span span = Span{});
        void restartFromSoft(Soft const& softPoint);
        int tick;
        void capFar(int restartTick);
        static constexpr int kFixedAhead = 240;
        static constexpr int kFreeOutKill = 120;
        int retractUnplayed(int keep, int played);
        std::vector<uint8_t> tailTo(int idx);
        std::vector<uint8_t> pathTo(int idx);
        int ancestorAt(int idx, size_t layer);
        std::vector<uint8_t> furthest;
        SearchResult finish(int bestIdx, bool success, std::string why);
        int bestOf(bool onlyCompleted);
        static int chkLevel();
        bool publish(size_t cutDepth);
        int lastPublishCheck;
        int lastSoft;
        int noSoftBefore;
        int impasseRecord;
        int popStreak;
        bool fromStart;
        int earlierFromStart;
        std::vector<std::pair<int, int>> spotRungs;
        int prevMinDebt;
        int prevMinSDebt;
        std::vector<std::pair<int, int>> debtSpots;
        std::vector<std::pair<int, int>> pdebtSpots;
        struct DebtTry {
            int spot;
            int from;
            int beam;
            int q;
        };
        std::vector<DebtTry> debtTried;
        int lastPopRestartTick;
        int lastLostSeen;
        CountTable classCount;
        CountTable lineageCount;
        CountTable keyCount;
        CountTable coarseCount;
        std::vector<Node*> variantsBuf;
        std::vector<float> divDist;
        struct DivF {
            float v;
            float vy;
            uint32_t mode;
        };
        std::vector<DivF> divF;
        std::vector<int32_t> convA;
        std::vector<int32_t> convB;
        int convergenceDepth();
        int recordTick;
        std::chrono::steady_clock::time_point recordTime;
        void livePublish();
        bool anyCompleted;
        size_t nLive;
        float layerQY;
        float layerQV;
        bool fineNow;
        bool lineageBoundary;
        size_t chunk;
        void expand(int worker);
        // what expandNode needs from the worker running it (references to the locals of expand)
        struct ExpandWorker {
            int& worker;
            std::vector<Node>& out;
            KeyTable<int>& seenLocal;
            long& nSteps;
            Arena& arena;
        };
        // what the children of one state need from it, taken before the first child is stepped: the last child
        // steps the parent's own game in place to save a copy
        struct ParentInfo {
            size_t const& i;
            Node& parent;
            Game* const& parentGame;
            bool const& holdingBefore;
            bool const& holding2Before;
            int const& nCost;
            int const& nFrag;
            int const& nLastPress;
            int const& nLastRelease;
            int const& nDebt;
            int const& nPdebt;
            int const& nOut;
            int const& nBound;
            int const& nAway;
            uint8_t const& nFlyMiss;
            char const& parentMode;
            float const& nDebtX0;
            float const& nDebtX1;
            float const& nBackX;
            uint64_t const& nBackSig;
            uint64_t const& nBackSigP;
            uint8_t const& nBackN;
            int const& nBackTick0;
            Span const& nSpan;
            uint8_t const& parentOwner;
            float const& prevX;
            uint16_t const& prevTp1;
            uint16_t const& prevTp2;
            unsigned int const& prevItemChanges;
            bool const& persistWatch;
            int const& prevPersist;
            int const& prevTraps;
            int (&itemV0)[4];
            int (&paceV0)[8];
            std::vector<int>& countV0;
            uint32_t& needMask;
            bool const& prevObbFragile;
            float const& prevHaz;
            float const& prevSpeed;
            SmallVec<std::pair<int, bool>, 8>& openPortals;
            bool const& prevRotAmb;
            RingList const& prevRings1;
            RingList& prevRings2;
            bool const& nRing;
            uint8_t (&acts)[4];
            int& nActs;
            bool const& segMode;
            int const& minSeg;
            unsigned int& relItemDelta;
            uint32_t& deadTakenP;
        };
        // expands one live state (all its inputs)
        void expandNode(ExpandWorker& wk, size_t i);
        // steps and scores the child of one input of the state
        void expandAction(ExpandWorker& wk, ParentInfo& pn, int ai);
        // state of one child while expandAction checks and scores it (shared by its stages)
        struct ChildStep {
            uint8_t act;
            bool hold;
            bool isPress;
            Game* game;
            bool inEndZone;
            int cAway;
            Span cSpan;
            int escCostNow;
            uint8_t cFlyMiss;
            int flyMissPenalty;
            float cBackX;
            uint64_t cBackSig;
            uint64_t cBackSigP;
            uint8_t cBackN;
            int cBackTick0;
            uint8_t cOwner;
            Node* child;
        };
        // steps the child: the last input reuses the parent's game, the others a copy from the worker's arena
        void stepChild(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        // kills the child where the search learned it must not go: doomed void, dead portals, avoided objects,
        // loop zones, trap corridors and jumps back into the part already played
        void applyLearnedKills(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        // counts the ticks the child spends away from every object of the level, and kills it after too many
        void checkPresence(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        // stuck and must-take rules, menus and the menu goal
        void checkStuckAndMenus(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        // escape rules: the child left the level's structure above or below (cost, then kill)
        void checkEscape(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        // flying modes: counts the portal exits the child flew past, cost then kill
        void checkFlyMiss(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        // kills the child when it passes an item target or a group goal that is not done
        void checkGoals(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        // a child sent back more than 300 units: kills it when it loops on the same spot and trigger state
        void checkLoopBack(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        // free modes: kills a wave that stays outside the level's extent
        void checkExtent(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        // a dead child is only counted (its killer is kept for the back off)
        bool dropDead(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        // creates the node of the child with the costs inherited from its parent
        void makeChild(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        // costs of the inputs (preferFewInputs): presses, missed groups, orbs and pads
        void addInputCosts(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        // press times, key, class and lineage of the child, then merges it with an equal child of this worker
        void finishChild(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c);
        size_t nNodes;
        std::chrono::steady_clock::time_point expandStart;
        int perThread;
        int nWorkers;
        std::chrono::steady_clock::time_point expandEnd;
        float frontX;
        int narrowAfter;
        int beamEff;
        void makeSoft();
        int leadBase();
        // starts the clock and the result, and lists the speed portals that move triggers bring from afar
        // (the search uses a finer grid near them)
        void beginRun();
        // level maps that the costs read: portal clusters, objects to touch, structure, void and end zone
        void buildCues();
        // counters the level waits for: count triggers, the clicks owed to event triggers, the pace of the items
        void findItemGoals();
        // the rest of the state: checkpoints, beam ladder, kill and revisit tables, worker pool and buffers
        void initState();
        // roots the search at the start state
        void startFromLevel();
        // stops when the caller cancels; in live mode, also takes back the unplayed path when the player loses an
        // attempt
        Flow checkCancelled();
        // stops when no tick record was beaten for stallSeconds, or when the tick or time budget is used up
        Flow checkBudget();
        // stops when a live state finished the level (the lowest portal debt first, when debts count) or at the
        // horizon
        Flow checkCompleted();
        // setup of the tick: front x, forced exits already passed, finer grid near moved speed portals and in the fine
        // zones, and the split of the live states among the workers
        void prepareLayer();
        // expands every live state by its inputs on the workers
        void expandLayer();
        // adds the kills the workers counted to the stats and remembers where the last kill of each kind happened
        void countKills();
        // puts the children of all workers in the next layer; children with the same key and trigger state are merged
        // into the cheapest one
        void mergeChildren();
        // sorts the next layer by portal debt, cost, fragility, last press and a salted hash
        void sortChildren();
        // when the lowest portal debt of the layer rises, a portal was just missed: restarts from before it, trying a
        // forced flying exit or a must-take orb first
        Flow checkDebtRise();
        // a dead end inside the zone of a forced exit lifts that exit
        void liftMustTakes();
        // every state died: learns why (kills, traps, missed portals, items) and restarts wider or from an earlier
        // point
        Flow backOff();
        // back to the normal beam, grid and press gap once the front passed the spot that needed them
        void narrowBack();
        // beam width of this tick: smaller when far ahead of the player in live mode, adapted to recent widenings
        void chooseWidth();
        // keeps the best beam-width states, with quotas per state class and lineage so the layer stays varied
        void pruneLayer();
        // debug (APSIM_TRACK): reports where the tracked macro stands in the layer and why it was dropped
        void traceTracked();
        // frees the dropped states, records the layer in the history and moves to the next tick
        void advanceTick();
        // debug (APSIM_CHECK >= 2): replays the best path from the start and compares it with its state
        void checkReplay();
        // saves a soft restart point when the layer narrowed to one state
        void keepSoftPoint();
        // publishes the part of the path all states agree on, and in live mode the path to play
        void publishPrefix();
        // live mode: pauses while the search is too far ahead of the player
        void waitForPlayer();

        // state and stages of backOff()
        bool loopImpasse;
        bool trapImpasse;
        Rung const* kRungs;
        static constexpr int kNRungs = kNLadder;
        int* rungOf;
        bool anchored;
        Game anchorGame;
        std::vector<uint8_t> anchorTail;
        int anchorTick;
        Span anchorSpan;
        // the layer died soon after some kills: lets the next try through the places of those kills (revisits,
        // flying exits, escapes, away zones)
        void allowKilledZones();
        // states sent back by a loop trigger: remembers the zones of those loops
        void findLoopImpasse();
        // counts what killed the states; when traps did, learns where the trap floor changes
        void findTrapImpasse();
        // keeps the furthest path; in live mode, a dead end just after the beam was reduced retries at the nominal
        // beam first
        Flow retryNominalBeam();
        // after a trap impasse, restarts from before the trap
        Flow restartBeforeTrap();
        // when the killers are moved by count triggers whose count is not reached, learns those counters as goals
        Flow learnCounterGoals();
        // learns the items whose triggers move the killers (up to 4) as targets for the next tries
        Flow learnItemTargets();
        // remembers the toggle orbs and blocks just behind the front that change the killers
        Flow learnRingGoals();
        // finds the widening rung of this spot (each new dead end at a spot climbs one rung)
        void pickRung();
        // on the 4th rung, learns the portals just behind the front as places to try
        void learnPortalGoals();
        // live mode: finds a restart point that is still ahead of the player
        void findLiveAnchor();
        // live mode: all rungs used; retries once from the point the player has reached
        void retryFromLive();
        // applies the rung (wider beam, finer grid, smaller press gap) and restarts from its restart point
        Flow widenAndRestart();
        // moves the search back to the restart tick
        Flow restartTick();

        // state and stages of checkDebtRise()
        int minDebt;
        int minSDebt;
        bool portalOnly;
        bool restarted;
        bool flyExitTry;
        bool mtTry;
        // lowest portal debt of the next layer, with and without the part owed to plain portals
        void measureDebt();
        // the lowest debt rose since the last tick: a portal was missed, so tries again from before it
        void onDebtRise();
        // remembers the debt levels for the next tick (unless the search restarted)
        Flow keepDebtLevel();

        // state and stages of onDebtRise()
        bool missed;
        bool nominal;
        // checks that a state of the old lowest debt is the parent of a state whose debt rose
        void findMissedPortal();
        // the miss is a flying exit: retries it as a forced exit, once per spot (flyExitTries)
        void tryFlyExit();
        // in live mode, a miss right after a beam reduction is retried at the nominal beam
        void checkNominalBeam();
        // restarts from before the missed portal, wider each time the same spot fails
        void restartBeforeMiss();

        // state and stages of restartBeforeMiss()
        int* debtRungOf;
        int effFrom(int back);
        int effBeam(Rung const& rung, int from);
        bool skipRung(int k);
        bool touchOnlyDebt;
        bool flyDebt;
        // the rung reached at this spot, counted apart for misses of plain portals
        void findDebtRung();
        // skips the rungs that would not change anything, and accepts the debt without a try when only touch
        // triggers were missed or the flying exit tries already failed here
        void skipDebtRungs();
        // live mode: no try for a plain portal miss when the search is short on time or close to the player
        void limitLiveDebtTries();
        // on the first rung, a missed flying exit becomes a forced exit (must take) for the try
        void forceFlyExit();
        // climbs one rung and restarts from its restart point with its beam
        void restartAtDebtRung();

        // the whole search: setup, then one stage after another for each tick
        SearchResult run();
    };

}

namespace ap::sim {
    using namespace search_impl;
}
