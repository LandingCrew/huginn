# Survival Mode

**Verified against `src/` at v0.19.10.**

Huginn integrates with exactly two survival mods, by hardcoded FormID lookup:
Creation Club **Survival Mode** and **Survival Mode Improved SKSE** (SMI). This
document covers both halves of that integration — recovering true base vitals
past a survival penalty, and reading the hunger/cold/exhaustion state itself.

File paths in the older version of this document (`src/context/ContextSensor.cpp`)
no longer exist. The code now lives in:

| Concern | File |
|---|---|
| True base max HP/MP/SP | `src/state/StateManager_Vitals.cpp` |
| Need levels, warmth, globals cache | `src/state/StateManager_Survival.cpp` |
| Stage constants | `src/state/StateConstants.h` (`SurvivalThreshold`) |
| Cached global pointers | `src/state/StateManager.h` |

---

## Part 1 — True base max vitals

### The problem

Skyrim's actor value system layers several modifiers on a base value:

- **Base** — racial/level-up value
- **Permanent modifiers** — perks, abilities, leveling
- **Temporary modifiers** — potions, spells, enchantments
- **Damage modifiers** — diseases, damage taken, **and survival penalties**

With 165 base Health but 88 effective Health under a survival penalty,
`GetPermanentActorValue()` returns 88 — the penalty is already folded in. To show
the player what their maximum would be without the debuff, the penalty has to be
recovered from somewhere.

### How SMI stores the penalty

SMI applies its penalties through `RestoreActorValue(kPermanent, …)` and records
the amount it subtracted in the Variable actor values:

| Actor value | Holds |
|---|---|
| `kVariable02` | Stamina penalty (hunger) |
| `kVariable03` | Magicka penalty (exhaustion) |
| `kVariable04` | Health penalty (cold) |

<!-- UNVERIFIED: that SMI writes these particular Variable AVs, and that it
     applies penalties via RestoreActorValue on the permanent modifier. This is
     SMI's internal behaviour, taken from its published source
     (https://github.com/colinswrath/Survival-Mode-Improved-SKSE) and not
     checkable from this repository. The formula below is written so that it
     degrades to the correct answer if the assumption is wrong — see
     "Graceful fallback". -->

### The formula

`PollPlayerVitals()` (`StateManager_Vitals.cpp`) computes both a base and an
effective maximum for each vital:

```
baseMax      = temporaryModifier + permanentActorValue + variableAVPenalty
effectiveMax = currentValue - damageModifier
percentage   = currentValue / effectiveMax     (clamped to [0,1])
```

The `variableAVPenalty` term is what recovers the pre-debuff value. Note that
`effectiveMax` is derived from the damage modifier, not from `baseMax` — the two
are computed independently, which is why an unknown survival mod that uses damage
modifiers still gets a correct effective max even if the base max is unrecoverable.

Non-finite damage modifiers abort the update and leave the previous state in
place.

### Graceful fallback

There is **no detection step**. The Variable AVs are read unconditionally; when
no mod is writing them they read `0.0f` and the formula collapses to
`temporary + permanent`, which is the right answer with no survival mod
installed. Nothing is logged, nothing warns, and no configuration is involved.

`SurvivalThreshold::MIN_PENALTY_DETECTION = 0.1f` exists in `StateConstants.h` as
the noise floor for "a penalty is present", but the vitals path does not branch
on it.

> The older version of this document showed a `hasSurvivalPenalties` detection
> block and a block of `[Vitals Debug]` log lines including
> "Survival mod detected: Using Variable AV penalty tracking". Neither exists in
> the code — `grep -rn "Vitals Debug" src/` returns nothing. Do not go looking
> for those lines in a log.

### Where base vs effective max is shown

The ImGui debug widget draws a **segmented vital bar**
(`StateManagerDebugWidget::DrawVitalBarWithDebuff`, `src/ui/`): the bar is scaled
to `baseMax`, the portion up to `effectiveMax` is drawn in the normal colour, and
the difference is drawn in a debuff colour when `baseMax - effectiveMax > 1.0`.
It is a bar, not the `HP: 73% (202/275) [202 eff]` text format the older document
described.

### Compatibility

| Mod | Variable AV penalty tracking | Base max recovery | Effective max |
|---|---|---|---|
| Survival Mode Improved SKSE | Assumed yes (see UNVERIFIED note above) | Full | Correct |
| CC Survival Mode alone | Unknown | Partial — falls back to `temporary + permanent` | Correct |
| No survival mod | N/A — reads 0 | Full (nothing to recover) | Correct |
| Frostfall, iNeed, Sunhelm, Campfire, others | Unknown | Depends entirely on how they apply penalties | Correct if they use damage modifiers |

The effective-max column is correct in every row because it never depends on the
survival mod's implementation.

---

## Part 2 — Survival state detection

Huginn reads hunger, cold and exhaustion **stages** so contexts can react —
surfacing food when starving, warming items when freezing. Stages are integers,
higher is worse, `0` is neutral.

### Cached globals

`CacheSurvivalGlobals()` runs once (guarded by `m_survivalGlobalsCached`) and
looks up two plugins.

**CC Survival Mode** — `ccqdrsse001-survivalmode.esl`:

| FormID | Meaning |
|---|---|
| `0x81B` | Cold need value (0–1000) |
| `0x81A` | Hunger need value (0–1000) |
| `0x816` | Exhaustion need value (0–1000) |
| `0x826` | Survival mode enabled toggle |

**Survival Mode Improved** — `SurvivalModeImproved.esp`:

| FormID | Meaning |
|---|---|
| `0xA14` | Pre-computed hunger stage (0–5) |
| `0xA1C` | Pre-computed exhaustion stage (0–5) |
| `0xD1E` | Pre-computed cold stage (0–5) |
| `0xF27` | Hunger enabled flag |
| `0xF28` | Cold enabled flag |
| `0xF29` | Exhaustion enabled flag |

`m_smiInstalled` is set if **any** of the three stage globals resolves.

<!-- UNVERIFIED: all twelve FormIDs above. They were taken from SMI's FormLoader.h
     and cannot be re-checked from this repository. What IS verified is that the
     code looks up exactly these values in exactly these plugins. -->

The FormIDs are also compiled in — there is no INI or JSON to point Huginn at a
different survival mod's globals.

### Poll order

`PollPlayerSurvival()`:

1. **Enabled gate.** `survivalModeActive` is set from the CC
   `Survival_ModeEnabled` global (`0x826`) alone — this is the single source of
   truth, deliberately not inferred from individual need values, because stale
   values from a previous session would produce false positives. If the toggle is
   off or absent, all three needs stay neutral and no further reading happens.
2. **SMI path** (when `m_smiInstalled`). Each need's stage global is read
   directly as an integer, but only if its enable flag is `>= 1.0`. A `nullptr`
   enable flag is treated as enabled — safe fallback. A need SMI has switched off
   stays at neutral, so food is not recommended when hunger tracking is disabled.
3. **CC fallback** (when SMI is absent). Raw 0–1000 values are converted to stages
   by the thresholds below.
4. **Warmth.** When survival mode is active, `warmthRating` is read from the
   native CC Survival `GetWarmthRating` engine function, resolved through
   `REL::RelocationID(25834, 26394)`. If the address does not resolve, the
   function is not called and warmth stays `0`.

State is written under a unique lock only when something changed; warmth uses a
`1.0` epsilon.

### CC threshold conversion

Only reached when SMI is **not** installed. Constants are named in
`SurvivalThreshold`.

**Cold** (UESP: <https://en.uesp.net/wiki/Skyrim:Cold>)

| Raw | Stage | Name |
|---|---|---|
| 0–49 | 0 | Warm |
| 50–119 | 1 | Comfortable |
| 120–299 | 2 | Chilly |
| 300–499 | 3 | Very Cold |
| 500–799 | 4 | Freezing |
| 800–1000 | 5 | Numb |

**Hunger** (UESP: <https://en.uesp.net/wiki/Skyrim:Hunger>)

| Raw | Stage | Name |
|---|---|---|
| 0–79 | 0 | Well Fed |
| 80–159 | 1 | Fed |
| 160–339 | 2 | Peckish |
| 340–519 | 3 | Hungry |
| 520–769 | 4 | Famished |
| 770–1000 | 5 | Starving |

**Exhaustion**

| Raw | Stage | Name |
|---|---|---|
| 0–149 | 0 | Refreshed |
| 150–299 | 1 | Slightly Tired |
| 300–449 | 2 | Tired |
| 450–599 | 3 | Weary |
| 600–1000 | 4 | Debilitated |

The fatigue scale also defines negative stages for the well-rested bonuses
(`FATIGUE_RESTED = -2`, `FATIGUE_WELL_RESTED` and `FATIGUE_LOVERS_COMFORT = -3`).
The CC conversion above never produces them — they can only arrive from an SMI
stage global.

Helper predicates on the state (`IsStarving()` ≥ 4, `IsFreezing()` ≥ 4,
`IsExhausted()` ≥ 3) are written to work on both scales.

### Log output

From `CacheSurvivalGlobals()`, once per session, at `info`:

```
[StateManager] Survival globals cache: Cold=found, Hunger=found, Exhaustion=found, Enabled=found
[StateManager] SMI detected: HungerStage=found, ColdStage=found, ExhaustionStage=found
[StateManager] Native warmth function cached
```

Without SMI, the second line is `[StateManager] SMI not installed`. Without CC
Survival Mode at all, every field on the first line reads `null`. If the warmth
relocation fails to resolve, the third line is
`[StateManager] Native warmth function not available`.

Per-poll survival changes log at `trace` and only in Debug builds, so in practice
nothing recurring appears.

### Unsupported survival mods

| Mod | Why |
|---|---|
| Frostfall | Its own globals; `CacheSurvivalGlobals()` does not look them up |
| Campfire | Frostfall's companion; same |
| Sunhelm | Its own implementation |
| iNeed | Different tracking system |
| Anything else | Unless it writes the CC Survival globals listed above |

With none of them detected, all lookups return `nullptr`, need levels stay at 0,
`survivalModeActive` stays `false`, and nothing warns. Survival contexts simply
never fire.

The Part 1 vitals formula is independent of this: an unsupported mod that happens
to write `kVariable02/03/04` still gets correct base-max recovery, and one that
applies penalties as damage modifiers still gets a correct effective max.

### Adding another survival mod

There is no configuration path — it requires a code change:

1. Find the mod's global variable FormIDs and plugin name.
2. Add cached `RE::TESGlobal*` members to `StateManager.h`.
3. Look them up in `CacheSurvivalGlobals()`.
4. Read them in `PollPlayerSurvival()`, mapping to the 0–5 stage scale.

---

## Testing status

The SMI path is the one that gets play-tested, because Survival Mode Improved is
part of the LoreRim modlist Huginn is developed against. The **CC-only threshold
fallback has never been exercised in play** — it is verified against UESP and by
reading the code. Same for the no-survival-mod case: correct by construction, not
by observation. See [lorerim.md](lorerim.md) and the roadmap's vanilla-build
integration pass.

---

## References

- [Survival Mode Improved SKSE](https://github.com/colinswrath/Survival-Mode-Improved-SKSE) — source of the FormIDs and the Variable-AV convention
- [CommonLibSSE-NG `ActorValueOwner`](https://ng.commonlib.dev/_actor_value_owner_8h_source.html)
- [UESP: Survival Mode](https://en.uesp.net/wiki/Skyrim:Survival_Mode), [Cold](https://en.uesp.net/wiki/Skyrim:Cold), [Hunger](https://en.uesp.net/wiki/Skyrim:Hunger)
- [mod-compatibility.md](mod-compatibility.md) — how survival food and warming items get classified
- [../architecture/1-states.md](../architecture/1-states.md) — where survival state sits in the state model
