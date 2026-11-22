// Engine that plays the real game step by step: single steps, level snapshots and restores, macro playback,
//rng and start-state alignment with the simulator.

#include "engine.hpp"
#include "offsets.hpp"
#include "../sim/gd.hpp"
#include "log.hpp"
#include "settings.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace ap {

    // the single engine
    Engine& Engine::get() {
        static Engine instance;
        return instance;
    }

#ifdef GEODE_IS_WINDOWS
    // reads the game's rng state
    uint64_t Engine::rng() { return *reinterpret_cast<uint64_t*>(geode::base::get() + offsets::kFastRandState); }

    // sets the game's rng state
    void Engine::seedRng(uint64_t seed) {
        *reinterpret_cast<uint64_t*>(geode::base::get() + offsets::kFastRandState) = seed;
    }

    // reads the teleport rng state
    uint64_t Engine::tpRng() { return *reinterpret_cast<uint64_t*>(geode::base::get() + offsets::kTeleportRandState); }

    // sets the teleport rng state
    void Engine::seedTpRng(uint64_t seed) {
        *reinterpret_cast<uint64_t*>(geode::base::get() + offsets::kTeleportRandState) = seed;
    }
#else
    // reads the game's rng state (other platforms)
    uint64_t Engine::rng() { return GameToolbox::getfast_srand(); }

    // sets the game's rng state (other platforms)
    void Engine::seedRng(uint64_t seed) { GameToolbox::fast_srand(seed); }

    // teleport rng state, 0 on other platforms
    uint64_t Engine::tpRng() { return 0; }

    // does nothing on other platforms
    void Engine::seedTpRng(uint64_t) {}
#endif

#ifdef GEODE_IS_WINDOWS
    // seed picked by the last resetLevel (PlayLayer::m_randomSeed)
    uint64_t Engine::resetSeed(PlayLayer* pl) {
        return pl ? *reinterpret_cast<uint64_t const*>(reinterpret_cast<uintptr_t>(pl) + offsets::kPlayLayerRandomSeed)
                  : 0;
    }
#else
    // 0 on other platforms
    uint64_t Engine::resetSeed(PlayLayer*) { return 0; }
#endif

    // seed of the current attempt, written in the recordings
    uint64_t Engine::attemptSeed(PlayLayer* pl) { return s_rngAligned ? kSeed : resetSeed(pl); }

    // puts the rng where the simulator has it after its reset: kSeed plus the reset's draws
    void Engine::alignRngAfterReset(PlayLayer* pl) {
        s_rngAligned = false;
        if (!pl) return;
        auto lcg = [](uint64_t state) { return state * sim::Game::kRandMul + sim::Game::kRandInc; };
        uint64_t const current = rng();
        uint64_t state = resetSeed(pl);
        int draws = 0;
        while (static_cast<uint32_t>(state) != static_cast<uint32_t>(current) && draws < 1000000) {
            state = lcg(state);
            ++draws;
        }
        if (static_cast<uint32_t>(state) != static_cast<uint32_t>(current)) {
            Log::print("RNG: reset seed not found; set to the simulator's seed.");
            seedRng(kSeed);
            s_rngAligned = true;
            return;
        }
        int const wantedDraws = s_simResetDraws >= 0 ? s_simResetDraws : draws;
        uint64_t aligned = kSeed;
        for (int i = 0; i < wantedDraws; ++i) aligned = lcg(aligned);
        seedRng(aligned);
        s_rngAligned = true;
        if (draws != wantedDraws) {
            Log::printf(
                    "RNG: {} draw(s) during the reset, {} in the simulator; the RNG is set to the simulator's "
                    "sequence.",
                    draws, wantedDraws);
        } else if (draws > 0) {
            Log::printf(
                    "RNG: {} draw(s) during the reset (random triggers at the start); the sequence matches the "
                    "simulator.",
                    draws);
        }
    }

    // closes the pause menu if it's open
    void Engine::dismissPause(PlayLayer* pl) {
        if (auto* scene = CCDirector::sharedDirector()->getRunningScene()) {
            for (auto* child : CCArrayExt<CCNode*>(scene->getChildren())) {
                if (auto* pause = typeinfo_cast<PauseLayer*>(child)) {
                    pause->onResume(nullptr);
                    break;
                }
            }
        }
        pl->m_isPaused = false;
    }

    //saves then turns off click between steps, click on steps and practice mode
    void Engine::saveFlags(PlayLayer* pl) {
        if (m_flagsSaved) return;
        m_savedAttempts = pl->m_attempts;
        m_savedCbs = pl->m_clickBetweenSteps;
        m_savedCos = pl->m_clickOnSteps;
        m_savedPractice = pl->m_isPracticeMode;
        m_flagsSaved = true;
        pl->m_clickBetweenSteps = false;
        pl->m_clickOnSteps = false;
        pl->m_isPracticeMode = false;
    }

    // puts back what saveFlags changed
    void Engine::restoreFlags(PlayLayer* pl) {
        if (!m_flagsSaved) return;
        m_flagsSaved = false;
        if (!pl || PlayLayer::get() != pl) return;
        pl->m_clickBetweenSteps = m_savedCbs;
        pl->m_clickOnSteps = m_savedCos;
        pl->m_isPracticeMode = m_savedPractice;
        pl->m_attempts = m_savedAttempts;
    }

    // plugs the engine into a freshly loaded level
    void Engine::attach(PlayLayer* pl) {
        m_layer = pl;
        m_active = false;
        m_editing = false;
        m_playing = false;
        m_pendingReset = false;
        m_redoReset = false;
        m_redoCount = 0;
        m_script = nullptr;
        m_restoring = false;
        m_inStep = false;
        m_flagsSaved = false;
        m_stepCounter = 0;
        m_curStep = 0;
        m_forcePractice = false;
        clearFlags();
    }

    // level left: forget it
    void Engine::onLayerGone() {
        m_active = false;
        m_editing = false;
        m_playing = false;
        m_script = nullptr;
        m_layer = nullptr;
        m_restoring = false;
        m_inStep = false;
        m_flagsSaved = false;
    }

    // plays exactly one tick, returns false if the player died or finished
    bool Engine::step() {
        auto* pl = m_layer;
        if (!pl) return false;
        clearFlags();

        pl->m_extraDelta = 0.0;
        pl->m_resumeTimer = 0;
        float timeWarp = pl->m_gameState.m_timeWarp;
        if (!(timeWarp > 0.f) || timeWarp > 1.f) timeWarp = 1.f;
        float const dt = timeWarp / 240.f;

        int const before = m_stepCounter;
        m_inStep = true;
        for (int guard = 0; guard < 8 && m_stepCounter == before && !m_died && !m_completed; ++guard) {
            pl->update(dt);
        }
        m_inStep = false;
        if (m_stepCounter == before && !m_died && !m_completed) {
            m_stalled = true;
            return false;
        }
        return !m_died && !m_completed;
    }

    // one update with no time so the game finishes setting up after a reset
    void Engine::refresh() {
        auto* pl = m_layer;
        if (!pl) return;
        pl->m_extraDelta = 0.0;
        m_inStep = true;
        pl->update(0.f);
        m_inStep = false;
    }

    // sets the script to play, it must stay alive elsewhere
    void Engine::setScript(std::vector<Toggle> const* script) {
        m_script = script;
        resyncScript();
    }

    // moves the script position to the current tick
    void Engine::resyncScript() {
        m_scriptIdx = 0;
        if (!m_script) return;
        while (m_scriptIdx < m_script->size() && (*m_script)[m_scriptIdx].step < m_curStep) ++m_scriptIdx;
    }

    // snapshot of the current game state
    std::shared_ptr<Snapshot> Engine::takeSnapshot() {
        auto snapshot = std::make_shared<Snapshot>();
        auto* pl = m_layer;
        if (!pl) return snapshot;
        snapshot->cp = pl->createCheckpoint();
        snapshot->p1 = PlayerState::save(pl->m_player1);
        snapshot->p2 = PlayerState::save(pl->m_player2);
        snapshot->step = m_stepCounter;
        snapshot->rng = rng();
        snapshot->tpRng = tpRng();
        snapshot->variance = pl->m_varianceValues;
        snapshot->p1pos = pl->m_player1 ? pl->m_player1->getPosition() : CCPoint{};
        snapshot->p2pos = pl->m_player2 ? pl->m_player2->getPosition() : CCPoint{};
        return snapshot;
    }

    // full level reset with fixed rngs, the deterministic start
    void Engine::fullReset() {
        auto* pl = m_layer;
        if (!pl) return;
        m_restoring = true;
        m_restoreSnap = nullptr;
        if (pl->m_checkpointArray && pl->m_checkpointArray->count() > 0) pl->removeAllCheckpoints();
        seedRng(kSeed);
        seedTpRng(kTpSeed);
        pl->resetLevel();
        m_restoring = false;
        pl->m_queuedButtons.clear();
        if (pl->m_player1) pl->m_player1->releaseAllButtons();
        if (pl->m_player2) pl->m_player2->releaseAllButtons();
        m_stepCounter = 0;
        m_curStep = 0;
        clearFlags();
        if (m_active) refresh();
        resyncScript();
    }

    // loads a snapshot through the game's checkpoint, returns false if the restore isn't exact
    bool Engine::restore(Snapshot const& snapshot) {
        auto* pl = m_layer;
        if (!pl) return false;
        if (snapshot.isStart()) {
            fullReset();
            return true;
        }

        for (int attempt = 0; attempt < 2; ++attempt) {
            m_restoring = true;
            m_restoreSnap = &snapshot;
            m_loadHookRan = false;
            bool const prevPractice = pl->m_isPracticeMode;
            if (m_forcePractice) pl->m_isPracticeMode = true;

            pl->m_checkpointArray->addObject(snapshot.cp.data());
            seedRng(snapshot.rng);
            seedTpRng(snapshot.tpRng);
            pl->resetLevel();
            if (pl->m_checkpointArray->count() > 0) pl->m_checkpointArray->removeLastObject();
            pl->m_varianceValues = snapshot.variance;
            seedRng(snapshot.rng);
            seedTpRng(snapshot.tpRng);

            pl->m_isPracticeMode = prevPractice;
            m_restoring = false;
            m_restoreSnap = nullptr;

            pl->m_queuedButtons.clear();
            m_stepCounter = snapshot.step;
            m_curStep = snapshot.step;
            clearFlags();
            refresh();
            resyncScript();

            bool exact = m_loadHookRan;
            if (exact && pl->m_player1) {
                auto pos = pl->m_player1->getPosition();
                if (std::fabs(pos.x - snapshot.p1pos.x) > 0.01f || std::fabs(pos.y - snapshot.p1pos.y) > 0.01f)
                    exact = false;
            }
            if (exact) return true;

            if (attempt == 0 && !m_forcePractice) {
                m_forcePractice = true;
                Log::print("Auto Pathing Tool: inexact restore, retrying with practice mode forced.");
                continue;
            }
            Log::printf("Auto Pathing Tool: WARNING inexact restore at tick {} (hook {}).", snapshot.step,
                        m_loadHookRan ? "ok" : "not called");
            return false;
        }
        return false;
    }

    //after the checkpoint load, put back what the checkpoint doesn't restore exactly
    void Engine::applyRestoreFixes() {
        auto* pl = m_layer;
        if (!pl || !m_restoreSnap) return;
        auto const& snapshot = *m_restoreSnap;
        if (pl->m_player1) snapshot.p1.apply(pl->m_player1);
        if (pl->m_player2) snapshot.p2.apply(pl->m_player2);
        pl->m_varianceValues = snapshot.variance;
        seedRng(snapshot.rng);
        seedTpRng(snapshot.tpRng);
        pl->m_queuedButtons.clear();
        m_loadHookRan = true;
    }

    // resets the level and starts injecting the macro's inputs
    void Engine::startPlayback(Macro const& macro) {
        auto* pl = PlayLayer::get();
        if (!pl || m_active) return;
        m_layer = pl;
        m_playMacro = macro;
        m_playing = true;
        saveFlags(pl);
        dismissPause(pl);
        seedRng(kSeed);
        pl->resetLevel();
        setScript(&m_playMacro.toggles);
    }

    // swaps the played macro, the script position follows the current tick
    void Engine::replacePlayback(Macro const& macro) {
        if (!m_playing) return;
        m_playMacro = macro;
        setScript(&m_playMacro.toggles);
    }

    // path editor: the level stops, the editor plays ticks with step() and goes back with restore()
    void Engine::beginEdit(PlayLayer* pl) {
        if (!pl || m_editing) return;
        m_layer = pl;
        m_editing = true;
        m_active = true;
        clearFlags();
    }

    // path editor done: the normal game (or the bot) takes the level back where it stands
    void Engine::endEdit() {
        if (!m_editing) return;
        m_editing = false;
        m_active = false;
        clearFlags();
        resyncScript();
    }

    void Engine::stopPlayback() {
        if (!m_playing) return;
        m_playing = false;
        if (!m_active) {
            m_script = nullptr;
            restoreFlags(m_layer);
        }
    }

    // while driving ticks the death is only noted and cancelled
    bool Engine::onDestroyPlayer(PlayLayer* pl, PlayerObject* player, GameObject* object) {
        if (pl != m_layer) return false;
        if (object && object == pl->m_anticheatSpike) return false;
        if (m_active) {
            if (!m_died) {
                m_died = true;
                m_deadPlayer = (player == pl->m_player2) ? 2 : 1;
                m_killerId = object ? object->m_uniqueID : -1;
                m_killerObjectId = object ? object->m_objectID : 0;
                m_deathPos = player ? player->getPosition() : CCPoint{};
            }
            return true;
        }
        return false;
    }

    // cancels the completion when driving ticks, restarts the level after a playback
    bool Engine::onLevelComplete(PlayLayer* pl) {
        if (pl != m_layer) return false;
        if (m_active) {
            m_completed = true;
            return true;
        }
        if (m_playing || m_pendingReset) {
            if (!m_pendingReset) {
                m_pendingReset = true;
                stopPlayback();
                Loader::get()->queueInMainThread([this, pl] {
                    m_pendingReset = false;
                    if (PlayLayer::get() == pl && m_layer == pl) pl->resetLevel();
                });
            }
            return true;
        }
        return false;
    }

    // counts the ticks (half ticks ignored)
    void Engine::onProcessCommands(GJBaseGameLayer* layer, bool isHalfTick) {
        if (isHalfTick) return;
        if (!m_layer || layer != static_cast<GJBaseGameLayer*>(m_layer)) return;
        m_curStep = m_stepCounter;
        ++m_stepCounter;
        if (m_redoReset && !m_pendingReset) {
            m_redoReset = false;
            m_pendingReset = true;
            auto* pl = m_layer;
            Loader::get()->queueInMainThread([this, pl] {
                m_pendingReset = false;
                if (PlayLayer::get() == pl && m_layer == pl) pl->resetLevel();
            });
        }
    }

    // the game doesn't replay the start triggers after a reset with no step played, so reset again (at most twice)
    void Engine::checkResetSteps(PlayLayer* pl, int stepsBefore) {
        if (!m_layer || pl != m_layer || m_restoring) return;
        if (stepsBefore > 0) {
            m_redoCount = 0;
            m_redoReset = false;
            return;
        }
        if (m_redoCount >= 2) return;
        ++m_redoCount;
        m_redoReset = true;
        Log::print(
                "reset: no step played since the previous reset, so the game would not replay the start triggers; "
                "resetting again after one step.");
    }

    // replaces the queued inputs with the script's toggles for this tick
    void Engine::onProcessQueuedButtons(GJBaseGameLayer* layer) {
        if (!m_layer || layer != static_cast<GJBaseGameLayer*>(m_layer)) return;
        if (!m_script) return;
        layer->m_queuedButtons.clear();
        auto const& script = *m_script;
        while (m_scriptIdx < script.size() && script[m_scriptIdx].step < m_curStep) ++m_scriptIdx;
        while (m_scriptIdx < script.size() && script[m_scriptIdx].step == m_curStep) {
            bool flip = false;
            if (auto* pl = PlayLayer::get(); pl && pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode)
                flip = GameManager::sharedState()->getGameVariable("0010");
            layer->queueButton(1, script[m_scriptIdx].down, script[m_scriptIdx].p2 != flip, 0.0);
            ++m_scriptIdx;
        }
    }

    // finishes a restore once the game loaded the checkpoint
    void Engine::onLoadFromCheckpoint(PlayLayer* pl) {
        if (m_restoring && pl == m_layer) applyRestoreFixes();
    }

    // seeds the rng before resetLevel draws from it
    void Engine::onPreResetLevel(PlayLayer* pl) {
        if (!m_layer || pl != m_layer || m_restoring) return;
        if (m_playing) seedRng(kSeed);
    }

    // saves the start state right after the loading reset
    void Engine::captureFresh(PlayLayer* pl) {
        m_haveFresh = false;
        if (!pl || !pl->m_player1) return;
        m_fresh1 = PlayerState::save(pl->m_player1);
        m_fresh2 = PlayerState::save(pl->m_player2);
        m_freshMaxPortalY = pl->m_gameState.m_unkUint17;
        m_freshGridSnapOff = pl->m_gameState.m_disableCameraGridSnap;
        m_freshNoOuterOb.clear();
        if (pl->m_objects) {
            for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects))
                if (obj && !obj->m_shouldUseOuterOb) m_freshNoOuterOb.push_back(obj);
        }
        m_haveFresh = m_fresh1.valid;
    }

    // puts back the start state saved by captureFresh (fields resetLevel doesn't reset)
    void Engine::applyFresh(PlayLayer* pl) {
        if (!m_haveFresh || !pl || pl != m_layer) return;
        auto put = [](PlayerObject* player, PlayerState const& saved) {
            if (!player || !saved.valid) return;
            PlayerState state = saved;
            state.position = player->getPosition();
            state.m_position = player->m_position;
            state.m_controlsDisabled = player->m_controlsDisabled;
            state.apply(player);
        };
        put(pl->m_player1, m_fresh1);
        put(pl->m_player2, m_fresh2);
        pl->m_gameState.m_totalTime = 0.0;
        pl->m_gameState.m_unkDouble3 = 0.0;
        pl->m_gameState.m_unkUint17 = m_freshMaxPortalY;
        pl->m_gameState.m_disableCameraGridSnap = m_freshGridSnapOff;
        if (pl->m_player1) pl->m_player1->m_totalTime = 0.0;
        if (pl->m_player2) pl->m_player2->m_totalTime = 0.0;
        for (auto* obj : m_freshNoOuterOb)
            if (obj) obj->m_shouldUseOuterOb = false;
    }

    // real reset: new attempt, tick counter back to 0
    void Engine::onResetLevel(PlayLayer* pl) {
        if (!m_layer || pl != m_layer) return;
        if (m_restoring) return;
        m_stepCounter = 0;
        m_curStep = 0;
        pl->m_queuedButtons.clear();
        resyncScript();
    }

}
