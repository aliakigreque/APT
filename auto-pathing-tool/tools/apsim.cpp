// apsim: command line build of the simulator, to check it against recordings (verify), search a level outside
// the game (solve) and run the self tests (test, exit code 1 when a check fails) and the other debug commands.
// Build: g++ -std=c++20 -O2 -pthread -o apsim tools/apsim.cpp sim/*.cpp

#include <map>
#include <set>
#include <array>
#include "../sim/gd.hpp"
#include "../sim/recording.hpp"
#include "../sim/search.hpp"
#include "../sim/robust.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <random>
#include <string>

using namespace ap::sim;

namespace {

    // gamemode letter, same letters as the recordings (RecRow::mode)
    char modeChar(Player const& p) {
        if (p.isShip) return 'S';
        if (p.isBall) return 'B';
        if (p.isBird) return 'U';
        if (p.isDart) return 'W';
        if (p.isRobot) return 'R';
        if (p.isSpider) return 'P';
        if (p.isSwing) return 'G';
        return 'C';
    }

    // true if the flag is among the arguments
    bool hasFlag(int argc, char** argv, char const* flag) {
        for (int i = 2; i < argc; ++i)
            if (!std::strcmp(argv[i], flag)) return true;
        return false;
    }

    // fresh game on a loaded level, copies the level settings then resets
    void setupGame(Game& g, World const& w) {
        g.world = &w;
        g.twoPlayerMode = w.twoPlayer;
        g.disableCameraGridSnap = w.gridSnapOff;
        g.enable22Changes = w.enable22;
        g.startX = w.startX;
        g.startY = w.startY;
        if (char const* seed = debugOptions().seed) g.rngSeed = static_cast<uint32_t>(std::strtoul(seed, nullptr, 0));
        g.reset();
    }

    // frames like when the mod plays, legacyCadence > 0 uses the old fixed frames of that many steps
    void applyModFrames(World& w, int legacyCadence) {
        if (w.startLastMaxPortalY < 0.f) w.startLastMaxPortalY = 0.f;
        if (legacyCadence > 0) {
            w.modPlays = true;
            w.allVisible = false;
            w.frameCadence = legacyCadence;
            return;
        }
        w.useModFrames(kModCadence);
    }

    // copies a recorded row into the player, full = almost every field, otherwise only position/speed/button/time
    void applyRow(Player& p, RecRow const& row, bool full) {
        p.x = row.nx;
        p.y = row.ny;
        p.yVelocity = row.yv;
        p.holdingJump = row.held;
        p.totalTime = row.tt;
        if (!full) return;
        p.lastX = row.nx;
        p.lastY = row.ny;
        p.playerSpeed = row.xs;
        p.speedMultiplier = row.sm;
        p.gravity = row.g;
        p.yStart = row.ys;
        p.gravityMod = row.gm;
        p.vehicleSize = row.vs;
        p.spriteWidthScale = row.vs;
        p.spriteHeightScale = row.vs;
        p.isShip = row.mode == 'S';
        p.isBall = row.mode == 'B';
        p.isBird = row.mode == 'U';
        p.isDart = row.mode == 'W';
        p.isRobot = row.mode == 'R';
        p.isSpider = row.mode == 'P';
        p.isSwing = row.mode == 'G';
        p.isOnGround = row.flag(RecRow::OnGround);
        p.isOnGround2 = row.flag(RecRow::OnGround2);
        p.isOnGround3 = row.flag(RecRow::OnGround3);
        p.isOnGround4 = row.flag(RecRow::OnGround4);
        p.isOnSlope = row.flag(RecRow::OnSlope);
        p.wasOnSlope = row.flag(RecRow::WasOnSlope);
        p.isUpsideDown = row.flag(RecRow::UpsideDown);
        p.isDashing = row.flag(RecRow::Dashing);
        p.isAccelerating = row.flag(RecRow::Accelerating);
        p.jumpBuffered = row.flag(RecRow::JumpBuffered);
        p.stateRingJump = row.flag(RecRow::StateRingJump);
        p.stateJumpBuffered = row.flag(RecRow::StateJumpBuffered) ? 1 : 0;
        p.wasJumpBuffered = row.flag(RecRow::WasJumpBuffered);
        p.touchedRing = row.flag(RecRow::TouchedRing);
        p.touchedPad = row.flag(RecRow::TouchedPad);
        p.maybeIsBoosted = row.flag(RecRow::MaybeBoosted);
        p.isLocked = row.flag(RecRow::Locked);
        p.isSliding = row.flag(RecRow::Sliding);
        p.isDead = row.flag(RecRow::Dead);
        p.ringJumpRelated = row.flag(RecRow::RingJumpRelated);
        p.hasEverJumped = row.flag(RecRow::HasEverJumped);
        p.isGoingLeft = row.flag(RecRow::GoingLeft);
        p.isSideways = row.flag(RecRow::Sideways);
        p.wasRobotJump = row.flag(RecRow::WasRobotJump);
        p.touchedCustomRing = row.flag(RecRow::TouchedCustomRing);
        p.touchedGravityPortal = row.flag(RecRow::TouchedGravityPortal);
        p.isCollidingWithSlope = row.flag(RecRow::CollidingWithSlope);
        p.stateRingJump2 = row.flag(RecRow::StateRingJump2);
        p.padRingRelated = row.flag(RecRow::PadRingRelated);
        p.maybeIsFalling = row.flag(RecRow::MaybeFalling);
        p.isCurrentSlopeTop = row.flag(RecRow::CurrentSlopeTop);
        p.hasEverHitRing = row.flag(RecRow::HasEverHitRing);
        p.lastJumpTime = row.ljt;
        p.lastLandTime = row.llt;
        p.lastFlipTime = row.lft;
        p.slopeAngle = row.sa;
        p.slopeVelocity = row.sv;
        p.slopeAngleRadians = row.sar;
        p.currentSlope = row.csl;
        p.lastGroundObject = row.lgo;
        p.collidedObject = row.col;
        p.objectSnappedTo = row.ost;
        p.lastActivatedPortal = row.lap;
        p.snapDistance = row.snap;
        p.fallSpeed = row.fs;
        p.yVelocityBeforeSlope = row.yvbs;
        p.collidedTopMinY = row.ctmy;
        p.collidedBottomMaxY = row.cbmy;
        p.collidedLeftMaxX = row.clmx;
        p.collidedRightMinX = row.crmx;
        p.groundYVelocity = row.gyv;
        p.currentSlopeYVelocity = row.csyv;
        p.hitboxSize = row.hb;
        p.width = row.pw;
        p.height = row.ph;
        p.rotationSpeed = row.rs;
        p.accelerationOrSpeed = row.aos;
        p.fallStartY = row.fsy;
        p.lastPortalX = row.lpx;
        p.lastPortalY = row.lpy;
        p.stateOnGround = row.so;
        p.stateNoAutoJump = row.sna;
        p.stateHitHead = row.shh;
        p.stateFlipGravity = row.sfg;
        p.stateDartSlide = row.sds;
        p.lastCollisionBottom = row.ucol[0];
        p.lastCollisionTop = row.ucol[1];
        p.lastCollisionLeft = row.ucol[2];
        p.lastCollisionRight = row.ucol[3];
        p.unk50C = row.unk[0];
        p.unk510 = row.unk[1];
        p.rotation = row.rot;
        p.touchingRings.clear();
    }

    struct Diff {
        std::string what;
        double sim = 0;
        double real = 0;
        bool cosmetic = false;
    };

    // compares the sim player with the next recorded row, returns true if there is no hard difference
    bool compareRow(Player const& p, RecRow const& row, std::vector<Diff>& diffs, double tolPos, double tolVel) {
        auto num = [&](char const* what, double a, double b, double tol) {
            if (std::fabs(a - b) > tol) diffs.push_back({what, a, b, false});
        };
        auto soft = [&](char const* what, double a, double b, double tol) {
            if (std::fabs(a - b) > tol) diffs.push_back({what, a, b, true});
        };
        auto flag = [&](char const* what, bool a, bool b) {
            if (a != b) diffs.push_back({what, a ? 1.0 : 0.0, b ? 1.0 : 0.0});
        };
        num("x", p.x, row.nx, tolPos);
        num("y", p.y, row.ny, tolPos);
        num("yVelocity", p.yVelocity, row.yv, tolVel);
        num("playerSpeed", p.playerSpeed, row.xs, 1e-6);
        num("vehicleSize", p.vehicleSize, row.vs, 1e-6);
        num("gravityMod", p.gravityMod, row.gm, 1e-6);
        num("totalTime", p.totalTime, row.tt, 1e-4);
        if (modeChar(p) != row.mode) diffs.push_back({std::string("mode ") + modeChar(p) + " vs " + row.mode, 0, 0});
        flag("isOnGround", p.isOnGround, row.flag(RecRow::OnGround));
        flag("isOnGround2", p.isOnGround2, row.flag(RecRow::OnGround2));
        flag("isOnGround3", p.isOnGround3, row.flag(RecRow::OnGround3));
        flag("isUpsideDown", p.isUpsideDown, row.flag(RecRow::UpsideDown));
        flag("isDashing", p.isDashing, row.flag(RecRow::Dashing));
        flag("isAccelerating", p.isAccelerating, row.flag(RecRow::Accelerating));
        flag("jumpBuffered", p.jumpBuffered, row.flag(RecRow::JumpBuffered));
        flag("stateRingJump", p.stateRingJump, row.flag(RecRow::StateRingJump));
        flag("touchedPad", p.touchedPad, row.flag(RecRow::TouchedPad));
        flag("maybeIsBoosted", p.maybeIsBoosted, row.flag(RecRow::MaybeBoosted));
        flag("isOnSlope", p.isOnSlope, row.flag(RecRow::OnSlope));
        flag("wasOnSlope", p.wasOnSlope, row.flag(RecRow::WasOnSlope));
        flag("padRingRelated", p.padRingRelated, row.flag(RecRow::PadRingRelated));
        flag("maybeIsFalling", p.maybeIsFalling, row.flag(RecRow::MaybeFalling));
        flag("isCollidingWithSlope", p.isCollidingWithSlope, row.flag(RecRow::CollidingWithSlope));
        num("collidedTopMinY", p.collidedTopMinY, row.ctmy, 1e-3);
        num("collidedBottomMaxY", p.collidedBottomMaxY, row.cbmy, 1e-3);
        num("lastGroundObject", p.lastGroundObject, row.lgo, 0);
        soft("lastActivatedPortal", p.lastActivatedPortal, row.lap, 0);
        num("objectSnappedTo", p.objectSnappedTo, row.ost, 0);
        soft("snapDistance", p.snapDistance, row.snap, 1e-3);
        soft("fallSpeed", p.fallSpeed, row.fs, tolVel);
        num("hitboxSize", p.hitboxSize, row.hb, 1e-6);
        num("width", p.width, row.pw, 1e-6);
        num("accelerationOrSpeed", p.accelerationOrSpeed, row.aos, 1e-6);
        num("fallStartY", p.fallStartY, row.fsy, tolPos);
        num("stateOnGround", p.stateOnGround, row.so, 0);
        num("stateNoAutoJump", p.stateNoAutoJump, row.sna, 0);
        num("stateHitHead", p.stateHitHead, row.shh, 0);
        num("stateFlipGravity", p.stateFlipGravity, row.sfg, 0);
        num("unk50C", p.unk50C, row.unk[0], 0);
        num("unk510", p.unk510, row.unk[1], 0);
        num("touchingRings", static_cast<double>(p.touchingRings.size()), row.ta, 0);
        soft("rotation", p.rotation, row.rot, 0.5);
        bool hard = false;
        for (auto const& d : diffs) hard = hard || !d.cosmetic;
        return !hard;
    }

    // builds the camera track and moving objects timeline from a recording, attaches it if not empty
    bool timelineFromRecording(Recording const& rec, World& w, Timeline& tl, int tlOffset) {
        for (auto const& row : rec.rows) {
            if (row.p != 1 || row.cz <= 0.f) continue;
            CamKey k;
            k.tick = row.t;
            k.zoom = row.cz;
            k.offsetY = row.coy;
            k.timeWarp = row.tw;
            k.zoomChanging = row.zc;
            k.staticMode = row.cs;
            k.minPortalY = row.pmin;
            k.maxPortalY = row.pmax;
            if (!tl.cam.empty()) {
                auto const& last = tl.cam.back();
                if (last.zoom == k.zoom && last.offsetY == k.offsetY && last.timeWarp == k.timeWarp &&
                    last.zoomChanging == k.zoomChanging && last.staticMode == k.staticMode &&
                    last.minPortalY == k.minPortalY && last.maxPortalY == k.maxPortalY)
                    continue;
            }
            tl.cam.push_back(k);
        }
        for (auto const& row : rec.rows)
            if (row.p == 1) tl.xTicks.emplace_back(row.t, row.nx);
        if (rec.objEvents.empty() && tl.cam.empty()) return false;
        tl.build(rec.objEvents, w);
        tl.tickOffset = tlOffset;
        w.attach(&tl);
        std::printf("timeline : %zu moving objects, %zu events, ticks %d..%d (offset %d) ; camera track : %zu keys\n",
                    tl.objs.size(), rec.objEvents.size(), tl.firstTick, tl.lastTick, tl.tickOffset, tl.cam.size());
        return true;
    }

    // prints what the sim can do with the triggers of this level
    void printTriggerStatus(World const& w) {
        if (!w.hasTriggerData) {
            std::printf("triggers : no data in the export (recorded timeline only)\n");
            return;
        }
        size_t triggerCount = 0;
        size_t positionalCount = 0;
        for (auto const& o : w.objs)
            if (o.cls == 1 && o.trigger) ++triggerCount;
        for (auto const& e : w.spawnLists) positionalCount += e.second.size();
        std::printf("triggers : simulated (%zu triggers, %zu positional, %d groups, %zu dynamic blocks)\n",
                    triggerCount, positionalCount, w.maxGroup, w.dynamicBlocks.size());
        if (!w.kfOptGroups.empty()) {
            size_t optimizedCount = 0;
            for (auto const& o : w.objs)
                if (o.kfOpt) ++optimizedCount;
            std::printf("optimized moving keyframes : %zu in %zu groups (moved at the end of the frame)\n",
                        optimizedCount, w.kfOptGroups.size());
        }
        if (!w.unsupported.empty()) {
            std::printf("not supported (ignored) :");
            for (auto const& u : w.unsupported) std::printf(" %s", u.c_str());
            std::printf("\n");
        }
        if (!w.cameraUnsupported.empty()) {
            std::printf("camera : not simulated (");
            for (auto const& u : w.cameraUnsupported) std::printf(" %s", u.c_str());
            std::printf(" ) : flying mode limits taken from the recorded camera track\n");
        }
        if (!w.approximations.empty()) {
            std::printf("approximations :");
            for (auto const& u : w.approximations) std::printf(" %s", u.c_str());
            std::printf("\n");
        }
    }

    struct ObjCmp {
        int ticksWithDiff = 0;
        int shown = 0;
        int maxShown = 12;
        int firstTick = -1;
        int watchUid = -1;
        float win = 0;
        struct Per {
            int first = -1;
            int last = -1;
            int n = 0;
            float maxD = 0;
            float maxR = 0;
            bool dis = false;
        };
        std::map<int, Per> per;
    };

    // compares each simulated moving object with its recorded state at this tick, updates cmp
    void compareObjects(Game& g, Timeline const& tl, int tick, ObjCmp& cmp) {
        if (!g.trig.on || tl.objs.empty()) return;
        if (cmp.watchUid >= 0) {
            GObj watched = g.objByUid(cmp.watchUid);
            if (watched && (tick % 10) == 0) {
                Point const p = watched.getRealPosition();
                Rect const r = watched.getObjectRect();
                std::printf(
                        "  [sim track] uid %d tick %d : (%.4f, %.4f) rot %.2f rect [%.2f %.2f %.2f %.2f] player (%.1f, "
                        "%.1f)\n",
                        cmp.watchUid, tick, p.x, p.y, watched.rotation(), r.x, r.y, r.w, r.h, g.p1.x, g.p1.y);
            }
        }
        int diffCount = 0;
        for (size_t d = 0; d < tl.objs.size(); ++d) {
            TimelineKey const& key = tl.at(static_cast<int>(d), tick - tl.tickOffset);
            if (key.tick == INT_MIN && tick - tl.tickOffset + tl.tickOffset < tl.firstTick) {}
            GObj o = g.obj(tl.objs[d].objIndex);
            Point const p = o.getRealPosition();
            if (p.x < g.p1.x - 1100.f && static_cast<float>(o.o->posX) < g.p1.x - 1100.f) continue;
            if (cmp.win > 0.f &&
                (std::fabs(key.x - g.p1.x) > cmp.win - 100.f || std::fabs(key.y - g.p1.y) > cmp.win - 100.f ||
                 std::fabs(p.x - g.p1.x) > cmp.win - 100.f || std::fabs(p.y - g.p1.y) > cmp.win - 100.f))
                continue;
            bool const simOff = o.disabled() || o.st().destroyed;
            bool const realOff = (key.flags & 3u) != 0;
            float const rot = o.rotation();
            bool diff = std::fabs(p.x - key.x) > 1e-3f || std::fabs(p.y - key.y) > 1e-3f || simOff != realOff ||
                        std::fabs(rot - key.rot) > 1e-2f;
            if (o.uid() == cmp.watchUid)
                std::printf(
                        "  [track] uid %d tick %d : sim (%.4f, %.4f) rot %.2f %s ; real (%.4f, %.4f) rot %.2f %s%s\n",
                        o.uid(), tick, p.x, p.y, rot, simOff ? "off" : "on", key.x, key.y, key.rot,
                        realOff ? "off" : "on", diff ? " MISMATCH" : "");
            if (!diff) continue;
            ++diffCount;
            {
                auto& stats = cmp.per[o.uid()];
                if (stats.first < 0) stats.first = tick;
                stats.last = tick;
                ++stats.n;
                stats.maxD = std::max(stats.maxD, std::max(std::fabs(p.x - key.x), std::fabs(p.y - key.y)));
                stats.maxR = std::max(stats.maxR, std::fabs(rot - key.rot));
                if (simOff != realOff) stats.dis = true;
            }
            if (cmp.shown < 12) {
                std::printf(
                        "  object uid %d (id %d) tick %d : sim (%.4f, %.4f) rot %.2f %s ; real (%.4f, %.4f) rot %.2f "
                        "%s\n",
                        o.uid(), o.o->id, tick, p.x, p.y, rot, simOff ? "off" : "on", key.x, key.y, key.rot,
                        realOff ? "off" : "on");
                ++cmp.shown;
            }
        }
        if (diffCount) {
            ++cmp.ticksWithDiff;
            if (cmp.firstTick < 0) cmp.firstTick = tick;
        }
    }

    void applyEnvContext(World& w);

    // "apsim info": trigger status and histogram of 2.2 / unsupported triggers
    int cmdInfo(int argc, char** argv) {
        if (argc < 3) {
            std::fprintf(stderr, "usage: apsim info <level.objects.jsonl>\n");
            return 2;
        }
        World w;
        std::string err;
        if (!loadLevel(argv[2], w, &err)) {
            std::fprintf(stderr, "level : %s\n", err.c_str());
            return 1;
        }
        applyEnvContext(w);
        std::printf("level : %zu objects, length %.1f, enable22 %d, platformer %d\n", w.objs.size(), w.length,
                    w.enable22 ? 1 : 0, w.platformer ? 1 : 0);
        printTriggerStatus(w);
        std::map<int, int> countById;
        for (auto const& o : w.objs)
            if (o.cls == 1 && (o.id >= 1900 || unsupportedReason(o))) ++countById[o.id];
        std::printf("triggers (id >= 1900 or not supported) :");
        for (auto const& [id, n] : countById)
            std::printf(" %d×%d%s", id, n, [&] {
                for (auto const& o : w.objs)
                    if (o.id == id && o.cls == 1) return unsupportedReason(o) ? "!" : "";
                return "";
            }());
        std::printf("\n");
        return 0;
    }

    // reads APSIM_ATTEMPTS and APSIM_PITEMS=item:value,... into the level
    void applyEnvContext(World& w) {
        if (debugOptions().attempts >= 0) {
            w.attempts = debugOptions().attempts;
            std::printf("game attempt (APSIM_ATTEMPTS) : %d\n", w.attempts);
        }
        char const* e = debugOptions().persistentItems;
        if (!e) return;
        std::string str(e);
        size_t start = 0;
        while (start < str.size()) {
            size_t end = str.find(',', start);
            if (end == std::string::npos) end = str.size();
            std::string const pair = str.substr(start, end - start);
            size_t const colon = pair.find(':');
            if (colon != std::string::npos)
                w.persistentItems.emplace_back(std::atoi(pair.substr(0, colon).c_str()),
                                               std::atoi(pair.substr(colon + 1).c_str()));
            start = end + 1;
        }
        std::sort(w.persistentItems.begin(), w.persistentItems.end());
        std::printf("persistent items (APSIM_PITEMS) :");
        for (auto const& p : w.persistentItems) std::printf(" %d=%d", p.first, p.second);
        std::printf("\n");
    }

    // attempt context from a recording (game attempts, teleport rng, persistent items)
    void applyAttemptContext(Recording const& rec, World& w) {
        if (rec.gameAttempts >= 0) w.attempts = rec.gameAttempts;
        if (rec.hasTpRng) {
            w.tpRngKnown = true;
            w.tpRngSeed = rec.tpRng;
        }
        if (rec.hasPersist) {
            w.persistentItems = rec.persistentItems;
            w.persistentTimers.clear();
            for (int k : rec.persistentTimers) w.persistentTimers.emplace_back(k, 0.0);
            bool any = false;
            for (auto const& e : rec.persistentItems)
                if (e.second != 0) any = true;
            if (any) {
                std::printf("persistent items :");
                for (auto const& e : rec.persistentItems) std::printf(" %d=%d", e.first, e.second);
                std::printf("\n");
            }
        }
    }

    // game frames of the recording, used to place the optimized keyframes
    void applyFrames(Recording const& rec, World& w) {
        w.frameStarts = recordingFrameStarts(rec);
        if (w.kfOptGroups.empty()) return;
        bool hasLt = false;
        for (auto const& row : rec.rows) {
            if (row.lt) {
                hasLt = true;
                break;
            }
        }
        if (w.frameStarts.empty())
            std::printf("frames : unknown (optimized keyframes kept up to date every step)\n");
        else
            std::printf("frames : %zu (%s), %.2f steps on average ; optimized keyframes in %zu groups\n",
                        w.frameStarts.size(), hasLt ? "recorded" : "deduced from m_position",
                        static_cast<double>(rec.rows.size()) / static_cast<double>(w.frameStarts.size()),
                        w.kfOptGroups.size());
    }

    // "apsim verify": replays a recording tick by tick and compares the sim with the real game, 0 if identical
    int cmdVerify(int argc, char** argv) {
        if (argc < 4) {
            std::fprintf(stderr,
                         "usage: apsim verify <level.objects.jsonl> <recording.jsonl> [--max N] [--verbose] "
                         "[--continue] [--notrig] [--noclip] [--mod|--nomod]\n");
            return 2;
        }
        int maxDiffs = 10;
        int traceTick = -1;
        int traceEnd = -1;
        int tlOffset = 1;
        int watchUid = -1;
        int untilTick = INT_MAX;
        bool rngFree = false;
        bool spawnsFirst = true;
        bool verbose = false;
        bool keepGoing = false;
        for (int i = 4; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--max") && i + 1 < argc)
                maxDiffs = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--verbose"))
                verbose = true;
            else if (!std::strcmp(argv[i], "--continue"))
                keepGoing = true;
            else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc) {
                char const* arg = argv[++i];
                traceTick = std::atoi(arg);
                char const* dash = std::strchr(arg + 1, '-');
                traceEnd = dash ? std::atoi(dash + 1) : traceTick;
            } else if (!std::strcmp(argv[i], "--tloff") && i + 1 < argc)
                tlOffset = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--obj") && i + 1 < argc)
                watchUid = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--until") && i + 1 < argc)
                untilTick = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--rngfree"))
                rngFree = true;
            else if (!std::strcmp(argv[i], "--spawnsfirst"))
                spawnsFirst = true;
            else if (!std::strcmp(argv[i], "--nospawnsfirst"))
                spawnsFirst = false;
        }
        World w;
        std::string err;
        if (!loadLevel(argv[2], w, &err)) {
            std::fprintf(stderr, "level : %s\n", err.c_str());
            return 1;
        }
        applyEnvContext(w);
        bool noTrig = false;
        bool noclip = false;
        int modFrames = -1;
        for (int i = 4; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--notrig")) noTrig = true;
            if (!std::strcmp(argv[i], "--noclip")) noclip = true;
            if (!std::strcmp(argv[i], "--mod")) modFrames = 1;
            if (!std::strcmp(argv[i], "--nomod")) modFrames = 0;
        }
        if (noTrig) {
            w.hasTriggerData = false;
            w.build();
        }
        printTriggerStatus(w);
        Recording rec;
        if (!loadRecording(argv[3], rec, &err)) {
            std::fprintf(stderr, "recording : %s\n", err.c_str());
            return 1;
        }
        std::printf("level %s : %zu objects, length %.1f ; recording : %zu ticks, end \"%s\"\n", w.levelName.c_str(),
                    w.objs.size(), w.length, rec.rows.size(), rec.endWhy.c_str());
        if (rec.rows.empty()) return 1;
        applyAttemptContext(rec, w);
        if (modFrames < 0) modFrames = rec.modPlays == 2 ? 1 : 0;
        if (rec.modPlays >= 1)
            std::printf("recording made while the mod was playing%s\n",
                        rec.modPlays == 2 ? " (one step per game update)" : "");
        if (modFrames == 1) {
            w.useModFrames(kModCadence);
            std::printf("frames : the mod's (every step is a frame, first frame of %d steps)\n", kModCadence);
        } else {
            applyFrames(rec, w);
        }
        if (w.startLastMaxPortalY < 0.f) w.startLastMaxPortalY = 0.f;
        Timeline tl;
        timelineFromRecording(rec, w, tl, tlOffset);
        Game g;
        setupGame(g, w);

        uint32_t seed = 0;
        bool haveSeed = false;
        if (rec.hasRngSeed) {
            seed = rec.rngSeed;
            haveSeed = true;
        } else if (rec.hasRng) {
            seed = rec.rng;
            for (int k = g.resetDraws(); k > 0; --k) seed = Game::rngPrev(seed);
            haveSeed = true;
        }
        if (haveSeed && rec.rows[0].t == 0) {
            g.rngSeed = seed;
            g.reset();
        }
        rec.p2l.apply(g.p2);
        if (rec.hasRng && rec.rows[0].t > 0) g.rng = rec.rng;
        g.noclip = noclip;
        if (noclip)
            std::printf(
                    "noclip : the recording was made with a noclip mod (death without effect) ; the simulator does the "
                    "same\n");
        ObjCmp objCmp;
        objCmp.watchUid = watchUid;
        objCmp.win = rec.objWin;
        if (g.trig.on)
            std::printf("objects : simulated positions compared to the recorded timeline (%zu moving objects)\n",
                        tl.objs.size());

        bool const replayPre = rec.rows[0].t > 0 && !rec.preInputs.empty() && rec.preInputs[0].first == 0;
        if (replayPre) {
            int const firstTick = rec.rows[0].t;
            auto heldAt = [&](int t) -> uint8_t {
                if (t >= firstTick) {
                    uint8_t buttons = rec.rows[0].held ? 1 : 0;
                    for (auto const& row2 : rec.rows2)
                        if (row2.t == firstTick && row2.held && w.twoPlayer) buttons |= 2;
                    return buttons;
                }
                auto it = std::upper_bound(rec.preInputs.begin(), rec.preInputs.end(), t,
                                           [](int v, auto const& e) { return v < e.first; });
                return it == rec.preInputs.begin() ? 0 : std::prev(it)->second;
            };
            auto run = [&](Game& game, bool ignore) {
                setupGame(game, w);
                if (haveSeed) {
                    game.rngSeed = seed;
                    game.reset();
                }
                rec.p2l.apply(game.p2);
                game.noclip = noclip;
                game.ignoreDamage = ignore;
                while (game.tick < firstTick && !game.playerDied) stepTick(game, heldAt(game.tick + 1));
            };
            run(g, false);
            if (g.playerDied) {
                std::printf(
                        "(replay from tick 0 : the simulator dies at tick %d (object uid %d) before the recording "
                        "starts - earlier divergence ; replayed without death)\n",
                        g.tick, g.killerUid);
                run(g, true);
                g.ignoreDamage = false;
            }
            if (rec.hasRng && g.rng != rec.rng) {
                std::printf(
                        "(replay from tick 0 : different rng at tick %d - sim %u, real %u ; the real one is used)\n",
                        firstTick, static_cast<unsigned>(g.rng), static_cast<unsigned>(rec.rng));
                g.rng = rec.rng;
            }
            std::printf("(recording started at tick %d : attempt replayed from tick 0 with its %zu input change(s))\n",
                        firstTick, rec.preInputs.size());
            {
                std::vector<Diff> preDiffs;
                compareRow(g.p1, rec.rows[0], preDiffs, 1e-4, 1e-9);
                preDiffs.erase(std::remove_if(preDiffs.begin(), preDiffs.end(),
                                              [](Diff const& d) { return d.what == "totalTime"; }),
                               preDiffs.end());
                if (!preDiffs.empty()) {
                    std::printf("resync : %zu difference(s) at tick %d :\n", preDiffs.size(), firstTick);
                    for (auto const& d : preDiffs)
                        std::printf("    %-22s sim=%.6f real=%.6f\n", d.what.c_str(), d.sim, d.real);
                }
            }
        } else if (g.trig.on && rec.rows[0].t > 0) {
            Game fastForward;
            setupGame(fastForward, w);
            if (haveSeed) {
                fastForward.rngSeed = seed;
                fastForward.reset();
            }
            rec.p2l.apply(fastForward.p2);
            fastForward.ignoreDamage = true;
            fastForward.noclip = noclip;
            while (fastForward.tick < rec.rows[0].t) stepTick(fastForward, false);
            auto savedObjState = g.objState;
            g = fastForward;
            g.ignoreDamage = false;
            g.noclip = noclip;
            g.objState = savedObjState;
            for (auto const& e : fastForward.objState.items)
                if (isSpawnableTrigger(w.objs[static_cast<size_t>(e.first)].id)) g.objState[e.first] = e.second;
            if (rec.hasRng) g.rng = rec.rng;
            std::printf("(recording started at tick %d : triggers advanced without inputs until then)\n",
                        rec.rows[0].t);
        }
        RecRow const& firstRow = rec.rows[0];
        if (firstRow.t == 0) {
            std::vector<Diff> startDiffs;
            compareRow(g.p1, firstRow, startDiffs, 1e-4, 1e-9);
            if (!startDiffs.empty()) {
                std::printf("simulated start differs from the real start : %zu difference(s) :\n", startDiffs.size());
                for (auto const& d : startDiffs)
                    std::printf("    %-22s sim=%.6f real=%.6f\n", d.what.c_str(), d.sim, d.real);
            }
        }
        {
            float const lastDx = g.p1.x - g.p1.lastX;
            float const lastDy = g.p1.y - g.p1.lastY;
            applyRow(g.p1, firstRow, true);
            if (firstRow.t > 0) {
                g.p1.lastX = g.p1.x - lastDx;
                g.p1.lastY = g.p1.y - lastDy;
            }
        }
        g.levelTime = firstRow.tt;
        g.commandIndex = static_cast<unsigned>(firstRow.ci);
        g.tick = firstRow.t;
        bool resync = true;
        for (int i = 4; i < argc; ++i)
            if (!std::strcmp(argv[i], "--noresync")) resync = false;
        {
            std::vector<Diff> initialDiffs;
            compareRow(g.p1, firstRow, initialDiffs, 1e-4, 1e-9);
            if (!initialDiffs.empty()) {
                std::printf("initial state : %zu difference(s) before the first tick :\n", initialDiffs.size());
                for (auto const& d : initialDiffs)
                    std::printf("    %-22s sim=%.6f real=%.6f\n", d.what.c_str(), d.sim, d.real);
            }
        }
        int shown = 0;
        int cosmeticCount = 0;
        int cosmeticShown = 0;
        int firstDiv = -1;
        size_t row2Index = 0;
        if (!rec.rows2.empty())
            std::printf("dual : %zu player 2 rows (ticks %d..%d) compared too\n", rec.rows2.size(), rec.rows2.front().t,
                        rec.rows2.back().t);
        size_t const rowCount = rec.rows.size();
        int splitSteps = 0;
        std::unordered_map<int, bool> heldP2;
        if (w.twoPlayer)
            for (auto const& row2 : rec.rows2) heldP2[row2.t] = row2.held;
        auto buttonsOf = [&](RecRow const& row) -> uint8_t {
            uint8_t buttons = row.held ? 1 : 0;
            if (w.twoPlayer) {
                auto it = heldP2.find(row.t);
                if (it != heldP2.end() && it->second) buttons |= 2;
            }
            return buttons;
        };

        for (size_t i = 0; i + 1 < rowCount; ++i) {
            if (rec.rows[i].t > untilTick) {
                std::printf("[orb] stop at tick %d, player (%.1f, %.1f)\n", rec.rows[i].t, g.p1.x, g.p1.y);
                for (size_t k = 0; k < w.objs.size(); ++k) {
                    Obj const& obj = w.objs[k];
                    if (!obj.isRing() && !obj.isPad()) continue;
                    Point const realPos = g.obj(static_cast<int>(k)).getRealPosition();
                    double const px = realPos.x;
                    double const py = realPos.y;
                    if (std::fabs(px - g.p1.x) < 400.0 && std::fabs(py - g.p1.y) < 400.0)
                        std::printf("[orb] uid %d id %d (%.1f, %.1f) start (%.1f, %.1f)%s\n", obj.uid, obj.id, px, py,
                                    obj.x, obj.y, g.obj(static_cast<int>(k)).disabled() ? " disabled" : "");
                }
                break;
            }
            RecRow const& cur0 = rec.rows[i];
            RecRow const& cur = cur0;
            RecRow const& next = rec.rows[i + 1];
            uint8_t const button = buttonsOf(next);
            uint8_t const curHeld = buttonsOf(cur);
            int const gap = next.ci - cur.ci;
            int const splits = gap > 2 ? (gap - 2) / 2 : 0;
            g_trace = (traceTick >= 0 && cur.t >= traceTick && cur.t <= traceEnd);
            if (g_trace)
                std::printf("tick %d : collision trace (player x=%.3f y=%.3f yv=%.4f rot=%.2f mode=%c)\n", cur.t,
                            g.p1.x, g.p1.y, g.p1.yVelocity, g.p1.rotation, modeChar(g.p1));
            if (splits > 0) {
                stepTick(g, curHeld);
                for (int sidx = 0; sidx < splits; ++sidx) {
                    stepTickDt(g, curHeld, 0.125f, true);
                    stepTickDt(g, (sidx == splits - 1) ? button : curHeld, 0.125f, true);
                }
                ++splitSteps;
            } else {
                stepTick(g, button);
            }
            if (spawnsFirst && !g.died && !g.completed)
                stepPre(g, 0.25f * (g.timeWarp < 1.f ? g.timeWarp : 1.f), false);
            g_trace = false;
            compareObjects(g, tl, next.t, objCmp);
            if (next.hasRng && next.p == 1 && g.rng != next.rng) {
                static int rngReports = 0;
                if (rngReports++ < 5)
                    std::printf("rng : tick %d sim %u real %u%s\n", next.t, g.rng, next.rng,
                                g.world->hasRandom ? "" : " (level without random trigger)");
                if (!rngFree) g.rng = next.rng;
            }

            std::vector<Diff> diffs;
            bool onlyCosmetic = compareRow(g.p1, next, diffs, 1e-4, 1e-9) && !diffs.empty();
            RecRow const* next2 = nullptr;
            while (row2Index < rec.rows2.size() && rec.rows2[row2Index].t < next.t) ++row2Index;
            if (row2Index < rec.rows2.size() && rec.rows2[row2Index].t == next.t) next2 = &rec.rows2[row2Index];
            if (next2) {
                if (!g.isDualMode) {
                    diffs.push_back({"p2 : dual (sim) vs present (real)", 0, 1, false});
                    onlyCosmetic = false;
                } else {
                    std::vector<Diff> p2Diffs;
                    bool const p2NoHardDiff = compareRow(g.p2, *next2, p2Diffs, 1e-4, 1e-9);
                    for (auto& d : p2Diffs) {
                        d.what = "p2 " + d.what;
                        diffs.push_back(d);
                    }
                    if (!p2Diffs.empty() && !p2NoHardDiff)
                        onlyCosmetic = false;
                    else if (!p2Diffs.empty() && diffs.size() == p2Diffs.size())
                        onlyCosmetic = true;
                }
            } else if (g.isDualMode && !rec.rows2.empty() && next.t > rec.rows2.front().t &&
                       next.t < rec.rows2.back().t) {
                diffs.push_back({"p2 : present (sim) vs absent (real)", 1, 0, false});
                onlyCosmetic = false;
            }
            if (g.died && next.t != rec.deathTick && rec.deathTick >= 0 && static_cast<int>(i) + 1 < rec.deathTick) {
                diffs.push_back({"early death (sim)", static_cast<double>(g.killerUid),
                                 static_cast<double>(rec.deathKiller), false});
            }
            if (onlyCosmetic &&
                !(g.died && next.t != rec.deathTick && rec.deathTick >= 0 && static_cast<int>(i) + 1 < rec.deathTick)) {
                ++cosmeticCount;
                if (cosmeticShown < 3) {
                    ++cosmeticShown;
                    std::printf("tick %d : cosmetic mismatch", rec.rows[i].t);
                    for (auto const& d : diffs) std::printf(" ; %s sim=%.3f real=%.3f", d.what.c_str(), d.sim, d.real);
                    std::printf("\n");
                }
                diffs.clear();
            }
            if (!diffs.empty()) {
                if (firstDiv < 0) firstDiv = static_cast<int>(i);
                if (shown < maxDiffs) {
                    std::printf("tick %d (button %d) : %zu difference(s)\n", rec.rows[i].t, button ? 1 : 0,
                                diffs.size());
                    for (auto const& d : diffs)
                        std::printf("    %-22s sim=%.6f real=%.6f\n", d.what.c_str(), d.sim, d.real);
                    ++shown;
                }
                if (!keepGoing) {
                    if (shown <= maxDiffs)
                        std::printf("    (next real state : x=%.4f y=%.4f yv=%.6f mode=%c flags=%08x ; resync)\n",
                                    next.x, next.y, next.yv, next.mode, next.flags);
                    if (resync) {
                        applyRow(g.p1, next, true);
                        if (next2 && g.isDualMode) applyRow(g.p2, *next2, true);
                        g.p1.lastX = rec.rows[i].nx;
                        g.p1.lastY = rec.rows[i].ny;
                    }
                    if (shown >= maxDiffs) break;
                }
            } else if (verbose) {
                std::printf("tick %d ok : x=%.3f y=%.3f yv=%.4f speed=%.2f parked=%.2f\n", rec.rows[i].t, g.p1.x,
                            g.p1.y, g.p1.yVelocity, g.p1.playerSpeed, g.parkedSpeed);
            }
            if (g.died) {
                std::printf("sim : died at tick %d (object uid %d) ; real : died at tick %d (uid %d)\n", g.tick - 1,
                            g.killerUid, rec.deathTick, rec.deathKiller);
                if (!keepGoing && resync && next.t != rec.deathTick && shown < maxDiffs) {
                    g.died = false;
                    g.playerDied = false;
                    g.killerUid = 0;
                    g.p1.isDead = false;
                    g.p2.isDead = false;
                    applyRow(g.p1, next, true);
                    g.p1.lastX = rec.rows[i].nx;
                    g.p1.lastY = rec.rows[i].ny;
                    continue;
                }
                break;
            }
            if (g.completed) {
                std::printf(
                        "sim : level complete at real tick %d (x=%.1f, length %.1f) ; recording : end \"%s\" at tick "
                        "%d\n",
                        next.t, g.p1.x, w.length, rec.endWhy.c_str(), rec.rows.back().t);
                break;
            }
        }
        if (!g.died && rec.deathTick >= 0 && rec.deathTick >= rec.rows.back().t) {
            g_trace = (traceTick >= 0 && rec.rows.back().t >= traceTick && rec.rows.back().t <= traceEnd);
            stepTick(g, rec.deathHeld >= 0 ? rec.deathHeld != 0 : rec.rows.back().held);
            g_trace = false;
            if (g.died) {
                bool same = g.killerUid == rec.deathKiller;
                if (!same && rec.deathKiller == 0 && g.killerUid != 0) {
                    auto killer = g.objByUid(g.killerUid);
                    same = killer &&
                           (killer.getType() == Solid || killer.getType() == Breakable || killer.getType() == Slope);
                }
                std::printf("sim : died at tick %d (object uid %d) ; real : died at tick %d (uid %d) -> %s\n",
                            g.tick - 1, g.killerUid, rec.deathTick, rec.deathKiller, same ? "same" : "DIFFERENT");
            } else {
                std::printf(
                        "sim : still alive after tick %d while the real player dies at tick %d (uid %d) : x=%.3f "
                        "y=%.3f yv=%.4f\n",
                        g.tick - 1, rec.deathTick, rec.deathKiller, g.p1.x, g.p1.y, g.p1.yVelocity);
            }
        }
        if (splitSteps) std::printf("(%d steps split in two by clicks between steps)\n", splitSteps);
        if (cosmeticCount)
            std::printf("(%d tick(s) with a cosmetic mismatch : rotation, fallSpeed, last portal or snap distance)\n",
                        cosmeticCount);

        if (g.trig.on) {
            auto const& heavy = g.trig.r();
            std::printf(
                    "trigger state (end) : %zu commands, %zu group offsets, %zu own objects, %zu changed cells, %zu "
                    "groups off, %zu pending spawns, %zu counters, %zu collisions, %zu touches\n",
                    heavy.M().cmds.size(), heavy.M().groupOff.size(),
                    g.trig.rd().objDyn.size() + (g.trig.pv ? g.trig.pv->dyn.items.size() : 0), heavy.M().cellOv.size(),
                    static_cast<size_t>(heavy.offCount), heavy.spawnQueue.size(), heavy.items.size(),
                    heavy.collisions.size(), heavy.touches.size());
            if (objCmp.ticksWithDiff == 0) {
                std::printf("objects : same as the timeline on every tick\n");
            } else {
                std::printf("objects : %d tick(s) with a mismatch (first : tick %d)\n", objCmp.ticksWithDiff,
                            objCmp.firstTick);
                std::vector<std::pair<int, int>> byFirstTick;
                for (auto const& [u, stats] : objCmp.per) byFirstTick.push_back({stats.first, u});
                std::sort(byFirstTick.begin(), byFirstTick.end());
                int shownObjs = 0;
                int gameObjCount = 0;
                for (auto const& [f, u] : byFirstTick) {
                    GObj o = g.objByUid(u);
                    if (!o) continue;
                    bool anyRelevant = false;
                    for (short gr : o.o->groups) {
                        if (g.world->relevantGroup(gr)) {
                            anyRelevant = true;
                            break;
                        }
                    }
                    bool const posTrig = o.o->type == Modifier && !o.o->touchTriggered;
                    bool const game = !o.o->decoration && !o.o->uiObject && anyRelevant && !posTrig;
                    if (game) ++gameObjCount;
                    if (shownObjs++ < 80) {
                        auto const& stats = objCmp.per[u];
                        std::printf(
                                "  [object] uid %d id %d type %d cls %d %s : ticks %d..%d (%d), max gap %.4f, max rot "
                                "%.2f%s\n",
                                u, o.o->id, o.o->type, o.o->cls, game ? "GAME" : "decoration", stats.first, stats.last,
                                stats.n, stats.maxD, stats.maxR, stats.dis ? ", different on/off state" : "");
                    }
                }
                std::printf("objects : %zu object(s) with a mismatch, %d of them matter for the player\n",
                            byFirstTick.size(), gameObjCount);
            }
        }
        if (firstDiv < 0)
            std::printf("OK : %zu identical ticks\n", rowCount - 1);
        else
            std::printf("first divergence : tick %d / %zu\n", firstDiv, rowCount - 1);
        return firstDiv < 0 ? 0 : 3;
    }

    // square object for the synthetic levels, centered on (x, y), 30x30 block by default
    Obj makeBlock(int uid, float x, float y, int type = Solid, float width = 30, float height = 30) {
        Obj o;
        o.uid = uid;
        o.id = 1;
        o.type = type;
        o.cls = 0;
        o.x = x;
        o.y = y;
        o.posX = x;
        o.posY = y;
        o.width = width;
        o.height = height;
        o.rect = Rect{x - width * 0.5f, y - height * 0.5f, width, height};
        o.startX = x;
        o.startY = y;
        return o;
    }

    // counts the failed checks of a self test and prints one line per check
    struct Checks {
        int failures = 0;
        void operator()(bool ok, char const* what) {
            std::printf("  [%s] %s\n", ok ? "ok" : "FAILED", what);
            if (!ok) ++failures;
        }
    };

    // true when a and b differ by less than tolerance
    bool near(double a, double b, double tolerance = 1e-3) { return std::fabs(a - b) < tolerance; }

    // "apsim synth": self test on a small handmade level (jump, block, spike, wall), plus the speed of the
    // simulator when measureSpeed; returns the number of failed checks
    int cmdSynth(bool measureSpeed) {
        Checks check;
        World w;
        w.levelName = "synth";
        w.length = 3000;
        w.maxGameplayY = 2400;
        w.startX = 0;
        w.startY = 105;
        w.objs.push_back(makeBlock(1, 600, 105));
        w.objs.push_back(makeBlock(2, 630, 105));
        {
            Obj spike = makeBlock(3, 900, 105, Hazard, 6, 12);
            spike.id = 8;
            w.objs.push_back(spike);
        }
        w.objs.push_back(makeBlock(4, 1200, 105));
        w.objs.push_back(makeBlock(5, 1200, 135));
        w.build();
        Game g;
        setupGame(g, w);
        std::printf("synth : start x=%.1f y=%.1f, ground=%.1f\n", g.p1.x, g.p1.y, 90.f + g.p1.hitboxSize * 0.5f);
        for (int i = 0; i < 20; ++i) stepTick(g, false);
        std::printf("after 20 ticks : x=%.3f y=%.3f yv=%.4f onGround=%d\n", g.p1.x, g.p1.y, g.p1.yVelocity,
                    g.p1.isOnGround);
        check(g.p1.isOnGround && near(g.p1.y, 105.0) && g.p1.yVelocity == 0.0, "cube : rests on the ground");

        stepTick(g, true);
        std::printf("jump : yv=%.4f y=%.3f\n", g.p1.yVelocity, g.p1.y);
        check(near(g.p1.yVelocity, 10.964), "cube : jump speed of the game (10.964)");
        int ticks = 0;
        float apex = g.p1.y;
        while (!g.p1.isOnGround && ticks < 400) {
            stepTick(g, false);
            apex = std::max(apex, g.p1.y);
            ++ticks;
        }
        std::printf("landing after %d ticks (%.3f s), apex y=%.3f, x=%.3f\n", ticks, ticks / 240.0, apex, g.p1.x);
        check(ticks == 102 && near(apex, 168.847), "cube : a jump lasts 102 ticks and peaks at y 168.847");

        g.reset();
        while (g.p1.x < 600 - 118 && !g.died) stepTick(g, false);
        stepTick(g, true);
        while (!g.p1.isOnGround && !g.died && g.p1.x < 700) stepTick(g, false);
        std::printf("block : x=%.3f y=%.3f (expected 135) lastGround=%d dead=%d\n", g.p1.x, g.p1.y,
                    g.p1.lastGroundObject, g.died);
        check(!g.died && near(g.p1.y, 135.0), "cube : lands on top of a block");

        g.reset();
        g.p1.x = 700;
        g.p1.lastX = 700;
        while (!g.died && g.p1.x < 1000) stepTick(g, false);
        std::printf("spike : dead=%d killer=%d at x=%.3f (tick %d)\n", g.died, g.killerUid, g.p1.x, g.tick);
        check(g.died && g.killerUid == 3, "cube : a spike kills");

        g.reset();
        g.p1.x = 700;
        g.p1.lastX = 700;
        while (!g.died && g.p1.x < 900 - 90) stepTick(g, false);
        stepTick(g, true);
        while (!g.died && g.p1.x < 1000) stepTick(g, false);
        std::printf("jump onto a spike : dead=%d x=%.3f y=%.3f\n", g.died, g.p1.x, g.p1.y);
        check(!g.died, "cube : a jump clears the spike");

        g.reset();
        g.p1.x = 1000;
        g.p1.lastX = 1000;
        while (!g.died && g.p1.x < 1300) stepTick(g, false);
        std::printf("wall : dead=%d killer=%d at x=%.3f\n", g.died, g.killerUid, g.p1.x);
        check(g.died && g.killerUid == 4, "cube : running into a wall kills");

        if (measureSpeed) {
            std::mt19937 rng(1);
            auto t0 = std::chrono::steady_clock::now();
            long done = 0;
            bool btn = false;
            g.reset();
            while (done < 2000000) {
                if ((rng() & 15) == 0) btn = !btn;
                stepTick(g, btn);
                ++done;
                if (g.died || g.completed) g.reset();
            }
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("speed : %.0f ticks/s (one core)\n", done / seconds);
        }
        return check.failures;
    }

    std::atomic<int> g_liveSearchTick{0};

    // "apsim solve": searches a full path through the level (what F3 does in the mod), 0 if found
    int cmdSolve(int argc, char** argv) {
        if (argc < 3) {
            std::fprintf(stderr,
                         "usage: apsim solve <level.objects.jsonl> [--rec recording.jsonl] [--threads N] [--beam N] "
                         "[--maxbeam N] [--gap N] [--time S] [--out macro.txt]\n");
            return 2;
        }
        World w;
        std::string err;
        if (!loadLevel(argv[2], w, &err)) {
            std::fprintf(stderr, "level : %s\n", err.c_str());
            return 1;
        }
        applyEnvContext(w);
        printTriggerStatus(w);
        for (auto const& m : w.menus) {
            std::printf("settings menu : item %d (%s) :", m.item, m.kind.c_str());
            for (auto const& op : m.options)
                std::printf(" [%s -> %d%s]", op.label.c_str(), op.value,
                            (m.hasPrefer && op.value == m.prefer)                                     ? ", wanted"
                            : std::find(m.forbid.begin(), m.forbid.end(), op.value) != m.forbid.end() ? ", forbidden"
                                                                                                      : "");
            std::printf("\n");
        }
        if (!w.ldmTouch.empty()) {
            std::printf("low detail : %zu object(s) labeled \"ldm\" never touched :", w.ldmTouch.size());
            for (int i : w.ldmTouch) std::printf(" %d", w.objs[static_cast<size_t>(i)].uid);
            std::printf("\n");
        }

        SearchOptions opt;
        opt.maxBeamWidth = 0;
        char const* outPath = nullptr;
        char const* recPath = nullptr;
        char const* ctxRec = nullptr;
        char const* fromRec = nullptr;
        int fromTick = -1;
        char const* prefixPath = nullptr;
        int prefixTicks = 0;
        double liveRate = 0;
        int legacyCadence = 0;
        for (int i = 3; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--rec") && i + 1 < argc)
                recPath = argv[++i];
            else if (!std::strcmp(argv[i], "--cadence") && i + 1 < argc)
                legacyCadence = std::max(1, std::atoi(argv[++i]));
            else if (!std::strcmp(argv[i], "--from-rec") && i + 2 < argc) {
                fromRec = argv[++i];
                fromTick = std::atoi(argv[++i]);
            } else if (!std::strcmp(argv[i], "--ctx") && i + 1 < argc)
                ctxRec = argv[++i];
            else if (!std::strcmp(argv[i], "--prefix") && i + 2 < argc) {
                prefixPath = argv[++i];
                prefixTicks = std::atoi(argv[++i]);
            } else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc)
                opt.threads = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--band") && i + 1 < argc)
                opt.bandCost = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--empty") && i + 1 < argc)
                opt.emptyCost = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--extent") && i + 2 < argc) {
                opt.extentCost = std::atoi(argv[++i]);
                opt.extentMargin = static_cast<float>(std::atof(argv[++i]));
            } else if (!std::strcmp(argv[i], "--portalcost") && i + 1 < argc)
                opt.missedPortalCost = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--flyexit") && i + 1 < argc)
                opt.flyExitCap = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--flytries") && i + 1 < argc)
                opt.flyExitTries = std::atoi(argv[++i]) != 0;
            else if (!std::strcmp(argv[i], "--boundcost") && i + 1 < argc)
                opt.boundCost = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--noportaldebt"))
                opt.portalDebt = false;
            else if (!std::strcmp(argv[i], "--waveexit") && i + 1 < argc)
                opt.waveExitReach = static_cast<float>(std::atof(argv[++i]));
            else if (!std::strcmp(argv[i], "--latespeed") && i + 1 < argc)
                opt.lateSpeedCost = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--tight") && i + 2 < argc) {
                opt.tightCost = std::atoi(argv[++i]);
                opt.tightCost2 = std::atoi(argv[++i]);
            } else if (!std::strcmp(argv[i], "--orbcost") && i + 1 < argc)
                opt.missedOrbCost = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--beam") && i + 1 < argc)
                opt.beamWidth = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--maxbeam") && i + 1 < argc)
                opt.maxBeamWidth = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--gap") && i + 1 < argc)
                opt.minPressGap = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--maxsoft") && i + 1 < argc)
                opt.maxSoft = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--qy") && i + 1 < argc)
                opt.quantY = static_cast<float>(std::atof(argv[++i]));
            else if (!std::strcmp(argv[i], "--qv") && i + 1 < argc)
                opt.quantV = static_cast<float>(std::atof(argv[++i]));
            else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
                outPath = argv[++i];
            else if (!std::strcmp(argv[i], "--time") && i + 1 < argc)
                opt.timeLimit = std::atof(argv[++i]);
            else if (!std::strcmp(argv[i], "--maxticks") && i + 1 < argc)
                opt.maxTicks = std::atol(argv[++i]);
            else if (!std::strcmp(argv[i], "--horizon") && i + 1 < argc)
                opt.horizonTicks = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--lag") && i + 1 < argc)
                opt.commitLag = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--nofragile")) {
                opt.fragileCost = 0;
                opt.fragileObbCost = 0;
            } else if (!std::strcmp(argv[i], "--live")) {
                liveRate = (i + 1 < argc && argv[i + 1][0] != '-') ? std::atof(argv[++i]) : 1.0;
            } else if (!std::strcmp(argv[i], "--touch") && i + 1 < argc) {
                std::string list = argv[++i];
                size_t pos = 0;
                while (pos < list.size()) {
                    size_t const comma = list.find(',', pos);
                    int const uid = std::atoi(
                            list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos).c_str());
                    if (uid > 0 && uid <= w.uidMax && w.byUid[static_cast<size_t>(uid)] >= 0)
                        opt.mustTouch.push_back(w.byUid[static_cast<size_t>(uid)]);
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
            }
        }
        if (opt.maxBeamWidth <= 0) opt.maxBeamWidth = std::max(opt.beamWidth * 16, 6400);
        Recording rec;
        Timeline tl;
        if (recPath) {
            if (!loadRecording(recPath, rec, &err)) {
                std::fprintf(stderr, "recording : %s\n", err.c_str());
                return 1;
            }
            timelineFromRecording(rec, w, tl, 1);
        }
        if (ctxRec || fromRec) {
            Recording ctxRecording;
            if (loadRecording(ctxRec ? ctxRec : fromRec, ctxRecording, &err)) applyAttemptContext(ctxRecording, w);
        }
        applyModFrames(w, legacyCadence);
        Game g;
        setupGame(g, w);

        if (fromRec) {
            Recording fromRecording;
            if (!loadRecording(fromRec, fromRecording, &err)) {
                std::fprintf(stderr, "recording : %s\n", err.c_str());
                return 1;
            }
            if (fromRecording.rows.empty()) return 1;
            if (fromRecording.rows[0].t > 0) {
                Game fastForward;
                setupGame(fastForward, w);
                fastForward.ignoreDamage = true;
                while (fastForward.tick < fromRecording.rows[0].t) stepTick(fastForward, false);
                g.trig = fastForward.trig;
                for (auto const& e : fastForward.objState.items)
                    if (isSpawnableTrigger(w.objs[static_cast<size_t>(e.first)].id)) g.objState[e.first] = e.second;
            }
            applyRow(g.p1, fromRecording.rows[0], true);
            g.levelTime = fromRecording.rows[0].tt;
            g.commandIndex = static_cast<unsigned>(fromRecording.rows[0].ci);
            g.tick = fromRecording.rows[0].t;
            for (size_t i = 0; i + 1 < fromRecording.rows.size() && fromRecording.rows[i].t < fromTick; ++i) {
                int const gap = fromRecording.rows[i + 1].ci - fromRecording.rows[i].ci;
                int const splits = gap > 2 ? (gap - 2) / 2 : 0;
                bool const button = fromRecording.rows[i + 1].held;
                if (splits > 0) {
                    stepTick(g, fromRecording.rows[i].held);
                    for (int k = 0; k < splits; ++k) {
                        stepTickDt(g, fromRecording.rows[i].held, 0.125f, true);
                        stepTickDt(g, (k == splits - 1) ? button : fromRecording.rows[i].held, 0.125f, true);
                    }
                } else {
                    stepTick(g, button);
                }
                if (g.died) {
                    std::printf("start replay : died at tick %d\n", g.tick);
                    return 1;
                }
            }
            std::printf("start : recording state at tick %d (x=%.1f y=%.1f mode %c)\n", g.tick, g.p1.x, g.p1.y,
                        modeChar(g.p1));
            for (auto const& row : fromRecording.rows) {
                if (row.t == g.tick) {
                    std::vector<Diff> startDiffs;
                    compareRow(g.p1, row, startDiffs, 1e-4, 1e-9);
                    for (auto const& d : startDiffs)
                        std::printf("    start mismatch : %s sim=%.6f real=%.6f%s\n", d.what.c_str(), d.sim, d.real,
                                    d.cosmetic ? " (cosmetic)" : "");
                    break;
                }
            }
        }
        Game const g0 = g;

        std::vector<uint8_t> prefix;
        if (prefixPath) {
            std::ifstream macroFile(prefixPath);
            std::string line;
            std::getline(macroFile, line);
            for (int i = 0; i < prefixTicks && i < static_cast<int>(line.size()) && !g.died; ++i) {
                uint8_t const buttons = static_cast<uint8_t>(line[static_cast<size_t>(i)] - '0');
                prefix.push_back(buttons);
                stepTick(g, buttons);
                opt.priorPath.push_back({g.p1.x, g.p1.y, static_cast<float>(g.tick)});
            }
            std::printf("start : tick %d of the macro (x=%.1f y=%.1f mode %c, dead=%d)\n", g.tick, g.p1.x, g.p1.y,
                        modeChar(g.p1), g.died);
        }
        std::printf("level %s : %zu objects, length %.1f (about %d ticks), start (%.1f, %.1f)\n", w.levelName.c_str(),
                    w.objs.size(), w.length, static_cast<int>((w.length - 300.f) / 1.29825f), g.p1.x, g.p1.y);

        std::atomic<int> played{0};
        std::atomic<int> playedAbs{0};
        int const liveBase = g.tick;
        playedAbs = liveBase;
        std::mutex liveMx;
        std::vector<uint8_t> livePlan;
        std::atomic<bool> searchDone{false};
        int lostAttempts = 0;
        int stalls = 0;
        double stallSec = 0;
        double firstPublish = -1;
        double maxStall = 0;
        std::atomic<int> attemptGen{0};
        int unexpectedDeaths = 0;
        std::atomic<int> lostAt{-1};
        std::atomic<int> lostAtAbs{-1};
        auto attemptT0 = std::chrono::steady_clock::now();
        double startWaitSec = 0;
        auto markLost = [&](int at) {
            lostAt = at;
            lostAtAbs = liveBase + at;
            attemptT0 = std::chrono::steady_clock::now();
        };
        Game const mirrorStart = g;
        std::thread gameThread;
        auto const liveT0 = std::chrono::steady_clock::now();
        static std::atomic<bool> starving{false};
        if (liveRate > 0) {
            opt.playedTicks = &playedAbs;
            opt.starving = &starving;
            opt.maxLead = 0;
            opt.waitFor = &lostAtAbs;
            opt.onPrefix = [&](std::vector<uint8_t> const& prefix) {
                std::lock_guard<std::mutex> lock(liveMx);
                if (firstPublish < 0)
                    firstPublish = std::chrono::duration<double>(std::chrono::steady_clock::now() - liveT0).count();
                int const p = played.load();
                bool lost = static_cast<int>(prefix.size()) < p ||
                            !std::equal(livePlan.begin(),
                                        livePlan.begin() + std::min<long>(p, static_cast<long>(livePlan.size())),
                                        prefix.begin());
                if (lost) {
                    ++lostAttempts;
                    ++attemptGen;
                    markLost(p);
                    played = 0;
                    playedAbs = liveBase;
                    std::printf("  [live] attempt lost : prefix %zu, played %d\n", prefix.size(), p);
                }
                livePlan = prefix;
            };
            gameThread = std::thread([&] {
                auto last = std::chrono::steady_clock::now();
                double tickAcc = 0;
                bool stalled = false;
                double stallStart = 0;
                while (!searchDone.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    auto const now = std::chrono::steady_clock::now();
                    double const dt = std::chrono::duration<double>(now - last).count();
                    last = now;
                    std::lock_guard<std::mutex> lock(liveMx);
                    int const available = static_cast<int>(livePlan.size());
                    {
                        static double nextReport = 15.0;
                        double const elapsed = std::chrono::duration<double>(now - liveT0).count();
                        if (debugOptions().check && elapsed >= nextReport) {
                            nextReport += 15.0;
                            std::printf(
                                    "  [live] %.0f s : player at tick %d, plan up to %d, search at tick %d, %d "
                                    "attempt(s) lost\n",
                                    elapsed, played.load(), available, g_liveSearchTick.load(), lostAttempts);
                            std::fflush(stdout);
                        }
                    }
                    constexpr double kStartWaitSec = 30.0;
                    if (played.load() == 0 && std::chrono::duration<double>(now - liveT0).count() < kStartWaitSec &&
                        g_liveSearchTick.load() - liveBase < kStartLead)
                        continue;
                    {
                        constexpr double kLostWaitSec = 120.0;
                        if (played.load() == 0 && lostAt >= 0 &&
                            std::chrono::duration<double>(now - attemptT0).count() < kLostWaitSec &&
                            g_liveSearchTick.load() - liveBase < lostAt.load() + 480) {
                            startWaitSec += dt;
                            continue;
                        }
                    }
                    if (available <= played.load() + 1) {
                        if (firstPublish >= 0) {
                            stallSec += dt;
                            if (!stalled) {
                                stalled = true;
                                ++stalls;
                                stallStart = stallSec;
                            }
                            maxStall = std::max(maxStall, stallSec - stallStart);
                        }
                        if (stalled && stallSec - stallStart > 0.75) starving = true;
                        constexpr double maxWait = 4.0;
                        if (stalled && stallSec - stallStart >= maxWait && played.load() > 0) {
                            if (debugOptions().check)
                                std::printf(
                                        "  [live] waited more than %.0f s at tick %d (search at tick %d) : attempt "
                                        "lost\n",
                                        maxWait, played.load(), g_liveSearchTick.load());
                            ++lostAttempts;
                            ++attemptGen;
                            markLost(played.load());
                            played = 0;
                            playedAbs = liveBase;
                            stalled = false;
                        }
                        continue;
                    }
                    if (stalled && debugOptions().check)
                        std::printf("  [live] waited %.2f s at tick %d (search at tick %d)\n", stallSec - stallStart,
                                    played.load(), g_liveSearchTick.load());
                    stalled = false;
                    starving = false;
                    tickAcc += dt * 240.0 * liveRate;
                    int const steps = static_cast<int>(tickAcc);
                    tickAcc -= steps;
                    played = std::min(available - 1, played.load() + steps);
                    playedAbs = liveBase + played.load();
                    static thread_local Game mirror = mirrorStart;
                    static thread_local int mirrorGen = 0;
                    if (mirrorGen != attemptGen.load()) {
                        mirror = mirrorStart;
                        mirrorGen = attemptGen.load();
                    }
                    std::vector<uint8_t> slice;
                    int const upTo = std::min(played.load(), available);
                    for (int t = mirror.tick - liveBase; t < upTo; ++t)
                        slice.push_back(livePlan[static_cast<size_t>(t)]);
                    int const genBefore = attemptGen.load();
                    liveMx.unlock();
                    for (uint8_t b : slice) {
                        if (mirror.died) break;
                        stepTick(mirror, b);
                    }
                    liveMx.lock();
                    if (mirror.died && genBefore == attemptGen.load()) {
                        ++unexpectedDeaths;
                        std::printf(
                                "  [live] death in the imitated game at tick %d (uid %d) ; plan up to %d - the "
                                "published plan doesn't lead where the search thought\n",
                                mirror.tick, mirror.killerUid, static_cast<int>(livePlan.size()));
                        ++lostAttempts;
                        ++attemptGen;
                        markLost(mirror.tick - liveBase);
                        played = 0;
                        playedAbs = liveBase;
                    }
                }
            });
        }

        static auto const tSolve0 = std::chrono::steady_clock::now();
        constexpr int progEvery = 4096;

        if (liveRate <= 0 && prefix.empty() && menuWanted(w)) {
            SearchOptions menuOpt = opt;
            menuOpt.menuGoal = true;
            menuOpt.persistChangeCost = 0;
            menuOpt.horizonTicks = 4800;
            menuOpt.timeLimit = 30.0;
            SearchResult menuResult = searchPath(g, menuOpt);
            std::printf("settings attempt : %s ; %zu ticks, %d presses, %.1f s ; persistent items after :",
                        menuResult.menuDone ? "settings set" : ("FAILED (" + menuResult.why + ")").c_str(),
                        menuResult.inputs.size(), menuResult.presses, menuResult.stats.seconds);
            for (auto const& e : menuResult.persistAfter) std::printf(" %d=%d", e.first, e.second);
            std::printf("\n");
            if (menuResult.menuDone) {
                if (outPath) {
                    std::ofstream menuFile(std::string(outPath) + ".settings.txt");
                    for (uint8_t b : menuResult.inputs) menuFile << static_cast<char>('0' + (b & 3));
                    menuFile << "\n";
                }
                w.persistentItems = menuResult.persistAfter;
                Game g2;
                setupGame(g2, w);
                g = g2;
            }
        }

        SearchResult r = searchPath(g, opt, nullptr, [](int t, int n) {
            g_liveSearchTick = t;
            if ((t % progEvery) == 0)
                std::printf("  tick %d : %d states (%.1f s)\n", t, n,
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - tSolve0).count());
        });
        if (liveRate > 0) {
            int const baseBeam = opt.beamWidth;
            int beamMul = 1;
            while (!r.success && opt.timeLimit > 0) {
                double const used = std::chrono::duration<double>(std::chrono::steady_clock::now() - tSolve0).count();
                double const budget = opt.timeLimit - used;
                if (budget < 5.0) break;
                beamMul = std::min(beamMul * 2, 4);
                SearchOptions again = opt;
                again.beamWidth = baseBeam * beamMul;
                again.maxBeamWidth = std::max(again.beamWidth * 16, opt.maxBeamWidth);
                again.timeLimit = budget;
                std::printf("  [live] search restarted (beam x%d) after : %s\n", beamMul, r.why.c_str());
                r = searchPath(g, again, nullptr, [](int t, int) { g_liveSearchTick = t; });
            }
        }
        if (liveRate > 0) {
            searchDone = true;
            gameThread.join();
            std::printf(
                    "live (speed %.2f) : first prefix after %.3f s ; player at tick %d ; %d wait(s), %.2f s in total "
                    "(max %.2f s) ; %d attempt(s) lost, %d of them unexpected deaths ; %.1f s of waiting at the start "
                    "after a loss ; search paused %.1f s (lead > %d ticks)\n",
                    liveRate, firstPublish, played.load(), stalls, stallSec, maxStall, lostAttempts, unexpectedDeaths,
                    startWaitSec, r.stats.pausedSeconds, opt.maxLead);
        }
        std::printf(
                "result : %s (%s) ; %zu ticks, %d presses, %ld ticks simulated, max layer %d, %d widenings, %d soft "
                "restarts, %d back-offs, %.3f s\n",
                r.success ? "OK" : "FAILED", r.why.c_str(), r.inputs.size(), r.presses, r.stats.ticksSimulated,
                r.stats.maxLayer, r.stats.widenings, r.stats.softRestarts, r.stats.hardPops, r.stats.seconds);
        if (r.stats.awayKills || r.stats.revisitKills || r.stats.stuckKills || r.stats.escapeKills)
            std::printf(
                    "rules : %ld state(s) killed off-level, %ld return(s) into the part already played dropped, %ld "
                    "state(s) trapped, %ld left the structure\n",
                    r.stats.awayKills, r.stats.revisitKills, r.stats.stuckKills, r.stats.escapeKills);
        if (r.stats.adaptTicks > 0)
            std::printf("adaptive beam : reduced %ld ticks out of %ld\n", r.stats.adaptNarrow, r.stats.adaptTicks);
        std::printf("debt : %d try(ies), %d debt(s) accepted, %ld ticks simulated for accepted debts (%.0f %%)\n",
                    r.stats.debtRungs, r.stats.debtAccepted, r.stats.debtTicks,
                    r.stats.ticksSimulated > 0 ? 100.0 * static_cast<double>(r.stats.debtTicks) /
                                                         static_cast<double>(r.stats.ticksSimulated)
                                               : 0.0);

        if (debugOptions().stats)
            std::printf(
                    "area trigger memo : %ld hits, %ld computations (%ld depending on y) ; cells : %ld hits, %ld "
                    "computations\n",
                    g_areaMemoStat[0].load(), g_areaMemoStat[1].load() + g_areaMemoStat[2].load(),
                    g_areaMemoStat[2].load(), g_areaMemoStat[3].load(), g_areaMemoStat[4].load());
        if (debugOptions().stats)
            std::printf(
                    "area trigger split : shared part %ld read back, %ld computed ; own part %ld read back, %ld "
                    "computed\n",
                    g_areaSplitStat[0].load(), g_areaSplitStat[1].load(), g_areaSplitStat[2].load(),
                    g_areaSplitStat[3].load());
        if (debugOptions().stats) {
            long hits;
            long miss;
            long hashes;
            trigMemoStats(hits, miss, hashes);
            std::printf("trigger memo (main thread) : %ld hits, %ld misses, %ld fingerprints computed\n", hits, miss,
                        hashes);
            std::printf("  time : expansion %.2f s, merge/prune %.2f s\n", r.stats.expandSeconds, r.stats.mergeSeconds);
            std::printf(
                    "  counters : heavy copies %ld, dyn copies %ld, dyn fingerprints %ld, follow-Y steps %ld, dyn "
                    "index %ld, re-sections %ld\n",
                    g_trigCounters[0], g_trigCounters[1], g_trigCounters[2], g_trigCounters[3], g_trigCounters[4],
                    g_trigCounters[5]);
            long const* memoDbg = trigMemoDbg();
            std::printf(
                    "  diag : lockP misses %ld, without memo %ld, commits dep0 %ld dep1 %ld dep2 %ld, spawn misses "
                    "%ld, side effects %ld, shared moves %ld\n",
                    memoDbg[0], memoDbg[1], memoDbg[2], memoDbg[3], memoDbg[4], memoDbg[5], memoDbg[6], memoDbg[7]);
        }
        if (debugOptions().stats) {
            long scanned;
            long movable;
            long calls;
            long kept;
            filterStats(scanned, movable, calls, kept);
            std::printf(
                    "pre-filter (main thread) : %ld calls, %.1f band entries / call, %.1f movables / call (%.1f "
                    "without a role), %.1f kept / call\n",
                    calls, calls ? double(scanned) / calls : 0, calls ? double(movable) / calls : 0,
                    calls ? double(filterStatMovDeco()) / calls : 0, calls ? double(kept) / calls : 0);
            std::printf("  dynamic indexes built : %ld\n", cellCacheBuilds());
        }
        if (!prefix.empty()) {
            r.inputs.insert(r.inputs.begin(), prefix.begin(), prefix.end());
        }

        Game replay = prefix.empty() ? g : g0;
        int const dumpEvery = debugOptions().pathEvery;
        for (uint8_t b : r.inputs) {
            stepTick(replay, b);
            if (dumpEvery > 0 && replay.tick % dumpEvery == 0)
                std::printf("  path tick %d : x=%.1f y=%.1f mode %c%s speed %.2f\n", replay.tick, replay.p1.x,
                            replay.p1.y,
                            replay.p1.isShip     ? 'S'
                            : replay.p1.isBall   ? 'B'
                            : replay.p1.isBird   ? 'U'
                            : replay.p1.isDart   ? 'W'
                            : replay.p1.isRobot  ? 'R'
                            : replay.p1.isSpider ? 'P'
                            : replay.p1.isSwing  ? 'G'
                                                 : 'C',
                            replay.p1.isUpsideDown ? " (upside down)" : "", replay.p1.playerSpeed);
            if (replay.died || replay.completed) break;
        }
        std::printf("replay : dead=%d (uid %d) complete=%d x=%.1f y=%.1f tick %d\n", replay.died, replay.killerUid,
                    replay.completed, replay.p1.x, replay.p1.y, replay.tick);

        if (debugOptions().stats) {
            long scanned;
            long movable;
            long calls;
            long kept;
            filterStats(scanned, movable, calls, kept);
            std::printf(
                    "pre-filter (main thread, after the replay) : %ld calls, %.1f band entries / call, %.1f movables / "
                    "call, %.1f kept / call, %.2f calls / tick\n",
                    calls, calls ? double(scanned) / calls : 0, calls ? double(movable) / calls : 0,
                    calls ? double(kept) / calls : 0, replay.tick ? double(calls) / replay.tick : 0);
        }

        if (!r.success && !replay.died && debugOptions().check) {
            for (int b = 0; b < 2; ++b) {
                Game probe = replay;
                int n = 0;
                while (!probe.died && !probe.completed && n < 240) {
                    stepTick(probe, b != 0);
                    ++n;
                }
                std::printf(
                        "  after the dead end, button %d for %d ticks : dead=%d (uid %d) x=%.1f y=%.1f yv=%.3f mode %c "
                        "ud=%d\n",
                        b, n, probe.died, probe.killerUid, probe.p1.x, probe.p1.y, probe.p1.yVelocity,
                        modeChar(probe.p1), probe.p1.isUpsideDown);
            }
        }
        if (!r.success) {
            float const deadEndX = replay.p1.x;
            for (size_t i = 0; i < w.objs.size(); ++i) {
                auto const& o = w.objs[i];
                if (o.isTouchTrigger() && o.rect.x + o.rect.w + 15.f > deadEndX - 1500.f &&
                    o.rect.x < deadEndX + 60.f) {
                    bool taken = false;
                    if (auto const* st = replay.osFind(static_cast<int>(i))) taken = st->activated;
                    std::printf(
                            "touch trigger near the dead end : uid %d id %d x=%.0f y=%.0f (%s) -> --touch %d to aim at "
                            "it\n",
                            o.uid, o.id, o.x, o.y, taken ? "touched" : "not touched", o.uid);
                }
            }
            std::printf("objects around the dead end (x %.0f) :\n", deadEndX);
            for (auto const& o : w.objs)
                if (o.type != Decoration && o.x > deadEndX - 200 && o.x < deadEndX + 150)
                    std::printf("  uid %d id %d type %d x=%.1f y=%.1f rect [%.1f %.1f %.1f %.1f]\n", o.uid, o.id,
                                o.type, o.x, o.y, o.rect.x, o.rect.y, o.rect.w, o.rect.h);
        }
        if (outPath) {
            std::ofstream file(outPath);
            for (size_t i = 0; i < r.inputs.size(); ++i) file << static_cast<char>('0' + (r.inputs[i] & 3));
            file << "\n";
        }
        return r.success ? 0 : 1;
    }

    // effect object (orb, portal...) for the synthetic levels, id is the GD object id
    Obj makeEffect(int uid, int id, int type, float x, float y, float width, float height) {
        Obj o = makeBlock(uid, x, y, type, width, height);
        o.id = id;
        o.cls = 1;
        return o;
    }

    // "apsim synthcore": checks dash, spider, dual and 2 player mode on handmade levels; returns the number of
    // failed checks
    int cmdSynthCore() {
        int failures = 0;
        auto check = [&](bool ok, char const* what) {
            std::printf("  [%s] %s\n", ok ? "ok" : "FAILED", what);
            if (!ok) ++failures;
        };

        {
            World w;
            w.levelName = "synthdash";
            w.length = 3000;
            w.maxGameplayY = 2400;
            w.startX = 0;
            w.startY = 105;
            Obj ring = makeEffect(1, 1704, DashRing, 450, 105, 30, 30);
            ring.rot = -45.f;
            ring.dashSpeed = 1.f;
            w.objs.push_back(ring);
            w.build();
            Game g;
            setupGame(g, w);
            while (!g.died && g.p1.x < 420) stepTick(g, false);
            int held = 0;
            float x0 = 0;
            float y0 = 0;
            while (!g.died && !g.p1.isDashing && held < 60) {
                stepTick(g, true);
                ++held;
            }
            check(g.p1.isDashing, "dash : the held orb starts the dash");
            x0 = g.p1.x;
            y0 = g.p1.y;
            for (int i = 0; i < 40; ++i) stepTick(g, true);
            float const dx = g.p1.x - x0;
            float const dy = g.p1.y - y0;
            std::printf("    dash 45° : dx=%.3f dy=%.3f yv=%.3f angle=%.1f\n", dx, dy, g.p1.yVelocity, g.p1.dashAngle);
            check(std::fabs(dx - dy) < 0.01f && dx > 10.f, "dash : 45 degree path (dy = dx)");
            check(g.p1.yVelocity == 0.0, "dash : no vertical speed during the dash");
            stepTick(g, false);
            check(!g.p1.isDashing, "dash : the release stops the dash");
            float const yTop = g.p1.y;
            for (int i = 0; i < 30; ++i) stepTick(g, false);
            check(g.p1.y < yTop, "dash : the player falls afterwards");
        }

        {
            World w;
            w.levelName = "synthspider";
            w.length = 3000;
            w.maxGameplayY = 2400;
            w.startX = 0;
            w.startY = 105;
            w.objs.push_back(makeEffect(1, 1331, SpiderPortal, 300, 150, 30, 90));
            int uid = 10;
            for (int i = 0; i < 40; ++i) w.objs.push_back(makeBlock(uid++, 15.f + 30.f * i, 255.f));
            w.build();
            Game g;
            setupGame(g, w);
            while (!g.died && g.p1.x < 450) stepTick(g, false);
            check(g.p1.isSpider && g.p1.isOnGround, "spider : spider mode on, on the ground");
            stepTick(g, true);
            std::printf("    after the press : y=%.3f ud=%d yv=%.3f\n", g.p1.y, g.p1.isUpsideDown, g.p1.yVelocity);
            check(std::fabs(g.p1.y - 226.5f) < 0.01f && g.p1.isUpsideDown,
                  "spider : teleported to the ceiling (y 226.5, gravity flipped)");
            for (int i = 0; i < 20; ++i) stepTick(g, false);
            check(std::fabs(g.p1.y - 226.5f) < 0.01f && !g.died, "spider : stays on the ceiling");
            stepTick(g, true);
            std::printf("    second press : y=%.3f ud=%d\n", g.p1.y, g.p1.isUpsideDown);
            check(std::fabs(g.p1.y - 103.5f) < 0.01f && !g.p1.isUpsideDown, "spider : back on the ground");
            for (int i = 0; i < 60; ++i) stepTick(g, false);
            check(!g.died && std::fabs(g.p1.y - 103.5f) < 0.01f, "spider : stable on the ground");
        }

        {
            World w;
            w.levelName = "synthdual";
            w.length = 3000;
            w.maxGameplayY = 2400;
            w.startX = 0;
            w.startY = 105;
            w.objs.push_back(makeEffect(1, 286, DualPortal, 300, 150, 30, 90));
            w.objs.push_back(makeEffect(2, 287, SoloPortal, 1200, 150, 30, 90));
            int uid = 10;
            for (int i = 0; i < 60; ++i) w.objs.push_back(makeBlock(uid++, 15.f + 30.f * i, 255.f));
            w.build();
            Game g;
            setupGame(g, w);
            while (!g.died && g.p1.x < 330) stepTick(g, false);
            check(g.isDualMode, "dual : portal taken, dual mode");
            std::printf("    p1 : x=%.3f y=%.3f yv=%.3f ; p2 : x=%.3f y=%.3f ud=%d yv=%.3f (tick %d)\n", g.p1.x, g.p1.y,
                        g.p1.yVelocity, g.p2.x, g.p2.y, g.p2.isUpsideDown, g.p2.yVelocity, g.tick);
            check(g.p2.isUpsideDown && std::fabs(g.p2.x - g.p1.x) < 0.01f, "dual : p2 copied from p1, gravity flipped");
            while (!g.died && g.p1.x < 600) stepTick(g, false);
            std::printf("    p1 y=%.3f p2 y=%.3f (expected 105 / 225) dead=%d\n", g.p1.y, g.p2.y, g.died);
            check(!g.died && std::fabs(g.p1.y - 105.f) < 0.01f && std::fabs(g.p2.y - 225.f) < 0.01f,
                  "dual : p1 on the ground, p2 on the ceiling");
            stepTick(g, true);
            check(g.p1.yVelocity > 5 && g.p2.yVelocity < -5, "dual : one press makes both jump (p2 downward)");
            for (int i = 0; i < 150 && !g.died; ++i) stepTick(g, false);
            check(!g.died && g.p1.isOnGround && g.p2.isOnGround, "dual : both land again");
            while (!g.died && g.p1.x < 1300) stepTick(g, false);
            check(!g.died && !g.isDualMode, "dual : solo portal taken, single mode");
            for (int i = 0; i < 120 && !g.died; ++i) stepTick(g, false);
            check(!g.died, "dual : stable after the solo portal");
        }

        {
            World w;
            w.levelName = "synth2p";
            w.length = 3000;
            w.maxGameplayY = 2400;
            w.startX = 0;
            w.startY = 105;
            w.twoPlayer = true;
            w.objs.push_back(makeEffect(1, 286, DualPortal, 300, 150, 30, 90));
            int uid = 10;
            for (int i = 0; i < 90; ++i) w.objs.push_back(makeBlock(uid++, 15.f + 30.f * i, 255.f));
            Obj spikeLow = makeBlock(uid++, 900.f, 105.f, Hazard, 6.f, 12.f);
            Obj spikeHigh = makeBlock(uid++, 1500.f, 225.f, Hazard, 6.f, 12.f);
            w.objs.push_back(spikeLow);
            w.objs.push_back(spikeHigh);
            w.build();
            Game g;
            setupGame(g, w);
            stepTick(g, 2);
            check(!g.p1.holdingJump && g.p1.yVelocity <= 0.0,
                  "2 players : outside dual, the player 2 button does nothing");
            stepTick(g, 0);
            while (!g.died && g.p1.x < 600) stepTick(g, 0);
            check(g.isDualMode && !g.died, "2 players : dual");
            stepTick(g, 2);
            check(std::fabs(g.p1.yVelocity) < 0.01 && g.p2.yVelocity < -5, "2 players : button 2 -> only p2 jumps");
            for (int i = 0; i < 150 && !g.died; ++i) stepTick(g, 0);
            stepTick(g, 1);
            check(g.p1.yVelocity > 5 && std::fabs(g.p2.yVelocity) < 0.01, "2 players : button 1 -> only p1 jumps");
            {
                Game test;
                setupGame(test, w);
                while (!test.died && !test.completed) stepTick(test, 0);
                std::printf("    without press : dead=%d killer %d x=%.1f p2 y=%.1f\n", test.died, test.killerUid,
                            test.p1.x, test.p2.y);
            }
            {
                Game test;
                setupGame(test, w);
                while (!test.died && !test.completed) {
                    stepTick(test, (test.p1.x > 840 && test.p1.x < 850) ? 1 : 0);
                }
                std::printf("    one p1 jump : dead=%d killer %d x=%.1f p2 y=%.1f complete=%d\n", test.died,
                            test.killerUid, test.p1.x, test.p2.y, test.completed);
            }
            SearchOptions opt;
            opt.threads = 1;
            opt.horizonTicks = 0;
            Game start;
            setupGame(start, w);
            SearchResult r = searchPath(start, opt);
            int p1Presses = 0;
            int p2Presses = 0;
            uint8_t prev = 0;
            for (uint8_t b : r.inputs) {
                if ((b & 1) && !(prev & 1)) ++p1Presses;
                if ((b & 2) && !(prev & 2)) ++p2Presses;
                prev = b;
            }
            std::printf("    2 player search : %s, %zu ticks, presses p1 %d p2 %d\n", r.success ? "OK" : r.why.c_str(),
                        r.inputs.size(), p1Presses, p2Presses);
            {
                Game test;
                setupGame(test, w);
                for (uint8_t b : r.inputs) {
                    stepTick(test, b);
                    if (test.died) break;
                }
                int actionCount[4] = {0, 0, 0, 0};
                for (uint8_t b : r.inputs) ++actionCount[b & 3];
                std::printf("    replay : dead=%d complete=%d ; actions 0:%d 1:%d 2:%d 3:%d ; dual=%d p2 y=%.1f\n",
                            test.died, test.completed, actionCount[0], actionCount[1], actionCount[2], actionCount[3],
                            test.isDualMode, test.p2.y);
            }
            check(r.success, "2 players : the search finds a path (p2 copies the state of p1 when dual starts)");
        }
        return failures;
    }

    // "apsim synthmove": a platform rising under the player and a falling spike, moved by a timeline; returns the
    // number of failed checks
    int cmdSynthMove() {
        Checks check;
        World w;
        w.levelName = "synthmove";
        w.maxGameplayY = 2400;
        w.startX = 0;
        w.startY = 105;
        w.length = 3000;
        for (int i = 0; i < 3; ++i) w.objs.push_back(makeBlock(10 + i, 615.f + 30.f * i, 75.f));
        {
            Obj spike = makeBlock(50, 1500.f, 300.f, Hazard, 6, 12);
            spike.id = 8;
            w.objs.push_back(spike);
        }
        w.build();
        std::vector<TimelineEvent> events;
        for (int t = 480; t <= 660; ++t)
            for (int i = 0; i < 3; ++i)
                events.push_back(TimelineEvent{t, 10 + i, 615.f + 30.f * i, 75.f + 0.5f * static_cast<float>(t - 480),
                                               0.f, 615.f + 30.f * i, 75.f + 0.5f * static_cast<float>(t - 480), 0});
        for (int t = 1000; t <= 1100; ++t)
            events.push_back(TimelineEvent{t, 50, 1500.f, 300.f - 1.65f * static_cast<float>(t - 1000), 0.f, 1500.f,
                                           300.f - 1.65f * static_cast<float>(t - 1000), 0});
        Timeline tl;
        tl.build(events, w);
        w.attach(&tl);
        std::printf("timeline : %zu objects, columns %zu\n", tl.objs.size(), tl.byColumn.size());
        Game g;
        setupGame(g, w);
        float maxY = 0;
        int landedTick = -1;
        while (!g.died && g.p1.x < 800 && g.tick < 2000) {
            stepTick(g, false);
            if (g.p1.lastGroundObject >= 10 && g.p1.lastGroundObject <= 12 && landedTick < 0) landedTick = g.tick;
            maxY = std::max(maxY, g.p1.y);
        }
        std::printf(
                "platform : landed at tick %d, max y %.2f (expected ~ 105+90 = 195 if carried), x=%.1f y=%.1f "
                "dead=%d\n",
                landedTick, maxY, g.p1.x, g.p1.y, g.died);
        check(landedTick == 451 && !g.died, "timeline : lands on the moving platform (tick 451)");
        Game g2;
        setupGame(g2, w);
        while (!g2.died && g2.p1.x < 1600 && g2.tick < 3000) stepTick(g2, false);
        std::printf("moving spike (stops at y=135) : dead=%d x=%.1f tick=%d (expected : alive)\n", g2.died, g2.p1.x,
                    g2.tick);
        check(!g2.died, "timeline : a spike that stops above the player does not kill");

        std::vector<TimelineEvent> events2 = events;
        for (int t = 1100; t <= 1130; ++t)
            events2.push_back(TimelineEvent{t, 50, 1500.f, 135.f - 0.5f * static_cast<float>(t - 1100), 0.f, 1500.f,
                                            135.f - 0.5f * static_cast<float>(t - 1100), 0});
        Timeline tl2;
        tl2.build(events2, w);
        w.attach(&tl2);
        Game g3;
        setupGame(g3, w);
        while (!g3.died && g3.p1.x < 1600 && g3.tick < 3000) stepTick(g3, false);
        std::printf(
                "moving spike (goes down to y=120) : dead=%d (uid %d) x=%.1f tick=%d (expected : killed by uid 50)\n",
                g3.died, g3.killerUid, g3.p1.x, g3.tick);
        check(g3.died && g3.killerUid == 50, "timeline : a spike that comes down onto the player kills");

        std::vector<TimelineEvent> events3 = events2;
        events3.push_back(TimelineEvent{0, 50, 1500.f, 300.f, 0.f, 1500.f, 300.f, 1});
        for (auto& e : events3)
            if (e.uid == 50 && e.tick > 0) e.flags = 1;
        Timeline tl3;
        tl3.build(events3, w);
        w.attach(&tl3);
        Game g4;
        setupGame(g4, w);
        while (!g4.died && g4.p1.x < 1600 && g4.tick < 3000) stepTick(g4, false);
        std::printf("spike turned off : dead=%d (expected : alive)\n", g4.died);
        check(!g4.died, "timeline : a spike turned off does not kill");
        return check.failures;
    }

    // slope object, dir is the game's m_slopeDirection (0 to 7), hazard = killing slope
    Obj makeSlope(int uid, float x, float y, float width, float height, int dir, bool uphill, bool hazard = false) {
        Obj o = makeBlock(uid, x, y, Slope, width, height);
        o.id = 289;
        o.slopeDir = dir;
        o.slopeUphill = uphill;
        o.slopeHazard = hazard;
        return o;
    }

    // "apsim synthslope": behaviour on slopes (up, down, hazard, ship, ceiling); prints the player state tick by tick
    // when verbose; returns the number of failed checks
    int cmdSynthSlope(bool verbose) {
        Checks check;
        World w;
        w.levelName = "synthslope";
        w.maxGameplayY = 2400;
        w.startX = 0;
        w.startY = 105;
        w.length = 4000;
        w.objs.push_back(makeSlope(1, 615, 105, 30, 30, 0, true));
        w.objs.push_back(makeBlock(2, 645, 105));
        w.objs.push_back(makeBlock(3, 1215, 135));
        w.objs.push_back(makeBlock(4, 1245, 135));
        w.objs.push_back(makeSlope(5, 1290, 135, 60, 30, 2, false));
        w.objs.push_back(makeSlope(7, 2415, 165, 30, 30, 1, false));
        w.objs.push_back(makeSlope(6, 1815, 105, 30, 30, 0, true, true));
        w.build();

        Game g;
        setupGame(g, w);
        float maxY = 0;
        int onSlopeTicks = 0;
        int firstOn = -1;
        double exitYv = 0;
        int exitTick = -1;
        while (!g.died && g.p1.x < 760 && g.tick < 2000) {
            stepTick(g, false);
            if (g.p1.isOnSlope) {
                ++onSlopeTicks;
                if (firstOn < 0) firstOn = g.tick;
            }
            if (!g.p1.isOnSlope && g.p1.wasOnSlope && exitTick < 0) {
                exitTick = g.tick;
                exitYv = g.p1.yVelocity;
            }
            maxY = std::max(maxY, g.p1.y);
            if (verbose && g.tick < 400 && (g.tick % 4 == 0) && g.p1.x > 560 && g.p1.x < 700)
                std::printf(
                        "  t=%d x=%.2f y=%.2f yv=%.3f onSlope=%d was=%d rot=%.1f slopeVel=%.3f slopeRot=%.3f "
                        "ground=%d\n",
                        g.tick, g.p1.x, g.p1.y, g.p1.yVelocity, g.p1.isOnSlope, g.p1.wasOnSlope, g.p1.rotation,
                        g.p1.slopeVelocity, g.p1.slopeRotation, g.p1.isOnGround);
        }
        std::printf(
                "uphill : first tick on the slope %d, %d ticks on the slope, exit tick %d yv=%.3f, max y %.2f, end "
                "x=%.1f y=%.1f dead=%d\n",
                firstOn, onSlopeTicks, exitTick, exitYv, maxY, g.p1.x, g.p1.y, g.died);
        check(!g.died && firstOn == 458 && onSlopeTicks == 24, "slope : runs up a slope (24 ticks on it)");

        Game g2;
        setupGame(g2, w);
        g2.p1.x = 1180;
        g2.p1.lastX = 1180;
        g2.p1.y = 165;
        g2.p1.lastY = 165;
        onSlopeTicks = 0;
        firstOn = -1;
        exitTick = -1;
        float minY = 1000;
        while (!g2.died && g2.p1.x < 1400 && g2.tick < 2000) {
            g_trace = false;
            stepTick(g2, false);
            g_trace = false;
            if (g2.p1.isOnSlope) {
                ++onSlopeTicks;
                if (firstOn < 0) firstOn = g2.tick;
            }
            if (!g2.p1.isOnSlope && g2.p1.wasOnSlope && exitTick < 0) {
                exitTick = g2.tick;
                exitYv = g2.p1.yVelocity;
            }
            if (verbose && g2.p1.x > 1250 && g2.p1.x < 1340 && g2.tick % 3 == 0)
                std::printf("  t=%d x=%.2f y=%.2f yv=%.3f onSlope=%d was=%d rot=%.1f slopeVel=%.3f ground=%d\n",
                            g2.tick, g2.p1.x, g2.p1.y, g2.p1.yVelocity, g2.p1.isOnSlope, g2.p1.wasOnSlope,
                            g2.p1.rotation, g2.p1.slopeVelocity, g2.p1.isOnGround);
            minY = std::min(minY, g2.p1.y);
        }
        std::printf(
                "downhill : first tick on the slope %d, %d ticks on the slope, exit tick %d yv=%.3f, end x=%.1f y=%.1f "
                "dead=%d\n",
                firstOn, onSlopeTicks, exitTick, exitYv, g2.p1.x, g2.p1.y, g2.died);
        check(!g2.died && onSlopeTicks == 15 && near(g2.p1.y, 105.0), "slope : runs down a slope to the ground");

        {
            World noSlope = w;
            noSlope.timeline = nullptr;
            noSlope.objs.erase(
                    std::remove_if(noSlope.objs.begin(), noSlope.objs.end(), [](Obj const& o) { return o.uid == 5; }),
                    noSlope.objs.end());
            noSlope.build();
            Game g5;
            setupGame(g5, noSlope);
            g5.p1.x = 1180;
            g5.p1.lastX = 1180;
            g5.p1.y = 165;
            g5.p1.lastY = 165;
            while (!g5.died && g5.p1.x < 1400 && g5.tick < 2000) {
                stepTick(g5, false);
                if (verbose && g5.p1.x > 1250 && g5.p1.x < 1300 && g5.tick % 3 == 0)
                    std::printf("  (no slope) t=%d x=%.2f y=%.2f yv=%.3f ground=%d ground2=%d\n", g5.tick, g5.p1.x,
                                g5.p1.y, g5.p1.yVelocity, g5.p1.isOnGround, g5.p1.isOnGround2);
            }
        }

        Game g3;
        setupGame(g3, w);
        g3.p1.x = 1700;
        g3.p1.lastX = 1700;
        while (!g3.died && g3.p1.x < 1900 && g3.tick < 2000) stepTick(g3, false);
        std::printf("slope-spike : dead=%d (uid %d, expected 6) x=%.1f\n", g3.died, g3.killerUid, g3.p1.x);
        check(g3.died && g3.killerUid == 6, "slope : a hazard slope kills");

        World shipWorld = w;
        shipWorld.timeline = nullptr;
        shipWorld.startMode = 1;
        shipWorld.build();
        Game g4;
        setupGame(g4, shipWorld);
        g4.p1.x = 400;
        g4.p1.lastX = 400;
        g4.p1.y = 130;
        g4.p1.lastY = 130;
        int shipSlope = -1;
        while (!g4.died && g4.p1.x < 760 && g4.tick < 2000) {
            stepTick(g4, false);
            if (g4.p1.isOnSlope && shipSlope < 0) shipSlope = g4.tick;
            if (verbose && g4.p1.x > 585 && g4.p1.x < 680 && g4.tick % 3 == 0)
                std::printf("  t=%d x=%.2f y=%.2f yv=%.3f onSlope=%d colSlope=%d rot=%.1f ground=%d\n", g4.tick,
                            g4.p1.x, g4.p1.y, g4.p1.yVelocity, g4.p1.isOnSlope, g4.p1.isCollidingWithSlope,
                            g4.p1.rotation, g4.p1.isOnGround);
        }
        std::printf("ship : first slope contact tick %d, ", shipSlope);
        std::printf("ship : end x=%.1f y=%.1f dead=%d (uid %d)\n", g4.p1.x, g4.p1.y, g4.died, g4.killerUid);
        check(!g4.died && shipSlope == 150, "slope : a ship slides up a slope");

        Game g6;
        setupGame(g6, w);
        g6.p1.x = 2200;
        g6.p1.lastX = 2200;
        bool jumped = false;
        float maxY6 = 0;
        int hitTick = -1;
        while (!g6.died && g6.p1.x < 2600 && g6.tick < 2000) {
            bool press = false;
            if (!jumped && g6.p1.x >= 2330) {
                press = true;
                jumped = true;
            }
            stepTick(g6, press);
            maxY6 = std::max(maxY6, g6.p1.y);
            if (verbose && g6.p1.x > 2380 && g6.p1.x < 2450 && g6.tick % 2 == 0)
                std::printf("  t=%d x=%.2f y=%.2f yv=%.3f onSlope=%d udSlope=%d ground=%d\n", g6.tick, g6.p1.x, g6.p1.y,
                            g6.p1.yVelocity, g6.p1.isOnSlope, g6.p1.maybeUpsideDownSlope, g6.p1.isOnGround);
            if (g6.p1.isOnSlope && hitTick < 0) hitTick = g6.tick;
        }
        std::printf(
                "ceiling slope : contact tick %d, max y %.2f (the underside of the slope is between 150 and 180), end "
                "x=%.1f dead=%d (uid %d)\n",
                hitTick, maxY6, g6.p1.x, g6.died, g6.killerUid);
        check(g6.died && g6.killerUid == 7 && maxY6 < 180.f, "slope : a jump into the underside of a slope dies on it");
        return check.failures;
    }

    // "apsim replay": plays a macro and prints the player state on the chosen ticks
    int cmdReplay(int argc, char** argv) {
        if (argc < 4) {
            std::fprintf(stderr,
                         "usage: apsim replay <level.objects.jsonl> <macro.txt> [--rec recording.jsonl] [--from T] "
                         "[--to T] [--then 0|1 N] [--trace T] [--noclip]\n");
            return 2;
        }
        World w;
        std::string err;
        if (!loadLevel(argv[2], w, &err)) {
            std::fprintf(stderr, "level : %s\n", err.c_str());
            return 1;
        }
        applyEnvContext(w);
        std::ifstream macroFile(argv[3]);
        std::string line;
        std::getline(macroFile, line);
        int from = 0;
        int to = 1 << 30;
        int thenBtn = -1;
        int thenN = 0;
        int traceTick = -1;
        Recording rec;
        Timeline tl;
        int legacyCadence = 0;
        for (int i = 4; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--rec") && i + 1 < argc) {
                if (!loadRecording(argv[++i], rec, &err)) {
                    std::fprintf(stderr, "recording : %s\n", err.c_str());
                    return 1;
                }
                timelineFromRecording(rec, w, tl, 1);
            } else if (!std::strcmp(argv[i], "--cadence") && i + 1 < argc)
                legacyCadence = std::max(1, std::atoi(argv[++i]));
            else if (!std::strcmp(argv[i], "--from") && i + 1 < argc)
                from = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--to") && i + 1 < argc)
                to = std::atoi(argv[++i]);
            else if (!std::strcmp(argv[i], "--then") && i + 2 < argc) {
                thenBtn = std::atoi(argv[++i]);
                thenN = std::atoi(argv[++i]);
            } else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc)
                traceTick = std::atoi(argv[++i]);
        }
        applyModFrames(w, legacyCadence);
        Game g;
        setupGame(g, w);
        if (hasFlag(argc, argv, "--noclip")) g.noclip = true;
        auto show = [&](int t, uint8_t b) {
            std::printf(
                    "t=%d b=%d x=%.3f y=%.3f yv=%.4f mode=%c size=%.1f ud=%d ground=%d slope=%d rot=%.1f speed=%.1f "
                    "dual=%d minY=%.1f maxY=%.1f cam=%.1f,%.1f z=%.3f st=%d%d off=%d unc=%.1f gap=%.2f amb=%d free=%d "
                    "p2=%.1f,%.1f,%c\n",
                    t, b ? 1 : 0, g.p1.x, g.p1.y, g.p1.yVelocity, modeChar(g.p1), g.p1.vehicleSize, g.p1.isUpsideDown,
                    g.p1.isOnGround, g.p1.isOnSlope, g.p1.rotation, g.p1.playerSpeed, g.isDualMode, getMinPortalY(g),
                    getMaxPortalY(g), g.cam.px, g.cam.py, g.cameraZoom, g.cam.st[0] != 0.f, g.cam.st[1] != 0.f,
                    cameraOffscreen(g, g.p1, 0.f), g.p1.rotUncertain ? g.p1.rotUnc : 0.f,
                    g.obbGap < 1e8f ? g.obbGap : -1.f, g.rotAmb ? 1 : 0, g.isFreeMode ? 1 : 0, g.p2.x, g.p2.y,
                    modeChar(g.p2));
        };
        int halfTick = -1;
        for (int a = 4; a < argc; ++a)
            if (!std::strcmp(argv[a], "--half") && a + 1 < argc) halfTick = std::atoi(argv[++a]);
        size_t i = 0;
        for (; i < line.size() && !g.died && !g.completed; ++i) {
            uint8_t const b = static_cast<uint8_t>(line[i] >= '0' && line[i] <= '3' ? line[i] - '0' : 0);
            g_trace = (static_cast<int>(i) == traceTick);
            if (static_cast<int>(i) == halfTick) {
                uint8_t const prev =
                        static_cast<uint8_t>(i > 0 && line[i - 1] >= '0' && line[i - 1] <= '3' ? line[i - 1] - '0' : 0);
                stepTickDt(g, prev, 0.125f, true);
                stepTickDt(g, b, 0.125f, true);
            } else {
                stepTick(g, b);
            }
            g_trace = false;
            if (static_cast<int>(i) >= from && static_cast<int>(i) <= to) show(static_cast<int>(i), b);
        }
        std::printf("end of the macro : tick %zu dead=%d (uid %d) complete=%d x=%.2f y=%.2f\n", i, g.died, g.killerUid,
                    g.completed, g.p1.x, g.p1.y);

        if (thenBtn >= 0) {
            for (int k = 0; k < thenN && !g.died && !g.completed; ++k) {
                g_trace = (g.tick == traceTick);
                stepTick(g, thenBtn != 0);
                g_trace = false;
                show(g.tick - 1, thenBtn != 0);
            }
            std::printf("after : tick %d dead=%d (uid %d) complete=%d x=%.2f y=%.2f\n", g.tick, g.died, g.killerUid,
                        g.completed, g.p1.x, g.p1.y);
        }
        return 0;
    }

    // "apsim synthsearch [seed] [threads] [qy] [qv]": random level of spikes and blocks, tests the search on it
    int cmdSynthSearch(int argc, char** argv) {
        unsigned seed = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 7u;
        std::mt19937 rng(seed);
        World w;
        w.levelName = "synthsearch";
        w.maxGameplayY = 2400;
        w.startX = 0;
        w.startY = 105;
        int uid = 1;
        float x = 500;
        while (x < 12000) {
            int kind = static_cast<int>(rng() % 4);
            if (kind == 0) {
                int n = 1 + static_cast<int>(rng() % 3);
                for (int i = 0; i < n; ++i) {
                    Obj spike = makeBlock(uid++, x + 30.f * i, 105, Hazard, 6, 12);
                    spike.id = 8;
                    w.objs.push_back(spike);
                }
                x += 30.f * n;
            } else if (kind == 1) {
                int h = 1 + static_cast<int>(rng() % 2);
                int len = 2 + static_cast<int>(rng() % 4);
                for (int i = 0; i < len; ++i)
                    for (int j = 0; j < h; ++j) w.objs.push_back(makeBlock(uid++, x + 30.f * i, 105 + 30.f * j));
                if (rng() % 2) {
                    Obj spike = makeBlock(uid++, x + 30.f * (len - 1), 105 + 30.f * h, Hazard, 6, 12);
                    spike.id = 8;
                    w.objs.push_back(spike);
                }
                x += 30.f * len;
            } else if (kind == 2) {
                int len = 2 + static_cast<int>(rng() % 3);
                for (int i = 0; i < len; ++i) w.objs.push_back(makeBlock(uid++, x + 30.f * i, 165));
                x += 30.f * len;
            }
            x += 90.f + 30.f * static_cast<float>(rng() % 6);
        }
        w.length = x + 300;
        w.build();
        Game g;
        setupGame(g, w);
        SearchOptions opt;
        opt.beamWidth = 500;
        if (argc > 3) opt.threads = std::atoi(argv[3]);
        if (argc > 4) opt.quantY = static_cast<float>(std::atof(argv[4]));
        if (argc > 5) opt.quantV = static_cast<float>(std::atof(argv[5]));
        std::printf("synthetic level : %zu objects, length %.0f (about %d ticks)\n", w.objs.size(), w.length,
                    static_cast<int>(w.length / 1.29825f));
        SearchResult r = searchPath(g, opt, nullptr, [](int t, int n) {
            if ((t % 2048) == 0) std::printf("  tick %d : %d states\n", t, n);
        });
        std::printf(
                "result : %s (%s) ; %zu ticks, %d presses, %ld ticks simulated, max layer %d, %d widenings, %d soft "
                "restarts, %d back-offs, %.3f s\n",
                r.success ? "OK" : "FAILED", r.why.c_str(), r.inputs.size(), r.presses, r.stats.ticksSimulated,
                r.stats.maxLayer, r.stats.widenings, r.stats.softRestarts, r.stats.hardPops, r.stats.seconds);
        Game replay;
        setupGame(replay, w);
        for (uint8_t b : r.inputs) {
            stepTick(replay, b);
            if (replay.died) break;
        }
        std::printf("replay : dead=%d complete=%d x=%.1f\n", replay.died, replay.completed, replay.p1.x);
        if (!r.success) {
            float const deadEndX = replay.p1.x;
            for (auto const& o : w.objs)
                if (o.x > deadEndX - 400 && o.x < deadEndX + 100)
                    std::printf("  obj uid %d type %d x=%.0f y=%.0f\n", o.uid, o.type, o.x, o.y);
        }
        return r.success ? 0 : 1;
    }

    // "apsim statediff": plays two macros up to a tick and prints how the two game states differ
    int cmdStateDiff(int argc, char** argv) {
        if (argc < 6) {
            std::fprintf(stderr, "usage: apsim statediff <level> <macroA> <macroB> <tick>\n");
            return 2;
        }
        World w;
        std::string err;
        if (!loadLevel(argv[2], w, &err)) {
            std::fprintf(stderr, "level : %s\n", err.c_str());
            return 1;
        }
        applyEnvContext(w);
        auto readMac = [](char const* path) {
            std::ifstream macroFile(path);
            std::string line;
            std::getline(macroFile, line);
            std::vector<uint8_t> inputs;
            for (char c : line)
                if (c >= '0' && c <= '3') inputs.push_back(static_cast<uint8_t>(c - '0'));
            return inputs;
        };
        auto macroA = readMac(argv[3]);
        auto macroB = readMac(argv[4]);
        int const T = std::atoi(argv[5]);
        Game a;
        Game b;
        setupGame(a, w);
        setupGame(b, w);
        while (a.tick < T && !a.died && a.tick < static_cast<int>(macroA.size()))
            stepTick(a, macroA[static_cast<size_t>(a.tick)]);
        while (b.tick < T && !b.died && b.tick < static_cast<int>(macroB.size()))
            stepTick(b, macroB[static_cast<size_t>(b.tick)]);
        std::printf("A : tick %d x=%.4f y=%.4f yv=%.4f dead=%d ; B : tick %d x=%.4f y=%.4f yv=%.4f dead=%d\n", a.tick,
                    a.p1.x, a.p1.y, a.p1.yVelocity, a.died, b.tick, b.p1.x, b.p1.y, b.p1.yVelocity, b.died);
        std::printf("rng %u / %u ; objState %zu / %zu ; triggeredIDs %zu / %zu ; activatedObjectIDs %zu / %zu\n", a.rng,
                    b.rng, a.objState.size(), b.objState.size(), a.triggeredIDs.size(), b.triggeredIDs.size(),
                    a.activatedObjectIDs.size(), b.activatedObjectIDs.size());
        if (a.trig.on && b.trig.on)
            std::printf("triggers : fingerprint %08x / %08x ; heavy %016llx / %016llx ; dyn %s ; player part %s\n",
                        trigHashMain(a), trigHashMain(b), static_cast<unsigned long long>(trigHeavyHash(a.trig.r())),
                        static_cast<unsigned long long>(trigHeavyHash(b.trig.r())),
                        (a.trig.d && b.trig.d) ? (trigDynEqual(*a.trig.d, *b.trig.d) ? "equal" : "different") : "?",
                        trigPvEqual(a.trig, b.trig) ? "equal" : "different");
        auto dumpObj = [&](char const* tag, Game const& game, Game const& other) {
            for (auto const& e : game.objState.items) {
                auto const* otherState = other.objState.find(e.first);
                ObjState const& s = e.second;
                if (otherState && otherState->activated == s.activated &&
                    otherState->activatedByPlayer1 == s.activatedByPlayer1 &&
                    otherState->activatedByPlayer2 == s.activatedByPlayer2 && otherState->destroyed == s.destroyed)
                    continue;
                Obj const& obj = w.objs[static_cast<size_t>(e.first)];
                std::printf(
                        "  object only/different in %s : uid %d id %d x %.0f y %.0f (act %d p1 %d p2 %d destroyed "
                        "%d)\n",
                        tag, obj.uid, obj.id, obj.x, obj.y, s.activated, s.activatedByPlayer1, s.activatedByPlayer2,
                        s.destroyed);
            }
        };
        dumpObj("A", a, b);
        dumpObj("B", b, a);
        for (auto const& k : a.triggeredIDs.items)
            if (!b.triggeredIDs.count(k)) std::printf("  trigger touched only in A : uid %d\n", k.first);
        for (auto const& k : b.triggeredIDs.items)
            if (!a.triggeredIDs.count(k)) std::printf("  trigger touched only in B : uid %d\n", k.first);
        return 0;
    }

    // "apsim pathmap": plays a macro and writes the player path and nearby objects as jsonl (for tools/pathmap.py)
    int cmdPathMap(int argc, char** argv) {
        if (argc < 5) {
            std::fprintf(stderr, "usage: apsim pathmap <level> <macro.txt> <out.jsonl> [--every N] [--noclip]\n");
            return 2;
        }
        World w;
        std::string err;
        if (!loadLevel(argv[2], w, &err)) {
            std::fprintf(stderr, "level : %s\n", err.c_str());
            return 1;
        }
        applyEnvContext(w);
        applyModFrames(w, 0);
        std::ifstream macroFile(argv[3]);
        std::string line;
        std::getline(macroFile, line);
        int every = 30;
        for (int i = 5; i < argc; ++i)
            if (!std::strcmp(argv[i], "--every") && i + 1 < argc) every = std::max(1, std::atoi(argv[++i]));
        Game g;
        setupGame(g, w);
        if (hasFlag(argc, argv, "--noclip")) g.noclip = true;
        std::FILE* f = std::fopen(argv[4], "w");
        if (!f) return 1;
        std::vector<char> seen(w.objs.size(), 0);
        std::vector<int> byX;
        std::vector<int> mobile;
        for (size_t i = 0; i < w.objs.size(); ++i) {
            Obj const& o = w.objs[i];
            if (o.type == Decoration && o.cls != 1) continue;
            if (o.cls == 1 && o.type == Modifier) continue;
            if (o.movable || w.mayHaveDyn[i] || (w.areaTarget.size() > i && w.areaTarget[i]))
                mobile.push_back(static_cast<int>(i));
            else
                byX.push_back(static_cast<int>(i));
        }
        std::sort(byX.begin(), byX.end(), [&](int a, int b) {
            return w.objs[static_cast<size_t>(a)].rect.x < w.objs[static_cast<size_t>(b)].rect.x;
        });
        auto dump = [&](int i) {
            Obj const& o = w.objs[static_cast<size_t>(i)];
            GObj obj = g.obj(i);
            Rect const r = obj.getObjectRect();
            std::fprintf(f, "{\"o\":[%d,%d,%d,%d,%.1f,%.1f,%.1f,%.1f,%d,%d]}\n", o.uid, o.id, o.type, o.cls, r.x, r.y,
                         r.w, r.h, obj.disabled() ? 1 : 0, g.tick);
        };
        auto sample = [&]() {
            float const x0 = g.p1.x - 900.f;
            float const x1 = g.p1.x + 1500.f;
            auto it = std::lower_bound(byX.begin(), byX.end(), x0 - 3000.f,
                                       [&](int a, float v) { return w.objs[static_cast<size_t>(a)].rect.x < v; });
            for (; it != byX.end() && w.objs[static_cast<size_t>(*it)].rect.x <= x1; ++it) {
                Obj const& o = w.objs[static_cast<size_t>(*it)];
                if (seen[static_cast<size_t>(*it)] || o.rect.x + o.rect.w < x0) continue;
                seen[static_cast<size_t>(*it)] = 1;
                dump(*it);
            }
            for (int i : mobile) {
                if (seen[static_cast<size_t>(i)]) continue;
                Rect const r = g.obj(i).getObjectRect();
                if (r.x + r.w < x0 || r.x > x1) continue;
                seen[static_cast<size_t>(i)] = 1;
                dump(i);
            }
        };
        sample();
        for (size_t i = 0; i < line.size() && !g.died && !g.completed; ++i) {
            uint8_t const b = static_cast<uint8_t>(line[i] >= '0' && line[i] <= '3' ? line[i] - '0' : 0);
            stepTick(g, b);
            std::fprintf(f, "{\"p\":[%d,%.2f,%.2f,%d,%d,\"%c\",%.2f,%d]}\n", g.tick - 1, g.p1.x, g.p1.y, b & 1,
                         g.p1.isUpsideDown ? 1 : 0, modeChar(g.p1), g.p1.vehicleSize, g.isDualMode ? 1 : 0);
            if (g.isDualMode) std::fprintf(f, "{\"p2\":[%d,%.2f,%.2f]}\n", g.tick - 1, g.p2.x, g.p2.y);
            if ((g.tick % every) == 0) sample();
        }
        std::fprintf(f, "{\"end\":[%d,%d,%d,%.2f,%.2f,%d]}\n", g.tick, g.died ? 1 : 0, g.killerUid, g.p1.x, g.p1.y,
                     g.completed ? 1 : 0);
        std::fclose(f);
        std::printf("map : tick %d dead=%d (killer %d) complete=%d x=%.1f y=%.1f -> %s\n", g.tick, g.died ? 1 : 0,
                    g.killerUid, g.completed ? 1 : 0, g.p1.x, g.p1.y, argv[4]);
        return 0;
    }

    // "apsim snap": plays a macro up to a tick and writes the objects around the player as jsonl (for tools/snap.py)
    int cmdSnap(int argc, char** argv) {
        if (argc < 6) {
            std::fprintf(stderr,
                         "usage: apsim snap <level> <macro.txt> <tick|-1> <out.jsonl> [--win W] [--then 0|1 N] "
                         "[--noclip]\n");
            return 2;
        }
        World w;
        std::string err;
        if (!loadLevel(argv[2], w, &err)) {
            std::fprintf(stderr, "level : %s\n", err.c_str());
            return 1;
        }
        applyEnvContext(w);
        applyModFrames(w, 0);
        std::ifstream macroFile(argv[3]);
        std::string line;
        std::getline(macroFile, line);
        int const T = std::atoi(argv[4]);
        float win = 600.f;
        int thenBtn = -1;
        int thenN = 0;
        float atX = 0;
        float atY = 0;
        bool haveAt = false;
        for (int i = 6; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--win") && i + 1 < argc)
                win = static_cast<float>(std::atof(argv[++i]));
            else if (!std::strcmp(argv[i], "--then") && i + 2 < argc) {
                thenBtn = std::atoi(argv[++i]);
                thenN = std::atoi(argv[++i]);
            } else if (!std::strcmp(argv[i], "--at") && i + 2 < argc) {
                atX = static_cast<float>(std::atof(argv[++i]));
                atY = static_cast<float>(std::atof(argv[++i]));
                haveAt = true;
            }
        }
        Game g;
        setupGame(g, w);
        if (hasFlag(argc, argv, "--noclip")) g.noclip = true;
        std::FILE* f = std::fopen(argv[5], "w");
        auto step = [&](uint8_t b) {
            stepTick(g, b);
            std::fprintf(f, "{\"p\":[%d,%.2f,%.2f,%d,%d,\"%c\",%.2f]}\n", g.tick - 1, g.p1.x, g.p1.y, b ? 1 : 0,
                         g.p1.isUpsideDown ? 1 : 0, modeChar(g.p1), g.p1.vehicleSize);
            if (g.isDualMode) std::fprintf(f, "{\"p2\":[%d,%.2f,%.2f]}\n", g.tick - 1, g.p2.x, g.p2.y);
        };
        for (size_t i = 0; i < line.size() && !g.died && !g.completed && (T < 0 || static_cast<int>(i) < T); ++i)
            step(static_cast<uint8_t>(line[i] >= '0' && line[i] <= '3' ? line[i] - '0' : 0));
        for (int k = 0; k < thenN && !g.died && !g.completed; ++k) step(thenBtn != 0);
        float const cx = haveAt ? atX : g.p1.x;
        float const cy = haveAt ? atY : g.p1.y;
        for (size_t i = 0; i < w.objs.size(); ++i) {
            Obj const& o = w.objs[i];
            if (o.type == Decoration && o.cls != 1) continue;
            GObj obj = g.obj(static_cast<int>(i));
            Rect r = obj.getObjectRect();
            if (std::fabs(r.x + r.w * 0.5f - cx) > win || std::fabs(r.y + r.h * 0.5f - cy) > win) continue;
            {
                float scaleX = o.scaleX;
                float scaleY = o.scaleY;
                if (g.trig.on) trigScaleAbs(g, static_cast<int>(i), scaleX, scaleY);
                std::fprintf(f, "{\"o\":[%d,%d,%d,%d,%.2f,%.2f,%.2f,%.2f,%d],\"rot\":%.2f,\"scl\":[%.4f,%.4f]}\n",
                             o.uid, o.id, o.type, o.cls, r.x, r.y, r.w, r.h, obj.disabled() ? 1 : 0, obj.rotation(),
                             scaleX, scaleY);
            }
        }
        std::fprintf(f, "{\"end\":[%d,%d,%d,%.2f,%.2f]}\n", g.tick, g.died ? 1 : 0, g.killerUid, cx, cy);
        std::fclose(f);
        std::printf("snapshot at tick %d (dead=%d killer %d) x=%.1f y=%.1f -> %s\n", g.tick, g.died, g.killerUid, cx,
                    cy, argv[5]);
        return 0;
    }

    // "apsim robust": measures the timing window of each press of a macro, --apply centers them
    int cmdRobust(int argc, char** argv) {
        if (argc < 4) {
            std::fprintf(stderr, "usage: apsim robust <level> <macro.txt> [--apply] [--out f] [--shift W]\n");
            return 2;
        }
        World w;
        std::string err;
        if (!loadLevel(argv[2], w, &err)) {
            std::fprintf(stderr, "level : %s\n", err.c_str());
            return 1;
        }
        applyEnvContext(w);
        std::ifstream macroFile(argv[3]);
        std::string line;
        std::getline(macroFile, line);
        std::vector<uint8_t> inputs;
        for (char c : line)
            if (c >= '0' && c <= '3') inputs.push_back(static_cast<uint8_t>(c - '0'));
        RobustOptions robustOpt;
        robustOpt.apply = false;
        std::string out;
        for (int i = 4; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--apply"))
                robustOpt.apply = true;
            else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)
                out = argv[++i];
            else if (!std::strcmp(argv[i], "--shift") && i + 1 < argc)
                robustOpt.maxShift = std::atoi(argv[++i]);
        }
        Game g;
        setupGame(g, w);
        auto t0 = std::chrono::steady_clock::now();
        RobustReport rep;
        std::vector<uint8_t> result = robustifyPlan(g, inputs, robustOpt, &rep);
        double const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf(
                "presses : %d ; measured windows : mean %.2f ticks, minimum %d ; 1 tick : %d (releases : %d) ; unknown "
                ": %d ; edges centered : %d ; validated %d (%.2f s)\n",
                rep.segments, rep.avgWindow, rep.minWindow, rep.narrow, rep.narrowRelease, rep.noConverge, rep.moved,
                rep.validated ? 1 : 0, seconds);
        std::map<int, int> hist;
        for (int v : rep.windows) ++hist[v];
        std::printf("histogram :");
        for (auto const& [k, n] : hist) std::printf(" %d:%d", k, n);
        std::printf("\n");
        if (!out.empty()) {
            std::ofstream outFile(out);
            for (uint8_t b : result) outFile << static_cast<char>('0' + b);
            outFile << "\n";
        }
        return 0;
    }

    // "apsim bench <level> [ticks] [macro] [recording]": ticks per second, random inputs or along a macro
    int cmdBench(int argc, char** argv) {
        if (argc < 3) {
            std::fprintf(stderr, "usage: apsim bench <level.objects.jsonl> [ticks] [--noclip]\n");
            return 2;
        }
        World w;
        std::string err;
        if (!loadLevel(argv[2], w, &err)) {
            std::fprintf(stderr, "level : %s\n", err.c_str());
            return 1;
        }
        applyEnvContext(w);
        long total = argc > 3 ? std::atol(argv[3]) : 2000000;
        Game g;
        setupGame(g, w);
        if (argc > 5) {
            Recording ctxRecording;
            if (loadRecording(argv[5], ctxRecording, &err)) {
                applyAttemptContext(ctxRecording, w);
                setupGame(g, w);
            }
        }
        if (hasFlag(argc, argv, "--noclip")) g.noclip = true;

        if (argc > 4) {
            std::vector<uint8_t> mac;
            FILE* fp = std::fopen(argv[4], "r");
            int ch;
            while (fp && (ch = std::fgetc(fp)) != EOF)
                if (ch >= '0' && ch <= '3') mac.push_back(static_cast<uint8_t>(ch - '0'));
            if (fp) std::fclose(fp);
            if (g.trig.on && g.trig.h) {
                TrigMotion const& motion0 = g.trig.r().M();
                if (motion0.pset)
                    std::printf("player part : %zu groups, %zu objects (%zu with collision), player triggers %s\n",
                                motion0.pset->groups.size(), motion0.pset->objs.size(), motion0.pset->collObjs.size(),
                                motion0.pset->trigObjs ? "yes" : "no");
                else
                    std::printf("player part : none\n");
            }
            auto t0 = std::chrono::steady_clock::now();
            long done = 0;
            Game h;
            constexpr int every = 2048;
            double tPrev = 0;
            double copySec = 0;
            double copySecPrev = 0;
            float xPrev = g.p1.x;
            for (int t = 0; t < static_cast<int>(mac.size()) && done < total && !g.died && !g.completed; ++t, ++done) {
                auto const c0 = std::chrono::steady_clock::now();
                h = g;
                copySec += std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
                stepTick(h, mac[static_cast<size_t>(t)]);
                g = std::move(h);
                if (((t + 1) % every) == 0) {
                    double const tNow = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    std::printf(
                            "  tick %d : %.3f s (%.1f us/tick, copy %.1f) x %.0f..%.0f mode %c%s dyn %zu cmds %zu os "
                            "%zu trig %zu\n",
                            t + 1, tNow, (tNow - tPrev) * 1e6 / every, (copySec - copySecPrev) * 1e6 / every, xPrev,
                            g.p1.x, modeChar(g.p1), g.isDualMode ? " dual" : "",
                            g.trig.on ? g.trig.rd().objDyn.size() : size_t{0},
                            g.trig.on ? g.trig.r().M().cmds.size() : size_t{0}, static_cast<size_t>(g.osSize()),
                            static_cast<size_t>(g.triggeredIDs.size()));
                    if (g.trig.on && g.trig.h) {
                        World const& W = *g.world;
                        size_t decoOnly = 0;
                        size_t decoCenter = 0;
                        size_t gameplay = 0;
                        size_t groupsRelevant = 0;
                        size_t groupsDeco = 0;
                        for (auto const& kv : g.trig.rd().objDyn) {
                            Obj const& o = W.objs[static_cast<size_t>(kv.first)];
                            if (o.type == Decoration) {
                                if (W.centerLike[static_cast<size_t>(kv.first)])
                                    ++decoCenter;
                                else
                                    ++decoOnly;
                            } else {
                                ++gameplay;
                            }
                        }
                        for (auto const& kv : g.trig.r().M().groupOff) {
                            if (W.relevantGroup(kv.first))
                                ++groupsRelevant;
                            else
                                ++groupsDeco;
                        }
                        std::printf(
                                "      own entries : %zu gameplay, %zu decoration center, %zu decoration only ; offset "
                                "groups : %zu observable, %zu decoration only\n",
                                gameplay, decoCenter, decoOnly, groupsRelevant, groupsDeco);
                    }
                    if (g.trig.on && g.trig.h && g.trig.r().M().pset) {
                        auto const& P = *g.trig.r().M().pset;
                        World const& W = *g.world;
                        size_t nDeco = 0;
                        size_t nInert = 0;
                        size_t nTrig = 0;
                        size_t nCen = 0;
                        for (int i : P.objs) {
                            Obj const& o = W.objs[static_cast<size_t>(i)];
                            if (o.type == Decoration)
                                ++nDeco;
                            else if (W.decoLikeAt(i))
                                ++nInert;
                            else if (o.cls == 1)
                                ++nTrig;
                            if (W.centerLike[static_cast<size_t>(i)]) ++nCen;
                        }
                        std::printf(
                                "      player part : %zu groups, %zu objects (%zu with collision ; %zu decorations, "
                                "%zu triggers without effect, %zu other cls 1, %zu centers), player triggers %s\n",
                                P.groups.size(), P.objs.size(), P.collObjs.size(), nDeco, nInert, nTrig, nCen,
                                P.trigObjs ? "yes" : "no");
                    }
                    tPrev = tNow;
                    xPrev = g.p1.x;
                    copySecPrev = copySec;
                }
            }
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("%ld ticks in %.3f s : %.0f ticks/s (dead %d complete %d x %.1f)\n", done, seconds,
                        done / seconds, g.died ? 1 : 0, g.completed ? 1 : 0, g.p1.x);
            return 0;
        }

        std::mt19937 rng(1);
        auto t0 = std::chrono::steady_clock::now();
        long done = 0;
        bool btn = false;
        while (done < total) {
            if ((rng() & 15) == 0) btn = !btn;
            stepTick(g, btn);
            ++done;
            if (g.died || g.completed) g.reset();
        }
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%ld ticks en %.3f s : %.0f ticks/s\n", done, seconds, done / seconds);
        return 0;
    }

}

namespace {
    // "apsim xworld": checks that no state leaks from one level to another
    int cmdXWorld(int argc, char** argv) {
        if (argc < 4) {
            std::fprintf(stderr, "usage: apsim xworld <level1[:macro]> <level2[:macro]> [...]\n");
            return 2;
        }
        std::vector<std::pair<std::string, std::string>> files;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            auto const colon = arg.find(':');
            if (colon == std::string::npos)
                files.emplace_back(arg, std::string());
            else
                files.emplace_back(arg.substr(0, colon), arg.substr(colon + 1));
        }
        constexpr int steps = 600;
        constexpr int rounds = 2;
        struct Row {
            uint32_t h;
            float x;
            float y;
        };
        int bad = 0;
        for (int round = 0; round < rounds; ++round)
            for (auto const& [file, mac] : files) {
                auto w = std::make_shared<World>();
                std::string err;
                if (!loadLevel(file, *w, &err)) {
                    std::fprintf(stderr, "%s : %s\n", file.c_str(), err.c_str());
                    return 1;
                }
                applyModFrames(*w, 0);
                std::vector<uint8_t> inputs;
                if (!mac.empty()) {
                    std::ifstream f(mac);
                    char ch;
                    while (f.get(ch))
                        if (ch >= '0' && ch <= '3') inputs.push_back(static_cast<uint8_t>(ch - '0'));
                    if (inputs.empty()) {
                        std::fprintf(stderr, "%s : empty macro\n", mac.c_str());
                        return 1;
                    }
                } else {
                    inputs.assign(static_cast<size_t>(steps), 0);
                }
                Game g;
                setupGame(g, *w);
                Game ref;
                std::vector<Row> refTrace;
                std::thread([&] {
                    setupGame(ref, *w);
                    Game r = ref;
                    for (size_t t = 0; t < inputs.size() && !r.died && !r.completed; ++t) {
                        stepTick(r, inputs[t]);
                        refTrace.push_back({trigHash(r), r.p1.x, r.p1.y});
                    }
                }).join();
                auto advOk = [&](Game const& x) {
                    if (!x.trig.on || !x.trig.h) return true;
                    for (auto const& adv : x.trig.r().M().advs)
                        if (adv.def < 0 || static_cast<size_t>(adv.def) >= w->advDefs.size()) return false;
                    return true;
                };
                int trigDiff = -1;
                auto replay = [&](Game x) {
                    for (size_t t = 0; t < refTrace.size(); ++t) {
                        stepTick(x, inputs[t]);
                        if (!advOk(x)) return -2 - x.tick;
                        if (x.p1.x != refTrace[t].x || x.p1.y != refTrace[t].y) return x.tick;
                        if (trigDiff < 0 && trigHash(x) != refTrace[t].h) trigDiff = x.tick;
                    }
                    return -1;
                };
                size_t const advCount = g.trig.on && g.trig.h ? g.trig.r().M().advs.size() : 0;
                size_t const refAdvCount = ref.trig.on && ref.trig.h ? ref.trig.r().M().advs.size() : 0;
                bool const okAdv = advOk(g);
                bool const same0 = trigHash(g) == trigHash(ref) && g.p1.x == ref.p1.x && g.p1.y == ref.p1.y;
                int divOther = -1;
                int divHere = -1;
                if (okAdv) {
                    std::thread([&] { divOther = replay(g); }).join();
                    divHere = replay(g);
                }
                bool const ok = okAdv && same0 && divOther == -1 && divHere == -1;
                std::string name = file;
                if (auto slash = name.find_last_of('/'); slash != std::string::npos) name = name.substr(slash + 1);
                auto divTxt = [](char const* where, int d) {
                    return d == -1   ? std::string()
                           : d <= -2 ? ", " + std::string(where) + " : foreign advanced follow at tick " +
                                               std::to_string(-2 - d)
                                     : ", " + std::string(where) + " : diverges at tick " + std::to_string(d);
                };
                std::printf("[%d] %-48s world %p : %zu ticks, advs %zu (ref %zu), %zu defs, start %s%s%s%s%s\n", round,
                            name.c_str(), static_cast<void*>(w.get()), refTrace.size(), advCount, refAdvCount,
                            w->advDefs.size(), same0 ? "same" : "DIFFERENT",
                            okAdv ? "" : ", ADVANCED FOLLOW OF ANOTHER LEVEL", divTxt("new thread", divOther).c_str(),
                            divTxt("this thread", divHere).c_str(),
                            trigDiff >= 0 ? (" (triggers : gap without effect on the player at tick " +
                                             std::to_string(trigDiff) + ")")
                                                    .c_str()
                                          : "");
                std::fflush(stdout);
                if (!ok) ++bad;
            }
        std::printf("%s\n", bad ? "xworld : CONTAMINATION from one level to another" : "xworld : OK");
        return bad ? 1 : 0;
    }
}

// reads the command and runs it
namespace {
    // "apsim test": runs every self test on the handmade levels (synth, synthcore, synthmove, synthslope); exits
    // with 1 when a check fails
    int cmdTest() {
        struct Suite {
            char const* name;
            int (*run)();
        };
        Suite const suites[] = {{"synth", [] { return cmdSynth(false); }},
                                {"synthcore", [] { return cmdSynthCore(); }},
                                {"synthmove", [] { return cmdSynthMove(); }},
                                {"synthslope", [] { return cmdSynthSlope(false); }}};
        int failures = 0;
        for (auto const& suite : suites) {
            std::printf("== %s\n", suite.name);
            failures += suite.run();
        }
        std::printf("%s (%d failed check(s))\n", failures ? "test : FAILED" : "test : OK", failures);
        return failures ? 1 : 0;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: apsim test|verify|solve|replay|bench|synth ...\n");
        return 2;
    }
    ap::sim::g_rngLogFrom = ap::sim::debugOptions().rngLogFrom;
    ap::sim::g_rngLogTo = ap::sim::debugOptions().rngLogTo;
    if (!std::strcmp(argv[1], "verify")) return cmdVerify(argc, argv);
    if (!std::strcmp(argv[1], "info")) return cmdInfo(argc, argv);
    if (!std::strcmp(argv[1], "test")) return cmdTest();
    if (!std::strcmp(argv[1], "synth")) return cmdSynth(true) ? 1 : 0;
    if (!std::strcmp(argv[1], "bench")) return cmdBench(argc, argv);
    if (!std::strcmp(argv[1], "synthsearch")) return cmdSynthSearch(argc, argv);
    if (!std::strcmp(argv[1], "solve")) return cmdSolve(argc, argv);
    if (!std::strcmp(argv[1], "synthmove")) return cmdSynthMove() ? 1 : 0;
    if (!std::strcmp(argv[1], "synthcore")) return cmdSynthCore() ? 1 : 0;
    if (!std::strcmp(argv[1], "synthslope")) return cmdSynthSlope(true) ? 1 : 0;
    if (!std::strcmp(argv[1], "replay")) return cmdReplay(argc, argv);
    if (!std::strcmp(argv[1], "snap")) return cmdSnap(argc, argv);
    if (!std::strcmp(argv[1], "pathmap")) return cmdPathMap(argc, argv);
    if (!std::strcmp(argv[1], "statediff")) return cmdStateDiff(argc, argv);
    if (!std::strcmp(argv[1], "robust")) return cmdRobust(argc, argv);
    if (!std::strcmp(argv[1], "xworld")) return cmdXWorld(argc, argv);
    std::fprintf(stderr, "unknown command\n");
    return 2;
}