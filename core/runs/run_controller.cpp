#include "rm/run_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

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

const ArenaMetrics* RunController::CurrentArena() const {
    return run_.arenas.empty() ? nullptr : &run_.arenas.back();
}

bool RunController::WeaponIdMode() const { return caps_.Has(cap::kWeaponId); }

std::string RunController::NextInstanceId() { return "w" + std::to_string(run_.next_weapon_serial++); }

OfferContext RunController::MakeOfferContext(double rarity_shift) const {
    OfferContext ctx;
    ctx.content = content_.get();
    ctx.character = Character();
    ctx.capabilities = &caps_;
    ctx.unlocked_perks = &profile_.unlocked_perks;
    ctx.unlocked_weapons = &profile_.unlocked_weapons;
    ctx.placeable_weapons = placeable_weapons_ ? &*placeable_weapons_ : nullptr;
    ctx.owned_perks = &run_.perks;
    ctx.rarity_shift = rarity_shift;
    const RouteDef* route = Route();
    ctx.reward_mult = route != nullptr ? route->reward_mult : 1.0;
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

bool RunController::CreditSupplies(const std::string& tx, std::int64_t amount, std::string_view reason) {
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

TxResult RunController::DebitSupplies(const std::string& tx, std::int64_t amount, std::string_view reason) {
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
            run_.item_grants.push_back(ItemGrant{static_cast<std::int64_t>(std::lround(p.game_item)),
                                                 std::max(1, static_cast<int>(std::lround(p.amount))), def->id});
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
    cp.character_id = run_.character_id;
    cp.seed = run_.seed;
    cp.arenas_cleared = static_cast<int>(run_.results.size());
    for (const ArenaMetrics& a : run_.arenas) cp.kills += a.player_kills;
    cp.credits_pending = run_.credits_pending;
    profile_.active_run = std::move(cp);
}

// ---- meta progression ----------------------------------------------------------------------

std::vector<CharacterAvailability> RunController::Characters() const {
    std::vector<CharacterAvailability> out;
    for (const CharacterDef& c : content_->characters()) {
        CharacterAvailability a{c.id, c.name, CheckCharacter(&c, profile_.unlocked_characters), c.unlock_cost_credits,
                                c.unlock.text, CheckChallenge(c, profile_.stats), c.costume_index};
        for (const SkinDef& s : c.skins) {
            a.skins.push_back({s.id, s.name, s.costume_index, CheckSkin(s, profile_.unlocked_characters), s.unlock.text,
                               CheckChallenge(s.unlock, profile_.stats)});
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
    constexpr std::string_view op = "start_run";
    if (run_.active) return Fail(op, ErrorCode::RunInProgress, "run " + run_.run_id + " has not ended");
    const CharacterDef* ch = content_->FindCharacter(character_id);
    switch (CheckCharacter(ch, profile_.unlocked_characters)) {
        case CharacterGate::Unknown:
            return Fail(op, ErrorCode::UnknownId, "unknown character '" + std::string(character_id) + "'");
        case CharacterGate::Disabled:
            return Fail(op, ErrorCode::Disabled, "character '" + std::string(character_id) + "' is not available yet");
        case CharacterGate::Locked:
            return Fail(op, ErrorCode::Locked, "character '" + std::string(character_id) + "' is locked");
        case CharacterGate::Selectable: break;
    }
    const SkinDef* skin = FindSkin(*ch, skin_id);
    if (skin == nullptr) {
        return Fail(op, ErrorCode::UnknownId, "unknown skin '" + std::string(skin_id) + "' for " + ch->id);
    }
    if (CheckSkin(*skin, profile_.unlocked_characters) == CharacterGate::Locked) {
        return Fail(op, ErrorCode::Locked, "skin '" + skin->id + "' is locked");
    }
    const EconomyConfig& eco = content_->economy();
    const RouteDef* start = content_->FindRoute(eco.run.starting_route);
    if (start == nullptr || !start->enabled || start->enemy_budget <= 0) {
        return Fail(op, ErrorCode::InvalidEncounter, "starting route '" + eco.run.starting_route + "' is not playable");
    }
    if (profile_.active_run) (void)SettleCheckpoint(*profile_.active_run, "interrupted");

    RunState fresh;
    fresh.active = true;
    fresh.run_id = RunIdFor(profile_.next_run_number);
    fresh.seed = seed;
    fresh.character_id = ch->id;
    fresh.skin_id = skin->id;
    fresh.character = ApplySkin(*ch, *skin);
    fresh.phase = Phase::Idle;
    fresh.arena_count = eco.run.arena_count;
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
    });
    run_.loadout = RollLoadout(run_.character, *content_, streams_->loadout);
    last_summary_.reset();
    ++profile_.next_run_number;
    ++profile_.stats.runs_started;

    Event e = MakeEvent(EventType::RunStarted);
    e.Add("character", ch->id).Add("seed", std::to_string(seed)).Add("rng_version", kRngVersion)
        .Add("arena_count", run_.arena_count).Add("route", start->id).Add("capabilities", caps_.ToString())
        .Add("weapon_mode", WeaponIdMode() ? std::string_view("by_weapon_id") : kSidearmSlot)
        .Add("skin", skin->id).Add("loadout_weapon", run_.loadout.weapon_id)
        .Add("loadout", FormatLoadout(run_.loadout, *content_)).Add("loadout_spent", run_.loadout.spent);
    Emit(std::move(e));

    // Unspent loadout points become supplies.
    const std::int64_t starting_supplies =
        eco.run.starting_supplies + std::max<std::int64_t>(run_.loadout.budget - run_.loadout.spent, 0);
    if (run_.supplies.Credit("start", starting_supplies) == TxResult::Applied && starting_supplies > 0) {
        Event c = MakeEvent(EventType::CurrencyChanged);
        c.Add("currency", "supplies").Add("delta", starting_supplies).Add("balance", run_.supplies.balance())
            .Add("tx", "start").Add("reason", "starting_supplies");
        Emit(std::move(c));
    }
    // Only the rolled weapon (the pistol): every other gun comes from supply perks, rewards or
    // the shop, so the run's weapon list matches what is really in the player's hands.
    if (!run_.loadout.weapon_id.empty()) {
        GrantWeapon(MakeStartingWeapon(run_.loadout.weapon_id, NextInstanceId()), "starting_weapon");
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
    UpdateCheckpoint();
    (void)SaveProfile("start_run");
    return Status::Success();
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
    run_.reward_offer.clear();
    run_.shop_offers.clear();
    run_.route_choices.clear();
    UpdateCheckpoint();
    (void)SaveProfile("run_failed");
}

Status RunController::OnPlayerDied(double now) {
    if (!run_.active) return Fail("player_died", ErrorCode::NoActiveRun, "no active run");
    if (run_.phase == Phase::RunComplete || run_.phase == Phase::RunFailed) {
        return Fail("player_died", ErrorCode::WrongPhase, "run already finished");
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
    ArenaMetrics m;
    m.arena = run_.arena_index;
    m.route_id = route->id;
    m.budget = route->enemy_budget;
    m.start_time = now;
    run_.arenas.push_back(std::move(m));
    run_.kill_records.clear();
    run_.last_melee_time.reset();
    run_.seen_enemy_ids.clear();
    run_.clamp_logged = false;
    run_.reward_offer.clear();
    run_.reward_chosen = false;
    run_.shop_offers.clear();
    run_.route_choices.clear();
    run_.route_chosen = false;
    run_.phase = Phase::ArenaActive;

    Event e = MakeEvent(EventType::ArenaStarted);
    e.Add("route", route->id).Add("tier", route->difficulty_tier).Add("budget", route->enemy_budget)
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

    if (kill.enemy_id && !run_.seen_enemy_ids.insert(*kill.enemy_id).second) {
        ++a.duplicate_kills;
        out.duplicate = true;
        out.remaining = RemainingEnemies();
        Event e = MakeEvent(EventType::EnemyKilled, EventLevel::Debug);
        e.Add("duplicate", true).Add("enemy_id", *kill.enemy_id);
        Emit(std::move(e));
        return out;
    }
    if (!kill.by_player) ++a.non_player_kills;
    const bool counts = kill.by_player || eco.run.non_player_kills_count_toward_budget;
    if (counts) {
        const bool within_budget = a.budget_kills < a.budget;
        ++a.budget_kills;
        out.counted = true;
        // Every player kill feeds combat state (kill streaks, after-melee); only kills inside
        // the budget pay out supplies/heals so stragglers cannot be farmed.
        const WeaponInstance* weapon = kill.by_player ? ApplicableWeapon(kill.weapon_id, kill.was_melee) : nullptr;
        if (kill.by_player) {
            ++a.player_kills;
            if (kill.was_melee.value_or(false)) run_.last_melee_time = kill.now;

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
            const std::string kill_tx = "kill" + std::to_string(a.budget_kills);
            const std::int64_t base = RoundAmount(static_cast<double>(eco.supplies.per_kill) * reward_mult);
            if (CreditSupplies(ArenaTx(a.arena, kill_tx + ":base"), base, "kill")) out.supplies_awarded += base;

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
                        const bool hit = p.chance >= 1.0 || streams_->loot.NextDouble() < p.chance;
                        if (!hit) continue;
                        const std::int64_t amount = RoundAmount(p.amount);
                        if (CreditSupplies(ArenaTx(a.arena, kill_tx + ":perk:" + id), amount, id)) {
                            out.supplies_awarded += amount;
                            out.triggered.push_back(id);
                        }
                    } else if (ef.kind == EffectKind::ItemOnKill && caps_.Has(cap::kInventory)) {
                        const int every = std::max(1, static_cast<int>(std::lround(p.every_kills)));
                        int& kills = run_.perk_kill_counts[id];
                        if (++kills % every != 0) continue;
                        if (p.chance > 0.0 && p.chance < 1.0 && streams_->loot.NextDouble() >= p.chance) continue;
                        run_.item_grants.push_back(
                            ItemGrant{static_cast<std::int64_t>(std::lround(p.game_item)),
                                      std::max(1, static_cast<int>(std::lround(p.amount))), id});
                        out.triggered.push_back(id);
                    }
                }
            }
            if (weapon != nullptr) {
                std::int64_t amount = 0;
                for (const Affix& af : weapon->affixes) {
                    if (af.kind == AffixKind::SuppliesOnKill) amount += RoundAmount(af.value);
                }
                if (amount > 0 && CreditSupplies(ArenaTx(a.arena, kill_tx + ":weapon"), amount, weapon->instance_id)) {
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
    for (const auto& [id, stacks] : run_.perks) {
        const PerkDef* def = content_->FindPerk(id);
        if (def == nullptr) continue;
        for (const EffectDef& ef : def->effects) {
            if (ef.kind == EffectKind::ItemOnArenaClear && caps_.Has(cap::kInventory)) {
                const EffectParams p = ef.AtStacks(stacks);
                run_.item_grants.push_back(ItemGrant{static_cast<std::int64_t>(std::lround(p.game_item)),
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
    run_.results.push_back(result);

    run_.reward_rarity_shift = route.rarity_bonus + eco.rewards.rating_rarity_bonus[RatingIndex(rating)];
    run_.reward_offer = GenerateRewardOffer(streams_->reward, MakeOfferContext(run_.reward_rarity_shift),
                                            [this] { return NextInstanceId(); });
    run_.reward_chosen = false;
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
    run_.phase = Phase::RewardSelection;
    return Status::Success();
}

Status RunController::RerollReward() {
    constexpr std::string_view op = "reroll_reward";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::RewardSelection || run_.reward_chosen) {
        return Fail(op, ErrorCode::WrongPhase, "reroll is only possible before choosing a reward");
    }
    if (run_.reroll_tokens <= 0) return Fail(op, ErrorCode::NoRerollTokens, "no reroll tokens");
    --run_.reroll_tokens;
    run_.reward_offer = GenerateRewardOffer(streams_->reward, MakeOfferContext(run_.reward_rarity_shift),
                                            [this] { return NextInstanceId(); });
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
                // No room for the gun: its shop value in supplies instead.
                converted_supplies = content_->economy().shop.weapon_price[RarityIndex(choice.rarity)];
                if (!CreditSupplies(ArenaTx(run_.arena_index, "reward"), converted_supplies, "reward")) {
                    return Fail(op, ErrorCode::GrantFailed, placed.message);
                }
            } else {
                GrantWeapon(choice.weapon, "reward");
            }
            break;
        }
        case RewardKind::Supplies:
            if (!CreditSupplies(ArenaTx(run_.arena_index, "reward"), choice.supplies, "reward")) {
                return Fail(op, ErrorCode::GrantFailed, "supplies reward could not be credited");
            }
            break;
        case RewardKind::Item: {
            const Status s = GrantItem(choice.item_id, choice.item_count, "reward");
            if (!s.ok()) {
                // A full inventory must never block the run: the item's shop value is paid in
                // supplies instead (the adapter shows why).
                const EquipmentDef* def = content_->loadouts().FindEquipment(choice.item_id);
                if (s.code != ErrorCode::GrantFailed || def == nullptr) return Fail(op, ErrorCode::GrantFailed, s.message);
                converted_supplies = def->price * std::max(1, choice.item_count);
                if (!CreditSupplies(ArenaTx(run_.arena_index, "reward"), converted_supplies, "reward")) {
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
    Emit(std::move(e));

    if (run_.arena_index >= run_.arena_count) {
        run_.phase = Phase::RunComplete;
        run_.end_reason = "complete";
        UpdateCheckpoint();
        (void)SaveProfile("run_complete");
    } else {
        EnterShop();
    }
    return Status::Success();
}

void RunController::EnterShop() {
    const RouteDef* route = Route();
    run_.shop_offers = GenerateShopOffers(streams_->shop, MakeOfferContext(route != nullptr ? route->rarity_bonus : 0.0),
                                          run_.weapons, run_.reroll_tokens, [this] { return NextInstanceId(); });
    run_.phase = Phase::Shop;
    Event e = MakeEvent(EventType::ShopEntered);
    e.Add("offers", run_.shop_offers.size()).Add("supplies", run_.supplies.balance());
    Emit(std::move(e));
}

Status RunController::Purchase(std::size_t index) {
    constexpr std::string_view op = "purchase";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::Shop) return Fail(op, ErrorCode::WrongPhase, "phase is " + std::string(PhaseName(run_.phase)));
    if (index >= run_.shop_offers.size()) return Fail(op, ErrorCode::InvalidIndex, "no offer at index " + std::to_string(index));
    ShopOffer& offer = run_.shop_offers[index];
    if (offer.purchased) return Fail(op, ErrorCode::AlreadyBought, "offer already bought");
    if (run_.supplies.balance() < offer.price) {
        return Fail(op, ErrorCode::InsufficientFunds,
                    "price " + std::to_string(offer.price) + ", balance " + std::to_string(run_.supplies.balance()));
    }
    const std::string tx = ArenaTx(run_.arena_index, "shop:" + std::to_string(index));
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
            } else if (it->level != offer.target_level || !UpgradeCost(eco, it->level)) {
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
        case ShopOfferKind::Item: grant = GrantItem(offer.item_id, 1, "shop"); break;
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
    EnterRouteSelection();
    return Status::Success();
}

void RunController::EnterRouteSelection() {
    const RouteDef* route = Route();
    const int tier = route != nullptr ? route->difficulty_tier : 1;
    run_.route_choices = GenerateRouteChoices(*content_, streams_->route, tier, [this](const PerkDef& p) {
        return CheckPerk(p, MakeGateInput(CursePolicy::Only)) == PerkGate::Eligible;
    });
    run_.route_chosen = false;
    run_.phase = Phase::RouteSelection;
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
    if (!choice.curse_perk_id.empty()) {
        const Status s = GrantPerk(choice.curse_perk_id, "route_curse", CursePolicy::Only, true);
        if (!s.ok()) EmitError("route_curse", s);
    }
    Event e = MakeEvent(EventType::RouteSelected);
    e.Add("index", index).Add("route", def->id).Add("tier", def->difficulty_tier).Add("budget", def->enemy_budget);
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

Result<ChestReward> RunController::OpenChest(double now) {
    constexpr std::string_view op = "open_chest";
    if (!run_.active) return Fail(op, ErrorCode::NoActiveRun, "no active run");
    if (run_.phase != Phase::ArenaActive) return Fail(op, ErrorCode::WrongPhase, "phase is " + std::string(PhaseName(run_.phase)));
    const EconomyConfig& eco = content_->economy();
    Rng& rng = streams_->loot;
    ChestReward out;
    out.number = ++run_.chests_opened;
    const int arena = std::max(1, run_.arena_index);
    std::int64_t supplies = eco.chests.supplies_base + eco.chests.supplies_per_arena * (arena - 1);

    const OfferContext ctx = MakeOfferContext(0.0);
    const double roll = rng.NextDouble();
    bool placed = false;
    if (roll < eco.chests.weapon_chance) {
        std::vector<const WeaponBaseDef*> bases = OfferableWeapons(ctx);
        std::erase_if(bases, [this](const WeaponBaseDef* b) {
            return std::any_of(run_.weapons.begin(), run_.weapons.end(), [b](const WeaponInstance& w) { return w.base_id == b->id; });
        });
        if (!bases.empty()) {
            const WeaponBaseDef* base = bases[rng.Index(bases.size())];
            WeaponInstance w = GenerateWeapon(rng, *content_, caps_, base->id, RollRarity(rng, eco.rewards.rarity_weights),
                                              NextInstanceId());
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
                supplies += e->price * e->bundle;  // inventory full: its value instead
            }
        }
    }
    if (supplies > 0 && CreditSupplies(ArenaTx(run_.arena_index, "chest" + std::to_string(out.number)), supplies, "chest")) {
        out.supplies = supplies;
    }
    Event e = MakeEvent(EventType::ChestOpened);
    e.Add("number", out.number).Add("supplies", out.supplies).Add("now", now);
    if (!out.item_id.empty()) e.Add("item", out.item_id).Add("count", out.item_count);
    if (!out.weapon_id.empty()) e.Add("weapon", out.weapon_id);
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
    GrantWeapon(MakeStartingWeapon(base->id, NextInstanceId()), "starting_weapon");
    Event e = MakeEvent(EventType::PerkAcquired, EventLevel::Debug);
    e.Add("starting_weapon", base->id).Add("replaced", previous);
    Emit(std::move(e));
    return Status::Success();
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
        if (completed) ++profile_.stats.runs_completed_as[run_.character_id];
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
    (void)EvaluateCharacterUnlocks();
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
    streams_.reset();
    last_summary_ = s;
    return s;
}

}  // namespace rm
