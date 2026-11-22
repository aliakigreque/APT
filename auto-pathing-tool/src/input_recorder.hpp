// Declaration of the F7 macro recorder.

#pragma once

#include <Geode/Geode.hpp>

#include <cstdint>
#include <vector>

namespace ap {

    class InputRecorder {
    public:
        static InputRecorder& get();

        // true while the F7 recorder is on
        bool enabled() const { return m_enabled; }
        void setEnabled(bool on);

        void onLevelLoaded(PlayLayer* pl);
        void onLayerGone();
        void onNewAttempt(PlayLayer* pl);
        void onQueuedButtons(GJBaseGameLayer* layer, int step);
        void onDeath(PlayLayer* pl, int step);
        void onComplete(PlayLayer* pl, int step);

    private:
        bool recording(PlayLayer* pl) const;

        bool m_enabled = false;
        PlayLayer* m_layer = nullptr;
        std::vector<uint8_t> m_inputs;
        uint8_t m_held = 0;
        bool m_fromStart = false;
    };

}
