// Declaration of the press timing check (robustifyPlan).

#pragma once
#include "gd.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace ap::sim {

    struct RobustOptions {
        int maxShift = 8;
        int settle = 90;
        int maxHorizon = 360;
        int minPressGap = 8;
        bool apply = true;
    };

    struct RobustReport {
        int segments = 0;
        int moved = 0;
        int narrow = 0;
        int narrowRelease = 0;
        int noConverge = 0;
        double avgWindow = 0;
        int minWindow = 0;
        std::vector<int> windows;
        bool validated = false;
    };

    std::vector<uint8_t> robustifyPlan(Game const& start, std::vector<uint8_t> const& inputs, RobustOptions const& opt,
                                       RobustReport* report = nullptr, std::atomic<bool> const* cancel = nullptr);

}
