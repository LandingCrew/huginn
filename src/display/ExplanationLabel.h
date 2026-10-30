#pragma once

#include "context/ContextReason.h"
#include "slot/SlotAssignment.h"
#include "slot/SlotClassifier.h"

#include <string>
#include <string_view>

namespace Huginn::Display
{
    // =========================================================================
    // EXPLANATION LABEL (architecture-critique #9/#10)
    // =========================================================================
    // The short "why is this here" subtext under a Wheeler entry.
    //
    // Wording lives here, in the display layer. R8.3: WHICH reason applies is
    // the most urgent explanation among the page's items (the largest
    // theta n cap term of each one's choice score, display/NeedReason.h;
    // PipelineCoordinator::UpdateContextReason), and it speaks only for a slot
    // whose OWN explanation is that reason (ScoredCandidate::explainedReason,
    // #64; R8.4 deleted the per-slot attribution off the context weights it
    // replaced). This file decides how to say it. Adding a reason means adding a case below.
    // =========================================================================

    /// Human-readable text for a context reason, or "" for None.
    /// Returns a view into static storage — no allocation.
    [[nodiscard]] inline constexpr std::string_view ReasonLabel(Context::ContextReason reason) noexcept
    {
        // Tripwire: a new ContextReason needs wording here and a need in
        // display/NeedReason.h.
        static_assert(Context::CONTEXT_REASON_COUNT == 29,
            "ContextReason changed — review ReasonLabel and NeedReason");

        using R = Context::ContextReason;
        switch (reason) {
            case R::CriticalHealth:  return "Critical HP";
            case R::Underwater:      return "Underwater";
            case R::LookingAtLock:   return "Lock";
            case R::AtForge:         return "At Forge";
            case R::AtEnchanter:     return "At Enchanter";
            case R::AtAlchemy:       return "At Alchemy Lab";
            case R::OnFire:          return "Fire Damage";
            case R::Poisoned:        return "Poisoned";
            case R::Diseased:        return "Diseased";
            case R::TakingFrost:     return "Frost Damage";
            case R::TakingShock:     return "Shock Damage";
            case R::Falling:         return "Falling";
            case R::LowHealth:       return "Low HP";
            case R::LowMagicka:      return "Low MP";
            case R::LowStamina:      return "Low SP";
            case R::WeaponLowCharge: return "Low Charge";
            case R::NeedsAmmo:       return "Low Ammo";
            case R::Cold:            return "Cold";
            case R::Hungry:          return "Hungry";
            case R::AllyInjured:     return "Ally Hurt";
            case R::LookingAtOre:    return "Ore Vein";
            case R::InDarkness:      return "Darkness";
            case R::Sneaking:        return "Sneaking";
            case R::TargetUndead:    return "Undead";
            case R::TargetDaedra:    return "Daedra";
            case R::TargetDragon:    return "Dragon";
            case R::MultipleEnemies: return "Outnumbered";
            case R::EnemyCasting:    return "Enemy Casting";

            // No wording, by definition. Listed rather than defaulted so the
            // compiler flags a new enumerator on top of the static_assert.
            case R::None:
            case R::_Count:
                break;
        }
        return {};
    }

    /// Derive the subtext explanation for one slot assignment.
    /// Priority: a Remembrance hold ("Swap Back") > the override's own reason >
    /// this tick's context reason, IF it is this item's own explanation >
    /// "Ally Support" (a spell for someone else) > "Favorite" > no label.
    /// @param contextReason Reason for THIS tick (DisplayContext::contextReason),
    ///        derived once by the pipeline from the page's explanations.
    [[nodiscard]] inline std::string DeriveExplanationLabel(
        const Slot::SlotAssignment& assignment,
        Context::ContextReason contextReason)
    {
        if (!assignment.HasCandidate()) {
            return {};
        }
        // Not a recommendation at all: what pressing this key just took off.
        if (assignment.IsRemembered()) {
            return "Swap Back";
        }

        const auto& candidate = assignment.candidate->candidate;
        const auto& base = Candidate::GetBase(candidate);

        // Two reasons, two burdens of proof (#64).
        //
        // An override's reason was stamped on THIS candidate by OverrideManager,
        // which surfaced it for exactly that reason — attributed by construction.
        //
        // The tick's reason was not: it is one fact about the page (the most
        // urgent explanation on it, held). It gets to speak for a slot only
        // where it is that item's own explanation (R8.3: the need behind the
        // largest term of its score); otherwise the ladder falls through, the
        // way it already does when no reason is active at all.
        if (base.overrideReason != Context::ContextReason::None) {
            if (const auto label = ReasonLabel(base.overrideReason); !label.empty()) {
                return std::string(label);
            }
        } else if (contextReason != Context::ContextReason::None &&
                   assignment.candidate->explainedReason == contextReason) {
            if (const auto label = ReasonLabel(contextReason); !label.empty()) {
                return std::string(label);
            }
        }

        // A spell or scroll for someone else (Healing Touch, Oakflesh on
        // Target) is on the page only with a follower near, so it says who it
        // is for -- on the widget and on the Wheeler entry alike (the user,
        // 2026-10-04). R11: "for someone else" is the effect catalog's
        // TargetsOthers facet (core/CapFacets.h), the same one the no_follower
        // filter reads.
        if ((std::holds_alternative<Candidate::SpellCandidate>(candidate) ||
             std::holds_alternative<Candidate::ScrollCandidate>(candidate)) &&
            Slot::SlotClassifier::SubjectOf(*assignment.candidate).facets.Has(Core::Facets::F::TargetsOthers)) {
            return "Ally Support";
        }

        // Nothing situational applies — mark the player's own picks.
        if (assignment.candidate->IsFavorited()) {
            return "Favorite";
        }

        return {};
    }

}  // namespace Huginn::Display
