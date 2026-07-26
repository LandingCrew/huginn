#pragma once

#include "context/ContextReason.h"
#include "slot/SlotAssignment.h"

#include <string>
#include <string_view>

namespace Huginn::Display
{
    // =========================================================================
    // EXPLANATION LABEL (architecture-critique #9/#10)
    // =========================================================================
    // The short "why is this here" subtext under a Wheeler entry.
    //
    // Wording lives here, in the display layer — the context layer decides WHICH
    // reason applies (Context::ContextRuleEngine::DominantReason, derived from
    // the same weight curves that drive scoring); this file decides how to say
    // it. Adding a reason means adding a case below.
    // =========================================================================

    /// Human-readable text for a context reason, or "" for None.
    /// Returns a view into static storage — no allocation.
    [[nodiscard]] inline constexpr std::string_view ReasonLabel(Context::ContextReason reason) noexcept
    {
        // Tripwire: a new ContextReason needs wording here and a threshold in
        // ContextRuleEngine::DominantReason.
        static_assert(Context::CONTEXT_REASON_COUNT == 27,
            "ContextReason changed — review ReasonLabel and DominantReason");

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
    /// Priority: the override's own reason > this tick's context reason >
    /// "Favorite" > no label.
    /// @param contextReason Reason for THIS tick (DisplayContext::contextReason),
    ///        derived once by the pipeline from the tick's context weights.
    [[nodiscard]] inline std::string DeriveExplanationLabel(
        const Slot::SlotAssignment& assignment,
        Context::ContextReason contextReason)
    {
        if (!assignment.HasCandidate()) {
            return {};
        }

        // Override candidates were surfaced FOR a specific reason (OverrideManager
        // stamps it); prefer that over the ambient one. Regular candidates carry
        // None and fall back to the tick's context reason.
        const auto& base = Candidate::GetBase(assignment.candidate->candidate);
        const auto reason = base.overrideReason != Context::ContextReason::None
            ? base.overrideReason
            : contextReason;

        if (const auto label = ReasonLabel(reason); !label.empty()) {
            return std::string(label);
        }

        // Nothing situational applies — mark the player's own picks.
        if (assignment.candidate->IsFavorited()) {
            return "Favorite";
        }

        return {};
    }

}  // namespace Huginn::Display
