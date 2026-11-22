#pragma once

// Menu icons: PNG files in <game>\RogueMercenaries\configs\icons, decoded with WIC into D3D9 textures
// on the render thread the first time they are drawn (see icons.cpp).

#include <filesystem>
#include <string>
#include <vector>

struct IDirect3DDevice9;

namespace rmre5::icons {

void SetDirectory(const std::filesystem::path& dir);

/// Render thread, inside the overlay's frame: the texture for icons\<name>.png as an ImGui
/// texture id, 0 if it is missing or cannot be decoded (logged once per name).
unsigned long long Texture(IDirect3DDevice9* device, const std::string& name);
/// A PNG's pixels as 32-bit BGRA (straight alpha), at most 1024x1024; false when it cannot be read.
bool DecodePng(const std::filesystem::path& file, unsigned& width, unsigned& height, std::vector<unsigned char>& pixels);

}  // namespace rmre5::icons
