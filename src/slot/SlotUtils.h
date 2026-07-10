#pragma once

#include "SlotAssignment.h"
#include "SlotLocker.h"
#include "ui/SlotTypes.h"
#include "learning/item/ItemData.h"
#include "weapon/WeaponData.h"
#include <set>
#include <spdlog/spdlog.h>

namespace Huginn::Slot
{
    // =============================================================================
    // SLOT UTILITIES
    // =============================================================================
    // Helper functions for converting SlotAssignments to widget/wheeler formats.
    // =============================================================================

    /// Get a short display name for a slot classification (for "No X" messages).
    /// Returns a string_view into static storage — no allocation per empty slot.
    [[nodiscard]] inline constexpr std::string_view GetClassificationDisplayName(SlotClassification c) noexcept
    {
        // Tripwire: adding a SlotClassification means this switch (and the other
        // classification switches in SlotConfig.h/SlotClassifier.cpp/SlotSettings.cpp)
        // may need a new case — they default to a generic fallback otherwise.
        static_assert(SLOT_CLASSIFICATION_COUNT == 21,
            "SlotClassification changed — review GetClassificationDisplayName and sibling switches");
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
            // Override potions get specific styling for health/magicka/stamina
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
                if (item.type == Item::ItemType::StaminaPotion ||
                    Item::HasTag(item.tags, Item::ItemTag::RestoreStamina)) {
                    return UI::SlotContent::StaminaPotion(name, formID);
                }
            }
            // For non-potion overrides, fall through to normal source-type logic
            // (don't default to Spell — weapons, soul gems, etc. need correct types)
        }

        if (assignment.IsWildcard()) {
            // Only use Wildcard type for spells/scrolls (UI styling + equippable via spell path).
            // Non-spell wildcards (weapons, potions, etc.) need their real SlotContentType
            // so EquipSlot routes them correctly.
            auto src = candidate.GetSourceType();
            if (src == Candidate::SourceType::Spell || src == Candidate::SourceType::Scroll) {
                return UI::SlotContent::Wildcard(name, confidence, formID);
            }
            // Fall through to normal source-type logic for non-spell wildcards
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

            case Candidate::SourceType::Staff:
                return UI::SlotContent::RangedWeapon(name, formID);

            case Candidate::SourceType::Ammo:
                return UI::SlotContent::Ammo(name, formID);

            case Candidate::SourceType::SoulGem:
                return UI::SlotContent::SoulGem(name, formID);

            default:
                return UI::SlotContent::Spell(name, confidence, formID);
        }
    }

    /// Enrich slot assignments with visual state flags (override, wildcard,
    /// confirmed, expiring) based on lock timers and assignment history.
    /// Call after SlotLocker::ApplyLocks(). Uses a single lock snapshot rather
    /// than per-slot locker queries (which would take up to 3 acquisitions each).
    inline void ComputeVisualStates(
        SlotAssignments& assignments,
        const SlotAssignments& rawAssignments,
        const SlotLocker& locker)
    {
        // Expiring threshold: show pulse during last 40% of lock duration
        // (e.g., 3s lock → pulse starts at 1.2s remaining)
        const float lockDurationMs = locker.GetConfig().lockDurationMs;
        const float EXPIRING_THRESHOLD_MS = lockDurationMs * 0.4f;

        const auto lockSnapshot = locker.GetLockSnapshot();
        const size_t lockCount = lockSnapshot.size();

        for (size_t i = 0; i < assignments.size(); ++i) {
            auto& assignment = assignments[i];

            // Priority 1: Override/Wildcard (highest visual priority)
            if (assignment.IsOverride()) {
                assignment.visualState = SlotVisualState::Override;
                continue;
            }
            if (assignment.IsWildcard()) {
                assignment.visualState = SlotVisualState::Wildcard;
                continue;
            }

            // Skip empty slots
            if (assignment.IsEmpty()) {
                assignment.visualState = SlotVisualState::Normal;
                continue;
            }

            const size_t slot = assignment.slotIndex;
            const SlotLocker::SlotLockView* lock =
                slot < lockCount ? &lockSnapshot[slot] : nullptr;

            // Priority 2: Confirmed (re-evaluated to same item)
            if (lock && assignment.formID != 0 &&
                lock->hadContent && lock->previousFormID == assignment.formID) {
                assignment.visualState = SlotVisualState::Confirmed;
                continue;
            }

            // Priority 3: Expiring (lock about to expire AND content will change)
            if (lock && lock->isLocked &&
                lock->remainingMs > 0.0f && lock->remainingMs <= EXPIRING_THRESHOLD_MS) {
                const bool contentWillChange = (i < rawAssignments.size() &&
                    rawAssignments[i].formID != 0 &&
                    rawAssignments[i].formID != assignment.formID);

                if (contentWillChange) {
                    assignment.visualState = SlotVisualState::Expiring;
                    logger::debug("[VisualState] Slot {} set to EXPIRING ({:.0f}ms remaining, {} -> {})",
                        i, lock->remainingMs, assignment.name,
                        i < rawAssignments.size() ? rawAssignments[i].name : "empty");
                    continue;
                }
            }

            // Default: normal state (no special effect)
            assignment.visualState = SlotVisualState::Normal;
        }

        // Log visual state summary (non-normal states only) — one condensed line
        if (spdlog::default_logger()->should_log(spdlog::level::debug)) {
            std::string visualSummary;
            for (size_t i = 0; i < assignments.size(); ++i) {
                const auto& assignment = assignments[i];
                if (!assignment.IsEmpty() && assignment.visualState != SlotVisualState::Normal) {
                    if (!visualSummary.empty()) visualSummary += ", ";
                    visualSummary += fmt::format("{}:{}({})",
                        i, SlotVisualStateToString(assignment.visualState), assignment.name);
                }
            }
            // Dedup: only log when the visual state actually changes
            // NOTE: Single-threaded (called from update thread only)
            static std::string s_lastVisualSummary;
            if (visualSummary != s_lastVisualSummary) {
                if (!visualSummary.empty()) {
                    logger::debug("[VisualState] {}", visualSummary);
                }
                s_lastVisualSummary = visualSummary;
            }
        }
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
