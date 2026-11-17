#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace rm {

enum class EventLevel { Error = 0, Info = 1, Debug = 2 };
std::string_view EventLevelName(EventLevel level);

/// Typed core events. Names (EventTypeName) are the "event" field in JSONL telemetry.
enum class EventType {
    RunStarted,
    ArenaStarted,
    EnemyKilled,
    PlayerDamaged,
    DamageModified,   // sampled (RunControllerOptions::damage_sample_every)
    PerkAcquired,
    PerkTriggered,
    CurrencyChanged,
    Purchase,
    RewardSelected,
    RouteSelected,
    ArenaCompleted,
    RunEnded,
    Error,
    ProfileLoaded,
    ProfileSaved,
    UnlockPurchased,
    ShopEntered,
    RewardRerolled,
    CharacterUnlocked,  // a character challenge was met
    ItemGranted,        // an inventory item was queued for the adapter (reward, shop)
    PerkEquipped,       // the menu changed a character's equipped perk
    ChestOpened,        // a chest (time-bonus pickup) paid out
    RunSuspended,       // its match was left; the run goes on later from its resume point
    RunResumed,         // a suspended or interrupted run goes on
    WeaponRefined,      // a held gun went one tier up (reward, shop, chest)
    WeaponLost,         // a gun left the run: no longer held (sold, used up) or given up
    PlayerJoined,       // local co-op: another player's seat joined the run (M10)
};
std::string_view EventTypeName(EventType type);

using FieldValue = std::variant<bool, std::int64_t, double, std::string>;

struct EventField {
    std::string key;
    FieldValue value;
};

struct Event {
    EventType type = EventType::Error;
    EventLevel level = EventLevel::Info;
    std::string run_id;  // empty outside a run
    int arena = 0;       // 1-based arena number, 0 = not in an arena
    std::vector<EventField> fields;

    Event& Add(std::string key, bool v) { return Push(std::move(key), FieldValue{v}); }
    Event& Add(std::string key, double v) { return Push(std::move(key), FieldValue{v}); }
    Event& Add(std::string key, std::string v) { return Push(std::move(key), FieldValue{std::move(v)}); }
    Event& Add(std::string key, std::string_view v) { return Push(std::move(key), FieldValue{std::string(v)}); }
    Event& Add(std::string key, const char* v) { return Push(std::move(key), FieldValue{std::string(v)}); }

    template <class T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
    Event& Add(std::string key, T v) {
        return Push(std::move(key), FieldValue{static_cast<std::int64_t>(v)});
    }

    [[nodiscard]] const FieldValue* Find(std::string_view key) const;

private:
    Event& Push(std::string key, FieldValue v) {
        fields.push_back(EventField{std::move(key), std::move(v)});
        return *this;
    }
};

/// Receives every core event. Implementations must not call back into RunController.
class IEventSink {
public:
    virtual ~IEventSink() = default;
    virtual void OnEvent(const Event& event) = 0;
};

/// Keeps events in memory (tests, simulator inspection).
class MemorySink final : public IEventSink {
public:
    void OnEvent(const Event& event) override { events.push_back(event); }
    [[nodiscard]] std::size_t Count(EventType type) const;
    void Clear() { events.clear(); }

    std::vector<Event> events;
};

/// Forwards to several sinks (e.g. JSONL file + UI notification queue).
class FanoutSink final : public IEventSink {
public:
    void AddSink(IEventSink* sink) { sinks_.push_back(sink); }
    void OnEvent(const Event& event) override {
        for (IEventSink* s : sinks_) {
            if (s != nullptr) s->OnEvent(event);
        }
    }

private:
    std::vector<IEventSink*> sinks_;
};

}  // namespace rm
