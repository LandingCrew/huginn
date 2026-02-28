#include "KeybindingSettings.h"
#include <SimpleIni.h>

namespace Huginn::Input
{
    void KeybindingSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        if (!std::filesystem::exists(iniPath)) {
            logger::info("[KeybindingSettings] INI file not found, using defaults: {}"sv, iniPath.string());
            return;
        }

        CSimpleIniA ini;
        ini.SetUnicode();
        SI_Error rc = ini.LoadFile(iniPath.string().c_str());

        if (rc < 0) {
            logger::error("[KeybindingSettings] Failed to load INI file: {}"sv, iniPath.string());
            return;
        }

        const char* section = "Keybindings";

        slot1Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot1Key", KeybindingDefaults::SLOT1_KEY));
        slot2Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot2Key", KeybindingDefaults::SLOT2_KEY));
        slot3Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot3Key", KeybindingDefaults::SLOT3_KEY));
        slot4Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot4Key", KeybindingDefaults::SLOT4_KEY));
        slot5Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot5Key", KeybindingDefaults::SLOT5_KEY));
        slot6Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot6Key", KeybindingDefaults::SLOT6_KEY));
        slot7Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot7Key", KeybindingDefaults::SLOT7_KEY));
        slot8Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot8Key", KeybindingDefaults::SLOT8_KEY));
        slot9Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot9Key", KeybindingDefaults::SLOT9_KEY));
        slot10Key = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot10Key", KeybindingDefaults::SLOT10_KEY));
        prevPageKey = static_cast<uint32_t>(ini.GetLongValue(section, "iPreviousPageKey", KeybindingDefaults::PREV_PAGE_KEY));
        nextPageKey = static_cast<uint32_t>(ini.GetLongValue(section, "iNextPageKey", KeybindingDefaults::NEXT_PAGE_KEY));

        logger::info("[KeybindingSettings] Loaded: Slots=[{},{},{},{},{},{},{},{},{},{}] Pages=[{},{}]"sv,
            slot1Key, slot2Key, slot3Key, slot4Key, slot5Key,
            slot6Key, slot7Key, slot8Key, slot9Key, slot10Key,
            prevPageKey, nextPageKey);
    }

    void KeybindingSettings::ResetToDefaults()
    {
        slot1Key  = KeybindingDefaults::SLOT1_KEY;
        slot2Key  = KeybindingDefaults::SLOT2_KEY;
        slot3Key  = KeybindingDefaults::SLOT3_KEY;
        slot4Key  = KeybindingDefaults::SLOT4_KEY;
        slot5Key  = KeybindingDefaults::SLOT5_KEY;
        slot6Key  = KeybindingDefaults::SLOT6_KEY;
        slot7Key  = KeybindingDefaults::SLOT7_KEY;
        slot8Key  = KeybindingDefaults::SLOT8_KEY;
        slot9Key  = KeybindingDefaults::SLOT9_KEY;
        slot10Key = KeybindingDefaults::SLOT10_KEY;
        prevPageKey = KeybindingDefaults::PREV_PAGE_KEY;
        nextPageKey = KeybindingDefaults::NEXT_PAGE_KEY;

        logger::info("[KeybindingSettings] Reset to defaults"sv);
    }

}  // namespace Huginn::Input
