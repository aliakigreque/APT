// Declaration of the level export from the game to the simulator.

#pragma once
#include "../sim/world.hpp"

#include <Geode/Geode.hpp>

#include <filesystem>
#include <string>

namespace ap {

    sim::World buildWorld(PlayLayer* pl);

    void applyAreaRandom(PlayLayer* pl);
    void forgetAreaRandom();

    geode::Result<std::filesystem::path> writeWorld(sim::World const& w, PlayLayer* pl,
                                                    std::filesystem::path const& dir, std::string const& tag);

}
