// Copy of the msvc ucrt qsort, so equal items end up in the same order as in the game.

#pragma once
#include <vector>
#include <utility>
#include <cstddef>

namespace ap::sim {
    // copy of the msvc ucrt qsort, comp returns an int like qsort
    template <class T, class Cmp>
    void crtQsort(std::vector<T>& items, Cmp comp) {
        size_t const num = items.size();
        if (num < 2) return;
        auto swp = [&](size_t a, size_t b) {
            if (a != b) std::swap(items[a], items[b]);
        };
        auto shortsort = [&](size_t lo, size_t hi) {
            while (hi > lo) {
                size_t mx = lo;
                for (size_t p = lo + 1; p <= hi; ++p)
                    if (comp(items[p], items[mx]) > 0) mx = p;
                swp(mx, hi);
                --hi;
            }
        };
        std::vector<std::pair<size_t, size_t>> stk;
        size_t lo = 0;
        size_t hi = num - 1;
        for (;;) {
            size_t const size = hi - lo + 1;
            if (size <= 8)
                shortsort(lo, hi);
            else {
                size_t mid = lo + size / 2;
                if (comp(items[lo], items[mid]) > 0) swp(lo, mid);
                if (comp(items[lo], items[hi]) > 0) swp(lo, hi);
                if (comp(items[mid], items[hi]) > 0) swp(mid, hi);
                long long loguy = static_cast<long long>(lo);
                long long higuy = static_cast<long long>(hi);
                long long const H = static_cast<long long>(hi);
                long long const L = static_cast<long long>(lo);
                long long m = static_cast<long long>(mid);
                for (;;) {
                    if (m > loguy) {
                        do {
                            ++loguy;
                        } while (loguy < m &&
                                 comp(items[static_cast<size_t>(loguy)], items[static_cast<size_t>(m)]) <= 0);
                    }
                    if (m <= loguy) {
                        do {
                            ++loguy;
                        } while (loguy <= H &&
                                 comp(items[static_cast<size_t>(loguy)], items[static_cast<size_t>(m)]) <= 0);
                    }
                    do {
                        --higuy;
                    } while (higuy > m && comp(items[static_cast<size_t>(higuy)], items[static_cast<size_t>(m)]) > 0);
                    if (higuy < loguy) break;
                    swp(static_cast<size_t>(loguy), static_cast<size_t>(higuy));
                    if (m == higuy) m = loguy;
                }
                ++higuy;
                if (m < higuy) {
                    do {
                        --higuy;
                    } while (higuy > m && comp(items[static_cast<size_t>(higuy)], items[static_cast<size_t>(m)]) == 0);
                }
                if (m >= higuy) {
                    do {
                        --higuy;
                    } while (higuy > L && comp(items[static_cast<size_t>(higuy)], items[static_cast<size_t>(m)]) == 0);
                }
                if (higuy - L >= H - loguy) {
                    if (L < higuy) stk.emplace_back(lo, static_cast<size_t>(higuy));
                    if (loguy < H) {
                        lo = static_cast<size_t>(loguy);
                        continue;
                    }
                } else {
                    if (loguy < H) stk.emplace_back(static_cast<size_t>(loguy), hi);
                    if (L < higuy) {
                        hi = static_cast<size_t>(higuy);
                        continue;
                    }
                }
            }
            if (stk.empty()) break;
            lo = stk.back().first;
            hi = stk.back().second;
            stk.pop_back();
        }
    }
}
