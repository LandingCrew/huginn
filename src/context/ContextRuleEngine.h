#pragma once

#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"
#include "state/WorldState.h"
#include "ContextWeightConfig.h"
#include "ContextReason.h"

namespace Huginn::Context
{
    // =============================================================================
    // CONTEXT WEIGHT MAP
    // =============================================================================
    // Normalized [0,1] weights representing how relevant each category is RIGHT NOW
    // based on current game state.
    //
    // DESIGN PRINCIPLES:
    // - 0.0 = not relevant (item won't surface)
    // - 0.05 = base relevance (noise floor for always-available items)
    // - 0.5 = high relevance
    // - 1.0 = critical/urgent relevance
    //
    // This replaces the old 0-10 scale from CandidateGenerator relevance scoring.
    // The normalization enables multiplicative scoring in UtilityScorer.
    // =============================================================================

    struct ContextWeightMap
    {
        // =========================================================================
        // HEALTH/RESOURCE RESTORATION
        // =========================================================================
        float healingWeight = 0.0f;         // Restore health
        float magickaRestoreWeight = 0.0f;  // Restore magicka
        float staminaRestoreWeight = 0.0f;  // Restore stamina

        // =========================================================================
        // DAMAGE / OFFENSIVE
        // =========================================================================
        float damageWeight = 0.0f;          // General damage spells

        // =========================================================================
        // ELEMENTAL RESISTANCE
        // =========================================================================
        float resistFireWeight = 0.0f;      // Resist fire (when taking fire damage)
        float resistFrostWeight = 0.0f;     // Resist frost (when taking frost damage)
        float resistShockWeight = 0.0f;     // Resist shock (when taking shock damage)
        float resistPoisonWeight = 0.0f;    // Resist poison (when poisoned)
        float resistDiseaseWeight = 0.0f;   // Resist disease (when diseased)

        // =========================================================================
        // ENVIRONMENTAL / SITUATIONAL
        // =========================================================================
        float waterbreathingWeight = 0.0f;  // Waterbreathing (when underwater)
        float unlockWeight = 0.0f;          // Unlock spells (when looking at lock)
        float slowFallWeight = 0.0f;        // Slow fall / become ethereal (when falling)

        // =========================================================================
        // WORKSTATION FORTIFY EFFECTS
        // =========================================================================
        float fortifySmithingWeight = 0.0f;     // Fortify Smithing (at forge)
        float fortifyEnchantingWeight = 0.0f;   // Fortify Enchanting (at enchanter)
        float fortifyAlchemyWeight = 0.0f;      // Fortify Alchemy (at alchemy lab)

        // =========================================================================
        // COMBAT / TACTICAL
        // =========================================================================
        float wardWeight = 0.0f;            // Ward spells (when enemy casting)
        float aoeWeight = 0.0f;             // Area-of-effect spells (multiple enemies)
        float summonWeight = 0.0f;          // Summon spells (in combat, no active summon)

        // =========================================================================
        // TARGET-SPECIFIC
        // =========================================================================
        float antiUndeadWeight = 0.0f;      // Turn undead, anti-undead damage
        float antiDaedraWeight = 0.0f;      // Anti-daedra spells (vs atronachs, dremora)
        float antiDragonWeight = 0.0f;      // Dragonrend, dragon-specific

        // =========================================================================
        // STEALTH
        // =========================================================================
        float stealthWeight = 0.0f;         // Invisibility, muffle (when sneaking)

        // =========================================================================
        // EQUIPMENT
        // =========================================================================
        float weaponChargeWeight = 0.0f;    // Soul gems (when weapon charge low)
        float ammoWeight = 0.0f;            // Ammo (when bow equipped, low ammo)
        float boundWeaponWeight = 0.0f;     // Bound weapon spells (when no weapon equipped)
        float weaponWeight = 0.0f;          // Physical weapons (always-on baseline for weapon candidates)
        float spellWeight = 0.0f;           // Spells (always-on baseline for spell candidates on typed slots)

        // =========================================================================
        // BUFF & RESIST POTIONS
        // =========================================================================
        float buffPotionWeight = 0.0f;      // Buff/resist potions (always-on baseline, mirrors weapon/spell)
        float buffCombatWeight = 0.0f;      // Buff/resist potions in combat (pop before/during fights)

        // =========================================================================
        // UTILITY / ALWAYS-AVAILABLE
        // =========================================================================
        float baseRelevanceWeight = 0.05f;  // Noise floor for items with no specific context

        // =========================================================================
        // HELPER METHODS
        // =========================================================================

        /**
         * @brief Get the maximum weight across all categories.
         * Used for debugging and validation.
         */
        [[nodiscard]] float GetMaxWeight() const noexcept
        {
            float maxWeight = 0.0f;
            maxWeight = std::max(maxWeight, healingWeight);
            maxWeight = std::max(maxWeight, magickaRestoreWeight);
            maxWeight = std::max(maxWeight, staminaRestoreWeight);
            maxWeight = std::max(maxWeight, damageWeight);
            maxWeight = std::max(maxWeight, resistFireWeight);
            maxWeight = std::max(maxWeight, resistFrostWeight);
            maxWeight = std::max(maxWeight, resistShockWeight);
            maxWeight = std::max(maxWeight, resistPoisonWeight);
            maxWeight = std::max(maxWeight, resistDiseaseWeight);
            maxWeight = std::max(maxWeight, waterbreathingWeight);
            maxWeight = std::max(maxWeight, unlockWeight);
            maxWeight = std::max(maxWeight, slowFallWeight);
            maxWeight = std::max(maxWeight, fortifySmithingWeight);
            maxWeight = std::max(maxWeight, fortifyEnchantingWeight);
            maxWeight = std::max(maxWeight, fortifyAlchemyWeight);
            maxWeight = std::max(maxWeight, wardWeight);
            maxWeight = std::max(maxWeight, aoeWeight);
            maxWeight = std::max(maxWeight, summonWeight);
            maxWeight = std::max(maxWeight, antiUndeadWeight);
            maxWeight = std::max(maxWeight, antiDaedraWeight);
            maxWeight = std::max(maxWeight, antiDragonWeight);
            maxWeight = std::max(maxWeight, stealthWeight);
            maxWeight = std::max(maxWeight, weaponChargeWeight);
            maxWeight = std::max(maxWeight, ammoWeight);
            maxWeight = std::max(maxWeight, boundWeaponWeight);
            maxWeight = std::max(maxWeight, weaponWeight);
            maxWeight = std::max(maxWeight, spellWeight);
            maxWeight = std::max(maxWeight, buffPotionWeight);
            maxWeight = std::max(maxWeight, buffCombatWeight);
            maxWeight = std::max(maxWeight, baseRelevanceWeight);
            return maxWeight;
        }

        /**
         * @brief Check if all weights are zero (no context).
         */
        [[nodiscard]] bool IsAllZero() const noexcept
        {
            return GetMaxWeight() < 0.001f;  // Epsilon for floating point comparison
        }
    };

    // =============================================================================
    // REASON → WEIGHT FIELD
    // =============================================================================
    // Each weight-backed reason is derived from exactly one ContextWeightMap
    // field. That pairing used to be implicit in DominantReason's Mark() calls;
    // it is now named here because a second consumer needs it: the display asks
    // "did THIS candidate draw weight from the field behind the tick's reason?"
    // before letting the reason speak for a slot (Context::ReasonAppliesTo, #64).
    //
    // Single-sourcing matters more than the small indirection costs. If the two
    // ever disagreed, a reason would fire off one field and be attributed off
    // another — labels that are individually plausible and collectively wrong,
    // which is precisely the failure #64 is about.
    // =============================================================================

    /// Pointer-to-member for the weight a reason is read off.
    using ReasonWeightField = float ContextWeightMap::*;

    /// The field behind `reason`, or nullptr for reasons no weight backs:
    /// None, and the three ContextReasonSignals facts (AllyInjured,
    /// LookingAtOre, InDarkness) that nothing scores on.
    [[nodiscard]] ReasonWeightField WeightFieldFor(ContextReason reason) noexcept;

    // =============================================================================
    // CONTEXT REASON SIGNALS
    // =============================================================================
    // Perceivable facts that DominantReason() reports but no scoring rule keys
    // on, so they have no ContextWeightMap entry to read them off:
    //   - injured follower: no heal-other weight exists (healingWeight is the
    //     player's own deficit)
    //   - ore vein / ambient light: no mining or light weight exists
    //
    // They are passed in raw (not pre-thresholded) so every threshold in the
    // reason vocabulary stays in one place — this file's implementation.
    // If any of these ever grows a scoring rule, delete the field and read the
    // weight instead.
    //
    // CAVEAT: having no weight also means having no INI knob. Every other reason
    // can be tuned or silenced from [ContextWeights] (a rule at 0 never reports);
    // these three always report at their hardcoded thresholds. That is the price
    // of labelling a fact nothing scores on — and the reason to prefer adding a
    // weight over adding a field here.
    // =============================================================================

    struct ContextReasonSignals
    {
        bool allyInjured = false;    // A follower is below the low-health threshold
        bool lookingAtOre = false;   // Crosshair on an ore vein
        float lightLevel = 1.0f;     // Ambient light [0,1] (WorldState::lightLevel)
    };

    // =============================================================================
    // CONTEXT RULE ENGINE
    // =============================================================================
    // Centralized context assessment engine that evaluates game state and produces
    // normalized [0,1] weights for each category.
    //
    // REPLACES:
    // - CandidateGenerator::ComputeSpellRelevance() (old 0-10 scale)
    // - CandidateGenerator::ComputeItemRelevance()
    // - CandidateGenerator::ComputeWeaponRelevance()
    // - CandidateGenerator::ComputeScrollRelevance()
    // - PriorCalculator's context-based prior adjustments (duplicate logic)
    //
    // ARCHITECTURE:
    // - Single source of truth for context rules
    // - Reads from INI-configurable ContextWeightSettings
    // - Uses continuous functions (not threshold discontinuities)
    // - Game-thread only (SetConfig mutates state without synchronization)
    //
    // USAGE:
    // ```cpp
    // ContextRuleEngine engine(settings);
    // auto weights = engine.EvaluateRules(player, targets, world);
    // float healingWeight = weights.healingWeight;  // 0.0-1.0
    // ```
    // =============================================================================

    class ContextRuleEngine
    {
    public:
        /**
         * @brief Construct the engine with a config snapshot.
         * @param config Immutable snapshot from ContextWeightSettings::BuildConfig()
         */
        explicit ContextRuleEngine(const State::ContextWeightConfig& config)
            : m_config(config)
        {
        }

        /// Replace the stored config snapshot (e.g., after INI hot-reload).
        void SetConfig(const State::ContextWeightConfig& config) { m_config = config; }

        /**
         * @brief Evaluate all context rules and produce normalized weights.
         *
         * @param player Current player state (vitals, buffs, equipment, position)
         * @param targets Current target collection (enemies, allies, primary target)
         * @param world Current world state (time, light, workstations, locks)
         * @return ContextWeightMap with normalized [0,1] weights for each category
         *
         * THREAD SAFETY: Both SetConfig() and EvaluateRules() must be called
         * from the game thread. Not safe for concurrent read+write from
         * different threads.
         */
        [[nodiscard]] ContextWeightMap EvaluateRules(
            const State::PlayerActorState& player,
            const State::TargetCollection& targets,
            const State::WorldState& world) const;

        /**
         * @brief Name the dominant reason the current context matters (#10).
         *
         * Reads the SAME ContextWeightMap the scorer consumes, so the display
         * explanation and the utility ranking can never disagree: a reason is
         * reported only while its weight is actually driving scoring. Threshold
         * per category is derived from this engine's own config — vitals invert
         * the smoothing curve (weight >= (1-pct)^exp is exactly "vital <= pct"),
         * binary rules fire at half their configured weight, which also lets the
         * resistance-scaled elemental rules go quiet for a resistant player.
         *
         * Returns the first reason in ContextReason order that clears its
         * threshold, or ContextReason::None.
         *
         * @param weights Weight map from EvaluateRules() for THIS tick.
         * @param signals Perceivable facts with no scoring weight of their own
         *        (see ContextReasonSignals).
         */
        [[nodiscard]] ContextReason DominantReason(
            const ContextWeightMap& weights,
            const ContextReasonSignals& signals) const;

    private:
        State::ContextWeightConfig m_config;

        // =========================================================================
        // INTERNAL RULE EVALUATION HELPERS
        // =========================================================================
        // These will be implemented in Stage 1c-1e as rules are migrated.
        // For Stage 1a (skeleton), EvaluateRules() returns all zeros.
        // =========================================================================

        // Stage 1c: Vital-based rules (health, magicka, stamina)
        void EvaluateVitalRules(
            ContextWeightMap& result,
            const State::PlayerActorState& player) const;

        // Stage 1d: Elemental/environmental rules
        void EvaluateElementalRules(
            ContextWeightMap& result,
            const State::PlayerActorState& player) const;

        void EvaluateEnvironmentalRules(
            ContextWeightMap& result,
            const State::PlayerActorState& player,
            const State::WorldState& world) const;

        // Stage 1e: Combat/target/equipment rules
        void EvaluateCombatRules(
            ContextWeightMap& result,
            const State::PlayerActorState& player,
            const State::TargetCollection& targets) const;

        void EvaluateTargetRules(
            ContextWeightMap& result,
            const State::TargetCollection& targets) const;

        void EvaluateEquipmentRules(
            ContextWeightMap& result,
            const State::PlayerActorState& player) const;
    };

}  // namespace Huginn::Context
