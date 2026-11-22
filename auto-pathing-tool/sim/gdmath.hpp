// sin and cos with the same float results as the game.

#pragma once
#include <cmath>

namespace ap::sim {
#if defined(_WIN32) || defined(AP_SIM_CRT_TRIG)
    // sin of the game (calls the same sinf)
    inline float gsin(float x) { return ::sinf(x); }
    // cos of the game (calls the same cosf)
    inline float gcos(float x) { return ::cosf(x); }
#else
    namespace gdmath_detail {
        inline double (*volatile const g_sin)(double) = ::sin;
        inline double (*volatile const g_cos)(double) = ::cos;
    }
    // sin with the same float result as the game, outside windows
    inline float gsin(float x) { return static_cast<float>(gdmath_detail::g_sin(static_cast<double>(x))); }
    // cos with the same float result as the game, outside windows
    inline float gcos(float x) { return static_cast<float>(gdmath_detail::g_cos(static_cast<double>(x))); }
#endif
    // sin in double precision
    inline double gsin(double x) { return std::sin(x); }
    // cos in double precision
    inline double gcos(double x) { return std::cos(x); }
}
