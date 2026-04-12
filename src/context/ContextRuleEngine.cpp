#include "ContextRuleEngine.h"
#include <algorithm>
#include <cmath>

namespace Huginn::Context
{
    // =============================================================================
    // MAIN EVALUATION METHOD
    // =============================================================================
    // Stage 1a (skeleton): Returns all zeros — no rules implemented yet.
    // Rules will be migrated in stages:
    // - Stage 1c: EvaluateVitalRules() (health, magicka, stamina)
    // - Stage 1d: EvaluateElementalRules(), EvaluateEnvironmentalRules()
    // - Stage 1e: EvaluateCombatRules(), EvaluateTargetRules(), EvaluateEquipmentRules()
    // =============================================================================

    ContextWeightMap ContextRuleEngine::EvaluateRules(
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const State::WorldState& world) const
    {
        ContextWeightMap result;

        // Stage 1a: All methods are stubs that do nothing.
        // They will be implemented in subsequent stages.

        // Vital-based rules (Stage 1c)
        EvaluateVitalRules(result, player);

        // Elemental resistance rules (Stage 1d)
        EvaluateElementalRules(result, player);

        // Environmental rules (Stage 1d)
        EvaluateEnvironmentalRules(result, player, world);

        // Combat/tactical rules (Stage 1e)
        EvaluateCombatRules(result, player, targets);

        // Target-specific rules (Stage 1e)
        EvaluateTargetRules(result, targets);

        // Equipment-based rules (Stage 1e)
        EvaluateEquipmentRules(result, player);

        return result;
    }

    // =============================================================================
    // VITAL-BASED RULES (Stage 1c implementation)
    // =============================================================================
    // Implements continuous weight functions for health/magicka/stamina restoration.
    //
    // KEY IMPROVEMENT over old system:
    // OLD: Hard thresholds created discontinuity cliffs
    //   - 51% HP → relevance = 0.5
    //   - 49% HP → relevance = 5.0 (10× jump!)
    //
    // NEW: Continuous functions eliminate cliffs
    //   - Formula: weight = (1 - vitalPct)^exponent
    //   - 51% HP → weight ≈ 0.24
    //   - 49% HP → weight ≈ 0.26 (smooth change)
    //
    // The exponent controls urgency curve shape:
    //   - 1.0 = linear (proportional to deficit)
    //   - 2.0 = quadratic (default, good balance)
    //   - 3.0 = cubic (very steep, only triggers at very low vitals)
    // =============================================================================

    void ContextRuleEngine::EvaluateVitalRules(
        ContextWeightMap& result,
        const State::PlayerActorState& player) const
    {
        // =====================================================================
        // HEALTH RESTORATION (Pure continuous, no thresholds)
        // =====================================================================
        // The curve IS the weight. No hard thresholds = no discontinuity cliffs.
        //
        // Formula: weight = (1 - vitalPct)^exponent
        //   At 100% health: deficit=0.0, weight=0.0 (no urgency)
        //   At  50% health: deficit=0.5, weight=0.25 (moderate urgency)
        //   At  25% health: deficit=0.75, weight=0.56 (high urgency)
        //   At  10% health: deficit=0.9, weight=0.81 (critical urgency)
        //
        // Exponent controls curve shape (default 2.0 = quadratic):
        //   Higher exponent = steeper at low health (more urgent)
        //   Lower exponent = gentler curve (earlier but less urgent)
        //
        // NOTE: Config weights (weightCriticalHealth, weightLowHealth) are NOT
        // used here. They'll be used in Stage 2+ as category-level multipliers
        // in the scoring formula, not as threshold overrides.
        // =====================================================================

        const float healthPct = player.vitals.health;

        if (healthPct < 1.0f) {
            const float deficit = 1.0f - healthPct;

            // Pure continuous curve - no thresholds, no cliffs
            const float curveValue = std::pow(deficit, m_config.fHealthSmoothingExponent);

            result.healingWeight = std::clamp(curveValue, 0.0f, 1.0f);
        }

        // =====================================================================
        // MAGICKA RESTORATION (Pure continuous, no thresholds)
        // =====================================================================

        const float magickaPct = player.vitals.magicka;

        if (magickaPct < 1.0f) {
            const float deficit = 1.0f - magickaPct;
            const float curveValue = std::pow(deficit, m_config.fMagickaSmoothingExponent);

            result.magickaRestoreWeight = std::clamp(curveValue, 0.0f, 1.0f);
        }

        // =====================================================================
        // STAMINA RESTORATION (Pure continuous, no thresholds)
        // =====================================================================
        // Uses slightly gentler exponent (1.5 vs 2.0) since stamina is less
        // critical than health.

        const float staminaPct = player.vitals.stamina;

        if (staminaPct < 1.0f) {
            const float deficit = 1.0f - staminaPct;
            const float curveValue = std::pow(deficit, m_config.fStaminaSmoothingExponent);

            result.staminaRestoreWeight = std::clamp(curveValue, 0.0f, 1.0f);
        }
    }

    // =============================================================================
    // ELEMENTAL RESISTANCE RULES (Stage 1d implementation)
    // =============================================================================
    // Binary weights (0 or 1) based on active elemental damage effects.
    //
    // OLD SYSTEM MAPPING (0-10 scale → [0,1] normalized):
    // - Fire/Frost/Shock: 8.0 → 0.8 (high priority)
    // - Poison: 6.0 → 0.6 (moderate priority)
    // - Disease: 3.0 → 0.3 (low priority, disease is less urgent)
    //
    // DESIGN:
    // - Binary rule: weight = configured value when effect is active, 0 otherwise
    // - No continuous scaling (unlike vitals) - you either have the effect or don't
    // - Future: could add intensity scaling if DamageEventSink tracks damage magnitude
    // =============================================================================

    void ContextRuleEngine::EvaluateElementalRules(
        ContextWeightMap& result,
        const State::PlayerActorState& player) const
    {
        // Resistance scaling: weight is reduced proportionally to existing resistance.
        // At 0% resist → full weight, 50% → half weight, 85%+ → near-zero.
        // Negative resistances (weaknesses) are clamped to not over-amplify.
        auto resistScale = [](float resistPct) {
            return std::clamp(1.0f - resistPct * 0.01f, 0.0f, 1.0f);
        };

        // Fire damage → Resist Fire relevant
        if (player.effects.isOnFire) {
            result.resistFireWeight = m_config.weightOnFire * resistScale(player.resistances.fire);
        }

        // Frost damage → Resist Frost relevant
        if (player.effects.isFrozen) {
            result.resistFrostWeight = m_config.weightFrozen * resistScale(player.resistances.frost);
        }

        // Shock damage → Resist Shock relevant
        if (player.effects.isShocked) {
            result.resistShockWeight = m_config.weightShocked * resistScale(player.resistances.shock);
        }

        // Poison → Resist Poison relevant
        if (player.effects.isPoisoned) {
            result.resistPoisonWeight = m_config.weightPoisoned * resistScale(player.resistances.poison);
        }

        // Disease → Resist Disease relevant (no disease resistance tracked)
        if (player.effects.isDiseased) {
            result.resistDiseaseWeight = m_config.weightDiseased;
        }
    }

    // =============================================================================
    // ENVIRONMENTAL RULES (Stage 1d implementation)
    // =============================================================================
    // Binary weights (0 or 1) based on environmental conditions and crosshair targets.
    //
    // OLD SYSTEM MAPPING (0-10 scale → [0,1] normalized):
    // - Underwater: 10.0 → 1.0 (critical - will drown)
    // - Looking at lock: 10.0 → 1.0 (critical - direct interaction)
    // - Falling: 8.0 → 0.8 (high priority - will take fall damage)
    // - Workstation (forge/enchanter/alchemy): 8.0 → 0.8 (high priority - boost crafting)
    //
    // DESIGN:
    // - Binary rules: weight = configured value when condition is met, 0 otherwise
    // - Workstation type mapping follows RE::TESFurniture::WorkBenchData::BenchType enum
    //   - Forge (1) / Smithing (2) → Fortify Smithing
    //   - Enchanting (3) / EnchantExp (4) → Fortify Enchanting
    //   - Alchemy (5) / AlchemyExp (6) → Fortify Alchemy
    // =============================================================================

    void ContextRuleEngine::EvaluateEnvironmentalRules(
        ContextWeightMap& result,
        const State::PlayerActorState& player,
        const State::WorldState& world) const
    {
        // =====================================================================
        // UNDERWATER → Waterbreathing (suppressed if already active)
        // =====================================================================
        if (player.isUnderwater && !player.buffs.hasWaterBreathing) {
            result.waterbreathingWeight = m_config.weightUnderwater;
        }

        // =====================================================================
        // LOOKING AT LOCK → Unlock spells
        // =====================================================================
        if (world.isLookingAtLock) {
            result.unlockWeight = m_config.weightLookingAtLock;
        }

        // =====================================================================
        // FALLING → Slow Fall / Become Ethereal
        // =====================================================================
        // StateManager sets isFalling based on velocity/height threshold
        if (player.isFalling) {
            result.slowFallWeight = m_config.weightFallingHigh;
        }

        // =====================================================================
        // WORKSTATION → Fortify Crafting
        // =====================================================================
        // RE::TESFurniture::WorkBenchData::BenchType enum values:
        //   kCreateObject(1), kSmithingWeapon(2), kSmithingArmor(7) → Fortify Smithing
        //   kEnchanting(3), kEnchantingExperiment(4)                → Fortify Enchanting
        //   kAlchemy(5), kAlchemyExperiment(6)                      → Fortify Alchemy
        if (world.isLookingAtWorkstation) {
            using BenchType = RE::TESFurniture::WorkBenchData::BenchType;
            const auto type = static_cast<BenchType>(world.workstationType);

            switch (type) {
            case BenchType::kCreateObject:
            case BenchType::kSmithingWeapon:
            case BenchType::kSmithingArmor:
                result.fortifySmithingWeight = m_config.weightAtForge;
                break;
            case BenchType::kEnchanting:
            case BenchType::kEnchantingExperiment:
                result.fortifyEnchantingWeight = m_config.weightAtEnchanter;
                break;
            case BenchType::kAlchemy:
            case BenchType::kAlchemyExperiment:
                result.fortifyAlchemyWeight = m_config.weightAtAlchemyLab;
                break;
            default:
                break;
            }
        }
    }

    // =============================================================================
    // COMBAT/TACTICAL RULES (Stage 1e implementation)
    // =============================================================================
    // Binary weights based on combat state, enemy count, and tactical situations.
    //
    // OLD SYSTEM MAPPING (0-10 scale → [0,1] normalized):
    // - In Combat: 3.0 → 0.3 (general combat damage relevance)
    // - Enemy Casting: 8.0 → 0.7 (ward spells highly relevant)
    // - Multiple Enemies (3+): 5.0 → 0.5 (AOE spell relevance)
    // - Summon (in combat, no active): 4.0 → moderate weight
    // - Sneaking: 4.0 → 0.4 (invisibility/muffle relevance)
    // =============================================================================

    void ContextRuleEngine::EvaluateCombatRules(
        ContextWeightMap& result,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets) const
    {
        // =====================================================================
        // GENERAL COMBAT DAMAGE
        // =====================================================================
        if (player.isInCombat) {
            result.damageWeight = m_config.weightInCombat;  // Already [0,1]
        }

        // =====================================================================
        // WARD SPELLS (Enemy Casting Detection)
        // =====================================================================
        if (targets.cachedAnyCasting) {
            result.wardWeight = m_config.weightEnemyCasting;  // Already [0,1]
        }

        // =====================================================================
        // AOE SPELLS (Multiple Enemies)
        // =====================================================================
        // Multiple enemies = 3+ hostiles (from old CandidateGenerator logic)
        const int enemyCount = targets.GetEnemyCount();
        if (enemyCount >= 3) {
            result.aoeWeight = m_config.weightMultipleEnemies;  // Already [0,1]
        }

        // =====================================================================
        // SUMMON SPELLS
        // =====================================================================
        // Relevant when in combat AND no active summon
        if (player.isInCombat && !player.buffs.hasActiveSummon) {
            result.summonWeight = m_config.weightSummon;
        }

        // =====================================================================
        // STEALTH (Invisibility/Muffle — suppressed if already invisible)
        // =====================================================================
        if (player.isSneaking && !player.buffs.isInvisible) {
            result.stealthWeight = m_config.weightSneaking;  // Already [0,1]
        }
    }

    // =============================================================================
    // TARGET-SPECIFIC RULES (Stage 1e implementation)
    // =============================================================================
    // Binary weights based on primary target type.
    //
    // OLD SYSTEM MAPPING (0-10 scale → [0,1] normalized):
    // - Target Undead: 6.0 → 0.6 (Turn Undead, Sun spells, anti-undead damage)
    // - Target Daedra: 6.0 → 0.6 (Anti-daedra magic vs atronachs/dremora)
    // - Target Dragon: 5.0 → 0.5 (Dragonrend, dragon-specific effects)
    //
    // DESIGN:
    // - Only checks primary target (crosshair or combat target)
    // - TargetType enum: None, Humanoid, Undead, Beast, Dragon, Construct, Daedra
    // =============================================================================

    void ContextRuleEngine::EvaluateTargetRules(
        ContextWeightMap& result,
        const State::TargetCollection& targets) const
    {
        // Check if we have a primary target
        if (!targets.primary.has_value()) {
            return;  // No target → all weights remain 0
        }

        const auto& primary = targets.primary.value();

        // =====================================================================
        // ANTI-UNDEAD (Turn Undead, Sun Damage, etc.)
        // =====================================================================
        if (primary.targetType == State::TargetType::Undead) {
            result.antiUndeadWeight = m_config.weightTargetUndead;  // Already [0,1]
        }

        // =====================================================================
        // ANTI-DAEDRA (vs Atronachs, Dremora, etc.)
        // =====================================================================
        if (primary.targetType == State::TargetType::Daedra) {
            result.antiDaedraWeight = m_config.weightTargetDaedra;  // Already [0,1]
        }

        // =====================================================================
        // ANTI-DRAGON (Dragonrend, dragon-specific)
        // =====================================================================
        if (primary.targetType == State::TargetType::Dragon) {
            result.antiDragonWeight = m_config.weightTargetDragon;  // Already [0,1]
        }
    }

    // =============================================================================
    // EQUIPMENT-BASED RULES (Stage 1e implementation)
    // =============================================================================
    // Weights based on equipment state and resource needs.
    //
    // OLD SYSTEM MAPPING (0-10 scale → [0,1] normalized):
    // - Weapon Charge: Variable based on severity
    //   - Moderate (< 25%): 5.0 → 0.5
    //   - Low (< 20%): 6.0 → 0.6
    //   - Critical (< 5%): 9.0 → 0.9
    // - Needs Ammo: 5.0 → 0.5 (bow/crossbow equipped, out of ammo)
    // - No Weapon: 4.0 → 0.4 (bound weapon spells relevant)
    //
    // DESIGN:
    // - Weapon charge uses continuous curve to avoid cliffs
    // - Ammo is binary (out or not)
    // - No weapon detection checks for missing melee/spell/bow
    // =============================================================================

    void ContextRuleEngine::EvaluateEquipmentRules(
        ContextWeightMap& result,
        const State::PlayerActorState& player) const
    {
        // =====================================================================
        // WEAPON BASELINE (Physical Weapons)
        // =====================================================================
        // Always-on weight so weapon candidates can surface on typed weapon
        // slots even out of combat. Without this, non-favorited weapons fall
        // below fMinimumUtility because damageWeight is 0.0 outside combat.
        result.weaponWeight = m_config.weightWeapon;

        // =====================================================================
        // SPELL BASELINE (All Spells)
        // =====================================================================
        // Always-on weight so spell candidates can surface on typed spell
        // slots (SpellsAny, SummonsAny, BuffsAny, etc.) even out of combat.
        // Without this, spells with no specific context (summons, buffs,
        // defensive) only get baseRelevance (0.05), which produces utility
        // below minimumUtility (0.1) in the multiplicative formula.
        result.spellWeight = m_config.weightSpell;

        // =====================================================================
        // WEAPON CHARGE (Soul Gems)
        // =====================================================================
        // Continuous scaling based on charge percentage (same pattern as vitals)
        // Formula: weight = (1 - chargePercent)^2
        //
        // Examples:
        //   75% charge: weight ≈ 0.06 (barely noticeable)
        //   50% charge: weight ≈ 0.25 (moderate urgency)
        //   25% charge: weight ≈ 0.56 (high urgency)
        //   10% charge: weight ≈ 0.81 (critical urgency)
        //
        if (player.hasEnchantedWeapon && player.weaponChargePercent < 1.0f) {
            const float chargeDeficit = 1.0f - player.weaponChargePercent;
            // Continuous curve for urgency (same pattern as health/magicka)
            const float chargeWeight = std::pow(chargeDeficit, m_config.fWeaponChargeSmoothingExponent);
            result.weaponChargeWeight = std::clamp(chargeWeight, 0.0f, 1.0f);
        }

        // =====================================================================
        // AMMO (Arrows/Bolts)
        // =====================================================================
        // Binary: Either out of ammo or not
        if (player.IsOutOfArrows() || player.IsOutOfBolts()) {
            result.ammoWeight = m_config.weightNeedsAmmo;  // Already [0,1]
        }

        // =====================================================================
        // BOUND WEAPON (No Weapon Equipped)
        // =====================================================================
        // Relevant when player has no melee weapon, bow, or offensive spell
        const bool noWeapon = !player.hasMeleeEquipped &&
                              !player.hasBowEquipped &&
                              !player.hasSpellEquipped;

        if (noWeapon) {
            result.boundWeaponWeight = m_config.weightNoWeapon;  // Already [0,1]
        }
    }

}  // namespace Huginn::Context
