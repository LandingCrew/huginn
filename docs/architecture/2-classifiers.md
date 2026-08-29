# Huginn Classification System (v0.12.x)

> See also:
> - [0-pipeline.md](0-pipeline.md) - Overall pipeline flow

## Overview

Classifiers classify a form when it is first registered. Classification is cheap (~0.01ms per form), so even 1000 forms take ~10ms — imperceptible during a loading screen.

```
FormID requested
       │
       ▼
┌──────────────┐
│  Classify    │
│  (in-place)  │
└──────┬───────┘
       │
       ▼
    Return
    result
```

---

## Spell Classification

### SpellType

**What it answers:** "What kind of spell is this?"

```cpp
enum class SpellType : uint8_t
{
    Unknown = 0,
    Healing,       // Restores health
    Damage,        // Deals damage
    Defensive,     // Wards, armor spells
    Utility,       // Light, detect, telekinesis
    Summon,        // Conjuration summons, bound weapons
    Buff,          // Enhancement (muffle, invisibility)
    Debuff         // Crowd control (calm, fear, paralyze)
};
```

### SpellTag (Bitflags)

**What it answers:** "What properties does this spell have?"

```cpp
enum class SpellTag : uint32_t
{
    None = 0,

    // Elements
    Fire          = 1 << 0,
    Frost         = 1 << 1,
    Shock         = 1 << 2,
    Poison        = 1 << 3,
    Sun           = 1 << 4,   // Anti-undead (Dawnguard)

    // Delivery
    Ranged        = 1 << 5,
    Melee         = 1 << 6,
    AOE           = 1 << 7,
    Concentration = 1 << 8,

    // Special properties
    AntiUndead    = 1 << 9,
    AntiDaedra    = 1 << 10,
    Stealth       = 1 << 11,  // Muffle, Invisibility, Waterbreathing
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

### MagicSchool & ElementType

> **Note:** `Item::ElementType` is a separate enum in `ItemData.h` with a different ordering and includes `Disease` instead of `Sun`. See [Item Classification](#item-classification) below.

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

```cpp
enum class ItemType : uint8_t
{
    Unknown = 0,
    HealthPotion,
    MagickaPotion,
    StaminaPotion,
    ResistPotion,
    BuffPotion,
    CurePotion,
    Poison,
    Food,
    Ingredient,
    SoulGem
};
```

> **Note:** Scrolls are NOT part of `ItemType`. They have their own `ScrollClassifier` using `ScrollType` (a type alias to `Spell::SpellType`). See [Scroll Classification](#scroll-classification) below.

### Item::ElementType

**Different from `Spell::ElementType`** — includes `Disease`, excludes `Sun`, different ordering for `Magic`/`Poison`:

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

Overflow tags that didn't fit in the 32-bit primary `ItemTag`. Used for poison modifiers, ravage effects, and soul gem capacities.

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

### Item Grouped Tag Fields

`ItemData` uses grouped tags with companion enum fields to avoid combinatorial explosion:

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

---

## API-Based Classification

Use CommonLibSSE APIs instead of name-matching.

### School Detection

```cpp
// SpellClassifier private method (v0.7.19: takes pre-computed RE::Effect*)
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

```cpp
// SpellClassifier private method (v0.7.19: takes pre-computed RE::Effect*)
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

The current implementation uses a school + hostility hybrid approach rather than pure archetype matching. Archetype is only used for specific cases (Summon, Healing, Buff, Utility). Tag-based fallback (`DeriveSpellTypeFromTags()`) handles remaining cases.

```cpp
// SpellClassifier private method — API-based only, no name fallback
SpellType SpellClassifier::DetermineSpellType(RE::SpellItem* spell, RE::EffectSetting* primaryEffect) const {
    // Pre-compute from primaryEffect
    const auto archetype = primaryEffect->GetArchetype();
    const bool isHostile = primaryEffect->data.flags.any(Flag::kHostile);
    const auto school = /* derived from primaryEffect->GetMagickSkill() */;

    // Healing: non-hostile health restoration via kValueModifier or kPeakValueModifier
    if (!isHostile && primaryAV == kHealth && (arch == kValueModifier || kPeakValueModifier))
        return SpellType::Healing;

    // Summon: kSummonCreature archetype only
    if (archetype == kSummonCreature) return SpellType::Summon;

    // Damage: hostile Destruction school
    if (isHostile && school == Destruction) return SpellType::Damage;

    // Defensive: Alteration DamageResist
    if (school == Alteration && !isHostile && primaryAV == kDamageResist) return SpellType::Defensive;

    // Debuff: hostile Illusion school
    if (isHostile && school == Illusion) return SpellType::Debuff;

    // Buff: non-hostile self-targeted (Invisibility, Cloak archetypes)
    if (!isHostile && delivery == kSelf && (arch == kInvisibility || kCloak)) return SpellType::Buff;

    // Utility: Light archetype
    if (archetype == kLight) return SpellType::Utility;

    return SpellType::Unknown;  // Falls back to DeriveSpellTypeFromTags()
}
```

### ItemType from Flags

```cpp
ItemType GetItemType(RE::AlchemyItem* item) {
    if (item->IsPoison()) return ItemType::Poison;
    if (item->IsFood()) return ItemType::Food;

    auto* effect = GetCostliestEffect(item);
    if (!effect || !effect->baseEffect) return ItemType::Unknown;

    auto arch = effect->baseEffect->GetArchetype();
    auto primaryAV = effect->baseEffect->data.primaryAV;

    if (arch == kCureDisease || arch == kCurePoison) {
        return ItemType::CurePotion;
    }

    if (arch == kValueModifier) {
        switch (primaryAV) {
            case RE::ActorValue::kHealth:  return ItemType::HealthPotion;
            case RE::ActorValue::kMagicka: return ItemType::MagickaPotion;
            case RE::ActorValue::kStamina: return ItemType::StaminaPotion;
        }
        if (effect->baseEffect->data.resistVariable != RE::ActorValue::kNone) {
            return ItemType::ResistPotion;
        }
    }

    return ItemType::BuffPotion;
}
```

### Costliest Effect (Primary Effect)

Uses Skyrim's actual cost formula (v0.7.10) with exponential magnitude scaling:

```cpp
RE::Effect* GetCostliestEffect(RE::MagicItem* item) {
    RE::Effect* costliest = nullptr;
    RE::Effect* firstValid = nullptr;
    float highestCost = -1.0f;

    for (auto* effect : item->effects) {
        if (!effect || !effect->baseEffect) continue;
        if (!firstValid) firstValid = effect;

        // Skyrim's actual cost formula: baseCost × magnitude^1.1 × durationFactor × areaFactor
        float baseCost = effect->baseEffect->data.baseCost;
        float magnitude = effect->effectItem.magnitude;
        float duration = effect->effectItem.duration;
        float area = effect->effectItem.area;

        float magnitudeFactor = std::pow(std::max(1.0f, magnitude), 1.1f);
        float durationFactor = (duration > 0) ? (duration / 10.0f) : 1.0f;
        float areaFactor = (area > 0) ? (0.15f * area) : 1.0f;

        float cost = baseCost * magnitudeFactor * durationFactor * areaFactor;
        if (cost > highestCost) {
            highestCost = cost;
            costliest = effect;
        }
    }
    return costliest ? costliest : firstValid;  // Safety fallback
}
```

---

## Scroll Classification

### Architecture

`ScrollClassifier` (v0.7.7) classifies `RE::ScrollItem*` by delegating to `SpellClassifier`. This works because `ScrollItem` inherits from `SpellItem` in Skyrim's type hierarchy.

```
RE::ScrollItem* scroll
       │
       ▼
ScrollClassifier::ClassifyScroll()
       │
       ├─ GetLinkedSpell() → cast scroll as RE::SpellItem*
       │
       ├─ SpellClassifier::ClassifySpell(linkedSpell) → SpellData
       │
       ├─ ConvertToScrollData() → ScrollData (copy type, tags, school, element)
       │
       └─ Override magnitude/duration with scroll-specific values
```

### ScrollData Type Aliases

`ScrollData` reuses spell classification types via type aliases:

```cpp
using ScrollType = Spell::SpellType;      // Damage, Healing, Defensive, Utility, Summon, Buff, Debuff
using ScrollTag = Spell::SpellTag;        // Fire, Frost, Shock, AOE, AntiUndead, etc.
using MagicSchool = Spell::MagicSchool;   // Destruction, Restoration, Alteration, Illusion, Conjuration
using ElementType = Spell::ElementType;   // Fire, Frost, Shock, Poison, Sun, Magic
```

Scrolls share `SlotContentType::Spell` in the display layer — there is no separate `Scroll` slot content type.

---

## Name-Matching Consolidation (v0.7.1)

### Problem

Prior to v0.7.1, name-matching was scattered across multiple functions:

| Function | NameContains() calls | Purpose |
|----------|---------------------|---------|
| `DetermineSpellTags()` | 43 | Comprehensive tag detection |
| `DetermineSpellTypeByName()` | 25 | Fallback type detection |
| `DetermineSpellType()` | 3 | Ward, bound, paralyze fallbacks |
| `DetermineElementType()` | 3 | Sun damage detection |
| **Total** | **47** | Duplicated logic |

### Solution: Single Source of Truth

`SpellTag` enum + `DetermineSpellTags()` handles ALL name-based detection. Other functions derive their values from tags instead of re-parsing names.

As of v0.7.21 (M6 optimization), `DetermineSpellTags()` uses a pre-lowercased string and a `contains()` lambda for all keyword matching — no more `NameContains()` calls.

```
┌─────────────────────────────────────────────────────────────────────┐
│                      ClassifySpell() Flow                           │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  STEP 1: Tags first (single source of name-matching)                │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  data.tags = DetermineSpellTags(spell)                        │  │
│  │  • Pre-lowercased name + contains() lambda                    │  │
│  │  • Generates all SpellTag bitflags                            │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                              │                                      │
│                              ▼                                      │
│  STEP 2: Type detection                                             │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  data.type = DetermineSpellType(spell, primaryEffect)         │  │
│  │  // School+hostility hybrid (see above)                       │  │
│  │  if (data.type == Unknown)                                    │  │
│  │      data.type = DeriveSpellTypeFromTags(data.tags)           │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                              │                                      │
│                              ▼                                      │
│  STEP 3: School detection (API only)                                │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  data.school = DetermineMagicSchool(costliestEffect)          │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                              │                                      │
│                              ▼                                      │
│  STEP 4: Element detection                                          │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  data.element = DetermineElementType(costliestEffect)         │  │
│  │  if (data.element == None)                                    │  │
│  │      data.element = DeriveElementFromTags(data.tags)          │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Helper Functions

```cpp
// Derive SpellType from computed SpellTag bitflags (fallback when API fails)
static SpellType DeriveSpellTypeFromTags(SpellTag tags) noexcept
{
    // Priority order matters - check most specific first
    if (HasTag(tags, SpellTag::RestoreHealth)) return SpellType::Healing;

    if (HasTag(tags, SpellTag::BoundWeapon) ||
        HasTag(tags, SpellTag::SummonDaedra) ||
        HasTag(tags, SpellTag::SummonUndead) ||
        HasTag(tags, SpellTag::SummonCreature))
        return SpellType::Summon;

    if (HasTag(tags, SpellTag::Ward) || HasTag(tags, SpellTag::Armor))
        return SpellType::Defensive;

    // ... etc (see SpellClassifier.cpp)
}

// Derive ElementType from computed SpellTag bitflags
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

### Result

| Before | After |
|--------|-------|
| 47 NameContains() calls across 4 functions | Single `contains()` lambda in `DetermineSpellTags()` |
| Duplicated name patterns | Single source of truth |
| Risk of type/element mismatch | Guaranteed consistency |

---

## Semantic Override Pattern (v0.7.2)

### Problem

API-based classification is reliable but not always semantically correct:

| Spell | API Result | Semantic Truth | Root Cause |
|-------|------------|----------------|------------|
| Sunbeam | `element=Magic` | `element=Sun` | Dawnguard sun spells use `kResistMagic` internally |
| Waterbreathing | `type=Unknown` | `type=Buff` | No archetype match, no existing tag pattern |
| Open Novice Lock | `type=Damage, element=Frost` | `type=Utility, element=None` | Modded spell with unusual effect configuration |

### Solution: Post-API Semantic Overrides

After API-based classification, apply semantic corrections based on tags or type:

```cpp
// STEP 4: Element - API first, then derive from tags
data.element = DetermineElementType(costliestEffect);  // API-based (resistVariable)
if (data.element == ElementType::None) {
    data.element = DeriveElementFromTags(data.tags);  // Tag-based fallback
}

// === SEMANTIC OVERRIDES ===

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

### Tag Detection Additions

```cpp
// In DetermineSpellTags():

// "open" → Telekinesis tag (derives to Utility type)
if (contains("open")) {
    tags |= SpellTag::Telekinesis;
}

// "waterbreath" → Stealth tag (derives to Buff type)
if (contains("waterbreath")) {
    tags |= SpellTag::Stealth;
}
```

### Type Derivation Update

```cpp
// In DeriveSpellTypeFromTags():

// Buff (stealth, invisibility, muffle, waterbreathing)
if (HasTag(tags, SpellTag::Invisibility) || HasTag(tags, SpellTag::Muffle) ||
    HasTag(tags, SpellTag::Stealth)) {  // Stealth now catches waterbreathing
    return SpellType::Buff;
}
```

### Design Principles

1. **API first, override second** - Trust the API for most spells, only override known semantic mismatches
2. **Tag-driven overrides** - Overrides are based on detected tags, not hardcoded FormIDs (mod-compatible)
3. **Type-driven element clearing** - Utility spells never deal elemental damage, so clear spurious elements
4. **Reuse existing tags** - "waterbreath" uses `Stealth` tag (environmental protection buff), "open" uses `Telekinesis` tag (both derive to correct types)

### Classification Updates

When classification logic changes, all forms are re-classified on the next registry scan (game load). No manual cache invalidation is needed.

---

## Weapon Classification (v0.7.6)

### WeaponType

**What it answers:** "What kind of weapon is this?"

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
    TwoHandSword,
    TwoHandAxe,
    TwoHandMace,

    // Ranged
    Bow,
    Crossbow,

    // Special
    Staff
};
```

### WeaponTag (Bitflags)

**What it answers:** "What properties does this weapon have?"

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

### AmmoType

**What it answers:** "What kind of ammo is this?"

```cpp
enum class AmmoType : uint8_t
{
    Unknown = 0,
    Arrow,          // Standard arrow
    Bolt            // Crossbow bolt
};
```

---

## Weapon Inventory Tracking (v0.7.6)

### WeaponRegistry Scope

**What it tracks:** All weapons and ammo in player inventory

Unlike SpellRegistry (tracks known spells) and ItemRegistry (tracks alchemy items), WeaponRegistry tracks **all weapons** in the player's inventory, with `isFavorited` and `isEquipped` as metadata flags.

### Favorites Detection: ExtraHotkey

**How Skyrim stores favorites:** When you star an item in the Favorites menu, Skyrim adds `ExtraHotkey` extra data to the inventory entry.

```cpp
// Correct favorites detection (WeaponRegistry.cpp)
if (entry->extraLists) {
    for (auto* extraList : *entry->extraLists) {
        if (!extraList) continue;

        // Check if favorited (has ExtraHotkey, regardless of slot assignment)
        // ExtraHotkey can be kUnbound (-1) or kSlot1-8 (0-7)
        if (extraList->HasType(RE::ExtraDataType::kHotkey)) {
            sw.isFavorited = true;
        }
    }
}
```

**Key insight:** `ExtraHotkey` exists with two values:
- `kUnbound (-1)` - Favorited but not assigned to quickslot keys 1-8
- `kSlot1-8 (0-7)` - Favorited AND assigned to a hotkey

**Common mistake:** Using `MagicFavorites::hotkeys` array instead of `ExtraHotkey`
- `MagicFavorites::hotkeys` only contains items assigned to quickslot keys 1-8
- Favorited-but-not-hotkeyed items will NOT appear in this array
- This is the wrong API for detecting all favorited items

### Charge Tracking

Enchanted weapons require charge monitoring via `ExtraCharge` extra data:

```cpp
// Get enchantment charge from ExtraCharge
auto* extraCharge = extraList->GetByType<RE::ExtraCharge>();
if (extraCharge) {
    sw.currentCharge = extraCharge->charge;
}

// Get max charge from enchantable form
auto* enchantable = weapon->As<RE::TESEnchantableForm>();
if (enchantable && enchantable->formEnchanting) {
    sw.maxCharge = static_cast<float>(enchantable->amountofEnchantment);
}
```

### Inventory Metadata Structure

```cpp
struct InventoryWeapon {
    WeaponData data;           // Classification (type, tags, damage, etc.)
    bool isFavorited = false;  // Has ExtraHotkey in extra data
    bool isEquipped = false;   // Currently in left or right hand
    float previousCharge = 0.0f; // Charge at last poll (for delta detection)
};

struct InventoryAmmo {
    AmmoData data;             // Classification (type, damage, etc.)
    int32_t count = 0;         // Current inventory count
    bool isEquipped = false;   // Currently equipped as active ammo
};
```

### Reconciliation Strategy

**RefreshCharges()** - High-frequency updates (500ms interval):
- Updates `isEquipped` status
- Updates enchantment charge levels
- Tracks `previousCharge` for detecting changes
- No add/remove operations

**ReconcileWeapons()** - Low-frequency reconciliation (30s interval):
- Adds newly acquired weapons
- Removes dropped/sold weapons
- Updates `isFavorited` status (detects favorites menu changes)
- Logs favorites status changes

### API-Based Classification

Use weapon data flags and keywords:

```cpp
WeaponType GetWeaponType(RE::TESObjectWEAP* weapon) {
    auto animType = weapon->GetWeaponType();
    switch (animType) {
        case RE::WEAPON_TYPE::kOneHandDagger:   return WeaponType::OneHandDagger;
        case RE::WEAPON_TYPE::kOneHandSword:    return WeaponType::OneHandSword;
        case RE::WEAPON_TYPE::kOneHandAxe:      return WeaponType::OneHandAxe;
        case RE::WEAPON_TYPE::kOneHandMace:     return WeaponType::OneHandMace;
        case RE::WEAPON_TYPE::kTwoHandSword:    return WeaponType::TwoHandSword;
        case RE::WEAPON_TYPE::kTwoHandAxe:      return WeaponType::TwoHandAxe;
        case RE::WEAPON_TYPE::kBow:             return WeaponType::Bow;
        case RE::WEAPON_TYPE::kCrossbow:        return WeaponType::Crossbow;
        case RE::WEAPON_TYPE::kStaff:           return WeaponType::Staff;
        default: return WeaponType::Unknown;
    }
}

bool IsSilver(RE::TESObjectWEAP* weapon) {
    return weapon->HasKeywordString("WeapMaterialSilver");
}
```

> **Note:** `RE::WEAPON_TYPE` has no `kTwoHandMace` value. `TwoHandMace` in `WeaponType` is not directly mapped from the API — it may be set via keyword detection or name matching for warhammers.

---

## Classification Data

Each registry stores classification results in its own data struct:
- `SpellData` (`src/spell/SpellData.h`) — spells
- `ItemData` (`src/learning/item/ItemData.h`) — potions, poisons, food, ingredients, soul gems
- `ScrollData` (`src/scroll/ScrollData.h`) — scrolls (reuses `SpellType`/`SpellTag` via type aliases)
- `WeaponData` (`src/weapon/WeaponData.h`) — weapons and ammo

These are populated directly by the classifier at registration time — no external persistence layer is used.

---

## Classification Priority

| Priority | Method | Reliability |
|----------|--------|-------------|
| 1 | Effect archetype | Very High |
| 2 | resistVariable | Very High |
| 3 | associatedSkill | Very High |
| 4 | Item flags (IsPoison, IsFood) | Very High |
| 5 | Keywords | High |
| 6 | Name matching | Low (fallback) |

---

## Future Work (Post v1.0)

Not in scope for current version:

- **Archetype classification** (Necromancer, Paladin, etc.) - For archetype-based slots
- **Mod keyword detection** (Triumvirate keywords) - For mod integration
- **User override files** (JSON) - For manual classification

See [ROADMAP.md](../ROADMAP.md) backlog.
