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
        inline constexpr float MAGNITUDE_VALUE_SCALE = 0.3f;

        // Thresholds
        inline constexpr float MINIMUM_UTILITY = 0.1f;
        inline constexpr float MINIMUM_CONTEXT_WEIGHT = 0.05f;
        inline constexpr float COLD_START_UCB_BOOST = 0.2f;

        // Performance
        inline constexpr size_t MAX_CANDIDATES_PER_CYCLE = 500;
        inline constexpr size_t TOP_N_CANDIDATES = 10;

        // Favorites
        inline constexpr const char* FAVORITES_MODE = "Boost";
        inline constexpr float FAVORITES_BOOST_MIN = 1.3f;
        inline constexpr float FAVORITES_BOOST_MAX = 2.5f;

    }

    // =========================================================================
    // SCORER SETTINGS
    // =========================================================================
    // Singleton that loads scoring parameters from Data/SKSE/Plugins/Huginn.ini.
    // Reads [Scoring] section (core, correlations, potion discrimination,
    // thresholds, performance) and [Favorites] section (mode, boost range).
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

        void LoadFromFile(const std::filesystem::path& iniPath);
        void ResetToDefaults();

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
        float magnitudeValueScale = ScorerDefaults::MAGNITUDE_VALUE_SCALE;

        // --- Thresholds ---
        float minimumUtility = ScorerDefaults::MINIMUM_UTILITY;
        float minimumContextWeight = ScorerDefaults::MINIMUM_CONTEXT_WEIGHT;
        float coldStartUCBBoost = ScorerDefaults::COLD_START_UCB_BOOST;

        // --- Performance ---
        size_t maxCandidatesPerCycle = ScorerDefaults::MAX_CANDIDATES_PER_CYCLE;
        size_t topNCandidates = ScorerDefaults::TOP_N_CANDIDATES;

        // --- Favorites ---
        FavoritesMode favoritesMode = FavoritesMode::Boost;
        float favoritesBoostMin = ScorerDefaults::FAVORITES_BOOST_MIN;
        float favoritesBoostMax = ScorerDefaults::FAVORITES_BOOST_MAX;

    };

}  // namespace Huginn::Scoring
