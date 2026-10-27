#include "engine.h"

#include <windows.h>

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
