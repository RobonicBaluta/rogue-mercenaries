#include "rm/profile.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "rm/events.h"
#include "rm/rng.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace rm {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr std::string_view kBankPrefix = "bank:";
constexpr std::string_view kChecksumPrefix = "fnv1a64:";

std::string Hex64(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string Checksum(const json& body) { return std::string(kChecksumPrefix) + Hex64(Fnv1a64(body.dump())); }

json SetToJson(const StringSet& s) {
    json arr = json::array();
    for (const std::string& v : s) arr.push_back(v);
    return arr;
}

json Body(const Profile& p) {
    json j;
    j["schema_version"] = kProfileSchemaVersion;
    j["credits"] = p.credits.balance();
    json ledger = json::object();
    for (const auto& [id, delta] : p.credits.transactions()) ledger[id] = delta;
    j["ledger"] = std::move(ledger);
    j["unlocked_characters"] = SetToJson(p.unlocked_characters);
    j["unlocked_perks"] = SetToJson(p.unlocked_perks);
    j["unlocked_weapons"] = SetToJson(p.unlocked_weapons);
    {
        json equipped = json::object();
        for (const auto& [character, perks] : p.equipped_perks) {
            if (!perks.empty()) equipped[character] = perks;
        }
        j["equipped_perks"] = std::move(equipped);
    }
    j["cosmetics"] = SetToJson(p.cosmetics);
    j["emotes"] = SetToJson(p.emotes);
    j["stats"] = {
        {"runs_started", p.stats.runs_started},
        {"runs_completed", p.stats.runs_completed},
        {"runs_failed", p.stats.runs_failed},
        {"best_arena", p.stats.best_arena},
        {"total_kills", p.stats.total_kills},
        {"credits_earned", p.stats.credits_earned},
        {"no_damage_arenas", p.stats.no_damage_arenas},
        {"s_rank_arenas", p.stats.s_rank_arenas},
    };
    json by_character = json::object();
    for (const auto& [id, n] : p.stats.runs_completed_as) by_character[id] = n;
    j["stats"]["runs_completed_as"] = std::move(by_character);
    j["next_run_number"] = p.next_run_number;
    if (p.active_run) {
        const ActiveRunCheckpoint& a = *p.active_run;
        j["active_run"] = {
            {"run_id", a.run_id},
            {"character_id", a.character_id},
            {"seed", a.seed},
            {"arenas_cleared", a.arenas_cleared},
            {"kills", a.kills},
            {"credits_pending", a.credits_pending},
        };
    } else {
        j["active_run"] = nullptr;
    }
    return j;
}

/// Validation helpers: each returns false and fills `err` on failure.
bool NonNegInt(const json& obj, const char* key, std::int64_t& out, std::string& err, bool required = true) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        if (required) err = std::string("missing field '") + key + "'";
        return !required;
    }
    if (!it->is_number_integer()) {
        err = std::string("field '") + key + "' must be an integer";
        return false;
    }
    out = it->get<std::int64_t>();
    if (out < 0) {
        err = std::string("field '") + key + "' must be >= 0";
        return false;
    }
    return true;
}

bool StringSetField(const json& obj, const char* key, StringSet& out, std::string& err, bool required = true) {
    const auto it = obj.find(key);
    if (it == obj.end()) {
        if (required) err = std::string("missing field '") + key + "'";
        return !required;
    }
    if (!it->is_array()) {
        err = std::string("field '") + key + "' must be an array";
        return false;
    }
    out.clear();
    for (const json& v : *it) {
        if (!v.is_string()) {
            err = std::string("field '") + key + "' must contain only strings";
            return false;
        }
        out.insert(v.get<std::string>());
    }
    return true;
}

Result<ParsedProfile> ParseV1(const json& doc) {
    std::string err;
    const auto fail = [&err]() {
        return Result<ParsedProfile>(Status::Error(ErrorCode::ParseError, "schema validation failed: " + err));
    };
    const auto cs = doc.find("checksum");
    if (cs == doc.end() || !cs->is_string()) {
        err = "missing checksum";
        return fail();
    }
    json body = doc;
    body.erase("checksum");
    if (cs->get<std::string>() != Checksum(body)) {
        return Status::Error(ErrorCode::ParseError, "checksum mismatch");
    }

    ParsedProfile out;
    Profile& p = out.profile;
    std::int64_t credits = 0;
    if (!NonNegInt(doc, "credits", credits, err)) return fail();
    const auto ledger = doc.find("ledger");
    if (ledger == doc.end() || !ledger->is_object()) {
        err = "field 'ledger' must be an object";
        return fail();
    }
    StringMap<std::int64_t> txs;
    for (const auto& item : ledger->items()) {
        if (!item.value().is_number_integer()) {
            err = "ledger entries must be integers";
            return fail();
        }
        txs.emplace(item.key(), item.value().get<std::int64_t>());
    }
    p.credits = Ledger::Restore(credits, std::move(txs));
    if (!StringSetField(doc, "unlocked_characters", p.unlocked_characters, err) ||
        !StringSetField(doc, "unlocked_perks", p.unlocked_perks, err) ||
        !StringSetField(doc, "unlocked_weapons", p.unlocked_weapons, err) ||
        !StringSetField(doc, "cosmetics", p.cosmetics, err, false) ||
        !StringSetField(doc, "emotes", p.emotes, err, false)) {
        return fail();
    }
    if (const auto eq = doc.find("equipped_perks"); eq != doc.end()) {
        if (!eq->is_object()) {
            err = "field 'equipped_perks' must be an object";
            return fail();
        }
        for (const auto& [character, list] : eq->items()) {
            if (!list.is_array()) {
                err = "field 'equipped_perks." + character + "' must be an array";
                return fail();
            }
            std::vector<std::string> perks;
            for (const json& v : list) {
                if (!v.is_string()) {
                    err = "field 'equipped_perks." + character + "' must contain only strings";
                    return fail();
                }
                perks.push_back(v.get<std::string>());
            }
            if (!perks.empty()) p.equipped_perks[character] = std::move(perks);
        }
    }
    const auto stats = doc.find("stats");
    if (stats == doc.end() || !stats->is_object()) {
        err = "field 'stats' must be an object";
        return fail();
    }
    if (!NonNegInt(*stats, "runs_started", p.stats.runs_started, err) ||
        !NonNegInt(*stats, "runs_completed", p.stats.runs_completed, err) ||
        !NonNegInt(*stats, "runs_failed", p.stats.runs_failed, err, false) ||
        !NonNegInt(*stats, "best_arena", p.stats.best_arena, err) ||
        !NonNegInt(*stats, "total_kills", p.stats.total_kills, err) ||
        !NonNegInt(*stats, "credits_earned", p.stats.credits_earned, err, false) ||
        !NonNegInt(*stats, "no_damage_arenas", p.stats.no_damage_arenas, err, false) ||
        !NonNegInt(*stats, "s_rank_arenas", p.stats.s_rank_arenas, err, false)) {
        return fail();
    }
    if (const auto by_character = stats->find("runs_completed_as"); by_character != stats->end()) {
        if (!by_character->is_object()) {
            err = "field 'stats.runs_completed_as' must be an object";
            return fail();
        }
        for (const auto& item : by_character->items()) {
            if (!item.value().is_number_integer() || item.value().get<std::int64_t>() < 0) {
                err = "stats.runs_completed_as entries must be non-negative integers";
                return fail();
            }
            p.stats.runs_completed_as[item.key()] = item.value().get<std::int64_t>();
        }
    }
    if (!NonNegInt(doc, "next_run_number", p.next_run_number, err)) return fail();
    if (p.next_run_number < 1) {
        err = "next_run_number must be >= 1";
        return fail();
    }
    const auto active = doc.find("active_run");
    if (active != doc.end() && !active->is_null()) {
        if (!active->is_object()) {
            err = "field 'active_run' must be an object or null";
            return fail();
        }
        ActiveRunCheckpoint a;
        const auto rid = active->find("run_id");
        const auto cid = active->find("character_id");
        const auto seed = active->find("seed");
        if (rid == active->end() || !rid->is_string() || cid == active->end() || !cid->is_string() ||
            seed == active->end() || !seed->is_number_integer()) {
            err = "active_run requires run_id, character_id and seed";
            return fail();
        }
        a.run_id = rid->get<std::string>();
        a.character_id = cid->get<std::string>();
        a.seed = seed->get<std::uint64_t>();
        std::int64_t cleared = 0;
        if (!NonNegInt(*active, "arenas_cleared", cleared, err) || !NonNegInt(*active, "kills", a.kills, err) ||
            !NonNegInt(*active, "credits_pending", a.credits_pending, err)) {
            return fail();
        }
        a.arenas_cleared = static_cast<int>(cleared);
        p.active_run = std::move(a);
    }
    return out;
}

/// Schema 0 (pre-release prototype, no checksum):
///   {"schema_version":0,"credits":N,"unlocks":["character:jill","perk:x","weapon:y"],
///    "runs_started":N,"runs_completed":N,"banked_runs":["run_0001"],"next_run":N}
Result<ParsedProfile> MigrateV0(const json& doc) {
    std::string err;
    const auto fail = [&err]() {
        return Result<ParsedProfile>(Status::Error(ErrorCode::ParseError, "v0 validation failed: " + err));
    };
    ParsedProfile out;
    out.migrated_from = 0;
    Profile& p = out.profile;
    std::int64_t credits = 0;
    if (!NonNegInt(doc, "credits", credits, err)) return fail();
    StringMap<std::int64_t> txs;
    StringSet banked;
    if (!StringSetField(doc, "banked_runs", banked, err, false)) return fail();
    for (const std::string& run : banked) txs.emplace(BankTxId(run), 0);
    p.credits = Ledger::Restore(credits, std::move(txs));
    StringSet unlocks;
    if (!StringSetField(doc, "unlocks", unlocks, err, false)) return fail();
    for (const std::string& u : unlocks) {
        const auto colon = u.find(':');
        const std::string kind = u.substr(0, colon);
        const std::string id = colon == std::string::npos ? std::string() : u.substr(colon + 1);
        if (id.empty()) {
            err = "unlock entry '" + u + "' must look like '<kind>:<id>'";
            return fail();
        }
        if (kind == "character") {
            p.unlocked_characters.insert(id);
        } else if (kind == "perk") {
            p.unlocked_perks.insert(id);
        } else if (kind == "weapon") {
            p.unlocked_weapons.insert(id);
        } else {
            err = "unknown unlock kind '" + kind + "'";
            return fail();
        }
    }
    if (!NonNegInt(doc, "runs_started", p.stats.runs_started, err, false) ||
        !NonNegInt(doc, "runs_completed", p.stats.runs_completed, err, false)) {
        return fail();
    }
    std::int64_t next = p.stats.runs_started + 1;
    if (!NonNegInt(doc, "next_run", next, err, false)) return fail();
    p.next_run_number = std::max<std::int64_t>(next, p.stats.runs_started + 1);
    return out;
}

bool ReadWhole(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return !in.bad();
}

/// Writes and flushes to stable storage (best effort fsync/_commit).
bool WriteDurable(const fs::path& path, const std::string& data) {
    std::FILE* f = nullptr;
#if defined(_WIN32)
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "wb");
#endif
    if (f == nullptr) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fflush(f) == 0 && ok;
#if defined(_WIN32)
    ok = _commit(_fileno(f)) == 0 && ok;
#else
    ok = fsync(fileno(f)) == 0 && ok;
#endif
    ok = std::fclose(f) == 0 && ok;
    return ok;
}

}  // namespace

std::string BankTxId(std::string_view run_id) { return std::string(kBankPrefix) + std::string(run_id); }

bool Profile::IsRunBanked(std::string_view run_id) const { return credits.Contains(BankTxId(run_id)); }

std::vector<std::string> Profile::BankedRunIds() const {
    std::vector<std::string> ids;
    for (const auto& [tx, delta] : credits.transactions()) {
        if (tx.rfind(kBankPrefix, 0) == 0) ids.push_back(tx.substr(kBankPrefix.size()));
    }
    return ids;
}

TxResult BankRunCredits(Profile& profile, std::string_view run_id, std::int64_t amount) {
    if (run_id.empty()) return TxResult::InvalidId;
    return profile.credits.Credit(BankTxId(run_id), amount);
}

std::string SerializeProfile(const Profile& profile) {
    json body = Body(profile);
    const std::string checksum = Checksum(body);
    body["checksum"] = checksum;
    return body.dump(2) + "\n";
}

Result<ParsedProfile> ParseProfile(std::string_view json_text) {
    json doc;
    try {
        doc = json::parse(json_text);
    } catch (const json::exception& e) {
        return Status::Error(ErrorCode::ParseError, std::string("invalid JSON: ") + e.what());
    }
    if (!doc.is_object()) return Status::Error(ErrorCode::ParseError, "profile must be a JSON object");
    const auto ver = doc.find("schema_version");
    if (ver == doc.end() || !ver->is_number_integer()) {
        return Status::Error(ErrorCode::ParseError, "missing integer schema_version");
    }
    const std::int64_t version = ver->get<std::int64_t>();
    if (version == 0) return MigrateV0(doc);
    if (version == kProfileSchemaVersion) return ParseV1(doc);
    return Status::Error(ErrorCode::ParseError,
                         "unsupported schema_version " + std::to_string(version) + " (this build reads 0.." +
                             std::to_string(kProfileSchemaVersion) + ")");
}

std::string_view ProfileSourceName(ProfileSource s) {
    switch (s) {
        case ProfileSource::Main: return "main";
        case ProfileSource::Backup: return "backup";
        case ProfileSource::Default: return "default";
    }
    return "default";
}

ProfileStore::ProfileStore(std::filesystem::path main_path, IEventSink* sink)
    : main_(std::move(main_path)), sink_(sink) {}

std::filesystem::path ProfileStore::backup_path() const {
    fs::path p = main_;
    p += ".bak";
    return p;
}

std::filesystem::path ProfileStore::temp_path() const {
    fs::path p = main_;
    p += ".tmp";
    return p;
}

ProfileLoadResult ProfileStore::Load() {
    ProfileLoadResult result;
    auto emit_error = [this](const std::string& op, const std::string& message, const fs::path& path,
                             const fs::path& preserved) {
        if (sink_ == nullptr) return;
        Event e;
        e.type = EventType::Error;
        e.level = EventLevel::Error;
        e.Add("operation", op).Add("message", message).Add("path", path.string());
        if (!preserved.empty()) e.Add("preserved_as", preserved.string());
        sink_->OnEvent(e);
    };

    auto try_load = [&](const fs::path& path, ProfileSource source) -> bool {
        std::error_code ec;
        if (!fs::exists(path, ec)) return false;
        std::string text;
        if (!ReadWhole(path, text)) {
            const std::string msg = path.filename().string() + ": cannot read file";
            result.errors.push_back(msg);
            emit_error("profile_load", msg, path, {});
            return false;
        }
        Result<ParsedProfile> parsed = ParseProfile(text);
        if (parsed.ok()) {
            result.profile = parsed.value().profile;
            result.migrated_from = parsed.value().migrated_from;
            result.source = source;
            return true;
        }
        // Preserve the rejected file for inspection; never delete user data.
        fs::path preserved;
        for (int n = 1; n < 10000; ++n) {
            fs::path candidate = main_;
            candidate += ".corrupt-" + std::to_string(n);
            if (!fs::exists(candidate, ec)) {
                preserved = candidate;
                break;
            }
        }
        if (!preserved.empty()) {
            fs::rename(path, preserved, ec);
            if (ec) preserved.clear();
        }
        const std::string msg = path.filename().string() + ": " + parsed.status().message;
        result.errors.push_back(msg);
        if (!preserved.empty()) result.preserved.push_back(preserved);
        emit_error("profile_load", msg, path, preserved);
        return false;
    };

    if (!try_load(main_, ProfileSource::Main) && !try_load(backup_path(), ProfileSource::Backup)) {
        result.profile = Profile{};
        result.source = ProfileSource::Default;
    }
    if (sink_ != nullptr) {
        Event e;
        e.type = EventType::ProfileLoaded;
        e.Add("source", ProfileSourceName(result.source))
            .Add("migrated_from", result.migrated_from)
            .Add("credits", result.profile.credits.balance())
            .Add("errors", result.errors.size());
        sink_->OnEvent(e);
    }
    return result;
}

Status ProfileStore::Save(const Profile& profile) {
    const std::string text = SerializeProfile(profile);
    std::error_code ec;
    if (main_.has_parent_path()) fs::create_directories(main_.parent_path(), ec);

    const fs::path tmp = temp_path();
    if (!WriteDurable(tmp, text)) {
        return Status::Error(ErrorCode::PersistenceFailed, "cannot write " + tmp.string());
    }
    // Keep the previous file as .bak only if it is still a valid profile, so a corrupt main
    // never overwrites a good backup.
    if (fs::exists(main_, ec)) {
        std::string previous;
        if (ReadWhole(main_, previous) && ParseProfile(previous).ok()) {
            fs::copy_file(main_, backup_path(), fs::copy_options::overwrite_existing, ec);
            if (ec && sink_ != nullptr) {
                Event e;
                e.type = EventType::Error;
                e.level = EventLevel::Error;
                e.Add("operation", "profile_backup").Add("message", ec.message());
                sink_->OnEvent(e);
            }
        }
    }
    ec.clear();
    fs::rename(tmp, main_, ec);  // replaces the existing file (MoveFileEx REPLACE_EXISTING on MSVC)
    if (ec) {
        return Status::Error(ErrorCode::PersistenceFailed, "cannot replace " + main_.string() + ": " + ec.message());
    }
    if (sink_ != nullptr) {
        Event e;
        e.type = EventType::ProfileSaved;
        e.level = EventLevel::Debug;
        e.Add("path", main_.string()).Add("credits", profile.credits.balance()).Add("bytes", text.size());
        sink_->OnEvent(e);
    }
    return Status::Success();
}

}  // namespace rm
