// Timeline: recorded states of moving objects, tick by tick.

#pragma once
#include <cstdint>
#include <vector>

namespace ap::sim {
    struct World;

    struct TimelineKey {
        int tick = 0;
        float x = 0;
        float y = 0;
        float nx = 0;
        float ny = 0;
        float rot = 0;
        uint8_t flags = 0;
    };

    struct TimelineEvent {
        int tick = 0;
        int uid = 0;
        float x = 0;
        float y = 0;
        float rot = 0;
        float nx = 0;
        float ny = 0;
        uint8_t flags = 0;
    };

    struct CamKey {
        int tick = 0;
        float zoom = 1.f;
        float offsetY = 0.f;
        float timeWarp = 1.f;
        bool zoomChanging = false;
        bool staticMode = false;
        float minPortalY = 0;
        float maxPortalY = 0;
    };

    struct ObjTimeline {
        int objIndex = -1;
        std::vector<TimelineKey> keys;
        float minX = 0;
        float maxX = 0;
        bool followsPlayerX = false;
    };

    struct Timeline {
        std::vector<ObjTimeline> objs;
        std::vector<int> dynOf;
        std::vector<std::vector<int>> byColumn;
        std::vector<CamKey> cam;
        std::vector<std::pair<int, float>> xTicks;
        int firstTick = 0;
        int lastTick = 0;
        int tickOffset = 1;

        // first recorded tick where the real player reached x, fallback if no positions
        int tickForX(float x, int fallback) const {
            if (xTicks.empty()) return fallback;
            if (x < xTicks.front().second) return xTicks.front().first;
            size_t lo = 0;
            size_t hi = xTicks.size() - 1;
            if (xTicks[hi].second < x - 0.01f) return xTicks[hi].first;
            while (lo < hi) {
                size_t const mid = (lo + hi) / 2;
                if (xTicks[mid].second < x - 0.01f)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            return xTicks[lo].first;
        }

        // camera key active at tick, nullptr before the first one
        CamKey const* camAt(int tick) const {
            if (cam.empty() || cam.front().tick > tick) return nullptr;
            size_t lo = 0;
            size_t hi = cam.size();
            while (hi - lo > 1) {
                size_t const mid = (lo + hi) / 2;
                if (cam[mid].tick <= tick)
                    lo = mid;
                else
                    hi = mid;
            }
            return &cam[lo];
        }

        void build(std::vector<TimelineEvent> const& events, World const& world);

        // state of moving object dyn at sim tick (last key <= tick + tickOffset)
        TimelineKey const& at(int dyn, int tick) const {
            auto const& k = objs[static_cast<size_t>(dyn)].keys;
            int const t = tick + tickOffset;
            size_t lo = 0;
            size_t hi = k.size();
            while (hi - lo > 1) {
                size_t const mid = (lo + hi) / 2;
                if (k[mid].tick <= t)
                    lo = mid;
                else
                    hi = mid;
            }
            return k[lo];
        }
    };
}
