#pragma once

// Engine diagnostics: periodic and change-triggered state logging plus a
// developer overlay panel. Used to verify layout hypotheses in a live game.

#include "settings.h"

namespace rmre5::probe {

void Configure(const Settings& settings);
// Game thread; cheap when nothing changed.
void OnGameTick();
// Render thread, inside an ImGui frame.
void DrawDeveloperPanel();

}  // namespace rmre5::probe
