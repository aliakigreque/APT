// Group state: moving, toggling and spawning groups, the move journal and the spatial cells of moved objects
// (GJBaseGameLayer::moveObjects, toggleGroup, spawnGroup).

#include "triggers_internal.hpp"

namespace ap::sim {
    namespace trigger_impl {
        // members of a group that count for physics, filtered once per group
        std::vector<int> const& physOfGroup(World const& w, int group) {
            if (tl_physWorld != &w) {
                tl_physWorld = &w;
                tl_physOfGroup.clear();
                tl_physDense.assign(static_cast<size_t>(std::max(0, w.maxGroup) + 1), nullptr);
            }
            bool const dense = group >= 0 && static_cast<size_t>(group) < tl_physDense.size();
            if (dense)
                if (auto const* p = tl_physDense[static_cast<size_t>(group)]) return *p;
            auto it = tl_physOfGroup.find(group);
            if (it != tl_physOfGroup.end()) {
                if (dense) tl_physDense[static_cast<size_t>(group)] = &it->second;
                return it->second;
            }
            std::vector<int> members;
            for (int idx : w.allOfGroup(group)) {
                if (w.decoLikeAt(idx) && !w.centerLike[static_cast<size_t>(idx)]) continue;
                members.push_back(idx);
            }
            std::vector<int> const& stored = tl_physOfGroup.emplace(group, std::move(members)).first->second;
            if (dense) tl_physDense[static_cast<size_t>(group)] = &stored;
            return stored;
        }

        // true if the group has a member that counts and isn't a player object, cached per player set
        bool hasNonPMember(Game const& g, PSet const* pset, int group) {
            World const& w = *g.world;
            auto scan = [&](std::vector<int> const& list) {
                for (int idx : list) {
                    if (bitOf(pset->objBits, idx)) continue;
                    if (w.decoLikeAt(idx) && !w.centerLike[static_cast<size_t>(idx)]) continue;
                    return true;
                }
                return false;
            };
            if (tl_remap.idx >= 0) return scan(w.allOfGroup(group));
            static thread_local std::unordered_map<uint64_t, bool> cache;
            static thread_local WKey cw;
            if (cw != &w || cache.size() > 65536) {
                cache.clear();
                cw = &w;
            }
            uint64_t const key =
                    (pset->sig * 0x9e3779b97f4a7c15ull) ^ static_cast<uint64_t>(static_cast<uint32_t>(group));
            auto it = cache.find(key);
            if (it != cache.end()) return it->second;
            bool const found = scan(physOfGroup(w, group));
            cache.emplace(key, found);
            return found;
        }

        // spatial layout of a group, built once per world
        GroupSpatial const& spatialOfGroup(World const& w, int group) {
            if (tl_spWorld != &w) {
                tl_spWorld = &w;
                tl_spatial.clear();
            }
            auto it = tl_spatial.find(group);
            if (it != tl_spatial.end()) return it->second;
            GroupSpatial spatial;
            for (int idx : physOfGroup(w, group)) {
                Obj const& o = w.objs[static_cast<size_t>(idx)];
                if (w.mayHaveDyn[static_cast<size_t>(idx)])
                    spatial.mobile.push_back(idx);
                else {
                    spatial.fixedX.emplace_back(static_cast<float>(o.posX), idx);
                    for (short gr : o.groups)
                        if (gr >= 0) spatial.groups.push_back(gr);
                }
            }
            std::sort(spatial.fixedX.begin(), spatial.fixedX.end());
            std::sort(spatial.groups.begin(), spatial.groups.end());
            spatial.groups.erase(std::unique(spatial.groups.begin(), spatial.groups.end()), spatial.groups.end());
            return tl_spatial.emplace(group, std::move(spatial)).first->second;
        }

        // own entry of an object (moves on its own), created at its current position if missing
        ObjDyn& dynEntry(Game& g, int idx) {
            if (isPO(g, idx)) return dynEntryP(g, idx);
            dynDep(2);
            fyCheck(idx, 1);
            if (g.trig.rd().objDyn.find(idx)) {
                ObjDyn* p = g.trig.wd().objDyn.find(idx);
                return *p;
            }
            Obj const& o = objAt(g, idx);
            TrigHeavy& heavy = g.trig.w();
            for (short gr : o.groups) markGroupMoved(heavy, gr);
            {
                auto& dynGroups = g.trig.wd().dynGroups;
                for (short gr : o.groups) {
                    if (gr < 0) continue;
                    size_t const word = static_cast<size_t>(gr) >> 6;
                    if (dynGroups.size() <= word) dynGroups.resize(word + 1, 0);
                    dynGroups[word] |= 1ull << (gr & 63);
                }
            }
            ObjDyn entry;
            groupPos(g, o, entry.x, entry.y);
            entry.secX = entry.x;
            entry.secY = entry.y;
            entry.sclX = o.scaleX;
            entry.sclY = o.scaleY;
            entry.cSclX = o.scaleX;
            entry.cSclY = o.scaleY;
            bool movedThisTick = false;
            for (short gr : o.groups) {
                if (auto* off = heavy.M().groupOff.find(gr))
                    if (off->stamp == tickStamp(g)) movedThisTick = true;
            }
            if (movedThisTick) {
                double startX, startY;
                groupStartPos(g, o, startX, startY);
                entry.lastX = static_cast<float>(startX);
                entry.lastY = static_cast<float>(startY);
                entry.stamp = tickStamp(g);
            } else {
                double prevX, prevY;
                groupPrevPos(g, o, prevX, prevY);
                entry.lastX = static_cast<float>(prevX);
                entry.lastY = static_cast<float>(prevY);
                entry.stamp = -1;
            }
            heavy.anyMotion = true;
            return g.trig.wd().objDyn[idx] = entry;
        }

        // same as dynEntry for a player object, stored in the per state part
        ObjDyn& dynEntryP(Game& g, int idx) {
            pRead();
            if (g.trig.pv)
                if (ObjDyn* p = g.trig.wp().dyn.find(idx)) return *p;
            Obj const& o = objAt(g, idx);
            ObjDyn entry;
            groupPos(g, o, entry.x, entry.y);
            entry.secX = entry.x;
            entry.secY = entry.y;
            entry.sclX = o.scaleX;
            entry.sclY = o.scaleY;
            entry.cSclX = o.scaleX;
            entry.cSclY = o.scaleY;
            bool movedThisTick = false;
            for (short gr : o.groups) {
                if (auto* off = offFind(g, gr))
                    if (off->stamp == tickStamp(g)) movedThisTick = true;
            }
            if (movedThisTick) {
                double startX, startY;
                groupStartPos(g, o, startX, startY);
                entry.lastX = static_cast<float>(startX);
                entry.lastY = static_cast<float>(startY);
                entry.stamp = tickStamp(g);
            } else {
                double prevX, prevY;
                groupPrevPos(g, o, prevX, prevY);
                entry.lastX = static_cast<float>(prevX);
                entry.lastY = static_cast<float>(prevY);
                entry.stamp = -1;
            }
            TrigPv& pv = g.trig.wp();
            pv.dynBloom |= o.groupBloom;
            return pv.dyn[idx] = entry;
        }

        // true if an object of the group has its own entry
        bool anyDynInGroup(Game const& g, int group) {
            dynDep(1);
            {
                auto const& dynGroups = g.trig.rd().dynGroups;
                size_t const word = static_cast<size_t>(group) >> 6;
                if (group < 0 || word >= dynGroups.size() || !((dynGroups[word] >> (group & 63)) & 1u)) return false;
            }
            for (auto const& e : g.trig.rd().objDyn) {
                Obj const& o = objAt(g, e.first);
                for (short gr : o.groups)
                    if (gr == group) return true;
            }
            return false;
        }

        // tracked members of a group (no plain deco) sorted by index
        std::vector<int> const& sortedTrackedOfGroup(World const& w, int group) {
            if (tl_stWorld != &w) {
                tl_stWorld = &w;
                tl_sortedTracked.clear();
            }
            auto it = tl_sortedTracked.find(group);
            if (it != tl_sortedTracked.end()) return it->second;
            std::vector<int> members;
            for (int idx : w.allOfGroup(group)) {
                if (!w.decoLikeAt(idx) || w.centerLike[static_cast<size_t>(idx)]) members.push_back(idx);
            }
            std::sort(members.begin(), members.end());
            return tl_sortedTracked.emplace(group, std::move(members)).first->second;
        }

        // fills out with the positions in objDyn.items of the group members that have their own entry
        void dynPositionsInGroup(Game const& g, int group, std::vector<size_t>& out) {
            out.clear();
            dynDep(1);
            TrigDyn const& dyn = g.trig.rd();
            {
                auto const& dynGroups = dyn.dynGroups;
                size_t const word = static_cast<size_t>(group) >> 6;
                if (group < 0 || word >= dynGroups.size() || !((dynGroups[word] >> (group & 63)) & 1u)) return;
            }
            auto const& items = dyn.objDyn.items;
            auto const& members = sortedTrackedOfGroup(*g.world, group);
            if (members.size() * 4 < items.size()) {
                size_t hint = 0;
                for (int idx : members) {
                    size_t const k = dyn.objDyn.indexHint(idx, hint);
                    if (k < items.size()) {
                        out.push_back(k);
                        fyCheck(idx, 2);
                    }
                }
            } else {
                for (size_t k = 0; k < items.size(); ++k) {
                    Obj const& o = objAt(g, items[k].first);
                    for (short gr : o.groups)
                        if (gr == group) {
                            out.push_back(k);
                            fyCheck(items[k].first, 2);
                            break;
                        }
                }
            }
        }

        // offset of a group in the undo journal
        GroupOff const* undoLookup(short gr, bool& hit) { return undoFind(gr, hit); }

        // old offset of a group at the current journal position, hit is false if it wasn't changed later
        GroupOff const* undoFind(short gr, bool& hit) {
            hit = false;
            auto const& undo = tl_undoJr->undo;
            auto it = std::lower_bound(undo.begin(), undo.end(), gr,
                                       [](UndoE const& e, short value) { return e.group < value; });
            for (; it != undo.end() && it->group == gr; ++it)
                if (it->pos > tl_jrPos) {
                    hit = true;
                    return it->absent ? nullptr : &it->old;
                }
            return nullptr;
        }

        // moves the own entries of the group's player objects, sectY: grid has y rows
        void moveFyMembers(Game& g, int group, double dx, double dy, bool sectY) {
            if (!g.trig.pv || g.trig.pv->dyn.items.empty()) return;
            if (group >= 0 && !(g.trig.pv->dynBloom & (1ull << (group & 63)))) return;
            bool any = false;
            for (auto const& e : g.trig.pv->dyn.items) {
                for (short gr : objAt(g, e.first).groups)
                    if (gr == group) {
                        any = true;
                        break;
                    }
                if (any) break;
            }
            if (!any) return;
            if (tl_memoComp) pHitAt();
            for (auto& e : g.trig.wp().dyn.items) {
                Obj const& o = objAt(g, e.first);
                bool inGroup = false;
                for (short gr : o.groups)
                    if (gr == group) {
                        inGroup = true;
                        break;
                    }
                if (!inGroup) continue;
                stampLast(g, e.second);
                if (dx != 0.0 && !o.lockX) e.second.x += dx;
                if (dy != 0.0) e.second.y += dy;
                if (dx != 0.0 || (dy != 0.0 && sectY)) {
                    e.second.secX = e.second.x;
                    e.second.secY = e.second.y;
                }
            }
        }

        // moveObjects for a player group: offset in the player part and own entries of its objects
        void moveGroupP(Game& g, int group, double dx, double dy) {
            if (tl_memoComp) pHitAt();
            TrigPv& pv = g.trig.wp();
            GroupOff* off = pv.off.find(static_cast<short>(group));
            if (!off) {
                GroupOff init;
                if (auto const* shared = g.trig.r().M().groupOff.find(static_cast<short>(group))) {
                    init = *shared;
                    if (shared->stamp == tickStamp(g)) {
                        init.x = shared->px;
                        init.y = shared->py;
                    }
                }
                if (dx == 0.0 && dy == 0.0 && !g.trig.r().M().groupOff.find(static_cast<short>(group))) return;
                pv.off[static_cast<short>(group)] = init;
                off = pv.off.find(static_cast<short>(group));
            }
            if (off->stamp != tickStamp(g)) {
                off->px = off->x;
                off->py = off->y;
                off->stamp = tickStamp(g);
            }
            off->x += dx;
            off->y += dy;
            moveFyMembers(g, group, dx, dy, g.world->ySection);
        }

        // section grid cell (cx, cy) for writing, copied if shared, kept sorted by uid
        std::vector<int>& cellMut(Game& g, int cx, int cy) {
            int const key = World::cellKey(cx, cy);
            TrigHeavy& heavy = g.trig.w();
            TrigMotion& motion = heavy.Mw();
            if (auto* cell = motion.cellOv.find(key)) {
                if (cell->v.use_count() > 1) cell->v = std::make_shared<std::vector<int>>(*cell->v);
                cell->valid = false;
                return *cell->v;
            }
            auto& slot = motion.cellOv[key];
            auto const& cells = g.world->cells;
            if (static_cast<size_t>(cx) < cells.size() &&
                static_cast<size_t>(cy) < cells[static_cast<size_t>(cx)].size()) {
                slot.v = std::make_shared<std::vector<int>>(cells[static_cast<size_t>(cx)][static_cast<size_t>(cy)]);
                auto const& objs = g.world->objs;
                auto& list = *slot.v;
                if (!std::is_sorted(list.begin(), list.end(), [&](int a, int b) {
                        return static_cast<unsigned>(objs[static_cast<size_t>(a)].uid) <
                               static_cast<unsigned>(objs[static_cast<size_t>(b)].uid);
                    }))
                    std::sort(list.begin(), list.end(), [&](int a, int b) {
                        return static_cast<unsigned>(objs[static_cast<size_t>(a)].uid) <
                               static_cast<unsigned>(objs[static_cast<size_t>(b)].uid);
                    });
            } else
                slot.v = std::make_shared<std::vector<int>>();
            slot.valid = false;
            return *slot.v;
        }

        // moves an object from cell (cx0, cy0) to (cx1, cy1), like removeObjectFromSection then addToSection
        void cellMove(Game& g, int idx, int cx0, int cy0, int cx1, int cy1) {
            auto const& objs = g.world->objs;
            unsigned const uid = static_cast<unsigned>(objs[static_cast<size_t>(idx)].uid);
            auto byUid = [&](int a, unsigned value) {
                return static_cast<unsigned>(objs[static_cast<size_t>(a)].uid) < value;
            };
            World const& w = *g.world;
            {
                auto& from = cellMut(g, cx0, cy0);
                auto it = std::lower_bound(from.begin(), from.end(), uid, byUid);
                if (it != from.end() && *it == idx)
                    from.erase(it);
                else {
                    auto found = std::find(from.begin(), from.end(), idx);
                    if (found != from.end()) from.erase(found);
                }
                if (from.empty()) {
                    auto const& cells = w.cells;
                    bool const staticEmpty = static_cast<size_t>(cx0) >= cells.size() ||
                                             static_cast<size_t>(cy0) >= cells[static_cast<size_t>(cx0)].size() ||
                                             cells[static_cast<size_t>(cx0)][static_cast<size_t>(cy0)].empty();
                    if (staticEmpty) g.trig.w().Mw().cellOv.erase(World::cellKey(cx0, cy0));
                }
            }
            auto& to = cellMut(g, cx1, cy1);
            to.insert(std::lower_bound(to.begin(), to.end(), uid, byUid), idx);
            if (static_cast<size_t>(idx) < w.blockMovingMark.size() && w.blockMovingMark[static_cast<size_t>(idx)]) {
                TrigMotion& motion = g.trig.w().Mw();
                Obj const& o = objs[static_cast<size_t>(idx)];
                if (World::cellOf(o.posX) == cx1 && w.cellYOf(o.posY) == cy1)
                    motion.blkCell.erase(idx);
                else
                    motion.blkCell[idx] = World::cellKey(cx1, cy1);
            }
        }

        // updates the section of an object moving from (oldX, oldY) to (newX, newY)
        void resection(Game& g, int idx, double oldX, double oldY, double newX, double newY) {
            World const& w = *g.world;
            if (g.trig.rd().objDyn.find(idx)) {
                fyCheck(idx, 3);
                ObjDyn* entry = g.trig.wd().objDyn.find(idx);
                oldX = entry->secX;
                oldY = entry->secY;
                entry->secX = newX;
                entry->secY = newY;
            }
            int const cx0 = World::cellOf(oldX);
            int const cx1 = World::cellOf(newX);
            int const cy0 = w.cellYOf(oldY);
            int const cy1 = w.cellYOf(newY);
            if (cx0 == cx1 && cy0 == cy1) return;
            ++g_trigCounters[5];
            Obj const& o = objAt(g, idx);
            if (o.type == Decoration || (o.extendedCollision && o.id != 0x718)) return;
            cellMove(g, idx, cx0, cy0, cx1, cy1);
        }

        // same as resection when the own entry d is already at hand
        void resectionDyn(Game& g, int idx, ObjDyn& d, double newX, double newY) {
            World const& w = *g.world;
            double const oldX = d.secX;
            double const oldY = d.secY;
            d.secX = newX;
            d.secY = newY;
            int const cx0 = World::cellOf(oldX);
            int const cx1 = World::cellOf(newX);
            int const cy0 = w.cellYOf(oldY);
            int const cy1 = w.cellYOf(newY);
            if (cx0 == cx1 && cy0 == cy1) return;
            ++g_trigCounters[5];
            Obj const& o = objAt(g, idx);
            if (o.type == Decoration || (o.extendedCollision && o.id != 0x718)) return;
            cellMove(g, idx, cx0, cy0, cx1, cy1);
        }

        // moveObjects(group, dx, dy) of the game: group offset, own entries and sections of every object
        void moveGroup(Game& g, int group, double dx, double dy, bool stampOnly) {
            if (group < 0 || group > 9999) return;
            World const& w = *g.world;
            PSet const* const pset = psetOf(g);
            if (pset) {
                bool const isPlayerGroup = bitOf(pset->groupBits, group);
                if (!isPlayerGroup && dx == 0.0 && dy == 0.0 &&
                    !g.trig.r().M().groupOff.find(static_cast<short>(group)))
                    return;
                if (!tl_canon) {
                    if (isPlayerGroup) {
                        moveGroupP(g, group, dx, dy);
                        return;
                    }
                    moveFyMembers(g, group, dx, dy, w.ySection);
                } else if (tl_jr) {
                    if (isPlayerGroup) {
                        if (!tl_inPend) {
                            JOp op{};
                            op.kind = JOp::PADD;
                            op.group = group;
                            op.dx = dx;
                            op.dy = dy;
                            jrPush(op);
                        }
                    } else if (bitOf(pset->memberGroupBits, group)) {
                        JOp op{};
                        op.kind = JOp::GMOVE;
                        op.group = group;
                        op.dx = dx;
                        op.dy = dy;
                        jrPush(op);
                    }
                }
            }
            bool const skipP = pset && !tl_canon;
            struct CanonReadScope {
                bool prev;
                bool on;
                explicit CanonReadScope(bool enable) : prev(tl_canonRead), on(enable) {
                    if (on) tl_canonRead = true;
                }
                ~CanonReadScope() {
                    if (on) tl_canonRead = prev;
                }
            };
            CanonReadScope const canonScope(pset && tl_canon);
            if (dx == 0.0 && dy == 0.0 && !g.trig.r().M().groupOff.find(static_cast<short>(group))) return;
            if (pset && tl_canon && tl_jr && !tl_jrImm && !bitOf(pset->groupBits, group) &&
                bitOf(pset->memberGroupBits, group)) {
                GroupOff const* cur = g.trig.r().M().groupOff.find(static_cast<short>(group));
                UndoE undo{};
                undo.group = group;
                undo.pos = static_cast<int>(tl_jr->ops.size());
                undo.absent = cur == nullptr;
                if (cur) undo.old = *cur;
                tl_jr->undo.push_back(undo);
            }
            TrigHeavy& heavy = g.trig.w();
            auto& offs = heavy.Mw().groupOff;
            GroupOff* off = offs.find(static_cast<short>(group));
            if (!off) {
                off = &offs[static_cast<short>(group)];
                heavy.anyMotion = true;
                markGroupMoved(heavy, static_cast<short>(group));
            }
            (void)stampOnly;
            if (dx == 0.0 && dy == 0.0) {
                if (off->stamp != tickStamp(g)) {
                    off->px = off->x;
                    off->py = off->y;
                    off->stamp = tickStamp(g);
                }
                static thread_local std::vector<size_t> dpos;
                dynPositionsInGroup(g, group, dpos);
                if (!dpos.empty()) {
                    dynDep(2);
                    auto& items = g.trig.wd().objDyn.items;
                    for (size_t k : dpos) stampLast(g, items[k].second);
                }
                return;
            }
            bool const sectX = dx != 0.0;
            bool const sectY = dy != 0.0 && w.ySection;
            World::GroupFast const* const fast = w.fastOf(group);
            auto const& objs = fast ? fast->other : w.objsOfGroup(group);
            static thread_local std::vector<double> oldX;
            static thread_local std::vector<double> oldY;
            static thread_local std::vector<char> oldDyn;
            if (sectX || sectY) {
                oldX.resize(objs.size());
                oldY.resize(objs.size());
                oldDyn.resize(objs.size());
                dynDep(2);
                bool const anyDyn = !g.trig.rd().objDyn.items.empty();
                double const groupOffX = off->x;
                double const groupOffY = off->y;
                size_t hint = 0;
                auto const& dynMap = g.trig.rd().objDyn;
                int lastSig = -1;
                static thread_local std::vector<std::pair<double, double>> lastOffs;
                TrigHeavy const& heavyRead = g.trig.r();
                size_t const wordCount = heavyRead.M().movedGroups.size();
                auto groupSum = [&](Obj const& o, double& x, double& y) {
                    if (o.groupSig != lastSig || lastSig < 0) {
                        lastOffs.clear();
                        for (short gr : o.groups) {
                            size_t const word = static_cast<size_t>(static_cast<unsigned short>(gr)) >> 6;
                            if (gr < 0 || word >= wordCount || !((heavyRead.M().movedGroups[word] >> (gr & 63)) & 1u)) {
                                continue;
                            }
                            if (auto* groupOff = offFind(g, gr)) lastOffs.emplace_back(groupOff->x, groupOff->y);
                        }
                        lastSig = o.groupSig;
                    }
                    x = o.posX;
                    y = o.posY;
                    for (auto const& e : lastOffs) {
                        if (!o.lockX) x += e.first;
                        y += e.second;
                    }
                };
                for (size_t i = 0; i < objs.size(); ++i) {
                    World::ObjLite const& lite = w.lite[static_cast<size_t>(objs[i])];
                    size_t const k = (anyDyn && w.mayHaveDyn[static_cast<size_t>(objs[i])])
                                             ? dynMap.indexHint(objs[i], hint)
                                             : dynMap.items.size();
                    auto const* entry = k < dynMap.items.size() ? &dynMap.items[k].second : nullptr;
                    oldDyn[i] = entry != nullptr;
                    if (entry) {
                        oldX[i] = entry->x;
                        oldY[i] = entry->y;
                        fyCheck(objs[i], 4);
                    } else if (lite.nGroups == 1 && lite.g0 == static_cast<short>(group)) {
                        oldX[i] = lite.lockX ? lite.posX : lite.posX + groupOffX;
                        oldY[i] = lite.posY + groupOffY;
                    } else if (lite.groupSig == lastSig && lastSig >= 0) {
                        double x = lite.posX;
                        double y = lite.posY;
                        for (auto const& e : lastOffs) {
                            if (!lite.lockX) x += e.first;
                            y += e.second;
                        }
                        oldX[i] = x;
                        oldY[i] = y;
                    } else
                        groupSum(objAt(g, objs[i]), oldX[i], oldY[i]);
                }
            }
            static thread_local std::vector<std::array<double, 3>> fastOld;
            fastOld.clear();
            if (fast && (sectX || sectY)) {
                double const groupOffX = off->x;
                double const groupOffY = off->y;
                static thread_local std::vector<int> cand;
                cand.clear();
                auto addRange = [&](std::vector<float> const& rests, std::vector<int> const& indices, double offset,
                                    double delta) {
                    double const margin = 0.05;
                    double const lo = std::min(0.0, delta) - margin;
                    double const hi = std::max(0.0, delta) + margin;
                    if (hi - lo >= 99.0) {
                        cand.insert(cand.end(), indices.begin(), indices.end());
                        return;
                    }
                    double shift = std::fmod(offset, 100.0);
                    if (shift < 0.0) shift += 100.0;
                    auto take = [&](double qFrom, double qTo) {
                        auto first = std::lower_bound(rests.begin(), rests.end(), static_cast<float>(qFrom) - 1e-3f);
                        auto last = std::upper_bound(rests.begin(), rests.end(), static_cast<float>(qTo) + 1e-3f);
                        for (auto it = first; it < last; ++it) {
                            cand.push_back(indices[static_cast<size_t>(it - rests.begin())]);
                        }
                    };
                    for (double base : {0.0, 100.0}) {
                        double qFrom = base - hi - shift;
                        double qTo = base - lo - shift;
                        while (qTo < 0.0) {
                            qFrom += 100.0;
                            qTo += 100.0;
                        }
                        while (qFrom >= 100.0) {
                            qFrom -= 100.0;
                            qTo -= 100.0;
                        }
                        if (qFrom < 0.0) {
                            take(qFrom + 100.0, 100.0);
                            take(0.0, qTo);
                        } else if (qTo >= 100.0) {
                            take(qFrom, 100.0);
                            take(0.0, qTo - 100.0);
                        } else
                            take(qFrom, qTo);
                    }
                };
                for (auto const& bucket : fast->buckets) {
                    double sumX = 0.0;
                    double sumY = 0.0;
                    for (short moverGroup : bucket.movers) {
                        if (moverGroup == static_cast<short>(group)) {
                            sumX += groupOffX;
                            sumY += groupOffY;
                            continue;
                        }
                        if (auto const* moverOff = offFind(g, moverGroup)) {
                            sumX += moverOff->x;
                            sumY += moverOff->y;
                        }
                    }
                    if (sectX) addRange(bucket.qx, bucket.byX, sumX, dx);
                    if (sectY) addRange(bucket.qy, bucket.byY, sumY, dy);
                }
                if (cand.size() > 1) {
                    std::sort(cand.begin(), cand.end());
                    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
                }
                for (int idx : cand) {
                    Obj const& o = objAt(g, idx);
                    double prevX, prevY;
                    groupPos(g, o, prevX, prevY);
                    fastOld.push_back({static_cast<double>(idx), prevX, prevY});
                }
            }
            if (off->stamp != tickStamp(g)) {
                off->px = off->x;
                off->py = off->y;
                off->stamp = tickStamp(g);
            }
            off->x += dx;
            off->y += dy;
            {
                static thread_local std::vector<size_t> dpos;
                dynPositionsInGroup(g, group, dpos);
                if (!dpos.empty()) {
                    dynDep(2);
                    auto& items = g.trig.wd().objDyn.items;
                    for (size_t k : dpos) {
                        auto& e = items[k];
                        Obj const& o = objAt(g, e.first);
                        stampLast(g, e.second);
                        if (dx != 0.0 && !o.lockX) e.second.x += dx;
                        if (dy != 0.0) e.second.y += dy;
                    }
                }
            }
            if (sectX || sectY) {
                for (size_t i = 0; i < objs.size(); ++i) {
                    if (skipP && bitOf(pset->objBits, objs[i])) continue;
                    double newX, newY;
                    if (oldDyn[i]) {
                        auto const* entry = g.trig.rd().objDyn.find(objs[i]);
                        newX = entry->x;
                        newY = entry->y;
                    } else {
                        double const movedX = w.lite[static_cast<size_t>(objs[i])].lockX ? oldX[i] : oldX[i] + dx;
                        double const movedY = oldY[i] + dy;
                        int const oldCell = World::cellOf(oldX[i]);
                        if (World::cellOf(movedX - 1e-6) == oldCell && World::cellOf(movedX + 1e-6) == oldCell &&
                            (!sectY || (w.cellYOf(movedY - 1e-6) == w.cellYOf(oldY[i]) &&
                                        w.cellYOf(movedY + 1e-6) == w.cellYOf(oldY[i]))))
                            continue;
                        groupPos(g, objAt(g, objs[i]), newX, newY);
                    }
                    resection(g, objs[i], oldX[i], oldY[i], newX, newY);
                }
            }
            for (auto const& fastEntry : fastOld) {
                int const idx = static_cast<int>(fastEntry[0]);
                if (skipP && bitOf(pset->objBits, idx)) continue;
                double const prevX = fastEntry[1];
                double const prevY = fastEntry[2];
                double const movedX = prevX + dx;
                double const movedY = prevY + dy;
                int const oldCell = World::cellOf(prevX);
                if (World::cellOf(movedX - 1e-6) == oldCell && World::cellOf(movedX + 1e-6) == oldCell &&
                    (!sectY ||
                     (w.cellYOf(movedY - 1e-6) == w.cellYOf(prevY) && w.cellYOf(movedY + 1e-6) == w.cellYOf(prevY))))
                    continue;
                double newX, newY;
                groupPos(g, objAt(g, idx), newX, newY);
                resection(g, idx, prevX, prevY, newX, newY);
            }
        }

        // true if the group wasn't turned off by a toggle
        bool groupEnabled(Game const& g, int group) {
            if (group < 0) group = 0;
            if (group > 9999) group = 9999;
            return !offBit(g.trig.r(), group);
        }

        // turns a group on or off, what a toggle trigger does
        void toggleGroup(Game& g, int id, bool activate) {
            int groupId = id < 0 ? 0 : id;
            if (groupId > 9999) groupId = 9999;
            bool const enabled = groupEnabled(g, groupId);
            if (enabled == activate) return;
            TrigHeavy& heavy = g.trig.w();
            size_t const word = static_cast<size_t>(groupId) >> 6;
            if (heavy.offBits.size() <= word) heavy.offBits.resize(word + 1, 0);
            if (activate) {
                heavy.offBits[word] &= ~(1ull << (groupId & 63));
                --heavy.offCount;
            } else {
                heavy.offBits[word] |= 1ull << (groupId & 63);
                ++heavy.offCount;
            }
            heavy.offBloom = 0;
            for (uint64_t bits : heavy.offBits) heavy.offBloom |= bits;
        }

        // toggleGroupTriggered of the game: toggles the group, then spawns it with the remap keys if turned on
        void toggleGroupTriggered(Game& g, int group, bool activate, int uid, int control, RemapKeys const* keys) {
            toggleGroup(g, group, activate);
            if (activate) spawnGroupLayer(g, group, false, 0.0, uid, control, (keys && keys->n) ? keys : nullptr);
        }

        // true if the trigger was already activated (non multi triggers only fire once)
        bool hasBeenActivated(Game const& g, int idx) {
            if (trigHasSlot(g, idx)) return trigActivatedBit(g, idx);
            ObjState const* state = g.osFind(idx);
            return state && (state->activatedByPlayer1 || state->activatedByPlayer2);
        }

        void triggerActivated(Game& g, int idx) {
            Obj const& o = objAt(g, idx);
            if (o.multiTriggered) return;
            if (trigHasSlot(g, idx)) {
                trigSetActivatedBit(g, idx);
                return;
            }
            ObjState& state = g.osMut(idx);
            state.activatedByPlayer1 = true;
            state.activatedByPlayer2 = true;
        }

        // queues a group spawn that fires after delay seconds
        void queueSpawn(Game& g, int group, float delay, bool ordered, int uid, int control, RemapKeys const* keys) {
            if (group <= 0) return;
            SpawnAction spawn;
            spawn.group = group;
            spawn.delay = static_cast<double>(delay);
            spawn.ordered = ordered;
            spawn.uid = uid;
            spawn.control = control;
            if (keys) spawn.keys = *keys;
            g.trig.w().spawnQueue.push_back(spawn);
        }

        // spawn ordered: each trigger waits for its x distance to the first one
        void spawnObjectsInOrder(Game& g, std::vector<int> const& objs, double delay, int uid, int control,
                                 RemapKeys const* keys) {
            float firstX = 0.f;
            bool first = false;
            for (int idx : objs) {
                Obj const& o = objAt(g, idx);
                bool const anim = o.cls != 1 && o.id == ObjId::AnimatedObject && o.animOnTrigger;
                if (!anim && (o.cls != 1 || !isSpawnableTrigger(o.id))) continue;
                float const posX = GObj{&o, &g, idx}.getPosition().x;
                if (first) {
                    float const spawnDelay =
                            static_cast<float>(static_cast<double>(posX - firstX) / 311.5801086425781 - delay);
                    if (0.f < spawnDelay) {
                        SpawnAction spawn;
                        spawn.obj = idx;
                        spawn.delay = static_cast<double>(spawnDelay);
                        spawn.uid = uid;
                        spawn.control = control;
                        if (keys) spawn.keys = *keys;
                        g.trig.w().spawnQueue.push_back(spawn);
                        continue;
                    }
                } else {
                    firstX = posX;
                    first = true;
                }
                spawnObject(g, idx, 0.0, keys);
            }
        }

        // spawnGroup of the game: activates every spawn triggered trigger of the group, once per step
        void spawnGroupLayer(Game& g, int group, bool ordered, double delay, int uid, int control,
                             RemapKeys const* keys) {
            int groupId = group < 0 ? 0 : group;
            if (groupId > 9999) groupId = 9999;
            if (!groupEnabled(g, groupId)) return;
            int const remapUid = (keys && keys->n) ? -keys->begin()[keys->n - 1] : 0;
            int const trigId = g.enable22Changes ? uid : 0;
            uint64_t const spawnKey = (static_cast<uint64_t>(static_cast<uint32_t>(group) & 0xFFFFu) << 48) |
                                      (static_cast<uint64_t>(static_cast<uint32_t>(trigId) & 0xFFFFFFu) << 24) |
                                      (static_cast<uint64_t>(static_cast<uint32_t>(remapUid) & 0xFFFFFFu));
            bool const already = g.trig.spawnedThisTick.count(spawnKey) != 0;
            if (tl_spRec) tl_spTests.emplace_back(spawnKey, already);
            if (already) return;
            g.trig.spawnedThisTick.insert(spawnKey);
            auto const& objs = g.world->allOfGroup(group);
            if (!ordered) {
                for (int idx : objs) spawnObject(g, idx, delay, keys);
            } else
                spawnObjectsInOrder(g, objs, delay, uid, control, keys);
        }

        // spawnObject of the game: triggers the object, with its remapped copy if there are remap keys
        void spawnObject(Game& g, int idx, double delay, RemapKeys const* keys) {
            Obj const& o = objAt(g, idx);
            if (o.cls != 1) return;
            if (trigGroupDisabled(g, o)) return;
            if (!o.multiTriggered && hasBeenActivated(g, idx)) return;
            if (!o.spawnTriggered || !isSpawnableTrigger(o.id)) return;
            if (!hasSimEffect(o.id)) return;
            triggerActivated(g, idx);
            if (o.id == ObjId::SpawnTrigger) {
                if (delay != 0.0)
                    g.trig.w().spawnStored[idx] = delay;
                else if (g.trig.r().spawnStored.find(idx))
                    g.trig.w().spawnStored.erase(idx);
            }
            if (!keys || keys->n == 0) {
                triggerObject(g, idx, 0);
                return;
            }
            RemapOverride const saved = tl_remap;
            Obj remapped = o;
            World const& w = *g.world;
            for (int i = 0; i + 1 < keys->n; ++i) {
                int const remapId = (*keys)[i];
                if (remapId <= 0 || static_cast<size_t>(remapId) >= w.spawnRemaps.size()) continue;
                auto const& pairs = w.spawnRemaps[static_cast<size_t>(remapId)];
                auto map = [&](int& field) {
                    for (auto const& e : pairs)
                        if (e.first == field) {
                            field = e.second;
                            return;
                        }
                };
                map(remapped.targetGroup);
                map(remapped.centerGroup);
                map(remapped.itemID);
                map(remapped.itemID2);
                map(remapped.controlID);
                map(remapped.rotTarget);
                map(remapped.targetModCenter);
                map(remapped.kfGroup);
                map(remapped.rotMinX);
                map(remapped.rotMinY);
                map(remapped.rotMaxX);
                map(remapped.rotMaxY);
            }
            tl_remap.idx = idx;
            tl_remap.obj = &remapped;
            tl_remap.parentKeys = keys;
            triggerObject(g, idx, 0);
            tl_remap = saved;
        }

    }
}
