#include "KeybindingSettings.h"
#include "IniLoad.h"
#include <SimpleIni.h>

namespace Huginn::Input
{
    void KeybindingSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        CSimpleIniA ini;
        if (LoadIniFile(ini, iniPath, "KeybindingSettings"sv)) {
            LoadFromIni(ini);
        }
    }

    // Each key falls back to the CURRENT member value, not the compile-time
    // default, so calls layer: load the main INI, then overlay the dMenu INI.
    // A key absent from the second file keeps what the first one set instead of
    // snapping back to the default. On a single call the members still hold the
    // compile-time defaults, so behaviour is unchanged.
    void KeybindingSettings::LoadFromIni(const CSimpleIniA& ini)
    {
        const char* section = "Keybindings";

        slot1Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot1Key", slot1Key));
        slot2Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot2Key", slot2Key));
        slot3Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot3Key", slot3Key));
        slot4Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot4Key", slot4Key));
        slot5Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot5Key", slot5Key));
        slot6Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot6Key", slot6Key));
        slot7Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot7Key", slot7Key));
        slot8Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot8Key", slot8Key));
        slot9Key  = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot9Key", slot9Key));
        slot10Key = static_cast<uint32_t>(ini.GetLongValue(section, "iSlot10Key", slot10Key));
        prevPageKey = static_cast<uint32_t>(ini.GetLongValue(section, "iPreviousPageKey", prevPageKey));
        nextPageKey = static_cast<uint32_t>(ini.GetLongValue(section, "iNextPageKey", nextPageKey));
        toggleKey   = static_cast<uint32_t>(ini.GetLongValue(section, "iToggleWidgetKey", toggleKey));

        logger::info("[KeybindingSettings] Loaded: Slots=[{},{},{},{},{},{},{},{},{},{}] Pages=[{},{}] Toggle={}"sv,
            slot1Key, slot2Key, slot3Key, slot4Key, slot5Key,
            slot6Key, slot7Key, slot8Key, slot9Key, slot10Key,
            prevPageKey, nextPageKey,
            toggleKey == KeybindingDefaults::TOGGLE_DISABLED ? 0 : toggleKey);
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
        toggleKey   = KeybindingDefaults::TOGGLE_KEY;

        logger::info("[KeybindingSettings] Reset to defaults"sv);
    }

}  // namespace Huginn::Input
