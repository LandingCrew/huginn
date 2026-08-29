# Candidate System Reference

> **Module:** `src/candidate/`
> **Version:** v0.7.x

## Files

| File | Purpose |
|------|---------|
| `CandidateTypes.h/cpp` | `SourceType`, `RelevanceTag`, candidate structs, `CandidateVariant` |
| `CandidateConfig.h` | Configuration struct with tunable parameters |
| `CandidateFilters.h/cpp` | Pre-filtering logic with statistics |
| `CandidateGenerator.h/cpp` | Main orchestrator, relevance computation |
| `CooldownManager.h/cpp` | Per-item cooldown tracking |

## Pipeline

```
Registries → Gather → Tag Relevance → Filter → Deduplicate → Output
```

## Filter Order (cheapest first)

1. Affordability (magicka / count > 0)
2. Equipped check
3. Cooldown check
4. Full Vitals (healing when full)
5. Active Buff check
6. Deduplication
7. Relevance threshold

## Key Config Flags

| Flag | Purpose |
|------|---------|
| `filterRedundantResists` | Filter resist potions AND spells when resistance high |
| `filterActiveBuffs` | Filter items with already-active effects |
| `filterHealingWhenFull` | Filter healing when HP full |
| `*Cooldown` fields | Per-type cooldown durations |

## Thread Safety

- Config: Read-only during `GenerateCandidates()`
- Cooldowns: `IsOnCooldown()` thread-safe; mutations main-thread only

## Performance Target

< 2ms per generation call
