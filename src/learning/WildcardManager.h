#pragma once

#include "ScoredCandidate.h"
#include "Config.h"
// SlotSettings only -- MAX_SLOTS_PER_PAGE / MAX_PAGES below -- and the wildcard
// key's two records (slot/WildcardKey.h). Not SlotAllocator.h: learning/ does
// not depend on the allocator's machinery (critique #10); the pipeline passes
// the page in and carries the request and the result across.
#include "../slot/SlotSettings.h"
#include "../slot/WildcardKey.h"
#include "core/WildcardGate.h"   // the per-page cache and clocks (host-tested)
#include <algorithm>  // std::clamp below
#include <cstdint>
#include <random>

namespace Huginn::Scoring
{
    // =============================================================================
    // WILDCARD PAGE
    // =============================================================================
    // The wildcard-relevant shape of the page an allocation fills. Passed as one
    // value so the cache key and the seat that goes with it can never be sourced
    // from different ticks or different pages.
    // =============================================================================

    struct WildcardPage
    {
        size_t index = 0;            // pageIndex -- keys the per-page wildcard cache
        size_t slotCount = 0;        // slots this page displays
        size_t seat = SIZE_MAX;      // its wildcard key (SlotAllocator::GetWildcardSeat), SIZE_MAX = none
        // The locker counts the player in combat (SlotLocker::IsInCombat): no
        // wildcard is placed, and every page's cached one is dropped
        // (core::WildcardMayBePlaced). An exploration pick held a key through
        // a whole fight under the combat freeze (2026-10-12).
        bool inCombat = false;
        // The page feeds a Wheeler wheel, which cannot show a weapon or armour
        // with no uniqueID (display/WheelUidFilter.h): its list leaves such rows
        // out, so a cached wildcard that is one cannot be shown and is dropped
        // (core::FateOfCachedWildcard) instead of blocking the next roll.
        bool hideUidlessGear = false;
        // The page the pipeline allocates this tick: only it may roll. A
        // Wheeler page off screen (WheelerBackend) shows its own cached
        // wildcard, never the displayed page's (the Wheeler leak, map Phase 8).
        bool display = true;
    };

    // =============================================================================
    // WILDCARD MANAGER (R10: wildcards by uncertainty, map Phase 8)
    // =============================================================================
    // Each page has ONE wildcard key, the seat (Core::SlotAlloc::SeatOf: the
    // last key in fill order with bWildcardsEnabled; key 8 on the shipped
    // pages). This class owns, per page, the cached wildcard and its clocks
    // (the decisions are Core::Wildcard::Gate's, core/WildcardGate.h, host-
    // tested); the allocation places and draws it (core/SlotAllocCore.h,
    // PASS 1w and 4w):
    //
    //   Plan(page, list)   before the page's allocation: the cached wildcard to
    //                      pin on the seat, or -- nothing cached, the refractory
    //                      over, not in combat, the displayed page -- a roll,
    //                      with its two uniforms and the [Wildcards] odds.
    //   Commit(page, res)  after it: a drawn wildcard is cached for
    //                      fCooldownSeconds with its propensity; a pinned one
    //                      the seat refused is dropped; a roll that drew
    //                      nothing is spent (one roll per refractory window).
    //   UpdateExpiry()     every tick: cooldowns lapse on every page.
    //
    // What a roll draws (core/WildcardPick.h): from the ranked rows the finished
    // page does not show and the seat takes -- every item type, not the top
    // row's type as before -- weighted by relevance x sd (the model's
    // probability that the player prefers the row to the seat's occupant,
    // times the row's posterior sd), plus a uniform share (fUniformShare); the
    // roll's chance runs from fBaseProbability (nothing uncertain near the
    // page) to fMaxProbability (an unknown item at its edge), once per
    // refractory window, so it sets how often wildcards appear. The propensity
    // (P(roll) x P(the draw shows its form): the chance per window that the
    // window shows that form) is exact and logged (v3 `wp`).
    //
    // Replaces the rank swap (v0.12.0 to R8.4): per-slot rolls at
    // base x slot index, a uniform draw among rows of the top row's type, and
    // a swap into a rank position that the slot hold and the fill had to
    // exempt (removed from the core with this class's rework). The per-page
    // cache, the shape check, the combat step and the all-page expiry stay
    // (#70, 0.23.40).
    // =============================================================================

    class WildcardManager
    {
    public:
        WildcardManager();
        ~WildcardManager() = default;

        /// What `page`'s allocation should do with its wildcard key this run.
        /// @param ranked The FULL ranked list (not the page's filtered one): a
        ///        cached wildcard whose rows all are uid-less gear on a page
        ///        that hides them is dropped here (core::FateOfCachedWildcard).
        /// Call once per allocation of the page, then Commit with its result.
        [[nodiscard]] Slot::WildcardRequest Plan(const WildcardPage& page, const ScoredCandidateList& ranked);

        /// What `page`'s allocation did with the request Plan made. A roll
        /// that drew nothing is spent: the refractory restarts (one roll per
        /// window, core/WildcardGate.h). Not called for an allocation the
        /// pipeline abandons.
        void Commit(const WildcardPage& page, const Slot::WildcardResult& result);

        /// Unconditional per-tick expiry check, across ALL pages: a wildcard on a
        /// page the player has switched away from must still age out on its own
        /// timer. Plan/Commit run only on non-skipped pipeline ticks, so without
        /// this an expired wildcard stays displayed while the pipeline is idle.
        /// @return true if ANY page's wildcard lapsed this call (the caller must
        ///         force a pipeline run so the key can change). Not narrowed to
        ///         the page on screen: after a game load the remembered page can
        ///         be stale exactly while the pipeline is skipped (SlotAllocator::
        ///         Reset sets the page without raising m_pageChanged). Over-
        ///         reporting costs one run that repaints the same thing;
        ///         under-reporting leaves an expired wildcard on screen.
        [[nodiscard]] bool UpdateExpiry();

        // Configuration ([Wildcards], Globals.cpp LoadWildcardConfigFromINI)
        void SetEnabled(bool enabled) { m_gate.settings.enabled = enabled; }
        void SetBaseProbability(float probability) { m_params.baseRoll = std::clamp(probability, 0.0f, 1.0f); }
        void SetMaxProbability(float probability) { m_params.maxRoll = std::clamp(probability, 0.0f, 1.0f); }
        void SetUniformShare(float share) { m_params.uniformShare = std::clamp(share, 0.0f, 1.0f); }
        void SetCooldown(float seconds) { m_gate.settings.cooldownSec = std::max(0.0f, seconds); }
        void SetRefractoryPeriod(float seconds) { m_gate.settings.refractorySec = std::max(0.0f, seconds); }

        [[nodiscard]] bool IsEnabled() const noexcept { return m_gate.settings.enabled; }
        [[nodiscard]] float GetBaseProbability() const noexcept { return static_cast<float>(m_params.baseRoll); }
        [[nodiscard]] float GetMaxProbability() const noexcept { return static_cast<float>(m_params.maxRoll); }
        [[nodiscard]] float GetUniformShare() const noexcept { return static_cast<float>(m_params.uniformShare); }
        [[nodiscard]] float GetCooldown() const noexcept { return static_cast<float>(m_gate.settings.cooldownSec); }
        [[nodiscard]] float GetRefractoryPeriod() const noexcept { return static_cast<float>(m_gate.settings.refractorySec); }

        // Query the current wildcard state. Page-scoped.
        [[nodiscard]] bool HasActiveWildcard(size_t pageIndex) const;
        [[nodiscard]] RE::FormID GetWildcard(size_t pageIndex) const;
        [[nodiscard]] uint16_t GetWildcardUniqueID(size_t pageIndex) const;
        [[nodiscard]] size_t GetActiveWildcardCount(size_t pageIndex) const { return HasActiveWildcard(pageIndex) ? 1 : 0; }

        /// R4, logging only: the propensity of `formID`'s cached wildcard on
        /// `pageIndex` -- P(the window's roll came up) x P(the draw showed that
        /// form), as the allocation that drew it computed them
        /// (core/WildcardPick.h; per window, core/WildcardGate.h). NaN when it
        /// is not that page's cached wildcard. Read on the update thread.
        [[nodiscard]] float GetWildcardPropensity(size_t pageIndex, RE::FormID formID) const;

        /// Reset wildcards on every page (e.g., on save load)
        void Reset();

    private:
        using Gate = Core::Wildcard::Gate<Slot::MAX_PAGES>;

        [[nodiscard]] static std::int64_t NowNs();

        Core::Wildcard::Params m_params{};
        Gate m_gate;

        // Random number generator (update thread only)
        std::mt19937& GetRNG();
    };

}  // namespace Huginn::Scoring
