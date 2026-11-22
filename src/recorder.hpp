//Declaration of the F6 recorder.

#pragma once

#include "objwatch.hpp"

#include <Geode/Geode.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace ap {

    inline constexpr float kObjWindow = 1500.f;

    class Recorder {
    public:
        static Recorder& get();

        void toggle(PlayLayer* pl);
        // true while the F6 recorder is on
        bool enabled() const { return m_enabled; }

        void onLevelLoaded(PlayLayer* pl);
        void onPreReset(PlayLayer* pl);
        void onReset(PlayLayer* pl);
        void onTick(PlayLayer* pl, int step, bool lastTick);
        void onDeath(PlayLayer* pl, PlayerObject* player, GameObject* killer, int step);
        void onComplete(int step);
        void onLayerGone();

        std::string statusText() const;

    private:
        void dumpLevel(PlayLayer* pl);
        void beginAttempt(PlayLayer* pl);
        void endAttempt(char const* why);
        void writeRow(PlayLayer* pl, PlayerObject* p, int step, int which, bool lastTick);
        std::string preInputs(PlayLayer* pl) const;

        bool m_enabled = false;
        bool m_levelDumped = false;
        std::ofstream m_file;
        std::filesystem::path m_path;
        int m_attempt = 0;
        uint64_t m_rng0 = 0;
        bool m_hasRng0 = false;
        uint64_t m_tp0 = 0;
        bool m_hasTp0 = false;
        int m_rows = 0;
        int m_totalRows = 0;
        int m_files = 0;
        std::string m_stamp;
        ObjectWatcher m_watch;
        bool m_snapshotDone = false;
        std::vector<sim::TimelineEvent> m_events;
        int m_objEvents = 0;
        std::vector<int> m_hist;
        int m_histLast = -1;
        bool m_histValid = false;
        // empties the input history
        void clearHistory(bool valid) {
            m_hist.clear();
            m_histLast = -1;
            m_histValid = valid;
        }
    };

}
