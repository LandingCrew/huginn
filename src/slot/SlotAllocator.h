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
    //
    // Thread Safety:
    //   AllocateSlots() is stateless and thread-safe.
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

        /// Get configuration for a specific slot in current page (returns copy)
        [[nodiscard]] SlotConfig GetSlotConfig(size_t slotIndex) const;

        /// Get all slot configurations for current page (returns copy)
        [[nodiscard]] std::vector<SlotConfig> GetSlotConfigs() const;

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
        // thread (non-current pages) — std::set/std::string are not thread-safe.
        mutable std::mutex m_logMutex;
        mutable std::set<SlotClassification> m_loggedMissingClassifications;
        mutable std::string m_lastUnplacedReason;

        // Config snapshot cache — avoids a SlotSettings shared_lock + vector copy
        // on every allocation tick. Refreshed only when SlotSettings bumps its
        // generation (INI reload). The shared_ptr keeps the snapshot alive for
        // the duration of any in-flight allocation, even across a concurrent reload.
        mutable std::mutex m_cacheMutex;
        mutable std::shared_ptr<const std::vector<PageConfig>> m_configCache;
        mutable uint32_t m_cacheGeneration = UINT32_MAX;

        /// Get the current page-config snapshot, refreshing from SlotSettings
        /// only when the generation changed. Thread-safe; cheap on the hot path.
        [[nodiscard]] std::shared_ptr<const std::vector<PageConfig>> GetConfigSnapshot() const;

        // =========================================================================
        // INTERNAL HELPERS
        // =========================================================================

        /// Core allocation implementation (takes configs directly)
        [[nodiscard]] SlotAssignments AllocateSlotsInternal(
            const std::vector<SlotConfig>& slotConfigs,
            const Scoring::ScoredCandidateList& candidates,
            const Override::OverrideCollection& overrides,
            const State::PlayerActorState& player,
            const State::WorldState& world) const;

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
