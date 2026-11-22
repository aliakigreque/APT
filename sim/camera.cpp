// Simulated camera: follows the player, camera triggers and the limits used by the physics.

#include "camera.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "gd.hpp"
#include "triggers.hpp"

namespace ap::sim {

    namespace {
        constexpr float kNoEdge = -99999.f;
        // screen width, the normal screen size if unknown
        float winW(Game const& g) { return g.world && g.world->winW > 0.f ? g.world->winW : 568.8889f; }
        // screen height, 320 if unknown
        float winH(Game const& g) { return g.world && g.world->winH > 0.f ? g.world->winH : 320.f; }

        // position of main object of group, returns false if no main object
        bool groupPos(Game const& g, int group, float& x, float& y) {
            x = 0.f;
            y = 0.f;
            if (group <= 0 || !g.world) return false;
            int const idx = g.world->mainObjectOf(group);
            if (idx < 0) return false;
            double dx, dy;
            trigRealPos(g, idx, dx, dy);
            x = static_cast<float>(dx);
            y = static_cast<float>(dy);
            return true;
        }

        // object followed by camera, group or only obj
        bool followPos(Game const& g, int group, float& x, float& y) {
            x = 0.f;
            y = 0.f;
            if (group < 1 || !g.world) return false;
            int idx = g.world->exportParentOf(group);
            if (idx < 0) {
                auto const& objs = g.world->allOfGroup(group);
                if (objs.size() != 1) return false;
                idx = objs[0];
            }
            double dx, dy;
            trigRealPos(g, idx, dx, dy);
            x = static_cast<float>(dx);
            y = static_cast<float>(dy);
            return true;
        }
        // camera offset on i axis
        float offAxis(Game const& g, int i) { return i == 0 ? g.trig.cameraOffsetX : g.cameraOffsetY; }

        // forces v to stay in between 2 numbers
        float clampf(float v, float lo, float hi) { return v < lo ? lo : (hi < v ? hi : v); }

        // stops camera animation
        void tweenOff(Game& g, int key) { g.trig.tweens.erase(key); }
        // starts camera animation
        void tweenOn(Game& g, int key, float from, float to, float duration, int easing, float rate) {
            trigTweenValue(g, key, from, to, duration, easing, rate, -1, -1);
        }
    }

    // gets camera animation value, sorts it, keys taken from game, TODO could change names later
    bool cameraTweenSet(Game& g, int key, float value) {
        CameraState& camera = g.cam;
        switch (key) {
        case 10:
            camera.s6[0] = value;
            return true;
        case 11:
            camera.s6[1] = value;
            return true;
        case 12:
            camera.s8[0] = value;
            return true;
        case 13:
            camera.s8[1] = value;
            return true;
        case 0x12:
            camera.gp26[0] = value;
            return true;
        case 0x13:
            camera.gp26[1] = value;
            return true;
        case 0x15:
            camera.s12[0] = value;
            return true;
        case 0x16:
            camera.s12[1] = value;
            return true;
        case 0x19:
            camera.dualProg = value;
            return true;
        default:
            return false;
        }
    }

    void cameraResetStatic(Game& g, bool all) {
        CameraState& camera = g.cam;
        if (all) {
            tweenOff(g, 10);
            tweenOff(g, 12);
            camera.s6[0] = 0;
            camera.s8[0] = 0;
            camera.st[0] = 0;
            camera.p18[0] = 0;
            camera.p22[0] = 0;
        }
        tweenOff(g, 11);
        tweenOff(g, 13);
        camera.s6[1] = 0;
        camera.s8[1] = 0;
        camera.st[1] = 0;
        camera.p18[1] = 0;
        camera.p22[1] = 0;
    }

    // set static camera to x and y
    void cameraStaticPos(Game& g, float x, float y, bool affectsX, bool affectsY, bool follow, float time, int easing,
                         float rate) {
        CameraState& camera = g.cam;
        camera.staticYPortal = false;
        float const halfWidth = (winW(g) * 0.5f) / g.cameraZoom;
        float const halfHeight = (winH(g) * 0.5f) / g.cameraZoom;
        float const start[2] = {(camera.lx + halfWidth) - g.trig.cameraOffsetX,
                                (camera.ly + halfHeight) - g.cameraOffsetY};
        float const target[2] = {x, y};
        float const velocity[2] = {camera.dpx, camera.dpy};
        for (int i = 0; i < 2; ++i) {
            if (!(i == 0 ? affectsX : affectsY)) continue;
            camera.st[i] = 1.f;
            camera.s6[i] = start[i];
            camera.s8[i] = 0.f;
            tweenOff(g, 12 + i);
            camera.s10[i] = clampf(velocity[i], -10000.f, 10000.f);
            camera.s11[i] = time;
            camera.s5[i] = start[i];
            camera.s7[i] = target[i];
        }
        if (follow) return;
        if (time <= 0.f) {
            if (affectsX) {
                tweenOff(g, 10);
                camera.s6[0] = camera.s7[0];
            }
            if (affectsY) {
                tweenOff(g, 11);
                camera.s6[1] = camera.s7[1];
            }
        } else {
            if (affectsX) tweenOn(g, 10, camera.s6[0], camera.s7[0], time, easing, rate);
            if (affectsY) tweenOn(g, 11, camera.s6[1], camera.s7[1], time, easing, rate);
        }
    }

    // static camera centered on the main object of a group (static camera trigger)
    void cameraStaticToGroup(Game& g, int group, bool affectsX, bool affectsY, bool follow, float followEase,
                             float time, int easing, float rate, bool smooth, float velMod) {
        CameraState& camera = g.cam;
        if (!g.world || g.world->mainObjectOf(group) < 0) return;
        float objX;
        float objY;
        groupPos(g, group, objX, objY);
        float const minFollowEase = 1.f <= followEase ? followEase : 1.f;
        for (int i = 0; i < 2; ++i) {
            if (!(i == 0 ? affectsX : affectsY)) continue;
            camera.s15[i] = follow ? 1.f : 0.f;
            camera.p22[i] = smooth ? 1.f : 0.f;
            camera.p23[i] = velMod;
            camera.s16[i] = static_cast<float>(group);
            camera.s14[i] = minFollowEase;
            tweenOff(g, 0x15 + i);
            if (camera.s15[i] != 0.f || camera.p22[i] != 0.f) {
                if (time <= 0.f) {
                    camera.s12[i] = 1.f;
                    camera.s13[i] = 1.f;
                } else {
                    camera.s12[i] = 0.f;
                    camera.s13[i] = 0.f;
                    tweenOn(g, 0x15 + i, 0.f, 1.f, time, easing, rate);
                }
            }
        }
        cameraStaticPos(g, objX, objY, affectsX, affectsY, follow || smooth, time, easing, rate);
    }

    //leaves static camera, the smooth return to the player starts on the next cameraUpdate, tto be tested might bug
    void cameraExitStatic(Game& g, bool exitX, bool exitY, float time, int easing, float rate, bool smooth,
                          float velMod, bool instant) {
        CameraState& camera = g.cam;
        if (instant) {
            if (exitX) camera.instX = true;
            if (exitY) camera.instY = true;
        }
        float const velocity[2] = {camera.dpx, camera.dpy};
        for (int i = 0; i < 2; ++i) {
            if (!(i == 0 ? exitX : exitY) || camera.st[i] == 0.f) continue;
            camera.p18[i] = 1.f;
            camera.st[i] = 0.f;
            tweenOff(g, 10 + i);
            camera.p19[i] = time;
            camera.p21[i] = static_cast<float>(easing);
            camera.p20[i] = rate;
            camera.p22[i] = smooth ? 1.f : 0.f;
            camera.p23[i] = velMod;
            camera.s10[i] = clampf(velocity[i], -10000.f, 10000.f);
            camera.s5[i] = camera.s6[i];
        }
        if (g.hasModeGround && camera.st[1] == 0.f) trigUpdateDualGroundAfterCamera(g);
    }

    // snaps the camera to the player if a teleport left him off screen on y
    void cameraCheckLimitAfterTeleport(Game& g, Player const& p, float yOffset) {
        CameraState& camera = g.cam;
        if (!camera.on || camera.st[1] != 0.f || p.isSideways) return;
        float const height = winH(g);
        if (height + camera.py + yOffset < p.y || p.y < camera.py - yOffset) {
            camera.instY = true;
            cameraUpdate(g, 0.f);
        }
    }

    // true if the static camera bounds apply to fly modes (static y set by a trigger, not a portal)
    bool cameraStaticBounds(Game const& g) { return g.cam.on && g.cam.st[1] != 0.f && !g.cam.staticYPortal; }

    namespace {
        // on screen height of the fly mode floor and ceiling, same as updateCameraBGArt
        void groundYs(Game const& g, float& floorY, float& ceilingY) {
            CameraState const& camera = g.cam;
            float const height = winH(g);
            float const heightOffset = 0.f;
            float const top = heightOffset + height;
            float const bottom = -heightOffset;
            float const objY = -camera.py * g.cameraZoom;
            float const halfCorridor = g.lastModeHeight * 0.5f;
            float const minFloorY = g.cameraZoom * 91.f + objY;
            floorY = (((1.f - halfCorridor) + height * 0.5f) - bottom) * camera.dualProg + bottom;
            if (floorY <= minFloorY) floorY = minFloorY;
            ceilingY = top - (top - ((halfCorridor - 1.f) + height * 0.5f)) * camera.dualProg;
        }
    }

    // fly mode floor in static camera (getMinPortalY), never below the ground at 90
    float cameraStaticMinY(Game const& g) {
        float floorY;
        float ceilingY;
        groundYs(g, floorY, ceilingY);
        float const minY = (floorY - 1.f) / g.cameraZoom + g.cam.ly;
        return 90.f <= minY ? minY : 90.f;
    }

    // fly mode ceiling in static camera (getMaxPortalY)
    float cameraStaticMaxY(Game const& g) {
        float floorY;
        float ceilingY;
        groundYs(g, floorY, ceilingY);
        return (ceilingY + 1.f) / g.cameraZoom + g.cam.ly;
    }

    // true if the player is off screen (past margin) on a static camera axis
    bool cameraOffscreen(Game const& g, Player const& p, float margin) {
        CameraState const& camera = g.cam;
        if (!camera.on || g.cameraZoom <= 0.f) return false;
        if (camera.st[1] != 0.f && !camera.staticYPortal) {
            float const height = winH(g) / g.cameraZoom;
            if (p.y < camera.py - margin || p.y > camera.py + height + margin) return true;
        }
        if (camera.st[0] != 0.f) {
            float const width = winW(g) / g.cameraZoom;
            if (p.x < camera.px - margin || p.x > camera.px + width + margin) return true;
        }
        return false;
    }

    // hash of the whole camera state, used to tell search states apart
    uint64_t cameraHash(CameraState const& camera) {
        auto mix = [](uint64_t h, uint64_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            h *= 0xbf58476d1ce4e5b9ull;
            return h ^ (h >> 29);
        };
        auto bits = [](float f) {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return static_cast<uint64_t>(u);
        };
        uint64_t h = 0x6a09e667f3bcc909ull;
        float const fields[] = {
                camera.px,        camera.py,         camera.lx,         camera.ly,       camera.sdx,
                camera.sdy,       camera.dpx,        camera.dpy,        camera.lastZoom, camera.prevOffX,
                camera.prevOffY,  camera.prevTgt[0], camera.prevTgt[1], camera.s5[0],    camera.s5[1],
                camera.s6[0],     camera.s6[1],      camera.s7[0],      camera.s7[1],    camera.s8[0],
                camera.s8[1],     camera.s9[0],      camera.s9[1],      camera.s10[0],   camera.s10[1],
                camera.s11[0],    camera.s11[1],     camera.s12[0],     camera.s12[1],   camera.s13[0],
                camera.s13[1],    camera.s14[0],     camera.s14[1],     camera.s15[0],   camera.s15[1],
                camera.s16[0],    camera.s16[1],     camera.st[0],      camera.st[1],    camera.p18[0],
                camera.p18[1],    camera.p19[0],     camera.p19[1],     camera.p20[0],   camera.p20[1],
                camera.p21[0],    camera.p21[1],     camera.p22[0],     camera.p22[1],   camera.p23[0],
                camera.p23[1],    camera.p24[0],     camera.p24[1],     camera.gp26[0],  camera.gp26[1],
                camera.gp27[0],   camera.gp27[1],    camera.gp28[0],    camera.gp28[1],  camera.ease32[0],
                camera.ease32[1], camera.exitTimer,  camera.easing,     camera.padding,  camera.dualProg};
        for (float f : fields) h = mix(h, bits(f));
        h = mix(h, static_cast<uint64_t>(static_cast<uint32_t>(camera.gpX)) |
                           (static_cast<uint64_t>(static_cast<uint32_t>(camera.gpY)) << 32));
        h = mix(h, static_cast<uint64_t>(camera.gpLockX) | (static_cast<uint64_t>(camera.gpLockY) << 1) |
                           (static_cast<uint64_t>(camera.staticYPortal) << 2) |
                           (static_cast<uint64_t>(camera.lastSideways) << 3) |
                           (static_cast<uint64_t>(camera.lastLeft) << 4) | (static_cast<uint64_t>(camera.instX) << 5) |
                           (static_cast<uint64_t>(camera.instY) << 6));
        for (int e : camera.edge) h = mix(h, static_cast<uint64_t>(static_cast<uint32_t>(e)));
        return h;
    }

    // resets the camera at the start of an attempt (GJBaseGameLayer::resetCamera)
    void cameraReset(Game& g) {
        CameraState& camera = g.cam;
        bool const enabled = g.world && g.world->needsCamera && g.trig.on;
        camera = CameraState{};
        camera.on = enabled;
        if (!enabled) return;
        cameraResetStatic(g, true);
        for (int k : {0x12, 0x13, 0x15, 0x16, 0x19}) tweenOff(g, k);
        camera.lastZoom = g.cameraZoom;
    }

    // moves the camera by one frame (GJBaseGameLayer::updateCamera), dt is in 1/60s. might be optimisable?
    void cameraUpdate(Game& g, float dt) {
        CameraState& camera = g.cam;
        if (!camera.on) return;
        World const& world = *g.world;
        Player const& p = g.p1;
        float const winWidth = winW(g);
        float const winHeight = winH(g);
        float const zoom = g.cameraZoom;
        float const lastZoom = camera.lastZoom;
        float const viewWidth = winWidth / zoom;
        float const viewHeight = winHeight / zoom;
        float const lastViewWidth = winWidth / lastZoom;
        float const lastViewHeight = winHeight / lastZoom;
        bool const platformer = g.isPlatformer;

        float edgeX;
        float edgeY;
        float maxX = kNoEdge;
        float rawMinX = kNoEdge;
        float topY;
        float minY;
        if (camera.edge[1] >= 1) {
            groupPos(g, camera.edge[1], edgeX, edgeY);
            maxX = edgeX - viewWidth;
        }
        if (camera.edge[0] >= 1) {
            groupPos(g, camera.edge[0], edgeX, edgeY);
            rawMinX = edgeX;
        }
        if (camera.edge[2] >= 1) {
            groupPos(g, camera.edge[2], edgeX, edgeY);
            topY = edgeY;
        } else
            topY = g.maxGameplayY - 150.f;
        groupPos(g, camera.edge[3], edgeX, edgeY);
        minY = (0.f <= edgeY) ? edgeY : 0.f;
        float hasMin[2] = {static_cast<float>(camera.edge[0]), static_cast<float>(camera.edge[3])};
        float hasMax[2] = {static_cast<float>(camera.edge[1]), static_cast<float>(camera.edge[2])};
        if (world.hasEndPortal) {
            float const levelEndX = (world.length - viewWidth) - g.trig.cameraOffsetX;
            if (maxX == kNoEdge || levelEndX < maxX) {
                hasMax[0] = 1.f;
                maxX = levelEndX;
            }
        }
        float minX = rawMinX;
        if ((world.camClampA || (world.camUnk322a && !world.camTestMode)) && !world.camClampB && !(15.f <= rawMinX))
            minX = 15.f;
        float const maxPos[2] = {maxX, topY - viewHeight};
        float const minPos[2] = {minX, minY};

        bool const flying = p.isShip || p.isBird || p.isDart || p.isSwing || p.isBall || p.isSpider;
        bool const sideways = p.isSideways;
        bool const left = p.isGoingLeft;
        bool const upsideDown = p.isUpsideDown;
        float instant[2];
        {
            int instantX;
            int instantY = 1;
            if (dt != 0.f && !camera.instX)
                instantX = (camera.p18[0] != 0.f) ? 1 : 0;
            else
                instantX = 1;
            if (dt != 0.f && !camera.instY && camera.p18[1] == 0.f) instantY = 0;
            instant[0] = static_cast<float>(instantX);
            instant[1] = static_cast<float>(instantY);
        }
        if (camera.lastSideways != sideways && 0.f < dt) {
            for (int i = 0; i < 2; ++i) {
                if (camera.st[i] == 0.f && instant[i] == 0.f) {
                    camera.p18[i] = 1.f;
                    camera.p19[i] = 1.f;
                    camera.p21[i] = 1.f;
                    camera.p20[i] = 2.f;
                    camera.p22[i] = 0.f;
                }
            }
        }
        if (camera.instX || camera.instY) {
            camera.instX = false;
            camera.instY = false;
            camera.exitTimer = 0.f;
        }
        float const sign = left ? -1.f : 1.f;
        float gameplayOffset[2];
        if (!sideways) {
            gameplayOffset[0] = (static_cast<float>(camera.gpX) * sign) / (camera.gpLockX ? 1.f : zoom);
            gameplayOffset[1] = 0.f;
        } else {
            gameplayOffset[0] = 0.f;
            gameplayOffset[1] = (static_cast<float>(camera.gpY) * sign) / (camera.gpLockY ? 1.f : zoom);
        }
        camera.px -= camera.s9[0];
        camera.py -= camera.s9[1];
        camera.px += g.trig.cameraOffsetX - camera.prevOffX;
        camera.py += g.cameraOffsetY - camera.prevOffY;
        float const playerPos[2] = {p.x, p.y};
        float const playerDelta[2] = {p.x - p.lastX, p.y - p.lastY};
        float camPos[2] = {camera.px, camera.py};
        float deadZoneHigh = 70.f;
        float deadZoneLow = 40.f;
        if (platformer) {
            deadZoneHigh = 55.f;
            deadZoneLow = 27.5f;
        }
        if (upsideDown) std::swap(deadZoneHigh, deadZoneLow);
        float const padX =
                (winWidth * 0.5f - (((winWidth * 0.5f - 2.f) - 30.f) * camera.padding + 30.f)) * (1.f / zoom);
        float const padY =
                (winHeight * 0.5f - (((winHeight * 0.5f - 2.f) - 30.f) * camera.padding + 30.f)) * (1.f / zoom);
        float const pad[2] = {padX, padY};
        float ease[2] = {platformer ? 8.f : 1.f, 10.f};
        bool lockY = false;
        if (!sideways) {
            if (!upsideDown && p.lastGroundedY == 105.f && lastZoom < zoom && !p.isShip && !p.isBird && !p.isDart &&
                !p.isSwing && p.y <= viewHeight - 90.f / zoom)
                lockY = true;
        } else
            std::swap(ease[0], ease[1]);
        if (zoom != lastZoom) {
            camPos[0] += lastViewWidth * 0.5f - viewWidth * 0.5f;
            if (!lockY) camPos[1] += lastViewHeight * 0.5f - viewHeight * 0.5f;
        }

        float target[2] = {camPos[0], camPos[1]};
        for (int i = 0; i < 2; ++i) {
            float const halfView = (i == 0 ? viewWidth : viewHeight) * 0.5f;
            if (camera.st[i] == 0.f) {
                if (i == 1 && lockY) {
                    target[1] = g.cameraOffsetY + 0.f;
                } else if ((i == 0) != sideways) {
                    camera.gp27[i] = gameplayOffset[i];
                    bool snap = camera.u30[i] != 0.f;
                    if (!snap && zoom != lastZoom && camera.gp26[i] == camera.gp28[i] && camera.lastLeft == left)
                        snap = true;
                    if (snap)
                        camera.gp26[i] = gameplayOffset[i];
                    else {
                        float const step = std::fabs(playerDelta[i]);
                        if (gameplayOffset[i] <= camera.gp26[i]) {
                            if (camera.gp26[i] - step <= gameplayOffset[i])
                                camera.gp26[i] = gameplayOffset[i];
                            else
                                camera.gp26[i] = camera.gp26[i] - step;
                        } else {
                            if (gameplayOffset[i] <= camera.gp26[i] + step)
                                camera.gp26[i] = gameplayOffset[i];
                            else
                                camera.gp26[i] = step + camera.gp26[i];
                        }
                    }
                    if (instant[i] != 0.f) camera.gp26[i] = gameplayOffset[i];
                    target[i] = (playerPos[i] - halfView) + camera.gp26[i] + offAxis(g, i);
                } else {
                    bool const upsideDownAxis = (i == 1) ? !upsideDown : upsideDown;
                    (void)upsideDownAxis;
                    float low;
                    float high;
                    if (!flying) {
                        low = deadZoneLow;
                        high = deadZoneHigh;
                    } else {
                        ease[i] = camera.easing;
                        low = pad[i];
                        high = pad[i];
                    }
                    float const camOffset = offAxis(g, i);
                    if (playerPos[i] + camOffset <= camPos[i] + halfView + high) {
                        if (playerPos[i] + camOffset < (camPos[i] + halfView) - low)
                            target[i] = (playerPos[i] - halfView) + low + camOffset;
                    } else
                        target[i] = ((playerPos[i] - halfView) - high) + camOffset;
                }
            } else {
                bool const follow = camera.s15[i] != 0.f;
                bool const smooth = camera.p22[i] != 0.f;
                float axisEase = 1.f;
                if (follow || smooth) {
                    float end[2];
                    if (follow)
                        followPos(g, static_cast<int>(camera.s16[i]), end[0], end[1]);
                    else {
                        end[0] = camera.s7[0];
                        end[1] = camera.s7[1];
                    }
                    float const progress = camera.s12[i];
                    if (1.f <= progress)
                        camera.s6[i] = end[i];
                    else if (smooth) {
                        float const c1[2] = {(camera.s11[0] * camera.s10[0]) / 3.f + camera.s5[0],
                                             (camera.s11[1] * camera.s10[1]) / 3.f + camera.s5[1]};
                        float const c2 = (c1[i] - end[i]) * camera.p23[i] + end[i];
                        float const u = progress;
                        float const v = 1.f - u;
                        camera.s6[i] = c1[i] * 3.f * u * v * v + v * camera.s5[i] * v * v + c2 * 3.f * u * u * v +
                                       end[i] * u * u * u;
                    } else if (progress != camera.s13[i]) {
                        camera.s6[i] = (end[i] - camera.s6[i]) / ((1.f - camera.s13[i]) / (progress - camera.s13[i])) +
                                       camera.s6[i];
                        camera.s13[i] = progress;
                    }
                    if (follow) {
                        float const scaledEase = camera.s12[i] * camera.s14[i];
                        axisEase = (1.f <= scaledEase) ? scaledEase : 1.f;
                    }
                }
                ease[i] = axisEase;
                float staticPos = camera.s6[i];
                if (i == 1 && g.hasModeGround) {
                    float const staticTarget = camera.s7[1];
                    float const minCenterY = (g.lastModeHeight * 0.5f) / zoom + 90.f;
                    if (staticTarget < minCenterY) {
                        float const remaining = (staticTarget == camera.s5[1])
                                                        ? 0.f
                                                        : (staticTarget - staticPos) / (staticTarget - camera.s5[1]);
                        staticPos = staticPos + (minCenterY - staticTarget) * (1.f - remaining);
                    }
                }
                target[i] = (staticPos - halfView) + offAxis(g, i);
            }
            if (maxPos[i] != kNoEdge) {
                if (maxPos[i] < target[i]) target[i] = maxPos[i];
                if ((0.f < hasMax[i] || platformer) && maxPos[i] - 60.f < target[i] &&
                    !(target[i] < camera.prevTgt[i] && target[i] < camPos[i]) && ease[i] < 24.f &&
                    !(ease[i] == 1.f && camera.st[i] != 0.f))
                    ease[i] = (1.f - (maxPos[i] - target[i]) / 60.f) * (24.f - ease[i]) + ease[i];
            }
            if (minPos[i] != kNoEdge) {
                if (target[i] < minPos[i]) target[i] = minPos[i];
                if ((0.f < hasMin[i] || platformer) && target[i] < minPos[i] + 60.f &&
                    !(camera.prevTgt[i] < target[i] && camPos[i] < target[i]) && ease[i] < 24.f &&
                    !(ease[i] == 1.f && camera.st[i] != 0.f))
                    ease[i] = (1.f - (target[i] - minPos[i]) / 60.f) * (24.f - ease[i]) + ease[i];
            }
        }
        camera.prevTgt[0] = target[0];
        camera.prevTgt[1] = target[1];
        if (instant[1] == 0.f) {
            if (0.f < camera.exitTimer) {
                ease[1] = 30.f;
                camera.exitTimer = camera.exitTimer - dt / 60.f;
            }
        } else {
            camera.exitTimer = 0.f;
            ease[1] = 1.f;
        }
        if (instant[0] != 0.f) ease[0] = 1.f;
        auto clampEdges = [&](float& x, float& y) {
            if (0.f < maxPos[0] && maxPos[0] < x) x = maxPos[0];
            if (0.f < maxPos[1] && maxPos[1] < y) y = maxPos[1];
            if (x < minPos[0]) x = minPos[0];
            if (y < minPos[1]) y = minPos[1];
        };
        clampEdges(target[0], target[1]);

        float next[2];
        for (int i = 0; i < 2; ++i) {
            next[i] = target[i];
            if (1.f < ease[i]) {
                float divisor = ease[i];
                if (0.f < dt) divisor = ease[i] / dt;
                next[i] = (target[i] - camPos[i]) / divisor + camPos[i];
            }
        }
        camPos[0] = next[0];
        camPos[1] = next[1];
        camera.ease32[0] = ease[0];
        camera.ease32[1] = ease[1];
        clampEdges(camPos[0], camPos[1]);

        float const exitDelta[2] = {camera.lx - camPos[0], camera.ly - camPos[1]};
        for (int i = 0; i < 2; ++i) {
            if (camera.p18[i] != 0.f) {
                camera.p18[i] = 0.f;
                camera.s8[i] = exitDelta[i];
                if (0.f < camera.p19[i]) {
                    if (camera.p22[i] == 0.f) {
                        tweenOff(g, 12 + i);
                        tweenOn(g, 12 + i, camera.s8[i], 0.f, camera.p19[i], static_cast<int>(camera.p21[i]),
                                camera.p20[i]);
                    } else {
                        tweenOff(g, 12 + i);
                        camera.p24[i] = camera.s8[i];
                        camera.s12[i] = 0.f;
                        tweenOff(g, 0x15 + i);
                        tweenOn(g, 0x15 + i, 0.f, 1.f, camera.p19[i], static_cast<int>(camera.p21[i]), camera.p20[i]);
                    }
                } else {
                    tweenOff(g, 12 + i);
                    camera.s8[i] = 0.f;
                }
            }
            if (camera.st[i] == 0.f && camera.p22[i] != 0.f) {
                if (1.f <= camera.s12[i]) {
                    camera.p22[i] = 0.f;
                    camera.s8[i] = 0.f;
                } else {
                    float const c1 = (camera.p19[i] * camera.s10[i]) / 3.f + camera.p24[i];
                    float const c2 = (c1 - 0.f) * camera.p23[i] + 0.f;
                    float const u = camera.s12[i];
                    float const v = 1.f - u;
                    camera.s8[i] =
                            c1 * 3.f * u * v * v + v * camera.p24[i] * v * v + c2 * 3.f * u * u * v + u * 0.f * u * u;
                }
            }
        }
        float const beforeExit[2] = {camPos[0], camPos[1]};
        camPos[0] += camera.s8[0];
        camPos[1] += camera.s8[1];
        clampEdges(camPos[0], camPos[1]);
        camera.s9[0] = camPos[0] - beforeExit[0];
        camera.s9[1] = camPos[1] - beforeExit[1];

        camera.px = camPos[0];
        camera.py = camPos[1];
        camera.sdx = (camera.lx + (winWidth / lastZoom) * 0.5f) - (camera.px + (winWidth / zoom) * 0.5f);
        camera.sdy = (camera.ly + (winHeight / lastZoom) * 0.5f) - (camera.py + (winHeight / zoom) * 0.5f);
        camera.lastZoom = zoom;
        camera.lastSideways = sideways;
        camera.lastLeft = left;
        camera.dpx = (camera.px - camera.lx) * 240.f;
        camera.dpy = (camera.py - camera.ly) * 240.f;
        camera.lx = camera.px;
        camera.ly = camera.py;
        camera.centerY = (viewHeight * 0.5f + camPos[1]) - g.cameraOffsetY;
        camera.prevOffX = g.trig.cameraOffsetX;
        camera.prevOffY = g.cameraOffsetY;
        camera.gp28[0] = camera.gp27[0];
        camera.gp28[1] = camera.gp27[1];
        camera.u30[0] = 0.f;
        camera.u30[1] = 0.f;
    }

}
