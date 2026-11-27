// Declaration of the Engine, which plays the real game one physics step at a time.

#pragma once

#include "macro.hpp"
#include "player_state.hpp"

#include <Geode/Geode.hpp>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace ap {
    struct Snapshot {
        geode::Ref<CheckpointObject> cp;
        PlayerState p1;
        PlayerState p2;
        int step = 0;
        uint64_t rng = 0;
        uint64_t tpRng = 0;
        std::array<float, 2000> variance{};
        cocos2d::CCPoint p1pos{};
        cocos2d::CCPoint p2pos{};

        // true if this snapshot is the start of the level
        bool isStart() const { return !cp; }
    };

    class Engine {
    public:
        static Engine& get();

        void attach(PlayLayer* pl);
        void onLayerGone();

        // true while the engine drives the ticks (path editor)
        bool active() const { return m_active; }
        // true while the engine calls update itself
        bool inStep() const { return m_inStep; }
        // true during an internal reset (fullReset, restore)
        bool restoring() const { return m_restoring; }

        // steps played since the attempt started
        int stepCounter() const { return m_stepCounter; }
        // tick the next step will play
        int curStep() const { return m_curStep; }
        bool step();
        // true if the player died during the last step
        bool died() const { return m_died; }
        // true if the game refused to play the last step
        bool stalled() const { return m_stalled; }
        // true if the level was completed during the last step
        bool completed() const { return m_completed; }
        // player (1 or 2) that died during the last step
        // could be used to show what killed the player in the path editor, keep
        int deadPlayer() const { return m_deadPlayer; }
        // unique id of the object that killed the player could be used to show what killed the player in the path editor, keep
        int killerId() const { return m_killerId; }
        // object id of the object that killed the player could be used to show what killed the player in the path editor, keep
        int killerObjectId() const { return m_killerObjectId; }
        // position of the player when it died could be used to show where the player died in the path editor, keep
        cocos2d::CCPoint deathPos() const { return m_deathPos; }

        // clears the death, completion and stall flags
        void clearFlags() {
            m_died = m_completed = m_stalled = false;
            m_killerId = -1;
            m_killerObjectId = 0;
        }

        void setScript(std::vector<Toggle> const* script);
        void resyncScript();

        std::shared_ptr<Snapshot> takeSnapshot();
        bool restore(Snapshot const& snapshot);
        void fullReset();

        void beginEdit(PlayLayer* pl);
        void endEdit();

        void startPlayback(Macro const& macro);
        void replacePlayback(Macro const& macro);
        void stopPlayback();
        // true while a macro is being played
        bool playing() const { return m_playing; }
        // number of presses in the macro being played
        size_t playMacroInputs() const { return m_playMacro.inputCount(); }
        // macro being played
        Macro const& playbackMacro() const { return m_playMacro; }

        bool onDestroyPlayer(PlayLayer* pl, PlayerObject* player, GameObject* object);
        bool onLevelComplete(PlayLayer* pl);
        void onProcessCommands(GJBaseGameLayer* layer, bool isHalfTick);
        void onProcessQueuedButtons(GJBaseGameLayer* layer);
        void onLoadFromCheckpoint(PlayLayer* pl);
        void onPreResetLevel(PlayLayer* pl);
        void onResetLevel(PlayLayer* pl);
        // true when music, respawn circle and attempt label should be skipped
        bool suppressCosmetics() const { return m_restoring || m_active; }

        static constexpr uint64_t kSeed = 0x4150494C4F54ull;
        static uint64_t rng();
        static void seedRng(uint64_t seed);
        static constexpr uint64_t kTpSeed = 0x54454C45504Full;
        static uint64_t tpRng();
        static void seedTpRng(uint64_t seed);
        static uint64_t resetSeed(PlayLayer* pl);
        static void alignRngAfterReset(PlayLayer* pl);
        static uint64_t attemptSeed(PlayLayer* pl);
        static inline bool s_rngAligned = false;
        static inline int s_simResetDraws = -1;
        void checkResetSteps(PlayLayer* pl, int stepsBefore);
        void captureFresh(PlayLayer* pl);
        void applyFresh(PlayLayer* pl);

    private:
        void applyRestoreFixes();
        void refresh();
        void dismissPause(PlayLayer* pl);
        void saveFlags(PlayLayer* pl);
        void restoreFlags(PlayLayer* pl);

        PlayLayer* m_layer = nullptr;
        bool m_active = false;
        bool m_inStep = false;
        bool m_editing = false;
        bool m_restoring = false;
        Snapshot const* m_restoreSnap = nullptr;
        bool m_loadHookRan = false;
        bool m_forcePractice = false;

        int m_stepCounter = 0;
        int m_curStep = 0;

        bool m_died = false;
        bool m_completed = false;
        bool m_stalled = false;
        int m_deadPlayer = 1;
        int m_killerId = -1;
        int m_killerObjectId = 0;
        cocos2d::CCPoint m_deathPos{};

        std::vector<Toggle> const* m_script = nullptr;
        size_t m_scriptIdx = 0;

        bool m_playing = false;
        bool m_pendingReset = false;
        bool m_redoReset = false;
        int m_redoCount = 0;
        Macro m_playMacro;

        PlayerState m_fresh1;
        PlayerState m_fresh2;
        bool m_haveFresh = false;
        float m_freshMaxPortalY = 0.f;
        bool m_freshGridSnapOff = false;
        std::vector<GameObject*> m_freshNoOuterOb;

        int m_savedAttempts = 0;
        bool m_savedCbs = false;
        bool m_savedCos = false;
        bool m_savedPractice = false;
        bool m_flagsSaved = false;
    };
}
