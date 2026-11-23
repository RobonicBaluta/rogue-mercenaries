#include "rm/run_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "rm/rosters.h"
#include "rm/run_resume.h"

namespace rm {
namespace {

std::string RunIdFor(std::int64_t number) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "run_%04lld", static_cast<long long>(number));
    return buf;
}

std::string ArenaTx(int arena, std::string_view what) {
    return "a" + std::to_string(arena) + ":" + std::string(what);
}

std::int64_t RoundAmount(double v) { return std::max<std::int64_t>(0, static_cast<std::int64_t>(std::llround(v))); }

const std::optional<ActiveRunCheckpoint> kNoCheckpoint;

}  // namespace

std::string_view UnlockKindName(UnlockKind k) {
    switch (k) {
        case UnlockKind::Character: return "character";
        case UnlockKind::Perk: return "perk";
        case UnlockKind::Weapon: return "weapon";
    }
    return "character";
}

std::string_view PhaseName(Phase p) {
    switch (p) {
        case Phase::Idle: return "idle";
        case Phase::ArenaActive: return "arena_active";
        case Phase::ArenaResults: return "arena_results";
        case Phase::RewardSelection: return "reward_selection";
        case Phase::Shop: return "shop";
        case Phase::RouteSelection: return "route_selection";
        case Phase::RunComplete: return "run_complete";
        case Phase::RunFailed: return "run_failed";
    }
    return "idle";
}

RunController::RunController(std::shared_ptr<const Content> content, Profile profile, RunControllerOptions options)
    : content_(std::move(content)),
      profile_(std::move(profile)),
      caps_(std::move(options.capabilities)),
      sink_(options.sink),
      store_(options.store),
      damage_sample_every_(options.damage_sample_every) {
    if (content_ == nullptr) throw std::invalid_argument("RunController requires loaded content");
    // Challenges added after a profile was written, or met by an older build, unlock on load.
    if (!EvaluateCharacterUnlocks().empty()) (void)SaveProfile("character_unlocks");
}

void RunController::SetCapabilities(Capabilities caps) { caps_ = std::move(caps); }

// ---- events / helpers ---------------------------------------------------------------------

Event RunController::MakeEvent(EventType type, EventLevel level) const {
    Event e;
    e.type = type;
    e.level = level;
    if (run_.active) {
        e.run_id = run_.run_id;
        e.arena = run_.arena_index;
        if (!run_.seats.empty()) e.Add("seat", run_.active_seat);
    }
    return e;
}

void RunController::Emit(Event event) const {
    if (sink_ != nullptr) sink_->OnEvent(event);
}

void RunController::EmitError(std::string_view operation, const Status& status) const {
    Event e = MakeEvent(EventType::Error, EventLevel::Error);
    e.Add("operation", operation).Add("code", ErrorCodeName(status.code)).Add("message", status.message);
    Emit(std::move(e));
}

Status RunController::Fail(std::string_view operation, ErrorCode code, std::string message) const {
    Status s = Status::Error(code, std::move(message));
    EmitError(operation, s);
    return s;
}

Status RunController::SaveProfile(std::string_view reason) {
    if (store_ == nullptr) return Status::Success();
    Status s = store_->Save(profile_);
    if (!s.ok()) EmitError(std::string("profile_save:") + std::string(reason), s);
    return s;
}

const CharacterDef* RunController::Character() const {
    // During a run the skin's overrides apply (weapons, passive, skill pool).
    return run_.active ? &run_.character : content_->FindCharacter(run_.character_id);
}

const RouteDef* RunController::Route() const { return content_->FindRoute(run_.route_id); }

ArenaMetrics* RunController::CurrentArena() { return run_.arenas.empty() ? nullptr : &run_.arenas.back(); }

void RunController::PauseArenaClock(double seconds) {
    if (!run_.active || run_.phase != Phase::ArenaActive || !(seconds > 0.0) || seconds > 3600.0) return;
    if (ArenaMetrics* a = CurrentArena()) a->start_time += seconds;
}

const ArenaMetrics* RunController::CurrentArena() const {
    return run_.arenas.empty() ? nullptr : &run_.arenas.back();
}

bool RunController::WeaponIdMode() const { return caps_.Has(cap::kWeaponId); }

std::string RunController::NextInstanceId() { return "w" + std::to_string(run_.next_weapon_serial++); }

namespace {
// The game item the build hands out for `item` (its items_as: Safari Chris's bombs for grenades), else `item`.
std::int64_t ItemAsGameItem(const SkinBuild& build, std::int64_t item) {
    for (const auto& [from, to] : build.items_as) {
        if (from == item) return to;
    }
    return item;
}
}  // namespace

OfferContext RunController::MakeOfferContext(double rarity_shift) const {
    OfferContext ctx;
    ctx.content = content_.get();
    ctx.character = Character();
    ctx.capabilities = &caps_;
    ctx.unlocked_perks = &profile_.unlocked_perks;
    ctx.unlocked_weapons = &profile_.unlocked_weapons;
    ctx.placeable_weapons = placeable_weapons_ ? &*placeable_weapons_ : nullptr;
    ctx.owned_weapons = &run_.weapons;
    ctx.owned_perks = &run_.perks;
    ctx.rarity_shift = rarity_shift + run_.character.build.rarity_shift;
    ctx.build = &run_.character.build;
    const RouteDef* route = Route();
    ctx.reward_mult = (route != nullptr ? route->reward_mult : 1.0) * run_.character.build.supplies_mult;
    // M10: what the other players were offered, their shop and their rewards (the user, 2026-11-22: "stores
    // and rewards should be separated for each player"): left out while anything else is left.
    offer_avoid_.clear();
    for (std::size_t i = 0; i < run_.seats.size(); ++i) {
        if (static_cast<int>(i) == run_.active_seat) continue;
        const PlayerSeat& other = run_.seats[i];
        for (const ShopOffer& o : other.shop_offers) {
            if (o.kind == ShopOfferKind::Weapon) offer_avoid_.insert("weapon:" + o.weapon.base_id);
            if (o.kind == ShopOfferKind::Perk) offer_avoid_.insert("perk:" + o.perk_id);
            if (o.kind == ShopOfferKind::Item) offer_avoid_.insert("item:" + o.item_id);
        }
        for (const RewardChoice& c : other.reward_offer) {
            if (c.kind == RewardKind::Weapon) offer_avoid_.insert("weapon:" + c.weapon.base_id);
            if (c.kind == RewardKind::Perk) offer_avoid_.insert("perk:" + c.perk_id);
            if (c.kind == RewardKind::Item) offer_avoid_.insert("item:" + c.item_id);
        }
    }
    ctx.avoid = offer_avoid_.empty() ? nullptr : &offer_avoid_;
    return ctx;
}

PerkGateInput RunController::MakeGateInput(CursePolicy curses) const {
    PerkGateInput in;
    in.character = Character();
    in.capabilities = &caps_;
    in.unlocked_perks = &profile_.unlocked_perks;
    in.owned = &run_.perks;
    in.curses = curses;
    return in;
}

namespace {
// Local co-op: a seat other than the first gets transaction ids of its own, so the same perk or
// reward pays each seat (the ledger refuses an id twice).
std::string SeatTx(const RunState& run, const std::string& tx) {
    return run.active_seat > 0 ? tx + ":seat" + std::to_string(run.active_seat) : tx;
}
}  // namespace

bool RunController::CreditSupplies(const std::string& seat_tx, std::int64_t amount, std::string_view reason, bool build_mult) {
    const std::string tx = SeatTx(run_, seat_tx);
    if (build_mult) amount = RoundAmount(static_cast<double>(amount) * run_.character.build.supplies_mult * (1.0 + run_.stats.supplies));
    if (amount <= 0) return false;
    const TxResult r = run_.supplies.Credit(tx, amount);
    if (r != TxResult::Applied) {
        EmitError("credit_supplies", Status::Error(ErrorCode::AlreadyApplied,
                                                   tx + ": " + std::string(TxResultName(r))));
        return false;
    }
    run_.supplies_earned += amount;
    if (run_.phase == Phase::ArenaActive) {
        if (ArenaMetrics* a = CurrentArena()) a->supplies_awarded += amount;
    }
    Event e = MakeEvent(EventType::CurrencyChanged);
    e.Add("currency", "supplies").Add("delta", amount).Add("balance", run_.supplies.balance())
        .Add("tx", tx).Add("reason", reason);
    Emit(std::move(e));
    return true;
}

TxResult RunController::DebitSupplies(const std::string& seat_tx, std::int64_t amount, std::string_view reason) {
    const std::string tx = SeatTx(run_, seat_tx);
    const TxResult r = run_.supplies.Debit(tx, amount);
    if (r == TxResult::Applied) {
        run_.supplies_spent += amount;
        Event e = MakeEvent(EventType::CurrencyChanged);
        e.Add("currency", "supplies").Add("delta", -amount).Add("balance", run_.supplies.balance())
            .Add("tx", tx).Add("reason", reason);
        Emit(std::move(e));
    }
    return r;
}

Status RunController::GrantPerk(std::string_view perk_id, std::string_view source, CursePolicy curses,
                                bool ignore_unlock) {
    const PerkDef* def = content_->FindPerk(perk_id);
    if (def == nullptr) return Status::Error(ErrorCode::UnknownId, "unknown perk '" + std::string(perk_id) + "'");
    PerkGateInput in = MakeGateInput(curses);
    in.ignore_unlock = ignore_unlock;
    const PerkGate gate = CheckPerk(*def, in);
    if (gate != PerkGate::Eligible) {
        return Status::Error(ErrorCode::NotEligible,
                             "perk '" + def->id + "' is not eligible: " + std::string(PerkGateName(gate)));
    }
    const int stacks = ++run_.perks[def->id];
    Event e = MakeEvent(EventType::PerkAcquired);
    e.Add("perk", def->id).Add("stacks", stacks).Add("source", source)
        .Add("rarity", RarityName(def->rarity)).Add("category", PerkCategoryName(def->category));
    Emit(std::move(e));
    // Supply perks: their items go to the adapter's queue (applied at the first arena, or at
    // once when granted mid-run).
    if (caps_.Has(cap::kInventory)) {
        for (const EffectDef& ef : def->effects) {
            if (ef.kind != EffectKind::ItemOnRunStart) continue;
            const EffectParams p = ef.AtStacks(stacks);
            const auto game_item = ItemAsGameItem(run_.character.build, static_cast<std::int64_t>(std::lround(p.game_item)));
            run_.item_grants.push_back(ItemGrant{game_item, std::max(1, static_cast<int>(std::lround(p.amount))), def->id});
            // A gun is one of the run's guns too, as the build's starting gun is: its level, the
            // upgrades and refines, the build's elements and its stats (2026-11-19, the user: "the
            // extra weapons that come from perks are not upgradeable", Excella's Dragunov without
            // Field Trials' element). One the run holds already (the build's) keeps its instance.
            const auto& weapons = content_->weapons();
            const auto base = std::find_if(weapons.begin(), weapons.end(),
                                           [game_item](const WeaponBaseDef& w) { return w.game_item >= 0 && w.game_item == game_item; });
            if (base != weapons.end() &&
                std::none_of(run_.weapons.begin(), run_.weapons.end(), [&base](const WeaponInstance& w) { return w.base_id == base->id; })) {
                GrantWeapon(MakeStartingWeapon(base->id, NextInstanceId()), def->id);
            }
        }
    }
    return Status::Success();
}

void RunController::GrantWeapon(WeaponInstance weapon, std::string_view source) {
    if (!WeaponIdMode()) {
        run_.weapons.clear();  // single sidearm slot
        run_.weapons.push_back(weapon);
    } else {
        const auto it = std::find_if(run_.weapons.begin(), run_.weapons.end(),
                                     [&weapon](const WeaponInstance& w) { return w.base_id == weapon.base_id; });
        if (it != run_.weapons.end()) {
            *it = weapon;
        } else {
            run_.weapons.push_back(weapon);
        }
    }
    Event e = MakeEvent(EventType::PerkAcquired, EventLevel::Debug);
    e.Add("weapon_instance", weapon.instance_id).Add("weapon", weapon.base_id)
        .Add("rarity", RarityName(weapon.rarity)).Add("affixes", weapon.affixes.size())
        .Add("slot", WeaponIdMode() ? std::string_view("by_weapon_id") : kSidearmSlot).Add("source", source);
    Emit(std::move(e));
}

void RunController::UpdateCheckpoint() {
    if (!run_.active) return;
    ActiveRunCheckpoint cp;
    cp.run_id = run_.run_id;
    cp.character_id = run_.seats.empty() || run_.active_seat == 0 ? run_.character_id : run_.seats.front().character_id;
    cp.seed = run_.seed;
    cp.arenas_cleared = static_cast<int>(run_.results.size());
    for (const ArenaMetrics& a : run_.arenas) cp.kills += a.player_kills;
    cp.credits_pending = run_.credits_pending;
    // The run's resume point stays until a newer one is saved (SaveResumePoint).
    if (profile_.active_run && profile_.active_run->run_id == run_.run_id) {
        cp.resume = std::move(profile_.active_run->resume);
        cp.resume_adapter = std::move(profile_.active_run->resume_adapter);
    }
    profile_.active_run = std::move(cp);
}

Status RunController::SaveResumePoint(std::string adapter_state) {
    constexpr std::string_view op = "save_resume_point";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    const bool beforeFirst = run_.phase == Phase::Idle && run_.arena_index == 0;
    const bool beforeNext = run_.phase == Phase::RouteSelection && run_.route_chosen;
    if (!beforeFirst && !beforeNext) {
        return Fail(op, ErrorCode::WrongPhase, std::string("a resume point is saved between arenas, not in ") + std::string(PhaseName(run_.phase)));
    }
    UpdateCheckpoint();
    profile_.active_run->resume = SerializeRunResume(run_);
    profile_.active_run->resume_adapter = std::move(adapter_state);
    return SaveProfile("resume_point");
}

// ---- the ladder (EconomyConfig::Ladder) ----------------------------------------------------

bool RunController::Beaten(std::string_view key) const {
    return profile_.stats.bosses_beaten.find(std::string(key)) != profile_.stats.bosses_beaten.end();
}

bool RunController::LengthOpen(const EconomyConfig::Ladder::Length& length) const {
    return length.unlocked_by.empty() || Beaten(length.unlocked_by);
}

bool RunController::FinalePossible(const EconomyConfig::Ladder::Length& length) const {
    const auto& finale = content_->economy().ladder.finale;
    return finale.boss >= 0 && finale.after == length.id;
}

bool RunController::FinaleEarned() const {
    const auto& needs = content_->economy().ladder.finale.needs_hard;
    return std::all_of(needs.begin(), needs.end(), [this](const std::string& id) {
        return std::find(run_.ladder_beaten.begin(), run_.ladder_beaten.end(), id + ":hard") != run_.ladder_beaten.end();
    });
}

std::vector<int> RunController::BossChoices(const EconomyConfig::Ladder::Length& length) const {
    std::vector<int> out{length.easy_boss};
    if (Beaten(length.id + ":easy")) out.insert(out.end(), length.hard_bosses.begin(), length.hard_bosses.end());
    return out;
}

namespace {
// The length a ladder boss beaten (or fought) at the run's current arena belongs to: the one whose
// arena it is (a shorter length's boss inside a longer run), else the run's own.
const EconomyConfig::Ladder::Length* BossTier(const EconomyConfig::Ladder& ladder, const EconomyConfig::Ladder::Length& run,
                                             int arena, int map_key) {
    if (const auto* tier = ladder.TierAt(run.id, arena); tier != nullptr && (map_key == tier->easy_boss || tier->IsHard(map_key))) {
        return tier;
    }
    return map_key == run.easy_boss || run.IsHard(map_key) ? &run : nullptr;
}
}  // namespace

double RunController::BossHpMult(int map_key) const {
    if (EndlessRun()) return EndlessBossHpMult(run_.arena_index);
    const EconomyConfig::Ladder& ladder = content_->economy().ladder;
    const auto* length = ladder.enabled ? ladder.Find(run_.length_id) : nullptr;
    if (length == nullptr) return 1.0;
    if (run_.arena_index > length->arenas && map_key == ladder.finale.boss) return ladder.finale.boss_hp_mult;
    const auto* tier = BossTier(ladder, *length, run_.arena_index, map_key);
    return tier != nullptr ? tier->boss_hp_mult : 1.0;
}

void RunController::RecordBossBeaten(int map_key) {
    const MapDef* map = content_->FindMapByStage(map_key);
    if (map == nullptr || !map->boss) return;
    std::vector<std::string> keys{"boss:" + map->boss->unit_class};
    const EconomyConfig::Ladder& ladder = content_->economy().ladder;
    if (const auto* length = ladder.enabled ? ladder.Find(run_.length_id) : nullptr) {
        std::string key;
        if (run_.arena_index > length->arenas && map_key == ladder.finale.boss) {
            key = "finale";
        } else if (const auto* tier = BossTier(ladder, *length, run_.arena_index, map_key)) {
            key = tier->id + (map_key == tier->easy_boss ? ":easy" : ":hard");
        }
        if (!key.empty()) {
            run_.ladder_beaten.push_back(key);
            keys.push_back(std::move(key));
        }
    }
    for (const std::string& key : keys) {
        if (!profile_.stats.bosses_beaten.insert(key).second) continue;
        run_.bosses_beaten_new.push_back(key);
        Event e = MakeEvent(EventType::PerkTriggered);
        e.Add("source", "ladder").Add("trigger", "boss_beaten").Add("key", key).Add("map", map_key);
        Emit(std::move(e));
    }
}

bool RunController::EndlessRun() const {
    const EconomyConfig::Ladder& ladder = content_->economy().ladder;
    return ladder.enabled && ladder.endless.Is(run_.length_id);
}

bool RunController::EndlessOpen() const {
    const EconomyConfig::Ladder::Endless& e = content_->economy().ladder.endless;
    return content_->economy().ladder.enabled && !e.id.empty() && (e.unlocked_by.empty() || Beaten(e.unlocked_by));
}

std::vector<RunController::EndlessTier> RunController::EndlessTiers() const {
    const EconomyConfig::Ladder& ladder = content_->economy().ladder;
    std::vector<EndlessTier> out;
    for (const auto& length : ladder.lengths) {
        if (LengthOpen(length)) out.push_back({BossChoices(length), length.boss_hp_mult});
    }
    if (ladder.finale.boss >= 0 && Beaten("finale")) out.push_back({{ladder.finale.boss}, ladder.finale.boss_hp_mult});
    return out;
}

std::vector<int> RunController::EndlessBossesAt(int arena) const {
    const int every = content_->economy().ladder.endless.boss_every;
    const std::vector<EndlessTier> tiers = EndlessTiers();
    if (tiers.empty() || every <= 0 || arena <= 0 || arena % every != 0) return {};
    return tiers[static_cast<std::size_t>(arena / every - 1) % tiers.size()].bosses;
}

double RunController::EndlessBossHpMult(int arena) const {
    const EconomyConfig::Ladder::Endless& e = content_->economy().ladder.endless;
    const std::vector<EndlessTier> tiers = EndlessTiers();
    if (tiers.empty() || e.boss_every <= 0 || arena <= 0 || arena % e.boss_every != 0) return 1.0;
    const std::size_t k = static_cast<std::size_t>(arena / e.boss_every - 1);
    const double round = static_cast<double>(k / tiers.size());
    return tiers[k % tiers.size()].hp_mult * (1.0 + e.boss_hp_per_round * round);
}

RunMap RunController::EndlessMap(int layers, Rng& rng) const {
    const int every = std::max(1, content_->economy().ladder.endless.boss_every);
    RunMapPlan plan;
    plan.layers = layers;
    for (int a = every; a < layers; a += every) plan.earlier_bosses[a] = EndlessBossesAt(a);
    plan.bosses = EndlessBossesAt(layers);
    return GenerateRunMap(*content_, rng, arena_pool_, first_arena_, plan);
}

void RunController::ExtendEndless() {
    const int from = run_.arena_count;
    const int to = from + content_->economy().ladder.endless.boss_every;
    // The whole map to `to` from its own stream (the same for a seed), of which the new arenas are
    // kept: their tiers and boss turns are those of their depth.
    Rng rng = Rng::Derive(run_.seed, "endless_map_" + std::to_string(to));
    RunMap grown = EndlessMap(to, rng);
    std::vector<std::vector<RunMapNode>>& layers = run_.run_map.layers;
    if (layers.empty() || layers.size() != static_cast<std::size_t>(from) || grown.layers.size() != static_cast<std::size_t>(to)) {
        Event e = MakeEvent(EventType::Error);
        e.Add("op", "endless").Add("reason", "run map shape").Add("arenas", from);
        Emit(std::move(e));
        return;
    }
    // The boss that ended the map leads to every node of the first new arena.
    for (RunMapNode& p : layers.back()) {
        p.next.clear();
        for (std::size_t k = 0; k < grown.layers[static_cast<std::size_t>(from)].size(); ++k) p.next.push_back(static_cast<int>(k));
    }
    for (int d = from; d < to; ++d) layers.push_back(std::move(grown.layers[static_cast<std::size_t>(d)]));
    run_.arena_count = to;
    // The guests again from the start of their own stream: the arenas already planned keep theirs.
    Rng rosters = Rng::Derive(run_.seed, streams::kRoster);
    PlanRunRosters(*content_, rosters, run_.run_map);
    Event e = MakeEvent(EventType::PerkTriggered);
    e.Add("source", "ladder").Add("trigger", "endless_grown").Add("arenas", to);
    Emit(std::move(e));
}

std::string RunController::PacingLengthId() const {
    const EconomyConfig::Ladder& ladder = content_->economy().ladder;
    if (!EndlessRun()) return run_.length_id;
    if (!ladder.endless.paced_as.empty()) return ladder.endless.paced_as;
    return ladder.lengths.empty() ? run_.length_id : ladder.lengths.back().id;
}

int RunController::PacingArenas() const {
    if (!EndlessRun()) return run_.arena_count;
    const auto* length = content_->economy().ladder.Find(PacingLengthId());
    return length != nullptr ? length->arenas : run_.arena_count;
}

std::vector<LadderUnlock> RunController::LadderUnlocksFor(const std::vector<std::string>& fresh_keys) const {
    std::vector<LadderUnlock> out;
    const EconomyConfig::Ladder& ladder = content_->economy().ladder;
    if (!ladder.enabled || fresh_keys.empty()) return out;
    const auto fresh = [&fresh_keys](std::string_view key) {
        return std::find(fresh_keys.begin(), fresh_keys.end(), key) != fresh_keys.end();
    };
    for (const auto& length : ladder.lengths) {
        // The easy boss beaten for the first time: its alternates are a choice from now on.
        if (fresh(length.id + ":easy")) {
            for (const int hard : length.hard_bosses) out.push_back({LadderUnlock::Kind::HardBoss, length.id, length.arenas, hard});
        }
        if (!length.unlocked_by.empty() && fresh(length.unlocked_by) && LengthOpen(length)) {
            out.push_back({LadderUnlock::Kind::Length, length.id, length.arenas, length.easy_boss});
            // The length that can go on to the finale: how to earn it.
            if (FinalePossible(length)) out.push_back({LadderUnlock::Kind::Finale, length.id, length.arenas, ladder.finale.boss});
        }
    }
    // Endless opened by this run's win.
    if (!ladder.endless.id.empty() && !ladder.endless.unlocked_by.empty() && fresh(ladder.endless.unlocked_by) && EndlessOpen()) {
        out.push_back({LadderUnlock::Kind::Length, ladder.endless.id, 0, -1});
    }
    const std::string& exotic = content_->economy().refine.exotic_unlocked_by;
    if (!exotic.empty() && fresh(exotic) && MaxRefineRarity() == Rarity::Exotic) {
        out.push_back({LadderUnlock::Kind::Exotic, std::string(), 0, -1});
    }
    return out;
}

namespace {

// A per-kill amount that need not be whole: the k-th counted kill of an arena pays what takes the
// arena's total from (k - 1) to k kills' worth, so the arena pays kills x amount (its whole part)
// with no rounding per kill (2 x 1.75 paid 4 a kill; the user, 2026-11-11: the economy by the numbers).
std::int64_t KillShare(double per_kill, int k) {
    if (per_kill <= 0.0 || k <= 0) return 0;
    constexpr double kEps = 1e-9;
    return static_cast<std::int64_t>(std::floor(k * per_kill + kEps)) - static_cast<std::int64_t>(std::floor((k - 1) * per_kill + kEps));
}

// A refine's chance for this run: longer than the runs the odds are set for, the same odds are spread
// over its arenas (a long run ended with three or four legendary guns, 2026-11-09).
double RefineChance(const EconomyConfig::Refine& refine, double chance, int arena_count) {
    if (refine.chances_for_arenas <= 0 || arena_count <= refine.chances_for_arenas) return chance;
    return chance * static_cast<double>(refine.chances_for_arenas) / static_cast<double>(arena_count);
}

}  // namespace

double RefineShopChance(const EconomyConfig::Refine& refine, std::string_view length_id, int arena_count) {
    const auto own = refine.shop_chance_by_length.find(std::string(length_id));
    return own != refine.shop_chance_by_length.end() ? own->second : RefineChance(refine, refine.shop_chance, arena_count);
}

Rarity RunController::MaxRefineRarity() const {
    const EconomyConfig& eco = content_->economy();
    const auto& exotic = eco.weapon_rarity[RarityIndex(Rarity::Exotic)];
    const bool has_tier = exotic.magnitude_mult > 0.0 && (exotic.affix_count > 0 || exotic.element_count > 0 || exotic.unique_count > 0);
    return has_tier && !eco.refine.exotic_unlocked_by.empty() && Beaten(eco.refine.exotic_unlocked_by) ? Rarity::Exotic
                                                                                                        : Rarity::Legendary;
}

const WeaponInstance* RunController::RefineTarget(const std::optional<std::string>& preferred) const {
    const Rarity top = MaxRefineRarity();
    const WeaponInstance* best = nullptr;
    for (const WeaponInstance& w : run_.weapons) {
        if (RarityIndex(w.rarity) >= RarityIndex(top)) continue;
        if (preferred && w.base_id == *preferred) return &w;
        if (best == nullptr || RarityIndex(w.rarity) > RarityIndex(best->rarity) ||
            (w.rarity == best->rarity && w.level >= best->level)) {
            best = &w;
        }
    }
    return best;
}

// A reward's and the shop's refines go to any gun that can go up, as likely (2026-11-11, the user: "in
// my run the flamethrower never got a chance to get refinement, only upgrades": they went to the best
// gun, so one gun took every refine); a chest's to the gun in hand (RefineTarget).
const WeaponInstance* RunController::RandomRefineTarget(Rng& rng) const {
    const Rarity top = MaxRefineRarity();
    std::vector<const WeaponInstance*> open;
    for (const WeaponInstance& w : run_.weapons) {
        if (RarityIndex(w.rarity) < RarityIndex(top)) open.push_back(&w);
    }
    return open.empty() ? nullptr : open[rng.Index(open.size())];
}

void RunController::AddRefineReward(bool sure) {
    const EconomyConfig::Refine& refine = content_->economy().refine;
    if (!refine.enabled || run_.reward_offer.empty()) return;
    if (RefineTarget(std::nullopt) == nullptr) return;  // no gun can go up
    const double chance = RefineChance(refine, refine.reward_chance, PacingArenas());
    if (!sure && !(chance > 0.0 && streams_->reward.NextDouble() < chance)) return;
    const WeaponInstance* target = RandomRefineTarget(streams_->reward);
    if (target == nullptr) return;
    RewardChoice choice;
    choice.kind = RewardKind::Refine;
    choice.weapon = RefineWeapon(streams_->reward, *content_, caps_, *target, BuildElements());
    choice.rarity = choice.weapon.rarity;
    choice.target_instance_id = target->instance_id;
    run_.reward_offer.back() = std::move(choice);  // in place of the last choice: the offer keeps its size
}

void RunController::AddRefineOffer() {
    const EconomyConfig::Refine& refine = content_->economy().refine;
    if (!refine.enabled) return;
    const SkinBuild& build = run_.character.build;
    if (RefineTarget(std::nullopt) == nullptr) return;  // no gun can go up
    const double chance = RefineShopChance(refine, PacingLengthId(), PacingArenas());
    if (!build.refine_every_shop && !(chance > 0.0 && streams_->shop.NextDouble() < chance)) return;
    const WeaponInstance* target = RandomRefineTarget(streams_->shop);
    if (target == nullptr) return;
    ShopOffer o;
    o.kind = ShopOfferKind::Refine;
    o.weapon = RefineWeapon(streams_->shop, *content_, caps_, *target, BuildElements());
    o.rarity = o.weapon.rarity;
    o.target_instance_id = target->instance_id;
    o.price = static_cast<std::int64_t>(std::llround(static_cast<double>(refine.price[RarityIndex(o.rarity)]) * build.refine_price_mult));
    run_.shop_offers.push_back(std::move(o));
}

Status RunController::ApplyRefine(const std::string& instance_id, const WeaponInstance& refined, std::string_view source) {
    const auto it = std::find_if(run_.weapons.begin(), run_.weapons.end(),
                                 [&instance_id](const WeaponInstance& w) { return w.instance_id == instance_id; });
    if (it == run_.weapons.end()) return Status::Error(ErrorCode::GrantFailed, "weapon " + instance_id + " is no longer held");
    if (RarityIndex(refined.rarity) != RarityIndex(it->rarity) + 1) {
        return Status::Error(ErrorCode::GrantFailed, "weapon " + instance_id + " changed since the refine was offered");
    }
    const int level = it->level;
    const Rarity from = it->rarity;
    *it = refined;
    it->level = level;  // a refine never touches the level
    Event e = MakeEvent(EventType::WeaponRefined);
    e.Add("weapon", it->base_id).Add("weapon_instance", it->instance_id).Add("source", source).Add("refined_from", RarityName(from))
        .Add("rarity", RarityName(it->rarity)).Add("affixes", it->affixes.size()).Add("elements", it->elements.size());
    Emit(std::move(e));
    return Status::Success();
}

std::vector<RunController::LadderLengthView> RunController::LadderLengths() const {
    std::vector<LadderLengthView> out;
    const EconomyConfig::Ladder& ladder = content_->economy().ladder;
    if (!ladder.enabled) return out;
    for (const auto& length : ladder.lengths) {
        LadderLengthView v;
        v.id = length.id;
        v.name = length.name;
        v.open = LengthOpen(length);
        v.easy_beaten = Beaten(length.id + ":easy");
        v.hard_beaten = Beaten(length.id + ":hard");
        v.hard_open = !length.hard_bosses.empty() && v.easy_beaten;
        v.finale_possible = FinalePossible(length);
        v.arenas = length.arenas;
        v.unlocked_by = length.unlocked_by;
        out.push_back(std::move(v));
    }
    if (!ladder.endless.id.empty()) {
        LadderLengthView v;
        v.id = ladder.endless.id;
        v.name = ladder.endless.name;
        v.open = EndlessOpen();
        v.endless = true;
        v.boss_every = ladder.endless.boss_every;
        v.unlocked_by = ladder.endless.unlocked_by;
        out.push_back(std::move(v));
    }
    return out;
}

bool RunController::CanResumeRun() const {
    return !run_.active && profile_.active_run && !profile_.active_run->resume.empty();
}

Result<std::string> RunController::ResumeRun() {
    constexpr std::string_view op = "resume_run";
    if (run_.active) return Fail(op, ErrorCode::RunInProgress, "run " + run_.run_id + " is active");
    if (!CanResumeRun()) return Fail(op, ErrorCode::NoActiveRun, "no run to go on with");
    RunState r;
    const Status parsed = ParseRunResume(profile_.active_run->resume, r);
    if (!parsed.ok()) return Fail(op, parsed.code, parsed.message);
    // Each player's character as StartRun (AddSeat) made it: the skin, then the builds of their perks.
    const auto build = [this](RunState& view) -> bool {
        const CharacterDef* ch = content_->FindCharacter(view.character_id);
        const SkinDef* skin = ch != nullptr ? FindSkin(*ch, view.skin_id) : nullptr;
        if (ch == nullptr || skin == nullptr) return false;
        view.character = ApplySkin(*ch, *skin);
        for (const auto& [id, stacks] : view.perks) {
            const PerkDef* def = content_->FindPerk(id);
            if (def != nullptr && def->build) ApplyPerkBuild(view.character.build, *def->build);
        }
        return true;
    };
    for (std::size_t i = 0; i < std::max<std::size_t>(r.seats.size(), 1); ++i) {
        RunState other;
        if (i > 0) SwapSeat(other, r.seats[i]);
        RunState& view = i == 0 ? r : other;
        const bool ok = build(view);
        const std::string who = "the run's character '" + view.character_id + "' or skin '" + view.skin_id + "' is gone";
        if (i > 0) SwapSeat(other, r.seats[i]);
        if (!ok) return Fail(op, ErrorCode::UnknownId, who);
    }
    r.active = true;
    r.phase = r.arena_index == 0 ? Phase::Idle : Phase::RouteSelection;
    r.route_chosen = true;
    run_ = std::move(r);
    // Streams for the arena it goes on at: drawn anew (an arena played again rolls anew).
    const std::string at = "@" + std::to_string(run_.arena_index);
    auto derive = [&](std::string_view stream) { return Rng::Derive(run_.seed, std::string(stream) + at); };
    streams_.emplace(Streams{derive(streams::kReward), derive(streams::kRoute), derive(streams::kLoot), derive(streams::kShop),
                             derive(streams::kCombat), derive(streams::kLoadout), derive(streams::kDirector)});
    last_summary_.reset();
    Event e = MakeEvent(EventType::RunResumed);
    e.Add("character", run_.character_id).Add("arena_index", run_.arena_index).Add("arenas_cleared", static_cast<std::int64_t>(run_.results.size()));
    Emit(std::move(e));
    return profile_.active_run->resume_adapter;
}

Status RunController::SuspendRun(double now) {
    constexpr std::string_view op = "suspend_run";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (!profile_.active_run || profile_.active_run->run_id != run_.run_id || profile_.active_run->resume.empty()) {
        return Fail(op, ErrorCode::NotEligible, "run " + run_.run_id + " has no resume point");
    }
    Event e = MakeEvent(EventType::RunSuspended);
    e.Add("phase", PhaseName(run_.phase)).Add("arena_index", run_.arena_index).Add("time", now);
    Emit(std::move(e));
    // The run as it was at its resume point; this arena is played again.
    run_ = RunState{};
    streams_.reset();
    director_.reset();
    return SaveProfile("run_suspended");
}

// ---- meta progression ----------------------------------------------------------------------

CharacterGate RunController::SkinGate(const SkinDef& skin) const {
    const CharacterGate gate = CheckSkin(skin, profile_.unlocked_characters);
    if (gate == CharacterGate::Selectable && skin.mod && !installed_mod_skins_.count(skin.id)) return CharacterGate::NotInstalled;
    return gate;
}

std::vector<CharacterAvailability> RunController::Characters() const {
    std::vector<CharacterAvailability> out;
    for (const CharacterDef& c : content_->characters()) {
        if (c.tester_only && !tester_characters_) continue;
        CharacterAvailability a{c.id, c.name, CheckCharacter(&c, profile_.unlocked_characters), c.unlock_cost_credits,
                                c.unlock.text, CheckChallenge(c, profile_.stats), c.costume_index};
        for (const SkinDef& s : c.skins) {
            a.skins.push_back({s.id, s.name, s.costume_index, SkinGate(s), s.unlock.text, CheckChallenge(s.unlock, profile_.stats)});
        }
        // A character that is only mod skins (Leon) waits for one of its mods.
        const bool allMods = !c.skins.empty() && std::all_of(c.skins.begin(), c.skins.end(), [](const SkinDef& s) { return s.mod.has_value(); });
        if (allMods && a.gate == CharacterGate::Selectable &&
            std::none_of(a.skins.begin(), a.skins.end(), [](const SkinAvailability& s) { return s.gate == CharacterGate::Selectable; })) {
            a.gate = CharacterGate::NotInstalled;
        }
        // A character whose every skin is locked (Wesker, until a boss opens one of his) is locked
        // too, with his skins' text and no progress shown: nothing of him can be played yet.
        if (!a.skins.empty() && a.gate == CharacterGate::Selectable &&
            std::all_of(a.skins.begin(), a.skins.end(), [](const SkinAvailability& s) { return s.gate == CharacterGate::Locked; })) {
            a.gate = CharacterGate::Locked;
            a.unlock_text = a.skins.front().unlock_text;
            a.challenge = {};
        }
        if (!c.skins.empty() && c.skins.front().costume_index >= 0) a.costume_index = c.skins.front().costume_index;
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<std::string> RunController::EvaluateCharacterUnlocks() {
    std::vector<std::string> unlocked;
    auto consider = [&](const std::string& id, const Challenge& challenge, const char* kind) {
        if (challenge.kind == ChallengeKind::None || profile_.unlocked_characters.count(id)) return;
        const ChallengeProgress progress = CheckChallenge(challenge, profile_.stats);
        if (!progress.met()) return;
        profile_.unlocked_characters.insert(id);
        unlocked.push_back(id);
        Event e = MakeEvent(EventType::CharacterUnlocked);
        e.Add(kind, id).Add("challenge", std::string(ChallengeKindName(challenge.kind))).Add("target", progress.target);
        Emit(std::move(e));
    };
    for (const CharacterDef& c : content_->characters()) {
        if (!c.enabled) continue;
        consider(c.id, c.unlock, "character");
        for (const SkinDef& s : c.skins) consider(s.id, s.unlock, "skin");
    }
    return unlocked;
}

Result<UnlockReceipt> RunController::PurchaseUnlock(UnlockKind kind, std::string_view id) {
    constexpr std::string_view op = "purchase_unlock";
    if (run_.active) return Fail(op, ErrorCode::RunInProgress, "unlocks can only be bought between runs");
    std::int64_t cost = 0;
    bool enabled = false;
    StringSet* target = nullptr;
    switch (kind) {
        case UnlockKind::Character:
            if (const CharacterDef* d = content_->FindCharacter(id)) {
                cost = d->unlock_cost_credits;
                enabled = d->enabled;
                target = &profile_.unlocked_characters;
            }
            break;
        case UnlockKind::Perk:
            if (const PerkDef* d = content_->FindPerk(id)) {
                cost = d->unlock_cost_credits;
                enabled = d->enabled;
                target = &profile_.unlocked_perks;
            }
            break;
        case UnlockKind::Weapon:
            if (const WeaponBaseDef* d = content_->FindWeapon(id)) {
                cost = d->unlock_cost_credits;
                enabled = d->enabled;
                target = &profile_.unlocked_weapons;
            }
            break;
    }
    const std::string label = std::string(UnlockKindName(kind)) + " '" + std::string(id) + "'";
    if (target == nullptr) return Fail(op, ErrorCode::UnknownId, "unknown " + label);
    if (!enabled) return Fail(op, ErrorCode::Disabled, label + " is not available yet");
    if (cost <= 0) return Fail(op, ErrorCode::NotUnlockable, label + " is unlocked by default");
    if (target->find(id) != target->end()) return Fail(op, ErrorCode::AlreadyUnlocked, label + " is already unlocked");

    const std::string tx = "unlock:" + std::string(UnlockKindName(kind)) + ":" + std::string(id);
    const TxResult r = profile_.credits.Debit(tx, cost);
    if (r == TxResult::AlreadyApplied) {
        target->emplace(id);  // ledger says it was bought: repair the unlock set
        (void)SaveProfile("unlock_repair");
        return Fail(op, ErrorCode::AlreadyUnlocked, label + " was already purchased");
    }
    if (r == TxResult::InsufficientFunds) {
        return Fail(op, ErrorCode::InsufficientFunds,
                    label + " costs " + std::to_string(cost) + ", balance " + std::to_string(profile_.credits.balance()));
    }
    if (r != TxResult::Applied) return Fail(op, ErrorCode::InvalidAmount, std::string(TxResultName(r)));
    target->emplace(id);
    const Status saved = SaveProfile("unlock");
    if (!saved.ok()) {
        profile_.credits.Rollback(tx);
        target->erase(target->find(id));
        return Fail(op, ErrorCode::PersistenceFailed, "unlock rolled back: " + saved.message);
    }
    Event e = MakeEvent(EventType::UnlockPurchased);
    e.Add("kind", UnlockKindName(kind)).Add("id", id).Add("cost", cost).Add("balance", profile_.credits.balance());
    Emit(std::move(e));
    Event c = MakeEvent(EventType::CurrencyChanged);
    c.Add("currency", "credits").Add("delta", -cost).Add("balance", profile_.credits.balance()).Add("tx", tx)
        .Add("reason", "unlock");
    Emit(std::move(c));
    return UnlockReceipt{kind, std::string(id), cost, profile_.credits.balance()};
}

const std::optional<ActiveRunCheckpoint>& RunController::interrupted_run() const {
    return run_.active ? kNoCheckpoint : profile_.active_run;
}

Result<RunSummary> RunController::SettleCheckpoint(const ActiveRunCheckpoint& checkpoint, std::string_view reason) {
    const EconomyConfig& eco = content_->economy();
    const auto amount = static_cast<std::int64_t>(
        std::floor(static_cast<double>(checkpoint.credits_pending) * eco.credits.death_rate));
    RunSummary s;
    s.run_id = checkpoint.run_id;
    s.character_id = checkpoint.character_id;
    s.seed = checkpoint.seed;
    s.completed = false;
    s.end_reason = std::string(reason);
    s.arenas_cleared = checkpoint.arenas_cleared;
    s.total_kills = checkpoint.kills;
    s.credits_pending = checkpoint.credits_pending;
    const TxResult r = BankRunCredits(profile_, checkpoint.run_id, amount);
    s.already_banked = r == TxResult::AlreadyApplied;
    if (r == TxResult::Applied) {
        s.credits_banked = amount;
        ++profile_.stats.runs_failed;
        profile_.stats.best_arena = std::max<std::int64_t>(profile_.stats.best_arena, checkpoint.arenas_cleared);
        profile_.stats.total_kills += checkpoint.kills;
        profile_.stats.credits_earned += amount;
    }
    profile_.active_run.reset();
    (void)EvaluateCharacterUnlocks();
    (void)SaveProfile("settle_interrupted_run");
    Event e;
    e.type = EventType::RunEnded;
    e.run_id = checkpoint.run_id;
    e.Add("reason", reason).Add("completed", false).Add("credits_banked", s.credits_banked)
        .Add("already_banked", s.already_banked).Add("arenas_cleared", s.arenas_cleared);
    Emit(std::move(e));
    return s;
}

Result<RunSummary> RunController::RecoverInterruptedRun() {
    if (run_.active) return Fail("recover_run", ErrorCode::RunInProgress, "a run is active");
    if (!profile_.active_run) return Fail("recover_run", ErrorCode::NoActiveRun, "no interrupted run in the profile");
    const ActiveRunCheckpoint cp = *profile_.active_run;
    return SettleCheckpoint(cp, "interrupted");
}

// ---- run lifecycle -------------------------------------------------------------------------

namespace {

// Spends the loadout budget: one of the character's starting weapons (by category cost),
// then random affordable equipment until nothing fits.
Loadout RollLoadout(const CharacterDef& ch, const Content& content, Rng& rng) {
    const LoadoutConfig& cfg = content.loadouts();
    Loadout out;
    out.budget = cfg.budget;
    if (ch.starting_weapons.empty()) return out;
    // Without a budget there is no roll: the first starting weapon is the sidearm, as before.
    const auto pick = cfg.budget > 0
                          ? static_cast<std::size_t>(rng.UniformInt(0, static_cast<std::int64_t>(ch.starting_weapons.size()) - 1))
                          : std::size_t{0};
    out.weapon_id = ch.starting_weapons[pick];
    std::int64_t cost = cfg.default_weapon_cost;
    if (const WeaponBaseDef* def = content.FindWeapon(out.weapon_id)) {
        const auto it = cfg.weapon_cost.find(WeaponCategoryName(def->category));
        if (it != cfg.weapon_cost.end()) cost = it->second;
    }
    out.spent = std::min(cost, out.budget);
    std::vector<int> counts(cfg.equipment.size(), 0);
    int distinct = 0;
    for (int guard = 0; guard < 64; ++guard) {
        std::vector<std::size_t> options;
        for (std::size_t i = 0; i < cfg.equipment.size(); ++i) {
            const EquipmentDef& e = cfg.equipment[i];
            if (counts[i] >= e.max_count || out.spent + e.cost > out.budget) continue;
            if (!e.for_weapons.empty()) continue;  // a launcher's rounds: no launcher at the start
            // The inventory has few free slots: past the cap only top up what was already picked.
            if (cfg.max_items > 0 && counts[i] == 0 && distinct >= cfg.max_items) continue;
            options.push_back(i);
        }
        if (options.empty()) break;
        const std::size_t i =
            options[static_cast<std::size_t>(rng.UniformInt(0, static_cast<std::int64_t>(options.size()) - 1))];
        if (counts[i] == 0) ++distinct;
        ++counts[i];
        out.spent += cfg.equipment[i].cost;
    }
    for (std::size_t i = 0; i < cfg.equipment.size(); ++i) {
        if (counts[i] > 0) {
            out.items.push_back({cfg.equipment[i].id, cfg.equipment[i].name, counts[i], cfg.equipment[i].game_item});
        }
    }
    return out;
}

}  // namespace

std::string FormatLoadout(const Loadout& loadout, const Content& content) {
    std::string text;
    if (const WeaponBaseDef* def = content.FindWeapon(loadout.weapon_id)) {
        text = def->name;
    } else {
        text = loadout.weapon_id;
    }
    for (const LoadoutItem& item : loadout.items) {
        text += ", " + item.name;
        if (item.count > 1) text += " x" + std::to_string(item.count);
    }
    return text;
}

Status RunController::StartRun(std::string_view character_id, std::uint64_t seed) {
    return StartRun(character_id, seed, std::string_view());
}

Status RunController::StartRun(std::string_view character_id, std::uint64_t seed, std::string_view skin_id) {
    return StartRun(character_id, seed, skin_id, std::string_view());
}

Status RunController::StartRun(std::string_view character_id, std::uint64_t seed, std::string_view skin_id,
                               std::string_view length_id) {
    constexpr std::string_view op = "start_run";
    if (run_.active) return Fail(op, ErrorCode::RunInProgress, "run " + run_.run_id + " has not ended");
    const CharacterDef* ch = nullptr;
    const SkinDef* skin = nullptr;
    if (Status s = CheckPlayable(op, character_id, skin_id, ch, skin); !s.ok()) return s;
    const EconomyConfig& eco = content_->economy();
    // The ladder's length (EconomyConfig::Ladder): the first one unless named.
    const EconomyConfig::Ladder::Length* length = nullptr;
    const bool endless = eco.ladder.enabled && eco.ladder.endless.Is(length_id);
    if (endless) {
        if (!EndlessOpen()) return Fail(op, ErrorCode::Locked, "run length '" + eco.ladder.endless.id + "' is locked");
    } else if (eco.ladder.enabled) {
        length = length_id.empty() ? &eco.ladder.lengths.front() : eco.ladder.Find(length_id);
        if (length == nullptr) return Fail(op, ErrorCode::UnknownId, "unknown run length '" + std::string(length_id) + "'");
        if (!LengthOpen(*length)) return Fail(op, ErrorCode::Locked, "run length '" + length->id + "' is locked");
    }
    const RouteDef* start = content_->FindRoute(eco.run.starting_route);
    if (start == nullptr || !start->enabled || start->enemy_budget <= 0) {
        return Fail(op, ErrorCode::InvalidEncounter, "starting route '" + eco.run.starting_route + "' is not playable");
    }
    if (profile_.active_run) (void)SettleCheckpoint(*profile_.active_run, "interrupted");

    RunState fresh;
    fresh.active = true;
    fresh.run_id = RunIdFor(profile_.next_run_number);
    fresh.seed = seed;
    fresh.phase = Phase::Idle;
    // The finale's arena is added when the run earns it (CompleteArena).
    // Endless: its map reaches its first boss; it grows as the run goes (ExtendEndless).
    fresh.arena_count = endless ? std::max(1, eco.ladder.endless.boss_every) : length != nullptr ? length->arenas : eco.run.arena_count;
    fresh.length_id = endless ? eco.ladder.endless.id : length != nullptr ? length->id : std::string();
    fresh.route_id = start->id;
    fresh.route_chosen = true;  // the first arena uses the starting route
    run_ = std::move(fresh);
    streams_.emplace(Streams{
        Rng::Derive(seed, streams::kReward),
        Rng::Derive(seed, streams::kRoute),
        Rng::Derive(seed, streams::kLoot),
        Rng::Derive(seed, streams::kShop),
        Rng::Derive(seed, streams::kCombat),
        Rng::Derive(seed, streams::kLoadout),
        Rng::Derive(seed, streams::kDirector),
    });
    SetUpSeat(*ch, *skin);
    run_.keys = eco.keys.enabled ? eco.keys.start : 0;
    if (endless) {
        run_.run_map = EndlessMap(run_.arena_count, streams_->route);
    } else if (length != nullptr) {
        // Every shorter length's bosses at their arenas, its own at its end (each its easy boss,
        // then its alternates too once the easy one is beaten); the finale's arena after them,
        // played only when the run earns it.
        RunMapPlan plan;
        plan.layers = run_.arena_count + (FinalePossible(*length) ? 1 : 0);
        for (const auto* tier : eco.ladder.BossTiers(length->id)) {
            if (tier != length) plan.earlier_bosses[tier->arenas] = BossChoices(*tier);
        }
        plan.bosses = BossChoices(*length);
        plan.finale = FinalePossible(*length) ? eco.ladder.finale.boss : -1;
        run_.run_map = GenerateRunMap(*content_, streams_->route, arena_pool_, first_arena_, plan);
    } else {
        run_.run_map = GenerateRunMap(*content_, streams_->route, arena_pool_, first_arena_);
    }
    // Its own stream: planning the guests never shifts the run map or anything else.
    Rng rosters = Rng::Derive(seed, streams::kRoster);
    PlanRunRosters(*content_, rosters, run_.run_map);
    last_summary_.reset();
    ++profile_.next_run_number;
    ++profile_.stats.runs_started;

    Event e = MakeEvent(EventType::RunStarted);
    e.Add("character", ch->id).Add("seed", std::to_string(seed)).Add("rng_version", kRngVersion)
        .Add("arena_count", run_.arena_count).Add("route", start->id).Add("map", PlannedMapKey())
        .Add("capabilities", caps_.ToString())
        .Add("weapon_mode", WeaponIdMode() ? std::string_view("by_weapon_id") : kSidearmSlot)
        .Add("skin", skin->id).Add("length", run_.length_id).Add("loadout_weapon", run_.loadout.weapon_id)
        .Add("loadout", FormatLoadout(run_.loadout, *content_)).Add("loadout_spent", run_.loadout.spent);
    Emit(std::move(e));
    EquipSeat(*ch);
    UpdateCheckpoint();
    (void)SaveProfile("start_run");
    return Status::Success();
}

Status RunController::CheckPlayable(std::string_view op, std::string_view character_id, std::string_view skin_id,
                                    const CharacterDef*& ch, const SkinDef*& skin) const {
    ch = content_->FindCharacter(character_id);
    CharacterGate gate = CheckCharacter(ch, profile_.unlocked_characters);
    if (ch != nullptr && ch->tester_only && !tester_characters_) gate = CharacterGate::Disabled;  // not on this install
    switch (gate) {
        case CharacterGate::Unknown:
            return Fail(op, ErrorCode::UnknownId, "unknown character '" + std::string(character_id) + "'");
        case CharacterGate::Disabled:
            return Fail(op, ErrorCode::Disabled, "character '" + std::string(character_id) + "' is not available yet");
        case CharacterGate::Locked:
            return Fail(op, ErrorCode::Locked, "character '" + std::string(character_id) + "' is locked");
        case CharacterGate::Selectable:
        case CharacterGate::NotInstalled: break;  // a skin's gate only
    }
    skin = FindSkin(*ch, skin_id);
    if (skin == nullptr) {
        return Fail(op, ErrorCode::UnknownId, "unknown skin '" + std::string(skin_id) + "' for " + ch->id);
    }
    switch (SkinGate(*skin)) {
        case CharacterGate::Locked: return Fail(op, ErrorCode::Locked, "skin '" + skin->id + "' is locked");
        case CharacterGate::NotInstalled:
            return Fail(op, ErrorCode::NotEligible, "skin '" + skin->id + "' needs its mod (" + skin->mod->name + ")");
        default: break;
    }
    return Status::Success();
}

void RunController::SetUpSeat(const CharacterDef& ch, const SkinDef& skin) {
    run_.character_id = ch.id;
    run_.skin_id = skin.id;
    run_.character = ApplySkin(ch, skin);
    run_.loadout = RollLoadout(run_.character, *content_, streams_->loadout);
}

void RunController::EquipSeat(const CharacterDef& character) {
    const CharacterDef* ch = &character;
    const EconomyConfig& eco = content_->economy();
    // Unspent loadout points become supplies (the run's starting supplies once, with the first seat).
    const std::int64_t starting_supplies = (run_.active_seat == 0 ? eco.run.starting_supplies : 0) +
                                           std::max<std::int64_t>(run_.loadout.budget - run_.loadout.spent, 0);
    const std::string start_tx = SeatTx(run_, "start");
    if (run_.supplies.Credit(start_tx, starting_supplies) == TxResult::Applied && starting_supplies > 0) {
        Event c = MakeEvent(EventType::CurrencyChanged);
        c.Add("currency", "supplies").Add("delta", starting_supplies).Add("balance", run_.supplies.balance())
            .Add("tx", start_tx).Add("reason", "starting_supplies");
        Emit(std::move(c));
    }
    // Only the rolled weapon (the pistol): every other gun comes from supply perks, rewards or
    // the shop, so the run's weapon list matches what is really in the player's hands.
    if (!run_.loadout.weapon_id.empty()) {
        GrantWeapon(MakeStartingWeapon(run_.loadout.weapon_id, NextInstanceId()), "starting_weapon");
    }
    // The skin's build may hand over a weapon of its own (a rolled rarity, e.g. a legendary).
    const SkinBuild& build = run_.character.build;
    if (!build.starting_weapon.empty() && build.starting_weapon != run_.loadout.weapon_id) {
        const Rarity rarity = ParseRarity(build.starting_weapon_rarity).value_or(Rarity::Common);
        WeaponInstance w = rarity == Rarity::Common
                               ? MakeStartingWeapon(build.starting_weapon, NextInstanceId())
                               : GenerateWeapon(streams_->loot, *content_, caps_, build.starting_weapon, rarity, NextInstanceId(),
                                                BuildElements());
        // Queued, not placed at once: the adapter first cuts the game's loadout down to the gun in
        // hand (which may be this very gun) and then hands the queue over.
        const WeaponBaseDef* base = content_->FindWeapon(w.base_id);
        if (base != nullptr && base->game_item >= 0 && caps_.Has(cap::kInventory)) {
            run_.item_grants.push_back(ItemGrant{base->game_item, 1, "build"});
        }
        GrantWeapon(std::move(w), "build");
    }
    // More of the build's guns (the tester's Gatling and rocket launcher), at common rarity, queued
    // the same way.
    for (const std::string& id : build.more_starting_weapons) {
        if (id == run_.loadout.weapon_id || id == build.starting_weapon) continue;
        WeaponInstance w = MakeStartingWeapon(id, NextInstanceId());
        const WeaponBaseDef* base = content_->FindWeapon(w.base_id);
        if (base != nullptr && base->game_item >= 0 && caps_.Has(cap::kInventory)) {
            run_.item_grants.push_back(ItemGrant{base->game_item, 1, "build"});
        }
        GrantWeapon(std::move(w), "build");
    }
    // The menu's choice, one perk per category (the style slot falls back to the character's
    // own trait); the skin's trait is granted separately below.
    for (const std::string& id : EquippedPerks(ch->id)) {
        if (id == run_.character.starting_passive) continue;  // granted as the starting passive below
        (void)GrantPerk(id, "equipped", CursePolicy::Exclude, true);
    }
    if (!run_.character.starting_passive.empty()) {
        const Status s = GrantPerk(run_.character.starting_passive, "starting_passive", CursePolicy::Exclude, true);
        if (!s.ok()) {
            Event w = MakeEvent(EventType::Error, EventLevel::Info);
            w.Add("operation", "starting_passive").Add("code", ErrorCodeName(s.code)).Add("message", s.message);
            Emit(std::move(w));
        }
    }
    // A perk's rule with its cost changes the run's build (Boulder Puncher, Superior Being).
    for (const auto& [id, stacks] : run_.perks) {
        const PerkDef* def = content_->FindPerk(id);
        if (def != nullptr && def->build) ApplyPerkBuild(run_.character.build, *def->build);
    }
}

// ---- local co-op (ROADMAP M10) -------------------------------------------------------------

int RunController::SeatCount() const {
    if (!run_.active) return 0;
    return run_.seats.empty() ? 1 : static_cast<int>(run_.seats.size());
}

Status RunController::ActivateSeat(int seat) {
    if (seat == run_.active_seat) return Status::Success();
    const int count = run_.seats.empty() ? 1 : static_cast<int>(run_.seats.size());
    if (seat < 0 || seat >= count) return Status::Error(ErrorCode::InvalidIndex, "no seat " + std::to_string(seat));
    SwapSeat(run_, run_.seats[static_cast<std::size_t>(run_.active_seat)]);  // the active seat waits
    SwapSeat(run_, run_.seats[static_cast<std::size_t>(seat)]);              // `seat` comes in
    run_.active_seat = seat;
    return Status::Success();
}

Status RunController::AddSeat(std::string_view character_id, std::string_view skin_id) {
    constexpr std::string_view op = "add_seat";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::Idle || run_.arena_index != 0) {
        return Fail(op, ErrorCode::WrongPhase, "a player joins before the run's first arena");
    }
    const int count = SeatCount();
    if (count >= content_->economy().coop.max_players) {
        return Fail(op, ErrorCode::NotEligible, "the run has " + std::to_string(count) + " players already");
    }
    const CharacterDef* ch = nullptr;
    const SkinDef* skin = nullptr;
    if (Status s = CheckPlayable(op, character_id, skin_id, ch, skin); !s.ok()) return s;
    // Two outfits in one game costume slot: the slot is one file, and a mod skin replaces it.
    for (int i = 0; i < count; ++i) {
        const bool active = i == run_.active_seat;
        const std::string& other_character = active ? run_.character_id : run_.seats[static_cast<std::size_t>(i)].character_id;
        const std::string& other_skin = active ? run_.skin_id : run_.seats[static_cast<std::size_t>(i)].skin_id;
        const CharacterDef* oc = content_->FindCharacter(other_character);
        const SkinDef* os = oc != nullptr ? FindSkin(*oc, other_skin) : nullptr;
        if (os != nullptr && os->id != skin->id && os->costume_index == skin->costume_index &&
            (os->mod.has_value() || skin->mod.has_value())) {
            return Fail(op, ErrorCode::NotEligible,
                        "skins '" + os->id + "' and '" + skin->id + "' need the same game costume slot " + std::to_string(skin->costume_index));
        }
    }
    if (run_.seats.empty()) {
        run_.seats.resize(1);  // seat 0, the active one: its state is in the run's fields
        run_.seat_status.resize(1);
    }
    run_.seats.emplace_back();
    run_.seat_status.emplace_back();
    const int seat = count;
    const int previous = run_.active_seat;
    (void)ActivateSeat(seat);
    SetUpSeat(*ch, *skin);
    Event e = MakeEvent(EventType::PlayerJoined);
    e.Add("character", ch->id).Add("skin", skin->id).Add("loadout_weapon", run_.loadout.weapon_id)
        .Add("loadout", FormatLoadout(run_.loadout, *content_)).Add("loadout_spent", run_.loadout.spent);
    Emit(std::move(e));
    EquipSeat(*ch);
    (void)ActivateSeat(previous);
    UpdateCheckpoint();
    (void)SaveProfile("add_seat");
    return Status::Success();
}

bool RunController::SeatDown(int seat) const {
    return seat >= 0 && static_cast<std::size_t>(seat) < run_.seat_status.size() &&
           run_.seat_status[static_cast<std::size_t>(seat)].down;
}

std::vector<int> RunController::ConsumeRevivedSeats() {
    std::vector<int> out;
    for (std::size_t i = 0; i < run_.seat_status.size(); ++i) {
        if (!run_.seat_status[i].revive) continue;
        run_.seat_status[i].revive = false;
        out.push_back(static_cast<int>(i));
    }
    return out;
}

RunState::SeatStatus* RunController::ActiveSeatStatus() {
    const auto i = static_cast<std::size_t>(run_.active_seat);
    return i < run_.seat_status.size() ? &run_.seat_status[i] : nullptr;
}

bool RunController::EverySeatChoseReward() const {
    if (!run_.reward_chosen) return false;
    for (std::size_t i = 0; i < run_.seats.size(); ++i) {
        if (static_cast<int>(i) != run_.active_seat && !run_.seats[i].reward_chosen) return false;
    }
    return true;
}

bool RunController::EverySeatLeftShop() const {
    for (const RunState::SeatStatus& s : run_.seat_status) {
        if (!s.shop_done) return false;
    }
    return true;
}

Status RunController::AbandonRun(double now) {
    if (!run_.active) return Fail("abandon_run", ErrorCode::NoActiveRun, "no active run");
    if (run_.phase == Phase::RunComplete || run_.phase == Phase::RunFailed) {
        return Fail("abandon_run", ErrorCode::WrongPhase, "run already finished; call EndRun");
    }
    FailRun(now, "abandoned");
    return Status::Success();
}

void RunController::FailRun(double now, std::string_view reason) {
    if (run_.phase == Phase::ArenaActive) {
        if (ArenaMetrics* a = CurrentArena()) {
            a->end_time = now;
            if (reason == "died") ++a->deaths;
            const RouteDef* route = Route();
            const double mult = route != nullptr ? route->credit_mult : 1.0;
            const std::int64_t partial = RoundAmount(
                static_cast<double>(content_->economy().credits.per_kill * a->player_kills) * mult);
            a->credits_accrued += partial;
            run_.credits_pending += partial;
        }
    }
    run_.phase = Phase::RunFailed;
    run_.end_reason = std::string(reason);
    ForEachSeat([this](int) {
        run_.reward_offer.clear();
        run_.shop_offers.clear();
    });
    run_.route_choices.clear();
    UpdateCheckpoint();
    (void)SaveProfile("run_failed");
}

Status RunController::OnPlayerDied(double now) {
    if (!run_.active) return Fail("player_died", ErrorCode::NoActiveRun, "no active run");
    if (run_.phase == Phase::RunComplete || run_.phase == Phase::RunFailed) {
        return Fail("player_died", ErrorCode::WrongPhase, "run already finished");
    }
    if (SeatCount() > 1) {
        RunState::SeatStatus* status = ActiveSeatStatus();
        if (status != nullptr && !status->down) {
            status->down = true;
            if (run_.phase == Phase::ArenaActive) {
                if (ArenaMetrics* a = CurrentArena()) ++a->deaths;
            }
            Event d = MakeEvent(EventType::PlayerDamaged);
            d.Add("down", true);
            Emit(std::move(d));
        }
        bool everyone = true;
        for (const RunState::SeatStatus& s : run_.seat_status) everyone = everyone && s.down;
        if (!everyone) {
            UpdateCheckpoint();
            return Status::Success();
        }
    }
    FailRun(now, "died");
    Event e = MakeEvent(EventType::PlayerDamaged);
    e.Add("died", true).Add("pending_credits", run_.credits_pending);
    Emit(std::move(e));
    return Status::Success();
}

Status RunController::BeginArena(double now) {
    constexpr std::string_view op = "begin_arena";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    const bool first = run_.phase == Phase::Idle && run_.arena_index == 0;
    const bool next = run_.phase == Phase::RouteSelection && run_.route_chosen;
    if (!first && !next) {
        return Fail(op, ErrorCode::WrongPhase,
                    "BeginArena needs a fresh run or a chosen route (phase " + std::string(PhaseName(run_.phase)) + ")");
    }
    if (run_.arena_index >= run_.arena_count) return Fail(op, ErrorCode::WrongPhase, "all arenas already played");
    const RouteDef* route = Route();
    if (route == nullptr || !route->enabled || route->enemy_budget <= 0) {
        return Fail(op, ErrorCode::InvalidEncounter, "route '" + run_.route_id + "' is not playable");
    }
    ++run_.arena_index;
    const EconomyConfig::Ladder& ladder = content_->economy().ladder;
    const EconomyConfig::Coop& coop = content_->economy().coop;
    const bool together = SeatCount() > 1;
    int budget = route->enemy_budget + ladder.budget_per_arena * ladder.Steps(run_.arena_index);
    if (together) budget = std::max(1, static_cast<int>(std::lround(static_cast<double>(budget) * coop.enemy_mult)));
    ArenaMetrics m;
    m.arena = run_.arena_index;
    m.route_id = route->id;
    m.budget = budget;
    m.start_time = now;
    run_.arenas.push_back(std::move(m));
    run_.seen_enemy_ids.clear();
    ForEachSeat([this](int) {
        run_.kill_records.clear();
        run_.last_melee_time.reset();
        // A build with arena_element: every gun carries one random element this arena.
        run_.arena_element = -1;
        if (run_.character.build.arena_element && !content_->elements().empty() && streams_) {
            const auto& elements = content_->elements();
            run_.arena_element = static_cast<int>(elements[streams_->loot.Index(elements.size())].element);
        }
        run_.reward_offer.clear();
        run_.reward_chosen = false;
        run_.shop_offers.clear();
    });
    // A player who went down in the last arena is back (the adapter sets their health).
    for (RunState::SeatStatus& s : run_.seat_status) {
        if (s.down) s.revive = true;
        s.down = false;
        s.shop_done = false;
    }
    run_.clamp_logged = false;
    run_.route_choices.clear();
    run_.route_chosen = false;
    run_.phase = Phase::ArenaActive;

    // The director runs the arena when the adapter can spawn and the map is known.
    director_.reset();
    boss_phases_used_ = 0;
    boss_add_kills_ = 0;
    std::string guests;
    if (caps_.Has(cap::kDirector)) {
        if (const std::optional<MapDef> map = ArenaMap(map_stage_)) {
            for (const MapEnemy& e : map->roster) {
                if (e.home >= 0) guests += (guests.empty() ? "" : ",") + e.unit_class;
            }
            ArenaThreat threat = content_->economy().enemy_threat.ForArena(run_.arena_index, route->difficulty_tier);
            threat.boss_hp_mult = BossHpMult(map_stage_) * (together ? coop.boss_hp_mult : 1.0);
            // Elites, once the profile holds their unlock (the 12th arena's boss beaten).
            const EconomyConfig::Elites& el = content_->economy().elites;
            const int arenaNumber = run_.arena_index + 1;
            if (el.chance > 0.0 && !el.elements.empty() && profile_.stats.bosses_beaten.count(el.unlock) != 0 && arenaNumber >= el.from_arena) {
                threat.elite_chance = std::min(el.max_chance, el.chance + el.chance_per_arena * (arenaNumber - el.from_arena));
                threat.elite_elements = el.elements;
            }
            MapDef arena = *map;
            if (together) {
                // Two players: more of them at once too (the map's cap and the arena's threat cap).
                auto scaled = [&](int n) { return std::clamp(static_cast<int>(std::lround(n * coop.alive_mult)), 1, 40); };
                arena.max_alive = scaled(arena.max_alive);
                if (threat.max_alive > 0) threat.max_alive = scaled(threat.max_alive);
            }
            director_.emplace(arena, budget, route->difficulty_tier, streams_->director.Next(), threat);
            director_->PickChests(content_->economy().chests.per_arena + run_.character.build.extra_chests);
            run_.arenas.back().budget = director_->budget();  // a boss arena: the boss alone
        }
    }

    Event e = MakeEvent(EventType::ArenaStarted);
    e.Add("route", route->id).Add("tier", route->difficulty_tier).Add("budget", run_.arenas.back().budget)
        .Add("threat", director_ ? director_->threat_level() : 0).Add("threat_held_to", director_ ? director_->pick_level() : 0)
        .Add("boss", director_ && director_->map().boss ? director_->map().boss->unit_class : std::string())
        .Add("guests", guests)
        .Add("par_time_seconds", route->modifiers.par_time_seconds)
        .Add("incoming_mult", route->modifiers.incoming_damage_mult)
        .Add("outgoing_mult", route->modifiers.outgoing_damage_mult).Add("now", now);
    Emit(std::move(e));
    return Status::Success();
}

KillOutcome RunController::OnEnemyKilled(const KillEvent& kill) {
    KillOutcome out;
    if (!run_.active || run_.phase != Phase::ArenaActive) {
        out.ignored = true;
        Event e = MakeEvent(EventType::EnemyKilled, EventLevel::Debug);
        e.Add("ignored", true).Add("by_player", kill.by_player).Add("phase", PhaseName(run_.phase));
        Emit(std::move(e));
        return out;
    }
    ArenaMetrics& a = *CurrentArena();
    const EconomyConfig& eco = content_->economy();
    const RouteDef* route = Route();
    const double reward_mult = route != nullptr ? route->reward_mult : 1.0;

    // With the director, only its own units exist and count; anything else is ignored.
    // Director ids are unique per spawn and counted once by the director itself, so they skip
    // the seen-id check (the engine reuses unit memory, and the adapter's ids name it).
    // A boss's add (MapBoss phases) is the director's but not part of the budget: the arena clears on the boss.
    const bool add = director_ && kill.enemy_id && director_->IsAdd(*kill.enemy_id);
    const bool directorUnit = director_ && kill.enemy_id && director_->Died(*kill.enemy_id);
    if (director_ && !directorUnit) {
        out.ignored = true;
        out.remaining = RemainingEnemies();
        Event e = MakeEvent(EventType::EnemyKilled, EventLevel::Debug);
        e.Add("ignored", true).Add("reason", "not a director unit");
        if (kill.enemy_id) e.Add("enemy_id", *kill.enemy_id);
        Emit(std::move(e));
        return out;
    }
    if (!directorUnit && kill.enemy_id && !run_.seen_enemy_ids.insert(*kill.enemy_id).second) {
        ++a.duplicate_kills;
        out.duplicate = true;
        out.remaining = RemainingEnemies();
        Event e = MakeEvent(EventType::EnemyKilled, EventLevel::Debug);
        e.Add("duplicate", true).Add("enemy_id", *kill.enemy_id);
        Emit(std::move(e));
        return out;
    }
    if (!kill.by_player) ++a.non_player_kills;
    const bool counts = kill.by_player || eco.run.non_player_kills_count_toward_budget || director_.has_value();
    if (counts) {
        const bool within_budget = add || a.budget_kills < a.budget;
        const int kill_index = add ? ++boss_add_kills_ : ++a.budget_kills;
        out.counted = true;
        // Every player kill feeds combat state (kill streaks, after-melee); only kills inside
        // the budget pay out supplies/heals so stragglers cannot be farmed.
        const WeaponInstance* weapon = kill.by_player ? ApplicableWeapon(kill.weapon_id, kill.was_melee) : nullptr;
        if (kill.by_player) {
            ++a.player_kills;
            if (kill.was_melee.value_or(false)) run_.last_melee_time = kill.now;
            ApplyBuildKill(kill, out);
            AddKillProcs(kill, weapon, out);

            // Kill-streak record (+ legendary window extensions from the killing weapon).
            double extend = 0.0;
            if (weapon != nullptr) {
                for (const Affix& af : weapon->affixes) {
                    if (af.kind == AffixKind::UniqueKillStreakExtend) {
                        extend += af.value;
                        out.triggered.push_back(af.unique_id);
                    }
                }
            }
            double longest = 0.0;
            for (const auto& [id, stacks] : run_.perks) {
                const PerkDef* def = content_->FindPerk(id);
                if (def == nullptr) continue;
                for (const EffectDef& ef : def->effects) {
                    if (ef.kind == EffectKind::OutgoingDamageMult && ef.condition == ConditionKind::KillStreak) {
                        longest = std::max(longest, ef.AtStacks(stacks).window_seconds);
                    }
                }
            }
            if (extend > 0.0) {
                for (KillRecord& r : run_.kill_records) {
                    if (kill.now < r.time + longest + r.extra_seconds) r.extra_seconds += extend;
                }
            }
            run_.kill_records.push_back(KillRecord{kill.now, extend});
            const double keep = std::max(longest, 1.0);
            run_.kill_records.erase(
                std::remove_if(run_.kill_records.begin(), run_.kill_records.end(),
                               [&](const KillRecord& r) { return kill.now >= r.time + keep + r.extra_seconds + 60.0; }),
                run_.kill_records.end());
            if (run_.kill_records.size() > 512) {
                run_.kill_records.erase(run_.kill_records.begin(),
                                        run_.kill_records.begin() + static_cast<std::ptrdiff_t>(run_.kill_records.size() - 512));
            }
        }
        if (within_budget) {  // counted kills pay out (non-player ones only when the economy says so)
            const std::string kill_tx = (add ? "add" : "kill") + std::to_string(kill_index);
            // Every per-kill amount is exact (KillShare), the build's multiplier in it.
            const double build_mult = run_.character.build.supplies_mult * (1.0 + run_.stats.supplies);
            const double elite_mult = kill.elite ? eco.elites.supplies_mult : 1.0;  // an elite pays more
            const std::int64_t base = KillShare(eco.supplies.per_kill * reward_mult * build_mult * elite_mult, kill_index);
            if (CreditSupplies(ArenaTx(a.arena, kill_tx + ":base"), base, "kill", false)) out.supplies_awarded += base;

            for (const auto& [id, stacks] : run_.perks) {
                const PerkDef* def = content_->FindPerk(id);
                if (def == nullptr) continue;
                for (const EffectDef& ef : def->effects) {
                    const EffectParams p = ef.AtStacks(stacks);
                    if (ef.kind == EffectKind::HealOnKill && caps_.Has(cap::kHeal)) {
                        run_.heal_requests.push_back(HealRequest{p.amount_fraction_of_max, id});
                        out.heal_fraction_queued += p.amount_fraction_of_max;
                        out.triggered.push_back(id);
                    } else if (ef.kind == EffectKind::SuppliesOnKill) {
                        if (ef.melee_only && !kill.was_melee.value_or(false)) continue;
                        const bool hit = p.chance >= 1.0 || streams_->loot.NextDouble() < p.chance;
                        if (!hit) continue;
                        const std::int64_t amount = KillShare(p.amount * build_mult, kill_index);
                        if (CreditSupplies(ArenaTx(a.arena, kill_tx + ":perk:" + id), amount, id, false)) {
                            out.supplies_awarded += amount;
                            out.triggered.push_back(id);
                        }
                    } else if (ef.kind == EffectKind::ItemOnKill && caps_.Has(cap::kInventory)) {
                        if (ef.melee_only && !kill.was_melee.value_or(false)) continue;
                        const int every = std::max(1, static_cast<int>(std::lround(p.every_kills)));
                        int& kills = run_.perk_kill_counts[id];
                        if (++kills % every != 0) continue;
                        if (p.chance > 0.0 && p.chance < 1.0 && streams_->loot.NextDouble() >= p.chance) continue;
                        run_.item_grants.push_back(
                            ItemGrant{ItemAsGameItem(run_.character.build, static_cast<std::int64_t>(std::lround(p.game_item))),
                                      std::max(1, static_cast<int>(std::lround(p.amount))), id});
                        out.triggered.push_back(id);
                    }
                }
            }
            if (weapon != nullptr) {
                double per_kill = 0.0;
                for (const Affix& af : weapon->affixes) {
                    if (af.kind == AffixKind::SuppliesOnKill) per_kill += af.value;
                }
                // Its supplies by level (weapons.json per_level supplies_per_kill: the pistols).
                if (const WeaponBaseDef* gun = content_->FindWeapon(weapon->base_id)) {
                    per_kill += gun->per_level.supplies_per_kill * std::max(0, weapon->level - 1);
                }
                const std::int64_t amount = KillShare(per_kill * build_mult, kill_index);
                if (amount > 0 && CreditSupplies(ArenaTx(a.arena, kill_tx + ":weapon"), amount, weapon->instance_id, false)) {
                    out.supplies_awarded += amount;
                }
            }
            for (const std::string& id : out.triggered) {
                Event t = MakeEvent(EventType::PerkTriggered, EventLevel::Debug);
                t.Add("source", id).Add("trigger", "kill");
                Emit(std::move(t));
            }
        }
    }
    out.remaining = RemainingEnemies();
    out.arena_complete = IsArenaComplete();
    Event e = MakeEvent(EventType::EnemyKilled);
    e.Add("by_player", kill.by_player).Add("counted", out.counted).Add("remaining", out.remaining)
        .Add("supplies", out.supplies_awarded).Add("now", kill.now);
    if (kill.weapon_id) e.Add("weapon_id", *kill.weapon_id);
    if (kill.was_melee) e.Add("melee", *kill.was_melee);
    if (kill.enemy_id) e.Add("enemy_id", *kill.enemy_id);
    if (out.heal_fraction_queued > 0.0) e.Add("heal_queued", out.heal_fraction_queued);
    Emit(std::move(e));
    return out;
}

void RunController::OnPlayerDamaged(const DamageEvent& damage) {
    if (damage.hp_max > 0.0) {
        run_.player_hp = damage.hp_after;
        run_.player_hp_max = damage.hp_max;
    }
    if (!run_.active || run_.phase != Phase::ArenaActive) {
        Event e = MakeEvent(EventType::PlayerDamaged, EventLevel::Debug);
        e.Add("ignored", true).Add("amount", damage.amount);
        Emit(std::move(e));
        return;
    }
    ArenaMetrics& a = *CurrentArena();
    ++a.damage_events;
    if (damage.amount > 0.0) a.damage_taken += damage.amount;
    a.hp_max_seen = std::max(a.hp_max_seen, damage.hp_max);
    Event e = MakeEvent(EventType::PlayerDamaged);
    e.Add("amount", damage.amount).Add("hp_before", damage.hp_before).Add("hp_after", damage.hp_after)
        .Add("hp_max", damage.hp_max).Add("now", damage.now);
    Emit(std::move(e));
}

double RunController::PendingHeal() const {
    double total = 0.0;
    for (const HealRequest& h : run_.heal_requests) total += h.fraction_of_max;
    return std::min(total, 1.0);
}

// The game item behind a weapon instance, when the adapter can place it: through the port at
// once (fails when nothing fits), else queued. Weapons without a game id stay run-only.
Status RunController::PlaceWeaponItem(const WeaponInstance& weapon, std::string_view source) {
    const WeaponBaseDef* base = content_->FindWeapon(weapon.base_id);
    if (base == nullptr || base->game_item < 0 || !caps_.Has(cap::kInventory)) return Status::Success();
    if (inventory_ != nullptr) {
        if (inventory_->Give(base->game_item, 1, source) <= 0) {
            return Status::Error(ErrorCode::GrantFailed, "inventory full: " + base->name + " did not fit");
        }
    } else {
        run_.item_grants.push_back(ItemGrant{base->game_item, 1, std::string(source)});
    }
    Event e = MakeEvent(EventType::ItemGranted);
    e.Add("weapon", base->id).Add("game_item", base->game_item).Add("source", source);
    Emit(std::move(e));
    return Status::Success();
}

Status RunController::GrantItem(std::string_view equipment_id, int count, std::string_view source) {
    const EquipmentDef* def = content_->loadouts().FindEquipment(equipment_id);
    if (def == nullptr) return Status::Error(ErrorCode::NotEligible, "unknown item " + std::string(equipment_id));
    {  // the build's own item in its place (items_as: Safari Chris's bombs), from wherever it comes
        OfferContext ctx;
        ctx.content = content_.get();
        ctx.build = &run_.character.build;
        def = ItemAs(ctx, def);
    }
    if (def->game_item < 0) return Status::Error(ErrorCode::NotEligible, "item " + def->id + " has no game item");
    if (!caps_.Has(cap::kInventory)) return Status::Error(ErrorCode::NotEligible, "inventory capability is not available");
    int placed = std::max(1, count);
    if (inventory_ != nullptr) {
        placed = inventory_->Give(def->game_item, placed, source);
        if (placed <= 0) return Status::Error(ErrorCode::GrantFailed, "inventory full: " + def->name + " did not fit");
    } else {
        run_.item_grants.push_back(ItemGrant{def->game_item, placed, std::string(source)});
    }
    Event e = MakeEvent(EventType::ItemGranted);
    e.Add("item", def->id).Add("count", placed).Add("source", source);
    Emit(std::move(e));
    return Status::Success();
}

namespace {
bool SelectableCategory(PerkCategory c) {
    return c == PerkCategory::Style || c == PerkCategory::Supply || c == PerkCategory::Support;
}
}  // namespace

Status RunController::EquipPerk(std::string_view character_id, std::string_view perk_id) {
    const CharacterDef* ch = content_->FindCharacter(character_id);
    if (ch == nullptr) return Status::Error(ErrorCode::UnknownId, "unknown character '" + std::string(character_id) + "'");
    const PerkDef* def = content_->FindPerk(perk_id);
    if (def == nullptr) return Status::Error(ErrorCode::UnknownId, "unknown perk '" + std::string(perk_id) + "'");
    if (!def->enabled || !SelectableCategory(def->category)) {
        return Status::Error(ErrorCode::NotEligible, "perk '" + def->id + "' cannot be equipped");
    }
    if (!CharacterMayUsePerk(*def, *ch)) {
        return Status::Error(ErrorCode::NotEligible, "perk '" + def->id + "' is for another character");
    }
    if (def->unlock_cost_credits > 0 && profile_.unlocked_perks.count(def->id) == 0) {
        return Status::Error(ErrorCode::NotEligible, "perk '" + def->id + "' is locked");
    }
    std::vector<std::string>& slots = profile_.equipped_perks[ch->id];
    std::erase_if(slots, [&](const std::string& id) {
        const PerkDef* other = content_->FindPerk(id);
        return other == nullptr || other->category == def->category;
    });
    slots.push_back(def->id);
    Event e = MakeEvent(EventType::PerkEquipped);
    e.Add("character", ch->id).Add("perk", def->id).Add("category", PerkCategoryName(def->category));
    Emit(std::move(e));
    return SaveProfile("equip");
}

Status RunController::UnequipPerk(std::string_view character_id, PerkCategory category) {
    const CharacterDef* ch = content_->FindCharacter(character_id);
    if (ch == nullptr) return Status::Error(ErrorCode::UnknownId, "unknown character '" + std::string(character_id) + "'");
    const auto it = profile_.equipped_perks.find(ch->id);
    if (it == profile_.equipped_perks.end()) return Status::Success();
    const auto before = it->second.size();
    std::erase_if(it->second, [&](const std::string& id) {
        const PerkDef* other = content_->FindPerk(id);
        return other == nullptr || other->category == category;
    });
    if (it->second.size() == before) return Status::Success();
    if (it->second.empty()) profile_.equipped_perks.erase(it);
    Event e = MakeEvent(EventType::PerkEquipped);
    e.Add("character", ch->id).Add("category", PerkCategoryName(category)).Add("cleared", true);
    Emit(std::move(e));
    return SaveProfile("equip");
}

const PerkDef* RunController::EquippedPerk(std::string_view character_id, PerkCategory category) const {
    const auto it = profile_.equipped_perks.find(std::string(character_id));
    if (it != profile_.equipped_perks.end()) {
        for (const std::string& id : it->second) {
            const PerkDef* def = content_->FindPerk(id);
            if (def != nullptr && def->category == category) return def;
        }
    }
    if (category == PerkCategory::Style) {
        if (const CharacterDef* ch = content_->FindCharacter(character_id)) {
            if (!ch->starting_passive.empty()) return content_->FindPerk(ch->starting_passive);
        }
    }
    return nullptr;
}

std::vector<std::string> RunController::EquippedPerks(std::string_view character_id) const {
    std::vector<std::string> out;
    for (PerkCategory c : {PerkCategory::Style, PerkCategory::Supply, PerkCategory::Support}) {
        if (const PerkDef* def = EquippedPerk(character_id, c)) out.push_back(def->id);
    }
    return out;
}

std::vector<ItemGrant> RunController::ConsumeItemGrants() {
    std::vector<ItemGrant> out;
    out.swap(run_.item_grants);
    return out;
}

std::vector<HealRequest> RunController::ConsumeHealRequests() {
    std::vector<HealRequest> out;
    out.swap(run_.heal_requests);
    return out;
}

bool RunController::IsArenaComplete() const {
    if (!run_.active || run_.phase != Phase::ArenaActive) return false;
    const ArenaMetrics* a = CurrentArena();
    return a != nullptr && a->budget_kills >= a->budget;
}

BossAddSource FindBossAdd(const Content& content, const std::string& unit_class) {
    BossAddSource out;
    out.enemy.unit_class = unit_class;
    auto entryOn = [&unit_class](const MapDef& m) -> const MapEnemy* {
        for (const MapEnemy& e : m.roster) {
            if (e.unit_class == unit_class) return &e;
        }
        return nullptr;
    };
    const auto& guests = content.economy().run_rosters.guests;
    if (const auto g = guests.find(unit_class); g != guests.end()) {
        if (const MapDef* m = content.FindMapByStage(g->second)) {
            if (const MapEnemy* e = entryOn(*m)) {
                out.enemy = *e;
                out.home = m;
            }
        }
    }
    for (const MapDef& m : content.maps()) {
        if (out.home != nullptr) break;
        if (m.boss && m.boss->unit_class == unit_class) {
            out.home = &m;
            out.boss = true;
            out.hp = m.boss->hp;
        } else if (const MapEnemy* e = entryOn(m)) {
            out.enemy = *e;
            out.home = &m;
        }
    }
    // As an add it is no special of its map and no guest.
    out.enemy.special = false;
    out.enemy.max_alive = 0;
    out.enemy.home = -1;
    return out;
}

std::optional<RunController::BossPhaseStart> RunController::BossWouldDie(double now) {
    if (!run_.active || run_.phase != Phase::ArenaActive || !director_ || !director_->map().boss) return std::nullopt;
    const MapBoss& boss = *director_->map().boss;
    if (boss_phases_used_ >= static_cast<int>(boss.phases.size())) return std::nullopt;
    BossPhaseStart s;
    s.index = boss_phases_used_++;
    s.count = static_cast<int>(boss.phases.size());
    s.phase = boss.phases[static_cast<std::size_t>(s.index)];
    s.hp = std::max(1, static_cast<int>(std::lround(director_->BossHp() * std::clamp(s.phase.hp_fraction, 0.01, 1.0))));
    std::vector<Director::AddSpec> adds;
    std::string names;
    for (const BossAdd& a : s.phase.adds) {
        const BossAddSource source = FindBossAdd(*content_, a.unit_class);
        for (int i = 0; i < a.count; ++i) adds.push_back(Director::AddSpec{source.enemy, source.hp});
        names += (names.empty() ? "" : " ") + a.unit_class + "x" + std::to_string(a.count);
    }
    director_->AddWave(std::move(adds), now + s.phase.down_seconds);
    Event e = MakeEvent(EventType::BossPhase);
    e.Add("boss", boss.unit_class).Add("phase", s.index + 1).Add("of", s.count).Add("hp", s.hp).Add("adds", names)
        .Add("down_seconds", s.phase.down_seconds).Add("now", now);
    Emit(std::move(e));
    return s;
}

int RunController::BossPhasesLeft() const {
    if (!run_.active || run_.phase != Phase::ArenaActive || !director_ || !director_->map().boss) return 0;
    return std::max(0, static_cast<int>(director_->map().boss->phases.size()) - boss_phases_used_);
}

int RunController::RemainingEnemies() const {
    if (!run_.active || run_.phase != Phase::ArenaActive) return 0;
    const ArenaMetrics* a = CurrentArena();
    return a == nullptr ? 0 : std::max(0, a->budget - a->budget_kills);
}

Result<ArenaResult> RunController::CompleteArena(double now) {
    constexpr std::string_view op = "complete_arena";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::ArenaActive) {
        return Fail(op, ErrorCode::WrongPhase, "phase is " + std::string(PhaseName(run_.phase)));
    }
    if (!IsArenaComplete()) {
        return Fail(op, ErrorCode::ArenaNotComplete, std::to_string(RemainingEnemies()) + " enemies remain");
    }
    ArenaMetrics& a = *CurrentArena();
    const RouteDef& route = *Route();
    const EconomyConfig& eco = content_->economy();
    a.end_time = now;
    a.completed = true;
    const double elapsed = std::max(0.0, now - a.start_time);
    const double hp_max = a.hp_max_seen > 0.0 ? a.hp_max_seen : run_.player_hp_max.value_or(0.0);
    const Rating rating = ComputeRating(eco.rating, elapsed, route.modifiers.par_time_seconds, a.damage_taken, hp_max,
                                        a.deaths);

    CreditSupplies(ArenaTx(a.arena, "clear"),
                   RoundAmount(static_cast<double>(eco.supplies.per_arena_clear) * route.reward_mult), "arena_clear");
    ForEachSeat([&](int) {
        for (const auto& [id, stacks] : run_.perks) {
            const PerkDef* def = content_->FindPerk(id);
            if (def == nullptr) continue;
            for (const EffectDef& ef : def->effects) {
                if (ef.kind == EffectKind::ItemOnArenaClear && caps_.Has(cap::kInventory)) {
                    const EffectParams p = ef.AtStacks(stacks);
                    run_.item_grants.push_back(ItemGrant{ItemAsGameItem(run_.character.build, static_cast<std::int64_t>(std::lround(p.game_item))),
                                                         std::max(1, static_cast<int>(std::lround(p.amount))), id});
                    Event t = MakeEvent(EventType::PerkTriggered);
                    t.Add("source", id).Add("trigger", "arena_clear");
                    Emit(std::move(t));
                    continue;
                }
                if (ef.kind != EffectKind::SuppliesOnArenaClear) continue;
                if (ef.require_no_damage && a.damage_taken > 0.0) continue;
                if (CreditSupplies(ArenaTx(a.arena, "clear:perk:" + id), RoundAmount(ef.AtStacks(stacks).amount), id)) {
                    Event t = MakeEvent(EventType::PerkTriggered);
                    t.Add("source", id).Add("trigger", "arena_clear");
                    Emit(std::move(t));
                }
            }
        }
    });

    const double credits = (static_cast<double>(eco.credits.per_kill * a.player_kills) +
                            static_cast<double>(eco.credits.per_arena_clear) * eco.credits.rating_mult[RatingIndex(rating)]) *
                           route.credit_mult;
    const std::int64_t accrued = RoundAmount(credits);
    a.credits_accrued += accrued;
    run_.credits_pending += accrued;

    ArenaResult result;
    result.arena = a.arena;
    result.route_id = a.route_id;
    result.time_seconds = elapsed;
    result.kills = a.player_kills;
    result.damage_taken = a.damage_taken;
    result.no_damage = a.damage_taken <= 0.0;
    result.deaths = a.deaths;
    result.rating = rating;
    result.supplies_awarded = a.supplies_awarded;
    result.credits_accrued = a.credits_accrued;
    if (const MapDef* map = content_->FindMapByStage(map_stage_)) {
        result.arena_name = map->name;
        result.boss = map->boss.has_value();
        if (result.boss) RecordBossBeaten(map_stage_);
    }
    // The run's last boss beaten after the needs_hard bosses: the finale's arena follows.
    const EconomyConfig::Ladder& ladder = eco.ladder;
    if (const auto* length = ladder.enabled ? ladder.Find(run_.length_id) : nullptr;
        length != nullptr && result.boss && run_.arena_index == length->arenas && run_.arena_count == length->arenas &&
        FinalePossible(*length) && FinaleEarned() && run_.run_map.layers.size() > static_cast<std::size_t>(length->arenas)) {
        run_.arena_count = length->arenas + 1;
        Event f = MakeEvent(EventType::PerkTriggered);
        f.Add("source", "ladder").Add("trigger", "finale_earned").Add("map", ladder.finale.boss);
        Emit(std::move(f));
    }
    // Endless: its last arena (a boss) cleared, the map reaches boss_every arenas further.
    if (EndlessRun() && run_.arena_index >= run_.arena_count) ExtendEndless();
    run_.results.push_back(result);

    // Each seat's own offer (its perks, weapons and build decide what it may get).
    const auto sure_rating = ParseRating(eco.refine.reward_rating);
    const double rated_chance = RefineChance(eco.refine, eco.refine.reward_rated_chance, PacingArenas());
    ForEachSeat([&](int) {
        run_.reward_rarity_shift = route.rarity_bonus + eco.rewards.rating_rarity_bonus[RatingIndex(rating)];
        // A refine among the rewards now and then; likelier after a top-rated arena. Decided here,
        // once an arena: a reroll of the offer keeps it.
        run_.reward_refine_sure = sure_rating && RatingIndex(rating) <= RatingIndex(*sure_rating) &&
                                  (rated_chance >= 1.0 || streams_->reward.NextDouble() < rated_chance);
        // The last arena offers nothing: there is nothing left to use a reward on.
        PruneLostWeapons();
        run_.reward_offer.clear();
        if (run_.arena_index < run_.arena_count) {
            run_.reward_offer = GenerateRewardOffer(streams_->reward, MakeOfferContext(run_.reward_rarity_shift),
                                                    [this] { return NextInstanceId(); });
            AddRefineReward(run_.reward_refine_sure);
        }
        run_.reward_chosen = false;
    });
    run_.phase = Phase::ArenaResults;
    UpdateCheckpoint();
    (void)SaveProfile("arena_completed");

    Event e = MakeEvent(EventType::ArenaCompleted);
    e.Add("route", a.route_id).Add("time_seconds", elapsed).Add("kills", a.player_kills)
        .Add("non_player_kills", a.non_player_kills).Add("damage_taken", a.damage_taken)
        .Add("no_damage", result.no_damage).Add("rating", RatingName(rating)).Add("supplies", a.supplies_awarded)
        .Add("credits", accrued).Add("reward_choices", run_.reward_offer.size());
    Emit(std::move(e));
    return result;
}

Status RunController::ContinueToRewards() {
    if (!run_.active) return Fail("continue_to_rewards", ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::ArenaResults) {
        return Fail("continue_to_rewards", ErrorCode::WrongPhase, "phase is " + std::string(PhaseName(run_.phase)));
    }
    if (run_.arena_index >= run_.arena_count) {
        CompleteRun();
        return Status::Success();
    }
    run_.phase = Phase::RewardSelection;
    return Status::Success();
}

void RunController::CompleteRun() {
    run_.phase = Phase::RunComplete;
    run_.end_reason = "complete";
    UpdateCheckpoint();
    (void)SaveProfile("run_complete");
}

Status RunController::RerollReward() {
    constexpr std::string_view op = "reroll_reward";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::RewardSelection || run_.reward_chosen) {
        return Fail(op, ErrorCode::WrongPhase, "reroll is only possible before choosing a reward");
    }
    if (run_.reroll_tokens <= 0) return Fail(op, ErrorCode::NoRerollTokens, "no reroll tokens");
    --run_.reroll_tokens;
    PruneLostWeapons();
    run_.reward_offer = GenerateRewardOffer(streams_->reward, MakeOfferContext(run_.reward_rarity_shift),
                                            [this] { return NextInstanceId(); });
    AddRefineReward(run_.reward_refine_sure);
    Event e = MakeEvent(EventType::RewardRerolled);
    e.Add("tokens_left", run_.reroll_tokens).Add("choices", run_.reward_offer.size());
    Emit(std::move(e));
    return Status::Success();
}

Status RunController::ChooseReward(std::size_t index) {
    constexpr std::string_view op = "choose_reward";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.reward_chosen) {
        return Fail(op, ErrorCode::AlreadyChosen, "reward for arena " + std::to_string(run_.arena_index) + " already chosen");
    }
    if (run_.phase != Phase::RewardSelection) {
        return Fail(op, ErrorCode::WrongPhase, "phase is " + std::string(PhaseName(run_.phase)));
    }
    if (index >= run_.reward_offer.size()) return Fail(op, ErrorCode::InvalidIndex, "no reward at index " + std::to_string(index));
    const RewardChoice choice = run_.reward_offer[index];
    std::int64_t converted_supplies = 0;
    switch (choice.kind) {
        case RewardKind::Perk: {
            const Status s = GrantPerk(choice.perk_id, "reward", CursePolicy::Exclude, false);
            if (!s.ok()) return Fail(op, ErrorCode::GrantFailed, s.message);
            break;
        }
        case RewardKind::Weapon: {
            const Status placed = PlaceWeaponItem(choice.weapon, "reward");
            if (!placed.ok()) {
                // No room for the gun: what selling it pays (half its shop price) instead.
                converted_supplies = content_->economy().shop.weapon_price[RarityIndex(choice.rarity)] / 2;
                if (!CreditSupplies(ArenaTx(run_.arena_index, "reward"), converted_supplies, "reward", false)) {
                    return Fail(op, ErrorCode::GrantFailed, placed.message);
                }
            } else {
                GrantWeapon(choice.weapon, "reward");
            }
            break;
        }
        case RewardKind::Supplies:
            if (!CreditSupplies(ArenaTx(run_.arena_index, "reward"), choice.supplies, "reward", false)) {
                return Fail(op, ErrorCode::GrantFailed, "supplies reward could not be credited");
            }
            break;
        case RewardKind::Refine: {
            const Status s = ApplyRefine(choice.target_instance_id, choice.weapon, "reward");
            if (!s.ok()) return Fail(op, ErrorCode::GrantFailed, s.message);
            break;
        }
        case RewardKind::Item: {
            const Status s = GrantItem(choice.item_id, choice.item_count, "reward");
            if (!s.ok()) {
                // A full inventory must never block the run: the item's shop value is paid in
                // supplies instead (the adapter shows why).
                const EquipmentDef* def = content_->loadouts().FindEquipment(choice.item_id);
                if (s.code != ErrorCode::GrantFailed || def == nullptr) return Fail(op, ErrorCode::GrantFailed, s.message);
                converted_supplies = def->price * std::max(1, choice.item_count);
                if (!CreditSupplies(ArenaTx(run_.arena_index, "reward"), converted_supplies, "reward", false)) {
                    return Fail(op, ErrorCode::GrantFailed, s.message);
                }
            }
            break;
        }
    }
    run_.reward_chosen = true;
    Event e = MakeEvent(EventType::RewardSelected);
    e.Add("index", index).Add("kind", RewardKindName(choice.kind)).Add("rarity", RarityName(choice.rarity));
    if (choice.kind == RewardKind::Perk) e.Add("perk", choice.perk_id);
    if (choice.kind == RewardKind::Item) e.Add("item", choice.item_id).Add("count", choice.item_count);
    if (converted_supplies > 0) e.Add("inventory_full_supplies", converted_supplies);
    if (choice.kind == RewardKind::Weapon) e.Add("weapon", choice.weapon.base_id).Add("weapon_instance", choice.weapon.instance_id);
    if (choice.kind == RewardKind::Supplies) e.Add("supplies", choice.supplies);
    if (choice.kind == RewardKind::Refine) e.Add("weapon_instance", choice.target_instance_id);
    Emit(std::move(e));

    if (!EverySeatChoseReward()) return Status::Success();  // the other players still choose
    if (run_.arena_index >= run_.arena_count) {
        CompleteRun();
    } else {
        EnterShop();
    }
    return Status::Success();
}

void RunController::EnterShop() {
    const RouteDef* route = Route();
    ForEachSeat([&](int) {
        PruneLostWeapons();
        run_.shop_offers = GenerateShopOffers(streams_->shop, MakeOfferContext(route != nullptr ? route->rarity_bonus : 0.0),
                                              run_.weapons, run_.reroll_tokens, [this] { return NextInstanceId(); });
        AddRefineOffer();
        run_.free_shop_rerolls = run_.character.build.free_shop_rerolls;
        run_.shop_restocks = 0;
        run_.shop_generation = 0;
    });
    for (RunState::SeatStatus& s : run_.seat_status) s.shop_done = false;
    run_.phase = Phase::Shop;
    Event e = MakeEvent(EventType::ShopEntered);
    e.Add("offers", run_.shop_offers.size()).Add("supplies", run_.supplies.balance());
    Emit(std::move(e));
}

double* RunStats::Field(std::string_view stat) {
    if (stat == "damage") return &damage;
    if (stat == "defense") return &defense;
    if (stat == "max_health") return &max_health;
    if (stat == "crit") return &crit;
    if (stat == "move_speed") return &move_speed;
    if (stat == "heal") return &heal;
    if (stat == "supplies") return &supplies;
    return nullptr;
}

double RunStats::Get(std::string_view stat) const {
    RunStats copy = *this;
    const double* field = copy.Field(stat);
    return field != nullptr ? *field : 0.0;
}

void RunController::ApplyBuildKill(const KillEvent& kill, KillOutcome& out) {
    const SkinBuild& build = run_.character.build;
    // A melee kill adds a random stat for the rest of the run (Excella): one below its cap (in a
    // stable order, so a seed rolls the same), up to the cap. "Below" leaves out a hair under it:
    // steps that add up just short of the cap (0.05 ten times) left a stat open for a gain of nothing.
    constexpr double kCapTolerance = 1e-9;
    if (kill.was_melee.value_or(false) && !build.melee_kill_stats.empty()) {
        // The run length's pace, when the build names one: a stat fills its cap in that many gains
        // (Excella, the user 2026-11-11: full by mid arena 3 of a short run, arena 7 of a long one).
        const auto pace = build.melee_kill_gains_by_length.find(PacingLengthId());
        const auto step = [&](const std::string& stat, double amount) {
            if (pace == build.melee_kill_gains_by_length.end()) return amount;
            const auto gains = pace->second.find(stat);
            const auto cap = build.melee_kill_caps.find(stat);
            return gains != pace->second.end() && cap != build.melee_kill_caps.end() && gains->second > 0 ? cap->second / gains->second : amount;
        };
        std::vector<std::pair<std::string, double>> open;
        for (const auto& [stat, amount] : build.melee_kill_stats) {
            const auto cap = build.melee_kill_caps.find(stat);
            if (cap == build.melee_kill_caps.end() || run_.stats.Get(stat) < cap->second - kCapTolerance) open.emplace_back(stat, step(stat, amount));
        }
        std::sort(open.begin(), open.end());
        if (!open.empty()) {
            const auto& [stat, amount] = open[streams_->loot.Index(open.size())];
            double* field = run_.stats.Field(stat);
            const auto cap = build.melee_kill_caps.find(stat);
            const double before = field != nullptr ? *field : 0.0;
            if (field != nullptr) *field = cap != build.melee_kill_caps.end() ? std::min(cap->second, before + amount) : before + amount;
            ++run_.stats.gains;
            out.stat_gained = stat;
            out.stat_amount = field != nullptr ? *field - before : 0.0;
            Event e = MakeEvent(EventType::PerkTriggered);
            e.Add("source", "build").Add("trigger", "melee_kill_stat").Add("stat", stat).Add("amount", out.stat_amount)
                .Add("total", field != nullptr ? *field : 0.0).Add("gains", run_.stats.gains);
            Emit(std::move(e));
        }
    }
    // An item every so many kills (Safari Chris: a bomb every 8; the user, 2026-11-23: "some way to generate mines"),
    // counted with the perks' every_kills (saved with the run).
    if (build.kill_item >= 0 && build.kill_item_every > 0 && caps_.Has(cap::kInventory)) {
        int& kills = run_.perk_kill_counts["build:kill_item"];
        if (++kills % build.kill_item_every == 0) {
            const int count = std::max(1, build.kill_item_amount);
            run_.item_grants.push_back(ItemGrant{build.kill_item, count, "build"});
            Event e = MakeEvent(EventType::PerkTriggered);
            e.Add("source", "build").Add("trigger", "kill_item").Add("game_item", build.kill_item).Add("count", count);
            Emit(std::move(e));
        }
    }
    // Keys from the listed enemy classes.
    const EconomyConfig::Keys& keys = content_->economy().keys;
    if (keys.enabled && kill.enemy_class && run_.keys < keys.max && keys.drop_chance > 0.0) {
        const bool listed = std::any_of(keys.classes.begin(), keys.classes.end(), [&kill](const std::string& prefix) {
            return kill.enemy_class->rfind(prefix, 0) == 0;
        });
        if (listed && streams_->loot.NextDouble() < keys.drop_chance) {
            ++run_.keys;
            out.key_dropped = true;
            Event e = MakeEvent(EventType::PerkTriggered);
            e.Add("source", "keys").Add("trigger", "key_drop").Add("keys", run_.keys).Add("class", *kill.enemy_class);
            Emit(std::move(e));
        }
    }
}

HealOutcome RunController::OnPlayerHealed(const HealContext& heal) {
    HealOutcome out;
    out.amount = heal.amount;
    if (!run_.active || heal.amount <= 0.0) return out;
    const SkinBuild& build = run_.character.build;
    out.amount = heal.amount * build.heal_mult * (1.0 + run_.stats.heal);
    if (build.heal_buff_fraction > 0.0 && heal.hp_max > 0.0 && heal.amount >= build.heal_buff_fraction * heal.hp_max) {
        out.high_tier = true;
        out.invulnerable_seconds = build.heal_invulnerable_seconds;
        out.damage_bonus = build.heal_damage_bonus;
        out.damage_seconds = build.heal_damage_seconds;
        if (out.invulnerable_seconds > 0.0) run_.invulnerable_until = std::max(run_.invulnerable_until, heal.now + out.invulnerable_seconds);
        if (out.damage_seconds > 0.0 && out.damage_bonus > 0.0) {
            run_.damage_buff = out.damage_bonus;
            run_.damage_buff_until = heal.now + out.damage_seconds;
        }
    }
    if (out.amount != heal.amount || out.high_tier) {
        Event e = MakeEvent(EventType::PerkTriggered);
        e.Add("source", "build").Add("trigger", "heal").Add("game_heal", heal.amount).Add("heal", out.amount)
            .Add("high_tier", out.high_tier).Add("hp_max", heal.hp_max);
        Emit(std::move(e));
    }
    return out;
}

SpecialState RunController::Special(double now, int slot) const {
    SpecialState s;
    if (slot < 0 || slot > 1) return s;
    const SkinSpecial& sp = run_.character.build.Special(slot);
    const RunState::SpecialUse& use = run_.specials[static_cast<std::size_t>(slot)];
    if (!run_.active || !sp.Any()) return s;
    s.kind = sp.kind;
    s.name = sp.name;
    s.icon = sp.icon;
    s.cooldown_total = use.cooldown > 0.0 ? use.cooldown : sp.cooldown_seconds;
    s.cooldown_left = std::max(0.0, use.ready_at - now);
    if (sp.every_arenas > 0 && use.used_arena > 0) {
        s.arenas_left = std::max(0, use.used_arena + sp.every_arenas - run_.arena_index);
    }
    s.ready = run_.phase == Phase::ArenaActive && s.cooldown_left <= 0.0 && s.arenas_left == 0;
    return s;
}

Status RunController::UseSpecial(double now, double cooldown_scale, int slot) {
    constexpr std::string_view op = "use_special";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (slot < 0 || slot > 1) return Fail(op, ErrorCode::InvalidArgument, "no special slot " + std::to_string(slot));
    const SkinSpecial& sp = run_.character.build.Special(slot);
    RunState::SpecialUse& use = run_.specials[static_cast<std::size_t>(slot)];
    if (!sp.Any()) return Fail(op, ErrorCode::NotEligible, "this build has no special");
    if (run_.phase != Phase::ArenaActive) return Fail(op, ErrorCode::WrongPhase, "phase is " + std::string(PhaseName(run_.phase)));
    const SpecialState state = Special(now, slot);
    if (state.cooldown_left > 0.0) return Fail(op, ErrorCode::NotEligible, sp.name + " is cooling down");
    if (state.arenas_left > 0) return Fail(op, ErrorCode::NotEligible, sp.name + " is used up for this arena");
    if (!(cooldown_scale > 0.0) || cooldown_scale > 100.0) return Fail(op, ErrorCode::InvalidArgument, "cooldown scale out of range");
    use.cooldown = sp.cooldown_seconds * cooldown_scale;
    use.ready_at = now + use.cooldown;
    use.used_arena = run_.arena_index;
    ++use.uses;
    if ((sp.kind == "dash" || sp.kind == "burst") && sp.duration_seconds > 0.0) {
        run_.invulnerable_until = std::max(run_.invulnerable_until, now + sp.duration_seconds);
    }
    Event e = MakeEvent(EventType::PerkTriggered);
    e.Add("source", "build").Add("trigger", "special").Add("kind", sp.kind).Add("slot", slot).Add("uses", use.uses)
        .Add("arena", run_.arena_index).Add("cooldown", use.cooldown).Add("now", now);
    Emit(std::move(e));
    return Status::Success();
}

std::int64_t RunController::ShopRestockPrice() const {
    if (!run_.active || run_.phase != Phase::Shop) return -1;
    if (run_.free_shop_rerolls > 0) return 0;
    const auto& shop = content_->economy().shop;
    if (shop.restock_price <= 0) return -1;
    return shop.restock_price + shop.restock_price_step * run_.shop_restocks;
}

Status RunController::RerollShop() {
    constexpr std::string_view op = "reroll_shop";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::Shop) return Fail(op, ErrorCode::WrongPhase, "phase is " + std::string(PhaseName(run_.phase)));
    if (const RunState::SeatStatus* st = ActiveSeatStatus(); st != nullptr && st->shop_done) {
        return Fail(op, ErrorCode::WrongPhase, "this player left the shop");
    }
    const std::int64_t price = ShopRestockPrice();
    if (price < 0) return Fail(op, ErrorCode::NotEligible, "no new stock at this shop");
    if (price > 0) {
        if (run_.supplies.balance() < price) {
            return Fail(op, ErrorCode::InsufficientFunds,
                        "price " + std::to_string(price) + ", balance " + std::to_string(run_.supplies.balance()));
        }
        const TxResult debit = DebitSupplies(ArenaTx(run_.arena_index, "restock" + std::to_string(run_.shop_restocks + 1)), price,
                                             "restock");
        if (debit != TxResult::Applied) return Fail(op, ErrorCode::InsufficientFunds, std::string(TxResultName(debit)));
        ++run_.shop_restocks;
    } else {
        --run_.free_shop_rerolls;
    }
    PruneLostWeapons();
    const RouteDef* route = Route();
    run_.shop_offers = GenerateShopOffers(streams_->shop, MakeOfferContext(route != nullptr ? route->rarity_bonus : 0.0),
                                          run_.weapons, run_.reroll_tokens, [this] { return NextInstanceId(); });
    AddRefineOffer();
    ++run_.shop_generation;
    Event e = MakeEvent(EventType::ShopEntered);
    e.Add("offers", run_.shop_offers.size()).Add("supplies", run_.supplies.balance()).Add("reroll", run_.shop_generation)
        .Add("free_rerolls_left", run_.free_shop_rerolls).Add("price", price);
    Emit(std::move(e));
    return Status::Success();
}

Status RunController::Purchase(std::size_t index) {
    constexpr std::string_view op = "purchase";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::Shop) return Fail(op, ErrorCode::WrongPhase, "phase is " + std::string(PhaseName(run_.phase)));
    if (const RunState::SeatStatus* st = ActiveSeatStatus(); st != nullptr && st->shop_done) {
        return Fail(op, ErrorCode::WrongPhase, "this player left the shop");
    }
    if (index >= run_.shop_offers.size()) return Fail(op, ErrorCode::InvalidIndex, "no offer at index " + std::to_string(index));
    ShopOffer& offer = run_.shop_offers[index];
    if (offer.purchased) return Fail(op, ErrorCode::AlreadyBought, "offer already bought");
    if (offer.restricted) return Fail(op, ErrorCode::NotEligible, run_.character.name + " cannot use this weapon");
    if (run_.supplies.balance() < offer.price) {
        return Fail(op, ErrorCode::InsufficientFunds,
                    "price " + std::to_string(offer.price) + ", balance " + std::to_string(run_.supplies.balance()));
    }
    const std::string tx = ArenaTx(run_.arena_index, (run_.shop_generation > 0 ? "shop" + std::to_string(run_.shop_generation) : std::string("shop")) +
                                                         ":" + std::to_string(index));
    const TxResult debit = DebitSupplies(tx, offer.price, "shop");
    if (debit != TxResult::Applied) {
        return Fail(op, debit == TxResult::InsufficientFunds ? ErrorCode::InsufficientFunds : ErrorCode::AlreadyBought,
                    std::string(TxResultName(debit)));
    }

    Status grant = Status::Success();
    const EconomyConfig& eco = content_->economy();
    switch (offer.kind) {
        case ShopOfferKind::Heal:
            if (!caps_.Has(cap::kHeal)) {
                grant = Status::Error(ErrorCode::NotEligible, "heal capability is not available");
            } else {
                run_.heal_requests.push_back(HealRequest{offer.heal_fraction, "shop"});
            }
            break;
        case ShopOfferKind::RerollToken:
            if (run_.reroll_tokens >= eco.run.max_reroll_tokens) {
                grant = Status::Error(ErrorCode::NotEligible, "reroll token limit reached");
            } else {
                ++run_.reroll_tokens;
            }
            break;
        case ShopOfferKind::WeaponUpgrade: {
            const auto it = std::find_if(run_.weapons.begin(), run_.weapons.end(), [&offer](const WeaponInstance& w) {
                return w.instance_id == offer.target_instance_id;
            });
            if (it == run_.weapons.end()) {
                grant = Status::Error(ErrorCode::GrantFailed, "weapon " + offer.target_instance_id + " is no longer held");
            } else if (it->level != offer.target_level || !WeaponUpgradeCost(MakeOfferContext(0.0), *it)) {
                grant = Status::Error(ErrorCode::GrantFailed, "weapon level changed since the offer was made");
            } else {
                ++it->level;
            }
            break;
        }
        case ShopOfferKind::Weapon:
            grant = PlaceWeaponItem(offer.weapon, "shop");
            if (grant.ok()) GrantWeapon(offer.weapon, "shop");
            break;
        case ShopOfferKind::Perk: grant = GrantPerk(offer.perk_id, "shop", CursePolicy::Exclude, false); break;
        case ShopOfferKind::Refine: grant = ApplyRefine(offer.target_instance_id, offer.weapon, "shop"); break;
        case ShopOfferKind::Item: {
            const EquipmentDef* def = content_->loadouts().FindEquipment(offer.item_id);
            grant = GrantItem(offer.item_id, def != nullptr ? def->bundle : 1, "shop");  // the same bundle a reward gives
            break;
        }
    }
    if (!grant.ok()) {
        // All-or-nothing: undo the debit so a failed grant never costs supplies.
        run_.supplies.Rollback(tx);
        run_.supplies_spent -= offer.price;
        Event r = MakeEvent(EventType::CurrencyChanged);
        r.Add("currency", "supplies").Add("delta", offer.price).Add("balance", run_.supplies.balance()).Add("tx", tx)
            .Add("reason", "rollback");
        Emit(std::move(r));
        return Fail(op, ErrorCode::GrantFailed, grant.message);
    }
    offer.purchased = true;
    Event e = MakeEvent(EventType::Purchase);
    e.Add("index", index).Add("kind", ShopOfferKindName(offer.kind)).Add("price", offer.price)
        .Add("balance", run_.supplies.balance());
    if (!offer.perk_id.empty()) e.Add("perk", offer.perk_id);
    if (!offer.item_id.empty()) e.Add("item", offer.item_id);
    if (!offer.target_instance_id.empty()) e.Add("weapon_instance", offer.target_instance_id);
    if (offer.kind == ShopOfferKind::Weapon) e.Add("weapon", offer.weapon.base_id);
    Emit(std::move(e));
    return Status::Success();
}

Status RunController::LeaveShop() {
    if (!run_.active) return Fail("leave_shop", ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::Shop) {
        return Fail("leave_shop", ErrorCode::WrongPhase, "phase is " + std::string(PhaseName(run_.phase)));
    }
    if (RunState::SeatStatus* status = ActiveSeatStatus()) status->shop_done = true;
    if (!EverySeatLeftShop()) return Status::Success();  // the other players are still shopping
    EnterRouteSelection();
    return Status::Success();
}

void RunController::EnterRouteSelection() {
    // The choices are the nodes the current one leads to in the run map.
    run_.route_choices.clear();
    const auto& layers = run_.run_map.layers;
    const auto layer = static_cast<std::size_t>(run_.map_layer);
    if (layer + 1 < layers.size() && static_cast<std::size_t>(run_.map_node) < layers[layer].size()) {
        for (const int n : layers[layer][static_cast<std::size_t>(run_.map_node)].next) {
            const RunMapNode& node = layers[layer + 1][static_cast<std::size_t>(n)];
            RouteChoice choice;
            choice.route_id = node.route_id;
            choice.map_key = node.map_key;
            choice.node = n;
            run_.route_choices.push_back(std::move(choice));
        }
    }
    RollRouteCurses(*content_, streams_->route, run_.route_choices, [this](const PerkDef& p) {
        return CheckPerk(p, MakeGateInput(CursePolicy::Only)) == PerkGate::Eligible;
    });
    run_.route_chosen = false;
    run_.phase = Phase::RouteSelection;
}

int RunController::PlannedMapKey() const {
    const auto& layers = run_.run_map.layers;
    if (!run_.active || static_cast<std::size_t>(run_.map_layer) >= layers.size()) return -1;
    const auto& layer = layers[static_cast<std::size_t>(run_.map_layer)];
    return static_cast<std::size_t>(run_.map_node) < layer.size() ? layer[static_cast<std::size_t>(run_.map_node)].map_key : -1;
}

std::vector<std::string> RunController::PlannedGuests() const {
    const auto& layers = run_.run_map.layers;
    if (!run_.active || static_cast<std::size_t>(run_.map_layer) >= layers.size()) return {};
    const auto& layer = layers[static_cast<std::size_t>(run_.map_layer)];
    return static_cast<std::size_t>(run_.map_node) < layer.size() ? layer[static_cast<std::size_t>(run_.map_node)].guests
                                                                   : std::vector<std::string>{};
}

std::optional<MapDef> RunController::ArenaMap(int stage) const {
    const MapDef* found = content_->FindMapByStage(stage);
    if (found == nullptr) return std::nullopt;
    const auto edit = map_edits_.find(stage);
    const MapDef map = edit != map_edits_.end() ? ApplyMapEdit(*found, edit->second) : *found;
    if (stage != PlannedMapKey()) return map;
    return WithGuests(*content_, map, PlannedGuests());
}

Status RunController::EditorRestartArena() {
    constexpr std::string_view op = "editor_restart_arena";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::ArenaActive || run_.arenas.empty() || run_.arena_index <= 0) {
        return Fail(op, ErrorCode::WrongPhase, "only during an arena (phase " + std::string(PhaseName(run_.phase)) + ")");
    }
    run_.arenas.pop_back();
    --run_.arena_index;
    director_.reset();
    run_.phase = Phase::RouteSelection;
    run_.route_chosen = true;
    return Status::Success();
}

void RunController::SetMapEdits(MapEdits edits) {
    map_edits_ = std::move(edits);
    if (!director_ || map_stage_ < 0) return;
    if (const std::optional<MapDef> map = ArenaMap(map_stage_)) director_->ReplacePoints(map->points);
}

Status RunController::ChooseRoute(std::size_t index) {
    constexpr std::string_view op = "choose_route";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::RouteSelection) {
        return Fail(op, ErrorCode::WrongPhase, "phase is " + std::string(PhaseName(run_.phase)));
    }
    if (run_.route_chosen) return Fail(op, ErrorCode::AlreadyChosen, "route already chosen");
    if (index >= run_.route_choices.size()) return Fail(op, ErrorCode::InvalidIndex, "no route at index " + std::to_string(index));
    const RouteChoice choice = run_.route_choices[index];
    const RouteDef* def = content_->FindRoute(choice.route_id);
    if (def == nullptr) return Fail(op, ErrorCode::InvalidEncounter, "unknown route '" + choice.route_id + "'");
    run_.route_id = def->id;
    run_.route_chosen = true;
    if (choice.node >= 0) {
        ++run_.map_layer;
        run_.map_node = choice.node;
    }
    if (!choice.curse_perk_id.empty()) {
        const Status s = GrantPerk(choice.curse_perk_id, "route_curse", CursePolicy::Only, true);
        if (!s.ok()) EmitError("route_curse", s);
    }
    Event e = MakeEvent(EventType::RouteSelected);
    e.Add("index", index).Add("route", def->id).Add("tier", def->difficulty_tier).Add("budget", def->enemy_budget)
        .Add("map", choice.map_key);
    if (!choice.curse_perk_id.empty()) e.Add("curse", choice.curse_perk_id);
    Emit(std::move(e));
    return Status::Success();
}

Status RunController::DevGrantPerk(std::string_view perk_id) {
    if (!run_.active) return Fail("dev_grant_perk", ErrorCode::NoActiveRun, "no active run");
    const Status s = GrantPerk(perk_id, "dev", CursePolicy::Any, false);
    if (!s.ok()) EmitError("dev_grant_perk", s);
    return s;
}

std::vector<SpawnRequest> RunController::DirectorTick(double now, const std::optional<std::array<double, 3>>& player,
                                                     const Director::PointFilter& prefer) {
    if (!director_ || !run_.active || run_.phase != Phase::ArenaActive) return {};
    return director_->Tick(now, player, prefer);
}

std::vector<SpawnRequest> RunController::DirectorTick(double now, const std::vector<PlayerSpot>& players,
                                                     const Director::PointFilter& prefer) {
    if (!director_ || !run_.active || run_.phase != Phase::ArenaActive) return {};
    return director_->Tick(now, players, prefer);
}

void RunController::DirectorSpawned(std::uint64_t ticket, const std::string& unit_id) {
    if (director_) director_->Spawned(ticket, unit_id);
}

void RunController::DirectorSpawnFailed(std::uint64_t ticket) {
    if (director_) director_->SpawnFailed(ticket);
}

void RunController::DirectorClassUnavailable(const std::string& unit_class) {
    if (director_) director_->Unavailable(unit_class);
}

std::optional<std::array<double, 3>> RunController::DirectorRelocate(const std::string& unit_id,
                                                                     const std::optional<std::array<double, 3>>& player,
                                                                     const Director::PointFilter& accept, bool nearest) {
    if (!director_ || run_.phase != Phase::ArenaActive) return std::nullopt;
    return director_->Relocate(unit_id, player, accept, nearest);
}

Result<ChestReward> RunController::OpenChest(double now, std::optional<std::string> held_weapon) {
    constexpr std::string_view op = "open_chest";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::ArenaActive) return Fail(op, ErrorCode::WrongPhase, "phase is " + std::string(PhaseName(run_.phase)));
    const EconomyConfig& eco = content_->economy();
    Rng& rng = streams_->loot;
    ChestReward out;
    const SkinBuild& build = run_.character.build;
    if (eco.keys.enabled && !build.chest_keys_exempt) {
        if (run_.keys <= 0) {  // stays shut; the adapter puts the chest back
            out.locked = true;
            Event e = MakeEvent(EventType::ChestOpened);
            e.Add("locked", true).Add("keys", run_.keys).Add("now", now);
            Emit(std::move(e));
            return out;
        }
        --run_.keys;
        out.key_used = true;
    }
    out.number = ++run_.chests_opened;
    const int arena = std::max(1, run_.arena_index);
    std::int64_t supplies = eco.chests.supplies_base + eco.chests.supplies_per_arena * (arena - 1);

    const OfferContext ctx = MakeOfferContext(0.0);
    // Sometimes a chest refines the gun in hand one tier (the rarity remake): on top of the rest.
    const double chest_refine = RefineChance(eco.refine, eco.refine.chest_chance, PacingArenas());
    if (eco.refine.enabled && chest_refine > 0.0 && rng.NextDouble() < chest_refine) {
        if (const WeaponInstance* target = RefineTarget(held_weapon)) {
            const WeaponInstance refined = RefineWeapon(rng, *content_, caps_, *target, BuildElements());
            const std::string id = target->instance_id;
            if (ApplyRefine(id, refined, "chest").ok()) {
                out.refined_instance_id = id;
                const WeaponBaseDef* base = content_->FindWeapon(refined.base_id);
                out.refined_name = base != nullptr ? base->name : refined.base_id;
                out.refined_rarity = std::string(RarityName(refined.rarity));
            }
        }
    }
    const double roll = rng.NextDouble();
    bool placed = false;
    // A build whose drops are one item (Safari Chris: mines) gets that item from every chest.
    if (build.drops_as >= 0) {
        for (const EquipmentDef& e : content_->loadouts().equipment) {
            if (e.game_item != build.drops_as) continue;
            if (GrantItem(e.id, e.bundle, "chest").ok()) {
                out.item_id = e.id;
                out.item_name = e.name;
                out.item_count = e.bundle;
            }
            placed = true;
            break;
        }
    }
    if (!placed && roll < eco.chests.weapon_chance) {
        std::vector<const WeaponBaseDef*> bases = OfferableWeapons(ctx);
        std::erase_if(bases, [this, &ctx](const WeaponBaseDef* b) {
            return !UsableWeapon(ctx, *b) ||
                   std::any_of(run_.weapons.begin(), run_.weapons.end(), [b](const WeaponInstance& w) { return w.base_id == b->id; });
        });
        if (!bases.empty()) {
            const WeaponBaseDef* base = bases[rng.Index(bases.size())];
            WeaponInstance w = GenerateWeapon(rng, *content_, caps_, base->id,
                                              RollRarity(rng, ShiftRarityWeights(eco.rewards.rarity_weights, ctx.rarity_shift)),
                                              NextInstanceId(), BuildElements());
            if (PlaceWeaponItem(w, "chest").ok()) {
                GrantWeapon(w, "chest");
                out.weapon_id = base->id;
                out.weapon_name = base->name;
                placed = true;
            }
        }
    }
    if (!placed && roll < eco.chests.weapon_chance + eco.chests.item_chance) {
        const std::vector<const EquipmentDef*> items = OfferableItems(ctx);
        if (!items.empty()) {
            const EquipmentDef* e = items[rng.Index(items.size())];
            if (GrantItem(e->id, e->bundle, "chest").ok()) {
                out.item_id = e->id;
                out.item_name = e->name;
                out.item_count = e->bundle;
                placed = true;
            } else {
                // No room: what selling it pays (half its shop price; the price is the bundle's,
                // and paying it times the bundle made a chest of 30 rounds worth 360 supplies).
                supplies += e->price / 2;
            }
        }
    }
    if (supplies > 0 && CreditSupplies(ArenaTx(run_.arena_index, "chest" + std::to_string(out.number)), supplies, "chest")) {
        out.supplies = supplies;
    }
    Event e = MakeEvent(EventType::ChestOpened);
    e.Add("number", out.number).Add("supplies", out.supplies).Add("now", now);
    if (out.key_used) e.Add("key_used", true).Add("keys", run_.keys);
    if (!out.item_id.empty()) e.Add("item", out.item_id).Add("count", out.item_count);
    if (!out.weapon_id.empty()) e.Add("weapon", out.weapon_id);
    if (!out.refined_instance_id.empty()) e.Add("refined", out.refined_instance_id).Add("refined_rarity", out.refined_rarity);
    Emit(std::move(e));
    return out;
}

Status RunController::SwapStartingWeapon(std::string_view base_id) {
    if (!run_.active) return Status::Error(ErrorCode::NoActiveRun, "no active run");
    const WeaponBaseDef* base = content_->FindWeapon(base_id);
    if (base == nullptr) return Status::Error(ErrorCode::UnknownId, "unknown weapon '" + std::string(base_id) + "'");
    if (run_.loadout.weapon_id == base->id) return Status::Success();
    const std::string previous = run_.loadout.weapon_id;
    std::erase_if(run_.weapons, [&previous](const WeaponInstance& w) { return w.base_id == previous; });
    run_.loadout.weapon_id = base->id;
    // A gun the run already holds (a skin build's rolled starting gun) keeps its instance.
    const bool held = std::any_of(run_.weapons.begin(), run_.weapons.end(),
                                  [base](const WeaponInstance& w) { return w.base_id == base->id; });
    if (!held) GrantWeapon(MakeStartingWeapon(base->id, NextInstanceId()), "starting_weapon");
    Event e = MakeEvent(EventType::PerkAcquired, EventLevel::Debug);
    e.Add("starting_weapon", base->id).Add("replaced", previous);
    Emit(std::move(e));
    return Status::Success();
}

std::vector<std::int64_t> RunController::RestockWeaponItems() const {
    std::vector<std::int64_t> out;
    if (!run_.active) return out;
    for (const WeaponInstance& w : run_.weapons) {
        const WeaponBaseDef* base = content_->FindWeapon(w.base_id);
        if (base != nullptr && base->restocks && base->game_item >= 0) out.push_back(base->game_item);
    }
    return out;
}

std::vector<ElementChoice> RunController::PendingElementChoices() const {
    std::vector<ElementChoice> out;
    if (!run_.active) return out;
    for (const WeaponInstance& w : run_.weapons) {
        if (w.element_offer.empty() || !w.elements.empty()) continue;
        ElementChoice c;
        c.instance_id = w.instance_id;
        c.base_id = w.base_id;
        c.rarity = w.rarity;
        c.evolved = w.element_evolved;
        c.offer = w.element_offer;
        out.push_back(std::move(c));
    }
    return out;
}

Status RunController::ChooseElement(std::string_view instance_id, int option) {
    constexpr const char* op = "choose_element";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    const auto it = std::find_if(run_.weapons.begin(), run_.weapons.end(),
                                 [instance_id](const WeaponInstance& w) { return w.instance_id == instance_id; });
    if (it == run_.weapons.end()) return Fail(op, ErrorCode::UnknownId, "no gun " + std::string(instance_id));
    if (it->element_offer.empty() || !it->elements.empty()) return Fail(op, ErrorCode::NotEligible, "that gun has no element to choose");
    std::optional<Element> chosen;
    if (option >= 0) {
        if (option >= static_cast<int>(it->element_offer.size())) return Fail(op, ErrorCode::InvalidIndex, "no such element offered");
        chosen = it->element_offer[static_cast<std::size_t>(option)];
    } else {
        // Random: any element but the build's, each as likely (the rare ones too).
        const std::vector<Element> exclude = BuildElements();
        std::vector<Element> pool;
        for (const ElementDef& e : content_->elements()) {
            if (std::find(exclude.begin(), exclude.end(), e.element) == exclude.end()) pool.push_back(e.element);
        }
        if (pool.empty()) return Fail(op, ErrorCode::NotEligible, "no element to draw");
        chosen = pool[streams_->reward.Index(pool.size())];
    }
    it->elements = {*chosen};
    it->element_offer.clear();
    Event e = MakeEvent(EventType::WeaponRefined);
    e.Add("weapon", it->base_id).Add("weapon_instance", it->instance_id).Add("source", "element_choice")
        .Add("element", ElementName(*chosen)).Add("random", option < 0).Add("evolved", it->element_evolved);
    Emit(std::move(e));
    return Status::Success();
}

Status RunController::TesterSetElement(std::string_view base_id, std::optional<Element> element, bool evolved) {
    constexpr const char* op = "tester_set_element";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (!run_.character.tester_only) return Fail(op, ErrorCode::NotEligible, "only the tester picks elements");
    const auto it = std::find_if(run_.weapons.begin(), run_.weapons.end(), [base_id](const WeaponInstance& w) { return w.base_id == base_id; });
    if (it == run_.weapons.end()) return Fail(op, ErrorCode::UnknownId, "the run has no " + std::string(base_id));
    it->elements.clear();
    if (element) it->elements.push_back(*element);
    it->element_evolved = element.has_value() && evolved;
    it->element_offer.clear();
    Event e = MakeEvent(EventType::WeaponRefined);
    e.Add("weapon", it->base_id).Add("weapon_instance", it->instance_id).Add("source", "tester")
        .Add("element", element ? std::string(ElementName(*element)) : std::string("none")).Add("evolved", it->element_evolved);
    Emit(std::move(e));
    return Status::Success();
}

Status RunController::TesterSetWeapon(std::string_view base_id) {
    constexpr const char* op = "tester_set_weapon";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (!run_.character.tester_only) return Fail(op, ErrorCode::NotEligible, "only the tester picks its gun");
    const WeaponBaseDef* def = content_->FindWeapon(base_id);
    if (def == nullptr || def->game_item < 0) return Fail(op, ErrorCode::UnknownId, "no game weapon '" + std::string(base_id) + "'");
    const auto it = std::find_if(run_.weapons.begin(), run_.weapons.end(), [base_id](const WeaponInstance& w) { return w.base_id == base_id; });
    WeaponInstance gun = it != run_.weapons.end() ? *it : MakeStartingWeapon(std::string(base_id), NextInstanceId());
    std::vector<std::string> dropped;
    for (const WeaponInstance& w : run_.weapons) {
        if (w.instance_id != gun.instance_id) dropped.push_back(w.base_id);
    }
    run_.weapons = {gun};
    Event e = MakeEvent(EventType::PerkAcquired, EventLevel::Debug);
    e.Add("weapon_instance", gun.instance_id).Add("weapon", gun.base_id).Add("rarity", RarityName(gun.rarity))
        .Add("level", gun.level).Add("dropped", dropped.size()).Add("source", "tester");
    Emit(std::move(e));
    return Status::Success();
}

Status RunController::DevSetWeaponLevel(std::string_view base_id, int level) {
    constexpr const char* op = "dev_set_weapon_level";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    const auto it = std::find_if(run_.weapons.begin(), run_.weapons.end(), [base_id](const WeaponInstance& w) { return w.base_id == base_id; });
    if (it == run_.weapons.end()) return Fail(op, ErrorCode::UnknownId, "the run has no " + std::string(base_id));
    const int max = std::max(1, content_->economy().upgrades.max_level);
    if (level < 1 || level > max) return Fail(op, ErrorCode::InvalidArgument, "level " + std::to_string(level) + " is not 1 to " + std::to_string(max));
    const int before = it->level;
    it->level = level;
    Event e = MakeEvent(EventType::PerkAcquired, EventLevel::Debug);
    e.Add("weapon", it->base_id).Add("weapon_instance", it->instance_id).Add("from_level", before).Add("to_level", level).Add("source", "dev_level");
    Emit(std::move(e));
    return Status::Success();
}

std::int64_t RunController::SaleValue(std::int64_t game_item, std::int64_t count) const {
    if (game_item < 0 || count <= 0) return 0;
    const EconomyConfig& eco = content_->economy();
    for (const WeaponBaseDef& base : content_->weapons()) {
        if (base.game_item != game_item) continue;
        Rarity rarity = Rarity::Common;
        for (const WeaponInstance& w : run_.weapons) {
            if (w.base_id == base.id) rarity = w.rarity;
        }
        return eco.shop.weapon_price[RarityIndex(rarity)] / 2;  // a gun is one, whatever its rounds
    }
    for (const EquipmentDef& e : content_->loadouts().equipment) {
        if (e.game_item != game_item || e.price <= 0) continue;
        return e.price * count / (2 * std::max<std::int64_t>(1, e.bundle));
    }
    return 0;
}

Status RunController::DropWeapon(std::int64_t game_item, std::string_view reason) {
    if (!run_.active) return Fail("drop_weapon", ErrorCode::NoActiveRun, "no active run");
    const auto it = std::find_if(run_.weapons.begin(), run_.weapons.end(), [this, game_item](const WeaponInstance& w) {
        const WeaponBaseDef* base = content_->FindWeapon(w.base_id);
        return base != nullptr && base->game_item == game_item;
    });
    if (it == run_.weapons.end()) return Fail("drop_weapon", ErrorCode::UnknownId, "the run holds no gun " + std::to_string(game_item));
    Event e = MakeEvent(EventType::WeaponLost);
    e.Add("weapon", it->base_id).Add("weapon_instance", it->instance_id).Add("level", it->level).Add("reason", reason);
    Emit(std::move(e));
    run_.weapons.erase(it);
    return Status::Success();
}

void RunController::PruneLostWeapons() {
    if (inventory_ == nullptr || !run_.active || !WeaponIdMode()) return;
    for (auto it = run_.weapons.begin(); it != run_.weapons.end();) {
        const WeaponBaseDef* base = content_->FindWeapon(it->base_id);
        const bool lost = base != nullptr && base->game_item >= 0 && !base->restocks &&
                          inventory_->Holds(base->game_item) == std::optional<bool>(false);
        if (!lost) {
            ++it;
            continue;
        }
        Event e = MakeEvent(EventType::WeaponLost);
        e.Add("weapon", it->base_id).Add("weapon_instance", it->instance_id).Add("level", it->level).Add("reason", "not_held");
        Emit(std::move(e));
        it = run_.weapons.erase(it);
    }
}

Status RunController::DevGrantWeapon(WeaponInstance weapon) {
    if (!run_.active) return Fail("dev_grant_weapon", ErrorCode::NoActiveRun, "no active run");
    if (content_->FindWeapon(weapon.base_id) == nullptr) {
        return Fail("dev_grant_weapon", ErrorCode::UnknownId, "unknown weapon '" + weapon.base_id + "'");
    }
    if (weapon.instance_id.empty()) weapon.instance_id = NextInstanceId();
    GrantWeapon(std::move(weapon), "dev");
    return Status::Success();
}

Status RunController::DevRefineWeapon(const std::optional<std::string>& base) {
    if (!run_.active) return Fail("dev_refine_weapon", ErrorCode::NoActiveRun, "no active run");
    const WeaponInstance* target = RefineTarget(base);
    if (target == nullptr) return Fail("dev_refine_weapon", ErrorCode::NotEligible, "no gun can go up a tier");
    const WeaponInstance refined = RefineWeapon(streams_->loot, *content_, caps_, *target, BuildElements());
    const Status s = ApplyRefine(target->instance_id, refined, "dev");
    return s.ok() ? s : Fail("dev_refine_weapon", ErrorCode::GrantFailed, s.message);
}

Status RunController::DevGrantSupplies(std::int64_t amount) {
    if (!run_.active) return Fail("dev_grant_supplies", ErrorCode::NoActiveRun, "no active run");
    if (amount <= 0) return Fail("dev_grant_supplies", ErrorCode::InvalidAmount, "amount must be positive");
    if (!CreditSupplies("dev:supplies:" + std::to_string(++dev_supply_grants_), amount, "dev", false)) {
        return Fail("dev_grant_supplies", ErrorCode::AlreadyApplied, "credit refused");
    }
    return Status::Success();
}

Status RunController::AdjustSupplies(std::int64_t delta, std::string_view reason) {
    constexpr std::string_view op = "adjust_supplies";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.phase == Phase::Idle || run_.phase == Phase::ArenaActive || run_.phase == Phase::RunComplete ||
        run_.phase == Phase::RunFailed) {
        return Fail(op, ErrorCode::WrongPhase, "phase is " + std::string(PhaseName(run_.phase)));
    }
    if (delta == 0) return Status::Success();
    const std::string tx = std::string(reason) + ":" + std::to_string(++external_supply_changes_);
    if (delta > 0) {
        if (!CreditSupplies(tx, delta, reason, false)) return Fail(op, ErrorCode::AlreadyApplied, "credit refused");
    } else if (DebitSupplies(tx, -delta, reason) != TxResult::Applied) {
        return Fail(op, ErrorCode::InsufficientFunds, "debit of " + std::to_string(-delta) + " refused");
    }
    return Status::Success();
}

Result<RunSummary> RunController::EndRun() {
    constexpr std::string_view op = "end_run";
    if (!run_.active) {
        if (last_summary_) {
            RunSummary repeat = *last_summary_;
            repeat.already_banked = true;
            repeat.credits_banked = 0;
            return repeat;
        }
        return Fail(op, ErrorCode::NoActiveRun, "no run to end");
    }
    if (run_.phase != Phase::RunComplete && run_.phase != Phase::RunFailed) {
        return Fail(op, ErrorCode::WrongPhase, "run is still in progress (phase " + std::string(PhaseName(run_.phase)) + ")");
    }
    (void)ActivateSeat(0);  // the summary is the first player's; every seat's character counts below
    const EconomyConfig& eco = content_->economy();
    const bool completed = run_.phase == Phase::RunComplete;
    const std::int64_t amount =
        completed ? run_.credits_pending + eco.credits.run_complete_bonus
                  : static_cast<std::int64_t>(std::floor(static_cast<double>(run_.credits_pending) * eco.credits.death_rate));

    RunSummary s;
    s.run_id = run_.run_id;
    s.character_id = run_.character_id;
    s.seed = run_.seed;
    s.completed = completed;
    s.end_reason = run_.end_reason;
    s.arenas_cleared = static_cast<int>(run_.results.size());
    s.arena_count = run_.arena_count;
    for (const ArenaMetrics& a : run_.arenas) s.total_kills += a.player_kills;
    s.supplies_earned = run_.supplies_earned;
    s.supplies_spent = run_.supplies_spent;
    s.credits_pending = run_.credits_pending;
    s.arenas = run_.results;
    s.perks = run_.perks;
    s.skin_id = run_.skin_id;
    for (const ArenaMetrics& a : run_.arenas) s.time_seconds += std::max(0.0, a.end_time - a.start_time);
    s.previous_best_arena = static_cast<int>(profile_.stats.best_arena);
    if (const MapDef* map = content_->FindMapByStage(map_stage_)) s.final_arena = map->name;
    s.endless = EndlessRun();

    const TxResult r = BankRunCredits(profile_, run_.run_id, amount);
    s.already_banked = r == TxResult::AlreadyApplied;
    if (r == TxResult::Applied) {
        s.credits_banked = amount;
        if (completed) {
            ++profile_.stats.runs_completed;
        } else {
            ++profile_.stats.runs_failed;
        }
        profile_.stats.best_arena = std::max<std::int64_t>(profile_.stats.best_arena, s.arenas_cleared);
        profile_.stats.total_kills += s.total_kills;
        profile_.stats.credits_earned += amount;
        if (completed) {
            StringSet counted;
            counted.insert(run_.character_id);
            ++profile_.stats.runs_completed_as[run_.character_id];
            for (std::size_t i = 1; i < run_.seats.size(); ++i) {
                if (counted.insert(run_.seats[i].character_id).second) ++profile_.stats.runs_completed_as[run_.seats[i].character_id];
            }
        }
        for (const ArenaResult& a : s.arenas) {
            if (a.no_damage) ++profile_.stats.no_damage_arenas;
            if (a.rating == Rating::S) ++profile_.stats.s_rank_arenas;
        }
        Event c = MakeEvent(EventType::CurrencyChanged);
        c.Add("currency", "credits").Add("delta", amount).Add("balance", profile_.credits.balance())
            .Add("tx", BankTxId(run_.run_id)).Add("reason", completed ? "run_complete" : "run_failed");
        Emit(std::move(c));
    }
    profile_.active_run.reset();
    s.unlocked = EvaluateCharacterUnlocks();
    s.ladder_unlocks = LadderUnlocksFor(run_.bosses_beaten_new);
    (void)SaveProfile("end_run");

    Event e = MakeEvent(EventType::RunEnded);
    e.Add("completed", completed).Add("reason", run_.end_reason).Add("arenas_cleared", s.arenas_cleared)
        .Add("kills", s.total_kills).Add("credits_pending", s.credits_pending).Add("credits_banked", s.credits_banked)
        .Add("already_banked", s.already_banked).Add("supplies_earned", s.supplies_earned)
        .Add("supplies_spent", s.supplies_spent).Add("credits_balance", profile_.credits.balance());
    Emit(std::move(e));

    run_.active = false;
    run_.phase = Phase::Idle;
    run_.heal_requests.clear();
    run_.seats.clear();
    run_.seat_status.clear();
    run_.active_seat = 0;
    streams_.reset();
    last_summary_ = s;
    return s;
}

}  // namespace rm
