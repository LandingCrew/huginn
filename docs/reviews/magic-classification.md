# Review: Data-Driven Spell Patterns

> **This is a dated review, not a specification.** It was written against
> v0.13.x/v0.14.x (measurement dated 2026-02-07) and proposes a design that
> **was never built**. Re-checked against `src/spell/` at v0.19.10 on
> 2026-08-29; the status notes below are that re-check. Nothing here describes
> current behaviour except where marked "Still true".
>
> The proposal is also **not tracked on [../roadmap.md](../roadmap.md)** — there
> is no spell-classification item there. If it is still wanted, it needs to be
> filed; this page is not a backlog.

---

## The finding (2026-02-07, still open)

A Lorerim stress test left 68 of 236 spells (29%) classified as `Unknown` —
6 vanilla and ~62 from Apocalypse, Mysticism/Odin and Triumvirate. The raw log
is [../compatibility/unknown-spells.md](../compatibility/unknown-spells.md) and
the pattern breakdown is
[../compatibility/unknown-spell-patterns.md](../compatibility/unknown-spell-patterns.md).

<!-- UNVERIFIED: the 68/236 figure is a single 2026-02-07 capture on one modlist.
It has not been re-measured at v0.19.x, and the classifier's fallback ordering
has changed since (see below), so the current unknown rate is not known. -->

**Still true:** the escape hatch for an unclassified spell is a hand-written
per-spell override. There is no substring/pattern facility anywhere in
`src/spell/`, so a mod author or list curator must enumerate every spell by name
or FormID.

## The proposal (never implemented)

Ship a second INI of *substring patterns* alongside the user override file, so
name rules become data rather than C++:

```
1. User overrides    (Huginn_Overrides.ini)      — exact name/FormID matches
2. Default patterns  (Huginn_SpellPatterns.ini)  — substring patterns  [NOT BUILT]
3. Hardcoded C++     (DetermineSpellTags)        — name-pattern keywords
4. API checks        (DetermineSpellType)        — SKSE archetype inspection
```

### Status at v0.19.10 — none of it landed

| Proposed change | Status |
|---|---|
| `SpellOverrides::m_patterns`, `MatchPattern()`, `HasPattern()` | **Not present.** `SpellOverrides` holds only `m_nameOverrides` and `m_formIDOverrides` |
| `LoadFromFile()` handling a `pattern=true` flag | **Not present.** It parses `[SpellName]` / `[FormID]` sections with `type =` and `tags =` only |
| `ClassifySpell()` pattern check between exact override and API | **Not present** |
| `SpellRegistry` loading two files | **Not done.** It loads exactly one path, `Data/SKSE/Plugins/Huginn_Overrides.ini` |
| `Huginn_SpellPatterns.ini` shipped | **Does not exist** in `configs/` |

The review's own "Completed: none yet" is therefore still accurate six months
later. Read the file table it contained as a sketch of where the work *would*
go, not as a record of work done.

### One thing worth noting if this is picked up

`configs/Huginn_Overrides.ini` is a **shared** file — `SpellRegistry` and
`ItemRegistry` both load that same path — but the shipped template documents
only item types (`HealthPotion`, `Food`, `SoulGem`, …). `SpellOverrides`
separately parses spell types (`Healing`, `Damage`, `Defensive`, `Utility`,
`Summon`, `Buff`, `Debuff`), and none of that vocabulary is documented in the
file users are told to copy. The "users can tweak without rebuilding" premise is
weaker in practice than the review assumed.

## What *did* change: the pipeline is no longer the one described

The review describes a 3-stage pipeline, "overrides → API checks → hardcoded
name-pattern tag fallback". That ordering was reworked. `ClassifySpell` now
computes tags **first** and derives from them
(`src/spell/SpellClassifier.cpp`):

1. **Overrides** — FormID first, then name.
2. **Tags** — `DetermineSpellTags()`, the single source of name matching.
   Overridable via `tags =`.
3. **Extended tags** — `DetermineSpellTagsExt()`. Deliberately **not**
   overridable from the INI (#79): the override file parses one `tags =` list
   against `SpellTag` names, and a second vocabulary for four tags nobody has
   asked to override was judged more surface than it earns.
4. **Type** — API (`DetermineSpellType`) first; on `Unknown`, fall back to
   `DeriveSpellTypeFromTags(tags, tagsExt)`.
5. **School** — API only, no name fallback.
6. **Element** — API (`resistVariable`) first, then `DeriveElementFromTags()`;
   then two corrections — `Magic` + `Sun` tag becomes `Sun` (Dawnguard), and a
   `Utility` spell is forced back to no element (some modded utility spells
   carry an elemental `resistVariable`).

Practically: name matching now happens **once**, and both the type and element
fallbacks read its result rather than re-scanning the name. A pattern layer
added today would slot into step 2, not into the "between override and API"
position the review names.

Name matching also has a safety rule it did not have in February.
`Util::NameContainsWord` (`src/util/NameMatch.h`) requires the match to start at
a word boundary, after a loose substring match tagged every Quicksilver weapon
as Silver and doubled its context weight against draugr (#81). Any pattern file
built on raw substrings would reintroduce exactly that class of bug, and should
route through the same helper.

---

## Build note (still true)

After setting or changing `VCPKG_ROOT`, `CommonLibSSEPath_NG` or
`CompiledPluginsPath`, restart VS Code. It inherits environment variables at
launch, so System Settings changes do not take effect until the editor restarts.
