// Player against solid blocks, ported from PlayerObject::collidedWithObjectInternal and related functions.

#include "gd.hpp"

#include <algorithm>
#include <cmath>

namespace ap::sim {

    // player box scaled by scaleW / scaleH, centered on the player
    static Rect playerRectWH(Player const& p, float scaleW, float scaleH, float baseWidth, float baseHeight) {
        float width = std::fabs(p.scaleX) * baseWidth * scaleW;
        float height = std::fabs(p.scaleY) * baseHeight * scaleH;
        return Rect{p.x - width * 0.5f, p.y - height * 0.5f, width, height};
    }

    // copies the current size into the box cache when the player has moved
    void refreshPlayerRect(Player const& p) {
        if (p.rectDirty) {
            p.rectScaleW = p.spriteWidthScale;
            p.rectScaleH = p.spriteHeightScale;
            p.rectWidth = p.width;
            p.rectHeight = p.height;
            p.rectDirty = false;
        }
    }

    // normal player hitbox, uses the cached size like the game does
    Rect playerRect(Player const& p) {
        refreshPlayerRect(p);
        return playerRectWH(p, p.rectScaleW, p.rectScaleH, p.rectWidth, p.rectHeight);
    }

    // inner hitbox (30% of the base size), the player dies if a block touches it
    Rect playerInnerRect(Player const& p) { return playerRectWH(p, 0.3f, 0.3f, p.width, p.height); }

    // moves the player and marks its box as dirty
    static void setPosition(Player& p, float x, float y) {
        p.x = x;
        p.y = y;
        playerMoved(p);
    }

    // 1 normally, -1 upside down
    static inline float sgnF(bool upsideDown) { return upsideDown ? -1.f : 1.f; }

    // records a touched edge, direction is 0 top, 1 bottom, 2 left, 3 right
    void updateCollide(Player& p, int direction, GObj object) {
        Rect box = playerRect(p);
        float halfH = box.h * 0.5f;
        float halfW = box.w * 0.5f;
        int uid = object ? object.uid() : 0;
        if (direction == 0) {
            float gravitySign = sgnF(p.isUpsideDown);
            double current = p.collidedTopMinY;
            double edgeY = static_cast<double>(gravitySign * halfH + p.y);
            double result = edgeY;
            if (current != 0.0) {
                result = current;
                if (!p.isUpsideDown) {
                    if (edgeY <= current) result = edgeY;
                } else if (current <= edgeY)
                    result = edgeY;
            }
            p.collidedTopMinY = result;
            if (uid == 0 || uid == p.lastCollisionTop) return;
            p.lastCollisionTop = uid;
            p.collisionLogTop.push_back(uid);
        } else if (direction == 1) {
            float gravitySign = sgnF(p.isUpsideDown);
            double current = p.collidedBottomMaxY;
            double edgeY = static_cast<double>(p.y - gravitySign * halfH);
            double result = edgeY;
            if (current != 0.0) {
                result = current;
                if (!p.isUpsideDown) {
                    if (current <= edgeY) result = edgeY;
                } else if (edgeY <= current)
                    result = edgeY;
            }
            p.collidedBottomMaxY = result;
            if (uid == 0 || uid == p.lastCollisionBottom) return;
            p.lastCollisionBottom = uid;
            p.collisionLogBottom.push_back(uid);
        } else if (direction == 2) {
            p.collidedLeftMaxX = static_cast<double>(p.x - halfW);
            if (uid) {
                p.lastCollisionLeft = uid;
                p.collisionLogLeft.push_back(uid);
            }
        } else if (direction == 3) {
            p.collidedRightMinX = static_cast<double>(halfW + p.x);
            if (uid) {
                p.lastCollisionRight = uid;
                p.collisionLogRight.push_back(uid);
            }
        }
    }

    // keeps the biggest value, 0 means nothing recorded yet
    static void noteMax(double& cur, double value) {
        if (cur != 0.0) {
            if (cur <= value) cur = value;
        } else
            cur = value;
    }

    // keeps the smallest value, 0 means nothing recorded yet
    static void noteMin(double& cur, double value) {
        if (cur != 0.0) {
            if (value <= cur) cur = value;
        } else
            cur = value;
    }

    // adds an object to the list of objects touched from below
    static void logBottom(Player& p, int uid) {
        if (uid != 0 && uid != p.lastCollisionBottom) {
            p.lastCollisionBottom = uid;
            p.collisionLogBottom.push_back(uid);
        }
    }

    // adds an object to the list of objects touched from above
    static void logTop(Player& p, int uid) {
        if (uid != 0 && uid != p.lastCollisionTop) {
            p.lastCollisionTop = uid;
            p.collisionLogTop.push_back(uid);
        }
    }

    // remembers the ground object and applies its no boost options
    static void setLastGround(Player& p, GObj object) {
        if (object) {
            p.lastGroundObject = object.uid();
            if (object.o->dontBoostY) p.stateBoostX = 2;
            if (object.o->dontBoostX) p.stateBoostY = 2;
        }
    }

    // destroys a breakable block and fires the DestroyBlock event
    static void breakObject(Game& g, GObj object) {
        if (object) object.mut().destroyed = true;
        if (object && object.getType() == Breakable) trigGameEvent(g, GameEvent::DestroyBlock);
    }

    // player lands on something, same as PlayerObject::hitGround
    void hitGround(Game& g, Player& p, GObj object, bool notFlipped) {
        p.groundObjectMaterial = object ? object.o->material : 0;
        bool const upsideDown = p.isUpsideDown;
        double const velocity = p.yVelocity;
        p.isOnIce = false;
        int const sign = upsideDown ? -1 : 1;
        if (!p.isDart) {
            int const material = object ? object.o->material : 0;
            double const fallVelocity = upsideDown ? -velocity : velocity;
            int landEvent = GameEvent::None;
            if (fallVelocity < -14.0)
                landEvent = GameEvent::HardLanding;
            else if (fallVelocity < -8.0)
                landEvent = GameEvent::NormalLanding;
            else if (fallVelocity < -4.0)
                landEvent = GameEvent::SoftLanding;
            else if (fallVelocity < -1.0)
                landEvent = p.isOnGround2 ? GameEvent::TinyLanding : GameEvent::FeatherLanding;
            if (landEvent) trigGameEvent(g, landEvent, material);
            if (p.fallStartY != 0.f) {
                float const fallDistance = std::fabs(p.fallStartY - p.y);
                int const fallEvent = fallDistance > 450.f   ? GameEvent::FallVHigh
                                      : fallDistance > 300.f ? GameEvent::FallHigh
                                      : fallDistance > 150.f ? GameEvent::FallMed
                                      : fallDistance > 7.5f  ? GameEvent::FallLow
                                                             : GameEvent::None;
                if (fallEvent) trigGameEvent(g, fallEvent, material);
            }
            p.fallStartY = 0.f;
        }
        p.yVelocity = 0;
        p.isSliding = false;
        if (!p.justPlacedStreak) p.justPlacedStreak = true;
        p.isOnGround2 = true;
        p.unkA29 = !notFlipped;
        if (static_cast<double>(sign) * velocity <= 5.0) {
            p.lastLandTime = p.totalTime;
            p.isOnGround = true;
        }
        p.isOnGround3 = true;
        p.stateOnGround = 2;
        if (!p.isBall) {
            if (p.isRotating || p.wasOnSlope) {
                p.isRotating = false;
                p.isBallRotating2 = false;
                p.isBallRotating = false;
                p.rotationSpeed = 0;
            }
        } else {
            if (p.isBallRotating && !p.isOnSlope) {
                p.isRotating = false;
                p.isBallRotating2 = false;
                p.isBallRotating = false;
                p.rotationSpeed = 0;
            }
            if (p.isBallRotating2) {
                p.isRotating = false;
                p.isBallRotating2 = false;
                p.isBallRotating = false;
                p.rotationSpeed = 0;
            }
            if (!p.isRotating && !p.isLocked && !p.isDashing) {
                p.isRotating = false;
                p.isBallRotating2 = false;
                p.isBallRotating = false;
                p.rotationSpeed = 0;
                runBallRotation(p, 1.f);
            }
        }
        if (!p.isBall && !p.isShip && !p.isBird && !p.isDart && !p.isSwing && !p.isOnSlope && !p.wasOnSlope) {
            int const angle = static_cast<int>(p.rotation);
            if (angle != (angle / 90) * 90) {
                p.isRotating = false;
                p.isBallRotating2 = false;
                p.isBallRotating = false;
                p.rotationSpeed = 0;
            }
        }
        (void)object;
        p.lastGroundedX = p.x;
        p.lastGroundedY = p.y;
        if (!p.isShip && !p.isBird && !p.isDart && !p.isSwing && p.shouldTryPlacingCheckpoint) {
            p.canPlaceCheckpoint = true;
        }
        p.shouldTryPlacingCheckpoint = false;
    }

    // like hitGround but the player can't jump from it (head side hit)
    void hitGroundNoJump(Game& g, Player& p, GObj object, bool notFlipped) {
        bool const onGround = p.isOnGround;
        bool const onGround2 = p.isOnGround2;
        double const landTime = p.lastLandTime;
        hitGround(g, p, GObj{}, notFlipped);
        (void)object;
        p.lastLandTime = landTime;
        p.isOnGround2 = onGround2;
        p.isOnGround = onGround;
    }

    // flips gravity after hitting a flip gravity block with the head
    static void didHitHead(Player& p) {
        if (p.stateFlipGravity <= 0) return;
        flipGravity(p, !p.isUpsideDown, true);
        setYVelocity(p, p.isUpsideDown ? 2.0 : -2.0);
        p.maybeIsBoosted = true;
        p.isOnGround2 = false;
        if (0 < p.stateNoAutoJump) {
            p.isOnGround = false;
            p.jumpBuffered = false;
            p.stateRingJump = false;
        }
    }

    // nudges the cube on x when it jumps between blocks at a usual jump distance
    void checkSnapJumpToObject(Game& g, Player& p, GObj object) {
        if (!object) return;
        GObj prev = p.objectSnappedTo ? g.objByUid(p.objectSnappedTo) : GObj{};
        if (prev && prev.uid() != object.uid() && prev.getType() == Solid) {
            Point prevPos = prev.getRealPosition();
            Point objectPos = object.getRealPosition();
            float const speed = p.playerSpeed;
            float upTwoDx = 90.f;
            float upOneDx = 120.f;
            float upTwoDxAlt = 90.f;
            float downOneDx = 150.f;
            float matchTolerance = 1.f;
            bool useAltUpTwo = false;
            bool miniGaps = false;
            if (speed == 0.9f)
                useAltUpTwo = true;
            else if (speed == 0.7f) {
                upTwoDxAlt = 60.f;
                downOneDx = 120.f;
                miniGaps = true;
            } else if (speed == 1.1f) {
                upOneDx = 150.f;
                upTwoDxAlt = 120.f;
                downOneDx = 195.f;
                matchTolerance = 2.f;
                useAltUpTwo = true;
            } else if (speed == 1.3f) {
                upOneDx = 180.f;
                downOneDx = 225.f;
                upTwoDxAlt = 135.f;
                matchTolerance = 2.f;
                useAltUpTwo = true;
            }
            if (useAltUpTwo) {
                upTwoDx = upTwoDxAlt;
                if (p.vehicleSize != 1.f) miniGaps = true;
            }
            if (miniGaps) {
                upOneDx = 90.f;
                upTwoDx = upTwoDxAlt;
            }
            float const gravitySign = p.isUpsideDown ? -1.f : 1.f;
            bool snap = false;
            if (matchTolerance < std::fabs(objectPos.x - (prevPos.x + upOneDx))) {
                if (std::fabs(objectPos.x - (prevPos.x + downOneDx)) <= matchTolerance &&
                    std::fabs(objectPos.y - (prevPos.y - gravitySign * 30.f)) <= matchTolerance)
                    snap = true;
                else if (std::fabs(objectPos.x - (prevPos.x + upTwoDx)) <= matchTolerance &&
                         std::fabs(objectPos.y - (gravitySign * 60.f + prevPos.y)) <= matchTolerance)
                    snap = true;
            } else {
                if (std::fabs(objectPos.y - (gravitySign * 30.f + prevPos.y)) <= matchTolerance)
                    snap = true;
                else if (std::fabs(objectPos.x - (prevPos.x + downOneDx)) <= matchTolerance &&
                         std::fabs(objectPos.y - (prevPos.y - gravitySign * 30.f)) <= matchTolerance)
                    snap = true;
                else if (std::fabs(objectPos.x - (prevPos.x + upTwoDx)) <= matchTolerance &&
                         std::fabs(objectPos.y - (gravitySign * 60.f + prevPos.y)) <= matchTolerance)
                    snap = true;
            }
            if (snap) {
                double target = static_cast<double>(object.getPosition().x) + p.snapDistance;
                double const playerX = static_cast<double>(p.x);
                double const tolerance = static_cast<double>(matchTolerance);
                if (tolerance < std::fabs(target - playerX)) {
                    target = (target <= playerX) ? playerX - tolerance : tolerance + playerX;
                }
                setPosition(p, static_cast<float>(target), p.y);
            }
        }
        p.objectSnappedTo = object.uid();
        p.snapDistance = static_cast<double>(p.x - object.getPosition().x);
    }

    // box of an object turned by rotateGameplayObject
    Rect rotatedObjectRect(GObj const& o, Game::RotOv const& ov) {
        (void)ov;
        if (o.hasOrientedBox()) return objectObb(o).boundingRect();
        if (o.o->rectGen) return o.getObjectRectWH(o.o->sws, o.o->shs);
        int const savedCount = o.g->rotOvN;
        o.g->rotOvN = 0;
        Rect const rect = o.getObjectRect();
        Point const basePos = o.getRealPosition();
        bool const wasAligned = o.rotationAligned();
        o.g->rotOvN = savedCount;
        Point const rotatedPos = o.getRealPosition();
        bool const isAligned = o.rotationAligned();
        float const centerOffX = rect.x + rect.w * 0.5f - basePos.x;
        float const centerOffY = rect.y + rect.h * 0.5f - basePos.y;
        float width = rect.w;
        float height = rect.h;
        if (wasAligned != isAligned) std::swap(width, height);
        float const centerX = rotatedPos.x + centerOffX;
        float const centerY = rotatedPos.y + centerOffY;
        return Rect{centerX - width * 0.5f, centerY - height * 0.5f, width, height};
    }

    static thread_local Game::RotOv tl_rotOv[64];

    // turns an object 90 degrees around the player (sideways gameplay)
    void rotateGameplayObject(Game& g, Player& p, GObj object) {
        if (g.rotOvN == 0) g.rotOv = tl_rotOv;
        if (!object || g.rotOvFind(object.index) || g.rotOvN >= 64) return;
        Point const pos = object.getRealPosition();
        float const sinA = gsin(1.5707964f);
        float const cosA = gcos(1.5707964f);
        float const offX = pos.x - p.x;
        float const offY = pos.y - p.y;
        float const rotatedX = (cosA * offX - sinA * offY) + p.x;
        float const rotatedY = cosA * offY + sinA * offX + p.y;
        g.rotOv[g.rotOvN++] =
                Game::RotOv{object.index, static_cast<double>(rotatedX - pos.x), static_cast<double>(rotatedY - pos.y)};
    }

    // runs a collision in the rotated frame then turns the player move back
    bool handleRotatedCollisionInternal(Game& g, Player& p, float dt, GObj object, Rect rect, bool skipCheck,
                                        bool skipPre, bool slope) {
        float const startX = p.x;
        float const startY = p.y;
        int const savedCount = g.rotOvN;
        Game::RotOv* const savedTable = g.rotOv;
        rotateGameplayObject(g, p, object);
        for (int slopeUid : p.potentialSlopes) rotateGameplayObject(g, p, g.objByUid(slopeUid));
        bool result = false;
        if (slope)
            collidedWithSlopeInternal(g, p, dt, object, skipPre);
        else
            result = collidedWithObjectInternal(g, p, dt, object, rect, skipCheck);
        float const endX = p.x;
        float const endY = p.y;
        setPosition(p, (endY - startY) + startX, startY - (endX - startX));
        g.rotOvN = savedCount;
        g.rotOv = savedTable;
        return result;
    }

    // player touches a solid object, goes through the rotated version when sideways
    bool collidedWithObject(Game& g, Player& p, float dt, GObj object, Rect rect, bool skipCheck) {
        if (p.isSideways) return handleRotatedCollisionInternal(g, p, dt, object, rect, skipCheck, false, false);
        return collidedWithObjectInternal(g, p, dt, object, rect, skipCheck);
    }

    // lands on, bumps into or dies from a solid object, returns true if handled
    bool collidedWithObjectInternal(Game& g, Player& self, float dt, GObj object, Rect rect, bool skipCheck) {
        (void)dt;
        (void)skipCheck;
        Rect const playerBox = playerRect(self);
        GObj sourceObject;
        if (rectEquals(rect, zeroRect())) {
            sourceObject = object;
            if (object) rect = object.getObjectRect();
        }
        Rect const objectBox = rect;
        float landTolerance = (!self.isPlatformer || self.wasOnSlope || self.isOnSlope) ? 10.f : 5.f;
        double baseTolerance = static_cast<double>(landTolerance);
        if (0 < self.stateScale) baseTolerance = 15.0;
        bool const upsideDown = self.isUpsideDown;
        double tolerance = static_cast<double>(upsideDown ? -1 : 1) * baseTolerance;
        if ((self.isShip || self.isBird || self.isDart || self.isSwing) && !self.isPlatformer) {
            tolerance = static_cast<double>(static_cast<float>(upsideDown ? -1 : 1) * 6.f);
        }
        if (self.wasOnSlope) tolerance = tolerance + static_cast<double>(upsideDown ? -1 : 1) * self.unk_584;
        bool const passable = sourceObject ? sourceObject.o->passable : false;
        bool fastMover = false;
        bool objectMovingUp = false;
        bool objectMovingDown = false;
        float objectVelocity = 0.f;
        if (object && 0.f < dt) {
            Point const last = object.lastPosition();
            Point const real = object.getRealPosition();
            if (last.x != real.x || last.y != real.y) {
                float lastAxisPos = self.isSideways ? last.x : last.y;
                float const realAxisPos = self.isSideways ? real.x : real.y;
                {
                    float scaleDx;
                    float scaleDy;
                    trigScalePosDelta(g, object.index, scaleDx, scaleDy);
                    float const scaleDelta = self.isSideways ? scaleDx : scaleDy;
                    if (scaleDelta != 0.f) {
                        float const playerAxisPos = self.isSideways ? self.x : self.y;
                        if (realAxisPos <= playerAxisPos)
                            lastAxisPos -= scaleDelta;
                        else
                            lastAxisPos += scaleDelta;
                    }
                }
                bool skip = false;
                if (!self.isUpsideDown) {
                    if (realAxisPos <= lastAxisPos) {
                        if (realAxisPos != lastAxisPos) objectMovingDown = true;
                        skip = true;
                    }
                } else {
                    if (lastAxisPos <= realAxisPos) {
                        if (realAxisPos != lastAxisPos) objectMovingDown = true;
                        skip = true;
                    }
                }
                if (!skip) {
                    objectVelocity = (realAxisPos - lastAxisPos) / dt;
                    fastMover = 5.0 < static_cast<double>(std::fabs(objectVelocity));
                    tolerance = tolerance + static_cast<double>(self.isUpsideDown ? -1 : 1) *
                                                    static_cast<double>(std::fabs(objectVelocity));
                    if (!self.isPlatformer) self.groundYVelocity = static_cast<double>(objectVelocity);
                    objectMovingUp = true;
                }
            }
        }

        bool const headAllowed =
                !((self.stateHitHead < 1 && !self.isPlatformer && self.stateFlipGravity < 1) || passable);

        float moveDelta = self.isSideways ? self.x - self.lastX : self.y - self.lastY;
        double const deltaY = static_cast<double>(moveDelta);
        float const halfH = playerBox.h * 0.5f;
        double const bottomEdge =
                static_cast<double>(self.y - halfH * static_cast<float>(upsideDown ? -1 : 1)) + tolerance;
        double const prevBottomEdge = bottomEdge - deltaY;
        double const topEdge =
                static_cast<double>(self.y - static_cast<float>(upsideDown ? 1 : -1) * halfH) - tolerance;
        bool movingAgainstGravity = upsideDown ? (deltaY < 0.0) : (deltaY > 0.0);

        double const objectMaxY = static_cast<double>(getMaxY(objectBox));
        float const objectMinYF = getMinY(objectBox);
        double const objectMinY = static_cast<double>(objectMinYF);
        bool headSide = false;
        bool checkInner = true;
        bool nearSlope = false;
        bool ceilingCase = false;
        bool aboveSlope = false;
        bool floorSide = false;
        bool const isBreakable = sourceObject && sourceObject.getType() == Breakable;
        bool const isCube = !(self.isShip || self.isBird || self.isDart || self.isSwing || self.isBall ||
                              self.isRobot || self.isSpider);
        bool const isFlying = self.isShip || self.isBird || self.isDart || self.isSwing;

        auto noteAfterLanding = [&](bool ceiling) {
            int const sourceUid = sourceObject ? sourceObject.uid() : 0;
            if (!self.isUpsideDown) {
                if (!ceiling) {
                    noteMax(self.collidedBottomMaxY, objectMaxY);
                    logBottom(self, sourceUid);
                    setLastGround(self, object);
                    self.groundYVelocity = static_cast<double>(objectVelocity);
                } else if (!isBreakable && !passable) {
                    noteMin(self.collidedTopMinY, static_cast<double>(objectMinYF));
                    logTop(self, sourceUid);
                }
            } else {
                if (ceiling) {
                    if (!isBreakable && !passable) {
                        noteMax(self.collidedTopMinY, objectMaxY);
                        logTop(self, sourceUid);
                    }
                } else {
                    noteMin(self.collidedBottomMaxY, static_cast<double>(objectMinYF));
                    logBottom(self, sourceUid);
                    setLastGround(self, object);
                    self.groundYVelocity = static_cast<double>(objectVelocity);
                }
            }
        };

        if (g_trace)
            std::printf(
                    "      [trace] collision uid %d : object y [%.3f %.3f] player y %.3f lastY %.3f halfH %.2f "
                    "bottomEdge %.3f prevBottomEdge %.3f tolerance %.3f yv %.3f upside down %d headAllowed %d (hitHead "
                    "%d flip %d) objectMovingUp %d objectMovingDown %d cube %d\n",
                    object ? object.uid() : 0, objectMinY, objectMaxY, self.y, self.lastY, halfH, bottomEdge,
                    prevBottomEdge, tolerance, self.yVelocity, upsideDown, headAllowed, self.stateHitHead,
                    self.stateFlipGravity, objectMovingUp, objectMovingDown, isCube);
        bool doFinal = true;
        if (self.isDart && self.stateDartSlide < 1) {
        } else {
            if (self.isOnSlope || self.wasOnSlope || !self.potentialSlopes.empty()) {
                bool const testAllSlopes = !(!self.isOnSlope && !self.wasOnSlope && sourceObject);
                bool slopeCovers = false;
                for (int slopeUid : self.potentialSlopes) {
                    GObj slope = g.objByUid(slopeUid);
                    if (!slope) continue;
                    bool test = true;
                    if (!testAllSlopes && slope.uid() != self.currentSlope) {
                        Point objectPos = sourceObject.getPosition();
                        Rect slopeBox = slope.getObjectRect();
                        test = (getMinX(slopeBox) < objectPos.x) && (objectPos.x < getMaxX(slopeBox));
                        slopeCovers = ceilingCase;
                    }
                    if (test) {
                        if (intersectsRect(objectBox, slope.getObjectRectWH(1.2f, 1.1f)) &&
                            intersectsRect(playerBox, slope.getObjectRectWH(1.2f, 1.1f))) {
                            nearSlope = true;
                            int const direction = slope.slopeDir();
                            bool const leftSide =
                                    (direction == 1) ||
                                    (((static_cast<unsigned>(direction) - 3u) & 0xfffffffcu) == 0 && direction != 4);
                            float const testX =
                                    (leftSide != slope.slopeUphill()) ? getMinX(objectBox) : getMaxX(objectBox);
                            double const slopeY = slopeYPos(slope, testX);
                            if (objectMaxY <= slopeY + 2.0 && (!leftSide || self.currentSlope == slope.uid())) {
                                slopeCovers = true;
                                ceilingCase = true;
                            }
                            if (slopeY - 2.0 <= objectMinY && (leftSide || self.currentSlope == slope.uid())) {
                                aboveSlope = true;
                            }
                        }
                    }
                    if (slopeCovers && aboveSlope) break;
                }
            }
            if (!self.isUpsideDown ||
                ((self.isShip || self.isBird || self.isDart || self.isSwing || self.isBall) && !passable)) {
                floorSide = true;
                if (self.isUpsideDown)
                    headSide = (objectMaxY <= bottomEdge || objectMaxY <= prevBottomEdge);
                else
                    headSide = !(objectMinY < bottomEdge && objectMinY < prevBottomEdge);
            } else {
                floorSide = false;
                headSide = (objectMaxY <= bottomEdge || objectMaxY <= prevBottomEdge);
            }
            movingAgainstGravity = false;
            (void)movingAgainstGravity;

            bool ceilingPath = false;
            if (floorSide || headAllowed) {
                double prevFloorEdge = prevBottomEdge;
                double floorEdge = bottomEdge;
                if (upsideDown) {
                    prevFloorEdge = topEdge - deltaY;
                    floorEdge = topEdge;
                }
                if (floorEdge < objectMaxY && prevFloorEdge < objectMaxY) {
                    ceilingCase = true;
                    ceilingPath = true;
                } else if (!upsideDown || !(self.isBird && isBreakable) || self.isPlatformer) {
                    double const yv = self.yVelocity;
                    if ((0.0 < yv && !self.wasOnSlope && !fastMover) || (nearSlope && ceilingCase)) {
                        if (!upsideDown) {
                            if (!headSide && objectMovingUp) {
                                double const savedYVelocity = self.yVelocity;
                                setPosition(self, self.x, static_cast<float>(static_cast<double>(halfH) + objectMaxY));
                                hitGround(g, self, sourceObject, self.isUpsideDown && isFlying);
                                noteAfterLanding(false);
                                setYVelocity(self, savedYVelocity);
                                if (self.yVelocity <= self.groundYVelocity) setYVelocity(self, self.groundYVelocity);
                            }
                        } else if (headSide && objectMovingDown) {
                            double const savedYVelocity = self.yVelocity;
                            setPosition(self, self.x, static_cast<float>(static_cast<double>(halfH) + objectMaxY));
                            hitGroundNoJump(g, self, sourceObject, self.isUpsideDown && isFlying);
                            if (!isBreakable && !passable) {
                                noteMax(self.collidedTopMinY, objectMaxY);
                                logTop(self, sourceObject ? sourceObject.uid() : 0);
                            }
                            setYVelocity(self, static_cast<double>(static_cast<float>(savedYVelocity)));
                            if (self.yVelocity <= self.groundYVelocity + 1.0) {
                                setYVelocity(self, self.groundYVelocity + 1.0);
                            }
                            didHitHead(self);
                        }
                        ceilingCase = false;
                        checkInner = false;
                        ceilingPath = true;
                    } else {
                        if (headSide && 0.0 < (self.isUpsideDown ? -1.0 : 1.0) * self.yVelocity)
                            trigGameEvent(g, GameEvent::HitHead);
                        setPosition(self, self.x, static_cast<float>(static_cast<double>(halfH) + objectMaxY));
                        if (isCube) checkSnapJumpToObject(g, self, sourceObject);
                        if (!headAllowed || floorSide) {
                            bool const notFlipped = self.isUpsideDown && isFlying;
                            if (!headSide)
                                hitGround(g, self, sourceObject, notFlipped);
                            else
                                hitGroundNoJump(g, self, sourceObject, notFlipped);
                        } else {
                            self.yVelocity = 0;
                        }
                        noteAfterLanding(headSide);
                        if (isCube && 0 < self.stateNoAutoJump && self.padRingRelated) self.jumpBuffered = false;
                        if (isFlying && objectMovingUp && headSide) setYVelocity(self, self.groundYVelocity);
                        if (headSide) didHitHead(self);
                        return true;
                    }
                } else {
                    if (intersectsRect(objectBox, playerInnerRect(self))) {
                        breakObject(g, sourceObject);
                        return true;
                    }
                    ceilingCase = false;
                    checkInner = false;
                    ceilingPath = true;
                }
            } else {
                ceilingCase = true;
                ceilingPath = true;
            }
            if (ceilingPath) {
                if (!self.isUpsideDown &&
                    ((!self.isShip && !self.isBird && !self.isDart && !self.isSwing && !self.isBall) || passable)) {
                    floorSide = false;
                    if (!headAllowed) {
                        doFinal = true;
                        goto FINAL;
                    }
                } else {
                    floorSide = true;
                }
                if (ceilingCase) {
                    double headEdge = topEdge;
                    double prevHeadEdge = topEdge - deltaY;
                    if (upsideDown) {
                        headEdge = bottomEdge;
                        prevHeadEdge = prevBottomEdge;
                    }
                    int const objectUid = sourceObject ? sourceObject.uid() : 0;
                    bool headBelowObject = false;
                    bool repeatUpsideDownHit = false;
                    if (headEdge <= objectMinY || prevHeadEdge <= objectMinY)
                        headBelowObject = true;
                    else if (!upsideDown) {
                        if (objectUid == self.unk510) headBelowObject = true;
                    } else if (objectUid == self.unk50C)
                        repeatUpsideDownHit = true;
                    if (headBelowObject || repeatUpsideDownHit) {
                        bool handled = false;
                        if (headBelowObject && !upsideDown && self.isBird && isBreakable && !self.isPlatformer) {
                            if (intersectsRect(objectBox, playerInnerRect(self))) {
                                breakObject(g, sourceObject);
                                return true;
                            }
                            handled = true;
                        }
                        if (!handled) {
                            if ((0.0 <= self.yVelocity || (self.isPlatformer && 0.0 < self.yVelocityRelated3) ||
                                 self.wasOnSlope || fastMover) &&
                                (!nearSlope || !aboveSlope)) {
                                if (headSide && 0.0 < (self.isUpsideDown ? -1.0 : 1.0) * self.yVelocity) {
                                    trigGameEvent(g, GameEvent::HitHead);
                                }
                                setPosition(self, self.x, static_cast<float>(objectMinY - static_cast<double>(halfH)));
                                if (isCube) checkSnapJumpToObject(g, self, sourceObject);
                                if (!headAllowed || floorSide) {
                                    bool const notFlipped = !self.isUpsideDown && isFlying;
                                    if (!headSide)
                                        hitGround(g, self, sourceObject, notFlipped);
                                    else
                                        hitGroundNoJump(g, self, sourceObject, notFlipped);
                                } else {
                                    self.yVelocity = 0;
                                }
                                noteAfterLanding(headSide);
                                if (isCube && 0 < self.stateNoAutoJump && self.padRingRelated) {
                                    self.jumpBuffered = false;
                                }
                                if (isFlying && objectMovingUp && headSide) {
                                    self.yVelocity = gdRound3(self.groundYVelocity);
                                }
                                if (headSide) didHitHead(self);
                                return true;
                            }
                            if (!self.isUpsideDown) {
                                if (headSide && objectMovingDown) {
                                    double const savedYVelocity = self.yVelocity;
                                    setPosition(self, self.x,
                                                static_cast<float>(objectMinY - static_cast<double>(halfH)));
                                    hitGroundNoJump(g, self, sourceObject, self.isUpsideDown && isFlying);
                                    if (!isBreakable && !passable) {
                                        noteMin(self.collidedTopMinY, objectMinY);
                                        logTop(self, sourceObject ? sourceObject.uid() : 0);
                                    }
                                    setYVelocity(self, savedYVelocity);
                                    if (self.groundYVelocity - 1.0 <= self.yVelocity) {
                                        setYVelocity(self, self.groundYVelocity - 1.0);
                                    }
                                    didHitHead(self);
                                }
                            } else if (!headSide && objectMovingUp) {
                                double const savedYVelocity = self.yVelocity;
                                setPosition(self, self.x, static_cast<float>(objectMinY - static_cast<double>(halfH)));
                                hitGround(g, self, sourceObject, self.isUpsideDown && isFlying);
                                noteAfterLanding(false);
                                setYVelocity(self, savedYVelocity);
                                if (self.groundYVelocity <= self.yVelocity) setYVelocity(self, self.groundYVelocity);
                                checkInner = false;
                                goto FINAL;
                            }
                        }
                        checkInner = false;
                    }
                }
            }
        }
    FINAL:
        (void)doFinal;
        if (g_trace) {
            Rect const in = playerInnerRect(self);
            std::printf(
                    "      [trace] final : checkInner %d inner [%.3f %.3f %.3f %.3f] object [%.3f %.3f %.3f %.3f] y "
                    "%.3f\n",
                    checkInner, in.x, in.y, in.w, in.h, objectBox.x, objectBox.y, objectBox.w, objectBox.h, self.y);
        }
        if (checkInner && !self.isPlatformer && !passable) {
            Rect const inner = playerInnerRect(self);
            if (intersectsRect(objectBox, inner)) {
                if (!headSide || self.lastFlipTime == 0.0 ||
                    (g.timingMargin ? 0.10000000149011612 - 2e-6 : 0.10000000149011612) <=
                            self.totalTime - self.lastFlipTime) {
                    if (self.isSpider) {
                        if (getMaxX(objectBox) < getMaxX(inner) && self.lastSpiderFlipTime != 0.0 &&
                            self.totalTime - self.lastSpiderFlipTime < 0.03999999910593033) {
                            return true;
                        }
                    }
                    if (sourceObject && sourceObject.getType() == Breakable) {
                        breakObject(g, sourceObject);
                        return true;
                    }
                    destroyPlayer(g, self, sourceObject ? sourceObject : object);
                    return true;
                }
                double const newY = !self.isUpsideDown ? objectMinY - static_cast<double>(halfH)
                                                       : static_cast<double>(halfH) + objectMaxY;
                setPosition(self, self.x, static_cast<float>(newY));
                hitGround(g, self, sourceObject, true);
                self.isOnGround2 = false;
                return true;
            }
        }
        return false;
    }

    // end of tick: slope exit, squeeze, moving grounds and fall events
    void postCollision(Game& g, Player& self, float dt, bool betweenSteps) {
        float impulseScale = dt * 4.f;
        if (!self.enableImpulseFix) impulseScale = betweenSteps ? 0.5f : 1.f;
        (void)impulseScale;
        bool const holdingLeft = false;
        bool const holdingRight = false;
        (void)holdingLeft;
        (void)holdingRight;

        GObj lastGround = self.lastGroundObject ? g.objByUid(self.lastGroundObject) : GObj{};
        if (lastGround && lastGround.disabled()) {
            self.lastGroundObject = 0;
            lastGround = GObj{};
        }
        if (self.currentSlope2) {
            GObj slope2 = g.objByUid(self.currentSlope2);
            if (slope2 && slope2.disabled()) self.currentSlope2 = 0;
        }
        if (lastGround) self.isOnIce = lastGround.o->iceBlock;
        if (self.isOnSlope && self.currentSlope && !self.maybeUpsideDownSlope) {
            GObj slope = g.objByUid(self.currentSlope);
            if (slope) self.isOnIce = slope.o->iceBlock;
        }

        if (self.isOnSlope != self.wasOnSlope) {
            if (!self.isOnSlope) {
                self.slopeEndTime = self.totalTime;
                double const slopeSpeed = static_cast<double>(self.slopeVelocity);
                double const yv = self.yVelocity;
                bool slopeBoost = (slopeSpeed < 0.0 && slopeSpeed < yv && self.isCurrentSlopeTop) ||
                                  (0.0 < slopeSpeed && yv < slopeSpeed && !self.isCurrentSlopeTop);
                if ((!self.maybeIsBoosted || self.isShip || self.isBird || self.isDart || self.isSwing ||
                     self.isBall) &&
                    slopeBoost) {
                    double slopeTime = self.totalTime - self.slopeStartTime;
                    double slopeFactor = 1.0;
                    if (slopeTime < 0.10000000149011612) {
                        slopeTime = slopeTime * 10.0;
                        slopeFactor = 0.4000000059604645;
                        if (0.4000000059604645 <= slopeTime) slopeFactor = slopeTime;
                    }
                    float const slopeBoostVelocity =
                            static_cast<float>(static_cast<double>(self.slopeVelocity) * slopeFactor);
                    bool isBelow;
                    bool isEqual;
                    bool const swappedCompare = !self.isUpsideDown
                                                        ? (self.isCurrentSlopeTop && self.slopeFlipGravityRelated)
                                                        : (self.isCurrentSlopeTop || !self.slopeFlipGravityRelated);
                    if (!swappedCompare) {
                        isEqual = static_cast<double>(slopeBoostVelocity) == self.yVelocity;
                        isBelow = static_cast<double>(slopeBoostVelocity) < self.yVelocity;
                    } else {
                        isEqual = self.yVelocity == static_cast<double>(slopeBoostVelocity);
                        isBelow = self.yVelocity < static_cast<double>(slopeBoostVelocity);
                    }
                    if (!isBelow && !isEqual) boostPlayer(self, slopeBoostVelocity);
                } else {
                    bool const upsideDown = self.isUpsideDown;
                    bool const slidingDown = !upsideDown ? (self.slopeVelocity < 0.f) : (0.f < self.slopeVelocity);
                    if (slidingDown && !self.maybeIsBoosted && !self.jumpBuffered && !self.stateRingJump &&
                        self.isCurrentSlopeTop == upsideDown) {
                        self.isOnGround2 = false;
                        int const gravitySign = upsideDown ? -1 : 1;
                        self.yVelocity = gdRound3(static_cast<double>(gravitySign) * -self.currentSlopeYVelocity);
                        self.isOnGround = false;
                        if (!self.isLocked && !self.isDashing) {
                            self.isRotating = false;
                            self.isBallRotating2 = false;
                            self.isBallRotating = false;
                            self.rotationSpeed = 0;
                            if (!self.isBall)
                                runNormalRotation(self, false, 1.f);
                            else
                                runBallRotation(self, 1.f);
                        }
                    }
                }
                self.currentSlopeYVelocity = 0;
                self.slopeVelocity = 0;
                self.slopeRotation = 0;
                self.currentSlope = 0;
            }
        }

        if (self.collidedTopMinY != 0.0 && self.collidedBottomMaxY != 0.0 && !self.disablePlayerSqueeze) {
            float const squeezeRatio = self.isPlatformer ? 0.8f : 0.7f;
            Rect const playerBox = playerRect(self);
            float const playerHeight = playerBox.h;
            float const gap = static_cast<float>(std::fabs(self.collidedTopMinY - self.collidedBottomMaxY));
            bool noSqueeze = false;
            if (gap == 0.f)
                noSqueeze = true;
            else if (!self.isUpsideDown) {
                if (self.collidedTopMinY < static_cast<double>(self.y) &&
                    self.collidedTopMinY < static_cast<double>(self.lastY))
                    noSqueeze = true;
            } else if (static_cast<double>(self.y) < self.collidedTopMinY &&
                       static_cast<double>(self.lastY) < self.collidedTopMinY)
                noSqueeze = true;
            if (!(noSqueeze || playerHeight * squeezeRatio <= gap)) {
                if (!self.ignoreDamage) {
                    if (!self.maybeCanRunIntoBlocks)
                        destroyPlayer(g, self, GObj{});
                    else
                        self.maybeIsColliding = true;
                }
            }
        }
        if (!self.ignoreDamage && !self.collisionLogTop.empty() && !self.collisionLogBottom.empty()) {
            for (int uid : self.collisionLogTop) {
                if (self.collisionLogBottom.contains(uid)) {
                    setPosition(self, self.lastX, self.lastY);
                    if (self.maybeCanRunIntoBlocks) {
                        self.maybeIsColliding = true;
                        return;
                    }
                    destroyPlayer(g, self, GObj{});
                    return;
                }
            }
        }

        if (self.isPlatformer && self.currentSlope2) self.preLastGroundObject = self.currentSlope2;
        if (self.isPlatformer && (self.lastGroundObject || self.preLastGroundObject)) {
            int carrier = 0;
            if (!self.isOnGround2)
                carrier = self.preLastGroundObject;
            else
                carrier = self.preLastGroundObject ? self.preLastGroundObject : self.lastGroundObject;
            GObj carrierObj = carrier ? g.objByUid(carrier) : GObj{};
            if (carrierObj && !carrierObj.o->nonStickX) {
                Point const last = carrierObj.lastPosition();
                Point const real = carrierObj.getRealPosition();
                float dx = 0.f;
                float dy = 0.f;
                if (!self.isSideways)
                    dx = real.x - last.x;
                else
                    dy = real.y - last.y;
                if (!self.isSideways) {
                    int const dynIndex = carrierObj.dyn();
                    if (dynIndex >= 0 && g.world->timeline->objs[static_cast<size_t>(dynIndex)].followsPlayerX) {
                        dx = 0.f;
                    }
                }
                setPosition(self, self.x + dx, self.y + dy);
            }
        }
        if (self.isPlatformer && !self.isOnGround2) self.preLastGroundObject = 0;

        bool groundFell = false;
        if (lastGround && !self.currentSlope2 && !lastGround.o->nonStickY) {
            Point const last = lastGround.lastPosition();
            Point const real = lastGround.getRealPosition();
            float scaleDx;
            float scaleDy;
            trigScalePosDelta(g, lastGround.index, scaleDx, scaleDy);
            if (last.x != real.x || last.y != real.y || scaleDy != 0.f) {
                float const lastAxisPos = self.isSideways ? last.x : last.y;
                float const realAxisPos = self.isSideways ? real.x : real.y;
                if (!self.isUpsideDown ? realAxisPos + scaleDy < lastAxisPos : lastAxisPos < realAxisPos - scaleDy) {
                    groundFell = true;
                }
            }
        }
        if (self.collisionLogBottom.empty() || groundFell) {
            self.isCollidingWithSlope = false;
            GObj groundObj = lastGround;
            self.lastGroundObject = 0;
            if (groundObj && groundFell) {
                float stickDistance = (self.isPlatformer ? 10.f : 5.f) * dt;
                if (groundObj.o->extraSticky) stickDistance = dt * 20.f;
                Rect const groundBox = groundObj.getObjectRect();
                Rect const playerBox = playerRect(self);
                Rect stickBox = groundBox;
                bool close;
                if (!self.isUpsideDown) {
                    if (!self.isSideways) {
                        stickBox.h += stickDistance;
                        close = getMinY(playerBox) - getMaxY(groundBox) < stickDistance;
                    } else {
                        stickBox.w += stickDistance;
                        close = getMinX(playerBox) - getMaxX(groundBox) < stickDistance;
                    }
                } else {
                    if (!self.isSideways) {
                        stickBox.h += stickDistance;
                        stickBox.y -= stickDistance;
                        close = getMinY(groundBox) - getMaxY(playerBox) < stickDistance;
                    } else {
                        stickBox.w += stickDistance;
                        stickBox.x -= stickDistance;
                        close = getMinX(groundBox) - getMaxX(playerBox) < stickDistance;
                    }
                }
                bool touching = intersectsRect(playerBox, stickBox);
                if (!touching && self.collidedObject) {
                    touching = true;
                    groundObj = g.objByUid(self.collidedObject);
                }
                if (close && touching && groundObj) {
                    self.isCollidingWithSlope = true;
                    collidedWithObject(g, self, 0.f, groundObj, zeroRect(), true);
                }
            }
        }
        bool slopeFalling = false;
        GObj slope2 = self.currentSlope2 ? g.objByUid(self.currentSlope2) : GObj{};
        if (slope2 && !slope2.o->nonStickY) {
            Point const last = slope2.lastPosition();
            Point const real = slope2.getRealPosition();
            if (real.x != last.x || real.y != last.y) {
                if (!self.isUpsideDown ? real.y < last.y : last.y < real.y) slopeFalling = true;
            }
        }
        if (self.collisionLogBottom.empty() || slopeFalling) {
            self.isCollidingWithSlope = false;
            self.currentSlope2 = 0;
            if (slopeFalling) {
                self.isCollidingWithSlope = true;
                if (!self.isSideways)
                    collidedWithSlopeInternal(g, self, 0.f, slope2, true);
                else
                    handleRotatedCollisionInternal(g, self, 0.f, slope2, zeroRect(), false, true, true);
            }
        }
        {
            double relatedVelocity = self.yVelocityRelated;
            if (relatedVelocity != 0.0 && self.stateBoostX < 1) {
                double const groundVelocity = self.groundYVelocity;
                if (impulseScale * 0.5f < static_cast<float>(std::fabs(relatedVelocity - groundVelocity))) {
                    if (!self.isUpsideDown) {
                        if (5.0 < relatedVelocity && groundVelocity < relatedVelocity) {
                            if (20.0 <= relatedVelocity) relatedVelocity = 20.0;
                            if (relatedVelocity <= -20.0) relatedVelocity = -20.0;
                            self.yVelocityRelated = relatedVelocity;
                            if (self.yVelocity <= relatedVelocity) {
                                boostPlayer(self, static_cast<float>(relatedVelocity));
                            }
                            self.currentSlope2 = 0;
                            self.lastGroundObject = 0;
                        }
                    } else if (relatedVelocity < -5.0 && relatedVelocity < groundVelocity) {
                        if (20.0 <= relatedVelocity) relatedVelocity = 20.0;
                        if (relatedVelocity <= -20.0) relatedVelocity = -20.0;
                        self.yVelocityRelated = relatedVelocity;
                        if (relatedVelocity <= self.yVelocity) boostPlayer(self, static_cast<float>(relatedVelocity));
                        self.currentSlope2 = 0;
                        self.lastGroundObject = 0;
                    }
                }
            }
        }
        self.collidedObject = 0;
        self.yVelocityRelated = self.groundYVelocity;
        self.groundYVelocity = 0;
        if (self.isOnGround2) {}
        if (!self.isOnSlope && !self.wasOnSlope) {
            self.collidingWithSlopeId = 0;
            double const velocity = self.yVelocity;
            double const prevFallSpeed = self.fallSpeed;
            if (!self.isUpsideDown) {
                if (velocity < -2.0 && -2.0 < prevFallSpeed) trigGameEvent(g, GameEvent::FallSpeedLow);
                if (velocity < -7.0 && -7.0 < prevFallSpeed) trigGameEvent(g, GameEvent::FallSpeedMed);
                if (velocity < -14.0 && -14.0 < prevFallSpeed) trigGameEvent(g, GameEvent::FallSpeedHigh);
            } else {
                if (2.0 < velocity && prevFallSpeed < 2.0) trigGameEvent(g, GameEvent::FallSpeedLow);
                if (7.0 < velocity && prevFallSpeed < 7.0) trigGameEvent(g, GameEvent::FallSpeedMed);
                if (14.0 < velocity && prevFallSpeed < 14.0) trigGameEvent(g, GameEvent::FallSpeedHigh);
            }
        }
        self.fallSpeed = self.yVelocity;
        self.reverseRelated -= 1;
    }

}
