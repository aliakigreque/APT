// Creation and stepping of the group commands of the move, rotate, follow, transform and keyframe triggers
// (GJEffectManager::createMoveCommand and the other create*Command).

#include "triggers_internal.hpp"

namespace ap::sim {
    namespace trigger_impl {
        // adds an empty group command
        GroupCmd& newCmd(Game& g) {
            TrigHeavy& heavy = g.trig.w();
            auto& cmds = heavy.Mw().cmds;
            cmds.emplace_back();
            return cmds.back();
        }

        // createMoveCommand of the game (type 0), locks follow the player or the camera
        void createMoveCommand(Game& g, double dx, double dy, int group, float duration, int easing, float rate,
                               bool lockPX, bool lockPY, bool lockCX, bool lockCY, float modX, float modY, int uid,
                               int control) {
            if (dx == 0.0 && dy == 0.0 && !lockPX && !lockPY && !lockCX && !lockCY) return;
            if (group <= 0) return;
            if (!g.world->relevantGroup(group)) return;
            GroupCmd& cmd = newCmd(g);
            cmd.type = CmdType::Move;
            cmd.uid = uid;
            cmd.control = control;
            cmd.target = group;
            cmd.easing = easing;
            cmd.lockPX = lockPX;
            cmd.lockPY = lockPY;
            cmd.lockCX = lockCX;
            cmd.lockCY = lockCY;
            cmd.duration = static_cast<double>(duration);
            cmd.rate = static_cast<double>(rate);
            cmd.modX = modX == 0.f ? 1.0 : static_cast<double>(modX);
            cmd.modY = modY == 0.f ? 1.0 : static_cast<double>(modY);
            cmd.lockedInX = lockPX || lockCX;
            cmd.lockedInY = lockPY || lockCY;
            if (dx == 0.0 && dy == 0.0 && !cmd.lockedInX && !cmd.lockedInY) {
                cmd.finished = true;
                cmd.finishRelated = true;
                return;
            }
            if (dx != 0.0 && !cmd.lockedInX) {
                cmd.actionType1 = 1;
                cmd.value1 = dx;
            }
            if (dy != 0.0 && !cmd.lockedInY) {
                if (cmd.actionType1 == 0) {
                    cmd.actionType1 = 2;
                    cmd.value1 = dy;
                } else {
                    cmd.actionType2 = 2;
                    cmd.value2 = dy;
                }
            }
        }

        // createRotateCommand of the game (type 1)
        void createRotateCommand(Game& g, float rotation, float duration, int target, int center, int easing,
                                 float rate, bool lockRot, int uid, int control) {
            if (!g.world->relevantGroup(target)) return;
            GroupCmd& cmd = newCmd(g);
            cmd.type = CmdType::Rotate;
            cmd.uid = uid;
            cmd.control = control;
            cmd.target = target;
            cmd.center = center;
            cmd.easing = easing;
            cmd.rate = static_cast<double>(rate);
            cmd.duration = static_cast<double>(duration);
            cmd.lockObjRot = lockRot;
            double const degrees = static_cast<double>(rotation);
            cmd.rotVal = 0;
            if (degrees == 0.0) {
                cmd.finished = true;
                cmd.finishRelated = true;
            } else {
                cmd.value1 = degrees;
                cmd.actionType1 = 3;
            }
        }

        // createFollowCommand of the game (type 2): target follows the center moves times the mods
        void createFollowCommand(Game& g, float xMod, float yMod, float duration, int target, int center, int uid,
                                 int control) {
            if (!g.world->relevantGroup(target)) return;
            GroupCmd& cmd = newCmd(g);
            cmd.type = CmdType::Follow;
            cmd.uid = uid;
            cmd.control = control;
            cmd.target = target;
            cmd.center = center;
            cmd.duration = static_cast<double>(duration);
            cmd.followXMod = static_cast<double>(xMod);
            cmd.followYMod = static_cast<double>(yMod);
            if (static_cast<double>(xMod) == 0.0 && static_cast<double>(yMod) == 0.0) {
                cmd.finished = true;
                cmd.finishRelated = true;
            }
        }

        // createPlayerFollowCommand of the game (type 3), follows the player y
        void createPlayerFollowCommand(Game& g, float speed, float delay, int offset, float maxSpeed, float duration,
                                       int target, int uid, int control) {
            if (!g.world->relevantGroup(target)) return;
            GroupCmd& cmd = newCmd(g);
            cmd.type = CmdType::FollowY;
            cmd.uid = uid;
            cmd.control = control;
            cmd.target = target;
            cmd.duration = static_cast<double>(duration);
            double clampedSpeed = static_cast<double>(speed);
            if (500.0 <= clampedSpeed) clampedSpeed = 500.0;
            if (clampedSpeed < 0.0) clampedSpeed = 0.0;
            cmd.fySpeed = clampedSpeed;
            cmd.fyDelay = static_cast<double>(delay);
            cmd.fyOffset = offset;
            cmd.fyMax = static_cast<double>(maxSpeed);
            if (clampedSpeed <= 0.0) {
                cmd.finished = true;
                cmd.finishRelated = true;
            }
            if (delay > 0.f && g.trig.yHist.empty()) g.trig.yHist.assign(200, 0.f);
        }

        // createTransformCommand of the game (type 4), scale around a center
        void createTransformCommand(Game& g, double scaleX, double scaleY, int target, int center, bool onlyMove,
                                    bool relRot, float duration, int easing, float rate, int uid, int control) {
            if (target <= 0 || !g.world->relevantGroup(target)) return;
            GroupCmd& cmd = newCmd(g);
            cmd.type = CmdType::Transform;
            cmd.uid = uid;
            cmd.control = control;
            cmd.target = target;
            cmd.center = center;
            cmd.lockObjRot = onlyMove;
            cmd.relRot = relRot;
            cmd.easing = easing;
            cmd.rate = static_cast<double>(rate);
            cmd.duration = static_cast<double>(duration);
            cmd.modX = scaleX;
            cmd.modY = scaleY;
            cmd.actionType1 = 4;
            cmd.value1 = 1.0;
            cmd.rotVal = 0;
            cmd.rotAccPrev = 0;
        }

        // eased progress of one action, adds the new delta to the command
        void updateAction(GroupCmd& cmd, int type, float value) {
            double duration = cmd.duration;
            if (duration <= 1.1920928955078125e-07) duration = 1.1920928955078125e-07;
            double progress = static_cast<double>(cmd.deltaTimeF) / duration;
            double clamped;
            if (1.0 < progress)
                clamped = 1.0;
            else
                clamped = progress < 0.0 ? 0.0 : progress;
            float const eased = easedValue(static_cast<float>(clamped), cmd.easing, static_cast<float>(cmd.rate));
            value = value - (1.f - eased) * value;
            if (type == 1) {
                if (!cmd.lockedInX) {
                    double const delta = static_cast<double>(value) - cmd.curX;
                    cmd.curX = static_cast<double>(value);
                    cmd.deltaX += delta;
                }
            } else if (type == 2) {
                if (!cmd.lockedInY) {
                    double const delta = static_cast<double>(value) - cmd.curY;
                    cmd.curY = static_cast<double>(value);
                    cmd.deltaY += delta;
                }
            } else if (type == 3 || type == 4) {
                double const old = cmd.rotVal;
                cmd.rotVal = static_cast<double>(value);
                cmd.rotDelta = (static_cast<double>(value) - old) + cmd.rotDelta;
            }
        }

        // advances a group command by dt
        void stepCmd(GroupCmd& cmd, float dt) {
            if (cmd.disabled) return;
            cmd.deltaTime = static_cast<double>(dt) + cmd.deltaTime;
            if (cmd.finished) return;
            if (!cmd.alreadyUpdated)
                cmd.deltaTimeF = dt + cmd.deltaTimeF;
            else
                cmd.alreadyUpdated = false;
            if (cmd.actionType1 == 0) {
                if (cmd.duration <= cmd.deltaTime && cmd.duration != -1.0) cmd.finished = true;
                return;
            }
            updateAction(cmd, cmd.actionType1, static_cast<float>(cmd.value1));
            if (cmd.actionType2 != 0) updateAction(cmd, cmd.actionType2, static_cast<float>(cmd.value2));
            if (0.f < cmd.deltaTimeF && cmd.duration <= static_cast<double>(cmd.deltaTimeF)) cmd.finished = true;
        }

        // the single path pool
        KfDynPool& kfDynPool() {
            static KfDynPool p;
            return p;
        }

        // rebuilt path with this hash, thread local copy of the pointers already seen
        KfDyn const& kfDynGet(World const& w, uint64_t key) {
            thread_local std::unordered_map<uint64_t, KfDyn const*> local;
            thread_local uint64_t lgen = ~0ull;
            KfDynPool& pool = kfDynPool();
            if (lgen != pool.gen.load(std::memory_order_acquire)) {
                local.clear();
                lgen = pool.gen.load(std::memory_order_acquire);
            }
            auto it = local.find(key);
            if (it != local.end()) return *it->second;
            std::lock_guard<std::mutex> lock(pool.m);
            auto found = pool.map.find(key);
            if (found == pool.map.end() || pool.sig != kfWorldSig(w)) {
                std::fprintf(stderr, "kfDynGet : path %016llx missing\n", static_cast<unsigned long long>(key));
                std::abort();
            }
            local.emplace(key, found->second.get());
            return *found->second;
        }

        // stores the path of anim for these keyframe states, returns its hash
        uint64_t kfDynPut(World const& w, int anim, std::vector<World::KfKeyState> const& states) {
            uint64_t hash = 0x6b8b4567327b23c6ull ^ static_cast<uint64_t>(static_cast<uint32_t>(anim));
            auto mixd = [&](double value) {
                uint64_t bits;
                std::memcpy(&bits, &value, 8);
                hash ^= bits + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
                hash *= 0xbf58476d1ce4e5b9ull;
            };
            for (auto const& key : states) {
                mixd(static_cast<double>(key.px));
                mixd(static_cast<double>(key.py));
                mixd(key.sx);
                mixd(key.sy);
                mixd(key.rotX);
                mixd(key.rotY);
            }
            if (hash == 0) hash = 1;
            KfDynPool& pool = kfDynPool();
            std::lock_guard<std::mutex> lock(pool.m);
            if (pool.sig != kfWorldSig(w)) {
                pool.map.clear();
                pool.sig = kfWorldSig(w);
                pool.gen.fetch_add(1, std::memory_order_acq_rel);
            }
            for (;;) {
                auto it = pool.map.find(hash);
                if (it == pool.map.end()) break;
                KfDyn const& existing = *it->second;
                bool same = existing.anim == anim && existing.st.size() == states.size();
                for (size_t i = 0; same && i < states.size(); ++i) {
                    same = existing.st[i].px == states[i].px && existing.st[i].py == states[i].py &&
                           existing.st[i].sx == states[i].sx && existing.st[i].sy == states[i].sy &&
                           existing.st[i].rotX == states[i].rotX && existing.st[i].rotY == states[i].rotY;
                }
                if (same) return hash;
                hash = hash * 0x9e3779b97f4a7c15ull + 1;
                if (hash == 0) hash = 1;
            }
            auto path = std::make_unique<KfDyn>();
            World::KfAnim const& source = w.kfAnims[static_cast<size_t>(anim)];
            path->anim = anim;
            path->st = states;
            path->A.target = source.target;
            path->A.uid = source.uid;
            path->A.control = source.control;
            path->A.keys = source.keys;
            buildKeyframeAnim(w, source.keys, states, path->A, path->splines, source.timeMod, source.pxm, source.pym,
                              source.rotm, source.sxm, source.sym);
            for (auto const& e : path->A.e)
                if (e.spawnGroup > 0) path->A.spawns = true;
            pool.map.emplace(hash, std::move(path));
            return hash;
        }

        // one step of a keyframe command (type 5), like GJEffectManager::prepareMoveActions
        void kfStep(Game& g, GroupCmd& cmd, float dt, KfOut& out, std::vector<KfSpawn>& spawns) {
            World const& w = *g.world;
            KfDyn const* dynPath = cmd.kfDyn ? &kfDynGet(w, cmd.kfDyn) : nullptr;
            auto const& animation = dynPath ? dynPath->A : w.kfAnims[static_cast<size_t>(cmd.kf)];
            std::vector<World::KfSpline> const& splines = dynPath ? dynPath->splines : w.kfSplines;
            auto const& entries = animation.e;
            cmd.deltaTime = static_cast<double>(dt) + cmd.deltaTime;
            double segTime = cmd.deltaTime;
            double elapsed = 0.0;
            World::KfEntry const* current = nullptr;
            int const count = static_cast<int>(entries.size());
            int const segCount = count - 1;
            for (int i = 0; i < segCount; ++i) {
                current = &entries[static_cast<size_t>(i)];
                double const easeDur = current->runEase;
                if (0.0 < easeDur && segTime < easeDur) {
                    segTime = easeDur * static_cast<double>(easedValue(static_cast<float>(segTime / easeDur),
                                                                       current->easing, current->rate));
                }
                if (segTime < current->dur || i == count - 2) {
                    elapsed += segTime;
                    break;
                }
                segTime -= current->dur;
                elapsed += current->dur;
            }
            float posX = 0.f;
            float posY = 0.f;
            double scaleX = 1.0;
            double scaleY = 1.0;
            double rot = 0.0;
            double rot0 = 0.0;
            bool onSpline = false;
            double remaining = elapsed;
            auto trySpawn = [&](int i) {
                if (i < cmd.kfSpawned) return;
                cmd.kfSpawned = i + 1;
                auto const& e = entries[static_cast<size_t>(i)];
                if (0 < e.spawnGroup) {
                    spawns.push_back(KfSpawn{e.uid, e.spawnGroup, cmd.control, e.spawnDelay, cmd.kfKeys});
                }
            };
            for (int i = 0; i < segCount; ++i) {
                auto const& e = entries[static_cast<size_t>(i)];
                auto const& next = entries[static_cast<size_t>(i + 1)];
                if (i == 0) rot0 = e.rot;
                bool proximitySpawn = false;
                if (0.0 < e.runDur && remaining < e.runDur) {
                    double splineTime = remaining;
                    if (current && segTime < current->dur && 0 < current->easing && !current->refOnly &&
                        current->runEase <= 0.0) {
                        float const easedPart =
                                easedValue(static_cast<float>(segTime / current->dur), current->easing, current->rate);
                        splineTime = (remaining - segTime) + current->dur * static_cast<double>(easedPart);
                        if (current->proximity && 0.95 < static_cast<double>(easedPart)) proximitySpawn = true;
                    }
                    onSpline = true;
                    if (e.splineX >= 0) {
                        posX = static_cast<float>(kfSplineEval(splines[static_cast<size_t>(e.splineX)], splineTime) +
                                                  static_cast<double>(posX));
                    }
                    if (e.splineY >= 0) {
                        posY = static_cast<float>(kfSplineEval(splines[static_cast<size_t>(e.splineY)], splineTime) +
                                                  static_cast<double>(posY));
                    }
                }
                bool const lastActive = (i == count - 2) && !(cmd.duration <= cmd.deltaTime);
                double part;
                if (remaining < e.dur || lastActive) {
                    part = remaining / e.dur;
                    if (0 < e.easing && !e.refOnly && e.runEase <= 0.0) {
                        part = static_cast<double>(easedValue(static_cast<float>(part), e.easing, e.rate));
                    }
                    if ((e.proximity && 0.95 < part) || proximitySpawn) trySpawn(i);
                } else {
                    part = 1.0;
                    trySpawn(i);
                }
                if (!onSpline) {
                    posX = static_cast<float>(static_cast<double>(next.dx) * part + static_cast<double>(posX));
                    posY = static_cast<float>(static_cast<double>(next.dy) * part + static_cast<double>(posY));
                }
                scaleX = scaleX + part * next.sxr;
                scaleY = scaleY + part * next.syr;
                rot = rot + part * next.rot;
                if (remaining < e.dur) break;
                remaining -= e.dur;
            }
            float const moveX = posX - static_cast<float>(cmd.curX);
            float const moveY = posY - static_cast<float>(cmd.curY);
            if (moveX != 0.f || moveY != 0.f || cmd.finishRelated) {
                out.move = true;
                out.dx = static_cast<double>(moveX);
                out.dy = static_cast<double>(moveY);
            }
            if (rot - cmd.rotVal != 0.0 || cmd.finishRelated) {
                out.rotate = true;
                out.rot = rot - cmd.rotVal;
            }
            if (cmd.finishRelated || (0.01 <= std::fabs(scaleX) && 0.01 <= std::fabs(scaleY))) {
                if (scaleX != cmd.modX || scaleY != cmd.modY || cmd.finishRelated) {
                    out.scale = true;
                    out.sNew = scaleX;
                    out.tNew = scaleY;
                    out.sOld = cmd.modX;
                    out.tOld = cmd.modY;
                    out.rot0 = rot0;
                }
                cmd.modX = scaleX;
                cmd.modY = scaleY;
            }
            cmd.curX = static_cast<double>(posX);
            cmd.curY = static_cast<double>(posY);
            cmd.rotVal = rot;
            if (cmd.duration <= cmd.deltaTime) cmd.finished = true;
        }

        // tryGetObject of the game: main object of the group, otherwise a random member
        int tryGetObjectRef(Game& g, int gr) {
            if (gr < 1) return kRefNone;
            World const& w = *g.world;
            int const mainObj = w.mainObjectOf(gr);
            if (mainObj >= 0) return mainObj;
            auto const& objs = w.allOfGroup(gr);
            if (objs.empty()) return kRefNone;
            tl_sideEffect = true;
            float roll = g.rand01();
            if (roll == 1.f) roll = 0.f;
            size_t const pick = static_cast<size_t>(static_cast<int>(static_cast<float>(objs.size()) * roll));
            return objs[std::min(pick, objs.size() - 1)];
        }

        // player targeted by the trigger (player 2 only in dual)
        int playerRef(Game const& g, Obj const& o) {
            if (o.tf(Obj::TF_TargetP1) || (o.tf(Obj::TF_TargetP2) && !g.isDualMode)) return kRefP1;
            if (o.tf(Obj::TF_TargetP2)) return kRefP2;
            return kRefNone;
        }

        // real position of a ref
        Point refPos(Game& g, int ref) {
            if (ref == kRefP1) {
                tl_sideEffect = true;
                return {g.p1.x, g.p1.y};
            }
            if (ref == kRefP2) {
                tl_sideEffect = true;
                return {g.p2.x, g.p2.y};
            }
            return g.obj(ref).getRealPosition();
        }

        // move of a dynamic move this step: (to - from) * part, dir mode moves dist toward to
        void dynMoveDelta(Game& g, int from, int to, float part, bool dir, float dist, double& moveX, double& moveY) {
            Point const fromPos = refPos(g, from);
            Point const toPos = refPos(g, to);
            float offX = toPos.x - fromPos.x;
            float offY = toPos.y - fromPos.y;
            if (dir) {
                float const len = std::sqrt(offX * offX + offY * offY);
                if (len > 0.f) {
                    offX = offX / len * dist;
                    offY = offY / len * dist;
                } else {
                    offX = dist;
                    offY = 0.f;
                }
            }
            moveX = static_cast<double>(offX * part);
            moveY = static_cast<double>(offY * part);
        }

        float refRot(Game& g, int ref) {
            if (ref == kRefP1) {
                tl_sideEffect = true;
                return g.p1.rotation;
            }
            if (ref == kRefP2) {
                tl_sideEffect = true;
                return g.p2.rotation;
            }
            return g.obj(ref).rotation();
        }

        // brings angle back in [-range, range]
        float wrapAngle(float angle, float range) {
            if (angle < -range) {
                float const turns = std::ceil(std::floor(std::fabs(angle) / range) * 0.5f);
                return turns * (range + range) + angle;
            }
            if (range < angle) {
                float const turns = std::ceil(std::floor(std::fabs(angle) / range) * 0.5f);
                return angle - turns * (range + range);
            }
            return angle;
        }

        // rotateTargetAngle of the game: rotation that turns the target toward the aim, clamped by min / max objects
        float rotateTargetAngle(Game& g, Obj const& trig, int center, int target, int aim, int mnx, int mny, int mxx,
                                int mxy) {
            Point const centerPos = refPos(g, center);
            Point aimPos = refPos(g, aim);
            float current;
            if (center == target)
                current = refRot(g, target);
            else {
                Point const targetPos = refPos(g, target);
                float const dx = targetPos.x - centerPos.x;
                float const dy = targetPos.y - centerPos.y;
                float const len = std::sqrt(dx * dx + dy * dy);
                if (0.01 <= static_cast<double>(len))
                    current = 90.f - std::atan2(dy, dx) * 57.29578f;
                else
                    current = refRot(g, target);
            }
            float wanted = 0.f;
            if (center != aim) {
                if (!trig.tf(Obj::TF_UseMoveTarget))
                    wanted = refRot(g, aim);
                else {
                    if (mnx != kRefNone) {
                        float const bound = refPos(g, mnx).x;
                        if (aimPos.x < bound) aimPos.x = bound;
                    }
                    if (mny != kRefNone) {
                        float const bound = refPos(g, mny).y;
                        if (aimPos.y < bound) aimPos.y = bound;
                    }
                    if (mxx != kRefNone) {
                        float const bound = refPos(g, mxx).x;
                        if (bound < aimPos.x) aimPos.x = bound;
                    }
                    if (mxy != kRefNone) {
                        float const bound = refPos(g, mxy).y;
                        if (bound < aimPos.y) aimPos.y = bound;
                    }
                    float const dx = aimPos.x - centerPos.x;
                    float const dy = aimPos.y - centerPos.y;
                    float const len = std::sqrt(dx * dx + dy * dy);
                    if (0.01 <= static_cast<double>(len)) wanted = 90.f - std::atan2(dy, dx) * 57.29578f;
                }
            }
            return wrapAngle((wanted - current) + trig.rotOffset, 180.f);
        }

        // triggerDynamicRotateCommand of the game: center, target and aim resolved now, in that order
        void triggerDynamicRotateCommand(Game& g, int idx, Obj const& o) {
            int const centerRef = tryGetObjectRef(g, o.centerGroup != 0 ? o.centerGroup : o.targetGroup);
            int const targetRef = tryGetObjectRef(g, o.targetGroup);
            int aimRef = playerRef(g, o);
            if (aimRef == kRefNone) aimRef = tryGetObjectRef(g, o.rotTarget);
            if (centerRef == kRefNone || aimRef == kRefNone || targetRef == kRefNone) return;
            DynRot rot;
            rot.trig = idx;
            rot.uid = o.uid;
            rot.control = o.controlID;
            rot.target = o.targetGroup;
            rot.center = o.centerGroup;
            rot.c = centerRef;
            rot.a = aimRef;
            rot.t = targetRef;
            rot.mnx = tryGetObjectRef(g, o.rotMinX);
            rot.mny = tryGetObjectRef(g, o.rotMinY);
            rot.mxx = tryGetObjectRef(g, o.rotMaxX);
            rot.mxy = tryGetObjectRef(g, o.rotMaxY);
            rot.first = 1;
            if (!g.world->relevantGroup(o.targetGroup)) return;
            TrigHeavy& heavy = g.trig.w();
            heavy.Mw().drots.push_back(rot);
            heavy.anyMotion = true;
        }

        // triggerMoveCommand of the game: works out the move (normal, move to, direction, dynamic), creates the command
        void triggerMoveCommand(Game& g, int idx) {
            Obj const& o = objAt(g, idx);
            double dx = static_cast<double>(o.mvX);
            double dy = static_cast<double>(o.mvY);
            {
                float duration = o.duration;
                if (duration < 1.f) duration = 1.f;
                float const quotient = o.mvX / duration;
                int const quotientInt = (std::isnan(quotient) || quotient >= 2147483648.f || quotient < -2147483648.f)
                                                ? INT_MIN
                                                : static_cast<int>(quotient);
                if (static_cast<unsigned>(quotientInt) + 999999u > 0x1e847eu) return;
            }
            bool lockX = o.tf(Obj::TF_LockPX);
            bool lockY = o.tf(Obj::TF_LockPY);
            bool lockCX = o.tf(Obj::TF_LockCX);
            bool lockCY = o.tf(Obj::TF_LockCY);
            float modX = o.moveModX;
            float modY = o.moveModY;
            if ((lockX || lockCX) && std::fabs(modX) > 99999.f) return;
            if ((lockY || lockCY) && std::fabs(modY) > 99999.f) return;
            bool const targetMode = o.tf(Obj::TF_UseMoveTarget) || o.tf(Obj::TF_DirSnap);
            auto tryGetObject = [&](int gr) -> int {
                if (gr < 1) return -1;
                World const& w = *g.world;
                int const mainObj = w.mainObjectOf(gr);
                if (mainObj >= 0) return mainObj;
                auto const& objs = w.allOfGroup(gr);
                if (objs.empty()) return -1;
                tl_sideEffect = true;
                float roll = g.rand01();
                if (roll == 1.f) roll = 0.f;
                size_t const pick = static_cast<size_t>(static_cast<int>(static_cast<float>(objs.size()) * roll));
                return objs[std::min(pick, objs.size() - 1)];
            };
            if (o.tf(Obj::TF_Dynamic) && targetMode) {
                int const from = tryGetObjectRef(g, o.targetModCenter > 0 ? o.targetModCenter : o.targetGroup);
                int to = playerRef(g, o);
                if (to == kRefNone) to = tryGetObjectRef(g, o.centerGroup);
                if (from == kRefNone || to == kRefNone) return;
                if (!g.world->relevantGroup(o.targetGroup)) return;
                DynRot rot;
                rot.kind = 1;
                rot.trig = idx;
                rot.uid = o.uid;
                rot.control = o.controlID;
                rot.target = o.targetGroup;
                rot.center = o.centerGroup;
                rot.c = from;
                rot.a = to;
                rot.t = kRefNone;
                rot.mnx = rot.mny = rot.mxx = rot.mxy = kRefNone;
                rot.first = 1;
                TrigHeavy& heavy = g.trig.w();
                heavy.Mw().drots.push_back(rot);
                heavy.anyMotion = true;
                return;
            }
            if (targetMode) {
                World const& w = *g.world;
                auto pick = [&](int gr) {
                    if (w.enable22) return tryGetObject(gr);
                    if (gr < 1) return -1;
                    int const mainObj = w.mainObjectOf(gr);
                    if (mainObj >= 0) return mainObj;
                    auto const& objs = w.allOfGroup(gr);
                    return objs.size() == 1 ? objs[0] : -1;
                };
                int const fromGroup = o.targetModCenter > 0 ? o.targetModCenter : o.targetGroup;
                int const fromIdx = pick(fromGroup);
                Point from{};
                Point to{};
                bool fromOk = fromIdx >= 0;
                bool toOk = false;
                int toUid = -1;
                if (fromOk) from = g.obj(fromIdx).getRealPosition();
                if (o.tf(Obj::TF_TargetP1) || (o.tf(Obj::TF_TargetP2) && !g.isDualMode)) {
                    to = {g.p1.x, g.p1.y};
                    toOk = true;
                    toUid = 1;
                    tl_sideEffect = true;
                } else if (o.tf(Obj::TF_TargetP2)) {
                    to = {g.p2.x, g.p2.y};
                    toOk = true;
                    toUid = 2;
                    tl_sideEffect = true;
                } else {
                    int const toIdx = pick(o.centerGroup);
                    if (toIdx >= 0) {
                        to = g.obj(toIdx).getRealPosition();
                        toOk = true;
                        toUid = objAt(g, toIdx).uid;
                    }
                }
                float offX = 0.f;
                float offY = 0.f;
                if (fromOk && toOk && objAt(g, fromIdx).uid != toUid) {
                    offX = to.x - from.x;
                    offY = to.y - from.y;
                }
                if (o.tf(Obj::TF_DirSnap)) {
                    float const len = std::sqrt(offX * offX + offY * offY);
                    if (len > 0.f) {
                        offX = offX / len * o.dirDistance;
                        offY = offY / len * o.dirDistance;
                    } else {
                        offX = o.dirDistance;
                        offY = 0.f;
                    }
                }
                if (o.tf(Obj::TF_UseMoveTarget)) {
                    if (o.moveTargetMode == 1)
                        offY = 0.f;
                    else if (o.moveTargetMode == 2)
                        offX = 0.f;
                }
                dx = static_cast<double>(offX);
                dy = static_cast<double>(offY);
                {
                    if (debugOptions().trigLogAt(g.tick)) {
                        std::printf(
                                "[triglog]   move to : target %d from group %d (object %d) to group %d (object %d) : "
                                "(%.1f, %.1f)\n",
                                o.targetGroup, fromGroup, fromIdx >= 0 ? w.objs[static_cast<size_t>(fromIdx)].uid : -1,
                                o.centerGroup, toUid, offX, offY);
                    }
                }
                lockX = lockY = lockCX = lockCY = false;
            }
            if (!o.tf(Obj::TF_Silent)) {
                createMoveCommand(g, dx, dy, o.targetGroup, o.duration, o.easing, o.easingRate, lockX, lockY, lockCX,
                                  lockCY, modX, modY, o.uid, o.controlID);
                return;
            }
            moveGroup(g, o.targetGroup, dx, dy, false);
            if (isPG(g, o.targetGroup) && !tl_canon) {
                pRead();
                if (g.trig.pv)
                    if (auto* off = g.trig.wp().off.find(static_cast<short>(o.targetGroup))) {
                        off->px = off->x;
                        off->py = off->y;
                    }
            } else if (g.trig.r().M().groupOff.find(static_cast<short>(o.targetGroup))) {
                auto* off = g.trig.w().Mw().groupOff.find(static_cast<short>(o.targetGroup));
                off->px = off->x;
                off->py = off->y;
            }
            if (g.trig.pv && !g.trig.pv->dyn.items.empty() && !tl_canon) {
                bool any = false;
                for (auto const& e : g.trig.pv->dyn.items) {
                    for (short gr : objAt(g, e.first).groups)
                        if (gr == o.targetGroup) {
                            any = true;
                            break;
                        }
                    if (any) break;
                }
                if (any) {
                    pRead();
                    for (auto& e : g.trig.wp().dyn.items) {
                        for (short gr : objAt(g, e.first).groups)
                            if (gr == o.targetGroup) {
                                e.second.lastX = static_cast<float>(e.second.x);
                                e.second.lastY = static_cast<float>(e.second.y);
                                break;
                            }
                    }
                }
            }
            if (anyDynInGroup(g, o.targetGroup)) {
                dynDep(2);
                for (auto& e : g.trig.wd().objDyn) {
                    Obj const& member = objAt(g, e.first);
                    for (short gr : member.groups)
                        if (gr == o.targetGroup) {
                            fyCheck(e.first, 5);
                            e.second.lastX = static_cast<float>(e.second.x);
                            e.second.lastY = static_cast<float>(e.second.y);
                            break;
                        }
                }
            }
        }

    }
}
