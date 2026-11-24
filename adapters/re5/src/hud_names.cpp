// The characters' own names on the HUD. See hud_names.h.

#include "hud_names.h"

#include <windows.h>

#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine.h"
#include "icons.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"
#include "textures.h"

namespace rmre5::hudnames {
namespace {

constexpr int kBoxes = 2;  // 0 "Chris" (player 1's base character), 1 "Sheva"
constexpr const char* kBoxNames[kBoxes] = {"chris", "sheva"};
// The texture: 512x512 DXT5 (cp03 in every language).
constexpr UINT kTextureSize = 512;
// The search from the HUD objects: pointers followed this deep, objects read this far.
constexpr int kSearchDepth = 4;
constexpr std::size_t kObjectBytes = 0x300;
constexpr std::size_t kMaxVisited = 30000;

struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

std::mutex g_mutex;
std::filesystem::path g_dir;
bool g_loaded = false;
bool g_configOk = false;
std::array<Box, kBoxes> g_boxes;
std::map<std::uint64_t, std::string> g_fingerprints;                        // -> "latin" / "rus"
std::map<std::string, std::map<std::string, std::array<std::string, kBoxes>>> g_names;  // id -> script -> files
std::map<std::string, std::vector<unsigned char>> g_tiles;                 // file -> DXT5 blocks ({} failed)
std::map<std::string, std::string> g_labels;                                // id -> its name as text ("Leon")

std::array<std::string, kBoxes> g_wanted;
std::uintptr_t g_roots[2] = {0, 0};
bool g_probe = false;
DWORD g_lastSearch = 0;

// ---- the configuration ----------------------------------------------------------------------

void LoadConfig() {
    g_loaded = true;
    std::ifstream in(g_dir / L"hud_names.json");
    if (!in) {
        RM_LOGW("hud names: no %s", (g_dir / L"hud_names.json").string().c_str());
        return;
    }
    try {
        const nlohmann::json j = nlohmann::json::parse(in);
        for (int b = 0; b < kBoxes; ++b) {
            const auto& v = j.at("boxes").at(kBoxNames[b]);
            g_boxes[static_cast<std::size_t>(b)] = {v.at(0).get<int>(), v.at(1).get<int>(), v.at(2).get<int>(), v.at(3).get<int>()};
        }
        for (const auto& [hash, script] : j.at("fingerprints").items()) {
            g_fingerprints[std::stoull(hash, nullptr, 16)] = script.get<std::string>();
        }
        for (const auto& [id, scripts] : j.at("names").items()) {
            for (const auto& [script, boxes] : scripts.items()) {
                auto& files = g_names[id][script];
                for (int b = 0; b < kBoxes; ++b) files[static_cast<std::size_t>(b)] = boxes.value(kBoxNames[b], std::string());
            }
        }
        if (j.contains("labels")) {
            for (const auto& [id, label] : j.at("labels").items()) g_labels[id] = label.get<std::string>();
        }
        for (const Box& box : g_boxes) {
            if (box.x0 < 0 || box.y0 < 0 || box.x1 > static_cast<int>(kTextureSize) || box.y1 > static_cast<int>(kTextureSize) ||
                box.x0 % 4 || box.y0 % 4 || box.x1 % 4 || box.y1 % 4 || box.x1 <= box.x0 || box.y1 <= box.y0) {
                RM_LOGW("hud names: a box is not whole 4x4 blocks inside the texture");
                return;
            }
        }
        g_configOk = true;
        RM_LOGI("hud names: %zu characters, %zu texture fingerprints", g_names.size(), g_fingerprints.size());
    } catch (const std::exception& e) {
        RM_LOGW("hud names: hud_names.json: %s", e.what());
    }
}

// ---- DXT5 -----------------------------------------------------------------------------------

struct Rgb {
    int r, g, b;
};

std::uint16_t Pack565(const Rgb& c) {
    return static_cast<std::uint16_t>((((c.r * 31 + 127) / 255) << 11) | (((c.g * 63 + 127) / 255) << 5) | ((c.b * 31 + 127) / 255));
}

Rgb Unpack565(std::uint16_t c) { return {((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31}; }

// BGRA pixels (w x h, both multiples of 4) as DXT5 blocks, row of blocks by row of blocks.
std::vector<unsigned char> EncodeDxt5(const std::vector<unsigned char>& bgra, unsigned w, unsigned h) {
    std::vector<unsigned char> out;
    out.reserve(static_cast<std::size_t>(w / 4) * (h / 4) * 16);
    for (unsigned by = 0; by < h / 4; ++by) {
        for (unsigned bx = 0; bx < w / 4; ++bx) {
            int a[16];
            Rgb c[16];
            for (int i = 0; i < 16; ++i) {
                const std::size_t p = (static_cast<std::size_t>(by * 4 + i / 4) * w + bx * 4 + i % 4) * 4;
                c[i] = {bgra[p + 2], bgra[p + 1], bgra[p]};
                a[i] = bgra[p + 3];
            }
            unsigned char block[16] = {};
            // Alpha: the block's highest and lowest, eight steps between.
            int a0 = *std::max_element(a, a + 16), a1 = *std::min_element(a, a + 16);
            if (a0 == a1) {
                if (a0 < 255) ++a0;
                else a1 = 254;
            }
            int ramp[8] = {a0, a1};
            for (int k = 1; k <= 6; ++k) ramp[k + 1] = ((7 - k) * a0 + k * a1) / 7;
            std::uint64_t abits = 0;
            for (int i = 0; i < 16; ++i) {
                int best = 0;
                for (int k = 1; k < 8; ++k) {
                    if (std::abs(a[i] - ramp[k]) < std::abs(a[i] - ramp[best])) best = k;
                }
                abits |= static_cast<std::uint64_t>(best) << (3 * i);
            }
            block[0] = static_cast<unsigned char>(a0);
            block[1] = static_cast<unsigned char>(a1);
            for (int k = 0; k < 6; ++k) block[2 + k] = static_cast<unsigned char>(abits >> (8 * k));
            // Colour: the brightest and darkest of the visible texels.
            int hi = -1, lo = -1;
            for (int pass = 0; pass < 2 && hi < 0; ++pass) {
                for (int i = 0; i < 16; ++i) {
                    if (pass == 0 && a[i] <= 8) continue;
                    const int l = c[i].r * 3 + c[i].g * 6 + c[i].b;
                    if (hi < 0 || l > c[hi].r * 3 + c[hi].g * 6 + c[hi].b) hi = i;
                    if (lo < 0 || l < c[lo].r * 3 + c[lo].g * 6 + c[lo].b) lo = i;
                }
            }
            std::uint16_t c0 = Pack565(c[hi]), c1 = Pack565(c[lo]);
            if (c0 < c1) std::swap(c0, c1);
            const Rgb p0 = Unpack565(c0), p1 = Unpack565(c1);
            const Rgb pal[4] = {p0, p1, {(2 * p0.r + p1.r) / 3, (2 * p0.g + p1.g) / 3, (2 * p0.b + p1.b) / 3},
                                {(p0.r + 2 * p1.r) / 3, (p0.g + 2 * p1.g) / 3, (p0.b + 2 * p1.b) / 3}};
            std::uint32_t cbits = 0;
            if (c0 != c1) {
                for (int i = 0; i < 16; ++i) {
                    int best = 0, bestDist = INT_MAX;
                    for (int k = 0; k < 4; ++k) {
                        const int dr = c[i].r - pal[k].r, dg = c[i].g - pal[k].g, db = c[i].b - pal[k].b;
                        const int dist = dr * dr + dg * dg + db * db;
                        if (dist < bestDist) {
                            bestDist = dist;
                            best = k;
                        }
                    }
                    cbits |= static_cast<std::uint32_t>(best) << (2 * i);
                }
            }
            block[8] = static_cast<unsigned char>(c0);
            block[9] = static_cast<unsigned char>(c0 >> 8);
            block[10] = static_cast<unsigned char>(c1);
            block[11] = static_cast<unsigned char>(c1 >> 8);
            for (int k = 0; k < 4; ++k) block[12 + k] = static_cast<unsigned char>(cbits >> (8 * k));
            out.insert(out.end(), block, block + 16);
        }
    }
    return out;
}

// A tile's DXT5 blocks (its PNG must be the box's size).
const std::vector<unsigned char>& Tile(const std::string& file, const Box& box) {
    auto it = g_tiles.find(file);
    if (it != g_tiles.end()) return it->second;
    std::vector<unsigned char> blocks;
    unsigned w = 0, h = 0;
    std::vector<unsigned char> pixels;
    if (!icons::DecodePng(g_dir / file, w, h, pixels)) {
        RM_LOGW("hud names: %s cannot be read", file.c_str());
    } else if (static_cast<int>(w) != box.x1 - box.x0 || static_cast<int>(h) != box.y1 - box.y0) {
        RM_LOGW("hud names: %s is %ux%u, its box %dx%d", file.c_str(), w, h, box.x1 - box.x0, box.y1 - box.y0);
    } else {
        blocks = EncodeDxt5(pixels, w, h);
    }
    return g_tiles.emplace(file, std::move(blocks)).first->second;
}

// ---- the texture ----------------------------------------------------------------------------
//
// An rTexture keeps its path at +4 ("id\eng\cockpit\cp03_360_NOMIP_BM") and its D3D texture
// (textures.h). The language archive loads its own cp03 and the Japanese one; both are written (the
// HUD's layouts use the language's).

using textures::D3DTextureOf;

bool LevelDesc(IDirect3DTexture9* t, D3DSURFACE_DESC& desc) { return textures::LevelDesc(t, 0, desc); }

bool LockBlocks(IDirect3DTexture9* t, const Box& box, DWORD flags, D3DLOCKED_RECT& locked) {
    RECT rect{box.x0, box.y0, box.x1, box.y1};
    return textures::Lock(t, 0, &rect, flags, locked);
}

void Unlock(IDirect3DTexture9* t) { textures::Unlock(t, 0); }

// A box's DXT5 blocks, block row by block row.
bool ReadBox(IDirect3DTexture9* t, const Box& box, std::vector<unsigned char>& out) {
    D3DLOCKED_RECT locked{};
    if (!LockBlocks(t, box, D3DLOCK_READONLY, locked)) return false;
    const std::size_t row = static_cast<std::size_t>(box.x1 - box.x0) / 4 * 16;
    const int rows = (box.y1 - box.y0) / 4;
    out.resize(row * rows);
    bool ok = true;
    for (int r = 0; r < rows && ok; ++r) {
        ok = SafeCopy(out.data() + r * row, reinterpret_cast<std::uintptr_t>(locked.pBits) + static_cast<std::uintptr_t>(r) * locked.Pitch, row);
    }
    Unlock(t);
    return ok;
}

bool WriteBox(IDirect3DTexture9* t, const Box& box, const std::vector<unsigned char>& blocks) {
    const std::size_t row = static_cast<std::size_t>(box.x1 - box.x0) / 4 * 16;
    const int rows = (box.y1 - box.y0) / 4;
    if (blocks.size() != row * rows) return false;
    D3DLOCKED_RECT locked{};
    if (!LockBlocks(t, box, 0, locked)) return false;
    bool ok = true;
    for (int r = 0; r < rows && ok; ++r) {
        ok = SafeWrite(reinterpret_cast<std::uintptr_t>(locked.pBits) + static_cast<std::uintptr_t>(r) * locked.Pitch, blocks.data() + r * row, row);
    }
    Unlock(t);
    return ok;
}

// The fingerprint of the boxes' bytes: FNV-1a 64 over the block rows from the texture's top left
// to the boxes' right and bottom (scripts/hud_names.py fingerprint).
std::uint64_t Fingerprint(IDirect3DTexture9* t) {
    Box all{0, 0, 0, 0};
    for (const Box& b : g_boxes) {
        all.x1 = std::max(all.x1, b.x1);
        all.y1 = std::max(all.y1, b.y1);
    }
    std::vector<unsigned char> bytes;
    if (!ReadBox(t, all, bytes)) return 0;
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : bytes) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

// A cp03 the names are written in.
struct Target {
    std::uintptr_t resource = 0;
    std::string path;
    IDirect3DTexture9* texture = nullptr;  // held (AddRef)
    std::string script;
    std::array<std::vector<unsigned char>, kBoxes> original;
    std::array<std::string, kBoxes> applied;
};
std::vector<Target> g_targets;
std::set<std::uintptr_t> g_rejected;  // rTextures looked at and not taken

bool IsNamesPath(const std::string& path) {
    const std::string tail = "\\cockpit\\cp03_360_NOMIP_BM";
    return path.size() > tail.size() && path.compare(0, 3, "id\\") == 0 && path.compare(path.size() - tail.size(), tail.size(), tail) == 0;
}

// ---- the search (a worker thread: it only reads memory) --------------------------------------

std::atomic<bool> g_searching{false};
std::vector<std::uintptr_t> g_found;  // rTextures of the names' path, from the last search (g_mutex)
bool g_foundReady = false;
std::map<std::string, std::uintptr_t> g_seen;  // every rTexture the last search met, by path (g_mutex)

// Developer (the UI lab): PNGs to write into textures, and what they covered.
struct LabRequest {
    std::string pathPart;
    int x = 0, y = 0;
    std::filesystem::path png;
};
struct LabUndo {
    IDirect3DTexture9* texture = nullptr;  // held
    Box box;
    std::vector<unsigned char> original;
    std::string path;
};
std::vector<LabRequest> g_labRequests;
std::vector<LabUndo> g_labUndo;
bool g_labRestore = false;

// From the HUD objects, the rTextures within kSearchDepth pointers whose path is a cp03.
void Search(std::array<std::uintptr_t, 2> roots, bool log) {
    static std::uintptr_t textureDti = 0;
    std::vector<std::pair<std::uintptr_t, int>> queue;
    std::set<std::uintptr_t> visited;
    for (std::uintptr_t root : roots) {
        if (root) queue.emplace_back(root, 0);
    }
    std::vector<std::uintptr_t> found;
    std::map<std::string, std::uintptr_t> seen;
    std::vector<unsigned char> buffer(kObjectBytes);
    const DWORD started = GetTickCount();
    for (std::size_t i = 0; i < queue.size() && visited.size() < kMaxVisited; ++i) {
        const auto [object, depth] = queue[i];
        if (!visited.insert(object).second) continue;
        if (!SafeCopy(buffer.data(), object, buffer.size())) continue;
        for (std::size_t off = 0; off + 4 <= buffer.size(); off += 4) {
            std::uintptr_t p = 0;
            std::memcpy(&p, buffer.data() + off, 4);
            if (p < 0x10000 || p >= 0x7FFF0000 || (p & 3) || visited.count(p)) continue;
            const std::uintptr_t dti = engine::DtiOf(p);
            if (dti && (dti == textureDti || (!textureDti && engine::DtiName(dti) == "rTexture"))) {
                textureDti = dti;
                visited.insert(p);
                const std::string path = ReadCString(p + 4, 64);
                if (log) RM_LOGW("hud names probe: rTexture %08X %s", static_cast<unsigned>(p), path.c_str());
                if (IsNamesPath(path)) found.push_back(p);
                seen.emplace(path, p);
            } else if (depth + 1 < kSearchDepth) {
                queue.emplace_back(p, depth + 1);
            }
        }
    }
    RM_LOGI("hud names: searched %zu objects in %lu ms, %zu names texture(s)", visited.size(), GetTickCount() - started, found.size());
    std::lock_guard lock(g_mutex);
    g_found = std::move(found);
    g_foundReady = true;
    for (const auto& [path, p] : seen) g_seen[path] = p;
}

// Render thread: the UI lab's writes and restores.
void RunLab() {
    if (g_labRestore) {
        g_labRestore = false;
        for (auto it = g_labUndo.rbegin(); it != g_labUndo.rend(); ++it) {
            WriteBox(it->texture, it->box, it->original);
            it->texture->Release();
            RM_LOGW("ui lab: %s (%d,%d)-(%d,%d) put back", it->path.c_str(), it->box.x0, it->box.y0, it->box.x1, it->box.y1);
        }
        g_labUndo.clear();
    }
    for (auto it = g_labRequests.begin(); it != g_labRequests.end();) {
        std::uintptr_t resource = 0;
        std::string path;
        for (const auto& [p, r] : g_seen) {
            if (p.find(it->pathPart) != std::string::npos) {
                resource = r;
                path = p;
                break;
            }
        }
        if (!resource) {
            ++it;  // after the next search
            continue;
        }
        IDirect3DTexture9* t = D3DTextureOf(resource);
        D3DSURFACE_DESC desc{};
        unsigned w = 0, h = 0;
        std::vector<unsigned char> pixels;
        const bool shape = t && LevelDesc(t, desc) && desc.Format == D3DFMT_DXT5;
        const bool png = icons::DecodePng(it->png, w, h, pixels);
        Box box{it->x, it->y, it->x + static_cast<int>(w), it->y + static_cast<int>(h)};
        std::vector<unsigned char> original;
        if (!shape || !png || w % 4 || h % 4 || box.x0 % 4 || box.y0 % 4 || box.x1 > static_cast<int>(desc.Width) ||
            box.y1 > static_cast<int>(desc.Height) || !ReadBox(t, box, original)) {
            RM_LOGW("ui lab: %s not written (%s, %s %ux%u at %d,%d)", path.c_str(), shape ? "DXT5" : "not a DXT5 texture",
                    png ? "PNG" : "PNG unreadable", w, h, it->x, it->y);
        } else if (WriteBox(t, box, EncodeDxt5(pixels, w, h))) {
            t->AddRef();
            g_labUndo.push_back({t, box, std::move(original), path});
            RM_LOGW("ui lab: %s: %s written at %d,%d (%ux%u)", path.c_str(), it->png.filename().string().c_str(), it->x, it->y, w, h);
        }
        it = g_labRequests.erase(it);
    }
}

void Forget(Target& t) {
    if (t.texture) t.texture->Release();
    t.texture = nullptr;
}

// Render thread: the found rTextures that are a known cp03 become targets.
void TakeFound() {
    for (std::uintptr_t resource : g_found) {
        if (g_rejected.count(resource) ||
            std::any_of(g_targets.begin(), g_targets.end(), [&](const Target& t) { return t.resource == resource; })) {
            continue;
        }
        Target target;
        target.resource = resource;
        target.path = ReadCString(resource + 4, 64);
        IDirect3DTexture9* t = D3DTextureOf(resource);
        D3DSURFACE_DESC desc{};
        const bool shape = t && LevelDesc(t, desc) && desc.Width == kTextureSize && desc.Height == kTextureSize && desc.Format == D3DFMT_DXT5;
        const std::uint64_t print = shape ? Fingerprint(t) : 0;
        const auto known = g_fingerprints.find(print);
        bool read = known != g_fingerprints.end();
        for (int b = 0; b < kBoxes && read; ++b) read = ReadBox(t, g_boxes[static_cast<std::size_t>(b)], target.original[static_cast<std::size_t>(b)]);
        if (!read) {
            RM_LOGW("hud names: %s not taken (texture %s, fingerprint %016llX)", target.path.c_str(), t ? (shape ? "512x512 DXT5" : "of another shape") : "not found",
                    static_cast<unsigned long long>(print));
            g_rejected.insert(resource);
            continue;
        }
        t->AddRef();
        target.texture = t;
        target.script = known->second;
        RM_LOGI("hud names: %s (%s names, pool %d)", target.path.c_str(), target.script.c_str(), static_cast<int>(desc.Pool));
        g_targets.push_back(std::move(target));
    }
    g_found.clear();
}

void Apply(Target& target, const std::array<std::string, kBoxes>& want) {
    for (int b = 0; b < kBoxes; ++b) {
        const auto i = static_cast<std::size_t>(b);
        std::string desired = want[i];
        const std::vector<unsigned char>* blocks = &target.original[i];
        if (!desired.empty()) {
            const auto id = g_names.find(desired);
            std::string file;
            if (id != g_names.end()) {
                const auto script = id->second.find(target.script);
                if (script != id->second.end()) file = script->second[i];
            }
            const std::vector<unsigned char>& tile = file.empty() ? target.original[i] : Tile(file, g_boxes[i]);
            if (file.empty() || tile.empty()) desired.clear();
            else blocks = &tile;
        }
        if (desired == target.applied[i]) continue;
        if (WriteBox(target.texture, g_boxes[i], *blocks)) {
            RM_LOGI("hud names: %s: the %s box shows %s", target.path.c_str(), kBoxNames[b], desired.empty() ? "the game's name" : desired.c_str());
            target.applied[i] = desired;
        } else {
            static int fails = 0;
            if (fails++ < 5) RM_LOGW("hud names: %s: the %s box could not be written", target.path.c_str(), kBoxNames[b]);
        }
    }
}

}  // namespace

void SetDirectory(const std::filesystem::path& dir) {
    std::lock_guard lock(g_mutex);
    g_dir = dir;
}

// The HUD's text names: the executable's tables of the characters' names (entry 0 "Chris", 1
// "Sheva") point at these while a run plays as another character in those slots.
constexpr std::uintptr_t kNameTables[] = {re5::addr::kInventoryNameTable, re5::addr::kCockpitNameTable};
constexpr const char* kGameNames[kBoxes] = {"Chris", "Sheva"};
std::array<std::array<char, 24>, kBoxes> g_labelText{};
std::array<std::array<std::uintptr_t, kBoxes>, 2> g_labelOriginal{};
std::array<std::string, kBoxes> g_labelShown;

void ApplyLabels() {
    if (!engine::Caps().hudNameTables) return;
    for (int b = 0; b < kBoxes; ++b) {
        const auto i = static_cast<std::size_t>(b);
        const auto label = g_labels.find(g_wanted[i]);
        const std::string text = g_wanted[i].empty() || label == g_labels.end() ? std::string() : label->second.substr(0, 23);
        if (text == g_labelShown[i]) continue;
        const auto ours = reinterpret_cast<std::uintptr_t>(g_labelText[i].data());
        for (std::size_t t = 0; t < std::size(kNameTables); ++t) {
            const std::uintptr_t entry = kNameTables[t] + i * 4;
            const auto current = ReadOr<std::uintptr_t>(entry, 0);
            if (text.empty()) {
                if (current == ours && g_labelOriginal[t][i]) SafeStore(entry, g_labelOriginal[t][i]);
                continue;
            }
            if (current != ours) {
                // Only over the game's own name.
                if (ReadCString(current, 16) != kGameNames[b]) {
                    RM_LOGW("hud names: the text name table %08X holds \"%s\", not %s; left alone", static_cast<unsigned>(kNameTables[t]),
                            ReadCString(current, 16).c_str(), kGameNames[b]);
                    continue;
                }
                g_labelOriginal[t][i] = current;
            }
        }
        if (!text.empty()) {
            std::array<char, 24> buffer{};
            std::memcpy(buffer.data(), text.c_str(), text.size());
            g_labelText[i] = buffer;
            for (std::size_t t = 0; t < std::size(kNameTables); ++t) {
                if (g_labelOriginal[t][i]) SafeStore(kNameTables[t] + i * 4, ours);
            }
        }
        RM_LOGI("hud names: the text names say %s for %s", text.empty() ? "the game's" : text.c_str(), kGameNames[b]);
        g_labelShown[i] = text;
    }
}

void SetWanted(const std::string& chrisBox, const std::string& shevaBox, std::uintptr_t cockpit0, std::uintptr_t cockpit1) {
    std::lock_guard lock(g_mutex);
    if (!g_loaded) LoadConfig();
    g_wanted = {chrisBox, shevaBox};
    g_roots[0] = cockpit0;
    g_roots[1] = cockpit1;
    if (g_configOk) ApplyLabels();
}

bool HasName(const std::string& character) {
    std::lock_guard lock(g_mutex);
    if (!g_loaded) LoadConfig();
    return g_names.count(character) != 0;
}

void Probe() {
    std::lock_guard lock(g_mutex);
    g_probe = true;
    g_lastSearch = 0;
    for (const Target& t : g_targets) {
        RM_LOGW("hud names probe: target %s, boxes %s / %s", t.path.c_str(), t.applied[0].c_str(), t.applied[1].c_str());
    }
}

void LabPut(const std::string& pathPart, int x, int y, const std::filesystem::path& png) {
    std::lock_guard lock(g_mutex);
    g_labRequests.push_back({pathPart, x, y, png});
    g_lastSearch = 0;  // look again now
}

void LabRestore() {
    std::lock_guard lock(g_mutex);
    g_labRestore = true;
    g_labRequests.clear();
}

void ProbeFrom(std::uintptr_t object) {
    std::thread([object] { Search({object, 0}, true); }).detach();
}

void OnDraw(IDirect3DDevice9* device) {
    if (!device) return;
    std::lock_guard lock(g_mutex);
    if (!g_loaded) LoadConfig();
    if (!g_configOk) return;
    if (g_foundReady) {
        g_foundReady = false;
        TakeFound();
    }
    // The game's texture may have gone (its rTexture no longer holds it): forget it. Once a second.
    static DWORD lastCheck = 0;
    const DWORD tick = GetTickCount();
    const bool check = tick - lastCheck >= 1000;
    if (check) lastCheck = tick;
    for (auto it = g_targets.begin(); it != g_targets.end();) {
        if (check && D3DTextureOf(it->resource) != it->texture) {
            RM_LOGW("hud names: %s went away", it->path.c_str());
            Forget(*it);
            it = g_targets.erase(it);
        } else {
            ++it;
        }
    }
    RunLab();
    const bool any = !g_wanted[0].empty() || !g_wanted[1].empty() || !g_labRequests.empty();
    // Looked for (on a worker) while a name is wanted and none is found, at most every 10 s.
    const bool labWaits = !g_labRequests.empty();
    if ((any || g_probe) && (g_targets.empty() || labWaits) && !g_searching.load() && (g_roots[0] || g_roots[1])) {
        const DWORD now = GetTickCount();
        if (!g_lastSearch || now - g_lastSearch >= 10000) {
            g_lastSearch = now;
            g_searching = true;
            const std::array<std::uintptr_t, 2> roots{g_roots[0], g_roots[1]};
            const bool log = g_probe;
            g_probe = false;
            std::thread([roots, log] {
                Search(roots, log);
                g_searching = false;
            }).detach();
        }
    }
    for (Target& t : g_targets) Apply(t, g_wanted);
}

}  // namespace rmre5::hudnames
