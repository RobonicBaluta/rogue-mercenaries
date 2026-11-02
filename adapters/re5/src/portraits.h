#pragma once

// Character portraits: the busts the game's own Mercenaries character select shows, read at
// runtime from the player's game archives (MerceResource.arc, Merce2Resource.arc; nothing of
// the game is shipped) into D3D9 textures, the first time each one is drawn (see portraits.cpp).

#include <filesystem>

struct IDirect3DDevice9;

namespace rmre5::portraits {

void SetGameDirectory(const std::filesystem::path& gameDir);

struct Portrait {
    unsigned long long texture = 0;  // ImGui texture id; 0 = none
    int width = 0;
    int height = 0;
};

/// Render thread, inside the overlay's frame: the bust for a costume slot (0..17), or none if
/// the slot has no bust or its archive cannot be read (logged once per slot).
Portrait Get(IDirect3DDevice9* device, int costume);

}  // namespace rmre5::portraits
