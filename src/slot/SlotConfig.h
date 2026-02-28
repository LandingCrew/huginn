#pragma once

#include <cstdint>
#include <string_view>

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

    enum class SlotClassification : uint8_t
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
        AmmoAny,        // Ammunition (arrows, bolts)

        // Unrestricted
        Regular,        // No restriction - accepts any candidate (legacy behavior)

        _Count          // Sentinel for array sizing
    };

    inline constexpr size_t SLOT_CLASSIFICATION_COUNT = static_cast<size_t>(SlotClassification::_Count);

    // =============================================================================
    // OVERRIDE FILTER
    // =============================================================================
    // Determines which override categories a slot accepts.
    //   None = no overrides (old false)
    //   Any  = any override (old true)
    //   HP/MP/SP = only that resource's overrides
    // =============================================================================

    enum class OverrideFilter : uint8_t
    {
        None,   // No overrides (old false)
        Any,    // Any override (old true)
        HP,     // Health overrides only
        MP,     // Magicka overrides only
        SP,     // Stamina overrides only
        _Count
    };

    static_assert(static_cast<uint8_t>(OverrideFilter::_Count) <= 8,
        "OverrideFilter enum exceeds expected size for serialization");

    [[nodiscard]] inline constexpr std::string_view OverrideFilterToString(OverrideFilter f) noexcept
    {
        switch (f) {
            case OverrideFilter::None: return "None";
            case OverrideFilter::Any:  return "Any";
            case OverrideFilter::HP:   return "HP";
            case OverrideFilter::MP:   return "MP";
            case OverrideFilter::SP:   return "SP";
            default:                   return "Unknown";
        }
    }

    // =============================================================================
    // SLOT CONFIGURATION
    // =============================================================================
    // Configuration for a single slot in the recommendation widget.
    // Combines classification filter with behavior flags.
    // =============================================================================

    struct SlotConfig
    {
        SlotClassification classification = SlotClassification::Regular;

        // If true, wildcards can appear in this slot (exploration picks)
        bool wildcardsEnabled = true;

        // Which override categories this slot accepts (None, Any, HP, MP, SP)
        OverrideFilter overrideFilter = OverrideFilter::Any;

        // If true, skip candidates that are already equipped (show alternatives only)
        bool skipEquipped = false;

        // Priority for allocation order (higher = filled first)
        // When multiple slots could accept a candidate, higher priority slots get it
        int8_t priority = 0;
    };

    // =============================================================================
    // HELPER FUNCTIONS
    // =============================================================================

    [[nodiscard]] inline constexpr std::string_view SlotClassificationToString(SlotClassification c) noexcept
    {
        switch (c) {
            case SlotClassification::DamageAny:   return "DamageAny";
            case SlotClassification::HealingAny:  return "HealingAny";
            case SlotClassification::BuffsAny:    return "BuffsAny";
            case SlotClassification::DefensiveAny: return "DefensiveAny";
            case SlotClassification::SummonsAny:  return "SummonsAny";
            case SlotClassification::Utility:     return "Utility";
            case SlotClassification::PotionsAny:  return "PotionsAny";
            case SlotClassification::ScrollsAny:  return "ScrollsAny";
            case SlotClassification::SpellsAny:   return "SpellsAny";
            case SlotClassification::SpellsDestruction: return "SpellsDestruction";
            case SlotClassification::SpellsRestoration: return "SpellsRestoration";
            case SlotClassification::SpellsConjuration: return "SpellsConjuration";
            case SlotClassification::SpellsIllusion:    return "SpellsIllusion";
            case SlotClassification::SpellsAlteration:  return "SpellsAlteration";
            case SlotClassification::WeaponsAny:   return "WeaponsAny";
            case SlotClassification::WeaponsMelee: return "WeaponsMelee";
            case SlotClassification::WeaponsRanged: return "WeaponsRanged";
            case SlotClassification::FoodAny:      return "FoodAny";
            case SlotClassification::AmmoAny:      return "AmmoAny";
            case SlotClassification::Regular:      return "Regular";
            default:                              return "Unknown";
        }
    }

}  // namespace Huginn::Slot
