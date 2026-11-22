// Declaration of the mod settings and the actions bound to keys.

#pragma once

#include <Geode/Geode.hpp>

#include <algorithm>
#include <string>

namespace ap {

    struct Settings {
        int maxCps = 12;
        int beamWidth = 150;
        int timeLimit = 0;

        bool safeMode = true;
        bool autoReset = false;
        bool saveOnDeath = false;

        bool fastSearch = false;
        bool overlay = true;
        bool showStatus = true;
        int statusCorner = 0;

        static Settings& get();
        void reload();

        // min ticks between two presses, 240 ticks per second
        int minGapTicks() const { return maxCps > 0 ? std::max(1, 240 / maxCps) : 1; }
    };

    inline constexpr char const* kStatusCorners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};

    enum class Action { Menu, Live, Edit, Offline, Play, Export, Record, Count };
    char const* actionSettingKey(Action a);
    char const* actionLabel(Action a);
    std::string keyName(Action a);
    void refreshKeyNames();

    void resetSettingsToDefaults();
    void resetKeysToDefaults();

}
