# Candidate System Reference

> **Module:** `src/candidate/`
> **Verified against:** v0.19.10

A file-level map of the candidate module. For *why* each filter exists and how
candidate generation sits in the pipeline, see
[architecture/3-candidate-filtering.md](../architecture/3-candidate-filtering.md).

## Files

| File | Purpose |
|------|---------|
| `CandidateTypes.h/cpp` | `SourceType`, `CandidateBase`, the per-source candidate structs, `CandidateVariant` |
| `CandidateConfig.h` | `CandidateConfig` struct + the `g_candidateConfig` global, `UncastableSpellPolicy` |
| `CandidateFilters.h/cpp` | `FilterStats` and the batch filter pass |
| `CandidateGenerator.h/cpp` | Singleton orchestrator: gather, filter, `GenerationStats` |
| `CooldownManager.h/cpp` | Per-item cooldown tracking |

## Pipeline

```
Registries → Gather → Filter (+ dedup) → Output
```

`GenerateCandidates(player, currentMagicka)` gathers from six sources in this
order — spells, potions, scrolls, weapons, ammo, soul gems — into a persistent
buffer that retains its capacity across calls, then runs one filter pass that
moves survivors into the returned vector.

**No relevance is computed here.** Relevance scoring moved out of this module:
`ContextRuleEngine` (via `UtilityScorer`) produces the context weights that rank
candidates and the dominant reason the pipeline hands to the display.

## Filter order (cheapest first)

Filters 1–4 run in a single `std::visit` dispatch per candidate
(`RunVisitorFilters`), returning on the first failure:

1. **Affordability** — spells: effective magicka cost vs. current magicka, subject
   to `uncastableSpellPolicy`; items: `count > 0`; weapons: always pass
2. **Equipped** — already equipped/known-equipped
3. **Full vitals** — restore-health/magicka/stamina when that vital is full
   (skipped for food and alcohol)
4. **Active buff** — invisibility, muffle, armor, an active summon, or a resist
   whose resistance already exceeds `resistThresholdToFilter`
5. **Cooldown** — against a set of active cooldowns snapshotted once per pass, so
   no per-candidate lock is taken
6. **Deduplication** — inline, on `CandidateBase::GetDeduplicationKey()`

A final truncation to `maxCandidatesAfterFilter` (default 500) applies if the
survivor count exceeds it.

`FilterStats` still carries a `filteredByRelevance` counter; nothing increments
it, because the relevance-threshold filter no longer exists.

## Key config flags

`CandidateConfig` (`CandidateConfig.h`) is entirely compile-time-defaulted. Only
two fields are read from INI, from the `[Candidates]` section:

| INI key | Field | Default |
|---------|-------|---------|
| `sUncastableSpellPolicy` | `uncastableSpellPolicy` (`Disallow` / `Penalize` / `Allow`) | `Disallow` |
| `bEnableSoulGemRecharge` | `enableSoulGemRecharge` | `true` |

The remaining fields are code-level tunables:

| Field | Purpose |
|-------|---------|
| `filterRedundantResists` / `resistThresholdToFilter` | Drop resist potions *and* spells once that resistance is high enough |
| `filterActiveBuffs` | Drop items whose effect is already active |
| `filterHealingWhenFull`, `filterMagickaWhenFull`, `filterStaminaWhenFull` | Drop restores for a full vital |
| `spellCooldown`, `potionCooldown`, `scrollCooldown`, `weaponCooldown`, `ammoCooldown`, `soulGemCooldown`, `foodCooldown` | Per-source cooldown durations, in seconds |
| `maxCandidatesAfterFilter` | Post-filter cap |

`RefreshConfigFromGlobal()` copies `g_candidateConfig` into the generator's own
snapshot and re-syncs the cooldown durations. Call it only while the update loop
is paused.

## Thread safety

- **Config** — read-only during `GenerateCandidates()`; the generator holds a
  private copy, so writes to `g_candidateConfig` must happen with the update loop
  paused
- **Cooldowns** — `CooldownManager` guards its map with a `std::shared_mutex`
  (readers `shared_lock`, writers `unique_lock`); mutations are main-thread only

## Performance target

< 2 ms per `GenerateCandidates()` call. The actual figure is recorded per call in
`GenerationStats::generationTimeMs`.
