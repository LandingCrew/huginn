#pragma once

#include "ScorerConfig.h"
#include "state/GameState.h"
#include "state/PlayerActorState.h"
#include "candidate/CandidateTypes.h"

namespace Huginn::Scoring
{
    // =============================================================================
    // POTION DISCRIMINATOR CONSTANTS
    // =============================================================================
    namespace PotionConstants
    {
        // Combat timing thresholds
        constexpr float SUSTAINED_COMBAT_THRESHOLD = 30.0f;   // Seconds for "sustained" combat
        constexpr float LINGERING_POISON_THRESHOLD = 10.0f;   // Seconds before lingering bonus

        // Multipliers - situational bonuses
        constexpr float RESIST_MULTIPLE_ENEMIES_MULT = 1.2f;  // Resist potions vs many enemies
        constexpr float SUSTAINED_COMBAT_REGEN_MULT = 1.3f;   // Regen in long fights
        constexpr float POISON_COMBAT_START_MULT = 1.3f;      // Poison at combat start
        constexpr float PARALYZE_SINGLE_TARGET_MULT = 1.5f;   // Paralyze vs single enemy
        constexpr float LINGERING_POISON_MULT = 1.2f;         // Lingering poison in combat
        constexpr float FOOD_OUT_OF_COMBAT_MULT = 1.2f;       // Safe to eat out of combat

        // Output clamping
        constexpr float MIN_MULTIPLIER = 0.5f;
        constexpr float MAX_MULTIPLIER = 2.5f;
    }

    // =============================================================================
    // POTION DISCRIMINATOR
    // =============================================================================
    // Provides intelligent multipliers for potion selection based on context:
    //
    // 1. Combat Timing:
    //    - Regen potions at combat start (first 10s) → 1.5x
    //    - Flat restore when HP/MP/SP critical → 1.5x
    //
    // 2. Value Ranking:
    //    - Higher magnitude potions get slight bonus
    //    - Prevents wasting strong potions when weak ones suffice
    //
    // 3. Situation Matching:
    //    - Multiple enemies → AoE resist potions preferred
    //    - Sustained combat → Regen preferred over flat restore
    // =============================================================================

    class PotionDiscriminator
    {
    public:
        explicit PotionDiscriminator(const ScorerConfig& config);
        ~PotionDiscriminator() = default;

        // Get multiplier for a candidate (1.0 if not applicable/not a potion)
        // Returns value in [0.5, 2.0] range typically.
        // @param combatOnsetAgo seconds since the current fight began, kNever
        //        out of combat (core/CombatOnset.h): the combat_onset sensor's
        //        clock. R8.1 (0.23.55): the discriminator's own combat timer
        //        (OnCombatStart / OnCombatEnd / Update) is gone; this is the
        //        one combat clock, so its start window and the combat_onset
        //        need agree. The multiplier itself stays: the legacy floors
        //        (R8's D2 feature) read it.
        [[nodiscard]] float GetMultiplier(
            const State::GameState& state,
            const State::PlayerActorState& player,
            const Candidate::CandidateVariant& candidate,
            float combatOnsetAgo) const;

    private:
        [[nodiscard]] float CalculateItemMultiplier(
            const State::GameState& state,
            const State::PlayerActorState& player,
            const Candidate::ItemCandidate& item,
            float combatOnsetAgo) const;

        // In the first fCombatStartWindow seconds of the current fight
        [[nodiscard]] bool IsInCombatStartWindow(float combatOnsetAgo) const;

        const ScorerConfig& m_config;
    };

}  // namespace Huginn::Scoring
