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
        inline constexpr const char* POTION_TIER_PREFERENCE = "Higher";

        // Item-context fit (FitScorer). FIT_MODE: 0 Off, 1 Shadow, 2 Apply.
        inline constexpr long FIT_MODE = 1;
        inline constexpr float FIT_AFFORD_MIN = 0.7f;
        inline constexpr float FIT_AFFORD_FULL_CASTS = 3.0f;
        inline constexpr float FIT_UNAFFORDABLE_MULT = 0.3f;
        inline constexpr float FIT_CONCENTRATION_SECONDS_PER_CAST = 2.0f;
        inline constexpr float FIT_OUT_OF_RANGE_MULT = 0.6f;
        inline constexpr float FIT_CLAMP_MIN = 0.2f;
        inline constexpr float FIT_CLAMP_MAX = 1.0f;

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
    // item fit, thresholds, performance) and [Favorites] section (mode, boost range).
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

        // Parse `iniPath` from disk, then load [Scoring]/[Favorites]. Thin wrapper
        // over LoadFromIni for standalone callers.
        void LoadFromFile(const std::filesystem::path& iniPath);
        // Load [Scoring]/[Favorites] from an already-parsed INI (parse-once path).
        void LoadFromIni(const CSimpleIniA& ini);
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
        [[nodiscard]] static PotionTierPreference ParsePotionTierPreference(const char* str);

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
        PotionTierPreference potionTierPreference = PotionTierPreference::Higher;

        // --- Item-context fit (FitScorer) ---
        FitMode fitMode = static_cast<FitMode>(ScorerDefaults::FIT_MODE);
        float fitAffordMin = ScorerDefaults::FIT_AFFORD_MIN;
        float fitAffordFullCasts = ScorerDefaults::FIT_AFFORD_FULL_CASTS;
        float fitUnaffordableMult = ScorerDefaults::FIT_UNAFFORDABLE_MULT;
        float fitConcentrationSecondsPerCast = ScorerDefaults::FIT_CONCENTRATION_SECONDS_PER_CAST;
        float fitOutOfRangeMult = ScorerDefaults::FIT_OUT_OF_RANGE_MULT;
        float fitClampMin = ScorerDefaults::FIT_CLAMP_MIN;
        float fitClampMax = ScorerDefaults::FIT_CLAMP_MAX;

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
