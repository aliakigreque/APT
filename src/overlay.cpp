// On-screen overlay: the planned path drawn in the level and the status line.

#include "overlay.hpp"

#include "settings.hpp"

#include <algorithm>

using namespace geode::prelude;

namespace ap {

    CCDrawNode* Overlay::s_path = nullptr;
    CCLabelBMFont* Overlay::s_label = nullptr;
    int Overlay::s_pathSegments = 0;

    // creates the path node and the status label in the level
    void Overlay::attach(PlayLayer* pl) {
        if (!pl) return;

        if (pl->m_objectLayer) {
            s_path = CCDrawNode::create();
            s_path->setID("path"_spr);
            cocos2d::ccBlendFunc blend;
            blend.src = GL_SRC_ALPHA;
            blend.dst = GL_ONE_MINUS_SRC_ALPHA;
            s_path->setBlendFunc(blend);
            pl->m_objectLayer->addChild(s_path, 500);
        }

        s_label = CCLabelBMFont::create("", "chatFont.fnt");
        s_label->setID("status"_spr);
        s_label->setScale(0.55f);
        s_label->setOpacity(220);
        s_label->setZOrder(1000);
        pl->addChild(s_label);
        applySettings();
    }

    // moves the status line to its corner and shows it if enabled
    void Overlay::applySettings() {
        if (!s_label) return;
        auto const& settings = Settings::get();
        auto const winSize = CCDirector::sharedDirector()->getWinSize();
        bool const right = settings.statusCorner == 1 || settings.statusCorner == 3;
        bool const bottom = settings.statusCorner >= 2;
        s_label->setAnchorPoint({right ? 1.f : 0.f, bottom ? 0.f : 1.f});
        s_label->setAlignment(right ? kCCTextAlignmentRight : kCCTextAlignmentLeft);
        float const x = right ? winSize.width - (bottom ? 6.f : 44.f) : 6.f;
        float const y = bottom ? 4.f : winSize.height - 4.f;
        s_label->setPosition({x, y});
        s_label->setVisible(settings.showStatus && s_label->getString() && *s_label->getString());
    }

    // forgets the nodes, the level deletes them
    void Overlay::detach() {
        s_path = nullptr;
        s_label = nullptr;
    }

    // changes the status text, skipped if it's the same
    void Overlay::setStatus(std::string const& text) {
        if (!s_label) return;
        static std::string last;
        if (text == last && s_label->getString() && text == s_label->getString()) return;
        last = text;
        s_label->setString(text.c_str());
        s_label->setVisible(!text.empty() && Settings::get().showStatus);
    }

    // erases the path if something is drawn
    void Overlay::clearPath() {
        if (s_path && s_pathSegments) {
            s_path->clear();
            s_pathSegments = 0;
        }
    }

    // shows or hides the path
    // could be used for a setting that hides the drawn path, keep
    void Overlay::setPathVisible(bool visible) {
        if (s_path) s_path->setVisible(visible);
    }

    // draws a line through every other point, redraws only if the points changed
    void Overlay::drawPoints(std::vector<cocos2d::CCPoint> const& points, int fromStep) {
        if (!s_path) return;
        static std::vector<cocos2d::CCPoint> s_last;
        static int s_lastFrom = -1;
        static cocos2d::CCDrawNode* s_lastNode = nullptr;
        bool const same = s_lastNode == s_path && s_lastFrom == fromStep && s_pathSegments > 0 && s_last.size() == points.size() &&
                        std::equal(points.begin(), points.end(), s_last.begin(), [](cocos2d::CCPoint const& a, cocos2d::CCPoint const& b) {
            return a.x == b.x && a.y == b.y;
        });
        if (same) {
            s_path->setVisible(true);
            return;
        }
        s_last = points;
        s_lastFrom = fromStep;
        s_lastNode = s_path;
        s_path->clear();
        s_path->setVisible(true);
        s_pathSegments = 0;
        if (points.size() < 2) return;
        ccColor4F const green{0.25f, 0.95f, 0.35f, 0.9f};
        for (size_t i = 0; i + 2 < points.size(); i += 2) {
            s_path->drawSegment(points[i], points[i + 2], 0.7f, green);
            ++s_pathSegments;
        }
    }

}
