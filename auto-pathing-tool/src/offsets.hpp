// Fields of Geometry Dash 2.2081 (Windows) that the Geode bindings don't name, read at fixed offsets. Each offset was
// found in the game's code with Ghidra, in the function named next to it; they change with every game version.

#pragma once
#include <cstdint>

namespace ap::offsets {
    // in GeometryDash.exe: GameToolbox::fast_rand state, the rng of random triggers
    inline constexpr uintptr_t kFastRandState = 0x6c2e90;
    //in GeometryDash.exe: second rng, used by teleport portals and the area trigger variance values
    inline constexpr uintptr_t kTeleportRandState = 0x6c2ef8;

    // PlayLayer::m_randomSeed (uint64), the seed resetLevel picks for the attempt
    inline constexpr uintptr_t kPlayLayerRandomSeed = 0x32e0;

    // GJBaseGameLayer: m_gameState.m_commandIndex (int), counts the steps; GJBaseGameLayer::processCommands
    inline constexpr uintptr_t kLayerCommandIndex = 0x3e0;
    // GJBaseGameLayer: objects moved by area triggers this step (GameObject* array) and their count (int);
    // GJBaseGameLayer::processAreaActions only copies them to their nodes on the visible frame
    inline constexpr uintptr_t kLayerAreaObjects = 0xe40;
    inline constexpr uintptr_t kLayerAreaObjectCount = 0xee8;
    // GJBaseGameLayer::m_varianceValues (2000 floats), the random values of the area triggers
    inline constexpr uintptr_t kLayerVarianceValues = 0x10cc;

    // GameObject::m_varianceIndex (short), which of the variance values the object uses
    inline constexpr uintptr_t kObjVarianceIndex = 0x3f4;
    //GameObject: command index of the last transform trigger applied to it (int)
    inline constexpr uintptr_t kObjTransformStamp = 0x4e4;
    // GameObject::m_isDecoration2 (bool): an optimized object, moved only on the last step of a frame
    inline constexpr uintptr_t kObjOptimized = 0x512;

    // EnhancedGameObject fields read by updateSyncedAnimation:
    // -m_totalTime when the animation started (float)
    inline constexpr uintptr_t kAnimStart = 0x558;
    // current random wait between two cycles (float)
    inline constexpr uintptr_t kAnimWait = 0x55c;
    // animation time at the end of the last cycle (float)
    inline constexpr uintptr_t kAnimCycleEnd = 0x560;
    // sum of the past waits (float)
    inline constexpr uintptr_t kAnimWaitSum = 0x564;
    // true if the animation has random waits (bool)
    inline constexpr uintptr_t kAnimWaits = 0x570;
}
