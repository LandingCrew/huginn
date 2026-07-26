#pragma once

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
    // Ordering is display priority — DominantReason() reports the FIRST reason
    // whose weight clears its threshold, so more urgent reasons must come first.
    // =========================================================================
    enum class ContextReason : uint8_t
    {
        None = 0,

        // Emergency
        CriticalHealth,

        // Environment / crosshair interaction
        Underwater,
        LookingAtLock,
        AtForge,
        AtEnchanter,
        AtAlchemy,

        // Active elemental / status damage
        OnFire,
        Poisoned,
        Diseased,
        TakingFrost,
        TakingShock,
        Falling,

        // Depleted resources
        LowHealth,
        LowMagicka,
        LowStamina,
        WeaponLowCharge,
        NeedsAmmo,

        // Surroundings
        AllyInjured,
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
