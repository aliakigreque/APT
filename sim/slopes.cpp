// Player on slopes, ported from the game's slope collision code.

#include "gd.hpp"

#include <cmath>
#include <cstdio>

namespace ap::sim {

    // returns true if the slope surface faces down (directions 1, 3, 5, 6, mask taken from the game)
    static bool slopeTopSet(int dir) {
        return dir >= 0 && dir < 7 && ((0x6au >> static_cast<unsigned>(dir)) & 1u) != 0;
    }

    // height of the slope surface at x, same as GameObject::slopeYPos
    double slopeYPos(GObj slope, float x) {
        Rect const r = slope.getObjectRect();
        float const minX = getMinX(r);
        float const maxX = getMaxX(r);
        float const minY = getMinY(r);
        float const maxY = getMaxY(r);
        float const ratio = r.h / r.w;
        double d = static_cast<double>(x);
        if (static_cast<double>(minX) < d) {
            d = (d - static_cast<double>(maxX)) * static_cast<double>(ratio);
            d = slope.slopeUphill() ? d + static_cast<double>(maxY) : static_cast<double>(minY) - d;
        } else {
            d = (static_cast<double>(minX) - d) * static_cast<double>(ratio);
            d = slope.slopeUphill() ? static_cast<double>(minY) - d : d + static_cast<double>(maxY);
        }
        if (slope.o->slopeHazard) d += slopeTopSet(slope.slopeDir()) ? -4.0 : 4.0;
        return d;
    }

    // rotates the player towards the slope angle
    void updateSlopeRotation(Player& p, float dt) {
        float const r = convertToClosestRotation(p, static_cast<float>(p.slopeRotation * 57.295780181884766));
        updateRotationTo(p, dt, r);
    }

    // collides with the slope's vertical wall and flat side like a block, returns true if nothing else is needed
    bool preSlopeCollision(Game& g, Player& p, float dt, GObj object) {
        if (object.uid() == p.collidingWithSlopeId) return false;
        Rect const objRect = object.getObjectRect();
        Rect const plRect = playerRect(p);
        int const dir = object.slopeDir();
        bool handled = false;
        float flatInset = 0.f;
        bool const wallLeft = (dir >= 2 && dir <= 4) || dir == 6;
        bool const topSet = slopeTopSet(dir);
        float wallInset = (p.isPlatformer && (p.isOnSlope || p.wasOnSlope)) ? 5.f : 0.f;
        Point const real = object.getRealPosition();
        Point const last = object.lastPosition();
        float const objDelta = p.isSideways ? real.y - last.y : real.x - last.x;
        float const speed =
                static_cast<float>(static_cast<double>(p.playerSpeed) * p.speedMultiplier * static_cast<double>(dt));
        if (wallLeft) {
            if ((!p.isGoingLeft || p.isPlatformer || objDelta < speed) && p.x < objRect.x) {
                handled = true;
                flatInset = (p.isGoingLeft && !p.isPlatformer) ? 1.f : 0.f;
                wallInset += flatInset;
                Rect const strip{objRect.x, objRect.y + wallInset, 1.f, objRect.h - (wallInset + wallInset)};
                if (intersectsRect(plRect, strip)) collidedWithObjectInternal(g, p, dt, object, strip, false);
            }
        } else {
            if ((p.isGoingLeft || p.isPlatformer || speed < objDelta) && getMaxX(objRect) < p.x) {
                handled = true;
                flatInset = (!p.isGoingLeft && !p.isPlatformer) ? 1.f : 0.f;
                wallInset += flatInset;
                Rect const strip{(objRect.w + objRect.x) - 1.f, objRect.y + wallInset, 1.f, objRect.h - (wallInset + wallInset)};
                if (intersectsRect(plRect, strip)) collidedWithObjectInternal(g, p, dt, object, strip, false);
            }
        }
        float const flatWidth = objRect.w - (flatInset + flatInset);
        Rect flatStrip;
        bool flat;
        if (topSet) {
            flatStrip = Rect{objRect.x + flatInset, (objRect.y + objRect.h) - 1.f, flatWidth, 1.f};
            flat = flatStrip.y + flatStrip.h <= p.y;
        } else {
            flatStrip = Rect{objRect.x + flatInset, objRect.y, flatWidth, 1.f};
            flat = p.y <= flatStrip.y + flatStrip.h;
        }
        if (flat) {
            handled = true;
            if (intersectsRect(plRect, flatStrip)) collidedWithObjectInternal(g, p, dt, object, flatStrip, false);
        } else if (!handled) {
            p.unk669 = false;
            if (!p.isOnSlope) {
                p.addPotentialSlope(object.uid());
                GObj current = p.currentPotentialSlope ? g.objByUid(p.currentPotentialSlope) : GObj{};
                if (!current)
                    p.currentPotentialSlope = object.uid();
                else {
                    bool replace = false;
                    bool const currentGoesDown = !current.slopeUphill() || slopeTopSet(current.slopeDir());
                    bool const objectGoesDown = !object.slopeUphill() || slopeTopSet(object.slopeDir());
                    if (currentGoesDown && objectGoesDown && object.getPosition().x < current.getPosition().x)
                        replace = true;
                    if (!replace && current.slopeUphill() && !slopeTopSet(current.slopeDir()) &&
                        current.getPosition().x < object.getPosition().x)
                        replace = true;
                    if (replace) p.currentPotentialSlope = object.uid();
                }
            }
        }
        if (g_trace)
            std::printf(
                    "      [trace] preSlope uid %d : handled=%d flat=%d intersects=%d (player [%.2f %.2f %.2f %.2f], "
                    "slope [%.2f %.2f %.2f %.2f])\n",
                    object.uid(), handled, flat, intersectsRect(objRect, plRect), plRect.x, plRect.y, plRect.w,
                    plRect.h, objRect.x, objRect.y, objRect.w, objRect.h);
        return handled || !intersectsRect(objRect, plRect);
    }

    // puts the player on the slope or hits it from below, same as the game
    void collidedWithSlopeInternal(Game& g, Player& p, float dt, GObj object, bool forced) {
        if (!forced && preSlopeCollision(g, p, dt, object)) return;
        Rect const plRect = playerRect(p);
        Rect const objRect = object.getObjectRect();
        bool const uphill = object.slopeUphill();
        bool movingDownhill = !uphill;
        bool goingLeft = false;
        if (p.isPlatformer ? false : p.isGoingLeft) {
            goingLeft = true;
            movingDownhill = uphill;
        }
        int const dir = object.slopeDir();
        bool const topSet = slopeTopSet(dir);
        bool keepSlopeVelSign = false;
        bool slopeOvertakes = false;
        Point const real = object.getRealPosition();
        Point const last = object.lastPosition();
        float const dMain = p.isSideways ? real.y - last.y : real.x - last.x;
        float const dOther = p.isSideways ? real.x - last.x : real.y - last.y;
        if (static_cast<double>(p.playerSpeed) * p.speedMultiplier * static_cast<double>(dt) <
            static_cast<double>(dMain)) {
            if (!p.isUpsideDown) {
                if (movingDownhill) keepSlopeVelSign = true;
            } else
                keepSlopeVelSign = !movingDownhill;
            slopeOvertakes = true;
            movingDownhill = !movingDownhill;
        }
        if (p.isUpsideDown != p.isSideways) movingDownhill = !movingDownhill;
        bool const wasOn = p.wasOnSlope;
        float const sign = p.isUpsideDown ? -1.f : 1.f;
        if (wasOn) {
            if (0.f < sign * p.slopeVelocity && movingDownhill && p.isCurrentSlopeTop == topSet &&
                p.isCurrentSlopeTop == p.isUpsideDown)
                return;
        }
        float const slopeYVelocity =
                objRect.h / static_cast<float>(static_cast<double>(objRect.w) / (static_cast<double>(p.playerSpeed) * p.speedMultiplier));
        if (wasOn) {
            if (0.f < sign * p.slopeVelocity && static_cast<double>(slopeYVelocity) < p.currentSlopeYVelocity) return;
        }
        float tolerance = static_cast<float>(movingDownhill ? 1 : 0);
        if (wasOn) tolerance = static_cast<float>(movingDownhill ? 4 : 0);
        float const dPerp = p.isSideways ? last.x - real.x : last.y - real.y;
        bool const movingUp = (!p.isUpsideDown && 0.f < dPerp) || (p.isUpsideDown && dPerp < 0.f);
        if (forced || movingUp) {
            float extra = std::fabs(dPerp);
            float const maxExtra = (p.isPlatformer ? 10.f : 5.f) * dt;
            if (maxExtra <= extra) extra = maxExtra;
            tolerance = tolerance + extra;
        }
        float const halfHeight = plRect.h * 0.5f;
        float const footOffset = halfHeight / gcos(static_cast<float>(p.slopeRotation));
        float const footY = !p.isUpsideDown ? (p.y - footOffset) - tolerance : footOffset + p.y + tolerance;
        if (wasOn) {
            if (!p.isUpsideDown) {
                if (objRect.y + objRect.h < footY) return;
            } else {
                if (footY < objRect.y) return;
            }
        } else {
            Rect const innerRect{objRect.x, objRect.y + 1.f, objRect.w, objRect.h - 2.f};
            if (!intersectsRect(plRect, innerRect)) {
                if (g_trace)
                    std::printf("      [trace] slope uid %d : no intersection with the inner rect\n", object.uid());
                return;
            }
        }
        float const posX = p.x;
        float const posY = p.y;
        float const minY = getMinY(objRect);
        float const maxY = getMaxY(objRect);
        float const angle = std::atan(objRect.h / objRect.w);
        float const slantedHalfHeight = (plRect.h / gcos(angle)) * 0.5f;
        float oppositeOffset = 0.f;
        bool oppositeSlope = false;
        if (wasOn && p.collidingWithSlopeId != object.uid() && p.isCurrentSlopeTop != topSet) {
            oppositeSlope = true;
            if (!p.isPlatformer) oppositeOffset = p.vehicleSize * 20.f;
        }
        float const slopeY = static_cast<float>(slopeYPos(object, posX));
        bool clampedEnd = false;
        bool cappedTop;
        float targetY;
        if (!topSet) {
            float const cap = (halfHeight + maxY) - oppositeOffset;
            float y = (slopeY + slantedHalfHeight) - oppositeOffset;
            float const unclampedY = y;
            if (cap <= y) y = cap;
            float const cappedY = y;
            cappedTop = cap <= unclampedY;
            if (y <= minY) y = minY;
            clampedEnd = cappedY <= minY;
            targetY = y;
        } else {
            float const cap = (minY - halfHeight) + oppositeOffset;
            float y = (slopeY - slantedHalfHeight) + oppositeOffset;
            float const unclampedY = y;
            if (y <= cap) y = cap;
            cappedTop = unclampedY <= cap;
            if (maxY <= y) {
                y = maxY;
                clampedEnd = true;
            }
            targetY = y;
        }
        if (std::isnan(targetY)) {
            if (g_trace)
                std::printf("[trace] slope uid %d : NaN target (zero size slope) : no effect\n", object.uid());
            return;
        }
        bool maybeUpsideDownSlope = false;
        bool capFlyVelocity = false;
        bool land;
        bool jumpBoost = false;
        bool skipHitGround = false;
        if (p.isUpsideDown == topSet) {
            bool allowSnap;
            if (!oppositeSlope && movingDownhill && !p.isOnSlope) {
                if (!p.maybeIsBoosted)
                    allowSnap = !p.isShip || !p.jumpBuffered;
                else if (p.isShip)
                    allowSnap = !p.jumpBuffered;
                else if (p.isBird || p.isDart || p.isSwing)
                    allowSnap = true;
                else
                    allowSnap = false;
            } else {
                allowSnap = false;
                if (!movingDownhill) allowSnap = forced || movingUp;
            }
            if (!p.isUpsideDown) {
                if (posY < targetY)
                    land = true;
                else if (!allowSnap)
                    land = false;
                else if (targetY + tolerance <= posY)
                    land = false;
                else if (!p.isBird)
                    land = true;
                else
                    land = p.yVelocity <= 0.0;
            } else {
                if (targetY < posY)
                    land = true;
                else if (!allowSnap)
                    land = false;
                else if (posY <= targetY - tolerance)
                    land = false;
                else if (!p.isBird)
                    land = true;
                else
                    land = 0.0 <= p.yVelocity;
            }
            bool const isFlying = p.isShip || p.isBird || p.isDart || p.isSwing;
            if (p.jumpBuffered && isFlying && !(movingDownhill && !p.isPlatformer)) {
                jumpBoost = true;
                skipHitGround = !(p.isPlatformer && oppositeSlope);
            }
            if (!land) {
            } else {
                bool shouldDie = object.o->slopeHazard;
                if (!shouldDie && (oppositeSlope || (p.isDart && p.stateDartSlide < 1)) && !p.isPlatformer &&
                    p.stateHitHead < 1)
                    shouldDie = true;
                if (shouldDie && !p.ignoreDamage) {
                    if (p.maybeCanRunIntoBlocks) {
                        p.maybeIsColliding = true;
                        return;
                    }
                    destroyPlayer(g, p, object);
                    return;
                }
            }
        } else {
            maybeUpsideDownSlope = true;
            bool const isFlying = p.isShip || p.isBird || p.isDart || p.isSwing;
            if (movingDownhill)
                tolerance = 0.f;
            else if (isFlying && p.jumpBuffered)
                tolerance = static_cast<float>((wasOn ? 1 : 0) + 1);
            bool const canSnap = !oppositeSlope && !(isFlying && movingDownhill && !p.isPlatformer) && !p.isOnSlope;
            if (!p.isUpsideDown) {
                if (targetY < posY)
                    land = true;
                else if (!canSnap)
                    land = false;
                else
                    land = targetY - tolerance < posY;
            } else {
                if (posY < targetY)
                    land = true;
                else if (!canSnap)
                    land = false;
                else
                    land = posY < targetY + tolerance;
            }
            skipHitGround = !p.jumpBuffered && !p.isPlatformer && (isFlying || p.isBall) && movingDownhill && wasOn;
            if ((skipHitGround || !p.jumpBuffered) && isFlying) capFlyVelocity = movingDownhill;
            if (land) {
                if (!(isFlying || p.isBall) && !p.isPlatformer && p.stateHitHead < 1) {
                    bool const recent = (p.gameModeChangedTime != 0.0 &&
                                        p.totalTime - p.gameModeChangedTime < 0.10000000149011612) ||
                                        (p.lastFlipTime != 0.0 && p.totalTime - p.lastFlipTime < 0.10000000149011612);
                    if (recent) {
                        p.x = posX;
                        p.y = targetY;
                        playerMoved(p);
                        double yVelocity = p.yVelocity;
                        if (!p.isUpsideDown) {
                            if (!(yVelocity < -2.0)) yVelocity = -2.0;
                        } else {
                            if (!(2.0 < yVelocity)) yVelocity = 2.0;
                        }
                        setYVelocity(p, yVelocity);
                        p.isOnGround = false;
                        p.isOnGround2 = false;
                        return;
                    }
                    bool const deep = !p.isUpsideDown ? (targetY < posY - 2.f) : (posY + 2.f < targetY);
                    if (!deep) {
                        p.isOnGround = false;
                        p.isOnGround2 = false;
                        return;
                    }
                    if (!p.ignoreDamage) {
                        if (p.maybeCanRunIntoBlocks) {
                            p.maybeIsColliding = true;
                            return;
                        }
                        destroyPlayer(g, p, object);
                        return;
                    }
                }
                bool shouldDie = object.o->slopeHazard;
                if (!shouldDie && (oppositeSlope || (p.isDart && p.stateDartSlide < 1)) && !p.isPlatformer &&
                    p.stateHitHead < 1)
                    shouldDie = true;
                if (shouldDie && !p.ignoreDamage) {
                    if (p.maybeCanRunIntoBlocks) {
                        p.maybeIsColliding = true;
                        return;
                    }
                    destroyPlayer(g, p, object);
                    return;
                }
            }
        }
        float levelTargetY = targetY;
        if (p.isSideways && g.rotOvN)
            if (auto const* ov = g.rotOvFind(object.index))
                levelTargetY = static_cast<float>(static_cast<double>(targetY) - ov->dy);
        if (wasOn) {
            bool const changed = p.unk3d0 != static_cast<double>(targetY) &&
                                (!p.isSideways || p.blackOrbRelated != static_cast<double>(levelTargetY));
            if (!changed)
                land = false;
            else if (dOther != 0.f && ((cappedTop && p.unk3e0) || (clampedEnd && p.unk3e1)) && !p.isDashing)
                land = false;
        }
        if (g_trace)
            std::printf("[trace] slope uid %d : path %c, target y=%.3f (slopeY=%.3f tol=%.3f cap=%d end=%d) land=%d movingDownhill=%d oppositeSlope=%d topSet=%d dPerp=%.4f movingUp=%d forced=%d real=(%.3f,%.3f) last=(%.3f,%.3f) posY=%.3f\n",
                    object.uid(), p.isUpsideDown == topSet ? 'A' : 'B', targetY, slopeY, tolerance, cappedTop,
                    clampedEnd, land, movingDownhill, oppositeSlope, topSet, dPerp, movingUp, forced, real.x, real.y,
                    last.x, last.y, posY);
        if (movingDownhill && p.isDart && p.jumpBuffered && topSet == p.isUpsideDown) return;
        if (!land) return;

        p.currentSlope = object.uid();
        p.slopeAngleRadians = std::atan(objRect.h / objRect.w);
        p.currentPotentialSlope = p.currentSlope;
        p.addPotentialSlope(object.uid());
        p.unk_584 = static_cast<double>(slantedHalfHeight - plRect.h * 0.5f);
        p.isCurrentSlopeTop = topSet;
        p.unk3d0 = static_cast<double>(targetY);
        p.blackOrbRelated = static_cast<double>(levelTargetY);
        p.collidingWithSlopeId = object.uid();
        p.slopeFlipGravityRelated = movingDownhill;
        float const oldSlopeRotation = static_cast<float>(p.slopeRotation);
        p.unk3e0 = cappedTop;
        p.unk3e1 = clampedEnd;
        int angleSign = p.isUpsideDown ? -1 : 1;
        if (!movingDownhill) angleSign = -angleSign;
        if (slopeOvertakes) angleSign = -angleSign;
        if (goingLeft) angleSign = -angleSign;
        if (p.isSideways) angleSign = -angleSign;
        p.slopeRotation = static_cast<double>((static_cast<float>(angleSign) * (angle * 57.29578f)) * 0.017453292f);
        p.x = posX;
        p.y = targetY;
        playerMoved(p);
        p.isOnSlope = true;
        if (!wasOn) {
            p.slopeStartTime = p.totalTime;
            p.yVelocityBeforeSlope = p.yVelocity;
        }
        p.maybeUpsideDownSlope = maybeUpsideDownSlope;
        if (!skipHitGround) {
            if (!maybeUpsideDownSlope) {
                float const yVelocity = static_cast<float>(p.yVelocity);
                hitGround(g, p, object, false);
                if (!p.isUpsideDown ? 5.f < yVelocity : yVelocity < -5.f)
                    setYVelocity(p, static_cast<double>(yVelocity));
                updateCollide(p, 1, object);
                p.currentSlope2 = object.uid();
                Rect const rect = object.getObjectRect();
                p.slopeAngle = std::atan(rect.h / rect.w);
                bool slidesUphill = object.slopeUphill();
                if (p.isSideways) slidesUphill = !slidesUphill;
                p.slopeSlidingMaybeRotated = slidesUphill;
                if (slidesUphill && 1.f <= p.slopeAngle) p.maybeSlidingTime = 3;
                if (real.x != last.x || real.y != last.y) {
                    if (!p.isUpsideDown) {
                        if (last.y < real.y) p.groundYVelocity = static_cast<double>((real.y - last.y) / dt);
                    } else {
                        if (real.x < last.x) p.groundYVelocity = static_cast<double>((real.x - last.x) / dt);
                    }
                }
            } else {
                double yVelocity = p.yVelocity;
                if (!p.isUpsideDown) {
                    if (!(yVelocity < 0.0)) yVelocity = 0.0;
                } else {
                    if (!(0.0 < yVelocity)) yVelocity = 0.0;
                }
                setYVelocity(p, yVelocity);
                p.isOnGround = false;
                p.isOnGround2 = false;
                updateCollide(p, 0, object);
            }
        }
        if (capFlyVelocity) {
            if (!p.isUpsideDown) {
                if (-2.0 < p.yVelocity) setYVelocity(p, static_cast<double>(1.f * -2.f));
            } else {
                if (p.yVelocity < 2.0) setYVelocity(p, static_cast<double>(-1.f * -2.f));
            }
        }
        if (jumpBoost) {
            if (!p.isUpsideDown) {
                if (p.yVelocity < 2.0) setYVelocity(p, static_cast<double>(1.f + 1.f));
            } else {
                if (-2.0 < p.yVelocity) setYVelocity(p, static_cast<double>(-1.f + -1.f));
            }
        }
        p.currentSlopeYVelocity = static_cast<double>(slopeYVelocity);
        float slopeSpeed = 0.8f / angle;
        slopeSpeed = (1.1f < slopeSpeed) ? 1.1f : slopeSpeed;
        slopeSpeed = slopeSpeed * (slopeYVelocity * 1.4f);
        if (movingDownhill && !keepSlopeVelSign) slopeSpeed = slopeSpeed * -1.f;
        float const gravitySign = p.isUpsideDown ? -1.f : 1.f;
        p.slopeVelocity = gravitySign * slopeSpeed;
        if (p.isPlatformer && maybeUpsideDownSlope && p.isBird) p.jumpBuffered = false;
        if (p.isShip || p.isBird || p.isDart || p.isSwing || p.isBall) {
            p.slopeVelocity = (gravitySign * slopeSpeed) * 0.75f;
            if (p.isBall) {
                if (static_cast<double>(oldSlopeRotation) != p.slopeRotation) {
                    float ballSpeed = 1.f / gcos(static_cast<float>(p.slopeRotation));
                    if (2.f < ballSpeed) ballSpeed = (ballSpeed - 1.f) * 0.7f + 1.f;
                    p.isRotating = false;
                    p.isBallRotating2 = false;
                    p.isBallRotating = false;
                    p.rotationSpeed = 0.f;
                    runBallRotation(p, ballSpeed);
                }
                return;
            }
        }
        updateSlopeRotation(p, dt);
    }

}
