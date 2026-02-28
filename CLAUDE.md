# CLAUDE.md

This file provides guidance to Claude Code when working with code in this repository.

## Project Overview

Huginn is an SKSE plugin for Skyrim — a **contextual quick-access system** that surfaces the right spell, potion, weapon, or item exactly when you need it.

> **Core Principle:** Only recommend based on information the player already has or could easily perceive. This is a convenience tool, not a cheat tool.

## Build Commands

```sh
cmake --preset vs2022-windows && cmake --build build --config Debug
```

**Prerequisites:** Visual Studio 2022 (v143), vcpkg (`VCPKG_ROOT`), CommonLibSSE-NG v3.7.0 (`CommonLibSSEPath_NG`), `CompiledPluginsPath` set to your SKSE plugins output directory.

**Clean build:** Delete `build/`, then reconfigure and rebuild.

CMakeLists.txt uses `GLOB_RECURSE` — new source files are picked up at configure time (no need to edit CMakeLists).

## Architecture Pipeline

```
Game (SKSE) → StateManager (11 polls) → 6 State Types → StateEvaluator → GameState
                                      ↓
                           Candidates → Utility Scorer → Slot Allocator → Widget/Wheeler
```

**Scoring formula:**
```
utility(item) = contextWeight × (1 + λ(confidence) × learningScore)
```

**Key architectural separation:**
- `ContextRuleEngine` — "What matters RIGHT NOW?" (game state → relevance weights)
- `PriorCalculator` — "Which item is intrinsically better?" (item properties → quality)
- `FeatureQLearner` — "What does THIS PLAYER prefer?" (learned preferences, 18-float feature vectors)
- `UtilityScorer` — Combines all three into final utility

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for full system design.

## Key Directories

| Directory | Purpose |
|-----------|---------|
| `src/state/` | StateManager + 11 poll methods, 6 state types |
| `src/candidate/` | Candidate generation and filtering |
| `src/learning/` | FeatureQLearner, PriorCalculator, UtilityScorer |
| `src/spell/`, `src/learning/item/`, `src/weapon/`, `src/scroll/` | Registries and classifiers |
| `src/slot/` | Slot allocation, locking, classification |
| `src/context/` | ContextRuleEngine (context weight computation) |
| `src/ui/` | IntuitionMenu (Scaleform), ImGui debug widgets |
| `src/swf/` | ActionScript 2.0 widget source (`Intuition.as`) |
| `src/wheeler/` | Wheeler mod integration (v1/v2 API) |
| `src/console/` | In-game console commands (`hg`) |
| `src/override/` | Override system (urgent potion surfacing) |
| `src/persist/` | Q-learner serialization (cosave) |
| `src/settings/` | SettingsReloader (dMenu hot-reload) |

## Configuration

**Compile-time constants** — `src/Config.h` (learning rate, rewards, intervals, target limits)

**INI settings** — `Data/SKSE/Plugins/Huginn.ini`:
- `[Scoring]` — Utility scoring params (loaded by `ScorerSettings`)
- `[ContextWeights]` — 17 context weight multipliers (loaded by `ContextWeightSettings`)
- `[Widget]` — Scaleform HUD position, alpha, scale, display mode
- `[Wheeler]` — Wheeler integration settings
- `[Candidates]` — Uncastable spell policy
- `[Pages]`, `[PageN]`, `[PageN.SlotM]` — Multi-page slot layout

## Console Commands

Registered as `Huginn` with short alias `hg` (in-game `~` console):

| Command | Description |
|---------|-------------|
| `hg refresh` | Force immediate recommendation update |
| `hg reload` | Hot-reload all settings from INI |
| `hg status` | Show system status |
| `hg unlock` | Clear all slot locks |
| `hg rebuild` | Force rebuild all registries |
| `hg weights <FormID>` | Show FQL weight vector (hex FormID) |
| `hg page <N>` | Switch to page N |
| `hg reset qvalues` | Clear Q-learning tables |
| `hg reset all` | Full system reset |

## Update Loop

`UpdateHandler` triggers at ~100ms intervals:
1. Poll context sensors (read game state)
2. Pipeline skip check (sensor dirty flag + discretized hash comparison)
3. Gather candidates (spells, items)
4. Score by utility
5. Allocate to slots (classification-based, multi-page)
6. Apply slot locks (temporal stability)
7. Push to Scaleform widget + ImGui + Wheeler

## SKSE Entry Points

In `Main.cpp`:
- `SKSEPlugin_Load` — Registers messaging listener
- `kDataLoaded` — D3D hook, ImGui, StateEvaluator, IntuitionMenu, console commands
- `kPostLoadGame`/`kNewGame` — Registries, FeatureQLearner, shows IntuitionMenu

## Forbidden Information (Cheating Prevention)

The system must NOT read:
- Enemy spell lists or abilities
- Hidden trap locations
- NPC inventories (before pickpocketing)
- Locked container contents
- Future events or predictions

## Critical SKSE Patterns

**MagicTarget access:**
```cpp
// WRONG — returns nullptr (RTTI-based, fails for Actor)
auto* mt = player->As<RE::MagicTarget>();
// CORRECT — use virtual function
auto* mt = player->AsMagicTarget();
```

**Scaleform string passing:**
```cpp
// WRONG — dangling pointer if backing string is destroyed before render
arg = myString.data();
// CORRECT — CreateString copies into the movie's managed heap
uiMovie->CreateString(&arg, myString.data());
```
Always use `CreateString` for string args to `GFxValue::Invoke`. Numbers and bools are value types.

**Thread safety (StateManager):**
- Copy-out pattern: accessors return copies, not references
- `std::shared_lock` for reads, `std::unique_lock` for writes
- Build state outside lock, then copy in (short critical sections)

**Scaleform thread safety:**
All `IntuitionMenu` public API methods defer GFx work via `SKSE::GetTaskInterface()->AddUITask()`. Never call `Invoke`/`CreateString` from the update thread.

**Performance:**
- Squared distance comparisons (`distanceSq < threshold * threshold`)
- Early-exit loops when target found
- Pre-compute `constexpr` thresholds

## Logging Principles

**Log transitions, not ticks.** The debug log runs at ~10 lines/sec — don't log non-events.

1. **No "nothing happened" logs.** Absence of a log line IS the signal.
2. **Dedup periodic logs.** Only log when the result changes (use `static` last-value).
3. **Log levels:** `info` = transitions/summaries (release), `debug` = diagnostics (debug only), `trace` = per-item/per-tick (effectively off).
4. **Registration lines → trace.** Summary count at `info` is sufficient.

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — Full system design
- [docs/architecture/](docs/architecture/) — Deep-dive docs (pipeline, states, classifiers, scoring, slots, UI, dMenu)
- [docs/compatibility/](docs/compatibility/) — Mod compatibility guides
- [docs/reference/ConsoleCommands.md](docs/reference/ConsoleCommands.md) — Console command reference
- [docs/ROADMAP.md](docs/ROADMAP.md) — Development roadmap and backlog
