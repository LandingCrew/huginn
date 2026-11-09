#pragma once

// =============================================================================
// WILDCARD GATE (R10) -- each page's cached wildcard and its clocks
// =============================================================================
// The decision half of Scoring::WildcardManager, over plain records so the
// host tests can drive it (tests/core/WildcardTests.cpp). The manager adds the
// clock (steady_clock in ns), the two uniforms, the uid check on the ranked
// list, the logs and the heartbeat; this decides.
//
//   Plan(page, now, fate)    before the page's allocation: the cached wildcard
//                            to pin on the seat, or a roll, or nothing.
//   Commit(page, in, now)    after it and the slot locks: a drawn wildcard the
//                            page shows is cached (one the locks hid counts
//                            for nothing, the roll waits); a pinned one the
//                            seat refused is dropped; a roll that drew
//                            nothing is spent.
//   UpdateExpiry(now)        every tick: cooldowns lapse on every page.
//
// The rules:
//
//   - Only the displayed page rolls. A page allocated off screen (a Wheeler
//     wheel, display = false) pins its OWN cached wildcard and never rolls,
//     so it never shows the displayed page's (the Wheeler leak, map Phase 8).
//   - In combat nothing is placed: no roll, no pin, and every page's cached
//     wildcard dropped (core/CombatLock.h StepWildcardsForCombat; stepped on
//     the displayed page's Plan). When combat ends every page's refractory
//     restarts.
//   - A page reshaped (its key count or its wildcard key moved, an INI
//     reload) drops its wildcard, with no refractory owed.
//   - A cached wildcard whose rows all are uid-less gear on a page whose
//     wheel hides them is dropped (core/WheelUidGate.h FateOfCachedWildcard).
//   - One roll per refractory window. Once fRefractorySeconds have passed
//     since the page's last wildcard ended, the next run on which the seat
//     can take one rolls (the allocation reports rollAsked); if the roll
//     fails, or comes up with nothing to draw, the roll is SPENT and the
//     refractory restarts. So P(roll) (core/WildcardPick.h, base plus the
//     uncertainty term) is the chance per window that a wildcard appears, and
//     it governs how often they do. A run whose seat was busy (an override,
//     a Remembrance hold) never evaluated the roll: it waits for a later run
//     in the same window. Before this, a failed roll tried again on the next
//     run, so P(roll) only set a delay of a few runs.
//   - The propensity logged with a wildcard (v3 `wp`) is therefore the
//     chance per window that the window shows that form: P(roll) x P(the draw
//     shows the form), both as the run that rolled computed them.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/CombatLock.h"
#include "core/SlotAllocCore.h"
#include "core/WheelUidGate.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace Huginn::Core::Wildcard
{
    inline constexpr std::size_t kNoSeat = static_cast<std::size_t>(-1);

    /// The [Wildcards] clocks (the odds are Params, core/WildcardPick.h).
    struct GateSettings
    {
        double cooldownSec = 30.0;      // fCooldownSeconds: how long a wildcard stays
        double refractorySec = 60.0;    // fRefractorySeconds: the window between wildcards
    };

    /// The wildcard-relevant shape of the page an allocation fills.
    struct GatePage
    {
        std::size_t index = 0;          // the page (keys its cache)
        std::size_t slotCount = 0;      // keys it shows
        std::size_t seat = kNoSeat;     // its wildcard key (SlotAlloc::SeatOf), kNoSeat = none
        bool inCombat = false;          // the lock's notion of combat
        bool display = true;            // the page the pipeline allocates this tick: only it rolls
    };

    /// One page's wildcard: the item, the shape it was rolled against, and its
    /// own clocks (per page: a shared clock would let page A's expiry impose a
    /// refractory on page B).
    struct PageCache
    {
        std::uint32_t formID = 0;
        std::uint16_t uniqueID = 0;
        double propensity = 0.0;        // logged only (v3 `wp`)
        // The shape this cache was rolled against. kNoSeat on both so a
        // never-used page mismatches a real shape on first use.
        std::size_t slotCount = kNoSeat;
        std::size_t seat = kNoSeat;
        std::int64_t rolledAtNs = 0;
        std::int64_t endedAtNs = 0;

        [[nodiscard]] bool Active() const noexcept { return formID != 0; }
        void Clear() noexcept
        {
            formID = 0;
            uniqueID = 0;
            propensity = 0.0;
        }
    };

    /// What Plan decided, and what it did on the way (for the manager's logs).
    struct PlanOut
    {
        std::uint32_t pinForm = 0;      // the cached wildcard to pin, 0 = none
        std::uint16_t pinUniqueID = 0;
        bool roll = false;              // roll for one this run
        // Notes (0 = none): what Plan dropped, and why.
        std::uint32_t reshapeDropped = 0;
        std::uint32_t expired = 0;
        std::uint32_t uidDropped = 0;
        std::vector<std::pair<std::size_t, std::uint32_t>> combatDropped;   // (page, form)
        bool combatOver = false;        // the refractory restarted on every page
    };

    /// What the page's allocation did with Plan's request (the slot core's
    /// WildcardOut and the row it drew).
    struct CommitIn
    {
        bool pinFound = false;          // the cached wildcard had a row in the page's list
        SlotAlloc::PinFate pin = SlotAlloc::PinFate::None;
        bool rollAsked = false;         // the allocation evaluated the roll
        std::uint32_t drawnForm = 0;    // the row it drew onto the seat, 0 = none
        std::uint16_t drawnUniqueID = 0;
        double propensity = 0.0;        // WildcardOut::Propensity()
        /// 0.24.6: the drawn row is what the page SHOWS on the seat after the
        /// slot locks (SlotLocker::ApplyLocks). False when the locker kept or
        /// cleared it anyway; the draw then counts for nothing (NotShown).
        bool drawnShown = true;
    };

    enum class CommitFate : std::uint8_t
    {
        None,       // nothing changed
        Refused,    // the pinned wildcard was refused by its key: dropped, refractory restarted
        Cached,     // a drawn wildcard is cached for the cooldown
        RollSpent,  // the roll was evaluated and drew nothing: the refractory restarts
        NotShown,   // 0.24.6: a row was drawn but the slot locks did not show it: nothing cached, the window stays open
    };

    /// 0.24.6: does the page show the drawn row on the wildcard key after the
    /// slot locks (SlotLocker::ApplyLocks)? Only as the wildcard, and only
    /// that stack: a lock that kept the key's old item, a post-lock dedup
    /// that cleared it, or a dedup refill that put another row there leaves
    /// the player with no wildcard to see (CommitIn::drawnShown).
    [[nodiscard]] constexpr bool DrawnShown(bool seatShowsWildcard, std::uint32_t shownForm,
        std::uint16_t shownUniqueID, std::uint32_t drawnForm, std::uint16_t drawnUniqueID) noexcept
    {
        return seatShowsWildcard && drawnForm != 0 && shownForm == drawnForm && shownUniqueID == drawnUniqueID;
    }

    template <std::size_t N>
    class Gate
    {
    public:
        static constexpr std::size_t kPages = N;

        /// A fresh start at `nowNs`: nothing cached, every refractory running
        /// from now (the first wildcard waits one window).
        void Reset(std::int64_t nowNs) noexcept
        {
            m_wasInCombat = false;
            for (auto& c : m_pages) {
                c = PageCache{};
                c.rolledAtNs = nowNs;
                c.endedAtNs = nowNs;
            }
        }

        GateSettings settings;

        /// What `page`'s allocation should do with its wildcard key this run.
        /// `fate(formID)` -> CachedWildcardFate: whether the page's list can
        /// show the cached wildcard (asked only when one is cached).
        template <class Fate>
        [[nodiscard]] PlanOut Plan(const GatePage& page, std::int64_t nowNs, Fate&& fate)
        {
            PlanOut r;
            if (page.index >= N) return r;
            auto& cache = m_pages[page.index];

            // A reshape drops the wildcard: it was rolled for a key that may
            // not mean the same thing now. No refractory is owed (it was never
            // shown at the new shape), so a roll may follow on this run.
            if (cache.slotCount != page.slotCount || cache.seat != page.seat) {
                if (cache.Active()) {
                    r.reshapeDropped = cache.formID;
                    cache.Clear();
                }
                cache.slotCount = page.slotCount;
                cache.seat = page.seat;
            }

            if (page.display) {
                const auto step = StepWildcardsForCombat(m_wasInCombat, page.inCombat);
                m_wasInCombat = page.inCombat;
                if (step.dropCached) {
                    for (std::size_t p = 0; p < N; ++p) {
                        if (m_pages[p].Active()) {
                            r.combatDropped.emplace_back(p, m_pages[p].formID);
                            m_pages[p].Clear();
                            m_pages[p].endedAtNs = nowNs;
                        }
                    }
                }
                if (step.restartRefractory) {
                    for (auto& c : m_pages) c.endedAtNs = nowNs;
                    r.combatOver = true;
                }
                if (!step.mayPlace) return r;
            } else if (page.inCombat) {
                return r;
            }

            if (page.seat == kNoSeat) return r;   // no wildcard key on this page

            if (ExpireIfDue(cache, nowNs)) r.expired = m_lastExpired;

            if (cache.Active()) {
                if (fate(cache.formID) == CachedWildcardFate::Drop) {
                    // Kept, it would block a roll for its whole cooldown while
                    // the key shows something else.
                    r.uidDropped = cache.formID;
                    cache.Clear();
                } else {
                    r.pinForm = cache.formID;   // Absent: kept, and nothing is pinned this run
                    r.pinUniqueID = cache.uniqueID;
                    return r;
                }
            }

            r.roll = page.display && Seconds(nowNs - cache.endedAtNs) >= settings.refractorySec;
            return r;
        }

        /// What `pageIndex`'s allocation did with Plan's request.
        CommitFate Commit(std::size_t pageIndex, const CommitIn& in, std::int64_t nowNs)
        {
            if (pageIndex >= N) return CommitFate::None;
            auto& cache = m_pages[pageIndex];
            if (cache.Active() && in.pinFound && in.pin == SlotAlloc::PinFate::Refused) {
                // Equipped under skip-equipped (the exploration is done) or of
                // a class the key does not take: dropped, as an expiry.
                cache.Clear();
                cache.endedAtNs = nowNs;
                return CommitFate::Refused;
            }
            if (in.drawnForm != 0 && !in.drawnShown) {
                // Never seen, so never a wildcard: not cached (no cooldown on
                // a key that shows something else), not logged or counted,
                // and the roll is not spent -- as a run whose seat a lock
                // held, it waits for a later run in the same window.
                return CommitFate::NotShown;
            }
            if (in.drawnForm != 0) {
                cache.formID = in.drawnForm;
                cache.uniqueID = in.drawnUniqueID;
                cache.propensity = in.propensity;
                cache.rolledAtNs = nowNs;
                return CommitFate::Cached;
            }
            if (in.rollAsked && !cache.Active()) {
                cache.endedAtNs = nowNs;   // one roll per window: this one is spent
                return CommitFate::RollSpent;
            }
            return CommitFate::None;
        }

        /// Every page's cooldown, every tick. True if any page's wildcard
        /// lapsed (the caller forces a pipeline run so the key can change).
        [[nodiscard]] bool UpdateExpiry(std::int64_t nowNs) noexcept
        {
            bool any = false;
            for (auto& c : m_pages) any = ExpireIfDue(c, nowNs) || any;
            return any;
        }

        [[nodiscard]] const PageCache* Page(std::size_t index) const noexcept
        {
            return index < N ? &m_pages[index] : nullptr;
        }

    private:
        [[nodiscard]] static double Seconds(std::int64_t ns) noexcept { return static_cast<double>(ns) * 1e-9; }

        bool ExpireIfDue(PageCache& c, std::int64_t nowNs) noexcept
        {
            if (!c.Active() || Seconds(nowNs - c.rolledAtNs) < settings.cooldownSec) return false;
            m_lastExpired = c.formID;
            c.Clear();
            c.endedAtNs = nowNs;
            return true;
        }

        std::array<PageCache, N> m_pages{};
        bool m_wasInCombat = false;
        std::uint32_t m_lastExpired = 0;
    };
}
