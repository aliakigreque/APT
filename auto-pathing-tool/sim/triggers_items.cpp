// Item, timer, sequence, area setup, control (stop/pause/resume) and camera tween triggers, plus the object
// collision callbacks.

#include "triggers_internal.hpp"

namespace ap::sim {
    namespace trigger_impl {
        // value of an item counter (0 if missing)
        int countForItem(Game const& g, int item) {
            if (item < 0) item = 0;
            if (item > 9999) item = 9999;
            auto* count = g.trig.r().items.find(item);
            return count ? *count : 0;
        }

        // updateCountForItem of the game: sets the item, then fires the count triggers waiting for it
        void updateCountForItem(Game& g, int item, int value) {
            if (item < 0) item = 0;
            if (item > 9999) item = 9999;
            int const old = countForItem(g, item);
            if (value != old && static_cast<size_t>(item) < g.world->checkedItems.size() &&
                g.world->checkedItems[static_cast<size_t>(item)]) {
                ++g.itemChanges;
                ++tl_itemDelta;
                if (debugOptions().trigLogAt(g.tick))
                    std::printf("[item] t=%d item %d : %d -> %d\n", g.tick, item, old, value);
            }
            {
                TrigHeavy& heavy = g.trig.w();
                if (value == 0)
                    heavy.items.erase(item);
                else
                    heavy.items[item] = value;
            }
            bool const up = !(value < old);
            {
                static thread_local std::vector<size_t> pos;
                static thread_local std::vector<CountAction> ents;
                pos.clear();
                ents.clear();
                auto const& counts = g.trig.r().counts;
                for (size_t i = 0; i < counts.size(); ++i)
                    if (counts[i].item == item) {
                        pos.push_back(i);
                        ents.push_back(counts[i]);
                    }
                if (pos.empty()) return;
                bool sorted = true;
                for (size_t k = 1; k < ents.size() && sorted; ++k) {
                    sorted = up ? !(ents[k].target < ents[k - 1].target) : !(ents[k - 1].target < ents[k].target);
                }
                if (!sorted) {
                    std::stable_sort(ents.begin(), ents.end(), [up](CountAction const& x, CountAction const& y) {
                        return up ? x.target < y.target : y.target < x.target;
                    });
                    auto& writable = g.trig.w().counts;
                    for (size_t k = 0; k < pos.size(); ++k) writable[pos[k]] = ents[k];
                }
            }
            for (size_t k = 0;;) {
                size_t foundIdx = SIZE_MAX;
                {
                    auto const& counts = g.trig.r().counts;
                    size_t seen = 0;
                    for (size_t i = 0; i < counts.size(); ++i)
                        if (counts[i].item == item) {
                            if (seen == k) {
                                foundIdx = i;
                                break;
                            }
                            ++seen;
                        }
                }
                if (foundIdx == SIZE_MAX) break;
                CountAction const action = g.trig.r().counts[foundIdx];
                bool erased = false;
                if (!action.paused && action.prev != value) {
                    int const prev = action.prev;
                    int const target = action.target;
                    g.trig.w().counts[foundIdx].prev = value;
                    bool fire;
                    if (target <= prev)
                        fire = (target < prev) && (value <= target);
                    else
                        fire = !(value < target);
                    if (fire) {
                        if (!action.multi) {
                            auto& writable = g.trig.w().counts;
                            writable.erase(writable.begin() + static_cast<long>(foundIdx));
                            erased = true;
                        }
                        toggleGroupTriggered(g, action.group, action.activate, action.uid, action.control,
                                             &action.keys);
                    }
                }
                if (!erased) ++k;
            }
        }

        // remap keys of the activation of idx (nullptr if it's not remapped)
        RemapKeys const* activationKeys(int idx) { return (tl_remap.idx == idx) ? tl_remap.parentKeys : nullptr; }

        // remaps a group id of the trigger being activated, like its fields (applyRemap of the game)
        int remapForActivation(Game const& g, int idx, int value) {
            if (tl_remap.idx != idx || !tl_remap.parentKeys) return value;
            RemapKeys const& keys = *tl_remap.parentKeys;
            World const& w = *g.world;
            for (int i = 0; i + 1 < keys.n; ++i) {
                int const remapId = keys[i];
                if (remapId <= 0 || static_cast<size_t>(remapId) >= w.spawnRemaps.size()) continue;
                for (auto const& e : w.spawnRemaps[static_cast<size_t>(remapId)])
                    if (e.first == value) {
                        value = e.second;
                        break;
                    }
            }
            return value;
        }

        // item or timer id clamped to [0, 9999]
        int clampItem(int id) { return id < 0 ? 0 : (id > 9999 ? 9999 : id); }

        // timer with this id, nullptr if missing
        TimerItem const* findTimer(Game const& g, int id) {
            for (auto const& timer : g.trig.r().timers)
                if (timer.id == id) return &timer;
            return nullptr;
        }

        // position of the timer in the list, -1 if missing
        int timerIndex(Game const& g, int id) {
            auto const& timers = g.trig.r().timers;
            for (size_t i = 0; i < timers.size(); ++i)
                if (timers[i].id == id) return static_cast<int>(i);
            return -1;
        }

        // timer value, 0 if missing
        double timerValue(Game const& g, int id) {
            auto const* timer = findTimer(g, clampItem(id));
            return timer ? timer->time : 0.0;
        }

        // createTimer of the game: creates the timer or updates the existing one
        void createTimer(Game& g, int id, double start, double target, bool stopEnabled, bool running,
                         bool dontOverride, float mod, bool ignoreTW, int group, RemapKeys const* keys, int uid,
                         int control) {
            int const index = timerIndex(g, id);
            TrigHeavy& heavy = g.trig.w();
            if (index < 0) {
                TimerItem timer;
                timer.id = id;
                timer.time = static_cast<double>(static_cast<float>(start));
                timer.target = static_cast<double>(static_cast<float>(target));
                timer.stopEnabled = stopEnabled;
                timer.running = running;
                timer.mod = mod;
                timer.ignoreTW = ignoreTW;
                timer.group = group;
                timer.uid = uid;
                timer.control = control;
                if (keys) timer.keys = *keys;
                msvcHashInsert(heavy.timers, heavy.timerBuckets, timer, [](TimerItem const& x) { return x.id; });
            } else {
                TimerItem& timer = heavy.timers[static_cast<size_t>(index)];
                timer.running = running;
                timer.mod = mod;
                timer.ignoreTW = ignoreTW;
                timer.target = target;
                timer.stopEnabled = stopEnabled;
                timer.group = group;
                timer.uid = uid;
                timer.control = control;
                timer.disabled = false;
                if (!dontOverride) timer.time = start;
            }
        }

        // updateTimer of the game: sets the timer value, creates it if missing
        void updateTimer(Game& g, int id, double value) {
            id = clampItem(id);
            if (value < -9999999.0) value = -9999999.0;
            if (value > 9999999.0) value = 9999999.0;
            int const index = timerIndex(g, id);
            if (index >= 0) {
                g.trig.w().timers[static_cast<size_t>(index)].time = value;
                return;
            }
            createTimer(g, id, value, 0.0, false, false, true, 1.f, false, 0, nullptr, 0, 0);
        }

        // getItemValue of the game: 1 item, 2 timer, 3 points, 4 time, 5 attempts
        double getItemValue(Game const& g, int type, int id) {
            switch (type) {
            case 1:
                return static_cast<double>(countForItem(g, id));
            case 2:
                return timerValue(g, id);
            case 3:
                return static_cast<double>(g.trig.r().points);
            case 4:
                return g.time2;
            case 5:
                return static_cast<double>(g.world ? g.world->attempts : 1);
            default:
                return 0.0;
            }
        }

        // a (op) b, 1 +, 2 -, 3 *, 4 / (0 if b is 0)
        double itemOp(double a, double b, int op) {
            switch (op) {
            case 1:
                return a + b;
            case 2:
                return a - b;
            case 3:
                return a * b;
            case 4:
                return b == 0.0 ? 0.0 : a / b;
            default:
                return 0.0;
            }
        }

        // 1 nearest, 2 floor, 3 ceil, else unchanged
        double itemRound(double value, int rounding) {
            return rounding == 1 ? std::round(value)
                                 : (rounding == 2 ? std::floor(value) : (rounding == 3 ? std::ceil(value) : value));
        }

        // 1 absolute value, 2 negative absolute value, else unchanged
        double itemSign(double value, int sign) {
            return sign == 1 ? std::fabs(value) : (sign == 2 ? -std::fabs(value) : value);
        }

        // item edit (3619): computes a value from items, timers or points and writes it to the target
        void activateItemEdit(Game& g, Obj const& o) {
            World const& w = *g.world;
            int id1 = o.itemID;
            int mode1 = static_cast<int>(w.prop(o, 476));
            int const targetMode0 = static_cast<int>(w.prop(o, 478));
            int const targetMode = targetMode0 < 1 ? 1 : targetMode0;
            int const targetOp = static_cast<int>(w.prop(o, 480));
            int const round1 = static_cast<int>(w.prop(o, 485));
            int const round2 = static_cast<int>(w.prop(o, 486));
            int const sign1 = static_cast<int>(w.prop(o, 578));
            int const sign2 = static_cast<int>(w.prop(o, 579));
            int const id2 = o.itemID2;
            int const mode2 = static_cast<int>(w.prop(o, 477));
            int const target = o.targetGroup;
            int const op12 = std::max(1, static_cast<int>(w.prop(o, 481)));
            int const opMod = std::max(3, static_cast<int>(w.prop(o, 482)));
            float const mod = static_cast<float>(
                    static_cast<double>(std::round(static_cast<float>(w.prop(o, 479)) * 1000.f)) / 1000.0);
            if (!(0 < target || targetMode == 3)) return;
            bool use1 = mode1 != 0 && (0 < id1 || mode1 == 3 || mode1 == 4);
            bool use2 = mode2 != 0 && (0 < id2 || mode2 == 3 || mode2 == 4);
            int firstMode = mode1;
            int firstId = id1;
            if (!use1 && use2) {
                use1 = true;
                use2 = false;
                firstMode = mode2;
                firstId = id2;
            }
            double value1 = getItemValue(g, firstMode, firstId);
            double const value2 = getItemValue(g, mode2, id2);
            double const targetValue = getItemValue(g, targetMode, target);
            if (use2) value1 = itemOp(value1, value2, op12);
            double result = static_cast<double>(mod);
            if (use1) result = itemOp(value1, result, opMod);
            result = itemSign(itemRound(result, round1), sign1);
            if (targetOp != 0) {
                switch (targetOp) {
                case 1:
                    result = result + targetValue;
                    break;
                case 2:
                    result = targetValue - result;
                    break;
                case 3:
                    result = result * targetValue;
                    break;
                case 4:
                    result = result == 0.0 ? 0.0 : targetValue / result;
                    break;
                default:
                    result = 0.0;
                    break;
                }
            }
            result = itemSign(itemRound(result, round2), sign2);
            if (targetMode == 1)
                updateCountForItem(g, target, static_cast<int>(result));
            else if (targetMode == 2)
                updateTimer(g, target, result);
            else if (targetMode == 3)
                g.trig.w().points = static_cast<int>(result);
        }

        // item compare (3620): compares two expressions and spawns the true or false group
        void activateItemCompare(Game& g, Obj const& o, RemapKeys const* keys) {
            World const& w = *g.world;
            int const round1 = static_cast<int>(w.prop(o, 485));
            int const round2 = static_cast<int>(w.prop(o, 486));
            int const sign1 = static_cast<int>(w.prop(o, 578));
            int const sign2 = static_cast<int>(w.prop(o, 579));
            int const id1 = o.itemID;
            int const id2 = o.itemID2;
            int const mode2 = static_cast<int>(w.prop(o, 477));
            int const compareOp = static_cast<int>(w.prop(o, 482));
            int op1 = static_cast<int>(w.prop(o, 480));
            if (op1 < 1) op1 = 3;
            int op2 = static_cast<int>(w.prop(o, 481));
            if (op2 < 1) op2 = 3;
            int mode1 = static_cast<int>(w.prop(o, 476));
            if (mode1 < 1) mode1 = 1;
            float const mod1 = std::round(static_cast<float>(w.prop(o, 479)) * 1000.f);
            float const mod2 = std::round(static_cast<float>(w.prop(o, 483)) * 1000.f);
            float const tolerance = static_cast<float>(
                    static_cast<double>(std::round(static_cast<float>(w.prop(o, 484)) * 1000.f)) / 1000.0);
            bool const use2 = mode2 != 0 && (0 < id2 || mode2 == 3 || mode2 == 4);
            double left = itemOp(getItemValue(g, mode1, id1), static_cast<double>(mod1 / 1000.f), op1);
            left = itemSign(itemRound(left, round1), sign1);
            double right = static_cast<double>(mod2 / 1000.f);
            if (use2) right = itemOp(getItemValue(g, mode2, id2), right, op2);
            right = itemSign(itemRound(right, round2), sign2);
            double const tol = static_cast<double>(tolerance);
            bool result = false;
            switch (compareOp) {
            case 0:
                result = std::fabs(left - right) <= tol;
                break;
            case 1:
                result = !(tol + left == right) && !(tol + left < right);
                break;
            case 2:
                result = right <= tol + left;
                break;
            case 3:
                result = !(right == left - tol) && !(right < left - tol);
                break;
            case 4:
                result = left - tol <= right;
                break;
            case 5:
                result = !(std::fabs(left - right) == tol) && !(std::fabs(left - right) < tol);
                break;
            default:
                break;
            }
            if (g_trace || debugOptions().trigLogAt(g.tick)) {
                std::printf("[items] t=%d compare uid %d : %g ? %g (op %d, tol %g) -> %s -> group %d\n", g.tick, o.uid,
                            left, right, compareOp, tol, result ? "true" : "false",
                            result ? o.targetGroup : o.centerGroup);
            }
            spawnGroupLayer(g, result ? o.targetGroup : o.centerGroup, false, 0.0, o.uid, o.controlID, keys);
        }

        // persistent item (3641): marks or unmarks an item (or timer) as persistent, can reset it or all of them
        void activatePersistentItem(Game& g, Obj const& o) {
            World const& w = *g.world;
            bool const mark = w.prop(o, 491) != 0.0;
            bool const isTimer = w.prop(o, 494) != 0.0;
            bool const all = w.prop(o, 492) != 0.0;
            bool const reset = w.prop(o, 493) != 0.0;
            auto setOf = [&](TrigHeavy& heavy) -> std::vector<int>& {
                return isTimer ? heavy.persistTimers : heavy.persistItems;
            };
            if (!all) {
                int const id = o.itemID;
                {
                    std::vector<int> const& current = isTimer ? g.trig.r().persistTimers : g.trig.r().persistItems;
                    bool const inSet = std::find(current.begin(), current.end(), id) != current.end();
                    if (mark && !inSet)
                        setOf(g.trig.w()).push_back(id);
                    else if (!mark && inSet) {
                        auto& set = setOf(g.trig.w());
                        set.erase(std::find(set.begin(), set.end(), id));
                    }
                }
                if (reset) {
                    if (!isTimer)
                        updateCountForItem(g, id, 0);
                    else if (findTimer(g, id))
                        updateTimer(g, id, 0.0);
                }
            } else {
                if (reset) {
                    std::vector<int> const ids = isTimer ? g.trig.r().persistTimers : g.trig.r().persistItems;
                    for (int id : ids) {
                        if (!isTimer)
                            updateCountForItem(g, id, 0);
                        else if (findTimer(g, id))
                            updateTimer(g, id, 0.0);
                    }
                }
                if (!mark && !(isTimer ? g.trig.r().persistTimers : g.trig.r().persistItems).empty()) {
                    setOf(g.trig.w()).clear();
                }
            }
        }

        // timer (3614), time event (3615) and time control (3617)
        void activateTimerTrigger(Game& g, Obj const& o, RemapKeys const* keys) {
            World const& w = *g.world;
            if (o.id == ObjId::TimeTrigger) {
                float const mod = static_cast<float>(w.prop(o, 470, 0.0));
                createTimer(g, o.itemID, w.prop(o, 467), w.prop(o, 473), w.prop(o, 474) != 0.0, w.prop(o, 471) == 0.0,
                            w.prop(o, 468) != 0.0, mod, w.prop(o, 469) != 0.0, o.targetGroup, keys, o.uid, o.controlID);
            } else if (o.id == ObjId::TimeEventTrigger) {
                TimeEvent event;
                event.timer = o.itemID;
                event.last = 0.f;
                event.target = static_cast<float>(w.prop(o, 473));
                event.group = o.targetGroup;
                event.uid = o.uid;
                event.control = o.controlID;
                event.multi = w.prop(o, 475) != 0.0;
                if (keys) event.keys = *keys;
                g.trig.w().timeEvents.push_back(event);
            } else if (o.id == ObjId::TimeControlTrigger) {
                int const controlType = static_cast<int>(w.prop(o, 472));
                int const index = timerIndex(g, o.itemID);
                if (index >= 0 && (controlType == 0 || controlType == 1)) {
                    g.trig.w().timers[static_cast<size_t>(index)].running = controlType == 0;
                }
            }
        }

        // sequence (3607): each activation moves along a list of (group, count) and spawns the group of that rank
        void activateSequence(Game& g, int idx, Obj const& o, RemapKeys const* keys) {
            World const& w = *g.world;
            auto const* ext = w.extOf(o);
            std::vector<double> const* list = ext ? ext->list(435) : nullptr;
            if (!list) return;
            int key = 0;
            if (w.prop(o, 505) != 0.0 && keys && keys->n > 0) key = -(*keys)[keys->n - 1];
            long long const seqKey = (static_cast<long long>(idx) << 32) | static_cast<unsigned>(key);
            SeqState const* saved = g.trig.r().seqs.find(seqKey);
            float const last = saved ? saved->last : -1.f;
            int count = saved ? saved->count : 0;
            float const sinceLast = static_cast<float>(g.time2 - static_cast<double>(last));
            float const minInterval = static_cast<float>(w.prop(o, 437));
            if (!(minInterval <= 0.f || last < 0.f || minInterval <= sinceLast)) return;
            float const resetTime = static_cast<float>(w.prop(o, 438));
            if (0 < count && 0.f < resetTime && 0.f <= last && resetTime < sinceLast) {
                if (static_cast<int>(w.prop(o, 439)) == 0)
                    count = 0;
                else {
                    float const reduced = static_cast<float>(count) - std::floor(sinceLast / resetTime);
                    count = static_cast<int>(reduced >= 0.f ? reduced : 0.f);
                }
            }
            SeqState& state = g.trig.w().seqs[seqKey];
            state.last = static_cast<float>(g.time2);
            int total = 0;
            for (size_t k = 1; k < list->size(); k += 2) total += static_cast<int>((*list)[k]);
            ++count;
            if (total < count) {
                int const mode = static_cast<int>(w.prop(o, 436));
                if (mode == 0) return;
                count = mode == 1 ? 1 : (mode == 2 ? total : count);
            }
            int group = 0;
            int sum = 0;
            for (size_t k = 0; k + 1 < list->size(); k += 2) {
                sum += static_cast<int>((*list)[k + 1]);
                if (count <= sum) {
                    group = static_cast<int>((*list)[k]);
                    break;
                }
            }
            state.count = count;
            group = remapForActivation(g, idx, group);
            spawnGroupLayer(g, group, false, -0.0, o.uid, o.controlID, keys);
        }

        // instant collision (3609): spawns the true or false group whether both blocks (or players) are touching
        void activateInstantCollision(Game& g, int idx, Obj const& o) {
            auto inContact = [&](int a, int b) {
                if (a > 10001) return false;
                int const high = std::max(a, b);
                int const low = std::min(a, b);
                return g.trig.collided.find((high + 1000) * 10000 + low) != nullptr;
            };
            bool result;
            if (o.tf(Obj::TF_FollowCPP))
                result = g.trig.collided.find((10001 + 1000) * 10000 + 10000) != nullptr;
            else {
                bool const targetP1 = o.tf(Obj::TF_TargetP1);
                bool const targetP2 = o.tf(Obj::TF_TargetP2);
                int const first = targetP1 ? 10000 : (targetP2 ? 10001 : o.itemID);
                result = inContact(first, o.itemID2);
                if (!result && targetP1 && targetP2) result = inContact(10001, o.itemID2);
            }
            RemapKeys const* keys = g.world->prop(o, 600, 0.0) != 0.0 ? activationKeys(idx) : nullptr;
            spawnGroupLayer(g, result ? o.targetGroup : o.centerGroup, false, 0.0, o.uid, o.controlID, keys);
        }

        // EnterEffectInstance::loadValuesFromObject: loads the 28 values of its area trigger
        void areaLoadValues(World const& w, AreaInst& a) {
            auto const& areaDef = w.areaDefs[static_cast<size_t>(a.def)];
            for (int k = 0; k < 28; ++k) a.v[k] = areaDef.v[k];
        }

        // addAreaEffect of the game: one instance per trigger, target, center and control
        void areaAdd(Game& g, int defIdx) {
            World const& w = *g.world;
            auto const& areaDef = w.areaDefs[static_cast<size_t>(defIdx)];
            Obj const& o = objAt(g, areaDef.idx);
            int const target =
                    areaDef.target != o.targetGroup && w.extOf(w.objs[static_cast<size_t>(areaDef.idx)])->has(226)
                            ? areaDef.target
                            : o.targetGroup;
            int const center = o.centerGroup;
            int const control = o.controlID;
            if (!w.relevantGroup(target) && (areaDef.effectID <= 0 || !w.areaEffectLiveAt(areaDef.effectID))) return;
            TrigHeavy& heavy = g.trig.w();
            auto& list = heavy.areasW(areaDef.kind);
            for (auto& inst : list) {
                if (inst.def == defIdx && inst.target == target && inst.center == center && inst.control == control) {
                    areaLoadValues(w, inst);
                    return;
                }
            }
            if (0 < areaDef.effectID) {
                for (size_t i = 0; i < list.size(); ++i)
                    if (w.areaDefs[static_cast<size_t>(list[i].def)].effectID == areaDef.effectID) {
                        list.erase(list.begin() + static_cast<long>(i));
                        break;
                    }
            }
            AreaInst inst;
            inst.def = defIdx;
            inst.counter = ++heavy.areaCounter;
            inst.target = target;
            inst.center = center;
            inst.control = control;
            areaLoadValues(w, inst);
            list.push_back(inst);
            heavy.areasUnsorted = static_cast<uint8_t>(heavy.areasUnsorted | (1u << areaDef.kind));
            if (target > 0 && target <= 9999) markGroupMoved(heavy, static_cast<short>(target));
            heavy.anyMotion = true;
        }

        // EnterEffectInstance::loadTransitions: each value that isn't -99 slides to the edit value
        void areaLoadTransitions(World const& w, AreaInst& a, World::AreaDef const& editDef) {
            for (int k = 0; k < 28; ++k) {
                float const to = editDef.v[k];
                if (to == -99.f) continue;
                if (!(0.f < editDef.duration)) {
                    a.v[k] = to;
                    a.a[k].on = 0;
                    continue;
                }
                AreaAnim& anim = a.a[k];
                anim.from = a.v[k];
                anim.to = to;
                anim.dur = editDef.duration;
                anim.elapsed = 0.f;
                anim.easing = editDef.easeInType;
                anim.rate = editDef.easeInRate;
                anim.on = 1;
            }
            (void)w;
        }

        // edit area trigger: changes the instances of the target areas (or of an effect id)
        void areaEdit(Game& g, int defIdx) {
            World const& w = *g.world;
            auto const& editDef = w.areaDefs[static_cast<size_t>(defIdx)];
            TrigHeavy const& heavy = g.trig.r();
            int const target = objAt(g, editDef.idx).targetGroup;
            if (!editDef.useEffectID) {
                for (int idx : w.allOfGroup(target)) {
                    int const areaDefIdx = w.areaDefOf[static_cast<size_t>(idx)];
                    if (areaDefIdx < 0 || w.areaDefs[static_cast<size_t>(areaDefIdx)].edit) continue;
                    int const kind = w.areaDefs[static_cast<size_t>(areaDefIdx)].kind;
                    bool any = false;
                    for (auto const& a : heavy.areas[kind])
                        if (a.def == areaDefIdx) {
                            any = true;
                            break;
                        }
                    if (!any) continue;
                    for (auto& a : g.trig.w().areasW(kind))
                        if (a.def == areaDefIdx) areaLoadTransitions(w, a, editDef);
                }
            } else {
                for (int kind = 0; kind < 3; ++kind) {
                    auto const& list = g.trig.r().areas[kind];
                    for (size_t i = 0; i < list.size(); ++i) {
                        if (w.areaDefs[static_cast<size_t>(list[i].def)].effectID != target) continue;
                        areaLoadTransitions(w, g.trig.w().areasW(kind)[i], editDef);
                        if (0 < target) break;
                    }
                }
            }
        }

        // 0 stop, 1 pause, 2 resume on everything a trigger started, by control id (>= 0) or by trigger uid
        void controlCommands(Game& g, int uid, int control, int cmd) {
            TrigHeavy& heavy = g.trig.w();
            for (auto& groupCmd : heavy.Mw().cmds) {
                bool const match = control >= 0 ? (groupCmd.control == control) : (groupCmd.uid == uid);
                if (!match) continue;
                if (cmd == 0)
                    groupCmd.finishRelated = true;
                else if (cmd == 1)
                    groupCmd.disabled = true;
                else if (cmd == 2)
                    groupCmd.disabled = false;
            }
            for (size_t i = 0; i < heavy.spawnQueue.size();) {
                auto& spawn = heavy.spawnQueue[i];
                bool const match = control >= 0 ? (spawn.control == control) : (spawn.uid == uid);
                if (!match) {
                    ++i;
                    continue;
                }
                if (cmd == 0) {
                    heavy.spawnQueue.erase(heavy.spawnQueue.begin() + static_cast<long>(i));
                    continue;
                }
                spawn.paused = (cmd == 1);
                ++i;
            }
            for (size_t i = 0; i < heavy.counts.size();) {
                auto& count = heavy.counts[i];
                bool const match = control >= 0 ? (count.control == control) : (count.uid == uid);
                if (!match) {
                    ++i;
                    continue;
                }
                if (cmd == 0) {
                    heavy.counts.erase(heavy.counts.begin() + static_cast<long>(i));
                    continue;
                }
                count.paused = (cmd == 1);
                ++i;
            }
            for (size_t i = 0; i < heavy.timers.size();) {
                auto& timer = heavy.timers[i];
                bool const match = control >= 0 ? (timer.control == control) : (timer.uid == uid);
                if (!match) {
                    ++i;
                    continue;
                }
                if (cmd == 0) {
                    heavy.timers.erase(heavy.timers.begin() + static_cast<long>(i));
                    continue;
                }
                timer.disabled = (cmd == 1);
                ++i;
            }
            for (size_t i = 0; i < heavy.timeEvents.size();) {
                auto& event = heavy.timeEvents[i];
                bool const match = control >= 0 ? (event.control == control) : (event.uid == uid);
                if (!match) {
                    ++i;
                    continue;
                }
                if (cmd == 0) {
                    heavy.timeEvents.erase(heavy.timeEvents.begin() + static_cast<long>(i));
                    continue;
                }
                event.done = (cmd == 1);
                ++i;
            }
            if (!heavy.M().drots.empty()) {
                bool any = false;
                for (auto const& rot : heavy.M().drots) {
                    if (control >= 0 ? (rot.control == control) : (rot.uid == uid)) {
                        any = true;
                        break;
                    }
                }
                if (any)
                    for (auto& rot : heavy.Mw().drots) {
                        if (!(control >= 0 ? (rot.control == control) : (rot.uid == uid))) continue;
                        if (cmd == 0)
                            rot.stop = 1;
                        else if (cmd == 1) {
                            rot.pause = 1;
                            rot.pauseOnce = 1;
                        } else if (cmd == 2) {
                            rot.pause = 0;
                            rot.pauseOnce = 0;
                        }
                    }
            }
            if (!heavy.events.items.empty()) {
                bool any = false;
                for (auto const& e : heavy.events.items) {
                    for (auto const& listener : e.second) {
                        if (control >= 0 ? (listener.control == control) : (listener.uid == uid)) {
                            any = true;
                            break;
                        }
                    }
                    if (any) break;
                }
                if (any) {
                    auto& events = g.trig.w().events;
                    for (auto& e : events.items) {
                        auto& listeners = e.second;
                        if (cmd == 0) {
                            listeners.erase(std::remove_if(listeners.begin(), listeners.end(),
                                                           [&](EventListener const& listener) {
                                                               return control >= 0 ? (listener.control == control)
                                                                                   : (listener.uid == uid);
                                                           }),
                                            listeners.end());
                        } else
                            for (auto& listener : listeners) {
                                if (control >= 0 ? (listener.control == control) : (listener.uid == uid)) {
                                    listener.paused = (cmd == 1) ? 1 : 0;
                                }
                            }
                    }
                }
            }
            if (!heavy.M().advs.empty())
                for (auto& adv : heavy.Mw().advs) {
                    int const advUid =
                            g.world->objs[static_cast<size_t>(g.world->advDefs[static_cast<size_t>(adv.def)].idx)].uid;
                    bool const match = control >= 0 ? (adv.control == control) : (advUid == uid);
                    if (!match) continue;
                    if (cmd == 0) {
                        adv.stopped = 1;
                        adv.finished = 0;
                        adv.doStart = 0;
                    } else if (cmd == 1) {
                        adv.finished = 1;
                        adv.doStart = 1;
                    } else {
                        adv.finished = 0;
                        adv.doStart = 0;
                    }
                }
            for (auto& collision : heavy.collisions) {
                bool const match = control >= 0 ? (collision.control == control) : (collision.uid == uid);
                if (!match) continue;
                if (cmd == 0)
                    collision.stopped = true;
                else
                    collision.paused = (cmd == 1);
            }
            for (auto& touch : heavy.touches) {
                bool const match = control >= 0 ? (touch.control == control) : (touch.uid == uid);
                if (!match) continue;
                if (cmd == 0)
                    touch.stopped = true;
                else
                    touch.paused = (cmd == 1);
            }
            for (auto& tween : g.trig.tweens.items) {
                bool const match = control >= 0 ? (tween.second.control == control) : (tween.second.uid == uid);
                if (!match) continue;
                tl_sideEffect = true;
                if (cmd == 0)
                    tween.second.finished = true;
                else
                    tween.second.paused = (cmd == 1);
            }
        }

        // controlAreaEffect of the game: 0 removes the instances of the area, 1 pause, 2 resume
        void controlArea(Game& g, int defIdx, int cmd) {
            World const& w = *g.world;
            int const kind = w.areaDefs[static_cast<size_t>(defIdx)].kind;
            bool any = false;
            for (auto const& a : g.trig.r().areas[kind])
                if (a.def == defIdx) {
                    any = true;
                    break;
                }
            if (!any) return;
            auto& list = g.trig.w().areasW(kind);
            for (size_t i = 0; i < list.size();) {
                if (list[i].def != defIdx) {
                    ++i;
                    continue;
                }
                if (cmd == 0) {
                    list.erase(list.begin() + static_cast<long>(i));
                    continue;
                }
                list[i].paused = cmd == 1;
                ++i;
            }
        }

        // same for every area instance with this control id
        void controlAreaWithControl(Game& g, int control, int cmd) {
            for (int kind = 0; kind < 3; ++kind) {
                bool any = false;
                for (auto const& a : g.trig.r().areas[kind])
                    if (a.control == control) {
                        any = true;
                        break;
                    }
                if (!any) continue;
                auto& list = g.trig.w().areasW(kind);
                for (size_t i = 0; i < list.size();) {
                    if (list[i].control != control) {
                        ++i;
                        continue;
                    }
                    if (cmd == 0) {
                        list.erase(list.begin() + static_cast<long>(i));
                        continue;
                    }
                    list[i].paused = cmd == 1;
                    ++i;
                }
            }
        }

        // triggers that a stop / pause aimed at their group can control (m_canBeControlled)
        bool canBeControlled(int id) {
            switch (id) {
            case 29:
            case 30:
            case 105:
            case 744:
            case 899:
            case 900:
            case 901:
            case 915:
            case 1006:
            case 1007:
            case 1268:
            case 1346:
            case 1347:
            case 1595:
            case 1611:
            case 1615:
            case 1812:
            case 1814:
            case 1815:
            case 1913:
            case 1916:
            case 2015:
            case 2067:
            case 2903:
            case 2999:
            case 3006:
            case 3007:
            case 3008:
            case 3009:
            case 3010:
            case 3016:
            case 3033:
            case 3602:
            case 3604:
            case 3614:
            case 3615:
                return true;
            default:
                return false;
            }
        }

        // stop / pause / resume of every trigger in a group
        void controlTriggersInGroup(Game& g, int group, int cmd) {
            for (int idx : g.world->allOfGroup(group)) {
                Obj const& o = objAt(g, idx);
                if (o.cls == 1 && o.id >= 3006 && o.id <= 3008) {
                    int const areaDefIdx = g.world->areaDefOf[static_cast<size_t>(idx)];
                    if (areaDefIdx >= 0) controlArea(g, areaDefIdx, cmd);
                    continue;
                }
                if (o.cls != 1 || !canBeControlled(o.id)) continue;
                controlCommands(g, o.uid, -1, cmd);
            }
        }

        // starts a camera tween under this key
        void tweenValue(Game& g, int key, float from, float to, float duration, int easing, float rate, int uid,
                        int control) {
            Tween tween;
            tween.from = from;
            tween.to = to;
            tween.duration = duration;
            tween.easing = easing;
            tween.rate = rate;
            tween.uid = uid;
            tween.control = control;
            tween.value = from;
            g.trig.tweens[key] = tween;
        }

        // zoom trigger, zoom clamped to [0.4, 3]
        void updateZoom(Game& g, float zoom, float duration, int easing, float rate, int uid, int control) {
            if (zoom <= 0.f) zoom = 1.f;
            float clamped = zoom <= 3.f ? zoom : 3.f;
            if (!(0.4f <= clamped)) clamped = 0.4f;
            g.targetCameraZoom = clamped;
            if (duration <= 0.f) {
                g.trig.tweens.erase(0xe);
                g.cameraZoom = clamped;
            } else
                tweenValue(g, 0xe, g.cameraZoom, clamped, duration, easing, rate, uid, control);
        }

        void updateCameraOffsetX(Game& g, float x, float duration, int easing, float rate, int uid, int control) {
            g.trig.targetOffX = x;
            if (0.f < duration)
                tweenValue(g, 0xf, g.trig.cameraOffsetX, x, duration, easing, rate, uid, control);
            else {
                g.trig.tweens.erase(0xf);
                g.trig.cameraOffsetX = x;
            }
        }

        void updateCameraOffsetY(Game& g, float y, float duration, int easing, float rate, int uid, int control) {
            g.trig.targetOffY = y;
            if (0.f < duration)
                tweenValue(g, 0x10, g.cameraOffsetY, y, duration, easing, rate, uid, control);
            else {
                g.trig.tweens.erase(0x10);
                g.cameraOffsetY = y;
            }
        }

        // handleObjectCollision of the game: fires the collision triggers listening to blocks a and b
        void handleObjectCollision(Game& g, bool entered, int a, int b) {
            for (size_t i = 0; i < g.trig.r().collisions.size(); ++i) {
                CollisionAction const act = g.trig.r().collisions[i];
                if (act.stopped || act.paused || act.onExit == entered) continue;
                if (act.a != a || act.b != b) continue;
                toggleGroupTriggered(g, act.group, act.activate, act.uid, act.control, &act.keys);
            }
        }

        // marks blocks a and b as touching, fires the triggers on the first contact
        void objectsCollided(Game& g, int a, int b) {
            if (a >= 10002 || b >= 10002) return;
            int const high = a < b ? b : a;
            int const low = a < b ? a : b;
            int const key = (high + 1000) * 10000 + low;
            if (auto* state = g.trig.collided.find(key)) {
                *state = 1;
                return;
            }
            handleObjectCollision(g, true, high, low);
            g.trig.collided[key] = 1;
        }

        // collision block against another object, boxes are given if already built
        void checkBlockAgainst(Game& g, int blockIdx, Rect const& blockRect, bool blockOriented, int otherIdx,
                               bool otherIsPlayer, Rect const& otherRect, OBB const* otherBox, OBB const* blockBox) {
            (void)otherIsPlayer;
            Obj const& block = objAt(g, blockIdx);
            Obj const& o = objAt(g, otherIdx);
            if (o.uid == block.uid) return;
            if (block.itemID >= 10002 || o.itemID >= 10002 || !g.world->collisionPairListened(block.itemID, o.itemID)) {
                return;
            }
            GObj other = g.obj(otherIdx);
            if (other.disabled()) return;
            if (!(otherRect.x <= blockRect.x + blockRect.w && blockRect.x <= otherRect.x + otherRect.w &&
                  otherRect.y <= blockRect.y + blockRect.h && blockRect.y <= otherRect.y + otherRect.h))
                return;
            if (blockOriented || other.hasOrientedBox()) {
                OBB const blockShape = blockBox ? *blockBox : objectObb(g.obj(blockIdx));
                OBB const otherShape = otherBox ? *otherBox : objectObb(other);
                if (!blockShape.overlaps1Way(otherShape) || !otherShape.overlaps1Way(blockShape)) return;
            }
            objectsCollided(g, block.itemID, o.itemID);
        }

    }

    // starts a camera tween, for the other files
    void trigTweenValue(Game& g, int key, float from, float to, float duration, int easing, float rate, int uid,
                        int control) {
        tweenValue(g, key, from, to, duration, easing, rate, uid, control);
    }

}
