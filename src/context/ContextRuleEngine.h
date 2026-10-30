#pragma once

#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"
#include "state/WorldState.h"
#include "ContextWeightConfig.h"
#include "ContextFacts.h"   // R8.4: the facts the rules and the live filters share

#include <algorithm>  // std::max (GetMaxWeight)

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
        float darknessWeight = 0.0f;        // Night Eye / light spells (when it is dark)
        float hungerWeight = 0.0f;          // Food (survival: hungry)
        float alcoholHungerWeight = 0.0f;   // hungerWeight for alcohol, 0 unless opted in
        float coldWeight = 0.0f;            // Warm food / warming spells (survival: cold)

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
        float allyHealWeight = 0.0f;        // A heal aimed at someone else: a follower is hurt
        bool followerPresent = false;       // A living follower is near (gates spells for others)

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
        float soulGemWeight = 0.0f;         // Soul gems (always-on baseline, mirrors weapon/spell/buff)
        float ammoWeight = 0.0f;            // Ammo (when bow equipped, low ammo)
        float boundWeaponWeight = 0.0f;     // Bound weapon spells (when no weapon equipped)
        float weaponWeight = 0.0f;          // Physical weapons (always-on baseline for weapon candidates)
        float spellWeight = 0.0f;           // Spells (always-on baseline for spell candidates on typed slots)

        // =========================================================================
        // BUFF & RESIST POTIONS
        // =========================================================================
        float buffPotionWeight = 0.0f;      // Buff/resist potions (always-on baseline, mirrors weapon/spell)
        float buffCombatWeight = 0.0f;      // Buff potions in combat (pop before/during fights); resists go by element

        // =========================================================================
        // UTILITY / ALWAYS-AVAILABLE
        // =========================================================================
        float baseRelevanceWeight = State::ContextWeightDefaults::BASE_RELEVANCE;  // 0.05: noise floor for items with no specific context

        // =========================================================================
        // HELPER METHODS
        // =========================================================================

        /**
         * @brief Get the maximum weight across all categories.
         * Used for debugging and validation.
         */
        [[nodiscard]] constexpr float GetMaxWeight() const noexcept
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
            maxWeight = std::max(maxWeight, darknessWeight);
            maxWeight = std::max(maxWeight, hungerWeight);
            maxWeight = std::max(maxWeight, coldWeight);
            maxWeight = std::max(maxWeight, fortifySmithingWeight);
            maxWeight = std::max(maxWeight, fortifyEnchantingWeight);
            maxWeight = std::max(maxWeight, fortifyAlchemyWeight);
            maxWeight = std::max(maxWeight, wardWeight);
            maxWeight = std::max(maxWeight, aoeWeight);
            maxWeight = std::max(maxWeight, summonWeight);
            maxWeight = std::max(maxWeight, allyHealWeight);
            maxWeight = std::max(maxWeight, antiUndeadWeight);
            maxWeight = std::max(maxWeight, antiDaedraWeight);
            maxWeight = std::max(maxWeight, antiDragonWeight);
            maxWeight = std::max(maxWeight, stealthWeight);
            maxWeight = std::max(maxWeight, weaponChargeWeight);
            maxWeight = std::max(maxWeight, soulGemWeight);
            maxWeight = std::max(maxWeight, ammoWeight);
            maxWeight = std::max(maxWeight, boundWeaponWeight);
            maxWeight = std::max(maxWeight, weaponWeight);
            maxWeight = std::max(maxWeight, spellWeight);
            maxWeight = std::max(maxWeight, buffPotionWeight);
            maxWeight = std::max(maxWeight, buffCombatWeight);
            maxWeight = std::max(maxWeight, baseRelevanceWeight);
            return maxWeight;
        }
    };

    // R8.4 (0.24.1): the reason derivation off this map (the dominant reason,
    // its weight-field table, the per-slot attribution probe and the two
    // label-only signals) is gone. The label reads each item's own
    // explanation, the largest theta n cap term of its choice score
    // (display/NeedReason.h, display/ExplanationLabel.h).

    // =============================================================================
    // CONTEXT RULE ENGINE -- kept for D2 (R8.4 allowlist)
    // =============================================================================
    // Centralized context assessment engine that evaluates game state and produces
    // normalized [0,1] weights for each category.
    //
    // R8.3 / R8.4: nothing ranks on these weights. They feed only the legacy
    // floors (fMinimumContextWeight, and the utility under fMinimumUtility),
    // i.e. a binary gate on which rows get the choice score's ln learn mult
    // (learning/legacy/LegacyLearnFeature.h), and the v3 log's old parts.
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
