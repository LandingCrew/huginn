#pragma once

#include "CandidateTypes.h"
#include "CandidateConfig.h"
#include "CooldownManager.h"
#include "state/PlayerActorState.h"
#include <vector>
#include <unordered_set>

namespace Huginn::Candidate
{
    /**
     * @brief Statistics tracking for filter operations.
     *
     * Used for debugging and performance monitoring to understand
     * how many candidates are filtered at each stage.
     */
    struct FilterStats
    {
        size_t inputCount = 0;               // Total candidates before filtering
        size_t filteredByAffordability = 0;  // Can't afford (no magicka, 0 count)
        size_t filteredByEquipped = 0;       // Already equipped
        size_t filteredByCooldown = 0;       // Recently used
        size_t filteredByActiveBuff = 0;     // Buff already active
        size_t filteredByRelevance = 0;      // Below minimum relevance threshold
        size_t filteredByDuplication = 0;    // Duplicate items removed
        size_t filteredByFullVitals = 0;     // Healing when health is full, etc.
        size_t outputCount = 0;              // Candidates after all filtering

        /// Reset all counters to zero
        void Reset() noexcept {
            inputCount = 0;
            filteredByAffordability = 0;
            filteredByEquipped = 0;
            filteredByCooldown = 0;
            filteredByActiveBuff = 0;
            filteredByRelevance = 0;
            filteredByDuplication = 0;
            filteredByFullVitals = 0;
            outputCount = 0;
        }

        /// Total filtered out
        [[nodiscard]] size_t TotalFiltered() const noexcept {
            return filteredByAffordability + filteredByEquipped + filteredByCooldown +
                   filteredByActiveBuff + filteredByRelevance + filteredByDuplication +
                   filteredByFullVitals;
        }
    };

    /**
     * @brief Pre-filters for candidate selection.
     *
     * Filters are applied in order of computational cost (cheapest first):
     * 1. Affordability - Can player use this? (magicka, count > 0)
     * 2. Equipped - Is it already equipped?
     * 3. Cooldown - Was it used recently?
     * 4. Full Vitals - Is healing/restore pointless?
     * 5. Active Buff - Is the effect already active?
     *
     * Each filter returns true if the candidate should be KEPT.
     */
    class CandidateFilters
    {
    public:
        CandidateFilters(CooldownManager& cooldownMgr, const CandidateConfig& config);

        // =========================================================================
        // INDIVIDUAL FILTERS (return true to KEEP candidate)
        // =========================================================================

        /**
         * @brief Check if player can afford to use this candidate.
         *
         * For spells: Computes effective cost via CalculateMagickaCost(player) with perk/enchant reductions
         * For items: Checks if count > 0
         * For weapons: Always returns true (no cost to equip)
         *
         * @param candidate The candidate to check
         * @param currentMagicka Player's current magicka
         * @return true if player can afford to use this candidate
         */
        [[nodiscard]] bool PassesAffordabilityFilter(
            const CandidateVariant& candidate,
            float currentMagicka
        ) const;

        /**
         * @brief Check if candidate is already equipped.
         *
         * @param candidate The candidate to check
         * @param player Current player state (for equipped items)
         * @return true if candidate is NOT already equipped
         */
        [[nodiscard]] bool PassesEquippedFilter(
            const CandidateVariant& candidate,
            const State::PlayerActorState& player
        ) const;

        /**
         * @brief Check if candidate is on cooldown (recently used).
         *
         * @param candidate The candidate to check
         * @return true if candidate is NOT on cooldown
         */
        [[nodiscard]] bool PassesCooldownFilter(
            const CandidateVariant& candidate
        ) const;

        /**
         * @brief Check if candidate's buff effect is already active.
         *
         * Filters redundant recommendations:
         * - Waterbreathing potion when already has waterbreathing
         * - Resist Fire when fire resistance >= threshold
         * - Invisibility when already invisible
         *
         * @param candidate The candidate to check
         * @param player Current player state (for active buffs/resistances)
         * @return true if buff effect is NOT already active
         */
        [[nodiscard]] bool PassesActiveBuffFilter(
            const CandidateVariant& candidate,
            const State::PlayerActorState& player
        ) const;

        /**
         * @brief Check if restore/healing items are useful.
         *
         * Filters pointless recommendations:
         * - Health potions when health is full
         * - Magicka potions when magicka is full
         * - Stamina potions when stamina is full
         *
         * @param candidate The candidate to check
         * @param player Current player state (for vital percentages)
         * @return true if item would be useful
         */
        [[nodiscard]] bool PassesFullVitalsFilter(
            const CandidateVariant& candidate,
            const State::PlayerActorState& player
        ) const;

        // =========================================================================
        // BATCH FILTER OPERATIONS
        // =========================================================================

        /**
         * @brief Filter gathered candidates into an output vector.
         *
         * Iterates gatherBuffer once, moving candidates that pass all filters
         * into the output vector.  Deduplication is performed inline (no
         * separate erase_if pass).  Affordability penalties and size limiting
         * are applied to the output vector afterward.
         *
         * After the call, gatherBuffer elements are in a moved-from state but
         * the buffer retains its allocated capacity for reuse.
         *
         * @param gatherBuffer Input candidates (elements are moved out)
         * @param output       Destination for surviving candidates (cleared first)
         * @param player       Current player state
         * @param currentMagicka Player's current magicka
         * @param stats        Output statistics (reset then populated)
         */
        void ApplyAllFilters(
            std::vector<CandidateVariant>& gatherBuffer,
            std::vector<CandidateVariant>& output,
            const State::PlayerActorState& player,
            float currentMagicka,
            FilterStats& stats
        );

        // =========================================================================
        // CONFIGURATION
        // =========================================================================

        /// Get reference to configuration
        [[nodiscard]] const CandidateConfig& GetConfig() const noexcept { return m_config; }

        /// Get reference to cooldown manager
        [[nodiscard]] CooldownManager& GetCooldownManager() noexcept { return m_cooldownMgr; }

    private:
        CooldownManager& m_cooldownMgr;
        const CandidateConfig& m_config;

        // Reusable deduplication set (avoids repeated allocation in ApplyAllFilters)
        mutable std::unordered_set<uint64_t> m_deduplicationSet;

        // Consolidated visitor result — which filter rejected a candidate
        enum class FilterResult : uint8_t {
            Passed,
            Affordability,
            Equipped,
            FullVitals,
            ActiveBuff
        };

        // Single std::visit dispatch running affordability, equipped, full vitals, active buff
        [[nodiscard]] FilterResult RunVisitorFilters(
            const CandidateVariant& candidate,
            const State::PlayerActorState& player,
            float currentMagicka
        ) const;

        // Helper: Check resist potion against current resistances
        [[nodiscard]] bool IsResistPotionRedundant(
            const ItemCandidate& item,
            const State::ActorResistances& resistances
        ) const;

        // Helper: Check resist spell against current resistances
        [[nodiscard]] bool IsResistSpellRedundant(
            const SpellCandidate& spell,
            const State::ActorResistances& resistances
        ) const;

    };

}  // namespace Huginn::Candidate
