#pragma once

// Combat upsert (fCombatUpsertMarginMult, fCombatUpsertCooldownSec): the one
// way a recommendation that merely scores better takes a combat-locked key.
//
// The combat freeze (0.23.37) cut page-0 changes in combat from 34-60 a minute
// to about 9 (2026-10-11 12:05-13:27 log), but made it absolute: an item far
// better than the one on a frozen key waited for the end of the fight. The
// upsert lets it through when the gap is wide.
//
// Measured against the item ON the key, never against the allocator's own
// seating: behind the locks the allocator keeps reshuffling its seats (48
// [Hold] give-ways in 200 s at margins of 0.4-0.9), and following that would
// bring the churn back.
//
// The rule, in the [Hold] log-score space s: a challenger takes the key when
//   s(challenger) - s(frozen item) > fCombatUpsertMarginMult * ln(1 + fChallengerMargin)
// (3 x ln 1.5 = 1.216 shipped: about 3.4x the utility under the R7 bridge).
// Both scores must be finite. With fChallengerMargin = 0 the barrier is 0 and
// any better challenger wins; fCombatUpsertMarginMult = 0 turns it off.
//
// Never on a key that
//   - is not combat-locked;
//   - shows an override or Remembrance ("Swap Back") item, or has an active
//     Remembrance hold of its own, shown there or not;
//   - shows a wildcard (an exploration pick scores low by design);
//   - changes this run anyway;
//   - is inside press grace: pressed, or changed, less than fPressGraceMs ago;
//   - took an upsert less than fCombatUpsertCooldownSec ago;
// and never while the Wheeler wheel is open (only urgent override entries
// write into an open wheel). The new item is combat-locked like any other.
//
// Pure: standard library only, host-tested in tests/core/CombatUpsertTests.cpp.

#include "core/SlotScoreMath.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace Huginn::Core
{
    /// What a challenger's score must clear over the frozen item's:
    /// marginMult * ln(1 + challengerMargin). +inf (off) for marginMult <= 0 or
    /// NaN, and for a NaN margin (LogHoldMargin's "nothing beats the holder").
    [[nodiscard]] inline double CombatUpsertBarrier(float marginMult, float challengerMargin) noexcept
    {
        if (!(marginMult > 0.0f) || !std::isfinite(marginMult)) {
            return std::numeric_limits<double>::infinity();
        }
        // ln m < 0 only for a margin below 0, which SlotSettings clamps away;
        // a barrier below 0 would let a worse item in.
        const double logMargin = std::max(LogHoldMargin(challengerMargin), 0.0);
        return static_cast<double>(marginMult) * logMargin;
    }

    /// The challenger clears the barrier over the frozen item. Never for a
    /// non-finite score on either side (an unranked row, a pinned sentinel)
    /// or an infinite barrier.
    [[nodiscard]] inline bool CombatUpsertBeats(SlotScore challenger, SlotScore frozen, double barrier) noexcept
    {
        if (!std::isfinite(challenger) || !std::isfinite(frozen) || !std::isfinite(barrier)) {
            return false;
        }
        return ScoreExceedsByMargin(challenger, frozen, barrier);
    }

    enum class CombatUpsertGate : uint8_t
    {
        Open,
        Off,              ///< barrier infinite (fCombatUpsertMarginMult = 0)
        NotCombatLocked,  ///< the key holds no combat lock
        WheelOpen,        ///< the Wheeler wheel is open
        Pinned,           ///< the key shows an override or Remembrance item
        RemembranceHold,  ///< an active Remembrance hold belongs to or is shown on this key
        Wildcard,         ///< the key shows a wildcard
        Changing,         ///< the key changes this run anyway
        PressGrace,       ///< pressed or changed less than fPressGraceMs ago
        Cooldown,         ///< upserted less than fCombatUpsertCooldownSec ago
    };

    struct CombatUpsertGateInput
    {
        bool enabled = false;          ///< the barrier is finite
        bool combatLocked = false;     ///< in combat and the key holds a combat lock
        bool wheelOpen = false;
        bool shownPinned = false;      ///< the key shows an override or Remembrance item
        bool remembranceHold = false;  ///< an active hold belongs to or is shown on this key
        bool shownWildcard = false;
        bool changingThisRun = false;  ///< this run's item is not what the key showed last run
        double graceMs = 0.0;          ///< fPressGraceMs; <= 0 = no grace gate
        double sincePressMs = -1.0;    ///< since the player last pressed the key; < 0 = never
        double sinceShownMs = -1.0;    ///< since the key's shown item last changed; < 0 = never
        double cooldownMs = 0.0;       ///< fCombatUpsertCooldownSec in ms; <= 0 = no cooldown
        double sinceUpsertMs = -1.0;   ///< since the key's last upsert; < 0 = never
    };

    [[nodiscard]] constexpr CombatUpsertGate DecideCombatUpsertGate(const CombatUpsertGateInput& in) noexcept
    {
        if (!in.enabled) return CombatUpsertGate::Off;
        if (!in.combatLocked) return CombatUpsertGate::NotCombatLocked;
        if (in.wheelOpen) return CombatUpsertGate::WheelOpen;
        if (in.shownPinned) return CombatUpsertGate::Pinned;
        if (in.remembranceHold) return CombatUpsertGate::RemembranceHold;
        if (in.shownWildcard) return CombatUpsertGate::Wildcard;
        if (in.changingThisRun) return CombatUpsertGate::Changing;
        if (in.graceMs > 0.0 &&
            ((in.sincePressMs >= 0.0 && in.sincePressMs < in.graceMs) ||
             (in.sinceShownMs >= 0.0 && in.sinceShownMs < in.graceMs))) {
            return CombatUpsertGate::PressGrace;
        }
        if (in.cooldownMs > 0.0 && in.sinceUpsertMs >= 0.0 && in.sinceUpsertMs < in.cooldownMs) {
            return CombatUpsertGate::Cooldown;
        }
        return CombatUpsertGate::Open;
    }

    struct CombatUpsertPick
    {
        std::size_t key = 0;          ///< index into `frozen`
        std::size_t challenger = 0;   ///< index into `challengers`
        double margin = 0.0;          ///< s(challenger) - s(frozen item)
    };

    /// Which challenger takes which key this run. `frozen[k]`: the score of the
    /// item on key k, nullopt when the key is not open to an upsert.
    /// `challengers[c]`: challenger scores. `fits[k][c]`: challenger c may go on
    /// key k (missing entries = no). Greedy on the margin, largest first (ties:
    /// lower key, then lower challenger index): one challenger per key and one
    /// key per challenger.
    [[nodiscard]] inline std::vector<CombatUpsertPick> PlanCombatUpserts(
        const std::vector<std::optional<SlotScore>>& frozen, const std::vector<SlotScore>& challengers,
        const std::vector<std::vector<bool>>& fits, double barrier)
    {
        std::vector<CombatUpsertPick> pairs;
        for (std::size_t k = 0; k < frozen.size(); ++k) {
            if (!frozen[k] || k >= fits.size()) continue;
            for (std::size_t c = 0; c < challengers.size(); ++c) {
                if (c >= fits[k].size() || !fits[k][c]) continue;
                if (!CombatUpsertBeats(challengers[c], *frozen[k], barrier)) continue;
                pairs.push_back({ k, c, challengers[c] - *frozen[k] });
            }
        }
        std::stable_sort(pairs.begin(), pairs.end(), [](const CombatUpsertPick& a, const CombatUpsertPick& b) {
            return a.margin > b.margin;
        });
        std::vector<CombatUpsertPick> picks;
        std::vector<bool> keyTaken(frozen.size(), false);
        std::vector<bool> challengerTaken(challengers.size(), false);
        for (const auto& p : pairs) {
            if (keyTaken[p.key] || challengerTaken[p.challenger]) continue;
            keyTaken[p.key] = true;
            challengerTaken[p.challenger] = true;
            picks.push_back(p);
        }
        return picks;
    }
}
