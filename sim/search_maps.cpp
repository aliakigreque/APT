// Level maps built once per search: state keys, empty cells, level extent, void hull, presence and structure
// maps, and the bands and cues the costs read.

#include "search_internal.hpp"

namespace ap::sim {
    namespace search_impl {
        //key of a state: rounded x, y and y velocity in steps, mode flags, and a hash of everything else
        Key makeKey(Game const& g, float quantY, float quantV) {
            Player const& p = g.p1;
            Key k{};
            k.x = static_cast<int32_t>(std::lround(p.x));
            k.y = static_cast<int32_t>(std::floor(p.y / quantY));
            k.v = static_cast<int32_t>(std::floor(p.yVelocity / quantV));
            uint32_t flags = 0;
            auto bit = [&](bool on, int index) {
                if (on) flags |= 1u << index;
            };
            bit(p.isShip, 0);
            bit(p.isBall, 1);
            bit(p.isBird, 2);
            bit(p.isDart, 3);
            bit(p.isRobot, 4);
            bit(p.isSpider, 5);
            bit(p.isSwing, 6);
            bit(p.isUpsideDown, 7);
            bit(p.isOnGround, 8);
            bit(p.isOnGround2, 9);
            bit(p.jumpBuffered, 10);
            bit(p.stateRingJump, 11);
            bit(p.maybeIsBoosted, 12);
            bit(p.isAccelerating, 13);
            bit(p.isDashing, 14);
            bit(p.touchedPad, 15);
            bit(p.vehicleSize != 1.f, 16);
            bit(p.holdingJump, 17);
            bit(p.isOnSlope, 18);
            bit(p.wasOnSlope, 19);
            bit(p.maybeIsFalling, 20);
            bit(g.isDualMode, 21);
            bit(p.padRingRelated, 22);
            bit(p.isOnGround3, 23);
            bit(p.isSideways, 27);
            bit(p.isGoingLeft, 28);
            int speedIdx = p.playerSpeed == 0.7f   ? 0
                           : p.playerSpeed == 0.9f ? 1
                           : p.playerSpeed == 1.1f ? 2
                           : p.playerSpeed == 1.3f ? 3
                                                   : 4;
            flags |= static_cast<uint32_t>(speedIdx) << 24;
            k.flags = flags;
            k.extra = static_cast<uint32_t>(g.osSize()) * 131u + static_cast<uint32_t>(p.touchingRings.size()) * 7u +
                      static_cast<uint32_t>(g.triggeredIDs.size()) * 1013u +
                      static_cast<uint32_t>(std::lround(p.gravityMod * 10.f)) * 65537u;
            k.extra ^= static_cast<uint32_t>(g.currentChannel) * 2246822519u + g.reversedChannels.hash() * 97u;
            if (g.isDualMode) {
                Player const& p2 = g.p2;
                k.extra ^= static_cast<uint32_t>(std::floor(p2.y / quantY)) * 2654435761u;
                k.extra ^= static_cast<uint32_t>(std::floor(p2.yVelocity / quantV)) * 40503u;
                uint32_t flags2 = 0;
                auto bit2 = [&](bool on, int index) {
                    if (on) flags2 |= 1u << index;
                };
                bit2(p2.isShip, 0);
                bit2(p2.isBall, 1);
                bit2(p2.isBird, 2);
                bit2(p2.isDart, 3);
                bit2(p2.isRobot, 4);
                bit2(p2.isSpider, 5);
                bit2(p2.isSwing, 6);
                bit2(p2.isUpsideDown, 7);
                bit2(p2.isOnGround, 8);
                bit2(p2.isOnGround2, 9);
                bit2(p2.jumpBuffered, 10);
                bit2(p2.stateRingJump, 11);
                bit2(p2.maybeIsBoosted, 12);
                bit2(p2.isAccelerating, 13);
                bit2(p2.isDashing, 14);
                bit2(p2.touchedPad, 15);
                bit2(p2.vehicleSize != 1.f, 16);
                bit2(p2.isOnSlope, 17);
                bit2(p2.wasOnSlope, 18);
                bit2(p2.maybeIsFalling, 19);
                bit2(p2.padRingRelated, 20);
                bit2(p2.isOnGround3, 21);
                bit2(std::fabs(p2.x - p.x) > 0.5f, 22);
                bit2(g.p2Btn, 23);
                bit2(p2.holdingJump, 24);
                bit2(g.p1Btn, 25);
                k.extra ^= flags2 * 2246822519u + static_cast<uint32_t>(p2.touchingRings.size()) * 3266489917u;
            }
            return k;
        }

        // fills the extent from the level objects
        void buildExtent(World const& w, Extent& extent, float groundY) {
            float minX = 1e30f;
            float maxX = -1e30f;
            extent.groundY = groundY;
            auto gameplay = [](Obj const& o) {
                return o.type != Decoration && !o.trigger && !o.disabled && o.type != Modifier &&
                       o.type != CollisionObject && o.rect.w < 3000.f && o.rect.h < 3000.f;
            };
            for (auto const& o : w.objs)
                if (gameplay(o)) {
                    minX = std::min(minX, o.rect.x);
                    maxX = std::max(maxX, o.rect.x + o.rect.w);
                }
            if (!(minX < maxX)) return;
            extent.x0 = minX - 300.f;
            size_t const numCols = static_cast<size_t>((maxX - minX + 600.f) / 30.f) + 1;
            if (numCols > 20000000) return;
            std::vector<float> colLo(numCols, 1e30f);
            std::vector<float> colHi(numCols, -1e30f);
            for (auto const& o : w.objs) {
                if (!gameplay(o)) continue;
                size_t const firstCol = static_cast<size_t>(std::max(0.f, (o.rect.x - extent.x0) / 30.f));
                size_t const lastCol = std::min(
                        numCols - 1, static_cast<size_t>(std::max(0.f, (o.rect.x + o.rect.w - extent.x0) / 30.f)));
                float const slack = o.movable ? 150.f : 0.f;
                for (size_t c = firstCol; c <= lastCol; ++c) {
                    colLo[c] = std::min(colLo[c], o.rect.y - slack);
                    colHi[c] = std::max(colHi[c], o.rect.y + o.rect.h + slack);
                }
            }
            extent.lo.assign(numCols, 1e30f);
            extent.hi.assign(numCols, -1e30f);
            for (size_t c = 0; c < numCols; ++c) {
                for (size_t k = (c >= 5 ? c - 5 : 0); k <= std::min(numCols - 1, c + 5); ++k) {
                    extent.lo[c] = std::min(extent.lo[c], colLo[k]);
                    extent.hi[c] = std::max(extent.hi[c], colHi[k]);
                }
            }
        }

        // fills the hull, objects inside the excluded boxes (x0, x1, y0, y1) don't count
        void buildVoidHull(World const& w, VoidHull& hull, std::vector<std::array<float, 4>> const& excluded) {
            float minX = 1e30f;
            float maxX = -1e30f;
            auto collidable = [&excluded](Obj const& o) {
                if (!(o.type != Decoration && !o.trigger && o.type != Modifier && o.type != CollisionObject &&
                      o.rect.w < 3000.f && o.rect.h < 3000.f))
                    return false;
                for (auto const& box : excluded) {
                    if (o.rect.maxX() >= box[0] && o.rect.x <= box[1] && o.rect.maxY() >= box[2] && o.rect.y <= box[3])
                        return false;
                }
                return true;
            };
            for (auto const& o : w.objs)
                if (collidable(o)) {
                    minX = std::min(minX, o.rect.x);
                    maxX = std::max(maxX, o.rect.maxX());
                }
            if (!(minX < maxX) || maxX - minX > 3.0e6f) return;
            hull.x0 = minX;
            size_t const n = static_cast<size_t>((maxX - minX) / VoidHull::kCol) + 2;
            std::vector<float> colTop(n, -1e30f);
            std::vector<float> colBot(n, 1e30f);
            std::vector<char> nearTeleport(n, 0);
            std::vector<char> hasStatic(n, 0);
            for (auto const& o : w.objs) {
                if (o.cls == 1 && (o.id == ObjId::TeleportTrigger || o.id == ObjId::TeleportArea)) {
                    int const col = static_cast<int>(std::floor((o.x - minX) / VoidHull::kCol));
                    for (int k = std::max(0, col - 100); k <= col && static_cast<size_t>(k) < n; ++k)
                        nearTeleport[static_cast<size_t>(k)] = 1;
                    continue;
                }
                if (!collidable(o)) continue;
                if (!o.movable) {
                    int const staticC0 = std::max(0, static_cast<int>(std::floor((o.rect.x - minX) / VoidHull::kCol)));
                    int const staticC1 =
                            std::min(static_cast<int>(n) - 1,
                                     static_cast<int>(std::floor((o.rect.maxX() - minX) / VoidHull::kCol)));
                    for (int c = staticC0; c <= staticC1; ++c) hasStatic[static_cast<size_t>(c)] = 1;
                }
                int const c0 = std::max(0, static_cast<int>(std::floor((o.rect.x - minX) / VoidHull::kCol)));
                int const c1 = std::min(static_cast<int>(n) - 1,
                                        static_cast<int>(std::floor((o.rect.maxX() - minX) / VoidHull::kCol)));
                for (int c = c0; c <= c1; ++c) {
                    colTop[static_cast<size_t>(c)] = std::max(colTop[static_cast<size_t>(c)], o.rect.maxY());
                    colBot[static_cast<size_t>(c)] = std::min(colBot[static_cast<size_t>(c)], o.rect.y);
                }
            }
            {
                std::vector<char> carriedGroups(static_cast<size_t>(w.maxGroup) + 2, 0);
                bool any = false;
                for (auto const& o : w.objs)
                    if (o.cls == 1 && o.id == ObjId::MoveTrigger && (o.tf(Obj::TF_LockPX) || o.tf(Obj::TF_LockCX)) &&
                        o.targetGroup > 0 && o.targetGroup <= w.maxGroup) {
                        carriedGroups[static_cast<size_t>(o.targetGroup)] = 1;
                        any = true;
                    }
                if (any)
                    for (auto const& o : w.objs) {
                        if (!collidable(o)) continue;
                        bool carried = false;
                        for (short group : o.groups) {
                            if (group > 0 && group <= w.maxGroup && carriedGroups[static_cast<size_t>(group)]) {
                                carried = true;
                                break;
                            }
                        }
                        if (!carried) continue;
                        int const c0 = std::max(0, static_cast<int>(std::floor((o.rect.x - minX) / VoidHull::kCol)));
                        for (size_t c = static_cast<size_t>(c0); c < n; ++c) {
                            colTop[c] = std::max(colTop[c], o.rect.maxY());
                            colBot[c] = std::min(colBot[c], o.rect.y);
                        }
                    }
            }
            int const ahead = static_cast<int>(VoidHull::kAhead / VoidHull::kCol);
            hull.top.assign(n, 1e30f);
            hull.bot.assign(n, -1e30f);
            std::deque<size_t> topQueue;
            std::deque<size_t> botQueue;
            std::deque<size_t> teleQueue;
            std::deque<size_t> staticQueue;
            for (size_t ci = n; ci-- > 0;) {
                if (hasStatic[ci]) staticQueue.push_back(ci);
                while (!staticQueue.empty() && staticQueue.front() > ci + static_cast<size_t>(ahead))
                    staticQueue.pop_front();
                while (!topQueue.empty() && colTop[topQueue.back()] <= colTop[ci]) topQueue.pop_back();
                topQueue.push_back(ci);
                while (!botQueue.empty() && colBot[botQueue.back()] >= colBot[ci]) botQueue.pop_back();
                botQueue.push_back(ci);
                if (nearTeleport[ci]) teleQueue.push_back(ci);
                while (topQueue.front() > ci + static_cast<size_t>(ahead)) topQueue.pop_front();
                while (botQueue.front() > ci + static_cast<size_t>(ahead)) botQueue.pop_front();
                while (!teleQueue.empty() && teleQueue.front() > ci + static_cast<size_t>(ahead)) teleQueue.pop_front();
                if (!teleQueue.empty() || staticQueue.empty() || colTop[topQueue.front()] < -1e29f) continue;
                hull.top[ci] = colTop[topQueue.front()];
                hull.bot[ci] = colBot[botQueue.front()];
            }
        }

        // fills the empty grid: breadth first distance from the gameplay objects, capped at kEmptyCells
        void buildEmpty(World const& w, EmptyGrid& grid) {
            float minX = 1e30f;
            float maxX = -1e30f;
            float minY = 1e30f;
            float maxY = -1e30f;
            auto gameplay = [](Obj const& o) {
                return o.type != Decoration && !o.trigger && !o.disabled && o.type != Modifier &&
                       o.type != CollisionObject && o.rect.w < 3000.f && o.rect.h < 3000.f;
            };
            for (auto const& o : w.objs) {
                if (!gameplay(o)) continue;
                minX = std::min(minX, o.rect.x);
                maxX = std::max(maxX, o.rect.maxX());
                minY = std::min(minY, o.rect.y);
                maxY = std::max(maxY, o.rect.maxY());
            }
            if (!(minX < maxX)) return;
            float const margin = EmptyGrid::kCell * 12;
            grid.x0 = minX - margin;
            grid.y0 = minY - margin;
            grid.nx = static_cast<int>((maxX - minX + 2 * margin) / EmptyGrid::kCell) + 1;
            grid.ny = static_cast<int>((maxY - minY + 2 * margin) / EmptyGrid::kCell) + 1;
            if (static_cast<long>(grid.nx) * grid.ny > 60000000L) {
                grid.nx = 0;
                return;
            }
            grid.d.assign(static_cast<size_t>(grid.nx) * static_cast<size_t>(grid.ny), 255);
            std::vector<int> frontier;
            std::vector<int> nextFrontier;
            for (auto const& o : w.objs) {
                if (!gameplay(o)) continue;
                int const cx0 = static_cast<int>((o.rect.x - grid.x0) / EmptyGrid::kCell);
                int const cx1 = static_cast<int>((o.rect.maxX() - grid.x0) / EmptyGrid::kCell);
                int const cy0 = static_cast<int>((o.rect.y - grid.y0) / EmptyGrid::kCell);
                int const cy1 = static_cast<int>((o.rect.maxY() - grid.y0) / EmptyGrid::kCell);
                for (int cy = std::max(0, cy0); cy <= std::min(grid.ny - 1, cy1); ++cy)
                    for (int cx = std::max(0, cx0); cx <= std::min(grid.nx - 1, cx1); ++cx) {
                        size_t const i =
                                static_cast<size_t>(cy) * static_cast<size_t>(grid.nx) + static_cast<size_t>(cx);
                        if (grid.d[i] != 0) {
                            grid.d[i] = 0;
                            frontier.push_back(static_cast<int>(i));
                        }
                    }
            }
            for (int dist = 1; dist <= kEmptyCells && !frontier.empty(); ++dist) {
                nextFrontier.clear();
                for (int i : frontier) {
                    int const cx = i % grid.nx;
                    int const cy = i / grid.nx;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            int const x = cx + dx;
                            int const y = cy + dy;
                            if (x < 0 || y < 0 || x >= grid.nx || y >= grid.ny) continue;
                            size_t const j =
                                    static_cast<size_t>(y) * static_cast<size_t>(grid.nx) + static_cast<size_t>(x);
                            if (grid.d[j] > dist) {
                                grid.d[j] = static_cast<uint8_t>(dist);
                                nextFrontier.push_back(static_cast<int>(j));
                            }
                        }
                }
                std::swap(frontier, nextFrontier);
            }
            for (auto& v : grid.d)
                if (v > kEmptyCells) v = kEmptyCells;
        }

        // fills the play bands: 10th and 90th percentile of object heights per kBandW slice
        void buildBands(World const& w, std::vector<Band>& out) {
            std::vector<std::vector<float>> heights;
            for (auto const& o : w.objs) {
                if (o.type == Decoration || o.trigger) continue;
                if (o.disabled) continue;
                int const b = static_cast<int>(std::floor(static_cast<float>(o.posX) / kBandW));
                if (b < 0 || b > 100000) continue;
                if (static_cast<size_t>(b) >= heights.size()) heights.resize(static_cast<size_t>(b) + 1);
                heights[static_cast<size_t>(b)].push_back(static_cast<float>(o.posY));
            }
            out.assign(heights.size(), Band{});
            for (size_t b = 0; b < heights.size(); ++b) {
                auto& v = heights[b];
                if (v.size() < 10) continue;
                std::sort(v.begin(), v.end());
                out[b] = Band{v[v.size() / 10], v[(v.size() * 9) / 10], static_cast<int>(v.size())};
            }
        }

        // cost of a speed portal taken late this tick (entered from the top or bottom instead of its front edge)
        int lateSpeedCost(Game const& g, float prevX, Cues const& cues, SearchOptions const& opt) {
            if (g.p1.isSideways || g.p1.isGoingLeft) return 0;
            Rect const playerBox = playerRect(g.p1);
            float const dx = g.p1.x - prevX;
            auto it = std::lower_bound(cues.passX.begin(), cues.passX.end(), std::make_pair(prevX, -(1 << 30)));
            for (; it != cues.passX.end() && it->first <= g.p1.x + 400.f; ++it) {
                int const k = it->second;
                World::PortalCluster const& c =
                        k >= 0 ? (*cues.clusters)[static_cast<size_t>(k)] : cues.extra[static_cast<size_t>(-k - 1)];
                if (!c.speed) continue;
                for (int m : c.members) {
                    auto const* state = g.osFind(m);
                    if (!state || !state->activated) continue;
                    Rect const r = const_cast<Game&>(g).obj(m).getObjectRect();
                    if (!(r.x <= playerBox.x + playerBox.w && playerBox.x <= r.x + r.w)) continue;
                    float const late = (playerBox.x + playerBox.w) - r.x - std::max(dx, 0.f);
                    if (late > opt.lateSpeedGap)
                        return std::min(opt.lateSpeedCost, 2 + 3 * static_cast<int>(late - opt.lateSpeedGap));
                    return 0;
                }
            }
            return 0;
        }

    }
}
