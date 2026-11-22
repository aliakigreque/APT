// Trigger state of a simulated game: groups, commands, items, areas, and the functions that read it.

#pragma once
#include <atomic>
#include <array>
#include "containers.hpp"
#include "world.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ap::sim {

    struct Game;
    struct Player;

    // kind of a GroupCmd: which trigger created it
    namespace CmdType {
        inline constexpr uint8_t Move = 0;
        inline constexpr uint8_t Rotate = 1;
        inline constexpr uint8_t Follow = 2;
        inline constexpr uint8_t FollowY = 3;
        inline constexpr uint8_t Transform = 4;
        inline constexpr uint8_t Keyframe = 5;
    }

    struct GroupCmd {
        int uid;
        int control;
        int target;
        int center;
        uint8_t type;
        bool finished;
        bool disabled;
        bool finishRelated;
        bool lockPX;
        bool lockPY;
        bool lockCX;
        bool lockCY;
        bool lockedInX;
        bool lockedInY;
        bool lockObjRot;
        int easing;
        double rate;
        double duration;
        double deltaTime;
        float deltaTimeF;
        int actionType1;
        int actionType2;
        int kf;
        double value1;
        double value2;
        double curX;
        double curY;
        double deltaX;
        double deltaY;
        double modX;
        double modY;
        double rotVal;
        double rotDelta;
        double rotAcc;
        double rotAccPrev;
        double followXMod;
        double followYMod;
        double fySpeed;
        double fyDelay;
        double fyMax;
        int fyOffset;
        bool alreadyUpdated;
        bool processed;
        bool doUpdate;
        bool relRot;
        uint64_t kfDyn;
        int kfSpawned;
        int kfCenterP1;
        SmallVec<int, 6> kfKeys;
        GroupCmd();
    };

    struct GroupOff {
        double x;
        double y;
        double px;
        double py;
        int stamp;
        GroupOff();
    };

    struct ObjDyn {
        double x;
        double y;
        double secX;
        double secY;
        float rotAcc;
        float rotAccY;
        float lastX;
        float lastY;
        int stamp;
        bool rotated;
        bool scaled;
        bool outerOb;
        float sclX;
        float sclY;
        float cSclX;
        float cSclY;
        float sclOffX;
        float sclOffY;
        int tStamp;
        ObjDyn();
    };

    struct AreaAnim {
        float from;
        float to;
        float dur;
        float elapsed;
        float rate;
        int easing;
        uint8_t on;
    };

    struct AreaInst {
        int def;
        int target;
        int center;
        int control;
        int counter;
        bool paused;
        float v[28];
        AreaAnim a[28];
        AreaInst();
    };

    struct AreaOff {
        float dx;
        float dy;
        float rot;
        float sx;
        float sy;
        float pdx;
        float pdy;
        float psx;
        float psy;
        int stamp;
        int latchStamp;
        uint8_t latch;
        uint8_t sclOn;
        uint8_t psclOn;
        AreaOff();
    };

    using RemapKeys = SmallVec<int, 6>;

    struct SpawnAction {
        double delay;
        double elapsed;
        int group;
        int uid;
        int control;
        int obj;
        bool ordered;
        bool finished;
        bool paused;
        RemapKeys keys;
        SpawnAction();
    };

    struct CountAction {
        int item;
        int target;
        int group;
        int uid;
        int control;
        int prev;
        bool activate;
        bool multi;
        bool stopped;
        bool paused;
        RemapKeys keys;
        CountAction();
    };

    struct CollisionAction {
        int a;
        int b;
        int group;
        int uid;
        int control;
        bool onExit;
        bool activate;
        bool stopped;
        bool paused;
        RemapKeys keys;
        CollisionAction();
    };

    struct TouchAction {
        int group;
        int uid;
        int control;
        int toggleMode;
        int playerMode;
        bool hold;
        bool dual;
        bool stopped;
        bool paused;
        RemapKeys keys;
        TouchAction();
    };

    struct Tween {
        float from;
        float to;
        float duration;
        float elapsed;
        float value;
        float rate;
        int easing;
        int uid;
        int control;
        bool finished;
        bool paused;
        Tween();
    };

    struct TimerItem {
        int id;
        double time;
        double target;
        float mod;
        bool running;
        bool stopEnabled;
        bool ignoreTW;
        bool disabled;
        int group;
        int uid;
        int control;
        RemapKeys keys;
        TimerItem();
    };

    struct TimeEvent {
        int timer;
        float last;
        float target;
        int group;
        int uid;
        int control;
        bool multi;
        bool done;
        RemapKeys keys;
        TimeEvent();
    };

    struct EventListener {
        int target;
        int uid;
        int control;
        int paused;
        RemapKeys keys;
        EventListener();
    };

    struct AdvInst {
        int def;
        int target;
        int key;
        int control;
        int order;
        uint8_t finished;
        uint8_t doStart;
        uint8_t stopped;
        uint8_t processed;
        uint8_t rngOnly;
        AdvInst();
    };

    struct AfPhys {
        float vx;
        float vy;
        float dx;
        float dy;
        float angle;
        float steer;
        int last;
        int stop;
        AfPhys();
    };

    struct DynRot {
        int trig;
        int uid;
        int control;
        int target;
        int center;
        int c;
        int a;
        int t;
        int mnx;
        int mny;
        int mxx;
        int mxy;
        float elapsed;
        float prevD;
        float eased;
        uint8_t first;
        uint8_t stop;
        uint8_t pause;
        uint8_t pauseOnce;
        uint8_t kind;
        DynRot();
    };

    struct SeqState {
        float last;
        int count;
        SeqState();
    };

    int trigThreadId();
    extern thread_local long g_trigCounters[8];
    extern thread_local int g_noShare;

    struct TrigDyn {
        FlatMap<int, ObjDyn, 16> objDyn;
        FlatMap<int, AfPhys> afPhys;
        std::vector<int> afObjs;
        std::vector<uint64_t> dynGroups;
        mutable uint64_t hashValue = 0;
        mutable bool hashValid = false;
        mutable uint64_t keysValue = 0;
        mutable bool keysValid = false;
        mutable uint64_t exclValue = 0;
        mutable void const* exclSet = nullptr;
        uint64_t ver = 0;
        int owner = trigThreadId();
        // empty dynamic trigger state
        TrigDyn() = default;
        // copies everything except owner, which becomes the copying thread
        TrigDyn(TrigDyn const& o)
            : objDyn(o.objDyn),
              afPhys(o.afPhys),
              afObjs(o.afObjs),
              dynGroups(o.dynGroups),
              hashValue(o.hashValue),
              hashValid(o.hashValid),
              keysValue(o.keysValue),
              keysValid(o.keysValid),
              exclValue(o.exclValue),
              exclSet(o.exclSet),
              ver(o.ver),
              owner(trigThreadId()) {}
        TrigDyn& operator=(TrigDyn const&) = default;
    };

    // new unique content version (thread id in the high bits, thread counter in the low ones)
    inline uint64_t trigNextVer() {
        thread_local uint64_t const base = static_cast<uint64_t>(trigThreadId()) << 44;
        thread_local uint64_t n = 0;
        return base | ++n;
    }

    struct PSet {
        std::vector<uint64_t> groupBits;
        std::vector<uint64_t> objBits;
        std::vector<uint64_t> memberGroupBits;
        std::vector<int> groups;
        std::vector<int> objs;
        std::vector<int> collObjs;
        bool trigObjs = false;
        uint64_t sig = 0;
    };

    struct TrigPv {
        FlatMap<short, GroupOff> off;
        FlatMap<int, ObjDyn, 4> dyn;
        FlatMap<int, AfPhys> af;
        std::vector<int> afObjs;
        uint64_t dynBloom = 0;
        std::vector<std::pair<double, double>> kfSnap;
        uint64_t ver = 0;
        mutable uint64_t hashValue = 0;
        mutable bool hashValid = false;
    };

    struct TrigMotion {
        uint64_t ver = 0;
        FlatMap<short, GroupOff> groupOff;
        std::vector<GroupCmd> cmds;
        struct CellOv {
            std::shared_ptr<std::vector<int>> v;
            mutable uint64_t hash = 0;
            mutable bool valid = false;
        };
        FlatMap<int, CellOv> cellOv;
        std::shared_ptr<PSet const> pset;
        std::vector<uint64_t> movedGroups;
        std::vector<AdvInst> advs;
        int advOrder = 0;
        bool advDirty = false;
        std::vector<DynRot> drots;
        FlatMap<int, int> blkCell;
        std::vector<std::pair<double, double>> kfSnap;
        mutable uint64_t movedHash = 0;
        mutable bool movedHashValid = false;
        mutable uint64_t hashValue = 0;
        mutable bool hashValid = false;
    };

    struct AreaLists {
        std::vector<AreaInst> L[3];
        mutable std::atomic<uint64_t> hash{0};
        mutable std::atomic<bool> hashValid{false};
        // empty area lists
        AreaLists() = default;
        // copies the lists, the hash starts invalid
        AreaLists(AreaLists const& o) : L{o.L[0], o.L[1], o.L[2]} {}
        AreaLists& operator=(AreaLists const&) = delete;
    };

    struct AreasRef {
        std::shared_ptr<AreaLists> p = emptyAreaLists();

        // area list number k
        std::vector<AreaInst> const& operator[](int k) const { return p->L[k]; }

        // empty lists shared by everyone
        static std::shared_ptr<AreaLists> const& emptyAreaLists() {
            static std::shared_ptr<AreaLists> const e = std::make_shared<AreaLists>();
            return e;
        }
    };

    struct TrigHeavy {
        std::shared_ptr<TrigMotion> mo = std::make_shared<TrigMotion>();

        // read movement part
        TrigMotion const& M() const { return *mo; }

        // write movement part, copies it first if shared
        TrigMotion& Mw() {
            if (mo.use_count() > 1) mo = std::make_shared<TrigMotion>(*mo);
            mo->hashValid = false;
            hashValid = false;
            mo->ver = trigNextVer();
            return *mo;
        }

        std::vector<uint64_t> offBits;
        int offCount = 0;
        uint64_t offBloom = 0;
        std::vector<SpawnAction> spawnQueue;
        std::vector<CountAction> counts;
        std::vector<CollisionAction> collisions;
        std::vector<TouchAction> touches;
        FlatMap<int, int> items;
        int spawnCursor = 0;
        FlatMap<int, int> spawnCursors;
        bool anyMotion = false;
        std::vector<uint64_t> actBits;
        FlatMap<int, double> spawnStored;
        FlatMap<int, RemapKeys> remapKeys;
        uint64_t remapSum = 0;
        std::vector<TimerItem> timers;
        uint32_t timerBuckets = 8;
        std::vector<TimeEvent> timeEvents;
        FlatMap<long long, SeqState> seqs;
        FlatMap<long long, std::vector<EventListener>> events;
        int points = 0;
        std::vector<int> persistItems;
        std::vector<int> persistTimers;
        AreasRef areas;

        // write area list k, copies the lists first if shared
        std::vector<AreaInst>& areasW(int k) {
            if (areas.p.use_count() > 1) areas.p = std::make_shared<AreaLists>(*areas.p);
            areas.p->hashValid.store(false, std::memory_order_relaxed);
            hashValid = false;
            return areas.p->L[k];
        }

        int areaCounter = 0;
        uint8_t areasUnsorted = 0;
        mutable uint64_t hashValue = 0;
        mutable bool hashValid = false;
        mutable uint64_t actHash = 0;
        mutable bool actHashValid = false;
        int owner = trigThreadId();
        // empty heavy trigger state
        TrigHeavy() = default;
        // copy of the heavy trigger state
        TrigHeavy(TrigHeavy const& o) = default;
        TrigHeavy& operator=(TrigHeavy const&) = default;
    };

    struct TrigLocalCtx {
        std::vector<std::pair<TrigHeavy const*, std::shared_ptr<TrigHeavy>>> mapH;
        std::vector<std::pair<TrigDyn const*, std::shared_ptr<TrigDyn>>> mapD;
        std::vector<std::shared_ptr<TrigHeavy>> deferredH;
        std::vector<std::shared_ptr<TrigDyn>> deferredD;
        long localized = 0;
    };
    struct Game;
    void trigLocalize(Game& g, TrigLocalCtx& ctx);
    void trigLocalFlush(TrigLocalCtx& ctx);

    struct TrigState {
        bool on = false;
        bool camTrack = false;
        std::shared_ptr<TrigHeavy> h;
        std::shared_ptr<TrigDyn> d;
        std::vector<float> yHist;
        int yHistIdx = 0;
        float yHistAcc = 0;
        std::vector<float> afRing;
        unsigned afU5 = 0;
        unsigned afU6 = 0;
        int afN = 0;
        uint64_t afHashObj = 0;
        uint64_t afHashP = 0;
        std::vector<uint64_t> afRingH;
        FlatMap<int, uint8_t> collided;
        FlatSet<uint64_t> spawnedThisTick;
        float queuedTimeWarp = 0;
        FlatMap<int, Tween> tweens;
        float cameraOffsetX = 0;
        float targetOffX = 0;
        float targetOffY = 0;
        float playerDx = 0;
        float playerDy = 0;
        using AreaOffMap = FlatMap<int, AreaOff>;
        std::shared_ptr<AreaOffMap const> areaOffP;

        // objects moved by area triggers this tick, empty map if none
        AreaOffMap const& areaOff() const {
            static AreaOffMap const kEmpty;
            return areaOffP ? *areaOffP : kEmpty;
        }

        // true if no object is moved by area triggers this tick
        bool areaOffEmpty() const { return !areaOffP || areaOffP->items.empty(); }

        uint64_t areaVer = 0;
        uint64_t areaVerS = 0;
        uint64_t areaSplitSig = 0;
        uint64_t areaVerY = 0;
        std::shared_ptr<std::vector<uint64_t> const> areaYBits;
        std::vector<std::pair<long long, unsigned>> evStamps;
        std::vector<int> areaMoved;
        std::vector<std::pair<int, int>> stateBlk;
        std::shared_ptr<TrigPv> pv;

        // write player part, creates or copies it if needed
        TrigPv& wp() {
            if (!pv)
                pv = std::make_shared<TrigPv>();
            else if (pv.use_count() > 1)
                pv = std::make_shared<TrigPv>(*pv);
            pv->hashValid = false;
            pv->ver = trigNextVer();
            return *pv;
        }

        // read heavy state
        TrigHeavy const& r() const { return *h; }

        // write heavy state, copies it first if shared
        TrigHeavy& w() {
            if (h.use_count() > 1) {
                h = std::make_shared<TrigHeavy>(*h);
                h->owner = trigThreadId();
                ++g_trigCounters[0];
            }
            h->hashValid = false;
            return *h;
        }

        // read single object positions
        TrigDyn const& rd() const { return *d; }

        // write single object positions, copies them first if shared
        TrigDyn& wd() {
            if (d.use_count() > 1) {
                d = std::make_shared<TrigDyn>(*d);
                ++g_trigCounters[1];
            }
            d->hashValid = false;
            d->keysValid = false;
            d->exclSet = nullptr;
            d->ver = trigNextVer();
            return *d;
        }

        // true if a group moved
        bool anyMotion() const { return h && h->anyMotion; }
    };

    bool isSpawnableTrigger(int id);
    bool hasSimEffect(int id);
    bool shouldLockX(int id);
    char const* unsupportedReason(Obj const& o);
    bool cameraOnlyTrigger(int id);
    void analyzeTriggers(World& w);

    void trigRealPos(Game const& g, int idx, double& x, double& y);
    Rect trigRect(Game const& g, int idx);
    extern thread_local bool tl_rectCacheOn;
    void trigPosCacheScopeBegin(bool& prev);
    void trigPosCacheScopeEnd(bool prev);
    Rect trigRectCached(Game const& g, int idx);
    void trigLastPos(Game const& g, int idx, float& x, float& y);
    void trigNodePos(Game const& g, int idx, float& x, float& y);
    float trigRotation(Game const& g, int idx);
    bool trigScaleAbs(Game const& g, int idx, float& sx, float& sy);
    void trigScalePosDelta(Game const& g, int idx, float& dx, float& dy);
    bool trigRotated(Game const& g, int idx);
    bool trigGroupDisabled(Game const& g, Obj const& o);
    bool trigGroupOn(Game const& g, int group);
    std::vector<int> const* trigCell(Game const& g, int cx, int cy);
    bool trigObjMoved(Game const& g, Obj const& o);
    AreaOff const* trigAreaOff(Game const& g, int idx);
    bool trigHasDyn(Game const& g, int idx);
    void trigSharedPos(Game const& g, int idx, double& x, double& y);
    void trigDebugObj(Game const& g, int idx);
    void trigAreaActions(Game& g, double dtSec);
    void trigAreaCells(Game const& g, int idx, int& gcx, int& gcy, int& ccx, int& ccy);
    void trigAreaCellsList(Game const& g, std::vector<std::array<int, 5>>& out, int wx0, int wx1, int wy0, int wy1);

    void trigReset(Game& g);
    void trigBeginTick(Game& g, double dtSec, bool secondHalf = false);
    void trigButton(Game& g, bool down, bool isPlayer1);
    uint64_t trigTouchSig(Game const& g);
    void trigMoveActions(Game& g, double dtSec, float dtPhys);
    void trigFrameSnap(Game& g);
    bool trigGroupOffset(Game const& g, int group, double& dx, double& dy);
    void trigCollisionBlocks(Game& g);
    void trigCheckSpawnObjects(Game& g);
    void trigUpdateTimers(Game& g, double dtSec);
    void trigEndTick(Game& g, double dtSec);
    void trigStateObjects(Game& g);
    void trigAfCommands(Game& g, bool half);
    void trigAfRecord(Game& g);
    void trigPrune(Game& g, float behindX);

    void triggerObject(Game& g, int idx, int playerUid);
    void trigPlayerTouched(Game& g, Player& p, int idx);
    void trigCustomRing(Game& g, Player& p, int idx);
    void trigGameEvent(Game& g, int event, int extra = 0);
    extern thread_local Game* g_evGame;

    struct EventGameScope {
        Game* prev;

        // sets the game that receives events for this scope
        explicit EventGameScope(Game* g) : prev(g_evGame) { g_evGame = g; }

        // puts back the previous game at the end of the scope
        ~EventGameScope() { g_evGame = prev; }
    };

    // sends a game event from player code, if a game is running
    inline void playerGameEvent(int event, int extra = 0) {
        if (g_evGame) trigGameEvent(*g_evGame, event, extra);
    }

    int objectTypeToGameEvent(int type);
    void trigCollected(Game& g, Player& p, int idx);
    void trigCoin(Game& g, int idx);
    bool trigActivatedBit(Game const& g, int idx);
    int trigPersistChanged(Game const& g);
    int trigItemValue(Game const& g, int item);
    extern std::atomic<long> g_areaMemoStat[5];
    extern std::atomic<long> g_areaSplitStat[4];
    void trigSetActivatedBit(Game& g, int idx);
    bool trigHasSlot(Game const& g, int idx);

    uint32_t trigHash(Game const& g);
    uint32_t trigHashMain(Game const& g);
    uint32_t trigHashLight(Game const& g, uint64_t h);
    uint32_t trigHashLightNoPv(Game const& g, uint64_t h);
    uint64_t trigHeavyHash(TrigHeavy const& h);
    uint64_t trigAreasHash(TrigHeavy const& h);
    void trigHeavyDiff(TrigHeavy const& a, TrigHeavy const& b);
    void trigTweenValue(Game& g, int key, float from, float to, float duration, int easing, float rate, int uid,
                        int control);
    void trigUpdateDualGroundAfterCamera(Game& g);
    void debugSetCorridor(Game& g, int uid);
    uint64_t trigDynHash(TrigDyn const& d);
    bool trigDynEqual(TrigDyn const& a, TrigDyn const& b);
    bool trigPvEqual(TrigState const& a, TrigState const& b);
    void trigAddPMoved(Game& g);
    bool trigMovedGameplayIn(Game& g, float x0, float x1, float y0, float y1);
    void trigMemoStats(long& hits, long& miss, long& hashes);
    void trigMemoNewEpoch();
    long const* trigMemoDbg();

    float easedValue(float t, int type, float rate);

}
