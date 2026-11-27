// Player physics ported from PlayerObject: jumps, gravity, game modes, orbs, pads, dash and resets.

#include "gd.hpp"
#include "camera.hpp"
#include "crtsort.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ap::sim {

    // -1 when the player is upside down, 1 otherwise
    static inline int sgn(Player const& p) { return p.isUpsideDown ? -1 : 1; }

    // sets y velocity, the game only keeps 3 decimals
    void setYVelocity(Player& p, double v) { p.yVelocity = gdRound3(v); }

    // returns true if the player is falling faster than 2x gravity, same as PlayerObject::playerIsFallingBugged
    bool playerIsFallingBugged(Player const& p) {
        if (p.isSideways || p.isPlatformer || p.isSwing || p.fixGravityBug) {
            float const twiceGravity = static_cast<float>(p.gravity + p.gravity);
            if (!p.isUpsideDown) return p.yVelocity < static_cast<double>(twiceGravity);
            return static_cast<double>(-twiceGravity) < p.yVelocity;
        }
        double const gravity = p.gravity;
        double signedGravity = gravity;
        if (p.unkA99) signedGravity = -gravity;
        if (!p.isUpsideDown)
            return p.yVelocity < static_cast<double>(static_cast<float>(signedGravity) + static_cast<float>(signedGravity));
        return gravity + gravity < p.yVelocity;
    }

    // flips the player gravity (flip = upside down), same as PlayerObject::flipGravity
    void flipGravity(Player& p, bool flip, bool noEffects) {
        (void)noEffects;
        if (p.isUpsideDown == flip) return;
        if (g_trace) std::printf("      [gravity] player %d -> upside down %d\n", p.uniqueID, flip ? 1 : 0);
        p.isUpsideDown = flip;
        playerGameEvent(flip ? 10 : 11);
        p.lastFlipTime = p.totalTime;
        p.collidedBottomMaxY = 0;
        p.collidedTopMinY = 0;
        p.unkA29 = false;
        if (p.wasOnSlope || p.isOnSlope) p.slopeFlipGravityRelated = !p.slopeFlipGravityRelated;
        p.collisionLogTop.clear();
        p.collisionLogBottom.clear();
        p.collisionLogLeft.clear();
        p.collisionLogRight.clear();
        p.unk50C = -1;
        p.unk510 = -1;
        p.lastCollisionBottom = -1;
        p.lastCollisionTop = -1;
        p.lastCollisionLeft = -1;
        p.lastCollisionRight = -1;
        if (!p.maybeReducedEffects) p.yVelocity = p.yVelocity * 0.5;
        if (!p.isBall && !p.isDart && !p.maybeReducedEffects) p.shouldTryPlacingCheckpoint = true;
        p.lastGroundedX = p.lastPortalX;
        p.lastGroundedY = p.lastPortalY;
        p.isOnGround = false;
        if (p.isBall) {
            p.isRotating = false;
            p.isBallRotating2 = false;
            p.isBallRotating = false;
            p.rotationSpeed = 0;
            runBallRotation2(p);
        }
    }

    // mirror portal, starts the 0.4s mirror transition (tween 7) unless noEffects
    void toggleFlipped(Game& g, bool flip, bool noEffects) {
        if (g.mirrorOn == flip) return;
        g.mirrorOn = flip;
        float const target = flip ? 1.f : 0.f;
        g.mirrorBack = false;
        if (noEffects) {
            g.mirrorT = target;
            return;
        }
        float& mirror = g.mirrorT;
        if (mirror == 0.f || mirror == 1.f)
            mirror = (target == 1.f) ? 1e-05f : 0.99999f;
        else if (!(g.mirrorOn ? (0.5f <= mirror) : (mirror <= 0.5f)))
            g.mirrorBack = true;
        trigTweenValue(g, 7, mirror, target, 0.4f, 1, 1.2f, -1, -1);
    }

    // flips a player, in dual the other one is flipped the other way if both are in the same mode
    void layerFlipGravity(Game& g, Player& p, bool flip, bool noEffects) {
        if (p.isUpsideDown == flip) return;
        flipGravity(p, flip, noEffects);
        if (g.isDualMode && !g.twoPlayerMode && !g.unlinkDualGravity) {
            Player& p1 = g.p1;
            Player& p2 = g.p2;
            if (p1.isShip == p2.isShip && p1.isBall == p2.isBall && p1.isBird == p2.isBird &&
                p1.isSpider == p2.isSpider && p1.isRobot == p2.isRobot && p1.isSwing == p2.isSwing) {
                Player& other = (p.uniqueID == p1.uniqueID) ? p2 : p1;
                flipGravity(other, !flip, noEffects);
            }
        }
    }

    // speed portal, sets jump speed, gravity and x speed for this speed
    void updateTimeMod(Player& p, float speed, bool noEffects) {
        (void)noEffects;
        p.playerSpeed = speed;
        if (speed == 0.9f) {
            p.yStart = 11.180031776428223;
            p.gravity = 0.9581990242004395;
            p.speedMultiplier = 5.7700018882751465;
        } else if (speed == 0.7f) {
            p.yStart = 10.620032;
            p.gravity = 0.940199;
            p.speedMultiplier = 5.980002;
        } else if (speed == 1.1f) {
            p.yStart = 11.420032;
            p.gravity = 0.957199;
            p.speedMultiplier = 5.870002;
        } else if (speed == 1.3f || speed == 1.6f) {
            p.yStart = 11.230032;
            p.gravity = 0.961199;
            p.speedMultiplier = 6.000002;
        } else {
            p.speedMultiplier = 5.7700018882751465;
        }
        if (p.isBall && !p.isLocked && !p.isDashing) {
            p.isRotating = false;
            p.isBallRotating2 = false;
            p.isBallRotating = false;
            p.rotationSpeed = 0;
            runBallRotation(p, 1.f);
        }
    }

    // sets rotation to 0, the hitbox is updated only if it changed
    static inline void setRotationZero(Player& p) {
        if (p.rotation != 0.f) {
            p.rotation = 0.f;
            playerMoved(p);
        }
        p.rotUnc = 0.f;
        p.rotUncertain = false;
    }

    // sets rotation to r degrees, the hitbox is updated only if it changed
    static inline void setRotationTo(Player& p, float r) {
        if (p.rotation != r) {
            p.rotation = r;
            playerMoved(p);
        }
        p.rotUnc = 0.f;
        p.rotUncertain = false;
    }

    // puts the player back flat after a spider jump (not for ship, ufo, wave, swing, ball, spider)
    static void spiderFlatten(Player& p) {
        if (p.isShip || p.isBird || p.isDart || p.isSwing || p.isBall || p.isSpider) return;
        setRotationTo(p, p.isSideways ? (p.isUpsideDown ? 90.f : 270.f) : (p.isUpsideDown ? 180.f : 0.f));
    }

    // size portal, enable = mini
    void togglePlayerScale(Player& p, bool enable, bool noEffects) {
        (void)noEffects;
        float const size = p.vehicleSize;
        if (size == 1.f && !enable) return;
        if (enable) {
            if (size != 1.f) return;
            p.vehicleSize = 0.6f;
        } else {
            if (p.isPlatformer) p.stateScale = 2;
            p.vehicleSize = 1.f;
        }
        p.spriteWidthScale = p.vehicleSize;
        p.spriteHeightScale = p.vehicleSize;
        if (p.isBall && p.isRotating && !p.isLocked && !p.isDashing) {
            p.isRotating = false;
            p.isBallRotating2 = false;
            p.isBallRotating = false;
            p.rotationSpeed = 0;
            runBallRotation(p, 1.f);
        }
    }

    //resets the hitbox to cube size and restarts the normal rotation
    void resetPlayerIcon(Player& p) {
        p.width = 30.f;
        p.height = 30.f;
        p.hitboxSize = 30.f;
        if (!p.isLocked && !p.isDashing) {
            p.isRotating = false;
            p.isBallRotating2 = false;
            p.isBallRotating = false;
            p.rotationSpeed = 0;
            if (!p.isBall)
                runNormalRotation(p, false, 1.f);
            else
                runBallRotation(p, 1.f);
        }
    }

    // leaves every mode except the one of portal type
    void switchedToMode(Player& p, int type) {
        if (type != ShipPortal) toggleFlyMode(p, false, false);
        if (type != UfoPortal) toggleBirdMode(p, false, false);
        if (type != BallPortal) toggleRollMode(p, false, false);
        if (type != WavePortal) toggleDartMode(p, false, false);
        if (type != RobotPortal) toggleRobotMode(p, false, false);
        if (type != SpiderPortal) toggleSpiderMode(p, false, false);
        if (type != SwingPortal) toggleSwingMode(p, false, false);
    }

    // enters or leaves ship mode, returns true if the mode changed
    bool toggleFlyMode(Player& p, bool enable, bool noEffects) {
        (void)noEffects;
        if (p.isShip == enable) return false;
        p.gameModeChangedTime = p.totalTime;
        p.isShip = enable;
        if (enable) switchedToMode(p, 5);
        p.isRotating = false;
        p.isBallRotating2 = false;
        p.isBallRotating = false;
        p.rotationSpeed = 0;
        p.yVelocity = p.yVelocity * 0.5;
        setRotationZero(p);
        p.isOnGround2 = false;
        p.isOnGround = false;
        p.shouldTryPlacingCheckpoint = false;
        if (!p.isShip) resetPlayerIcon(p);
        return true;
    }

    // enters or leaves ball mode, returns true if the mode changed
    bool toggleRollMode(Player& p, bool enable, bool noEffects) {
        (void)noEffects;
        if (p.isBall == enable) return false;
        p.gameModeChangedTime = p.totalTime;
        p.isBall = enable;
        if (enable) switchedToMode(p, 0x10);
        if (!p.isBall) setRotationTo(p, p.isUpsideDown ? 180.f : 0.f);
        p.isRotating = false;
        p.isBallRotating2 = false;
        p.isBallRotating = false;
        p.rotationSpeed = 0;
        return true;
    }

    // enters or leaves ufo mode, returns true if the mode changed
    bool toggleBirdMode(Player& p, bool enable, bool noEffects) {
        (void)noEffects;
        if (p.isBird == enable) return false;
        p.gameModeChangedTime = p.totalTime;
        p.isBird = enable;
        if (enable) switchedToMode(p, 0x13);
        p.isRotating = false;
        p.isBallRotating2 = false;
        p.isBallRotating = false;
        p.rotationSpeed = 0;
        p.yVelocity = p.yVelocity * 0.5;
        setRotationZero(p);
        p.isOnGround2 = false;
        p.isOnGround = false;
        p.shouldTryPlacingCheckpoint = false;
        if (!p.isBird) resetPlayerIcon(p);
        return true;
    }

    //enters or leaves wave mode (smaller hitbox), returns true if the mode changed
    bool toggleDartMode(Player& p, bool enable, bool noEffects) {
        (void)noEffects;
        if (p.isDart == enable) return false;
        p.gameModeChangedTime = p.totalTime;
        p.isDart = enable;
        if (enable) switchedToMode(p, 0x1a);
        p.isRotating = false;
        p.isBallRotating2 = false;
        p.isBallRotating = false;
        p.rotationSpeed = 0;
        p.yVelocity = p.yVelocity * 0.5;
        setRotationZero(p);
        p.isOnGround2 = false;
        p.isOnGround = false;
        p.shouldTryPlacingCheckpoint = false;
        if (!p.isDart) {
            resetPlayerIcon(p);
        } else {
            p.hitboxSize = 20.f;
            p.width = 10.f;
            p.height = 10.f;
        }
        return true;
    }

    // enters or leaves robot mode, returns true if the mode changed
    bool toggleRobotMode(Player& p, bool enable, bool noEffects) {
        (void)noEffects;
        if (p.isRobot == enable) return false;
        p.gameModeChangedTime = p.totalTime;
        p.isRobot = enable;
        if (enable) {
            switchedToMode(p, 0x1b);
            p.accelerationOrSpeed = 1.5;
            p.isRotating = false;
            p.isBallRotating2 = false;
            p.isBallRotating = false;
            p.rotationSpeed = 0;
            setRotationZero(p);
        }
        p.isRotating = false;
        p.isBallRotating2 = false;
        p.isBallRotating = false;
        p.rotationSpeed = 0;
        return true;
    }

    // enters or leaves spider mode, returns true if the mode changed
    bool toggleSpiderMode(Player& p, bool enable, bool noEffects) {
        (void)noEffects;
        if (p.isSpider == enable) return false;
        p.gameModeChangedTime = p.totalTime;
        p.isSpider = enable;
        if (enable) {
            switchedToMode(p, 0x21);
            p.hitboxSize = 27.f;
            p.width = 27.f;
            p.height = 27.f;
            p.accelerationOrSpeed = 1.5;
            p.isRotating = false;
            p.isBallRotating2 = false;
            p.isBallRotating = false;
            p.rotationSpeed = 0;
            setRotationZero(p);
        } else {
            resetPlayerIcon(p);
            p.isRotating = false;
            p.isBallRotating2 = false;
            p.isBallRotating = false;
            p.rotationSpeed = 0;
        }
        return true;
    }

    // enters or leaves swing mode, returns true if the mode changed
    bool toggleSwingMode(Player& p, bool enable, bool noEffects) {
        (void)noEffects;
        if (p.isSwing == enable) return false;
        p.gameModeChangedTime = p.totalTime;
        p.isSwing = enable;
        if (enable) switchedToMode(p, 0x29);
        p.isRotating = false;
        p.isBallRotating2 = false;
        p.isBallRotating = false;
        p.rotationSpeed = 0;
        p.yVelocity = p.yVelocity * 0.5;
        setRotationZero(p);
        p.isOnGround2 = false;
        p.isOnGround = false;
        p.shouldTryPlacingCheckpoint = false;
        if (!p.isSwing) {
            resetPlayerIcon(p);
            setRotationTo(p, p.isUpsideDown ? 180.f : 0.f);
        }
        return true;
    }

    // pad push, yVelocity is the pad strength
    void propellPlayer(Player& p, float yVelocity, bool noEffects, int objectType) {
        (void)noEffects;
        (void)objectType;
        p.maybeIsBoosted = true;
        p.isOnGround2 = false;
        p.isOnGround = false;
        p.shouldTryPlacingCheckpoint = true;
        p.isOnSlope = false;
        p.wasOnSlope = false;
        float const sizeScale = (p.vehicleSize != 1.f) ? 0.8f : 1.f;
        setYVelocity(p, static_cast<double>(static_cast<float>(sgn(p)) * yVelocity * 16.f * sizeScale));
        if (p.isBall || p.isSpider || p.isSwing) p.yVelocity = p.yVelocity * 0.6000000238418579;
        if (!p.isLocked && !p.isDashing) {
            p.isRotating = false;
            p.isBallRotating2 = false;
            p.isBallRotating = false;
            p.rotationSpeed = 0;
            if (!p.isBall)
                runNormalRotation(p, false, 1.f);
            else
                runBallRotation(p, 1.f);
        }
        p.lastGroundedX = p.x;
        p.lastGroundedY = p.y;
    }

    // applies a pad to the player, spider pads (0x2c) teleport like a spider jump
    void bumpPlayer(Game& g, Player& p, float bumpMod, int objectType, bool noEffects, GObj object) {
        if (p.isPlatformer || !p.fixRobotJump) p.touchedPad = true;
        if (objectType != 0x2c) {
            propellPlayer(p, bumpMod, noEffects, objectType);
            p.isAccelerating = false;
            if (objectType == 0x22) {
                p.isAccelerating = true;
                p.lastGroundedX = 0;
                p.lastGroundedY = 0;
            }
            return;
        }
        if (object) {
            bool const upsideDown = p.isUpsideDown;
            if (g_trace)
                std::printf("      [spider pad] player %d (p1=%d) upside down %d, pad uid %d downward %d\n", p.uniqueID,
                            &p == &g.p1 ? 1 : 0, upsideDown ? 1 : 0, object.uid(), object.isFacingDown() ? 1 : 0);
            if ((p.isSideways ? object.isFacingLeft() : object.isFacingDown()) != upsideDown)
                flipGravity(p, !upsideDown, true);
        }
        spiderTestJumpInternal(g, p, false);
        trigGameEvent(g, GameEvent::SpiderTeleport);
        spiderFlatten(p);
    }

    // player touched a pad, picks its strength for the current mode
    void layerBumpPlayer(Game& g, Player& p, GObj object) {
        if (!canBeActivatedByPlayer(g, p, object)) return;
        Point const pos = object.getPosition();
        p.lastPortalX = pos.x;
        p.lastPortalY = pos.y - 10.f;
        activatedByPlayer(p, object);
        int const type = object.getType();
        float bumpMod = 1.f;
        if (type == PinkJumpPad) {
            if (p.isShip)
                bumpMod = 0.35f;
            else if (p.isBird)
                bumpMod = 0.4f;
            else if (p.isBall)
                bumpMod = 0.7f;
            else if (p.isSpider)
                bumpMod = 0.7f;
            else
                bumpMod = 0.65f;
        } else if (type == RedJumpPad) {
            if (p.isShip)
                bumpMod = (1.f <= p.vehicleSize) ? 0.63f : 0.95f;
            else if (p.isBird)
                bumpMod = (1.f <= p.vehicleSize) ? 0.6f : 0.98f;
            else
                bumpMod = 1.25f;
        }
        p.lastActivatedPortal = object.uid();
        if (object.o->reverse) reversePlayer(g, p, object);
        bumpPlayer(g, p, bumpMod, type, object.o->hasNoEffects, object);
        trigGameEvent(g, objectTypeToGameEvent(type));
        trigGameEvent(g, GameEvent::PadActivated);
    }

    // gives the player a y velocity when leaving a slope, the cube starts a half turn
    void boostPlayer(Player& p, float yVelocity) {
        p.maybeIsBoosted = true;
        p.isOnGround2 = false;
        p.isOnGround = false;
        p.shouldTryPlacingCheckpoint = true;
        p.isAccelerating = true;
        p.yVelocity = gdRound3(static_cast<double>(yVelocity));
        if (!p.isDashing) {
            if (!p.isShip && !p.isBird && !p.isDart && !p.isSwing && !p.isBall && !p.isRobot && !p.isSpider) {
                p.isBallRotating2 = false;
                p.isBallRotating = false;
                float const rotDuration = (p.vehicleSize == 1.f) ? 0.8666667f : 0.6666667f;
                int const halfTurn = p.isUpsideDown ? 0xb4 : -0xb4;
                p.rotationSpeed = static_cast<float>(halfTurn) / rotDuration;
                p.isRotating = true;
            }
        }
        p.lastGroundedX = p.x;
        p.lastGroundedY = p.y;
    }

    // player takes an orb (yellow, pink, blue, green, black, red, custom, dash, spider, teleport)
    void ringJump(Game& g, Player& p, GObj object, bool skipCheck) {
        (void)skipCheck;
        if (p.isDead || !object) return;
        int const uid = object.uid();
        if (p.ringRelatedSet.count(uid)) return;
        bool const noEffects = object.o->hasNoEffects;
        int const type = object.getType();
        bool const isPlainRing = !(type == CustomRing || type == TeleportOrb);
        if (!p.stateRingJump2) return;
        if (p.isDashing) return;
        if (!p.stateJumpBuffered) return;
        if ((p.touchedRing || !isPlainRing) && (p.touchedCustomRing || type != CustomRing)) {
            if (p.touchedGravityPortal) return;
            if (type != TeleportOrb) return;
        }
        if (object.o->reverse) reversePlayer(g, p, object);
        p.ringJumpRelated = true;
        p.ringRelatedSet.insert(uid);
        trigGameEvent(g, GameEvent::OrbActivated);
        trigGameEvent(g, objectTypeToGameEvent(type));
        if (type == CustomRing)
            p.touchedCustomRing = true;
        else if (type == TeleportOrb)
            p.touchedGravityPortal = true;
        else
            p.touchedRing = true;
        p.touchingRings.erase(uid);
        if (isPlainRing) p.padRingRelated = true;

        if (type == CustomRing) {
            trigCustomRing(g, p, object.index);
            if (p.enable22Changes) p.stateRingJump = false;
        } else if (type == TeleportOrb) {
            teleportPlayer(g, object, p);
            p.stateRingJump = false;
        } else if (type == SpiderOrb) {
            bool const wasUpsideDown = p.isUpsideDown;
            bool flip = false;
            if ((p.isSideways ? object.isFacingLeft() : object.isFacingDown()) != wasUpsideDown) {
                flip = !wasUpsideDown;
                flipGravity(p, flip, true);
            }
            spiderTestJump(g, p, flip);
        } else if (type == DashRing) {
            startDashing(g, p, object);
        } else if (type == GravityDashRing) {
            layerFlipGravity(g, p, !p.isUpsideDown, true);
            startDashing(g, p, object);
        } else {
            p.stateRingJump = false;
            if (type == DropRing) {
                float dropVel = static_cast<float>(sgn(p)) * -15.f;
                if (!p.isShip && !p.isBird && !p.isDart && !p.isSwing) {
                    if (!p.isRobot && p.isSpider) dropVel = dropVel * 1.1f;
                } else {
                    dropVel = static_cast<float>(sgn(p)) * -14.f;
                    if (p.isBird) dropVel = dropVel * 0.8f;
                }
                setYVelocity(p, static_cast<double>(dropVel));
                if (!p.isBall) {
                    if (!p.isLocked && !p.isDashing) {
                        p.isRotating = false;
                        p.isBallRotating2 = false;
                        p.isBallRotating = false;
                        p.rotationSpeed = 0;
                        runNormalRotation(p, false, 1.f);
                    }
                } else {
                    runBallRotation2(p);
                }
                p.hasEverHitRing = true;
                p.isAccelerating = true;
                if (p.isBall || p.isSwing) p.jumpBuffered = false;
            } else {
                p.maybeIsBoosted = true;
                p.isOnGround2 = false;
                p.isOnGround = false;
                float const sizeScale = (p.vehicleSize != 1.f) ? 0.8f : 1.f;
                float jumpSpeed = static_cast<float>(p.yStart);
                if (type == GravityRing)
                    jumpSpeed = jumpSpeed * 0.8f;
                else if (type == GreenRing) {
                    if (p.isShip) jumpSpeed = jumpSpeed * 0.7f;
                } else if (type == PinkJumpRing) {
                    if (p.isShip)
                        jumpSpeed = jumpSpeed * 0.37f;
                    else if (p.isBird)
                        jumpSpeed = jumpSpeed * 0.42f;
                    else if (p.isBall)
                        jumpSpeed = jumpSpeed * 0.77f;
                    else
                        jumpSpeed = jumpSpeed * 0.72f;
                } else if (type == RedJumpRing) {
                    if (p.isShip) {
                        if (p.vehicleSize != 1.f) jumpSpeed = jumpSpeed * 1.4f;
                    } else if (p.isBird)
                        jumpSpeed = (p.vehicleSize == 1.f) ? jumpSpeed * 1.02f : jumpSpeed * 1.36f;
                    else if (p.isBall)
                        jumpSpeed = jumpSpeed * 1.34f;
                    else if (p.isRobot)
                        jumpSpeed = jumpSpeed * 1.28f;
                    else if (p.isSpider)
                        jumpSpeed = jumpSpeed * 1.34f;
                    else
                        jumpSpeed = jumpSpeed * 1.38f;
                } else if (p.isRobot) {
                    jumpSpeed = jumpSpeed * 0.9f;
                }
                if (type == GreenRing) layerFlipGravity(g, p, !p.isUpsideDown, true);
                setYVelocity(p, static_cast<double>(static_cast<float>(sgn(p)) * jumpSpeed * sizeScale));
                if (!p.isBall) {
                    if (!p.isLocked && !p.isDashing) {
                        p.isRotating = false;
                        p.isBallRotating2 = false;
                        p.isBallRotating = false;
                        p.rotationSpeed = 0;
                        runNormalRotation(p, false, 1.f);
                    }
                } else {
                    runBallRotation2(p);
                }
                p.lastGroundedX = p.x;
                p.lastGroundedY = p.y;
                p.hasEverHitRing = true;
                if (p.isBall || p.isSpider) {
                    p.yVelocity = p.yVelocity * 0.699999988079071;
                    p.jumpBuffered = false;
                } else if (p.isSwing) {
                    p.yVelocity = p.yVelocity * 0.6000000238418579;
                    p.jumpBuffered = false;
                }
                if (type == GravityRing) layerFlipGravity(g, p, !p.isUpsideDown, true);
                if (type == RedJumpRing) p.isAccelerating = true;
            }
        }
        (void)noEffects;
        activatedByPlayer(p, object);
        incrementJumps(p);
    }

    // the game counts jumps here, only the "has jumped" flag is kept
    void incrementJumps(Player& p) { p.hasEverJumped = true; }

    // called every tick the player touches an orb, jumps right away if the button is held
    void playerTouchedRing(Game& g, Player& p, GObj object) {
        int const uid = object.uid();
        if (object.st().lastTouchedCommandIndex < static_cast<int>(g.ringPrevCI))
            trigGameEvent(g, GameEvent::OrbTouched);
        p.touchingRings.insert(uid);
        p.touchedRings.insert(uid);
        object.mut().lastTouchedCommandIndex = static_cast<int>(g.commandIndex);
        if (!p.isShip && !p.isBird && !p.isDart && !p.isSwing && !object.o->claimTouch) ringJump(g, p, object, false);
    }

    // keeps only the orbs touched during the last collision pass
    static void resetTouchedRings(Player& p) {
        RingList kept;
        for (int uid : p.touchingRings)
            if (p.touchedRings.count(uid)) kept.push_back(uid);
        p.touchingRings = kept;
        p.touchedRings.clear();
    }

    // player presses a button (1 jump, 2 and 3 left and right), returns false if controls are locked
    bool pushButton(Game& g, Player& p, int button) {
        if (!p.inputsLocked) {
            if (button == 1) p.holdingJump = true;
        }
        if (p.isLocked || p.controlsDisabled) return false;
        if (button == 1) {
            p.jumpBuffered = true;
            p.stateRingJump = true;
            p.padRingRelated = false;
            p.ringRelatedSet.clear();
            p.stateJumpBuffered = p.jumpBuffered ? 1 : 0;
            p.stateRingJump2 = p.stateRingJump;
            p.touchedRing = false;
            p.touchedCustomRing = false;
            p.touchedGravityPortal = false;
            p.maybeTouchedBreakableBlock = false;
            if (p.touchingRings.empty()) {
                if ((p.isBall || (!p.isShip && !p.isBird && !p.isDart && !p.isSwing && p.jumpBuffered)) && p.isOnGround)
                    updateJump(g, p, 0.f);
            } else {
                RingList custom;
                RingList others;
                RingList const rings = p.touchingRings;
                for (int uid : rings) {
                    GObj ring = g.objByUid(uid);
                    if (!ring) continue;
                    if (ring.o->claimTouch) {
                        ringJump(g, p, ring, false);
                        p.jumpBuffered = false;
                        return true;
                    }
                    if (ring.getType() == CustomRing)
                        custom.push_back(uid);
                    else
                        others.push_back(uid);
                }
                for (int uid : custom) ringJump(g, p, g.objByUid(uid), false);
                for (int uid : others) ringJump(g, p, g.objByUid(uid), false);
            }
        }
        return true;
    }

    // sets the player direction (left = true), the ball restarts its rotation
    void doReversePlayer(Game& g, Player& p, bool left) {
        (void)g;
        bool const wasLeft = p.isGoingLeft;
        p.isGoingLeft = left;
        if (wasLeft != left) p.reverseRelated = 2;
        if (p.isBall && !p.isLocked && !p.isDashing) {
            p.isRotating = false;
            p.isBallRotating2 = false;
            p.isBallRotating = false;
            p.rotationSpeed = 0;
            runBallRotation(p, 1.f);
        }
    }

    // rotate gameplay trigger: turns the axes of the player
    void playerRotateGameplay(Game& g, Player& p, int moveDir, int groundDir, bool editVel, float modX, float modY, bool overrideVel, bool dontSlide) {
        double const yVelocity = p.yVelocity;
        double const speed = static_cast<double>(p.playerSpeed) * p.speedMultiplier;
        bool const wasUpside = p.isUpsideDown;
        bool const wasSide = p.isSideways;
        bool const wasLeft = p.isGoingLeft;
        bool const sideways = static_cast<unsigned>(moveDir - 3) < 2u;
        p.isSideways = sideways;
        flipGravity(p, moveDir == 1 || moveDir == 4, true);
        doReversePlayer(g, p, p.isPlatformer ? p.isGoingLeft : (groundDir == 3 || groundDir == 2));
        if (sideways == wasSide) {
            if (wasUpside == p.isUpsideDown && wasLeft == p.isGoingLeft) return;
        } else {
            p.collisionLogTop.clear();
            p.collisionLogBottom.clear();
            p.collisionLogLeft.clear();
            p.collisionLogRight.clear();
            p.lastCollisionBottom = -1;
            p.lastCollisionTop = -1;
            p.lastCollisionLeft = -1;
            p.lastCollisionRight = -1;
            p.unk50C = -1;
            p.unk510 = -1;
            float vx = static_cast<float>(yVelocity);
            float vy = static_cast<float>(speed);
            if (editVel) {
                if (!overrideVel) {
                    vx = vx * modX;
                    vy = vy * modY;
                } else {
                    vx = modX;
                    vy = modY;
                }
            }
            p.isAccelerating = true;
            p.yVelocity = static_cast<double>(vy);
            if (p.isPlatformer) p.affectedByForces = true;
            p.maybeIsBoosted = true;
            p.isOnGround2 = false;
            p.lastGroundedX = 0;
            p.lastGroundedY = 0;
            if (dontSlide) {
                p.isAccelerating = false;
                p.affectedByForces = false;
            }
            bool const plainCube = !p.isShip && !p.isBird && !p.isDart && !p.isSwing && !p.isBall && !p.isRobot && !p.isSpider && !p.isRotating && !p.isLocked;
            if (plainCube && !p.isDashing) {
                p.isRotating = false;
                p.isBallRotating2 = false;
                p.isBallRotating = false;
                p.rotationSpeed = 0;
                runNormalRotation(p, false, 1.f);
            }
            if (p.isDashing) {
                double const oldDashX = p.dashX;
                p.dashX = p.dashY;
                p.dashY = static_cast<double>(static_cast<float>(oldDashX));
            }
        }
        if (!p.isDashing) return;
        if (wasLeft != p.isGoingLeft) {
            if (sideways != wasSide) p.dashAngle += 180.0;
            p.dashAngle = (p.dashAngle <= 0.0 ? -180.0 : 180.0) - p.dashAngle;
        }
        if (p.dashAngle > 180.0)
            p.dashAngle -= 360.0;
        else if (p.dashAngle < -180.0)
            p.dashAngle += 360.0;
        updateDashArt(g, p);
    }

    // reverse orb, pad or portal, with reverse sync the gap to the object is caught up later
    void reversePlayer(Game& g, Player& p, GObj object) {
        if (p.isPlatformer) return;
        if (object && p.reverseSync) {
            float const syncX = p.x + static_cast<float>(p.maybeReverseSpeed);
            float const syncY = p.y + static_cast<float>(p.maybeReverseSpeed);
            Point const objectPos = object.getRealPosition();
            float const gap = p.isSideways ? objectPos.y - syncY : objectPos.x - syncX;
            p.maybeReverseSpeed = static_cast<double>(gap + gap) + p.maybeReverseSpeed;
        }
        doReversePlayer(g, p, !p.isGoingLeft);
    }

    // player control trigger, releases everything then locks the controls
    void disablePlayerControls(Game& g, Player& p) {
        p.controlsDisabled = false;
        p.inputsLocked = true;
        p.jumpBuffered = false;
        p.stateRingJump = false;
        p.touchedPad = true;
        if (p.isDashing) stopDashing(g, p);
        releaseButton(g, p, 2);
        releaseButton(g, p, 3);
        p.inputsLocked = false;
        p.controlsDisabled = true;
    }

    // unlocks the controls, presses jump again if it's still held
    void enablePlayerControls(Game& g, Player& p) {
        p.controlsDisabled = false;
        if (p.holdingJump) pushButton(g, p, 1);
    }

    // releases every button and stops the dash
    void releaseAllButtons(Game& g, Player& p) {
        if (!p.inputsLocked) p.holdingJump = false;
        if (!p.controlsDisabled) {
            p.jumpBuffered = false;
            p.stateRingJump = false;
            p.touchedPad = true;
            if (p.isDashing) stopDashing(g, p);
        }
    }

    // player releases a button (1 jump, 2 left, 3 right, 5 left and right)
    bool releaseButton(Game& g, Player& p, int button) {
        if (button == 5) {
            releaseButton(g, p, 2);
            releaseButton(g, p, 3);
            return !p.controlsDisabled;
        }
        if (!p.inputsLocked) {
            if (button == 1) p.holdingJump = false;
        }
        if (p.controlsDisabled) return false;
        if (button == 1) {
            p.jumpBuffered = false;
            p.stateRingJump = false;
            p.touchedPad = true;
            if (p.isDashing) {
                stopDashing(g, p);
                return true;
            }
        }
        return true;
    }

    // y velocity for one tick depending on the mode, same as PlayerObject::updateJump
    void updateJump(Game& g, Player& self, float dt) {
        float factor = static_cast<float>(self.gravity);
        bool wantsJump = self.jumpBuffered;
        if (self.isRobot) wantsJump = self.stateRingJump && self.jumpBuffered;
        if (self.isBall || self.isShip || self.isBird || self.isDart || self.isSwing || self.isSpider)
            factor = 0.958199f;
        float gravityAccel = factor;
        if (self.gravityMod != 1.f) gravityAccel = factor * self.gravityMod;
        float capScale = 0.8f;
        float fallScale = 0.8f;
        float sizeScale = 1.f;
        if (self.vehicleSize != 1.f) sizeScale = fallScale;
        if (self.isShip || self.isBird || self.isDart || self.isSwing) {
            if (self.vehicleSize != 1.f) sizeScale = 0.85f;
            {
                double const yVel = self.yVelocity;
                if (!self.isUpsideDown) {
                    if ((0.0 <= yVel && yVel < static_cast<double>(8.f / sizeScale)) ||
                        (yVel <= 0.0 && static_cast<double>(-6.4f / sizeScale) < yVel))
                        self.isAccelerating = false;
                } else {
                    if ((yVel <= 0.0 && static_cast<double>(-(8.f / sizeScale)) < yVel) ||
                        (0.0 <= yVel && yVel < static_cast<double>(-(-6.4f / sizeScale))))
                        self.isAccelerating = false;
                }
            }
            if (!self.isShip) {
                if (!self.isBird) {
                    if (!self.isSwing) {
                        int const gravSign = sgn(self);
                        int const holdDir = self.jumpBuffered ? 1 : -1;
                        setYVelocity(self, static_cast<double>(static_cast<float>(gravSign) * 8.f * static_cast<float>(holdDir)));
                        setYVelocity(self, static_cast<double>(self.playerSpeed) * self.speedMultiplier *
                                                   static_cast<double>(gravSign) * static_cast<double>(holdDir));
                        if (self.wasJumpBuffered != self.jumpBuffered)
                            trigGameEvent(g, self.jumpBuffered ? GameEvent::WavePush : GameEvent::WaveRelease);
                        goto clampFlyVelocity;
                    }
                    if (self.stateRingJump && self.jumpBuffered) {
                        self.stateRingJump = false;
                        double const yVel = self.yVelocity;
                        flipGravity(self, !self.isUpsideDown, true);
                        setYVelocity(self, static_cast<double>(static_cast<float>(yVel) * 0.8f));
                        trigGameEvent(g, GameEvent::SwingSwitch);
                    }
                    sizeScale = 1.f;
                    capScale = 1.f;
                    factor = (self.vehicleSize != 1.f) ? 0.6f : 0.4f;
                    factor = static_cast<float>(sgn(self)) * dt * gravityAccel * factor;
                } else {
                    if (self.stateRingJump && self.jumpBuffered) {
                        self.stateRingJump = false;
                        factor = (self.vehicleSize != 1.f) ? 8.f : 7.f;
                        double yVel;
                        bool atTarget;
                        bool pastTarget;
                        if (!self.isUpsideDown) {
                            yVel = static_cast<double>(sizeScale * factor);
                            atTarget = yVel == self.yVelocity;
                            pastTarget = yVel < self.yVelocity;
                        } else {
                            yVel = static_cast<double>(factor * -1.f * sizeScale);
                            atTarget = self.yVelocity == yVel;
                            pastTarget = self.yVelocity < yVel;
                        }
                        if (!pastTarget && !atTarget) {
                            setYVelocity(self, yVel);
                            if ((self.wasOnSlope || self.isOnSlope) && 0.f < self.slopeVelocity) {
                                double const flapVelocity = self.yVelocity;
                                setYVelocity(self, static_cast<double>(self.slopeVelocity * 0.5f) + flapVelocity);
                                double const maxVel =
                                        static_cast<double>(static_cast<float>(flapVelocity * 1.399999976158142));
                                double cappedVel = self.yVelocity;
                                if (maxVel <= self.yVelocity) cappedVel = maxVel;
                                setYVelocity(self, cappedVel);
                            }
                        }
                        trigGameEvent(g, GameEvent::UFOJump);
                    }
                    if (!playerIsFallingBugged(self)) fallScale = 1.2f;
                    factor = (static_cast<float>(sgn(self)) * dt * gravityAccel * fallScale * 0.5f) / sizeScale;
                }
                setYVelocity(self, static_cast<double>(-factor) + self.yVelocity);
                if (self.jumpBuffered) self.isOnGround2 = false;
            } else {
                bool holding;
                bool toAccelBranch = false;
                bool toFallBranch = false;
                if (!self.jumpBuffered) {
                    if (self.isAccelerating)
                        toAccelBranch = true;
                    else
                        toFallBranch = true;
                }
                if (!toAccelBranch && !toFallBranch) {
                    if (!self.isAccelerating) {
                        fallScale = -1.f;
                        holding = self.jumpBuffered;
                    } else
                        toAccelBranch = true;
                }
                if (toAccelBranch) {
                    if (!self.isUpsideDown) {
                        if (self.yVelocity < 0.0) fallScale = -1.f;
                    } else if (0.0 < self.yVelocity)
                        fallScale = -1.f;
                    toFallBranch = true;
                }
                if (toFallBranch) {
                    holding = self.jumpBuffered;
                    if (!holding && !playerIsFallingBugged(self)) fallScale = 1.2f;
                }
                if (self.isPlatformer) gravityAccel = gravityAccel * 0.8f;
                if (0.f <= fallScale) factor = gravityAccel;
                gravityAccel = 0.4f;
                if (holding && playerIsFallingBugged(self)) gravityAccel = 0.5f;
                setYVelocity(self, static_cast<double>(-((static_cast<float>(sgn(self)) * dt * factor * fallScale * gravityAccel) / sizeScale)) + self.yVelocity);
                holding = self.jumpBuffered;
                if (holding) self.isOnGround2 = false;
                if (self.wasJumpBuffered != holding)
                    trigGameEvent(g, holding ? GameEvent::ShipBoostStart : GameEvent::ShipBoostEnd);
            }
        clampFlyVelocity:
            if (!self.isAccelerating && !self.isDart) {
                double yVel = self.yVelocity;
                if (!self.isUpsideDown) {
                    double const minVel = static_cast<double>((capScale * -8.f) / sizeScale);
                    if (yVel <= minVel) yVel = minVel;
                    setYVelocity(self, yVel);
                    sizeScale = 8.f / sizeScale;
                } else {
                    if (yVel <= static_cast<double>(-8.f / sizeScale)) yVel = static_cast<double>(-8.f / sizeScale);
                    setYVelocity(self, yVel);
                    sizeScale = (capScale * 8.f) / sizeScale;
                }
                yVel = self.yVelocity;
                if (static_cast<double>(sizeScale) <= self.yVelocity) yVel = static_cast<double>(sizeScale);
                setYVelocity(self, yVel);
            }
            if (playerIsFallingBugged(self)) self.maybeIsBoosted = false;
            goto finish;
        }
        factor = 1.f;
        if (self.isBall)
            factor = 0.6f;
        else if (self.isSpider)
            factor = 0.6f;
        else if (self.isRobot)
            factor = 0.9f;
        {
            bool const notPlatformer = true;
            bool const noFlip = false;
            if (wantsJump && self.isOnGround) {
                if (!self.isSpider) {
                    if (!self.isDashing && (!self.isPlatformer || notPlatformer)) {
                        self.maybeIsBoosted = true;
                        self.isOnGround2 = false;
                        self.isOnGround = false;
                        self.stateRingJump = false;
                        self.touchedPad = false;
                        self.accelerationOrSpeed = 0;
                        factor = static_cast<float>(self.yStart);
                        if (self.isRobot) factor = factor * 0.5f;
                        setYVelocity(self, static_cast<double>(static_cast<float>(sgn(self)) * factor * sizeScale));
                        if (self.wasOnSlope || self.isOnSlope) {
                            factor = self.slopeVelocity;
                            if (0.f < static_cast<float>(sgn(self)) * factor) {
                                double const yVel = self.yVelocity;
                                double sinceSlope = self.totalTime - self.slopeStartTime;
                                double rampOrCap = 1.0;
                                if (sinceSlope < 0.10000000149011612) {
                                    sinceSlope = sinceSlope * 10.0;
                                    rampOrCap = 0.4000000059604645;
                                    if (0.4000000059604645 <= sinceSlope) rampOrCap = sinceSlope;
                                }
                                float slopeBoost = static_cast<float>(static_cast<double>(factor) * rampOrCap);
                                if (self.isBall) slopeBoost = factor;
                                setYVelocity(self, static_cast<double>(slopeBoost * 0.25f) + yVel);
                                rampOrCap = static_cast<double>(static_cast<float>(yVel * 1.399999976158142));
                                double velocity = self.yVelocity;
                                if (!self.isUpsideDown) {
                                    if (rampOrCap <= velocity) velocity = rampOrCap;
                                } else if (velocity <= rampOrCap)
                                    velocity = rampOrCap;
                                setYVelocity(self, velocity);
                            }
                        }
                        self.shouldTryPlacingCheckpoint = true;
                        self.lastJumpTime = self.totalTime;
                        incrementJumps(self);
                        if (!self.isBall) {
                            if (!self.isRobot) {
                                if (!self.isSpider && !self.isLocked && !self.isDashing) {
                                    self.isRotating = false;
                                    self.isBallRotating2 = false;
                                    self.isBallRotating = false;
                                    self.rotationSpeed = 0;
                                    runNormalRotation(self, false, 1.f);
                                }
                            }
                        } else {
                            flipGravity(self, !self.isUpsideDown, true);
                            self.yVelocity = self.yVelocity * 0.6000000238418579;
                            self.isOnGround3 = false;
                            self.jumpBuffered = false;
                        }
                        if (!self.isShip && !self.isBird && !self.isDart && !self.isSwing && !self.isBall)
                            trigGameEvent(g, self.isRobot ? GameEvent::RobotBoostStart : GameEvent::NormalJump);
                        else if (self.isBall && !self.isRobot)
                            trigGameEvent(g, GameEvent::BallSwitch);
                        goto finish;
                    }
                } else if (!self.isDashing) {
                    spiderTestJump(g, self, noFlip);
                    goto finish;
                }
            }
        }
        if (self.maybeIsBoosted) {
            if (self.isRobot && self.jumpBuffered && !self.touchedPad && self.accelerationOrSpeed < 1.5) {
                self.accelerationOrSpeed = static_cast<double>(dt * 0.1f) + self.accelerationOrSpeed;
                setYVelocity(self, static_cast<double>(static_cast<float>(sgn(self)) * dt * gravityAccel * factor) + self.yVelocity);
            }
            setYVelocity(self, self.yVelocity -
                                       static_cast<double>(static_cast<float>(sgn(self)) * dt * gravityAccel * factor));
            if (self.isRobot && self.touchedPad && !self.wasRobotJump) trigGameEvent(g, GameEvent::RobotBoostStop);
            if (!playerIsFallingBugged(self)) {
                if (!self.isPlatformer) goto finish;
                double const yVel = self.yVelocity;
                bool const isFalling = !self.isUpsideDown ? (yVel < 0.0) : (0.0 < yVel);
                if (!isFalling && yVel != 0.0) goto finish;
            }
            self.maybeIsBoosted = false;
            self.maybeIsFalling = true;
            self.isOnGround2 = false;
            self.fallStartY = self.y;
            if (self.isRobot && !self.touchedPad) trigGameEvent(g, GameEvent::RobotBoostStop);
            goto finish;
        }
        if (self.isOnGround) self.fallStartY = self.y;
        if (playerIsFallingBugged(self)) self.isOnGround = false;
        setYVelocity(self,
                     self.yVelocity - static_cast<double>(static_cast<float>(sgn(self)) * dt * gravityAccel * factor));
        {
            double yVel = self.yVelocity;
            if (!self.isUpsideDown) {
                if (yVel <= -15.0) yVel = -15.0;
            } else if (15.0 <= yVel)
                yVel = 15.0;
            self.yVelocity = gdRound3(yVel);
            yVel = self.yVelocity;
            bool atThreshold;
            bool aboveThreshold;
            if (!self.isUpsideDown) {
                atThreshold = yVel == -0.25;
                aboveThreshold = -0.25 < yVel;
            } else {
                atThreshold = yVel == 0.25;
                aboveThreshold = yVel < 0.25;
            }
            if (!aboveThreshold && !atThreshold && !self.isBall && !self.isSpider && !self.isRobot &&
                !self.isRotating && !self.isOnSlope && !self.isCollidingWithSlope &&
                (!self.isPlatformer || !self.isOnGround2) && !self.isLocked && !self.isDashing) {
                self.isRotating = false;
                self.isBallRotating2 = false;
                self.isBallRotating = false;
                self.rotationSpeed = 0;
                if (!self.isBall)
                    runNormalRotation(self, false, 1.f);
                else
                    runBallRotation(self, 1.f);
            }
            if (!playerIsFallingBugged(self)) goto finish;
            yVel = self.yVelocity;
            if (!self.isUpsideDown) {
                atThreshold = yVel == -4.0;
                aboveThreshold = -4.0 < yVel;
            } else {
                atThreshold = yVel == 4.0;
                aboveThreshold = yVel < 4.0;
            }
            if (aboveThreshold || atThreshold) goto finish;
            self.isOnGround2 = false;
        }
    finish:
        self.wasJumpBuffered = self.jumpBuffered;
        self.wasRobotJump = self.touchedPad;
    }

    // moves the player for one tick, before collisions, same as PlayerObject::update
    void playerUpdate(Game& g, Player& self, float dt) {
        float const scaledDt = dt * 0.9f;
        if (self.yVelocity <= 1000.0) {
            if (self.yVelocity < -1000.0) self.yVelocity = -1000.0;
        } else
            self.yVelocity = 1000.0;
        if (self.isDead) return;
        self.lastX = self.x;
        self.lastY = self.y;
        self.yVelocityRelated3 = 0;
        if (!self.isLocked) {
            updateJump(g, self, scaledDt);
            if (0 < self.stateForce) {
                float forceScale = 1.f;
                if (self.isShip)
                    forceScale = 0.47f;
                else if (self.isBird)
                    forceScale = 0.58f;
                else if (self.isSwing)
                    forceScale = 0.4f;
                else if (self.isBall)
                    forceScale = 0.6f;
                else if (self.isSpider)
                    forceScale = 0.6f;
                else if (self.isRobot)
                    forceScale = 0.9f;
                if (self.vehicleSize != 1.f) {
                    if (self.isShip)
                        forceScale = 0.5875f;
                    else if (self.isBird)
                        forceScale = 0.72499996f;
                    else if (self.isSwing)
                        forceScale = 0.61538464f;
                }
                double const forceDy = static_cast<double>(scaledDt * self.stateForceY * forceScale);
                setYVelocity(self, forceDy + self.yVelocity);
                if (forceDy != 0.0) self.isAccelerating = true;
                if (self.isUpsideDown ? forceDy < 0.0 : forceDy > 0.0)
                    self.accelerationOrSpeed = (forceDy / 12.9399995803833) * 1.5 + self.accelerationOrSpeed;
            }
            bool dashing = self.isDashing;
            if (dashing) self.yVelocity = 0;
            double const scaledDtDouble = static_cast<double>(scaledDt);
            double vertStep = scaledDtDouble * self.yVelocity;
            double advanceStep = static_cast<double>(self.playerSpeed) * self.speedMultiplier;
            advanceStep = advanceStep * static_cast<double>(dt);
            double const maxReverseStep = advanceStep * 0.019999999552965164;
            if (!self.isDart) {
                if (dashing) vertStep = self.dashY * advanceStep;
            } else if (!dashing) {
                int const gravSign = sgn(self);
                int const holdDir = self.jumpBuffered ? 1 : -1;
                vertStep = std::fabs(advanceStep) * static_cast<double>(gravSign) * static_cast<double>(holdDir);
                if (self.vehicleSize != 1.f) vertStep = vertStep + vertStep;
            } else {
                vertStep = self.dashY * advanceStep;
            }
            self.yVelocityRelated3 = static_cast<float>(vertStep);
            if (self.isGoingLeft) advanceStep = advanceStep * -1.0;
            advanceStep = advanceStep + self.maybeReverseAcceleration;
            if (!self.isSideways) {
                self.x = self.x + static_cast<float>(advanceStep);
                self.y = self.y + static_cast<float>(vertStep);
            } else {
                self.x = self.x + static_cast<float>(vertStep);
                self.y = self.y + static_cast<float>(advanceStep);
            }
            playerMoved(self);
            self.maybeReverseAcceleration = 0;
            double const reverseSpeed = self.maybeReverseSpeed;
            if (reverseSpeed != 0.0) {
                double step = std::fabs(reverseSpeed);
                if (maxReverseStep <= std::fabs(reverseSpeed)) step = maxReverseStep;
                if (reverseSpeed <= 0.0) step = -step;
                self.maybeReverseAcceleration = step;
                self.maybeReverseSpeed = reverseSpeed - step;
            }
        }
        self.stateJumpBuffered = self.jumpBuffered ? 1 : 0;
        self.stateRingJump2 = self.stateRingJump;
        self.touchedRing = false;
        self.touchedCustomRing = false;
        self.touchedGravityPortal = false;
        self.maybeTouchedBreakableBlock = false;
        self.stateNoAutoJump -= 1;
        self.stateDartSlide -= 1;
        self.stateFlipGravity -= 1;
        self.stateHitHead -= 1;
        self.stateOnGround -= 1;
        self.stateBoostX -= 1;
        self.stateBoostY -= 1;
        self.maybeStateForce2 -= 1;
        self.stateScale -= 1;
        self.stateForce -= 1;
        self.stateForceX = 0;
        self.stateForceY = 0;
        self.nForceKeys = 0;
        self.jumpPadRelated.clear();
        if (self.isPlatformer && self.isDashing && self.dashRing) {}
    }

    // resets the player to its starting state (new attempt, player 2 joining)
    void resetObject(Game& g, Player& p) {
        p.maybeReducedEffects = true;
        p.wasTeleported = false;
        p.isCollidingWithSlope = false;
        p.collidedObject = 0;
        p.lastGroundObject = 0;
        p.currentSlope2 = 0;
        p.preLastGroundObject = 0;
        p.collidingWithLeft = 0;
        p.collidingWithRight = 0;
        p.groundYVelocity = 0;
        p.yVelocityRelated = 0;
        p.ringJumpRelated = false;
        p.maybeReverseSpeed = 0;
        p.maybeReverseAcceleration = 0;
        doReversePlayer(g, p, false);
        playerRotateGameplay(g, p, 2, 4, false, 0.f, 0.f, false, false);
        p.affectedByForces = false;
        p.isOnIce = false;
        p.maybeUpsideDownSlope = false;
        p.controlsDisabled = false;
        p.gravityMod = 1.f;
        p.maybeIsColliding = false;
        p.isOnGround = false;
        p.isOnGround2 = true;
        p.lastLandTime = 0;
        p.fallSpeed = 0;
        p.dashRing = 0;
        p.dashStartTime = 0;
        p.stateOnGround = 0;
        p.stateNoAutoJump = 0;
        p.stateDartSlide = 0;
        p.stateHitHead = 0;
        p.stateFlipGravity = 0;
        p.stateBoostX = 0;
        p.stateBoostY = 0;
        p.maybeStateForce2 = 0;
        p.stateScale = 0;
        p.stateForce = 0;
        p.stateForceX = 0;
        p.stateForceY = 0;
        p.nForceKeys = 0;
        p.jumpPadRelated.clear();
        p.touchingRings.clear();
        p.touchedRings.clear();
        stopDashing(g, p);
        p.canPlaceCheckpoint = false;
        p.lastGroundedX = 0;
        p.lastGroundedY = 0;
        p.lastActivatedPortal = 0;
        p.lastPortalX = 0;
        p.lastPortalY = 0;
        p.isLocked = false;
        p.objectSnappedTo = 0;
        p.shouldTryPlacingCheckpoint = false;
        p.isOnSlope = false;
        p.wasOnSlope = false;
        p.slopeVelocity = 0;
        p.collidingWithSlopeId = -1;
        p.currentSlopeYVelocity = 0;
        p.isAccelerating = false;
        p.slopeRotation = 0;
        p.currentSlope = 0;
        p.currentPotentialSlope = 0;
        p.potentialSlopes.clear();
        p.unk669 = false;
        p.collidedTopMinY = 0;
        p.collidedBottomMaxY = 0;
        p.collidedLeftMaxX = 0;
        p.collidedRightMinX = 0;
        p.totalTime = 0;
        p.maybeIsBoosted = false;
        p.yVelocity = 0;
        flipGravity(p, false, false);
        toggleFlyMode(p, false, false);
        toggleBirdMode(p, false, false);
        toggleRollMode(p, false, false);
        toggleDartMode(p, false, false);
        toggleRobotMode(p, false, false);
        toggleSpiderMode(p, false, false);
        toggleSwingMode(p, false, false);
        p.vehicleSize = 0.6f;
        togglePlayerScale(p, false, false);
        p.isRotating = false;
        p.isBallRotating2 = false;
        p.isBallRotating = false;
        p.rotationSpeed = 0;
        p.rotation = 0.f;
        p.rotUnc = 0.f;
        p.rotUncertain = false;
        p.isDead = false;
        updateTimeMod(p, 0.9f, false);
        p.maybeReducedEffects = false;
    }

    // keeps an angle (degrees) in (-180, 180]
    static float normAngle(float a) {
        if (a <= 180.f) {
            if (a < -180.f) a += 360.f;
        } else
            a -= 360.f;
        return a;
    }

    // starts a dash from a dash orb, direction from the orb rotation, limited to 70 degrees outside platformer
    void startDashing(Game& g, Player& p, GObj ring) {
        if (p.isDashing) return;
        p.isDashing = true;
        p.lastLandTime = 0;
        p.isRotating = false;
        p.isBallRotating2 = false;
        p.isBallRotating = false;
        p.rotationSpeed = 0;
        float angle = static_cast<float>(p.dashAngle);
        if (ring) {
            p.dashRing = ring.uid();
            p.dashStartTime = p.totalTime;
            angle = ring.rotation();
            if (ring.flipXEff()) angle += 180.f;
            if (ring.nodeSkewed()) angle += 180.f;
            angle = normAngle(std::fmod(-angle, 360.f));
            if (!p.isPlatformer) {
                float offset = p.isSideways ? -90.f : 0.f;
                if (p.isGoingLeft) offset += 180.f;
                float relAngle = angle;
                if (offset != 0.f) relAngle = normAngle(angle + offset);
                if (90.f < std::fabs(relAngle)) {
                    float const halfTurn = (relAngle <= 0.f) ? -180.f : 180.f;
                    relAngle = normAngle(halfTurn - relAngle);
                }
                float clamped = relAngle;
                if (70.f < relAngle || relAngle < -70.f) clamped = (relAngle <= 0.f) ? -70.f : 70.f;
                if (offset != 0.f) clamped = normAngle(clamped - offset);
                angle = clamped;
            }
            float const rad = angle * 0.017453292f;
            float const speed =
                    !p.isPlatformer ? 1.f : (ring.o->dashSpeed > 0.f ? ring.o->dashSpeed : 1.f) * 5.7700018882751465f;
            float dx = gcos(rad) * speed;
            float dy = gsin(rad) * speed;
            if (p.isSideways) std::swap(dx, dy);
            p.dashX = static_cast<double>(dx);
            p.dashY = static_cast<double>(dy);
            if (!p.isPlatformer) {
                p.dashY = static_cast<double>(dy / std::fabs(dx));
                p.dashX = std::fabs(p.dashY);
            }
            p.dashAngle = static_cast<double>(angle);
            if (g_trace)
                std::printf(
                        "      [trace] startDashing uid %d rot=%.2f (rx %.2f ry %.2f flipX %d, mirror %.4f dir %.0f%s) "
                        "-> angle %.2f dashX %.4f dashY %.4f k %.3f\n",
                        ring.uid(), ring.rotation(), ring.o->rotX, ring.o->rotY, ring.flipXEff() ? 1 : 0, g.mirrorT,
                        g.mirrorDir, ring.mirrorFlipped() ? " flipped" : "", angle, p.dashX, p.dashY, speed);
        }
        updateDashArt(g, p);
        p.lastGroundedX = 0;
        p.lastGroundedY = 0;
        trigGameEvent(g, GameEvent::DashStart);
    }

    // points the player in the dash direction and remembers when the dash spin started
    void updateDashArt(Game& g, Player& p) {
        if (!p.isDashing) return;
        float const rotation =
                (p.isSideways ? 90.f : 0.f) - static_cast<float>(p.dashAngle) + (p.isGoingLeft ? 180.f : 0.f);
        if (p.rotation != rotation) {
            p.rotation = rotation;
            playerMoved(p);
        }
        p.rotUnc = 0.f;
        p.rotUncertain = false;
        p.dashArtTick = g.tick;
        p.dashArtSign = ((p.isSideways != p.isUpsideDown) ? -1.f : 1.f) * p.gravityMod;
    }

    // ends the dash, cube and ball keep the angle of their dash spin (288 degrees per 0.3s, one frame per step)
    void stopDashing(Game& g, Player& p) {
        if (!p.isDashing) return;
        p.isDashing = false;
        p.lastLandTime = 0;
        p.dashRing = 0;
        if (!p.isShip && !p.isBird && !p.isDart && !p.isSwing && !p.isRobot && !p.isSpider) {
            int const frames = std::max(0, g.tick - p.dashArtTick - 1);
            float const rotation = p.dashArtSign * (288.f / 0.3f) * (1.f / 240.f) * static_cast<float>(frames);
            if (p.rotation != rotation) {
                p.rotation = rotation;
                playerMoved(p);
            }
            bool const modPlays = g.world && (g.world->modPlays || g.world->frameCadence > 1);
            p.rotUncertain = !modPlays;
            p.rotUnc = modPlays ? 0.f : 18.f;
        }
        if (p.isBall) runBallRotation(p, 1.f);
        trigGameEvent(g, GameEvent::DashStop);
    }

    // moves the player and marks its hitbox for update
    static void setPositionPlayer(Player& p, float x, float y) {
        p.x = x;
        p.y = y;
        playerMoved(p);
    }

    // spider jump: teleports to the nearest surface then flattens
    void spiderTestJump(Game& g, Player& p, bool dynamic) {
        (void)dynamic;
        spiderTestJumpInternal(g, p, false);
        trigGameEvent(g, GameEvent::SpiderTeleport);
        spiderFlatten(p);
    }

    // fills out with the solid objects (statics) or hazards touching rect r
    static void objectsInRect(Game& g, Rect const& r, bool statics, std::vector<GObj>& out) {
        out.clear();
        World const& w = *g.world;
        int const minCellX = std::max(0, World::cellOfF(r.x) - 1);
        int const maxCellX = World::cellOfF(r.x + r.w) + 1;
        int const minCellY = std::max(0, w.cellYOfF(r.y) - 1);
        int const maxCellY = w.cellYOfF(r.y + r.h) + 1;
        auto consider = [&](int idx) {
            GObj o = g.obj(idx);
            int const type = o.getType();
            if (statics ? !(type == Solid || type == Slope || type == Breakable)
                        : !(type == Hazard || type == AnimatedHazard))
                return;
            if (o.disabled() || o.st().destroyed) return;
            Rect const objRect = o.getObjectRect();
            if (o.o->radius <= 0.f) {
                if (!intersectsRect(r, objRect)) return;
                if (!statics && o.hasOrientedBox()) {
                    OBB rectBox;
                    rectBox.calculateWithCenter(Point{r.x + r.w * 0.5f, r.y + r.h * 0.5f}, r.w, r.h, 0.f);
                    OBB const objBox = objectObb(o);
                    if (!(objBox.overlaps1Way(rectBox) && rectBox.overlaps1Way(objBox))) return;
                }
                out.push_back(o);
            } else if (!statics) {
                float const radius = o.circleRadius();
                Point const center = o.getRealPosition();
                bool hit = r.contains(center.x, center.y);
                if (!hit) {
                    float const dx = std::max(std::max(r.x - center.x, 0.f), center.x - (r.x + r.w));
                    float const dy = std::max(std::max(r.y - center.y, 0.f), center.y - (r.y + r.h));
                    hit = dx * dx + dy * dy < radius * radius;
                }
                if (hit) out.push_back(o);
            }
        };
        auto const& areaMoved = g.trig.areaMoved;
        auto considerUnmoved = [&](int idx) {
            if (!areaMoved.empty() && std::binary_search(areaMoved.begin(), areaMoved.end(), idx)) return;
            consider(idx);
        };
        if (g.trig.on)
            for (int idx : areaMoved) consider(idx);
        for (int cx = minCellX; cx <= maxCellX; ++cx) {
            if (g.trig.on) {
                for (int cy = minCellY; cy <= maxCellY; ++cy) {
                    if (auto const* ov = trigCell(g, cx, cy)) {
                        for (int idx : *ov) considerUnmoved(idx);
                        continue;
                    }
                    if (static_cast<size_t>(cx) < w.cells.size() &&
                        static_cast<size_t>(cy) < w.cells[static_cast<size_t>(cx)].size())
                        for (int idx : w.cells[static_cast<size_t>(cx)][static_cast<size_t>(cy)]) considerUnmoved(idx);
                }
                continue;
            }
            if (static_cast<size_t>(cx) >= w.cells.size()) break;
            auto const& column = w.cells[static_cast<size_t>(cx)];
            for (int cy = minCellY; cy <= maxCellY && static_cast<size_t>(cy) < column.size(); ++cy)
                for (int idx : column[static_cast<size_t>(cy)]) consider(idx);
            if (w.timeline && static_cast<size_t>(cx) < w.timeline->byColumn.size())
                for (int timelineIdx : w.timeline->byColumn[static_cast<size_t>(cx)]) {
                    TimelineKey const& key = w.timeline->at(timelineIdx, g.tlTick);
                    if (World::cellOf(static_cast<double>(key.x)) == cx)
                        consider(w.timeline->objs[static_cast<size_t>(timelineIdx)].objIndex);
                }
        }
    }

    // spider teleport to the nearest solid in the gravity direction
    void spiderTestJumpInternal(Game& g, Player& p, bool dynamic) {
        if (p.isDead || p.isLocked || p.maybeReducedEffects) return;
        if (p.isPlatformer) return;
        bool const side = p.isSideways;
        float const hazardWidth = p.vehicleSize * 8.f;
        bool const fly = p.isShip || p.isBird || p.isDart || p.isSwing || p.isBall || p.isSpider;
        bool const checkCamera = side || !(!g.isFreeMode && fly);
        Rect const playerBox = playerRect(p);
        auto vMin = [&](Rect const& r) { return side ? getMinX(r) : getMinY(r); };
        auto vMax = [&](Rect const& r) { return side ? getMaxX(r) : getMaxY(r); };
        auto fMin = [&](Rect const& r) { return side ? getMinY(r) : getMinX(r); };
        auto fMax = [&](Rect const& r) { return side ? getMaxY(r) : getMaxX(r); };
        auto vPos = [&](Rect& r) -> float& { return side ? r.x : r.y; };
        auto vLen = [&](Rect& r) -> float& { return side ? r.w : r.h; };
        auto fPos = [&](Rect& r) -> float& { return side ? r.y : r.x; };
        auto fLen = [&](Rect& r) -> float& { return side ? r.h : r.w; };
        Rect boxCopy = playerBox;
        float const boxV = vPos(boxCopy);
        float const boxVLen = vLen(boxCopy);
        float const boxF = fPos(boxCopy);
        float const boxFLen = fLen(boxCopy);
        float const posV = side ? p.x : p.y;
        float const posF = side ? p.y : p.x;
        float low;
        float high;
        if (side) {
            high = p.x + 3000.f;
            low = std::max(0.f, p.x - 3000.f);
        } else if (checkCamera) {
            low = std::max(90.f, p.y - 3000.f);
            high = p.y + 3000.f;
        } else {
            low = getMinPortalY(g);
            high = getMaxPortalY(g);
        }
        Rect search = playerBox;
        if (!p.isUpsideDown) {
            vPos(search) = boxV + (boxVLen - 2.f);
            vLen(search) = high - vPos(search);
        } else {
            vLen(search) = (boxV - low) + 2.f;
            vPos(search) = low;
        }
        Rect hazardRect = search;
        fPos(hazardRect) = boxF;
        fLen(hazardRect) = boxFLen;
        Rect wide = hazardRect;
        fLen(wide) += 4.f;
        fPos(hazardRect) += (fLen(hazardRect) - hazardWidth) * 0.5f;
        fLen(hazardRect) = hazardWidth;
        fLen(search) = boxFLen * 0.5f + 1.f;
        fPos(search) = !p.isGoingLeft ? boxFLen * 0.5f + boxF : (boxF - boxFLen * 0.5f) - 1.f;
        std::vector<GObj> statics;
        std::vector<GObj> damaging;
        objectsInRect(g, search, true, statics);
        objectsInRect(g, hazardRect, false, damaging);
        bool isDynamic = dynamic;
        if ((statics.empty() && !damaging.empty()) || dynamic) {
            isDynamic = true;
            objectsInRect(g, wide, true, statics);
        }
        auto key = [&](GObj const& o) {
            Rect const rect = o.getObjectRect();
            return !p.isUpsideDown ? static_cast<int>(vMin(rect) * 10.f) : -static_cast<int>(vMax(rect) * 10.f);
        };
        crtQsort(statics, [&](GObj const& a, GObj const& b) { return key(a) - key(b); });
        crtQsort(damaging, [&](GObj const& a, GObj const& b) { return key(a) - key(b); });
        auto realV = [&](GObj const& o) {
            Point const realPos = o.getRealPosition();
            return side ? realPos.x : realPos.y;
        };
        float target = 0.f;
        bool hazardFirst = false;
        if (statics.empty()) {
            if (!damaging.empty()) {
                hazardFirst = true;
                target = realV(damaging[0]);
            }
        } else {
            Rect probe = playerBox;
            bool accepted = false;
            for (size_t i = 0; i < statics.size() && !accepted; ++i) {
                GObj const& o = statics[i];
                Rect const objRect = o.getObjectRect();
                float candidate = !p.isUpsideDown ? vMin(objRect) - boxVLen * 0.5f : vMax(objRect) + boxVLen * 0.5f;
                bool slopeTarget = false;
                if (o.getType() == Slope) {
                    float const unrotatedV = realV(o);
                    int const savedRotOvN = g.rotOvN;
                    Game::RotOv* const savedRotOv = g.rotOv;
                    if (side) rotateGameplayObject(g, p, o);
                    int const slopeDir = o.slopeDir();
                    bool const topSet =
                            slopeDir >= 0 && slopeDir < 7 && ((0x6au >> static_cast<unsigned>(slopeDir)) & 1u) != 0;
                    if (topSet != p.isUpsideDown) {
                        double const slopeY = slopeYPos(o, p.x);
                        Point const realPos = o.getRealPosition();
                        float const sign = p.isUpsideDown ? -1.f : 1.f;
                        candidate = ((side ? unrotatedV : realPos.y) + static_cast<float>(slopeY - static_cast<double>(realPos.y))) - boxVLen * 0.5f * sign;
                        slopeTarget = true;
                    }
                    g.rotOvN = savedRotOvN;
                    g.rotOv = savedRotOv;
                }
                bool const behind = !p.isUpsideDown ? (candidate < posV - 10.f) : (posV + 10.f < candidate);
                if (behind) continue;
                bool checkBlockers;
                if (!p.isGoingLeft)
                    checkBlockers = posF < fMin(objRect);
                else
                    checkBlockers = fMax(objRect) < posF;
                if (checkBlockers) {
                    vPos(probe) = candidate - boxVLen * 0.5f;
                    bool blocked = false;
                    for (size_t k = 0; k < statics.size() && !blocked; ++k) {
                        if (k == i) continue;
                        Rect blockerRect = statics[k].getObjectRect();
                        if (!slopeTarget) {
                            vLen(blockerRect) -= 4.f;
                            vPos(blockerRect) += 2.f;
                        }
                        if (intersectsRect(blockerRect, probe)) blocked = true;
                    }
                    if (blocked) continue;
                }
                target = candidate;
                accepted = true;
            }
            if (!accepted) target = 0.f;
        }
        if (target == 0.f) target = !p.isUpsideDown ? high - boxVLen * 0.5f : low + boxVLen * 0.5f;
        if (!hazardFirst && !damaging.empty()) {
            float const hazardV = realV(damaging[0]);
            bool const beyond = !p.isUpsideDown ? target < hazardV : hazardV < target;
            if (!beyond && target != hazardV) {
                target = hazardV;
                if (!isDynamic) {
                    spiderTestJumpInternal(g, p, true);
                    return;
                }
            }
        }
        if (side)
            setPositionPlayer(p, target, p.y);
        else
            setPositionPlayer(p, p.x, target);
        flipGravity(p, !p.isUpsideDown, true);
        setYVelocity(p, p.isUpsideDown ? 1.0 : -1.0);
        p.isOnGround3 = false;
        p.jumpBuffered = false;
        p.collisionLogTop.clear();
        p.collisionLogBottom.clear();
        p.collisionLogLeft.clear();
        p.collisionLogRight.clear();
        p.unk50C = -1;
        p.unk510 = -1;
        p.lastCollisionBottom = -1;
        p.lastCollisionTop = -1;
        p.lastCollisionLeft = -1;
        p.lastCollisionRight = -1;
        p.lastSpiderFlipTime = p.totalTime;
        if (checkCamera) cameraCheckLimitAfterTeleport(g, p, 60.f);
    }

    // starts the cube half turn in the air, notNormalMode forces it for other modes
    void runNormalRotation(Player& p, bool notNormalMode, float speed) {
        if (notNormalMode ||
            (!p.isShip && !p.isBird && !p.isDart && !p.isSwing && !p.isRobot && !p.isSpider && !p.isDashing)) {
            float const rotDuration = (p.vehicleSize == 1.f) ? 0.43333334f : 0.33333334f;
            p.isRotating = true;
            int const halfTurn = p.isUpsideDown ? -0xb4 : 0xb4;
            int const leftSign = p.isGoingLeft ? -1 : 1;
            int const sideSign = p.isSideways ? -1 : 1;
            p.rotationSpeed =
                    (static_cast<float>(halfTurn * leftSign) * static_cast<float>(sideSign) * p.gravityMod * speed) /
                    rotDuration;
        }
    }

    // resetTouchedRings for step.cpp, called each tick
    void resetTouchedRingsForTick(Player& p) { resetTouchedRings(p); }

}
