#include "engine.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
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
    // A site whose own first bytes are zero (data: the boss Jill's attack chance table starts with
    // two 0.0 floats) reads the same packed or not and is left out of the wait; it is still compared
    // below. With it in, the wait ran out its whole timeout (2026-11-22: 120 s before the mod started).
    auto allSitesWritten = [] {
        for (const auto& sig : re5::kSignatures) {
            if (std::strncmp(sig.bytes, "00 00 00 00", 11) == 0) continue;
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
    g_caps.mercIntro = SignatureOk("merc_start_state");
    g_caps.playMenu = SignatureOk("playmenu_move") && SignatureOk("playmenu_extra_content") && SignatureOk("smessage_load") && SignatureOk("playmenu_decide_mask") &&
                      SignatureOk("playmenu_cursor_count") && SignatureOk("playmenu_label_table") &&
                      SignatureOk("menu_input") && SignatureOk("smessage_get_dti") &&
                      SignatureOk("playmenu_draw") && SignatureOk("playmenu_row_y") &&
                      SignatureOk("playmenu_label_call") && SignatureOk("menu_draw_label") &&
                      SignatureOk("menu_draw_help") && SignatureOk("playmenu_help_call") &&
                      SignatureOk("menu_navigate") && SignatureOk("menu_row_color") &&
                      SignatureOk("menu_sound_decide") && SignatureOk("menu_sound_cancel") &&
                      SignatureOk("menu_sound_deny") && SignatureOk("menu_ball_index") &&
                      SignatureOk("playmenu_cancel_mask");
    g_caps.menuMoveSound = g_caps.playMenu && SignatureOk("menu_sound_move");
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
    g_caps.splitScreen = g_caps.flowScript && SignatureOk("flow_split_setup");
    g_caps.gameOverCheck = SignatureOk("app_request") && SignatureOk("game_over_check");
    g_caps.mercEndEvent = g_caps.gameOverCheck && SignatureOk("merc_end_event");
    g_caps.appRequestWord = g_caps.gameOverCheck && SignatureOk("app_request_word");
    g_caps.playerDeathChecks = g_caps.gameOverCheck && SignatureOk("player_dead_or_frozen") && SignatureOk("player_in_death");
    g_caps.cockpitRings = g_caps.playerDeathChecks && SignatureOk("cockpit_rings_check");
    g_caps.menuRowGlow = SignatureOk("menu_row_glow") && SignatureOk("layout_name") && SignatureOk("layout_element");
    g_caps.hudNameTables = SignatureOk("inventory_name_read") && SignatureOk("cockpit_name_read");
    g_caps.seatMenuInput = SignatureOk("menu_input") && SignatureOk("pad_trigger_bits") && SignatureOk("pad_repeat_bits") &&
                           SignatureOk("keyboard_trigger") && SignatureOk("keyboard_repeat") && SignatureOk("menu_list_step") &&
                           SignatureOk("menu_navigate_keyboard") && SignatureOk("menu_direction_masks");
    g_caps.playerMayAct = g_caps.playerDeathChecks && SignatureOk("player_may_act");
    g_caps.pickupPartner = g_caps.playerDeathChecks && SignatureOk("player_partner") && SignatureOk("pickup_partner_call");
    g_caps.playerGameOver = g_caps.playerDeathChecks && SignatureOk("player_game_over");
    g_caps.wpSlotUiVisible = g_caps.playerDeathChecks && SignatureOk("wpslot_ui_visible");
    g_caps.gameoverScreen = g_caps.resultsScreen && SignatureOk("gameover_move") && SignatureOk("gameover_move_slot") &&
                            SignatureOk("gameover_decide") && SignatureOk("gameover_decide_block");
    g_caps.pauseScreen = g_caps.playMenu && SignatureOk("pause_move");
    g_caps.slotClear = g_caps.inventory && SignatureOk("inventory_slot_clear");
    g_caps.gameShop = SignatureOk("shop_create") && SignatureOk("unit_register");
    g_caps.enemySpawn = g_caps.enemies && SignatureOk("enemy_create") && SignatureOk("enemy_set_spawn") &&
                        SignatureOk("class_lookup") && SignatureOk("link_units");
    g_caps.timeBonus = g_caps.mercCountdown && SignatureOk("merc_add_countdown") && SignatureOk("time_bonus_action");
    g_caps.hitCheckProbe = g_caps.enemies && SignatureOk("enemy_hit_check");
    g_caps.bossGunfire = g_caps.enemies && SignatureOk("em82_may_hurt") && SignatureOk("uem82_get_dti");
    g_caps.bossNoRefill = g_caps.enemies && SignatureOk("em41_update") && SignatureOk("em40_update") && SignatureOk("uem41_get_dti");
    g_caps.bossBrain = g_caps.enemies && SignatureOk("em40_can_dive") && SignatureOk("em40_ctor_story_wait") &&
                       SignatureOk("em40_intel_release") && SignatureOk("em40_part_life_apply");
    g_caps.faderState = SignatureOk("fader_ctor");
    g_caps.stage308Setup = SignatureOk("s308_setup") && SignatureOk("s308_fuel_read") && SignatureOk("find_model") &&
                           SignatureOk("scene_base_setup") && SignatureOk("as308_get_dti");
    g_caps.areaCameras = SignatureOk("area_camera_check") && SignatureOk("area_camera_none") && SignatureOk("area_camera_force") &&
                         SignatureOk("area_camera_set") && SignatureOk("rahcamera_get_dti");
    g_caps.fatalHandler = SignatureOk("game_fatal_error");
    g_caps.stageLayouts = SignatureOk("stage_layouts") && SignatureOk("register_layout");
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
    g_caps.saveGuard = SignatureOk("save_file_write");
    g_caps.appRequest = SignatureOk("app_request");
    g_caps.itemBoxGuard = g_caps.itemBox && SignatureOk("itembox_panel_update") && SignatureOk("itembox_panel_tab");
    g_caps.itemBoxPictures = g_caps.itemBox && SignatureOk("itembox_picture_name");
    // The motion list type is data: its name is checked too.
    const auto motionListName = ReadOr<std::uintptr_t>(re5::addr::kDtiMotionList + 4, 0);
    g_caps.motions = g_caps.players && SignatureOk("resource_get") && SignatureOk("resource_release") &&
                     SignatureOk("motion_list_set") && SignatureOk("char_set_motion") && motionListName &&
                     ReadCString(motionListName, 16) == "rMotionList";
    g_caps.effects = g_caps.motions && SignatureOk("effect_create") && SignatureOk("effect_call_site") &&
                     ReadCString(ReadOr<std::uintptr_t>(re5::addr::kDtiEffectList + 4, 0), 16) == "rEffectList";
    g_caps.effectStop = g_caps.effects && SignatureOk("effect_stop");
    g_caps.flameStream = g_caps.effectStop && SignatureOk("wp21_fire");
    g_caps.flameCharge = SignatureOk("wp21_charge");
    g_caps.partner = g_caps.players && g_caps.gameWork && SignatureOk("player_add") && SignatureOk("class_by_id");
    g_caps.partnerDeath = g_caps.partner && SignatureOk("player_death_event");
    g_caps.equipCell = g_caps.inventory && SignatureOk("player_equip_cell");
    g_caps.weaponLevels = g_caps.inventory && SignatureOk("weapon_reload_levels") && SignatureOk("weapon_capacity_levels");
    g_caps.infiniteAmmo = g_caps.inventory && SignatureOk("weapon_infinite_check") && SignatureOk("shot_infinite_check");
    g_caps.em84DieBlow = SignatureOk("em84_dieblow_gameover");
    g_caps.em82Tuning = SignatureOk("em82_set_pose") && SignatureOk("em82_pick_rage") && SignatureOk("em82_rage_timer");
    g_caps.em84Tuning = SignatureOk("em84_chance_read") && SignatureOk("em84_cooldown_a_read") && SignatureOk("em84_cooldown_b_read") &&
                        SignatureOk("em84_chance_data") && SignatureOk("em84_cooldown_a_data") && SignatureOk("em84_cooldown_b_data");
    g_caps.weaponScope = g_caps.inventory && g_caps.actions && SignatureOk("weapon_record") && SignatureOk("weapon_scope_read");
    g_caps.twinGuns = SignatureOk("item_unit_create") && SignatureOk("unit_set_euler") && SignatureOk("unit_attach") &&
                      SignatureOk("wp76_fire") && DtiName(re5::addr::kDtiWp76) == "uWp76";
    g_caps.jointPosition = SignatureOk("joint_position");
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

using MayHurtFn = std::uint32_t(__thiscall*)(std::uintptr_t self, std::uint32_t region, std::uint32_t amount);

bool CallMayHurt(std::uintptr_t fn, std::uintptr_t self, int region, int amount, std::uint32_t& out) {
    __try {
        out = reinterpret_cast<MayHurtFn>(fn)(self, static_cast<std::uint32_t>(region), static_cast<std::uint32_t>(amount));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

}  // namespace

std::optional<std::uint32_t> EnemyMayHurt(std::uintptr_t enemy, int region, int amount) {
    const auto vtable = enemy ? ReadOr<std::uintptr_t>(enemy, 0) : 0;
    const auto fn = vtable ? ReadOr<std::uintptr_t>(vtable + re5::off::kVtableMayHurt, 0) : 0;
    if (fn < 0x00400000 || fn > 0x01000000) return std::nullopt;
    std::uint32_t out = 0;
    if (!CallMayHurt(fn, enemy, region, amount, out)) return std::nullopt;
    return out & 0xFFu;  // a bool in AL (Aheri's: MOV AL,1 / XOR AL,AL)
}

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

bool StartAction(std::uintptr_t character, int id, const char* expectedClass, bool atZeroHp) {
    if (!g_caps.actions || !character || id < 0) return false;
    const auto hp = ReadHp(character);
    if (!hp || hp->hp < 0 || (hp->hp == 0 && !atZeroHp)) return false;
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

std::vector<int> ActionIdsOf(std::uintptr_t character, const char* cls) {
    std::vector<int> ids;
    if (!character || !cls) return ids;
    const auto count = ReadOr<std::uint32_t>(character + re5::off::kCharActionCount, 0);
    const auto data = ReadOr<std::uintptr_t>(character + re5::off::kCharActionData, 0);
    if (count > 512 || !data) return ids;
    for (std::uint32_t id = 0; id < count; ++id) {
        const auto action = ReadOr<std::uintptr_t>(data + static_cast<std::size_t>(id) * 4, 0);
        if (action && ClassName(action) == cls) ids.push_back(static_cast<int>(id));
    }
    return ids;
}

std::vector<std::string> ActionNames(std::uintptr_t character) {
    std::vector<std::string> names;
    if (!character) return names;
    const auto count = ReadOr<std::uint32_t>(character + re5::off::kCharActionCount, 0);
    const auto data = ReadOr<std::uintptr_t>(character + re5::off::kCharActionData, 0);
    if (count > 512 || !data) return names;
    for (std::uint32_t id = 0; id < count; ++id) {
        const auto action = ReadOr<std::uintptr_t>(data + static_cast<std::size_t>(id) * 4, 0);
        names.push_back(action ? ClassName(action) : std::string());
    }
    return names;
}

int CurrentActionId(std::uintptr_t character) {
    const auto current = character ? ReadOr<std::uintptr_t>(character + re5::off::kPlayerAction, 0) : 0;
    if (!current) return -1;
    const auto count = ReadOr<std::uint32_t>(character + re5::off::kCharActionCount, 0);
    const auto data = ReadOr<std::uintptr_t>(character + re5::off::kCharActionData, 0);
    if (count > 512 || !data) return -1;
    for (std::uint32_t id = 0; id < count; ++id) {
        if (ReadOr<std::uintptr_t>(data + static_cast<std::size_t>(id) * 4, 0) == current) return static_cast<int>(id);
    }
    return -1;
}

namespace {

using ActionStartFn = std::uintptr_t(__fastcall*)(std::uintptr_t action);
bool g_holdStartTried = false;
bool g_holdStartIn = false;

// The hold's start: the game's own, then the done bit the old action's end left is cleared (the
// game's other starts clear the whole word, 0x00B7A240).
std::uintptr_t __fastcall HoldStart(std::uintptr_t action) {
    const auto result = reinterpret_cast<ActionStartFn>(re5::addr::kFnPlActTestStart)(action);
    const auto player = ReadOr<std::uintptr_t>(action + re5::off::kActionPlayer, 0);
    if (player) {
        const auto flags = ReadOr<std::uint32_t>(player + re5::off::kPlayerActionFlags, 0);
        if (flags & re5::off::kActionDone) SafeStore<std::uint32_t>(player + re5::off::kPlayerActionFlags, flags & ~re5::off::kActionDone);
    }
    return result;
}

}  // namespace

bool EnsureHoldStartClearsDone(std::uintptr_t player) {
    if (g_holdStartIn) return true;
    if (g_holdStartTried || !player) return false;
    const auto count = ReadOr<std::uint32_t>(player + re5::off::kCharActionCount, 0);
    const auto data = ReadOr<std::uintptr_t>(player + re5::off::kCharActionData, 0);
    if (!data || count <= static_cast<std::uint32_t>(re5::off::kPlActHold) || count > 512) return false;
    g_holdStartTried = true;
    const auto action = ReadOr<std::uintptr_t>(data + static_cast<std::size_t>(re5::off::kPlActHold) * 4, 0);
    const auto vtable = action ? ReadOr<std::uintptr_t>(action, 0) : 0;
    const std::uintptr_t slotAt = re5::addr::kVtablePlActTest + re5::off::kActionSlotStart;
    const auto start = ReadOr<std::uintptr_t>(slotAt, 0);
    const auto update = ReadOr<std::uintptr_t>(slotAt + 4, 0);
    const auto done = ReadOr<std::uintptr_t>(slotAt + 0x10, 0);  // slot 10
    const std::string cls = action ? ClassName(action) : "";
    const bool expected = cls == "cPlAction_pl_act_test" && vtable == re5::addr::kVtablePlActTest &&
                          start == re5::addr::kFnPlActTestStart && update == re5::addr::kFnPlActTestUpdate &&
                          done == re5::addr::kFnActionDefaultDone;
    if (!expected) {
        RM_LOGW("actions: the hold's start left as it is: action 0xA8 %s, vtable %08X, start %08X, update %08X, slot 10 %08X",
                cls.c_str(), static_cast<unsigned>(vtable), static_cast<unsigned>(start), static_cast<unsigned>(update),
                static_cast<unsigned>(done));
        return false;
    }
    const auto ours = reinterpret_cast<std::uintptr_t>(&HoldStart);
    auto* p = reinterpret_cast<void*>(slotAt);
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(ours), PAGE_READWRITE, &old)) {
        RM_LOGW("actions: the hold's start left as it is (its vtable is not writable)");
        return false;
    }
    const bool ok = SafeStore<std::uintptr_t>(slotAt, ours);
    VirtualProtect(p, sizeof(ours), old, &old);
    g_holdStartIn = ok && ReadOr<std::uintptr_t>(slotAt, 0) == ours;
    RM_LOGI("actions: the hold's start (vtable %08X slot 6) %s", static_cast<unsigned>(re5::addr::kVtablePlActTest),
            g_holdStartIn ? "now clears the done bit after its own" : "could not be replaced");
    return g_holdStartIn;
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

bool CallResourceGetTyped(std::uintptr_t manager, std::uintptr_t dti, const char* path, unsigned flags, std::uintptr_t& out) {
    __try {
        out = reinterpret_cast<ResourceGetFn>(re5::addr::kFnResourceGet)(manager, dti, path, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

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

std::uintptr_t EffectList(const char* path, bool load) {
    if (!g_caps.motions || !path) return 0;  // the same get and release as the motions'
    const auto manager = ReadOr<std::uintptr_t>(re5::addr::kResourceManager, 0);
    if (!manager) return 0;
    std::uintptr_t list = 0;
    if (!CallResourceGetTyped(manager, re5::addr::kDtiEffectList, path, kLookupOnly, list)) {
        RM_LOGW("effects: the lookup of %s faulted", path);
        return 0;
    }
    if (list || !load) return list;
    if (!EffectFileExists(path)) {
        RM_LOGW("effects: %s.efl is not in nativePC_MT; not loaded", path);
        return 0;
    }
    if (!CallResourceGetTyped(manager, re5::addr::kDtiEffectList, path, 0, list)) {
        RM_LOGW("effects: loading %s faulted", path);
        return 0;
    }
    return list;
}

namespace {

using EffectCreateFn = std::uint32_t(__thiscall*)(std::uintptr_t manager, std::uintptr_t list, std::uintptr_t unit, int joint,
                                                  std::uintptr_t offset, int mode, std::uintptr_t settings, int group);

bool CallEffectCreate(std::uintptr_t manager, std::uintptr_t list, std::uintptr_t unit, int joint, int group, std::uintptr_t settings,
                      std::uint32_t& id) {
    __try {
        id = reinterpret_cast<EffectCreateFn>(re5::addr::kFnEffectCreate)(manager, list, unit, joint, re5::addr::kEffectOffsetZero, 4,
                                                                         settings, group);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool CallEffectStop(std::uintptr_t manager, std::uint32_t id) {
    __try {
        reinterpret_cast<void(__thiscall*)(std::uintptr_t, std::uint32_t)>(re5::addr::kFnEffectStop)(manager, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

}  // namespace

std::uint32_t StartEffect(const char* path, std::uintptr_t unit, int joint, float scale) {
    if (!g_caps.effects || !path || !*path || !unit) return 0;
    const auto manager = ReadOr<std::uintptr_t>(re5::addr::kEffectManager, 0);
    if (!manager) return 0;
    const std::uintptr_t list = EffectList(path, false);  // lookup-only: a list not in memory is skipped
    if (!list) return 0;
    // The group the game's own starters give it (0x00770650): core lists 0x12, stage ones 0x14, others 0x13.
    const std::string_view p(path);
    const int group = p.rfind("effect\\efl\\core", 0) == 0 ? 0x12 : p.rfind("effect\\efl\\stage", 0) == 0 ? 0x14 : 0x13;
    // The game's own settings (two masks, a scale, a time rate), the scale ours when asked.
    alignas(16) std::uint32_t settings[12] = {};
    for (std::size_t i = 0; i < 12; ++i) settings[i] = ReadOr<std::uint32_t>(re5::addr::kEffectSettingsDefault + i * 4, 0);
    if (scale != 1.0f && scale > 0.0f) {
        for (std::size_t i = 4; i < 7; ++i) std::memcpy(&settings[i], &scale, 4);
    }
    std::uint32_t id = 0;
    const bool ok = CallEffectCreate(manager, list, unit, joint, group, reinterpret_cast<std::uintptr_t>(settings), id);
    ReleaseResource(list);
    if (!ok) {
        RM_LOGW("effects: starting %s faulted; the elements' looks are off", path);
        g_caps.effects = false;
        return 0;
    }
    return id != 0 ? id : 1;  // started (an id of 0 would read as "not started")
}

bool PlayEffect(const char* path, std::uintptr_t unit) { return StartEffect(path, unit, -1) != 0; }

bool StopEffect(std::uint32_t id) {
    if (!g_caps.effectStop || id == 0 || id == 1) return false;
    const auto manager = ReadOr<std::uintptr_t>(re5::addr::kEffectManager, 0);
    if (!manager) return false;
    if (!CallEffectStop(manager, id)) {
        RM_LOGW("effects: stopping effect %u faulted; stopping is off", id);
        g_caps.effectStop = false;
        return false;
    }
    return true;
}

bool EffectInMemory(const char* path) {
    const std::uintptr_t list = EffectList(path, false);
    if (!list) return false;
    ReleaseResource(list);
    return true;
}

bool ReadCameraView(CameraView& out) {
    const auto views = ReadCameraViews();
    if (views.empty()) return false;
    out = views.front();
    return true;
}

std::vector<CameraView> ReadCameraViews() {
    std::vector<CameraView> views;
    const auto system = ReadOr<std::uintptr_t>(re5::addr::kSCameraInstance, 0);
    if (!system) return views;
    for (int i = 0; i < re5::off::kSCameraViewportCount; ++i) {
        const std::uintptr_t viewport = system + re5::off::kSCameraViewports + static_cast<std::size_t>(i) * re5::off::kViewportSize;
        const auto camera = ReadOr<std::uintptr_t>(viewport + re5::off::kViewportCamera, 0);
        if (ReadOr<std::uint8_t>(viewport + re5::off::kViewportActive, 0) == 0 || camera < 0x10000) continue;
        CameraView v;
        v.viewport = i;
        v.camera = camera;
        for (int k = 0; k < 3; ++k) {
            v.eye[k] = ReadOr<float>(camera + re5::off::kCameraEye + static_cast<std::size_t>(k) * 4, 0.0f);
            v.target[k] = ReadOr<float>(camera + re5::off::kCameraTarget + static_cast<std::size_t>(k) * 4, 0.0f);
            v.up[k] = ReadOr<float>(camera + re5::off::kCameraUp + static_cast<std::size_t>(k) * 4, 0.0f);
        }
        v.fov = ReadOr<float>(camera + re5::off::kCameraFov, 0.0f);
        v.aspect = ReadOr<float>(camera + re5::off::kCameraAspect, 0.0f);
        v.nearPlane = ReadOr<float>(camera + re5::off::kCameraNear, 0.0f);
        v.farPlane = ReadOr<float>(camera + re5::off::kCameraFar, 0.0f);
        for (int k = 0; k < 4; ++k) v.region[k] = ReadOr<std::int32_t>(viewport + re5::off::kViewportRegion + static_cast<std::size_t>(k) * 4, 0);
        if (!(v.fov > 1.0f && v.fov < 179.0f && v.aspect > 0.1f && v.aspect < 10.0f && v.nearPlane > 0.0f && v.farPlane > v.nearPlane)) continue;
        views.push_back(v);
    }
    return views;
}

std::vector<std::string> LoadedEffectLists() {
    std::vector<std::string> out;
    if (!g_caps.effects) return out;
    const auto manager = ReadOr<std::uintptr_t>(re5::addr::kResourceManager, 0);
    const auto type = ReadOr<std::uint32_t>(re5::addr::kDtiEffectList + re5::addr::kDtiTypeHash, 0);
    if (!manager || !type) return out;
    for (int i = 0; i < re5::addr::kResourceTableSlots; ++i) {
        const auto resource = ReadOr<std::uintptr_t>(manager + re5::addr::kResourceTable + static_cast<std::size_t>(i) * 4, 0);
        if (!resource || ReadOr<std::uint32_t>(resource + re5::addr::kResourceTypeHash, 0) != type) continue;
        std::string path = ReadCString(resource + re5::addr::kResourcePath, 64);
        if (path.rfind("effect\\efl\\", 0) == 0) out.push_back(std::move(path));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool EffectFileExists(const std::string& path) {
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return false;
    std::filesystem::path file = std::filesystem::path(exe).parent_path() / "nativePC_MT" / (path + ".efl");
    std::error_code ec;
    return std::filesystem::is_regular_file(file, ec);
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

namespace {

using PlayerAddFn = std::uintptr_t(__thiscall*)(std::uintptr_t players, int slot, int character, int costume, int arg);
using ClassByIdFn = std::uintptr_t(__cdecl*)(std::uint32_t classId);
constexpr std::size_t kPlayerAreaStart = 0x23300;  // the add's entry table base (save + slot data + this)
constexpr std::size_t kPlayerAreaSize = 0x1600;    // entries, items, counts and the slots' extra blocks

bool CallPlayerAdd(std::uintptr_t players, int slot, int character, int costume, std::uintptr_t& out) {
    __try {
        out = reinterpret_cast<PlayerAddFn>(re5::addr::kFnPlayerAdd)(players, slot, character, costume, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

std::uintptr_t ClassById(std::uint32_t classId) {
    __try {
        return reinterpret_cast<ClassByIdFn>(re5::addr::kFnClassById)(classId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::uintptr_t PlayerAreaBase() {
    const auto save = ReadOr<std::uintptr_t>(re5::addr::kSaveData, 0);
    if (!save) return 0;
    const auto slot = ReadOr<std::int32_t>(save + re5::off::kSaveSlot, -1);
    if (slot < 0 || slot > 7) return 0;
    return save + static_cast<std::size_t>(slot) * re5::off::kSaveSlotStride + kPlayerAreaStart;
}

}  // namespace

namespace {

using ItemUnitCreateFn = std::uintptr_t(__thiscall*)(std::uintptr_t manager, std::uintptr_t dti, int arg);
using UnitAttachFn = void(__thiscall*)(std::uintptr_t unit, std::uintptr_t parent, int joint);
using UnitSetEulerFn = void(__thiscall*)(std::uintptr_t unit, const float* euler);
using GunFireFn = int(__thiscall*)(std::uintptr_t gun, const float* muzzle, const float* dir, int value, unsigned mode);
using UnitVoidFn = void(__thiscall*)(std::uintptr_t unit);
using UnitIntFn = void(__thiscall*)(std::uintptr_t unit, int value);
constexpr std::size_t kUnitOwner = 0xFDC;      // the boss's guns and their shots: who they belong to
constexpr std::size_t kVtableUnitAttach = 0x34;
constexpr std::size_t kVtableUnitEnable = 0x88;  // the boss calls it with 1 after the create
constexpr std::size_t kVtableUnitDelete = 0x30;
constexpr std::size_t kVtableGunFire = 0x1C0;
constexpr unsigned kGunFireMode = 3;           // the boss's ordinary shot (4: another aim mode)

std::uintptr_t VtableSlot(std::uintptr_t object, std::size_t slot) {
    const auto vtable = ReadOr<std::uintptr_t>(object, 0);
    return vtable ? ReadOr<std::uintptr_t>(vtable + slot, 0) : 0;
}

}  // namespace

std::uintptr_t CreateTwinGun() {
    if (!g_caps.twinGuns) return 0;
    const auto manager = ReadOr<std::uintptr_t>(re5::addr::kItemManager, 0);
    if (!manager) return 0;
    std::uintptr_t gun = 0;
    __try {
        gun = reinterpret_cast<ItemUnitCreateFn>(re5::addr::kFnItemUnitCreate)(manager, re5::addr::kDtiWp76, 0);
        if (gun) {
            const auto enable = VtableSlot(gun, kVtableUnitEnable);
            if (enable) reinterpret_cast<UnitIntFn>(enable)(gun, 1);
            *reinterpret_cast<std::uint32_t*>(gun + 4) &= ~0x2000u;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RM_LOGW("twin guns: the create faulted; off");
        g_caps.twinGuns = false;
        return 0;
    }
    return gun;
}

bool AttachTwinGun(std::uintptr_t gun, std::uintptr_t parent, int joint, const float offset[3], const float euler[3]) {
    if (!g_caps.twinGuns || !gun || !parent || VtableSlot(gun, kVtableUnitAttach) != re5::addr::kFnUnitAttach) return false;
    __try {
        reinterpret_cast<UnitAttachFn>(re5::addr::kFnUnitAttach)(gun, parent, joint);
        *reinterpret_cast<std::uintptr_t*>(gun + kUnitOwner) = parent;
        for (int k = 0; k < 3; ++k) *reinterpret_cast<float*>(gun + re5::off::kUnitPosition + k * 4) = offset[k];
        reinterpret_cast<UnitSetEulerFn>(re5::addr::kFnUnitSetEuler)(gun, euler);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RM_LOGW("twin guns: the attach faulted; off");
        g_caps.twinGuns = false;
        return false;
    }
    return true;
}

bool FireTwinGun(std::uintptr_t gun, const float muzzle[3], const float dir[3]) {
    if (!g_caps.twinGuns || !gun || VtableSlot(gun, kVtableGunFire) != re5::addr::kFnWp76Fire) return false;
    // As the boss fires hers (0x0093C2E0): the gun's own fire action (10, 1, 0, -1), then the shot.
    const auto change = VtableSlot(gun, re5::off::kVtableChangeAction);
    __try {
        if (change == re5::addr::kFnChangeAction) reinterpret_cast<ChangeActionFn>(change)(gun, 10, 1, 0, -1, 0);
        reinterpret_cast<GunFireFn>(re5::addr::kFnWp76Fire)(gun, muzzle, dir, 0, kGunFireMode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RM_LOGW("twin guns: the shot faulted; off");
        g_caps.twinGuns = false;
        return false;
    }
    return true;
}

bool JointPosition(std::uintptr_t unit, int bone, float out[3]) {
    if (!g_caps.jointPosition || !unit || bone < 0 || !out) return false;
    using JointPositionFn = void(__thiscall*)(std::uintptr_t unit, float* out, int bone, const float* offset);
    alignas(16) static const float kNoOffset[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // the boss's (0x01249CF0)
    alignas(16) float at[4] = {};
    __try {
        reinterpret_cast<JointPositionFn>(re5::addr::kFnJointPosition)(unit, at, bone, kNoOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RM_LOGW("joint position faulted; off");
        g_caps.jointPosition = false;
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        if (!(at[k] == at[k]) || at[k] < -1.0e7f || at[k] > 1.0e7f) return false;
        out[k] = at[k];
    }
    return true;
}

bool DeleteUnit(std::uintptr_t unit) {
    const auto del = unit ? VtableSlot(unit, kVtableUnitDelete) : 0;
    if (!del) return false;
    __try {
        *reinterpret_cast<std::uint32_t*>(unit + 4) &= ~0x2000u;
        reinterpret_cast<UnitVoidFn>(del)(unit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

std::uintptr_t AddPlayer(int slot, int character, int costume) {
    if (!g_caps.partner || slot < 1 || slot > 3 || character < 0 || character > 0x20 || costume < 0 || costume > 0x10) return 0;
    const auto players = ReadOr<std::uintptr_t>(re5::addr::kSPlayerInstance, 0);
    if (!players || !PlayerAreaBase()) return 0;
    std::uintptr_t unit = 0;
    if (!CallPlayerAdd(players, slot, character, costume, unit)) {
        RM_LOGW("partner: the game's add-player faulted; partners off");
        g_caps.partner = false;
        return 0;
    }
    return unit;
}

std::vector<std::uint8_t> SavePlayerArea() {
    const std::uintptr_t base = PlayerAreaBase();
    std::vector<std::uint8_t> copy(kPlayerAreaSize);
    if (!base || !SafeCopy(copy.data(), base, copy.size())) return {};
    return copy;
}

bool RestorePlayerArea(const std::vector<std::uint8_t>& copy) {
    const std::uintptr_t base = PlayerAreaBase();
    return base && copy.size() == kPlayerAreaSize && SafeWrite(base, copy.data(), copy.size());
}

std::string PlayerClassName(int character, int costume) {
    const std::uint32_t id = (0x10000u | static_cast<std::uint32_t>(character & 0xFFFF)) << 8 | static_cast<std::uint32_t>(costume & 0xFF);
    const std::uintptr_t dti = ClassById(id);
    const auto name = dti ? ReadOr<std::uintptr_t>(dti + 4, 0) : 0;
    return name ? ReadCString(name, 64) : std::string();
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

namespace {
thread_local std::uintptr_t t_actingPlayer = 0;  // ActingPlayerScope
}  // namespace

ActingPlayerScope::ActingPlayerScope(std::uintptr_t unit) : previous_(t_actingPlayer) {
    if (unit) t_actingPlayer = unit;
}

ActingPlayerScope::~ActingPlayerScope() { t_actingPlayer = previous_; }

const std::array<std::uintptr_t, 4>& HumanPlayers();

std::uintptr_t LocalPlayer() {
    if (t_actingPlayer) return t_actingPlayer;
    // The first human player (the cached scan, M10), else ReadPlayers' pick (the first listed).
    for (const std::uintptr_t human : HumanPlayers()) {
        if (human) return human;
    }
    const auto snap = ReadPlayers();
    return snap.localSlot >= 0 ? snap.players[snap.localSlot].object : 0;
}

// The human players by sPlayer slot (control 0), read at most once every 2 ms on a thread: the
// co-op turns ask for them many times a tick (ReadPlayers also reads names and health).
const std::array<std::uintptr_t, 4>& HumanPlayers() {
    thread_local std::array<std::uintptr_t, 4> humans{};
    thread_local LARGE_INTEGER readAt{};
    static const LONGLONG window = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart / 500;  // 2 ms
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (readAt.QuadPart != 0 && now.QuadPart - readAt.QuadPart < window) return humans;
    readAt = now;
    humans.fill(0);
    if (!g_caps.players) return humans;
    const auto sp = ReadOr<std::uintptr_t>(re5::addr::kSPlayerInstance, 0);
    if (!sp) return humans;
    for (int i = 0; i < 4; ++i) {
        const auto object = ReadOr<std::uintptr_t>(sp + re5::off::kSPlayerList + i * 4, 0);
        if (!object || ReadOr<std::int32_t>(object + re5::off::kPlayerControl, -1) != 0) continue;
        if (!IsKindOf(object, re5::addr::kDtiUPlayer)) continue;
        humans[static_cast<std::size_t>(i)] = object;
    }
    return humans;
}

int HumanPlayerSlot(std::uintptr_t unit) {
    if (!unit) return -1;
    const auto& humans = HumanPlayers();
    for (int i = 0; i < 4; ++i) {
        if (humans[static_cast<std::size_t>(i)] == unit) return i;
    }
    return -1;
}

std::uintptr_t HumanPlayerInSlot(int slot) {
    if (slot < 0 || slot >= 4) return 0;
    return HumanPlayers()[static_cast<std::size_t>(slot)];
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
