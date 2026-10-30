#include "ScorerSettings.h"
#include "IniLoad.h"
#include "state/StateManager.h"   // SetRefreshPriorSec (R8.1)

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

        // Thresholds
        minimumUtility = ReadClampedFloat(ini, section, "fMinimumUtility", ScorerDefaults::MINIMUM_UTILITY, 0.0f, 1000.0f, "ScorerSettings"sv);
        minimumContextWeight = ReadClampedFloat(ini, section, "fMinimumContextWeight", ScorerDefaults::MINIMUM_CONTEXT_WEIGHT, 0.0f, 1000.0f, "ScorerSettings"sv);
        coldStartUCBBoost = ReadClampedFloat(ini, section, "fColdStartUCBBoost", ScorerDefaults::COLD_START_UCB_BOOST, 0.0f, 1000.0f, "ScorerSettings"sv);

        // Performance
        topNCandidates = static_cast<size_t>(
            ini.GetLongValue(section, "iTopNCandidates", static_cast<long>(ScorerDefaults::TOP_N_CANDIDATES)));

        // Refresh pass (R8.1): pushed to the state layer, which computes the
        // active-buff filters' refresh bits on its next magic-effects poll.
        refreshPriorSec = ReadClampedFloat(ini, section, "fRefreshPriorSec", ScorerDefaults::REFRESH_PRIOR_SEC,
            0.0f, ScorerDefaults::REFRESH_PRIOR_SEC_MAX, "ScorerSettings"sv);
        State::StateManager::GetSingleton().SetRefreshPriorSec(refreshPriorSec);

        // =====================================================================
        // [Favorites] section
        // =====================================================================
        const char* favSection = "Favorites";

        const char* modeStr = ini.GetValue(favSection, "sFavoritesMode", ScorerDefaults::FAVORITES_MODE);
        favoritesMode = ParseFavoritesMode(modeStr);

        favoritesBoostMax = ReadClampedFloat(ini, favSection, "fFavoritesBoostMax", ScorerDefaults::FAVORITES_BOOST_MAX, 0.0f, 1000.0f, "ScorerSettings"sv);

        logger::info("[ScorerSettings] Loaded: λMin={:.2f}, λMax={:.2f}, "
            "exploration={:.2f}, favorites={}, boost={:.1f}, "
            "minUtility={:.2f}, minCtxWeight={:.2f}, coldStart={:.2f}, topN={}, "
            "refreshPrior={:.1f}s",
            lambdaMin, lambdaMax, explorationWeight,
            favoritesMode == FavoritesMode::Boost ? "Boost" :
                favoritesMode == FavoritesMode::Off ? "Off" : "Suppress",
            favoritesBoostMax,
            minimumUtility, minimumContextWeight, coldStartUCBBoost, topNCandidates,
            refreshPriorSec);
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

        minimumUtility = ScorerDefaults::MINIMUM_UTILITY;
        minimumContextWeight = ScorerDefaults::MINIMUM_CONTEXT_WEIGHT;
        coldStartUCBBoost = ScorerDefaults::COLD_START_UCB_BOOST;

        topNCandidates = ScorerDefaults::TOP_N_CANDIDATES;

        refreshPriorSec = ScorerDefaults::REFRESH_PRIOR_SEC;
        State::StateManager::GetSingleton().SetRefreshPriorSec(refreshPriorSec);

        favoritesMode = FavoritesMode::Boost;
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

        cfg.minimumUtility = minimumUtility;
        cfg.minimumContextWeight = minimumContextWeight;
        cfg.coldStartUCBBoost = coldStartUCBBoost;

        cfg.topNCandidates = topNCandidates;

        cfg.favoritesMode = favoritesMode;
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
