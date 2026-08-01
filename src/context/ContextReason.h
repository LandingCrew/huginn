#pragma once

#include <cstddef>
#include <cstdint>

namespace Huginn::Context
{
    // =========================================================================
    // CONTEXT REASON (architecture-critique #10)
    // =========================================================================
    // One vocabulary for "why does the current situation matter", shared by:
    //   - ContextRuleEngine::DominantReason() — derived from the SAME continuous
    //     weight curves that drive scoring (no parallel threshold pass)
    //   - CandidateBase::overrideReason — stamped by OverrideManager on the
    //     candidate it surfaced, so the display can say why it jumped the queue
    //
    // The enum carries no display text: the display layer owns the wording
    // (Display::ReasonLabel in display/ExplanationLabel.h).
    //
    // Ordering IS display priority: DominantReason() marks every reason whose
    // threshold is met and then returns the lowest enumerator among them, so
    // reordering here reorders the labels — no second list to keep in sync.
    // =========================================================================
    enum class ContextReason : uint8_t
    {
        None = 0,

        // Emergency
        CriticalHealth,

        // Environment / crosshair interaction the player is acting ON
        Underwater,
        LookingAtLock,

        // Active elemental / status damage
        OnFire,
        Poisoned,
        Diseased,
        TakingFrost,
        TakingShock,

        // Imminent physical harm
        Falling,

        // Depleted resources
        LowHealth,
        LowMagicka,
        LowStamina,
        WeaponLowCharge,
        NeedsAmmo,

        // Ambient surroundings — true for as long as you stand there, so they
        // rank below anything actively hurting or depleting you. The crafting
        // stations sit here rather than with the crosshair interactions above:
        // looking at a forge is not more urgent than bleeding out next to it.
        AllyInjured,
        AtForge,
        AtEnchanter,
        AtAlchemy,
        LookingAtOre,
        InDarkness,
        Sneaking,

        // Target / combat
        TargetUndead,
        TargetDaedra,
        TargetDragon,
        MultipleEnemies,
        EnemyCasting,

        _Count  // Sentinel for array sizing / exhaustiveness asserts — must be last
    };

    /// Number of enumerators including None, excluding the _Count sentinel.
    inline constexpr size_t CONTEXT_REASON_COUNT = static_cast<size_t>(ContextReason::_Count);
}
