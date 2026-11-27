// Builds the World from a level file: object parsing, section grid, groups and level settings.

#include <string_view>
#include <cstdio>
#include "world.hpp"
#include <cstdlib>
#include "timeline.hpp"
#include "triggers.hpp"
#include "jsonl.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace ap::sim {

    // brings every object back to its start state and builds the grid, collision index and portal clusters
    void World::build() {
        if (!platformer) startX = 0.f;
        if (spawnGroup > 0) {
            int const mainIdx = mainObjectOf(spawnGroup);
            if (mainIdx >= 0) {
                Obj const& spawnObj = objs[static_cast<size_t>(mainIdx)];
                startX = platformer ? spawnObj.startX : 0.f;
                startY = spawnObj.startY;
            }
        }
        auto obbCorners = [](Obj const& o, float px, float py, float rot, float out[8]) {
            float const angle = -rot * 0.017453292f;
            float const sn = gsin(angle);
            float const cs = gcos(angle);
            float const halfWidth = (o.sws * o.width * o.scaleX) * 0.5f;
            float const halfHeight = (o.shs * o.height * o.scaleY) * 0.5f;
            float const widthX = cs * halfWidth;
            float const widthY = sn * halfWidth;
            float const heightX = -sn * halfHeight;
            float const heightY = cs * halfHeight;
            float const cx = px + o.boxOffX;
            float const cy = py + o.boxOffY;
            float const corners[8] = {cx - widthX - heightX, cy - widthY - heightY, cx + widthX - heightX,
                                    cy + widthY - heightY, cx + widthX + heightX, cy + widthY + heightY,
                                    cx - widthX + heightX, cy - widthY + heightY};
            for (int i = 0; i < 8; ++i) out[i] = corners[i];
        };
        for (auto& o : objs) {
            if (o.rectGenDone) continue;
            o.rectGenDone = true;
            float const px = static_cast<float>(o.posX);
            float const py = static_cast<float>(o.posY);
            Rect const genRect = genericObjRect(o, px, py, o.scaleX, o.scaleY, o.rotationAligned);
            o.rectGen = !o.useOuterOb && genRect.x == o.rect.x && genRect.y == o.rect.y && genRect.w == o.rect.w &&
                        genRect.h == o.rect.h;
            if (o.hasObb) {
                float corners[8];
                obbCorners(o, px, py, o.rotX, corners);
                bool same = true;
                for (int i = 0; i < 8; ++i) {
                    if (corners[i] != o.obb[i]) {
                        same = false;
                        break;
                    }
                }
                o.obbGen = same;
            }
        }
        for (auto& o : objs) {
            double const dx = static_cast<double>(o.startX) - o.posX;
            double const dy = static_cast<double>(o.startY) - o.posY;
            if (std::fabs(dx) < 1e-4 && std::fabs(dy) < 1e-4) continue;
            o.posX = o.startX;
            o.posY = o.startY;
            o.x = o.startX;
            o.y = o.startY;
            float const px = static_cast<float>(o.posX);
            float const py = static_cast<float>(o.posY);
            if (o.rectGen) {
                o.rect = genericObjRect(o, px, py, o.scaleX, o.scaleY, o.rotationAligned);
            } else {
                o.rect.x += static_cast<float>(dx);
                o.rect.y += static_cast<float>(dy);
            }
            if (o.obbGen) {
                obbCorners(o, px, py, o.rotX, o.obb);
            } else {
                for (int i = 0; i < 4; ++i) {
                    o.obb[i * 2] += static_cast<float>(dx);
                    o.obb[i * 2 + 1] += static_cast<float>(dy);
                }
            }
            if (o.useOuterOb && o.obbGen) {
                float x0 = 0.f;
                float x1 = 0.f;
                float y0 = 0.f;
                float y1 = 0.f;
                for (int i = 0; i < 4; ++i) {
                    float const cornerX = o.obb[i * 2];
                    float const cornerY = o.obb[i * 2 + 1];
                    if (!(cornerX <= x1)) x1 = cornerX;
                    if (cornerX < x0 || x0 == 0.f) x0 = cornerX;
                    if (!(cornerY <= y1)) y1 = cornerY;
                    if (cornerY < y0 || y0 == 0.f) y0 = cornerY;
                }
                o.rect = Rect{x0, y0, x1 - x0, y1 - y0};
            }
        }
        for (auto& o : objs) {
            if (!o.hasStartRS) continue;
            o.hasStartRS = false;
            float const oldRot = o.rotX;
            float const newRot = o.startRotX;
            float const scaleRatioX = o.scaleX != 0.f ? o.startScaleX / o.scaleX : 1.f;
            float const scaleRatioY = o.scaleY != 0.f ? o.startScaleY / o.scaleY : 1.f;
            auto off90 = [](float r) {
                float const m = std::fmod(std::fabs(r), 90.f);
                return m > 1e-3f && m < 90.f - 1e-3f;
            };
            (void)scaleRatioX;
            (void)scaleRatioY;
            bool const wasOff = off90(oldRot);
            bool const isOff = off90(newRot);
            o.rot = newRot;
            o.rotX = o.startRotX;
            o.rotY = o.startRotY;
            o.scaleX = o.startScaleX;
            o.scaleY = o.startScaleY;
            o.rotationAligned = newRot == 90.f || newRot == -90.f || newRot == 270.f || newRot == -270.f;
            if (isOff)
                o.useOuterOb = true;
            else if (wasOff)
                o.useOuterOb = false;
            if (o.sws == 0.f) o.sws = 1.f;
            if (o.shs == 0.f) o.shs = 1.f;
            float corners[8];
            obbCorners(o, static_cast<float>(o.posX), static_cast<float>(o.posY), newRot, corners);
            for (int i = 0; i < 8; ++i) o.obb[i] = corners[i];
            o.hasObb = true;
            o.obbGen = true;
            if (o.useOuterOb) {
                float x0 = 0.f;
                float x1 = 0.f;
                float y0 = 0.f;
                float y1 = 0.f;
                for (int i = 0; i < 4; ++i) {
                    float const cornerX = corners[i * 2];
                    float const cornerY = corners[i * 2 + 1];
                    if (!(cornerX <= x1)) x1 = cornerX;
                    if (cornerX < x0 || x0 == 0.f) x0 = cornerX;
                    if (!(cornerY <= y1)) y1 = cornerY;
                    if (cornerY < y0 || y0 == 0.f) y0 = cornerY;
                }
                o.rect = Rect{x0, y0, x1 - x0, y1 - y0};
            } else {
                o.rect = genericObjRect(o, static_cast<float>(o.posX), static_cast<float>(o.posY), o.scaleX, o.scaleY, o.rotationAligned);
                o.rectGen = true;
            }
        }
        for (auto& o : objs) {
            if (!o.obbGen) continue;
            float const angle = -o.rotX * 0.017453292f;
            float const sn = gsin(angle);
            float const cs = gcos(angle);
            float const halfWidth = (o.sws * o.width * o.scaleX) * 0.5f;
            float const halfHeight = (o.shs * o.height * o.scaleY) * 0.5f;
            o.obbHx = cs * halfWidth;
            o.obbHy = sn * halfWidth;
            o.obbVx = -sn * halfHeight;
            o.obbVy = cs * halfHeight;
        }
        for (auto& o : objs) {
            float w = (o.scaleX * o.width) * o.sws;
            float h = (o.scaleY * o.height) * o.shs;
            if (o.rotationAligned) std::swap(w, h);
            o.genHW = w * 0.5f;
            o.genHH = h * 0.5f;
            o.genW = w < 0.f ? 0.f : w;
            o.genH = h < 0.f ? 0.f : h;
        }
        if (!uiDone) {
            uiDone = true;
            std::vector<char> uiGroups;
            auto mainOf = [&](int group) {
                if (group < 1) return -1;
                int found = -1;
                int count = 0;
                for (size_t i = 0; i < objs.size() && count < 2; ++i) {
                    for (short objGroup : objs[i].groups) {
                        if (objGroup == group) {
                            found = static_cast<int>(i);
                            ++count;
                            break;
                        }
                    }
                }
                return count == 1 ? found : -1;
            };
            for (auto const& o : objs)
                if (o.cls == 1 && o.id == ObjId::UiTrigger && o.targetGroup > 0) {
                    bool hasRef = o.centerGroup > 0 && mainOf(o.centerGroup) >= 0;
                    if (!hasRef && o.centerGroup > 0) {
                        for (auto const& parent : objs) {
                            if (!parent.tf(Obj::TF_GroupParent)) continue;
                            for (short objGroup : parent.groups) {
                                if (objGroup == o.centerGroup) {
                                    hasRef = true;
                                    break;
                                }
                            }
                            if (hasRef) break;
                        }
                    }
                    if (!hasRef) continue;
                    if (uiGroups.size() <= static_cast<size_t>(o.targetGroup))
                        uiGroups.resize(static_cast<size_t>(o.targetGroup) + 1, 0);
                    uiGroups[static_cast<size_t>(o.targetGroup)] = 1;
                }
            int uiCount = 0;
            if (!uiGroups.empty())
                for (auto& o : objs) {
                    bool const positional =
                            (o.cls == 1 && o.trigger && !o.touchTriggered) || (o.type == Modifier && !o.touchTriggered);
                    if (positional || o.type == Decoration || o.type == CollisionObject || o.type == Special) continue;
                    bool inUiGroup = false;
                    for (short group : o.groups) {
                        if (group > 0 && static_cast<size_t>(group) < uiGroups.size() &&
                            uiGroups[static_cast<size_t>(group)]) {
                            inUiGroup = true;
                            break;
                        }
                    }
                    if (!inUiGroup) continue;
                    o.uiObject = true;
                    o.uiType = o.type;
                    o.type = Decoration;
                    ++uiCount;
                }
            uiObjects = uiCount;
        }
        cells.clear();
        extended.clear();
        uidMax = 0;
        checkedItems.assign(10000, 0);
        for (auto const& o : objs) {
            if (o.cls != 1) continue;
            auto mark = [&](int item) {
                if (item >= 0 && item < 10000) checkedItems[static_cast<size_t>(item)] = 1;
            };
            if (o.id == ObjId::CountTrigger || o.id == ObjId::InstantCountTrigger) mark(o.itemID);
            if (o.id == ObjId::ItemCompareTrigger) {
                int firstType = static_cast<int>(prop(o, 476));
                if (firstType < 1) firstType = 1;
                if (firstType == 1) mark(o.itemID);
                if (static_cast<int>(prop(o, 477)) == 1) mark(o.itemID2);
            }
        }
        bool const useTimeline = timeline && !triggersSimulable();
        for (auto const& o : objs) uidMax = std::max(uidMax, o.uid);
        byUid.assign(static_cast<size_t>(uidMax) + 1, -1);
        for (size_t i = 0; i < objs.size(); ++i) {
            auto const& o = objs[i];
            if (o.uid >= 0) byUid[static_cast<size_t>(o.uid)] = static_cast<int>(i);
            if (o.type == Decoration) continue;
            if (o.id != 0x718 && o.extendedCollision) {
                extended.push_back(static_cast<int>(i));
                continue;
            }
            if (useTimeline && i < timeline->dynOf.size() && timeline->dynOf[i] >= 0) continue;
            int const cx = cellOf(o.posX);
            int const cy = cellYOf(o.posY);
            if (cells.size() <= static_cast<size_t>(cx)) cells.resize(static_cast<size_t>(cx) + 1);
            auto& column = cells[static_cast<size_t>(cx)];
            if (column.size() <= static_cast<size_t>(cy)) column.resize(static_cast<size_t>(cy) + 1);
            column[static_cast<size_t>(cy)].push_back(static_cast<int>(i));
        }
        canPruneBehind = !platformer;
        for (auto const& o : objs) {
            if (!canPruneBehind) break;
            if (o.cls == 1 && o.id == ObjId::RotateGameplayTrigger) {
                canPruneBehind = false;
                break;
            }
            bool const isTeleport = o.type == TeleportPortal || o.type == TeleportOrb ||
                                    (o.cls == 1 && (o.id == ObjId::TeleportTrigger || o.id == ObjId::TeleportArea));
            if (!isTeleport) continue;
            auto backTarget = [&](Obj const& target) { return target.x < o.x - 300.f; };
            if (o.tpOrange) {
                int const targetIdx = indexOfUid(o.tpOrange);
                if (targetIdx < 0 || backTarget(objs[static_cast<size_t>(targetIdx)])) canPruneBehind = false;
            } else if (o.targetGroup > 0) {
                for (int targetIdx : allOfGroup(o.targetGroup)) {
                    if (backTarget(objs[static_cast<size_t>(targetIdx)])) {
                        canPruneBehind = false;
                        break;
                    }
                }
            } else if (o.type != TeleportPortal)
                canPruneBehind = false;
        }
        for (auto& column : cells)
            for (auto& cell : column)
                std::sort(cell.begin(), cell.end(), [&](int a, int b) {
                    return static_cast<unsigned>(objs[static_cast<size_t>(a)].uid) < static_cast<unsigned>(objs[static_cast<size_t>(b)].uid);
                });
        auto buildIndex = [&](std::vector<int> const& list, CellIndex& index) {
            index.bands.clear();
            index.mov.clear();
            index.teleporter = false;
            for (int idx : list) {
                Obj const& o = objs[static_cast<size_t>(idx)];
                float minY = o.rect.y;
                float maxY = o.rect.y + o.rect.h;
                if (o.type == Slope) {
                    float slopeW = o.scaleX * o.width * 2.f;
                    float slopeH = o.scaleY * o.height * 2.f;
                    if (o.rotationAligned) std::swap(slopeW, slopeH);
                    float const centerY = static_cast<float>(o.posY) + o.boxOffY;
                    float const bottomY = centerY - slopeH * 0.5f;
                    float const height = std::max(slopeH, 0.f);
                    minY = std::min(minY - o.rect.h * 0.5f, bottomY);
                    maxY = std::max(maxY + o.rect.h * 0.5f, bottomY + height);
                }
                if (o.radius > 0.f) {
                    float radius = o.scaleX;
                    if (radius == 1.f && o.scaleY == 1.f) {
                        radius = o.radius;
                    } else {
                        if (radius <= o.scaleY) radius = o.scaleY;
                        radius = radius * o.radius;
                    }
                    float const circleY = static_cast<float>(o.posY);
                    float const reach = std::fabs(radius) + 1.f;
                    minY = std::min(minY, circleY - reach);
                    maxY = std::max(maxY, circleY + reach);
                }
                bool const dyn = useTimeline && static_cast<size_t>(idx) < timeline->dynOf.size() && timeline->dynOf[static_cast<size_t>(idx)] >= 0;
                bool const isTeleport = o.type == TeleportPortal || o.id == ObjId::TeleportTrigger;
                if (o.movable || dyn) {
                    MovEntry entry;
                    entry.idx = idx;
                    entry.uid = static_cast<unsigned>(o.uid);
                    entry.minY = minY;
                    entry.maxY = maxY;
                    entry.dyn = dyn;
                    entry.teleporter = isTeleport;
                    entry.ngroups = 0;
                    for (short group : o.groups)
                        if (entry.ngroups < 10) entry.groups[entry.ngroups++] = group;
                    index.mov.push_back(entry);
                    continue;
                }
                if (isTeleport) index.teleporter = true;
                int firstBand = static_cast<int>(std::floor(minY / CellIndex::kBandH));
                int lastBand = static_cast<int>(std::floor(maxY / CellIndex::kBandH));
                if (firstBand < 0) firstBand = 0;
                if (lastBand < firstBand) lastBand = firstBand;
                if (lastBand > 4095) lastBand = 4095;
                if (static_cast<size_t>(lastBand) >= index.bands.size())
                    index.bands.resize(static_cast<size_t>(lastBand) + 1);
                for (int band = firstBand; band <= lastBand; ++band)
                    index.bands[static_cast<size_t>(band)].push_back(
                            BandEntry{idx, static_cast<unsigned>(o.uid), minY, maxY});
            }
            index.computeSorted();
        };
        cellIndex.assign(cells.size(), {});
        for (size_t cx = 0; cx < cells.size(); ++cx) {
            cellIndex[cx].resize(cells[cx].size());
            for (size_t cy = 0; cy < cells[cx].size(); ++cy) buildIndex(cells[cx][cy], cellIndex[cx][cy]);
        }
        buildIndex(extended, extendedIndex);
        portalPassX.clear();
        for (size_t i = 0; i < objs.size(); ++i) {
            auto const& o = objs[i];
            if (o.type == Decoration || o.disabled || o.groupDisabled) continue;
            if (!o.isPortal() && !o.isSpeedPortal()) continue;
            if (useTimeline && i < timeline->dynOf.size() && timeline->dynOf[i] >= 0) continue;
            portalPassX.emplace_back(o.rect.x + o.rect.w + 15.f, static_cast<int>(i));
        }
        std::sort(portalPassX.begin(), portalPassX.end());
        portalClusters.clear();
        clusterPassX.clear();
        auto family = [](Obj const& o) -> int {
            if (o.isSpeedPortal()) return 1;
            switch (o.type) {
            case ShipPortal:
            case CubePortal:
            case BallPortal:
            case UfoPortal:
            case WavePortal:
            case RobotPortal:
            case SpiderPortal:
            case SwingPortal:
                return 2;
            case InverseGravityPortal:
            case NormalGravityPortal:
            case GravityTogglePortal:
                return 3;
            case RegularSizePortal:
            case MiniSizePortal:
                return 4;
            case InverseMirrorPortal:
            case NormalMirrorPortal:
                return 5;
            case DualPortal:
            case SoloPortal:
                return 6;
            default:
                return 0;
            }
        };
        struct Cand {
            float cx;
            int fam;
            int idx;
        };
        std::vector<Cand> candidates;
        for (auto const& e : portalPassX) {
            Obj const& o = objs[static_cast<size_t>(e.second)];
            candidates.push_back(Cand{o.rect.x + o.rect.w * 0.5f, family(o), e.second});
        }
        std::sort(candidates.begin(), candidates.end(), [](Cand const& a, Cand const& b) { return a.fam != b.fam ? a.fam < b.fam : a.cx < b.cx; });
        float lastCx = 0.f;
        int lastFam = -1;
        for (auto const& c : candidates) {
            Obj const& o = objs[static_cast<size_t>(c.idx)];
            bool const join = c.fam != 0 && c.fam == lastFam && !portalClusters.empty() && c.cx - lastCx <= 45.f;
            if (!join) {
                portalClusters.emplace_back();
                auto& cluster = portalClusters.back();
                cluster.speed = c.fam == 1;
                cluster.yLo = o.rect.y;
                cluster.yHi = o.rect.y + o.rect.h;
            }
            auto& cluster = portalClusters.back();
            cluster.members.push_back(c.idx);
            if (hasTriggerData && o.movable) cluster.dyn = true;
            if (c.fam == 2 && o.type != WavePortal) cluster.modeOut = true;
            cluster.passX = std::max(cluster.passX, o.rect.x + o.rect.w + 15.f);
            cluster.approachX = std::min(cluster.approachX, o.rect.x - 15.f);
            cluster.yLo = std::min(cluster.yLo, o.rect.y);
            cluster.yHi = std::max(cluster.yHi, o.rect.y + o.rect.h);
            lastCx = c.cx;
            lastFam = c.fam;
        }
        for (size_t k = 0; k < portalClusters.size(); ++k)
            clusterPassX.emplace_back(portalClusters[k].passX, static_cast<int>(k));
        std::sort(clusterPassX.begin(), clusterPassX.end());
    }

    namespace {
        //sprite width / height scale from the exported rect when the export doesn't have them
        void deriveSpriteScales(Obj& o) {
            if (o.sws > 0.f && o.shs > 0.f) return;
            float const scaledW = std::fabs(o.scaleX) * o.width;
            float const scaledH = std::fabs(o.scaleY) * o.height;
            float widthScale = 1.f;
            float heightScale = 1.f;
            if (!o.rotationAligned) {
                if (scaledW > 0.f) widthScale = o.rect.w / scaledW;
                if (scaledH > 0.f) heightScale = o.rect.h / scaledH;
            } else {
                if (scaledW > 0.f) widthScale = o.rect.h / scaledW;
                if (scaledH > 0.f) heightScale = o.rect.w / scaledH;
            }
            if (!(widthScale > 0.f) || !std::isfinite(widthScale)) widthScale = 1.f;
            if (!(heightScale > 0.f) || !std::isfinite(heightScale)) heightScale = 1.f;
            o.sws = widthScale;
            o.shs = heightScale;
        }
    }

    // object ids whose raw properties are kept
    bool wantsExtProps(int id) {
        switch (id) {
        case 3607:
        case 3609:
        case 3614:
        case 3615:
        case 3617:
        case 3618:
        case 3619:
        case 3620:
        case 3640:
        case 3641:
        case 3643:
        case 3032:
        case 3033:
        case 2067:
        case 3016:
        case 3660:
        case 3661:
        case 1912:
        case 2068:
        case 3655:
        case 3604:
        case 1914:
        case 2062:
        case 2925:
        case 2901:
        case 2015:
        case 12:
        case 13:
        case 47:
        case 111:
        case 660:
        case 745:
        case 1331:
        case 1933:
        case 286:
        case 287:
            return true;
        default:
            return id >= 3006 && id <= 3017;
        }
    }

    namespace {
        // keys whose value is a list
        bool isListKey(int key) {
            return key == 57 || key == 435 || key == 442 || key == 274 || key == 430 || key == 152;
        }
    }

    // "1,3620,2,345,..." to ExtProps, list keys use '.' as separator
    ExtProps parseExtProps(std::string_view save) {
        ExtProps props;
        size_t pos = 0;
        auto next = [&](std::string_view& token) {
            if (pos > save.size()) return false;
            size_t comma = save.find(',', pos);
            if (comma == std::string_view::npos) comma = save.size();
            token = save.substr(pos, comma - pos);
            pos = comma + 1;
            return true;
        };
        std::string_view keyText;
        std::string_view valueText;
        while (next(keyText) && next(valueText)) {
            int key = 0;
            bool valid = !keyText.empty();
            for (char c : keyText) {
                if (c < '0' || c > '9') {
                    valid = false;
                    break;
                }
                key = key * 10 + (c - '0');
            }
            if (!valid) continue;
            std::string value(valueText);
            if (isListKey(key)) {
                std::vector<double> list;
                size_t start = 0;
                while (start <= value.size()) {
                    size_t dot = value.find('.', start);
                    if (dot == std::string::npos) dot = value.size();
                    if (dot > start)
                        list.push_back(std::strtod(value.substr(start, dot - start).c_str(), nullptr));
                    else if (start != value.size())
                        list.push_back(0.0);
                    start = dot + 1;
                }
                props.lists.emplace_back(key, std::move(list));
            } else {
                char* end = nullptr;
                double const number = std::strtod(value.c_str(), &end);
                if (end != value.c_str()) props.kv.emplace_back(key, number);
            }
        }
        return props;
    }

    // ExtProps back to a save string (used by the mod export)
    std::string formatExtProps(ExtProps const& props) {
        std::string out;
        char buf[64];
        for (auto const& [key, value] : props.kv) {
            std::snprintf(buf, sizeof buf, "%s%d,%.17g", out.empty() ? "" : ",", key, value);
            out += buf;
        }
        for (auto const& [key, list] : props.lists) {
            std::snprintf(buf, sizeof buf, "%s%d,", out.empty() ? "" : ",", key);
            out += buf;
            for (size_t j = 0; j < list.size(); ++j) {
                std::snprintf(buf, sizeof buf, "%s%.17g", j ? "." : "", list[j]);
                out += buf;
            }
        }
        return out;
    }

    // loads a Recorder export (levels/<level>.objects.jsonl), fills err on failure
    bool loadLevel(std::string const& path, World& world, std::string* err) {
        std::ifstream file(path);
        if (!file.is_open()) {
            if (err) *err = "cannot open " + path;
            return false;
        }
        std::string line;
        int lineNo = 0;
        std::vector<std::pair<int, int>> parentUids;
        while (std::getline(file, line)) {
            ++lineNo;
            if (line.empty()) continue;
            jsonl::Value json;
            if (!jsonl::parseLine(line, json)) {
                if (err) *err = "invalid JSON line " + std::to_string(lineNo);
                return false;
            }
            if (json.strOr("ev") == "level") {
                world.levelName = json.strOr("level");
                world.levelId = json.intOr("levelId");
                world.length = static_cast<float>(json.numOr("length"));
                world.groundP1 = static_cast<float>(json.numOr("groundP1", 105.0));
                world.groundShip = static_cast<float>(json.numOr("groundShip", 0.0));
                world.minPortalY = static_cast<float>(json.numOr("minPortalY"));
                world.maxPortalY = static_cast<float>(json.numOr("maxPortalY"));
                world.platformer = json.boolOr("platformer");
                world.maxGameplayY = static_cast<float>(json.numOr("maxGameplayY", 0.0));
                world.winW = static_cast<float>(json.numOr("winW", 0.0));
                world.winH = static_cast<float>(json.numOr("winH", 0.0));
                world.hasEndPortal = json.boolOr("endPortal", !world.platformer);
                world.camClampA = json.boolOr("kA23");
                world.camClampB = json.boolOr("kA24");
                world.camUnk322a = json.boolOr("unk322a");
                world.camTestMode = json.boolOr("testMode");
                world.twoPlayer = json.boolOr("twoPlayer");
                world.gridSnapOff = json.boolOr("gridSnapOff");
                world.enable22 = json.boolOr("enable22", true);
                world.ySection = json.boolOr("ySection", world.platformer);
                world.startMode = json.intOr("startMode");
                world.startSpeed = json.intOr("startSpeed");
                world.startMini = json.boolOr("startMini");
                world.startDual = json.boolOr("startDual");
                world.rotateGameplay = json.boolOr("rotateGameplay");
                world.startFlipped = json.boolOr("startFlip");
                world.startReverse = json.boolOr("startRev");
                world.spawnGroup = json.intOr("spawnGroup");
                world.fixRobotJump = json.boolOr("fixRobotJump");
                world.decreaseBoostSlide = json.boolOr("decreaseBoostSlide");
                world.allowMultiRotation = json.boolOr("multiRot");
                world.reverseSync = json.boolOr("reverseSync");
                world.enablePlayerSqueeze = json.boolOr("enablePlayerSqueeze");
                world.fixGravityBug = json.boolOr("fixGravityBug");
                world.enableImpulseFix = json.boolOr("enableImpulseFix");
                world.fixRadiusCollision = json.boolOr("fixRadius");
                world.fixNegativeScale = json.boolOr("fixNegScale");
                world.allowStaticRotate = json.boolOr("staticRotate");
                world.sortGroups = json.boolOr("sortGroups");
                world.areaParExported = json.boolOr("areaPar");
                world.moveOptExported = json.boolOr("mopt");
                world.startX = static_cast<float>(json.numOr("startX", 0.0));
                world.startY = static_cast<float>(json.numOr("startY", 105.0));
                world.hasTriggerData = json.boolOr("trig", false) || json.intOr("trig") != 0;
                continue;
            }
            if (json.strOr("ev") == "parents") {
                if (auto* pairs = json.get("p"); pairs && pairs->kind == jsonl::Value::Array)
                    for (auto const& e : pairs->arr) {
                        if (e.kind != jsonl::Value::Array || e.arr.size() < 2) continue;
                        int const group = static_cast<int>(e.arr[0].num);
                        int const uid = static_cast<int>(e.arr[1].num);
                        if (group < 0 || group > 9999) continue;
                        if (world.groupParent.size() <= static_cast<size_t>(group))
                            world.groupParent.resize(static_cast<size_t>(group) + 1, -1);
                        parentUids.emplace_back(group, uid);
                    }
                continue;
            }
            if (json.strOr("ev") == "arearand") {
                if (auto* values = json.get("v"); values && values->kind == jsonl::Value::Array) {
                    world.areaRand.clear();
                    for (auto const& e : values->arr) world.areaRand.push_back(static_cast<float>(e.num));
                }
                continue;
            }
            if (json.strOr("ev") != "") continue;

            Obj o;
            o.uid = json.intOr("uid");
            o.id = json.intOr("id");
            o.type = json.intOr("type");
            o.cls = json.intOr("cls");
            o.x = static_cast<float>(json.numOr("px"));
            o.y = static_cast<float>(json.numOr("py"));
            o.posX = json.numOr("x");
            o.posY = json.numOr("y");
            o.rot = static_cast<float>(json.numOr("rot"));
            o.rotX = static_cast<float>(json.numOr("rx"));
            o.rotY = static_cast<float>(json.numOr("ry"));
            o.scaleX = static_cast<float>(json.numOr("sx", 1));
            o.scaleY = static_cast<float>(json.numOr("sy", 1));
            if (auto* rect = json.get("rect"); rect && rect->arr.size() == 4) {
                o.rect = {static_cast<float>(rect->arr[0].num), static_cast<float>(rect->arr[1].num), static_cast<float>(rect->arr[2].num), static_cast<float>(rect->arr[3].num)};
            }
            o.width = static_cast<float>(json.numOr("w"));
            o.height = static_cast<float>(json.numOr("h"));
            if (auto* box = json.get("box"); box && box->arr.size() == 2) {
                o.boxOffX = static_cast<float>(box->arr[0].num);
                o.boxOffY = static_cast<float>(box->arr[1].num);
            }
            o.radius = static_cast<float>(json.numOr("rad"));
            if (auto* corners = json.get("obb"); corners && corners->arr.size() == 8) {
                o.hasObb = true;
                for (int i = 0; i < 8; ++i) o.obb[i] = static_cast<float>(corners->arr[static_cast<size_t>(i)].num);
            }
            o.slopeDir = json.intOr("sdir");
            unsigned flags = static_cast<unsigned>(json.numOr("f"));
            auto bit = [&](int i) { return (flags >> i) & 1u; };
            o.disabled = false;
            (void)bit(0);
            o.extendedCollision = bit(1);
            o.trigger = bit(2);
            o.passable = bit(3);
            o.noTouch = bit(4);
            o.decoration = bit(5);
            o.hide = bit(6);
            o.gripSlope = bit(7);
            o.slopeUphill = bit(8);
            o.slopeHazard = bit(9);
            o.flipX = bit(10);
            o.flipY = bit(11);
            o.invisibleBlock = bit(12);
            o.groupDisabled = bit(13);
            o.rotationAligned = bit(15);
            o.activated = bit(17);
            o.iceBlock = bit(19);
            o.dontBoostY = bit(20);
            o.dontBoostX = bit(21);
            o.extraSticky = bit(22);
            o.nonStickX = bit(23);
            o.nonStickY = bit(24);
            o.scaleStick = bit(25);
            o.highDetail = bit(26);
            o.useOuterOb = bit(27);
            o.hasNoEffects = bit(30);
            o.claimTouch = bit(31);
            o.ringSpawnOnly = json.boolOr("rso");
            o.areaRi = static_cast<short>(json.intOr("ri", -1));
            o.material = static_cast<short>(json.intOr("mat"));
            o.channel = json.intOr("ch");
            if (auto* groupList = json.get("g"))
                for (auto const& e : groupList->arr) o.groups.push_back(static_cast<short>(e.num));
            if (auto* sections = json.get("sec"); sections && sections->arr.size() == 3) {
                o.secOuter = static_cast<int>(sections->arr[0].num);
                o.secMiddle = static_cast<int>(sections->arr[1].num);
                o.secInner = static_cast<int>(sections->arr[2].num);
            }
            o.targetGroup = json.intOr("tgt");
            o.duration = static_cast<float>(json.numOr("dur"));
            o.touchTriggered = json.boolOr("touch");
            o.spawnTriggered = json.boolOr("spawn");
            o.multiTriggered = json.boolOr("multi");
            o.multiActivate = json.boolOr("multiAct");
            o.reverse = json.boolOr("rev");
            o.singlePTouch = json.boolOr("single");
            if (auto* startPos = json.get("sp"); startPos && startPos->arr.size() == 2) {
                o.startX = static_cast<float>(startPos->arr[0].num);
                o.startY = static_cast<float>(startPos->arr[1].num);
            } else {
                o.startX = o.x;
                o.startY = o.y;
            }
            if (auto* startRotScale = json.get("srs"); startRotScale && startRotScale->arr.size() == 4) {
                o.hasStartRS = true;
                o.startRotX = static_cast<float>(startRotScale->arr[0].num);
                o.startRotY = static_cast<float>(startRotScale->arr[1].num);
                o.startScaleX = static_cast<float>(startRotScale->arr[2].num);
                o.startScaleY = static_cast<float>(startRotScale->arr[3].num);
            } else {
                o.startRotX = o.rotX;
                o.startRotY = o.rotY;
                o.startScaleX = o.scaleX;
                o.startScaleY = o.scaleY;
            }
            o.tpOrange = json.intOr("tp");
            o.tpYOffset = static_cast<float>(json.numOr("tpy"));
            o.tpYellow = json.boolOr("tpyellow");
            o.tpGravity = json.intOr("tpgrav");
            o.tpSaveOffset = json.boolOr("tpsave");
            o.tpIgnoreX = json.boolOr("tpix");
            o.tpIgnoreY = json.boolOr("tpiy");
            o.tpStaticForce = json.boolOr("tpsf");
            o.tpStaticForceV = static_cast<float>(json.numOr("tpsfv"));
            o.tpStaticForceAdd = json.boolOr("tpsfa");
            o.tpRedirectDash = json.boolOr("tprd");
            o.tpRedirectForce = json.boolOr("tprf");
            o.tpSnapGround = json.boolOr("tpsg");
            o.tpRedirectMod = static_cast<float>(json.numOr("tprfm", 1.0));
            o.tpRedirectMin = static_cast<float>(json.numOr("tprfmin"));
            o.tpRedirectMax = static_cast<float>(json.numOr("tprfmax"));
            o.tpEase = json.boolOr("tpease");
            o.tpInstantCam = json.boolOr("tpic");
            if (auto* forceBlock = json.get("fb"); forceBlock && forceBlock->arr.size() == 6) {
                o.force = static_cast<float>(forceBlock->arr[0].num);
                o.forceMin = static_cast<float>(forceBlock->arr[1].num);
                o.forceMax = static_cast<float>(forceBlock->arr[2].num);
                o.forceRelative = forceBlock->arr[3].num != 0;
                o.forceRange = forceBlock->arr[4].num != 0;
                o.forceKey = static_cast<int>(forceBlock->arr[5].num);
            }
            o.dashSpeed = static_cast<float>(json.numOr("dashSpeed"));
            o.endBoost = static_cast<float>(json.numOr("endBoost"));
            o.maxDuration = static_cast<float>(json.numOr("maxDur"));
            o.allowCollide = json.boolOr("allowCollide");
            o.stopSlide = json.boolOr("stopSlide");
            if ((o.type == DashRing || o.type == GravityDashRing) && !(o.dashSpeed > 0.f)) o.dashSpeed = 1.f;
            o.centerGroup = json.intOr("ctr");
            if (auto* moveOffset = json.get("mv"); moveOffset && moveOffset->arr.size() == 2) {
                o.mvX = static_cast<float>(moveOffset->arr[0].num);
                o.mvY = static_cast<float>(moveOffset->arr[1].num);
            }
            o.easing = json.intOr("ease");
            o.easingRate = static_cast<float>(json.numOr("rate"));
            o.moveTargetMode = json.intOr("mtm");
            o.targetModCenter = json.intOr("tmc");
            o.moveModX = static_cast<float>(json.numOr("mmx"));
            o.moveModY = static_cast<float>(json.numOr("mmy"));
            o.dirDistance = static_cast<float>(json.numOr("dmd"));
            o.rotDeg = static_cast<float>(json.numOr("rdeg"));
            o.times360 = json.intOr("t360");
            o.rotTarget = json.intOr("rtid");
            o.rotOffset = static_cast<float>(json.numOr("roff"));
            if (auto* aim = json.get("aim"); aim && aim->kind == jsonl::Value::Array && aim->arr.size() == 5) {
                o.rotMinX = static_cast<int>(aim->arr[0].num);
                o.rotMinY = static_cast<int>(aim->arr[1].num);
                o.rotMaxX = static_cast<int>(aim->arr[2].num);
                o.rotMaxY = static_cast<int>(aim->arr[3].num);
                o.dynEasing = static_cast<int>(aim->arr[4].num);
                o.aimExt = true;
            }
            o.followXMod = static_cast<float>(json.numOr("fxm"));
            o.followYMod = static_cast<float>(json.numOr("fym"));
            o.fySpeed = static_cast<float>(json.numOr("fys"));
            o.fyDelay = static_cast<float>(json.numOr("fyd"));
            o.fyOffset = json.intOr("fyo");
            o.fyMaxSpeed = static_cast<float>(json.numOr("fymax"));
            o.touchToggleMode = json.intOr("ttm");
            o.touchPlayerMode = json.intOr("tpm");
            o.itemID = json.intOr("item");
            o.itemID2 = json.intOr("item2");
            o.controlID = json.intOr("ctl");
            o.zoom = static_cast<float>(json.numOr("zoom"));
            o.timeWarpMod = static_cast<float>(json.numOr("tw"));
            o.spawnDelay = static_cast<float>(json.numOr("sdel"));
            o.spawnDelayRange = static_cast<float>(json.numOr("sdr"));
            o.count = json.intOr("cnt");
            o.compare = json.intOr("cmp");
            o.countMult = static_cast<float>(json.numOr("cmul"));
            o.stopCmd = json.intOr("stop");
            o.gravityValue = static_cast<float>(json.numOr("grav"));
            o.optFlags = json.intOr("opt");
            o.optValues = json.get("optv") ? json.intOr("optv") : -1;
            if (auto* posOffset = json.get("po"); posOffset && posOffset->arr.size() == 2) {
                o.posOffX = static_cast<float>(posOffset->arr[0].num);
                o.posOffY = static_cast<float>(posOffset->arr[1].num);
            }
            o.sws = static_cast<float>(json.numOr("sws"));
            o.shs = static_cast<float>(json.numOr("shs"));
            o.tflags = static_cast<uint32_t>(json.numOr("tf"));
            o.collectPoints = json.intOr("cpts");
            o.animOnTrigger = json.boolOr("aot");
            o.linkedGroup = json.intOr("lnk");
            o.moveOpt = json.boolOr("mopt");
            o.ord = json.intOr("ord");
            o.rgMove = json.intOr("rgm");
            o.rgGround = json.intOr("rgg");
            o.rgFlags = json.intOr("rgf");
            o.rgChannel = json.intOr("rgc");
            o.camFlags = json.intOr("cam");
            if (auto* remap = json.get("remap"); remap && remap->arr.size() >= 2) {
                if (world.spawnRemaps.empty()) world.spawnRemaps.emplace_back();
                std::vector<std::pair<int, int>> table;
                for (size_t k = 0; k + 1 < remap->arr.size(); k += 2) {
                    int const a = static_cast<int>(remap->arr[k].num);
                    int const b = static_cast<int>(remap->arr[k + 1].num);
                    bool duplicate = false;
                    for (auto& e : table) {
                        if (e.first == a) {
                            e.second = b;
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) table.emplace_back(a, b);
                }
                world.spawnRemaps.push_back(std::move(table));
                o.remapIdx = static_cast<int>(world.spawnRemaps.size()) - 1;
            }
            o.rgModX = static_cast<float>(json.numOr("rgx"));
            o.rgModY = static_cast<float>(json.numOr("rgy"));
            if (auto* saveString = json.get("kv");
                saveString && saveString->kind == jsonl::Value::String && !saveString->str.empty()) {
                world.ext.push_back(parseExtProps(saveString->str));
                o.ext = static_cast<int>(world.ext.size()) - 1;
            }
            if (auto* text = json.get("txt"); text && text->kind == jsonl::Value::String && !text->str.empty())
                world.texts.emplace_back(static_cast<int>(world.objs.size()), text->str);
            deriveSpriteScales(o);
            world.objs.push_back(std::move(o));
        }
        world.build();
        {
            int maxGroupId = 0;
            for (auto const& o : world.objs)
                for (short group : o.groups) maxGroupId = std::max(maxGroupId, static_cast<int>(group));
            world.exportParents.assign(static_cast<size_t>(maxGroupId) + 1, -1);
            for (auto const& [group, uid] : parentUids) {
                int const idx = world.indexOfUid(uid);
                if (idx >= 0 && group >= 0 && group <= maxGroupId)
                    world.exportParents[static_cast<size_t>(group)] = idx;
            }
        }
        analyzeTriggers(world);
        world.build();
        return true;
    }

}
