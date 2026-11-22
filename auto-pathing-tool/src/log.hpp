// Declaration of the mod log.

#pragma once

#include <Geode/Geode.hpp>

#include <string>

namespace ap {

    class Log {
    public:
        static void print(std::string const& line);

        // writes one line formatted with {}
        template <class... Args>
        static void printf(fmt::format_string<Args...> format, Args&&... args) {
            print(fmt::format(format, std::forward<Args>(args)...));
        }
    };

}
