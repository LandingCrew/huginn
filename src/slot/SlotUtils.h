#pragma once

#include "SlotAssignment.h"
#include "ui/SlotTypes.h"
#include "learning/item/ItemData.h"
#include "weapon/WeaponData.h"

namespace Huginn::Slot
{
    // =============================================================================
    // SLOT UTILITIES
    // =============================================================================
    // Helper functions for converting SlotAssignments to widget/wheeler formats.
    // =============================================================================

    /// Get a short display name for a slot classification (for "No X" messages)
    [[nodiscard]] inline std::string GetClassificationDisplayName(SlotClassification c) noexcept
    {
        switch (c) {
            case SlotClassification::DamageAny:   return "damage";
            case SlotClassification::HealingAny:  return "healing";
            case SlotClassification::BuffsAny:    return "buffs";
            case SlotClassification::DefensiveAny: return "defensive";
            case SlotClassification::SummonsAny:  return "summons";
            case SlotClassification::Utility:     return "utility";
            case SlotClassification::PotionsAny:  return "potions";
            case SlotClassification::ScrollsAny:  return "scrolls";
            case SlotClassification::SpellsAny:   return "spells";
            case SlotClassification::SpellsDestruction: return "destruction spells";
            case SlotClassification::SpellsRestoration: return "restoration spells";
            case SlotClassification::SpellsConjuration: return "conjuration spells";
            case SlotClassification::SpellsIllusion:    return "illusion spells";
            case SlotClassification::SpellsAlteration:  return "alteration spells";
            case SlotClassification::WeaponsAny:  return "weapons";
            default:                              return "match";
        }
    }

    /// Convert a SlotAssignment to SlotContent for display layers.
    /// Handles all candidate types and assignment types appropriately.
    [[nodiscard]] inline UI::SlotContent ToSlotContent(const SlotAssignment& assignment)
    {
        if (assignment.IsEmpty() || !assignment.HasCandidate()) {
            // Show "(No healing)" etc. for typed slots, "(Learning...)" for Regular
            if (assignment.classification != SlotClassification::Regular) {
                std::string msg = std::format("(No {})", GetClassificationDisplayName(assignment.classification));
                return UI::SlotContent::NoMatch(msg);
            }
            return UI::SlotContent::NoMatch("(Learning...)");
        }

        const auto& candidate = *assignment.candidate;
        const std::string& name = assignment.name;
        const float confidence = assignment.utility;
        const RE::FormID formID = assignment.formID;

        // Handle by assignment type first
        if (assignment.IsOverride()) {
            // Check if this is a health or magicka potion override
            if (candidate.Is<Candidate::ItemCandidate>()) {
                const auto& item = candidate.As<Candidate::ItemCandidate>();
                if (item.type == Item::ItemType::HealthPotion ||
                    Item::HasTag(item.tags, Item::ItemTag::RestoreHealth)) {
                    return UI::SlotContent::HealthPotion(name, formID);
                }
                if (item.type == Item::ItemType::MagickaPotion ||
                    Item::HasTag(item.tags, Item::ItemTag::RestoreMagicka)) {
                    return UI::SlotContent::MagickaPotion(name, formID);
                }
            }
            // For other overrides, show as regular spell/item with high confidence
            return UI::SlotContent::Spell(name, 1.0f, formID);
        }

        if (assignment.IsWildcard()) {
            return UI::SlotContent::Wildcard(name, confidence, formID);
        }

        // Normal assignment - determine type based on candidate
        switch (candidate.GetSourceType()) {
            case Candidate::SourceType::Spell:
            case Candidate::SourceType::Scroll:
                return UI::SlotContent::Spell(name, confidence, formID);

            case Candidate::SourceType::Potion:
            case Candidate::SourceType::Food:
                // Check potion type for specific styling
                if (candidate.Is<Candidate::ItemCandidate>()) {
                    const auto& item = candidate.As<Candidate::ItemCandidate>();
                    if (item.type == Item::ItemType::HealthPotion) {
                        return UI::SlotContent::HealthPotion(name, formID);
                    }
                    if (item.type == Item::ItemType::MagickaPotion) {
                        return UI::SlotContent::MagickaPotion(name, formID);
                    }
                    if (item.type == Item::ItemType::StaminaPotion) {
                        return UI::SlotContent::StaminaPotion(name, formID);
                    }
                }
                // All other potions (resist, buff, cure, etc.) use generic Potion type
                return UI::SlotContent::Potion(name, formID);

            case Candidate::SourceType::Weapon:
                // Check if melee or ranged
                if (candidate.Is<Candidate::WeaponCandidate>()) {
                    const auto& weapon = candidate.As<Candidate::WeaponCandidate>();
                    if (Weapon::HasTag(weapon.tags, Weapon::WeaponTag::Ranged)) {
                        return UI::SlotContent::RangedWeapon(name, formID);
                    }
                }
                return UI::SlotContent::MeleeWeapon(name, formID);

            case Candidate::SourceType::Ammo:
                return UI::SlotContent::RangedWeapon(name, formID);

            case Candidate::SourceType::SoulGem:
                return UI::SlotContent::SoulGem(name, formID);

            default:
                return UI::SlotContent::Spell(name, confidence, formID);
        }
    }

    /// Extract UniqueIDs from slot assignments (for Wheeler weapon support)
    /// Returns 0 for non-weapon candidates (spells, potions, scrolls)
    [[nodiscard]] inline std::vector<uint16_t> ExtractUniqueIDs(const SlotAssignments& assignments)
    {
        std::vector<uint16_t> ids;
        ids.reserve(assignments.size());

        for (const auto& a : assignments) {
            uint16_t uid = 0;
            if (a.HasCandidate()) {
                uid = a.candidate->GetUniqueID();
            }
            ids.push_back(uid);
        }

        return ids;
    }

    /// Extract FormIDs from slot assignments (for Wheeler)
    [[nodiscard]] inline std::vector<RE::FormID> ExtractFormIDs(const SlotAssignments& assignments)
    {
        std::vector<RE::FormID> formIDs;
        formIDs.reserve(assignments.size());

        for (const auto& a : assignments) {
            formIDs.push_back((!a.IsEmpty() && a.formID != 0) ? a.formID : 0);
        }

        return formIDs;
    }

    /// Extract wildcard flags from slot assignments (for Wheeler)
    [[nodiscard]] inline std::vector<bool> ExtractWildcardFlags(const SlotAssignments& assignments)
    {
        std::vector<bool> flags;
        flags.reserve(assignments.size());

        for (const auto& a : assignments) {
            flags.push_back(!a.IsEmpty() ? a.IsWildcard() : false);
        }

        return flags;
    }

    /// Extract subtext labels from slot assignments (for Wheeler)
    [[nodiscard]] inline std::vector<std::string> ExtractSubtexts(const SlotAssignments& assignments)
    {
        std::vector<std::string> labels;
        labels.reserve(assignments.size());
        for (const auto& a : assignments) {
            labels.push_back(a.subtextLabel);
        }
        return labels;
    }

    /// Derive a short explanation label from a candidate's relevance tags.
    /// Returns the first matching human-readable reason, or empty string if none.
    /// Used for Wheeler subtext when showExplanationLabel is enabled.
    [[nodiscard]] inline std::string DeriveExplanationLabel(const SlotAssignment& assignment)
    {
        if (!assignment.HasCandidate()) {
            return {};
        }

        const auto& base = Candidate::GetBase(assignment.candidate->candidate);
        const auto tags = base.relevanceTags;

        // Priority order: most urgent/specific first
        using RT = Candidate::RelevanceTag;

        if (HasTag(tags, RT::CriticalHealth))    return "Critical HP";
        if (HasTag(tags, RT::Underwater))         return "Underwater";
        if (HasTag(tags, RT::LookingAtLock))      return "Lock";
        if (HasTag(tags, RT::OnFire))             return "Fire Damage";
        if (HasTag(tags, RT::Poisoned))           return "Poisoned";
        if (HasTag(tags, RT::Diseased))           return "Diseased";
        if (HasTag(tags, RT::TakingFrost))        return "Frost Damage";
        if (HasTag(tags, RT::TakingShock))        return "Shock Damage";
        if (HasTag(tags, RT::Falling))            return "Falling";
        if (HasTag(tags, RT::LowHealth))          return "Low HP";
        if (HasTag(tags, RT::LowMagicka))         return "Low MP";
        if (HasTag(tags, RT::LowStamina))         return "Low SP";
        if (HasTag(tags, RT::WeaponLowCharge))    return "Low Charge";
        if (HasTag(tags, RT::NeedsAmmo))          return "Low Ammo";
        if (HasTag(tags, RT::AllyInjured))        return "Ally Hurt";
        if (HasTag(tags, RT::LookingAtOre))       return "Ore Vein";
        if (HasTag(tags, RT::InDarkness))         return "Darkness";
        if (HasTag(tags, RT::Sneaking))           return "Sneaking";
        if (HasTag(tags, RT::TargetUndead))       return "Undead";
        if (HasTag(tags, RT::TargetDragon))       return "Dragon";
        if (HasTag(tags, RT::MultipleEnemies))    return "Outnumbered";
        if (HasTag(tags, RT::EnemyCasting))       return "Enemy Casting";

        // Favorited items get a label if nothing more specific applies
        if (assignment.candidate->IsFavorited())  return "Favorite";

        return {};
    }

}  // namespace Huginn::Slot
