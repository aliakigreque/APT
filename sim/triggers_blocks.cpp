// Collision blocks and instant collision triggers, spawn-on-touch objects and the end of a step
// (GJBaseGameLayer::checkCollisionBlocks, checkSpawnObjects).

#include "triggers_internal.hpp"

namespace ap::sim {
    namespace trigger_impl {
        // content key of a block pass: motion, dyn, area offsets, player part if a block is a player object, toggles
        uint64_t blkPassKey(Game const& g) {
            TrigState const& T = g.trig;
            TrigHeavy const& H = T.r();
            uint64_t h = mix64(motionHashOf(H.M()), trigDynHash(T.rd()));
            h = mix64(h, T.areaVer);
            if (T.pv) {
                if (PSet const* playerSet = psetOf(g)) {
                    thread_local PSet const* lastPs = nullptr;
                    thread_local bool lastAny = false;
                    thread_local WKey lastW;
                    World const& w = *g.world;
                    if (playerSet != lastPs || lastW != &w) {
                        bool any = false;
                        auto check = [&](int idx) {
                            if (!any && bitOf(playerSet->objBits, idx)) any = true;
                        };
                        for (int i : w.dynamicBlocks) check(i);
                        for (int i : w.blockExtended) check(i);
                        for (int i : w.blockMoving) check(i);
                        for (auto const& col : w.blockCells) {
                            for (auto const& cell : col) {
                                for (int i : cell) check(i);
                            }
                        }
                        lastPs = playerSet;
                        lastAny = any;
                        lastW = &w;
                    }
                    if (lastAny) h = mix64(h, pvHash(*T.pv));
                }
            }
            if (H.offCount) {
                for (uint64_t x : H.offBits) h = mix64(h, x);
            }
            return h;
        }

    }

    // contacts between collision blocks, listened by collision triggers, same as updateCollisionBlocks. could be broken down? TODO
    void trigCollisionBlocks(Game& g) {
        World const& w = *g.world;
        if (!w.hasCollisionTriggers) return;
        struct RectScope {
            bool prev;

            RectScope() : prev(tl_rectCacheOn) { tl_rectCacheOn = !(g_noShare & 2); }

            ~RectScope() { tl_rectCacheOn = prev; }
        } rectScope;
        PosCacheScope posScope;
        if (w.dynamicBlocks.empty() && w.blockCells.empty() && w.blockExtended.empty() && w.blockMoving.empty()) return;

        struct BlkIdx {
            uint64_t mv = ~0ull;
            WKey w;
            std::vector<std::pair<int, int>> byCell;
            std::vector<uint32_t> outStamp;
            uint32_t stamp = 0;
        };
        thread_local BlkIdx bix;
        TrigMotion const* motion = g.trig.h ? &g.trig.r().M() : nullptr;
        bool const anyBlkMoved = motion && !motion->blkCell.items.empty();
        bool const blkSmall = anyBlkMoved && motion->blkCell.items.size() <= 24;
        if (anyBlkMoved && !blkSmall && (bix.mv != motion->ver || bix.w != &w || (g_noShare & 2))) {
            if (bix.w != &w || bix.outStamp.size() != w.objs.size()) {
                bix.outStamp.assign(w.objs.size(), 0);
                bix.stamp = 0;
            }
            bix.mv = motion->ver;
            bix.w = &w;
            bix.byCell.clear();
            if (++bix.stamp == 0) {
                std::fill(bix.outStamp.begin(), bix.outStamp.end(), 0u);
                bix.stamp = 1;
            }
            for (auto const& e : motion->blkCell.items) {
                bix.byCell.emplace_back(e.second, e.first);
                bix.outStamp[static_cast<size_t>(e.first)] = bix.stamp;
            }
            if (bix.byCell.size() > 1) std::sort(bix.byCell.begin(), bix.byCell.end());
        }
        auto const& areaMoved = g.trig.areaMoved;
        thread_local std::vector<int> mvArea;
        mvArea.clear();
        if (!w.blockMoving.empty()) {
            for (int idx : areaMoved)
                if (w.blockMovingMark[static_cast<size_t>(idx)]) mvArea.push_back(idx);
        }
        auto forMoving = [&](Rect const& r, auto&& fn) {
            if (w.blockMoving.empty()) return;
            int const cellX0 = std::max(World::cellOf(static_cast<double>(r.x)) - 1, 0);
            int const cellX1 = World::cellOf(static_cast<double>(r.x + r.w)) + 1;
            int const cellY0 = std::max(w.cellYOf(static_cast<double>(r.y)) - 1, 0);
            int const cellY1 = w.cellYOf(static_cast<double>(r.y + r.h)) + 1;
            auto take = [&](int idx) {
                if (!areaMoved.empty() && std::binary_search(areaMoved.begin(), areaMoved.end(), idx)) return;
                fn(idx);
            };
            struct HitE {
                int first, second;
            };
            HitE hit[24];
            int hitCount = 0;
            int hitPos = 0;
            if (blkSmall) {
                for (auto const& e : motion->blkCell.items) {
                    int const cellKey = e.second;
                    int const keyX = cellKey / 4096;
                    int const keyY = cellKey % 4096;
                    if (keyX < cellX0 || keyX > cellX1 || keyY < cellY0 || keyY > cellY1) continue;
                    hit[hitCount++] = {cellKey, e.first};
                }
                if (hitCount > 1) {
                    std::sort(hit, hit + hitCount, [](HitE const& a, HitE const& b) {
                        return a.first < b.first || (a.first == b.first && a.second < b.second);
                    });
                }
            }
            int const xEnd = std::max(std::min(cellX1, static_cast<int>(w.blockMovCells.size()) - 1), blkSmall ? (hitCount ? hit[hitCount - 1].first / 4096 : -1) : cellX1);
            for (int cx = cellX0; cx <= xEnd; ++cx) {
                if (static_cast<size_t>(cx) < w.blockMovCells.size()) {
                    auto const& col = w.blockMovCells[static_cast<size_t>(cx)];
                    for (int cy = cellY0; cy <= cellY1 && static_cast<size_t>(cy) < col.size(); ++cy) {
                        for (int idx : col[static_cast<size_t>(cy)]) {
                            if (anyBlkMoved && (blkSmall ? motion->blkCell.find(idx) != nullptr : bix.outStamp[static_cast<size_t>(idx)] == bix.stamp))
                                continue;
                            take(idx);
                        }
                    }
                }
                if (anyBlkMoved && !blkSmall) {
                    auto it = std::lower_bound(bix.byCell.begin(), bix.byCell.end(), std::make_pair(World::cellKey(cx, cellY0), INT32_MIN));
                    for (; it != bix.byCell.end() && it->first <= World::cellKey(cx, cellY1); ++it) take(it->second);
                } else if (blkSmall) {
                    while (hitPos < hitCount && hit[hitPos].first / 4096 == cx) take(hit[hitPos++].second);
                }
            }
            for (int idx : mvArea) fn(idx);
        };
        auto candidates = [&](Rect const& r, bool withMoving, auto&& fn) {
            int const cellX0 = std::max(World::cellOf(static_cast<double>(r.x)) - 1, 0);
            int const cellX1 = World::cellOf(static_cast<double>(r.x + r.w)) + 1;
            int const cellY0 = std::max(w.cellYOf(static_cast<double>(r.y)) - 1, 0);
            int const cellY1 = w.cellYOf(static_cast<double>(r.y + r.h)) + 1;
            for (int cx = cellX0; cx <= cellX1; ++cx) {
                if (static_cast<size_t>(cx) >= w.blockCells.size()) break;
                auto const& col = w.blockCells[static_cast<size_t>(cx)];
                for (int cy = cellY0; cy <= cellY1 && static_cast<size_t>(cy) < col.size(); ++cy) {
                    for (int idx : col[static_cast<size_t>(cy)]) fn(idx);
                }
            }
            for (int idx : w.blockExtended) fn(idx);
            for (int idx : w.dynamicBlocks) fn(idx);
            if (withMoving) forMoving(r, fn);
        };
        size_t const dynCount = w.dynamicBlocks.size();
        auto overlap = [](Rect const& a, Rect const& b) {
            return a.x <= b.x + b.w && b.x <= a.x + a.w && a.y <= b.y + b.h && b.y <= a.y + a.h;
        };

        thread_local int bpWin = 0;
        thread_local int bpReuse = 0;
        thread_local int bpBuild = 0;
        thread_local int bpCool = 0;
        thread_local WKey bpW;
        if (bpW != &w) {
            bpW = &w;
            bpWin = bpReuse = bpBuild = bpCool = 0;
        }
        bool const cool = bpCool > 0;
        if (cool) --bpCool;
        bool const cacheOn = !cool && !(g_noShare & 2) && dynCount >= 30 && g.trig.h && g.trig.d && g.rotOvN == 0;
        uint64_t const passKey = cacheOn ? blkPassKey(g) : 0;
        bool const reuse = cacheOn && tl_blkPass.valid && tl_blkPass.w == &w && tl_blkPass.key == passKey;
        bool const build = cacheOn && !reuse && tl_blkPassLast == passKey && tl_blkPassLastW == &w;
        if (cacheOn) {
            tl_blkPassLast = passKey;
            tl_blkPassLastW = &w;
        }
        BlkPass& cache = (reuse || build) ? tl_blkPass : tl_blkScratch;
        bool const shared = reuse || build;
        if (cacheOn) {
            bpReuse += reuse ? 1 : 0;
            bpBuild += build ? 1 : 0;
            if (++bpWin >= 512) {
                if (bpReuse < 3 * std::max(bpBuild, 1)) bpCool = 4096;
                bpWin = bpReuse = bpBuild = 0;
            }
        }
        if (!reuse) {
            cache.valid = false;
            cache.dRect.resize(dynCount);
            cache.dOff.resize(dynCount);
            for (size_t k = 0; k < dynCount; ++k) {
                int const blockIdx = w.dynamicBlocks[k];
                if (!w.collisionItem(objAt(g, blockIdx).itemID)) {
                    cache.dOff[k] = 1;
                    continue;
                }
                GObj block = g.obj(blockIdx);
                cache.dOff[k] = block.disabled() ? 1 : 0;
                if (!cache.dOff[k]) cache.dRect[k] = block.getObjectRect();
            }
            cache.dSweep = dynCount >= 16;
            cache.dMaxW = 0.f;
            if (cache.dSweep) {
                cache.dSort.clear();
                cache.dRanges.clear();
                for (size_t j = 0; j < dynCount; ++j) {
                    if (cache.dOff[j]) continue;
                    Rect const& r = cache.dRect[j];
                    if (!std::isfinite(r.x) || !std::isfinite(r.w) || !std::isfinite(r.y) || !std::isfinite(r.h)) {
                        cache.dSweep = false;
                        break;
                    }
                    cache.dSort.push_back(static_cast<uint32_t>(j));
                    if (r.w > cache.dMaxW) cache.dMaxW = r.w;
                }
                if (cache.dSweep) {
                    auto itemOf = [&](uint32_t j) { return objAt(g, w.dynamicBlocks[j]).itemID; };
                    auto const& rects = cache.dRect;
                    auto less = [&](uint32_t a, uint32_t b) {
                        int const itemA = itemOf(a);
                        int const itemB = itemOf(b);
                        if (itemA != itemB) return itemA < itemB;
                        return rects[a].x < rects[b].x || (rects[a].x == rects[b].x && a < b);
                    };
                    thread_local std::vector<uint32_t> prevOrd;
                    thread_local std::vector<uint32_t> inSet;
                    thread_local uint32_t inNow = 0;
                    thread_local WKey ordW;
                    if (ordW != &w || inSet.size() != dynCount) {
                        ordW = &w;
                        prevOrd.clear();
                        inSet.assign(dynCount, 0u);
                        inNow = 0;
                    }
                    if (inNow > 0xFFFFFF00u) {
                        std::fill(inSet.begin(), inSet.end(), 0u);
                        inNow = 0;
                    }
                    uint32_t const markIn = ++inNow;
                    uint32_t const taken = ++inNow;
                    for (uint32_t j : cache.dSort) inSet[j] = markIn;
                    thread_local std::vector<uint32_t> seq;
                    seq.clear();
                    for (uint32_t j : prevOrd) {
                        if (inSet[j] == markIn) {
                            seq.push_back(j);
                            inSet[j] = taken;
                        }
                    }
                    for (uint32_t j : cache.dSort)
                        if (inSet[j] == markIn) seq.push_back(j);
                    size_t moves = 0;
                    size_t const budget = 8 * seq.size() + 64;
                    bool tooMany = false;
                    for (size_t i = 1; i < seq.size() && !tooMany; ++i) {
                        uint32_t const v = seq[i];
                        size_t k = i;
                        while (k > 0 && less(v, seq[k - 1])) {
                            seq[k] = seq[k - 1];
                            --k;
                            if (++moves > budget) {
                                tooMany = true;
                                break;
                            }
                        }
                        seq[k] = v;
                    }
                    if (tooMany) std::sort(seq.begin(), seq.end(), less);
                    cache.dSort.swap(seq);
                    prevOrd.assign(cache.dSort.begin(), cache.dSort.end());
                    cache.dSortX.resize(cache.dSort.size());
                    for (size_t i = 0; i < cache.dSort.size(); ++i) {
                        cache.dSortX[i] = cache.dRect[cache.dSort[i]].x;
                        int const item = itemOf(cache.dSort[i]);
                        if (cache.dRanges.empty() || cache.dRanges.back().item != item) {
                            cache.dRanges.push_back(
                                    BlkPass::ItemRange{item, static_cast<uint32_t>(i), static_cast<uint32_t>(i)});
                        }
                        cache.dRanges.back().e = static_cast<uint32_t>(i + 1);
                    }
                }
            }
        }
        auto const& dRect = cache.dRect;
        auto const& dOff = cache.dOff;
        bool const dSweep = cache.dSweep;
        float const dMaxW = cache.dMaxW;
        auto const& dSort = cache.dSort;
        auto const& dSortX = cache.dSortX;
        auto const& dRanges = cache.dRanges;
        thread_local std::vector<OBB> dObb;
        thread_local std::vector<uint8_t> dObbOk;
        dObb.resize(dynCount);
        dObbOk.assign(dynCount, 0);
        auto obbOf = [&](size_t k) -> OBB const* {
            if (!dObbOk[k]) {
                dObb[k] = objectObb(g.obj(w.dynamicBlocks[k]));
                dObbOk[k] = 1;
            }
            return &dObb[k];
        };
        thread_local std::vector<uint32_t> dCand;
        auto enumOthers = [&](size_t k, Rect const& blockRect, auto&& fn) {
            int const blockIdx = w.dynamicBlocks[k];
            int ord = 0;
            int const cellX0 = std::max(World::cellOf(static_cast<double>(blockRect.x)) - 1, 0);
            int const cellX1 = World::cellOf(static_cast<double>(blockRect.x + blockRect.w)) + 1;
            int const cellY0 = std::max(w.cellYOf(static_cast<double>(blockRect.y)) - 1, 0);
            int const cellY1 = w.cellYOf(static_cast<double>(blockRect.y + blockRect.h)) + 1;
            for (int cx = cellX0; cx <= cellX1; ++cx) {
                if (static_cast<size_t>(cx) >= w.blockCells.size()) break;
                auto const& col = w.blockCells[static_cast<size_t>(cx)];
                for (int cy = cellY0; cy <= cellY1 && static_cast<size_t>(cy) < col.size(); ++cy) {
                    for (int other : col[static_cast<size_t>(cy)]) {
                        if (other != blockIdx) fn(other, ord);
                        ++ord;
                    }
                }
            }
            for (int other : w.blockExtended) {
                if (other != blockIdx) fn(other, ord);
                ++ord;
            }
            if (w.blockMovPair[k]) {
                forMoving(blockRect, [&](int other) {
                    if (other != blockIdx) fn(other, ord);
                    ++ord;
                });
            }
        };
        auto fullBlock = [&](size_t k, int mode, size_t pos) {
            if (dOff[k] || !w.collisionItem(objAt(g, w.dynamicBlocks[k]).itemID)) return;
            int const blockIdx = w.dynamicBlocks[k];
            int const blockItem = objAt(g, blockIdx).itemID;
            auto pairOk = [&](int other) {
                int const otherItem = objAt(g, other).itemID;
                return w.collisionItem(otherItem) && otherItem < 10002 && blockItem < 10002 && w.collisionPairListened(blockItem, otherItem) && !alreadyCollided(g, blockItem, otherItem);
            };
            Rect const blockRect = dRect[k];
            bool const useObb = true;
            if (mode <= 1) {
                enumOthers(k, blockRect, [&](int other, int ord) {
                    if (mode == 1 && static_cast<size_t>(ord) < pos) return;
                    if (!pairOk(other)) return;
                    Rect const otherRect = g.obj(other).getObjectRect();
                    if (!overlap(otherRect, blockRect)) return;
                    checkBlockAgainst(g, blockIdx, blockRect, useObb, other, false, otherRect, nullptr, obbOf(k));
                });
            }
            if (dSweep) {
                if (mode != 2) {
                    float const hi = blockRect.x + blockRect.w;
                    float const lo = blockRect.x - dMaxW - (1.f + std::fabs(blockRect.x) * 1e-6f);
                    dCand.clear();
                    if (blockItem >= 0 && blockItem < 10002) {
                        for (int const partnerItem : w.collisionPartners[static_cast<size_t>(blockItem)]) {
                            auto range =
                                    std::lower_bound(dRanges.begin(), dRanges.end(), partnerItem, [](BlkPass::ItemRange const& a, int v) { return a.item < v; });
                            if (range == dRanges.end() || range->item != partnerItem) continue;
                            if (alreadyCollided(g, blockItem, partnerItem)) continue;
                            auto const xBegin = dSortX.begin() + range->b;
                            auto const xEnd = dSortX.begin() + range->e;
                            for (auto it = std::lower_bound(xBegin, xEnd, lo); it != xEnd && *it <= hi; ++it) {
                                uint32_t const j = dSort[static_cast<size_t>(it - dSortX.begin())];
                                if (j != k && overlap(dRect[j], blockRect)) dCand.push_back(j);
                            }
                        }
                    }
                    if (dCand.size() > 1) std::sort(dCand.begin(), dCand.end());
                }
                for (size_t q = mode == 2 ? pos : 0; q < dCand.size(); ++q) {
                    uint32_t const j = dCand[q];
                    checkBlockAgainst(g, blockIdx, blockRect, useObb, w.dynamicBlocks[j], false, dRect[j], obbOf(j), obbOf(k));
                }
            } else {
                for (size_t j = mode == 2 ? pos : 0; j < dynCount; ++j) {
                    if (j == k || dOff[j] || !overlap(dRect[j], blockRect) || !pairOk(w.dynamicBlocks[j])) continue;
                    checkBlockAgainst(g, blockIdx, blockRect, useObb, w.dynamicBlocks[j], false, dRect[j], obbOf(j), obbOf(k));
                }
            }
        };
        if (!shared) {
            for (size_t k = 0; k < dynCount; ++k) fullBlock(k, 0, 0);
        } else {
            if (build) {
                cache.obb.resize(dynCount);
                for (size_t k = 0; k < dynCount; ++k) {
                    if (!cache.dOff[k]) cache.obb[k] = objectObb(g.obj(w.dynamicBlocks[k]));
                }
                cache.blocks.clear();
                cache.a.clear();
                cache.b.clear();
                for (size_t k = 0; k < dynCount; ++k) {
                    if (dOff[k] || !w.collisionItem(objAt(g, w.dynamicBlocks[k]).itemID)) continue;
                    int const blockIdx = w.dynamicBlocks[k];
                    Obj const& blockObj = objAt(g, blockIdx);
                    int const blockItem = blockObj.itemID;
                    Rect const blockRect = dRect[k];
                    BlkPass::Blk entry{static_cast<uint32_t>(k), static_cast<uint32_t>(cache.a.size()), 0, 0, 0};
                    enumOthers(k, blockRect, [&](int other, int ord) {
                        Obj const& otherObj = objAt(g, other);
                        int const otherItem = otherObj.itemID;
                        if (!(w.collisionItem(otherItem) && otherItem < 10002 && blockItem < 10002 && w.collisionPairListened(blockItem, otherItem)))
                            return;
                        GObj otherG = g.obj(other);
                        Rect const otherRect = otherG.getObjectRect();
                        if (!overlap(otherRect, blockRect)) return;
                        OBB const otherObb = objectObb(otherG);
                        bool const pass = cache.obb[k].overlaps1Way(otherObb) && otherObb.overlaps1Way(cache.obb[k]);
                        cache.a.push_back(BlkPass::EA{other, ord, otherItem, static_cast<uint8_t>(pass ? 1 : 0), static_cast<uint8_t>(otherObj.uid == blockObj.uid ? 1 : 0)});
                    });
                    entry.aE = static_cast<uint32_t>(cache.a.size());
                    entry.bB = static_cast<uint32_t>(cache.b.size());
                    if (dSweep) {
                        float const hi = blockRect.x + blockRect.w;
                        float const lo = blockRect.x - dMaxW - (1.f + std::fabs(blockRect.x) * 1e-6f);
                        size_t const bStart = cache.b.size();
                        if (blockItem >= 0 && blockItem < 10002) {
                            for (int const partnerItem : w.collisionPartners[static_cast<size_t>(blockItem)]) {
                                auto range =
                                        std::lower_bound(dRanges.begin(), dRanges.end(), partnerItem, [](BlkPass::ItemRange const& a, int v) { return a.item < v; });
                                if (range == dRanges.end() || range->item != partnerItem) continue;
                                auto const xBegin = dSortX.begin() + range->b;
                                auto const xEnd = dSortX.begin() + range->e;
                                for (auto it = std::lower_bound(xBegin, xEnd, lo); it != xEnd && *it <= hi; ++it) {
                                    uint32_t const j = dSort[static_cast<size_t>(it - dSortX.begin())];
                                    if (j != k && overlap(dRect[j], blockRect)) {
                                        cache.b.push_back(BlkPass::EB{j, partnerItem, 0, 0, 0});
                                    }
                                }
                            }
                        }
                        std::stable_sort(cache.b.begin() + static_cast<std::ptrdiff_t>(bStart), cache.b.end(), [](BlkPass::EB const& x, BlkPass::EB const& y) { return x.j < y.j; });
                    } else {
                        for (size_t j = 0; j < dynCount; ++j) {
                            if (j == k || dOff[j] || !overlap(dRect[j], blockRect)) continue;
                            int const otherItem = objAt(g, w.dynamicBlocks[j]).itemID;
                            if (!(w.collisionItem(otherItem) && otherItem < 10002 && blockItem < 10002 && w.collisionPairListened(blockItem, otherItem)))
                                continue;
                            cache.b.push_back(BlkPass::EB{static_cast<uint32_t>(j), otherItem, 0, 0, 0});
                        }
                    }
                    for (size_t q = entry.bB; q < cache.b.size(); ++q) {
                        auto& pair = cache.b[q];
                        Obj const& otherObj = objAt(g, w.dynamicBlocks[pair.j]);
                        OBB const& blockObb = cache.obb[k];
                        OBB const& otherObb = cache.obb[pair.j];
                        pair.pass = (blockObb.overlaps1Way(otherObb) && otherObb.overlaps1Way(blockObb)) ? 1 : 0;
                        pair.uidEq = otherObj.uid == blockObj.uid ? 1 : 0;
                        pair.ok = (blockItem < 10002 && otherObj.itemID < 10002 && w.collisionPairListened(blockItem, otherObj.itemID)) ? 1 : 0;
                    }
                    entry.bE = static_cast<uint32_t>(cache.b.size());
                    if (entry.aE > entry.aB || entry.bE > entry.bB) cache.blocks.push_back(entry);
                }
                cache.key = passKey;
                cache.w = &w;
                cache.valid = true;
            }

            thread_local std::vector<uint32_t> usedSt;
            thread_local uint32_t usedNow = 0;
            if (usedSt.size() != dynCount) {
                usedSt.assign(dynCount, 0u);
                usedNow = 0;
            }
            if (++usedNow == 0) {
                std::fill(usedSt.begin(), usedSt.end(), 0u);
                usedNow = 1;
            }
            auto markUsed = [&](size_t k) { usedSt[k] = usedNow; };
            auto live = [&](int a, int b) {
                if (a >= 10002 || b >= 10002) return false;
                int const hiItem = a < b ? b : a;
                int const loItem = a < b ? a : b;
                if (g.trig.collided.find((hiItem + 1000) * 10000 + loItem)) return false;
                for (auto const& act : g.trig.r().collisions) {
                    if (!act.stopped && !act.paused && !act.onExit && act.a == hiItem && act.b == loItem) return true;
                }
                return false;
            };
            size_t resumeK = dynCount;
            int resumeMode = 0;
            size_t resumePos = 0;
            for (auto const& blk : cache.blocks) {
                size_t const k = blk.k;
                int const blockIdx = w.dynamicBlocks[k];
                int const blockItem = objAt(g, blockIdx).itemID;
                bool stop = false;
                for (uint32_t q = blk.aB; q < blk.aE; ++q) {
                    auto const& pair = cache.a[q];
                    if (alreadyCollided(g, blockItem, pair.item)) continue;
                    if (pair.pass && !pair.uidEq && !g.obj(pair.other).disabled() && live(blockItem, pair.item)) {
                        resumeK = k;
                        resumeMode = 1;
                        resumePos = static_cast<size_t>(pair.ord);
                        stop = true;
                        break;
                    }
                    markUsed(k);
                    if (pair.uidEq || !pair.pass) continue;
                    if (g.obj(pair.other).disabled()) continue;
                    objectsCollided(g, blockItem, pair.item);
                }
                if (stop) break;
                if (dSweep) {
                    thread_local std::vector<int> skipIt;
                    skipIt.clear();
                    if (blk.bE > blk.bB && blockItem >= 0 && blockItem < 10002) {
                        for (int const partnerItem : w.collisionPartners[static_cast<size_t>(blockItem)]) {
                            if (alreadyCollided(g, blockItem, partnerItem)) skipIt.push_back(partnerItem);
                        }
                    }
                    auto skipped = [&](int item) {
                        for (int s : skipIt)
                            if (s == item) return true;
                        return false;
                    };
                    size_t candPos = 0;
                    for (uint32_t q = blk.bB; q < blk.bE; ++q) {
                        auto const& pair = cache.b[q];
                        if (skipped(pair.item)) continue;
                        if (pair.ok && !pair.uidEq && pair.pass && !g.obj(w.dynamicBlocks[pair.j]).disabled() &&
                            live(blockItem, objAt(g, w.dynamicBlocks[pair.j]).itemID)) {
                            dCand.clear();
                            for (uint32_t r = blk.bB; r < blk.bE; ++r) {
                                if (!skipped(cache.b[r].item)) dCand.push_back(cache.b[r].j);
                            }
                            resumeK = k;
                            resumeMode = 2;
                            resumePos = candPos;
                            stop = true;
                            break;
                        }
                        markUsed(pair.j);
                        markUsed(k);
                        ++candPos;
                        if (!pair.ok || pair.uidEq || !pair.pass) continue;
                        if (g.obj(w.dynamicBlocks[pair.j]).disabled()) continue;
                        objectsCollided(g, blockItem, objAt(g, w.dynamicBlocks[pair.j]).itemID);
                    }
                } else {
                    for (uint32_t q = blk.bB; q < blk.bE; ++q) {
                        auto const& pair = cache.b[q];
                        if (alreadyCollided(g, blockItem, pair.item)) continue;
                        if (pair.ok && !pair.uidEq && pair.pass && !g.obj(w.dynamicBlocks[pair.j]).disabled() &&
                            live(blockItem, pair.item)) {
                            resumeK = k;
                            resumeMode = 2;
                            resumePos = pair.j;
                            stop = true;
                            break;
                        }
                        markUsed(pair.j);
                        markUsed(k);
                        if (!pair.ok || pair.uidEq || !pair.pass) continue;
                        if (g.obj(w.dynamicBlocks[pair.j]).disabled()) continue;
                        objectsCollided(g, blockItem, pair.item);
                    }
                }
                if (stop) break;
            }
            if (resumeK < dynCount) {
                for (size_t k = 0; k < dynCount; ++k) {
                    if (usedSt[k] == usedNow) {
                        dObb[k] = cache.obb[k];
                        dObbOk[k] = 1;
                    }
                }
                fullBlock(resumeK, resumeMode, resumePos);
                for (size_t k = resumeK + 1; k < dynCount; ++k) fullBlock(k, 0, 0);
            }
        }

        for (int pIndex = 0; pIndex < (g.isDualMode ? 2 : 1); ++pIndex) {
            Player const& p = pIndex == 0 ? g.p1 : g.p2;
            if (p.isDead) continue;
            int const item = 10000 + pIndex;
            if (!w.collisionItem(item)) continue;
            OBB playerObb;
            playerObb.calculateWithCenter(Point{p.x, p.y}, p.spriteWidthScale * 30.f * p.scaleX,
                                          p.spriteHeightScale * 30.f * p.scaleY, -p.rotation * 0.017453292f);
            Rect const playerRect = playerObb.boundingRect();
            candidates(playerRect, w.blockMovPlayer[pIndex], [&](int other) {
                if (!w.collisionPairListened(item, objAt(g, other).itemID) ||
                    alreadyCollided(g, item, objAt(g, other).itemID))
                    return;
                GObj otherG = g.obj(other);
                if (otherG.disabled()) return;
                Rect const otherRect = otherG.getObjectRect();
                if (!(otherRect.x <= playerRect.x + playerRect.w && playerRect.x <= otherRect.x + otherRect.w && otherRect.y <= playerRect.y + playerRect.h && playerRect.y <= otherRect.y + otherRect.h))
                    return;
                OBB const otherObb = objectObb(otherG);
                if (!playerObb.overlaps1Way(otherObb) || !otherObb.overlaps1Way(playerObb)) return;
                objectsCollided(g, item, objAt(g, other).itemID);
            });
        }
        if (g.isDualMode && !g.p1.isDead && !g.p2.isDead && w.collisionPairListened(10000, 10001) &&
            !alreadyCollided(g, 10000, 10001)) {
            auto boxes = [](Player const& p, OBB& b, Rect& r) {
                b.calculateWithCenter(Point{p.x, p.y}, p.spriteWidthScale * 30.f * p.scaleX,
                                      p.spriteHeightScale * 30.f * p.scaleY, -p.rotation * 0.017453292f);
                r = b.boundingRect();
            };
            OBB b1;
            OBB b2;
            Rect r1;
            Rect r2;
            boxes(g.p1, b1, r1);
            boxes(g.p2, b2, r2);
            if (r1.x <= r2.x + r2.w && r2.x <= r1.x + r1.w && r1.y <= r2.y + r2.h && r2.y <= r1.y + r1.h &&
                b1.overlaps1Way(b2) && b2.overlaps1Way(b1)) {
                objectsCollided(g, 10000, 10001);
            }
        }
    }

    // activates the position triggers the player has reached, in channel order, same as checkSpawnObjects
    void trigCheckSpawnObjects(Game& g) {
        TrigState& T = g.trig;
        World const& w = *g.world;
        float const px = g.p1.x;
        float const py = g.p1.y;
        auto cursorOf = [&](int ch) {
            if (ch == 0) return T.r().spawnCursor;
            auto* cursor = T.r().spawnCursors.find(ch);
            return cursor ? *cursor : 0;
        };
        auto reached = [&](Obj const& o, bool rev) {
            bool const side = g.p1.isSideways && !g.isPlatformer;
            if (!side) return rev ? !(o.startX < px) : !(px < o.startX);
            return rev ? !(o.startY < py) : !(py < o.startY);
        };
        bool const side = g.p1.isSideways && !g.isPlatformer;
        int const startChannel = g.currentChannel;
        bool const startReversed = g.reversedChannels.contains(startChannel);
        {
            std::vector<int> const* list = w.spawnList(startChannel);
            if (!list) return;
            int const cursor = cursorOf(startChannel);
            if (cursor >= static_cast<int>(list->size()) ||
                !reached(objAt(g, (*list)[static_cast<size_t>(cursor)]), startReversed))
                return;
        }
        memoTick(g);
        bool const memoActive = memoOn();
        uint64_t key = 0;
        uint64_t keysBefore = 0;
        std::shared_ptr<TrigDyn> fullBefore;
        if (memoActive) {
            key = mix64(mix64(mix64(trigHeavyHash(T.r()), bits64(side ? py : px)), static_cast<uint64_t>(static_cast<uint32_t>(startChannel)) | (side ? 1ull << 40 : 0) | (startReversed ? 1ull << 41 : 0)),
                        0x7707ull);
            if (MemoEntry const* entry = memoLookup(T, key)) {
                g.itemChanges += entry->itemDelta;
                return;
            }
            keysBefore = dynKeysHash(T.rd());
            fullBefore = T.d;
            spawnMark(T);
        }
        tl_sideEffect = false;
        tl_dynDep = 0;
        tl_itemDelta = 0;
        tl_memoComp = memoActive;
        tl_pHit = false;
        for (;;) {
            int const ch = g.currentChannel;
            std::vector<int> const* list = w.spawnList(ch);
            if (!list) break;
            int cursor = cursorOf(ch);
            if (cursor >= static_cast<int>(list->size())) break;
            int const idx = (*list)[static_cast<size_t>(cursor)];
            Obj const& o = objAt(g, idx);
            if (!reached(o, g.reversedChannels.contains(ch))) break;
            ++cursor;
            if (ch == 0)
                T.w().spawnCursor = cursor;
            else
                T.w().spawnCursors[ch] = cursor;
            if (g_trace) {
                std::printf(
                        "[spawn] channel %d%s cursor %d : uid %d id %d start (%.1f, %.1f) player (%.1f, %.1f)\n",
                        ch, (g.reversedChannels.contains(ch)) ? " reversed" : "", cursor - 1, o.uid, o.id, o.startX, o.startY, px, py);
            }
            if (!trigGroupDisabled(g, o)) triggerObject(g, idx, 0);
        }
        tl_memoComp = false;
        if (!tl_sideEffect && !tl_pHit && memoActive) memoCommit(key, T, keysBefore, fullBefore);
    }

    //advances the advanced follow delay counter by round(timewarp * 1000), half on half steps (processCommands)
    void trigAfCommands(Game& g, bool half) {
        if (!g.trig.on || !g.world->afRingFloats) return;
        TrigState& T = g.trig;
        float const warp = std::round(g.timeWarp * 1000.f);
        if (half) {
            T.afU6 = static_cast<unsigned>(static_cast<long long>(static_cast<float>(static_cast<int>(warp)) * 0.5f + static_cast<float>(T.afU6)));
        } else
            T.afU6 = T.afU6 + static_cast<unsigned>(static_cast<int>(warp));
    }

    // end of step: records the last position of each advanced follow key in its ring, when it's time
    void trigAfRecord(Game& g) {
        World const& w = *g.world;
        if (!g.trig.on || !w.afRingFloats) return;
        TrigState& T = g.trig;
        if (!(T.afU5 == 0 || T.afU5 + 1000u <= T.afU6)) return;
        T.afU5 = T.afU6;
        ++T.afN;
        if (T.afRing.size() != static_cast<size_t>(w.afRingFloats)) {
            T.afRing.assign(static_cast<size_t>(w.afRingFloats), 0.f);
        }
        if (T.afRingH.size() != w.afRings.size()) T.afRingH.assign(w.afRings.size(), 0ull);
        size_t ringIdx = 0;
        for (auto const& ring : w.afRings) {
            float x = 0.f;
            float y = 0.f;
            if (ring.key == -1) {
                x = g.p1.lastX;
                y = g.p1.lastY;
            } else if (ring.key == -2) {
                if (!g.p2Origin) {
                    x = g.p2.lastX;
                    y = g.p2.lastY;
                }
            } else if (ring.key == -3) {
                x = g.cam.lx;
                y = g.cam.ly;
            } else
                trigLastPos(g, ring.obj, x, y);
            size_t const pos = static_cast<size_t>(T.afN % ring.size + ring.base) * 2;
            uint64_t& hash = ring.key > 0 ? T.afHashObj : T.afHashP;
            uint64_t const oldHash = mix64(mix64(pos, bits64(T.afRing[pos])), bits64(T.afRing[pos + 1]));
            T.afRing[pos] = x;
            T.afRing[pos + 1] = y;
            uint64_t const newHash = mix64(mix64(pos, bits64(x)), bits64(y));
            hash ^= oldHash ^ newHash;
            T.afRingH[ringIdx++] ^= oldHash ^ newHash;
        }
    }

    // state blocks (3640) not touched this step turn off and spawn their center group
    void trigStateObjects(Game& g) {
        auto& blocks = g.trig.stateBlk;
        if (blocks.empty()) return;
        int const cmd = static_cast<int>(g.commandIndex);
        std::vector<int> outs;
        for (size_t i = 0; i < blocks.size();) {
            if ((blocks[i].second >> 1) < cmd) {
                if (blocks[i].second & 1) outs.push_back(blocks[i].first);
                blocks.erase(blocks.begin() + static_cast<long>(i));
            } else
                ++i;
        }
        for (int idx : outs) {
            Obj const& o = g.world->objs[static_cast<size_t>(idx)];
            if (o.centerGroup > 0) spawnGroupLayer(g, o.centerGroup, false, 0.0, o.uid, o.controlID, nullptr);
        }
    }

    // end of trigger tick: collision exits, player y history, camera tweens, mirror, timewarp
    void trigEndTick(Game& g, double dtSec) {
        TrigState& T = g.trig;
        for (size_t i = 0; i < T.collided.items.size();) {
            auto e = T.collided.items[i];
            if (e.second) {
                ++i;
                continue;
            }
            T.collided.items.erase(T.collided.items.begin() + static_cast<long>(i));
            int const key = e.first;
            int const a = (key - 10000000) / 10000;
            int const b = key - (a + 1000) * 10000;
            handleObjectCollision(g, false, a, b);
            i = 0;
        }
        if (g.world && (g.world->fyDelayAlways || !g.world->fyDelayX.empty())) {
            T.yHistAcc += static_cast<float>(dtSec);
            if (0.01f <= T.yHistAcc) {
                T.yHistAcc -= 0.01f;
                ++T.yHistIdx;
            }
            World const& w = *g.world;
            bool need = w.fyDelayAlways;
            if (!need) {
                auto it = std::lower_bound(w.fyDelayX.begin(), w.fyDelayX.end(), g.p1.x - 1400.f);
                need = it != w.fyDelayX.end() && *it <= g.p1.x + 1400.f;
            }
            if (!need && !T.yHist.empty()) {
                for (auto const& c : T.r().M().cmds) {
                    if (c.type == CmdType::FollowY && !c.disabled && c.fyDelay > 0.0) {
                        need = true;
                        break;
                    }
                }
            }
            if (need && T.yHist.empty())
                T.yHist.assign(200, 0.f);
            else if (!need && !T.yHist.empty()) {
                T.yHist.clear();
                T.yHist.shrink_to_fit();
            }
            if (!T.yHist.empty()) T.yHist[static_cast<size_t>(T.yHistIdx % 200)] = g.p1.y;
        } else if (!T.yHist.empty()) {
            T.yHistAcc += static_cast<float>(dtSec);
            if (0.01f <= T.yHistAcc) {
                T.yHistAcc -= 0.01f;
                ++T.yHistIdx;
            }
            T.yHist[static_cast<size_t>(T.yHistIdx % 200)] = g.p1.y;
        }
        for (size_t i = 0; i < T.tweens.items.size();) {
            auto& e = T.tweens.items[i];
            Tween& tween = e.second;
            if (!tween.finished && !tween.paused) {
                tween.elapsed = static_cast<float>(dtSec) + tween.elapsed;
                float f = tween.elapsed < tween.duration ? tween.elapsed / tween.duration : 1.f;
                if (0 < tween.easing) f = easedValue(f, tween.easing, tween.rate);
                tween.finished = tween.duration <= tween.elapsed;
                tween.value = (tween.to - tween.from) * f + tween.from;
            }
            switch (e.first) {
            case 7:
                g.mirrorT = tween.value;
                break;
            case 0xe:
                g.cameraZoom = tween.value;
                break;
            case 0xf:
                T.cameraOffsetX = tween.value;
                break;
            case 0x10:
                g.cameraOffsetY = tween.value;
                break;
            default:
                cameraTweenSet(g, e.first, tween.value);
                break;
            }
            if (tween.finished)
                T.tweens.items.erase(T.tweens.items.begin() + static_cast<long>(i));
            else
                ++i;
        }
        if (g.mirrorT == 1.f)
            g.mirrorDir = -1.f;
        else if (g.mirrorT == 0.f)
            g.mirrorDir = 1.f;
        if (0.f < T.queuedTimeWarp) {
            float v = T.queuedTimeWarp;
            if (2.f <= v) v = 2.f;
            if (!(0.1f <= v)) v = 0.1f;
            g.timeWarp = v;
            T.queuedTimeWarp = 0.f;
        }
    }

    // old pruning of data behind the player, does nothing now
    void trigPrune(Game& g, float behindX) {
        (void)g;
        (void)behindX;
    }

}
