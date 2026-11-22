// Costs and checks about portals and orbs: clusters taken or missed, flying exits, prepaid portals, and the
// spans that follow a state between two portals.

#include "search_internal.hpp"

namespace ap::sim {
    namespace search_impl {
        // one letter for the game mode, for debug output
        char modeCharOf(Player const& p) {
            return p.isShip     ? 'S'
                   : p.isBall   ? 'B'
                   : p.isBird   ? 'U'
                   : p.isDart   ? 'W'
                   : p.isRobot  ? 'R'
                   : p.isSpider ? 'P'
                   : p.isSwing  ? 'G'
                                : 'C';
        }

        // one bit for the game mode (bit 0 is cube)
        uint8_t modeBitOf(Player const& p) {
            return static_cast<uint8_t>(1u << (p.isShip     ? 1
                                               : p.isBall   ? 2
                                               : p.isBird   ? 3
                                               : p.isDart   ? 4
                                               : p.isRobot  ? 5
                                               : p.isSpider ? 6
                                               : p.isSwing  ? 7
                                                            : 0));
        }

        // portal cluster from its index, negative indexes are the extra clusters
        World::PortalCluster const& clusterOf(Cues const& cues, int k) {
            return k >= 0 ? (*cues.clusters)[static_cast<size_t>(k)] : cues.extra[static_cast<size_t>(-k - 1)];
        }

        // cluster paid on approach instead of when passed (not speed, not learned)
        bool prepaid(World::PortalCluster const& c) { return !c.speed && !c.learned; }

        // returns true if a player already took the portal in this state
        bool portalTakenBy(Game const& g, int idx) {
            if (idx < 0) return false;
            auto const* state = g.osFind(idx);
            if (state && (state->activatedByPlayer1 || state->activatedByPlayer2 || state->activated)) return true;
            return g.trig.on && trigActivatedBit(g, idx);
        }

        // returns true for the 8 game mode portals
        bool isGameModePortal(int type) {
            switch (type) {
            case ShipPortal:
            case CubePortal:
            case BallPortal:
            case UfoPortal:
            case WavePortal:
            case RobotPortal:
            case SpiderPortal:
            case SwingPortal:
                return true;
            default:
                return false;
            }
        }

        // returns true if any portal of the cluster was taken
        bool clusterTaken(Game const& g, World::PortalCluster const& c) {
            for (int m : c.members) {
                auto const* state = g.osFind(m);
                if (state && (state->activatedByPlayer1 || state->activatedByPlayer2 || state->activated)) return true;
                if (g.trig.on && trigActivatedBit(g, m)) return true;
            }
            return false;
        }

        // vertical distance from player 1 to the cluster (current positions if animated), 1e30 if nothing to take
        float clusterDy(Game const& g, World::PortalCluster const& c) {
            bool anyActive = false;
            bool anyEffect = false;
            for (int m : c.members) {
                if (!const_cast<Game&>(g).obj(m).disabled()) {
                    anyActive = true;
                    if (!portalNoOp(g, g.world->objs[static_cast<size_t>(m)])) {
                        anyEffect = true;
                        break;
                    }
                }
            }
            if (!anyActive) return 1e30f;
            if (!anyEffect && !c.speed && !c.learned && !g.isDualMode) return 1e30f;
            float yLo = c.yLo;
            float yHi = c.yHi;
            if (c.dyn) {
                yLo = 1e30f;
                yHi = -1e30f;
                for (int m : c.members) {
                    GObj const o = const_cast<Game&>(g).obj(m);
                    if (o.disabled()) continue;
                    Rect const r = o.getObjectRect();
                    Rect const startRect = o.o->rect;
                    if (std::fabs((r.x + r.w * 0.5f) - (startRect.x + startRect.w * 0.5f)) > 90.f) continue;
                    yLo = std::min(yLo, r.y);
                    yHi = std::max(yHi, r.y + r.h);
                }
                if (!(yLo <= yHi)) return 1e30f;
            }
            float const y = g.p1.y;
            return y < yLo ? yLo - y : (y > yHi ? y - yHi : 0.f);
        }

        //returns true if the cluster is not taken and within reach
        bool clusterOwed(Game const& g, World::PortalCluster const& c) {
            return !clusterTaken(g, c) && clusterDy(g, c) <= portalReach(g.p1, c);
        }

        // cost pulling a flying player toward the closest exit portal ahead (off when flyExitCap is 0)
        int flyExitCost(Game const& g, Cues const& cues, SearchOptions const& opt) {
            Player const& p = g.p1;
            if (opt.flyExitCap <= 0 || p.isSideways || g.isPlatformer || g.isDualMode) return 0;
            if (!(p.isShip || p.isBird || p.isDart || p.isSwing)) return 0;
            if (cues.passX.empty()) return 0;
            float best = 1e30f;
            auto it = std::upper_bound(cues.passX.begin(), cues.passX.end(), std::make_pair(p.x, 1 << 30));
            for (; it != cues.passX.end(); ++it) {
                World::PortalCluster const& c = clusterOf(cues, it->second);
                if (c.approachX > p.x + opt.flyExitAhead) {
                    if (it->first > p.x + opt.flyExitAhead + 600.f) break;
                    continue;
                }
                if (c.speed || c.learned) continue;
                int taken = -1;
                for (int m : c.members) {
                    Obj const& o = g.world->objs[static_cast<size_t>(m)];
                    if (!flyExitType(p, o.type)) continue;
                    Rect const r = c.dyn ? const_cast<Game&>(g).obj(m).getObjectRect() : o.rect;
                    float const dy = p.y < r.y ? r.y - p.y : (p.y > r.y + r.h ? p.y - (r.y + r.h) : 0.f);
                    if (!(dy < best)) continue;
                    if (taken < 0) taken = clusterTaken(g, c) ? 1 : 0;
                    if (taken) break;
                    if (const_cast<Game&>(g).obj(m).disabled()) continue;
                    best = dy;
                }
                if (best <= 0.f) return 0;
            }
            if (!(best < 1e29f)) return 0;
            int const cap = opt.flyExitCap + (p.isDart ? 2 : 0);
            return std::min(cap, static_cast<int>(best / opt.flyExitStep));
        }

        // prepaid clusters whose approach window contains x (at most 8), with whether they are owed
        void openClusters(Game const& g, float x, Cues const& cues, SmallVec<std::pair<int, bool>, 8>& out) {
            out.clear();
            if (cues.approachX.empty() || g.p1.isSideways) return;
            auto it = std::upper_bound(cues.passX.begin(), cues.passX.end(), std::make_pair(x, -(1 << 30)));
            for (; it != cues.passX.end() && it->first <= x + 600.f; ++it) {
                World::PortalCluster const& c = clusterOf(cues, it->second);
                if (!prepaid(c) || c.approachX > x + 60.f) continue;
                if (out.n < 8) out.push_back({it->second, c.approachX <= x && clusterOwed(g, c)});
            }
        }

        // what a state resumed inside cluster windows already owes
        int prepaidAt(Game const& g, Cues const& cues, SearchOptions const& opt) {
            SmallVec<std::pair<int, bool>, 8> open;
            openClusters(g, g.p1.x, cues, open);
            int cost = 0;
            for (auto const& e : open)
                if (e.second) cost += opt.missedPortalCost;
            return cost;
        }

        // cost change of the prepaid portal clusters this tick
        int portalPrepay(Game const& g, float prevX, Cues const& cues, SearchOptions const& opt,
                         SmallVec<std::pair<int, bool>, 8> const& open) {
            if (g.p1.isSideways || cues.approachX.empty()) return 0;
            float const nowX = g.p1.x;
            int cost = 0;
            for (auto const& e : open) {
                World::PortalCluster const& c = clusterOf(cues, e.first);
                bool const owedNow = c.approachX <= nowX && clusterOwed(g, c);
                cost += (owedNow ? opt.missedPortalCost : 0) - (e.second ? opt.missedPortalCost : 0);
            }
            if (!(prevX < nowX)) return cost;
            auto it = std::upper_bound(cues.approachX.begin(), cues.approachX.end(),
                                       std::make_pair(prevX + 60.f, 1 << 30));
            for (; it != cues.approachX.end() && it->first <= nowX; ++it) {
                bool inOpen = false;
                for (auto const& e : open) {
                    if (e.first == it->second) {
                        inOpen = true;
                        break;
                    }
                }
                if (!inOpen && clusterOwed(g, clusterOf(cues, it->second))) cost += opt.missedPortalCost;
            }
            return cost;
        }

        // cost of the portal clusters passed without being taken
        int missedPortalsCost(Game const& g, float prevX, Cues const& cues, SearchOptions const& opt, int& speedMisses,
                              bool outside) {
            speedMisses = 0;
            g_portalMisses = 0;
            if (cues.passX.empty()) return 0;
            if (g.p1.isSideways) return 0;
            float const nowX = g.p1.x;
            if (!(prevX < nowX)) return 0;
            auto it = std::upper_bound(cues.passX.begin(), cues.passX.end(), std::make_pair(prevX, 1 << 30));
            int cost = 0;
            for (; it != cues.passX.end() && it->first <= nowX; ++it) {
                int const k = it->second;
                World::PortalCluster const& c =
                        k >= 0 ? (*cues.clusters)[static_cast<size_t>(k)] : cues.extra[static_cast<size_t>(-k - 1)];
                bool taken = false;
                bool anyActive = false;
                bool anyEffect = false;
                for (int m : c.members) {
                    auto const* state = g.osFind(m);
                    if (state && (state->activatedByPlayer1 || state->activatedByPlayer2 || state->activated)) {
                        taken = true;
                        break;
                    }
                    if (g.trig.on && trigActivatedBit(g, m)) {
                        taken = true;
                        break;
                    }
                    if (!const_cast<Game&>(g).obj(m).disabled()) {
                        anyActive = true;
                        if (!portalNoOp(g, g.world->objs[static_cast<size_t>(m)])) anyEffect = true;
                    }
                }
                if (!taken && anyActive && !anyEffect && !c.speed && !c.learned && !g.isDualMode) continue;
                bool const isPrepaid = prepaid(c) && !cues.approachX.empty();
                if (taken || !anyActive) continue;
                float yLo = c.yLo;
                float yHi = c.yHi;
                if (c.dyn) {
                    yLo = 1e30f;
                    yHi = -1e30f;
                    for (int m : c.members) {
                        GObj const o = const_cast<Game&>(g).obj(m);
                        if (o.disabled()) continue;
                        Rect const r = o.getObjectRect();
                        Rect const startRect = o.o->rect;
                        if (std::fabs((r.x + r.w * 0.5f) - (startRect.x + startRect.w * 0.5f)) > 90.f) continue;
                        yLo = std::min(yLo, r.y);
                        yHi = std::max(yHi, r.y + r.h);
                    }
                    if (!(yLo <= yHi)) continue;
                }
                float const y = g.p1.y;
                float const dy = y < yLo ? yLo - y : (y > yHi ? y - yHi : 0.f);
                if (c.learned)
                    cost += opt.missedSpeedCost;
                else if (c.speed) {
                    bool const ground = !(g.p1.isShip || g.p1.isBird || g.p1.isDart || g.p1.isSwing);
                    bool otherFloor = opt.speedSepGround && ground && dy > 0.f &&
                                      cues.structure.separated(it->first, y, yLo, yHi);
                    if (otherFloor && y < yLo && cues.structure.freeAt(it->first, y).first < -1e29f) otherFloor = false;
                    if (otherFloor)
                        for (int m : c.members) {
                            int const id = g.world->objs[static_cast<size_t>(m)].id;
                            float const speed = id == ObjId::SpeedSlow      ? 0.7f
                                                : id == ObjId::SpeedFast    ? 1.1f
                                                : id == ObjId::SpeedFaster  ? 1.3f
                                                : id == ObjId::SpeedFastest ? 1.6f
                                                                            : 0.9f;
                            if (speed < g.p1.playerSpeed - 0.01f) {
                                otherFloor = false;
                                break;
                            }
                        }
                    if (dy <= 900.f && !otherFloor) {
                        cost += opt.missedSpeedCost;
                        ++speedMisses;
                        g_lastMissX = it->first;
                    }
                } else if (isPrepaid) {
                    bool reach = dy <= portalReach(g.p1, c) || outside;
                    if (!reach && !cues.structure.empty() && dy > 0.f) {
                        Player const& player = g.p1;
                        if (player.isShip || player.isBird || player.isDart || player.isSwing) {
                            bool hasExitPortal = false;
                            for (int m : c.members) {
                                if (flyExitType(player, g.world->objs[static_cast<size_t>(m)].type)) {
                                    hasExitPortal = true;
                                    break;
                                }
                            }
                            if (hasExitPortal) {
                                auto const portalSpace = cues.structure.freeAt(it->first, 0.5f * (yLo + yHi));
                                if (y >= portalSpace.first && y <= portalSpace.second) reach = true;
                                if (!reach && g.isFreeMode) {
                                    auto const playerSpace = cues.structure.freeAt(it->first, y);
                                    bool const roof =
                                            playerSpace.second > 1e29f && playerSpace.first > -1e29f && y > yHi;
                                    bool const under = playerSpace.first < -1e29f && playerSpace.second < 1e29f &&
                                                       y < yLo && y > 705.f;
                                    if (roof || under) reach = true;
                                }
                            }
                        }
                    }
                    if (reach && opt.portalDebt) {
                        ++speedMisses;
                        ++g_portalMisses;
                        g_lastMissX = -it->first;
                    }
                } else if (dy <= 90.f || outside)
                    cost += opt.missedPortalCost;
            }
            return cost;
        }

        // number of flying mode exits passed this tick without being taken while in reach
        int flyExitMisses(Game const& g, float prevX, Cues const& cues, bool outside, bool waveOnly, float maxReach) {
            if (cues.passX.empty() || g.p1.isSideways || g.isDualMode || g.isPlatformer) return 0;
            Player const& p = g.p1;
            if (!(p.isShip || p.isBird || p.isDart || p.isSwing)) return 0;
            if (waveOnly && !p.isDart) return 0;
            float const nowX = p.x;
            if (!(prevX < nowX)) return 0;
            int n = 0;
            auto it = std::upper_bound(cues.passX.begin(), cues.passX.end(), std::make_pair(prevX, 1 << 30));
            for (; it != cues.passX.end() && it->first <= nowX; ++it) {
                World::PortalCluster const& c = clusterOf(cues, it->second);
                if (c.speed || c.learned) continue;
                bool exit = false;
                bool taken = false;
                float lo = 1e30f;
                float hi = -1e30f;
                for (int m : c.members) {
                    auto const* state = g.osFind(m);
                    if (state && (state->activatedByPlayer1 || state->activatedByPlayer2 || state->activated)) {
                        taken = true;
                        break;
                    }
                    GObj const o = const_cast<Game&>(g).obj(m);
                    if (o.disabled()) continue;
                    if (!flyExitType(p, g.world->objs[static_cast<size_t>(m)].type)) continue;
                    exit = true;
                    Rect const r = o.getObjectRect();
                    lo = std::min(lo, r.y);
                    hi = std::max(hi, r.y + r.h);
                }
                if (taken || !exit) continue;
                float const dy = p.y < lo ? lo - p.y : (p.y > hi ? p.y - hi : 0.f);
                if (!(dy <= std::min(portalReach(p, c), maxReach) || outside)) continue;
                if (dy > 0.f && cues.structure.separated(it->first, p.y, lo, hi)) continue;
                ++n;
            }
            return n;
        }

        // cost of orbs touched last tick (prevRings1/2), left this tick without being taken
        int missedOrbsCost(Game const& g, RingList const& prevRings1, RingList const& prevRings2,
                           SearchOptions const& opt) {
            int cost = 0;
            auto check = [&](RingList const& prev, Player const& p) {
                if (p.isDart) return;
                for (int uid : prev) {
                    if (p.touchingRings.contains(uid)) continue;
                    int const idx = g.world->indexOfUid(uid);
                    if (idx < 0) continue;
                    Obj const& o = g.world->objs[static_cast<size_t>(idx)];
                    if (o.id == ObjId::GravityOrb || o.id == ObjId::ToggleOrb || o.id == ObjId::ToggleBlock) continue;
                    auto const* state = g.osFind(idx);
                    if (state && (state->activatedByPlayer1 || state->activatedByPlayer2 || state->activated)) continue;
                    cost += opt.missedOrbCost;
                }
            };
            check(prevRings1, g.p1);
            if (g.isDualMode) check(prevRings2, g.p2);
            return cost;
        }

    }
}
