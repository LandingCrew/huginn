#include "SlotClassifier.h"
#include "spell/SpellData.h"
#include "learning/item/ItemData.h"
#include "scroll/ScrollData.h"

namespace Huginn::Slot
{
    bool SlotClassifier::Matches(
        const Scoring::ScoredCandidate& candidate,
        SlotClassification classification) noexcept
    {
        return Matches(candidate.candidate, classification);
    }

    bool SlotClassifier::Matches(
        const Candidate::CandidateVariant& candidate,
        SlotClassification classification) noexcept
    {
        // Regular accepts everything
        if (classification == SlotClassification::Regular) {
            return true;
        }

        return std::visit([classification](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, Candidate::SpellCandidate>) {
                return MatchesSpell(c, classification);
            }
            else if constexpr (std::is_same_v<T, Candidate::ItemCandidate>) {
                return MatchesItem(c, classification);
            }
            else if constexpr (std::is_same_v<T, Candidate::ScrollCandidate>) {
                return MatchesScroll(c, classification);
            }
            else if constexpr (std::is_same_v<T, Candidate::WeaponCandidate>) {
                return MatchesWeapon(c, classification);
            }
            else if constexpr (std::is_same_v<T, Candidate::AmmoCandidate>) {
                return classification == SlotClassification::AmmoAny ||
                       classification == SlotClassification::Regular;
            }
            else {
                return false;
            }
        }, candidate);
    }

    SlotClassification SlotClassifier::Classify(
        const Scoring::ScoredCandidate& candidate) noexcept
    {
        // Priority order: most specific classification first
        static constexpr SlotClassification priorities[] = {
            SlotClassification::HealingAny,
            SlotClassification::DamageAny,
            SlotClassification::DefensiveAny,
            SlotClassification::BuffsAny,
            SlotClassification::SummonsAny,
            SlotClassification::Utility,
            SlotClassification::FoodAny,        // Before PotionsAny so food gets specific match
            SlotClassification::PotionsAny,
            SlotClassification::ScrollsAny,
            SlotClassification::WeaponsMelee,   // Before WeaponsAny so melee gets specific match
            SlotClassification::WeaponsRanged,
            SlotClassification::WeaponsAny,
            SlotClassification::AmmoAny,
            SlotClassification::SpellsDestruction,
            SlotClassification::SpellsRestoration,
            SlotClassification::SpellsConjuration,
            SlotClassification::SpellsIllusion,
            SlotClassification::SpellsAlteration,
            SlotClassification::SpellsAny,
        };

        for (auto c : priorities) {
            if (Matches(candidate, c)) {
                return c;
            }
        }

        return SlotClassification::Regular;
    }

    // =============================================================================
    // SPELL CLASSIFICATION
    // =============================================================================

    bool SlotClassifier::MatchesSpell(
        const Candidate::SpellCandidate& spell,
        SlotClassification classification) noexcept
    {
        using SpellType = Spell::SpellType;
        using SpellTag = Spell::SpellTag;

        switch (classification) {
            case SlotClassification::DamageAny:
                return spell.type == SpellType::Damage;

            case SlotClassification::HealingAny:
                return spell.type == SpellType::Healing ||
                       Spell::HasTag(spell.tags, SpellTag::RestoreHealth);

            case SlotClassification::BuffsAny:
                return spell.type == SpellType::Buff ||
                       Spell::HasTag(spell.tags, SpellTag::Armor) ||
                       Spell::HasTag(spell.tags, SpellTag::Invisibility) ||
                       Spell::HasTag(spell.tags, SpellTag::Muffle);

            case SlotClassification::DefensiveAny:
                return spell.type == SpellType::Defensive ||
                       Spell::HasTag(spell.tags, SpellTag::Ward) ||
                       Spell::HasTag(spell.tags, SpellTag::Armor);

            case SlotClassification::SummonsAny:
                return spell.type == SpellType::Summon ||
                       Spell::HasTag(spell.tags, SpellTag::Conjuration) ||
                       Spell::HasTag(spell.tags, SpellTag::SummonDaedra) ||
                       Spell::HasTag(spell.tags, SpellTag::SummonUndead) ||
                       Spell::HasTag(spell.tags, SpellTag::SummonCreature) ||
                       Spell::HasTag(spell.tags, SpellTag::BoundWeapon);

            case SlotClassification::Utility:
                return spell.type == SpellType::Utility ||
                       Spell::HasTag(spell.tags, SpellTag::Light) ||
                       Spell::HasTag(spell.tags, SpellTag::DetectLife) ||
                       Spell::HasTag(spell.tags, SpellTag::Telekinesis);

            case SlotClassification::SpellsAny:
                return true;  // All spells match SpellsAny

            // Per-school spell filters
            case SlotClassification::SpellsDestruction:
                return spell.school == Spell::MagicSchool::Destruction;
            case SlotClassification::SpellsRestoration:
                return spell.school == Spell::MagicSchool::Restoration;
            case SlotClassification::SpellsConjuration:
                return spell.school == Spell::MagicSchool::Conjuration;
            case SlotClassification::SpellsIllusion:
                return spell.school == Spell::MagicSchool::Illusion;
            case SlotClassification::SpellsAlteration:
                return spell.school == Spell::MagicSchool::Alteration;

            // Item-type classifications don't match spells
            case SlotClassification::PotionsAny:
            case SlotClassification::ScrollsAny:
            case SlotClassification::WeaponsAny:
            case SlotClassification::WeaponsMelee:
            case SlotClassification::WeaponsRanged:
            case SlotClassification::FoodAny:
            case SlotClassification::AmmoAny:
                return false;

            case SlotClassification::Regular:
                return true;

            default:
                return false;
        }
    }

    // =============================================================================
    // ITEM (POTION) CLASSIFICATION
    // =============================================================================

    bool SlotClassifier::MatchesItem(
        const Candidate::ItemCandidate& item,
        SlotClassification classification) noexcept
    {
        using ItemType = Item::ItemType;
        using ItemTag = Item::ItemTag;

        switch (classification) {
            case SlotClassification::DamageAny:
                // Poisons are damage
                return item.type == ItemType::Poison;

            case SlotClassification::HealingAny:
                return item.type == ItemType::HealthPotion ||
                       Item::HasTag(item.tags, ItemTag::RestoreHealth);

            case SlotClassification::BuffsAny:
                return item.type == ItemType::BuffPotion ||
                       Item::HasTag(item.tags, ItemTag::FortifyHealth) ||
                       Item::HasTag(item.tags, ItemTag::FortifyMagicka) ||
                       Item::HasTag(item.tags, ItemTag::FortifyStamina) ||
                       Item::HasTag(item.tags, ItemTag::FortifyMagicSchool) ||
                       Item::HasTag(item.tags, ItemTag::FortifyCombatSkill) ||
                       Item::HasTag(item.tags, ItemTag::Invisibility);

            case SlotClassification::DefensiveAny:
                return item.type == ItemType::ResistPotion ||
                       Item::HasTag(item.tags, ItemTag::ResistFire) ||
                       Item::HasTag(item.tags, ItemTag::ResistFrost) ||
                       Item::HasTag(item.tags, ItemTag::ResistShock) ||
                       Item::HasTag(item.tags, ItemTag::ResistMagic) ||
                       Item::HasTag(item.tags, ItemTag::ResistPoison);

            case SlotClassification::SummonsAny:
                // No items summon things
                return false;

            case SlotClassification::Utility:
                // Waterbreathing, cure potions, etc.
                return item.type == ItemType::CurePotion ||
                       Item::HasTag(item.tags, ItemTag::Waterbreathing) ||
                       Item::HasTag(item.tags, ItemTag::CureDisease) ||
                       Item::HasTag(item.tags, ItemTag::CurePoison);

            case SlotClassification::PotionsAny:
                // All potions and consumables (not food or soul gems)
                return item.type == ItemType::HealthPotion ||
                       item.type == ItemType::MagickaPotion ||
                       item.type == ItemType::StaminaPotion ||
                       item.type == ItemType::ResistPotion ||
                       item.type == ItemType::BuffPotion ||
                       item.type == ItemType::CurePotion ||
                       item.type == ItemType::Poison;

            case SlotClassification::FoodAny:
                return item.type == ItemType::Food;

            case SlotClassification::SpellsAny:
            case SlotClassification::SpellsDestruction:
            case SlotClassification::SpellsRestoration:
            case SlotClassification::SpellsConjuration:
            case SlotClassification::SpellsIllusion:
            case SlotClassification::SpellsAlteration:
            case SlotClassification::ScrollsAny:
            case SlotClassification::WeaponsAny:
            case SlotClassification::WeaponsMelee:
            case SlotClassification::WeaponsRanged:
            case SlotClassification::AmmoAny:
                return false;

            case SlotClassification::Regular:
                return true;

            default:
                return false;
        }
    }

    // =============================================================================
    // SCROLL CLASSIFICATION
    // =============================================================================

    bool SlotClassifier::MatchesScroll(
        const Candidate::ScrollCandidate& scroll,
        SlotClassification classification) noexcept
    {
        // Scrolls use the same SpellType/SpellTag as spells (via type aliases)
        // Use Spell::HasTag since ScrollTag is an alias for SpellTag
        using ScrollType = Scroll::ScrollType;
        using SpellTag = Spell::SpellTag;

        switch (classification) {
            case SlotClassification::DamageAny:
                return scroll.type == ScrollType::Damage;

            case SlotClassification::HealingAny:
                return scroll.type == ScrollType::Healing ||
                       Spell::HasTag(scroll.tags, SpellTag::RestoreHealth);

            case SlotClassification::BuffsAny:
                return scroll.type == ScrollType::Buff ||
                       Spell::HasTag(scroll.tags, SpellTag::Armor) ||
                       Spell::HasTag(scroll.tags, SpellTag::Invisibility) ||
                       Spell::HasTag(scroll.tags, SpellTag::Muffle);

            case SlotClassification::DefensiveAny:
                return scroll.type == ScrollType::Defensive ||
                       Spell::HasTag(scroll.tags, SpellTag::Ward) ||
                       Spell::HasTag(scroll.tags, SpellTag::Armor);

            case SlotClassification::SummonsAny:
                return scroll.type == ScrollType::Summon ||
                       Spell::HasTag(scroll.tags, SpellTag::Conjuration) ||
                       Spell::HasTag(scroll.tags, SpellTag::SummonDaedra) ||
                       Spell::HasTag(scroll.tags, SpellTag::SummonUndead) ||
                       Spell::HasTag(scroll.tags, SpellTag::SummonCreature);

            case SlotClassification::Utility:
                return scroll.type == ScrollType::Utility ||
                       Spell::HasTag(scroll.tags, SpellTag::Light) ||
                       Spell::HasTag(scroll.tags, SpellTag::DetectLife);

            case SlotClassification::ScrollsAny:
                return true;  // All scrolls match ScrollsAny

            // Per-school filters — scrolls carry school data just like spells
            case SlotClassification::SpellsDestruction:
                return scroll.school == Scroll::MagicSchool::Destruction;
            case SlotClassification::SpellsRestoration:
                return scroll.school == Scroll::MagicSchool::Restoration;
            case SlotClassification::SpellsConjuration:
                return scroll.school == Scroll::MagicSchool::Conjuration;
            case SlotClassification::SpellsIllusion:
                return scroll.school == Scroll::MagicSchool::Illusion;
            case SlotClassification::SpellsAlteration:
                return scroll.school == Scroll::MagicSchool::Alteration;

            case SlotClassification::PotionsAny:
            case SlotClassification::SpellsAny:
            case SlotClassification::WeaponsAny:
            case SlotClassification::WeaponsMelee:
            case SlotClassification::WeaponsRanged:
            case SlotClassification::FoodAny:
            case SlotClassification::AmmoAny:
                return false;

            case SlotClassification::Regular:
                return true;

            default:
                return false;
        }
    }

    // =============================================================================
    // WEAPON CLASSIFICATION
    // =============================================================================

    bool SlotClassifier::MatchesWeapon(
        const Candidate::WeaponCandidate& weapon,
        SlotClassification classification) noexcept
    {
        switch (classification) {
            case SlotClassification::DamageAny:
                // Weapons deal damage, so they match DamageAny
                return true;

            case SlotClassification::WeaponsAny:
                return true;

            case SlotClassification::WeaponsMelee:
                return Weapon::HasTag(weapon.tags, Weapon::WeaponTag::Melee);

            case SlotClassification::WeaponsRanged:
                return Weapon::HasTag(weapon.tags, Weapon::WeaponTag::Ranged);

            // Weapons don't match other classifications
            case SlotClassification::HealingAny:
            case SlotClassification::BuffsAny:
            case SlotClassification::DefensiveAny:
            case SlotClassification::SummonsAny:
            case SlotClassification::Utility:
            case SlotClassification::PotionsAny:
            case SlotClassification::ScrollsAny:
            case SlotClassification::SpellsAny:
            case SlotClassification::SpellsDestruction:
            case SlotClassification::SpellsRestoration:
            case SlotClassification::SpellsConjuration:
            case SlotClassification::SpellsIllusion:
            case SlotClassification::SpellsAlteration:
            case SlotClassification::FoodAny:
            case SlotClassification::AmmoAny:
                return false;

            case SlotClassification::Regular:
                return true;

            default:
                return false;
        }
    }

}  // namespace Huginn::Slot
