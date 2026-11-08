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
    std::wstring tail;    // the end of the game's path, lowercase, backslashes
    std::wstring target;  // the file opened instead (absolute)
};

/// Hooks CreateFileA/W (queued; MH_ApplyQueued enables it).
bool Install();
/// Replaces the redirect set (thread safe; the game's loader threads open files).
void SetRedirects(std::vector<Redirect> redirects);
std::vector<Redirect> Redirects();
/// Developer: logs every .arc the game opens (and every redirect, always).
void SetLogOpens(bool on);
/// Normalises a path tail for Redirect::tail.
std::wstring NormaliseTail(const std::wstring& tail);

}  // namespace rmre5::fileredirect
