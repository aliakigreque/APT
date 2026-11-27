// Expansion of a layer: every live state is stepped with each of its inputs and every child is checked and scored (the hot part of the search).

#include "search_internal.hpp"

namespace ap::sim {
    auto SearchRun::expand(int worker) -> void {
        auto& out = nextBuckets[static_cast<size_t>(worker)];
        auto& seenLocal = localSeen[static_cast<size_t>(worker)];
        out.clear();
        seenLocal.clear();
        out.reserve(nLive / static_cast<size_t>(nThreads) * 2 + 8);
        long nSteps = 0;
        long nDone = 0;
        Arena& arena = arenas[static_cast<size_t>(worker)];
        auto const workStart = std::chrono::steady_clock::now();

        ExpandWorker wk{worker, out, seenLocal, nSteps, arena};
        for (int src = 0; src < nThreads; ++src) {
            int const srcWorker = (worker + src) % nThreads;
            auto const& srcList = part[static_cast<size_t>(srcWorker)];
            auto& cursor = partCursor[static_cast<size_t>(srcWorker)];
            for (;;) {
                size_t const chunkBegin = cursor.fetch_add(chunk);
                if (chunkBegin >= srcList.size()) break;
                size_t const chunkEnd = std::min(srcList.size(), chunkBegin + chunk);
                nDone += static_cast<long>(chunkEnd - chunkBegin);
                for (size_t k = chunkBegin; k < chunkEnd; ++k) {
                    expandNode(wk, srcList[k]);
                }
            }
        }
        simulated += nSteps;
        busy[static_cast<size_t>(worker)] +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - workStart).count();
        nodesDone[static_cast<size_t>(worker)] += nDone;
    }

    // expands one live state: takes what its children need from it, then steps one child per input
    auto SearchRun::expandNode(ExpandWorker& wk, size_t i) -> void {
        Node& parent = *live[i];
        Game* const parentGame = parent.g;
        if (nThreads > 1) trigLocalize(*parentGame, localCtx[static_cast<size_t>(wk.worker)]);
        bool const holdingBefore = parentGame->p1.holdingJump;
        bool const holding2Before = parentGame->p2Btn;
        bool const twoButtons = parentGame->twoPlayerMode && parentGame->isDualMode;

        int const nCost = parent.cost;
        int const nFrag = parent.frag;
        int const nLastPress = parent.lastPress;
        int const nLastRelease = parent.lastRelease;
        int const nDebt = parent.debt;
        int const nPdebt = parent.pdebt;
        int const nOut = parent.outTicks;
        int const nBound = parent.boundTicks;
        int const nAway = parent.away;
        uint8_t const nFlyMiss = parent.flyMiss;
        char const parentMode = modeCharOf(parentGame->p1);
        float const nDebtX0 = parent.debtX0;
        float const nDebtX1 = parent.debtX1;
        float const nBackX = parent.backX;
        uint64_t const nBackSig = parent.backSig;
        uint64_t const nBackSigP = parent.backSigP;
        uint8_t const nBackN = parent.backN;
        int const nBackTick0 = parent.backTick0;
        Span const nSpan = parent.span;
        uint8_t const parentOwner = parent.owner;
        float const prevX = parentGame->p1.x;
        uint16_t const prevTp1 = parentGame->p1.tpCount;
        uint16_t const prevTp2 = parentGame->p2.tpCount;
        unsigned const prevItemChanges = parentGame->itemChanges;
        bool const persistWatch = opt.playedTicks && !parentGame->world->persistItemIds.empty();
        int const prevPersist = persistWatch ? trigPersistChanged(*parentGame) : 0;
        int const prevTraps = cues.traps.empty() ? 0 : trapsTouchedNear(*parentGame, cues, prevX);

        int itemV0[4] = {0, 0, 0, 0};
        for (size_t q = 0; q < itemTargets.size() && q < 4; ++q) itemV0[q] = itemTargetGap(*parentGame, itemTargets[q]);
        int paceV0[8];
        for (size_t q = 0; q < paceItems.size() && q < 8; ++q)
            paceV0[q] = trigItemValue(*parentGame, paceItems[q].item);
        static thread_local std::vector<int> countV0;
        countV0.resize(countTargets.size());
        for (size_t q = 0; q < countTargets.size(); ++q) countV0[q] = trigItemValue(*parentGame, countTargets[q].item);
        uint32_t needMask = 0;
        for (size_t q = 0; q < needOff.size() && q < 32; ++q)
            if (trigGroupOn(*parentGame, needOff[q])) needMask |= 1u << q;
        bool const prevObbFragile = parentGame->obbGap < opt.fragileObbGap;
        float const prevHaz = parentGame->hazGap;
        float const prevSpeed = parentGame->p1.playerSpeed;
        SmallVec<std::pair<int, bool>, 8> openPortals;
        openClusters(*parentGame, prevX, cues, openPortals);
        bool const prevRotAmb = parentGame->rotAmb;
        RingList const prevRings1 = parentGame->p1.touchingRings;
        RingList prevRings2;
        if (parentGame->isDualMode) prevRings2 = parentGame->p2.touchingRings;
        bool const nRing = parent.ringSincePress;
        int const gapHere = gapNow;
        bool const canPress = holdingBefore || tick - nLastPress >= gapHere || (nRing && tick - nLastPress >= std::min(gapHere, kRingGap));

        uint8_t acts[4];
        int nActs = 0;
        bool const canPress2 = holding2Before || tick - nLastPress >= gapHere || (nRing && tick - nLastPress >= std::min(gapHere, kRingGap));
        bool const segMode = parentGame->p1.isDart && !twoButtons;
        int const minSeg = std::max(1, opt.minPressGap / 3);
        for (int buttons = 0; buttons < (twoButtons ? 4 : 2); ++buttons) {
            if ((buttons & 1) && !canPress) continue;
            if ((buttons & 2) && !canPress2) continue;
            acts[nActs++] = static_cast<uint8_t>(buttons);
        }
        unsigned relItemDelta = ~0u;
        uint32_t deadTakenP = 0;
        if (!deadPortals.empty())
            for (size_t q = 0; q < deadPortals.size(); ++q)
                if (!deadPortals[q].off && std::fabs(deadPortals[q].x - parentGame->p1.x) < 160.f &&
                    portalTakenBy(*parentGame, deadPortals[q].idx))
                    deadTakenP |= 1u << q;

        ParentInfo pn{i,         parent,         parentGame, holdingBefore,   holding2Before,
                    nCost,       nFrag,          nLastPress, nLastRelease,    nDebt,
                    nPdebt,      nOut,           nBound,     nAway,           nFlyMiss,
                    parentMode,  nDebtX0,        nDebtX1,    nBackX,          nBackSig,
                    nBackSigP,   nBackN,         nBackTick0, nSpan,           parentOwner,
                    prevX,       prevTp1,        prevTp2,    prevItemChanges, persistWatch,
                    prevPersist, prevTraps,      itemV0,     paceV0,          countV0,
                    needMask,    prevObbFragile, prevHaz,    prevSpeed,       openPortals,
                    prevRotAmb,  prevRings1,     prevRings2, nRing,           acts,
                    nActs,       segMode,        minSeg,     relItemDelta,    deadTakenP};
        for (int ai = 0; ai < nActs; ++ai) {
            expandAction(wk, pn, ai);
        }
    }

    // steps the child of input acts[ai] and scores it; a child that dies or is dropped is not kept
    auto SearchRun::expandAction(ExpandWorker& wk, ParentInfo& pn, int ai) -> void {
        ChildStep c{};
        stepChild(wk, pn, ai, c);
        applyLearnedKills(wk, pn, ai, c);
        checkPresence(wk, pn, ai, c);
        checkStuckAndMenus(wk, pn, ai, c);
        checkEscape(wk, pn, ai, c);
        checkFlyMiss(wk, pn, ai, c);
        checkGoals(wk, pn, ai, c);
        checkLoopBack(wk, pn, ai, c);
        checkExtent(wk, pn, ai, c);
        if (!dropDead(wk, pn, ai, c)) return;
        makeChild(wk, pn, ai, c);
        addInputCosts(wk, pn, ai, c);
        finishChild(wk, pn, ai, c);
    }

    // steps the child: the last input reuses the parent's game, the others a copy from the worker's arena
    auto SearchRun::stepChild(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> void {
        c.act = pn.acts[ai];
        c.hold = (c.act & 1) != 0;
        c.isPress = (c.hold && !pn.holdingBefore) || ((c.act & 2) && !pn.holding2Before);
        if (ai == pn.nActs - 1)
            c.game = pn.parentGame;
        else {
            c.game = wk.arena.alloc();
            *c.game = *pn.parentGame;
        }
        stepTick(*c.game, c.act);
        ++wk.nSteps;
        if (c.act == 0) pn.relItemDelta = c.game->itemChanges - pn.prevItemChanges;
        c.inEndZone = c.game->p1.x >= cues.endZoneX;
    }

    // kills the child where the search learned it must not go: doomed void, dead portals, avoided objects, loop zones, trap corridors and jumps back into the part already played
    auto SearchRun::applyLearnedKills(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> void {
        if (!c.game->died && !c.game->isDualMode && !c.inEndZone && c.game->p1.x < cues.voidEndX &&
            cues.voidHull.doomed(c.game->p1) && (c.game->p1.isUpsideDown || pn.nAway >= 0 || !cues.presence.ok()))
            c.game->died = true;

        if (!c.game->died && !deadPortals.empty())
            for (size_t q = 0; q < deadPortals.size(); ++q) {
                DeadPortal const& portal = deadPortals[q];
                if (portal.off || (pn.deadTakenP >> q) & 1u || std::fabs(portal.x - c.game->p1.x) >= 150.f) continue;
                if (portalTakenBy(*c.game, portal.idx)) {
                    c.game->died = true;
                    c.game->killerUid = kDeadPortalKiller;
                    break;
                }
            }

        if (!c.game->died && !avoidUids.empty()) {
            for (int uid : c.game->p1.ringRelatedSet)
                if (std::binary_search(avoidUids.begin(), avoidUids.end(), uid)) {
                    c.game->died = true;
                    c.game->killerUid = uid;
                    break;
                }
            if (!c.game->died && c.game->isDualMode)
                for (int uid : c.game->p2.ringRelatedSet)
                    if (std::binary_search(avoidUids.begin(), avoidUids.end(), uid)) {
                        c.game->died = true;
                        c.game->killerUid = uid;
                        break;
                    }
        }

        if (!c.game->died && !loopZones.empty() && !c.game->isPlatformer)
            for (auto const& z : loopZones)
                if (c.game->p1.x >= z.x0 && c.game->p1.x <= z.x1 && std::fabs(c.game->p1.y - z.y) <= 45.f) {
                    c.game->died = true;
                    c.game->killerUid = 0;
                    break;
                }

        if (!c.game->died && !trapCorr.empty() && !c.game->isPlatformer && !c.inEndZone)
            for (auto const& corr : trapCorr) {
                if (corr.off) continue;
                long const col = static_cast<long>(std::floor((c.game->p1.x - corr.x0) / 30.f));
                if (col < 0 || col >= static_cast<long>(corr.band.size())) continue;
                auto const& band = corr.band[static_cast<size_t>(col)];
                if (c.game->p1.y >= band[0] && c.game->p1.y <= band[1] &&
                    (corr.modes[static_cast<size_t>(col)] & modeBitOf(c.game->p1))) {
                    c.game->died = true;
                    c.game->killerUid = kTrapKiller;
                    break;
                }
            }

        if (!c.game->died && revisitOn && c.game->p1.x < pn.prevX - kRevisitJump && !c.game->isPlatformer &&
            revisited(*c.game)) {
            bool ok = false;
            for (auto const& z : revisitOk)
                if (c.game->p1.x >= z.first && c.game->p1.x <= z.second) {
                    ok = true;
                    break;
                }
            if (!ok) {
                c.game->died = true;
                c.game->killerUid = kRevisitKiller;
                revisitKillsOf[static_cast<size_t>(wk.worker)].push_back(c.game->p1.x);
            }
        }
    }

    // counts the ticks the child spends away from every object of the level, and kills it after too many
    auto SearchRun::checkPresence(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> void {
        c.cAway = pn.nAway;
        if (!c.game->died && cues.presence.ok() && !c.game->isPlatformer) {
            int const presence1 =
                    c.inEndZone ? 1 : cues.presence.any(*c.game, c.game->p1.x, c.game->p1.y, opt.awayRx, opt.awayRy);
            int const presence2 =
                    (c.inEndZone || !c.game->isDualMode || presence1 == 0)
                            ? 1
                            : cues.presence.any(*c.game, c.game->p2.x, c.game->p2.y, opt.awayRx, opt.awayRy);
            bool const present = presence1 != 0 && presence2 != 0;
            bool const real = presence1 == 1 && presence2 == 1;
            c.cAway = !present ? (pn.nAway < 0 ? -1 : pn.nAway + 1) : (real || pn.nAway >= 0) ? 0 : -1;
            bool inZone = false;
            for (auto const& z : awayZones)
                if (c.game->p1.x >= z.first && c.game->p1.x <= z.second) {
                    inZone = true;
                    break;
                }
            if (!present && pn.nAway >= 0 && inZone) {
                auto falling = [](Player const& p) {
                    if (p.isShip || p.isBird || p.isDart || p.isSwing || p.isSideways || p.isOnGround) return false;
                    return (p.isUpsideDown ? p.yVelocity : -p.yVelocity) >= 10.0;
                };
                bool const fallOk1 = presence1 != 0 || falling(c.game->p1);
                bool const fallOk2 = !c.game->isDualMode || presence1 == 0
                                    ? (!c.game->isDualMode || falling(c.game->p2) ||
                                    cues.presence.any(*c.game, c.game->p2.x, c.game->p2.y, opt.awayRx, opt.awayRy) != 0)
                                    : (presence2 != 0 || falling(c.game->p2));
                if (fallOk1 && fallOk2) c.cAway = pn.nAway;
            }
            if (!present &&
                (c.game->p1.tpCount != pn.prevTp1 || (c.game->isDualMode && c.game->p2.tpCount != pn.prevTp2)))
                c.cAway = -1;
            if (c.cAway > 0) {
                int limit = opt.awayKill;
                for (auto const& z : awayZones)
                    if (c.game->p1.x >= z.first && c.game->p1.x <= z.second) {
                        limit *= 4;
                        break;
                    }
                bool free = false;
                for (auto const& z : awayFreeZones)
                    if (c.game->p1.x >= z.first && c.game->p1.x <= z.second) {
                        free = true;
                        break;
                    }
                if (c.cAway > limit && !free) {
                    c.game->died = true;
                    c.game->killerUid = kAwayKiller;
                    awayKillsOf[static_cast<size_t>(wk.worker)].push_back(c.game->p1.x);
                }
            }
        }
    }

    // stuck and must-take rules, menus and the menu goal
    auto SearchRun::checkStuckAndMenus(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> void {
        c.cSpan = pn.nSpan;
        if (!c.game->isPlatformer && !c.inEndZone && c.game->trig.on)
            c.cSpan.touchCheck(trigTouchSig(*c.game), c.game->tick);
        c.escCostNow = 0;
        if (!c.game->died && opt.stuckKill > 0 && !c.game->isPlatformer && !c.inEndZone &&
            c.cSpan.step(c.game->p1.x, c.game->p1.y, c.game->tick, opt.stuckKill, opt.stuckMargin, c.game->p1.isGoingLeft)) {
            c.game->died = true;
            c.game->killerUid = kStuckKiller;
            stuckKillsOf[static_cast<size_t>(wk.worker)].push_back(c.game->p1.x);
        }
        if (!c.game->died && !mustTakes.empty() && !c.game->isPlatformer && c.game->p1.x > pn.prevX)
            for (auto const& mustTake : mustTakes) {
                if (!(pn.prevX < mustTake.passX && c.game->p1.x >= mustTake.passX)) continue;
                World::PortalCluster const& cluster = clusterOf(cues, mustTake.cluster);
                bool active = false;
                for (int member : cluster.members)
                    if (!c.game->obj(member).disabled()) {
                        active = true;
                        break;
                    }
                if (active && !clusterTaken(*c.game, cluster)) {
                    c.game->died = true;
                    c.game->killerUid = kMustKiller;
                    break;
                }
            }
        if (!c.game->died && (!c.game->world->menus.empty() || !c.game->world->ldmTouch.empty())) {
            if (menuViolation(*c.game)) {
                c.game->died = true;
                c.game->killerUid = kMenuKiller;
            } else if (opt.menuGoal && menuGoalReached(*c.game))
                c.game->completed = true;
        }
    }

    // escape rules: the child left the level's structure above or below (cost, then kill)
    auto SearchRun::checkEscape(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> void {
        bool const escOn =
                (opt.escapeKill > 0 || opt.escapeCost > 0) && !c.game->died && !c.game->isPlatformer && !c.inEndZone;
        bool const roofOn = opt.escapeRoofCost > 0 && !c.game->died && !c.game->isPlatformer && !c.inEndZone;
        if (!c.game->died && !c.game->isPlatformer && !c.inEndZone) c.cSpan.tpNote(*c.game);
        int const escSide = (escOn || roofOn) ? escapeSide(*c.game, c.cSpan) : 0;
        if (escOn) c.cSpan.escStep(escSide == 2 || (escSide == 1 && opt.escapeRoof), 0);
        if (escOn && opt.escapeCost > 0 && c.cSpan.esc > 2 * opt.escapeGrace) c.escCostNow = opt.escapeCost;
        if (roofOn) {
            c.cSpan.roofStep(escSide == 1);
            if (c.cSpan.escR > 2 * opt.escapeGrace) c.escCostNow += opt.escapeRoofCost;
        }
        bool escOk = false;
        if (escOn && opt.escapeKill > 0 && c.cSpan.esc > 2 * opt.escapeKill)
            for (auto const& z : escOkZones)
                if (c.game->p1.x >= z.first && c.game->p1.x <= z.second) {
                    escOk = true;
                    break;
                }
        if (escOn && opt.escapeKill > 0 && c.cSpan.esc > 2 * opt.escapeKill && !escOk) {
            c.game->died = true;
            c.game->killerUid = kEscapeKiller;
            escapeKillsOf[static_cast<size_t>(wk.worker)].push_back(c.game->p1.x);
        }
    }

    // flying modes: counts the portal exits the child flew past, cost then kill
    auto SearchRun::checkFlyMiss(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> void {
        c.cFlyMiss = pn.nFlyMiss;
        c.flyMissPenalty = 0;
        if (modeCharOf(c.game->p1) != pn.parentMode || c.game->isDualMode) c.cFlyMiss = 0;
        if (opt.flyMissKill > 0 && !c.game->died && !c.inEndZone) {
            int const flyMisses = flyExitMisses(
                    *c.game, pn.prevX, cues,
                    opt.outsideMiss && (c.cSpan.escR > 2 * opt.escapeGrace || c.cSpan.esc > 2 * opt.escapeGrace),
                    opt.flyMissWaveOnly, opt.flyMissWaveOnly ? opt.waveMissReach : 1e30f);
            if (flyMisses > 0) {
                c.cFlyMiss = static_cast<uint8_t>(std::min(250, c.cFlyMiss + flyMisses));
                if (c.cFlyMiss >= opt.flyMissKill) {
                    bool ok = false;
                    for (auto const& z : flyOkZones)
                        if (c.game->p1.x >= z.first && c.game->p1.x <= z.second) {
                            ok = true;
                            break;
                        }
                    if (!ok) {
                        c.game->died = true;
                        c.game->killerUid = kFlyKiller;
                    } else if (opt.flyMissWaveOnly) {
                        c.flyMissPenalty += 10 * opt.missedPortalCost * flyMisses;
                    }
                }
            }
        }
    }

    // kills the child when it passes an item target or a group goal that is not done
    auto SearchRun::checkGoals(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> void {
        if (!c.game->died)
            for (size_t q = 0; q < itemTargets.size() && q < 4; ++q) {
                ItemTarget const& target = itemTargets[q];
                bool const missed =(target.cmp >= 0 || target.avoid)
                                ? pn.prevX < target.x && c.game->p1.x >= target.x && itemTargetGap(*c.game, target) != 0
                                : pn.prevX < target.x + 60.f && c.game->p1.x >= target.x + 60.f && !trigGroupOn(*c.game, target.group);
                if (missed) {
                    c.game->died = true;
                    c.game->killerUid = pn.parentGame->world->objs[static_cast<size_t>(target.idx)].uid;
                    break;
                }
            }

        if (!c.game->died)
            for (auto const& goal : groupGoals) {
                if (!(pn.prevX < goal.checkX && c.game->p1.x >= goal.checkX)) continue;
                bool ok = trigGroupOn(*c.game, goal.group);
                for (size_t k = 0; k < goal.acts.size() && !ok; ++k) {
                    Obj const& counter = pn.parentGame->world->objs[static_cast<size_t>(goal.acts[k])];
                    if (trigGroupDisabled(*c.game, counter)) continue;
                    int const value = trigItemValue(*c.game, std::clamp(counter.itemID, 0, 9999));
                    bool satisfied;
                    if (counter.id == ObjId::CountTrigger)
                        satisfied = counter.count >= 0 ? value >= counter.count : value <= counter.count;
                    else
                        satisfied = counter.compare == 0   ? value == counter.count : counter.compare == 1 ? value > counter.count : value < counter.count;
                    if (satisfied) ok = true;
                }
                if (!ok) {
                    c.game->died = true;
                    c.game->killerUid = 0;
                    break;
                }
            }
    }

    // a child sent back more than 300 units: kills it when it loops on the same spot and trigger state
    auto SearchRun::checkLoopBack(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> void {
        c.cBackX = pn.nBackX;
        c.cBackSig = pn.nBackSig;
        c.cBackSigP = pn.nBackSigP;
        c.cBackN = pn.nBackN;
        c.cBackTick0 = pn.nBackTick0;
        if (!c.game->died && !c.game->isPlatformer && backPortalsOn && c.game->p1.x < pn.prevX - 300.f)
            for (auto const& bp : backPortals)
                if (std::fabs(pn.prevX - bp.sx) < 150.f && std::fabs(c.game->p1.x - bp.dx) < 150.f && std::fabs(c.game->p1.y - bp.dy) < 150.f) {
                    c.game->died = true;
                    c.game->killerUid = kSeedLoopKiller;
                    return;
                }
        if (!c.game->died && !c.game->isPlatformer && c.game->p1.x < pn.prevX - 300.f) {
            uint64_t const sigP = loopSig(*c.game, false);
            uint64_t const sig = loopSig(*c.game, true);
            bool const sameSpot = std::fabs(c.game->p1.x - pn.nBackX) < 2.f && sigP == pn.nBackSigP;
            c.cBackN = sameSpot ? static_cast<uint8_t>(std::min(pn.nBackN + 1, 250)) : uint8_t{1};
            if (!sameSpot) c.cBackTick0 = c.game->tick;
            if ((sameSpot && sig == pn.nBackSig) || c.cBackN >= 3) {
                c.game->died = true;
                c.game->killerUid = 0;
                loopStartsOf[static_cast<size_t>(wk.worker)].push_back(c.cBackTick0);
                loopZonesOf[static_cast<size_t>(wk.worker)].push_back(LoopZone{c.game->p1.x, pn.prevX, c.game->p1.y});
            }
            c.cBackX = c.game->p1.x;
            c.cBackSig = sig;
            c.cBackSigP = sigP;
        }
    }

    // free modes: kills a wave that stays outside the level's extent
    auto SearchRun::checkExtent(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> void {
        if (!c.game->died && c.game->isFreeMode && opt.extentCost > 0 && !c.game->isPlatformer && !c.inEndZone &&
            pn.nOut >= kFreeOutKill && c.game->p1.isDart && outsideExtent(*c.game, cues.extent, opt.extentMargin) &&
            !movedGameplayNear(*c.game, cues.extent, opt.extentMargin)) {
            c.game->died = true;
            c.game->killerUid = 0;
        }
    }

    // a dead child is only counted (its killer is kept for the back off)
    auto SearchRun::dropDead(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> bool {
        c.cOwner = c.game == pn.parentGame ? pn.parentOwner : static_cast<uint8_t>(wk.worker);
        if (c.game->died) {
            if (dbgHoldTrack && pn.parent.dbgHold && c.hold)
                std::fprintf(stderr, "[hold] tick %d : the held lineage dies (uid %d) x=%.1f y=%.1f\n", tick,
                            c.game->killerUid, c.game->p1.x, c.game->p1.y);
            killersOf[static_cast<size_t>(wk.worker)].push_back(c.game->killerUid);
            if (c.cOwner == wk.worker)
                wk.arena.release(c.game);
            else
                arenas[c.cOwner].releaseFrom(wk.worker, c.game);
            return false;
        }
        return true;
    }

    // creates the node of the child with the costs inherited from its parent
    auto SearchRun::makeChild(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> void {
        wk.out.emplace_back();
        c.child = &(wk.out.back());
        c.child->dbgHold = (pn.parent.dbgHold && c.hold) ? 1 : 0;
        c.child->g = c.game;
        c.child->owner = c.cOwner;
        c.child->parent = static_cast<int>(pn.i);
        c.child->action = c.act;
        c.child->cost = pn.nCost;
        c.child->frag = pn.nFrag;
        c.child->debt = pn.nDebt;
        c.child->debtX0 = pn.nDebtX0;
        c.child->debtX1 = pn.nDebtX1;
        c.child->pdebt = pn.nPdebt;
        c.child->away = c.cAway;
        if (c.cAway > 0 && opt.awayCost > 0) c.child->cost += opt.awayCost;
        c.child->backX = c.cBackX;
        c.child->backSig = c.cBackSig;
        c.child->backSigP = c.cBackSigP;
        c.child->backN = c.cBackN;
        c.child->backTick0 = c.cBackTick0;
        c.child->span = c.cSpan;
        c.child->flyMiss = c.cFlyMiss;
    }

    // costs of the inputs (preferFewInputs): presses, missed groups, orbs and pads
    auto SearchRun::addInputCosts(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> void {
        if (opt.preferFewInputs) {
            if (pn.needMask)
                for (size_t q = 0; q < needOff.size() && q < 32; ++q)
                    if ((pn.needMask >> q & 1u) && !trigGroupOn(*c.game, needOff[q]))
                        c.child->cost -= opt.missedSpeedCost;

            if (c.isPress) {
                bool rewarded =
                        (c.game->itemChanges - pn.prevItemChanges) != (pn.relItemDelta == ~0u ? 0u : pn.relItemDelta);
                if (rewarded && !trapWin.empty())
                    for (auto const& win : trapWin)
                        if (c.game->p1.x >= win.first && c.game->p1.x <= win.second) {
                            rewarded = false;
                            break;
                        }
                if (rewarded && c.game->p1.x > itemRewardUntilX && !c.game->isPlatformer) rewarded = false;
                if (rewarded && !countTargets.empty() && !c.game->isPlatformer) {
                    bool counted = false;
                    bool closer = false;
                    for (size_t q = 0; q < countTargets.size(); ++q) {
                        auto const& target = countTargets[q];
                        int const v1 = trigItemValue(*c.game, target.item);
                        int const v0 = pn.countV0[q];
                        if (v0 == v1) continue;
                        counted = true;
                        bool const live =
                                target.instant ? target.x >= c.game->p1.x - 60.f : target.x <= c.game->p1.x + 30.f;
                        if (live && std::abs(v1 - target.count) < std::abs(v0 - target.count)) {
                            closer = true;
                            break;
                        }
                    }
                    if (counted && !closer) rewarded = false;
                }

                if (rewarded && !paceItems.empty() && !c.game->isPlatformer) {
                    bool anyPace = false;
                    bool ahead = false;
                    for (size_t q = 0; q < paceItems.size() && q < 8; ++q) {
                        int const value = trigItemValue(*c.game, paceItems[q].item);
                        if (value == pn.paceV0[q]) continue;
                        anyPace = true;
                        auto const& thresholds = paceItems[q].th;
                        auto it =
                                std::lower_bound(thresholds.begin(), thresholds.end(),
                                                std::make_pair(c.game->p1.x - 60.f, std::numeric_limits<int>::min()));
                        if (it == thresholds.end()) {
                            ahead = true;
                            continue;
                        }
                        float const thresholdX = std::max(it->first, 1.f);
                        float const expected =
                                static_cast<float>(it->second) * std::clamp(c.game->p1.x / thresholdX, 0.f, 1.f);
                        if (static_cast<float>(value) > std::ceil(expected))
                            ahead = true;
                        else {
                            ahead = false;
                            break;
                        }
                    }
                    if (anyPace && ahead) rewarded = false;
                }

                int movedAway = 0;
                for (size_t q = 0; q < itemTargets.size() && q < 4; ++q) {
                    ItemTarget const& target = itemTargets[q];
                    if (c.game->p1.x >= target.x + 60.f) continue;
                    int const d0 = pn.itemV0[q];
                    int const d1 = itemTargetGap(*c.game, target);
                    if (d1 == d0) continue;
                    if (d1 < d0)
                        rewarded = true;
                    else if (d1 > d0) {
                        rewarded = false;
                        ++movedAway;
                    }
                }
                c.child->cost += rewarded ? -opt.pressCost : opt.pressCost;
                if (movedAway) c.child->cost += opt.missedSpeedCost * movedAway;
            }

            if (c.game->tickGap < opt.fragileGap) c.child->frag += opt.fragileCost;
            if (c.game->obbGap < opt.fragileObbGap && !pn.prevObbFragile && !fineNow)
                c.child->cost += opt.fragileObbCost;
            if (!fineNow && c.game->hazGap < opt.tightGap && !(pn.prevHaz < opt.tightGap))
                c.child->cost += opt.tightCost;
            if (!fineNow && c.game->hazGap < opt.tightGap2 && !(pn.prevHaz < opt.tightGap2))
                c.child->cost += opt.tightCost2;
            if (opt.lateSpeedCost > 0 && c.game->p1.playerSpeed != pn.prevSpeed)
                c.child->cost += lateSpeedCost(*c.game, pn.prevX, cues, opt);
            if (c.game->rotAmb && !pn.prevRotAmb) c.child->cost += opt.rotAmbCost;
            if (opt.missedOrbCost > 0 && (pn.prevRings1.n || pn.prevRings2.n) && !fineNow)
                c.child->cost += missedOrbsCost(*c.game, pn.prevRings1, pn.prevRings2, opt);
            if (pn.persistWatch && trigPersistChanged(*c.game) > pn.prevPersist) c.child->cost += opt.persistChangeCost;
            if (!cues.traps.empty() && trapsTouchedNear(*c.game, cues, pn.prevX) > pn.prevTraps)
                c.child->cost += opt.trapTouchCost;

            int speedMisses = 0;
            c.child->cost += missedPortalsCost(
                    *c.game, pn.prevX, cues, opt, speedMisses,
                    opt.outsideMiss && (c.cSpan.escR > 2 * opt.escapeGrace || c.cSpan.esc > 2 * opt.escapeGrace));
            c.child->cost += portalPrepay(*c.game, pn.prevX, cues, opt, pn.openPortals);
            if (c.child->cost < 0) c.child->cost = 0;
            c.child->debt += speedMisses;
            if (speedMisses > 0) {
                c.child->debtX1 = c.child->debtX0;
                c.child->debtX0 = g_lastMissX;
            }
            {}
            c.child->pdebt += g_portalMisses;
            if (!cues.attract.empty()) c.child->cost += attractCost(*c.game, cues);
            if (opt.flyExitCap > 0) c.child->cost += flyExitCost(*c.game, cues, opt);
            if (opt.bandCost > 0 && outOfBand(*c.game, cues)) c.child->cost += opt.bandCost;
            if (opt.emptyCost > 0 && farFromLevel(*c.game, cues)) c.child->cost += opt.emptyCost;

            if (opt.boundCost > 0) {
                Player const& player = c.game->p1;
                bool atBound = false;
                bool const flyMode = player.isShip || player.isBird || player.isDart || player.isSwing;
                if ((flyMode ? player.yVelocity == 0.0 : c.game->isDualMode) && !c.game->isPlatformer) {
                    float const half = playerRect(player).h * 0.5f;
                    float const lo = getMinPortalY(*c.game);
                    float const hi = getMaxPortalY(*c.game);
                    atBound = (player.y - half - lo < 4.f && lo > 130.f) || (hi - player.y - half < 4.f);
                }
                if (flyMode)
                    c.child->boundTicks = atBound ? pn.nBound + 1 : 0;
                else if (c.game->isDualMode) {
                    bool const grounded = player.isOnGround && player.yVelocity == 0.0;
                    c.child->boundTicks =
                            atBound ? pn.nBound + 1 : (grounded ? 0 : (pn.nBound > 0 ? pn.nBound + 1 : 0));
                } else
                    c.child->boundTicks = 0;
                if (c.child->boundTicks > (flyMode ? 30 : 60)) c.child->cost += opt.boundCost;
            }
            if (opt.slowMoCost > 0 && c.game->timeWarp < 0.5f) c.child->cost += opt.slowMoCost;
            for (auto const& z : clickZones)
                if (c.game->p1.x >= z.x0 && c.game->p1.x <= z.x1) {
                    int const owed = std::abs(z.count - trigItemValue(*c.game, z.item));
                    c.child->cost += 3 * std::min(owed, 50);
                }
            c.child->cost += c.escCostNow;
            c.child->cost += c.flyMissPenalty;

            if (opt.extentCost > 0) {
                if (outsideExtent(*c.game, cues.extent, opt.extentMargin) &&
                    !movedGameplayNear(*c.game, cues.extent, opt.extentMargin)) {
                    c.child->cost += opt.extentCost;
                    c.child->outTicks = pn.nOut + 1;
                    bool const flying =
                            c.game->p1.isShip || c.game->p1.isBird || c.game->p1.isDart || c.game->p1.isSwing;
                    if (c.child->outTicks == 90 && flying && opt.portalDebt) {
                        ++c.child->debt;
                        ++c.child->pdebt;
                        c.child->outJust = true;
                    }
                } else
                    c.child->outTicks = 0;
            }

            if (track.on && c.child->cost != pn.nCost)
                if (Game const* trackedGame = track.at(tick + 1))
                    if (makeKey(*trackedGame, qY, qV) == makeKey(*c.game, qY, qV))
                        std::fprintf(stderr,
                                     "[track-cost] tick %d x %.0f y %.1f : +%d (press %d portals %d orbs %d attract %d "
                                     "band %d empty %d obb %d)\n",
                                     tick + 1, c.game->p1.x, c.game->p1.y, c.child->cost - pn.nCost, c.isPress ? 1 : 0,
                                     missedPortalsCost(*c.game, pn.prevX, cues, opt, speedMisses),
                                     (opt.missedOrbCost > 0 && (pn.prevRings1.n || pn.prevRings2.n))
                                             ? missedOrbsCost(*c.game, pn.prevRings1, pn.prevRings2, opt)
                                             : 0,
                                     cues.attract.empty() ? 0 : attractCost(*c.game, cues),
                                     outOfBand(*c.game, cues) ? 1 : 0, farFromLevel(*c.game, cues) ? 1 : 0,
                                     (c.game->obbGap < opt.fragileObbGap && !pn.prevObbFragile) ? 1 : 0),
                                [&] {
                                    for (int u : pn.prevRings1) std::fprintf(stderr, "  orb uid %d\n", u);
                                }();
        }
    }

    // press times, key, class and lineage of the child, then merges it with an equal child of this worker
    auto SearchRun::finishChild(ExpandWorker& wk, ParentInfo& pn, int ai, ChildStep& c) -> void {
        c.child->lastPress = c.isPress ? tick : pn.nLastPress;
        c.child->lastRelease = (pn.holdingBefore && !c.hold) ? tick : pn.nLastRelease;

        int const segCost = std::max(1, opt.pressCost / 2);
        if (pn.segMode && opt.preferFewInputs) {
            if (c.isPress && tick - pn.nLastRelease < pn.minSeg)
                c.child->cost += segCost;
            else if (pn.holdingBefore && !c.hold && tick - pn.nLastPress < pn.minSeg)
                c.child->cost += segCost;
        }
        {
            bool const ringNow = c.game->p1.touchedRing || c.game->p1.touchedCustomRing ||
                                 (c.game->isDualMode && (c.game->p2.touchedRing || c.game->p2.touchedCustomRing));
            c.child->ringSincePress = c.isPress ? ringNow : (pn.nRing || ringNow);
        }

        c.child->key = makeKey(*c.game, layerQY, layerQV);
        c.child->cls = classOfState(c.child->key, *c.game);
        if (lineageBoundary) {
            c.child->lin1 = pn.parent.lin0;
            c.child->lin0 = static_cast<uint32_t>(pn.i);
        } else {
            c.child->lin0 = pn.parent.lin0;
            c.child->lin1 = pn.parent.lin1;
        }
        c.child->dead = false;
        c.child->kept = false;
        auto const found = wk.seenLocal.findOrInsert(c.child->key, static_cast<int>(wk.out.size()) - 1);
        if (found.second) {
            int last = -1;
            bool merged = false;
            for (int sameIdx = *found.first; sameIdx >= 0; sameIdx = wk.out[static_cast<size_t>(sameIdx)].sameNext) {
                last = sameIdx;
                Node& same = wk.out[static_cast<size_t>(sameIdx)];
                if (!sameTrig(same, (*c.child))) continue;
                if (nodeLess((*c.child), same)) {
                    int const link = same.sameNext;
                    std::swap(same, (*c.child));
                    same.sameNext = link;
                }
                if (c.child->owner == wk.worker)
                    wk.arena.release(c.child->g);
                else
                    arenas[c.child->owner].releaseFrom(wk.worker, c.child->g);
                wk.out.pop_back();
                merged = true;
                break;
            }
            if (!merged) wk.out[static_cast<size_t>(last)].sameNext = static_cast<int>(wk.out.size()) - 1;
        }
    }

    // expands every live state by its inputs on the workers
    auto SearchRun::expandLayer() -> void {
        nNodes = live.size();
        expandStart = std::chrono::steady_clock::now();
        perThread = 24;
        if (nodeCostUs > 0.0) perThread = std::clamp(static_cast<int>(250.0 / nodeCostUs), 6, 24);
        nWorkers = std::min(nThreads, std::max(1, static_cast<int>(nNodes / static_cast<size_t>(perThread))));
        if (nWorkers == 1) {
            for (int w = 1; w < nThreads; ++w) nextBuckets[static_cast<size_t>(w)].clear();
            expand(0);
        } else {
            for (int w = 1; w < nThreads; ++w) nextBuckets[static_cast<size_t>(w)].clear();
            pool->run([&](int w) { expand(w); }, nWorkers);
        }
        if (nThreads > 1)
            for (auto& ctx : localCtx) trigLocalFlush(ctx);
        if (nThreads > 1)
            for (auto& arena : arenas) arena.collect();
        {
            double busyNow = 0;
            long nodesNow = 0;
            for (int w = 0; w < nThreads; ++w) {
                busyNow += busy[static_cast<size_t>(w)];
                nodesNow += nodesDone[static_cast<size_t>(w)];
            }
            double const busyDelta = busyNow - busyPrev;
            long const nodesDelta = nodesNow - nodesPrev;
            busyPrev = busyNow;
            nodesPrev = nodesNow;
            if (nodesDelta > 0) {
                double const costUs = busyDelta * 1e6 / static_cast<double>(nodesDelta);
                nodeCostUs = nodeCostUs <= 0.0 ? costUs : 0.95 * nodeCostUs + 0.05 * costUs;
            }
        }
        expandEnd = std::chrono::steady_clock::now();
        res.stats.expandSeconds += std::chrono::duration<double>(expandEnd - expandStart).count();
    }

}
