#pragma once

#include "core/SlotClassification.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Huginn::Slot
{
    // SlotClassification (the enum and SLOT_CLASSIFICATION_COUNT) lives in
    // core/SlotClassification.h since R11, so the pure class predicates
    // (core/CapFacets.h) can name it.

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
        Other,  // Non-resource overrides only (drowning, weapon charge, ammo)
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
            case OverrideFilter::Other: return "Other";
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

        // If true, skip candidates that are already equipped (show alternatives
        // only). On by default since v0.21.51: an equipped SPELL never reaches
        // a slot (CandidateFilters), and a held weapon taking a key is the
        // same waste (the Hunting Bow in hand on key 7, 2026-09-28).
        bool skipEquipped = true;

        // If true, pressing this slot to equip something holds what the equip
        // took off here for [SlotLocker] fRemembranceDurationMs (Remembrance.h)
        bool remembrance = true;

        // Priority for allocation order (higher = filled first)
        // When multiple slots could accept a candidate, higher priority slots get it
        int8_t priority = 0;
    };

    // =============================================================================
    // HELPER FUNCTIONS
    // =============================================================================

    [[nodiscard]] inline constexpr std::string_view SlotClassificationToString(SlotClassification c) noexcept
    {
        static_assert(SLOT_CLASSIFICATION_COUNT == 24,
            "SlotClassification changed — add the new case to this switch and its siblings");
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
            case SlotClassification::DamageMagic:       return "DamageMagic";
            case SlotClassification::PoisonsAny:        return "PoisonsAny";
            case SlotClassification::WeaponsAny:   return "WeaponsAny";
            case SlotClassification::WeaponsMelee: return "WeaponsMelee";
            case SlotClassification::WeaponsRanged: return "WeaponsRanged";
            case SlotClassification::FoodAny:      return "FoodAny";
            case SlotClassification::AlcoholAny:   return "AlcoholAny";
            case SlotClassification::AmmoAny:      return "AmmoAny";
            case SlotClassification::ApparelAny:   return "ApparelAny";
            case SlotClassification::Regular:      return "Regular";
            default:                              return "Unknown";
        }
    }

}  // namespace Huginn::Slot
