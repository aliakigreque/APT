// Level analysis done once on load: keyframe splines, menus, and the tables that say which groups and triggers
// can move gameplay objects.

#include "triggers_internal.hpp"

namespace ap::sim {
    namespace trigger_impl {
        // natural cubic spline through the points (x[i], y[i]), same math as tk::spline
        void kfSplineSet(World::KfSpline& spline, std::vector<double> const& x, std::vector<double> const& y) {
            int const n = static_cast<int>(x.size());
            spline.x = x;
            spline.y = y;
            std::vector<double> lower(static_cast<size_t>(n), 0.0);
            std::vector<double> diag(static_cast<size_t>(n), 0.0);
            std::vector<double> upper(static_cast<size_t>(n), 0.0);
            std::vector<double> rhs(static_cast<size_t>(n), 0.0);
            auto xAt = [&](int i) { return x[static_cast<size_t>(i)]; };
            auto yAt = [&](int i) { return y[static_cast<size_t>(i)]; };
            for (int i = 1; i < n - 1; ++i) {
                lower[static_cast<size_t>(i)] = 1.0 / 3.0 * (xAt(i) - xAt(i - 1));
                diag[static_cast<size_t>(i)] = 2.0 / 3.0 * (xAt(i + 1) - xAt(i - 1));
                upper[static_cast<size_t>(i)] = 1.0 / 3.0 * (xAt(i + 1) - xAt(i));
                rhs[static_cast<size_t>(i)] =
                        (yAt(i + 1) - yAt(i)) / (xAt(i + 1) - xAt(i)) - (yAt(i) - yAt(i - 1)) / (xAt(i) - xAt(i - 1));
            }
            diag[0] = 2.0;
            upper[0] = 0.0;
            rhs[0] = 0.0;
            diag[static_cast<size_t>(n - 1)] = 2.0;
            lower[static_cast<size_t>(n - 1)] = 0.0;
            rhs[static_cast<size_t>(n - 1)] = 0.0;
            std::vector<double> invDiag(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) {
                size_t const k = static_cast<size_t>(i);
                invDiag[k] = 1.0 / diag[k];
                if (i > 0) lower[k] *= invDiag[k];
                if (i < n - 1) upper[k] *= invDiag[k];
                diag[k] = 1.0;
            }
            for (int k = 0; k < n - 1; ++k) {
                size_t const row = static_cast<size_t>(k);
                size_t const next = static_cast<size_t>(k + 1);
                double const factor = -lower[next] / diag[row];
                lower[next] = -factor;
                diag[next] = diag[next] + factor * upper[row];
            }
            std::vector<double> forward(static_cast<size_t>(n));
            std::vector<double> c(static_cast<size_t>(n));
            for (int i = 0; i < n; ++i) {
                size_t const k = static_cast<size_t>(i);
                double const sum = i > 0 ? lower[k] * forward[k - 1] : 0.0;
                forward[k] = rhs[k] * invDiag[k] - sum;
            }
            for (int i = n - 1; i >= 0; --i) {
                size_t const k = static_cast<size_t>(i);
                double const sum = i < n - 1 ? upper[k] * c[k + 1] : 0.0;
                c[k] = (forward[k] - sum) / diag[k];
            }
            spline.c = c;
            spline.d.assign(static_cast<size_t>(n), 0.0);
            spline.b.assign(static_cast<size_t>(n), 0.0);
            for (int i = 0; i < n - 1; ++i) {
                size_t const k = static_cast<size_t>(i);
                spline.d[k] = 1.0 / 3.0 * (c[k + 1] - c[k]) / (xAt(i + 1) - xAt(i));
                spline.b[k] = (yAt(i + 1) - yAt(i)) / (xAt(i + 1) - xAt(i)) -
                              1.0 / 3.0 * (2.0 * c[k] + c[k + 1]) * (xAt(i + 1) - xAt(i));
            }
            double const lastWidth = xAt(n - 1) - xAt(n - 2);
            spline.d[static_cast<size_t>(n - 1)] = 0.0;
            spline.b[static_cast<size_t>(n - 1)] = 3.0 * spline.d[static_cast<size_t>(n - 2)] * lastWidth * lastWidth +
                                                   2.0 * c[static_cast<size_t>(n - 2)] * lastWidth +
                                                   spline.b[static_cast<size_t>(n - 2)];
            spline.c0 = c[0];
        }

        // value of the spline at time t
        double kfSplineEval(World::KfSpline const& spline, double t) {
            size_t const n = spline.x.size();
            auto it = std::upper_bound(spline.x.begin(), spline.x.end(), t);
            size_t idx = static_cast<size_t>(std::max(static_cast<long>(it - spline.x.begin()) - 1, 0L));
            double const h = t - spline.x[idx];
            if (t < spline.x[0]) return (spline.c0 * h + spline.b[0]) * h + spline.y[0];
            if (t > spline.x[n - 1]) return (spline.c[n - 1] * h + spline.b[n - 1]) * h + spline.y[n - 1];
            return ((spline.d[idx] * h + spline.c[idx]) * h + spline.b[idx]) * h + spline.y[idx];
        }

        // entries of a keyframe animation, like GJEffectManager::createKeyframeCommand
        void buildKeyframeAnim(World const& w, std::vector<int> const& keys,
                               std::vector<World::KfKeyState> const& keyStates, World::KfAnim& anim,
                               std::vector<World::KfSpline>& splines, float timeMod, float posXMod, float posYMod,
                               float rotMod, float scaleXMod, float scaleYMod) {
            double const timeScale = std::max(0.0001f, timeMod);
            auto& entries = anim.e;
            entries.clear();
            int const n = static_cast<int>(keys.size());
            if (n == 0) return;
            entries.reserve(static_cast<size_t>(n) + 1);
            float firstX = 0;
            float firstY = 0;
            float firstRot = 0;
            float firstScaleX = 1;
            float firstScaleY = 1;
            float prevPx = 0;
            float prevPy = 0;
            float prevRot = 0;
            float prevScaleXRatio = 1;
            float prevScaleYRatio = 1;
            int prevEntryIdx = -1;
            int prevKeyIdx = -1;
            bool looped = false;
            for (int i = 0; i < n; ++i) {
                int const objIdx = keys[static_cast<size_t>(i)];
                Obj const& key = w.objs[static_cast<size_t>(objIdx)];
                World::KfKeyState const& keyState = keyStates[static_cast<size_t>(i)];
                World::KfEntry entry;
                entry.uid = key.uid;
                float px = keyState.px;
                float py = keyState.py;
                double scaleX = keyState.sx;
                double scaleY = keyState.sy;
                double rot = keyState.rotX;
                double const rotY = keyState.rotY;
                double const rotDiff = std::fabs(rot - rotY);
                if (rotDiff <= 179.999 || 180.001 <= rotDiff) {
                    if (scaleX < 0.0 && scaleY < 0.0) {
                        rot += rot <= 0.0 ? 180.0 : -180.0;
                        scaleX = -scaleX;
                        scaleY = -scaleY;
                    }
                } else if (std::fabs(rot) <= std::fabs(rotY))
                    scaleX = -scaleX;
                else {
                    rot += rot <= 0.0 ? 180.0 : -180.0;
                    scaleY = -scaleY;
                }
                double scaleXRatio;
                double scaleYRatio;
                if (i == 0) {
                    firstX = px;
                    firstY = py;
                    firstRot = static_cast<float>(rot);
                    firstScaleX = static_cast<float>(scaleX);
                    firstScaleY = static_cast<float>(scaleY);
                    scaleXRatio = 1.0;
                    scaleYRatio = 1.0;
                } else {
                    if (posXMod != 1.f) px = (px - firstX) * posXMod + firstX;
                    if (posYMod != 1.f) py = (py - firstY) * posYMod + firstY;
                    if (scaleXMod != 1.f) {
                        scaleX = static_cast<double>(scaleXMod) * (scaleX - static_cast<double>(firstScaleX)) +
                                 static_cast<double>(firstScaleX);
                    }
                    if (scaleYMod != 1.f) {
                        scaleY = static_cast<double>(scaleYMod) * (scaleY - static_cast<double>(firstScaleY)) +
                                 static_cast<double>(firstScaleY);
                    }
                    if (rotMod != 1.f) {
                        rot = static_cast<double>(rotMod) * (rot - static_cast<double>(firstRot)) +
                              static_cast<double>(firstRot);
                    }
                    scaleXRatio = scaleX / static_cast<double>(firstScaleX);
                    scaleYRatio = scaleY / static_cast<double>(firstScaleY);
                }
                entry.dx = px;
                entry.dy = py;
                entry.ax = px;
                entry.ay = py;
                entry.sxr = scaleXRatio;
                entry.syr = scaleYRatio;
                int const revolutions = static_cast<int>(w.prop(key, 537, 0.0));
                entry.rot = static_cast<double>(revolutions * 360) + rot;
                entry.dur = timeScale * static_cast<double>(key.duration);
                entry.easing = static_cast<int>(w.prop(key, 30, 0.0));
                {
                    float rate = static_cast<float>(w.prop(key, 85, 0.0));
                    if (rate <= 0.f) rate = 2.f;
                    entry.rate = rate;
                }
                entry.curve = w.prop(key, 378, 0.0) != 0.0;
                entry.timeMode = static_cast<int>(w.prop(key, 379, 0.0));
                bool const closeLoop = w.prop(key, 376, 0.0) != 0.0;
                if (prevEntryIdx >= 0) {
                    World::KfKeyState const& prevKeyState = keyStates[static_cast<size_t>(prevKeyIdx)];
                    if (std::fabs(prevKeyState.px - keyState.px) < 1.1920929e-07f &&
                        std::fabs(prevKeyState.py - keyState.py) < 1.1920929e-07f)
                        entries[static_cast<size_t>(prevEntryIdx)].curve = false;
                }
                bool const canBeRefOnly = i >= 1 && !(!closeLoop && i >= n - 1);
                entry.refOnly = (w.prop(key, 375, 0.0) != 0.0) && canBeRefOnly;
                if (prevEntryIdx >= 0) {
                    entry.dx = entry.dx - prevPx;
                    entry.dy = entry.dy - prevPy;
                    entry.sxr -= static_cast<double>(prevScaleXRatio);
                    entry.syr -= static_cast<double>(prevScaleYRatio);
                    entry.rot -= static_cast<double>(prevRot);
                    double const rotDelta = entry.rot - static_cast<double>(revolutions * 360);
                    int const dir = static_cast<int>(w.prop(key, 536, 0.0));
                    if (180.0 < rotDelta || (0.0 < rotDelta && dir == 2))
                        entry.rot -= 360.0;
                    else if (rotDelta < -180.0 || (rotDelta < 0.0 && dir == 1))
                        entry.rot += 360.0;
                    auto& prevEntry = entries[static_cast<size_t>(prevEntryIdx)];
                    prevEntry.spawnGroup = static_cast<int>(w.prop(key, 71, 0.0));
                    prevEntry.proximity = w.prop(key, 377, 0.0) != 0.0;
                    {
                        double spawnDelay = w.prop(key, 557, 0.0);
                        double const delay63 = w.prop(key, 63, 0.0);
                        if (static_cast<float>(delay63) != 0.f) spawnDelay = delay63;
                        prevEntry.spawnDelay = static_cast<float>(spawnDelay);
                    }
                }
                prevEntryIdx = static_cast<int>(entries.size());
                entries.push_back(entry);
                prevKeyIdx = i;
                prevPx = px;
                prevPy = py;
                prevRot = static_cast<float>(rot);
                prevScaleXRatio = static_cast<float>(scaleXRatio);
                prevScaleYRatio = static_cast<float>(scaleYRatio);
                if (looped) break;
                if (i == n - 1 && closeLoop) {
                    i = -1;
                    looped = true;
                }
            }
            {
                std::vector<size_t> run;
                size_t head = 0;
                bool haveHead = false;
                double refDuration = 0.0;
                for (size_t q = 0; q < entries.size(); ++q) {
                    if (!entries[q].refOnly) {
                        if (!run.empty()) {
                            run.push_back(q);
                            double const dur = entries[head].dur;
                            size_t const segCount = run.size() - 1;
                            if (entries[head].timeMode == 1) {
                                for (size_t j = 0; j < segCount; ++j)
                                    entries[run[j]].dur = dur / static_cast<double>(segCount);
                                refDuration = 0.0;
                            } else if (entries[head].timeMode == 2) {
                                double total = 0.0;
                                for (size_t j = 0; j < segCount; ++j) {
                                    float const dx = entries[run[j + 1]].ax - entries[run[j]].ax;
                                    float const dy = entries[run[j + 1]].ay - entries[run[j]].ay;
                                    float const dist = std::sqrt(dx * dx + dy * dy);
                                    total += static_cast<double>(dist);
                                    entries[run[j]].dur = static_cast<double>(dist);
                                }
                                for (size_t j = 0; j < segCount; ++j) {
                                    double share = dur / static_cast<double>(segCount);
                                    if (total != 0.0) share = (entries[run[j]].dur / total) * dur;
                                    entries[run[j]].dur = share;
                                }
                                refDuration = 0.0;
                            }
                            if (0 < entries[head].easing) entries[head].runEase = dur + refDuration;
                        }
                        refDuration = 0.0;
                        head = q;
                        haveHead = true;
                        run.clear();
                    } else {
                        if (run.empty() && haveHead) run.push_back(head);
                        run.push_back(q);
                        refDuration += entries[q].dur;
                    }
                }
            }
            anim.duration = 0.0;
            if (entries.size() != 1)
                for (size_t q = 0; q + 1 < entries.size(); ++q) anim.duration += entries[q].dur;
            {
                std::vector<size_t> run;
                size_t const count = entries.size();
                for (size_t q = 0; q < count; ++q) {
                    if (!entries[q].curve || count - 1 <= q) {
                        if (!run.empty()) {
                            if (run.size() < 2) {
                                for (size_t r : run) entries[r].curve = false;
                            } else {
                                run.push_back(q);
                                if (run.size() > 2) {
                                    std::vector<double> times(run.size());
                                    std::vector<double> xs(run.size());
                                    std::vector<double> ys(run.size());
                                    for (size_t j = 0; j < run.size(); ++j) {
                                        if (j == 0) {
                                            times[0] = 0;
                                            xs[0] = 0;
                                            ys[0] = 0;
                                            continue;
                                        }
                                        xs[j] = static_cast<double>(entries[run[j]].dx) + xs[j - 1];
                                        ys[j] = static_cast<double>(entries[run[j]].dy) + ys[j - 1];
                                        double segDur = entries[run[j - 1]].dur;
                                        if (segDur < 1e-05) segDur = 1e-05;
                                        times[j] = segDur + times[j - 1];
                                    }
                                    World::KfSpline splineX;
                                    World::KfSpline splineY;
                                    kfSplineSet(splineX, times, xs);
                                    kfSplineSet(splineY, times, ys);
                                    auto& runHead = entries[run[0]];
                                    runHead.splineX = static_cast<int>(splines.size());
                                    splines.push_back(std::move(splineX));
                                    runHead.splineY = static_cast<int>(splines.size());
                                    splines.push_back(std::move(splineY));
                                    runHead.runDur = times.back();
                                }
                            }
                            run.clear();
                        }
                    } else
                        run.push_back(q);
                }
            }
        }

    }

    // finds settings menus made of pickups and the option to pick
    void analyzeMenus(World& w) {
        w.menus.clear();
        w.ldmTouch.clear();
        if (w.texts.empty()) return;
        auto lower = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        auto labelOf = [&](Obj const& t) -> std::string const* {
            std::string const* best = nullptr;
            float bestDist = 1e30f;
            for (auto const& [ti, txt] : w.texts) {
                Obj const& o = w.objs[static_cast<size_t>(ti)];
                float const dx = o.x - t.x;
                float const dy = o.y - t.y;
                if (std::fabs(dx) > 75.f || dy < -60.f || dy > 120.f) continue;
                float const dist = dx * dx + dy * dy;
                if (dist < bestDist) {
                    bestDist = dist;
                    best = &txt;
                }
            }
            return best;
        };
        auto numberOf = [&](std::string const& label) -> int {
            std::string digits;
            for (char c : lower(label))
                if (c != ' ') digits += c;
            for (char const* suffix : {"hz", "fps"}) {
                size_t const len = std::strlen(suffix);
                if (digits.size() > len && digits.compare(digits.size() - len, len, suffix) == 0)
                    digits.resize(digits.size() - len);
            }
            if (digits.empty() || digits.size() > 4) return -1;
            for (char c : digits)
                if (c < '0' || c > '9') return -1;
            int const value = std::atoi(digits.c_str());
            return (value >= 20 && value <= 1000) ? value : -1;
        };
        bool refreshText = false;
        for (auto const& e : w.texts) {
            std::string const text = lower(e.second);
            if (text.find("refresh") != std::string::npos || text.find("fps") != std::string::npos ||
                text.find(" hz") != std::string::npos)
                refreshText = true;
        }
        std::map<int, World::MenuItem> byItem;
        auto isSetter = [](Obj const& o) {
            return o.cls == 1 && o.id == ObjId::PickupTrigger && o.tf(Obj::TF_CountOverride) && o.compare == 0;
        };
        auto addOption = [&](size_t touchIdx, Obj const& setter) {
            std::string const* label = labelOf(w.objs[touchIdx]);
            if (!label) return;
            World::MenuItem& menu = byItem[setter.itemID];
            menu.item = setter.itemID;
            for (auto const& option : menu.options)
                if (option.idx == static_cast<int>(touchIdx) && option.value == setter.count) return;
            menu.options.push_back(World::MenuOption{static_cast<int>(touchIdx), setter.count, *label});
        };
        for (size_t i = 0; i < w.objs.size(); ++i) {
            Obj const& o = w.objs[i];
            if (isSetter(o) && o.touchTriggered) {
                addOption(i, o);
                continue;
            }
            bool const collect = o.type == Collectible && o.tf(Obj::TF_CollectToggle) && o.tf(Obj::TF_ActivateGroup);
            bool const touchSpawn = o.cls == 1 && o.id == ObjId::SpawnTrigger && o.touchTriggered;
            if ((collect || touchSpawn) && o.targetGroup > 0) {
                for (int t : w.allOfGroup(o.targetGroup)) {
                    Obj const& spawned = w.objs[static_cast<size_t>(t)];
                    if (isSetter(spawned) && spawned.spawnTriggered) addOption(i, spawned);
                }
            }
        }
        for (auto& [item, menu] : byItem) {
            int numberCount = 0;
            int bestHz = -1;
            int bestValue = 0;
            for (auto const& option : menu.options) {
                std::string const label = lower(option.label);
                if (label.find("ldm") != std::string::npos || label.find("low detail") != std::string::npos ||
                    label.find("lowdetail") != std::string::npos || label.find("low-detail") != std::string::npos) {
                    menu.kind = "ldm";
                    if (std::find(menu.forbid.begin(), menu.forbid.end(), option.value) == menu.forbid.end())
                        menu.forbid.push_back(option.value);
                    continue;
                }
                int const hz = numberOf(option.label);
                if (hz < 0) continue;
                ++numberCount;
                if (bestHz < 0 || std::abs(hz - 240) < std::abs(bestHz - 240) ||
                    (std::abs(hz - 240) == std::abs(bestHz - 240) && hz > bestHz)) {
                    bestHz = hz;
                    bestValue = option.value;
                }
            }
            if (numberCount >= 2 && (refreshText || numberCount >= 3) && menu.kind.empty()) {
                menu.kind = "refresh rate";
                menu.hasPrefer = true;
                menu.prefer = bestValue;
                for (auto const& option : menu.options) {
                    if (numberOf(option.label) >= 0 && option.value != bestValue &&
                        std::find(menu.forbid.begin(), menu.forbid.end(), option.value) == menu.forbid.end())
                        menu.forbid.push_back(option.value);
                }
            }
            if (!menu.kind.empty()) w.menus.push_back(std::move(menu));
        }
        auto isLdm = [&](std::string const& t) {
            std::string const text = lower(t);
            return text.find("ldm") != std::string::npos || text.find("low detail") != std::string::npos ||
                   text.find("lowdetail") != std::string::npos || text.find("low-detail") != std::string::npos;
        };
        bool anyLdm = false;
        for (auto const& e : w.texts) {
            if (isLdm(e.second)) {
                anyLdm = true;
                break;
            }
        }
        if (anyLdm) {
            auto isRing = [](Obj const& o) {
                return o.type == YellowJumpRing || o.type == PinkJumpRing || o.type == GravityRing ||
                       o.type == GreenRing || o.type == DropRing || o.type == RedJumpRing || o.type == CustomRing ||
                       o.type == DashRing || o.type == GravityDashRing || o.type == SpiderOrb || o.type == TeleportOrb;
            };
            auto inRange = [](Obj const& t, Obj const& o) {
                float const dx = t.x - o.x;
                float const dy = t.y - o.y;
                return !(std::fabs(dx) > 75.f || dy < -60.f || dy > 120.f);
            };
            auto dist2 = [](Obj const& t, Obj const& o) {
                float const dx = t.x - o.x;
                float const dy = t.y - o.y;
                return dx * dx + dy * dy;
            };
            std::vector<char> mark(w.objs.size(), 0);
            for (auto const& [ti, txt] : w.texts) {
                if (!isLdm(txt)) continue;
                Obj const& t = w.objs[static_cast<size_t>(ti)];
                int bestRing = -1;
                int bestTrig = -1;
                float ringDist = 1e30f;
                float trigDist = 1e30f;
                for (size_t i = 0; i < w.objs.size(); ++i) {
                    Obj const& o = w.objs[i];
                    bool const ring = isRing(o) || o.type == Collectible;
                    bool const touchTrig = o.cls == 1 && o.touchTriggered;
                    if (!(ring || touchTrig) || !inRange(t, o)) continue;
                    float const dist = dist2(t, o);
                    if (ring) {
                        if (dist < ringDist) {
                            ringDist = dist;
                            bestRing = static_cast<int>(i);
                        }
                    } else if (dist < trigDist) {
                        trigDist = dist;
                        bestTrig = static_cast<int>(i);
                    }
                }
                if (bestRing >= 0)
                    mark[static_cast<size_t>(bestRing)] = 1;
                else if (bestTrig >= 0)
                    mark[static_cast<size_t>(bestTrig)] = 1;
            }
            for (size_t i = 0; i < w.objs.size(); ++i)
                if (mark[i]) w.ldmTouch.push_back(static_cast<int>(i));
        }
    }

    // level analysis on load: fills the World tables the trigger simulation needs
    namespace {
        // The level analysis done on load, one pass after another; the tables a pass builds for the later ones are
        // members.
        struct TriggerAnalysis {
            TriggerAnalysis(World& w) : w(w) {}

            // arguments of analyzeTriggers
            World& w;

            std::vector<std::pair<int, int>> channelDir;
            std::vector<std::pair<int, int>> remapEdges;
            template <class T1>
            auto forRemapped(int gr, T1&& f);
            std::vector<char> strongCenter;
            std::unordered_map<int, std::vector<int>> byKfGroup;
            std::vector<std::pair<int, int>> kfUses;
            std::vector<uint8_t> kfCandidate;
            bool anyKfCandidate;
            std::vector<int> dynItems;
            bool testedItem(int item);
            // objects with the same list of groups share one signature (groupSig)
            void signGroupLists();
            // objects of each group (groupObjs, groupAll) and the parents of the groups
            void indexGroups();
            // sorts the members of the groups that spawn triggers target by start x, as the game does
            void sortGroupMembers();
            // the triggers of each spawn order list (spawnLists), in the order the game fires them
            void listSpawnOrder();
            // groups that move or toggle, where they first move, and which objects can move (movable, toggleable,
            // lockX)
            void markMovableGroups();
            // objects used as centers of rotations and scales (centerLike): they are not gameplay
            void findCenters();
            // follow player Y triggers with a delay (fyDelayX), the persistent items saved by the level, then the menus
            void listDelaysItemsMenus();
            // keyframe and teleport targets count as centers too
            void findKeyframeCenters();
            // for each target group, its representative objects and their children (targetGroups)
            void buildTargetGroups();
            // the objects that rotate triggers aim at or snap to count as centers
            void markRotateTargets();
            // objects the advanced follow triggers must keep (afKeep)
            void markAdvancedFollowKeep();
            // for each group, the objects that move it (groupMovers), and the objects that are only decoration
            void findGroupMovers();
            // keyframe animations and their splines (kfAnims, kfSplines)
            void buildKeyframes();
            // objects that can get their own position, rotation or scale (mayHaveDyn, scaleTarget), and whether the
            // player can be moved (pPossible)
            void markDynamicObjects();
            // definitions of the area triggers and of the objects they target (areaDefs)
            void buildAreas();
            // definitions of the advanced follow triggers (advDefs) and their rings of saved positions (afRings)
            void buildAdvancedFollows();
            // keyframe animations whose keys can move, and the groups whose keyframes can take the fast path (kfOpt)
            void markOptimizedKeyframes();
            // one slot per spawnable trigger with an effect, for its activated bit (trigSlot)
            void assignTriggerSlots();
            // collision blocks, their pairs and cells, and which of them move (blockMoving)
            void buildCollisionBlocks();
            // whether teleports have several targets (tpMulti) and whether the level has random triggers (hasRandom)
            void findRandomness();
            // game events that the event triggers listen to (eventUsed)
            void listEvents();
            // triggers the simulator does not support or only approximates
            void listUnsupported();
            // groups whose moves can take the fast path (groupFast)
            void markFastGroups();

            // the whole of analyzeTriggers, one stage after another
            void run();
        };
    }

    template <class T1>
    auto TriggerAnalysis::forRemapped(int gr, T1&& f) {
        f(gr);
        if (remapEdges.empty() || gr <= 0) return;
        std::vector<int> seen{gr};
        for (size_t i = 0; i < seen.size(); ++i) {
            for (auto const& e : remapEdges) {
                if (e.first == seen[i] && std::find(seen.begin(), seen.end(), e.second) == seen.end()) {
                    seen.push_back(e.second);
                    f(e.second);
                }
            }
        }
    }

    auto TriggerAnalysis::testedItem(int item) -> bool {
        if (w.collisionPairListened(10000, item) || w.collisionPairListened(10001, item)) return true;
        for (int d : dynItems)
            if (w.collisionPairListened(d, item)) return true;
        return false;
    }

    // objects with the same list of groups share one signature (groupSig)
    auto TriggerAnalysis::signGroupLists() -> void {
        {
            std::map<std::vector<short>, int> sigIds;
            for (auto& o : w.objs) {
                auto it = sigIds.emplace(o.groups, static_cast<int>(sigIds.size()));
                o.groupSig = it.first->second;
            }
        }
    }

    // objects of each group (groupObjs, groupAll) and the parents of the groups
    auto TriggerAnalysis::indexGroups() -> void {
        w.maxGroup = 0;
        for (auto const& o : w.objs)
            for (short gr : o.groups) w.maxGroup = std::max(w.maxGroup, static_cast<int>(gr));
        w.groupObjs.assign(static_cast<size_t>(w.maxGroup) + 1, {});
        w.groupAll.assign(static_cast<size_t>(w.maxGroup) + 1, {});
        if (w.groupParent.size() != static_cast<size_t>(w.maxGroup) + 1) {
            std::vector<int> parents(static_cast<size_t>(w.maxGroup) + 1, -1);
            for (size_t i = 0; i < w.groupParent.size() && i < parents.size(); ++i) parents[i] = w.groupParent[i];
            w.groupParent.swap(parents);
        }
        for (size_t i = 0; i < w.objs.size(); ++i) {
            auto const& o = w.objs[i];
            for (short gr : o.groups) {
                if (gr < 0) continue;
                w.groupAll[static_cast<size_t>(gr)].push_back(static_cast<int>(i));
                if (o.type != Decoration) w.groupObjs[static_cast<size_t>(gr)].push_back(static_cast<int>(i));
            }
        }
    }

    // sorts the members of the groups that spawn triggers target by start x, as the game does
    auto TriggerAnalysis::sortGroupMembers() -> void {
        {
            std::vector<char> sortGroup(w.groupAll.size(), w.sortGroups ? 1 : 0);
            if (!w.sortGroups) {
                for (auto const& o : w.objs) {
                    if (o.cls == 1 && o.id == ObjId::SpawnTrigger) {
                        int gr = std::clamp(o.targetGroup, 0, 9999);
                        if (static_cast<size_t>(gr) < sortGroup.size()) sortGroup[static_cast<size_t>(gr)] = 1;
                    }
                }
            }
            for (size_t gr = 0; gr < w.groupAll.size(); ++gr) {
                auto& members = w.groupAll[gr];
                if (!sortGroup[gr] || members.size() < 2) continue;
                crtQsort(members, [&](int a, int b) {
                    return static_cast<int>(w.objs[static_cast<size_t>(a)].startX) -
                           static_cast<int>(w.objs[static_cast<size_t>(b)].startX);
                });
            }
        }
    }

    // the triggers of each spawn order list (spawnLists), in the order the game fires them
    auto TriggerAnalysis::listSpawnOrder() -> void {
        w.spawnLists.clear();
        for (size_t i = 0; i < w.objs.size(); ++i) {
            auto const& o = w.objs[i];
            if (!(o.trigger && o.cls == 1 && !o.touchTriggered && !o.spawnTriggered)) continue;
            std::vector<int>* list = nullptr;
            for (auto& e : w.spawnLists)
                if (e.first == o.channel) list = &e.second;
            if (!list) {
                w.spawnLists.emplace_back(o.channel, std::vector<int>{});
                list = &w.spawnLists.back().second;
            }
            list->push_back(static_cast<int>(i));
        }
        for (auto const& e : w.spawnLists)
            for (int idx : e.second) {
                Obj const& o = w.objs[static_cast<size_t>(idx)];
                if (o.id != ObjId::RotateGameplayTrigger || !(o.rgFlags & 4)) continue;
                bool seen = false;
                for (auto const& c : channelDir)
                    if (c.first == o.rgChannel) seen = true;
                if (seen) continue;
                static int const kTab[2][2][4] = {{{0, 4, 3, 5}, {1, 7, 2, 6}}, {{2, 6, 1, 7}, {3, 5, 0, 4}}};
                int rot = static_cast<int>(o.rot) % 360;
                if (rot < 0) rot += 360;
                int const objDir = (rot % 90 == 0) ? kTab[o.flipX ? 1 : 0][o.flipY ? 1 : 0][rot / 90] : 0;
                int const dir = (objDir == 2 || objDir == 3)   ? 3
                                : (objDir == 4 || objDir == 7) ? 2
                                : (objDir == 5 || objDir == 6) ? 1
                                                               : 4;
                channelDir.emplace_back(o.rgChannel, dir);
            }
        for (auto& e : w.spawnLists) {
            auto& list = e.second;
            int dir = 4;
            for (auto const& c : channelDir)
                if (c.first == e.first) dir = c.second;
            auto sortKey = [&](int idx, int& ord, int& posKey) {
                Obj const& o = w.objs[static_cast<size_t>(idx)];
                ord = o.ord;
                float const f = static_cast<float>(o.ord);
                posKey = dir == 1   ? static_cast<int>(f + o.startY)
                         : dir == 2 ? static_cast<int>(f - o.startY)
                         : dir == 3 ? static_cast<int>(f - o.startX)
                                    : static_cast<int>(f + o.startX);
            };
            std::sort(list.begin(), list.end(), [&](int a, int b) {
                int ordA;
                int posA;
                int ordB;
                int posB;
                sortKey(a, ordA, posA);
                sortKey(b, ordB, posB);
                if (ordA != ordB) return ordA < ordB;
                if (posA != posB) return posA < posB;
                return w.objs[static_cast<size_t>(a)].uid < w.objs[static_cast<size_t>(b)].uid;
            });
        }
    }

    // groups that move or toggle, where they first move, and which objects can move (movable, toggleable,
    // lockX)
    auto TriggerAnalysis::markMovableGroups() -> void {
        for (auto const& t : w.spawnRemaps)
            for (auto const& e : t)
                if (e.first != e.second) remapEdges.push_back(e);
        {
            std::vector<char> movedGroups(static_cast<size_t>(w.maxGroup) + 1, 0);
            std::vector<char> toggledGroups(static_cast<size_t>(w.maxGroup) + 1, 0);
            auto mark = [&](std::vector<char>& flags, int gr0) {
                forRemapped(gr0, [&](int gr) {
                    if (gr >= 0 && gr <= w.maxGroup) flags[static_cast<size_t>(gr)] = 1;
                });
            };
            for (auto const& o : w.objs) {
                if (o.cls != 1) continue;
                switch (o.id) {
                case 901:
                case 1346:
                case 1347:
                case 1814:
                case 2067:
                    mark(movedGroups, o.targetGroup);
                    break;
                case 1049:
                case 1611:
                case 1811:
                case 1815:
                case 1595:
                case 3609:
                case 3640:
                case 3643:
                    mark(toggledGroups, o.targetGroup);
                    break;
                case 1594:
                    if (!o.ringSpawnOnly) mark(toggledGroups, o.targetGroup);
                    break;
                default:
                    break;
                }
                if (o.type == Collectible || o.type == UserCoin) {
                    if (o.tf(Obj::TF_CollectToggle)) mark(toggledGroups, o.targetGroup);
                }
            }
            w.groupLastMoveX.assign(static_cast<size_t>(w.maxGroup) + 1, -1e30f);
            std::vector<float> activationX(static_cast<size_t>(w.maxGroup) + 1, -1e30f);
            {
                constexpr float kInf = 1e30f;
                constexpr float kMaxSpeed = 3200.f;
                std::vector<std::vector<int>> spawners(static_cast<size_t>(w.maxGroup) + 1);
                auto markInf = [&](int gr) {
                    if (gr > 0 && gr <= w.maxGroup) activationX[static_cast<size_t>(gr)] = kInf;
                };
                for (size_t i = 0; i < w.objs.size(); ++i) {
                    Obj const& o = w.objs[i];
                    if (o.cls != 1) {
                        if (o.isRing() || o.type == Collectible || o.type == UserCoin) markInf(o.targetGroup);
                        continue;
                    }
                    switch (o.id) {
                    case 1268:
                        if (o.targetGroup > 0 && o.targetGroup <= w.maxGroup)
                            spawners[static_cast<size_t>(o.targetGroup)].push_back(static_cast<int>(i));
                        break;
                    case 901:
                    case 1346:
                    case 1347:
                    case 1814:
                    case 2067:
                    case 3016:
                    case 1049:
                    case 1616:
                    case 1006:
                    case 1007:
                    case 899:
                    case 1520:
                    case 1913:
                    case 1916:
                    case 1917:
                    case 2066:
                    case 2900:
                    case 2925:
                    case 1935:
                    case 2015:
                    case 2062:
                    case 2899:
                        break;
                    case 3033:
                        break;
                    case 3032:
                        markInf(static_cast<int>(w.prop(o, 71, 0.0)));
                        break;
                    default:
                        markInf(o.targetGroup);
                        markInf(o.centerGroup);
                        break;
                    }
                }
                if (w.spawnGroup > 0) markInf(w.spawnGroup);
                for (auto const& e : w.spawnRemaps)
                    for (auto const& pr : e) markInf(pr.second);
                std::vector<char> done(activationX.size(), 0);
                for (size_t gr = 0; gr < activationX.size(); ++gr)
                    if (activationX[gr] >= kInf) done[gr] = 1;
                for (int pass = 0; pass < 24; ++pass) {
                    bool changed = false;
                    for (size_t gr = 1; gr < activationX.size(); ++gr) {
                        if (done[gr]) continue;
                        float x = -1e30f;
                        bool ok = true;
                        for (int spawnerIdx : spawners[gr]) {
                            Obj const& spawner = w.objs[static_cast<size_t>(spawnerIdx)];
                            float const delayDist =
                                    (spawner.spawnDelay + std::fabs(spawner.spawnDelayRange)) * kMaxSpeed;
                            if (spawner.touchTriggered) {
                                x = kInf;
                                break;
                            }
                            if (!spawner.spawnTriggered) {
                                x = std::max(x, spawner.x + delayDist);
                                continue;
                            }
                            float groupX = -1e30f;
                            for (short g2 : spawner.groups) {
                                if (g2 <= 0 || g2 > w.maxGroup) continue;
                                if (!done[static_cast<size_t>(g2)]) {
                                    ok = false;
                                    break;
                                }
                                groupX = std::max(groupX, activationX[static_cast<size_t>(g2)]);
                            }
                            if (!ok) break;
                            x = std::max(x, groupX >= kInf ? kInf : groupX + delayDist);
                        }
                        if (ok) {
                            activationX[gr] = x;
                            done[gr] = 1;
                            changed = true;
                        }
                    }
                    if (!changed) break;
                }
                for (size_t gr = 0; gr < activationX.size(); ++gr)
                    if (!done[gr]) activationX[gr] = kInf;
            }
            auto spawnedX = [&](Obj const& o) {
                float maxX = -1e30f;
                for (short g2 : o.groups) {
                    if (g2 > 0 && g2 <= w.maxGroup) maxX = std::max(maxX, activationX[static_cast<size_t>(g2)]);
                }
                return maxX;
            };
            std::vector<std::vector<std::pair<float, float>>> groupMoves(static_cast<size_t>(w.maxGroup) + 1);
            std::vector<std::vector<std::pair<float, int>>> groupRots(static_cast<size_t>(w.maxGroup) + 1);
            for (auto const& o : w.objs) {
                if (o.cls != 1) continue;
                if (o.id == ObjId::MoveTrigger && !o.spawnTriggered && !o.touchTriggered &&
                    !o.tf(Obj::TF_UseMoveTarget) && !o.tf(Obj::TF_DirSnap) && !o.tf(Obj::TF_LockPX) &&
                    !o.tf(Obj::TF_LockPY) && !o.tf(Obj::TF_LockCX) && !o.tf(Obj::TF_LockCY)) {
                    forRemapped(o.targetGroup, [&](int gr) {
                        if (gr >= 0 && gr <= w.maxGroup)
                            groupMoves[static_cast<size_t>(gr)].emplace_back(o.x, std::max(0.f, o.mvX));
                    });
                    continue;
                }
                if (o.id == ObjId::RotateTrigger && !o.spawnTriggered && !o.touchTriggered) {
                    forRemapped(o.targetGroup, [&](int gr) {
                        if (gr >= 0 && gr <= w.maxGroup)
                            groupRots[static_cast<size_t>(gr)].emplace_back(o.x, o.centerGroup);
                    });
                    continue;
                }
                switch (o.id) {
                case 901:
                case 1346:
                case 1347:
                case 1814:
                case 2067:
                case 3016:
                case 3033: {
                    if (o.id == ObjId::RotateTrigger && o.centerGroup <= 0) break;
                    float const lastX = o.touchTriggered ? 1e30f : (o.spawnTriggered ? spawnedX(o) : o.x);
                    forRemapped(o.targetGroup, [&](int gr) {
                        if (gr >= 0 && gr <= w.maxGroup) {
                            float& moveX = w.groupLastMoveX[static_cast<size_t>(gr)];
                            moveX = std::max(moveX, lastX);
                        }
                    });
                    break;
                }
                default:
                    break;
                }
            }
            w.gfFirst.assign(static_cast<size_t>(w.maxGroup) + 2, 0);
            w.gfX.clear();
            w.gfSum.clear();
            for (size_t gr = 0; gr < groupMoves.size(); ++gr) {
                auto& moves = groupMoves[gr];
                std::sort(moves.begin(), moves.end());
                w.gfFirst[gr] = static_cast<int>(w.gfX.size());
                size_t const base = w.gfX.size();
                for (auto const& e : moves) {
                    w.gfX.push_back(e.first);
                    w.gfSum.push_back(e.second);
                }
                for (size_t i = w.gfX.size(); i-- > base + 1;) w.gfSum[i - 1] += w.gfSum[i];
            }
            w.gfFirst[groupMoves.size()] = static_cast<int>(w.gfX.size());
            w.grFirst.assign(static_cast<size_t>(w.maxGroup) + 2, 0);
            w.grX.clear();
            w.grCenter.clear();
            for (size_t gr = 0; gr < groupRots.size(); ++gr) {
                auto& rots = groupRots[gr];
                std::sort(rots.begin(), rots.end());
                w.grFirst[gr] = static_cast<int>(w.grX.size());
                for (auto const& e : rots) {
                    w.grX.push_back(e.first);
                    w.grCenter.push_back(e.second);
                }
            }
            w.grFirst[groupRots.size()] = static_cast<int>(w.grX.size());
            for (auto& o : w.objs) {
                o.movable = false;
                o.toggleable = false;
                o.lockX = !w.platformer && shouldLockX(o.id);
                o.groupBloom = 0;
                for (short gr : o.groups) {
                    if (gr >= 0) o.groupBloom |= 1ull << (gr & 63);
                    if (gr >= 0 && gr <= w.maxGroup) {
                        if (movedGroups[static_cast<size_t>(gr)]) o.movable = true;
                        if (toggledGroups[static_cast<size_t>(gr)]) o.toggleable = true;
                    }
                }
            }
            w.lite.resize(w.objs.size());
            for (size_t i = 0; i < w.objs.size(); ++i) {
                Obj const& o = w.objs[i];
                w.lite[i] = World::ObjLite{o.posX,
                                           o.posY,
                                           o.groupSig,
                                           o.groups.empty() ? short(-1) : o.groups[0],
                                           static_cast<uint8_t>(o.lockX ? 1 : 0),
                                           static_cast<uint8_t>(std::min<size_t>(o.groups.size(), 255))};
            }
        }
    }

    // objects used as centers of rotations and scales (centerLike): they are not gameplay
    auto TriggerAnalysis::findCenters() -> void {
        w.centerLike.assign(w.objs.size(), 0);
        strongCenter = std::vector<char>(w.objs.size(), 0);
        for (int gr = 1; gr <= w.maxGroup; ++gr) {
            int const mainObj = w.mainObjectOf(gr);
            if (mainObj >= 0) w.centerLike[static_cast<size_t>(mainObj)] = 1;
            int const parent = w.exportParentOf(gr);
            if (parent >= 0 && static_cast<size_t>(parent) < w.centerLike.size())
                w.centerLike[static_cast<size_t>(parent)] = 1;
        }
    }

    // follow player Y triggers with a delay (fyDelayX), the persistent items saved by the level, then the menus
    auto TriggerAnalysis::listDelaysItemsMenus() -> void {
        w.fyDelayX.clear();
        w.fyDelayAlways = false;
        for (auto const& o : w.objs) {
            if (o.cls != 1 || o.id != ObjId::FollowPlayerYTrigger || !(o.fyDelay > 0.f)) continue;
            if (o.spawnTriggered || o.touchTriggered)
                w.fyDelayAlways = true;
            else
                w.fyDelayX.push_back(static_cast<float>(o.posX));
        }
        std::sort(w.fyDelayX.begin(), w.fyDelayX.end());
        w.persistItemIds.clear();
        for (auto const& o : w.objs) {
            if (o.cls != 1 || o.id != ObjId::PersistentItemTrigger || o.ext < 0) continue;
            if (w.prop(o, 491, 0.0) == 0.0 || w.prop(o, 494, 0.0) != 0.0) continue;
            int const itemId = std::clamp(o.itemID, 0, 9999);
            if (std::find(w.persistItemIds.begin(), w.persistItemIds.end(), itemId) == w.persistItemIds.end())
                w.persistItemIds.push_back(itemId);
        }
        analyzeMenus(w);
    }

    // keyframe and teleport targets count as centers too
    auto TriggerAnalysis::findKeyframeCenters() -> void {
        {
            for (size_t i = 0; i < w.objs.size(); ++i) {
                if (w.objs[i].cls == 1 && w.objs[i].id == ObjId::KeyframeObject && w.objs[i].ext >= 0)
                    byKfGroup[static_cast<int>(w.prop(w.objs[i], 373, 0.0))].push_back(static_cast<int>(i));
            }
            if (!byKfGroup.empty()) {
                for (auto const& t : w.objs) {
                    if (t.cls != 1 || t.id != ObjId::KeyframeAnimTrigger || t.ext < 0) continue;
                    int const animGroup = static_cast<int>(w.prop(t, 76, 0.0));
                    if (animGroup <= 0) continue;
                    forRemapped(animGroup, [&](int remapped) {
                        if (remapped <= 0) return;
                        for (int m : w.allOfGroup(remapped)) {
                            Obj const& key = w.objs[static_cast<size_t>(m)];
                            if (key.id != ObjId::KeyframeObject || key.ext < 0) continue;
                            auto it = byKfGroup.find(static_cast<int>(w.prop(key, 373, 0.0)));
                            if (it == byKfGroup.end()) continue;
                            for (int q : it->second) w.centerLike[static_cast<size_t>(q)] = 1;
                            kfUses.push_back({static_cast<int>(&t - w.objs.data()), it->first});
                        }
                    });
                }
            }
        }
        for (auto const& o : w.objs) {
            bool const isTeleport = o.type == TeleportPortal || o.type == TeleportOrb ||
                                    (o.cls == 1 && (o.id == ObjId::TeleportTrigger || o.id == ObjId::TeleportArea));
            if (!isTeleport || o.targetGroup <= 0) continue;
            for (int idx : w.allOfGroup(o.targetGroup)) {
                w.centerLike[static_cast<size_t>(idx)] = 1;
                strongCenter[static_cast<size_t>(idx)] |= 2;
            }
        }
    }

    // for each target group, its representative objects and their children (targetGroups)
    auto TriggerAnalysis::buildTargetGroups() -> void {
        w.targetGroups.clear();
        w.targetGroupIdx.clear();
        w.targetGroupDense.clear();
        w.areaRef.assign(w.objs.size(), 0);
        {
            std::vector<std::vector<int>> parentOfGroups(w.objs.size());
            for (int gr = 1; gr <= w.maxGroup; ++gr) {
                int const parent = w.exportParentOf(gr);
                if (parent >= 0) parentOfGroups[static_cast<size_t>(parent)].push_back(gr);
            }
            std::unordered_map<int, std::vector<int>> linked;
            for (size_t i = 0; i < w.objs.size(); ++i) {
                if (w.objs[i].linkedGroup > 0) linked[w.objs[i].linkedGroup].push_back(static_cast<int>(i));
            }
            auto hasGP = [&](int i) { return w.objs[static_cast<size_t>(i)].tf(Obj::TF_GroupParent); };
            auto hasAP = [&](int i) { return w.objs[static_cast<size_t>(i)].tf(Obj::TF_AreaParent); };
            auto build = [&](int group, bool ignoreParent, bool ignoreLinked) {
                long long const key = World::tgKey(group, ignoreParent, ignoreLinked);
                if (group <= 0 || w.targetGroupIdx.count(key)) return;
                auto const& members = w.allOfGroup(group);
                std::unordered_map<int, int> familyOf;
                for (int i : members) familyOf[i] = w.objs[static_cast<size_t>(i)].linkedGroup;
                std::map<int, int> repOf;
                std::map<int, int> centerOf;
                int const parent = w.exportParentOf(group);
                auto offer = [&](int famKey, int child, bool isOwner) {
                    auto r = repOf.find(famKey);
                    if (r == repOf.end() || ((!hasAP(r->second) || isOwner) && hasAP(child))) repOf[famKey] = child;
                    auto c = centerOf.find(famKey);
                    if (c == centerOf.end() || ((!hasGP(c->second) || isOwner) && hasGP(child)))
                        centerOf[famKey] = child;
                };
                if (!ignoreParent)
                    for (int member : members) {
                        for (int g2 : parentOfGroups[static_cast<size_t>(member)]) {
                            if (!(parent < 0 || (member == parent && g2 == group))) continue;
                            int const famKey = -g2;
                            for (int child : w.allOfGroup(g2)) {
                                familyOf.erase(child);
                                if (hasGP(child) || hasAP(child) || child == member)
                                    offer(famKey, child, child == member);
                            }
                            auto r = repOf.find(famKey);
                            if (r != repOf.end()) familyOf[r->second] = famKey;
                        }
                    }
                if (!ignoreLinked)
                    for (int child : members) {
                        auto it = familyOf.find(child);
                        if (it == familyOf.end() || it->second <= 0 || !(hasGP(child) || hasAP(child))) continue;
                        int const famKey = it->second;
                        auto r = repOf.find(famKey);
                        if (r == repOf.end() || (!hasAP(r->second) && hasAP(child))) repOf[famKey] = child;
                        auto c = centerOf.find(famKey);
                        if (c == centerOf.end() || (!hasGP(c->second) && hasGP(child))) centerOf[famKey] = child;
                    }
                if (repOf.empty()) return;
                World::TargetGroups targets;
                for (int o : members) {
                    auto it = familyOf.find(o);
                    if (it == familyOf.end()) continue;
                    int const famKey = it->second;
                    if (famKey == 0) {
                        targets.loose.push_back(o);
                        continue;
                    }
                    auto r = repOf.find(famKey);
                    if (r == repOf.end()) {
                        targets.loose.push_back(o);
                        continue;
                    }
                    if (r->second != o) continue;
                    World::TargetGroups::Rep rep;
                    rep.obj = o;
                    auto c = centerOf.find(famKey);
                    rep.center = c != centerOf.end() ? c->second : -1;
                    if (famKey < 0)
                        rep.children = w.allOfGroup(-famKey);
                    else if (auto l = linked.find(famKey); l != linked.end())
                        rep.children = l->second;
                    targets.reps.push_back(std::move(rep));
                }
                targets.looseSorted = targets.loose;
                std::sort(targets.looseSorted.begin(), targets.looseSorted.end());
                for (auto const& rep : targets.reps) {
                    w.centerLike[static_cast<size_t>(rep.obj)] = 1;
                    if (rep.center >= 0) w.centerLike[static_cast<size_t>(rep.center)] = 1;
                    bool hasPhysical = false;
                    for (int c : rep.children) {
                        if (w.objs[static_cast<size_t>(c)].type != Decoration) {
                            hasPhysical = true;
                            break;
                        }
                    }
                    if (hasPhysical) {
                        w.areaRef[static_cast<size_t>(rep.obj)] = 1;
                        if (rep.center >= 0) w.areaRef[static_cast<size_t>(rep.center)] = 1;
                    }
                }
                w.targetGroupIdx[key] = static_cast<int>(w.targetGroups.size());
                w.targetGroups.push_back(std::move(targets));
            };
            for (auto const& o : w.objs) {
                if (o.cls != 1 || !((o.id >= 3006 && o.id <= 3008) || o.id == ObjId::AdvancedFollowTrigger)) continue;
                auto const* ex = w.extOf(o);
                if (!ex) continue;
                int target = static_cast<int>(ex->num(51, o.targetGroup));
                if (int const areaTarget = static_cast<int>(ex->num(226, 0));
                    o.id != ObjId::AdvancedFollowTrigger && areaTarget != 0) {
                    target = areaTarget;
                }
                bool const ignoreParent = ex->num(280, 0) != 0;
                bool const ignoreLinked = ex->num(281, 0) != 0;
                forRemapped(target, [&](int gr) { build(gr, ignoreParent, ignoreLinked); });
            }
            long long maxKey = -1;
            for (auto const& e : w.targetGroupIdx) maxKey = std::max(maxKey, e.first);
            w.targetGroupDense.assign(static_cast<size_t>(maxKey + 1), -1);
            for (auto const& e : w.targetGroupIdx) {
                if (e.first >= 0) w.targetGroupDense[static_cast<size_t>(e.first)] = e.second;
            }
        }
    }

    // the objects that rotate triggers aim at or snap to count as centers
    auto TriggerAnalysis::markRotateTargets() -> void {
        for (auto const& o : w.objs) {
            if (o.cls != 1 || o.id != ObjId::RotateTrigger || !(o.tf(Obj::TF_UseMoveTarget) || o.tf(Obj::TF_DirSnap)))
                continue;
            for (int gr0 : {o.centerGroup, o.targetGroup, o.rotTarget, o.rotMinX, o.rotMinY, o.rotMaxX, o.rotMaxY}) {
                forRemapped(gr0, [&](int gr) {
                    if (gr > 0)
                        for (int idx : w.allOfGroup(gr)) w.centerLike[static_cast<size_t>(idx)] = 1;
                });
            }
        }
    }

    // objects the advanced follow triggers must keep (afKeep)
    auto TriggerAnalysis::markAdvancedFollowKeep() -> void {
        w.afKeep.assign(w.objs.size(), 0);
        {
            constexpr size_t kAfKeepMax = 40;
            auto keepGroup = [&](int gr0) {
                forRemapped(gr0, [&](int gr) {
                    if (gr <= 0 || gr > w.maxGroup) return;
                    size_t decoCount = 0;
                    for (int idx : w.allOfGroup(gr)) {
                        if (w.objs[static_cast<size_t>(idx)].type == Decoration &&
                            !w.centerLike[static_cast<size_t>(idx)])
                            ++decoCount;
                    }
                    if (decoCount > kAfKeepMax) return;
                    for (int idx : w.allOfGroup(gr)) {
                        w.afKeep[static_cast<size_t>(idx)] = 1;
                        w.centerLike[static_cast<size_t>(idx)] = 1;
                        strongCenter[static_cast<size_t>(idx)] |= 16;
                    }
                });
            };
            std::vector<int> controlEdits;
            for (auto const& o : w.objs) {
                if (o.cls != 1 || o.id != ObjId::EditAdvancedFollowTrigger || o.ext < 0) continue;
                if (w.prop(o, 535, 0.0) != 0.0)
                    controlEdits.push_back(o.targetGroup);
                else
                    keepGroup(o.targetGroup);
            }
            if (!controlEdits.empty()) {
                for (auto const& o : w.objs) {
                    if (o.cls == 1 && o.id == ObjId::AdvancedFollowTrigger && o.ext >= 0 &&
                        std::find(controlEdits.begin(), controlEdits.end(), o.controlID) != controlEdits.end())
                        keepGroup(static_cast<int>(w.prop(o, 51, static_cast<double>(o.targetGroup))));
                }
            }
        }
    }

    // for each group, the objects that move it (groupMovers), and the objects that are only decoration
    auto TriggerAnalysis::findGroupMovers() -> void {
        w.groupMovers.assign(w.groupAll.size(), {});
        for (size_t gr = 0; gr < w.groupAll.size(); ++gr) {
            for (int idx : w.groupAll[gr]) {
                if (w.objs[static_cast<size_t>(idx)].type != Decoration || w.centerLike[static_cast<size_t>(idx)])
                    w.groupMovers[gr].push_back(idx);
            }
        }
        {
            std::vector<char> centerObj(w.objs.size(), 0);
            auto markCenterOf = [&](int gr0) -> bool {
                bool changed = false;
                forRemapped(gr0, [&](int gr) {
                    if (gr <= 0 || gr > w.maxGroup) return;
                    auto setCenter = [&](int idx) {
                        if (idx >= 0 && !centerObj[static_cast<size_t>(idx)]) {
                            centerObj[static_cast<size_t>(idx)] = 1;
                            changed = true;
                        }
                    };
                    setCenter(w.mainObjectOf(gr));
                    setCenter(w.exportParentOf(gr));
                    if (!w.allOfGroup(gr).empty()) setCenter(w.allOfGroup(gr).front());
                });
                return changed;
            };
            struct Cond {
                int target;
                int center;
            };
            std::vector<Cond> conds;
            for (auto const& o : w.objs) {
                {
                    bool const isTeleport =
                            o.type == TeleportPortal || o.type == TeleportOrb ||
                            (o.cls == 1 && (o.id == ObjId::TeleportTrigger || o.id == ObjId::TeleportArea));
                    if (isTeleport && o.targetGroup > 0) {
                        forRemapped(o.targetGroup, [&](int gr) {
                            if (gr <= 0 || gr > w.maxGroup) return;
                            for (int idx : w.allOfGroup(gr)) centerObj[static_cast<size_t>(idx)] = 1;
                        });
                    }
                }
                if (o.cls != 1) continue;
                switch (o.id) {
                case 1346:
                    conds.push_back({o.targetGroup, o.centerGroup});
                    if (o.tf(Obj::TF_UseMoveTarget) || o.tf(Obj::TF_DirSnap)) {
                        for (int gr : {o.targetGroup, o.rotTarget, o.rotMinX, o.rotMinY, o.rotMaxX, o.rotMaxY})
                            conds.push_back({o.targetGroup, gr});
                    }
                    break;
                case 1347:
                    conds.push_back({o.targetGroup, o.centerGroup});
                    break;
                case 901:
                    if (o.tf(Obj::TF_UseMoveTarget) || o.tf(Obj::TF_DirSnap)) {
                        conds.push_back({o.targetGroup, o.targetModCenter});
                        conds.push_back({o.targetGroup, o.centerGroup});
                        conds.push_back({o.targetGroup, o.targetGroup});
                    }
                    break;
                case 2067:
                    if (w.extOf(o)) conds.push_back({o.targetGroup, static_cast<int>(w.prop(o, 71, 0.0))});
                    break;
                case 3022:
                    markCenterOf(o.targetGroup);
                    break;
                case 1914:
                    markCenterOf(o.centerGroup);
                    break;
                case 2062:
                    markCenterOf(o.targetGroup);
                    break;
                case 3006:
                case 3007:
                case 3008:
                    markCenterOf(o.centerGroup);
                    break;
                case 3661:
                    markCenterOf(o.centerGroup);
                    break;
                case 3016:
                    if (auto const* ex = w.extOf(o)) {
                        if (!(ex->num(138, 0.0) != 0.0 || ex->num(200, 0.0) != 0.0 || ex->num(201, 0.0) != 0.0))
                            conds.push_back({o.targetGroup, static_cast<int>(ex->num(71, 0.0))});
                    }
                    break;
                default:
                    break;
                }
            }
            std::vector<char> orderedGroups(static_cast<size_t>(w.maxGroup) + 1, 0);
            for (auto const& o : w.objs) {
                if (o.cls == 1 && o.id == ObjId::SpawnTrigger && o.tf(Obj::TF_SpawnOrdered)) {
                    forRemapped(o.targetGroup, [&](int gr) {
                        if (gr >= 0 && gr <= w.maxGroup) orderedGroups[static_cast<size_t>(gr)] = 1;
                    });
                }
            }
            bool usesRandom = false;
            for (auto const& o : w.objs) {
                if (o.cls == 1 && (o.id == ObjId::RandomTrigger || o.id == ObjId::AdvancedRandomTrigger ||
                                   (o.id == ObjId::SpawnTrigger && o.spawnDelayRange != 0.f)))
                    usesRandom = true;
            }
            auto relIn = [&](std::vector<char> const& relevant, int gr0) {
                bool found = false;
                forRemapped(gr0, [&](int gr) {
                    if (gr > 0 && gr <= w.maxGroup && relevant[static_cast<size_t>(gr)]) found = true;
                });
                return found;
            };
            std::unordered_map<int, std::vector<int>> areaByEffect;
            for (size_t i = 0; i < w.objs.size(); ++i) {
                Obj const& o = w.objs[i];
                if (o.cls != 1 || o.id < 3006 || o.id > 3008) continue;
                if (int const effectId = static_cast<int>(w.prop(o, 225, 0.0)); effectId > 0)
                    areaByEffect[effectId].push_back(static_cast<int>(i));
            }
            auto areaTargetRelevant = [&](Obj const& o, std::vector<char> const& relevant) {
                if (relIn(relevant, o.targetGroup)) return true;
                if (auto const* ex = w.extOf(o)) {
                    if (int const t = static_cast<int>(ex->num(51, 0.0)); t > 0 && relIn(relevant, t)) return true;
                    if (int const t = static_cast<int>(ex->num(226, 0.0)); t > 0 && relIn(relevant, t)) return true;
                }
                return false;
            };
            auto effectLive = [&](int effectId, std::vector<char> const& relevant) {
                auto it = areaByEffect.find(effectId);
                if (it == areaByEffect.end()) return false;
                for (int j : it->second) {
                    if (areaTargetRelevant(w.objs[static_cast<size_t>(j)], relevant)) return true;
                }
                return false;
            };
            auto inertMove = [&](Obj const& o, std::vector<char> const& relevant) -> bool {
                auto const* ex = w.extOf(o);
                auto targetRelevant = [&](bool areaKeys) {
                    if (relIn(relevant, o.targetGroup)) return true;
                    if (ex) {
                        if (int const t = static_cast<int>(ex->num(51, 0.0)); t > 0 && relIn(relevant, t)) return true;
                        if (areaKeys) {
                            if (int const t = static_cast<int>(ex->num(226, 0.0)); t > 0 && relIn(relevant, t))
                                return true;
                        }
                    }
                    return false;
                };
                switch (o.id) {
                case 901:
                case 1346:
                    if ((o.tf(Obj::TF_UseMoveTarget) || o.tf(Obj::TF_DirSnap)) && usesRandom) return false;
                    return !targetRelevant(false);
                case 1347:
                case 1814:
                case 2067:
                    return !targetRelevant(false);
                case 3006:
                case 3007:
                case 3008:
                    if (int const effectId = ex ? static_cast<int>(ex->num(225, 0.0)) : 0;
                        effectId > 0 && effectLive(effectId, relevant))
                        return false;
                    return !targetRelevant(true);
                case 3016:
                    return !usesRandom && !targetRelevant(false);
                default:
                    return false;
                }
            };
            auto posObservable = [&](Obj const& o, std::vector<char> const& relevant) {
                if (o.type == Decoration) return false;
                bool const isTrigger = o.cls == 1 && isSpawnableTrigger(o.id);
                if (!isTrigger) return true;
                if (!hasSimEffect(o.id)) return false;
                if (o.spawnTriggered && !o.touchTriggered) {
                    for (short gr : o.groups) {
                        if (gr >= 0 && gr <= w.maxGroup && orderedGroups[static_cast<size_t>(gr)]) return true;
                    }
                    return false;
                }
                if (inertMove(o, relevant)) return false;
                return true;
            };
            std::vector<char> relevantGroups(static_cast<size_t>(w.maxGroup) + 1, 0);
            std::vector<char> prevRelevant(relevantGroups.size(), 0);
            for (bool changed = true; changed;) {
                changed = false;
                std::fill(relevantGroups.begin(), relevantGroups.end(), 0);
                for (size_t i = 0; i < w.objs.size(); ++i) {
                    Obj const& o = w.objs[i];
                    if (posObservable(o, prevRelevant) || centerObj[i] || (i < w.areaRef.size() && w.areaRef[i]) ||
                        (i < w.afKeep.size() && w.afKeep[i])) {
                        for (short gr : o.groups) {
                            if (gr >= 0 && gr <= w.maxGroup) relevantGroups[static_cast<size_t>(gr)] = 1;
                        }
                    }
                }
                for (Cond const& c : conds) {
                    bool relevant = false;
                    forRemapped(c.target, [&](int gr) {
                        if (gr >= 0 && gr <= w.maxGroup && relevantGroups[static_cast<size_t>(gr)]) relevant = true;
                    });
                    if (relevant && markCenterOf(c.center)) changed = true;
                }
                if (relevantGroups != prevRelevant) {
                    changed = true;
                    prevRelevant = relevantGroups;
                }
            }
            w.groupRelevant = relevantGroups;
            w.areaEffectLive.clear();
            for (auto const& [effectId, areas] : areaByEffect) {
                if (effectLive(effectId, relevantGroups)) {
                    if (w.areaEffectLive.size() <= static_cast<size_t>(effectId))
                        w.areaEffectLive.resize(static_cast<size_t>(effectId) + 1, 0);
                    w.areaEffectLive[static_cast<size_t>(effectId)] = 1;
                }
            }
            w.decoLike.assign(w.objs.size(), 0);
            {
                size_t inertCount = 0;
                for (size_t i = 0; i < w.objs.size(); ++i) {
                    Obj const& o = w.objs[i];
                    if (o.type == Decoration) {
                        w.decoLike[i] = 1;
                        continue;
                    }
                    if (o.cls == 1 && isSpawnableTrigger(o.id) && !posObservable(o, relevantGroups)) {
                        w.decoLike[i] = 1;
                        ++inertCount;
                    }
                }
                if (debugOptions().check) {
                    size_t effectCount = 0;
                    for (auto const& [effectId, areas] : areaByEffect) {
                        if (!w.areaEffectLiveAt(effectId)) effectCount += areas.size();
                    }
                    std::fprintf(stderr,
                                 "[decor] %zu triggers without effect on the game treated as decoration (of which %zu "
                                 "area triggers with an effect id and no area that matters)\n",
                                 inertCount, effectCount);
                }
            }
            {
                auto keepMain = [&](int gr0, char why) {
                    forRemapped(gr0, [&](int gr) {
                        if (gr <= 0 || gr > w.maxGroup) return;
                        for (int idx : {w.mainObjectOf(gr), w.exportParentOf(gr)}) {
                            if (idx >= 0 && static_cast<size_t>(idx) < strongCenter.size())
                                strongCenter[static_cast<size_t>(idx)] |= why;
                        }
                    });
                };
                auto relAny = [&](int gr0) {
                    bool found = false;
                    forRemapped(gr0, [&](int gr) {
                        if (gr > 0 && gr <= w.maxGroup && relevantGroups[static_cast<size_t>(gr)]) found = true;
                    });
                    return found;
                };
                for (int gr = 1; gr <= w.maxGroup; ++gr)
                    if (relevantGroups[static_cast<size_t>(gr)]) keepMain(gr, 32);
                for (auto const& o : w.objs) {
                    if (o.cls != 1) continue;
                    auto const* ex = w.extOf(o);
                    bool relevant = relAny(o.targetGroup);
                    if (!relevant && ex) {
                        int const target51 = static_cast<int>(ex->num(51, 0.0));
                        int const target226 = static_cast<int>(ex->num(226, 0.0));
                        relevant = (target51 > 0 && relAny(target51)) || (target226 > 0 && relAny(target226));
                    }
                    if (o.id == ObjId::KeyframeAnimTrigger) relevant = true;
                    if (!relevant) continue;
                    for (int gr0 : {o.targetGroup, o.centerGroup, o.targetModCenter, o.rotTarget, o.rotMinX, o.rotMinY,
                                    o.rotMaxX, o.rotMaxY})
                        keepMain(gr0, 64);
                    if (o.id == ObjId::RotateTrigger && (o.tf(Obj::TF_UseMoveTarget) || o.tf(Obj::TF_DirSnap))) {
                        for (int gr0 :
                             {o.centerGroup, o.targetGroup, o.rotTarget, o.rotMinX, o.rotMinY, o.rotMaxX, o.rotMaxY}) {
                            forRemapped(gr0, [&](int gr) {
                                if (gr > 0)
                                    for (int idx : w.allOfGroup(gr)) strongCenter[static_cast<size_t>(idx)] |= 8;
                            });
                        }
                    }
                    if (ex) {
                        for (int k : {71, 342, 538, 373, 76}) {
                            int const value = static_cast<int>(ex->num(k, 0.0));
                            if (value > 0) keepMain(value, 64);
                        }
                    }
                }
                for (auto const& [ti, keyGroup] : kfUses) {
                    Obj const& t = w.objs[static_cast<size_t>(ti)];
                    auto it = byKfGroup.find(keyGroup);
                    if (it == byKfGroup.end()) continue;
                    bool used = relAny(t.targetGroup) || (t.ext >= 0 && relAny(static_cast<int>(w.prop(t, 51, 0.0))));
                    for (int q : it->second) {
                        if (used) break;
                        Obj const& key = w.objs[static_cast<size_t>(q)];
                        if (w.prop(key, 71, 0.0) > 0.0 || relAny(static_cast<int>(w.prop(key, 51, 0.0)))) used = true;
                    }
                    if (used)
                        for (int q : it->second) strongCenter[static_cast<size_t>(q)] |= 1;
                }
                for (auto const& o : w.objs) {
                    if (o.cls != 1 || !((o.id >= 3006 && o.id <= 3008) || o.id == ObjId::AdvancedFollowTrigger))
                        continue;
                    auto const* ex = w.extOf(o);
                    if (!ex) continue;
                    int target = static_cast<int>(ex->num(51, o.targetGroup));
                    if (int const areaTarget = static_cast<int>(ex->num(226, 0));
                        o.id != ObjId::AdvancedFollowTrigger && areaTarget != 0) {
                        target = areaTarget;
                    }
                    bool const ignoreParent = ex->num(280, 0) != 0;
                    bool const ignoreLinked = ex->num(281, 0) != 0;
                    forRemapped(target, [&](int gr) {
                        if (gr <= 0 || gr > w.maxGroup || !relevantGroups[static_cast<size_t>(gr)]) return;
                        if (World::TargetGroups const* targets = w.targetGroupsOf(gr, ignoreParent, ignoreLinked)) {
                            for (auto const& rep : targets->reps) {
                                strongCenter[static_cast<size_t>(rep.obj)] |= 4;
                                if (rep.center >= 0) strongCenter[static_cast<size_t>(rep.center)] |= 4;
                            }
                        }
                    });
                }
                size_t cleared = 0;
                for (size_t i = 0; i < w.objs.size(); ++i) {
                    if (!w.centerLike[i] || strongCenter[i] || centerObj[i] || !w.decoLike[i]) continue;
                    if (i < w.areaRef.size() && w.areaRef[i]) continue;
                    w.centerLike[i] = 0;
                    ++cleared;
                }
                w.centerWhy.assign(w.objs.size(), 0);
                for (size_t i = 0; i < w.objs.size(); ++i) {
                    w.centerWhy[i] =
                            static_cast<uint8_t>(static_cast<uint8_t>(strongCenter[i]) | (centerObj[i] ? 128 : 0) |
                                                 ((i < w.areaRef.size() && w.areaRef[i]) ? 128 : 0));
                }
                if (debugOptions().check)
                    std::fprintf(stderr, "[decor] %zu decorations are no longer possible centers (broad rule only)\n",
                                 cleared);
            }
            for (size_t gr = 0; gr < w.groupAll.size(); ++gr) {
                w.groupMovers[gr].clear();
                for (int idx : w.groupAll[gr]) {
                    if (!w.decoLike[static_cast<size_t>(idx)] || w.centerLike[static_cast<size_t>(idx)])
                        w.groupMovers[gr].push_back(idx);
                }
            }
        }
    }

    // keyframe animations and their splines (kfAnims, kfSplines)
    auto TriggerAnalysis::buildKeyframes() -> void {
        w.kfAnims.clear();
        w.kfSplines.clear();
        w.kfFirst.assign(w.objs.size(), -1);
        w.kfCount.assign(w.objs.size(), 0);
        {
            std::vector<std::pair<int, std::vector<int>>> keyGroups;
            auto listOf = [&](int keyGroup) -> std::vector<int>& {
                for (auto& e : keyGroups)
                    if (e.first == keyGroup) return e.second;
                keyGroups.emplace_back(keyGroup, std::vector<int>{});
                return keyGroups.back().second;
            };
            bool anyKeys = false;
            for (size_t i = 0; i < w.objs.size(); ++i) {
                Obj const& o = w.objs[i];
                if (o.cls != 1 || o.id != ObjId::KeyframeObject || o.ext < 0) continue;
                anyKeys = true;
                listOf(static_cast<int>(w.prop(o, 373, 0.0))).push_back(static_cast<int>(i));
            }
            for (auto& e : keyGroups) {
                std::stable_sort(e.second.begin(), e.second.end(), [&](int a, int b) {
                    return w.prop(w.objs[static_cast<size_t>(a)], 374, 0.0) <
                           w.prop(w.objs[static_cast<size_t>(b)], 374, 0.0);
                });
            }
            if (anyKeys)
                for (size_t i = 0; i < w.objs.size(); ++i) {
                    Obj const& t = w.objs[i];
                    if (t.cls != 1 || t.id != ObjId::KeyframeAnimTrigger || t.ext < 0) continue;
                    int const animGroup = static_cast<int>(w.prop(t, 76, 0.0));
                    if (animGroup <= 0) continue;
                    float const timeMod = static_cast<float>(w.prop(t, 520, 0.0));
                    float const posXMod = static_cast<float>(w.prop(t, 521, 0.0));
                    float posYMod = static_cast<float>(w.prop(t, 545, 0.0));
                    if (posYMod == 0.f) posYMod = posXMod;
                    float const rotMod = static_cast<float>(w.prop(t, 522, 0.0));
                    float const scaleXMod = static_cast<float>(w.prop(t, 523, 0.0));
                    float scaleYMod = static_cast<float>(w.prop(t, 546, 0.0));
                    if (scaleYMod == 0.f) scaleYMod = scaleXMod;
                    auto buildFor = [&](int group) -> std::pair<int, int> {
                        std::vector<int> seen;
                        int const first = static_cast<int>(w.kfAnims.size());
                        for (int m : w.allOfGroup(group)) {
                            Obj const& key = w.objs[static_cast<size_t>(m)];
                            if (key.id != ObjId::KeyframeObject || key.ext < 0) continue;
                            int const keyGroup = static_cast<int>(w.prop(key, 373, 0.0));
                            if (std::find(seen.begin(), seen.end(), keyGroup) != seen.end()) continue;
                            seen.push_back(keyGroup);
                            World::KfAnim anim;
                            anim.target = t.targetGroup != 0 ? t.targetGroup : static_cast<int>(w.prop(key, 51, 0.0));
                            anim.uid = t.uid;
                            anim.control = t.controlID;
                            anim.keys = listOf(keyGroup);
                            anim.timeMod = timeMod;
                            anim.pxm = posXMod;
                            anim.pym = posYMod;
                            anim.rotm = rotMod;
                            anim.sxm = scaleXMod;
                            anim.sym = scaleYMod;
                            std::vector<World::KfKeyState> keyStates(anim.keys.size());
                            for (size_t q = 0; q < anim.keys.size(); ++q) {
                                Obj const& keyObj = w.objs[static_cast<size_t>(anim.keys[q])];
                                keyStates[q].px = static_cast<float>(keyObj.posX);
                                keyStates[q].py = static_cast<float>(keyObj.posY);
                                keyStates[q].sx = static_cast<double>(keyObj.scaleX * (keyObj.flipX ? -1.f : 1.f));
                                keyStates[q].sy = static_cast<double>(keyObj.scaleY * (keyObj.flipY ? -1.f : 1.f));
                                keyStates[q].rotX = static_cast<double>(keyObj.rotX);
                                keyStates[q].rotY = static_cast<double>(keyObj.rotY);
                            }
                            buildKeyframeAnim(w, anim.keys, keyStates, anim, w.kfSplines, timeMod, posXMod, posYMod,
                                              rotMod, scaleXMod, scaleYMod);
                            for (auto const& e : anim.e)
                                if (e.spawnGroup > 0) anim.spawns = true;
                            w.kfAnims.push_back(std::move(anim));
                        }
                        return {first, static_cast<int>(w.kfAnims.size()) - first};
                    };
                    auto const range = buildFor(animGroup);
                    w.kfFirst[i] = range.first;
                    w.kfCount[i] = range.second;
                    w.objs[i].kfGroup = animGroup;
                    forRemapped(animGroup, [&](int remapped) {
                        if (remapped == animGroup || remapped <= 0) return;
                        long long const key = (static_cast<long long>(i) << 20) | static_cast<long long>(remapped);
                        if (w.kfAlt.count(key)) return;
                        w.kfAlt[key] = buildFor(remapped);
                    });
                }
        }
    }

    // objects that can get their own position, rotation or scale (mayHaveDyn, scaleTarget), and whether the
    // player can be moved (pPossible)
    auto TriggerAnalysis::markDynamicObjects() -> void {
        w.mayHaveDyn.assign(w.objs.size(), 0);
        for (auto const& o : w.objs) {
            if (o.cls != 1 ||
                (o.id != ObjId::RotateTrigger && o.id != ObjId::FollowPlayerYTrigger && o.id != ObjId::ScaleTrigger))
                continue;
            forRemapped(o.targetGroup, [&](int gr) {
                for (int idx : w.allOfGroup(gr)) w.mayHaveDyn[static_cast<size_t>(idx)] = 1;
            });
        }
        for (auto const& anim : w.kfAnims) {
            forRemapped(anim.target, [&](int gr) {
                for (int idx : w.allOfGroup(gr)) {
                    w.mayHaveDyn[static_cast<size_t>(idx)] = 1;
                    w.objs[static_cast<size_t>(idx)].movable = true;
                }
            });
        }
        w.scaleTarget.assign(w.objs.size(), 0);
        for (auto const& o : w.objs) {
            if (o.cls != 1 || o.id != ObjId::ScaleTrigger) continue;
            forRemapped(o.targetGroup, [&](int gr) {
                for (int idx : w.allOfGroup(gr)) w.scaleTarget[static_cast<size_t>(idx)] = 1;
            });
        }
        for (auto const& anim : w.kfAnims) {
            forRemapped(anim.target, [&](int gr) {
                for (int idx : w.allOfGroup(gr)) w.scaleTarget[static_cast<size_t>(idx)] = 1;
            });
        }
        w.pPossible = false;
        for (auto const& o : w.objs) {
            if (o.cls != 1) continue;
            if (o.id == ObjId::FollowPlayerYTrigger || o.id == ObjId::AdvancedFollowTrigger ||
                (o.id == ObjId::MoveTrigger && (o.tf(Obj::TF_LockPY) || o.tf(Obj::TF_LockCY)))) {
                w.pPossible = true;
                break;
            }
        }
    }

    // definitions of the area triggers and of the objects they target (areaDefs)
    auto TriggerAnalysis::buildAreas() -> void {
        w.areaDefs.clear();
        w.areaDefOf.assign(w.objs.size(), -1);
        w.areaTarget.assign(w.objs.size(), 0);
        w.hasAreaEffects = false;
        for (size_t i = 0; i < w.objs.size(); ++i) {
            Obj const& o = w.objs[i];
            if (o.cls != 1) continue;
            bool const isEdit = o.id >= 3011 && o.id <= 3013;
            if (!(o.id >= 3006 && o.id <= 3008) && !isEdit) continue;
            auto const* ex = w.extOf(o);
            if (!ex) continue;
            World::AreaDef area;
            area.idx = static_cast<int>(i);
            area.id = o.id;
            area.edit = isEdit;
            area.kind = isEdit ? o.id - 3011 : o.id - 3006;
            float const fallback = 0.f;
            auto num = [&](int k) { return static_cast<float>(ex->num(k, static_cast<double>(fallback))); };
            auto intNum = [&](int k) {
                return ex->has(k) ? static_cast<float>(static_cast<int>(ex->num(k))) : fallback;
            };
            int const keys[28] = {222, 223, 220, 221, 263, 264, 282, 218, 219, 231, 232, 237, 238, 239,
                                  240, 233, 234, 235, 236, 270, 271, 265, 285, 275, 286, 252, 253, 288};
            bool const isInt[28] = {1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0};
            for (int k = 0; k < 28; ++k) area.v[k] = isInt[k] ? intNum(keys[k]) : num(keys[k]);
            auto rate = [&](int k) {
                double const d = ex->num(k, 0.0);
                return static_cast<float>(static_cast<int>(static_cast<float>(d) * 100.f)) / 100.f;
            };
            area.easeInType = static_cast<int>(ex->num(242, 0));
            area.easeInRate = rate(243);
            area.easeOutType = static_cast<int>(ex->num(248, 0));
            area.easeOutRate = rate(249);
            area.relative = ex->num(287, 0) != 0;
            area.xyMode = ex->num(241, 0) != 0;
            area.easeOut = ex->num(261, 0) != 0;
            area.dirType = static_cast<int>(ex->num(262, 0));
            area.inbound = ex->num(276, 0) != 0;
            area.twoDirections = ex->num(283, 0) != 0;
            area.priority = static_cast<int>(ex->num(341, 0));
            area.effectID = static_cast<int>(ex->num(225, 0));
            area.useEffectID = ex->num(355, 0) != 0;
            area.target = static_cast<int>(ex->num(51, o.targetGroup));
            area.center = static_cast<int>(ex->num(71, o.centerGroup));
            area.special = static_cast<int>(ex->num(538, 0));
            if (int const target2 = static_cast<int>(ex->num(226, 0)); target2 != 0) area.target = target2;
            if (int const center2 = static_cast<int>(ex->num(342, 0)); center2 != 0) {
                if (center2 < 0)
                    area.special = center2;
                else
                    area.center = center2;
            }
            area.control = o.controlID;
            area.duration = o.duration;
            area.ignoreGroupParent = ex->num(280, 0) != 0;
            area.ignoreLinked = ex->num(281, 0) != 0;
            area.dontEditParent = ex->num(539, 0) != 0;
            if (!isEdit)
                for (int k : {1, 3, 8, 10, 12, 14, 16, 18, 20, 26})
                    if (area.v[k] != 0.f) area.variance = true;
            w.areaDefOf[i] = static_cast<int>(w.areaDefs.size());
            w.areaDefs.push_back(area);
            if (!isEdit) {
                w.hasAreaEffects = true;
                auto mark = [&](int idx) {
                    w.areaTarget[static_cast<size_t>(idx)] = 1;
                    w.objs[static_cast<size_t>(idx)].movable = true;
                    if (area.kind != 0 && w.objs[static_cast<size_t>(idx)].type != Decoration)
                        w.mayHaveDyn[static_cast<size_t>(idx)] = 1;
                };
                forRemapped(area.target, [&](int gr) {
                    for (int idx : w.allOfGroup(gr)) mark(idx);
                    if (auto const* targets = w.targetGroupsOf(gr, area.ignoreGroupParent, area.ignoreLinked)) {
                        for (auto const& rep : targets->reps)
                            for (int idx : rep.children) mark(idx);
                    }
                });
            }
        }
        for (auto& targets : w.targetGroups)
            for (auto& rep : targets.reps) {
                rep.phys.clear();
                for (int c : rep.children) {
                    if (!w.decoLikeAt(c) || w.centerLike[static_cast<size_t>(c)]) rep.phys.push_back(c);
                }
            }
    }

    // definitions of the advanced follow triggers (advDefs) and their rings of saved positions (afRings)
    auto TriggerAnalysis::buildAdvancedFollows() -> void {
        w.advDefs.clear();
        w.advDefOf.assign(w.objs.size(), -1);
        for (size_t i = 0; i < w.objs.size(); ++i) {
            Obj const& o = w.objs[i];
            if (o.cls != 1 || o.id != ObjId::AdvancedFollowTrigger) continue;
            auto const* ex = w.extOf(o);
            if (!ex) continue;
            World::AdvDef follow;
            auto floatOf = [&](int k) { return static_cast<float>(ex->num(k, 0.0)); };
            auto boolOf = [&](int k) { return ex->num(k, 0.0) != 0.0; };
            auto intOf = [&](int k) { return static_cast<int>(ex->num(k, 0.0)); };
            follow.idx = static_cast<int>(i);
            follow.target = intOf(51);
            follow.control = o.controlID;
            follow.key = boolOf(138) ? -1 : boolOf(200) ? -2 : boolOf(201) ? -3 : intOf(71);
            follow.delay = floatOf(292);
            follow.delayV = floatOf(293);
            follow.maxSpeed = floatOf(298);
            follow.maxSpeedV = floatOf(299);
            follow.xOnly = boolOf(306);
            follow.yOnly = boolOf(307);
            follow.range = floatOf(308);
            follow.rangeV = floatOf(309);
            follow.accel = floatOf(334);
            follow.accelV = floatOf(335);
            follow.startSpeed = floatOf(300);
            follow.startSpeedV = floatOf(301);
            follow.startRef = intOf(560);
            follow.startDir = floatOf(563);
            follow.startDirV = floatOf(564);
            follow.dirRef = intOf(565);
            follow.startMode = intOf(572);
            follow.useTargetDir = boolOf(305);
            follow.ignoreDisabled = boolOf(336);
            follow.rotate = boolOf(339);
            follow.rotOffset = floatOf(340);
            follow.easing = floatOf(361);
            follow.easingV = floatOf(362);
            follow.m1Range = floatOf(359);
            follow.m1RangeV = floatOf(360);
            follow.m1Fric = floatOf(558);
            follow.m1FricV = floatOf(559);
            follow.m1Acc = floatOf(357);
            follow.m1AccV = floatOf(358);
            follow.m1Fric2 = floatOf(561);
            follow.m1Fric2V = floatOf(562);
            follow.dirEasing = floatOf(363);
            follow.angleMinDist = floatOf(364);
            follow.priority = intOf(365);
            follow.mode = intOf(367);
            follow.once = boolOf(571);
            follow.steer = floatOf(316);
            follow.steerV = floatOf(317);
            follow.steerLowOn = boolOf(337);
            follow.steerLow = floatOf(318);
            follow.steerLowV = floatOf(319);
            follow.steerHighOn = boolOf(338);
            follow.steerHigh = floatOf(320);
            follow.steerHighV = floatOf(321);
            follow.speedLow = floatOf(322);
            follow.speedLowV = floatOf(323);
            follow.speedHigh = floatOf(324);
            follow.speedHighV = floatOf(325);
            follow.brk = floatOf(326);
            follow.brkV = floatOf(327);
            follow.brkAngle = floatOf(328);
            follow.brkAngleV = floatOf(329);
            follow.brkSteer = floatOf(330);
            follow.brkSteerV = floatOf(331);
            follow.brkLimit = floatOf(332);
            follow.brkLimitV = floatOf(333);
            follow.ignoreGroupParent = boolOf(280);
            follow.ignoreLinked = boolOf(281);
            forRemapped(follow.target, [&](int gr) {
                if (gr > 0 && gr <= w.maxGroup && !w.moversOfGroup(gr).empty()) follow.relevant = true;
                if (auto const* targets = w.targetGroupsOf(gr, follow.ignoreGroupParent, follow.ignoreLinked)) {
                    for (auto const& rep : targets->reps)
                        if (!rep.phys.empty()) follow.relevant = true;
                }
            });
            w.advDefOf[i] = static_cast<int>(w.advDefs.size());
            w.advDefs.push_back(follow);
            forRemapped(follow.target, [&](int gr) {
                for (int idx : w.allOfGroup(gr)) {
                    w.areaTarget[static_cast<size_t>(idx)] = 1;
                    w.objs[static_cast<size_t>(idx)].movable = true;
                    w.mayHaveDyn[static_cast<size_t>(idx)] = 1;
                }
                if (auto const* targets = w.targetGroupsOf(gr, follow.ignoreGroupParent, follow.ignoreLinked)) {
                    for (auto const& rep : targets->reps)
                        for (int idx : rep.children) {
                            w.areaTarget[static_cast<size_t>(idx)] = 1;
                            w.objs[static_cast<size_t>(idx)].movable = true;
                            w.mayHaveDyn[static_cast<size_t>(idx)] = 1;
                        }
                }
            });
        }
        w.afRings.clear();
        w.afRingFloats = 0;
        {
            int maxSize = 0;
            std::vector<std::pair<int, int>> wanted;
            auto addKey = [&](int key, int size) {
                if (key == 0) return;
                for (auto& e : wanted) {
                    if (e.first == key) {
                        e.second = std::max(e.second, size);
                        return;
                    }
                }
                wanted.emplace_back(key, size);
            };
            for (auto const& follow : w.advDefs) {
                if (!(follow.delay > 0.f || follow.delayV != 0.f)) continue;
                int const size = static_cast<int>(
                        std::ceil(static_cast<float>(static_cast<double>(follow.delayV + follow.delay) /
                                                     0.004166666666666667)) +
                        1.f);
                maxSize = std::max(maxSize, size);
                if (follow.key < 0)
                    addKey(follow.key, size);
                else
                    forRemapped(follow.key, [&](int gr) { addKey(gr, size); });
            }
            if (maxSize > 0)
                for (auto const& o : w.objs)
                    if (o.cls == 1 && o.id == ObjId::RetargetAdvancedFollowTrigger) {
                        int const key = o.tf(Obj::TF_TargetP1)    ? -1
                                        : o.tf(Obj::TF_TargetP2)  ? -2
                                        : o.tf(Obj::TF_FollowCPP) ? -3
                                                                  : o.centerGroup;
                        if (key < 0)
                            addKey(key, maxSize);
                        else if (key > 0)
                            forRemapped(key, [&](int gr) { addKey(gr, maxSize); });
                    }
            for (auto const& [key, size] : wanted) {
                World::AfRing ring;
                ring.key = key;
                ring.size = std::max(size, 1);
                if (key > 0) {
                    ring.obj = w.mainObjectOf(key);
                    if (ring.obj < 0) continue;
                }
                bool merged = false;
                for (auto& r : w.afRings) {
                    if ((key > 0 && r.obj == ring.obj) || (key < 0 && r.key == key)) {
                        if (ring.size > r.size) r.size = ring.size;
                        merged = true;
                        break;
                    }
                }
                if (!merged) w.afRings.push_back(ring);
            }
            int base = 0;
            for (auto& r : w.afRings) {
                r.base = base;
                base += r.size;
            }
            w.afRingFloats = base * 2;
        }
    }

    // keyframe animations whose keys can move, and the groups whose keyframes can take the fast path (kfOpt)
    auto TriggerAnalysis::markOptimizedKeyframes() -> void {
        for (auto& anim : w.kfAnims) {
            anim.dynKeys = false;
            for (int k : anim.keys) {
                if (w.objs[static_cast<size_t>(k)].movable || w.mayHaveDyn[static_cast<size_t>(k)] ||
                    w.areaTarget[static_cast<size_t>(k)]) {
                    anim.dynKeys = true;
                    break;
                }
            }
        }
        for (auto& o : w.objs) o.kfOpt = false;
        w.kfOptGroups.clear();
        kfCandidate = std::vector<uint8_t>(w.objs.size(), 0);
        anyKfCandidate = false;
        for (auto const& anim : w.kfAnims) {
            if (anim.dynKeys)
                for (int k : anim.keys) {
                    kfCandidate[static_cast<size_t>(k)] = 1;
                    anyKfCandidate = true;
                }
        }
        for (auto const& t : w.objs) {
            if (t.cls != 1 || !(t.id == ObjId::TeleportTrigger || t.type == TeleportPortal) || t.targetGroup <= 0 ||
                t.tpOrange)
                continue;
            for (int i : w.allOfGroup(t.targetGroup)) {
                Obj const& o = w.objs[static_cast<size_t>(i)];
                bool const opt = w.moveOptExported ? o.moveOpt : (o.type == Decoration && o.cls != 1);
                if (opt && o.movable) {
                    kfCandidate[static_cast<size_t>(i)] = 1;
                    anyKfCandidate = true;
                }
            }
        }
        if (w.moveOptExported)
            for (size_t i = 0; i < w.objs.size(); ++i) {
                Obj const& o = w.objs[i];
                if (o.moveOpt && o.movable && o.type != Decoration && o.cls != 1) {
                    kfCandidate[i] = 1;
                    anyKfCandidate = true;
                }
            }
        if (anyKfCandidate) {
            std::vector<uint8_t> deopt(w.objs.size(), 0);
            auto center = [&](int gr0) {
                forRemapped(gr0, [&](int gr) {
                    if (gr <= 0 || gr > 9999) return;
                    int const parent = static_cast<size_t>(gr) < w.exportParents.size()
                                               ? w.exportParents[static_cast<size_t>(gr)]
                                               : -1;
                    if (parent >= 0) {
                        deopt[static_cast<size_t>(parent)] = 1;
                        return;
                    }
                    auto const& all = w.allOfGroup(gr);
                    if (all.size() == 1) deopt[static_cast<size_t>(all[0])] = 1;
                });
            };
            auto whole = [&](int gr0) {
                forRemapped(gr0, [&](int gr) {
                    if (gr > 0 && gr <= 9999)
                        for (int i : w.allOfGroup(gr)) deopt[static_cast<size_t>(i)] = 1;
                });
            };
            for (auto const& t : w.objs) {
                if (t.cls != 1) continue;
                int const centerGroup = t.centerGroup != 0 ? t.centerGroup : static_cast<int>(w.prop(t, 71, 0.0));
                switch (t.id) {
                case 901:
                    center(centerGroup);
                    center(t.targetModCenter);
                    break;
                case 1346:
                    whole(t.targetGroup);
                    center(centerGroup);
                    center(t.rotTarget);
                    break;
                case 1347:
                case 3016:
                case 3661:
                    center(centerGroup);
                    break;
                case 2067:
                    center(centerGroup);
                    whole(t.targetGroup);
                    break;
                case 2915:
                    center(t.targetGroup);
                    break;
                case 3006:
                case 3007:
                case 3008: {
                    int const areaCenter = static_cast<int>(w.prop(t, 342, 0.0));
                    center(areaCenter != 0 ? areaCenter : centerGroup);
                    break;
                }
                default:
                    break;
                }
            }
            for (size_t k = 0; k < w.objs.size(); ++k) {
                if (!kfCandidate[k] || deopt[k]) continue;
                Obj& o = w.objs[k];
                o.kfOpt = true;
                for (short gr : o.groups)
                    if (gr > 0) w.kfOptGroups.push_back(gr);
            }
            std::sort(w.kfOptGroups.begin(), w.kfOptGroups.end());
            w.kfOptGroups.erase(std::unique(w.kfOptGroups.begin(), w.kfOptGroups.end()), w.kfOptGroups.end());
        }
    }

    // one slot per spawnable trigger with an effect, for its activated bit (trigSlot)
    auto TriggerAnalysis::assignTriggerSlots() -> void {
        w.trigSlot.assign(w.objs.size(), -1);
        w.nTrigSlots = 0;
        for (size_t i = 0; i < w.objs.size(); ++i) {
            if (w.objs[i].cls == 1 && isSpawnableTrigger(w.objs[i].id) && hasSimEffect(w.objs[i].id))
                w.trigSlot[i] = w.nTrigSlots++;
        }
    }

    // collision blocks, their pairs and cells, and which of them move (blockMoving)
    auto TriggerAnalysis::buildCollisionBlocks() -> void {
        w.hasCollisionTriggers = false;
        for (auto const& o : w.objs)
            if (o.cls == 1 && (o.id == ObjId::CollisionTrigger || o.id == ObjId::InstantCollisionTrigger))
                w.hasCollisionTriggers = true;
        w.dynamicBlocks.clear();
        w.blockCells.clear();
        w.blockExtended.clear();
        w.blockMoving.clear();
        w.collisionPairs.clear();
        {
            auto addPair = [&](int a, int b) {
                int const hi = a < b ? b : a;
                int const lo = a < b ? a : b;
                w.collisionPairs.push_back((hi + 1000) * 10000 + lo);
            };
            for (auto const& o : w.objs) {
                if (o.cls != 1) continue;
                if ((o.id == ObjId::CollisionTrigger || o.id == ObjId::InstantCollisionTrigger) &&
                    o.tf(Obj::TF_FollowCPP))
                    addPair(10000, 10001);
                else if (o.id == ObjId::CollisionTrigger) {
                    addPair(o.tf(Obj::TF_TargetP1) ? 10000 : (o.tf(Obj::TF_TargetP2) ? 10001 : o.itemID), o.itemID2);
                    if (o.tf(Obj::TF_TargetP1) && o.tf(Obj::TF_TargetP2)) addPair(10001, o.itemID2);
                } else if (o.id == ObjId::InstantCollisionTrigger) {
                    bool const p1 = o.tf(Obj::TF_TargetP1);
                    bool const p2 = o.tf(Obj::TF_TargetP2);
                    addPair(p1 ? 10000 : (p2 ? 10001 : o.itemID), o.itemID2);
                    if (p1 && p2) addPair(10001, o.itemID2);
                }
            }
            std::sort(w.collisionPairs.begin(), w.collisionPairs.end());
            w.collisionPairs.erase(std::unique(w.collisionPairs.begin(), w.collisionPairs.end()),
                                   w.collisionPairs.end());
            {
                auto const& pairs = w.collisionPairs;
                w.collPairRow.clear();
                w.collPairQ0 = 0;
                size_t i = static_cast<size_t>(std::lower_bound(pairs.begin(), pairs.end(), 0) - pairs.begin());
                if (i < pairs.size()) {
                    int const firstRow = pairs[i] / 10000;
                    int const lastRow = pairs.back() / 10000;
                    w.collPairQ0 = firstRow;
                    w.collPairRow.assign(static_cast<size_t>(lastRow - firstRow + 2), 0);
                    for (int q = firstRow; q <= lastRow + 1; ++q) {
                        while (i < pairs.size() && pairs[i] / 10000 < q) ++i;
                        w.collPairRow[static_cast<size_t>(q - firstRow)] = static_cast<int>(i);
                    }
                }
            }
            w.collisionItemListened.assign(10002, 0);
            for (int k : w.collisionPairs) {
                int const hi = k / 10000 - 1000;
                int const lo = k % 10000;
                if (hi >= 0 && hi < 10002) w.collisionItemListened[static_cast<size_t>(hi)] = 1;
                if (lo >= 0 && lo < 10002) w.collisionItemListened[static_cast<size_t>(lo)] = 1;
            }
            w.collisionPartners.assign(10002, {});
            for (int k : w.collisionPairs) {
                int const hi = k / 10000 - 1000;
                int const lo = k % 10000;
                if (hi < 0 || hi >= 10002 || lo < 0 || lo >= 10002) continue;
                w.collisionPartners[static_cast<size_t>(hi)].push_back(lo);
                if (lo != hi) w.collisionPartners[static_cast<size_t>(lo)].push_back(hi);
            }
            for (auto& partners : w.collisionPartners) {
                std::sort(partners.begin(), partners.end());
                partners.erase(std::unique(partners.begin(), partners.end()), partners.end());
            }
        }
        for (auto const& o : w.objs) {
            if (o.isCollisionBlock() && o.tf(Obj::TF_DynamicBlock)) dynItems.push_back(o.itemID);
        }
        std::sort(dynItems.begin(), dynItems.end());
        dynItems.erase(std::unique(dynItems.begin(), dynItems.end()), dynItems.end());
        for (size_t i = 0; i < w.objs.size(); ++i) {
            auto const& o = w.objs[i];
            if (!o.isCollisionBlock()) continue;
            if (o.tf(Obj::TF_DynamicBlock)) {
                w.dynamicBlocks.push_back(static_cast<int>(i));
                continue;
            }
            if (o.extendedCollision) {
                w.blockExtended.push_back(static_cast<int>(i));
                continue;
            }
            if (o.movable) {
                if (!testedItem(o.itemID)) continue;
                w.blockMoving.push_back(static_cast<int>(i));
                continue;
            }
            int const cx = World::cellOf(o.posX);
            int const cy = w.cellYOf(o.posY);
            if (w.blockCells.size() <= static_cast<size_t>(cx)) w.blockCells.resize(static_cast<size_t>(cx) + 1);
            auto& column = w.blockCells[static_cast<size_t>(cx)];
            if (column.size() <= static_cast<size_t>(cy)) column.resize(static_cast<size_t>(cy) + 1);
            column[static_cast<size_t>(cy)].push_back(static_cast<int>(i));
        }
        w.blockMovingMark.assign(w.objs.size(), 0);
        for (int idx : w.blockMoving) w.blockMovingMark[static_cast<size_t>(idx)] = 1;
        {
            std::vector<int> movingItems;
            for (int idx : w.blockMoving) movingItems.push_back(w.objs[static_cast<size_t>(idx)].itemID);
            std::sort(movingItems.begin(), movingItems.end());
            movingItems.erase(std::unique(movingItems.begin(), movingItems.end()), movingItems.end());
            auto pairsWithMoving = [&](int item) {
                for (int b : movingItems)
                    if (w.collisionPairListened(item, b)) return true;
                return false;
            };
            w.blockMovPair.assign(w.dynamicBlocks.size(), 0);
            for (size_t k = 0; k < w.dynamicBlocks.size(); ++k)
                w.blockMovPair[k] = pairsWithMoving(w.objs[static_cast<size_t>(w.dynamicBlocks[k])].itemID) ? 1 : 0;
            w.blockMovPlayer[0] = pairsWithMoving(10000);
            w.blockMovPlayer[1] = pairsWithMoving(10001);
        }
        w.blockMovCells.clear();
        for (int idx : w.blockMoving) {
            Obj const& o = w.objs[static_cast<size_t>(idx)];
            int const cx = World::cellOf(o.posX);
            int const cy = w.cellYOf(o.posY);
            if (w.blockMovCells.size() <= static_cast<size_t>(cx)) w.blockMovCells.resize(static_cast<size_t>(cx) + 1);
            auto& column = w.blockMovCells[static_cast<size_t>(cx)];
            if (column.size() <= static_cast<size_t>(cy)) column.resize(static_cast<size_t>(cy) + 1);
            column[static_cast<size_t>(cy)].push_back(idx);
        }
    }

    // whether teleports have several targets (tpMulti) and whether the level has random triggers (hasRandom)
    auto TriggerAnalysis::findRandomness() -> void {
        w.tpMulti = false;
        for (auto const& o : w.objs) {
            if (!((o.cls == 1 && o.id == ObjId::TeleportTrigger) || o.type == TeleportPortal ||
                  o.type == TeleportOrb) ||
                o.tpOrange || o.targetGroup <= 0)
                continue;
            forRemapped(o.targetGroup, [&](int gr) {
                if (gr > 0 && w.allOfGroup(gr).size() > 1) w.tpMulti = true;
            });
        }
        w.hasRandom = false;
        for (auto const& o : w.objs) {
            if (o.cls == 1 && (o.id == ObjId::RandomTrigger || o.id == ObjId::AdvancedRandomTrigger ||
                               (o.id == ObjId::SpawnTrigger && o.spawnDelayRange != 0.f)))
                w.hasRandom = true;
        }
        {
            auto multi = [&](int gr) { return gr > 0 && w.mainObjectOf(gr) < 0 && w.allOfGroup(gr).size() > 1; };
            for (auto const& follow : w.advDefs) {
                if (follow.relevant && (follow.startSpeedV != 0.f || follow.startDirV != 0.f ||
                                        multi(follow.startRef) || multi(follow.dirRef)))
                    w.hasRandom = true;
            }
        }
    }

    // game events that the event triggers listen to (eventUsed)
    auto TriggerAnalysis::listEvents() -> void {
        w.eventUsed[0] = w.eventUsed[1] = 0;
        for (auto const& o : w.objs) {
            if (o.cls != 1 || o.id != ObjId::EventTrigger) continue;
            auto const* ex = w.extOf(o);
            if (!ex) continue;
            if (auto const* events = ex->list(430)) {
                for (double d : *events) {
                    int const eventId = static_cast<int>(d);
                    if (eventId > 0 && eventId < 128) w.eventUsed[eventId >> 6] |= 1ull << (eventId & 63);
                }
            }
        }
    }

    // triggers the simulator does not support or only approximates
    auto TriggerAnalysis::listUnsupported() -> void {
        w.unsupported.clear();
        w.cameraUnsupported.clear();
        w.approximations.clear();
        {
            bool varNoTable = false;
            for (auto const& area : w.areaDefs)
                if (area.variance && w.areaRand.empty()) varNoTable = true;
            if (varNoTable)
                w.approximations.push_back(
                        "area trigger with variance (the game's random values are not in the export: taken as zero)");
            bool screen = false;
            for (auto const& area : w.areaDefs)
                if (!area.edit && area.special <= -3) screen = true;
            bool cam = screen;
            bool camOld = false;
            for (auto const& o : w.objs) {
                if (o.cls != 1) continue;
                if (o.id == ObjId::StaticCameraTrigger)
                    cam = true;
                else if (o.id == ObjId::MoveTrigger && (o.tf(Obj::TF_LockCX) || o.tf(Obj::TF_LockCY)))
                    cam = true;
                if ((o.id == ObjId::StaticCameraTrigger || o.id == ObjId::EdgeCameraTrigger) && o.ext < 0)
                    camOld = true;
            }
            if (camOld) cam = false;
            w.needsCamera = cam;
        }
        if (w.hasTriggerData) {
            for (auto const& o : w.objs) {
                if (o.type == Decoration) continue;
                if (o.cls == 1 && o.id == ObjId::AdvancedFollowTrigger && !w.relevantGroup(o.targetGroup)) continue;
                bool const decoOnly = o.cls == 1 && (o.id == ObjId::MoveTrigger || o.id == ObjId::RotateTrigger) &&
                                      !w.relevantGroup(o.targetGroup);
                if (char const* why = decoOnly ? nullptr : unsupportedReason(o)) {
                    std::string entry = std::to_string(o.id) + " (" + why + ")";
                    auto& list = (o.cls == 1 && cameraOnlyTrigger(o.id)) ? w.cameraUnsupported : w.unsupported;
                    if (std::find(list.begin(), list.end(), entry) == list.end()) list.push_back(entry);
                }
                if (o.cls == 1 && o.id == ObjId::EventTrigger && o.ext >= 0 && w.prop(o, 525, 0.0) != 0.0) {
                    std::string entry = "3604 (Event of a single player: not simulated)";
                    if (std::find(w.approximations.begin(), w.approximations.end(), entry) == w.approximations.end())
                        w.approximations.push_back(entry);
                }
                if (o.cls == 1 && o.id == ObjId::EventTrigger && o.ext < 0 && o.targetGroup > 0) {
                    std::vector<int> todo{o.targetGroup};
                    std::vector<char> seen(static_cast<size_t>(w.maxGroup) + 2, 0);
                    bool relevant = false;
                    while (!todo.empty() && !relevant) {
                        int const gr = todo.back();
                        todo.pop_back();
                        if (gr <= 0 || gr > w.maxGroup || seen[static_cast<size_t>(gr)]) continue;
                        seen[static_cast<size_t>(gr)] = 1;
                        for (int idx : w.allOfGroup(gr)) {
                            Obj const& t = w.objs[static_cast<size_t>(idx)];
                            if (t.cls == 1 && (isSpawnableTrigger(t.id) || t.trigger)) {
                                switch (t.id) {
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
                                    break;
                                case 901:
                                case 1049:
                                case 1268:
                                case 1346:
                                case 1347:
                                case 1585:
                                case 1616:
                                case 1814:
                                    todo.push_back(t.targetGroup);
                                    todo.push_back(t.centerGroup);
                                    break;
                                default:
                                    relevant = true;
                                    break;
                                }
                            } else if (t.type != Decoration)
                                relevant = true;
                            if (relevant) break;
                        }
                    }
                    if (relevant) {
                        std::string entry = "3604 (Event acting on the game, old export: export the level again)";
                        if (std::find(w.approximations.begin(), w.approximations.end(), entry) ==
                            w.approximations.end())
                            w.approximations.push_back(entry);
                    }
                }
                if (o.cls == 1 && o.id == ObjId::AdvancedFollowTrigger && o.ext >= 0 &&
                    w.relevantGroup(o.targetGroup)) {
                    int const defIdx = w.advDefOf[static_cast<size_t>(&o - w.objs.data())];
                    if (defIdx >= 0) {
                        auto const& follow = w.advDefs[static_cast<size_t>(defIdx)];
                        std::string entry;
                        if (w.areaRand.empty() && (follow.delayV != 0.f))
                            entry = "3016 (advanced follow with delay variance: the game's random values are not in "
                                    "the export)";
                        else if (w.areaRand.empty() &&
                                 (follow.maxSpeedV != 0.f || follow.accelV != 0.f || follow.rangeV != 0.f ||
                                  follow.easingV != 0.f || follow.steerV != 0.f))
                            entry = "3016 (advanced follow with variance: the game's random values are not in the "
                                    "export)";
                        if (!entry.empty() && std::find(w.approximations.begin(), w.approximations.end(), entry) ==
                                                      w.approximations.end())
                            w.approximations.push_back(entry);
                    }
                }
                if (o.cls == 1 && o.id == ObjId::RotateTrigger && !o.aimExt &&
                    (o.tf(Obj::TF_UseMoveTarget) || o.tf(Obj::TF_DirSnap)) && w.relevantGroup(o.targetGroup)) {
                    bool const dyn = o.tf(Obj::TF_Dynamic) && o.duration != 0.f;
                    if (dyn || o.tf(Obj::TF_UseMoveTarget)) {
                        std::string const entry =
                                dyn ? "1346 (dynamic rotate, old export: m_dynamicModeEasing and limits taken as zero)"
                                    : "1346 (rotate to a target, old export: min / max limits taken as zero)";
                        if (std::find(w.approximations.begin(), w.approximations.end(), entry) ==
                            w.approximations.end())
                            w.approximations.push_back(entry);
                    }
                }
                auto spreadGroup = [&](int gr) {
                    auto const& members = w.allOfGroup(gr);
                    Obj const& a = w.objs[static_cast<size_t>(members[0])];
                    for (size_t k = 1; k < members.size(); ++k) {
                        Obj const& b = w.objs[static_cast<size_t>(members[k])];
                        if ((!o.tpIgnoreX && b.posX != a.posX) || (!o.tpIgnoreY && b.posY != a.posY)) return true;
                    }
                    return false;
                };
                (void)spreadGroup;
                if (w.enable22 && o.cls == 1 && o.id == ObjId::MoveTrigger &&
                    (o.tf(Obj::TF_UseMoveTarget) || o.tf(Obj::TF_DirSnap))) {
                    int const followGroup = o.targetModCenter > 0 ? o.targetModCenter : o.targetGroup;
                    auto ambiguous = [&](int gr) {
                        if (gr <= 0 || w.mainObjectOf(gr) >= 0) return false;
                        auto const& objs = w.allOfGroup(gr);
                        if (objs.size() <= 1) return false;
                        for (size_t k = 1; k < objs.size(); ++k) {
                            if (w.objs[static_cast<size_t>(objs[k])].posX !=
                                        w.objs[static_cast<size_t>(objs[0])].posX ||
                                w.objs[static_cast<size_t>(objs[k])].posY != w.objs[static_cast<size_t>(objs[0])].posY)
                                return true;
                        }
                        return false;
                    };
                    bool const toPlayer = o.tf(Obj::TF_TargetP1) || o.tf(Obj::TF_TargetP2);
                    auto multi = [&](int gr) {
                        return gr > 0 && w.mainObjectOf(gr) < 0 && w.allOfGroup(gr).size() > 1;
                    };
                    (void)ambiguous;
                    if (multi(followGroup) || (!toPlayer && multi(o.centerGroup))) w.hasRandom = true;
                }
                if (o.cls == 1 && o.id == ObjId::RotateTrigger &&
                    (o.tf(Obj::TF_UseMoveTarget) || o.tf(Obj::TF_DirSnap))) {
                    auto multi = [&](int gr) {
                        return gr > 0 && w.mainObjectOf(gr) < 0 && w.allOfGroup(gr).size() > 1;
                    };
                    bool const toPlayer = o.tf(Obj::TF_TargetP1) || o.tf(Obj::TF_TargetP2);
                    if (multi(o.centerGroup != 0 ? o.centerGroup : o.targetGroup) || multi(o.targetGroup) ||
                        (!toPlayer && multi(o.rotTarget)) || multi(o.rotMinX) || multi(o.rotMinY) || multi(o.rotMaxX) ||
                        multi(o.rotMaxY))
                        w.hasRandom = true;
                }
            }
        }
    }

    // groups whose moves can take the fast path (groupFast)
    auto TriggerAnalysis::markFastGroups() -> void {
        {
            std::vector<char> targetable(static_cast<size_t>(w.maxGroup) + 1, 0);
            std::vector<char> afTarget(static_cast<size_t>(w.maxGroup) + 1, 0);
            auto markTargetable = [&](int gr0) {
                forRemapped(gr0, [&](int gr) {
                    if (gr > 0 && gr <= w.maxGroup) targetable[static_cast<size_t>(gr)] = 1;
                });
            };
            for (auto const& o : w.objs) {
                if (o.cls != 1) continue;
                switch (o.id) {
                case 901:
                case 1346:
                case 1347:
                case 1814:
                case 2067:
                case 3016:
                case 3006:
                case 3007:
                case 3008:
                case 3011:
                case 3012:
                case 3013:
                    markTargetable(o.targetGroup);
                    break;
                default:
                    break;
                }
                if (o.id == ObjId::AdvancedFollowTrigger) {
                    forRemapped(o.targetGroup, [&](int gr) {
                        if (gr > 0 && gr <= w.maxGroup) afTarget[static_cast<size_t>(gr)] = 1;
                    });
                }
            }
            for (auto const& anim : w.kfAnims) markTargetable(anim.target);
            w.groupFast.assign(static_cast<size_t>(w.maxGroup) + 1, World::GroupFast{});
            for (int gr = 1; gr <= w.maxGroup; ++gr) {
                auto const& objs = w.objsOfGroup(gr);
                if (objs.size() < 48 || !targetable[static_cast<size_t>(gr)] || afTarget[static_cast<size_t>(gr)])
                    continue;
                World::GroupFast fast;
                std::map<std::vector<short>,
                         std::pair<std::vector<std::pair<float, int>>, std::vector<std::pair<float, int>>>>
                        byMovers;
                for (int idx : objs) {
                    Obj const& o = w.objs[static_cast<size_t>(idx)];
                    bool const simple = !w.mayHaveDyn[static_cast<size_t>(idx)] && !o.lockX && o.posX > 1.0 &&
                                        o.posY > 1.0 && o.posX < 9e6 && o.posY < 9e6 &&
                                        !(o.extendedCollision && o.id != 0x718) &&
                                        !w.centerLike[static_cast<size_t>(idx)];
                    if (!simple) {
                        fast.other.push_back(idx);
                        continue;
                    }
                    std::vector<short> movers;
                    for (short g2 : o.groups) {
                        if (g2 > 0 && g2 <= w.maxGroup && targetable[static_cast<size_t>(g2)]) movers.push_back(g2);
                    }
                    auto& bucket = byMovers[movers];
                    bucket.first.emplace_back(static_cast<float>(std::fmod(o.posX, 100.0)), idx);
                    bucket.second.emplace_back(static_cast<float>(std::fmod(o.posY, 100.0)), idx);
                }
                for (auto& [movers, byPos] : byMovers) {
                    if (byPos.first.size() < 8) {
                        for (auto const& e : byPos.first) fast.other.push_back(e.second);
                        continue;
                    }
                    World::FastBucket bucket;
                    bucket.movers = movers;
                    std::sort(byPos.first.begin(), byPos.first.end());
                    std::sort(byPos.second.begin(), byPos.second.end());
                    for (auto const& e : byPos.first) {
                        bucket.qx.push_back(e.first);
                        bucket.byX.push_back(e.second);
                    }
                    for (auto const& e : byPos.second) {
                        bucket.qy.push_back(e.first);
                        bucket.byY.push_back(e.second);
                    }
                    fast.buckets.push_back(std::move(bucket));
                }
                if (fast.buckets.empty()) continue;
                std::sort(fast.other.begin(), fast.other.end());
                w.groupFast[static_cast<size_t>(gr)] = std::move(fast);
            }
        }
    }

    auto TriggerAnalysis::run() -> void {
        signGroupLists();
        indexGroups();
        sortGroupMembers();
        listSpawnOrder();
        markMovableGroups();
        findCenters();
        listDelaysItemsMenus();
        findKeyframeCenters();
        buildTargetGroups();
        markRotateTargets();
        markAdvancedFollowKeep();
        findGroupMovers();
        buildKeyframes();
        markDynamicObjects();
        buildAreas();
        buildAdvancedFollows();
        markOptimizedKeyframes();
        assignTriggerSlots();
        buildCollisionBlocks();
        findRandomness();
        listEvents();
        listUnsupported();
        markFastGroups();
    }

    void analyzeTriggers(World& w) {
        TriggerAnalysis analysis(w);
        analysis.run();
    }

}
