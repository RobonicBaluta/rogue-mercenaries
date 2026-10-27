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
}  // namespace addr

namespace off {
// uCharacter
constexpr std::size_t kHp = 0x1364;     // int16
constexpr std::size_t kMaxHp = 0x1366;  // int16
// uPlayer
constexpr std::size_t kPlayerNumber = 0x2D9C;   // uint8, copied into enemy+0x136C on hit
constexpr std::size_t kPlayerControl = 0x2DA8;  // int32; 0/1/2 observed in code paths (hypothesis: 2 = AI)
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
    // getDTI stubs prove the DTI addresses above.
    {"splayer_get_dti", 0x007EA880, "B8 E0 27 1B 01 C3"},
    {"uplayer_get_dti", 0x00B7F080, "B8 D8 31 20 01 C3"},
    {"uenemy_get_dti", 0x009E0B70, "B8 4C 4F 1F 01 C3"},
    {"ucharacter_get_dti", 0x0084F720, "B8 88 A0 1E 01 C3"},
};

}  // namespace re5
