#pragma once

#include <string>
#include <vector>

// Supported third-party mods (2026-11-08, the user's plan): a mod's files are read from the mod's
// own folder instead of the game's, without overwriting anything there. The game opens its files
// by path (kernel32 CreateFileA/W: "...\nativePC_MT\Image\Archive\uPl05ExcellaNormal.arc"); a
// redirect names a path's tail ("Image\Archive\uPl05ExcellaNormal.arc", any case and slash) and
// the file to open in its place. A redirect is used for the opens after it is set: an archive the
// game already holds in memory is not read again until its next load.
namespace rmre5::fileredirect {

struct Redirect {
    std::wstring tail;    // the end of the game's path, lowercase, backslashes; ending in "\\": a folder
    std::wstring target;  // the file opened instead (absolute); for a folder, the folder whose files stand in
};

/// Hooks CreateFileA/W and GetFileAttributesA/W, GetFileAttributesExA/W (queued; MH_ApplyQueued
/// enables them): a mod's files that the game folder lacks (a costume's voice lines) must also
/// exist when the game asks first.
bool Install();
/// Replaces the redirect set (thread safe; the game's loader threads open files).
void SetRedirects(std::vector<Redirect> redirects);
std::vector<Redirect> Redirects();
/// Developer: logs every .arc the game opens (and every redirect, always).
void SetLogOpens(bool on);
/// Normalises a path tail for Redirect::tail.
std::wstring NormaliseTail(const std::wstring& tail);
/// The archives the game opened last, newest first (at most 16), as opened (a redirect's file).
std::vector<std::wstring> RecentArchives();
/// The music streams (.sngw) the game asked to open since the last call, oldest first, as the game
/// named them (a redirect's own name, not its file); at most 32 kept.
std::vector<std::wstring> TakeStreamOpens();

}  // namespace rmre5::fileredirect
