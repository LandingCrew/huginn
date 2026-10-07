#include "LearningSettings.h"
#include "IniLoad.h"

#include <algorithm>

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

        // Memory with a useful life (roadmap Phase 3 #3a). Clamped again by
        // the learner; these ranges are the documented ones.
        const MemoryLife defaults{};
        memory.enabled = ini.GetBoolValue(section, "bForgetUnusedItems", defaults.enabled);
        memory.lifeHours = std::clamp(static_cast<float>(
            ini.GetDoubleValue(section, "fUsefulLifeHours", defaults.lifeHours)), 0.1f, 10000.0f);
        memory.lifePerPickHours = std::clamp(static_cast<float>(
            ini.GetDoubleValue(section, "fUsefulLifePerPickHours", defaults.lifePerPickHours)), 0.0f, 1000.0f);
        memory.fadeHours = std::clamp(static_cast<float>(
            ini.GetDoubleValue(section, "fFadeHours", defaults.fadeHours)), 0.05f, 1000.0f);
        memory.forgetBelow = std::clamp(static_cast<float>(
            ini.GetDoubleValue(section, "fForgetBelow", defaults.forgetBelow)), 0.0f, 0.5f);

        logger::info("[LearningSettings] Loaded: external={}, timeWindow={:.0f}ms"sv,
            learnFromExternalEquips ? "on" : "off", externalEquipTimeWindow);
        if (memory.enabled) {
            logger::info("[LearningSettings] Useful life: {:.1f}h + {:.1f}h x ln(1 + picks) of play, "
                "fading over ~{:.1f}h, forgotten under {:.0f}%"sv,
                memory.lifeHours, memory.lifePerPickHours, memory.fadeHours, 100.0f * memory.forgetBelow);
        } else {
            logger::info("[LearningSettings] Useful life: off (nothing fades or is forgotten)"sv);
        }
    }

    void LearningSettings::ResetToDefaults()
    {
        learnFromExternalEquips = LearningDefaults::LEARN_FROM_EXTERNAL_EQUIPS;
        externalEquipTimeWindow = LearningDefaults::EXTERNAL_EQUIP_TIME_WINDOW;
        memory = MemoryLife{};

        logger::info("[LearningSettings] Reset to defaults"sv);
    }

    LearningConfig LearningSettings::BuildConfig() const
    {
        LearningConfig config;
        config.learnFromExternalEquips = learnFromExternalEquips;
        config.externalEquipTimeWindow = externalEquipTimeWindow;
        config.memory = memory;
        return config;
    }

}  // namespace Huginn::Learning
