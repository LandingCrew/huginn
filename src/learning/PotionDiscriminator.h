#pragma once

#include "ScorerConfig.h"
#include "state/GameState.h"
#include "state/PlayerActorState.h"
#include "candidate/CandidateTypes.h"
#include <atomic>

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

        // Potion magnitude thresholds
        constexpr float STRONG_POTION_MAGNITUDE = 100.0f;     // Above this = "strong" potion
        constexpr float MAGNITUDE_NORMALIZATION = 200.0f;     // Max magnitude for normalization

        // Multipliers - situational bonuses
        constexpr float RESIST_MULTIPLE_ENEMIES_MULT = 1.2f;  // Resist potions vs many enemies
        constexpr float SUSTAINED_COMBAT_REGEN_MULT = 1.3f;   // Regen in long fights
        constexpr float OVERKILL_PENALTY_MULT = 0.8f;         // Strong potion when not needed
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
        // Returns value in [0.5, 2.0] range typically
        [[nodiscard]] float GetMultiplier(
            const State::GameState& state,
            const State::PlayerActorState& player,
            const Candidate::CandidateVariant& candidate) const;

        // Track combat start for timing-based decisions
        void OnCombatStart();
        void OnCombatEnd();

        // Update time tracking (call each frame)
        void Update(float deltaSeconds);

    private:
        [[nodiscard]] float CalculateItemMultiplier(
            const State::GameState& state,
            const State::PlayerActorState& player,
            const Candidate::ItemCandidate& item) const;

        // Check if we're in the "combat start" window
        [[nodiscard]] bool IsInCombatStartWindow() const;

        // Calculate magnitude bonus (higher magnitude = slight bonus)
        [[nodiscard]] float GetMagnitudeBonus(float magnitude) const;

        const ScorerConfig& m_config;

        // Combat timing tracking
        // Using atomics to prevent data races between Update()/OnCombatStart()/OnCombatEnd()
        // and GetMultiplier() which may be called from different threads
        std::atomic<bool> m_inCombat{false};
        std::atomic<float> m_combatDuration{0.0f};  // Seconds since combat started
    };

}  // namespace Huginn::Scoring
