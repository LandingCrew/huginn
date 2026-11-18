#pragma once

// =============================================================================
// SPELL POWER (0.24.29) -- a spell's effects as the player would cast them now
// =============================================================================
// The effect catalog reads a spell's record: Fire Shell on Self is 8 % for
// 60 s there, 20 % for 186 s on the spell's own tooltip at the player's
// skill and perks (Requiem scales magnitude and duration with them). This
// asks the game the same questions the cast does, per effect:
//   - the perk entry points Mod Spell Magnitude / Mod Spell Duration
//     (BGSEntryPoint, with the spell and the effect), as AdjustForPerks does;
//   - the school's power modifier (Fortify <School>: kAlterationPowerModifier
//     ...), on the magnitude or the duration as the effect's flags say
//     (kPowerAffectsMagnitude / kPowerAffectsDuration).
// effect/LiveValues.h reads every spell the player knows through this, and
// checks it against the game's own numbers when one runs on the player.
//
// MAIN THREAD ONLY: the entry points walk the player's perk list.
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace Huginn::Effect
{
    struct SpellEffectPower
    {
        std::uint32_t index = 0;        // its place in the item's effect list, empty slots counted
        RE::FormID effect = 0;          // the MGEF
        std::string name;
        float baseMagnitude = 0.0f;     // the record
        std::uint32_t baseDuration = 0;
        float perkMagnitude = 0.0f;     // after Mod Spell Magnitude
        float perkDuration = 0.0f;      // after Mod Spell Duration
        float powerModifier = 0.0f;     // the school's power modifier AV (percent)
        bool powerAffectsMagnitude = false;
        bool powerAffectsDuration = false;
        float magnitude = 0.0f;         // final
        float duration = 0.0f;          // final, seconds
    };

    /// Each effect of `spell` as `caster` would cast it now. MAIN THREAD ONLY.
    [[nodiscard]] std::vector<SpellEffectPower> SpellPowerOf(RE::Actor* caster, RE::MagicItem* spell);
}
