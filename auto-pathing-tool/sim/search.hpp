// Declaration of the path search and its options.

#pragma once
#include "gd.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ap::sim {

    struct SearchOptions {
        int beamWidth = 150;
        int maxBeamWidth = 40000;
        int horizonTicks = 0;
        int minPressGap = 8;
        float quantY = 2.f;
        float quantV = 0.5f;
        bool preferFewInputs = true;
        int pressCost = 4;
        float fragileGap = 2.f;
        int fragileCost = 1;
        float fragileObbGap = 6.f;
        int fragileObbCost = 4;
        int rotAmbCost = 200;
        int missedSpeedCost = 400;
        int trapTouchCost = 5000;
        int persistChangeCost = 4000;
        bool menuGoal = false;
        int missedOrbCost = 12;
        int missedPortalCost = 200;
        float waveExitReach = 400.f;
        int flyExitCap = 0;
        bool flyExitTries = true;
        int flyMissKill = 1;
        bool flyMissWaveOnly = true;
        float waveMissReach = 160.f;
        bool outsideMiss = false;
        float flyExitStep = 45.f;
        float flyExitAhead = 480.f;
        bool portalDebt = true;
        float tightGap = 4.f;
        int tightCost = 0;
        float tightGap2 = 1.5f;
        int tightCost2 = 0;
        int lateSpeedCost = 90;
        float lateSpeedGap = 6.f;
        int bandCost = 1;
        int boundCost = 1;
        int extentCost = 2;
        float extentMargin = 60.f;
        int emptyCost = 1;
        int awayKill = 120;
        int awayCost = 3;
        float awayRx = 300.f;
        float awayRy = 300.f;
        bool revisitKill = true;
        std::vector<std::array<float, 3>> priorPath;
        std::vector<int> mustTouch;
        bool pruneVoid = true;
        bool learnTouch = true;
        bool autoTouch = true;
        int robustnessLookahead = 0;
        int threads = 0;
        double timeLimit = 0;
        double stallSeconds = 0;
        long maxTicks = 0;
        int commitInterval = 240;
        int commitLag = 2400;
        int softInterval = 240;
        int maxSoft = 14;
        std::atomic<int> const* playedTicks = nullptr;
        std::atomic<int> const* waitFor = nullptr;
        int minLead = 480;
        int minLeadLow = 48;
        int maxLead = 0;
        int leadResume = 1200;
        std::atomic<bool> const* starving = nullptr;
        int stuckKill = 960;
        float stuckMargin = 30.f;
        int escapeKill = 140;
        bool escapeRoof = false;
        int escapeCost = 0;
        int escapeGrace = 60;
        int escapeRoofCost = 3;
        int adaptBeamK = 4;
        int adaptBeamMin = 40;
        bool speedSepGround = true;
        int slowMoCost = 0;
        bool floorReserve = false;
        std::function<void(std::vector<uint8_t> const& prefix)> onPrefix;
        std::function<void()> threadInit;
    };

    struct SearchStats {
        long ticksSimulated = 0;
        int maxLayer = 0;
        int widenings = 0;
        int softRestarts = 0;
        int hardPops = 0;
        double seconds = 0;
        double expandSeconds = 0;
        double mergeSeconds = 0;
        double pausedSeconds = 0;
        long awayKills = 0;
        long revisitKills = 0;
        long stuckKills = 0;
        long escapeKills = 0;
        int debtRungs = 0;
        long adaptTicks = 0;
        long adaptNarrow = 0;
        int debtAccepted = 0;
        long debtTicks = 0;
    };

    struct SearchResult {
        bool success = false;
        int startTick = 0;
        std::vector<uint8_t> inputs;
        int endTick = 0;
        int presses = 0;
        std::string why;
        SearchStats stats;
        bool fromStart = false;
        bool menuDone = false;
        std::vector<std::pair<int, int>> persistAfter;
    };

    bool menuViolation(Game const& g);
    bool menuGoalReached(Game const& g);
    bool menuWanted(World const& w);
    std::vector<std::pair<int, int>> persistAfterOf(Game const& g);

    SearchResult searchPath(Game const& start, SearchOptions const& opt, std::atomic<bool> const* cancel = nullptr,
                            std::function<void(int, int)> const& progress = {});

}
