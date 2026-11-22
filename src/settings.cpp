// Mod settings and key bindings, read from Geode and kept in sync when they change.

#include "settings.hpp"
#include "overlay.hpp"
#include <Geode/loader/SettingV3.hpp>
#include <array>
#include <mutex>

using namespace geode::prelude;

namespace ap {
    namespace {
        std::mutex s_keyMx;
        std::array<std::string, static_cast<size_t>(Action::Count)> s_keyNames;
        constexpr char const* kMenuSettings[] = {"safe-mode",   "auto-reset",     "save-on-death",     "fast-search",
                                                 "max-cps",     "search-beam",    "search-time-limit", "overlay",
                                                 "show-status", "status-position"};
    }

    // the only instance of the settings
    Settings& Settings::get() {
        static Settings instance;
        return instance;
    }

    // reads every setting from geode, clamped to valid values
    void Settings::reload() {
        auto* mod = Mod::get();
        auto getInt = [&](char const* key) { return static_cast<int>(mod->getSettingValue<int64_t>(key)); };

        maxCps = std::clamp(getInt("max-cps"), 2, 60);
        beamWidth = std::clamp(getInt("search-beam"), 100, 2400);
        timeLimit = std::max(0, getInt("search-time-limit"));

        overlay = mod->getSettingValue<bool>("overlay");
        safeMode = mod->getSettingValue<bool>("safe-mode");
        autoReset = mod->getSettingValue<bool>("auto-reset");
        saveOnDeath = mod->getSettingValue<bool>("save-on-death");
        fastSearch = mod->getSettingValue<bool>("fast-search");
        showStatus = mod->getSettingValue<bool>("show-status");
        std::string const corner = mod->getSettingValue<std::string>("status-position");
        statusCorner = 0;
        for (int i = 0; i < 4; ++i)
            if (corner == kStatusCorners[i]) statusCorner = i;
    }

    // geode setting that holds the action key ("key-menu"...)
    char const* actionSettingKey(Action a) {
        switch (a) {
        case Action::Menu:
            return "key-menu";
        case Action::Live:
            return "key-live";
        case Action::Edit:
            return "key-edit";
        case Action::Offline:
            return "key-offline";
        case Action::Play:
            return "key-play-macro";
        case Action::Export:
            return "key-save-macro";
        case Action::Record:
            return "key-record-game";
        default:
            return "";
        }
    }

    // readable name of the action for the menu
    char const* actionLabel(Action a) {
        switch (a) {
        case Action::Menu:
            return "Open the menu";
        case Action::Live:
            return "Find the path while playing";
        case Action::Edit:
            return "Edit the path";
        case Action::Offline:
            return "Find the whole path first";
        case Action::Play:
            return "Play the macro";
        case Action::Export:
            return "Save the macro";
        case Action::Record:
            return "Record the game (bug reports)";
        default:
            return "";
        }
    }

    // current key of the action ("F2", "none"...), safe from any thread
    std::string keyName(Action a) {
        std::lock_guard lock(s_keyMx);
        auto const& name = s_keyNames[static_cast<size_t>(a)];
        return name.empty() ? std::string("none") : name;
    }

    // reads the first keybind of every action, then swaps the names under the lock
    void refreshKeyNames() {
        std::array<std::string, static_cast<size_t>(Action::Count)> names;
        for (size_t i = 0; i < names.size(); ++i) {
            auto const binds =
                    Mod::get()->getSettingValue<std::vector<Keybind>>(actionSettingKey(static_cast<Action>(i)));
            names[i] = binds.empty() ? std::string() : binds.front().toString();
        }
        std::lock_guard lock(s_keyMx);
        s_keyNames = std::move(names);
    }

    // resets the menu settings and applies them
    void resetSettingsToDefaults() {
        for (auto key : kMenuSettings)
            if (auto setting = Mod::get()->getSetting(key)) setting->reset();
        Settings::get().reload();
        Overlay::applySettings();
    }

    // resets every keybind
    void resetKeysToDefaults() {
        for (size_t i = 0; i < static_cast<size_t>(Action::Count); ++i)
            if (auto setting = Mod::get()->getSetting(actionSettingKey(static_cast<Action>(i)))) setting->reset();
        refreshKeyNames();
    }

}

// reads the settings and watches their changes when the mod loads
$on_mod(Loaded) {
    ap::Settings::get().reload();
    ap::refreshKeyNames();

    auto onChange = [] {
        ap::Settings::get().reload();
        ap::Overlay::applySettings();
    };
    for (auto key : {"overlay", "safe-mode", "auto-reset", "save-on-death", "fast-search", "show-status"}) {
        listenForSettingChanges<bool>(key, [onChange](bool) { onChange(); });
    }
    for (auto key : {"max-cps", "search-beam", "search-time-limit"}) {
        listenForSettingChanges<int64_t>(key, [onChange](int64_t) { onChange(); });
    }
    listenForSettingChanges<std::string>("status-position", [onChange](std::string) { onChange(); });
    for (size_t i = 0; i < static_cast<size_t>(ap::Action::Count); ++i) {
        listenForSettingChanges<std::vector<Keybind>>(ap::actionSettingKey(static_cast<ap::Action>(i)),
                                                      [](std::vector<Keybind>) { ap::refreshKeyNames(); });
    }
}
