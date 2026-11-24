#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct IDirect3DDevice9;

// An element gun shows its element (the user's item 4, 2026-11-24: "modifying the items either
// textures or adding some effect and changing the item image to reflect the element"):
//  - its picture, a 102x102 cut of the item picture sheet (id\jpn\cockpit\cp05_360_NOMIP_BM, which
//    the match's inventory, its rings and the item box draw from), takes the element's colour and
//    its badge in the top right corner (configs\element_badges, scripts/element_badges.py);
//  - its model's colour maps (pawn\wp\wpNN\model\*_BM) take the element's colour.
// The colour moves each DXT block's two colours (rm/dxt.h), so the game's own detail stays; the
// badge's blocks are encoded again. Where each gun's picture sits and its model's folder:
// configs\item_pictures.json (scripts/item_pictures.py). The game's bytes go back when the run's
// guns change or the run ends; a texture the game loads again is its own again. Nothing on disk
// changes. The element's effect on the gun in hand is mod.cpp's (DriveGunAuras).
namespace rmre5::gunlooks {

struct Gun {
    std::uint32_t item = 0;  // the game's item (0x01NN)
    std::string element;     // rm::ElementName ("incendiary")
    bool evolved = false;
};

struct Options {
    bool pictures = true;
    bool models = true;
    float pictureTint = 0.45f;   // how far the picture's colours move to the element's
    float modelTint = 0.3f;      // the model's
    float evolvedExtra = 0.15f;  // more for an evolved element
};

/// The mod's configs folder (item_pictures.json, element_badges\); read once.
void SetDirectory(const std::filesystem::path& configDir);
void SetOptions(const Options& options);
/// Mod tick: the guns that show their element now, the first player's first (a picture two guns
/// share shows the first's). Empty: every look is the game's own.
void SetWanted(std::vector<Gun> guns);
/// The game's thread, in the overlay's frame: finds the textures (once a second while there is
/// something to show or put back), writes and puts back.
void OnDraw(IDirect3DDevice9* device);

}  // namespace rmre5::gunlooks
