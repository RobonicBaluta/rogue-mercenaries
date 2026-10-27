#pragma once

// Glue between engine hooks and the roguelite core.

#include "hooks.h"
#include "settings.h"

namespace rmre5 {

void ModInit(const Paths& paths, const Settings& settings);
// After hooks are installed: tells the core which engine features are live.
void ModCapabilitiesReady();
void ModShutdown();
HookSink* ModSink();
// ImGui draw callback (render thread).
void ModDraw();

}  // namespace rmre5
