#pragma once

#include "SlotConfig.h"
#include "SlotAssignment.h"
#include "SlotClassifier.h"
#include "SlotSettings.h"
#include "learning/ScoredCandidate.h"
#include "override/OverrideConditions.h"
#include "state/PlayerActorState.h"
#include "state/WorldState.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
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
        /// Wheeler closed; or the inventory changed (GameState hash excludes it,
        /// so consumed/dropped items would otherwise linger). Sets the same flag
        /// page cycling does, so it bypasses the pipeline's hash-skip.
        void MarkPageDirty() noexcept { m_pageChanged = true; }

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
            const State::WorldState& world) const;

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
        mutable uint32_t m_seatingGeneration = UINT32_MAX;

        /// Put items back in the slots they were in last pass, where the layout
        /// still allows it.
        ///
        /// Runs AFTER the rank-ordered fill, so it never changes WHICH items are
        /// shown -- only where they sit. Overrides are pinned (they were placed
        /// in a slot chosen for them, and their subtext says so). It can leave a
        /// slot empty, by moving its occupant back to the seat it wants; the
        /// caller refills those from the remaining candidates before recording,
        /// so seating never opens a hole in the middle of the widget.
        void ApplySeating(
            size_t pageIndex,
            uint32_t generation,
            const std::vector<SlotConfig>& slotConfigs,
            SlotAssignments& assignments,
            const State::PlayerActorState* player) const;

        /// Remember where everything ended up, for the next allocation of this
        /// page. Called after the refill, so an item that has just arrived gets
        /// a seat of its own straight away.
        ///
        /// An override slot keeps the seat of whatever normally lives there, and
        /// the item the override displaced keeps its claim on it rather than
        /// taking a new seat where it is standing. Otherwise one low-health
        /// potion would permanently rehome the item it pushed aside -- the
        /// override clears, the displaced item's seat now says "where I was
        /// pushed to", and seating holds it there for good. Overrides fire
        /// routinely, so that is the feature quietly undoing itself.
        void RecordSeating(
            size_t pageIndex,
            uint32_t generation,
            const SlotAssignments& assignments) const;

        /// Whether `assignment` may sit in slot `slotIndex` of this layout:
        /// the same classification, wildcard and skip-equipped rules
        /// FindBestCandidate applies when it picks one in the first place.
        [[nodiscard]] static bool SlotAccepts(
            const SlotConfig& config,
            const SlotAssignment& assignment,
            const State::PlayerActorState* player);

        /// Compute priority order from configs into a caller-provided buffer
        /// (no heap allocation). Returns the number of valid entries written.
        [[nodiscard]] size_t ComputePriorityOrder(
            const std::vector<SlotConfig>& configs,
            std::array<size_t, MAX_SLOTS_PER_PAGE>& outOrder) const;

        /// Helper: Try to find the best candidate for a slot
        [[nodiscard]] std::optional<Scoring::ScoredCandidate> FindBestCandidate(
            const Scoring::ScoredCandidateList& candidates,
            SlotClassification classification,
            const std::set<RE::FormID>& assignedFormIDs,
            const std::set<std::string_view>& assignedNames,
            bool skipEquipped = false,
            const State::PlayerActorState* player = nullptr,
            bool skipWildcards = false) const;
    };

}  // namespace Huginn::Slot
