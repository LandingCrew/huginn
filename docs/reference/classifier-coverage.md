# Classifier Coverage by Load Order

How much of each load order Huginn's classifiers can type, measured from the
`hg dump` commands. Every count is the whole LOAD ORDER, not what a character
carries. Re-run the dumps and add a dated section when a load order or a
classifier changes enough to matter; keep the old sections, so drift shows.

## How to measure

Debug builds only. In the console, after a save loads:

```
hg dump spells     (spells, powers and scrolls -> Huginn_Spells.csv)
hg dump scrolls    (-> Huginn_Scrolls.csv)
hg dump potions    (potions and poisons -> Huginn_Potions.csv)
hg dump weapons    (weapons and ammo -> Huginn_Weapons.csv)
hg dump apparel    (enchanted armour -> Huginn_Apparel.csv)
hg dump food       (-> Huginn_Food.csv)
```

Files land in the SKSE log folder (`My Games\Skyrim.INI\SKSE` on the dev
machine) and are overwritten by the next run, so copy them out before
switching load orders.

## 2026-10-01 -- v0.22.8

| | LoreRim 5 | Simonrim Essentials | Vanilla (+ CC) |
|---|---|---|---|
| Spells / lesser powers / powers / scrolls | 4,652 / 290 / 82 / 1,200 | 1,248 / 43 / 26 / 285 | 682 / 21 / 49 / 101 |
| Unclassified (all script-only) | 976 | 148 | 50 |
| Of those, learnable from tomes | 105 of 1,107 | 13 of 317 | 0 of 115 |
| Weapons + ammo | 13,082 + 215 | 6,828 + 60 | 3,295 + 53 |
| Weapons unclassified | 19 | 18 | 18 |
| Enchanted apparel | 4,588 | 5,609 | 2,843 |
| Craft gear: Alchemy / Smithing | 191 / 2 | 6 (wrong) / 0 | 147 / 81 |
| Potions + poisons (unclassified) | 439 (34) | 335 (2) | 293 (2) |
| Food + drink (satisfy hunger / warm) | 177, 2026-09-29 | 235: 192 food, 43 alcohol (199 / 29) | not measured |

What the numbers mean:

- **Every unclassified spell and scroll, on all three, is script-only**: all
  of its effects are scripted (archetype and actor value -1), so the
  classifier has no game data to read. Everything with a readable effect is
  typed. Most are rightly unclassified (quest and FX spells); the
  player-facing ones are tracked on the roadmap under "Script-only powers
  and scrolls are unclassified".
- **Unclassified weapons** are the same everywhere: dart and gas traps, FX
  dummies, the Bloodskal firing weapon (plus LoreRim's Unarmed pseudo-item,
  which Huginn handles separately). None can be carried.
- **Unclassified potions** on vanilla and simonrim are two quest potions
  (Esbern's Potion, Philter of the Phantom). LoreRim's 34 were checked one by
  one: quest potions, camping kits, Vigilant's stat items, trait elixirs and
  Wintersun's Potion of Immolation, a harmful drink. All correctly untyped.
- **Craft gear** differs by load order, not by bug: vanilla has no Fortify
  Enchanting apparel at all, and LoreRim has only two Fortify Smithing
  pieces. On simonrim, Thaumaturgy turns "of the Alchemist" gear into Fortify
  Potion Duration (not a crafting buff) and removes Fortify Smithing; the 6
  it reads as Alchemy are its Fortify Poison Use rings, which reuse the
  vanilla Fortify Alchemy actor value -- a misread, on the roadmap.
- **Defensive spell elements**: every Defensive spell's element matches its
  resist actor value on all three (LoreRim 80, simonrim 27, vanilla 2).
- **Food on simonrim** is clean: all 29 warming items are the "Hot ..."
  survival soups, stews and pies, and the only food that does not satisfy
  hunger is Soul Husk (a Dawnguard soul-protection item) and Gourmet's two
  quest "Special" drinks. Not measured: food on vanilla; LoreRim's 177 is
  from 2026-09-29, before the survival tagging fixes.
