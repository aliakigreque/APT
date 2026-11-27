// Declaration of the simulated camera state.

#pragma once
#include <cstdint>

namespace ap::sim {

    struct Game;
    struct Player;

    struct CameraState {
        float px = 0;
        float py = 0;
        float lx = 0;
        float ly = 0;
        float sdx = 0;
        float sdy = 0;
        float dpx = 0;
        float dpy = 0;
        float lastZoom = 1;
        float prevOffX = 0;
        float prevOffY = 0;
        float prevTgt[2] = {0, 0};

        float s5[2] = {0, 0};
        float s6[2] = {0, 0};
        float s7[2] = {0, 0};
        float s8[2] = {0, 0};
        float s9[2] = {0, 0};
        float s10[2] = {0, 0};
        float s11[2] = {0, 0};
        float s12[2] = {0, 0};
        float s13[2] = {0, 0};
        float s14[2] = {1, 1};
        float s15[2] = {0, 0};
        float s16[2] = {0, 0};
        float st[2] = {0, 0};

        float p18[2] = {0, 0};
        float p19[2] = {0, 0};
        float p20[2] = {0, 0};
        float p21[2] = {0, 0};
        float p22[2] = {0, 0};
        float p23[2] = {0, 0};
        float p24[2] = {0, 0};

        float gp26[2] = {0, 0};
        float gp27[2] = {0, 0};
        float gp28[2] = {0, 0};
        float u30[2] = {0, 0};
        float ease32[2] = {1, 10};
        float exitTimer = 0;
        int gpX = 75;
        int gpY = 75;
        bool gpLockX = false;
        bool gpLockY = false;
        bool staticYPortal = false;
        bool lastSideways = false;
        bool lastLeft = false;
        bool instX = false;
        bool instY = false;
        int edge[4] = {0, 0, 0, 0};
        float easing = 10.f;
        float padding = 0.5f;
        float dualProg = 0;
        float centerY = 0;
        bool on = false;
    };

    void cameraReset(Game& g);
    void cameraUpdate(Game& g, float dt);
    void cameraStaticPos(Game& g, float x, float y, bool affectsX, bool affectsY, bool follow, float time, int easing, float rate);
    void cameraStaticToGroup(Game& g, int group, bool affectsX, bool affectsY, bool follow, float followEase, float time, int easing, float rate, bool smooth, float velMod);
    void cameraExitStatic(Game& g, bool exitX, bool exitY, float time, int easing, float rate, bool smooth, float velMod, bool instant);
    void cameraResetStatic(Game& g, bool all);
    void cameraCheckLimitAfterTeleport(Game& g, Player const& p, float yOffset);
    bool cameraTweenSet(Game& g, int key, float value);
    bool cameraStaticBounds(Game const& g);
    float cameraStaticMinY(Game const& g);
    float cameraStaticMaxY(Game const& g);
    bool cameraOffscreen(Game const& g, Player const& p, float margin);
    uint64_t cameraHash(CameraState const& camera);

}
