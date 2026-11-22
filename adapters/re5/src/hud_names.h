#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct IDirect3DDevice9;

// The characters' own names on the HUD (the user, 2026-11-21: "Try to add the correct names of the
// characters in game"). The weapon rings and the inventory tabs draw "Chris" and "Sheva" from the
// cockpit texture id\<language>\cockpit\cp03 (its top left strip; layouts cp_life_360 and
// cp_bukislot_360), whichever character wears those costume slots: Leon wears Chris's, Ada Sheva's.
// While a run plays as one of them their name is written over the game's in that texture, in the
// same style (configs\hud_names, made by scripts/hud_names.py), and the game's bytes are put back
// when it ends. Nothing on disk changes: the loaded texture is found from the HUD objects and known
// by the bytes of its name boxes (a fingerprint per language).
namespace rmre5::hudnames {

/// The folder with hud_names.json and its tiles; read once.
void SetDirectory(const std::filesystem::path& dir);
/// Mod tick: the character whose name goes in each of the game's two boxes ("" keeps the game's
/// own), and the HUD objects the texture is found from (the two cockpits, 0 when none).
void SetWanted(const std::string& chrisBox, const std::string& shevaBox, std::uintptr_t cockpit0, std::uintptr_t cockpit1);
/// True when `character` has name tiles.
bool HasName(const std::string& character);
/// Render thread (the overlay's frame): finds the texture, writes or puts back the boxes.
void OnDraw(IDirect3DDevice9* device);
/// Developer: the next search logs every texture it meets.
void Probe();
/// Developer: logs every texture within reach of `object` (a worker thread).
void ProbeFrom(std::uintptr_t object);
/// Developer (the UI lab): a PNG written into a loaded DXT5 texture the HUD uses (the first whose
/// path holds `pathPart`) at x, y (multiples of 4), on the render thread; LabRestore puts every
/// written region back.
void LabPut(const std::string& pathPart, int x, int y, const std::filesystem::path& png);
void LabRestore();

}  // namespace rmre5::hudnames
