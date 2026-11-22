// Player rotation and the rotated hitboxes of objects.

#include "gd.hpp"

#include <algorithm>
#include <cmath>

namespace ap::sim {

    // goes from angle a to angle b (radians) by the shortest way, t is the fraction of the path
    static float lerpAngle(float a, float b, float t) {
        float cosA = gcos(a * 0.5f);
        float sinA = gsin(a * 0.5f);
        float cosB = gcos(b * 0.5f);
        float sinB = gsin(b * 0.5f);
        float dot = sinB * sinA + cosB * cosA;
        if (dot < 0.f) {
            dot = -dot;
            sinB = -sinB;
            cosB = -cosB;
        }
        float weightA = 1.f - t;
        if (0.0001f < 1.f - dot) {
            dot = std::acos(dot);
            float const sinOmega = gsin(dot);
            weightA = gsin(weightA * dot);
            weightA = weightA / sinOmega;
            dot = gsin(dot * t);
            t = dot / sinOmega;
        }
        double const halfAngle = std::atan2(static_cast<double>(sinA * weightA + t * sinB),
                                            static_cast<double>(cosA * weightA + t * cosB));
        return static_cast<float>(halfAngle + halfAngle);
    }

    // returns the angle equal to rotation (90 degree steps) closest to the player's current rotation
    float convertToClosestRotation(Player const& p, float rotation) {
        float candidate = rotation;
        if (!p.isShip && !p.isBird && !p.isDart && !p.isSwing && !p.isRobot && !p.isSpider && !p.isDashing) {
            float current = static_cast<float>(static_cast<int>(p.rotation) % 0x168);
            if (rotation <= current) {
                if (rotation < current) {
                    do {
                        rotation = candidate;
                        candidate = rotation + 90.f;
                    } while (candidate < current);
                }
            } else {
                do {
                    rotation = candidate;
                    candidate = rotation - 90.f;
                } while (current < candidate);
            }
            if (std::fabs(current - rotation) <= std::fabs(current - candidate)) return rotation;
        }
        return candidate;
    }

    // shrinks the rotation uncertainty after moving a fraction t towards a known target, not in the game
    static void shrinkRotUnc(Player& p, float t) {
        if (p.rotUnc <= 0.f) return;
        p.rotUnc *= 1.f - std::clamp(t, 0.f, 1.f);
        if (p.rotUnc < 0.25f) {
            p.rotUnc = 0.f;
            p.rotUncertain = false;
        }
    }

    // moves the player rotation towards rotation (degrees), same as PlayerObject::updateRotation(dt, rotation)
    void updateRotationTo(Player& p, float dt, float rotation) {
        if (p.isDashing) return;
        float const startRotation = p.rotation;
        float speedFactor = p.playerSpeed * 0.175f;
        if (!p.isShip) {
            if (!p.isBird && !p.isDart && !p.isSwing && !p.isBall && p.isOnGround2 && !p.isOnSlope && !p.wasOnSlope)
                speedFactor = speedFactor * 3.f;
        } else {
            speedFactor = speedFactor * 0.5f;
        }
        if (speedFactor * dt <= dt) dt = speedFactor * dt;
        float r = lerpAngle(startRotation * 0.017453292f, rotation * 0.017453292f, dt);
        r = r * 57.29578f;
        p.rotation = r;
        shrinkRotUnc(p, dt);
    }

    // tilts flying modes towards their movement since last step, same as PlayerObject::updateShipRotation
    static void updateShipRotation(Player& p, float dt) {
        if (p.isOnSlope || p.isDashing) return;
        bool const isFlying = p.isBird || p.isShip || p.isSwing || p.isDart;
        float along;
        float across;
        if (!p.isSideways) {
            along = p.x - p.shipRotX;
            if (isFlying && p.isGoingLeft) along = std::fabs(along);
            across = -(p.y - p.shipRotY);
        } else {
            along = p.y - p.shipRotY;
            if (isFlying && p.isGoingLeft) along = std::fabs(along);
            across = p.x - p.shipRotX;
        }
        float const distSquared = across * across + along * along;
        if (distSquared < dt * 1.2f) {
            if (!p.isPlatformer || (!p.isShip && !p.isBird)) return;
        }
        float target = std::atan2(across, along);
        float const current = p.rotation * 0.01745329238474369f;
        if (p.isBird) target *= -0.4f;
        float factor = 0.15f;
        if (p.isCollidingWithSlope && isFlying) target = 0.f;
        if (isFlying && p.isGoingLeft) target *= -1.f;
        if (p.isBird) {
            factor = 0.07f;
            if (p.isGoingLeft == p.isUpsideDown)
                target = std::max(-0.1f, target);
            else
                target = std::min(0.1f, target);
            if (p.isPlatformer) {
                target = 0.f;
            }
        } else if (p.isDart) {
            factor = (p.vehicleSize == 1.f) ? 0.25f : 0.4f;
        } else if (p.isPlatformer && p.isShip) {
            factor = 0.06f;
            target = 0.f;
        }
        if (p.isCollidingWithSlope && isFlying) factor *= 0.25f;
        float const t = std::min(dt, factor * dt);
        float const r = lerpAngle(current, target, t);
        p.rotation = r * 57.29578f;
        shrinkRotUnc(p, t);
    }

    //rotates the player for one step, called after collisions
    void updateRotation(Game& g, Player& p, float dt) {
        (void)g;
        if (p.rotUncertain && p.rotUnc <= 0.f) p.rotUncertain = false;
        if (p.isLocked || p.isDashing) {
        } else if (!p.isBall) {
            if (!p.isOnSlope) {
                if (!p.isShip && !p.isBird && !p.isDart && !p.isSwing) {
                    bool handled = false;
                    if (!p.isBird && !p.isDart && !p.isSwing && p.isOnGround2) {
                        double const yVelocity = p.yVelocity;
                        bool aboveRest;
                        bool atRest;
                        if (!p.isUpsideDown) {
                            atRest = yVelocity == -0.25;
                            aboveRest = -0.25 < yVelocity;
                        } else {
                            atRest = yVelocity == 0.25;
                            aboveRest = yVelocity < 0.25;
                        }
                        bool const falling = !aboveRest && !atRest;
                        if ((aboveRest || atRest) && p.wasOnSlope == falling) {
                            if (p.isPlatformer == falling || p.isRotating == falling) {
                                p.isRotating = false;
                                p.isBallRotating2 = false;
                                p.isBallRotating = false;
                                p.rotationSpeed = 0;
                                updateRotationTo(p, dt, convertToClosestRotation(p, 0.f));
                            }
                            handled = true;
                        }
                    }
                    if (!handled && (p.isRobot || p.isSpider) && !p.wasOnSlope) updateRotationTo(p, dt * 0.5f, 0.f);
                } else {
                    updateShipRotation(p, dt);
                }
            } else {
                float const slopeDegrees = static_cast<float>(p.slopeRotation * 57.295780181884766);
                updateRotationTo(p, dt, convertToClosestRotation(p, slopeDegrees));
            }
        }
        if (!p.isLocked && !p.isDashing && p.rotationSpeed != 0.f) {
            float speedMultiplier = 1.f;
            if (p.isBallRotating && p.isBall) speedMultiplier = p.rotateSpeed;
            p.rotation = p.rotation + (dt / 60.f) * p.rotationSpeed * speedMultiplier;
        }
        p.shipRotX = p.x;
        p.shipRotY = p.y;
    }

    // ball spin factor for each speed portal, values from PlayerObject::runBallRotation
    static float ballSpeedFactor(float playerSpeed) {
        if (playerSpeed == 0.7f) return 1.2405638f;
        if (playerSpeed == 1.1f) return 0.80424345f;
        if (playerSpeed == 1.3f) return 0.6657693f;
        if (playerSpeed == 1.6f) return 0.5409375f;
        return 1.f;
    }

    // starts the ball rolling on the ground, speed is a multiplier (1 is normal)
    void runBallRotation(Player& p, float speed) {
        if (p.isDashing || p.isPlatformer) return;
        p.isBallRotating = speed != 1.f;
        p.rotateSpeed = speed;
        float const sizeFactor = (p.vehicleSize == 1.f) ? 1.f : 0.8f;
        float const speedFactor = ballSpeedFactor(p.playerSpeed);
        int const baseSpeed = p.isUpsideDown ? -0x78 : 0x78;
        int const direction = p.isGoingLeft ? -1 : 1;
        float r = static_cast<float>(baseSpeed * direction) / (sizeFactor * 0.2f * speedFactor);
        p.rotationSpeed = r;
        if (p.isSideways) p.rotationSpeed = r * -1.f;
        p.isRotating = true;
    }

    // starts the ball air spin (gravity flip, orbs, pads), same as PlayerObject::runBallRotation2
    void runBallRotation2(Player& p) {
        if (p.isDashing) return;
        float const sizeFactor = (p.vehicleSize == 1.f) ? 1.f : 0.8f;
        float const speedFactor = ballSpeedFactor(p.playerSpeed);
        int const baseSpeed = p.isUpsideDown ? 0x154 : -0x154;
        p.isBallRotating2 = true;
        int const direction = p.isGoingLeft ? -1 : 1;
        p.rotationSpeed =
                (static_cast<float>(baseSpeed * direction) * p.gravityMod) / (sizeFactor * 0.8f * speedFactor);
    }

}
