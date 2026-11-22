#include "music.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <random>

#include "log.h"
#include "memory.h"
#include "re5_layout.h"
#include "rm/music.h"

namespace rmre5::music {
namespace {

// Where a list's entries are in the game's memory: entry 0's size field, the distance between
// entries, and each field from an entry's size field (found by the values the file on disk holds).
struct Layout {
    bool found = false;
    std::uintptr_t first = 0;
    std::size_t stride = 0;
    int length = 4, channels = 8, loopStart = 12, loopEnd = 16;
};

struct List {
    const char* resource;  // the loaded list's resource path
    const wchar_t* file;   // its file, under nativePC_MT
    std::vector<rm::music::StreamEntry> entries;
    Layout layout;
    std::uintptr_t searchedIn = 0;  // the resource the layout was found in (looked for again in another)
    bool missingLogged = false;
};

std::array<List, 2> g_lists = {List{"sound\\bgm\\st_bgm", L"sound\\bgm\\st_bgm.stq", {}, {}, 0, false},
                               List{"sound\\bgm_dc\\st_bgm_dc", L"sound\\bgm_dc\\st_bgm_dc.stq", {}, {}, 0, false}};
bool g_loaded = false;
std::filesystem::path g_game;  // ...\nativePC_MT
std::filesystem::path g_dataDir;
std::string g_theme;  // its path, lower case; empty while not learned
bool g_themeRead = false;
rm::music::Rotation g_rotation{(static_cast<std::uint64_t>(std::random_device{}()) << 32) | std::random_device{}()};
int g_opensLogged = 0;
double g_armedAt = -1.0e9;  // when the last arena's track was chosen

// The theme's entry while another track plays in its place (its own values kept to put back).
struct Patch {
    bool on = false;
    int list = -1;
    std::size_t index = 0;
    std::uint32_t saved[5] = {};  // size, length, channels, loop start, loop end
    std::string track;
    std::wstring tail, target;
};
Patch g_patch;

std::string Lower(std::string s) {
    for (char& c : s) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Narrow(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) out += c < 0x80 ? static_cast<char>(c) : '?';
    return out;
}

void LoadLists() {
    if (g_loaded) return;
    g_loaded = true;
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return;
    g_game = std::filesystem::path(exe).parent_path() / "nativePC_MT";
    for (List& l : g_lists) {
        std::ifstream in(g_game / l.file, std::ios::binary);
        if (!in) {
            RM_LOGI("music: no %s", Narrow(l.file).c_str());
            continue;
        }
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        l.entries = rm::music::ParseStreamList(bytes, &error);
        int battle = 0;
        for (const auto& e : l.entries) battle += rm::music::IsBattleTrack(e) ? 1 : 0;
        RM_LOGI("music: %s, %zu tracks, %d battle tracks%s%s", Narrow(l.file).c_str(), l.entries.size(), battle, error.empty() ? "" : ": ",
                error.c_str());
    }
}

void ReadTheme() {
    if (g_themeRead || g_dataDir.empty()) return;
    g_themeRead = true;
    std::ifstream in(g_dataDir / L"music_theme.txt");
    std::string line;
    if (in && std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        g_theme = Lower(line);
        RM_LOGI("music: the Mercenaries theme is %s (music_theme.txt)", g_theme.c_str());
    }
}

struct Found {
    int list = -1;
    std::size_t index = 0;
    [[nodiscard]] const rm::music::StreamEntry& entry() const { return g_lists[static_cast<std::size_t>(list)].entries[index]; }
};

std::optional<Found> FindEntry(const std::string& lowerPath) {
    for (std::size_t l = 0; l < g_lists.size(); ++l) {
        const auto& entries = g_lists[l].entries;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (Lower(entries[i].path) == lowerPath) return Found{static_cast<int>(l), i};
        }
    }
    return std::nullopt;
}

bool FileExists(const std::filesystem::path& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::uintptr_t FindResource(const char* path) {
    const auto manager = ReadOr<std::uintptr_t>(re5::addr::kResourceManager, 0);
    if (!manager) return 0;
    for (int i = 0; i < re5::addr::kResourceTableSlots; ++i) {
        const auto r = ReadOr<std::uintptr_t>(manager + re5::addr::kResourceTable + static_cast<std::size_t>(i) * 4, 0);
        if (!r) continue;
        if (Lower(ReadCString(r + re5::addr::kResourcePath, 64)) == path) return r;
    }
    return 0;
}

// The loaded resources whose path has "bgm" (when a list is not found, to tell what it is called).
std::string BgmResources() {
    std::string out;
    const auto manager = ReadOr<std::uintptr_t>(re5::addr::kResourceManager, 0);
    for (int i = 0; manager && i < re5::addr::kResourceTableSlots && out.size() < 400; ++i) {
        const auto r = ReadOr<std::uintptr_t>(manager + re5::addr::kResourceTable + static_cast<std::size_t>(i) * 4, 0);
        if (!r) continue;
        const std::string p = ReadCString(r + re5::addr::kResourcePath, 64);
        if (Lower(p).find("bgm") != std::string::npos) out += " " + p;
    }
    return out.empty() ? " none" : out;
}

std::vector<std::uint8_t> ReadRegion(std::uintptr_t at, std::size_t size) {
    std::vector<std::uint8_t> out;
    constexpr std::size_t kPage = 0x1000;
    std::size_t done = 0;
    while (done < size) {
        const std::size_t chunk = std::min(kPage - ((at + done) & (kPage - 1)), size - done);
        const std::size_t old = out.size();
        out.resize(old + chunk);
        if (!SafeCopy(out.data() + old, at + done, chunk)) {
            out.resize(old);
            break;
        }
        done += chunk;
    }
    return out;
}

// An address `off` bytes from `base` (an offset may be negative).
std::uintptr_t Off(std::uintptr_t base, int off) { return base + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(off)); }

std::uint32_t U32(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

// The list's entries in memory: found by entry 0's size and length side by side, then entry 1's,
// then every entry's; the other fields where every entry has its own value.
bool Locate(List& l) {
    const std::uintptr_t res = FindResource(l.resource);
    if (!res) {
        if (!l.missingLogged) RM_LOGI("music: %s is not in memory (loaded:%s)", l.resource, BgmResources().c_str());
        l.missingLogged = true;
        return false;
    }
    const auto& e = l.entries;
    if (e.size() < 2) return false;
    if (l.layout.found && l.searchedIn == res && ReadOr<std::uint32_t>(l.layout.first + 4, 0) == e[0].length) return true;
    l.searchedIn = res;
    l.layout = {};
    std::vector<std::uintptr_t> starts = {res};
    const auto head = ReadRegion(res, 0x200);
    for (std::size_t o = 0; o + 4 <= head.size(); o += 4) {
        const std::uint32_t d = U32(head, o);
        if (d >= 0x10000 && d < 0x7FFF0000 && (d & 3) == 0 && d != res) starts.push_back(d);
    }
    const std::size_t span = std::min<std::size_t>(e.size() * 0x40 + 0x200, 0x20000);
    for (const std::uintptr_t s : starts) {
        const auto buf = ReadRegion(s, span);
        for (std::size_t o = 0; o + 8 <= buf.size(); o += 4) {
            if (U32(buf, o) != e[0].size || U32(buf, o + 4) != e[0].length) continue;
            for (std::size_t stride = 8; stride <= 0x100 && o + stride + 8 <= buf.size(); stride += 4) {
                if (U32(buf, o + stride) != e[1].size || U32(buf, o + stride + 4) != e[1].length) continue;
                const std::uintptr_t first = s + o;
                bool all = true;
                for (std::size_t i = 0; i < e.size() && all; ++i) {
                    all = ReadOr<std::uint32_t>(first + i * stride, 0) == e[i].size && ReadOr<std::uint32_t>(first + i * stride + 4, 0) == e[i].length;
                }
                if (!all) continue;
                const auto fieldAt = [&](std::uint32_t rm::music::StreamEntry::*field) {
                    for (int off = -16; off < static_cast<int>(stride); off += 4) {
                        if (off == 0 || off == 4) continue;
                        bool ok = true;
                        for (std::size_t i = 0; i < e.size() && ok; ++i) {
                            ok = ReadOr<std::uint32_t>(Off(first + i * stride, off), 0xDEADBEEFu) == e[i].*field;
                        }
                        if (ok) return off;
                    }
                    return INT_MIN;
                };
                Layout found;
                found.first = first;
                found.stride = stride;
                found.channels = fieldAt(&rm::music::StreamEntry::channels);
                found.loopStart = fieldAt(&rm::music::StreamEntry::loop_start);
                found.loopEnd = fieldAt(&rm::music::StreamEntry::loop_end);
                if (found.channels == INT_MIN || found.loopStart == INT_MIN || found.loopEnd == INT_MIN) {
                    RM_LOGW("music: %s's sizes are at %08X (every %zu bytes) but not all its other fields (channels %d, loop %d..%d)", l.resource,
                            static_cast<unsigned>(first), stride, found.channels, found.loopStart, found.loopEnd);
                    return false;
                }
                found.found = true;
                l.layout = found;
                RM_LOGI("music: %s in memory at %08X (resource %08X): %zu entries every %zu bytes, size +0, length +4, channels %+d, loop %+d..%+d",
                        l.resource, static_cast<unsigned>(first), static_cast<unsigned>(res), e.size(), stride, found.channels, found.loopStart,
                        found.loopEnd);
                return true;
            }
        }
    }
    std::string hex;
    for (std::size_t o = 0; o < std::min<std::size_t>(head.size(), 0x60); o += 4) {
        char part[12];
        std::snprintf(part, sizeof(part), " %08X", U32(head, o));
        hex += part;
    }
    RM_LOGW("music: %s (resource %08X) holds none of its entries' values; its first bytes:%s", l.resource, static_cast<unsigned>(res), hex.c_str());
    return false;
}

void Store(std::uintptr_t entry, const Layout& lay, const std::uint32_t values[5]) {
    SafeStore<std::uint32_t>(entry, values[0]);
    SafeStore<std::uint32_t>(Off(entry, lay.length), values[1]);
    SafeStore<std::uint32_t>(Off(entry, lay.channels), values[2]);
    SafeStore<std::uint32_t>(Off(entry, lay.loopStart), values[3]);
    SafeStore<std::uint32_t>(Off(entry, lay.loopEnd), values[4]);
}

void Restore() {
    if (!g_patch.on) return;
    List& l = g_lists[static_cast<std::size_t>(g_patch.list)];
    if (Locate(l)) Store(l.layout.first + g_patch.index * l.layout.stride, l.layout, g_patch.saved);
    RM_LOGI("music: the theme is its own again (it was %s)", g_patch.track.c_str());
    g_patch = {};
}

bool Apply(const Found& theme, const Found& track) {
    Restore();
    List& l = g_lists[static_cast<std::size_t>(theme.list)];
    const rm::music::StreamEntry& src = track.entry();
    const std::filesystem::path file = g_game / (src.path + ".sngw");
    if (!FileExists(file) || !Locate(l)) return false;
    const Layout& lay = l.layout;
    const std::uintptr_t at = lay.first + theme.index * lay.stride;
    Patch p;
    p.saved[0] = ReadOr<std::uint32_t>(at, 0);
    p.saved[1] = ReadOr<std::uint32_t>(Off(at, lay.length), 0);
    p.saved[2] = ReadOr<std::uint32_t>(Off(at, lay.channels), 0);
    p.saved[3] = ReadOr<std::uint32_t>(Off(at, lay.loopStart), 0);
    p.saved[4] = ReadOr<std::uint32_t>(Off(at, lay.loopEnd), 0);
    if (p.saved[0] != theme.entry().size || p.saved[1] != theme.entry().length) {
        RM_LOGW("music: the theme's entry holds %u/%u, not its own %u/%u; left alone", p.saved[0], p.saved[1], theme.entry().size, theme.entry().length);
        return false;
    }
    const std::uint32_t values[5] = {src.size, src.length, src.channels, src.loop_start, src.loop_end};
    Store(at, lay, values);
    p.on = true;
    p.list = theme.list;
    p.index = theme.index;
    p.track = src.Leaf();
    const std::string tail = Lower(theme.entry().path) + ".sngw";
    p.tail = fileredirect::NormaliseTail(std::wstring(tail.begin(), tail.end()));
    p.target = file.wstring();
    g_patch = p;
    return true;
}

}  // namespace

void Tick(const Context& c) {
    if (!c.dataDir.empty()) g_dataDir = c.dataDir;
    const auto opens = fileredirect::TakeStreamOpens();
    if (!opens.empty()) {
        LoadLists();
        ReadTheme();
    }
    for (const std::wstring& w : opens) {
        std::string path = Lower(Narrow(w));
        const std::size_t sound = path.find("sound\\");
        if (sound != std::string::npos) path = path.substr(sound);
        if (path.size() > 5 && path.compare(path.size() - 5, 5, ".sngw") == 0) path.resize(path.size() - 5);
        const auto f = FindEntry(path);
        if (g_opensLogged++ < 80) {
            RM_LOGI("music: the game opens %s (%s%s)", path.c_str(), !f ? "in no list" : f->entry().Loops() ? "loops" : "once",
                    g_patch.on && f && f->list == g_patch.list && f->index == g_patch.index ? (", read as " + g_patch.track).c_str() : "");
        }
        if (g_theme.empty() && f && f->entry().Loops() && c.inRun && c.inMatch && c.arenaOn && c.mercenariesMap && !g_patch.on) {
            g_theme = path;
            std::ofstream out(g_dataDir / L"music_theme.txt");
            out << path << "\n";
            RM_LOGI("music: the Mercenaries theme is %s; from the next arena it takes turns with the battle tracks", path.c_str());
        }
    }
    if (g_patch.on && (!c.enabled || (!c.inRun && c.now - g_armedAt > 60.0))) Restore();
}

void NextArena(bool enabled, double now) {
    g_armedAt = now;
    LoadLists();
    ReadTheme();
    if (!enabled || g_theme.empty()) {
        Restore();
        return;
    }
    const auto theme = FindEntry(g_theme);
    if (!theme) {
        RM_LOGW("music: the theme %s is in no list; the music stays", g_theme.c_str());
        Restore();
        return;
    }
    std::vector<std::string> pool = {g_theme};
    for (const List& l : g_lists) {
        for (const rm::music::StreamEntry& e : l.entries) {
            if (rm::music::IsBattleTrack(e) && FileExists(g_game / (e.path + ".sngw"))) pool.push_back(Lower(e.path));
        }
    }
    g_rotation.SetPool(pool);
    const std::string pick = g_rotation.Next();
    if (pick == g_theme) {
        Restore();
        RM_LOGI("music: this arena plays the Mercenaries theme (%zu tracks take turns)", pool.size());
        return;
    }
    const auto track = FindEntry(pick);
    if (!track || !Apply(*theme, *track)) {
        Restore();
        RM_LOGW("music: %s could not take the theme's place; the theme plays", pick.c_str());
        return;
    }
    RM_LOGI("music: this arena plays %s in the theme's place (%.0f s, %zu tracks take turns)", track->entry().Leaf().c_str(),
            track->entry().length / 48000.0, pool.size());
}

std::vector<fileredirect::Redirect> Redirects() {
    if (!g_patch.on) return {};
    return {fileredirect::Redirect{g_patch.tail, g_patch.target}};
}

std::string RedirectKey() { return g_patch.on ? Narrow(g_patch.target) : std::string(); }

}  // namespace rmre5::music
