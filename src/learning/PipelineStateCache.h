#pragma once

#include "ScoredCandidate.h"
#include "slot/SlotAssignment.h"
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace Huginn::Learning
{
    // =========================================================================
    // PIPELINE STATE CACHE
    // =========================================================================
    // Snapshots the most recent pipeline scoring results so that external equip
    // events (TESEquipEvent) can attribute what the pipeline "thought" at the
    // time the player equipped an item.
    //
    // Updated every ~100ms by the update loop (single writer).
    // Read on TESEquipEvent by SpellRegistry::ProcessEvent (rare reader).
    //
    // Stores a flat FormID -> {rank, utility} map instead of copying the full
    // ScoredCandidateList to keep memory at ~5KB instead of ~25KB.
    // =========================================================================

    class PipelineStateCache
    {
    public:
        static PipelineStateCache& GetSingleton()
        {
            static PipelineStateCache instance;
            return instance;
        }

        // Called from UpdateLoop after scoring + allocation
        void Update(
            const Scoring::ScoredCandidateList& scored,
            const Slot::SlotAssignments& currentPageAssignments,
            size_t currentPage)
        {
            std::unique_lock lock(m_mutex);

            m_timestamp = std::chrono::steady_clock::now();
            m_currentPage = currentPage;

            // Build FormID -> {rank, utility} map from scored candidates
            m_candidateMap.clear();
            for (size_t i = 0; i < scored.size(); ++i) {
                m_candidateMap[scored[i].GetFormID()] = CachedCandidate{i, scored[i].utility};
            }

            // Build displayed FormID set from current page assignments
            m_displayedFormIDs.clear();
            for (const auto& assignment : currentPageAssignments) {
                if (!assignment.IsEmpty() && assignment.formID != 0) {
                    m_displayedFormIDs.insert(assignment.formID);
                }
            }

            logger::trace("[PipelineStateCache] Updated: {} candidates, {} displayed, page {}",
                m_candidateMap.size(), m_displayedFormIDs.size(), m_currentPage);
        }

        // Refresh the cache timestamp without changing data.
        // Called when the pipeline skips scoring (state unchanged) — the cached
        // candidates/assignments are still valid, just need a fresh timestamp
        // so IsStale() doesn't reject external equip events.
        void RefreshTimestamp()
        {
            std::unique_lock lock(m_mutex);
            m_timestamp = std::chrono::steady_clock::now();
        }

        // Query for external equip attribution
        struct CandidateInfo
        {
            bool wasCandidate = false;
            size_t rank = 0;
            float utility = 0.0f;
            bool wasDisplayed = false;
            size_t displayPage = 0;
        };

        [[nodiscard]] CandidateInfo GetCandidateInfo(RE::FormID formID) const
        {
            std::shared_lock lock(m_mutex);

            CandidateInfo info;

            auto it = m_candidateMap.find(formID);
            if (it != m_candidateMap.end()) {
                info.wasCandidate = true;
                info.rank = it->second.rank;
                info.utility = it->second.utility;
            }

            if (m_displayedFormIDs.contains(formID)) {
                info.wasDisplayed = true;
                info.displayPage = m_currentPage;
            }

            return info;
        }

        [[nodiscard]] size_t GetCandidateCount() const
        {
            std::shared_lock lock(m_mutex);
            return m_candidateMap.size();
        }

        [[nodiscard]] size_t GetDisplayedCount() const
        {
            std::shared_lock lock(m_mutex);
            return m_displayedFormIDs.size();
        }

        [[nodiscard]] bool IsStale(float maxAgeMs = 500.0f) const
        {
            std::shared_lock lock(m_mutex);
            auto elapsed = std::chrono::steady_clock::now() - m_timestamp;
            auto elapsedMs = std::chrono::duration<float, std::milli>(elapsed).count();
            return elapsedMs > maxAgeMs;
        }

    private:
        PipelineStateCache() = default;
        ~PipelineStateCache() = default;
        PipelineStateCache(const PipelineStateCache&) = delete;
        PipelineStateCache& operator=(const PipelineStateCache&) = delete;

        mutable std::shared_mutex m_mutex;
        std::chrono::steady_clock::time_point m_timestamp{};

        // FormID -> {rank, utility} for O(1) lookup
        struct CachedCandidate
        {
            size_t rank = 0;
            float utility = 0.0f;
        };
        std::unordered_map<RE::FormID, CachedCandidate> m_candidateMap;

        // FormIDs that were displayed on the current page
        std::unordered_set<RE::FormID> m_displayedFormIDs;
        size_t m_currentPage = 0;
    };

}  // namespace Huginn::Learning
