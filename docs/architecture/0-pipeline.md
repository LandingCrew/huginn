# Huginn Recommendation Pipeline

This document describes the data flow from game state to slot recommendations as currently implemented in v0.18.x.

> **Related documentation:**
> - [1-states.md](1-states.md) - State models (WorldState, PlayerActorState, TargetCollection, tracking states)
> - [4-contextual-bandits.md](4-contextual-bandits.md) - contextual bandit learning implementation
> - [5-slots.md](5-slots.md) - Slot classification and overrides

---

## Current Implementation Status (v0.18.x)

**Scoring Formula:**
```
utility = contextWeight × (1 + λ(confidence) × learningScore)
          × correlationBonus × potionMultiplier × favoritesMultiplier
```

Where `λ(confidence) = lambdaMin + confidence × (lambdaMax − lambdaMin)`. Context acts as a gate: zero context = zero utility regardless of learning.

**Architecture Progress:**
- ✅ StateManager with 11 poll methods (complete)
- ✅ ContextRuleEngine — sole source of context weights
- ✅ CandidateGenerator relevance refactored — tags only, no scoring
- ✅ UtilityScorer — multiplicative formula + cold-start UCB fallback
- ✅ UsageMemory — event-driven short-term recency boost
- ✅ SlotAllocator with multi-page support
- ✅ Two-tier lazy decay (replaces skip penalties)
- ✅ contextual bandit learning persistence via SKSE cosave
- ✅ PipelineStateCache for external equip attribution
- ✅ ExternalEquipLearner for learning from vanilla/hotkey equips
- ✅ Cold-start UCB context boost for empty-slot prevention
- ✅ PipelineCoordinator extraction (orchestrates all pipeline steps)
- ✅ IDisplayBackend interface (pluggable display targets)
- ✅ Legacy additive formula removed — single scoring path

---

## Pipeline Overview

```mermaid
graph TB
    Game[Game Engine SKSE] -->|polling ~100ms| SM[StateManager]
    SM -->|11 poll methods| States[6 State Types]

    States --> WS[WorldState]
    States --> PAS[PlayerActorState]
    States --> TC[TargetCollection]
    States --> HTS[HealthTrackingState]
    States --> STS[StaminaTrackingState]
    States --> MTS[MagickaTrackingState]

    WS --> PC[PipelineCoordinator]
    PAS --> PC
    TC --> PC
    HTS --> PC
    STS --> PC
    MTS --> PC

    PC -->|GatherState| SE[StateEvaluator]
    SE -->|discretized| GS[GameState Hash]

    PC -->|EnrichElementalDamage| Enrich[Bridge elemental timers<br/>→ effect flags]
    HTS --> Enrich

    PC -->|ScoreCandidates| CG[CandidateGenerator]
    CG -->|candidates| US[UtilityScorer]

    GS --> CRE[ContextRuleEngine]
    WS --> CRE
    PAS --> CRE
    TC --> CRE
    CRE -->|context weights| US

    GS --> QL[FeatureQLearner]
    QL -->|reward estimates| US

    UM[UsageMemory] -->|recency boost| US

    PC -->|AllocateAndLock| SA[SlotAllocator]
    US -->|scored candidates| SA
    SA -->|slot assignments| SL[SlotLocker]
    SL -->|stable slots| VS[ComputeVisualStates]
    VS -->|annotated slots| Display

    PC -->|UpdateCaches| PSC[PipelineStateCache]
    PSC -->|attribution data| EEL[ExternalEquipLearner]
    EEL -->|tiered reward| QL

    PC -->|PushDisplay| Display[IDisplayBackend]
    Display --> IB[IntuitionBackend<br/>Scaleform HUD]
    Display --> WB[WheelerBackend<br/>Radial Menu]

    style SM fill:#e1f5ff
    style PC fill:#f0e1ff
    style US fill:#fff4e1
    style CRE fill:#e1ffe1
    style SA fill:#ffe1f5
    style PSC fill:#ffe1e1
    style EEL fill:#ffe1e1
    style UM fill:#fff4e1
    style Display fill:#e1f5ff
```

**Key Components:**

| Component | Role | Source File |
|-----------|------|-------------|
| **StateManager** | Polls game state at intervals (100ms-1000ms) | `src/state/StateManager.h` |
| **StateEvaluator** | Discretizes state for Q-table lookup | `src/state/StateEvaluator.h` |
| **PipelineCoordinator** | Orchestrates all pipeline steps in sequence | `src/pipeline/PipelineCoordinator.h` |
| **CandidateGenerator** | Gathers available items + tags for filtering | `src/candidate/CandidateGenerator.h` |
| **ContextRuleEngine** | Evaluates context rules → weight map | `src/context/ContextRuleEngine.h` |
| **UtilityScorer** | Combines context + learning → utility | `src/learning/UtilityScorer.h` |
| **UsageMemory** | Event-driven short-term recency boost | `src/learning/UsageMemory.h` |
| **OverrideManager** | Evaluates urgent override conditions | `src/override/OverrideManager.h` |
| **SlotAllocator** | Assigns candidates to classified slots | `src/slot/SlotAllocator.h` |
| **SlotLocker** | Temporal stability via lock timers | `src/slot/SlotLocker.h` |
| **PipelineStateCache** | Caches scored candidates for external equip attribution | `src/learning/PipelineStateCache.h` |
| **ExternalEquipLearner** | Learns from vanilla menu/hotkey equips | `src/learning/ExternalEquipLearner.h` |
| **IDisplayBackend** | Interface for display targets (Intuition, Wheeler) | `src/display/IDisplayBackend.h` |

---

## PipelineCoordinator

The `PipelineCoordinator` singleton orchestrates the full recommendation pipeline. Extracted from the original `OnUpdate()` function, each step method maps 1:1 to a block of the original code. Data flows through a `PipelineContext` struct that makes inter-step dependencies explicit.

**Step execution order** (`PipelineCoordinator::RunPipeline()`):

```
1. GatherState          — Fetch state snapshots from StateManager + StateEvaluator
2. ResolveDisplayPage   — Sync active page to the backend's desired page (IDisplayBackend::GetDesiredPage), snapshot it onto the context; result folds into the skip decision
3. CheckHashSkip        — Compare discretized hash; skip if unchanged (+ elemental / page-change override)
4. LogStateTransition   — Log diff of changed GameState fields
5. EnrichElementalDamage — Bridge HealthTrackingState elemental timers → PlayerActorState effect flags
6. ScoreCandidates      — CandidateGenerator.GenerateCandidates() → UtilityScorer.ScoreCandidates()
7. AllocateAndLock      — OverrideManager → SlotAllocator (for the snapshotted page) → SlotLocker → ComputeVisualStates
8. UpdateCaches         — PipelineStateCache + EquipManager slot contents (snapshotted page)
9. PushDisplay          — Push DisplayContext to all IDisplayBackend instances
```

> **Page consistency:** `ResolveDisplayPage` resolves and snapshots the active page (index/count/slot count/name onto `PipelineContext`) *before* the hash-skip and allocation. Allocation, caches, and push all read that snapshot, so a page switch arriving off-thread mid-tick (Wheeler callback, page-cycle keys, console) can't tear assignments from their page metadata — it just raises the page-dirty flag and re-runs next tick.

**PipelineContext** (`src/pipeline/PipelineCoordinator.h`):

The data bundle shared between steps. Contains:
- **Inputs:** `deltaMs`, `player`, `actorValue`, `now`
- **State snapshots:** `GameState`, `PlayerActorState`, `TargetCollection`, `WorldState`, `HealthTrackingState`, `MagickaTrackingState`, `StaminaTrackingState`, `currentMagicka`, `stateHash`, `elementalDamageActive`
- **Pipeline outputs:** `scoredCandidates`, `overrides`, `rawAssignments`, `assignments`

---

## Hybrid Scoring System (Three Tiers)

```mermaid
graph TD
    subgraph Tier1[Tier 1: Override Hard Rules]
        OR1[Health < 10% → Health potion]
        OR2[Drowning → Waterbreathing]
        OR3[On fire → Resist Fire]
        OR4[Weapon empty → Soul gem]
    end

    subgraph Tier2[Tier 2: Context Weights Soft Heuristics]
        CW1[In combat → Damage +2.0]
        CW2[Sneaking → Muffle +3.0]
        CW3[At forge → Fortify Smithing +8.0]
        CW4[Multiple enemies → AOE +2.0]
    end

    subgraph Tier3[Tier 3: Contextual Bandit Learning Player Preference]
        QL1[Fireball vs Ice Spike]
        QL2[Oakflesh usage patterns]
        QL3[Scroll preferences]
    end

    Tier1 -->|Bypasses scoring| Output[Slot Allocation]
    Tier2 -->|Gates relevance| Tier3
    Tier3 -->|Ranks within context| Output

    style Tier1 fill:#ffcccc
    style Tier2 fill:#ccffcc
    style Tier3 fill:#ccccff
```

**Tier Breakdown:**

| Tier | Purpose | Examples | Implementation |
|------|---------|----------|----------------|
| **Override** | Urgent situations with obvious answer | Critical HP, drowning, on fire | `OverrideManager` hard rules, bypass scoring |
| **Context Weight** | Situational relevance (rule-based) | Combat buffs, workstation potions | `ContextRuleEngine` + `CandidateGenerator` |
| **Contextual Bandit Learning** | Player-specific preference | Item choice within context | `FeatureQLearner` reward estimate lookup |

---

## Stage Descriptions

### Stage 1: Game State Polling (StateManager)

The StateManager contains 11 poll methods that act as **sensors**, reading directly from the game engine (SKSE API):

```mermaid
graph LR
    subgraph Polling[StateManager Update 100ms]
        P1[PollWorldObjects]
        P2[PollPlayerVitals]
        P3[PollPlayerMagicEffects]
        P4[PollPlayerEquipment]
        P5[PollPlayerSurvival]
        P6[PollPlayerPosition]
        P7[PollTargets]
        P8[PollHealthTracking]
        P9[PollPlayerResistances]
        P10[PollStaminaTracking]
        P11[PollMagickaTracking]
    end

    P1 --> WS[WorldState]
    P2 --> PAS1[PlayerActorState.vitals]
    P3 --> PAS2[PlayerActorState.effects + buffs]
    P4 --> PAS3[PlayerActorState.equipment]
    P5 --> PAS4[PlayerActorState.survival]
    P6 --> PAS5[PlayerActorState.position]
    P7 --> TC[TargetCollection]
    P8 --> HTS[HealthTrackingState]
    P9 --> PAS6[PlayerActorState.resistances]
    P10 --> STS[StaminaTrackingState]
    P11 --> MTS[MagickaTrackingState]

    style WS fill:#e1f5ff
    style TC fill:#e1f5ff
    style HTS fill:#e1f5ff
    style STS fill:#e1f5ff
    style MTS fill:#e1f5ff
```

**Poll Methods:**

| Poll Method | Reads From | Updates | Interval |
|-------------|-----------|---------|----------|
| `PollWorldObjects()` | `RE::CrosshairPickData`, `RE::Calendar` | `WorldState` | 100ms |
| `PollPlayerVitals()` | `RE::ActorValueOwner::GetActorValue()` | `PlayerActorState.vitals` | 100ms |
| `PollPlayerMagicEffects()` | `RE::Actor::GetActiveEffectList()` | `PlayerActorState.effects + .buffs` | 100ms |
| `PollPlayerEquipment()` | `RE::Actor::GetEquippedObject()` | `PlayerActorState.equipment` | 100ms |
| `PollPlayerSurvival()` | `RE::ActorValue` (survival stats) | `PlayerActorState.survival` | 1000ms |
| `PollPlayerPosition()` | `RE::Actor::IsSwimming()`, `IsSneaking()` | `PlayerActorState.position` | 100ms |
| `PollTargets()` | `RE::ProcessLists::GetSingleton()` | `TargetCollection` | 100ms |
| `PollHealthTracking()` | Health delta, `DamageEventSink` enrichment | `HealthTrackingState` | 100ms |
| `PollPlayerResistances()` | `RE::ActorValue` (resistances) | `PlayerActorState.resistances` | 500ms |
| `PollStaminaTracking()` | Stamina delta, usage source classification | `StaminaTrackingState` | 100ms |
| `PollMagickaTracking()` | Magicka delta, casting state classification | `MagickaTrackingState` | 100ms |

**State Types** (output):

| Struct | Purpose | Details |
|--------|---------|---------|
| `WorldState` | Crosshair targets, locks, workstations, time, light | See [1-states.md](1-states.md) |
| `PlayerActorState` | Vitals, effects, buffs, equipment, survival, position, resistances | See [1-states.md](1-states.md) |
| `TargetCollection` | Primary target + up to 50 tracked actors | See [1-states.md](1-states.md) |
| `HealthTrackingState` | Recent damage/healing events, rates, elemental damage timers | See [1-states.md](1-states.md) |
| `StaminaTrackingState` | Stamina usage events, rates, source classification | See [1-states.md](1-states.md) |
| `MagickaTrackingState` | Magicka usage events, rates, casting state | See [1-states.md](1-states.md) |

**Discretized State** (for contextual bandit learning):

| Struct | Purpose | Details |
|--------|---------|---------|
| `GameState` | Discretized buckets for the pipeline skip-check (36,288 states) | See [4-contextual-bandits.md](4-contextual-bandits.md) |

### Stage 2: Candidate Generator

Gathers all potential recommendations from four registries:

```mermaid
graph TB
    subgraph Sources[Item Registries]
        SR[SpellRegistry]
        IR[ItemRegistry<br/>potions, soul gems, ammo, food]
        WR[WeaponRegistry]
        SCR[ScrollRegistry]
    end

    SR -->|spells| Filter[Affordability Filter]
    IR -->|potions, soul gems,<br/>ammo| Filter
    WR -->|weapons| Filter
    SCR -->|scrolls| Filter

    Filter -->|candidates| Output[CandidateVariant list]

    style Filter fill:#fff4e1
```

**Registries** (`CandidateGenerator` member references):

| Registry | Class | Candidate Types |
|----------|-------|-----------------|
| `m_spellRegistry` | `Spell::SpellRegistry` | Known spells (filtered by magicka affordability) |
| `m_itemRegistry` | `Item::ItemRegistry` | Potions, soul gems, ammo, food |
| `m_weaponRegistry` | `Weapon::WeaponRegistry` | Favorited weapons (melee + ranged + staves) |
| `m_scrollRegistry` | `Scroll::ScrollRegistry` | Scrolls with relevant effects |

**Gathered Items:**

- **Spells** (from spell registry) - filtered by `PlayerActorState.vitals.magicka`
- **Potions** (from item registry) - based on `PlayerActorState.effects` and `vitals`
- **Scrolls** (from scroll registry) - alternative to spells
- **Weapons/Armor** (from weapon registry, favorites) - based on `PlayerActorState.equipment`
- **Soul gems** (from item registry) - if `PlayerActorState.equipment.weaponChargePercent < 0.25`
- **Staves** - treated as spells with charge
- **Arrows** (from item registry) - if `PlayerActorState.equipment.hasBowEquipped`

Items are classified by **effect type** (not school), which works for vanilla and modded content.

> `CandidateGenerator` computes neither relevance scores nor display explanations — it gathers, filters, and dedups. `ContextRuleEngine` owns both: its `ContextWeightMap` ranks candidates, and `DominantReason()` names the single `ContextReason` the pipeline hands to the display each tick for the Wheeler subtext label (critique #10).

### Stage 3: Elemental Damage Enrichment

Before scoring, `PipelineCoordinator::EnrichElementalDamage()` bridges instant-hit elemental detection into the player state:

```cpp
// If HealthTrackingState recorded fire/frost/shock within the enrichment window,
// set the corresponding PlayerActorState.effects flag so ContextRuleEngine sees it.
if (healthTracking.timeSinceLastFire < ELEMENTAL_DAMAGE_ENRICHMENT_WINDOW)
    playerState.effects.isOnFire = true;
if (healthTracking.timeSinceLastFrost < ELEMENTAL_DAMAGE_ENRICHMENT_WINDOW)
    playerState.effects.isFrozen = true;
if (healthTracking.timeSinceLastShock < ELEMENTAL_DAMAGE_ENRICHMENT_WINDOW)
    playerState.effects.isShocked = true;
```

This is necessary because instant-hit elemental damage (e.g., a fireball impact) doesn't create a persistent magic effect that `PollPlayerMagicEffects` would detect. The `DamageEventSink` records the elemental type at impact time, and enrichment carries it forward into the scoring pipeline.

### Stage 4: Override Check (OverrideManager)

Urgent conditions bypass normal scoring. Implemented by `OverrideManager::EvaluateOverrides()`.

```mermaid
graph TD
    Check{Override<br/>Condition?}
    Check -->|HP < 10%| HP[Priority 100:<br/>Health Potion]
    Check -->|MP < 10%| MP[Priority 70:<br/>Magicka Potion]
    Check -->|SP < 10%| SP[Priority 60:<br/>Stamina Potion]
    Check -->|Drowning| DW[Priority 50:<br/>Waterbreathing]
    Check -->|Ammo < 10| AM[Priority 40:<br/>Best Ammo]
    Check -->|Charge 0%| WC[Priority 35:<br/>Soul Gem]
    Check -->|None| Normal[Continue to<br/>Normal Scoring]

    HP --> Slot[Force Update Slot]
    MP --> Slot
    SP --> Slot
    DW --> Slot
    AM --> Slot
    WC --> Slot

    style HP fill:#ffcccc
    style MP fill:#ccccff
    style SP fill:#ccffcc
    style DW fill:#cce5ff
    style AM fill:#ffe5cc
    style WC fill:#ffccff
```

**Override Rules** (see [5-slots.md](5-slots.md) for full details):

| Priority | Condition | Threshold | Hysteresis | Action | INI Toggle |
|----------|-----------|-----------|------------|--------|------------|
| 100 | CRITICAL_HEALTH | HP < 10% | 15% | Best health potion | `bEnableCriticalHealth` |
| 70 | CRITICAL_MAGICKA | MP < 10% | 15% | Best magicka potion | `bEnableCriticalMagicka` |
| 60 | CRITICAL_STAMINA | SP < 10% | 15% | Best stamina potion | `bEnableCriticalStamina` |
| 50 | DROWNING | Underwater, no buff | N/A | Waterbreathing potion | `bEnableDrowning` |
| 40 | LOW_AMMO | < 10 arrows/bolts | 15 | Best ammo | `bEnableLowAmmo` |
| 35 | WEAPON_EMPTY | Charge = 0% | 5% | Best filled soul gem | `bEnableWeaponCharge` |

> **Note:** Compile-time code defaults are 10% as shown. The shipped `configs/Huginn.ini` sets all vital thresholds to **35%** for a more proactive experience. The INI value is authoritative at runtime; the table reflects the code fallback only.

All potion overrides use the `bAllowImpurePotions` INI setting to control whether potions with harmful side effects are used as a last resort.

**Pipeline position:** Overrides are evaluated in `AllocateAndLock` as a **peer step** to slot allocation — the `OverrideCollection` is passed as a parameter to `SlotAllocator::AllocateSlots()`, not as a gate before it.

### Stage 5: Context Evaluation (ContextRuleEngine)

ContextRuleEngine is the sole source of context weights. It evaluates game state once per scoring pass and produces a `ContextWeightMap` consumed by `UtilityScorer`.

```mermaid
graph TB
    subgraph Input[Context Inputs]
        GS[GameState discretized]
        PAS[PlayerActorState continuous]
        TC[TargetCollection]
        WS[WorldState]
    end

    GS --> Rules[Context Rules]
    PAS --> Rules
    TC --> Rules
    WS --> Rules

    Rules --> Map[ContextWeightMap]

    subgraph Map[ContextWeightMap]
        R1[healingWeight: 0.5]
        R2[resistFireWeight: 0.8]
        R3[fortifySmithingWeight: 0.8]
        R4[damageWeight: 0.3]
    end

    Map --> Scorer[UtilityScorer]

    style Rules fill:#e1ffe1
```

**Example Rules:**

| Condition | Category | Weight | Rationale |
|-----------|----------|--------|-----------|
| health < 20% | HealingAny | 1.0 | Critical — player about to die |
| health < 50% | HealingAny | up to 0.5 (quadratic) | Scales with urgency |
| taking fire damage | ResistFire | 0.8 | Active threat |
| underwater, no waterbreathing | Waterbreathing | 1.0 | Will drown |
| at forge workstation | FortifySmithing | 0.8 | Obvious context |
| enemies nearby, sneaking | Invisibility | 0.5 | Stealth utility |

Context weights are normalized to [0,1] and configured via the `[ContextWeights]` INI section.

### Stage 6: Utility Scoring (UtilityScorer)

The scorer combines context weights, learning, and other factors into final utility scores using the multiplicative formula.

```mermaid
graph TB
    subgraph Inputs[Scoring Inputs]
        CW[contextWeight]
        Q[reward estimate]
        Prior[prior heuristic]
        UCB[exploration bonus]
        Rec[recency boost]
        Corr[correlation bonus]
        Potion[potion multiplier]
        Fav[favorites multiplier]
    end

    Q --> Learn[learningScore =<br/>α*Q + 1-α*prior + β*UCB<br/>+ recencyBoost]
    Prior --> Learn
    UCB --> Learn
    Rec --> Learn

    CW --> Formula[utility =<br/>ctx × 1 + λ*learn<br/>× corr × potion × fav]
    Learn --> Formula
    Corr --> Formula
    Potion --> Formula
    Fav --> Formula

    Formula --> Output[Final Utility]

    style Formula fill:#e1ffe1
```

**Scoring Formula:**

```cpp
// Step 1-4: Compute learning score
learningScore = α * Q + (1 - α) * prior + β * UCB + recencyBoost

// Step 5-7: Gather multipliers
correlationBonus  = CorrelationBooster  (equipment synergies, always multiplicative)
potionMultiplier  = PotionDiscriminator (combat timing, value discrimination)
favoritesMultiplier = Favorites system  (boost/off/suppress)

// Step 8: Final utility (multiplicative formula)
λ = lambdaMin + confidence × (lambdaMax - lambdaMin)
utility = contextWeight × (1 + λ × learningScore)
          × correlationBonus × potionMultiplier × favoritesMultiplier
```

Where:
- `contextWeight` = From ContextRuleEngine [0,1] — zero context = zero utility
- `learningScore` = `α×Q + (1-α)×prior + β×UCB + recencyBoost`
- `λ` = Confidence-adaptive: `lambdaMin + confidence × (lambdaMax - lambdaMin)` (0.5 at cold start → 3.0 at full confidence)
- `Q` = Learned preference from FeatureQLearner (18-float linear function approximation)
- `α` = Confidence (more training → trust Q more)
- `prior` = Intrinsic quality heuristic from PriorCalculator (magnitude, cost, charge)
- `UCB` = Exploration bonus for untried items
- `recencyBoost` = From UsageMemory — additive boost for items used repeatedly in the same context

**UsageMemory (Event-Driven Short-Term Recall):**

`UsageMemory` (`src/learning/UsageMemory.h`) tracks recent item usage in a ring buffer. When the same item is used 3+ times in the same discretized game context, it receives an additive `recencyBoost = 1.5` to its learning score. This creates short-term situational recall — if the player keeps using Fireball in this context, boost Fireball.

| Parameter | Value | Purpose |
|-----------|-------|---------|
| `BUFFER_CAPACITY` | 20 events | Ring buffer size (self-pruning) |
| `MATCH_THRESHOLD` | 3 uses | Minimum matching events to trigger boost |
| `RECENCY_BOOST` | 1.5 | Additive to `learningScore` inside `(1 + λ × learn)` |

UsageMemory also detects **misclicks**: if the player switches to a different item within `MISCLICK_WINDOW_SECONDS` in the same context, the previous equip is flagged as unintentional. Thread-safe via internal mutex (multiple writers: Wheeler, equip callback, ExternalEquipLearner; one reader: update thread).

**Cold-Start Fallback:**

When too few candidates pass the minimum utility threshold (e.g., new character with no Q-data), the scorer runs a fallback pass with UCB-boosted context weights:

```
effectiveContext = max(contextWeight, coldStartUCBBoost × UCB)
```

Untried items get UCB ≈ 1.0, which decays with visits — the fallback is self-healing. This prevents the "Top 0 Candidates" empty-slot scenario where `baseRelevanceWeight = 0.05` is too low to pass `fMinimumUtility = 0.1`. Configured via `fColdStartUCBBoost = 0.2` in `[Scoring]` INI section (0.0 disables).

### Stage 7: Slot Allocation (SlotAllocator)

```mermaid
graph TB
    Scored[Scored Candidates] --> OvM[OverrideManager<br/>EvaluateOverrides]
    OvM -->|overrides| SA[SlotAllocator<br/>AllocateSlots]
    Scored --> SA
    SA --> Lock[SlotLocker<br/>ApplyLocks]
    Lock --> VS[ComputeVisualStates]
    VS --> Output[Stable Slot<br/>Assignments]

    subgraph Slots[Slot Classifications]
        S1[DamageAny]
        S2[HealingAny]
        S3[BuffsAny]
        S4[DefensiveAny]
        S5[SummonsAny]
        S6[Utility]
        S7[PotionsAny]
        S8[WeaponsAny]
        S9[Regular]
    end

    SA -.->|matches| Slots

    style OvM fill:#ffcccc
    style SA fill:#ffe1f5
    style Lock fill:#fff4e1
    style VS fill:#e1f5ff
```

**Slot Classifications** (from `SlotClassification` enum in `src/slot/SlotConfig.h`):

- **Effect-based:** `DamageAny`, `HealingAny`, `BuffsAny`, `DefensiveAny`, `SummonsAny`, `Utility`
- **Item-type based:** `PotionsAny`, `ScrollsAny`, `SpellsAny`, `SpellsDestruction`, `SpellsRestoration`, `SpellsConjuration`, `SpellsIllusion`, `SpellsAlteration`, `WeaponsAny`, `WeaponsMelee`, `WeaponsRanged`, `FoodAny`, `AmmoAny`
- **Unrestricted:** `Regular` (accepts any candidate)

**Duplicate Removal:**

Remove items already allocated to higher-priority slots. **No penalty** for filtered duplicates (the item wasn't rejected by the player).

**Slot Allocation:**

Pick top-scoring candidate per slot, respecting:
- Slot classification filter
- Override priority (overrides passed as parameter to `AllocateSlots`)
- Deduplication
- Lock status (can be broken by high-priority overrides)

**SlotLocker Temporal Stability:**

Prevents UI flicker from brief state oscillations by holding slot content for configurable duration (default 3000ms). Overrides with priority >= 50 can break locks immediately.

**ComputeVisualStates:**

After locking, each slot is assigned a `SlotVisualState` that drives UI animations:

| Visual State | Meaning | Trigger |
|--------------|---------|---------|
| `Normal` | Default, no effect | Slot unchanged or empty |
| `Confirmed` | Re-evaluated, same item | Lock expired but new scoring picked the same item |
| `Expiring` | Lock about to expire, content will change | Last 40% of lock duration AND raw assignment differs |
| `Override` | Override triggered | Slot has `AssignmentType::Override` |
| `Wildcard` | Exploration pick | Slot has `AssignmentType::Wildcard` |

---

## Display Backends

The display layer uses the `IDisplayBackend` interface (`src/display/IDisplayBackend.h`). Each backend receives a `DisplayContext` struct containing slot assignments, scored candidates, overrides, player/world state, the resolved page state (index, count, slot count, name), and timing. The page fields are the tick snapshot from `ResolveDisplayPage`, so backends read them off the context instead of re-fetching from the `SlotAllocator`/`SlotSettings` singletons.

A backend also implements `GetDesiredPage()` — returns the page it wants shown (or `-1` for no opinion). The coordinator polls this before allocation to resolve the active page. Wheeler-specific concerns (urgent-override auto-focus) live in `WheelerBackend`, not the coordinator.

**Registered backends** (in `PipelineCoordinator.cpp`):

| Backend | Class | Purpose |
|---------|-------|---------|
| **IntuitionBackend** | `Display::IntuitionBackend` | Scaleform HUD widget (primary player-facing) |
| **WheelerBackend** | `Display::WheelerBackend` | Wheeler mod radial menu integration |

**Debug-only displays** (not `IDisplayBackend`):
- `UtilityScorerDebugWidget` — ImGui overlay with per-slot scoring breakdown (`_DEBUG` builds only)
- `StateManagerDebugWidget` — ImGui state display (`_DEBUG` builds only)

Adding a new display target: implement `IDisplayBackend` (`Push`, `IsEnabled`, and optionally `GetDesiredPage` if it drives page selection), add an instance to the `s_displayBackends[]` array in `PipelineCoordinator.cpp`.

---

## Feedback Loop

> **Design Principle (v0.13.0+):** Learning is decoupled from the presentation layer (Wheeler/Widget). The system learns from all equip events — both Huginn-mediated (Wheeler, hotkeys) and external (vanilla menu, favorites). Negative signals come from L2 regularization and time-based decay, not from Wheeler open/close events or skip penalties. See [../refactor/roadmap.md](../roadmap.md) for rationale.

```mermaid
graph LR
    Player[Player Action] --> Event{Equip Event}
    Event -->|From Wheeler| E1[+8.0 reward]
    Event -->|From hotkey 1-0| E2[+8.0 reward]
    Event -->|From consumption| E2b[+5.0 reward]
    Event -->|From vanilla menu| E3[Tiered reward<br/>0.0-8.0]

    E1 --> QL[FeatureQLearner<br/>Update weights]
    E2 --> QL
    E2b --> QL

    E3 --> EEL[ExternalEquipLearner<br/>Pipeline Attribution]
    EEL -->|Case A: not candidate| R_A[x1.0 = +8.0 — strongest]
    EEL -->|Case B-low: scored low| R0[x0.2 = +1.6 — noise]
    EEL -->|Case B-med: mid-rank| R1[x0.4 = +3.2 — undervalued]
    EEL -->|Case C: scored high, not shown| R2[x0.8 = +6.4 — near-miss]
    EEL -->|Case D: shown other page| R3[x0.5 = +4.0 — page issue]
    EEL -->|Case E: shown current page| Skip2[0.0 — player saw it]
    R_A --> QL
    R0 --> QL
    R1 --> QL
    R2 --> QL
    R3 --> QL

    E1 --> UM[UsageMemory<br/>Ring Buffer]
    E2 --> UM
    E3 --> UM

    Decay[Weight Decay] --> QL
    Decay -->|L2 regularization| D1[Per-update weight shrinkage]
    Decay -->|MaybeDecay| D2[2%/hr on idle items > 5 min]

    Misclick[Rapid re-equip < 3s] -->|UsageMemory| Penalty[-3.0 penalty]
    Penalty --> QL

    QL --> Future[Future<br/>Recommendations]

    style E1 fill:#ccffcc
    style E2 fill:#ccffcc
    style E2b fill:#ccffcc
    style E3 fill:#ccffee
    style EEL fill:#ffe1e1
    style UM fill:#fff4e1
    style D1 fill:#fff4e1
    style D2 fill:#f0f0f0
    style Penalty fill:#ffcccc
```

**Learning Signals:**

| Signal | Source | Effect | Purpose |
|--------|--------|--------|---------|
| Equip reward | Wheeler selection / hotkey 1-0 | +8.0 (`EQUIP_REWARD`) to equipped item | Positive reinforcement (Huginn-mediated) |
| Consume reward | Potion/scroll consumption | +5.0 (`CONSUME_REWARD`) to consumed item | Separate tuning for finite resources |
| External equip reward | Vanilla menu / favorites | 0.0 to +8.0 (tiered by pipeline attribution) | Learn from non-Huginn equips |
| Misclick penalty | Rapid equip-then-switch (< 3s) | -3.0 (`MISCLICK_PENALTY`) to discarded item | Penalize accidental equips |
| Recency boost | UsageMemory ring buffer | +1.5 after 3+ uses in same context | Short-term situational recall |
| L2 regularization | Per gradient descent step | Weight shrinkage (λ=0.01) | Continuous dampening, prevents unbounded drift |
| Time-based decay | `MaybeDecay()` per candidate read | 2%/hr exponential decay (idle > 5 min) | Stale entries erode over time |

**External Equip Attribution Cases** (`ExternalEquipLearner`, Phase 3b):

| Case | Condition | Reward Multiplier | Rationale |
|------|-----------|-------------------|-----------|
| A | Not in candidate pool | x1.0 = +8.0 (strongest) | Player went out of their way — strongest preference signal |
| B-low | Low-rank (beyond `FAR_MISS_SLOTS` overshoot) | x0.20 = +1.6 | Scored low, possible noise |
| B-med | Mid-rank (between `NEAR_MISS_SLOTS` and `FAR_MISS_SLOTS`) | x0.40 = +3.2 | Scoring undervalued |
| C | High-rank (within `NEAR_MISS_SLOTS`), not displayed | x0.80 = +6.4 | Near-miss — scoring correct, slot allocation missed |
| D | Displayed on different page | x0.50 = +4.0 | Multi-page UX issue |
| E | Displayed on current page | 0.0 (skip) | Player saw it, chose vanilla UI anyway |

**Anti-spam filters:** 3s minimum between same-item equips, low-stakes filter (skip if not in combat with full health), re-equip filter, recent-Wheeler filter (skip if Wheeler open in last 2s).

**Removed signals (v0.13.0):**

| Former Signal | Reason Removed |
|---------------|----------------|
| Skip penalty (-1.0 on wheel close) | Punished correct recommendations during state transitions. Learning should not be coupled to presentation layer. |
| Cast bonus (+3.0) | Never implemented (CAST_BONUS defined but unused). |

See [4-contextual-bandits.md](4-contextual-bandits.md) for learning update details and [../refactor/roadmap.md](../roadmap.md) for design rationale.

---

## Pipeline Summary

```mermaid
graph TB
    Start[Game State] --> SM[StateManager<br/>11 poll methods]
    SM --> Models[6 State Types]
    Models --> PC[PipelineCoordinator]

    PC -->|1. GatherState| SE[StateEvaluator<br/>Discretize]
    SE --> GS[GameState Hash]

    GS -->|2. ResolveDisplayPage| RDP[Snapshot active page<br/>onto context]

    RDP -->|3. CheckHashSkip| Skip{Hash or<br/>page changed?}
    Skip -->|No| Done[Pipeline Skipped]
    Skip -->|Yes| Continue[Continue]

    Continue -->|4. LogStateTransition| Log[Log Diff]
    Log -->|5. EnrichElementalDamage| Enrich[Bridge elemental<br/>timers → effect flags]

    Enrich -->|6. ScoreCandidates| CG[CandidateGenerator]
    CG --> US[UtilityScorer]
    GS --> CRE[ContextRuleEngine]
    CRE --> US
    GS --> QL[FeatureQLearner]
    QL --> US

    US -->|7. AllocateAndLock| OvM[OverrideManager]
    OvM --> SA[SlotAllocator<br/>snapshotted page]
    SA --> SL[SlotLocker]
    SL --> VS[ComputeVisualStates]

    VS -->|8. UpdateCaches| PSC[PipelineStateCache]
    VS -->|9. PushDisplay| Display[IDisplayBackend]
    Display --> IB[IntuitionBackend]
    Display --> WB[WheelerBackend]

    style SM fill:#e1f5ff
    style PC fill:#f0e1ff
    style CG fill:#fff4e1
    style CRE fill:#e1ffe1
    style US fill:#ffe1f5
    style SA fill:#cce5ff
    style Display fill:#e1f5ff
```

**Processing Stages:**

| Stage | Step | Input | Output | Notes |
|-------|------|-------|--------|-------|
| 1 | `GatherState` | Game world | 6 state types + discretized `GameState` | 11 poll methods at 100ms-1000ms intervals |
| 2 | `ResolveDisplayPage` | Backend `GetDesiredPage()` | Active page synced + snapshotted onto context | Page change folds into the skip decision; snapshot keeps the whole tick on one page |
| 3 | `CheckHashSkip` | `GameState` hash, page-change flag | Skip/continue decision | Elemental damage / page change override hash skip |
| 4 | `LogStateTransition` | Previous + current `GameState` | Log output | Only logs when hash changes |
| 5 | `EnrichElementalDamage` | `HealthTrackingState` timers | `PlayerActorState.effects` flags | Bridges instant-hit detection to context rules |
| 6 | `ScoreCandidates` | State types, registries | Scored candidates | `CandidateGenerator` → `UtilityScorer` |
| 7 | `AllocateAndLock` | Scored candidates, state, snapshotted page | Stable slot assignments | `OverrideManager` → `SlotAllocator` (snapshotted page) → `SlotLocker` → `ComputeVisualStates` |
| 8 | `UpdateCaches` | Scored candidates, assignments | Cached pipeline snapshot | `PipelineStateCache` + `EquipManager` slot contents (snapshotted page) |
| 9 | `PushDisplay` | Assignments, overrides, page snapshot | UI updates | `IntuitionBackend` (Scaleform HUD) + `WheelerBackend` (radial menu) |

---

## See Also

- [../ARCHITECTURE.md](../ARCHITECTURE.md) - Overall system design
- [4-contextual-bandits.md](4-contextual-bandits.md) - Learning system (FeatureQLearner architecture)
- [5-slots.md](5-slots.md) - Slot classification and overrides
- [1-states.md](1-states.md) - State model architecture
