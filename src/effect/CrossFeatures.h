#pragma once

// =============================================================================
// RUNTIME CROSS-FEATURES -- the five cap(i) columns that need the live state
// =============================================================================
// overshoot_health/_magicka/_stamina, weapon_charge, stack_count,
// ammo_matches_launcher, school_fortified (effects.csv). The math is in
// core/CrossFeatures.h (host-tested); this file only gathers its inputs from
// the StateManager's player snapshot and the inventory stack.
//
// R2 wires them as far as `hg cap` (nothing reads them for scoring). The
// selection log (R4) and the scorer (R8) will call Compute per tick for the
// candidates they hold; the implementation map's Phase 1 names this file.
// =============================================================================

#include "effect/EffectCatalog.h"
#include "state/PlayerActorState.h"

namespace Huginn::Effect
{
    struct RuntimeFeatures
    {
        float overshootHealth = 0.0f;
        float overshootMagicka = 0.0f;
        float overshootStamina = 0.0f;
        float weaponCharge = 0.0f;
        float stackCount = 0.0f;
        float ammoMatchesLauncher = 0.0f;
        float schoolFortified = 0.0f;
    };

    struct StackInfo
    {
        std::int32_t count = 0;
        float charge = 0.0f;     // current enchantment charge (ExtraCharge), if any
        float maxCharge = 0.0f;  // the enchantment's capacity
    };

    /// The runtime columns of one item for the player's state right now.
    [[nodiscard]] RuntimeFeatures ComputeRuntimeFeatures(const CatalogEntry& item, const State::PlayerActorState& player,
                                                         const StackInfo& stack);
}
