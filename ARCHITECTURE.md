# Architecture

## Boundaries

The reusable C++ core owns run rules, seeded randomness, progression, purchases, reward generation, persistence contracts and event reactions. It must build and run without the RE5 executable, Windows process memory or game headers. The RE5 adapter is the only layer allowed to know about DLL loading, D3D9, engine addresses, signatures, object layouts and stage-specific behavior. A future RE4R adapter can implement the same contracts without changing core rules.

```text
configs/ ──> core definitions ──> run/economy/perk state
                                      ▲       │
                                      │ events│ commands
simulation/ ── IGameAdapter ── adapters/re5/┘
                    │                  │
                    └── events ──> telemetry JSONL
core/persistence/ ── versioned mod save (separate from the game's save)
```

## Proposed modules

- `core/rng`: deterministic, versioned PRNG interface; derive route and reward streams from a run seed without consuming a shared stream unpredictably.
- `core/runs`: run/arena phase state machine, finite enemy budget and reset semantics.
- `core/characters`: character IDs, starting gear/passives, unlock requirements and allowed perk pools.
- `core/perks`: data-defined category, rarity, tags, trigger, stacking, exclusions and character restriction; event-driven activation.
- `core/weapons`: instance ID, base game weapon ID, level, rarity, affixes and numeric modifiers. Existing game weapon remains the physical representation.
- `core/loot` and `core/rewards`: deterministic reward candidates and mutually exclusive selections.
- `core/economy`: credits and supplies with checked, atomic debit/credit transactions and reward idempotency keys.
- `core/persistence`: versioned JSON, write-temp/flush/replace, validation and recovery of the mod-owned file only.
- `core/encounters`: route definitions and finite authored enemy budgets; engine callbacks report observed outcomes.
- `adapters/re5`: x86 proxy, build identity guard, signature registry, D3D9/UI, player/enemy observation and tested game commands.
- `simulation`: fake adapter and multi-run balance model; no engine addresses.
- `telemetry`: structured events, configurable verbosity, bounded log rotation and safe diagnostic ZIP.

## Adapter contract (initial draft)

Keep observations distinct from commands. Return an unavailable/unknown result rather than inventing a default when an engine feature is not verified.

```cpp
struct GameSnapshot {
    bool gameplayActive;
    std::optional<float> playerHealth;
    std::optional<int> aliveEnemyCount;
    std::optional<std::string> stageId;
};

class IGameAdapter {
public:
    virtual ~IGameAdapter() = default;
    virtual GameSnapshot Observe() = 0;
    virtual bool ApplyDamageModifier(float multiplier) = 0;
    virtual bool ShowNotification(std::string_view text) = 0;
};
```

Actual hook callbacks emit typed events such as `PlayerDamaged`, `EnemyKilled`, `ArenaStarted`, `ArenaFinished`, `PerkActivated`, `PurchaseCommitted` and `RewardSelected`. Event payloads include source confidence/build identity when relevant. Core code consumes events on a safe queue; engine memory is never read from UI or simulation threads unless the callback is verified thread-safe.

## State and persistence

- `PersistentProfile`: schema version, credits, unlocked character/skill/perk/gear IDs and idempotent reward ledger.
- `RunState`: seed, run ID, selected character, supplies, equipped perks, weapon instances, arena index, route, metrics and completion flag.
- Begin-run constructs a fresh `RunState`; only `PersistentProfile` survives.
- Save belongs in a mod-owned user data directory and never modifies RE5's save file.
- Every purchase validates item, price, stock, balance and run phase before debiting and granting as one transaction.

## Configuration and content

Human-editable JSON under `configs/` defines routes, perk definitions, reward tables and prices. Validation at startup rejects duplicate identifiers, invalid ranges, unsupported triggers and incompatible exclusions. Seeded tests pin the generator version so content changes can intentionally update balance baselines.

## Logging and diagnostics

JSONL events carry UTC timestamp, mod/adapter version and run ID. Log state transitions and meaningful gameplay events, not every frame. Diagnostic export includes logs, validated config, build identity, mod version and a text summary; omit personal files and game binaries.

## Threading and safety

DllMain performs only loader-safe setup and schedules initialization after attach. The worker validates the host module identity before scanning. Every signature must have an expected unique match and surrounding-byte check. Failed scans disable only their capability, report clearly, and never fall back to static guessed offsets. Hook uninstallation and process exit are tested before gameplay hooks are added.

## Build and release shape

- Core/simulator: C++20 where supported, ordinary x64 development host is allowed.
- RE5 adapter: MSVC Win32 (x86) DLL; separate output folder and manifest records compiler/toolset.
- Data, saves, logs and generated tests do not live next to the executable by default.
- The first proxy name is selected only after import/loader conflict checks; current executable imports XINPUT1_3, DINPUT8 and d3d9.
