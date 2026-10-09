#pragma once

#include "SlotConfig.h"
#include "SlotClassCap.h"
#include "SlotAssignment.h"
#include "SlotClassifier.h"
#include "SlotSettings.h"
#include "SlotSnapshot.h"
#include "learning/ScoredCandidate.h"
#include "override/OverrideConditions.h"
#include "state/PlayerActorState.h"
#include "state/WorldState.h"
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace Huginn::Slot
{
    // =============================================================================
    // SLOT ALLOCATOR
    // =============================================================================
    // The pipeline component that allocates scored candidates to typed slots.
    // Takes the output from UtilityScorer and produces SlotAssignments for
    // the widget and Wheeler.
    //
    // Pipeline position:
    //   CandidateGenerator → UtilityScorer → [SlotAllocator] → Widget/Wheeler
    //
    // v0.12.0: Multi-page support
    //   - Allocation is now stateless - takes slot configs as parameter
    //   - SlotSettings manages page configurations
    //   - Each page has its own slot layout (up to 10 pages, 10 slots per page)
    //
    // Responsibilities:
    //   1. Classification filtering: Match candidates to slot types
    //   2. Deduplication: Each candidate appears in at most one slot
    //   3. Override injection: Force critical items (health potion, etc.)
    //   4. Priority ordering: Fill high-priority slots first
    //   5. Wildcard handling: Mark exploration picks appropriately
    //   6. Seating: an item that is still recommended keeps the slot it was in
    //
    // Thread Safety:
    //   AllocateSlots() is thread-safe.
    //
    //   It is no longer stateless, and the exception is (6). Seating remembers,
    //   per page, which item sat in which slot last time that page was
    //   allocated, because WHICH items to show is a ranking question the
    //   candidate list answers, and WHERE to put them is a memory question
    //   nothing else in the pipeline can answer: SlotLocker is per slot index
    //   and single-page, and the Wheeler pages never reach it at all. The
    //   memory is a small per-page array of dedup keys behind m_seatingMutex.
    // =============================================================================

    class SlotAllocator
    {
    public:
        static SlotAllocator& GetSingleton();

        // =========================================================================
        // INITIALIZATION (Legacy - for backwards compatibility)
        // =========================================================================

        /// Initialize from SlotSettings (call after SlotSettings::LoadFromFile)
        /// This is optional - allocation methods can work without initialization
        /// by reading directly from SlotSettings.
        void Initialize();

        /// Reset allocator state
        void Reset();

        /// Check if initialized (legacy - always returns true now)
        [[nodiscard]] bool IsInitialized() const noexcept { return true; }

        // =========================================================================
        // PAGE MANAGEMENT
        // =========================================================================

        /// Get current active page index
        [[nodiscard]] size_t GetCurrentPage() const noexcept { return m_currentPage; }

        /// Set current active page (bounds-checked)
        void SetCurrentPage(size_t pageIndex);

        /// Cycle to next page (wraps around)
        void NextPage();

        /// Cycle to previous page (wraps around)
        void PreviousPage();

        /// Get total page count (from SlotSettings)
        [[nodiscard]] size_t GetPageCount() const;

        // =========================================================================
        // MAIN ALLOCATION API
        // =========================================================================

        /// Allocate scored candidates to slots for the CURRENT page.
        ///
        /// DEAD (no production callers): the pipeline pins the resolved page and
        /// calls AllocateSlotsForPage directly. Kept — with the 1-arg overload
        /// below, its only caller — as the legacy/test allocation entry point.
        /// Prefer AllocateSlotsForPage(GetCurrentPage(), ...) so allocation and the
        /// page snapshot can't diverge.
        ///
        /// @param candidates     Scored candidates from UtilityScorer (sorted by utility)
        /// @param overrides      Active overrides from OverrideManager (already prioritized)
        /// @param player         Current player state (for context-aware decisions)
        /// @param world          Current world state (for context-aware decisions)
        /// @return               SlotAssignments ready for widget/wheeler
        [[nodiscard]] SlotAssignments AllocateSlots(
            const Scoring::ScoredCandidateList& candidates,
            const Override::OverrideCollection& overrides,
            const State::PlayerActorState& player,
            const State::WorldState& world) const;

        /// Allocate for a SPECIFIC page.
        ///
        /// @param pageIndex      Page index (0-based, max 3)
        /// @param candidates     Scored candidates from UtilityScorer
        /// @param overrides      Active overrides from OverrideManager
        /// @param player         Current player state
        /// @param world          Current world state
        /// @return               SlotAssignments for the specified page
        [[nodiscard]] SlotAssignments AllocateSlotsForPage(
            size_t pageIndex,
            const Scoring::ScoredCandidateList& candidates,
            const Override::OverrideCollection& overrides,
            const State::PlayerActorState& player,
            const State::WorldState& world) const;

        /// Simplified allocation without overrides (for testing/legacy)
        [[nodiscard]] SlotAssignments AllocateSlots(
            const Scoring::ScoredCandidateList& candidates) const;

        // =========================================================================
        // CONFIGURATION ACCESS
        // =========================================================================

        /// Get the number of slots in current page
        [[nodiscard]] size_t GetSlotCount() const;

        /// Get the number of slots in a specific page
        [[nodiscard]] size_t GetSlotCount(size_t pageIndex) const;

        /// Number of slots on a page whose config permits wildcards
        /// (bWildcardsEnabled). This is a page-level CAPACITY, not a per-index
        /// map: FindBestCandidate skips wildcard candidates for slots that
        /// forbid them, but which slot takes a given wildcard is decided by
        /// classification and priority, so no caller can know in advance. What
        /// it can know is that a page with N such slots can never display more
        /// than N wildcards — WildcardManager rolls against this so it cannot
        /// cache one that has no seat. 0 if the page index is out of range.
        [[nodiscard]] size_t GetWildcardSlotCount(size_t pageIndex) const;

        /// Get configuration for a specific slot in current page (returns copy)
        [[nodiscard]] SlotConfig GetSlotConfig(size_t slotIndex) const;

        /// True when the slot is unrestricted (SlotClassification::Regular).
        /// Any page, not only the current one; out of range reads as regular.
        [[nodiscard]] bool IsRegularSlot(size_t pageIndex, size_t slotIndex) const;

        /// Get all slot configurations for current page (returns copy)
        [[nodiscard]] std::vector<SlotConfig> GetSlotConfigs() const;

        /// Get page name for a specific page index (returns copy). "Page" if OOR.
        [[nodiscard]] std::string GetPageName(size_t pageIndex) const;

        /// Get page name for current page (returns copy)
        [[nodiscard]] std::string GetCurrentPageName() const;

        /// Check and consume page-changed flag (for update loop to detect page cycling)
        [[nodiscard]] bool ConsumePageChanged() noexcept { return m_pageChanged.exchange(false); }

        /// Non-destructive peek at the page-changed flag.
        /// Used by the update loop to check before the pipeline guard, so the
        /// flag isn't lost if the guard fails (e.g., registry still loading).
        [[nodiscard]] bool PeekPageChanged() const noexcept { return m_pageChanged.load(); }

        /// Force a pipeline recompute of the current page without changing which
        /// page is active. General "something off-state changed, re-run" signal:
        /// a consumer (e.g. IntuitionMenu) missed a page change while disabled;
        /// Wheeler closed; the inventory changed (GameState hash excludes it,
        /// so consumed/dropped items would otherwise linger); or the equipped
        /// ammo count changed, which the hash excludes for the same reason and
        /// which left the widget printing a count 38 seconds stale. Sets the same flag
        /// page cycling does, so it bypasses the pipeline's hash-skip.
        void MarkPageDirty() noexcept { m_pageChanged = true; }

        /// `[SlotLocker] bFillJobKeysFromRegular`: an empty key with a class
        /// takes a matching item standing on a Regular key. The caller checks
        /// the setting; public so the unit test can drive it with a made-up
        /// page, which live play rarely produces on demand.
        void PullIntoEmptyJobKeys(
            const std::vector<SlotConfig>& slotConfigs,
            SlotAssignments& assignments,
            const State::PlayerActorState* player,
            const std::array<size_t, MAX_SLOTS_PER_PAGE>& priorityOrder,
            size_t priorityCount) const;

#ifndef NDEBUG
        /// Tests only: one allocation of a made-up layout on `pageIndex`, with
        /// no player and the given overrides, under layout generation `generation`.
        /// Two calls with the same generation run the seating memory and the
        /// slot hold, as two pipeline passes would. Leaves seating behind for
        /// that generation; the caller Reset()s.
        [[nodiscard]] SlotAssignments AllocateForTest(size_t pageIndex, uint32_t generation,
            const std::vector<SlotConfig>& slotConfigs,
            const Scoring::ScoredCandidateList& candidates,
            const Override::OverrideCollection& overrides = {}) const;

        /// Tests only: the layout generation real allocations run under now,
        /// so a test allocation can share the seating memory without making
        /// every page start over (the slot capture's campaign, SlotCapture.cpp).
        [[nodiscard]] uint32_t CurrentGenerationForTest() const;
#endif

    private:
        SlotAllocator() = default;
        ~SlotAllocator() = default;
        SlotAllocator(const SlotAllocator&) = delete;
        SlotAllocator& operator=(const SlotAllocator&) = delete;

        // Current active page index (atomic: read from update thread, written from input thread)
        std::atomic<size_t> m_currentPage{0};

        // Dirty flag: set by page cycling input, consumed by update loop
        std::atomic<bool> m_pageChanged{false};

        // Log-dedup caches (mutable: AllocateSlotsInternal is const, these are
        // just log noise filters). Guarded by m_logMutex because allocation can
        // run on both the update thread (current page) and a Wheeler callback
        // thread (non-current pages) — std::set is not thread-safe.
        // Unplaced-override warns latch per condition enum, never per reason
        // string: reasons can embed live values (e.g. ammo counts) that would
        // re-fire the warn every tick. Cleared on Reset() and re-armed by
        // Initialize() when the config changes.
        mutable std::mutex m_logMutex;
        mutable std::set<SlotClassification> m_loggedMissingClassifications;
        mutable std::set<Override::OverrideCondition> m_warnedUnplacedConditions;

        // Override placement log dedup, per page x condition: the last item and
        // slot logged, and whether the condition was last seen displaced. A
        // member under m_logMutex, NOT thread_local: allocation runs on more
        // than one thread (see above), and a thread_local copy per thread
        // re-logged the same placement every tick (LoreRim 2026-10-01: 'LOW
        // AMMO' x10 in one second, page 0 and page 1 each tick).
        struct OverrideLogEntry
        {
            RE::FormID formID = 0;
            size_t slot = SIZE_MAX;
            bool displaced = false;
            [[nodiscard]] bool Empty() const noexcept { return formID == 0 && slot == SIZE_MAX && !displaced; }
        };
        mutable std::array<std::array<OverrideLogEntry, Override::OVERRIDE_CONDITION_COUNT>, MAX_PAGES>
            m_overrideLogs{};

        // What the class cap kept off each page last pass (its [SlotClassCap] line),
        // so [SlotClassCap] logs when that changes, not every pass. m_logMutex.
        mutable std::array<std::string, MAX_PAGES> m_classCapLog{};

        /// Should this placement be logged? Returns why (for the debug line), or
        /// nullptr to stay quiet, and records the placement either way.
        [[nodiscard]] const char* NoteOverridePlaced(size_t page,
            Override::OverrideCondition condition, RE::FormID formID, size_t slot) const;
        /// True on the transition into displaced; records it.
        [[nodiscard]] bool NoteOverrideDisplaced(size_t page, Override::OverrideCondition condition) const;
        /// Forget every page's placements (no override active any more).
        void ResetOverrideLogs(size_t page) const;

        // Config snapshot cache — avoids a SlotSettings shared_lock + vector copy
        // on every allocation tick. Refreshed only when SlotSettings bumps its
        // generation (INI reload). The shared_ptr keeps the snapshot alive for
        // the duration of any in-flight allocation, even across a concurrent reload.
        mutable std::mutex m_cacheMutex;
        mutable std::shared_ptr<const std::vector<PageConfig>> m_configCache;
        mutable uint32_t m_cacheGeneration = UINT32_MAX;

        /// Get the current page-config snapshot, refreshing from SlotSettings
        /// only when the generation changed. Thread-safe; cheap on the hot path.
        /// @param outGeneration Receives the generation THIS snapshot belongs to.
        ///   Callers that later compare a generation must use this rather than
        ///   re-reading m_cacheGeneration: another thread refreshing the cache in
        ///   between would hand them a number that describes a different layout
        ///   than the configs they are holding.
        [[nodiscard]] std::shared_ptr<const std::vector<PageConfig>> GetConfigSnapshot(
            uint32_t* outGeneration = nullptr) const;

        // =========================================================================
        // INTERNAL HELPERS
        // =========================================================================

        /// Core allocation implementation (takes configs directly).
        /// pageIndex identifies which page slotConfigs belongs to — used only
        /// for per-page log dedup and the config-wide unplaced-override check.
        [[nodiscard]] SlotAssignments AllocateSlotsInternal(
            size_t pageIndex,
            uint32_t configGeneration,
            const std::vector<SlotConfig>& slotConfigs,
            const Scoring::ScoredCandidateList& candidates,
            const Override::OverrideCollection& overrides,
            const State::PlayerActorState& player,
            const State::WorldState& world,
            bool forTest = false) const;

        /// True if any slot on ANY configured page accepts this override
        /// category. Overrides are global; a page without an accepting slot is
        /// not a config problem as long as some other page has one.
        [[nodiscard]] bool AnyPageAcceptsCategory(Override::OverrideCategory category) const;

        /// Config-wide placeability check: for every ENABLED override condition,
        /// verify some slot on some page accepts its category. Warns to the log
        /// and surfaces an in-game notification listing the unplaceable ones.
        /// Also warns (log only) when more enabled conditions map to a category
        /// than the best single page has accepting slots — a contention risk
        /// that can starve lower-priority overrides when several fire at once.
        /// Called from Initialize() (startup + every settings reload).
        void ValidateOverridePlaceability() const;

        // =========================================================================
        // SEATING MEMORY (anti-juggling)
        // =========================================================================
        // One dedup key per slot per page: what sat there when this page was last
        // allocated. 0 means the slot was empty. Rebuilt on every allocation of
        // that page, cleared when the layout generation changes or on Reset().
        mutable std::mutex m_seatingMutex;
        mutable std::array<std::array<uint64_t, MAX_SLOTS_PER_PAGE>, MAX_PAGES> m_seating{};
        /// Where each item was PLACED last pass, which differs from its seat
        /// for a guest -- an item whose own seat an override is occupying.
        /// HoldIncumbents holds guests where they stand; guarded by the same
        /// mutex and generation as m_seating.
        mutable std::array<std::array<uint64_t, MAX_SLOTS_PER_PAGE>, MAX_PAGES> m_lastPlaced{};
        mutable uint32_t m_seatingGeneration = UINT32_MAX;

        /// Home keys: the last HOME_MEMORY_PER_SLOT seat owners that left each
        /// slot's page entirely, most recent first, and when. A seat is freed
        /// the moment its owner leaves the screen; this is what lets the owner
        /// claim it back (ApplySeating). Same mutex and generation as
        /// m_seating, in memory only -- slot management stays stateless across
        /// saves.
        struct Departure
        {
            uint64_t key = 0;
            std::chrono::steady_clock::time_point leftAt{};
        };
        static constexpr size_t HOME_MEMORY_PER_SLOT = 3;
        mutable std::array<std::array<std::array<Departure, HOME_MEMORY_PER_SLOT>, MAX_SLOTS_PER_PAGE>, MAX_PAGES>
            m_departed{};
        /// Per slot: the item that claimed it through home keys and has not sat
        /// in it yet (a returner, or one waiting out an override). Seating and
        /// the hold mark the move that finally puts it there
        /// (SlotAssignment::seatMoved) -- that move, and the gap-filler it
        /// swaps out, are the only ones that release a lock early.
        mutable std::array<std::array<uint64_t, MAX_SLOTS_PER_PAGE>, MAX_PAGES> m_homeClaims{};

        /// One page's seating memory as the slot core's record, and back.
        /// Caller holds m_seatingMutex.
        [[nodiscard]] Core::SlotAlloc::PageMemory MemoryOfLocked(size_t pageIndex) const;
        void SetMemoryLocked(size_t pageIndex, const Core::SlotAlloc::PageMemory& memory) const;

        /// The game's side of an allocation's events: override placement logs
        /// (deduped per page and condition), the Remembrance notes, the
        /// [Hold], [HomeKey] and pull lines, and the home-key telemetry.
        void ReportEvents(
            size_t pageIndex,
            const std::vector<SlotConfig>& slotConfigs,
            const Scoring::ScoredCandidateList& candidates,
            const Override::OverrideCollection& overrides,
            const std::vector<size_t>& overrideIndex,
            const std::vector<Core::SlotAlloc::Event>& events,
            bool forTest) const;
    };

}  // namespace Huginn::Slot
