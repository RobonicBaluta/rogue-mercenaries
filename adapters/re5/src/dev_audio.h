#pragma once

#include <filesystem>

namespace rmre5 {

bool SetOwnAudioMute(bool mute);
// Worker thread: mutes (and leaves a marker) or, if a marker from an earlier
// muted run exists, unmutes. Blocks for about 30 s while the session appears.
void ManageDevAudio(const std::filesystem::path& dataDir, bool mute);
// Window thread on WM_CLOSE: undo a developer mute while the session exists.
void RestoreDevAudioOnExit();

}  // namespace rmre5
