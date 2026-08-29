# Candidate Filtering & Scoring Rules

> **Purpose:** Reference for hard filter rules, intrinsic priors, and filter configuration.
> Updated for v0.12.x (CandidateGenerator, UtilityScorer, OverrideManager).

> **See also:**
> - [0-pipeline.md](0-pipeline.md) - Overall pipeline flow and three-tier scoring system
> - [5-slots.md](5-slots.md) - Slot classification, overrides, and allocation
> - [4-contextual-bandits.md](4-contextual-bandits.md) - contextual bandit learning and utility scoring formula

---

## 1. Hard Filters (Binary Gates)

These filters **remove candidates** from consideration entirely. A candidate must pass ALL filters to proceed to scoring. Filters are applied in a single forward pass in `CandidateFilters::ApplyAllFilters()`.

### 1.1 Implemented Filters

All filters are implemented in `CandidateFilters.cpp` and applied in order:

| # | Filter | Condition | Effect | Configurable? |
|---|--------|-----------|--------|---------------|
| 1 | **Affordability** | Spells: `effectiveCost > currentMagicka` (policy-based); Items/Scrolls/Ammo: `count <= 0`; Weapons: always pass | Policy-based or Skip | ✅ INI `[Candidates]` |
| 2 | **Equipped** | Spells: `player.IsSpellEquipped(formID)`; Ammo: `isEquipped`; Weapons: **always pass** (equipped-skip is per-slot via `bSkipEquipped` in `SlotAllocator`) | Skip | ❌ No |
| 3 | **Cooldown** | `IsOnCooldown(formID, sourceType)` via CooldownManager | Skip | ✅ Per-type in `CandidateConfig` |
| 4 | **Full Vitals** | HP=100% + healing item, MP=100% + magicka item, SP=100% + stamina item | Skip | ❌ No (threshold: 1% epsilon) |
| 5 | **Active Buff** | Resist potion when already resisting, invisibility when invisible, muffle, armor, active summon | Skip | ✅ Thresholds in `CandidateConfig` |

> **Note:** Filter #6 (Relevance) was **removed in Stage 1g**. The `baseRelevance` field no longer exists on candidates. Context-based filtering now happens in `UtilityScorer` via `fMinimumContextWeight` (0.05) and `fMinimumUtility` (0.1) in the `[Scoring]` INI section.

After filtering, deduplication removes duplicate candidates (inline in the same forward pass), and the list is truncated to `maxCandidatesAfterFilter` (default: 500) in gather order (spells first, then potions, then scrolls, then weapons, then ammo). No sorting is performed — real prioritization happens later in `UtilityScorer`.

### 1.2 Cooldown Defaults

Configured in `CandidateConfig.h`:

| Item Type | Default Cooldown | Notes |
|-----------|------------------|-------|
| Spells | 2.0s | Prevent spam-recommending same spell |
| Potions | 3.0s | Account for effect duration |
| Scrolls | 2.0s | Consumable, less penalty |
| Weapons | 5.0s | Explicit equip |
| Soul Gems | 3.0s | One recharge action at a time |
| Ammo | 1.0s | Brief cooldown |
| Food | 3.0s | Similar to potions |
| Staves | 2.0s | Uses `spellCooldown` (Staff SourceType) |

### 1.3 Uncastable Spell Policy

Controls what happens to spells the player can't currently afford (insufficient magicka). Configured via INI `[Candidates]` section.

**Three modes:**

| Mode | INI Value | Behavior |
|------|-----------|----------|
| **Disallow** | `Disallow` | Filter out uncastable spells entirely (default, original behavior) |
| **Penalize** | `Penalize` | Keep uncastable spells but compute magicka shortfall ratio (see note) |
| **Allow** | `Allow` | Keep uncastable spells at full relevance (no penalty applied) |

**Penalty formula (Penalize mode):**

```
penalty = clamp(currentMagicka / effectiveCost, penaltyFloor, 1.0)
```

- `effectiveCost` = `spell->CalculateMagickaCost(player)` (includes perk/enchant reductions)
- For concentration spells, `CalculateMagickaCost` returns per-second cost, so the ratio represents "what fraction of one second you can sustain"
- `penaltyFloor` prevents completely zeroing out high-cost spells (default: 0.05 = 5% minimum relevance)

> **Latent issue (Stage 1g):** The penalty is computed but the `baseRelevance *= penalty` application is commented out because the `baseRelevance` field was removed. The TODO in code suggests integrating this into `ContextRuleEngine` or `PriorCalculator` in a future version.

**INI settings:**

| Setting | Default | Description |
|---------|---------|-------------|
| `sUncastableSpellPolicy` | `Disallow` | One of: `Disallow`, `Penalize`, `Allow` |
| `fUncastablePenaltyFloor` | `0.05` | Minimum multiplier in Penalize mode (0.0 - 1.0) |

**Example scenarios (Penalize mode):**

| Spell | Cost | Current MP | Ratio | Relevance Multiplier |
|-------|------|------------|-------|---------------------|
| Fireball (one-shot) | 200mp | 50mp | 0.25 | 0.25x |
| Flames (concentration) | 14mp/s | 7mp | 0.50 | 0.50x |
| Master spell | 500mp | 10mp | 0.02 | 0.05x (clamped to floor) |
| Healing (affordable) | 30mp | 100mp | 3.33 | 1.0x (can afford, no penalty) |

---

## 2. Hard Overrides

Overrides bypass normal scoring entirely, forcing specific items into slots for urgent conditions. For the full override table (priorities, thresholds, hysteresis, potion selection), see:

- [0-pipeline.md](0-pipeline.md) — Stage 3: Override Check
- [5-slots.md](5-slots.md) — Override System (detailed evaluation flow)
- `src/override/OverrideManager.cpp` — Implementation
- `[Overrides]` section in `Huginn.ini` — All thresholds are INI-configurable

---

## 3. Intrinsic Priors (PriorCalculator)

`PriorCalculator` provides **intrinsic quality heuristics** that bootstrap contextual bandit learning before sufficient data is collected. These priors evaluate ONLY intrinsic item properties — they are **NOT context-aware**.

> **Key architectural separation (Stage 1g):** All context-dependent weighting (HP-based healing bonuses, combat damage bonuses, target-specific bonuses, stealth bonuses, etc.) has moved to `ContextRuleEngine`. `PriorCalculator` now only evaluates intrinsic item quality.

**Source:** `src/learning/PriorCalculator.cpp`

**Base Prior:** 0.3 for all candidates (clamped to [0.0, 1.0])

### 3.1 Spell Priors

| Factor | Formula | Max Effect | Rationale |
|--------|---------|------------|-----------|
| Spell cost | `costNormalized × COST_SCALE_FACTOR` | +0.1 | Higher cost = more powerful spell tier (Expert > Novice) |

```
prior = BASE_PRIOR + min(baseCost / 200.0, 1.0) × 0.1
```

Example: Expert spell (cost 200) gets prior 0.4; Novice spell (cost 20) gets prior 0.31.

### 3.2 Item (Potion) Priors

| Factor | Formula | Max Effect | Rationale |
|--------|---------|------------|-----------|
| Magnitude | `log(1 + mag) / log(1 + 100) × MAGNITUDE_SCALE_FACTOR` | +0.15 | Bigger potion = better quality (diminishing returns) |
| Scarcity | `-(1 - count/5) × COUNT_PENALTY_SCALE` | -0.1 | Avoid depleting last few items |

```
prior = BASE_PRIOR + magnitudeBonus - scarcityPenalty
```

Example: 200 HP potion (50 in stock) gets ~0.45; 50 HP potion (2 left) gets ~0.29.

### 3.3 Weapon Priors

| Factor | Formula | Max Effect | Rationale |
|--------|---------|------------|-----------|
| Low charge | `-(1 - chargePercent) × CHARGE_PENALTY_SCALE` | -0.2 | Depleted enchantment = lower quality weapon |

Only applies when `hasEnchantment && chargePercent < 20%`.

### 3.4 Ammo Priors

| Factor | Formula | Max Effect | Rationale |
|--------|---------|------------|-----------|
| Type match | `AMMO_TYPE_MATCH_BONUS` | +0.15 | Arrows for bow, bolts for crossbow (compatibility) |
| Scarcity | `(1 - count/20) × AMMO_SCARCITY_SCALE` | +0.1 | Surface low-count ammo for awareness |

### 3.5 Scroll Priors

Returns `BASE_PRIOR` (0.3) unconditionally. Scrolls have no intrinsic quality differences — their value is entirely contextual (handled by `ContextRuleEngine`).

### 3.6 Prior Constants

**Source:** `src/learning/PriorCalculator.h`

```cpp
static constexpr float BASE_PRIOR = 0.3f;

// Magnitude scaling (logarithmic)
static constexpr float MAGNITUDE_REFERENCE_VALUE = 100.0f;  // Major healing potion
static constexpr float MAGNITUDE_SCALE_FACTOR = 0.15f;      // Max bonus from magnitude

// Spell cost scaling (linear)
static constexpr float MAX_REASONABLE_SPELL_COST = 200.0f;  // Expert-level spells
static constexpr float COST_SCALE_FACTOR = 0.1f;            // Max bonus from high cost

// Inventory scarcity penalty
static constexpr float LOW_COUNT_THRESHOLD = 5.0f;          // Below this, reduce priority
static constexpr float COUNT_PENALTY_SCALE = 0.1f;          // Max penalty for nearly depleted

// Weapon charge penalty
static constexpr float CHARGE_PENALTY_SCALE = 0.2f;         // Max penalty for depleted charge

// Ammo
static constexpr float AMMO_LOW_COUNT_THRESHOLD = 20.0f;    // Below this, boost priority
static constexpr float AMMO_SCARCITY_SCALE = 0.1f;          // Max bonus for nearly depleted
static constexpr float AMMO_TYPE_MATCH_BONUS = 0.15f;       // Arrow/bolt matches weapon
```

### 3.7 Context Weights (ContextRuleEngine)

All context-dependent weighting is handled by `ContextRuleEngine` (`src/context/ContextRuleEngine.cpp`), NOT by `PriorCalculator`. Context weights include:

- HP-based healing bonuses
- Combat/damage bonuses, AOE bonuses for 3+ enemies
- Target-specific bonuses (anti-undead, anti-daedra)
- Stealth/invisibility bonuses
- Distance-based range bonuses
- Elemental resist bonuses when taking fire/frost/shock damage
- Survival mode food/warmth bonuses
- Weapon combat bonuses, silver vs undead
- Ammo bonuses for bow/crossbow equipped
- Workstation context (forge → Fortify Smithing, enchanter → Fortify Enchanting, etc.)

See [4-contextual-bandits.md](4-contextual-bandits.md) and `[ContextWeights]` INI section for the full weight table.

### 3.8 Workstation Context Weights (v0.12.x)

Workstation detection (`WorldState.isLookingAtWorkstation`, `workstationType`) is wired to relevance scoring via `RelevanceTag` context tags. The actual scoring weights are applied by `ContextRuleEngine`, not `CandidateGenerator` (which only sets the `RelevanceTag` bitmask flags).

| Condition | Item Match | Weight | Source |
|-----------|-----------|--------|--------|
| At forge (type 1-2) | `FortifyCombatSkill` + `Smithing` | `8.0 × workstationBoost` | `ContextRuleEngine` |
| At enchanter (type 3-4) | `FortifyMagicSchool` + `Enchanting` | `8.0 × workstationBoost` | `ContextRuleEngine` |
| At alchemy lab (type 5-6) | `FortifyUtilitySkill` + `Alchemy` | `8.0 × workstationBoost` | `ContextRuleEngine` |

---

## 4. Relevance Filters (Context Gates)

Relevance filtering uses a **two-layer approach**:

### 4.1 Hard Gate: Full Vitals Filter

`PassesFullVitalsFilter` removes items that are clearly irrelevant:
- Health potions/healing spells when HP >= 99% (epsilon = 1%)
- Magicka potions/restore magicka when MP >= 99%
- Stamina potions when SP >= 99%

### 4.2 Hard Gate: Active Buff Filter

`PassesActiveBuffFilter` removes items whose effects are already active:
- Resist potions when player already has sufficient resistance (configurable threshold)
- Invisibility items when player is already invisible
- Muffle spells when player already has muffle
- Armor spells when already buffed
- Active summons

### 4.3 Soft Gate: Utility Thresholds (UtilityScorer)

After hard filtering, `UtilityScorer` applies two soft thresholds from the `[Scoring]` INI section:

| Setting | Default | Description |
|---------|---------|-------------|
| `fMinimumContextWeight` | `0.05` | Items below this context weight are skipped early (no scoring) |
| `fMinimumUtility` | `0.1` | Items below this final utility are filtered from results |

These replace the old `baseRelevance < minRelevanceToInclude` filter that was removed in Stage 1g.

### 4.4 Filter Configuration

Configured in `CandidateConfig.h`:

```cpp
// Active buff filtering
float resistThresholdToFilter = 50.0f;    // Percentage (not ratio) above which resist items are filtered
bool filterRedundantResists = true;
bool filterActiveBuffs = true;
bool filterHealingWhenFull = true;
bool filterMagickaWhenFull = true;
bool filterStaminaWhenFull = true;

// Candidate limits
uint32_t maxCandidatesAfterFilter = 500;   // Truncate after this many
```

> **Note:** `CandidateConfig` settings are currently compile-time defaults only. Only `sUncastableSpellPolicy` and `fUncastablePenaltyFloor` are loaded from INI. A `LoadFromINI` method is stubbed out for future implementation.

---

## 5. Scoring Formula

After filtering, surviving candidates are scored by `UtilityScorer` (`src/learning/UtilityScorer.cpp`). For the full formula, components, and parameters, see:

- [0-pipeline.md](0-pipeline.md) — Stage 5: Utility Scoring (formula breakdown)
- [4-contextual-bandits.md](4-contextual-bandits.md) — Utility Calculation (reward estimates, confidence, UCB)

The intrinsic priors documented in Section 3 above feed into the scoring formula as the heuristic component. Context weights from `ContextRuleEngine` provide the situational relevance multiplier.

---

## 6. Implementation Status

All hard filters (Section 1) are implemented. Intrinsic priors (Section 3) are implemented. Context-dependent weighting is fully handled by `ContextRuleEngine`. Override status is tracked in [5-slots.md](5-slots.md) and [0-pipeline.md](0-pipeline.md).

### Elemental Damage Enrichment (v0.12.x)

Instant-hit elemental damage (fire bolts, ice spikes) is now detected via `HealthTrackingState` timestamps enriched into `PlayerActorState.effects` flags in `Main.cpp`. This supplements `PollPlayerMagicEffects()` which only catches ongoing DoT effects.

| Damage Type | Enrichment Window | Effect Flag | Source |
|-------------|-------------------|-------------|--------|
| Fire | 5.0s (`ELEMENTAL_DAMAGE_ENRICHMENT_WINDOW`) | `isOnFire` | `timeSinceLastFire` via `DamageEventSink` |
| Frost | 5.0s | `isFrozen` | `timeSinceLastFrost` via `DamageEventSink` |
| Shock | 5.0s | `isShocked` | `timeSinceLastShock` via `DamageEventSink` |

Sub-threshold hits (damage reduced below `HEALTH_DAMAGE_THRESHOLD` by resists) are recorded with zero magnitude so per-type timestamps still update.

---

## 7. Configuration

### 7.1 INI Configuration (Implemented)

All key settings are exposed via `Data/SKSE/Plugins/Huginn.ini`:

**`[Candidates]` section:**
```ini
sUncastableSpellPolicy = Disallow    ; Disallow / Penalize / Allow
fUncastablePenaltyFloor = 0.05       ; Min multiplier in Penalize mode
```

**`[Scoring]` section (relevance thresholds):**
```ini
fMinimumUtility = 0.1                ; Items below this utility are filtered from results
fMinimumContextWeight = 0.05         ; Items below this context weight are skipped early
```

**`[Overrides]` section** (all thresholds, hysteresis bands, enable toggles):
```ini
fCriticalHealthThreshold = 0.35      ; HP% that triggers health override (INI value)
fCriticalHealthHysteresis = 0.15     ; HP% above threshold to deactivate
bEnableCriticalHealth = true         ; Toggle
; ... (same pattern for magicka, stamina, weapon charge, ammo, drowning)
fMinOverrideDurationMs = 2000        ; Anti-flicker minimum duration
bAllowImpurePotions = true           ; Allow potions with side effects
```

> **Note on defaults:** The compile-time defaults in `OverrideConfig.h` are 10% for critical health/magicka/stamina thresholds. The shipped `Huginn.ini` sets these to 35%. Without the INI file, behavior differs significantly.

### 7.2 Compile-Time Configuration

**`CandidateConfig.h`** (`src/candidate/CandidateConfig.h`) holds:
- Cooldown durations per item type
- Active buff filter thresholds (including `resistThresholdToFilter = 50.0f`)
- Uncastable spell policy and penalty floor
- Max candidates after filter (500)
- Future: `LoadFromINI` method stubbed but not yet implemented

**`OverrideConfig.h`** (`src/override/OverrideConfig.h`) holds:
- Fallback threshold defaults (used if INI missing)
- Priority constants
- Override utility boost (1000.0)

### 7.3 Future: MCM/dMenu

Full in-game configuration with sliders/toggles. Not yet implemented.

---

## 8. Design Decisions

| Question | Decision | Rationale |
|----------|----------|-----------|
| Relevance: gate or weight? | **Both** (hard gate via vitals/buffs + soft via UtilityScorer thresholds) | Full vitals and active buffs are hard gates; everything else is soft |
| Override threshold scale | **Percentage (0.0-1.0)** | Consistent with vitals representation |
| Prior scope | **Intrinsic quality only** | Context handled by ContextRuleEngine (separation of concerns) |
| Prior scale | **0.0-1.0** | Normalized, easy to reason about |
| Cooldown granularity | **Per item type** | Different items have different use patterns |
| Hysteresis band size | **15%** (INI default) | Balances responsiveness with stability |
| Override priority range | **0-100** | Room for user-defined overrides |
| Favorites handling | **1.3x–2.5x multiplier** (INI-configurable via `[Favorites]` section) | `fFavoritesBoostMin` / `fFavoritesBoostMax` — user favorites surface prominently |

---

## 9. Testing Checklist

### 9.1 Filter Tests

- [ ] Spell with cost > magicka is excluded (Disallow mode)
- [ ] Spell with cost > magicka is kept (Penalize/Allow modes)
- [ ] Equipped spell is excluded
- [ ] Equipped weapon passes global filter (per-slot skip in SlotAllocator)
- [ ] Equipped ammo is excluded
- [ ] Recently used item respects cooldown
- [ ] Healing spell excluded when HP = 100%
- [ ] Resist potion excluded when already resisting above 50%
- [ ] Invisible potion excluded when already invisible
- [ ] Deduplication removes duplicate candidates in single pass

### 9.2 Prior Tests

- [ ] Base prior is 0.3 with no intrinsic adjustments
- [ ] Expert spell (cost 200) gets higher prior than Novice (cost 20)
- [ ] High-magnitude potion gets higher prior than low-magnitude
- [ ] Low-count potion gets scarcity penalty
- [ ] Depleted enchanted weapon gets charge penalty
- [ ] Type-matched ammo gets compatibility bonus
- [ ] Scrolls always get BASE_PRIOR (0.3)

### 9.3 Context Weight Tests (ContextRuleEngine)

- [ ] Healing context weight increases at low HP
- [ ] Anti-Undead weight increases vs undead target
- [ ] Stealth weight increases while sneaking
- [ ] Workstation potions surface at matching workstations

---

## See Also

- [0-pipeline.md](0-pipeline.md) - Overall recommendation pipeline
- [5-slots.md](5-slots.md) - Slot classification and overrides (detailed)
- [4-contextual-bandits.md](4-contextual-bandits.md) - contextual bandit learning implementation
- `src/learning/PriorCalculator.cpp` - Intrinsic prior calculation (all candidate types)
- `src/context/ContextRuleEngine.cpp` - Context-dependent weighting (all game state conditions)
- `src/learning/UtilityScorer.cpp` - Scoring formula implementation
- `src/candidate/CandidateFilters.cpp` - All hard filters
- `src/candidate/CandidateConfig.h` - Filter/scoring configuration
- `src/override/OverrideManager.cpp` - Override evaluation and item selection
- `src/override/OverrideConfig.h` - Override defaults and runtime settings
