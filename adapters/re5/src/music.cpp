#include "music.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <random>

#include "log.h"
#include "memory.h"
#include "rm/music.h"

namespace rmre5::music {
namespace {

struct List {
    const wchar_t* file;  // its file, under nativePC_MT
    std::vector<rm::music::StreamEntry> entries;
};

std::array<List, 2> g_lists = {List{L"sound\\bgm\\st_bgm.stq", {}}, List{L"sound\\bgm_dc\\st_bgm_dc.stq", {}}};
bool g_loaded = false;
std::filesystem::path g_game;  // ...\nativePC_MT
std::filesystem::path g_dataDir;
std::string g_theme;  // its path, lower case
bool g_themeRead = false;
rm::music::Rotation g_rotation{(static_cast<std::uint64_t>(std::random_device{}()) << 32) | std::random_device{}()};
int g_opensLogged = 0;
double g_armedAt = -1.0e9;  // when the last arena's track was chosen

// The next arena's track waits until no track plays: the swap is made between matches (the user's
// crash, 2026-11-22 21:16: armed as the match was ending, the playing b_bgm011, 94 s in, got b_bgm005's
// shorter length and loop points under it; both sound threads faulted, in the game's decoders
// 0x00455DC0 and 0x00670500, 0.4 s later). Made at the first tick 2 s after the match is
// over; given up (the theme then plays) if the next match is on first.
struct Pending {
    bool on = false;
    std::string pick;   // the theme's own path: the theme
    bool sawEnd = false; // the match it was armed in is over
};
Pending g_pending;
bool g_inMatch = false;
double g_matchOverAt = -1.0e9;

// The track in the theme's place (none while the theme is its own).
struct Patch {
    bool on = false;
    int list = -1;
    std::size_t index = 0;  // the theme's entry
    std::string track;
    std::wstring tail, target;
};
Patch g_patch;

// What the track takes in the game's memory (the user's runs, 2026-11-22, "themes don't loop"): every
// copy of the theme's stream list (the file as it is on disk: per entry its path's offset, size, length,
// channels, loop start, loop end; three of them in a match, the list's resource one of them) and every
// info block holding the theme's numbers as length, channels, size, loop start, loop end (one). With
// these the first arena's track looped. A search by the theme's loop points alone had also found the
// mod's own data (its parsed list, its locals on the stack) and changed it, which broke the later
// arenas: these are found by their layout, which none of the mod's has.
struct GameCopies {
    bool searched = false;
    int list = -1;                      // the theme's list
    std::vector<std::uintptr_t> lists;  // each copy's first entry
    std::vector<std::uintptr_t> infos;  // each info block's length field
};
GameCopies g_copies;
constexpr std::size_t kRecord = 0x18;

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
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        l.entries = rm::music::ParseStreamList(bytes, &error);
        // Its bytes go before the memory is given back: the search would take a freed copy for the game's.
        std::fill(bytes.begin(), bytes.end(), std::uint8_t{0});
        int battle = 0;
        for (const auto& e : l.entries) battle += rm::music::IsBattleTrack(e) ? 1 : 0;
        RM_LOGI("music: %s, %zu tracks, %d battle tracks%s%s", Narrow(l.file).c_str(), l.entries.size(), battle, error.empty() ? "" : ": ",
                error.c_str());
    }
}

// The Mercenaries theme: s_bgm020, opened as every match starts, on its maps and the campaign areas
// alike (the user's run, 2026-11-22: bgm008 at the results screen, bgm007 at the stage screen,
// s_bgm020 0.2 s after the match was detected and 2 s before its arena began). <data>\music_theme.txt
// names another (its stream's path, no extension).
constexpr const char* kDefaultTheme = "sound\\bgm\\oto\\bgm\\s_bgm\\s_bgm020";

void ReadTheme() {
    if (g_themeRead || g_dataDir.empty()) return;
    g_themeRead = true;
    g_theme = kDefaultTheme;
    std::ifstream in(g_dataDir / L"music_theme.txt");
    std::string line;
    if (in && std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) g_theme = Lower(line);
    }
    RM_LOGI("music: the Mercenaries theme is %s%s", g_theme.c_str(), g_theme == kDefaultTheme ? "" : " (music_theme.txt)");
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

std::uint32_t At(std::uintptr_t a) { return ReadOr<std::uint32_t>(a, 0xDEADBEEFu); }

// An entry's path field: its offset in the file, or (a list made ready) a pointer to its path.
bool PathFieldIs(std::uint32_t v, const rm::music::StreamEntry& e) {
    if (v == e.path_offset) return true;
    return v >= 0x10000 && v < 0x7FFF0000 && Lower(ReadCString(v, 80)) == Lower(e.path);
}

bool RecordIs(std::uintptr_t record, const rm::music::StreamEntry& e, bool numbers) {
    if (!PathFieldIs(At(record), e)) return false;
    return !numbers || (At(record + 4) == e.size && At(record + 8) == e.length);
}

// A copy of the list in memory whose first entry is at `first`: its first, second and last entries
// as on disk, and the theme's entry by its path's offset (its numbers may be a track's).
bool ListCopyAt(std::uintptr_t first, const List& l, std::size_t themeIndex) {
    const auto& e = l.entries;
    if (e.size() < 3 || themeIndex >= e.size()) return false;
    for (const std::size_t i : {std::size_t{0}, std::size_t{1}, e.size() - 1}) {
        if (i != themeIndex && !RecordIs(first + i * kRecord, e[i], true)) return false;
    }
    return RecordIs(first + themeIndex * kRecord, e[themeIndex], false);
}

// An info block holding one track's numbers (length, channels, size, loop start, loop end).
bool InfoHolds(std::uintptr_t at, const rm::music::StreamEntry& e) {
    return At(at) == e.length && At(at + 4) == e.channels && At(at + 8) == e.size && At(at + 12) == e.loop_start && At(at + 16) == e.loop_end;
}
bool InfoHoldsATrack(std::uintptr_t at) {
    for (const List& l : g_lists) {
        for (const auto& e : l.entries) {
            if (InfoHolds(at, e)) return true;
        }
    }
    return false;
}

void WriteRecord(std::uintptr_t first, std::size_t index, const rm::music::StreamEntry& v) {
    const std::uintptr_t r = first + index * kRecord;
    SafeStore<std::uint32_t>(r + 4, v.size);
    SafeStore<std::uint32_t>(r + 8, v.length);
    SafeStore<std::uint32_t>(r + 12, v.channels);
    SafeStore<std::uint32_t>(r + 16, v.loop_start);
    SafeStore<std::uint32_t>(r + 20, v.loop_end);
}

void WriteInfo(std::uintptr_t at, const rm::music::StreamEntry& v) {
    SafeStore<std::uint32_t>(at, v.length);
    SafeStore<std::uint32_t>(at + 4, v.channels);
    SafeStore<std::uint32_t>(at + 8, v.size);
    SafeStore<std::uint32_t>(at + 12, v.loop_start);
    SafeStore<std::uint32_t>(at + 16, v.loop_end);
}

// Every copy of the list and every info block holding the theme's numbers (called with the theme's
// numbers in place), in the process's own writable memory.
void FindGameCopies(const Found& theme) {
    const List& l = g_lists[static_cast<std::size_t>(theme.list)];
    const auto& e = l.entries;
    const rm::music::StreamEntry& t = theme.entry();
    g_copies = {};
    g_copies.searched = true;
    g_copies.list = theme.list;
    const DWORD started = GetTickCount();
    std::size_t scanned = 0;
    constexpr std::size_t kChunk = 0x10000, kOverlap = 0x20;
    std::vector<std::uint8_t> buf(kChunk + kOverlap);
    const auto own = reinterpret_cast<std::uintptr_t>(buf.data());
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    std::uintptr_t at = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const std::uintptr_t top = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);
    const auto add = [](std::vector<std::uintptr_t>& v, std::uintptr_t a) {
        if (v.size() < 16 && std::find(v.begin(), v.end(), a) == v.end()) v.push_back(a);
    };
    while (at < top) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof(mbi)) == 0) break;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t end = base + mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool writable = prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && writable && (mbi.Protect & PAGE_GUARD) == 0) {
            for (std::uintptr_t chunk = base; chunk < end; chunk += kChunk) {
                const std::size_t n = static_cast<std::size_t>(std::min<std::uintptr_t>(kChunk + kOverlap, end - chunk));
                if (chunk + n > own && chunk < own + buf.size()) continue;  // the search's own buffer
                if (!SafeCopy(buf.data(), chunk, n)) continue;
                scanned += n;
                for (std::size_t o = 4; o + 20 <= n; o += 4) {
                    std::uint32_t d = 0;
                    std::memcpy(&d, buf.data() + o, 4);
                    if (d == e[0].size) {
                        std::uint32_t len = 0, path = 0;
                        std::memcpy(&len, buf.data() + o + 4, 4);
                        std::memcpy(&path, buf.data() + o - 4, 4);
                        if (len == e[0].length && PathFieldIs(path, e[0]) && ListCopyAt(chunk + o - 4, l, theme.index)) add(g_copies.lists, chunk + o - 4);
                    } else if (d == t.length) {
                        std::uint32_t v[4] = {};
                        std::memcpy(v, buf.data() + o + 4, 16);
                        if (v[0] == t.channels && v[1] == t.size && v[2] == t.loop_start && v[3] == t.loop_end) add(g_copies.infos, chunk + o);
                    }
                }
            }
        }
        at = end;
    }
    std::string where;
    const auto hex = [](std::uintptr_t a) {
        char text[12];
        std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned>(a));
        return std::string(text);
    };
    for (const std::uintptr_t a : g_copies.lists) where += " list " + hex(a);
    for (const std::uintptr_t a : g_copies.infos) where += " info " + hex(a);
    RM_LOGI("music: %zu copies of the list and %zu info blocks in %zu MB of the game's memory (%lu ms):%s", g_copies.lists.size(),
            g_copies.infos.size(), scanned >> 20, GetTickCount() - started, where.c_str());
}

// Each copy and info block takes `v`'s numbers (the theme's, or a track's); what no longer is one is
// left alone. False when one is gone (looked for again).
bool WriteCopies(const Found& theme, const rm::music::StreamEntry& v) {
    const List& l = g_lists[static_cast<std::size_t>(theme.list)];
    bool all = true;
    for (const std::uintptr_t first : g_copies.lists) {
        if (ListCopyAt(first, l, theme.index)) {
            WriteRecord(first, theme.index, v);
        } else {
            all = false;
        }
    }
    for (const std::uintptr_t info : g_copies.infos) {
        if (InfoHoldsATrack(info)) {
            WriteInfo(info, v);
        } else {
            all = false;
        }
    }
    return all;
}

void Restore() {
    if (!g_patch.on) return;
    const Found theme{g_patch.list, g_patch.index};
    WriteCopies(theme, theme.entry());  // the theme's own numbers, from the file on disk
    RM_LOGI("music: the theme is its own again (it was %s)", g_patch.track.c_str());
    g_patch = {};
}

bool Apply(const Found& theme, const Found& track) {
    Restore();
    const rm::music::StreamEntry& src = track.entry();
    const std::filesystem::path file = g_game / (src.path + ".sngw");
    if (!FileExists(file)) return false;
    // With the theme's numbers in place: the copies known still there, else looked for again.
    if (!g_copies.searched || g_copies.list != theme.list || !WriteCopies(theme, theme.entry()) || g_copies.lists.empty()) FindGameCopies(theme);
    if (g_copies.lists.empty()) {
        RM_LOGW("music: no copy of the theme's list in memory");
        return false;
    }
    WriteCopies(theme, src);
    Patch p;
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

void ApplyPick(const std::string& pick);  // below, with NextArena

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
    }
    // Between matches only: the armed track once the match is over, the theme's own numbers back once
    // no run is on (two minutes after the last arena was armed).
    if (c.inMatch && !g_inMatch) {
        if (g_pending.on && g_pending.sawEnd) {
            RM_LOGW("music: the match began before %s could be swapped in; the theme as it is", g_pending.pick.c_str());
            g_pending = {};
        }
    }
    if (!c.inMatch && g_inMatch) {
        g_matchOverAt = c.now;
        if (g_pending.on) g_pending.sawEnd = true;
    }
    g_inMatch = c.inMatch;
    if (c.inMatch) return;
    if (g_pending.on && c.now - g_matchOverAt >= 2.0) {
        const std::string pick = g_pending.pick;
        g_pending = {};
        ApplyPick(pick);
    }
    if (g_patch.on && (!c.enabled || (!c.inRun && c.now - g_armedAt > 120.0))) Restore();
}

void NextArena(bool enabled, double now) {
    g_armedAt = now;
    LoadLists();
    ReadTheme();
    if (!enabled || g_theme.empty()) {
        g_pending = {true, g_theme, false};
        return;
    }
    if (!FindEntry(g_theme)) {
        RM_LOGW("music: the theme %s is in no list; the music stays", g_theme.c_str());
        g_pending = {true, g_theme, false};
        return;
    }
    std::vector<std::string> pool = {g_theme};
    for (const List& l : g_lists) {
        for (const rm::music::StreamEntry& e : l.entries) {
            if (rm::music::IsBattleTrack(e) && FileExists(g_game / (e.path + ".sngw"))) pool.push_back(Lower(e.path));
        }
    }
    g_rotation.SetPool(pool);
    g_pending = {true, g_rotation.Next(), false};
    RM_LOGI("music: the next arena plays %s (%zu tracks take turns; swapped once no track plays)",
            g_pending.pick == g_theme ? "the Mercenaries theme" : FindEntry(g_pending.pick) ? FindEntry(g_pending.pick)->entry().Leaf().c_str() : "?",
            pool.size());
    if (!g_inMatch) {
        // From the menus: nothing of the theme plays.
        const std::string pick = g_pending.pick;
        g_pending = {};
        ApplyPick(pick);
    }
}

void ApplyPick(const std::string& pick) {
    const auto theme = FindEntry(g_theme);
    if (!theme || pick == g_theme) {
        Restore();
        if (theme) RM_LOGI("music: this arena plays the Mercenaries theme");
        return;
    }
    const auto track = FindEntry(pick);
    if (!track || !Apply(*theme, *track)) {
        Restore();
        RM_LOGW("music: %s could not take the theme's place; the theme plays", pick.c_str());
        return;
    }
    RM_LOGI("music: this arena plays %s in the theme's place (%.0f s; %zu list copies and %zu info blocks)", track->entry().Leaf().c_str(),
            track->entry().length / 48000.0, g_copies.lists.size(), g_copies.infos.size());
}

std::vector<fileredirect::Redirect> Redirects() {
    if (!g_patch.on) return {};
    return {fileredirect::Redirect{g_patch.tail, g_patch.target}};
}

std::string RedirectKey() { return g_patch.on ? Narrow(g_patch.target) : std::string(); }

}  // namespace rmre5::music
