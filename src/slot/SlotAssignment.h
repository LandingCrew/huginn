#pragma once

#include "learning/ScoredCandidate.h"
#include "SlotConfig.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Huginn::Slot
{
    // =============================================================================
    // ASSIGNMENT TYPE
    // =============================================================================
    // Indicates how a candidate was assigned to a slot, which affects
    // display styling and debugging.
    // =============================================================================

    enum class AssignmentType : uint8_t
    {
        Empty,      // Slot has no content
        Normal,     // Standard utility-based assignment
        Override,   // Forced by override condition (critical health, etc.)
        Wildcard,   // Exploration pick (blue styling in widget)
    };

    [[nodiscard]] inline constexpr std::string_view AssignmentTypeToString(AssignmentType t) noexcept
    {
        switch (t) {
            case AssignmentType::Empty:    return "Empty";
            case AssignmentType::Normal:   return "Normal";
            case AssignmentType::Override: return "Override";
            case AssignmentType::Wildcard: return "Wildcard";
            default:                       return "Unknown";
        }
    }

    // =============================================================================
    // VISUAL STATE
    // =============================================================================
    // Drives per-slot animation effects in the UI. Computed by the pipeline
    // based on slot lifecycle (lock state, override priority, wildcard status).
    // =============================================================================

    enum class SlotVisualState : uint8_t
    {
        Normal      = 0,  // Default - no special effect
        Confirmed   = 1,  // Re-evaluated, same item (single flash)
        Expiring    = 2,  // Lock about to expire, content will change (slow pulse)
        Override    = 3,  // Override triggered (slide + flash, highest priority)
        Wildcard    = 4   // Wildcard exploration (same visual as Override)
    };

    [[nodiscard]] inline constexpr std::string_view SlotVisualStateToString(SlotVisualState s) noexcept
    {
        switch (s) {
            case SlotVisualState::Normal:    return "Normal";
            case SlotVisualState::Confirmed: return "Confirmed";
            case SlotVisualState::Expiring:  return "Expiring";
            case SlotVisualState::Override:  return "Override";
            case SlotVisualState::Wildcard:  return "Wildcard";
            default:                         return "Unknown";
        }
    }

    // =============================================================================
    // SLOT ASSIGNMENT
    // =============================================================================
    // The result of allocating a candidate to a slot. Contains everything
    // the widget/wheeler needs to display the slot.
    // =============================================================================

    struct SlotAssignment
    {
        size_t slotIndex = 0;                       // Which slot (0 = top/first)
        AssignmentType type = AssignmentType::Empty;
        SlotClassification classification = SlotClassification::Regular;  // Slot's classification

        // The assigned candidate (nullopt if empty)
        std::optional<Scoring::ScoredCandidate> candidate;

        // Quick-access fields (cached from candidate for efficiency)
        RE::FormID formID = 0;
        std::string name;
        float utility = 0.0f;

        // Pre-computed subtext label for Wheeler display.
        // Set by pipeline (overrides, lock timers, explanations).
        // Empty string = no subtext.
        std::string subtextLabel;

        // Visual state for UI animation (computed by pipeline)
        SlotVisualState visualState = SlotVisualState::Normal;

        // =======================================================================
        // FACTORY METHODS
        // =======================================================================

        static SlotAssignment Empty(size_t index, SlotClassification classification)
        {
            return { index, AssignmentType::Empty, classification, std::nullopt, 0, "", 0.0f };
        }

        static SlotAssignment FromCandidate(
            size_t index,
            SlotClassification classification,
            const Scoring::ScoredCandidate& sc,
            AssignmentType assignType = AssignmentType::Normal)
        {
            return {
                index,
                assignType,
                classification,
                sc,
                sc.GetFormID(),
                sc.GetName(),
                sc.utility
            };
        }

        // =======================================================================
        // QUERY METHODS
        // =======================================================================

        [[nodiscard]] bool IsEmpty() const noexcept { return type == AssignmentType::Empty; }
        [[nodiscard]] bool IsOverride() const noexcept { return type == AssignmentType::Override; }
        [[nodiscard]] bool IsWildcard() const noexcept { return type == AssignmentType::Wildcard; }
        [[nodiscard]] bool HasCandidate() const noexcept { return candidate.has_value(); }
        [[nodiscard]] bool IsConfirmed() const noexcept { return visualState == SlotVisualState::Confirmed; }
        [[nodiscard]] bool IsExpiring() const noexcept { return visualState == SlotVisualState::Expiring; }

        // Get source type (Spell, Potion, etc.) - returns Spell if empty
        [[nodiscard]] Candidate::SourceType GetSourceType() const noexcept
        {
            if (candidate) {
                return candidate->GetSourceType();
            }
            return Candidate::SourceType::Spell;  // Default
        }
    };

    // =============================================================================
    // SLOT ASSIGNMENTS COLLECTION
    // =============================================================================

    using SlotAssignments = std::vector<SlotAssignment>;

}  // namespace Huginn::Slot
