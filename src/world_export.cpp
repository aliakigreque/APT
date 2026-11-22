// Reads the loaded level from the game (objects, triggers, settings) into the simulator's World,
// and writes it to levels/ as jsonl for apsim.

#include "world_export.hpp"
#include "offsets.hpp"

#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <tuple>
#include <unordered_map>

#include "../sim/triggers.hpp"
#include "log.hpp"

#include <Geode/cocos/support/zip_support/ZipUtils.h>

using namespace geode::prelude;

namespace ap {
    namespace {
        // formats a float with enough digits to read back the exact value
        std::string f32(float value) { return fmt::format("{:.9g}", value); }

        // formats a double with full precision
        std::string f64(double value) { return fmt::format("{:.17g}", value); }

        // level string in plain text
        std::string levelStringOf(PlayLayer* pl) {
            if (!pl || !pl->m_level) return {};
            std::string raw = std::string(pl->m_level->m_levelString);
            if (raw.rfind("H4sI", 0) == 0) {
                std::string decoded = std::string(cocos2d::ZipUtils::decompressString(gd::string(raw), false, 0));
                if (!decoded.empty()) return decoded;
            }
            return raw;
        }
        struct ExtWant {
            size_t obj;
            int id;
            float x;
            float y;
        };

        //reads the 2.2 trigger properties that only exist in the level string (not in memory) why does this exist
        void fillExtProps(PlayLayer* pl, sim::World& w, std::vector<ExtWant> const& want) {
            if (want.empty()) return;
            std::string const levelString = levelStringOf(pl);
            if (levelString.empty()) return;
            using Key = std::tuple<int, long long, long long>;
            auto key = [](int id, float x, float y) {
                return Key{id, std::llround(static_cast<double>(x) * 100.0),
                           std::llround(static_cast<double>(y) * 100.0)};
            };
            std::map<Key, std::deque<std::string_view>> candidates;
            std::string_view text(levelString);
            size_t pos = text.find(';');
            while (pos != std::string_view::npos && pos + 1 < text.size()) {
                size_t const end = text.find(';', pos + 1);
                std::string_view objectText =
                        text.substr(pos + 1, end == std::string_view::npos ? std::string_view::npos : end - pos - 1);
                pos = end;
                if (objectText.empty()) continue;
                int id = 0;
                double x = 0;
                double y = 0;
                int seen = 0;
                size_t i = 0;
                while (i < objectText.size() && seen < 3) {
                    size_t const comma = objectText.find(',', i);
                    if (comma == std::string_view::npos) break;
                    size_t next = objectText.find(',', comma + 1);
                    if (next == std::string_view::npos) next = objectText.size();
                    std::string_view const field = objectText.substr(i, comma - i);
                    std::string_view const value = objectText.substr(comma + 1, next - comma - 1);
                    std::string const valueText(value);
                    if (field == "1") {
                        id = std::atoi(valueText.c_str());
                        ++seen;
                    } else if (field == "2") {
                        x = std::atof(valueText.c_str());
                        ++seen;
                    } else if (field == "3") {
                        y = std::atof(valueText.c_str());
                        ++seen;
                    }
                    i = next + 1;
                }
                if (!sim::wantsExtProps(id)) continue;
                candidates[key(id, static_cast<float>(x), static_cast<float>(y) + 90.f)].push_back(objectText);
            }
            int missing = 0;
            std::vector<ExtWant> retry;
            for (auto const& entry : want) {
                auto it = candidates.find(key(entry.id, entry.x, entry.y));
                if (it == candidates.end() || it->second.empty()) {
                    retry.push_back(entry);
                    continue;
                }
                w.ext.push_back(sim::parseExtProps(it->second.front()));
                it->second.pop_front();
                w.objs[entry.obj].ext = static_cast<int>(w.ext.size()) - 1;
            }
            for (auto const& entry : retry) {
                Key const wanted = key(entry.id, entry.x, entry.y);
                auto it = candidates.end();
                {
                    for (int dx = -1; dx <= 1 && it == candidates.end(); ++dx)
                        for (int dy = -1; dy <= 1 && it == candidates.end(); ++dy) {
                            auto nearby = candidates.find(
                                    Key{std::get<0>(wanted), std::get<1>(wanted) + dx, std::get<2>(wanted) + dy});
                            if (nearby != candidates.end() && !nearby->second.empty()) it = nearby;
                        }
                }
                if (it == candidates.end()) {
                    ++missing;
                    continue;
                }
                w.ext.push_back(sim::parseExtProps(it->second.front()));
                it->second.pop_front();
                w.objs[entry.obj].ext = static_cast<int>(w.ext.size()) - 1;
            }
            if (missing) Log::printf("export: 2.2 properties not found in the level string for {} trigger(s)", missing);
        }

        // name usable in a file name
        std::string safeName(std::string const& name) {
            std::string out;
            for (char c : name) {
                out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
            }
            return out.empty() ? "level" : out;
        }
    }

    namespace {
        struct AreaKey {
            PlayLayer* pl = nullptr;
            int levelId = 0;
            unsigned count = 0;
            float length = 0;
        };
        AreaKey g_areaKey;
        std::unordered_map<int, short> g_areaRi;

        // identifies the loaded level (layer, id, object count, length)
        AreaKey areaKeyOf(PlayLayer* pl) {
            AreaKey key;
            key.pl = pl;
            key.levelId = pl->m_level ? static_cast<int>(pl->m_level->m_levelID.value()) : 0;
            key.count = pl->m_objects ? pl->m_objects->count() : 0u;
            key.length = pl->m_levelLength;
            return key;
        }

    }

    // writes the saved variance index back into every object
    void applyAreaRandom(PlayLayer* pl) {
        if (!pl || !pl->m_objects || g_areaRi.empty()) return;
        AreaKey const key = areaKeyOf(pl);
        if (key.pl != g_areaKey.pl || key.levelId != g_areaKey.levelId || key.count != g_areaKey.count) return;
        for (auto* object : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (!object) continue;
            auto it = g_areaRi.find(object->m_uniqueID);
            if (it != g_areaRi.end()) {
                *reinterpret_cast<short*>(reinterpret_cast<uintptr_t>(object) + offsets::kObjVarianceIndex) =
                        it->second;
            }
        }
    }

    // forgets the saved indexes
    void forgetAreaRandom() {
        g_areaRi.clear();
        g_areaKey = AreaKey{};
    }

    //copies everything the sim needs from the loaded level
    sim::World buildWorld(PlayLayer* pl) {
        sim::World w;
        if (!pl) return w;
        w.levelName = pl->m_level ? safeName(std::string(pl->m_level->m_levelName)) : "level";
        w.levelId = pl->m_level ? static_cast<int>(pl->m_level->m_levelID.value()) : 0;
        w.length = pl->m_levelLength;
        w.groundP1 = pl->getGroundHeightForMode(0);
        w.groundShip = pl->getGroundHeightForMode(1);
        w.minPortalY = pl->getMinPortalY();
        w.maxPortalY = pl->getMaxPortalY();
        w.platformer = pl->m_isPlatformer;
        w.maxGameplayY = pl->m_maxGameplayY;
        w.twoPlayer = pl->m_levelSettings ? pl->m_levelSettings->m_twoPlayerMode : false;
        w.ySection = pl->m_levelSettings
                             ? (pl->m_levelSettings->m_platformerMode || pl->m_levelSettings->m_dynamicLevelHeight)
                             : pl->m_isPlatformer;
        if (auto* settings = pl->m_levelSettings) {
            w.startMode = settings->m_startMode;
            w.startSpeed = static_cast<int>(settings->m_startSpeed);
            w.startMini = settings->m_startMini;
            w.startDual = settings->m_startDual;
            w.rotateGameplay = settings->m_rotateGameplay;
            w.startFlipped = settings->m_isFlipped;
            w.startReverse = settings->m_reverseGameplay;
            w.spawnGroup = settings->m_spawnGroup;
            w.fixRobotJump = settings->m_fixRobotJump;
            w.decreaseBoostSlide = settings->m_decreaseBoostSlide;
            w.allowMultiRotation = settings->m_allowMultiRotation;
            w.reverseSync = settings->m_reverseSync;
            w.enablePlayerSqueeze = settings->m_enablePlayerSqueeze;
            w.fixGravityBug = settings->m_fixGravityBug;
            w.enableImpulseFix = settings->m_enableImpulseFix;
            w.fixRadiusCollision = settings->m_fixRadiusCollision;
            w.fixNegativeScale = settings->m_fixNegativeScale;
            w.allowStaticRotate = settings->m_allowStaticRotate;
            w.sortGroups = settings->m_sortGroups;
        }
        w.gridSnapOff = pl->m_gameState.m_disableCameraGridSnap;
        {
            auto const winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();
            w.winW = winSize.width;
            w.winH = winSize.height;
            w.hasEndPortal = pl->m_endPortal != nullptr;
            if (auto* settings = pl->m_levelSettings) {
                w.camClampA = settings->m_propertykA23;
                w.camClampB = settings->m_propertykA24;
            }
            w.camUnk322a = pl->m_unk322a;
            w.camTestMode = pl->m_isTestMode;
        }
        w.enable22 = pl->m_enable22Changes;
        w.startX = 0.f;
        w.startY = 105.f;
        if (!pl->m_objects) return w;
        std::vector<ExtWant> extWant;
        w.objs.reserve(static_cast<size_t>(pl->m_objects->count()));
        for (auto* o : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (!o) continue;
            if (o == pl->m_anticheatSpike) continue;
            sim::Obj data;
            data.areaRi = *reinterpret_cast<short*>(reinterpret_cast<uintptr_t>(o) + offsets::kObjVarianceIndex);
            data.material = o->m_objectMaterial;
            data.uid = o->m_uniqueID;
            data.id = o->m_objectID;
            data.type = static_cast<int>(o->m_objectType);
            data.cls = static_cast<int>(o->m_classType);
            auto pos = o->getPosition();
            data.x = pos.x;
            data.y = pos.y;
            data.posX = o->m_positionX;
            data.posY = o->m_positionY;
            data.rot = o->getRotation();
            data.rotX = o->getRotationX();
            data.rotY = o->getRotationY();
            data.scaleX = o->m_scaleX;
            data.scaleY = o->m_scaleY;
            auto rect = o->getObjectRect();
            data.rect = sim::Rect{rect.origin.x, rect.origin.y, rect.size.width, rect.size.height};
            data.width = o->m_width;
            data.height = o->m_height;
            data.boxOffX = o->m_boxOffset.x;
            data.boxOffY = o->m_boxOffset.y;
            data.radius = o->m_objectRadius;
            data.slopeDir = o->m_slopeDirection;
            data.disabled = o->m_isDisabled;
            data.extendedCollision = o->m_hasExtendedCollision;
            data.trigger = o->m_isTrigger;
            data.passable = o->m_isPassable;
            data.noTouch = o->m_isNoTouch;
            data.decoration = o->m_isDecoration;
            data.hide = o->m_isHide;
            data.gripSlope = o->m_isGripSlope;
            data.slopeUphill = o->m_slopeUphill;
            data.slopeHazard = o->m_slopeIsHazard;
            data.flipX = o->m_isFlipX;
            data.flipY = o->m_isFlipY;
            data.invisibleBlock = o->m_isInvisibleBlock;
            data.groupDisabled = o->m_isGroupDisabled;
            data.rotationAligned = o->m_isRotationAligned;
            data.activated = o->m_isActivated;
            data.iceBlock = o->m_isIceBlock;
            data.dontBoostY = o->m_isDontBoostY;
            data.dontBoostX = o->m_isDontBoostX;
            data.extraSticky = o->m_isExtraSticky;
            data.nonStickX = o->m_isNonStickX;
            data.nonStickY = o->m_isNonStickY;
            data.scaleStick = o->m_isScaleStick;
            data.highDetail = o->m_isHighDetail;
            data.useOuterOb = o->m_shouldUseOuterOb;
            data.hasNoEffects = o->m_hasNoEffects;
            if (o->m_objectID == 2065) data.animOnTrigger = static_cast<EnhancedGameObject*>(o)->m_animateOnTrigger;
            {
                auto* ring = typeinfo_cast<RingObject*>(o);
                if (!ring && o->m_objectType == GameObjectType::CustomRing) ring = static_cast<RingObject*>(o);
                if (ring) {
                    data.claimTouch = ring->m_claimTouch;
                    data.ringSpawnOnly = ring->m_isSpawnOnly;
                }
            }
            if (o->m_shouldUseOuterOb && o->m_orientedBox) {
                if (auto* box = o->getOrientedBox()) {
                    data.hasObb = true;
                    for (int i = 0; i < 4; ++i) {
                        data.obb[i * 2] = box->m_corners[i].x;
                        data.obb[i * 2 + 1] = box->m_corners[i].y;
                    }
                }
            }
            if (o->m_classType == GameObjectClassType::Effect) {
                auto* effect = static_cast<EffectGameObject*>(o);
                data.targetGroup = effect->m_targetGroupID;
                data.duration = effect->m_duration;
                data.touchTriggered = effect->m_isTouchTriggered;
                data.spawnTriggered = effect->m_isSpawnTriggered;
                data.multiTriggered = effect->m_isMultiTriggered;
                data.multiActivate = effect->m_isMultiActivate;
                data.reverse = effect->m_isReverse;
                data.channel = effect->m_channelValue;
                data.singlePTouch = effect->m_isSinglePTouch;
                data.centerGroup = effect->m_centerGroupID;
                data.mvX = effect->m_moveOffset.x;
                data.mvY = effect->m_moveOffset.y;
                data.camFlags = (effect->m_cameraIsFreeMode ? 1 : 0) | (effect->m_cameraDisableGridSnap ? 2 : 0) | 4;
                data.easing = static_cast<int>(effect->m_easingType);
                data.easingRate = effect->m_easingRate;
                data.moveTargetMode = static_cast<int>(effect->m_moveTargetMode);
                data.targetModCenter = effect->m_targetModCenterID;
                data.moveModX = effect->m_moveModX;
                data.moveModY = effect->m_moveModY;
                data.dirDistance = effect->m_directionModeDistance;
                data.rotDeg = effect->m_rotationDegrees;
                data.times360 = effect->m_times360;
                data.rotTarget = effect->m_rotationTargetID;
                data.rotOffset = effect->m_rotationOffset;
                if (o->m_objectID == 1346) {
                    auto* enhanced = static_cast<EnhancedTriggerObject*>(o);
                    data.rotMinX = enhanced->m_minXID;
                    data.rotMinY = enhanced->m_minYID;
                    data.rotMaxX = enhanced->m_maxXID;
                    data.rotMaxY = enhanced->m_maxYID;
                    data.dynEasing = effect->m_dynamicModeEasing;
                    data.aimExt = true;
                }
                data.followXMod = effect->m_followXMod;
                data.followYMod = effect->m_followYMod;
                data.fySpeed = effect->m_followYSpeed;
                data.fyDelay = effect->m_followYDelay;
                data.fyOffset = effect->m_followYOffset;
                data.fyMaxSpeed = effect->m_followYMaxSpeed;
                data.touchToggleMode = static_cast<int>(effect->m_touchToggleMode);
                data.touchPlayerMode = static_cast<int>(effect->m_touchPlayerMode);
                data.itemID = effect->m_itemID;
                data.itemID2 = effect->m_itemID2;
                data.controlID = effect->m_controlID;
                data.collectPoints = effect->m_collectiblePoints;
                data.zoom = effect->m_zoomValue;
                data.timeWarpMod = effect->m_timeWarpTimeMod;
                data.gravityValue = effect->m_gravityValue;
                data.ord = effect->m_ordValue;
                auto flag = [&](bool value, uint32_t bit) {
                    if (value) data.tflags |= bit;
                };
                flag(effect->m_lockToPlayerX, sim::Obj::TF_LockPX);
                flag(effect->m_lockToPlayerY, sim::Obj::TF_LockPY);
                flag(effect->m_lockToCameraX, sim::Obj::TF_LockCX);
                flag(effect->m_lockToCameraY, sim::Obj::TF_LockCY);
                flag(effect->m_useMoveTarget, sim::Obj::TF_UseMoveTarget);
                flag(effect->m_isDirectionFollowSnap360, sim::Obj::TF_DirSnap);
                flag(effect->m_isDynamicMode, sim::Obj::TF_Dynamic);
                flag(effect->m_isSilent, sim::Obj::TF_Silent);
                flag(effect->m_lockObjectRotation, sim::Obj::TF_LockObjRot);
                flag(effect->m_activateGroup, sim::Obj::TF_ActivateGroup);
                flag(effect->m_touchHoldMode, sim::Obj::TF_TouchHold);
                flag(effect->m_isDualMode, sim::Obj::TF_DualMode);
                flag(effect->m_spawnOrdered, sim::Obj::TF_SpawnOrdered);
                flag(effect->m_triggerOnExit, sim::Obj::TF_TriggerOnExit);
                flag(effect->m_targetControlID, sim::Obj::TF_TargetControlID);
                flag(effect->m_isDynamicBlock, sim::Obj::TF_DynamicBlock);
                flag(effect->m_targetPlayer1, sim::Obj::TF_TargetP1);
                flag(effect->m_targetPlayer2, sim::Obj::TF_TargetP2);
                flag(effect->m_subtractCount, sim::Obj::TF_SubtractCount);
                flag(effect->m_collectibleIsPickupItem, sim::Obj::TF_PickupItem);
                flag(effect->m_collectibleIsToggleTrigger, sim::Obj::TF_CollectToggle);
                flag(effect->m_endReversed, sim::Obj::TF_EndReversed);
                flag(effect->m_followCPP, sim::Obj::TF_FollowCPP);
                if (o->m_objectID == 1268) {
                    auto* spawn = static_cast<SpawnTriggerGameObject*>(o);
                    data.spawnDelay = spawn->m_spawnDelay;
                    data.spawnDelayRange = spawn->m_delayRange;
                    flag(spawn->m_resetRemap, sim::Obj::TF_ResetRemap);
                    flag(!spawn->m_remapObjects.empty(), sim::Obj::TF_HasRemap);
                    for (auto const& remap : spawn->m_remapObjects) {
                        data.remapPairs.push_back(remap.m_groupID);
                        data.remapPairs.push_back(remap.m_chance);
                    }
                }
                if (o->m_objectID == 1611 || o->m_objectID == 1811 || o->m_objectID == 1817) {
                    auto* counter = static_cast<CountTriggerGameObject*>(o);
                    data.count = counter->m_pickupCount;
                    data.compare = counter->m_pickupTriggerMode;
                    data.countMult = counter->m_pickupTriggerMultiplier;
                    flag(counter->m_multiActivate, sim::Obj::TF_CountMulti);
                    flag(counter->m_isOverride, sim::Obj::TF_CountOverride);
                }
                if (o->m_objectID == 1616) {
                    auto* control = static_cast<TriggerControlGameObject*>(o);
                    data.stopCmd = static_cast<int>(control->m_customTriggerValue);
                }
                if (o->m_objectID == 1932) {
                    auto* playerControl = static_cast<PlayerControlGameObject*>(o);
                    data.optFlags = (playerControl->m_stopJump ? 1 : 0) | (playerControl->m_stopMove ? 2 : 0) |
                                    (playerControl->m_stopRotation ? 4 : 0) | (playerControl->m_stopSlide ? 8 : 0);
                    data.optValues = 0;
                    data.optFlags |= 16;
                }
                if (o->m_objectID == 2900) {
                    auto* rotate = static_cast<RotateGameplayGameObject*>(o);
                    data.rgMove = rotate->m_moveDirection;
                    data.rgGround = rotate->m_groundDirection;
                    data.rgChannel = rotate->m_targetChannelID;
                    data.rgModX = rotate->m_velocityModX;
                    data.rgModY = rotate->m_velocityModY;
                    data.rgFlags = (rotate->m_editVelocity ? 1 : 0) | (rotate->m_overrideVelocity ? 2 : 0) |
                                   (rotate->m_changeChannel ? 4 : 0) | (rotate->m_channelOnly ? 8 : 0) |
                                   (rotate->m_instantOffset ? 16 : 0) | (rotate->m_dontSlide ? 32 : 0) | 64;
                }
                if (o->m_objectID == 2899) {
                    auto* options = static_cast<GameOptionsTrigger*>(o);
                    auto put = [&](GameOptionsSetting setting, int bit) {
                        if (setting == GameOptionsSetting::Disabled) return;
                        data.optFlags |= bit;
                        if (setting == GameOptionsSetting::On) data.optValues |= bit;
                    };
                    put(options->m_unlinkDualGravity, 1);
                    put(options->m_disableP1Controls, 2);
                    put(options->m_disableP2Controls, 4);
                    put(options->m_boostSlide, 8);
                }
            }
            data.posOffX = o->m_positionXOffset;
            data.posOffY = o->m_positionYOffset;
            data.sws = o->m_spriteWidthScale;
            data.shs = o->m_spriteHeightScale;
            if (o->m_hasGroupParent) data.tflags |= sim::Obj::TF_GroupParent;
            if (o->m_hasAreaParent) data.tflags |= sim::Obj::TF_AreaParent;
            data.linkedGroup = o->m_linkedGroup;
#ifdef GEODE_IS_WINDOWS
            data.moveOpt = *reinterpret_cast<bool const*>(reinterpret_cast<uintptr_t>(o) + offsets::kObjOptimized);
#endif
            {
                auto* teleport = typeinfo_cast<TeleportPortalObject*>(o);
                if (!teleport && (o->m_objectID == 3022 || o->m_objectID == 3027 ||
                                  o->m_objectType == GameObjectType::TeleportPortal ||
                                  o->m_objectType == GameObjectType::TeleportOrb)) {
                    teleport = static_cast<TeleportPortalObject*>(o);
                }
                if (teleport) {
                    data.tpOrange = teleport->m_orangePortal ? teleport->m_orangePortal->m_uniqueID : 0;
                    data.tpYOffset = teleport->m_teleportYOffset;
                    data.tpYellow = teleport->m_isYellowPortal;
                    data.tpGravity = teleport->m_gravityMode;
                    data.tpSaveOffset = teleport->m_saveOffset;
                    data.tpIgnoreX = teleport->m_ignoreX;
                    data.tpIgnoreY = teleport->m_ignoreY;
                    data.tpStaticForce = teleport->m_staticForceEnabled;
                    data.tpStaticForceV = teleport->m_staticForce;
                    data.tpStaticForceAdd = teleport->m_staticForceAdditive;
                    data.tpRedirectDash = teleport->m_redirectDash;
                    data.tpRedirectForce = teleport->m_redirectForceEnabled;
                    data.tpSnapGround = teleport->m_snapGround;
                    data.tpEase = teleport->m_teleportEase;
                    data.tpInstantCam = teleport->m_instantCamera;
                    data.tpRedirectMod = teleport->m_redirectForceMod;
                    data.tpRedirectMin = teleport->m_redirectForceMin;
                    data.tpRedirectMax = teleport->m_redirectForceMax;
                }
            }
            if (o->m_objectID == 2069 || o->m_objectID == 3645) {
                auto* forceBlock = static_cast<ForceBlockGameObject*>(o);
                data.force = forceBlock->m_force;
                data.forceMin = forceBlock->m_minForce;
                data.forceMax = forceBlock->m_maxForce;
                data.forceRelative = forceBlock->m_relativeForce;
                data.forceRange = forceBlock->m_forceRange;
                data.forceKey = forceBlock->m_forceID;
            }
            {
                bool const dashType = o->m_objectType == GameObjectType::DashRing ||
                                      o->m_objectType == GameObjectType::GravityDashRing;
                auto* dashRing = typeinfo_cast<DashRingObject*>(o);
                if (!dashRing && dashType) dashRing = static_cast<DashRingObject*>(o);
                if (dashRing) {
                    data.dashSpeed = dashRing->m_dashSpeed;
                    data.endBoost = dashRing->m_endBoost;
                    data.maxDuration = dashRing->m_maxDuration;
                    data.allowCollide = dashRing->m_allowCollide;
                    data.stopSlide = dashRing->m_stopSlide;
                }
                if (dashType && !(data.dashSpeed > 0.f)) data.dashSpeed = 1.f;
            }
            {
                auto startPos = o->getStartPos();
                data.startX = startPos.x;
                data.startY = startPos.y;
                data.startRotX = o->m_startRotationX;
                data.startRotY = o->m_startRotationY;
                data.startScaleX = o->m_startScaleX;
                data.startScaleY = o->m_startScaleY;
                data.hasStartRS = data.startRotX != data.rotX || data.startRotY != data.rotY ||
                                  data.startScaleX != data.scaleX || data.startScaleY != data.scaleY;
            }
            if (o->m_groups && o->m_groupCount > 0)
                for (int i = 0; i < o->m_groupCount && i < 10; ++i) data.groups.push_back((*o->m_groups)[i]);
            data.secOuter = o->m_outerSectionIndex;
            data.secMiddle = o->m_middleSectionIndex;
            data.secInner = o->m_innerSectionIndex;
            if (sim::wantsExtProps(data.id)) {
                extWant.push_back(ExtWant{w.objs.size(), data.id, data.startX, data.startY});
            }
            if (data.id == sim::ObjId::Text) {
                std::string text = std::string(static_cast<TextGameObject*>(o)->m_text);
                if (!text.empty() && text.size() <= 200) {
                    w.texts.emplace_back(static_cast<int>(w.objs.size()), std::move(text));
                }
            }
            w.objs.push_back(std::move(data));
        }
        fillExtProps(pl, w, extWant);
        w.hasTriggerData = true;
#ifdef GEODE_IS_WINDOWS
        w.moveOptExported = true;
#endif
        if (pl->m_parentGroupsDict) {
            int maxGroup = 0;
            for (auto const& o : w.objs) {
                for (short group : o.groups) maxGroup = std::max(maxGroup, static_cast<int>(group));
            }
            w.exportParents.assign(static_cast<size_t>(maxGroup) + 1, -1);
            std::unordered_map<int, int> byUid;
            for (size_t i = 0; i < w.objs.size(); ++i) byUid[w.objs[i].uid] = static_cast<int>(i);
            for (auto [key, obj] : CCDictionaryExt<int, GameObject*>(pl->m_parentGroupsDict)) {
                if (!obj || key < 0 || key > maxGroup) continue;
                auto it = byUid.find(obj->m_uniqueID);
                if (it != byUid.end()) w.exportParents[static_cast<size_t>(key)] = it->second;
            }
        }
        for (auto& o : w.objs) {
            if (o.remapPairs.size() < 2) continue;
            if (w.spawnRemaps.empty()) w.spawnRemaps.emplace_back();
            std::vector<std::pair<int, int>> pairs;
            for (size_t k = 0; k + 1 < o.remapPairs.size(); k += 2) {
                pairs.emplace_back(o.remapPairs[k], o.remapPairs[k + 1]);
            }
            w.spawnRemaps.push_back(std::move(pairs));
            o.remapIdx = static_cast<int>(w.spawnRemaps.size()) - 1;
        }
        w.areaRand.resize(2000);
        std::memcpy(w.areaRand.data(),
                    reinterpret_cast<void const*>(reinterpret_cast<uintptr_t>(pl) + offsets::kLayerVarianceValues),
                    2000 * sizeof(float));
        w.build();
        sim::analyzeTriggers(w);
        w.build();
        if (w.hasAreaEffects || !w.advDefs.empty()) {
            std::unordered_map<int, short> keep;
            size_t i = 0;
            for (auto* o : CCArrayExt<GameObject*>(pl->m_objects)) {
                if (!o || o == pl->m_anticheatSpike) continue;
                if (i < w.objs.size() && w.areaTarget[i] && w.objs[i].uid == o->m_uniqueID) {
                    keep[o->m_uniqueID] = w.objs[i].areaRi;
                }
                ++i;
            }
            g_areaKey = areaKeyOf(pl);
            g_areaRi = std::move(keep);
        } else {
            forgetAreaRandom();
            for (auto& o : w.objs) o.areaRi = -1;
            w.areaRand.clear();
        }
        return w;
    }

    // one json line for the level, then group parents, area random values, and one line per object
    geode::Result<std::filesystem::path> writeWorld(sim::World const& w, PlayLayer* pl,
                                                    std::filesystem::path const& dir, std::string const& tag) {
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        if (pl && pl->m_level) {
            std::ofstream levelFile(dir / (tag + ".levelstring.txt"), std::ios::out | std::ios::trunc);
            if (levelFile.is_open()) levelFile << std::string(pl->m_level->m_levelString);
        }
        auto path = dir / (tag + ".objects.jsonl");
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (!file.is_open()) return Err(fmt::format("cannot write {}", geode::utils::string::pathToString(path)));
        file << fmt::format(
                R"({{"ev":"level","level":"{}","levelId":{},"length":{},"objects":{},"groundP1":{})"
                R"(,"groundShip":{},"minPortalY":{},"maxPortalY":{},"platformer":{},"maxGameplayY":{})"
                R"(,"twoPlayer":{},"gridSnapOff":{},"enable22":{},"ySection":{},"startX":{},"startY":{})"
                R"(,"startMode":{},"startSpeed":{},"startMini":{},"startDual":{},"rotateGameplay":{})"
                R"(,"fixRobotJump":{},"decreaseBoostSlide":{},"enablePlayerSqueeze":{},"fixGravityBug":{})"
                R"(,"enableImpulseFix":{},"spawnGroup":{},"multiRot":{},"reverseSync":{},"fixRadius":{})"
                R"(,"fixNegScale":{},"staticRotate":{},"trig":{},"winW":{},"winH":{},"endPortal":{},"kA23":{})"
                R"(,"kA24":{},"unk322a":{},"testMode":{},"sortGroups":{},"startFlip":{},"startRev":{})"
                R"(,"areaPar":true,"mopt":{}}})"
                "\n",
                w.levelName, w.levelId, f32(w.length), w.objs.size(), f32(w.groundP1), f32(w.groundShip),
                f32(w.minPortalY), f32(w.maxPortalY), w.platformer, f32(w.maxGameplayY), w.twoPlayer, w.gridSnapOff,
                w.enable22, w.ySection, f32(w.startX), f32(w.startY), w.startMode, w.startSpeed, w.startMini,
                w.startDual, w.rotateGameplay, w.fixRobotJump, w.decreaseBoostSlide, w.enablePlayerSqueeze,
                w.fixGravityBug, w.enableImpulseFix, w.spawnGroup, w.allowMultiRotation, w.reverseSync,
                w.fixRadiusCollision, w.fixNegativeScale, w.allowStaticRotate, w.hasTriggerData ? 1 : 0, f32(w.winW),
                f32(w.winH), w.hasEndPortal, w.camClampA, w.camClampB, w.camUnk322a, w.camTestMode, w.sortGroups,
                w.startFlipped, w.startReverse,
#ifdef GEODE_IS_WINDOWS
                true);
#else
                false);
#endif
        {
            std::string parents;
            for (size_t group = 0; group < w.exportParents.size(); ++group)
                if (w.exportParents[group] >= 0) {
                    parents += fmt::format("{}[{},{}]", parents.empty() ? "" : ",", group,
                                           w.objs[static_cast<size_t>(w.exportParents[group])].uid);
                }
            if (!parents.empty())
                file << fmt::format(R"({{"ev":"parents","p":[{}]}})"
                                    "\n",
                                    parents);
        }
        if ((w.hasAreaEffects || !w.advDefs.empty()) && !w.areaRand.empty()) {
            std::string values;
            for (size_t k = 0; k < w.areaRand.size(); ++k) {
                values += fmt::format("{}{}", k ? "," : "", f32(w.areaRand[k]));
            }
            file << fmt::format(R"({{"ev":"arearand","v":[{}]}})"
                                "\n",
                                values);
        }
        for (auto const& o : w.objs) {
            unsigned flags = 0;
            auto setBit = [&](bool value, int index) {
                if (value) flags |= (1u << index);
            };
            setBit(o.disabled, 0);
            setBit(o.extendedCollision, 1);
            setBit(o.trigger, 2);
            setBit(o.passable, 3);
            setBit(o.noTouch, 4);
            setBit(o.decoration, 5);
            setBit(o.hide, 6);
            setBit(o.gripSlope, 7);
            setBit(o.slopeUphill, 8);
            setBit(o.slopeHazard, 9);
            setBit(o.flipX, 10);
            setBit(o.flipY, 11);
            setBit(o.invisibleBlock, 12);
            setBit(o.groupDisabled, 13);
            setBit(o.rotationAligned, 15);
            setBit(o.activated, 17);
            setBit(o.iceBlock, 19);
            setBit(o.dontBoostY, 20);
            setBit(o.dontBoostX, 21);
            setBit(o.extraSticky, 22);
            setBit(o.nonStickX, 23);
            setBit(o.nonStickY, 24);
            setBit(o.scaleStick, 25);
            setBit(o.highDetail, 26);
            setBit(o.useOuterOb, 27);
            setBit(o.hasNoEffects, 30);
            setBit(o.claimTouch, 31);
            std::string extra;
            if (o.cls == 1) {
                extra += fmt::format(
                        R"(,"tgt":{},"dur":{},"touch":{},"spawn":{},"multi":{},"multiAct":{},"rev":{},"ch":{})"
                        R"(,"single":{})",
                        o.targetGroup, f32(o.duration), o.touchTriggered, o.spawnTriggered, o.multiTriggered,
                        o.multiActivate, o.reverse, o.channel, o.singlePTouch);
                auto addInt = [&](char const* key, int value) {
                    if (value) extra += fmt::format(R"(,"{}":{})", key, value);
                };
                auto addFloat = [&](char const* key, float value) {
                    if (value != 0.f) extra += fmt::format(R"(,"{}":{})", key, f32(value));
                };
                addInt("ctr", o.centerGroup);
                if (o.mvX != 0.f || o.mvY != 0.f) extra += fmt::format(R"(,"mv":[{},{}])", f32(o.mvX), f32(o.mvY));
                addInt("ease", o.easing);
                addFloat("rate", o.easingRate);
                addInt("mtm", o.moveTargetMode);
                addInt("tmc", o.targetModCenter);
                addFloat("mmx", o.moveModX);
                addFloat("mmy", o.moveModY);
                addFloat("dmd", o.dirDistance);
                addFloat("rdeg", o.rotDeg);
                addInt("t360", o.times360);
                addInt("rtid", o.rotTarget);
                addFloat("roff", o.rotOffset);
                if (o.aimExt && (o.tflags & (sim::Obj::TF_UseMoveTarget | sim::Obj::TF_DirSnap))) {
                    extra += fmt::format(R"(,"aim":[{},{},{},{},{}])", o.rotMinX, o.rotMinY, o.rotMaxX, o.rotMaxY,
                                         o.dynEasing);
                }
                addFloat("fxm", o.followXMod);
                addFloat("fym", o.followYMod);
                addFloat("fys", o.fySpeed);
                addFloat("fyd", o.fyDelay);
                addInt("fyo", o.fyOffset);
                addFloat("fymax", o.fyMaxSpeed);
                addInt("ttm", o.touchToggleMode);
                addInt("tpm", o.touchPlayerMode);
                addInt("item", o.itemID);
                addInt("item2", o.itemID2);
                addInt("ctl", o.controlID);
                addFloat("zoom", o.zoom);
                addFloat("tw", o.timeWarpMod);
                addFloat("sdel", o.spawnDelay);
                addFloat("sdr", o.spawnDelayRange);
                addInt("cnt", o.count);
                addInt("cmp", o.compare);
                addFloat("cmul", o.countMult);
                addInt("stop", o.stopCmd);
                addInt("opt", o.optFlags);
                if (o.optFlags) extra += fmt::format(R"(,"optv":{})", o.optValues);
                if (o.ringSpawnOnly) extra += ",\"rso\":true";
                addFloat("grav", o.gravityValue);
                addInt("ord", o.ord);
                addInt("cam", o.camFlags);
                if (!o.remapPairs.empty()) {
                    extra += ",\"remap\":[";
                    for (size_t k = 0; k < o.remapPairs.size(); ++k) {
                        extra += fmt::format("{}{}", k ? "," : "", o.remapPairs[k]);
                    }
                    extra += "]";
                }
                addInt("rgm", o.rgMove);
                addInt("rgg", o.rgGround);
                addInt("rgf", o.rgFlags);
                addInt("rgc", o.rgChannel);
                addFloat("rgx", o.rgModX);
                addFloat("rgy", o.rgModY);
            }
            if (auto const* ext = w.extOf(o)) extra += fmt::format(R"(,"kv":"{}")", sim::formatExtProps(*ext));
            if (o.id == sim::ObjId::Text) {
                size_t const index = static_cast<size_t>(&o - w.objs.data());
                auto it = std::lower_bound(w.texts.begin(), w.texts.end(), static_cast<int>(index),
                                           [](auto const& entry, int value) { return entry.first < value; });
                if (it != w.texts.end() && it->first == static_cast<int>(index)) {
                    std::string escaped;
                    for (char c : it->second) {
                        unsigned char const byte = static_cast<unsigned char>(c);
                        if (c == '"' || c == '\\') {
                            escaped += '\\';
                            escaped += c;
                        } else if (byte < 0x20 || byte >= 0x7f)
                            escaped += ' ';
                        else
                            escaped += c;
                    }
                    extra += fmt::format(R"(,"txt":"{}")", escaped);
                }
            }
            if (o.tflags) extra += fmt::format(R"(,"tf":{})", o.tflags);
            if (o.collectPoints) extra += fmt::format(R"(,"cpts":{})", o.collectPoints);
            if (o.animOnTrigger) extra += R"(,"aot":true)";
            if (o.linkedGroup) extra += fmt::format(R"(,"lnk":{})", o.linkedGroup);
            if (o.moveOpt) extra += R"(,"mopt":true)";
            if (o.posOffX != 0.f || o.posOffY != 0.f) {
                extra += fmt::format(R"(,"po":[{},{}])", f32(o.posOffX), f32(o.posOffY));
            }
            if (o.sws != 0.f || o.shs != 0.f) extra += fmt::format(R"(,"sws":{},"shs":{})", f32(o.sws), f32(o.shs));
            if (o.tpOrange || o.tpGravity || o.tpYellow || o.id == sim::ObjId::TeleportTrigger || o.tpSaveOffset ||
                o.tpIgnoreX || o.tpIgnoreY || o.tpStaticForce || o.tpRedirectDash || o.tpRedirectForce ||
                o.tpSnapGround || o.tpEase || o.tpInstantCam)
                extra += fmt::format(
                        R"(,"tp":{},"tpy":{},"tpyellow":{},"tpgrav":{},"tpsave":{},"tpix":{},"tpiy":{},"tpsf":{})"
                        R"(,"tpsfv":{},"tpsfa":{},"tprd":{},"tprf":{},"tpsg":{},"tprfm":{},"tprfmin":{})"
                        R"(,"tprfmax":{},"tpease":{},"tpic":{})",
                        o.tpOrange, f32(o.tpYOffset), o.tpYellow, o.tpGravity, o.tpSaveOffset, o.tpIgnoreX, o.tpIgnoreY,
                        o.tpStaticForce, f32(o.tpStaticForceV), o.tpStaticForceAdd, o.tpRedirectDash, o.tpRedirectForce,
                        o.tpSnapGround, f32(o.tpRedirectMod), f32(o.tpRedirectMin), f32(o.tpRedirectMax), o.tpEase,
                        o.tpInstantCam);
            if (o.id == sim::ObjId::ForceBlock || o.id == sim::ObjId::ForceCircle)
                extra += fmt::format(R"(,"fb":[{},{},{},{},{},{}])", f32(o.force), f32(o.forceMin), f32(o.forceMax),
                                     o.forceRelative ? 1 : 0, o.forceRange ? 1 : 0, o.forceKey);
            if (o.dashSpeed != 0.f || o.maxDuration != 0.f)
                extra +=
                        fmt::format(R"(,"dashSpeed":{},"endBoost":{},"maxDur":{},"allowCollide":{},"stopSlide":{})",
                                    f32(o.dashSpeed), f32(o.endBoost), f32(o.maxDuration), o.allowCollide, o.stopSlide);
            extra += fmt::format(R"(,"sp":[{},{}])", f32(o.startX), f32(o.startY));
            if (size_t const index = static_cast<size_t>(&o - w.objs.data());
                o.areaRi >= 0 && index < w.areaTarget.size() && w.areaTarget[index])
                extra += fmt::format(R"(,"ri":{})", o.areaRi);
            if (o.material != 0) extra += fmt::format(R"(,"mat":{})", o.material);
            if (o.hasStartRS)
                extra += fmt::format(R"(,"srs":[{},{},{},{}])", f32(o.startRotX), f32(o.startRotY), f32(o.startScaleX),
                                     f32(o.startScaleY));
            std::string groups;
            for (size_t i = 0; i < o.groups.size(); ++i) groups += fmt::format("{}{}", i ? "," : "", o.groups[i]);
            std::string obb;
            if (o.hasObb) {
                obb = ",\"obb\":[";
                for (int i = 0; i < 4; ++i) {
                    obb += fmt::format("{}{},{}", i ? "," : "", f32(o.obb[i * 2]), f32(o.obb[i * 2 + 1]));
                }
                obb += "]";
            }
            file << fmt::format(
                    R"({{"uid":{},"id":{},"x":{},"y":{},"px":{},"py":{},"rot":{},"rx":{},"ry":{},"sx":{},"sy":{})"
                    R"(,"type":{},"cls":{},"rect":[{},{},{},{}],"w":{},"h":{},"box":[{},{}],"rad":{},"sdir":{})"
                    R"(,"f":{},"g":[{}]{}{},"sec":[{},{},{}]}})"
                    "\n",
                    o.uid, o.id, f64(o.posX), f64(o.posY), f32(o.x), f32(o.y), f32(o.rot), f32(o.rotX), f32(o.rotY),
                    f32(o.scaleX), f32(o.scaleY), o.type, o.cls, f32(o.rect.x), f32(o.rect.y), f32(o.rect.w),
                    f32(o.rect.h), f32(o.width), f32(o.height), f32(o.boxOffX), f32(o.boxOffY), f32(o.radius),
                    o.slopeDir, flags, groups, extra, obb, o.secOuter, o.secMiddle, o.secInner);
        }
        return Ok(path);
    }
}
