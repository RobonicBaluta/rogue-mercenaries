#pragma once

#include <filesystem>

#include "settings.h"

// The mod's own effects (rm/effects.h): <data>\generated\rmEffects.arc, built from the player's own
// uWp21.arc and coreEff.arc (read, never written) on a thread of its own at start, and kept while
// those are unchanged. The game reads it as Image\Archive\rmEffects.arc (a file redirect) with a
// run's loads, so its effects are in memory like the game's own.
namespace rmre5::effectspack {

/// Starts the build (or the check that the built one is current).
void Start(const Paths& paths);
/// The archive once it is there; empty before, or when it could not be built.
std::filesystem::path Ready();

}  // namespace rmre5::effectspack
