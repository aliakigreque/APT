// Moves of one step: applies the move, rotate, follow and transform commands to the groups (GJBaseGameLayer::processMoveActions, processRotationActions, processTransformActions).

#include "triggers_internal.hpp"

namespace ap::sim {
    namespace trigger_impl {
        // the move step (prepareMoveActions and what follows), dtSec = trigger step, dtPhys = physics step
        namespace {
            // The moves of one step (processMoveActions and the rotate, follow and transform actions), done in stages; the state that several stages share is members.
            struct MoveStep {
                MoveStep(Game& g, double dtSec, float dtPhys) : g(g), dtSec(dtSec), dtPhys(dtPhys) {}

                Game& g;
                double dtSec;
                float dtPhys;

                TrigState* T = nullptr;
                World const* w = nullptr;
                float dt;
                bool newPath;
                bool canon;
                bool lockPX;
                bool lockPY;
                bool lockCam;
                bool hasFollowY;
                bool hasFollow;
                bool lockP;
                bool hasAdv;
                bool hasAdvP;
                std::vector<int> const* fySet;
                bool fySplit;
                bool split;
                bool useMemo;
                struct FyScope {
                    FyScope(std::vector<int> const* s, Game& g) {
                        tl_fyExcl = s;
                        tl_fyHit = false;
                        tl_applyGame = &g;
                    }
                    ~FyScope() {
                        tl_fyExcl = nullptr;
                        tl_dBefore.reset();
                        tl_applyGame = nullptr;
                    }
                };
                std::optional<FyScope> fyScope;
                bool pruneTick;
                int cellBehind;
                uint64_t key;
                static inline thread_local std::vector<FollowYCmd> followY;
                bool computed;
                uint64_t mkey;
                uint64_t keysBefore;
                std::shared_ptr<TrigDyn> fullBefore;
                // how far the player moved this step, for the moves locked to the player (zero after a death or a big jump)
                Flow measurePlayerMove();
                // which commands lock to the player or the camera, and which follow or advanced follow the player
                void findLocks();
                // when the result does not depend on this path, looks the step up in the shared memo first
                void lookUpMemo();
                // applies the commands of the step to the groups, when the memo did not have them
                void computeMoves();
                // player follow steps of a split step, then drops the entries that came back to their start
                void finishMoves();

                // state and stages of computeMoves()
                TrigHeavy* H = nullptr;
                struct Pending {
                    int group;
                    double dx, dy;
                    bool stampOnly;
                    bool claimed;
                    std::vector<JTerm> terms;
                    bool refused = false;
                };
                static inline thread_local std::vector<Pending> pending;
                struct KfRot {
                    int target;
                    double rot;
                    bool finishing;
                };
                struct KfScale {
                    int target;
                    double sNew, tNew, sOld, tOld;
                    bool finishing;
                };
                struct KfDmc {
                    int group;
                    float dx, dy;
                    int center;
                };
                static inline thread_local std::vector<KfDmc> kfDmc;
                struct TfReg {
                    uint8_t kind;
                    int i;
                };
                static inline thread_local std::vector<TfReg> tfReg;
                static inline thread_local std::vector<KfRot> kfRots;
                static inline thread_local std::vector<KfScale> kfScales;
                static inline thread_local std::vector<KfSpawn> kfSpawns;
                struct RotReg {
                    int kind, i;
                };
                static inline thread_local std::vector<RotReg> rotReg;
                bool kfSideEffect;
                Pending& pendingFor(int group);
                std::vector<GroupCmd>* cmdsP;
                struct DynR {
                    int target, center;
                    float rot;
                    bool finishing, used;
                };
                static inline thread_local std::vector<DynR> dynRs;
                struct RE {
                    int target, center;
                    float d, dObj;
                    bool finishing, stamped, processed;
                };
                static inline thread_local std::vector<RE> re;
                size_t nOrd;
                bool multiRot;
                bool inClaim(size_t j, int t, int c);
                void claimRot(int t, int c, float& rot, float& rotOff);
                Pending* claimMove(int target, float& pdx, float& pdy);
                void claimedToGroupOff(int target, float pdx, float pdy);
                static double clampF(double v);
                void applyTransform(int target, int centerGroup, int centerIdx, double fxN, double fxO, double fyN,
                                    double fyO, bool onlyMove, bool relRot);
                void applyRotation(int target, int centerIdx, float rot, float rotObj, bool finishing);
                int rotCenterOf(int target, int center);
                // saves the state before the step for the memo and empties the tables of the step
                void prepareCommands();
                // steps every command (move, rotate, follow, transform, keyframe) and registers what it does to its group
                void stepCommands();
                // rotations of the rotate triggers that aim at or follow an object
                void stepDynamicRotations();
                // lists the rotations to apply in the game's order; without multi rotation only one per target counts
                void orderRotations();
                // applies the scale and transform commands to their groups
                void applyTransforms();
                // applies the rotations, then the moves of the dynamic rotations
                void applyRotations();
                // applies the moves of the keyframe commands
                void applyKeyframeMoves();
                // applies the moves collected for each group (on the journal for player groups)
                void applyPendingMoves();
                // collects the follow player Y commands, stepped after the other moves
                void collectFollowY();
                // on the canonical path, records the follow and follow Y steps in the journal
                void journalFollows();
                // applies the follow commands (one group follows another)
                void applyFollows();
                // clears the deltas of the commands for the next step
                void clearDeltas();
                // stores the result of the step in the memo, unless a keyframe had side effects
                void commitMemo();

                // the whole of trigMoveActionsImpl, one stage after another
                void run();
            };
        }

        // how far the player moved this step, for the moves locked to the player (zero after a death or a big jump)
        auto MoveStep::measurePlayerMove() -> Flow {
            T = &(g.trig);
            w = &(*g.world);
            dt = static_cast<float>(dtSec);
            newPath = tl_newPath;
            canon = newPath && tl_canon;
            {
                Player const& p = g.p1;
                float dy = p.y - p.lastY;
                if (!g.playerDied &&
                    static_cast<double>(dtPhys) * 16.0 < static_cast<double>(std::fabs(dy - p.yVelocityRelated3)))
                    dy = 0.f;
                float dx = p.x - p.lastX;
                if (g.playerDied) {
                    dx = 0.f;
                    dy = 0.f;
                }
                T->playerDx = dx;
                T->playerDy = dy;
            }
            if (T->r().M().cmds.empty() && T->r().M().advs.empty() && T->r().M().drots.empty()) return Flow::Return;
            memoTick(g);
            return Flow::Next;
        }

        // which commands lock to the player or the camera, and which follow or advanced follow the player
        auto MoveStep::findLocks() -> void {
            lockPX = false;
            lockPY = false;
            lockCam = false;
            hasFollowY = false;
            hasFollow = false;
            for (auto const& c : T->r().M().cmds) {
                if (c.disabled || c.type != CmdType::Move || c.finishRelated) continue;
                if (c.lockPX) lockPX = true;
                if (c.lockPY) lockPY = true;
                if (c.lockCX || c.lockCY) lockCam = true;
            }
            for (auto const& c : T->r().M().cmds) {
                if (c.disabled) continue;
                if (c.type == CmdType::FollowY) hasFollowY = true;
                if (c.type == CmdType::Follow) hasFollow = true;
            }
            lockP = lockPX || lockPY;
            hasAdv = false;
            for (auto const& I : T->r().M().advs) {
                if (!I.rngOnly || !I.processed) {
                    hasAdv = true;
                    break;
                }
            }
            hasAdvP = false;
            for (auto const& I : T->r().M().advs) {
                if ((!I.rngOnly || !I.processed) && advPlayerDep(*w, I)) {
                    hasAdvP = true;
                    break;
                }
            }
            fySet = nullptr;
            fySplit = false;
            if ((hasFollowY || hasAdvP) && !newPath) {
                fySet = &fyObjectsOf(*w, T->r().M().cmds, T->r().M().advs);
                fySplit = true;
                if (hasFollow) {
                    for (auto const& c : T->r().M().cmds) {
                        if (c.type == CmdType::Follow && !c.disabled && !followIndependent(*w, c, *fySet)) {
                            fySplit = false;
                            break;
                        }
                    }
                }
                if (!fySplit) fySet = nullptr;
            }
        }

        // when the result does not depend on this path, looks the step up in the shared memo first
        auto MoveStep::lookUpMemo() -> void {
            split = !newPath && (hasFollowY || hasAdvP) && (!hasFollow || fySplit);
            useMemo = !newPath && !((hasFollowY || hasAdvP) && hasFollow && !fySplit) && memoOn();
            fyScope.emplace(useMemo ? fySet : nullptr, g);
            pruneTick = !g.p1.isGoingLeft && (g.tick & 63) == 0;
            cellBehind = World::cellOf(static_cast<double>(g.p1.x)) - 12;
            key = 0;
            if (useMemo) {
                key = mix64(mix64(mix64(trigHeavyHash(T->r()), bits64(dtSec)), bits64(dtPhys)), 0x3011ull);
                if (!T->afRing.empty())
                    key = mix64(mix64(key, T->afHashObj), (static_cast<uint64_t>(T->afU6 - T->afU5) << 32) | static_cast<uint32_t>(T->afN));
                if (pruneTick) key = mix64(key, static_cast<uint64_t>(static_cast<uint32_t>(cellBehind)));
                if (lockPX) key = mix64(key, bits64(T->playerDx));
                if (lockPY) key = mix64(key, bits64(T->playerDy));
                if (lockCam) key = mix64(mix64(key, bits64(g.cam.sdx)), bits64(g.cam.sdy));
            }
            followY.clear();
            computed = true;
            mkey = 0;
            if (useMemo) {
                if (MemoEntry const* m = memoLookup(*T, key)) {
                    followY = m->fy;
                    computed = false;
                    g.itemChanges += m->itemDelta;
                } else {
                    if (lockP) ++tl_dbg[0];
                    bool withOff = false;
                    for (auto const& I : T->r().M().advs) {
                        if (w->advDefs[static_cast<size_t>(I.def)].ignoreDisabled) {
                            withOff = true;
                            break;
                        }
                    }
                    mkey = mix64(mix64(mix64(motionHash(T->r(), withOff), bits64(dtSec)), bits64(dtPhys)), 0x4d07ull);
                    if (!T->afRing.empty())
                        mkey = mix64(mix64(mkey, T->afHashObj), (static_cast<uint64_t>(T->afU6 - T->afU5) << 32) | static_cast<uint32_t>(T->afN));
                    if (pruneTick) mkey = mix64(mkey, static_cast<uint64_t>(static_cast<uint32_t>(cellBehind)));
                    if (lockPX) mkey = mix64(mkey, bits64(T->playerDx));
                    if (lockPY) mkey = mix64(mkey, bits64(T->playerDy));
                    if (lockCam) mkey = mix64(mix64(mkey, bits64(g.cam.sdx)), bits64(g.cam.sdy));
                    if (MemoEntry const* m = motionLookup(*T, mkey)) {
                        followY = m->fy;
                        computed = false;
                        ++tl_dbg[7];
                        g.itemChanges += m->itemDelta;
                    }
                }
            } else
                ++tl_dbg[1];
            keysBefore = 0;
        }

        // applies the commands of the step to the groups, when the memo did not have them
        auto MoveStep::computeMoves() -> void {
            if (computed) {
                prepareCommands();
                stepCommands();
                stepDynamicRotations();
                orderRotations();
                applyTransforms();
                applyRotations();
                applyKeyframeMoves();
                applyPendingMoves();
                collectFollowY();
                journalFollows();
                applyFollows();
                clearDeltas();
                commitMemo();
            }
        }

        auto MoveStep::pendingFor(int group) -> Pending& {
            for (auto& p : pending)
                if (p.group == group) return p;
            pending.push_back(Pending{group, 0.0, 0.0, false, false, {}});
            return pending.back();
        }

        auto MoveStep::inClaim(size_t j, int t, int c) -> bool {
            if (re[j].target != t || re[j].center != c) return false;
            if (multiRot) return true;
            for (size_t k = j + 1; k < re.size(); ++k)
                if (re[k].target == t && re[k].center == c) return false;
            return true;
        }

        auto MoveStep::claimRot(int t, int c, float& rot, float& rotOff) -> void {
            for (size_t j = 0; j < re.size(); ++j) {
                if (re[j].processed || !inClaim(j, t, c)) continue;
                re[j].processed = true;
                rot = re[j].d + rot;
                rotOff = re[j].dObj + rotOff;
            }
        }

        auto MoveStep::claimMove(int target, float& pdx, float& pdy) -> Pending* {
            for (auto& p : pending) {
                if (p.group != target) continue;
                if (p.claimed || p.refused) return nullptr;
                p.claimed = true;
                pdx = static_cast<float>(p.dx);
                pdy = static_cast<float>(p.dy);
                return &p;
            }
            return nullptr;
        }

        auto MoveStep::claimedToGroupOff(int target, float pdx, float pdy) -> void {
            if (pdx == 0.f && pdy == 0.f) return;
            if (PSet const* psU = (canon && tl_jr && !tl_jrImm) ? psetOf(g) : nullptr;
                psU && !bitOf(psU->groupBits, target) && bitOf(psU->memberGroupBits, target)) {
                GroupOff const* cur = T->r().M().groupOff.find(static_cast<short>(target));
                UndoE u{};
                u.group = target;
                u.pos = static_cast<int>(tl_jr->ops.size());
                u.absent = cur == nullptr;
                if (cur) u.old = *cur;
                tl_jr->undo.push_back(u);
            }
            TrigHeavy& H2 = T->w();
            GroupOff& off = H2.Mw().groupOff[static_cast<short>(target)];
            markGroupMoved(H2, static_cast<short>(target));
            if (off.stamp != tickStamp(g)) {
                off.px = off.x;
                off.py = off.y;
                off.stamp = tickStamp(g);
            }
            off.x += static_cast<double>(pdx);
            off.y += static_cast<double>(pdy);
            H2.anyMotion = true;
        }

        auto MoveStep::clampF(double v) -> double {
            return (std::fabs(v) <= 0.01 && std::fabs(v) != 0.01) ? (v < 0.0 ? -0.01 : 0.01) : v;
        }

        auto MoveStep::applyTransform(int target, int centerGroup, int centerIdx, double fxN, double fxO, double fyN, double fyO, bool onlyMove, bool relRot) -> void {
            PSet const* const psS = canon ? psetOf(g) : nullptr;
            bool const targetP = psS && bitOf(psS->memberGroupBits, target);
            if (centerIdx < 0) {
                if (onlyMove) return;
                if (psS && targetP) {
                    if (!tl_jr) {
                        pHitAt();
                        return;
                    }
                    JOp op{};
                    op.kind = JOp::SCALE;
                    op.group = target;
                    op.center = -1;
                    op.onlyMove = false;
                    op.tfN[0] = fxN;
                    op.tfO[0] = fxO;
                    op.tfN[1] = fyN;
                    op.tfO[1] = fyO;
                    jrPush(op);
                }
                double const rx = fxN / fxO;
                double const ry = fyN / fyO;
                for (int idx : w->allOfGroup(target)) {
                    if (psS && bitOf(psS->objBits, idx)) continue;
                    if (w->decoLikeAt(idx)) continue;
                    ObjDyn& d = dynEntry(g, idx);
                    sclPrev(g, d);
                    tfScaleNoCenter(d, rx, ry);
                }
                return;
            }
            bool const centerP = psS && bitOf(psS->objBits, centerIdx);
            bool centerNonP = false;
            if (psS && (targetP || centerP)) {
                if (!tl_jr) {
                    pHitAt();
                    return;
                }
                if (centerP && hasNonPMember(g, psS, target)) {
                    if (tl_canonRead) {
                        pHitAt();
                        return;
                    }
                    centerNonP = true;
                }
            }
            Obj const& center = objAt(g, centerIdx);
            float cx = 0.f;
            float cy = 0.f;
            float centerRot = 0.f;
            if (!centerP || centerNonP) {
                double ccx, ccy;
                realPosBase(g, centerIdx, ccx, ccy);
                cx = static_cast<float>(ccx - static_cast<double>(center.posOffX));
                cy = static_cast<float>(ccy - static_cast<double>(center.posOffY));
                if (relRot) centerRot = center.rotX + trigRotation(g, centerIdx) + 0.f;
            }
            float rot = 0.f;
            float rotOff = 0.f;
            claimRot(target, centerGroup, rot, rotOff);
            float pdx = 0.f;
            float pdy = 0.f;
            Pending* const claimed = claimMove(target, pdx, pdy);
            if (psS && (targetP || centerP)) {
                JOp op{};
                op.kind = JOp::SCALE;
                op.group = target;
                op.center = centerIdx;
                op.cP = centerP;
                op.cx = cx;
                op.cy = cy;
                op.onlyMove = onlyMove;
                op.tfN[0] = fxN;
                op.tfO[0] = fxO;
                op.tfN[1] = fyN;
                op.tfO[1] = fyO;
                op.rot = rot;
                op.rotObj = rotOff;
                op.pdx = pdx;
                op.pdy = pdy;
                op.scr = centerRot;
                op.scrP = relRot && centerP;
                if (claimed && bitOf(psS->groupBits, target)) {
                    op.t0 = static_cast<int>(tl_jr->terms.size());
                    tl_jr->terms.insert(tl_jr->terms.end(), claimed->terms.begin(), claimed->terms.end());
                    op.t1 = static_cast<int>(tl_jr->terms.size());
                    op.claimed = true;
                }
                jrPush(op);
                if (centerP && !centerNonP) return;
            }
            Aff const P = tfWorld(cx, cy, fxN, fxO, fyN, fyO, rot + centerRot, centerRot);
            for (int idx : (tl_remap.idx >= 0 ? w->allOfGroup(target) : physOfGroup(*w, target))) {
                if (psS && bitOf(psS->objBits, idx)) continue;
                Obj const& o = objAt(g, idx);
                if (w->decoLikeAt(idx) && !w->centerLike[static_cast<size_t>(idx)]) continue;
                ObjDyn& d = dynEntry(g, idx);
                double const oldX = d.x;
                double const oldY = d.y;
                if (o.type != Decoration) sclPrev(g, d);
                stampLast(g, d);
                tfObj(o, d, P, cx, cy, pdx, pdy, onlyMove, rotOff);
                if (isPO(g, idx)) {
                    d.secX = d.x;
                    d.secY = d.y;
                } else
                    resection(g, idx, oldX, oldY, d.x, d.y);
            }
            claimedToGroupOff(target, pdx, pdy);
        }

        auto MoveStep::applyRotation(int target, int centerIdx, float rot, float rotObj, bool finishing) -> void {
            if (rot == 0.f && !finishing) return;
            auto const& objs = w->allOfGroup(target);
            PSet const* const psR = canon ? psetOf(g) : nullptr;
            bool const targetP = psR && bitOf(psR->memberGroupBits, target);
            bool const centerP = psR && centerIdx >= 0 && bitOf(psR->objBits, centerIdx);
            if (psR && (targetP || centerP)) {
                if (!tl_jr) {
                    pHitAt();
                    return;
                }
            }
            if (centerIdx < 0) {
                if (rotObj == 0.f) return;
                if (psR && targetP) {
                    JOp op{};
                    op.kind = JOp::ROT;
                    op.group = target;
                    op.center = -1;
                    op.rot = rot;
                    op.rotObj = rotObj;
                    op.finishRelated = finishing;
                    jrPush(op);
                }
                for (int idx : (tl_remap.idx >= 0 ? objs : physOfGroup(*w, target))) {
                    if (psR && bitOf(psR->objBits, idx)) continue;
                    Obj const& o = objAt(g, idx);
                    if (w->decoLikeAt(idx) && !w->centerLike[static_cast<size_t>(idx)]) continue;
                    if (!o.canRotateFree()) continue;
                    ObjDyn& d = dynEntry(g, idx);
                    d.rotAccY = rotObj + d.rotAccY;
                    d.rotAcc = rotObj + d.rotAcc;
                    d.rotated = true;
                }
                return;
            }
            Obj const& center = objAt(g, centerIdx);
            float cx = 0.f;
            float cy = 0.f;
            bool const centerNonP = centerP && hasNonPMember(g, psR, target);
            if (centerNonP && tl_canonRead) {
                pHitAt();
                return;
            }
            if (!centerP || centerNonP) {
                double ccx, ccy;
                realPosBase(g, centerIdx, ccx, ccy);
                cx = static_cast<float>(ccx - static_cast<double>(center.posOffX));
                cy = static_cast<float>(ccy - static_cast<double>(center.posOffY));
            }
            float pdx = 0.f;
            float pdy = 0.f;
            Pending* const claimed = claimMove(target, pdx, pdy);
            if (psR && (targetP || centerP)) {
                JOp op{};
                op.kind = JOp::ROT;
                op.group = target;
                op.center = centerIdx;
                op.cP = centerP;
                op.cx = cx;
                op.cy = cy;
                op.rot = rot;
                op.rotObj = rotObj;
                op.pdx = pdx;
                op.pdy = pdy;
                op.finishRelated = finishing;
                if (claimed && bitOf(psR->groupBits, target)) {
                    op.t0 = static_cast<int>(tl_jr->terms.size());
                    tl_jr->terms.insert(tl_jr->terms.end(), claimed->terms.begin(), claimed->terms.end());
                    op.t1 = static_cast<int>(tl_jr->terms.size());
                    op.claimed = true;
                }
                jrPush(op);
            }
            float const rad = -rot * 0.01745329252f;
            float const cs = gcos(rad);
            float const sn = gsin(rad);
            float const ta = cs;
            float const tb = sn;
            float const tcf = -sn;
            float const td = cs;
            TrigDyn* dynState = nullptr;
            size_t hint = 0;
            for (int idx : (tl_remap.idx >= 0 ? objs : physOfGroup(*w, target))) {
                if (psR && bitOf(psR->objBits, idx)) continue;
                Obj const& o = objAt(g, idx);
                if (w->decoLikeAt(idx) && !w->centerLike[static_cast<size_t>(idx)]) continue;
                if (!dynState) {
                    dynDep(2);
                    dynState = &g.trig.wd();
                }
                fyCheck(idx, 7);
                ObjDyn* dynPtr = isPO(g, idx) ? &dynEntryP(g, idx) : dynState->objDyn.findHint(idx, hint);
                if (!dynPtr) {
                    dynEntry(g, idx);
                    dynState = &g.trig.wd();
                    hint = 0;
                    dynPtr = dynState->objDyn.findHint(idx, hint);
                }
                ObjDyn& d = *dynPtr;
                double const oldX = d.x;
                double const oldY = d.y;
                stampLast(g, d);
                float const rx = static_cast<float>((d.x - static_cast<double>(o.posOffX)) - static_cast<double>(cx));
                float const ry = static_cast<float>((d.y - static_cast<double>(o.posOffY)) - static_cast<double>(cy));
                float const px = static_cast<float>(static_cast<double>(ta) * rx + static_cast<double>(tcf) * ry + static_cast<double>(cx));
                float const py = static_cast<float>(static_cast<double>(tb) * rx + static_cast<double>(td) * ry + static_cast<double>(cy));
                d.x = (static_cast<double>(pdx + px) - d.x) + static_cast<double>(o.posOffX) + d.x;
                d.y = (static_cast<double>(pdy + py) - d.y) + static_cast<double>(o.posOffY) + d.y;
                if (rotObj != 0.f && o.canRotateFree()) {
                    d.rotAccY = rotObj + d.rotAccY;
                    d.rotAcc = rotObj + d.rotAcc;
                    d.rotated = true;
                }
                (void)oldX;
                (void)oldY;
                if (isPO(g, idx)) {
                    d.secX = d.x;
                    d.secY = d.y;
                } else
                    resectionDyn(g, idx, d, d.x, d.y);
            }
            claimedToGroupOff(target, pdx, pdy);
        }

        auto MoveStep::rotCenterOf(int target, int center) -> int {
            int centerIdx = w->mainObjectOf(center);
            if (target == center) {
                int const parent = w->exportParentOf(center);
                if (parent >= 0) centerIdx = parent;
            }
            return centerIdx;
        }

        // saves the state before the step for the memo and empties the tables of the step
        auto MoveStep::prepareCommands() -> void {
            if (useMemo) {
                keysBefore = dynKeysHash(T->rd());
                fullBefore = T->d;
                tl_dynDep = 0;
                tl_itemDelta = 0;
                tl_sideEffect = false;
                spawnMark(*T);
                tl_fyHit = false;
                if (tl_fyExcl) tl_dBefore = T->d;
            }
            H = &(T->w());
            pending.clear();
            rotReg.clear();
            tfReg.clear();
            kfDmc.clear();
            kfRots.clear();
            kfScales.clear();
            kfSpawns.clear();
            kfSideEffect = false;
            cmdsP = &H->Mw().cmds;
        }

        // steps every command (move, rotate, follow, transform, keyframe) and registers what it does to its group
        auto MoveStep::stepCommands() -> void {
            for (size_t cmdIdx = 0; cmdIdx < cmdsP->size(); ++cmdIdx) {
                GroupCmd& c = (*cmdsP)[cmdIdx];
                if (c.disabled) continue;
                switch (c.type) {
                case 0: {
                    Pending& pn = pendingFor(c.target);
                    if (!c.finishRelated) {
                        stepCmd(c, dt);
                        double dx = c.deltaX;
                        double dy = c.deltaY;
                        if (c.lockPX)
                            dx = static_cast<double>(T->playerDx) * c.modX;
                        else if (c.lockCX)
                            dx = static_cast<double>(-g.cam.sdx) * c.modX;
                        if (canon && (c.lockPY || c.lockCY) && isPG(g, c.target)) {
                            pn.terms.push_back(JTerm{dx, 0.0, c.modX, c.modY, static_cast<uint8_t>(c.lockPY ? 2 : 8)});
                            pn.dx += dx;
                            pn.dy += 0.0;
                            break;
                        }
                        if (c.lockPY)
                            dy = static_cast<double>(T->playerDy) * c.modY;
                        else if (c.lockCY)
                            dy = static_cast<double>(static_cast<float>(static_cast<double>(-g.cam.sdy) * c.modY));
                        if (canon && isPG(g, c.target)) pn.terms.push_back(JTerm{dx, dy, 0.0, 0.0, 0});
                        pn.dx += dx;
                        pn.dy += dy;
                    } else {
                        pn.stampOnly = true;
                    }
                    break;
                }
                case 1:
                    c.processed = false;
                    if (!c.finishRelated) {
                        stepCmd(c, dt);
                        c.rotAccPrev = c.rotAcc;
                        c.rotAcc = c.rotAcc + c.rotDelta;
                    } else {
                        c.rotAccPrev = c.rotAcc;
                    }
                    break;
                case 2:
                case 3:
                case 4:
                    if (!c.finishRelated) stepCmd(c, dt);
                    if (c.type == CmdType::Transform) tfReg.push_back(TfReg{0, static_cast<int>(cmdIdx)});
                    break;
                case 5: {
                    KfOut out;
                    kfSpawns.clear();
                    kfStep(g, c, dt, out, kfSpawns);
                    if (out.move) {
                        Pending& pn = pendingFor(c.target);
                        if (canon && isPG(g, c.target)) pn.terms.push_back(JTerm{out.dx, out.dy, 0.0, 0.0, 0});
                        pn.dx += out.dx;
                        pn.dy += out.dy;
                        if (c.finishRelated) pn.stampOnly = true;
                        int const kfCenter = c.kfCenterP1 - 1;
                        if (kfCenter >= 0) {
                            if (canon && isPO(g, kfCenter)) pHitAt();
                            ObjDyn const* centerDyn = dynOf(g, kfCenter);
                            if (trigRotation(g, kfCenter) != 0.f ||
                                (centerDyn && (centerDyn->sclOffX != 0.f || centerDyn->sclOffY != 0.f))) {
                                kfDmc.push_back(KfDmc{c.target, static_cast<float>(out.dx), static_cast<float>(out.dy), kfCenter});
                                pn.refused = true;
                            }
                        }
                    }
                    if (out.rotate) {
                        kfRots.push_back(KfRot{c.target, out.rot, c.finishRelated});
                        rotReg.push_back(RotReg{2, static_cast<int>(kfRots.size()) - 1});
                    }
                    if (out.scale) {
                        kfScales.push_back(KfScale{c.target, out.sNew, out.tNew, out.sOld, out.tOld, c.finishRelated});
                        tfReg.push_back(TfReg{1, static_cast<int>(kfScales.size()) - 1});
                    }
                    if (!kfSpawns.empty()) {
                        static thread_local std::vector<KfSpawn> sp;
                        sp = kfSpawns;
                        kfSpawns.clear();
                        for (auto const& s : sp) {
                            RemapKeys const* keys = s.keys.n ? &s.keys : nullptr;
                            if (s.delay <= 0.f)
                                spawnGroupLayer(g, s.group, false, 0.0, s.uid, s.control, keys);
                            else
                                queueSpawn(g, s.group, s.delay, false, s.uid, s.control, keys);
                        }
                        cmdsP = &T->w().Mw().cmds;
                    }
                    break;
                }
                default:
                    break;
                }
                if ((*cmdsP)[cmdIdx].type == CmdType::Rotate && !(*cmdsP)[cmdIdx].disabled)
                    rotReg.push_back(RotReg{0, static_cast<int>(cmdIdx)});
            }
        }

        // rotations of the rotate triggers that aim at or follow an object
        auto MoveStep::stepDynamicRotations() -> void {
            dynRs.clear();
            if (!T->r().M().drots.empty()) {
                auto& list = T->w().Mw().drots;
                for (size_t i = 0; i < list.size();) {
                    DynRot& e = list[i];
                    if (e.kind != 0) {
                        ++i;
                        continue;
                    }
                    if (e.pause && !e.pauseOnce) {
                        ++i;
                        continue;
                    }
                    Obj const& trigObj = objAt(g, e.trig);
                    float const dur = trigObj.duration;
                    float const left = dur - e.elapsed;
                    DynR r{e.target, e.center, 0.f, false, false};
                    if ((left <= 0.f && dur != -1.f && !e.first) || e.stop || e.pause) {
                        r.finishing = true;
                        dynRs.push_back(r);
                        if (!e.pause) {
                            list.erase(list.begin() + static_cast<long>(i));
                            continue;
                        }
                        e.pauseOnce = 0;
                        ++i;
                        continue;
                    }
                    if (!e.first) {
                        if (dur != -1.f) e.elapsed = (dt <= left ? dt : left) + e.elapsed;
                    } else
                        e.first = 0;
                    float const angle = rotateTargetAngle(g, trigObj, e.c, e.t, e.a, e.mnx, e.mny, e.mxx, e.mxy);
                    int const easing = trigObj.dynEasing < 1 ? 1 : trigObj.dynEasing;
                    r.rot = angle / static_cast<float>(easing);
                    dynRs.push_back(r);
                    ++i;
                }
            }
            T->w();
            for (auto const& e : T->r().M().drots) {
                if (e.kind != 1) continue;
                int const targetGroup = objAt(g, e.trig).targetGroup;
                for (auto& p : pending)
                    if (p.group == targetGroup) p.refused = true;
            }
        }

        // lists the rotations to apply in the game's order; without multi rotation only one per target counts
        auto MoveStep::orderRotations() -> void {
            re.clear();
            for (auto const& r : rotReg) {
                RE e{};
                if (r.kind == 0) {
                    GroupCmd const& c = T->r().M().cmds[static_cast<size_t>(r.i)];
                    e.target = c.target;
                    e.center = c.center;
                    e.d = static_cast<float>(c.rotAcc - c.rotAccPrev);
                    e.dObj = c.lockObjRot ? 0.f : e.d;
                    e.finishing = c.finishRelated || c.finished;
                } else {
                    KfRot const& kfRot = kfRots[static_cast<size_t>(r.i)];
                    e.target = kfRot.target;
                    e.center = kfRot.target;
                    e.d = static_cast<float>(kfRot.rot);
                    e.dObj = e.d;
                    e.finishing = kfRot.finishing;
                }
                re.push_back(e);
            }
            nOrd = re.size();
            for (auto const& r : dynRs) re.push_back(RE{r.target, r.center, r.rot, r.rot, r.finishing, false, false});
            multiRot = w->allowMultiRotation;
            for (size_t i = 0; i < re.size(); ++i) {
                if (multiRot || i >= nOrd) {
                    re[i].stamped = true;
                    continue;
                }
                bool lastOfTarget = true;
                for (size_t j = i + 1; j < nOrd; ++j) {
                    if (re[j].target == re[i].target) {
                        lastOfTarget = false;
                        break;
                    }
                }
                re[i].stamped = lastOfTarget;
            }
        }

        // applies the scale and transform commands to their groups
        auto MoveStep::applyTransforms() -> void {
            for (auto const& tr : tfReg) {
                if (tr.kind == 0) {
                    GroupCmd& c = T->w().Mw().cmds[static_cast<size_t>(tr.i)];
                    if (c.type != CmdType::Transform || c.disabled) continue;
                    double const progNow = c.rotVal;
                    double const progPrev = c.rotAccPrev;
                    if (progNow == progPrev && !(c.finishRelated || c.finished)) continue;
                    double const fxN = clampF((c.modX - 1.0) * progNow + 1.0);
                    double const fxO = clampF((c.modX - 1.0) * progPrev + 1.0);
                    double const fyN = clampF((c.modY - 1.0) * progNow + 1.0);
                    double const fyO = clampF((c.modY - 1.0) * progPrev + 1.0);
                    c.rotAccPrev = progNow;
                    int const target = c.target;
                    int const center = c.center;
                    bool const onlyMove = c.lockObjRot;
                    bool const relRot = c.relRot;
                    int const centerIdx = center > 0 ? w->mainObjectOf(center) : -1;
                    applyTransform(target, center, centerIdx, fxN, fxO, fyN, fyO, onlyMove, relRot);
                } else {
                    KfScale const s = kfScales[static_cast<size_t>(tr.i)];
                    double const fxN = clampF((s.sNew - 1.0) * 1.0 + 1.0);
                    double const fyN = clampF((s.tNew - 1.0) * 1.0 + 1.0);
                    double fxO = (s.sNew - 1.0) * 0.0 + 1.0;
                    double fyO = (s.tNew - 1.0) * 0.0 + 1.0;
                    if (s.sOld != 0.0) {
                        fxO = s.sOld;
                        fyO = s.tOld;
                    }
                    applyTransform(s.target, s.target, w->mainObjectOf(s.target), fxN, clampF(fxO), fyN, clampF(fyO), false, true);
                }
            }
        }

        // applies the rotations, then the moves of the dynamic rotations
        auto MoveStep::applyRotations() -> void {
            {
                for (size_t i = 0; i < re.size(); ++i) {
                    RE& e = re[i];
                    if (!e.stamped || e.processed) continue;
                    if (e.d == 0.f && !e.finishing) continue;
                    e.processed = true;
                    float rot = e.d;
                    float rotObj = e.dObj;
                    for (size_t j = 0; j < re.size(); ++j) {
                        if (re[j].processed || !inClaim(j, e.target, e.center)) continue;
                        re[j].processed = true;
                        rot += re[j].d;
                        rotObj += re[j].dObj;
                    }
                    applyRotation(e.target, rotCenterOf(e.target, e.center), rot, rotObj, e.finishing);
                }
                for (auto& c : T->w().Mw().cmds)
                    if (c.type == CmdType::Rotate && !c.disabled) c.processed = true;
            }
            {
                bool anyMove = false;
                for (auto const& e : T->r().M().drots) {
                    if (e.kind == 1) {
                        anyMove = true;
                        break;
                    }
                }
                if (anyMove) {
                    auto& list = T->w().Mw().drots;
                    for (size_t i = 0; i < list.size();) {
                        DynRot& e = list[i];
                        if (e.kind != 1 || (e.pause && !e.pauseOnce)) {
                            ++i;
                            continue;
                        }
                        Obj const& trigObj = objAt(g, e.trig);
                        float const dur = trigObj.duration;
                        float elapsed = e.elapsed;
                        float const prev = e.prevD;
                        float const left = dur - elapsed;
                        float const rem = dur - prev;
                        if ((left <= 0.f && dur != -1.f && !e.first) || e.stop || e.pause) {
                            Pending& pn = pendingFor(e.target);
                            pn.stampOnly = true;
                            if (!e.pause) {
                                list.erase(list.begin() + static_cast<long>(i));
                                continue;
                            }
                            e.pauseOnce = 0;
                            ++i;
                            continue;
                        }
                        if (!e.first) {
                            if (dur != -1.f) {
                                elapsed = (dt <= left ? dt : left) + elapsed;
                                e.elapsed = elapsed;
                            }
                        } else
                            e.first = 0;
                        float const t = elapsed / dur;
                        float const ePrev = e.eased;
                        float eased = easedValue(t, trigObj.easing, trigObj.easingRate);
                        if (eased == 1.f && dt < left)
                            eased = eased + ((1.f < e.eased || e.eased == 1.f) ? -0.0001f : 0.0001f);
                        e.eased = eased;
                        e.prevD = eased * dur;
                        float f = rem == 0.f ? 0.f : (eased * dur - prev) / rem;
                        if (dur <= 0.f) f = 1.f;
                        bool const dir = trigObj.tf(Obj::TF_DirSnap);
                        float const k = dir ? (dur <= 0.f ? 1.f : rem / dur) * trigObj.dirDistance : 0.f;
                        (void)ePrev;
                        Pending* pn = nullptr;
                        for (auto& p : pending) {
                            if (p.group == e.target) {
                                pn = &p;
                                break;
                            }
                        }
                        if (!pn) {
                            pending.push_back(Pending{e.target, 0.0, 0.0, false, false, {}});
                            pn = &pending.back();
                        }
                        bool const endP = canon && tl_jr && ((e.c >= 0 && isPO(g, e.c)) || (e.a >= 0 && isPO(g, e.a)) || e.a == kRefP1 || e.a == kRefP2 || e.c == kRefP1 || e.c == kRefP2);
                        if (endP && isPG(g, e.target)) {
                            DMove const dm{e.c, e.a, f, k, dir};
                            tl_jr->dm.push_back(dm);
                            int const dmIdx = static_cast<int>(tl_jr->dm.size()) - 1;
                            JTerm term{0.0, 0.0, 0.0, 0.0, 16};
                            term.dm = dmIdx;
                            pn->terms.push_back(term);
                            JOp op{};
                            op.kind = JOp::DMOVE;
                            op.t0 = dmIdx;
                            jrPush(op);
                            ++i;
                            continue;
                        }
                        double mdx = 0.0;
                        double mdy = 0.0;
                        dynMoveDelta(g, e.c, e.a, f, dir, k, mdx, mdy);
                        if (canon && isPG(g, e.target)) pn->terms.push_back(JTerm{mdx, mdy, 0.0, 0.0, 0});
                        pn->dx += mdx;
                        pn->dy += mdy;
                        ++i;
                    }
                }
            }
        }

        // applies the moves of the keyframe commands
        auto MoveStep::applyKeyframeMoves() -> void {
            for (auto const& e : kfDmc) {
                float const rot = trigRotation(g, e.center);
                ObjDyn const* centerDyn = dynOf(g, e.center);
                float const a = -rot * 0.017453292f;
                float const vx = gcos(a) * e.dx - gsin(a) * e.dy;
                float const vy = gsin(a) * e.dx + gcos(a) * e.dy;
                float const sx = static_cast<float>((static_cast<double>(centerDyn ? centerDyn->sclOffX : 0.f) + 1.0) *
                                                    static_cast<double>(vx));
                float const sy = static_cast<float>((static_cast<double>(centerDyn ? centerDyn->sclOffY : 0.f) + 1.0) *
                                                    static_cast<double>(vy));
                float const cx = sx - e.dx;
                float const cy = sy - e.dy;
                for (auto& p : pending)
                    if (p.group == e.group) {
                        if (canon && isPG(g, e.group))
                            p.terms.push_back(JTerm{static_cast<double>(cx), static_cast<double>(cy), 0.0, 0.0, 0});
                        p.dx += static_cast<double>(cx);
                        p.dy += static_cast<double>(cy);
                        break;
                    }
            }
            if (g_trace) {
                for (auto& p : pending) {
                    std::printf("[trig] tick %d : group %d dx %.6f dy %.6f%s%s\n", g.tick, p.group, p.dx, p.dy, p.claimed ? " (claimed)" : "", p.stampOnly ? " (end)" : "");
                }
            }
        }

        // applies the moves collected for each group (on the journal for player groups)
        auto MoveStep::applyPendingMoves() -> void {
            for (auto& p : pending) {
                bool const playerGroup = canon && tl_jr && isPG(g, p.group);
                if (playerGroup) {
                    JOp op{};
                    op.kind = JOp::PEND;
                    op.group = p.group;
                    op.stampOnly = p.stampOnly;
                    op.claimed = p.claimed;
                    op.t0 = static_cast<int>(tl_jr->terms.size());
                    tl_jr->terms.insert(tl_jr->terms.end(), p.terms.begin(), p.terms.end());
                    op.t1 = static_cast<int>(tl_jr->terms.size());
                    jrPush(op);
                    tl_inPend = true;
                }
                if (!p.claimed) {
                    float const fx = static_cast<float>(p.dx);
                    float const fy = static_cast<float>(p.dy);
                    if (fx != 0.f || fy != 0.f)
                        moveGroup(g, p.group, static_cast<double>(fx), static_cast<double>(fy), false);
                    else if (p.stampOnly)
                        moveGroup(g, p.group, 0.0, 0.0, true);
                }
                tl_inPend = false;
            }
        }

        // collects the follow player Y commands, stepped after the other moves
        auto MoveStep::collectFollowY() -> void {
            for (auto const& c : T->r().M().cmds) {
                if (c.type != CmdType::FollowY || c.disabled) continue;
                FollowYCmd const cmd{c.target, c.fySpeed, c.fyMax, c.fyDelay, c.fyOffset};
                bool found = false;
                for (auto& f : followY) {
                    if (f.target == c.target) {
                        f = cmd;
                        found = true;
                        break;
                    }
                }
                if (!found) followY.push_back(cmd);
            }
        }

        // on the canonical path, records the follow and follow Y steps in the journal
        auto MoveStep::journalFollows() -> void {
            if (canon) {
                if (!followY.empty() && tl_jr) {
                    JOp op{};
                    op.kind = JOp::FOLLOWY;
                    tl_jr->fy = followY;
                    jrPush(op);
                }
                followY.clear();
                if (hasAdv) {
                    tl_advSel = 1;
                    // advanced follows take dt in game units (1/240 s), as processAdvancedFollowActions does
                    advancedFollowStep(g, static_cast<float>(dtSec * 240.0));
                    tl_advSel = 0;
                    bool anyP = false;
                    for (auto const& I : T->r().M().advs) {
                        if (advIsP(g, I)) {
                            anyP = true;
                            break;
                        }
                    }
                    if (anyP && tl_jr) {
                        PSet const* const psA = psetOf(g);
                        for (auto const& I : T->r().M().advs) {
                            if (!advIsP(g, I) || I.key <= 0) continue;
                            int const mainObj = w->mainObjectOf(I.key);
                            if (mainObj < 0 || bitOf(psA->objBits, mainObj) ||
                                w->areaTarget[static_cast<size_t>(mainObj)])
                                continue;
                            bool dup = false;
                            for (auto const& k : tl_jr->advKeys)
                                if (k.key == I.key) dup = true;
                            if (dup) continue;
                            AdvKey saved{};
                            saved.key = I.key;
                            saved.ok = afTargetPos(g, I.key, saved.x, saved.y);
                            tl_jr->advKeys.push_back(saved);
                        }
                        JOp op{};
                        op.kind = JOp::ADVP;
                        jrPush(op);
                    }
                }
            } else if (!split) {
                for (auto const& c : followY) playerFollowStep(g, c, dt);
                followY.clear();
                if (hasAdv) advancedFollowStep(g, static_cast<float>(dtSec * 240.0));
            } else if (hasAdv && !hasAdvP)
                advancedFollowStep(g, static_cast<float>(dtSec * 240.0));
        }

        // applies the follow commands (one group follows another)
        auto MoveStep::applyFollows() -> void {
            {
                struct FNode {
                    int key, target, center;
                    double mx, my;
                    bool finish;
                };
                static thread_local std::vector<FNode> fnodes;
                fnodes.clear();
                for (auto const& c : T->r().M().cmds) {
                    if (c.type != CmdType::Follow || c.disabled) continue;
                    int const key = c.target * 1000 + c.center;
                    FNode* node = nullptr;
                    for (auto& f : fnodes) {
                        if (f.key == key) {
                            node = &f;
                            break;
                        }
                    }
                    if (!node) {
                        fnodes.push_back(FNode{key, c.target, c.center, 0.0, 0.0, false});
                        node = &fnodes.back();
                    }
                    node->mx = c.followXMod;
                    node->my = c.followYMod;
                    node->center = c.center;
                    if (c.finishRelated) node->finish = true;
                }
                for (auto const& node : fnodes) {
                    int const centerIdx = w->mainObjectOf(node.center);
                    if (centerIdx < 0) continue;
                    bool const pFollow = newPath && isPO(g, centerIdx);
                    if (pFollow && canon) {
                        if (tl_jr) {
                            JOp op{};
                            op.kind = JOp::PFOLLOW;
                            op.group = node.target;
                            op.center = node.center;
                            op.dx = node.mx;
                            op.dy = node.my;
                            op.finishRelated = node.finish;
                            jrPush(op);
                        }
                        continue;
                    }
                    ObjDyn& centerDyn = dynEntry(g, centerIdx);
                    stampLast(g, centerDyn);
                    double const centerX = centerDyn.x;
                    double const centerY = centerDyn.y;
                    float const lastX = centerDyn.lastX;
                    float const lastY = centerDyn.lastY;
                    double const dx = (centerX - static_cast<double>(lastX)) * node.mx;
                    double const dy = (centerY - static_cast<double>(lastY)) * node.my;
                    if (dx == 0.0 && dy == 0.0 && !node.finish) continue;
                    moveGroup(g, node.target, dx, dy, false);
                }
            }
        }

        // clears the deltas of the commands for the next step
        auto MoveStep::clearDeltas() -> void {
            {
                TrigHeavy& H4 = T->w();
                for (auto& c : H4.Mw().cmds) {
                    if (c.disabled) continue;
                    c.deltaX = 0;
                    c.deltaY = 0;
                    c.rotDelta = 0;
                    if (!c.finishRelated) {
                        if (c.finished) c.finishRelated = true;
                    } else
                        c.doUpdate = true;
                }
                {
                    auto& cmds = H4.Mw().cmds;
                    cmds.erase(std::remove_if(cmds.begin(), cmds.end(), [](GroupCmd const& c) { return c.doUpdate; }), cmds.end());
                }
                if (pruneTick && H4.M().cellOv.size() > 32) {
                    auto& cells = H4.Mw().cellOv.items;
                    cells.erase(std::remove_if(cells.begin(), cells.end(), [&](auto const& e) { return e.first / 4096 < cellBehind; }), cells.end());
                }
            }
        }

        // stores the result of the step in the memo, unless a keyframe had side effects
        auto MoveStep::commitMemo() -> void {
            kfSideEffect = tl_sideEffect;
            if (useMemo && !kfSideEffect) {
                int const dep = tl_dynDep;
                memoCommit(key, *T, keysBefore, fullBefore, &followY);
                if (mkey) {
                    tl_dynDep = dep;
                    memoCommit(mkey, *T, keysBefore, fullBefore, &followY);
                }
            }
        }

        // player follow steps of a split step, then drops the entries that came back to their start
        auto MoveStep::finishMoves() -> void {
            tl_fyExcl = nullptr;
            tl_dBefore.reset();
            if (split) {
                for (auto const& c : followY) playerFollowStep(g, c, dt);
                if (hasAdvP) advancedFollowStep(g, static_cast<float>(dtSec * 240.0));
            }
            pruneDyn(g);
        }

        auto MoveStep::run() -> void {
            if (measurePlayerMove() == Flow::Return) return;
            findLocks();
            lookUpMemo();
            computeMoves();
            finishMoves();
        }

        void trigMoveActionsImpl(Game& g, double dtSec, float dtPhys) {
            MoveStep moves(g, dtSec, dtPhys);
            moves.run();
        }

        // replays a ROT op on the player objects of the group (like applyRotation), player center read on this state
        void rotateP(Game& g, JOp const& op, Journal const& jr) {
            World const& w = *g.world;
            PSet const* pset = psetOf(g);
            if (!pset) return;
            int const target = op.group;
            if (op.center < 0) {
                if (op.rotObj == 0.f) return;
                for (int idx : physOfGroup(w, target)) {
                    if (!bitOf(pset->objBits, idx)) continue;
                    Obj const& o = objAt(g, idx);
                    if (w.decoLikeAt(idx) && !w.centerLike[static_cast<size_t>(idx)]) continue;
                    if (!o.canRotateFree()) continue;
                    ObjDyn& d = dynEntryP(g, idx);
                    d.rotAccY = op.rotObj + d.rotAccY;
                    d.rotAcc = op.rotObj + d.rotAcc;
                    d.rotated = true;
                }
                return;
            }
            float cx = op.cx;
            float cy = op.cy;
            if (op.cP) {
                Obj const& center = objAt(g, op.center);
                double ccx, ccy;
                realPosBase(g, op.center, ccx, ccy);
                cx = static_cast<float>(ccx - static_cast<double>(center.posOffX));
                cy = static_cast<float>(ccy - static_cast<double>(center.posOffY));
            }
            float const rad = -op.rot * 0.01745329252f;
            float const cs = gcos(rad);
            float const sn = gsin(rad);
            float const ta = cs;
            float const tb = sn;
            float const tcf = -sn;
            float const td = cs;
            float pdx = op.pdx;
            float pdy = op.pdy;
            if (op.claimed) {
                double sumX = 0.0;
                double sumY = 0.0;
                for (int k = op.t0; k < op.t1; ++k) {
                    JTerm const& t = jr.terms[static_cast<size_t>(k)];
                    double dy = t.dy;
                    if (t.lock & 2)
                        dy = static_cast<double>(g.trig.playerDy) * t.modY;
                    else if (t.lock & 8)
                        dy = static_cast<double>(static_cast<float>(static_cast<double>(-g.cam.sdy) * t.modY));
                    sumX += t.dx;
                    sumY += dy;
                }
                pdx = static_cast<float>(sumX);
                pdy = static_cast<float>(sumY);
            }
            for (int idx : physOfGroup(w, target)) {
                if (!bitOf(pset->objBits, idx)) continue;
                Obj const& o = objAt(g, idx);
                if (w.decoLikeAt(idx) && !w.centerLike[static_cast<size_t>(idx)]) continue;
                ObjDyn& d = dynEntryP(g, idx);
                stampLast(g, d);
                float const rx = static_cast<float>((d.x - static_cast<double>(o.posOffX)) - static_cast<double>(cx));
                float const ry = static_cast<float>((d.y - static_cast<double>(o.posOffY)) - static_cast<double>(cy));
                float const px = static_cast<float>(static_cast<double>(ta) * rx + static_cast<double>(tcf) * ry + static_cast<double>(cx));
                float const py = static_cast<float>(static_cast<double>(tb) * rx + static_cast<double>(td) * ry + static_cast<double>(cy));
                d.x = (static_cast<double>(pdx + px) - d.x) + static_cast<double>(o.posOffX) + d.x;
                d.y = (static_cast<double>(pdy + py) - d.y) + static_cast<double>(o.posOffY) + d.y;
                if (op.rotObj != 0.f && o.canRotateFree()) {
                    d.rotAccY = op.rotObj + d.rotAccY;
                    d.rotAcc = op.rotObj + d.rotAcc;
                    d.rotated = true;
                }
                d.secX = d.x;
                d.secY = d.y;
            }
            if (bitOf(pset->groupBits, target) && (pdx != 0.f || pdy != 0.f)) {
                TrigPv& P = g.trig.wp();
                GroupOff* off = P.off.find(static_cast<short>(target));
                if (!off) {
                    GroupOff init;
                    if (auto const* c = g.trig.r().M().groupOff.find(static_cast<short>(target))) init = *c;
                    P.off[static_cast<short>(target)] = init;
                    off = P.off.find(static_cast<short>(target));
                }
                if (off->stamp != tickStamp(g)) {
                    off->px = off->x;
                    off->py = off->y;
                    off->stamp = tickStamp(g);
                }
                off->x += static_cast<double>(pdx);
                off->y += static_cast<double>(pdy);
            }
        }

        // replays a SCALE op on the player objects of the group (like applyTransform)
        void scaleP(Game& g, JOp const& op, Journal const& jr) {
            World const& w = *g.world;
            PSet const* pset = psetOf(g);
            if (!pset) return;
            if (op.center < 0) {
                if (op.onlyMove) return;
                double const rx = op.tfN[0] / op.tfO[0];
                double const ry = op.tfN[1] / op.tfO[1];
                for (int idx : w.allOfGroup(op.group)) {
                    if (!bitOf(pset->objBits, idx)) continue;
                    if (w.decoLikeAt(idx)) continue;
                    ObjDyn& d = dynEntryP(g, idx);
                    sclPrev(g, d);
                    tfScaleNoCenter(d, rx, ry);
                }
                return;
            }
            float cx = op.cx;
            float cy = op.cy;
            if (op.cP) {
                Obj const& center = objAt(g, op.center);
                double ccx, ccy;
                realPosBase(g, op.center, ccx, ccy);
                cx = static_cast<float>(ccx - static_cast<double>(center.posOffX));
                cy = static_cast<float>(ccy - static_cast<double>(center.posOffY));
            }
            float centerRot = op.scr;
            if (op.scrP) centerRot = objAt(g, op.center).rotX + trigRotation(g, op.center) + 0.f;
            float pdx = op.pdx;
            float pdy = op.pdy;
            if (op.claimed) {
                double sumX = 0.0;
                double sumY = 0.0;
                for (int k = op.t0; k < op.t1; ++k) {
                    JTerm const& t = jr.terms[static_cast<size_t>(k)];
                    double dy = t.dy;
                    if (t.lock & 2)
                        dy = static_cast<double>(g.trig.playerDy) * t.modY;
                    else if (t.lock & 8)
                        dy = static_cast<double>(static_cast<float>(static_cast<double>(-g.cam.sdy) * t.modY));
                    sumX += t.dx;
                    sumY += dy;
                }
                pdx = static_cast<float>(sumX);
                pdy = static_cast<float>(sumY);
            }
            Aff const P = tfWorld(cx, cy, op.tfN[0], op.tfO[0], op.tfN[1], op.tfO[1], op.rot + centerRot, centerRot);
            for (int idx : physOfGroup(w, op.group)) {
                if (!bitOf(pset->objBits, idx)) continue;
                Obj const& o = objAt(g, idx);
                if (w.decoLikeAt(idx) && !w.centerLike[static_cast<size_t>(idx)]) continue;
                ObjDyn& d = dynEntryP(g, idx);
                if (o.type != Decoration) sclPrev(g, d);
                stampLast(g, d);
                tfObj(o, d, P, cx, cy, pdx, pdy, op.onlyMove, op.rotObj);
                d.secX = d.x;
                d.secY = d.y;
            }
            if (bitOf(pset->groupBits, op.group) && (pdx != 0.f || pdy != 0.f)) {
                TrigPv& Pv = g.trig.wp();
                GroupOff* off = Pv.off.find(static_cast<short>(op.group));
                if (!off) {
                    GroupOff init;
                    if (auto const* c = g.trig.r().M().groupOff.find(static_cast<short>(op.group))) init = *c;
                    Pv.off[static_cast<short>(op.group)] = init;
                    off = Pv.off.find(static_cast<short>(op.group));
                }
                if (off->stamp != tickStamp(g)) {
                    off->px = off->x;
                    off->py = off->y;
                    off->stamp = tickStamp(g);
                }
                off->x += static_cast<double>(pdx);
                off->y += static_cast<double>(pdy);
            }
        }

        // plays one journal op on the real values of this state
        void execOp(Game& g, JOp const& o, Journal const& jr, double dtSec) {
            TrigState& T = g.trig;
            float const dt = static_cast<float>(dtSec);
            bool const canonBefore = tl_canon;
            bool const canonReadBefore = tl_canonRead;
            Journal* const jrBefore = tl_jr;
            tl_canon = false;
            tl_canonRead = false;
            tl_jr = nullptr;
            {
                switch (o.kind) {
                case JOp::PEND: {
                    double pdx = 0.0;
                    double pdy = 0.0;
                    for (int k = o.t0; k < o.t1; ++k) {
                        JTerm const& t = jr.terms[static_cast<size_t>(k)];
                        if (t.lock & 16) {
                            if (t.dm >= 0 && static_cast<size_t>(t.dm) < tl_dmVal.size()) {
                                pdx += tl_dmVal[static_cast<size_t>(t.dm)].first;
                                pdy += tl_dmVal[static_cast<size_t>(t.dm)].second;
                            }
                            continue;
                        }
                        double dy = t.dy;
                        if (t.lock & 2)
                            dy = static_cast<double>(T.playerDy) * t.modY;
                        else if (t.lock & 8)
                            dy = static_cast<double>(static_cast<float>(static_cast<double>(-g.cam.sdy) * t.modY));
                        pdx += t.dx;
                        pdy += dy;
                    }
                    if (o.claimed) break;
                    {
                        float const fx = static_cast<float>(pdx);
                        float const fy = static_cast<float>(pdy);
                        if (fx != 0.f || fy != 0.f)
                            moveGroupP(g, o.group, static_cast<double>(fx), static_cast<double>(fy));
                        else if (o.stampOnly)
                            moveGroupP(g, o.group, 0.0, 0.0);
                    }
                    break;
                }
                case JOp::GMOVE:
                    moveFyMembers(g, o.group, o.dx, o.dy, g.world->ySection);
                    break;
                case JOp::PADD:
                    moveGroupP(g, o.group, o.dx, o.dy);
                    break;
                case JOp::FOLLOWY:
                    for (auto const& c : jr.fy) playerFollowStep(g, c, dt);
                    break;
                case JOp::ADVP:
                    tl_advSel = 2;
                    tl_advKeys = &jr.advKeys;
                    advancedFollowStep(g, static_cast<float>(dtSec * 240.0));
                    tl_advKeys = nullptr;
                    tl_advSel = 0;
                    break;
                case JOp::ROT:
                    rotateP(g, o, jr);
                    break;
                case JOp::SCALE:
                    scaleP(g, o, jr);
                    break;
                case JOp::DMOVE: {
                    DMove const& d = jr.dm[static_cast<size_t>(o.t0)];
                    if (tl_dmVal.size() < jr.dm.size()) tl_dmVal.resize(jr.dm.size());
                    dynMoveDelta(g, d.c, d.a, d.f, d.dir, d.k, tl_dmVal[static_cast<size_t>(o.t0)].first, tl_dmVal[static_cast<size_t>(o.t0)].second);
                    break;
                }
                case JOp::PFOLLOW: {
                    int const centerIdx = g.world->mainObjectOf(o.center);
                    if (centerIdx < 0) break;
                    ObjDyn& centerDyn = dynEntry(g, centerIdx);
                    stampLast(g, centerDyn);
                    double const centerX = centerDyn.x;
                    double const centerY = centerDyn.y;
                    float const lastX = centerDyn.lastX;
                    float const lastY = centerDyn.lastY;
                    double const dx = (centerX - static_cast<double>(lastX)) * o.dx;
                    double const dy = (centerY - static_cast<double>(lastY)) * o.dy;
                    if (dx == 0.0 && dy == 0.0 && !o.finishRelated) break;
                    if (isPG(g, o.group))
                        moveGroupP(g, o.group, dx, dy);
                    else
                        moveGroup(g, o.group, dx, dy, false);
                    break;
                }
                }
            }
            tl_canon = canonBefore;
            tl_canonRead = canonReadBefore;
            tl_jr = jrBefore;
        }

        // replays the whole journal on this state
        void phaseB(Game& g, Journal const& jr, double dtSec) {
            bool const hasUndo = !jr.undo.empty();
            if (hasUndo) {
                tl_undoJr = &jr;
                tl_undoOn = true;
            }
            for (size_t k = 0; k < jr.ops.size(); ++k) {
                tl_jrPos = static_cast<int>(k);
                execOp(g, jr.ops[k], jr, dtSec);
            }
            if (hasUndo) {
                tl_undoJr = nullptr;
                tl_undoOn = false;
            }
        }

        // plays an op right away (immediate mode, called by jrPush)
        void jrExec(JOp const& o) { execOp(*tl_jrGame, o, *tl_jr, tl_jrDt); }

    }

    // move step of the triggers (processMoveActionsStep), memoized for all states of a tick
    void trigMoveActions(Game& g, double dtSec, float dtPhys) {
        TrigState& T = g.trig;
        setPlayerDelta(g, dtPhys);
        if (T.r().M().cmds.empty() && T.r().M().advs.empty() && T.r().M().drots.empty()) return;
        updatePSet(g);
        PSet const* pset = psetOf(g);
        if (!pset) {
            trigMoveActionsImpl(g, dtSec, dtPhys);
            return;
        }
        memoTick(g);
        bool lockPX = false;
        bool lockCX = false;
        for (auto const& c : T.r().M().cmds) {
            if (c.disabled || c.type != CmdType::Move || c.finishRelated) continue;
            if (c.lockPX) lockPX = true;
            if (c.lockCX) lockCX = true;
        }
        bool const pruneTick = !g.p1.isGoingLeft && (g.tick & 63) == 0;
        int const cellBehind = World::cellOf(static_cast<double>(g.p1.x)) - 12;
        bool const memoEnabled = memoOn();
        uint64_t const psig = pset->sig;
        bool tryShared = memoEnabled;
        if (tryShared) {
            auto it = tl_pHitUntil.find(psig);
            if (it != tl_pHitUntil.end() && g.tick >= it->second.at && g.tick < it->second.until) {
                tryShared = false;
                ++tl_pStat[3];
            }
        }
        if (tryShared) {
            uint64_t key = mix64(mix64(mix64(trigHeavyHash(T.r()), bits64(dtSec)), bits64(dtPhys)), 0x3015ull);
            if (!T.afRing.empty()) {
                World const& w = *g.world;
                uint64_t ringHash = 0x51a9e7ull;
                for (size_t r = 0; r < w.afRings.size() && r < T.afRingH.size(); ++r) {
                    auto const& ring = w.afRings[r];
                    if (ring.key <= 0) continue;
                    if (ring.obj >= 0 &&
                        (bitOf(pset->objBits, ring.obj) || w.areaTarget[static_cast<size_t>(ring.obj)]))
                        continue;
                    ringHash = mix64(ringHash ^ static_cast<uint64_t>(r), T.afRingH[r]);
                }
                key = mix64(mix64(key, ringHash),
                            (static_cast<uint64_t>(T.afU6 - T.afU5) << 32) | static_cast<uint32_t>(T.afN));
            }
            if (pruneTick) key = mix64(key, static_cast<uint64_t>(static_cast<uint32_t>(cellBehind)));
            if (lockPX) key = mix64(key, bits64(T.playerDx));
            if (lockCX) key = mix64(key, bits64(g.cam.sdx));
            if (MemoEntry const* m = memoLookup(T, key)) {
                ++tl_pStat[0];
                g_pStatAll[0].fetch_add(1, std::memory_order_relaxed);
                g.itemChanges += m->itemDelta;
                if (m->jr) phaseB(g, *m->jr, dtSec);
                pruneDyn(g);
                return;
            }
            uint64_t mkey = 0;
            {
                bool withOff = false;
                for (auto const& I : T.r().M().advs) {
                    if (g.world->advDefs[static_cast<size_t>(I.def)].ignoreDisabled) {
                        withOff = true;
                        break;
                    }
                }
                mkey = mix64(mix64(mix64(motionHash(T.r(), withOff), bits64(dtSec)), bits64(dtPhys)), 0x3017ull);
                if (!T.afRing.empty()) {
                    World const& w = *g.world;
                    uint64_t ringHash = 0x51a9e7ull;
                    for (size_t r = 0; r < w.afRings.size() && r < T.afRingH.size(); ++r) {
                        auto const& ring = w.afRings[r];
                        if (ring.key <= 0) continue;
                        if (ring.obj >= 0 &&
                            (bitOf(pset->objBits, ring.obj) || w.areaTarget[static_cast<size_t>(ring.obj)]))
                            continue;
                        ringHash = mix64(ringHash ^ static_cast<uint64_t>(r), T.afRingH[r]);
                    }
                    mkey = mix64(mix64(mkey, ringHash),
                                (static_cast<uint64_t>(T.afU6 - T.afU5) << 32) | static_cast<uint32_t>(T.afN));
                }
                if (pruneTick) mkey = mix64(mkey, static_cast<uint64_t>(static_cast<uint32_t>(cellBehind)));
                if (lockPX) mkey = mix64(mkey, bits64(T.playerDx));
                if (lockCX) mkey = mix64(mkey, bits64(g.cam.sdx));
                if (MemoEntry const* m = motionLookup(T, mkey)) {
                    ++tl_pStat[0];
                    g_pStatAll[0].fetch_add(1, std::memory_order_relaxed);
                    for (uint64_t gr : m->spawned) T.spawnedThisTick.insert(gr);
                    g.itemChanges += m->itemDelta;
                    if (m->jr) phaseB(g, *m->jr, dtSec);
                    pruneDyn(g);
                    return;
                }
            }
            Game const snap = g;
            auto jr = std::make_shared<Journal>();
            uint64_t const keysBefore = dynKeysHash(T.rd());
            std::shared_ptr<TrigDyn> const fullBefore = T.d;
            tl_dynDep = 0;
            tl_itemDelta = 0;
            tl_sideEffect = false;
            spawnMark(T);
            tl_pHit = false;
            tl_newPath = true;
            tl_canon = true;
            tl_canonRead = true;
            tl_memoComp = true;
            tl_jr = jr.get();
            trigMoveActionsImpl(g, dtSec, dtPhys);
            tl_newPath = false;
            tl_canon = false;
            tl_canonRead = false;
            tl_memoComp = false;
            tl_jr = nullptr;
            if (!tl_pHit) {
                ++tl_pStat[1];
                g_pStatAll[1].fetch_add(1, std::memory_order_relaxed);
                if (!tl_pHitUntil.empty()) tl_pHitUntil.erase(psig);
                std::stable_sort(jr->undo.begin(), jr->undo.end(),
                                [](UndoE const& a, UndoE const& b) { return a.group < b.group; });
                if (!tl_sideEffect) {
                    tl_commitJr = jr;
                    int const dep = tl_dynDep;
                    memoCommit(key, T, keysBefore, fullBefore, nullptr);
                    tl_dynDep = dep;
                    memoCommit(mkey, T, keysBefore, fullBefore, nullptr);
                    tl_commitJr.reset();
                } else
                    ++tl_pStat[4];
                phaseB(g, *jr, dtSec);
                return;
            }
            ++tl_pStat[2];
            g = snap;
            {
                if (tl_pHitUntil.size() > 1024) tl_pHitUntil.clear();
                PHit& hit = tl_pHitUntil[psig];
                bool const streak = hit.wait > 0 && g.tick >= hit.until && g.tick <= hit.until + 1;
                hit.wait = streak ? std::min(240, hit.wait * 2) : 1;
                hit.at = g.tick;
                hit.until = g.tick + hit.wait;
            }
        } else if (!memoEnabled)
            ++tl_pStat[5];
        tl_pHit = false;
        Journal imm;
        tl_newPath = true;
        tl_canon = true;
        tl_canonRead = false;
        tl_memoComp = false;
        tl_jr = &imm;
        tl_jrImm = true;
        tl_jrGame = &g;
        tl_jrDt = dtSec;
        trigMoveActionsImpl(g, dtSec, dtPhys);
        tl_newPath = false;
        tl_canon = false;
        tl_jr = nullptr;
        tl_jrImm = false;
        tl_jrGame = nullptr;
    }

    namespace trigger_impl {
        // eased value of v (0..1), interpolated in the buffer like getEasedAreaValue
        float areaEase(float v, int type, float rate) {
            if (type == 0) return v;
            EaseBuf const* buf = nullptr;
            for (auto const& e : tl_easeBufs) {
                if (e.type == type && e.rate == rate) {
                    buf = &e;
                    break;
                }
            }
            if (!buf) {
                EaseBuf newBuf;
                newBuf.type = type;
                newBuf.rate = rate;
                for (int i = 0; i <= 100; ++i) newBuf.t[i] = easedValue(static_cast<float>(i) / 100.f, type, rate);
                newBuf.t[101] = newBuf.t[100];
                tl_easeBufs.push_back(newBuf);
                buf = &tl_easeBufs.back();
            }
            int k = static_cast<int>(v * 100.f);
            if (k < 0) k = 0;
            if (k > 100) k = 100;
            return (v - static_cast<float>(k) * 0.01f) * (buf->t[k + 1] - buf->t[k]) * 100.f + buf->t[k];
        }

        // 0..1 value of a distance for an area of length len with dead zone deadZone (0x140227b60)
        float areaValueFromDist(float dist, int len, float deadZone) {
            float const v = dist / static_cast<float>(len);
            if (!(v == v)) return 1.f;
            float f = (deadZone != 0.f) ? (v - deadZone) / (1.f - deadZone) : v;
            if (f < 0.f) return 0.f;
            if (1.f < f) return 1.f;
            return f;
        }

        // adds to areaMoved the advanced follow objects that left their grid cell
        void addAfMoved(Game& g) {
            TrigState& T = g.trig;
            if (!T.d || T.rd().afObjs.empty()) return;
            World const& w = *g.world;
            bool added = false;
            size_t const n0 = T.areaMoved.size();
            for (int idx : T.rd().afObjs) {
                Obj const& o = objAt(g, idx);
                if (o.type == Decoration || (o.extendedCollision && o.id != 0x718)) continue;
                int gcx, gcy, ccx, ccy;
                trigAreaCells(g, idx, gcx, gcy, ccx, ccy);
                if (gcx == ccx && gcy == ccy) continue;
                T.areaMoved.push_back(idx);
                added = true;
            }
            (void)w;
            if (added) sortAppended(T.areaMoved, n0);
        }

    }

    // same as PosCacheScope but as two calls, start
    void trigPosCacheScopeBegin(bool& prev) {
        prev = tl_posCacheOn;
        tl_posCacheOn = !(g_noShare & 2);
    }

    // same as PosCacheScope but as two calls, end
    void trigPosCacheScopeEnd(bool prev) { tl_posCacheOn = prev; }

}
