# Huginn Classification System (v0.19.x)

> See also:
> - [0-pipeline.md](0-pipeline.md) - Overall pipeline flow
> - [3-candidate-filtering.md](3-candidate-filtering.md) - What happens to a classified form next
> - [5-slots.md](5-slots.md) - Slot classification and allocation
> - [reviews/magic-classification.md](../reviews/magic-classification.md) - Magic classification review

## Overview

Classifiers classify a form when it is registered. Classification is cheap
(~0.01ms per form, per the design note in `WeaponRegistry::AddWeapon`), so
results are not cached — the classifier is re-run whenever a form is added to a
registry.

```
Form (SpellItem / AlchemyItem / TESSoulGem / ScrollItem / TESObjectWEAP / TESAmmo)
       │
       ▼
┌──────────────┐
│  Classify    │  ← INI overrides checked first (spells + alchemy items only)
│  (in-place)  │
└──────┬───────┘
       │
       ▼
  ...Data struct stored in the registry entry
```

| Registry | Classifier | Data struct | Source forms |
|---|---|---|---|
| `src/spell/SpellRegistry.h` | `SpellClassifier` | `SpellData` | Player-known `RE::SpellItem` |
| `src/learning/item/ItemRegistry.h` | `ItemClassifier` | `ItemData` | `RE::AlchemyItem`, `RE::TESSoulGem` |
| `src/scroll/ScrollRegistry.h` | `ScrollClassifier` | `ScrollData` | `RE::ScrollItem` |
| `src/weapon/WeaponRegistry.h` | `WeaponClassifier` | `WeaponData`, `AmmoData` | `RE::TESObjectWEAP`, `RE::TESAmmo` |

All four registries share the CRTP storage core in
`src/registry/FormRegistry.h` (vector of entries + FormID index +
`shared_mutex` + loading flag). `WeaponRegistry` keeps its own weapon+ammo
stores but uses the same lock-free query helpers.

---

## Spell Classification

### SpellType

**What it answers:** "What kind of spell is this?"

`src/spell/SpellData.h:6`

```cpp
enum class SpellType : uint8_t
{
    Unknown = 0,
    Healing,       // Restoration healing spells
    Damage,        // Destruction damage spells
    Defensive,     // Armor/ward spells
    Utility,       // Utility spells (detect, light, etc.)
    Summon,        // Conjuration summons
    Buff,          // Enhancement spells (Courage, Muffle, etc.)
    Debuff         // Weakening spells (Paralyze, Calm, etc.)
};
```

### SpellTag (Bitflags)

**What it answers:** "What properties does this spell have?"

`src/spell/SpellData.h:19` — **all 32 bits are used.** Anything new goes in
`SpellTagExt`.

```cpp
enum class SpellTag : uint32_t
{
    None = 0,

    // Damage types
    Fire          = 1 << 0,
    Frost         = 1 << 1,
    Shock         = 1 << 2,
    Poison        = 1 << 3,
    Sun           = 1 << 4,   // Anti-undead

    // Range/area
    Ranged        = 1 << 5,
    Melee         = 1 << 6,
    AOE           = 1 << 7,
    Concentration = 1 << 8,

    // Special properties
    AntiUndead    = 1 << 9,
    AntiDaedra    = 1 << 10,
    Stealth       = 1 << 11,  // Muffle, Invisibility
    Conjuration   = 1 << 12,

    // Restoration specific
    RestoreHealth  = 1 << 13,
    RestoreMagicka = 1 << 14,
    RestoreStamina = 1 << 15,
    Ward           = 1 << 16,
    TurnUndead     = 1 << 17,

    // Alteration specific
    Armor          = 1 << 18,
    DetectLife     = 1 << 19,
    Light          = 1 << 20,
    Telekinesis    = 1 << 21,
    Paralysis      = 1 << 22,

    // Illusion specific
    Calm           = 1 << 23,
    Fear           = 1 << 24,
    Frenzy         = 1 << 25,
    Invisibility   = 1 << 26,
    Muffle         = 1 << 27,

    // Conjuration specific
    SummonDaedra   = 1 << 28,
    SummonUndead   = 1 << 29,
    SummonCreature = 1 << 30,
    BoundWeapon    = 1u << 31
};
```

### SpellTagExt (Extended Bitflags)

**Added in #79.** `SpellTag` ran out of bits, and four context weights raised by
`ContextRuleEngine` (`unlockWeight`, `slowFallWeight`, `antiDragonWeight`,
`waterbreathingWeight`) had nothing to match against — the contexts fired,
named themselves on the widget, and moved no ranking.

`src/spell/SpellData.h:89`

```cpp
enum class SpellTagExt : uint16_t
{
    None = 0,

    Unlock         = 1 << 0,  // Open Lock / Knock           → unlockWeight
    SlowFall       = 1 << 1,  // Slow Fall / Become Ethereal → slowFallWeight
    AntiDragon     = 1 << 2,  // Dragonrend and friends      → antiDragonWeight
    Waterbreathing = 1 << 3   // → waterbreathingWeight
};
```

Tested with `HasTagExt()`, deliberately **not** an overload of `HasTag()` — the
two enums are distinct types, so an overload would compile but a reader could
not tell which set a call site meant.

Detection (`DetermineSpellTagsExt`, `src/spell/SpellClassifier.cpp:395`) walks
**every** effect rather than only the costliest one, because these are often the
cheap rider on a multi-effect spell:

| Tag | Detection |
|---|---|
| `Unlock` | `Archetype::kOpen`, plus whole-word name fallback on "unlock" / "open lock" (some mods drive the unlock through `kScript`) |
| `SlowFall` | `Archetype::kEtherealize`, plus whole-word "slowfall" / "slow fall" / "featherfall" / "feather fall" |
| `Waterbreathing` | Non-hostile effect with `primaryAV == kWaterBreathing`. Hostile effects excluded: the actor value says *which* stat is touched, not in which direction |
| `AntiDragon` | **Name only** — no archetype or actor value describes it. Whole-word "dragonrend" / "dragonbane"; a substring test would catch Dragonhide |

Extended tags are **not overridable from the INI** — the override file parses a
single `tags =` list against `SpellTag` names.

### MagicSchool & ElementType

> **Note:** `Item::ElementType` is a separate enum in `ItemData.h` with a
> different ordering and includes `Disease` instead of `Sun`. See
> [Item Classification](#item-classification) below.

`src/spell/SpellData.h:148` and `:160`

```cpp
// Spell::MagicSchool — ordered by gameplay frequency, not alphabetically
enum class MagicSchool : uint8_t
{
    Unknown = 0,
    Destruction,
    Restoration,
    Alteration,
    Illusion,
    Conjuration
};

// Spell::ElementType — no Disease (that's Item-only)
enum class ElementType : uint8_t
{
    None = 0,
    Fire,
    Frost,
    Shock,
    Poison,
    Sun,    // Anti-undead (Dawnguard)
    Magic   // Generic magic damage (no resist)
};
```

---

## Item Classification

### ItemType

**What it answers:** "What kind of item is this?"

`src/learning/item/ItemData.h:64`

```cpp
enum class ItemType : uint8_t
{
    Unknown = 0,
    HealthPotion,     // Restore health
    MagickaPotion,    // Restore magicka
    StaminaPotion,    // Restore stamina
    ResistPotion,     // Resist fire/frost/shock/poison/magic
    BuffPotion,       // Fortify attribute/skill
    CurePotion,       // Cure disease/poison
    Poison,           // Hostile (apply to weapon)
    Food,             // CC Survival Mode food
    Alcohol,          // Alcoholic beverages (ale, mead, wine, skooma)
    Ingredient,       // Raw alchemy ingredient
    SoulGem           // Soul gem for weapon recharge
};
```

`Alcohol` is a **sub-classification of `Food`**: `ClassifyItem` types an item as
`Food` first, then re-types it to `Alcohol` if `IsAlcohol()` matches
(`src/learning/item/ItemClassifier.cpp:854`). Detection is two-tier —
keywords (`VendorItemAlcohol`, `CACO_IsAlcohol`, `VendorItemSkooma`), then a
name list of specific drinks, then generic terms ("ale", "mead", "wine",
"beer", "brandy") matched at a **word boundary** so "Scale Armor" and
"Wineberry" do not become drinks.

> **Note:** Scrolls are NOT part of `ItemType`. They have their own
> `ScrollClassifier` using `ScrollType` (a type alias to `Spell::SpellType`).
> See [Scroll Classification](#scroll-classification) below.

### Item::ElementType

**Different from `Spell::ElementType`** — includes `Disease`, excludes `Sun`,
different ordering for `Magic`/`Poison`:

`src/learning/item/ItemData.h:49`

```cpp
// Item::ElementType — for resist/weakness potion effects
enum class ElementType : uint8_t
{
    None = 0,
    Fire,
    Frost,
    Shock,
    Magic,         // Generic magic resistance/weakness
    Poison,
    Disease
};
```

### ItemTag (Bitflags)

**What it answers:** "What effects does this item have?"

`src/learning/item/ItemData.h:87` — also full at 32 bits.

```cpp
enum class ItemTag : uint32_t
{
    None = 0,

    // Restoration (bits 0-2)
    RestoreHealth       = 1 << 0,
    RestoreMagicka      = 1 << 1,
    RestoreStamina      = 1 << 2,

    // Resistances (bits 3-8)
    ResistFire          = 1 << 3,
    ResistFrost         = 1 << 4,
    ResistShock         = 1 << 5,
    ResistMagic         = 1 << 6,
    ResistPoison        = 1 << 7,
    ResistDisease       = 1 << 8,

    // Fortify Attributes (bits 9-11)
    FortifyHealth       = 1 << 9,
    FortifyMagicka      = 1 << 10,
    FortifyStamina      = 1 << 11,

    // Fortify Skills — Grouped (bits 12-14)
    // Check corresponding enum field for which skill/school
    FortifyMagicSchool  = 1 << 12,  // Check 'school' field (Destruction, etc.)
    FortifyCombatSkill  = 1 << 13,  // Check 'combatSkill' field (OneHanded, etc.)
    FortifyUtilitySkill = 1 << 14,  // Check 'utilitySkill' field (Sneak, etc.)

    // Regeneration (bits 15-17)
    RegenHealth         = 1 << 15,
    RegenMagicka        = 1 << 16,
    RegenStamina        = 1 << 17,

    // Cures (bits 18-19)
    CureDisease         = 1 << 18,
    CurePoison          = 1 << 19,

    // Survival Mode (bits 20-21)
    SatisfiesHunger     = 1 << 20,
    SatisfiesCold       = 1 << 21,  // Warming effect (soups, stews)

    // Utility Buffs (bits 22-24)
    Invisibility        = 1 << 22,
    Waterbreathing      = 1 << 23,
    FortifyCarryWeight  = 1 << 24,  // When overencumbered

    // Poison Effects — Damage (bits 25-27)
    DamageHealth        = 1 << 25,
    DamageMagicka       = 1 << 26,
    DamageStamina       = 1 << 27,

    // Poison Effects — Control (bits 28-31)
    Paralyze            = 1 << 28,
    Slow                = 1 << 29,
    Fear                = 1 << 30,
    Frenzy              = 1u << 31
};
```

### ItemTagExt (Extended Bitflags)

**What it answers:** "What less-common effects does this item have?"

Overflow tags that didn't fit in the 32-bit primary `ItemTag`. Used for poison
modifiers, ravage effects, weakness elements, and soul gem capacities.

`src/learning/item/ItemData.h:179`

```cpp
enum class ItemTagExt : uint16_t
{
    None = 0,

    // Poison modifiers
    Lingering           = 1 << 0,   // Damage over time modifier

    // Weakness effects (check 'element' field for which)
    WeaknessElement     = 1 << 1,   // Weakness to Fire/Frost/Shock/Magic/Poison

    // Ravage effects (instant damage to MAX attribute)
    RavageHealth        = 1 << 2,
    RavageMagicka       = 1 << 3,
    RavageStamina       = 1 << 4,

    // Damage regen effects
    DamageHealthRegen   = 1 << 5,
    DamageMagickaRegen  = 1 << 6,
    DamageStaminaRegen  = 1 << 7,

    // Soul gem capacities
    SoulGemPetty        = 1 << 8,
    SoulGemLesser       = 1 << 9,
    SoulGemCommon       = 1 << 10,
    SoulGemGreater      = 1 << 11,
    SoulGemGrand        = 1 << 12,
    SoulGemBlack        = 1 << 13
};
```

`ItemData::HasHarmfulSideEffects()` reads the ravage / damage-regen / weakness
bits alongside `isHostile` and the primary damage tags — Skooma carries
`DamageHealth` beside `RestoreStamina` without the hostile flag, and emergency
overrides only surface pure potions.

> **Not currently set by the classifier:** the `Ravage*` and `Damage*Regen`
> bits are declared and *read* (by `HasHarmfulSideEffects`), but
> `PopulateItemTags` never sets them.
> <!-- UNVERIFIED: no writer for ItemTagExt::Ravage*/Damage*Regen found anywhere
>      in src/. Either dead bits or a genuine detection gap — reported, not fixed. -->

### Item Grouped Tag Fields

`ItemData` uses grouped tags with companion enum fields to avoid combinatorial
explosion. `DetermineFortifySkillType()`
(`src/learning/item/ItemClassifier.cpp:317`) maps an `RE::ActorValue` onto the
right pair, and accepts the LORERIM `*PowerModifier` actor-value variants
alongside the vanilla ones.

```cpp
// When FortifyMagicSchool is set, check which school:
Item::MagicSchool school;      // Alteration, Conjuration, Destruction, Illusion, Restoration, Enchanting

// When FortifyCombatSkill is set, check which skill:
Item::CombatSkill combatSkill; // OneHanded, TwoHanded, Marksman, Block, HeavyArmor, LightArmor, Smithing

// When FortifyUtilitySkill is set, check which skill:
Item::UtilitySkill utilitySkill; // Sneak, Lockpicking, Pickpocket, Speech, Alchemy

// When resist/weakness tags are set, check which element:
Item::ElementType element;     // Fire, Frost, Shock, Magic, Poison, Disease
```

Two mappings are lossy on purpose: `kUnarmedDamage` (LORERIM Fortify Unarmed)
maps to `CombatSkill::OneHanded`, and `kSpeedMult` sets `FortifyUtilitySkill`
with `utilitySkill` left `None` — the tag exists only to force `BuffPotion`
classification.

### Soul Gems

Soul gems reach `ItemData` by two different paths:

| Path | Function | `magnitude` means | Capacity bits |
|---|---|---|---|
| Real `RE::TESSoulGem` | `ClassifySoulGem` (`ItemClassifier.cpp:85`) | `GetContainedSoul()` — the soul **held** (`SOUL_LEVEL`, 0–5) | From `GetMaximumCapacity()`; `CanHoldNPCSoul()` adds `SoulGemBlack` |
| `RE::AlchemyItem` wearing soul-gem keywords (modded) | fallback block in `ClassifyItem` | Capacity, standing in for a soul that cannot be read here | From the `SoulGem*` keywords |

`magnitude` is the soul held, **not** the gem's size: capacity used to be the
magnitude, which ranked a Grand gem holding a petty soul above a Common gem
holding a common one. `SOUL_LEVEL` tops out at `kGrand = 5` — there is no 6, so
the keyword-fallback path clamps Black to 5 as well. Base-form souls only;
player-filled gems keep theirs in `ExtraSoul`, which `ItemRegistry`'s scan reads
and overrides (see [limitations/soul-gems.md](../limitations/soul-gems.md)).

`ItemData::filledCount` is how many instances in the stack hold a soul — **not**
the stack size. Ten petty gems with one soul among them is a registry count of
10 and a `filledCount` of 1, and only the 1 can recharge anything.

---

## API-Based Classification

Use CommonLibSSE APIs instead of name-matching.

### School Detection

`src/spell/SpellClassifier.cpp:575`

```cpp
MagicSchool SpellClassifier::DetermineMagicSchool(RE::Effect* costliestEffect) const {
    if (!costliestEffect || !costliestEffect->baseEffect) return MagicSchool::Unknown;

    switch (costliestEffect->baseEffect->GetMagickSkill()) {
        case RE::ActorValue::kDestruction:  return MagicSchool::Destruction;
        case RE::ActorValue::kRestoration:  return MagicSchool::Restoration;
        case RE::ActorValue::kAlteration:   return MagicSchool::Alteration;
        case RE::ActorValue::kIllusion:     return MagicSchool::Illusion;
        case RE::ActorValue::kConjuration:  return MagicSchool::Conjuration;
        default:                            return MagicSchool::Unknown;
    }
}
```

### Element Detection

`src/spell/SpellClassifier.cpp:595`

```cpp
ElementType SpellClassifier::DetermineElementType(RE::Effect* costliestEffect) const {
    if (!costliestEffect || !costliestEffect->baseEffect) return ElementType::None;

    switch (costliestEffect->baseEffect->data.resistVariable) {
        case RE::ActorValue::kResistFire:   return ElementType::Fire;
        case RE::ActorValue::kResistFrost:  return ElementType::Frost;
        case RE::ActorValue::kResistShock:  return ElementType::Shock;
        case RE::ActorValue::kPoisonResist: return ElementType::Poison;
        case RE::ActorValue::kResistMagic:  return ElementType::Magic;
        default:                            return ElementType::None;
    }
}
```

### SpellType Detection (School + Hostility Hybrid)

`DetermineSpellType` (`src/spell/SpellClassifier.cpp:84`) uses a school +
hostility hybrid rather than pure archetype matching. Archetype alone decides
only Summon, Buff and Utility. Tag-based fallback
(`DeriveSpellTypeFromTags()`) handles everything it returns `Unknown` for.

```cpp
// SpellClassifier private method — API-based only, no name fallback
SpellType SpellClassifier::DetermineSpellType(RE::SpellItem* spell, RE::EffectSetting* primaryEffect) const {
    // Pre-computed from primaryEffect: archetype, isHostile, school

    // Healing: non-hostile health restoration via kValueModifier or kPeakValueModifier
    if (!isHostile && primaryAV == kHealth && (arch == kValueModifier || arch == kPeakValueModifier))
        return SpellType::Healing;

    // Summon: kSummonCreature archetype only
    if (archetype == kSummonCreature) return SpellType::Summon;

    // Damage: hostile Destruction school
    if (isHostile && school == Destruction) return SpellType::Damage;

    // Defensive: non-hostile Alteration on kDamageResist
    if (school == Alteration && !isHostile && primaryAV == kDamageResist) return SpellType::Defensive;

    // Debuff: hostile Illusion school
    if (isHostile && school == Illusion) return SpellType::Debuff;

    // Buff: non-hostile self-targeted (Invisibility, Cloak archetypes)
    if (!isHostile && spell->GetDelivery() == kSelf && (arch == kInvisibility || arch == kCloak))
        return SpellType::Buff;

    // Utility: Light archetype
    if (archetype == kLight) return SpellType::Utility;

    return SpellType::Unknown;  // Falls back to DeriveSpellTypeFromTags()
}
```

### ItemType Detection

`DetermineItemType` (`src/learning/item/ItemClassifier.cpp:143`) is tiered —
soul gems are checked *before* the alchemy flags, and non-medicine forms bail
out to `Unknown` for the tag fallback to handle:

```cpp
ItemType ItemClassifier::DetermineItemType(RE::AlchemyItem* item) noexcept {
    // TIER 0: Soul gems (AlchemyItem forms wearing SoulGem* keywords)
    if (HasSoulGemKeyword(item)) return ItemType::SoulGem;

    // TIER 1: Built-in API flags
    if (item->IsPoison()) return ItemType::Poison;
    if (item->IsFood())   return ItemType::Food;   // may be re-typed to Alcohol by the caller
    if (!item->IsMedicine()) return ItemType::Unknown;  // ingredients etc. → tag fallback

    // TIER 2: Effect-based classification for medicines
    auto* effect = GetCostliestEffect(item);
    if (!effect || !effect->baseEffect) return ItemType::BuffPotion;

    auto arch      = effect->baseEffect->GetArchetype();
    auto primaryAV = effect->baseEffect->data.primaryAV;

    if (arch == kCureDisease || arch == kCurePoison) return ItemType::CurePotion;

    if (arch == kValueModifier && !effect->baseEffect->IsHostile()) {
        switch (primaryAV) {                       // Health/Magicka/Stamina → restore potions
        case kHealth:  return ItemType::HealthPotion;
        case kMagicka: return ItemType::MagickaPotion;
        case kStamina: return ItemType::StaminaPotion;
        default: break;
        }
        // resistVariable set (incl. kResistDisease) → ResistPotion
    }

    // kPeakValueModifier (fortify vitals) → BuffPotion
    // kDualValueModifier → restore potion if primaryAV is a vital (LORERIM), else BuffPotion
    // kInvisibility → BuffPotion
    return ItemType::BuffPotion;  // default
}
```

`ItemType::Ingredient` is never produced by auto-classification — the only
assignment anywhere in `src/` is `ItemOverrides::ParseItemType` (`type = Ingredient`
in the INI). Raw ingredients are also `RE::FormType::Ingredient`, which the item
registry's inventory filter does not accept (it takes `AlchemyItem` and
`SoulGem`), so in practice no ingredient reaches the registry at all. The
`Unknown` branch above is about non-medicine *alchemy* forms.

### Costliest Effect (Primary Effect)

**Two different formulas.** The spell/scroll side uses Skyrim's actual cost
formula with exponential magnitude scaling and an area factor; the item side
uses a simpler linear form with no area term.

`src/spell/SpellClassifier.cpp:531` — public, so `ScrollClassifier` can reuse it:

```cpp
RE::Effect* SpellClassifier::GetCostliestEffect(RE::SpellItem* spell) const {
    RE::Effect* costliest = nullptr;
    RE::Effect* firstValid = nullptr;
    float highestCost = -1.0f;

    for (auto* effect : spell->effects) {
        if (!effect || !effect->baseEffect) continue;
        if (!firstValid) firstValid = effect;

        // baseCost × magnitude^1.1 × durationFactor × areaFactor
        float magnitudeFactor = std::pow(std::max(1.0f, effect->effectItem.magnitude), 1.1f);
        float durationFactor  = (duration > 0) ? (duration / 10.0f) : 1.0f;
        float areaFactor      = (area > 0) ? (0.15f * area) : 1.0f;

        float cost = effect->baseEffect->data.baseCost * magnitudeFactor * durationFactor * areaFactor;
        if (cost > highestCost) { highestCost = cost; costliest = effect; }
    }
    return costliest ? costliest : firstValid;  // safety fallback
}
```

`src/learning/item/ItemClassifier.cpp:739`:

```cpp
// baseCost × max(1, magnitude) × durationFactor  — no exponent, no area factor,
// and NO first-valid fallback (returns nullptr when nothing scores above 0).
float cost = baseCost * std::max(1.0f, magnitude) * durationFactor;
```

---

## Scroll Classification

### Architecture

`ScrollClassifier` classifies `RE::ScrollItem*` by delegating to
`SpellClassifier`. This works because `ScrollItem` inherits from `SpellItem` in
Skyrim's type hierarchy — **the scroll pointer is passed straight to
`ClassifySpell()`**; there is no linked-spell lookup.

```
RE::ScrollItem* scroll
       │
       ▼
ScrollClassifier::ClassifyScroll()          (src/scroll/ScrollClassifier.cpp:7)
       │
       ├─ SpellClassifier::ClassifySpell(scroll) → SpellData
       │
       ├─ ConvertToScrollData() → ScrollData (type, tags, tagsExt, school, element, baseCost)
       │
       └─ Override magnitude/duration from the scroll's own costliest effect
```

`ClassifyScroll` has a documented contract: for non-null input the result
always carries the scroll's real `formID`. There is no rejection path (unlike
`WeaponClassifier`, which returns a `formID == 0` sentinel), because
`ScrollRegistry` stores results unchecked and re-keys its FormID index on
`data.formID` during swap-pop removal.

### ScrollData Type Aliases

`src/scroll/ScrollData.h:15`

```cpp
using ScrollType    = Spell::SpellType;     // Damage, Healing, Defensive, Utility, Summon, Buff, Debuff
using ScrollTag     = Spell::SpellTag;      // Fire, Frost, Shock, AOE, AntiUndead, etc.
using ScrollTagExt  = Spell::SpellTagExt;   // Unlock, SlowFall, AntiDragon, Waterbreathing (#79)
using MagicSchool   = Spell::MagicSchool;   // Destruction, Restoration, Alteration, Illusion, Conjuration
using ElementType   = Spell::ElementType;   // Fire, Frost, Shock, Poison, Sun, Magic
```

Scrolls share `SlotContentType::Spell` in the display layer
(`src/ui/SlotTypes.h:11`) — there is no separate `Scroll` slot content type.

---

## Name Matching

`SpellTag` + `DetermineSpellTags()` (`src/spell/SpellClassifier.cpp:238`) is
the single source of name-based detection for spells: the name is lowercased
once and every keyword test runs through a `contains()` lambda over that
buffer. Other functions derive their values from the tags instead of re-parsing
the name.

<!-- UNVERIFIED: the pre-v0.7.1 doc quoted "47 NameContains() calls across 4
     functions" (with a per-function table that summed to 74). Neither number is
     checkable against current source — the functions it counted no longer exist
     in that form. Historical claim retained without the table. -->

Where a fallback must read the name and a substring match would be unsafe, the
classifiers use `Util::NameContainsWord()` (`src/util/NameMatch.h`) — a
case-insensitive match that must *start* at a word boundary. This exists
because "Quicksilver" contains "silver": a loose match tagged Quicksilver
weapons `WeaponTag::Silver`, and once the anti-undead context began reading
that tag an observed Quicksilver Greatsword doubled its weight against draugr
(ctx 0.30 → 0.60, #81). The leading boundary only — "Silvered Sword" must still
match "silver".

```
┌─────────────────────────────────────────────────────────────────────┐
│                      ClassifySpell() Flow                           │
│                  (src/spell/SpellClassifier.cpp:11)                 │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  STEP 0: Override lookup — FormID first, then exact name            │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  m_overrides.GetOverride(formID) ?: GetOverride(name)         │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                              │                                      │
│                              ▼                                      │
│  STEP 1: Tags first (single source of name-matching)                │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  data.tags    = override->tags ?: DetermineSpellTags(spell)   │  │
│  │  data.tagsExt = DetermineSpellTagsExt(spell)  // never        │  │
│  │                                              // overridable   │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                              │                                      │
│                              ▼                                      │
│  STEP 2: Type detection                                             │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  data.type = override->type ?: DetermineSpellType(...)        │  │
│  │  if (data.type == Unknown)                                    │  │
│  │      data.type = DeriveSpellTypeFromTags(tags, tagsExt)       │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                              │                                      │
│                              ▼                                      │
│  STEP 3: School detection (API only, never overridable)             │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  data.school = DetermineMagicSchool(costliestEffect)          │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                              │                                      │
│                              ▼                                      │
│  STEP 4: Element detection + semantic overrides                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  data.element = DetermineElementType(costliestEffect)         │  │
│  │  if (None) data.element = DeriveElementFromTags(data.tags)    │  │
│  │  Magic + Sun tag → Sun;  type == Utility → None               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  Then: baseCost, isConcentration, range (reusing costliestEffect)   │
└─────────────────────────────────────────────────────────────────────┘
```

The costliest effect is computed **once** per spell and threaded through every
helper (v0.7.19: O(4n) → O(n) effect iterations per spell at load time).

### Helper Functions

`DeriveSpellTypeFromTags` (`src/spell/SpellClassifier.cpp:155`) takes **both**
tag sets. Priority order, most specific first:

| Order | Condition | Result |
|---|---|---|
| 1 | `RestoreHealth` | `Healing` |
| 2 | `BoundWeapon` / `SummonDaedra` / `SummonUndead` / `SummonCreature` | `Summon` |
| 3 | `Ward` / `Armor` | `Defensive` |
| 4 | `Paralysis` / `Calm` / `Fear` / `Frenzy` / `TurnUndead` / `AntiDaedra` | `Debuff` |
| 5 | `Invisibility` / `Muffle` / `Stealth` | `Buff` |
| 6 | ext `Unlock` | `Utility` |
| 7 | ext `AntiDragon` | `Debuff` |
| 8 | ext `SlowFall` / `Waterbreathing` | `Buff` |
| 9 | `DetectLife` / `Light` / `Telekinesis` | `Utility` |
| 10 | `Fire` / `Frost` / `Shock` / `Poison` / `Sun` (broadest, checked last) | `Damage` |

The extended tags sit **below** every primary-role check on purpose:
`DetermineSpellTagsExt` walks every effect, so an ext tag is often a rider (a
ward that also grants waterbreathing). The primary tags describe what a spell is
*for*; the ext tags describe something it also happens to do. A pure Open Lock
or Waterbreathing spell carries no primary tag at all and falls through to
rows 6–8, which is the case they exist to catch.

```cpp
// Derive ElementType from computed SpellTag bitflags (SpellClassifier.cpp:226)
static ElementType DeriveElementFromTags(SpellTag tags) noexcept
{
    if (HasTag(tags, SpellTag::Sun))    return ElementType::Sun;
    if (HasTag(tags, SpellTag::Fire))   return ElementType::Fire;
    if (HasTag(tags, SpellTag::Frost))  return ElementType::Frost;
    if (HasTag(tags, SpellTag::Shock))  return ElementType::Shock;
    if (HasTag(tags, SpellTag::Poison)) return ElementType::Poison;
    return ElementType::None;
}
```

### Benefits

| Benefit | Description |
|---------|-------------|
| **Consistency** | If a spell name matches "fire", it will always have `SpellTag::Fire` AND `ElementType::Fire` |
| **Maintainability** | Adding a new detection pattern only requires changes in `DetermineSpellTags()` |
| **Performance** | Name parsing happens once per spell, not multiple times |
| **Testability** | Tag derivation is pure functions with no external dependencies |

---

## Semantic Override Pattern

### Problem

API-based classification is reliable but not always semantically correct:

| Spell | API Result | Semantic Truth | Root Cause |
|-------|------------|----------------|------------|
| Sunbeam | `element=Magic` | `element=Sun` | Dawnguard sun spells use `kResistMagic` internally |
| Waterbreathing | `type=Unknown` | `type=Buff` | No archetype match (plain value modifier on `kWaterBreathing`) |
| Open Lock / Knock | `type=Unknown` | `type=Utility` | `kOpen` archetype is not in `DetermineSpellType`'s table |

### Solution: Post-API Semantic Overrides

Two element corrections applied after element detection
(`src/spell/SpellClassifier.cpp:67–75`):

```cpp
// Override 1: Sun damage overrides Magic
// Dawnguard spells use kResistMagic but are semantically Sun damage
if (data.element == ElementType::Magic && HasTag(data.tags, SpellTag::Sun)) {
    data.element = ElementType::Sun;
}

// Override 2: Utility spells shouldn't have elemental damage
// Some modded spells incorrectly use elemental resistVariable
if (data.type == SpellType::Utility && data.element != ElementType::None) {
    data.element = ElementType::None;
}
```

### Type corrections are now tag-based, not tag-*abuse*-based

> **Changed in #79 — the old approach is gone.** Before `SpellTagExt`, the
> classifier reached the right types by deliberately mislabelling: `"open"` was
> tagged `Telekinesis` and `"waterbreath"` was tagged `Stealth`, purely so
> `DeriveSpellTypeFromTags()` would return `Utility` and `Buff`. Neither line
> exists any more. The `Stealth` one was actively harmful:
> `WeightForCandidate` reads `Stealth` into `stealthWeight`, so every
> waterbreathing spell ranked as a sneaking tool. Both now carry a real
> `SpellTagExt` bit and derive their type from it.

### Design Principles

1. **API first, override second** — trust the API for most spells, only override known semantic mismatches
2. **Tag-driven overrides** — overrides are based on detected tags, not hardcoded FormIDs (mod-compatible)
3. **Type-driven element clearing** — Utility spells never deal elemental damage, so clear spurious elements
4. **Give a concept its own bit** — where a tag would have to be borrowed to reach the right type, add an ext tag instead

---

## INI Classification Overrides

Spells and alchemy items can have their classification forced from
`Data/SKSE/Plugins/Huginn_Overrides.ini` (template: `configs/Huginn_Overrides.ini`).

| | |
|---|---|
| **Applies to** | Spells (`SpellOverrides`) and alchemy items (`ItemOverrides`). Not scrolls, weapons or ammo |
| **Section key** | Item/spell name, or an 8-hex-digit FormID (`00012345`, or `0x00012345`). FormID is matched first |
| **Keys** | `type = <TypeName>`, `tags = Tag1,Tag2,...` |
| **Not overridable** | `SpellTagExt`, school, element, magnitude, duration, cost — all still computed from the API |

Two things to know:

- **Both parsers read the same file.** `SpellOverrides::LoadFromFile` and
  `ItemOverrides::LoadFromFile` each walk *every* section, so an item section
  also becomes a spell override (with unrecognised tag names parsing to `None`)
  and vice versa. There is no `[Spells]`/`[Items]` namespacing, so a spell and a
  potion sharing a display name share an override entry.
- **Reload is uneven.** `SpellRegistry` remembers the path and re-loads it at
  the top of every `RebuildRegistry()`, so `hg rebuild` / `hg reset all` pick up
  INI edits without a restart. `ItemRegistry` loads overrides **only in its
  constructor** — item classification edits need a game restart. `hg reload`
  does not touch either: its `[Overrides]` step reloads
  `Override::Settings` (the urgent-potion override system), not classification.

---

## Weapon Classification

### WeaponType

`src/weapon/WeaponData.h:12`

```cpp
enum class WeaponType : uint8_t
{
    Unknown = 0,

    // One-handed melee
    OneHandSword,
    OneHandAxe,
    OneHandMace,
    OneHandDagger,

    // Two-handed melee
    // (Skyrim has no distinct warhammer WEAPON_TYPE — warhammers report as
    //  kTwoHandAxe, so there is no TwoHandMace value to map them to.)
    TwoHandSword,
    TwoHandAxe,

    // Ranged
    Bow,
    Crossbow,

    // Special
    Staff
};
```

> **`TwoHandMace` no longer exists.** It was removed because `RE::WEAPON_TYPE`
> has no `kTwoHandMace`: warhammers report as `kTwoHandAxe`, and nothing ever
> assigned the value. `DetermineWeaponType`
> (`src/weapon/WeaponClassifier.cpp:92`) is a pure 1:1 switch over
> `weapon->GetWeaponType()` with a `warn` on the default arm — no keyword or
> name fallback.

### WeaponTag (Bitflags)

`src/weapon/WeaponData.h:56`

```cpp
enum class WeaponTag : uint32_t
{
    None = 0,

    // Combat style (bits 0-3)
    Melee          = 1 << 0,
    Ranged         = 1 << 1,
    OneHanded      = 1 << 2,
    TwoHanded      = 1 << 3,

    // Material properties (bits 4-6)
    Silver         = 1 << 4,   // Bonus vs undead/werewolves
    Daedric        = 1 << 5,   // High-tier material
    Bound          = 1 << 6,   // Conjured weapons

    // Enchantment presence (bit 7)
    Enchanted      = 1 << 7,

    // Enchantment elements (bits 8-10)
    EnchantFire         = 1 << 8,
    EnchantFrost        = 1 << 9,
    EnchantShock        = 1 << 10,

    // Special enchantment effects (bits 11-19)
    EnchantAbsorbHealth     = 1 << 11,
    EnchantAbsorbMagicka    = 1 << 12,
    EnchantAbsorbStamina    = 1 << 13,
    EnchantSoulTrap         = 1 << 14,
    EnchantParalyze         = 1 << 15,
    EnchantFear             = 1 << 16,
    EnchantTurnUndead       = 1 << 17,
    EnchantBanish           = 1 << 18,
    EnchantSilence          = 1 << 19,

    // Weapon state (bit 20)
    NeedsCharge    = 1 << 20,  // Enchanted weapon with low charge

    // Ammo-specific (bit 21)
    MagicAmmo      = 1 << 21   // Enchanted arrows/bolts
};
```

Tag assignment (`DetermineWeaponTags`, `src/weapon/WeaponClassifier.cpp:140`):

- Combat-style bits come from the `WeaponType`. **Staves are `Ranged | OneHanded`**; bows and crossbows are `Ranged | TwoHanded`.
- `Silver` / `Daedric` / `Bound` are keyword-first (`WeapMaterialSilver`, `WeapMaterialDaedric`, `WeapTypeBoundWeapon`) with a name fallback. Only the silver fallback is word-boundary matched (#81).
- Enchantment bits come from walking the enchantment's effects: element from `resistVariable`, the rest from archetype (`kAbsorb` + primaryAV, `kSoulTrap`, `kParalysis`, `kTurnUndead`, `kBanish`, and `kDemoralize`/`kFrenzy` → `EnchantFear`).
- `NeedsCharge` is **not** set by the classifier — it is maintained by `WeaponRegistry` as charge crosses `Config::WEAPON_CHARGE_LOW_THRESHOLD` (0.2).
- `EnchantSilence` is declared but never set. <!-- UNVERIFIED: no writer for WeaponTag::EnchantSilence anywhere in src/. -->

Staves are a special case in `ClassifyWeapon`: they are inherently enchanted but
do not use `formEnchanting`, so `hasEnchantment` is `(enchantment != nullptr) || type == Staff`.

### AmmoType

`src/weapon/WeaponData.h:42`

```cpp
enum class AmmoType : uint8_t
{
    Unknown = 0,
    Arrow,          // Standard arrow
    Bolt            // Crossbow bolt
};
```

Determined by `ammo->IsBolt()`, defaulting to `Arrow`. Ammo reuses `WeaponTag`
for its `Silver` and `MagicAmmo` bits plus whatever its enchantment contributes.

---

## Registries

### Rebuild and reconcile triggers

`RebuildRegistry()` clears the store and re-classifies everything. It runs in
exactly two situations (`src/Main.cpp:158–218`, `src/console/ConsoleCommands.cpp:94`):

| Trigger | Behaviour |
|---|---|
| `kNewGame` | All four registries rebuilt |
| `kPostLoadGame` (save load) | **Reconcile, not rebuild** — `ReconcileSpells` / `ReconcileItems` / `ReconcileWeapons` / `ReconcileScrolls`. Existing entries keep the classification they already have |
| `hg rebuild`, `hg reset all` | All four registries rebuilt |

So "changed classification logic takes effect on the next game load" is only
true for a **new game**; on a save load, use `hg rebuild`.

Steady-state cadence, driven by `UpdateLoop.cpp` against `src/Config.h`:

| Registry | Fast pass | Interval | Slow pass | Interval |
|---|---|---|---|---|
| Spell | `RefreshFavorites` | `SPELL_FAVORITES_REFRESH_INTERVAL_MS` (500ms) | `ReconcileSpells` | `SPELL_RECONCILE_INTERVAL_MS` (5s) |
| Item | `RefreshCounts` | `ITEM_COUNT_REFRESH_INTERVAL_MS` (500ms) | `ReconcileItems` | `ITEM_RECONCILE_INTERVAL_MS` (30s) |
| Scroll | `RefreshCounts` | `ITEM_COUNT_REFRESH_INTERVAL_MS` (500ms) | `ReconcileScrolls` | `ITEM_RECONCILE_INTERVAL_MS` (30s) |
| Weapon | `RefreshCharges` | `WEAPON_REFRESH_INTERVAL_MS` (500ms) | `ReconcileWeapons` | `WEAPON_RECONCILE_INTERVAL_MS` (30s) |

`SpellRegistry` additionally sinks `TESEquipEvent` for immediate equip/unequip
detection, and `AddNewSpell()` can add a single spell out of band.

Capacity limits: `MAX_TRACKED_ITEMS` (500, shared by item + scroll registries),
`MAX_TRACKED_WEAPONS` (100), `MAX_TRACKED_AMMO` (50). Exceeding a limit logs a
warning and skips the remainder.

### Scope

| Registry | What it tracks |
|---|---|
| Spell | Player-known spells from **two** sources — the actor base's spell list (racial powers, starting spells) and `GetActorRuntimeData().addedSpells` (tomes, quest rewards, console), deduplicated by FormID and filtered to `kSpell` / `kLeveledSpell` |
| Item | Every `AlchemyItem` and `TESSoulGem` in the player's inventory, via `Util::GetInventorySafe` |
| Scroll | Every `ScrollItem` in inventory, with counts |
| Weapon | **All** weapons and ammo in inventory, with `isFavorited` / `isEquipped` as metadata flags |

Inventory scans use `GetInventory()` (through `Util::GetInventorySafe`), not
`entryList` + `countDelta`: the latter only tracks *changes* and missed items in
the player's base container, such as starting gear.

### Favorites detection

Two different mechanisms, because Skyrim stores the two kinds of favorite
differently:

**Spells — `RE::MagicFavorites`** (`SpellRegistry::ScanSpellFavorites`,
`src/spell/SpellRegistry.cpp:448`). The `spells` array holds every favorited
spell; the `hotkeys` array is also scanned for hotkey-assigned ones.

**Weapons and inventory items — `InventoryEntryData::IsFavorited()`**
(`WeaponRegistry::ExtractWeaponMetadata`, `src/weapon/WeaponRegistry.cpp:679`).

```cpp
// FIX (v0.12.x): IsFavorited() catches both starred favorites AND hotkeyed items
if (includeExtraLists && entry) {
    sw.isFavorited = entry->IsFavorited();
}
```

> **Historical trap:** the earlier implementations here were both wrong.
> Checking `ExtraDataType::kHotkey` directly detects only items assigned to
> quickslots 1–8 and misses starred-but-not-hotkeyed favorites; and
> `MagicFavorites::hotkeys` is the wrong list for *inventory* items entirely.
> `IsFavorited()` covers both cases and is what the code uses now.

Favorite **discovery** for weapons is owned solely by `ReconcileWeapons` (30s).
`RefreshCharges` no longer walks the inventory, so it cannot see newly-starred
weapons.

### Charge tracking

Enchanted weapons need charge monitoring via `ExtraCharge`:

```cpp
// Current charge, per inventory instance
auto* extraCharge = extraList->GetByType<RE::ExtraCharge>();
if (extraCharge) { sw.currentCharge = extraCharge->charge; }

// Max charge from the enchantable base form
auto* enchantable = weapon->As<RE::TESEnchantableForm>();
if (enchantable) { sw.maxCharge = static_cast<float>(enchantable->amountofEnchantment); }
```

`WeaponData::currentCharge` stores the **normalised fraction**
(`currentCharge / maxCharge`), not the raw value. `previousCharge` holds the
previous fraction for delta detection.

`RefreshCharges` (`src/weapon/WeaponRegistry.cpp:109`) reads charge from **only
the equipped weapons** (at most two) via `GetEquippedEntryData` — enchantment
charge only drains on equipped weapons, and the old full-inventory walk was the
single largest Huginn CPU cost (~1.2ms/call at 2Hz). It also refreshes tracked
ammo counts from the inventory-changes `entryList`.

> **Do not call `RE::InventoryChanges::GetItemCount`.** It crashes on save-load
> (bisected in PR #41). Walk `invChanges->entryList` and accumulate `countDelta`
> instead.

Both `RefreshCharges` and any extraList read are gated on
`Util::IsExtraListStable()` — for `EXTRALIST_STABILIZATION_MS` (500ms) after a
load, extraList pointers may be stale and dereferencing them crashes.
`RebuildRegistry` deliberately skips extraLists entirely, and a post-load
reconcile that lands inside the window primes a short retry
(`WEAPON_RECONCILE_RETRY_MS`, 1s) rather than waiting the full 30s.

### Inventory Metadata Structures

```cpp
struct InventoryWeapon {          // src/weapon/WeaponData.h:248
    WeaponData data;              // Classification (type, tags, damage, charge fraction)
    bool isFavorited = false;
    bool isEquipped = false;
    float previousCharge = 0.0f;  // Charge fraction at last poll
};

struct InventoryAmmo {            // src/weapon/WeaponData.h:266
    AmmoData data;
    int32_t count = 0;
    bool isEquipped = false;      // Currently equipped as active ammo
};

struct InventoryScroll {          // src/scroll/ScrollData.h:69
    ScrollData data;
    int32_t count = 0;
    int32_t previousCount = 0;    // For consumption detection
};
```

`WeaponData::uniqueID` carries `ExtraUniqueID` so Wheeler can address a specific
inventory instance rather than the base form. It is 0 until extraLists
stabilise. `WeaponClassifier` returns a `formID == 0` sentinel for weapons with
neither a display name nor an editor ID; `WeaponRegistry` tombstones those in
`m_rejectedWeapons` so the failure logs once instead of every scan.

---

## Classification Data

Each registry stores classification results in its own data struct:

- `SpellData` (`src/spell/SpellData.h:214`) — spells
- `ItemData` (`src/learning/item/ItemData.h:334`) — potions, poisons, food, alcohol, ingredients, soul gems
- `ScrollData` (`src/scroll/ScrollData.h:21`) — scrolls (reuses `SpellType`/`SpellTag`/`SpellTagExt` via type aliases)
- `WeaponData` / `AmmoData` (`src/weapon/WeaponData.h:167`, `:211`) — weapons and ammo

These are populated directly by the classifier at registration time — no cache,
no external persistence layer. (The only persisted state is the contextual
bandit's weights; see [4-contextual-bandits.md](4-contextual-bandits.md).)

---

## How Classification Reaches the Slots

Classification fields are copied onto candidates by the factory functions in
`src/candidate/CandidateTypes.h` (`SpellCandidate::FromSpellData` and friends),
so `SpellCandidate` carries `type`, `tags`, `tagsExt`, `school`, `element`, and
`ItemCandidate` carries `type`, `tags`, `tagsExt`, `school`, `combatSkill`,
`utilitySkill`. `SlotClassifier` (`src/slot/SlotClassifier.cpp`) then decides
whether a candidate may occupy a slot with a given `SlotClassification`
(`src/slot/SlotConfig.h:21` — 21 values including the `_Count` sentinel, guarded
by a `static_assert` tripwire).

| Classification | Spell | Item | Scroll | Weapon / Ammo |
|---|---|---|---|---|
| `DamageAny` | `type == Damage` | `type == Poison` | `type == Damage` | any weapon |
| `HealingAny` | `Healing` or `RestoreHealth` tag | `HealthPotion` or `RestoreHealth` | `Healing` or `RestoreHealth` | — |
| `BuffsAny` | `Buff` or `Armor`/`Invisibility`/`Muffle` | `BuffPotion` or Fortify*/`Invisibility` | `Buff` or `Armor`/`Invisibility`/`Muffle` | — |
| `DefensiveAny` | `Defensive` or `Ward`/`Armor` | `ResistPotion` or any `Resist*` | `Defensive` or `Ward`/`Armor` | — |
| `SummonsAny` | `Summon` or any summon/`BoundWeapon` tag | — | `Summon` or summon tags | — |
| `Utility` | `Utility` or `Light`/`DetectLife`/`Telekinesis`/**ext `Unlock`** | `CurePotion` or `Waterbreathing`/`Cure*` | `Utility` or `Light`/`DetectLife` | — |
| `PotionsAny` | — | the seven potion/poison types (not food, alcohol, soul gems, ingredients) | — | — |
| `FoodAny` / `AlcoholAny` | — | `type == Food` / `type == Alcohol` | — | — |
| `ScrollsAny` | — | — | all | — |
| `SpellsAny` | all | — | — | — |
| `Spells<School>` | `school ==` that school | — | `school ==` that school (scrolls carry school too) | — |
| `WeaponsAny` / `WeaponsMelee` / `WeaponsRanged` | — | — | — | all / `Melee` tag / `Ranged` tag |
| `AmmoAny` | — | — | — | ammo only |
| `Regular` | all | all | all | all |

Two asymmetries worth knowing:

- **Scroll `Utility` does not check ext `Unlock`,** although `ScrollData` now carries `tagsExt`. A Scroll of Knock reaches a Utility slot only via `ScrollType::Utility`. <!-- UNVERIFIED: looks like an oversight rather than a decision — the spell arm was given the ext check in #79 and the scroll arm was not. -->
- **Scroll `SummonsAny` omits `BoundWeapon`,** which the spell arm includes.

`SlotClassifier::Classify()` also produces a single "best" classification for a
candidate (used for display and debugging) by walking a fixed priority list —
effect-based classifications first, then `FoodAny` before `AlcoholAny` before
`PotionsAny`, then weapons (melee/ranged before `WeaponsAny`), then the
per-school spell filters before `SpellsAny`.

See [5-slots.md](5-slots.md) for allocation, locking and the multi-page layout.

---

## Classification Priority

| Priority | Method | Reliability |
|----------|--------|-------------|
| 1 | Effect archetype (`GetArchetype()`) | Very High |
| 2 | `data.resistVariable` | Very High |
| 3 | `GetMagickSkill()` (spell school) / `data.primaryAV` | Very High |
| 4 | Item flags (`IsPoison`, `IsFood`, `IsMedicine`) | Very High |
| 5 | Keywords (`WeapMaterialSilver`, `SoulGem*`, `Survival_Food*`, `VendorItemAlcohol`) | High |
| 6 | Name matching — `contains()` or `NameContainsWord()` | Low (fallback) |
| 7 | INI override | Absolute, when present (checked first, spells and items only) |

---

## Future Work (Post v1.0)

Not in scope for current version:

- **Archetype classification** (Necromancer, Paladin, etc.) - For archetype-based slots
- **Mod keyword detection** (Triumvirate keywords) - For mod integration
- **User override files** (JSON) - For manual classification beyond the current INI

See [roadmap.md](../roadmap.md) backlog and [8-future-work.md](8-future-work.md).
