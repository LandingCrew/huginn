#pragma once

// =============================================================================
// SELECTION GATE -- a pending selection waits for its parked equip (0.24.26)
// =============================================================================
// A Huginn key's selection is opened at the press (SelectionTracker::Select)
// and confirms when the item is still equipped SELECTION_CONFIRM_MS later.
// Since the hand equip gate (core/HandEquipGate.h) an equip into a hand may
// wait up to 3 s for a cast to be released -- as long as that window -- so
// the tracker could decide before the equip existed: a misclick window cut
// short by the wait, "not confirmed" for an equip that landed late, and a
// phantom confirm when the other hand already held the same spell (the left
// hand charging Fireball, Fireball pressed for the right: still equipped).
//
// So while a gate request for the form is parked (or acting), the record is
// held: no confirm, no expiry, no count-drop confirm. Holds are counted --
// a second press of the same item holds it again before the first lets go
// -- and when the last one is released by an equip that went through, the
// deadline is re-timed from that moment (RetimedDeadline). A release by a
// drop does not re-time: the gate withdraws the record then anyway, or it
// belongs to another press.
//
// Milliseconds on the caller's clock (the tracker's steady clock).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <algorithm>

namespace Huginn::Core
{
    class SelectionGate
    {
    public:
        void Hold() noexcept { ++m_holds; }

        /// Release one hold; true when it was the last. A release with no hold
        /// left (the record was re-created since its hold) does nothing.
        bool Release() noexcept
        {
            if (m_holds <= 0) return false;
            return --m_holds == 0;
        }

        [[nodiscard]] bool Held() const noexcept { return m_holds > 0; }

        /// The deadline once the gate lets go: the full window from the act,
        /// never earlier than the deadline the press set.
        [[nodiscard]] static constexpr double RetimedDeadline(double deadlineMs, double actMs, double windowMs) noexcept
        {
            return std::max(deadlineMs, actMs + windowMs);
        }

    private:
        int m_holds = 0;
    };
}
