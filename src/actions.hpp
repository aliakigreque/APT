// Actions shared by the menu and the key: search, play, save, record, load.

#pragma once

#include "macro.hpp"

namespace ap::actions {

    void solve(bool live);
    void play();
    void exportMacro();
    void record();
    void playFile(MacroFile const& file);

    bool playing();

}
