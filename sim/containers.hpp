// Small containers used by the simulator: fixed vector, sorted flat map and flat set.

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace ap::sim {

    template <typename T, int N>
    struct SmallVec {
        T data[N]{};
        int n = 0;

        // number of items
        int size() const { return n; }
        // true if there is no item
        bool empty() const { return n == 0; }

        // removes every item, the free slots stay zeroed
        void clear() {
            for (int i = 0; i < n; ++i) data[i] = T{};
            n = 0;
        }

        // first item, for loops
        T* begin() { return data; }
        // end of the items, for loops
        T* end() { return data + n; }
        // first item, for loops (const)
        T const* begin() const { return data; }
        // end of the items, for loops (const)
        T const* end() const { return data + n; }
        // item at index i
        T& operator[](int i) { return data[i]; }
        // item at index i (const)
        T const& operator[](int i) const { return data[i]; }
        // last item
        T& back() { return data[n - 1]; }

        // adds an item at the end, ignored when full
        void push_back(T const& value) {
            if (n < N) data[n++] = value;
        }

        // true if the value is in the vector
        bool contains(T const& value) const {
            for (int i = 0; i < n; ++i)
                if (data[i] == value) return true;
            return false;
        }

        // adds value if missing, returns true if added
        bool insert(T const& value) {
            if (contains(value)) return false;
            push_back(value);
            return true;
        }

        // 1 if the value is in the vector, 0 otherwise (like std::set::count)
        size_t count(T const& value) const { return contains(value) ? 1 : 0; }

        // removes every copy of value, keeps the order
        void erase(T const& value) {
            int w = 0;
            for (int i = 0; i < n; ++i)
                if (!(data[i] == value)) data[w++] = data[i];
            for (int i = w; i < n; ++i) data[i] = T{};
            n = w;
        }

        // removes the item at i, keeps the order
        void eraseAt(int i) {
            for (int j = i; j + 1 < n; ++j) data[j] = data[j + 1];
            --n;
            data[n] = T{};
        }
    };

    template <typename K, typename V, int BW = 1>
    struct FlatMap {
        std::vector<std::pair<K, V>> items;

        struct Bloom {
            uint64_t w[BW] = {};

            // true if the hash may be in the map
            bool test(uint64_t hash) const { return (w[(hash >> 6) % BW] >> (hash & 63)) & 1u; }
            // marks the hash as present
            void set(uint64_t hash) { w[(hash >> 6) % BW] |= 1ull << (hash & 63); }

            // forgets every hash
            void clear() {
                for (auto& x : w) x = 0;
            }
        } bloom;

        // key as 64 bits, int keys or (int, int) pairs
        static uint64_t keyBits(K const& key) {
            if constexpr (std::is_integral_v<K>)
                return static_cast<uint64_t>(static_cast<int64_t>(key));
            else
                return (static_cast<uint64_t>(static_cast<uint32_t>(key.first)) << 32) ^
                       static_cast<uint64_t>(static_cast<uint32_t>(key.second));
        }

        // fibonacci hashing
        static uint64_t bloomHash(K const& key) { return (keyBits(key) * 0x9e3779b97f4a7c15ull) >> 40; }
        // false means the key is surely missing
        bool mayContain(K const& key) const { return bloom.test(bloomHash(key)); }

        // number of entries
        size_t size() const { return items.size(); }

        // removes every entry
        void clear() {
            items.clear();
            bloom.clear();
        }

        // first entry, for loops
        auto begin() { return items.begin(); }
        // end of the entries, for loops
        auto end() { return items.end(); }
        // first entry, for loops (const)
        auto begin() const { return items.begin(); }
        // end of the entries, for loops (const)
        auto end() const { return items.end(); }

        // first item not below key
        auto lower(K const& key) const {
            return std::lower_bound(items.begin(), items.end(), key,
                                    [](auto const& a, K const& b) { return a.first < b; });
        }

        // first entry with a key not below key (const)
        auto lower(K const& key) {
            return std::lower_bound(items.begin(), items.end(), key,
                                    [](auto const& a, K const& b) { return a.first < b; });
        }

        // value of key, nullptr if missing
        V const* find(K const& key) const {
            if (!mayContain(key)) return nullptr;
            auto it = lower(key);
            return (it != items.end() && it->first == key) ? &it->second : nullptr;
        }

        // value of key, nullptr if absent (const)
        V* find(K const& key) {
            if (!mayContain(key)) return nullptr;
            auto it = lower(key);
            return (it != items.end() && it->first == key) ? &it->second : nullptr;
        }

        // 1 if the key is in the map, 0 otherwise
        size_t count(K const& key) const { return find(key) ? 1 : 0; }

        // find for growing keys: gallops forward from the last position (hint)
        V* findHint(K const& key, size_t& hint) {
            size_t const n = items.size();
            if (n == 0) return nullptr;
            if (hint >= n || items[hint].first > key) hint = 0;
            size_t lo = hint;
            size_t step = 1;
            size_t hi = hint;
            while (hi < n && items[hi].first < key) {
                lo = hi;
                hi += step;
                step <<= 1;
            }
            if (hi > n) hi = n;
            auto it = std::lower_bound(items.begin() + static_cast<long>(lo), items.begin() + static_cast<long>(hi),
                                       key, [](auto const& a, K const& b) { return a.first < b; });
            hint = static_cast<size_t>(it - items.begin());
            return (it != items.end() && it->first == key) ? &it->second : nullptr;
        }

        // same as findHint but returns the index, size() if missing
        size_t indexHint(K const& key, size_t& hint) const {
            size_t const n = items.size();
            if (n == 0) return 0;
            if (hint >= n || items[hint].first > key) hint = 0;
            size_t lo = hint;
            size_t step = 1;
            size_t hi = hint;
            while (hi < n && items[hi].first < key) {
                lo = hi;
                hi += step;
                step <<= 1;
            }
            if (hi > n) hi = n;
            auto it = std::lower_bound(items.begin() + static_cast<long>(lo), items.begin() + static_cast<long>(hi),
                                       key, [](auto const& a, K const& b) { return a.first < b; });
            hint = static_cast<size_t>(it - items.begin());
            return (it != items.end() && it->first == key) ? hint : n;
        }

        // value of key, inserted with V{} if missing
        V& operator[](K const& key) {
            bloom.set(bloomHash(key));
            auto it = lower(key);
            if (it == items.end() || it->first != key) it = items.insert(it, std::make_pair(key, V{}));
            return it->second;
        }

        // the bloom bit stays set, it only means "maybe"
        void erase(K const& key) {
            auto it = lower(key);
            if (it != items.end() && it->first == key) items.erase(it);
        }
    };

    template <typename K>
    struct FlatSet {
        std::vector<K> items;

        // number of keys
        size_t size() const { return items.size(); }
        // removes every key
        void clear() { items.clear(); }

        // 1 if the key is in the set, 0 otherwise
        size_t count(K const& key) const {
            auto it = std::lower_bound(items.begin(), items.end(), key);
            return (it != items.end() && *it == key) ? 1 : 0;
        }

        // adds the key, keeps the set sorted
        void insert(K const& key) {
            auto it = std::lower_bound(items.begin(), items.end(), key);
            if (it == items.end() || *it != key) items.insert(it, key);
        }

        // removes the key if present
        void erase(K const& key) {
            auto it = std::lower_bound(items.begin(), items.end(), key);
            if (it != items.end() && *it == key) items.erase(it);
        }
    };

}
