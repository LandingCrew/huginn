#include "ScorerSettings.h"

namespace Huginn::Scoring
{
    void ScorerSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        if (!std::filesystem::exists(iniPath)) {
            logger::info("[ScorerSettings] INI file not found, using defaults: {}"sv, iniPath.string());
            return;
        }

        CSimpleIniA ini;
        ini.SetUnicode();
        SI_Error rc = ini.LoadFile(iniPath.string().c_str());

        if (rc < 0) {
            logger::error("[ScorerSettings] Failed to load INI file: {}"sv, iniPath.string());
            return;
        }

        // =====================================================================
        // [Scoring] section
        // =====================================================================
        const char* section = "Scoring";

        // Core
        lambdaMin = static_cast<float>(
            ini.GetDoubleValue(section, "fLambdaMin", ScorerDefaults::LAMBDA_MIN));
        lambdaMax = static_cast<float>(
            ini.GetDoubleValue(section, "fLambdaMax", ScorerDefaults::LAMBDA_MAX));
        explorationWeight = static_cast<float>(
            ini.GetDoubleValue(section, "fExplorationWeight", ScorerDefaults::EXPLORATION_WEIGHT));

        // Correlation bonuses
        bowArrowBonus = static_cast<float>(
            ini.GetDoubleValue(section, "fBowArrowBonus", ScorerDefaults::BOW_ARROW_BONUS));
        crossbowBoltBonus = static_cast<float>(
            ini.GetDoubleValue(section, "fCrossbowBoltBonus", ScorerDefaults::CROSSBOW_BOLT_BONUS));
        meleeDefensiveBonus = static_cast<float>(
            ini.GetDoubleValue(section, "fMeleeDefensiveBonus", ScorerDefaults::MELEE_DEFENSIVE_BONUS));
        silverUndeadBonus = static_cast<float>(
            ini.GetDoubleValue(section, "fSilverUndeadBonus", ScorerDefaults::SILVER_UNDEAD_BONUS));
        fortifySchoolBonus = static_cast<float>(
            ini.GetDoubleValue(section, "fFortifySchoolBonus", ScorerDefaults::FORTIFY_SCHOOL_BONUS));
        staffLowMagickaBonus = static_cast<float>(
            ini.GetDoubleValue(section, "fStaffLowMagickaBonus", ScorerDefaults::STAFF_LOW_MAGICKA_BONUS));
        twoHandedDefensiveBonus = static_cast<float>(
            ini.GetDoubleValue(section, "fTwoHandedDefensiveBonus", ScorerDefaults::TWO_HANDED_DEFENSIVE_BONUS));

        // Potion discrimination
        combatStartWindow = static_cast<float>(
            ini.GetDoubleValue(section, "fCombatStartWindow", ScorerDefaults::COMBAT_START_WINDOW));
        regenPotionCombatStartMult = static_cast<float>(
            ini.GetDoubleValue(section, "fRegenPotionCombatStartMult", ScorerDefaults::REGEN_POTION_COMBAT_START_MULT));
        flatRestoreLowResourceMult = static_cast<float>(
            ini.GetDoubleValue(section, "fFlatRestoreLowResourceMult", ScorerDefaults::FLAT_RESTORE_LOW_RESOURCE_MULT));
        magnitudeValueScale = static_cast<float>(
            ini.GetDoubleValue(section, "fMagnitudeValueScale", ScorerDefaults::MAGNITUDE_VALUE_SCALE));

        // Thresholds
        minimumUtility = static_cast<float>(
            ini.GetDoubleValue(section, "fMinimumUtility", ScorerDefaults::MINIMUM_UTILITY));
        minimumContextWeight = static_cast<float>(
            ini.GetDoubleValue(section, "fMinimumContextWeight", ScorerDefaults::MINIMUM_CONTEXT_WEIGHT));
        coldStartUCBBoost = static_cast<float>(
            ini.GetDoubleValue(section, "fColdStartUCBBoost", ScorerDefaults::COLD_START_UCB_BOOST));

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

        favoritesBoostMin = static_cast<float>(
            ini.GetDoubleValue(favSection, "fFavoritesBoostMin", ScorerDefaults::FAVORITES_BOOST_MIN));
        favoritesBoostMax = static_cast<float>(
            ini.GetDoubleValue(favSection, "fFavoritesBoostMax", ScorerDefaults::FAVORITES_BOOST_MAX));

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
