#pragma once

// Engine layout for Steam build 11465250 (re5dx9.exe 1.0.0.129, PE timestamp
// 0x63E9CE00, image size 0x1929000, fixed base 0x400000). Derived from the
// unpacked in-memory image; see research/re5-engine-map.md for how each value
// was found. Every function address is guarded by an expected-bytes check in
// kSignatures before anything touches it, so a different build disables the
// matching capability instead of patching unknown code.

#include <cstddef>
#include <cstdint>

namespace re5 {

constexpr std::uint32_t kExpectedTimeDateStamp = 0x63E9CE00;
constexpr std::uint32_t kExpectedSizeOfImage = 0x01929000;
constexpr std::uintptr_t kImageBase = 0x00400000;

namespace addr {
// Singletons: MT Framework keeps `static T* mpInstance` directly before the
// class DTI object.
constexpr std::uintptr_t kSPlayerInstance = 0x011B27DC;  // sPlayer*
constexpr std::uintptr_t kSEnemyInstance = 0x011B1FCC;   // sEnemy*
constexpr std::uintptr_t kGameRoot = 0x012340A4;         // app object; +0x1042C -> game work

// DTI (runtime type info) objects.
constexpr std::uintptr_t kDtiUCharacter = 0x011EA088;
constexpr std::uintptr_t kDtiUPlayer = 0x012031D8;
constexpr std::uintptr_t kDtiUEnemy = 0x011F4F4C;

// Functions (all __thiscall).
constexpr std::uintptr_t kFnEnemyOnHit = 0x009E0840;      // void uEnemy::OnHit(cHit*)
constexpr std::uintptr_t kFnCharacterSubHp = 0x0084F7C0;  // void uCharacter::SubHp(short)
constexpr std::uintptr_t kFnPlayerSubHp = 0x00B7C950;     // void uPlayer::SubHp(short)
constexpr std::uintptr_t kFnHitGetAttacker = 0x00C49530;  // cUnit* cHit::GetAttacker()
constexpr std::uintptr_t kFnMercAddTime = 0x00718050;     // void ?::MercAddTime(int seconds)
// void cMercs::UpdateCountdown(float dt): counts [this+0x4DC] down, sets the
// time-up state [this+0x4D4] = 1 at zero (or at the 150/400 kill cap) and
// mirrors the value into game work +0x680. Called once per frame in a match.
constexpr std::uintptr_t kFnMercUpdateCountdown = 0x00717850;
// Play menu screen (uIdStartUp01) per-frame move, and the menu input query
// (__stdcall int(int pad, int mode)) it calls first.
constexpr std::uintptr_t kFnPlayMenuMove = 0x00A396C0;
constexpr std::uintptr_t kFnMenuInput = 0x009E3420;
// Play menu row/help draw, called from inside its move(); reads the enabled mask.
constexpr std::uintptr_t kFnPlayMenuDraw = 0x00A373B0;
// void DrawMenuText(int message, int x, int y, const uint32_t* argb, float scale)
// (ret 0x14, ignores ecx): draws one menu row label.
constexpr std::uintptr_t kFnMenuDrawLabel = 0x00A17660;
// uIdMenu helpers shared by the uId* screens (all __thiscall on the screen):
//   int Navigate(int cursor, int count): applies up/down (with repeat and
//     wrap) from the pad and returns the new cursor;
//   void RowColor(uint32_t* argb, bool selected, uint8_t alpha, int fade):
//     the list's gray/white row colour with the screen's fade-in alpha;
//   SoundDecide/SoundCancel/SoundDeny: the menu's three UI sounds.
constexpr std::uintptr_t kFnMenuNavigate = 0x009E4BD0;
constexpr std::uintptr_t kFnMenuRowColor = 0x00A17350;
constexpr std::uintptr_t kFnMenuSoundDecide = 0x009E4A70;
constexpr std::uintptr_t kFnMenuSoundCancel = 0x009E4A50;
constexpr std::uintptr_t kFnMenuSoundDeny = 0x009E4AC0;
// void uIdTopMenu00::SetBallIndex(int item): the planet's highlighted node.
constexpr std::uintptr_t kFnMenuSetBallIndex = 0x00A58170;
// Mercenaries setup screens (move() = vtable slot 6), in play order:
//   uIdStartUp20: "Iniciar/Marcadores" (state 2, cursor +0x48 -> gw+0x5C8),
//     then the Solo/Duo page (state 10, cursor +0x18C -> gw+0x5CC);
//   uIdStartUp21: stage (state 2, cursor +0x48 -> gw+0x5D4);
//   uIdStartUp22: character/costume (state 2, +0x194 -> gw+0x600+4*slot),
//     states 80..82 while the match loads.
constexpr std::uintptr_t kFnStartUp20Move = 0x00A47CE0;
constexpr std::uintptr_t kFnStartUp21Move = 0x00A49C40;
constexpr std::uintptr_t kFnStartUp22Move = 0x00A48260;
// Screen fader singleton (uIdStartUp22 fades to black with vtable slot 12
// `(float alpha, int frames)` after the character decide and polls slot 14).
constexpr std::uintptr_t kFadeInstance = 0x011E8804;
// Title-flow script (found with Ghidra, research/ghidra.md). The Mercenaries
// setup script FUN_00BF78B0 runs: state 10 = Step(ctx, 0) (stage screen),
// state 0xB = Step(ctx, 1) (character screen), state 0x14 = start the match.
// Step(ctx, kind) and Step20(ctx) (the Iniciar/Solo screen) create a screen
// when [ctx+0x25] == 0 and return its result (+1 accepted, -1 back, -2) once
// it closes; returning +1 at once skips the screen.
constexpr std::uintptr_t kFnFlowStep = 0x00BF2CF0;
constexpr std::uintptr_t kFnFlowStep20 = 0x00BF2BD0;
constexpr std::uintptr_t kFnFlowMercSetup = 0x00BF78B0;
// Inventory (found with Ghidra from uPlayer::PickUpItem, FUN_007E9DF0): the
// inventory object is uPlayer+0x21A0; __thiscall helpers on it:
//   int AddToStacks(int item, int count): tops up existing stacks of `item`
//     up to each slot's max, returns what did not fit;
//   int AddAmmo(int item, int count).
// Item ids are (category << 8) | index: 1 weapons, 2 ammo, 4 keys/treasure.
constexpr std::uintptr_t kFnInventoryAddToStacks = 0x00C4BEE0;
constexpr std::uintptr_t kFnInventoryAddAmmo = 0x00C4DA90;
// void uIdMenu::DrawHelp(int message, int y): help line under a menu, shared by
// most uId* screens. The Play menu draws rows at y = (item + 3) * 50 and its
// help at y = 570, which a ninth row (y = 550) would sit under.
constexpr std::uintptr_t kFnMenuDrawHelp = 0x00A17CF0;
constexpr int kPlayMenuHelpY = 570;
constexpr int kPlayMenuFirstRowY = 150;
constexpr int kPlayMenuRowPitch = 50;
constexpr std::uintptr_t kPadInstance = 0x01249C40;
constexpr std::uintptr_t kSMessageInstance = 0x011B26F0;
// Message ids: Play menu item 8 shows label 936 ("NUEVA PARTIDA") through the
// item->label table at 0x01131D90. Row 0 reads the same table cell when it
// turns into "NUEVA PARTIDA", so 936 itself must not be redirected.
constexpr int kMessageItem8Label = 936;
}  // namespace addr

namespace off {
// uCharacter
constexpr std::size_t kHp = 0x1364;     // int16
constexpr std::size_t kMaxHp = 0x1366;  // int16
// uPlayer
constexpr std::size_t kPlayerNumber = 0x2D9C;   // uint8, copied into enemy+0x136C on hit
constexpr std::size_t kPlayerControl = 0x2DA8;  // int32; 0/1/2 observed in code paths (hypothesis: 2 = AI)
// uPlayer inventory: 24 slots of 0x30 bytes starting at inventory+4.
constexpr std::size_t kPlayerInventory = 0x21A0;
constexpr int kInventorySlots = 0x18;
constexpr std::size_t kInventoryFirstSlot = 0x4;
constexpr std::size_t kInventorySlotSize = 0x30;
constexpr std::size_t kSlotItemId = 0x4;   // int32
constexpr std::size_t kSlotCount = 0x8;    // int32
constexpr std::size_t kSlotMax = 0xC;      // int32 stack capacity
constexpr std::size_t kSlotValid = 0x29;   // uint8 == 1 for a used slot
// uEnemy
constexpr std::size_t kEnemyLastHitPlayer = 0x136C;  // int32 player number or -1
// sPlayer
constexpr std::size_t kSPlayerList = 0x24;   // uPlayer*[4]
constexpr std::size_t kSPlayerCount = 0x34;  // uint32
// sEnemy
constexpr std::size_t kSEnemyCount = 0x24;  // uint32
constexpr std::size_t kSEnemyArray = 0x30;  // uEnemy**
// cHit
constexpr std::size_t kHitValid = 0x48;  // uint8
// app object -> game work
constexpr std::size_t kGameWork = 0x1042C;
// game work
constexpr std::size_t kGameMode = 0x58;       // int32; Mercenaries code checks 2 and 5
constexpr std::size_t kGameStage = 0x574;     // int32 (hypothesis: stage/area id)
constexpr std::size_t kMercTimer = 0x680;     // float seconds remaining (display mirror, rewritten each frame)
constexpr std::size_t kMercKills = 0x58C;     // int32 Mercenaries kill counter (150/400 cap ends the match)
constexpr std::size_t kMercScore = 0x6B4;     // int32 (from the 2021 trainer's pointer path)
// Mercenaries match object (`this` of kFnMercUpdateCountdown)
constexpr std::size_t kMercCountdown = 0x4DC;  // float seconds remaining (authoritative)
constexpr std::size_t kMercState = 0x4D4;      // int32: 0 running, 1 time up / finished
// Play menu (uIdStartUp01: CONTINUAR ... CONTENIDO EXTRA)
constexpr std::size_t kMenuState = 0x14;       // uint8 state machine; 2 = waiting for input
constexpr std::size_t kMenuCursor = 0x48;      // int32 selected item
constexpr std::size_t kMenuItemCount = 0x190;  // int32: 7, or 8 with "Contenido extra"
constexpr std::size_t kMenuEnabledMask = 0x194; // uint32 bit per item; clear bits draw red
constexpr std::size_t kMenuBlink = 0x3D;        // uint8: selected row pulses while set
constexpr std::size_t kMenuTopMenu = 0x128;     // uIdTopMenu00* (planet and side labels)
constexpr std::size_t kFadeVtableOut = 0x30;   // fader vtable slot 12: FadeOut(float alpha, int frames)
constexpr std::size_t kMenuModeCursor = 0x18C;  // uIdStartUp20 Solo/Duo page: int32 cursor
constexpr std::size_t kMenuCharacterIndex = 0x194;  // uIdStartUp22: int32 selected costume slot
// game work: Mercenaries setup written by those screens
constexpr std::size_t kGameWorkMercItem = 0x5C8;   // int32: 0 Iniciar, 1 Marcadores
constexpr std::size_t kGameWorkMercMode = 0x5CC;   // int32: 0 Solo, 1 Duo split, 2 Duo online
constexpr std::size_t kGameWorkStage = 0x5D4;      // int32 stage index (0..7)
constexpr std::size_t kGameWorkCharacter = 0x600;  // int32[4] costume slot per player
constexpr std::size_t kGameWorkMercFlag = 0x79E;   // uint8 set by the Solo/Duo page's 4th option
constexpr std::size_t kFlowStepState = 0x25;       // script ctx: 0 create, 1 wait, 2 done
// Pad singleton: decide/cancel button masks for the menu input bits.
constexpr std::size_t kPadDecideMask = 0x5B4;
constexpr std::size_t kPadCancelMask = 0x5B8;
// sMessage -> rMessage for etc\message\mes_system_<lang>
constexpr std::size_t kMessageSystemSlot = 0x11534;
constexpr std::size_t kRMessagePath = 0x04;    // char[] resource path
constexpr std::size_t kRMessageTypeCrc = 0x5C; // 0x10C460E6 for rMessage
constexpr std::size_t kRMessageTable = 0x60;   // const uint16_t** per-message pointers
constexpr std::size_t kRMessageData = 0x64;    // raw MSG2 file: "MSG2" magic, u16 count at +0x1C
constexpr std::size_t kMsg2Count = 0x1C;
// MtObject
constexpr std::size_t kVtableGetDti = 0x10;  // vtable slot 4
// MtDTI
constexpr std::size_t kDtiName = 0x04;
constexpr std::size_t kDtiParent = 0x10;
}  // namespace off

struct Signature {
    const char* name;
    std::uintptr_t va;
    const char* bytes;  // hex, space separated
};

// Expected bytes at every site we hook, call or rely on for a layout fact.
inline constexpr Signature kSignatures[] = {
    {"enemy_on_hit", addr::kFnEnemyOnHit, "56 57 8B 7C 24 0C 85 FF 8B F1"},
    {"character_sub_hp", addr::kFnCharacterSubHp, "66 8B 44 24 04 66 29 81 64 13 00 00"},
    {"player_sub_hp", addr::kFnPlayerSubHp, "56 8B F1 66 8B 4C 24 08 57 0F BF BE 64 13 00 00"},
    {"hit_get_attacker", addr::kFnHitGetAttacker, "56 57 8B F9 8B 77 20"},
    // MercAddTime reads [kGameRoot]+kGameWork and adds to +kMercTimer.
    {"merc_add_time", addr::kFnMercAddTime, "A1 A4 40 23 01 56 8B 74 24 08 57 8B F9 8B 88 2C 04 01 00"},
    {"merc_timer_store", 0x007180E6, "F3 0F 11 89 80 06 00 00"},
    {"merc_update_countdown", addr::kFnMercUpdateCountdown,
     "A1 A4 40 23 01 8B 80 2C 04 01 00 57 8B F9 8B 88 74 05 00 00 3B 88 78 05 00 00"},
    // sPlayer::GetPlayer(i) returns [this + i*4 + 0x24] for i in [0,4).
    {"splayer_get_player", 0x007E6220, "8B 44 24 04 83 F8 FF 7F 05 33 C0 C2 04 00 83 F8 04 7D F6 8B 44 81 24"},
    // uEnemy::ApplyDamage calls the SubHp virtual (+0x114) that the base hook covers.
    {"enemy_apply_damage_subhp", 0x009DEB02, "8B 06 8B 90 14 01 00 00 57 8B CE"},
    // Play menu: move entry, decide-mask test, cursor/count use, label table.
    {"playmenu_move", addr::kFnPlayMenuMove, "53 56 57 6A FF 6A FF 8B F1 E8 52 9D FA FF 8B F8"},
    {"playmenu_decide_mask", 0x00A39745, "8B 0D 40 9C 24 01 85 B9 B4 05 00 00"},
    {"playmenu_cursor_count", 0x00A397A2, "8B 86 90 01 00 00 8B 4E 48 50 51 8B"},
    {"playmenu_label_table", 0x01131D90, "A9 03 AA 03 AB 03 AD 03 AC 03 B0 03 AF 03 AE 03 A8 03"},
    {"menu_input", addr::kFnMenuInput, "A1 A4 40 23 01 80 B8 28 04 01 00 00 74 05 33 C0 C2 08 00"},
    {"smessage_get_dti", 0x007E06E0, "B8 F4 26 1B 01 C3"},
    {"playmenu_draw", addr::kFnPlayMenuDraw, "83 EC 0C 56 57 8B F1 E8 C4 C3 FA FF 33 FF 39 BE 90 01 00 00"},
    // Row y = (item + 3) * 50, then `push 640; push label; mov ecx, esi; call DrawMenuText`.
    {"playmenu_row_y", 0x00A373D2, "8D 5F 03 6B DB 32"},
    {"playmenu_label_call", 0x00A374D4, "68 80 02 00 00 55 8B CE E8 7F 01 FE FF"},
    {"menu_draw_label", addr::kFnMenuDrawLabel, "8B 44 24 04 8B 54 24 10 F3 0F 10 44 24 14 F3 0F 59 05 14 2E"},
    {"menu_navigate", addr::kFnMenuNavigate, "A1 A4 40 23 01 80 B8 28 04 01 00 00 53 56 57 8B D9"},
    {"menu_row_color", addr::kFnMenuRowColor, "0F B6 54 24 0C 8B 44 24 10 0F AF 54 81 78"},
    {"menu_sound_decide", addr::kFnMenuSoundDecide, "51 8B 0D 0C 1D 1B 01 6A 45 E8"},
    {"menu_sound_cancel", addr::kFnMenuSoundCancel, "51 8B 0D 0C 1D 1B 01 6A 4E E8"},
    {"menu_sound_deny", addr::kFnMenuSoundDeny, "51 8B 0D 0C 1D 1B 01 6A 47 E8"},
    {"menu_ball_index", addr::kFnMenuSetBallIndex, "8B 41 5C 85 C0 74 0A 8B 4C 24 04 89 88 58 0D 00 00"},
    // Play menu move: `test [edx+0x5B8], edi` is the cancel check after decide.
    {"playmenu_cancel_mask", 0x00A39774, "85 BA B8 05 00 00 75 6A"},
    {"startup20_move", addr::kFnStartUp20Move, "53 56 57 6A FF 6A FF 8B F1 E8 32 B7 F9 FF 33 DB"},
    {"startup21_move", addr::kFnStartUp21Move, "53 55 56 57 6A FF 6A FF 8B F1 E8 D1 97 F9 FF 6A"},
    {"startup22_move", addr::kFnStartUp22Move, "53 55 56 8B F1 8B 86 88 01 00 00 8B 8E 8C 01 00"},
    // uIdStartUp21 decide: `mov edx,[esi+0x48]; mov [ecx+0x5D4], edx` (stage).
    {"startup21_stage_write", 0x00A49F01, "8B 56 48 89 91 D4 05 00 00"},
    // uIdStartUp22 decide: `mov [eax+ecx*4+0x600], edx` (costume slot).
    {"startup22_character_write", 0x00A48573, "89 94 88 00 06 00 00"},
    // uIdStartUp22: `fld1; mov ecx,[fader]; ...; push 0x20; push ecx; fstp; call [vt+0x30]`.
    {"startup22_fade_out", 0x00A4867D, "D9 E8 8B 0D 04 88 1E 01 8B 11 8B 42 30 6A 20 51 D9 1C 24 FF D0"},
    {"flow_step", addr::kFnFlowStep, "56 8B F1 0F BE 46 25 83 E8 00 74 6A 83 E8 01 74"},
    {"flow_step20", addr::kFnFlowStep20, "56 8B F1 0F BE 46 25 83 E8 00 74 57 83 E8 01 74"},
    {"flow_merc_setup", addr::kFnFlowMercSetup, "56 8B F1 0F BE 46 24 83 F8 15 0F 87 AE 01 00 00"},
    {"inventory_add_to_stacks", addr::kFnInventoryAddToStacks, "83 EC 64 53 8B 5C 24 6C 55 56 57 33 FF 33 C0 89"},
    {"inventory_add_ammo", addr::kFnInventoryAddAmmo, "53 8B 5C 24 0C 55 8B 6C 24 0C 56 57 8B F9 33 F6"},
    // uPlayer::PickUpItem: `lea ecx,[esi+0x21A0]; call AddToStacks`.
    {"player_pickup_inventory", 0x007EA134, "8D 8E A0 21 00 00 E8 A1 1D 46 00"},
    {"menu_draw_help", addr::kFnMenuDrawHelp, "51 8B 51 78 69 D2 FF 00 00 00 B8 39 8E E3 38 F7 EA"},
    // Play menu draw: `push 570; push eax; mov ecx, esi; call DrawHelp`.
    {"playmenu_help_call", 0x00A37566, "68 3A 02 00 00 50 8B CE E8 7D 07 FE FF"},
    // getDTI stubs prove the DTI addresses above.
    {"splayer_get_dti", 0x007EA880, "B8 E0 27 1B 01 C3"},
    {"uplayer_get_dti", 0x00B7F080, "B8 D8 31 20 01 C3"},
    {"uenemy_get_dti", 0x009E0B70, "B8 4C 4F 1F 01 C3"},
    {"ucharacter_get_dti", 0x0084F720, "B8 88 A0 1E 01 C3"},
};

}  // namespace re5
