#include "engine.h"

#include <windows.h>

#include <cmath>

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

int WarpUnit(std::uintptr_t unit, const float pos[3]) {
    if (!unit) return 0;
    const float x = ReadOr<float>(unit + re5::off::kUnitPosition, 0.0f);
    const float y = ReadOr<float>(unit + re5::off::kUnitPosition + 4, 0.0f);
    const float z = ReadOr<float>(unit + re5::off::kUnitPosition + 8, 0.0f);
    int written = 0;
    for (std::size_t off = 0; off < 0x4000; off += 4) {
        const float cx = ReadOr<float>(unit + off, 0.0f), cy = ReadOr<float>(unit + off + 4, 0.0f), cz = ReadOr<float>(unit + off + 8, 0.0f);
        if (std::fabs(cx - x) < 1.0f && std::fabs(cy - y) < 1.0f && std::fabs(cz - z) < 1.0f) {
            for (int i = 0; i < 3; ++i) SafeStore(unit + off + static_cast<std::size_t>(i) * 4, pos[i]);
            ++written;
            off += 8;
        }
    }
    return written;
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
