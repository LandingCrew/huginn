# LoreRim

**Verified against `src/` at v0.19.10.**

LoreRim is the modlist Huginn is developed and play-tested against. That makes it
important to this project, but it is important as a **test environment**, not as
a supported target: there is no LoreRim-specific code, no LoreRim detection, and
no LoreRim configuration profile.

The previous version of this file specified all three, plus a weight-adjustment
table and a plugin JSON file. None of it was ever implemented — see
[mod-compatibility.md § What does not exist](mod-compatibility.md#what-does-not-exist).
This rewrite covers what is actually true: what LoreRim exercises, what it hides,
and where its shape has bent the project's testing coverage.

---

## There is no LoreRim integration

`grep -rn LookupModByName src/` returns one hit, and it is a comment in
`Tests.cpp`. Nothing in the plugin knows which modlist it is running under.
Concretely, none of the following exist:

- `IsRequiemLoaded()` / `IsLorerimLoaded()`
- A magicka-value multiplier that reacts to Requiem's combat regen behaviour
- An earlier melee-fallback threshold for "magicka won't come back"
- A per-modlist weight profile, or any weight that varies by installed mods
- A race-to-`TargetType` mapping table for Mihail creatures

Every context weight is normalized to `[0,1]` and read from `[ContextWeights]` in
`Huginn.ini`. The same values apply on every install.

---

## What LoreRim actually exercises

Because LoreRim is where the play-testing happens, these are the paths with real
in-game coverage:

| Area | Why LoreRim covers it |
|---|---|
| Large spell registries | The spell-mod stack puts 200+ spells in the registry, which is the load-time classification and reconciliation stress case |
| `SpellType::Unknown` handling | A modded spell stack is where unclassifiable spells actually occur — see [unknown-spell-patterns.md](unknown-spell-patterns.md) |
| Runtime spell distribution | SPID is present, so the 5-second reconcile path is exercised constantly rather than theoretically |
| Survival contexts | Survival Mode Improved is present, so the SMI stage-global path (not the CC-threshold fallback) is the one that gets play-tested |
| Weapon charge / soul gems | Enchanted-weapon play with filled gems is the live path for the soul-gem and weapon-charge weights |

---

## What LoreRim hides

This is the part worth reading. Being Requiem-based, LoreRim removes content that
vanilla ships, and Huginn has contexts that only fire on that content. Those
contexts have **never been exercised in play**.

### The workstation context is inert (roadmap #63)

The workstation context exists to rank Fortify Smithing / Fortify Enchanting
potions when the player stands at the matching bench. Requiem-based alchemy does
not produce those effects, so on LoreRim the context fires, labels itself, and
has nothing to rank.

<!-- UNVERIFIED: that Requiem specifically strips Fortify Smithing and Fortify
     Enchanting from its alchemy effect list. This is Requiem's content, not
     Huginn's, and cannot be checked from this repository. It is recorded as the
     working explanation for the observed emptiness on LoreRim. -->

The live targets on a Requiem list would be filled soul gems and fortify apparel
instead — and **apparel is not a candidate source at all** (`SourceType` has no
armor entry, roadmap #65), so that answer is unavailable. The vanilla path has
unit-test coverage (test 6h) and no play coverage.

### Three of the four extended spell tags are unreachable

`SpellTagExt` covers `Unlock`, `SlowFall`, `AntiDragon` and `Waterbreathing`.
Only `Waterbreathing` corresponds to a spell a LoreRim character routinely
carries. The other three cover spells the list either does not ship or does not
give the player, so `unlockWeight`, `slowFallWeight` and `antiDragonWeight` have
code-verified detection and no play coverage.

### The CC threshold fallback never runs

With SMI installed, `PollPlayerSurvival()` reads pre-computed stage globals and
never reaches the raw-value conversion branch. The CC-only thresholds documented
in [Survival-Mode-Improved-SKSE.md](Survival-Mode-Improved-SKSE.md) are therefore
verified against UESP and the code, not against play.

### The open item

The roadmap carries a **vanilla-build integration pass** whose honest scope is
"boot a vanilla profile once and walk the contexts". Until that happens, any
claim in these docs about vanilla behaviour should be read as *code-verified*,
not *play-verified*.

---

## Creature classification on LoreRim

LoreRim includes a large set of Mihail creature mods. Huginn classifies target
actors by **race EditorID substring**, not by keyword, and the substring table is
compiled in — see
[mod-compatibility.md § Enemy and creature mods](mod-compatibility.md#enemy-and-creature-mods)
for the full list.

The consequence for a list like this one: creatures whose race EditorID contains
a recognised substring (wolves, bears, trolls, spiders, mammoths, skeevers) type
correctly; the rest — sea giants, minotaurs, wraiths, guars, nix-hounds — fall
through to `Humanoid`. That costs relevance on the anti-undead and anti-daedra
contexts for those encounters. It cannot be fixed from configuration.

The old race-mapping JSON block in this file (`"MihailSeaGiantRace": "Beast"` and
so on) described a configuration format that does not exist, and asserted
classifications the code does not produce. It has been removed rather than
corrected, because there is nowhere to put it.

---

## Requiem's effect on scoring

Requiem changes spell costs. The one place that matters is affordability, and it
is handled generically: `CandidateGenerator` recomputes
`spellItem->CalculateMagickaCost(playerRef)` every gather pass, so whatever
Requiem's perks and skill scaling do to a spell's cost is what the affordability
filter sees. The `[Candidates]` uncastable-spell policy (`Disallow` / `Penalize`
/ `Allow`) is the knob for how a spell you cannot currently afford is treated.

There is no modelling of combat magicka regeneration and no "magicka is scarce"
weighting. If Requiem's economy makes the defaults feel wrong, the adjustment is
`[ContextWeights]`, applied globally.

---

## The modlist itself

A snapshot of LoreRim's Gameplay – Spells & Magic section is kept at
[lorerim-spelllist.md](lorerim-spelllist.md). It is a capture of the list at one
point in time, not a compatibility statement — none of those mods has a Huginn
patch, and none needs one, because classification is generic.

---

## See also

- [mod-compatibility.md](mod-compatibility.md) — how classification and overrides actually work
- [unknown-spell-patterns.md](unknown-spell-patterns.md) — spells this stack leaves unclassified
- [Survival-Mode-Improved-SKSE.md](Survival-Mode-Improved-SKSE.md) — the SMI path LoreRim uses
- [../architecture/2-classifiers.md](../architecture/2-classifiers.md) — classifier internals
- [../roadmap.md](../roadmap.md) — #63, #65 and the vanilla integration pass
