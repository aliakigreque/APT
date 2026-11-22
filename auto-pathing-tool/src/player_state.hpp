// PlayerState: a copy of a PlayerObject that can be put back on the player.

#pragma once

#include <Geode/Geode.hpp>

namespace ap {

    struct PlayerState {
#define AP_PS_DECLARE
#include "player_state.inl"
#undef AP_PS_DECLARE

        gd::unordered_map<int, GJPointDouble> m_rotateObjectsRelated{};
        gd::vector<float> m_playerFollowFloats{};
        gd::unordered_set<int> m_ringRelatedSet{};
        gd::unordered_set<int> m_touchedRings{};
        std::vector<GameObject*> touchingRings{};

        cocos2d::CCPoint position{};
        float rotation = 0.f;
        float scaleX = 1.f;
        float scaleY = 1.f;
        bool valid = false;

        static PlayerState save(PlayerObject* p);
        void apply(PlayerObject* p) const;
    };

}
