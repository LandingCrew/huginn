# Unknown spells — captured log

**This file is a data capture, not documentation.** It is the `LogAllSpells()`
dump of the `Unknown` bucket from one Debug session on the LoreRim modlist, dated
**2026-02-07** — roughly v0.7.x. The plugin is at v0.19.10 and this capture has
not been retaken.

It is kept because it is the only recorded sample of what a real modded spell
stack leaves unclassified. The analysis derived from it lives in
[unknown-spell-patterns.md](unknown-spell-patterns.md); read that first. This
file is the raw evidence behind it.

---

## Reading it

Each line is one `SpellData::ToString()`. Every entry here has `type=Unknown`,
meaning both `DetermineSpellType()` (engine API) and `DeriveSpellTypeFromTags()`
(name tags) declined to label it. `school` and `element` are often still
populated — those come from separate API reads and are unaffected by the type
failing.

`Unknown` does not mean filtered out. See
[mod-compatibility.md § What `Unknown` actually costs you](mod-compatibility.md#what-unknown-actually-costs-you).

## Format drift since the capture

`SpellData::ToString()` gained a `tagsExt={:04X}` field in #79, between the
`tags` and `cost` fields. Lines captured today have one more column than the ones
below. Nothing else about the format changed.

## What would classify differently now

Not re-measured — inferred from the current classifier, and each depends on the
engine-API path still declining, as it did in this capture:

- **`Featherfall`** — `DetermineSpellTagsExt` matches the whole word
  `featherfall`, sets `SpellTagExt::SlowFall`, and the derivation returns
  `Buff`.
- **`Knock`** — has no name match (the ext name tests are `unlock` and
  `open lock`, not `knock`), but if the effect uses the `kOpen` archetype it now
  picks up `SpellTagExt::Unlock` and derives to `Utility`. Archetype-dependent.
- Any waterbreathing spell in a stack like this would now be tagged
  `Waterbreathing` and typed `Buff` rather than being mislabelled `Stealth`.
  There is no such spell in this particular capture.

Everything else in the dump — reanimation, soul trap, dispel, detection,
transmute variants, non-hostile Illusion buffs, scripted mod spells — still
lands in `Unknown`. The categories are tabulated in
[unknown-spell-patterns.md § Still unclassified](unknown-spell-patterns.md#still-unclassified).

## Retaking the capture

`LogAllSpells()` is called from `Main.cpp:367`, which is inside an
`#ifndef NDEBUG` block on the load-game path. So: a **Debug** build, and a
**load game** rather than a new game. A Release build produces nothing, and there
is no console command that triggers it.

---

## The capture

```text
[2026-02-07 16:25:06.089][SpellRegistry.cpp:426 ][I]: --- Unknown (68 spells) ---
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E0159D8, name='Ruin', type=Unknown, school=Restoration, element=Magic, tags=00000000, cost=250, concentration=false, range=10000, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=9202852F, name='Distraction', type=Unknown, school=Illusion, element=None, tags=00000000, cost=50, concentration=false, range=10000, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=92027FA2, name='Featherfall', type=Unknown, school=Alteration, element=None, tags=00000000, cost=150, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E111CFB, name='Circle of the Moons', type=Unknown, school=Restoration, element=None, tags=00000080, cost=200, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E0803F2, name='Perilous Path', type=Unknown, school=Alteration, element=None, tags=00000000, cost=200, concentration=false, range=1024, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=99005C61, name='Infection', type=Unknown, school=Restoration, element=Poison, tags=00000000, cost=90, concentration=false, range=10000, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E02CD7E, name='Atronach Mark', type=Unknown, school=Conjuration, element=None, tags=00000000, cost=200, concentration=false, range=4000, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=99005E9C, name='Blend', type=Unknown, school=Illusion, element=None, tags=00000000, cost=150, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=FE075821, name='Blinding Spores', type=Unknown, school=Restoration, element=Poison, tags=00000000, cost=200, concentration=false, range=10000, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=9900080D, name='Featherwalking', type=Unknown, school=Alteration, element=None, tags=00000100, cost=20, concentration=true, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=924068C9, name='Dampening Rune', type=Unknown, school=Illusion, element=None, tags=00000080, cost=200, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E00B646, name='Drop Zone', type=Unknown, school=Alteration, element=None, tags=00000000, cost=100, concentration=false, range=4096, fav=true]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=92129548, name='Dispel Soul Gems', type=Unknown, school=Restoration, element=None, tags=00000000, cost=150, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E036A05, name='Circle of Strength', type=Unknown, school=Restoration, element=None, tags=00000080, cost=100, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=99005B43, name='Feather on Target', type=Unknown, school=Alteration, element=None, tags=00000000, cost=100, concentration=false, range=4096, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=99005B42, name='Feather on Self', type=Unknown, school=Alteration, element=None, tags=00000000, cost=100, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E00541E, name='Transmutate Stride', type=Unknown, school=Alteration, element=None, tags=00000100, cost=10, concentration=true, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=FE07581F, name='Floral Blockade', type=Unknown, school=Restoration, element=Poison, tags=00000100, cost=200, concentration=true, range=1500, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E022DAB, name='Inferno', type=Unknown, school=Destruction, element=Fire, tags=00000000, cost=130, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=FE6EA813, name='Circle of Entropy', type=Unknown, school=Destruction, element=None, tags=00000080, cost=50, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E036A03, name='Lamb of Mara', type=Unknown, school=Restoration, element=None, tags=00000000, cost=150, concentration=false, range=4000, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E13E4FD, name='Leech Seed', type=Unknown, school=Restoration, element=Poison, tags=00000000, cost=150, concentration=false, range=6144, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E0378A9, name='Mind Vision', type=Unknown, school=Illusion, element=None, tags=00000000, cost=100, concentration=false, range=4096, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E0E8B77, name='Mystic Wind', type=Unknown, school=Restoration, element=None, tags=00000000, cost=120, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E034D12, name='Necroplague', type=Unknown, school=Restoration, element=None, tags=00000000, cost=150, concentration=false, range=2000, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E006FA6, name='Ocato's Recital', type=Unknown, school=Alteration, element=None, tags=00000000, cost=200, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.089][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E01857E, name='Power of the Master', type=Unknown, school=Conjuration, element=None, tags=00000000, cost=320, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=99005CC2, name='Radiant Hazard', type=Unknown, school=Restoration, element=Magic, tags=00000000, cost=135, concentration=false, range=768, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E003E0F, name='Slay Living', type=Unknown, school=Restoration, element=None, tags=00000000, cost=100, concentration=false, range=4096, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=00065BD7, name='Reanimate Corpse', type=Unknown, school=Conjuration, element=None, tags=00000000, cost=400, concentration=false, range=4000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E007FFD, name='Transmutate Adventure Gear', type=Unknown, school=Alteration, element=None, tags=00000000, cost=150, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E00CC14, name='Raise Wall', type=Unknown, school=Alteration, element=None, tags=00000180, cost=50, concentration=true, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=990060D6, name='Recuperating Hands', type=Unknown, school=Restoration, element=None, tags=00000100, cost=72, concentration=true, range=10000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=990060D5, name='Recuperate', type=Unknown, school=Restoration, element=None, tags=00000100, cost=72, concentration=true, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=FE037874, name='Soul Split', type=Unknown, school=Conjuration, element=None, tags=00000000, cost=200, concentration=false, range=4096, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=0004DBA4, name='Soul Trap', type=Unknown, school=Conjuration, element=None, tags=00000000, cost=100, concentration=false, range=10000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=9900083A, name='Stone Shot', type=Unknown, school=Alteration, element=None, tags=00000000, cost=90, concentration=false, range=10000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=FE07581D, name='Strider's Shroud', type=Unknown, school=Restoration, element=Poison, tags=00000000, cost=200, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=FE4EE803, name='Warmth', type=Unknown, school=Restoration, element=None, tags=00000000, cost=100, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=0004B146, name='Turn Lesser Undead', type=Unknown, school=Restoration, element=None, tags=00000000, cost=100, concentration=false, range=10000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E007A94, name='Transmutate Fins', type=Unknown, school=Alteration, element=None, tags=00000000, cost=200, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=99000804, name='Transmute Night Eye', type=Unknown, school=Alteration, element=None, tags=00000000, cost=100, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E12649C, name='Wither', type=Unknown, school=Alteration, element=None, tags=00000000, cost=150, concentration=false, range=5000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E0012DC, name='Nature's Balance', type=Unknown, school=Restoration, element=None, tags=00000000, cost=800, concentration=false, range=4000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E00BB22, name='Thoughtsteal', type=Unknown, school=Illusion, element=None, tags=00000000, cost=200, concentration=false, range=10000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=99005982, name='Charming Aura', type=Unknown, school=Illusion, element=None, tags=00000000, cost=100, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E00D6F5, name='Transmutate Strength', type=Unknown, school=Alteration, element=None, tags=00000100, cost=80, concentration=true, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=00021143, name='Clairvoyance', type=Unknown, school=Illusion, element=None, tags=00000100, cost=25, concentration=true, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E016F92, name='Consuming Power', type=Unknown, school=Conjuration, element=None, tags=00000000, cost=100, concentration=false, range=4000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=FE06D826, name='Consecrate Dead', type=Unknown, school=Restoration, element=None, tags=00000000, cost=10, concentration=false, range=2750, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=99005C76, name='Radiant Touch', type=Unknown, school=Restoration, element=Magic, tags=00000040, cost=40, concentration=false, range=10000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=9900622D, name='Knock', type=Unknown, school=Alteration, element=None, tags=00000000, cost=40, concentration=false, range=4096, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=0007E8E1, name='Reanimate Zombie', type=Unknown, school=Conjuration, element=None, tags=00000000, cost=200, concentration=false, range=4000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E08558A, name='Pale Shadow', type=Unknown, school=Illusion, element=None, tags=00000000, cost=200, concentration=false, range=10000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E03527C, name='Holy Spirit', type=Unknown, school=Restoration, element=None, tags=00000000, cost=100, concentration=false, range=4000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=92028532, name='Darkvision', type=Unknown, school=Illusion, element=None, tags=00000000, cost=50, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E00BB25, name='Dispel Magic', type=Unknown, school=Illusion, element=None, tags=00000000, cost=100, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=0004DEE8, name='Courage', type=Unknown, school=Illusion, element=None, tags=00000000, cost=50, concentration=false, range=10000, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E01FCCA, name='Sealed Resolve', type=Unknown, school=Restoration, element=None, tags=00000000, cost=250, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6F22411C, name='Weaken', type=Unknown, school=Illusion, element=None, tags=00000000, cost=100, concentration=false, range=4096, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=6F224119, name='Frailty', type=Unknown, school=Illusion, element=None, tags=00000000, cost=150, concentration=false, range=4096, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=FE009851, name='Orum's Aquatic Escape', type=Unknown, school=Illusion, element=None, tags=00000000, cost=200, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=99005D0C, name='Dispel on Ally', type=Unknown, school=Restoration, element=None, tags=00000000, cost=400, concentration=false, range=4096, fav=false]
[2026-02-07 16:25:06.090][SpellRegistry.cpp:428 ][D]:   SpellData[id=9202851A, name='Dispel on Self', type=Unknown, school=Restoration, element=None, tags=00000000, cost=400, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.091][SpellRegistry.cpp:428 ][D]:   SpellData[id=99005B33, name='Pack Mule on Self', type=Unknown, school=Alteration, element=None, tags=00000000, cost=150, concentration=false, range=0, fav=false]
[2026-02-07 16:25:06.091][SpellRegistry.cpp:428 ][D]:   SpellData[id=9900086C, name='Pack Mule on Target', type=Unknown, school=Alteration, element=None, tags=00000000, cost=150, concentration=false, range=4096, fav=false]
[2026-02-07 16:25:06.091][SpellRegistry.cpp:428 ][D]:   SpellData[id=6F1E7427, name='Aura of Might', type=Unknown, school=Restoration, element=None, tags=00000100, cost=80, concentration=true, range=0, fav=false]
[2026-02-07 16:25:06.091][SpellRegistry.cpp:428 ][D]:   SpellData[id=6E00AB69, name='Detonate Lock', type=Unknown, school=Alteration, element=None, tags=00000000, cost=800, concentration=false, range=500, fav=false]
```

---

## See also

- [unknown-spell-patterns.md](unknown-spell-patterns.md) — the analysis of this capture
- [mod-compatibility.md](mod-compatibility.md) — overrides, and what `Unknown` costs
- [../architecture/2-classifiers.md](../architecture/2-classifiers.md) — classifier architecture
