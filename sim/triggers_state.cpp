// Hashes, equality and memo of the trigger state, used by the search to share work between equal states, plus
// the reset, begin-of-step and timer updates.

#include "triggers_internal.hpp"

namespace ap::sim {
    namespace trigger_impl {
        // true if both dyn states have the same content, excluded objects aside when excl is set
        bool dynSame(TrigDyn const& a, TrigDyn const& b, void const* excl) {
            if (&a == &b || a.ver == b.ver) return true;
            if (!excl) return trigDynEqual(a, b);
            std::vector<uint64_t> const& exclWords = exclBits(*static_cast<std::vector<int> const*>(excl));
            size_t const wordCount = exclWords.size();
            auto isExcluded = [&](int k) {
                size_t const word = static_cast<size_t>(k) >> 6;
                return k >= 0 && word < wordCount && ((exclWords[word] >> (k & 63)) & 1u);
            };
            auto const& itemsA = a.objDyn.items;
            auto const& itemsB = b.objDyn.items;
            size_t i = 0;
            size_t j = 0;
            size_t sizeA = itemsA.size();
            size_t sizeB = itemsB.size();
            if (sizeA == sizeB) {
                size_t k = 0;
                for (; k < sizeA; ++k) {
                    if (itemsA[k].first != itemsB[k].first) break;
                    if (!podEq(itemsA[k].second, itemsB[k].second) && !isExcluded(itemsA[k].first)) return false;
                }
                if (k == sizeA) return true;
                i = j = k;
            }
            for (;;) {
                while (i < sizeA && isExcluded(itemsA[i].first)) ++i;
                while (j < sizeB && isExcluded(itemsB[j].first)) ++j;
                if (i == sizeA || j == sizeB) return i == sizeA && j == sizeB;
                if (itemsA[i].first != itemsB[j].first || !podEq(itemsA[i].second, itemsB[j].second)) return false;
                ++i;
                ++j;
            }
        }

        // applies the dyn writes (object, new value) of a memo entry to the state
        void applyDynW(TrigState& trig, std::vector<std::pair<int, ObjDyn>> const& writes) {
            if (writes.empty()) return;
            TrigDyn& dyn = trig.wd();
            for (auto const& e : writes) {
                if (ObjDyn* found = dyn.objDyn.find(e.first)) {
                    *found = e.second;
                    continue;
                }
                dyn.objDyn[e.first] = e.second;
                if (tl_applyGame) {
                    auto& dynGroups = dyn.dynGroups;
                    for (short gr : objAt(*tl_applyGame, e.first).groups) {
                        if (gr < 0) continue;
                        size_t const word = static_cast<size_t>(gr) >> 6;
                        if (dynGroups.size() <= word) dynGroups.resize(word + 1, 0);
                        dynGroups[word] |= 1ull << (gr & 63);
                    }
                }
            }
        }

        // entries added or changed from a (before) to b (after), false if one was removed
        bool dynDiff(TrigDyn const& a, TrigDyn const& b, std::vector<std::pair<int, ObjDyn>>& out) {
            out.clear();
            if (&a == &b) return true;
            if (a.afPhys.size() != b.afPhys.size() || a.afObjs != b.afObjs) return false;
            auto const& itemsA = a.objDyn.items;
            auto const& itemsB = b.objDyn.items;
            size_t i = 0;
            for (auto const& e : itemsB) {
                while (i < itemsA.size() && itemsA[i].first < e.first) return false;
                if (i < itemsA.size() && itemsA[i].first == e.first) {
                    if (std::memcmp(&itemsA[i].second, &e.second, sizeof(ObjDyn)) != 0) out.push_back(e);
                    ++i;
                } else
                    out.push_back(e);
            }
            return i == itemsA.size();
        }

        // hash of the motion part, cached in it
        uint64_t motionHashOf(TrigMotion const& motion) {
            if (motion.hashValid) return motion.hashValue;
            uint64_t hash = 0x13198a2e03707344ull;
            hash = mix64(hash, motion.groupOff.size());
            for (auto const& e : motion.groupOff.items) {
                hash = mix64(hash, static_cast<uint64_t>(static_cast<uint16_t>(e.first)));
                hash = hashPod(hash, e.second);
            }
            hash = hashVec(hash, motion.cmds);
            if (motion.pset) hash = mix64(hash, motion.pset->sig);
            hash = mix64(hash, motion.cellOv.size());
            for (auto const& e : motion.cellOv.items) {
                hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(e.first)));
                auto const& cell = e.second;
                if (!cell.valid) {
                    uint64_t cellHash = 0x9e3779b97f4a7c15ull;
                    if (cell.v) {
                        cellHash = mix64(cellHash, cell.v->size());
                        for (int v : *cell.v) {
                            cellHash = mix64(cellHash, static_cast<uint64_t>(static_cast<uint32_t>(v)));
                        }
                    }
                    cell.hash = cellHash;
                    cell.valid = true;
                }
                hash = mix64(hash, cell.hash);
            }
            if (!motion.movedHashValid) {
                uint64_t movedHash = mix64(0xbe5466cf34e90c6cull, motion.movedGroups.size());
                for (uint64_t bits : motion.movedGroups) movedHash = mix64(movedHash, bits);
                motion.movedHash = movedHash;
                motion.movedHashValid = true;
            }
            hash = mix64(hash, motion.movedHash);
            if (!motion.advs.empty() || motion.advOrder) {
                hash = hashVec(hash, motion.advs);
                hash = mix64(hash, static_cast<uint64_t>(motion.advOrder));
                hash = mix64(hash, motion.advDirty ? 1u : 0u);
            }
            if (!motion.drots.empty()) hash = mix64(hashVec(hash, motion.drots), 0x5d0e1a3bull);
            if (!motion.kfSnap.empty()) hash = mix64(hashVec(hash, motion.kfSnap), 0x2f1d6b07ull);
            if (!motion.blkCell.items.empty()) {
                hash = mix64(hash, motion.blkCell.size());
                for (auto const& e : motion.blkCell.items) {
                    hash = mix64(mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(e.first))),
                                 static_cast<uint64_t>(static_cast<uint32_t>(e.second)));
                }
            }
            motion.hashValue = hash;
            motion.hashValid = true;
            return hash;
        }

    }

    // hash of one remapKeys entry, summed so it updates in O(1)
    uint64_t remapEntryHash(int idx, RemapKeys const& keys) {
        uint64_t hash = mix64(0x3c6ef372fe94f82bull, static_cast<uint64_t>(static_cast<uint32_t>(idx)));
        hash = mix64(hash, keys.n);
        for (int v : keys) hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(v)));
        return mix64(hash, 0x9b05688c2b3e6c1full);
    }

    // hash of the area instances, cached in the shared lists
    uint64_t trigAreasHash(TrigHeavy const& H) {
        AreaLists const& lists = *H.areas.p;
        if (lists.hashValid.load(std::memory_order_acquire)) return lists.hash.load(std::memory_order_relaxed);
        uint64_t hash = 0x6a09e667f3bcc909ull;
        for (int k = 0; k < 3; ++k) hash = hashVec(hash, lists.L[k]);
        lists.hash.store(hash, std::memory_order_relaxed);
        lists.hashValid.store(true, std::memory_order_release);
        return hash;
    }

    // hash of the whole heavy state, cached in it
    uint64_t trigHeavyHash(TrigHeavy const& H) {
        if (H.hashValid) return H.hashValue;
        ++tl_hashes;
        uint64_t hash = 0x243f6a8885a308d3ull;
        hash = mix64(hash, motionHashOf(H.M()));
        hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(H.offCount)));
        for (uint64_t bits : H.offBits) hash = mix64(hash, bits);
        hash = hashVec(hash, H.spawnQueue);
        hash = hashVec(hash, H.counts);
        hash = hashVec(hash, H.collisions);
        hash = hashVec(hash, H.touches);
        hash = mix64(hash, trigAreasHash(H));
        hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(H.areaCounter)) |
                                   (static_cast<uint64_t>(H.areasUnsorted) << 40));
        hash = mix64(hash, H.items.size());
        for (auto const& e : H.items.items) {
            hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(e.first)));
            hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(e.second)));
        }
        hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(H.spawnCursor)));
        for (auto const& e : H.spawnCursors.items) {
            hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(e.first)));
            hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(e.second)));
        }
        hash = mix64(hash, H.anyMotion ? 1u : 0u);
        if (!H.actHashValid) {
            uint64_t actHash = mix64(0x452821e638d01377ull, H.actBits.size());
            for (uint64_t bits : H.actBits) actHash = mix64(actHash, bits);
            H.actHash = actHash;
            H.actHashValid = true;
        }
        hash = mix64(hash, H.actHash);
        hash = mix64(hash, H.remapKeys.size());
        hash = mix64(hash, H.remapSum);
        hash = hashVec(hash, H.timers);
        hash = hashVec(hash, H.timeEvents);
        hash = mix64(hash, H.timerBuckets);
        hash = mix64(hash, H.seqs.size());
        for (auto const& e : H.seqs.items) {
            hash = mix64(hash, static_cast<uint64_t>(e.first));
            hash = hashPod(hash, e.second);
        }
        hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(H.points)));
        hash = mix64(hash, 0x9e1100000000ull | H.persistItems.size());
        for (int v : H.persistItems) hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(v)) ^ 0x9e11ull);
        hash = mix64(hash, 0x9e1200000000ull | H.persistTimers.size());
        for (int v : H.persistTimers) hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(v)) ^ 0x9e12ull);
        hash = mix64(hash, H.events.size());
        for (auto const& e : H.events.items) {
            hash = mix64(hash, static_cast<uint64_t>(e.first));
            hash = hashVec(hash, e.second);
        }
        hash = mix64(hash, H.spawnStored.size());
        for (auto const& e : H.spawnStored.items) {
            hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(e.first)));
            hash = mix64(hash, bits64(e.second));
        }
        H.hashValue = hash;
        H.hashValid = true;
        return hash;
    }

    namespace trigger_impl {
        // hash of the motion part, plus the turned off groups if withOff
        uint64_t motionHash(TrigHeavy const& heavy, bool withOff) {
            uint64_t hash = motionHashOf(heavy.M());
            if (withOff) {
                hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(heavy.offCount)));
                for (uint64_t bits : heavy.offBits) hash = mix64(hash, bits);
            }
            return hash;
        }

    }

    // offset of a group, false if it never moved
    bool trigGroupOffset(Game const& g, int group, double& dx, double& dy) {
        dx = dy = 0.0;
        if (!g.trig.on || !g.trig.h || group <= 0 || group > 32767) return false;
        if (GroupOff const* off = offFind(g, static_cast<short>(group))) {
            dx = off->x;
            dy = off->y;
            return true;
        }
        return false;
    }

    // true if the trigger has an activated bit slot
    bool trigHasSlot(Game const& g, int idx) {
        return g.trig.on && idx >= 0 && static_cast<size_t>(idx) < g.world->trigSlot.size() &&
               g.world->trigSlot[static_cast<size_t>(idx)] >= 0;
    }

    // number of persistent items that changed from their start value
    int trigPersistChanged(Game const& g) {
        World const& w = *g.world;
        if (w.persistItemIds.empty() || !g.trig.on || !g.trig.h) return 0;
        int changed = 0;
        for (int id : w.persistItemIds) {
            int start = 0;
            for (auto const& e : w.persistentItems)
                if (e.first == id) {
                    start = e.second;
                    break;
                }
            auto const* value = g.trig.r().items.find(id);
            if ((value ? *value : 0) != start) ++changed;
        }
        return changed;
    }

    // true if the trigger was activated (bit version)
    bool trigActivatedBit(Game const& g, int idx) {
        if (!trigHasSlot(g, idx)) return false;
        int const slot = g.world->trigSlot[static_cast<size_t>(idx)];
        auto const& bits = g.trig.r().actBits;
        size_t const word = static_cast<size_t>(slot) >> 6;
        return word < bits.size() && ((bits[word] >> (slot & 63)) & 1u);
    }

    // marks the trigger as activated (bit version)
    void trigSetActivatedBit(Game& g, int idx) {
        if (!trigHasSlot(g, idx)) return;
        int const slot = g.world->trigSlot[static_cast<size_t>(idx)];
        if (trigActivatedBit(g, idx)) return;
        auto& bits = g.trig.w().actBits;
        size_t const word = static_cast<size_t>(slot) >> 6;
        if (bits.size() <= word) bits.resize(word + 1, 0);
        bits[word] |= 1ull << (slot & 63);
        g.trig.w().actHashValid = false;
    }

    thread_local long g_trigCounters[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    thread_local int g_noShare = 0;

    // hash of the dyn state, cached in it
    uint64_t trigDynHash(TrigDyn const& D) {
        if (D.hashValid) return D.hashValue;
        ++g_trigCounters[2];
        uint64_t hash = 0x13198a2e03707344ull;
        hash = mix64(hash, D.objDyn.size());
        for (auto const& e : D.objDyn.items) {
            hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(e.first)));
            hash = hashPod(hash, e.second);
        }
        if (!D.afObjs.empty() || D.afPhys.size() != 0) {
            hash = mix64(hash, D.afObjs.size());
            for (int i : D.afObjs) hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(i)));
            hash = mix64(hash, D.afPhys.size());
            for (auto const& e : D.afPhys.items) {
                hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(e.first)));
                hash = hashPod(hash, e.second);
            }
        }
        D.hashValue = hash;
        D.hashValid = true;
        return hash;
    }

    // true if both dyn states have the same content
    bool trigDynEqual(TrigDyn const& a, TrigDyn const& b) {
        if (&a == &b) return true;
        if (a.hashValid && b.hashValid) return a.hashValue == b.hashValue;
        if (a.objDyn.size() != b.objDyn.size() || a.afObjs != b.afObjs || a.afPhys.size() != b.afPhys.size()) {
            return false;
        }
        auto const& itemsA = a.objDyn.items;
        auto const& itemsB = b.objDyn.items;
        for (size_t i = 0, n = itemsA.size(); i < n; ++i)
            if (itemsA[i].first != itemsB[i].first ||
                std::memcmp(&itemsA[i].second, &itemsB[i].second, sizeof(ObjDyn)) != 0)
                return false;
        auto const& physA = a.afPhys.items;
        auto const& physB = b.afPhys.items;
        for (size_t i = 0, n = physA.size(); i < n; ++i)
            if (physA[i].first != physB[i].first ||
                std::memcmp(&physA[i].second, &physB[i].second, sizeof(AfPhys)) != 0)
                return false;
        return true;
    }

    namespace trigger_impl {
        // hash of the object list of the dyn state only (not the values)
        uint64_t dynKeysHash(TrigDyn const& dyn) {
            if (dyn.keysValid) return dyn.keysValue;
            uint64_t hash = 0xa4093822299f31d0ull;
            hash = mix64(hash, dyn.objDyn.size());
            for (auto const& e : dyn.objDyn.items) {
                hash = mix64(hash, static_cast<uint64_t>(static_cast<uint32_t>(e.first)));
            }
            dyn.keysValue = hash;
            dyn.keysValid = true;
            return hash;
        }

    }

    // small id of the current thread, starting at 1
    int trigThreadId() {
        static std::atomic<int> next{1};
        thread_local int const id = next.fetch_add(1);
        return id;
    }

    // gives this thread its own copy of state parts shared by many states of other threads
    void trigLocalize(Game& g, TrigLocalCtx& ctx) {
        TrigState& trig = g.trig;
        int const me = trigThreadId();
        if (trig.h && trig.h->owner != me && trig.h.use_count() > 4) {
            std::shared_ptr<TrigHeavy> local;
            for (auto const& e : ctx.mapH)
                if (e.first == trig.h.get()) {
                    local = e.second;
                    break;
                }
            if (!local) {
                local = std::make_shared<TrigHeavy>(*trig.h);
                local->owner = me;
                ctx.mapH.emplace_back(trig.h.get(), local);
            }
            ctx.deferredH.push_back(std::move(trig.h));
            trig.h = std::move(local);
            ++ctx.localized;
        }
        if (trig.d && trig.d->owner != me && trig.d.use_count() > 4) {
            std::shared_ptr<TrigDyn> local;
            for (auto const& e : ctx.mapD)
                if (e.first == trig.d.get()) {
                    local = e.second;
                    break;
                }
            if (!local) {
                local = std::make_shared<TrigDyn>(*trig.d);
                ctx.mapD.emplace_back(trig.d.get(), local);
            }
            ctx.deferredD.push_back(std::move(trig.d));
            trig.d = std::move(local);
        }
    }

    // frees what trigLocalize kept
    void trigLocalFlush(TrigLocalCtx& ctx) {
        ctx.mapH.clear();
        ctx.mapD.clear();
        ctx.deferredH.clear();
        ctx.deferredD.clear();
    }

    // resets the trigger state at the start of an attempt, persistent items keep their value
    void trigReset(Game& g) {
        g.trig = TrigState{};
        g.trig.on = g.world && g.world->triggersSimulable();
        if (g.trig.on) {
            g.trig.h = std::make_shared<TrigHeavy>();
            g.trig.d = std::make_shared<TrigDyn>();
            for (auto const& [item, value] : g.world->persistentItems) {
                if (value != 0 && item >= 0 && item <= 9999) g.trig.h->items[item] = value;
            }
            for (auto const& e : g.world->persistentItems) g.trig.h->persistItems.push_back(e.first);
            for (auto const& e : g.world->persistentTimers) g.trig.h->persistTimers.push_back(e.first);
        }
        g.trig.camTrack =
                g.trig.on && !g.world->cameraSimulable() && g.world->timeline && !g.world->timeline->cam.empty();
    }

    // start of a step: clears the per step data and runs the spawn queue (updateSpawnTriggers of the game)
    void trigBeginTick(Game& g, double dtSec, bool secondHalf) {
        TrigState& trig = g.trig;
        memoTick(g);
        if (!secondHalf) trig.spawnedThisTick.clear();
        {
            unsigned const cmdIndex = g.commandIndex;
            auto& stamps = trig.evStamps;
            stamps.erase(std::remove_if(stamps.begin(), stamps.end(),
                                        [cmdIndex](auto const& e) { return e.second != cmdIndex; }),
                         stamps.end());
        }
        for (auto& pair : trig.collided.items) pair.second = 0;
        if (trig.r().spawnQueue.empty()) return;
        bool const mOn = memoOn();
        uint64_t key = 0;
        uint64_t keysBefore = 0;
        std::shared_ptr<TrigDyn> fullBefore;
        if (mOn) {
            key = mix64(mix64(trigHeavyHash(trig.r()), bits64(dtSec)), 0x5109ull);
            if (MemoEntry const* entry = memoLookup(trig, key)) {
                g.itemChanges += entry->itemDelta;
                return;
            }
            keysBefore = dynKeysHash(trig.rd());
            fullBefore = trig.d;
            spawnMark(trig);
        }
        ++tl_dbg[5];
        tl_sideEffect = false;
        tl_dynDep = 0;
        tl_itemDelta = 0;
        tl_memoComp = mOn;
        tl_pHit = false;
        size_t const count = trig.r().spawnQueue.size();
        for (size_t i = 0; i < count; ++i) {
            if (i >= trig.r().spawnQueue.size()) break;
            SpawnAction spawn = trig.r().spawnQueue[i];
            if (!spawn.finished) {
                if (spawn.paused) continue;
                spawn.elapsed += dtSec;
                spawn.finished = spawn.delay <= spawn.elapsed;
                {
                    auto& queued = trig.w().spawnQueue[i];
                    queued.elapsed = spawn.elapsed;
                    queued.finished = spawn.finished;
                }
                if (!spawn.finished) continue;
            }
            double const over = spawn.elapsed - spawn.delay;
            if (spawn.obj < 0) {
                spawnGroupLayer(g, spawn.group, spawn.ordered, over, spawn.uid, spawn.control, &spawn.keys);
            } else
                spawnObject(g, spawn.obj, over, &spawn.keys);
        }
        {
            auto& queue = trig.w().spawnQueue;
            queue.erase(
                    std::remove_if(queue.begin(), queue.end(), [](SpawnAction const& spawn) { return spawn.finished; }),
                    queue.end());
        }
        tl_memoComp = false;
        if (!tl_sideEffect && !tl_pHit) {
            if (mOn) memoCommit(key, trig, keysBefore, fullBefore);
        } else
            ++tl_dbg[6];
    }

    // updateTimers of the game: running timers advance (spawn when they reach their target), then their time events
    void trigUpdateTimers(Game& g, double dtSec) {
        TrigState& trig = g.trig;
        if (trig.r().timers.empty()) return;
        bool const mOn = memoOn();
        uint64_t key = 0;
        uint64_t keysBefore = 0;
        std::shared_ptr<TrigDyn> fullBefore;
        if (mOn) {
            key = mix64(mix64(trigHeavyHash(trig.r()), bits64(dtSec)), 0x7173ull);
            if (MemoEntry const* entry = memoLookup(trig, key)) {
                g.itemChanges += entry->itemDelta;
                return;
            }
            keysBefore = dynKeysHash(trig.rd());
            fullBefore = trig.d;
            spawnMark(trig);
        }
        tl_sideEffect = false;
        tl_dynDep = 0;
        tl_itemDelta = 0;
        tl_memoComp = mOn;
        tl_pHit = false;
        float const dt = static_cast<float>(dtSec);
        static thread_local std::vector<int> ids;
        ids.clear();
        for (auto const& timer : trig.r().timers) ids.push_back(timer.id);
        for (int id : ids) {
            int const index = timerIndex(g, id);
            if (index < 0) continue;
            {
                TimerItem const& timer = trig.r().timers[static_cast<size_t>(index)];
                if (timer.running && !timer.disabled) {
                    double const before = timer.time;
                    double const after = static_cast<double>(dt * timer.mod) + before;
                    TimerItem& writable = trig.w().timers[static_cast<size_t>(index)];
                    writable.time = after;
                    if (writable.stopEnabled && ((before < writable.target && writable.target <= after) ||
                                                 (writable.target < before && after <= writable.target))) {
                        writable.time = writable.target;
                        writable.running = false;
                        if (0 < writable.group) {
                            TimerItem const copy = writable;
                            spawnGroupLayer(g, copy.group, false, 0.0, copy.uid, copy.control, &copy.keys);
                        }
                    }
                }
            }
            int const indexAfter = timerIndex(g, id);
            if (indexAfter < 0) continue;
            for (size_t i = 0; i < trig.r().timeEvents.size();) {
                TimeEvent const event = trig.r().timeEvents[i];
                if (event.timer != id) {
                    ++i;
                    continue;
                }
                TimerItem const current = trig.r().timers[static_cast<size_t>(indexAfter)];
                double const now = current.time;
                if (!event.done) {
                    float const last = event.last;
                    float const target = event.target;
                    bool const fire = (last < target && static_cast<double>(target) <= now && 0.f < current.mod) ||
                                      (target < last && now <= static_cast<double>(target) && current.mod < 0.f);
                    if (fire) {
                        bool erased = false;
                        if (!event.multi) {
                            auto& events = trig.w().timeEvents;
                            events.erase(events.begin() + static_cast<long>(i));
                            erased = true;
                        }
                        spawnGroupLayer(g, event.group, false, 0.0, current.uid, current.control, &current.keys);
                        if (erased) continue;
                    }
                }
                if (i < trig.r().timeEvents.size()) {
                    trig.w().timeEvents[i].last =
                            static_cast<float>(trig.r().timers[static_cast<size_t>(indexAfter)].time);
                }
                ++i;
            }
        }
        tl_memoComp = false;
        if (!tl_sideEffect && !tl_pHit && mOn) memoCommit(key, trig, keysBefore, fullBefore);
    }

    // hash of the active touch triggers, changes when one is added or stopped
    uint64_t trigTouchSig(Game const& g) {
        if (!g.trig.on) return 0;
        uint64_t sig = 0x9e3779b97f4a7c15ull;
        for (TouchAction const& a : g.trig.r().touches) {
            if (!a.stopped && !a.paused) sig = mix64(sig, static_cast<uint64_t>(static_cast<uint32_t>(a.uid)));
        }
        return sig;
    }

    // player pressed or released the button, toggles the groups of touch triggers (GJEffectManager::processTouchToggle)
    void trigButton(Game& g, bool down, bool isPlayer1) {
        TrigState& T = g.trig;
        for (size_t i = 0; i < T.r().touches.size(); ++i) {
            TouchAction const a = T.r().touches[i];
            if (a.stopped || a.paused) continue;
            if (a.dual && isPlayer1) continue;
            if (!(a.playerMode == 0 || (a.playerMode == 1 && isPlayer1) || (a.playerMode == 2 && !isPlayer1))) continue;
            if (!down && !a.hold) continue;
            bool newState = !groupEnabled(g, a.group);
            if (!a.hold) {
                if (a.toggleMode != 0) newState = a.toggleMode == 1;
            } else {
                if (a.toggleMode != 0) newState = (a.toggleMode == 1) ? down : !down;
            }
            if (debugOptions().trigLogAt(g.tick)) {
                std::printf("[touch] tick %d : trigger uid %d, group %d -> %s (%s)\n", g.tick, a.uid, a.group,
                            newState ? "on" : "off", down ? "press" : "release");
            }
            toggleGroupTriggered(g, a.group, newState, a.uid, a.control, &a.keys);
        }
    }

    void trigMemoStats(long& hits, long& miss, long& hashes) {
        hits = tl_memoHits;
        miss = tl_memoMiss;
        hashes = tl_hashes;
    }

    // new search: each thread resets its memo usefulness counters
    void trigMemoNewEpoch() { g_memoEpoch.fetch_add(1, std::memory_order_relaxed); }

    // debug: prints the player part, tracked objects and memo commit counters, returns this thread's tl_dbg
    long const* trigMemoDbg() {
        std::fprintf(stderr, "[player part, all threads] %ld hits, %ld computations\n", g_pStatAll[0].load(),
                     g_pStatAll[1].load());
        std::fprintf(stderr,
                     "[player part] shared : %ld hits, %ld computations ; fallback : %ld misses, %ld direct ; side "
                     "effects %ld ; without memo %ld\n",
                     tl_pStat[0], tl_pStat[1], tl_pStat[2], tl_pStat[3], tl_pStat[4], tl_pStat[5]);
        std::fprintf(stderr,
                     "[tracked objects touched] dynEntry %ld, group %ld, resection %ld, moveGroup %ld, toggle %ld, "
                     "access %ld, rotation %ld\n",
                     tl_fySite[1], tl_fySite[2], tl_fySite[3], tl_fySite[4], tl_fySite[5], tl_fySite[6], tl_fySite[7]);
        std::fprintf(stderr,
                     "[commits refused] full %ld, tracked object touched %ld, difference not representable %ld, no "
                     "state before %ld\n",
                     tl_commitFail[0], tl_commitFail[1], tl_commitFail[2], tl_commitFail[3]);
        return tl_dbg;
    }

    // debug: prints which parts of two heavy states differ
    void trigHeavyDiff(TrigHeavy const& a, TrigHeavy const& b) {
        auto cmpMap = [&](char const* name, auto const& x, auto const& y) {
            uint64_t hashX = mix64(0, x.size());
            uint64_t hashY = mix64(0, y.size());
            for (auto const& e : x.items) {
                hashX = hashPod(hashX, e.first);
                hashX = hashPod(hashX, e.second);
            }
            for (auto const& e : y.items) {
                hashY = hashPod(hashY, e.first);
                hashY = hashPod(hashY, e.second);
            }
            if (hashX != hashY) std::fprintf(stderr, "    differs : %s (%zu / %zu)\n", name, x.size(), y.size());
        };
        cmpMap("groupOff", a.M().groupOff, b.M().groupOff);
        for (auto const& e : a.M().groupOff.items) {
            auto const* other = b.M().groupOff.find(e.first);
            if (!other || hashPod(0, *other) != hashPod(0, e.second)) {
                std::fprintf(stderr, "      group %d : (%.3f, %.3f) / (%.3f, %.3f)\n", e.first, e.second.x, e.second.y,
                             other ? other->x : 0.0, other ? other->y : 0.0);
            }
        }
        for (auto const& e : a.items.items) {
            auto const* other = b.items.find(e.first);
            if (!other || *other != e.second)
                std::fprintf(stderr, "      item %d : %d / %d\n", e.first, e.second, other ? *other : -999);
        }
        if (a.offBits != b.offBits) std::fprintf(stderr, "    differs : offBits\n");
        if (hashVec(0, a.M().cmds) != hashVec(0, b.M().cmds)) {
            std::fprintf(stderr, "    differs : cmds (%zu / %zu)\n", a.M().cmds.size(), b.M().cmds.size());
            for (size_t i = 0; i < std::min(a.M().cmds.size(), b.M().cmds.size()); ++i) {
                if (hashPod(0, a.M().cmds[i]) != hashPod(0, b.M().cmds[i])) {
                    auto const& c = a.M().cmds[i];
                    auto const& d = b.M().cmds[i];
                    std::fprintf(stderr,
                                 "      cmd %zu : uid %d type %d target %d lockPX %d lockPY %d | dx %.3f/%.3f dy "
                                 "%.3f/%.3f curX %.3f/%.3f curY %.3f/%.3f end %d/%d\n",
                                 i, c.uid, c.type, c.target, c.lockPX, c.lockPY, c.deltaX, d.deltaX, c.deltaY, d.deltaY,
                                 c.curX, d.curX, c.curY, d.curY, c.finished, d.finished);
                    break;
                }
            }
        }
        if (hashVec(0, a.spawnQueue) != hashVec(0, b.spawnQueue)) {
            std::fprintf(stderr, "    differs : spawnQueue (%zu / %zu)\n", a.spawnQueue.size(), b.spawnQueue.size());
            for (size_t i = 0; i < std::max(a.spawnQueue.size(), b.spawnQueue.size()); ++i) {
                auto print = [](SpawnAction const* x) {
                    if (!x) {
                        std::fprintf(stderr, " -");
                        return;
                    }
                    std::fprintf(stderr, " [g %d obj %d uid %d delay %.6f el %.6f end %d pause %d ord %d keys %d]",
                                 x->group, x->obj, x->uid, x->delay, x->elapsed, x->finished, x->paused, x->ordered,
                                 x->keys.n);
                    for (int k : x->keys) std::fprintf(stderr, " %d", k);
                };
                std::fprintf(stderr, "      %zu :", i);
                print(i < a.spawnQueue.size() ? &a.spawnQueue[i] : nullptr);
                std::fprintf(stderr, " /");
                print(i < b.spawnQueue.size() ? &b.spawnQueue[i] : nullptr);
                std::fprintf(stderr, "\n");
            }
        }
        if (hashVec(0, a.counts) != hashVec(0, b.counts)) std::fprintf(stderr, "    differs : counts\n");
        if (hashVec(0, a.collisions) != hashVec(0, b.collisions)) std::fprintf(stderr, "    differs : collisions\n");
        if (hashVec(0, a.touches) != hashVec(0, b.touches)) std::fprintf(stderr, "    differs : touches\n");
        cmpMap("items", a.items, b.items);
        if (a.M().cellOv.size() != b.M().cellOv.size()) {
            std::fprintf(stderr, "    differs : cellOv (%zu / %zu)\n", a.M().cellOv.size(), b.M().cellOv.size());
        }
        if (a.spawnCursor != b.spawnCursor)
            std::fprintf(stderr, "    differs : spawnCursor %d / %d\n", a.spawnCursor, b.spawnCursor);
        if (a.actBits != b.actBits) std::fprintf(stderr, "    differs : actBits\n");
        if (a.M().movedGroups != b.M().movedGroups) std::fprintf(stderr, "    differs : movedGroups\n");
        if (hashVec(0, a.timers) != hashVec(0, b.timers)) std::fprintf(stderr, "    differs : timers\n");
        if (a.points != b.points) std::fprintf(stderr, "    differs : points\n");
        for (int k = 0; k < 3; ++k) {
            if (hashVec(0, a.areas[k]) != hashVec(0, b.areas[k])) std::fprintf(stderr, "    differs : areas[%d]\n", k);
        }
    }

    // full hash of the trigger state, used to merge identical states in the search
    uint32_t trigHash(Game const& g) {
        TrigState const& T = g.trig;
        if (!T.on || !T.h) return 0;
        uint64_t h = trigHeavyHash(*T.h);
        if (T.d) h = mix64(h, trigDynHash(*T.d));
        return trigHashLight(g, h);
    }

    // hash without the dyn state and the player part, those are compared later only when this matches
    uint32_t trigHashMain(Game const& g) {
        TrigState const& T = g.trig;
        if (!T.on || !T.h) return 0;
        return trigHashLightNoPv(g, trigHeavyHash(*T.h));
    }

    namespace trigger_impl {
        // hash of the player part, cached
        uint64_t pvHash(TrigPv const& P) {
            if (P.hashValid) return P.hashValue;
            uint64_t h = 0x3c6ef372fe94f82bull;
            h = mix64(h, P.off.size());
            for (auto const& e : P.off.items) {
                h = mix64(h, static_cast<uint64_t>(static_cast<uint16_t>(e.first)));
                h = hashPod(h, e.second);
            }
            h = mix64(h, P.dyn.size());
            for (auto const& e : P.dyn.items) {
                h = mix64(h, static_cast<uint64_t>(static_cast<uint32_t>(e.first)));
                h = hashPod(h, e.second);
            }
            h = mix64(h, P.af.size());
            for (auto const& e : P.af.items) {
                h = mix64(h, static_cast<uint64_t>(static_cast<uint32_t>(e.first)));
                h = hashPod(h, e.second);
            }
            h = mix64(h, P.afObjs.size());
            for (int i : P.afObjs) h = mix64(h, static_cast<uint64_t>(static_cast<uint32_t>(i)));
            if (!P.kfSnap.empty()) h = mix64(hashVec(h, P.kfSnap), 0x6b8b4567ull);
            P.hashValue = h;
            P.hashValid = true;
            return h;
        }

    }

    // same player parts, compared byte by byte (stops at the first difference)
    bool trigPvEqual(TrigState const& a, TrigState const& b) {
        TrigPv const* pa = a.pv.get();
        TrigPv const* pb = b.pv.get();
        if (pa == pb) return true;
        if (pvEmpty(pa) || pvEmpty(pb)) return pvEmpty(pa) && pvEmpty(pb);
        if (pa->off.items.size() != pb->off.items.size() || pa->dyn.items.size() != pb->dyn.items.size() ||
            pa->af.items.size() != pb->af.items.size() || pa->afObjs != pb->afObjs ||
            pa->kfSnap.size() != pb->kfSnap.size() ||
            (!pa->kfSnap.empty() &&
             std::memcmp(pa->kfSnap.data(), pb->kfSnap.data(), pa->kfSnap.size() * sizeof(pa->kfSnap[0])) != 0)) {
            return false;
        }
        for (size_t i = 0; i < pa->off.items.size(); ++i) {
            if (pa->off.items[i].first != pb->off.items[i].first ||
                std::memcmp(&pa->off.items[i].second, &pb->off.items[i].second, sizeof(GroupOff)) != 0)
                return false;
        }
        for (size_t i = 0; i < pa->dyn.items.size(); ++i) {
            if (pa->dyn.items[i].first != pb->dyn.items[i].first ||
                std::memcmp(&pa->dyn.items[i].second, &pb->dyn.items[i].second, sizeof(ObjDyn)) != 0)
                return false;
        }
        for (size_t i = 0; i < pa->af.items.size(); ++i) {
            if (pa->af.items[i].first != pb->af.items[i].first ||
                std::memcmp(&pa->af.items[i].second, &pb->af.items[i].second, sizeof(AfPhys)) != 0)
                return false;
        }
        return true;
    }

    // adds the player part and the light part to h
    uint32_t trigHashLight(Game const& g, uint64_t h) {
        TrigState const& T = g.trig;
        if (!pvEmpty(T.pv.get())) h = mix64(h, pvHash(*T.pv));
        return trigHashLightNoPv(g, h);
    }

    // adds the light part (y history, collisions, tweens, camera, rng...) to h, without the player part
    uint32_t trigHashLightNoPv(Game const& g, uint64_t h) {
        TrigState const& T = g.trig;
        if (!T.yHist.empty()) {
            for (float v : T.yHist) h = mix64(h, bits64(v));
            h = mix64(h, static_cast<uint64_t>(static_cast<uint32_t>(T.yHistIdx)));
            h = mix64(h, bits64(T.yHistAcc));
        }
        h = mix64(h, T.collided.size());
        for (auto const& e : T.collided.items) {
            h = mix64(h, static_cast<uint64_t>(static_cast<uint32_t>(e.first)) * 2u + e.second);
        }
        h = mix64(h, bits64(T.queuedTimeWarp));
        for (auto const& e : T.stateBlk) {
            h = mix64(h,
                      (static_cast<uint64_t>(static_cast<uint32_t>(e.first)) << 32) | static_cast<uint32_t>(e.second));
        }
        if (!T.afRing.empty()) {
            h = mix64(mix64(mix64(h, T.afHashObj ^ T.afHashP), (static_cast<uint64_t>(T.afU5) << 32) | T.afU6),
                      static_cast<uint64_t>(static_cast<uint32_t>(T.afN)));
        }
        for (auto const& e : T.evStamps) {
            if (e.second == g.commandIndex) h = mix64(h, static_cast<uint64_t>(e.first) ^ 0x5eedull);
        }
        for (auto const& e : T.tweens.items) {
            h = mix64(h, static_cast<uint64_t>(static_cast<uint32_t>(e.first)));
            h = hashPod(h, e.second);
        }
        if (g.mirrorOn || g.mirrorT != 0.f || g.mirrorDir != 1.f) {
            h = mix64(mix64(h, bits64(g.mirrorT)),
                      (g.mirrorOn ? 1u : 0u) | (g.mirrorBack ? 2u : 0u) | (g.mirrorDir < 0.f ? 4u : 0u));
        }
        if (g.cam.on) h = mix64(h, cameraHash(g.cam));
        if (g.world->hasRandom) h = mix64(h, g.rng);
        if (g.world->tpMulti && g.world->tpRngKnown) h = mix64(h, static_cast<uint64_t>(g.tpRng) ^ 0x7e1eull);
        return static_cast<uint32_t>(h ^ (h >> 32));
    }

}
