// Object ids of Geometry Dash 2.2081 that the simulator tests by id (the editor's object numbers, also saved in the
// level string as key 1). Triggers dispatched in a switch keep their number there, next to the function they call.

#pragma once

namespace ap::sim::ObjId {
    inline constexpr int GravityOrb = 84;
    inline constexpr int SpeedSlow = 200;
    inline constexpr int SpeedNormal = 201;
    inline constexpr int SpeedFast = 202;
    inline constexpr int SpeedFaster = 203;
    inline constexpr int SpeedFastest = 1334;
    inline constexpr int MoveTrigger = 901;
    inline constexpr int Text = 914;
    inline constexpr int ToggleTrigger = 1049;
    inline constexpr int SpawnTrigger = 1268;
    inline constexpr int RotateTrigger = 1346;
    inline constexpr int FollowTrigger = 1347;
    // toggle orb: an orb that toggles a group when the player jumps on it
    inline constexpr int ToggleOrb = 1594;
    inline constexpr int CountTrigger = 1611;
    inline constexpr int InstantCountTrigger = 1811;
    inline constexpr int FollowPlayerYTrigger = 1814;
    inline constexpr int CollisionTrigger = 1815;
    inline constexpr int CollisionBlock = 1816;
    inline constexpr int PickupTrigger = 1817;
    inline constexpr int RandomTrigger = 1912;
    inline constexpr int StaticCameraTrigger = 1914;
    inline constexpr int RotateCameraTrigger = 2015;
    inline constexpr int EdgeCameraTrigger = 2062;
    inline constexpr int Checkpoint = 2063;
    // animated object that an animate trigger can start (animOnTrigger)
    inline constexpr int AnimatedObject = 2065;
    inline constexpr int ScaleTrigger = 2067;
    inline constexpr int AdvancedRandomTrigger = 2068;
    inline constexpr int ForceBlock = 2069;
    inline constexpr int RotateGameplayTrigger = 2900;
    inline constexpr int GameplayOffsetTrigger = 2901;
    // second teleport object, handled like the teleport trigger (it can send the player back)
    inline constexpr int TeleportArea = 2902;
    inline constexpr int CameraModeTrigger = 2925;
    inline constexpr int AdvancedFollowTrigger = 3016;
    inline constexpr int TeleportTrigger = 3022;
    inline constexpr int KeyframeObject = 3032;
    inline constexpr int KeyframeAnimTrigger = 3033;
    inline constexpr int EndTrigger = 3600;
    inline constexpr int EventTrigger = 3604;
    inline constexpr int InstantCollisionTrigger = 3609;
    inline constexpr int UiTrigger = 3613;
    inline constexpr int TimeTrigger = 3614;
    inline constexpr int TimeEventTrigger = 3615;
    inline constexpr int TimeControlTrigger = 3617;
    inline constexpr int ItemCompareTrigger = 3620;
    inline constexpr int StateBlock = 3640;
    inline constexpr int PersistentItemTrigger = 3641;
    // toggle block: a block that toggles a group when the player touches it
    inline constexpr int ToggleBlock = 3643;
    inline constexpr int ForceCircle = 3645;
    inline constexpr int EditAdvancedFollowTrigger = 3660;
    inline constexpr int RetargetAdvancedFollowTrigger = 3661;
}
