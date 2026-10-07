#pragma once

#include "FeatureBanditLearner.h"
#include <SimpleIni.h>
#include <filesystem>

namespace Huginn::Learning
{
    // =========================================================================
    // DEFAULT VALUES (compile-time constants)
    // =========================================================================

    namespace LearningDefaults
    {
        // Master toggle
        inline constexpr bool LEARN_FROM_EXTERNAL_EQUIPS = true;

        // Attribution window: max cache age for pipeline state to be valid (ms)
        // The update loop refreshes the cache timestamp every tick (~100ms) even when
        // scoring is skipped, so 2s covers menu pauses and occasional hiccups.
        inline constexpr float EXTERNAL_EQUIP_TIME_WINDOW = 2000.0f;

        // The anti-spam interval and the per-case reward multipliers that lived
        // here were removed with the one selection path (2026-10-02): a
        // selection is one reward whatever the device, and repeats of one pick
        // merge in SelectionTracker. Their INI keys are ignored if still present.
    }

    // =========================================================================
    // LEARNING CONFIGURATION (Immutable snapshot)
    // =========================================================================
    // POD struct produced by LearningSettings::BuildConfig().
    // ExternalEquipLearner stores a copy for consistent, race-free reads.
    // =========================================================================

    struct LearningConfig
    {
        bool learnFromExternalEquips = LearningDefaults::LEARN_FROM_EXTERNAL_EQUIPS;
        float externalEquipTimeWindow = LearningDefaults::EXTERNAL_EQUIP_TIME_WINDOW;
        // Memory with a useful life (FeatureBanditLearner::SetMemoryLife);
        // the defaults live on the struct.
        MemoryLife memory{};
    };

    // =========================================================================
    // LEARNING SETTINGS
    // =========================================================================
    // Singleton that loads external equip learning parameters from
    // Data/SKSE/Plugins/Huginn.ini [Learning] section.
    // =========================================================================

    class LearningSettings
    {
    public:
        static LearningSettings& GetSingleton()
        {
            static LearningSettings instance;
            return instance;
        }

        void LoadFromFile(const std::filesystem::path& iniPath);
        void LoadFromIni(const CSimpleIniA& ini);
        void ResetToDefaults();

        /// Produce an immutable snapshot of all learning settings.
        [[nodiscard]] LearningConfig BuildConfig() const;

    private:
        LearningSettings() = default;
        ~LearningSettings() = default;
        LearningSettings(const LearningSettings&) = delete;
        LearningSettings& operator=(const LearningSettings&) = delete;

        bool learnFromExternalEquips = LearningDefaults::LEARN_FROM_EXTERNAL_EQUIPS;
        float externalEquipTimeWindow = LearningDefaults::EXTERNAL_EQUIP_TIME_WINDOW;
        MemoryLife memory{};
    };

}  // namespace Huginn::Learning
