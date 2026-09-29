#include "ScorerSettings.h"
#include "IniLoad.h"
#include <algorithm>
#include <utility>

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
        potionTierPreference = ParsePotionTierPreference(
            ini.GetValue(section, "sPotionTierPreference", ScorerDefaults::POTION_TIER_PREFERENCE));

        // Item-context fit (FitScorer)
        {
            const long rawMode = ini.GetLongValue(section, "iFitMode", ScorerDefaults::FIT_MODE);
            const long mode = std::clamp(rawMode, 0L, 2L);
            if (mode != rawMode) {
                logger::warn("[ScorerSettings] iFitMode = {} out of range [0, 2], clamped to {}"sv, rawMode, mode);
            }
            fitMode = static_cast<FitMode>(mode);
        }
        fitAffordMin = ReadClampedFloat(ini, section, "fFitAffordMin", ScorerDefaults::FIT_AFFORD_MIN, 0.0f, 1.0f, "ScorerSettings"sv);
        fitAffordFullCasts = ReadClampedFloat(ini, section, "fFitAffordFullCasts", ScorerDefaults::FIT_AFFORD_FULL_CASTS, 1.0f, 100.0f, "ScorerSettings"sv);
        fitUnaffordableMult = ReadClampedFloat(ini, section, "fFitUnaffordableMult", ScorerDefaults::FIT_UNAFFORDABLE_MULT, 0.0f, 1.0f, "ScorerSettings"sv);
        fitConcentrationSecondsPerCast = ReadClampedFloat(ini, section, "fFitConcentrationSecondsPerCast", ScorerDefaults::FIT_CONCENTRATION_SECONDS_PER_CAST, 0.1f, 60.0f, "ScorerSettings"sv);
        fitOutOfRangeMult = ReadClampedFloat(ini, section, "fFitOutOfRangeMult", ScorerDefaults::FIT_OUT_OF_RANGE_MULT, 0.0f, 1.0f, "ScorerSettings"sv);
        fitClampMin = ReadClampedFloat(ini, section, "fFitClampMin", ScorerDefaults::FIT_CLAMP_MIN, 0.0f, 1.0f, "ScorerSettings"sv);
        fitClampMax = ReadClampedFloat(ini, section, "fFitClampMax", ScorerDefaults::FIT_CLAMP_MAX, 0.0f, 10.0f, "ScorerSettings"sv);
        if (fitClampMin > fitClampMax) {
            logger::warn("[ScorerSettings] fFitClampMin ({:.2f}) > fFitClampMax ({:.2f}), swapping"sv,
                fitClampMin, fitClampMax);
            std::swap(fitClampMin, fitClampMax);
        }

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
            "minUtility={:.2f}, minCtxWeight={:.2f}, coldStart={:.2f}, topN={}, potionTier={}",
            lambdaMin, lambdaMax, explorationWeight,
            favoritesMode == FavoritesMode::Boost ? "Boost" :
                favoritesMode == FavoritesMode::Off ? "Off" : "Suppress",
            favoritesBoostMin, favoritesBoostMax,
            minimumUtility, minimumContextWeight, coldStartUCBBoost, topNCandidates,
            potionTierPreference == PotionTierPreference::Higher ? "Higher" :
                potionTierPreference == PotionTierPreference::Lower ? "Lower" : "None");
        logger::info("[ScorerSettings] Fit: mode={}, affordMin={:.2f}, affordFullCasts={:.1f}, "
            "unaffordable={:.2f}, concSecPerCast={:.1f}, outOfRange={:.2f}, clamp=[{:.2f}, {:.2f}]",
            FitModeToString(fitMode), fitAffordMin, fitAffordFullCasts,
            fitUnaffordableMult, fitConcentrationSecondsPerCast, fitOutOfRangeMult,
            fitClampMin, fitClampMax);
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
        potionTierPreference = PotionTierPreference::Higher;

        fitMode = static_cast<FitMode>(ScorerDefaults::FIT_MODE);
        fitAffordMin = ScorerDefaults::FIT_AFFORD_MIN;
        fitAffordFullCasts = ScorerDefaults::FIT_AFFORD_FULL_CASTS;
        fitUnaffordableMult = ScorerDefaults::FIT_UNAFFORDABLE_MULT;
        fitConcentrationSecondsPerCast = ScorerDefaults::FIT_CONCENTRATION_SECONDS_PER_CAST;
        fitOutOfRangeMult = ScorerDefaults::FIT_OUT_OF_RANGE_MULT;
        fitClampMin = ScorerDefaults::FIT_CLAMP_MIN;
        fitClampMax = ScorerDefaults::FIT_CLAMP_MAX;

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
        cfg.potionTierPreference = potionTierPreference;

        cfg.fitMode = fitMode;
        cfg.fitAffordMin = fitAffordMin;
        cfg.fitAffordFullCasts = fitAffordFullCasts;
        cfg.fitUnaffordableMult = fitUnaffordableMult;
        cfg.fitConcentrationSecondsPerCast = fitConcentrationSecondsPerCast;
        cfg.fitOutOfRangeMult = fitOutOfRangeMult;
        cfg.fitClampMin = fitClampMin;
        cfg.fitClampMax = fitClampMax;

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

    PotionTierPreference ScorerSettings::ParsePotionTierPreference(const char* str)
    {
        if (str && _stricmp(str, "None") == 0) return PotionTierPreference::None;
        if (str && _stricmp(str, "Lower") == 0) return PotionTierPreference::Lower;
        return PotionTierPreference::Higher;  // Default: use the best one now
    }

}  // namespace Huginn::Scoring
