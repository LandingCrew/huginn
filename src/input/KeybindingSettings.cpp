#include "KeybindingSettings.h"
#include "IniLoad.h"
#include <SimpleIni.h>

namespace Huginn::Input
{
    // Keybindings live in ONE file: the main Huginn.ini (dMenu no longer
    // declares them), parsed over the shipped Huginn.ini (Settings::LoadMainIni),
    // so a key the player's file leaves out reads the shipped value; the
    // constants below apply only to a key in neither file.
    void KeybindingSettings::LoadFromIni(const CSimpleIniA& ini)
    {
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
        resetKey   = static_cast<uint32_t>(ini.GetLongValue(section, "iResetWidgetKey", KeybindingDefaults::RESET_KEY));

        logger::info("[KeybindingSettings] Loaded: Slots=[{},{},{},{},{},{},{},{},{},{}] Pages=[{},{}] Reset={}"sv,
            slot1Key, slot2Key, slot3Key, slot4Key, slot5Key,
            slot6Key, slot7Key, slot8Key, slot9Key, slot10Key,
            prevPageKey, nextPageKey,
            resetKey == KeybindingDefaults::RESET_DISABLED ? 0 : resetKey);
    }

}  // namespace Huginn::Input
