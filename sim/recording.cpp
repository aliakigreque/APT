// Reads the recordings written by the F6 recorder.

#include "recording.hpp"
#include "jsonl.hpp"

#include <fstream>

namespace ap::sim {

    // reads the file line by line: "start", "death", "complete", "obj", "end" events, or a player row
    bool loadRecording(std::string const& path, Recording& rec, std::string* err) {
        std::ifstream file(path);
        if (!file.is_open()) {
            if (err) *err = "cannot open " + path;
            return false;
        }
        std::string line;
        int lineNo = 0;
        while (std::getline(file, line)) {
            ++lineNo;
            if (line.empty()) continue;
            jsonl::Value v;
            if (!jsonl::parseLine(line, v)) {
                if (err) *err = "invalid JSON line " + std::to_string(lineNo);
                return false;
            }
            std::string event = v.strOr("ev");
            if (event == "start") {
                rec.level = v.strOr("level");
                rec.levelId = v.intOr("levelId");
                rec.attempt = v.intOr("attempt");
                rec.length = static_cast<float>(v.numOr("length"));
                rec.dual = v.boolOr("dual");
                rec.practice = v.boolOr("practice");
                rec.platformer = v.boolOr("platformer");
                rec.cbs = v.boolOr("cbs");
                rec.cos = v.boolOr("cos");
                rec.startX = static_cast<float>(v.numOr("startX"));
                rec.startY = static_cast<float>(v.numOr("startY"));
                rec.groundP1 = static_cast<float>(v.numOr("groundP1", 105.0));
                rec.gravity = v.numOr("gravity");
                rec.yStart = v.numOr("yStart");
                rec.speed = static_cast<float>(v.numOr("speed"));
                rec.vehicleSize = static_cast<float>(v.numOr("vehicleSize", 1.0));
                rec.objWin = static_cast<float>(v.numOr("objWin", 0.0));
                if (v.get("p2snap")) {
                    rec.p2l.valid = true;
                    rec.p2l.snapDistance = v.numOr("p2snap");
                    rec.p2l.isOnGround3 = v.intOr("p2og3") != 0;
                    rec.p2l.maybeIsFalling = v.intOr("p2mf") != 0;
                    rec.p2l.padRingRelated = v.intOr("p2prr") != 0;
                    rec.p2l.fallStartY = static_cast<float>(v.numOr("p2fsy"));
                }
                if (v.get("rng")) {
                    rec.hasRng = true;
                    rec.rng = static_cast<uint32_t>(v.numOr("rng"));
                }
                if (v.get("gatt")) rec.gameAttempts = v.intOr("gatt");
                if (v.get("mp")) rec.modPlays = v.intOr("mp");
                if (v.get("rng0")) {
                    rec.hasRng0 = true;
                    rec.rng0 = static_cast<uint32_t>(v.numOr("rng0"));
                }
                if (v.get("tprng")) {
                    rec.hasTpRng = true;
                    rec.tpRng = static_cast<uint32_t>(v.numOr("tprng"));
                }
                if (v.get("rngs")) {
                    rec.hasRngSeed = true;
                    rec.rngSeed = static_cast<uint32_t>(v.numOr("rngs"));
                }
                if (auto const* items = v.get("pitems")) {
                    rec.hasPersist = true;
                    for (size_t i = 0; i + 1 < items->arr.size(); i += 2)
                        rec.persistentItems.emplace_back(static_cast<int>(items->arr[i].num),
                                                         static_cast<int>(items->arr[i + 1].num));
                }
                if (auto const* pre = v.get("pre"))
                    for (size_t i = 0; i + 1 < pre->arr.size(); i += 2)
                        rec.preInputs.emplace_back(static_cast<int>(pre->arr[i].num),
                                                   static_cast<uint8_t>(pre->arr[i + 1].num));
                if (auto const* timers = v.get("ptimers"))
                    for (auto const& e : timers->arr) rec.persistentTimers.push_back(static_cast<int>(e.num));
                continue;
            }
            if (event == "death") {
                rec.deathTick = v.intOr("t");
                rec.deathKiller = v.intOr("killer");
                rec.deathKillerId = v.intOr("killerId");
                rec.deathX = static_cast<float>(v.numOr("x"));
                rec.deathY = static_cast<float>(v.numOr("y"));
                rec.deathHeld = v.get("h") ? v.intOr("h") : -1;
                continue;
            }
            if (event == "complete") {
                rec.completeTick = v.intOr("t");
                continue;
            }
            if (event == "obj") {
                int const t = v.intOr("t");
                if (auto* arr = v.get("o"); arr && arr->kind == jsonl::Value::Array) {
                    for (auto const& e : arr->arr) {
                        if (e.kind != jsonl::Value::Array || e.arr.size() < 5) continue;
                        TimelineEvent objEvent;
                        objEvent.tick = t;
                        objEvent.uid = static_cast<int>(e.arr[0].num);
                        objEvent.x = static_cast<float>(e.arr[1].num);
                        objEvent.y = static_cast<float>(e.arr[2].num);
                        objEvent.rot = static_cast<float>(e.arr[3].num);
                        objEvent.flags = static_cast<uint8_t>(e.arr[4].num);
                        objEvent.nx = objEvent.x;
                        objEvent.ny = objEvent.y;
                        if (e.arr.size() >= 7) {
                            objEvent.nx = static_cast<float>(e.arr[5].num);
                            objEvent.ny = static_cast<float>(e.arr[6].num);
                        }
                        rec.objEvents.push_back(objEvent);
                    }
                }
                continue;
            }
            if (event == "end") {
                rec.endWhy = v.strOr("why");
                continue;
            }
            if (event == "level") continue;
            if (!v.get("t")) continue;

            RecRow row;
            row.t = v.intOr("t");
            row.p = v.intOr("p", 1);
            row.held = v.intOr("h") != 0;
            row.x = static_cast<float>(v.numOr("x"));
            row.y = static_cast<float>(v.numOr("y"));
            row.nx = static_cast<float>(v.numOr("nx"));
            row.ny = static_cast<float>(v.numOr("ny"));
            row.lt = v.intOr("lt") != 0;
            row.rot = static_cast<float>(v.numOr("rot"));
            row.yv = v.numOr("yv");
            row.xs = static_cast<float>(v.numOr("xs"));
            row.sm = v.numOr("sm", 1.0);
            row.g = v.numOr("g");
            row.ys = v.numOr("ys");
            row.gm = static_cast<float>(v.numOr("gm", 1.0));
            row.vs = static_cast<float>(v.numOr("vs", 1.0));
            auto modeStr = v.strOr("m", "C");
            row.mode = modeStr.empty() ? 'C' : modeStr[0];
            row.flags = static_cast<uint32_t>(v.numOr("f"));
            row.tt = v.numOr("tt");
            row.ljt = v.numOr("ljt");
            row.llt = v.numOr("llt");
            row.lft = v.numOr("lft");
            row.sa = static_cast<float>(v.numOr("sa"));
            row.sv = static_cast<float>(v.numOr("sv"));
            row.sar = static_cast<float>(v.numOr("sar"));
            row.csl = v.intOr("csl");
            row.lgo = v.intOr("lgo");
            row.col = v.intOr("col");
            row.ost = v.intOr("ost");
            row.lap = v.intOr("lap");
            row.snap = v.numOr("snap");
            row.fs = v.numOr("fs");
            row.yvbs = v.numOr("yvbs");
            row.ctmy = v.numOr("ctmy");
            row.cbmy = v.numOr("cbmy");
            row.clmx = v.numOr("clmx");
            row.crmx = v.numOr("crmx");
            row.gyv = v.numOr("gyv");
            row.csyv = v.numOr("csyv");
            row.tw = static_cast<float>(v.numOr("tw", 1.0));
            row.ta = v.intOr("ta");
            row.dual = v.intOr("dual") != 0;
            row.pmin = static_cast<float>(v.numOr("pmin", -1.0));
            row.pmax = static_cast<float>(v.numOr("pmax", -1.0));
            row.zc = v.intOr("zc") != 0;
            row.cz = static_cast<float>(v.numOr("cz", 0.0));
            row.coy = static_cast<float>(v.numOr("coy", 0.0));
            row.cs = v.intOr("cs") != 0;
            row.hb = static_cast<float>(v.numOr("hb", 30.0));
            row.pw = static_cast<float>(v.numOr("pw", 30.0));
            row.ph = static_cast<float>(v.numOr("ph", 30.0));
            row.rs = static_cast<float>(v.numOr("rs"));
            row.aos = v.numOr("aos");
            row.fsy = static_cast<float>(v.numOr("fsy"));
            row.lpx = static_cast<float>(v.numOr("lpx"));
            row.lpy = static_cast<float>(v.numOr("lpy"));
            row.ci = v.intOr("ci");
            row.so = v.intOr("so");
            row.sna = v.intOr("sna");
            row.shh = v.intOr("shh");
            row.sfg = v.intOr("sfg");
            row.sds = v.intOr("sds");
            if (auto* arr = v.get("ucol"); arr && arr->arr.size() == 4)
                for (int i = 0; i < 4; ++i) row.ucol[i] = static_cast<int>(arr->arr[static_cast<size_t>(i)].num);
            if (auto* arr = v.get("unk"); arr && arr->arr.size() == 2)
                for (int i = 0; i < 2; ++i) row.unk[i] = static_cast<int>(arr->arr[static_cast<size_t>(i)].num);
            row.ia = v.intOr("ia") != 0;
            row.rj = v.intOr("rj") != 0;
            if (v.get("rng")) {
                row.hasRng = true;
                row.rng = static_cast<uint32_t>(v.numOr("rng"));
            }
            (row.p == 2 ? rec.rows2 : rec.rows).push_back(row);
        }
        return true;
    }

}
