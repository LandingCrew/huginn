#pragma once

#include <algorithm>
#include <cstdint>

// =============================================================================
// NEED CAP MATH (pure; host-tested in tests/core/NeedCapMathTests.cpp)
// =============================================================================
// The arithmetic behind Slot::NeedCap (src/slot/NeedCap.h), which owns the
// counting and the candidate classification. Kept here, free of game types, so
// it can be proven on the host. See src/core/README.md.
//
// On a Regular key, the first `freePerNeed` items of a need on the page compete
// at full utility; each one past that pays x discount, x discount^2, ...
// =============================================================================

namespace Huginn::Core
{
    /// The discount the cap works with: clamped to [0, 1]. NaN stays NaN (and
    /// so reads as "off" below), as std::clamp leaves it.
    [[nodiscard]] inline float ClampNeedCapDiscount(float discount) noexcept
    {
        return std::clamp(discount, 0.0f, 1.0f);
    }

    /// The cap is on only for a discount below 1. 1 (or NaN) turns it off.
    [[nodiscard]] inline bool NeedCapActive(float discount) noexcept
    {
        return ClampNeedCapDiscount(discount) < 1.0f;
    }

    /// Multiplier on an item's utility when `shown` items of its need are
    /// already on the page: 1 while shown < freePerNeed, then
    /// discount^(shown - freePerNeed + 1). 1 when the cap is off.
    ///
    /// Repeated multiplication, not std::pow, so the result is bit-identical
    /// to the loop NeedCap::Factor ran before it moved here. Stops early once
    /// the product is exactly 0, so a huge `shown` cannot spin.
    [[nodiscard]] inline float NeedCapFactor(float discount, std::uint32_t freePerNeed, std::uint32_t shown) noexcept
    {
        const float d = ClampNeedCapDiscount(discount);
        if (!(d < 1.0f)) {
            return 1.0f;
        }
        if (shown < freePerNeed) {
            return 1.0f;
        }
        const std::uint64_t steps = static_cast<std::uint64_t>(shown) - freePerNeed + 1;
        float factor = 1.0f;
        for (std::uint64_t i = 0; i < steps && factor != 0.0f; ++i) {
            factor *= d;
        }
        return factor;
    }
}  // namespace Huginn::Core
