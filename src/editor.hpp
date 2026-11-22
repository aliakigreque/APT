// Declaration of the F3 path editor.

#pragma once

#include "engine.hpp"

#include <Geode/Geode.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ap {

    class Editor {
    public:
        static Editor& get();

        // true while the path editor is open
        bool active() const { return m_active; }
        void toggle();

        void onLevelLoaded(PlayLayer* pl);
        void onLayerGone();
        void onNewAttempt();
        void beforeFrame(PlayLayer* pl);

        void frame(PlayLayer* pl, float dt);
        void injectInput(GJBaseGameLayer* layer, int step);
        bool onKey(cocos2d::enumKeyCodes key, bool down);

    private:
        enum class Source { Live, Playback };

        void start(PlayLayer* pl);
        void resume();
        bool stepForward();
        void goTo(int tick);
        void saveState();
        void resumeMusic(PlayLayer* pl);
        void buildHud(PlayLayer* pl);
        void updateHud();
        void readQueuedJump(PlayLayer* pl);
        uint8_t inputAt(int tick) const;
        void pollKeys();
        // jump buttons held now, from the game's queue and from the keyboard
        uint8_t held() const { return m_held | m_polledHeld; }

        bool m_active = false;
        bool m_musicPending = false;
        Source m_source = Source::Live;
        PlayLayer* m_layer = nullptr;

        std::vector<uint8_t> m_inputs;
        int m_cursor = 0;
        bool m_dead = false;
        int m_firstEdit = -1;

        std::map<int, std::shared_ptr<Snapshot>> m_states;
        int m_lastSaved = -1;
        uint8_t m_lastSent = 0;

        uint8_t m_held = 0;
        bool m_backHeld = false;
        bool m_forwardHeld = false;
        bool m_polling = false;
        static bool s_queueWorks;
        uint8_t m_polledHeld = 0;
        float m_holdTime = 0.f;
        float m_holdTicks = 0.f;
        int m_pending = 0;

        cocos2d::CCNode* m_hud = nullptr;
        cocos2d::CCLabelBMFont* m_title = nullptr;
        cocos2d::CCLabelBMFont* m_info = nullptr;
        cocos2d::CCDrawNode* m_timeline = nullptr;
    };

}
