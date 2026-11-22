// Macro types: inputs, toggles and the Macro container.

#pragma once

#include <Geode/Geode.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ap {

    struct Toggle {
        int step;
        bool down;
        bool p2 = false;
        // sorted by tick, a release goes before a press on the same tick
        bool operator<(Toggle const& other) const {
            return step < other.step || (step == other.step && !down && other.down);
        }
    };

    struct Input {
        int press;
        int hold;
        // tick of the release of this input
        int release() const { return press + hold; }
    };

    class Macro {
    public:
        std::vector<Toggle> toggles;

        // removes every toggle
        void clear() { toggles.clear(); }
        // true if the macro has no toggle
        bool empty() const { return toggles.empty(); }
        size_t inputCount() const;

        void append(Input input);
        void popLast();
        Input last() const;
        bool heldBefore(int step) const;
        void truncateBefore(int step);

        geode::Result<std::filesystem::path> exportGDR(std::filesystem::path const& dir, std::string const& levelName,
                                                       int levelId, std::string const& botVersion,
                                                       std::string const& fileSuffix = "") const;
    };

    struct MacroFile {
        Macro macro;
        std::string levelName;
        int levelId = 0;
    };
    geode::Result<MacroFile> importGDR(std::filesystem::path const& path);

}
