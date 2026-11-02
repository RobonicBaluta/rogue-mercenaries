#include "portraits.h"

#include <windows.h>

#include <d3d9.h>

#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "log.h"
#include "rm/inflate.h"

namespace rmre5::portraits {
namespace {

// The bust of each costume slot: 0..9 as the game's character select shows them (its strip is in
// slot order), 10..17 matched to the class and model each slot loads in a match.
struct Bust {
    const wchar_t* archive;
    const char* entry;
};
constexpr std::array<Bust, 18> kBusts = {{
    {L"MerceResource", "id\\jpn\\versus\\versus_17_360_NOMIP_BM"},    // 0 Chris BSAA
    {L"MerceResource", "id\\jpn\\versus\\versus_18_360_NOMIP_BM"},    // 1 Chris Safari
    {L"MerceResource", "id\\jpn\\versus\\versus_19_360_NOMIP_BM"},    // 2 Chris S.T.A.R.S.
    {L"MerceResource", "id\\jpn\\versus\\versus_20_360_NOMIP_BM"},    // 3 Sheva BSAA
    {L"MerceResource", "id\\jpn\\versus\\versus_24_360_NOMIP_BM"},    // 4 Sheva Clubbin'
    {L"MerceResource", "id\\jpn\\versus\\versus_23_360_NOMIP_BM"},    // 5 Sheva Tribal
    {L"MerceResource", "id\\jpn\\versus\\versus_25_360_NOMIP_BM"},    // 6 Jill BSAA
    {L"MerceResource", "id\\jpn\\versus\\versus_26_360_NOMIP_BM"},    // 7 Jill Battle Suit
    {L"MerceResource", "id\\jpn\\versus\\versus_27_360_NOMIP_BM"},    // 8 Wesker Midnight
    {L"MerceResource", "id\\jpn\\versus\\versus_28_360_NOMIP_BM"},    // 9 Wesker S.T.A.R.S.
    {L"Merce2Resource", "id\\jpn\\merce_dc\\versus_18_360_NOMIP_BM"},  // 10 Chris Heavy Metal
    {L"Merce2Resource", "id\\jpn\\merce_dc\\versus_20_360_NOMIP_BM"},  // 11 Sheva Fairy Tale (uPl01ShebaCos4)
    {L"Merce2Resource", "id\\jpn\\merce_dc\\versus_21_360_NOMIP_BM"},  // 12 Josh
    {L"Merce2Resource", "id\\jpn\\merce_dc\\versus_22_360_NOMIP_BM"},  // 13 Excella
    {L"Merce2Resource", "id\\jpn\\merce_dc\\versus_17_360_NOMIP_BM"},  // 14 Chris Warrior
    {L"Merce2Resource", "id\\jpn\\merce_dc\\versus_19_360_NOMIP_BM"},  // 15 Sheva Business (uPl01ShebaCos3)
    {L"Merce2Resource", "id\\jpn\\merce_dc\\versus_23_360_NOMIP_BM"},  // 16 Barry
    {L"Merce2Resource", "id\\jpn\\merce_dc\\versus_24_360_NOMIP_BM"},  // 17 Rebecca
}};

constexpr std::uint32_t kTextureType = 0x241F5DEB;  // rTexture
constexpr std::size_t kMaxTexture = 8u << 20;

std::filesystem::path g_archiveDir;
IDirect3DDevice9* g_device = nullptr;
std::map<int, Portrait> g_cache;  // texture 0: tried and failed

// One resource of an MT Framework archive: "ARC\0", u16 version, u16 count, then per entry a
// 64-byte name, u32 type, u32 packed size, u32 size (low 24 bits), u32 offset; zlib data.
bool ReadEntry(const std::filesystem::path& file, const char* name, std::vector<std::uint8_t>& out) {
    std::ifstream in(file, std::ios::binary);
    char header[8] = {};
    if (!in.read(header, sizeof(header)) || std::memcmp(header, "ARC\0", 4) != 0) return false;
    std::uint16_t count = 0;
    std::memcpy(&count, header + 6, 2);
    for (std::uint16_t i = 0; i < count; ++i) {
        char record[80] = {};
        if (!in.read(record, sizeof(record))) return false;
        std::uint32_t type = 0, packed = 0, size = 0, offset = 0;
        std::memcpy(&type, record + 64, 4);
        std::memcpy(&packed, record + 68, 4);
        std::memcpy(&size, record + 72, 4);
        std::memcpy(&offset, record + 76, 4);
        if (type != kTextureType || std::strncmp(record, name, 64) != 0) continue;
        if (packed == 0 || packed > kMaxTexture) return false;
        std::vector<std::uint8_t> data(packed);
        in.seekg(offset);
        if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(packed))) return false;
        return rm::ZlibInflate(data.data(), data.size(), out, kMaxTexture) && out.size() == (size & 0x00FFFFFFu);
    }
    return false;
}

// TEX version 0x70: u16 width at +0xC, height at +0xE, the format's FourCC at +0x14 (DXT1, DXT3,
// DXT5), the pixel data offset at +0x28; NOMIP textures have one level.
IDirect3DTexture9* CreateTexture(IDirect3DDevice9* device, const std::vector<std::uint8_t>& tex, int& width, int& height) {
    if (tex.size() < 0x2C || std::memcmp(tex.data(), "TEX\0", 4) != 0) return nullptr;
    std::uint16_t w = 0, h = 0;
    std::uint32_t fourcc = 0, start = 0;
    std::memcpy(&w, tex.data() + 0x0C, 2);
    std::memcpy(&h, tex.data() + 0x0E, 2);
    std::memcpy(&fourcc, tex.data() + 0x14, 4);
    std::memcpy(&start, tex.data() + 0x28, 4);
    const std::size_t blockBytes = fourcc == MAKEFOURCC('D', 'X', 'T', '1') ? 8 : 16;
    if ((fourcc != MAKEFOURCC('D', 'X', 'T', '1') && fourcc != MAKEFOURCC('D', 'X', 'T', '3') && fourcc != MAKEFOURCC('D', 'X', 'T', '5')) ||
        w == 0 || h == 0) {
        return nullptr;
    }
    const std::size_t rowBytes = (static_cast<std::size_t>(w) + 3) / 4 * blockBytes;
    const std::size_t rows = (static_cast<std::size_t>(h) + 3) / 4;
    if (start > tex.size() || tex.size() - start < rowBytes * rows) return nullptr;
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(w, h, 1, 0, static_cast<D3DFORMAT>(fourcc), D3DPOOL_MANAGED, &texture, nullptr))) return nullptr;
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
        texture->Release();
        return nullptr;
    }
    for (std::size_t r = 0; r < rows; ++r) {
        std::memcpy(static_cast<unsigned char*>(locked.pBits) + r * static_cast<std::size_t>(locked.Pitch), tex.data() + start + r * rowBytes, rowBytes);
    }
    texture->UnlockRect(0);
    width = w;
    height = h;
    return texture;
}

Portrait Load(IDirect3DDevice9* device, int costume) {
    const Bust& bust = kBusts[static_cast<std::size_t>(costume)];
    const auto archive = g_archiveDir / (std::wstring(bust.archive) + L".arc");
    std::vector<std::uint8_t> tex;
    if (!ReadEntry(archive, bust.entry, tex)) {
        RM_LOGW("portraits: costume %d: %s not read from %s", costume, bust.entry, archive.string().c_str());
        return {};
    }
    Portrait p;
    IDirect3DTexture9* texture = CreateTexture(device, tex, p.width, p.height);
    if (!texture) {
        RM_LOGW("portraits: costume %d: %s is not a texture this device takes", costume, bust.entry);
        return {};
    }
    p.texture = reinterpret_cast<unsigned long long>(texture);
    RM_LOGI("portraits: costume %d loaded (%dx%d)", costume, p.width, p.height);
    return p;
}

}  // namespace

void SetGameDirectory(const std::filesystem::path& gameDir) { g_archiveDir = gameDir / L"nativePC_MT" / L"Image" / L"Archive"; }

Portrait Get(IDirect3DDevice9* device, int costume) {
    if (!device || costume < 0 || costume >= static_cast<int>(kBusts.size())) return {};
    if (device != g_device) {  // a new device: the old one's textures went with it (not released)
        g_cache.clear();
        g_device = device;
    }
    auto it = g_cache.find(costume);
    if (it == g_cache.end()) it = g_cache.emplace(costume, Load(device, costume)).first;
    return it->second;
}

}  // namespace rmre5::portraits
