#pragma once

#include "ScoredCandidate.h"
#include "slot/SlotAssignment.h"
#include "slot/SlotClassifier.h"
#include <chrono>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    // Stores a flat FormID -> {rank, utility, ctx, need} map instead of copying
    // the full ScoredCandidateList.
    //
    // Also read by RewardLog at every learner reward, for the chosen item and
    // the page shown beside it.
    // =========================================================================

    class PipelineStateCache
    {
    public:
        static PipelineStateCache& GetSingleton()
        {
            static PipelineStateCache instance;
            return instance;
        }

        // Sentinel rank for candidates beyond the sorted prefix: their true rank
        // is unknown (partial_sort leaves the tail unordered), so attribution
        // must treat them as far-miss (B-low), never near-miss. Consumers
        // compute overshoot = rank - displayedCount, which any real
        // displayedCount keeps far above FAR_MISS_SLOTS for this value.
        static constexpr size_t kUnrankedTail = std::numeric_limits<size_t>::max() / 2;

        // Called from UpdateLoop after scoring + allocation.
        // sortedPrefix: number of leading entries in `scored` that are actually
        // in utility order (UtilityScorer uses partial_sort for top-N only; the
        // tail is in unspecified order). Ranks beyond the prefix are stored as
        // kUnrankedTail so attribution deterministically classifies them as
        // far-miss instead of reading a meaningless tail index.
        // NOTE: wildcard swaps run after sorting, so a prefix rank may hold a
        // wildcard-promoted item — intentional: attribution should see what was
        // actually surfaced, not the pre-wildcard utility order.
        void Update(
            const Scoring::ScoredCandidateList& scored,
            const Slot::SlotAssignments& currentPageAssignments,
            size_t currentPage,
            size_t sortedPrefix)
        {
            std::unique_lock lock(m_mutex);

            m_timestamp.store(SteadyNow(), std::memory_order_release);
            m_currentPage = currentPage;

            // Build FormID -> {rank, utility, ctx, need} map from scored candidates.
            // ctx and need are for the reward-time log (RewardLog), which has to
            // say what the pipeline thought of the chosen item and of everything
            // shown beside it, and only has this cache to ask.
            m_candidateMap.clear();
            for (size_t i = 0; i < scored.size(); ++i) {
                const size_t rank = (i < sortedPrefix) ? i : kUnrankedTail;
                m_candidateMap[scored[i].GetFormID()] = CachedCandidate{
                    rank, scored[i].utility, scored[i].GetContextWeight(),
                    Slot::SlotClassifier::Classify(scored[i]) };
            }

            // Build displayed set (and its per-slot detail) from current page assignments
            m_displayedFormIDs.clear();
            m_displayed.clear();
            for (const auto& assignment : currentPageAssignments) {
                if (!assignment.IsEmpty() && assignment.formID != 0) {
                    m_displayedFormIDs.insert(assignment.formID);
                    m_displayed.push_back(DisplayedSlot{ assignment.slotIndex, assignment.formID, assignment.name });
                }
            }

            logger::trace("[PipelineStateCache] Updated: {} candidates, {} displayed, page {}",
                m_candidateMap.size(), m_displayedFormIDs.size(), m_currentPage);
        }

        // Refresh the cache timestamp without changing data.
        // Called when the pipeline skips scoring (state unchanged) — the cached
        // candidates/assignments are still valid, just need a fresh timestamp
        // so IsStale() doesn't reject external equip events.
        // Lock-free: only touches an atomic counter, no mutex needed.
        void RefreshTimestamp()
        {
            m_timestamp.store(SteadyNow(), std::memory_order_release);
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

        // Everything the reward-time log needs, read under one lock so the
        // chosen item and the shown items come from the same pipeline run.
        struct ShownEntry
        {
            size_t slotIndex = 0;
            RE::FormID formID = 0;
            std::string name;
            bool wasCandidate = false;
            size_t rank = 0;
            float utility = 0.0f;
            float contextWeight = 0.0f;
            Slot::SlotClassification need = Slot::SlotClassification::Regular;
        };
        struct RewardSnapshot
        {
            bool wasCandidate = false;
            size_t rank = 0;
            float utility = 0.0f;
            float contextWeight = 0.0f;
            Slot::SlotClassification need = Slot::SlotClassification::Regular;
            size_t page = 0;
            float ageMs = 0.0f;
            std::vector<ShownEntry> shown;   // Current page, slot order
        };

        [[nodiscard]] RewardSnapshot GetRewardSnapshot(RE::FormID formID) const
        {
            RewardSnapshot snap;
            snap.ageMs = AgeMs();

            std::shared_lock lock(m_mutex);
            snap.page = m_currentPage;

            if (auto it = m_candidateMap.find(formID); it != m_candidateMap.end()) {
                snap.wasCandidate = true;
                snap.rank = it->second.rank;
                snap.utility = it->second.utility;
                snap.contextWeight = it->second.contextWeight;
                snap.need = it->second.need;
            }

            snap.shown.reserve(m_displayed.size());
            for (const auto& d : m_displayed) {
                ShownEntry e{ d.slotIndex, d.formID, d.name };
                if (auto it = m_candidateMap.find(d.formID); it != m_candidateMap.end()) {
                    e.wasCandidate = true;
                    e.rank = it->second.rank;
                    e.utility = it->second.utility;
                    e.contextWeight = it->second.contextWeight;
                    e.need = it->second.need;
                }
                snap.shown.push_back(std::move(e));
            }
            return snap;
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
            return AgeMs() > maxAgeMs;
        }

    private:
        PipelineStateCache() = default;
        ~PipelineStateCache() = default;
        PipelineStateCache(const PipelineStateCache&) = delete;
        PipelineStateCache& operator=(const PipelineStateCache&) = delete;

        static int64_t SteadyNow()
        {
            return std::chrono::steady_clock::now().time_since_epoch().count();
        }

        [[nodiscard]] float AgeMs() const
        {
            auto cachedTicks = m_timestamp.load(std::memory_order_acquire);
            auto elapsed = std::chrono::steady_clock::duration(SteadyNow() - cachedTicks);
            return std::chrono::duration<float, std::milli>(elapsed).count();
        }

        mutable std::shared_mutex m_mutex;
        std::atomic<int64_t> m_timestamp{0};

        // FormID -> {rank, utility, ctx, need} for O(1) lookup
        struct CachedCandidate
        {
            size_t rank = 0;
            float utility = 0.0f;
            float contextWeight = 0.0f;
            Slot::SlotClassification need = Slot::SlotClassification::Regular;  // SlotClassifier::Classify
        };
        std::unordered_map<RE::FormID, CachedCandidate> m_candidateMap;

        // FormIDs that were displayed on the current page
        std::unordered_set<RE::FormID> m_displayedFormIDs;
        struct DisplayedSlot
        {
            size_t slotIndex = 0;
            RE::FormID formID = 0;
            std::string name;
        };
        std::vector<DisplayedSlot> m_displayed;   // Same items, slot order, for RewardLog
        size_t m_currentPage = 0;
    };

}  // namespace Huginn::Learning
