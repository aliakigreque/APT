// Copy of std::sort from the msvc stl, so equal items end up in the same order as in the game.

#pragma once
#include <cstddef>
#include <utility>

namespace ap::sim::msvc {

    // insertion sort, used for 32 items or less (_Insertion_sort_unchecked)
    template <class It, class Pr>
    inline void insertionSort(It first, It last, Pr pred) {
        if (first == last) return;
        for (It mid = first; ++mid != last;) {
            It hole = mid;
            auto val = std::move(*mid);
            if (pred(val, *first)) {
                for (It p = mid; p != first; --p) *p = std::move(*(p - 1));
                *first = std::move(val);
            } else {
                for (It prev = hole; pred(val, *--prev); hole = prev) *hole = std::move(*prev);
                *hole = std::move(val);
            }
        }
    }

    // puts the median of the 3 items in mid (_Med3_unchecked)
    template <class It, class Pr>
    inline void med3(It first, It mid, It last, Pr pred) {
        using std::swap;
        if (pred(*mid, *first)) swap(*mid, *first);
        if (pred(*last, *mid)) {
            swap(*last, *mid);
            if (pred(*mid, *first)) swap(*mid, *first);
        }
    }

    // puts the pivot in mid, tukey's ninther above 40 items (_Guess_median_unchecked)
    template <class It, class Pr>
    inline void guessMedian(It first, It mid, It last, Pr pred) {
        std::ptrdiff_t const count = last - first;
        if (40 < count) {
            std::ptrdiff_t const step = (count + 1) >> 3;
            std::ptrdiff_t const twoStep = step << 1;
            med3(first, first + step, first + twoStep, pred);
            med3(mid - step, mid, mid + step, pred);
            med3(last - twoStep, last - step, last, pred);
            med3(first + step, mid, last - step, pred);
        } else {
            med3(first, mid, last, pred);
        }
    }

    //splits into < pivot, == pivot, > pivot, returns the == range (_Partition_by_median_guess_unchecked)
    template <class It, class Pr>
    inline std::pair<It, It> partitionByPivot(It first, It pfirst, It last, Pr pred) {
        using std::swap;
        It plast = pfirst + 1;
        while (first < pfirst && !pred(*(pfirst - 1), *pfirst) && !pred(*pfirst, *(pfirst - 1))) --pfirst;
        while (plast < last && !pred(*plast, *pfirst) && !pred(*pfirst, *plast)) ++plast;
        It gfirst = plast;
        It glast = pfirst;
        for (;;) {
            for (; gfirst < last; ++gfirst) {
                if (pred(*pfirst, *gfirst))
                    continue;
                else if (pred(*gfirst, *pfirst))
                    break;
                else if (plast != gfirst) {
                    swap(*plast, *gfirst);
                    ++plast;
                } else
                    ++plast;
            }
            for (; first < glast; --glast) {
                It const gprev = glast - 1;
                if (pred(*gprev, *pfirst))
                    continue;
                else if (pred(*pfirst, *gprev))
                    break;
                else if (--pfirst != gprev)
                    swap(*pfirst, *gprev);
            }
            if (glast == first && gfirst == last) return {pfirst, plast};
            if (glast == first) {
                if (plast != gfirst) swap(*pfirst, *plast);
                ++plast;
                swap(*pfirst, *gfirst);
                ++pfirst;
                ++gfirst;
            } else if (gfirst == last) {
                if (--glast != --pfirst) swap(*glast, *pfirst);
                swap(*pfirst, *--plast);
            } else {
                swap(*gfirst, *--glast);
                ++gfirst;
            }
        }
    }

    // moves val up the heap from hole, not above top (_Push_heap_by_index)
    template <class It, class T, class Pr>
    inline void pushHeapByIndex(It first, std::ptrdiff_t hole, std::ptrdiff_t top, T&& val, Pr pred) {
        for (std::ptrdiff_t idx = (hole - 1) >> 1; top < hole && pred(*(first + idx), val); idx = (hole - 1) >> 1) {
            *(first + hole) = std::move(*(first + idx));
            hole = idx;
        }
        *(first + hole) = std::forward<T>(val);
    }

    // moves the hole down to a leaf, then puts val back (_Pop_heap_hole_by_index)
    template <class It, class T, class Pr>
    inline void popHeapHoleByIndex(It first, std::ptrdiff_t hole, std::ptrdiff_t bottom, T&& val, Pr pred) {
        std::ptrdiff_t const top = hole;
        std::ptrdiff_t idx = hole;
        std::ptrdiff_t const maxNonLeaf = (bottom - 1) >> 1;
        while (idx < maxNonLeaf) {
            idx = 2 * idx + 2;
            if (pred(*(first + idx), *(first + (idx - 1)))) --idx;
            *(first + hole) = std::move(*(first + idx));
            hole = idx;
        }
        if (idx == maxNonLeaf && bottom % 2 == 0) {
            *(first + hole) = std::move(*(first + (bottom - 1)));
            hole = bottom - 1;
        }
        pushHeapByIndex(first, hole, top, std::forward<T>(val), pred);
    }

    //builds a heap on the range (_Make_heap_unchecked)
    template <class It, class Pr>
    inline void makeHeap(It first, It last, Pr pred) {
        std::ptrdiff_t const bottom = last - first;
        for (std::ptrdiff_t hole = bottom >> 1; hole > 0;) {
            --hole;
            auto val = std::move(*(first + hole));
            popHeapHoleByIndex(first, hole, bottom, std::move(val), pred);
        }
    }

    // sorts a heap (_Sort_heap_unchecked)
    template <class It, class Pr>
    inline void sortHeap(It first, It last, Pr pred) {
        for (; last - first >= 2; --last) {
            It l = last - 1;
            auto val = std::move(*l);
            *l = std::move(*first);
            popHeapHoleByIndex(first, 0, l - first, std::move(val), pred);
        }
    }

    // quicksort, heap sort when ideal runs out (_Sort_unchecked)
    template <class It, class Pr>
    inline void sortUnchecked(It first, It last, std::ptrdiff_t ideal, Pr pred) {
        for (;;) {
            if (last - first <= 32) {
                insertionSort(first, last, pred);
                return;
            }
            if (ideal <= 0) {
                makeHeap(first, last, pred);
                sortHeap(first, last, pred);
                return;
            }
            It const mid0 = first + ((last - first) >> 1);
            guessMedian(first, mid0, last - 1, pred);
            std::pair<It, It> const mid = partitionByPivot(first, mid0, last, pred);
            ideal = (ideal >> 1) + (ideal >> 2);
            if (mid.first - first < last - mid.second) {
                sortUnchecked(first, mid.first, ideal, pred);
                first = mid.second;
            } else {
                sortUnchecked(mid.second, last, ideal, pred);
                last = mid.first;
            }
        }
    }

    // same as msvc std::sort with a less than predicate
    template <class It, class Pr>
    inline void sort(It first, It last, Pr pred) {
        sortUnchecked(first, last, last - first, pred);
    }

}
