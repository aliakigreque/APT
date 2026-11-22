// Builds the timeline of moving objects from a recording, used when triggers are not simulated.

#include "timeline.hpp"
#include "world.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

namespace ap::sim {

    // groups the events by object into keys, then finds player-locked objects and fills the section columns
    void Timeline::build(std::vector<TimelineEvent> const& events, World const& world) {
        objs.clear();
        std::stable_sort(cam.begin(), cam.end(), [](CamKey const& a, CamKey const& b) { return a.tick < b.tick; });
        std::stable_sort(xTicks.begin(), xTicks.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        for (size_t i = 1; i < xTicks.size(); ++i) xTicks[i].second = std::max(xTicks[i].second, xTicks[i - 1].second);
        dynOf.assign(world.objs.size(), -1);
        byColumn.clear();
        firstTick = INT_MAX;
        lastTick = INT_MIN;

        std::vector<TimelineEvent> sortedEvents = events;
        std::stable_sort(sortedEvents.begin(), sortedEvents.end(), [](TimelineEvent const& a, TimelineEvent const& b) {
            if (a.uid != b.uid) return a.uid < b.uid;
            return a.tick < b.tick;
        });
        for (size_t i = 0; i < sortedEvents.size();) {
            size_t j = i;
            while (j < sortedEvents.size() && sortedEvents[j].uid == sortedEvents[i].uid) ++j;
            int const idx = world.indexOfUid(sortedEvents[i].uid);
            if (idx >= 0 && world.objs[static_cast<size_t>(idx)].type != Decoration) {
                Obj const& o = world.objs[static_cast<size_t>(idx)];
                ObjTimeline objTimeline;
                objTimeline.objIndex = idx;
                TimelineKey startKey;
                startKey.tick = INT_MIN;
                startKey.x = static_cast<float>(o.posX);
                startKey.y = static_cast<float>(o.posY);
                startKey.rot = o.rot;
                startKey.nx = startKey.x;
                startKey.ny = startKey.y;
                startKey.flags =
                        static_cast<uint8_t>((o.groupDisabled ? 1 : 0) | (o.disabled ? 2 : 0) | (o.hide ? 4 : 0));
                objTimeline.keys.push_back(startKey);
                for (size_t e = i; e < j; ++e) {
                    TimelineKey k;
                    k.tick = sortedEvents[e].tick;
                    k.x = sortedEvents[e].x;
                    k.y = sortedEvents[e].y;
                    k.rot = sortedEvents[e].rot;
                    k.flags = sortedEvents[e].flags;
                    k.nx = sortedEvents[e].nx;
                    k.ny = sortedEvents[e].ny;
                    if (!objTimeline.keys.empty() && objTimeline.keys.back().tick == k.tick)
                        objTimeline.keys.back() = k;
                    else
                        objTimeline.keys.push_back(k);
                    firstTick = std::min(firstTick, k.tick);
                    lastTick = std::max(lastTick, k.tick);
                }
                objTimeline.minX = objTimeline.maxX = static_cast<float>(o.posX);
                for (auto const& k : objTimeline.keys) {
                    objTimeline.minX = std::min(objTimeline.minX, k.x);
                    objTimeline.maxX = std::max(objTimeline.maxX, k.x);
                }
                dynOf[static_cast<size_t>(idx)] = static_cast<int>(objs.size());
                objs.push_back(std::move(objTimeline));
            }
            i = j;
        }
        if (firstTick == INT_MAX) {
            firstTick = 0;
            lastTick = 0;
        }

        if (!xTicks.empty()) {
            auto xAt = [&](int tick, float& out) {
                auto it = std::lower_bound(xTicks.begin(), xTicks.end(), std::make_pair(tick, -1e30f));
                if (it == xTicks.end() || it->first != tick) return false;
                out = it->second;
                return true;
            };
            for (auto& objTimeline : objs) {
                int moving = 0;
                int same = 0;
                for (size_t k = 1; k < objTimeline.keys.size(); ++k) {
                    auto const& a = objTimeline.keys[k - 1];
                    auto const& b = objTimeline.keys[k];
                    if (b.tick != a.tick + 1 || b.x == a.x) continue;
                    float playerXa;
                    float playerXb;
                    if (!xAt(a.tick, playerXa) || !xAt(b.tick, playerXb)) continue;
                    ++moving;
                    if (std::fabs((b.x - a.x) - (playerXb - playerXa)) < 0.01f) ++same;
                }
                objTimeline.followsPlayerX = moving >= 8 && same * 10 >= moving * 6;
            }
        }

        for (size_t d = 0; d < objs.size(); ++d) {
            Obj const& o = world.objs[static_cast<size_t>(objs[d].objIndex)];
            float const half = std::max(o.rect.w, o.rect.h) * 0.5f + 1.f;
            int const firstColumn = World::cellOf(static_cast<double>(objs[d].minX - half));
            int const lastColumn = World::cellOf(static_cast<double>(objs[d].maxX + half));
            if (byColumn.size() <= static_cast<size_t>(lastColumn))
                byColumn.resize(static_cast<size_t>(lastColumn) + 1);
            for (int c = firstColumn; c <= lastColumn; ++c)
                byColumn[static_cast<size_t>(c)].push_back(static_cast<int>(d));
        }
    }

}
