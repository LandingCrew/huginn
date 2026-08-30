# Candidate Generation, Filtering & Priors

> **Purpose:** Reference for what becomes a candidate, the hard filters that remove
> candidates, the intrinsic priors that bootstrap learning, and how survivors reach
> `UtilityScorer`.
> **Verified against v0.19.10 source** (`src/candidate/`, `src/learning/PriorCalculator.*`,
> `src/learning/UtilityScorer.*`, `src/Globals.cpp`, `configs/Huginn.ini`).

> **See also:**
> - [0-pipeline.md](0-pipeline.md) - Overall pipeline flow and the three-tier scoring system
> - [5-slots.md](5-slots.md) - Slot classification, overrides, and allocation
> - [4-contextual-bandits.md](4-contextual-bandits.md) - contextual bandit learning and the utility formula

---

## 1. Candidate Sources

### 1.1 SourceType

`SourceType` (`src/candidate/CandidateTypes.h:32`) is the tag that says which registry a
candidate came from. It has exactly eight values, asserted at compile time
(`static_assert(SOURCE_TYPE_COUNT == 8)`, `CandidateTypes.h:316`):

| Value | # | Variant alternative | Notes |
|---|---|---|---|
| `Spell` | 0 | `SpellCandidate` | |
| `Potion` | 1 | `ItemCandidate` | Health/Magicka/Stamina/Resist/Buff/Cure potions and Poisons |
| `Scroll` | 2 | `ScrollCandidate` | |
| `Weapon` | 3 | `WeaponCandidate` | |
| `Ammo` | 4 | `AmmoCandidate` | |
| `SoulGem` | 5 | `ItemCandidate` | |
| `Food` | 6 | `ItemCandidate` | Also Alcohol and Ingredient |
| `Staff` | 7 | `WeaponCandidate` | `WeaponType::Staff` promotes the source type |

> **There is no armor / apparel source type.** Fortify gear can never be gathered,
> scored, or recommended. This is roadmap issue #65, and it is the reason the Requiem
> answer to #63 is blocked — see [../roadmap.md](../roadmap.md). Any older doc claiming
> apparel is a candidate source is wrong.

`CandidateVariant` (`CandidateTypes.h:206`) has only **five** alternatives —
`SpellCandidate`, `ItemCandidate`, `WeaponCandidate`, `AmmoCandidate`, `ScrollCandidate`.
`SourceType` is finer-grained than the variant: one `ItemCandidate` can be a Potion, a
Food, or a SoulGem, and one `WeaponCandidate` can be a Weapon or a Staff. The mapping is
done in the factory functions (`CandidateTypes.cpp`), not by the gatherers.

### 1.2 Gather order

`CandidateGenerator::GenerateCandidates()` (`CandidateGenerator.cpp:114`) fills a
persistent gather buffer in a fixed order:

1. Spells (`SpellRegistry::ForEachSpell`) — effective magicka cost is computed here, once,
   via `spellItem->CalculateMagickaCost(player)` and cached on the candidate
2. Potions / food / ingredients (`ItemRegistry::ForEachItem`, skipping soul gems)
3. Scrolls (`ScrollRegistry::ForEachScroll`)
4. Weapons and staves (`WeaponRegistry::ForEachWeapon`)
5. Ammo (`WeaponRegistry::ForEachAmmo`)
6. Soul gems (`ItemRegistry::ForEachItem`, soul gems only) — `CandidateGenerator.cpp:310`

Gather order matters only because truncation (§2.4) keeps the first N in this order.
No relevance or priority is stamped during gathering; that is entirely
`ContextRuleEngine` + `UtilityScorer`'s job.

**Soul gems** are gathered wholesale, not "the best one". Every *filled* gem type becomes
a candidate (gems stack, so that is one candidate per Petty/Lesser/Common/… tier, not per
gem). Empty gems are excluded — they cannot recharge anything. `candidate.count` is set to
`filledCount` rather than the stack count when `filledCount > 0`
(`CandidateGenerator.cpp:362`), so a stack of ten petty gems holding one soul does not dodge
the scarcity penalty in `PriorCalculator`. The urgent path is separate and still picks a
single gem: `OverrideManager::FindSoulGem()`.

Gathering the whole gem set is gated by `enableSoulGemRecharge` (`CandidateGenerator.cpp:323`).

---

## 2. Hard Filters (Binary Gates)

Hard filters **remove candidates** entirely. A candidate must pass all of them to reach
scoring. Everything happens in one forward pass in `CandidateFilters::ApplyAllFilters()`
(`CandidateFilters.cpp:215`), which moves survivors out of the gather buffer into an output
vector — no in-place `erase_if`.

### 2.1 Order of operations

| # | Stage | Where |
|---|---|---|
| 1 | **Consolidated visitor** — affordability, equipped, full vitals, active buff, in that order, early-exiting on the first failure | `RunVisitorFilters()`, `CandidateFilters.cpp:20` |
| 2 | **Cooldown** — tested against a set of active cooldown keys snapshotted once per pass, so there is no per-candidate lock | `CandidateFilters.cpp:289` |
| 3 | **Deduplication** — inline, via a reusable `unordered_set<uint64_t>` | `CandidateFilters.cpp:300` |
| 4 | **Truncation** to `maxCandidatesAfterFilter` | `CandidateFilters.cpp:317` |

Stages 1–4 are a single `std::visit` dispatch per candidate, not four. Cooldown comes
*after* the vitals and buff checks, not before them — it is the cheapest lookup but it needs
the snapshot taken outside the loop.

There is also a vector-integrity guard at the top of `ApplyAllFilters` (size > capacity, or
capacity > 100000 → bail out and clear), a defence against external heap corruption on the
game's job threads.

### 2.2 What each filter checks, per candidate type

The visitor is written per variant alternative, and the arms are **not symmetric**. This
table is the code, not an idealisation:

| | Affordability | Equipped | Full vitals | Active buff |
|---|---|---|---|---|
| **Spell** | `currentMagicka < effectiveCost` → policy-dependent (§3) | `player.IsSpellEquipped(formID)` | RestoreHealth, RestoreMagicka | Invisibility, Muffle, Armor, `SpellType::Summon` vs `hasActiveSummon`, plus resist redundancy |
| **Item** (Potion/Food/SoulGem) | `count <= 0` | — (never filtered) | RestoreHealth, RestoreMagicka, RestoreStamina — **skipped entirely for `Food` and `Alcohol`** | resist redundancy, Waterbreathing, Invisibility |
| **Scroll** | `count <= 0` | — | RestoreHealth only | Invisibility, Muffle |
| **Ammo** | `count <= 0` | `isEquipped` | — | — |
| **Weapon / Staff** | always passes | always passes | — | — |

Gaps worth knowing, all present in v0.19.10:

- Spells have **no** stamina-full arm; scrolls have no magicka- or stamina-full arm.
- Items have no Muffle, Armor, or Summon arm.
- Weapons and staves pass every global filter. Equipped-weapon skipping is **per slot**,
  via `bSkipEquipped` in the `[PageN.SlotM]` INI sections, honoured by `SlotAllocator`
  (`src/slot/SlotAllocator.cpp:610`).

**Full-vital threshold:** `State::DefaultState::FULL_VITAL` is `1.0` and
`FULL_VITAL_EPSILON` is `0.01` (`CandidateFilters.cpp:8`), so the gate fires at ≥ 99%.

**Resist redundancy:** `IsResistPotionRedundant` (`CandidateFilters.cpp:370`) checks the
ResistFire/Frost/Shock/Poison/Magic item tags against `player.resistances` and the
`resistThresholdToFilter` percentage. `IsResistSpellRedundant`
(`CandidateFilters.cpp:403`) has no tag to read — `SpellTag` is full at 32 bits — so it
uses `SpellType::Defensive` or `SpellType::Buff` plus `ElementType` instead. Magic
resistance has no spell arm as a result.

### 2.3 Deduplication

The key is 64 bits, laid out `[sourceType:8][uniqueID:16][formID:32]`
(`CandidateTypes.h:74`). `uniqueID` (from `ExtraUniqueID`) separates enchanted weapon
instances that share a base FormID; it is 0 for everything else, so the key still
deduplicates correctly for non-weapons.

### 2.4 Truncation

`maxCandidatesAfterFilter` defaults to 500 (`CandidateConfig.h:119`). The survivors are cut
in **gather order** — no sort happens here, real prioritisation is `UtilityScorer`'s job.
Because weapons are gathered fourth and soul gems last, a truncating inventory drops gems
first. Truncation logs at `warn`.

### 2.5 Cooldown defaults

Compile-time only, from `CandidateConfig` (`CandidateConfig.h:92` onward). `CooldownManager`
seeds its own duration array from a default-constructed `CandidateConfig`, so the two cannot
drift (`CooldownManager.cpp:14`).

| Source type | Default |
|---|---|
| Spell | 2.0s |
| Potion | 3.0s |
| Scroll | 2.0s |
| Weapon | 5.0s |
| Ammo | 1.0s |
| SoulGem | 3.0s |
| Food | 3.0s |
| Staff | 2.0s (shares `spellCooldown` by design) |

### 2.6 FilterStats

`FilterStats` (`CandidateFilters.h:18`) counts what each stage removed. Note that
`filteredByRelevance` is **dead** — nothing increments it since the relevance filter was
removed. It is still summed into `TotalFiltered()`, harmlessly.

> **The relevance filter is gone.** `baseRelevance` no longer exists on `CandidateBase`, and
> neither does the `GetRelevance()` helper. Context-based exclusion now happens in
> `UtilityScorer` via the two soft thresholds in §5.

---

## 3. Uncastable Spell Policy

Controls what happens to spells the player cannot currently afford. Declared as
`UncastableSpellPolicy` in `CandidateConfig.h:12`.

| Mode | INI value | Actual behavior in v0.19.10 |
|---|---|---|
| **Disallow** | `Disallow` | `FilterResult::Affordability` — the spell is removed (default) |
| **Penalize** | `Penalize` | The spell is **kept, unmodified** |
| **Allow** | `Allow` | The spell is kept, unmodified |

`effectiveCost` is `spell->CalculateMagickaCost(player)`, cached during gathering
(`CandidateGenerator.cpp`), so perk and enchantment reductions are included. For
concentration spells `CalculateMagickaCost` returns a per-second cost; if it comes back
≤ 0 the base cost is substituted.

> **Penalize is not implemented.** There is no shortfall ratio, no penalty floor, and no
> multiplier anywhere in `src/`. `PassesAffordabilityFilter` and `RunVisitorFilters` both
> branch only on `policy == Disallow`, so `Penalize` and `Allow` are behaviorally
> identical. The `penaltyFloor` field that used to hold this has been removed from
> `CandidateConfig`, and `fUncastablePenaltyFloor` was removed from
> `configs/Huginn.ini` in 0.19.13 rather than left implying it works. `Penalize`
> remains behaviourally identical to `Allow`; that is tracked on the roadmap as a
> scoring feature to design, not a settings bug.

---

## 4. Intrinsic Priors (PriorCalculator)

`PriorCalculator` (`src/learning/PriorCalculator.cpp`) supplies **intrinsic quality
heuristics** that bootstrap the contextual bandit before it has data. They are
deliberately **not context-aware** — every context-dependent adjustment lives in
`ContextRuleEngine`. The class takes `PlayerActorState` and, pointedly, no `GameState`:
the player state is there only so the ammo path can see which launcher is equipped.

**Base prior:** `BASE_PRIOR = 0.3f`. Every path clamps its result to `[0.0, 1.0]`.

### 4.1 Spell priors (`PriorCalculator.cpp:36`)

```
prior = BASE_PRIOR + min(baseCost / MAX_REASONABLE_SPELL_COST, 1) × COST_SCALE_FACTOR
```

Uses `baseCost`, not `effectiveCost` — spell tier is intrinsic, the player's perks are not.
Expert spell (cost 200) → 0.4; Novice (cost 20) → 0.31.

### 4.2 Item priors (`PriorCalculator.cpp:67`)

```
prior = BASE_PRIOR
      + min(log(1 + magnitude) / log(1 + MAGNITUDE_REFERENCE_VALUE), 1) × MAGNITUDE_SCALE_FACTOR
      - (1 - count/LOW_COUNT_THRESHOLD) × COUNT_PENALTY_SCALE       [only when 0 < count < 5]
```

### 4.3 Weapon priors (`PriorCalculator.cpp:104`)

```
prior = BASE_PRIOR - (1 - chargePercent) × CHARGE_PENALTY_SCALE
```

Applies only when `hasEnchantment && GetChargePercent() < 0.2`. `GetChargePercent()`
returns 1.0 when `maxCharge` is 0, so unenchanted weapons are untouched.

### 4.4 Ammo priors (`PriorCalculator.cpp:130`)

```
prior = BASE_PRIOR
      + AMMO_TYPE_MATCH_BONUS                                  [Arrow+bow or Bolt+crossbow]
      + (1 - count/AMMO_LOW_COUNT_THRESHOLD) × AMMO_SCARCITY_SCALE  [only when 0 < count < 20]
```

Scarcity is a **bonus** here, the inverse of items: running low on arrows is exactly when
you want to be told.

### 4.5 Scroll priors (`PriorCalculator.cpp:171`)

Scrolls now mirror the item path — magnitude bonus plus scarcity penalty, same constants:

```
prior = BASE_PRIOR
      + min(log(1 + magnitude) / log(1 + MAGNITUDE_REFERENCE_VALUE), 1) × MAGNITUDE_SCALE_FACTOR
      - (1 - count/LOW_COUNT_THRESHOLD) × COUNT_PENALTY_SCALE
```

> Older docs said scrolls return `BASE_PRIOR` unconditionally. That has not been true since
> the scroll magnitude/scarcity arm was added.

### 4.6 Prior constants

**Source:** `src/learning/PriorCalculator.h` (private, compile-time, not INI-exposed).

```cpp
static constexpr float BASE_PRIOR = 0.3f;

// Magnitude scaling (logarithmic) — items and scrolls
static constexpr float MAGNITUDE_REFERENCE_VALUE = 100.0f;  // Major healing potion
static constexpr float MAGNITUDE_SCALE_FACTOR = 0.15f;

// Spell cost scaling (linear)
static constexpr float MAX_REASONABLE_SPELL_COST = 200.0f;
static constexpr float COST_SCALE_FACTOR = 0.1f;

// Inventory scarcity penalty — items and scrolls
static constexpr float LOW_COUNT_THRESHOLD = 5.0f;
static constexpr float COUNT_PENALTY_SCALE = 0.1f;

// Weapon charge penalty
static constexpr float CHARGE_PENALTY_SCALE = 0.2f;

// Ammo
static constexpr float AMMO_LOW_COUNT_THRESHOLD = 20.0f;
static constexpr float AMMO_SCARCITY_SCALE = 0.1f;
static constexpr float AMMO_TYPE_MATCH_BONUS = 0.15f;
```

`CalculatePrior` dispatches through `std::visit` with a `static_assert(always_false_v<T>)`
fallback, so adding a sixth `CandidateVariant` alternative is a build error until someone
decides its prior.

### 4.7 Context weights are somewhere else

All context-dependent weighting is `ContextRuleEngine`'s
(`src/context/ContextRuleEngine.cpp`), and the candidate→weight lookup is
`Context::WeightForCandidate` (`src/context/ContextWeightForCandidate.cpp`), which takes the
`max` over every weight whose tag the candidate carries. Context covers HP-driven healing,
combat and AOE relevance, target type (undead / daedra / dragon), stealth, elemental resist
while taking that damage type, environmental cases (underwater, looking at a lock, falling),
weapon charge tiers, ammo need, and workstations.

See [4-contextual-bandits.md](4-contextual-bandits.md) and the `[ContextWeights]` INI
section for the full table.

### 4.8 Workstation context

Detection is `WorldState.isLookingAtWorkstation` / `workstationType`; the weights are set in
`ContextRuleEngine::EvaluateEnvironmentalRules` (`ContextRuleEngine.cpp:248`), keyed on
`RE::TESFurniture::WorkBenchData::BenchType`:

| Bench types | Weight set | INI key | Shipped value |
|---|---|---|---|
| `kCreateObject` (1), `kSmithingWeapon` (2), `kSmithingArmor` (7) | `fortifySmithingWeight` | `fWeightAtForge` | 0.8 |
| `kEnchanting` (3), `kEnchantingExperiment` (4) | `fortifyEnchantingWeight` | `fWeightAtEnchanter` | 0.8 |
| `kAlchemy` (5), `kAlchemyExperiment` (6) | `fortifyAlchemyWeight` | `fWeightAtAlchemyLab` | 0.8 |

Weights are normalised to `[0,1]`; the `8.0` figure in old docs was the pre-normalisation
0–10 scale, and there is no separate `workstationBoost` multiplier. `RelevanceTag` no longer
exists anywhere in `src/` — `CandidateGenerator` sets no tags of any kind.

> Requiem-family modlists (LoreRim) strip Fortify Smithing/Enchanting from alchemy, so this
> context has nothing to rank there — roadmap #63.

---

## 5. Reaching UtilityScorer

`PipelineCoordinator::ScoreCandidates` (`src/pipeline/PipelineCoordinator.cpp:278`) calls
`GenerateCandidates` and hands the survivors straight to
`UtilityScorer::ScoreCandidates`. In [0-pipeline.md](0-pipeline.md)'s numbering that is
Stage 2 → Stage 6.

`ScoreCandidates` (`src/learning/UtilityScorer.cpp:26`) then:

1. Evaluates context rules **once** for the whole list, and optionally hands the weight map
   back so the widget can explain the ranking off the same weights that produced it.
2. Pre-computes `StateFeatures` once and acquires the learner / usage-memory readers once,
   so the loop does two lock acquisitions instead of ~2N.
3. **Soft gate 1** — skips any candidate whose context weight is below
   `minimumContextWeight` (`UtilityScorer.cpp:78`). **Favorited candidates are exempt**:
   explicit player intent must stay observable to the learner even at low context.
4. Scores each survivor (§6).
5. **Soft gate 2** — keeps the result only if `utility >= minimumUtility`
   (`UtilityScorer.cpp:93`).
6. **Cold-start fallback** (`UtilityScorer.cpp:112`) — if fewer than `topNCandidates` (10)
   survived, re-walks the unscored candidates with their context weight floored at
   `coldStartUCBBoost × ucb`, so untried items can surface. Results are marked
   `isColdStartBoosted`. Self-heals as UCB decays with visits.
7. `ApplyFavoritesRankScaling` — replaces the provisional uniform favorites multiplier with
   the rank-scaled one. It never drops entries, even if rescaling pushes one under
   `minimumUtility`.
8. `std::partial_sort` for the top N.
9. `WildcardManager::ApplyWildcards` against the page **this tick allocates**, taken from
   the pipeline snapshot rather than a live `SlotAllocator` read.

These two thresholds are what replaced the removed `baseRelevance` filter:

| Setting | INI key (`[Scoring]`) | Default |
|---|---|---|
| `minimumContextWeight` | `fMinimumContextWeight` | `0.05` |
| `minimumUtility` | `fMinimumUtility` | `0.1` |
| `coldStartUCBBoost` | `fColdStartUCBBoost` | `0.2` |
| `topNCandidates` | `iTopNCandidates` | `10` |

> `maxCandidatesPerCycle` is parsed by `ScorerSettings` and
> stored on `ScorerConfig`, but nothing reads it — the scoring loop is bounded only by
> `maxCandidatesAfterFilter` upstream. Dead setting — the `iMaxCandidatesPerCycle`
> KEY was removed from the shipped INI in 0.19.13; the parsing code and the
> `ScorerConfig` field are still there and still dead, tracked on the roadmap.

---

## 6. Scoring Formula

The single source of truth is `UtilityScorer::ComputeUtility`
(`src/learning/UtilityScorer.cpp:318`), used by both the normal path and the cold-start
fallback:

```
utility = contextWeight × (1 + λ(confidence) × learningScore)
          × correlationBonus × potionMultiplier × favoritesMultiplier
```

with (`UtilityScorer.cpp:263`, `:334`)

```
learningScore = α·Q + (1 − α)·prior + β·UCB + recencyBoost
α             = confidence          (from FeatureQLearner visit counts, not configurable)
β             = explorationWeight   (fExplorationWeight, default 0.2)
λ(confidence) = lambdaMin + confidence × (lambdaMax − lambdaMin)   (0.5 → 3.0)
```

Context is a **gate**: zero context weight means zero utility no matter what the learner
believes. `prior` is §4's intrinsic value; `recencyBoost` comes from `UsageMemory` and is
folded into `learningScore`, so the cold-start path inherits it automatically and must not
add it a second time.

`correlationBonus`, `potionMultiplier` and `favoritesMultiplier` come from
`CorrelationBooster`, `PotionDiscriminator` and the favorites mode respectively. See
[0-pipeline.md](0-pipeline.md) Stage 6 and
[4-contextual-bandits.md](4-contextual-bandits.md) for the learner side.

---

## 7. Hard Overrides

Overrides bypass normal scoring, forcing specific items into slots for urgent conditions.
They are evaluated **after** scoring, at the top of
`PipelineCoordinator::AllocateAndLock` (`PipelineCoordinator.cpp:384`) — Stage 4 in
[0-pipeline.md](0-pipeline.md)'s narrative ordering, but structurally part of allocation.
For the full table (priorities, thresholds, hysteresis, potion selection) see:

- [0-pipeline.md](0-pipeline.md) — Stage 4: Override Check
- [5-slots.md](5-slots.md) — Override System (detailed evaluation flow)
- `src/override/OverrideManager.cpp` — Implementation
- `src/override/OverrideConditions.h` — Priority constants (`Priority::CRITICAL_HEALTH = 100`, …)
- `[Overrides]` in `Huginn.ini` — All thresholds are INI-configurable

The only field candidates carry for this is `CandidateBase::overrideReason`
(`Context::ContextReason`), set by `OverrideManager` alone; ordinary candidates leave it
`None` and the display falls back to the per-tick context reason.

---

## 8. Configuration

### 8.1 `[Candidates]` — what is actually read

`LoadCandidateConfigFromINI` (`src/Globals.cpp:181`) reads exactly two keys into the global
`g_candidateConfig`:

```ini
[Candidates]
sUncastableSpellPolicy = Disallow    ; Disallow / Penalize / Allow (case-insensitive)
bEnableSoulGemRecharge = true
```

It is called on `kPostLoadGame`/`kNewGame` (`src/Main.cpp:221`) and again on
`hg reload` (`src/settings/SettingsReloader.cpp:155`).

> **Was inert until 0.19.13; fixed.** `CandidateGenerator` keeps its own
> `CandidateConfig m_config`, and nothing assigned it from `g_candidateConfig` —
> `Initialize()` did not copy it and `RefreshConfigFromGlobal()` had no callers,
> so the filters ran on compile-time defaults and changing either value in the
> INI had no effect. Invisible because the shipped values matched those defaults.
> `RefreshConfigFromGlobal()` is now called from `InitializeGameSystems` (outside
> the `IsInitialized` guard, so a save load re-pushes rather than keeping the
> previous game's values) and from `SettingsReloader`, and `ResetAllToDefaults`
> resets the global too.
>
> Note what this did NOT fix: `bEnableSoulGemRecharge` gates
> `GatherSoulGemCandidates` and nothing else, so `OverrideManager::FindSoulGem`
> still surfaces a gem on the urgent weapon-charge path with the setting off.
> Roadmap item.

### 8.2 `[Scoring]` — thresholds that do apply

```ini
[Scoring]
fMinimumUtility = 0.1                ; below this, dropped from results
fMinimumContextWeight = 0.05         ; below this, skipped before scoring (favorites exempt)
fColdStartUCBBoost = 0.2             ; context floor for untried items
iTopNCandidates = 10
```

Loaded by `ScorerSettings` (`src/learning/ScorerSettings.cpp`), which is hot-reloadable.

### 8.3 `[Favorites]`

```ini
sFavoritesMode = Boost               ; Boost / Off / Suppress
fFavoritesBoostMin = 1.3
fFavoritesBoostMax = 2.5
```

In Boost mode favorites are ranked by utility each update; the strongest gets `Max`, the
weakest `Min`, linear in between, and a single favorite gets `Max`.

### 8.4 `[Overrides]`

All thresholds, hysteresis bands and enable toggles are INI-configurable:

```ini
fCriticalHealthThreshold = 0.35      ; shipped value
fCriticalHealthHysteresis = 0.15     ; deactivate at threshold + gap
bEnableCriticalHealth = true
; same pattern for magicka, stamina, weapon charge (0.25/0.05), low ammo (10/15)
fMinOverrideDurationMs = 2000        ; anti-flicker minimum
bAllowImpurePotions = true
bEnableDrowning = true               ; no threshold — binary
```

> **Note on defaults:** the compile-time fallbacks in `src/override/OverrideConfig.h` are
> **10%** for critical health/magicka/stamina; the shipped `Huginn.ini` sets **35%**.
> Without the INI file the behavior differs significantly.

### 8.5 Compile-time only

`src/candidate/CandidateConfig.h` holds everything the INI cannot reach:

- Cooldown durations per source type (§2.5)
- Active-buff filter toggles: `filterRedundantResists`, `filterActiveBuffs`,
  `filterHealingWhenFull`, `filterMagickaWhenFull`, `filterStaminaWhenFull`
- `resistThresholdToFilter = 50.0f` — a **percentage**, not a ratio
- `maxCandidatesAfterFilter = 500`
- A commented-out `LoadFromINI` / `SaveToINI` pair that was never implemented

`src/learning/PriorCalculator.h` holds the prior constants (§4.6). `src/override/OverrideConfig.h`
holds the fallback override thresholds; the priority constants live next door in
`OverrideConditions.h`.

---

## 9. Elemental Damage Enrichment

Instant-hit elemental damage (fire bolts, ice spikes) does not show up in
`PollPlayerMagicEffects()`, which only catches ongoing DoT effects. It is bridged by
`PipelineCoordinator::EnrichElementalDamage` (`PipelineCoordinator.cpp:230`), which turns
recent per-type damage timestamps from `HealthTrackingState` into
`PlayerActorState.effects` flags before context evaluation runs.

| Damage type | Window | Effect flag | Timestamp source |
|---|---|---|---|
| Fire | 5.0s | `isOnFire` | `timeSinceLastFire` via `DamageEventSink` |
| Frost | 5.0s | `isFrozen` | `timeSinceLastFrost` |
| Shock | 5.0s | `isShocked` | `timeSinceLastShock` |

The window is `State::VitalTracking::ELEMENTAL_DAMAGE_ENRICHMENT_WINDOW = 5.0f`
(`src/state/StateConstants.h:684`). Sub-threshold hits — damage reduced below
`HEALTH_DAMAGE_THRESHOLD` by resistances — are recorded with zero magnitude
(`src/state/StateManager_HealthTracking.cpp:149`) so the per-type timestamps still update
and a heavily-resisted player still gets resist recommendations.

> This lived in `Main.cpp` before the pipeline extraction. It is not there any more.

---

## 10. Design Decisions

| Question | Decision | Rationale |
|---|---|---|
| Relevance: gate or weight? | **Both** — hard gates for vitals/buffs, soft thresholds in `UtilityScorer` | "You are at full health" is binary; "this is somewhat relevant" is not |
| Prior scope | **Intrinsic quality only** | Context is `ContextRuleEngine`'s, enforced by `PriorCalculator` taking no `GameState` |
| Prior scale | **0.0–1.0**, clamped on every path | Normalized, easy to reason about |
| Cooldown granularity | **Per source type** | Different item classes have different use rhythms |
| Soul gem breadth | **Every filled tier**, not the best one | A hard-coded capacity preference in front of the preference learner meant a Petty gem could never be scored, chosen, or rewarded |
| Weapon equipped-skip | **Per slot** (`bSkipEquipped`), not global | A slot showing alternatives and a slot showing your current weapon are both valid layouts |
| Override threshold scale | **Fraction (0.0–1.0)** | Consistent with how vitals are represented |
| Favorites handling | **1.3×–2.5× rank-scaled multiplier**, INI-configurable | Favorites are explicit player intent, and are exempt from the context-weight gate for the same reason |

---

## 11. Manual Verification Checklist

There is no automated test suite in the repository; these are in-game checks
(see [../playtest/LongPlaySoak.md](../playtest/LongPlaySoak.md) for the soak protocol and
`hg recs` for a scored breakdown).

### 11.1 Filters

- [ ] Spell costing more than current magicka is excluded (Disallow)
- [ ] Spell costing more than current magicka is kept (Penalize / Allow — identical today)
- [ ] Equipped spell is excluded
- [ ] Equipped weapon passes the global filters; per-slot `bSkipEquipped` still hides it
- [ ] Equipped ammo is excluded
- [ ] Recently used item respects its per-type cooldown
- [ ] Healing spell and health potion excluded at ≥ 99% HP
- [ ] Food and alcohol still surface at full health (vitals arm is skipped for them)
- [ ] Resist potion excluded when that resistance is already above 50%
- [ ] Invisibility potion excluded while invisible
- [ ] Two enchanted weapons sharing a base FormID both survive dedup (distinct `uniqueID`)
- [ ] Empty soul gems never appear; a stack of ten petty gems with one soul reports count 1

### 11.2 Priors

- [ ] Base prior is 0.3 with no intrinsic adjustments
- [ ] Expert spell (cost 200) outranks Novice (cost 20) on prior alone
- [ ] High-magnitude potion beats low-magnitude
- [ ] Low-count potion takes a scarcity penalty; low-count ammo takes a scarcity *bonus*
- [ ] Depleted enchanted weapon (< 20% charge) takes a charge penalty
- [ ] Arrows get the type-match bonus with a bow equipped, bolts do not
- [ ] Scrolls take magnitude and scarcity like potions (not a flat 0.3)

### 11.3 Context weights

- [ ] Healing weight rises as HP falls
- [ ] Anti-undead weight rises against an undead target
- [ ] Stealth weight rises while sneaking
- [ ] Fortify Smithing potions surface at a forge, Enchanting at an arcane enchanter,
      Alchemy at an alchemy lab (vanilla alchemy required — Requiem strips these)

---

## See Also

- [0-pipeline.md](0-pipeline.md) — Overall recommendation pipeline
- [2-classifiers.md](2-classifiers.md) — How registry entries get their types and tags
- [4-contextual-bandits.md](4-contextual-bandits.md) — Contextual bandit learning implementation
- [5-slots.md](5-slots.md) — Slot classification and overrides
- [../reference/candidate-system.md](../reference/candidate-system.md) — Candidate system reference
- [../reference/ConsoleCommands.md](../reference/ConsoleCommands.md) — `hg recs`, `hg reload`
- `src/candidate/CandidateGenerator.cpp` — Gathering from the registries
- `src/candidate/CandidateFilters.cpp` — All hard filters
- `src/candidate/CandidateTypes.h` — `SourceType`, `CandidateVariant`, dedup key
- `src/candidate/CandidateConfig.h` — Filter configuration (compile-time)
- `src/learning/PriorCalculator.cpp` — Intrinsic priors
- `src/learning/UtilityScorer.cpp` — Scoring formula and soft thresholds
- `src/context/ContextRuleEngine.cpp` — Context-dependent weighting
- `src/context/ContextWeightForCandidate.cpp` — Candidate → context weight lookup
- `src/override/OverrideManager.cpp` — Override evaluation and item selection
