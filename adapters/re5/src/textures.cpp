// The game's loaded textures. See textures.h.

#include "textures.h"

#include <algorithm>
#include <cctype>

#include "engine.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5::textures {
namespace {

bool InImage(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.Type == MEM_IMAGE &&
           mbi.State == MEM_COMMIT;
}

D3DRESOURCETYPE ResourceType(IDirect3DResource9* r) {
    __try {
        return r->GetType();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<D3DRESOURCETYPE>(0);
    }
}

// The first field of `object` (up to `bytes`) whose DTI is named `name`.
std::uintptr_t FieldOfClass(std::uintptr_t object, std::size_t bytes, const char* name, std::uintptr_t& dtiCache) {
    for (std::size_t off = 4; off < bytes; off += 4) {
        const auto p = ReadOr<std::uintptr_t>(object + off, 0);
        if (p < 0x10000 || p >= 0x7FFF0000 || (p & 3)) continue;
        const std::uintptr_t dti = engine::DtiOf(p);
        if (!dti) continue;
        if (dti == dtiCache || (!dtiCache && engine::DtiName(dti) == name)) {
            dtiCache = dti;
            return p;
        }
    }
    return 0;
}

}  // namespace

IDirect3DTexture9* D3DTextureOf(std::uintptr_t resource) {
    static std::uintptr_t wrapperDti = 0;
    const std::uintptr_t wrapper = FieldOfClass(resource, 0x120, "cTrans::Texture", wrapperDti);
    if (!wrapper) return nullptr;
    for (std::size_t off = 4; off < 0x40; off += 4) {
        const auto p = ReadOr<std::uintptr_t>(wrapper + off, 0);
        if (p < 0x10000 || p >= 0x7FFF0000 || (p & 3)) continue;
        const auto vtable = ReadOr<std::uintptr_t>(p, 0);
        if (!vtable || !InImage(vtable) || !InImage(ReadOr<std::uintptr_t>(vtable, 0))) continue;
        auto* r = reinterpret_cast<IDirect3DResource9*>(p);
        if (ResourceType(r) == D3DRTYPE_TEXTURE) return static_cast<IDirect3DTexture9*>(r);
    }
    return nullptr;
}

bool LevelDesc(IDirect3DTexture9* texture, UINT level, D3DSURFACE_DESC& desc) {
    __try {
        return SUCCEEDED(texture->GetLevelDesc(level, &desc));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

DWORD LevelCount(IDirect3DTexture9* texture) {
    __try {
        return texture->GetLevelCount();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool Lock(IDirect3DTexture9* texture, UINT level, const RECT* rect, DWORD flags, D3DLOCKED_RECT& locked) {
    __try {
        return SUCCEEDED(texture->LockRect(level, &locked, rect, flags)) && locked.pBits;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Unlock(IDirect3DTexture9* texture, UINT level) {
    __try {
        texture->UnlockRect(level);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

std::vector<Loaded> Find(const std::function<bool(const std::string& lowerPath)>& want) {
    std::vector<Loaded> out;
    const auto manager = ReadOr<std::uintptr_t>(re5::addr::kResourceManager, 0);
    if (!manager) return out;
    // rTexture's type hash, as its DTI holds it (build/dti_all.txt: 0x241F5DEB).
    const auto type = ReadOr<std::uint32_t>(re5::addr::kDtiTexture + re5::addr::kDtiTypeHash, re5::addr::kTextureTypeHash);
    for (int i = 0; i < re5::addr::kResourceTableSlots; ++i) {
        const auto resource = ReadOr<std::uintptr_t>(manager + re5::addr::kResourceTable + static_cast<std::size_t>(i) * 4, 0);
        if (!resource || ReadOr<std::uint32_t>(resource + re5::addr::kResourceTypeHash, 0) != type) continue;
        std::string path = ReadCString(resource + re5::addr::kResourcePath, 64);
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (want(lower)) out.push_back({std::move(path), resource});
    }
    return out;
}

}  // namespace rmre5::textures
