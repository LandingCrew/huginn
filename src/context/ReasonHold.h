#pragma once

#include "ContextReason.h"

namespace Huginn::Context
{
    // =========================================================================
    // REASON HOLD (#62)
    // =========================================================================
    // DominantReason answers per tick. Some reasons are momentarily true — a
    // crouch, a crosshair crossing a draugr — and a reason true for 100ms still
    // repaints every label on the wheel and reverts before it can be read:
    //
    //   14:47:51 [Context] Reason: (none) → Sneaking
    //   14:47:51 [Context] Reason: Sneaking → (none)
    //
    // Both lines are CORRECT. The player really was sneaking. The problem is
    // display stability, not detection, which is why this is separate from #60
    // and why it is applied here rather than inside DominantReason: the ranking
    // must keep using the instantaneous truth, and only the label is damped.
    //
    // POLICY — asymmetric, and priority-aware:
    //
    //   * A MORE URGENT reason is adopted instantly. Critical HP must never
    //     wait behind a stale Sneaking. Priority is ContextReason's declaration
    //     order, the same ordering DominantReason resolves ties with, so there
    //     is no second notion of urgency to keep in sync.
    //
    //   * Anything else — releasing to None, or a LESS urgent reason arriving —
    //     waits out the hold. That covers both observed shapes: the X → none → X
    //     blink, and the same-band swap seen mid-fight when a crosshair drifts
    //     off a draugr onto a bandit (Undead → Outnumbered → Undead).
    //
    // Bounded by construction: the hold delays only a DOWNGRADE, never an
    // escalation, and never by more than holdMs.
    // =========================================================================

    class ReasonHold
    {
    public:
        /// @param raw     What DominantReason produced this tick.
        /// @param nowMs   Monotonic milliseconds. Wall clock rather than tick
        ///                count because the pipeline skips unchanged states —
        ///                ticks are not evenly spaced.
        /// @param holdMs  How long a reason survives after it stops being true.
        [[nodiscard]] ContextReason Update(ContextReason raw, double nowMs, float holdMs) noexcept
        {
            // Still true: refresh and keep showing it.
            if (raw == m_held) {
                m_lastTrueMs = nowMs;
                return m_held;
            }

            // Nothing held, or the newcomer outranks what is held. Lower
            // enumerator == higher priority; None is 0 and would otherwise read
            // as the most urgent of all, hence the explicit exclusion.
            if (m_held == ContextReason::None ||
                (raw != ContextReason::None && raw < m_held)) {
                m_held = raw;
                m_lastTrueMs = nowMs;
                return m_held;
            }

            // A downgrade (or a release to None): keep the old reason readable
            // until the hold expires.
            if (nowMs - m_lastTrueMs < holdMs) {
                return m_held;
            }

            m_held = raw;
            m_lastTrueMs = nowMs;
            return m_held;
        }

        /// Drop the held reason. On save load the previous character's context
        /// says nothing about this one, and the hold would otherwise survive it.
        void Reset() noexcept { *this = ReasonHold{}; }

        [[nodiscard]] ContextReason Held() const noexcept { return m_held; }

    private:
        ContextReason m_held = ContextReason::None;
        double m_lastTrueMs = 0.0;
    };
}
