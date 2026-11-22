// Player: the simulated PlayerObject, with the fields named as in the Geode bindings.

#pragma once

#include "containers.hpp"

#include <cstdint>

namespace ap::sim {
    using RingList = SmallVec<int, 48>;

    struct Obj;

    struct Player {
        float x = 0;
        float y = 0;
        float lastX = 0;
        float lastY = 0;

        double yVelocity = 0;
        float playerSpeed = 0.9f;
        double speedMultiplier = 5.7700018882751465;
        double yStart = 11.180031776428223;
        double gravity = 0.9581990242004395;
        float gravityMod = 1.f;
        float vehicleSize = 1.f;

        float hitboxSize = 30.f;
        float width = 30.f;
        float height = 30.f;
        float scaleX = 1.f;
        float scaleY = 1.f;
        float spriteWidthScale = 1.f;
        float spriteHeightScale = 1.f;
        mutable bool rectDirty = true;
        mutable float rectScaleW = 1.f;
        mutable float rectScaleH = 1.f;
        mutable float rectWidth = 30.f;
        mutable float rectHeight = 30.f;
        mutable bool obbDirty = true;
        mutable float obbScaleW = 1.f;
        mutable float obbScaleH = 1.f;

        double totalTime = 0;
        double accelerationOrSpeed = 0;
        double fallSpeed = 0;
        float yVelocityRelated3 = 0;
        double yVelocityRelated = 0;
        double groundYVelocity = 0;
        double yVelocityBeforeSlope = 0;
        double currentSlopeYVelocity = 0;
        double slopeRotation = 0;
        double snapDistance = 0;
        double maybeReverseSpeed = 0;
        double maybeReverseAcceleration = 0;

        bool isShip = false;
        bool isBird = false;
        bool isBall = false;
        bool isDart = false;
        bool isRobot = false;
        bool isSpider = false;
        bool isSwing = false;
        bool isUpsideDown = false;
        bool isGoingLeft = false;
        bool isSideways = false;
        bool isPlatformer = false;

        bool isOnGround = false;
        bool isOnGround2 = true;
        bool isOnGround3 = false;
        bool isOnGround4 = false;
        bool isOnSlope = false;
        bool wasOnSlope = false;
        bool isCollidingWithSlope = false;
        float slopeVelocity = 0;
        float slopeAngle = 0;
        float slopeAngleRadians = 0;
        double slopeStartTime = 0;
        double slopeEndTime = 0;
        bool slopeFlipGravityRelated = false;
        bool isCurrentSlopeTop = false;
        bool maybeUpsideDownSlope = false;
        bool slopeSlidingMaybeRotated = false;
        double unk_584 = 0;
        double unk3d0 = 0;
        double blackOrbRelated = 0;
        bool unk3e0 = false;
        bool unk3e1 = false;
        int maybeSlidingTime = 0;
        int collidingWithSlopeId = -1;

        bool jumpBuffered = false;
        bool stateRingJump = false;
        bool wasJumpBuffered = false;
        bool wasRobotJump = false;
        unsigned char stateJumpBuffered = 0;
        bool stateRingJump2 = false;
        bool touchedRing = false;
        bool touchedCustomRing = false;
        bool touchedGravityPortal = false;
        bool maybeTouchedBreakableBlock = false;
        bool touchedPad = false;
        bool isAccelerating = false;
        bool affectedByForces = false;
        bool maybeIsBoosted = false;
        bool maybeIsFalling = false;
        bool hasEverJumped = false;
        bool hasEverHitRing = false;
        bool padRingRelated = false;
        bool ringJumpRelated = false;
        bool maybeIsColliding = false;
        bool isSliding = false;
        bool isOnIce = false;

        bool isLocked = false;
        bool controlsDisabled = false;
        bool inputsLocked = false;
        bool isDashing = false;
        bool isDead = false;
        bool isOutOfBounds = false;
        bool wasTeleported = false;
        uint16_t tpCount = 0;

        bool fixGravityBug = false;
        bool fixRobotJump = true;
        bool enable22Changes = true;
        bool decreaseBoostSlide = true;
        bool unkA99 = false;
        bool unkA29 = false;
        bool unk669 = false;
        bool maybeReducedEffects = false;
        bool enableImpulseFix = false;
        bool disablePlayerSqueeze = false;
        bool ignoreDamage = false;
        bool maybeCanRunIntoBlocks = false;
        int reverseRelated = 0;
        bool reverseSync = false;
        bool justPlacedStreak = false;
        bool shouldTryPlacingCheckpoint = false;
        bool canPlaceCheckpoint = false;
        bool holdingJump = false;

        double lastJumpTime = 0;
        double lastLandTime = 0;
        double lastFlipTime = 0;
        double lastSpiderFlipTime = 0;
        double gameModeChangedTime = 0;
        float fallStartY = 0;

        double dashX = 0;
        double dashY = 0;
        double dashAngle = 0;
        double dashStartTime = 0;
        int dashRing = 0;
        int dashArtTick = -1;
        float dashArtSign = 1.f;

        int stateOnGround = 0;
        int stateNoAutoJump = 0;
        int stateDartSlide = 0;
        int stateHitHead = 0;
        int stateFlipGravity = 0;
        int stateBoostX = 0;
        int stateBoostY = 0;
        int maybeStateForce2 = 0;
        int stateScale = 0;
        int stateForce = 0;
        float stateForceX = 0;
        float stateForceY = 0;
        int forceKeys[4] = {0, 0, 0, 0};
        int nForceKeys = 0;
        int groundObjectMaterial = 0;

        double collidedTopMinY = 0;
        double collidedBottomMaxY = 0;
        double collidedLeftMaxX = 0;
        double collidedRightMinX = 0;
        int lastCollisionBottom = -1;
        int lastCollisionTop = -1;
        int lastCollisionLeft = -1;
        int lastCollisionRight = -1;
        int unk50C = -1;
        int unk510 = -1;
        int collidedObject = 0;
        int lastGroundObject = 0;
        int preLastGroundObject = 0;
        int maybeLastGroundObject = 0;
        int currentSlope = 0;
        int currentSlope2 = 0;
        int currentPotentialSlope = 0;
        int collidingWithLeft = 0;
        int collidingWithRight = 0;
        int objectSnappedTo = 0;
        int lastActivatedPortal = 0;
        float lastPortalX = 0;
        float lastPortalY = 0;
        float lastGroundedX = 0;
        float lastGroundedY = 0;
        SmallVec<int, 8> potentialSlopes;

        // remembers a slope the player may collide with this tick
        void addPotentialSlope(int uid) { potentialSlopes.insert(uid); }

        SmallVec<int, 8> collisionLogTop;
        SmallVec<int, 8> collisionLogBottom;
        SmallVec<int, 8> collisionLogLeft;
        SmallVec<int, 8> collisionLogRight;

        RingList touchingRings;
        RingList ringRelatedSet;
        RingList touchedRings;
        SmallVec<int, 8> jumpPadRelated;

        bool isRotating = false;
        bool isBallRotating = false;
        bool isBallRotating2 = false;
        float rotationSpeed = 0;
        float rotateSpeed = 1.f;
        float rotation = 0;
        bool rotUncertain = false;
        float rotUnc = 0.f;
        float shipRotX = 0;
        float shipRotY = 0;
        bool isHidden = false;

        bool isSecondPlayer = false;
        int uniqueID = 1;

        // ship, ufo, wave or swing
        // could replace the repeated ship, ufo, wave and swing checks in the search, keep
        bool isFlyMode() const { return isShip || isBird || isDart || isSwing; }
    };

    struct P2Leftover {
        bool valid = false;
        double snapDistance = 0;
        bool isOnGround3 = false;
        bool maybeIsFalling = false;
        bool padRingRelated = false;
        float fallStartY = 0;
        bool hasExtra = false;
        bool touchedPad = false;
        double accelerationOrSpeed = 0;

        // puts the saved player 2 fields back on a fresh player 2
        void apply(Player& p2) const {
            if (!valid) return;
            p2.snapDistance = snapDistance;
            p2.isOnGround3 = isOnGround3;
            p2.maybeIsFalling = maybeIsFalling;
            p2.padRingRelated = padRingRelated;
            p2.fallStartY = fallStartY;
            if (hasExtra) {
                p2.touchedPad = touchedPad;
                p2.accelerationOrSpeed = accelerationOrSpeed;
            }
        }
    };
    using PlayerLeftover = P2Leftover;
}
