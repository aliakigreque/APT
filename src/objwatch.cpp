// Watches the objects that triggers move, rotate or hide, tick by tick, for the recorder.

#include "objwatch.hpp"

#include <cmath>

using namespace geode::prelude;

namespace ap {
    // keeps the objects that triggers can change, with their starting state
    void ObjectWatcher::reset(PlayLayer* pl) {
        m_c.clear();
        if (!pl || !pl->m_objects) return;
        m_c.reserve(1024);
        for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (!obj || obj == pl->m_anticheatSpike) continue;
            if (obj->m_objectType == GameObjectType::Decoration) continue;
            if (!obj->m_groups || obj->m_groupCount <= 0) continue;
            Cand candidate;
            candidate.obj = obj;
            candidate.uid = obj->m_uniqueID;
            candidate.x = static_cast<float>(obj->m_positionX);
            candidate.y = static_cast<float>(obj->m_positionY);
            candidate.nx = obj->getPositionX();
            candidate.ny = obj->getPositionY();
            candidate.rot = obj->getObjectRotation();
            candidate.flags = flagsOf(obj);
            candidate.seen = true;
            m_c.push_back(candidate);
        }
    }

    // adds an event for every object whose position, rotation or flags changed
    size_t ObjectWatcher::sample(int tick, std::vector<sim::TimelineEvent>& out, bool all, cocos2d::CCPoint center,
                                 float window) {
        size_t count = 0;
        bool const windowed = window > 0.f && !all;
        for (auto& candidate : m_c) {
            GameObject* object = candidate.obj;
            float const x = static_cast<float>(object->m_positionX);
            float const y = static_cast<float>(object->m_positionY);
            if (windowed && (std::fabs(x - center.x) > window || std::fabs(y - center.y) > window)) continue;
            float const nodeX = object->getPositionX();
            float const nodeY = object->getPositionY();
            float const rot = object->getObjectRotation();
            uint8_t const flags = flagsOf(object);
            bool const changed = !candidate.seen || all || x != candidate.x || y != candidate.y ||
                                 rot != candidate.rot || flags != candidate.flags || nodeX != candidate.nx ||
                                 nodeY != candidate.ny;
            if (!changed) continue;
            candidate.x = x;
            candidate.y = y;
            candidate.nx = nodeX;
            candidate.ny = nodeY;
            candidate.rot = rot;
            candidate.flags = flags;
            candidate.seen = true;
            sim::TimelineEvent event;
            event.tick = tick;
            event.uid = candidate.uid;
            event.x = x;
            event.y = y;
            event.nx = nodeX;
            event.ny = nodeY;
            event.rot = rot;
            event.flags = flags;
            out.push_back(event);
            ++count;
        }
        return count;
    }
}
