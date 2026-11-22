#pragma once

// The arena music (2026-11-22, the user: "cycle through the games battle themes ... the mercenaries
// theme should also be there, but not always"). The Mercenaries theme is learned the first time a
// run's arena on a Mercenaries map opens a looping track of the game's stream lists (remembered in
// <data>\music_theme.txt). From then on each arena's track comes from rm::music::Rotation over the
// theme and every battle track (b_bgm, the Gold Edition's too): the theme's file is read from the
// track's (a file redirect) and the theme's entry in the game's loaded list takes the track's size,
// length and loop points, put back when the run is over. No game file is changed.

#include <filesystem>
#include <string>
#include <vector>

#include "file_redirect.h"

namespace rmre5::music {

struct Context {
    bool enabled = true;         // settings music_rotation
    bool inRun = false;          // a run is active
    bool inMatch = false;
    bool arenaOn = false;        // its arena is being fought (no screen between arenas)
    bool mercenariesMap = false; // a Mercenaries stage (not a campaign area)
    std::filesystem::path dataDir;
    double now = 0.0;
};

/// Each tick: the streams the game opened (logged; the theme learned), the theme's entry put back
/// once no run is on (a minute after the last arena was armed: a run's first match starts its run).
void Tick(const Context& c);
/// The next arena's track (the match about to load): call when it is armed.
void NextArena(bool enabled, double now);
/// The file redirect of the track playing in the theme's place (none while the theme is its own),
/// and a key that changes with it.
std::vector<fileredirect::Redirect> Redirects();
std::string RedirectKey();

}  // namespace rmre5::music
