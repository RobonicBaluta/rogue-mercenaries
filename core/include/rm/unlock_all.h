#pragma once

#include <string_view>

namespace rm {

class Content;
struct Profile;

/// The file a profile lives in, in the mod's data folder. The everything-unlocked package plays on
/// a file of its own, so it never reads or writes the player's normal progress: installing the
/// normal package again is all it takes to be back on it.
std::string_view ProfileFileName(bool everything_unlocked);

/// Opens everything a profile earns over time: every character and skin, perk and weapon, every
/// run length and hard boss, the Exotic tier, and Credits to spare. A skin that wears another
/// author's mod still needs that mod's files. Safe to call on every load, so content added by a
/// later version is open too.
void UnlockEverything(Profile& profile, const Content& content);

}  // namespace rm
