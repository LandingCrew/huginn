#pragma once

#include "SlotConfig.h"
#include "candidate/CandidateTypes.h"
#include "learning/ScoredCandidate.h"

namespace Huginn::Slot
{
    // =============================================================================
    // SLOT CLASSIFIER
    // =============================================================================
    // Determines whether a candidate matches a slot's classification.
    // Uses existing spell/item tags and types for classification.
    //
    // Classification Rules:
    //   DamageAny:    SpellType::Damage, or Poison items, or damage scrolls
    //   HealingAny:   SpellType::Healing, or HealthPotion items, or healing scrolls
    //   BuffsAny:     SpellType::Buff, or BuffPotion items, or buff scrolls
    //   DefensiveAny: SpellType::Defensive, Ward tag, or ResistPotion items
    //   SummonsAny:   SpellType::Summon, or summon scrolls
    //   Utility:      SpellType::Utility (light, detect, unlock, etc.)
    //   PotionsAny:   Any Potion source type
    //   ScrollsAny:   Any Scroll source type
    //   WeaponsAny:   Any Weapon source type
    //   Regular:      Always matches (no restriction)
    // =============================================================================

    class SlotClassifier
    {
    public:
        // Check if a candidate matches a classification
        [[nodiscard]] static bool Matches(
            const Scoring::ScoredCandidate& candidate,
            SlotClassification classification) noexcept;

        // Check if a candidate variant matches a classification
        [[nodiscard]] static bool Matches(
            const Candidate::CandidateVariant& candidate,
            SlotClassification classification) noexcept;

        // Get the best classification for a candidate (for debugging/display)
        [[nodiscard]] static SlotClassification Classify(
            const Scoring::ScoredCandidate& candidate) noexcept;

    private:
        // Spell-specific classification
        [[nodiscard]] static bool MatchesSpell(
            const Candidate::SpellCandidate& spell,
            SlotClassification classification) noexcept;

        // Item-specific classification (potions, food, soul gems)
        [[nodiscard]] static bool MatchesItem(
            const Candidate::ItemCandidate& item,
            SlotClassification classification) noexcept;

        // Scroll-specific classification
        [[nodiscard]] static bool MatchesScroll(
            const Candidate::ScrollCandidate& scroll,
            SlotClassification classification) noexcept;

        // Weapon-specific classification
        [[nodiscard]] static bool MatchesWeapon(
            const Candidate::WeaponCandidate& weapon,
            SlotClassification classification) noexcept;
    };

}  // namespace Huginn::Slot
