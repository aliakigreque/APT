// Restarts, results and publishing of the search: roots, restart points, path reconstruction, the final
// result and the commits of the converged prefix.

#include "search_internal.hpp"

namespace ap::sim {
    auto SearchRun::escapeSide(Game const& g, Span const& span) -> int {
        if (!g.isFreeMode || cues.structure.empty()) return 0;
        auto fly = [](Player const& p) { return (p.isShip || p.isBird || p.isDart || p.isSwing) && !p.isSideways; };
        int side = fly(g.p1) && !span.tpRecent(false, g.tick) ? cues.structure.escaped(g.p1.x, g.p1.y, 105.f) : 0;
        if (side == 0 && g.isDualMode && fly(g.p2) && !span.tpRecent(true, g.tick))
            side = cues.structure.escaped(g.p2.x, g.p2.y, 105.f);
        return side;
    }

    auto SearchRun::escapedNow(Game const& g, Span const& span) -> bool {
        int const side = escapeSide(g, span);
        return side == 2 || (side == 1 && opt.escapeRoof);
    }

    auto SearchRun::spanReplay(Span& span, Game const& g) -> void {
        if (g.died || g.isPlatformer || !(g.p1.x < cues.endZoneX)) return;
        if (g.trig.on) span.touchCheck(trigTouchSig(g), g.tick);
        if (opt.stuckKill > 0) span.step(g.p1.x, g.p1.y, g.tick, opt.stuckKill, opt.stuckMargin);
        span.tpNote(g);
        if (opt.escapeKill > 0 || opt.escapeCost > 0) span.escStep(escapedNow(g, span), 0);
        if (opt.escapeRoofCost > 0) span.roofStep(escapeSide(g, span) == 1);
    }

    auto SearchRun::commonPrefixLen(std::vector<uint8_t> const& a, std::vector<uint8_t> const& b) -> size_t {
        size_t n = 0;
        while (n < a.size() && n < b.size() && a[n] == b[n]) ++n;
        return n;
    }

    auto SearchRun::gapForRung(int rung) -> int {
        int const gapDiv = ladder[std::min(rung, kNLadder - 1)].gapDiv;
        return std::max(std::min(opt.minPressGap, 4), opt.minPressGap / gapDiv);
    }

    auto SearchRun::flyTriesExhausted(float x) -> bool {
        int fails = 0;
        for (float failX : flyTryFails)
            if (failX >= x - 3000.f && failX <= x + 300.f) ++fails;
        return fails >= 2;
    }

    auto SearchRun::visitBinOf(float x) -> int { return static_cast<int>(std::floor(x / kRevisitBin)); }

    auto SearchRun::visitPurge(int fromTick) -> void {
        while (!visitOrder.empty()) {
            auto& bin = visitBins[static_cast<size_t>(visitOrder.back())];
            if (bin.empty() || bin.back().tick < fromTick) break;
            bin.pop_back();
            visitOrder.pop_back();
        }
    }

    auto SearchRun::visitAddXY(float x, float y, int atTick) -> void {
        int const binIdx = visitBinOf(x);
        if (binIdx < 0 || binIdx > (1 << 22)) return;
        if (static_cast<size_t>(binIdx) >= visitBins.size()) visitBins.resize(static_cast<size_t>(binIdx) + 64);
        auto& bin = visitBins[static_cast<size_t>(binIdx)];
        if (!bin.empty() && std::fabs(bin.back().y - y) < 12.f && atTick - bin.back().tick < 120) return;
        bin.push_back(Visit{y, atTick});
        visitOrder.push_back(binIdx);
    }

    auto SearchRun::visitAdd(Game const& g) -> void {
        if (revisitOn && !g.died) visitAddXY(g.p1.x, g.p1.y, g.tick);
    }

    auto SearchRun::revisited(Game const& g) -> bool {
        int const binIdx = visitBinOf(g.p1.x);
        for (int bin = binIdx - 1; bin <= binIdx + 1; ++bin) {
            if (bin < 0 || static_cast<size_t>(bin) >= visitBins.size()) continue;
            for (auto const& visit : visitBins[static_cast<size_t>(bin)])
                if (visit.tick <= g.tick - kRevisitAge && std::fabs(visit.y - g.p1.y) <= kRevisitDy) return true;
        }
        return false;
    }

    auto SearchRun::applyZone(float x) -> void {
        if (opt.playedTicks || !(x >= zoneX0 && x <= zoneX1)) return;
        beam = std::max(beam, zoneSet.beam);
        qY = std::min(qY, zoneSet.qY);
        qV = std::min(qV, zoneSet.qV);
        gapNow = std::min(gapNow, zoneSet.gap);
    }

    auto SearchRun::applyPassed(float rootX) -> void {
        for (auto const& pass : passed) {
            if (!(pass.x > rootX && pass.x - rootX <= 4000.f)) continue;
            beam = std::max(beam, pass.beam);
            qY = std::min(qY, pass.qY);
            qV = std::min(qV, pass.qV);
            gapNow = std::min(gapNow, pass.gap);
            holdX = std::max(holdX, pass.x);
        }
    }

    auto SearchRun::releaseUnkept(std::vector<std::vector<Node>>& buckets) -> void {
        for (auto& bucket : buckets)
            for (auto& node : bucket)
                if (!node.kept && node.g) {
                    arenas[node.owner].release(node.g);
                    node.g = nullptr;
                }
    }

    auto SearchRun::restartFromMany(std::vector<Root> const& roots, int atTick) -> void {
        for (auto& bucket : curBuckets) bucket.clear();
        for (auto& bucket : nextBuckets) bucket.clear();
        live.clear();
        rootTails.clear();
        curBuckets[0].reserve(roots.size());
        for (auto const& root : roots) {
            curBuckets[0].emplace_back();
            Node& node = curBuckets[0].back();
            node.g = arenas[0].alloc();
            *node.g = *root.g;
            node.g->timingMargin = true;
            node.owner = 0;
            node.parent = -1;
            node.action = static_cast<uint8_t>((root.g->p1Btn ? 1 : 0) | (root.g->p2Btn ? 2 : 0));
            node.dbgHold = 1;
            node.debt = root.debt;
            node.pdebt = root.pdebt;
            node.away = root.away;
            node.span = root.span;
            node.cost = (root.debt - root.pdebt) * opt.missedSpeedCost + prepaidAt(*root.g, cues, opt);
            node.lin0 = static_cast<uint32_t>(curBuckets[0].size() - 1);
            node.lin1 = node.lin0;
            node.lastPress = -1000000;
            node.lastRelease = -1000000;
            rootTails.push_back(*root.tail);
        }
        for (auto& node : curBuckets[0]) live.push_back(&node);
        history.clear();
        rootTick = atTick;
        if (!roots.empty()) applyPassed(roots[0].g->p1.x);
        if (debugOptions().check) {
            int minDebt = 1 << 30;
            int nMinDebt = 0;
            for (auto const& root : roots) {
                if (root.debt < minDebt) {
                    minDebt = root.debt;
                    nMinDebt = 0;
                }
                if (root.debt == minDebt) ++nMinDebt;
            }
            std::fprintf(stderr, "[restart] tick %d : %zu roots, min debt %d (%d roots)\n", atTick, roots.size(),
                         minDebt, nMinDebt);
        }
        if (debugOptions().check >= 2) {
            rootGamesDbg.clear();
            for (auto const& root : roots) rootGamesDbg.push_back(*root.g);
        }
    }

    auto SearchRun::restartFrom(Game const& g, int atTick, std::vector<uint8_t> tail, Span span) -> void {
        restartFromMany({Root{&g, &tail, 0, 0, -1, span}}, atTick);
    }

    auto SearchRun::restartFromSoft(Soft const& softPoint) -> void {
        std::vector<Root> roots;
        for (auto const& state : softPoint.states)
            roots.push_back(Root{&state.g, &state.tail, state.debt, state.pdebt, state.away, state.span});
        restartFromMany(roots, softPoint.tick);
    }

    auto SearchRun::capFar(int restartTick) -> void {
        if (!opt.playedTicks) return;
        if (tick - restartTick > kFourSeconds && beam > opt.beamWidth * 4)
            beam = std::min(opt.maxBeamWidth, opt.beamWidth * 4);
    }

    auto SearchRun::retractUnplayed(int keep, int played) -> int {
        if (hard.size() <= 1 || hard.back().tick <= keep) return 0;
        int popped = 0;
        while (hard.size() > 1 && hard.back().tick > keep && hard[hard.size() - 2].tick >= played) {
            hard.pop_back();
            ++res.stats.hardPops;
            ++popped;
        }
        std::vector<uint8_t> const removed(inputs.begin() + static_cast<long>(hard.back().inputsLen), inputs.end());
        inputs.resize(hard.back().inputsLen);
        for (auto& rootTail : rootTails) rootTail.insert(rootTail.begin(), removed.begin(), removed.end());
        for (auto& softPoint : soft)
            for (auto& state : softPoint.states) state.tail.insert(state.tail.begin(), removed.begin(), removed.end());
        if (opt.onPrefix) opt.onPrefix(inputs);
        return popped;
    }

    auto SearchRun::tailTo(int idx) -> std::vector<uint8_t> {
        std::vector<uint8_t> tail;
        for (int depth = static_cast<int>(history.size()) - 1; depth >= 0 && idx >= 0; --depth) {
            tail.push_back(history[static_cast<size_t>(depth)][static_cast<size_t>(idx)].second);
            idx = history[static_cast<size_t>(depth)][static_cast<size_t>(idx)].first;
        }
        std::reverse(tail.begin(), tail.end());
        std::vector<uint8_t> out = (idx >= 0 && static_cast<size_t>(idx) < rootTails.size())
                                           ? rootTails[static_cast<size_t>(idx)]
                                           : std::vector<uint8_t>{};
        out.insert(out.end(), tail.begin(), tail.end());
        return out;
    }

    auto SearchRun::pathTo(int idx) -> std::vector<uint8_t> {
        std::vector<uint8_t> out = inputs;
        std::vector<uint8_t> tail = tailTo(idx);
        out.insert(out.end(), tail.begin(), tail.end());
        return out;
    }

    auto SearchRun::ancestorAt(int idx, size_t layer) -> int {
        for (size_t depth = history.size(); depth > layer && idx >= 0; --depth)
            idx = history[depth - 1][static_cast<size_t>(idx)].first;
        return idx;
    }

    auto SearchRun::finish(int bestIdx, bool success, std::string why) -> SearchResult {
        if (debugOptions().check && bestIdx >= 0) {
            std::map<std::pair<int, char>, std::pair<int, int>> byDebtMode;
            for (Node* node : live) {
                auto& entry = byDebtMode[{node->debt, modeCharOf(node->g->p1)}];
                if (!entry.first || node->cost < entry.second) entry.second = node->cost;
                ++entry.first;
            }
            std::fprintf(
                    stderr, "[end] chosen: debt %d cost %d mode %c ; layer:", live[static_cast<size_t>(bestIdx)]->debt,
                    live[static_cast<size_t>(bestIdx)]->cost, modeCharOf(live[static_cast<size_t>(bestIdx)]->g->p1));
            for (auto const& entry : byDebtMode)
                std::fprintf(stderr, " [debt %d %c : %d states, min cost %d]", entry.first.first, entry.first.second,
                             entry.second.first, entry.second.second);
            std::fprintf(stderr, "\n");
        }
        res.inputs = bestIdx >= 0 ? pathTo(bestIdx) : furthest;
        res.success = success;
        if (success && opt.menuGoal && bestIdx >= 0) {
            Game const& bestGame = *live[static_cast<size_t>(bestIdx)]->g;
            if (menuGoalReached(bestGame)) {
                res.menuDone = true;
                res.persistAfter = persistAfterOf(bestGame);
            }
        }
        res.why = std::move(why);
        res.endTick = res.startTick + static_cast<int>(res.inputs.size());
        res.presses = 0;
        uint8_t prevButtons = static_cast<uint8_t>((start.p1Btn ? 1 : 0) | (start.p2Btn ? 2 : 0));
        for (uint8_t buttons : res.inputs) {
            if ((buttons & 1) && !(prevButtons & 1)) ++res.presses;
            if ((buttons & 2) && !(prevButtons & 2)) ++res.presses;
            prevButtons = buttons;
        }
        res.stats.ticksSimulated = simulated.load();
        if (debugOptions().stats) {
            for (int worker = 0; worker < nThreads; ++worker)
                std::fprintf(stderr, "  thread %d : %.2f s busy, %ld nodes\n", worker,
                             busy[static_cast<size_t>(worker)], nodesDone[static_cast<size_t>(worker)]);
            std::fprintf(stderr, "  pool : main thread wait %.2f s, %ld slow waits, %ld late starts\n",
                         pool->waitSeconds, pool->slowWaits, pool->lateStarts);
            long nLocalized = 0;
            for (auto const& ctx : localCtx) nLocalized += ctx.localized;
            std::fprintf(stderr, "  localized shared states : %ld\n", nLocalized);
        }
        res.stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
        return res;
    }

    auto SearchRun::bestOf(bool onlyCompleted) -> int {
        int best = -1;
        for (size_t i = 0; i < live.size(); ++i) {
            if (onlyCompleted && !live[i]->g->completed) continue;
            if (best < 0) {
                best = static_cast<int>(i);
                continue;
            }
            Node const& a = *live[i];
            Node const& b = *live[static_cast<size_t>(best)];
            if (g_debtFirst && a.debt != b.debt) {
                if (a.debt < b.debt) best = static_cast<int>(i);
                continue;
            }
            if (a.cost < b.cost) best = static_cast<int>(i);
        }
        return best;
    }

    auto SearchRun::chkLevel() -> int {
        static int const level = debugOptions().check;
        return level;
    }

    auto SearchRun::publish(size_t cutDepth) -> bool {
        int const best = bestOf(false);
        if (best < 0) return false;
        std::vector<uint8_t> full = tailTo(best);
        if (cutDepth == 0 || cutDepth >= full.size()) return false;
        std::vector<uint8_t> prefix(full.begin(), full.begin() + static_cast<long>(cutDepth));
        Game g = hard.back().g;
        if (revisitOn) visitPurge(g.tick + 1);
        Span span = hard.back().span;
        for (uint8_t act : prefix) {
            stepTick(g, act);
            visitAdd(g);
            spanReplay(span, g);
        }
        if (g.died) return false;
        inputs.insert(inputs.end(), prefix.begin(), prefix.end());
        hard.push_back(Checkpoint{g, g.tick, inputs.size(), span});
        if (chkLevel() >= 2) {
            Game replay = start;
            for (uint8_t act : inputs) stepTick(replay, act);
            if (replay.p1.x != g.p1.x || replay.p1.y != g.p1.y || replay.tick != g.tick)
                std::fprintf(stderr,
                             "[check] inconsistent published prefix : replay of inputs (%zu) -> (%.3f,%.3f) tick %d, "
                             "hard point (%.3f,%.3f) tick %d (cut %zu, rootLen %zu, hist %zu)\n",
                             inputs.size(), replay.p1.x, replay.p1.y, replay.tick, g.p1.x, g.p1.y, g.tick, cutDepth,
                             rootTails.empty() ? size_t{0} : rootTails[0].size(), history.size());
        }

        if (hard.size() >= 3 && hard[hard.size() - 2].tick - hard[hard.size() - 3].tick < 120)
            hard.erase(hard.end() - 2);
        if (hard.size() > 64) hard.erase(hard.begin() + 1);
        size_t const rootLen = rootTails.empty() ? 0 : rootTails[0].size();

        auto keepLive = [&](auto&& keep) {
            size_t writePos = 0;
            auto* lastLayer = history.empty() ? nullptr : &history.back();
            for (size_t i = 0; i < live.size(); ++i) {
                if (keep(static_cast<int>(i))) {
                    live[writePos] = live[i];
                    if (lastLayer) (*lastLayer)[writePos] = (*lastLayer)[i];
                    ++writePos;
                } else {
                    live[i]->dead = true;
                    arenas[live[i]->owner].release(live[i]->g);
                    live[i]->g = nullptr;
                }
            }
            live.resize(writePos);
            if (lastLayer) lastLayer->resize(writePos);
        };
        if (cutDepth > rootLen) {
            size_t const cutLayers = cutDepth - rootLen;
            int const ancestor = ancestorAt(best, cutLayers);
            size_t const before = live.size();
            keepLive([&](int idx) { return ancestorAt(idx, cutLayers) == ancestor; });
            if (debugOptions().check)
                std::fprintf(stderr,
                             "[check] publish tick %d : cut %zu rootLen %zu L %zu A %d hist %zu : %zu -> %zu states\n",
                             tick, cutDepth, rootLen, cutLayers, ancestor, history.size(), before, live.size());
            history.erase(history.begin(), history.begin() + static_cast<long>(cutLayers));
            rootTails.assign(1, {});
        } else {
            std::vector<char> rootOk(rootTails.size(), 0);
            bool anyDead = false;
            for (size_t root = 0; root < rootTails.size(); ++root) {
                auto& tail = rootTails[root];
                rootOk[root] = tail.size() >= cutDepth && std::equal(prefix.begin(), prefix.end(), tail.begin());
                if (!rootOk[root]) anyDead = true;
                if (tail.size() >= cutDepth)
                    tail.erase(tail.begin(), tail.begin() + static_cast<long>(cutDepth));
                else
                    tail.clear();
            }
            if (anyDead) {
                size_t const before = live.size();
                keepLive([&](int idx) {
                    int const root = history.empty() ? idx : ancestorAt(idx, 0);
                    return root >= 0 && static_cast<size_t>(root) < rootOk.size() && rootOk[static_cast<size_t>(root)];
                });
                if (debugOptions().check)
                    std::fprintf(stderr,
                                 "[check] publish (roots) tick %d : cut %zu rootLen %zu hist %zu : %zu -> %zu states\n",
                                 tick, cutDepth, rootLen, history.size(), before, live.size());
                if (history.empty()) {
                    std::vector<std::vector<uint8_t>> keptTails;
                    for (size_t root = 0; root < rootTails.size(); ++root)
                        if (rootOk[root]) keptTails.push_back(std::move(rootTails[root]));
                    rootTails.swap(keptTails);
                }
            }
        }
        rootTick = hard.back().tick + static_cast<int>(rootTails.empty() ? 0 : rootTails[0].size());

        std::deque<Soft> kept;
        for (auto& softPoint : soft) {
            std::vector<SoftState> keptStates;
            for (auto& state : softPoint.states) {
                if (state.tail.size() <= cutDepth) continue;
                if (!std::equal(prefix.begin(), prefix.end(), state.tail.begin())) continue;
                state.tail.erase(state.tail.begin(), state.tail.begin() + static_cast<long>(cutDepth));
                keptStates.push_back(std::move(state));
            }
            if (keptStates.empty()) continue;
            softPoint.states.swap(keptStates);
            kept.push_back(std::move(softPoint));
        }
        soft.swap(kept);
        if (chkLevel() >= 2) {
            int const bestNow = bestOf(false);
            if (bestNow >= 0) {
                std::vector<uint8_t> path = pathTo(bestNow);
                Game replay = start;
                for (uint8_t act : path) stepTick(replay, act);
                Game const& ref = *live[static_cast<size_t>(bestNow)]->g;
                if (replay.p1.x != ref.p1.x || replay.p1.y != ref.p1.y || replay.died)
                    std::fprintf(stderr,
                                 "[check] publish cut=%zu rootTail=%zu hist=%zu : replay (%.3f,%.3f dead=%d) != node "
                                 "(%.3f,%.3f) tick %d\n",
                                 cutDepth, rootTails.empty() ? size_t{0} : rootTails[0].size(), history.size(),
                                 replay.p1.x, replay.p1.y, replay.died, ref.p1.x, ref.p1.y, tick);
            }
        }
        if (opt.onPrefix) opt.onPrefix(inputs);
        return true;
    }

    auto SearchRun::convergenceDepth() -> int {
        convA.clear();
        for (size_t i = 0; i < live.size(); ++i) convA.push_back(static_cast<int32_t>(i));
        size_t const rootLen = rootTails.empty() ? 0 : rootTails[0].size();
        for (size_t depth = history.size(); depth > 0; --depth) {
            std::sort(convA.begin(), convA.end());
            convA.erase(std::unique(convA.begin(), convA.end()), convA.end());
            if (convA.size() == 1) return static_cast<int>(rootLen + depth);
            convB.clear();
            for (int32_t idx : convA) convB.push_back(history[depth - 1][static_cast<size_t>(idx)].first);
            convA.swap(convB);
        }
        std::sort(convA.begin(), convA.end());
        convA.erase(std::unique(convA.begin(), convA.end()), convA.end());
        if (convA.size() == 1) return static_cast<int>(rootLen);
        size_t common = rootLen;
        for (size_t i = 1; i < convA.size(); ++i)
            common = std::min(common, commonPrefixLen(rootTails[static_cast<size_t>(convA[0])],
                                                      rootTails[static_cast<size_t>(convA[i])]));
        return static_cast<int>(common);
    }

    auto SearchRun::livePublish() -> void {
        int const played = opt.playedTicks->load();
        int const lead = std::clamp((tick - played) / 3, opt.minLeadLow, opt.minLead);
        int const publishedTo = hard.back().tick;
        bool const starvingNow = opt.starving && opt.starving->load(std::memory_order_relaxed);
        if (played == 0 && !starvingNow && tick - hard.front().tick < kStartLead) return;
        if (publishedTo < played + lead / 2 + 1) {
            int const depthAvail = tick - publishedTo;
            int const behind = (played == 0 && !starvingNow) ? 240 : 8;
            int cut = std::min(played + lead - publishedTo, depthAvail - behind);
            if (opt.starving && opt.starving->load(std::memory_order_relaxed) && widenedAtTick >= 0)
                capLiftedAt = widenedAtTick;
            if (widenedAtTick >= 0 && tick <= widenedAtTick + 480 && ladderRoot >= 0 && capLiftedAt != widenedAtTick)
                cut = std::min(cut, ladderRoot - publishedTo);
            if (cut > 0) publish(static_cast<size_t>(cut));
        }
    }

    auto SearchRun::makeSoft() -> void {
        Soft softPoint;
        softPoint.tick = tick;
        std::vector<int> order(live.size());
        for (size_t i = 0; i < live.size(); ++i) order[i] = static_cast<int>(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return live[static_cast<size_t>(a)]->cost < live[static_cast<size_t>(b)]->cost ||
                   (live[static_cast<size_t>(a)]->cost == live[static_cast<size_t>(b)]->cost &&
                    live[static_cast<size_t>(a)]->lastPress < live[static_cast<size_t>(b)]->lastPress);
        });

        std::vector<uint64_t> classes;
        std::vector<char> taken(live.size(), 0);
        std::vector<int> chosen;
        for (int i : order) {
            if (chosen.size() >= kSoftStates) break;
            Game const& game = *live[static_cast<size_t>(i)]->g;
            uint64_t const sig =
                    (static_cast<uint64_t>(live[static_cast<size_t>(i)]->cls) << 32) ^
                    (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int>(std::floor(game.p1.y / 10.f)) + 4096))
                     << 12) ^
                    static_cast<uint64_t>(
                            static_cast<uint32_t>(static_cast<int>(std::floor(game.p1.yVelocity)) + 2048) & 0xfffu);
            if (std::find(classes.begin(), classes.end(), sig) != classes.end()) continue;
            classes.push_back(sig);
            taken[static_cast<size_t>(i)] = 1;
            chosen.push_back(i);
        }
        for (int i : order) {
            if (chosen.size() >= kSoftStates) break;
            if (taken[static_cast<size_t>(i)]) continue;
            taken[static_cast<size_t>(i)] = 1;
            chosen.push_back(i);
        }
        {
            size_t const depth = history.size();
            size_t const nChosen = chosen.size();
            std::vector<std::vector<uint8_t>> tails(nChosen, std::vector<uint8_t>(depth));
            std::vector<int> idx(chosen.begin(), chosen.end());
            for (size_t h = depth; h-- > 0;) {
                auto const& layer = history[h];
                for (size_t k = 0; k < nChosen; ++k) {
                    if (idx[k] < 0) continue;
                    auto const& e = layer[static_cast<size_t>(idx[k])];
                    tails[k][h] = e.second;
                    idx[k] = e.first;
                }
            }
            softPoint.states.reserve(nChosen);
            for (size_t k = 0; k < nChosen; ++k) {
                int const root = idx[k];
                std::vector<uint8_t> out = (root >= 0 && static_cast<size_t>(root) < rootTails.size())
                                                   ? rootTails[static_cast<size_t>(root)]
                                                   : std::vector<uint8_t>{};
                out.insert(out.end(), tails[k].begin(), tails[k].end());
                softPoint.states.push_back(SoftState{
                        *live[static_cast<size_t>(chosen[k])]->g, std::move(out),
                        live[static_cast<size_t>(chosen[k])]->debt, live[static_cast<size_t>(chosen[k])]->pdebt,
                        live[static_cast<size_t>(chosen[k])]->away, live[static_cast<size_t>(chosen[k])]->span});
            }
        }
        if (debugOptions().check >= 2) {
            for (size_t k = 0; k < softPoint.states.size(); ++k) {
                Game replay = start;
                for (uint8_t act : inputs) stepTick(replay, act);
                for (uint8_t act : softPoint.states[k].tail) stepTick(replay, act);
                Game const& ref = softPoint.states[k].g;
                if (replay.p1.x != ref.p1.x || replay.p1.y != ref.p1.y || replay.tick != ref.tick)
                    std::fprintf(stderr,
                                 "[check2] soft point tick %d state %zu : replay (%.3f,%.3f tick %d) != state "
                                 "(%.3f,%.3f tick %d) ; tail %zu\n",
                                 tick, k, replay.p1.x, replay.p1.y, replay.tick, ref.p1.x, ref.p1.y, ref.tick,
                                 softPoint.states[k].tail.size());
            }
        }
        soft.push_back(std::move(softPoint));
        if (static_cast<int>(soft.size()) > opt.maxSoft) soft.pop_front();
        lastSoft = tick;
    }

    auto SearchRun::leadBase() -> int {
        int base = opt.playedTicks->load();
        if (opt.waitFor) {
            int const lostTick = opt.waitFor->load();
            if (lostTick >= 0) base = std::max(base, lostTick + 480);
        }
        return base;
    }

}
