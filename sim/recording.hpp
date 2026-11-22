// Recording types: one row per player per tick, and the moving objects.

#pragma once
#include "player.hpp"
#include "timeline.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ap::sim {

    struct RecRow {
        int t = 0;
        int p = 1;
        bool held = false;
        float x = 0;
        float y = 0;
        float nx = 0;
        float ny = 0;
        float rot = 0;
        double yv = 0;
        float xs = 0;
        double sm = 1;
        double g = 0;
        double ys = 0;
        float gm = 1;
        float vs = 1;
        char mode = 'C';
        uint32_t flags = 0;
        double tt = 0;
        double ljt = 0;
        double llt = 0;
        double lft = 0;
        float sa = 0;
        float sv = 0;
        float sar = 0;
        int csl = 0;
        int lgo = 0;
        int col = 0;
        int ost = 0;
        int lap = 0;
        double snap = 0;
        double fs = 0;
        double yvbs = 0;
        double ctmy = 0;
        double cbmy = 0;
        double clmx = 0;
        double crmx = 0;
        double gyv = 0;
        double csyv = 0;
        float tw = 1;
        float pmin = -1;
        float pmax = -1;
        bool zc = false;
        float cz = 0;
        float coy = 0;
        bool cs = false;
        int ta = 0;
        bool dual = false;
        float hb = 30;
        float pw = 30;
        float ph = 30;
        float rs = 0;
        float fsy = 0;
        float lpx = 0;
        float lpy = 0;
        double aos = 0;
        int ci = 0;
        int so = 0;
        int sna = 0;
        int shh = 0;
        int sfg = 0;
        int sds = 0;
        int ucol[4] = {-1, -1, -1, -1};
        int unk[2] = {-1, -1};
        bool ia = false;
        bool rj = false;
        bool hasRng = false;
        uint32_t rng = 0;
        bool lt = false;

        // reads flag number i
        bool flag(int i) const { return (flags >> i) & 1u; }

        enum : int {
            OnGround = 0,
            OnGround2,
            OnGround3,
            OnGround4,
            OnSlope,
            WasOnSlope,
            UpsideDown,
            Dashing,
            Accelerating,
            JumpBuffered,
            StateRingJump,
            StateJumpBuffered,
            WasJumpBuffered,
            TouchedRing,
            TouchedPad,
            MaybeBoosted,
            Locked,
            Sliding,
            Dead,
            RingJumpRelated,
            HasEverJumped,
            GoingLeft,
            Sideways,
            WasRobotJump,
            TouchedCustomRing,
            TouchedGravityPortal,
            CollidingWithSlope,
            StateRingJump2,
            PadRingRelated,
            MaybeFalling,
            CurrentSlopeTop,
            HasEverHitRing
        };
    };

    struct Recording {
        std::string level;
        int levelId = 0;
        int attempt = 0;
        float length = 0;
        bool dual = false;
        bool practice = false;
        bool platformer = false;
        bool cbs = false;
        bool cos = false;
        float startX = 0;
        float startY = 0;
        float groundP1 = 105.f;
        double gravity = 0;
        double yStart = 0;
        float speed = 0;
        float vehicleSize = 1;
        std::vector<RecRow> rows;
        std::vector<RecRow> rows2;
        std::string endWhy;
        int deathTick = -1;
        int deathKiller = 0;
        int deathKillerId = 0;
        int deathHeld = -1;
        float objWin = 0;
        P2Leftover p2l;
        bool hasRng = false;
        uint32_t rng = 0;
        bool hasTpRng = false;
        uint32_t tpRng = 0;
        bool hasRng0 = false;
        uint32_t rng0 = 0;
        bool hasRngSeed = false;
        uint32_t rngSeed = 0;
        std::vector<std::pair<int, uint8_t>> preInputs;
        int gameAttempts = -1;
        int modPlays = -1;
        bool hasPersist = false;
        std::vector<std::pair<int, int>> persistentItems;
        std::vector<int> persistentTimers;
        float deathX = 0;
        float deathY = 0;
        int completeTick = -1;
        std::vector<TimelineEvent> objEvents;
    };

    // first tick of each game frame, from the "lt" marks or else from player 1 position changes, empty if unknown
    inline std::vector<int> recordingFrameStarts(Recording const& r) {
        std::vector<int> s;
        if (r.rows.size() < 2) return s;
        bool anyLt = false;
        for (auto const& row : r.rows) {
            if (row.lt) {
                anyLt = true;
                break;
            }
        }
        s.push_back(r.rows[0].t);
        size_t moves = 0;
        for (size_t i = 1; i < r.rows.size(); ++i) {
            RecRow const& a = r.rows[i - 1];
            RecRow const& b = r.rows[i];
            bool start = b.t != a.t + 1;
            if (!start) start = anyLt ? a.lt : (b.x != a.x || b.y != a.y);
            if (start) {
                s.push_back(b.t);
                ++moves;
            }
        }
        if (!anyLt && moves * 16 < r.rows.size()) s.clear();
        return s;
    }

    bool loadRecording(std::string const& path, Recording& rec, std::string* err = nullptr);

}
