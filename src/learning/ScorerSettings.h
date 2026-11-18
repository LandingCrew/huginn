#pragma once

#include "ScorerConfig.h"
#include <SimpleIni.h>
#include <filesystem>

namespace Huginn::Scoring
{
    // =========================================================================
    // DEFAULT VALUES (compile-time constants)
    // =========================================================================
    // These match the current ScorerConfig struct defaults exactly.
    // Used as fallbacks when INI keys are missing.

    namespace ScorerDefaults
    {
        // Core scoring
        inline constexpr float LAMBDA_MIN = 0.5f;
        inline constexpr float LAMBDA_MAX = 3.0f;
        inline constexpr float EXPLORATION_WEIGHT = 0.2f;

        // Correlation bonuses
        inline constexpr float BOW_ARROW_BONUS = 2.0f;
        inline constexpr float CROSSBOW_BOLT_BONUS = 2.0f;
        inline constexpr float MELEE_DEFENSIVE_BONUS = 1.5f;
        inline constexpr float SILVER_UNDEAD_BONUS = 2.0f;
        inline constexpr float FORTIFY_SCHOOL_BONUS = 2.0f;
        inline constexpr float STAFF_LOW_MAGICKA_BONUS = 1.5f;
        inline constexpr float TWO_HANDED_DEFENSIVE_BONUS = 1.2f;

        // Potion discrimination
        inline constexpr float COMBAT_START_WINDOW = 10.0f;
        inline constexpr float REGEN_POTION_COMBAT_START_MULT = 1.5f;
        inline constexpr float FLAT_RESTORE_LOW_RESOURCE_MULT = 1.5f;

        // Thresholds
        inline constexpr float MINIMUM_UTILITY = 0.1f;
        inline constexpr float MINIMUM_CONTEXT_WEIGHT = 0.05f;
        inline constexpr float COLD_START_UCB_BOOST = 0.2f;

        // Performance
        inline constexpr size_t TOP_N_CANDIDATES = 10;

        // R8.1 (0.23.55): the refresh pass (doc 11 decision 4). An active-buff
        // filter lets its item through in the buff's last N seconds, and
        // covered(i)'s left(r) prior is centred there. 0 turns the pass off.
        inline constexpr float REFRESH_PRIOR_SEC = 5.0f;
        inline constexpr float REFRESH_PRIOR_SEC_MAX = 120.0f;

        // Favorites
        inline constexpr const char* FAVORITES_MODE = "Boost";
        inline constexpr float FAVORITES_BOOST_MAX = 2.5f;

    }

    // =========================================================================
    // SCORER SETTINGS
    // =========================================================================
    // Singleton that loads scoring parameters from Data/SKSE/Plugins/Huginn.ini.
    // Reads [Scoring] section (core, correlations, potion discrimination,
    // thresholds, performance, the refresh pass) and [Favorites] section
    // (mode, boost). R8.4 (0.24.1): everything here but the refresh pass and
    // the favourites mode feeds only the D2 legacy feature
    // (learning/legacy/LegacyLearnFeature.h): which rows the old floors keep
    // and their ln learn mult. Nothing ranks on the old utility.
    //
    // Call BuildConfig() to produce a ScorerConfig struct suitable for
    // UtilityScorer::SetConfig().
    // =========================================================================

    class ScorerSettings
    {
    public:
        static ScorerSettings& GetSingleton()
        {
            static ScorerSettings instance;
            return instance;
        }

        // Load [Scoring]/[Favorites] from an already-parsed INI (parse-once path;
        // Settings::LoadMainIni, over the shipped Huginn.ini, whose values are
        // the defaults).
        void LoadFromIni(const CSimpleIniA& ini);

        // Build a ScorerConfig struct from current settings
        [[nodiscard]] ScorerConfig BuildConfig() const;

    private:
        ScorerSettings() = default;
        ~ScorerSettings() = default;
        ScorerSettings(const ScorerSettings&) = delete;
        ScorerSettings& operator=(const ScorerSettings&) = delete;

        // Helper: parse string to FavoritesMode enum
        [[nodiscard]] static FavoritesMode ParseFavoritesMode(const char* str);

        // --- Core Scoring ---
        float lambdaMin = ScorerDefaults::LAMBDA_MIN;
        float lambdaMax = ScorerDefaults::LAMBDA_MAX;
        float explorationWeight = ScorerDefaults::EXPLORATION_WEIGHT;

        // --- Correlation Bonuses ---
        float bowArrowBonus = ScorerDefaults::BOW_ARROW_BONUS;
        float crossbowBoltBonus = ScorerDefaults::CROSSBOW_BOLT_BONUS;
        float meleeDefensiveBonus = ScorerDefaults::MELEE_DEFENSIVE_BONUS;
        float silverUndeadBonus = ScorerDefaults::SILVER_UNDEAD_BONUS;
        float fortifySchoolBonus = ScorerDefaults::FORTIFY_SCHOOL_BONUS;
        float staffLowMagickaBonus = ScorerDefaults::STAFF_LOW_MAGICKA_BONUS;
        float twoHandedDefensiveBonus = ScorerDefaults::TWO_HANDED_DEFENSIVE_BONUS;

        // --- Potion Discrimination ---
        float combatStartWindow = ScorerDefaults::COMBAT_START_WINDOW;
        float regenPotionCombatStartMult = ScorerDefaults::REGEN_POTION_COMBAT_START_MULT;
        float flatRestoreLowResourceMult = ScorerDefaults::FLAT_RESTORE_LOW_RESOURCE_MULT;

        // --- Thresholds ---
        float minimumUtility = ScorerDefaults::MINIMUM_UTILITY;
        float minimumContextWeight = ScorerDefaults::MINIMUM_CONTEXT_WEIGHT;
        float coldStartUCBBoost = ScorerDefaults::COLD_START_UCB_BOOST;

        // --- Performance ---
        size_t topNCandidates = ScorerDefaults::TOP_N_CANDIDATES;

        // --- Refresh pass (R8.1) --- pushed into StateManager on every load
        // (SetRefreshPriorSec), which computes the filters' bits with it.
        float refreshPriorSec = ScorerDefaults::REFRESH_PRIOR_SEC;

        // --- Favorites ---
        FavoritesMode favoritesMode = FavoritesMode::Boost;
        float favoritesBoostMax = ScorerDefaults::FAVORITES_BOOST_MAX;

    };

}  // namespace Huginn::Scoring
