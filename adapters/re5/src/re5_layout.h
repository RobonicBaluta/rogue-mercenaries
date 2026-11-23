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
// void uPlayer::AddHp(short): adds, clamps to the maximum, flags +0x2D8C |= 2 (the vtable slot
// before SubHp; uCharacter's own SetHp/AddHp/SubHp are 0x0084F740/F780/F7C0). Healing items.
constexpr std::uintptr_t kFnPlayerAddHp = 0x00B7C8D0;
// int X::PickUpItem(uPlayer* player, int itemHandle), thiscall, ret 8: looks the ground item up
// with kFnItemFromHandle (this = [kItemManager]) and gives its id (word +0x76) and count
// (dword +0x78) to the player (category 1 Create, 2 AddAmmo, others AddToStacks).
constexpr std::uintptr_t kFnPickUpItem = 0x007E9DF0;
constexpr std::uintptr_t kFnItemFromHandle = 0x007BEA20;
// Collision: int __stdcall SegmentHit(const float seg[8] {start xyz, pad, end xyz, pad},
// float hit[3], float out2[4], void* out3 (zeroed), 1, mask, 1). The query reads the collision
// singleton [0x01249C64] itself. The player's dodge picker (0x00B6CEF0) casts its dodge lines with
// it (through the yaw wrapper 0x0076E6A0) and treats `result & 4` as blocked; the ground snap
// (0x00B590F0) casts down from 1 m above a unit and takes hit[1] as the floor when the result is
// non-zero. Both load ECX = [kCollision] first: the inner query (0x004B95A0) keeps ECX as its
// world, so the mod calls it as a thiscall on that object (a call without it faulted in game,
// 2026-11-03).
constexpr std::uintptr_t kFnSegmentHit = 0x0076E520;
constexpr std::uintptr_t kCollision = 0x01249C64;  // the collision singleton the query runs on (ECX)
// Character actions (uCharacter, players and enemies alike): bool ChangeAction(int id, int,
// void* requester, int priority, int) is vtable slot 0xDC; the base 0x00B59030 refuses an id past
// the list or while a stronger action holds (+0x10E8 > priority), then requests it (+0x10E0).
// uPlayer (0x00B5C900) and uEnemy (0x009D9DC0) wrap the base. The game starts its dodges with
// (id, 0, 0, -1, 0) and the co-op sync replays any action by id the same way.
constexpr std::uintptr_t kFnChangeAction = 0x00B59030;
constexpr std::uintptr_t kFnPlayerChangeAction = 0x00B5C900;
// The player's hold, cPlAction_pl_act_test (off::kPlActHold): its vtable and the functions in its
// slots 6 (start, a thunk to 0x00C8B1E0), 7 (update, empty) and 10 (the default that sets the done
// bit). See off::kPlActHold.
constexpr std::uintptr_t kVtablePlActTest = 0x01045CF8;
// Area cameras (research/re5-engine-map.md, "Area cameras"): a campaign area's camera zones, the
// rAHCamera resource (DTI 0x011E894C, "stage\sNNN\soft\sNNN") the player camera (uCameraCtrl)
// holds in kAreaCameraResource. Every frame its update (0x00841CE0) asks kFnAreaCameraCheck
// (thiscall, the controller) which zone the player stands in: none, it sets the zone camera -1
// through kFnAreaCameraSet(index, 1) and returns 2. kFnAreaCameraForce(index) (thiscall, RET 4,
// AL) is a script's or an object's "this zone's camera now" (0x0076C880, 0x00AF1470).
constexpr std::uintptr_t kAreaCameraResource = 0x011E9F44;
constexpr std::uintptr_t kFnAreaCameraCheck = 0x00841840;
constexpr std::uintptr_t kFnAreaCameraForce = 0x008417C0;
constexpr std::uintptr_t kFnAreaCameraSet = 0x0083D530;
// The enemy hit check (research/re5-engine-map.md "Enemy hit collection"): thiscall (enemy, part
// or 0), 1 when a pending hit goes on to OnHit. Developer probe only (dev "hitprobe").
constexpr std::uintptr_t kFnEnemyHitCheck = 0x009E0700;
// Uroboros Wesker's (uEm82, vtable 0x00F8D878) "may this hit hurt" (vtable +0x204, thiscall
// (region, amount), ret 8, AL), asked by uEnemy::ApplyDamage (0x009DEA50) before any damage: a
// region of 0x3C and up hurts while his core part (+0x1F50) is open (its type 2, its +0x1E24
// clear); any other drains his gauges (+0x1F9C, +0x1FA0) and does not (research/re5-engine-map.md).
constexpr std::uintptr_t kFnEm82MayHurt = 0x009217E0;
// Uroboros Wesker's second form (2026-11-22, the boss audit): thiscall (pose, apply), ret 8. It sets
// his pose byte (off::kEm82Pose); with apply 1 it switches his model variant (0x0084DBF0) and his hit
// shapes. The story's StageChange calls it; his brain's picker (0x009C8850) then draws his moves from
// the second form's table (0x01120648: the drill grab under 2 m, the ground slam, javelins), and
// Tornado (his body gauge +0x1F9C empty) sets his rage (off::kEm82RageBit, its timer
// off::kEm82RageTimer): the rage table (drill, slam, javelin) until the timer runs out and his core
// shows (CoreRev).
constexpr std::uintptr_t kFnEm82SetPose = 0x00921220;
// The boss Jill's (uEm84) tables, read only by her brain (2026-11-22, the boss audit): her chance to
// attack by distance band, far to near (6 floats: 0, 0, 0.3, 0.3, 0.9, 1; read at 0x009D041F), and
// her two fire cooldowns by the game's per-player level 0..10 (11 floats each, 2.5 s down to 0.3 s;
// read at 0x009CE3D1 and 0x009D04F7).
constexpr std::uintptr_t kJillAttackChance = 0x01120A64;
// The boss Jill's blow-away death (cEm84ActDieBlow, vtable 0x00F90560 - 0x10; its update 0x00947000)
// asks the application for the game over when its motion ends (`push 0x8000000; call 0x00726120` at
// 0x00947038): the story's "partner died" (2026-11-22, a rocket killed her in a run and the match
// ended). Her other death (DieNormal, update 0x00946D60) asks for nothing. The call's return address:
constexpr std::uintptr_t kJillDieBlowGameOverReturn = 0x00947042;
constexpr std::uintptr_t kJillFireCooldownA = 0x011209B8;
constexpr std::uintptr_t kJillFireCooldownB = 0x01120AA8;
// Uroboros Mkono's (uEm41, vtable 0x00F7D160 slot +0x18) update: `byte [esi+0x2118] = 0`, the
// Uroboros' update 0x008AE2B0, then its health set to its maximum (+0x1366) through vtable +0x10C
// (hooks_combat.cpp: a run's boss skips that).
constexpr std::uintptr_t kFnEm41Update = 0x008B6040;
// uEm40 vtable +0x258: may it dive under the floor to reach the players (their rooms, its timers).
constexpr std::uintptr_t kFnEm40CanDive = 0x008AA740;
constexpr std::uintptr_t kFnEm40Update = 0x008AE2B0;
// The flamethrower's unit, uWp21 (research/re5-engine-map.md, "The flamethrower").
constexpr std::uintptr_t kVtableWp21 = 0x0102D0C8;
// Stage s308's setup (aS308, vtable slot of 0x00F557B0; DTI 0x011B0DF4): the player's flamethrower
// looked up in the model list [0x011B2200] (0x007F2FD0, thiscall (kind, id), ret 8: a uBioModel
// whose +0xD50/+0xD54 match), and its fuel at +0x1AF8 read whether found or not; the scenes' base
// setup 0x0072D240 (thiscall) is what it calls first (stage_fixes.cpp).
constexpr std::uintptr_t kFnS308Setup = 0x0074CFB0;
constexpr std::uintptr_t kFnSceneBaseSetup = 0x0072D240;
constexpr std::uintptr_t kFnFindModel = 0x007F2FD0;
constexpr std::uintptr_t kModelList = 0x011B2200;
constexpr std::uintptr_t kFnPlActTestStart = 0x00C91600;
constexpr std::uintptr_t kFnPlActTestUpdate = 0x00C76EA0;
constexpr std::uintptr_t kFnActionDefaultDone = 0x00C71510;
constexpr std::uintptr_t kFnEnemyChangeAction = 0x009D9DC0;
// Action tables ({DTI, 0} per id; vtable slots 0xE8 table, 0xEC count): uPlayer 0x0113AF80 (177),
// uEm10 0x01115950 (157).
constexpr std::uintptr_t kFnPlayerActionTable = 0x00B7F090;
// The player's dodge picker (0x00B6CEF0) before a dodge: the threat point at player+0x2E10, then
// this prep (thiscall on [player+0x2D64]+0x220, the player as its argument; it resets a motion
// state unless the player is reacting to damage), then ChangeAction(avoid_*, 0, 0, -1, 0).
constexpr std::uintptr_t kFnPlayerDodgePrep = 0x00B82F10;
// uCharacter's turn to the yaw at +0x1904 (thiscall: the character, frames; ret 4), virtual
// +0x154 in every character vtable: under 2 frames it sets the unit's Euler Y (FUN_0048A7F0 get,
// FUN_00489CE0 set; Y = theta faces (sin theta, cos theta)) to that yaw at once, else it stores a
// per-frame step (+0x1908, +0x190C frames left, flag +0x18DD bit 2) that the virtual at 0x00B55DF0
// applies. The co-op sync feeds it; the local player's own turning does not use +0x1904 (it reads
// 0 there). Static analysis 2026-11-03.
constexpr std::uintptr_t kFnCharTurnTo = 0x00B55D40;
// Motions (static analysis 2026-11-03, research/re5-engine-map.md "Motions"). A model's 16 motion
// list slots are at +0xA88; a motion id is slot << 8 | index into that list, played per layer
// (layer n at +0x380 + n * 0xE0; layer 0 the body). The resource manager [kResourceManager]'s get
// (thiscall: type DTI, path, flags; ret 0xC) returns a cached resource with a reference added;
// flags bit 7 makes it a lookup that never loads (a missing file is the game's fatal error).
// Its release takes the manager in EAX and the resource in EDI. The slot setter (thiscall: list,
// slot; ret 8) is reference counted and refreshes the layers; the character's motion call
// (thiscall: layer, id, blend frames, start frame, speed, flags; ret 0x18; flag 4: play once) is
// the one enemies use at vtable +0x16C and the player's own calls for its upper layers.
constexpr std::uintptr_t kResourceManager = 0x01234580;
// Its table of the resources in memory (static analysis 2026-11-11: the get 0x00448B70 hashes the
// path, 0x00449400, and looks it up, 0x00448D30): 0x800 buckets of 4 resource pointers at +0x1038;
// a resource holds its path at +4 (inline, lower case), the path's hash at +0x58 and its type's
// (its DTI's +0x20) at +0x5C.
constexpr std::size_t kResourceTable = 0x1038;
constexpr int kResourceTableSlots = 0x800 * 4;
constexpr std::size_t kResourcePath = 0x4;
constexpr std::size_t kResourceTypeHash = 0x5C;
constexpr std::size_t kDtiTypeHash = 0x20;
constexpr std::uintptr_t kFnResourceGet = 0x00448B70;
constexpr std::uintptr_t kFnResourceRelease = 0x00447020;
constexpr std::uintptr_t kFnMotionListSet = 0x005CCDD0;
constexpr std::uintptr_t kFnCharSetMotion = 0x0084CFF0;
constexpr std::uintptr_t kDtiMotionList = 0x0124B574;  // rMotionList
constexpr std::uintptr_t kDtiEffectList = 0x0124ABE0;  // rEffectList (build/dti_all.txt)
// Starting an effect list on a unit, the game's way (static analysis 2026-11-11): the starters
// (0x007723E0 and its kin, called by every enemy and weapon) build the list's path, get it with a
// get that loads a missing file (the game's fatal error), then call the creator on the effect
// manager [kEffectManager]: thiscall (list, unit, joint -1, offset, 4, settings, group; ret 0x1C).
// It makes the effect (the manager's vtable +0x40), binds it to the unit (it follows it), copies
// the settings' scale and registers it with the unit manager under `group` (0x12 core lists, 0x14
// stage ones, 0x13 the others: 0x00770650). The enemies' own call (0x00853DEF) passes the offset
// kEffectOffsetZero and the settings kEffectSettingsDefault. The mod gets the list lookup-only
// instead, so a list no archive in memory holds is skipped, never loaded.
constexpr std::uintptr_t kFnEffectCreate = 0x00771E60;
constexpr std::uintptr_t kEffectManager = 0x01249C5C;
constexpr std::uintptr_t kEffectOffsetZero = 0x01249CF0;
constexpr std::uintptr_t kEffectSettingsDefault = 0x010E3BB0;
// The effect manager's stop of an effect by the id its start returned (the creator returns it):
// thiscall on [kEffectManager] (id), ret 4.
constexpr std::uintptr_t kFnEffectStop = 0x007712C0;
// uWp21 (the flamethrower)'s fire, its vtable +0x1C8 (static analysis 2026-11-11): thiscall
// (attack data), ret 4. It sets the firing flag (0x00B95810: +0x1C10 = 1), stops the stream effect
// it holds (+0x1C8C, kFnEffectStop), starts the stream wp2100 at joint 0 into +0x1C8C and wp2104
// (not kept) through the descriptor starter 0x007723E0 (thiscall on the manager: descriptor, unit,
// joint, offset, 4, settings; ret 0x18), and keeps the attack data at +0x1C80. Its update (vtable
// +0x1C, 0x00BBC8C0) turns up to 16 points of +0x1C8C's particles into hits (0x004E4120 reads them)
// and plays the flame's sounds while +0x1C8C is set: the flame's look and its hits are that effect.
constexpr std::uintptr_t kFnWp21Fire = 0x00BA4E00;
// uWp21's charge mode (fastcall: the flamethrower; static 2026-11-23): the unit is moved to a fixed
// spot (0x01032578..0x01032584: the story's pump), plays its motion 0x2B, is let go of the player
// (vtable +0x5C, +0x3C, +0x40) and +0x1C88 (mbChargeMode) = 1. Called by the player's put action
// (0x00C80630) for the flamethrower when the put is a charge (the action's +0x20 = 1) and a uWp21
// exists (0x006187F0 with its DTI 0x01204314), by the co-op message 0x0078AC30 and by 0x00BBCED0.
constexpr std::uintptr_t kFnWp21Charge = 0x00BA5470;
// The camera system, sCamera (sBioCamera, DTI 0x011B1950; static analysis 2026-11-11): its instance
// here (set by its constructor 0x0043FB20, cleared by its destructor), its 8 viewports (property
// mViewport, sCamera::Viewport 0x5E0 bytes each, from +0x30), each with its camera (mpCamera +4)
// and mActive (+0x10). A camera (uCamera, properties 0x004D4010) keeps the far plane +0x18, the near
// +0x1C, the aspect +0x20, the vertical field of view in degrees +0x24, the eye +0x30, the up +0x40
// and the target +0x50; its view (0x00835C20 -> 0x006D1030) and projection (0x00835C50 ->
// 0x006D1650) are right handed with row vectors (rm/screen_fx.h does the same).
constexpr std::uintptr_t kSCameraInstance = 0x0123457C;
// sPlayer's add-player (thiscall on [kSPlayerInstance]: slot, character, costume, a game work value
// for +0x4C0; ret 0x10): refuses a slot in use; fills the save's player entry of that slot from
// the character table (0x00C92D60 on save + slot data + 0x23300: the entry at +0x10 + slot*0x50,
// its items at +0x150 + slot*0x420), sets game work +0x480 (control: 1 = computer), +0x490 (-1)
// and +0x4C0 for the slot, creates the player (0x007E9800 -> 0x007E9730: unit group 6, number
// +0x2D9C) and sets the slot's bit in game work +0x478. The Port stage's own code (0x00741690)
// adds Josh this way: (2, 4, 0, 0), then sets +0x2DAC |= 1 and his position (+0x30).
constexpr std::uintptr_t kFnPlayerAdd = 0x007E9A50;
// The game's per-gun data, indexed by the item's low byte (research/re5-engine-map.md "Gun stat tables").
constexpr std::uintptr_t kWeaponRecords = 0x011B5170;       // 0x140 bytes each (0x00770380)
constexpr std::size_t kWeaponRecordSize = 0x140;
constexpr int kWeaponRecordCount = 0x50;
constexpr std::uintptr_t kWeaponCapacityLevels = 0x011C06A0;  // int32 rounds by LvBulletMax
constexpr std::uintptr_t kWeaponReloadLevels = 0x011C0664;    // float time multiplier by LvReloadSpeed
constexpr std::size_t kWeaponLevelStride = 0x1E8;
// A player's execution (a chainsaw's, every enemy kill move, the die_* actions): thiscall (player
// number, or -1 for the first one down). Starts the death camera that ends in the game over; only
// the campaign's mode filters which player it is for (research/re5-engine-map.md).
constexpr std::uintptr_t kFnPlayerDeathEvent = 0x0076D9F0;
// The match's game-over check (fastcall (state object), 3 callers in the main loop's states): with
// app +0x10438 & 0x8000000 set it switches the main state to 9, the game over (M10 F6: the flag is
// set by more than the death camera; in a split-screen match one player's death sets it).
constexpr std::uintptr_t kFnGameOverCheck = 0x00719F10;
// A message handler of the match (thiscall (sender, message), ret 8; one of the dispatch table's
// thunks at 0x00792D61, and called with sender -1 from 0x00792ADA): in a split-screen match (game
// work +0x58 mode 2 or 5) it checks the players (0x007E6220, 0x007E6370, 0x00B6B330), then writes
// the Mercenaries phase 2 (game work +0x67C) with message +8 and +0xC into +0x680/+0x690 and asks
// the application for 0x80000000: the match's end. Suspected (2026-11-21, not yet seen) to be what
// one player's fall sends, freezing the other (the user: "the other player's hud weapons and
// movement are disabled, basically is a softlock").
constexpr std::uintptr_t kFnMercEndEvent = 0x00792830;
// The application's request words (thiscall (flags, index), ret 8): ORs `flags` into +0x1043C +
// 4 x index (index 1..3; +0x1043C itself is their OR, rebuilt each frame at 0x0077E380). Word 1's
// 0x400000 stops every player's input (sBioPad's lock bytes); a split-screen fall asks for it from
// the game-over check (0x00719F8B) and from the UI right after the execution event (0x009E1754).
constexpr std::uintptr_t kFnAppRequestWord = 0x00726170;
// uPlayer (thiscall, bool): "dead or frozen" - its virtual +0x15C, or +0x1364 == 1 with +0x1BAC at
// or under 0, or bit 1 of the action flags (+0x2D7C) or of +0x2D98, else the application's freeze
// flag 0x20000000 - and "in the death sequence" (the same without the freeze flag). Some 45 places
// ask them; in a split-screen match a "dead" player freezes the other (the game-over check, the UI
// at 0x009E16A0).
constexpr std::uintptr_t kFnPlayerDeadOrFrozen = 0x00B6B380;
constexpr std::uintptr_t kFnPlayerInDeath = 0x00B6B330;
// aGame (the match's game task: [kLinkUnitRoot] + 0x382C, DTI name "aGame") keeps a byte per player
// at [+0x348][player]: the player is "off" (its read, 0x007162D0, has some 20 callers; the cockpit
// draws no weapon rings for a player that is off). Its setter 0x00719590 (thiscall (player, on,
// every view), ret 0xC) also sets the player's +0x2D64 -> +0x624 byte (0x00B5C470), its views
// (virtual +0xCC) and bit 0x800 of +0x2640 -> +4. A co-op fall does not call it (2026-11-21).
constexpr std::uintptr_t kGameTaskOffset = 0x382C;
constexpr std::uintptr_t kGamePlayerOffBytes = 0x348;
// uIdCockpit00 (the in-match HUD of one view, the weapon rings): its move (0x009F8FC0) first sets
// its player (+0x1A0) and that player's partner (+0x1A4, a live player or null; SetPlayer
// 0x009ED920), then asks its rings decision (thiscall, only the move calls it): it clears +0xC3,
// then sets it to 1 unless the stage is 109, the application is in a menu or frozen (+0x1043C &
// 0x8000, flags & 0x60000600), aGame +0x138 or the player's off byte is set, its player is not a
// live player or is dead (its virtual +0x15C, bit 1 of its action flags), and in a split-screen
// match (game work +0x58 not 1) the same for its partner (null skips it), its player's +0x2DA8,
// game work +0x79C or 0x0076C740 says no. With +0xC3 at 0 the move draws no weapon rings. (The
// inventory grid is another object, uIdWpSlot00 below.)
constexpr std::uintptr_t kFnCockpitRingsCheck = 0x009F1020;
constexpr std::uintptr_t kCockpitPlayer = 0x1A0;
constexpr std::uintptr_t kCockpitPartner = 0x1A4;
constexpr std::uintptr_t kCockpitRingsOn = 0xC3;
// cOpeWpSlot (a player's inventory grid, player +0x2654; vtable 0x010435B0) update, thiscall
// (player, pad), ret 8, from the player's move (0x00B83533, 0x00B8CD65, 0x00B8CE1B): the grid opens
// only off stages 100, 109 and 111, without application flags 0x600 or 0x08001000 (the game over
// among them), aGame +0x138 or the player's aGame bytes (+0x348, +0x34C).
constexpr std::uintptr_t kFnWpSlotUpdate = 0x00C6C420;
// uPlayer's "may act" check (thiscall, bool): no when +0x1364 is 1, when it is dead (virtual +0x15C,
// health at or under 0) or +0x1BCC is above 0, and in a split-screen match (game work +0x58 not 1)
// when its partner (0x00B5BED0 (1)) is dead. The inventory grid's open check (0x00C698B0, also
// application flags 0x60000000, action flags bits 1, 6 and 8, 0x00B5AB30) asks it, and so do five
// of the player's moves (0x00764895, 0x00B828D7, 0x00B840D3, 0x00B898BE, 0x00B89BDE).
constexpr std::uintptr_t kFnPlayerMayAct = 0x00B6A820;
// uPlayer's partner (thiscall (1), ret 4): its +0x2A74.. links, each kept only while it is a live
// player (0x007E6370).
constexpr std::uintptr_t kFnPlayerPartner = 0x00B5BED0;
// uPlayer's "may take this item" (0x00B65BF0, thiscall (item, ?, ?), ret 0xC; the ground item's
// prompt and its take ask it): a weapon already held says no, then outside the campaign (game
// work +0x58 not 1) it says no while the partner (kFnPlayerPartner, called here) is dead - bit 1
// of its action flags or of +0x2D98 - before it looks at the inventory: a co-op survivor got the
// game's red FULL with 7 of 9 cells free (2026-11-21). The call's own target is replaced.
constexpr std::uintptr_t kPickupPartnerCall = 0x00B65CD7;
// uIdWpSlot00 (the inventory grid on screen; vtable 0x00FACED4, move 0x00A741B0): its player at
// +0x2C and that player's partner at +0x30 (the move sets it right after this check). Its
// visibility check (thiscall, bool; only the move calls it) says no with request word 0x8000 or
// application flags 0x60000600, aGame +0x138 or the player's aGame byte, on stage 100, when its
// player is not a live player or is dead (virtual +0x15C, bit 1 of the action flags), and in a
// split-screen match (game work +0x58 not 1) when its partner is dead.
constexpr std::uintptr_t kFnWpSlotUiVisible = 0x00A70BE0;
constexpr std::uintptr_t kWpSlotUiPlayer = 0x2C;
constexpr std::uintptr_t kWpSlotUiPartner = 0x30;
// uPlayer's game over (virtual +0x1C4, thiscall): with bit 4 of the action flags it first calls its
// virtual +0x1B4, then, unless +0x2DA8 is 2, sets the application's game over flag 0x08000000
// itself under the application's lock (no request through 0x00726120). A fallen player calls it
// every frame once its death has played out (some 5 s after the fall).
constexpr std::uintptr_t kFnPlayerGameOver = 0x00B64B90;
// uPlayer: the hands take the item of a cell, thiscall (cell, 1), what the game's weapon_change
// action calls at its swap (research/re5-engine-map.md).
constexpr std::uintptr_t kFnPlayerEquipCell = 0x00B73B50;
// Twin guns (E16 spike): the boss Jill (uEm84) makes two uWp76 units with sItem's create of a uItem
// class (thiscall on [kItemManager]: dti, 0; 0x007BD750, from 0x009398E0), sets their rotation
// (thiscall: float euler[3], 0x00489CE0) and fires them with their +0x1C0 (0x00BB2B20 for uWp76:
// muzzle, direction, a value it returns, mode 3; a shot unit (7, 0x186) owned by the gun, which must
// be attached to a uPawn: the gun's +0x34 (0x00489860) stores parent +0x18 and joint +0x1C).
constexpr std::uintptr_t kFnItemUnitCreate = 0x007BD750;
constexpr std::uintptr_t kFnUnitSetEuler = 0x00489CE0;
constexpr std::uintptr_t kFnUnitAttach = 0x00489860;
constexpr std::uintptr_t kFnWp76Fire = 0x00BB2B20;
// A model's bone in the world (thiscall: out, bone id, offset): the bone id through the byte map at
// +0x31C to a joint of 0x90 bytes at +0x318 (its world matrix at +0x50), the offset transformed by
// it; a negative id takes the unit's own matrix (+0xA0). The boss Jill's fire (0x0093C2E0) puts
// her shots at her bones 11 and 6 (her hands) with the offset 0x01249CF0 (zero).
constexpr std::uintptr_t kFnJointPosition = 0x00B56650;
constexpr std::uintptr_t kDtiWp76 = 0x01204DE8;    // uWp76, the boss Jill's SMG
constexpr std::uintptr_t kItemJointTable = 0x0111C1F0;  // her guns' joints and offsets: 0x30 per (gun * 2 + pose)
// cEm10ActDeadArmyBomb (Majini action 0x93), its update 0x00C10BA0, step 0: the effect of the
// body's end. In game mode 5 (Reunion) any of uEm10/11/12/14/21/92 (0x00C00770) gets uEm14's
// em140a, which Excella's own archive carries; in mode 2 only uEm14 does and the others ask for
// their class's effect 0x25: em1025 is in no PC archive, and the game ends ("Failed open file
// ... em1025.efl"). Excella's finisher (cEmAction_pl_act_attack_neck, her variant: 350, 440,
// 170 frames) ends in it (user playtest 2026-11-03, the Mines). The check: `cmp [gw+0x58], 5;
// mov ecx, [esi+0x1C]; jnz` at kDeadBombModeCheck, the jnz (75 0B) at kDeadBombModeJump.
constexpr std::uintptr_t kDeadBombModeCheck = 0x00C10BFE;
constexpr std::uintptr_t kDeadBombModeJump = 0x00C10C11;
constexpr std::uintptr_t kFnEm10ActionTable = 0x00868590;
constexpr std::uintptr_t kFnHitGetAttacker = 0x00C49530;  // cUnit* cHit::GetAttacker()
constexpr std::uintptr_t kFnMercAddTime = 0x00718050;     // void ?::MercAddTime(int seconds)
// void cMercs::UpdateCountdown(float dt): counts [this+0x4DC] down, sets the
// time-up state [this+0x4D4] = 1 at zero (or at the 150/400 kill cap) and
// mirrors the value into game work +0x680. Called once per frame in a match.
constexpr std::uintptr_t kFnMercUpdateCountdown = 0x00717850;
// The Mercenaries start, the game's main state 6 (static analysis 2026-11-11): aGame's dispatcher
// 0x00725E80 calls it while aGame's state byte (+4) is 6, aGame in ECX. State 1's step 1 sends a
// Mercenaries match (game work mode 1, 2 or 5) here. It locks every player's controls each frame
// (0x007E63C0(1)); its step (aGame +5) 0x0A sets the match up and fades in, 0x0C starts the intro's
// next camera shot (0x00717640, an event from the table 0x010CA908: four variants of three shots,
// 40..47, each ended by -1), 0x0D waits for it, a pad's press (the skip mask [0x01249C1C]+0x186C,
// 0x00785E40) going to 0x14, and 0x0E waits 0.1 s and goes back to 0x0C. A -1 in the table goes to
// 0x14 too: fade out (0.25 s), the event's state cleared (0x007D8350), the match's resources (steps
// 0x1E, 0x1F), then 0x20 sets main state 2, the match. A run's match goes from 0x0C to 0x14, the
// table's end, so no shot plays (hooks_merc.cpp). The match update's own intro (0x007245D0: game
// work phase 0 counts its +0x4BC down) is not it: alpha.16 set that timer to 0 whenever it read
// more, never logged it (adapter.log 2026-11-11 13:31), and the intro played its 10 s all the same.
constexpr std::uintptr_t kFnMercStartState = 0x0071F840;
// cMercs::AddTime(this, int seconds) (Ghidra FUN_0071CBF0): every countdown bonus (combo
// +5 s, time-bonus pickups) goes through it.
constexpr std::uintptr_t kFnMercAddCountdown = 0x0071CBF0;
// The time-bonus pickup's action update (FUN_00AD9130, vtable slot of the action object at
// 0x00FEFF7C): state byte +0x14 (0 delay, 1 grant, 2 fade out), +0x18 the map object
// (seconds as int16 at +0x1364).
constexpr std::uintptr_t kFnTimeBonusAction = 0x00AD9130;
// In-match cockpit HUD uIdCockpit00 (vtable 0x00F9E480, move 0x009F8FC0 = slot 6): its move
// clears every child sprite's draw byte (+8), copies the Mercenaries countdown (game work
// +0x680) into +0x34C with the show flag +0x344, and its timer draw (FUN_009F0390) then sets
// the six digit sprites and the separators. Skipping the draw leaves the clock off screen.
constexpr std::uintptr_t kFnCockpitDrawTimer = 0x009F0390;
// Mercenaries HUD uIdMerce00 draw tail (FUN_00A1E580, called from its move 0x00A1E650): clears
// the draw byte of every element of its two layout pages (handles at +0x44/+0x48), then (unless
// the game is paused) its helpers switch the score, combo and bonus panels back on.
constexpr std::uintptr_t kFnMerceHudDraw = 0x00A1E580;
// Layout sprite manager (sLayoutSpr) and the two calls that clear loop makes:
// elements = PageElements(this = manager, page) (count at elements+0xC), and
// SetElementDraw(this = manager, draw, page, element) writes the element's draw byte (+8).
constexpr std::uintptr_t kLayoutManager = 0x011B2334;
constexpr std::uintptr_t kFnLayoutPageElements = 0x007C16B0;
constexpr std::uintptr_t kFnLayoutSetElementDraw = 0x007C1600;
// Archive loader (research/re5-engine-map.md, "Resources"): the instance, the commit of the
// list being built (FUN_007D4A10), and the two ways to queue a unit class's archive:
// LoadClassArchive(this, dti, flag) (FUN_007D6770, the class id at dti+0x20) and
// LoadUnitArchive(this, category, id, variant, flag) (FUN_007D5820, class id
// category<<24 | id<<8 | variant; it also queues the class's companions).
constexpr std::uintptr_t kResourceLoader = 0x011B26C4;
constexpr std::uintptr_t kFnLoadListCommit = 0x007D4A10;
constexpr std::uintptr_t kFnLoadClassArchive = 0x007D6770;
constexpr std::uintptr_t kFnLoadUnitArchive = 0x007D5820;
// LoadRequest(this = loader, path, dti or 0, flag) (FUN_007D4960, ret 0xC): queues
// "Image\Archive\<name>" on the list being built (an existing path is only kept).
constexpr std::uintptr_t kFnLoadRequest = 0x007D4960;
// Object manager sObjModel (instance [0x011B2720], vtable 0x00F65240): Create (slot 11,
// this, dti, group, mode, flag; ret 0x10) and the set spawn (slot 13, this, group, id; ret 8).
// Set record of a set manager (sEnemy or sObjModel): GetRecord(this, group, id), record +0xC
// the class name holder (+8 the name), +0x10 the descriptor.
constexpr std::uintptr_t kObjModelManager = 0x011B2720;
constexpr std::uintptr_t kFnObjCreate = 0x007E2410;
constexpr std::uintptr_t kFnObjSetSpawn = 0x007E2480;
constexpr std::uintptr_t kFnSetRecord = 0x007F3D70;
constexpr std::uintptr_t kFnSetRecordCount = 0x007F3DD0;  // RecordCount(this = set manager, group)
// The area load's layout pass (FUN_0072B6B0, thiscall, one stack argument, ret 4: one of the
// stage's four archives): each rLayout resource ("lot", archive +0x60 the resource array, +0x64
// its count; resource +4 its path) goes to RegisterLayout, except that in a Mercenaries match
// (game mode 2) only the ones under `scr\` or `soft\merce\` do: a campaign stage's own object
// layouts (`soft\sNNN_IDfNN_Ins`: its ladders) are skipped. RegisterLayout (FUN_00728D20, one
// stack argument, ret 4) files a `..._Ins` layout with sObjModel (FUN_007F3C60) at the group its
// name gives (`_id` + s 0, m 1, o 2, f 3, times 20, plus the number: IDf01 is 61).
constexpr std::uintptr_t kFnStageLayouts = 0x0072B6B0;
constexpr std::uintptr_t kFnRegisterLayout = 0x00728D20;
// Item manager sItem (instance [0x011B2200], vtable 0x00F63578): Create (slot 11) and set
// spawn (slot 13), the same calls as sObjModel's.
constexpr std::uintptr_t kItemManager = 0x011B2200;
constexpr std::uintptr_t kFnItemCreate = 0x007BE760;
constexpr std::uintptr_t kFnItemSetSpawn = 0x007C0A80;
// uOmHourGlass (the Mercenaries time-bonus statue, DTI 0x011FCAC0, vtable 0x00FEFFB0):
// constructor (this; returns this), bonus seconds int16 at +0x1364 (default 60).
constexpr std::uintptr_t kFnHourGlassCtor = 0x00AD8CB0;
// Mercenaries match setup (FUN_00BF5050, cdecl, no arguments): stage choice gw+0x5D4 ->
// stage number (table 0x010CB9B0: 400..407) -> the save's progress stage.
constexpr std::uintptr_t kFnMercMatchSetup = 0x00BF5050;
// Save/progress data: [kSaveData] + [+0x20 slot] * 0x5940; +0x273D8 the stage number of the
// current/next match, +0x273DC its room (0 for Mercenaries).
constexpr std::uintptr_t kSaveData = 0x011B2158;
// The engine's fatal error handler: (context, format, ...) formats, calls the context's
// report callback (FatalAppExitA through 0x00422D00) and exits. Callers include
// "ERROR!!!! sActionButton::setResource : no resource" and "Failed open file. %s %d".
constexpr std::uintptr_t kFnGameFatalError = 0x006CFEF0;
// Class by class id (FUN_006D0690, cdecl): hash buckets at 0x012386A0, chain +0x14, id +0x20.
constexpr std::uintptr_t kFnClassById = 0x006D0690;
// Play menu screen (uIdStartUp01) per-frame move, and the menu input query
// (__stdcall int(int pad, int mode)) it calls first.
constexpr std::uintptr_t kFnPlayMenuMove = 0x00A396C0;
constexpr std::uintptr_t kFnMenuInput = 0x009E3420;
// Play menu row/help draw, called from inside its move(); reads the enabled mask.
constexpr std::uintptr_t kFnPlayMenuDraw = 0x00A373B0;
// A menu row's selection glow (thiscall (index 1..8, message, selected, y), ret 0x10): the
// layout's element "sel_%02d" gets the message's width, is drawn, and with a y of 0 or more
// stands there (+0x54) instead of its own row. The Play menu's draw passes its rows' (-1).
constexpr std::uintptr_t kFnMenuRowGlow = 0x00A17E90;
// A layout's named element: the name (a ref-counted string made by kFnLayoutName, thiscall
// (out, const char*)) is looked up with kFnLayoutElement, thiscall on [kLayoutManager] (the
// name, which it releases, and the screen's layer, screen +0x18).
constexpr std::uintptr_t kFnLayoutName = 0x00425DC0;
constexpr std::uintptr_t kFnLayoutElement = 0x007C1DC0;
constexpr std::size_t kLayoutElementY = 0x54;  // float
constexpr std::uintptr_t kPlayMenuLabelTable = 0x01131D90;  // uint16 message per row
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
// M10: the pieces of Navigate and of the menu input query, for one player's own pad. The pad object
// ([kPadInstance]) keeps each pad's menu bits at +pad * off::kPadStride: +off::kPadTrigger (pressed
// this frame, 0x00799820) and +off::kPadRepeat (repeating, 0x007998B0); Navigate ORs its main pad's
// (+0x5D8) with the keyboard's (thiscall on the pad object (1): kFnKeyboardTrigger, kFnKeyboardRepeat)
// and hands both to kFnMenuListStep (thiscall (trigger, repeat, cursor, count), ret 0x10, `this`
// unused), which steps with the masks kMenuMaskDown and kMenuMaskUp. kMenuMaskRight and
// kMenuMaskLeft are the two other directions of the same table (0x01121C70..7F).
constexpr std::uintptr_t kFnKeyboardTrigger = 0x0079B340;
constexpr std::uintptr_t kFnKeyboardRepeat = 0x0079B530;
constexpr std::uintptr_t kFnMenuListStep = 0x009E35D0;
// The characters' names the HUD prints as text (ASCII, by character: Chris, Sheva, Jill, Wesker, ...):
// the inventory tabs (read at 0x00A70529) and a cockpit label (read at 0x009F34D6).
constexpr std::uintptr_t kInventoryNameTable = 0x011341E8;
constexpr std::uintptr_t kCockpitNameTable = 0x01123460;
constexpr std::uintptr_t kMenuMaskRight = 0x01121C70;
constexpr std::uintptr_t kMenuMaskLeft = 0x01121C74;
constexpr std::uintptr_t kMenuMaskDown = 0x01121C78;
constexpr std::uintptr_t kMenuMaskUp = 0x01121C7C;
constexpr std::uintptr_t kFnMenuSoundDeny = 0x009E4AC0;
// The cursor-move tick (sound id 0x42): the Play menu's move plays it when its cursor changes
// (after Navigate and the sId mouse hit test, 0x00A397DF).
constexpr std::uintptr_t kFnMenuSoundMove = 0x009E4AA0;
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
// Mercenaries results screen uIdMerceEd00 (vtable 0x00FA71D0): move() = slot 6. It reads
// the menu input query like the setup screens, so a decide can be fed to it; "Aceptar"
// sends the game back to its Mercenaries menu scene (0xB), where the setup script runs again.
constexpr std::uintptr_t kFnMerceResultsMove = 0x00A20890;
// Pause screen uIdPause00 (vtable 0x00FA821C): move() = slot 6, runs every frame of a match.
constexpr std::uintptr_t kFnPauseMove = 0x00A308C0;
// The pause menu pauses the world by setting kAppPauseRequest in the application flags
// (under the application's critical section when it runs threaded) and clears kAppPaused
// when it closes. Import thunks of Enter/LeaveCriticalSection as the game calls them.
constexpr std::uintptr_t kAppForceLock = 0x01249C2E;
constexpr std::uintptr_t kImportEnterCriticalSection = 0x00F4A0F8;
constexpr std::uintptr_t kImportLeaveCriticalSection = 0x00F4A164;
constexpr std::uintptr_t kFnFlowMercSetup = 0x00BF78B0;
// The split-screen setup script (MODE SELECT row 2, M10 F0): state 0 resets both players' fields
// (gw+0x620/+0x624 present, +0x654/+0x658 pads -1), 0xB Step(ctx, 0) the stage screen, 0xC creates
// the two-player lobby uIdStartUp24 itself (FUN_00A46690) and waits for it, 0x14 starts the match
// (scene request 0x27).
constexpr std::uintptr_t kFnFlowSplitSetup = 0x00BF8070;
// Inventory (found with Ghidra from uPlayer::PickUpItem, FUN_007E9DF0): the
// inventory object is uPlayer+0x21A0; __thiscall helpers on it:
//   int AddToStacks(int item, int count): tops up existing stacks of `item`
//     up to each slot's max, returns what did not fit;
//   int AddAmmo(int item, int count).
// Item ids are (category << 8) | index: 1 weapons, 2 ammo, 4 keys/treasure.
constexpr std::uintptr_t kFnInventoryAddToStacks = 0x00C4BEE0;
constexpr std::uintptr_t kFnInventoryAddAmmo = 0x00C4DA90;
//   void* Create(int item, int count, int a, int b, int c, int flags, int attr[9],
//                uPlayer* player, int position, int zero): new slot; PickUpItem
//     passes (-1, -1, -1, 0xD = find a free slot, the world item's 9 attribute
//     bytes, the player, a default position, 0). Returns the slot or 0.
constexpr std::uintptr_t kFnInventoryCreate = 0x00C4B480;
// Inventory record clear (FUN_00C4C4E0, this = record, flag 1 = also delete the record's
// weapon object +0x2C through the unit manager): the game's own discard; wiping the record
// by hand leaves the weapon model holstered on the character.
constexpr std::uintptr_t kFnInventorySlotClear = 0x00C4C4E0;
// Enemy spawning (roadmap E1/E2). sEnemy::Create(this = [kSEnemyInstance], dti, group, mode,
// flag) is vtable slot 11; the game's set spawn (scripts, Mercenaries waves) is kFnEnemySetSpawn
// (this = sEnemy, group, id, flag). Class lookup by name: kFnClassLookup(name, base DTI).
constexpr std::uintptr_t kFnEnemyCreate = 0x007AF550;
constexpr std::uintptr_t kFnEnemySetSpawn = 0x007AF5F0;
constexpr std::uintptr_t kFnClassLookup = 0x006D05F0;
constexpr std::uintptr_t kDtiLookupUnitBase = 0x011AF714;  // base passed by the set spawn for unit classes
constexpr std::uintptr_t kDtiSetInfoPawn = 0x0120BEA0;     // cSetInfoPawn DTI (descriptor base)
constexpr std::uintptr_t kFnLinkUnits = 0x0072CD00;        // this = [[kLinkUnitRoot] + kLinkUnitManager], unit
constexpr std::uintptr_t kLinkUnitRoot = 0x01234578;
constexpr int kEnemyUnitGroup = 9;
// cSetInfoEm10 vtable (dump: slot 6 = apply 0x00CC70E0, the descriptor's "Apply to Pawn").
constexpr std::uintptr_t kSetInfoEm10Vtable = 0x01049C94;
constexpr std::uintptr_t kFnSetInfoEm10Apply = 0x00CC70E0;
// Campaign shop screen uIdShop00 (vtable 0x00FA8878): the title-flow step FUN_00BF24D0 creates
// it with kFnShopCreate (ECX = kShopCreateThis), sets obj+4 |= 0x2000, registers it with
// kFnUnitRegister(this = [kUnitManager], group 0x1A, obj), waits for obj+0x3C (closed) and
// deletes it with obj->vtable[0x30] after clearing 0x2000.
constexpr std::uintptr_t kFnShopCreate = 0x00A35C10;
constexpr std::uintptr_t kShopCreateThis = 0x011F5608;
constexpr std::uintptr_t kFnUnitRegister = 0x00428B80;
constexpr std::uintptr_t kUnitManager = 0x012340B0;
// Campaign Organize/Buy screen uIdItemBox00 (vtable 0x00FA60A8, 0x5B88 bytes): the match's phase
// 3 (FUN_007189F0) loads Image\Archive\ShopResource (layout id\jpn\itembox\itembox_360 and its
// textures) into loader list 1, creates the screen with kFnItemBoxCreate() (cdecl, allocates and
// constructs), sets obj+4 |= 0x2000, registers it (kFnUnitRegister, group 0x1A), stores it at
// [kGameFlow] + off::kGameFlowItemBox, waits for obj+0x3C and deletes it like the shop screen.
constexpr std::uintptr_t kFnItemBoxCreate = 0x00A07980;
// Item records: [kItemTableRoot + category*4] + index*0x48; dword 2 = magazine (weapons) or
// pickup amount, dword 8 = stack maximum (-1 for weapons), dword 12 = kind (3 = weapon).
constexpr std::uintptr_t kItemTableRoot = 0x010EACF0;
constexpr std::uint32_t kItemUnarmed = 0x0100;
constexpr std::uint32_t kItemKnife = 0x0101;
constexpr std::uint32_t kItemM92F = 0x0102;
constexpr std::uint32_t kItemHandgunAmmo = 0x0201;
// Scene manager singleton: the title-flow script requests the Mercenaries match with
// FUN_007F1610(this = [kSceneManager], 0xD, 0), which writes this+0xF8 / this+0xFC.
constexpr std::uintptr_t kSceneManager = 0x011B1DD8;
// Game-flow singleton ([kGameFlow]): the Mercenaries results screen leaves the match by
// calling kFnFlowToMercMenu(this) (Ghidra FUN_00794F30), which sets this+0x1304 = 0xB
// (scene), this+0x130C = 0 and this+0x131C = 1 (transition pending) when no transition
// is running (this+0x12E0/0x12E4/0x12F0 == 0) and this+0x38 (current scene) is set.
constexpr std::uintptr_t kGameFlow = 0x01249C1C;
constexpr std::uintptr_t kFnFlowToMercMenu = 0x00794F30;
// Mercenaries game-over screen uIdGameover00 (vtable 0x00FA54A8, "Continue? Yes / No"):
// move() = slot 6. State 1 fades "you are dead" in, 2 waits for the choice (cursor +0x190:
// 0 yes, 1 no). The decide sets +0x196 = 1, a 30-frame delay at +0x198 (the float at
// kGameoverDecideDelay), calls kFnGameoverDecide(this) (the application's retry 0x80000 or
// quit 0x10000 flag) and goes to state 3, then the exit states 0x5A..0x5C. The match's death
// handler (FUN_00725030) then restarts the stage or, on quit, enters the match's quit state
// (FUN_00720A50): it shows a save screen ("data saved", the game's save is written) when the
// Mercenaries phase (game work +0x67C) is set, then changes to the title scene, from where the
// title flow re-enters the Mercenaries menu. Offline, the game-flow requests of these screens
// (kFnFlowToMercMenu, FUN_00794EC0) only record a result: [kGameFlow]+0x38 is null.
constexpr std::uintptr_t kFnGameoverMove = 0x00A00B40;
// The Organize/Buy screen's per-player panel update (thiscall: the screen, the panel; the panel
// at screen+0x1B0 + player*0xA64): mode byte +0 (0 top menu: Organize, Buy, Exit with the cursor
// at +0x19; 1 the organize grid; 7 Buy; 8 Sell; 10 Upgrade), step +1, player +2. Its tab setter
// (thiscall: the screen, the panel, the tab; 0 the grid's layout) goes with a mode change.
// Static analysis 2026-11-03 (mes_system 0x168 + the help ids name the modes).
constexpr std::uintptr_t kFnItemBoxPanelUpdate = 0x00A0EF00;
constexpr std::uintptr_t kFnItemBoxPanelTab = 0x00A09A40;
// The same screen's item table: 105 records of 12 bytes (u16 item; u16 picture, the layout
// element "item_NNN" cut from cockpit\cp05; u16 most per storage record, 1 for a gun; two flag
// bytes; the model path its preview loads). Its picture lookup (stdcall: buffer, item) names
// "item_094", which the layout lacks, for an item without a record; the grid's draw, the model
// request and the storage sort (by record order) read the records directly, all within
// 0x00A02950..0x00A06D10. Static analysis 2026-11-03 (the layout's rects decoded from cp05).
constexpr std::uintptr_t kFnItemBoxPictureName = 0x00A02A10;
constexpr std::uintptr_t kItemBoxTable = 0x01128C70;
constexpr int kItemBoxTableRecords = 105;
// The match's quit state (fastcall this = the match): its step byte at +5; step 0, in
// Mercenaries (game mode 2), opens the save screen and writes the game's save when the
// Mercenaries phase (game work +0x67C) is set. The game over's "No" and the pause menu's quit
// both end here (2026-11-03: a run quit from the pause menu wrote the save).
constexpr std::uintptr_t kFnMatchQuitState = 0x00720A50;
// The game's save file write: the resource loader's ([0x011B26C4]) step for a request of type 4
// (thiscall: request; the dispatcher 0x007D53E0 calls it while the request's +0x48 lacks bit 4).
// It names the file "savedata.bin" (0x007A5C90), copies 0x5D50 bytes (the file's size) from
// [0x011B1DD8]+0x110, writes, reads back and compares; a write that went through ends with the
// request's +0x48 = 4 and 1 returned. Its step is the loader's byte +0x21 (0 = not begun).
// A pause menu quit of a run wrote the save with no quit state (0x00720A50) on the way (user
// playtests 2026-11-03, 2026-11-04, 2026-11-05).
constexpr std::uintptr_t kFnSaveFileWrite = 0x007D4510;
// The application's request word (thiscall on [kGameRoot]: flags ORed into +0x10438 under its
// lock). 0x10000 makes the running match (main state 2, 0x00724A20) leave for the menus through
// state 0xC, as the pause menu's forced exit does (developer "quitmatch").
constexpr std::uintptr_t kFnAppRequest = 0x00726120;
constexpr std::uintptr_t kFnGameoverDecide = 0x00A00550;
constexpr std::uintptr_t kGameoverDecideDelay = 0x00FF02FC;
// void aGame::NotifyItemGet(int item, int count): the game's "item acquired"
// notice (uIdItemGet00) with the item's own name and icon.
constexpr std::uintptr_t kFnGameNotifyItem = 0x00719B60;
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
// uPlayer: the current action object (cPlAction_pl_act_*: relax at rest, weapon_fire while a shot
// lands, attack_* for the hand-to-hand moves, knife_attack).
constexpr std::size_t kPlayerAction = 0x10F8;
// uCharacter actions: the instances, one per id (MtArray at +0x10CC: count +0x10D0, data +0x10DC),
// the requested id (+0x10E0, INT_MIN none) and the yaw (+0x1904, radians: the co-op sync writes a
// packed short times 2 pi / 65536 there; the target of the turn, addr::kFnCharTurnTo).
constexpr std::size_t kCharActionCount = 0x10D0;
constexpr std::size_t kCharActionData = 0x10DC;
constexpr std::size_t kCharActionRequest = 0x10E0;
// The request store (0x00B59340): a request while none is pending moves the last one to +0x10E4,
// and +0x10EC (byte) is set until the action step (0x00B56480) takes it.
constexpr std::size_t kCharActionPrevRequest = 0x10E4;
constexpr std::size_t kCharActionPending = 0x10EC;
constexpr std::size_t kCharYaw = 0x1904;
// uEnemy: its target (a player; cleared by uEm40's update 0x008AE2B0 when it is no unit, read by
// uEm40's far grab 0x008A85E0 to pick its attack). Majini code sets it widely (their search).
constexpr std::size_t kEnemyTarget = 0x1BA4;
// uEm40: the story's waiting mode (byte, 1 from its constructor): its brain (uEmIntel40) walks it and
// never attacks until story flag 9 clears it (0x0099C39F), and asks for a roar (kEm40WakeRoar) then.
constexpr std::size_t kEm40StoryWait = 0x20C0;
constexpr std::size_t kEm40WakeRoar = 0x1FE9;
// uEm40's arms and head: int16 life, then its maximum (its descriptor's field + 200, 0x008AE1E0).
constexpr std::size_t kEm40ArmLLife = 0x1F74;
constexpr std::size_t kEm40ArmRLife = 0x1F7C;
constexpr std::size_t kEm40HeadLife = 0x1F84;
// uEm40 on s103: the room (0..5, -1 none) of player 0 and player 1, set by its update.
constexpr std::size_t kEm40RoomPlayer1 = 0x20D8;
constexpr std::size_t kEm40RoomPlayer2 = 0x20DC;
constexpr std::size_t kEm41UpdateFlag = 0x2118;  // cleared first by Mkono's update
constexpr std::size_t kPlayerThreatPoint = 0x2E10;  // float x, y, z: what a dodge is relative to
constexpr std::size_t kPlayerDodgeOwner = 0x2D64;   // the dodge prep's object (+0x220)
constexpr std::size_t kDodgePrepOffset = 0x220;
constexpr int kPlActRelax = 0x01;       // cPlAction_pl_act_relax
constexpr int kPlActAvoidFront = 0x51;  // cPlAction_pl_act_avoid_front (toward the threat point)
// cPlAction_pl_act_test: a hold. Its start is only the common one (0x00C8B1E0: the action's step
// bytes) and its update is empty (0x00C76EA0): no motion of its own and it never marks itself
// done, so a motion played over it plays out. (setEventAct_wait, 0x84, marks itself done on its
// second update and the player drops back to wait: user playtest 2026-11-03.)
constexpr int kPlActHold = 0xA8;
// Its vtable (11 slots): slot 6 (+0x18) the start (the thunk 0x00C91600 to 0x00C8B1E0), slot 7
// (+0x1C) the update 0x00C76EA0, slot 8 (+0x20) the end (the default 0x00C71490, which runs slot 10
// until the done bit is set), slot 10 (+0x28) the default 0x00C71510 (sets the done bit). The
// action step 0x00B56480 ends the old action (its slot 8: the done bit set) before it starts the
// new one; other starts clear the flag word (0x00B7A240), the hold's does not, so it began "done"
// and the game asked for wait again the same frame (the mod's runs, 2026-11-05: the request taken,
// wait asked for, before the next tick). Addresses: addr::kVtablePlActTest and the slots' functions.
constexpr std::size_t kActionSlotStart = 0x18;
constexpr std::size_t kActionPlayer = 0x18;  // an action's player
// uPlayer: the current action's flag word. Bit 28 = the action may be left for the next one (set
// when an action is done; hundreds of input checks test it, the action's step loop 0x00C71490
// runs until it is set).
constexpr std::size_t kPlayerActionFlags = 0x2D7C;
constexpr std::uint32_t kActionDone = 0x10000000;
// Motions (addr::kFnCharSetMotion): the model's motion list slots and layer count, and layer 0's
// motion id (u16), current frame and length.
constexpr std::size_t kMotionLists = 0xA88;
constexpr int kMotionListSlots = 16;
constexpr std::size_t kMotionLayerCount = 0x374;
constexpr std::size_t kMotionLayer0Id = 0x384;
constexpr std::size_t kMotionLayer0Frame = 0x398;
constexpr std::size_t kMotionLayer0Frames = 0x39C;
constexpr std::size_t kModelJointCount = 0x314;
constexpr std::size_t kModelBoneToJoint = 0x31C;  // byte table: a motion track's bone id -> joint (0xFF none)
constexpr std::size_t kVtableChangeAction = 0xDC;
// An enemy's hit check (ApplyDamage asks it before any damage; thiscall: the hit's region, an amount;
// ret 8; nonzero: the hit takes health). Uroboros Aheri's (0x008C0CD0, static 2026-11-22): a region of
// 0x3C and up is core (region / 0x3C - 1): its health at +0x1FC4 + core * 8 (short, max at +0x1FC6)
// goes down, a broken core sets +0x1E2C bit core; with her +0x18DC bit 2 (the main core out) those
// hits add to +0x2154 instead (over 2000: +0x1E2C |= 0x8000). Any other region is her body: the hit
// takes health.
constexpr std::size_t kVtableMayHurt = 0x204;
constexpr std::size_t kVtableTurnTo = 0x154;
constexpr std::uint32_t kCollisionMask = 0x3FFFFF00;  // the dodge picker's and the ground snap's
constexpr std::uint32_t kSegmentBlocked = 4;          // the dodge picker's "blocked" bit
// uEm10 (Majini) reactions, from its action table.
constexpr int kEm10DmgHead = 0x6D;    // cEm10ActDmgHead: head stagger (the melee prompt's state)
constexpr int kEm10DmgFlyoff = 0x73;  // cEm10ActDmgFlyoff: blown away
constexpr int kEm10DmgYoroke = 0x7C;  // cEm10ActDmgYoroke: stagger
// uPlayer
constexpr std::size_t kWeaponScopeType = 0x98;   // weapon record: not 0 = aims through a scope (the game reads it every frame)
constexpr std::size_t kSlotLevelReload = 0x22;   // byte of the slot's levels (kSlotUpgrades + 2)
constexpr std::size_t kSlotLevelCapacity = 0x23; // byte of the slot's levels (kSlotUpgrades + 3)
// A gun that never runs out: the game's own Infinite Ammo bonus sets this bit of a picked-up gun's
// capacity level (`or byte [slot+0x23], 0x80` at 0x007EA03C, when the player's bonus mask has that
// gun), and the may-use-a-round check 0x00C4C200 calls any capacity level past 13 infinite (before
// the weapon table's own flags). The shot then takes no round (0x00C8A188: the check, else
// 0x00C4C1E0 subtracts one) and the has-rounds check 0x00C4C2F0 says yes at 0 rounds.
constexpr std::uint8_t kSlotInfiniteBit = 0x80;
// Uroboros Wesker (uEm82), addr::kFnEm82SetPose: his pose byte (1: the second form), his flags (the
// rage bit) and his rage timer (float seconds; CoreRev once it is out).
constexpr std::size_t kEm82Pose = 0x1F85;
constexpr std::size_t kEm82Flags = 0x1BB4;
constexpr std::uint32_t kEm82RageBit = 0x1000;
constexpr std::size_t kEm82RageTimer = 0x1F90;
constexpr std::size_t kPlayerWeaponCell = 0x2DD8;  // int32: the grid cell of what is in the hands
constexpr std::size_t kPlayerNumber = 0x2D9C;   // uint8, copied into enemy+0x136C on hit
constexpr std::size_t kPlayerControl = 0x2DA8;  // int32 from game work +0x480 + slot*4: 0 the local player, 1 a computer one
constexpr std::size_t kPlayerAddFlags = 0x2DAC;  // the Port stage sets bit 0 on the Josh it adds
// Game work's player slots (addr::kFnPlayerAdd): the bits of the players in the match, then per
// slot (4 bytes each) the control, a second value (-1 when added) and the add's last argument.
constexpr std::size_t kGameWorkPlayerBits = 0x478;
constexpr std::size_t kGameWorkPlayerControl = 0x480;
constexpr std::size_t kGameWorkPlayerSecond = 0x490;
constexpr std::size_t kGameWorkPlayerArg = 0x4C0;
// The save's player entries (addr::kFnPlayerAdd): from save + slot data + 0x23310, 0x50 each;
// their items from + 0x23450, 0x420 each.
constexpr std::size_t kSavePlayerEntries = 0x23310;
constexpr std::size_t kSavePlayerEntry = 0x50;
constexpr std::size_t kSavePlayerItems = 0x23450;
constexpr std::size_t kSavePlayerItemsSize = 0x420;
// uPlayer inventory: 24 slots of 0x30 bytes starting at inventory+4.
constexpr std::size_t kPlayerInventory = 0x21A0;
constexpr int kInventorySlots = 0x18;
constexpr std::size_t kInventoryFirstSlot = 0x4;
constexpr std::size_t kInventorySlotSize = 0x30;
// The record of the gun carried on the back, cached by the inventory's lookup 0x00C4B700(0x10) (a
// gatling or longbow first; else kept while the gun in hand is not a back gun). 0x00C4DFF0 attaches
// that record's gun to the back joint after every re-attach (Chris's 0x75, Sheva's 2). Nothing clears
// it when the record is discarded (2026-11-19, research/re5-engine-map.md "The back gun").
constexpr std::size_t kInventoryBackGun = 0x484;
constexpr std::size_t kSlotItemId = 0x4;   // int32
constexpr std::size_t kSlotCount = 0x8;    // int32
constexpr std::size_t kSlotMax = 0xC;      // int32 stack capacity
constexpr std::size_t kSlotValid = 0x29;   // uint8 == 1 for a used slot
constexpr std::size_t kSlotEquipped = 0x1C;  // int32 == 1 for the weapon in the player's hands
// A weapon's upgrade levels, one byte per stat over two uint32 (M92F: byte 0 firepower, 2 reload,
// 3 capacity, 5 critical); read live (a reload fills to the capacity level). Mercenaries guns come
// with levels set (M92F 0A000005 00000200: capacity 10 = 50 rounds).
constexpr std::size_t kSlotUpgrades = 0x20;
// Scene manager ([addr::kSceneManager]): pending scene request and its argument.
constexpr std::size_t kSceneRequest = 0xF8;
constexpr std::size_t kSceneRequestArg = 0xFC;
// Enemy spawning.
constexpr std::size_t kSEnemyVtableCreate = 0x2C;
constexpr std::size_t kSetInfoPosition = 0x20;    // float x, y, z
constexpr std::size_t kSetInfoEm10Weapon = 0xF0;  // cSetInfoEm10 mArmId (0x70100 = default)
constexpr std::size_t kSetInfoEm10Armed = 0xE9;   // cSetInfoEm10 mbArmed (1 in 179 of 187 surveyed descriptors)
constexpr std::size_t kSetInfoEm10Mask = 0x108;   // cSetInfoEm10 mMaskType (-1 in every surveyed descriptor)
constexpr std::size_t kSetInfoAngleY = 0x34;      // cSetInfoCoord rotation about Y, radians (forward = sin, cos)
constexpr std::size_t kSetInfoVtableApply = 0x18;
constexpr std::size_t kEnemySetGroup = 0xD50;
constexpr std::size_t kEnemySetId = 0xD54;
constexpr std::size_t kLinkUnitManager = 0x3830;
constexpr std::size_t kUnitPosition = 0x30;       // float x, y, z of any uCoord unit
constexpr std::size_t kEnemyArmId = 0x1BE4;       // uEm10 weapon id copied from the descriptor (+4: count)
constexpr std::size_t kUnitRotation = 0x40;       // quaternion x, y, z, w of any uCoord unit (MT uCoord layout)
// x, y, z scale of a uCoord unit: MT's uCoord keeps position, rotation, scale, then its local
// matrix (+0x60) and world matrix (+0xA0, read by 0x00B56650). Seen 2026-11-08 on the
// flamethrower (scripts/test_flame_hide.sh): 0.001 there hides its tank and hose; the game sets
// 1 again when it is equipped. The mod writes it only where it reads (1, 1, 1) or its own value.
constexpr std::size_t kUnitScale = 0x50;
// sCamera and its cameras (addr::kSCameraInstance)
constexpr std::size_t kSCameraViewports = 0x30;
constexpr int kSCameraViewportCount = 8;
constexpr std::size_t kViewportSize = 0x5E0;
constexpr std::size_t kViewportCamera = 0x04;
constexpr std::size_t kViewportActive = 0x10;
constexpr std::size_t kViewportRegion = 0x14;
constexpr std::size_t kCameraFar = 0x18;
constexpr std::size_t kCameraNear = 0x1C;
constexpr std::size_t kCameraAspect = 0x20;
constexpr std::size_t kCameraFov = 0x24;
constexpr std::size_t kCameraEye = 0x30;
constexpr std::size_t kCameraUp = 0x40;
constexpr std::size_t kCameraTarget = 0x50;
// uWp21, the flamethrower (addr::kFnWp21Fire)
constexpr std::size_t kWp21Firing = 0x1C10;        // uint8, set by its fire
constexpr std::size_t kWp21AttackData = 0x1C80;
constexpr std::size_t kWp21StreamEffect = 0x1C8C;  // uint32, the stream effect's id (0: none)
// The player camera (uCameraCtrl) and its zone camera (addr::kFnAreaCameraCheck): bit 0 of the
// flags is a forced zone (a script's), the zone it stands in, the zone camera's index (-1 none)
// and the one before.
// The enemy hit check's fields: the hit record (its +0x48: a hit pending), the block byte, the
// flags (bit 0x80 refuses), five cooldown floats (any over 0 refuses).
constexpr std::size_t kEnemyHitRecord = 0x129C;
constexpr std::size_t kHitRecordPending = 0x48;
constexpr std::size_t kEnemyHitBlock = 0x1E24;
constexpr std::size_t kEnemyHitFlags = 0x18DC;
constexpr std::size_t kEnemyHitCooldowns = 0x1DFC;
// The player camera's current camera (uCameraCtrl +0x1B0: its own +0xDF0 or the event one +0x2DE0)
// and the zone check's answer (+0x30F0).
constexpr std::size_t kCamCtrlCurrent = 0x1B0;
constexpr std::size_t kCamCtrlZoneAnswer = 0x30F0;
// The fader (addr::kFadeInstance, vtable 0x00F67828, built by 0x007F1B70): its state +0x20..+0x2C.
constexpr std::size_t kFaderState = 0x20;
constexpr std::size_t kCamCtrlAreaFlags = 0x30E8;
constexpr std::size_t kCamCtrlArea = 0x30EC;
constexpr std::size_t kCamCtrlZoneCamera = 0x30F4;
constexpr std::size_t kCamCtrlPrevZoneCamera = 0x30F8;
// cSetInfoEnemy reflected fields (property list FUN_00CCE070): the enemy's starting behaviour.
constexpr std::size_t kSetInfoEnemyFirst = 0x70;  // mInAir; the enemy fields run to mbForceMoveFSM (+0xD5)
constexpr std::size_t kSetInfoIsForNet = 0x84;
constexpr std::size_t kSetInfoIsFindPl = 0x85;    // bool mIsFindPL
constexpr std::size_t kSetInfoIsGoto = 0x86;
constexpr std::size_t kSetInfoSearchType = 0xB0;  // u32 mSearchType; +0xB4 mSearchForward, +0xB8 mSearchCircle (float)
constexpr std::size_t kSetInfoAreaKeepType = 0xC0;  // u32; +0xC4 mAreaKeepDist (float), +0xC8 mAreaKeepHitNo
// Application object ([addr::kGameRoot]): flags and the lock around them.
constexpr std::size_t kAppLock = 0x4;
constexpr std::size_t kAppThreadedFlag = 0x1C;
constexpr std::size_t kAppFlags = 0x10438;
// More application flags. Uroboros Wesker's StageChange (his transformation) sets 0x100000, 0x80000000
// and 0x20000000 here and 0x200 in kAppFlags as it begins (0x0092C6A0) and clears them as it ends
// (0x0092C990, which also applies his second form through 0x00921220).
constexpr std::size_t kAppEventFlags = 0x10448;
constexpr std::uint32_t kEm82StageChangeEventFlags = 0x100000u | 0x80000000u | 0x20000000u;
constexpr std::uint32_t kEm82StageChangeAppFlags = 0x200u;
constexpr std::size_t kAppScreenFlags = 0x10434;     // & kAppScreensPaused: screens' moves skip a frame
constexpr std::uint32_t kAppScreensPaused = 0x500;
// aGame: +0x348 points at a byte per player; the cockpit HUD draws its other mode (the item
// screen, hypothesis 2026-11-03) while that byte is 1 (FUN_007162D0).
constexpr std::size_t kGameItemScreen = 0x348;
constexpr std::uint32_t kAppPauseRequest = 0x800;
constexpr std::uint32_t kAppPaused = 0x1000;
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
// save data ([addr::kSaveData])
constexpr std::size_t kSaveSlot = 0x20;
constexpr std::size_t kSaveSlotStride = 0x5940;
constexpr std::size_t kSaveStage = 0x273D8;
constexpr std::size_t kGameFlowItemBox = 0x1840;  // [kGameFlow]: the open Organize/Buy screen (uIdItemBox00)
// Mercenaries match object (`this` of kFnMercUpdateCountdown)
constexpr std::size_t kMercCountdown = 0x4DC;  // float seconds remaining (authoritative)
constexpr std::size_t kMercState = 0x4D4;      // int32: 0 running, 1 time up / finished
// aGame (the game's main state machine, addr::kFnMercStartState)
constexpr std::size_t kGameStateStep = 0x05;      // uint8, the main state's step
constexpr std::uint8_t kMercStartShot = 0x0C;     // kFnMercStartState: the intro's next camera shot
constexpr std::uint8_t kMercStartShotWait = 0x0D; // ... its shot playing (a pad's press skips)
constexpr std::uint8_t kMercStartShotPause = 0x0E; // ... 0.1 s between two shots
constexpr std::uint8_t kMercStartEnd = 0x14;      // ... the intro over: fade out, then the match
// Play menu (uIdStartUp01: CONTINUAR ... CONTENIDO EXTRA)
constexpr std::size_t kMenuState = 0x14;       // uint8 state machine; 2 = waiting for input
// uIdMerceEd00: set once its option list has run (state 10 the second time); its option
// handler checks the menu input's cancel bit (8) before the fade gate.
constexpr std::size_t kResultsOptionsFlag = 0x2BC;
constexpr std::uint32_t kMenuInputCancelBit = 0x8;
// uIdMerceEd00 exit path: 0x5A requests the flow change, 0x5B waits, 0x5C closes the screen.
constexpr std::uint8_t kResultsExitState = 0x5A;
// uIdGameover00 (see addr::kFnGameoverMove).
constexpr std::uint8_t kGameoverInputState = 2;
constexpr std::uint8_t kGameoverDecidedState = 3;
constexpr std::size_t kMatchQuitStep = 0x05;  // uint8, kFnMatchQuitState's step
constexpr std::size_t kLoaderStep = 0x21;           // uint8, addr::kFnSaveFileWrite's step (0 = not begun)
constexpr std::size_t kLoaderRequestState = 0x48;   // int32, a loader request's state (bit 4 = done)
constexpr std::int32_t kLoaderRequestDone = 4;
constexpr std::size_t kGameoverCursor = 0x190;   // int32: 0 yes (retry), 1 no (quit)
constexpr std::size_t kGameoverDecided = 0x196;  // uint8
constexpr std::size_t kGameoverDelay = 0x198;    // float: frames until the exit states
constexpr std::size_t kGameWorkMercPhase = 0x67C;  // int32 Mercenaries phase (0 the intro, 1 the match; 2, 3 at its end)
constexpr std::size_t kMenuCursor = 0x48;      // int32 selected item
constexpr std::size_t kMenuItemCount = 0x190;  // int32: 7, or 8 with "Contenido extra"
constexpr std::size_t kMenuExtraContentRow = 0x198;  // uint8: the init gave it that eighth row
// game work: the Play menu's enabled rows, copied to kMenuEnabledMask every frame (bit 6 The Mercenaries)
constexpr std::size_t kGameWorkPlayMenuMask = 0x20;
// app: set at start-up when Steam says Extra Content (DLC 352000 or 352001) is owned
constexpr std::size_t kAppOwnsExtraContent = 0x10568;
constexpr std::size_t kAppOwnsExtraContent2 = 0x10567;
// sMessage (addr::kSMessageInstance): the other way the Play menu gets its eighth row
constexpr std::size_t kMessageExtraContent = 0x2F658;
constexpr std::size_t kMenuEnabledMask = 0x194; // uint32 bit per item; clear bits draw red
constexpr std::size_t kMenuBlink = 0x3D;        // uint8: selected row pulses while set
constexpr std::size_t kMenuTopMenu = 0x128;     // uIdTopMenu00* (planet and side labels)
constexpr std::size_t kFadeVtableIn = 0x2C;    // fader vtable slot 11: FadeIn(float alpha, int frames)
constexpr std::size_t kFadeVtableOut = 0x30;   // fader vtable slot 12: FadeOut(float alpha, int frames)
constexpr std::size_t kMenuModeCursor = 0x18C;  // uIdStartUp20 Solo/Duo page: int32 cursor
constexpr std::size_t kMenuCharacterIndex = 0x194;  // uIdStartUp22: int32 selected costume slot
// game work: Mercenaries setup written by those screens
constexpr std::size_t kGameWorkMercItem = 0x5C8;   // int32: 0 Iniciar, 1 Marcadores
constexpr std::size_t kGameWorkMercMode = 0x5CC;   // int32: 0 Solo, 1 Duo (online), 2 Split screen (MODE SELECT)
constexpr std::size_t kGameWorkStage = 0x5D4;      // int32 stage index (0..7)
constexpr std::size_t kGameWorkCharacter = 0x600;  // int32[4] costume slot per player
constexpr std::size_t kGameWorkMercFlag = 0x79E;   // uint8 set by the Solo/Duo page's 4th option
constexpr std::size_t kFlowStepState = 0x25;       // script ctx: 0 create, 1 wait, 2 done
constexpr std::size_t kFlowScreen = 0x18;          // script ctx: the screen it waits on
// A setup screen's result bytes (the script's Step reads them once +0x3C is set): +0x3E back
// (-1), +0x3F the other way out (-2), neither: accepted (+1).
constexpr std::size_t kScreenDone = 0x3C;
constexpr std::size_t kScreenBack = 0x3E;
// Pad singleton: decide/cancel button masks for the menu input bits.
constexpr std::size_t kPadDecideMask = 0x5B4;
constexpr std::size_t kPadCancelMask = 0x5B8;
// M10: the pad object's per-pad menu bits (addr::kFnMenuListStep's note) and its switches: no input
// at all while +0x5DC is set, and the player the keyboard belongs to (+0x614). The menu input query
// also answers nothing while the application's +0x10428 is set.
constexpr std::size_t kPadStride = 0x2C0;
constexpr std::size_t kPadTrigger = 0x198;
constexpr std::size_t kPadRepeat = 0x1A4;
constexpr std::size_t kPadsInputOff = 0x5DC;
constexpr std::size_t kPadsKeyboardPlayer = 0x614;
constexpr std::size_t kAppMenuInputOff = 0x10428;
// sMessage -> rMessage for etc\message\mes_system_<lang>
// One loaded rMessage per language and file, at +kMessageFirstTable + (language * 19 + file) * 0x3A0
// (0x007D9F50). mes_system is file 0. Languages: 0 j, 1 e, 2 g, 3 f, 4 s, 5 i, 6 r, 7 p, 8 c, 9 chT, 10 chS.
constexpr std::size_t kMessageFirstTable = 0x1B4;
constexpr std::size_t kMessageLanguageStride = 19 * 0x3A0;
constexpr int kMessageLanguages = 11;
// The language they were loaded for: [kSaveData] + [+kSaveSlotIndex] * kSaveLanguageStride + kSaveLanguage.
constexpr std::size_t kSaveSlotIndex = 0x20;
constexpr std::size_t kSaveLanguageStride = 0x8C74;
constexpr std::size_t kSaveLanguage = 0x70;
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
    {"player_add_hp", addr::kFnPlayerAddHp, "66 8B 44 24 04 56 8B F1 57 0F BF BE 64 13 00 00 66 01 86 64 13 00 00"},
    {"pickup_item", addr::kFnPickUpItem, "83 EC 38 56 8B 74 24 40 57 6A 01 8B CE E8 CE 20 37 00"},
    {"item_from_handle", addr::kFnItemFromHandle, "53 55 56 8B D9 8B 43 24 57 33 FF 85 C0 76 55 8B 6C 24 14 85"},
    {"player_dodge_prep", addr::kFnPlayerDodgePrep, "8B 44 24 04 8B 80 8C 2D 00 00 8B D0 C1 EA 08 F6"},
    // `... lea eax, [esp+0x10]; push eax; mov esi, ecx; call GetEuler; mov eax, [ebp+8]; cmp eax, 1`.
    {"char_turn_to", addr::kFnCharTurnTo, "55 8B EC 83 E4 F0 83 EC 1C 56 8D 44 24 10 50 8B F1 E8 9A 4A 93 FF 8B 45 08 83 F8 01"},
    {"resource_get", addr::kFnResourceGet, "55 8B EC 83 E4 F8 83 EC 0C 8B 45 08 53 56 57 50 8B 45 0C 8B F9 E8 76 08 00 00 80 7F"},
    // `push esi; mov esi, eax; xor bl, bl; cmp [esi+0x1C], bl ...`: the manager in EAX.
    {"resource_release", addr::kFnResourceRelease, "53 56 8B F0 32 DB 38 5E 1C 75 08 38 1D 2E 9C 24 01 74 0A 8D 46 04 50 FF 15 F8 A0 F4"},
    // `... mov edi, [esi+ebx*4+0xA88]; test edi, edi; jz; mov eax, [kResourceManager]; call release`.
    {"motion_list_set", addr::kFnMotionListSet, "53 8B 5C 24 0C 56 8B F1 57 8B BC 9E 88 0A 00 00 85 FF 74 15 A1 80 45 23 01 E8 32 A2"},
    // `push esi; mov esi, [esp+8]; cmp esi, -1; push edi; mov edi, ecx; jle; cmp esi, 4; jge;
    // cmp [edi+esi*4+0x24], 0; jz; pop edi; xor eax, eax`.
    {"player_death_event", addr::kFnPlayerDeathEvent, "51 55 8B E9 89 6C 24 04 E8 A3 E2 FF FF 8B 0D DC 27 1B 01 E8 48 90 07 00"},
    {"game_over_check", addr::kFnGameOverCheck, "51 A1 A4 40 23 01 53 55 8B D9 8B 88 2C 04 01 00 83 79 58 01 56 57 89 5C"},
    {"merc_end_event", addr::kFnMercEndEvent, "8B 54 24 04 53 55 8B E9 8B 0D A4 40 23 01 8B 81 2C 04 01 00"},
    {"player_dead_or_frozen", addr::kFnPlayerDeadOrFrozen, "56 8B F1 8B 06 8B 90 5C 01 00 00 FF D2 84 C0 75 2F 66 83 BE 64 13 00 00 01"},
    {"player_game_over", addr::kFnPlayerGameOver, "56 8B F1 8B 86 7C 2D 00 00 C1 E8 04 A8 01 74 0A 8B 16 8B 82 B4 01 00 00 FF D0 83 BE A8 2D 00 00 02"},
    {"wpslot_ui_visible", addr::kFnWpSlotUiVisible, "A1 A4 40 23 01 F7 80 3C 04 01 00 00 80 00 00 53 56 8B F1 8B 5E 30 57 8B 7E 2C 74 06"},
    {"player_partner", addr::kFnPlayerPartner, "56 8B F1 8B 86 74 2A 00 00 85 C0 74 1A 8B 0D DC 27 1B 01 50 E8 87 A4 C8"},
    {"pickup_partner_call", 0x00B65CC2, "A1 A4 40 23 01 8B 88 2C 04 01 00 83 79 58 01 74 30 6A 01 8B CD E8 F4 61 FF FF"},
    {"player_may_act", addr::kFnPlayerMayAct,
     "56 8B F1 66 83 BE 64 13 00 00 01 75 04 32 C0 5E C3 8B 06 8B 90 5C 01 00 00 FF D2 3C 01 74 EE 80 BE CC 1B 00 00 00 7F E5 A1 A4 40 23 01"},
    {"wpslot_update", addr::kFnWpSlotUpdate, "51 56 8B F1 C6 46 7E 00 A1 58 21 1B 01 8B 48 20 69 C9 40 59 00 00 8B 84 01 D8 73 02 00 83 F8 64"},
    {"cockpit_rings_check", addr::kFnCockpitRingsCheck, "56 8B F1 C6 86 C3 00 00 00 00 A1 58 21 1B 01 8B 48 20 69 C9 40 59 00 00"},
    {"player_in_death", addr::kFnPlayerInDeath, "56 8B F1 8B 06 8B 90 5C 01 00 00 FF D2 84 C0 74 04 B0 01 5E C3 66 83 BE 64 13 00 00 01"},
    {"app_request_word", addr::kFnAppRequestWord,
     "56 8B F1 80 7E 1C 00 75 09 80 3D 2E 9C 24 01 00 74 0A 8D 46 04 50 FF 15 F8 A0 F4 00 0F B6 4C 24 0C 8B 54 24 08 09 94 8E 3C 04 01 00"},
    {"player_equip_cell", addr::kFnPlayerEquipCell, "56 57 8B 7C 24 0C 83 FF 0F 8B F1 75 13 57 8D 8E A0 21 00 00 E8 97 7F 0D 00"},
    {"weapon_record", 0x00770380, "8B 44 24 04 8B C8 81 E1 00 FF 00 00 81 F9 00 01 00 00 74 03 33 C0 C3 25 FF 00 00 00 8D 04 80 C1 E0 06 05 70 51 1B 01 C3"},
    {"weapon_scope_read", 0x00B699E2, "83 B8 98 00 00 00 00 75 24 A1 58 21 1B 01"},
    // `push ebp; mov ebp, esp; and esp, -16; sub esp, 0x14; push ebx; mov ebx, [ebp+8]; test ebx, ebx;
    // push esi; push edi; mov edi, ecx; jnz`: the effect creator (the list in EBX, the manager in EDI).
    {"effect_create", addr::kFnEffectCreate, "55 8B EC 83 E4 F0 83 EC 14 53 8B 5D 08 85 DB 56 57 8B F9 75 0B"},
    // An enemy's own start of an effect: `mov ecx, [kEffectManager]; push settings; push 4; push offset; push -1`.
    {"effect_call_site", 0x00853DEF, "8B 0D 5C 9C 24 01 68 B0 3B 0E 01 6A 04 68 F0 9C 24 01 6A FF"},
    // `push esi; push edi; mov edi, ecx; cmp byte [edi+0x1C], 0; ...; mov esi, [edi+0x260]; xor eax, eax`.
    {"effect_stop", addr::kFnEffectStop,
     "56 57 8B F9 80 7F 1C 00 75 09 80 3D 2E 9C 24 01 00 74 0A 8D 47 04 50 FF 15 F8 A0 F4 00 8B B7 60 02 00 00 33"},
    // `sub esp, 0x10; push esi; push edi; mov edi, [esp+0x1C]; push edi; mov esi, ecx; call 0x00B95810;
    // mov eax, [esi+0x1C8C]; test eax, eax; je; mov ecx, [kEffectManager]; push eax; call kFnEffectStop`.
    // `push ebp; mov ebp, esp; and esp, -16; sub esp, 0x1C; movss xmm0, [0x01032584]; movss xmm1,
    // [0x01032580]; push esi; mov esi, ecx`.
    {"wp21_charge", addr::kFnWp21Charge, "55 8B EC 83 E4 F0 83 EC 1C F3 0F 10 05 84 25 03 01 F3 0F 10 0D 80 25 03 01 56 8B F1"},
    {"wp21_fire", addr::kFnWp21Fire,
     "83 EC 10 56 57 8B 7C 24 1C 57 8B F1 E8 FF 09 FF FF 8B 86 8C 1C 00 00 85 C0 74 0C 8B 0D 5C 9C 24 01 50 E8 99 C4 BC FF"},
    {"weapon_reload_levels", 0x00A34A70, "8B 44 24 04 3D 50 01 00 00 72 03 D9 EE C3 8B C8 81 E1 00 FF 00 00 81 F9 00 01 00 00 75 ED 25 FF 00 00 00 6B C0 7A 03 44 24 08 D9 04 85 64 06 1C 01 C3"},
    {"weapon_capacity_levels", 0x00C4C0AD, "0F B6 49 23 25 FF 00 00 00 6B C0 7A 03 C1 8B 04 85 A0 06 1C 01 C2 04 00"},
    {"player_add", addr::kFnPlayerAdd, "56 8B 74 24 08 83 FE FF 57 8B F9 7E 13 83 FE 04 7D 0E 83 7C B7 24 00 74 07 5F 33 C0"},
    {"item_unit_create", addr::kFnItemUnitCreate, "8B 54 24 04 56 8B 35 90 60 1F 01 39 72 04 74 19 8B 42 10 85"},
    {"unit_set_euler", addr::kFnUnitSetEuler, "55 8B EC 83 E4 F0 83 EC 74 0F 57 C0 F3 0F 10 25 34 34 0A 01"},
    {"unit_attach", addr::kFnUnitAttach, "8B 44 24 04 8B 54 24 08 89 41 18 89 51 1C C2 08 00"},
    {"wp76_fire", addr::kFnWp76Fire, "55 8B EC 83 E4 F0 81 EC 84 00 00 00 53 56 8B F1 57 8B 7E 18"},
    {"joint_position", addr::kFnJointPosition, "55 8B EC 83 E4 F0 8B 45 0C 83 EC 40 83 F8 FF 0F 8F C7 00 00 00 F3 0F 10 81 A4 00 00 00"},
    {"char_set_motion", addr::kFnCharSetMotion, "53 56 8B F1 8B 8E 74 19 00 00 85 C9 57 74 05 E8 7C 1B 40 00 8B 7C 24 10 85 FF 8B 5C"},
    // `mov edx, [kGameRoot]; mov eax, [edx+0x1042C]; cmp [eax+0x58], 5; mov ecx, [esi+0x1C]; jnz +0B;
    // push ecx; call 0x00C00770; add esp, 4; jmp +0A; push uEm14 DTI; call IsKindOf`.
    {"dead_bomb_mode_check", addr::kDeadBombModeCheck,
     "8B 15 A4 40 23 01 8B 82 2C 04 01 00 83 78 58 05 8B 4E 1C 75 0B 51 E8 57 FB FE FF 83 C4 04 EB 0A 68 D0 A9 1E 01"},
    {"segment_hit", addr::kFnSegmentHit, "55 8B EC 83 E4 F0 8B 55 08 F3 0F 10 52 08 F3 0F 5C 52 18"},
    {"change_action", addr::kFnChangeAction, "56 57 8B 7C 24 0C 81 FF 00 00 00 80 8B F1 74 5C"},
    {"player_change_action", addr::kFnPlayerChangeAction, "56 8B F1 80 BE 6C 2D 00 00 00 75 6C 8B 86 74 2A"},
    {"enemy_change_action", addr::kFnEnemyChangeAction, "C6 81 A0 1B 00 00 00 C7 44 24 14 00 00 00 00 E9"},
    {"player_action_table", addr::kFnPlayerActionTable, "B8 80 AF 13 01 C3"},
    {"em10_action_table", addr::kFnEm10ActionTable, "B8 50 59 11 01 C3"},
    {"hit_get_attacker", addr::kFnHitGetAttacker, "56 57 8B F9 8B 77 20"},
    // MercAddTime reads [kGameRoot]+kGameWork and adds to +kMercTimer.
    {"merc_add_time", addr::kFnMercAddTime, "A1 A4 40 23 01 56 8B 74 24 08 57 8B F9 8B 88 2C 04 01 00"},
    // `push ecx; push esi; mov esi, ecx; mov ecx, [sPlayer]; push 1; call lock; mov al, [esi+5];
    // movzx ecx, al; cmp ecx, 0x20; ja`: the step's switch.
    {"merc_start_state", addr::kFnMercStartState,
     "51 56 8B F1 8B 0D DC 27 1B 01 6A 01 E8 6F 6B 0C 00 8A 46 05 0F B6 C8 83 F9 20 0F 87 A7 04 00 00"},
    {"merc_timer_store", 0x007180E6, "F3 0F 11 89 80 06 00 00"},
    {"merc_update_countdown", addr::kFnMercUpdateCountdown,
     "A1 A4 40 23 01 8B 80 2C 04 01 00 57 8B F9 8B 88 74 05 00 00 3B 88 78 05 00 00"},
    // sPlayer::GetPlayer(i) returns [this + i*4 + 0x24] for i in [0,4).
    {"splayer_get_player", 0x007E6220, "8B 44 24 04 83 F8 FF 7F 05 33 C0 C2 04 00 83 F8 04 7D F6 8B 44 81 24"},
    // uEnemy::ApplyDamage calls the SubHp virtual (+0x114) that the base hook covers.
    {"enemy_apply_damage_subhp", 0x009DEB02, "8B 06 8B 90 14 01 00 00 57 8B CE"},
    // Play menu: move entry, decide-mask test, cursor/count use, label table.
    {"playmenu_move", addr::kFnPlayMenuMove, "53 56 57 6A FF 6A FF 8B F1 E8 52 9D FA FF 8B F8"},
    {"smessage_load", 0x007D9F50, "81 EC 14 01 00 00 33 C0 38 84 24 18 01 00 00 89 0C 24 0F 95 C0 57 89 81 58 F6 02 00 8B 0D 58 21 1B 01 8B 51 20 69 D2 74 8C 00 00"},
    {"playmenu_extra_content", 0x00A395EB, "A1 F0 26 1B 01 39 88 58 F6 02 00 75 0D A1 A4 40 23 01 38 88 68 05 01 00"},
    {"playmenu_decide_mask", 0x00A39745, "8B 0D 40 9C 24 01 85 B9 B4 05 00 00"},
    {"playmenu_cursor_count", 0x00A397A2, "8B 86 90 01 00 00 8B 4E 48 50 51 8B"},
    {"playmenu_label_table", 0x01131D90, "A9 03 AA 03 AB 03 AD 03 AC 03 B0 03 AF 03 AE 03 A8 03"},
    {"menu_input", addr::kFnMenuInput, "A1 A4 40 23 01 80 B8 28 04 01 00 00 74 05 33 C0 C2 08 00"},
    // M10: one player's own menu input (addr::kFnMenuListStep's note).
    {"pad_trigger_bits", 0x00799820, "80 B9 DC 05 00 00 00 74 05 33 C0 C2 04 00 8B 44 24 04 69 C0 C0 02 00 00 8B 84 08 98 01 00 00"},
    {"pad_repeat_bits", 0x007998B0, "80 B9 DC 05 00 00 00 74 05 33 C0 C2 04 00 8B 44 24 04 69 C0 C0 02 00 00 8B 84 08 A4 01 00 00"},
    {"keyboard_trigger", addr::kFnKeyboardTrigger, "8A 81 DC 05 00 00 84 C0 8B 15 74 9C 24 01 53 56 75 23 80 B9 F0 05 00 00 01"},
    {"keyboard_repeat", addr::kFnKeyboardRepeat, "8A 81 DC 05 00 00 84 C0 8B 15 74 9C 24 01 53 56 75 23 80 B9 F0 05 00 00 01"},
    {"menu_list_step", addr::kFnMenuListStep, "A1 78 1C 12 01 8B 54 24 08 56 8B 74 24 08 85 C6 57 74 07 B9 01 00 00 00"},
    {"menu_navigate_keyboard", 0x009E4BE7, "8B 0D 40 9C 24 01 8B 81 D8 05 00 00 50 E8 B7 4C DB FF 8B 0D 40 9C 24 01 6A 01 8B F8 E8 28 69 DB FF"},
    {"menu_direction_masks", addr::kMenuMaskRight, "20 00 02 00 80 00 08 00 40 00 04 00 10 00 01 00"},
    // A gun that never runs out (off::kSlotInfiniteBit): the check's level test, and the shot's call of it.
    {"weapon_infinite_check", 0x00C4C233, "80 7F 23 0D 76 07 5F B0 01 5E C2 04"},
    // Boss tuning (2026-11-22): Uroboros Wesker's second form switch and his picker's rage checks; the
    // boss Jill's three table reads and the tables as the game ships them.
    {"em82_set_pose", addr::kFnEm82SetPose, "55 8B EC 83 E4 F0 51 8A 45 08 53 56 BB 01 00 00 00 38 5D 0C 8B F1 57 88 86 85 1F 00 00"},
    {"em82_pick_rage", 0x009C88FB, "8B 8A B4 1B 00 00 C1 E9 0C 83 C4 04 F6 C1 01"},
    {"em82_rage_timer", 0x009C8B3E, "8B 88 B4 1B 00 00 8B D1 C1 EA 0C F6 C2 01 74 1E 0F 57 C0 0F 2F 80 90 1F 00 00"},
    {"em84_chance_read", 0x009D041F, "D9 04 B5 64 0A 12 01"},
    {"em84_dieblow_gameover", 0x00947032, "8B 0D A4 40 23 01 68 00 00 00 08 E8 DE F0 DD FF"},
    {"em84_cooldown_a_read", 0x009CE3D1, "D9 04 85 B8 09 12 01"},
    {"em84_cooldown_b_read", 0x009D04F7, "D9 04 85 A8 0A 12 01"},
    {"em84_chance_data", addr::kJillAttackChance, "00 00 00 00 00 00 00 00 9A 99 99 3E 9A 99 99 3E 66 66 66 3F 00 00 80 3F"},
    {"em84_cooldown_a_data", addr::kJillFireCooldownA, "00 00 20 40 CD CC 0C 40 66 66 E6 3F 00 00 C0 3F 9A 99 99 3F 66 66 66 3F"},
    {"em84_cooldown_b_data", addr::kJillFireCooldownB, "00 00 20 40 CD CC 0C 40 66 66 E6 3F 00 00 C0 3F 9A 99 99 3F 66 66 66 3F"},
    {"shot_infinite_check", 0x00C8A188, "E8 73 20 FC FF 84 C0 0F 85"},
    // The HUD's text names (addr::kInventoryNameTable): the two reads of the tables.
    {"inventory_name_read", 0x00A70529, "8B 2C 85 E8 41 13 01 C1 E1 18 8D 7C 24 50"},
    {"cockpit_name_read", 0x009F34D6, "8B 04 85 60 34 12 01 8D 54 24 7C 83 C4 0C"},
    {"smessage_get_dti", 0x007E06E0, "B8 F4 26 1B 01 C3"},
    {"menu_row_glow", addr::kFnMenuRowGlow, "83 EC 20 56 8B F1 83 7E 78 09 57 0F 85 BC 00 00 00"},
    {"layout_name", addr::kFnLayoutName, "53 8B 5C 24 08 85 DB 56 8B F1 74 5C"},
    {"layout_element", addr::kFnLayoutElement, "8B 44 24 08 83 F8 5C 56 57 73 1E 8B 0D 34 23 1B 01"},
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
    {"menu_sound_move", addr::kFnMenuSoundMove, "51 8B 0D 0C 1D 1B 01 6A 42 E8"},
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
    {"merce_results_move", addr::kFnMerceResultsMove, "53 56 57 6A FF 6A FF 8B F1 E8 82 2B FC FF 8B 0D"},
    {"gameover_move", addr::kFnGameoverMove, "53 55 56 8B F1 8B 0D 78 45 23 01 57 E8 BF 80 D6 FF"},
    {"gameover_move_slot", 0x00FA54C0, "40 0B A0 00"},
    {"gameover_decide", addr::kFnGameoverDecide, "83 B9 90 01 00 00 00 56 57 8B 3D A4 40 23 01 8D 77 1C 75 4C"},
    // The move's decide: `movss xmm0, [delay]; mov ecx, esi; mov [esi+0x196], bl;
    // movss [esi+0x198], xmm0; call Decide`.
    {"gameover_decide_block", 0x00A00D66, "F3 0F 10 05 FC 02 FF 00 8B CE 88 9E 96 01 00 00 F3 0F 11 86 98 01 00 00 E8 CD F7 FF FF"},
    {"itembox_panel_update", addr::kFnItemBoxPanelUpdate, "57 8B F9 E8 68 40 FF FF 85 C0 0F 85 99 01 00 00"},
    {"itembox_panel_tab", addr::kFnItemBoxPanelTab, "8A 44 24 08 56 8B 74 24 08 88 46 18"},
    // The picture lookup's scan of the item table (`movzx edi, word [eax + 0x01128C70]`).
    {"itembox_picture_name", addr::kFnItemBoxPictureName, "8B 54 24 08 56 33 C9 57 BE 94 00 00 00 33 C0 90 0F B7 B8 70 8C 12 01"},
    {"match_quit_state", addr::kFnMatchQuitState, "57 8B F9 8A 47 05 0F B6 C8 83 F9 0C 0F 87 A1 00 00 00"},
    {"save_file_write", addr::kFnSaveFileWrite, "81 EC 04 02 00 00 53 55 56 8B F1 0F B6 46 21 32 DB BD 08 00 00 00 3B C5"},
    {"app_request", addr::kFnAppRequest, "56 8B F1 80 7E 1C 00 75 09 80 3D 2E 9C 24 01 00 74 0A 8D 46 04 50 FF 15 F8 A0 F4 00"},
    {"pause_move", addr::kFnPauseMove, "A1 A4 40 23 01 F7 80 34 04 01 00 00 05 00 00 56"},
    {"inventory_slot_clear", addr::kFnInventorySlotClear, "8B 44 24 04 53 56 57 50 8B F1 E8 B1 EE FF FF 6A 2C 33 DB 8D"},
    {"enemy_create", addr::kFnEnemyCreate, "8B 15 50 4F 1F 01 56 8B F1 8B 4C 24 08 39 51 04 74 20 8B 41"},
    {"enemy_set_spawn", addr::kFnEnemySetSpawn, "56 8B F1 8B 06 8B 50 40 FF D2 3C 01 75 06 33 C0 5E C2 0C 00"},
    {"class_lookup", addr::kFnClassLookup, "56 8B 74 24 08 85 F6 75 04 33 C0 5E C3 53 56 E8 0C 73 01 00"},
    {"link_units", addr::kFnLinkUnits, "53 57 8B D9 33 FF 39 BB E8 05 00 00 76 65 55 8B 6C 24 10 56"},
    {"shop_create", addr::kFnShopCreate, "8B 0D 94 F2 1A 01 8B 01 8B 50 14 56 6A 10 68 C4 1D 00 00 FF"},
    {"unit_register", addr::kFnUnitRegister, "56 8B 74 24 0C 85 F6 57 8B F9 75 07 5F 32 C0 5E C2 08 00 80"},
    {"itembox_create", addr::kFnItemBoxCreate, "8B 0D 94 F2 1A 01 8B 01 8B 50 14 6A 10 68 88 5B 00 00 FF D2"},
    {"merc_add_countdown", addr::kFnMercAddCountdown, "A1 A4 40 23 01 56 8B F1 8B 88 2C 04 01 00 8B 41 58 85 C0 0F"},
    {"time_bonus_action", addr::kFnTimeBonusAction, "55 8B EC 83 E4 F0 83 EC 24 53 56 57 8B F1 E8 6D FC FF FF 83"},
    // Countdown update: `cmp [eax+0x58C], edx; jl` is the kill cap (150, or 400) ending the match.
    {"merc_kill_cap", 0x007178FD, "39 90 8C 05 00 00 7C 0A C7 87 D4 04 00 00 01 00"},
    {"cockpit_draw_timer", addr::kFnCockpitDrawTimer, "51 A1 A4 40 23 01 F7 80 38 04 01 00 00 04 00 70 57 8B F9 0F 85"},
    {"cockpit_move_slot", 0x00F9E498, "C0 8F 9F 00"},
    // The timer draw's digit tables: templates 0..9 = cockpit sprites 276..285; targets
    // (hundredths, seconds, minutes, units first) = 291..286.
    {"cockpit_digit_templates", 0x01122BF8, "14 01 00 00 15 01 00 00 16 01 00 00 17 01 00 00 18 01 00 00 19 01 00 00 1A 01 00 00 1B 01 00 00 1C 01 00 00 1D 01 00 00"},
    {"cockpit_digit_targets", 0x01122C20, "23 01 00 00 22 01 00 00 21 01 00 00 20 01 00 00 1F 01 00 00 1E 01 00 00"},
    {"merce_hud_draw", addr::kFnMerceHudDraw,
     "51 53 55 56 8B E9 57 8D 5D 44 C7 44 24 10 02 00 00 00 8B 03 8B 0D 34 23 1B 01 50 E8"},
    {"layout_page_elements", addr::kFnLayoutPageElements, "8B 44 24 04 83 F8 5C 73 19 8B 44 81 20 85 C0 74 11 8B 40 08 85 C0 74 0A"},
    {"layout_set_element_draw", addr::kFnLayoutSetElementDraw, "8B 44 24 08 83 F8 5C 73 38 56 8B 74 81 20 85 F6 74 2E 8B 46 08 85 C0 74"},
    {"load_list_commit", addr::kFnLoadListCommit, "51 53 55 57 8B F9 8B 87 D0 07 02 00 8D 04 80 33 ED 33 C9 8D 04 87 33 DB"},
    {"load_class_archive", addr::kFnLoadClassArchive, "8B 44 24 04 85 C0 74 21 8B 40 20 8B 54 24 08 52 0F B6 D0 52 8B D0 C1 EA"},
    {"load_request", addr::kFnLoadRequest, "53 8B 5C 24 08 85 DB 57 8B F9 0F 84 99 00 00 00 83 BF D0 07 02 00 04 75"},
    {"load_unit_archive", addr::kFnLoadUnitArchive, "81 EC 08 01 00 00 8B 84 24 0C 01 00 00 0F B6 94 24 14 01 00 00 53 8B 9C"},
    // The Mercenaries set loader queues each record's class on the loader instance:
    // `mov ecx, [0x011B26C4]; push ebx; push 0; push esi; call LoadClassArchive`.
    {"loader_instance_use", 0x0072AB6D, "8B 0D C4 26 1B 01 53 6A 00 56 E8"},
    {"obj_create", addr::kFnObjCreate, "8B 44 24 10 8B 54 24 08 56 57 50 8B 44 24 10 8B F9 8B 4C 24 18 51 52 50"},
    {"obj_set_spawn", addr::kFnObjSetSpawn, "83 EC 6C 57 8B F9 8B 07 8B 50 40 FF D2 3C 01 75 09 33 C0 5F 83 C4 6C C2"},
    {"set_record_count", addr::kFnSetRecordCount, "8A 44 24 04 0F B6 D0 3B 51 60 73 1F 8B 49 6C 0F BE C0 83 3C"},
    {"set_record", addr::kFnSetRecord, "8A 44 24 04 0F B6 D0 3B 51 60 56 73 21 8B 49 6C 0F BE C0 83 3C 81 00 8D"},
    {"stage_layouts", addr::kFnStageLayouts, "83 EC 14 53 89 4C 24 08 8B 4C 24 1C 33 DB 3B CB 0F 84 59 03 00 00 33 C0"},
    {"register_layout", addr::kFnRegisterLayout, "81 EC 3C 01 00 00 53 55 8B AC 24 48 01 00 00 56 57 8D 5D 04 6A 5F 53 BE"},
    // sObjModel vtable slots 11..13: Create, the shared 0x007F3A80, the set spawn.
    {"obj_model_vtable", 0x00F6526C, "10 24 7E 00 80 3A 7F 00 80 24 7E 00"},
    {"item_create", addr::kFnItemCreate, "8B 44 24 10 8B 54 24 08 56 57 50 8B 44 24 10 8B F9 8B 4C 24 18 51 52 50"},
    {"item_set_spawn", addr::kFnItemSetSpawn, "56 8B F1 8B 06 8B 50 40 FF D2 3C 01 75 06 33 C0 5E C2 08 00 80 7C 24 08"},
    {"item_vtable", 0x00F635A4, "60 E7 7B 00 80 3A 7F 00 80 0A 7C 00"},
    {"hourglass_ctor", addr::kFnHourGlassCtor, "56 8B F1 E8 C8 B2 FA FF 33 C0 88 86 70 19 00 00 89 86 58 13 00 00 C7 06"},
    {"merc_match_setup", addr::kFnMercMatchSetup, "83 EC 08 A1 A4 40 23 01 8B 80 2C 04 01 00 8B 88 DC 05 00 00 8B 90 D4 05"},
    // `movzx ebx, word [edx*2 + 0x010CB9B0]`: the stage choice's stage number.
    {"merc_stage_table_use", 0x00BF506B, "0F B7 1C 55 B0 B9 0C 01"},
    {"game_fatal_error", addr::kFnGameFatalError, "8B 4C 24 08 81 EC 00 04 00 00 8D 84 24 0C 04 00 00 50 51 8D"},
    {"class_by_id", addr::kFnClassById, "8B 4C 24 04 8B C1 C1 E8 04 25 FF 0F 00 00 8B 04 85 A0 86 23 01 85 C0 74"},
    // The element count the clear loop reads: `mov eax, [ecx+0xC]; ret`.
    {"layout_element_count", 0x00CDADA0, "8B 41 0C C3"},
    {"flow_step20", addr::kFnFlowStep20, "56 8B F1 0F BE 46 25 83 E8 00 74 57 83 E8 01 74"},
    {"flow_merc_setup", addr::kFnFlowMercSetup, "56 8B F1 0F BE 46 24 83 F8 15 0F 87 AE 01 00 00"},
    {"flow_split_setup", addr::kFnFlowSplitSetup, "56 8B F1 0F BE 46 24 83 F8 15 0F 87 48 02 00 00"},
    {"inventory_add_to_stacks", addr::kFnInventoryAddToStacks, "83 EC 64 53 8B 5C 24 6C 55 56 57 33 FF 33 C0 89"},
    {"inventory_add_ammo", addr::kFnInventoryAddAmmo, "53 8B 5C 24 0C 55 8B 6C 24 0C 56 57 8B F9 33 F6"},
    {"inventory_create", addr::kFnInventoryCreate, "8B 44 24 48 8B 54 24 44 50 8B 44 24 44 52 8B 54"},
    {"game_notify_item", addr::kFnGameNotifyItem, "8B 44 24 04 8B 54 24 08 89 81 F8 00 00 00 89 91"},
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
    // Area cameras: the zone check (`cmp [0x011E9F44], 0`, `test byte [edi+0x30E8], 1`), its "no
    // zone" end (`+0x30F8 = +0x30F4`, `+0x30F4 = -1`, `push 1; push eax; call 0x0083D530`,
    // `+0x30EC = esi`), the forced zone and the camera set; rAHCamera's getDTI.
    {"area_camera_check", addr::kFnAreaCameraCheck,
     "55 8B EC 83 E4 F0 83 EC 44 83 3D 44 9F 1E 01 00 53 56 57 8B F9 89 7C 24 14 0F 84 1E 03 00 00 F6 87 E8 30 00 00 01"},
    {"area_camera_none", 0x00841B29,
     "8B 8F F4 30 00 00 89 8F F8 30 00 00 74 0E 8B 56 08 8B 42 04 89 87 F4 30 00 00 EB 0A C7 87 F4 30 00 00 FF FF FF FF "
     "8B 87 F4 30 00 00 3B C8 74 0A 6A 01 50 8B CF E8 CD B9 FF FF 85 F6 89 B7 EC 30 00 00"},
    {"area_camera_force", addr::kFnAreaCameraForce, "56 57 8B 3D 44 9F 1E 01 85 FF 8B F1 75 07 5F 32 C0 5E C2 04 00"},
    {"area_camera_set", addr::kFnAreaCameraSet, "55 8B EC 83 E4 F0 81 EC D4 00 00 00 53 56 8B 75 08 83 FE FF 8B D9 57"},
    {"rahcamera_get_dti", 0x007F71C0, "B8 4C 89 1E 01 C3"},
    // The enemy hit check: `mov ebp, [esp+8]` (the part), `cmovz eax, esi`, `mov edi, [eax+0x129C]`.
    // Uroboros Wesker's "may this hit hurt": `cmp ax, 0x3C`, `mov ecx, [esi+0x1F50]`; his getDTI.
    {"em82_may_hurt", addr::kFnEm82MayHurt, "8B 44 24 04 53 33 DB 66 3D 3C 00 56 8B F1 72 38 8B 8E 50 1F 00 00 83 C0 C4 0F B7 C0"},
    {"uem82_get_dti", 0x0092F860, "B8 60 12 1F 01 C3"},
    // Uroboros Mkono's update (its refill: `movzx ecx, word [esi+0x1366]`, `call [eax+0x10C]`), the
    // Uroboros' update and Mkono's getDTI.
    {"em41_update", addr::kFnEm41Update, "56 8B F1 C6 86 18 21 00 00 00 E8 61 82 FF FF 0F B7 8E 66 13 00 00 8B 06 8B 90 0C 01 00 00 51 8B"},
    {"em40_update", addr::kFnEm40Update, "55 8B EC 83 E4 F0 83 EC 24 53 56 8B F1 8B 86 A4 1B 00 00 33"},
    {"uem41_get_dti", 0x008BA110, "B8 38 D4 1E 01 C3"},
    {"em40_can_dive", addr::kFnEm40CanDive, "56 8B F1 80 BE C0 20 00 00 00 74 04 32 C0 5E C3 53 57 E8 29 F7 FF FF 8B F8 85 FF 0F 84 1C 01 00"},
    {"em40_ctor_story_wait", 0x008AE09C, "C6 86 C0 20 00 00 01 88 9E C1 20 00 00"},
    {"em40_intel_release", 0x0099C39F, "8B 86 80 02 00 00 88 98 C0 20 00 00 C6 80 E9 1F 00 00 01"},
    {"em40_part_life_apply", 0x008AE1E0, "56 8B 74 24 08 85 F6 0F 84 B5 00 00 00 66 8B 86 E0 00 00 00 66 05 C8 00 66 85 C0 0F B7 D0 66 89 81 74 1F 00 00"},
    // s308's setup: `sub esp, 0x14; push ebp; mov ebp, ecx; call 0x0072D240`, `test [eax+0x10434], 0x500`;
    // its read of the flamethrower's fuel (`mov edx, [ebp+0x664]; mov eax, [edx+0x1AF8]`); the
    // model lookup; the base setup; aS308's getDTI.
    {"s308_setup", addr::kFnS308Setup, "83 EC 14 55 8B E9 E8 85 02 FE FF A1 A4 40 23 01 F7 80 34 04 01 00 00 05"},
    {"s308_fuel_read", 0x0074D124, "8B 95 64 06 00 00 8B 82 F8 1A 00 00"},
    {"find_model", addr::kFnFindModel, "51 53 55 56 8B D9 8B 6B 24 57 33 FF 85 ED 76 5F 8B 43 30 83 3C B8 00"},
    {"scene_base_setup", addr::kFnSceneBaseSetup, "83 EC 08 53 55 89 4C 24 0C 8B 0D 64 9C 24 01 56"},
    {"as308_get_dti", 0x0074DC70, "B8 F4 0D 1B 01 C3"},
    {"enemy_hit_check", addr::kFnEnemyHitCheck, "55 8B 6C 24 08 56 85 ED 8B F1 8B C5 0F 44 C6 57 8B B8 9C 12 00 00 85 FF"},
    // The fader's constructor: `mov [esi], 0x00F67828; mov [0x011E8804], esi; [esi+0x20..0x2C] = 0`.
    {"fader_ctor", 0x007F1B78, "33 C0 C7 06 28 78 F6 00 89 35 04 88 1E 01 89 46 20 89 46 24 89 46 28 89 46 2C"},
};

}  // namespace re5
