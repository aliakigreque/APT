// Copies a PlayerObject to a PlayerState and back, for snapshots and resets.

#include "player_state.hpp"

using namespace geode::prelude;

namespace ap {

    // copies every field of the player (the inl uses s and p)
    PlayerState PlayerState::save(PlayerObject* p) {
        PlayerState s;
        if (!p) return s;

#define AP_PS_SAVE
#include "player_state.inl"
#undef AP_PS_SAVE

        s.m_rotateObjectsRelated = p->m_rotateObjectsRelated;
        s.m_playerFollowFloats = p->m_playerFollowFloats;
        s.m_ringRelatedSet = p->m_ringRelatedSet;
        s.m_touchedRings = p->m_touchedRings;
        s.touchingRings.clear();
        if (p->m_touchingRings) {
            for (auto* obj : CCArrayExt<GameObject*>(p->m_touchingRings)) {
                s.touchingRings.push_back(obj);
            }
        }

        s.position = p->getPosition();
        s.rotation = p->getRotation();
        s.scaleX = p->getScaleX();
        s.scaleY = p->getScaleY();
        s.valid = true;
        return s;
    }

    // puts the saved fields back on the player and revives it
    void PlayerState::apply(PlayerObject* p) const {
        if (!p || !valid) return;
        auto const& s = *this;

#define AP_PS_APPLY
#include "player_state.inl"
#undef AP_PS_APPLY

        p->m_rotateObjectsRelated = s.m_rotateObjectsRelated;
        p->m_playerFollowFloats = s.m_playerFollowFloats;
        p->m_ringRelatedSet = s.m_ringRelatedSet;
        p->m_touchedRings = s.m_touchedRings;
        if (p->m_touchingRings) {
            p->m_touchingRings->removeAllObjects();
            for (auto* obj : s.touchingRings) p->m_touchingRings->addObject(obj);
        }

        p->setPosition(s.position);
        p->setRotation(s.rotation);
        if (p->getScaleX() != s.scaleX) p->setScaleX(s.scaleX);
        if (p->getScaleY() != s.scaleY) p->setScaleY(s.scaleY);
        p->m_isDead = false;
    }

}
