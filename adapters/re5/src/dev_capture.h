#pragma once

#include <filesystem>

struct IDirect3DDevice9;

namespace rmre5 {

void ConfigureDevCapture(const std::filesystem::path& dataDir);
// Present hook, after the overlay has drawn.
void MaybeCaptureBackBuffer(IDirect3DDevice9* device);

}  // namespace rmre5
