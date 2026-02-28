#pragma once

#include "LearningSettings.h"  // For LearningConfig
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace Huginn::Learning
{
    // =========================================================================
    // EXTERNAL EQUIP LEARNER
    // =========================================================================
    // Applies tiered learning rewards when the player equips items through
    // vanilla UI (inventory, favorites, hotkeys) rather than through Huginn.
    //
    // Attribution uses PipelineStateCache to determine what the pipeline
    // "thought" about the item at the time of equip, and applies a scaled
    // reward based on how well Huginn was already surfacing the item.
    //
    // Cases:
    //   A: Not a candidate          → skip (0.0)
    //   B-low: Low-rank candidate   → small reward (0.2×)
    //   B-med: Mid-rank candidate   → medium reward (0.4×)
    //   C: High-rank, not displayed → high reward (0.8×)
    //   D: Displayed, other page    → medium reward (0.5×)
    //   E: Displayed, current page  → skip (Huginn already surfaced it)
    // =========================================================================

    class ExternalEquipLearner
    {
    public:
        static ExternalEquipLearner& GetSingleton()
        {
            static ExternalEquipLearner instance;
            return instance;
        }

        // Called by ExternalEquipListener and SpellRegistry on external equip
        void OnExternalEquip(RE::FormID formID, const char* formType);

        /// Replace the stored config snapshot (e.g., after INI hot-reload).
        void SetConfig(const LearningConfig& config) { m_config = config; }

    private:
        ExternalEquipLearner() = default;
        ~ExternalEquipLearner() = default;
        ExternalEquipLearner(const ExternalEquipLearner&) = delete;
        ExternalEquipLearner& operator=(const ExternalEquipLearner&) = delete;

        // Config snapshot (updated via SetConfig on hot-reload)
        LearningConfig m_config;

        // Anti-spam: FormID → last learning timestamp
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> m_lastLearnTime;
        mutable std::mutex m_mutex;

        // Anti-spam map cleanup thresholds
        static constexpr size_t MAX_ANTI_SPAM_ENTRIES = 200;
        static constexpr float CLEANUP_AGE_SECONDS = 600.0f;  // 10 minutes

        // Slot-relative thresholds: how many ranks past the display cutoff
        // determines the attribution case. E.g., with 5 display slots:
        //   rank 5-7 (overshoot 0-2) → Case C "near-miss" (high reward)
        //   rank 8-10 (overshoot 3-5) → Case B-med (medium reward)
        //   rank 11+ (overshoot 6+) → Case B-low (low reward)
        static constexpr size_t NEAR_MISS_SLOTS = 2;   // Within 2 ranks of display = near-miss
        static constexpr size_t FAR_MISS_SLOTS = 5;     // Within 5 ranks = mid, beyond = low

        // Return true if this equip should be skipped (filtering heuristics)
        bool ShouldSkip(RE::FormID formID) const;

        // Determine reward multiplier and case label from pipeline state
        struct Attribution
        {
            float multiplier = 0.0f;
            const char* caseLabel = "?";
        };
        Attribution ComputeAttribution(RE::FormID formID) const;
    };

}  // namespace Huginn::Learning
