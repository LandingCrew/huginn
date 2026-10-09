#pragma once

#include "ScoredCandidate.h"
#include "slot/SlotAssignment.h"
#include "slot/SlotClassifier.h"
#include <chrono>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Huginn::Learning
{
    // =========================================================================
    // PIPELINE STATE CACHE
    // =========================================================================
    // Snapshots the most recent pipeline scoring results so that a player
    // selection can record what the pipeline "thought" at the moment of the
    // choice: external-equip attribution (the A-E case labels) and the
    // selection log (SelectionLog), which records the whole scored list.
    //
    // Updated by the update loop after each pipeline run (single writer).
    // Read when a selection is made (SelectionTracker::Select) and on
    // TESEquipEvent attribution -- rare readers.
    //
    // Holds one compact row per scored candidate (formID, slot class, rank and the
    // full ScoreBreakdown -- ~80 bytes) rather than the ScoredCandidateList
    // itself, which carries the whole CandidateVariant and its strings.
    // =========================================================================

    class PipelineStateCache
    {
    public:
        static PipelineStateCache& GetSingleton()
        {
            static PipelineStateCache instance;
            return instance;
        }

        // Sentinel rank for candidates beyond the reported prefix (iTopNCandidates).
        // Until R7 their true rank was unknown (partial_sort left the tail
        // unordered); the list is fully sorted now, but attribution still
        // treats them as far-miss (B-low), never near-miss, as before. Consumers
        // compute overshoot = rank - displayedCount, which any real
        // displayedCount keeps far above FAR_MISS_SLOTS for this value.
        static constexpr size_t kUnrankedTail = std::numeric_limits<size_t>::max() / 2;

        /// One scored candidate as the pipeline saw it. Everything a different
        /// utility formula would need to re-rank it offline.
        struct ScoreRow
        {
            RE::FormID formID = 0;
            Candidate::SourceType sourceType{};
            Slot::SlotClassification slotClass = Slot::SlotClassification::Regular;  // SlotClassifier::Classify
            size_t rank = 0;            // kUnrankedTail past the sorted prefix
            float utility = 0.0f;
            Scoring::ScoreBreakdown breakdown;
            bool isWildcard = false;
            bool isColdStartBoosted = false;
            bool isRememberedOnly = false;
        };

        /// One filled slot on the current page.
        struct ShownSlot
        {
            size_t slotIndex = 0;
            RE::FormID formID = 0;
            std::string name;
            Slot::AssignmentType type = Slot::AssignmentType::Normal;  // Override / Wildcard / Remembered flags
            uint16_t uniqueID = 0;   // R4: the stack (weapons, armour), for the selection log v3
        };

        /// R4: one ELIGIBLE candidate of the run -- every item CandidateGenerator
        /// produced, before the scorer's floors (fMinimumContextWeight,
        /// fMinimumUtility) dropped any. One row per item: a weapon stack is
        /// its own row (formID + uniqueID). Copied out of the run for the
        /// selection log v3 (learning/SelectionLogV3); nothing scores from it.
        struct EligibleRow
        {
            RE::FormID formID = 0;
            uint16_t uniqueID = 0;
            Candidate::SourceType sourceType{};
            int32_t count = 0;                  // inventory count (consumables, ammo, scrolls, torches), else 0
            float chargeFraction = 0.0f;        // weapons/staves: current / max enchantment charge
            bool enchanted = false;             // weapons/staves with an enchantment (charge applies)
            float utility = std::numeric_limits<float>::quiet_NaN();     // when it passed the floors
            float wildcardPropensity = std::numeric_limits<float>::quiet_NaN();  // when it is a wildcard
        };

        /// R4: the page as shown, without the scored list (cheap; the selection
        /// log v3 takes one per logged context).
        struct ShownPage
        {
            bool valid = false;
            size_t page = 0;
            size_t pageSlots = 0;
            float ageMs = 0.0f;
            uint64_t generation = 0;
            std::vector<ShownSlot> shown;
        };

        /// Everything the selection log needs, read under one lock so the scored
        /// list and the page come from the same pipeline run.
        struct Snapshot
        {
            bool valid = false;            // A pipeline run has been cached at all
            size_t page = 0;
            float ageMs = 0.0f;            // How old the run was when the snapshot was taken
            size_t sortedPrefix = 0;
            std::vector<ScoreRow> scores;  // Pipeline order; the first sortedPrefix are ranked
            std::vector<ShownSlot> shown;  // Current page, slot order

            [[nodiscard]] const ScoreRow* Find(RE::FormID formID) const
            {
                for (const auto& row : scores) {
                    if (row.formID == formID) return &row;
                }
                return nullptr;
            }
        };

        // Called from UpdateLoop after scoring + allocation.
        // sortedPrefix: number of leading entries in `scored` whose rank is
        // reported. Until R7 UtilityScorer sorted only the top N (partial_sort)
        // and the tail was in unspecified order; it sorts the whole list now,
        // but ranks beyond the prefix are still stored as kUnrankedTail, so
        // attribution classifies them as far-miss exactly as before.
        // NOTE: wildcard swaps run after sorting, so a prefix rank may hold a
        // wildcard-promoted item — intentional: attribution should see what was
        // actually surfaced, not the pre-wildcard utility order.
        // R4: `eligible` (moved in) and `pageSlots` are kept for the selection
        // log v3 only; attribution and the v2 log read none of it.
        void Update(
            const Scoring::ScoredCandidateList& scored,
            const Slot::SlotAssignments& currentPageAssignments,
            size_t currentPage,
            size_t sortedPrefix,
            std::vector<EligibleRow> eligible = {},
            size_t pageSlots = 0)
        {
            std::unique_lock lock(m_mutex);

            m_timestamp.store(SteadyNow(), std::memory_order_release);
            m_currentPage = currentPage;
            m_sortedPrefix = std::min(sortedPrefix, scored.size());
            m_valid = true;
            m_eligible = std::move(eligible);
            m_pageSlots = pageSlots;
            ++m_generation;

            m_scores.clear();
            m_scores.reserve(scored.size());
            m_index.clear();
            for (size_t i = 0; i < scored.size(); ++i) {
                const auto& sc = scored[i];
                // FIRST row wins, as in Snapshot::Find. One FormID can have
                // several rows (the weapon registry keeps one per stack), and
                // the first is the best-ranked -- attribution and the selection
                // log must agree on which one they mean.
                m_index.try_emplace(sc.GetFormID(), m_scores.size());
                m_scores.push_back(ScoreRow{
                    .formID = sc.GetFormID(),
                    .sourceType = sc.GetSourceType(),
                    .slotClass = Slot::SlotClassifier::Classify(sc),
                    .rank = (i < sortedPrefix) ? i : kUnrankedTail,
                    .utility = sc.utility,
                    .breakdown = sc.breakdown,
                    .isWildcard = sc.isWildcard,
                    .isColdStartBoosted = sc.isColdStartBoosted,
                    .isRememberedOnly = sc.isRememberedOnly,
                });
            }

            m_shown.clear();
            for (const auto& assignment : currentPageAssignments) {
                if (!assignment.IsEmpty() && assignment.formID != 0) {
                    m_shown.push_back(ShownSlot{ assignment.slotIndex, assignment.formID,
                        assignment.name, assignment.type, assignment.uniqueID });
                }
            }

            logger::trace("[PipelineStateCache] Updated: {} candidates, {} displayed, page {}",
                m_scores.size(), m_shown.size(), m_currentPage);
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

            if (auto it = m_index.find(formID); it != m_index.end()) {
                const auto& row = m_scores[it->second];
                info.wasCandidate = true;
                info.rank = row.rank;
                info.utility = row.utility;
            }

            for (const auto& s : m_shown) {
                if (s.formID == formID) {
                    info.wasDisplayed = true;
                    info.displayPage = m_currentPage;
                    break;
                }
            }

            return info;
        }

        /// Copy of the last run, for a selection to keep until it confirms.
        [[nodiscard]] Snapshot TakeSnapshot() const
        {
            Snapshot snap;
            snap.ageMs = AgeMs();

            std::shared_lock lock(m_mutex);
            snap.valid = m_valid;
            snap.page = m_currentPage;
            snap.sortedPrefix = m_sortedPrefix;
            snap.scores = m_scores;
            snap.shown = m_shown;
            return snap;
        }

        /// R4: the run counter (one per Update; 0 before the first).
        [[nodiscard]] uint64_t Generation() const
        {
            std::shared_lock lock(m_mutex);
            return m_generation;
        }

        /// R4: the eligible rows of the last run, if it is newer than
        /// `haveGeneration` (else nothing is copied and false is returned).
        bool TakeEligibleIfNewer(uint64_t haveGeneration, std::vector<EligibleRow>& out, uint64_t& generation) const
        {
            std::shared_lock lock(m_mutex);
            if (m_generation == haveGeneration) return false;
            out = m_eligible;
            generation = m_generation;
            return true;
        }

        /// R4: the page as shown by the last run, and how old it is.
        [[nodiscard]] ShownPage TakeShown() const
        {
            ShownPage page;
            page.ageMs = AgeMs();
            std::shared_lock lock(m_mutex);
            page.valid = m_valid;
            page.page = m_currentPage;
            page.pageSlots = m_pageSlots;
            page.generation = m_generation;
            page.shown = m_shown;
            return page;
        }

        [[nodiscard]] size_t GetCandidateCount() const
        {
            std::shared_lock lock(m_mutex);
            return m_scores.size();
        }

        [[nodiscard]] size_t GetDisplayedCount() const
        {
            std::shared_lock lock(m_mutex);
            return m_shown.size();
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

        bool m_valid = false;
        size_t m_sortedPrefix = 0;
        std::vector<ScoreRow> m_scores;                     // Pipeline order
        std::unordered_map<RE::FormID, size_t> m_index;     // FormID -> m_scores index
        std::vector<ShownSlot> m_shown;                     // Current page, slot order
        size_t m_currentPage = 0;
        // R4 (selection log v3 only)
        std::vector<EligibleRow> m_eligible;
        size_t m_pageSlots = 0;
        uint64_t m_generation = 0;
    };

}  // namespace Huginn::Learning
