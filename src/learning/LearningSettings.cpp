#include "LearningSettings.h"
#include "IniLoad.h"

namespace Huginn::Learning
{
    void LearningSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        CSimpleIniA ini;
        if (LoadIniFile(ini, iniPath, "LearningSettings"sv)) {
            LoadFromIni(ini);
        }
    }

    void LearningSettings::LoadFromIni(const CSimpleIniA& ini)
    {
        const char* section = "Learning";

        learnFromExternalEquips = ini.GetBoolValue(section, "bLearnFromExternalEquips",
            LearningDefaults::LEARN_FROM_EXTERNAL_EQUIPS);

        externalEquipTimeWindow = static_cast<float>(
            ini.GetDoubleValue(section, "fExternalEquipTimeWindow",
                LearningDefaults::EXTERNAL_EQUIP_TIME_WINDOW));

        logger::info("[LearningSettings] Loaded: external={}, timeWindow={:.0f}ms"sv,
            learnFromExternalEquips ? "on" : "off", externalEquipTimeWindow);
    }

    void LearningSettings::ResetToDefaults()
    {
        learnFromExternalEquips = LearningDefaults::LEARN_FROM_EXTERNAL_EQUIPS;
        externalEquipTimeWindow = LearningDefaults::EXTERNAL_EQUIP_TIME_WINDOW;

        logger::info("[LearningSettings] Reset to defaults"sv);
    }

    LearningConfig LearningSettings::BuildConfig() const
    {
        LearningConfig config;
        config.learnFromExternalEquips = learnFromExternalEquips;
        config.externalEquipTimeWindow = externalEquipTimeWindow;
        return config;
    }

}  // namespace Huginn::Learning
