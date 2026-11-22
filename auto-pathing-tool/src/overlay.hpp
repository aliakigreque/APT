// Declaration of the on-screen overlay.

#pragma once

#include "engine.hpp"
#include "macro.hpp"

#include <Geode/Geode.hpp>

#include <string>
#include <vector>

namespace ap {

    class Overlay {
    public:
        static void attach(PlayLayer* pl);
        static void detach();

        static void setStatus(std::string const& text);
        static void applySettings();
        static void drawPoints(std::vector<cocos2d::CCPoint> const& points, int fromStep);
        static void clearPath();
        static void setPathVisible(bool visible);

    private:
        static cocos2d::CCDrawNode* s_path;
        static cocos2d::CCLabelBMFont* s_label;
        static int s_pathSegments;
    };

}
