#pragma once

#include "ScoredCandidate.h"
#include "Config.h"
// SlotSettings only — MAX_SLOTS_PER_PAGE / MAX_PAGES below. SlotAllocator.h was
// included for a legacy ApplyWildcards overload that read the live slot count;
// the page is now passed in from the pipeline snapshot, and learning/ no longer
// depends on slot/ machinery (critique #10).
#include "../slot/SlotSettings.h"
#include <algorithm>  // std::clamp below
#include <array>      // m_pages below
#include <chrono>
#include <random>
// <algorithm>/<array> were arriving through the deleted SlotAllocator.h. They
// still resolve via MSVC's <random>/<vector> chains, which is a bet on standard
// library internals — stated explicitly instead.

namespace Huginn::Scoring
{
    // =============================================================================
    // WILDCARD PAGE
    // =============================================================================
    // The wildcard-relevant shape of the page a tick allocates. Passed as one
    // value rather than as loose counts so the cache key and the bounds that go
    // with it can never be sourced from different ticks or different pages.
    //
    // wildcardSlots is a page-level CAPACITY, not a per-index map: a wildcard is
    // injected at a ranking position, and allocation then hands it to whichever
    // slot's classification matches (SlotAllocator walks slots in priority
    // order), so there is no way to know in advance which slot will take it.
    // What IS knowable is that a page with N wildcard-accepting slots can never
    // display more than N of them — roll more and the surplus is stranded.
    // =============================================================================

    struct WildcardPage
    {
        size_t index = 0;          // pageIndex — keys the per-page wildcard cache
        size_t slotCount = 0;      // slots this page displays
        size_t wildcardSlots = 0;  // of those, how many set bWildcardsEnabled
    };

    // =============================================================================
    // WILDCARD MANAGER (v0.12.0: Generalized for N slots)
    // =============================================================================
    // Manages wildcard exploration for recommendations.
    // Wildcards allow untried or lower-scored candidates to occasionally appear
    // in recommendation slots, promoting exploration of the Q-learning space.
    //
    // Design:
    //   - Slot 0 (index 0) is always the top-scored pick (no wildcard by default)
    //   - Subsequent slots have configurable wildcard probability
    //   - Probability increases with slot index: P(slot i) = baseProbability * i
    //   - Once selected, wildcards persist for a configurable duration (default 30s)
    //   - After wildcard expires, refractory period before new wildcard (default 5s)
    //
    // v0.12.0 Changes:
    //   - Generalized from fixed 3 slots to N slots
    //   - Replaced m_cachedWildcardSlot2/3 with vector
    //   - Probability scales with slot index using configurable base probability
    //   - Compatible with multi-page system (each page can have different slot count)
    //
    // v0.19.6 — PER-PAGE CACHE. The cache used to be one global array indexed by
    // position and shared by every page, which made "a cached wildcard nothing
    // can display" reachable three ways: a switch to a smaller page stranded
    // entries above its slot count (#70); a page whose slots all forbid
    // wildcards had entries rolled for it anyway; and the slotCount < 2 guard
    // skipped the repair path entirely on a 1-slot page. All three suppressed
    // re-rolls, because the liveness check scanned the whole cache while only a
    // bounded prefix could ever be displayed.
    //
    // Each page now owns its array, its cooldown and its refractory timer, and
    // each array records the page SHAPE it was rolled against — a shape change
    // (an INI hot-reload resizing a page, or toggling bWildcardsEnabled)
    // discards that page's cache wholesale. Every cached entry is therefore, by
    // construction, within the bounds of a page that can display it: stranding
    // has nowhere left to happen, and no bounds-repair pass is needed.
    //
    // This class is generic and works with any ScoredCandidate type (spells, items, etc.)
    // =============================================================================

    class WildcardManager
    {
    public:
        WildcardManager();
        ~WildcardManager() = default;

        // Apply wildcards to a ranked candidate list
        // Modifies the list in-place by swapping wildcard candidates into eligible slots
        // @param page The page THIS tick allocates — must be the pipeline
        //        snapshot (PipelineContext::displayPageIndex / displaySlotCount /
        //        displayWildcardSlots), not a live SlotAllocator read. Wildcard
        //        eligibility is page-relative, so reading it live lets a mid-tick
        //        page switch wildcard against a page the pipeline is not
        //        allocating. A single-argument overload used to do exactly that
        //        (critique #10) — do not reintroduce one.
        void ApplyWildcards(ScoredCandidateList& rankedCandidates, const WildcardPage& page);

        // Unconditional per-tick expiry check, across ALL pages: a wildcard on a
        // page the player has switched away from must still age out on its own
        // timer. ApplyWildcards (which normally drives expiry) runs only on
        // non-skipped pipeline ticks, so without this an expired wildcard stays
        // displayed while the pipeline is idle.
        // @return true if ANY page's wildcards lapsed this call (caller must
        //         force a pipeline run so the slot content can swap).
        //         Deliberately not narrowed to "the page on screen": this class
        //         only learns which page that is when ApplyWildcards runs, i.e.
        //         on non-skipped pipeline ticks, and SlotAllocator::Initialize()
        //         resets m_currentPage directly without raising m_pageChanged
        //         (SlotAllocator.cpp:70) — so after an `hg reload` a remembered
        //         page can be stale exactly while the pipeline is skipped, which
        //         is the one situation this return value exists to break out of.
        //         Over-reporting costs one pipeline run that repaints the same
        //         thing; under-reporting leaves an expired wildcard on screen.
        [[nodiscard]] bool UpdateExpiry();

        // Configuration
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        void SetBaseProbability(float probability) { m_baseProbability = std::clamp(probability, 0.0f, 1.0f); }
        void SetMaxProbability(float probability) { m_maxProbability = std::clamp(probability, 0.0f, 1.0f); }
        void SetCooldown(float seconds) { m_cooldownSeconds = std::max(0.0f, seconds); }
        void SetRefractoryPeriod(float seconds) { m_refractorySeconds = std::max(0.0f, seconds); }
        void SetFirstSlotExcluded(bool excluded) { m_firstSlotExcluded = excluded; }

        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
        [[nodiscard]] float GetBaseProbability() const noexcept { return m_baseProbability; }
        [[nodiscard]] float GetMaxProbability() const noexcept { return m_maxProbability; }
        [[nodiscard]] float GetCooldown() const noexcept { return m_cooldownSeconds; }
        [[nodiscard]] float GetRefractoryPeriod() const noexcept { return m_refractorySeconds; }
        [[nodiscard]] bool IsFirstSlotExcluded() const noexcept { return m_firstSlotExcluded; }

        // Query current wildcard state. Page-scoped: "is a wildcard active" is
        // only answerable about a specific page now, so callers must say which.
        [[nodiscard]] bool HasActiveWildcard(size_t pageIndex) const;
        [[nodiscard]] RE::FormID GetWildcardForSlot(size_t pageIndex, size_t slotIndex) const;
        [[nodiscard]] size_t GetActiveWildcardCount(size_t pageIndex) const;

        // Legacy setters (map to base probability)
        void SetSlot2Probability(float p) { m_baseProbability = p; }
        void SetSlot3Probability(float p) { m_maxProbability = p; }
        [[nodiscard]] float GetSlot2Probability() const noexcept { return m_baseProbability; }
        [[nodiscard]] float GetSlot3Probability() const noexcept { return m_maxProbability; }

        // Reset wildcards on every page (e.g., on save load)
        void Reset();

    private:
        // Maximum slots/pages to track — canonical constants from SlotSettings.h
        static constexpr size_t MAX_WILDCARD_SLOTS = Slot::MAX_SLOTS_PER_PAGE;
        static constexpr size_t MAX_WILDCARD_PAGES = Slot::MAX_PAGES;

        // Wildcard persistence tracking (per slot)
        struct WildcardSlot {
            RE::FormID formID = 0;
            Candidate::SourceType sourceType = Candidate::SourceType::Spell;
        };

        // One page's wildcard state: the entries, the page shape they were
        // rolled against, and their own cooldown/refractory clocks. The timers
        // are per-page on purpose — a shared clock would let page A's expiry
        // impose a refractory on page B, the same class of cross-page coupling
        // the shared array caused.
        struct PageWildcards {
            std::array<WildcardSlot, MAX_WILDCARD_SLOTS> slots{};

            // Shape this cache was rolled against. Any change invalidates it
            // wholesale — see the class comment. SIZE_MAX so a never-populated
            // page mismatches a real shape on first use without a separate flag.
            size_t slotCount = SIZE_MAX;
            size_t wildcardSlots = SIZE_MAX;

            std::chrono::steady_clock::time_point lastWildcardTime{};
            std::chrono::steady_clock::time_point wildcardEndTime{};
        };

        // Internal helpers
        [[nodiscard]] static bool PageHasActiveWildcard(const PageWildcards& cache);
        static void ClearPageEntries(PageWildcards& cache);
        void UpdateWildcardExpiry(PageWildcards& cache, std::chrono::steady_clock::time_point now);
        void RollNewWildcards(PageWildcards& cache,
                              const ScoredCandidateList& rankedCandidates,
                              size_t slotCount,
                              size_t wildcardSlots,
                              std::chrono::steady_clock::time_point now);
        void ApplyWildcardsToRanking(ScoredCandidateList& rankedCandidates,
                                     const PageWildcards& cache,
                                     size_t slotCount);

        // Calculate wildcard probability for a given slot index
        [[nodiscard]] float GetProbabilityForSlot(size_t slotIndex, size_t slotCount) const;

        // Find a random candidate from the list (excluding already used).
        // @param minIndex Lowest ranking position worth drawing from. A wildcard
        //        is only ever surfaced by SWAPPING a lower-ranked candidate UP
        //        into the slot, so a draw from at or above the target slot is a
        //        roll that can never be displayed — see the call site.
        [[nodiscard]] RE::FormID SelectRandomCandidate(
            const ScoredCandidateList& candidates,
            Candidate::SourceType sourceType,
            const std::vector<RE::FormID>& excludeFormIDs,
            size_t minIndex);

        // Settings
        bool m_enabled = true;
        bool m_firstSlotExcluded = true;           // Slot 0 never gets wildcards by default
        float m_baseProbability = 0.165f;          // Base probability (slot 1 gets this)
        float m_maxProbability = 0.5f;             // Maximum probability cap
        float m_cooldownSeconds = Config::WILDCARD_COOLDOWN_SECONDS;
        float m_refractorySeconds = 5.0f;

        std::array<PageWildcards, MAX_WILDCARD_PAGES> m_pages;

        // Random number generator (thread-local for safety)
        std::mt19937& GetRNG();

        // Reusable buffer for candidate selection (avoids heap allocation in hot path)
        mutable std::vector<RE::FormID> m_eligibleBuffer;

        // Reusable per-call scratch (update thread only; cleared at each use):
        // FormIDs assigned this roll, and swap tracking across the ranked list
        // (full-list sized — swap SOURCE indices can be anywhere in the list).
        std::vector<RE::FormID> m_usedFormIDsBuffer;
        std::vector<bool> m_swappedBuffer;
    };

}  // namespace Huginn::Scoring
