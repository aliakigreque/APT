// Measures how much each press of a path can move in time and centers it in its window.

#include "robust.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "triggers.hpp"

namespace ap::sim {

    namespace {
        struct Seg {
            int bit;
            int a;
            int b;
        };

        // exact compare of the player fields that decide what happens next
        bool samePlayer(Player const& p, Player const& q) {
            return p.x == q.x && p.y == q.y && p.yVelocity == q.yVelocity && p.isUpsideDown == q.isUpsideDown &&
                   p.isShip == q.isShip && p.isBall == q.isBall && p.isBird == q.isBird && p.isDart == q.isDart &&
                   p.isRobot == q.isRobot && p.isSpider == q.isSpider && p.isSwing == q.isSwing &&
                   p.isOnGround == q.isOnGround && p.isOnGround2 == q.isOnGround2 && p.vehicleSize == q.vehicleSize &&
                   p.playerSpeed == q.playerSpeed && p.gravityMod == q.gravityMod && p.isDashing == q.isDashing &&
                   p.holdingJump == q.holdingJump && p.jumpBuffered == q.jumpBuffered && p.isSideways == q.isSideways &&
                   p.isGoingLeft == q.isGoingLeft && p.isDead == q.isDead;
        }

        // returns true if both games will play the rest of the plan the same way
        bool samePhys(Game const& x, Game const& y) {
            if (x.died != y.died || x.completed != y.completed) return false;
            if (!samePlayer(x.p1, y.p1)) return false;
            if (x.isDualMode != y.isDualMode) return false;
            if (x.isDualMode && !samePlayer(x.p2, y.p2)) return false;
            if (x.osSize() != y.osSize() || x.triggeredIDs.size() != y.triggeredIDs.size()) return false;
            if (x.trig.on && trigHash(x) != trigHash(y)) return false;
            return true;
        }

        // plays the plan from tick `from` to `to`, stops on death or level end
        void run(Game& g, std::vector<uint8_t> const& plan, int from, int to) {
            for (int t = from; t < to && !g.died && !g.completed; ++t)
                stepTick(g, t < static_cast<int>(plan.size()) ? plan[static_cast<size_t>(t)] : 0);
        }
    }

    // tries every shift of each edge, keeps the ones that end in the same state, then centers the edge
    std::vector<uint8_t> robustifyPlan(Game const& start, std::vector<uint8_t> const& inputs, RobustOptions const& opt,
                                       RobustReport* report, std::atomic<bool> const* cancel) {
        RobustReport rep;
        int const planSize = static_cast<int>(inputs.size());
        int const startTick = start.tick;

        std::vector<Seg> segs;
        for (int bit = 0; bit < 2; ++bit) {
            int segStart = -1;
            for (int t = 0; t <= planSize; ++t) {
                bool const on = t < planSize && ((inputs[static_cast<size_t>(t)] >> bit) & 1u);
                if (on && segStart < 0) segStart = t;
                if (!on && segStart >= 0) {
                    segs.push_back(Seg{bit, segStart, t});
                    segStart = -1;
                }
            }
        }
        std::sort(segs.begin(), segs.end(),
                  [](Seg const& x, Seg const& y) { return x.a != y.a ? x.a < y.a : x.bit < y.bit; });
        std::vector<uint8_t> plan = inputs;
        Game cur = start;
        int curTick = 0;
        auto planRel = [&](Game& g, std::vector<uint8_t> const& p, int from, int to) {
            for (int t = from; t < to && !g.died && !g.completed; ++t)
                stepTick(g, t < static_cast<int>(p.size()) ? p[static_cast<size_t>(t)] : 0);
        };

        struct Edge {
            int pos;
            int seg;
            bool press;
        };
        std::vector<Edge> edges;
        for (size_t k = 0; k < segs.size(); ++k) {
            edges.push_back(Edge{segs[k].a, static_cast<int>(k), true});
            if (segs[k].b < planSize) edges.push_back(Edge{segs[k].b, static_cast<int>(k), false});
        }
        std::sort(edges.begin(), edges.end(),
                  [](Edge const& x, Edge const& y) { return x.pos != y.pos ? x.pos < y.pos : x.seg < y.seg; });
        auto setSeg = [&](std::vector<uint8_t>& p, Seg const& sg, bool on) {
            uint8_t const mask = static_cast<uint8_t>(1u << sg.bit);
            for (int t = std::max(0, sg.a); t < std::min(planSize, sg.b); ++t) {
                if (on)
                    p[static_cast<size_t>(t)] |= mask;
                else
                    p[static_cast<size_t>(t)] &= static_cast<uint8_t>(~mask);
            }
        };
        double windowSum = 0;
        int windowCount = 0;
        int minWindow = 1 << 30;
        std::vector<int> pressWindow(segs.size(), -1);

        for (Edge const& e : edges) {
            if (cancel && cancel->load()) break;
            Seg& seg = segs[static_cast<size_t>(e.seg)];
            bool const press = e.press;
            int const pos = press ? seg.a : seg.b;

            int prevEnd = -1000000;
            int prevStart = -1000000;
            int nextStart = 1000000;
            for (size_t j = 0; j < segs.size(); ++j) {
                if (static_cast<int>(j) == e.seg) continue;
                Seg const& other = segs[j];
                if (other.bit != seg.bit) continue;
                if (other.a < seg.a) {
                    prevEnd = std::max(prevEnd, other.b);
                    prevStart = std::max(prevStart, other.a);
                } else {
                    nextStart = std::min(nextStart, other.a);
                }
            }

            int lo = -opt.maxShift;
            int hi = opt.maxShift;
            if (press) {
                lo = std::max(lo, prevEnd + 1 - seg.a);
                lo = std::max(lo, prevStart + opt.minPressGap - seg.a);
                hi = std::min(hi, seg.b - 1 - seg.a);
                hi = std::min(hi, nextStart - opt.minPressGap - seg.a);
            } else {
                lo = std::max(lo, seg.a + 1 - seg.b);
                hi = std::min(hi, nextStart - 1 - seg.b);
            }
            lo = std::max(lo, curTick - pos);
            if (hi < 0) hi = 0;
            if (lo > 0) lo = 0;

            int nextEdge = 1000000;
            for (Edge const& f : edges) {
                if (f.seg == e.seg && (f.press == press || press)) continue;
                Seg const& other = segs[static_cast<size_t>(f.seg)];
                int const otherPos = f.press ? other.a : other.b;
                if (otherPos > pos + hi) nextEdge = std::min(nextEdge, otherPos);
            }
            int const lastTouched = pos + hi;
            int compareTick = std::min(lastTouched + opt.settle, pos + opt.maxHorizon);
            if (nextEdge < 1000000) compareTick = std::min(compareTick, nextEdge);
            compareTick = std::min(compareTick, planSize);
            if (compareTick <= lastTouched || lo == hi) continue;

            int const firstTouched = pos + lo;
            planRel(cur, plan, curTick, firstTouched);
            curTick = firstTouched;
            if (cur.died || cur.completed) break;
            Game ref = cur;
            planRel(ref, plan, firstTouched, compareTick);
            if (ref.died) continue;

            std::vector<char> shiftOk(static_cast<size_t>(hi - lo + 1), 0);
            shiftOk[static_cast<size_t>(-lo)] = 1;
            std::vector<uint8_t> variant;
            bool anyConverged = false;
            for (int dir = -1; dir <= 1; dir += 2) {
                for (int shift = dir; shift >= lo && shift <= hi; shift += dir) {
                    variant = plan;
                    Seg shifted = seg;
                    if (press)
                        shifted.a += shift;
                    else
                        shifted.b += shift;
                    setSeg(variant, seg, false);
                    setSeg(variant, shifted, true);
                    Game g = cur;
                    planRel(g, variant, firstTouched, compareTick);
                    bool const good = !g.died && samePhys(g, ref);
                    shiftOk[static_cast<size_t>(shift - lo)] = good ? 1 : 0;
                    if (good)
                        anyConverged = true;
                    else
                        break;
                }
            }

            int windowLo = 0;
            int windowHi = 0;
            while (windowLo - 1 >= lo && shiftOk[static_cast<size_t>(windowLo - 1 - lo)]) --windowLo;
            while (windowHi + 1 <= hi && shiftOk[static_cast<size_t>(windowHi + 1 - lo)]) ++windowHi;
            int const width = windowHi - windowLo + 1;
            if (!anyConverged) {
                bool const fly = cur.p1.isShip || cur.p1.isDart || cur.p1.isBird || cur.p1.isSwing;
                if (fly) continue;
            }
            if (press) pressWindow[static_cast<size_t>(e.seg)] = width;
            if (!press && width == 1) ++rep.narrowRelease;

            int const center = (windowLo + windowHi) / 2;
            if (opt.apply && center != 0) {
                Seg shifted = seg;
                if (press)
                    shifted.a += center;
                else
                    shifted.b += center;
                setSeg(plan, seg, false);
                setSeg(plan, shifted, true);
                seg = shifted;
                ++rep.moved;
            }
        }

        for (size_t k = 0; k < segs.size(); ++k) {
            ++rep.segments;
            int const window = pressWindow[k];
            rep.windows.push_back(window);
            if (window < 0) {
                ++rep.noConverge;
                continue;
            }
            windowSum += window;
            ++windowCount;
            minWindow = std::min(minWindow, window);
            if (window == 1) ++rep.narrow;
        }
        (void)startTick;
        rep.avgWindow = windowCount ? windowSum / windowCount : 0.0;
        rep.minWindow = windowCount ? minWindow : 0;

        std::vector<uint8_t> result = plan;
        if (rep.moved > 0) {
            Game original = start;
            Game changed = start;
            run(original, inputs, 0, planSize);
            run(changed, plan, 0, planSize);
            bool const okFinal = !changed.died && (changed.completed == original.completed) &&
                                 (original.completed || samePhys(original, changed) || !original.died);
            if (!okFinal) {
                result = inputs;
                rep.moved = 0;
                rep.validated = false;
            } else {
                rep.validated = true;
            }
        } else {
            rep.validated = true;
        }
        if (report) *report = rep;
        return result;
    }

}
