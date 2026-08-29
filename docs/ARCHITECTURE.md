# Huginn - Architecture

*The right tool, right when you need it*

## Vision

A **contextual quick-access system** that surfaces the right spell, potion, weapon, or item exactly when you need it - without menu diving.

That's it.

It is not:
- an autonomous agent
- a planner
- a decision-maker that replaces the player

It is a just-in-time affordance surface.


> **Core Principle:** Only recommend based on information the player already has or could easily perceive. This is a convenience tool, not a cheat tool.

> **Architecture Version:** verified against **v0.19.10** (`CMakeLists.txt:5`). `StateManager` runs 11 poll methods (`GetPollTable()` in `src/state/StateManager.cpp`) producing 6 state types (3 core: WorldState, PlayerActorState, TargetCollection; 3 tracking: HealthTrackingState, StaminaTrackingState, MagickaTrackingState). The recommendation tick is orchestrated by `PipelineCoordinator` (`src/pipeline/PipelineCoordinator.cpp`). The legacy ContextSensor system has been fully removed. See [docs/architecture/1-states.md](architecture/1-states.md) for the full state model reference.

> **On the naming:** precisely, this is `a linear contextual bandit with implicit feedback, per-action linear reward models, UCB-style exploration, and heuristic priors`. The docs use that vocabulary throughout. We deliberately avoid state transitions, TD learning, MDP framing and policy optimization — long-horizon planning is neither required nor desirable here, and the learned values are immediate preference scores, not long-term action values.
>
> **The code still says QLearner** — `FeatureQLearner`, `QLearnerSerializer`, the `FQLW` cosave record, `hg reset qvalues`. That name is historical and is kept only because renaming it would break the cosave format and a documented console command. See [4-contextual-bandits.md](architecture/4-contextual-bandits.md) for the update rule that settles which algorithm this actually is.


---

## The Problem

| Situation | Vanilla UX | With Huginn |
|-----------|------------|------------------------|
| Need specific spell from 50+ known | 3-15s menu dive | Widget + Wheeler show top recommendations |
| Weapon out of charge mid-combat | 5-20s finding (best) soul gem | "Press [hotkey] Recharge" |
| Standing in fire | 20s finding resist potion | "Press [hotkey] Resist Fire" |
| Out of mana, enemy in melee range | Menu -> Weapons -> Equip | "Press [hotkey] Equip Sword" |
| Looking at locked chest (Adept) | Menu -> Magic -> Find spell | "Press [hotkey] Open Lock" |
| On cliff edge, looking down | Hope you prepared earlier | "Press [hotkey] Slow Fall" |

> **Note:** Number of hotkeys is configurable (up to 10 per page, up to 10 pages). Integration with [Wheeler](https://www.nexusmods.com/skyrimspecialedition/mods/97345) provides radial menu support.
> **Note:** Note the issue is not with searching the menus or duration. the issue is with the frequency of access or the disruption when you are in a flow-state
---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     StateManager (11 Poll Methods)                          │
│                    (Observable state only - no cheating)                    │
├─────────────────────────────────────────────────────────────────────────────┤
│  PollWorldObjects()        -> WorldState                                    │
│    - isLookingAtLock, lockLevel, isLookingAtOreVein                         │
│    - isLookingAtWorkstation, workstationType                                │
│    - timeOfDay, lightLevel, isInterior                                      │
│                                                                             │
│  PollPlayerVitals()        -> PlayerActorState.vitals                       │
│    - health%, magicka%, stamina% (current and max)                          │
│                                                                             │
│  PollPlayerMagicEffects()  -> PlayerActorState.effects + .buffs             │
│    - DOTs: fire, frost, shock, poison, disease, health drain                │
│    - Buffs: waterbreathing, invisible, muffle, armor, cloaks, summons       │
│                                                                             │
│  PollPlayerEquipment()     -> PlayerActorState.equipment                    │
│    - weapons (left/right hand), shield, staff                               │
│    - weaponChargePercent, arrowCount, boltCount                             │
│                                                                             │
│  PollPlayerSurvival()      -> PlayerActorState.survival                     │
│    - hungerLevel, coldLevel, fatigueLevel (CC Survival Mode)                │
│                                                                             │
│  PollPlayerPosition()      -> PlayerActorState.position                     │
│    - isUnderwater, isSneaking, isInCombat, isMounted, isFalling             │
│                                                                             │
│  PollTargets()             -> TargetCollection                              │
│    - primaryTarget (crosshair > combat), up to 50 tracked actors            │
│    - per-target: vitals, distance, hostility, casting state                 │
│    - (50 is stress-test max; 8-12 recommended for production)               │
│                                                                             │
│  PollPlayerResistances()   -> PlayerActorState.resistances                  │
│    - fire, frost, shock, poison, magic resistance percentages               │
│                                                                             │
│  PollHealthTracking()      -> HealthTrackingState                           │
│    - damage/healing events, rates, elemental damage timers                  │
│                                                                             │
│  PollStaminaTracking()     -> StaminaTrackingState                          │
│    - stamina usage events, rates, source classification                     │
│                                                                             │
│  PollMagickaTracking()     -> MagickaTrackingState                          │
│    - magicka usage events, rates, casting state                             │
└────────────────────────────────────┬────────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                     6 State Types (3 core + 3 tracking)                     │
│           (Structured representation of observable game state)              │
├─────────────────────────────────────────────────────────────────────────────┤
│  WorldState           - Environment, interactables (locks, ore, stations)   │
│  PlayerActorState     - Vitals, effects, buffs, equipment, survival, pos    │
│  TargetCollection     - Primary + secondary targets with priority scoring   │
│  HealthTrackingState  - Recent damage/healing events, elemental timers      │
│  StaminaTrackingState - Stamina usage events, rates, source classification  │
│  MagickaTrackingState - Magicka usage events, rates, casting state          │
│                                                                             │
│  StateEvaluator converts raw state -> GameState (72,576 hash states)        │
│    - health/magicka buckets (6 levels each), stamina (6, excluded from hash)│
│    - distance bucket (3 levels), targetType (7 types)                       │
│    - enemyCount (4 levels), allyStatus (3 levels: None/Present/Injured)     │
│    - anyCasting (hostile casting), inCombat, isSneaking (boolean)           │
│                                                                             │
│  See: docs/architecture/1-states.md for full state model reference          │
└────────────────────────────────────┬────────────────────────────────────────┘
                                     │
        ┌────────────────────────────┼────────────────────────────┐
        │                            │                            │
        ▼                            ▼                            ▼
┌──────────────────┐     ┌──────────────────────┐   ┌──────────────────────────┐
│ Spell Classifier │     │   Item Classifier    │   │    State Models          │
│                  │     │                      │   │   (StateManager)         │
├──────────────────┤     ├──────────────────────┤   ├──────────────────────────┤
│ Classifies spell │     │ Classifies items by  │   │ Provides context for     │
│ effects by type: │     │ effect/use:          │   │ filtering:               │
│                  │     │                      │   │                          │
│ - RestoreHealth  │     │ - Healing potions    │   │ - Health% low?           │
│ - DamageFire     │     │ - Resist potions     │   │ - In combat?             │
│ - Summon         │     │ - Food (survival)    │   │ - Underwater?            │
│ - Ward           │     │ - Soul gems          │   │ - Looking at lock?       │
│ - Invisibility   │     │ - Scrolls            │   │ - Enemy distance?        │
│ - etc.           │     │ - etc.               │   │ - Ally injured?          │
│                  │     │                      │   │                          │
│ See architecture/│     │ See architecture/    │   │ See architecture/        │
│ 2-classifiers.md │     │ 2-classifiers.md     │   │ 1-states.md              │
└────────┬─────────┘     └──────────┬───────────┘   └────────────┬─────────────┘
         │                          │                            │
         └──────────────────────────┼────────────────────────────┘
                                    │
                                    ▼
         ┌──────────────────────────────────────────────────────────┐
         │                   PipelineCoordinator                    │
         │      Orchestrates 11 pipeline steps per update tick      │
         ├──────────────────────────────────────────────────────────┤
         │   1. GatherState           - Copy state snapshots        │
         │   2. ResolveDisplayPage    - Snapshot the active page    │
         │   3. CheckHashSkip         - Skip if nothing changed     │
         │   4. LogStateTransition    - Log the GameState diff      │
         │   5. EnrichElementalDamage - Elemental hit detection     │
         │   6. ScoreCandidates       - CandidateGenerator ->       │
         │                                ContextRuleEngine ->      │
         │                                UtilityScorer             │
         │   7. AllocateAndLock       - OverrideManager ->          │
         │                                SlotAllocator ->          │
         │                                SlotLocker                │
         │   8. DeriveDisplayLabels   - Subtext label per slot      │
         │   9. UpdateCaches          - PipelineStateCache          │
         │  10. PushDisplay           - IDisplayBackend             │
         │  11. LogRecommendations    - Top-N log / `hg recs`       │
         │                                                          │
         │  A step may abandon the tick: CheckHashSkip when nothing │
         │  changed, AllocateAndLock on a mid-tick page switch.     │
         │                                                          │
         │  See: docs/architecture/0-pipeline.md for details        │
         └────────────────────────────┬─────────────────────────────┘
                                      │
              ┌───────────────────────┼───────────────────────┐
              │                       │                       │
              ▼                       ▼                       ▼
┌──────────────────────┐  ┌────────────────────┐  ┌────────────────────────┐
│  Utility Scorer      │  │  Override Manager  │  │   Slot Allocator       │
│  (Step 6)            │  │  (Step 7)          │  │   + SlotLocker         │
├──────────────────────┤  ├────────────────────┤  ├────────────────────────┤
│  utility = context   │  │ Hard overrides for │  │ Best-scoring           │
│   × (1 + λ × learn)  │  │ critical states:   │  │ candidate per slot,    │
│   × correlation      │  │ - Critical HP      │  │ by classification      │
│   × potionMult       │  │ - Critical MP / SP │  │                        │
│   × favoritesMult    │  │ - Drowning         │  │ Multi-page: up to      │
│                      │  │ - Low ammo         │  │ 10 pages × 10 slots    │
│  learningScore =     │  │ - Low weapon charge│  │                        │
│  α·Q + (1-α)·prior   │  │                    │  │ SlotLocker: temporal   │
│  + β·UCB + recency   │  │ Hysteresis-based   │  │ stability (3s locks,   │
│                      │  │ enter/exit with    │  │ priority-based break)  │
│ See: architecture/   │  │ priority levels    │  │                        │
│ 4-contextual-        │  │                    │  │ See: architecture/     │
│ bandits.md           │  │ See: 5-slots.md    │  │ 5-slots.md             │
└──────────┬───────────┘  └────────┬───────────┘  └──────────┬─────────────┘
           │                       │                         │
           └───────────────────────┼─────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                    IDisplayBackend (Step 10: PushDisplay)                   │
│              Renders slot assignments to player-facing displays             │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────┐  ┌──────────────────────────────┐      │
│  │ IntuitionBackend                │  │ WheelerBackend               │      │
│  │ (Scaleform HUD — Primary)       │  │ (Radial Menu — Optional)     │      │
│  │  IntuitionMenu -> Intuition.as  │  │  WheelerClient -> API        │      │
│  │  12 slot content types          │  │  API v1-v4 compatible        │      │
│  │  Slide/fade/instant animations  │  │  Subtext labels (v2+)        │      │
│  │  See: architecture/             │  │  See: architecture/          │      │
│  │  6-ui-ux.md                     │  │  6-ui-ux.md                  │      │
│  └─────────────────────────────────┘  └──────────────────────────────┘      │
│                                                                             │
│  ImGui Debug Widgets (debug builds only):                                   │
│    UtilityScorerDebugWidget, StateManagerDebugWidget,                       │
│    RegistryDebugWidget                                                      │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Feedback Loop

```
Player equips item ──► EquipEventBus ──► Subscribers:
                                          ├── FQLSubscriber (FeatureQLearner weight update)
                                          ├── UsageMemorySubscriber (recency tracking + misclick)
                                          └── CooldownSubscriber (candidate cooldown)

Equip sources:
  Wheeler selection / hotkey 1-0  ──► +8.0 EQUIP_REWARD (direct reinforcement)
  Potion/scroll consumption       ──► +5.0 CONSUME_REWARD
  Vanilla menu / favorites        ──► ExternalEquipLearner (tiered 0.0-8.0 by attribution)
  Rapid equip-then-switch (<3s)   ──► -3.0 MISCLICK_PENALTY

Weight decay:
  L2 regularization (FeatureQLearner::L2_LAMBDA = 0.01) folded into each weight update
  Time-based MaybeDecay() — 2%/hr exponential decay on items idle > 5 min

Update rule (FeatureQLearner::Update):
  error = reward - w·φ            (no bootstrapped successor term — see "On the naming")
  w[i] += α·error·φ[i] - α·λ·w[i] (18-float φ, clamped)

See: docs/architecture/4-contextual-bandits.md for full learning system details
```

Constants live in `src/Config.h` (`EQUIP_REWARD`, `CONSUME_REWARD`, `MISCLICK_PENALTY`,
`MISCLICK_WINDOW_SECONDS`, `DECAY_RATE_PER_HOUR`, `DECAY_THRESHOLD_MINUTES`); the
subscribers in `src/learning/EquipSubscribers.h`; the attribution tiers in
`src/learning/ExternalEquipLearner.h`.

---

## Settings Management

Huginn settings are split across two INI files to support dMenu integration:

- **Main INI** (`Data/SKSE/Plugins/Huginn.ini`, shipped as `configs/Huginn.ini`) — `[Overrides]`, `[Candidates]`, `[Scoring]`, `[Favorites]`, `[ContextWeights]`, `[Wildcards]`, `[Subtexts]`, `[Wheeler]`, `[Pages]` + `[PageN]` / `[PageN.SlotM]`, `[Learning]`, `[SlotLocker]`, `[Keybindings]`
- **dMenu INI** (`Data/SKSE/Plugins/dmenu/customSettings/ini/Huginn.ini`) — `[Widget]` and `[Debug]` only, when dMenu is installed (`GetDMenuIniPath()` in `src/Globals.cpp`). `[Keybindings]` stays in the main INI

One key, one home: dMenu owns `[Widget]` and `[Debug]`; nothing is overlaid from both files. `SettingsReloader` handles hot-reload via the SKSE `ModCallbackEvent`s `dmenu_updateSettings` and `dmenu_buttonCallback`, serializing itself through `UpdateHandler::RunExclusive`. See [docs/architecture/7-dmenu-integration.md](architecture/7-dmenu-integration.md) for details.

---

For detailed documentation on specific subsystems, see [docs/architecture/](architecture/):

| Document | Contents |
|----------|----------|
| [0-pipeline.md](architecture/0-pipeline.md) | Full recommendation pipeline with data flow |
| [1-states.md](architecture/1-states.md) | State models and StateManager polling |
| [2-classifiers.md](architecture/2-classifiers.md) | Spell, item, scroll, weapon classification |
| [3-candidate-filtering.md](architecture/3-candidate-filtering.md) | Candidate generation and filtering |
| [4-contextual-bandits.md](architecture/4-contextual-bandits.md) | Feature-based contextual bandit learning and reward system |
| [5-slots.md](architecture/5-slots.md) | Slot types, overrides, wildcards, multi-page |
| [6-ui-ux.md](architecture/6-ui-ux.md) | Intuition widget and Wheeler integration |
| [7-dmenu-integration.md](architecture/7-dmenu-integration.md) | dMenu integration and two-INI architecture |
| [8-future-work.md](architecture/8-future-work.md) | Deferred ideas: temporal prediction, urgency multipliers, HMM combat states |

And the reference material in [docs/reference/](reference/):

| Document | Contents |
|----------|----------|
| [ConsoleCommands.md](reference/ConsoleCommands.md) | The `hg` console commands |
| [candidate-system.md](reference/candidate-system.md) | Candidate registries and generation |
| [Performance.md](reference/Performance.md) | Performance budget and measurements |
| [intuition-scaleform-build.md](reference/intuition-scaleform-build.md) | Building `Intuition.swf` from `src/swf/Intuition.as` |
| [WheelerAPI_minimal.h](reference/WheelerAPI_minimal.h), [WheelerAPIClient.h](reference/WheelerAPIClient.h) | Vendored Wheeler API headers for reference |

Provenance and staleness for the whole tree: [docs/README.md](README.md).
