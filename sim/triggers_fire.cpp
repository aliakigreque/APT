// Firing of a trigger when the player crosses or touches it (GJBaseGameLayer::triggerObject), and the game
// events of the event trigger.

#include "triggers_internal.hpp"

namespace ap::sim {
    namespace trigger_impl {
        // event trigger (3604): registers (or removes, on reset) listeners for its events
        void activateEventTrigger(Game& g, int idx, Obj const& o, RemapKeys const* keys) {
            ExtProps const* ex = g.world->extOf(o);
            if (!ex) return;
            std::vector<double> const* events = ex->list(430);
            if (!events || events->empty()) return;
            int const extra = remapForActivation(g, idx, static_cast<int>(ex->num(447, 0))) * 10000 + remapForActivation(g, idx, static_cast<int>(ex->num(525, 0)));
            bool const reset = ex->num(431, 0) != 0;
            std::vector<int> ids;
            for (double d : *events) ids.push_back(static_cast<int>(d));
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            auto& H = g.trig.w();
            for (int event : ids) {
                long long const key =
                        (static_cast<long long>(event) << 32) | static_cast<long long>(static_cast<uint32_t>(extra));
                if (reset) {
                    if (auto* listeners = H.events.find(key)) {
                        listeners->erase(
                                std::remove_if(listeners->begin(), listeners->end(), [&](EventListener const& l) { return l.target == o.targetGroup; }),
                                listeners->end());
                    }
                } else {
                    EventListener listener;
                    listener.target = o.targetGroup;
                    listener.uid = o.uid;
                    listener.control = o.controlID;
                    if (keys) listener.keys = *keys;
                    H.events[key].push_back(listener);
                }
            }
        }

    }

    // object type to game event number, 0 if none, same table as GJBaseGameLayer::objectTypeToGameEvent
    int objectTypeToGameEvent(int type) {
        switch (type) {
        case InverseGravityPortal:
            return GameEvent::PortalGravityFlip;
        case NormalGravityPortal:
            return GameEvent::PortalGravityNormal;
        case ShipPortal:
            return GameEvent::PortalShip;
        case CubePortal:
            return GameEvent::PortalNormal;
        case YellowJumpPad:
            return GameEvent::YellowPad;
        case PinkJumpPad:
            return GameEvent::PinkPad;
        case GravityPad:
            return GameEvent::GravityPad;
        case YellowJumpRing:
            return GameEvent::YellowOrb;
        case PinkJumpRing:
            return GameEvent::PinkOrb;
        case GravityRing:
            return GameEvent::GravityOrb;
        case InverseMirrorPortal:
            return GameEvent::PortalFlip;
        case NormalMirrorPortal:
            return GameEvent::PortalUnFlip;
        case BallPortal:
            return GameEvent::PortalBall;
        case RegularSizePortal:
            return GameEvent::PortalNormalScale;
        case MiniSizePortal:
            return GameEvent::PortalMiniScale;
        case UfoPortal:
            return GameEvent::PortalUFO;
        case Breakable:
            return GameEvent::DestroyBlock;
        case SecretCoin:
            return GameEvent::UserCoin;
        case DualPortal:
            return GameEvent::PortalDualOn;
        case SoloPortal:
            return GameEvent::PortalDualOff;
        case WavePortal:
            return GameEvent::PortalWave;
        case RobotPortal:
            return GameEvent::PortalRobot;
        case TeleportPortal:
            return GameEvent::PortalTeleport;
        case GreenRing:
            return GameEvent::GreenOrb;
        case Collectible:
            return GameEvent::PickupItem;
        case UserCoin:
            return GameEvent::UserCoin;
        case DropRing:
            return GameEvent::DropOrb;
        case SpiderPortal:
            return GameEvent::PortalSpider;
        case RedJumpPad:
            return GameEvent::RedPad;
        case RedJumpRing:
            return GameEvent::RedOrb;
        case CustomRing:
            return GameEvent::CustomOrb;
        case DashRing:
            return GameEvent::DashOrb;
        case GravityDashRing:
            return GameEvent::GravityDashOrb;
        case SwingPortal:
            return GameEvent::PortalSwing;
        case GravityTogglePortal:
            return GameEvent::PortalGravityInvert;
        case SpiderOrb:
            return GameEvent::SpiderOrb;
        case SpiderPad:
            return GameEvent::SpiderPad;
        case TeleportOrb:
            return GameEvent::TeleportOrb;
        default:
            return 0;
        }
    }

    thread_local Game* g_evGame = nullptr;

    // a game event happened: spawns the groups of the event triggers listening to it, once per command
    void trigGameEvent(Game& g, int event, int extra) {
        if (!g.trig.on || !g.world->listensEvent(event)) return;
        TrigState& T = g.trig;
        int const key = extra * 10000;
        long long const fullKey =
                (static_cast<long long>(event) << 32) | static_cast<long long>(static_cast<uint32_t>(key));
        for (auto& e : T.evStamps) {
            if (e.first == fullKey) {
                if (e.second == g.commandIndex) return;
                e.second = g.commandIndex;
                goto stamped;
            }
        }
        T.evStamps.emplace_back(fullKey, g.commandIndex);
    stamped:
        std::vector<EventListener> const* found = T.r().events.find(fullKey);
        if (!found || found->empty()) return;
        std::vector<EventListener> const listeners = *found;
        if (g_trace) {
            std::printf("      [event] tick %d : %d (key %d) -> %d listener(s) :", g.tick, event, key,
                        static_cast<int>(listeners.size()));
            for (auto const& l : listeners) std::printf(" uid %d->g%d", l.uid, l.target);
            std::printf("\n");
        }
        for (auto const& l : listeners) {
            if (!l.paused) spawnGroupLayer(g, l.target, false, 0.0, l.uid, l.control, &l.keys);
        }
    }

    // activates trigger idx (EffectGameObject::triggerObject), playerUid is the player that touched it, 0 if none
    void triggerObject(Game& g, int idx, int playerUid) {
        (void)playerUid;
        Obj const& o = objAt(g, idx);
        switch (o.id) {
        case 1913:
        case 1916:
        case 1935:
        case 3022:
        case 2066:
        case 2899:
        case 1932:
        case 1917:
        case 2900:
        case 2925:
        case 3600:
        case 200:
        case 201:
        case 202:
        case 203:
        case 1334:
        case 1914:
        case 2062:
        case 2901:
            tl_sideEffect = true;
            break;
        case 3640:
        case 3618:
            tl_sideEffect = true;
            break;
        case 1814:
            if (o.fyDelay > 0.f) tl_sideEffect = true;
            break;
        default:
            break;
        }
        if (g_trace)
            std::printf("[trig] tick %d : trigger uid %d id %d (x %.1f) -> group %d\n", g.tick, o.uid, o.id, o.startX, o.targetGroup);
        {
            if (debugOptions().trigLogAt(g.tick)) {
                char const* why = unsupportedReason(o);
                std::printf("[triglog] t=%d uid %d id %d x=%.0f y=%.0f tgt %d ctr %d%s%s\n", g.tick, o.uid, o.id, o.x, o.y, o.targetGroup, o.centerGroup, why ? " NOT SIMULATED: " : "", why ? why : "");
            }
        }
        switch (o.id) {
        case 901:
            triggerMoveCommand(g, idx);
            break;
        case 1049:
            toggleGroup(g, o.targetGroup, o.tf(Obj::TF_ActivateGroup));
            break;
        case 1268: {
            double const stored = g.trig.r().spawnStored.find(idx) ? *g.trig.r().spawnStored.find(idx) : 0.0;
            float delay = o.spawnDelay;
            if (o.spawnDelayRange != 0.f) {
                tl_sideEffect = true;
                float const r = g.rand01();
                float const d = ((r + r) - 1.f) * o.spawnDelayRange + delay;
                delay = 0.f <= d ? d : 0.f;
            }
            float const remaining = static_cast<float>(static_cast<double>(delay) - stored);
            bool const ordered = o.tf(Obj::TF_SpawnOrdered);
            RemapKeys const* keys = nullptr;
            RemapKeys newKeys;
            {
                RemapKeys const* parent = (tl_remap.idx == idx) ? tl_remap.parentKeys : nullptr;
                RemapKeys const* old = g.trig.r().remapKeys.find(idx);
                bool const reset = o.tf(Obj::TF_ResetRemap);
                bool const parentEmpty = !parent || parent->n == 0;
                bool set = true;
                if (o.remapIdx >= 1) {
                    newKeys.push_back(o.remapIdx);
                    if (!reset && parent) {
                        for (int k : *parent) newKeys.push_back(k);
                    }
                    if (parentEmpty || reset) newKeys.push_back(-o.uid);
                } else if (!reset) {
                    if (parent) newKeys = *parent;
                } else {
                    set = false;
                    if (old) newKeys = *old;
                }
                if (set) {
                    bool const same = old && old->n == newKeys.n && std::equal(old->begin(), old->end(), newKeys.begin());
                    if (!same && (newKeys.n || old)) {
                        TrigHeavy& H = g.trig.w();
                        if (old) H.remapSum -= remapEntryHash(idx, *old);
                        H.remapKeys[idx] = newKeys;
                        H.remapSum += remapEntryHash(idx, newKeys);
                    }
                }
                if (newKeys.n) keys = &newKeys;
            }
            if (0.f < remaining)
                queueSpawn(g, o.targetGroup, remaining, ordered, o.uid, o.controlID, keys);
            else
                spawnGroupLayer(g, o.targetGroup, ordered, static_cast<double>(-remaining), o.uid, o.controlID, keys);
            if (g.trig.r().spawnStored.find(idx)) g.trig.w().spawnStored.erase(idx);
            break;
        }
        case 3033: {
            World const& w = *g.world;
            if (o.ext < 0 || idx < 0 || static_cast<size_t>(idx) >= w.kfFirst.size()) break;
            int first = w.kfFirst[static_cast<size_t>(idx)];
            int count = w.kfCount[static_cast<size_t>(idx)];
            if (o.kfGroup != w.objs[static_cast<size_t>(idx)].kfGroup) {
                auto it = w.kfAlt.find((static_cast<long long>(idx) << 20) | static_cast<long long>(o.kfGroup));
                if (it != w.kfAlt.end()) {
                    first = it->second.first;
                    count = it->second.second;
                } else
                    count = 0;
            }
            bool const trigTarget = w.objs[static_cast<size_t>(idx)].targetGroup != 0;
            for (int a = 0; a < count; ++a) {
                auto const& baseAnim = w.kfAnims[static_cast<size_t>(first + a)];
                if (baseAnim.e.empty()) continue;
                int const target = trigTarget ? o.targetGroup : baseAnim.target;
                if (!baseAnim.spawns && !w.relevantGroup(target)) continue;
                uint64_t dynKey = 0;
                if (baseAnim.dynKeys) {
                    std::vector<World::KfKeyState> states(baseAnim.keys.size());
                    for (size_t q = 0; q < baseAnim.keys.size(); ++q) {
                        int const keyIdx = baseAnim.keys[q];
                        Obj const& keyObj = objAt(g, keyIdx);
                        Point const p = g.obj(keyIdx).getRealPosition();
                        float absScaleX, absScaleY;
                        trigScaleAbs(g, keyIdx, absScaleX, absScaleY);
                        float const r = trigRotation(g, keyIdx);
                        states[q].px = p.x;
                        states[q].py = p.y;
                        states[q].sx = static_cast<double>(absScaleX * (keyObj.flipX ? -1.f : 1.f));
                        states[q].sy = static_cast<double>(absScaleY * (keyObj.flipY ? -1.f : 1.f));
                        states[q].rotX = static_cast<double>(keyObj.rotX + r);
                        states[q].rotY = static_cast<double>(keyObj.rotY + r);
                    }
                    dynKey = kfDynPut(w, first + a, states);
                }
                auto const& anim = dynKey ? kfDynGet(w, dynKey).A : baseAnim;
                GroupCmd& c = newCmd(g);
                c.type = CmdType::Keyframe;
                c.uid = o.uid;
                c.control = o.controlID;
                c.target = target;
                c.center = target;
                c.kf = first + a;
                c.kfDyn = dynKey;
                c.kfSpawned = 0;
                c.duration = anim.duration;
                {
                    int const centerIdx = o.centerGroup > 0 ? w.mainObjectOf(o.centerGroup) : -1;
                    c.kfCenterP1 = centerIdx + 1;
                }
                {
                    if (debugOptions().trigLogAt(g.tick)) {
                        std::printf("[triglog]   keyframes : target %d, animation %d (%zu keys, duration %.3f%s)\n",
                                    target, first + a, anim.e.size(), anim.duration,
                                    dynKey ? ", moving keys : path rebuilt" : "");
                    }
                }
                if (RemapKeys const* k = activationKeys(idx)) c.kfKeys = *k;
            }
            break;
        }
        case 1346: {
            float rot = static_cast<float>(o.times360) * 360.f + o.rotDeg;
            bool const targetMode = o.tf(Obj::TF_UseMoveTarget) || o.tf(Obj::TF_DirSnap);
            if (targetMode && o.tf(Obj::TF_Dynamic) && o.duration != 0.f) {
                triggerDynamicRotateCommand(g, idx, o);
                break;
            }
            if (targetMode) {
                int const center = tryGetObjectRef(g, o.centerGroup != 0 ? o.centerGroup : o.targetGroup);
                int const target = tryGetObjectRef(g, o.targetGroup);
                int aim = playerRef(g, o);
                if (aim == kRefNone) aim = tryGetObjectRef(g, o.rotTarget);
                if (center != kRefNone && aim != kRefNone && target != kRefNone) {
                    int const maxY = tryGetObjectRef(g, o.rotMaxY);
                    int const maxX = tryGetObjectRef(g, o.rotMaxX);
                    int const minY = tryGetObjectRef(g, o.rotMinY);
                    int const minX = tryGetObjectRef(g, o.rotMinX);
                    rot = rotateTargetAngle(g, o, center, target, aim, minX, minY, maxX, maxY);
                }
                {
                    if (debugOptions().trigLogAt(g.tick)) {
                        std::printf("[triglog]   rotate to a target : group %d, center %d, aim %d -> %.4f degrees\n",
                                    o.targetGroup, center >= 0 ? objAt(g, center).uid : center,
                                    aim >= 0 ? objAt(g, aim).uid : aim, rot);
                    }
                }
            }
            createRotateCommand(g, rot, o.duration, o.targetGroup, o.centerGroup, o.easing, o.easingRate,
                                o.tf(Obj::TF_LockObjRot), o.uid, o.controlID);
            break;
        }
        case 2067: {
            if (o.ext < 0) break;
            World const& w = *g.world;
            double scaleX = static_cast<double>(static_cast<float>(w.prop(o, 150, 1.0)));
            double scaleY = static_cast<double>(static_cast<float>(w.prop(o, 151, 1.0)));
            if (scaleX == 0.0) scaleX = 1.0;
            if (scaleY == 0.0) scaleY = 1.0;
            if (w.prop(o, 153, 0.0) != 0.0) scaleX = 1.0 / scaleX;
            if (w.prop(o, 154, 0.0) != 0.0) scaleY = 1.0 / scaleY;
            int const raw71 = static_cast<int>(w.prop(o, 71, 0.0));
            int const center =
                    (idx >= 0 && raw71 == w.objs[static_cast<size_t>(idx)].centerGroup) ? o.centerGroup : raw71;
            if (w.prop(o, 577, 0.0) != 0.0) {
                int const mainIdx = w.mainObjectOf(center);
                if (mainIdx >= 0) {
                    float absX, absY;
                    trigScaleAbs(g, mainIdx, absX, absY);
                    Obj const& mainObj = objAt(g, mainIdx);
                    float const mulX = absX / mainObj.scaleX;
                    float const mulY = absY / mainObj.scaleY;
                    if (mulX != 0.f) scaleX = (scaleX - 1.0) / static_cast<double>(mulX) + 1.0;
                    if (mulY != 0.f) scaleY = (scaleY - 1.0) / static_cast<double>(mulY) + 1.0;
                }
            }
            createTransformCommand(g, scaleX, scaleY, o.targetGroup, center, w.prop(o, 133, 0.0) != 0.0, w.prop(o, 452, 0.0) != 0.0, o.duration, o.easing, o.easingRate, o.uid, o.controlID);
            break;
        }
        case 1347:
            createFollowCommand(g, o.followXMod, o.followYMod, o.duration, o.targetGroup, o.centerGroup, o.uid,
                                o.controlID);
            break;
        case 1814:
            createPlayerFollowCommand(g, o.fySpeed, o.fyDelay, o.fyOffset, o.fyMaxSpeed, o.duration, o.targetGroup, o.uid, o.controlID);
            break;
        case 1815: {
            auto addAction = [&](int a, int b) {
                CollisionAction c;
                c.a = a < b ? b : a;
                c.b = a < b ? a : b;
                c.group = o.targetGroup;
                c.onExit = o.tf(Obj::TF_TriggerOnExit);
                c.activate = o.tf(Obj::TF_ActivateGroup);
                c.uid = o.uid;
                c.control = o.controlID;
                if (RemapKeys const* k = activationKeys(idx)) c.keys = *k;
                g.trig.w().collisions.push_back(c);
            };
            if (o.tf(Obj::TF_FollowCPP))
                addAction(10000, 10001);
            else {
                addAction(o.tf(Obj::TF_TargetP1) ? 10000 : (o.tf(Obj::TF_TargetP2) ? 10001 : o.itemID), o.itemID2);
                if (o.tf(Obj::TF_TargetP1) && o.tf(Obj::TF_TargetP2)) addAction(10001, o.itemID2);
            }
            break;
        }
        case 1595: {
            TouchAction t;
            t.group = o.targetGroup;
            t.hold = o.tf(Obj::TF_TouchHold);
            t.toggleMode = o.touchToggleMode;
            t.playerMode = o.touchPlayerMode;
            t.dual = o.tf(Obj::TF_DualMode);
            t.uid = o.uid;
            t.control = o.controlID;
            if (RemapKeys const* k = activationKeys(idx)) t.keys = *k;
            {
                if (debugOptions().trigLogAt(g.tick)) {
                    std::printf("[touch] tick %d : trigger uid %d registered (group %d, mode %d%s)\n", g.tick, o.uid, t.group, t.toggleMode, t.hold ? ", held" : "");
                }
            }
            g.trig.w().touches.push_back(t);
            break;
        }
        case 1616:
            if (!o.tf(Obj::TF_TargetControlID))
                controlTriggersInGroup(g, o.targetGroup, o.stopCmd);
            else {
                controlCommands(g, 0, o.targetGroup, o.stopCmd);
                controlAreaWithControl(g, o.targetGroup, o.stopCmd);
            }
            break;
        case 1611: {
            CountAction action;
            action.item = o.itemID;
            action.target = o.count;
            action.group = o.targetGroup;
            action.activate = o.tf(Obj::TF_ActivateGroup);
            action.multi = o.tf(Obj::TF_CountMulti);
            action.uid = o.uid;
            action.control = o.controlID;
            action.prev = countForItem(g, o.itemID);
            if (RemapKeys const* k = activationKeys(idx)) action.keys = *k;
            g.trig.w().counts.push_back(action);
            break;
        }
        case 1811: {
            int const count = countForItem(g, o.itemID);
            bool fire = false;
            if (o.compare == 0)
                fire = count == o.count;
            else if (o.compare == 1)
                fire = o.count < count;
            else if (o.compare == 2)
                fire = count < o.count;
            if (fire) {
                toggleGroupTriggered(g, o.targetGroup, o.tf(Obj::TF_ActivateGroup), o.uid, o.controlID, activationKeys(idx));
            }
            break;
        }
        case 1817: {
            int const item = o.itemID;
            int newCount;
            if (o.compare == 1) {
                newCount = static_cast<int>(std::round(static_cast<float>(countForItem(g, item)) * o.countMult));
            } else if (o.compare == 2) {
                if (o.countMult == 0.f) break;
                newCount = static_cast<int>(std::round(static_cast<float>(countForItem(g, item)) / o.countMult));
            } else if (o.tf(Obj::TF_CountOverride))
                newCount = o.count;
            else
                newCount = countForItem(g, item) + o.count;
            updateCountForItem(g, item, newCount);
            break;
        }
        case 3619:
            if (o.ext >= 0) activateItemEdit(g, o);
            break;
        case 3620:
            if (o.ext >= 0) activateItemCompare(g, o, activationKeys(idx));
            break;
        case 3641:
            if (o.ext >= 0) activatePersistentItem(g, o);
            break;
        case 3614:
        case 3615:
        case 3617:
            if (o.ext >= 0) activateTimerTrigger(g, o, activationKeys(idx));
            break;
        case 3607:
            if (o.ext >= 0) activateSequence(g, idx, o, activationKeys(idx));
            break;
        case 3609:
            activateInstantCollision(g, idx, o);
            break;
        case 3640: {
            spawnGroupLayer(g, o.targetGroup, false, 0.0, o.uid, o.controlID, activationKeys(idx));
            auto& blocks = g.trig.stateBlk;
            auto it = std::find_if(blocks.begin(), blocks.end(), [idx](auto const& e) { return e.first == idx; });
            if (it == blocks.end())
                blocks.emplace_back(idx, 1);
            else
                it->second |= 1;
            break;
        }
        case 3604:
            if (o.ext >= 0) activateEventTrigger(g, idx, o, activationKeys(idx));
            break;
        case 3660: {
            World const& w = *g.world;
            auto const* ex = w.extOf(o);
            if (!ex) break;
            auto propF = [&](int k) { return static_cast<float>(ex->num(k, 0.0)); };
            auto propI = [&](int k) { return static_cast<int>(ex->num(k, 0.0)); };
            float const modX = propF(566);
            float const modXV = propF(567);
            float const modY = propF(568);
            float const modYV = propF(569);
            float const speed0 = propF(300);
            float const speedV = propF(301);
            float const dir0 = propF(563);
            float const dirV = propF(564);
            int const ref1 = propI(560);
            int const ref2 = propI(565);
            bool const xOnly = ex->num(306, 0.0) != 0.0;
            bool const yOnly = ex->num(307, 0.0) != 0.0;
            bool const byControl = ex->num(535, 0.0) != 0.0;
            tl_sideEffect = true;
            int const cmd = static_cast<int>(g.commandIndex);
            auto edit = [&](int obj) {
                bool const playerObj = isPO(g, obj);
                AfPhys const* entry = nullptr;
                if (playerObj) {
                    pRead();
                    entry = g.trig.pv ? g.trig.pv->af.find(obj) : nullptr;
                } else
                    entry = g.trig.rd().afPhys.find(obj);
                bool const sim = entry && entry->last == cmd;
                Obj const& targetObj = w.objs[static_cast<size_t>(obj)];
                bool const tracked = !w.decoLikeAt(obj) || w.centerLike[static_cast<size_t>(obj)];
                if (!sim && (tracked || !afApproxEntry(g, obj))) return;
                float const r1 = g.rand01();
                float const r2 = g.rand01();
                float const r3 = g.rand01();
                float const speed = r3 * speedV + speed0;
                float pushX = 0.f;
                float pushY = 0.f;
                bool const push = speed != 0.f;
                if (push) {
                    float const dir = g.rand01() * dirV + dir0;
                    afStartVel(g, obj, ref1, ref2, dir, speed, pushX, pushY);
                }
                if (!sim) return;
                AfPhys* phys = playerObj ? g.trig.wp().af.find(obj) : g.trig.wd().afPhys.find(obj);
                if (!phys) return;
                phys->vx = (r1 * modXV + modX) * phys->vx;
                phys->vy = (r2 * modYV + modY) * phys->vy;
                if (push) {
                    if (!xOnly) phys->vy = pushY + phys->vy;
                    if (!yOnly) phys->vx = pushX + phys->vx;
                }
                if (g_trace) {
                    std::printf("      [af edit] tick %d : uid %d -> v (%.4f, %.4f)%s\n", g.tick, targetObj.uid,
                                phys->vx, phys->vy, push ? " (push)" : "");
                }
            };
            if (!byControl) {
                for (int t : w.allOfGroup(o.targetGroup)) edit(t);
            } else {
                std::vector<AdvInst> const advs = g.trig.r().M().advs;
                for (auto const& inst : advs) {
                    if (inst.control != o.targetGroup) continue;
                    World::AdvDef const& def = w.advDefs[static_cast<size_t>(inst.def)];
                    if (auto const* targetGroups =
                                w.targetGroupsOf(inst.target, def.ignoreGroupParent, def.ignoreLinked)) {
                        for (int t : targetGroups->loose) edit(t);
                        for (auto const& rep : targetGroups->reps) edit(rep.obj);
                    } else {
                        for (int t : w.allOfGroup(inst.target)) edit(t);
                    }
                }
            }
            break;
        }
        case 3661: {
            int key = o.tf(Obj::TF_TargetP1)    ? -1
                    : o.tf(Obj::TF_TargetP2)  ? -2
                    : o.tf(Obj::TF_FollowCPP) ? -3
                    : o.centerGroup;
            if (key == 0) break;
            World const& w = *g.world;
            if (g.trig.r().M().advs.empty()) break;
            bool any = false;
            TrigMotion& motion = g.trig.w().Mw();
            if (!o.tf(Obj::TF_TargetControlID)) {
                for (int t : w.allOfGroup(o.targetGroup)) {
                    if (w.objs[static_cast<size_t>(t)].id != ObjId::AdvancedFollowTrigger) continue;
                    int const def =
                            static_cast<size_t>(t) < w.advDefOf.size() ? w.advDefOf[static_cast<size_t>(t)] : -1;
                    if (def < 0) continue;
                    for (auto& inst : motion.advs) {
                        if (inst.def == def) {
                            inst.key = key;
                            any = true;
                        }
                    }
                }
            } else {
                for (auto& inst : motion.advs) {
                    if (inst.control == o.targetGroup) {
                        inst.key = key;
                        any = true;
                    }
                }
            }
            if (any) {
                motion.advDirty = true;
                tl_sideEffect = true;
            }
            break;
        }
        case 3618:
            for (int t : g.world->allOfGroup(o.targetGroup)) {
                Obj const& targetObj = g.world->objs[static_cast<size_t>(t)];
                if (targetObj.id == ObjId::Checkpoint ||
                    !(targetObj.type == Collectible || targetObj.type == Breakable))
                    continue;
                g.triggeredIDs.erase(std::make_pair(targetObj.uid, 0));
                GObj const targetG = g.obj(t);
                if (targetG.st().destroyed) targetG.mut().destroyed = false;
            }
            break;
        case 3608:
            if (o.centerGroup > 0 && g.world->hasRandom) (void)tryGetObjectRef(g, o.centerGroup);
            break;
        case 3016: {
            int const def = (static_cast<size_t>(idx) < g.world->advDefOf.size()) ? g.world->advDefOf[static_cast<size_t>(idx)] : -1;
            if (def < 0) break;
            World::AdvDef const& advDef = g.world->advDefs[static_cast<size_t>(def)];
            bool const rngOnly = !advDef.relevant && tl_remap.idx != idx;
            int const key = advDef.key < 0 ? advDef.key : o.centerGroup;
            TrigHeavy& H = g.trig.w();
            bool found = false;
            TrigMotion& motion = H.Mw();
            for (auto& inst : motion.advs) {
                if (inst.def == def && inst.target == o.targetGroup && inst.key == key && inst.control == o.controlID) {
                    inst.finished = 0;
                    inst.doStart = 0;
                    inst.order = ++motion.advOrder;
                    found = true;
                }
            }
            if (!found) {
                AdvInst inst;
                inst.def = def;
                inst.target = o.targetGroup;
                inst.key = key;
                inst.control = o.controlID;
                inst.order = ++motion.advOrder;
                inst.rngOnly = rngOnly ? 1 : 0;
                motion.advs.push_back(inst);
            }
            motion.advDirty = true;
            break;
        }
        case 1912: {
            tl_sideEffect = true;
            float const r = g.rand01() * 100.f;
            spawnGroupLayer(g, o.duration < r ? o.centerGroup : o.targetGroup, false, -0.0, o.uid, o.controlID, activationKeys(idx));
            break;
        }
        case 2068: {
            tl_sideEffect = true;
            auto const* ex = g.world->extOf(o);
            std::vector<double> const* list = ex ? ex->list(152) : nullptr;
            int total = 0;
            if (list) {
                for (size_t k = 1; k < list->size(); k += 2) total += static_cast<int>((*list)[k]);
            }
            int const pick = static_cast<int>(std::round(g.rand01() * static_cast<float>(total)));
            int group = 0;
            int acc = 0;
            if (list) {
                for (size_t k = 0; k + 1 < list->size(); k += 2) {
                    acc += static_cast<int>((*list)[k + 1]);
                    if (pick <= acc) {
                        group = static_cast<int>((*list)[k]);
                        break;
                    }
                }
            }
            group = remapForActivation(g, idx, group);
            spawnGroupLayer(g, group, false, -0.0, o.uid, o.controlID, activationKeys(idx));
            break;
        }
        case 3006:
        case 3007:
        case 3008: {
            int const def = g.world->areaDefOf[static_cast<size_t>(idx)];
            if (def >= 0) areaAdd(g, def);
            break;
        }
        case 3011:
        case 3012:
        case 3013: {
            int const def = g.world->areaDefOf[static_cast<size_t>(idx)];
            if (def >= 0) areaEdit(g, def);
            break;
        }
        case 3024: {
            int const effectId = o.targetGroup;
            World const& w = *g.world;
            for (int kind = 0; kind < 3; ++kind) {
                bool any = false;
                for (auto const& inst : g.trig.r().areas[kind]) {
                    if (w.areaDefs[static_cast<size_t>(inst.def)].effectID == effectId) {
                        any = true;
                        break;
                    }
                }
                if (!any) continue;
                auto& list = g.trig.w().areasW(kind);
                for (size_t i = 0; i < list.size();) {
                    if (w.areaDefs[static_cast<size_t>(list[i].def)].effectID != effectId) {
                        ++i;
                        continue;
                    }
                    list.erase(list.begin() + static_cast<long>(i));
                    if (0 < effectId) break;
                }
            }
            break;
        }
        case 1913:
            updateZoom(g, o.zoom, o.duration, o.easing, o.easingRate, o.uid, o.controlID);
            break;
        case 1916:
            if (o.moveTargetMode != 2) {
                updateCameraOffsetX(g, o.mvX, o.duration, o.easing, o.easingRate, o.uid, o.controlID);
            }
            if (o.moveTargetMode != 1) {
                updateCameraOffsetY(g, o.mvY, o.duration, o.easing, o.easingRate, o.uid, o.controlID);
            }
            break;
        case 1935:
            g.trig.queuedTimeWarp = o.timeWarpMod;
            break;
        case 3600:
            if (!g.died && !g.completed) {
                if (o.targetGroup > 0) {
                    spawnGroupLayer(g, o.targetGroup, false, 0.0, o.uid, o.controlID, activationKeys(idx));
                }
                g.completed = true;
            }
            break;
        case 200:
        case 201:
        case 202:
        case 203:
        case 1334: {
            float speed = 0.9f;
            if (o.id == ObjId::SpeedSlow)
                speed = 0.7f;
            else if (o.id == ObjId::SpeedFast)
                speed = 1.1f;
            else if (o.id == ObjId::SpeedFaster)
                speed = 1.3f;
            else if (o.id == ObjId::SpeedFastest)
                speed = 1.6f;
            g.parkedSpeed = speed;
            g.parkedSpeedNoEffects = o.hasNoEffects;
            break;
        }
        case 3022:
            teleportPlayer(g, GObj{&o, &g, idx}, g.p1);
            break;
        case 1917:
            if (!g.p1.isPlatformer) doReversePlayer(g, g.p1, !g.p1.isGoingLeft);
            if (g.isDualMode && !g.p2.isPlatformer) doReversePlayer(g, g.p2, !g.p2.isGoingLeft);
            break;
        case 1932: {
            if (!(o.optFlags & 16)) break;
            bool const targetP1 = o.tf(Obj::TF_TargetP1);
            bool const targetP2 = o.tf(Obj::TF_TargetP2);
            auto apply = [&](Player& p) {
                if (o.optFlags & 1) {
                    if (!p.inputsLocked) p.holdingJump = false;
                    if (!p.controlsDisabled) {
                        p.jumpBuffered = false;
                        p.stateRingJump = false;
                        p.touchedPad = true;
                        if (p.isDashing) stopDashing(g, p);
                    }
                }
                if ((o.optFlags & 2) && g.isPlatformer) {
                    releaseButton(g, p, 2);
                    releaseButton(g, p, 3);
                }
                if (o.optFlags & 4) {
                    p.isRotating = false;
                    p.isBallRotating = false;
                    p.isBallRotating2 = false;
                    p.rotationSpeed = 0;
                }
                if (o.optFlags & 8) {
                    p.isAccelerating = false;
                    p.affectedByForces = false;
                }
            };
            if (targetP1 || !targetP2) apply(g.p1);
            if (targetP2 || !targetP1) apply(g.p2);
            break;
        }
        case 2899: {
            if (o.optValues < 0) break;
            auto on = [&](int bit) { return (o.optValues & bit) != 0; };
            if (o.optFlags & 1) g.unlinkDualGravity = on(1);
            if (o.optFlags & 2) {
                if (on(2))
                    disablePlayerControls(g, g.p1);
                else
                    enablePlayerControls(g, g.p1);
            }
            if (o.optFlags & 4) {
                if (on(4))
                    disablePlayerControls(g, g.p2);
                else
                    enablePlayerControls(g, g.p2);
            }
            if (o.optFlags & 8) {
                g.p1.decreaseBoostSlide = on(8);
                g.p2.decreaseBoostSlide = on(8);
            }
            break;
        }
        case 2925: {
            bool const wasFree = g.isFreeMode;
            applyCameraMode(g, g.obj(idx));
            if (ExtProps const* ex = g.world->extOf(o); ex && ex->num(112) != 0.0) {
                float easing = static_cast<float>(ex->num(113));
                float padding = static_cast<float>(ex->num(114));
                if (easing <= 1.f) easing = 1.f;
                if (40.f <= easing) easing = 40.f;
                if (padding <= 0.f) padding = 0.f;
                if (1.f <= padding) padding = 1.f;
                g.cam.easing = easing;
                g.cam.padding = padding;
            }
            if (wasFree != g.isFreeMode) trigUpdateDualGroundAfterCamera(g);
            break;
        }
        case 1914: {
            if (!g.cam.on) break;
            ExtProps const* ex = g.world->extOf(o);
            bool const exitStatic = ex && ex->num(110) != 0.0;
            bool const follow = ex && ex->num(212) != 0.0;
            bool const smooth = ex && ex->num(453) != 0.0;
            bool const instant = ex && ex->num(465) != 0.0;
            float const followEasing = ex ? static_cast<float>(ex->num(213)) : 0.f;
            float const velocityMod = ex ? static_cast<float>(ex->num(454)) : 0.f;
            int const mode = o.moveTargetMode;
            if (!exitStatic) {
                cameraStaticToGroup(g, o.centerGroup, mode != 2, mode != 1, follow, followEasing, o.duration, o.easing,
                                    o.easingRate, smooth, velocityMod);
            } else {
                cameraExitStatic(g, mode != 2, mode != 1, o.duration, o.easing, o.easingRate, smooth, velocityMod, instant);
            }
            break;
        }
        case 2062: {
            ExtProps const* ex = g.world->extOf(o);
            int const dir = ex ? static_cast<int>(ex->num(164)) : 0;
            if (dir >= 1 && dir <= 4) g.cam.edge[dir - 1] = o.targetGroup;
            break;
        }
        case 2901: {
            if (o.moveTargetMode != 2) {
                g.cam.gpX = static_cast<int>(o.mvX);
                g.cam.gpLockX = o.tf(Obj::TF_LockPX);
            }
            if (o.moveTargetMode != 1) {
                g.cam.gpY = static_cast<int>(o.mvY);
                g.cam.gpLockY = o.tf(Obj::TF_LockPY);
            }
            break;
        }
        case 2900: {
            if (!(o.rgFlags & 64)) break;
            if (o.rgFlags & 4) {
                g.currentChannel = o.rgChannel;
                bool const rev = static_cast<unsigned>(o.rgGround - 2) < 2u;
                bool const has = g.reversedChannels.contains(o.rgChannel);
                if (rev && !has)
                    g.reversedChannels.insert(o.rgChannel);
                else if (!rev && has)
                    g.reversedChannels.erase(o.rgChannel);
            }
            if (!(o.rgFlags & 8)) {
                bool const editVelocity = (o.rgFlags & 1) != 0;
                bool const overrideVelocity = (o.rgFlags & 2) != 0;
                bool const dontSlide = (o.rgFlags & 32) != 0;
                playerRotateGameplay(g, g.p1, o.rgMove, o.rgGround, editVelocity, o.rgModX, o.rgModY, overrideVelocity, dontSlide);
                if (g.isDualMode) {
                    playerRotateGameplay(g, g.p2, o.rgMove, o.rgGround, editVelocity, o.rgModX, o.rgModY, overrideVelocity, dontSlide);
                }
            }
            break;
        }
        case 2066:
            if (!o.tf(Obj::TF_FollowCPP)) {
                if (!o.tf(Obj::TF_TargetP2)) g.p1.gravityMod = o.gravityValue;
                if (!o.tf(Obj::TF_TargetP1)) g.p2.gravityMod = o.gravityValue;
            } else if (playerUid == 1)
                g.p1.gravityMod = o.gravityValue;
            else if (playerUid == 2)
                g.p2.gravityMod = o.gravityValue;
            break;
        default:
            break;
        }
    }

    // player took a custom orb (1594): toggles or spawns its group, same as PlayerObject::ringJump
    void trigCustomRing(Game& g, Player& p, int idx) {
        (void)p;
        if (!g.trig.on) return;
        Obj const& o = objAt(g, idx);
        if (g_trace) {
            std::printf("[trig] tick %d custom orb uid %d -> group %d %s\n", g.tick, o.uid, o.targetGroup, o.ringSpawnOnly ? "spawn" : (o.tf(Obj::TF_ActivateGroup) ? "on" : "off"));
        }
        {
            if (!trigHasSlot(g, idx)) g.osMut(idx).activated = true;
        }
        if (!o.ringSpawnOnly)
            toggleGroupTriggered(g, o.targetGroup, o.tf(Obj::TF_ActivateGroup), o.uid, o.controlID);
        else
            spawnGroupLayer(g, o.targetGroup, false, 0.0, o.uid, o.controlID);
    }

    // player p touches trigger idx (touch triggers and state blocks), same as playerTouchedTrigger
    void trigPlayerTouched(Game& g, Player& p, int idx) {
        Obj const& o = objAt(g, idx);
        if (o.id == ObjId::StateBlock) {
            auto& blocks = g.trig.stateBlk;
            auto it = std::find_if(blocks.begin(), blocks.end(), [idx](auto const& e) { return e.first == idx; });
            int const stamp = static_cast<int>(g.commandIndex) * 2;
            if (it == blocks.end())
                blocks.emplace_back(idx, stamp);
            else
                it->second = stamp | (it->second & 1);
        }
        if (!o.touchTriggered) return;
        if (!hasSimEffect(o.id)) return;
        int const playerId = o.singlePTouch ? 0 : p.uniqueID;
        auto const key = std::make_pair(o.uid, playerId);
        bool const touching = g.activatedObjectIDs.count(key) != 0;
        if (o.multiTriggered) {
            if (!touching) g.triggeredIDs.erase(key);
            g.activatedObjectIDs[key] = g.commandIndex;
        }
        if (!g.triggeredIDs.count(key)) {
            g.triggeredIDs.insert(key);
            triggerObject(g, idx, p.uniqueID);
        }
        triggerActivated(g, idx);
        if (!trigHasSlot(g, idx)) g.osMut(idx).activated = true;
    }

    // player collected an item or a user coin: pickup count, toggle, points
    void trigCollected(Game& g, Player& p, int idx) {
        Obj const& o = objAt(g, idx);
        if (o.tf(Obj::TF_PickupItem)) {
            int item = o.itemID;
            if (item < 0) item = 0;
            if (item > 9999) item = 9999;
            int const count = countForItem(g, item);
            updateCountForItem(g, item, o.tf(Obj::TF_SubtractCount) ? count - 1 : count + 1);
        }
        if (o.tf(Obj::TF_CollectToggle)) {
            toggleGroupTriggered(g, o.targetGroup, o.tf(Obj::TF_ActivateGroup), o.uid, o.controlID);
        }
        if (o.collectPoints != 0) g.trig.w().points += o.collectPoints;
        (void)p;
    }

    // a coin was taken: spawns its target group
    void trigCoin(Game& g, int idx) {
        Obj const& o = objAt(g, idx);
        if (o.targetGroup > 0) spawnGroupLayer(g, o.targetGroup, false, 0.0, o.uid, o.controlID, nullptr);
    }

}
