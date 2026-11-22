// Geode hooks on PlayLayer, GJBaseGameLayer, the keyboard and the effect manager: plugs the mod into the game loop,
// resets, deaths, inputs and keys, and decides how many physics steps the game plays per frame.

#include "log.hpp"
#include "offsets.hpp"
#include "engine.hpp"
#include "macro.hpp"
#include "overlay.hpp"
#include "recorder.hpp"
#include "settings.hpp"
#include "simplanner.hpp"
#include "world_export.hpp"
#include "actions.hpp"
#include "editor.hpp"
#include "input_recorder.hpp"
#include "ui/menu.hpp"

#include <Geode/Geode.hpp>
#include <limits>
#include <chrono>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJEffectManager.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <algorithm>

using namespace geode::prelude;
using ap::Engine;
using ap::SimPlanner;

namespace {
    std::vector<geode::Ref<CCNode>> s_pausedNodes;

    // adds node and all its children to out
    void collectNodes(CCNode* node, std::vector<geode::Ref<CCNode>>& out) {
        if (!node) return;
        out.emplace_back(node);
        if (auto* children = node->getChildren())
            for (auto* child : CCArrayExt<CCNode*>(children)) collectNodes(child, out);
    }

    // pauses both players' cocos actions so the dash rotation doesn't run on real time during a freeze
    void pausePlayerActions(PlayLayer* pl) {
        if (!s_pausedNodes.empty() || !pl) return;
        collectNodes(pl->m_player1, s_pausedNodes);
        collectNodes(pl->m_player2, s_pausedNodes);
        auto* actionManager = CCDirector::sharedDirector()->getActionManager();
        for (auto& node : s_pausedNodes) actionManager->pauseTarget(node.data());
    }

    // resumes the actions paused by pausePlayerActions
    void resumePlayerActions() {
        if (s_pausedNodes.empty()) return;
        auto* actionManager = CCDirector::sharedDirector()->getActionManager();
        for (auto& node : s_pausedNodes) actionManager->resumeTarget(node.data());
        s_pausedNodes.clear();
    }

    std::vector<GameObject*> s_tfCand;
    PlayLayer* s_tfLayer = nullptr;

    // true if the object matters for gameplay (not decoration, not a trigger)
    bool gameplayObject(GameObject* obj) {
        if (!obj) return false;
        int const type = static_cast<int>(obj->m_objectType);
        return type != static_cast<int>(GameObjectType::Decoration) &&
               type != static_cast<int>(GameObjectType::Modifier);
    }

    // fills s_tfCand once the level is loaded
    void collectTransformCandidates(PlayLayer* pl) {
        s_tfCand.clear();
        s_tfLayer = pl;
        if (!pl || !pl->m_objects) return;
        for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects))
            if (gameplayObject(obj) && obj->m_groups && obj->m_groupCount > 0) s_tfCand.push_back(obj);
    }

    // copies scale and rotation to the nodes of gameplay objects after every step
    void resyncGameplayObjects(GJBaseGameLayer* layer) {
        auto const base = reinterpret_cast<uintptr_t>(layer);
        int const count = *reinterpret_cast<int const*>(base + ap::offsets::kLayerAreaObjectCount);
        if (count > 0) {
            auto* const* areaObjects =
                    *reinterpret_cast<GameObject* const* const*>(base + ap::offsets::kLayerAreaObjects);
            for (int i = 0; i < count && areaObjects; ++i) {
                if (GameObject* obj = areaObjects[i]; gameplayObject(obj)) {
                    obj->setRRotation(0.f);
                    obj->setRScale(1.f);
                }
            }
        }
        if (!s_tfLayer || static_cast<GJBaseGameLayer*>(s_tfLayer) != layer) return;
        int const commandIndex = *reinterpret_cast<int const*>(base + ap::offsets::kLayerCommandIndex);
        for (GameObject* obj : s_tfCand) {
            if (*reinterpret_cast<int const*>(reinterpret_cast<uintptr_t>(obj) + ap::offsets::kObjTransformStamp) ==
                commandIndex)
                obj->setRScale(1.f);
        }
    }

    // updates the status line and the drawn path, called every frame
    void refreshUi(PlayLayer* pl) {
        auto& planner = SimPlanner::get();
        auto& engine = Engine::get();
        auto const& settings = ap::Settings::get();
        (void)pl;

        auto withRecording = [](std::string text) {
            if (ap::Recorder::get().enabled()) text += " (debug recording)";
            return text;
        };
        if (planner.phase() == SimPlanner::Phase::Live) {
            {
                static auto lastStatus = std::chrono::steady_clock::time_point{};
                auto const now = std::chrono::steady_clock::now();
                if (now - lastStatus > std::chrono::milliseconds(100)) {
                    lastStatus = now;
                    ap::Overlay::setStatus(withRecording(planner.statusText()));
                }
            }
            if (settings.overlay) {
                static unsigned lastVersion = ~0u;
                static int lastBucket = -1;
                static auto lastDraw = std::chrono::steady_clock::time_point{};
                int const liveStep = planner.liveStep();
                int const bucket = liveStep / 240;
                unsigned const version = planner.pathVersion();
                auto const now = std::chrono::steady_clock::now();
                if ((version != lastVersion || bucket != lastBucket) &&
                    (bucket != lastBucket || now - lastDraw > std::chrono::milliseconds(200))) {
                    lastVersion = version;
                    lastBucket = bucket;
                    lastDraw = now;
                    int const from = std::max(0, bucket * 240 - 240);
                    ap::Overlay::drawPoints(planner.plannedPath(from, from + 3600), from);
                }
            } else {
                ap::Overlay::clearPath();
            }
        } else if (planner.phase() == SimPlanner::Phase::OfflineSearch) {
            ap::Overlay::setStatus(withRecording(planner.statusText()));
        } else if (engine.playing()) {
            ap::Overlay::setStatus(
                    withRecording(fmt::format("Auto Pathing Tool: playing the macro ({} inputs) - {} to stop",
                                              engine.playMacroInputs(), ap::keyName(ap::Action::Play))));
        } else if (planner.phase() == SimPlanner::Phase::OfflineDone ||
                   planner.phase() == SimPlanner::Phase::OfflineFailed) {
            ap::Overlay::setStatus(withRecording(planner.statusText()));
        } else {
            ap::Overlay::setStatus(ap::Recorder::get().statusText());
            ap::Overlay::clearPath();
        }
    }

    //starts or stops the search, live or offline
    void toggleSolve(bool live) {
        auto* pl = PlayLayer::get();
        if (!pl) return;
        auto& planner = SimPlanner::get();
        if (planner.running()) {
            planner.stop();
            Notification::create("Auto Pathing Tool: stopped", NotificationIcon::Info)->show();
            return;
        }
        if (Engine::get().playing()) Engine::get().stopPlayback();
        if (pl->m_isPlatformer) {
            Notification::create("Auto Pathing Tool: platformer levels are not supported", NotificationIcon::Error)
                    ->show();
            return;
        }
        if (!planner.hasWorld()) {
            Notification::create("Auto Pathing Tool: this level could not be loaded into the simulator",
                                 NotificationIcon::Error)
                    ->show();
            return;
        }
        bool started = live ? planner.startLive(pl) : planner.startOffline(pl);
        if (!started) Notification::create("Auto Pathing Tool: cannot start here", NotificationIcon::Error)->show();
    }

    // starts or stops playing the last macro found
    void togglePlay() {
        auto* pl = PlayLayer::get();
        if (!pl) return;
        auto& engine = Engine::get();
        if (SimPlanner::get().running()) return;
        if (engine.playing()) {
            engine.stopPlayback();
            Notification::create("Auto Pathing Tool: playback stopped", NotificationIcon::Info)->show();
            return;
        }
        auto const& macro = SimPlanner::get().macro();
        if (macro.empty()) {
            Notification::create(
                    fmt::format("Auto Pathing Tool: no macro to play yet (run the bot first with {} or {})",
                                ap::keyName(ap::Action::Offline), ap::keyName(ap::Action::Live)),
                    NotificationIcon::Warning)
                    ->show();
            return;
        }
        engine.startPlayback(macro);
    }

    // writes the last macro found as a .gdr file in the mod save folder
    void exportMacro() {
        auto* pl = PlayLayer::get();
        auto const& macro = SimPlanner::get().macro();
        if (!pl || macro.empty()) {
            Notification::create("Auto Pathing Tool: no macro to export", NotificationIcon::Warning)->show();
            return;
        }
        std::string levelName = pl->m_level ? std::string(pl->m_level->m_levelName) : "level";
        int levelId = pl->m_level ? static_cast<int>(pl->m_level->m_levelID.value()) : 0;
        auto result = macro.exportGDR(Mod::get()->getSaveDir() / "macros", levelName, levelId,
                                      Mod::get()->getVersion().toVString());
        if (result) {
            ap::Log::printf("macro exported: {}", geode::utils::string::pathToString(result.unwrap()));
            Notification::create("Auto Pathing Tool: macro exported (mod save folder, macros/)",
                                 NotificationIcon::Success)
                    ->show();
        } else {
            Notification::create(fmt::format("Auto Pathing Tool: export failed: {}", result.unwrapErr()),
                                 NotificationIcon::Error)
                    ->show();
        }
    }
}

namespace {
    struct SafeState {
        PlayLayer* layer = nullptr;
        bool botTouched = false;
        bool snap = false;
        int jumpsStat = 0;
        int levelJumps = 0;
    };
    SafeState g_safe;

    // true if the bot is controlling the game right now
    bool botControls() {
        auto& engine = Engine::get();
        return engine.playing() || engine.active() || SimPlanner::get().phase() == SimPlanner::Phase::Live;
    }

    // saves the jump counters (stat "1" and level jumps) to restore them later
    void safeSnapshot(PlayLayer* pl) {
        g_safe.snap = true;
        g_safe.jumpsStat = GameStatsManager::sharedState()->getStat("1");
        g_safe.levelJumps = pl->m_level ? static_cast<int>(pl->m_level->m_jumps.value()) : 0;
    }

    // marks the attempt as touched once the bot takes control
    void safeObserve(PlayLayer* pl) {
        if (g_safe.layer != pl) g_safe = SafeState{pl};
        if (botControls()) {
            if (!g_safe.botTouched && !g_safe.snap) safeSnapshot(pl);
            g_safe.botTouched = true;
        }
    }

    // returns true if safe mode must stop this attempt from counting
    bool safeBlocks(PlayLayer* pl) {
        safeObserve(pl);
        return ap::Settings::get().safeMode && g_safe.botTouched;
    }

    // puts the jump counters back to their saved values
    void safeRestoreJumps(PlayLayer* pl) {
        if (!safeBlocks(pl) || !g_safe.snap) return;
        GameStatsManager::sharedState()->setStat("1", g_safe.jumpsStat);
        if (pl->m_level) pl->m_level->m_jumps = g_safe.levelJumps;
    }

    // new attempt (right after resetLevel), played by the bot or not
    void safeNewAttempt(PlayLayer* pl, bool controlled) {
        g_safe = SafeState{pl};
        if (controlled) {
            g_safe.botTouched = true;
            safeSnapshot(pl);
        }
    }
}

class $modify(APPlayLayer, PlayLayer){// level is fully built: load it into the simulator and attach the mod
                                      void setupHasCompleted(){PlayLayer::setupHasCompleted();
Engine::get().attach(this);
Engine::get().captureFresh(this);
collectTransformCandidates(this);
ap::Overlay::attach(this);
ap::Recorder::get().onLevelLoaded(this);
SimPlanner::get().onLevelLoaded(this);
ap::Editor::get().onLevelLoaded(this);
ap::InputRecorder::get().onLevelLoaded(this);
g_safe = SafeState{this};
}

//new attempt: keep the rng and the start state in sync with the simulator
void resetLevel() {
    auto& engine = Engine::get();
    if (engine.restoring()) {
        auto* statsManager = GameStatsManager::sharedState();
        int const totalAttempts = statsManager->getStat("2");
        int const levelAttempts = m_level ? static_cast<int>(m_level->m_attempts.value()) : 0;
        PlayLayer::resetLevel();
        if (engine.playing() || SimPlanner::get().phase() == SimPlanner::Phase::Live)
            Engine::alignRngAfterReset(this);
        else
            Engine::s_rngAligned = false;
        ap::applyAreaRandom(this);
        statsManager->setStat("2", totalAttempts);
        if (m_level) m_level->m_attempts = levelAttempts;
        engine.onResetLevel(this);
        return;
    }
    engine.onPreResetLevel(this);
    SimPlanner::get().onPreRealReset(this);
    if (engine.playing() || SimPlanner::get().phase() == SimPlanner::Phase::Live) Engine::seedTpRng(Engine::kTpSeed);
    ap::Recorder::get().onPreReset(this);
    int const stepsBefore = engine.stepCounter();
    safeRestoreJumps(this);
    auto* safeStatsManager = GameStatsManager::sharedState();
    int const safeTotalAttempts = safeStatsManager->getStat("2");
    int const safeLevelAttempts = m_level ? static_cast<int>(m_level->m_attempts.value()) : 0;
    PlayLayer::resetLevel();
    bool const controlled = engine.playing() || SimPlanner::get().phase() == SimPlanner::Phase::Live;
    safeNewAttempt(this, controlled || engine.active());
    if (controlled && ap::Settings::get().safeMode) {
        safeStatsManager->setStat("2", safeTotalAttempts);
        if (m_level) m_level->m_attempts = safeLevelAttempts;
    }
    if (controlled)
        Engine::alignRngAfterReset(this);
    else
        Engine::s_rngAligned = false;
    if (controlled) engine.checkResetSteps(this, stepsBefore);
    ap::applyAreaRandom(this);
    if (controlled) {
        engine.applyFresh(this);
        m_resumeTimer = 0;
    }
    engine.onResetLevel(this);
    ap::Editor::get().onNewAttempt();
    ap::InputRecorder::get().onNewAttempt(this);
    SimPlanner::get().onRealReset(this);
    ap::Recorder::get().onReset(this);
}

// death: let the engine drop it, notify the mod, and keep safe mode attempts out of the stats
void destroyPlayer(PlayerObject* player, GameObject* object) {
    if (Engine::get().onDestroyPlayer(this, player, object)) return;
    if (object != m_anticheatSpike && player && !player->m_isDead) {
        ap::Recorder::get().onDeath(this, player, object, Engine::get().curStep());
        SimPlanner::get().onRealDeath(this, Engine::get().curStep(), object);
        ap::InputRecorder::get().onDeath(this, Engine::get().curStep());
    }
    bool const safe = object != m_anticheatSpike && safeBlocks(this);
    if (safe) safeRestoreJumps(this);
    bool const wasTest = m_isTestMode;
    if (safe) m_isTestMode = true;
    PlayLayer::destroyPlayer(player, object);
    if (safe) m_isTestMode = wasTest;
}

// level end: same as destroyPlayer, the completion doesn't count in safe mode
void levelComplete() {
    if (Engine::get().onLevelComplete(this)) return;
    ap::Recorder::get().onComplete(Engine::get().curStep());
    SimPlanner::get().onRealComplete(this, Engine::get().curStep());
    ap::InputRecorder::get().onComplete(this, Engine::get().curStep());
    bool const safe = safeBlocks(this);
    if (safe) safeRestoreJumps(this);
    bool const wasTest = m_isTestMode;
    if (safe) m_isTestMode = true;
    PlayLayer::levelComplete();
    if (safe) m_isTestMode = wasTest;
}

// no end animation when the engine skipped the completion
void playEndAnimationToPos(cocos2d::CCPoint pos) {
    if (Engine::get().onLevelComplete(this)) return;
    PlayLayer::playEndAnimationToPos(pos);
}

// the engine finishes restoring its state after a checkpoint load
void loadFromCheckpoint(CheckpointObject* cp) {
    PlayLayer::loadFromCheckpoint(cp);
    Engine::get().onLoadFromCheckpoint(this);
}

// no checkpoints while the engine drives the ticks
CheckpointObject* markCheckpoint() {
    if (Engine::get().active()) return nullptr;
    return PlayLayer::markCheckpoint();
}

// same as markCheckpoint
void storeCheckpoint(CheckpointObject* cp) {
    if (Engine::get().active()) return;
    PlayLayer::storeCheckpoint(cp);
}

// no music during internal resets and offline runs
void startMusic() {
    if (Engine::get().suppressCosmetics()) return;
    PlayLayer::startMusic();
}

// no spawn circle during internal resets and offline runs
void spawnCircle() {
    if (Engine::get().suppressCosmetics()) return;
    PlayLayer::spawnCircle();
}

// no "Attempt N" label update during internal resets and offline runs
void updateAttempts() {
    if (Engine::get().suppressCosmetics()) return;
    PlayLayer::updateAttempts();
}

// leaving the level: forget everything that points to it before it gets destroyed
void onQuit() {
    safeRestoreJumps(this);
    g_safe = SafeState{};
    ap::forgetAreaRandom();
    resumePlayerActions();
    s_tfCand.clear();
    s_tfLayer = nullptr;
    SimPlanner::get().onLayerGone();
    ap::Recorder::get().onLayerGone();
    ap::Editor::get().onLayerGone();
    ap::InputRecorder::get().onLayerGone();
    Engine::get().onLayerGone();
    ap::Overlay::detach();
    PlayLayer::onQuit();
}
}
;

namespace {
    struct DashArt {
        PlayerObject* p = nullptr;
        int step = 0;
        float sign = 1.f;
    };
    DashArt s_dashArt[2];

    // true when the mod is playing (macro playback or live bot)
    bool modPlays() { return Engine::get().playing() || SimPlanner::get().phase() == SimPlanner::Phase::Live; }

    // slot of the player, a new one is given if create is true
    DashArt* dashArtOf(PlayerObject* player, bool create) {
        for (auto& slot : s_dashArt)
            if (slot.p == player) return &slot;
        if (!create) return nullptr;
        for (auto& slot : s_dashArt) {
            if (!slot.p) {
                slot.p = player;
                return &slot;
            }
        }
        s_dashArt[1] = DashArt{player};
        return &s_dashArt[1];
    }

    // true for the modes where the icon spins during a dash (cube and ball)
    bool dashIconMode(PlayerObject* player) {
        return !player->m_isShip && !player->m_isBird && !player->m_isDart && !player->m_isSwing &&
               !player->m_isRobot && !player->m_isSpider && !player->m_isPlatformer;
    }
}

class $modify(APPlayerObject, PlayerObject){// dash start: remember the step and the direction of the icon rotation
                                            void updateDashArt(){PlayerObject::updateDashArt();
if (m_isDashing && dashIconMode(this)) {
    if (auto* slot = dashArtOf(this, true)) {
        slot->step = Engine::get().curStep();
        slot->sign = ((m_isSideways != m_isUpsideDown) ? -1.f : 1.f) * m_gravityMod;
    }
}
}

// dash end: the game reads the icon rotation here, so set it to the simulator's value first
void stopDashing() {
    if (m_isDashing && dashIconMode(this) && m_iconSprite && modPlays()) {
        if (auto* slot = dashArtOf(this, false)) {
            int const frames = std::max(0, Engine::get().curStep() - slot->step - 1);
            m_iconSprite->setRotation(slot->sign * (288.f / 0.3f) * (1.f / 240.f) * static_cast<float>(frames));
        }
    }
    PlayerObject::stopDashing();
}

// no spawn effect during internal resets and offline runs
void playSpawnEffect() {
    if (Engine::get().suppressCosmetics()) return;
    PlayerObject::playSpawnEffect();
}

// no spawn circle during internal resets and offline runs
void spawnCircle() {
    if (Engine::get().suppressCosmetics()) return;
    PlayerObject::spawnCircle();
}
}
;

class $modify(APPauseLayer,
              PauseLayer){// going to the editor leaves the level without onQuit, so do the same cleanup here
                          void goEdit(){if (auto* pl = PlayLayer::get()) safeRestoreJumps(pl);
g_safe = SafeState{};
resumePlayerActions();
s_tfCand.clear();
s_tfLayer = nullptr;
SimPlanner::get().onLayerGone();
ap::Recorder::get().onLayerGone();
ap::Editor::get().onLayerGone();
ap::InputRecorder::get().onLayerGone();
Engine::get().onLayerGone();
ap::Overlay::detach();
PauseLayer::goEdit();
}
}
;

namespace {
    double g_frameGameSec = 0.0;

    struct FrameScope {
        std::chrono::steady_clock::time_point t0;
        int steps0;

        // starts timing a frame
        explicit FrameScope(int startSteps) : t0(std::chrono::steady_clock::now()), steps0(startSteps) {
            g_frameGameSec = 0.0;
        }

        // ends the frame timing, logs slow frames
        ~FrameScope() {
            static std::chrono::steady_clock::time_point s_last{};
            auto const now = std::chrono::steady_clock::now();
            double const hookSec = std::chrono::duration<double>(now - t0).count();
            double const frameSec = s_last == std::chrono::steady_clock::time_point{}
                                            ? 0.0
                                            : std::chrono::duration<double>(t0 - s_last).count();
            s_last = t0;
            SimPlanner::get().frameTiming(frameSec, g_frameGameSec, std::max(0.0, hookSec - g_frameGameSec),
                                          Engine::get().stepCounter() - steps0);
        }
    };
}

class $modify(APBaseGameLayer,
              GJBaseGameLayer){// not a hook: the original update, timed for FrameScope
                               void apGameUpdate(float delta){auto const start = std::chrono::steady_clock::now();
GJBaseGameLayer::update(delta);
g_frameGameSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// game loop: decides how many physics steps the game plays this frame when the mod is playing
void update(float dt) {
    auto& engine = Engine::get();
    auto* pl = PlayLayer::get();
    if (!pl || this != static_cast<GJBaseGameLayer*>(pl)) return GJBaseGameLayer::update(dt);
    if (engine.inStep()) return GJBaseGameLayer::update(dt);
    if (ap::Editor::get().active()) {
        ap::Editor::get().frame(pl, dt);
        return;
    }

    FrameScope const frameScope(engine.stepCounter());
    auto& planner = SimPlanner::get();
    planner.frame(pl);
    safeObserve(pl);
    if (planner.frozen()) {
        pausePlayerActions(pl);
        refreshUi(pl);
        return;
    }
    resumePlayerActions();
    ap::Editor::get().beforeFrame(pl);
    if (planner.phase() == SimPlanner::Phase::Live || engine.playing()) {
        static double s_acc = 0.0;
        static PlayLayer* s_accLayer = nullptr;
        if (s_accLayer != pl) {
            s_accLayer = pl;
            s_acc = 0.0;
        }
        float timeWarp = pl->m_gameState.m_timeWarp;
        if (!(timeWarp > 0.f) || timeWarp > 1.f) timeWarp = 1.f;
        int constexpr cadence = ap::sim::kModCadence;
        if (!planner.fixedCadence() && engine.stepCounter() >= cadence) {
            if (s_acc > 0.0) {
                pl->m_extraDelta += s_acc;
                s_acc = 0.0;
            }
            if (int const budget = planner.liveStepBudget(); budget < std::numeric_limits<int>::max()) {
                double const neededSteps =
                        (static_cast<double>(dt) + pl->m_extraDelta) * 240.0 / static_cast<double>(timeWarp);
                if (neededSteps > static_cast<double>(budget) + 0.5) {
                    pl->m_extraDelta = 0.0;
                    if (budget >= 1) apGameUpdate(static_cast<float>(budget) * timeWarp / 240.f);
                    refreshUi(pl);
                    return;
                }
            }
            apGameUpdate(dt);
            refreshUi(pl);
            return;
        }
        float const frameDt = static_cast<float>(cadence) * timeWarp / 240.f;
        s_acc += static_cast<double>(dt) + pl->m_extraDelta;
        pl->m_extraDelta = 0.0;
        int frames = static_cast<int>(s_acc / static_cast<double>(frameDt));
        s_acc -= static_cast<double>(frames) * static_cast<double>(frameDt);
        if (int const budget = planner.liveStepBudget(); budget / cadence < frames) {
            frames = std::max(budget / cadence, 0);
            s_acc = 0.0;
        }
        for (int i = 0; i < frames && PlayLayer::get() == pl; ++i) {
            pl->m_extraDelta = 0.0;
            apGameUpdate(frameDt);
        }
        refreshUi(pl);
        return;
    }
    apGameUpdate(dt);
    refreshUi(pl);
}

// move step of the game, plus the node copy the simulator expects every step
void processMoveActionsStep(float dt, bool visibleFrame) {
    GJBaseGameLayer::processMoveActionsStep(dt, visibleFrame);
    if (!visibleFrame && modPlays()) {
        if (auto* pl = PlayLayer::get(); pl && this == static_cast<GJBaseGameLayer*>(pl)) resyncGameplayObjects(this);
    }
}

// start of each tick: count it and let the recorder and the planner read the real player
void processCommands(float dt, bool isHalfTick, bool isLastTick) {
    auto& engine = Engine::get();
    engine.onProcessCommands(this, isHalfTick);
    if (!isHalfTick && !engine.active() && !engine.restoring()) {
        if (auto* pl = PlayLayer::get(); pl && this == static_cast<GJBaseGameLayer*>(pl)) {
            ap::Recorder::get().onTick(pl, engine.curStep(), isLastTick);
            SimPlanner::get().onRealTick(pl, engine.curStep());
        }
    }
    GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
}

// inject the macro or live bot inputs before the game handles the queued buttons
void processQueuedButtons(float dt, bool clearInputQueue) {
    if (ap::Editor::get().active()) {
        ap::Editor::get().injectInput(this, Engine::get().curStep());
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
        return;
    }
    ap::InputRecorder::get().onQueuedButtons(this, Engine::get().curStep());
    Engine::get().onProcessQueuedButtons(this);
    SimPlanner::get().onQueuedButtons(this, Engine::get().curStep());
    GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
}
}
;

class $modify(APEffectManager, GJEffectManager){
        // optimized objects only move on the last step of a frame, force every step when one that matters moves
        void prepareMoveActions(float dt, bool intermediate){if (intermediate && modPlays()){
                if (auto* pl = PlayLayer::get();
                    pl && pl->m_effectManager == this && SimPlanner::get().forceOptimizedMoves(Engine::get().curStep()))
                        intermediate = false;
}
GJEffectManager::prepareMoveActions(dt, intermediate);
}
}
;

class $modify(APEnhancedGameObject, EnhancedGameObject){
        // the mod resets m_totalTime each attempt, a negative animation time would crash random variant animations
        void updateSyncedAnimation(float totalTime, int frameIndex){auto const base = reinterpret_cast<uintptr_t>(this);
float& start = *reinterpret_cast<float*>(base + ap::offsets::kAnimStart);
bool const waits = *reinterpret_cast<uint8_t const*>(base + ap::offsets::kAnimWaits) != 0;
float elapsed = totalTime + start;
if (waits) elapsed = elapsed + *reinterpret_cast<float const*>(base + ap::offsets::kAnimWaitSum);
if (!(elapsed >= 0.f)) {
    start = -totalTime;
    if (waits) {
        *reinterpret_cast<float*>(base + ap::offsets::kAnimWaitSum) = 0.f;
        *reinterpret_cast<float*>(base + ap::offsets::kAnimWait) = 0.f;
        *reinterpret_cast<float*>(base + ap::offsets::kAnimCycleEnd) = 0.f;
    }
}
EnhancedGameObject::updateSyncedAnimation(totalTime, frameIndex);
}
}
;

class $modify(APFMOD, FMODAudioEngine){
        // no sound effects while the engine drives the ticks
        int playEffectAdvanced(gd::string path, float speed, float unknown, float volume, float pitch,
                               bool fastFourierTransform, bool reverb, int startMillis, int endMillis, int fadeIn,
                               int fadeOut, bool loopEnabled, int effectID, bool override, bool noPreload,
                               int channelID, int uniqueID, float minInterval,
                               int sfxGroup){if (Engine::get().active()) return 0;
return FMODAudioEngine::playEffectAdvanced(path, speed, unknown, volume, pitch, fastFourierTransform, reverb,
                                           startMillis, endMillis, fadeIn, fadeOut, loopEnabled, effectID, override,
                                           noPreload, channelID, uniqueID, minInterval, sfxGroup);
}

// same as playEffectAdvanced
int playEffect(gd::string path) {
    if (Engine::get().active()) return 0;
    return FMODAudioEngine::playEffect(path);
}
}
;

namespace ap::actions {
    // starts or stops the search
    void solve(bool live) { toggleSolve(live); }

    // starts or stops playing the last macro
    void play() { togglePlay(); }

    // calls the exportMacro above, not this one
    void exportMacro() { ::exportMacro(); }

    // starts or stops recording
    void record() { ap::Recorder::get().toggle(PlayLayer::get()); }

    // returns true while a macro is playing
    bool playing() { return Engine::get().playing(); }

    // plays a macro loaded from a .gdr file
    void playFile(MacroFile const& file) {
        auto* pl = PlayLayer::get();
        if (!pl) {
            Notification::create("Auto Pathing Tool: open the level first, then play the macro",
                                 NotificationIcon::Warning)
                    ->show();
            return;
        }
        if (SimPlanner::get().running()) {
            Notification::create("Auto Pathing Tool: stop the search first", NotificationIcon::Warning)->show();
            return;
        }
        int const levelId = pl->m_level ? static_cast<int>(pl->m_level->m_levelID.value()) : 0;
        if (file.levelId != 0 && levelId != 0 && file.levelId != levelId) {
            Notification::create(fmt::format("Auto Pathing Tool: this macro was made for {}",
                                             file.levelName.empty() ? "another level" : file.levelName),
                                 NotificationIcon::Warning, 3.f)
                    ->show();
        }
        if (Engine::get().playing()) Engine::get().stopPlayback();
        Engine::get().startPlayback(file.macro);
    }
}

class $modify(APKeyboardDispatcher,
              CCKeyboardDispatcher){// handles the mod keys, and the path editor keys while it is open
                                    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat,
                                                             double timestamp){auto& editor = ap::Editor::get();
if (editor.active()) {
    bool const move = key == KEY_Left || key == KEY_Right || key == KEY_A || key == KEY_D;
    if (!repeat) editor.onKey(key, down);
    if (move) return true;
}
return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
}
}
;

// builds the settings menu entries once the game has loaded
$on_game(Loaded) {
    KeyboardInputEvent()
            .listen(
                    [](KeyboardInputData& data) {
                        auto& editor = ap::Editor::get();
                        if (!editor.active()) return ListenerResult::Propagate;
                        bool const move =
                                data.key == KEY_Left || data.key == KEY_Right || data.key == KEY_A || data.key == KEY_D;
                        if (data.action != KeyboardInputData::Action::Repeat)
                            editor.onKey(data.key, data.action == KeyboardInputData::Action::Press);
                        return move ? ListenerResult::Stop : ListenerResult::Propagate;
                    },
                    -1000000)
            .leak();
    auto bind = [](char const* key, void (*action)()) {
        listenForKeybindSettingPresses(key, [action](Keybind const&, bool down, bool repeat, double) {
            if (!down || repeat || ap::ui::menuOpen()) return;
            action();
        });
    };
    bind("key-live", [] { ap::actions::solve(true); });
    bind("key-edit", [] { ap::Editor::get().toggle(); });
    bind("key-offline", [] { ap::actions::solve(false); });
    bind("key-play-macro", [] { ap::actions::play(); });
    bind("key-save-macro", [] { ap::actions::exportMacro(); });
    bind("key-record-game", [] { ap::actions::record(); });
    listenForKeybindSettingPresses("key-menu", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) ap::ui::toggleMenu();
    });
}
