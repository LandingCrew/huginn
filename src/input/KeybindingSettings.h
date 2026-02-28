#pragma once

#include <filesystem>
#include <cstdint>

namespace Huginn::Input
{
    /**
     * @brief Default keybinding values
     * DirectInput scancodes for keyboard keys
     */
    namespace KeybindingDefaults
    {
        constexpr uint32_t SLOT1_KEY  = 2;   // 1 key
        constexpr uint32_t SLOT2_KEY  = 3;   // 2 key
        constexpr uint32_t SLOT3_KEY  = 4;   // 3 key
        constexpr uint32_t SLOT4_KEY  = 5;   // 4 key
        constexpr uint32_t SLOT5_KEY  = 6;   // 5 key
        constexpr uint32_t SLOT6_KEY  = 7;   // 6 key
        constexpr uint32_t SLOT7_KEY  = 8;   // 7 key
        constexpr uint32_t SLOT8_KEY  = 9;   // 8 key
        constexpr uint32_t SLOT9_KEY  = 10;  // 9 key
        constexpr uint32_t SLOT10_KEY = 11;  // 0 key
        constexpr uint32_t PREV_PAGE_KEY = 12;  // - key
        constexpr uint32_t NEXT_PAGE_KEY = 13;  // = key
    }

    /**
     * @brief Keybinding configuration loaded from INI
     */
    struct KeybindingSettings
    {
        uint32_t slot1Key  = KeybindingDefaults::SLOT1_KEY;
        uint32_t slot2Key  = KeybindingDefaults::SLOT2_KEY;
        uint32_t slot3Key  = KeybindingDefaults::SLOT3_KEY;
        uint32_t slot4Key  = KeybindingDefaults::SLOT4_KEY;
        uint32_t slot5Key  = KeybindingDefaults::SLOT5_KEY;
        uint32_t slot6Key  = KeybindingDefaults::SLOT6_KEY;
        uint32_t slot7Key  = KeybindingDefaults::SLOT7_KEY;
        uint32_t slot8Key  = KeybindingDefaults::SLOT8_KEY;
        uint32_t slot9Key  = KeybindingDefaults::SLOT9_KEY;
        uint32_t slot10Key = KeybindingDefaults::SLOT10_KEY;
        uint32_t prevPageKey = KeybindingDefaults::PREV_PAGE_KEY;
        uint32_t nextPageKey = KeybindingDefaults::NEXT_PAGE_KEY;

        /**
         * @brief Load settings from INI file
         * @param iniPath Path to Huginn.ini
         */
        void LoadFromFile(const std::filesystem::path& iniPath);

        /**
         * @brief Reset all settings to compile-time defaults
         */
        void ResetToDefaults();
    };
}
