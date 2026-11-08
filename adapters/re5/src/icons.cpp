#include "icons.h"

#include <windows.h>

#include <d3d9.h>
#include <wincodec.h>

#include <cstring>
#include <map>
#include <vector>

#include "log.h"
#include "rm/common.h"

namespace rmre5::icons {
namespace {

std::filesystem::path g_dir;
IDirect3DDevice9* g_device = nullptr;
std::map<std::string, IDirect3DTexture9*> g_textures;  // nullptr: tried and failed

template <typename T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// 32-bit BGRA pixels of a PNG (straight alpha, as A8R8G8B8 lays them out in memory).
bool Decode(const std::filesystem::path& file, UINT& width, UINT& height, std::vector<unsigned char>& pixels) {
    // The game thread may already have COM in either model; both keep WIC usable.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
              SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                              WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(converter->GetSize(&width, &height)) && width > 0 && height > 0 && width <= 1024 && height <= 1024;
    if (ok) {
        pixels.resize(static_cast<std::size_t>(width) * height * 4);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    }
    Release(converter);
    Release(frame);
    Release(decoder);
    Release(factory);
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

IDirect3DTexture9* Load(IDirect3DDevice9* device, const std::string& name) {
    const auto file = g_dir / (std::wstring(name.begin(), name.end()) + L".png");
    UINT width = 0, height = 0;
    std::vector<unsigned char> pixels;
    if (!Decode(file, width, height, pixels)) {
        RM_LOGW("icons: %s.png missing or unreadable in %s", name.c_str(), rm::PathText(g_dir).c_str());
        return nullptr;
    }
    // Managed: the runtime keeps a copy, so the texture survives a device reset.
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) {
        RM_LOGW("icons: texture for %s not created", name.c_str());
        return nullptr;
    }
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
        texture->Release();
        return nullptr;
    }
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(static_cast<unsigned char*>(locked.pBits) + static_cast<std::size_t>(y) * locked.Pitch,
                    pixels.data() + static_cast<std::size_t>(y) * width * 4, width * 4);
    }
    texture->UnlockRect(0);
    RM_LOGI("icons: %s loaded (%ux%u)", name.c_str(), width, height);
    return texture;
}

}  // namespace

void SetDirectory(const std::filesystem::path& dir) { g_dir = dir; }

unsigned long long Texture(IDirect3DDevice9* device, const std::string& name) {
    if (!device || name.empty()) return 0;
    if (device != g_device) {  // a new device: the old one's textures went with it (not released)
        g_textures.clear();
        g_device = device;
    }
    auto it = g_textures.find(name);
    if (it == g_textures.end()) it = g_textures.emplace(name, Load(device, name)).first;
    return reinterpret_cast<unsigned long long>(it->second);
}

}  // namespace rmre5::icons
