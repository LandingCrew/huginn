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
    // The hold delays only a DOWNGRADE, never an escalation, and never by more
    // than holdMs. Both halves of that need the pipeline to keep running, and
    // the skip check bails before scoring, so IsHolding() exists for the two
    // gates to bypass on while one is pending — the same way they already do for
    // the elemental window and for a fall. Keep them wired together (see
    // PipelineCoordinator::NeedsForcedRun) or the guarantee is a class-level
    // truth the system does not honour.
    //
    // TIMED FROM THE DEPARTURE, NOT FROM THE LAST SIGHTING. The first draft
    // refreshed a "last true" stamp on every tick the reason still held, and
    // measured the hold from that. It reads as equivalent and is not: that stamp
    // only advances on a tick the pipeline actually RUNS, and a reason that
    // stays true is precisely what lets the pipeline go quiet. Observed
    // 2026-08-06 — a six-second crouch in a still room left the stamp frozen at
    // the crouch, so standing up was already past holdMs and the label dropped
    // on the same tick (1ms, against a 1000ms hold):
    //
    //   19:16:21.243 [Pipeline] Sneak:Sneaking→Standing
    //   19:16:21.244 [Context] Reason: Sneaking → (none)
    //
    // The blink case survived that — both its edges move the hash, so both ticks
    // run — but the same-band swap did not: dwell on a draugr longer than holdMs
    // and the Undead → Outnumbered → Undead flicker comes straight back. Anchor
    // on the tick the reason DEPARTS instead. That tick always runs, because
    // whatever made the reason change is what opened the gate.
    // =========================================================================

    class ReasonHold
    {
    public:
        /// @param raw     What DominantReason produced this tick.
        /// @param nowMs   Monotonic milliseconds. Wall clock rather than tick
        ///                count because the pipeline skips unchanged states —
        ///                ticks are not evenly spaced.
        /// @param holdMs  How long a reason survives after it stops being true.
        ///                Zero is a real setting, not a degenerate one: the
        ///                caller clamps to the slot lock duration, which the INI
        ///                can disable. It must make the hold transparent.
        [[nodiscard]] ContextReason Update(ContextReason raw, double nowMs, float holdMs) noexcept
        {
            // Still true: keep showing it, and cancel any pending downgrade.
            // Nothing is stamped here — see the class note on why a "last seen
            // true" stamp cannot be trusted to advance.
            if (raw == m_held) {
                m_holding = false;
                return m_held;
            }

            // Nothing held, or the newcomer outranks what is held. Lower
            // enumerator == higher priority; None is 0 and would otherwise read
            // as the most urgent of all, hence the explicit exclusion.
            if (m_held == ContextReason::None ||
                (raw != ContextReason::None && raw < m_held)) {
                m_held = raw;
                m_holding = false;
                return m_held;
            }

            // A downgrade (or a release to None): keep the old reason readable
            // until the hold expires, counted from the tick it departed.
            if (!m_holding) {
                m_holding = true;
                m_departedMs = nowMs;
            }
            // Deliberately not `else`: with holdMs == 0 the stamp above and this
            // test happen on the same tick, and the downgrade must land on it.
            // The stamp also survives the raw reason changing again mid-hold —
            // the clock measures how long m_held has been stale, so an
            // oscillating raw reason cannot keep extending it.
            if (nowMs - m_departedMs < holdMs) {
                return m_held;
            }

            m_held = raw;
            m_holding = false;
            return m_held;
        }

        /// Drop the held reason. On save load the previous character's context
        /// says nothing about this one, and the hold would otherwise survive it.
        void Reset() noexcept { *this = ReasonHold{}; }

        [[nodiscard]] ContextReason Held() const noexcept { return m_held; }

        /// True when the last Update returned a reason that is no longer raw-true
        /// — i.e. a downgrade is pending. The pipeline must keep running while
        /// this holds, or the expiry never gets a tick to land on; see the note
        /// on the class.
        [[nodiscard]] bool IsHolding() const noexcept { return m_holding; }

    private:
        ContextReason m_held = ContextReason::None;
        // When m_held stopped being raw-true. Meaningful only while m_holding.
        double m_departedMs = 0.0;
        bool m_holding = false;
    };
}
