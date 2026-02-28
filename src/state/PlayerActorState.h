#pragma once

#include "StateConstants.h"
#include "StateTypes.h"              // For EffectType
#include <cmath>
#include <cstdint>
#include <string_view>

namespace Huginn::State
{
  // =============================================================================
  // SHARED ACTOR COMPONENTS (v0.6.1)
  // =============================================================================
  // These structs are shared between PlayerActorState and TargetActorState
  // to eliminate duplication and enable consistent vitals/effects/buffs handling.
  //
  // DESIGN RATIONALE:
  // - Fixes duplication identified in state-refactoring.md (player vs target vitals)
  // - Enables future target buff/debuff detection (Phase 7)
  // - All structs trivially copyable for thread safety
  // =============================================================================

  // =============================================================================
  // ACTOR VITALS (shared component)
  // =============================================================================
  // Health, magicka, stamina percentages and max values.
  // Used by both PlayerActorState and TargetActorState.
  // =============================================================================

  struct ActorVitals
  {
    float health = DefaultState::FULL_VITAL;      // 0.0-1.0 percentage
    float magicka = DefaultState::FULL_VITAL;     // 0.0-1.0 percentage
    float stamina = DefaultState::FULL_VITAL;     // 0.0-1.0 percentage
    float maxHealth = DefaultState::DEFAULT_MAX_VITAL;    // Effective max value (after debuffs)
    float maxMagicka = DefaultState::DEFAULT_MAX_VITAL;
    float maxStamina = DefaultState::DEFAULT_MAX_VITAL;
    float baseMaxHealth = DefaultState::DEFAULT_MAX_VITAL;  // Base max value (before debuffs)
    float baseMaxMagicka = DefaultState::DEFAULT_MAX_VITAL;
    float baseMaxStamina = DefaultState::DEFAULT_MAX_VITAL;

    // Helper methods
    [[nodiscard]] bool IsHealthLow() const noexcept {
      return health < VitalThreshold::LOW;
    }

    [[nodiscard]] bool IsHealthCritical() const noexcept {
      return health < VitalThreshold::CRITICAL;
    }

    [[nodiscard]] bool IsMagickaLow() const noexcept {
      return magicka < VitalThreshold::LOW;
    }

    [[nodiscard]] bool IsMagickaCritical() const noexcept {
      return magicka < VitalThreshold::CRITICAL;
    }

    [[nodiscard]] bool IsStaminaLow() const noexcept {
      return stamina < VitalThreshold::LOW;
    }

    [[nodiscard]] bool IsStaminaCritical() const noexcept {
      return stamina < VitalThreshold::CRITICAL;
    }

#ifdef _DEBUG
    void LogDifferences(const ActorVitals& other, std::string_view prefix = "") const {
      if (std::abs(health - other.health) >= Epsilon::VITAL_PERCENTAGE) {
        logger::trace("  {}.health changed: {:.2f} -> {:.2f}", prefix, other.health, health);
      }
      if (std::abs(magicka - other.magicka) >= Epsilon::VITAL_PERCENTAGE) {
        logger::trace("  {}.magicka changed: {:.2f} -> {:.2f}", prefix, other.magicka, magicka);
      }
      if (std::abs(stamina - other.stamina) >= Epsilon::VITAL_PERCENTAGE) {
        logger::trace("  {}.stamina changed: {:.2f} -> {:.2f}", prefix, other.stamina, stamina);
      }
      if (std::abs(maxHealth - other.maxHealth) >= Epsilon::VITAL_MAX_VALUE) {
        logger::trace("  {}.maxHealth changed: {:.1f} -> {:.1f}", prefix, other.maxHealth, maxHealth);
      }
      if (std::abs(maxMagicka - other.maxMagicka) >= Epsilon::VITAL_MAX_VALUE) {
        logger::trace("  {}.maxMagicka changed: {:.1f} -> {:.1f}", prefix, other.maxMagicka, maxMagicka);
      }
      if (std::abs(maxStamina - other.maxStamina) >= Epsilon::VITAL_MAX_VALUE) {
        logger::trace("  {}.maxStamina changed: {:.1f} -> {:.1f}", prefix, other.maxStamina, maxStamina);
      }
      if (std::abs(baseMaxHealth - other.baseMaxHealth) >= Epsilon::VITAL_MAX_VALUE) {
        logger::trace("  {}.baseMaxHealth changed: {:.1f} -> {:.1f}", prefix, other.baseMaxHealth, baseMaxHealth);
      }
      if (std::abs(baseMaxMagicka - other.baseMaxMagicka) >= Epsilon::VITAL_MAX_VALUE) {
        logger::trace("  {}.baseMaxMagicka changed: {:.1f} -> {:.1f}", prefix, other.baseMaxMagicka, baseMaxMagicka);
      }
      if (std::abs(baseMaxStamina - other.baseMaxStamina) >= Epsilon::VITAL_MAX_VALUE) {
        logger::trace("  {}.baseMaxStamina changed: {:.1f} -> {:.1f}", prefix, other.baseMaxStamina, baseMaxStamina);
      }
    }
#endif

    bool operator==(const ActorVitals& other) const {
      bool equal = std::abs(health - other.health) < Epsilon::VITAL_PERCENTAGE &&
                   std::abs(magicka - other.magicka) < Epsilon::VITAL_PERCENTAGE &&
                   std::abs(stamina - other.stamina) < Epsilon::VITAL_PERCENTAGE &&
                   std::abs(maxHealth - other.maxHealth) < Epsilon::VITAL_MAX_VALUE &&
                   std::abs(maxMagicka - other.maxMagicka) < Epsilon::VITAL_MAX_VALUE &&
                   std::abs(maxStamina - other.maxStamina) < Epsilon::VITAL_MAX_VALUE &&
                   std::abs(baseMaxHealth - other.baseMaxHealth) < Epsilon::VITAL_MAX_VALUE &&
                   std::abs(baseMaxMagicka - other.baseMaxMagicka) < Epsilon::VITAL_MAX_VALUE &&
                   std::abs(baseMaxStamina - other.baseMaxStamina) < Epsilon::VITAL_MAX_VALUE;

#ifdef _DEBUG
      if (!equal) {
        logger::trace("ActorVitals changed:");
        LogDifferences(other, "ActorVitals");
      }
#endif
      return equal;
    }
  };

  // =============================================================================
  // ACTOR EFFECTS (shared component)
  // =============================================================================
  // Damage-over-time effects (fire, poison, frost, shock, disease, drains).
  // Used by both PlayerActorState (v0.6.1) and TargetActorState (Phase 7).
  // =============================================================================

  struct ActorEffects
  {
    bool isOnFire = false;
    bool isPoisoned = false;
    bool isFrozen = false;        // Taking frost damage
    bool isShocked = false;       // Taking shock damage
    bool isDiseased = false;
    bool hasMagickaPoison = false;  // Magicka damage over time
    bool hasStaminaPoison = false;  // Stamina damage over time
    bool hasHealthDrain = false;    // Health damage over time (bleeding)

    [[nodiscard]] bool HasAnyDamageEffect() const noexcept {
      return isOnFire || isPoisoned || isFrozen || isShocked || hasMagickaPoison || hasStaminaPoison || hasHealthDrain;
    }

#ifdef _DEBUG
    void LogDifferences(const ActorEffects& other, std::string_view prefix = "") const {
      if (isOnFire != other.isOnFire) {
        logger::trace("  {}.isOnFire changed: {} -> {}", prefix, other.isOnFire, isOnFire);
      }
      if (isPoisoned != other.isPoisoned) {
        logger::trace("  {}.isPoisoned changed: {} -> {}", prefix, other.isPoisoned, isPoisoned);
      }
      if (isFrozen != other.isFrozen) {
        logger::trace("  {}.isFrozen changed: {} -> {}", prefix, other.isFrozen, isFrozen);
      }
      if (isShocked != other.isShocked) {
        logger::trace("  {}.isShocked changed: {} -> {}", prefix, other.isShocked, isShocked);
      }
      if (isDiseased != other.isDiseased) {
        logger::trace("  {}.isDiseased changed: {} -> {}", prefix, other.isDiseased, isDiseased);
      }
      if (hasMagickaPoison != other.hasMagickaPoison) {
        logger::trace("  {}.hasMagickaPoison changed: {} -> {}", prefix, other.hasMagickaPoison, hasMagickaPoison);
      }
      if (hasStaminaPoison != other.hasStaminaPoison) {
        logger::trace("  {}.hasStaminaPoison changed: {} -> {}", prefix, other.hasStaminaPoison, hasStaminaPoison);
      }
      if (hasHealthDrain != other.hasHealthDrain) {
        logger::trace("  {}.hasHealthDrain changed: {} -> {}", prefix, other.hasHealthDrain, hasHealthDrain);
      }
    }
#endif

    bool operator==(const ActorEffects& other) const {
      bool equal = isOnFire == other.isOnFire &&
                   isPoisoned == other.isPoisoned &&
                   isFrozen == other.isFrozen &&
                   isShocked == other.isShocked &&
                   isDiseased == other.isDiseased &&
                   hasMagickaPoison == other.hasMagickaPoison &&
                   hasStaminaPoison == other.hasStaminaPoison &&
                   hasHealthDrain == other.hasHealthDrain;

#ifdef _DEBUG
      if (!equal) {
        logger::trace("ActorEffects changed:");
        LogDifferences(other, "ActorEffects");
      }
#endif
      return equal;
    }
  };

  // =============================================================================
  // ACTOR BUFFS (shared component)
  // =============================================================================
  // Protective buffs (armor spells, cloaks, wards, resistances, summons).
  // Used by both PlayerActorState (v0.6.1) and TargetActorState (Phase 7).
  // =============================================================================

  struct ActorBuffs
  {
    bool hasWaterBreathing = false;
    bool isInvisible = false;
    bool hasMuffle = false;
    bool hasArmorBuff = false;          // Any armor spell active (Oakflesh, Stoneflesh, etc.)
    bool hasCloakActive = false;        // Flame Cloak, Frost Cloak, Lightning Cloak
    bool hasActiveSummon = false;       // Any conjured creature

    EffectType activeCloakType = EffectType::None;  // Fire/Frost/Shock

    // Regen buffs/debuffs (v0.6.6)
    bool hasHealthRegenBuff = false;     // Fortify Health Regen
    bool hasHealthRegenDebuff = false;   // Damage Health Regen (disease, curse)
    bool hasMagickaRegenBuff = false;    // Fortify Magicka Regen
    bool hasMagickaRegenDebuff = false;  // Damage Magicka Regen
    bool hasStaminaRegenBuff = false;    // Fortify Stamina Regen
    bool hasStaminaRegenDebuff = false;  // Damage Stamina Regen

    // Fortify Magic School buffs (v0.8.x) - for spell synergy
    // When active, spells of matching school get correlation bonus
    bool hasFortifyDestruction = false;
    bool hasFortifyConjuration = false;
    bool hasFortifyRestoration = false;
    bool hasFortifyAlteration = false;
    bool hasFortifyIllusion = false;
    bool hasFortifyEnchanting = false;   // Not a school, but same pattern

    [[nodiscard]] bool HasAnyBuff() const noexcept {
      return hasWaterBreathing || isInvisible || hasMuffle ||
             hasArmorBuff || hasCloakActive || hasActiveSummon ||
             hasHealthRegenBuff || hasMagickaRegenBuff || hasStaminaRegenBuff ||
             hasFortifyDestruction || hasFortifyConjuration || hasFortifyRestoration ||
             hasFortifyAlteration || hasFortifyIllusion || hasFortifyEnchanting;
    }

    [[nodiscard]] bool HasAnyFortifySchool() const noexcept {
      return hasFortifyDestruction || hasFortifyConjuration || hasFortifyRestoration ||
             hasFortifyAlteration || hasFortifyIllusion;
    }

    [[nodiscard]] bool HasAnyRegenDebuff() const noexcept {
      return hasHealthRegenDebuff || hasMagickaRegenDebuff || hasStaminaRegenDebuff;
    }

#ifdef _DEBUG
    void LogDifferences(const ActorBuffs& other, std::string_view prefix = "") const {
      if (hasWaterBreathing != other.hasWaterBreathing) {
        logger::trace("  {}.hasWaterBreathing changed: {} -> {}", prefix, other.hasWaterBreathing, hasWaterBreathing);
      }
      if (isInvisible != other.isInvisible) {
        logger::trace("  {}.isInvisible changed: {} -> {}", prefix, other.isInvisible, isInvisible);
      }
      if (hasMuffle != other.hasMuffle) {
        logger::trace("  {}.hasMuffle changed: {} -> {}", prefix, other.hasMuffle, hasMuffle);
      }
      if (hasArmorBuff != other.hasArmorBuff) {
        logger::trace("  {}.hasArmorBuff changed: {} -> {}", prefix, other.hasArmorBuff, hasArmorBuff);
      }
      if (hasCloakActive != other.hasCloakActive) {
        logger::trace("  {}.hasCloakActive changed: {} -> {}", prefix, other.hasCloakActive, hasCloakActive);
      }
      if (activeCloakType != other.activeCloakType) {
        logger::trace("  {}.activeCloakType changed: {} -> {}", prefix,
                      static_cast<int>(other.activeCloakType), static_cast<int>(activeCloakType));
      }
      if (hasActiveSummon != other.hasActiveSummon) {
        logger::trace("  {}.hasActiveSummon changed: {} -> {}", prefix, other.hasActiveSummon, hasActiveSummon);
      }
      // Regen buffs/debuffs (v0.6.6)
      if (hasHealthRegenBuff != other.hasHealthRegenBuff) {
        logger::trace("  {}.hasHealthRegenBuff changed: {} -> {}", prefix, other.hasHealthRegenBuff, hasHealthRegenBuff);
      }
      if (hasHealthRegenDebuff != other.hasHealthRegenDebuff) {
        logger::trace("  {}.hasHealthRegenDebuff changed: {} -> {}", prefix, other.hasHealthRegenDebuff, hasHealthRegenDebuff);
      }
      if (hasMagickaRegenBuff != other.hasMagickaRegenBuff) {
        logger::trace("  {}.hasMagickaRegenBuff changed: {} -> {}", prefix, other.hasMagickaRegenBuff, hasMagickaRegenBuff);
      }
      if (hasMagickaRegenDebuff != other.hasMagickaRegenDebuff) {
        logger::trace("  {}.hasMagickaRegenDebuff changed: {} -> {}", prefix, other.hasMagickaRegenDebuff, hasMagickaRegenDebuff);
      }
      if (hasStaminaRegenBuff != other.hasStaminaRegenBuff) {
        logger::trace("  {}.hasStaminaRegenBuff changed: {} -> {}", prefix, other.hasStaminaRegenBuff, hasStaminaRegenBuff);
      }
      if (hasStaminaRegenDebuff != other.hasStaminaRegenDebuff) {
        logger::trace("  {}.hasStaminaRegenDebuff changed: {} -> {}", prefix, other.hasStaminaRegenDebuff, hasStaminaRegenDebuff);
      }
      // Fortify Magic School buffs (v0.8.x)
      if (hasFortifyDestruction != other.hasFortifyDestruction) {
        logger::trace("  {}.hasFortifyDestruction changed: {} -> {}", prefix, other.hasFortifyDestruction, hasFortifyDestruction);
      }
      if (hasFortifyConjuration != other.hasFortifyConjuration) {
        logger::trace("  {}.hasFortifyConjuration changed: {} -> {}", prefix, other.hasFortifyConjuration, hasFortifyConjuration);
      }
      if (hasFortifyRestoration != other.hasFortifyRestoration) {
        logger::trace("  {}.hasFortifyRestoration changed: {} -> {}", prefix, other.hasFortifyRestoration, hasFortifyRestoration);
      }
      if (hasFortifyAlteration != other.hasFortifyAlteration) {
        logger::trace("  {}.hasFortifyAlteration changed: {} -> {}", prefix, other.hasFortifyAlteration, hasFortifyAlteration);
      }
      if (hasFortifyIllusion != other.hasFortifyIllusion) {
        logger::trace("  {}.hasFortifyIllusion changed: {} -> {}", prefix, other.hasFortifyIllusion, hasFortifyIllusion);
      }
      if (hasFortifyEnchanting != other.hasFortifyEnchanting) {
        logger::trace("  {}.hasFortifyEnchanting changed: {} -> {}", prefix, other.hasFortifyEnchanting, hasFortifyEnchanting);
      }
    }
#endif

    bool operator==(const ActorBuffs& other) const {
      bool equal = hasWaterBreathing == other.hasWaterBreathing &&
                   isInvisible == other.isInvisible &&
                   hasMuffle == other.hasMuffle &&
                   hasArmorBuff == other.hasArmorBuff &&
                   hasCloakActive == other.hasCloakActive &&
                   activeCloakType == other.activeCloakType &&
                   hasActiveSummon == other.hasActiveSummon &&
                   // Regen buffs/debuffs (v0.6.6)
                   hasHealthRegenBuff == other.hasHealthRegenBuff &&
                   hasHealthRegenDebuff == other.hasHealthRegenDebuff &&
                   hasMagickaRegenBuff == other.hasMagickaRegenBuff &&
                   hasMagickaRegenDebuff == other.hasMagickaRegenDebuff &&
                   hasStaminaRegenBuff == other.hasStaminaRegenBuff &&
                   hasStaminaRegenDebuff == other.hasStaminaRegenDebuff &&
                   // Fortify Magic School buffs (v0.8.x)
                   hasFortifyDestruction == other.hasFortifyDestruction &&
                   hasFortifyConjuration == other.hasFortifyConjuration &&
                   hasFortifyRestoration == other.hasFortifyRestoration &&
                   hasFortifyAlteration == other.hasFortifyAlteration &&
                   hasFortifyIllusion == other.hasFortifyIllusion &&
                   hasFortifyEnchanting == other.hasFortifyEnchanting;

#ifdef _DEBUG
      if (!equal) {
        logger::trace("ActorBuffs changed:");
        LogDifferences(other, "ActorBuffs");
      }
#endif
      return equal;
    }
  };

  // =============================================================================
  // ACTOR RESISTANCES (v0.6.6)
  // =============================================================================
  // Elemental and magic resistances for deprioritizing redundant resist buffs.
  // Used by PlayerActorState.
  // =============================================================================

  struct ActorResistances
  {
    float fire = 0.0f;      // % resistance (0-100+, can be negative)
    float frost = 0.0f;
    float shock = 0.0f;
    float poison = 0.0f;
    float magic = 0.0f;

    // High resistance checks (for deprioritizing resist spells/potions)
    [[nodiscard]] bool HasHighFireResist() const noexcept {
      return fire >= ResistanceThreshold::HIGH_RESIST;
    }

    [[nodiscard]] bool HasHighFrostResist() const noexcept {
      return frost >= ResistanceThreshold::HIGH_RESIST;
    }

    [[nodiscard]] bool HasHighShockResist() const noexcept {
      return shock >= ResistanceThreshold::HIGH_RESIST;
    }

    [[nodiscard]] bool HasHighPoisonResist() const noexcept {
      return poison >= ResistanceThreshold::HIGH_RESIST;
    }

    [[nodiscard]] bool HasHighMagicResist() const noexcept {
      return magic >= ResistanceThreshold::HIGH_RESIST;
    }

    // Capped resistance checks (resist spells nearly useless)
    [[nodiscard]] bool IsFireResistCapped() const noexcept {
      return fire >= ResistanceThreshold::CAPPED_RESIST;
    }

    [[nodiscard]] bool IsFrostResistCapped() const noexcept {
      return frost >= ResistanceThreshold::CAPPED_RESIST;
    }

    [[nodiscard]] bool IsShockResistCapped() const noexcept {
      return shock >= ResistanceThreshold::CAPPED_RESIST;
    }

    bool operator==(const ActorResistances& other) const {
      return std::abs(fire - other.fire) < ResistanceThreshold::EPSILON &&
             std::abs(frost - other.frost) < ResistanceThreshold::EPSILON &&
             std::abs(shock - other.shock) < ResistanceThreshold::EPSILON &&
             std::abs(poison - other.poison) < ResistanceThreshold::EPSILON &&
             std::abs(magic - other.magic) < ResistanceThreshold::EPSILON;
    }
  };

  // =============================================================================
  // PLAYER ACTOR STATE (v0.6.1)
  // =============================================================================
  // Complete player state using shared components + player-specific fields.
  // Thread-safe via copy-out pattern.
  // =============================================================================

  struct PlayerActorState
  {
    // Shared actor components
    ActorVitals vitals;
    ActorEffects effects;
    ActorBuffs buffs;

    // Equipment state (player-specific)
    float weaponChargePercent = DefaultState::FULL_CHARGE;  // 0.0-1.0
    bool hasEnchantedWeapon = false;
    std::int32_t arrowCount = DefaultState::NO_ARROWS;
    bool hasBowEquipped = false;
    RE::FormID rightHandWeapon = 0;
    RE::FormID leftHandWeapon = 0;
    RE::FormID rightHandSpell = 0;  // Spell FormID in right hand (0 = none)
    RE::FormID leftHandSpell = 0;   // Spell FormID in left hand (0 = none)
    RE::FormID equippedShield = 0;
    bool hasMeleeEquipped = false;
    bool hasStaffEquipped = false;
    bool hasShieldEquipped = false;
    bool hasOneHandedEquipped = false;
    bool hasTwoHandedEquipped = false;
    bool hasSpellEquipped = false;
    bool hasCrossbowEquipped = false;
    bool hasTorchEquipped = false;
    float weaponChargeMax = 0.0f;
    std::int32_t boltCount = DefaultState::NO_ARROWS;
    std::string equippedAmmoName;  // Name of currently equipped arrow/bolt (for display)
    float equippedAmmoDamage = 0.0f;  // Base damage of equipped ammo (for bow total damage calc)

    // Survival state (player-specific)
    int hungerLevel = DefaultState::NEUTRAL_SURVIVAL;
    int coldLevel = DefaultState::NEUTRAL_SURVIVAL;
    int fatigueLevel = DefaultState::NEUTRAL_SURVIVAL;
    bool survivalModeActive = false;
    float warmthRating = 0.0f;  // v0.6.6: Current warmth from gear/buffs (CC Survival Mode)

    // Resistances (v0.6.6)
    ActorResistances resistances;

    // Transformation state (v0.6.6)
    int vampireStage = VampireThreshold::NOT_VAMPIRE;  // 0=Not vampire, 1-4=Vampire stage
    bool isWerewolf = false;     // Has beast blood (can transform)
    bool isInBeastForm = false;  // Currently transformed

    // Position/state flags (player-specific)
    bool isUnderwater = false;
    bool isSwimming = false;
    bool isFalling = false;
    float heightAboveGround = DefaultState::ON_GROUND;
    bool isOverencumbered = false;
    bool isSneaking = false;
    bool isInCombat = false;
    bool isMounted = false;
    bool isMountedOnDragon = false;

    // Equipment helpers
    [[nodiscard]] bool IsWeaponChargeLow() const noexcept {
      return hasEnchantedWeapon && weaponChargePercent < 0.25f;  // Config::WEAPON_CHARGE_LOW_THRESHOLD
    }

    [[nodiscard]] bool IsWeaponChargeCritical() const noexcept {
      return hasEnchantedWeapon && weaponChargePercent < 0.10f;  // Config::WEAPON_CHARGE_CRITICAL_THRESHOLD
    }

    [[nodiscard]] bool IsOutOfArrows() const noexcept {
      return hasBowEquipped && arrowCount == DefaultState::NO_ARROWS;
    }

    [[nodiscard]] bool IsOutOfBolts() const noexcept {
      return hasCrossbowEquipped && boltCount == DefaultState::NO_ARROWS;
    }

    [[nodiscard]] bool IsSpellEquipped(RE::FormID spellFormID) const noexcept {
      return spellFormID != 0 && (rightHandSpell == spellFormID || leftHandSpell == spellFormID);
    }

    [[nodiscard]] bool IsWeaponEquipped(RE::FormID weaponFormID) const noexcept {
      return weaponFormID != 0 && (rightHandWeapon == weaponFormID || leftHandWeapon == weaponFormID);
    }

    [[nodiscard]] bool IsItemEquipped(RE::FormID formID) const noexcept {
      return IsSpellEquipped(formID) || IsWeaponEquipped(formID) || equippedShield == formID;
    }

    // Survival helpers
    [[nodiscard]] bool IsStarving() const noexcept {
      return survivalModeActive && hungerLevel >= SurvivalThreshold::HUNGER_FAMISHED;
    }

    [[nodiscard]] bool IsFreezing() const noexcept {
      return survivalModeActive && coldLevel >= SurvivalThreshold::COLD_FREEZING;
    }

    [[nodiscard]] bool IsExhausted() const noexcept {
      return survivalModeActive && fatigueLevel >= SurvivalThreshold::FATIGUE_WEARY;
    }

    [[nodiscard]] bool HasRestBonus() const noexcept {
      return survivalModeActive && fatigueLevel < DefaultState::NEUTRAL_SURVIVAL;
    }

    // Warmth helpers (v0.6.6)
    [[nodiscard]] bool IsWarmthLow() const noexcept {
      return survivalModeActive && warmthRating < WarmthThreshold::LOW;
    }

    [[nodiscard]] bool HasGoodWarmth() const noexcept {
      return warmthRating >= WarmthThreshold::HIGH;
    }

    [[nodiscard]] float GetWarmthContextWeight() const noexcept {
      if (!survivalModeActive) return 0.0f;
      // Higher weight when warmth is low (need warming items/spells)
      if (warmthRating < WarmthThreshold::LOW) return 0.30f;
      if (warmthRating < WarmthThreshold::MODERATE) return 0.15f;
      return 0.0f;
    }

    // Vampire/Werewolf helpers (v0.6.6)
    [[nodiscard]] bool IsVampire() const noexcept {
      return vampireStage > VampireThreshold::NOT_VAMPIRE;
    }

    [[nodiscard]] bool IsSunVulnerable() const noexcept {
      return vampireStage >= VampireThreshold::SUN_VULNERABLE_STAGE;
    }

    [[nodiscard]] bool CanTransformToWerewolf() const noexcept {
      return isWerewolf && !isInBeastForm;
    }

    // Survival context weight helpers (for recommendation engine)
    [[nodiscard]] float GetHungerContextWeight() const noexcept {
      if (!survivalModeActive) return 0.0f;
      if (hungerLevel >= SurvivalThreshold::HUNGER_FAMISHED) return 0.30f;
      if (hungerLevel >= SurvivalThreshold::HUNGER_HUNGRY) return 0.15f;
      return 0.0f;
    }

    [[nodiscard]] float GetColdContextWeight() const noexcept {
      if (!survivalModeActive) return 0.0f;
      if (coldLevel >= SurvivalThreshold::COLD_FREEZING) return 0.30f;
      if (coldLevel >= SurvivalThreshold::COLD_VERY_COLD) return 0.15f;
      return 0.0f;
    }

    [[nodiscard]] float GetFatigueContextWeight() const noexcept {
      if (!survivalModeActive) return 0.0f;
      if (fatigueLevel >= SurvivalThreshold::FATIGUE_WEARY) return 0.30f;
      if (fatigueLevel >= SurvivalThreshold::FATIGUE_TIRED) return 0.15f;
      return 0.0f;
    }

    // Equality comparison (epsilon-tolerant)
    bool operator==(const PlayerActorState& other) const {
      return vitals == other.vitals &&
             effects == other.effects &&
             buffs == other.buffs &&
             // Equipment
             std::abs(weaponChargePercent - other.weaponChargePercent) < Epsilon::WEAPON_CHARGE &&
             hasEnchantedWeapon == other.hasEnchantedWeapon &&
             arrowCount == other.arrowCount &&
             hasBowEquipped == other.hasBowEquipped &&
             rightHandWeapon == other.rightHandWeapon &&
             leftHandWeapon == other.leftHandWeapon &&
             rightHandSpell == other.rightHandSpell &&
             leftHandSpell == other.leftHandSpell &&
             equippedShield == other.equippedShield &&
             hasMeleeEquipped == other.hasMeleeEquipped &&
             hasStaffEquipped == other.hasStaffEquipped &&
             hasShieldEquipped == other.hasShieldEquipped &&
             hasOneHandedEquipped == other.hasOneHandedEquipped &&
             hasTwoHandedEquipped == other.hasTwoHandedEquipped &&
             hasSpellEquipped == other.hasSpellEquipped &&
             hasCrossbowEquipped == other.hasCrossbowEquipped &&
             hasTorchEquipped == other.hasTorchEquipped &&
             std::abs(weaponChargeMax - other.weaponChargeMax) < Epsilon::WEAPON_CHARGE &&
             boltCount == other.boltCount &&
             equippedAmmoName == other.equippedAmmoName &&
             std::abs(equippedAmmoDamage - other.equippedAmmoDamage) < 0.1f &&
             // Survival
             hungerLevel == other.hungerLevel &&
             coldLevel == other.coldLevel &&
             fatigueLevel == other.fatigueLevel &&
             survivalModeActive == other.survivalModeActive &&
             std::abs(warmthRating - other.warmthRating) < 1.0f &&  // v0.6.6
             // Resistances (v0.6.6)
             resistances == other.resistances &&
             // Transformation (v0.6.6)
             vampireStage == other.vampireStage &&
             isWerewolf == other.isWerewolf &&
             isInBeastForm == other.isInBeastForm &&
             // Position
             isUnderwater == other.isUnderwater &&
             isSwimming == other.isSwimming &&
             isFalling == other.isFalling &&
             isOverencumbered == other.isOverencumbered &&
             isSneaking == other.isSneaking &&
             isInCombat == other.isInCombat &&
             isMounted == other.isMounted &&
             isMountedOnDragon == other.isMountedOnDragon;
      // Note: heightAboveGround excluded (continuous value, isFalling captures important state)
    }
  };
}
