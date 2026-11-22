// World: the static level (objects, groups, settings) and the analysis done on it when it loads.

#pragma once
#include "object_ids.hpp"
#include <cstdlib>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <string_view>
#include "gdmath.hpp"

namespace ap::sim {
    struct Timeline;

    inline constexpr int kModCadence = 4;
    inline constexpr int kStartLead = 2400;

    enum ObjType : int {
        Solid = 0,
        Hazard = 2,
        InverseGravityPortal = 3,
        NormalGravityPortal = 4,
        ShipPortal = 5,
        CubePortal = 6,
        Decoration = 7,
        YellowJumpPad = 8,
        PinkJumpPad = 9,
        GravityPad = 10,
        YellowJumpRing = 11,
        PinkJumpRing = 12,
        GravityRing = 13,
        InverseMirrorPortal = 14,
        NormalMirrorPortal = 15,
        BallPortal = 16,
        RegularSizePortal = 17,
        MiniSizePortal = 18,
        UfoPortal = 19,
        Modifier = 20,
        Breakable = 21,
        SecretCoin = 22,
        DualPortal = 23,
        SoloPortal = 24,
        Slope = 25,
        WavePortal = 26,
        RobotPortal = 27,
        TeleportPortal = 28,
        GreenRing = 29,
        Collectible = 30,
        UserCoin = 31,
        DropRing = 32,
        SpiderPortal = 33,
        RedJumpPad = 34,
        RedJumpRing = 35,
        CustomRing = 36,
        DashRing = 37,
        GravityDashRing = 38,
        CollisionObject = 39,
        Special = 40,
        SwingPortal = 41,
        GravityTogglePortal = 42,
        SpiderOrb = 43,
        SpiderPad = 44,
        EnterEffectObject = 45,
        TeleportOrb = 46,
        AnimatedHazard = 47,
    };

    // game events of the event trigger (3604), same numbers and names as GJGameEvent in the Geode bindings
    namespace GameEvent {
        inline constexpr int None = 0;
        inline constexpr int TinyLanding = 1;
        inline constexpr int FeatherLanding = 2;
        inline constexpr int SoftLanding = 3;
        inline constexpr int NormalLanding = 4;
        inline constexpr int HardLanding = 5;
        inline constexpr int HitHead = 6;
        inline constexpr int OrbTouched = 7;
        inline constexpr int OrbActivated = 8;
        inline constexpr int PadActivated = 9;
        inline constexpr int GravityInverted = 10;
        inline constexpr int GravityRestored = 11;
        inline constexpr int NormalJump = 12;
        inline constexpr int RobotBoostStart = 13;
        inline constexpr int RobotBoostStop = 14;
        inline constexpr int UFOJump = 15;
        inline constexpr int ShipBoostStart = 16;
        inline constexpr int ShipBoostEnd = 17;
        inline constexpr int SpiderTeleport = 18;
        inline constexpr int BallSwitch = 19;
        inline constexpr int SwingSwitch = 20;
        inline constexpr int WavePush = 21;
        inline constexpr int WaveRelease = 22;
        inline constexpr int DashStart = 23;
        inline constexpr int DashStop = 24;
        inline constexpr int Teleported = 25;
        inline constexpr int PortalNormal = 26;
        inline constexpr int PortalShip = 27;
        inline constexpr int PortalBall = 28;
        inline constexpr int PortalUFO = 29;
        inline constexpr int PortalWave = 30;
        inline constexpr int PortalRobot = 31;
        inline constexpr int PortalSpider = 32;
        inline constexpr int PortalSwing = 33;
        inline constexpr int YellowOrb = 34;
        inline constexpr int PinkOrb = 35;
        inline constexpr int RedOrb = 36;
        inline constexpr int GravityOrb = 37;
        inline constexpr int GreenOrb = 38;
        inline constexpr int DropOrb = 39;
        inline constexpr int CustomOrb = 40;
        inline constexpr int DashOrb = 41;
        inline constexpr int GravityDashOrb = 42;
        inline constexpr int SpiderOrb = 43;
        inline constexpr int TeleportOrb = 44;
        inline constexpr int YellowPad = 45;
        inline constexpr int PinkPad = 46;
        inline constexpr int RedPad = 47;
        inline constexpr int GravityPad = 48;
        inline constexpr int SpiderPad = 49;
        inline constexpr int PortalGravityFlip = 50;
        inline constexpr int PortalGravityNormal = 51;
        inline constexpr int PortalGravityInvert = 52;
        inline constexpr int PortalFlip = 53;
        inline constexpr int PortalUnFlip = 54;
        inline constexpr int PortalNormalScale = 55;
        inline constexpr int PortalMiniScale = 56;
        inline constexpr int PortalDualOn = 57;
        inline constexpr int PortalDualOff = 58;
        inline constexpr int PortalTeleport = 59;
        inline constexpr int Checkpoint = 60;
        inline constexpr int DestroyBlock = 61;
        inline constexpr int UserCoin = 62;
        inline constexpr int PickupItem = 63;
        inline constexpr int CheckpointRespawn = 64;
        inline constexpr int FallLow = 65;
        inline constexpr int FallMed = 66;
        inline constexpr int FallHigh = 67;
        inline constexpr int FallVHigh = 68;
        inline constexpr int JumpPush = 69;
        inline constexpr int JumpRelease = 70;
        inline constexpr int LeftPush = 71;
        inline constexpr int LeftRelease = 72;
        inline constexpr int RightPush = 73;
        inline constexpr int RightRelease = 74;
        inline constexpr int PlayerReversed = 75;
        inline constexpr int FallSpeedLow = 76;
        inline constexpr int FallSpeedMed = 77;
        inline constexpr int FallSpeedHigh = 78;
    }

    struct Rect {
        float x = 0;
        float y = 0;
        float w = 0;
        float h = 0;

        // right edge
        float maxX() const { return x + w; }

        // top edge
        float maxY() const { return y + h; }

        // horizontal center
        float midX() const { return x + w * 0.5f; }

        // vertical center
        // could be used by ported game code that reads the center of a rect, keep
        float midY() const { return y + h * 0.5f; }

        // edges included
        bool contains(float px, float py) const { return px >= x && px <= maxX() && py >= y && py <= maxY(); }
    };

    struct Obj {
        int uid = 0;
        int id = 0;
        int type = 0;
        int cls = 0;

        float x = 0;
        float y = 0;
        double posX = 0;
        double posY = 0;
        float rot = 0;
        float rotX = 0;
        float rotY = 0;
        float scaleX = 1;
        float scaleY = 1;

        Rect rect;
        float width = 0;
        float height = 0;
        float boxOffX = 0;
        float boxOffY = 0;
        float radius = 0;

        bool hasObb = false;
        bool rectGen = false;
        bool obbGen = false;
        bool rectGenDone = false;
        float obbHx = 0;
        float obbHy = 0;
        float obbVx = 0;
        float obbVy = 0;
        float genW = 0;
        float genH = 0;
        float genHW = 0;
        float genHH = 0;
        bool useOuterOb = false;
        float obb[8]{};

        int slopeDir = 0;
        bool slopeUphill = false;
        bool slopeHazard = false;
        bool gripSlope = false;

        bool disabled = false;
        bool extendedCollision = false;
        bool trigger = false;
        bool passable = false;
        bool noTouch = false;
        bool decoration = false;
        bool hide = false;
        bool invisibleBlock = false;
        bool groupDisabled = false;
        bool flipX = false;
        bool flipY = false;
        bool rotationAligned = true;
        bool activated = false;
        bool hasNoEffects = false;
        bool animOnTrigger = false;
        bool uiObject = false;
        int uiType = 0;
        bool claimTouch = false;
        bool ringSpawnOnly = false;
        int channel = 0;
        int ord = 0;
        bool iceBlock = false;
        bool dontBoostY = false;
        bool dontBoostX = false;
        bool extraSticky = false;
        bool nonStickX = false;
        bool nonStickY = false;
        bool scaleStick = false;
        bool highDetail = false;

        int targetGroup = 0;
        float duration = 0;
        bool touchTriggered = false;
        bool spawnTriggered = false;
        bool multiTriggered = false;
        bool multiActivate = false;
        bool reverse = false;
        bool singlePTouch = false;
        std::vector<short> groups;
        int groupSig = -1;
        float startX = 0;
        float startY = 0;
        float startRotX = 0;
        float startRotY = 0;
        float startScaleX = 1;
        float startScaleY = 1;
        bool hasStartRS = false;
        int secOuter = -1;
        int secMiddle = -1;
        int secInner = -1;

        int tpOrange = 0;
        float tpYOffset = 0;
        bool tpYellow = false;
        int tpGravity = 0;
        bool tpSaveOffset = false;
        bool tpIgnoreX = false;
        bool tpIgnoreY = false;
        bool tpStaticForce = false;
        float tpStaticForceV = 0;
        bool tpStaticForceAdd = false;
        bool tpRedirectDash = false;
        bool tpRedirectForce = false;
        bool tpSnapGround = false;
        bool tpEase = false;
        bool tpInstantCam = false;
        float tpRedirectMod = 1.f;
        float tpRedirectMin = 0.f;
        float tpRedirectMax = 0.f;

        float dashSpeed = 0;
        float endBoost = 0;
        float maxDuration = 0;
        bool allowCollide = false;
        bool stopSlide = false;

        float force = 0;
        float forceMin = 0;
        float forceMax = 0;
        bool forceRelative = false;
        bool forceRange = false;
        int forceKey = 0;

        int centerGroup = 0;
        float mvX = 0;
        float mvY = 0;
        int easing = 0;
        float easingRate = 0;
        int moveTargetMode = 0;
        int targetModCenter = 0;
        float moveModX = 0;
        float moveModY = 0;
        float dirDistance = 0;
        float rotDeg = 0;
        int times360 = 0;
        int rotTarget = 0;
        float rotOffset = 0;
        int rotMinX = 0;
        int rotMinY = 0;
        int rotMaxX = 0;
        int rotMaxY = 0;
        int dynEasing = 0;
        bool aimExt = false;
        int linkedGroup = 0;
        bool moveOpt = false;
        int kfGroup = 0;
        float followXMod = 0;
        float followYMod = 0;
        float fySpeed = 0;
        float fyDelay = 0;
        float fyMaxSpeed = 0;
        int fyOffset = 0;
        int touchToggleMode = 0;
        int touchPlayerMode = 0;
        int itemID = 0;
        int itemID2 = 0;
        int controlID = 0;
        int collectPoints = 0;
        float zoom = 0;
        float timeWarpMod = 0;
        float spawnDelay = 0;
        float spawnDelayRange = 0;
        int count = 0;
        int compare = 0;
        float countMult = 0;
        int stopCmd = 0;
        float gravityValue = 0;
        int optFlags = 0;
        int optValues = 0;
        int rgMove = 0;
        int rgGround = 0;
        int rgFlags = 0;
        int rgChannel = 0;
        int camFlags = 0;
        int remapIdx = 0;
        std::vector<int> remapPairs;
        float rgModX = 0;
        float rgModY = 0;
        short material = 0;
        short areaRi = -1;
        int ext = -1;
        float posOffX = 0;
        float posOffY = 0;
        float sws = 0;
        float shs = 0;

        uint32_t tflags = 0;
        enum TF : uint32_t {
            TF_LockPX = 1u << 0,
            TF_LockPY = 1u << 1,
            TF_LockCX = 1u << 2,
            TF_LockCY = 1u << 3,
            TF_UseMoveTarget = 1u << 4,
            TF_DirSnap = 1u << 5,
            TF_Dynamic = 1u << 6,
            TF_Silent = 1u << 7,
            TF_LockObjRot = 1u << 8,
            TF_ActivateGroup = 1u << 9,
            TF_TouchHold = 1u << 10,
            TF_DualMode = 1u << 11,
            TF_SpawnOrdered = 1u << 12,
            TF_TriggerOnExit = 1u << 13,
            TF_TargetControlID = 1u << 14,
            TF_DynamicBlock = 1u << 15,
            TF_TargetP1 = 1u << 16,
            TF_TargetP2 = 1u << 17,
            TF_SubtractCount = 1u << 18,
            TF_PickupItem = 1u << 19,
            TF_CollectToggle = 1u << 20,
            TF_EndReversed = 1u << 21,
            TF_ResetRemap = 1u << 22,
            TF_CountMulti = 1u << 23,
            TF_CountOverride = 1u << 24,
            TF_HasRemap = 1u << 25,
            TF_GroupParent = 1u << 26,
            TF_AreaParent = 1u << 27,
            TF_FollowCPP = 1u << 28,
        };

        // true if the bit is set in tflags
        bool tf(uint32_t bit) const { return (tflags & bit) != 0; }

        bool movable = false;
        bool toggleable = false;
        bool kfOpt = false;
        uint64_t groupBloom = 0;
        bool lockX = false;

        // m_canRotateFree, false for solids, breakables and slopes
        bool canRotateFree() const { return !(type == 0 || type == 21 || type == 25); }

        // true for a collision block (1816)
        bool isCollisionBlock() const { return id == ObjId::CollisionBlock; }

        // true for every kind of orb
        bool isRing() const {
            return type == YellowJumpRing || type == PinkJumpRing || type == GravityRing || type == GreenRing ||
                   type == DropRing || type == RedJumpRing || type == CustomRing || type == DashRing ||
                   type == GravityDashRing || type == SpiderOrb || type == TeleportOrb;
        }

        // true for every kind of pad
        bool isPad() const {
            return type == YellowJumpPad || type == PinkJumpPad || type == GravityPad || type == RedJumpPad ||
                   type == SpiderPad;
        }

        // the five speed portals
        bool isSpeedPortal() const {
            return type == Modifier && (id == ObjId::SpeedSlow || id == ObjId::SpeedNormal || id == ObjId::SpeedFast ||
                                        id == ObjId::SpeedFaster || id == ObjId::SpeedFastest);
        }

        // trigger activated when the player touches it
        bool isTouchTrigger() const {
            return type == Modifier && cls == 1 && touchTriggered && !spawnTriggered && !isSpeedPortal();
        }

        // every portal except speed portals
        bool isPortal() const {
            return type == InverseGravityPortal || type == NormalGravityPortal || type == ShipPortal ||
                   type == CubePortal || type == InverseMirrorPortal || type == NormalMirrorPortal ||
                   type == BallPortal || type == RegularSizePortal || type == MiniSizePortal || type == UfoPortal ||
                   type == DualPortal || type == SoloPortal || type == WavePortal || type == RobotPortal ||
                   type == TeleportPortal || type == SpiderPortal || type == SwingPortal || type == GravityTogglePortal;
        }
    };

    struct ExtProps {
        std::vector<std::pair<int, double>> kv;
        std::vector<std::pair<int, std::vector<double>>> lists;

        // value of key, or def if missing
        double num(int key, double def = 0.0) const {
            for (auto const& e : kv)
                if (e.first == key) return e.second;
            return def;
        }

        // true if the raw property key is set
        bool has(int key) const {
            for (auto const& e : kv)
                if (e.first == key) return true;
            return false;
        }

        // list of key, or nullptr if missing
        std::vector<double> const* list(int key) const {
            for (auto const& e : lists)
                if (e.first == key) return &e.second;
            return nullptr;
        }
    };

    bool wantsExtProps(int id);
    ExtProps parseExtProps(std::string_view save);
    std::string formatExtProps(ExtProps const& props);

    // same as GameObject::getObjectRect_2 from a real position, negative size becomes 0
    inline Rect genericObjRect(Obj const& o, float px, float py, float scaleX, float scaleY, bool aligned) {
        float w = (scaleX * o.width) * o.sws;
        float h = (scaleY * o.height) * o.shs;
        if (aligned) std::swap(w, h);
        float const cx = px + o.boxOffX;
        float const cy = py + o.boxOffY;
        return Rect{cx - w * 0.5f, cy - h * 0.5f, w < 0.f ? 0.f : w, h < 0.f ? 0.f : h};
    }

    // same thing at start scale and rotation, sizes computed by World::build
    inline Rect genericObjRectStatic(Obj const& o, float px, float py) {
        return Rect{(px + o.boxOffX) - o.genHW, (py + o.boxOffY) - o.genHH, o.genW, o.genH};
    }

    struct WorldSerial {
        uint64_t v;

        // next unique world id
        static uint64_t next() noexcept {
            static std::atomic<uint64_t> n{1};
            return n.fetch_add(1, std::memory_order_relaxed);
        }

        // new world: new id
        WorldSerial() noexcept : v(next()) {}

        // copied world: new id, thread caches don't mix the two
        WorldSerial(WorldSerial const&) noexcept : v(next()) {}

        // assigned world: new id
        WorldSerial& operator=(WorldSerial const&) noexcept {
            v = next();
            return *this;
        }
    };

    struct World {
        WorldSerial serial;

        std::string levelName;
        int levelId = 0;
        float length = 0;
        float groundP1 = 105.f;
        float groundShip = 0.f;
        float minPortalY = 0;
        float maxPortalY = 0;
        bool platformer = false;
        float maxGameplayY = 0;
        float winW = 0;
        float winH = 0;
        bool hasEndPortal = true;
        bool camClampA = false;
        bool camClampB = false;
        bool camUnk322a = false;
        bool camTestMode = false;

        uint64_t eventUsed[2] = {0, 0};

        // true if an event trigger listens to this game event
        bool listensEvent(int event) const {
            return event >= 0 && event < 128 && ((eventUsed[event >> 6] >> (event & 63)) & 1u);
        }

        bool hasRandom = false;
        std::vector<float> fyDelayX;
        bool fyDelayAlways = false;
        bool uiDone = false;
        int uiObjects = 0;
        bool canPruneBehind = false;
        bool needsCamera = false;
        bool twoPlayer = false;
        bool gridSnapOff = false;
        bool enable22 = true;
        float startLastMaxPortalY = -1.f;
        bool tpRngKnown = false;
        uint64_t tpRngSeed = 0;
        bool tpMulti = false;
        bool ySection = false;

        float startX = 0;
        float startY = 105.f;
        int startMode = 0;
        int startSpeed = 0;
        bool startMini = false;
        bool startDual = false;
        bool rotateGameplay = false;
        bool startFlipped = false;
        bool startReverse = false;
        int spawnGroup = 0;

        bool fixRobotJump = false;
        bool decreaseBoostSlide = false;
        bool enablePlayerSqueeze = false;
        bool fixGravityBug = false;
        bool enableImpulseFix = false;
        bool allowMultiRotation = false;
        bool reverseSync = false;
        bool areaParExported = false;
        bool moveOptExported = false;
        bool sortGroups = false;
        bool fixRadiusCollision = false;
        bool fixNegativeScale = false;
        bool allowStaticRotate = false;

        std::vector<Obj> objs;
        std::vector<ExtProps> ext;
        std::vector<uint8_t> checkedItems;

        // raw properties of o, or nullptr
        ExtProps const* extOf(Obj const& o) const {
            return o.ext >= 0 && o.ext < static_cast<int>(ext.size()) ? &ext[static_cast<size_t>(o.ext)] : nullptr;
        }

        // raw property of o, or def
        double prop(Obj const& o, int key, double def = 0.0) const {
            auto const* e = extOf(o);
            return e ? e->num(key, def) : def;
        }

        int attempts = 2;
        std::vector<std::pair<int, int>> persistentItems;
        std::vector<std::pair<int, double>> persistentTimers;
        std::vector<int> persistItemIds;
        std::vector<std::pair<int, std::string>> texts;

        struct MenuOption {
            int idx = -1;
            int value = 0;
            std::string label;
        };

        struct MenuItem {
            int item = 0;
            std::string kind;
            std::vector<MenuOption> options;
            bool hasPrefer = false;
            int prefer = 0;
            std::vector<int> forbid;
        };

        std::vector<MenuItem> menus;
        std::vector<int> ldmTouch;

        int frameCadence = 1;
        std::vector<int> frameStarts;
        bool modPlays = false;
        bool allVisible = false;

        // first tick of the frame containing tick
        int frameStartOf(int tick) const {
            if (!frameStarts.empty() && tick <= frameStarts.back() + 16) {
                auto it = std::upper_bound(frameStarts.begin(), frameStarts.end(), tick);
                if (it != frameStarts.begin()) {
                    int const s = *(it - 1);
                    return tick - s > 16 ? tick : s;
                }
                return tick;
            }
            if (allVisible) return tick < frameCadence ? 0 : tick;
            return frameCadence > 1 ? tick - ((tick % frameCadence) + frameCadence) % frameCadence : tick;
        }

        std::vector<short> kfOptGroups;

        // the mod plays the plan: first frame of firstFrameSteps steps, then every step is visible
        void useModFrames(int firstFrameSteps) {
            modPlays = true;
            allVisible = true;
            frameCadence = firstFrameSteps;
            frameStarts.clear();
            for (auto& o : objs) o.kfOpt = false;
            kfOptGroups.clear();
        }

        std::vector<std::vector<std::vector<int>>> cells;

        struct BandEntry {
            int idx;
            unsigned uid;
            float minY;
            float maxY;
        };

        struct MovEntry {
            int idx;
            unsigned uid;
            float minY;
            float maxY;
            uint8_t ngroups;
            short groups[10];
            bool dyn;
            bool teleporter;
        };

        struct CellIndex {
            static constexpr float kBandH = 64.f;
            std::vector<std::vector<BandEntry>> bands;
            std::vector<MovEntry> mov;
            bool teleporter = false;
            bool byPos = false;
            bool sortedRuns = false;

            // checks whether every cell list is sorted by uid then index
            void computeSorted() {
                auto ok = [](auto const& v) {
                    for (size_t i = 1; i < v.size(); ++i)
                        if (v[i].uid < v[i - 1].uid || (v[i].uid == v[i - 1].uid && v[i].idx < v[i - 1].idx))
                            return false;
                    return true;
                };
                sortedRuns = ok(mov);
                for (auto const& b : bands)
                    if (sortedRuns && !ok(b)) sortedRuns = false;
            }
        };

        std::vector<std::vector<CellIndex>> cellIndex;
        CellIndex extendedIndex;
        std::vector<int> extended;
        std::vector<std::pair<float, int>> portalPassX;

        struct PortalCluster {
            float passX = 0;
            float approachX = 1e30f;
            float yLo = 0;
            float yHi = 0;
            bool speed = false;
            bool dyn = false;
            bool learned = false;
            bool modeOut = false;
            std::vector<int> members;
        };

        std::vector<PortalCluster> portalClusters;
        std::vector<std::pair<float, int>> clusterPassX;
        int uidMax = 0;
        std::vector<int> byUid;

        bool hasTriggerData = false;
        std::vector<std::string> unsupported;
        std::vector<std::string> cameraUnsupported;
        std::vector<std::string> approximations;

        // true if the level export has the trigger data needed to simulate triggers
        bool triggersSimulable() const { return hasTriggerData; }

        // true if the camera can be simulated (no unsupported camera trigger)
        bool cameraSimulable() const { return cameraUnsupported.empty(); }

        std::vector<std::vector<int>> groupObjs;

        struct ObjLite {
            double posX;
            double posY;
            int groupSig;
            short g0;
            uint8_t lockX;
            uint8_t nGroups;
        };

        std::vector<ObjLite> lite;

        struct FastBucket {
            std::vector<short> movers;
            std::vector<int> byX;
            std::vector<int> byY;
            std::vector<float> qx;
            std::vector<float> qy;
        };

        struct GroupFast {
            std::vector<FastBucket> buckets;
            std::vector<int> other;
        };

        std::vector<GroupFast> groupFast;

        // fast access of group, or nullptr
        GroupFast const* fastOf(int group) const {
            return (group >= 0 && static_cast<size_t>(group) < groupFast.size() &&
                    !groupFast[static_cast<size_t>(group)].buckets.empty())
                           ? &groupFast[static_cast<size_t>(group)]
                           : nullptr;
        }

        std::vector<std::vector<int>> groupAll;
        std::vector<int> groupParent;
        std::vector<int> exportParents;
        int maxGroup = 0;
        std::vector<std::pair<int, std::vector<int>>> spawnLists;
        std::vector<std::vector<std::pair<int, int>>> spawnRemaps;

        std::vector<int> dynamicBlocks;
        std::vector<std::vector<std::vector<int>>> blockCells;
        std::vector<int> blockExtended;
        std::vector<int> blockMoving;
        std::vector<uint8_t> blockMovingMark;
        std::vector<std::vector<std::vector<int>>> blockMovCells;
        std::vector<uint8_t> blockMovPair;
        bool blockMovPlayer[2] = {false, false};

        std::vector<int> collisionPairs;
        std::vector<uint8_t> collisionItemListened;
        std::vector<std::vector<int>> collisionPartners;
        std::vector<uint8_t> areaRef;

        // true if itemId is part of a listened pair
        bool collisionItem(int itemId) const {
            return itemId >= 0 && static_cast<size_t>(itemId) < collisionItemListened.size() &&
                   collisionItemListened[static_cast<size_t>(itemId)];
        }

        std::vector<int> collPairRow;
        int collPairQ0 = 0;

        // true if a collision trigger listens to this item pair
        bool collisionPairListened(int itemA, int itemB) const {
            int const mx = itemA < itemB ? itemB : itemA;
            int const mn = itemA < itemB ? itemA : itemB;
            int const key = (mx + 1000) * 10000 + mn;
            if (key >= 0 && !collPairRow.empty()) {
                int const q = key / 10000 - collPairQ0;
                if (q < 0 || q + 1 >= static_cast<int>(collPairRow.size())) return false;
                for (int i = collPairRow[static_cast<size_t>(q)], e = collPairRow[static_cast<size_t>(q) + 1]; i < e;
                     ++i)
                    if (collisionPairs[static_cast<size_t>(i)] == key) return true;
                return false;
            }
            return std::binary_search(collisionPairs.begin(), collisionPairs.end(), key);
        }

        bool hasCollisionTriggers = false;
        std::vector<int> trigSlot;
        int nTrigSlots = 0;

        std::vector<char> centerLike;
        std::vector<char> afKeep;
        std::vector<char> decoLike;
        std::vector<char> areaEffectLive;

        // true if an area trigger with this effect id has a target that matters
        bool areaEffectLiveAt(int effectId) const {
            return effectId > 0 && static_cast<size_t>(effectId) < areaEffectLive.size() &&
                   areaEffectLive[static_cast<size_t>(effectId)];
        }

        std::vector<uint8_t> centerWhy;

        // true if the object has no gameplay role (decorations only before trigger analysis)
        bool decoLikeAt(int idx) const {
            if (idx < 0) return false;
            if (static_cast<size_t>(idx) < decoLike.size()) return decoLike[static_cast<size_t>(idx)] != 0;
            return static_cast<size_t>(idx) < objs.size() && objs[static_cast<size_t>(idx)].type == 7;
        }

        std::vector<std::vector<int>> groupMovers;

        // members of a group that matter for tracking, empty if none
        std::vector<int> const& moversOfGroup(int group) const {
            static std::vector<int> const kEmpty;
            return (group >= 0 && group < static_cast<int>(groupMovers.size()))
                           ? groupMovers[static_cast<size_t>(group)]
                           : kEmpty;
        }

        std::vector<char> groupRelevant;
        std::vector<float> groupLastMoveX;
        std::vector<int> gfFirst;
        std::vector<float> gfX;
        std::vector<float> gfSum;
        std::vector<int> grFirst;
        std::vector<float> grX;
        std::vector<int> grCenter;

        // forward x simple moves from fromX can still give to group
        float groupFutureFwd(int group, float fromX) const {
            if (group < 0 || static_cast<size_t>(group) + 1 >= gfFirst.size()) return 0.f;
            int const a = gfFirst[static_cast<size_t>(group)];
            int const b = gfFirst[static_cast<size_t>(group) + 1];
            int const k = static_cast<int>(std::lower_bound(gfX.begin() + a, gfX.begin() + b, fromX) - gfX.begin());
            return k < b ? gfSum[static_cast<size_t>(k)] : 0.f;
        }

        std::vector<char> mayHaveDyn;
        std::vector<char> scaleTarget;
        bool pPossible = true;
        alignas(8) mutable bool pBorn = false;

        struct KfEntry {
            double dur = 0;
            int easing = 0;
            float rate = 2;
            bool refOnly = false;
            int spawnGroup = 0;
            float spawnDelay = 0;
            bool proximity = false;
            bool curve = false;
            double runDur = 0;
            double runEase = 0;
            int timeMode = 0;
            int uid = 0;
            float dx = 0;
            float dy = 0;
            float ax = 0;
            float ay = 0;
            double sxr = 1;
            double syr = 1;
            double rot = 0;
            int splineX = -1;
            int splineY = -1;
        };

        struct KfSpline {
            std::vector<double> x;
            std::vector<double> y;
            std::vector<double> b;
            std::vector<double> c;
            std::vector<double> d;
            double c0 = 0;
        };

        struct KfAnim {
            int target = 0;
            int uid = 0;
            int control = 0;
            double duration = 0;
            bool spawns = false;
            std::vector<KfEntry> e;
            std::vector<int> keys;
            float timeMod = 0;
            float pxm = 1;
            float pym = 1;
            float rotm = 1;
            float sxm = 1;
            float sym = 1;
            bool dynKeys = false;
        };

        struct KfKeyState {
            float px = 0;
            float py = 0;
            double sx = 1;
            double sy = 1;
            double rotX = 0;
            double rotY = 0;
        };

        std::vector<KfAnim> kfAnims;
        std::vector<KfSpline> kfSplines;
        std::vector<int> kfFirst;
        std::vector<int> kfCount;
        std::unordered_map<long long, std::pair<int, int>> kfAlt;

        struct AreaDef {
            int idx = -1;
            int id = 0;
            int kind = 0;
            bool edit = false;
            int target = 0;
            int center = 0;
            int special = 0;
            int control = 0;
            float duration = 0;
            float v[28] = {};
            int easeInType = 0;
            int easeOutType = 0;
            float easeInRate = 0;
            float easeOutRate = 0;
            bool easeOut = false;
            bool relative = false;
            bool xyMode = false;
            bool inbound = false;
            bool twoDirections = false;
            bool useEffectID = false;
            int dirType = 0;
            int priority = 0;
            int effectID = 0;
            bool variance = false;
            bool ignoreGroupParent = false;
            bool ignoreLinked = false;
            bool dontEditParent = false;
        };

        std::vector<AreaDef> areaDefs;

        struct TargetGroups {
            std::vector<int> loose;
            std::vector<int> looseSorted;

            struct Rep {
                int obj = -1;
                int center = -1;
                std::vector<int> children;
                std::vector<int> phys;
            };

            std::vector<Rep> reps;
        };

        std::vector<TargetGroups> targetGroups;
        std::unordered_map<long long, int> targetGroupIdx;

        // options use the 2 low bits
        static long long tgKey(int group, bool ignoreGroupParent, bool ignoreLinked) {
            return static_cast<long long>(group) * 4 + (ignoreGroupParent ? 2 : 0) + (ignoreLinked ? 1 : 0);
        }

        std::vector<int> targetGroupDense;

        // target groups for (group, options), or nullptr
        TargetGroups const* targetGroupsOf(int group, bool ignoreGroupParent, bool ignoreLinked) const {
            if (targetGroupDense.empty()) return nullptr;
            long long const k = tgKey(group, ignoreGroupParent, ignoreLinked);
            if (k < 0 || k >= static_cast<long long>(targetGroupDense.size())) return nullptr;
            int const i = targetGroupDense[static_cast<size_t>(k)];
            return i < 0 ? nullptr : &targetGroups[static_cast<size_t>(i)];
        }

        struct AdvDef {
            int idx = -1;
            int target = 0;
            int key = 0;
            int control = 0;
            int mode = 0;
            int priority = 0;
            int startRef = 0;
            int dirRef = 0;
            int startMode = 0;
            float delay = 0;
            float delayV = 0;
            float maxSpeed = 0;
            float maxSpeedV = 0;
            float range = 0;
            float rangeV = 0;
            float accel = 0;
            float accelV = 0;
            float startSpeed = 0;
            float startSpeedV = 0;
            float startDir = 0;
            float startDirV = 0;
            bool xOnly = false;
            bool yOnly = false;
            bool useTargetDir = false;
            bool ignoreDisabled = false;
            bool rotate = false;
            bool once = false;
            float rotOffset = 0;
            float easing = 0;
            float easingV = 0;
            float m1Range = 0;
            float m1RangeV = 0;
            float m1Fric = 0;
            float m1FricV = 0;
            float m1Acc = 0;
            float m1AccV = 0;
            float m1Fric2 = 0;
            float m1Fric2V = 0;
            float angleMinDist = 0;
            float dirEasing = 0;
            float steer = 0;
            float steerV = 0;
            float steerLow = 0;
            float steerLowV = 0;
            float steerHigh = 0;
            float steerHighV = 0;
            float speedLow = 0;
            float speedLowV = 0;
            float speedHigh = 0;
            float speedHighV = 0;
            float brk = 0;
            float brkV = 0;
            float brkAngle = 0;
            float brkAngleV = 0;
            float brkSteer = 0;
            float brkSteerV = 0;
            float brkLimit = 0;
            float brkLimitV = 0;
            bool steerLowOn = false;
            bool steerHighOn = false;
            bool ignoreGroupParent = false;
            bool ignoreLinked = false;
            bool relevant = false;
        };

        std::vector<AdvDef> advDefs;

        struct AfRing {
            int key = 0;
            int obj = -1;
            int size = 0;
            int base = 0;
        };

        std::vector<AfRing> afRings;
        int afRingFloats = 0;
        std::vector<int> advDefOf;
        std::vector<int> areaDefOf;
        std::vector<char> areaTarget;
        std::vector<float> areaRand;
        bool hasAreaEffects = false;

        // true if the group can change the gameplay
        bool relevantGroup(int group) const {
            return group >= 0 && group < static_cast<int>(groupRelevant.size()) &&
                   groupRelevant[static_cast<size_t>(group)];
        }

        // positional triggers of a channel, or nullptr
        std::vector<int> const* spawnList(int channel) const {
            for (auto const& e : spawnLists)
                if (e.first == channel) return &e.second;
            return nullptr;
        }

        // exported parent of group, or -1
        int exportParentOf(int group) const {
            return (group >= 0 && group < static_cast<int>(exportParents.size()))
                           ? exportParents[static_cast<size_t>(group)]
                           : -1;
        }

        // parent object of a group, -1 if none
        int parentOf(int group) const {
            return (group >= 0 && group < static_cast<int>(groupParent.size()))
                           ? groupParent[static_cast<size_t>(group)]
                           : -1;
        }

        // objects of group without decorations
        std::vector<int> const& objsOfGroup(int group) const {
            static std::vector<int> const kEmpty;
            return (group >= 0 && group < static_cast<int>(groupObjs.size())) ? groupObjs[static_cast<size_t>(group)]
                                                                              : kEmpty;
        }

        // objects of group, decorations included
        std::vector<int> const& allOfGroup(int group) const {
            static std::vector<int> const kEmpty;
            return (group >= 0 && group < static_cast<int>(groupAll.size())) ? groupAll[static_cast<size_t>(group)]
                                                                             : kEmpty;
        }

        // GJBaseGameLayer::tryGetMainObject: group parent, else the only member, else -1
        int mainObjectOf(int group) const {
            if (group < 1) return -1;
            int const ep = exportParentOf(group);
            bool const useEp = ep >= 0 && (enable22 || !objs[static_cast<size_t>(ep)].tf(Obj::TF_GroupParent));
            int const p = useEp ? ep : parentOf(group);
            if (p >= 0) return p;
            auto const& g = allOfGroup(group);
            return g.size() == 1 ? g[0] : -1;
        }

        // one int key for a cell (cy < 4096)
        static int cellKey(int cx, int cy) { return cx * 4096 + cy; }

        Timeline const* timeline = nullptr;

        // sets the timeline of moving objects and rebuilds the world
        void attach(Timeline const* tl) {
            timeline = tl;
            build();
        }

        void build();

        // object index of uid, or -1
        int indexOfUid(int uid) const { return (uid >= 0 && uid <= uidMax) ? byUid[static_cast<size_t>(uid)] : -1; }

        // GD section of a position (100 units, capped at 1e7 like GJBaseGameLayer::addToSection)
        static int cellOf(double v) {
            float const factor = 0.01f;
            double r = 0.0;
            if (0.0 < v) r = (v < 10000000.0) ? static_cast<double>(factor) * v : static_cast<double>(factor * 1e7f);
            return static_cast<int>(r);
        }

        // same thing in float
        static int cellOfF(float v) {
            float const factor = 0.01f;
            float r = 0.f;
            if (0.f < v) r = (v < 1e7f) ? v * factor : factor * 1e7f;
            return static_cast<int>(r);
        }

        // y section, always 0 without y sections
        int cellYOf(double v) const { return ySection ? cellOf(v) : 0; }

        // section row of a y position (float version)
        int cellYOfF(float v) const { return ySection ? cellOfF(v) : 0; }
    };

    struct WKey {
        uint64_t s = 0;
        // key with no world
        WKey() = default;

        // key of a world
        WKey(World const* world) noexcept : s(world ? world->serial.v : 0) {}

        // true if both keys are the same world
        friend bool operator==(WKey a, WKey b) noexcept { return a.s == b.s; }

        // true if the keys are different worlds
        friend bool operator!=(WKey a, WKey b) noexcept { return a.s != b.s; }
    };

    bool loadLevel(std::string const& path, World& world, std::string* err = nullptr);
}
