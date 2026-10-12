#pragma once

// =============================================================================
// WHEEL ATTRIBUTION HOLD -- an outside pick that may be Huginn's own wheel
// =============================================================================
// Wheeler equips the item BEFORE it calls Huginn back, so a pick off Huginn's
// own wheel reaches SelectionTracker first as an outside selection
// (via "Wheeler wheel": a wheel was open and nothing else explained it), and
// its own Wheeler selection lands a few milliseconds later (LoreRim
// 2026-10-11 18:30:10: equip event .273, Pending .276, callback .277). The
// tracker has relabelled that pick since 2026-10-02, and confirmation only
// ever saw the relabelled source -- but the Pending line was written first,
// as src=External, and a consumable whose count drop was read before the
// callback could have confirmed as an outside pick.
//
// The hold closes both: a pending selection that might still turn out to be
// Huginn's (an outside pick via "Wheeler wheel") is held for HOLD_MS. While
// held it is not announced and not confirmed. Its Huginn selection arriving
// inside the window announces it once, under Huginn's label; the window
// running out announces it as the outside pick it is (one of the player's own
// wheels). A count drop during the hold is remembered and confirms when the
// hold resolves, either way.
//
// One per pending selection. Times are milliseconds on any steady clock the
// caller likes. Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstdint>
#include <string_view>

namespace Huginn::Core
{
    class WheelAttributionHold
    {
    public:
        /// Wide enough for the measured gap (1-6 ms; 0.85 s is the late potion
        /// equip of a KEY press, which never comes this way), short enough that
        /// an own-wheel pick's Pending line is not noticeably late.
        static constexpr std::int64_t HOLD_MS = 200;

        /// The via PlayerInputGate gives an outside equip made while any
        /// Wheeler wheel is open and nothing else explains it.
        static constexpr std::string_view kAnyWheelVia = "Wheeler wheel";

        /// Should an outside pick that came `via` this way be held?
        [[nodiscard]] static constexpr bool ShouldHold(bool outside, std::string_view via) noexcept
        {
            return outside && via == kAnyWheelVia;
        }

        /// What the caller does now.
        enum class Action : std::uint8_t
        {
            None,              // nothing (still held, or not held at all)
            Announce,          // write the Pending line under the current label
            AnnounceConfirm,   // write it, then confirm (a count drop came during the hold)
            Confirm,           // confirm now (not held: the ordinary path)
            Defer,             // a count drop during the hold: confirm when it resolves
        };

        WheelAttributionHold() = default;

        /// A new pending selection. Held when ShouldHold; else announced now.
        [[nodiscard]] Action Begin(std::int64_t nowMs, bool outside, std::string_view via) noexcept
        {
            held_ = ShouldHold(outside, via);
            consumed_ = false;
            releaseAtMs_ = nowMs + HOLD_MS;
            return held_ ? Action::None : Action::Announce;
        }

        [[nodiscard]] bool Held() const noexcept { return held_; }

        /// Huginn's own selection of the same item arrived (it is relabelled).
        /// Held: announce it now, under Huginn's label. Not held: nothing here
        /// (the caller logs the late relabel as before).
        [[nodiscard]] Action OnHuginnSelect() noexcept { return Resolve(); }

        /// The item's count dropped for real. Held: remember it. Else confirm.
        [[nodiscard]] Action OnConsumed() noexcept
        {
            if (!held_) return Action::Confirm;
            consumed_ = true;
            return Action::Defer;
        }

        /// Per tick: the window ran out, so it is the outside pick it looked like.
        [[nodiscard]] Action Tick(std::int64_t nowMs) noexcept
        {
            if (!held_ || nowMs < releaseAtMs_) return Action::None;
            return Resolve();
        }

    private:
        Action Resolve() noexcept
        {
            if (!held_) return Action::None;
            held_ = false;
            return consumed_ ? Action::AnnounceConfirm : Action::Announce;
        }

        bool held_ = false;
        bool consumed_ = false;
        std::int64_t releaseAtMs_ = 0;
    };
}
