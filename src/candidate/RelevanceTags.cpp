#include "RelevanceTags.h"

namespace Huginn::Candidate
{
    RelevanceTag ComputeRelevanceTags(
        const State::WorldState& world,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const State::HealthTrackingState& healthTracking,
        const State::MagickaTrackingState& magickaTracking,
        const State::StaminaTrackingState& staminaTracking)
    {
        RelevanceTag tags = RelevanceTag::None;

        // Vital-based tags
        if (player.vitals.IsHealthLow()) {
            tags |= RelevanceTag::LowHealth;
        }
        if (player.vitals.IsHealthCritical()) {
            tags |= RelevanceTag::CriticalHealth;
        }
        if (player.vitals.IsMagickaLow()) {
            tags |= RelevanceTag::LowMagicka;
        }
        if (player.vitals.IsStaminaLow()) {
            tags |= RelevanceTag::LowStamina;
        }

        // Effect-based tags
        if (player.effects.isOnFire) {
            tags |= RelevanceTag::OnFire;
        }
        if (player.effects.isPoisoned) {
            tags |= RelevanceTag::Poisoned;
        }
        if (player.effects.isDiseased) {
            tags |= RelevanceTag::Diseased;
        }
        if (player.effects.isFrozen) {
            tags |= RelevanceTag::TakingFrost;
        }
        if (player.effects.isShocked) {
            tags |= RelevanceTag::TakingShock;
        }

        // Environment-based tags
        if (player.isUnderwater) {
            tags |= RelevanceTag::Underwater;
        }
        if (world.isLookingAtLock) {
            tags |= RelevanceTag::LookingAtLock;
        }
        if (world.isLookingAtOreVein) {
            tags |= RelevanceTag::LookingAtOre;
        }
        if (world.lightLevel < 0.3f) {
            tags |= RelevanceTag::InDarkness;
        }
        if (player.isFalling) {
            tags |= RelevanceTag::Falling;
        }

        // Workstation-based tags
        // Mapping: RE::TESFurniture::WorkBenchData::BenchType → RelevanceTag
        //   1=Forge, 2=Smithing       → AtForge    (Fortify Smithing)
        //   3=Enchanting, 4=EnchantExp → AtEnchanter (Fortify Enchanting)
        //   5=Alchemy, 6=AlchemyExp   → AtAlchemy   (Fortify Alchemy)
        //   7=Tanning, 8=Smelter, 9=Cooking → no matching fortify potion
        if (world.isLookingAtWorkstation) {
            switch (world.workstationType) {
                case 1: case 2: tags |= RelevanceTag::AtForge; break;
                case 3: case 4: tags |= RelevanceTag::AtEnchanter; break;
                case 5: case 6: tags |= RelevanceTag::AtAlchemy; break;
            }
        }

        // Combat-based tags
        if (player.isInCombat) {
            tags |= RelevanceTag::InCombat;
        }

        // Use TargetCollection query methods for enemy/ally counts
        const int enemyCount = targets.GetEnemyCount();
        if (enemyCount > 0) {
            tags |= RelevanceTag::EnemyNearby;
        }
        if (enemyCount >= 3) {
            tags |= RelevanceTag::MultipleEnemies;
        }
        if (targets.HasInjuredFollower()) {
            tags |= RelevanceTag::AllyInjured;
        }
        // Enemy casting — drives the ward / Resist Magic subtext. Same
        // TargetCollection flag ContextRuleEngine uses for wardWeight, and
        // anyCasting is in GameState's hash, so the transition reliably re-runs the
        // pipeline. (Revived here rather than left as a dead label — #10 review.)
        if (targets.cachedAnyCasting) {
            tags |= RelevanceTag::EnemyCasting;
        }

        // Equipment-based tags
        if (player.IsWeaponChargeLow()) {
            tags |= RelevanceTag::WeaponLowCharge;
        }
        if (player.IsOutOfArrows() || player.IsOutOfBolts()) {
            tags |= RelevanceTag::NeedsAmmo;
        }
        if (!player.hasMeleeEquipped && !player.hasSpellEquipped && !player.hasBowEquipped) {
            tags |= RelevanceTag::NoWeapon;
        }

        // Target-type tags - check primary target if present
        if (targets.primary.has_value()) {
            if (targets.primary->targetType == State::TargetType::Undead) {
                tags |= RelevanceTag::TargetUndead;
            }
            if (targets.primary->targetType == State::TargetType::Dragon) {
                tags |= RelevanceTag::TargetDragon;
            }
        }

        // Stealth-based tags
        if (player.isSneaking) {
            tags |= RelevanceTag::Sneaking;
        }

        // Rate-based tags (v0.12.x - sub-threshold vital tracking)
        // These fire before vitals cross percentage thresholds, enabling early-warning recommendations
        if (healthTracking.damageRate > 2.0f) {
            tags |= RelevanceTag::HealthDeclining;
        }
        if (magickaTracking.IsMagickaDraining()) {  // usage.rate > regen.rate + 5
            tags |= RelevanceTag::MagickaDraining;
        }
        if (staminaTracking.IsStaminaDraining()) {  // usage.rate > regen.rate + 5
            tags |= RelevanceTag::StaminaDraining;
        }

        return tags;
    }
}
