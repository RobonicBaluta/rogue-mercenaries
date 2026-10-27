// Native "ROGUE MERCENARIES" entry in RE5's Play menu (uIdStartUp01).
//
// The Play menu already supports a ninth row: its layout and its label table
// map item 8 to message 936 ("NUEVA PARTIDA"), normally unused here. While the
// menu is open this module
//   * raises the item count from 8 to 9, so the game draws, highlights and
//     navigates the new row itself;
//   * draws that row from a borrowed message holding "ROGUE MERCENARIES" in
//     the game's own glyph encoding (widths copied from the loaded messages),
//     and adds a help line for it. Message 936 itself is left alone: row 0
//     reads the same table cell when it turns into "NUEVA PARTIDA";
//   * marks the row enabled (otherwise the game tints it red) and moves the
//     help line one row down so it does not cover the new row;
//   * turns "decide" on row 8 into the game's own "Los Mercenarios" decide
//     (row 6) and records that the player chose Rogue Mercenaries.
// The borrowed messages are PS3-only texts a PC build never shows; their
// pointers are restored as soon as the menu closes.

#include "native_menu.h"

#include <windows.h>

#include <MinHook.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"
#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5::nativemenu {
namespace {

using MoveFn = void(__thiscall*)(std::uintptr_t self);
using MenuInputFn = std::uint32_t(__stdcall*)(int pad, int mode);
using DrawHelpFn = void(__thiscall*)(std::uintptr_t self, int message, int y);
using DrawLabelFn = void(__thiscall*)(void* ecx, int message, int x, int y, const std::uint32_t* argb, float scale);

constexpr int kRogueItem = 8;
constexpr int kMercenariesItem = 6;
constexpr std::uint8_t kStateInput = 2;
constexpr std::uint32_t kRMessageCrc = 0x10C460E6;
constexpr std::uint32_t kMsg2Magic = 0x3247534D;  // "MSG2"
// Borrowed message ids (mes_system): PS3-only texts.
constexpr int kLabelMessage = 653;  // "No se ha conectado el PlayStation Eye."
constexpr int kHelpMessage = 656;   // PlayStation Move calibration hint
constexpr char kLabel[] = "ROGUE MERCENARIES";
constexpr char kHelp[] = "Chain Mercenaries stages into one run\nwith perks and rewards in between.";
constexpr int kRogueRowY = re5::addr::kPlayMenuFirstRowY + kRogueItem * re5::addr::kPlayMenuRowPitch;
constexpr int kShiftedHelpY = re5::addr::kPlayMenuHelpY + re5::addr::kPlayMenuRowPitch;

MoveFn g_origMove = nullptr;
MoveFn g_origDraw = nullptr;
DrawHelpFn g_origDrawHelp = nullptr;
DrawLabelFn g_origDrawLabel = nullptr;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_chosen{false};
std::atomic<DWORD> g_lastMenuFrame{0};
std::atomic<bool> g_atInput{false};  // menu state was "input" after the last move()
// Play menu inside its draw on this thread. The engine runs menu move() on
// job threads, and the label/help helpers are shared by every menu.
thread_local std::uintptr_t t_drawingMenu = 0;

// A message pointer pointed at mod text while the menu is open.
struct Borrowed {
    int id = 0;
    std::uintptr_t slot = 0;      // address of the message pointer
    std::uintptr_t original = 0;  // its original value
    std::vector<std::uint16_t> text;
};
// Texts are lent from menu job threads and returned from the game tick.
std::mutex g_textMutex;
Borrowed g_label{kLabelMessage};
Borrowed g_help{kHelpMessage};
bool g_textFailed = false;
std::atomic<bool> g_applied{false};
std::atomic<bool> g_helpLent{false};

struct Messages {
    std::uintptr_t table = 0;
    int count = 0;
};

Messages FindMessages() {
    const auto sMessage = ReadOr<std::uintptr_t>(re5::addr::kSMessageInstance, 0);
    const auto rMessage = sMessage ? ReadOr<std::uintptr_t>(sMessage + re5::off::kMessageSystemSlot, 0) : 0;
    if (!rMessage || ReadOr<std::uint32_t>(rMessage + re5::off::kRMessageTypeCrc, 0) != kRMessageCrc) return {};
    const std::string path = ReadCString(rMessage + re5::off::kRMessagePath, 64);
    if (path.rfind("etc\\message\\mes_system", 0) != 0) return {};
    const auto data = ReadOr<std::uintptr_t>(rMessage + re5::off::kRMessageData, 0);
    if (!data || ReadOr<std::uint32_t>(data, 0) != kMsg2Magic) return {};
    return {ReadOr<std::uintptr_t>(rMessage + re5::off::kRMessageTable, 0),
            ReadOr<std::uint16_t>(data + re5::off::kMsg2Count, 0)};
}

// Glyph code for a character in the game's message encoding (Latin fonts:
// A=0x38, a=0x6C, two codes apart).
std::uint16_t GlyphCode(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<std::uint16_t>(0x38 + 2 * (c - 'A'));
    if (c >= 'a' && c <= 'z') return static_cast<std::uint16_t>(0x6C + 2 * (c - 'a'));
    switch (c) {
        case ' ': return 0x1C;
        case ',': return 0x24;
        case '.': return 0x26;
        default: return 0;
    }
}

// Each glyph is (code, width); a line break is (0, 0x0403) and the text ends
// with (0, 0x0401).
bool Encode(const char* s, const std::array<std::uint16_t, 0x200>& width, std::vector<std::uint16_t>& out) {
    out.clear();
    for (const char* p = s; *p; ++p) {
        if (*p == '\n') {
            out.push_back(0x0000);
            out.push_back(0x0403);
            continue;
        }
        const std::uint16_t code = GlyphCode(*p);
        if (!code || !width[code]) {
            RM_LOGW("native menu: no glyph for '%c'", *p);
            return false;
        }
        out.push_back(code);
        out.push_back(width[code]);
    }
    out.push_back(0x0000);
    out.push_back(0x0401);
    return true;
}

bool BuildTexts(const Messages& m) {
    // The game's own Mercenaries label must be Latin capitals in this encoding,
    // otherwise (Japanese, Chinese, Russian fonts) the texts cannot be built.
    const auto mercs = ReadOr<std::uintptr_t>(m.table + 943 * 4, 0);
    std::uint16_t first = 0;
    if (!mercs || !SafeRead(mercs, first) || first < 0x38 || first > 0x6A) {
        RM_LOGW("native menu: message font is not Latin; Rogue Mercenaries entry disabled");
        return false;
    }
    std::array<std::uint16_t, 0x200> width{};
    for (int id = 0; id < m.count; ++id) {
        const auto text = ReadOr<std::uintptr_t>(m.table + id * 4, 0);
        if (!text) continue;
        for (int i = 0; i < 512; ++i) {
            std::uint16_t glyph[2] = {};
            if (!SafeCopy(glyph, text + i * 4, sizeof(glyph))) break;
            if (glyph[0] == 0 && glyph[1] == 0x0401) break;  // end of message
            // Control codes carry parameters >= 0x100 in the second field.
            if (glyph[0] < width.size() && glyph[1] > 0 && glyph[1] < 0x100) width[glyph[0]] = glyph[1];
        }
    }
    if (!Encode(kLabel, width, g_label.text)) return false;
    if (!Encode(kHelp, width, g_help.text)) g_help.text.clear();  // the row works without help
    return true;
}

void Lend(Borrowed& b, std::uintptr_t table) {
    if (b.text.empty()) return;
    b.slot = table + b.id * 4;
    b.original = ReadOr<std::uintptr_t>(b.slot, 0);
    SafeStore(b.slot, reinterpret_cast<std::uintptr_t>(b.text.data()));
}

void Return(Borrowed& b) {
    if (!b.slot) return;
    // Only put the original back if the slot still holds our pointer (the
    // message file may have been reloaded meanwhile).
    if (ReadOr<std::uintptr_t>(b.slot, 0) == reinterpret_cast<std::uintptr_t>(b.text.data())) {
        SafeStore(b.slot, b.original);
    }
    b.slot = 0;
}

bool ApplyTexts() {
    if (g_applied.load()) return true;
    std::lock_guard lock(g_textMutex);
    if (g_applied.load()) return true;
    if (g_textFailed) return false;
    const Messages m = FindMessages();
    if (!m.table || m.count <= kHelpMessage) return false;  // messages not loaded yet; retry next frame
    if (g_label.text.empty() && !BuildTexts(m)) {
        g_textFailed = true;
        return false;
    }
    Lend(g_label, m.table);
    Lend(g_help, m.table);
    g_helpLent = g_help.slot != 0;
    g_applied = true;
    RM_LOGI("native menu: texts set (messages %d, %d of %d)", kLabelMessage, kHelpMessage, m.count);
    return true;
}

void RestoreTexts() {
    std::lock_guard lock(g_textMutex);
    if (!g_applied.load()) return;
    g_applied = false;
    g_helpLent = false;
    Return(g_label);
    Return(g_help);
    RM_LOGI("native menu: texts restored");
}

bool DecidePressed() {
    const auto pad = ReadOr<std::uintptr_t>(re5::addr::kPadInstance, 0);
    const auto mask = pad ? ReadOr<std::uint32_t>(pad + re5::off::kPadDecideMask, 0) : 0;
    if (!mask) return false;
    __try {
        return (reinterpret_cast<MenuInputFn>(re5::addr::kFnMenuInput)(-1, -1) & mask) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Extended(std::uintptr_t menu) {
    return g_applied && ReadOr<std::int32_t>(menu + re5::off::kMenuItemCount, 0) == 9;
}

void __fastcall PlayMenuMoveDetour(std::uintptr_t self, void*) {
    if (g_enabled.load()) {
        g_lastMenuFrame = GetTickCount();
        std::int32_t count = 0;
        // Only extend the full Gold Edition menu (8 rows; 9 once extended).
        // Trial/benchmark variants use other counts and keep the vanilla menu.
        if (SafeRead(self + re5::off::kMenuItemCount, count) && (count == 8 || count == 9)) {
            SafeStore<std::int32_t>(self + re5::off::kMenuItemCount, ApplyTexts() ? 9 : 8);
        }
        const auto cursor = ReadOr<std::int32_t>(self + re5::off::kMenuCursor, -1);
        const auto state = ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0);
        // move() handles decide before navigation, so the cursor here is the
        // row drawn highlighted last frame.
        if (state == kStateInput && DecidePressed()) {
            RM_LOGI("native menu: decide on row %d of %d", cursor,
                    ReadOr<std::int32_t>(self + re5::off::kMenuItemCount, 0));
            if (cursor == kRogueItem && Extended(self)) {
                // Let the game run its own "Los Mercenarios" selection.
                SafeStore<std::int32_t>(self + re5::off::kMenuCursor, kMercenariesItem);
                g_chosen = true;
                RM_LOGI("native menu: ROGUE MERCENARIES chosen");
            }
        }
    }
    g_origMove(self);
    // A decide moves the state off "input" within this same call.
    g_atInput = ReadOr<std::uint8_t>(self + re5::off::kMenuState, 0) == kStateInput;
}

void __fastcall PlayMenuDrawDetour(std::uintptr_t self, void*) {
    const bool extended = g_enabled.load() && Extended(self);
    if (extended) {
        // move() refreshes the enabled mask from game work every frame and
        // then calls this draw.
        const auto mask = ReadOr<std::uint32_t>(self + re5::off::kMenuEnabledMask, 0);
        SafeStore<std::uint32_t>(self + re5::off::kMenuEnabledMask, mask | (1u << kRogueItem));
        t_drawingMenu = self;
    }
    g_origDraw(self);
    t_drawingMenu = 0;
    // The game draws no help line for row 8.
    if (extended && g_helpLent.load() && ReadOr<std::int32_t>(self + re5::off::kMenuCursor, -1) == kRogueItem) {
        g_origDrawHelp(self, kHelpMessage, kShiftedHelpY);
    }
}

void __fastcall DrawLabelDetour(void* ecx, void*, int message, int x, int y, const std::uint32_t* argb,
                                float scale) {
    if (t_drawingMenu && message == re5::addr::kMessageItem8Label && y == kRogueRowY) {
        message = kLabelMessage;
    }
    g_origDrawLabel(ecx, message, x, y, argb, scale);
}

void __fastcall DrawHelpDetour(std::uintptr_t self, void*, int message, int y) {
    if (self && self == t_drawingMenu && y == re5::addr::kPlayMenuHelpY) {
        y = kShiftedHelpY;
    }
    g_origDrawHelp(self, message, y);
}

bool QueueHook(std::uintptr_t address, void* detour, void** original) {
    auto* target = reinterpret_cast<void*>(address);
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK) status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        RM_LOGE("native menu: hook at 0x%08X failed: %s", static_cast<unsigned>(address), MH_StatusToString(status));
        return false;
    }
    return true;
}

}  // namespace

bool Install() {
    if (!engine::Caps().playMenu) {
        RM_LOGW("native menu: signatures missing; Play menu entry disabled");
        return false;
    }
    // The move hook goes last: without it the other detours never activate.
    if (!QueueHook(re5::addr::kFnMenuDrawHelp, reinterpret_cast<void*>(&DrawHelpDetour),
                   reinterpret_cast<void**>(&g_origDrawHelp)) ||
        !QueueHook(re5::addr::kFnMenuDrawLabel, reinterpret_cast<void*>(&DrawLabelDetour),
                   reinterpret_cast<void**>(&g_origDrawLabel)) ||
        !QueueHook(re5::addr::kFnPlayMenuDraw, reinterpret_cast<void*>(&PlayMenuDrawDetour),
                   reinterpret_cast<void**>(&g_origDraw)) ||
        !QueueHook(re5::addr::kFnPlayMenuMove, reinterpret_cast<void*>(&PlayMenuMoveDetour),
                   reinterpret_cast<void**>(&g_origMove))) {
        return false;
    }
    g_enabled = true;
    RM_LOGI("native menu: hook uIdStartUp01::move at 0x%08X queued", static_cast<unsigned>(re5::addr::kFnPlayMenuMove));
    return true;
}

void OnGameTick() {
    // The menu object stops calling move() when it closes; restore the texts.
    if (g_applied && GetTickCount() - g_lastMenuFrame.load() > 500) {
        RestoreTexts();
    }
}

bool MenuAtInput() { return g_atInput.load() && GetTickCount() - g_lastMenuFrame.load() < 500; }
bool TakeChosen() { return g_chosen.exchange(false); }
bool Chosen() { return g_chosen.load(); }

}  // namespace rmre5::nativemenu
