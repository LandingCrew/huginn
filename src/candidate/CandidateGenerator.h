#pragma once

#include "CandidateTypes.h"
#include "CandidateConfig.h"
#include "CandidateFilters.h"
#include "CooldownManager.h"
#include "spell/SpellRegistry.h"
#include "learning/item/ItemRegistry.h"
#include "weapon/WeaponRegistry.h"
#include "scroll/ScrollRegistry.h"
#include "state/PlayerActorState.h"    // GenerateCandidates / Gather* take PlayerActorState
#include <chrono>
#include <memory>

namespace Huginn::Candidate
{
    /**
     * @brief Statistics from candidate generation for monitoring and debugging.
     */
    struct GenerationStats
    {
        size_t spellsScanned = 0;
        size_t potionsScanned = 0;
        size_t weaponsScanned = 0;
        size_t scrollsScanned = 0;
        size_t ammoScanned = 0;
        size_t soulGemsScanned = 0;
        FilterStats filterStats;
        float generationTimeMs = 0.0f;

        void Reset() noexcept {
            spellsScanned = 0;
            potionsScanned = 0;
            weaponsScanned = 0;
            scrollsScanned = 0;
            ammoScanned = 0;
            soulGemsScanned = 0;
            filterStats.Reset();
            generationTimeMs = 0.0f;
        }
    };

    /**
     * @brief Central orchestrator for candidate generation pipeline.
     *
     * The CandidateGenerator is responsible for:
     * 1. Gathering candidates from all registries (spells, items, weapons, scrolls)
     * 2. Computing relevance tags based on current game state (for filtering)
     * 3. Applying pre-filters (affordability, equipped, cooldown, active buff)
     * 4. Deduplicating candidates
     * 5. Returning filtered candidates for scoring
     *
     * PIPELINE (Stage 1g):
     * ```
     * Registries → Gather → Filter → Deduplicate → Output
     *                                                  ↓
     *                               (Scored by UtilityScorer + ContextRuleEngine)
     * ```
     *
     * NOTE: CandidateGenerator computes neither relevance scores nor display tags.
     * Context weight scoring lives in ContextRuleEngine (via UtilityScorer); the
     * per-tick display relevance tags live in Candidate::ComputeRelevanceTags,
     * called once by the pipeline (critique #10).
     *
     * USAGE:
     * ```cpp
     * auto& generator = CandidateGenerator::GetSingleton();
     * generator.Initialize(spellReg, itemReg, weaponReg, scrollReg);
     *
     * // In update loop
     * generator.Update(deltaSeconds);
     * auto candidates = generator.GenerateCandidates(player, magicka);
     * ```
     *
     * PERFORMANCE TARGET: <2ms per generation
     */
    class CandidateGenerator
    {
    public:
        static CandidateGenerator& GetSingleton();

        /**
         * @brief Initialize the generator with registry references.
         * Must be called before GenerateCandidates().
         */
        void Initialize(
            Spell::SpellRegistry& spellRegistry,
            Item::ItemRegistry& itemRegistry,
            Weapon::WeaponRegistry& weaponRegistry,
            Scroll::ScrollRegistry& scrollRegistry
        );

        /**
         * @brief Check if generator has been initialized.
         */
        [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

        /**
         * @brief Generate and filter candidates for the current player state.
         *
         * @param player Current player state (vitals, buffs, equipment)
         * @param currentMagicka Player's current magicka (for affordability check)
         * @return Vector of filtered candidates, ready for scoring
         * @note Per-tick relevance tags (for the display label) are no longer
         *       produced here — the pipeline computes them once via
         *       Candidate::ComputeRelevanceTags (critique #10).
         */
        [[nodiscard]] std::vector<CandidateVariant> GenerateCandidates(
            const State::PlayerActorState& player,
            float currentMagicka
        );

        /**
         * @brief Update cooldowns. Call each frame/update tick.
         * @param deltaSeconds Time since last update
         * @return true if any cooldown expired this tick (caller must force a
         *         pipeline run so the item can resurface)
         */
        [[nodiscard]] bool Update(float deltaSeconds);

        /**
         * @brief Start cooldown for an item that was used.
         * Called by Wheeler/equip handlers when items are activated.
         */
        void StartCooldown(RE::FormID formID, SourceType type);

        /**
         * @brief Get the last generation statistics.
         */
        [[nodiscard]] const GenerationStats& GetStats() const noexcept { return m_stats; }

        /**
         * @brief Get mutable reference to configuration.
         * @warning NOT THREAD-SAFE: Only call when update loop is paused.
         *          Changes take effect immediately but may cause inconsistent
         *          state if GenerateCandidates() is running concurrently.
         */
        CandidateConfig& GetConfig() noexcept { return m_config; }

        /**
         * @brief Get const reference to configuration.
         */
        [[nodiscard]] const CandidateConfig& GetConfig() const noexcept { return m_config; }

        /**
         * @brief Refresh configuration from the global g_candidateConfig.
         * @note Call this when menu changes are committed, not during generation.
         * @note THREAD-SAFE: Call only when update loop is paused.
         */
        void RefreshConfigFromGlobal() noexcept;

        /**
         * @brief Get the cooldown manager (for external cooldown queries).
         */
        [[nodiscard]] CooldownManager& GetCooldownManager() noexcept { return m_cooldownMgr; }

        /**
         * @brief Log candidates to debug log.
         */
        void LogCandidates(const std::vector<CandidateVariant>& candidates) const;

        /**
         * @brief Reset generator state (clear cooldowns, reset stats).
         */
        void Reset();

    private:
        CandidateGenerator();
        ~CandidateGenerator() = default;

        // Disable copy/move
        CandidateGenerator(const CandidateGenerator&) = delete;
        CandidateGenerator& operator=(const CandidateGenerator&) = delete;

        // =========================================================================
        // REGISTRY REFERENCES
        // =========================================================================

        Spell::SpellRegistry* m_spellRegistry = nullptr;
        Item::ItemRegistry* m_itemRegistry = nullptr;
        Weapon::WeaponRegistry* m_weaponRegistry = nullptr;
        Scroll::ScrollRegistry* m_scrollRegistry = nullptr;

        bool m_initialized = false;

        // =========================================================================
        // INTERNAL COMPONENTS
        // =========================================================================

        CooldownManager m_cooldownMgr;
        CandidateConfig m_config;
        std::unique_ptr<CandidateFilters> m_filters;

        GenerationStats m_stats;

        // Persistent gather buffer — retains capacity across calls.
        // Candidates are gathered here, then survivors are moved into a
        // local output vector by ApplyAllFilters.  Never moved-from, so
        // the allocation persists for the lifetime of the singleton.
        std::vector<CandidateVariant> m_gatherBuffer;

        // =========================================================================
        // CANDIDATE GATHERING (from registries)
        // =========================================================================

        void GatherSpellCandidates(
            std::vector<CandidateVariant>& out,
            const State::PlayerActorState& player
        );

        void GatherPotionCandidates(
            std::vector<CandidateVariant>& out,
            const State::PlayerActorState& player
        );

        void GatherWeaponCandidates(
            std::vector<CandidateVariant>& out,
            const State::PlayerActorState& player
        );

        void GatherAmmoCandidates(
            std::vector<CandidateVariant>& out,
            const State::PlayerActorState& player
        );

        void GatherScrollCandidates(
            std::vector<CandidateVariant>& out,
            const State::PlayerActorState& player
        );

        void GatherSoulGemCandidates(
            std::vector<CandidateVariant>& out,
            const State::PlayerActorState& player
        );

        // =========================================================================
        // RELEVANCE SCORING HELPERS - REMOVED (Stage 1g)
        // =========================================================================
        // All relevance scoring has been moved to ContextRuleEngine.
        // The Compute*Relevance() methods have been removed.
        // Context weight is computed by Context::WeightForCandidate().
    };

}  // namespace Huginn::Candidate
