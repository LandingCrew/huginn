#pragma once

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

        // Anti-spam: minimum interval between learning from the same item (sec)
        inline constexpr float EXTERNAL_EQUIP_MIN_INTERVAL = 3.0f;

        // Reward multipliers by attribution case
        // Case C: Scored high but not displayed (player knew what they needed)
        inline constexpr float HIGH_UTILITY_REWARD_MULT = 0.8f;

        // Case B: Scored medium, not displayed
        inline constexpr float MEDIUM_UTILITY_REWARD_MULT = 0.4f;

        // Case B: Scored low (player disagrees with scoring)
        inline constexpr float LOW_UTILITY_REWARD_MULT = 0.2f;

        // Case D: Displayed on a different page than current
        inline constexpr float DIFFERENT_PAGE_REWARD_MULT = 0.5f;

        // Case A: Not a candidate — player went out of their way to equip
        // something the pipeline didn't even consider. Strongest preference signal.
        inline constexpr float NOT_CANDIDATE_REWARD_MULT = 1.0f;
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
        float externalEquipMinInterval = LearningDefaults::EXTERNAL_EQUIP_MIN_INTERVAL;
        float highUtilityRewardMult = LearningDefaults::HIGH_UTILITY_REWARD_MULT;
        float mediumUtilityRewardMult = LearningDefaults::MEDIUM_UTILITY_REWARD_MULT;
        float lowUtilityRewardMult = LearningDefaults::LOW_UTILITY_REWARD_MULT;
        float differentPageRewardMult = LearningDefaults::DIFFERENT_PAGE_REWARD_MULT;
        float notCandidateRewardMult = LearningDefaults::NOT_CANDIDATE_REWARD_MULT;
    };

    inline constexpr LearningConfig DefaultLearningConfig{};

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
        void ResetToDefaults();

        /// Produce an immutable snapshot of all learning settings.
        [[nodiscard]] LearningConfig BuildConfig() const;

        // Accessors
        [[nodiscard]] bool IsExternalEquipLearningEnabled() const noexcept { return learnFromExternalEquips; }
        [[nodiscard]] float GetExternalEquipTimeWindow() const noexcept { return externalEquipTimeWindow; }
        [[nodiscard]] float GetExternalEquipMinInterval() const noexcept { return externalEquipMinInterval; }
        [[nodiscard]] float GetHighUtilityRewardMult() const noexcept { return highUtilityRewardMult; }
        [[nodiscard]] float GetMediumUtilityRewardMult() const noexcept { return mediumUtilityRewardMult; }
        [[nodiscard]] float GetLowUtilityRewardMult() const noexcept { return lowUtilityRewardMult; }
        [[nodiscard]] float GetDifferentPageRewardMult() const noexcept { return differentPageRewardMult; }
        [[nodiscard]] float GetNotCandidateRewardMult() const noexcept { return notCandidateRewardMult; }

    private:
        LearningSettings() = default;
        ~LearningSettings() = default;
        LearningSettings(const LearningSettings&) = delete;
        LearningSettings& operator=(const LearningSettings&) = delete;

        bool learnFromExternalEquips = LearningDefaults::LEARN_FROM_EXTERNAL_EQUIPS;
        float externalEquipTimeWindow = LearningDefaults::EXTERNAL_EQUIP_TIME_WINDOW;
        float externalEquipMinInterval = LearningDefaults::EXTERNAL_EQUIP_MIN_INTERVAL;
        float highUtilityRewardMult = LearningDefaults::HIGH_UTILITY_REWARD_MULT;
        float mediumUtilityRewardMult = LearningDefaults::MEDIUM_UTILITY_REWARD_MULT;
        float lowUtilityRewardMult = LearningDefaults::LOW_UTILITY_REWARD_MULT;
        float differentPageRewardMult = LearningDefaults::DIFFERENT_PAGE_REWARD_MULT;
        float notCandidateRewardMult = LearningDefaults::NOT_CANDIDATE_REWARD_MULT;
    };

}  // namespace Huginn::Learning
