#pragma once

// The arena music (2026-11-22, the user: "cycle through the games battle themes ... the mercenaries
// theme should also be there, but not always"). Each arena's track comes from rm::music::Rotation over
// the Mercenaries theme (s_bgm020, which every match opens as it starts; <data>\music_theme.txt names
// another) and every battle track (b_bgm, the Gold Edition's too): the theme's file is read from the
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
    bool inMatch = false;        // a match is on (its track may be playing: no swap then)
    std::filesystem::path dataDir;
    double now = 0.0;
};

/// Each tick: the streams the game opened (logged), the theme's entry put back once no run is on (two
/// minutes after the last arena was armed: a run's first match starts its run).
void Tick(const Context& c);
/// The next arena's track (the match about to load): call when it is armed. The swap itself is made once
/// no match is on (Tick), never under a playing track.
void NextArena(bool enabled, double now);
/// The file redirect of the track playing in the theme's place (none while the theme is its own),
/// and a key that changes with it.
std::vector<fileredirect::Redirect> Redirects();
std::string RedirectKey();

}  // namespace rmre5::music
