// Back off when every state died: learns from the kills (zones, traps, counters, items, rings, portals) and
// restarts wider or from an earlier point.

#include "search_internal.hpp"

namespace ap::sim {
    // every state died: learns why (kills, traps, missed portals, items) and restarts wider or from an earlier point
    auto SearchRun::backOff() -> Flow {
        if (liveNext.empty()) {
            allowKilledZones();
            findLoopImpasse();
            findTrapImpasse();
            if (Flow const flow = retryNominalBeam(); flow != Flow::Next) return flow;
            if (Flow const flow = restartBeforeTrap(); flow != Flow::Next) return flow;
            if (Flow const flow = learnCounterGoals(); flow != Flow::Next) return flow;
            if (Flow const flow = learnItemTargets(); flow != Flow::Next) return flow;
            if (Flow const flow = learnRingGoals(); flow != Flow::Next) return flow;
            pickRung();
            learnPortalGoals();
            findLiveAnchor();
            retryFromLive();
            if (Flow const flow = widenAndRestart(); flow != Flow::Next) return flow;
            if (Flow const flow = restartTick(); flow != Flow::Next) return flow;
        }
        return Flow::Next;
    }

    // the layer died soon after some kills: lets the next try through the places of those kills (revisits,
    // flying exits, escapes, away zones)
    auto SearchRun::allowKilledZones() -> void {
        ++res.stats.widenings;
        if (revisitOn && tick - lastRevisitKillTick <= kFourSeconds) {
            bool covered = false;
            for (auto const& z : revisitOk)
                if (lastRevisitX >= z.first && lastRevisitX <= z.second) {
                    covered = true;
                    break;
                }
            if (!covered) {
                revisitOk.emplace_back(lastRevisitX - 300.f, lastRevisitX + 300.f);
                if (debugOptions().check)
                    std::fprintf(
                            stderr,
                            "[back] tick %d (x~%.0f) : dead end after dropped returns ; arrival at x %.0f allowed\n",
                            tick, lastLiveX, lastRevisitX);
            }
        }
        if (tick - lastFlyKillTick <= 1440) {
            bool covered = false;
            bool widened = false;
            for (auto& z : flyOkZones)
                if (lastFlyKillX >= z.first && lastFlyKillX <= z.second && lastLiveX <= z.second) {
                    if (z.second - z.first < 1500.f) {
                        z.first = std::min(z.first, lastFlyKillX - 3000.f);
                        z.second = std::max(z.second, std::max(lastFlyKillX, lastLiveX) + 600.f);
                        widened = true;
                    }
                    covered = true;
                    break;
                }
            if (widened && debugOptions().check)
                std::fprintf(stderr, "[fly] tick %d (x~%.0f) : dead end again, relaxed zone widened\n", tick,
                             lastLiveX);
            if (!covered) {
                flyOkZones.emplace_back(lastFlyKillX - 600.f, std::max(lastFlyKillX, lastLiveX) + 300.f);
                if (debugOptions().check)
                    std::fprintf(stderr,
                                 "[fly] tick %d (x~%.0f) : dead end after \"two missed exits\" kills (x~%.0f) ; rule "
                                 "relaxed from x %.0f to %.0f\n",
                                 tick, lastLiveX, lastFlyKillX, flyOkZones.back().first, flyOkZones.back().second);
            }
        }
        if (tick - lastEscKillTick <= 480) {
            bool covered = false;
            for (auto const& z : escOkZones)
                if (lastEscKillX >= z.first && lastEscKillX <= z.second && lastLiveX <= z.second) {
                    covered = true;
                    break;
                }
            if (!covered) {
                escOkZones.emplace_back(lastEscKillX - 3000.f, std::max(lastEscKillX, lastLiveX) + 600.f);
                if (debugOptions().check)
                    std::fprintf(stderr,
                                 "[structure] tick %d (x~%.0f) : dead end after \"structure exit\" kills (x~%.0f) ; "
                                 "rule relaxed from x %.0f to %.0f\n",
                                 tick, lastLiveX, lastEscKillX, escOkZones.back().first, escOkZones.back().second);
            }
        }
        if (tick - lastAwayKillTick <= 480 && cues.presence.ok()) {
            bool covered = false;
            bool extended = false;
            float const killX = std::max(lastAwayKillX, lastLiveX);
            for (auto& z : awayZones)
                if (lastLiveX >= z.first && lastLiveX <= z.second) {
                    if (killX + 150.f > z.second) {
                        z.second = killX + 900.f;
                        extended = true;
                    }
                    covered = true;
                    break;
                }
            if (!covered) {
                awayZones.emplace_back(lastLiveX - 1500.f, killX + 300.f);
                if (debugOptions().check)
                    std::fprintf(stderr,
                                 "[off-level] tick %d (x~%.0f) : dead end after off-level kills ; delay x4 from x %.0f "
                                 "to %.0f\n",
                                 tick, lastLiveX, lastLiveX - 1500.f, killX + 300.f);
            } else if (extended && debugOptions().check)
                std::fprintf(stderr,
                             "[off-level] tick %d (x~%.0f) : dead end at the end of a relaxed zone ; zone extended to "
                             "x %.0f\n",
                             tick, lastLiveX, killX + 900.f);
        }
        if (debugOptions().check)
            std::fprintf(
                    stderr,
                    "[dead end] tick %d (x~%.0f) beam %d q %.2f soft %zu hard %zu streak %d (layer : %zu states%s)\n",
                    tick, lastLiveX, beam, qY, soft.size(), hard.size(), popStreak, live.size(),
                    live.empty()
                            ? ""
                            : (live[0]->g ? (live[0]->g->died ? ", live[0] dead" : "") : ", live[0] without state"));
    }

    // states sent back by a loop trigger: remembers the zones of those loops
    auto SearchRun::findLoopImpasse() -> void {
        loopImpasse = false;
        {
            int loopStart = std::numeric_limits<int>::max();
            for (auto const& v : loopStartsOf)
                for (int loopTick : v) loopStart = std::min(loopStart, loopTick);
            loopImpasse = loopStart != std::numeric_limits<int>::max();
            if (loopStart != std::numeric_limits<int>::max()) {
                for (auto const& v : loopZonesOf)
                    for (auto const& z : v) {
                        if (!(z.x1 - z.x0 > 300.f)) continue;
                        bool merged = false;
                        for (auto& zone : loopZones)
                            if (std::fabs(zone.y - z.y) <= 30.f && z.x0 <= zone.x1 && z.x1 >= zone.x0) {
                                zone.x0 = std::min(zone.x0, z.x0);
                                zone.x1 = std::max(zone.x1, z.x1);
                                merged = true;
                                break;
                            }
                        if (!merged && loopZones.size() < 16) {
                            loopZones.push_back(z);
                            if (debugOptions().check)
                                std::fprintf(stderr,
                                             "[loop] learned zone : x %.0f .. %.0f, y %.0f (a lineage entering it is "
                                             "dropped)\n",
                                             z.x0, z.x1, z.y);
                            if (opt.pruneVoid && start.world && !start.world->platformer) {
                                std::vector<std::array<float, 4>> exclude;
                                for (auto const& zone : loopZones)
                                    exclude.push_back({zone.x0 - 30.f, zone.x1 + 30.f, zone.y - 60.f, zone.y});
                                cues.voidHull = VoidHull{};
                                buildVoidHull(*start.world, cues.voidHull, exclude);
                            }
                        }
                    }
                size_t const before = soft.size();
                while (!soft.empty() && soft.back().tick > loopStart - 240) {
                    soft.pop_back();
                    ++res.stats.softRestarts;
                }
                if (debugOptions().check)
                    std::fprintf(stderr,
                                 "[loop] tick %d : endless loop entered at tick %d ; %zu restart point(s) of the loop "
                                 "removed\n",
                                 tick, loopStart, before - soft.size());
            }
        }
    }

    // counts what killed the states; when traps did, learns where the trap floor changes
    auto SearchRun::findTrapImpasse() -> void {
        trapImpasse = false;
        {
            size_t nDead = 0;
            size_t nStuckD = 0;
            size_t nTrapD = 0;
            for (auto const& v : killersOf) {
                nDead += v.size();
                for (int u : v) {
                    if (u == kStuckKiller)
                        ++nStuckD;
                    else if (u == kTrapKiller)
                        ++nTrapD;
                }
            }
            if (nTrapD > 0 && nTrapD * 2 >= nDead)
                for (auto& corr : trapCorr) {
                    if (corr.off || !(lastLiveX >= corr.xStart - 60.f && lastLiveX <= corr.xStart + 600.f)) continue;
                    corr.off = true;
                    fromStart = false;
                    popStreak = 0;
                    lastPopRestartTick = std::numeric_limits<int>::max();
                    if (debugOptions().check)
                        std::fprintf(stderr,
                                     "[trap] tick %d (x~%.0f) : the learned corridor (from x %.0f) kills everything as "
                                     "soon as it is entered ; given up\n",
                                     tick, lastLiveX, corr.xStart);
                }
            int const best = live.empty() ? -1 : bestOf(false);
            if (!loopImpasse && opt.stuckKill > 0 && nStuckD > 0 && nStuckD * 2 >= nDead && trapCorr.size() < 8 &&
                best >= 0 && live[static_cast<size_t>(best)]->g && !start.world->platformer) {
                Game const& bestGame = *live[static_cast<size_t>(best)]->g;
                size_t hardIdx = hard.size() - 1;
                float ymin = bestGame.p1.y;
                float ymax = bestGame.p1.y;
                while (hardIdx > 0) {
                    float const y = hard[hardIdx].g.p1.y;
                    ymin = std::min(ymin, y);
                    ymax = std::max(ymax, y);
                    if (ymax - ymin > kTrapFloorChange || tick - hard[hardIdx].tick > 14400) break;
                    --hardIdx;
                }
                std::vector<uint8_t> acts(
                        inputs.begin() + static_cast<long>(std::min(hard[hardIdx].inputsLen, inputs.size())),
                        inputs.end());
                {
                    std::vector<uint8_t> const tail = tailTo(best);
                    acts.insert(acts.end(), tail.begin(), tail.end());
                }

                struct PS {
                    float x;
                    float y;
                    uint8_t m;
                    int t;
                };
                std::vector<PS> path;
                path.reserve(acts.size() + 1);
                Game g = hard[hardIdx].g;
                path.push_back(PS{g.p1.x, g.p1.y, modeBitOf(g.p1), g.tick});
                for (uint8_t act : acts) {
                    if (g.died) break;
                    stepTick(g, act);
                    path.push_back(PS{g.p1.x, g.p1.y, modeBitOf(g.p1), g.tick});
                }
                bool const reachedNow = !g.died && g.tick == tick;
                size_t corrStart = path.size();
                if (reachedNow) {
                    float lo = path.back().y;
                    float hi = path.back().y;
                    for (size_t i = path.size(); i-- > 0;) {
                        lo = std::min(lo, path[i].y);
                        hi = std::max(hi, path[i].y);
                        if (hi - lo > kTrapFloorChange) {
                            corrStart = i + 1;
                            break;
                        }
                    }
                    if (corrStart == path.size()) {
                        int const tMin = tick - opt.stuckKill - 240;
                        corrStart = 0;
                        while (corrStart < path.size() && path[corrStart].t < tMin) ++corrStart;
                    }
                }
                bool known = false;
                if (corrStart < path.size())
                    for (auto const& corr : trapCorr)
                        if (std::fabs(corr.xStart - path[corrStart].x) < 300.f) {
                            known = true;
                            break;
                        }
                if (corrStart < path.size() && !known && path[corrStart].t > hard.front().tick) {
                    TrapCorr corr;
                    float xmin = 1e30f;
                    float xmax = -1e30f;
                    for (size_t i = corrStart; i < path.size(); ++i) {
                        xmin = std::min(xmin, path[i].x);
                        xmax = std::max(xmax, path[i].x);
                    }
                    corr.x0 = std::floor(xmin / 30.f) * 30.f;
                    size_t const nCols = static_cast<size_t>((xmax - corr.x0) / 30.f) + 1;
                    if (nCols <= 40000) {
                        corr.band.assign(nCols, std::array<float, 2>{1e30f, -1e30f});
                        corr.modes.assign(nCols, 0);
                        for (size_t i = corrStart; i < path.size(); ++i) {
                            size_t const col = static_cast<size_t>((path[i].x - corr.x0) / 30.f);
                            if (col >= nCols) continue;
                            std::pair<float, float> const freeY = cues.structure.empty()
                                                                          ? std::pair<float, float>{-1e30f, 1e30f}
                                                                          : cues.structure.freeAt(path[i].x, path[i].y);
                            corr.band[col][0] =
                                    std::min(corr.band[col][0], std::max(freeY.first, path[i].y - kTrapBand));
                            corr.band[col][1] =
                                    std::max(corr.band[col][1], std::min(freeY.second, path[i].y + kTrapBand));
                            corr.modes[col] = static_cast<uint8_t>(corr.modes[col] | path[i].m);
                        }
                        corr.startTick = path[corrStart].t;
                        corr.xStart = path[corrStart].x;
                        if (debugOptions().check)
                            std::fprintf(stderr,
                                         "[trap] tick %d (x~%.0f) : trap with no way out ; corridor learned from x "
                                         "%.0f (tick %d, y %.0f) to x %.0f - a lineage entering it dies ; restart from "
                                         "before tick %d\n",
                                         tick, lastLiveX, corr.xStart, corr.startTick, path[corrStart].y, xmax,
                                         corr.startTick - 240);
                        popBeforeTick = corr.startTick - 240;
                        trapSoftFrom = corr.startTick;
                        trapCorr.push_back(std::move(corr));
                        trapImpasse = true;
                        loopImpasse = true;
                    }
                }
            }
        }
    }

    // keeps the furthest path; in live mode, a dead end just after the beam was reduced retries at the nominal
    // beam first
    auto SearchRun::retryNominalBeam() -> Flow {
        if (static_cast<size_t>(tick - res.startTick) + 1 > furthest.size()) furthest = pathTo(bestOf(false));
        if (opt.playedTicks && lastReducedTick >= tick - 240 && tick >= noReduceUntil && !trapImpasse) {
            bool tried = false;
            for (int spot : nominalSpots)
                if (std::abs(spot - tick) <= 48) {
                    tried = true;
                    break;
                }
            if (!tried) {
                nominalSpots.push_back(tick);
                noReduceUntil = tick + 480;
                if (debugOptions().check)
                    std::fprintf(stderr,
                                 "[live] tick %d (x~%.0f) : dead end under a reduced beam ; back to the normal beam\n",
                                 tick, lastLiveX);
                while (!soft.empty() && soft.back().tick > tick - 240) {
                    soft.pop_back();
                    ++res.stats.softRestarts;
                }
                widenedAtTick = tick;
                spotTick = tick;
                spotX = std::max(spotX, lastLiveX);
                if (!soft.empty())
                    restartFromSoft(soft.back());
                else
                    restartFrom(hard.back().g, hard.back().tick, {}, hard.back().span);
                tick = rootTick;
                ladderRoot = rootTick;
                lastPublishCheck = tick;
                lastSoft = tick;
                prevMinDebt = -1;
                prevMinSDebt = -1;
                return Flow::Continue;
            }
        }
        return Flow::Next;
    }

    // after a trap impasse, restarts from before the trap
    auto SearchRun::restartBeforeTrap() -> Flow {
        if (trapImpasse) {
            popStreak = 0;
            fromStart = false;
            lastPopRestartTick = std::numeric_limits<int>::max();
            impasseRecord = popBeforeTick + 240;
        } else {
            if (tick > impasseRecord + 48) {
                popStreak = 0;
                fromStart = false;
                lastPopRestartTick = std::numeric_limits<int>::max();
            } else if (fromStart && tick < impasseRecord - 48 && earlierFromStart < 3) {
                ++earlierFromStart;
                popStreak = 0;
                fromStart = false;
                lastPopRestartTick = std::numeric_limits<int>::max();
            } else if (fromStart) {
                res.fromStart = true;
                {
                    finalResult =
                            finish(-1, false, "no path found (tick " + std::to_string(tick) + ", even from the start)");
                    return Flow::Return;
                }
            }
            impasseRecord = std::max(impasseRecord, tick);
        }
        return Flow::Next;
    }

    // when the killers are moved by count triggers whose count is not reached, learns those counters as goals
    auto SearchRun::learnCounterGoals() -> Flow {
        if (opt.preferFewInputs && start.trig.on && !live.empty() && live[0]->g && !trapImpasse) {
            Game const& bestGame = *live[0]->g;
            bool touchOn = false;
            for (auto const& touch : bestGame.trig.r().touches)
                if (!touch.stopped) {
                    touchOn = true;
                    break;
                }
            if (touchOn) {
                World const& w = *start.world;
                float const frontX = lastLiveX;
                std::map<int, int> hist;
                int total = 0;
                for (auto const& v : killersOf)
                    for (int u : v) {
                        ++hist[u];
                        ++total;
                    }
                std::vector<short> killerGroups;
                for (auto const& e : hist) {
                    if (e.second * 10 < total) continue;
                    int const killerIdx = w.indexOfUid(e.first);
                    if (killerIdx < 0) continue;
                    for (short group : w.objs[static_cast<size_t>(killerIdx)].groups)
                        if (group > 0) killerGroups.push_back(group);
                }
                float minX = 1e30f;
                for (size_t i = 0; i < w.objs.size() && !killerGroups.empty(); ++i) {
                    Obj const& o = w.objs[i];
                    if (o.cls != 1 || (o.id != ObjId::CountTrigger && o.id != ObjId::InstantCountTrigger) ||
                        !o.tf(Obj::TF_ActivateGroup))
                        continue;
                    if (std::find(killerGroups.begin(), killerGroups.end(), static_cast<short>(o.targetGroup)) ==
                        killerGroups.end())
                        continue;
                    if (!(o.x > frontX - 1800.f && o.x < frontX + 30.f)) continue;
                    if (std::find(trapLearned.begin(), trapLearned.end(), static_cast<int>(i)) != trapLearned.end())
                        continue;
                    int item = o.itemID;
                    if (item < 0) item = 0;
                    if (item > 9999) item = 9999;
                    auto const* value = bestGame.trig.r().items.find(item);
                    int const count = value ? *value : 0;
                    bool reached;
                    if (o.id == ObjId::CountTrigger)
                        reached = o.count >= 0 ? count >= o.count : count <= o.count;
                    else
                        reached = o.compare == 0   ? count == o.count
                                  : o.compare == 1 ? o.count < count
                                                   : count < o.count;
                    if (!reached) continue;
                    trapLearned.push_back(static_cast<int>(i));
                    trapWin.emplace_back(o.x - 30.f, frontX + 600.f);
                    minX = std::min(minX, o.x);
                    if (debugOptions().check)
                        std::fprintf(stderr,
                                     "[trap] tick %d (x~%.0f) : trigger uid %d (item %d = %d) turns the killers' group "
                                     "%d back on ; presses without reward from x %.0f to %.0f\n",
                                     tick, frontX, o.uid, item, count, o.targetGroup, o.x - 30.f, frontX + 600.f);
                }
                if (minX < 1e29f) {
                    while (!soft.empty() && !soft.back().states.empty() &&
                           soft.back().states[0].g.p1.x > minX - 120.f) {
                        soft.pop_back();
                        ++res.stats.softRestarts;
                    }
                    if (soft.empty())
                        while (hard.size() > 1 && hard.back().g.p1.x > minX - 120.f) {
                            hard.pop_back();
                            ++res.stats.hardPops;
                            inputs.resize(hard.back().inputsLen);
                            if (opt.onPrefix) opt.onPrefix(inputs);
                        }
                    widenedAtTick = tick;
                    spotTick = tick;
                    spotX = std::max(spotX, lastLiveX);
                    if (!soft.empty())
                        restartFromSoft(soft.back());
                    else
                        restartFrom(hard.back().g, hard.back().tick, {}, hard.back().span);
                    tick = rootTick;
                    ladderRoot = rootTick;
                    lastPublishCheck = tick;
                    lastSoft = tick;
                    prevMinDebt = -1;
                    prevMinSDebt = -1;
                    return Flow::Continue;
                }
            }
        }
        return Flow::Next;
    }

    // learns the items whose triggers move the killers (up to 4) as targets for the next tries
    auto SearchRun::learnItemTargets() -> Flow {
        if (opt.learnTouch && start.trig.on && !live.empty() && live[0]->g && !trapImpasse) {
            World const& w = *start.world;
            Game const& bestGame = *live[0]->g;
            float const frontX = lastLiveX;
            std::map<int, int> hist;
            int total = 0;
            for (auto const& v : killersOf)
                for (int u : v) {
                    ++hist[u];
                    ++total;
                }
            std::vector<short> killerGroups;
            for (auto const& e : hist) {
                if (e.second * 10 < total) continue;
                int const killerIdx = w.indexOfUid(e.first);
                if (killerIdx < 0) continue;
                for (short group : w.objs[static_cast<size_t>(killerIdx)].groups)
                    if (group > 0 && std::find(killerGroups.begin(), killerGroups.end(), group) == killerGroups.end())
                        killerGroups.push_back(group);
            }
            if (!killerGroups.empty() && itemTargets.size() < 4) {
                float tMinX = 1e30f;
                for (ItemTarget const& target : itemTargetsFor(w, bestGame, causeIx, killerGroups, frontX)) {
                    if (itemTargets.size() >= 4) break;
                    if (std::any_of(itemTargets.begin(), itemTargets.end(), [&](ItemTarget const& known) {
                            return known.idx == target.idx ||
                                   (known.item == target.item && known.count == target.count &&
                                    known.group == target.group);
                        }))
                        continue;
                    itemTargets.push_back(target);
                    tMinX = std::min(tMinX, target.x);
                    if (std::none_of(groupGoals.begin(), groupGoals.end(),
                                     [&](GroupGoal const& goal) { return goal.group == target.group; })) {
                        GroupGoal goal;
                        goal.group = target.group;
                        goal.checkX = -1e30f;
                        std::set<int> items;
                        for (int counterIdx : causeIx.byTarget[static_cast<size_t>(target.group)]) {
                            Obj const& counter = w.objs[static_cast<size_t>(counterIdx)];
                            if ((counter.id == ObjId::InstantCountTrigger || counter.id == ObjId::CountTrigger) &&
                                causeActivates(counter) && !counter.spawnTriggered && !counter.touchTriggered &&
                                counter.x < frontX) {
                                goal.acts.push_back(counterIdx);
                                items.insert(std::clamp(counter.itemID, 0, 9999));
                            }
                        }
                        float firstX = 1e30f;
                        int nPickups = 0;
                        for (size_t i = 0; i < w.objs.size(); ++i) {
                            Obj const& o = w.objs[i];
                            if ((o.type != Collectible && o.type != UserCoin) || !items.count(o.itemID) ||
                                !o.tf(Obj::TF_PickupItem) || !(o.x < target.x))
                                continue;
                            goal.checkX = std::max(goal.checkX, o.rect.maxX() + 60.f);
                            firstX = std::min(firstX, o.rect.x);
                            ++nPickups;
                        }
                        if (nPickups > 0 && goal.acts.size() <= 512) {
                            tMinX = std::min(tMinX, firstX);
                            if (debugOptions().check)
                                std::fprintf(stderr,
                                             "[counter] group %d : %zu counters, %d pickup(s) between x %.0f and %.0f "
                                             "- a state without a satisfied lit counter is dropped there\n",
                                             target.group, goal.acts.size(), nPickups, firstX, goal.checkX);
                            groupGoals.push_back(std::move(goal));
                        }
                    }
                    if (debugOptions().check)
                        std::fprintf(stderr,
                                     "[counter] tick %d (x~%.0f) : group %d (rescue from the killers) is only turned "
                                     "on by counter uid %d (x %.0f) : item %d %s %d (best state : %d) ; restart from "
                                     "before x %.0f\n",
                                     tick, frontX, target.group, w.objs[static_cast<size_t>(target.idx)].uid, target.x,
                                     target.item,
                                     target.mode == 1   ? ">"
                                     : target.mode == 2 ? "<"
                                     : target.mode == 3 ? "reached"
                                                        : "=",
                                     target.count, trigItemValue(bestGame, target.item), target.x);
                }
                if (tMinX < 1e29f) {
                    std::sort(cues.passX.begin(), cues.passX.end());
                    while (!soft.empty() && !soft.back().states.empty() &&
                           soft.back().states[0].g.p1.x > tMinX - 120.f) {
                        soft.pop_back();
                        ++res.stats.softRestarts;
                    }
                    if (soft.empty())
                        while (hard.size() > 1 && hard.back().g.p1.x > tMinX - 120.f) {
                            hard.pop_back();
                            ++res.stats.hardPops;
                            inputs.resize(hard.back().inputsLen);
                            if (opt.onPrefix) opt.onPrefix(inputs);
                        }
                    beam = opt.playedTicks ? opt.beamWidth : std::min(opt.maxBeamWidth, opt.beamWidth * 4);
                    widenedAtTick = tick;
                    spotTick = tick;
                    spotX = std::max(spotX, lastLiveX);
                    if (!soft.empty())
                        restartFromSoft(soft.back());
                    else
                        restartFrom(hard.back().g, hard.back().tick, {}, hard.back().span);
                    tick = rootTick;
                    ladderRoot = rootTick;
                    lastPublishCheck = tick;
                    lastSoft = tick;
                    prevMinDebt = -1;
                    prevMinSDebt = -1;
                    return Flow::Continue;
                }
            }

            CauseResult const causes =
                    killerGroups.empty() ? CauseResult{} : impasseCauses(w, bestGame, causeIx, killerGroups, frontX);
            float minX = 1e30f;
            int nTake = 0;
            int nAvoid = 0;
            for (int objIdx : causes.avoid) {
                int const uid = w.objs[static_cast<size_t>(objIdx)].uid;
                if (std::binary_search(avoidUids.begin(), avoidUids.end(), uid)) continue;
                avoidUids.insert(std::upper_bound(avoidUids.begin(), avoidUids.end(), uid), uid);
                ++nAvoid;
                auto const* state = bestGame.osFind(objIdx);
                if (state && state->activated) minX = std::min(minX, w.objs[static_cast<size_t>(objIdx)].rect.x);
            }
            for (auto const& take : causes.take) {
                if (std::find(needOff.begin(), needOff.end(), take.group) != needOff.end()) continue;
                needOff.push_back(take.group);
                float lastX = -1e30f;
                for (int orb : take.orbs) lastX = std::max(lastX, w.objs[static_cast<size_t>(orb)].rect.x);
                minX = std::min(minX, lastX);
                ++nTake;
                if (debugOptions().check)
                    std::fprintf(stderr,
                                 "[cause] tick %d (x~%.0f) : group %d to turn off (%zu toggle orb(s), the last at x "
                                 "%.0f) : a toggle of this group blocks the rescue from the killers\n",
                                 tick, frontX, take.group, take.orbs.size(), lastX);
            }
            if (nAvoid && debugOptions().check)
                std::fprintf(stderr,
                             "[cause] tick %d (x~%.0f) : %d toggle orb(s) to avoid (they turn on a toggle that blocks "
                             "the rescue)\n",
                             tick, frontX, nAvoid);
            if (minX < 1e29f) {
                std::sort(cues.passX.begin(), cues.passX.end());
                while (soft.size() > 1 && !soft.back().states.empty() &&
                       soft.back().states[0].g.p1.x > minX - 0.75f * kAttractAhead) {
                    soft.pop_back();
                    ++res.stats.softRestarts;
                }
                while (!soft.empty() && !soft.back().states.empty() && soft.back().states[0].g.p1.x > minX - 120.f) {
                    soft.pop_back();
                    ++res.stats.softRestarts;
                }
                if (soft.empty())
                    while (hard.size() > 1 && hard.back().g.p1.x > minX - 120.f) {
                        hard.pop_back();
                        ++res.stats.hardPops;
                        inputs.resize(hard.back().inputsLen);
                        if (opt.onPrefix) opt.onPrefix(inputs);
                    }
                beam = opt.playedTicks ? opt.beamWidth : std::min(opt.maxBeamWidth, opt.beamWidth * 4);
                widenedAtTick = tick;
                spotTick = tick;
                spotX = std::max(spotX, lastLiveX);
                if (!soft.empty())
                    restartFromSoft(soft.back());
                else
                    restartFrom(hard.back().g, hard.back().tick, {}, hard.back().span);
                tick = rootTick;
                ladderRoot = rootTick;
                lastPublishCheck = tick;
                lastSoft = tick;
                prevMinDebt = -1;
                prevMinSDebt = -1;
                return Flow::Continue;
            }
        }
        return Flow::Next;
    }

    // remembers the toggle orbs and blocks just behind the front that change the killers
    auto SearchRun::learnRingGoals() -> Flow {
        if (opt.learnTouch && start.trig.on) {
            float const frontX = lastLiveX;
            float minX = 1e30f;
            World const& w = *start.world;
            for (size_t i = 0; i < w.objs.size(); ++i) {
                Obj const& o = w.objs[i];
                bool const ring = (o.id == ObjId::ToggleOrb || o.id == ObjId::ToggleBlock) &&
                                  ringEffectMatters(w, o.targetGroup) &&
                                  !std::binary_search(avoidUids.begin(), avoidUids.end(), o.uid);
                if ((o.id == ObjId::ToggleOrb || o.id == ObjId::ToggleBlock) && !ring) continue;
                if (!ring &&
                    (o.cls != 1 || !o.touchTriggered || o.spawnTriggered || !hasSimEffect(o.id) || !touchMatters(w, o)))
                    continue;
                if (o.id == ObjId::SpeedSlow || o.id == ObjId::SpeedNormal || o.id == ObjId::SpeedFast ||
                    o.id == ObjId::SpeedFaster || o.id == ObjId::SpeedFastest)
                    continue;
                if (!ring && (o.rect.w > 600.f || o.rect.h > 600.f || o.multiTriggered || isTrapTouch(w, o))) continue;
                float const passX = o.rect.x + o.rect.w + 15.f;
                if (!(passX > frontX - 1800.f && o.rect.x < frontX)) continue;
                if (!live.empty() && live[0]->g && std::fabs((o.rect.y + o.rect.h * 0.5f) - live[0]->g->p1.y) > 900.f)
                    continue;
                bool known = false;
                for (auto const& extra : cues.extra)
                    if (extra.learned && !extra.members.empty() && extra.members[0] == static_cast<int>(i)) {
                        known = true;
                        break;
                    }
                if (known) continue;
                bool touched = false;
                if (!live.empty() && live[0]->g) {
                    if (ring) {
                        auto const* state = live[0]->g->osFind(static_cast<int>(i));
                        touched = state && state->activated;
                    } else
                        touched = trigActivatedBit(*live[0]->g, static_cast<int>(i));
                }
                if (touched) {
                    if (debugOptions().check)
                        std::fprintf(stderr, "[touch] tick %d : trigger uid %d already touched\n", tick, o.uid);
                    continue;
                }
                World::PortalCluster cluster;
                cluster.passX = passX;
                cluster.yLo = o.rect.y;
                cluster.yHi = o.rect.y + o.rect.h;
                cluster.learned = true;
                cluster.members.push_back(static_cast<int>(i));
                cues.attract.push_back(Attract{o.rect.x, o.rect.y, o.rect.y + o.rect.h});
                cues.extra.push_back(std::move(cluster));
                cues.passX.emplace_back(passX, -static_cast<int>(cues.extra.size()));
                minX = std::min(minX, o.rect.x);
                if (debugOptions().check)
                    std::fprintf(stderr,
                                 "[touch] tick %d (x~%.0f) : %s uid %d (x %.0f) learned as a required passage\n", tick,
                                 frontX, ring ? "toggle orb" : "trigger", o.uid, o.x);
            }
            if (minX < 1e29f) {
                std::sort(cues.passX.begin(), cues.passX.end());
                if (!opt.playedTicks)
                    while (soft.size() > 1 && !soft.back().states.empty() &&
                           soft.back().states[0].g.p1.x > minX - 0.75f * kAttractAhead) {
                        soft.pop_back();
                        ++res.stats.softRestarts;
                    }
                while (!soft.empty() && !soft.back().states.empty() && soft.back().states[0].g.p1.x > minX - 120.f) {
                    soft.pop_back();
                    ++res.stats.softRestarts;
                }
                if (soft.empty())
                    while (hard.size() > 1 && hard.back().g.p1.x > minX - 120.f) {
                        hard.pop_back();
                        ++res.stats.hardPops;
                        inputs.resize(hard.back().inputsLen);
                        if (opt.onPrefix) opt.onPrefix(inputs);
                    }
                beam = opt.playedTicks ? opt.beamWidth : std::min(opt.maxBeamWidth, opt.beamWidth * 4);
                widenedAtTick = tick;
                spotTick = tick;
                spotX = std::max(spotX, lastLiveX);
                if (!soft.empty())
                    restartFromSoft(soft.back());
                else
                    restartFrom(hard.back().g, hard.back().tick, {}, hard.back().span);
                tick = rootTick;
                ladderRoot = rootTick;
                lastPublishCheck = tick;
                lastSoft = tick;
                prevMinDebt = -1;
                prevMinSDebt = -1;
                return Flow::Continue;
            }
        }
        return Flow::Next;
    }

    // finds the widening rung of this spot (each new dead end at a spot climbs one rung)
    auto SearchRun::pickRung() -> void {
        kRungs = ladder;
        rungOf = nullptr;
        for (auto& e : spotRungs)
            if (std::abs(e.first - tick) <= 48) {
                rungOf = &e.second;
                break;
            }
        if (!rungOf) {
            spotRungs.emplace_back(tick, 0);
            rungOf = &spotRungs.back().second;
        }
        if (loopImpasse) *rungOf = std::max(*rungOf, kNRungs);
    }

    // on the 4th rung, learns the portals just behind the front as places to try
    auto SearchRun::learnPortalGoals() -> void {
        if (*rungOf == 3 && opt.learnTouch && !live.empty() && live[0]->g) {
            float const frontX = lastLiveX;
            float minX = 1e30f;
            Game const& bestGame = *live[0]->g;
            World const& w = *start.world;
            for (auto const& cluster : w.portalClusters) {
                if (cluster.speed) continue;
                if (!(cluster.passX > frontX - 1800.f && cluster.passX - 15.f < frontX)) continue;
                float const midY = 0.5f * (cluster.yLo + cluster.yHi);
                if (std::fabs(midY - bestGame.p1.y) > 900.f) continue;
                bool taken = false;
                bool anyActive = false;
                for (int member : cluster.members) {
                    auto const* state = bestGame.osFind(member);
                    if (state && (state->activatedByPlayer1 || state->activatedByPlayer2 || state->activated)) {
                        taken = true;
                        break;
                    }
                    if (!const_cast<Game&>(bestGame).obj(member).disabled()) anyActive = true;
                }
                if (taken || !anyActive) continue;
                bool known = false;
                for (auto const& extra : cues.extra)
                    if (extra.learned && !extra.members.empty() && extra.members[0] == cluster.members[0]) {
                        known = true;
                        break;
                    }
                if (known) continue;
                World::PortalCluster learnedCluster = cluster;
                learnedCluster.learned = true;
                float leftX = 1e30f;
                for (int member : cluster.members) leftX = std::min(leftX, w.objs[static_cast<size_t>(member)].rect.x);
                cues.attract.push_back(Attract{leftX, cluster.yLo, cluster.yHi});
                cues.extra.push_back(std::move(learnedCluster));
                cues.passX.emplace_back(cluster.passX, -static_cast<int>(cues.extra.size()));
                minX = std::min(minX, leftX);
                if (debugOptions().check)
                    std::fprintf(
                            stderr,
                            "[portal] tick %d (x~%.0f) : portal uid %d (x %.0f y %.0f) learned as a required passage\n",
                            tick, frontX, w.objs[static_cast<size_t>(cluster.members[0])].uid, leftX, midY);
            }
            if (minX < 1e29f) {
                std::sort(cues.passX.begin(), cues.passX.end());
                while (soft.size() > 1 && !soft.back().states.empty() &&
                       soft.back().states[0].g.p1.x > minX - 0.75f * kAttractAhead) {
                    soft.pop_back();
                    ++res.stats.softRestarts;
                }
                while (!soft.empty() && !soft.back().states.empty() && soft.back().states[0].g.p1.x > minX - 120.f) {
                    soft.pop_back();
                    ++res.stats.softRestarts;
                }
            }
        }
    }

    // live mode: finds a restart point that is still ahead of the player
    auto SearchRun::findLiveAnchor() -> void {
        anchored = false;
        anchorGame = Game{};
        anchorTail = std::vector<uint8_t>{};
        anchorTick = 0;
        anchorSpan = Span{};
        if (opt.playedTicks) {
            while (*rungOf < kNRungs) {
                Rung const rung = kRungs[*rungOf];
                int const hardTick = hard.back().tick;
                int restartAt = hardTick;
                if (rung.anchor)
                    restartAt = std::max(hardTick, tick - rung.back);
                else
                    for (auto it = soft.rbegin(); it != soft.rend(); ++it)
                        if (it->tick <= tick - rung.back) {
                            restartAt = it->tick;
                            break;
                        }
                if (tick - restartAt >= std::min(rung.back, 240) / 2) break;
                if (hardTick <= res.startTick) break;
                if (debugOptions().check)
                    std::fprintf(stderr,
                                 "[live] tick %d : rung %d skipped (restart at tick %d, %d ticks before the dead end : "
                                 "published prefix)\n",
                                 tick, *rungOf, restartAt, tick - restartAt);
                ++*rungOf;
            }
        }
    }

    // live mode: all rungs used; retries once from the point the player has reached
    auto SearchRun::retryFromLive() -> void {
        if (*rungOf >= kNRungs && opt.playedTicks && !loopImpasse) {
            bool tried = false;
            for (int spot : liveRetrySpots)
                if (std::abs(spot - tick) <= 48) tried = true;
            if (!tried) {
                liveRetrySpots.push_back(tick);
                int const played = opt.playedTicks->load(std::memory_order_relaxed);
                int const popped = retractUnplayed(std::max(played + kFixedAhead, tick - 720), played);
                if (popped > 0) {
                    *rungOf = 0;
                    if (debugOptions().check)
                        std::fprintf(
                                stderr,
                                "[live] tick %d : ladder used up, %d unplayed hard points removed, ladder restarted\n",
                                tick, popped);
                }
            }
        }
    }

    // applies the rung (wider beam, finer grid, smaller press gap) and restarts from its restart point
    auto SearchRun::widenAndRestart() -> Flow {
        if (*rungOf < kNRungs) {
            Rung const rung = kRungs[*rungOf];
            gapNow = gapForRung(*rungOf);

            if (*rungOf >= 1) {
                float const fineX0 = lastLiveX - 2000.f;
                float const fineX1 = lastLiveX + 120.f;
                bool known = false;
                for (auto& z : fineZones)
                    if (fineX1 >= z.first && fineX0 <= z.second) {
                        z.first = std::min(z.first, fineX0);
                        z.second = std::max(z.second, fineX1);
                        known = true;
                        break;
                    }
                if (!known) {
                    fineZones.emplace_back(fineX0, fineX1);
                    if (debugOptions().check)
                        std::fprintf(stderr, "[precision] tick %d : x %.0f .. %.0f now searched with q/4\n", tick,
                                     fineX0, fineX1);
                }
            }
            ++*rungOf;

            if (rung.anchor && !live.empty() && !loopImpasse) {
                int const best = bestOf(false);
                if (best >= 0) {
                    std::vector<uint8_t> tail = tailTo(best);
                    int const want = std::max(hard.back().tick, tick - rung.back);
                    size_t const nSteps = static_cast<size_t>(want - hard.back().tick);
                    if (nSteps <= tail.size()) {
                        anchorGame = hard.back().g;
                        anchorSpan = hard.back().span;
                        for (size_t k = 0; k < nSteps && !anchorGame.died; ++k) {
                            stepTick(anchorGame, tail[k]);
                            spanReplay(anchorSpan, anchorGame);
                        }
                        if (!anchorGame.died && anchorGame.tick == want) {
                            anchorTail.assign(tail.begin(), tail.begin() + static_cast<long>(nSteps));
                            anchorTick = want;
                            anchored = true;
                        }
                    }
                }
            }
            while (!soft.empty() && soft.back().tick > tick - rung.back) {
                soft.pop_back();
                ++res.stats.softRestarts;
            }
            beam = std::min(opt.maxBeamWidth, opt.beamWidth * rung.mul);
            qY = opt.quantY / static_cast<float>(1 << rung.q);
            qV = opt.quantV / static_cast<float>(1 << rung.q);
            applyZone(lastLiveX);
            if (anchored && debugOptions().check)
                std::fprintf(stderr,
                             "[anchor] tick %d : restart from the ancestor of the best state at tick %d (x %.0f y "
                             "%.0f), beam %d\n",
                             tick, anchorTick, anchorGame.p1.x, anchorGame.p1.y, beam);
        } else {
            bool learnedDead = false;
            {
                static bool const dbgDead = debugOptions().check != 0;
                int const bestIdx = live.empty() ? -1 : bestOf(false);
                uint8_t firstMode = 0;
                bool sameMode = true;
                bool dualAny = false;
                for (Node* p : live)
                    if (p->g) {
                        uint8_t const mode = modeBitOf(p->g->p1);
                        if (!firstMode)
                            firstMode = mode;
                        else if (mode != firstMode) {
                            sameMode = false;
                            break;
                        }
                        if (p->g->isDualMode) dualAny = true;
                    }
                int nActive = 0;
                for (auto const& portal : deadPortals)
                    if (!portal.off) ++nActive;
                bool const canLearn = !loopImpasse && sameMode && !dualAny && bestIdx >= 0 &&
                                      live[static_cast<size_t>(bestIdx)]->g && nActive < 16 &&
                                      deadLearns < kMaxDeadLearns && !start.world->platformer;
                if (!canLearn && dbgDead)
                    std::fprintf(stderr, "[maze] tick %d (x~%.0f) : nothing learned (%s)\n", tick, lastLiveX,
                                 loopImpasse                    ? "loop"
                                 : !sameMode                    ? "mixed modes"
                                 : dualAny                      ? "dual"
                                 : nActive >= 16                ? "16 portals already learned"
                                 : deadLearns >= kMaxDeadLearns ? "enough tries"
                                                                : "no state");
                if (canLearn) {
                    size_t hardIdx = hard.size() - 1;
                    while (hardIdx > 0 && tick - hard[hardIdx - 1].tick <= 14400) --hardIdx;
                    std::vector<uint8_t> acts(
                            inputs.begin() + static_cast<long>(std::min(hard[hardIdx].inputsLen, inputs.size())),
                            inputs.end());
                    {
                        std::vector<uint8_t> const tl = tailTo(bestIdx);
                        acts.insert(acts.end(), tl.begin(), tl.end());
                    }
                    Game g = hard[hardIdx].g;
                    World const& world = *start.world;
                    float const fromX = g.p1.x - 200.f;
                    float const toX = lastLiveX + 400.f;
                    std::vector<int> modePortals;
                    for (size_t i = 0; i < world.objs.size(); ++i) {
                        Obj const& o = world.objs[i];
                        if (!isGameModePortal(o.type) || o.rect.maxX() < fromX || o.rect.x > toX) continue;
                        if (!portalTakenBy(g, static_cast<int>(i))) modePortals.push_back(static_cast<int>(i));
                    }

                    struct Take {
                        int idx;
                        int tick;
                        float x;
                    };
                    std::vector<Take> taken;
                    std::vector<char> wasTaken(modePortals.size(), 0);
                    for (uint8_t act : acts) {
                        stepTick(g, act);
                        if (g.died) break;
                        for (size_t j = 0; j < modePortals.size(); ++j) {
                            if (wasTaken[j]) continue;
                            Obj const& o = world.objs[static_cast<size_t>(modePortals[j])];
                            if (o.rect.maxX() < g.p1.x - 150.f || o.rect.x > g.p1.x + 150.f) continue;
                            if (portalTakenBy(g, modePortals[j])) {
                                wasTaken[j] = 1;
                                taken.push_back(Take{modePortals[j], g.tick, g.p1.x});
                            }
                        }
                    }
                    int pick = -1;
                    int pickDead = -1;
                    for (size_t j = taken.size(); j-- > 0 && pick < 0;) {
                        int const uid = world.objs[static_cast<size_t>(taken[j].idx)].uid;
                        int deadIdx = -1;
                        for (size_t q = 0; q < deadPortals.size(); ++q)
                            if (deadPortals[q].uid == uid) {
                                deadIdx = static_cast<int>(q);
                                break;
                            }
                        if (deadIdx < 0)
                            pick = static_cast<int>(j);
                        else if (deadPortals[static_cast<size_t>(deadIdx)].off &&
                                 deadLearns > deadPortals[static_cast<size_t>(deadIdx)].offAt) {
                            pick = static_cast<int>(j);
                            pickDead = deadIdx;
                        }
                    }
                    if (dbgDead) {
                        std::fprintf(stderr,
                                     "[maze] tick %d (x~%.0f) : layer in mode %c ; mode portals of the best path since "
                                     "tick %d :",
                                     tick, lastLiveX, modeCharOf(live[static_cast<size_t>(bestIdx)]->g->p1),
                                     hard[hardIdx].tick);
                        for (auto const& take : taken)
                            std::fprintf(stderr, " %d(x %.0f, t %d)", world.objs[static_cast<size_t>(take.idx)].uid,
                                         take.x, take.tick);
                        std::fprintf(stderr, "\n");
                    }
                    if (pick >= 0 && taken[static_cast<size_t>(pick)].tick > hard.front().tick) {
                        Take const& take = taken[static_cast<size_t>(pick)];
                        Obj const& o = world.objs[static_cast<size_t>(take.idx)];
                        if (pickDead >= 0) {
                            deadPortals[static_cast<size_t>(pickDead)].off = false;
                            deadPortals[static_cast<size_t>(pickDead)].tick = take.tick;
                        } else
                            deadPortals.push_back(DeadPortal{o.uid, take.idx, o.rect.midX(), take.tick, false, 0});
                        ++deadLearns;
                        learnedDead = true;
                        popBeforeTick = take.tick - 240;
                        popStreak = 0;
                        fromStart = false;
                        lastPopRestartTick = std::numeric_limits<int>::max();
                        if (dbgDead)
                            std::fprintf(
                                    stderr,
                                    "[maze] tick %d (x~%.0f) : portal uid %d (x %.0f y %.0f, taken at tick %d) leads "
                                    "to a dead end - a lineage taking it dies ; restart from before tick %d\n",
                                    tick, lastLiveX, o.uid, o.x, o.y, take.tick, take.tick - 240);
                    }
                }
                if (!learnedDead && !deadPortals.empty()) {
                    size_t nDead = 0;
                    size_t nDeadP = 0;
                    for (auto const& v : killersOf)
                        for (int u : v) {
                            ++nDead;
                            if (u == kDeadPortalKiller) ++nDeadP;
                        }
                    if (nDeadP > 0 && nDeadP * 2 >= nDead)
                        for (auto& portal : deadPortals) {
                            if (portal.off || !(portal.x >= lastLiveX - 600.f && portal.x <= lastLiveX + 120.f))
                                continue;
                            portal.off = true;
                            portal.offAt = deadLearns;
                            if (dbgDead)
                                std::fprintf(stderr,
                                             "[maze] tick %d (x~%.0f) : learned portal uid %d cannot be avoided ; "
                                             "given up\n",
                                             tick, lastLiveX, portal.uid);
                        }
                }
            }

            int nPops = 1 << std::min(popStreak, 4);
            int const wantPops = nPops;
            int hardPopped = 0;
            noSoftBefore = std::max(noSoftBefore, trapSoftFrom >= 0 ? std::min(tick + 1, trapSoftFrom) : tick + 1);
            trapSoftFrom = -1;

            auto restartTick = [&] { return soft.empty() ? hard.back().tick : soft.back().tick; };

            auto popOne = [&] {
                if (!soft.empty()) {
                    soft.pop_back();
                    ++res.stats.softRestarts;
                    return true;
                }
                if (hard.size() > 1) {
                    hard.pop_back();
                    ++res.stats.hardPops;
                    ++hardPopped;
                    return true;
                }
                return false;
            };
            while (nPops > 0 && popOne()) --nPops;
            if (popBeforeTick != std::numeric_limits<int>::max()) {
                while (restartTick() > popBeforeTick && popOne()) {}
                popBeforeTick = std::numeric_limits<int>::max();
            }
            while (restartTick() >= lastPopRestartTick && popOne()) {}
            lastPopRestartTick = restartTick();
            bool const poppedHard = hardPopped > 0;
            if (poppedHard) {
                inputs.resize(hard.back().inputsLen);
                if (opt.onPrefix) opt.onPrefix(inputs);
                if (chkLevel() >= 2) {
                    Game replay = start;
                    for (uint8_t act : inputs) stepTick(replay, act);
                    std::fprintf(stderr,
                                 "[check] back off : %d hard points removed, inputs %zu -> (%.3f,%.3f) tick %d, hard "
                                 "point (%.3f,%.3f) tick %d\n",
                                 hardPopped, inputs.size(), replay.p1.x, replay.p1.y, replay.tick, hard.back().g.p1.x,
                                 hard.back().g.p1.y, hard.back().tick);
                }
            }
            if (nPops == wantPops && !learnedDead) {
                finalResult = finish(-1, false, "no path found (tick " + std::to_string(tick) + ")");
                return Flow::Return;
            }
            fromStart = !learnedDead && soft.empty() && hard.size() <= 1;
            ++popStreak;
            tieSalt = KeyHash::splitmix(tieSalt + 0x9e3779b97f4a7c15ull);
            beam = std::min(opt.maxBeamWidth, opt.beamWidth * 16);
            if (loopImpasse) beam = opt.beamWidth;
            qY = opt.quantY;
            qV = opt.quantV;
            gapNow = opt.minPressGap;
        }
        return Flow::Next;
    }

    // moves the search back to the restart tick
    auto SearchRun::restartTick() -> Flow {
        widenedAtTick = tick;
        spotTick = tick;
        spotX = std::max(spotX, lastLiveX);
        capFar(anchored ? anchorTick : (soft.empty() ? hard.back().tick : soft.back().tick));
        if (anchored)
            restartFrom(anchorGame, anchorTick, anchorTail, anchorSpan);
        else if (!soft.empty())
            restartFromSoft(soft.back());
        else
            restartFrom(hard.back().g, hard.back().tick, {}, hard.back().span);
        tick = rootTick;
        ladderRoot = rootTick;
        lastPublishCheck = tick;
        lastSoft = tick;
        return Flow::Continue;
        return Flow::Next;
    }

}
