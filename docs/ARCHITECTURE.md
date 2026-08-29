# Huginn - Architecture

*The right tool, right when you need it*

## Vision

A **contextual quick-access system** that surfaces the a spell, potion, weapon, or item exactly when you need it - without menu diving.

That's it.

It is not:
- an autonomous agent
- a planner
- a decision-maker that replaces the player

It is a just-in-time affordance surface.


> **Core Principle:** Only recommend based on information the player already has or could easily perceive. This is a convenience tool, not a cheat tool.

> **Architecture Version:** v0.13.x uses StateManager with 11 poll methods producing 6 state types (3 core: WorldState, PlayerActorState, TargetCollection; 3 tracking: HealthTrackingState, StaminaTrackingState, MagickaTrackingState). Orchestrated by PipelineCoordinator (v0.14.x). The legacy ContextSensor system has been fully removed. See [docs/architecture/1-states.md](architecture/1-states.md) for full state model reference.

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
│                           6 State Types (v0.6.9+)                           │
│           (Structured representation of observable game state)              │
├─────────────────────────────────────────────────────────────────────────────┤
│  WorldState           - Environment, interactables (locks, ore, stations)   │
│  PlayerActorState     - Vitals, effects, buffs, equipment, survival, pos    │
│  TargetCollection     - Primary + secondary targets with priority scoring   │
│  HealthTrackingState  - Recent damage/healing events, elemental timers      │
│  StaminaTrackingState - Stamina usage events, rates, source classification  │
│  MagickaTrackingState - Magicka usage events, rates, casting state          │
│                                                                             │
│  StateEvaluator converts raw state -> GameState (36,288 hash states)        │
│    - health/magicka buckets (6 levels each), stamina (6, excluded from hash)│
│    - distance bucket (3 levels), targetType (7 types)                       │
│    - enemyCount (4 levels), allyStatus (3 levels: None/Present/Injured)     │
│    - inCombat, isSneaking (boolean)                                         │
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
         │          PipelineCoordinator (v0.14.x)                   │
         │       Orchestrates 8 pipeline steps per update tick      │
         ├──────────────────────────────────────────────────────────┤
         │  1. GatherState          - Copy state snapshots          │
         │  2. HashSkip             - Skip if state unchanged       │
         │  3. LogStateTransition   - Debug logging                 │
         │  4. EnrichElementalDamage - Elemental hit detection      │
         │  5. ScoreCandidates      - CandidateGenerator ->         │
         │                            ContextRuleEngine ->          │
         │                            UtilityScorer                 │
         │  6. AllocateAndLock      - OverrideManager ->            │
         │                            SlotAllocator ->              │
         │                            SlotLocker                    │
         │  7. UpdateCaches         - PipelineStateCache            │
         │  8. PushDisplay          - IDisplayBackend               │
         │                                                          │
         │  See: docs/architecture/0-pipeline.md for details        │
         └────────────────────────────┬─────────────────────────────┘
                                      │
              ┌───────────────────────┼───────────────────────┐
              │                       │                       │
              ▼                       ▼                       ▼
┌──────────────────────┐  ┌────────────────────┐  ┌────────────────────────┐
│  Utility Scorer      │  │  Override Manager  │  │   Slot Allocator       │
│  (Step 5)            │  │  (Step 6)          │  │   + SlotLocker         │
├──────────────────────┤  ├────────────────────┤  ├────────────────────────┤
│  utility = context   │  │ Hard overrides for │  │ Top N by utility ->    │
│   × (1 + λ × learn)  │  │ critical states:   │  │ classified slots       │
│   × correlation      │  │ - Critical HP      │  │                        │
│   × potionMult       │  │ - Drowning         │  │ Multi-page: up to 10   │
│   × favoritesMult    │  │ - Taking element   │  │ pages × 10 slots       │
│                      │  │   damage           │  │                        │
│  learningScore =     │  │ - Low weapon charge│  │ SlotLocker: temporal   │
│  α·Q + (1-α)·prior   │  │                    │  │ stability (3s locks,   │
│  + β·UCB + recency   │  │ Hysteresis-based   │  │ priority-based break)  │
│                      │  │ enter/exit with     │  │                        │
│ See: architecture/   │  │ priority levels     │  │ See: architecture/     │
│ 4-contextual-bandits.md       │  │                    │  │ 5-slots.md             │
└──────────┬───────────┘  └────────┬───────────┘  └──────────┬─────────────┘
           │                       │                         │
           └───────────────────────┼─────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                   IDisplayBackend (Step 8: PushDisplay)                     │
│              Renders slot assignments to player-facing displays             │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────┐  ┌────────────────────────────┐       │
│  │ IntuitionBackend               │  │ WheelerBackend             │       │
│  │ (Scaleform HUD — Primary)      │  │ (Radial Menu — Optional)   │       │
│  │  IntuitionMenu -> Intuition.as  │  │   WheelerClient -> API     │       │
│  │  9 slot types with colors       │  │   v1/v2 API compatible     │       │
│  │  Slide/fade/instant animations  │  │   Subtext labels (v2)      │       │
│  │  See: architecture/6-ui-ux.md   │  │   See: architecture/       │       │
│  │                                 │  │   6-ui-ux.md               │       │
│  └─────────────────────────────────┘  └────────────────────────────┘       │
│                                                                             │
│  ImGui Debug Widgets (debug builds only):                                   │
│    UtilityScorerDebugWidget, StateManagerDebugWidget, RegistryDebugWidget   │
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
  L2 regularization (λ=0.01) during gradient descent — continuous per-update dampening
  Time-based MaybeDecay() — 2%/hr exponential decay on items idle > 5 min

See: docs/architecture/4-contextual-bandits.md for full learning system details
```

---

## Settings Management

Huginn settings are split across two INI files to support dMenu integration:

- **Main INI** (`Huginn.ini`) — All sections: Scoring, ContextWeights, Slot, Override, Learning, Wheeler, Candidates, etc.
- **dMenu INI** (`dmenu/.../ini/Huginn.ini`) — Widget, Keybindings, Debug only (dMenu-managed subset)

`SettingsReloader` handles hot-reload via SKSE `ModCallbackEvent`. See [docs/architecture/7-dmenu-integration.md](architecture/7-dmenu-integration.md) for details.

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
| [8-future-work.md](architecture/8-future-work.md) | Future work: temporal prediction, SARSA, HMM |
