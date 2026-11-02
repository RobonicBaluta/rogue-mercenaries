#pragma once

// Menu icons: PNG files in <game>\RogueMercenaries\configs\icons, decoded with WIC into D3D9 textures
// on the render thread the first time they are drawn (see icons.cpp).

#include <filesystem>
#include <string>

struct IDirect3DDevice9;

namespace rmre5::icons {

void SetDirectory(const std::filesystem::path& dir);

/// Render thread, inside the overlay's frame: the texture for icons\<name>.png as an ImGui
/// texture id, 0 if it is missing or cannot be decoded (logged once per name).
unsigned long long Texture(IDirect3DDevice9* device, const std::string& name);

}  // namespace rmre5::icons
