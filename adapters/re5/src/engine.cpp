#include "engine.h"

#include <windows.h>

#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "memory.h"
#include "re5_layout.h"

namespace rmre5::engine {
namespace {

Verified g_caps;

bool SignatureOk(const char* name) {
    for (const auto& sig : re5::kSignatures) {
        if (std::strcmp(sig.name, name) == 0) {
            return BytesMatch(sig.va, sig.bytes);
        }
    }
    return false;
}

std::uintptr_t GameWorkObject() {
    const auto root = ReadOr<std::uintptr_t>(re5::addr::kGameRoot, 0);
    if (!root) {
        return 0;
    }
    return ReadOr<std::uintptr_t>(root + re5::off::kGameWork, 0);
}

}  // namespace

const Verified& Caps() { return g_caps; }

bool VerifySignatures(unsigned timeoutMs) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(re5::kImageBase);
    IMAGE_NT_HEADERS32 nt{};
    if (!SafeRead(re5::kImageBase + dos->e_lfanew, nt)) {
        RM_LOGE("cannot read PE header of host module");
        return false;
    }
    RM_LOGI("host PE TimeDateStamp=0x%08X SizeOfImage=0x%08X", nt.FileHeader.TimeDateStamp,
            nt.OptionalHeader.SizeOfImage);
    g_caps.buildIdentity = nt.FileHeader.TimeDateStamp == re5::kExpectedTimeDateStamp &&
                           nt.OptionalHeader.SizeOfImage == re5::kExpectedSizeOfImage;
    if (!g_caps.buildIdentity) {
        RM_LOGE("unsupported re5dx9.exe build; all engine capabilities disabled");
        return false;
    }

    // The packer decompresses the code section in chunks after imports (and
    // this DLL) load. Unpacked-but-not-yet-written pages read as zero, so
    // wait until no signature site is all zero before comparing anything.
    auto allSitesWritten = [] {
        for (const auto& sig : re5::kSignatures) {
            unsigned char head[4]{};
            if (!SafeCopy(head, sig.va, sizeof(head)) || (head[0] | head[1] | head[2] | head[3]) == 0) {
                return false;
            }
        }
        return true;
    };
    const DWORD start = GetTickCount();
    while (!allSitesWritten()) {
        if (GetTickCount() - start > timeoutMs) {
            RM_LOGE("code was not unpacked within %u ms", timeoutMs);
            break;
        }
        Sleep(50);
    }
    Sleep(100);  // let the chunk containing the last site finish
    RM_LOGI("code unpacked after %lu ms", GetTickCount() - start);

    bool all = true;
    for (const auto& sig : re5::kSignatures) {
        std::string actual;
        const bool ok = BytesMatch(sig.va, sig.bytes, &actual);
        all &= ok;
        if (ok) {
            RM_LOGI("signature %-26s 0x%08X OK", sig.name, static_cast<unsigned>(sig.va));
        } else {
            RM_LOGE("signature %-26s 0x%08X MISMATCH actual=%s", sig.name, static_cast<unsigned>(sig.va),
                    actual.c_str());
        }
    }
    g_caps.players = SignatureOk("splayer_get_player") && SignatureOk("splayer_get_dti") &&
                     SignatureOk("uplayer_get_dti") && SignatureOk("player_sub_hp");
    g_caps.enemies = SignatureOk("uenemy_get_dti") && SignatureOk("character_sub_hp");
    g_caps.enemyOnHit = SignatureOk("enemy_on_hit") && SignatureOk("enemy_apply_damage_subhp");
    g_caps.characterSubHp = SignatureOk("character_sub_hp") && SignatureOk("ucharacter_get_dti");
    g_caps.playerSubHp = SignatureOk("player_sub_hp");
    g_caps.playerAddHp = SignatureOk("player_add_hp");
    g_caps.pickupItem = SignatureOk("pickup_item") && SignatureOk("item_from_handle");
    g_caps.collision = SignatureOk("segment_hit");
    g_caps.dodge = SignatureOk("player_dodge_prep");
    g_caps.turn = SignatureOk("char_turn_to");
    g_caps.actions = g_caps.characterSubHp && SignatureOk("change_action") && SignatureOk("player_change_action") &&
                     SignatureOk("enemy_change_action") && SignatureOk("player_action_table") &&
                     SignatureOk("em10_action_table");
    g_caps.hitAttacker = SignatureOk("hit_get_attacker");
    g_caps.gameWork = SignatureOk("merc_add_time") && SignatureOk("merc_timer_store");
    g_caps.mercCountdown = g_caps.gameWork && SignatureOk("merc_update_countdown");
    g_caps.playMenu = SignatureOk("playmenu_move") && SignatureOk("playmenu_decide_mask") &&
                      SignatureOk("playmenu_cursor_count") && SignatureOk("playmenu_label_table") &&
                      SignatureOk("menu_input") && SignatureOk("smessage_get_dti") &&
                      SignatureOk("playmenu_draw") && SignatureOk("playmenu_row_y") &&
                      SignatureOk("playmenu_label_call") && SignatureOk("menu_draw_label") &&
                      SignatureOk("menu_draw_help") && SignatureOk("playmenu_help_call") &&
                      SignatureOk("menu_navigate") && SignatureOk("menu_row_color") &&
                      SignatureOk("menu_sound_decide") && SignatureOk("menu_sound_cancel") &&
                      SignatureOk("menu_sound_deny") && SignatureOk("menu_ball_index") &&
                      SignatureOk("playmenu_cancel_mask");
    g_caps.setupScreens = g_caps.gameWork && SignatureOk("menu_input") && SignatureOk("startup20_move") &&
                          SignatureOk("startup21_move") && SignatureOk("startup22_move") &&
                          SignatureOk("startup21_stage_write") && SignatureOk("startup22_character_write");
    g_caps.fader = SignatureOk("startup22_fade_out");
    g_caps.inventory = g_caps.players && SignatureOk("inventory_add_to_stacks") && SignatureOk("inventory_add_ammo") &&
                       SignatureOk("inventory_create") && SignatureOk("player_pickup_inventory") &&
                       SignatureOk("game_notify_item");
    g_caps.flowScript = g_caps.gameWork && SignatureOk("flow_step") && SignatureOk("flow_step20") &&
                        SignatureOk("flow_merc_setup") && SignatureOk("startup21_stage_write") &&
                        SignatureOk("startup22_character_write");
    g_caps.resultsScreen = g_caps.flowScript && SignatureOk("merce_results_move");
    g_caps.gameoverScreen = g_caps.resultsScreen && SignatureOk("gameover_move") && SignatureOk("gameover_move_slot") &&
                            SignatureOk("gameover_decide") && SignatureOk("gameover_decide_block");
    g_caps.pauseScreen = g_caps.playMenu && SignatureOk("pause_move");
    g_caps.slotClear = g_caps.inventory && SignatureOk("inventory_slot_clear");
    g_caps.gameShop = SignatureOk("shop_create") && SignatureOk("unit_register");
    g_caps.enemySpawn = g_caps.enemies && SignatureOk("enemy_create") && SignatureOk("enemy_set_spawn") &&
                        SignatureOk("class_lookup") && SignatureOk("link_units");
    g_caps.timeBonus = g_caps.mercCountdown && SignatureOk("merc_add_countdown") && SignatureOk("time_bonus_action");
    g_caps.fatalHandler = SignatureOk("game_fatal_error");
    g_caps.itemBox = SignatureOk("itembox_create") && SignatureOk("unit_register");
    g_caps.matchSetup = g_caps.gameWork && SignatureOk("merc_match_setup") && SignatureOk("merc_stage_table_use");
    g_caps.worldObjects = SignatureOk("obj_create") && SignatureOk("obj_set_spawn") && SignatureOk("set_record") && SignatureOk("set_record_count") &&
                          SignatureOk("obj_model_vtable") && SignatureOk("class_lookup") &&
                          SignatureOk("item_create") && SignatureOk("item_set_spawn") && SignatureOk("item_vtable") &&
                          SignatureOk("hourglass_ctor");
    g_caps.resources = g_caps.gameWork && SignatureOk("load_list_commit") && SignatureOk("load_class_archive") &&
                       SignatureOk("load_unit_archive") && SignatureOk("load_request") && SignatureOk("loader_instance_use") && SignatureOk("class_lookup") &&
                       SignatureOk("class_by_id");
    g_caps.mercHud = g_caps.mercCountdown && SignatureOk("merc_kill_cap") && SignatureOk("cockpit_draw_timer") &&
                     SignatureOk("cockpit_move_slot") && SignatureOk("merce_hud_draw") &&
                     SignatureOk("layout_page_elements") && SignatureOk("layout_set_element_draw") &&
                     SignatureOk("layout_element_count") && SignatureOk("cockpit_digit_templates") &&
                     SignatureOk("cockpit_digit_targets");
    // After gameWork and itemBox: computed before them, these three were always off (the pause
    // menu's quit wrote the game's save, the Organize screen kept Buy and Upgrade, user playtest
    // 2026-11-03).
    g_caps.matchQuit = g_caps.gameWork && SignatureOk("match_quit_state");
    g_caps.itemBoxGuard = g_caps.itemBox && SignatureOk("itembox_panel_update") && SignatureOk("itembox_panel_tab");
    g_caps.itemBoxPictures = g_caps.itemBox && SignatureOk("itembox_picture_name");
    // The motion list type is data: its name is checked too.
    const auto motionListName = ReadOr<std::uintptr_t>(re5::addr::kDtiMotionList + 4, 0);
    g_caps.motions = g_caps.players && SignatureOk("resource_get") && SignatureOk("resource_release") &&
                     SignatureOk("motion_list_set") && SignatureOk("char_set_motion") && motionListName &&
                     ReadCString(motionListName, 16) == "rMotionList";
    return all;
}

std::uintptr_t DtiOf(std::uintptr_t object) {
    const auto vtable = ReadOr<std::uintptr_t>(object, 0);
    const auto getDti = ReadOr<std::uintptr_t>(vtable + re5::off::kVtableGetDti, 0);
    // getDTI is a `mov eax, imm32; ret` stub on every class seen so far; read
    // the immediate instead of calling into the game.
    unsigned char stub[6]{};
    if (!getDti || !SafeCopy(stub, getDti, sizeof(stub)) || stub[0] != 0xB8 || stub[5] != 0xC3) {
        return 0;
    }
    std::uintptr_t dti = 0;
    std::memcpy(&dti, stub + 1, sizeof(dti));
    return dti;
}

bool IsKindOf(std::uintptr_t object, std::uintptr_t dti) {
    auto current = DtiOf(object);
    for (int depth = 0; current && depth < 24; ++depth) {
        if (current == dti) {
            return true;
        }
        current = ReadOr<std::uintptr_t>(current + re5::off::kDtiParent, 0);
    }
    return false;
}

std::string DtiName(std::uintptr_t dti) {
    return dti ? ReadCString(ReadOr<std::uintptr_t>(dti + re5::off::kDtiName, 0)) : std::string{};
}

std::string ClassName(std::uintptr_t object) { return DtiName(DtiOf(object)); }

bool InMeleeAction(std::uintptr_t player) {
    const auto action = player ? ReadOr<std::uintptr_t>(player + re5::off::kPlayerAction, 0) : 0;
    const std::uintptr_t dti = action ? DtiOf(action) : 0;
    if (!dti) return false;
    // One name read per action class.
    static std::mutex mutex;
    static std::map<std::uintptr_t, bool> melee;
    std::lock_guard lock(mutex);
    const auto it = melee.find(dti);
    if (it != melee.end()) return it->second;
    const std::string name = DtiName(dti);
    const bool hit = name.rfind("cPlAction_pl_act_attack", 0) == 0 || name == "cPlAction_pl_act_knife_attack";
    melee.emplace(dti, hit);
    return hit;
}

std::optional<CharacterHp> ReadHp(std::uintptr_t character) {
    std::int16_t hp = 0;
    std::int16_t max = 0;
    if (!SafeRead(character + re5::off::kHp, hp) || !SafeRead(character + re5::off::kMaxHp, max) || max <= 0) {
        return std::nullopt;
    }
    return CharacterHp{hp, max};
}

bool WriteHp(std::uintptr_t character, int hp) {
    const auto current = ReadHp(character);
    if (!current) {
        return false;
    }
    const auto clamped = static_cast<std::int16_t>(hp < 0 ? 0 : (hp > current->max ? current->max : hp));
    return SafeStore(character + re5::off::kHp, clamped);
}

bool SegmentResult::Blocked() const { return (flags & re5::off::kSegmentBlocked) != 0; }

namespace {

// The query runs on the collision singleton the caller leaves in ECX (the game's callers load
// [kCollision] just before; the inner query 0x004B95A0 stores ECX as its world): a thiscall.
using SegmentHitFn = unsigned(__thiscall*)(std::uintptr_t collision, const float* seg, float* hit, float* out2, void* out3,
                                           int a5, std::uint32_t mask, int a7);

bool CallSegmentHit(std::uintptr_t collision, const float* seg, float* hit, float* out2, void* out3, unsigned& flags) {
    __try {
        flags = reinterpret_cast<SegmentHitFn>(re5::addr::kFnSegmentHit)(collision, seg, hit, out2, out3, 1,
                                                                        re5::off::kCollisionMask, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

using ChangeActionFn = bool(__thiscall*)(std::uintptr_t self, int id, int a2, std::uintptr_t requester, int priority, int a5);

bool CallChangeAction(std::uintptr_t fn, std::uintptr_t self, int id, bool& started) {
    __try {
        started = reinterpret_cast<ChangeActionFn>(fn)(self, id, 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool Finite(float v) { return v == v && v > -1.0e9f && v < 1.0e9f; }

}  // namespace

std::optional<SegmentResult> CastSegment(const float from[3], const float to[3]) {
    if (!g_caps.collision) return std::nullopt;
    for (int k = 0; k < 3; ++k) {
        if (!Finite(from[k]) || !Finite(to[k])) return std::nullopt;
    }
    alignas(16) float seg[8] = {from[0], from[1], from[2], 0.0f, to[0], to[1], to[2], 0.0f};
    alignas(16) float hit[4] = {};
    alignas(16) float out2[4] = {};
    alignas(16) unsigned char out3[32] = {};
    const auto collision = ReadOr<std::uintptr_t>(re5::addr::kCollision, 0);
    if (!collision) return std::nullopt;  // no world loaded
    SegmentResult r;
    if (!CallSegmentHit(collision, seg, hit, out2, out3, r.flags)) {
        RM_LOGW("collision: segment query faulted; casts off");
        g_caps.collision = false;
        return std::nullopt;
    }
    for (int k = 0; k < 3; ++k) r.hit[k] = hit[k];
    return r;
}

bool StartAction(std::uintptr_t character, int id, const char* expectedClass) {
    if (!g_caps.actions || !character || id < 0) return false;
    const auto hp = ReadHp(character);
    if (!hp || hp->hp <= 0) return false;
    const auto count = ReadOr<std::uint32_t>(character + re5::off::kCharActionCount, 0);
    const auto data = ReadOr<std::uintptr_t>(character + re5::off::kCharActionData, 0);
    if (static_cast<std::uint32_t>(id) >= count || count > 512 || !data) return false;
    const auto action = ReadOr<std::uintptr_t>(data + static_cast<std::size_t>(id) * 4, 0);
    if (!action || ClassName(action) != expectedClass) return false;
    const auto vtable = ReadOr<std::uintptr_t>(character, 0);
    const auto fn = vtable ? ReadOr<std::uintptr_t>(vtable + re5::off::kVtableChangeAction, 0) : 0;
    if (fn != re5::addr::kFnPlayerChangeAction && fn != re5::addr::kFnEnemyChangeAction && fn != re5::addr::kFnChangeAction) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            RM_LOGW("actions: %s has ChangeAction %08X, not a known one; not called", ClassName(character).c_str(),
                    static_cast<unsigned>(fn));
        }
        return false;
    }
    bool started = false;
    if (!CallChangeAction(fn, character, id, started)) {
        RM_LOGW("actions: ChangeAction faulted; actions off");
        g_caps.actions = false;
        return false;
    }
    return started;
}

namespace {

using DodgePrepFn = void(__thiscall*)(std::uintptr_t self, std::uintptr_t player);

bool CallDodgePrep(std::uintptr_t self, std::uintptr_t player) {
    __try {
        reinterpret_cast<DodgePrepFn>(re5::addr::kFnPlayerDodgePrep)(self, player);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

}  // namespace

bool StartDodge(std::uintptr_t player, const float toward[3]) {
    if (!g_caps.dodge || !g_caps.actions || !player) return false;
    for (int k = 0; k < 3; ++k) {
        if (!Finite(toward[k])) return false;
    }
    for (int k = 0; k < 3; ++k) SafeStore<float>(player + re5::off::kPlayerThreatPoint + static_cast<std::size_t>(k) * 4, toward[k]);
    const auto owner = ReadOr<std::uintptr_t>(player + re5::off::kPlayerDodgeOwner, 0);
    if (owner && !CallDodgePrep(owner + re5::off::kDodgePrepOffset, player)) {
        RM_LOGW("dodge: prep faulted; dodges off");
        g_caps.dodge = false;
        return false;
    }
    return StartAction(player, re5::off::kPlActAvoidFront, "cPlAction_pl_act_avoid_front");
}

std::string CurrentActionName(std::uintptr_t character) {
    const auto action = character ? ReadOr<std::uintptr_t>(character + re5::off::kPlayerAction, 0) : 0;
    return action ? ClassName(action) : std::string();
}

namespace {

using TurnToFn = void(__thiscall*)(std::uintptr_t character, unsigned frames);

bool CallTurnTo(std::uintptr_t fn, std::uintptr_t character, unsigned frames) {
    __try {
        reinterpret_cast<TurnToFn>(fn)(character, frames);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

}  // namespace

namespace {

// Writes code bytes over the ones expected there; false (nothing written) when they differ.
bool WriteCode(std::uintptr_t address, const char* expectedHex, const unsigned char* bytes, std::size_t size) {
    if (!BytesMatch(address, expectedHex)) return false;
    auto* p = reinterpret_cast<void*>(address);
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    const bool ok = SafeWrite(address, bytes, size);
    VirtualProtect(p, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, size);
    return ok;
}

}  // namespace

void ApplyCodeFixes() {
    // Excella's finisher ends a Majini in its bomb death, whose effect in mode 2 is a file the PC
    // game lacks: the death takes the mode-5 (Reunion) choice instead, uEm14's em140a from her own
    // archive (re5_layout.h, kDeadBombModeJump).
    if (SignatureOk("dead_bomb_mode_check")) {
        static const unsigned char kNops[] = {0x90, 0x90};
        const bool ok = WriteCode(re5::addr::kDeadBombModeJump, "75 0B", kNops, sizeof(kNops));
        RM_LOGI("code fix: a Majini's bomb death takes Reunion's effect (Excella's finisher): %s", ok ? "applied" : "failed");
    } else {
        RM_LOGW("code fix: the bomb death's mode check is not as expected; Excella's finisher on a Majini may end the game");
    }
}

bool TurnTo(std::uintptr_t character, float yaw, int frames) {
    if (!g_caps.turn || !character || !(yaw > -7.0f && yaw < 7.0f)) return false;
    // Only through the character's own virtual, when it is the routine the signature checked.
    const auto vtable = ReadOr<std::uintptr_t>(character, 0);
    const auto fn = vtable ? ReadOr<std::uintptr_t>(vtable + re5::off::kVtableTurnTo, 0) : 0;
    if (fn != re5::addr::kFnCharTurnTo) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            RM_LOGW("turn: %s has %08X at the turn's slot, not the known one; not called", ClassName(character).c_str(),
                    static_cast<unsigned>(fn));
        }
        return false;
    }
    if (!SafeStore(character + re5::off::kCharYaw, yaw)) return false;
    if (!CallTurnTo(fn, character, static_cast<unsigned>(std::max(0, frames)))) {
        RM_LOGW("turn: the turn faulted; turning off");
        g_caps.turn = false;
        return false;
    }
    return true;
}

namespace {

using ResourceGetFn = std::uintptr_t(__thiscall*)(std::uintptr_t manager, std::uintptr_t dti, const char* path, unsigned flags);
using MotionListSetFn = void(__thiscall*)(std::uintptr_t model, std::uintptr_t list, int slot);
using CharSetMotionFn = void(__thiscall*)(std::uintptr_t character, int layer, unsigned id, float blend, float start, float speed,
                                          unsigned flags);
constexpr unsigned kLookupOnly = 0x80;    // the get's flags: never load
constexpr unsigned kPlayOnce = 4;         // the motion call's flags
constexpr std::size_t kListMotions = 0x60;  // rMotionList: its motion data (u16 count at +6, pointers from +8)
constexpr std::size_t kTrackSize = 0x20;    // a motion's track: +1 type (0..2 a bone's, 3..4 the root's), +3 bone id
constexpr std::uint8_t kBodyBones = 64;     // bone ids 0..63: every player rig's body (the rest: clothing, hair)

bool CallResourceGet(std::uintptr_t manager, const char* path, std::uintptr_t& out) {
    __try {
        out = reinterpret_cast<ResourceGetFn>(re5::addr::kFnResourceGet)(manager, re5::addr::kDtiMotionList, path, kLookupOnly);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool CallResourceRelease(std::uintptr_t manager, std::uintptr_t resource) {
    const std::uintptr_t fn = re5::addr::kFnResourceRelease;
    __try {
        // The release takes the manager in EAX and the resource in EDI.
        __asm {
            mov eax, manager
            mov edi, resource
            call fn
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool CallMotionListSet(std::uintptr_t model, std::uintptr_t list, int slot) {
    __try {
        reinterpret_cast<MotionListSetFn>(re5::addr::kFnMotionListSet)(model, list, slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool CallCharSetMotion(std::uintptr_t character, unsigned id, float blend, float speed) {
    __try {
        reinterpret_cast<CharSetMotionFn>(re5::addr::kFnCharSetMotion)(character, 0, id, blend, 0.0f, speed, kPlayOnce);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// The motion record of `index` in the unit's slot (0: none).
std::uintptr_t MotionRecord(std::uintptr_t unit, int slot, int index) {
    if (!unit || slot < 0 || slot >= re5::off::kMotionListSlots || index < 0 || index > 0xFF) return 0;
    const auto list = ReadOr<std::uintptr_t>(unit + re5::off::kMotionLists + static_cast<std::size_t>(slot) * 4, 0);
    const auto motions = list ? ReadOr<std::uintptr_t>(list + kListMotions, 0) : 0;
    if (!motions || index >= ReadOr<std::uint16_t>(motions + 6, 0)) return 0;
    return ReadOr<std::uintptr_t>(motions + 8 + static_cast<std::size_t>(index) * 4, 0);
}

}  // namespace

std::uintptr_t LoadedMotionList(const char* path) {
    if (!g_caps.motions || !path) return 0;
    const auto manager = ReadOr<std::uintptr_t>(re5::addr::kResourceManager, 0);
    if (!manager) return 0;
    std::uintptr_t list = 0;
    if (!CallResourceGet(manager, path, list)) {
        RM_LOGW("motions: the resource lookup faulted; borrowed motions off");
        g_caps.motions = false;
        return 0;
    }
    if (list && DtiOf(list) != re5::addr::kDtiMotionList && !IsKindOf(list, re5::addr::kDtiMotionList)) {
        RM_LOGW("motions: %s is a %s, not a motion list; not used", path, ClassName(list).c_str());
        ReleaseResource(list);
        return 0;
    }
    return list;
}

void ReleaseResource(std::uintptr_t resource) {
    const auto manager = ReadOr<std::uintptr_t>(re5::addr::kResourceManager, 0);
    if (!g_caps.motions || !resource || !manager) return;
    if (!CallResourceRelease(manager, resource)) {
        RM_LOGW("motions: a resource release faulted; borrowed motions off");
        g_caps.motions = false;
    }
}

int BindMotionList(std::uintptr_t unit, std::uintptr_t list) {
    if (!g_caps.motions || !unit || !list) return -1;
    int freeSlot = -1;
    for (int slot = 0; slot < re5::off::kMotionListSlots; ++slot) {
        const auto held = ReadOr<std::uintptr_t>(unit + re5::off::kMotionLists + static_cast<std::size_t>(slot) * 4, 1);
        if (held == list) return slot;
        if (held == 0) freeSlot = slot;  // the highest free one
    }
    if (freeSlot < 0) return -1;
    if (!CallMotionListSet(unit, list, freeSlot)) {
        RM_LOGW("motions: the motion list setter faulted; borrowed motions off");
        g_caps.motions = false;
        return -1;
    }
    return ReadOr<std::uintptr_t>(unit + re5::off::kMotionLists + static_cast<std::size_t>(freeSlot) * 4, 0) == list ? freeSlot : -1;
}

std::optional<MotionFit> CheckMotion(std::uintptr_t unit, int slot, int index) {
    const std::uintptr_t motion = MotionRecord(unit, slot, index);
    if (!motion) return std::nullopt;
    const auto tracks = ReadOr<std::uintptr_t>(motion, 0);
    const int count = ReadOr<std::int32_t>(motion + 4, 0);
    const auto remap = ReadOr<std::uintptr_t>(unit + re5::off::kModelBoneToJoint, 0);
    const auto joints = ReadOr<std::uint32_t>(unit + re5::off::kModelJointCount, 0);
    if (!tracks || count <= 0 || count > 1024 || !remap) return std::nullopt;
    MotionFit fit;
    fit.frames = static_cast<float>(ReadOr<std::int32_t>(motion + 8, 0));
    for (int t = 0; t < count; ++t) {
        const std::uintptr_t track = tracks + static_cast<std::size_t>(t) * kTrackSize;
        if (ReadOr<std::uint8_t>(track + 1, 0xFF) > 2) continue;  // the root's, or unreadable
        const std::uint8_t bone = ReadOr<std::uint8_t>(track + 3, 0xFF);
        const std::uint8_t joint = ReadOr<std::uint8_t>(remap + bone, 0xFF);
        const bool reaches = joint != 0xFF && joint < joints;
        ++fit.tracks;
        fit.mapped += reaches ? 1 : 0;
        if (bone < kBodyBones) {
            ++fit.bodyTracks;
            fit.bodyMapped += reaches ? 1 : 0;
        }
    }
    return fit;
}

bool PlayMotion(std::uintptr_t unit, int slot, int index, float blendFrames, float speed) {
    if (!g_caps.motions || !MotionRecord(unit, slot, index) || ReadOr<std::uint32_t>(unit + re5::off::kMotionLayerCount, 0) == 0) {
        return false;
    }
    const unsigned id = static_cast<unsigned>(slot) << 8 | static_cast<unsigned>(index);
    if (!CallCharSetMotion(unit, id, blendFrames, speed)) {
        RM_LOGW("motions: the motion call faulted; borrowed motions off");
        g_caps.motions = false;
        return false;
    }
    return ReadOr<std::uint16_t>(unit + re5::off::kMotionLayer0Id, 0xFFFF) == id;
}

std::optional<BodyMotion> ReadBodyMotion(std::uintptr_t unit) {
    if (!unit || ReadOr<std::uint32_t>(unit + re5::off::kMotionLayerCount, 0) == 0) return std::nullopt;
    BodyMotion m;
    m.id = ReadOr<std::uint16_t>(unit + re5::off::kMotionLayer0Id, 0xFFFF);
    m.frame = ReadOr<float>(unit + re5::off::kMotionLayer0Frame, 0.0f);
    m.frames = ReadOr<float>(unit + re5::off::kMotionLayer0Frames, 0.0f);
    return m;
}

int WarpUnit(std::uintptr_t unit, const float pos[3]) {
    if (!unit) return 0;
    const float x = ReadOr<float>(unit + re5::off::kUnitPosition, 0.0f);
    const float y = ReadOr<float>(unit + re5::off::kUnitPosition + 4, 0.0f);
    const float z = ReadOr<float>(unit + re5::off::kUnitPosition + 8, 0.0f);
    // A unit not placed yet stands at the origin, and every run of zeros in it (null pointers,
    // counters) would pass for a copy of its position: writing them ended the game on a match's
    // first frame (Steel Docks, 1189 "copies", a coordinate read as a pointer). Wait until it is.
    constexpr float kUnplaced = 50.0f;  // engine units (half a metre) from the origin
    if (std::fabs(x) < kUnplaced && std::fabs(y) < kUnplaced && std::fabs(z) < kUnplaced) return 0;
    // The copies first, then the writes, and none when there are implausibly many (2 to 12 seen).
    constexpr int kMaxCopies = 24;
    std::vector<std::size_t> copies;
    for (std::size_t off = 0; off < 0x4000; off += 4) {
        const float cx = ReadOr<float>(unit + off, 0.0f), cy = ReadOr<float>(unit + off + 4, 0.0f), cz = ReadOr<float>(unit + off + 8, 0.0f);
        if (std::fabs(cx - x) < 1.0f && std::fabs(cy - y) < 1.0f && std::fabs(cz - z) < 1.0f) {
            copies.push_back(off);
            off += 8;
        }
    }
    if (copies.size() > static_cast<std::size_t>(kMaxCopies)) {
        RM_LOGW("warp: %zu places hold the unit's position %.0f %.0f %.0f, more than a unit has; not moved", copies.size(), x, y, z);
        return 0;
    }
    for (const std::size_t off : copies) {
        for (int i = 0; i < 3; ++i) SafeStore(unit + off + static_cast<std::size_t>(i) * 4, pos[i]);
    }
    return static_cast<int>(copies.size());
}

bool SetHealth(std::uintptr_t character, int hp) {
    if (!ReadHp(character) || hp <= 0) {
        return false;
    }
    const auto value = static_cast<std::int16_t>(hp > 32767 ? 32767 : hp);
    return SafeStore(character + re5::off::kMaxHp, value) && SafeStore(character + re5::off::kHp, value);
}

PlayersSnapshot ReadPlayers() {
    PlayersSnapshot snap;
    if (!g_caps.players) {
        return snap;
    }
    const auto sp = ReadOr<std::uintptr_t>(re5::addr::kSPlayerInstance, 0);
    if (!sp) {
        return snap;
    }
    const auto count = ReadOr<std::uint32_t>(sp + re5::off::kSPlayerCount, 0);
    snap.count = count > 4 ? 4 : static_cast<int>(count);
    for (int i = 0; i < 4; ++i) {
        const auto object = ReadOr<std::uintptr_t>(sp + re5::off::kSPlayerList + i * 4, 0);
        if (!object || !IsKindOf(object, re5::addr::kDtiUPlayer)) {
            continue;
        }
        auto& info = snap.players[i];
        info.object = object;
        info.slot = i;
        info.number = ReadOr<std::uint8_t>(object + re5::off::kPlayerNumber, 0xFF);
        info.control = ReadOr<std::int32_t>(object + re5::off::kPlayerControl, -1);
        info.hp = ReadHp(object).value_or(CharacterHp{});
        info.className = ClassName(object);
    }
    // Hypothesis under test: control 0 marks the locally controlled player and
    // 2 the AI partner. Fall back to the first listed player.
    for (const auto& info : snap.players) {
        if (info.object && info.control == 0) {
            snap.localSlot = info.slot;
            break;
        }
    }
    if (snap.localSlot < 0) {
        for (const auto& info : snap.players) {
            if (info.object) {
                snap.localSlot = info.slot;
                break;
            }
        }
    }
    return snap;
}

std::uintptr_t LocalPlayer() {
    const auto snap = ReadPlayers();
    return snap.localSlot >= 0 ? snap.players[snap.localSlot].object : 0;
}

EnemiesSnapshot ReadEnemies() {
    EnemiesSnapshot snap;
    if (!g_caps.enemies) {
        return snap;
    }
    const auto se = ReadOr<std::uintptr_t>(re5::addr::kSEnemyInstance, 0);
    if (!se) {
        return snap;
    }
    const auto count = ReadOr<std::uint32_t>(se + re5::off::kSEnemyCount, 0);
    const auto array = ReadOr<std::uintptr_t>(se + re5::off::kSEnemyArray, 0);
    if (!array || count > 512) {
        return snap;
    }
    snap.listed = static_cast<int>(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto enemy = ReadOr<std::uintptr_t>(array + i * 4, 0);
        if (!enemy || !IsKindOf(enemy, re5::addr::kDtiUEnemy)) {
            continue;
        }
        const auto hp = ReadHp(enemy);
        if (hp && hp->hp > 0) {
            ++snap.alive;
        }
    }
    return snap;
}

std::vector<EnemyState> ReadEnemyStates() {
    std::vector<EnemyState> out;
    if (!g_caps.enemies) return out;
    const auto se = ReadOr<std::uintptr_t>(re5::addr::kSEnemyInstance, 0);
    if (!se) return out;
    const auto count = ReadOr<std::uint32_t>(se + re5::off::kSEnemyCount, 0);
    const auto array = ReadOr<std::uintptr_t>(se + re5::off::kSEnemyArray, 0);
    if (!array || count > 512) return out;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto enemy = ReadOr<std::uintptr_t>(array + i * 4, 0);
        if (!enemy || !IsKindOf(enemy, re5::addr::kDtiUEnemy)) continue;
        const auto hp = ReadHp(enemy);
        if (hp) out.push_back(EnemyState{enemy, hp->hp, hp->max});
    }
    return out;
}

std::optional<GameWork> ReadGameWork() {
    if (!g_caps.gameWork) {
        return std::nullopt;
    }
    GameWork work;
    work.object = GameWorkObject();
    if (!work.object) {
        return std::nullopt;
    }
    work.mode = ReadOr<std::int32_t>(work.object + re5::off::kGameMode, -1);
    work.stage = ReadOr<std::int32_t>(work.object + re5::off::kGameStage, -1);
    work.mercTimer = ReadOr<float>(work.object + re5::off::kMercTimer, 0.0f);
    work.mercScore = ReadOr<std::int32_t>(work.object + re5::off::kMercScore, 0);
    work.mercKills = ReadOr<std::int32_t>(work.object + re5::off::kMercKills, 0);
    return work;
}

bool WriteMercTimer(float seconds) {
    const auto object = g_caps.gameWork ? GameWorkObject() : 0;
    return object && SafeStore(object + re5::off::kMercTimer, seconds);
}

std::uintptr_t HitAttacker(std::uintptr_t hit) {
    if (!g_caps.hitAttacker || !hit) {
        return 0;
    }
    using Fn = std::uintptr_t(__thiscall*)(std::uintptr_t);
    __try {
        return reinterpret_cast<Fn>(re5::addr::kFnHitGetAttacker)(hit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::string CharacterIdForClass(const std::string& className) {
    // Costume variants are subclasses (e.g. uPl00ChrisNormal), so match on the
    // uPlNN number prefix.
    static constexpr struct {
        const char* prefix;
        const char* id;
    } kMap[] = {
        {"uPl00", "chris"}, {"uPl01", "sheva"},   {"uPl02", "jill"},    {"uPl03", "wesker"}, {"uPl04", "josh"},
        {"uPl05", "excella"}, {"uPl06", "barry"}, {"uPl07", "rebecca"}, {"uPl86", "irving"},
    };
    for (const auto& entry : kMap) {
        if (className.rfind(entry.prefix, 0) == 0) {
            return entry.id;
        }
    }
    return {};
}

}  // namespace rmre5::engine
