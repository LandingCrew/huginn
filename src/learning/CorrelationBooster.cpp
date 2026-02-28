#include "CorrelationBooster.h"

namespace Huginn::Scoring
{
    CorrelationBooster::CorrelationBooster(const ScorerConfig& config)
        : m_config(config)
    {
    }

    float CorrelationBooster::CalculateBonus(
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const Candidate::CandidateVariant& candidate) const
    {
        float multiplier = std::visit([this, &player, &targets](const auto& c) -> float {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, Candidate::SpellCandidate>) {
                return CalculateSpellCorrelation(player, targets, c);
            } else if constexpr (std::is_same_v<T, Candidate::ItemCandidate>) {
                return CalculateItemCorrelation(player, targets, c);
            } else if constexpr (std::is_same_v<T, Candidate::WeaponCandidate>) {
                return CalculateWeaponCorrelation(player, targets, c);
            } else if constexpr (std::is_same_v<T, Candidate::AmmoCandidate>) {
                return CalculateAmmoCorrelation(player, targets, c);
            } else if constexpr (std::is_same_v<T, Candidate::ScrollCandidate>) {
                return CalculateScrollCorrelation(player, targets, c);
            } else {
                return 1.0f;  // Neutral (no correlation bonus) - prevents zeroing unknown types
            }
        }, candidate);

        return multiplier;
    }

    // =========================================================================
    // SPELL CORRELATIONS
    // =========================================================================

    float CorrelationBooster::CalculateSpellCorrelation(
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const Candidate::SpellCandidate& spell) const
    {
        float multiplier = 1.0f;  // Start at neutral

        // =============================================================================
        // FORTIFY SCHOOL SYNERGY (v0.8.x)
        // =============================================================================
        // When player has Fortify [School] active, spells of that school get multiplier.
        // This is a real game mechanic - fortify potions increase spell effectiveness.
        // =============================================================================
        bool fortifyMatch = false;
        switch (spell.school) {
            case Spell::MagicSchool::Destruction:
                fortifyMatch = player.buffs.hasFortifyDestruction;
                break;
            case Spell::MagicSchool::Conjuration:
                fortifyMatch = player.buffs.hasFortifyConjuration;
                break;
            case Spell::MagicSchool::Restoration:
                fortifyMatch = player.buffs.hasFortifyRestoration;
                break;
            case Spell::MagicSchool::Alteration:
                fortifyMatch = player.buffs.hasFortifyAlteration;
                break;
            case Spell::MagicSchool::Illusion:
                fortifyMatch = player.buffs.hasFortifyIllusion;
                break;
            default:
                break;
        }

        if (fortifyMatch) {
            multiplier *= (1.0f + m_config.fortifySchoolBonus);  // ×3.0 with default 2.0
#ifdef _DEBUG
            logger::trace("[CorrelationBooster] Fortify {} synergy: ×{:.1f} for '{}'",
                Spell::MagicSchoolToString(spell.school),
                1.0f + m_config.fortifySchoolBonus,
                spell.name);
#endif
        }

        // Melee + no shield → Defensive spells (wards) get multiplier
        // (Synergy: player needs magical defense without physical shield)
        if (player.hasMeleeEquipped && !player.hasShieldEquipped) {
            if (Spell::HasTag(spell.tags, Spell::SpellTag::Ward) ||
                spell.type == Spell::SpellType::Defensive) {
                multiplier *= (1.0f + m_config.meleeDefensiveBonus);  // ×2.5 with default 1.5
            }
        }

        // Two-handed weapon + enemies nearby → Defensive spells get multiplier
        // (Synergy: player can't block effectively with 2H, needs magical defense)
        if (player.hasTwoHandedEquipped && targets.GetEnemyCount() > 0) {
            if (Spell::HasTag(spell.tags, Spell::SpellTag::Ward) ||
                Spell::HasTag(spell.tags, Spell::SpellTag::Armor)) {
                multiplier *= (1.0f + m_config.twoHandedDefensiveBonus);  // ×2.2 with default 1.2
            }
        }

        // Staff equipped + low magicka → Staff-castable spells less relevant
        // (Player can use staff instead)
        // This is actually handled by the staff candidate getting a bonus instead

        return multiplier;
    }

    // =========================================================================
    // ITEM CORRELATIONS
    // =========================================================================

    float CorrelationBooster::CalculateItemCorrelation(
        const State::PlayerActorState& player,
        [[maybe_unused]] const State::TargetCollection& targets,
        const Candidate::ItemCandidate& item) const
    {
        float multiplier = 1.0f;  // Start at neutral

        // Melee + no shield → Defensive potions get multiplier
        if (player.hasMeleeEquipped && !player.hasShieldEquipped) {
            // Resist potions become more valuable
            if (Item::HasTag(item.tags, Item::ItemTag::ResistFire) ||
                Item::HasTag(item.tags, Item::ItemTag::ResistFrost) ||
                Item::HasTag(item.tags, Item::ItemTag::ResistShock) ||
                Item::HasTag(item.tags, Item::ItemTag::ResistMagic)) {
                multiplier *= (1.0f + m_config.meleeDefensiveBonus * 0.5f);  // ×1.75 with default 1.5
            }
        }

        // Poisoned melee weapon → Paralyze poison less urgent (already applied)
        // This negative correlation is handled elsewhere

        // Two-handed weapon → Stamina potions get multiplier
        // (Two-handed power attacks cost more stamina)
        if (player.hasTwoHandedEquipped) {
            if (Item::HasTag(item.tags, Item::ItemTag::RestoreStamina) ||
                Item::HasTag(item.tags, Item::ItemTag::FortifyStamina)) {
                multiplier *= 1.5f;  // ×1.5
            }
        }

        // Enchanted weapon with low charge → Soul gems get multiplier
        // (This is primarily handled in relevance tags, but we can add correlation)
        if (player.IsWeaponChargeLow() && item.type == Item::ItemType::SoulGem) {
            multiplier *= 2.0f;  // ×2.0
        }

        return multiplier;
    }

    // =========================================================================
    // WEAPON CORRELATIONS
    // =========================================================================

    float CorrelationBooster::CalculateWeaponCorrelation(
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const Candidate::WeaponCandidate& weapon) const
    {
        float multiplier = 1.0f;  // Start at neutral

        // Silver weapon + undead target → Big multiplier
        if (Weapon::HasTag(weapon.tags, Weapon::WeaponTag::Silver)) {
            if (HasUndeadTarget(targets)) {
                multiplier *= (1.0f + m_config.silverUndeadBonus);  // ×3.0 with default 2.0
            }
        }

        // Ranged weapon + bow/crossbow already equipped with different type → Suggest switch
        // (e.g., have bow equipped, suggest crossbow for different playstyle)
        // This is optional and might cause confusion, so skip for now

        // Melee weapon + currently casting spells → Suggest weapon switch for enemies at melee
        // (Synergy: transition from spellcasting to melee combat)
        if (player.hasSpellEquipped && !player.hasMeleeEquipped) {
            if (Weapon::HasTag(weapon.tags, Weapon::WeaponTag::Melee)) {
                // Small multiplier to suggest having a melee backup
                multiplier *= 1.3f;  // ×1.3
            }
        }

        // Staff + low magicka → Staff gets multiplier
        // (Staffs can cast spells without magicka cost)
        if (weapon.type == Weapon::WeaponType::Staff) {
            if (player.vitals.IsMagickaLow()) {
                multiplier *= (1.0f + m_config.staffLowMagickaBonus);  // ×2.5 with default 1.5
            }
        }

        return multiplier;
    }

    // =========================================================================
    // AMMO CORRELATIONS
    // =========================================================================

    float CorrelationBooster::CalculateAmmoCorrelation(
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const Candidate::AmmoCandidate& ammo) const
    {
        float multiplier = 1.0f;  // Start at neutral

        // Bow equipped → Arrow multiplier
        if (player.hasBowEquipped && ammo.type == Weapon::AmmoType::Arrow) {
            multiplier *= (1.0f + m_config.bowArrowBonus);  // ×3.0 with default 2.0
        }

        // Crossbow equipped → Bolt multiplier
        if (player.hasCrossbowEquipped && ammo.type == Weapon::AmmoType::Bolt) {
            multiplier *= (1.0f + m_config.crossbowBoltBonus);  // ×3.0 with default 2.0
        }

        // Silver ammo + undead target
        if (Weapon::HasTag(ammo.tags, Weapon::WeaponTag::Silver)) {
            if (HasUndeadTarget(targets)) {
                multiplier *= (1.0f + m_config.silverUndeadBonus);  // ×3.0 with default 2.0
            }
        }

        // Magic ammo in combat
        if (Weapon::HasTag(ammo.tags, Weapon::WeaponTag::MagicAmmo)) {
            if (player.isInCombat) {
                multiplier *= 1.5f;  // ×1.5 (magic arrows/bolts are more valuable in combat)
            }
        }

        return multiplier;
    }

    // =========================================================================
    // SCROLL CORRELATIONS
    // =========================================================================

    float CorrelationBooster::CalculateScrollCorrelation(
        const State::PlayerActorState& player,
        [[maybe_unused]] const State::TargetCollection& targets,
        const Candidate::ScrollCandidate& scroll) const
    {
        float multiplier = 1.0f;  // Start at neutral

        // =============================================================================
        // FORTIFY SCHOOL SYNERGY (v0.8.x) - Scrolls benefit from fortify buffs too
        // =============================================================================
        bool fortifyMatch = false;
        switch (scroll.school) {
            case Scroll::MagicSchool::Destruction:
                fortifyMatch = player.buffs.hasFortifyDestruction;
                break;
            case Scroll::MagicSchool::Conjuration:
                fortifyMatch = player.buffs.hasFortifyConjuration;
                break;
            case Scroll::MagicSchool::Restoration:
                fortifyMatch = player.buffs.hasFortifyRestoration;
                break;
            case Scroll::MagicSchool::Alteration:
                fortifyMatch = player.buffs.hasFortifyAlteration;
                break;
            case Scroll::MagicSchool::Illusion:
                fortifyMatch = player.buffs.hasFortifyIllusion;
                break;
            default:
                break;
        }

        if (fortifyMatch) {
            multiplier *= (1.0f + m_config.fortifySchoolBonus);  // ×3.0 with default 2.0
#ifdef _DEBUG
            logger::trace("[CorrelationBooster] Fortify {} synergy: ×{:.1f} for scroll '{}'",
                Spell::MagicSchoolToString(scroll.school),
                1.0f + m_config.fortifySchoolBonus,
                scroll.name);
#endif
        }

        // Melee + no shield → Defensive scrolls (wards) get multiplier
        if (player.hasMeleeEquipped && !player.hasShieldEquipped) {
            if (Spell::HasTag(scroll.tags, Spell::SpellTag::Ward) ||
                scroll.type == Scroll::ScrollType::Defensive) {
                multiplier *= (1.0f + m_config.meleeDefensiveBonus);  // ×2.5 with default 1.5
            }
        }

        // Low magicka → All scrolls get multiplier (they don't cost magicka)
        if (player.vitals.IsMagickaCritical()) {
            multiplier *= 2.0f;  // ×2.0 (scrolls are very valuable when OOM)
        } else if (player.vitals.IsMagickaLow()) {
            multiplier *= 1.5f;  // ×1.5
        }

        return multiplier;
    }

    // =========================================================================
    // HELPERS
    // =========================================================================

    bool CorrelationBooster::HasUndeadTarget(const State::TargetCollection& targets) const
    {
        // Check primary target
        if (targets.primary.has_value()) {
            if (targets.primary->targetType == State::TargetType::Undead) {
                return true;
            }
        }

        // Check any tracked target
        for (const auto& [formId, target] : targets.targets) {
            if (target.targetType == State::TargetType::Undead) {
                return true;
            }
        }

        return false;
    }

}  // namespace Huginn::Scoring
