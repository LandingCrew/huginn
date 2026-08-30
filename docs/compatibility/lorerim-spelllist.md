# LoreRim spell mods — captured list

**This file is a data capture, not documentation.** It is a partial paste of the
"Gameplay – Spells & Magic" section of the LoreRim modlist, taken at an
unrecorded date, kept because the spell stack is what makes LoreRim useful as a
classification stress test.

Two things to know before using it:

- **It is incomplete.** The source header read *"Gameplay - Spells &
  Magic (46 items)"*; sixteen entries were pasted. The rest were never captured.
- **It is not a compatibility list.** None of these mods has a Huginn patch, and
  none needs one — spell classification is generic and knows nothing about which
  mod added a spell. See
  [mod-compatibility.md](mod-compatibility.md). Presence here means only "this
  was in the modlist", not "this is supported" or "this was tested".

<!-- UNVERIFIED: the composition of the LoreRim modlist, the Nexus IDs below, and
     whether any of these mods is still in the list at its current version.
     Nothing in this repository can check a third-party modlist's contents. -->

The source paste interleaved unlabeled four-digit numerals between entries. They
were not attributable to anything — not Nexus IDs, which are in the URLs — so
they have been dropped rather than guessed at.

---

## Captured entries

| Mod | Nexus |
|---|---|
| Use Telekinesis on Traps | [59350](https://www.nexusmods.com/skyrimspecialedition/mods/59350) |
| Runemaster Magic | [145420](https://www.nexusmods.com/skyrimspecialedition/mods/145420) |
| Wizarding Traversal Magic | [124125](https://www.nexusmods.com/skyrimspecialedition/mods/124125) |
| Holy Templar Magic (SkyPatched) | [113360](https://www.nexusmods.com/skyrimspecialedition/mods/113360) |
| Elemental Mastery Magic | [139953](https://www.nexusmods.com/skyrimspecialedition/mods/139953) |
| Wildwaker Magic | [123549](https://www.nexusmods.com/skyrimspecialedition/mods/123549) |
| Frostbitten Dreams Magic | [108653](https://www.nexusmods.com/skyrimspecialedition/mods/108653) |
| Ancient Blood Magic II | [115106](https://www.nexusmods.com/skyrimspecialedition/mods/115106) |
| Dark Hierophant Magic | [108499](https://www.nexusmods.com/skyrimspecialedition/mods/108499) |
| Obscure Magic | [103805](https://www.nexusmods.com/skyrimspecialedition/mods/103805) |
| Sonic Magic | [76360](https://www.nexusmods.com/skyrimspecialedition/mods/76360) |
| Constellation Magic | [92104](https://www.nexusmods.com/skyrimspecialedition/mods/92104) |
| Abyssal Tides Magic | [97892](https://www.nexusmods.com/skyrimspecialedition/mods/97892) |
| Apocalypse - Magic of Skyrim | [1090](https://www.nexusmods.com/skyrimspecialedition/mods/1090) |
| Triumvirate - Mage Archetypes | [39170](https://www.nexusmods.com/skyrimspecialedition/mods/39170) |
| Survival Spells | [43096](https://www.nexusmods.com/skyrimspecialedition/mods/43096) |

---

## Why this matters to Huginn

A stack this size is what puts 200+ spells in `SpellRegistry` at load, which is
the case that exercises classification throughput, reconciliation, and the
`Unknown` fallback path. The dump of what it leaves unclassified is
[unknown-spells.md](unknown-spells.md).

It also shapes what does *not* get tested. LoreRim's Requiem base removes content
that vanilla ships, so several contexts have never fired in play — see
[lorerim.md § What LoreRim hides](lorerim.md#what-lorerim-hides).

---

## See also

- [lorerim.md](lorerim.md) — what this modlist exercises and hides
- [mod-compatibility.md](mod-compatibility.md) — how modded spells get classified
- [unknown-spell-patterns.md](unknown-spell-patterns.md) — what falls through
