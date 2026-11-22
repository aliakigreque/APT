// Reads of the current position, rectangle, rotation and scale of an object as the triggers left it, with
// per-step caches.

#include "triggers_internal.hpp"

namespace ap::sim {
    // true if the object has its own entry
    bool trigHasDyn(Game const& g, int idx) { return g.trig.on && g.trig.d && dynOf(g, idx) != nullptr; }

    // area trigger offsets of the object this tick, nullptr if none (or all 0)
    AreaOff const* trigAreaOff(Game const& g, int idx) {
        World const& w = *g.world;
        if (!w.hasAreaEffects || !w.areaTarget[static_cast<size_t>(idx)]) return nullptr;
        AreaOff const* off = areaOffFind(g, idx);
        if (!off || (off->dx == 0.f && off->dy == 0.f && off->rot == 0.f && !off->sclOn)) return nullptr;
        return off;
    }

    thread_local bool tl_posCacheOn = false;

    namespace trigger_impl {
        // groupPos with the group offsets cached per motion version, canon: shared offsets even for player groups
        void groupPosCachedImpl(Game const& g, Obj const& o, double& x, double& y, bool canon) {
            if (!tl_posCacheOn || o.groups.size() > 8 || o.groupSig < 0 || (canon && tl_undoOn)) {
                if (canon) {
                    bool const prevCanon = tl_canonRead;
                    tl_canonRead = true;
                    groupPos(g, o, x, y);
                    tl_canonRead = prevCanon;
                } else
                    groupPos(g, o, x, y);
                return;
            }
            if (!canon)
                if (PSet const* pset = psetOf(g))
                    for (short gr : o.groups)
                        if (bitOf(pset->groupBits, gr)) {
                            groupPos(g, o, x, y);
                            return;
                        }
            struct Slot {
                uint64_t ver;
                int sig;
                int n;
                double ox[8];
                double oy[8];
            };
            static thread_local Slot cache[1024];
            TrigMotion const& motion = g.trig.r().M();
            Slot& slot = cache[(static_cast<uint32_t>(o.groupSig) * 2654435761u ^
                                static_cast<uint32_t>(motion.ver * 0x9e3779b97f4a7c15ull >> 44)) &
                               1023u];
            if (slot.sig != o.groupSig || slot.ver != motion.ver) {
                slot.sig = o.groupSig;
                slot.ver = motion.ver;
                slot.n = 0;
                size_t const wordCount = motion.movedGroups.size();
                for (short gr : o.groups) {
                    size_t const word = static_cast<size_t>(static_cast<unsigned short>(gr)) >> 6;
                    if (gr < 0 || word >= wordCount || !((motion.movedGroups[word] >> (gr & 63)) & 1u)) continue;
                    if (auto* off = motion.groupOff.find(gr)) {
                        slot.ox[slot.n] = off->x;
                        slot.oy[slot.n] = off->y;
                        ++slot.n;
                    }
                }
            }
            x = o.posX;
            y = o.posY;
            for (int i = 0; i < slot.n; ++i) {
                if (!o.lockX) x += slot.ox[i];
                y += slot.oy[i];
            }
        }

        // position of an optimized keyframe object: group offsets saved at the start of the frame
        void kfOptPos(Game const& g, Obj const& o, double& x, double& y) {
            auto const& kfGroups = g.world->kfOptGroups;
            PSet const* const pset = psetOf(g);
            TrigPv const* const pv = pset ? g.trig.pv.get() : nullptr;
            if (pset)
                for (short gr : o.groups)
                    if (bitOf(pset->groupBits, gr)) {
                        if (!pv || pv->kfSnap.size() != kfGroups.size()) {
                            groupPos(g, o, x, y);
                            return;
                        }
                        pRead();
                        break;
                    }
            auto const& snap = g.trig.r().M().kfSnap;
            x = o.posX;
            y = o.posY;
            for (short gr : o.groups) {
                auto const it = std::lower_bound(kfGroups.begin(), kfGroups.end(), gr);
                if (it == kfGroups.end() || *it != gr) continue;
                size_t const i = static_cast<size_t>(it - kfGroups.begin());
                std::pair<double, double> offset{0.0, 0.0};
                if (pset && bitOf(pset->groupBits, gr)) {
                    offset = pv->kfSnap[i];
                    if (offset.first != offset.first) {
                        groupPos(g, o, x, y);
                        return;
                    }
                } else if (i < snap.size())
                    offset = snap[i];
                else
                    continue;
                if (!o.lockX) x += offset.first;
                y += offset.second;
            }
        }

        // position without the area trigger offsets
        void realPosBase(Game const& g, int idx, double& x, double& y) {
            if (tl_canonRead && isPO(g, idx)) {
                pHitAt();
            }
            if (auto* entry = dynOf(g, idx)) {
                x = entry->x;
                y = entry->y;
                return;
            }
            Obj const& o = objAt(g, idx);
            if (o.kfOpt) {
                kfOptPos(g, o, x, y);
                return;
            }
            groupPosCached(g, o, x, y);
        }

    }

    // saves the group offsets of optimized keyframes at the start of each frame
    void trigFrameSnap(Game& g) {
        World const& w = *g.world;
        if (w.kfOptGroups.empty() || !g.trig.on || !g.trig.h) return;
        if (w.frameStartOf(g.tick) != g.tick) return;
        size_t const groupCount = w.kfOptGroups.size();
        if (PSet const* pset = psetOf(g)) {
            bool anyPlayer = false;
            for (short gr : w.kfOptGroups)
                if (bitOf(pset->groupBits, gr)) {
                    anyPlayer = true;
                    break;
                }
            if (anyPlayer) {
                static thread_local std::vector<std::pair<double, double>> curP;
                double const nan = std::numeric_limits<double>::quiet_NaN();
                curP.assign(groupCount, {nan, nan});
                for (size_t i = 0; i < groupCount; ++i) {
                    short const gr = w.kfOptGroups[i];
                    if (!bitOf(pset->groupBits, gr)) continue;
                    curP[i] = {0.0, 0.0};
                    if (auto const* off = offFind(g, gr)) curP[i] = {off->x, off->y};
                }
                TrigPv const* const pv = g.trig.pv.get();
                auto same = [](std::vector<std::pair<double, double>> const& a,
                               std::vector<std::pair<double, double>> const& b) {
                    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0;
                };
                if (!pv || !same(pv->kfSnap, curP)) g.trig.wp().kfSnap = curP;
            }
        }
        TrigMotion const& motion = g.trig.r().M();
        static thread_local std::vector<std::pair<double, double>> cur;
        static thread_local uint64_t sameVer = 0;
        if (motion.ver != 0 && motion.ver == sameVer) return;
        cur.assign(groupCount, {0.0, 0.0});
        bool any = false;
        size_t const wordCount = motion.movedGroups.size();
        for (size_t i = 0; i < groupCount; ++i) {
            short const gr = w.kfOptGroups[i];
            size_t const word = static_cast<size_t>(static_cast<unsigned short>(gr)) >> 6;
            if (word >= wordCount || !((motion.movedGroups[word] >> (gr & 63)) & 1u)) continue;
            if (auto const* off = motion.groupOff.find(gr)) {
                cur[i] = {off->x, off->y};
                any = true;
            }
        }
        if (motion.kfSnap.empty() ? !any : motion.kfSnap == cur) {
            sameVer = motion.ver;
            return;
        }
        struct FM {
            int tick = -1;
            WKey w;
            std::vector<std::pair<std::shared_ptr<TrigHeavy>, std::shared_ptr<TrigHeavy>>> e;
        };
        static thread_local FM fm;
        if (fm.tick != g.tick || fm.w != g.world) {
            fm.tick = g.tick;
            fm.w = g.world;
            fm.e.clear();
        }
        for (auto const& pair : fm.e)
            if (pair.first == g.trig.h) {
                g.trig.h = pair.second;
                return;
            }
        std::shared_ptr<TrigHeavy> const before = g.trig.h;
        g.trig.w().Mw().kfSnap = cur;
        if (fm.e.size() < 64) fm.e.emplace_back(before, g.trig.h);
    }

    // debug: prints the position data of an object
    void trigDebugObj(Game const& g, int idx) {
        World const& w = *g.world;
        Obj const& o = objAt(g, idx);
        std::printf("    [posdbg] t=%d uid %d id %d movable %d lockX %d centerLike %d", g.tick, o.uid, o.id,
                    o.movable ? 1 : 0, o.lockX ? 1 : 0, w.centerLike[static_cast<size_t>(idx)] ? 1 : 0);
        if (g.trig.on && g.trig.h) {
            if (auto* entry = dynOf(g, idx)) std::printf(" dyn (%.3f, %.3f)", entry->x, entry->y);
            TrigHeavy const& heavy = g.trig.r();
            size_t const wordCount = heavy.M().movedGroups.size();
            for (short gr : o.groups) {
                size_t const word = static_cast<size_t>(static_cast<unsigned short>(gr)) >> 6;
                bool const moved = gr >= 0 && word < wordCount && ((heavy.M().movedGroups[word] >> (gr & 63)) & 1u);
                auto const* off = offFind(g, gr);
                std::printf(" | g%d rel %d moved %d off %s", gr, w.relevantGroup(gr) ? 1 : 0, moved ? 1 : 0,
                            off ? "yes" : "no");
                if (off) std::printf(" (%.3f, %.3f)", off->x, off->y);
            }
        }
        std::printf("\n");
    }

    // shared (canonical) position, where the shared grid stores the object
    void trigSharedPos(Game const& g, int idx, double& x, double& y) {
        if (!g.trig.on || !g.trig.h) {
            Obj const& o = g.world->objs[static_cast<size_t>(idx)];
            x = o.posX;
            y = o.posY;
            return;
        }
        if (isPO(g, idx)) {
            groupPosCanon(g, objAt(g, idx), x, y);
            return;
        }
        realPosBase(g, idx, x, y);
    }

    // real position of an object, area offsets included
    void trigRealPos(Game const& g, int idx, double& x, double& y) {
        realPosBase(g, idx, x, y);
        if (auto* off = trigAreaOff(g, idx)) {
            x += static_cast<double>(off->dx);
            y += static_cast<double>(off->dy);
        }
    }

    thread_local bool tl_rectCacheOn = false;

    // trigRect with a cache keyed by motion and dyn versions
    Rect trigRectCached(Game const& g, int idx) {
        if (!tl_rectCacheOn || !g.trig.d || !g.trig.h) return trigRect(g, idx);
        if (isPO(g, idx)) return trigRect(g, idx);
        if (trigAreaOff(g, idx)) return trigRect(g, idx);
        struct Slot {
            uint64_t mv;
            uint64_t dv;
            int idx;
            Rect r;
        };
        static thread_local Slot cache[4096];
        static thread_local WKey cacheW;
        if (cacheW != g.world) {
            cacheW = g.world;
            for (auto& entry : cache) entry.idx = -1;
        }
        uint64_t const motionVer = g.trig.r().M().ver;
        uint64_t const dynVer = g.trig.rd().ver;
        Slot& slot = cache[(static_cast<uint32_t>(idx) * 2654435761u ^
                            static_cast<uint32_t>(motionVer * 0x9e3779b97f4a7c15ull >> 40) ^
                            static_cast<uint32_t>(dynVer * 0xc2b2ae3d27d4eb4full >> 44)) &
                           4095u];
        if (slot.idx == idx && slot.mv == motionVer && slot.dv == dynVer) return slot.r;
        Rect const rect = trigRect(g, idx);
        slot.mv = motionVer;
        slot.dv = dynVer;
        slot.idx = idx;
        slot.r = rect;
        return rect;
    }

    // current collision rect of an object, triggers included (getObjectRect of the game)
    Rect trigRect(Game const& g, int idx) {
        Obj const& o = objAt(g, idx);
        double x, y;
        if (AreaOff const* off = trigAreaOff(g, idx)) {
            ObjDyn const* entry = dynOf(g, idx);
            realPosBase(g, idx, x, y);
            x += static_cast<double>(off->dx);
            y += static_cast<double>(off->dy);
            bool const rotated = (entry && entry->rotated) || off->rot != 0.f;
            float const sclX = off->sclOn ? off->sx : (entry && entry->scaled ? entry->sclX : o.scaleX);
            float const sclY = off->sclOn ? off->sy : (entry && entry->scaled ? entry->sclY : o.scaleY);
            bool const scaled = (entry && entry->scaled) || off->sclOn;
            if (rotated || scaled) {
                float const rot = o.rotX + (entry ? entry->rotAcc : 0.f) + off->rot;
                float const rotMod = std::fmod(std::fabs(rot), 90.f);
                if (o.useOuterOb || (entry && entry->outerOb) || rotated) {
                    return objectObb(const_cast<Game&>(g).obj(idx)).boundingRect();
                }
                float width = sclX * o.width * o.sws;
                float height = sclY * o.height * o.shs;
                if (!rotated) {
                    if (o.rotationAligned) std::swap(width, height);
                } else if (rotMod > 1e-3f)
                    std::swap(width, height);
                return Rect{static_cast<float>(x) + o.boxOffX - width * 0.5f,
                            static_cast<float>(y) + o.boxOffY - height * 0.5f, std::max(width, 0.f),
                            std::max(height, 0.f)};
            }
            if (o.rectGen) return genericObjRectStatic(o, static_cast<float>(x), static_cast<float>(y));
            Rect rect = o.rect;
            rect.x += static_cast<float>(x) - static_cast<float>(o.posX);
            rect.y += static_cast<float>(y) - static_cast<float>(o.posY);
            return rect;
        }
        if (auto* entry = dynOf(g, idx)) {
            if (entry->rotated || entry->scaled || entry->outerOb) {
                float const rot = o.rotX + entry->rotAcc;
                float const rotMod = std::fmod(std::fabs(rot), 90.f);
                if (o.useOuterOb || entry->outerOb || entry->rotated) {
                    return objectObb(const_cast<Game&>(g).obj(idx)).boundingRect();
                }
                float width = entry->sclX * o.width * o.sws;
                float height = entry->sclY * o.height * o.shs;
                if (!entry->rotated) {
                    if (o.rotationAligned) std::swap(width, height);
                } else if (rotMod > 1e-3f)
                    std::swap(width, height);
                return Rect{static_cast<float>(entry->x) + o.boxOffX - width * 0.5f,
                            static_cast<float>(entry->y) + o.boxOffY - height * 0.5f, std::max(width, 0.f),
                            std::max(height, 0.f)};
            }
            x = entry->x;
            y = entry->y;
        } else
            groupPos(g, o, x, y);
        if (o.rectGen) return genericObjRectStatic(o, static_cast<float>(x), static_cast<float>(y));
        if (o.useOuterOb && o.obbGen) {
            Point corners[4];
            OBB::cornersFromHalfAxes(corners,
                                     Point{static_cast<float>(x) + o.boxOffX, static_cast<float>(y) + o.boxOffY},
                                     o.obbHx, o.obbHy, o.obbVx, o.obbVy);
            return OBB::boundingRectOf(corners);
        }
        Rect rect = o.rect;
        rect.x += static_cast<float>(x) - static_cast<float>(o.posX);
        rect.y += static_cast<float>(y) - static_cast<float>(o.posY);
        return rect;
    }

    // m_lastPosition of the object, used by the physics for moving platform speed
    void trigLastPos(Game const& g, int idx, float& x, float& y) {
        World const& w = *g.world;
        if (w.hasAreaEffects && w.areaTarget[static_cast<size_t>(idx)]) {
            if (AreaOff const* off = areaOffFind(g, idx)) {
                bool const active = off->dx != 0.f || off->dy != 0.f || off->rot != 0.f || off->sclOn;
                if (active || off->pdx != 0.f || off->pdy != 0.f) {
                    double curX, curY;
                    realPosBase(g, idx, curX, curY);
                    if (active && w.frameStartOf(g.tick) == 0) {
                        x = static_cast<float>(curX + static_cast<double>(off->dx));
                        y = static_cast<float>(curY + static_cast<double>(off->dy));
                        return;
                    }
                    double startX, startY;
                    if (auto* entry = dynOf(g, idx)) {
                        if (entry->stamp == tickStamp(g)) {
                            startX = static_cast<double>(entry->lastX);
                            startY = static_cast<double>(entry->lastY);
                        } else {
                            startX = entry->x;
                            startY = entry->y;
                        }
                    } else if (objAt(g, idx).kfOpt) {
                        startX = curX;
                        startY = curY;
                    } else
                        groupStartPos(g, objAt(g, idx), startX, startY);
                    if (!active && startX == curX && startY == curY) {
                        x = static_cast<float>(curX);
                        y = static_cast<float>(curY);
                        return;
                    }
                    x = static_cast<float>(startX + static_cast<double>(off->pdx));
                    y = static_cast<float>(startY + static_cast<double>(off->pdy));
                    return;
                }
            }
        }
        if (auto* entry = dynOf(g, idx)) {
            x = entry->lastX;
            y = entry->lastY;
            return;
        }
        double prevX, prevY;
        groupPrevPos(g, objAt(g, idx), prevX, prevY);
        x = static_cast<float>(prevX);
        y = static_cast<float>(prevY);
    }

    // position of the object node: start of the current tick, area offset included
    void trigNodePos(Game const& g, int idx, float& x, float& y) {
        float areaX = 0.f;
        float areaY = 0.f;
        if (auto* off = trigAreaOff(g, idx)) {
            areaX = off->dx;
            areaY = off->dy;
        }
        if (auto* entry = dynOf(g, idx)) {
            if (entry->stamp == tickStamp(g)) {
                x = entry->lastX;
                y = entry->lastY;
            } else {
                x = static_cast<float>(entry->x);
                y = static_cast<float>(entry->y);
            }
            x += areaX;
            y += areaY;
            return;
        }
        double startX, startY;
        groupStartPos(g, objAt(g, idx), startX, startY);
        x = static_cast<float>(startX) + areaX;
        y = static_cast<float>(startY) + areaY;
    }

    // grid cell where the object is stored (gcx, gcy) and cell where it really is (ccx, ccy)
    void trigAreaCells(Game const& g, int idx, int& gcx, int& gcy, int& ccx, int& ccy) {
        World const& w = *g.world;
        if (isPO(g, idx)) {
            double storedX, storedY;
            groupPosCanon(g, objAt(g, idx), storedX, storedY);
            gcx = World::cellOf(storedX);
            gcy = w.cellYOf(storedY);
            ObjDyn const* entry = g.trig.pv ? g.trig.pv->dyn.find(idx) : nullptr;
            double x, y;
            if (entry && !std::binary_search(g.trig.pv->afObjs.begin(), g.trig.pv->afObjs.end(), idx)) {
                x = entry->secX;
                y = entry->secY;
            } else
                trigRealPos(g, idx, x, y);
            ccx = World::cellOf(x);
            ccy = w.cellYOf(y);
            return;
        }
        double storedX, storedY, x, y;
        if (auto* entry = dynOf(g, idx)) {
            storedX = entry->secX;
            storedY = entry->secY;
            x = entry->x;
            y = entry->y;
        } else {
            groupPosCanon(g, objAt(g, idx), storedX, storedY);
            x = storedX;
            y = storedY;
        }
        gcx = World::cellOf(storedX);
        gcy = w.cellYOf(storedY);
        if (auto* off = trigAreaOff(g, idx)) {
            x += static_cast<double>(off->dx);
            y += static_cast<double>(off->dy);
        }
        ccx = World::cellOf(x);
        ccy = w.cellYOf(y);
    }

    // current scale of the object, returns true if it comes from a trigger
    bool trigScaleAbs(Game const& g, int idx, float& sx, float& sy) {
        Obj const& o = objAt(g, idx);
        sx = o.scaleX;
        sy = o.scaleY;
        bool any = false;
        if (auto* entry = dynOf(g, idx))
            if (entry->scaled) {
                sx = entry->sclX;
                sy = entry->sclY;
                any = true;
            }
        if (auto* off = trigAreaOff(g, idx))
            if (off->sclOn) {
                sx = off->sx;
                sy = off->sy;
                any = true;
            }
        return any;
    }

    // getScalePosDelta of the game: half the box size change since the scale was last saved
    void trigScalePosDelta(Game const& g, int idx, float& dx, float& dy) {
        dx = 0.f;
        dy = 0.f;
        if (!g.trig.on || idx < 0) return;
        ObjDyn const* entry = dynOf(g, idx);
        AreaOff const* off = nullptr;
        if (World const& w = *g.world; w.hasAreaEffects && w.areaTarget[static_cast<size_t>(idx)]) {
            off = areaOffFind(g, idx);
            if (off && !off->sclOn && !off->psclOn) off = nullptr;
        }
        if (!off && (!entry || !entry->scaled)) return;
        Obj const& o = objAt(g, idx);
        Rect const rect = trigRect(g, idx);
        float width = rect.w;
        float height = rect.h;
        if (o.rotationAligned) std::swap(width, height);
        float scaleX, scaleY, prevScaleX, prevScaleY;
        if (off) {
            trigScaleAbs(g, idx, scaleX, scaleY);
            bool const dynScaled = entry && entry->scaled;
            if (dynScaled && entry->tStamp == tickStamp(g)) {
                prevScaleX = entry->cSclX;
                prevScaleY = entry->cSclY;
            } else if (off->psclOn) {
                prevScaleX = off->psx;
                prevScaleY = off->psy;
            } else {
                prevScaleX = dynScaled ? entry->sclX : o.scaleX;
                prevScaleY = dynScaled ? entry->sclY : o.scaleY;
            }
        } else {
            scaleX = entry->sclX;
            prevScaleX = entry->cSclX;
            scaleY = entry->sclY;
            prevScaleY = entry->cSclY;
        }
        if (static_cast<int>(scaleX * 10000.f) != static_cast<int>(prevScaleX * 10000.f)) {
            dx = (width - (width / scaleX) * prevScaleX) * 0.5f;
        }
        if (static_cast<int>(scaleY * 10000.f) != static_cast<int>(prevScaleY * 10000.f)) {
            dy = (height - (height / scaleY) * prevScaleY) * 0.5f;
        }
        if (o.rotationAligned) std::swap(dx, dy);
    }

    // rotation added by triggers, in degrees
    float trigRotation(Game const& g, int idx) {
        float rot = 0.f;
        if (auto* entry = dynOf(g, idx)) rot = entry->rotAcc;
        if (auto* off = trigAreaOff(g, idx)) rot += off->rot;
        return rot;
    }

    // true if a trigger rotated the object (or it must use its oriented box)
    bool trigRotated(Game const& g, int idx) {
        if (auto* off = trigAreaOff(g, idx))
            if (off->rot != 0.f) return true;
        if (auto* entry = dynOf(g, idx)) return entry->rotated || entry->outerOb;
        return false;
    }

    // true if the group isn't turned off by a toggle
    bool trigGroupOn(Game const& g, int group) { return groupEnabled(g, group); }

    // item counter value, 0 when triggers aren't simulated
    int trigItemValue(Game const& g, int item) { return g.trig.on ? countForItem(g, item) : 0; }

    // true if one of the object's groups is turned off
    bool trigGroupDisabled(Game const& g, Obj const& o) {
        TrigHeavy const& heavy = g.trig.r();
        if (!o.toggleable || (o.groupBloom & heavy.offBloom) == 0) return false;
        for (short gr : o.groups)
            if (gr >= 0 && offBit(heavy, gr)) return true;
        return false;
    }

    // true if one of the object's groups was ever moved
    bool trigObjMoved(Game const& g, Obj const& o) {
        auto const& bits = g.trig.r().M().movedGroups;
        if (bits.empty()) return false;
        for (short gr : o.groups) {
            if (gr < 0) continue;
            size_t const word = static_cast<size_t>(gr) >> 6;
            if (word < bits.size() && ((bits[word] >> (gr & 63)) & 1u)) return true;
        }
        return false;
    }

    // grid cell changed by moves, nullptr if it's still the static one
    std::vector<int> const* trigCell(Game const& g, int cx, int cy) {
        auto* cell = g.trig.r().M().cellOv.find(World::cellKey(cx, cy));
        return cell ? cell->v.get() : nullptr;
    }

    // true if a moved gameplay object touches the box
    bool trigMovedGameplayIn(Game& g, float x0, float x1, float y0, float y1) {
        if (!g.trig.on || !g.trig.h) return false;
        World const& w = *g.world;
        Rect const box{x0, y0, x1 - x0, y1 - y0};
        auto hit = [&](int idx) {
            Obj const& o = w.objs[static_cast<size_t>(idx)];
            if (o.type == Decoration || o.trigger || o.type == Modifier || o.type == CollisionObject || o.cls == 1) {
                return false;
            }
            GObj const view = g.obj(idx);
            if (view.disabled()) return false;
            Rect const rect = view.getObjectRect();
            if (rect.w >= 3000.f || rect.h >= 3000.f) return false;
            return intersectsRect(box, rect);
        };
        for (int idx : g.trig.areaMoved)
            if (hit(idx)) return true;
        auto const& overrides = g.trig.r().M().cellOv.items;
        int const cx0 = std::max(0, World::cellOfF(x0) - 1);
        int const cx1 = World::cellOfF(x1) + 1;
        int const cy0 = std::max(0, w.cellYOfF(y0) - 1);
        int const cy1 = w.cellYOfF(y1) + 1;
        auto it = std::lower_bound(overrides.begin(), overrides.end(), World::cellKey(cx0, 0),
                                   [](auto const& e, int key) { return e.first < key; });
        for (; it != overrides.end() && it->first <= World::cellKey(cx1, 4095); ++it) {
            int const cy = it->first % 4096;
            if (cy < cy0 || cy > cy1 || !it->second.v) continue;
            for (int idx : *it->second.v)
                if (hit(idx)) return true;
        }
        return false;
    }

}
