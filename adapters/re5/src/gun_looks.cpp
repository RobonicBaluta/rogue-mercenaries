// An element gun's picture and model in its element's colours. See gun_looks.h.

#include "gun_looks.h"

#include <windows.h>

#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include <nlohmann/json.hpp>

#include "icons.h"
#include "log.h"
#include "memory.h"
#include "rm/dxt.h"
#include "textures.h"

namespace rmre5::gunlooks {
namespace {

namespace dxt = rm::dxt;

struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    [[nodiscard]] int Width() const { return x1 - x0; }
    [[nodiscard]] int Height() const { return y1 - y0; }
};

struct Look {
    std::string element;
    bool evolved = false;
    bool operator==(const Look& o) const { return element == o.element && evolved == o.evolved; }
    bool operator!=(const Look& o) const { return !(*this == o); }
    [[nodiscard]] std::string Text() const { return element + (evolved ? " (evolved)" : ""); }
};

// The elements' colours: those of the mod's element text (mod.cpp ElementColour).
bool ElementRgb(const std::string& element, float rgb[3]) {
    static const std::map<std::string, std::array<float, 3>> kColours = {
        {"incendiary", {255, 140, 60}}, {"shock", {255, 230, 90}}, {"cryo", {140, 215, 255}},
        {"explosive", {255, 90, 80}},   {"toxic", {140, 240, 100}},
    };
    const auto it = kColours.find(element);
    if (it == kColours.end()) return false;
    std::copy(it->second.begin(), it->second.end(), rgb);
    return true;
}

std::mutex g_mutex;
std::filesystem::path g_dir;
bool g_loaded = false;
bool g_ok = false;
std::string g_sheetPath;  // lower case
int g_sheetWidth = 0, g_sheetHeight = 0;
std::map<std::uint32_t, Rect> g_pictures;
std::map<std::uint32_t, std::string> g_folders;  // item -> "wp02"
Options g_options;
std::vector<Gun> g_wanted;
bool g_changed = false;
DWORD g_lastLook = 0;

void LoadConfig() {
    g_loaded = true;
    const auto file = g_dir / L"item_pictures.json";
    std::ifstream in(file);
    if (!in) {
        RM_LOGW("gun looks: no %s", file.string().c_str());
        return;
    }
    try {
        const nlohmann::json j = nlohmann::json::parse(in);
        g_sheetPath = j.at("texture").get<std::string>();
        std::transform(g_sheetPath.begin(), g_sheetPath.end(), g_sheetPath.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        g_sheetWidth = j.at("width").get<int>();
        g_sheetHeight = j.at("height").get<int>();
        for (const auto& [item, r] : j.at("pictures").items()) {
            const Rect rect{r.at(0).get<int>(), r.at(1).get<int>(), r.at(2).get<int>(), r.at(3).get<int>()};
            if (rect.x0 < 0 || rect.y0 < 0 || rect.x1 > g_sheetWidth || rect.y1 > g_sheetHeight || rect.Width() < 48 || rect.Height() < 48) {
                RM_LOGW("gun looks: picture %s is not inside the sheet", item.c_str());
                continue;
            }
            g_pictures[static_cast<std::uint32_t>(std::stoul(item, nullptr, 16))] = rect;
        }
        if (j.contains("models")) {
            for (const auto& [item, folder] : j.at("models").items()) {
                std::string f = folder.get<std::string>();
                std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                g_folders[static_cast<std::uint32_t>(std::stoul(item, nullptr, 16))] = f;
            }
        }
        g_ok = !g_pictures.empty();
        RM_LOGI("gun looks: %zu gun pictures in %s, %zu model folders", g_pictures.size(), g_sheetPath.c_str(), g_folders.size());
    } catch (const std::exception& e) {
        RM_LOGW("gun looks: item_pictures.json: %s", e.what());
    }
}

// ---- blocks ---------------------------------------------------------------------------------

// A level's blocks inside `rect` (multiples of 4), row of blocks by row of blocks.
bool ReadBlocks(IDirect3DTexture9* t, UINT level, const Rect& rect, std::size_t blockBytes, std::vector<std::uint8_t>& out) {
    RECT r{rect.x0, rect.y0, rect.x1, rect.y1};
    D3DLOCKED_RECT locked{};
    if (!textures::Lock(t, level, &r, D3DLOCK_READONLY, locked)) return false;
    const std::size_t row = static_cast<std::size_t>(std::max(1, rect.Width() / 4)) * blockBytes;
    const int rows = std::max(1, rect.Height() / 4);
    out.resize(row * static_cast<std::size_t>(rows));
    bool ok = true;
    for (int i = 0; i < rows && ok; ++i) {
        ok = SafeCopy(out.data() + static_cast<std::size_t>(i) * row, reinterpret_cast<std::uintptr_t>(locked.pBits) + static_cast<std::uintptr_t>(i) * locked.Pitch, row);
    }
    textures::Unlock(t, level);
    return ok;
}

bool WriteBlocks(IDirect3DTexture9* t, UINT level, const Rect& rect, std::size_t blockBytes, const std::vector<std::uint8_t>& blocks) {
    const std::size_t row = static_cast<std::size_t>(std::max(1, rect.Width() / 4)) * blockBytes;
    const int rows = std::max(1, rect.Height() / 4);
    if (blocks.size() != row * static_cast<std::size_t>(rows)) return false;
    RECT r{rect.x0, rect.y0, rect.x1, rect.y1};
    D3DLOCKED_RECT locked{};
    if (!textures::Lock(t, level, &r, 0, locked)) return false;
    bool ok = true;
    for (int i = 0; i < rows && ok; ++i) {
        ok = SafeWrite(reinterpret_cast<std::uintptr_t>(locked.pBits) + static_cast<std::uintptr_t>(i) * locked.Pitch, blocks.data() + static_cast<std::size_t>(i) * row, row);
    }
    textures::Unlock(t, level);
    return ok;
}

dxt::Tint TintFor(const Look& look, float strength) {
    dxt::Tint t;
    float rgb[3] = {255, 255, 255};
    if (!ElementRgb(look.element, rgb)) return t;  // strength 0: unchanged
    t.strength = std::clamp(strength + (look.evolved ? g_options.evolvedExtra : 0.0f), 0.0f, 1.0f);
    t.r = rgb[0];
    t.g = rgb[1];
    t.b = rgb[2];
    return t;
}

// ---- the badges -----------------------------------------------------------------------------

struct Badge {
    unsigned width = 0, height = 0;
    std::vector<dxt::Rgba> pixels;  // empty: none
};
std::map<std::string, Badge> g_badges;

const Badge& BadgeFor(const Look& look) {
    const std::string file = look.element + (look.evolved ? "_evolved" : "") + ".png";
    auto it = g_badges.find(file);
    if (it != g_badges.end()) return it->second;
    Badge b;
    std::vector<unsigned char> bgra;
    unsigned w = 0, h = 0;
    if (!icons::DecodePng(g_dir / L"element_badges" / file, w, h, bgra)) {
        RM_LOGW("gun looks: element_badges\\%s cannot be read; the picture gets the colour only", file.c_str());
    } else if (w % 4 || h % 4 || w > 64 || h > 64) {
        RM_LOGW("gun looks: element_badges\\%s is %ux%u, not whole blocks up to 64x64", file.c_str(), w, h);
    } else {
        b.width = w;
        b.height = h;
        b.pixels.resize(static_cast<std::size_t>(w) * h);
        for (std::size_t i = 0; i < b.pixels.size(); ++i) b.pixels[i] = {bgra[i * 4 + 2], bgra[i * 4 + 1], bgra[i * 4], bgra[i * 4 + 3]};
    }
    return g_badges.emplace(file, std::move(b)).first->second;
}

// ---- the picture sheet ----------------------------------------------------------------------

// The blocks wholly inside a picture (its cuts are 102 pixels, not always on a block's edge).
Rect Inner(const Rect& r) { return {(r.x0 + 3) / 4 * 4, (r.y0 + 3) / 4 * 4, r.x1 / 4 * 4, r.y1 / 4 * 4}; }

// The picture's blocks with the look: every colour moved, the badge drawn over its top right corner
// (2 pixels in).
std::vector<std::uint8_t> PictureBlocks(const std::vector<std::uint8_t>& original, const Rect& inner, const Look& look) {
    const Badge& badge = BadgeFor(look);
    dxt::Overlay overlay;
    if (!badge.pixels.empty()) {
        overlay.pixels = badge.pixels;
        overlay.width = static_cast<int>(badge.width);
        overlay.height = static_cast<int>(badge.height);
        overlay.x = (inner.Width() - 2) / 4 * 4 - overlay.width;
        overlay.y = 4;
    }
    return dxt::RecolourDxt5(original, inner.Width() / 4, TintFor(look, g_options.pictureTint), badge.pixels.empty() ? nullptr : &overlay);
}

struct Sheet {
    std::uintptr_t resource = 0;
    IDirect3DTexture9* texture = nullptr;  // held (AddRef)
    struct Written {
        Rect inner;
        std::vector<std::uint8_t> original;
        Look look;
    };
    std::map<std::pair<int, int>, Written> written;  // by the picture's top left
};
Sheet g_sheet;

void DropSheet() {
    if (g_sheet.texture) g_sheet.texture->Release();
    g_sheet = {};
}

// The sheet as `resource` holds it now; a sheet loaded again is the game's own again.
void TakeSheet(std::uintptr_t resource) {
    IDirect3DTexture9* t = resource ? textures::D3DTextureOf(resource) : nullptr;
    if (resource == g_sheet.resource && t == g_sheet.texture) return;
    if (g_sheet.texture) RM_LOGI("gun looks: the picture sheet went (loaded again or released)");
    DropSheet();
    g_sheet.resource = resource;
    if (!t) return;
    D3DSURFACE_DESC desc{};
    if (!textures::LevelDesc(t, 0, desc) || desc.Format != D3DFMT_DXT5 || static_cast<int>(desc.Width) != g_sheetWidth ||
        static_cast<int>(desc.Height) != g_sheetHeight) {
        static int told = 0;
        if (told++ < 3) RM_LOGW("gun looks: the picture sheet is not the %dx%d DXT5 expected; left alone", g_sheetWidth, g_sheetHeight);
        return;
    }
    t->AddRef();
    g_sheet.texture = t;
    RM_LOGI("gun looks: the picture sheet found (pool %d)", static_cast<int>(desc.Pool));
}

void ApplySheet(const std::map<std::pair<int, int>, std::pair<Rect, Look>>& want) {
    if (!g_sheet.texture) return;
    for (auto it = g_sheet.written.begin(); it != g_sheet.written.end();) {
        const auto w = want.find(it->first);
        if (w != want.end() && w->second.second == it->second.look) {
            ++it;
            continue;
        }
        if (WriteBlocks(g_sheet.texture, 0, it->second.inner, 16, it->second.original)) {
            RM_LOGI("gun looks: the picture at %d,%d is the game's again", it->first.first, it->first.second);
        }
        it = g_sheet.written.erase(it);
    }
    for (const auto& [key, wanted] : want) {
        if (g_sheet.written.count(key)) continue;
        Sheet::Written w;
        w.inner = Inner(wanted.first);
        w.look = wanted.second;
        if (!ReadBlocks(g_sheet.texture, 0, w.inner, 16, w.original)) {
            static int fails = 0;
            if (fails++ < 5) RM_LOGW("gun looks: the picture at %d,%d could not be read", key.first, key.second);
            continue;
        }
        if (!WriteBlocks(g_sheet.texture, 0, w.inner, 16, PictureBlocks(w.original, w.inner, w.look))) {
            static int fails = 0;
            if (fails++ < 5) RM_LOGW("gun looks: the picture at %d,%d could not be written", key.first, key.second);
            continue;
        }
        RM_LOGI("gun looks: the picture at %d,%d shows %s", key.first, key.second, w.look.Text().c_str());
        g_sheet.written.emplace(key, std::move(w));
    }
}

// ---- the models -----------------------------------------------------------------------------

struct Model {
    std::uintptr_t resource = 0;
    std::string path;
    IDirect3DTexture9* texture = nullptr;  // held
    std::size_t blockBytes = 0;            // 8 DXT1, 16 DXT3/DXT5
    std::vector<std::vector<std::uint8_t>> original;  // each level's blocks
    Look look;
};
std::vector<Model> g_models;

// A whole level's blocks (every level, the smallest too: a block covers what is under 4 pixels).
bool LevelBlocks(IDirect3DTexture9* t, UINT level, std::size_t blockBytes, std::vector<std::uint8_t>* read,
                 const std::vector<std::uint8_t>* write) {
    D3DSURFACE_DESC d{};
    if (!textures::LevelDesc(t, level, d)) return false;
    const std::size_t row = std::max<std::size_t>(1, (d.Width + 3) / 4) * blockBytes;
    const std::size_t rows = std::max<std::size_t>(1, (d.Height + 3) / 4);
    if (write && write->size() != row * rows) return false;
    D3DLOCKED_RECT locked{};
    if (!textures::Lock(t, level, nullptr, write ? 0 : D3DLOCK_READONLY, locked)) return false;
    if (read) read->resize(row * rows);
    bool ok = true;
    for (std::size_t i = 0; i < rows && ok; ++i) {
        const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(locked.pBits) + i * static_cast<std::size_t>(locked.Pitch);
        ok = write ? SafeWrite(at, write->data() + i * row, row) : SafeCopy(read->data() + i * row, at, row);
    }
    textures::Unlock(t, level);
    return ok;
}

// "pawn\wp\wp02\model\wp0200_bm" -> "wp02"; "" when not a gun's colour map.
std::string ModelMapFolder(const std::string& lower) {
    static const std::string kHead = "pawn\\wp\\";
    if (lower.rfind(kHead, 0) != 0 || lower.size() < kHead.size() + 4 + 7) return {};
    if (lower.compare(kHead.size() + 4, 7, "\\model\\") != 0) return {};
    if (lower.size() < 3 || lower.compare(lower.size() - 3, 3, "_bm") != 0) return {};
    return lower.substr(kHead.size(), 4);
}

void Release(Model& m) {
    if (m.texture) m.texture->Release();
    m.texture = nullptr;
}

// Reads a colour map's levels; false (logged once a path) when it is not one the tint knows.
bool TakeModel(Model& m) {
    IDirect3DTexture9* t = textures::D3DTextureOf(m.resource);
    D3DSURFACE_DESC desc{};
    static std::set<std::string> told;
    if (!t || !textures::LevelDesc(t, 0, desc)) return false;
    if (desc.Format == D3DFMT_DXT1) m.blockBytes = 8;
    else if (desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT5) m.blockBytes = 16;
    if (!m.blockBytes || desc.Pool == D3DPOOL_DEFAULT) {
        if (told.insert(m.path).second) RM_LOGW("gun looks: %s is format %08X pool %d; left alone", m.path.c_str(), static_cast<unsigned>(desc.Format), static_cast<int>(desc.Pool));
        return false;
    }
    const DWORD count = textures::LevelCount(t);
    for (DWORD level = 0; level < count; ++level) {
        std::vector<std::uint8_t> blocks;
        if (!LevelBlocks(t, level, m.blockBytes, &blocks, nullptr)) {
            if (told.insert(m.path).second) RM_LOGW("gun looks: %s level %lu could not be read; left alone", m.path.c_str(), level);
            return false;
        }
        m.original.push_back(std::move(blocks));
    }
    if (m.original.empty()) return false;
    t->AddRef();
    m.texture = t;
    return true;
}

bool WriteLevel(Model& m, std::size_t level, const std::vector<std::uint8_t>& blocks) {
    return LevelBlocks(m.texture, static_cast<UINT>(level), m.blockBytes, nullptr, &blocks);
}

bool PutBack(Model& m) {
    bool ok = true;
    for (std::size_t level = 0; level < m.original.size(); ++level) ok = WriteLevel(m, level, m.original[level]) && ok;
    return ok;
}

bool Recolour(Model& m, const Look& look) {
    const dxt::Tint tint = TintFor(look, g_options.modelTint);
    bool ok = true;
    for (std::size_t level = 0; level < m.original.size(); ++level) {
        std::vector<std::uint8_t> blocks = m.original[level];
        for (std::size_t at = 0; at + m.blockBytes <= blocks.size(); at += m.blockBytes) {
            dxt::TintColourBlock(blocks.data() + at + (m.blockBytes == 16 ? 8 : 0), m.blockBytes == 8, tint);
        }
        ok = WriteLevel(m, level, blocks) && ok;
    }
    return ok;
}

void ApplyModels(const std::vector<textures::Loaded>& maps, const std::map<std::string, Look>& want) {
    // What went (released, or loaded again: the game's own again).
    for (auto it = g_models.begin(); it != g_models.end();) {
        const bool here = std::any_of(maps.begin(), maps.end(), [&](const textures::Loaded& l) { return l.resource == it->resource; });
        if (!here || textures::D3DTextureOf(it->resource) != it->texture) {
            Release(*it);
            it = g_models.erase(it);
        } else {
            ++it;
        }
    }
    for (const textures::Loaded& map : maps) {
        std::string lower = map.path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto w = want.find(ModelMapFolder(lower));
        const Look look = w == want.end() ? Look{} : w->second;
        auto m = std::find_if(g_models.begin(), g_models.end(), [&](const Model& x) { return x.resource == map.resource; });
        if (m != g_models.end() && m->look == look) continue;
        if (m != g_models.end()) {
            PutBack(*m);
            if (look.element.empty()) {
                RM_LOGI("gun looks: %s is the game's again", m->path.c_str());
                Release(*m);
                g_models.erase(m);
                continue;
            }
        } else {
            if (look.element.empty()) continue;
            Model fresh;
            fresh.resource = map.resource;
            fresh.path = map.path;
            if (!TakeModel(fresh)) continue;
            g_models.push_back(std::move(fresh));
            m = g_models.end() - 1;
        }
        const bool ok = Recolour(*m, look);
        m->look = look;
        RM_LOGI("gun looks: %s (%zu levels) %s %s", m->path.c_str(), m->original.size(), ok ? "shows" : "could not be fully written for",
                look.Text().c_str());
    }
}

}  // namespace

void SetDirectory(const std::filesystem::path& configDir) {
    std::lock_guard lock(g_mutex);
    g_dir = configDir;
}

void SetOptions(const Options& options) {
    std::lock_guard lock(g_mutex);
    g_options = options;
    g_changed = true;
}

void SetWanted(std::vector<Gun> guns) {
    std::lock_guard lock(g_mutex);
    const auto same = [](const Gun& a, const Gun& b) { return a.item == b.item && a.element == b.element && a.evolved == b.evolved; };
    if (guns.size() == g_wanted.size() && std::equal(guns.begin(), guns.end(), g_wanted.begin(), same)) return;
    g_wanted = std::move(guns);
    g_changed = true;
}

void OnDraw(IDirect3DDevice9* device) {
    if (!device) return;
    std::lock_guard lock(g_mutex);
    if (!g_loaded) LoadConfig();
    if (!g_ok) return;
    if (g_wanted.empty() && g_sheet.written.empty() && g_models.empty()) return;
    // Looked at once a second (a texture loaded again since, or released), at once when the guns changed.
    const DWORD now = GetTickCount();
    if (!g_changed && now - g_lastLook < 1000) return;
    g_lastLook = now;
    g_changed = false;

    std::map<std::pair<int, int>, std::pair<Rect, Look>> pictures;
    std::map<std::string, Look> models;
    for (const Gun& gun : g_wanted) {
        const Look look{gun.element, gun.evolved};
        if (g_options.pictures) {
            if (const auto p = g_pictures.find(gun.item); p != g_pictures.end()) pictures.emplace(std::make_pair(p->second.x0, p->second.y0), std::make_pair(p->second, look));
        }
        if (g_options.models) {
            if (const auto f = g_folders.find(gun.item); f != g_folders.end()) models.emplace(f->second, look);
        }
    }
    std::uintptr_t sheet = 0;
    std::vector<textures::Loaded> maps;
    for (textures::Loaded& t : textures::Find([](const std::string& lower) { return lower == g_sheetPath || !ModelMapFolder(lower).empty(); })) {
        std::string lower = t.path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == g_sheetPath) {
            if (!sheet) sheet = t.resource;
        } else {
            maps.push_back(std::move(t));
        }
    }
    TakeSheet(sheet);
    ApplySheet(pictures);
    ApplyModels(maps, models);
}

}  // namespace rmre5::gunlooks
