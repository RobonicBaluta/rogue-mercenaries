#pragma once

// Character portraits: the busts the game's own Mercenaries character select shows, read at
// runtime from the player's game archives (MerceResource.arc, Merce2Resource.arc; nothing of
// the game is shipped) into D3D9 textures, the first time each one is drawn (see portraits.cpp).

#include <array>
#include <filesystem>
#include <string>

struct IDirect3DDevice9;

namespace rmre5::portraits {

void SetGameDirectory(const std::filesystem::path& gameDir);

struct Portrait {
    unsigned long long texture = 0;  // ImGui texture id; 0 = none
    int width = 0;
    int height = 0;
};

/// Render thread, inside the overlay's frame: the bust for a costume slot (0..17), or a mod
/// skin's picture (kModPortraitBase and up, SetModPortrait), or none if the slot has no bust or
/// its file cannot be read (logged once per slot).
Portrait Get(IDirect3DDevice9* device, int costume);

/// A supported mod's own picture, read from the player's copy of the mod (never shipped): a
/// texture entry of an archive, or a region of an image (crop x, y, width, height; empty: all).
constexpr int kModPortraitBase = 100;
struct ModPortrait {
    std::filesystem::path file;
    std::string entry;            // an archive's texture entry; empty: file is an image (WIC: jpg, png)
    std::array<int, 4> crop{};    // width 0: the whole image
};
/// Any thread: the picture shown for portrait slot `slot` (kModPortraitBase and up) from now on.
void SetModPortrait(int slot, ModPortrait source);

}  // namespace rmre5::portraits
