#pragma once

// Developer probe for the game's menu screens; see menu_probe.cpp.

#include <string>

namespace rmre5::menuprobe {

// Queues hooks on every uId* class move(); call before MH_ApplyQueued.
bool Install();
// Game thread: logs screens opening and closing.
void OnGameTick();
std::string Describe();
// Byte snapshot of every live screen, then a dword diff against it.
void Snapshot();
void Diff();

}  // namespace rmre5::menuprobe
