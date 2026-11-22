// F3 path editor: freezes the level, lets the player step through ticks, change the inputs,
// and hands the edited prefix back to the live search.

#include "editor.hpp"

#include "settings.hpp"
#include "simplanner.hpp"

#include <algorithm>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

namespace ap {

    namespace {
        constexpr int kPlayStateGap = 480;
        constexpr int kEditStateGap = 30;
        constexpr int kMaxTicksPerFrame = 240;

        // per tick inputs of a macro, as long as its last toggle
        std::vector<uint8_t> inputsOf(Macro const& macro) {
            std::vector<uint8_t> inputs;
            uint8_t buttons = 0;
            for (auto const& toggle : macro.toggles) {
                if (toggle.step < 0) continue;
                if (inputs.size() < static_cast<size_t>(toggle.step))
                    inputs.resize(static_cast<size_t>(toggle.step), buttons);
                uint8_t const bit = toggle.p2 ? 2 : 1;
                buttons = toggle.down ? (buttons | bit) : (buttons & ~bit);
                if (inputs.size() == static_cast<size_t>(toggle.step))
                    inputs.push_back(buttons);
                else
                    inputs[static_cast<size_t>(toggle.step)] = buttons;
            }
            return inputs;
        }

        // macro toggles of per tick inputs
        Macro macroOf(std::vector<uint8_t> const& inputs) {
            Macro macro;
            uint8_t previous = 0;
            for (size_t i = 0; i < inputs.size(); ++i) {
                uint8_t const buttons = inputs[i] & 3;
                if ((buttons ^ previous) & 1)
                    macro.toggles.push_back(Toggle{static_cast<int>(i), (buttons & 1) != 0, false});
                if ((buttons ^ previous) & 2)
                    macro.toggles.push_back(Toggle{static_cast<int>(i), (buttons & 2) != 0, true});
                previous = buttons;
            }
            return macro;
        }

        // in 2 player mode, game option "0010" swaps the players
        bool playersSwapped(PlayLayer* pl) {
            return pl && pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode &&
                   GameManager::sharedState()->getGameVariable("0010");
        }

        // shows a Geode notification with the mod name
        void notify(std::string const& text, NotificationIcon icon = NotificationIcon::Info) {
            Notification::create("Auto Pathing Tool: " + text, icon)->show();
        }
    }

    bool Editor::s_queueWorks = false;

    // the single path editor
    Editor& Editor::get() {
        static Editor editor;
        return editor;
    }

    // F3: starts editing, or gives the edited path back to the bot
    void Editor::toggle() {
        auto* pl = PlayLayer::get();
        if (!pl) return;
        if (m_active)
            resume();
        else
            start(pl);
    }

    // level lifetime
    void Editor::onLevelLoaded(PlayLayer* pl) {
        m_layer = pl;
        m_active = false;
        m_musicPending = false;
        m_states.clear();
        m_lastSaved = -1;
        m_hud = nullptr;
    }

    // forgets the level, it is closing
    void Editor::onLayerGone() {
        m_layer = nullptr;
        m_active = false;
        m_musicPending = false;
        m_states.clear();
        m_lastSaved = -1;
        m_hud = nullptr;
        m_title = nullptr;
        m_info = nullptr;
        m_timeline = nullptr;
    }

    // a real reset: the saved states belong to the previous attempt, and editing stops without giving anything back
    void Editor::onNewAttempt() {
        m_musicPending = false;
        m_states.clear();
        m_lastSaved = -1;
        if (!m_active) return;
        m_active = false;
        Engine::get().endEdit();
        if (m_hud) m_hud->removeFromParent();
        m_hud = nullptr;
        if (m_layer) m_layer->resumeAudio();
    }

    // while the bot plays: one saved state every 2 seconds, the first one at the start of the attempt
    void Editor::beforeFrame(PlayLayer* pl) {
        if (m_active || pl != m_layer) return;
        if (m_musicPending) {
            m_musicPending = false;
            resumeMusic(pl);
        }
        auto& engine = Engine::get();
        if (SimPlanner::get().phase() != SimPlanner::Phase::Live && !engine.playing()) return;
        if (!pl->m_player1 || pl->m_player1->m_isDead) return;
        int const step = engine.stepCounter();
        if (m_lastSaved >= 0 && step - m_lastSaved < kPlayStateGap) return;
        if (m_states.count(step)) return;
        m_states[step] = engine.takeSnapshot();
        m_lastSaved = step;
    }

    // the music goes on from the time of the tick the level stands at
    void Editor::resumeMusic(PlayLayer* pl) {
        pl->resumeAudio();
        if (!pl->m_levelSettings) return;
        double const seconds = pl->m_gameState.m_levelTime + static_cast<double>(pl->m_levelSettings->m_songOffset);
        if (seconds >= 0.0)
            FMODAudioEngine::sharedEngine()->setMusicTimeMS(static_cast<unsigned int>(seconds * 1000.0), true, 0);
    }

    // input of a tick, past the end the buttons stay as they were
    uint8_t Editor::inputAt(int tick) const {
        if (tick >= 0 && static_cast<size_t>(tick) < m_inputs.size()) return m_inputs[static_cast<size_t>(tick)];
        return m_inputs.empty() ? 0 : m_inputs.back();
    }

    // stops the level where the bot is and takes its inputs
    void Editor::start(PlayLayer* pl) {
        auto& planner = SimPlanner::get();
        auto& engine = Engine::get();
        if (pl->m_isPlatformer) return;
        if (planner.phase() == SimPlanner::Phase::Live) {
            m_source = Source::Live;
            m_inputs = planner.liveInputs();
        } else if (engine.playing()) {
            m_source = Source::Playback;
            m_inputs = inputsOf(engine.playbackMacro());
        } else {
            notify(fmt::format("start the bot ({}) or play a macro first, then press {} to edit its path",
                               keyName(Action::Live), keyName(Action::Edit)),
                   NotificationIcon::Warning);
            return;
        }
        if (!pl->m_player1 || pl->m_player1->m_isDead) {
            notify("wait for the next attempt, the player is dead", NotificationIcon::Warning);
            return;
        }
        m_layer = pl;
        m_cursor = engine.stepCounter();
        if (m_inputs.size() < static_cast<size_t>(m_cursor))
            m_inputs.resize(static_cast<size_t>(m_cursor), inputAt(m_cursor));
        m_lastSent = m_cursor > 0 ? (inputAt(m_cursor - 1) & 3) : 0;
        m_dead = false;
        m_firstEdit = -1;
        m_held = 0;
        m_backHeld = false;
        m_forwardHeld = false;
        m_polledHeld = 0;
        m_pending = 0;
        m_holdTime = 0.f;
        m_holdTicks = 0.f;

        pl->pauseAudio();
        engine.beginEdit(pl);
        m_active = true;
        saveState();
        buildHud(pl);
        updateHud();
    }

    // gives the inputs up to the cursor back to the bot, which goes on from here
    void Editor::resume() {
        if (m_dead) {
            notify("the player is dead here: go back before the death first", NotificationIcon::Warning);
            return;
        }
        std::vector<uint8_t> prefix(m_inputs.begin(),
                                    m_inputs.begin() + std::min<long>(m_cursor, static_cast<long>(m_inputs.size())));
        prefix.resize(static_cast<size_t>(m_cursor), inputAt(m_cursor - 1));

        auto& engine = Engine::get();
        engine.endEdit();
        m_active = false;
        if (m_hud) m_hud->removeFromParent();
        m_hud = nullptr;
        m_title = nullptr;
        m_info = nullptr;
        m_timeline = nullptr;
        m_states.erase(m_states.upper_bound(m_cursor), m_states.end());
        m_lastSaved = m_states.empty() ? -1 : m_states.rbegin()->first;

        m_musicPending = true;

        auto& planner = SimPlanner::get();
        if (m_source == Source::Live && planner.phase() == SimPlanner::Phase::Live) {
            planner.resumeFromEdit(prefix, m_firstEdit);
            planner.setMacroFromInputs(prefix);
            notify(fmt::format("the bot goes on from tick {}", m_cursor), NotificationIcon::Success);
        } else if (m_source == Source::Playback && engine.playing()) {
            Macro const macro = macroOf(m_inputs);
            engine.replacePlayback(macro);
            planner.setMacroFromInputs(m_inputs);
            notify(fmt::format("the macro goes on from tick {}", m_cursor), NotificationIcon::Success);
        }
    }

    // saves the level state at the cursor if it isn't saved yet
    void Editor::saveState() {
        if (m_states.count(m_cursor)) return;
        m_states[m_cursor] = Engine::get().takeSnapshot();
    }

    // plays the tick at the cursor: the jump button held now is its input
    bool Editor::stepForward() {
        if (m_dead) return false;
        auto& engine = Engine::get();
        if (engine.stepCounter() != m_cursor) m_cursor = engine.stepCounter();
        uint8_t const before = inputAt(m_cursor);
        bool const twoPlayer = m_layer && m_layer->m_levelSettings && m_layer->m_levelSettings->m_twoPlayerMode;
        uint8_t const now =
                twoPlayer ? static_cast<uint8_t>(held() & 3) : static_cast<uint8_t>((before & 2) | (held() & 1));
        if (m_inputs.size() <= static_cast<size_t>(m_cursor))
            m_inputs.resize(static_cast<size_t>(m_cursor) + 1, inputAt(m_cursor));
        m_inputs[static_cast<size_t>(m_cursor)] = now;
        if (now != before) {
            if (m_firstEdit < 0 || m_cursor < m_firstEdit) m_firstEdit = m_cursor;
            m_states.erase(m_states.upper_bound(m_cursor), m_states.end());
        }
        engine.step();
        if (engine.stalled()) return false;
        ++m_cursor;
        if (engine.died() || engine.completed()) {
            m_dead = engine.died();
            return false;
        }
        if (m_cursor % kEditStateGap == 0) saveState();
        return true;
    }

    // restores the closest saved state before the tick, then plays the inputs up to it
    void Editor::goTo(int tick) {
        tick = std::clamp(tick, 0, m_cursor);
        auto it = m_states.upper_bound(tick);
        if (it == m_states.begin()) {
            if (m_states.empty()) return;
            tick = it->first;
        } else {
            --it;
        }
        auto& engine = Engine::get();
        engine.restore(*it->second);
        m_cursor = it->first;
        m_lastSent = m_cursor > 0 ? (inputAt(m_cursor - 1) & 3) : 0;
        m_dead = false;
        while (m_cursor < tick) {
            engine.step();
            if (engine.stalled()) break;
            ++m_cursor;
            if (engine.died()) {
                m_dead = true;
                break;
            }
            if (m_cursor % kEditStateGap == 0) saveState();
        }
    }

    // arrows held: one tick at once, then faster and faster
    void Editor::frame(PlayLayer* pl, float dt) {
        if (!m_active || pl != m_layer) return;
        int const cursorBefore = m_cursor;
        readQueuedJump(pl);
        pollKeys();
        if (m_pending > 0)
            for (int i = 0; i < m_pending; ++i)
                if (!stepForward()) break;
        if (m_pending < 0) goTo(m_cursor + m_pending);
        m_pending = 0;
        int const direction = (m_forwardHeld ? 1 : 0) - (m_backHeld ? 1 : 0);
        if (direction == 0) {
            m_holdTime = 0.f;
            m_holdTicks = 0.f;
        } else {
            m_holdTime += dt;
            if (m_holdTime > 0.3f) {
                float const rate = m_holdTime < 1.3f ? 30.f : m_holdTime < 3.3f ? 120.f : 480.f;
                m_holdTicks += rate * dt;
            }
            int const ticks = std::min(static_cast<int>(m_holdTicks), kMaxTicksPerFrame);
            m_holdTicks -= static_cast<float>(ticks);
            if (ticks > 0) {
                if (direction > 0) {
                    for (int i = 0; i < ticks; ++i)
                        if (!stepForward()) break;
                } else {
                    goTo(m_cursor - ticks);
                }
            }
        }
        if (m_cursor != cursorBefore) pl->pauseAudio();
        updateHud();
    }

    // the edited input of the tick, as button changes in the game's queue
    void Editor::injectInput(GJBaseGameLayer* layer, int step) {
        if (!m_active || !m_layer || layer != static_cast<GJBaseGameLayer*>(m_layer)) return;
        layer->m_queuedButtons.clear();
        uint8_t const desired = inputAt(step) & 3;
        bool const flip = playersSwapped(m_layer);
        if ((desired ^ m_lastSent) & 1) layer->queueButton(1, (desired & 1) != 0, flip, 0.0);
        if ((desired ^ m_lastSent) & 2) layer->queueButton(1, (desired & 2) != 0, !flip, 0.0);
        m_lastSent = desired;
    }

    // jump presses the game queued (whatever the jump is bound to), read and emptied every frame
    void Editor::readQueuedJump(PlayLayer* pl) {
        auto& queue = pl->m_queuedButtons;
        bool const flip = playersSwapped(pl);
        for (auto const& command : queue) {
            if (command.m_button != PlayerButton::Jump) continue;
            s_queueWorks = true;
            uint8_t const bit = (command.m_isPlayer2 != flip) ? 2 : 1;
            m_held = command.m_isPush ? (m_held | bit) : (m_held & ~bit);
        }
        queue.clear();
    }

    // reads the keyboard and the mouse while the game window has the focus
    void Editor::pollKeys() {
#ifdef GEODE_IS_WINDOWS
        DWORD processId = 0;
        HWND const window = GetForegroundWindow();
        if (window) GetWindowThreadProcessId(window, &processId);
        if (processId != GetCurrentProcessId()) {
            m_polledHeld = 0;
            m_backHeld = m_forwardHeld = false;
            m_polling = true;
            return;
        }
        // GetAsyncKeyState sets its high bit while the key is held
        constexpr SHORT kKeyHeldBit = static_cast<SHORT>(0x8000);
        auto down = [](int key) { return (GetAsyncKeyState(key) & kKeyHeldBit) != 0; };
        bool const back = down(VK_LEFT) || down('A');
        bool const forward = down(VK_RIGHT) || down('D');
        if (back && !m_backHeld) {
            m_holdTime = 0.f;
            m_holdTicks = 0.f;
            m_pending -= 1;
        }
        if (forward && !m_forwardHeld) {
            m_holdTime = 0.f;
            m_holdTicks = 0.f;
            m_pending += 1;
        }
        m_backHeld = back;
        m_forwardHeld = forward;
        bool const twoPlayer = m_layer && m_layer->m_levelSettings && m_layer->m_levelSettings->m_twoPlayerMode;
        uint8_t polled = 0;
        if (!s_queueWorks) {
            if (down(VK_SPACE) || down('W') || down(VK_LBUTTON)) polled |= 1;
            if (down(VK_UP)) polled |= twoPlayer ? 2 : 1;
        }
        m_polledHeld = polled;
        m_polling = true;
#endif
    }

    // left / A goes back, right / D goes forward; the first tick moves at once
    bool Editor::onKey(cocos2d::enumKeyCodes key, bool down) {
        if (!m_active) return false;
        bool const back = key == KEY_Left || key == KEY_A;
        bool const forward = key == KEY_Right || key == KEY_D;
        if (!back && !forward) return false;
        if (m_polling) return true;
        bool& held = back ? m_backHeld : m_forwardHeld;
        if (down && !held) {
            m_holdTime = 0.f;
            m_holdTicks = 0.f;
            m_pending += back ? -1 : 1;
        }
        held = down;
        return true;
    }

    // banner, key reminder and timeline
    void Editor::buildHud(PlayLayer* pl) {
        if (m_hud) m_hud->removeFromParent();
        auto const winSize = CCDirector::sharedDirector()->getWinSize();
        m_hud = CCNode::create();
        m_hud->setID("path-editor"_spr);
        m_hud->setZOrder(1100);
        pl->addChild(m_hud);

        m_title = CCLabelBMFont::create("", "goldFont.fnt");
        m_title->setScale(.6f);
        m_title->setPosition({winSize.width / 2.f, winSize.height - 18.f});
        m_hud->addChild(m_title);

        m_info = CCLabelBMFont::create("", "chatFont.fnt");
        m_info->setScale(.5f);
        m_info->setPosition({winSize.width / 2.f, winSize.height - 36.f});
        m_hud->addChild(m_info);

        auto* keys = CCLabelBMFont::create(fmt::format("Left / A: back    Right / D: forward (hold to go faster)    "
                                                       "Jump held: press on those ticks    {}: let the bot go on",
                                                       keyName(Action::Edit))
                                                   .c_str(),
                                           "chatFont.fnt");
        keys->setScale(.42f);
        keys->setPosition({winSize.width / 2.f, 12.f});
        keys->setOpacity(220);
        m_hud->addChild(keys);

        m_timeline = CCDrawNode::create();
        m_hud->addChild(m_timeline);
    }

    // tick, time, jump state, and the presses around the cursor (2 s each side)
    void Editor::updateHud() {
        if (!m_hud || !m_layer) return;
        double const seconds = m_cursor / 240.0;
        float const length = std::max(1.f, m_layer->m_levelLength);
        float const x = m_layer->m_player1 ? m_layer->m_player1->getPositionX() : 0.f;
        int const percent = std::clamp(static_cast<int>(x / length * 100.f), 0, 100);
        m_title->setString(fmt::format("EDIT - tick {} ({:.2f} s, {}%)", m_cursor, seconds, percent).c_str());
        bool const twoPlayer = m_layer->m_levelSettings && m_layer->m_levelSettings->m_twoPlayerMode;
        uint8_t const jump = held();
        std::string info = twoPlayer
                                   ? fmt::format("player 1: {}    player 2: {}", (jump & 1) ? "jump held" : "released",
                                                 (jump & 2) ? "jump held" : "released")
                                   : ((jump & 1) ? "jump held" : "jump released");
        if (m_backHeld) info = "<< " + info;
        if (m_forwardHeld) info += " >>";
        if (m_dead)
            info = "DEAD on this tick - go back before it";
        else if (m_firstEdit >= 0)
            info += fmt::format("    changed from tick {}", m_firstEdit);
        m_info->setString(info.c_str());
        m_info->setColor(m_dead ? ccColor3B{255, 90, 90} : ccColor3B{255, 255, 255});

        auto const winSize = CCDirector::sharedDirector()->getWinSize();
        float const left = 40.f;
        float const right = winSize.width - 40.f;
        float const y = 30.f;
        constexpr int kSpan = 480;
        m_timeline->clear();
        m_timeline->drawSegment({left, y}, {right, y}, 1.f, ccc4f(.3f, .35f, .55f, .9f));
        auto xOf = [&](int tick) {
            return left +
                   (right - left) * static_cast<float>(tick - (m_cursor - kSpan)) / static_cast<float>(2 * kSpan);
        };
        for (int tick = std::max(0, m_cursor - kSpan); tick < m_cursor + kSpan; ++tick) {
            if (!(inputAt(tick) & 1)) continue;
            bool const edited = m_firstEdit >= 0 && tick >= m_firstEdit && tick < m_cursor;
            float const px = xOf(tick);
            m_timeline->drawSegment({px, y - 5.f}, {px, y + 5.f}, .6f,
                                    edited ? ccc4f(1.f, .68f, .2f, 1.f) : ccc4f(1.f, .85f, .3f, .8f));
        }
        float const cx = xOf(m_cursor);
        m_timeline->drawSegment({cx, y - 9.f}, {cx, y + 9.f}, 1.2f, ccc4f(1.f, 1.f, 1.f, 1.f));
    }

}
