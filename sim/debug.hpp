// Debug switches of the simulator, read once from APSIM_* environment variables. Only apsim uses them: the mod never
// sets these variables, so in the game every switch stays off.

#pragma once
#include <cstdlib>

namespace ap::sim {
    struct DebugOptions {
        // APSIM_CHECK=1..3: self checks of the search (state copies, keys, plans), more checks at higher levels
        int check = 0;
        // APSIM_STATS: counters of the search and the collision filters, printed at the end
        bool stats = false;
        // APSIM_TRIGLOG=tick: one line per trigger fired (moves, item compares, spawns...) from that tick on
        int trigLogFrom = -1;
        // APSIM_MEMOCHECK=1..7: runs every tick twice, with and without the shared trigger memo, and reports a
        // mismatch (bit 4 also traces the first mismatching tick)
        int memoCheck = 0;
        // APSIM_TRACK=macro, APSIM_TRACK_FROM / _TO=ticks: follows a known macro inside the search and prints why
        // the search drops its states
        char const* track = nullptr;
        int trackFrom = -1;
        int trackTo = -1;
        // APSIM_PATH=N: apsim prints the player state of the found path every N ticks
        int pathEvery = 0;
        // APSIM_RNGLOG=from,to: logs the rng draws between those ticks
        int rngLogFrom = -1;
        int rngLogTo = -1;
        // level context forced by apsim instead of the recording's: APSIM_SEED (rng seed), APSIM_ATTEMPTS (attempt
        // counter of the game), APSIM_PITEMS=item:value,... (persistent items)
        char const* seed = nullptr;
        int attempts = -1;
        char const* persistentItems = nullptr;

        bool trigLogAt(int tick) const { return trigLogFrom >= 0 && tick >= trigLogFrom; }

        static DebugOptions fromEnv() {
            DebugOptions o;
            auto num = [](char const* name, int fallback) {
                char const* value = std::getenv(name);
                return value ? std::atoi(value) : fallback;
            };
            o.check = num("APSIM_CHECK", 0);
            if (std::getenv("APSIM_CHECK") && o.check < 1) o.check = 1;
            o.stats = std::getenv("APSIM_STATS") != nullptr;
            o.trigLogFrom = num("APSIM_TRIGLOG", -1);
            if (std::getenv("APSIM_TRIGLOG") && o.trigLogFrom < 0) o.trigLogFrom = 0;
            o.memoCheck = num("APSIM_MEMOCHECK", 0);
            o.track = std::getenv("APSIM_TRACK");
            o.trackFrom = num("APSIM_TRACK_FROM", -1);
            o.trackTo = num("APSIM_TRACK_TO", -1);
            o.pathEvery = num("APSIM_PATH", 0);
            if (char const* range = std::getenv("APSIM_RNGLOG")) {
                char* end = nullptr;
                o.rngLogFrom = static_cast<int>(std::strtol(range, &end, 10));
                o.rngLogTo = (end && *end == ',') ? std::atoi(end + 1) : o.rngLogFrom;
            }
            o.seed = std::getenv("APSIM_SEED");
            o.attempts = num("APSIM_ATTEMPTS", -1);
            o.persistentItems = std::getenv("APSIM_PITEMS");
            return o;
        }
    };

    // the switches, read on the first call
    inline DebugOptions const& debugOptions() {
        static DebugOptions const options = DebugOptions::fromEnv();
        return options;
    }
}
