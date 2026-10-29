#pragma once

#include "CandidateTypes.h"
#include "CandidateConfig.h"
#include "CooldownManager.h"
#include "state/PlayerActorState.h"
#include "core/FilterReason.h"
#include "core/HardGates.h"     // R8.1: the hard gates' inputs
#include <optional>
#include <span>
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
        size_t filteredByDuplication = 0;    // Duplicate items removed
        size_t filteredByFullVitals = 0;     // Healing when health is full, etc.
        size_t filteredByHardGate = 0;       // R8.1: no follower / no workstation / not dark / suppressed favourite
        size_t outputCount = 0;              // Candidates after all filtering

        /// Reset all counters to zero
        void Reset() noexcept {
            inputCount = 0;
            filteredByAffordability = 0;
            filteredByEquipped = 0;
            filteredByCooldown = 0;
            filteredByActiveBuff = 0;
            filteredByDuplication = 0;
            filteredByFullVitals = 0;
            filteredByHardGate = 0;
            outputCount = 0;
        }

        /// Total filtered out
        [[nodiscard]] size_t TotalFiltered() const noexcept {
            return filteredByAffordability + filteredByEquipped + filteredByCooldown +
                   filteredByActiveBuff + filteredByDuplication +
                   filteredByFullVitals + filteredByHardGate;
        }
    };

    /**
     * @brief Pre-filters for candidate selection.
     *
     * Filters are applied in this order (the visitor stage first):
     * 1. Affordability - Can player use this? (magicka, count > 0)
     * 2. Equipped - Is it already equipped?
     * 3. Full Vitals - Is healing/restore pointless?
     * 4. Hard gates (R8.1) - the old engine's context zeros: a spell for
     *    others with no follower near, fortify apparel with no station up,
     *    a torch while it is not dark; and a favourite under Suppress
     * 5. Active Buff - Is the effect already active? (passes again in the
     *    buff's last fRefreshPriorSec seconds: ActorBuffs::refreshDue)
     * 6. Cooldown - Was it used recently?
     * 7. Deduplication, truncation
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

        // R8.1: PassesActiveBuffFilter / PassesFullVitalsFilter are gone.
        // They re-ran the whole visitor and asked for one reason, so once the
        // hard gates ran ahead of the active-buff filters a gated candidate
        // read as passing. Nothing called them; ApplyAllFilters is the API.

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
         * @param gates        R8.1: the hard gates' facts (Context::HardGateInputs)
         * @param stats        Output statistics (reset then populated)
         * @param heldAside    A Remembrance hold's own item that affordability
         *                     or a hard gate dropped, set aside for the hold
         */
        void ApplyAllFilters(
            std::vector<CandidateVariant>& gatherBuffer,
            std::vector<CandidateVariant>& output,
            const State::PlayerActorState& player,
            float currentMagicka,
            const Core::Gate::Inputs& gates,
            FilterStats& stats,
            std::span<const RE::FormID> heldIDs = {},
            std::vector<CandidateVariant>* heldAside = nullptr,
            std::vector<Core::Filter::Dropped>* dropped = nullptr   // 0.23.46: each drop and its reason (v3 log)
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

        // Single std::visit dispatch running affordability, equipped, full
        // vitals, the hard gates (R8.1), active buff: the filter that rejected
        // the candidate, nullopt when it passed. 0.23.46: the reason names the
        // branch (full_health, armor, summon_limit ...), not just the stage,
        // for the v3 log.
        [[nodiscard]] std::optional<Core::Filter::Reason> RunVisitorFilters(
            const CandidateVariant& candidate,
            const State::PlayerActorState& player,
            float currentMagicka,
            const Core::Gate::Inputs& gates
        ) const;

        // Helper: Check resist potion against current resistances (an element
        // whose block is due for its refresh, R8.1, is not redundant)
        [[nodiscard]] bool IsResistPotionRedundant(
            const ItemCandidate& item,
            const State::ActorResistances& resistances,
            Core::Refresh::DueBits refreshDue
        ) const;

        // Helper: Check resist spell against current resistances (as above)
        [[nodiscard]] bool IsResistSpellRedundant(
            const SpellCandidate& spell,
            const State::ActorResistances& resistances,
            Core::Refresh::DueBits refreshDue
        ) const;

    };

}  // namespace Huginn::Candidate
