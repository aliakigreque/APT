// Beam search for a full input path through the level, offline and live (several threads). This file has the
// entry point, the setup of a run and its main loop; the other search_*.cpp files have the level maps, the costs
// and the stages of one tick.

#include "search_internal.hpp"

namespace ap::sim {
    namespace search_impl {
        // value of a persistent item when the level starts (0 if not saved)
        int menuStartValue(World const& w, int item) {
            for (auto const& e : w.persistentItems)
                if (e.first == item) return e.second;
            return 0;
        }

    }

    // returns true if the state set a forbidden menu value or touched a low detail mode object
    bool menuViolation(Game const& g) {
        World const& w = *g.world;
        for (int idx : w.ldmTouch) {
            Obj const& o = w.objs[static_cast<size_t>(idx)];
            if (o.cls == 1 && o.touchTriggered) {
                if (g.trig.on && trigActivatedBit(g, idx)) return true;
                continue;
            }
            auto const* state = g.osFind(idx);
            if (state &&
                (state->activatedByPlayer1 || state->activatedByPlayer2 || state->activated || state->destroyed))
                return true;
        }
        for (auto const& menu : w.menus) {
            if (menu.forbid.empty()) continue;
            int const value = trigItemValue(g, menu.item);
            if (value == menuStartValue(w, menu.item)) continue;
            if (std::find(menu.forbid.begin(), menu.forbid.end(), value) != menu.forbid.end()) return true;
        }
        return false;
    }

    // returns true if every wanted menu value is set (and saved when the level can save it)
    bool menuGoalReached(Game const& g) {
        World const& w = *g.world;
        if (!g.trig.on || !g.trig.h) return false;
        bool any = false;
        for (auto const& menu : w.menus) {
            if (!menu.hasPrefer) continue;
            any = true;
            if (trigItemValue(g, menu.item) != menu.prefer) return false;
            bool const canPersist =
                    std::find(w.persistItemIds.begin(), w.persistItemIds.end(), menu.item) != w.persistItemIds.end();
            auto const& persisted = g.trig.r().persistItems;
            if (canPersist && std::find(persisted.begin(), persisted.end(), menu.item) == persisted.end()) return false;
        }
        return any;
    }

    // returns true if the wanted menu values differ from the starting ones
    bool menuWanted(World const& w) {
        for (auto const& menu : w.menus)
            if (menu.hasPrefer && menuStartValue(w, menu.item) != menu.prefer) return true;
        return false;
    }

    // persistent items (item, value) as the next attempt will start with them
    std::vector<std::pair<int, int>> persistAfterOf(Game const& g) {
        World const& w = *g.world;
        std::vector<std::pair<int, int>> out = w.persistentItems;
        if (!g.trig.on || !g.trig.h) return out;
        for (int id : g.trig.r().persistItems) {
            int const value = trigItemValue(g, id);
            bool found = false;
            for (auto& e : out) {
                if (e.first == id) {
                    e.second = value;
                    found = true;
                    break;
                }
            }
            if (!found) out.emplace_back(id, value);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // starts the clock and the result, and lists the speed portals that move triggers bring from afar
    // (the search uses a finer grid near them)
    auto SearchRun::beginRun() -> void {
        startTime = std::chrono::steady_clock::now();
        res.startTick = start.tick;
        g_debtFirst = opt.portalDebt;
        g_waveExitReach.store(opt.waveExitReach, std::memory_order_relaxed);
        endTick = opt.horizonTicks > 0 ? start.tick + opt.horizonTicks : (1 << 30);
        if (start.world) {
            World const& world = *start.world;

            std::unordered_map<int, float> maxMoveOfGroup;
            for (auto const& o : world.objs)
                if (o.id == ObjId::MoveTrigger && o.cls == 1 && o.targetGroup > 0) {
                    float const dist = std::fabs(o.mvX) + std::fabs(o.mvY);
                    float& maxMove = maxMoveOfGroup[o.targetGroup];
                    maxMove = std::max(maxMove, dist);
                }
            for (auto const& cluster : world.portalClusters) {
                if (!cluster.speed || !cluster.dyn) continue;
                float moved = 0.f;
                for (int member : cluster.members)
                    for (short group : world.objs[static_cast<size_t>(member)].groups)
                        if (auto it = maxMoveOfGroup.find(group); it != maxMoveOfGroup.end())
                            moved = std::max(moved, it->second);
                if (moved > 300.f) dynSpeedX.push_back(cluster.passX);
            }
        }
        std::sort(dynSpeedX.begin(), dynSpeedX.end());
        if (debugOptions().check && start.world)
            std::fprintf(stderr,
                         "[level] %zu speed portal cluster(s) brought from afar, %zu persistent item(s), %d inert "
                         "interface object(s)\n",
                         dynSpeedX.size(), start.world->persistItemIds.size(), start.world->uiObjects);
    }

    // level maps that the costs read: portal clusters, objects to touch, structure, void and end zone
    auto SearchRun::buildCues() -> void {
        track.init(start);
        tieSalt = 0;
        if (start.world) {
            cues.clusters = &start.world->portalClusters;
            cues.passX = start.world->clusterPassX;
            std::vector<int> toTouch = opt.mustTouch;

            if (opt.autoTouch && start.trig.on) {
                World const& w = *start.world;
                for (size_t i = 0; i < w.objs.size(); ++i) {
                    Obj const& o = w.objs[i];
                    if (o.cls != 1 || !o.touchTriggered || o.spawnTriggered || !hasSimEffect(o.id) ||
                        !touchMatters(w, o))
                        continue;
                    if (o.id == ObjId::SpeedSlow || o.id == ObjId::SpeedNormal || o.id == ObjId::SpeedFast ||
                        o.id == ObjId::SpeedFaster || o.id == ObjId::SpeedFastest)
                        continue;
                    if (o.rect.w > 600.f || o.rect.h > 600.f) continue;
                    if (isTrapTouch(w, o)) continue;
                    if (o.multiTriggered) continue;
                    toTouch.push_back(static_cast<int>(i));
                }
                std::sort(toTouch.begin(), toTouch.end());
                toTouch.erase(std::unique(toTouch.begin(), toTouch.end()), toTouch.end());
            }

            if (opt.trapTouchCost > 0 && start.trig.on) {
                World const& w = *start.world;
                for (size_t i = 0; i < w.objs.size(); ++i) {
                    Obj const& o = w.objs[i];
                    if (o.cls != 1 || !o.touchTriggered || o.multiTriggered || !isTrapTouch(w, o)) continue;
                    cues.traps.push_back(Trap{o.rect.x - 60.f, o.rect.x + o.rect.w + 60.f, static_cast<int>(i)});
                    if (debugOptions().check)
                        std::fprintf(stderr,
                                     "[trap] touch trigger uid %d (x %.0f, y %.0f): turns off a part of the level "
                                     "ahead or turns a hidden wall back on, avoided\n",
                                     o.uid, o.x, o.y);
                }
                std::sort(cues.traps.begin(), cues.traps.end(),
                          [](Trap const& a, Trap const& b) { return a.x0 < b.x0; });
            }

            for (int idx : toTouch) {
                if (idx < 0 || static_cast<size_t>(idx) >= start.world->objs.size()) continue;
                Obj const& o = start.world->objs[static_cast<size_t>(idx)];
                World::PortalCluster cluster;
                cluster.passX = o.rect.x + o.rect.w + 15.f;
                cluster.approachX = o.rect.x - 15.f;
                cluster.yLo = o.rect.y;
                cluster.yHi = o.rect.y + o.rect.h;
                cluster.members.push_back(idx);
                cues.extra.push_back(std::move(cluster));
                cues.passX.emplace_back(cues.extra.back().passX, -static_cast<int>(cues.extra.size()));
            }
            std::sort(cues.passX.begin(), cues.passX.end());

            if (opt.missedPortalCost > 0) {
                for (auto const& e : cues.passX) {
                    World::PortalCluster const& cluster = clusterOf(cues, e.second);
                    if (prepaid(cluster) && cluster.approachX < 1e29f)
                        cues.approachX.emplace_back(cluster.approachX, e.second);
                }
                std::sort(cues.approachX.begin(), cues.approachX.end());
            }

            if (opt.bandCost > 0) buildBands(*start.world, cues.bands);
            if (opt.emptyCost > 0) buildEmpty(*start.world, cues.empty);
            if (opt.extentCost > 0) buildExtent(*start.world, cues.extent, 105.f);

            if (opt.pruneVoid && !start.world->platformer) buildVoidHull(*start.world, cues.voidHull);
            if (opt.awayKill > 0 && !start.world->platformer && !start.world->twoPlayer)
                cues.presence.build(*start.world);
            if ((opt.escapeKill > 0 || opt.escapeCost > 0 || opt.speedSepGround) && !start.world->platformer &&
                !start.world->twoPlayer)
                cues.structure.build(*start.world);
            if (!start.world->platformer) {
                constexpr float kEndSlack = 900.f;
                float const endX = endActivationX(*start.world);
                if (endX < std::numeric_limits<float>::infinity()) {
                    cues.endZoneX = endX - kEndSlack;
                    if (debugOptions().check)
                        std::fprintf(stderr, "[end] End trigger fires at x %.0f: void rules lifted from x %.0f\n", endX,
                                     cues.endZoneX);
                }
            }
        }
    }

    // counters the level waits for: count triggers, the clicks owed to event triggers, the pace of the items
    auto SearchRun::findItemGoals() -> void {
        if (start.world && start.trig.on)
            for (auto const& o : start.world->objs)
                if (o.cls == 1 && (o.id == ObjId::CountTrigger || o.id == ObjId::InstantCountTrigger) &&
                    !o.spawnTriggered && !o.touchTriggered)
                    countTargets.push_back(CountTarget{std::clamp(o.itemID, 0, 9999), o.count, o.x,
                                                       o.id == ObjId::InstantCountTrigger});
        if (start.world && start.trig.on) {
            World const& w = *start.world;
            for (auto const& e : w.objs) {
                if (e.cls != 1 || e.id != ObjId::EventTrigger || e.spawnTriggered || e.touchTriggered ||
                    e.targetGroup <= 0)
                    continue;
                for (int i : w.allOfGroup(e.targetGroup)) {
                    Obj const& pick = w.objs[static_cast<size_t>(i)];
                    if (pick.cls != 1 || pick.id != ObjId::PickupTrigger) continue;
                    int const item = std::clamp(pick.itemID, 0, 9999);
                    for (auto const& target : countTargets)
                        if (target.item == item && target.x >= e.x - 600.f)
                            clickZones.push_back(
                                    ClickZone{e.x - 30.f, std::max(e.x, target.x) + 1500.f, item, target.count});
                }
            }
            if (debugOptions().check)
                for (auto const& z : clickZones)
                    std::fprintf(stderr, "[counter] clicks owed x %.0f .. %.0f (object %d -> %d)\n", z.x0, z.x1, z.item,
                                 z.count);
        }
        itemRewardUntilX = std::numeric_limits<float>::infinity();
        if (start.world && start.trig.on) {
            float lastCounterX = -std::numeric_limits<float>::infinity();
            for (auto const& o : start.world->objs) {
                if (o.cls != 1 || (o.id != ObjId::CountTrigger && o.id != ObjId::InstantCountTrigger &&
                                   o.id != ObjId::ItemCompareTrigger))
                    continue;
                if (o.spawnTriggered || o.touchTriggered) {
                    lastCounterX = std::numeric_limits<float>::infinity();
                    break;
                }
                lastCounterX = std::max(lastCounterX, o.x);
            }
            if (lastCounterX > -std::numeric_limits<float>::infinity()) itemRewardUntilX = lastCounterX + 60.f;
        }
        if (start.world && start.trig.on) {
            std::map<int, PaceItem> byItem;
            std::set<int> unknownItems;
            for (auto const& o : start.world->objs) {
                if (o.cls != 1 || (o.id != ObjId::CountTrigger && o.id != ObjId::InstantCountTrigger)) continue;
                int const item = std::clamp(o.itemID, 0, 9999);
                if (o.spawnTriggered || o.touchTriggered) {
                    unknownItems.insert(item);
                    continue;
                }
                int threshold = o.count;
                if (o.id == ObjId::InstantCountTrigger) threshold = o.compare == 1 ? o.count + 1 : o.count;
                if (threshold <= 0) continue;
                auto& pace = byItem[item];
                pace.item = item;
                pace.th.emplace_back(o.x, threshold);
            }
            for (auto const& o : start.world->objs)
                if (o.cls == 1 && o.id == ObjId::ItemCompareTrigger) {
                    unknownItems.insert(std::clamp(o.itemID, 0, 9999));
                    unknownItems.insert(std::clamp(o.itemID2, 0, 9999));
                }
            for (auto& e : byItem) {
                if (unknownItems.count(e.first) || paceItems.size() >= 8) continue;
                std::sort(e.second.th.begin(), e.second.th.end());
                paceItems.push_back(std::move(e.second));
            }
        }
    }

    // the rest of the state: checkpoints, beam ladder, kill and revisit tables, worker pool and buffers
    auto SearchRun::initState() -> void {
        hard.push_back(Checkpoint{start, start.tick, 0});
        lastLiveX = 0.f;
        rootTick = start.tick;
        beam = opt.beamWidth;
        widenedAtTick = -1;
        gapNow = opt.minPressGap;
        ladder = kLadder;
        ladderRoot = -1;
        capLiftedAt = -2;
        lastReducedTick = std::numeric_limits<int>::min() / 2;
        noReduceUntil = std::numeric_limits<int>::min() / 2;
        dbgHoldTrack = debugOptions().check >= 3;
        lastFlyKillTick = std::numeric_limits<int>::min() / 2;
        lastFlyKillX = 0.f;
        lastEscKillTick = std::numeric_limits<int>::min() / 2;
        lastEscKillX = 0.f;
        lastAwayKillTick = std::numeric_limits<int>::min() / 2;
        lastAwayKillX = -1e30f;
        lastRevisitKillTick = std::numeric_limits<int>::min() / 2;
        lastRevisitX = 0.f;
        revisitOn = opt.revisitKill && !(start.world && (start.world->platformer || start.world->twoPlayer));
        if (revisitOn)
            for (auto const& v : opt.priorPath) visitAddXY(v[0], v[1], static_cast<int>(v[2]));
        deadLearns = 0;
        popBeforeTick = std::numeric_limits<int>::max();
        trapSoftFrom = -1;
        qY = opt.quantY;
        qV = opt.quantV;
        spotX = -1e30f;
        holdX = -1e30f;
        spotTick = -1;
        zoneX0 = 1e30f;
        zoneX1 = -1e30f;
        zoneSet = Passed{0.f, 0, 0.f, 0.f, 0};
        nThreads = opt.threads > 0 ? opt.threads : std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
        killersOf.resize(static_cast<size_t>(nThreads));
        awayKillsOf.resize(static_cast<size_t>(nThreads));
        stuckKillsOf.resize(static_cast<size_t>(nThreads));
        escapeKillsOf.resize(static_cast<size_t>(nThreads));
        revisitKillsOf.resize(static_cast<size_t>(nThreads));
        loopStartsOf.resize(static_cast<size_t>(nThreads));
        loopZonesOf.resize(static_cast<size_t>(nThreads));
        trigMemoNewEpoch();
        pool.emplace(nThreads, opt.threadInit);
        curBuckets = std::vector<std::vector<Node>>(static_cast<size_t>(nThreads));
        nextBuckets = std::vector<std::vector<Node>>(static_cast<size_t>(nThreads));
        arenas = std::vector<Arena>(static_cast<size_t>(nThreads));
        for (auto& a : arenas) a.incoming.resize(static_cast<size_t>(nThreads));
        localSeen = std::vector<KeyTable<int>>(static_cast<size_t>(nThreads));
        adaptWidenSeen = 0;
        busy = std::vector<double>(static_cast<size_t>(nThreads), 0.0);
        nodesDone = std::vector<long>(static_cast<size_t>(nThreads), 0);
        nodeCostUs = 0.0;
        busyPrev = 0.0;
        nodesPrev = 0;
        localCtx = std::vector<TrigLocalCtx>(static_cast<size_t>(nThreads));
        part = std::vector<std::vector<size_t>>(static_cast<size_t>(nThreads));
        partCursor = std::vector<std::atomic<size_t>>(static_cast<size_t>(nThreads));
        debtTickMark = -1;
    }

    // roots the search at the start state
    auto SearchRun::startFromLevel() -> void {
        restartFrom(start, start.tick, {});
        tick = start.tick;
        lastPublishCheck = tick;
        lastSoft = tick;
        noSoftBefore = -1;
        impasseRecord = -1;
        popStreak = 0;
        fromStart = false;
        earlierFromStart = 0;
        prevMinDebt = -1;
        prevMinSDebt = -1;
        lastPopRestartTick = std::numeric_limits<int>::max();
        lastLostSeen = -1;
        recordTick = tick;
        recordTime = startTime;
    }

    auto SearchRun::run() -> SearchResult {
        beginRun();
        buildCues();
        findItemGoals();
        initState();
        startFromLevel();
        while (true) {
            if (checkCancelled() == Flow::Return) return std::move(finalResult);
            if (checkBudget() == Flow::Return) return std::move(finalResult);
            if (checkCompleted() == Flow::Return) return std::move(finalResult);
            prepareLayer();
            expandLayer();
            countKills();
            mergeChildren();
            sortChildren();
            if (checkDebtRise() == Flow::Continue) continue;
            liftMustTakes();
            if (Flow const flow = backOff(); flow != Flow::Next) {
                if (flow == Flow::Continue) continue;
                return std::move(finalResult);
            }
            narrowBack();
            chooseWidth();
            pruneLayer();
            traceTracked();
            advanceTick();
            checkReplay();
            keepSoftPoint();
            publishPrefix();
            waitForPlayer();
        }
    }

    SearchResult searchPath(Game const& start, SearchOptions const& opt, std::atomic<bool> const* cancel,
                            std::function<void(int, int)> const& progress) {
        SearchRun run(start, opt, cancel, progress);
        return run.run();
    }

}
