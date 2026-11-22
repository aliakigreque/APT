// The F1 menu and its popups: main window, settings, keys, macros list.

#include "menu.hpp"

#include "../actions.hpp"
#include "../input_recorder.hpp"
#include "../simplanner.hpp"
#include "../overlay.hpp"
#include "../settings.hpp"

#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextArea.hpp>
#include <Geode/ui/TextInput.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <vector>

using namespace geode::prelude;

namespace ap::ui {

    namespace {

        constexpr ccColor4B kWhite{255, 255, 255, 255};
        constexpr ccColor4B kGray{200, 200, 200, 255};
        constexpr ccColor4B kRed{255, 120, 120, 255};

        class Window;
        std::vector<Window*> s_windows;
        bool s_capturing = false;
        bool s_pausedByMenu = false;

        // presses resume on the pause layer
        void resumeLevel() {
            auto* pl = PlayLayer::get();
            if (!pl || !pl->m_isPaused) return;
            if (auto* scene = CCDirector::sharedDirector()->getRunningScene()) {
                for (auto* child : CCArrayExt<CCNode*>(scene->getChildren())) {
                    if (auto* pause = typeinfo_cast<PauseLayer*>(child)) {
                        pause->onResume(nullptr);
                        return;
                    }
                }
            }
        }

        class Window : public geode::Popup {
        protected:
            // popup with a title, added to s_windows
            bool initWindow(float width, float height, char const* title) {
                if (!Popup::init(width, height)) return false;
                this->setTitle(title);
                s_windows.push_back(this);
                return true;
            }

            // removes this window from s_windows
            void forget() { s_windows.erase(std::remove(s_windows.begin(), s_windows.end(), this), s_windows.end()); }

            // forgets the window when it is destroyed
            ~Window() override { forget(); }

        public:
            // shows the current key names again, after the keys changed
            virtual void refreshKeys() {}

            // resumes the level after the last window if the menu paused it
            void onClose(CCObject* sender) override {
                forget();
                Popup::onClose(sender);
                if (s_windows.empty() && s_pausedByMenu) {
                    s_pausedByMenu = false;
                    geode::queueInMainThread([] { resumeLevel(); });
                }
            }
        };

        // closes every window, top first (on a copy, onClose edits the list)
        void closeAll() {
            auto copy = s_windows;
            for (auto it = copy.rbegin(); it != copy.rend(); ++it) (*it)->onClose(nullptr);
        }

        // button sprite with a text in the game's style
        ButtonSprite* buttonSprite(char const* text, char const* background = "GJ_button_01.png", float scale = .8f) {
            auto* sprite = ButtonSprite::create(text, "goldFont.fnt", background, .8f);
            sprite->setScale(scale);
            return sprite;
        }

        // centered chatFont text
        SimpleTextArea* textArea(std::string const& text, float scale, float width, ccColor4B color = kWhite) {
            auto* area = SimpleTextArea::create(text, "chatFont.fnt", scale, width);
            area->setAlignment(kCCTextAlignmentCenter);
            area->setAnchorPoint({.5f, .5f});
            area->setColor(color);
            return area;
        }

        // bigFont label
        CCLabelBMFont* label(std::string const& text, float scale, ccColor3B color = {255, 255, 255}) {
            auto* result = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
            result->setScale(scale);
            result->setColor(color);
            return result;
        }

        // pauses the level under the menu
        void pauseLevel() {
            auto* pl = PlayLayer::get();
            if (!pl || pl->m_isPaused || pl->m_hasCompletedLevel || pl->m_levelEndAnimationStarted) return;
            pl->pauseGame(false);
            s_pausedByMenu = true;
        }

        // macros folder in the save dir
        std::filesystem::path macrosDir() { return Mod::get()->getSaveDir() / "macros"; }

        class SettingsPopup : public Window {
            struct NumberRow {
                char const* key;
                int min;
                int max;
                int step;
                TextInput* input = nullptr;
            };
            std::vector<NumberRow> m_rows;
            CCLabelBMFont* m_corner = nullptr;
            static constexpr char const* kToggleKeys[] = {"auto-reset", "fast-search"};
            std::vector<CCMenuItemToggler*> m_toggles;

        public:
            // usual cocos create
            static SettingsPopup* create() {
                auto* popup = new SettingsPopup();
                if (popup->initSettings()) {
                    popup->autorelease();
                    return popup;
                }
                delete popup;
                return nullptr;
            }

        protected:
            // one row per number setting, then the corner row and reset
            bool initSettings() {
                if (!initWindow(360.f, 300.f, "Settings")) return false;
                m_rows = {
                        {"max-cps", 2, 60, 1},
                        {"search-beam", 100, 2400, 50},
                        {"search-time-limit", 0, 3600, 10},
                };
                char const* names[] = {"Max clicks per second", "Beam (states per tick)",
                                       "Offline time budget (s, 0 = none)"};
                for (size_t i = 0; i < m_rows.size(); ++i) {
                    float const y = 90.f - 32.f * static_cast<float>(i);
                    auto* name = label(names[i], .35f);
                    name->setAnchorPoint({0.f, .5f});
                    m_mainLayer->addChildAtPosition(name, Anchor::Center, ccp(-160.f, y));

                    auto* minus = CCMenuItemSpriteExtra::create(buttonSprite("-", "GJ_button_04.png", .6f), this,
                                                                menu_selector(SettingsPopup::onStep));
                    minus->setTag(static_cast<int>(i) * 2);
                    m_buttonMenu->addChildAtPosition(minus, Anchor::Center, ccp(52.f, y));

                    auto* input = TextInput::create(60.f, "0", "bigFont.fnt");
                    input->setCommonFilter(CommonFilter::Uint);
                    input->setMaxCharCount(4);
                    input->setScale(.8f);
                    input->setString(std::to_string(Mod::get()->getSettingValue<int64_t>(m_rows[i].key)));
                    input->setCallback([this, i](std::string const& text) { this->onTyped(i, text); });
                    m_mainLayer->addChildAtPosition(input, Anchor::Center, ccp(100.f, y));
                    m_rows[i].input = input;

                    auto* plus = CCMenuItemSpriteExtra::create(buttonSprite("+", "GJ_button_04.png", .6f), this,
                                                               menu_selector(SettingsPopup::onStep));
                    plus->setTag(static_cast<int>(i) * 2 + 1);
                    m_buttonMenu->addChildAtPosition(plus, Anchor::Center, ccp(148.f, y));
                }

                float const cornerY = 90.f - 32.f * 3.f;
                auto* name = label("Bot info position", .35f);
                name->setAnchorPoint({0.f, .5f});
                m_mainLayer->addChildAtPosition(name, Anchor::Center, ccp(-160.f, cornerY));
                auto* prev = CCMenuItemSpriteExtra::create(buttonSprite("<", "GJ_button_04.png", .6f), this,
                                                           menu_selector(SettingsPopup::onCorner));
                prev->setTag(-1);
                m_buttonMenu->addChildAtPosition(prev, Anchor::Center, ccp(52.f, cornerY));
                m_corner = label(kStatusCorners[Settings::get().statusCorner], .35f);
                m_mainLayer->addChildAtPosition(m_corner, Anchor::Center, ccp(100.f, cornerY));
                auto* next = CCMenuItemSpriteExtra::create(buttonSprite(">", "GJ_button_04.png", .6f), this,
                                                           menu_selector(SettingsPopup::onCorner));
                next->setTag(1);
                m_buttonMenu->addChildAtPosition(next, Anchor::Center, ccp(148.f, cornerY));

                char const* toggleNames[] = {"Auto reset (live dead end)", "Fastest search (less smooth)"};
                for (int i = 0; i < 2; ++i) {
                    float const y = cornerY - 32.f * static_cast<float>(i + 1);
                    auto* name = label(toggleNames[i], .35f);
                    name->setAnchorPoint({0.f, .5f});
                    m_mainLayer->addChildAtPosition(name, Anchor::Center, ccp(-160.f, y));
                    auto* toggler = CCMenuItemToggler::createWithStandardSprites(
                            this, menu_selector(SettingsPopup::onToggle), .6f);
                    toggler->toggle(Mod::get()->getSettingValue<bool>(kToggleKeys[i]));
                    toggler->setTag(i);
                    m_buttonMenu->addChildAtPosition(toggler, Anchor::Center, ccp(148.f, y));
                    m_toggles.push_back(toggler);
                }

                auto* reset = CCMenuItemSpriteExtra::create(buttonSprite("Reset to defaults", "GJ_button_06.png", .65f),
                                                            this, menu_selector(SettingsPopup::onReset));
                m_buttonMenu->addChildAtPosition(reset, Anchor::Bottom, ccp(0.f, 26.f));
                return true;
            }

            // clamps and saves the value, then shows it
            void setValue(size_t index, int value) {
                auto& row = m_rows[index];
                value = std::clamp(value, row.min, row.max);
                Mod::get()->setSettingValue<int64_t>(row.key, value);
                row.input->setString(std::to_string(value));
            }

            // - or + button
            void onStep(CCObject* sender) {
                int const tag = sender->getTag();
                size_t const i = static_cast<size_t>(tag / 2);
                if (i >= m_rows.size()) return;
                int const current = static_cast<int>(Mod::get()->getSettingValue<int64_t>(m_rows[i].key));
                setValue(i, current + (tag % 2 ? m_rows[i].step : -m_rows[i].step));
            }

            // saves a typed value only if it's in range, onClose fixes the rest
            void onTyped(size_t index, std::string const& text) {
                if (text.empty() || text.size() > 6) return;
                int const value = std::atoi(text.c_str());
                if (value >= m_rows[index].min && value <= m_rows[index].max) {
                    Mod::get()->setSettingValue<int64_t>(m_rows[index].key, value);
                }
            }

            // the toggler state flips after this callback, hence the !
            void onToggle(CCObject* sender) {
                int const i = sender->getTag();
                if (i < 0 || i >= 2) return;
                Mod::get()->setSettingValue<bool>(kToggleKeys[i],
                                                  !static_cast<CCMenuItemToggler*>(sender)->isToggled());
            }

            // next or previous corner, wraps around
            void onCorner(CCObject* sender) {
                int const corner = (Settings::get().statusCorner + sender->getTag() + 4) % 4;
                Mod::get()->setSettingValue<std::string>("status-position", kStatusCorners[corner]);
                m_corner->setString(kStatusCorners[corner]);
            }

            // resets the settings and shows the new values
            void onReset(CCObject*) {
                resetSettingsToDefaults();
                for (auto& row : m_rows) {
                    row.input->setString(std::to_string(Mod::get()->getSettingValue<int64_t>(row.key)));
                }
                m_corner->setString(kStatusCorners[Settings::get().statusCorner]);
                for (size_t i = 0; i < m_toggles.size(); ++i)
                    m_toggles[i]->toggle(Mod::get()->getSettingValue<bool>(kToggleKeys[i]));
                Notification::create("Auto Pathing Tool: settings reset to defaults", NotificationIcon::Success)
                        ->show();
            }

        public:
            // out of range typed values get clamped
            void onClose(CCObject* sender) override {
                for (size_t i = 0; i < m_rows.size(); ++i) {
                    int const value = std::atoi(std::string(m_rows[i].input->getString()).c_str());
                    if (value < m_rows[i].min || value > m_rows[i].max) setValue(i, value);
                }
                Window::onClose(sender);
            }
        };

        class KeysPopup : public Window {
            std::vector<ButtonSprite*> m_keys;
            int m_capture = -1;

        public:
            // usual cocos create
            static KeysPopup* create() {
                auto* popup = new KeysPopup();
                if (popup->initKeys()) {
                    popup->autorelease();
                    return popup;
                }
                delete popup;
                return nullptr;
            }

        protected:
            // one row per action
            bool initKeys() {
                if (!initWindow(340.f, 300.f, "Keys")) return false;
                int const count = static_cast<int>(Action::Count);
                for (int i = 0; i < count; ++i) {
                    float const y = 94.f - 24.f * static_cast<float>(i);
                    auto* name = label(actionLabel(static_cast<Action>(i)), .38f);
                    name->setAnchorPoint({0.f, .5f});
                    m_mainLayer->addChildAtPosition(name, Anchor::Center, ccp(-150.f, y));
                    name->setScale(std::min(.38f, 220.f / std::max(1.f, name->getContentSize().width)));
                    auto* sprite = buttonSprite(keyName(static_cast<Action>(i)).c_str(), "GJ_button_04.png", .55f);
                    auto* button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(KeysPopup::onKey));
                    button->setTag(i);
                    m_buttonMenu->addChildAtPosition(button, Anchor::Center, ccp(115.f, y));
                    m_keys.push_back(sprite);
                }
                auto* hint = textArea("Click a key, then press the new one. Esc: cancel. Delete: no key.", .5f, 290.f,
                                      kGray);
                m_mainLayer->addChildAtPosition(hint, Anchor::Bottom, ccp(0.f, 58.f));
                auto* reset = CCMenuItemSpriteExtra::create(buttonSprite("Reset keys", "GJ_button_06.png", .6f), this,
                                                            menu_selector(KeysPopup::onReset));
                m_buttonMenu->addChildAtPosition(reset, Anchor::Bottom, ccp(0.f, 26.f));
                return true;
            }

            // shows the current keys
            void refresh() {
                for (size_t i = 0; i < m_keys.size(); ++i) {
                    m_keys[i]->setString(keyName(static_cast<Action>(i)).c_str());
                }
            }

            // stops waiting for a key
            void stopCapture() {
                m_capture = -1;
                s_capturing = false;
                refresh();
            }

            // starts waiting for a key
            void onKey(CCObject* sender) {
                if (m_capture >= 0) stopCapture();
                m_capture = sender->getTag();
                s_capturing = true;
                m_keys[static_cast<size_t>(m_capture)]->setString("Press a key...");
            }

            // resets every key
            void onReset(CCObject*) {
                stopCapture();
                resetKeysToDefaults();
                refresh();
            }

            // keys that can't be bound alone
            static bool isModifier(enumKeyCodes key) {
                switch (key) {
                case KEY_Shift:
                case KEY_Control:
                case KEY_Alt:
                case KEY_LeftShift:
                case KEY_RightShift:
                case KEY_LeftControl:
                case KEY_RightControl:
                case KEY_LeftMenu:
                case KEY_RightMenu:
                case KEY_LeftWindowsKey:
                case KEY_RightWindowsKey:
                case KEY_CapsLock:
                case KEY_None:
                    return true;
                default:
                    return false;
                }
            }

        public:
            // while waiting: esc cancels, delete or backspace unbinds, any other key (with modifiers) binds
            void keyDown(enumKeyCodes key, double time) override {
                if (m_capture < 0) return Popup::keyDown(key, time);
                if (key == KEY_Escape) return stopCapture();
                if (isModifier(key)) return;
                auto const action = static_cast<Action>(m_capture);
                std::vector<Keybind> value;
                if (key != KEY_Delete && key != KEY_Backspace) {
                    auto* dispatcher = CCKeyboardDispatcher::get();
                    KeyboardModifier modifiers = KeyboardModifier::None;
                    if (dispatcher->getShiftKeyPressed()) modifiers |= KeyboardModifier::Shift;
                    if (dispatcher->getControlKeyPressed()) modifiers |= KeyboardModifier::Control;
                    if (dispatcher->getAltKeyPressed()) modifiers |= KeyboardModifier::Alt;
                    Keybind const keybind(key, modifiers);
                    value.push_back(keybind);
                    for (int i = 0; i < static_cast<int>(Action::Count); ++i) {
                        if (i == m_capture) continue;
                        auto const other = Mod::get()->getSettingValue<std::vector<Keybind>>(
                                actionSettingKey(static_cast<Action>(i)));
                        if (std::find(other.begin(), other.end(), keybind) != other.end()) {
                            Notification::create(fmt::format("Auto Pathing Tool: {} is also the key of \"{}\"",
                                                             keybind.toString(), actionLabel(static_cast<Action>(i))),
                                                 NotificationIcon::Warning, 3.f)
                                    ->show();
                        }
                    }
                }
                Mod::get()->setSettingValue<std::vector<Keybind>>(actionSettingKey(action), value);
                refreshKeyNames();
                stopCapture();
            }

            // esc cancels the capture instead of closing
            void keyBackClicked() override {
                if (m_capture >= 0) return stopCapture();
                Popup::keyBackClicked();
            }

            // cancels a capture before closing
            void onClose(CCObject* sender) override {
                m_capture = -1;
                s_capturing = false;
                Window::onClose(sender);
                for (auto* window : s_windows) window->refreshKeys();
            }
        };

        class MacrosPopup : public Window {
            static constexpr int kPerPage = 5;
            std::vector<std::filesystem::path> m_files;
            int m_page = 0;
            std::vector<CCNode*> m_rowNodes;
            CCLabelBMFont* m_pageLabel = nullptr;
            SimpleTextArea* m_info = nullptr;

        public:
            // usual cocos create
            static MacrosPopup* create() {
                auto* popup = new MacrosPopup();
                if (popup->initMacros()) {
                    popup->autorelease();
                    return popup;
                }
                delete popup;
                return nullptr;
            }

        protected:
            // page arrows, info line and open folder button
            bool initMacros() {
                if (!initWindow(380.f, 270.f, "Load a macro")) return false;
                scan();

                if (auto* prevSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png")) {
                    prevSpr->setScale(.6f);
                    auto* prev = CCMenuItemSpriteExtra::create(prevSpr, this, menu_selector(MacrosPopup::onPage));
                    prev->setTag(-1);
                    prev->setID("previous-page"_spr);
                    m_buttonMenu->addChildAtPosition(prev, Anchor::Left, ccp(-6.f, 0.f));
                }
                if (auto* nextSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png")) {
                    nextSpr->setFlipX(true);
                    nextSpr->setScale(.6f);
                    auto* next = CCMenuItemSpriteExtra::create(nextSpr, this, menu_selector(MacrosPopup::onPage));
                    next->setTag(1);
                    next->setID("next-page"_spr);
                    m_buttonMenu->addChildAtPosition(next, Anchor::Right, ccp(6.f, 0.f));
                }

                m_pageLabel = label("", .35f, {200, 200, 200});
                m_mainLayer->addChildAtPosition(m_pageLabel, Anchor::Bottom, ccp(0.f, 66.f));
                m_info = textArea("", .55f, 340.f, kGray);
                m_mainLayer->addChildAtPosition(m_info, Anchor::Bottom, ccp(0.f, 48.f));

                auto* folder = CCMenuItemSpriteExtra::create(buttonSprite("Open folder", "GJ_button_04.png", .6f), this,
                                                             menu_selector(MacrosPopup::onFolder));
                m_buttonMenu->addChildAtPosition(folder, Anchor::Bottom, ccp(0.f, 22.f));

                build();
                return true;
            }

            // lists the gdr files, newest first
            void scan() {
                m_files.clear();
                std::error_code error;
                auto const dir = macrosDir();
                if (!std::filesystem::is_directory(dir, error)) return;
                std::filesystem::directory_iterator it(dir, error), end;
                for (; !error && it != end; it.increment(error)) {
                    std::error_code fileError;
                    if (!it->is_regular_file(fileError)) continue;
                    auto const name = geode::utils::string::pathToString(it->path().filename());
                    if (name.ends_with(".gdr.json") || name.ends_with(".gdr") || name.ends_with(".json")) {
                        m_files.push_back(it->path());
                    }
                }
                std::sort(m_files.begin(), m_files.end(), [](auto const& a, auto const& b) {
                    std::error_code errorA;
                    std::error_code errorB;
                    return std::filesystem::last_write_time(a, errorA) > std::filesystem::last_write_time(b, errorB);
                });
            }

            // file name without extension, spaces instead of _, cut to 34 characters
            static std::string displayName(std::filesystem::path const& path) {
                std::string name = geode::utils::string::pathToString(path.filename());
                for (char const* suffix : {".gdr.json", ".gdr", ".json"}) {
                    std::string_view const extension = suffix;
                    if (name.size() > extension.size() && name.ends_with(extension)) {
                        name.resize(name.size() - extension.size());
                        break;
                    }
                }
                std::replace(name.begin(), name.end(), '_', ' ');
                if (name.size() > 34) name = name.substr(0, 33) + "...";
                return name;
            }

            // rebuilds the rows of the current page
            void build() {
                for (auto* node : m_rowNodes) node->removeFromParent();
                m_rowNodes.clear();
                int const pages = std::max(1, (static_cast<int>(m_files.size()) + kPerPage - 1) / kPerPage);
                m_page = std::clamp(m_page, 0, pages - 1);
                m_pageLabel->setString(fmt::format("Page {} / {}", m_page + 1, pages).c_str());
                if (m_files.empty()) {
                    m_info->setText(
                            "No macros yet. Each level solved by the bot is saved here, and Save adds the current "
                            "macro.");
                    return;
                }
                m_info->setText("Play opens the macro in the level you are in.");
                for (int k = 0; k < kPerPage; ++k) {
                    int const index = m_page * kPerPage + k;
                    if (index >= static_cast<int>(m_files.size())) break;
                    float const y = 62.f - 30.f * static_cast<float>(k);
                    auto* name = CCLabelBMFont::create(displayName(m_files[static_cast<size_t>(index)]).c_str(),
                                                       "chatFont.fnt");
                    name->setScale(.7f);
                    name->setAnchorPoint({0.f, .5f});
                    m_mainLayer->addChildAtPosition(name, Anchor::Center, ccp(-160.f, y));
                    auto* play = CCMenuItemSpriteExtra::create(buttonSprite("Play", "GJ_button_01.png", .55f), this,
                                                               menu_selector(MacrosPopup::onPlay));
                    play->setTag(index);
                    m_buttonMenu->addChildAtPosition(play, Anchor::Center, ccp(140.f, y));
                    m_rowNodes.push_back(name);
                    m_rowNodes.push_back(play);
                }
            }

            // tag is -1 or 1, build clamps the page
            void onPage(CCObject* sender) {
                m_page += sender->getTag();
                build();
            }

            // opens the macros folder, creates it if needed
            void onFolder(CCObject*) {
                std::error_code error;
                std::filesystem::create_directories(macrosDir(), error);
                file::openFolder(macrosDir());
            }

            // loads the macro and plays it once the menu is closed
            void onPlay(CCObject* sender) {
                size_t const index = static_cast<size_t>(sender->getTag());
                if (index >= m_files.size()) return;
                if (!PlayLayer::get()) {
                    m_info->setText("Open the level first, then play its macro from here.");
                    m_info->setColor(kRed);
                    return;
                }
                auto result = importGDR(m_files[index]);
                if (result.isErr()) {
                    m_info->setText(fmt::format("Can't play this macro: {}.", result.unwrapErr()));
                    m_info->setColor(kRed);
                    return;
                }
                auto file = std::move(result).unwrap();
                closeAll();
                geode::queueInMainThread([file = std::move(file)] { actions::playFile(file); });
            }
        };

        // opens a web page from the menu
        void openLink(char const* url) { geode::utils::web::openLinkInBrowser(url); }

        // dark rounded box behind a section of the menu (darker for the boxes inside a section)
        CCScale9Sprite* panel(float width, float height, GLubyte opacity = 70) {
            auto* box = CCScale9Sprite::create("square02_small.png");
            box->setContentSize({width, height});
            box->setColor({0, 0, 0});
            box->setOpacity(opacity);
            return box;
        }

        // button of a fixed width, so the two buttons of a section fill it
        ButtonSprite* wideButton(char const* text, char const* background) {
            auto* sprite = ButtonSprite::create(text, 76, true, "goldFont.fnt", background, 28.f, .7f);
            sprite->setScale(.95f);
            return sprite;
        }

        // the key's name, plain and bright
        CCLabelBMFont* keycap(std::string const& key) {
            auto* text = CCLabelBMFont::create(key.c_str(), "goldFont.fnt");
            text->setScale(.42f);
            return text;
        }

        // logo from the mod's resources, scaled to a size in points
        CCSprite* logo(char const* file, float size) {
            auto* sprite = CCSprite::create(file);
            if (sprite && sprite->getContentSize().width > 0.f) sprite->setScale(size / sprite->getContentSize().width);
            return sprite;
        }

        class MenuPopup : public Window {
            SimpleTextArea* m_info = nullptr;
            std::vector<std::pair<CCLabelBMFont*, Action>> m_keycaps;

        public:
            // usual cocos create
            static MenuPopup* create() {
                auto* popup = new MenuPopup();
                if (popup->initMenu()) {
                    popup->autorelease();
                    return popup;
                }
                delete popup;
                return nullptr;
            }

        protected:
            enum Check { ShowPath, ShowInfo, SafeMode, Record, Play, SaveOnDeath };

            // checkbox with its text, in a section
            float addCheck(Check check, char const* text, bool on, CCPoint pos) {
                auto* toggler =
                        CCMenuItemToggler::createWithStandardSprites(this, menu_selector(MenuPopup::onCheck), .55f);
                toggler->toggle(on);
                toggler->setTag(check);
                m_buttonMenu->addChildAtPosition(toggler, Anchor::Center, pos);
                auto* name = label(text, .34f);
                name->setAnchorPoint({0.f, .5f});
                m_mainLayer->addChildAtPosition(name, Anchor::Center, pos + ccp(15.f, 0.f));
                return 15.f + name->getScaledContentSize().width;
            }

            // section title
            void addHeader(char const* text, CCPoint pos) {
                auto* title = CCLabelBMFont::create(text, "goldFont.fnt");
                title->setScale(.6f);
                m_mainLayer->addChildAtPosition(title, Anchor::Center, pos);
            }

            // builds the main menu window
            bool initMenu() {
                if (!initWindow(440.f, 290.f, "Auto Pathing Tool")) return false;
                auto const& options = Settings::get();

                if (auto* discordLogo = logo("discord.png"_spr, 32.f)) {
                    auto* discord =
                            CCMenuItemSpriteExtra::create(discordLogo, this, menu_selector(MenuPopup::onDiscord));
                    discord->setID("discord"_spr);
                    m_buttonMenu->addChildAtPosition(discord, Anchor::TopRight, ccp(-26.f, -25.f));
                }

                m_info = textArea(infoText(), .5f, 380.f, kGray);
                m_mainLayer->addChildAtPosition(m_info, Anchor::Top, ccp(0.f, -42.f));

                CCPoint const left{-108.f, 4.f};
                m_mainLayer->addChildAtPosition(panel(204.f, 186.f), Anchor::Center, left);
                addHeader("Pathing Tool", left + ccp(0.f, 78.f));
                m_mainLayer->addChildAtPosition(panel(190.f, 50.f, 110), Anchor::Center, left + ccp(0.f, 41.f));
                Action const actionsShown[] = {Action::Live, Action::Edit, Action::Offline};
                for (int i = 0; i < 3; ++i) {
                    float const y = 56.f - 15.f * static_cast<float>(i);
                    auto* cap = keycap(keyName(actionsShown[i]));
                    m_keycaps.emplace_back(cap, actionsShown[i]);
                    cap->setAnchorPoint({0.f, .5f});
                    m_mainLayer->addChildAtPosition(cap, Anchor::Center, left + ccp(-88.f, y));
                    auto* line = CCLabelBMFont::create(actionLabel(actionsShown[i]), "chatFont.fnt");
                    line->setAnchorPoint({0.f, .5f});
                    line->setScale(std::min(.5f, 140.f / std::max(1.f, line->getContentSize().width)));
                    line->setColor({255, 233, 194});
                    m_mainLayer->addChildAtPosition(line, Anchor::Center, left + ccp(-58.f, y));
                }
                addCheck(ShowPath, "Show path", options.overlay, left + ccp(-80.f, 2.f));
                addCheck(ShowInfo, "Show info", options.showStatus, left + ccp(-80.f, -22.f));
                addCheck(SafeMode, "Safe mode", options.safeMode, left + ccp(-80.f, -46.f));
                auto* keys = CCMenuItemSpriteExtra::create(wideButton("Keys", "GJ_button_04.png"), this,
                                                           menu_selector(MenuPopup::onKeys));
                m_buttonMenu->addChildAtPosition(keys, Anchor::Center, left + ccp(-47.f, -74.f));
                auto* settings = CCMenuItemSpriteExtra::create(wideButton("Settings", "GJ_button_04.png"), this,
                                                               menu_selector(MenuPopup::onSettings));
                m_buttonMenu->addChildAtPosition(settings, Anchor::Center, left + ccp(47.f, -74.f));

                CCPoint const right{108.f, 4.f};
                m_mainLayer->addChildAtPosition(panel(204.f, 186.f), Anchor::Center, right);
                addHeader("Macro", right + ccp(0.f, 78.f));
                m_mainLayer->addChildAtPosition(panel(190.f, 50.f, 110), Anchor::Center, right + ccp(0.f, 41.f));
                auto* caption = CCLabelBMFont::create("CURRENT MACRO", "chatFont.fnt");
                caption->setScale(.42f);
                caption->setColor({232, 199, 156});
                caption->setAnchorPoint({0.f, .5f});
                m_mainLayer->addChildAtPosition(caption, Anchor::Center, right + ccp(-88.f, 58.f));
                auto* current = textArea(macroText(), .48f, 176.f, {255, 233, 194, 255});
                m_mainLayer->addChildAtPosition(current, Anchor::Center, right + ccp(0.f, 36.f));
                addCheck(Record, "Record my inputs", InputRecorder::get().enabled(), right + ccp(-80.f, 2.f));
                float const playEnd = addCheck(Play, "Play", actions::playing(), right + ccp(-80.f, -22.f));
                auto* playKey = keycap(keyName(Action::Play));
                m_keycaps.emplace_back(playKey, Action::Play);
                playKey->setAnchorPoint({0.f, .5f});
                m_mainLayer->addChildAtPosition(playKey, Anchor::Center, right + ccp(-80.f + playEnd + 6.f, -22.f));
                addCheck(SaveOnDeath, "Save on death", options.saveOnDeath, right + ccp(-80.f, -46.f));
                auto* load = CCMenuItemSpriteExtra::create(wideButton("Load", "GJ_button_01.png"), this,
                                                           menu_selector(MenuPopup::onLoad));
                m_buttonMenu->addChildAtPosition(load, Anchor::Center, right + ccp(-47.f, -74.f));
                auto* save = CCMenuItemSpriteExtra::create(wideButton("Save", "GJ_button_01.png"), this,
                                                           menu_selector(MenuPopup::onSave));
                m_buttonMenu->addChildAtPosition(save, Anchor::Center, right + ccp(47.f, -74.f));

                auto* band = CCNode::create();
                band->setContentSize({330.f, 34.f});
                band->setAnchorPoint({.5f, .5f});
                auto* bandBack = CCScale9Sprite::create("GJ_square02.png");
                bandBack->setContentSize({330.f, 34.f});
                bandBack->setColor({41, 171, 224});
                bandBack->setPosition({165.f, 17.f});
                band->addChild(bandBack);
                if (auto* kofiLogo = logo("kofi.png"_spr, 22.f)) {
                    kofiLogo->setPosition({24.f, 17.f});
                    band->addChild(kofiLogo);
                }
                auto* phrase = CCLabelBMFont::create("This bot runs on coffee. Keep it brewing!", "bigFont.fnt");
                phrase->setScale(std::min(.36f, 270.f / std::max(1.f, phrase->getContentSize().width)));
                phrase->setPosition({185.f, 22.f});
                band->addChild(phrase);
                auto* tip =
                        CCLabelBMFont::create("The mod is free. Click here to leave a tip on Ko-fi!", "chatFont.fnt");
                tip->setScale(.45f);
                tip->setPosition({185.f, 9.f});
                band->addChild(tip);
                auto* kofi = CCMenuItemSpriteExtra::create(band, this, menu_selector(MenuPopup::onKofi));
                kofi->setID("ko-fi"_spr);
                m_buttonMenu->addChildAtPosition(kofi, Anchor::Bottom, ccp(0.f, 24.f));
                return true;
            }

            // shows the keys chosen in the keys window
            void refreshKeys() override {
                for (auto const& [cap, action] : m_keycaps) cap->setString(keyName(action).c_str());
            }

            // level name
            static std::string infoText() {
                if (auto* pl = PlayLayer::get(); pl && pl->m_level)
                    return fmt::format("Level: {}", std::string(pl->m_level->m_levelName));
                return "Open a level to use the bot.";
            }

            // ticks and presses of the current macro
            static std::string macroText() {
                auto const& macro = SimPlanner::get().macro();
                if (macro.empty()) return "None yet: run the bot, record your inputs or load one.";
                int const ticks = macro.toggles.back().step + 1;
                return fmt::format("{} ticks ({:.1f} s), {} inputs", ticks, ticks / 240.0, macro.inputCount());
            }

            // toggles an option from its checkbox (the state flips after this callback)
            void onCheck(CCObject* sender) {
                bool const on = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
                switch (sender->getTag()) {
                case ShowPath:
                    Mod::get()->setSettingValue<bool>("overlay", on);
                    break;
                case ShowInfo:
                    Mod::get()->setSettingValue<bool>("show-status", on);
                    break;
                case SafeMode:
                    Mod::get()->setSettingValue<bool>("safe-mode", on);
                    break;
                case SaveOnDeath:
                    Mod::get()->setSettingValue<bool>("save-on-death", on);
                    break;
                case Record:
                    InputRecorder::get().setEnabled(on);
                    break;
                case Play: {
                    if (on == actions::playing()) break;
                    if (!PlayLayer::get()) {
                        m_info->setText("Open the level first, then play its macro.");
                        m_info->setColor(kRed);
                        break;
                    }
                    closeAll();
                    geode::queueInMainThread([] { actions::play(); });
                    break;
                }
                default:
                    break;
                }
            }

            // opens the macros list
            void onLoad(CCObject*) {
                if (auto* popup = MacrosPopup::create()) popup->show();
            }

            // saves the last macro found
            void onSave(CCObject*) {
                if (!PlayLayer::get()) {
                    m_info->setText("Open the level first: the macro is saved with its name.");
                    m_info->setColor(kRed);
                    return;
                }
                actions::exportMacro();
            }

            void onSettings(CCObject*) {
                if (auto* popup = SettingsPopup::create()) popup->show();
            }

            // opens the key bindings
            void onKeys(CCObject*) {
                if (auto* popup = KeysPopup::create()) popup->show();
            }

            // opens the Discord link
            void onDiscord(CCObject*) { openLink("https://discord.gg/S4QzXYf65G"); }

            // opens the Ko-fi link
            void onKofi(CCObject*) { openLink("https://ko-fi.com/aliaky"); }
        };

    }

    // true if a menu window is open
    bool menuOpen() { return !s_windows.empty(); }

    // closes the top window, or opens the menu and pauses the level
    void toggleMenu() {
        if (s_capturing) return;
        if (!s_windows.empty()) {
            s_windows.back()->onClose(nullptr);
            return;
        }
        pauseLevel();
        if (auto* popup = MenuPopup::create()) popup->show();
    }

}
