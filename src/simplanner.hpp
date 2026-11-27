// Declaration of the SimPlanner, which runs the search and plays its path live.

#pragma once
#include "../sim/gd.hpp"
#include "../sim/search.hpp"
#include "engine.hpp"
#include "macro.hpp"

#include <Geode/Geode.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ap {

    class SimPlanner {
    public:
        enum class Phase { Idle, OfflineSearch, OfflineDone, OfflineFailed, Live };

        static SimPlanner& get();

        void onLevelLoaded(PlayLayer* pl);
        void onLayerGone();

        bool startOffline(PlayLayer* pl);
        bool startLive(PlayLayer* pl);
        std::vector<uint8_t> liveInputs() const;
        void resumeFromEdit(std::vector<uint8_t> const& prefix, int firstChange, int played = -1);
        void setMacroFromInputs(std::vector<uint8_t> const& inputs);
        void stop();

        // current phase (idle, offline search, live...)
        Phase phase() const { return m_phase; }
        // true while a search runs, offline or live
        bool running() const { return m_phase == Phase::OfflineSearch || m_phase == Phase::Live; }
        bool frozen() const;
        int liveStepBudget() const;
        // true if the game must play fixed packs of steps (no simulated world)
        bool fixedCadence() const { return !m_world; }

        void frame(PlayLayer* pl);
        void onRealReset(PlayLayer* pl);
        void onPreRealReset(PlayLayer* pl);
        void readAttemptContext(PlayLayer* pl);
        void onRealTick(PlayLayer* pl, int step);
        void onQueuedButtons(GJBaseGameLayer* layer, int step);
        void onRealDeath(PlayLayer* pl, int step, GameObject* killer);
        void onRealComplete(PlayLayer* pl, int step);

        std::string statusText() const;
        // last macro found
        Macro const& macro() const { return m_macro; }
        // true once the simulated world of the level is built
        bool hasWorld() const { return m_hasWorld; }
        std::vector<cocos2d::CCPoint> plannedPath(int fromTick, int toTick) const;
        // number of plan ticks the game has played
        int liveStep() const { return m_consumed.load(); }
        // version of the planned path, changes when the path changes
        unsigned pathVersion() const { return m_pathVersion.load(); }
        void frameTiming(double frameSec, double gameSec, double modSec, int steps);
        bool forceOptimizedMoves(int step) const;

    private:
        // default planner, use get()
        SimPlanner() = default;
        void joinThread();
        void offlineThread();
        void liveThread();
        sim::Game freshGame() const;
        bool startMenuAttempt(sim::SearchOptions const& options, std::function<void(int, int)> const& progress);
        void diagnose();
        void setStatus(std::string text);

        PlayLayer* m_layer = nullptr;
        std::shared_ptr<sim::World> m_world;
        int m_liveAttempts = -1;
        bool m_persistFrozen = false;
        bool m_hasWorld = false;
        float m_startX = 0;
        float m_startY = 105.f;
        bool m_haveStart = false;
        sim::P2Leftover m_p2l;
        static void readLeft(PlayerObject* player, sim::PlayerLeftover& leftover);
        sim::PlayerLeftover m_p1l;
        mutable std::mutex m_startMx;

        std::atomic<Phase> m_phase{Phase::Idle};

        std::thread m_thread;
        std::atomic<bool> m_cancel{false};
        std::atomic<bool> m_threadDone{false};

        sim::SearchResult m_offlineResult;
        bool m_offlineConsumed = false;
        Macro m_macro;
        int m_macroTicks = 0;

        mutable std::mutex m_mx;
        std::vector<uint8_t> m_plan;
        std::vector<uint8_t> m_executed;
        std::vector<uint8_t> m_committed;
        std::atomic<bool> m_searchComplete{false};
        std::vector<cocos2d::CCPoint> m_path;
        std::vector<cocos2d::CCPoint> m_frozenPath;
        std::atomic<bool> m_pathFrozen{false};
        std::atomic<unsigned> m_pathVersion{0};
        std::atomic<int> m_consumed{0};
        std::atomic<int> m_planLen{0};
        std::atomic<int> m_generation{0};
        mutable std::chrono::steady_clock::time_point m_frozenSince{};
        mutable double m_frozenFor = 0;
        mutable std::atomic<bool> m_starving{false};
        std::atomic<bool> m_holding{false};
        mutable int m_holdSeenTick = -1;
        mutable std::chrono::steady_clock::time_point m_holdSeenAt{};
        std::chrono::steady_clock::time_point m_attemptStart{};
        mutable int m_frozenGen = -1;
        mutable std::atomic<int> m_giveUpGen{-1};
        std::atomic<int> m_lostAt{-1};
        std::atomic<bool> m_doomed{false};
        std::vector<uint8_t> m_fixedPrefix;
        std::shared_ptr<sim::Game> m_fixedStart;
        std::vector<int> m_resyncTicks;
        int m_lastDivStep = -10;
        float m_lastDivDx = 0.f;
        void resyncFromReal(sim::Game corrected);
        std::atomic<bool> m_restartSearch{false};
        std::atomic<int> m_planEpoch{0};
        mutable std::chrono::steady_clock::time_point m_editHoldSince{};
        bool m_deadEndExported = false;
        std::atomic<bool> m_deadEnd{false};
        std::atomic<bool> m_abandon{false};
        std::atomic<bool> m_stuckReset{false};
        uint8_t m_lastSent = 0;

        struct RealSample {
            int step;
            float x;
            float y;
            double yv;
            int held;
            uint32_t rng;
            double snap;
        };
        std::vector<RealSample> m_real;
        std::vector<RealSample> m_prevReal;
        std::string m_persistWarned;

        bool m_menuTried = false;
        std::atomic<bool> m_menuPending{false};
        std::atomic<int> m_menuGen{-1};
        std::atomic<bool> m_adoptPersist{false};
        std::vector<std::pair<int, int>> m_menuPersist;
        std::vector<std::pair<int, int>> m_newPersist;

        std::vector<uint8_t> m_prevExecuted;
        std::atomic<int> m_divergences{0};
        bool m_rngReported = false;
        std::atomic<int> m_frontierTick{0};
        std::atomic<int> m_liveThreads{0};

        struct FrameWin {
            double wall = 0;
            double game = 0;
            double mod = 0;
            double worst = 0;
            int frames = 0;
            int steps = 0;
            std::chrono::steady_clock::time_point lastLog{};
        } m_fw;
        double m_lastSearchSeconds = 0;
        int m_lastLayer = 0;
        std::atomic<bool> m_searchCancel{false};

        mutable std::mutex m_pathMx;
        sim::Game m_pathGame;
        std::vector<uint8_t> m_pathInputs;
        std::vector<std::pair<size_t, sim::Game>> m_pathCk;
        std::shared_ptr<sim::Game> m_pathFixed;
        static constexpr size_t kPathCk = 2400;
        bool m_optSync = false;
        std::vector<short> m_optGroups;
        std::vector<uint8_t> m_optMove;
        std::vector<std::pair<double, double>> m_optLast;
        void optSnap(sim::Game const& g, std::vector<std::pair<double, double>>& out) const;
        void resetPath();
        void updatePath(std::vector<uint8_t> const& inputs);
        void publishPlan(std::vector<uint8_t> const& inputs, bool definitive);

        std::thread m_obsThread;
        std::mutex m_obsMx;
        std::condition_variable m_obsCv;
        std::vector<uint8_t> m_obsInputs;
        bool m_obsNew = false;
        bool m_obsQuit = false;
        sim::Game m_mirror;
        void startObserver();
        void stopObserver();
        void observerThread();
        void verifyMirror();

        bool m_flagsSaved = false;
        bool m_savedCbs = false;
        bool m_savedCos = false;
        void saveFlags(PlayLayer* pl);
        void restoreFlags(PlayLayer* pl);
        void exportForDebug(PlayLayer* pl, char const* why);

        mutable std::mutex m_statusMx;
        std::string m_status;
    };

}
