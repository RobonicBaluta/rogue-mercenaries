// Developer frame capture: when <data>\capture.request exists, the next
// Present writes the back buffer (game + overlay) to <data>\captures\*.bmp and
// deletes the request. Independent of window composition, so it works while
// the game window is covered by other applications.

#include "dev_capture.h"

#include <windows.h>

#include <d3d9.h>

#include <cstdio>
#include <vector>

#include "log.h"

namespace rmre5 {
namespace {

std::filesystem::path g_dataDir;
DWORD g_lastCheck = 0;
int g_counter = 0;

bool WriteBmp(const std::filesystem::path& file, const std::vector<std::uint32_t>& pixels, UINT width,
              UINT height) {
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);  // top-down
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    const DWORD imageBytes = width * height * 4;
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(info);
    fileHeader.bfSize = fileHeader.bfOffBits + imageBytes;
    FILE* out = _wfopen(file.c_str(), L"wb");
    if (!out) return false;
    std::fwrite(&fileHeader, sizeof(fileHeader), 1, out);
    std::fwrite(&info, sizeof(info), 1, out);
    std::fwrite(pixels.data(), 1, imageBytes, out);
    std::fclose(out);
    return true;
}

}  // namespace

void ConfigureDevCapture(const std::filesystem::path& dataDir) { g_dataDir = dataDir; }

void MaybeCaptureBackBuffer(IDirect3DDevice9* device) {
    if (g_dataDir.empty()) return;
    const DWORD now = GetTickCount();
    if (now - g_lastCheck < 500) return;
    g_lastCheck = now;
    std::error_code ec;
    const auto request = g_dataDir / L"capture.request";
    if (!std::filesystem::exists(request, ec)) return;
    std::filesystem::remove(request, ec);

    IDirect3DSurface9* backBuffer = nullptr;
    IDirect3DSurface9* copy = nullptr;
    D3DSURFACE_DESC desc{};
    bool ok = false;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) &&
        SUCCEEDED(backBuffer->GetDesc(&desc)) &&
        (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8) &&
        SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy,
                                                      nullptr)) &&
        SUCCEEDED(device->GetRenderTargetData(backBuffer, copy))) {
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(copy->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            std::vector<std::uint32_t> pixels(static_cast<std::size_t>(desc.Width) * desc.Height);
            for (UINT y = 0; y < desc.Height; ++y) {
                const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(locked.pBits) +
                                                                         y * locked.Pitch);
                for (UINT x = 0; x < desc.Width; ++x) {
                    pixels[static_cast<std::size_t>(y) * desc.Width + x] = row[x] | 0xFF000000u;
                }
            }
            copy->UnlockRect();
            const auto dir = g_dataDir / L"captures";
            std::filesystem::create_directories(dir, ec);
            wchar_t name[64];
            swprintf_s(name, L"frame-%03d.bmp", ++g_counter);
            ok = WriteBmp(dir / name, pixels, desc.Width, desc.Height);
            RM_LOGI("dev capture %ls %ux%u -> %s", name, desc.Width, desc.Height, ok ? "ok" : "write failed");
        }
    }
    if (!ok) {
        RM_LOGW("dev capture failed (format=%d)", static_cast<int>(desc.Format));
    }
    if (copy) copy->Release();
    if (backBuffer) backBuffer->Release();
}

}  // namespace rmre5
