// One simulated physics step: inputs, triggers, player movement, collisions, portals and camera.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "gd.hpp"
#include "timeline.hpp"

#include <algorithm>
#include <cmath>

namespace ap::sim {
    void applyStartSettings(Game& g);

    // platformer objects can always be activated again
    static bool canMultiActivate(GObj object, bool platformer) {
        if (platformer) return true;
        return object.o->multiActivate;
    }

    // true if this player already activated the object
    bool hasBeenActivatedByPlayer(Player const& p, GObj object) {
        if (canMultiActivate(object, p.isPlatformer)) return false;
        if (object.g && trigHasSlot(*object.g, object.index)) return trigActivatedBit(*object.g, object.index);
        return p.uniqueID == 1 ? object.st().activatedByPlayer1 : object.st().activatedByPlayer2;
    }

    // marks the object as activated by both players
    void triggerActivated(GObj object) {
        ObjState& state = object.mut();
        state.activated = true;
        state.activatedByPlayer1 = true;
        state.activatedByPlayer2 = true;
    }

    // marks the object activated by this player
    void activatedByPlayer(Player& p, GObj object) {
        if (object.g && trigHasSlot(*object.g, object.index)) {
            if (!canMultiActivate(object, p.isPlatformer)) trigSetActivatedBit(*object.g, object.index);
            return;
        }
        ObjState& state = object.mut();
        state.activated = true;
        if (!canMultiActivate(object, p.isPlatformer)) {
            if (p.uniqueID == 1)
                state.activatedByPlayer1 = true;
            else
                state.activatedByPlayer2 = true;
        }
    }

    // a multi activate object only fires again once the player stopped touching it
    bool canBeActivatedByPlayer(Game& g, Player& p, GObj object) {
        auto const key = std::make_pair(object.uid(), p.uniqueID);
        if (canMultiActivate(object, p.isPlatformer)) {
            if (unsigned* lastIndex = g.activatedObjectIDs.find(key)) {
                *lastIndex = g.commandIndex;
                return false;
            }
        }
        g.activatedObjectIDs[key] = g.commandIndex;
        return !hasBeenActivatedByPlayer(p, object);
    }

    // touch triggers and speed portals (speed is applied at the end of the step)
    void playerTouchedTrigger(Game& g, Player& p, GObj object) {
        if (g.trig.on) {
            trigPlayerTouched(g, p, object.index);
            return;
        }
        if (!object.o->touchTriggered) return;
        int const playerId = object.o->singlePTouch ? 0 : p.uniqueID;
        auto const key = std::make_pair(object.uid(), playerId);
        bool const touching = g.activatedObjectIDs.count(key) != 0;
        if (object.o->multiTriggered) {
            if (!touching) g.triggeredIDs.erase(key);
            g.activatedObjectIDs[key] = g.commandIndex;
        }
        if (g.triggeredIDs.count(key)) {
            object.mut().activated = true;
            return;
        }
        g.triggeredIDs.insert(key);
        int const id = object.o->id;
        if (id == ObjId::SpeedSlow || id == ObjId::SpeedNormal || id == ObjId::SpeedFast || id == ObjId::SpeedFaster ||
            id == ObjId::SpeedFastest) {
            float speed = 0.9f;
            if (id == ObjId::SpeedSlow)
                speed = 0.7f;
            else if (id == ObjId::SpeedFast)
                speed = 1.1f;
            else if (id == ObjId::SpeedFaster)
                speed = 1.3f;
            else if (id == ObjId::SpeedFastest)
                speed = 1.6f;
            g.parkedSpeed = speed;
            g.parkedSpeedNoEffects = object.o->hasNoEffects;
        }
        object.mut().activated = true;
    }

    // corridor height of a game mode, dual / solo portals use the last mode
    float getGroundHeightForMode(Game const& g, int type) {
        for (int guard = 0; guard < 4; ++guard) {
            switch (type) {
            case 5:
            case 0x13:
            case 0x1a:
            case 0x29:
                return 300.f;
            case 0x10:
                return 240.f;
            case 0x17:
            case 0x18:
                if (g.lastPortalMode == 0x17 || g.lastPortalMode == 0x18) return 270.f;
                type = g.lastPortalMode;
                continue;
            default:
                return 270.f;
            }
        }
        return 270.f;
    }

    // places the mode corridor around the portal and starts the camera animation
    static void animateInDualGroundNew(Game& g, GObj portal, float height, bool instant) {
        float portalStartY = 0.f;
        if (portal) portalStartY = portal.o->startY;
        float corridorBottom = portalStartY - height * 0.5f;
        if (!g.disableCameraGridSnap) corridorBottom = std::floor(corridorBottom / 30.f) * 30.f;
        float const clampedBottom = 90.f <= corridorBottom ? corridorBottom : 90.f;
        g.portalCenterY = clampedBottom + height * 0.5f;
        if (g.cam.on) {
            CameraState& camera = g.cam;
            float duration = (g.isDualMode || g.hasModeGround) ? 0.4f : 0.5f;
            if (instant) duration = 0.f;
            if (0.f < duration) {
                if (!g.hasModeGround || height != g.lastModeHeight) {
                    float from = camera.dualProg;
                    if (0.f <= from) {
                        if (0.f < from) {
                            float const winHeight = g.world && g.world->winH > 0.f ? g.world->winH : 320.f;
                            from = (((1.f - g.lastModeHeight * 0.5f) + winHeight * 0.5f) * camera.dualProg) /
                                   ((1.f - height * 0.5f) + winHeight * 0.5f);
                            camera.dualProg = from;
                        }
                    } else {
                        camera.dualProg = 0.f;
                        from = 0.f;
                    }
                    trigTweenValue(g, 0x19, from, 1.f, duration, 1, 2.f, -1, -1);
                }
            } else {
                g.trig.tweens.erase(0x19);
                camera.dualProg = 1.f;
            }
            if (camera.st[1] == 0.f || camera.staticYPortal) {
                cameraStaticPos(g, 0.f, g.portalCenterY, false, true, false, duration, 1, 2.f);
                camera.staticYPortal = true;
            }
        }
        g.lastModeHeight = height;
        g.portalY = portalStartY;
        g.hasModeGround = true;
    }

    // two dual balls on the same side push each other apart
    static void checkRepellPlayer(Game& g) {
        if (!g.isDualMode || g.twoPlayerMode || !g.p1.isBall || !g.p2.isBall) return;
        if (g.p1.isUpsideDown != g.p2.isUpsideDown) return;
        float const halfHeight1 = g.p1.vehicleSize * 30.f * 0.5f;
        float const halfHeight2 = g.p2.vehicleSize * 30.f * 0.5f;
        if (!(std::fabs(g.p1.y - g.p2.y) < halfHeight2 + halfHeight1 + 5.f)) return;
        Player* target;
        if (!g.p1.isOnGround2) {
            target = &g.p1;
            if (!g.p2.isOnGround2) return;
        } else {
            target = &g.p2;
        }
        flipGravity(*target, !target->isUpsideDown, true);
        setYVelocity(*target, target->isUpsideDown ? 2.0 : -2.0);
    }

    // updates the mode corridor after a mode change
    static void updateDualGround(Game& g, Player& player, int mode, bool instant = false) {
        GObj portal = g.lastActivatedPortal1 ? g.objByUid(g.lastActivatedPortal1) : GObj{};
        if (g.isDualMode && g.lastActivatedPortal2) portal = g.objByUid(g.lastActivatedPortal2);
        if (static_cast<unsigned>(mode - 0x17) > 1u) g.lastPortalMode = mode;
        float modeHeight = getGroundHeightForMode(g, mode);
        float corridorHeight = modeHeight;
        if (g.isDualMode) {
            if (modeHeight == 240.f) modeHeight = 270.f;
            Player const& other = (player.uniqueID == g.p1.uniqueID) ? g.p2 : g.p1;
            int type;
            if (!other.isShip && !other.isBird && !other.isDart && !other.isSwing)
                type = other.isBall ? 0x10 : 6;
            else
                type = 5;
            corridorHeight = getGroundHeightForMode(g, type);
            if (corridorHeight <= modeHeight) corridorHeight = modeHeight;
        }
        if (g_trace)
            std::printf("      [corridor] mode %d height %.1f portal uid %d (start y %.2f) dual %d\n", mode,
                        corridorHeight, portal ? portal.uid() : 0, portal ? portal.o->startY : 0.f,
                        g.isDualMode ? 1 : 0);
        if (!g.isFreeMode && (corridorHeight != 270.f || mode == 0x21 || g.isDualMode)) {
            animateInDualGroundNew(g, portal, corridorHeight, instant);
        } else {
            g.hasModeGround = false;
            if (g.cam.on) {
                CameraState& camera = g.cam;
                if (camera.staticYPortal) {
                    g.trig.tweens.erase(11);
                    g.trig.tweens.erase(13);
                    camera.s6[1] = 0.f;
                    camera.s8[1] = 0.f;
                    camera.st[1] = 0.f;
                    camera.p18[1] = 0.f;
                    camera.p22[1] = 0.f;
                }
                trigTweenValue(g, 0x19, camera.dualProg, 0.f, g.isDualMode ? 0.320000022649765f : 0.4f, 1, 1.5f, -1, -1);
            }
        }
    }

    // moves the dual ground after the camera update
    void trigUpdateDualGroundAfterCamera(Game& g) { updateDualGround(g, g.p1, g.lastPortalMode); }

    // forces the ground corridor of a portal (apsim debugging)
    void debugSetCorridor(Game& g, int uid) {
        g.lastActivatedPortal1 = uid;
        updateDualGround(g, g.p1, g.lastPortalMode, true);
    }

    // mode portal taken: corridor, events, and dual gravity if both players end up in the same mode
    void playerWillSwitchMode(Game& g, Player& p, GObj object) {
        if (!object) return;
        g.lastActivatedPortal1 = object.uid();
        applyCameraMode(g, object);
        int const mode = object.getType();
        updateDualGround(g, p, mode);
        trigGameEvent(g, objectTypeToGameEvent(mode));
        if (g.isDualMode && !g.twoPlayerMode) {
            Player& other = (p.uniqueID == g.p1.uniqueID) ? g.p2 : g.p1;
            bool otherSameMode = false;
            switch (mode) {
            case 5:
                otherSameMode = other.isShip;
                break;
            case 6:
                otherSameMode = !other.isShip && !other.isBird && !other.isDart && !other.isSwing && !other.isBall && !other.isRobot && !other.isSpider;
                break;
            case 0x10:
                otherSameMode = other.isBall;
                break;
            case 0x13:
                otherSameMode = other.isBird;
                break;
            case 0x1b:
                otherSameMode = other.isRobot;
                break;
            case 0x21:
                otherSameMode = other.isSpider;
                break;
            case 0x29:
                otherSameMode = other.isSwing;
                break;
            default:
                break;
            }
            if (g_trace)
                std::printf("[portal] player %d mode %d : other player same mode=%d upside down=%d -> upside down %d\n",
                        p.uniqueID, mode, otherSameMode ? 1 : 0, other.isUpsideDown ? 1 : 0,
                        otherSameMode ? (!other.isUpsideDown ? 1 : 0) : -1);
            if (otherSameMode && !g.unlinkDualGravity) flipGravity(p, !other.isUpsideDown, true);
        }
    }

    // teleport portals, orbs and triggers: position, gravity, then velocity redirect or static force
    void teleportPlayer(Game& g, GObj object, Player& p) {
        p.wasTeleported = true;
        ++p.tpCount;
        int const id = object.o->id;
        GObj target = object.o->tpOrange ? g.objByUid(object.o->tpOrange) : GObj{};
        float yOffset = object.o->tpYOffset;
        if (target) yOffset = target.o->startY - object.o->startY;
        bool teleported = false;
        if (!target && object.o->targetGroup > 0) {
            auto const& group = g.world->allOfGroup(object.o->targetGroup);
            if (group.size() == 1)
                target = g.obj(group[0]);
            else if (group.size() > 1 && g.world->tpRngKnown) {
                g.tpRng = Game::rngNext(g.tpRng);
                float random = static_cast<float>((g.tpRng >> 16) & 0x7fffu) / 32767.f;
                if (random == 1.f) random = 0.f;
                size_t const pick = static_cast<size_t>(
                        static_cast<int>(static_cast<float>(static_cast<int>(group.size())) * random));
                if (pick < group.size()) target = g.obj(group[pick]);
            } else if (group.size() > 1) {
                bool const ignoreX = object.o->tpIgnoreX;
                bool const ignoreY = object.o->tpIgnoreY;
                Point const firstPos = g.obj(group[0]).getRealPosition();
                bool same = true;
                for (size_t k = 1; k < group.size() && same; ++k) {
                    Point const pos = g.obj(group[k]).getRealPosition();
                    same = (ignoreX || pos.x == firstPos.x) && (ignoreY || pos.y == firstPos.y);
                }
                if (same) target = g.obj(group[0]);
            }
        }
        if (target) {
            {
                Point const pos = object.getPosition();
                p.lastPortalX = pos.x;
                p.lastPortalY = pos.y;
                p.lastActivatedPortal = object.uid();
                p.fallStartY = 0;
                float newX;
                float newY;
                if (id == 0x2eb) {
                    newX = p.x;
                    newY = yOffset + object.getRealPosition().y;
                } else {
                    Point const targetPos = target.getRealPosition();
                    newX = targetPos.x;
                    newY = targetPos.y;
                }
                if (object.o->tpSaveOffset) {
                    Point const objectPos = object.getRealPosition();
                    newX = newX - (objectPos.x - p.x);
                    newY = newY - (objectPos.y - p.y);
                }
                if (object.o->tpIgnoreX) newX = p.x;
                if (object.o->tpIgnoreY) newY = p.y;
                if (g_trace)
                    std::printf("[teleport] tick %d uid %d id %d : player (%.3f, %.3f) -> (%.3f, %.3f) target uid %d\n", g.tick, object.uid(), id, p.x, p.y, newX, newY, target.uid());
                p.x = newX;
                p.y = newY;
                playerMoved(p);
                teleported = true;
            }
        }
        int const gravity = object.o->tpGravity;
        if (0 < gravity) {
            bool flip = false;
            bool apply = true;
            if (gravity == 1)
                flip = false;
            else if (gravity == 2)
                flip = true;
            else if (gravity == 3)
                flip = !p.isUpsideDown;
            else
                apply = false;
            if (apply) flipGravity(p, flip, true);
        }
        if (id != 0xbce) activatedByPlayer(p, object);
        if (p.isDashing && object.o->tpRedirectDash) updateDashArt(g, p);
        if (teleported) {
            p.lastPortalX = target.getPosition().x;
            p.lastPortalY = target.getPosition().y;
            p.lastActivatedPortal = target.uid();
            p.isOnGround2 = false;
            p.lastGroundedX = 0;
            p.lastGroundedY = 0;
        }
        if (object.o->tpRedirectForce) {
            float angle;
            if (target) {
                int const targetId = target.o->id;
                float const base = (targetId == 38 || targetId == 747 || targetId == 749 || targetId == 2064 || targetId == 2902) ? 180.f : 90.f;
                angle = (target.flipXEff() ? 180.f : 0.f) + (base - target.nodeRotX());
            } else {
                angle = (object.flipXEff() ? 180.f : 0.f) - object.nodeRotX();
            }
            float const targetAngle = angle * 0.017453292f;
            float vx = 0.f;
            float vy = static_cast<float>(p.yVelocity);
            float const velocityAngle = std::atan2(vy, vx);
            float delta = targetAngle - velocityAngle;
            float const pi = 3.1415927f;
            if (delta < -pi)
                delta = std::ceil(std::floor(std::fabs(delta) / pi) * 0.5f) * (pi + pi) + delta;
            else if (pi < delta)
                delta = delta - std::ceil(std::floor(std::fabs(delta) / pi) * 0.5f) * (pi + pi);
            if (delta != 0.f) {
                float const cs = gcos(delta);
                float const sn = gsin(delta);
                float const rotatedX = vx * cs - vy * sn;
                float const rotatedY = vy * cs + vx * sn;
                vx = rotatedX;
                vy = rotatedY;
            }
            float const redirectMod = object.o->tpRedirectMod;
            float const redirectMin = object.o->tpRedirectMin;
            float const redirectMax = object.o->tpRedirectMax;
            vx *= redirectMod;
            vy *= redirectMod;
            float const length = std::sqrt(vx * vx + vy * vy);
            if (redirectMax > 0.f && length > redirectMax) {
                float const factor = redirectMax / length;
                vx *= factor;
                vy *= factor;
            } else if (redirectMin > 0.f && redirectMin > length) {
                if (length == 0.f) {
                    vx = gcos(targetAngle) * redirectMin;
                    vy = gsin(targetAngle) * redirectMin;
                } else {
                    float const factor = redirectMin / length;
                    vx *= factor;
                    vy *= factor;
                }
            }
            if (p.isSideways) std::swap(vx, vy);
            p.isAccelerating = true;
            p.yVelocity = static_cast<double>(vy);
        } else if (object.o->tpStaticForce) {
            float const staticForce = object.o->tpStaticForceV;
            bool const add = object.o->tpStaticForceAdd;
            if (staticForce == 0.f && !add) {
                p.isAccelerating = false;
                p.yVelocity = 0.0;
            } else {
                float angle;
                if (target) {
                    int const targetId = target.o->id;
                    float const base = (targetId == 38 || targetId == 747 || targetId == 749 || targetId == 2064 || targetId == 2902) ? 180.f : 90.f;
                    angle = (target.flipXEff() ? 180.f : 0.f) + (base - target.nodeRotX());
                } else {
                    angle = (object.flipXEff() ? 180.f : 0.f) - object.nodeRotX();
                }
                float const radians = angle * 0.017453292f;
                float dx = gcos(radians);
                float dy = gsin(radians);
                float const length = std::sqrt(dx * dx + dy * dy);
                if (length > 0.f) {
                    dx = dx / length * staticForce;
                    dy = dy / length * staticForce;
                    if (p.isSideways) std::swap(dx, dy);
                    p.isAccelerating = true;
                    p.yVelocity = static_cast<double>(dy) + (add ? p.yVelocity : 0.0);
                }
            }
        }
        if (object.o->tpEase) g.cam.exitTimer = 0.5f;
        if (object.o->tpInstantCam) {
            g.cam.instX = true;
            g.cam.instY = true;
        }
        if (object.o->tpSnapGround) {
            p.lastGroundedX = p.x;
            p.lastGroundedY = p.y;
        }
        if (teleported) cameraCheckLimitAfterTeleport(g, p, object.o->tpEase ? 180.f : 60.f);
        trigGameEvent(g, GameEvent::Teleported);
    }

    // copies position, gravity, mode, speed and size from src to dst (entering dual)
    static void copyAttributes(Game& g, Player& dst, Player const& src) {
        dst.maybeReducedEffects = true;
        dst.totalTime = src.totalTime;
        dst.x = src.x;
        dst.y = src.y;
        playerMoved(dst);
        dst.shipRotX = src.shipRotX;
        dst.shipRotY = src.shipRotY;
        flipGravity(dst, src.isUpsideDown, true);
        doReversePlayer(g, dst, src.isGoingLeft);
        auto updateDashIfOn = [&](bool changed, bool enable) {
            if (changed && enable) updateDashArt(g, dst);
        };
        updateDashIfOn(toggleFlyMode(dst, src.isShip, false), src.isShip);
        updateDashIfOn(toggleBirdMode(dst, src.isBird, false), src.isBird);
        updateDashIfOn(toggleRollMode(dst, src.isBall, false), src.isBall);
        updateDashIfOn(toggleDartMode(dst, src.isDart, false), src.isDart);
        updateDashIfOn(toggleRobotMode(dst, src.isRobot, false), src.isRobot);
        updateDashIfOn(toggleSpiderMode(dst, src.isSpider, false), src.isSpider);
        updateDashIfOn(toggleSwingMode(dst, src.isSwing, false), src.isSwing);
        updateTimeMod(dst, src.playerSpeed, false);
        togglePlayerScale(dst, src.vehicleSize != 1.f, false);
        setYVelocity(dst, src.yVelocity);
        dst.maybeReducedEffects = false;
        dst.jumpBuffered = src.jumpBuffered;
        dst.stateRingJump = src.stateRingJump;
    }

    // camera settings of a portal / camera mode trigger (free mode, grid snap, easing, padding)
    void applyCameraMode(Game& g, GObj object) {
        if (!object || object.o->cls != 1 || !(object.o->camFlags & 4)) return;
        g.isFreeMode = (object.o->camFlags & 1) != 0;
        g.disableCameraGridSnap = (object.o->camFlags & 2) != 0;
        if (object.o->id != ObjId::CameraModeTrigger && g.world)
            if (ExtProps const* props = g.world->extOf(*object.o); props && props->num(112) != 0.0) {
                float easing = static_cast<float>(props->num(113));
                float padding = static_cast<float>(props->num(114));
                if (easing <= 1.f) easing = 1.f;
                if (40.f <= easing) easing = 40.f;
                if (padding <= 0.f) padding = 0.f;
                if (1.f <= padding) padding = 1.f;
                g.cam.easing = easing;
                g.cam.padding = padding;
            }
    }

    // dual portal on: p2 copies p1 with opposite gravity; off: p2 is hidden
    void toggleDualMode(Game& g, GObj object, bool dual, Player& p, bool noEffects) {
        (void)noEffects;
        if (g.isDualMode == dual) return;
        g.isDualMode = dual;
        if (dual) g.p2Origin = false;
        g.p1.unkA99 = dual;
        g.p2.unkA99 = dual;
        if (dual) {
            Player& p1 = g.p1;
            Player& p2 = g.p2;
            resetObject(g, p2);
            copyAttributes(g, p2, p1);
            if (!g.unlinkDualGravity) {
                flipGravity(p2, !p1.isUpsideDown, true);
                setYVelocity(p2, -p1.yVelocity);
            } else {
                flipGravity(p2, p1.isUpsideDown, true);
                setYVelocity(p2, p1.yVelocity);
            }
            p2.isOnGround = false;
            p2.isOnGround2 = false;
            p2.isHidden = p1.isHidden;
            if (g.world->twoPlayer) {
                if (!p2.inputsLocked) p2.holdingJump = false;
                if (!p2.controlsDisabled) {
                    p2.jumpBuffered = false;
                    p2.stateRingJump = false;
                    p2.touchedPad = true;
                    if (p2.isDashing) stopDashing(g, p2);
                }
            }
            if (object) g.lastActivatedPortal2 = object.uid();
            playerWillSwitchMode(g, p, object);
            return;
        }
        if (p.uniqueID == g.p2.uniqueID) copyAttributes(g, g.p1, g.p2);
        g.p2.isHidden = true;
        releaseAllButtons(g, g.p2);
        g.lastActivatedPortal2 = 0;
        applyCameraMode(g, object);
        Player const& p1 = g.p1;
        int type;
        if (!p1.isShip && !p1.isBird && !p1.isDart && !p1.isSwing)
            type = p1.isBall ? 0x10 : (p1.isSpider ? 0x21 : 6);
        else
            type = 5;
        updateDualGround(g, p, type);
    }

    // kills the player, unless noclip
    void destroyPlayer(Game& g, Player& p, GObj object) {
        if (g.noclip) return;
        if (g.p1.isLocked) return;
        if (g.playerDied) return;
        g.playerDied = true;
        g.died = true;
        g.killerUid = object ? object.uid() : 0;
        g.deadPlayer = p.uniqueID;
        p.isDead = true;
        if (g.isDualMode) {
            g.p1.isDead = true;
            g.p2.isDead = true;
        }
    }

    // press / release a button, sent to p2 too in dual unless a dual touch trigger takes it
    void handleButton(Game& g, bool down, int button, bool isPlayer1) {
        bool dualTouch = false;
        if (g.trig.on) {
            for (auto const& touch : g.trig.r().touches) {
                if (touch.dual && !touch.stopped && !touch.paused) {
                    dualTouch = true;
                    break;
                }
            }
        }
        if (down)
            g.dualTouchTrigger = dualTouch;
        else if (!g.dualTouchTrigger)
            dualTouch = false;
        bool handled = false;
        bool const skipAll = !isPlayer1 && dualTouch;
        if (!skipAll) {
            if (isPlayer1 || !g.twoPlayerMode)
                handled = down ? pushButton(g, g.p1, button) : releaseButton(g, g.p1, button);
            if (!g.dualTouchTrigger && g.isDualMode && (!isPlayer1 || !g.twoPlayerMode))
                handled = down ? pushButton(g, g.p2, button) : releaseButton(g, g.p2, button);
            if (handled && button >= 1 && button <= 3)
                trigGameEvent(g, GameEvent::JumpPush + 2 * (button - 1) + (down ? 0 : 1));
        }
        if (g.trig.on) trigButton(g, down, isPlayer1);
    }

    // new players at the start position with the level physics options
    void resetPlayer(Game& g) {
        g.p1 = Player{};
        g.p2 = Player{};
        g.p1.uniqueID = 1;
        g.p2.uniqueID = 2;
        g.p2.isSecondPlayer = true;
        g.p1.x = g.startX;
        g.p1.y = g.startY;
        playerMoved(g.p1);
        g.p2.x = g.startX;
        g.p2.y = g.startY;
        playerMoved(g.p2);
        g.p1.lastX = g.p1.x;
        g.p1.lastY = g.p1.y;
        g.p2.lastX = g.p2.x;
        g.p2.lastY = g.p2.y;
        resetObject(g, g.p1);
        resetObject(g, g.p2);
        for (Player* pp : {&g.p1, &g.p2}) {
            Player& p = *pp;
            p.isPlatformer = g.isPlatformer;
            p.enable22Changes = !g.enable22Changes;
            if (g.world) {
                p.fixRobotJump = !g.world->fixRobotJump;
                p.decreaseBoostSlide = !g.world->decreaseBoostSlide;
                p.disablePlayerSqueeze = !g.world->enablePlayerSqueeze && !g.isPlatformer;
                p.fixGravityBug = g.world->fixGravityBug;
                p.enableImpulseFix = g.world->enableImpulseFix;
                p.reverseSync = g.world->reverseSync;
            } else {
                p.fixRobotJump = true;
                p.decreaseBoostSlide = true;
                p.disablePlayerSqueeze = !g.isPlatformer;
            }
        }
        if (g.world) applyStartSettings(g);
    }

    // start gravity, reverse, dual, size, mode and speed from the level settings
    void applyStartSettings(Game& g) {
        World const& w = *g.world;
        flipGravity(g.p1, w.startFlipped, true);
        doReversePlayer(g, g.p1, w.startReverse);
        g.p1.isSideways = w.rotateGameplay;
        if (w.startDual) toggleDualMode(g, GObj{}, true, g.p1, true);
        togglePlayerScale(g.p1, w.startMini, true);
        if (g.isDualMode) {
            togglePlayerScale(g.p2, w.startMini, true);
            flipGravity(g.p2, !g.p1.isUpsideDown, true);
            doReversePlayer(g, g.p2, g.p1.isGoingLeft);
        }
        int type = 6;
        switch (w.startMode) {
        case 1:
            type = 5;
            break;
        case 2:
            type = 0x10;
            break;
        case 3:
            type = 0x13;
            break;
        case 4:
            type = 0x1a;
            break;
        case 5:
            type = 0x1b;
            break;
        case 6:
            type = 0x21;
            break;
        case 7:
            type = 0x29;
            break;
        default:
            break;
        }
        for (Player* pp : {&g.p1, &g.p2}) {
            Player& p = *pp;
            if (type != CubePortal) switchedToMode(p, type);
            toggleFlyMode(p, type == ShipPortal, true);
            toggleBirdMode(p, type == UfoPortal, true);
            toggleRollMode(p, type == BallPortal, true);
            toggleDartMode(p, type == WavePortal, true);
            toggleRobotMode(p, type == RobotPortal, true);
            toggleSpiderMode(p, type == SpiderPortal, true);
            toggleSwingMode(p, type == SwingPortal, true);
            if (!g.isDualMode) break;
        }
        updateDualGround(g, g.p1, type, true);
        float speed = 0.f;
        switch (w.startSpeed) {
        case 1:
            speed = 0.7f;
            break;
        case 2:
            speed = 1.1f;
            break;
        case 3:
            speed = 1.3f;
            break;
        case 4:
            speed = 1.6f;
            break;
        default:
            break;
        }
        if (speed != 0.f) {
            g.parkedSpeed = 0;
            g.parkedSpeedNoEffects = false;
            updateTimeMod(g.p1, speed, false);
            if (g.isDualMode) updateTimeMod(g.p2, speed, false);
        }
    }

    // resets the game to the level start
    void Game::reset() {
        objState.clear();
        objCold.reset();
        objColdX = -1e30f;
        objColdShadow = 0;
        trigReset(*this);
        cameraOffsetY = 0.f;
        isDualMode = false;
        timeWarp = 1.f;
        cameraZoom = 1.f;
        targetCameraZoom = 1.f;
        mirrorOn = false;
        mirrorBack = false;
        mirrorT = 0.f;
        mirrorDir = 1.f;
        cameraReset(*this);
        levelTime = 0;
        time2 = 0;
        p2Btn = false;
        p1Btn = false;
        dualTouchTrigger = false;
        circleOption = world ? world->fixRadiusCollision : false;
        commandIndex = 0;
        ringPrevCI = 0;
        rng = rngSeed;
        tpRng = world ? static_cast<uint32_t>(world->tpRngSeed) : 0u;
        currentProgress = 0;
        parkedSpeed = 0;
        parkedSpeedNoEffects = false;
        lastMaxPortalY = 0;
        playerDied = false;
        died = false;
        killerUid = 0;
        deadPlayer = 0;
        completed = false;
        tick = 0;
        tlTick = 0;
        activatedObjectIDs.clear();
        triggeredIDs.clear();
        lastActivatedPortal1 = 0;
        lastActivatedPortal2 = 0;
        lastPortalMode = 0;
        spikeOn = !isEditor && !isPracticeMode;
        spikeVerified = false;
        spikeX = 0.f;
        spikeY = 0.f;
        hasModeGround = false;
        lastModeHeight = 0;
        portalCenterY = 0;
        portalY = 0;
        isFreeMode = false;
        disableCameraGridSnap = world ? world->gridSnapOff : false;
        currentChannel = 0;
        reversedChannels.clear();
        if (world) {
            maxGameplayY = world->maxGameplayY;
            isPlatformer = world->platformer;
        }
        resetPlayer(*this);
        p2Origin = !isDualMode;
        p2.touchedPad = true;
        lastMaxPortalY =
                (world && world->startLastMaxPortalY >= 0.f) ? world->startLastMaxPortalY : getMaxPortalY(*this);
        if (trig.on) trigCheckSpawnObjects(*this);
        if (cam.on) {
            cameraUpdate(*this, 0.f);
            cam.sdx = (world && !world->camUnk322a && world->winW > 0.f) ? 15.f - (75.f - world->winW * 0.5f) : 0.f;
            cam.sdy = 0.f;
        }
    }

    // forgets activation state of objects 600 units behind the player
    void pruneBehind(Game& g) {
        float behind = g.p1.x;
        if (g.isDualMode) behind = std::min(behind, g.p2.x);
        behind -= 600.f;
        if (g.p1.isGoingLeft || g.p2.isGoingLeft) return;
        World const& w = *g.world;
        auto isFar = [&](int uid) {
            int const idx = w.indexOfUid(uid);
            if (idx < 0) return false;
            Obj const& o = w.objs[static_cast<size_t>(idx)];
            return o.rect.x + o.rect.w < behind && o.x < behind;
        };
        if (g.activatedObjectIDs.size() > 24) {
            auto& items = g.activatedObjectIDs.items;
            items.erase(std::remove_if(items.begin(), items.end(), [&](auto const& e) { return isFar(e.first.first); }), items.end());
        }
        if (g.triggeredIDs.size() > 24) {
            auto& items = g.triggeredIDs.items;
            items.erase(std::remove_if(items.begin(), items.end(), [&](auto const& e) { return isFar(e.first); }), items.end());
        }
        if (g.objState.size() > 24) {
            auto& items = g.objState.items;
            items.erase(std::remove_if(items.begin(), items.end(), [&](auto const& e) {
                Obj const& o = w.objs[static_cast<size_t>(e.first)];
                if (g.trig.on && o.movable) return false;
                    return o.rect.x + o.rect.w < behind && o.x < behind;
                }), items.end());
        }
        if (g.trig.on) trigPrune(g, behind);
    }

    // moves object states 3000 units behind into the shared cold map (when pruning isn't safe)
    void archiveBehind(Game& g) {
        World const& w = *g.world;
        float coldX = g.p1.x;
        if (g.isDualMode) coldX = std::min(coldX, g.p2.x);
        coldX -= 3000.f;
        if (coldX < g.objColdX) coldX = g.objColdX;
        auto cold = [&](std::pair<int, ObjState> const& e) {
            Obj const& o = w.objs[static_cast<size_t>(e.first)];
            if (o.movable || (o.type == Modifier && !o.isSpeedPortal())) return false;
            return o.rect.x + o.rect.w < coldX;
        };
        size_t coldCount = 0;
        for (auto const& e : g.objState.items)
            if (cold(e)) ++coldCount;
        if (coldCount < 48) return;
        auto archive = std::make_shared<FlatMap<int, ObjState>>();
        auto& out = archive->items;
        out.reserve((g.objCold ? g.objCold->items.size() : 0) + coldCount);
        static thread_local std::vector<std::pair<int, ObjState>> moved;
        moved.clear();
        for (auto const& e : g.objState.items)
            if (cold(e)) moved.push_back(e);
        size_t i = 0;
        size_t j = 0;
        auto const* oldItems = g.objCold ? &g.objCold->items : nullptr;
        size_t const oldCount = oldItems ? oldItems->size() : 0;
        while (i < oldCount || j < moved.size()) {
            if (j >= moved.size() || (i < oldCount && (*oldItems)[i].first < moved[j].first)) {
                out.push_back((*oldItems)[i++]);
            } else {
                if (i < oldCount && (*oldItems)[i].first == moved[j].first) ++i;
                out.push_back(moved[j++]);
            }
        }
        for (auto const& e : out) archive->bloom.set(FlatMap<int, ObjState>::bloomHash(e.first));
        auto& items = g.objState.items;
        items.erase(std::remove_if(items.begin(), items.end(), cold), items.end());
        g.objState.bloom.clear();
        for (auto const& e : items) g.objState.bloom.set(FlatMap<int, ObjState>::bloomHash(e.first));
        g.objCold = std::move(archive);
        g.objColdX = coldX;
        g.objColdShadow = 0;
    }

    void stepTickImpl(Game& g, uint8_t buttonDown);

    // one tick; with APSIM_MEMOCHECK, also runs it without shared trigger results and reports any difference
    void stepTick(Game& g, uint8_t buttonDown) {
        int const check = debugOptions().memoCheck;
        if (!check || !g.trig.on) {
            stepTickImpl(g, buttonDown);
            return;
        }
        Game ref = g;
        static std::atomic<int> traced{0};
        Game before = (check & 4) ? g : Game{};
        g_noShare = check & 3;
        stepTickImpl(ref, buttonDown);
        g_noShare = 0;
        stepTickImpl(g, buttonDown);
        static std::atomic<int> reported{0};
        bool const mismatch =
                !(ref.died && g.died) &&
                (ref.p1.x != g.p1.x || ref.p1.y != g.p1.y || ref.p1.yVelocity != g.p1.yVelocity || ref.died != g.died ||
                trigHash(ref) != trigHash(g) ||
                ((ref.tickGap < 2.5f || g.tickGap < 2.5f) && ref.tickGap != g.tickGap) ||
                ((ref.obbGap < 13.f || g.obbGap < 13.f) && ref.obbGap != g.obbGap) ||
                ref.p1.rotation != g.p1.rotation || ref.osSize() != g.osSize() || ref.itemChanges != g.itemChanges ||
                ref.p1.isOnGround != g.p1.isOnGround || ref.triggeredIDs.size() != g.triggeredIDs.size() ||
                ref.rng != g.rng || ref.tpRng != g.tpRng);
        if (mismatch && reported.fetch_add(1) < 12) {
            std::fprintf(stderr, "[memo] tick %d : mismatch ; ref (%.4f, %.4f, dead %d, trig %08x) shared (%.4f, %.4f, dead %d, trig %08x)\n", g.tick - 1, ref.p1.x, ref.p1.y, ref.died, trigHash(ref), g.p1.x, g.p1.y, g.died, trigHash(g));
            std::fprintf(stderr, "items %u / %u ; trigIDs %zu / %zu ; ground %d / %d ; rng %u / %u ; teleporters %u / %u\n",
                        ref.itemChanges, g.itemChanges, ref.triggeredIDs.size(), g.triggeredIDs.size(),
                        ref.p1.isOnGround, g.p1.isOnGround, ref.rng, g.rng, ref.tpRng, g.tpRng);
            std::fprintf(stderr, "gaps ref %.3f %.3f shared %.3f %.3f ; rot %.3f / %.3f ; objState %zu / %zu\n",
                        ref.tickGap, ref.obbGap, g.tickGap, g.obbGap, ref.p1.rotation, g.p1.rotation, ref.osSize(), g.osSize());
            std::fprintf(stderr, "    ground ref %d (uid %d) shared %d (uid %d) ; yv %.4f / %.4f ; mode %d%d%d\n",
                        ref.p1.isOnGround, ref.p1.lastGroundObject, g.p1.isOnGround, g.p1.lastGroundObject,
                        ref.p1.yVelocity, g.p1.yVelocity, g.p1.isShip, g.p1.isBall, g.p1.isBird);
            if (trigHeavyHash(*ref.trig.h) != trigHeavyHash(*g.trig.h)) trigHeavyDiff(*ref.trig.h, *g.trig.h);
            if ((check & 4) && traced.fetch_add(1) == 0) {
                std::fflush(stdout);
                std::printf("===== trace without sharing\n");
                {
                    Game game = before;
                    g_noShare = check & 3;
                    g_trace = true;
                    stepTickImpl(game, buttonDown);
                    g_trace = false;
                    g_noShare = 0;
                }
                std::printf("===== trace with sharing\n");
                {
                    Game game = before;
                    g_trace = true;
                    stepTickImpl(game, buttonDown);
                    g_trace = false;
                }
                std::fflush(stdout);
            }
            if (!trigPvEqual(ref.trig, g.trig)) {
                TrigPv const* refPv = ref.trig.pv.get();
                TrigPv const* sharedPv = g.trig.pv.get();
                std::fprintf(
                        stderr,
                        "    differs : player part (off %zu / %zu, dyn %zu / %zu, af %zu / %zu, afObjs %zu / %zu)\n",
                        refPv ? refPv->off.size() : 0, sharedPv ? sharedPv->off.size() : 0,
                        refPv ? refPv->dyn.size() : 0, sharedPv ? sharedPv->dyn.size() : 0,
                        refPv ? refPv->af.size() : 0, sharedPv ? sharedPv->af.size() : 0,
                        refPv ? refPv->afObjs.size() : 0, sharedPv ? sharedPv->afObjs.size() : 0);
                if (refPv && sharedPv) {
                    int diffCount = 0;
                    for (auto const& e : refPv->off.items) {
                        auto const* o = sharedPv->off.find(e.first);
                        if (!o || memcmp(o, &e.second, sizeof(GroupOff))) {
                            if (diffCount++ < 4)
                                std::fprintf(stderr, "group %d : ref (%.6f, %.6f st %d px %.4f) shared (%.6f, %.6f st %d px %.4f)\n",
                                            e.first, e.second.x, e.second.y, e.second.stamp, e.second.px,
                                            o ? o->x : 0.0, o ? o->y : 0.0, o ? o->stamp : -9, o ? o->px : 0.0);
                        }
                    }
                    for (auto const& e : sharedPv->off.items)
                        if (!refPv->off.find(e.first) && diffCount++ < 6)
                            std::fprintf(stderr, "group %d only shared (%.6f, %.6f)\n", e.first, e.second.x, e.second.y);
                    for (auto const& e : refPv->dyn.items) {
                        auto const* o = sharedPv->dyn.find(e.first);
                        if (!o || memcmp(o, &e.second, sizeof(ObjDyn))) {
                            if (diffCount++ < 8)
                                std::fprintf(stderr, "obj %d (uid %d) : ref (%.6f, %.6f last %.4f st %d) shared (%.6f, %.6f last %.4f st %d)\n",
                                            e.first, g.world->objs[static_cast<size_t>(e.first)].uid, e.second.x,
                                            e.second.y, e.second.lastX, e.second.stamp, o ? o->x : 0.0, o ? o->y : 0.0,
                                            o ? o->lastX : 0.f, o ? o->stamp : -9);
                        }
                    }
                    for (auto const& e : sharedPv->dyn.items)
                        if (!refPv->dyn.find(e.first) && diffCount++ < 10)
                            std::fprintf(stderr, "      obj %d only shared\n", e.first);
                }
            }
            if (trigDynHash(*ref.trig.d) != trigDynHash(*g.trig.d)) {
                std::fprintf(stderr, "    differs : own state (%zu / %zu entries ; af %zu / %zu)\n",
                            ref.trig.d->objDyn.size(), g.trig.d->objDyn.size(), ref.trig.d->afPhys.size(), g.trig.d->afPhys.size());
                auto const& refDyn = ref.trig.d->objDyn.items;
                auto const& sharedDyn = g.trig.d->objDyn.items;
                int diffCount = 0;
                for (size_t k = 0; k < std::min(refDyn.size(), sharedDyn.size()) && diffCount < 4; ++k)
                    if (refDyn[k].first != sharedDyn[k].first ||
                        memcmp(&refDyn[k].second, &sharedDyn[k].second, sizeof(ObjDyn))) {
                        ++diffCount;
                        Obj const& o = g.world->objs[static_cast<size_t>(refDyn[k].first)];
                        std::fprintf(stderr, "obj uid %d id %d (grp %d) : ref (%.6f, %.6f last %.4f,%.4f st %d) shared (%.6f, %.6f last %.4f,%.4f st %d)\n",
                                    o.uid, o.id, o.groups.empty() ? -1 : o.groups[0], refDyn[k].second.x,
                                    refDyn[k].second.y, refDyn[k].second.lastX, refDyn[k].second.lastY,
                                    refDyn[k].second.stamp, sharedDyn[k].second.x, sharedDyn[k].second.y,
                                    sharedDyn[k].second.lastX, sharedDyn[k].second.lastY, sharedDyn[k].second.stamp);
                    }
                auto const& refPhys = ref.trig.d->afPhys.items;
                auto const& sharedPhys = g.trig.d->afPhys.items;
                for (size_t k = 0; k < std::min(refPhys.size(), sharedPhys.size()); ++k)
                    if (refPhys[k].first != sharedPhys[k].first ||
                        memcmp(&refPhys[k].second, &sharedPhys[k].second, sizeof(AfPhys))) {
                        std::fprintf(stderr, "afPhys obj %d : ref v(%.4f,%.4f) last %d / shared v(%.4f,%.4f) last %d\n",
                                    refPhys[k].first, refPhys[k].second.vx, refPhys[k].second.vy,
                                    refPhys[k].second.last, sharedPhys[k].second.vx, sharedPhys[k].second.vy, sharedPhys[k].second.last);
                        break;
                    }
                if (ref.trig.d->afObjs != g.trig.d->afObjs)
                    std::fprintf(stderr, "afObjs differ (%zu / %zu)\n", ref.trig.d->afObjs.size(), g.trig.d->afObjs.size());
            }
        }
    }

    // one 1/240 tick: timeline camera keys, physics step, then dropping old object states
    void stepTickImpl(Game& g, uint8_t buttonDown) {
        g.tlTick = g.tick;
        Timeline const* timeline = g.world ? g.world->timeline : nullptr;
        if (timeline && !timeline->xTicks.empty() && !g.trig.on) {
            float const dt = 0.25f * (g.timeWarp < 1.f ? g.timeWarp : 1.f);
            float const xNext =
                    g.p1.x + static_cast<float>(static_cast<double>(g.p1.playerSpeed) * g.p1.speedMultiplier *
                                                static_cast<double>(dt) * (g.p1.isGoingLeft ? -1.0 : 1.0));
            g.tlTick = timeline->tickForX(xNext, g.tick + timeline->tickOffset) - timeline->tickOffset;
        }
        g.camKey = (timeline && (!g.trig.on || g.trig.camTrack)) ? timeline->camAt(timeline->xTicks.empty() ? g.tick : timeline->tickForX(g.p1.x, g.tick)) : nullptr;
        if (g.camKey) {
            g.timeWarp = g.camKey->timeWarp;
            g.cameraZoom = g.camKey->zoom;
            g.targetCameraZoom = g.camKey->zoomChanging ? g.camKey->zoom + 1.f : g.camKey->zoom;
            g.cameraOffsetY = g.camKey->offsetY;
        }
        float const warpBefore = g.timeWarp;
        stepTickDt(g, buttonDown, 0.25f * (g.timeWarp < 1.f ? g.timeWarp : 1.f), false);
        if (g.world && g.world->canPruneBehind && (g.tick & 63) == 0 && g.objState.size() > 48 && !g.p1.isGoingLeft &&
            !g.p1.isSideways && !(g.isDualMode && g.p2.isGoingLeft)) {
            World const& w = *g.world;
            float const behind = std::min(g.p1.x, g.isDualMode ? g.p2.x : g.p1.x) - 3000.f;
            auto& items = g.objState.items;
            items.erase(std::remove_if(items.begin(), items.end(), [&](auto const& e) {
                Obj const& o = w.objs[static_cast<size_t>(e.first)];
                if (o.type == Modifier || o.rect.x + o.rect.w >= behind) return false;
                if (!o.movable) return true;
                Rect const r = g.obj(e.first).getObjectRect();
                return r.x + r.w < behind;
                }), items.end());
        } else if (g.world && !g.world->canPruneBehind && (g.tick & 63) == 0 && g.objState.size() > 96) {
            archiveBehind(g);
        }
        if (g.camKey) {
            CamKey const* nextKey =
                    timeline->camAt(timeline->xTicks.empty() ? g.tick : timeline->tickForX(g.p1.x, g.tick));
            if (nextKey && nextKey->timeWarp != warpBefore) {
                double const ratio = static_cast<double>(std::min(nextKey->timeWarp, 1.f)) / static_cast<double>(std::min(warpBefore, 1.f));
                g.levelTime += (0.25 * static_cast<double>(std::min(warpBefore, 1.f)) / 60.0) * (ratio - 1.0);
                g.p1.totalTime = g.levelTime;
                g.p2.totalTime = g.levelTime;
            }
        }
    }

    // trigger work done before the step (frame snapshot, begin tick)
    void stepPre(Game& g, float dt, bool betweenSteps) {
        EventGameScope const evScope(&g);
        double const dSecF = static_cast<double>(static_cast<float>(static_cast<double>(dt) / 60.0));
        if (g.trig.on && !betweenSteps) trigFrameSnap(g);
        if (g.trig.on) trigBeginTick(g, dSecF, false);
        g.preDone = true;
    }

    // one physics step: inputs, triggers, players, collisions, rotation, camera
    void stepTickDt(Game& g, uint8_t buttons, float dt, bool betweenSteps) {
        EventGameScope const evScope(&g);
        bool const buttonDown = (buttons & 1) != 0;
        g.tickGap = 1e9f;
        g.obbGap = 1e9f;
        g.hazGap = 1e9f;
        g.rotAmb = false;
        double const dSec = static_cast<double>(dt) / 60.0;
        g.levelTime += static_cast<double>(static_cast<float>(dSec));
        g.time2 += dSec / static_cast<double>(g.timeWarp > 0.f ? g.timeWarp : 1.f);
        g.p1.totalTime = g.levelTime;
        g.p2.totalTime = g.levelTime;
        g.isBetweenSteps = betweenSteps;
        bool const secondHalf = betweenSteps && g.splitHalf == 1;
        g.splitHalf = betweenSteps ? static_cast<uint8_t>(g.splitHalf ^ 1u) : 0;
        double const dSecF = static_cast<double>(static_cast<float>(dSec));
        if (g.preDone) {
            g.preDone = false;
        } else {
            if (g.trig.on && !betweenSteps) trigFrameSnap(g);
            if (g.trig.on) trigBeginTick(g, dSecF, secondHalf);
        }
        g.commandIndex += betweenSteps ? 1 : 2;
        trigAfCommands(g, betweenSteps);
        if (!g.playerDied) {
            bool const modInputs = g.world && (g.world->modPlays || g.world->frameCadence > 1);
            if (modInputs ? (buttonDown != g.p1Btn) : (buttonDown != g.p1.holdingJump))
                handleButton(g, buttonDown, 1, true);
            g.p1Btn = buttonDown;
            if (g.twoPlayerMode) {
                bool const button2 = (buttons & 2) != 0;
                if (button2 != g.p2Btn) {
                    g.p2Btn = button2;
                    handleButton(g, button2, 1, false);
                }
            }
        }
        resetTouchedRingsForTick(g.p1);
        if (g.isDualMode) resetTouchedRingsForTick(g.p2);
        if (g.trig.on) g.trig.spawnedThisTick.clear();
        for (Player* pp : {&g.p1, &g.p2}) {
            Player& p = *pp;
            p.collisionLogTop.clear();
            p.collisionLogBottom.clear();
            p.collisionLogLeft.clear();
            p.collisionLogRight.clear();
            p.unk50C = p.lastCollisionBottom;
            p.unk510 = p.lastCollisionTop;
            p.lastCollisionBottom = -1;
            p.lastCollisionTop = -1;
            p.lastCollisionLeft = -1;
            p.lastCollisionRight = -1;
            if (!g.isDualMode) break;
        }
        if (g.trig.on && !g.playerDied) trigUpdateTimers(g, dSecF);
        if (g.trig.on) {
            trigMoveActions(g, dSec, dt);
            trigAreaActions(g, dSec);
        }
        if (g.spikeOn && !g.spikeVerified) {
            if (g.ignoreDamage) g.spikeVerified = true;
            g.spikeX = g.p1.x;
            g.spikeY = g.p1.y;
        }
        if (g.trig.on) trigCollisionBlocks(g);
        if (!g.playerDied) {
            bool const ignore = g.isEditor || g.ignoreDamage;
            g.p2.maybeReverseSpeed = g.p1.maybeReverseSpeed;
            g.p2.maybeReverseAcceleration = g.p1.maybeReverseAcceleration;
            bool const wasDual = g.isDualMode;
            playerUpdate(g, g.p1, dt);
            if (!g.enable22Changes || !g.p1.isLocked) checkCollisions(g, g.p1, dt, ignore);
            if (wasDual && g.isDualMode) {
                playerUpdate(g, g.p2, dt);
                if (!g.enable22Changes || !g.p2.isLocked) checkCollisions(g, g.p2, dt, ignore);
            }
            if (g.trig.on) trigCheckSpawnObjects(g);
            updateRotation(g, g.p1, dt);
            if (g.isDualMode) updateRotation(g, g.p2, dt);
            if (g.trig.on) trigAfRecord(g);
            g.lastMaxPortalY = getMaxPortalY(g);
            if (g.trig.on) {
                float const warpBefore = g.timeWarp;
                trigEndTick(g, dSec);
                if (g.timeWarp != warpBefore && !g.camKey) {
                    double const ratio = static_cast<double>(std::min(g.timeWarp, 1.f)) / static_cast<double>(std::min(warpBefore, 1.f));
                    g.levelTime += dSec * (ratio - 1.0);
                    g.p1.totalTime = g.levelTime;
                    g.p2.totalTime = g.levelTime;
                }
            }
            checkRepellPlayer(g);
            g.ringPrevCI = g.commandIndex;
            if (g.trig.on) trigStateObjects(g);
            if (!g.activatedObjectIDs.items.empty()) {
                auto& items = g.activatedObjectIDs.items;
                unsigned const commandIndex = g.commandIndex;
                items.erase(std::remove_if(items.begin(), items.end(), [commandIndex](auto const& e) { return e.second < commandIndex; }),
                            items.end());
            }
        }
        if (g.cam.on && !g.playerDied && (!betweenSteps || (g.halfTicks & 1) == 1)) cameraUpdate(g, 0.25f);
        if (!g.died && g.world && g.p1.x >= g.world->length - 300.f / (g.cameraZoom > 0.f ? g.cameraZoom : 1.f))
            g.completed = true;
        if (g.spikeOn && g.spikeVerified && std::fabs(g.p1.x - g.spikeX) > 200.f &&
            (!g.isDualMode || std::fabs(g.p2.x - g.spikeX) > 200.f))
            g.spikeOn = false;
        if (g.parkedSpeed != 0.f) {
            float const speed = g.parkedSpeed;
            bool const noEffects = g.parkedSpeedNoEffects;
            g.parkedSpeed = 0;
            g.parkedSpeedNoEffects = false;
            updateTimeMod(g.p1, speed, noEffects);
            if (g.isDualMode) updateTimeMod(g.p2, speed, noEffects);
        }
        if (!betweenSteps) {
            ++g.tick;
        } else {
            g.halfTicks++;
            if ((g.halfTicks & 1) == 0) ++g.tick;
        }
        if (!g.isPlatformer && (g.tick & 63) == 0 && g.world && g.world->canPruneBehind) pruneBehind(g);
    }

}
