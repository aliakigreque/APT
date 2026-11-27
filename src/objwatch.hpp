// Declaration of the object watcher used by the recorder.

#pragma once
#include "../sim/timeline.hpp"

#include <Geode/Geode.hpp>

#include <vector>

namespace ap {

    class ObjectWatcher {
    public:
        void reset(PlayLayer* pl);
        size_t sample(int tick, std::vector<sim::TimelineEvent>& out, bool all = false, cocos2d::CCPoint center = {}, float window = 0.f);
        // number of watched objects
        size_t candidates() const { return m_c.size(); }

    private:
        struct Cand {
            GameObject* obj = nullptr;
            int uid = 0;
            float x = 0;
            float y = 0;
            float rot = 0;
            float nx = 0;
            float ny = 0;
            uint8_t flags = 0;
            bool seen = false;
        };
        // packs the disabled and hidden states in one byte, same bits as sim::TimelineEvent::flags
        static uint8_t flagsOf(GameObject* object) {
            uint8_t flags = 0;
            if (object->m_isGroupDisabled) flags |= 1;
            if (object->m_isDisabled) flags |= 2;
            if (object->m_isHide) flags |= 4;
            return flags;
        }
        std::vector<Cand> m_c;
    };

}
