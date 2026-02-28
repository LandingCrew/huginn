#include "CandidateTypes.h"

namespace Huginn::Candidate
{
    // =============================================================================
    // SPELL CANDIDATE FACTORY
    // =============================================================================
    SpellCandidate SpellCandidate::FromSpellData(const Spell::SpellData& data)
    {
        SpellCandidate candidate;
        candidate.formID = data.formID;
        candidate.name = data.name;
        candidate.sourceType = SourceType::Spell;

        candidate.type = data.type;
        candidate.tags = data.tags;
        candidate.school = data.school;
        candidate.element = data.element;
        candidate.baseCost = data.baseCost;
        candidate.isConcentration = data.isConcentration;
        candidate.range = data.range;
        candidate.isFavorited = data.isFavorited;

        // canAfford will be set by filters based on current magicka
        candidate.canAfford = true;

        return candidate;
    }

    // =============================================================================
    // ITEM CANDIDATE FACTORIES
    // =============================================================================
    ItemCandidate ItemCandidate::FromItemData(const Item::ItemData& data, int32_t inventoryCount)
    {
        ItemCandidate candidate;
        candidate.formID = data.formID;
        candidate.name = data.name;

        // Map ItemType to appropriate SourceType
        switch (data.type) {
            case Item::ItemType::HealthPotion:
            case Item::ItemType::MagickaPotion:
            case Item::ItemType::StaminaPotion:
            case Item::ItemType::ResistPotion:
            case Item::ItemType::BuffPotion:
            case Item::ItemType::CurePotion:
            case Item::ItemType::Poison:
                candidate.sourceType = SourceType::Potion;
                break;
            case Item::ItemType::Food:
            case Item::ItemType::Ingredient:
                candidate.sourceType = SourceType::Food;
                break;
            case Item::ItemType::SoulGem:
                candidate.sourceType = SourceType::SoulGem;
                break;
            default:
                candidate.sourceType = SourceType::Potion;
                break;
        }

        candidate.type = data.type;
        candidate.tags = data.tags;
        candidate.tagsExt = data.tagsExt;  // v0.8: Extended tags
        candidate.school = data.school;
        candidate.combatSkill = data.combatSkill;
        candidate.utilitySkill = data.utilitySkill;
        candidate.magnitude = data.magnitude;
        candidate.duration = data.duration;
        candidate.count = inventoryCount;

        // Can afford if we have at least one
        candidate.canAfford = inventoryCount > 0;

        return candidate;
    }

    ItemCandidate ItemCandidate::FromInventoryItem(const Item::InventoryItem& invItem)
    {
        return FromItemData(invItem.data, invItem.count);
    }

    // =============================================================================
    // WEAPON CANDIDATE FACTORIES
    // =============================================================================
    WeaponCandidate WeaponCandidate::FromWeaponData(const Weapon::WeaponData& data, bool favorited)
    {
        WeaponCandidate candidate;
        candidate.formID = data.formID;
        candidate.uniqueID = data.uniqueID;
        candidate.name = data.name;

        // Staff is a special weapon type
        if (data.type == Weapon::WeaponType::Staff) {
            candidate.sourceType = SourceType::Staff;
        } else {
            candidate.sourceType = SourceType::Weapon;
        }

        candidate.type = data.type;
        candidate.tags = data.tags;
        candidate.baseDamage = data.baseDamage;
        candidate.hasEnchantment = data.hasEnchantment;
        candidate.currentCharge = data.currentCharge;
        candidate.maxCharge = data.maxCharge;
        candidate.isFavorited = favorited;

        // Weapons are always "affordable" (no cost to equip)
        candidate.canAfford = true;

        return candidate;
    }

    WeaponCandidate WeaponCandidate::FromInventoryWeapon(const Weapon::InventoryWeapon& invWeapon)
    {
        WeaponCandidate candidate = FromWeaponData(invWeapon.data, invWeapon.isFavorited);
        candidate.isEquipped = invWeapon.isEquipped;
        return candidate;
    }

    // =============================================================================
    // AMMO CANDIDATE FACTORIES
    // =============================================================================
    AmmoCandidate AmmoCandidate::FromAmmoData(const Weapon::AmmoData& data, int32_t inventoryCount)
    {
        AmmoCandidate candidate;
        candidate.formID = data.formID;
        candidate.name = data.name;
        candidate.sourceType = SourceType::Ammo;

        candidate.type = data.type;
        candidate.tags = data.tags;
        candidate.baseDamage = data.baseDamage;
        candidate.hasEnchantment = data.hasEnchantment;
        candidate.count = inventoryCount;

        // Can afford if we have at least one
        candidate.canAfford = inventoryCount > 0;

        return candidate;
    }

    AmmoCandidate AmmoCandidate::FromInventoryAmmo(const Weapon::InventoryAmmo& invAmmo)
    {
        AmmoCandidate candidate = FromAmmoData(invAmmo.data, invAmmo.count);
        candidate.isEquipped = invAmmo.isEquipped;
        return candidate;
    }

    // =============================================================================
    // SCROLL CANDIDATE FACTORIES
    // =============================================================================
    ScrollCandidate ScrollCandidate::FromScrollData(const Scroll::ScrollData& data, int32_t inventoryCount)
    {
        ScrollCandidate candidate;
        candidate.formID = data.formID;
        candidate.name = data.name;
        candidate.sourceType = SourceType::Scroll;

        candidate.type = data.type;
        candidate.tags = data.tags;
        candidate.school = data.school;
        candidate.element = data.element;
        candidate.magnitude = data.magnitude;
        candidate.duration = data.duration;
        candidate.count = inventoryCount;

        // Can afford if we have at least one
        candidate.canAfford = inventoryCount > 0;

        return candidate;
    }

    ScrollCandidate ScrollCandidate::FromInventoryScroll(const Scroll::InventoryScroll& invScroll)
    {
        return FromScrollData(invScroll.data, invScroll.count);
    }

}  // namespace Huginn::Candidate
