// The stages of one tick around the expansion: stop checks, layer setup, merging and sorting the children, beam width and pruning, tracking and moving to the next tick.

#include "search_internal.hpp"

namespace ap::sim {
    // stops when the caller cancels; in live mode, also takes back the unplayed path when the player loses an attempt
    auto SearchRun::checkCancelled() -> Flow {
        if (cancel && cancel->load()) {
            finalResult = finish(bestOf(false), false, "cancelled");
            return Flow::Return;
        }
        if (opt.waitFor) {
            int const lostTick = opt.waitFor->load(std::memory_order_relaxed);
            if (lostTick >= 0 && lostTick != lastLostSeen) {
                lastLostSeen = lostTick;
                constexpr int kLostBack = 1200;
                int const keep = std::max(hard.front().tick + kFixedAhead, lostTick - kLostBack);
                int const popped = retractUnplayed(keep, hard.front().tick);
                for (auto& e : spotRungs)
                    if (e.first > hard.back().tick) e.second = 0;
                if (debugOptions().check)
                    std::fprintf(
                            stderr,
                            "[live] attempt lost at tick %d : %d hard points removed, restart possible from tick %d\n",
                            lostTick, popped, hard.back().tick);
            }
        }
        return Flow::Next;
    }

    // stops when no tick record was beaten for stallSeconds, or when the tick or time budget is used up
    auto SearchRun::checkBudget() -> Flow {
        if (opt.stallSeconds > 0 && (tick & 63) == 0) {
            auto const now = std::chrono::steady_clock::now();
            if (tick > recordTick) {
                recordTick = tick;
                recordTime = now;
            } else if (std::chrono::duration<double>(now - recordTime).count() > opt.stallSeconds) {
                if (static_cast<size_t>(tick - res.startTick) + 1 > furthest.size()) furthest = pathTo(bestOf(false));
                {
                    finalResult = finish(-1, false, "no progress for " + std::to_string(static_cast<int>(opt.stallSeconds)) + " s (record tick " + std::to_string(recordTick) + ")");
                    return Flow::Return;
                }
            }
        }
        if ((opt.maxTicks > 0 && simulated.load() > opt.maxTicks) ||
            (opt.timeLimit > 0 && (tick & 63) == 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count() > opt.timeLimit)) {
            if (static_cast<size_t>(tick - res.startTick) + 1 > furthest.size()) furthest = pathTo(bestOf(false));
            {
                finalResult = finish(-1, false, "time budget used up (tick " + std::to_string(tick) + ")");
                return Flow::Return;
            }
        }
        return Flow::Next;
    }

    // stops when a live state finished the level (the lowest portal debt first, when debts count) or at the horizon
    auto SearchRun::checkCompleted() -> Flow {
        anyCompleted = false;
        for (Node* node : live)
            if (node->g->completed) {
                anyCompleted = true;
                break;
            }
        if (anyCompleted && g_debtFirst) {
            int minDebtAll = 1 << 30;
            int minDebtDone = 1 << 30;
            for (Node* node : live) {
                minDebtAll = std::min(minDebtAll, node->debt);
                if (node->g->completed) minDebtDone = std::min(minDebtDone, node->debt);
            }
            if (minDebtAll < minDebtDone && tick < endTick) anyCompleted = false;
        }
        if (tick >= endTick || anyCompleted) {
            finalResult = finish(bestOf(anyCompleted), true, anyCompleted ? "level complete" : "horizon reached");
            return Flow::Return;
        }
        return Flow::Next;
    }

    // setup of the tick: front x, forced exits already passed, finer grid near moved speed portals and in the fine zones, and the split of the live states among the workers
    auto SearchRun::prepareLayer() -> void {
        nLive = live.size();
        lastLiveX = (nLive && live[0]->g) ? live[0]->g->p1.x : 0.f;
        if (!mustTakes.empty() && nLive) {
            float maxLiveX = -1e30f;
            for (size_t i = 0; i < nLive; ++i)
                if (live[i]->g) maxLiveX = std::max(maxLiveX, live[i]->g->p1.x);
            bool const minAll = [&] {
                for (size_t i = 0; i < nLive; ++i)
                    if (live[i]->g && live[i]->g->p1.x <= maxLiveX - 1.f) return false;
                return true;
            }();
            (void)minAll;
            float minLiveX = 1e30f;
            for (size_t i = 0; i < nLive; ++i)
                if (live[i]->g) minLiveX = std::min(minLiveX, live[i]->g->p1.x);
            mustTakes.erase(std::remove_if(mustTakes.begin(), mustTakes.end(), [&](MustTake const& mustTake) { return minLiveX > mustTake.x1; }),
                            mustTakes.end());
        }
        layerQY = qY;
        layerQV = qV;
        {
            if (opt.playedTicks && !dynSpeedX.empty() && nLive) {
                auto it = std::lower_bound(dynSpeedX.begin(), dynSpeedX.end(), lastLiveX - 60.f);
                if (it != dynSpeedX.end() && *it <= lastLiveX + 900.f && qY >= opt.quantY) {
                    layerQY = qY * 0.5f;
                    layerQV = qV * 0.5f;
                }
            }
        }
        fineNow = false;
        if (!fineZones.empty() && nLive)
            for (auto const& z : fineZones)
                if (lastLiveX >= z.first && lastLiveX <= z.second) {
                    layerQY = std::min(layerQY, opt.quantY * 0.25f);
                    layerQV = std::min(layerQV, opt.quantV * 0.25f);
                    fineNow = true;
                    break;
                }
        lineageBoundary = (tick % kLineagePeriod) == 0;
        for (auto& pt : part) pt.clear();
        for (auto& v : killersOf) v.clear();
        for (auto& v : awayKillsOf) v.clear();
        for (auto& v : stuckKillsOf) v.clear();
        for (auto& v : escapeKillsOf) v.clear();
        for (auto& v : revisitKillsOf) v.clear();
        for (auto& v : loopStartsOf) v.clear();
        for (auto& v : loopZonesOf) v.clear();
        if (nThreads > 1) {
            for (size_t i = 0; i < nLive; ++i) {
                Game const* game = live[i]->g;
                uint64_t const hash = (game && game->trig.on && game->trig.h) ? trigHeavyHash(*game->trig.h) : static_cast<uint64_t>(i);
                part[static_cast<size_t>((hash * 0x9e3779b97f4a7c15ull) >> 40) % static_cast<size_t>(nThreads)].push_back(i);
            }
        } else
            for (size_t i = 0; i < nLive; ++i)
                part[live[i]->owner < static_cast<uint8_t>(nThreads) ? live[i]->owner : 0].push_back(i);
        for (int w = 0; w < nThreads; ++w) partCursor[static_cast<size_t>(w)].store(0, std::memory_order_relaxed);
        chunk = std::max<size_t>(4, nLive / (static_cast<size_t>(nThreads) * 12));
    }

    // adds the kills the workers counted to the stats and remembers where the last kill of each kind happened
    auto SearchRun::countKills() -> void {
        {
            {
                size_t nStuckK = 0;
                for (auto const& v : stuckKillsOf) nStuckK += v.size();
                res.stats.stuckKills += static_cast<long>(nStuckK);
            }
            {
                size_t nEscK = 0;
                for (auto const& v : escapeKillsOf) {
                    nEscK += v.size();
                    for (float x : v) lastEscKillX = std::max(lastEscKillX, x);
                }
                if (nEscK) {
                    lastEscKillTick = tick;
                    res.stats.escapeKills += static_cast<long>(nEscK);
                }
            }
            size_t nAwayK = 0;
            float awayMaxX = -1e30f;
            for (auto const& v : awayKillsOf) {
                nAwayK += v.size();
                for (float x : v) awayMaxX = std::max(awayMaxX, x);
            }
            if (nAwayK) {
                lastAwayKillTick = tick;
                lastAwayKillX = awayMaxX;
                res.stats.awayKills += static_cast<long>(nAwayK);
            }
            if (opt.flyMissKill > 0) {
                bool flyKilled = false;
                for (auto const& v : killersOf) {
                    for (int u : v)
                        if (u == kFlyKiller) {
                            flyKilled = true;
                            break;
                        }
                    if (flyKilled) break;
                }
                if (flyKilled) {
                    lastFlyKillTick = tick;
                    lastFlyKillX = lastLiveX;
                }
            }
            size_t nRevK = 0;
            for (auto const& v : revisitKillsOf) {
                nRevK += v.size();
                if (!v.empty()) lastRevisitX = v.back();
            }
            if (nRevK) {
                if (debugOptions().check && tick - lastRevisitKillTick > 240)
                    std::fprintf(stderr, "[back] tick %d : %zu lineage(s) brought back to x %.0f in the part already played, dropped\n", tick, nRevK, lastRevisitX);
                lastRevisitKillTick = tick;
                res.stats.revisitKills += static_cast<long>(nRevK);
            }
        }
    }

    // puts the children of all workers in the next layer; children with the same key and trigger state are merged into the cheapest one
    auto SearchRun::mergeChildren() -> void {
        seen.clear();
        liveNext.clear();
        for (auto& bucket : nextBuckets) {
            for (auto& node : bucket) {
                auto const found = seen.findOrInsert(node.key, &node);
                if (!found.second) {
                    liveNext.push_back(&node);
                    continue;
                }
                Node** link = found.first;
                bool merged = false;
                for (Node* same = *link; same; link = &same->sameNextP, same = same->sameNextP) {
                    if (!sameTrig(*same, node)) continue;
                    if (nodeLess(node, *same)) {
                        same->dead = true;
                        node.sameNextP = same->sameNextP;
                        *link = &node;
                        liveNext.push_back(&node);
                    } else
                        node.dead = true;
                    merged = true;
                    break;
                }
                if (!merged) {
                    *link = &node;
                    liveNext.push_back(&node);
                }
            }
        }
    }

    // sorts the next layer by portal debt, cost, fragility, last press and a salted hash
    auto SearchRun::sortChildren() -> void {
        if (!liveNext.empty()) {
            size_t writePos = 0;

            for (Node* p : liveNext)
                if (!p->dead) liveNext[writePos++] = p;
            liveNext.resize(writePos);
            sortBuf.clear();
            for (Node* p : liveNext) {
                uint64_t const costBits = static_cast<uint64_t>(std::min(p->cost, 0xffff)) & 0xffffu;
                uint64_t const fragBits = static_cast<uint64_t>(std::min(p->frag, 0xff)) & 0xffu;
                uint64_t const pressBits = static_cast<uint64_t>(std::clamp(p->lastPress + 0xffff - tick, 0, 0xffff));
                uint64_t const hashBits =
                        (tieSalt ? KeyHash::splitmix(KeyHash{}(p->key) ^ tieSalt) : KeyHash{}(p->key)) & 0xfffffu;
                uint64_t const debtBits = g_debtFirst ? static_cast<uint64_t>(std::clamp(p->debt, 0, 15)) : 0u;
                sortBuf.emplace_back(
                        (debtBits << 60) | (costBits << 44) | (fragBits << 36) | (pressBits << 20) | hashBits, p);
            }
            std::sort(sortBuf.begin(), sortBuf.end(), [](auto const& a, auto const& b) {
                return a.first != b.first ? a.first < b.first : nodeLess(*a.second, *b.second);
            });
            for (size_t i = 0; i < sortBuf.size(); ++i) liveNext[i] = sortBuf[i].second;
            if (track.on) {
                track.rankPre = -1;
                track.nPre = static_cast<int>(liveNext.size());
                if (Game const* trackedGame = track.at(tick + 1)) {
                    Key const trackKey = makeKey(*trackedGame, qY, qV);
                    for (size_t i = 0; i < liveNext.size(); ++i)
                        if (liveNext[i]->key == trackKey) {
                            track.rankPre = static_cast<int>(i);
                            track.costPre = liveNext[i]->cost;
                            break;
                        }
                }
            }
        }
    }

    // back to the normal beam, grid and press gap once the front passed the spot that needed them
    auto SearchRun::narrowBack() -> void {
        if (spotTick >= 0 && !liveNext.empty() && liveNext[0]->g && liveNext[0]->g->p1.x > spotX + 60.f) {
            if (beam > opt.beamWidth || qY < opt.quantY || gapNow < opt.minPressGap) {
                bool merged = false;
                for (auto& pass : passed)
                    if (std::fabs(pass.x - spotX) <= 90.f) {
                        pass.beam = std::max(pass.beam, beam);
                        pass.qY = std::min(pass.qY, qY);
                        pass.qV = std::min(pass.qV, qV);
                        pass.gap = std::min(pass.gap, gapNow);
                        merged = true;
                        break;
                    }
                if (!merged) passed.push_back(Passed{spotX, beam, qY, qV, gapNow});
                if (debugOptions().check)
                    std::fprintf(stderr, "[passed] tick %d : dead end x %.0f passed (beam %d, q %.2f, gap %d)\n", tick, spotX, beam, qY, gapNow);
                if (!opt.playedTicks) {
                    bool nearPrev = false;
                    for (auto const& pass : passed)
                        if (pass.x < spotX - 90.f && spotX - pass.x <= kZoneGap) {
                            nearPrev = true;
                            break;
                        }
                    if (nearPrev || spotX <= zoneX1) {
                        if (spotX > zoneX1) {
                            zoneX0 = spotX - kZoneGap;
                            zoneSet = Passed{spotX, beam, qY, qV, gapNow};
                        } else {
                            zoneSet.beam = std::max(zoneSet.beam, beam);
                            zoneSet.qY = std::min(zoneSet.qY, qY);
                            zoneSet.qV = std::min(zoneSet.qV, qV);
                            zoneSet.gap = std::min(zoneSet.gap, gapNow);
                        }
                        zoneX1 = std::max(zoneX1, spotX + kZoneLen);
                        holdX = std::max(holdX, zoneX1);
                        if (debugOptions().check)
                            std::fprintf(stderr, "[zone] x %.0f .. %.0f : beam %d, q %.2f, gap %d kept\n", zoneX0, zoneX1, zoneSet.beam, zoneSet.qY, zoneSet.gap);
                    }
                }
            }
            spotTick = -1;
            spotX = -1e30f;
        }
        frontX = (!liveNext.empty() && liveNext[0]->g) ? liveNext[0]->g->p1.x : 0.f;
        narrowAfter = opt.playedTicks ? 120 : 480;
        if ((beam > opt.beamWidth || qY != opt.quantY || gapNow != opt.minPressGap) && widenedAtTick >= 0 &&
            tick - widenedAtTick > narrowAfter && frontX > holdX + 60.f && spotTick < 0) {
            beam = opt.beamWidth;
            qY = opt.quantY;
            qV = opt.quantV;
            gapNow = opt.minPressGap;
        }
    }

    // beam width of this tick: smaller when far ahead of the player in live mode, adapted to recent widenings
    auto SearchRun::chooseWidth() -> void {
        beamEff = beam;
        if (opt.playedTicks && beam == opt.beamWidth && tick >= noReduceUntil) {
            int const played = opt.playedTicks->load();
            int const lead = tick - played;
            if (played > res.startTick) {
                if (lead < 240)
                    beamEff = std::max(40, beam / 3);
                else if (lead < 480)
                    beamEff = std::max(50, beam / 2);
                else if (lead < 1200)
                    beamEff = std::max(60, beam * 2 / 3);
                else if (lead < 2400)
                    beamEff = std::max(75, beam * 5 / 6);
            }
            if (beamEff < beam) lastReducedTick = tick;
        }
        {
            int const adaptK = opt.adaptBeamK;
            int const adaptMin = opt.adaptBeamMin;
            constexpr float adaptBack = 3000.f;
            if (res.stats.widenings != adaptWidenSeen) {
                adaptWidenSeen = res.stats.widenings;
                noAdaptZones.emplace_back(lastLiveX - adaptBack, lastLiveX + 1500.f);
            }
            bool inNoAdapt = false;
            if (adaptK > 0) {
                float const layerX = (!liveNext.empty() && liveNext[0]->g) ? liveNext[0]->g->p1.x : lastLiveX;
                for (auto const& z : noAdaptZones)
                    if (layerX >= z.first && layerX <= z.second) {
                        inNoAdapt = true;
                        break;
                    }
            }
            if (adaptK > 0 && !liveNext.empty() && liveNext[0]->g) {
                Player const& front = liveNext[0]->g->p1;
                if (front.isShip || front.isBird || front.isDart || front.isSwing) inNoAdapt = true;
            }
            if (adaptK > 0 && !inNoAdapt && beam == opt.beamWidth && tick >= noReduceUntil &&
                static_cast<int>(liveNext.size()) > adaptMin) {
                cellsBuf.clear();
                for (Node* node : liveNext) {
                    Player const& p = node->g->p1;
                    float const pos = p.isSideways ? p.x : p.y;
                    uint64_t cell = static_cast<uint64_t>(node->cls & 0x3ffu);
                    cell |= (static_cast<uint64_t>(
                                    static_cast<uint32_t>(static_cast<int>(std::floor(pos / 12.f)) + 0x80000) &
                                    0xfffffu))
                            << 10;
                    cell |= (static_cast<uint64_t>(
                                    static_cast<uint32_t>(
                                            static_cast<int>(std::floor(static_cast<float>(p.yVelocity) / 1.5f)) +
                                            0x800) &
                                    0xfffu))
                            << 30;
                    if (node->g->isDualMode)
                        cell ^= KeyHash::splitmix(static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int>(std::floor(node->g->p2.y / 12.f)))) + 0x1234567ull);
                    cellsBuf.push_back(cell);
                }
                std::sort(cellsBuf.begin(), cellsBuf.end());
                int const nCells = static_cast<int>(std::unique(cellsBuf.begin(), cellsBuf.end()) - cellsBuf.begin());
                int const width = std::max(adaptMin, adaptK * nCells);
                if (width < beamEff) {
                    beamEff = width;
                    ++res.stats.adaptNarrow;
                    lastReducedTick = tick;
                }
                ++res.stats.adaptTicks;
            }
        }
    }

    // keeps the best beam-width states, with quotas per state class and lineage so the layer stays varied
    auto SearchRun::pruneLayer() -> void {
        if (static_cast<int>(liveNext.size()) > beamEff) {
            int const maxKeep = beamEff;

            keyCount.clear();
            variantsBuf.clear();
            {
                size_t writePos = 0;
                for (size_t i = 0; i < liveNext.size(); ++i) {
                    int& keyUses = keyCount[static_cast<uint32_t>(KeyHash{}(liveNext[i]->key))];
                    if (keyUses++ < kVariants)
                        liveNext[writePos++] = liveNext[i];
                    else
                        variantsBuf.push_back(liveNext[i]);
                }
                liveNext.resize(writePos);
            }

            int reserve = 0;
            {
                constexpr int divDen = 4;
                if (divDen > 0 && static_cast<int>(liveNext.size()) > maxKeep && maxKeep >= 8) {
                    reserve = std::min(96, maxKeep / divDen);
                    size_t const nNodes = liveNext.size();
                    divDist.assign(nNodes, std::numeric_limits<float>::max());
                    divF.resize(nNodes);
                    for (size_t i = 0; i < nNodes; ++i) {
                        Player const& p = liveNext[i]->g->p1;
                        divF[i] = DivF{(p.isSideways ? p.x : p.y) * 0.125f, static_cast<float>(p.yVelocity) * 0.5f, liveNext[i]->cls & 0x3ffu};
                    }
                    for (int slot = 0; slot < reserve; ++slot) {
                        size_t pick = static_cast<size_t>(slot);
                        if (slot > 0) {
                            float bestDist = -1.f;
                            for (size_t i = static_cast<size_t>(slot); i < nNodes; ++i)
                                if (divDist[i] > bestDist) {
                                    bestDist = divDist[i];
                                    pick = i;
                                }
                            if (bestDist <= 0.5f) {
                                reserve = slot;
                                break;
                            }
                        }
                        size_t const slotIdx = static_cast<size_t>(slot);
                        std::swap(liveNext[slotIdx], liveNext[pick]);
                        std::swap(divDist[slotIdx], divDist[pick]);
                        std::swap(divF[slotIdx], divF[pick]);
                        DivF const picked = divF[slotIdx];
                        for (size_t i = slotIdx + 1; i < nNodes; ++i) {
                            DivF const& other = divF[i];
                            float const dist = other.mode != picked.mode ? 1e6f : std::fabs(other.v - picked.v) + std::fabs(other.vy - picked.vy);
                            if (dist < divDist[i]) divDist[i] = dist;
                        }
                    }
                    if (reserve > 0)
                        std::sort(liveNext.begin() + reserve, liveNext.end(),
                                  [](Node* a, Node* b) { return nodeLess(*a, *b); });
                }
            }

            if (opt.floorReserve && static_cast<int>(liveNext.size()) > maxKeep && maxKeep >= 32) {
                auto coarseOf = [](Node const* node) {
                    Player const& p = node->g->p1;
                    float const pos = p.isSideways ? p.x : p.y;
                    uint32_t const yBand = static_cast<uint32_t>(
                            std::clamp(static_cast<int>(std::floor(pos / 300.f)) + 1024, 0, 0x3fffff));
                    return (node->cls & 0x3ffu) | (yBand << 10);
                };
                coarseCount.clear();
                for (Node* node : liveNext) coarseCount[coarseOf(node)] = 0;
                if (coarseCount.size() >= 2) {
                    int const perBand = std::max(2, maxKeep / 16);
                    size_t reserveEnd = static_cast<size_t>(reserve);
                    for (size_t i = 0; i < reserveEnd; ++i) ++coarseCount[coarseOf(liveNext[i])];
                    size_t const reserveStart = reserveEnd;
                    size_t const reserveMax =
                            std::min(static_cast<size_t>(maxKeep), reserveStart + static_cast<size_t>(maxKeep / 4));
                    for (size_t i = reserveEnd; i < liveNext.size() && reserveEnd < reserveMax; ++i) {
                        int& bandUses = coarseCount[coarseOf(liveNext[i])];
                        if (bandUses >= perBand) continue;
                        ++bandUses;
                        std::swap(liveNext[reserveEnd], liveNext[i]);
                        ++reserveEnd;
                    }
                    if (reserveEnd > reserveStart) {
                        std::sort(liveNext.begin() + static_cast<long>(reserveEnd), liveNext.end(),
                                  [](Node* a, Node* b) { return nodeLess(*a, *b); });
                        reserve = static_cast<int>(reserveEnd);
                    }
                }
            }

            classCount.clear();
            for (Node* node : liveNext) classCount[node->cls] = 0;
            int const quota = std::max(2, maxKeep / (2 * std::max<int>(1, static_cast<int>(classCount.size()))));
            lineageCount.clear();
            for (Node* node : liveNext) lineageCount[node->lin1] = 0;
            int const nLin = static_cast<int>(lineageCount.size());
            int const lineageQuota = (nLin > 1 && nLin * 4 <= maxKeep) ? std::max(1, maxKeep / (4 * nLin)) : 0;
            size_t keepEnd = static_cast<size_t>(reserve);
            for (size_t i = 0; i < keepEnd; ++i) {
                ++classCount[liveNext[i]->cls];
                ++lineageCount[liveNext[i]->lin1];
            }
            for (size_t i = keepEnd; i < liveNext.size(); ++i) {
                int& classUses = classCount[liveNext[i]->cls];
                int& lineageUses = lineageCount[liveNext[i]->lin1];
                bool const keep = (classUses < quota) || (lineageQuota > 0 && lineageUses < lineageQuota);
                if (keep && static_cast<int>(keepEnd) < maxKeep) {
                    ++classUses;
                    ++lineageUses;
                    std::swap(liveNext[keepEnd], liveNext[i]);
                    ++keepEnd;
                }
            }
            if (static_cast<int>(keepEnd) < maxKeep) {
                std::sort(liveNext.begin() + static_cast<long>(keepEnd), liveNext.end(),
                          [](Node* a, Node* b) { return nodeLess(*a, *b); });
            }
            if (static_cast<int>(liveNext.size()) < maxKeep)
                liveNext.insert(liveNext.end(), variantsBuf.begin(), variantsBuf.end());
            liveNext.resize(std::min(liveNext.size(), static_cast<size_t>(maxKeep)));
        }
    }

    // debug (APSIM_TRACK): reports where the tracked macro stands in the layer and why it was dropped
    auto SearchRun::traceTracked() -> void {
        if (track.on)
            if (Game const* trackedGame = track.at(tick + 1)) {
                Key const trackKey = makeKey(*trackedGame, qY, qV);
                int rank = -1;
                for (size_t i = 0; i < liveNext.size(); ++i)
                    if (liveNext[i]->key == trackKey) {
                        rank = static_cast<int>(i);
                        break;
                    }
                int const maxCost = liveNext.empty() ? 0 : liveNext.back()->cost;
                int const minCost = liveNext.empty() ? 0 : liveNext.front()->cost;
                if (rank < 0 && track.alive) {
                    if (track.rankPre >= 0)
                        std::fprintf(stderr, "[track] tick %d (x %.0f y %.1f) : pruned (rank %d / %d before pruning, cost %d ; kept : %zu, costs %d..%d, beam %d)\n", tick + 1, trackedGame->p1.x, trackedGame->p1.y, track.rankPre, track.nPre, track.costPre, liveNext.size(), minCost, maxCost, beam);
                    else
                        std::fprintf(stderr, "[track] tick %d (x %.0f y %.1f) : missing before pruning (merged, or parent lost/press not allowed) ; layer %d\n", tick + 1, trackedGame->p1.x, trackedGame->p1.y, track.nPre);
                    for (Node* node : liveNext) {
                        Game const& twin = *node->g;
                        if (std::fabs(twin.p1.x - trackedGame->p1.x) > 0.01f ||
                            std::fabs(twin.p1.y - trackedGame->p1.y) > 0.05f ||
                            std::fabs(twin.p1.yVelocity - trackedGame->p1.yVelocity) > 0.05f ||
                            twin.p1.isUpsideDown != trackedGame->p1.isUpsideDown)
                            continue;
                        {
                            Key const twinKey = makeKey(twin, qY, qV);
                            Key const trackedKey = makeKey(*trackedGame, qY, qV);
                            std::fprintf(stderr, "[twin] keys x %d/%d y %d/%d v %d/%d flags %08x/%08x extra %08x/%08x\n", twinKey.x, trackedKey.x, twinKey.y, trackedKey.y, twinKey.v, trackedKey.v, twinKey.flags, trackedKey.flags, twinKey.extra, trackedKey.extra);
                        }
                        std::fprintf(stderr, "    [twin] cost %d held %d/%d objects %zu/%zu touched %zu/%zu orbs %zu/%zu :", node->cost, twin.p1.holdingJump, trackedGame->p1.holdingJump,
                                    static_cast<size_t>(twin.objState.size()),
                                    static_cast<size_t>(trackedGame->objState.size()),
                                    static_cast<size_t>(twin.triggeredIDs.size()),
                                    static_cast<size_t>(trackedGame->triggeredIDs.size()),
                                    static_cast<size_t>(twin.p1.touchingRings.size()),
                                    static_cast<size_t>(trackedGame->p1.touchingRings.size()));
                        int shown = 0;
                        for (auto const& e : trackedGame->objState.items)
                            if (!twin.objState.find(e.first) && shown < 8) {
                                std::fprintf(stderr, " +%d(id %d x %.0f)",
                                            twin.world->objs[static_cast<size_t>(e.first)].uid,
                                            twin.world->objs[static_cast<size_t>(e.first)].id,
                                            twin.world->objs[static_cast<size_t>(e.first)].x);
                                ++shown;
                            }
                        for (auto const& e : twin.objState.items)
                            if (!trackedGame->objState.find(e.first) && shown < 12) {
                                std::fprintf(stderr, " -%d(id %d x %.0f)",
                                            twin.world->objs[static_cast<size_t>(e.first)].uid,
                                            twin.world->objs[static_cast<size_t>(e.first)].id,
                                            twin.world->objs[static_cast<size_t>(e.first)].x);
                                ++shown;
                            }
                        for (auto const& e : trackedGame->triggeredIDs.items)
                            if (!twin.triggeredIDs.count(e) && shown < 16) {
                                std::fprintf(stderr, " +T%d", e.first);
                                ++shown;
                            }
                        for (auto const& e : twin.triggeredIDs.items)
                            if (!trackedGame->triggeredIDs.count(e) && shown < 20) {
                                std::fprintf(stderr, " -T%d", e.first);
                                ++shown;
                            }
                        std::fprintf(stderr, "\n");
                    }
                } else if (rank >= 0 && !track.alive)
                    std::fprintf(stderr, "[track] tick %d : found again (rank %d, cost %d)\n", tick + 1, rank, liveNext[static_cast<size_t>(rank)]->cost);
                else if (rank >= 0 && (tick + 1) % 100 == 0)
                    std::fprintf(stderr, "[track] tick %d : rank %d / %zu, cost %d (min %d)\n", tick + 1, rank, liveNext.size(), liveNext[static_cast<size_t>(rank)]->cost, minCost);
                if (rank < 0 && !track.alive) {
                    int nTwins = 0;
                    for (Node* node : liveNext) {
                        Game const& twin = *node->g;
                        if (std::fabs(twin.p1.x - trackedGame->p1.x) > 0.01f ||
                            std::fabs(twin.p1.y - trackedGame->p1.y) > 0.5f ||
                            std::fabs(twin.p1.yVelocity - trackedGame->p1.yVelocity) > 0.5f)
                            continue;
                        Key const twinKey = makeKey(twin, qY, qV);
                        Key const trackedKey = makeKey(*trackedGame, qY, qV);
                        std::fprintf(stderr, "[twin t%d] y %.3f/%.3f yv %.3f/%.3f held %d/%d flags %08x/%08x extra %08x/%08x cost %d last press %d\n",
                                    tick + 1, twin.p1.y, trackedGame->p1.y, twin.p1.yVelocity,
                                    trackedGame->p1.yVelocity, twin.p1.holdingJump, trackedGame->p1.holdingJump,
                                    twinKey.flags, trackedKey.flags, twinKey.extra, trackedKey.extra, node->cost,
                                    node->lastPress);
                        for (uint8_t button = 0; button < 2; ++button) {
                            Game twinNext = twin;
                            Game trackedNext = *trackedGame;
                            stepTick(twinNext, button);
                            stepTick(trackedNext, button);
                            std::fprintf(stderr, "button %d : twin y %.3f yv %.3f dash %d dead %d orbs %d | tracked y %.3f yv %.3f dash %d dead %d orbs %d\n",
                                        button, twinNext.p1.y, twinNext.p1.yVelocity, twinNext.p1.isDashing,
                                        twinNext.died, twinNext.p1.touchingRings.size(), trackedNext.p1.y,
                                        trackedNext.p1.yVelocity, trackedNext.p1.isDashing, trackedNext.died,
                                        trackedNext.p1.touchingRings.size());
                        }
                        ++nTwins;
                    }
                    if (!nTwins)
                        std::fprintf(stderr, "    [twin t%d] none (y %.3f yv %.3f held %d)\n", tick + 1, trackedGame->p1.y, trackedGame->p1.yVelocity, trackedGame->p1.holdingJump);
                }
                track.alive = rank >= 0;
            }
        if (dbgHoldTrack) {
            bool anyKept = false;
            for (Node* p : liveNext)
                if (p->dbgHold) {
                    anyKept = true;
                    break;
                }
            if (!anyKept)
                for (auto& b : nextBuckets)
                    for (auto& node : b)
                        if (node.dbgHold) {
                            bool inNext = false;
                            for (Node* p : liveNext)
                                if (p == &node) {
                                    inNext = true;
                                    break;
                                }
                            if (!inNext)
                                std::fprintf(
                                        stderr, "[hold] tick %d : held lineage %s (cost %d, x=%.1f y=%.1f yv=%.3f)\n",
                                        tick,
                                        node.dead ? "merged (same key, another cheaper node)" : "pruned (full beam)",
                                        node.cost, node.g ? node.g->p1.x : 0.f, node.g ? node.g->p1.y : 0.f,
                                        node.g ? node.g->p1.yVelocity : 0.0);
                        }
        }
    }

    // frees the dropped states, records the layer in the history and moves to the next tick
    auto SearchRun::advanceTick() -> void {
        res.stats.maxLayer = std::max(res.stats.maxLayer, static_cast<int>(liveNext.size()));
        res.stats.mergeSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - expandEnd).count();
        for (Node* p : liveNext) p->kept = true;
        releaseUnkept(nextBuckets);
        for (Node* p : liveNext) p->kept = false;
        {
            std::vector<std::pair<int32_t, uint8_t>> layer;
            layer.reserve(liveNext.size());
            for (Node* p : liveNext) layer.emplace_back(p->parent, p->action);
            history.push_back(std::move(layer));
        }
        curBuckets.swap(nextBuckets);
        live.swap(liveNext);
        ++tick;
        if (progress && (tick & 255) == 0) progress(tick, static_cast<int>(live.size()));
    }

    // debug (APSIM_CHECK >= 2): replays the best path from the start and compares it with its state
    auto SearchRun::checkReplay() -> void {
        {
            static int const checkLevel = debugOptions().check;
            if (checkLevel >= 2 && (tick % 60) == 0 && !live.empty()) {
                int const best = bestOf(false);
                std::vector<uint8_t> path = pathTo(best);
                Game replay = start;
                for (uint8_t act : path) stepTick(replay, act);
                Game const& ref = *live[static_cast<size_t>(best)]->g;
                if (replay.p1.x != ref.p1.x || replay.p1.y != ref.p1.y || replay.died) {
                    std::fprintf(stderr, "[check2] tick %d : path replay (%zu) -> (%.3f,%.3f dead=%d tick %d) != node %d (%.3f,%.3f) ; inputs %zu rootLen %zu hist %zu roots %zu\n",
                                tick, path.size(), replay.p1.x, replay.p1.y, replay.died, replay.tick, best, ref.p1.x,
                                ref.p1.y, inputs.size(), rootTails.empty() ? size_t{0} : rootTails[0].size(),
                                history.size(), rootTails.size());
                    int const root = history.empty() ? best : ancestorAt(best, 0);
                    if (root >= 0 && static_cast<size_t>(root) < rootGamesDbg.size()) {
                        Game rootReplay = start;
                        for (uint8_t act : inputs) stepTick(rootReplay, act);
                        for (uint8_t act : rootTails[static_cast<size_t>(root)]) stepTick(rootReplay, act);
                        Game const& rootGame = rootGamesDbg[static_cast<size_t>(root)];
                        std::fprintf(stderr, "root %d : replay inputs+tail -> (%.3f,%.3f tick %d) ; root state (%.3f,%.3f tick %d)\n",
                                    root, rootReplay.p1.x, rootReplay.p1.y, rootReplay.tick, rootGame.p1.x, rootGame.p1.y, rootGame.tick);
                        Game fromRoot = rootGame;
                        std::vector<uint8_t> tail = tailTo(best);
                        for (size_t q = rootTails[static_cast<size_t>(root)].size(); q < tail.size(); ++q)
                            stepTick(fromRoot, tail[q]);
                        std::fprintf(stderr, "from the root + history (%zu) -> (%.3f,%.3f tick %d)\n",
                                    tail.size() - rootTails[static_cast<size_t>(root)].size(), fromRoot.p1.x, fromRoot.p1.y, fromRoot.tick);
                    }
                }
            }
        }
    }

    // saves a soft restart point when the layer narrowed to one state
    auto SearchRun::keepSoftPoint() -> void {
        if (live.size() == 1 && static_cast<int>(history.size()) >= 64 && tick - lastSoft >= 64 &&
            tick >= noSoftBefore) {
            makeSoft();
            beam = opt.beamWidth;
            qY = opt.quantY;
            qV = opt.quantV;
        } else if (opt.softInterval > 0 && tick - lastSoft >= opt.softInterval && live.size() > 1 && tick >= noSoftBefore) {
            makeSoft();
        }
    }

    // publishes the part of the path all states agree on, and in live mode the path to play
    auto SearchRun::publishPrefix() -> void {
        if (!decisionZones.empty()) updateDecisionZones();
        if (opt.commitInterval > 0 && tick - lastPublishCheck >= opt.commitInterval) {
            lastPublishCheck = tick;
            int const depthAvail = tick - hard.back().tick;
            int cut = depthAvail - opt.commitLag;
            if (cut > 0) cut = std::min(cut, convergenceDepth());
            cut = std::min(cut, depthAvail - 32);
            if (cut > 0) publish(static_cast<size_t>(cut));
        }
        if (opt.playedTicks && (tick & 7) == 0) livePublish();
    }

    // live mode: pauses while the search is too far ahead of the player
    auto SearchRun::waitForPlayer() -> void {
        if (opt.playedTicks && opt.maxLead > 0 && (tick & 15) == 0 && tick - leadBase() > opt.maxLead) {
            auto const pauseStart = std::chrono::steady_clock::now();
            while (!(cancel && cancel->load()) && tick - leadBase() > opt.maxLead - opt.leadResume && !(opt.starving && opt.starving->load(std::memory_order_relaxed))) {
                livePublish();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            res.stats.pausedSeconds +=
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - pauseStart).count();
            recordTime = std::chrono::steady_clock::now();
        }
    }

}
