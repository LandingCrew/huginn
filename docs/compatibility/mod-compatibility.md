# Mod Compatibility

**Verified against `src/` at v0.19.10.**

This file was written around v0.6–v0.7 and described an extension system that was
never built. It has been rewritten to describe what the plugin actually does. If
you are looking for the JSON plugin format, the per-mod `[ModName_Overrides]` INI
sections, or the Requiem/LoreRim weight profiles that used to be documented here,
none of them exist in the code and none of them ever did — see
[What does not exist](#what-does-not-exist).

---

## The short version

Huginn contains **no per-mod code**. There is no `LookupModByName` call anywhere
in `src/`, so nothing branches on Requiem, LoreRim, Apocalypse, Frostfall or
anything else. Compatibility comes from three places instead:

| Mechanism | Where |
|---|---|
| Engine-API classification, with name-based fallbacks | `src/spell/`, `src/learning/item/`, `src/scroll/`, `src/weapon/` |
| One user override file | `Data/SKSE/Plugins/Huginn_Overrides.ini` |
| Two hardcoded survival-mod integrations | `src/state/StateManager_Survival.cpp` (CC Survival Mode + SMI) |

Everything else is generic. A modded spell is treated exactly like a vanilla one.

---

## How a modded thing gets classified

All four classifiers use the same shape: **engine API first, tags second,
user override on top of both.**

### Spells (`src/spell/SpellClassifier.cpp`)

`ClassifySpell()` runs in this order:

1. **Override lookup** — by FormID first, then by exact name.
2. **Tags** — `DetermineSpellTags()` matches ~50 substrings against the
   lowercased spell name (`fire`, `frost`, `ward`, `conjure`, `muffle`, …).
   Overridable.
3. **Extended tags** — `DetermineSpellTagsExt()` walks *every* effect (not just
   the costliest) looking for the `kOpen` and `kEtherealize` archetypes and for
   a non-hostile value modifier on `kWaterBreathing`, plus whole-word name
   matches for `dragonrend`/`dragonbane`/`slowfall`/`unlock`/`open lock`.
   **Not overridable from the INI.**
4. **Type** — `DetermineSpellType()` reads the costliest effect's archetype,
   hostility flag, school and primary actor value. If that returns `Unknown`,
   `DeriveSpellTypeFromTags()` falls back to the tags from step 2/3.
5. **School** — API only (`GetMagickSkill()` on the costliest effect). No name
   fallback, so a modded spell in a custom school reads as `Unknown`.
6. **Element** — API `resistVariable` first, then derived from tags.

The practical consequence: a modded spell with a **vanilla-shaped effect record**
classifies correctly regardless of which mod added it, and a modded spell with an
unusual name and a scripted or custom effect lands in `SpellType::Unknown`.

### What `Unknown` actually costs you

An `Unknown` spell is **not filtered out**. It stays in the candidate pool and is
still eligible for every slot classified `SpellsAny` (`SlotClassifier.cpp:143`
returns `true` for all spells) and for the per-school slots if its school
resolved. What it loses is context relevance: `WeightForCandidate()`
(`src/context/ContextWeightForCandidate.cpp`) starts every spell at
`max(spellWeight, baseRelevanceWeight)` and then raises that only for tags and
types it recognises. A spell with no tags and no type never rises above the
baseline, so in practice it surfaces only on a permissive slot, as a wildcard
exploration pick, or once the contextual bandit has learned you use it.

So: an unclassified spell is *quiet*, not *invisible*. See
[unknown-spell-patterns.md](unknown-spell-patterns.md).

### Scrolls (`src/scroll/ScrollClassifier.cpp`)

`ScrollItem` derives from `SpellItem`, so the scroll classifier delegates
straight to `SpellClassifier::ClassifySpell()` and copies the result across,
then overwrites magnitude/duration from the scroll's own costliest effect.
Anything true of spell classification is true of scrolls.

Note that scrolls have a **separate, open problem** that is not a compatibility
issue: they sit in the pool every tick but score near zero against trained items,
so a scroll can rarely surface before it has been used (roadmap, "Scroll
cold-start").

### Potions, food and ingredients (`src/learning/item/ItemClassifier.cpp`)

Same shape — override, then `PopulateItemTags()`, then `DetermineItemType()`
with `DeriveItemTypeFromTags()` as fallback, then an alcohol sub-classification
pass on anything typed `Food`.

Two places explicitly accommodate mods:

- **Alcohol** is detected by keyword first — `VendorItemAlcohol`,
  `CACO_IsAlcohol`, `VendorItemSkooma` — before falling back to name matching
  (`ale`, `mead`, `wine`, `skooma`, `brandy`, `mazte`, `flin`, …).
- **Survival food** is detected by the CC Survival keywords
  `Survival_FoodRestoreHunger[Small|Medium|Large]` and
  `Survival_FoodWarm[Small|Medium|Large]`.

A mod that ships those keywords works with no configuration. A mod that ships its
own equivalents needs an override entry per item.

### Weapons (`src/weapon/WeaponClassifier.cpp`)

`DetermineWeaponType()` switches on `GetWeaponType()` — pure engine data, so any
weapon mod that registers a normal weapon type classifies correctly. Anything
outside the nine known types logs a warning and becomes `WeaponType::Unknown`.

**Weapons and ammo have no override file.** `LoadOverrides` exists only on
`SpellRegistry` and `ItemRegistry`.

### Apparel

Not a candidate source at all. `SourceType` (`src/candidate/CandidateTypes.h`)
covers Spell, Potion, Scroll, Weapon, Ammo, SoulGem, Food and Staff — there is
no armor entry, so fortify gear can never be recommended no matter how it is
classified. This is roadmap item #65 and it is open.

---

## The override file

**One file, one location:** `Data/SKSE/Plugins/Huginn_Overrides.ini`. Both
`SpellRegistry` and `ItemRegistry` load that same path
(`SpellRegistry.cpp:14`, `ItemRegistry.cpp:13`). There is no `overrides/`
directory, no per-mod file, and no load-order priority between files. A template
with worked examples ships at `configs/Huginn_Overrides.ini`.

### Syntax

Each **section** is one spell or item, named either by its exact display name or
by an 8-digit hex FormID:

```ini
[Blinding Spores]
type = Debuff
tags = Poison, Ranged

[0x0004DBA4]
type = Utility
```

Both keys are optional. A section with only `type` keeps auto-detected tags; a
section with only `tags` keeps the auto-detected type — but note that supplying
`tags` replaces the whole auto-detected tag set rather than adding to it.

FormID matching wins over name matching. Name matching is **exact and
case-sensitive** — `HasOverride(data.name)` is a hash-map lookup, not a substring
test, despite what the old version of this file claimed.

Hot-reload: `hg rebuild` re-reads the file (`SpellRegistry.cpp:77`), and loading
is idempotent — the spell side clears previous entries first.

### Spell `type` values

`Unknown`, `Healing`, `Damage`, `Defensive`, `Utility`, `Summon`, `Buff`,
`Debuff`. That is the complete `SpellType` enum; there is no `DamageFire`,
`SummonCreature` or `Cloak`.

### Spell `tags` values

Parsed case-insensitively by `SpellOverrides::ParseSingleTag`. An unrecognised
tag logs a warning and is skipped.

| | |
|---|---|
| Damage | `Fire`, `Frost`, `Shock`, `Poison`, `Sun` |
| Range/area | `Ranged`, `Melee` (or `Touch`), `AOE`, `Concentration` |
| Special | `AntiUndead`, `AntiDaedra`, `Stealth`, `Conjuration` |
| Restoration | `RestoreHealth` (or `Restoration`), `RestoreMagicka`, `RestoreStamina`, `Ward`, `TurnUndead` |
| Alteration | `Armor` (or `Alteration`, `Defensive`), `DetectLife`, `Light`, `Telekinesis`, `Paralysis` |
| Illusion | `Calm` (or `Illusion`, `Charm`), `Fear`, `Frenzy`, `Invisibility`, `Muffle` |
| Aliases | `Destruction` and `Offensive` both map to `Fire` |

The four extended tags — `Unlock`, `SlowFall`, `AntiDragon`, `Waterbreathing` —
are deliberately **not** settable from the INI. If auto-detection misses one, set
`type` instead and accept the loss of the matching context weight.

### Item `type` values

`Unknown`, `HealthPotion` (`Health`), `MagickaPotion` (`Magicka`),
`StaminaPotion` (`Stamina`), `ResistPotion` (`Resist`), `BuffPotion`
(`Buff`/`Fortify`), `CurePotion` (`Cure`), `Poison`, `Food`, `Alcohol`,
`Ingredient`.

`ItemOverrides::ParseItemType` does **not** accept `SoulGem` or `Scroll`, even
though the shipped template's comment block lists `SoulGem` and one example uses
`Scroll`. Those entries log "Unknown item type" and are ignored — the template is
wrong, not the code.

### Item `tags` values

`RestoreHealth`, `RestoreMagicka`, `RestoreStamina`; `ResistFire`, `ResistFrost`,
`ResistShock`, `ResistMagic`, `ResistPoison`, `ResistDisease`; `FortifyHealth`,
`FortifyMagicka`, `FortifyStamina`, `FortifyMagicSchool`, `FortifyCombatSkill`,
`FortifyUtilitySkill`, `FortifyCarryWeight`; `RegenHealth`, `RegenMagicka`,
`RegenStamina`; `CureDisease`, `CurePoison`; `SatisfiesHunger` (`Hunger`),
`SatisfiesCold` (`Warm`/`Warming`); `DamageHealth`, `DamageMagicka`,
`DamageStamina`, `Paralyze`, `Slow`, `Frenzy`, `Fear`, `Invisibility`,
`Waterbreathing`.

Two legacy names warn rather than work: `FortifySkill` is deprecated and silently
becomes `FortifyCombatSkill`; `Lingering` moved to an extended tag that the INI
cannot reach.

---

## Enemy and creature mods

Target classification is **race EditorID substring matching**, not keywords.
`StateEvaluator::ClassifyActor()` (`src/state/StateEvaluator.cpp:133`) reads
`GetRace()->GetFormEditorID()` and tests it case-insensitively in this order:

| `TargetType` | Matches |
|---|---|
| `Dragon` | contains `dragon`, **or** the race carries `RACE_DATA::Flag::kFlies` |
| `Undead` | `draugr`, `skeleton`, `vampire`, `ghost`, `zombie` |
| `Daedra` | `atronach`, `dremora`, `daedra`, `scamp`, `daedroth`, `seeker`, `lurker` |
| `Construct` | `dwarven`, `dwemer`, `sphere`, `centurion`, `ballista` |
| `Beast` | `wolf`, `bear`, `saber`/`sabre`, `spider`, `troll`, `mammoth`, `skeever`, `horker`, `mudcrab`, `slaughterfish` |
| `Humanoid` | everything else, including a null race or null EditorID |

There are no keyword checks and no `ActorTypeDragon` FormID constants — the old
version of this file invented both.

What this means for creature mods:

- A creature whose race EditorID happens to contain one of those substrings
  classifies correctly with no patch. A Mihail wolf variant reads as `Beast`.
- A creature whose race EditorID does not — most Mihail additions, e.g. a sea
  giant, minotaur or wraith race — falls through to `Humanoid`. There is no way
  to fix this from configuration; the race table is compiled in.
- Two false positives fall out of the substring rule: any flying race is typed
  `Dragon` (a modded cliff racer would be), and a dragon-priest race is typed
  `Dragon` rather than `Undead` because the `dragon` test runs first.

`TargetType` feeds `antiUndeadWeight`/`antiDaedraWeight`/`antiDragonWeight` in
`ContextRuleEngine` and one feature in the bandit's state vector
(`StateFeatures.h`), so a misclassification costs relevance on a niche context —
it does not break anything.

---

## Perk and magic overhauls (Requiem, Ordinator, Adamant, Vokrii)

Nothing detects these. What actually adapts:

- **Affordability uses perk-aware cost.** `CandidateGenerator` calls
  `spellItem->CalculateMagickaCost(playerRef)` for every spell every gather pass
  (`CandidateGenerator.cpp:158`) and caches it as `effectiveCost`. Whatever the
  overhaul does to cost through perks and enchantments is reflected there.
- **The registry's `baseCost` is not perk-aware.** `SpellClassifier::GetBaseCost`
  calls `CalculateMagickaCost(nullptr)`. It is display/priors data, not the
  filter input.
- **The uncastable-spell policy is configurable** — `[Candidates]` in
  `Huginn.ini`, values `Disallow` (drop spells you cannot afford), `Penalize`
  (keep, scaled down by the magicka shortfall) or `Allow`.

What does **not** adapt: there is no model of combat magicka regeneration, no
"magicka is precious" multiplier, and no earlier melee fallback threshold. The
weight tables in the old version of this file — "+4.0 vanilla / +8.0 LoreRim" and
so on — described nothing that was ever implemented. Real context weights are
normalized to `[0,1]` and configured in `[ContextWeights]`; see
[../architecture/5-slots.md](../architecture/5-slots.md) and the INI comments.

<!-- UNVERIFIED: Requiem's combat magicka-regen behaviour, its skill-scaled spell
     costs, and Ordinator's Vancian Magic are claims about those mods' internals.
     They are plausible and widely reported but nothing in this repository can
     confirm them, and no Huginn behaviour depends on them being true. -->

---

## Combat mods (MCO, Precision, poise mods, dodge mods)

No interaction. Combat state is `IsInCombat()` and vitals are read through
`ActorValueOwner`; neither is something a combat animation or poise mod replaces.
Nothing in `src/` reads stagger state, dodge i-frames or attack commitment.

---

## Framework mods

### SPID — Spell Perk Item Distributor

Spells added after load are picked up by reconciliation, not by a one-shot scan.
`SPELL_RECONCILE_INTERVAL_MS = 5000` (`src/Config.h:18`), so a distributed spell
enters the registry within five seconds. `ScanPlayerSpells` reads both the actor
base spell list and `addedSpells`, deduplicating by FormID, which is the case
SPID and perk-granted spells actually hit.

Items and weapons reconcile more slowly — `ITEM_RECONCILE_INTERVAL_MS` and
`WEAPON_RECONCILE_INTERVAL_MS` are both 30000.

### KID — Keyword Item Distributor

Keywords are read at classification time and the result is **cached in the
registry**. If KID adds a keyword after a form has been classified, the cached
classification is stale until the registry is rebuilt. `hg rebuild` forces that.
In practice KID runs at data load, before the registries are built on
`kPostLoadGame`, so this is a theoretical rather than an observed problem.

### FLM, SkyPatcher, OAR, PapyrusUtil, Object Categorization Framework

No interaction — Huginn reads final form data, so whatever these produce is what
gets classified.

---

## UI mods

### Wheeler

Huginn drives Wheeler through the exported `GetWheelerAPI` entry point
(`src/wheeler/WheelerConnection.cpp`), creating its own **managed** wheels, one
per configured page. The API headers Huginn mirrors are C0kAdam's
(`src/wheeler/WheelerAPI.h`).

Supported API versions are **1 through 4** (`API_VERSION_MIN`/`MAX`). What each
level adds:

| API version | Adds | Degradation below it |
|---|---|---|
| v1 | Managed wheels, all three callbacks | — |
| v2 | `SetManagedWheelEntrySubtext` | No subtexts: no wildcard, override, lock-timer or explanation labels under entries |
| v3 | `DeleteManagedWheelsForClient` | Wheels are deleted one at a time by index |
| v4 | `GetManagedWheelsForClient`; Wheeler no longer drops client wheels on save load | Huginn re-finds its wheels by scanning instead of asking |

A version below 1 is rejected outright. A version **above 4** is accepted with a
warning on the assumption that the interface was only appended to — if a future
Wheeler reorders `IWheelerAPI`, that warning is the only breadcrumb before a
crash.

`ItemActivatedCallback` is the learning feedback path.
`WheelerClient::OnItemActivated` sets `m_itemActivatedWhileOpen` (which
suppresses the skip penalty), applies the post-activation policy, and publishes
to the equip bus, where the `FeatureQLearner` subscriber applies the reward. It
is registered on **every** supported API version, not just v2+ — the old claim
that v1 "may not fire" it is not something this repository can confirm, and it is
not reflected in the version gate.

<!-- UNVERIFIED: which Wheeler builds actually export which API version, and
     whether any shipped Wheeler build fails to invoke ItemActivatedCallback.
     That is Wheeler-side behaviour; the header here declares the callback from
     v1 onward. -->

Known Wheeler-adjacent issue, open on the roadmap: with `bAutoFocusOnOpen = true`
(the default), a non-Huginn wheel sitting at position 0 is skipped on every open.
0.19.3 added a one-shot warning when a redirect actually skips a wheel; the
behaviour itself is unchanged. Set `bAutoFocusOnOpen = false` if it bites.

### TrueHUD, casting-bar mods, other HUD mods

Separate draw layers; no code interaction. The Scaleform widget's position, alpha
and scale are configurable under `[Widget]` if something overlaps.

<!-- UNVERIFIED: that TrueHUD's boss bars and target info do not visually collide
     with the Intuition widget at default positions. Nobody has checked this
     against a current build. -->

---

## Survival mods

Two are integrated, by hardcoded FormID lookup:

| Mod | Support |
|---|---|
| CC Survival Mode (`ccqdrsse001-survivalmode.esl`) | Raw 0–1000 need globals, converted to stages by compiled-in thresholds |
| Survival Mode Improved SKSE (`SurvivalModeImproved.esp`) | Pre-computed stage globals plus per-need enable flags; takes precedence when present |

Everything else — Frostfall, Sunhelm, iNeed, Campfire — is unsupported, because
`CacheSurvivalGlobals()` looks up only those two plugins. When neither is
installed every lookup returns `nullptr`, all three need levels stay neutral,
`survivalModeActive` stays `false`, and nothing warns.

Full detail, including the exact FormIDs and thresholds:
[Survival-Mode-Improved-SKSE.md](Survival-Mode-Improved-SKSE.md).

---

## What does not exist

Listed explicitly because the previous version of this document specified all of
it in detail, and a reader who saw that version will otherwise go looking.

| Documented in the old file | Reality |
|---|---|
| `Data/SKSE/Plugins/Huginn/plugins/*.Huginn.json` community plugins | No JSON parsing anywhere in `src/` |
| `required_esp` conditional plugin loading | No `LookupModByName` call in `src/` |
| Per-mod override files and a three-tier priority chain | One file: `Huginn_Overrides.ini` |
| `[Apocalypse_Overrides]` with `"Name"=type:X,tags:Y` lines | Section-per-spell, `type =` / `tags =` keys |
| Effect types `DamageFire`, `SummonCreature`, `Cloak`, `ArmorBuff`, `ResistFire`… | Eight `SpellType` values only |
| `IsRequiemLoaded()` / `IsLorerimLoaded()` and per-modlist weight profiles | No mod detection, no per-mod weights |
| `HasKeywordID(ActorTypeDragon)` keyword-based enemy classification | Race EditorID substrings |
| `actor_types.races` race-to-type mapping | Compiled-in substring table, not configurable |
| Feature flags, `context.survival` need bucket configuration | Compiled-in thresholds |

---

## Testing status

Huginn is developed and play-tested against a Requiem-based modlist (LoreRim).
**Everything in this document that concerns vanilla behaviour is verified by
reading the code and by unit test, not by play.** There is an open roadmap item
for a vanilla-build integration pass whose honest scope is "boot a vanilla
profile once and walk the contexts".

Two known consequences:

- The **workstation context** ranks Fortify Smithing/Enchanting potions.
  Requiem-based lists strip those effects from alchemy, so the context has
  nothing to rank and is inert on exactly the modlists that get play-tested
  (roadmap #63). Test 6h stands in for it.
- Three of the four extended spell tags — `Unlock`, `SlowFall`, `AntiDragon` —
  cover spells no LoreRim character can carry. Only `Waterbreathing` lights up on
  an unmodded game, and none of the four has been exercised in play.

Treat vanilla-only claims here as code-verified rather than play-verified.

---

## Troubleshooting

**A spell never appears.**
Check, in order: it is in the player's spell list and player-castable
(`ScanPlayerSpells` filters to player-castable spell types); you can afford it,
or the `[Candidates]` uncastable policy is not `Disallow`; it is not already
equipped on a slot with `bSkipEquipped`; some configured slot's classification
accepts it. `hg recs 20` dumps the scored breakdown to the log.

**A spell or potion is classified wrongly.**
Find its logged `SpellData[...]`/`ItemData[...]` line, then add a section to
`Huginn_Overrides.ini` keyed on the exact name or the FormID, and `hg rebuild`.

**Nothing in the log lists my spells.**
`LogAllSpells()` — the `--- Damage (N spells) ---` grouped dump — runs only in
**Debug builds**, only on a load-game, from `Main.cpp:367`. A Release build will
not produce it.

**A creature gets the wrong anti-X weighting.**
Its race EditorID does not contain a matching substring. Not fixable from
configuration.

Console commands: [../reference/ConsoleCommands.md](../reference/ConsoleCommands.md).

---

## See also

- [../architecture/2-classifiers.md](../architecture/2-classifiers.md) — classifier internals
- [../architecture/0-pipeline.md](../architecture/0-pipeline.md) — where classification sits in the update loop
- [../architecture/5-slots.md](../architecture/5-slots.md) — slot classifications and filtering
- [unknown-spell-patterns.md](unknown-spell-patterns.md) — what still lands in `Unknown`
- [lorerim.md](lorerim.md) — the reference play-test modlist
- [Survival-Mode-Improved-SKSE.md](Survival-Mode-Improved-SKSE.md) — survival integration
