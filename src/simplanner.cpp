// Live mode: builds the simulated world of the level, runs the path search in background threads, feeds the found inputs to the real game and handles divergences, deaths and dead ends.

#include "simplanner.hpp"

#include "log.hpp"
#include "settings.hpp"
#include "world_export.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>

using namespace geode::prelude;

namespace ap {

    namespace {
        constexpr double kLostWait = 120.0;

        // search threads run below the game's priority so the game keeps rendering
        void lowerThreadPriority() {
#ifdef GEODE_IS_WINDOWS
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        }

        // number of physical cores, 0 if unknown
        int physicalCores() {
#ifdef GEODE_IS_WINDOWS
            DWORD length = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
            if (length == 0) return 0;
            std::vector<uint8_t> buffer(length);
            auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
            if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) return 0;
            int cores = 0;
            for (DWORD offset = 0; offset + sizeof(DWORD) * 2 <= length;) {
                auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
                if (entry->Size == 0) break;
                if (entry->Relationship == RelationProcessorCore) ++cores;
                offset += entry->Size;
            }
            return cores;
#else
            return 0;
#endif
        }

        // number of search threads, live leaves more cores to the game
        int searchThreads(bool live) {
            int const cores = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
            if (!live) return std::max(1, cores - 1);
            if (Settings::get().fastSearch) return std::max(std::max(1, cores / 2), cores - 2);
            int const physical = physicalCores();
            if (physical <= 0) return std::max(1, cores / 2 - 1);
            return std::max(1, physical - 1);
        }
    }

    // true if game option "0010" swaps the players' buttons (2 player mode only)
    static bool flipTwoPlayer() {
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_levelSettings || !pl->m_levelSettings->m_twoPlayerMode) return false;
        return GameManager::sharedState()->getGameVariable("0010");
    }

    // turns one byte per tick (bit 0 player 1, bit 1 player 2) into press and release toggles
    static void appendToggles(Macro& macro, std::vector<uint8_t> const& inputs) {
        uint8_t previous = 0;
        for (size_t i = 0; i < inputs.size(); ++i) {
            uint8_t const buttons = inputs[i] & 3;
            if ((buttons ^ previous) & 1)
                macro.toggles.push_back(Toggle{static_cast<int>(i), (buttons & 1) != 0, false});
            if ((buttons ^ previous) & 2)
                macro.toggles.push_back(Toggle{static_cast<int>(i), (buttons & 2) != 0, true});
            previous = buttons;
        }
    }

    // the single planner
    SimPlanner& SimPlanner::get() {
        static SimPlanner p;
        return p;
    }

    // sets the status line, called from several threads
    void SimPlanner::setStatus(std::string text) {
        std::lock_guard<std::mutex> lock(m_statusMx);
        m_status = std::move(text);
    }

    // read by the overlay and the menu
    std::string SimPlanner::statusText() const {
        std::lock_guard<std::mutex> lock(m_statusMx);
        return m_status;
    }

    //true while the game has to wait for the search
    bool SimPlanner::frozen() const {
        if (m_phase == Phase::OfflineSearch) return true;
        if (m_phase == Phase::Live) {
            if (m_doomed.load()) return false;
            int const consumed = m_consumed.load();
            int const planLen = m_planLen.load();
            if (m_editHoldSince != std::chrono::steady_clock::time_point{}) {
                double const waited =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_editHoldSince).count();
                if (planLen >= consumed + 240 || m_searchComplete.load() || waited > 10.0)
                    m_editHoldSince = {};
                else
                    return true;
            }
            if (consumed == 0 && !m_searchComplete.load() &&
                m_attemptStart != std::chrono::steady_clock::time_point{}) {
                double const elapsed =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_attemptStart).count();
                if (elapsed < 30.0 && m_frontierTick.load() < sim::kStartLead) return true;
                int const lost = m_lostAt.load();
                if (lost >= 0 && elapsed < kLostWait && m_frontierTick.load() < lost + 480) return true;
            }
            bool const wait = planLen <= consumed + 1 && !m_searchComplete.load();
            if (!wait) {
                m_frozenSince = {};
                m_frozenFor = 0;
                m_starving = false;
                return false;
            }
            int const generation = m_generation.load();
            if (m_giveUpGen == generation) return false;
            auto const now = std::chrono::steady_clock::now();
            if (m_frozenSince != std::chrono::steady_clock::time_point{} && m_frozenGen == generation)
                m_frozenFor += std::min(0.1, std::chrono::duration<double>(now - m_frozenSince).count());
            else
                m_frozenFor = 0;
            m_frozenSince = now;
            m_frozenGen = generation;
            if (m_frozenFor > 0.75) m_starving = true;
            if (m_frontierTick.load() != m_holdSeenTick) {
                m_holdSeenTick = m_frontierTick.load();
                m_holdSeenAt = now;
            }
            bool const holdOn = m_holding.load() && std::chrono::duration<double>(now - m_holdSeenAt).count() < 30.0;
            double const limit = holdOn ? 300.0 : 4.0;
            if (m_frozenFor > limit) {
                m_giveUpGen = generation;
                m_frozenSince = {};
                m_frozenFor = 0;
                Log::printf(
                        "live: the search has not advanced for {} s at tick {} (plan published up to {}, search at "
                        "tick {}); the attempt goes on without a plan, the search goes on for the next one.",
                        static_cast<int>(limit), consumed, planLen, m_frontierTick.load());
                return false;
            }
            return true;
        }
        return false;
    }

    // ticks left in the published plan, no limit when there is no plan to follow
    int SimPlanner::liveStepBudget() const {
        if (m_phase != Phase::Live || m_doomed.load() || m_searchComplete.load())
            return std::numeric_limits<int>::max();
        if (m_giveUpGen == m_generation.load()) return std::numeric_limits<int>::max();
        return std::max(0, m_planLen.load() - m_consumed.load());
    }

    // logs where the frame time goes when live runs under 30 fps (at most every 5 s)
    void SimPlanner::frameTiming(double frameSec, double gameSec, double modSec, int steps) {
        if (m_phase != Phase::Live) {
            m_fw = FrameWin{};
            return;
        }
        if (!(frameSec > 0.0) || frameSec > 5.0) return;
        FrameWin& window = m_fw;
        window.wall += frameSec;
        window.game += gameSec;
        window.mod += modSec;
        window.worst = std::max(window.worst, frameSec);
        ++window.frames;
        window.steps += steps;
        if (window.wall < 1.0) return;
        double const fps = window.frames / window.wall;
        auto const now = std::chrono::steady_clock::now();
        if (fps < 30.0 && (window.lastLog == std::chrono::steady_clock::time_point{} || now - window.lastLog > std::chrono::seconds(5))) {
            int const consumed = m_consumed.load();
            int const planLen = m_planLen.load();
            int const frontier = m_frontierTick.load();
            Log::printf(
                    "live: {:.1f} fps (worst frame {:.0f} ms) at tick {}; per frame: game {:.1f} ms for {:.1f} steps "
                    "({:.2f} ms per step), mod {:.1f} ms, "
                    "rest (drawing, waits, other) {:.1f} ms; game speed {:.0f} %; lead: plan {:.1f} s, search {:.1f} "
                    "s; {} search threads",
                    fps, window.worst * 1000.0, consumed, window.game * 1000.0 / window.frames,
                    static_cast<double>(window.steps) / window.frames,
                    window.steps > 0 ? window.game * 1000.0 / window.steps : 0.0, window.mod * 1000.0 / window.frames,
                    std::max(0.0, (window.wall - window.game - window.mod) * 1000.0 / window.frames),
                    100.0 * window.steps / (240.0 * window.wall), (planLen - consumed) / 240.0,
                    (frontier - consumed) / 240.0, m_liveThreads.load());
            window.lastLog = now;
        }
        auto const lastLog = window.lastLog;
        window = FrameWin{};
        window.lastLog = lastLog;
    }

    // turns off click between steps and click on steps during live
    void SimPlanner::saveFlags(PlayLayer* pl) {
        if (m_flagsSaved || !pl) return;
        m_savedCbs = pl->m_clickBetweenSteps;
        m_savedCos = pl->m_clickOnSteps;
        pl->m_clickBetweenSteps = false;
        pl->m_clickOnSteps = false;
        m_flagsSaved = true;
    }

    // puts back what saveFlags changed
    void SimPlanner::restoreFlags(PlayLayer* pl) {
        if (!m_flagsSaved) return;
        m_flagsSaved = false;
        if (!pl || PlayLayer::get() != pl) return;
        pl->m_clickBetweenSteps = m_savedCbs;
        pl->m_clickOnSteps = m_savedCos;
    }

    // writes the world to levels/ so a failure can be reproduced with apsim
    void SimPlanner::exportForDebug(PlayLayer* pl, char const* why) {
        if (!pl || !m_world) return;
        auto dir = Mod::get()->getSaveDir() / "levels";
        std::string name = pl->m_level ? std::string(pl->m_level->m_levelName) : "level";
        int const id = pl->m_level ? static_cast<int>(pl->m_level->m_levelID.value()) : 0;
        for (auto& ch : name)
            if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
        std::string const tag = fmt::format("{}_{}", name, id);
        auto result = writeWorld(*m_world, pl, dir, tag);
        if (result)
            Log::printf("export ({}): {}", why, geode::utils::string::pathToString(result.unwrap()));
        else
            Log::printf("export ({}) failed: {}", why, result.unwrapErr());
    }

    // new simulated game at the start of the attempt
    sim::Game SimPlanner::freshGame() const {
        sim::Game g;
        g.world = m_world.get();
        g.twoPlayerMode = m_world->twoPlayer;
        g.disableCameraGridSnap = m_world->gridSnapOff;
        g.enable22Changes = m_world->enable22;
        sim::PlayerLeftover p1Leftover;
        sim::P2Leftover p2Leftover;
        {
            std::lock_guard<std::mutex> lock(m_startMx);
            g.startX = m_haveStart ? m_startX : m_world->startX;
            g.startY = m_haveStart ? m_startY : m_world->startY;
            p1Leftover = m_p1l;
            p2Leftover = m_p2l;
            g.reset();
        }
        p2Leftover.apply(g.p2);
        p1Leftover.apply(g.p1);
        return g;
    }

    // copies the player fields that resetLevel doesn't reset
    void SimPlanner::readLeft(PlayerObject* player, sim::PlayerLeftover& leftover) {
        leftover.valid = true;
        leftover.snapDistance = player->m_snapDistance;
        leftover.isOnGround3 = player->m_isOnGround3;
        leftover.maybeIsFalling = player->m_maybeIsFalling;
        leftover.padRingRelated = player->m_padRingRelated;
        leftover.fallStartY = player->m_fallStartY;
        leftover.hasExtra = true;
        leftover.touchedPad = player->m_touchedPad;
        leftover.accelerationOrSpeed = player->m_accelerationOrSpeed;
    }

    // builds the simulated world of the level and logs what it supports
    void SimPlanner::onLevelLoaded(PlayLayer* pl) {
        stop();
        m_layer = pl;
        m_hasWorld = false;
        if (!pl) return;
        auto start = std::chrono::steady_clock::now();
        m_world = std::make_shared<sim::World>(buildWorld(pl));
        size_t const kfOptGroups = m_world->kfOptGroups.size();
        m_optGroups = m_world->kfOptGroups;
        m_optSync = !m_optGroups.empty();
        m_world->useModFrames(sim::kModCadence);
        m_world->camUnk322a = false;
        m_world->startLastMaxPortalY = pl->m_gameState.m_unkUint17;
        m_world->tpRngKnown = true;
        m_world->tpRngSeed = Engine::kTpSeed;
        m_hasWorld = !m_world->objs.empty();
        {
            std::lock_guard<std::mutex> lock(m_startMx);
            m_haveStart = false;
            if (pl->m_player1 && !pl->m_isPracticeMode) {
                auto const pos = pl->m_player1->getPosition();
                if (pos.x >= -100.f && pos.x <= m_world->length && pos.y >= 0.f &&
                    pos.y <= std::max(3000.f, m_world->maxGameplayY + 300.f)) {
                    m_startX = pos.x;
                    m_startY = pos.y;
                    m_haveStart = true;
                }
            }
            m_p1l = {};
            m_p2l = {};
            if (pl->m_player1) readLeft(pl->m_player1, m_p1l);
            if (pl->m_player2) readLeft(pl->m_player2, m_p2l);
        }
        int draws = -1;
        if (m_hasWorld) std::thread([this, &draws] { draws = freshGame().resetDraws(); }).join();
        Engine::s_simResetDraws = draws;
        if (pl->m_player1) {
            auto const pos = pl->m_player1->getPosition();
            std::lock_guard<std::mutex> lock(m_startMx);
            m_startX = pos.x;
            m_startY = pos.y;
        }
        double const buildMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        Log::printf(
                "simulator: world built, {} objects, {:.1f} ms (length {:.0f}, start {:.1f},{:.1f}, maxY {:.0f}, start "
                "spike {}, y grid {})",
                m_world->objs.size(), buildMs, m_world->length, m_world->startX, m_world->startY, m_world->maxGameplayY,
                pl->m_anticheatSpike ? "ignored, as in the game" : "none", m_world->ySection ? "yes" : "no");
        {
            size_t triggerCount = 0;
            size_t positionalCount = 0;
            for (auto const& obj : m_world->objs)
                if (obj.cls == 1 && obj.trigger) ++triggerCount;
            for (auto const& entry : m_world->spawnLists) positionalCount += entry.second.size();
            Log::printf("triggers: simulated ({} triggers, {} positional, {} groups, {} dynamic collision blocks)",
                        triggerCount, positionalCount, m_world->maxGroup, m_world->dynamicBlocks.size());
            for (auto const& text : m_world->unsupported) Log::printf("triggers: not simulated (ignored) - {}", text);
            for (auto const& text : m_world->cameraUnsupported)
                Log::printf("triggers: camera not simulated (ignored) - {}", text);
            for (auto const& text : m_world->approximations) Log::printf("triggers: approximation - {}", text);
            if (kfOptGroups) {
                Log::printf(
                        "\"optimized\" moving keyframes ({} groups): moved every step (all frames \"visible\" while "
                        "the mod plays)",
                        kfOptGroups);
            }
            for (auto const& menu : m_world->menus) {
                std::string optionsText;
                for (auto const& op : menu.options) {
                    optionsText += fmt::format(
                            " [{} -> {}{}]", op.label, op.value,
                            (menu.hasPrefer && op.value == menu.prefer) ? ", chosen"
                            : std::find(menu.forbid.begin(), menu.forbid.end(), op.value) != menu.forbid.end()
                                    ? ", avoided"
                                    : "");
                }
                Log::printf("settings menu: item {} ({}):{}", menu.item, menu.kind, optionsText);
            }
            if (!m_world->ldmTouch.empty()) {
                Log::printf(
                        "low detail: {} object(s) tagged \"ldm\" (orb, touch trigger, pickup), never touched by the "
                        "bot",
                        m_world->ldmTouch.size());
            }
        }
    }

    // level left: stop everything and drop the world
    void SimPlanner::onLayerGone() {
        Engine::s_simResetDraws = -1;
        stop();
        m_layer = nullptr;
        m_hasWorld = false;
        m_world.reset();
    }

    // cancels the search thread and waits for it
    void SimPlanner::joinThread() {
        m_cancel = true;
        if (m_thread.joinable()) m_thread.join();
        m_cancel = false;
    }

    // inputs of the current attempt: what was played, then the rest of the plan
    std::vector<uint8_t> SimPlanner::liveInputs() const {
        if (m_phase != Phase::Live) return {};
        std::lock_guard<std::mutex> lock(m_mx);
        std::vector<uint8_t> inputs = m_executed;
        if (m_plan.size() > inputs.size())
            inputs.insert(inputs.end(), m_plan.begin() + static_cast<long>(inputs.size()), m_plan.end());
        return inputs;
    }

    // the edited inputs are already played (the game stands at their end): fix them and search from there
    void SimPlanner::resumeFromEdit(std::vector<uint8_t> const& prefix, int firstChange, int played) {
        if (m_phase != Phase::Live) return;
        if (played < 0 || played > static_cast<int>(prefix.size())) played = static_cast<int>(prefix.size());
        ++m_planEpoch;
        {
            std::lock_guard<std::mutex> lock(m_mx);
            if (m_fixedStart && firstChange >= 0 && firstChange < m_fixedStart->tick) m_fixedStart.reset();
            m_fixedPrefix = prefix;
            m_plan = prefix;
            m_committed = prefix;
            m_executed.clear();
            for (int i = 0; i < played; ++i) m_executed.push_back(prefix[static_cast<size_t>(i)] & 3);
            m_real.clear();
        }
        m_consumed = played;
        m_planLen = static_cast<int>(prefix.size());
        m_lastSent = played == 0 ? 0 : (prefix[static_cast<size_t>(played - 1)] & 3);
        m_doomed = false;
        m_deadEnd = false;
        m_abandon = false;
        m_stuckReset = false;
        m_giveUpGen = -1;
        m_lostAt = -1;
        m_frozenSince = {};
        m_frozenFor = 0;
        m_starving = false;
        m_searchComplete = false;
        m_attemptStart = std::chrono::steady_clock::now();
        if (m_pathFrozen.exchange(false)) {
            std::lock_guard<std::mutex> lock(m_pathMx);
            m_frozenPath.clear();
        }
        publishPlan(prefix, false);
        m_editHoldSince = std::chrono::steady_clock::now();
        m_restartSearch = true;
        m_searchCancel = true;
        Log::printf("live: {} ticks fixed, live mode goes on from tick {}", prefix.size(), played);
    }

    // a divergence: the inputs played so far stay fixed, the search starts from the corrected state
    void SimPlanner::resyncFromReal(sim::Game corrected) {
        if (m_phase != Phase::Live) return;
        std::vector<uint8_t> prefix;
        std::vector<uint8_t> plan;
        {
            std::lock_guard<std::mutex> lock(m_mx);
            prefix = m_executed;
            plan = m_plan;
        }
        int const played = static_cast<int>(prefix.size());
        bool keepPlan = false;
        if (plan.size() > prefix.size()) {
            sim::Game g = corrected;
            while (!g.died && !g.completed && g.tick < static_cast<int>(plan.size()))
                sim::stepTick(g, plan[static_cast<size_t>(g.tick)]);
            keepPlan = !g.died;
        }
        if (keepPlan) prefix = plan;
        int const tick = corrected.tick;
        {
            std::lock_guard<std::mutex> lock(m_mx);
            m_fixedStart = std::make_shared<sim::Game>(std::move(corrected));
        }
        Log::printf(
                "live: the real player differs from the simulator at tick {}; the simulator takes the real position{}.",
                tick, keepPlan ? ", the plan still holds from there and stays" : " and the search goes on from there");
        resumeFromEdit(prefix, -1, played);
    }

    // the edited inputs become the macro that play and save use
    void SimPlanner::setMacroFromInputs(std::vector<uint8_t> const& inputs) {
        m_macro.clear();
        appendToggles(m_macro, inputs);
        m_macroTicks = static_cast<int>(inputs.size());
    }

    // stops the search and live mode, puts back the game flags
    void SimPlanner::stop() {
        Phase const previousPhase = m_phase;
        m_searchCancel = true;
        joinThread();
        m_searchCancel = false;
        stopObserver();
        if (previousPhase == Phase::Live) restoreFlags(PlayLayer::get());
        if (previousPhase == Phase::Live) {
            if (auto* pl = PlayLayer::get(); pl && pl->m_player1) {
                pl->m_queuedButtons.clear();
                if (m_lastSent & 1) pl->queueButton(1, false, flipTwoPlayer(), 0.0);
                if (m_lastSent & 2) pl->queueButton(1, false, !flipTwoPlayer(), 0.0);
                m_lastSent = 0;
            }
        }
        if (previousPhase != Phase::Idle) {
            m_phase = (previousPhase == Phase::OfflineDone || previousPhase == Phase::OfflineFailed) ? previousPhase : Phase::Idle;
        }
        m_persistFrozen = false;
    }

    // simulates from the start without pressing, then holding, and logs what kills the player
    void SimPlanner::diagnose() {
        if (!m_world) return;
        bool haveStart;
        float startX;
        float startY;
        {
            std::lock_guard<std::mutex> lock(m_startMx);
            haveStart = m_haveStart;
            startX = m_startX;
            startY = m_startY;
        }
        Log::printf("diagnostic: simulated start ({:.2f}, {:.2f}){}; world: {} objects, length {:.0f}",
                    haveStart ? startX : m_world->startX, haveStart ? startY : m_world->startY,
                    haveStart ? " (real position at reset)" : " (world position)", m_world->objs.size(),
                    m_world->length);
        for (int mode = 0; mode < 2; ++mode) {
            sim::Game g = freshGame();
            int guard = 0;
            while (!g.died && !g.completed && guard++ < 100000 && !m_cancel.load()) sim::stepTick(g, mode == 1);
            if (m_cancel.load()) return;
            if (g.died) {
                sim::GObj killer = g.killerUid ? g.objByUid(g.killerUid) : sim::GObj{};
                Log::printf(
                        "diagnostic ({}): death at tick {} at ({:.2f}, {:.2f}) yv {:.4f}; killer uid {}{}",
                        mode ? "button held" : "no input", g.tick - 1, g.p1.x, g.p1.y, g.p1.yVelocity, g.killerUid,
                        killer ? fmt::format(" (id {}, type {}, at {:.1f},{:.1f}, rect {:.1f},{:.1f} {:.1f}x{:.1f})",
                                            killer.o->id, killer.o->type, killer.o->x, killer.o->y, killer.o->rect.x,
                                            killer.o->rect.y, killer.o->rect.w, killer.o->rect.h) : std::string(" (ground / ceiling / crushed)"));
            } else {
                Log::printf("diagnostic ({}): alive after {} ticks, x = {:.1f}{}", mode ? "button held" : "no input",
                            g.tick, g.p1.x, g.completed ? " (level complete)" : "");
            }
        }
    }

    // starts the offline search on the whole level
    bool SimPlanner::startOffline(PlayLayer* pl) {
        if (!pl || !m_hasWorld || !m_world) return false;
        stop();
        if (Engine::get().playing()) Engine::get().stopPlayback();
        m_layer = pl;
        m_offlineConsumed = false;
        m_threadDone = false;
        m_cancel = false;
        m_phase = Phase::OfflineSearch;
        readAttemptContext(pl);
        if (m_world) m_world->attempts = pl->m_attempts + 1;
        setStatus("Auto Pathing Tool: searching (simulator)...");
        Log::print("offline search: simulator, whole level.");
        m_thread = std::thread([this] { offlineThread(); });
        return true;
    }

    // offline search thread
    void SimPlanner::offlineThread() {
        lowerThreadPriority();
        auto const& settings = Settings::get();
        sim::SearchOptions options;
        options.threadInit = lowerThreadPriority;
        options.beamWidth = settings.beamWidth;
        options.maxBeamWidth = std::max(settings.beamWidth * 16, 6000);
        options.minPressGap = settings.minGapTicks();
        options.horizonTicks = 0;
        options.timeLimit = settings.timeLimit > 0 ? static_cast<double>(settings.timeLimit) : 0.0;
        options.threads = searchThreads(false);
        sim::Game g = freshGame();
        auto progress = [this](int tick, int layer) {
            setStatus(fmt::format("Auto Pathing Tool: searching... tick {} ({:.1f} s of level), {} states", tick, tick / 240.0, layer));
        };
        if (sim::menuWanted(*m_world)) {
            sim::SearchOptions menuOptions = options;
            menuOptions.menuGoal = true;
            menuOptions.persistChangeCost = 0;
            menuOptions.horizonTicks = 4800;
            menuOptions.timeLimit = 30.0;
            sim::SearchResult menuResult = sim::searchPath(g, menuOptions, &m_cancel, progress);
            if (menuResult.menuDone && !m_cancel.load()) {
                Log::printf(
                        "offline search: the level starts with a settings menu; this plan sets the settings ({} ticks, "
                        "{} presses) and ends in the menu. On the next attempt, press {} again: the level search will "
                        "start from these settings.",
                        menuResult.inputs.size(), menuResult.presses, keyName(Action::Offline));
                menuResult.why = "menu settings applied";
                m_offlineResult = std::move(menuResult);
                m_threadDone = true;
                return;
            }
            if (!m_cancel.load()) {
                Log::printf("offline search: settings menu recognized, but no path sets the wanted settings ({}).",
                            menuResult.why);
            }
        }
        m_offlineResult = sim::searchPath(g, options, &m_cancel, progress);
        if (!m_offlineResult.success && !m_cancel.load()) diagnose();
        m_threadDone = true;
    }

    // starts live mode: the level restarts and plays while the search runs
    bool SimPlanner::startLive(PlayLayer* pl) {
        if (!pl || !m_hasWorld || !m_world) return false;
        stop();
        if (Engine::get().playing()) Engine::get().stopPlayback();
        m_layer = pl;
        {
            std::lock_guard<std::mutex> lock(m_mx);
            m_plan.clear();
            m_executed.clear();
            m_committed.clear();
            m_real.clear();
            m_divergences = 0;
        }
        m_rngReported = false;
        {
            std::lock_guard<std::mutex> lock(m_pathMx);
            m_path.clear();
        }
        ++m_pathVersion;
        m_consumed = 0;
        m_planLen = 0;
        m_frozenSince = {};
        m_frozenFor = 0;
        m_giveUpGen = -1;
        m_lostAt = -1;
        m_doomed = false;
        m_deadEnd = false;
        m_abandon = false;
        m_stuckReset = false;
        m_searchComplete = false;
        m_menuTried = false;
        {
            std::lock_guard<std::mutex> lock(m_mx);
            m_fixedPrefix.clear();
            m_fixedStart.reset();
        }
        m_resyncTicks.clear();
        m_restartSearch = false;
        m_menuPending = false;
        m_menuGen = -1;
        m_adoptPersist = false;
        m_lastSent = 0;
        m_cancel = false;
        m_searchCancel = false;
        m_threadDone = false;
        saveFlags(pl);
        m_phase = Phase::Live;
        setStatus("Auto Pathing Tool: live - first plan...");
        Log::print(
                "live mode: the full search runs continuously and publishes its final prefix; the attempt restarts "
                "from the beginning.");
        Engine::seedRng(Engine::kSeed);
        m_liveAttempts = pl->m_attempts;
        m_persistFrozen = false;
        pl->resetLevel();
        readAttemptContext(pl);
        m_persistFrozen = true;
        m_world->attempts = m_liveAttempts;
        pl->m_attempts = m_liveAttempts;
        startObserver();
        m_thread = std::thread([this] { liveThread(); });
        return true;
    }

    // restarts the drawn path from the start (observer thread)
    void SimPlanner::resetPath() {
        m_pathGame = freshGame();
        m_pathInputs.clear();
        m_pathCk.clear();
        m_pathFixed.reset();
        optSnap(m_pathGame, m_optLast);
        {
            std::lock_guard<std::mutex> lock(m_pathMx);
            m_path.clear();
            m_path.push_back({m_pathGame.p1.x, m_pathGame.p1.y});
            m_optMove.clear();
        }
        ++m_pathVersion;
    }

    // offsets of the optimized groups that matter
    void SimPlanner::optSnap(sim::Game const& g, std::vector<std::pair<double, double>>& out) const {
        out.assign(m_optGroups.size(), {0.0, 0.0});
        if (!g.world) return;
        for (size_t i = 0; i < m_optGroups.size(); ++i)
            sim::trigGroupOffset(g, m_optGroups[i], out[i].first, out[i].second);
    }

    // true if optimized objects must move on this step (one step margin each side, true when unsure)
    bool SimPlanner::forceOptimizedMoves(int step) const {
        if (!m_optSync) return false;
        if (m_phase != Phase::Live || step < 0) return true;
        std::lock_guard<std::mutex> lock(m_pathMx);
        if (m_pathFrozen.load()) return true;
        size_t const index = static_cast<size_t>(step);
        if (index + 1 >= m_optMove.size()) return true;
        return m_optMove[index] || m_optMove[index + 1] || (index > 0 && m_optMove[index - 1]);
    }

    // replays the published plan for the drawn path, from the last common checkpoint when the plan changed
    void SimPlanner::updatePath(std::vector<uint8_t> const& inputs) {
        size_t common = 0;
        size_t const oldSize = m_pathInputs.size();
        while (common < inputs.size() && common < m_pathInputs.size() && inputs[common] == m_pathInputs[common])
            ++common;
        std::shared_ptr<sim::Game> fixed;
        {
            std::lock_guard<std::mutex> lock(m_mx);
            fixed = m_fixedStart;
        }
        if (fixed != m_pathFixed) {
            int earliest = std::numeric_limits<int>::max();
            if (fixed) earliest = std::min(earliest, fixed->tick);
            if (m_pathFixed) earliest = std::min(earliest, m_pathFixed->tick);
            if (earliest >= 0 && static_cast<size_t>(earliest) < common) common = static_cast<size_t>(earliest);
            m_pathFixed = fixed;
            if (common >= m_pathInputs.size() && common > 0 && !m_pathInputs.empty())
                common = std::min(common, m_pathInputs.size() - 1);
        }
        if (common < m_pathInputs.size()) {
            while (!m_pathCk.empty() && m_pathCk.back().first > common) m_pathCk.pop_back();
            size_t from = 0;
            if (m_pathCk.empty()) {
                m_pathGame = freshGame();
            } else {
                m_pathGame = m_pathCk.back().second;
                from = m_pathCk.back().first;
            }
            m_pathInputs.resize(from);
            if (m_optSync) optSnap(m_pathGame, m_optLast);
            std::lock_guard<std::mutex> lock(m_pathMx);
            m_path.resize(from + 1);
            if (m_optMove.size() > from) m_optMove.resize(from);
        }
        if (inputs.size() <= m_pathInputs.size()) {
            if (common < oldSize) ++m_pathVersion;
            return;
        }
        std::vector<cocos2d::CCPoint> newPoints;
        newPoints.reserve(inputs.size() - m_pathInputs.size());
        std::vector<uint8_t> newOptMoves;
        static thread_local std::vector<std::pair<double, double>> optCur;
        if (m_optSync) newOptMoves.reserve(inputs.size() - m_pathInputs.size());
        for (size_t i = m_pathInputs.size(); i < inputs.size(); ++i) {
            if (i % kPathCk == 0 && i > 0 && (m_pathCk.empty() || m_pathCk.back().first < i))
                m_pathCk.emplace_back(i, m_pathGame);
            if (m_pathFixed && static_cast<int>(i) == m_pathFixed->tick) m_pathGame = *m_pathFixed;
            sim::stepTick(m_pathGame, inputs[i]);
            newPoints.push_back({m_pathGame.p1.x, m_pathGame.p1.y});
            if (m_optSync) {
                optSnap(m_pathGame, optCur);
                newOptMoves.push_back(optCur != m_optLast ? 1 : 0);
                m_optLast.swap(optCur);
            }
        }
        m_pathInputs.assign(inputs.begin(), inputs.end());
        {
            std::lock_guard<std::mutex> lock(m_pathMx);
            m_path.insert(m_path.end(), newPoints.begin(), newPoints.end());
            if (m_optSync) {
                if (m_optMove.size() > m_pathInputs.size() - newOptMoves.size())
                    m_optMove.resize(m_pathInputs.size() - newOptMoves.size());
                m_optMove.insert(m_optMove.end(), newOptMoves.begin(), newOptMoves.end());
            }
        }
        ++m_pathVersion;
    }

    // called by the search when its final prefix changes, marks the attempt lost if it changes what was played
    void SimPlanner::publishPlan(std::vector<uint8_t> const& inputs, bool definitive) {
        bool const wasDoomed = m_doomed.load();
        {
            std::lock_guard<std::mutex> lock(m_mx);
            m_committed = inputs;
            int const consumed = m_consumed.load();
            if (m_doomed.load()) {
            } else if (static_cast<int>(inputs.size()) < consumed && !m_plan.empty()) {
                Log::printf(
                        "live: the final prefix moved back to tick {} while {} ticks were played; new attempt, the "
                        "search goes on.",
                        inputs.size(), consumed);
                m_doomed = true;
                m_abandon = true;
            } else {
                size_t const executedCount =
                        std::min<size_t>(m_executed.size(), static_cast<size_t>(std::max(consumed, 0)));
                size_t firstChanged = executedCount;
                for (size_t i = 0; i < executedCount && i < inputs.size(); ++i) {
                    if ((inputs[i] & 3) != m_executed[i]) {
                        firstChanged = i;
                        break;
                    }
                }
                if (firstChanged < executedCount && !m_plan.empty()) {
                    Log::printf(
                            "live: the published plan changes tick {}, already played (played {}, plan {}{}); new "
                            "attempt, the search goes on.",
                            firstChanged, static_cast<int>(m_executed[firstChanged]),
                            static_cast<int>(inputs[firstChanged] & 3),
                            firstChanged >= m_plan.size() ? ", played without a plan" : "");
                    m_doomed = true;
                    m_abandon = true;
                } else {
                    size_t const played = std::min<size_t>(m_plan.size(), static_cast<size_t>(std::max(consumed, 0)));
                    if (played > 0 &&
                        !std::equal(m_plan.begin(), m_plan.begin() + static_cast<long>(played), inputs.begin())) {
                        Log::printf(
                                "live: the new path differs from the {} ticks already played; new attempt, the search "
                                "goes on.",
                                consumed);
                        m_doomed = true;
                        m_abandon = true;
                    } else {
                        m_plan = inputs;
                        m_planLen = static_cast<int>(m_plan.size());
                    }
                }
            }
        }
        if (!wasDoomed && m_doomed.load() && !m_pathFrozen.load()) {
            std::lock_guard<std::mutex> lock(m_pathMx);
            m_frozenPath = m_path;
            m_pathFrozen = true;
        }
        {
            std::lock_guard<std::mutex> lock(m_obsMx);
            m_obsInputs = inputs;
            m_obsNew = true;
        }
        m_obsCv.notify_one();
        (void)definitive;
    }

    // replays the ticks really played in the mirror game and logs where the real game differs
    void SimPlanner::verifyMirror() {
        std::vector<RealSample> samples;
        std::vector<RealSample> prevSamples;
        std::vector<uint8_t> curInputs;
        std::vector<uint8_t> prevInputs;
        {
            std::lock_guard<std::mutex> lock(m_mx);
            prevSamples.swap(m_prevReal);
            prevInputs.swap(m_prevExecuted);
            if (m_real.empty() && prevSamples.empty()) return;
            samples.swap(m_real);
            curInputs = m_executed;
        }
        std::vector<std::pair<RealSample, bool>> allSamples;
        for (auto const& sample : prevSamples) allSamples.emplace_back(sample, true);
        for (auto const& sample : samples) allSamples.emplace_back(sample, false);
        bool prevDone = prevSamples.empty();
        for (auto const& [sample, isPrev] : allSamples) {
            if (!isPrev && !prevDone) {
                prevDone = true;
                m_mirror = freshGame();
                m_divergences = 0;
                m_rngReported = false;
            }
            std::vector<uint8_t> const& inputs = isPrev ? prevInputs : curInputs;
            if (sample.step < m_mirror.tick || !m_mirror.world) {
                m_mirror = freshGame();
                m_divergences = 0;
                m_rngReported = false;
            }
            if (!m_mirror.world) return;
            while (m_mirror.tick < sample.step && m_mirror.tick < static_cast<int>(inputs.size()) && !m_mirror.died)
                sim::stepTick(m_mirror, inputs[static_cast<size_t>(m_mirror.tick)]);
            if (m_mirror.tick != sample.step) continue;
            if (!m_rngReported && m_mirror.world && m_mirror.world->hasRandom) {
                auto rngClose = [](uint32_t a, uint32_t b) {
                    for (int i = 0; i <= 8; ++i, a = sim::Game::rngNext(a))
                        if (a == b) return true;
                    return false;
                };
                if (!rngClose(m_mirror.rng, sample.rng) && !rngClose(sample.rng, m_mirror.rng)) {
                    m_rngReported = true;
                    Log::printf(
                            "RNG: mismatch at tick {} (simulator {}, game {}): random triggers no longer land as "
                            "planned.",
                            sample.step, m_mirror.rng, sample.rng);
                }
            }
            float const dx = m_mirror.p1.x - sample.x;
            float const dy = m_mirror.p1.y - sample.y;
            if (std::fabs(dx) > 0.01f || std::fabs(dy) > 0.01f) {
                if (m_divergences < 20) {
                    Log::printf(
                            "divergence tick {}: sim ({:.4f}, {:.4f}, yv {:.5f}) real ({:.4f}, {:.4f}, yv {:.5f}); "
                            "button played {}, held in game {}",
                            sample.step, m_mirror.p1.x, m_mirror.p1.y, m_mirror.p1.yVelocity, sample.x, sample.y,
                            sample.yv,
                            sample.step > 0 && static_cast<size_t>(sample.step - 1) < inputs.size()
                                    ? static_cast<int>(inputs[static_cast<size_t>(sample.step - 1)] & 1)
                                    : -1,
                            sample.held);
                }
                if (m_divergences == 0) {
                    std::string history;
                    for (int tick = std::max(0, sample.step - 12);
                        tick < sample.step && static_cast<size_t>(tick) < inputs.size(); ++tick)
                        history += fmt::format(" {}:{}", tick, static_cast<int>(inputs[static_cast<size_t>(tick)] & 1));
                    Log::printf("divergence: buttons played before tick {}:{}", sample.step, history);
                }
                ++m_divergences;
                bool const pastPlanEnd = m_searchComplete.load() && sample.step >= m_planLen.load() - 1;
                bool const steady = !isPrev && m_lastDivStep == sample.step - 1 && std::fabs(dy) < 0.01f && std::fabs(dx) > 0.05f && std::fabs(dx) < 3.f && std::fabs(dx - m_lastDivDx) < 0.05f;
                bool spaced = m_resyncTicks.size() < 4;
                for (int t : m_resyncTicks)
                    if (std::abs(t - sample.step) < 240) spaced = false;
                if (!isPrev) {
                    m_lastDivStep = sample.step;
                    m_lastDivDx = dx;
                }
                if (!isPrev && steady && spaced && sample.step >= 240 && !m_mirror.died && !m_mirror.completed &&
                    !pastPlanEnd && m_phase == Phase::Live && !m_doomed.load()) {
                    m_resyncTicks.push_back(sample.step);
                    sim::Game corrected = m_mirror;
                    float const shiftX = sample.x - corrected.p1.x;
                    float const shiftY = sample.y - corrected.p1.y;
                    corrected.p1.x += shiftX;
                    corrected.p1.lastX += shiftX;
                    corrected.p1.y += shiftY;
                    corrected.p1.lastY += shiftY;
                    corrected.p1.yVelocity = sample.yv;
                    corrected.p1.snapDistance = sample.snap;
                    m_mirror = corrected;
                    resyncFromReal(std::move(corrected));
                }
            }
        }
    }

    // starts the observer thread (path drawing and checking)
    void SimPlanner::startObserver() {
        stopObserver();
        {
            std::lock_guard<std::mutex> lock(m_obsMx);
            m_obsQuit = false;
            m_obsNew = false;
            m_obsInputs.clear();
        }
        m_obsThread = std::thread([this] { observerThread(); });
    }

    // stops the observer thread and waits for it
    void SimPlanner::stopObserver() {
        {
            std::lock_guard<std::mutex> lock(m_obsMx);
            m_obsQuit = true;
        }
        m_obsCv.notify_all();
        if (m_obsThread.joinable()) m_obsThread.join();
        {
            std::lock_guard<std::mutex> lock(m_obsMx);
            m_obsQuit = false;
            m_obsNew = false;
            m_obsInputs.clear();
        }
    }

    // observer thread: redraws the path on each new plan and checks the real game 4 times per second
    void SimPlanner::observerThread() {
        lowerThreadPriority();
        resetPath();
        m_mirror = freshGame();
        std::vector<uint8_t> inputs;
        for (;;) {
            bool newPlan = false;
            {
                std::unique_lock<std::mutex> lock(m_obsMx);
                m_obsCv.wait_for(lock, std::chrono::milliseconds(250), [this] { return m_obsQuit || m_obsNew; });
                if (m_obsQuit) return;
                if (m_obsNew) {
                    inputs.swap(m_obsInputs);
                    m_obsNew = false;
                    newPlan = true;
                }
            }
            if (newPlan) updatePath(inputs);
            verifyMirror();
        }
    }

    // live: plays an attempt that only sets the level's settings menu, returns false if not needed or not found
    bool SimPlanner::startMenuAttempt(sim::SearchOptions const& options, std::function<void(int, int)> const& progress) {
        if (m_menuTried || !m_world || !sim::menuWanted(*m_world)) return false;
        m_menuTried = true;
        sim::SearchOptions menuOptions = options;
        menuOptions.menuGoal = true;
        menuOptions.persistChangeCost = 0;
        menuOptions.playedTicks = nullptr;
        menuOptions.starving = nullptr;
        menuOptions.waitFor = nullptr;
        menuOptions.onPrefix = nullptr;
        menuOptions.horizonTicks = 4800;
        menuOptions.timeLimit = 30.0;
        sim::SearchResult const menuResult = sim::searchPath(freshGame(), menuOptions, &m_searchCancel, progress);
        if (m_cancel.load()) return false;
        if (!menuResult.menuDone) {
            Log::printf("live: settings menu recognized, but no path sets the wanted settings ({}).", menuResult.why);
            return false;
        }
        std::string summary;
        for (auto const& menu : m_world->menus) {
            if (menu.hasPrefer) {
                std::string label;
                for (auto const& op : menu.options) {
                    if (op.value == menu.prefer) {
                        label = op.label;
                        break;
                    }
                }
                summary += fmt::format("{}refresh rate {} (item {} = {})", summary.empty() ? "" : ", ", label, menu.item, menu.prefer);
            } else if (menu.kind == "ldm") {
                summary += fmt::format("{}low detail avoided (item {})", summary.empty() ? "" : ", ", menu.item);
            }
        }
        m_menuPersist = menuResult.persistAfter;
        bool const now = m_consumed.load() == 0 && !m_doomed.load();
        m_menuGen = m_generation.load() + (now ? 0 : 1);
        m_menuPending = true;
        if (!now) m_doomed = true;
        publishPlan(menuResult.inputs, true);
        m_searchComplete = true;
        Log::printf(
                "live: the level starts with a settings menu ({}); {} attempt sets them ({} ticks) and ends in the "
                "menu, the one after plays the level.",
                summary, now ? "this" : "the next", menuResult.inputs.size());
        setStatus(
                fmt::format("Auto Pathing Tool: live - settings menu: {}; the next attempt plays the level", summary));
        return true;
    }

    // live search thread: searches, publishes the plan, and starts again with a wider beam after a dead end
    void SimPlanner::liveThread() {
        lowerThreadPriority();
        auto const& settings = Settings::get();
        auto statusLine = [this](int searchTick, int layer) {
            int const consumed = m_consumed.load();
            int const planLen = m_planLen.load();
            int const lost = m_lostAt.load();
            if (consumed == 0 && lost >= 0 && searchTick < lost + 480) {
                return fmt::format(
                        "Auto Pathing Tool: live - waiting at the start: the search (tick {}) must pass tick {}, where "
                        "the last attempt stopped{}",
                        searchTick, lost, layer > 0 ? fmt::format(" ({} states)", layer) : std::string());
            }
            if (consumed == 0 && planLen == 0 && searchTick < sim::kStartLead) {
                return fmt::format(
                        "Auto Pathing Tool: live - waiting at the start: the search looks 10 s ahead first (tick {} / "
                        "{}){}",
                        searchTick, sim::kStartLead, layer > 0 ? fmt::format(" ({} states)", layer) : std::string());
            }
            if (m_doomed.load() || m_giveUpGen.load() == m_generation.load()) {
                return fmt::format(
                        "Auto Pathing Tool: live - tick {}, {} at tick {}; the search (tick {}{}) goes on for the next "
                        "attempt",
                        consumed, m_doomed.load() ? "attempt lost, plan stopped" : "no plan (the search fell behind)",
                        planLen, searchTick, layer > 0 ? fmt::format(", {} states", layer) : std::string());
            }
            return fmt::format("Auto Pathing Tool: live - tick {} / plan up to {} (+{:.1f} s), search at tick {}{}",
                            consumed, planLen, (planLen - consumed) / 240.0, searchTick,
                            layer > 0 ? fmt::format(" ({} states)", layer) : std::string());
        };
        bool haveResult = false;
        sim::SearchResult result;
        int beamMul = 1;
        int generation = m_generation.load();
        while (!m_cancel.load()) {
            if (haveResult && m_restartSearch.exchange(false)) haveResult = false;
            if (!haveResult) {
                m_searchCancel = false;
                m_deadEnd = false;
                {
                    std::lock_guard<std::mutex> lock(m_mx);
                    m_committed.clear();
                }
                m_searchComplete = false;
                m_frontierTick = 0;
                sim::SearchOptions options;
                options.beamWidth = settings.beamWidth * beamMul;
                options.maxBeamWidth = std::max(options.beamWidth * 16, 6000);
                options.minPressGap = settings.minGapTicks();
                options.horizonTicks = 0;
                options.threads = searchThreads(true);
                options.threadInit = lowerThreadPriority;
                if (m_liveThreads.exchange(options.threads) != options.threads) {
                    Log::printf(
                            "live: {} search threads (CPU: {} logical threads, {} physical cores), below the game's "
                            "priority",
                            options.threads, std::thread::hardware_concurrency(), physicalCores());
                }
                options.commitInterval = 120;
                options.playedTicks = &m_consumed;
                options.starving = &m_starving;
                options.holding = &m_holding;
                options.minLead = 480;
                options.maxLead = 0;
                options.waitFor = &m_lostAt;
                int const epoch = m_planEpoch.load();
                options.onPrefix = [this, epoch](std::vector<uint8_t> const& prefix) {
                    if (m_planEpoch.load() == epoch) publishPlan(prefix, true);
                };
                auto lastStatus = std::chrono::steady_clock::now();
                auto progress = [this, &lastStatus, &statusLine](int tick, int layer) {
                    m_frontierTick = tick;
                    m_lastLayer = layer;
                    auto const now = std::chrono::steady_clock::now();
                    if (now - lastStatus > std::chrono::milliseconds(250)) {
                        lastStatus = now;
                        setStatus(statusLine(tick, layer));
                    }
                };
                std::vector<uint8_t> prefix;
                std::shared_ptr<sim::Game> fixedStart;
                {
                    std::lock_guard<std::mutex> lock(m_mx);
                    prefix = m_fixedPrefix;
                    fixedStart = m_fixedStart;
                }
                if (!prefix.empty()) {
                    options.onPrefix = [this, prefix, epoch](std::vector<uint8_t> const& found) {
                        if (m_planEpoch.load() != epoch) return;
                        std::vector<uint8_t> plan = prefix;
                        plan.insert(plan.end(), found.begin(), found.end());
                        publishPlan(plan, true);
                    };
                }
                if (prefix.empty() && startMenuAttempt(options, progress)) {
                    result = sim::SearchResult{};
                    haveResult = true;
                    generation = m_generation.load();
                    continue;
                }
                if (m_cancel.load()) break;
                auto start = std::chrono::steady_clock::now();
                sim::Game searchStart = freshGame();
                size_t fromTick = 0;
                if (fixedStart && static_cast<size_t>(fixedStart->tick) <= prefix.size()) {
                    searchStart = *fixedStart;
                    fromTick = static_cast<size_t>(fixedStart->tick);
                }
                for (size_t i = fromTick; i < prefix.size(); ++i) sim::stepTick(searchStart, prefix[i]);
                result = sim::searchPath(searchStart, options, &m_searchCancel, progress);
                if (m_restartSearch.exchange(false)) {
                    m_searchCancel = false;
                    continue;
                }
                result.inputs.insert(result.inputs.begin(), prefix.begin(), prefix.end());
                m_lastSearchSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                if (m_cancel.load()) break;
                haveResult = true;
                generation = m_generation.load();
                if (result.success) {
                    publishPlan(result.inputs, true);
                    m_searchComplete = true;
                    Log::printf("live: full plan ({} ticks, {} presses) found in {:.1f} s", result.inputs.size(),
                                result.presses, result.stats.seconds);
                    setStatus(
                            fmt::format("Auto Pathing Tool: live - full plan to the end of the level ({} presses, "
                                        "{:.1f} s of search)",
                                        result.presses, result.stats.seconds));
                } else if (result.fromStart && startMenuAttempt(options, progress)) {
                } else {
                    publishPlan(result.inputs, false);
                    m_doomed = true;
                    m_deadEnd = true;
                    Log::printf(
                            "live: no path found beyond tick {} ({}); the attempt will fail there, then the search "
                            "starts again with a wider beam.",
                            result.inputs.size(), result.why);
                    beamMul = std::min(beamMul * 2, 4);
                    if (result.inputs.size() < 2) diagnose();
                    if (auto* pl = PlayLayer::get(); pl && !m_deadEndExported) {
                        m_deadEndExported = true;
                        exportForDebug(pl, "live dead end");
                    }
                    setStatus(fmt::format(
                            "Auto Pathing Tool: live - dead end at {:.1f} s ({}); new search on the next attempt",
                            result.inputs.size() / 240.0, result.why));
                }
                continue;
            }
            if (generation != m_generation.load()) {
                generation = m_generation.load();
                if (m_menuPending.load()) {
                    if (generation <= m_menuGen.load()) continue;
                    m_menuPending = false;
                    if (m_adoptPersist.exchange(false)) {
                        std::string text;
                        {
                            std::lock_guard<std::mutex> lock(m_startMx);
                            m_world->persistentItems = m_newPersist;
                            for (auto const& entry : m_newPersist)
                                text += fmt::format(" {}={}", entry.first, entry.second);
                        }
                        Log::printf(
                                "live: settings done; persistent items at the start are now:{} - the level search "
                                "restarts with them.",
                                text.empty() ? std::string(" (none)") : text);
                    }
                    beamMul = 1;
                    haveResult = false;
                    continue;
                }
                if (m_adoptPersist.exchange(false)) {
                    std::string text;
                    {
                        std::lock_guard<std::mutex> lock(m_startMx);
                        m_world->persistentItems = m_newPersist;
                        for (auto const& entry : m_newPersist) text += fmt::format(" {}={}", entry.first, entry.second);
                    }
                    Log::printf(
                            "live: the last attempt changed the persistent items, they are now:{} - the search starts "
                            "again with them.",
                            text.empty() ? std::string(" (none)") : text);
                    m_deadEnd = false;
                    beamMul = 1;
                    haveResult = false;
                    continue;
                }
                if (result.success) {
                    m_doomed = false;
                    setStatus(fmt::format("Auto Pathing Tool: live - replaying the full plan ({} presses)", result.presses));
                } else {
                    haveResult = false;
                    continue;
                }
            }
            if (!m_doomed.load() && !result.success && !m_menuPending.load())
                setStatus(statusLine(m_frontierTick.load(), 0));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        m_threadDone = true;
    }

    // every frame on the game thread: picks up the offline result, saves the macro and plays it
    void SimPlanner::frame(PlayLayer* pl) {
        if (m_phase == Phase::OfflineSearch && m_threadDone.load()) {
            joinThread();
            auto const& result = m_offlineResult;
            m_macro.clear();
            m_macroTicks = static_cast<int>(result.inputs.size());
            appendToggles(m_macro, result.inputs);
            if (result.success) {
                m_phase = Phase::OfflineDone;
                Log::printf(
                        "search finished: {} ({:.2f} s, {} ticks simulated, {} presses, max layer {}, {} widenings)",
                        result.why, result.stats.seconds, result.stats.ticksSimulated, result.presses,
                        result.stats.maxLayer, result.stats.widenings);
                if (result.menuDone) {
                    setStatus(
                            fmt::format("Auto Pathing Tool: settings menu - this plan sets the settings and ends in "
                                        "the menu; on the next attempt, press {} for the level",
                                        keyName(Action::Offline)));
                } else {
                    setStatus(fmt::format(
                            "Auto Pathing Tool: solved in {:.1f} s - {} presses. Playing... ({}: replay, {}: export)",
                            result.stats.seconds, result.presses, keyName(Action::Play), keyName(Action::Export)));
                }
                if (!result.menuDone && pl && pl->m_level) {
                    auto saved = m_macro.exportGDR(
                            Mod::get()->getSaveDir() / "macros", std::string(pl->m_level->m_levelName),
                            static_cast<int>(pl->m_level->m_levelID.value()), Mod::get()->getVersion().toVString());
                    if (saved)
                        Log::printf("macro saved: {}", geode::utils::string::pathToString(saved.unwrap().filename()));
                    else
                        Log::printf("macro not saved: {}", saved.unwrapErr());
                }
                if (pl && PlayLayer::get() == pl) Engine::get().startPlayback(m_macro);
            } else {
                m_phase = Phase::OfflineFailed;
                Log::printf("search: failed ({}), {} ticks found, {:.2f} s", result.why, result.inputs.size(),
                            result.stats.seconds);
                if (pl) exportForDebug(pl, "offline failure");
                setStatus(fmt::format(
                        "Auto Pathing Tool: failed ({}) at {:.1f} s of level - {}: play the best, {}: retry",
                        result.why, result.inputs.size() / 240.0, keyName(Action::Play), keyName(Action::Offline)));
            }
        }
    }

    // copies the attempt count and the persistent items and timers into the world
    void SimPlanner::readAttemptContext(PlayLayer* pl) {
        if (!pl || !m_world) return;
        m_world->attempts = pl->m_attempts;
        if (auto* effectManager = pl->m_effectManager) {
            std::vector<std::pair<int, int>> items;
            for (auto const& [k, v] : effectManager->m_persistentItemCountMap) items.emplace_back(k, v);
            std::sort(items.begin(), items.end());
            m_world->persistentItems = std::move(items);
            std::vector<std::pair<int, double>> timers;
            for (int k : effectManager->m_persistentTimerItemSet) timers.emplace_back(k, 0.0);
            std::sort(timers.begin(), timers.end());
            m_world->persistentTimers = std::move(timers);
        }
    }

    // before a real reset: keep the attempt number and the persistent items the plan was made for
    void SimPlanner::onPreRealReset(PlayLayer* pl) {
        if (m_phase == Phase::Live && pl == m_layer && m_liveAttempts >= 0) pl->m_attempts = m_liveAttempts;
        if (m_phase == Phase::Live && pl == m_layer && m_menuPending.load() &&
            m_generation.load() == m_menuGen.load() && pl->m_effectManager) {
            std::vector<std::pair<int, int>> items;
            for (auto const& [k, v] : pl->m_effectManager->m_persistentItemCountMap) items.emplace_back(k, v);
            std::sort(items.begin(), items.end());
            if (items != m_menuPersist) {
                std::string planned;
                std::string inGame;
                for (auto const& entry : m_menuPersist) planned += fmt::format(" {}={}", entry.first, entry.second);
                for (auto const& entry : items) inGame += fmt::format(" {}={}", entry.first, entry.second);
                Log::printf("live: settings: persistent items planned{}, in game{} - the game's are kept.", planned,
                            inGame);
            }
            {
                std::lock_guard<std::mutex> lock(m_startMx);
                m_newPersist = std::move(items);
            }
            m_adoptPersist = true;
            {
                std::lock_guard<std::mutex> lock(m_mx);
                m_committed.clear();
            }
            m_searchComplete = false;
            return;
        }
        if (m_phase == Phase::Live && pl == m_layer && m_deadEnd.load() && m_world && pl->m_effectManager) {
            std::vector<std::pair<int, int>> items;
            for (auto const& [k, v] : pl->m_effectManager->m_persistentItemCountMap) items.emplace_back(k, v);
            std::sort(items.begin(), items.end());
            bool changed;
            {
                std::lock_guard<std::mutex> lock(m_startMx);
                changed = items != m_world->persistentItems;
                if (changed) m_newPersist = std::move(items);
            }
            if (changed) {
                m_adoptPersist = true;
                {
                    std::lock_guard<std::mutex> lock(m_mx);
                    m_committed.clear();
                }
                m_searchComplete = false;
                return;
            }
        }
        if (m_phase == Phase::Live && pl == m_layer && m_persistFrozen && m_world && pl->m_effectManager) {
            auto& itemMap = pl->m_effectManager->m_persistentItemCountMap;
            auto const& wanted = m_world->persistentItems;
            bool same = itemMap.size() == wanted.size();
            if (same) {
                for (auto const& [k, v] : wanted) {
                    auto it = itemMap.find(k);
                    if (it == itemMap.end() || it->second != v) {
                        same = false;
                        break;
                    }
                }
            }
            if (!same) {
                std::string diff;
                for (auto const& [k, v] : itemMap) {
                    int wantedValue = 0;
                    for (auto const& entry : wanted) {
                        if (entry.first == k) {
                            wantedValue = entry.second;
                            break;
                        }
                    }
                    if (wantedValue != v) diff += fmt::format(" {}: {} -> {}", k, v, wantedValue);
                }
                itemMap.clear();
                for (auto const& [k, v] : wanted) itemMap[k] = v;
                if (!diff.empty() && diff != m_persistWarned) {
                    m_persistWarned = diff;
                    Log::printf(
                            "persistent items reset to their value at the start of live mode:{} (the level changes "
                            "them between attempts; the plan is computed for the starting values)",
                            diff);
                }
            }
        }
    }

    // after a real reset: read the real start, then start the next attempt of the plan
    void SimPlanner::onRealReset(PlayLayer* pl) {
        if (m_phase == Phase::Live && pl == m_layer && m_liveAttempts >= 0) pl->m_attempts = m_liveAttempts;
        std::unique_lock<std::mutex> startLock(m_startMx);
        if (pl && pl->m_player2) readLeft(pl->m_player2, m_p2l);
        if (pl && pl->m_player1) readLeft(pl->m_player1, m_p1l);
        if (pl && pl->m_player1 && !pl->m_isPracticeMode && m_world) {
            auto const pos = pl->m_player1->getPosition();
            if (pos.x >= -100.f && pos.x <= m_world->length && pos.y >= 0.f &&
                pos.y <= std::max(3000.f, m_world->maxGameplayY + 300.f)) {
                m_startX = pos.x;
                m_startY = pos.y;
                m_haveStart = true;
            } else {
                Log::printf("reset: suspicious start position ({:.1f}, {:.1f}), keeping the world's ({:.1f}, {:.1f})", pos.x, pos.y, m_world->startX, m_world->startY);
            }
        }
        startLock.unlock();
        if (m_phase != Phase::Live || pl != m_layer) return;
        if (m_world && pl->m_effectManager && !m_adoptPersist.load()) {
            std::vector<std::pair<int, int>> items;
            for (auto const& [k, v] : pl->m_effectManager->m_persistentItemCountMap) items.emplace_back(k, v);
            std::sort(items.begin(), items.end());
            auto valueIn = [](std::vector<std::pair<int, int>> const& list, int key) {
                for (auto const& entry : list)
                    if (entry.first == key) return entry.second;
                return 0;
            };
            std::string diff;
            for (auto const& entry : items) {
                if (valueIn(m_world->persistentItems, entry.first) != entry.second) {
                    diff += fmt::format(" {}: {} -> {}", entry.first, valueIn(m_world->persistentItems, entry.first),
                                        entry.second);
                }
            }
            for (auto const& entry : m_world->persistentItems) {
                if (valueIn(items, entry.first) != entry.second && std::none_of(items.begin(), items.end(), [&](auto const& item) { return item.first == entry.first; }))
                    diff += fmt::format(" {}: {} -> 0", entry.first, entry.second);
            }
            if (!diff.empty() && diff != m_persistWarned) {
                m_persistWarned = diff;
                Log::printf(
                        "persistent items changed by the previous attempt:{} - the plan was computed without this "
                        "change (exit the level and press {} again to start from the current state)",
                        diff, keyName(Action::Live));
            }
        }
        size_t planLen = 0;
        {
            std::lock_guard<std::mutex> lock(m_mx);
            m_plan = m_committed;
            if (!m_real.empty()) {
                m_prevReal = std::move(m_real);
                m_prevExecuted = m_executed;
            }
            m_executed.clear();
            m_real.clear();
            planLen = m_plan.size();
            m_consumed = 0;
        }
        m_planLen = static_cast<int>(planLen);
        m_attemptStart = std::chrono::steady_clock::now();
        m_editHoldSince = {};
        m_lastSent = 0;
        m_doomed = false;
        m_abandon = false;
        m_stuckReset = false;
        if (m_pathFrozen.exchange(false)) {
            std::lock_guard<std::mutex> lock(m_pathMx);
            m_frozenPath.clear();
            m_frozenPath.shrink_to_fit();
        }
        ++m_pathVersion;
        ++m_generation;
        setStatus(fmt::format("Auto Pathing Tool: live - new attempt (plan up to {:.1f} s)...", planLen / 240.0));
    }

    // saves the real player 1 state of this tick for verifyMirror
    void SimPlanner::onRealTick(PlayLayer* pl, int step) {
        if (m_phase != Phase::Live || pl != m_layer || !pl->m_player1) return;
        auto* player = pl->m_player1;
        std::lock_guard<std::mutex> lock(m_mx);
        int held = -1;
        if (auto it = player->m_holdingButtons.find(1); it != player->m_holdingButtons.end()) held = it->second ? 1 : 0;
        if (m_real.size() < 4096) {
            m_real.push_back(RealSample{step, player->getPositionX(), player->getPositionY(), player->m_yVelocity, held, static_cast<uint32_t>(Engine::rng()), player->m_snapDistance});
        }
    }

    // sends the planned buttons of this tick to the game
    void SimPlanner::onQueuedButtons(GJBaseGameLayer* layer, int step) {
        if (m_phase != Phase::Live || !m_layer || layer != static_cast<GJBaseGameLayer*>(m_layer)) return;
        layer->m_queuedButtons.clear();
        uint8_t desired = m_lastSent;
        {
            std::lock_guard<std::mutex> lock(m_mx);
            if (step < static_cast<int>(m_plan.size())) desired = m_plan[static_cast<size_t>(step)] & 3;
            if (step >= 0) {
                if (m_executed.size() > static_cast<size_t>(step)) m_executed.resize(static_cast<size_t>(step));
                while (m_executed.size() < static_cast<size_t>(step)) m_executed.push_back(m_lastSent & 3);
                m_executed.push_back(desired);
            }
            m_consumed = step + 1;
        }
        if (Settings::get().autoReset && (m_doomed.load() || m_giveUpGen.load() == m_generation.load()) &&
            step >= m_planLen.load() + 1440 && !m_stuckReset.exchange(true)) {
            Log::printf(
                    "live: lost attempt still alive 6 s after the end of its plan (tick {}, plan up to {}); level "
                    "restarted.",
                    step, m_planLen.load());
            PlayLayer* const pl = m_layer;
            Loader::get()->queueInMainThread([pl] {
                if (PlayLayer::get() == pl) pl->resetLevel();
            });
        }
        bool const flip = flipTwoPlayer();
        if ((desired ^ m_lastSent) & 1) layer->queueButton(1, (desired & 1) != 0, flip, 0.0);
        if ((desired ^ m_lastSent) & 2) layer->queueButton(1, (desired & 2) != 0, !flip, 0.0);
        m_lastSent = desired;
    }

    // a real death: logged, and the next attempt waits until the search is past it
    void SimPlanner::onRealDeath(PlayLayer* pl, int step, GameObject* killer) {
        if (m_phase != Phase::Live || pl != m_layer) return;
        if (m_menuPending.load() && m_generation.load() == m_menuGen.load()) {
            Log::printf("live: end of the settings attempt (death at tick {}, in the menu, as planned).", step);
            return;
        }
        m_lostAt = step;
        bool const doomed = m_doomed.load();
        Log::printf("live: real death at tick {} (object uid {} id {}); plan up to {}{}", step,
                    killer ? killer->m_uniqueID : 0, killer ? killer->m_objectID : 0, m_planLen.load(),
                    doomed ? " (known dead end)" : " - the simulator did not predict this death");
        if (doomed || step >= m_planLen.load()) return;
        if (auto* p1 = pl->m_player1) {
            cocos2d::CCPoint planned{0.f, 0.f};
            bool havePlanned = false;
            {
                std::lock_guard<std::mutex> lock(m_pathMx);
                if (step >= 0 && static_cast<size_t>(step) < m_path.size()) {
                    planned = m_path[static_cast<size_t>(step)];
                    havePlanned = true;
                }
            }
            if (havePlanned) {
                Log::printf("live: at death, real player at ({:.1f}, {:.1f}), planned at ({:.1f}, {:.1f})", p1->getPositionX(), p1->getPositionY(), planned.x, planned.y);
            } else {
                Log::printf("live: at death, real player at ({:.1f}, {:.1f})", p1->getPositionX(), p1->getPositionY());
            }
        }
    }

    // the live plan finished the level: it becomes the macro to play or export
    void SimPlanner::onRealComplete(PlayLayer* pl, int step) {
        if (m_phase != Phase::Live || pl != m_layer) return;
        Log::printf("live: level complete at tick {}!", step);
        std::lock_guard<std::mutex> lock(m_mx);
        m_macro.clear();
        m_macroTicks = static_cast<int>(m_plan.size());
        appendToggles(m_macro, m_plan);
    }

    // planned positions between two ticks, for the overlay
    std::vector<cocos2d::CCPoint> SimPlanner::plannedPath(int fromTick, int toTick) const {
        std::vector<cocos2d::CCPoint> out;
        std::lock_guard<std::mutex> lock(m_pathMx);
        auto const& path = m_pathFrozen.load() ? m_frozenPath : m_path;
        int const count = static_cast<int>(path.size());
        fromTick = std::max(0, fromTick);
        toTick = std::min(toTick, count - 1);
        for (int i = fromTick; i <= toTick; ++i) out.push_back(path[static_cast<size_t>(i)]);
        return out;
    }

}
