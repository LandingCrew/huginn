#pragma once

// =============================================================================
// SLOT CLASSIFICATION -- the classes a slot can restrict itself to
// =============================================================================
// Moved whole from slot/SlotConfig.h in R11 (it keeps its namespace, as
// ActorTypeClassifier did), so the pure class predicates over cap(i)
// (core/CapFacets.h) can name it. The enum stays: players name its values in
// their INI layouts (`sClassification = HealingAny`). What each value MATCHES
// is derived from the effect catalog since R11 (CapFacets.h), not from the
// old spell and item classifiers' tags.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace Huginn::Slot
{
    // =============================================================================
    // SLOT CLASSIFICATION
    // =============================================================================
    // Determines what type of items can be assigned to a slot.
    // Each slot can restrict candidates to a specific category, enabling
    // predictable layouts like "Damage | Healing | Buffs".
    //
    // Classification is hierarchical:
    //   - Effect-based (DamageAny, HealingAny, etc.) for gameplay roles
    //   - Item-type based (PotionsAny, ScrollsAny) for inventory management
    //   - Regular (no restriction) for legacy/freestyle layouts
    // =============================================================================

    enum class SlotClassification : std::uint8_t
    {
        // Effect-based classifications (recommended)
        DamageAny,      // Fire, frost, shock, poison damage spells/scrolls/poisons
        HealingAny,     // RestoreHealth spells/potions
        BuffsAny,       // Armor, cloak, invisibility, muffle, fortify
        DefensiveAny,   // Ward, resist potions, armor spells
        SummonsAny,     // Conjuration summons, bound weapons
        Utility,        // Light, detect, unlock, transmute, waterbreathing

        // Item-type based classifications (for inventory-focused layouts)
        PotionsAny,     // Any potion (restore, resist, buff — not food)
        ScrollsAny,     // Any scroll
        SpellsAny,      // Any spell (all schools, all types)
        SpellsDestruction,  // Destruction spells only (fire, frost, shock damage)
        SpellsRestoration,  // Restoration spells only (healing, wards, turn undead)
        SpellsConjuration,  // Conjuration spells only (summons, bound weapons)
        SpellsIllusion,     // Illusion spells only (calm, fear, frenzy, muffle, invis)
        SpellsAlteration,   // Alteration spells only (armor, detect, light, paralysis)
        WeaponsAny,     // Any weapon (melee + ranged + staff)
        WeaponsMelee,   // Melee weapons only (sword, axe, mace, dagger)
        WeaponsRanged,  // Ranged weapons only (bow, crossbow, staff)
        FoodAny,        // Food items (CC Survival Mode)
        AlcoholAny,     // Alcoholic beverages (ale, mead, wine, skooma)
        AmmoAny,        // Ammunition (arrows, bolts)
        ApparelAny,     // #65: fortify-crafting gear (only scores at a workstation)
        DamageMagic,    // Damage spells and damage scrolls: DamageAny minus
                        // weapons and poisons, for an "attack magic" key
        PoisonsAny,     // Poisons only (a poison key for an archer or assassin)

        // Unrestricted
        Regular,        // No restriction - accepts any candidate (legacy behavior)

        _Count          // Sentinel for array sizing
    };

    inline constexpr std::size_t SLOT_CLASSIFICATION_COUNT = static_cast<std::size_t>(SlotClassification::_Count);
}  // namespace Huginn::Slot
