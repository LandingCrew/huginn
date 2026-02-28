#pragma once

// Only compile in debug mode
#ifdef _DEBUG

#include "../learning/ScoredCandidate.h"
#include "../learning/UsageMemory.h"
#include "../learning/UtilityScorer.h"
#include "../slot/SlotAssignment.h"
#include "../slot/SlotClassifier.h"
#include "../slot/SlotConfig.h"
#include <imgui.h>
#include <shared_mutex>

namespace Huginn::UI
{
    /**
     * @brief Debug widget to display UtilityScorer output — per-slot page-aware view
     *
     * Shows the active page's slot assignments with per-slot candidate breakdown:
     *   - Collapsible sections per slot (classification, winner, runners-up)
     *   - Score breakdown (context, Q, prior, UCB, correlation, multipliers)
     *   - Color-coded headers by assignment type (override/wildcard/normal/empty)
     *   - Event buffer with context hash matching
     *
     * USAGE:
     *   - Call UpdateSlotData() after slot allocation to cache results
     *   - Draw() renders the widget each frame
     *   - Toggle visibility with ToggleVisible()
     *
     * THREAD SAFETY:
     *   - UpdateSlotData() is called from the game update thread
     *   - Draw() is called from the render thread (D3D11 hook)
     *   - Uses copy-out pattern with std::shared_mutex to prevent data races
     */
    class UtilityScorerDebugWidget
    {
    public:
        static UtilityScorerDebugWidget& GetSingleton() noexcept;

        // Disable copy/move
        UtilityScorerDebugWidget(const UtilityScorerDebugWidget&) = delete;
        UtilityScorerDebugWidget(UtilityScorerDebugWidget&&) = delete;
        UtilityScorerDebugWidget& operator=(const UtilityScorerDebugWidget&) = delete;
        UtilityScorerDebugWidget& operator=(UtilityScorerDebugWidget&&) = delete;

        /**
         * @brief Update cached slot data (call after slot allocation)
         * @param candidates Full scored candidate list (for per-slot filtering)
         * @param assignments Slot assignments for the active page
         * @param pageIndex Current page index (0-based)
         * @param pageName Current page display name
         * @param pageCount Total number of pages
         * @param coldStartBoostedCount Number of candidates with cold-start UCB boost
         */
        void UpdateSlotData(const Scoring::ScoredCandidateList& candidates,
                            const Slot::SlotAssignments& assignments,
                            size_t pageIndex,
                            const std::string& pageName,
                            size_t pageCount,
                            size_t coldStartBoostedCount = 0);

        /**
         * @brief Update cached usage memory snapshot for event buffer display
         * @param snapshot Copy of the UsageMemory ring buffer
         * @param currentContextHash Current GameState hash for highlighting matching events
         */
        void UpdateUsageMemory(std::vector<Learning::UsageEvent> snapshot,
                               uint32_t currentContextHash);

        /**
         * @brief Draw the debug widget (called each frame)
         */
        void Draw();

        /**
         * @brief Show or hide the widget
         */
        void SetVisible(bool visible) { m_isVisible = visible; }

        /**
         * @brief Check if widget is visible
         */
        [[nodiscard]] bool IsVisible() const { return m_isVisible; }

        /**
         * @brief Toggle visibility
         */
        void ToggleVisible() { m_isVisible = !m_isVisible; }

        /**
         * @brief Set max candidates to cache (total across all slots)
         */
        void SetMaxDisplay(size_t count) { m_maxDisplay = count; }

    private:
        UtilityScorerDebugWidget() = default;
        ~UtilityScorerDebugWidget() = default;

        // Section renderers
        void DrawSlotSection(const Slot::SlotAssignment& assignment,
                             const Scoring::ScoredCandidateList& candidates,
                             size_t slotIdx);
        void DrawCandidateCard(const Scoring::ScoredCandidate& candidate, size_t rank);
        void DrawScoreSummary(const Scoring::ScoreBreakdown& breakdown);
        void DrawUtilityBar(float utility, float maxUtility);

        // Helpers
        [[nodiscard]] const char* GetSourceTypeName(Candidate::SourceType type) const;
        [[nodiscard]] ImVec4 GetSourceTypeColor(Candidate::SourceType type) const;
        [[nodiscard]] ImVec4 GetAssignmentTypeColor(Slot::AssignmentType type) const;

        // Thread-safe cached state
        // m_mutex protects all m_cached* fields from concurrent access:
        //   - UpdateSlotData() acquires unique_lock (writer)
        //   - Draw() acquires shared_lock, copies out, then releases before iterating
        mutable std::shared_mutex m_mutex;
        Scoring::ScoredCandidateList m_cachedCandidates;           // GUARDED_BY(m_mutex)
        Slot::SlotAssignments m_cachedAssignments;                 // GUARDED_BY(m_mutex)
        size_t m_cachedPageIndex = 0;                              // GUARDED_BY(m_mutex)
        std::string m_cachedPageName;                              // GUARDED_BY(m_mutex)
        size_t m_pageCount = 0;                                    // GUARDED_BY(m_mutex)
        size_t m_coldStartBoostedCount = 0;                        // GUARDED_BY(m_mutex)
        std::vector<Learning::UsageEvent> m_cachedUsageSnapshot;   // GUARDED_BY(m_mutex)
        uint32_t m_cachedContextHash = 0;                          // GUARDED_BY(m_mutex)
        bool m_isVisible = true;  // Visible by default in Debug builds
        size_t m_maxDisplay = 10; // Max total candidates to cache
        size_t m_candidatesPerSlot = 3;  // Runners-up shown per slot section

        // Layout - position on right side to avoid overlap with Registry widget
        float m_posX = 450.0f;  // Right side of screen
        float m_posY = 320.0f;  // Below Cache widget
    };
}

#endif  // _DEBUG
