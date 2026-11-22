// F6 recorder: writes the real player state and moving objects tick by tick to recordings/,
// the ground truth used to check the simulator.

#include "recorder.hpp"
#include "engine.hpp"
#include "settings.hpp"
#include "simplanner.hpp"
#include "log.hpp"
#include "world_export.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>

using namespace geode::prelude;

namespace ap {

    namespace {
        //persistent item counters as "id,count,id,count...", sorted
        std::string persistItems(PlayLayer* pl) {
            std::vector<std::pair<int, int>> items;
            if (pl && pl->m_effectManager) {
                for (auto const& [id, count] : pl->m_effectManager->m_persistentItemCountMap) {
                    items.emplace_back(id, count);
                }
            }
            std::sort(items.begin(), items.end());
            std::string out;
            for (auto const& [id, count] : items) out += fmt::format("{}{},{}", out.empty() ? "" : ",", id, count);
            return out;
        }

        // persistent timer ids as "id,id...", sorted
        std::string persistTimers(PlayLayer* pl) {
            std::vector<int> timers;
            if (pl && pl->m_effectManager) {
                for (int id : pl->m_effectManager->m_persistentTimerItemSet) timers.push_back(id);
            }
            std::sort(timers.begin(), timers.end());
            std::string out;
            for (int id : timers) out += fmt::format("{}{}", out.empty() ? "" : ",", id);
            return out;
        }

        // name usable in a file name
        std::string safeName(std::string const& name) {
            std::string out;
            for (char c : name) {
                out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
            }
            return out.empty() ? "level" : out;
        }

        // "<name>_<id>" of the level
        std::string levelTag(PlayLayer* pl) {
            std::string name = pl->m_level ? std::string(pl->m_level->m_levelName) : "level";
            int id = pl->m_level ? static_cast<int>(pl->m_level->m_levelID.value()) : 0;
            return fmt::format("{}_{}", safeName(name), id);
        }

        // local date and time for file names
        std::string nowStamp() {
            std::time_t now = std::time(nullptr);
            char buffer[32];
            std::strftime(buffer, sizeof buffer, "%Y%m%d_%H%M%S", std::localtime(&now));
            return buffer;
        }

        // unique id of the object, 0 if null
        int uid(GameObject* object) { return object ? object->m_uniqueID : 0; }

        // one letter per game mode, C for cube
        char modeChar(PlayerObject* p) {
            if (p->m_isShip) return 'S';
            if (p->m_isBall) return 'B';
            if (p->m_isBird) return 'U';
            if (p->m_isDart) return 'W';
            if (p->m_isRobot) return 'R';
            if (p->m_isSpider) return 'P';
            if (p->m_isSwing) return 'G';
            return 'C';
        }

        // formats a float with enough digits to read back the exact value
        std::string f32(float value) { return fmt::format("{:.9g}", value); }

        // formats a double with full precision
        std::string f64(double value) { return fmt::format("{:.17g}", value); }
    }

    // the only recorder
    Recorder& Recorder::get() {
        static Recorder r;
        return r;
    }

    // starts or stops recording, starting mid level records the level and opens a file at once
    void Recorder::toggle(PlayLayer* pl) {
        if (m_enabled) {
            endAttempt("stopped");
            m_enabled = false;
            Notification::create(
                    fmt::format("Auto Pathing Tool: recording stopped ({} file(s), {} ticks)", m_files, m_totalRows),
                    NotificationIcon::Info)
                    ->show();
            Log::printf("recording stopped: {} file(s), {} ticks in total.", m_files, m_totalRows);
            return;
        }
        m_enabled = true;
        m_stamp = nowStamp();
        m_attempt = 0;
        m_files = 0;
        m_totalRows = 0;
        if (pl) {
            dumpLevel(pl);
            beginAttempt(pl);
        }
        Notification::create(fmt::format("Auto Pathing Tool: recording ({} to stop)", keyName(Action::Record)),
                             NotificationIcon::Info)
                ->show();
        Log::print("recording started: play normally, each attempt gives one file.");
    }

    // new level, the watcher and the level dump start over
    void Recorder::onLevelLoaded(PlayLayer* pl) {
        clearHistory(true);
        m_watch.reset(pl);
        m_levelDumped = false;
        if (m_file.is_open()) endAttempt("level reloaded");
        if (m_enabled && pl) dumpLevel(pl);
    }

    // level exited, closes the file
    void Recorder::onLayerGone() {
        clearHistory(false);
        if (m_file.is_open()) endAttempt("level exited");
        m_levelDumped = false;
    }

    // saves the random generators before the reset draws from them
    void Recorder::onPreReset(PlayLayer* pl) {
        (void)pl;
        m_rng0 = Engine::rng();
        m_hasRng0 = true;
        m_tp0 = Engine::tpRng();
        m_hasTp0 = true;
    }

    // one file per attempt
    void Recorder::onReset(PlayLayer* pl) {
        clearHistory(true);
        if (!m_enabled) return;
        if (m_file.is_open()) endAttempt("reset");
        beginAttempt(pl);
    }

    // writes the objects moved on the death tick, then the death line, and closes the file
    void Recorder::onDeath(PlayLayer* pl, PlayerObject* player, GameObject* killer, int step) {
        if (!m_enabled || !m_file.is_open()) return;
        if (pl && m_snapshotDone) {
            m_events.clear();
            m_watch.sample(step + 1, m_events, false, pl->m_player1 ? pl->m_player1->getPosition() : cocos2d::CCPoint{},
                           kObjWindow);
            if (!m_events.empty()) {
                m_file << fmt::format(R"({{"ev":"obj","t":{},"o":[)", step + 1);
                for (size_t i = 0; i < m_events.size(); ++i) {
                    auto const& event = m_events[i];
                    m_file << fmt::format("{}[{},{},{},{},{},{},{}]", i ? "," : "", event.uid, f32(event.x),
                                          f32(event.y), f32(event.rot), event.flags, f32(event.nx), f32(event.ny));
                }
                m_file << "]}\n";
                m_objEvents += static_cast<int>(m_events.size());
            }
        }
        bool held = false;
        if (auto* p1 = pl->m_player1) {
            if (auto it = p1->m_holdingButtons.find(1); it != p1->m_holdingButtons.end()) held = it->second;
        }
        m_file << fmt::format(R"({{"ev":"death","t":{},"p":{},"killer":{},"killerId":{},"x":{},"y":{},"h":{}}})"
                              "\n",
                              step, (player == pl->m_player2) ? 2 : 1, uid(killer), killer ? killer->m_objectID : 0,
                              f32(player ? player->m_position.x : 0.f), f32(player ? player->m_position.y : 0.f),
                              held ? 1 : 0);
        endAttempt("death");
    }

    // writes the complete line and close the file
    void Recorder::onComplete(int step) {
        if (!m_enabled || !m_file.is_open()) return;
        m_file << fmt::format(R"({{"ev":"complete","t":{}}})"
                              "\n",
                              step);
        endAttempt("level complete");
    }

    // ,"pre":[tick,buttons,...] so the verifier can replay the attempt from tick 0, empty if not needed
    std::string Recorder::preInputs(PlayLayer* pl) const {
        if (!m_histValid || m_hist.size() < 2 || m_hist[0] != 0 || pl->m_isPracticeMode) return {};
        if (m_hist.size() == 2 && Engine::get().curStep() <= 0) return {};
        std::string out = R"(,"pre":[)";
        for (size_t i = 0; i < m_hist.size(); ++i) out += fmt::format("{}{}", i ? "," : "", m_hist[i]);
        return out + "]";
    }

    //opens the file of a new attempt and writes the starting state of the level and players
    void Recorder::beginAttempt(PlayLayer* pl) {
        if (!pl) return;
        ++m_attempt;
        auto dir = Mod::get()->getSaveDir() / "recordings";
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        m_path = dir / fmt::format("{}_{}_a{}.jsonl", levelTag(pl), m_stamp, m_attempt);
        m_file.open(m_path, std::ios::out | std::ios::trunc);
        m_rows = 0;
        m_snapshotDone = false;
        m_objEvents = 0;
        if (m_watch.candidates() == 0) m_watch.reset(pl);
        if (!m_file.is_open()) {
            Log::printf("recording: cannot open {}", geode::utils::string::pathToString(m_path));
            return;
        }
        auto* p = pl->m_player1;
        auto* p2 = pl->m_player2;
        m_file << fmt::format(
                R"({{"ev":"start","level":"{}","levelId":{},"attempt":{},"length":{},"dual":{},"practice":{})"
                R"(,"platformer":{},"cbs":{},"cos":{},"startX":{},"startY":{},"groundP1":{},"gravity":{})"
                R"(,"yStart":{},"speed":{},"vehicleSize":{},"objWin":{},"p2snap":{},"p2og3":{},"p2mf":{})"
                R"(,"p2prr":{},"p2fsy":{},"rng":{},"gatt":{},"pitems":[{}],"ptimers":[{}]{}}})"
                "\n",
                pl->m_level ? safeName(std::string(pl->m_level->m_levelName)) : "",
                pl->m_level ? static_cast<int>(pl->m_level->m_levelID.value()) : 0, m_attempt, f32(pl->m_levelLength),
                pl->m_gameState.m_isDualMode, pl->m_isPracticeMode, pl->m_isPlatformer, pl->m_clickBetweenSteps,
                pl->m_clickOnSteps, f32(p ? p->m_position.x : 0.f), f32(p ? p->m_position.y : 0.f),
                f32(pl->getGroundHeightForMode(0)), f64(p ? p->m_gravity : 0.0), f64(p ? p->m_yStart : 0.0),
                f32(p ? p->m_playerSpeed : 0.f), f32(p ? p->m_vehicleSize : 1.f), kObjWindow,
                f64(p2 ? p2->m_snapDistance : 0.0), p2 ? static_cast<int>(p2->m_isOnGround3) : 0,
                p2 ? static_cast<int>(p2->m_maybeIsFalling) : 0, p2 ? static_cast<int>(p2->m_padRingRelated) : 0,
                f32(p2 ? p2->m_fallStartY : 0.f), static_cast<uint32_t>(Engine::rng()), pl->m_attempts,
                persistItems(pl), persistTimers(pl),
                (m_hasRng0 ? fmt::format(R"(,"rng0":{})", static_cast<uint32_t>(m_rng0)) : std::string()) +
                        fmt::format(R"(,"mp":{})", (Engine::get().playing() ||
                                                    ap::SimPlanner::get().phase() == ap::SimPlanner::Phase::Live)
                                                           ? 1
                                                           : 0) +
                        fmt::format(R"(,"rngs":{})", static_cast<uint32_t>(Engine::attemptSeed(pl))) +
                        fmt::format(R"(,"tprng":{})", static_cast<uint32_t>(m_hasTp0 ? m_tp0 : Engine::tpRng())) +
                        preInputs(pl));
        m_hasRng0 = false;
        m_hasTp0 = false;
    }

    // writes the end line with the reason and closes the file
    void Recorder::endAttempt(char const* why) {
        if (!m_file.is_open()) return;
        m_file << fmt::format(R"({{"ev":"end","why":"{}","rows":{},"objEvents":{},"objCandidates":{}}})"
                              "\n",
                              why, m_rows, m_objEvents, m_watch.candidates());
        m_file.close();
        ++m_files;
        m_totalRows += m_rows;
        Log::printf("recording: {} ticks -> {} ({})", m_rows, geode::utils::string::pathToString(m_path.filename()),
                    why);
    }

    // tracks the inputs, then writes the player lines and the moved objects of this tick
    void Recorder::onTick(PlayLayer* pl, int step, bool lastTick) {
        if (m_hist.size() >= 2 && step < m_hist[m_hist.size() - 2]) clearHistory(step == 0);
        if (pl && m_histValid) {
            auto heldOf = [](PlayerObject* p) {
                if (!p) return false;
                auto it = p->m_holdingButtons.find(1);
                return it != p->m_holdingButtons.end() && it->second;
            };
            int const buttons =
                    (heldOf(pl->m_player1) ? 1 : 0) | ((pl->m_gameState.m_isDualMode && heldOf(pl->m_player2)) ? 2 : 0);
            if (buttons != m_histLast) {
                m_hist.push_back(step);
                m_hist.push_back(buttons);
                m_histLast = buttons;
            }
        }
        if (!m_enabled || !m_file.is_open() || !pl) return;
        if (pl->m_player1) writeRow(pl, pl->m_player1, step, 1, lastTick);
        if (pl->m_gameState.m_isDualMode && pl->m_player2) writeRow(pl, pl->m_player2, step, 2, lastTick);
        ++m_rows;
        m_events.clear();
        m_watch.sample(step, m_events, !m_snapshotDone,
                       pl->m_player1 ? pl->m_player1->getPosition() : cocos2d::CCPoint{}, kObjWindow);
        m_snapshotDone = true;
        if (!m_events.empty()) {
            m_file << fmt::format(R"({{"ev":"obj","t":{},"o":[)", step);
            for (size_t i = 0; i < m_events.size(); ++i) {
                auto const& event = m_events[i];
                m_file << fmt::format("{}[{},{},{},{},{},{},{}]", i ? "," : "", event.uid, f32(event.x), f32(event.y),
                                      f32(event.rot), event.flags, f32(event.nx), f32(event.ny));
            }
            m_file << "]}\n";
            m_objEvents += static_cast<int>(m_events.size());
        }
    }

    // writes the full state of one player for this tick, the keys are read by sim/recording.cpp
    void Recorder::writeRow(PlayLayer* pl, PlayerObject* p, int step, int which, bool lastTick) {
        bool held = false;
        if (auto it = p->m_holdingButtons.find(1); it != p->m_holdingButtons.end()) held = it->second;
        unsigned flags = 0;
        auto bit = [&](bool value, int index) {
            if (value) flags |= (1u << index);
        };
        bit(p->m_isOnGround, 0);
        bit(p->m_isOnGround2, 1);
        bit(p->m_isOnGround3, 2);
        bit(p->m_isOnGround4, 3);
        bit(p->m_isOnSlope, 4);
        bit(p->m_wasOnSlope, 5);
        bit(p->m_isUpsideDown, 6);
        bit(p->m_isDashing, 7);
        bit(p->m_isAccelerating, 8);
        bit(p->m_jumpBuffered, 9);
        bit(p->m_stateRingJump, 10);
        bit(p->m_stateJumpBuffered != 0, 11);
        bit(p->m_wasJumpBuffered, 12);
        bit(p->m_touchedRing, 13);
        bit(p->m_touchedPad, 14);
        bit(p->m_maybeIsBoosted, 15);
        bit(p->m_isLocked, 16);
        bit(p->m_isSliding, 17);
        bit(p->m_isDead, 18);
        bit(p->m_ringJumpRelated, 19);
        bit(p->m_hasEverJumped, 20);
        bit(p->m_isGoingLeft, 21);
        bit(p->m_isSideways, 22);
        bit(p->m_wasRobotJump, 23);
        bit(p->m_touchedCustomRing, 24);
        bit(p->m_touchedGravityPortal, 25);
        bit(p->m_isCollidingWithSlope, 26);
        bit(p->m_stateRingJump2, 27);
        bit(p->m_padRingRelated, 28);
        bit(p->m_maybeIsFalling, 29);
        bit(p->m_isCurrentSlopeTop, 30);
        bit(p->m_hasEverHitRing, 31);

        auto pos = p->getPosition();
        m_file << fmt::format(
                R"({{"t":{},"p":{},"h":{},"x":{},"y":{},"nx":{},"ny":{},"rot":{},"yv":{},"xs":{},"sm":{},"g":{})"
                R"(,"ys":{},"gm":{},"vs":{},"m":"{}","f":{},"tt":{},"ljt":{},"llt":{},"lft":{},"sa":{},"sv":{})"
                R"(,"sar":{},"csl":{},"lgo":{},"col":{},"snap":{},"ost":{},"fs":{},"yvbs":{},"ctmy":{},"cbmy":{})"
                R"(,"clmx":{},"crmx":{},"lap":{},"gyv":{},"csyv":{},"tw":{},"ta":{},"dual":{},"hb":{},"pw":{})"
                R"(,"ph":{},"rs":{},"aos":{},"fsy":{},"lpx":{},"lpy":{},"ci":{},"so":{},"sna":{},"shh":{})"
                R"(,"sfg":{},"sds":{},"ucol":[{},{},{},{}],"unk":[{},{}],"ia":{},"rj":{},"cz":{},"coy":{},"cs":{})"
                R"(,"pmin":{},"pmax":{},"zc":{},"cam":[{},{},{},{},{},{},{},{}],"rng":{},"lt":{}}})"
                "\n",
                step, which, held ? 1 : 0, f32(p->m_position.x), f32(p->m_position.y), f32(pos.x), f32(pos.y),
                f32(p->getRotation()), f64(p->m_yVelocity), f32(p->m_playerSpeed), f64(p->m_speedMultiplier),
                f64(p->m_gravity), f64(p->m_yStart), f32(p->m_gravityMod), f32(p->m_vehicleSize), modeChar(p), flags,
                f64(p->m_totalTime), f64(p->m_lastJumpTime), f64(p->m_lastLandTime), f64(p->m_lastFlipTime),
                f32(p->m_slopeAngle), f32(p->m_slopeVelocity), f32(p->m_slopeAngleRadians), uid(p->m_currentSlope),
                uid(p->m_lastGroundObject), uid(p->m_collidedObject), f64(p->m_snapDistance), uid(p->m_objectSnappedTo),
                f64(p->m_fallSpeed), f64(p->m_yVelocityBeforeSlope), f64(p->m_collidedTopMinY),
                f64(p->m_collidedBottomMaxY), f64(p->m_collidedLeftMaxX), f64(p->m_collidedRightMinX),
                uid(p->m_lastActivatedPortal), f64(p->m_groundYVelocity), f64(p->m_currentSlopeYVelocity),
                f32(pl->m_gameState.m_timeWarp), p->m_touchingRings ? p->m_touchingRings->count() : 0,
                pl->m_gameState.m_isDualMode ? 1 : 0, f32(p->m_unkAngle1), f32(p->m_width), f32(p->m_height),
                f32(p->m_rotationSpeed), f64(p->m_accelerationOrSpeed), f32(p->m_fallStartY), f32(p->m_lastPortalPos.x),
                f32(p->m_lastPortalPos.y), pl->m_gameState.m_commandIndex, p->m_stateOnGround, p->m_stateNoAutoJump,
                p->m_stateHitHead, p->m_stateFlipGravity, p->m_stateDartSlide, p->m_lastCollisionBottom,
                p->m_lastCollisionTop, p->m_lastCollisionLeft, p->m_lastCollisionRight, p->m_unk50C, p->m_unk510,
                p->m_isAccelerating ? 1 : 0, p->m_ringJumpRelated ? 1 : 0, f32(pl->m_gameState.m_cameraZoom),
                f32(pl->m_gameState.m_cameraOffset.y),
                (pl->m_gameState.m_unkPoint17.y != 0.f && !pl->m_gameState.m_unkBool1) ? 1 : 0,
                f32(pl->getMinPortalY()), f32(pl->getMaxPortalY()),
                pl->m_gameState.m_cameraZoom != pl->m_gameState.m_targetCameraZoom ? 1 : 0,
                f32(pl->m_gameState.m_cameraPosition.x), f32(pl->m_gameState.m_cameraPosition.y),
                f32(pl->m_gameState.m_cameraStepDiff.x), f32(pl->m_gameState.m_cameraStepDiff.y),
                f32(pl->m_gameState.m_cameraOffset.x), f32(pl->m_gameState.m_unkPoint17.x),
                f32(pl->m_gameState.m_unkPoint17.y), f32(pl->m_gameState.m_cameraAngle),
                static_cast<uint32_t>(Engine::rng()), lastTick ? 1 : 0);
    }

    // exports the level objects once per level
    void Recorder::dumpLevel(PlayLayer* pl) {
        if (m_levelDumped || !pl || !pl->m_objects) return;
        m_levelDumped = true;
        auto dir = Mod::get()->getSaveDir() / "levels";
        auto const tag = levelTag(pl);
        sim::World world = buildWorld(pl);
        auto result = writeWorld(world, pl, dir, tag);
        if (result) {
            Log::printf("level exported: {} objects -> levels/{}.objects.jsonl (+ levelstring)", world.objs.size(),
                        tag);
        } else
            Log::printf("level: export failed ({})", result.unwrapErr());
    }

    //status line text while recording
    std::string Recorder::statusText() const {
        if (!m_enabled) return "";
        return fmt::format("Auto Pathing Tool: debug recording - attempt {}, {} ticks ({} files)", m_attempt, m_rows,
                           m_files);
    }

}
