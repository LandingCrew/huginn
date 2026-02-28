#include "PriorCalculator.h"
#include <algorithm>

namespace Huginn::Scoring
{
    float PriorCalculator::CalculatePrior(
        const State::GameState& state,
        const State::PlayerActorState& player,
        const Candidate::CandidateVariant& candidate) const
    {
        return std::visit([this, &state, &player](const auto& c) -> float {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, Candidate::SpellCandidate>) {
                return CalculateSpellPrior(state, player, c);
            } else if constexpr (std::is_same_v<T, Candidate::ItemCandidate>) {
                return CalculateItemPrior(state, player, c);
            } else if constexpr (std::is_same_v<T, Candidate::WeaponCandidate>) {
                return CalculateWeaponPrior(state, player, c);
            } else if constexpr (std::is_same_v<T, Candidate::AmmoCandidate>) {
                return CalculateAmmoPrior(state, player, c);
            } else if constexpr (std::is_same_v<T, Candidate::ScrollCandidate>) {
                return CalculateScrollPrior(state, player, c);
            } else {
                return BASE_PRIOR;
            }
        }, candidate);
    }

    // =========================================================================
    // SPELL PRIORS - Intrinsic quality heuristics only (NO context dependencies)
    // =========================================================================

    float PriorCalculator::CalculateSpellPrior(
        const State::GameState& state,
        [[maybe_unused]] const State::PlayerActorState& player,
        const Candidate::SpellCandidate& spell) const
    {
        float prior = BASE_PRIOR;

        // Spell cost bonus (higher cost generally means more powerful spell)
        // Uses baseCost as a proxy for spell power (Expert > Adept > Apprentice)
        // This is INTRINSIC: a 200-cost spell is higher quality than a 20-cost spell
        if (spell.baseCost > 0) {
            float costNormalized = std::min(
                static_cast<float>(spell.baseCost) / MAX_REASONABLE_SPELL_COST,
                1.0f
            );
            prior += costNormalized * COST_SCALE_FACTOR;
        }

        // NO context checks allowed here - that's ContextRuleEngine's job:
        // - NO state.health checks (ContextRuleEngine handles healingWeight)
        // - NO state.inCombat checks (ContextRuleEngine handles damageWeight)
        // - NO state.targetType checks (ContextRuleEngine handles antiUndeadWeight)
        // - NO player.effects checks (ContextRuleEngine handles resistFireWeight)
        // - NO state.isSneaking checks (ContextRuleEngine handles stealthWeight)
        // - NO state.distance checks (ContextRuleEngine handles rangedWeight)

        return std::clamp(prior, 0.0f, 1.0f);
    }

    // =========================================================================
    // ITEM (POTION) PRIORS - Intrinsic quality heuristics only
    // =========================================================================

    float PriorCalculator::CalculateItemPrior(
        const State::GameState& state,
        const State::PlayerActorState& player,
        const Candidate::ItemCandidate& item) const
    {
        float prior = BASE_PRIOR;

        // Magnitude bonus (logarithmic scaling to prevent huge potions from dominating)
        // Higher magnitude = better quality, but with diminishing returns
        // This is INTRINSIC: a 200 HP potion is higher quality than a 50 HP potion
        if (item.magnitude > 0.0f) {
            float magRatio = std::log(1.0f + item.magnitude) /
                            std::log(1.0f + MAGNITUDE_REFERENCE_VALUE);
            prior += std::min(magRatio, 1.0f) * MAGNITUDE_SCALE_FACTOR;
        }

        // Inventory scarcity penalty (prefer not to deplete last few items)
        // This is INTRINSIC: "I only have 2 of these left" vs "I have 50"
        // Not game context - this is about item availability, not whether you NEED it
        if (item.count > 0 && item.count < LOW_COUNT_THRESHOLD) {
            float countRatio = item.count / LOW_COUNT_THRESHOLD;
            prior -= (1.0f - countRatio) * COUNT_PENALTY_SCALE;
        }

        // NO context checks allowed here - that's ContextRuleEngine's job:
        // - NO state.health checks (ContextRuleEngine handles healingWeight)
        // - NO state.magicka/stamina checks (ContextRuleEngine handles restoreMagickaWeight)
        // - NO player.effects checks (ContextRuleEngine handles resistFireWeight, curePoisonWeight)
        // - NO state.isSneaking checks (ContextRuleEngine handles invisibilityWeight)
        // - NO player.survival checks (ContextRuleEngine handles foodWeight, warmthWeight)
        // - NO state.inCombat checks (ContextRuleEngine handles damageWeight)

        return std::clamp(prior, 0.0f, 1.0f);
    }

    // =========================================================================
    // WEAPON PRIORS - Intrinsic quality heuristics only
    // =========================================================================

    float PriorCalculator::CalculateWeaponPrior(
        const State::GameState& state,
        const State::PlayerActorState& player,
        const Candidate::WeaponCandidate& weapon) const
    {
        float prior = BASE_PRIOR;

        // Enchanted weapon charge penalty (intrinsic weapon state, not game context)
        // A weapon with 5% charge is intrinsically worse than 100% charge
        // This is INTRINSIC: the weapon's current charge is part of its quality
        if (weapon.hasEnchantment && weapon.GetChargePercent() < 0.2f) {
            float chargeDeficit = 1.0f - weapon.GetChargePercent();
            prior -= chargeDeficit * CHARGE_PENALTY_SCALE;
        }

        // NO context checks allowed here - that's ContextRuleEngine's job:
        // - NO state.inCombat checks (ContextRuleEngine handles weaponWeight)
        // - NO state.targetType checks (Silver vs Undead is context, not intrinsic)
        // - NO state.distance checks (ranged vs melee range is context)
        // - NO damage comparisons (would require equipped weapon data for context)

        return std::clamp(prior, 0.0f, 1.0f);
    }

    // =========================================================================
    // AMMO PRIORS - Intrinsic quality heuristics only
    // =========================================================================

    float PriorCalculator::CalculateAmmoPrior(
        const State::GameState& state,
        const State::PlayerActorState& player,
        const Candidate::AmmoCandidate& ammo) const
    {
        using namespace Weapon;

        float prior = BASE_PRIOR;

        // Type matching bonus (intrinsic property: does this ammo FIT the equipped weapon?)
        // This is INTRINSIC COMPATIBILITY, not context
        // A bow-compatible arrow is intrinsically better when you have a bow equipped
        bool typeMatch = false;
        if (ammo.type == AmmoType::Arrow && player.hasBowEquipped) {
            typeMatch = true;
        } else if (ammo.type == AmmoType::Bolt && player.hasCrossbowEquipped) {
            typeMatch = true;
        }

        if (typeMatch) {
            prior += AMMO_TYPE_MATCH_BONUS;  // Intrinsic: ammo is compatible with equipped weapon
        }

        // Inventory scarcity bonus (prefer to surface when running low)
        // Note: For ammo this is INVERSE of items - we WANT to know when we're low
        // This is still intrinsic: "I only have 5 arrows left" is item state, not game context
        if (ammo.count > 0 && ammo.count < AMMO_LOW_COUNT_THRESHOLD) {
            float scarcity = 1.0f - (ammo.count / AMMO_LOW_COUNT_THRESHOLD);
            prior += scarcity * AMMO_SCARCITY_SCALE;
        }

        // NO context checks allowed here - that's ContextRuleEngine's job:
        // - NO state.inCombat checks (ContextRuleEngine handles ammoWeight)
        // - NO state.targetType checks (Silver ammo vs Undead is context)

        return std::clamp(prior, 0.0f, 1.0f);
    }

    // =========================================================================
    // SCROLL PRIORS - Minimal (scrolls have no intrinsic quality differences)
    // =========================================================================

    float PriorCalculator::CalculateScrollPrior(
        const State::GameState& state,
        const State::PlayerActorState& player,
        const Candidate::ScrollCandidate& scroll) const
    {
        // Scrolls have no intrinsic properties to compare
        // - No magnitude field (effect data not stored in candidate)
        // - No cost field (one-use items, no magicka cost)
        // - No quality tiers (all scrolls of same type are identical)
        // Their value is entirely contextual (handled by ContextRuleEngine)

        // Example: "Scroll of Healing" value depends on current HP (context)
        //          not on any intrinsic property of the scroll itself

        return BASE_PRIOR;  // Just return base prior for all scrolls
    }

}  // namespace Huginn::Scoring
