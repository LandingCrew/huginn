#pragma once

#include <algorithm>
#include <cstdint>

// =============================================================================
// SLOT CLASS CAP MATH (pure; host-tested in tests/core/SlotClassCapMathTests.cpp)
// =============================================================================
// The arithmetic behind Slot::SlotClassCap (src/slot/SlotClassCap.h), which
// owns the counting and the candidate classification. Kept here, free of game types, so
// it can be proven on the host. See src/core/README.md.
//
// On a Regular key, the first `freePerClass` items of a slot class on the page
// compete at full utility; each one past that pays x discount, x discount^2, ...
// =============================================================================

namespace Huginn::Core
{
    /// The discount the cap works with: clamped to [0, 1]. NaN stays NaN (and
    /// so reads as "off" below), as std::clamp leaves it.
    [[nodiscard]] inline float ClampClassCapDiscount(float discount) noexcept
    {
        return std::clamp(discount, 0.0f, 1.0f);
    }

    /// The cap is on only for a discount below 1. 1 (or NaN) turns it off.
    [[nodiscard]] inline bool ClassCapActive(float discount) noexcept
    {
        return ClampClassCapDiscount(discount) < 1.0f;
    }

    /// Multiplier on an item's utility when `shown` items of its slot class are
    /// already on the page: 1 while shown < freePerClass, then
    /// discount^(shown - freePerClass + 1). 1 when the cap is off.
    ///
    /// Repeated multiplication, not std::pow, so the result is bit-identical
    /// to the loop SlotClassCap::Factor (NeedCap::Factor until 0.23.8) ran
    /// before it moved here.
    ///
    /// The loop stops once one more multiplication would not change the
    /// product (factor * d == factor). From there every further step yields
    /// the same value, so stopping changes no bit. That fixed point is 0 for a
    /// small discount, but for d > 0.5 the product sticks at a nonzero
    /// denormal instead (x * 0.75 rounds back to x), which a "stop at 0" test
    /// never reached: ClassCapFactor(0.75f, 0, 1e8) ran 1e8 steps. With the
    /// fixed-point test the work is bounded by the steps to that point (about
    /// 10^5 at d = 0.999), whatever `shown` is. SlotClassCap passes a uint8
    /// count, so in the game it is at most 256 steps either way.
    ///
    /// A zero discount is answered directly: -0.0 (std::clamp keeps the sign)
    /// made the old loop alternate -0, +0, -0, ..., which never reaches a
    /// fixed point but is just d^steps -- -0 for an odd count, +0 otherwise.
    /// So the result matches the old loop bit for bit for -0.0 too, sign
    /// included.
    [[nodiscard]] inline float ClassCapFactor(float discount, std::uint32_t freePerClass, std::uint32_t shown) noexcept
    {
        const float d = ClampClassCapDiscount(discount);
        if (!(d < 1.0f)) {
            return 1.0f;
        }
        if (shown < freePerClass) {
            return 1.0f;
        }
        const std::uint64_t steps = static_cast<std::uint64_t>(shown) - freePerClass + 1;
        if (d == 0.0f) {   // +0 or -0
            return (steps % 2 == 1) ? d : 0.0f;
        }
        float factor = 1.0f;
        for (std::uint64_t i = 0; i < steps; ++i) {
            const float next = factor * d;
            if (next == factor) {
                break;   // a fixed point: every remaining step gives this value again
            }
            factor = next;
        }
        return factor;
    }
}  // namespace Huginn::Core
