// Why the states die: loop signatures, trap touches, and the walk back from a killer to the triggers and items
// that move it; plus the worker pool.

#include "search_internal.hpp"

namespace ap::sim {
    namespace search_impl {
        // hash of a state to detect endless loops, with or without the trigger state
        uint64_t loopSig(Game const& g, bool withTriggers) {
            uint64_t h = (withTriggers && g.trig.on) ? static_cast<uint64_t>(trigHashMain(g)) : 0x51f15e5u;
            auto mix = [&](double v) {
                uint64_t b;
                std::memcpy(&b, &v, sizeof b);
                h ^= b + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            };
            Player const& p = g.p1;
            mix(p.y);
            mix(p.yVelocity);
            mix(p.playerSpeed);
            mix(p.vehicleSize);
            mix(g.isDualMode ? g.p2.y : 0.0);
            mix(static_cast<double>((p.isShip ? 1 : 0) | (p.isBird ? 2 : 0) | (p.isDart ? 4 : 0) | (p.isSwing ? 8 : 0) |
                                    (p.isBall ? 16 : 0) | (p.isRobot ? 32 : 0) | (p.isSpider ? 64 : 0) |
                                    (p.isUpsideDown ? 128 : 0) | (g.isDualMode ? 256 : 0)));
            return h;
        }

        // counts the objects ahead that turning on this group would turn off
        void offAheadOf(World const& w, int group, bool viaToggle, float x0, int depth, std::vector<char>& seenGroups,
                        int& n, float& lo, float& hi) {
            if (group <= 0 || depth > 3 || static_cast<size_t>(group) >= seenGroups.size() ||
                seenGroups[static_cast<size_t>(group)])
                return;
            seenGroups[static_cast<size_t>(group)] = 1;
            for (int i : w.allOfGroup(group)) {
                Obj const& t = w.objs[static_cast<size_t>(i)];
                if (viaToggle) {
                    if (t.cls == 1 || t.type == Decoration || t.x <= x0) continue;
                    ++n;
                    lo = std::min(lo, t.x);
                    hi = std::max(hi, t.x);
                    continue;
                }
                if (t.cls != 1 || !t.spawnTriggered) continue;
                if (t.id == ObjId::ToggleTrigger && !t.tf(Obj::TF_ActivateGroup))
                    offAheadOf(w, t.targetGroup, true, x0, depth + 1, seenGroups, n, lo, hi);
                else if (t.id == ObjId::SpawnTrigger)
                    offAheadOf(w, t.targetGroup, false, x0, depth + 1, seenGroups, n, lo, hi);
            }
        }

        // true if the toggle only turns on a hidden wall far ahead
        bool turnsOnWallFarAhead(World const& w, Obj const& o) {
            if (o.targetGroup <= 0) return false;
            int obstacles = 0;
            for (int i : w.allOfGroup(o.targetGroup)) {
                Obj const& t = w.objs[static_cast<size_t>(i)];
                if (t.cls == 1 || t.type == Decoration) continue;
                if (t.type != Solid && t.type != Hazard && t.type != Slope && t.type != AnimatedHazard) return false;
                if (t.x < o.x + 3000.f) return false;
                ++obstacles;
            }
            if (obstacles == 0) return false;
            for (size_t i = 0; i < w.objs.size(); ++i) {
                Obj const& t = w.objs[i];
                if (t.id == ObjId::ToggleTrigger && t.cls == 1 && !t.touchTriggered && !t.spawnTriggered &&
                    t.targetGroup == o.targetGroup && !t.tf(Obj::TF_ActivateGroup) && t.x <= o.x)
                    return true;
            }
            return false;
        }

        // returns true if the touch trigger turns off at least 300 objects spread over 3000 units ahead (a trap)
        bool isTrapTouchItself(World const& w, Obj const& o) {
            if (o.cls != 1 || !o.touchTriggered) return false;
            std::vector<char> seenGroups(static_cast<size_t>(w.maxGroup) + 2, 0);
            int n = 0;
            float lo = 1e30f;
            float hi = -1e30f;
            if (o.id == ObjId::ToggleTrigger && o.tf(Obj::TF_ActivateGroup)) return turnsOnWallFarAhead(w, o);
            if (o.id == ObjId::ToggleTrigger && !o.tf(Obj::TF_ActivateGroup))
                offAheadOf(w, o.targetGroup, true, o.x, 0, seenGroups, n, lo, hi);
            else if (o.id == ObjId::SpawnTrigger)
                offAheadOf(w, o.targetGroup, false, o.x, 0, seenGroups, n, lo, hi);
            else
                return false;
            return n >= 300 && hi - lo > 3000.f;
        }

        // hitboxes of the trap touch triggers of a level, worked out once per level
        std::vector<Rect> trapRectsOf(World const& w) {
            static std::mutex mutex;
            static World const* cachedWorld = nullptr;
            static size_t cachedSize = 0;
            static std::vector<Rect> rects;
            std::lock_guard<std::mutex> lock(mutex);
            if (cachedWorld != &w || cachedSize != w.objs.size()) {
                rects.clear();
                for (Obj const& t : w.objs)
                    if (isTrapTouchItself(w, t)) rects.push_back(t.rect);
                cachedWorld = &w;
                cachedSize = w.objs.size();
            }
            return rects;
        }

        // a trap, or a touch trigger stacked on one: touching it means touching the trap
        bool isTrapTouch(World const& w, Obj const& o) {
            if (o.cls != 1 || !o.touchTriggered) return false;
            if (isTrapTouchItself(w, o)) return true;
            for (Rect const& r : trapRectsOf(w))
                if (o.rect.x < r.x + r.w && r.x < o.rect.x + o.rect.w && o.rect.y < r.y + r.h &&
                    r.y < o.rect.y + o.rect.h)
                    return true;
            return false;
        }

        // number of traps around x that were touched in this state
        int trapsTouchedNear(Game const& g, Cues const& cues, float x) {
            int n = 0;
            for (auto const& t : cues.traps) {
                if (t.x0 > x) break;
                if (t.x1 < x) continue;
                if (g.trig.on && trigActivatedBit(g, t.idx)) ++n;
            }
            return n;
        }

        // returns true if a toggle orb on this group changes gameplay (following spawns up to 3 deep)
        bool ringEffectMatters(World const& w, int group, int depth) {
            if (group <= 0 || depth > 3) return false;
            for (int i : w.allOfGroup(group)) {
                Obj const& t = w.objs[static_cast<size_t>(i)];
                if (!t.trigger && t.cls != 1) {
                    if (t.type != Decoration) return true;
                    continue;
                }
                switch (t.id) {
                case 901:
                case 1346:
                case 1347:
                case 1814:
                case 3016:
                    if (w.relevantGroup(t.targetGroup)) return true;
                    break;
                case 1049:
                case 1268:
                    if (ringEffectMatters(w, t.targetGroup, depth + 1)) return true;
                    break;
                default:
                    break;
                }
            }
            return false;
        }

        // at an impasse at x fx: which toggle orbs to take or avoid so the killer groups get moved or turned off
        CauseResult impasseCauses(World const& w, Game const& bg, CauseIndex& ix,
                                  std::vector<short> const& killerGroups, float fx) {
            ix.build(w);
            CauseResult out;
            CauseWalk walk{w, bg, ix, fx, out, {}};
            for (short killer : killerGroups) {
                if (killer <= 0 || killer >= 10000) continue;
                for (int r : ix.byTarget[static_cast<size_t>(killer)]) {
                    Obj const& o = w.objs[static_cast<size_t>(r)];
                    bool const rescue = o.id == ObjId::MoveTrigger || o.id == ObjId::RotateTrigger ||
                                        o.id == ObjId::FollowTrigger || o.id == ObjId::FollowPlayerYTrigger ||
                                        (o.id == ObjId::ToggleTrigger && causeDeactivates(o));
                    if (!rescue || o.id == ObjId::ToggleOrb || o.id == ObjId::ToggleBlock) continue;
                    if (!(o.x > fx - 2400.f && o.x < fx + 600.f)) continue;
                    if (!o.spawnTriggered && walk.fired(r)) continue;
                    walk.whyNot(r, 0);
                }
            }
            return out;
        }

        // item conditions that would have avoided the impasse (same arguments as impasseCauses)
        std::vector<ItemTarget> itemTargetsFor(World const& w, Game const& bg, CauseIndex& ix,
                                               std::vector<short> const& killerGroups, float fx) {
            ix.build(w);
            std::vector<ItemTarget> out;
            auto fromCounters = [&](short group) {
                for (int a : ix.byTarget[static_cast<size_t>(group)]) {
                    Obj const& c = w.objs[static_cast<size_t>(a)];
                    if ((c.id != ObjId::InstantCountTrigger && c.id != ObjId::CountTrigger) || !causeActivates(c) ||
                        c.spawnTriggered || c.touchTriggered || !(c.x < fx))
                        continue;
                    if (trigGroupDisabled(bg, c)) continue;
                    if (std::any_of(out.begin(), out.end(), [&](ItemTarget const& t) {
                            return t.idx == a ||
                                   (t.item == std::clamp(c.itemID, 0, 9999) && t.count == c.count && t.group == group);
                        }))
                        continue;
                    ItemTarget t;
                    t.idx = a;
                    t.item = std::clamp(c.itemID, 0, 9999);
                    t.count = c.count;
                    t.group = group;
                    t.x = c.x;
                    for (int a2 : ix.byTarget[static_cast<size_t>(group)]) {
                        Obj const& c2 = w.objs[static_cast<size_t>(a2)];
                        if ((c2.id == ObjId::InstantCountTrigger || c2.id == ObjId::CountTrigger) &&
                            causeActivates(c2) && !c2.spawnTriggered && !c2.touchTriggered && c2.x < fx)
                            t.x = std::max(t.x, c2.x);
                    }
                    t.mode = c.id == ObjId::CountTrigger ? 3 : (c.compare == 1 ? 1 : c.compare == 2 ? 2 : 0);
                    if (t.dist(trigItemValue(bg, t.item)) == 0) continue;
                    out.push_back(t);
                }
            };
            {
                float const px = bg.p1.x;
                float const py = bg.p1.y;
                std::vector<short> offGroups;
                for (auto const& o : w.objs) {
                    if (o.type != Solid && o.type != Slope) continue;
                    if (o.rect.maxX() < px - 240.f || o.rect.x > px + 240.f || o.rect.maxY() < py - 150.f ||
                        o.rect.y > py + 150.f)
                        continue;
                    for (short group : o.groups)
                        if (group > 0 && group < 10000 && !trigGroupOn(bg, group) &&
                            std::find(offGroups.begin(), offGroups.end(), group) == offGroups.end())
                            offGroups.push_back(group);
                    if (offGroups.size() > 16) break;
                }
                for (short group : offGroups) fromCounters(group);
            }
            for (short killer : killerGroups) {
                if (killer <= 0 || killer >= 10000) continue;
                for (int r : ix.byTarget[static_cast<size_t>(killer)]) {
                    Obj const& o = w.objs[static_cast<size_t>(r)];
                    bool const rescue =
                            ((o.id == ObjId::ToggleOrb || o.id == ObjId::ToggleBlock) && causeDeactivates(o)) ||
                            (o.id == ObjId::ToggleTrigger && causeDeactivates(o));
                    if (!rescue || !(o.x > fx - 2400.f && o.x < fx + 600.f)) continue;
                    for (short group : o.groups) {
                        if (group <= 0 || group >= 10000 || trigGroupOn(bg, group)) continue;
                        fromCounters(group);
                    }
                }
            }
            return out;
        }

    }
}
