# CLAUDE.md

This file provides guidance to Claude Code when working with code in this repository.

## Project Overview

Huginn is an SKSE plugin for Skyrim — a **contextual quick-access system** that surfaces the right spell, potion, weapon, or item exactly when you need it.

> **Core Principle:** Only recommend based on information the player already has or could easily perceive. This is a convenience tool, not a cheat tool.

## Build Commands

```sh
cmake --preset vs2022-windows && cmake --build build --config Debug
```

**Prerequisites:** Visual Studio 2022 (v143), CMake 3.25+, vcpkg (`VCPKG_ROOT`), CommonLibSSE-NG v3.7.0 (`CommonLibSSEPath_NG`), `CompiledPluginsPath` set to your SKSE plugins output directory.

**Warnings:** Huginn's own targets (the plugin, `huginn_core_tests`, `huginn_effect_report`) build at `/W4 /WX`, set in `cmake/HuginnWarnings.cmake`; CommonLib and Tracy are added as SYSTEM and stay at `/W0`. Configure with `-DHUGINN_WARNINGS_AS_ERRORS=OFF` to keep the warnings without failing the build.

**Clean build:** Delete `build/`, then reconfigure and rebuild.

CMakeLists.txt uses `GLOB_RECURSE` — new source files are picked up at configure time (no need to edit CMakeLists).

## Architecture Pipeline

```
Game (SKSE) → StateManager (11 polls) → 6 state types ──→ need vector n (NeedMonitor, [Needs] curves)
                                                        ↓
Registries → Candidates → Filters (facets of cap(i)) → Choice score μ → Slot allocation → Widget / Wheeler
                              ↑ effect catalog: cap(i) per item ↑      (classes from facets, challenger hold, wildcard key)
```

**The score** (doc 9; [docs/architecture/9-implementation-map.md](docs/architecture/9-implementation-map.md) Phases 6-10):
```
μ(item) = Σ_j v_j · a_j(item) + b(item) + θ_covered · covered(item) + w · ln learn mult(item) + w' · util missing(item)
          v_j = Σ_k n_k θ[k][j] + Σ_f recent_f θ_f[j]      (n = the need vector, a = cap(i) + runtime cross-features)
```
Every candidate that passes the filters is ranked on μ (`ScoredCandidate::score`), any sign. θ (need × effect column), b (per item), κ (the menu's cost) and `recent_<family>` start from `Huginn_Bootstrap.json` (R6's offline fit) and are learned online by `ChoiceLearner` (Var_p precision update, Plackett-Luce over co-picks, the outside option u0). The slot hold keeps a key unless a challenger beats it by more than their joint uncertainty (R9, `core/ChallengerRule.h`); one wildcard key per page is drawn by relevance × sd (R10).

**D2, the one part of the old engine kept, and why.** R6's fit found no variant without the old learner that passed its gate, and chose D2: θ + b + recent **plus the old learner's learning score**, `ln learn mult = ln(1 + λ(α)·learningScore)`, on the rows the old floors keep (`util missing` = 1 on the others), weight 1.36. R6 could not tell D2 ("engine A") from the same model without it ("engine B": `ln learn mult` 0 and `util missing` 1 on every row) because of display bias (every logged pick was made on a page the old engine built), so since 0.24.17 **B is live and A runs in shadow** (the user's call, 2026-10-30: judge B by feel while A is mirrored in the background). `[Scoring] bLegacyLearnFeature` (default **off** since 0.24.17, hot-reloadable; on = A live, both supported) picks what the live score reads; `[Scoring] bShadowLegacy` (default on, hot-reloadable; moot with the feature on) computes A beside B on every run that ranks: A's μ (B's run with the feature put back, `core/ChoiceLive.h` RankMu), each row's rank under A and B, and A's and B's pages built WITHOUT memory from the live allocation's input (`core/SlotAllocStateless.h`; B's stateless page is the control), logged on the v3 rows (`amu ar br ak bk`) and in the `[Soak]` line's last field (`shadowA ...`), never shown, never learned from, never saved; `tools/shadow/shadow_compare.py` reads the logs (menu reach-ins A's page would have caught). Either switch on runs the legacy pass (`src/learning/legacy/`): FeatureBanditLearner, UsageMemory, PriorCalculator, CorrelationBooster, PotionDiscriminator, the old context rules as a binary gate, the floors and the cold start, arithmetic unchanged; and with them the old spell / item / scroll classifiers, because the context rules read their tags. Nothing in that pass ranks or places anything by itself. It is confined mechanically: `python -I tools/prune/r8_check.py` must pass (LEGACY: the old engine only in allowlisted or budgeted files; CLASSIFIER, R11: the classifiers' tags and types only in their own files and D2's inputs) and every mutation probe in `tools/prune/test_r8_check.py` must fail it. It retires, with BNDW, when a refit on 0.23.53+ logs passes R6's gate without it, or by the user's call. Without a usable bootstrap the pipeline ranks on the old utility instead (ln u, the FALLBACK; `hg status` and an error line say so).

**The live engine reads items through the effect catalog** (R11): `EffectCatalog` maps every form in the load order to cap(i) (`core/EffectColumns.h`, the columns of `docs/architecture/9-data/effects.csv`); `core/CapFacets.h` answers the yes/no questions the slot classes, the candidate filters, the override finders, the explanation label and the widget's styling ask ("restores health?", "for someone else?", "a poison?"), through `effect/Facets.h`. The classifiers' tags are not read outside D2.

**Key architectural separation:**
- `ChoiceLearner`: θ, b, recent, the bootstrap, the online update, the discovery boost (what the character has held, and what it newly gained: b's variance × m(age) in the wildcard draw only, `core/DiscoveryBoost.h`), the cosave records THTA / BIAS / DISC (`core/ChoiceScore.h`, `core/ChoiceLive.h`, `core/ChoiceUpdate.h`)
- `PipelineCoordinator`: one tick: generate, filter, rank (`RankOnChoice`), allocate, lock, display, log (v3 rows)
- `EffectCatalog` + `core/CapFacets.h`: what an item is and does (cap(i), facets); `NeedMonitor` + `core/NeedEvaluator.h`: what the situation needs (n)
- `UtilityScorer`: the sort, the magicka / stamina envelopes, `[Recs]`; no old-engine code
- `learning/legacy/` + `context/ContextRuleEngine` + the classifiers: D2's feature only (above)

See [docs/README.md](docs/README.md) for the full system design.

## Key Directories

| Directory | Purpose |
|-----------|---------|
| `src/state/` | StateManager + 11 poll methods, 6 state types |
| `src/needs/` | NeedMonitor (the need vector every tick), the snapshot builder (`hg needs`), [Needs] settings, the R3 capture |
| `src/effect/` | EffectReader (forms -> records), EffectCatalog (cap(i) per item, built when the main menu first opens; the override files, R11), Facets (the live engine's questions of a candidate, R11), CrossFeatures, `hg dump all` (every build since R11) |
| `src/candidate/` | Candidate generation and the filters (hard gates, full vitals, active buffs, resists, summon limit; from the candidate's facets since R11) |
| `src/learning/` | ChoiceLearner (the model), ChoiceShadow, UtilityScorer (sort, envelopes, [Recs]), WildcardManager, the selection logs (v3 rows the learner trains on; v2 kept for audit), the equip event bus and subscribers; `legacy/` (D2's feature: LegacyScorer, LegacyLearnFeature, LegacyRun) and the old learner's files it needs (FeatureBanditLearner, UsageMemory, PriorCalculator, CorrelationBooster, PotionDiscriminator) |
| `src/spell/`, `src/learning/item/`, `src/weapon/`, `src/scroll/`, `src/apparel/` | Registries (what the player carries and knows) and the old classifiers; since R11 the classifiers' tags and types feed only D2 (`r8_check` CLASSIFIER) |
| `src/slot/` | Slot allocation, the hold and the challenger rule, Remembrance, the class cap, slot classes (`SlotClassifier`: core/CapFacets.h over the catalog, R11) |
| `src/context/` | ContextFacts (the facts the hard gates and the skip gate read); ReasonHold (the label's hold); ContextRuleEngine and the context weights (D2's floors' input only) |
| `src/display/` | The display backends (Intuition widget, Wheeler) and the explanation label |
| `src/ui/` | IntuitionMenu (Scaleform), ImGui debug widgets |
| `src/swf/` | ActionScript 2.0 widget source (`Intuition.as`) |
| `src/wheeler/` | Wheeler mod integration (v1/v2 API) |
| `src/console/` | In-game console commands (`hg`) |
| `src/override/` | Override system (urgent restores, cures, ammo, waterbreathing, soul gems); its finders read the catalog's facets since R11 |
| `src/persist/` | Cosave: HCID (character, learning clock; `CharacterRecord`, the decision in `core/LearningClock`, 0.24.22), THTA / BIAS / DISC (the choice learner's θ by stable key, b per item, the discovery boost's seen and boosted forms; `ChoiceSerializer`, codec in `core/ChoiceCosave`), BNDW (the old learner; stays while D2 reads it) |
| `src/telemetry/` | The `[Soak]` heartbeat (goals, churn, rates, θ drift since R11) and the slot-change causes |
| `src/settings/` | SettingsReloader (dMenu hot-reload, Reset to Defaults); ShippedDefaults + IniOverlay: every settings INI is read over the shipped file compiled into the DLL (`cmake/EmbedShipped.cmake`), so `configs/Huginn.ini` and the dMenu descriptor are the defaults (0.24.10) |
| `src/core/` | Pure, host-tested code (`huginn_core_tests`, `src/core/README.md`): the effect mapper (EffectColumns, EffectRecords, EffectRules, EffectConditions, EffectMapper, CrossFeatures), the override files (OverrideFiles, R11) and the facets (CapFacets, with the slot classes' rules; SlotClassification moved here, R11); the need vector and its sensors (NeedEvaluator, NeedIds, NeedSnapshot, NeedSnapshotIO, NeedSensorMath, ResponseCurve, DropAhead, EdgeApproach, WaterSelfCheck, FallTracker, BenchKind, WeaponLoadout); the model (MiniJson, Bootstrap, ObviousPairs, Sha256, ChoiceScore, ChoiceUpdate, ChoiceLive, ChoiceBeliefs, ChoiceCosave, Battery; ThetaDrift, R11; DiscoveryBoost, 0.24.14; LearningClock (the shared learning clock, its tick table and the load's keep-or-replace decision), 0.24.22); the slot math (SlotAllocCore, SlotClassCapMath, SlotScoreMath, SlotSnapshotIO, ChallengerRule, WildcardPick, WildcardGate, SlotAllocStateless (0.24.17)); the D2 shadow: its pass (LegacyShadowPass: engine A's mu, ranks and stateless pages from const inputs), ranks and [Soak] tally (ShadowCompare, 0.24.17); the logs (DecisionLog v3, NeedEpisodes and the lap clock's stick-deadzone and menu-flag holds (LapHoldInputs, 0.23.55 on engine-rewrite), OldScore, ReachLog); Coverage, FilterReason, HardGates, RefreshPass, CombatOnset; ActorTypeClassifier, TargetType, TargetFamilies; MiniRegex, SlicedWalk, RingBuffer, SignatureDedup; DMenuDefaults (the dMenu descriptor's defaults, 0.24.10); HandEquipGate (a slot key's equip waits for the cast to be released, never interrupts it, drops after 3 s; 0.24.26) |

## Configuration

**Compile-time constants**: `src/Config.h` (intervals, target limits, the old learner's constants D2 uses)

**INI settings**: `Data/SKSE/Plugins/Huginn.ini`, read over the shipped `configs/Huginn.ini` (compiled in): a key the player's file leaves out reads the shipped value, and dMenu's Reset to Defaults writes the shipped file back (the old one kept as `Huginn.ini.<YYYYMMDD-HHMMSS>.bak`):
- `[Scoring]`: `bLegacyLearnFeature` (D2's feature: off = engine B live, the 0.24.17 default; on = engine A live), `bShadowLegacy` (0.24.17: A in shadow beside B, logged only), both hot-reloadable, `fCoveredPrior`, `fRefreshPriorSec` (the active-buff filters' refresh window); the old scoring params (`ScorerSettings`) are D2's inputs only
- `[Learning]`: `sBootstrapBias` (b's seed from the bootstrap), `fThetaProcessNoise`, the memory life, which both learners use (ChoiceLearner's b battery; the old learner's for D2), and the discovery boost (`bDiscoveryBoost`, `fDiscoveryBoostVarMult`, `fDiscoveryBoostHalfLifeSec`; hot-reloadable)
- `[Favorites]`: the favourites mode (Boost / Suppress) and `fFavoriteLifeMult`
- `[ContextWeights]`: the old context weights (`ContextWeightSettings`), D2's floors' input only
- `[Needs]`: one response curve per need (`<need> = <kind> <p1> <p2>`; defaults from `docs/architecture/9-data/needs.csv`, `tools/needs/make_need_ids.py`); a curve the bootstrap carries (encumbrance) overrides its line
- `[Wildcards]`, `[SlotLocker]` (the hold, the challenger rule, the class cap, Remembrance, home keys): slot behaviour
- `[Wheeler]`: Wheeler integration; `[Subtexts]`: Wheeler's entry labels (v2 API)
- `[Keybindings]`: the slot keys and page keys (main INI since 0.19.0)
- `[Equip]` (0.24.26): `bRedrawAfterHandEquip` (default on, hot-reloadable): after a key equips a spell, scroll, weapon or staff while drawn (weapon state kDrawn), `DrawWeaponMagicHands(true)`, as Wheeler-Refined does. Not settings: a hand equip waits for a casting hand and goes when it is released, never interrupting the cast (`input/HandEquipGate`, `core/HandEquipGate.h`; still casting after 3 s drops the press; caster states 7-9 are waited on 250 ms), and the slot keys are left to the game in a loading screen, the main menu or a pausing menu
- `[Candidates]`: uncastable spell policy, soul-gem recharge candidates
- `[Overrides]`: the urgent overrides' thresholds
- `[Pages]`, `[PageN]`, `[PageN.SlotM]`: multi-page slot layout (`sClassification` names a slot class; what it matches comes from cap(i), docs/architecture/5-slots.md)

**dMenu INI**: `Data/SKSE/Plugins/dmenu/customSettings/ini/Huginn.ini`, bound by `Data/SKSE/Plugins/dmenu/customSettings/Huginn.json`: `[Widget]` (Scaleform HUD position, alpha, scale, display mode) and `[Debug]`; read from the main Huginn.ini only when that file is absent (`GetDMenuIniPath`).

**Bootstrap**: `configs/Huginn_Bootstrap.json` (installed to `Data/SKSE/Plugins/`): R6's fit (θ, κ, `recent_<family>`, b, the encumbrance curve, the D2 weights; format: `docs/architecture/9-bootstrap-format.md`). Missing or invalid: the FALLBACK ranking.

**Effect overrides** (R11, `core/OverrideFiles.h`): `Data/SKSE/Plugins/Huginn/EffectOverrides/*.ini` (mods' files, alphabetical), then the player's `Data/SKSE/Plugins/Huginn_EffectOverrides.ini` last: `[Overrides] <plugin>|<local FormID hex> = <effects.csv column>`, `[Huginn] iFormatVersion = 1`. They pin an effect's column in cap(i), so the score and the facets both see it. `Huginn_Overrides.ini` (type / tag corrections) belongs to the old classifiers and so reaches only D2.

## Console Commands

Registered as `Huginn` with short alias `hg` (in-game `~` console):

| Command | Description |
|---------|-------------|
| `hg refresh` | Force immediate recommendation update |
| `hg recs [N]` | Dump top-N to the log (default 10): μ, sd, the top three θ·n·cap terms, b, the D2 feature and its inputs; the header carries the run's outside option u0 (R11) |
| `hg needs` | Print the live need vector: non-zero needs, value, curve input, where each curve came from (default / [Needs] / bootstrap) |
| `hg theta [FormID]` | R11. No argument: what ranks, θ / b counts and drift from the bootstrap (means moved, mean and largest change and where, mean ln variance ratio), b's spread, the outside option now. With a hex FormID: that item's b (and its discovery boost, if newly gained), cap(i), facets, every θ·n·a term the live need vector gives it, and its largest weights under any need |
| `hg dump theta` | R11. Write every moved θ weight (beside the bootstrap's) and every b to `Huginn_Theta.csv` |
| `hg reload` | Hot-reload all settings from INI |
| `hg status` | System status: what ranks (the choice score, or the FALLBACK), θ entries and moved, b count, picks learned, the discovery boost (on/off, forms known, boosted now), the D2 switch, the old learner's counts, the registries, the page |
| `hg unlock` | Clear all slot locks |
| `hg rebuild` | Force rebuild all registries |
| `hg weights <FormID>` | The OLD learner's weight vector for an item (hex FormID): D2's input, not the ranking |
| `hg dump all` | Every item × effect in the load order with the catalog's view (effect column, cap(i), the slot class it derives) to `Huginn_All.csv`; every build since R11 (mod authors write effect overrides from it) |
| `hg cap <FormID>` | An item's effect columns, per-stack cap, runtime cross-features, its perk-gated rows and the perk-adjusted cap (debug builds) |
| `hg dump weights` | Every OLD learner entry to `Huginn_Weights.csv` (kind and facets from the catalog; debug builds) |
| `hg dump races` | Every race, its keywords and NPC count to `Huginn_Races.csv` (debug builds) |
| `hg actors` | List the actors the target poll tracks right now: name, race, type, hostile, dead, summon and its commander, distance band; the scoring target marked (to the console and the log) |
| `hg hands` | Where each hand's weapon model hangs in both bipeds (culled, detached, missing) and the swap that put it there; for an equipped weapon that does not show (to the console and the log) |
| `hg page <N>` | Switch to page N |
| `hg reset weights` | θ back to the bootstrap, b cleared; the old learner (D2's input) cleared too; the discovery boost's sets are kept (what the character has held is not a learned weight). The dMenu "Reset Learned Weights" button does the same |
| `hg reset all` | Full system reset (the above, registries rebuilt, every pipeline subsystem reset) |

R11 removed `hg dump spells`, `food`, `potions`, `scrolls`, `weapons`, `apparel` and `diseases` with the classifiers they printed; `hg dump all` replaces them.

## Update Loop

`UpdateHandler` triggers at ~100ms intervals:
1. Poll context sensors (read game state); evaluate the need vector
2. Pipeline skip check (sensor dirty flag, discretized hash, the need signature, the learners' weights-changed latches)
3. Gather candidates from the registries; filter them on their facets (cap(i)) and the hard gates
4. The D2 legacy pass (behind `bLegacyLearnFeature` or `bShadowLegacy`), then every candidate ranked on μ (and, with B live and the shadow on, A's μ beside it)
5. Allocate to slots (slot classes from facets, the class cap, multi-page; the wildcard key); the D2 shadow's ranks and stateless pages (`ShadowLegacyPages`, logged only)
6. The slot hold: locks, the challenger rule, Remembrance, home keys
7. Push to the Scaleform widget + ImGui + Wheeler; log the v3 rows and `[Recs]`

## SKSE Entry Points

In `Main.cpp`:
- `SKSEPlugin_Load`: registers the messaging listener; installs the code hooks (`InstallHooks`: D3D11 Present, and the Debug input hook) before any game thread runs them
- `kDataLoaded`: ImGui, StateEvaluator, IntuitionMenu, console commands; the effect catalog's build waits for the main menu to open
- `kPostLoadGame`/`kNewGame`: registries, the learners (ChoiceLearner reads the bootstrap the first time; FeatureBanditLearner for D2), the cosave restore, shows IntuitionMenu

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

- [docs/README.md](docs/README.md) — Full system design
- [docs/architecture/](docs/architecture/) — Deep-dive docs (pipeline, states, slots, UI, dMenu; doc 9 and its implementation map for the engine; `archive/` for the old learner)
- [Mod compatibility](https://github.com/LandingCrew/huginn/wiki/Mod-Compatibility) — mod compatibility guides (wiki).
  Player-facing; the Wheeler instance-identity rule lives here rather than in
  the repo. A wiki link that 404s means the page has not been created yet —
  GitHub only creates the wiki repo on the first page saved through the web UI.
- Console commands — the table under [Console Commands](#console-commands)
  above is the reference. There is no wiki page; the earlier link here pointed
  at one that was never created, for content already in this file.
- [docs/testing/TESTING-INDEX.md](docs/testing/TESTING-INDEX.md) — Test suite index and profiling guide
- [docs/roadmap.md](docs/roadmap.md) — Development roadmap and backlog
