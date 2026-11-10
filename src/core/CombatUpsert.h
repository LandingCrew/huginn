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
// The rule (0.24.13: R9's Bayesian challenger rule, core/ChallengerRule.h, at
// a stricter confidence). When the scorer's beliefs cover the pair (the choice
// ranking, both rows of this run's table), a challenger takes the key when
//   P(challenger better) > fCombatChallengerConfidence
//   <=>  s(challenger) - s(frozen item) > ln m + z_combat sigma_Delta,
// z_combat = Phi^-1(fCombatChallengerConfidence) (0.995: z 2.576), ln m =
// fChallengerMinGap -- the hold's own rule, with the combat confidence. The
// shipped 0.995 keeps the old ratio: the fixed-margin upsert's barrier was 3x
// the calm hold's margin (3 x ln 1.5 against ln 1.5), and 3 x z(0.8) = 2.525 =
// z(0.9942); 0.995 is the nearest round value (z 3.06x the calm one).
// Where no belief covers the pair -- the FALLBACK ranking (no bootstrap, no
// sigma), or a frozen item this run did not score (its row is an older run's)
// -- the fixed barrier, as the hold falls back to the fixed margin:
//   s(challenger) - s(frozen item) > fCombatUpsertMarginMult * ln(1 + fChallengerMargin)
// (3 x ln 1.5 = 1.216 shipped: about 3.4x the utility under the R7 bridge).
// Both scores must be finite. With fChallengerMargin = 0 the barrier is 0 and
// any better challenger wins; fCombatUpsertMarginMult = 0 turns the upsert off
// in both rankings (the barrier is infinite and the planner plans nothing).
//
// No swap-back on fixed beliefs, as for the hold: both lines are >= 0 and the
// belief line is symmetric in the pair, so the item an upsert displaced could
// not take the key back even without CombatUpsertMemory, which bars it anyway.
//
// Stale keys (0.24.29, bCombatUpsertStaleKeys). The freeze keeps every key on
// what it held when the fight began, and with sd ~2 on both sides the combat
// line asks a lead of about 7: on 2026-11-09 Drop Zone (ranked 124th) and a
// weapon ranked 167th sat on keys through a fire fight while the page the
// ranking alone builds (core/SlotAllocStateless.h, the memoryless page) put a
// Resist Fire potion on key 7. A key whose item that page no longer shows is
// stale: the player is not reaching for it, so a change there costs less than
// on a key the ranking still wants. A stale key is asked at a looser line --
// z = Phi^-1(fCombatStaleConfidence) (0.95: z 1.645), the fixed barrier
// 2 x ln(1 + fChallengerMargin) -- never above the combat line. Every other
// rule here (the gate, the page spacing, the cooldown, no swap-back) holds.
// The caller (SlotLocker) also requires the item rank past iCombatStaleRank,
// and never marks stale a key that took a stale upsert this fight or that the
// player pressed this fight on the item it shows: the first version (the
// calm 0.8 line, no such limits) made 21 changes in 2 min 10 s in game, items
// it brought in going stale and out again, and swapped out a Fireball the
// player had just cast.
//
// Which challengers are asked (bTestAllChallengers, shared with the hold):
// 0 (default) asks each key's BEST fitting challenger only; 1 asks them in
// rank order and the first that clears the line takes the key. Under the
// fixed barrier the two are the same (a lower score cannot clear a line the
// best one missed), which is why the fallback path is unchanged either way.
//
// Never on a key that
//   - is not combat-locked;
//   - shows an override or Remembrance ("Swap Back") item, or has an active
//     Remembrance hold of its own, shown there or not;
//   - the allocator wants for an override or Remembrance item this run (it is
//     about to take the key: an upsert first would be two changes);
//   - shows a wildcard (an exploration pick scores low by design);
//   - is inside press grace: changed less than fPressGraceMs ago, or pressed
//     less than max(fPressGraceMs, the press floor) ago -- the floor covers
//     the double-tap window and the hold threshold, so a pending tap or hold
//     never acts on an upsert even with fPressGraceMs = 0;
//   - took an upsert less than fCombatUpsertCooldownSec ago;
// and never while the Wheeler wheel is open (only urgent override entries
// write into an open wheel). The new item is combat-locked like any other.
//
// Never a challenger: an item an upsert displaced this fight, or one that
// left any key less than max(fCombatUpsertCooldownSec, the page spacing) ago
// (CombatUpsertMemory), or one leaving a key this very run. Without that a
// displaced item could upsert the key it "belongs" on -- the home-key move the
// combat lock defers to the end of the fight, back as two changes (verifier,
// PR #208). And at most one upsert per page per run, kCombatUpsertPageSpacingMs
// apart; none in a run where any key of the page changes (one visible change
// per run -- the caller checks it before the per-key gate, which is why the
// gate has no "changing" case); and none less than max(fPressGraceMs, the
// press floor) after the Wheeler wheel closes (WheelClosedRecently): the
// widget comes back then and the player reaches for a key.
//
// Pure (the standard library and core/ChallengerRule.h), host-tested in
// tests/core/CombatUpsertTests.cpp.

#include "core/ChallengerRule.h"
#include "core/SlotScoreMath.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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

    /// At most one upsert per page per this long (and one per run). Several
    /// keys can clear the barrier on the same run -- a context flip re-scores
    /// the page at once -- and each arrival is a change the player has to read;
    /// 3 s spaces them past the 600 ms press grace and the 300 ms double-tap
    /// window with room to look. The replay of the 2026-10-11 log puts the
    /// rate near two a minute, so the spacing bites only on such bursts.
    inline constexpr double kCombatUpsertPageSpacingMs = 3000.0;

    /// The shortest a press keeps its key from an upsert, whatever
    /// fPressGraceMs says (0 included): the longer of the input handler's
    /// default double-tap window (300 ms, from key up) and hold threshold
    /// (400 ms, from key down), plus one 100 ms update tick. The input handler
    /// asks for longer when its own settings are longer (SlotLocker::NoteKeyPress).
    inline constexpr double kCombatUpsertPressFloorMs = 500.0;

    enum class CombatUpsertGate : uint8_t
    {
        Open,
        Off,              ///< barrier infinite (fCombatUpsertMarginMult = 0)
        NotCombatLocked,  ///< the key holds no combat lock
        WheelOpen,        ///< the Wheeler wheel is open
        Pinned,           ///< the key shows an override or Remembrance item
        WantedPinned,     ///< the allocator wants an override or Remembrance item on this key
        RemembranceHold,  ///< an active Remembrance hold belongs to or is shown on this key
        Wildcard,         ///< the key shows a wildcard
        PressGrace,       ///< pressed or changed too recently
        Cooldown,         ///< upserted less than fCombatUpsertCooldownSec ago
    };

    struct CombatUpsertGateInput
    {
        bool enabled = false;          ///< the barrier is finite
        bool combatLocked = false;     ///< in combat and the key holds a combat lock
        bool wheelOpen = false;
        bool shownPinned = false;      ///< the key shows an override or Remembrance item
        bool wantedPinned = false;     ///< this run's allocator pick for the key is an override / Remembrance item
        bool remembranceHold = false;  ///< an active hold belongs to or is shown on this key
        bool shownWildcard = false;
        double graceMs = 0.0;          ///< fPressGraceMs; <= 0 = no grace on a change
        double pressFloorMs = kCombatUpsertPressFloorMs;  ///< a press holds the key at least this long
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
        if (in.wantedPinned) return CombatUpsertGate::WantedPinned;
        if (in.remembranceHold) return CombatUpsertGate::RemembranceHold;
        if (in.shownWildcard) return CombatUpsertGate::Wildcard;
        const double pressMs = in.graceMs > in.pressFloorMs ? in.graceMs : in.pressFloorMs;
        if ((in.sincePressMs >= 0.0 && in.sincePressMs < pressMs) ||
            (in.graceMs > 0.0 && in.sinceShownMs >= 0.0 && in.sinceShownMs < in.graceMs)) {
            return CombatUpsertGate::PressGrace;
        }
        if (in.cooldownMs > 0.0 && in.sinceUpsertMs >= 0.0 && in.sinceUpsertMs < in.cooldownMs) {
            return CombatUpsertGate::Cooldown;
        }
        return CombatUpsertGate::Open;
    }

    /// No upsert this soon after the Wheeler wheel closed: the widget is back
    /// and the player reaches for a key, so a challenger that waited behind
    /// the open wheel must not land in the first run after it. `sinceCloseMs`
    /// < 0 = the wheel has not closed (since the last reset).
    [[nodiscard]] constexpr bool WheelClosedRecently(double sinceCloseMs, double graceMs) noexcept
    {
        const double holdMs = graceMs > kCombatUpsertPressFloorMs ? graceMs : kCombatUpsertPressFloorMs;
        return sinceCloseMs >= 0.0 && sinceCloseMs < holdMs;
    }

    /// The combat upsert's line (0.24.13). `barrier`: the fixed barrier
    /// (CombatUpsertBarrier), for a pair no belief covers; infinite or NaN =
    /// the upsert is off (fCombatUpsertMarginMult = 0), whatever the beliefs.
    /// `minGap` = ln m (fChallengerMinGap, clamped by Challenger::ClampMinGap),
    /// `z` = Phi^-1(fCombatChallengerConfidence) (Challenger::ZForConfidence),
    /// both >= 0. `testAll` = bTestAllChallengers.
    struct CombatUpsertRule
    {
        double barrier = std::numeric_limits<double>::infinity();
        double minGap = 0.0;
        double z = 0.0;
        bool testAll = false;
        /// 0.24.29: staleKeys[k] = key k's item is not on the memoryless page
        /// (missing entries = not stale); such a key is asked at zStale and
        /// barrierStale, each capped at the combat line's.
        std::vector<bool> staleKeys;
        double zStale = 0.0;
        double barrierStale = std::numeric_limits<double>::infinity();

        /// The line key k is asked at: the combat line, or the stale one for a stale key.
        [[nodiscard]] CombatUpsertRule ForKey(std::size_t k) const
        {
            CombatUpsertRule r{ barrier, minGap, z, testAll, {}, 0.0, std::numeric_limits<double>::infinity() };
            if (k < staleKeys.size() && staleKeys[k]) {
                if (zStale >= 0.0 && zStale < r.z) r.z = zStale;
                if (barrierStale >= 0.0 && barrierStale < r.barrier) r.barrier = barrierStale;
            }
            return r;
        }
        [[nodiscard]] bool Stale(std::size_t k) const noexcept { return k < staleKeys.size() && staleKeys[k]; }
    };

    /// One pair under the rule. `sigmaDelta`: the pair's sigma_Delta; NaN or
    /// negative = no belief covers the pair, and the fixed barrier decides
    /// (`*sdOut` = -1); else the challenger rule at the combat confidence
    /// (`*sdOut` = sigma_Delta). Never for a non-finite score on either side.
    [[nodiscard]] inline bool CombatUpsertTakes(SlotScore challenger, SlotScore frozen, double sigmaDelta,
        const CombatUpsertRule& rule, double* sdOut = nullptr) noexcept
    {
        if (sdOut) *sdOut = -1.0;
        if (!std::isfinite(challenger) || !std::isfinite(frozen)) return false;
        if (!(sigmaDelta >= 0.0)) {   // NaN or negative: no belief
            return CombatUpsertBeats(challenger, frozen, rule.barrier);
        }
        if (sdOut) *sdOut = sigmaDelta;
        return Challenger::Exceeds(challenger, frozen, sigmaDelta, rule.minGap, rule.z);
    }

    struct CombatUpsertPick
    {
        std::size_t key = 0;          ///< index into `frozen`
        std::size_t challenger = 0;   ///< index into `challengers`
        double margin = 0.0;          ///< s(challenger) - s(frozen item)
        double sd = -1.0;             ///< sigma_Delta when the challenger rule decided; < 0 = the fixed barrier
        std::size_t passedOver = 0;   ///< higher-ranked challengers for the key that missed the line (testAll)
        bool stale = false;           ///< 0.24.29: the key was stale, asked at the stale line
    };

    /// Which challenger takes which key this run. `frozen[k]`: the score of the
    /// item on key k, nullopt when the key is not open to an upsert.
    /// `challengers[c]`: challenger scores. `fits[k][c]`: challenger c may go on
    /// key k (missing entries = no). `sigma[k][c]`: the pair's sigma_Delta
    /// (missing or NaN = no belief: the fixed barrier).
    ///
    /// For each open key, its challengers in rank order (score, highest first;
    /// ties: lower index), skipping those that do not fit or are already
    /// placed: with rule.testAll off only the first is asked; on, the first
    /// that clears the line is the key's pick. Greedy over the keys' picks on
    /// the margin, largest first (ties: lower key): one challenger per key and
    /// one key per challenger, at most `maxPicks` in all (the game passes 1:
    /// one upsert per page per run). Under the fixed barrier alone this is the
    /// pre-0.24.13 planner (every passing pair, greedy on the margin): a key's
    /// best challenger has its widest margin, and a lower one cannot clear a
    /// barrier the best one missed.
    [[nodiscard]] inline std::vector<CombatUpsertPick> PlanCombatUpserts(
        const std::vector<std::optional<SlotScore>>& frozen, const std::vector<SlotScore>& challengers,
        const std::vector<std::vector<bool>>& fits, const std::vector<std::vector<double>>& sigma,
        const CombatUpsertRule& rule, std::size_t maxPicks = std::numeric_limits<std::size_t>::max())
    {
        std::vector<CombatUpsertPick> picks;
        if (!std::isfinite(rule.barrier)) return picks;   // off
        std::vector<std::size_t> order;
        for (std::size_t c = 0; c < challengers.size(); ++c) {
            if (std::isfinite(challengers[c])) order.push_back(c);
        }
        std::stable_sort(order.begin(), order.end(),
            [&challengers](std::size_t a, std::size_t b) { return challengers[a] > challengers[b]; });
        auto sigmaOf = [&sigma](std::size_t k, std::size_t c) {
            return k < sigma.size() && c < sigma[k].size() ? sigma[k][c] : std::numeric_limits<double>::quiet_NaN();
        };
        std::vector<bool> keyTaken(frozen.size(), false);
        std::vector<bool> challengerTaken(challengers.size(), false);
        while (picks.size() < maxPicks) {
            std::optional<CombatUpsertPick> best;
            for (std::size_t k = 0; k < frozen.size(); ++k) {
                if (keyTaken[k] || !frozen[k] || k >= fits.size()) continue;
                const CombatUpsertRule keyRule = rule.ForKey(k);
                std::size_t passed = 0;
                for (const std::size_t c : order) {
                    if (challengerTaken[c] || c >= fits[k].size() || !fits[k][c]) continue;
                    double sd = -1.0;
                    if (CombatUpsertTakes(challengers[c], *frozen[k], sigmaOf(k, c), keyRule, &sd)) {
                        const double margin = challengers[c] - *frozen[k];
                        if (!best || margin > best->margin) {
                            best = CombatUpsertPick{ k, c, margin, sd, passed, rule.Stale(k) };
                        }
                        break;
                    }
                    if (!rule.testAll) break;   // the best challenger only
                    ++passed;
                }
            }
            if (!best) break;
            keyTaken[best->key] = true;
            challengerTaken[best->challenger] = true;
            picks.push_back(*best);
        }
        return picks;
    }

    /// The fixed barrier alone (no beliefs, best challenger only): the
    /// fallback ranking's planner, and the pre-0.24.13 one.
    [[nodiscard]] inline std::vector<CombatUpsertPick> PlanCombatUpserts(
        const std::vector<std::optional<SlotScore>>& frozen, const std::vector<SlotScore>& challengers,
        const std::vector<std::vector<bool>>& fits, double barrier,
        std::size_t maxPicks = std::numeric_limits<std::size_t>::max())
    {
        CombatUpsertRule rule;
        rule.barrier = barrier;
        return PlanCombatUpserts(frozen, challengers, fits, {}, rule, maxPicks);
    }

    /// An item as the upsert compares them: the same form and stack, or the
    /// same display name (as post-lock dedup does).
    struct UpsertItem
    {
        uint32_t formID = 0;
        uint16_t uniqueID = 0;
        std::string name;

        [[nodiscard]] bool Same(uint32_t f, uint16_t u, std::string_view n) const noexcept
        {
            return (formID == f && uniqueID == u) || (!name.empty() && name == n);
        }
    };

    /// What the upsert must remember between runs, page-wide: the items that
    /// recently left a key (and which of them an upsert displaced), and when
    /// the page last took an upsert. Times are ms on one monotonic clock.
    class CombatUpsertMemory
    {
    public:
        /// `item` stopped showing on a key at `nowMs`; `byUpsert`: an upsert
        /// replaced it.
        void NoteLeft(UpsertItem item, double nowMs, bool byUpsert)
        {
            for (auto& d : m_left) {
                if (d.item.Same(item.formID, item.uniqueID, item.name)) {
                    d.atMs = nowMs;
                    d.byUpsert = d.byUpsert || byUpsert;
                    return;
                }
            }
            m_left.push_back({ std::move(item), nowMs, byUpsert });
        }

        /// Not a challenger: displaced by an upsert this fight, or left a key
        /// less than `recentMs` ago.
        [[nodiscard]] bool Excluded(uint32_t formID, uint16_t uniqueID, std::string_view name, double nowMs,
            double recentMs) const noexcept
        {
            for (const auto& d : m_left) {
                if (!d.item.Same(formID, uniqueID, name)) continue;
                if (d.byUpsert || nowMs - d.atMs < recentMs) return true;
            }
            return false;
        }

        /// The page may take an upsert: none yet, or the last one at least
        /// `spacingMs` ago.
        [[nodiscard]] bool PageOpen(double nowMs, double spacingMs) const noexcept
        {
            return !m_hasUpsert || nowMs - m_lastUpsertMs >= spacingMs;
        }

        void NoteUpsert(double nowMs) noexcept
        {
            m_hasUpsert = true;
            m_lastUpsertMs = nowMs;
        }

        /// Drop departures older than `recentMs` that no upsert caused.
        void Prune(double nowMs, double recentMs)
        {
            std::erase_if(m_left, [&](const Left& d) { return !d.byUpsert && nowMs - d.atMs >= recentMs; });
        }

        /// Combat over: what upserts displaced may come back.
        void EndFight()
        {
            std::erase_if(m_left, [](const Left& d) { return d.byUpsert; });
            m_hasUpsert = false;
        }

        void Clear()
        {
            m_left.clear();
            m_hasUpsert = false;
        }

        [[nodiscard]] std::size_t Size() const noexcept { return m_left.size(); }

    private:
        struct Left
        {
            UpsertItem item;
            double atMs = 0.0;
            bool byUpsert = false;
        };
        std::vector<Left> m_left;
        bool m_hasUpsert = false;
        double m_lastUpsertMs = 0.0;
    };
}
