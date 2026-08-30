# Unknown spell patterns

**Verified against `src/` at v0.19.10.**

This file was a v0.7.9 planning document: a list of spell-name patterns that fell
through the classifier, with proposed tags and a testing checklist. Most of that
plan was never carried out in the form it described, and the parts that did land
took a different shape. It has been rewritten as a description of where the
classifier stands now and what is genuinely still unclassified.

For the classification pipeline itself see
[../architecture/2-classifiers.md](../architecture/2-classifiers.md); for what
`Unknown` costs a spell in practice see
[mod-compatibility.md § What `Unknown` actually costs you](mod-compatibility.md#what-unknown-actually-costs-you).

---

## What "Unknown" means

`SpellType::Unknown` is a spell the classifier could not label — not a spell it
rejected. An `Unknown` spell stays in the candidate pool, matches any slot
classified `SpellsAny`, matches the per-school slots if `GetMagickSkill()`
resolved, and can be picked as a wildcard. What it loses is the ability to be
*raised* by context: `WeightForCandidate()` gives it
`max(spellWeight, baseRelevanceWeight)` and nothing more, because every other arm
of that function keys off a tag or a type.

Two spells can be `Unknown` for very different reasons:

- **Typed but untagged** — a spell whose school resolved and whose name matched
  nothing. It can still fill school slots.
- **Fully opaque** — a scripted effect with no recognisable archetype, no
  resist variable, and an unmatched name.

Neither is a bug on its own. The classifier is deliberately conservative: a wrong
label is worse than no label, because a wrong label actively mis-ranks the spell.

---

## The two-stage fallback, as built

`ClassifySpell()` tries the engine API first and only then the tags:

```
DetermineSpellType(spell, costliestEffect)     // archetype / hostility / school / primary AV
    └─ if Unknown → DeriveSpellTypeFromTags(tags, tagsExt)
```

`DetermineSpellType` recognises: non-hostile health value/peak-value modifiers
(`Healing`), `kSummonCreature` (`Summon`), hostile Destruction (`Damage`),
non-hostile Alteration on `kDamageResist` (`Defensive`), hostile Illusion
(`Debuff`), self-delivered `kInvisibility`/`kCloak` (`Buff`), and `kLight`
(`Utility`). Everything else falls to the tag derivation.

`DeriveSpellTypeFromTags` checks, in order: `RestoreHealth` → Healing; bound
weapon or any summon tag → Summon; `Ward`/`Armor` → Defensive; paralysis, calm,
fear, frenzy, turn-undead or anti-daedra → Debuff; invisibility, muffle or
stealth → Buff; **then the extended tags**; then detect-life, light or telekinesis
→ Utility; then any damage element → Damage.

---

## What landed instead of the v0.7.9 plan

The plan proposed adding new `SpellTag` bits for Soul, Detection and Movement.
That could not happen: `SpellTag` is a `uint32_t` and **all 32 bits are taken**
(`SpellData.h` says so explicitly). What landed instead, in #79, is a second
enum:

```cpp
enum class SpellTagExt : uint16_t {
    Unlock, SlowFall, AntiDragon, Waterbreathing
};
```

Four bits, chosen because they were the four context weights
(`unlockWeight`, `slowFallWeight`, `antiDragonWeight`, `waterbreathingWeight`)
that `ContextRuleEngine` computed, `DominantReason` named on the widget, and no
candidate could ever match — the contexts fired, labelled themselves, and moved
no ranking.

Detection is API-first for three of the four (`DetermineSpellTagsExt`), walking
**every** effect rather than the costliest one, because these are typically the
cheap rider on a multi-effect spell:

| Ext tag | Detection | Derives to |
|---|---|---|
| `Unlock` | `kOpen` archetype, plus whole-word `unlock` / `open lock` | `Utility` |
| `SlowFall` | `kEtherealize` archetype, plus whole-word `slowfall` / `slow fall` / `featherfall` / `feather fall` | `Buff` |
| `Waterbreathing` | Non-hostile value modifier on `kWaterBreathing` | `Buff` |
| `AntiDragon` | Name only — whole-word `dragonrend` / `dragonbane` | `Debuff` |

Three details worth knowing:

- The ext checks sit **below** every primary-role check in
  `DeriveSpellTypeFromTags`, on purpose. Because detection scans all effects, an
  ext tag is often a rider — a ward that also grants waterbreathing. Typing that
  ward as a `Buff` would be a regression. A *pure* Open Lock or Waterbreathing
  spell carries no primary tag, so it falls through to the ext arm, which is the
  case they exist to catch.
- The waterbreathing check **excludes hostile effects**. The actor value says
  which stat is touched, not in which direction, so a curse that strips
  waterbreathing reads identically. Recommending the spell that drowns you is
  the one outcome worse than recommending nothing.
- Matching is **whole-word** (`Util::NameContainsWord`), not substring. Bare
  `dragon` would swallow Dragonhide, a vanilla self-armour spell; bare `slow` is
  a debuff.

Before #79 these types were reached by mislabelling — `open` was tagged
`Telekinesis` and `waterbreath` was tagged `Stealth`, purely so the derivation
would produce `Utility` and `Buff`. The `Stealth` one had a live cost: the spell
arm of `WeightForCandidate` reads `Stealth` into `stealthWeight`, so every
waterbreathing spell was ranked as a sneaking tool.

`SlotClassifier::MatchesSpell` also names `SpellTagExt::Unlock` explicitly on the
`Utility` classification rather than trusting the derivation, so an unlock spell
that the API happens to type as something else still reaches utility slots.

---

## What the name matcher covers today

`DetermineSpellTags()` lowercases the name once and runs ~50 substring tests.
Recognised, grouped:

| Group | Substrings |
|---|---|
| Fire | `fire`, `flame`, `incinerate`, `burn` |
| Frost | `frost`, `ice`, `freeze`, `blizzard` |
| Shock | `shock`, `lightning`, `thunder`, `spark` |
| Poison / Sun | `poison`; `sun`, `vampire's bane` (also sets AntiUndead) |
| Anti-undead | `turn undead`, `circle of protection`, `repel undead` |
| Anti-daedra | `banish`, `expel daedra` |
| Range / area | `bolt`, `ball`, `spear`, `blast`; `touch`, `grasp`; `ball`, `cloak`, `storm`, `rune`, `wall`, `circle` |
| Restoration | `heal`, `cure`, `restore health`; `restore magicka`; `restore stamina`; `ward` |
| Alteration | `armor`, `flesh`; `detect life`; `light`/`candlelight`/`magelight` (excluding `lightning`); `telekinesis`; `paralyze` |
| Illusion | `calm`, `pacify`; `fear`, `rout`; `frenzy`, `fury`, `mayhem`; `invisibility`; `muffle` |
| Conjuration | `summon`, `conjure` (then `atronach`/`dremora` → Daedra, `zombie`/`wraith`/`boneman`/`mistman` → Undead, else Creature); `bound` |

`Concentration` is set from the casting type, not the name.

---

## Still unclassified

These are the categories that reach `Unknown` today. None of them has a tag, and
none is scheduled — they are listed so nobody re-derives the list from a log.

| Category | Examples from the v0.7.x capture | Why it is hard |
|---|---|---|
| Reanimation | Reanimate Corpse, Reanimate Zombie | `kReanimate` is its own archetype, distinct from `kSummonCreature`; nothing maps it |
| Soul trap | Soul Trap, Soul Split | No tag bit for "soul", and no free bit in `SpellTag` |
| Detection | Clairvoyance, Mind Vision, Thoughtsteal, Darkvision | `DetectLife` covers only the literal `detect life` name |
| Movement / carry | Feather on Self, Pack Mule, Transmutate Stride | `SlowFall` covers falling, not carry weight or speed |
| Dispel | Dispel Magic, Dispel on Ally | No archetype match, no tag |
| Transmute variants | Transmutate Fins, Transmute Night Eye | Names vary per mod; no shared shape |
| Illusion buffs | Courage, Charming Aura | Non-hostile Illusion is not covered by the hostile-Illusion → Debuff rule |
| Scripted mod spells | Ocato's Recital, Perilous Path, Drop Zone | `kScript` describes nothing |

Adding any of these means either finding a free encoding (`SpellTagExt` has 12
unused bits) or accepting the `Unknown` baseline. The bar for adding one should
be that it unlocks a **context weight that already exists and is going unread** —
which is exactly the case #79 made for its four.

The user-facing answer in the meantime is an override entry; see
[mod-compatibility.md § The override file](mod-compatibility.md#the-override-file).
Note that the override INI cannot set extended tags, only `type` and the 32
primary tags.

---

## Testing status

The extended-tag detection is covered by unit tests. Three of the four —
`Unlock`, `SlowFall`, `AntiDragon` — cover spells no LoreRim character can carry,
and LoreRim is the only modlist Huginn is play-tested on, so those three have
**never been exercised in game**. Only `Waterbreathing` lights up on an unmodded
Skyrim. The roadmap's vanilla-build integration pass is the item that would
close this.

---

## See also

- [../architecture/2-classifiers.md](../architecture/2-classifiers.md) — classifier architecture
- [mod-compatibility.md](mod-compatibility.md) — overrides, and what `Unknown` costs
- [unknown-spells.md](unknown-spells.md) — the raw v0.7.x capture this analysis came from
- [../roadmap.md](../roadmap.md) — open items
- `src/spell/SpellClassifier.cpp`, `src/spell/SpellData.h` — the code
