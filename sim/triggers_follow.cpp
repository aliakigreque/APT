// Follow, player-follow and advanced follow triggers, the set of groups that depend on the player, and the transform helpers (GJBaseGameLayer::processAdvancedFollowActions, processFollowActions).

#include "triggers_internal.hpp"

namespace ap::sim {
    namespace trigger_impl {
        // one step of follow player Y (1814): objects of the target group move toward player 1's y
        void playerFollowStep(Game& g, FollowYCmd const& c, float dt) {
            ++g_trigCounters[3];
            World const& w = *g.world;
            TrigState& T = g.trig;
            double const frames = static_cast<double>(dt * 60.f);
            double factor = frames * c.fySpeed;
            if (factor > 1.0) factor = 1.0;
            if (factor < 0.0) factor = 0.0;
            float const maxStep = static_cast<float>(frames * c.fyMax);
            float playerY;
            if (0.f < static_cast<float>(c.fyDelay)) {
                int delaySlots = static_cast<int>(std::floor(static_cast<float>(c.fyDelay) / 0.01f));
                if (delaySlots > 199) delaySlots = 199;
                if (delaySlots < 0) delaySlots = 0;
                int slot = T.yHistIdx % 200 - delaySlots;
                if (slot < 0) slot += 200;
                playerY = T.yHist.empty() ? g.p1.y : T.yHist[static_cast<size_t>(slot)];
            } else
                playerY = g.p1.y;
            decltype(&g.trig.wd().objDyn) objDyn = nullptr;
            size_t hint = 0;
            for (int idx : w.moversOfGroup(c.target)) {
                ObjDyn* dynPtr = nullptr;
                if (!isPO(g, idx)) {
                    dynDep(2);
                    fyCheck(idx, 1);
                    if (!objDyn) objDyn = &T.wd().objDyn;
                    dynPtr = objDyn->findHint(idx, hint);
                }
                if (!dynPtr) {
                    dynPtr = &dynEntry(g, idx);
                    if (objDyn) objDyn = &T.wd().objDyn;
                }
                ObjDyn& d = *dynPtr;
                stampLast(g, d);
                if (playerY == 0.f) continue;
                float gap = ((playerY + static_cast<float>(c.fyOffset)) - static_cast<float>(d.y)) *
                            static_cast<float>(factor);
                if (gap == 0.f) continue;
                float step = gap;
                if (0.f < maxStep) {
                    if (gap <= maxStep) {
                        if (-maxStep <= gap)
                            step = gap;
                        else
                            step = -maxStep;
                    } else
                        step = maxStep;
                }
                d.y = static_cast<double>(step) + d.y;
            }
            if (objDyn) T.wd();
        }

        // drops dyn entries of objects far behind the player that can't come back, every 64 ticks
        void pruneDyn(Game& g) {
            if (g.p1.isGoingLeft || (g.tick & 63) != 0 || g.trig.rd().objDyn.size() <= 64) return;
            World const& w = *g.world;
            float const xBehind = g.p1.x - 1200.f;
            auto const& cmds = g.trig.r().M().cmds;
            auto mayMove = [&](int idx, float px, float py) {
                float forward = 0.f;
                float reach = px;
                for (short gr : w.objs[static_cast<size_t>(idx)].groups) {
                    if (gr <= 0 || gr > w.maxGroup) continue;
                    if (w.groupLastMoveX[static_cast<size_t>(gr)] >= g.p1.x) return true;
                    forward += w.groupFutureFwd(gr, g.p1.x);
                    int const first = w.grFirst[static_cast<size_t>(gr)];
                    int const last = w.grFirst[static_cast<size_t>(gr) + 1];
                    for (int k = static_cast<int>(std::lower_bound(w.grX.begin() + first, w.grX.begin() + last, g.p1.x) - w.grX.begin()); 
                    k < last; ++k) {
                        int const centerGroup = w.grCenter[static_cast<size_t>(k)];
                        int const centerIdx = centerGroup > 0 ? w.mainObjectOf(centerGroup) : -1;
                        if (centerIdx < 0) continue;
                        if (w.groupLastMoveX[static_cast<size_t>(centerGroup)] >= g.p1.x) return true;
                        auto const& center = objAt(g, centerIdx);
                        float const cx = static_cast<float>(center.posX);
                        float const cy = static_cast<float>(center.posY);
                        float centerForward = 0.f;
                        for (short centerGr : w.objs[static_cast<size_t>(centerIdx)].groups) {
                            if (centerGr > 0 && centerGr <= w.maxGroup)
                                centerForward += w.groupFutureFwd(centerGr, g.p1.x);
                        }
                        reach = std::max(reach, cx + std::hypot(px - cx, py - cy) + centerForward);
                    }
                    for (auto const& c : cmds)
                        if (!c.finished && c.target == gr) return true;
                }
                return reach + forward >= xBehind;
            };
            auto prunable = [&](auto const& e) {
                if (w.centerLike[static_cast<size_t>(e.first)]) return false;
                auto const& obj = objAt(g, e.first);
                float const px = std::max(static_cast<float>(e.second.x), static_cast<float>(obj.posX));
                if (!(px < xBehind)) return false;
                return !mayMove(e.first, static_cast<float>(e.second.x), static_cast<float>(e.second.y)) &&
                        !mayMove(e.first, static_cast<float>(obj.posX), static_cast<float>(obj.posY));
            };
            bool any = false;
            for (auto const& e : g.trig.rd().objDyn) {
                if (prunable(e)) {
                    any = true;
                    break;
                }
            }
            if (!any) return;
            auto& entries = g.trig.wd().objDyn.items;
            entries.erase(std::remove_if(entries.begin(), entries.end(), [&](auto const& e) { return prunable(e); }), entries.end());
        }

        // current position of the followed key (-1 p1, -2 p2, -3 camera, else a group), false if unknown or (0, 0)
        bool afTargetPos(Game const& g, int key, float& x, float& y) {
            if (tl_advKeys && key > 0) {
                for (auto const& saved : *tl_advKeys) {
                    if (saved.key == key) {
                        x = saved.x;
                        y = saved.y;
                        return saved.ok;
                    }
                }
            }
            if (key == -1) {
                x = g.p1.x;
                y = g.p1.y;
            } else if (key == -2) {
                if (g.p2Origin) {
                    x = 0.f;
                    y = 0.f;
                } else {
                    x = g.p2.x;
                    y = g.p2.y;
                }
            } else if (key == -3) {
                x = g.cam.px;
                y = g.cam.py;
            } else {
                int const mainObj = g.world->mainObjectOf(key);
                if (mainObj < 0) return false;
                if (trigAreaOff(g, mainObj)) tl_sideEffect = true;
                double dx, dy;
                trigRealPos(g, mainObj, dx, dy);
                x = static_cast<float>(dx);
                y = static_cast<float>(dy);
            }
            return !(x == 0.f && y == 0.f);
        }

        // position of the key delay seconds ago, interpolated in its history ring (GJBaseGameLayer::getSavedPosition)
        bool afSavedPos(Game const& g, int key, float delay, float& x, float& y) {
            if (!(0.f < delay)) return afTargetPos(g, key, x, y);
            World const& w = *g.world;
            TrigState const& T = g.trig;
            x = 0.f;
            y = 0.f;
            double const backD = static_cast<double>(delay) / 0.004166666666666667 - static_cast<double>(static_cast<float>(T.afU6 - T.afU5) / 1000.f);
            float const back = static_cast<float>(backD > 0.0 ? backD : 0.0);
            int const steps = static_cast<int>(std::ceil(back));
            if (steps >= T.afN) return false;
            int obj = -1;
            if (key > 0) {
                obj = w.mainObjectOf(key);
                if (obj < 0) return false;
            }
            World::AfRing const* ring = nullptr;
            for (auto const& r : w.afRings) {
                if (key > 0 ? r.obj == obj : r.key == key) {
                    ring = &r;
                    break;
                }
            }
            if (!ring || T.afRing.size() < static_cast<size_t>(w.afRingFloats)) return false;
            int i = T.afN % ring->size - steps;
            if (i < 0) i += ring->size;
            int const j = i + 1 < ring->size ? i + 1 : 0;
            float const* older = &T.afRing[static_cast<size_t>(ring->base + i) * 2];
            float const* newer = &T.afRing[static_cast<size_t>(ring->base + j) * 2];
            float const t = static_cast<float>(steps) - back;
            x = older[0] + (newer[0] - older[0]) * t;
            y = older[1] + (newer[1] - older[1]) * t;
            return !(x == 0.f && y == 0.f);
        }

        //moves one object for advanced follow, it stays in its grid cell and is listed in afObjs instead
        void afMoveObj(Game& g, int idx, double dx, double dy) {
            Obj const& o = objAt(g, idx);
            ObjDyn& d = dynEntry(g, idx);
            stampLast(g, d);
            if (dx != 0.0 && !o.lockX) d.x += dx;
            if (dy != 0.0) d.y += dy;
            if (isPO(g, idx)) {
                if (!g.trig.pv || !std::binary_search(g.trig.pv->afObjs.begin(), g.trig.pv->afObjs.end(), idx)) {
                    auto& list = g.trig.wp().afObjs;
                    list.insert(std::lower_bound(list.begin(), list.end(), idx), idx);
                }
                return;
            }
            auto const& afObjs = g.trig.rd().afObjs;
            if (!std::binary_search(afObjs.begin(), afObjs.end(), idx)) {
                auto& list = g.trig.wd().afObjs;
                list.insert(std::lower_bound(list.begin(), list.end(), idx), idx);
            }
        }

        // start velocity: direction (90 - dir) degrees, turned by a reference (its movement or the way to it)
        void afStartVel(Game& g, int idx, int ref1, int ref2, float dir, float speed, float& vx, float& vy) {
            float angle = (90.f - dir) * 0.017453292f;
            float refX = 0.f;
            float refY = 0.f;
            int const ref1Result = ref1 > 0 ? tryGetObjectRef(g, ref1) : kRefNone;
            int const ref1Obj = ref1Result >= 0 ? ref1Result : -1;
            if (ref1Obj >= 0) {
                double px, py;
                trigRealPos(g, ref1Obj, px, py);
                float lx, ly;
                trigLastPos(g, ref1Obj, lx, ly);
                refX = lx - static_cast<float>(px);
                refY = ly - static_cast<float>(py);
            } else {
                int const ref2Result = (ref1Result == kRefNone && ref2 > 0) ? tryGetObjectRef(g, ref2) : kRefNone;
                int const ref2Obj = ref2Result >= 0 ? ref2Result : -1;
                if (ref2Obj >= 0) {
                    double ax, ay, bx, by;
                    trigRealPos(g, idx, ax, ay);
                    trigRealPos(g, ref2Obj, bx, by);
                    refX = static_cast<float>(ax) - static_cast<float>(bx);
                    refY = static_cast<float>(ay) - static_cast<float>(by);
                }
            }
            if (refX != 0.f || refY != 0.f)
                angle += static_cast<float>(static_cast<double>(std::atan2(refY, refX)) + 1.5707963267948966);
            vx = gcos(angle) * speed;
            vy = gsin(angle) * speed;
        }

        // runs advanced folow instance instIdx for this step, started = only set up the physic state
        void afProcessInstance(Game& g, size_t instIdx, bool started, float dt) {
            World const& w = *g.world;
            unsigned const cmd = g.commandIndex;
            AdvInst const inst = g.trig.r().M().advs[instIdx];
            World::AdvDef const& A = w.advDefs[static_cast<size_t>(inst.def)];
            if (inst.rngOnly) {
                if (started || inst.processed) return;
                for (int idx : w.allOfGroup(inst.target)) {
                    Obj const& o = objAt(g, idx);
                    float targetX, targetY;
                    if (!afSavedPos(g, inst.key, A.delay + afRand(w, o, A.delayV, 0), targetX, targetY)) return;
                    if (A.ignoreDisabled && trigGroupDisabled(g, o)) continue;
                    if (A.range != 0.f || A.rangeV != 0.f) {
                        double objX, objY;
                        trigRealPos(g, idx, objX, objY);
                        float dx = targetX - static_cast<float>(objX);
                        float dy = targetY - static_cast<float>(objY);
                        if (A.xOnly)
                            dy = 0.f;
                        else if (A.yOnly)
                            dx = 0.f;
                        float const dist = std::sqrt(dx * dx + dy * dy);
                        float const range = A.range + afRand(w, o, A.rangeV, 3);
                        if (range != 0.f && range < dist) continue;
                    }
                    tl_sideEffect = true;
                    float const startSpeed = g.rand01() * A.startSpeedV + A.startSpeed;
                    if (startSpeed != 0.f) (void)g.rand01();
                    g.trig.w().Mw().advs[instIdx].processed = 1;
                    return;
                }
                return;
            }
            auto followOne = [&](int idx, World::TargetGroups::Rep const* rep) {
                auto moveAll = [&](double mdx, double mdy) {
                    if (!rep) {
                        afMoveObj(g, idx, mdx, mdy);
                        return;
                    }
                    for (int child : rep->phys) afMoveObj(g, child, mdx, mdy);
                };
                Obj const& o = objAt(g, idx);
                bool const playerObj = isPO(g, idx);
                auto afMap = [&]() -> FlatMap<int, AfPhys>& {
                    if (playerObj) {
                        pRead();
                        return g.trig.wp().af;
                    }
                    return g.trig.wd().afPhys;
                };
                AfPhys* phys = afMap().find(idx);
                if (!phys) {
                    afMap()[idx] = AfPhys();
                    phys = afMap().find(idx);
                }
                if (started) {
                    if (phys->last < static_cast<int>(cmd)) {
                        phys->last = static_cast<int>(cmd);
                        moveAll(0.0, 0.0);
                    }
                    return;
                }
                if (phys->stop >= static_cast<int>(cmd)) return;
                float targetX, targetY;
                if (!afSavedPos(g, inst.key, A.delay + afRand(w, o, A.delayV, 0), targetX, targetY)) return;
                if (A.ignoreDisabled && trigGroupDisabled(g, o)) return;
                double objX, objY;
                trigRealPos(g, idx, objX, objY);
                float dx = targetX - static_cast<float>(objX);
                float dy = targetY - static_cast<float>(objY);
                float const dist = std::sqrt(dx * dx + dy * dy);
                float const maxSpeed = A.maxSpeed + afRand(w, o, A.maxSpeedV, 1);
                float const accel = (A.accel + afRand(w, o, A.accelV, 2)) * 0.01f;
                float const range = A.range + afRand(w, o, A.rangeV, 3);
                if (A.xOnly)
                    dy = 0.f;
                else if (A.yOnly)
                    dx = 0.f;
                if (range != 0.f && range < dist) return;
                if (!g.trig.r().M().advs[instIdx].processed) {
                    tl_sideEffect = true;
                    float const startSpeed = g.rand01() * A.startSpeedV + A.startSpeed;
                    if (startSpeed != 0.f) {
                        float const dir = g.rand01() * A.startDirV + A.startDir;
                        float vx, vy;
                        afStartVel(g, idx, A.startRef, A.dirRef, dir, startSpeed, vx, vy);
                        phys = afMap().find(idx);
                        if (phys->last == 0 || A.startMode == 1) {
                            if (!A.xOnly) phys->vy = vy;
                            if (!A.yOnly) phys->vx = vx;
                        } else if (A.startMode == 2) {
                            if (!A.xOnly) phys->vy += vy;
                            if (!A.yOnly) phys->vx += vx;
                        }
                    }
                    g.trig.w().Mw().advs[instIdx].processed = 1;
                    phys = afMap().find(idx);
                }
                phys->last = static_cast<int>(cmd);
                if (A.once) phys->stop = static_cast<int>(cmd);
                bool shared = true;
                if (A.mode == 2) {
                    shared = false;
                    float const len = std::sqrt(dx * dx + dy * dy);
                    float nx = 1.f;
                    float ny = 0.f;
                    if (len != 0.f) {
                        nx = dx / len;
                        ny = dy / len;
                    }
                    if (phys->dx == 0.f && phys->dy == 0.f) {
                        phys->dx = nx;
                        phys->dy = ny;
                    }
                    float steer = (A.steer + afRand(w, o, A.steerV, 9)) * 0.01f;
                    float const steerLow = (A.steerLow + afRand(w, o, A.steerLowV, 10)) * 0.01f;
                    float const steerHigh = (A.steerHigh + afRand(w, o, A.steerHighV, 11)) * 0.01f;
                    float const speedLow = A.speedLow + afRand(w, o, A.speedLowV, 12);
                    float const speedHigh = A.speedHigh + afRand(w, o, A.speedHighV, 13);
                    float const brake = (A.brk + afRand(w, o, A.brkV, 14)) * 0.01f;
                    float const brakeAngle = (A.brkAngle + afRand(w, o, A.brkAngleV, 15)) * 0.017453292f;
                    float const brakeSteer = (A.brkSteer + afRand(w, o, A.brkSteerV, 16)) * 0.01f;
                    float const brakeLimit = A.brkLimit + afRand(w, o, A.brkLimitV, 16);
                    float const speed = std::fabs(std::sqrt(phys->vx * phys->vx + phys->vy * phys->vy));
                    if (A.steerLowOn && speed <= speedLow)
                        steer = steerLow;
                    else if (A.steerHighOn && speedHigh <= speed && 0.f < speedHigh)
                        steer = steerHigh;
                    float const angDiff = afWrap(std::atan2(ny, nx) - std::atan2(phys->dy, phys->dx), 3.1415927f);
                    if (0.f < brake && brakeAngle < std::fabs(angDiff) && brakeLimit < speed) steer = brakeSteer;
                    float smoothSteer = (steer - phys->steer) / 10.f + phys->steer;
                    phys->steer = smoothSteer;
                    float turn = 0.f <= angDiff ? std::min(smoothSteer, angDiff) : std::max(-smoothSteer, angDiff);
                    if (turn != 0.f) {
                        float const a = turn * dt;
                        float const cs = gcos(a);
                        float const sn = gsin(a);
                        float const vx = phys->vx;
                        float const vy = phys->vy;
                        float const newVx = vx * cs - vy * sn;
                        if (!A.xOnly) phys->vy = vx * sn + vy * cs;
                        if (!A.yOnly) phys->vx = newVx;
                        float const newDir = std::atan2(phys->dy, phys->dx) + a;
                        phys->dx = gcos(newDir);
                        phys->dy = gsin(newDir);
                    }
                    float acc = accel;
                    if (0.f < brake && 0.f < std::fabs(angDiff - turn) - brakeAngle) {
                        float const vx = phys->vx;
                        float const vy = phys->vy;
                        float const maxBrakeX = std::fabs(brake * phys->dx * dt);
                        float const maxBrakeY = std::fabs(brake * phys->dy * dt);
                        float const brakeX = 0.f <= vx ? std::min(vx, maxBrakeX) : std::max(vx, -maxBrakeX);
                        float const brakeY = 0.f <= vy ? std::min(vy, maxBrakeY) : std::max(vy, -maxBrakeY);
                        if (!A.xOnly) phys->vy = vy - brakeY;
                        if (!A.yOnly) phys->vx = vx - brakeX;
                        acc = 0.f;
                    }
                    float const accDirX = A.useTargetDir ? nx : phys->dx;
                    float const accDirY = A.useTargetDir ? ny : phys->dy;
                    if (!A.xOnly) phys->vy = accDirY * acc * dt + phys->vy;
                    if (!A.yOnly) phys->vx = accDirX * acc * dt + phys->vx;
                } else if (A.mode == 1) {
                    float const nearRange = A.m1Range + afRand(w, o, A.m1RangeV, 6);
                    float friction = (A.m1Fric + afRand(w, o, A.m1FricV, 7)) * 0.01f;
                    float accelNow = accel;
                    float const len = std::sqrt(dx * dx + dy * dy);
                    float nx = 1.f;
                    float ny = 0.f;
                    if (len != 0.f) {
                        nx = dx / len;
                        ny = dy / len;
                    }
                    if (0.f < nearRange && dist < nearRange) {
                        float const nearAccel = (A.m1Acc + afRand(w, o, A.m1AccV, 8)) * 0.01f;
                        float const nearFriction = (A.m1Fric2 + afRand(w, o, A.m1Fric2V, 9)) * 0.01f;
                        float const t = static_cast<float>(1.0 - static_cast<double>(dist / nearRange));
                        accelNow = (nearAccel - accelNow) * t + accelNow;
                        friction = (nearFriction - friction) * t + friction;
                    }
                    if (friction != 0.f) {
                        float const frictionStep = friction * dt;
                        float const vx = phys->vx - phys->vx * frictionStep;
                        float const vy = phys->vy - phys->vy * frictionStep;
                        if (!A.xOnly) phys->vy = vy;
                        if (!A.yOnly) phys->vx = vx;
                    }
                    float const accelStep = accelNow * dt;
                    float const vx = phys->vx + nx * accelStep;
                    float const vy = phys->vy + ny * accelStep;
                    if (!A.xOnly) phys->vy = vy;
                    if (!A.yOnly) phys->vx = vx;
                } else {
                    float const easing = A.easing + afRand(w, o, A.easingV, 6);
                    float const fraction = 1.f / (1.f > easing ? 1.f : easing);
                    if (!A.xOnly) phys->vy = dy * fraction;
                    if (!A.yOnly) phys->vx = dx * fraction;
                }
                if (shared) {
                    if (A.angleMinDist <= dist) phys->angle = std::atan2(phys->vy, phys->vx);
                    float const curAngle = std::atan2(phys->dy, phys->dx);
                    float const step = afWrap(phys->angle - curAngle, 3.1415927f) / std::max(1.f, A.dirEasing);
                    float const newAngle = std::atan2(phys->dy, phys->dx) + step;
                    phys->dx = gcos(newAngle);
                    phys->dy = gsin(newAngle);
                }
                float rot = 0.f;
                if (A.rotate) {
                    float const curRot = o.rot + trigRotation(g, idx);
                    float const targetRot = (90.f - std::atan2(phys->dy, phys->dx) * 57.29578f) + A.rotOffset;
                    rot = afWrap(targetRot - curRot, 180.f) * 0.25f;
                }
                {
                    float speed = A.xOnly ? std::fabs(phys->vx) : A.yOnly ? std::fabs(phys->vy) : std::sqrt(phys->vx * phys->vx + phys->vy * phys->vy);
                    if (maxSpeed < speed && 0.f < maxSpeed) {
                        float const scale = maxSpeed / speed;
                        if (A.xOnly)
                            phys->vx = phys->vx * scale;
                        else if (A.yOnly)
                            phys->vy = scale * phys->vy;
                        else {
                            phys->vx *= scale;
                            phys->vy *= scale;
                        }
                    }
                }
                float moveX = phys->vx * dt;
                float moveY = phys->vy * dt;
                if (A.xOnly)
                    moveY = 0.f;
                else if (A.yOnly)
                    moveX = 0.f;
                if (moveX != 0.f || moveY != 0.f) moveAll(static_cast<double>(moveX), static_cast<double>(moveY));
                if (rot != 0.f) {
                    if (!rep) {
                        if (o.canRotateFree()) {
                            ObjDyn& d = dynEntry(g, idx);
                            d.rotAcc = rot + d.rotAcc;
                            d.rotAccY = rot + d.rotAccY;
                            d.rotated = true;
                        }
                    } else {
                        double baseX, baseY;
                        realPosBase(g, idx, baseX, baseY);
                        float const cx = static_cast<float>(baseX - static_cast<double>(o.posOffX));
                        float const cy = static_cast<float>(baseY - static_cast<double>(o.posOffY));
                        float const rad = -rot * 0.01745329252f;
                        float const cs = gcos(rad);
                        float const sn = gsin(rad);
                        for (int child : rep->phys) {
                            Obj const& childObj = objAt(g, child);
                            double bx, by;
                            realPosBase(g, child, bx, by);
                            float const lx = static_cast<float>(bx - static_cast<double>(childObj.posOffX));
                            float const ly = static_cast<float>(by - static_cast<double>(childObj.posOffY));
                            float const rx = lx - cx;
                            float const ry = ly - cy;
                            float const tx = (rx * cs + ry * -sn) + cx;
                            float const ty = (rx * sn + ry * cs) + cy;
                            float const ddx = (tx + moveX) - lx;
                            float const ddy = (ty + moveY) - ly;
                            afMoveObj(g, child, static_cast<double>(ddx), static_cast<double>(ddy));
                            if (childObj.canRotateFree()) {
                                ObjDyn& d = dynEntry(g, child);
                                d.rotAcc = rot + d.rotAcc;
                                d.rotAccY = rot + d.rotAccY;
                                d.rotated = true;
                            }
                        }
                    }
                }
            };
            if (auto const* targets = w.targetGroupsOf(inst.target, A.ignoreGroupParent, A.ignoreLinked)) {
                for (int idx : targets->loose) {
                    if (!w.decoLikeAt(idx) || w.centerLike[static_cast<size_t>(idx)]) followOne(idx, nullptr);
                }
                for (auto const& rep : targets->reps) followOne(rep.obj, &rep);
            } else
                for (int idx : w.moversOfGroup(inst.target)) followOne(idx, nullptr);
        }

        //guess for decoration without simulated follow physics: would the game follow it this step
        bool afApproxEntry(Game& g, int obj) {
            World const& w = *g.world;
            Obj const& o = objAt(g, obj);
            for (auto const& I : g.trig.r().M().advs) {
                if (I.finished && !I.doStart) continue;
                World::AdvDef const& A = w.advDefs[static_cast<size_t>(I.def)];
                bool targeted = false;
                if (auto const* targets = w.targetGroupsOf(I.target, A.ignoreGroupParent, A.ignoreLinked)) {
                    for (int i : targets->loose) {
                        if (i == obj) {
                            targeted = true;
                            break;
                        }
                    }
                    if (!targeted) {
                        for (auto const& rep : targets->reps) {
                            if (rep.obj == obj) {
                                targeted = true;
                                break;
                            }
                        }
                    }
                } else {
                    for (short gr : o.groups) {
                        if (gr == I.target) {
                            targeted = true;
                            break;
                        }
                    }
                }
                if (!targeted) continue;
                if (I.stopped || I.finished) return true;
                float targetX, targetY;
                if (!afSavedPos(g, I.key, A.delay + afRand(w, o, A.delayV, 0), targetX, targetY)) continue;
                if (A.ignoreDisabled && trigGroupDisabled(g, o)) continue;
                float const range = A.range + afRand(w, o, A.rangeV, 3);
                if (range == 0.f) return true;
                double px, py;
                trigRealPos(g, obj, px, py);
                float const dx = targetX - static_cast<float>(px);
                float const dy = targetY - static_cast<float>(py);
                if (!(range < std::sqrt(dx * dx + dy * dy))) return true;
            }
            return false;
        }

        // one step of all advanced follows (GJBaseGameLayer::processAdvancedFollowActions), dt in game units
        void advancedFollowStep(Game& g, float dt) {
            {
                TrigHeavy const& H = g.trig.r();
                if (H.M().advs.empty()) return;
                dynDep(2);
                if (H.M().advDirty) {
                    TrigHeavy& Hw = g.trig.w();
                    World const& w = *g.world;
                    auto& advs = Hw.Mw().advs;
                    auto const less = [&](AdvInst const& a, AdvInst const& b) {
                        int const prioA = w.advDefs[static_cast<size_t>(a.def)].priority;
                        int const prioB = w.advDefs[static_cast<size_t>(b.def)].priority;
                        if (prioA != prioB) return prioB < prioA;
                        return a.order < b.order;
                    };
                    for (size_t i = 1; i < advs.size(); ++i) {
                        if (!less(advs[i], advs[i - 1])) continue;
                        AdvInst const moving = advs[i];
                        size_t j = i;
                        while (j > 0 && less(moving, advs[j - 1])) {
                            advs[j] = advs[j - 1];
                            --j;
                        }
                        advs[j] = moving;
                    }
                    Hw.Mw().advDirty = false;
                }
            }
            size_t const n = g.trig.r().M().advs.size();
            for (size_t i = 0; i < n; ++i) {
                AdvInst const& I = g.trig.r().M().advs[i];
                if (tl_advSel != 0 && (advIsP(g, I) ? 1 : 0) != tl_advSel - 1) continue;
                if (!I.finished)
                    afProcessInstance(g, i, I.stopped != 0, dt);
                else if (I.doStart) {
                    g.trig.w().Mw().advs[i].doStart = 0;
                    afProcessInstance(g, i, true, dt);
                }
            }
            {
                bool any = false;
                for (auto const& I : g.trig.r().M().advs) {
                    if (I.stopped) {
                        any = true;
                        break;
                    }
                }
                if (any) {
                    auto& advs = g.trig.w().Mw().advs;
                    advs.erase(
                            std::remove_if(advs.begin(), advs.end(), [](AdvInst const& I) { return I.stopped != 0; }),
                            advs.end());
                }
            }
            {
                int const cmd = static_cast<int>(g.commandIndex);
                bool any = false;
                if (tl_advSel != 2) {
                    for (auto const& e : g.trig.rd().afPhys) {
                        if (e.second.last < cmd) {
                            any = true;
                            break;
                        }
                    }
                    if (any) {
                        auto& items = g.trig.wd().afPhys.items;
                        items.erase(std::remove_if(items.begin(), items.end(), [&](auto const& e) { return e.second.last < cmd; }),
                                    items.end());
                    }
                }
                if (tl_advSel != 1 && g.trig.pv && !g.trig.pv->af.items.empty()) {
                    bool anyP = false;
                    for (auto const& e : g.trig.pv->af.items) {
                        if (e.second.last < cmd) {
                            anyP = true;
                            break;
                        }
                    }
                    if (anyP) {
                        auto& items = g.trig.wp().af.items;
                        items.erase(std::remove_if(items.begin(), items.end(), [&](auto const& e) { return e.second.last < cmd; }),
                                    items.end());
                    }
                }
            }
        }

        // sorted objects moved by follow player Y and player dependent advanced follows, cached per target list
        std::vector<int> const& fyObjectsOf(World const& w, std::vector<GroupCmd> const& cmds,
                                            std::vector<AdvInst> const& advs) {
            static thread_local WKey tw;
            static thread_local std::map<std::vector<int>, std::vector<int>> cache;
            if (tw != &w) {
                tw = &w;
                cache.clear();
                ++tl_fyGen;
            }
            static thread_local std::vector<int> tg;
            static thread_local std::vector<int> ta;
            tg.clear();
            ta.clear();
            for (auto const& c : cmds)
                if (c.type == CmdType::FollowY && !c.disabled) tg.push_back(c.target);
            for (auto const& I : advs)
                if (!I.rngOnly && advPlayerDep(w, I)) ta.push_back(I.target);
            std::sort(tg.begin(), tg.end());
            tg.erase(std::unique(tg.begin(), tg.end()), tg.end());
            std::sort(ta.begin(), ta.end());
            ta.erase(std::unique(ta.begin(), ta.end()), ta.end());
            tg.push_back(-1);
            tg.insert(tg.end(), ta.begin(), ta.end());
            auto it = cache.find(tg);
            if (it != cache.end()) return it->second;
            std::vector<int> objs;
            for (int gr : tg)
                if (gr >= 0)
                    for (int idx : w.moversOfGroup(gr)) objs.push_back(idx);
            std::sort(objs.begin(), objs.end());
            objs.erase(std::unique(objs.begin(), objs.end()), objs.end());
            return cache.emplace(tg, std::move(objs)).first->second;
        }

        // true if the follow (type 2) touches no follow Y object, by its center or its targets
        bool followIndependent(World const& w, GroupCmd const& c, std::vector<int> const& fy) {
            static thread_local WKey tw;
            static thread_local std::map<std::tuple<void const*, int, int>, bool> cache;
            if (tw != &w) {
                tw = &w;
                cache.clear();
            }
            auto const cacheKey = std::make_tuple(static_cast<void const*>(&fy), c.target, c.center);
            auto it = cache.find(cacheKey);
            if (it != cache.end()) return it->second;
            bool ok = true;
            int const centerIdx = w.mainObjectOf(c.center);
            if (centerIdx >= 0 && std::binary_search(fy.begin(), fy.end(), centerIdx)) ok = false;
            if (ok) {
                for (int idx : w.allOfGroup(c.target)) {
                    if (std::binary_search(fy.begin(), fy.end(), idx)) {
                        ok = false;
                        break;
                    }
                }
            }
            cache.emplace(cacheKey, ok);
            return ok;
        }

        // center object of a rotate (1), scale (4) or keyframe (5) command, like the trigger step picks it, -1 if none
        int closureCenterOf(World const& w, GroupCmd const& c) {
            if (c.type == CmdType::Transform) return c.center > 0 ? w.mainObjectOf(c.center) : -1;
            int centerIdx = c.type == CmdType::Keyframe ? w.mainObjectOf(c.target) : w.mainObjectOf(c.center);
            if (c.target == c.center || c.type == CmdType::Keyframe) {
                int const parent = w.exportParentOf(c.target);
                if (parent >= 0) centerIdx = parent;
            }
            return centerIdx;
        }

        // groups and objects of the player part for this state, nullptr if there are none
        std::shared_ptr<PSet const> computePSet(Game const& g) {
            if (!g.world->pPossible) return nullptr;
            TrigMotion const& M = g.trig.r().M();
            static thread_local TrigMotion const* lastM = nullptr;
            static thread_local uint64_t lastVer = 0;
            static thread_local WKey lastW;
            static thread_local std::shared_ptr<PSet const> lastRes;
            if (lastM == &M && lastVer == M.ver && lastW == g.world && M.ver != 0) return lastRes;
            World const& w = *g.world;
            PSet const* prev = M.pset.get();
            uint64_t seedSig = prev ? prev->sig : 0x243f6a8885a308d3ull;
            bool seed = prev != nullptr;
            int nLock = 0;
            int nAdv = 0;
            {
                for (auto const& c : M.cmds) {
                    if (c.disabled) continue;
                    if (c.type == CmdType::Move && !c.finishRelated && (c.lockPY || c.lockCY)) {
                        seedSig = mix64(seedSig, static_cast<uint64_t>(c.target) * 8u + 1u);
                        ++nLock;
                    } else if (c.type == CmdType::FollowY) {
                        seedSig = mix64(seedSig, static_cast<uint64_t>(c.target) * 8u + 3u);
                    } else if (c.type == CmdType::Follow) {
                        seedSig = mix64(seedSig, ((static_cast<uint64_t>(static_cast<uint32_t>(c.target)) << 20) ^ static_cast<uint32_t>(c.center)) * 8u + 2u);
                    }
                }
                if (prev) {
                    for (auto const& e : M.drots) {
                        if (e.kind == 1 &&
                            ((e.c >= 0 && bitOf(prev->objBits, e.c)) || (e.a >= 0 && bitOf(prev->objBits, e.a)))) {
                            seedSig = mix64(mix64(seedSig, (static_cast<uint64_t>(static_cast<uint32_t>(e.target)) << 32 ^ static_cast<uint32_t>(e.c)) * 8u + 6u), static_cast<uint32_t>(e.a));
                        }
                    }
                }
                for (auto const& c : M.cmds) {
                    if (c.disabled ||
                        !(c.type == CmdType::Rotate || c.type == CmdType::Transform || c.type == CmdType::Keyframe))
                        continue;
                    int const centerIdx = closureCenterOf(w, c);
                    if (centerIdx < 0 || (prev && !bitOf(prev->objBits, centerIdx))) continue;
                    seedSig = mix64(seedSig, ((static_cast<uint64_t>(static_cast<uint32_t>(c.target)) << 32) ^
                                              static_cast<uint32_t>(centerIdx)) * 8u + 4u + static_cast<uint64_t>(c.type) * 0x10000u);
                }
                for (auto const& e : M.drots) {
                    if (e.kind == 1 && (e.a == kRefP1 || e.a == kRefP2 || e.c == kRefP1 || e.c == kRefP2) &&
                        w.relevantGroup(e.target)) {
                        seedSig =
                                mix64(seedSig, (static_cast<uint64_t>(static_cast<uint32_t>(e.target)) << 8) * 8u + 7u);
                        ++nLock;
                    }
                }
                for (auto const& I : M.advs) {
                    seedSig = mix64(seedSig, ((static_cast<uint64_t>(static_cast<uint32_t>(I.target)) << 24) ^
                                                static_cast<uint64_t>(static_cast<uint32_t>(I.key))) * 8u + 5u);
                    if (I.rngOnly) continue;
                    if (I.key < 0)
                        ++nAdv;
                    else {
                        int const mainObj = w.mainObjectOf(I.key);
                        if (mainObj >= 0 && w.areaTarget[static_cast<size_t>(mainObj)]) ++nAdv;
                    }
                }
                if (nLock || nAdv) seed = true;
            }
            std::shared_ptr<PSet const> res;
            if (seed) {
                static thread_local std::unordered_map<uint64_t, std::shared_ptr<PSet const>> sigCache;
                static thread_local WKey cacheW;
                if (cacheW != g.world) {
                    sigCache.clear();
                    cacheW = g.world;
                }
                auto it = sigCache.find(seedSig);
                if (it != sigCache.end())
                    res = it->second;
                else {
                    static thread_local std::vector<char> pg;
                    static thread_local std::vector<uint64_t> po;
                    pg.assign(10000, 0);
                    if (po.size() != (w.objs.size() >> 6) + 1) po.assign((w.objs.size() >> 6) + 1, 0);
                    std::vector<int> groupsL;
                    std::vector<int> objsL;
                    std::vector<int> work;
                    auto setO = [&](int i) {
                        if (i < 0) return false;
                        uint64_t& word = po[static_cast<size_t>(i) >> 6];
                        uint64_t const mask = 1ull << (i & 63);
                        if (word & mask) return false;
                        word |= mask;
                        objsL.push_back(i);
                        return true;
                    };
                    auto hasO = [&](int i) { return i >= 0 && ((po[static_cast<size_t>(i) >> 6] >> (i & 63)) & 1u); };
                    auto setG = [&](int gr) {
                        if (gr <= 0 || gr > 9999 || pg[static_cast<size_t>(gr)]) return false;
                        pg[static_cast<size_t>(gr)] = 1;
                        groupsL.push_back(gr);
                        work.push_back(gr);
                        return true;
                    };
                    if (prev) {
                        for (int gr : prev->groups) {
                            pg[static_cast<size_t>(gr)] = 1;
                            groupsL.push_back(gr);
                        }
                        for (int i : prev->objs) setO(i);
                    }
                    for (auto const& c : M.cmds) {
                        if (c.disabled) continue;
                        if (c.type == CmdType::Move && !c.finishRelated && (c.lockPY || c.lockCY))
                            setG(c.target);
                        else if (c.type == CmdType::FollowY)
                            for (int i : w.moversOfGroup(c.target)) setO(i);
                    }
                    std::vector<char> advP(M.advs.size(), 0);
                    for (size_t k = 0; k < M.advs.size(); ++k) {
                        AdvInst const& I = M.advs[k];
                        if (I.key < 0)
                            advP[k] = 1;
                        else {
                            int const mainObj = w.mainObjectOf(I.key);
                            if (mainObj >= 0 && w.areaTarget[static_cast<size_t>(mainObj)]) advP[k] = 1;
                        }
                    }
                    for (bool changed = true; changed;) {
                        changed = false;
                        while (!work.empty()) {
                            int const gr = work.back();
                            work.pop_back();
                            for (int i : w.allOfGroup(gr)) setO(i);
                        }
                        for (size_t k = 0; k < M.advs.size(); ++k) {
                            AdvInst const& I = M.advs[k];
                            if (advP[k] == 2) continue;
                            if (!advP[k]) {
                                bool dep = I.key > 0 && hasO(w.mainObjectOf(I.key));
                                if (!dep) {
                                    for (int i : w.moversOfGroup(I.target)) {
                                        if (hasO(i)) {
                                            dep = true;
                                            break;
                                        }
                                    }
                                }
                                if (!dep) continue;
                            }
                            advP[k] = 2;
                            for (int i : w.moversOfGroup(I.target))
                                if (setO(i)) changed = true;
                        }
                        for (auto const& e : M.drots) {
                            if (e.kind == 1 && ((e.c >= 0 && hasO(e.c)) || (e.a >= 0 && hasO(e.a)) || e.a == kRefP1 || e.a == kRefP2 || e.c == kRefP1 || e.c == kRefP2) && setG(e.target))
                                changed = true;
                        }
                        for (auto const& c : M.cmds) {
                            if (c.disabled) continue;
                            if (c.type == CmdType::Follow) {
                                if (hasO(w.mainObjectOf(c.center)) && setG(c.target)) changed = true;
                            } else if (c.type == CmdType::Rotate || c.type == CmdType::Transform || c.type == CmdType::Keyframe) {
                                int const centerIdx = closureCenterOf(w, c);
                                if (hasO(centerIdx))
                                    for (int i : physOfGroup(w, c.target))
                                        if (setO(i)) changed = true;
                            }
                        }
                        if (!work.empty()) changed = true;
                    }
                    bool relevant = !groupsL.empty();
                    if (!relevant) {
                        for (int i : objsL) {
                            Obj const& o = w.objs[static_cast<size_t>(i)];
                            if (o.cls == 1 || (o.type != Decoration && !(o.extendedCollision && o.id != 0x718))) {
                                relevant = true;
                                break;
                            }
                        }
                    }
                    if (!prev && !relevant) {
                        for (int i : objsL) po[static_cast<size_t>(i) >> 6] = 0;
                        lastM = &M;
                        lastVer = M.ver;
                        lastW = g.world;
                        lastRes = nullptr;
                        if (sigCache.size() > 4096) sigCache.clear();
                        sigCache.emplace(seedSig, nullptr);
                        return nullptr;
                    }
                    auto pset = std::make_shared<PSet>();
                    std::sort(groupsL.begin(), groupsL.end());
                    groupsL.erase(std::unique(groupsL.begin(), groupsL.end()), groupsL.end());
                    std::sort(objsL.begin(), objsL.end());
                    pset->groupBits.assign(10000 / 64 + 1, 0);
                    pset->memberGroupBits.assign(10000 / 64 + 1, 0);
                    pset->objBits.assign(po.size(), 0);
                    uint64_t sig = 0x6a09e667f3bcc909ull;
                    for (int gr : groupsL) {
                        pset->groupBits[static_cast<size_t>(gr) >> 6] |= 1ull << (gr & 63);
                        sig = mix64(sig, static_cast<uint64_t>(gr));
                    }
                    sig = mix64(sig, 0xffffull);
                    for (int i : objsL) {
                        pset->objBits[static_cast<size_t>(i) >> 6] |= 1ull << (i & 63);
                        sig = mix64(sig, static_cast<uint64_t>(i));
                        Obj const& o = w.objs[static_cast<size_t>(i)];
                        for (short gr : o.groups)
                            if (gr > 0 && gr < 10000)
                                pset->memberGroupBits[static_cast<size_t>(gr) >> 6] |= 1ull << (gr & 63);
                        if (o.cls == 1) pset->trigObjs = true;
                        if (o.type != Decoration && !(o.extendedCollision && o.id != 0x718))
                            pset->collObjs.push_back(i);
                    }
                    for (int i : objsL) po[static_cast<size_t>(i) >> 6] = 0;
                    pset->groups = std::move(groupsL);
                    pset->objs = std::move(objsL);
                    pset->sig = sig;
                    if (prev && prev->sig == sig)
                        res = M.pset;
                    else
                        res = std::move(pset);
                    if (sigCache.size() > 4096) sigCache.clear();
                    sigCache.emplace(seedSig, res);
                }
            }
            lastM = &M;
            lastVer = M.ver;
            lastW = g.world;
            lastRes = res;
            return res;
        }

        // recomputes the player part, new group and objects move their state from the shared part into it
        void updatePSet(Game& g) {
            std::shared_ptr<PSet const> pset = computePSet(g);
            TrigState& T = g.trig;
            if (pset.get() == T.r().M().pset.get()) return;
            if (!pset) return;
            PSet const* prev = T.r().M().pset.get();
            if (prev && prev->sig == pset->sig) return;
            World const& w = *g.world;
            std::atomic_ref<bool>(w.pBorn).store(true, std::memory_order_release);
            TrigMotion& HM = T.w().Mw();
            HM.pset = pset;
            T.h->anyMotion = true;
            for (int gr : pset->groups) {
                if (prev && bitOf(prev->groupBits, gr)) continue;
                markGroupMoved(*T.h, static_cast<short>(gr));
                if (auto const* c = T.r().M().groupOff.find(static_cast<short>(gr))) {
                    TrigPv& P = T.wp();
                    if (!P.off.find(static_cast<short>(gr))) P.off[static_cast<short>(gr)] = *c;
                }
            }
            for (int idx : pset->objs) {
                if (prev && bitOf(prev->objBits, idx)) continue;
                for (short gr : objAt(g, idx).groups) markGroupMoved(*T.h, gr);
                if (T.d) {
                    if (ObjDyn const* e = T.rd().objDyn.find(idx)) {
                        ObjDyn const d = *e;
                        {
                            TrigPv& P = T.wp();
                            if (!P.dyn.find(idx)) {
                                P.dyn[idx] = d;
                                P.dynBloom |= objAt(g, idx).groupBloom;
                            }
                        }
                        T.wd().objDyn.erase(idx);
                        Obj const& o = objAt(g, idx);
                        if (o.type != Decoration && !(o.extendedCollision && o.id != 0x718)) {
                            double bx, by;
                            {
                                bool const canonReadBefore = tl_canonRead;
                                tl_canonRead = true;
                                groupPos(g, o, bx, by);
                                tl_canonRead = canonReadBefore;
                            }
                            int const cx0 = World::cellOf(d.secX);
                            int const cy0 = w.cellYOf(d.secY);
                            int const cx1 = World::cellOf(bx);
                            int const cy1 = w.cellYOf(by);
                            if (cx0 != cx1 || cy0 != cy1) cellMove(g, idx, cx0, cy0, cx1, cy1);
                        }
                    }
                    if (AfPhys const* a = T.rd().afPhys.find(idx)) {
                        AfPhys const phys = *a;
                        {
                            TrigPv& P = T.wp();
                            P.af[idx] = phys;
                        }
                        T.wd().afPhys.erase(idx);
                    }
                    auto const& afObjs = T.rd().afObjs;
                    if (std::binary_search(afObjs.begin(), afObjs.end(), idx)) {
                        auto& sharedList = T.wd().afObjs;
                        sharedList.erase(std::lower_bound(sharedList.begin(), sharedList.end(), idx));
                        auto& playerList = T.wp().afObjs;
                        playerList.insert(std::lower_bound(playerList.begin(), playerList.end(), idx), idx);
                    }
                }
            }
        }

        // world matrix of the children of the transform node, like the game
        Aff tfWorld(float cx, float cy, double fxN, double fxO, double fyN, double fyO, float rotT, float centerRot) {
            Aff const R = nodeAff(0.f, 0.f, -centerRot, -centerRot, 1.f, 1.f);
            Aff const S = nodeAff(0.f, 0.f, 0.f, 0.f, static_cast<float>(1.0 / fxO), static_cast<float>(1.0 / fyO));
            Aff const I = nodeAff(0.f, 0.f, 0.f, 0.f, 1.f, 1.f);
            Aff const Tn = nodeAff(cx, cy, rotT, rotT, static_cast<float>(fxN), static_cast<float>(fyN));
            Aff t = affConcat(R, S);
            t = affConcat(t, I);
            t = affConcat(t, Tn);
            t = affConcat(t, I);
            t = affConcat(t, I);
            return t;
        }

        // applies a matrix to an object and reads back its scale and rotation
        void tfObj(Obj const& o, ObjDyn& d, Aff const& P, float cx, float cy, float pdx, float pdy, bool onlyMove, float rotOff) {
            double const bx = d.x - static_cast<double>(o.posOffX);
            double const by = d.y - static_cast<double>(o.posOffY);
            float const rX = o.rotX + d.rotAcc;
            float const rY = o.rotY + d.rotAccY;
            Aff const N = nodeAff(static_cast<float>(bx - static_cast<double>(cx)), static_cast<float>(by - static_cast<double>(cy)), rX, rY, d.sclX, d.sclY);
            Aff const M = affConcat(N, P);
            double newX = (static_cast<double>(M.tx + pdx) - bx) + d.x;
            double newY = (static_cast<double>(M.ty + pdy) - by) + d.y;
            if (newX > 1e7)
                newX = 1e7;
            else if (newX < -1e7)
                newX = -1e7;
            if (newY > 1e7)
                newY = 1e7;
            else if (newY < -1e7)
                newY = -1e7;
            d.x = newX;
            d.y = newY;
            if (!onlyMove) {
                float const newSclX = std::sqrt(M.b * M.b + M.a * M.a);
                float const newSclY = std::sqrt(M.c * M.c + M.d * M.d);
                float const angleX = (M.c == 0.f && M.d > 0.f) ? 1.57079637f : std::atan2(M.d, M.c);
                float const angleY = (M.b == 0.f && M.a > 0.f) ? std::copysign(0.f, M.b) : std::atan2(M.b, M.a);
                float const newRotX = -static_cast<float>(static_cast<double>(angleX) * 57.29577951308232 - 90.0);
                float const newRotY = -static_cast<float>(static_cast<double>(angleY) * 57.29577951308232);
                d.rotAcc = static_cast<float>(static_cast<double>(d.rotAcc) + (static_cast<double>(newRotX) - static_cast<double>(rX)));
                d.rotAccY = static_cast<float>(static_cast<double>(d.rotAccY) + (static_cast<double>(newRotY) - static_cast<double>(rY)));
                if (o.type != Decoration) d.outerOb = true;
                float const ex = (newSclX - d.sclX) + 0.f;
                float const ey = (newSclY - d.sclY) + 0.f;
                d.sclOffX = ex + d.sclOffX;
                d.sclOffY = ey + d.sclOffY;
                d.sclX = ex + d.sclX;
                d.sclY = ey + d.sclY;
                d.scaled = true;
            } else if (rotOff != 0.f) {
                d.rotAcc = rotOff + d.rotAcc;
                d.rotAccY = rotOff + d.rotAccY;
                if (o.type != Decoration) d.rotated = true;
            }
        }

    }
}
