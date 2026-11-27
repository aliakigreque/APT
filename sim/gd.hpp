// Core of the simulator: the Game state, object handles (GObj) and the declarations of the physics, important!
// collision, trigger and step functions.

#pragma once

#include "containers.hpp"
#include "debug.hpp"
#include "player.hpp"
#include "world.hpp"
#include "timeline.hpp"
#include "triggers.hpp"
#include "camera.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace ap::sim {
    extern bool g_trace;
#if defined(__GNUC__) || defined(__clang__)
#define AP_RET_ADDR() __builtin_return_address(0)
#else
#define AP_RET_ADDR() nullptr
#endif
    extern int g_rngLogFrom;
    extern int g_rngLogTo;
    void rngLogDraw(int tick, void const* site);

    struct Point {
        float x = 0;
        float y = 0;
    };

    // left edge of a rect (x and y are the bottom left corner)
    inline float getMinX(Rect const& r) { return r.x; }
    // right edge of a rect
    inline float getMaxX(Rect const& r) { return r.x + r.w; }
    // bottom edge of a rect
    inline float getMinY(Rect const& r) { return r.y; }
    // top edge of a rect
    inline float getMaxY(Rect const& r) { return r.y + r.h; }
    // horizontal center of a rect
    // could be used by ported game code that reads the center of a rect (CCRect::getMidX), keep
    inline float getMidX(Rect const& r) { return r.x + r.w * 0.5f; }
    // vertical center of a rect
    // could be used by ported game code that reads the center of a rect (CCRect::getMidY), keep
    inline float getMidY(Rect const& r) { return r.y + r.h * 0.5f; }

    // same as CCRect::intersectsRect, touching edges count
    inline bool intersectsRect(Rect const& a, Rect const& b) {
        return !(getMaxX(a) < getMinX(b) || getMaxX(b) < getMinX(a) || getMaxY(a) < getMinY(b) || getMaxY(b) < getMinY(a));
    }

    // true if both rects are exactly the same
    inline bool rectEquals(Rect const& a, Rect const& b) {
        return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    }

    // CCRectZero
    inline Rect zeroRect() { return Rect{0, 0, 0, 0}; }

    //rounds to 3 decimals like PlayerObject::setYVelocity
    inline double gdRound3(double v) {
        double t = static_cast<double>(static_cast<int>(v));
        if (v != t) return std::round((v - t) * 1000.0) / 1000.0 + t;
        return v;
    }

    struct OBB {
        Point corner[4];
        Point axis[2];
        double origin[2] = {0, 0};
        void calculateWithCenter(Point center, float width, float height, float rotationAngle);

        // same with the half axes already computed
        void fromHalfAxes(Point center, float Xx, float Xy, float Yx, float Yy) {
            cornersFromHalfAxes(corner, center, Xx, Xy, Yx, Yy);
            finish();
        }

        // 4 corners from the center and the half axes X (Xx, Xy) and Y (Yx, Yy)
        static void cornersFromHalfAxes(Point* corner, Point center, float Xx, float Xy, float Yx, float Yy) {
            corner[0] = Point{center.x - Xx - Yx, center.y - Xy - Yy};
            corner[1] = Point{center.x + Xx - Yx, center.y + Xy - Yy};
            corner[2] = Point{center.x + Xx + Yx, center.y + Xy + Yy};
            corner[3] = Point{center.x - Xx + Yx, center.y - Xy + Yy};
        }

        void finish();
        bool overlaps1Way(OBB const& other) const;
        float separation(OBB const& other) const;
        // OBB2D::getBoundingRect
        Rect boundingRect() const { return boundingRectOf(corner); }

        // bounding rect of 4 corners, same floats as the game's loop
        static Rect boundingRectOf(Point const* corner) {
            {
                float const x0 = corner[0].x;
                float const x1 = corner[1].x;
                float const x2 = corner[2].x;
                float const x3 = corner[3].x;
                float const y0 = corner[0].y;
                float const y1 = corner[1].y;
                float const y2 = corner[2].y;
                float const y3 = corner[3].y;
                auto nz = [](float v) { return (v < 0.f) | (v > 0.f); };
                if (nz(x0) & nz(x1) & nz(x2) & nz(x3) & nz(y0) & nz(y1) & nz(y2) & nz(y3)) {
                    float const ax = x0 < x1 ? x0 : x1;
                    float const bx = x2 < x3 ? x2 : x3;
                    float const mnx = ax < bx ? ax : bx;
                    float const cx = x0 < x1 ? x1 : x0;
                    float const dx = x2 < x3 ? x3 : x2;
                    float const mx4 = cx < dx ? dx : cx;
                    float const mxx = mx4 > 0.f ? mx4 : 0.f;
                    float const ay = y0 < y1 ? y0 : y1;
                    float const by = y2 < y3 ? y2 : y3;
                    float const mny = ay < by ? ay : by;
                    float const cy = y0 < y1 ? y1 : y0;
                    float const dy = y2 < y3 ? y3 : y2;
                    float const my4 = cy < dy ? dy : cy;
                    float const mxy = my4 > 0.f ? my4 : 0.f;
                    return Rect{mnx, mny, mxx - mnx, mxy - mny};
                }
            }
            float mnx = 0.f;
            float mny = 0.f;
            float mxx = 0.f;
            float mxy = 0.f;
            for (int i = 0; i < 4; ++i) {
                float const x = corner[i].x;
                float const y = corner[i].y;
                float const nx = (x <= mxx) ? mxx : x;
                if (x < mnx || mnx == 0.f) mnx = x;
                float const ny = (y <= mxy) ? mxy : y;
                if (y < mny || mny == 0.f) mny = y;
                mxx = nx;
                mxy = ny;
            }
            return Rect{mnx, mny, mxx - mnx, mxy - mny};
        }
    };

    struct ObjState {
        bool activated = false;
        bool activatedByPlayer1 = false;
        bool activatedByPlayer2 = false;
        int lastTouchedCommandIndex = -1;
        bool destroyed = false;
        double spawnStored = 0;
    };

    struct Game;

    struct GObj {
        Obj const* o = nullptr;
        Game* g = nullptr;
        int index = -1;
        // true if the handle points to an object
        explicit operator bool() const { return o != nullptr; }
        ObjState const& st() const;
        ObjState& mut() const;
        int dyn() const;
        TimelineKey const* cur() const;
        bool trig() const;

        Point getPosition() const;
        Point getRealPosition() const;
        Point lastPosition() const;
        Rect getObjectRect() const;
        bool disabled() const;
        float rotation() const;
        bool rotationAligned() const;
        Rect getObjectRectWH(float width, float height) const;
        float circleRadius() const;
        // object type (block, hazard, portal...)
        int getType() const { return o->type; }
        // unique id of the object in the level
        int uid() const { return o->uid; }
        bool hasOrientedBox() const;
        bool isFacingDown() const;
        bool isFacingLeft() const;
        int slopeDir() const;
        bool slopeUphill() const;
        bool mirrorFlipped() const;
        bool flipXEff() const;
        bool flipYEff() const;
        float nodeRotX() const;
        bool nodeSkewed() const;
    };

    struct Game {
        World const* world = nullptr;

        FlatMap<int, ObjState> objState;
        std::shared_ptr<FlatMap<int, ObjState> const> objCold;
        float objColdX = -1e30f;
        int objColdShadow = 0;
        ObjState const* osFind(int index) const;
        ObjState& osMut(int index);

        // number of objects with a state
        size_t osSize() const {
            return objState.size() + (objCold ? objCold->size() : 0) - static_cast<size_t>(objColdShadow);
        }

        Player p1;
        Player p2;
        bool isDualMode = false;
        bool p2Origin = false;
        bool isFreeMode = false;
        float timeWarp = 1.f;
        float cameraZoom = 1.f;
        float targetCameraZoom = 1.f;

        bool mirrorOn = false;
        bool mirrorBack = false;
        float mirrorT = 0.f;
        float mirrorDir = 1.f;

        // true in the second half of a mirror transition, when the game flips objects
        bool mirrorFlipPhase() const {
            return mirrorT != 0.f && mirrorT != 1.f && (mirrorDir == 1.f ? 0.5f <= mirrorT : mirrorT <= 0.5f);
        }

        CamKey const* camKey = nullptr;
        float tickGap = 1e9f;
        float obbGap = 1e9f;
        bool rotAmb = false;
        float hazGap = 1e9f;
        double levelTime = 0;
        bool p2Btn = false;
        bool p1Btn = false;
        bool dualTouchTrigger = false;
        unsigned itemChanges = 0;
        double time2 = 0;
        unsigned commandIndex = 0;
        unsigned ringPrevCI = 0;

        uint32_t rng = 0;
        uint32_t tpRng = 0;
        uint32_t rngSeed = kRngSeed;
        // fast_rand state put back on each reset when no recording gave the real one (its bytes spell "ILOT")
        static constexpr uint32_t kRngSeed = 0x494C4F54u;
        // fast_rand is the MSVC rand() linear congruential generator: state = state * 214013 + 2531011
        static constexpr uint32_t kRandMul = 0x343fdu;
        static constexpr uint32_t kRandInc = 0x269ec3u;
        // inverse of kRandMul modulo 2^32, steps the generator backwards. verify ghidra?
        static constexpr uint32_t kRandMulInverse = 0xb9b33155u;

        // draws a number between 0 and 1 like fast_rand
        float rand01() {
            if (g_rngLogFrom >= 0 && tick >= g_rngLogFrom && tick <= g_rngLogTo) rngLogDraw(tick, AP_RET_ADDR());
            rng = rng * kRandMul + kRandInc;
            // like MSVC rand(): bits 16..30 of the state, a value from 0 to 32767
            return static_cast<float>((rng >> 16) & 0x7fffu) / 32767.f;
        }

        static uint32_t rngNext(uint32_t state) { return state * kRandMul + kRandInc; }
        // previous rng state
        static uint32_t rngPrev(uint32_t state) { return (state - kRandInc) * kRandMulInverse; }

        // number of rng draws done by Game::reset
        int resetDraws() const {
            Game t = *this;
            uint32_t const probe = 0x9E3779B9u;
            t.rngSeed = probe;
            t.reset();
            int k = 0;
            for (uint32_t s = probe; s != t.rng && k < 1000000; s = rngNext(s)) ++k;
            return k;
        }

        unsigned currentProgress = 0;
        float parkedSpeed = 0;
        bool parkedSpeedNoEffects = false;
        float lastMaxPortalY = 0;
        float maxGameplayY = 0;
        bool enable22Changes = true;
        bool fixGravityBug = false;
        bool ignoreDamage = false;

        bool spikeOn = false;
        bool spikeVerified = false;
        float spikeX = 0.f;
        float spikeY = 0.f;

        bool timingMargin = false;
        bool noclip = false;
        bool playerDied = false;
        float startX = 0;
        float startY = 105.f;
        bool disableCameraGridSnap = false;
        int lastActivatedPortal1 = 0;
        int lastActivatedPortal2 = 0;
        bool unlinkDualGravity = false;
        bool hasModeGround = false;
        float portalY = 0;
        FlatSet<std::pair<int, int>> triggeredIDs;
        bool isPracticeMode = false;
        bool isEditor = false;
        bool twoPlayerMode = false;
        bool circleOption = false;
        int currentChannel = 0;

        struct ChannelSet {
            uint64_t lo = 0;
            SmallVec<int, 8> hi;

            // true if the channel is in the set
            bool contains(int channel) const {
                return (channel >= 0 && channel < 64) ? ((lo >> channel) & 1u) != 0 : hi.contains(channel);
            }

            // adds a channel
            void insert(int channel) {
                if (channel >= 0 && channel < 64)
                    lo |= 1ull << channel;
                else
                    hi.insert(channel);
            }

            // removes a channel
            void erase(int channel) {
                if (channel >= 0 && channel < 64)
                    lo &= ~(1ull << channel);
                else
                    hi.erase(channel);
            }

            // removes every channel
            void clear() {
                lo = 0;
                hi.clear();
            }

            // hash of the set, for the search keys
            uint32_t hash() const {
                uint64_t h = lo * 0x9E3779B97F4A7C15ull;
                for (int c : hi) h = (h ^ static_cast<uint32_t>(c)) * 0x100000001B3ull;
                return static_cast<uint32_t>(h ^ (h >> 32));
            }
        } reversedChannels;

        struct RotOv {
            int index;
            double dx;
            double dy;
        };
        RotOv* rotOv = nullptr;
        int rotOvN = 0;

        // rotated object entry, nullptr if not rotated
        RotOv const* rotOvFind(int index) const {
            for (int i = 0; i < rotOvN; ++i)
                if (rotOv[i].index == index) return &rotOv[i];
            return nullptr;
        }

        bool isPlatformer = false;
        float lastModeHeight = 0;
        float portalCenterY = 0;
        float cameraOffsetY = 0;
        bool staticCamera = false;
        int lastPortalMode = 0;
        int tick = 0;
        int tlTick = 0;
        int halfTicks = 0;
        bool isBetweenSteps = false;
        uint8_t splitHalf = 0;
        bool preDone = false;
        FlatMap<std::pair<int, int>, unsigned> activatedObjectIDs;
        TrigState trig;
        CameraState cam;

        bool died = false;
        int killerUid = 0;
        int deadPlayer = 0;
        bool completed = false;

        // object by index
        GObj obj(int index) { return GObj{&world->objs[static_cast<size_t>(index)], this, index}; }

        // object by uid, empty if not found
        GObj objByUid(int uid) {
            int i = world->indexOfUid(uid);
            return i < 0 ? GObj{} : obj(i);
        }

        void reset();
    };

    // hot table first, then the cold one
    inline ObjState const* Game::osFind(int index) const {
        if (ObjState const* s = objState.find(index)) return s;
        if (objCold) {
            Obj const& o = world->objs[static_cast<size_t>(index)];
            if (o.rect.x + o.rect.w < objColdX) return objCold->find(index);
        }
        return nullptr;
    }

    // always writes to the hot table
    inline ObjState& Game::osMut(int index) {
        if (ObjState* s = objState.find(index)) return *s;
        if (objCold) {
            Obj const& o = world->objs[static_cast<size_t>(index)];
            if (o.rect.x + o.rect.w < objColdX)
                if (ObjState const* c = objCold->find(index)) {
                    ObjState const v = *c;
                    ObjState& s = objState[index];
                    s = v;
                    ++objColdShadow;
                    return s;
                }
        }
        return objState[index];
    }

    // state to read, default state if never touched
    inline ObjState const& GObj::st() const {
        static ObjState const kDefault{};
        ObjState const* s = g->objState.find(index);
        if (!s && g->objCold && o->rect.x + o->rect.w < g->objColdX) s = g->objCold->find(index);
        return s ? *s : kDefault;
    }

    // state to write, creates it
    inline ObjState& GObj::mut() const { return g->osMut(index); }

    // index in the recorded timeline, -1 if static
    inline int GObj::dyn() const {
        Timeline const* tl = g ? g->world->timeline : nullptr;
        return (tl && index >= 0 && static_cast<size_t>(index) < tl->dynOf.size()) ? tl->dynOf[static_cast<size_t>(index)] : -1;
    }

    // true if triggers are simulated
    inline bool GObj::trig() const { return g && g->trig.on; }

    // from triggers, the timeline, or the exported position
    inline Point GObj::getRealPosition() const {
        double x;
        double y;
        if (trig()) {
            if (!g->trig.anyMotion() || !o->movable || !trigObjMoved(*g, *o)) {
                x = o->posX;
                y = o->posY;
            } else
                trigRealPos(*g, index, x, y);
        } else if (auto* k = cur()) {
            x = static_cast<double>(k->x);
            y = static_cast<double>(k->y);
        } else {
            x = o->posX;
            y = o->posY;
        }
        if (g && g->rotOvN)
            if (auto* r = g->rotOvFind(index)) {
                if (!o->lockX) x += r->dx;
                y += r->dy;
            }
        return {static_cast<float>(x), static_cast<float>(y)};
    }

    Rect rotatedObjectRect(GObj const& o, Game::RotOv const& ov);

    // current hitbox
    inline Rect GObj::getObjectRect() const {
        if (g && g->rotOvN)
            if (auto* ov = g->rotOvFind(index)) return rotatedObjectRect(*this, *ov);
        Rect r;
        if (trig()) {
            if (!g->trig.anyMotion() || !o->movable || !trigObjMoved(*g, *o))
                r = o->rect;
            else
                r = trigRectCached(*g, index);
        } else if (auto* k = cur()) {
            if (o->rectGen)
                r = genericObjRectStatic(*o, k->x, k->y);
            else {
                r = o->rect;
                r.x += k->x - static_cast<float>(o->posX);
                r.y += k->y - static_cast<float>(o->posY);
            }
        } else
            r = o->rect;
        return r;
    }

    // m_isGroupDisabled or m_isDisabled
    inline bool GObj::disabled() const {
        if (trig()) return o->disabled || (o->toggleable && trigGroupDisabled(*g, *o));
        if (auto* k = cur()) return (k->flags & 3u) != 0 || o->disabled;
        return o->groupDisabled || o->disabled;
    }

    inline float GObj::rotation() const {
        float r;
        if (trig())
            r = (o->movable && trigObjMoved(*g, *o)) ? o->rot + trigRotation(*g, index) : o->rot;
        else if (auto* k = cur())
            r = k->rot;
        else
            r = o->rot;
        if (g && g->rotOvN && g->rotOvFind(index)) r -= 90.f;
        return r;
    }

    // m_isRotationAligned: rotated by exactly 90 or 270
    inline bool GObj::rotationAligned() const {
        if (g && g->rotOvN && g->rotOvFind(index)) {
            float const r = rotation();
            return r == 90.f || r == -90.f || r == 270.f || r == -270.f;
        }
        if (trig() && g->trig.anyMotion() && o->movable && trigObjMoved(*g, *o)) {
            float const a = trigRotation(*g, index);
            if (a != 0.f) {
                float const r = o->rot + a;
                return r == 90.f || r == -90.f || r == 270.f || r == -270.f;
            }
        }
        return o->rotationAligned;
    }

    // signed scale like GameObject::getObjectRect, negative sizes become 0 like CCRect
    inline Rect GObj::getObjectRectWH(float width, float height) const {
        float ax = o->scaleX;
        float ay = o->scaleY;
        if (trig() && g->trig.anyMotion() && o->movable) trigScaleAbs(*g, index, ax, ay);
        float w = ax * o->width * width;
        float h = ay * o->height * height;
        if (rotationAligned()) std::swap(w, h);
        Point p = getRealPosition();
        float bx = o->boxOffX;
        float by = o->boxOffY;
        return Rect{p.x + bx - w * 0.5f, p.y + by - h * 0.5f, w < 0.f ? 0.f : w, h < 0.f ? 0.f : h};
    }

    // radius times the biggest current scale, like playerCircleCollision
    inline float GObj::circleRadius() const {
        float ax = o->scaleX;
        float ay = o->scaleY;
        if (trig() && g->trig.anyMotion() && o->movable) {
            World const& w = *g->world;
            size_t const i = static_cast<size_t>(index);
            if ((i < w.scaleTarget.size() && w.scaleTarget[i]) || (w.hasAreaEffects && w.areaTarget[i]))
                trigScaleAbs(*g, index, ax, ay);
        }
        float r = ax;
        if (r == 1.f && ay == 1.f) return o->radius;
        if (r <= ay) r = ay;
        return r * o->radius;
    }

    // true while the mirror transition flips the object (PlayLayer::screenFlipObject)
    inline bool GObj::mirrorFlipped() const { return g && g->mirrorFlipPhase(); }

    // objects at 90 or 270 get flipped on y instead
    inline bool GObj::flipXEff() const {
        if (!mirrorFlipped()) return o->flipX;
        float const a = std::fabs(rotation());
        return (a == 90.f || a == 270.f) ? o->flipX : !o->flipX;
    }

    // flipY with the mirror flip
    inline bool GObj::flipYEff() const {
        if (!mirrorFlipped()) return o->flipY;
        float const a = std::fabs(rotation());
        return (a == 90.f || a == 270.f) ? !o->flipY : o->flipY;
    }

    // the mirror sets the node to -start rotation when it is not a multiple of 90
    inline float GObj::nodeRotX() const {
        float const r = rotation();
        if (mirrorFlipped() && static_cast<int>(std::fabs(r)) % 90 != 0) return -o->rot;
        return r;
    }

    //true if rotation x and y are opposite (startDashing)
    inline bool GObj::nodeSkewed() const {
        if (mirrorFlipped() && static_cast<int>(std::fabs(rotation())) % 90 != 0) return false;
        return 179.f < std::fabs(o->rotX - o->rotY);
    }

    // m_shouldUseOuterOb, or rotated by a trigger
    inline bool GObj::hasOrientedBox() const {
        return o->useOuterOb ||
               (trig() && g->trig.anyMotion() && o->movable && trigObjMoved(*g, *o) && trigRotated(*g, index));
    }

    // node position (CCNode::getPosition), start of the tick for moving objects
    inline Point GObj::getPosition() const {
        if (trig()) {
            if (!g->trig.anyMotion() || !o->movable || !trigObjMoved(*g, *o))
                return {static_cast<float>(o->posX), static_cast<float>(o->posY)};
            float x;
            float y;
            trigNodePos(*g, index, x, y);
            return {x, y};
        }
        int const d = dyn();
        if (d >= 0) {
            TimelineKey const& k = g->world->timeline->at(d, g->tlTick - 1);
            return {k.nx, k.ny};
        }
        return getRealPosition();
    }

    // real position on the last tick (m_lastPosition)
    inline Point GObj::lastPosition() const {
        Point p;
        if (trig()) {
            if (!g->trig.anyMotion() || !o->movable || !trigObjMoved(*g, *o))
                p = {static_cast<float>(o->posX), static_cast<float>(o->posY)};
            else {
                float x;
                float y;
                trigLastPos(*g, index, x, y);
                p = {x, y};
            }
        } else {
            int const d = dyn();
            if (d >= 0) {
                TimelineKey const& k = g->world->timeline->at(d, g->tlTick - 1);
                p = {k.x, k.y};
            } else
                p = {static_cast<float>(o->posX), static_cast<float>(o->posY)};
        }
        if (g && g->rotOvN)
            if (auto* r = g->rotOvFind(index)) {
                p.x += static_cast<float>(r->dx);
                p.y += static_cast<float>(r->dy);
            }
        return p;
    }

    // timeline key of this tick, nullptr if none, TODO verify exception....
    inline TimelineKey const* GObj::cur() const {
        if (trig()) return nullptr;
        int const d = dyn();
        return d >= 0 ? &g->world->timeline->at(d, g->tlTick) : nullptr;
    }

    void setYVelocity(Player& p, double v);
    bool playerIsFallingBugged(Player const& p);
    void flipGravity(Player& p, bool flip, bool noEffects);
    void layerFlipGravity(Game& g, Player& p, bool flip, bool noEffects);
    void toggleFlipped(Game& g, bool flip, bool noEffects);
    void updateTimeMod(Player& p, float speed, bool noEffects);
    void togglePlayerScale(Player& p, bool enable, bool noEffects);
    bool toggleFlyMode(Player& p, bool enable, bool noEffects);
    bool toggleRollMode(Player& p, bool enable, bool noEffects);
    bool toggleBirdMode(Player& p, bool enable, bool noEffects);
    bool toggleDartMode(Player& p, bool enable, bool noEffects);
    bool toggleRobotMode(Player& p, bool enable, bool noEffects);
    bool toggleSpiderMode(Player& p, bool enable, bool noEffects);
    bool toggleSwingMode(Player& p, bool enable, bool noEffects);
    void switchedToMode(Player& p, int type);
    void resetPlayerIcon(Player& p);
    void updateJump(Game& g, Player& p, float dt);
    void playerUpdate(Game& g, Player& p, float dt);
    bool pushButton(Game& g, Player& p, int button);
    bool releaseButton(Game& g, Player& p, int button);
    void disablePlayerControls(Game& g, Player& p);
    void doReversePlayer(Game& g, Player& p, bool left);
    void applyCameraMode(Game& g, GObj object);
    void playerRotateGameplay(Game& g, Player& p, int moveDir, int groundDir, bool editVel, float modX, float modY, bool overrideVel, bool dontSlide);
    void reversePlayer(Game& g, Player& p, GObj object);
    void enablePlayerControls(Game& g, Player& p);
    void propellPlayer(Player& p, float yVelocity, bool noEffects, int objectType);
    void bumpPlayer(Game& g, Player& p, float bumpMod, int objectType, bool noEffects, GObj object);
    void boostPlayer(Player& p, float yVelocity);
    void ringJump(Game& g, Player& p, GObj ring, bool skipCheck);
    void startDashing(Game& g, Player& p, GObj ring);
    void stopDashing(Game& g, Player& p);
    void updateDashArt(Game& g, Player& p);
    void spiderTestJump(Game& g, Player& p, bool dynamic);
    void spiderTestJumpInternal(Game& g, Player& p, bool dynamic);
    void resetObject(Game& g, Player& p);
    void releaseAllButtons(Game& g, Player& p);
    void incrementJumps(Player& p);

    Rect playerRect(Player const& p);
    Rect playerInnerRect(Player const& p);

    // distance between two rects, negative when they overlap
    inline float rectGap(Rect const& a, Rect const& b) {
        return std::max(std::max(b.x - (a.x + a.w), a.x - (b.x + b.w)), std::max(b.y - (a.y + a.h), a.y - (b.y + b.h)));
    }

    // call after moving the player, both boxes need a refresh
    inline void playerMoved(Player& p) {
        p.rectDirty = true;
        p.obbDirty = true;
    }

    void refreshPlayerRect(Player const& p);
    void updateCollide(Player& p, int direction, GObj object);
    void hitGround(Game& g, Player& p, GObj object, bool notFlipped);
    void hitGroundNoJump(Game& g, Player& p, GObj object, bool notFlipped);
    void checkSnapJumpToObject(Game& g, Player& p, GObj object);
    bool collidedWithObject(Game& g, Player& p, float dt, GObj object, Rect rect, bool skipCheck);
    bool collidedWithObjectInternal(Game& g, Player& p, float dt, GObj object, Rect rect, bool skipCheck);
    void rotateGameplayObject(Game& g, Player& p, GObj object);
    bool handleRotatedCollisionInternal(Game& g, Player& p, float dt, GObj object, Rect rect, bool skipCheck,
                                        bool skipPre, bool slope);
    void collidedWithSlopeInternal(Game& g, Player& p, float dt, GObj object, bool forced);
    bool preSlopeCollision(Game& g, Player& p, float dt, GObj object);
    double slopeYPos(GObj slope, float x);
    void updateSlopeRotation(Player& p, float dt);
    float convertToClosestRotation(Player const& p, float rotation);
    void updateRotationTo(Player& p, float dt, float rotation);
    void postCollision(Game& g, Player& p, float dt, bool betweenSteps);
    int checkCollisions(Game& g, Player& p, float dt, bool ignoreDamage);
    void collisionCheckObjects(Game& g, Player& p, std::vector<int> const& objects, float dt);
    bool playerCircleCollision(Game& g, Player& p, GObj object);
    OBB objectObb(GObj o);
    float getMaxPortalY(Game const& g);
    float getMinPortalY(Game const& g);

    void layerBumpPlayer(Game& g, Player& p, GObj object);
    void playerTouchedRing(Game& g, Player& p, GObj object);
    void resetTouchedRingsForTick(Player& p);
    void runNormalRotation(Player& p, bool notNormalMode, float speed);
    void runBallRotation(Player& p, float speed);
    void runBallRotation2(Player& p);
    void updateRotation(Game& g, Player& p, float dt);

    void playerTouchedTrigger(Game& g, Player& p, GObj object);
    float getGroundHeightForMode(Game const& g, int type);
    void playerWillSwitchMode(Game& g, Player& p, GObj object);
    bool canBeActivatedByPlayer(Game& g, Player& p, GObj object);
    void activatedByPlayer(Player& p, GObj object);
    void triggerActivated(GObj object);
    long cellCacheBuilds();
    void filterStats(long& scanned, long& mov, long& calls, long& kept);
    long filterStatMovDeco();
    bool hasBeenActivatedByPlayer(Player const& p, GObj object);
    void teleportPlayer(Game& g, GObj portal, Player& p);
    void toggleDualMode(Game& g, GObj object, bool dual, Player& p, bool noEffects);
    void destroyPlayer(Game& g, Player& p, GObj object);
    void handleButton(Game& g, bool down, int button, bool isPlayer1);
    void resetPlayer(Game& g);
    void stepTick(Game& g, uint8_t buttons);
    void stepTickDt(Game& g, uint8_t buttons, float dt, bool betweenSteps);
    void stepPre(Game& g, float dt, bool betweenSteps);
}
