#pragma once

#include <SimpleIni.h>
#include <filesystem>

namespace Huginn::State { struct ContextWeightConfig; }

namespace Huginn::State
{
    // =========================================================================
    // DEFAULT VALUES (compile-time constants)
    // =========================================================================
    // These match the former WEIGHT_* constants from Config.h exactly.
    // Used as fallbacks when INI keys are missing.

    namespace ContextWeightDefaults
    {
        // Elemental / status effects (OLD SCALE: 0-10, kept for backwards compatibility)
        inline constexpr float ON_FIRE = 8.0f;
        inline constexpr float POISONED = 6.0f;
        inline constexpr float FROZEN = 8.0f;
        inline constexpr float SHOCKED = 8.0f;
        inline constexpr float DISEASED = 3.0f;

        // Environmental (OLD SCALE: 0-10)
        inline constexpr float UNDERWATER = 10.0f;
        inline constexpr float FALLING_HIGH = 8.0f;
        inline constexpr float LOOKING_AT_LOCK = 10.0f;

        // Weapon charge (OLD SCALE: 0-10)
        inline constexpr float WEAPON_CHARGE_MODERATE = 5.0f;   // 25% threshold
        inline constexpr float WEAPON_CHARGE_LOW = 6.0f;        // 20% threshold
        inline constexpr float WEAPON_CHARGE_CRITICAL = 9.0f;   // 5% threshold

        // Active buff suppression (negative = suppress redundant suggestions)
        inline constexpr float HAS_WATERBREATHING = -10.0f;
        inline constexpr float HAS_INVISIBILITY = -10.0f;
        inline constexpr float HAS_MUFFLE = -8.0f;
        inline constexpr float HAS_ARMOR_BUFF = -5.0f;
        inline constexpr float HAS_CLOAK = -8.0f;
        inline constexpr float HAS_SUMMON = -6.0f;

        // =====================================================================
        // NEW: NORMALIZED WEIGHTS [0,1] for ContextRuleEngine
        // =====================================================================
        // These replace the old 0-10 scale with normalized [0,1] weights.
        // Mapping: 10.0 → 1.0 (critical), 5.0 → 0.5 (high), 3.0 → 0.3 (moderate)
        // =====================================================================

        // Health/resource restoration
        inline constexpr float CRITICAL_HEALTH = 1.0f;      // < 20% HP
        inline constexpr float LOW_HEALTH = 0.5f;           // < 50% HP
        inline constexpr float LOW_MAGICKA = 0.4f;          // < 30% magicka
        inline constexpr float LOW_STAMINA = 0.3f;          // < 30% stamina

        // Combat/tactical
        inline constexpr float IN_COMBAT = 0.3f;            // General combat damage
        inline constexpr float MULTIPLE_ENEMIES = 0.5f;     // 3+ enemies (AOE viable)
        inline constexpr float ENEMY_CASTING = 0.7f;        // Enemy casting (ward)
        inline constexpr float SNEAKING = 0.4f;             // Sneaking (invisibility/muffle)

        // Workstations (normalized from 8.0 → 0.8)
        inline constexpr float AT_FORGE = 0.8f;
        inline constexpr float AT_ENCHANTER = 0.8f;
        inline constexpr float AT_ALCHEMY_LAB = 0.8f;

        // Target-specific
        inline constexpr float TARGET_UNDEAD = 0.6f;        // Anti-undead spells
        inline constexpr float TARGET_DAEDRA = 0.6f;        // Anti-daedra spells
        inline constexpr float TARGET_DRAGON = 0.5f;        // Dragonrend, dragon-specific

        // Equipment
        inline constexpr float NEEDS_AMMO = 0.5f;           // Bow equipped, low ammo
        inline constexpr float NO_WEAPON = 0.4f;            // No weapon equipped (bound weapon)
        inline constexpr float WEAPON = 0.2f;               // Physical weapons (always-on baseline)
        inline constexpr float SPELL = 0.2f;                // Spells (always-on baseline for typed spell slots)

        // Utility baseline
        inline constexpr float BASE_RELEVANCE = 0.05f;      // Noise floor for always-available items

        // =====================================================================
        // CONTINUOUS FUNCTION SMOOTHING PARAMETERS
        // =====================================================================
        // Controls the shape of continuous weight curves to eliminate
        // discontinuity cliffs (e.g., 51% HP → 0.5, 49% HP → 5.0 jump)
        // =====================================================================

        inline constexpr float HEALTH_SMOOTHING_EXPONENT = 2.0f;    // Quadratic falloff
        inline constexpr float MAGICKA_SMOOTHING_EXPONENT = 2.0f;   // Quadratic falloff
        inline constexpr float STAMINA_SMOOTHING_EXPONENT = 1.5f;   // Slightly less steep
    }

    // =========================================================================
    // CONTEXT WEIGHT SETTINGS
    // =========================================================================
    // Singleton that loads context weight multipliers from
    // Data/SKSE/Plugins/Huginn.ini [ContextWeights] section.
    //
    // These weights control how strongly each environmental/status condition
    // influences the recommendation system's context scoring.
    // =========================================================================

    class ContextWeightSettings
    {
    public:
        static ContextWeightSettings& GetSingleton()
        {
            static ContextWeightSettings instance;
            return instance;
        }

        void LoadFromFile(const std::filesystem::path& iniPath);
        void ResetToDefaults();

        /// Produce an immutable snapshot of all context weight fields.
        [[nodiscard]] ContextWeightConfig BuildConfig() const;

        // =====================================================================
        // LEGACY WEIGHTS (OLD SCALE: 0-10)
        // =====================================================================
        // These are kept for backwards compatibility with existing code.
        // Will be phased out as ContextRuleEngine replaces CandidateGenerator.
        // =====================================================================

        // --- Elemental / status effects ---
        float weightOnFire = ContextWeightDefaults::ON_FIRE;
        float weightPoisoned = ContextWeightDefaults::POISONED;
        float weightFrozen = ContextWeightDefaults::FROZEN;
        float weightShocked = ContextWeightDefaults::SHOCKED;
        float weightDiseased = ContextWeightDefaults::DISEASED;

        // --- Environmental ---
        float weightUnderwater = ContextWeightDefaults::UNDERWATER;
        float weightFallingHigh = ContextWeightDefaults::FALLING_HIGH;
        float weightLookingAtLock = ContextWeightDefaults::LOOKING_AT_LOCK;

        // --- Weapon charge ---
        float weightWeaponChargeModerate = ContextWeightDefaults::WEAPON_CHARGE_MODERATE;
        float weightWeaponChargeLow = ContextWeightDefaults::WEAPON_CHARGE_LOW;
        float weightWeaponChargeCritical = ContextWeightDefaults::WEAPON_CHARGE_CRITICAL;

        // --- Active buff suppression ---
        float weightHasWaterbreathing = ContextWeightDefaults::HAS_WATERBREATHING;
        float weightHasInvisibility = ContextWeightDefaults::HAS_INVISIBILITY;
        float weightHasMuffle = ContextWeightDefaults::HAS_MUFFLE;
        float weightHasArmorBuff = ContextWeightDefaults::HAS_ARMOR_BUFF;
        float weightHasCloak = ContextWeightDefaults::HAS_CLOAK;
        float weightHasSummon = ContextWeightDefaults::HAS_SUMMON;

        // =====================================================================
        // NEW: NORMALIZED WEIGHTS [0,1] for ContextRuleEngine
        // =====================================================================
        // These replace the old 0-10 scale. Used by ContextRuleEngine::EvaluateRules()
        // to produce normalized context weights for multiplicative scoring.
        // =====================================================================

        // --- Health/resource restoration ---
        float weightCriticalHealth = ContextWeightDefaults::CRITICAL_HEALTH;
        float weightLowHealth = ContextWeightDefaults::LOW_HEALTH;
        float weightLowMagicka = ContextWeightDefaults::LOW_MAGICKA;
        float weightLowStamina = ContextWeightDefaults::LOW_STAMINA;

        // --- Combat/tactical ---
        float weightInCombat = ContextWeightDefaults::IN_COMBAT;
        float weightMultipleEnemies = ContextWeightDefaults::MULTIPLE_ENEMIES;
        float weightEnemyCasting = ContextWeightDefaults::ENEMY_CASTING;
        float weightSneaking = ContextWeightDefaults::SNEAKING;

        // --- Workstations ---
        float weightAtForge = ContextWeightDefaults::AT_FORGE;
        float weightAtEnchanter = ContextWeightDefaults::AT_ENCHANTER;
        float weightAtAlchemyLab = ContextWeightDefaults::AT_ALCHEMY_LAB;

        // --- Target-specific ---
        float weightTargetUndead = ContextWeightDefaults::TARGET_UNDEAD;
        float weightTargetDaedra = ContextWeightDefaults::TARGET_DAEDRA;
        float weightTargetDragon = ContextWeightDefaults::TARGET_DRAGON;

        // --- Equipment ---
        float weightNeedsAmmo = ContextWeightDefaults::NEEDS_AMMO;
        float weightNoWeapon = ContextWeightDefaults::NO_WEAPON;
        float weightWeapon = ContextWeightDefaults::WEAPON;
        float weightSpell = ContextWeightDefaults::SPELL;

        // --- Utility baseline ---
        float weightBaseRelevance = ContextWeightDefaults::BASE_RELEVANCE;

        // =====================================================================
        // CONTINUOUS FUNCTION SMOOTHING PARAMETERS
        // =====================================================================
        // Exponents for continuous weight curves (eliminates discontinuity cliffs)
        // Formula: weight = (1 - vitalPct)^exponent
        // Higher exponent = steeper curve (more urgent at low vitals)
        // =====================================================================

        float fHealthSmoothingExponent = ContextWeightDefaults::HEALTH_SMOOTHING_EXPONENT;
        float fMagickaSmoothingExponent = ContextWeightDefaults::MAGICKA_SMOOTHING_EXPONENT;
        float fStaminaSmoothingExponent = ContextWeightDefaults::STAMINA_SMOOTHING_EXPONENT;

    private:
        ContextWeightSettings() = default;
        ~ContextWeightSettings() = default;
        ContextWeightSettings(const ContextWeightSettings&) = delete;
        ContextWeightSettings& operator=(const ContextWeightSettings&) = delete;
    };

}  // namespace Huginn::State
