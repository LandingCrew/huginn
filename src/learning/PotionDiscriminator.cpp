#include "PotionDiscriminator.h"
#include <algorithm>

namespace Huginn::Scoring
{
    PotionDiscriminator::PotionDiscriminator(const ScorerConfig& config)
        : m_config(config)
    {
    }

    float PotionDiscriminator::GetMultiplier(
        const State::GameState& state,
        const State::PlayerActorState& player,
        const Candidate::CandidateVariant& candidate) const
    {
        // Only applies to items (potions, food, etc.)
        const auto* item = Candidate::TryGetAs<Candidate::ItemCandidate>(candidate);
        if (!item) {
            return 1.0f;  // Not an item, no multiplier
        }

        return CalculateItemMultiplier(state, player, *item);
    }

    float PotionDiscriminator::CalculateItemMultiplier(
        const State::GameState& state,
        const State::PlayerActorState& player,
        const Candidate::ItemCandidate& item) const
    {
        using namespace Item;

        float multiplier = 1.0f;

        // =====================================================================
        // Combat Timing: Regen vs Flat Restore
        // =====================================================================

        bool isRegenPotion = HasTag(item.tags, ItemTag::RegenHealth) ||
                            HasTag(item.tags, ItemTag::RegenMagicka);
        bool isFlatRestore = HasTag(item.tags, ItemTag::RestoreHealth) ||
                            HasTag(item.tags, ItemTag::RestoreMagicka) ||
                            HasTag(item.tags, ItemTag::RestoreStamina);

        // Regen potions are better at combat start (get full duration benefit)
        if (isRegenPotion && IsInCombatStartWindow()) {
            multiplier *= m_config.regenPotionCombatStartMult;
        }

        // Flat restore is better when resource is critical (need immediate help)
        if (isFlatRestore) {
            bool resourceCritical = false;

            if (HasTag(item.tags, ItemTag::RestoreHealth)) {
                resourceCritical = (state.health <= State::HealthBucket::VeryLow);
            } else if (HasTag(item.tags, ItemTag::RestoreMagicka)) {
                resourceCritical = (state.magicka <= State::MagickaBucket::VeryLow);
            } else if (HasTag(item.tags, ItemTag::RestoreStamina)) {
                resourceCritical = (state.stamina <= State::StaminaBucket::VeryLow);
            }

            if (resourceCritical) {
                multiplier *= m_config.flatRestoreLowResourceMult;
            }
        }

        // =====================================================================
        // Value Ranking: Higher Magnitude = Slight Bonus
        // =====================================================================

        // Apply magnitude bonus for potions with significant magnitude
        if (item.magnitude > 0.0f) {
            multiplier += GetMagnitudeBonus(item.magnitude);
        }

        // =====================================================================
        // Situation-Specific Adjustments
        // =====================================================================

        // Multiple enemies → Resist potions more valuable
        // (Taking damage from multiple sources)
        if (state.enemyCount >= State::EnemyCountBucket::Few) {
            if (HasTag(item.tags, ItemTag::ResistFire) ||
                HasTag(item.tags, ItemTag::ResistFrost) ||
                HasTag(item.tags, ItemTag::ResistShock) ||
                HasTag(item.tags, ItemTag::ResistMagic)) {
                multiplier *= PotionConstants::RESIST_MULTIPLE_ENEMIES_MULT;
            }
        }

        // Sustained combat → Regen potions more valuable
        // Load atomics once to ensure consistent view within this check
        bool inCombat = m_inCombat.load(std::memory_order_relaxed);
        float combatDuration = m_combatDuration.load(std::memory_order_relaxed);
        if (inCombat && combatDuration > PotionConstants::SUSTAINED_COMBAT_THRESHOLD && isRegenPotion) {
            multiplier *= PotionConstants::SUSTAINED_COMBAT_REGEN_MULT;
        }

        // Low resource + high magnitude → Slight penalty
        // (Don't waste strong potions when weak ones would suffice)
        if (item.magnitude > PotionConstants::STRONG_POTION_MAGNITUDE) {
            bool needsLess = false;

            // Check if player only needs a small amount
            if (HasTag(item.tags, ItemTag::RestoreHealth)) {
                // If health is only at Medium (41-60%), a strong potion is overkill
                needsLess = (state.health == State::HealthBucket::Medium);
            } else if (HasTag(item.tags, ItemTag::RestoreMagicka)) {
                needsLess = (state.magicka == State::MagickaBucket::Medium);
            } else if (HasTag(item.tags, ItemTag::RestoreStamina)) {
                needsLess = (state.stamina == State::StaminaBucket::Medium);
            }

            if (needsLess) {
                multiplier *= PotionConstants::OVERKILL_PENALTY_MULT;
            }
        }

        // =====================================================================
        // Poison Timing
        // =====================================================================

        if (item.type == ItemType::Poison) {
            // Poisons are best applied at combat start (before engagement)
            if (IsInCombatStartWindow()) {
                multiplier *= PotionConstants::POISON_COMBAT_START_MULT;
            }

            // Paralyze poisons more valuable against single strong enemies
            if (HasTag(item.tags, ItemTag::Paralyze)) {
                if (state.enemyCount == State::EnemyCountBucket::One) {
                    multiplier *= PotionConstants::PARALYZE_SINGLE_TARGET_MULT;
                }
            }

            // Lingering poisons more valuable in sustained combat
            // v0.8: Lingering moved to ItemTagExt
            // Note: combatDuration already loaded above for sustained combat check
            if (HasTagExt(item.tagsExt, ItemTagExt::Lingering) &&
                combatDuration > PotionConstants::LINGERING_POISON_THRESHOLD) {
                multiplier *= PotionConstants::LINGERING_POISON_MULT;
            }
        }

        // =====================================================================
        // Food (Survival Mode)
        // =====================================================================

        if (HasTag(item.tags, ItemTag::SatisfiesHunger)) {
            // If already starving, urgency is handled by prior - but add timing bonus
            // Eating during combat is risky, prefer eating out of combat
            // Note: inCombat already loaded above for sustained combat check
            if (!inCombat) {
                multiplier *= PotionConstants::FOOD_OUT_OF_COMBAT_MULT;
            }
        }

        return std::clamp(multiplier, PotionConstants::MIN_MULTIPLIER, PotionConstants::MAX_MULTIPLIER);
    }

    void PotionDiscriminator::OnCombatStart()
    {
        m_combatDuration.store(0.0f, std::memory_order_relaxed);
        m_inCombat.store(true, std::memory_order_release);  // Release ensures duration is visible
    }

    void PotionDiscriminator::OnCombatEnd()
    {
        m_inCombat.store(false, std::memory_order_relaxed);
        m_combatDuration.store(0.0f, std::memory_order_relaxed);
    }

    void PotionDiscriminator::Update(float deltaSeconds)
    {
        if (m_inCombat.load(std::memory_order_relaxed)) {
            // Proper atomic read-modify-write via CAS loop.
            // std::atomic<float> lacks fetch_add, so compare_exchange_weak
            // ensures no lost updates if called from multiple threads.
            float expected = m_combatDuration.load(std::memory_order_relaxed);
            while (!m_combatDuration.compare_exchange_weak(
                expected, expected + deltaSeconds,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
                // expected is reloaded on failure; loop retries with fresh value
            }
        }
    }

    bool PotionDiscriminator::IsInCombatStartWindow() const
    {
        // Load both atomically for consistent view
        bool inCombat = m_inCombat.load(std::memory_order_acquire);  // Acquire pairs with release in OnCombatStart
        float duration = m_combatDuration.load(std::memory_order_relaxed);
        return inCombat && duration < m_config.combatStartWindow;
    }

    float PotionDiscriminator::GetMagnitudeBonus(float magnitude) const
    {
        // Normalize magnitude to [0, 1] range
        // Typical potion magnitudes:
        //   - Weak: 25-50
        //   - Standard: 50-100
        //   - Strong: 100-200
        //   - Ultimate: 200+
        float normalized = std::clamp(magnitude / PotionConstants::MAGNITUDE_NORMALIZATION, 0.0f, 1.0f);

        // Scale by config value
        return normalized * m_config.magnitudeValueScale;
    }

}  // namespace Huginn::Scoring
