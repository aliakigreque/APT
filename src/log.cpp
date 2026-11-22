// Mod log, written to the Geode console and to auto-pathing-tool.log.

#include "log.hpp"

#include <cstdio>
#include <fstream>
#include <mutex>

using namespace geode::prelude;

namespace ap {

    // log file in the save folder, opened once, null if it can't be opened
    static std::ofstream* logFile() {
        static std::ofstream* f = nullptr;
        static bool tried = false;
        if (!tried) {
            tried = true;
            auto* out = new std::ofstream(Mod::get()->getSaveDir() / "auto-pathing-tool.log",
                                          std::ios::out | std::ios::trunc);
            if (out->is_open())
                f = out;
            else
                delete out;
        }
        return f;
    }

    // writes the line to the geode log and to the file
    void Log::print(std::string const& line) {
        static std::mutex mx;
        std::lock_guard<std::mutex> lock(mx);
        log::info("{}", line);
        if (auto* file = logFile()) {
            *file << line << '\n';
            file->flush();
        }
    }

}
