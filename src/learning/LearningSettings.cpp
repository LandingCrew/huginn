#include "LearningSettings.h"

namespace Huginn::Learning
{
    void LearningSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        if (!std::filesystem::exists(iniPath)) {
            logger::info("[LearningSettings] INI file not found, using defaults: {}"sv, iniPath.string());
            return;
        }

        CSimpleIniA ini;
        ini.SetUnicode();
        SI_Error rc = ini.LoadFile(iniPath.string().c_str());

        if (rc < 0) {
            logger::error("[LearningSettings] Failed to load INI file: {}"sv, iniPath.string());
            return;
        }

        const char* section = "Learning";

        learnFromExternalEquips = ini.GetBoolValue(section, "bLearnFromExternalEquips",
            LearningDefaults::LEARN_FROM_EXTERNAL_EQUIPS);

        externalEquipTimeWindow = static_cast<float>(
            ini.GetDoubleValue(section, "fExternalEquipTimeWindow",
                LearningDefaults::EXTERNAL_EQUIP_TIME_WINDOW));

        externalEquipMinInterval = static_cast<float>(
            ini.GetDoubleValue(section, "fExternalEquipMinInterval",
                LearningDefaults::EXTERNAL_EQUIP_MIN_INTERVAL));

        highUtilityRewardMult = static_cast<float>(
            ini.GetDoubleValue(section, "fHighUtilityRewardMult",
                LearningDefaults::HIGH_UTILITY_REWARD_MULT));

        mediumUtilityRewardMult = static_cast<float>(
            ini.GetDoubleValue(section, "fMediumUtilityRewardMult",
                LearningDefaults::MEDIUM_UTILITY_REWARD_MULT));

        lowUtilityRewardMult = static_cast<float>(
            ini.GetDoubleValue(section, "fLowUtilityRewardMult",
                LearningDefaults::LOW_UTILITY_REWARD_MULT));

        differentPageRewardMult = static_cast<float>(
            ini.GetDoubleValue(section, "fDifferentPageRewardMult",
                LearningDefaults::DIFFERENT_PAGE_REWARD_MULT));

        notCandidateRewardMult = static_cast<float>(
            ini.GetDoubleValue(section, "fNotCandidateRewardMult",
                LearningDefaults::NOT_CANDIDATE_REWARD_MULT));

        logger::info("[LearningSettings] Loaded: external={}, timeWindow={:.0f}ms, "
            "minInterval={:.1f}s, highMult={:.2f}, medMult={:.2f}, lowMult={:.2f}, "
            "pageMult={:.2f}, notCandidateMult={:.2f}",
            learnFromExternalEquips ? "on" : "off",
            externalEquipTimeWindow, externalEquipMinInterval,
            highUtilityRewardMult, mediumUtilityRewardMult,
            lowUtilityRewardMult, differentPageRewardMult, notCandidateRewardMult);
    }

    void LearningSettings::ResetToDefaults()
    {
        learnFromExternalEquips = LearningDefaults::LEARN_FROM_EXTERNAL_EQUIPS;
        externalEquipTimeWindow = LearningDefaults::EXTERNAL_EQUIP_TIME_WINDOW;
        externalEquipMinInterval = LearningDefaults::EXTERNAL_EQUIP_MIN_INTERVAL;
        highUtilityRewardMult = LearningDefaults::HIGH_UTILITY_REWARD_MULT;
        mediumUtilityRewardMult = LearningDefaults::MEDIUM_UTILITY_REWARD_MULT;
        lowUtilityRewardMult = LearningDefaults::LOW_UTILITY_REWARD_MULT;
        differentPageRewardMult = LearningDefaults::DIFFERENT_PAGE_REWARD_MULT;
        notCandidateRewardMult = LearningDefaults::NOT_CANDIDATE_REWARD_MULT;

        logger::info("[LearningSettings] Reset to defaults"sv);
    }

    LearningConfig LearningSettings::BuildConfig() const
    {
        LearningConfig config;
        config.learnFromExternalEquips = learnFromExternalEquips;
        config.externalEquipTimeWindow = externalEquipTimeWindow;
        config.externalEquipMinInterval = externalEquipMinInterval;
        config.highUtilityRewardMult = highUtilityRewardMult;
        config.mediumUtilityRewardMult = mediumUtilityRewardMult;
        config.lowUtilityRewardMult = lowUtilityRewardMult;
        config.differentPageRewardMult = differentPageRewardMult;
        config.notCandidateRewardMult = notCandidateRewardMult;
        return config;
    }

}  // namespace Huginn::Learning
