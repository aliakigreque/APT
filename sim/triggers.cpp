// Simulates the GD 2.2081 triggers, transcribed from GJBaseGameLayer, GJEffectManager and GroupCommandObject2.
// This file has the easing curves, the constructors of the trigger state and the list of supported triggers; the
// other triggers_*.cpp files each handle one family of triggers or one stage of a step.

#include "triggers_internal.hpp"

namespace ap::sim {
    namespace trigger_impl {
        // bounce curve (GameToolbox::bounceTime)
        float bounceTime(float time) {
            double const t = static_cast<double>(time);
            if (t < 0.36363636363636365) return 7.5625f * time * time;
            if (t < 0.7272727272727273) {
                time += -0.5454545617103577f;
                return 7.5625f * time * time + 0.75f;
            }
            if (t < 0.9090909090909091) {
                time += -0.8181818127632141f;
                return 7.5625f * time * time + 0.9375f;
            }
            time += -0.9545454382896423f;
            return 7.5625f * time * time + 0.984375f;
        }

    }

    // copy of GameToolbox::getEasedValue
    float easedValue(float t, int type, float rate) {
        float result = t;
        if (type == 0) return t;
        if (rate <= 0.f) rate = 2.f;
        switch (type) {
        case 1: {
            float x = t + t;
            if (1.f <= x)
                result = 1.f - gpow(2.f - x, rate) * 0.5f;
            else
                result = gpow(x, rate) * 0.5f;
        } break;
        case 2:
            result = gpow(t, rate);
            break;
        case 3:
            result = gpow(t, 1.f / rate);
            break;
        case 4:
            if (t != 0.f && t != 1.f) {
                float const s = t + t + -1.f;
                float wave = (s - rate * 0.25f) * 3.1415927f;
                wave = gsin((wave + wave) / rate);
                if (s < 0.f)
                    result = gpow(2.f, s * 10.f) * -0.5f * wave;
                else
                    result = gpow(2.f, s * -10.f) * wave * 0.5f + 1.f;
            }
            break;
        case 5:
            if (t != 0.f && t != 1.f) {
                float wave = ((t + -1.f) - rate * 0.25f) * 3.1415927f;
                wave = gsin((wave + wave) / rate);
                result = wave * -gpow(2.f, (t + -1.f) * 10.f);
            }
            break;
        case 6:
            if (t != 0.f && t != 1.f) {
                float wave = (t - rate * 0.25f) * 3.1415927f;
                wave = gsin((wave + wave) / rate);
                result = wave * gpow(2.f, t * -10.f) + 1.f;
            }
            break;
        case 7:
            if (0.5f <= t)
                result = bounceTime((t + t) - 1.f) * 0.5f + 0.5f;
            else
                result = (1.f - bounceTime(1.f - (t + t))) * 0.5f;
            break;
        case 8:
            result = 1.f - bounceTime(1.f - t);
            break;
        case 9:
            result = bounceTime(t);
            break;
        case 10: {
            float x = (t + t) - 1.f;
            if (1.f <= t + t)
                result = (2.f - gpow(2.f, x * -10.f)) * 0.5f;
            else
                result = gpow(2.f, x * 10.f) * 0.5f;
        } break;
        case 11:
            result = 0.f;
            if (t != 0.f) result = gpow(2.f, (t - 1.f) * 10.f) - 0.001f;
            break;
        case 12:
            result = 1.f;
            if (t != 1.f) result = 1.f - gpow(2.f, t * -10.f);
            break;
        case 13:
            result = (gcos(t * 3.1415927f) - 1.f) * -0.5f;
            break;
        case 14:
            result = 1.f - gcos(t * 1.5707964f);
            break;
        case 15:
            result = gsin(t * 1.5707964f);
            break;
        case 16: {
            float x = t + t;
            if (1.f <= x) {
                x = x + -2.f;
                result = (x * 3.5949094f + 2.5949094f) * x * x * 0.5f + 1.f;
            } else
                result = (x * 3.5949094f - 2.5949094f) * x * x * 0.5f;
        } break;
        case 17:
            result = t * t * (t * 2.70158f - 1.70158f);
            break;
        case 18: {
            float x = t + -1.f;
            result = x * x * (x * 2.70158f + 1.70158f) + 1.f;
        } break;
        default:
            break;
        }
        return result;
    }

    // zeroed move command
    GroupCmd::GroupCmd() {
        std::memset(static_cast<void*>(this), 0, sizeof *this);
        modX = 1;
        modY = 1;
        alreadyUpdated = true;
    }

    // zeroed group offset
    GroupOff::GroupOff() {
        std::memset(static_cast<void*>(this), 0, sizeof *this);
        stamp = -1;
    }

    // zeroed object state
    ObjDyn::ObjDyn() {
        std::memset(static_cast<void*>(this), 0, sizeof *this);
        stamp = -1;
        sclX = 1.f;
        sclY = 1.f;
        cSclX = 1.f;
        cSclY = 1.f;
        tStamp = -1;
    }

    // zeroed area instance
    AreaInst::AreaInst() {
        std::memset(static_cast<void*>(this), 0, sizeof *this);
        def = -1;
    }

    // zeroed area offset, latch stamp set to never
    AreaOff::AreaOff() {
        std::memset(static_cast<void*>(this), 0, sizeof *this);
        stamp = -1000000;
        latchStamp = -1000000;
    }

    SpawnAction::SpawnAction() {
        std::memset(static_cast<void*>(this), 0, sizeof *this);
        obj = -1;
    }

    CountAction::CountAction() { std::memset(static_cast<void*>(this), 0, sizeof *this); }

    CollisionAction::CollisionAction() { std::memset(static_cast<void*>(this), 0, sizeof *this); }

    // zeroed touch action
    TouchAction::TouchAction() { std::memset(static_cast<void*>(this), 0, sizeof *this); }

    // zeroed tween
    Tween::Tween() { std::memset(static_cast<void*>(this), 0, sizeof *this); }

    // zeroed timer
    TimerItem::TimerItem() { std::memset(static_cast<void*>(this), 0, sizeof *this); }

    // zeroed time event
    TimeEvent::TimeEvent() { std::memset(static_cast<void*>(this), 0, sizeof *this); }

    // zeroed event listener
    EventListener::EventListener() { std::memset(static_cast<void*>(this), 0, sizeof *this); }

    // zeroed advanced follow instance
    AdvInst::AdvInst() { std::memset(static_cast<void*>(this), 0, sizeof *this); }

    // zeroed advanced follow physics
    AfPhys::AfPhys() { std::memset(static_cast<void*>(this), 0, sizeof *this); }

    // zeroed dynamic rotation
    DynRot::DynRot() { std::memset(static_cast<void*>(this), 0, sizeof *this); }

    // zeroed sequence state
    SeqState::SeqState() { std::memset(static_cast<void*>(this), 0, sizeof *this); }

    // true if the trigger has an effect the sim reproduces (colors, pulse, shake... are ignored)
    bool hasSimEffect(int id) {
        switch (id) {
        case 901:
        case 1049:
        case 1268:
        case 1346:
        case 1347:
        case 1814:
        case 1815:
        case 1595:
        case 1616:
        case 1611:
        case 1811:
        case 1817:
        case 1913:
        case 1916:
        case 1935:
        case 3600:
        case 200:
        case 201:
        case 202:
        case 203:
        case 1334:
        case 3022:
        case 2066:
        case 2899:
        case 1932:
        case 1917:
        case 2900:
        case 2925:
        case 3607:
        case 3609:
        case 3614:
        case 3615:
        case 3617:
        case 3619:
        case 3620:
        case 3641:
        case 2067:
        case 3033:
        case 3640:
        case 3006:
        case 3007:
        case 3008:
        case 3011:
        case 3012:
        case 3013:
        case 3024:
        case 3604:
        case 3608:
        case 3618:
        case 3661:
        case 3660:
        case 3016:
        case 1912:
        case 2068:
        case 1914:
        case 2062:
        case 2901:
            return true;
        default:
            return false;
        }
    }

    // copy of GameObject::isSpawnableTrigger there has to be a better way :(
    bool isSpawnableTrigger(int id) {
        switch (id) {
        case 29:
        case 30:
        case 32:
        case 33:
        case 105:
        case 744:
        case 899:
        case 900:
        case 901:
        case 915:
        case 1006:
        case 1007:
        case 1049:
        case 1268:
        case 1346:
        case 1347:
        case 1520:
        case 1585:
        case 1595:
        case 1611:
        case 1612:
        case 1613:
        case 1616:
        case 1811:
        case 1812:
        case 1814:
        case 1815:
        case 1817:
        case 1818:
        case 1819:
        case 1912:
        case 1913:
        case 1914:
        case 1916:
        case 1917:
        case 1931:
        case 1932:
        case 1934:
        case 1935:
        case 2015:
        case 2062:
        case 2063:
        case 2066:
        case 2067:
        case 2068:
        case 2899:
            return true;
        case 2900:
        case 2901:
        case 2903:
        case 2904:
        case 2905:
        case 2907:
        case 2909:
        case 2910:
        case 2911:
        case 2912:
        case 2913:
        case 2914:
        case 2915:
        case 2916:
        case 2917:
        case 2919:
        case 2920:
        case 2921:
        case 2922:
        case 2923:
        case 2924:
        case 2925:
        case 2999:
        case 3029:
        case 3030:
        case 3031:
        case 3033:
        case 3600:
        case 3602:
        case 3603:
        case 3604:
        case 3605:
        case 3606:
        case 3607:
        case 3608:
        case 3609:
        case 3612:
        case 3613:
        case 3614:
        case 3615:
        case 3617:
        case 3618:
        case 3619:
        case 3620:
        case 3640:
        case 3641:
        case 3655:
        case 3660:
        case 3661:
        case 3662:
            return true;
        default:
            return id >= 3006 && id <= 3024;
        }
    }

    // copy of GameObject::shouldLockX: moves and follows don't apply x offsets to these objects
    bool shouldLockX(int id) {
        if (id >= 0x16 && id <= 0x22) return true;
        if (id >= 0x37 && id <= 0x3b) return true;
        switch (id) {
        case 0x69:
        case 200:
        case 201:
        case 202:
        case 203:
        case 744:
        case 899:
        case 900:
        case 901:
        case 915:
        case 1006:
        case 1007:
        case 1049:
        case 1268:
        case 1334:
        case 1346:
        case 1347:
        case 1616:
        case 1812:
        case 1814:
        case 1815:
        case 1912:
        case 1913:
        case 1914:
        case 1915:
        case 1916:
        case 1917:
        case 1934:
        case 1935:
        case 2015:
        case 2062:
        case 2067:
        case 2068:
        case 2900:
        case 2901:
        case 2925:
        case 2999:
        case 3023:
        case 3024:
        case 3029:
        case 3030:
        case 3031:
        case 3612:
        case 3613:
        case 3640:
        case 3660:
        case 3661:
        case 3662:
            return true;
        default:
            break;
        }
        if (id >= 3006 && id <= 3021) return true;
        if (id >= 3602 && id <= 3609) return true;
        return false;
    }

    // why the simulator can't replay this object, nullptr if it can
    char const* unsupportedReason(Obj const& o) {
        if (o.cls != 1) return nullptr;
        if (o.type == Collectible || o.type == UserCoin) return nullptr;
        if (!isSpawnableTrigger(o.id) && !o.trigger) return nullptr;
        bool const targetMode = o.tf(Obj::TF_UseMoveTarget) || o.tf(Obj::TF_DirSnap);
        switch (o.id) {
        case 901:
            return nullptr;
        case 1049:
        case 1347:
        case 1814:
        case 1595:
        case 1616:
        case 1611:
        case 1811:
        case 1817:
        case 1815:
        case 1812:
        case 1913:
        case 1916:
        case 1935:
        case 1931:
        case 1934:
        case 1816:
        case 2066:
        case 1520:
        case 1585:
            return nullptr;
        case 1268:
            if (o.tf(Obj::TF_HasRemap) && o.remapIdx <= 0)
                return "spawn with remap (old export: export the level again)";
            return nullptr;
        case 1346:
            (void)targetMode;
            return nullptr;
        case 29:
        case 30:
        case 32:
        case 33:
        case 105:
        case 744:
        case 899:
        case 900:
        case 915:
        case 1006:
        case 1007:
            return nullptr;
        case 1912:
            return nullptr;
        case 1914:
            return o.ext >= 0 ? nullptr : "static camera (old export: export the level again)";
        case 1932:
            return (o.optFlags & 16) ? nullptr : "player control (old export)";
        case 2015:
            return nullptr;
        case 2062:
            return o.ext >= 0 ? nullptr : "camera edge (old export: export the level again)";
        case 2016:
            return nullptr;
        case 2063:
            return "checkpoint (2.2)";
        case 2899:
            return (o.optFlags && o.optValues < 0) ? "game options (old export: unknown values)" : nullptr;
        case 2067: {
            if (o.ext < 0) return "scale (old export: export the level again)";
            return nullptr;
        }
        case 2068:
            return o.ext >= 0 ? nullptr : "advanced random (old export: export the level again)";
        case 2925:
            return (o.camFlags & 4) ? nullptr : "camera mode (old export)";
        case 3607:
        case 3614:
        case 3615:
        case 3617:
        case 3619:
        case 3620:
        case 3641:
            return o.ext >= 0 ? nullptr : "2.2 item / timer / sequence (old export: export the level again)";
        case 3609:
            return nullptr;
        case 3640:
            return nullptr;
        case 3618:
            return nullptr;
        case 3661:
            return nullptr;
        case 3660:
            return o.ext >= 0 ? nullptr : "advanced follow edit (old export: export the level again)";
        default:
            break;
        }
        if (o.id < 2900) return nullptr;
        switch (o.id) {
        case 2903:
        case 2999:
        case 3006:
        case 3007:
        case 3008:
        case 3009:
        case 3010:
        case 3011:
        case 3012:
        case 3013:
        case 3014:
        case 3015:
        case 3017:
        case 3018:
        case 3019:
        case 3020:
        case 3021:
        case 3023:
        case 3024:
        case 3029:
        case 3030:
        case 3031:
        case 3600:
        case 3602:
        case 3603:
        case 3605:
        case 3606:
        case 3608:
        case 3612:
        case 3613:
        case 3642:
        case 3662:
        case 3604:
            return nullptr;
        case 3655:
            return nullptr;
        case 3032:
            return nullptr;
        case 3016:
            return o.ext >= 0 ? nullptr : "advanced follow (old export: export the level again)";
        case 3022:
            if (o.tpRedirectDash) return "teleport with dash redirection";
            return nullptr;
        case 3033:
            return o.ext >= 0 ? nullptr : "keyframe (old export: export the level again)";
        case 2900:
            return (o.rgFlags & 64) ? nullptr : "rotate gameplay (old export: export the level again)";
        case 3645:
            return nullptr;
        case 2902:
            return nullptr;
        case 2901:
            return nullptr;
        default:
            break;
        }
        if (o.id >= 2904 && o.id <= 2924) return nullptr;
        return "2.2 trigger not simulated";
    }

    //camera triggers, read from the recorded track when the camera can't be simulated
    bool cameraOnlyTrigger(int id) {
        return id == ObjId::StaticCameraTrigger || id == ObjId::RotateCameraTrigger || id == ObjId::EdgeCameraTrigger || id == ObjId::CameraModeTrigger || id == ObjId::GameplayOffsetTrigger;
    }

}
