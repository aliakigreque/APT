// Helpers, types and per-thread state shared by the trigger files (triggers*.cpp). Only those files include it;
// the rest of the simulator goes through triggers.hpp.

#pragma once
#include <map>
#include <list>
#include <string>
#include <array>
#include <set>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <algorithm>
#include <climits>
#include <atomic>
#include <cmath>
#include <math.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <limits>
#include <optional>
#include "gd.hpp"
#include "crtsort.hpp"
#include "triggers.hpp"
#include "camera.hpp"
#include "msvc_sort.hpp"

namespace ap::sim::trigger_impl {
    // what a stage of a split function tells its caller: go on, or return now
    enum class Flow { Next, Continue, Break, Return };

    inline thread_local int tl_dynDep = 0;

    // raises the dependency level
    inline void dynDep(int level) {
        if (tl_dynDep < level) tl_dynDep = level;
    }

    inline thread_local bool tl_canon = false;

    inline thread_local bool tl_canonRead = false;

    inline thread_local bool tl_pHit = false;

    inline thread_local bool tl_memoComp = false;

    // bit i of a bit array stored in 64 bit words
    inline bool bitOf(std::vector<uint64_t> const& bits, int i) {
        if (i < 0) return false;
        size_t const word = static_cast<size_t>(i) >> 6;
        return word < bits.size() && ((bits[word] >> (i & 63)) & 1u);
    }

    // true once the level has a player part (thread safe read)
    inline bool pBornOf(World const& w) { return std::atomic_ref<bool>(w.pBorn).load(std::memory_order_acquire); }

    // player groups / objects of g, or nullptr
    inline PSet const* psetOf(Game const& g) {
        return (pBornOf(*g.world) && g.trig.h && g.trig.h->mo) ? g.trig.h->mo->pset.get() : nullptr;
    }

    // true if group is a player group
    inline bool isPG(Game const& g, int group) {
        PSet const* pset = psetOf(g);
        return pset && bitOf(pset->groupBits, group);
    }

    // true if object idx is a player object
    inline bool isPO(Game const& g, int idx) {
        PSet const* pset = psetOf(g);
        return pset && bitOf(pset->objBits, idx);
    }

    // marks the computation as not shareable
    inline void pHitAt() { tl_pHit = true; }

    // call before reading per state data during a shared computation
    inline void pRead() {
        if (tl_memoComp || tl_canonRead) pHitAt();
    }

    struct UndoE;

    struct Journal;

    GroupOff const* undoLookup(short gr, bool& hit);

    inline thread_local bool tl_undoOn = false;

    // current offset of a group (player part, undo journal, then shared movement state), nullptr if never moved
    inline GroupOff const* offFind(Game const& g, short group) {
        if (!tl_canonRead && g.trig.pv) {
            PSet const* pset = psetOf(g);
            if (pset && bitOf(pset->groupBits, group)) {
                if (tl_memoComp) pHitAt();
                if (auto* off = g.trig.pv->off.find(group)) return off;
            }
        }
        if (tl_undoOn) {
            bool hit;
            GroupOff const* off = undoLookup(group, hit);
            if (hit) return off;
        }
        return g.trig.r().M().groupOff.find(group);
    }

    inline thread_local std::vector<int> const* tl_fyExcl = nullptr;

    inline thread_local unsigned tl_fyGen = 0;

    inline thread_local bool tl_fyHit = false;

    inline thread_local long tl_fySite[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    // flags the computation if it touches an excluded object, counts it per call site
    inline void fyCheck(int idx, int site = 0) {
        if (tl_fyExcl && std::binary_search(tl_fyExcl->begin(), tl_fyExcl->end(), idx)) {
            tl_fyHit = true;
            ++tl_fySite[site & 7];
        }
    }

    inline thread_local unsigned tl_itemDelta = 0;

    inline thread_local bool tl_sideEffect = false;

    float bounceTime(float time);

    // powf called through a volatile pointer: the compiler can neither fold it at compile time nor swap it for a
    // builtin, so every call is a real CRT powf call, like in the game
    inline float (*volatile const g_powf)(float, float) = ::powf;

    // powf of the game, through the crt
    inline float gpow(float base, float exponent) { return g_powf(base, exponent); }

    void kfSplineSet(World::KfSpline& spline, std::vector<double> const& x, std::vector<double> const& y);

    double kfSplineEval(World::KfSpline const& spline, double t);

    void buildKeyframeAnim(World const& w, std::vector<int> const& keys,
                           std::vector<World::KfKeyState> const& keyStates, World::KfAnim& anim,
                           std::vector<World::KfSpline>& splines, float timeMod, float posXMod, float posYMod,
                           float rotMod, float scaleXMod, float scaleYMod);

}

namespace ap::sim {
    void analyzeMenus(World& w);

}

namespace ap::sim::trigger_impl {
    struct RemapOverride {
        int idx = -1;
        Obj const* obj = nullptr;
        RemapKeys const* parentKeys = nullptr;
    };

    inline thread_local RemapOverride tl_remap;

    // object at idx, or its remapped copy while it's being triggered
    inline Obj const& objAt(Game const& g, int idx) {
        if (idx == tl_remap.idx) [[unlikely]]
            return *tl_remap.obj;
        return g.world->objs[static_cast<size_t>(idx)];
    }

    // time stamp of the current step, m_commandIndex of the game
    inline int tickStamp(Game const& g) { return static_cast<int>(g.commandIndex); }

    // saves the current scale once per step, before the object gets transformed
    inline void sclPrev(Game const& g, ObjDyn& d) {
        if (d.tStamp != tickStamp(g)) {
            d.cSclX = d.sclX;
            d.cSclY = d.sclY;
            d.tStamp = tickStamp(g);
        }
    }

    inline thread_local WKey tl_physWorld;

    inline thread_local std::unordered_map<int, std::vector<int>> tl_physOfGroup;

    inline thread_local std::vector<std::vector<int> const*> tl_physDense;

    std::vector<int> const& physOfGroup(World const& w, int group);

    bool hasNonPMember(Game const& g, PSet const* pset, int group);

    struct GroupSpatial {
        std::vector<std::pair<float, int>> fixedX;
        std::vector<int> mobile;
        std::vector<short> groups;
    };

    inline thread_local WKey tl_spWorld;

    inline thread_local std::unordered_map<int, GroupSpatial> tl_spatial;

    GroupSpatial const& spatialOfGroup(World const& w, int group);

    // real position of an object without its own entry: start position + group offsets
    inline void groupPos(Game const& g, Obj const& o, double& x, double& y) {
        x = o.posX;
        y = o.posY;
        TrigHeavy const& heavy = g.trig.r();
        size_t const wordCount = heavy.M().movedGroups.size();
        for (short gr : o.groups) {
            size_t const word = static_cast<size_t>(static_cast<unsigned short>(gr)) >> 6;
            if (gr < 0 || word >= wordCount || !((heavy.M().movedGroups[word] >> (gr & 63)) & 1u)) continue;
            if (auto* off = offFind(g, gr)) {
                if (!o.lockX) x += off->x;
                y += off->y;
            }
        }
    }

    // m_lastPosition of an object without its own entry: position before the last tick one of its groups moved
    inline void groupPrevPos(Game const& g, Obj const& o, double& x, double& y) {
        x = o.posX;
        y = o.posY;
        int last = -1;
        for (short gr : o.groups)
            if (auto* off = offFind(g, gr)) last = std::max(last, off->stamp);
        for (short gr : o.groups)
            if (auto* off = offFind(g, gr)) {
                bool const movedLast = off->stamp == last;
                if (!o.lockX) x += movedLast ? off->px : off->x;
                y += movedLast ? off->py : off->y;
            }
    }

    // node position at the start of the current tick
    inline void groupStartPos(Game const& g, Obj const& o, double& x, double& y) {
        x = o.posX;
        y = o.posY;
        for (short gr : o.groups)
            if (auto* off = offFind(g, gr)) {
                bool const moved = off->stamp == tickStamp(g);
                if (!o.lockX) x += moved ? off->px : off->x;
                y += moved ? off->py : off->y;
            }
    }

    // flags a group as moved in the motion state
    inline void markGroupMoved(TrigHeavy& heavy, short gr) {
        if (gr < 0) return;
        size_t const word = static_cast<size_t>(gr) >> 6;
        uint64_t const bit = 1ull << (gr & 63);
        {
            auto const& moved = heavy.M().movedGroups;
            if (word < moved.size() && (moved[word] & bit)) return;
        }
        TrigMotion& motion = heavy.Mw();
        if (motion.movedGroups.size() <= word) motion.movedGroups.resize(word + 1, 0);
        motion.movedGroups[word] |= bit;
        motion.movedHashValid = false;
    }

    ObjDyn& dynEntryP(Game& g, int idx);

    ObjDyn& dynEntry(Game& g, int idx);

    ObjDyn& dynEntryP(Game& g, int idx);

    bool anyDynInGroup(Game const& g, int group);

    inline thread_local WKey tl_sgWorld;

    inline thread_local std::unordered_map<int, std::vector<int>> tl_sortedGroup;

    inline thread_local std::unordered_map<int, std::vector<int>> tl_sortedTracked;

    inline thread_local WKey tl_stWorld;

    std::vector<int> const& sortedTrackedOfGroup(World const& w, int group);

    void dynPositionsInGroup(Game const& g, int group, std::vector<size_t>& out);

    struct FollowYCmd {
        int target;
        double fySpeed;
        double fyMax;
        double fyDelay;
        int fyOffset;
    };

    struct JTerm {
        double dx;
        double dy;
        double modX;
        double modY;
        uint8_t lock;
        int dm = -1;
    };

    struct JOp {
        enum Kind : uint8_t { PEND, GMOVE, PADD, FOLLOWY, ADVP, PFOLLOW, ROT, SCALE, DMOVE } kind;
        int group = 0;
        double dx = 0;
        double dy = 0;
        bool stampOnly = false;
        bool claimed = false;
        int t0 = 0;
        int t1 = 0;
        int center = 0;
        bool finishRelated = false;
        bool cP = false;
        float cx = 0;
        float cy = 0;
        float rot = 0;
        float rotObj = 0;
        float pdx = 0;
        float pdy = 0;
        double tfN[2] = {1, 1};
        double tfO[2] = {1, 1};
        bool onlyMove = false;
        float scr = 0;
        bool scrP = false;
    };

    struct DMove {
        int c;
        int a;
        float f;
        float k;
        bool dir;
    };

    struct AdvKey {
        int key;
        float x;
        float y;
        bool ok;
    };

    struct UndoE {
        int group;
        int pos;
        bool absent;
        GroupOff old;
    };

    struct Journal {
        std::vector<JOp> ops;
        std::vector<JTerm> terms;
        std::vector<FollowYCmd> fy;
        std::vector<AdvKey> advKeys;
        std::vector<UndoE> undo;
        std::vector<DMove> dm;
    };

    inline thread_local std::vector<std::pair<double, double>> tl_dmVal;

    inline thread_local Journal const* tl_undoJr = nullptr;

    inline thread_local int tl_jrPos = 0;

    GroupOff const* undoFind(short gr, bool& hit);

    GroupOff const* undoLookup(short gr, bool& hit);

    GroupOff const* undoFind(short gr, bool& hit);

    inline thread_local std::vector<AdvKey> const* tl_advKeys = nullptr;

    inline thread_local Journal* tl_jr = nullptr;

    inline thread_local bool tl_jrImm = false;

    void jrExec(JOp const& o);

    // adds an operation to the journal, runs it right away in immediate mode
    inline void jrPush(JOp const& o) {
        tl_jr->ops.push_back(o);
        if (tl_jrImm) jrExec(tl_jr->ops.back());
    }

    inline thread_local bool tl_inPend = false;

    // m_lastPosition of an own entry, updated once per tick
    inline void stampLast(Game& g, ObjDyn& d) {
        if (d.stamp != tickStamp(g)) {
            d.lastX = static_cast<float>(d.x);
            d.lastY = static_cast<float>(d.y);
            d.stamp = tickStamp(g);
        }
    }

    void moveFyMembers(Game& g, int group, double dx, double dy, bool sectY);

    void moveGroupP(Game& g, int group, double dx, double dy);

    std::vector<int>& cellMut(Game& g, int cx, int cy);

    void cellMove(Game& g, int idx, int cx0, int cy0, int cx1, int cy1);

    void resection(Game& g, int idx, double oldX, double oldY, double newX, double newY);

    void resectionDyn(Game& g, int idx, ObjDyn& d, double newX, double newY);

    void moveGroup(Game& g, int group, double dx, double dy, bool stampOnly);

    // true if the group is turned off (one bit per group)
    inline bool offBit(TrigHeavy const& heavy, int gr) {
        size_t const word = static_cast<size_t>(gr) >> 6;
        return word < heavy.offBits.size() && ((heavy.offBits[word] >> (gr & 63)) & 1u);
    }

    bool groupEnabled(Game const& g, int group);

    void toggleGroup(Game& g, int id, bool activate);

    void spawnGroupLayer(Game& g, int group, bool ordered, double delay, int uid, int control,
                         RemapKeys const* keys = nullptr);

    void spawnObject(Game& g, int idx, double delay, RemapKeys const* keys = nullptr);

    void toggleGroupTriggered(Game& g, int group, bool activate, int uid, int control, RemapKeys const* keys = nullptr);

    bool hasBeenActivated(Game const& g, int idx);

    void triggerActivated(Game& g, int idx);

    void queueSpawn(Game& g, int group, float delay, bool ordered, int uid, int control,
                    RemapKeys const* keys = nullptr);

    void spawnObjectsInOrder(Game& g, std::vector<int> const& objs, double delay, int uid, int control,
                             RemapKeys const* keys);

    inline thread_local bool tl_spRec = false;

    inline thread_local std::vector<std::pair<uint64_t, bool>> tl_spTests;

    void spawnGroupLayer(Game& g, int group, bool ordered, double delay, int uid, int control, RemapKeys const* keys);

    void spawnObject(Game& g, int idx, double delay, RemapKeys const* keys);

    GroupCmd& newCmd(Game& g);

    void createMoveCommand(Game& g, double dx, double dy, int group, float duration, int easing, float rate,
                           bool lockPX, bool lockPY, bool lockCX, bool lockCY, float modX, float modY, int uid,
                           int control);

    void createRotateCommand(Game& g, float rotation, float duration, int target, int center, int easing, float rate,
                             bool lockRot, int uid, int control);

    void createFollowCommand(Game& g, float xMod, float yMod, float duration, int target, int center, int uid,
                             int control);

    void createPlayerFollowCommand(Game& g, float speed, float delay, int offset, float maxSpeed, float duration,
                                   int target, int uid, int control);

    void createTransformCommand(Game& g, double scaleX, double scaleY, int target, int center, bool onlyMove,
                                bool relRot, float duration, int easing, float rate, int uid, int control);

    void updateAction(GroupCmd& cmd, int type, float value);

    void stepCmd(GroupCmd& cmd, float dt);

    struct KfDyn {
        int anim = -1;
        std::vector<World::KfKeyState> st;
        World::KfAnim A;
        std::vector<World::KfSpline> splines;
    };

    struct KfDynPool {
        std::mutex m;
        std::unordered_map<uint64_t, std::unique_ptr<KfDyn>> map;
        uint64_t sig = 0;
        std::atomic<uint64_t> gen{0};
    };

    KfDynPool& kfDynPool();

    // world signature, the pool is cleared when it changes
    inline uint64_t kfWorldSig(World const& w) {
        return (w.serial.v * 0x9e3779b97f4a7c15ull) ^ (w.objs.size() << 20) ^ w.kfAnims.size();
    }

    KfDyn const& kfDynGet(World const& w, uint64_t key);

    uint64_t kfDynPut(World const& w, int anim, std::vector<World::KfKeyState> const& states);

    struct KfSpawn {
        int uid;
        int group;
        int control;
        float delay;
        RemapKeys keys;
    };

    struct KfOut {
        double dx = 0;
        double dy = 0;
        bool move = false;
        double rot = 0;
        bool rotate = false;
        double sNew = 1;
        double tNew = 1;
        double sOld = 0;
        double tOld = 0;
        double rot0 = 0;
        bool scale = false;
    };

    void kfStep(Game& g, GroupCmd& cmd, float dt, KfOut& out, std::vector<KfSpawn>& spawns);

    constexpr int kRefNone = INT32_MIN;

    constexpr int kRefP1 = -1;

    constexpr int kRefP2 = -2;

    int tryGetObjectRef(Game& g, int gr);

    int playerRef(Game const& g, Obj const& o);

    Point refPos(Game& g, int ref);

    void dynMoveDelta(Game& g, int from, int to, float part, bool dir, float dist, double& moveX, double& moveY);

    float refRot(Game& g, int ref);

    float wrapAngle(float angle, float range);

    float rotateTargetAngle(Game& g, Obj const& trig, int center, int target, int aim, int mnx, int mny, int mxx,
                            int mxy);

    void triggerDynamicRotateCommand(Game& g, int idx, Obj const& o);

    void triggerMoveCommand(Game& g, int idx);

    int countForItem(Game const& g, int item);

    void updateCountForItem(Game& g, int item, int value);

    RemapKeys const* activationKeys(int idx);

    int remapForActivation(Game const& g, int idx, int value);

    int clampItem(int id);

    TimerItem const* findTimer(Game const& g, int id);

    int timerIndex(Game const& g, int id);

    double timerValue(Game const& g, int id);

    // std::hash<int> of MSVC (FNV-1a on the 4 bytes)
    inline size_t msvcHashInt(int key) {
        uint64_t hash = 0xcbf29ce484222325ull;
        uint32_t const bits = static_cast<uint32_t>(key);
        for (int i = 0; i < 4; ++i) {
            hash ^= (bits >> (8 * i)) & 0xffu;
            hash *= 0x100000001b3ull;
        }
        return static_cast<size_t>(hash);
    }

    // inserts e keeping the iteration order of a MSVC unordered_map (the game goes through its timers that way)
    template <class T, class KeyOf>
    void msvcHashInsert(std::vector<T>& list, uint32_t& buckets, T const& e, KeyOf key) {
        size_t const newSize = list.size() + 1;
        if (1.f < static_cast<float>(newSize) / static_cast<float>(buckets)) {
            size_t const needed =
                    std::max<size_t>(8, static_cast<size_t>(std::ceil(static_cast<float>(newSize) / 1.f)));
            size_t bucketCount = (buckets < 512 && static_cast<size_t>(buckets) * 8 >= needed)
                                         ? static_cast<size_t>(buckets) * 8
                                         : needed;
            size_t pow2 = 1;
            while (pow2 < bucketCount) pow2 <<= 1;
            bucketCount = pow2;
            size_t const mask = bucketCount - 1;
            std::list<T> order(list.begin(), list.end());
            std::vector<typename std::list<T>::iterator> bucketHead(bucketCount, order.end());
            for (auto it = order.begin(); it != order.end();) {
                auto const next = std::next(it);
                size_t const bucket = msvcHashInt(key(*it)) & mask;
                if (bucketHead[bucket] == order.end())
                    bucketHead[bucket] = it;
                else {
                    order.splice(bucketHead[bucket], order, it);
                    bucketHead[bucket] = it;
                }
                it = next;
            }
            list.assign(order.begin(), order.end());
            buckets = static_cast<uint32_t>(bucketCount);
        }
        size_t const mask = static_cast<size_t>(buckets) - 1;
        size_t const bucket = msvcHashInt(key(e)) & mask;
        for (size_t i = 0; i < list.size(); ++i)
            if ((msvcHashInt(key(list[i])) & mask) == bucket) {
                list.insert(list.begin() + static_cast<long>(i), e);
                return;
            }
        list.push_back(e);
    }

    void createTimer(Game& g, int id, double start, double target, bool stopEnabled, bool running, bool dontOverride,
                     float mod, bool ignoreTW, int group, RemapKeys const* keys, int uid, int control);

    void updateTimer(Game& g, int id, double value);

    double getItemValue(Game const& g, int type, int id);

    double itemOp(double a, double b, int op);

    double itemRound(double value, int rounding);

    double itemSign(double value, int sign);

    void activateItemEdit(Game& g, Obj const& o);

    void activateItemCompare(Game& g, Obj const& o, RemapKeys const* keys);

    void activatePersistentItem(Game& g, Obj const& o);

    void activateTimerTrigger(Game& g, Obj const& o, RemapKeys const* keys);

    void activateSequence(Game& g, int idx, Obj const& o, RemapKeys const* keys);

    void activateInstantCollision(Game& g, int idx, Obj const& o);

    void areaLoadValues(World const& w, AreaInst& a);

    void areaAdd(Game& g, int defIdx);

    void areaLoadTransitions(World const& w, AreaInst& a, World::AreaDef const& editDef);

    void areaEdit(Game& g, int defIdx);

    void controlCommands(Game& g, int uid, int control, int cmd);

    void controlArea(Game& g, int defIdx, int cmd);

    void controlAreaWithControl(Game& g, int control, int cmd);

    bool canBeControlled(int id);

    void controlTriggersInGroup(Game& g, int group, int cmd);

    void tweenValue(Game& g, int key, float from, float to, float duration, int easing, float rate, int uid,
                    int control);

    void updateZoom(Game& g, float zoom, float duration, int easing, float rate, int uid, int control);

    void updateCameraOffsetX(Game& g, float x, float duration, int easing, float rate, int uid, int control);

    void updateCameraOffsetY(Game& g, float y, float duration, int easing, float rate, int uid, int control);

    void handleObjectCollision(Game& g, bool entered, int a, int b);

    // true if blocks a and b are already marked touching this step
    inline bool alreadyCollided(Game const& g, int a, int b) {
        int const high = a < b ? b : a;
        int const low = a < b ? a : b;
        auto const* state = g.trig.collided.find((high + 1000) * 10000 + low);
        return state && *state == 1;
    }

    void objectsCollided(Game& g, int a, int b);

    void checkBlockAgainst(Game& g, int blockIdx, Rect const& blockRect, bool blockOriented, int otherIdx,
                           bool otherIsPlayer, Rect const& otherRect, OBB const* otherBox, OBB const* blockBox);

    // own entry of an object (position, rotation, scale), nullptr if it has none
    inline ObjDyn const* dynOf(Game const& g, int idx) {
        if (!g.world->mayHaveDyn[static_cast<size_t>(idx)]) return nullptr;
        if (isPO(g, idx)) {
            pRead();
            return g.trig.pv ? g.trig.pv->dyn.find(idx) : nullptr;
        }
        dynDep(2);
        fyCheck(idx, 6);
        return g.trig.rd().objDyn.find(idx);
    }

    struct AreaWork {
        std::vector<std::pair<int, AreaOff>> v;
        size_t nSorted = 0;
        std::vector<int> slot;
        std::vector<uint32_t> gen;
        uint32_t cur = 0;
        TrigState const* owner = nullptr;

        // starts a new pass, generations make clearing free
        void begin(size_t nObjs) {
            if (slot.size() != nObjs) {
                slot.assign(nObjs, 0);
                gen.assign(nObjs, 0u);
                cur = 0;
            }
            if (++cur == 0) {
                std::fill(gen.begin(), gen.end(), 0u);
                cur = 1;
            }
            v.clear();
            nSorted = 0;
        }

        // area offset of an object this tick, nullptr if none
        AreaOff* find(int idx) {
            size_t const i = static_cast<size_t>(idx);
            return gen[i] == cur ? &v[static_cast<size_t>(slot[i])].second : nullptr;
        }

        // offset of idx, created if missing
        AreaOff& get(int idx) {
            size_t const i = static_cast<size_t>(idx);
            if (gen[i] == cur) return v[static_cast<size_t>(slot[i])].second;
            gen[i] = cur;
            slot[i] = static_cast<int>(v.size());
            v.emplace_back(idx, AreaOff{});
            return v.back().second;
        }
    };

    inline thread_local AreaWork tl_areaWork;

    // area offset of an object, wherever it's stored
    inline AreaOff const* areaOffFind(Game const& g, int idx) {
        if (tl_areaWork.owner == &g.trig) return tl_areaWork.find(idx);
        if (g.trig.areaOffEmpty()) return nullptr;
        return g.trig.areaOff().find(idx);
    }

}

namespace ap::sim {
    extern thread_local bool tl_posCacheOn;

}

namespace ap::sim::trigger_impl {
    void groupPosCachedImpl(Game const& g, Obj const& o, double& x, double& y, bool canon);

    // position of a group member with the cached group offsets
    inline void groupPosCached(Game const& g, Obj const& o, double& x, double& y) {
        groupPosCachedImpl(g, o, x, y, false);
    }

    // same, in the canonical order of the offsets
    inline void groupPosCanon(Game const& g, Obj const& o, double& x, double& y) {
        groupPosCachedImpl(g, o, x, y, true);
    }

    void kfOptPos(Game const& g, Obj const& o, double& x, double& y);

    void realPosBase(Game const& g, int idx, double& x, double& y);

}

namespace ap::sim {
    extern thread_local bool tl_rectCacheOn;

}

namespace ap::sim::trigger_impl {
    // mixes v into the hash h
    inline uint64_t mix64(uint64_t h, uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h *= 0xbf58476d1ce4e5b9ull;
        return h ^ (h >> 29);
    }

    // raw bytes of a value (8 bytes max) as an integer
    template <class T>
    inline uint64_t bits64(T v) {
        static_assert(sizeof(T) <= 8);
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof v);
        return bits;
    }

    // one round of xxHash64
    inline uint64_t xxRound(uint64_t acc, uint64_t input) {
        acc += input * 0xC2B2AE3D27D4EB4Full;
        acc = (acc << 31) | (acc >> 33);
        return acc * 0x9E3779B185EBCA87ull;
    }

    // hashes every byte of a flat struct, 4 xxHash lanes for big ones
    template <class T>
    inline uint64_t hashPod(uint64_t h, T const& v) {
        unsigned char const* bytes = reinterpret_cast<unsigned char const*>(&v);
        size_t size = sizeof(T);
        size_t i = 0;
        if constexpr (sizeof(T) >= 32) {
            uint64_t lane1 = h + 0x60EA27EEADC0B5D6ull;
            uint64_t lane2 = h ^ 0xC2B2AE3D27D4EB4Full;
            uint64_t lane3 = h;
            uint64_t lane4 = h - 0x9E3779B185EBCA87ull;
            for (; i + 32 <= size; i += 32) {
                uint64_t words[4];
                std::memcpy(words, bytes + i, 32);
                lane1 = xxRound(lane1, words[0]);
                lane2 = xxRound(lane2, words[1]);
                lane3 = xxRound(lane3, words[2]);
                lane4 = xxRound(lane4, words[3]);
            }
            h = mix64(mix64(mix64(mix64(h, lane1), lane2), lane3), lane4);
        }
        for (; i + 8 <= size; i += 8) {
            uint64_t word;
            std::memcpy(&word, bytes + i, 8);
            h = mix64(h, word);
        }
        if (i < size) {
            uint64_t word = 0;
            std::memcpy(&word, bytes + i, size - i);
            h = mix64(h, word);
        }
        return h;
    }

    // hash of a vector of flat structs
    template <class T>
    inline uint64_t hashVec(uint64_t h, std::vector<T> const& v) {
        h = mix64(h, v.size());
        for (auto const& e : v) h = hashPod(h, e);
        return h;
    }

    struct MemoEntry {
        uint64_t key;
        std::shared_ptr<TrigHeavy> h;
        std::shared_ptr<TrigDyn> d;
        std::vector<FollowYCmd> fy;
        unsigned itemDelta = 0;
        std::vector<uint64_t> spawned;
        std::shared_ptr<Journal const> jr;
        std::vector<std::pair<uint64_t, bool>> spTests;
        bool fyMode = false;
        std::vector<std::pair<int, ObjDyn>> dynW;
        uint64_t baseKey = 0;
        std::shared_ptr<TrigDyn> dynB;
        void const* excl = nullptr;
    };

    inline thread_local std::vector<uint64_t> tl_spBefore;

    inline thread_local std::vector<uint64_t> tl_spAfter;

    // before a memoized computation: saves the spawned groups and starts recording the spawn tests
    inline void spawnMark(TrigState const& trig) {
        tl_spBefore = trig.spawnedThisTick.items;
        tl_spTests.clear();
        tl_spRec = true;
    }

    inline thread_local std::vector<MemoEntry> tl_memo;

    inline thread_local std::vector<uint64_t> tl_memoKeys;

    inline thread_local std::shared_ptr<Journal const> tl_commitJr;

    inline thread_local std::vector<MemoEntry> tl_memo2;

    inline thread_local std::vector<uint64_t> tl_memo2Keys;

    inline thread_local int tl_memoLevels = 0;

    inline thread_local int tl_memoTick = -1;

    inline thread_local WKey tl_memoWorld;

    inline thread_local long tl_memoHits = 0;

    inline thread_local long tl_memoMiss = 0;

    inline thread_local long tl_hashes = 0;

    inline thread_local long tl_dbg[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    struct MemoIndex {
        static constexpr int kBits = 10;
        int head[1 << kBits];
        int tail[1 << kBits];
        std::vector<int> next;

        MemoIndex() { clear(); }

        // bucket of a memo key
        static size_t bucket(uint64_t key) {
            return static_cast<size_t>((key * 0x9e3779b97f4a7c15ull) >> (64 - kBits));
        }

        // empties the index
        void clear() {
            std::fill(std::begin(head), std::end(head), -1);
            next.clear();
        }

        // adds entry i under key
        void add(uint64_t key, int i) {
            size_t const bucketIdx = bucket(key);
            if (static_cast<size_t>(i) >= next.size()) next.resize(static_cast<size_t>(i) + 1, -1);
            next[static_cast<size_t>(i)] = -1;
            if (head[bucketIdx] < 0)
                head[bucketIdx] = i;
            else
                next[static_cast<size_t>(tail[bucketIdx])] = i;
            tail[bucketIdx] = i;
        }
    };

    inline thread_local MemoIndex tl_memoIdx;

    inline thread_local MemoIndex tl_memo2Idx;

    // clears the memo of this thread on a new tick or world
    inline void memoTick(Game const& g) {
        if (tl_memoTick != g.tick || tl_memoWorld != g.world) {
            tl_memoTick = g.tick;
            tl_memoWorld = g.world;
            tl_memo.clear();
            tl_memoKeys.clear();
            tl_memo2.clear();
            tl_memo2Keys.clear();
            tl_memoIdx.clear();
            tl_memo2Idx.clear();
            tl_memoLevels = 0;
        }
    }

    inline thread_local TrigState const* tl_spCheck = nullptr;

    // true if the entry's spawn tests give the same answers for this state
    inline bool spOk(MemoEntry const& e) {
        if (e.spTests.empty() || !tl_spCheck) return true;
        for (auto const& test : e.spTests) {
            if ((tl_spCheck->spawnedThisTick.count(test.first) != 0) != test.second) return false;
        }
        return true;
    }

    // memo entry (levels 0-1) with this key that fits the checked state
    inline MemoEntry const* memoFind(uint64_t key) {
        uint64_t const* keys = tl_memoKeys.data();
        for (int i = tl_memoIdx.head[MemoIndex::bucket(key)]; i >= 0; i = tl_memoIdx.next[static_cast<size_t>(i)])
            if (keys[i] == key && spOk(tl_memo[static_cast<size_t>(i)])) return &tl_memo[static_cast<size_t>(i)];
        return nullptr;
    }

    uint64_t dynKeysHash(TrigDyn const& dyn);

    // true if both entries have the same bytes
    inline bool podEq(ObjDyn const& a, ObjDyn const& b) {
        static_assert(sizeof(ObjDyn) % 8 == 0, "ObjDyn : multiple of 8 bytes");
        uint64_t wordsA[sizeof(ObjDyn) / 8];
        uint64_t wordsB[sizeof(ObjDyn) / 8];
        std::memcpy(wordsA, &a, sizeof wordsA);
        std::memcpy(wordsB, &b, sizeof wordsB);
        uint64_t diff = 0;
        for (size_t k = 0; k < sizeof(ObjDyn) / 8; ++k) diff |= wordsA[k] ^ wordsB[k];
        return diff == 0;
    }

    // excluded objects (player y follow) as bits, rebuilt only when the list changes
    inline std::vector<uint64_t> const& exclBits(std::vector<int> const& excluded) {
        static thread_local std::vector<int> const* last = nullptr;
        static thread_local unsigned lastGen = ~0u;
        static thread_local size_t lastN = 0;
        static thread_local int lastF = 0;
        static thread_local int lastM = 0;
        static thread_local int lastB = 0;
        static thread_local std::vector<uint64_t> bits;
        int const front = excluded.empty() ? 0 : excluded.front();
        int const mid = excluded.empty() ? 0 : excluded[excluded.size() / 2];
        int const back = excluded.empty() ? 0 : excluded.back();
        if (last != &excluded || lastGen != tl_fyGen || lastN != excluded.size() || front != lastF || mid != lastM ||
            back != lastB) {
            lastF = front;
            lastM = mid;
            lastB = back;
            lastGen = tl_fyGen;
            bits.assign(excluded.empty() ? 1 : (static_cast<size_t>(excluded.back()) >> 6) + 1, 0);
            for (int i : excluded)
                if (i >= 0) bits[static_cast<size_t>(i) >> 6] |= 1ull << (i & 63);
            last = &excluded;
            lastN = excluded.size();
        }
        return bits;
    }

    bool dynSame(TrigDyn const& a, TrigDyn const& b, void const* excl);

    // level 2 entry with this key whose dyn state before the computation matches dyn
    inline MemoEntry const* memoFind2(uint64_t key, TrigDyn const& dyn) {
        void const* const excl = tl_fyExcl;
        for (int slot = tl_memo2Idx.head[MemoIndex::bucket(key)]; slot >= 0;
             slot = tl_memo2Idx.next[static_cast<size_t>(slot)]) {
            size_t const i = static_cast<size_t>(slot);
            if (tl_memo2Keys[i] != key) continue;
            MemoEntry const& entry = tl_memo2[i];
            if (entry.excl == excl && entry.dynB && spOk(entry) && dynSame(*entry.dynB, dyn, excl)) return &entry;
        }
        return nullptr;
    }

    inline thread_local Game* tl_applyGame = nullptr;

    void applyDynW(TrigState& trig, std::vector<std::pair<int, ObjDyn>> const& writes);

    bool dynDiff(TrigDyn const& a, TrigDyn const& b, std::vector<std::pair<int, ObjDyn>>& out);

    inline thread_local int tl_mwLook = 0;

    inline thread_local int tl_mwHits = 0;

    inline thread_local int tl_mBypass = 0;

    inline thread_local unsigned tl_memoEpoch = 0;

    inline std::atomic<unsigned> g_memoEpoch{1};

    // true if the memo should be used for this computation
    inline bool memoOn() {
        if (g_noShare & 1) return false;
        unsigned const epoch = g_memoEpoch.load(std::memory_order_relaxed);
        if (tl_memoEpoch != epoch) {
            tl_memoEpoch = epoch;
            tl_mBypass = 0;
            tl_mwLook = 0;
            tl_mwHits = 0;
        }
        if (tl_mBypass > 0) {
            --tl_mBypass;
            return false;
        }
        return true;
    }

    // counts a memo lookup, skips the next 1024 if under 5% hit in 256
    inline void memoStat(bool hit) {
        ++tl_mwLook;
        if (hit) ++tl_mwHits;
        if (tl_mwLook >= 256) {
            if (tl_mwHits * 20 < tl_mwLook) tl_mBypass = 1024;
            tl_mwLook = 0;
            tl_mwHits = 0;
        }
    }

    // looks up a result on the three levels, applies it to the state if found
    inline MemoEntry const* memoLookup(TrigState& trig, uint64_t key) {
        tl_spCheck = &trig;
        MemoEntry const* entry = memoFind(key);
        if (!entry && (tl_memoLevels & 1)) entry = memoFind(mix64(key, dynKeysHash(trig.rd())));
        if (!entry && (tl_memoLevels & 2)) entry = memoFind2(key, trig.rd());
        if (!entry) {
            ++tl_memoMiss;
            memoStat(false);
            return nullptr;
        }
        ++tl_memoHits;
        memoStat(true);
        trig.h = entry->h;
        if (entry->fyMode)
            applyDynW(trig, entry->dynW);
        else if (entry->d)
            trig.d = entry->d;
        for (uint64_t gr : entry->spawned) trig.spawnedThisTick.insert(gr);
        return entry;
    }

    inline thread_local std::shared_ptr<TrigDyn> tl_dBefore;

    inline thread_local long tl_commitFail[4] = {0, 0, 0, 0};

    // stores the result of the computation just done, keyed by its observed dyn dependency
    inline void memoCommit(uint64_t key, TrigState const& trig, uint64_t keysBefore,
                           std::shared_ptr<TrigDyn> const& fullBefore, std::vector<FollowYCmd> const* fy = nullptr) {
        tl_spRec = false;
        size_t const cap = trig.h && trig.r().M().pset ? 512u : 64u;
        if (tl_memo.size() + tl_memo2.size() >= cap) {
            ++tl_commitFail[0];
            return;
        }
        MemoEntry entry;
        if (tl_fyExcl) {
            if (tl_fyHit) {
                ++tl_commitFail[1];
                return;
            }
            entry.fyMode = true;
            if (!tl_dBefore) {
                ++tl_commitFail[3];
                return;
            }
            if (!dynDiff(*tl_dBefore, *trig.d, entry.dynW)) {
                ++tl_commitFail[2];
                return;
            }
        }
        entry.h = trig.h;
        entry.jr = tl_commitJr;
        entry.spTests = tl_spTests;
        entry.itemDelta = tl_itemDelta;
        ++tl_dbg[2 + std::min(tl_dynDep, 2)];
        if (tl_dynDep == 0)
            entry.key = key;
        else if (tl_dynDep == 1)
            entry.key = mix64(key, keysBefore);
        else {
            entry.key = key;
            entry.baseKey = key;
            entry.dynB = fullBefore;
            entry.excl = tl_fyExcl;
            if (!entry.fyMode) entry.d = trig.d;
        }
        if (fy) entry.fy = *fy;
        if (trig.spawnedThisTick.size() != tl_spBefore.size()) {
            for (uint64_t gr : trig.spawnedThisTick.items) {
                if (!std::binary_search(tl_spBefore.begin(), tl_spBefore.end(), gr)) entry.spawned.push_back(gr);
            }
        }
        if (tl_dynDep == 1)
            tl_memoLevels |= 1;
        else if (tl_dynDep >= 2)
            tl_memoLevels |= 2;
        if (tl_dynDep >= 2) {
            if (!entry.dynB) return;
            tl_memo2Idx.add(entry.key, static_cast<int>(tl_memo2Keys.size()));
            tl_memo2Keys.push_back(entry.key);
            tl_memo2.push_back(std::move(entry));
            return;
        }
        tl_memoIdx.add(entry.key, static_cast<int>(tl_memoKeys.size()));
        tl_memoKeys.push_back(entry.key);
        tl_memo.push_back(std::move(entry));
    }

    uint64_t motionHashOf(TrigMotion const& motion);

}

namespace ap::sim {
    uint64_t remapEntryHash(int idx, RemapKeys const& keys);

}

namespace ap::sim::trigger_impl {
    uint64_t motionHash(TrigHeavy const& heavy, bool withOff);

    struct MoSwap {
        std::shared_ptr<TrigHeavy> from;
        TrigMotion const* mo;
        bool any;
        std::shared_ptr<TrigHeavy> to;
    };

    inline thread_local std::vector<MoSwap> tl_moSwap;

    inline thread_local size_t tl_moSwapNext = 0;

    // memo lookup for the motion step: only takes the motion part of the result
    inline MemoEntry const* motionLookup(TrigState& trig, uint64_t key) {
        MemoEntry const* entry = memoFind(key);
        if (!entry && (tl_memoLevels & 1)) entry = memoFind(mix64(key, dynKeysHash(trig.rd())));
        if (!entry && (tl_memoLevels & 2)) entry = memoFind2(key, trig.rd());
        if (!entry) return nullptr;
        memoStat(true);
        if (trig.r().mo != entry->h->mo || trig.r().anyMotion != entry->h->anyMotion) {
            TrigMotion const* const mo = entry->h->mo.get();
            bool const any = entry->h->anyMotion;
            bool done = false;
            if (trig.h.use_count() > 1) {
                for (auto const& cached : tl_moSwap) {
                    if (cached.from == trig.h && cached.mo == mo && cached.any == any &&
                        cached.to->owner == trigThreadId()) {
                        trig.h = cached.to;
                        done = true;
                        break;
                    }
                }
            }
            if (!done) {
                std::shared_ptr<TrigHeavy> from = trig.h.use_count() > 1 ? trig.h : nullptr;
                TrigHeavy& heavy = trig.w();
                heavy.mo = entry->h->mo;
                heavy.anyMotion = any;
                if (from) {
                    constexpr size_t kSlots = 8;
                    if (tl_moSwap.size() < kSlots)
                        tl_moSwap.push_back(MoSwap{std::move(from), mo, any, trig.h});
                    else {
                        tl_moSwap[tl_moSwapNext] = MoSwap{std::move(from), mo, any, trig.h};
                        tl_moSwapNext = (tl_moSwapNext + 1) % kSlots;
                    }
                }
            }
        }
        if (entry->fyMode)
            applyDynW(trig, entry->dynW);
        else if (entry->d)
            trig.d = entry->d;
        return entry;
    }

}

namespace ap::sim {
    extern thread_local long g_trigCounters[8];

    extern thread_local int g_noShare;

}

namespace ap::sim::trigger_impl {
    uint64_t dynKeysHash(TrigDyn const& dyn);

    void playerFollowStep(Game& g, FollowYCmd const& c, float dt);

    void pruneDyn(Game& g);

    inline thread_local int tl_advSel = 0;

    // true if the instance belongs to the player part (depends on each simulated player)
    inline bool advIsP(Game const& g, AdvInst const& I) {
        PSet const* pset = psetOf(g);
        if (!pset) return false;
        World const& w = *g.world;
        if (I.key < 0) return true;
        int const mainObj = w.mainObjectOf(I.key);
        if (mainObj >= 0 && (w.areaTarget[static_cast<size_t>(mainObj)] || bitOf(pset->objBits, mainObj))) return true;
        auto const& movers = w.moversOfGroup(I.target);
        return !movers.empty() && bitOf(pset->objBits, movers.front());
    }

    // wraps the angle into [-limit, limit], same formula as the game (0x22e8b0)
    inline float afWrap(float angle, float limit) {
        if (angle < -limit) {
            float const turns = std::ceil(std::floor(std::fabs(angle) / limit) * 0.5f);
            return turns * (limit + limit) + angle;
        }
        if (limit < angle) {
            float const turns = std::ceil(std::floor(std::fabs(angle) / limit) * 0.5f);
            return angle - turns * (limit + limit);
        }
        return angle;
    }

    // random part of a setting: variance times the object's random value number slot
    inline float afRand(World const& w, Obj const& o, float variance, int slot) {
        if (variance == 0.f) return 0.f;
        if (w.areaRand.empty() || o.areaRi < 0) return 0.f;
        size_t const i = static_cast<size_t>(o.areaRi) + static_cast<size_t>(slot);
        return i < w.areaRand.size() ? variance * w.areaRand[i] : 0.f;
    }

    bool afTargetPos(Game const& g, int key, float& x, float& y);

    bool afSavedPos(Game const& g, int key, float delay, float& x, float& y);

    void afMoveObj(Game& g, int idx, double dx, double dy);

    void afStartVel(Game& g, int idx, int ref1, int ref2, float dir, float speed, float& vx, float& vy);

    void afProcessInstance(Game& g, size_t instIdx, bool started, float dt);

    bool afApproxEntry(Game& g, int obj);

    void advancedFollowStep(Game& g, float dt);

    // true if the advanced follow depends on the player: follows a player or the camera, or an area moved object
    inline bool advPlayerDep(World const& w, AdvInst const& I) {
        if (I.key < 0) return true;
        int const mainObj = w.mainObjectOf(I.key);
        return mainObj >= 0 && static_cast<size_t>(mainObj) < w.areaTarget.size() &&
               w.areaTarget[static_cast<size_t>(mainObj)];
    }

    std::vector<int> const& fyObjectsOf(World const& w, std::vector<GroupCmd> const& cmds,
                                        std::vector<AdvInst> const& advs);

    bool followIndependent(World const& w, GroupCmd const& c, std::vector<int> const& fy);

    int closureCenterOf(World const& w, GroupCmd const& c);

    std::shared_ptr<PSet const> computePSet(Game const& g);

    void updatePSet(Game& g);

    // player 1 movement during the last tick, read by moves locked on the player
    inline void setPlayerDelta(Game& g, float dtPhys) {
        Player const& p = g.p1;
        float dy = p.y - p.lastY;
        if (!g.playerDied &&
            static_cast<double>(dtPhys) * 16.0 < static_cast<double>(std::fabs(dy - p.yVelocityRelated3)))
            dy = 0.f;
        float dx = p.x - p.lastX;
        if (g.playerDied) {
            dx = 0.f;
            dy = 0.f;
        }
        g.trig.playerDx = dx;
        g.trig.playerDy = dy;
    }

    inline thread_local bool tl_newPath = false;

    struct Aff {
        float a, b, c, d, tx, ty;
    };

    // nodeToParentTransform of a node at (x, y), anchor (0, 0), no skew
    inline Aff nodeAff(float x, float y, float rotX, float rotY, float sx, float sy) {
        float cosX = 1.f;
        float sinX = 0.f;
        float cosY = 1.f;
        float sinY = 0.f;
        if (rotX != 0.f || rotY != 0.f) {
            float const radX = -(rotX * 0.01745329252f);
            float const radY = -(rotY * 0.01745329252f);
            cosX = gcos(radX);
            sinX = gsin(radX);
            if (radY == radX) {
                cosY = cosX;
                sinY = sinX;
            } else {
                cosY = gcos(radY);
                sinY = gsin(radY);
            }
        }
        return Aff{cosY * sx, sinY * sx, -sinX * sy, cosX * sy, x, y};
    }

    // t1 then t2, like CCAffineTransformConcat
    inline Aff affConcat(Aff const& t1, Aff const& t2) {
        float const a = t1.a * t2.a + t1.b * t2.c;
        float const b = t1.a * t2.b + t1.b * t2.d;
        float const c = t1.c * t2.a + t1.d * t2.c;
        float const d = t1.c * t2.b + t1.d * t2.d;
        float const tx = t1.tx * t2.a + t1.ty * t2.c + t2.tx;
        float const ty = t1.tx * t2.b + t1.ty * t2.d + t2.ty;
        return Aff{a, b, c, d, tx, ty};
    }

    Aff tfWorld(float cx, float cy, double fxN, double fxO, double fyN, double fyO, float rotT, float centerRot);

    void tfObj(Obj const& o, ObjDyn& d, Aff const& P, float cx, float cy, float pdx, float pdy, bool onlyMove,
               float rotOff);

    // scale without a center: scale += scale * ratio - scale
    inline void tfScaleNoCenter(ObjDyn& d, double rx, double ry) {
        double const sx = static_cast<double>(d.sclX);
        double const sy = static_cast<double>(d.sclY);
        float const dx = static_cast<float>(sx * rx - sx);
        float const dy = static_cast<float>(sy * ry - sy);
        d.sclOffX = dx + d.sclOffX;
        d.sclOffY = dy + d.sclOffY;
        d.sclX = d.sclX + dx;
        d.sclY = d.sclY + dy;
        d.scaled = true;
    }

    void trigMoveActionsImpl(Game& g, double dtSec, float dtPhys);

    void rotateP(Game& g, JOp const& op, Journal const& jr);

    void scaleP(Game& g, JOp const& op, Journal const& jr);

    void execOp(Game& g, JOp const& o, Journal const& jr, double dtSec);

    void phaseB(Game& g, Journal const& jr, double dtSec);

    inline thread_local Game* tl_jrGame = nullptr;

    inline thread_local double tl_jrDt = 0;

    void jrExec(JOp const& o);

    struct PHit {
        int at = 0, until = 0, wait = 0;
    };

    inline thread_local std::unordered_map<uint64_t, PHit> tl_pHitUntil;

    inline thread_local long tl_pStat[6] = {0, 0, 0, 0, 0, 0};

    inline std::atomic<long> g_pStatAll[2] = {0, 0};

    struct EaseBuf {
        int type;
        float rate;
        float t[102];
    };

    inline thread_local std::vector<EaseBuf> tl_easeBufs;

    float areaEase(float v, int type, float rate);

    float areaValueFromDist(float dist, int len, float deadZone);

    // v[0, n0) is sorted, sorts what was appended and merges it, then removes duplicates
    inline void sortAppended(std::vector<int>& v, size_t n0) {
        if (v.size() == n0) return;
        auto const mid = v.begin() + static_cast<long>(n0);
        if (!std::is_sorted(v.begin(), mid))
            std::sort(v.begin(), v.end());
        else {
            std::sort(mid, v.end());
            std::inplace_merge(v.begin(), mid, v.end());
        }
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }

    void addAfMoved(Game& g);

    struct PosCacheScope {
        bool prev;

        // turns on the position cache for this scope
        PosCacheScope() : prev(tl_posCacheOn) { tl_posCacheOn = !(g_noShare & 2); }

        // puts the position cache back as it was
        ~PosCacheScope() { tl_posCacheOn = prev; }
    };

}

namespace ap::sim {
    extern std::atomic<long> g_areaMemoStat[5];

    extern std::atomic<long> g_areaSplitStat[4];

}

namespace ap::sim::trigger_impl {
    uint64_t pvHash(TrigPv const& P);

    void trigAreaActionsImpl(Game& g, double dtSec);

    void trigAreaActionsImpl(Game& g, double dtSec);

    struct BlkPass {
        uint64_t key = 0;
        WKey w;
        bool valid = false;
        std::vector<Rect> dRect;
        std::vector<uint8_t> dOff;
        bool dSweep = false;
        float dMaxW = 0.f;
        std::vector<uint32_t> dSort;
        std::vector<float> dSortX;
        struct ItemRange {
            int item;
            uint32_t b, e;
        };
        std::vector<ItemRange> dRanges;
        std::vector<OBB> obb;
        struct EA {
            int other;
            int ord;
            int item;
            uint8_t pass, uidEq;
        };
        struct EB {
            uint32_t j;
            int item;
            uint8_t pass, uidEq, ok;
        };
        struct Blk {
            uint32_t k, aB, aE, bB, bE;
        };
        std::vector<EA> a;
        std::vector<EB> b;
        std::vector<Blk> blocks;
    };

    inline thread_local BlkPass tl_blkPass;

    inline thread_local BlkPass tl_blkScratch;

    inline thread_local uint64_t tl_blkPassLast = 0;

    inline thread_local WKey tl_blkPassLastW;

    uint64_t blkPassKey(Game const& g);

    void activateEventTrigger(Game& g, int idx, Obj const& o, RemapKeys const* keys);

}

namespace ap::sim {
    extern thread_local Game* g_evGame;

}

namespace ap::sim::trigger_impl {
    uint64_t pvHash(TrigPv const& P);

    // true if there is no player part or it holds nothing
    inline bool pvEmpty(TrigPv const* P) {
        return !P || (P->off.items.empty() && P->dyn.items.empty() && P->af.items.empty() && P->afObjs.empty() &&
                      P->kfSnap.empty());
    }

}

namespace ap::sim {
    using namespace trigger_impl;
}
