#include "rm/events.h"

#include <algorithm>

namespace rm {

std::string_view EventLevelName(EventLevel level) {
    switch (level) {
        case EventLevel::Error: return "error";
        case EventLevel::Info: return "info";
        case EventLevel::Debug: return "debug";
    }
    return "info";
}

std::string_view EventTypeName(EventType type) {
    switch (type) {
        case EventType::RunStarted: return "run_started";
        case EventType::ArenaStarted: return "arena_started";
        case EventType::EnemyKilled: return "enemy_killed";
        case EventType::PlayerDamaged: return "player_damaged";
        case EventType::DamageModified: return "damage_modified";
        case EventType::PerkAcquired: return "perk_acquired";
        case EventType::PerkTriggered: return "perk_triggered";
        case EventType::CurrencyChanged: return "currency_changed";
        case EventType::Purchase: return "purchase";
        case EventType::RewardSelected: return "reward_selected";
        case EventType::RouteSelected: return "route_selected";
        case EventType::ArenaCompleted: return "arena_completed";
        case EventType::RunEnded: return "run_ended";
        case EventType::Error: return "error";
        case EventType::ProfileLoaded: return "profile_loaded";
        case EventType::ProfileSaved: return "profile_saved";
        case EventType::UnlockPurchased: return "unlock_purchased";
        case EventType::ShopEntered: return "shop_entered";
        case EventType::RewardRerolled: return "reward_rerolled";
        case EventType::CharacterUnlocked: return "character_unlocked";
        case EventType::ItemGranted: return "item_granted";
        case EventType::PerkEquipped: return "perk_equipped";
        case EventType::ChestOpened: return "chest_opened";
    }
    return "unknown";
}

const FieldValue* Event::Find(std::string_view key) const {
    for (const EventField& f : fields) {
        if (f.key == key) return &f.value;
    }
    return nullptr;
}

std::size_t MemorySink::Count(EventType type) const {
    return static_cast<std::size_t>(std::count_if(
        events.begin(), events.end(), [type](const Event& e) { return e.type == type; }));
}

}  // namespace rm
