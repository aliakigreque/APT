// Portal debt: when the lowest debt of the layer rises, a portal was just missed and the search tries again
// from before it, climbing a ladder of wider tries.

#include "search_internal.hpp"

namespace ap::sim {
    // when the lowest portal debt of the layer rises, a portal was just missed: restarts from before it, trying a
    // forced flying exit or a must-take orb first
    auto SearchRun::checkDebtRise() -> Flow {
        if (!liveNext.empty() && opt.missedSpeedCost > 0) {
            measureDebt();
            onDebtRise();
            if (Flow const flow = keepDebtLevel(); flow != Flow::Next) return flow;
        }
        return Flow::Next;
    }

    // lowest portal debt of the next layer, with and without the part owed to plain portals
    auto SearchRun::measureDebt() -> void {
        minDebt = std::numeric_limits<int>::max();
        minSDebt = std::numeric_limits<int>::max();
        for (Node* p : liveNext) {
            minDebt = std::min(minDebt, p->debt);
            minSDebt = std::min(minSDebt, p->debt - p->pdebt);
        }
        portalOnly = prevMinSDebt >= 0 && minSDebt <= prevMinSDebt;
        restarted = false;
        flyExitTry = false;
        mtTry = false;
    }

    // the lowest debt rose since the last tick: a portal was missed, so tries again from before it
    auto SearchRun::onDebtRise() -> void {
        if (prevMinDebt >= 0 && minDebt > prevMinDebt) {
            findMissedPortal();
            tryFlyExit();
            checkNominalBeam();
            restartBeforeMiss();
        }
    }

    // checks that a state of the old lowest debt is the parent of a state whose debt rose
    auto SearchRun::findMissedPortal() -> void {
        missed = false;
        for (Node* p : liveNext) {
            if (p->debt <= prevMinDebt || p->parent < 0 || static_cast<size_t>(p->parent) >= live.size()) continue;
            if (p->outJust) continue;
            if (live[static_cast<size_t>(p->parent)]->debt == prevMinDebt) {
                missed = true;
                break;
            }
        }
    }

    // the miss is a flying exit: retries it as a forced exit, once per spot (flyExitTries)
    auto SearchRun::tryFlyExit() -> void {
        if (missed && opt.flyExitTries && !liveNext.empty()) {
            bool fly = false;
            int flyCluster = 0;
            float flyPassX = 0.f;
            for (Node* p : liveNext) {
                if (fly) break;
                if (p->debt <= prevMinDebt || p->parent < 0 || static_cast<size_t>(p->parent) >= live.size() || !p->g)
                    continue;
                if (live[static_cast<size_t>(p->parent)]->debt != prevMinDebt || p->outJust) continue;
                Player const& player = p->g->p1;
                if (!(player.isShip || player.isBird || player.isDart || player.isSwing) || !(p->debtX0 < 0.f))
                    continue;
                float const missX = -p->debtX0;
                auto it = std::lower_bound(cues.passX.begin(), cues.passX.end(),
                                           std::make_pair(missX - 0.5f, -(1 << 30)));
                for (; it != cues.passX.end() && it->first <= missX + 0.5f && !fly; ++it)
                    for (int m : clusterOf(cues, it->second).members)
                        if (flyExitType(player, p->g->world->objs[static_cast<size_t>(m)].type)) {
                            fly = true;
                            flyCluster = it->second;
                            flyPassX = it->first;
                            break;
                        }
            }
            if (fly) {
                bool tried = false;
                for (auto const& e : pdebtSpots)
                    if (std::abs(e.first - tick) <= 60) {
                        tried = true;
                        break;
                    }
                if (!tried && flyTriesExhausted(lastLiveX)) tried = true;
                if (!tried) {
                    {
                        bool known = false;
                        for (auto const& mt : mustTakes)
                            if (mt.cluster == flyCluster) {
                                known = true;
                                break;
                            }
                        if (!known) {
                            mustTakes.push_back(MustTake{flyCluster, flyPassX, flyPassX + 900.f});
                            mtTry = true;
                        }
                    }
                    missed = false;
                    flyExitTry = true;
                    if (debugOptions().check)
                        std::fprintf(stderr,
                                     "[flying] tick %d (x~%.0f) : flying mode exit missed by the whole beam (x %.0f) ; "
                                     "one try\n",
                                     tick, lastLiveX,
                                     liveNext[0]->debtX0 < 0.f ? -liveNext[0]->debtX0 : liveNext[0]->debtX0);
                }
            }
        }
    }

    // in live mode, a miss right after a beam reduction is retried at the nominal beam
    auto SearchRun::checkNominalBeam() -> void {
        nominal = false;
        if (!missed && opt.playedTicks && lastReducedTick >= tick - 240 && tick >= noReduceUntil) {
            nominal = true;
            for (int spot : nominalSpots)
                if (std::abs(spot - tick) <= 48) {
                    nominal = false;
                    break;
                }
        }
    }

    // restarts from before the missed portal, wider each time the same spot fails
    auto SearchRun::restartBeforeMiss() -> void {
        if (nominal) {
            nominalSpots.push_back(tick);
            noReduceUntil = tick + 480;
            if (debugOptions().check)
                std::fprintf(stderr,
                             "[live] tick %d (x~%.0f) : wanted path lost under a reduced beam (debt %d -> %d) ; back "
                             "to the normal beam\n",
                             tick, lastLiveX, prevMinDebt, minDebt);
            while (!soft.empty() && soft.back().tick > tick - 240) {
                soft.pop_back();
                ++res.stats.softRestarts;
            }
            widenedAtTick = tick;
            spotTick = tick;
            spotX = std::max(spotX, lastLiveX);
            for (auto& b : nextBuckets)
                for (auto& node : b) node.kept = false;
            releaseUnkept(nextBuckets);
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
            restarted = true;
        } else if (!missed) {
            findDebtRung();
            skipDebtRungs();
            limitLiveDebtTries();
            forceFlyExit();
            restartAtDebtRung();
        } else if (debugOptions().check)
            std::fprintf(stderr, "[debt] tick %d (x~%.0f) : cluster out of reach, debt %d accepted\n", tick, lastLiveX,
                         minDebt);
    }

    auto SearchRun::effFrom(int back) -> int {
        for (auto it = soft.rbegin(); it != soft.rend(); ++it)
            if (it->tick <= tick - back) return it->tick;
        return hard.back().tick;
    }

    auto SearchRun::effBeam(Rung const& rung, int from) -> int {
        int width = std::min(opt.maxBeamWidth, opt.beamWidth * rung.mul);
        if (opt.playedTicks) {
            int const window = std::max(240, tick - from);
            width = std::min(width, opt.beamWidth * std::max(4, (kFourSeconds * 4) / window));
        }
        return width;
    }

    auto SearchRun::skipRung(int k) -> bool {
        if (ladder[k].anchor) return true;
        if (opt.playedTicks && !portalOnly) {
            for (int j = 0; j < k; ++j)
                if (!ladder[j].anchor && ladder[j].back > ladder[k].back) return true;
            int const from = effFrom(ladder[k].back);
            int const width = effBeam(ladder[k], from);
            for (auto const& tried : debtTried)
                if (std::abs(tried.spot - tick) <= 480 && tried.from == from && tried.beam >= width &&
                    tried.q >= ladder[k].q)
                    return true;
        }
        return false;
    }

    // the rung reached at this spot, counted apart for misses of plain portals
    auto SearchRun::findDebtRung() -> void {
        debtRungOf = nullptr;
        if (portalOnly) {
            for (auto& e : pdebtSpots)
                if (std::abs(e.first - tick) <= 60) {
                    debtRungOf = &e.second;
                    break;
                }
            if (!debtRungOf) {
                pdebtSpots.emplace_back(tick, 0);
                debtRungOf = &pdebtSpots.back().second;
            }
        } else
            for (auto& e : debtSpots)
                if (std::abs(e.first - tick) <= 480) {
                    debtRungOf = &e.second;
                    break;
                }
        if (!debtRungOf) {
            int startRung = 0;
            for (auto const& e : spotRungs)
                if (std::abs(e.first - tick) <= 480) startRung = std::max(startRung, e.second);
            if (startRung >= 6) startRung = kNLadder;
            debtSpots.emplace_back(tick, startRung);
            debtRungOf = &debtSpots.back().second;
            debtTickMark = -1;
        }
    }

    // skips the rungs that would not change anything, and accepts the debt without a try when only touch
    // triggers were missed or the flying exit tries already failed here
    auto SearchRun::skipDebtRungs() -> void {
        while (*debtRungOf < kNLadder && skipRung(*debtRungOf)) ++*debtRungOf;
        touchOnlyDebt = false;
        {
            auto isExtraAt = [&](float x) {
                float const absX = std::fabs(x);
                auto it =
                        std::lower_bound(cues.passX.begin(), cues.passX.end(), std::make_pair(absX - 0.5f, -(1 << 30)));
                bool any = false;
                bool extra = true;
                for (; it != cues.passX.end() && it->first <= absX + 0.5f; ++it) {
                    any = true;
                    if (it->second >= 0) extra = false;
                }
                return any && extra;
            };
            int nSurv = 0;
            bool allExtra = true;
            for (Node* p : liveNext) {
                if (p->debt != minDebt || p->debtX0 == 0.f) continue;
                ++nSurv;
                if (!isExtraAt(p->debtX0) || (p->debtX1 != 0.f && !isExtraAt(p->debtX1))) {
                    allExtra = false;
                    break;
                }
            }
            touchOnlyDebt = portalOnly && nSurv > 0 && allExtra;
        }
        flyDebt = (flyExitTry || (opt.flyExitTries && !liveNext.empty() && liveNext[0]->g &&
                                  (liveNext[0]->g->p1.isDart || liveNext[0]->g->p1.isShip ||
                                   liveNext[0]->g->p1.isBird || liveNext[0]->g->p1.isSwing)));
        if (portalOnly && *debtRungOf > (flyDebt ? 2 : 0)) *debtRungOf = kNLadder;
        if (touchOnlyDebt && opt.playedTicks) {
            if (*debtRungOf < kNLadder && debugOptions().check)
                std::fprintf(
                        stderr,
                        "[debt] tick %d (x~%.0f) : only touch triggers to touch ; debt accepted without a try (live)\n",
                        tick, lastLiveX);
            *debtRungOf = kNLadder;
        }
        if (portalOnly && flyDebt && *debtRungOf < kNLadder && flyTriesExhausted(lastLiveX)) {
            if (debugOptions().check)
                std::fprintf(stderr,
                             "[flying] tick %d (x~%.0f) : exit tries already failed in this zone ; debt accepted "
                             "without a try\n",
                             tick, lastLiveX);
            *debtRungOf = kNLadder;
        }
    }

    // live mode: no try for a plain portal miss when the search is short on time or close to the player
    auto SearchRun::limitLiveDebtTries() -> void {
        if (portalOnly && *debtRungOf < kNLadder) {
            double const elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
            bool waveExit = flyDebt;
            if (!waveExit && !liveNext.empty() && liveNext[0]->g && liveNext[0]->g->p1.isDart) {
                auto it = std::lower_bound(cues.passX.begin(), cues.passX.end(),
                                           std::make_pair(lastLiveX - 900.f, -(1 << 30)));
                for (; it != cues.passX.end() && it->first <= lastLiveX + 1.f; ++it)
                    if (clusterOf(cues, it->second).modeOut) {
                        waveExit = true;
                        break;
                    }
            }
            if (!waveExit && opt.playedTicks && elapsed > 2.0 &&
                static_cast<double>(tick - res.startTick) < 480.0 * elapsed)
                *debtRungOf = kNLadder;
            if (opt.playedTicks && tick - opt.playedTicks->load(std::memory_order_relaxed) < 480)
                *debtRungOf = kNLadder;
        }
    }

    // on the first rung, a missed flying exit becomes a forced exit (must take) for the try
    auto SearchRun::forceFlyExit() -> void {
        if (portalOnly && *debtRungOf == 0 && !start.world->platformer) {
            float forcedX = 0.f;
            for (Node* p : liveNext) {
                if (p->debt != minDebt || !p->g || !(p->debtX0 < 0.f)) continue;
                Player const& player = p->g->p1;
                if (!(player.isShip || player.isBird || player.isDart || player.isSwing)) continue;
                float const missX = -p->debtX0;
                auto it = std::lower_bound(cues.passX.begin(), cues.passX.end(),
                                           std::make_pair(missX - 0.5f, -(1 << 30)));
                for (; it != cues.passX.end() && it->first <= missX + 0.5f; ++it) {
                    bool isExit = false;
                    for (int m : clusterOf(cues, it->second).members)
                        if (flyExitType(player, p->g->world->objs[static_cast<size_t>(m)].type)) {
                            isExit = true;
                            break;
                        }
                    if (!isExit) continue;
                    bool known = false;
                    for (auto const& mustTake : mustTakes)
                        if (mustTake.cluster == it->second) {
                            known = true;
                            break;
                        }
                    if (!known) {
                        mustTakes.push_back(MustTake{it->second, it->first, it->first + 900.f});
                        forcedX = it->first;
                        mtTry = true;
                    }
                    break;
                }
                if (forcedX != 0.f) break;
            }
            if (forcedX != 0.f && debugOptions().check)
                std::fprintf(stderr, "[flying] tick %d (x~%.0f) : flying mode exit (x %.0f) forced during the try\n",
                             tick, lastLiveX, forcedX);
        }
    }

    // climbs one rung and restarts from its restart point with its beam
    auto SearchRun::restartAtDebtRung() -> void {
        if (!restarted && *debtRungOf < kNLadder) {
            Rung const rung = ladder[*debtRungOf];
            gapNow = gapForRung(*debtRungOf);
            ++*debtRungOf;
            ++res.stats.widenings;
            ++res.stats.debtRungs;
            if (debtTickMark < 0) debtTickMark = simulated.load();
            if (debugOptions().check)
                std::fprintf(stderr,
                             "[debt] tick %d (x~%.0f) : no state left on the wanted path (debt %d -> %d) ; back %d "
                             "ticks, beam x%d, q/%d\n",
                             tick, lastLiveX, prevMinDebt, minDebt, rung.back, rung.mul, 1 << rung.q);
            if (debugOptions().check) {
                std::fprintf(stderr, "    survivors (debt %d) : missed clusters", minDebt);
                int shownN = 0;
                std::vector<std::pair<float, float>> seenX;
                for (Node* p : liveNext) {
                    if (p->debt != minDebt || shownN >= 6) continue;
                    std::pair<float, float> const debtXs{p->debtX0, p->debtX1};
                    if (std::find(seenX.begin(), seenX.end(), debtXs) != seenX.end()) continue;
                    seenX.push_back(debtXs);
                    ++shownN;
                    std::fprintf(stderr, " [%.0f %.0f]", p->debtX1, p->debtX0);
                }
                std::fprintf(stderr, " (negative: mode portal)\n");
            }
            while (!soft.empty() && soft.back().tick > tick - rung.back) {
                soft.pop_back();
                ++res.stats.softRestarts;
            }
            int const from = soft.empty() ? hard.back().tick : soft.back().tick;
            beam = (opt.playedTicks && !portalOnly) ? effBeam(rung, from)
                                                    : std::min(opt.maxBeamWidth, opt.beamWidth * rung.mul);
            if (mtTry && portalOnly) beam = std::min(beam, opt.beamWidth * 2);
            if (opt.playedTicks && !portalOnly) debtTried.push_back(DebtTry{tick, from, beam, rung.q});
            qY = opt.quantY / static_cast<float>(1 << rung.q);
            qV = opt.quantV / static_cast<float>(1 << rung.q);
            applyZone(lastLiveX);
            widenedAtTick = tick;
            spotTick = tick;
            spotX = std::max(spotX, lastLiveX);
            for (auto& b : nextBuckets)
                for (auto& node : b) node.kept = false;
            releaseUnkept(nextBuckets);
            capFar(from);
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
            restarted = true;
        } else if (!restarted) {
            ++res.stats.debtAccepted;
            bool const debtWasTried = debtTickMark >= 0;
            if (debtTickMark >= 0) {
                res.stats.debtTicks += simulated.load() - debtTickMark;
                debtTickMark = -1;
            }
            if (flyDebt && portalOnly && debtWasTried) flyTryFails.push_back(lastLiveX);
            if (debugOptions().check)
                std::fprintf(stderr, "[debt] tick %d (x~%.0f) : debt %d accepted (ladder used up)\n", tick, lastLiveX,
                             minDebt);
        }
    }

    // remembers the debt levels for the next tick (unless the search restarted)
    auto SearchRun::keepDebtLevel() -> Flow {
        if (restarted) return Flow::Continue;
        prevMinDebt = minDebt;
        prevMinSDebt = minSDebt;
        return Flow::Next;
    }

    // a dead end inside the zone of a forced exit lifts that exit
    auto SearchRun::liftMustTakes() -> void {
        if (liveNext.empty() && !mustTakes.empty()) {
            size_t const nBefore = mustTakes.size();
            mustTakes.erase(std::remove_if(mustTakes.begin(), mustTakes.end(),
                                           [&](MustTake const& mustTake) {
                                               return lastLiveX >= mustTake.passX - 600.f && lastLiveX <= mustTake.x1;
                                           }),
                            mustTakes.end());
            if (mustTakes.size() != nBefore) flyTryFails.push_back(lastLiveX);
            if (mustTakes.size() != nBefore && debugOptions().check)
                std::fprintf(stderr,
                             "[flying] tick %d (x~%.0f) : dead end in the zone of a forced exit ; exit lifted\n", tick,
                             lastLiveX);
        }
    }

}
