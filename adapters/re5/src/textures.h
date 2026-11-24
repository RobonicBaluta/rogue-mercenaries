#pragma once

#include <windows.h>

#include <d3d9.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The game's loaded textures (rTexture) and their Direct3D objects, for the parts of the mod that
// rewrite one in memory (hud_names: the characters' names; gun_looks: an element gun's picture and
// model). Nothing on disk changes. Call from the game's thread (the overlay's frame, at Present).
namespace rmre5::textures {

/// The D3D texture of an rTexture: among its fields a cTrans::Texture whose +0x10 is the
/// IDirect3DTexture9 (seen 2026-11-21, rTexture +0xCC); nullptr when it has none.
IDirect3DTexture9* D3DTextureOf(std::uintptr_t resource);
/// A level's description; false when the call faults or fails.
bool LevelDesc(IDirect3DTexture9* texture, UINT level, D3DSURFACE_DESC& desc);
/// Its levels (0 when the call faults).
DWORD LevelCount(IDirect3DTexture9* texture);
/// LockRect / UnlockRect, guarded.
bool Lock(IDirect3DTexture9* texture, UINT level, const RECT* rect, DWORD flags, D3DLOCKED_RECT& locked);
void Unlock(IDirect3DTexture9* texture, UINT level);

struct Loaded {
    std::string path;  // as the resource holds it ("pawn\wp\wp02\model\wp0200_BM")
    std::uintptr_t resource = 0;
};
/// The rTextures in memory now (the resource manager's table, re5_layout.h kResourceTable) whose
/// path, in lower case, passes `want`.
std::vector<Loaded> Find(const std::function<bool(const std::string& lowerPath)>& want);

}  // namespace rmre5::textures
