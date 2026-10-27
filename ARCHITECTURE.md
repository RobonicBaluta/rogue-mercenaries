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

## Adapter contract (as built)

The core facade is `rm::RunController` (`core/include/rm/run_controller.h`); the adapter reports verified engine features through `rm::Capabilities` and receives typed events through `rm::IEventSink`. Content needing a missing capability is never offered.

`adapters/re5` (builds `dinput8.dll`, x86, static CRT):

| File | Role |
|---|---|
| `proxy.cpp`, `dinput8.def` | Forwards the five dinput8 exports to `SysWOW64\dinput8.dll` |
| `dllmain.cpp` | Worker thread: settings, log, crash handler, signature verification, hook installation in one `MH_ApplyQueued` batch |
| `re5_layout.h` | Every address/offset for build 11465250, each guarded by an expected-bytes signature |
| `engine.cpp` | Fault-tolerant reads: players, enemies, HP, DTI type checks, game work |
| `hooks_combat.cpp` | `uEnemy::OnHit` scope + base `uCharacter::SubHp` (outgoing), `uPlayer::SubHp` (incoming) |
| `hooks_merc.cpp` | Mercenaries countdown update: arena time floor, end-of-arena time-up |
| `hooks_input.cpp` | DirectInput/XInput: game tick, overlay input, blocking while the menu is open (held keys stay hidden after closing) |
| `hooks_d3d9.cpp`, `overlay.cpp` | Present/Reset hooks, Dear ImGui HUD and menu, digit/Enter hotkeys |
| `mod.cpp` | Maps engine events to `RunController`, arena state machine on Mercenaries matches, UI panels, developer tools; every entry point is exception-guarded |
| `probe.cpp`, `crash.cpp`, `dev_*.cpp` | Diagnostics, crash logging, test-only audio mute and frame capture |

Arena mapping: a Mercenaries match whose countdown runs while the run waits for an arena begins that arena. Player kills reduce the kill budget. On clear the mod zeroes the authoritative countdown and the game shows its own results. The reward → shop → route flow happens on that screen, and the next match begins the next arena.

Threading: Present and the input poll run on the game's main thread (the core tick); enemy hits are processed on game worker threads. All `RunController` calls take one mutex.

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
