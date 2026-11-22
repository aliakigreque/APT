// F7 macro recorder: records the player's own inputs as a macro.

#include "input_recorder.hpp"

#include "editor.hpp"
#include "engine.hpp"
#include "log.hpp"
#include "settings.hpp"
#include "simplanner.hpp"

using namespace geode::prelude;

namespace ap {

    // the single macro recorder
    InputRecorder& InputRecorder::get() {
        static InputRecorder recorder;
        return recorder;
    }

    // turns the F7 recorder on or off, recording starts with the next attempt
    void InputRecorder::setEnabled(bool on) {
        m_enabled = on;
        m_inputs.clear();
        m_held = 0;
        m_fromStart = false;
        if (on) {
            Notification::create("Auto Pathing Tool: recording your inputs from the next attempt",
                                 NotificationIcon::Info)
                    ->show();
        }
    }

    // attaches to a new level, forgets the inputs
    void InputRecorder::onLevelLoaded(PlayLayer* pl) {
        m_layer = pl;
        m_inputs.clear();
        m_held = 0;
        m_fromStart = true;
    }

    // forgets the level, it is closing
    void InputRecorder::onLayerGone() {
        m_layer = nullptr;
        m_inputs.clear();
        m_fromStart = false;
    }

    // a new attempt: the inputs of the previous one are dropped
    void InputRecorder::onNewAttempt(PlayLayer* pl) {
        m_layer = pl;
        m_inputs.clear();
        m_held = 0;
        m_fromStart = true;
    }

    // your attempt, in normal mode, with nothing else driving the level
    bool InputRecorder::recording(PlayLayer* pl) const {
        if (!m_enabled || !m_fromStart || !pl || pl != m_layer || pl->m_isPracticeMode || pl->m_isPlatformer)
            return false;
        if (SimPlanner::get().phase() == SimPlanner::Phase::Live || Engine::get().playing()) return false;
        return !Engine::get().active() && !Editor::get().active();
    }

    // the queue holds the presses and releases of this tick, the held state after them is the tick's input
    void InputRecorder::onQueuedButtons(GJBaseGameLayer* layer, int step) {
        auto* pl = PlayLayer::get();
        if (!pl || layer != static_cast<GJBaseGameLayer*>(pl) || !recording(pl) || step < 0) return;
        bool const flip = pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode &&
                          GameManager::sharedState()->getGameVariable("0010");
        for (auto const& command : layer->m_queuedButtons) {
            if (command.m_button != PlayerButton::Jump) continue;
            uint8_t const bit = (command.m_isPlayer2 != flip) ? 2 : 1;
            m_held = command.m_isPush ? (m_held | bit) : (m_held & ~bit);
        }
        if (m_inputs.size() < static_cast<size_t>(step)) m_inputs.resize(static_cast<size_t>(step), m_held);
        if (m_inputs.size() == static_cast<size_t>(step))
            m_inputs.push_back(m_held);
        else
            m_inputs[static_cast<size_t>(step)] = m_held;
    }

    // with "Save on death", the attempt is saved in macros/ and becomes the current macro (to edit it)
    void InputRecorder::onDeath(PlayLayer* pl, int step) {
        if (!recording(pl) || m_inputs.empty()) return;
        m_fromStart = false;
        if (!Settings::get().saveOnDeath) return;
        m_inputs.resize(static_cast<size_t>(std::max(step, 1)), m_held);
        SimPlanner::get().setMacroFromInputs(m_inputs);
        float const length = std::max(1.f, pl->m_levelLength);
        int const percent = pl->m_player1 ? static_cast<int>(pl->m_player1->getPositionX() / length * 100.f) : 0;
        std::string const levelName = pl->m_level ? std::string(pl->m_level->m_levelName) : "level";
        int const levelId = pl->m_level ? static_cast<int>(pl->m_level->m_levelID.value()) : 0;
        auto result = SimPlanner::get().macro().exportGDR(Mod::get()->getSaveDir() / "macros", levelName, levelId,
                                                          Mod::get()->getVersion().toVString(),
                                                          fmt::format("_death_{}pct", std::clamp(percent, 0, 100)));
        if (result) {
            Log::printf("recorded attempt saved on death: {}", geode::utils::string::pathToString(result.unwrap()));
            Notification::create(fmt::format("Auto Pathing Tool: attempt saved ({}%, macros/)", percent),
                                 NotificationIcon::Success)
                    ->show();
        }
    }

    // a finished attempt becomes the current macro
    void InputRecorder::onComplete(PlayLayer* pl, int step) {
        if (!recording(pl) || m_inputs.empty()) return;
        m_fromStart = false;
        m_inputs.resize(static_cast<size_t>(std::max(step, 1)), m_held);
        SimPlanner::get().setMacroFromInputs(m_inputs);
        Notification::create(fmt::format("Auto Pathing Tool: your run is the current macro ({} to save it)",
                                         keyName(Action::Export)),
                             NotificationIcon::Success)
                ->show();
    }

}
