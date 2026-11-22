// Area triggers (move, rotate, scale, fade, tint areas) and their effects on the objects of a step
// (GJBaseGameLayer::processAreaActions).

#include "triggers_internal.hpp"

namespace ap::sim {
    std::atomic<long> g_areaMemoStat[5];

    std::atomic<long> g_areaSplitStat[4];

    // cells of the moved objects that touch the window, memoized per tick
    void trigAreaCellsList(Game const& g, std::vector<std::array<int, 5>>& out, int wx0, int wx1, int wy0, int wy1) {
        out.clear();
        auto inWindow = [&](std::array<int, 5> const& e) {
            return (e[3] >= wx0 && e[3] <= wx1 && e[4] >= wy0 && e[4] <= wy1) ||
                   (e[1] >= wx0 && e[1] <= wx1 && e[2] >= wy0 && e[2] <= wy1);
        };
        auto const& moved = g.trig.areaMoved;
        auto addOne = [&](int idx) {
            std::array<int, 5> e;
            e[0] = idx;
            trigAreaCells(g, idx, e[1], e[2], e[3], e[4]);
            if (inWindow(e)) out.push_back(e);
        };
        if ((g_noShare & 1) || moved.size() < 16 || !g.trig.h) {
            for (int idx : moved) addOne(idx);
            return;
        }
        PSet const* const playerSet = psetOf(g);
        std::vector<uint64_t> const* const ownBits = g.trig.areaYBits ? g.trig.areaYBits.get() : nullptr;
        auto isOwn = [&](int i) {
            return (playerSet && bitOf(playerSet->objBits, i)) ||
                   (ownBits && (((*ownBits)[static_cast<size_t>(i) >> 6] >> (i & 63)) & 1u));
        };

        struct M {
            int stamp = -1;
            WKey w;
            std::vector<std::pair<uint64_t, std::vector<std::array<int, 5>>>> e;
        };
        static thread_local M memo;
        int const now = tickStamp(g);
        if (memo.stamp != now || memo.w != g.world) {
            memo.stamp = now;
            memo.w = g.world;
            memo.e.clear();
        }
        uint64_t sharedKey =
                mix64(ownBits ? mix64(g.trig.areaVerS, 0x5157a11ull) : g.trig.areaVer, trigHeavyHash(g.trig.r()));
        sharedKey = mix64(sharedKey, g.trig.d ? trigDynHash(*g.trig.d) : 0u);
        size_t sharedCount = 0;
        uint64_t ownListHash = 0x0e1f5ull;
        for (int i : moved) {
            if (isOwn(i)) {
                ownListHash = mix64(ownListHash, static_cast<uint64_t>(static_cast<uint32_t>(i)));
                continue;
            }
            sharedKey = mix64(sharedKey, static_cast<uint64_t>(static_cast<uint32_t>(i)));
            ++sharedCount;
        }
        sharedKey = mix64(sharedKey, sharedCount);
        std::vector<std::array<int, 5>> const* sharedCells = nullptr;
        for (auto const& E : memo.e) {
            if (E.first == sharedKey) {
                sharedCells = &E.second;
                ++g_areaMemoStat[3];
                break;
            }
        }
        static thread_local std::vector<std::array<int, 5>> sharedScratch;
        if (!sharedCells) {
            sharedScratch.clear();
            for (int idx : moved) {
                if (!isOwn(idx)) {
                    std::array<int, 5> e;
                    e[0] = idx;
                    trigAreaCells(g, idx, e[1], e[2], e[3], e[4]);
                    sharedScratch.push_back(e);
                }
            }
            ++g_areaMemoStat[4];
            if (memo.e.size() < 32) {
                memo.e.emplace_back(sharedKey, sharedScratch);
                sharedCells = &memo.e.back().second;
            } else
                sharedCells = &sharedScratch;
        }
        bool const cellsCheck = debugOptions().memoCheck != 0;
        if (cellsCheck && sharedCells != &sharedScratch) {
            std::vector<std::array<int, 5>> reference;
            for (int idx : moved) {
                if (!isOwn(idx)) {
                    std::array<int, 5> e;
                    e[0] = idx;
                    trigAreaCells(g, idx, e[1], e[2], e[3], e[4]);
                    reference.push_back(e);
                }
            }
            if (reference != *sharedCells) {
                static thread_local int reportCount = 0;
                if (reportCount++ < 20) {
                    std::fprintf(stderr,
                                 "[cell memo] MISMATCH tick %d x %.2f y %.2f : %zu entries (memo) vs %zu ; split %d\n",
                                 g.tick, g.p1.x, g.p1.y, sharedCells->size(), reference.size(), ownBits ? 1 : 0);
                    int shown = 0;
                    for (size_t i = 0; i < std::min(reference.size(), sharedCells->size()) && shown < 5; ++i) {
                        if (reference[i] != (*sharedCells)[i]) {
                            ++shown;
                            auto const& a = (*sharedCells)[i];
                            auto const& b = reference[i];
                            std::fprintf(
                                    stderr,
                                    "    uid %d/%d PO %d : memo (%d,%d)->(%d,%d) computed (%d,%d)->(%d,%d) ; dyn %d\n",
                                    g.world->objs[static_cast<size_t>(a[0])].uid,
                                    g.world->objs[static_cast<size_t>(b[0])].uid, isPO(g, b[0]) ? 1 : 0, a[1], a[2],
                                    a[3], a[4], b[1], b[2], b[3], b[4],
                                    g.world->mayHaveDyn[static_cast<size_t>(b[0])] ? 1 : 0);
                        }
                    }
                }
                sharedScratch = reference;
                sharedCells = &sharedScratch;
            }
        }
        for (auto const& e : *sharedCells)
            if (inWindow(e)) out.push_back(e);
        if (sharedCount == moved.size()) return;

        size_t const ownCount = moved.size() - sharedCount;
        if (ownCount < 16) {
            for (int idx : moved)
                if (isOwn(idx)) addOne(idx);
            return;
        }
        uint64_t ownKey =
                mix64(mix64(ownListHash, ownCount), ownBits ? mix64(g.trig.areaVerY, 0x7a11ull) : g.trig.areaVer);
        ownKey = mix64(ownKey, trigHeavyHash(g.trig.r()));
        bool anyPlayerObj = false;
        if (g.trig.d) {
            auto const& dynItems = g.trig.rd().objDyn.items;
            auto dynIt = dynItems.begin();
            for (int idx : moved) {
                if (!isOwn(idx)) continue;
                if (playerSet && bitOf(playerSet->objBits, idx)) {
                    anyPlayerObj = true;
                    continue;
                }
                while (dynIt != dynItems.end() && dynIt->first < idx) ++dynIt;
                if (dynIt != dynItems.end() && dynIt->first == idx) {
                    ownKey = hashPod(mix64(ownKey, static_cast<uint64_t>(static_cast<uint32_t>(idx))), dynIt->second);
                }
            }
        } else if (playerSet) {
            for (int idx : moved) {
                if (isOwn(idx) && bitOf(playerSet->objBits, idx)) {
                    anyPlayerObj = true;
                    break;
                }
            }
        }
        ownKey = mix64(ownKey, (anyPlayerObj && g.trig.pv) ? pvHash(*g.trig.pv) : 0x9d0ull);
        if (ownBits && g.trig.areaVerY == 0) {
            for (int idx : moved)
                if (isOwn(idx)) addOne(idx);
            return;
        }
        struct MY {
            int stamp = -1;
            WKey w;
            std::vector<std::pair<uint64_t, std::vector<std::array<int, 5>>>> e;
        };
        static thread_local MY ownMemo;
        if (ownMemo.stamp != now || ownMemo.w != g.world) {
            ownMemo.stamp = now;
            ownMemo.w = g.world;
            ownMemo.e.clear();
        }
        std::vector<std::array<int, 5>> const* ownCells = nullptr;
        for (auto const& E : ownMemo.e) {
            if (E.first == ownKey) {
                ownCells = &E.second;
                break;
            }
        }
        static thread_local std::vector<std::array<int, 5>> ownScratch;
        if (!ownCells) {
            ownScratch.clear();
            for (int idx : moved) {
                if (isOwn(idx)) {
                    std::array<int, 5> e;
                    e[0] = idx;
                    trigAreaCells(g, idx, e[1], e[2], e[3], e[4]);
                    ownScratch.push_back(e);
                }
            }
            if (ownMemo.e.size() < 32) {
                ownMemo.e.emplace_back(ownKey, ownScratch);
                ownCells = &ownMemo.e.back().second;
            } else
                ownCells = &ownScratch;
        }
        if (cellsCheck && ownCells != &ownScratch) {
            std::vector<std::array<int, 5>> reference;
            for (int idx : moved) {
                if (isOwn(idx)) {
                    std::array<int, 5> e;
                    e[0] = idx;
                    trigAreaCells(g, idx, e[1], e[2], e[3], e[4]);
                    reference.push_back(e);
                }
            }
            if (reference != *ownCells) {
                static thread_local int ownReportCount = 0;
                if (ownReportCount++ < 20) {
                    std::fprintf(stderr,
                                 "[cell memo] MISMATCH (own part) tick %d x %.2f y %.2f : %zu entries (memo) vs %zu\n",
                                 g.tick, g.p1.x, g.p1.y, ownCells->size(), reference.size());
                }
                ownScratch = reference;
                ownCells = &ownScratch;
            }
        }
        for (auto const& e : *ownCells)
            if (inWindow(e)) out.push_back(e);
    }

    // runs the area triggers, then keeps the outer box flag of rotated objects
    void trigAreaActions(Game& g, double dtSec) {
        trigAreaActionsImpl(g, dtSec);
        if (g.world->hasAreaEffects && !g.trig.areaOffEmpty()) {
            int const now = tickStamp(g);
            World const& w = *g.world;
            struct SC {
                std::shared_ptr<TrigState::AreaOffMap const> map;
                int stamp = -1;
                WKey w;
                std::vector<int> cand;
            };
            static thread_local SC sc;
            if (sc.map != g.trig.areaOffP || sc.stamp != now || sc.w != g.world) {
                sc.map = g.trig.areaOffP;
                sc.stamp = now;
                sc.w = g.world;
                sc.cand.clear();
                for (auto const& e : g.trig.areaOff().items) {
                    AreaOff const& off = e.second;
                    if (off.stamp != now || off.rot == 0.f) continue;
                    int const idx = e.first;
                    if (!w.mayHaveDyn[static_cast<size_t>(idx)] || objAt(g, idx).type == Decoration) continue;
                    sc.cand.push_back(idx);
                }
            }
            static thread_local std::vector<int> stick;
            stick.clear();
            for (int idx : sc.cand) {
                ObjDyn const* dyn = dynOf(g, idx);
                if (!dyn || !dyn->outerOb) stick.push_back(idx);
            }
            for (int idx : stick) dynEntry(g, idx).outerOb = true;
        }
        if (psetOf(g)) trigAddPMoved(g);
    }

    //adds player objects near the player to areaMoved, so collisions test them at their real position
    void trigAddPMoved(Game& g) {
        PSet const* playerSet = psetOf(g);
        if (!playerSet || playerSet->collObjs.empty()) return;
        TrigState& T = g.trig;
        int const p1Cell = World::cellOfF(g.p1.x);
        int const p2Cell = g.isDualMode ? World::cellOfF(g.p2.x) : p1Cell;
        int const lo = std::min(p1Cell, p2Cell) - 2;
        int const hi = std::max(p1Cell, p2Cell) + 2;
        size_t const startSize = T.areaMoved.size();
        struct Canon {
            uint64_t ver = 0;
            PSet const* ps = nullptr;
            uint64_t sig = 0;
            WKey w;
            std::vector<double> x;
            std::vector<int> cx;
            std::vector<int> ord;
            std::vector<double> xs;
            std::vector<uint32_t> mark;
            uint32_t stamp = 0;
        };
        static thread_local Canon cn;
        TrigMotion const& motion = T.r().M();
        size_t const n = playerSet->collObjs.size();
        if (motion.ver == 0 || cn.ver != motion.ver || cn.ps != playerSet || cn.sig != playerSet->sig ||
            cn.w != g.world) {
            bool const sameSet =
                    cn.ps == playerSet && cn.sig == playerSet->sig && cn.w == g.world && cn.ord.size() == n;
            cn.ver = motion.ver;
            cn.ps = playerSet;
            cn.sig = playerSet->sig;
            cn.w = g.world;
            cn.x.resize(n);
            cn.cx.resize(n);
            bool const prevCanonRead = tl_canonRead;
            tl_canonRead = true;
            for (size_t k = 0; k < n; ++k) {
                double bx, by;
                groupPos(g, objAt(g, playerSet->collObjs[k]), bx, by);
                cn.x[k] = bx;
                cn.cx[k] = World::cellOf(bx);
            }
            tl_canonRead = prevCanonRead;
            auto lessK = [&](int a, int b) {
                return cn.x[static_cast<size_t>(a)] < cn.x[static_cast<size_t>(b)] ||
                       (cn.x[static_cast<size_t>(a)] == cn.x[static_cast<size_t>(b)] && a < b);
            };
            if (!sameSet) {
                cn.ord.resize(n);
                for (size_t k = 0; k < n; ++k) cn.ord[k] = static_cast<int>(k);
                std::sort(cn.ord.begin(), cn.ord.end(), lessK);
            } else {
                size_t moves = 0;
                for (size_t i = 1; i < n && moves <= 4 * n; ++i) {
                    int const v = cn.ord[i];
                    size_t j = i;
                    while (j > 0 && lessK(v, cn.ord[j - 1])) {
                        cn.ord[j] = cn.ord[j - 1];
                        --j;
                        ++moves;
                    }
                    cn.ord[j] = v;
                }
                if (moves > 4 * n) std::sort(cn.ord.begin(), cn.ord.end(), lessK);
            }
            cn.xs.resize(n);
            for (size_t i = 0; i < n; ++i) cn.xs[i] = cn.x[static_cast<size_t>(cn.ord[i])];
            if (motion.ver == 0) cn.ps = nullptr;
        }
        if (cn.mark.size() != n) {
            cn.mark.assign(n, 0u);
            cn.stamp = 0;
        }
        if (++cn.stamp == 0) {
            std::fill(cn.mark.begin(), cn.mark.end(), 0u);
            cn.stamp = 1;
        }
        double maxOffsetX = 0.0;
        if (TrigPv const* playerPart = g.trig.pv.get()) {
            for (auto const& e : playerPart->off.items) {
                GroupOff const* canonOff = motion.groupOff.find(e.first);
                maxOffsetX += std::fabs(e.second.x - (canonOff ? canonOff->x : 0.0));
            }
        }
        TrigPv const* playerPart = g.trig.pv.get();
        auto addIfNear = [&](size_t k) {
            int const idx = playerSet->collObjs[k];
            int const canonCell = cn.cx[k];
            if (!(canonCell < lo || canonCell > hi)) {
                T.areaMoved.push_back(idx);
                return;
            }
            ObjDyn const* dyn = playerPart ? playerPart->dyn.find(idx) : nullptr;
            if (!dyn) {
                double areaDx = 0.0;
                if (auto* areaOff = trigAreaOff(g, idx)) areaDx = static_cast<double>(areaOff->dx);
                double const bx = cn.x[k] + areaDx;
                if (World::cellOf(bx + maxOffsetX + 1.0) < lo || World::cellOf(bx - maxOffsetX - 1.0) > hi) return;
            } else if (isPO(g, idx) && !std::binary_search(playerPart->afObjs.begin(), playerPart->afObjs.end(), idx)) {
                int const curCell = World::cellOf(dyn->secX);
                if (curCell < lo || curCell > hi) return;
                T.areaMoved.push_back(idx);
                return;
            }
            int storedX, storedY, curX, curY;
            trigAreaCells(g, idx, storedX, storedY, curX, curY);
            if ((storedX < lo || storedX > hi) && (curX < lo || curX > hi)) return;
            T.areaMoved.push_back(idx);
        };
        auto addOnce = [&](int idx) {
            auto it = std::lower_bound(playerSet->collObjs.begin(), playerSet->collObjs.end(), idx);
            if (it == playerSet->collObjs.end() || *it != idx) return;
            size_t const k = static_cast<size_t>(it - playerSet->collObjs.begin());
            if (cn.mark[k] == cn.stamp) return;
            cn.mark[k] = cn.stamp;
            addIfNear(k);
        };
        if (((playerPart ? playerPart->dyn.size() : 0) + T.areaOff().items.size()) * 4 >= n) {
            for (size_t k = 0; k < n; ++k) addIfNear(k);
            sortAppended(T.areaMoved, startSize);
            return;
        }
        if (playerPart)
            for (auto const& e : playerPart->dyn.items) addOnce(e.first);
        for (auto const& e : T.areaOff().items)
            if (trigAreaOff(g, e.first)) addOnce(e.first);
        auto const first = std::partition_point(cn.xs.begin(), cn.xs.end(),
                                                [&](double x) { return World::cellOf(x + maxOffsetX + 1.0) < lo; });
        auto const last = std::partition_point(first, cn.xs.end(),
                                               [&](double x) { return !(World::cellOf(x - maxOffsetX - 1.0) > hi); });
        for (auto i = first; i != last; ++i) {
            size_t const k = static_cast<size_t>(cn.ord[static_cast<size_t>(i - cn.xs.begin())]);
            if (cn.mark[k] == cn.stamp) continue;
            addIfNear(k);
        }
        sortAppended(T.areaMoved, startSize);
    }

    namespace trigger_impl {
        // area triggers of one tick (move, rotate, scale), same as processAreaEffects
        namespace {
            struct AreaMemo {
                struct E {
                    uint64_t ver = 0, outVer = 0, hH = 0, pvH = 0, dH = 0;
                    double dt = 0;
                    float x = 0, y = 0, x2 = 0, y2 = 0, camX = 0, camY = 0, zoom = 0;
                    bool dual = false;
                    std::shared_ptr<TrigState::AreaOffMap const> off;
                    std::vector<int> moved;
                };
                int stamp = -1;
                WKey w;
                std::vector<E> e;
            };

            struct WorkScope {
                AreaWork& A;

                ~WorkScope() { A.owner = nullptr; }
            };

            // The area triggers of one step (GJBaseGameLayer::processAreaActions), done in stages with a memo shared by
            // the search threads; the state that several stages share is members.
            struct AreaStep {
                AreaStep(Game& g, double dtSec) : g(g), dtSec(dtSec) {}

                // arguments of trigAreaActionsImpl
                Game& g;
                double dtSec;

                PosCacheScope posScope;
                World const* w = nullptr;
                TrigState* T = nullptr;
                TrigHeavy const* heavyIn = nullptr;
                bool anyInst;
                float dt;
                int now;
                PSet const* playerSet;
                bool inert(AreaInst const& inst);
                int depOf(int k, AreaInst const& inst);
                bool needY;
                bool needCam;
                bool needCamX;
                bool needP2;
                bool needPv;
                static inline thread_local AreaMemo amo;
                static inline thread_local int amoCalls = 0;
                static inline thread_local int amoHits = 0;
                static inline thread_local int amoOff = 0;
                bool memoOn;
                float keyY;
                float keyX2;
                float keyY2;
                float keyCX;
                float keyCY;
                float keyZ;
                bool keyDual;
                void noSplit();
                uint64_t hH;
                uint64_t pvH;
                uint64_t dH;
                std::shared_ptr<TrigState::AreaOffMap const> memoCheckPending;
                bool memoCheckHit;
                uint64_t inVer;
                TrigHeavy const* H = nullptr;
                Player const* p1 = nullptr;
                AreaWork* work = nullptr;
                template <class T1>
                auto loadWork(T1&& keep);
                std::optional<WorkScope> workScope;
                static inline thread_local bool warnedScreen = false;
                static constexpr auto allObjs = [](int) { return true; };
                template <class T1, class T2>
                auto kernel2(T1&& selectInst, T2&& selectObj);
                template <class T1>
                auto kernel(T1&& selectInst);
                std::shared_ptr<TrigState::AreaOffMap> finishWork();
                static constexpr uint64_t kEmptyVer = ~0ull;
                template <class T1, class T2>
                uint64_t canonVer(T1 const& items, T2&& keep);
                static constexpr auto all = [](int) { return true; };
                void finishState();
                std::shared_ptr<TrigState::AreaOffMap const> prevAreaOff;
                const std::vector<std::pair<int, AreaOff>>* items = nullptr;
                // nothing to do without area triggers or active areas: only the moves of the advanced follows are kept
                Flow skipWithoutAreas();
                // steps the value animations of the areas (memoized per step for the states with the same areas)
                void stepAreaAnimations();
                // sorts the areas by priority and finds what their effects depend on: player y, camera, second player
                void findDependencies();
                // looks the step up in the area memo, keyed by the trigger state and what the effects depend on
                Flow lookUpAreaMemo();
                // per-call setup of the work buffer the effects write into
                void prepareWork();
                // when the effects depend on the player or the camera, applies the parts that do not and the parts that
                // do apart, so the first part can be shared
                Flow applySplit();
                // applies all the areas in one pass
                void applyAll();
                // debug (APSIM_MEMOCHECK): compares a memo hit with the computed result
                void checkMemoHit();
                // gives the result a version number (the same content gets the same number) and stores it in the memo
                void storeAreaResult();

                // the whole of trigAreaActionsImpl, one stage after another
                void run();
            };
        }

        auto AreaStep::inert(AreaInst const& inst) -> bool {
            auto const& def = w->areaDefs[static_cast<size_t>(inst.def)];
            if (!physOfGroup(*w, inst.target).empty()) return false;
            World::TargetGroups const* targetGroups =
                    w->targetGroupsOf(inst.target, def.ignoreGroupParent, def.ignoreLinked);
            return !targetGroups || targetGroups->reps.empty();
        }

        auto AreaStep::depOf(int k, AreaInst const& inst) -> int {
            auto const& def = w->areaDefs[static_cast<size_t>(inst.def)];
            int const center = def.special != 0 ? def.special : inst.center;
            int deps = 0;
            if (inert(inst)) return 0;
            if (playerSet) {
                if (inst.target > 0 && bitOf(playerSet->memberGroupBits, static_cast<short>(inst.target)))
                    deps |= 4;
                else if (center >= 0) {
                    int const centerIdx = w->mainObjectOf(center);
                    if (centerIdx >= 0 && isPO(g, centerIdx)) deps |= 16;
                }
            }
            if (center == -1 || center == -2) {
                if (def.dirType != 1 || (k == 0 && def.relative)) deps |= 1;
                if (center == -2) deps |= 8;
            } else if (center < -2) {
                deps |= (def.dirType == 1 && !(k == 0 && def.relative)) ? 32 : 2;
            }
            return deps;
        }

        auto AreaStep::noSplit() -> void {
            T->areaVerS = 0;
            T->areaVerY = 0;
            T->areaSplitSig = 0;
            T->areaYBits.reset();
        }

        template <class T1>
        auto AreaStep::loadWork(T1&& keep) {
            work->begin(w->objs.size());
            auto const& prev = T->areaOff().items;
            work->v.reserve(prev.size() + 64);
            for (auto const& e : prev) {
                if (!keep(e.first)) continue;
                work->gen[static_cast<size_t>(e.first)] = work->cur;
                work->slot[static_cast<size_t>(e.first)] = static_cast<int>(work->v.size());
                work->v.push_back(e);
                AreaOff& off = work->v.back().second;
                off.pdx = off.dx;
                off.pdy = off.dy;
                off.dx = 0.f;
                off.dy = 0.f;
                off.rot = 0.f;
                off.psclOn = off.sclOn;
                off.psx = off.sx;
                off.psy = off.sy;
                off.sclOn = 0;
            }
            work->nSorted = work->v.size();
        }

        template <class T1, class T2>
        auto AreaStep::kernel2(T1&& selectInst, T2&& selectObj) {
            for (int kind : {2, 1, 0}) {
                for (auto const& inst : H->areas[kind]) {
                    if (inst.paused) continue;
                    if (!selectInst(kind, inst)) continue;
                    auto const& def = w->areaDefs[static_cast<size_t>(inst.def)];

                    int center = def.special != 0 ? def.special : inst.center;
                    float centerX = 0.f;
                    float centerY = 0.f;
                    if (center >= 0) {
                        int const centerIdx = w->mainObjectOf(center);
                        if (centerIdx >= 0) {
                            double x, y;
                            realPosBase(g, centerIdx, x, y);
                            centerX = static_cast<float>(x);
                            centerY = static_cast<float>(y);
                        }
                    } else if (center == -1 || (center == -2 && !g.isDualMode)) {
                        centerX = p1->x;
                        centerY = p1->y;
                    } else if (center == -2) {
                        centerX = g.p2.x;
                        centerY = g.p2.y;
                    } else {
                        if (!warnedScreen) {
                            warnedScreen = true;
                        }
                        float const screenW = w->winW > 0.f ? w->winW : 568.8889f;
                        float const screenH = w->winH > 0.f ? w->winH : 320.f;
                        float const viewW = screenW / g.cameraZoom;
                        float const viewH = screenH / g.cameraZoom;
                        float const camX = g.cam.on ? g.cam.px : p1->x - 75.f / g.cameraZoom - viewW * 0.25f;
                        float const camY = g.cam.on ? g.cam.py : p1->y - viewH * 0.5f;
                        float anchorX = viewW * 0.5f;
                        float anchorY = viewH * 0.5f;
                        switch (center) {
                        case -4:
                            anchorX = 0.f;
                            anchorY = 0.f;
                            break;
                        case -5:
                            anchorX = 0.f;
                            break;
                        case -6:
                            anchorX = 0.f;
                            anchorY = viewH;
                            break;
                        case -7:
                            anchorY = 0.f;
                            break;
                        case -8:
                            anchorY = viewH;
                            break;
                        case -9:
                            anchorX = viewW;
                            anchorY = 0.f;
                            break;
                        case -10:
                            anchorX = viewW;
                            break;
                        case -11:
                            anchorX = viewW;
                            anchorY = viewH;
                            break;
                        default:
                            break;
                        }
                        centerX = camX + anchorX;
                        centerY = camY + anchorY;
                    }

                    float v[28];
                    for (int i = 0; i < 28; ++i) v[i] = inst.v[i];
                    if (def.dirType != 0) {
                        v[25] = v[2];
                        v[26] = v[3];
                    }
                    float const range = static_cast<float>(static_cast<int>(v[1] + v[0] + v[2] + v[3]));
                    float const minMod = std::fabs(v[5]) <= std::fabs(v[4]) ? v[5] : v[4];
                    float const reach = (1.f / std::fabs(minMod)) * range;
                    int cellX0 = World::cellOfF(centerX - reach);
                    int cellX1 = World::cellOfF(centerX + reach);
                    int cellY0 = w->cellYOfF(centerY - reach);
                    int cellY1 = w->cellYOfF(centerY + reach);
                    int const maxCellX = World::cellOfF(1e7f);
                    int const maxCellY = w->cellYOfF(1e7f);
                    if (def.dirType == 1) {
                        cellY0 = 0;
                        cellY1 = maxCellY;
                        if (def.inbound) cellX1 = maxCellX;
                        if (def.inbound == def.twoDirections) cellX0 = 0;
                    } else if (def.dirType == 2) {
                        cellX0 = 0;
                        cellX1 = maxCellX;
                        if (def.inbound) cellY1 = maxCellY;
                        if (def.inbound == def.twoDirections) cellY0 = 0;
                    } else if (def.inbound) {
                        cellX0 = 0;
                        cellX1 = maxCellX;
                        cellY0 = 0;
                        cellY1 = maxCellY;
                    }
                    centerX += v[2];
                    centerY += v[25];
                    if (def.twoDirections) v[4] = -std::fabs(v[4]);

                    static thread_local std::vector<int> cand;
                    cand.clear();
                    {
                        GroupSpatial const& spatial = spatialOfGroup(*w, inst.target);
                        cand.insert(cand.end(), spatial.mobile.begin(), spatial.mobile.end());
                        auto const& fixedX = spatial.fixedX;
                        bool groupMoved = false;
                        for (short group : spatial.groups) {
                            if (H->M().groupOff.find(group)) {
                                groupMoved = true;
                                break;
                            }
                        }
                        if (groupMoved || cellX1 >= maxCellX) {
                            for (auto const& e : fixedX) cand.push_back(e.second);
                        } else {
                            float lo1 = p1->x - 900.f;
                            float hi1 = p1->x + 900.f;
                            float lo2 = static_cast<float>(cellX0) * 100.f;
                            float hi2 = static_cast<float>(cellX1 + 1) * 100.f;
                            if (lo2 < lo1) {
                                std::swap(lo1, lo2);
                                std::swap(hi1, hi2);
                            }
                            auto addRange = [&](float lo, float hi) {
                                auto first =
                                        std::lower_bound(fixedX.begin(), fixedX.end(), std::make_pair(lo - 1.f, -1));
                                for (auto i = first; i != fixedX.end() && i->first <= hi + 1.f; ++i) {
                                    cand.push_back(i->second);
                                }
                            };
                            if (lo2 <= hi1)
                                addRange(lo1, std::max(hi1, hi2));
                            else {
                                addRange(lo1, hi1);
                                addRange(lo2, hi2);
                            }
                        }
                    }
                    World::TargetGroups const* const targetGroups =
                            w->targetGroupsOf(inst.target, def.ignoreGroupParent, def.ignoreLinked);
                    auto childUsed = [&](World::TargetGroups::Rep const& rep, int child) {
                        return !(def.dontEditParent && child == rep.obj);
                    };
                    auto curPos = [&](int obj, float& px, float& py) {
                        double baseX, baseY;
                        realPosBase(g, obj, baseX, baseY);
                        px = static_cast<float>(baseX);
                        py = static_cast<float>(baseY);
                        if (AreaOff const* off = work->find(obj)) {
                            if (off->stamp == now) {
                                px = static_cast<float>(static_cast<double>(px) + static_cast<double>(off->dx));
                                py = static_cast<float>(static_cast<double>(py) + static_cast<double>(off->dy));
                            }
                        }
                    };
                    auto addOff = [&](int obj, float dx, float dy) {
                        if (dx == 0.f && dy == 0.f) return;
                        AreaOff& off = work->get(obj);
                        if (off.stamp != now) {
                            off.dx = 0.f;
                            off.dy = 0.f;
                            off.rot = 0.f;
                            off.sclOn = 0;
                        }
                        if (!objAt(g, obj).lockX) off.dx += dx;
                        off.dy += dy;
                        off.stamp = now;
                    };
                    auto areaMoveChildren = [&](World::TargetGroups::Rep const& rep, float dx, float dy) {
                        for (int child : rep.phys)
                            if (childUsed(rep, child)) addOff(child, dx, dy);
                    };
                    auto areaRotateChildren = [&](World::TargetGroups::Rep const& rep, float r) {
                        if (rep.center < 0) return;
                        float pivotX, pivotY;
                        curPos(rep.center, pivotX, pivotY);
                        float const rad = -r * 0.01745329252f;
                        float const cosA = r != 0.f ? gcos(rad) : 1.f;
                        float const sinA = r != 0.f ? gsin(rad) : 0.f;
                        float const Aa = cosA;
                        float const Ab = sinA;
                        float const Ac = -sinA;
                        float const Ad = cosA;
                        for (int child : rep.phys) {
                            if (!childUsed(rep, child)) continue;
                            float px, py;
                            curPos(child, px, py);
                            float const localX = px - pivotX;
                            float const localY = py - pivotY;
                            float const tx = localX * Aa + localY * Ac + pivotX;
                            float const ty = localX * Ab + localY * Ad + pivotY;
                            addOff(child, tx - px, ty - py);
                            if (r != 0.f && objAt(g, child).canRotateFree()) {
                                AreaOff& off = work->get(child);
                                if (off.stamp != now) {
                                    off.dx = 0.f;
                                    off.dy = 0.f;
                                    off.rot = 0.f;
                                    off.sclOn = 0;
                                }
                                off.rot += r;
                                off.stamp = now;
                            }
                        }
                    };
                    auto areaScaleChildren = [&](World::TargetGroups::Rep const& rep, float factorX, float factorY) {
                        if (rep.center < 0) return;
                        float pivotX, pivotY;
                        curPos(rep.center, pivotX, pivotY);
                        float const Aa = 1.f * factorX;
                        float const Ab = 0.f * factorX;
                        float const Ac = -0.f * factorY;
                        float const Ad = 1.f * factorY;
                        for (int child : rep.phys) {
                            if (!childUsed(rep, child)) continue;
                            Obj const& childObj = objAt(g, child);
                            float px, py;
                            curPos(child, px, py);
                            float const localX = px - pivotX;
                            float const localY = py - pivotY;
                            AreaOff const* prevOff = work->find(child);
                            if (prevOff && prevOff->stamp != now) prevOff = nullptr;
                            float sclX, sclY;
                            trigScaleAbs(g, child, sclX, sclY);
                            float rotAdd = 0.f;
                            if (auto* dyn = dynOf(g, child)) rotAdd = dyn->rotAcc;
                            float const rotX = childObj.rotX + rotAdd + (prevOff ? prevOff->rot : 0.f);
                            float const rotY = childObj.rotY + rotAdd + (prevOff ? prevOff->rot : 0.f);
                            float cxB = 1.f;
                            float sxB = 0.f;
                            float cyB = 1.f;
                            float syB = 0.f;
                            if (rotX != 0.f || rotY != 0.f) {
                                float const rx = -rotX * 0.01745329252f;
                                float const ry = -rotY * 0.01745329252f;
                                cxB = gcos(rx);
                                sxB = gsin(rx);
                                cyB = gcos(ry);
                                syB = gsin(ry);
                            }
                            float const Ba = cyB * sclX;
                            float const Bb = syB * sclX;
                            float const Bc = -sxB * sclY;
                            float const Bd = cxB * sclY;
                            float const Ta = Ba * Aa + Bb * Ac;
                            float const Tb = Ba * Ab + Bb * Ad;
                            float const Tc = Bc * Aa + Bd * Ac;
                            float const Td = Bc * Ab + Bd * Ad;
                            float const tx = localX * Aa + localY * Ac + pivotX;
                            float const ty = localX * Ab + localY * Ad + pivotY;
                            addOff(child, tx - px, ty - py);
                            float const newScaleX = std::sqrt(Ta * Ta + Tb * Tb);
                            float const newScaleY = std::sqrt(Td * Td + Tc * Tc);
                            float const rotDeltaX = -(std::atan2(Td, Tc) * 57.29578f - 90.f) - rotX;
                            float const rotDeltaY = -(std::atan2(Tb, Ta) * 57.29578f) - rotY;
                            bool const rotChanged = rotDeltaX != 0.f || rotDeltaY != 0.f;
                            bool const scaleChanged = (newScaleX - sclX) != 0.f || (newScaleY - sclY) != 0.f;
                            if (!rotChanged && !scaleChanged) continue;
                            AreaOff& off = work->get(child);
                            if (off.stamp != now) {
                                off.dx = 0.f;
                                off.dy = 0.f;
                                off.rot = 0.f;
                                off.sclOn = 0;
                            }
                            if (rotChanged) off.rot += rotDeltaX;
                            if (scaleChanged) {
                                off.sx = sclX + (newScaleX - sclX);
                                off.sy = sclY + (newScaleY - sclY);
                                off.sclOn = 1;
                            }
                            off.stamp = now;
                        }
                    };

                    auto effect = [&](int idx, World::TargetGroups::Rep const* rep) {
                        Obj const& o = objAt(g, idx);
                        if (o.cls == 1 && o.type == Modifier && !o.touchTriggered) return;
                        double baseXd, baseYd;
                        realPosBase(g, idx, baseXd, baseYd);
                        float const bx = static_cast<float>(baseXd);
                        float const by = static_cast<float>(baseYd);
                        bool const nearPlayer = std::fabs(bx - p1->x) < 900.f;
                        int const cellX = World::cellOfF(bx);
                        int const cellY = w->cellYOfF(by);
                        bool const inRange = cellX0 <= cellX && cellX <= cellX1 && cellY0 <= cellY && cellY <= cellY1;
                        if (!inRange && !nearPlayer) return;
                        auto randomOf = [&](int k) {
                            return (o.areaRi >= 0 && static_cast<size_t>(o.areaRi + k) < w->areaRand.size())
                                           ? w->areaRand[static_cast<size_t>(o.areaRi + k)]
                                           : 0.f;
                        };
                        float dist;
                        bool show;
                        if (def.dirType == 1) {
                            float const randOffset = v[3] != 0.f ? v[3] * randomOf(1) : 0.f;
                            dist = (bx - centerX) + randOffset;
                            dist *= (0.f <= dist) ? v[5] : v[4];
                            show = !(bx < centerX);
                        } else if (def.dirType == 2) {
                            float const randOffset = v[3] != 0.f ? v[3] * randomOf(1) : 0.f;
                            dist = (by - centerY) + randOffset;
                            dist *= (0.f <= dist) ? v[5] : v[4];
                            show = !(by < centerY);
                        } else {
                            float const randOffY = v[26] != 0.f ? v[26] * randomOf(2) : 0.f;
                            float const randOffX = v[3] != 0.f ? v[3] * randomOf(1) : 0.f;
                            float const relX = bx - (centerX + randOffX);
                            float const relY = by - (centerY + randOffY);
                            dist = std::sqrt(relX * relX + relY * relY);
                            show = !(by < centerY);
                        }
                        float const lenVar = v[1] != 0.f ? v[1] * randomOf(0) : 0.f;
                        float value = areaValueFromDist(dist, static_cast<int>(lenVar + v[0]), v[6]);
                        if (def.inbound) value = 1.f - value;
                        if (!(value < 1.f)) return;
                        AreaOff* slot = nullptr;
                        auto offsetEntry = [&]() -> AreaOff& {
                            if (!slot) {
                                slot = &work->get(idx);
                            }
                            return *slot;
                        };
                        auto eased = [&](float val) {
                            int type = def.easeInType;
                            float rate = def.easeInRate;
                            if (def.easeOut) {
                                bool showNow = show;
                                if (0.01f < val && val < 0.99f) {
                                    AreaOff& off = offsetEntry();
                                    if (off.latchStamp == now - 1 || off.latchStamp == now) showNow = off.latch != 0;
                                    off.latch = showNow ? 1 : 0;
                                    off.latchStamp = now;
                                }
                                if (!showNow) {
                                    type = def.easeOutType;
                                    rate = def.easeOutRate;
                                }
                            }
                            return areaEase(val, type, rate);
                        };
                        if (kind == 0) {
                            float dx = 0.f;
                            float dy = 0.f;
                            if (!def.xyMode) {
                                float moveDist = v[8] != 0.f ? v[8] * randomOf(10) : 0.f;
                                moveDist += v[7];
                                if (moveDist == 0.f) return;
                                float const easedVal = eased(value);
                                float dirX, dirY;
                                if (!def.relative) {
                                    float const angle = ((v[9] + randomOf(11) * v[10]) - 90.f) * 0.017453292f;
                                    dirX = gcos(angle);
                                    dirY = gsin(angle);
                                } else {
                                    float const relX = bx - centerX;
                                    float const relY = by - centerY;
                                    float const len = std::sqrt(relX * relX + relY * relY);
                                    float const fade = v[27] < 0.f ? 0.f : v[27];
                                    if (len < fade) moveDist = moveDist * len * (1.f / fade);
                                    if (0.f < len) {
                                        dirX = relX / len;
                                        dirY = relY / len;
                                    } else {
                                        dirX = 0.f;
                                        dirY = 0.f;
                                    }
                                }
                                float const amount = (1.f - easedVal) * moveDist;
                                dx = amount * dirX;
                                dy = amount * dirY;
                            } else {
                                float const moveX = randomOf(8) * v[12] + v[11];
                                float const moveY = randomOf(9) * v[14] + v[13];
                                if (moveX == 0.f && moveY == 0.f) return;
                                float const amount = 1.f - eased(value);
                                dx = amount * moveX;
                                dy = amount * moveY;
                            }
                            if (rep) {
                                areaMoveChildren(*rep, dx, dy);
                                return;
                            }
                            if (dx == 0.f && dy == 0.f) return;
                            AreaOff& off = offsetEntry();
                            if (!o.lockX) off.dx += dx;
                            off.dy += dy;
                            off.stamp = now;
                        } else if (kind == 1) {
                            float const rotValue = randomOf(18) * v[20] + v[19];
                            float rot = 0.f;
                            if (rotValue != 0.f) rot = (1.f - eased(value)) * rotValue;
                            if (rep) {
                                areaRotateChildren(*rep, rot);
                                return;
                            }
                            if (rot == 0.f) return;
                            AreaOff& off = offsetEntry();
                            off.rot += rot;
                            off.stamp = now;
                        } else {
                            float const scaleValX = randomOf(12) * v[16] + v[15];
                            float const scaleValY = randomOf(13) * v[18] + v[17];
                            float factorX = 1.f;
                            float factorY = 1.f;
                            if (scaleValX != 0.f || scaleValY != 0.f) {
                                float const easedVal = eased(value);
                                factorX = (scaleValX - 1.f) * (1.f - easedVal) + 1.f;
                                factorY = (scaleValY - 1.f) * (1.f - easedVal) + 1.f;
                            }
                            if (rep) {
                                areaScaleChildren(*rep, factorX, factorY);
                                return;
                            }
                            if (factorX == 1.f && factorY == 1.f) return;
                            AreaOff& off = offsetEntry();
                            float curScaleX, curScaleY;
                            if (off.stamp == now && off.sclOn) {
                                curScaleX = off.sx;
                                curScaleY = off.sy;
                            } else {
                                ObjDyn const* dyn = dynOf(g, idx);
                                curScaleX = dyn && dyn->scaled ? dyn->sclX : o.scaleX;
                                curScaleY = dyn && dyn->scaled ? dyn->sclY : o.scaleY;
                            }
                            float const addX = (factorX - 1.f) * curScaleX;
                            float const addY = (factorY - 1.f) * curScaleY;
                            if (addX != 0.f || addY != 0.f) {
                                off.sx = curScaleX + addX;
                                off.sy = curScaleY + addY;
                                off.sclOn = 1;
                            }
                            off.stamp = now;
                        }
                    };
                    for (int idx : cand) {
                        if (!selectObj(idx)) continue;
                        if (targetGroups && !std::binary_search(targetGroups->looseSorted.begin(),
                                                                targetGroups->looseSorted.end(), idx))
                            continue;
                        effect(idx, nullptr);
                    }
                    if (targetGroups) {
                        for (auto const& rep : targetGroups->reps) {
                            if (selectObj(rep.obj)) effect(rep.obj, &rep);
                        }
                    }
                }
            }
        }

        template <class T1>
        auto AreaStep::kernel(T1&& selectInst) {
            kernel2(selectInst, allObjs);
        }

        auto AreaStep::finishWork() -> std::shared_ptr<TrigState::AreaOffMap> {
            work->owner = nullptr;
            auto& entries = work->v;
            if (entries.size() > work->nSorted) {
                auto const byKey = [](auto const& x, auto const& y) { return x.first < y.first; };
                std::sort(entries.begin() + static_cast<long>(work->nSorted), entries.end(), byKey);
                std::inplace_merge(entries.begin(), entries.begin() + static_cast<long>(work->nSorted), entries.end(),
                                   byKey);
            }
            std::shared_ptr<TrigState::AreaOffMap> map;
            for (auto const& e : entries) {
                AreaOff const& off = e.second;
                bool const active = off.dx != 0.f || off.dy != 0.f || off.rot != 0.f || off.sclOn;
                if (!active && off.pdx == 0.f && off.pdy == 0.f && !off.psclOn && off.latchStamp < now - 1) continue;
                if (!map) {
                    map = std::make_shared<TrigState::AreaOffMap>();
                    map->items.reserve(entries.size());
                }
                map->items.push_back(e);
                map->bloom.set(TrigState::AreaOffMap::bloomHash(e.first));
            }
            return map;
        }

        template <class T1, class T2>
        auto AreaStep::canonVer(T1 const& items, T2&& keep) -> uint64_t {
            static thread_local std::unordered_map<uint64_t, uint64_t> canon;
            if (canon.size() > 8192) canon.clear();
            uint64_t contentHash = 0x5be0cd19137e2179ull;
            size_t count = 0;
            for (auto const& e : items) {
                if (!keep(e.first)) continue;
                contentHash = mix64(contentHash, static_cast<uint64_t>(static_cast<uint32_t>(e.first)));
                contentHash = hashPod(contentHash, e.second);
                ++count;
            }
            if (count == 0) return kEmptyVer;
            auto [cit, fresh] = canon.emplace(contentHash, 0);
            if (fresh) cit->second = trigNextVer();
            return cit->second;
        }

        auto AreaStep::finishState() -> void {
            auto const& items = T->areaOff().items;
            T->areaMoved.clear();
            for (auto const& e : items) {
                if (e.second.dx == 0.f && e.second.dy == 0.f) continue;
                Obj const& o = objAt(g, e.first);
                if (o.type == Decoration || (o.extendedCollision && o.id != 0x718)) continue;
                T->areaMoved.push_back(e.first);
            }
        }

        // nothing to do without area triggers or active areas: only the moves of the advanced follows are kept
        auto AreaStep::skipWithoutAreas() -> Flow {
            w = &(*g.world);
            T = &(g.trig);
            if (!w->hasAreaEffects) {
                if (!w->advDefs.empty()) {
                    T->areaMoved.clear();
                    addAfMoved(g);
                }
                return Flow::Return;
            }
            heavyIn = &(T->r());
            anyInst = !heavyIn->areas[0].empty() || !heavyIn->areas[1].empty() || !heavyIn->areas[2].empty();
            if (!anyInst && T->areaOffEmpty()) {
                T->areaVerS = 0;
                T->areaVerY = 0;
                T->areaSplitSig = 0;
                T->areaYBits.reset();
                if (!w->advDefs.empty()) {
                    T->areaMoved.clear();
                    addAfMoved(g);
                }
                return Flow::Return;
            }
            dt = static_cast<float>(dtSec);
            now = tickStamp(g);
            return Flow::Next;
        }

        // steps the value animations of the areas (memoized per step for the states with the same areas)
        auto AreaStep::stepAreaAnimations() -> void {
            {
                bool anyAnim = false;
                for (int k = 0; k < 3 && !anyAnim; ++k) {
                    for (auto const& inst : heavyIn->areas[k]) {
                        if (inst.paused) continue;
                        for (auto const& anim : inst.a) {
                            if (anim.on) {
                                anyAnim = true;
                                break;
                            }
                        }
                        if (anyAnim) break;
                    }
                }
                struct AnimMemo {
                    int stamp = -1;
                    WKey w;
                    std::vector<std::pair<std::pair<uint64_t, uint64_t>, std::shared_ptr<TrigHeavy>>> e;
                };
                static thread_local AnimMemo am;
                uint64_t animKey = 0;
                uint64_t dtBits = 0;
                bool const animMemoOn = anyAnim && !(g_noShare & 1);
                if (animMemoOn) {
                    if (am.stamp != now || am.w != g.world) {
                        am.stamp = now;
                        am.w = g.world;
                        am.e.clear();
                    }
                    animKey = trigHeavyHash(*heavyIn);
                    dtBits = bits64(dtSec);
                    for (auto const& x : am.e) {
                        if (x.first.first == animKey && x.first.second == dtBits) {
                            T->h = x.second;
                            anyAnim = false;
                            break;
                        }
                    }
                }
                if (anyAnim) {
                    TrigHeavy& H = T->w();
                    struct Commit {
                        bool on;
                        uint64_t k, d;
                        TrigState& T;

                        ~Commit() {
                            if (on && am.e.size() < 64) am.e.push_back({{k, d}, T.h});
                        }
                    } const commit{animMemoOn, animKey, dtBits, *T};
                    struct AreasAnimMemo {
                        int stamp = -1;
                        WKey w;
                        std::vector<std::tuple<uint64_t, uint64_t, std::shared_ptr<AreaLists>>> e;
                    };
                    static thread_local AreasAnimMemo aam;
                    bool done = false;
                    uint64_t areasHash = 0;
                    if (animMemoOn) {
                        if (aam.stamp != now || aam.w != g.world) {
                            aam.stamp = now;
                            aam.w = g.world;
                            aam.e.clear();
                        }
                        areasHash = trigAreasHash(H);
                        for (auto const& x : aam.e) {
                            if (std::get<0>(x) == areasHash && std::get<1>(x) == dtBits) {
                                H.areas.p = std::get<2>(x);
                                H.hashValid = false;
                                done = true;
                                break;
                            }
                        }
                    }
                    if (!done) {
                        for (int k = 0; k < 3; ++k) {
                            bool anyOn = false;
                            for (auto const& inst : H.areas[k]) {
                                if (inst.paused) continue;
                                for (auto const& anim : inst.a) {
                                    if (anim.on) {
                                        anyOn = true;
                                        break;
                                    }
                                }
                                if (anyOn) break;
                            }
                            if (!anyOn) continue;
                            for (auto& inst : H.areasW(k)) {
                                if (inst.paused) continue;
                                for (int i = 0; i < 28; ++i) {
                                    AreaAnim& anim = inst.a[i];
                                    if (!anim.on) continue;
                                    anim.elapsed += dt;
                                    if (anim.elapsed < anim.dur) {
                                        float const t = areaEase(anim.elapsed / anim.dur, anim.easing, anim.rate);
                                        inst.v[i] = (anim.to - anim.from) * t + anim.from;
                                    } else {
                                        inst.v[i] = anim.to;
                                        anim.on = 0;
                                    }
                                }
                            }
                        }
                        if (animMemoOn && aam.e.size() < 64) aam.e.emplace_back(areasHash, dtBits, H.areas.p);
                    }
                }
            }
        }

        // sorts the areas by priority and finds what their effects depend on: player y, camera, second player
        auto AreaStep::findDependencies() -> void {
            if (T->r().areasUnsorted) {
                TrigHeavy& heavyW = T->w();
                for (int k = 0; k < 3; ++k) {
                    if (!(heavyW.areasUnsorted & (1u << k))) continue;
                    auto& list = heavyW.areasW(k);
                    msvc::sort(list.begin(), list.end(), [&](AreaInst const& x, AreaInst const& y) {
                        return w->areaDefs[static_cast<size_t>(x.def)].priority >
                               w->areaDefs[static_cast<size_t>(y.def)].priority;
                    });
                }
                heavyW.areasUnsorted = 0;
            }
            playerSet = psetOf(g);
            needY = false;
            needCam = false;
            needCamX = false;
            needP2 = false;
            needPv = false;
            for (int k = 0; k < 3; ++k) {
                for (auto const& inst : T->r().areas[k]) {
                    if (inst.paused) continue;
                    int const deps = depOf(k, inst);
                    if (deps & 1) needY = true;
                    if (deps & 2) needCam = true;
                    if (deps & 32) needCamX = true;
                    if (deps & (4 | 16)) needPv = true;
                    if (deps & 8) needP2 = true;
                }
            }
        }

        // looks the step up in the area memo, keyed by the trigger state and what the effects depend on
        auto AreaStep::lookUpAreaMemo() -> Flow {
            memoOn = !(g_noShare & 1) && !needY;
            if (memoOn && amoOff > 0) {
                --amoOff;
                memoOn = false;
            }
            keyY = needY ? g.p1.y : 0.f;
            keyX2 = needP2 ? g.p2.x : 0.f;
            keyY2 = needP2 ? g.p2.y : 0.f;
            keyCX = (needCam || needCamX) ? g.cam.px : 0.f;
            keyCY = needCam ? (g.cam.on ? g.cam.py : g.p1.y) : 0.f;
            keyZ = (needCam || needCamX) ? g.cameraZoom : 0.f;
            keyDual = (needP2 || needCam || needCamX) && g.isDualMode;
            hH = 0;
            pvH = 0;
            dH = 0;
            memoCheckHit = false;
            if (memoOn) {
                hH = trigHeavyHash(T->r());
                pvH = (T->pv && needPv) ? pvHash(*T->pv) : 0;
                dH = T->d ? trigDynHash(*T->d) : 0;
                if (amo.stamp != now || amo.w != g.world) {
                    amo.stamp = now;
                    amo.w = g.world;
                    amo.e.clear();
                }
                for (auto const& E : amo.e) {
                    if (E.ver != T->areaVer || E.hH != hH || E.pvH != pvH || E.dH != dH) continue;
                    if (bits64(E.dt) != bits64(dtSec) || E.x != g.p1.x || E.y != keyY || E.x2 != keyX2 ||
                        E.y2 != keyY2 || E.camX != keyCX || E.camY != keyCY || E.zoom != keyZ || E.dual != keyDual)
                        continue;
                    if (debugOptions().memoCheck) {
                        memoCheckPending = E.off;
                        memoCheckHit = true;
                        break;
                    }
                    T->areaOffP = E.off;
                    T->areaMoved = E.moved;
                    T->areaVer = E.outVer;
                    noSplit();
                    ++g_areaMemoStat[0];
                    ++amoHits;
                    if (++amoCalls >= 512) {
                        if (amoHits * 4 < amoCalls) amoOff = 4096;
                        amoCalls = 0;
                        amoHits = 0;
                    }
                    addAfMoved(g);
                    return Flow::Return;
                }
            }
            return Flow::Next;
        }

        // per-call setup of the work buffer the effects write into
        auto AreaStep::prepareWork() -> void {
            inVer = T->areaVer;
            ++g_areaMemoStat[needY ? 2 : 1];
            if (memoOn && ++amoCalls >= 512) {
                if (amoHits * 4 < amoCalls) amoOff = 4096;
                amoCalls = 0;
                amoHits = 0;
            }
            H = &(T->r());
            p1 = &(g.p1);
            work = &(tl_areaWork);
            workScope.emplace(*work);
        }

        // when the effects depend on the player or the camera, applies the parts that do not and the parts that do
        // apart, so the first part can be shared
        auto AreaStep::applySplit() -> Flow {
            prevAreaOff = T->areaOffP;
            if ((needY || needCam || needPv) && !(g_noShare & 1)) {
                struct SplitInfo {
                    uint64_t sig = 0;
                    bool disjoint = false;
                    bool needP2S = false;
                    bool needCamXS = false;
                    bool anyY = false;
                    bool anyS = false;
                    bool ownPO = false;
                    bool sCenterPO = false;
                    std::shared_ptr<std::vector<uint64_t> const> yBits;
                    std::vector<short> sGroups;
                    std::vector<uint8_t> own[3];
                    std::vector<uint8_t> ownSel[3];
                    std::vector<int> sObjs;
                };
                uint64_t sig = mix64(0x243f6a8885a308d3ull, playerSet ? playerSet->sig : 0u);
                for (int k = 0; k < 3; ++k) {
                    for (auto const& inst : H->areas[k]) {
                        sig = mix64(sig, (static_cast<uint64_t>(k) << 60) ^
                                                 (static_cast<uint64_t>(static_cast<uint32_t>(inst.def)) << 28) ^
                                                 static_cast<uint64_t>(static_cast<uint32_t>(inst.target)));
                        sig = mix64(sig, (static_cast<uint64_t>(static_cast<uint32_t>(inst.center)) << 8) |
                                                 static_cast<uint64_t>(inst.paused ? 128 : depOf(k, inst)));
                    }
                }
                static thread_local std::vector<std::shared_ptr<SplitInfo>> splitCache;
                static thread_local WKey splitWorld;
                if (splitWorld != &*w) {
                    splitCache.clear();
                    splitWorld = &*w;
                }
                SplitInfo const* split = nullptr;
                for (auto const& c : splitCache) {
                    if (c->sig == sig) {
                        split = c.get();
                        break;
                    }
                }
                if (!split) {
                    auto info = std::make_shared<SplitInfo>();
                    info->sig = sig;
                    size_t const wordCount = (w->objs.size() + 63) / 64;
                    std::vector<uint64_t> ownBits(wordCount, 0ull);
                    std::vector<uint64_t> sharedBits(wordCount, 0ull);
                    auto hasBit = [&](std::vector<uint64_t> const& b, int idx) {
                        return idx >= 0 && ((b[static_cast<size_t>(idx) >> 6] >> (idx & 63)) & 1u) != 0;
                    };
                    auto setBit = [&](std::vector<uint64_t>& b, int idx) {
                        if (idx >= 0) b[static_cast<size_t>(idx) >> 6] |= 1ull << (idx & 63);
                    };
                    auto forTouched = [&](AreaInst const& inst, auto&& fn) {
                        auto const& def = w->areaDefs[static_cast<size_t>(inst.def)];
                        for (int idx : physOfGroup(*w, inst.target)) fn(idx);
                        if (World::TargetGroups const* targetGroups =
                                    w->targetGroupsOf(inst.target, def.ignoreGroupParent, def.ignoreLinked)) {
                            for (auto const& rep : targetGroups->reps) {
                                fn(rep.obj);
                                if (rep.center >= 0) fn(rep.center);
                                for (int child : rep.phys) fn(child);
                            }
                        }
                    };
                    for (int k = 0; k < 3; ++k) {
                        info->own[k].assign(H->areas[k].size(), 0);
                        for (size_t i = 0; i < H->areas[k].size(); ++i) {
                            AreaInst const& inst = H->areas[k][i];
                            if (inst.paused) continue;
                            int const deps = depOf(k, inst);
                            bool const stateDep = (deps & 3) != 0;
                            info->own[k][i] = stateDep ? 1 : 0;
                            if (!stateDep && (deps & 8)) info->needP2S = true;
                            if (!stateDep && (deps & 32)) info->needCamXS = true;
                            if (!stateDep && (deps & 16)) info->sCenterPO = true;
                            forTouched(inst, [&](int idx) {
                                if (stateDep || isPO(g, idx)) setBit(ownBits, idx);
                            });
                        }
                    }
                    for (bool changed = true; changed;) {
                        changed = false;
                        for (int k = 0; k < 3; ++k) {
                            for (auto const& inst : H->areas[k]) {
                                if (inst.paused) continue;
                                auto const& def = w->areaDefs[static_cast<size_t>(inst.def)];
                                World::TargetGroups const* targetGroups =
                                        w->targetGroupsOf(inst.target, def.ignoreGroupParent, def.ignoreLinked);
                                if (!targetGroups) continue;
                                for (auto const& rep : targetGroups->reps) {
                                    bool anyOwn = hasBit(ownBits, rep.obj) || hasBit(ownBits, rep.center);
                                    for (size_t q = 0; q < rep.phys.size() && !anyOwn; ++q) {
                                        anyOwn = hasBit(ownBits, rep.phys[q]);
                                    }
                                    if (!anyOwn) continue;
                                    auto mark = [&](int idx) {
                                        if (idx >= 0 && !hasBit(ownBits, idx)) {
                                            setBit(ownBits, idx);
                                            changed = true;
                                        }
                                    };
                                    mark(rep.obj);
                                    mark(rep.center);
                                    for (int child : rep.phys) mark(child);
                                }
                            }
                        }
                    }
                    for (int k = 0; k < 3; ++k) {
                        for (size_t i = 0; i < H->areas[k].size(); ++i) {
                            AreaInst const& inst = H->areas[k][i];
                            if (inst.paused || info->own[k][i]) continue;
                            forTouched(inst, [&](int idx) {
                                if (!hasBit(ownBits, idx)) setBit(sharedBits, idx);
                            });
                        }
                    }
                    info->disjoint = true;
                    for (size_t i = 0; i < wordCount; ++i) {
                        for (uint64_t bits = sharedBits[i]; bits; bits &= bits - 1) {
                            int const idx = static_cast<int>(i * 64 + static_cast<size_t>(__builtin_ctzll(bits)));
                            for (short group : w->objs[static_cast<size_t>(idx)].groups) {
                                if (group >= 0) info->sGroups.push_back(group);
                            }
                        }
                    }
                    for (int k = 0; k < 3; ++k) {
                        for (size_t i = 0; i < H->areas[k].size(); ++i) {
                            AreaInst const& inst = H->areas[k][i];
                            if (inst.paused || info->own[k][i]) continue;
                            auto const& def = w->areaDefs[static_cast<size_t>(inst.def)];
                            int const center = def.special != 0 ? def.special : inst.center;
                            if (center < 0) continue;
                            int const centerIdx = w->mainObjectOf(center);
                            if (centerIdx >= 0) {
                                for (short group : w->objs[static_cast<size_t>(centerIdx)].groups) {
                                    if (group >= 0) info->sGroups.push_back(group);
                                }
                            }
                        }
                    }
                    std::sort(info->sGroups.begin(), info->sGroups.end());
                    info->sGroups.erase(std::unique(info->sGroups.begin(), info->sGroups.end()), info->sGroups.end());
                    for (int k = 0; k < 3; ++k) {
                        info->ownSel[k].assign(H->areas[k].size(), 0);
                        for (size_t i = 0; i < H->areas[k].size(); ++i) {
                            AreaInst const& inst = H->areas[k][i];
                            if (inst.paused) continue;
                            auto const& def = w->areaDefs[static_cast<size_t>(inst.def)];
                            bool any = false;
                            for (int idx : physOfGroup(*w, inst.target)) {
                                if (hasBit(ownBits, idx)) {
                                    any = true;
                                    break;
                                }
                            }
                            if (!any) {
                                if (World::TargetGroups const* targetGroups =
                                            w->targetGroupsOf(inst.target, def.ignoreGroupParent, def.ignoreLinked)) {
                                    for (auto const& rep : targetGroups->reps) {
                                        if (hasBit(ownBits, rep.obj)) {
                                            any = true;
                                            break;
                                        }
                                    }
                                }
                            }
                            info->ownSel[k][i] = any ? 1 : 0;
                        }
                    }
                    for (size_t i = 0; i < wordCount; ++i) {
                        for (uint64_t bits = sharedBits[i]; bits; bits &= bits - 1) {
                            info->sObjs.push_back(
                                    static_cast<int>(i * 64 + static_cast<size_t>(__builtin_ctzll(bits))));
                        }
                    }
                    for (int k = 0; k < 3; ++k) {
                        for (size_t i = 0; i < H->areas[k].size(); ++i) {
                            AreaInst const& inst = H->areas[k][i];
                            if (inst.paused || info->own[k][i]) continue;
                            auto const& def = w->areaDefs[static_cast<size_t>(inst.def)];
                            if (World::TargetGroups const* targetGroups =
                                        w->targetGroupsOf(inst.target, def.ignoreGroupParent, def.ignoreLinked)) {
                                for (auto const& rep : targetGroups->reps) {
                                    info->sObjs.push_back(rep.obj);
                                    if (rep.center >= 0) info->sObjs.push_back(rep.center);
                                    for (int child : rep.phys) info->sObjs.push_back(child);
                                }
                            }
                            int const center = def.special != 0 ? def.special : inst.center;
                            if (center < 0) continue;
                            int const centerIdx = w->mainObjectOf(center);
                            if (centerIdx >= 0) info->sObjs.push_back(centerIdx);
                        }
                    }
                    std::sort(info->sObjs.begin(), info->sObjs.end());
                    info->sObjs.erase(std::unique(info->sObjs.begin(), info->sObjs.end()), info->sObjs.end());
                    for (size_t i = 0; i < wordCount && !info->anyY; ++i)
                        if (ownBits[i]) info->anyY = true;
                    for (size_t i = 0; i < wordCount && !info->anyS; ++i)
                        if (sharedBits[i]) info->anyS = true;
                    if (playerSet) {
                        for (size_t i = 0; i < wordCount && !info->ownPO; ++i) {
                            for (uint64_t bits = ownBits[i]; bits; bits &= bits - 1) {
                                int const idx = static_cast<int>(i * 64 + static_cast<size_t>(__builtin_ctzll(bits)));
                                if (bitOf(playerSet->objBits, idx)) {
                                    info->ownPO = true;
                                    break;
                                }
                            }
                        }
                        for (int k = 0; k < 3 && !info->ownPO; ++k) {
                            for (auto const& inst : H->areas[k]) {
                                if (!inst.paused && (depOf(k, inst) & 16)) {
                                    info->ownPO = true;
                                    break;
                                }
                            }
                        }
                    }
                    info->yBits = std::make_shared<std::vector<uint64_t> const>(std::move(ownBits));
                    if (splitCache.size() >= 16) splitCache.erase(splitCache.begin());
                    splitCache.push_back(info);
                    split = info.get();
                }
                if (split->disjoint) {
                    std::vector<uint64_t> const& ownBits = *split->yBits;
                    auto isOwn = [&](int idx) {
                        return ((ownBits[static_cast<size_t>(idx) >> 6] >> (idx & 63)) & 1u) != 0;
                    };
                    auto notOwn = [&](int idx) { return !isOwn(idx); };
                    auto ownInst = [&](int k, AreaInst const& inst) {
                        return split->own[k][static_cast<size_t>(&inst - H->areas[k].data())] != 0;
                    };

                    bool sharedNeeded = split->anyS;
                    if (!sharedNeeded) {
                        for (auto const& e : T->areaOff().items) {
                            if (!isOwn(e.first)) {
                                sharedNeeded = true;
                                break;
                            }
                        }
                    }
                    uint64_t const sharedInVer = !sharedNeeded ? kEmptyVer
                                                 : (T->areaSplitSig == split->sig && T->areaVerS != 0)
                                                         ? T->areaVerS
                                                         : canonVer(T->areaOff().items, notOwn);
                    std::shared_ptr<TrigState::AreaOffMap const> sharedOut;
                    uint64_t sharedVer = kEmptyVer;
                    bool found = false;
                    if (sharedNeeded) {
                        struct SM {
                            struct E {
                                uint64_t sIn, hH, dH, sig, outVer;
                                double dt;
                                float x, x2, cx, z;
                                bool dual;
                                std::shared_ptr<TrigState::AreaOffMap const> out;
                                uint64_t hg = 0, hi = 0;
                                std::shared_ptr<TrigMotion const> mo;
                            };
                            int stamp = -1;
                            WKey w;
                            std::vector<E> e;
                        };
                        static thread_local SM sm;
                        if (sm.stamp != now || sm.w != g.world) {
                            sm.stamp = now;
                            sm.w = g.world;
                            sm.e.clear();
                        }
                        TrigMotion const& motion = H->M();
                        uint64_t sharedHash = 0;
                        {
                            struct HC {
                                uint64_t ver = 0, sig = 0, h = 0;
                            };
                            static thread_local HC hc[8];
                            static thread_local unsigned hcNext = 0;
                            bool hit = false;
                            if (motion.ver != 0) {
                                for (auto const& c : hc) {
                                    if (c.ver == motion.ver && c.sig == split->sig) {
                                        sharedHash = c.h;
                                        hit = true;
                                        break;
                                    }
                                }
                            }
                            if (!hit) {
                                sharedHash = mix64(0x7a5e11ull, motion.pset ? motion.pset->sig : 0u);
                                size_t const wordCount = motion.movedGroups.size();
                                for (short group : split->sGroups) {
                                    size_t const word = static_cast<size_t>(static_cast<unsigned short>(group)) >> 6;
                                    bool const moved =
                                            word < wordCount && ((motion.movedGroups[word] >> (group & 63)) & 1u);
                                    GroupOff const* off = motion.groupOff.find(group);
                                    if (!moved && !off) continue;
                                    sharedHash = mix64(sharedHash, (static_cast<uint64_t>(static_cast<uint16_t>(group))
                                                                    << 1) | (moved ? 1u : 0u));
                                    if (off) sharedHash = hashPod(sharedHash, *off);
                                }
                                if (!motion.kfSnap.empty()) {
                                    sharedHash = mix64(hashVec(sharedHash, motion.kfSnap), 0x2f1d6b07ull);
                                }
                                if (motion.ver != 0) {
                                    hc[hcNext & 7u] = HC{motion.ver, split->sig, sharedHash};
                                    ++hcNext;
                                }
                            }
                        }
                        uint64_t const sharedGroupsHash = sharedHash;
                        uint64_t sharedInstHash = 0;
                        {
                            struct IC {
                                uint64_t hh = 0, sig = 0, h = 0;
                            };
                            static thread_local IC ic[8];
                            static thread_local unsigned icNext = 0;
                            uint64_t const areasHash = trigAreasHash(*H);
                            bool hit = false;
                            for (auto const& c : ic) {
                                if (c.hh == areasHash && c.sig == split->sig) {
                                    sharedInstHash = c.h;
                                    hit = true;
                                    break;
                                }
                            }
                            if (!hit) {
                                for (int k : {2, 1, 0}) {
                                    for (auto const& inst : H->areas[k]) {
                                        if (inst.paused || ownInst(k, inst)) continue;
                                        sharedInstHash = hashPod(mix64(sharedInstHash, static_cast<uint64_t>(k)), inst);
                                    }
                                }
                                ic[icNext & 7u] = IC{areasHash, split->sig, sharedInstHash};
                                ++icNext;
                            }
                        }
                        sharedHash = mix64(sharedHash, sharedInstHash);
                        if (split->sCenterPO) {
                            for (int k : {2, 1, 0}) {
                                for (auto const& inst : H->areas[k]) {
                                    if (inst.paused || ownInst(k, inst)) continue;
                                    if (depOf(k, inst) & 16) {
                                        auto const& def = w->areaDefs[static_cast<size_t>(inst.def)];
                                        int const center = def.special != 0 ? def.special : inst.center;
                                        double x, y;
                                        realPosBase(g, w->mainObjectOf(center), x, y);
                                        bool const rel = k == 0 && def.relative;
                                        bool const useX = rel || def.dirType != 2;
                                        bool const useY = rel || def.dirType != 1;
                                        sharedHash =
                                                mix64(sharedHash,
                                                      (useX ? bits64(static_cast<float>(x)) : 0x11u) ^
                                                              ((useY ? bits64(static_cast<float>(y)) : 0x22u) << 32) ^
                                                              0xce17e5ull);
                                    }
                                }
                            }
                        }
                        uint64_t sharedDynHash = 0;
                        if (T->d) {
                            struct DC {
                                uint64_t ver = 0, sig = 0, h = 0;
                            };
                            static thread_local DC dc[8];
                            static thread_local unsigned dcNext = 0;
                            TrigDyn const& dyn = *T->d;
                            bool hit = false;
                            if (dyn.ver != 0) {
                                for (auto const& c : dc) {
                                    if (c.ver == dyn.ver && c.sig == split->sig) {
                                        sharedDynHash = c.h;
                                        hit = true;
                                        break;
                                    }
                                }
                            }
                            if (!hit) {
                                uint64_t hash = 0x3d1b58baull;
                                auto const& dynItems = dyn.objDyn.items;
                                auto const& sharedObjs = split->sObjs;
                                size_t i = 0;
                                size_t j = 0;
                                while (i < dynItems.size() && j < sharedObjs.size()) {
                                    if (dynItems[i].first < sharedObjs[j])
                                        ++i;
                                    else if (sharedObjs[j] < dynItems[i].first)
                                        ++j;
                                    else {
                                        hash = hashPod(mix64(hash, static_cast<uint64_t>(
                                                                           static_cast<uint32_t>(dynItems[i].first))),
                                                       dynItems[i].second);
                                        ++i;
                                        ++j;
                                    }
                                }
                                sharedDynHash = hash;
                                if (dyn.ver != 0) {
                                    dc[dcNext & 7u] = DC{dyn.ver, split->sig, hash};
                                    ++dcNext;
                                }
                            }
                        }
                        float const sKeyX2 = split->needP2S ? g.p2.x : 0.f;
                        bool const sKeyDual = (split->needP2S || split->needCamXS) && g.isDualMode;
                        float const sKeyCamX = split->needCamXS ? g.cam.px : 0.f;
                        float const sKeyZoom = split->needCamXS ? g.cameraZoom : 0.f;
                        for (auto const& E : sm.e) {
                            if (E.sIn != sharedInVer || E.hH != sharedHash || E.dH != sharedDynHash ||
                                E.sig != split->sig || bits64(E.dt) != bits64(dtSec) || E.x != g.p1.x ||
                                E.x2 != sKeyX2 || E.dual != sKeyDual)
                                continue;
                            if (bits64(E.cx) != bits64(sKeyCamX) || bits64(E.z) != bits64(sKeyZoom)) continue;
                            sharedOut = E.out;
                            sharedVer = E.outVer;
                            found = true;
                            ++g_areaMemoStat[0];
                            ++g_areaSplitStat[0];
                            break;
                        }
                        if (!found) {
                            ++g_areaSplitStat[1];
                            loadWork(notOwn);
                            work->owner = &*T;
                            kernel2([&](int k, AreaInst const& inst) { return !ownInst(k, inst); }, notOwn);
                            sharedOut = finishWork();
                            sharedVer = sharedOut ? canonVer(sharedOut->items, all) : kEmptyVer;
                            if (sm.e.size() < 32) {
                                sm.e.push_back(SM::E{sharedInVer, sharedHash, sharedDynHash, split->sig, sharedVer,
                                                     dtSec, g.p1.x, sKeyX2, sKeyCamX, sKeyZoom, sKeyDual, sharedOut,
                                                     sharedGroupsHash, sharedInstHash, nullptr});
                            }
                        }
                    }

                    std::shared_ptr<TrigState::AreaOffMap> ownOut;
                    bool anyOwnEntry = false;
                    if (!split->anyY) {
                        for (auto const& e : T->areaOff().items) {
                            if (isOwn(e.first)) {
                                anyOwnEntry = true;
                                break;
                            }
                        }
                    }
                    bool const ownPart = split->anyY || anyOwnEntry;
                    struct YM {
                        struct E {
                            uint64_t k, yVer, aVer;
                            std::shared_ptr<TrigState::AreaOffMap const> out;
                            std::shared_ptr<std::vector<int> const> moved;
                            uint64_t c[6];
                        };
                        int stamp = -1;
                        WKey w;
                        std::vector<E> e;
                    };
                    static thread_local YM ym;
                    bool const ownMemoOn = ownPart;
                    uint64_t ownKey = 0;
                    uint64_t ownVer = kEmptyVer;
                    uint64_t ownKeyParts[6] = {0, 0, 0, 0, 0, 0};
                    YM::E const* ownHit = nullptr;
                    if (ownMemoOn) {
                        if (ym.stamp != now || ym.w != g.world) {
                            ym.stamp = now;
                            ym.w = g.world;
                            ym.e.clear();
                        }
                        uint64_t const ownInVer = (T->areaSplitSig == split->sig && T->areaVerY != 0)
                                                          ? T->areaVerY
                                                          : canonVer(T->areaOff().items, isOwn);
                        ownKeyParts[0] = mix64(split->sig, sharedVer);
                        ownKeyParts[1] = ownInVer;
                        ownKeyParts[2] = trigHeavyHash(*H);
                        ownKeyParts[3] = T->d ? trigDynHash(*T->d) : 0u;
                        ownKeyParts[4] = (split->ownPO && T->pv) ? pvHash(*T->pv) : 0u;
                        uint64_t posHash = mix64(bits64(dtSec), bits64(g.p1.x));
                        if (needY) posHash = mix64(posHash, bits64(g.p1.y));
                        if (needCam || needCamX)
                            posHash = mix64(mix64(posHash, bits64(g.cam.px)), bits64(g.cameraZoom));
                        if (needCam) posHash = mix64(posHash, bits64(g.cam.on ? g.cam.py : g.p1.y));
                        if (needP2) posHash = mix64(mix64(posHash, bits64(g.p2.x)), bits64(g.p2.y));
                        ownKeyParts[5] = mix64(posHash, g.isDualMode ? 0xd0a1ull : 0x51e9ull);
                        ownKey = 0x59e7a11ull;
                        for (uint64_t c : ownKeyParts) ownKey = mix64(ownKey, c);
                        for (auto const& E : ym.e) {
                            if (E.k == ownKey) {
                                ownHit = &E;
                                break;
                            }
                        }
                    }
                    if (ownHit) {
                        T->areaOffP = ownHit->out;
                        ownVer = ownHit->yVer;
                        ++g_areaMemoStat[0];
                        ++g_areaSplitStat[2];
                    } else {
                        if (ownPart) {
                            ++g_areaSplitStat[3];
                            loadWork(isOwn);
                            work->owner = &*T;
                            kernel2(
                                    [&](int k, AreaInst const& inst) {
                                        return split->ownSel[k][static_cast<size_t>(&inst - H->areas[k].data())] != 0;
                                    },
                                    isOwn);
                            ownOut = finishWork();
                        }
                        if (!ownOut)
                            T->areaOffP = sharedOut;
                        else if (!sharedOut)
                            T->areaOffP = ownOut;
                        else {
                            auto merged = std::make_shared<TrigState::AreaOffMap>();
                            merged->items.resize(sharedOut->items.size() + ownOut->items.size());
                            std::merge(sharedOut->items.begin(), sharedOut->items.end(), ownOut->items.begin(),
                                       ownOut->items.end(), merged->items.begin(),
                                       [](auto const& x, auto const& y) { return x.first < y.first; });
                            for (auto const& e : merged->items) {
                                merged->bloom.set(TrigState::AreaOffMap::bloomHash(e.first));
                            }
                            T->areaOffP = merged;
                        }
                        ownVer = ownOut ? canonVer(ownOut->items, all) : kEmptyVer;
                    }
                    if (debugOptions().memoCheck) {
                        std::shared_ptr<TrigState::AreaOffMap const> const got = T->areaOffP;
                        T->areaOffP = prevAreaOff;
                        loadWork(all);
                        work->owner = &*T;
                        kernel([](int, AreaInst const&) { return true; });
                        std::shared_ptr<TrigState::AreaOffMap const> const reference = finishWork();
                        T->areaOffP = got;
                        size_t const gotCount = got ? got->items.size() : 0;
                        size_t const refCount = reference ? reference->items.size() : 0;
                        bool same = gotCount == refCount;
                        for (size_t i = 0; same && i < gotCount; ++i) {
                            same = got->items[i].first == reference->items[i].first &&
                                   std::memcmp(&got->items[i].second, &reference->items[i].second, sizeof(AreaOff)) ==
                                           0;
                        }
                        if (!same) {
                            std::fprintf(stderr,
                                         "[area-split] MISMATCH tick %d : %zu entries (split) vs %zu (full), memo %s\n",
                                         g.tick, gotCount, refCount, found ? "read back" : "computed");
                            int shown = 0;
                            for (size_t i = 0, j = 0; (i < gotCount || j < refCount) && shown < 6;) {
                                int const ki = i < gotCount ? got->items[i].first : INT32_MAX;
                                int const kj = j < refCount ? reference->items[j].first : INT32_MAX;
                                if (ki == kj) {
                                    if (std::memcmp(&got->items[i].second, &reference->items[j].second,
                                                    sizeof(AreaOff)) != 0) {
                                        AreaOff const& gotOff = got->items[i].second;
                                        AreaOff const& refOff = reference->items[j].second;
                                        std::fprintf(stderr,
                                                     "    object %d (uid %d, %s) : dx %g/%g dy %g/%g rot %g/%g scl "
                                                     "%d/%d sx %g/%g pdx %g/%g latch %d/%d st %d/%d\n",
                                                     ki, w->objs[static_cast<size_t>(ki)].uid,
                                                     isOwn(ki) ? "own" : "shared", gotOff.dx, refOff.dx, gotOff.dy,
                                                     refOff.dy, gotOff.rot, refOff.rot, gotOff.sclOn, refOff.sclOn,
                                                     gotOff.sx, refOff.sx, gotOff.pdx, refOff.pdx, gotOff.latch,
                                                     refOff.latch, gotOff.latchStamp, refOff.latchStamp);
                                        ++shown;
                                    }
                                    ++i;
                                    ++j;
                                } else if (ki < kj) {
                                    std::fprintf(stderr, "    object %d (uid %d) extra in the split\n", ki,
                                                 w->objs[static_cast<size_t>(ki)].uid);
                                    ++i;
                                    ++shown;
                                } else {
                                    std::fprintf(stderr, "    object %d (uid %d) missing from the split\n", kj,
                                                 w->objs[static_cast<size_t>(kj)].uid);
                                    ++j;
                                    ++shown;
                                }
                            }
                        }
                    }
                    T->areaVerS = sharedVer;
                    T->areaVerY = ownVer;
                    T->areaSplitSig = split->sig;
                    T->areaYBits = split->yBits;
                    if (ownHit) {
                        T->areaVer = ownHit->aVer;
                        T->areaMoved = *ownHit->moved;
                    } else {
                        T->areaVer = T->areaOffEmpty()
                                             ? 0
                                             : (ownVer != kEmptyVer ? mix64(mix64(sharedVer, ownVer), 0x7e11a5ull)
                                                                    : mix64(sharedVer, 0x5ab1e5ull));
                        finishState();
                        if (ownMemoOn && ym.e.size() < 64) {
                            ym.e.push_back(YM::E{ownKey,
                                                 ownVer,
                                                 T->areaVer,
                                                 T->areaOffP,
                                                 std::make_shared<std::vector<int> const>(T->areaMoved),
                                                 {ownKeyParts[0], ownKeyParts[1], ownKeyParts[2], ownKeyParts[3],
                                                  ownKeyParts[4], ownKeyParts[5]}});
                        }
                    }
                    addAfMoved(g);
                    return Flow::Return;
                }
            }
            return Flow::Next;
        }

        // applies all the areas in one pass
        auto AreaStep::applyAll() -> void {
            noSplit();
            loadWork(all);
            work->owner = &*T;
            kernel([](int, AreaInst const&) { return true; });
            T->areaOffP = finishWork();
        }

        // debug (APSIM_MEMOCHECK): compares a memo hit with the computed result
        auto AreaStep::checkMemoHit() -> void {
            if (memoCheckHit) {
                auto const* memoMap = memoCheckPending.get();
                auto const* computedMap = T->areaOffP.get();
                size_t const memoCount = memoMap ? memoMap->items.size() : 0;
                size_t const computedCount = computedMap ? computedMap->items.size() : 0;
                bool same = memoCount == computedCount;
                for (size_t i = 0; same && i < memoCount; ++i) {
                    same = memoMap->items[i].first == computedMap->items[i].first &&
                           std::memcmp(&memoMap->items[i].second, &computedMap->items[i].second, sizeof(AreaOff)) == 0;
                }
                if (!same) {
                    static thread_local int nrep = 0;
                    if (nrep++ < 20) {
                        std::fprintf(stderr,
                                     "[area memo] MISMATCH tick %d x %.3f y %.3f : %zu entries (memo) vs %zu "
                                     "(computed) ; needY %d cam %d p2 %d pv %d\n",
                                     g.tick, g.p1.x, g.p1.y, memoCount, computedCount, needY, needCam, needP2, needPv);
                        int shown = 0;
                        for (size_t i = 0, j = 0; (i < memoCount || j < computedCount) && shown < 4;) {
                            int const ki = i < memoCount ? memoMap->items[i].first : INT32_MAX;
                            int const kj = j < computedCount ? computedMap->items[j].first : INT32_MAX;
                            if (ki == kj) {
                                if (std::memcmp(&memoMap->items[i].second, &computedMap->items[j].second,
                                                sizeof(AreaOff)) != 0) {
                                    AreaOff const& memoOff = memoMap->items[i].second;
                                    AreaOff const& computedOff = computedMap->items[j].second;
                                    std::fprintf(stderr,
                                                 "    object uid %d PO %d : dx %g/%g dy %g/%g rot %g/%g sx %g/%g pdx "
                                                 "%g/%g\n",
                                                 w->objs[static_cast<size_t>(ki)].uid, isPO(g, ki) ? 1 : 0, memoOff.dx,
                                                 computedOff.dx, memoOff.dy, computedOff.dy, memoOff.rot,
                                                 computedOff.rot, memoOff.sx, computedOff.sx, memoOff.pdx,
                                                 computedOff.pdx);
                                    ++shown;
                                }
                                ++i;
                                ++j;
                            } else if (ki < kj) {
                                std::fprintf(stderr, "    uid %d only in the memo\n",
                                             w->objs[static_cast<size_t>(ki)].uid);
                                ++i;
                                ++shown;
                            } else {
                                std::fprintf(stderr, "    uid %d only in the computation\n",
                                             w->objs[static_cast<size_t>(kj)].uid);
                                ++j;
                                ++shown;
                            }
                        }
                        for (int k = 0; k < 3; ++k) {
                            for (auto const& inst : H->areas[k]) {
                                if (!inst.paused) {
                                    std::fprintf(
                                            stderr, "    instance uid %d kind %d dep %d\n",
                                            w->objs[static_cast<size_t>(w->areaDefs[static_cast<size_t>(inst.def)].idx)]
                                                    .uid,
                                            k, depOf(k, inst));
                                }
                            }
                        }
                    }
                }
            }
        }

        // gives the result a version number (the same content gets the same number) and stores it in the memo
        auto AreaStep::storeAreaResult() -> void {
            finishState();
            items = &(T->areaOff().items);
            if (items->empty())
                T->areaVer = 0;
            else if (!memoOn)
                T->areaVer = trigNextVer();
            else {
                static thread_local std::unordered_map<uint64_t, uint64_t> canon;
                if (canon.size() > 8192) canon.clear();
                uint64_t contentHash = 0x5be0cd19137e2179ull;
                for (auto const& e : *items) {
                    contentHash = mix64(contentHash, static_cast<uint64_t>(static_cast<uint32_t>(e.first)));
                    contentHash = hashPod(contentHash, e.second);
                }
                auto [cit, fresh] = canon.emplace(contentHash, 0);
                if (fresh) cit->second = trigNextVer();
                T->areaVer = cit->second;
            }
            if (memoOn && amo.e.size() < 32) {
                AreaMemo::E entry;
                entry.ver = inVer;
                entry.outVer = T->areaVer;
                entry.hH = hH;
                entry.pvH = pvH;
                entry.dH = dH;
                entry.dt = dtSec;
                entry.x = g.p1.x;
                entry.y = keyY;
                entry.x2 = keyX2;
                entry.y2 = keyY2;
                entry.camX = keyCX;
                entry.camY = keyCY;
                entry.zoom = keyZ;
                entry.dual = keyDual;
                entry.off = T->areaOffP;
                entry.moved = T->areaMoved;
                amo.e.push_back(std::move(entry));
            }
            addAfMoved(g);
        }

        auto AreaStep::run() -> void {
            if (skipWithoutAreas() == Flow::Return) return;
            stepAreaAnimations();
            findDependencies();
            if (lookUpAreaMemo() == Flow::Return) return;
            prepareWork();
            if (applySplit() == Flow::Return) return;
            applyAll();
            checkMemoHit();
            storeAreaResult();
        }

        void trigAreaActionsImpl(Game& g, double dtSec) {
            AreaStep areas(g, dtSec);
            areas.run();
        }

    }
}
