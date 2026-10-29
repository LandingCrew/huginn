#include "PotionDiscriminator.h"
#include "core/CombatOnset.h"   // the one combat clock (R8.1)
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
        const Candidate::CandidateVariant& candidate,
        float combatOnsetAgo) const
    {
        // Only applies to items (potions, food, etc.)
        const auto* item = Candidate::TryGetAs<Candidate::ItemCandidate>(candidate);
        if (!item) {
            return 1.0f;  // Not an item, no multiplier
        }

        return CalculateItemMultiplier(state, player, *item, combatOnsetAgo);
    }

    float PotionDiscriminator::CalculateItemMultiplier(
        const State::GameState& state,
        const State::PlayerActorState& player,
        const Candidate::ItemCandidate& item,
        float combatOnsetAgo) const
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
        if (isRegenPotion && IsInCombatStartWindow(combatOnsetAgo)) {
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

        // Strength ordering WITHIN one potion (Fair vs Faint) is not decided
        // here: a potion scored on its own cannot see its siblings. See
        // UtilityScorer::ApplyPotionTierPreference, which replaced a magnitude
        // bonus here and an "overkill" penalty below. Together they flipped
        // Fair and Faint as health crossed the Medium bucket, and in play the
        // three strengths of Restore Health traded slots every few seconds.

        // =====================================================================
        // Situation-Specific Adjustments
        // =====================================================================

        // Multiple enemies → a resist for a threat the player can SEE is more
        // valuable (taking damage from multiple sources). Gated like
        // CorrelationBooster's melee/no-shield boost: the element they are
        // taking now (effects flags include the hit-enrichment window), or
        // Resist Magic while a hostile casts. It used to lift every resist
        // tag -- foods with a resist rider included -- whatever the element.
        if (state.enemyCount >= State::EnemyCountBucket::Few) {
            const bool threatened =
                (player.effects.isOnFire && HasTag(item.tags, ItemTag::ResistFire)) ||
                (player.effects.isFrozen && HasTag(item.tags, ItemTag::ResistFrost)) ||
                (player.effects.isShocked && HasTag(item.tags, ItemTag::ResistShock)) ||
                (state.anyCasting == State::CastingStatus::EnemyCasting &&
                 HasTag(item.tags, ItemTag::ResistMagic));
            if (threatened) {
                multiplier *= PotionConstants::RESIST_MULTIPLE_ENEMIES_MULT;
            }
        }

        // Sustained combat → Regen potions more valuable. R8.1: in combat and
        // the fight's seconds from the combat_onset clock (core/CombatOnset.h);
        // out of combat the seconds read 0, as the old timer did.
        const bool inCombat = combatOnsetAgo < Core::Needs::kNever;
        const float combatDuration = Core::Needs::CombatSeconds(combatOnsetAgo);
        if (inCombat && combatDuration > PotionConstants::SUSTAINED_COMBAT_THRESHOLD && isRegenPotion) {
            multiplier *= PotionConstants::SUSTAINED_COMBAT_REGEN_MULT;
        }

        // =====================================================================
        // Poison Timing
        // =====================================================================

        if (item.type == ItemType::Poison) {
            // Poisons are best applied at combat start (before engagement)
            if (IsInCombatStartWindow(combatOnsetAgo)) {
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

    bool PotionDiscriminator::IsInCombatStartWindow(float combatOnsetAgo) const
    {
        // R8.1: the combat_onset sensor's clock, not a timer of our own.
        return Core::Needs::InCombatStartWindow(combatOnsetAgo, m_config.combatStartWindow);
    }


}  // namespace Huginn::Scoring
