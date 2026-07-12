#include "ScorerSettings.h"
#include "IniLoad.h"

namespace Huginn::Scoring
{
    void ScorerSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        CSimpleIniA ini;
        if (LoadIniFile(ini, iniPath, "ScorerSettings"sv)) {
            LoadFromIni(ini);
        }
    }

    void ScorerSettings::LoadFromIni(const CSimpleIniA& ini)
    {
        // =====================================================================
        // [Scoring] section
        // =====================================================================
        const char* section = "Scoring";

        // Core
        lambdaMin = ReadClampedFloat(ini, section, "fLambdaMin", ScorerDefaults::LAMBDA_MIN, 0.0f, 1000.0f, "ScorerSettings"sv);
        lambdaMax = ReadClampedFloat(ini, section, "fLambdaMax", ScorerDefaults::LAMBDA_MAX, 0.0f, 1000.0f, "ScorerSettings"sv);
        explorationWeight = ReadClampedFloat(ini, section, "fExplorationWeight", ScorerDefaults::EXPLORATION_WEIGHT, 0.0f, 1000.0f, "ScorerSettings"sv);

        // Correlation bonuses
        bowArrowBonus = ReadClampedFloat(ini, section, "fBowArrowBonus", ScorerDefaults::BOW_ARROW_BONUS, 0.0f, 1000.0f, "ScorerSettings"sv);
        crossbowBoltBonus = ReadClampedFloat(ini, section, "fCrossbowBoltBonus", ScorerDefaults::CROSSBOW_BOLT_BONUS, 0.0f, 1000.0f, "ScorerSettings"sv);
        meleeDefensiveBonus = ReadClampedFloat(ini, section, "fMeleeDefensiveBonus", ScorerDefaults::MELEE_DEFENSIVE_BONUS, 0.0f, 1000.0f, "ScorerSettings"sv);
        silverUndeadBonus = ReadClampedFloat(ini, section, "fSilverUndeadBonus", ScorerDefaults::SILVER_UNDEAD_BONUS, 0.0f, 1000.0f, "ScorerSettings"sv);
        fortifySchoolBonus = ReadClampedFloat(ini, section, "fFortifySchoolBonus", ScorerDefaults::FORTIFY_SCHOOL_BONUS, 0.0f, 1000.0f, "ScorerSettings"sv);
        staffLowMagickaBonus = ReadClampedFloat(ini, section, "fStaffLowMagickaBonus", ScorerDefaults::STAFF_LOW_MAGICKA_BONUS, 0.0f, 1000.0f, "ScorerSettings"sv);
        twoHandedDefensiveBonus = ReadClampedFloat(ini, section, "fTwoHandedDefensiveBonus", ScorerDefaults::TWO_HANDED_DEFENSIVE_BONUS, 0.0f, 1000.0f, "ScorerSettings"sv);

        // Potion discrimination
        combatStartWindow = ReadClampedFloat(ini, section, "fCombatStartWindow", ScorerDefaults::COMBAT_START_WINDOW, 0.0f, 1000.0f, "ScorerSettings"sv);
        regenPotionCombatStartMult = ReadClampedFloat(ini, section, "fRegenPotionCombatStartMult", ScorerDefaults::REGEN_POTION_COMBAT_START_MULT, 0.0f, 1000.0f, "ScorerSettings"sv);
        flatRestoreLowResourceMult = ReadClampedFloat(ini, section, "fFlatRestoreLowResourceMult", ScorerDefaults::FLAT_RESTORE_LOW_RESOURCE_MULT, 0.0f, 1000.0f, "ScorerSettings"sv);
        magnitudeValueScale = ReadClampedFloat(ini, section, "fMagnitudeValueScale", ScorerDefaults::MAGNITUDE_VALUE_SCALE, 0.0f, 1000.0f, "ScorerSettings"sv);

        // Thresholds
        minimumUtility = ReadClampedFloat(ini, section, "fMinimumUtility", ScorerDefaults::MINIMUM_UTILITY, 0.0f, 1000.0f, "ScorerSettings"sv);
        minimumContextWeight = ReadClampedFloat(ini, section, "fMinimumContextWeight", ScorerDefaults::MINIMUM_CONTEXT_WEIGHT, 0.0f, 1000.0f, "ScorerSettings"sv);
        coldStartUCBBoost = ReadClampedFloat(ini, section, "fColdStartUCBBoost", ScorerDefaults::COLD_START_UCB_BOOST, 0.0f, 1000.0f, "ScorerSettings"sv);

        // Performance
        maxCandidatesPerCycle = static_cast<size_t>(
            ini.GetLongValue(section, "iMaxCandidatesPerCycle", static_cast<long>(ScorerDefaults::MAX_CANDIDATES_PER_CYCLE)));
        topNCandidates = static_cast<size_t>(
            ini.GetLongValue(section, "iTopNCandidates", static_cast<long>(ScorerDefaults::TOP_N_CANDIDATES)));

        // =====================================================================
        // [Favorites] section
        // =====================================================================
        const char* favSection = "Favorites";

        const char* modeStr = ini.GetValue(favSection, "sFavoritesMode", ScorerDefaults::FAVORITES_MODE);
        favoritesMode = ParseFavoritesMode(modeStr);

        favoritesBoostMin = ReadClampedFloat(ini, favSection, "fFavoritesBoostMin", ScorerDefaults::FAVORITES_BOOST_MIN, 0.0f, 1000.0f, "ScorerSettings"sv);
        favoritesBoostMax = ReadClampedFloat(ini, favSection, "fFavoritesBoostMax", ScorerDefaults::FAVORITES_BOOST_MAX, 0.0f, 1000.0f, "ScorerSettings"sv);

        logger::info("[ScorerSettings] Loaded: λMin={:.2f}, λMax={:.2f}, "
            "exploration={:.2f}, favorites={}, boostRange=[{:.1f}, {:.1f}], "
            "minUtility={:.2f}, minCtxWeight={:.2f}, coldStart={:.2f}, topN={}",
            lambdaMin, lambdaMax, explorationWeight,
            favoritesMode == FavoritesMode::Boost ? "Boost" :
                favoritesMode == FavoritesMode::Off ? "Off" : "Suppress",
            favoritesBoostMin, favoritesBoostMax,
            minimumUtility, minimumContextWeight, coldStartUCBBoost, topNCandidates);
    }

    void ScorerSettings::ResetToDefaults()
    {
        lambdaMin = ScorerDefaults::LAMBDA_MIN;
        lambdaMax = ScorerDefaults::LAMBDA_MAX;
        explorationWeight = ScorerDefaults::EXPLORATION_WEIGHT;

        bowArrowBonus = ScorerDefaults::BOW_ARROW_BONUS;
        crossbowBoltBonus = ScorerDefaults::CROSSBOW_BOLT_BONUS;
        meleeDefensiveBonus = ScorerDefaults::MELEE_DEFENSIVE_BONUS;
        silverUndeadBonus = ScorerDefaults::SILVER_UNDEAD_BONUS;
        fortifySchoolBonus = ScorerDefaults::FORTIFY_SCHOOL_BONUS;
        staffLowMagickaBonus = ScorerDefaults::STAFF_LOW_MAGICKA_BONUS;
        twoHandedDefensiveBonus = ScorerDefaults::TWO_HANDED_DEFENSIVE_BONUS;

        combatStartWindow = ScorerDefaults::COMBAT_START_WINDOW;
        regenPotionCombatStartMult = ScorerDefaults::REGEN_POTION_COMBAT_START_MULT;
        flatRestoreLowResourceMult = ScorerDefaults::FLAT_RESTORE_LOW_RESOURCE_MULT;
        magnitudeValueScale = ScorerDefaults::MAGNITUDE_VALUE_SCALE;

        minimumUtility = ScorerDefaults::MINIMUM_UTILITY;
        minimumContextWeight = ScorerDefaults::MINIMUM_CONTEXT_WEIGHT;
        coldStartUCBBoost = ScorerDefaults::COLD_START_UCB_BOOST;

        maxCandidatesPerCycle = ScorerDefaults::MAX_CANDIDATES_PER_CYCLE;
        topNCandidates = ScorerDefaults::TOP_N_CANDIDATES;

        favoritesMode = FavoritesMode::Boost;
        favoritesBoostMin = ScorerDefaults::FAVORITES_BOOST_MIN;
        favoritesBoostMax = ScorerDefaults::FAVORITES_BOOST_MAX;

        logger::info("[ScorerSettings] Reset to defaults"sv);
    }

    ScorerConfig ScorerSettings::BuildConfig() const
    {
        ScorerConfig cfg;

        cfg.lambdaMin = lambdaMin;
        cfg.lambdaMax = lambdaMax;
        cfg.explorationWeight = explorationWeight;

        cfg.bowArrowBonus = bowArrowBonus;
        cfg.crossbowBoltBonus = crossbowBoltBonus;
        cfg.meleeDefensiveBonus = meleeDefensiveBonus;
        cfg.silverUndeadBonus = silverUndeadBonus;
        cfg.fortifySchoolBonus = fortifySchoolBonus;
        cfg.staffLowMagickaBonus = staffLowMagickaBonus;
        cfg.twoHandedDefensiveBonus = twoHandedDefensiveBonus;

        cfg.combatStartWindow = combatStartWindow;
        cfg.regenPotionCombatStartMult = regenPotionCombatStartMult;
        cfg.flatRestoreLowResourceMult = flatRestoreLowResourceMult;
        cfg.magnitudeValueScale = magnitudeValueScale;

        cfg.minimumUtility = minimumUtility;
        cfg.minimumContextWeight = minimumContextWeight;
        cfg.coldStartUCBBoost = coldStartUCBBoost;

        cfg.maxCandidatesPerCycle = maxCandidatesPerCycle;
        cfg.topNCandidates = topNCandidates;

        cfg.favoritesMode = favoritesMode;
        cfg.favoritesBoostMin = favoritesBoostMin;
        cfg.favoritesBoostMax = favoritesBoostMax;

        return cfg;
    }

    FavoritesMode ScorerSettings::ParseFavoritesMode(const char* str)
    {
        if (_stricmp(str, "Off") == 0) return FavoritesMode::Off;
        if (_stricmp(str, "Suppress") == 0) return FavoritesMode::Suppress;
        return FavoritesMode::Boost;  // Default
    }

}  // namespace Huginn::Scoring
