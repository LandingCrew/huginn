#pragma once

// Combat upsert (fCombatChallengerMargin, fCombatWantedSec, fCombatUpsertCooldownSec): the one
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
// The rule (0.24.29; the game side is SlotLocker::ApplyCombatUpserts). One line:
//   s(challenger) - s(item on the key) > fCombatChallengerMargin   (1.0; below 0 = off)
// and the challenger has been wanted -- on the memoryless page (this run's
// ranking alone, core/SlotAllocStateless.h) and on no key -- for
// fCombatWantedSec (1.5 s) without a break (CombatWantedClock below).
//
// A key whose item an active-buff filter dropped this run (the Resist Fire
// just drunk, its buff now running: the need it answers is covered) has no
// score this run, only the one it was locked on. Measured against that, a
// covered key would hold the answer to a need that is gone; so a covered key
// takes any challenger past the wanted clock, ahead of the keys that need a
// lead. The same for a spell for someone else dropped because no ally is in
// the crosshair (no_ally_aimed, 0.24.31): the one it was for is out of view,
// and its "on Self" twin is the buff (Core::Filter::FreesCombatKey).
// Every other filter that drops a key's item (it cannot be afforded, it
// is equipped, on cooldown just after use, a heal at full health) says the
// player used it or will want it back in a moment: those keys keep the score
// they were locked on.
//
// Why (2026-11-09, LoreRim): the line before it was the challenger rule at
// fCombatChallengerConfidence 0.995, z 2.576 x sigma_Delta. With sd 2-3 on
// both items it asked a lead of about 7, so the freeze held keys on items
// ranked 45th-167th through whole fights (an answer such as Resist Fire got
// no key until the fight ended), and every patch on it -- stale keys at 0.8,
// then 0.95, once per key per fight -- either churned (21 changes in 2 min) or
// held (all 8 swaps spent in 23 s, then nothing could take a key). A replay of
// that evening's 31 fights put this rule at about 8.5 changes a minute (the
// freeze was tuned to about 9), a median wait of 4.4 s from wanted to shown,
// and no press on an item it had already moved. The wanted clock is what
// keeps the churn down: half of the items the memoryless page names stop
// being named within a second or two.
//
// An item an upsert displaced may come back once it has been off its key for
// UpsertRecentMs, has been wanted again for fCombatWantedSec (its clock starts
// when it leaves the key: time wanted while shown does not count), and beats
// the key's item by the margin: the fight's context changed, not a flicker.
//
// Each key is asked about its best fitting challenger only: under one fixed
// line a lower score cannot clear what the best one missed.
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
// Pure (the standard library), host-tested in tests/core/CombatUpsertTests.cpp.

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
    /// The challenger beats the frozen item by more than `margin`
    /// (fCombatChallengerMargin). Never for a non-finite score on either side
    /// (an unranked row, a pinned sentinel) or a margin below 0 or not finite
    /// (the upsert off).
    [[nodiscard]] inline bool CombatUpsertBeats(SlotScore challenger, SlotScore frozen, double margin) noexcept
    {
        if (!std::isfinite(challenger) || !std::isfinite(frozen) || !(margin >= 0.0) || !std::isfinite(margin)) {
            return false;
        }
        return ScoreExceedsByMargin(challenger, frozen, margin);
    }

    /// At most one upsert per page per this long (and one per run). Several
    /// keys can clear the margin on the same run -- a context flip re-scores
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
        Off,              ///< fCombatChallengerMargin below 0 (the upsert off)
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
        bool enabled = false;          ///< fCombatChallengerMargin >= 0
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

    /// How long until a key the gate closed on time alone opens (0.24.29,
    /// review of PR #254): 0 when it is open; for PressGrace and Cooldown the
    /// time until the press, the change and the upsert are all far enough
    /// behind; +inf for any other gate (it opens on an event, not a time).
    [[nodiscard]] constexpr double CombatUpsertGateWaitMs(const CombatUpsertGateInput& in) noexcept
    {
        const auto gate = DecideCombatUpsertGate(in);
        if (gate == CombatUpsertGate::Open) return 0.0;
        if (gate != CombatUpsertGate::PressGrace && gate != CombatUpsertGate::Cooldown) {
            return std::numeric_limits<double>::infinity();
        }
        double wait = 0.0;
        const double pressMs = in.graceMs > in.pressFloorMs ? in.graceMs : in.pressFloorMs;
        if (in.sincePressMs >= 0.0 && in.sincePressMs < pressMs && pressMs - in.sincePressMs > wait) {
            wait = pressMs - in.sincePressMs;
        }
        if (in.graceMs > 0.0 && in.sinceShownMs >= 0.0 && in.sinceShownMs < in.graceMs &&
            in.graceMs - in.sinceShownMs > wait) {
            wait = in.graceMs - in.sinceShownMs;
        }
        if (in.cooldownMs > 0.0 && in.sinceUpsertMs >= 0.0 && in.sinceUpsertMs < in.cooldownMs &&
            in.cooldownMs - in.sinceUpsertMs > wait) {
            wait = in.cooldownMs - in.sinceUpsertMs;
        }
        return wait;
    }

    /// No upsert this soon after the Wheeler wheel closed: the widget is back
    /// and the player reaches for a key, so a challenger that waited behind
    /// the open wheel must not land in the first run after it. `sinceCloseMs`
    /// < 0 = the wheel has not closed (since the last reset).
    [[nodiscard]] constexpr double WheelCloseHoldMs(double graceMs) noexcept
    {
        return graceMs > kCombatUpsertPressFloorMs ? graceMs : kCombatUpsertPressFloorMs;
    }

    [[nodiscard]] constexpr bool WheelClosedRecently(double sinceCloseMs, double graceMs) noexcept
    {
        return sinceCloseMs >= 0.0 && sinceCloseMs < WheelCloseHoldMs(graceMs);
    }

    /// A key open to an upsert: the score of the item on it, and whether an
    /// active-buff filter dropped that item this run (its need is covered).
    struct UpsertKey
    {
        SlotScore score = kUnrankedScore;
        bool covered = false;
    };

    struct CombatUpsertPick
    {
        std::size_t key = 0;          ///< index into `keys`
        std::size_t challenger = 0;   ///< index into `challengers`
        double lead = 0.0;            ///< s(challenger) - s(item on the key)
        bool covered = false;         ///< the key's item's need was covered: no lead asked
    };

    /// Which challenger takes which key this run. `keys[k]`: the item on key
    /// k, nullopt when the key is not open to an upsert. `challengers[c]`:
    /// challenger scores. `fits[k][c]`: challenger c may go on key k (missing
    /// entries = no). `margin`: fCombatChallengerMargin (below 0 or not
    /// finite: none).
    ///
    /// For each open key, its best challenger (highest score, ties: lower
    /// index) that fits and is not already placed: it takes the key when the
    /// key is covered, or when it beats the key's item by the margin. Greedy
    /// over the keys' picks, the covered keys first and then the widest lead
    /// (ties: lower key): one challenger per key and one key per challenger,
    /// at most `maxPicks` in all (the game passes 1: one upsert per page per
    /// run).
    [[nodiscard]] inline std::vector<CombatUpsertPick> PlanCombatUpserts(
        const std::vector<std::optional<UpsertKey>>& keys, const std::vector<SlotScore>& challengers,
        const std::vector<std::vector<bool>>& fits, double margin,
        std::size_t maxPicks = std::numeric_limits<std::size_t>::max())
    {
        std::vector<CombatUpsertPick> picks;
        if (!(margin >= 0.0) || !std::isfinite(margin)) return picks;   // off
        std::vector<std::size_t> order;
        for (std::size_t c = 0; c < challengers.size(); ++c) {
            if (std::isfinite(challengers[c])) order.push_back(c);
        }
        std::stable_sort(order.begin(), order.end(),
            [&challengers](std::size_t a, std::size_t b) { return challengers[a] > challengers[b]; });
        // The covered keys rank before any lead; among them, the lower key.
        auto before = [](const CombatUpsertPick& a, const CombatUpsertPick& b) {
            if (a.covered != b.covered) return a.covered;
            return !a.covered && a.lead > b.lead;
        };
        std::vector<bool> keyTaken(keys.size(), false);
        std::vector<bool> challengerTaken(challengers.size(), false);
        while (picks.size() < maxPicks) {
            std::optional<CombatUpsertPick> best;
            for (std::size_t k = 0; k < keys.size(); ++k) {
                if (keyTaken[k] || !keys[k] || k >= fits.size()) continue;
                const auto& key = *keys[k];
                for (const std::size_t c : order) {
                    if (challengerTaken[c] || c >= fits[k].size() || !fits[k][c]) continue;
                    // The best fitting challenger only.
                    if (key.covered || CombatUpsertBeats(challengers[c], key.score, margin)) {
                        const CombatUpsertPick pick{ k, c, challengers[c] - key.score, key.covered };
                        if (!best || before(pick, *best)) best = pick;
                    }
                    break;
                }
            }
            if (!best) break;
            keyTaken[best->key] = true;
            challengerTaken[best->challenger] = true;
            picks.push_back(*best);
        }
        return picks;
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

    /// Since when each challenger has been wanted without a break
    /// (fCombatWantedSec): on the page the upsert challenges from and on no
    /// key. An item that drops out of the list -- it stopped being wanted, or
    /// a key shows it -- starts over when it comes back, so an item that just
    /// left a key has no wanted time from the runs it was shown. Per fight.
    class CombatWantedClock
    {
    public:
        /// This run's wanted items. Each keeps its start while it stays in
        /// the list; one new to it starts at `nowMs`; the rest are forgotten.
        void Note(const std::vector<UpsertItem>& wanted, double nowMs)
        {
            std::vector<Since> next;
            next.reserve(wanted.size());
            for (const auto& w : wanted) {
                double since = nowMs;
                for (const auto& s : m_since) {
                    if (s.item.Same(w.formID, w.uniqueID, w.name)) {
                        since = s.sinceMs;
                        break;
                    }
                }
                next.push_back({ w, since });
            }
            m_since = std::move(next);
        }

        /// How long the item has been wanted at `nowMs`; < 0 when it is not.
        [[nodiscard]] double ForMs(uint32_t formID, uint16_t uniqueID, std::string_view name,
            double nowMs) const noexcept
        {
            for (const auto& s : m_since) {
                if (s.item.Same(formID, uniqueID, name)) return nowMs - s.sinceMs;
            }
            return -1.0;
        }

        /// `ms` passed that does not count (the game was suspended): every
        /// wait resumes where it stopped.
        void Skip(double ms) noexcept
        {
            if (!(ms > 0.0)) return;
            for (auto& s : m_since) s.sinceMs += ms;
        }

        void Clear() noexcept { m_since.clear(); }

        [[nodiscard]] std::size_t Size() const noexcept { return m_since.size(); }

    private:
        struct Since
        {
            UpsertItem item;
            double sinceMs = 0.0;
        };
        std::vector<Since> m_since;
    };

    /// When a waiting challenger's time gates have all opened (0.24.29, review
    /// of PR #254): the latest of its wanted clock reaching fCombatWantedSec
    /// (`maturesAtMs`), the end of its recent-departure wait
    /// (`excludedUntilMs`), the page's spacing (`pageOpenAtMs`), the hold
    /// after the Wheeler wheel closed (`wheelOpenAtMs`) and the first key
    /// opening on time (`keysOpenAtMs`: CombatUpsertGateWaitMs); -inf for a
    /// gate that is not closed.
    [[nodiscard]] constexpr double UpsertOpenAtMs(double maturesAtMs, double excludedUntilMs, double pageOpenAtMs,
        double wheelOpenAtMs, double keysOpenAtMs) noexcept
    {
        double at = maturesAtMs;
        if (excludedUntilMs > at) at = excludedUntilMs;
        if (pageOpenAtMs > at) at = pageOpenAtMs;
        if (wheelOpenAtMs > at) at = wheelOpenAtMs;
        if (keysOpenAtMs > at) at = keysOpenAtMs;
        return at;
    }

    /// When the pipeline must next run for the combat upsert: the earliest of
    /// the challengers' opening times (UpsertOpenAtMs) after `nowMs`; +inf
    /// when none is still ahead. The wanted clock moves only on a run, and a
    /// quiet fight skips runs (nothing hashed changes once its element is
    /// held), so without this a matured wait sat until an unrelated change.
    /// A challenger whose gates are already open schedules nothing: it was
    /// judged on this run, and only a change the hash sees changes that.
    [[nodiscard]] inline double NextUpsertWakeMs(const std::vector<double>& openAtMs, double nowMs) noexcept
    {
        double wake = std::numeric_limits<double>::infinity();
        for (const double at : openAtMs) {
            if (at > nowMs && at < wake) wake = at;
        }
        return wake;
    }

    /// What the upsert must remember between runs, page-wide: the items that
    /// recently left a key, and when the page last took an upsert. Times are
    /// ms on one monotonic clock.
    ///
    /// 0.24.29 (review of PR #254): every departure is alike, an upsert's too.
    /// The item an upsert displaced used to be barred for the rest of the
    /// fight; under one fixed margin and the wanted clock it may come back
    /// once it is off its key for `recentMs` and beats the key's item by the
    /// margin (the fight changed, not a flicker). The flag that barred it was
    /// set by SlotLocker's churn pass on every upsert, overriding the upsert
    /// path's own note, so a Resist Fire bumped off a covered key never came
    /// back; it is gone, and no path can bar an item for the fight.
    class CombatUpsertMemory
    {
    public:
        /// `item` stopped showing on a key at `nowMs`, for whatever reason.
        void NoteLeft(UpsertItem item, double nowMs)
        {
            for (auto& d : m_left) {
                if (d.item.Same(item.formID, item.uniqueID, item.name)) {
                    d.atMs = nowMs;
                    return;
                }
            }
            m_left.push_back({ std::move(item), nowMs });
        }

        /// Not a challenger: left a key less than `recentMs` ago.
        [[nodiscard]] bool Excluded(uint32_t formID, uint16_t uniqueID, std::string_view name, double nowMs,
            double recentMs) const noexcept
        {
            for (const auto& d : m_left) {
                if (!d.item.Same(formID, uniqueID, name)) continue;
                if (nowMs - d.atMs < recentMs) return true;
            }
            return false;
        }

        /// When the item's recent-departure wait ends (it left a key at most
        /// `recentMs` before); -inf when it has not left one.
        [[nodiscard]] double ExcludedUntilMs(uint32_t formID, uint16_t uniqueID, std::string_view name,
            double recentMs) const noexcept
        {
            double until = -std::numeric_limits<double>::infinity();
            for (const auto& d : m_left) {
                if (d.item.Same(formID, uniqueID, name) && d.atMs + recentMs > until) until = d.atMs + recentMs;
            }
            return until;
        }

        /// The page may take an upsert: none yet, or the last one at least
        /// `spacingMs` ago.
        [[nodiscard]] bool PageOpen(double nowMs, double spacingMs) const noexcept
        {
            return !m_hasUpsert || nowMs - m_lastUpsertMs >= spacingMs;
        }

        /// When the page may take its next upsert; -inf when it has taken none.
        [[nodiscard]] double PageOpenAtMs(double spacingMs) const noexcept
        {
            return m_hasUpsert ? m_lastUpsertMs + spacingMs : -std::numeric_limits<double>::infinity();
        }

        void NoteUpsert(double nowMs) noexcept
        {
            m_hasUpsert = true;
            m_lastUpsertMs = nowMs;
        }

        /// Drop departures older than `recentMs`.
        void Prune(double nowMs, double recentMs)
        {
            std::erase_if(m_left, [&](const Left& d) { return nowMs - d.atMs >= recentMs; });
        }

        /// Combat over: the next fight's first upsert need not wait out the
        /// page spacing. Recent departures stay until they age out.
        void EndFight() noexcept { m_hasUpsert = false; }

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
        };
        std::vector<Left> m_left;
        bool m_hasUpsert = false;
        double m_lastUpsertMs = 0.0;
    };
}
