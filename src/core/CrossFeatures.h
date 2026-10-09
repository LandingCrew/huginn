#pragma once

// =============================================================================
// CROSS FEATURES -- the runtime columns of cap(i)
// =============================================================================
// Five effects.csv columns depend on the item AND the player's state right
// now, so they cannot sit in the static catalog (implementation map, Phase 1):
//
//   overshoot_health/_magicka/_stamina  how far a restore overshoots the gap
//   weapon_charge                        an enchanted weapon's charge left
//   stack_count                          how many the player carries
//   ammo_matches_launcher                arrows with a bow, bolts with a crossbow
//   school_fortified                     the spell's school has an active Fortify
//
// The game side (src/effect/CrossFeatures.*) reads the state and calls these.
// R2 computes them on demand (`hg cap`); nothing reads them for scoring yet.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Huginn::Core::Effect
{
    /// clamp((restored - deficit) / max, -1, 1). `restored` is the item's total
    /// (magnitude x max(duration, 1)); a full-restore sentinel restores `max`.
    /// 0 when the vital's maximum is unknown (<= 0).
    [[nodiscard]] inline float Overshoot(float restored, bool fullRestore, float deficit, float maxValue) noexcept
    {
        if (!(maxValue > 0.0f)) return 0.0f;
        const float amount = fullRestore ? maxValue : std::max(restored, 0.0f);
        const float d = std::clamp(deficit, 0.0f, maxValue);
        return std::clamp((amount - d) / maxValue, -1.0f, 1.0f);
    }

    /// Current / max charge, in [0, 1]; 0 for an unenchanted item (max <= 0).
    [[nodiscard]] inline float WeaponCharge(float current, float maxCharge) noexcept
    {
        if (!(maxCharge > 0.0f)) return 0.0f;
        return std::clamp(current / maxCharge, 0.0f, 1.0f);
    }

    /// log1p(count) / log1p(20), saturating at 1 (20 or more carried).
    [[nodiscard]] inline float StackCount(std::int32_t count) noexcept
    {
        if (count <= 0) return 0.0f;
        return std::min(1.0f, static_cast<float>(std::log1p(static_cast<double>(count)) / std::log1p(20.0)));
    }

    enum class Launcher : std::uint8_t { None, Bow, Crossbow };

    /// 1 when the ammo fits the launcher in hand: arrows with a bow, bolts with
    /// a crossbow.
    [[nodiscard]] inline float AmmoMatchesLauncher(bool isBolt, Launcher inHand) noexcept
    {
        if (inHand == Launcher::Bow) return isBolt ? 0.0f : 1.0f;
        if (inHand == Launcher::Crossbow) return isBolt ? 1.0f : 0.0f;
        return 0.0f;
    }

    /// Bit per school (core/EffectMapper.h School: Alteration = 1 ...
    /// Restoration = 5) of the Fortify <school> effects active on the player.
    using SchoolMask = std::uint8_t;

    [[nodiscard]] constexpr SchoolMask SchoolBit(std::uint8_t school) noexcept
    {
        return school == 0 || school > 7 ? SchoolMask{ 0 } : static_cast<SchoolMask>(1u << school);
    }

    /// 1 when the item's school has an active Fortify <school> on the player.
    [[nodiscard]] inline float SchoolFortified(std::uint8_t itemSchool, SchoolMask fortified) noexcept
    {
        return (SchoolBit(itemSchool) & fortified) != 0 ? 1.0f : 0.0f;
    }
}
