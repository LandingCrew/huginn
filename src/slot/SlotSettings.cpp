#include "SlotSettings.h"
#include <SimpleIni.h>
#include <algorithm>

namespace Huginn::Slot
{
    void SlotSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        if (!std::filesystem::exists(iniPath)) {
            SKSE::log::info("[SlotSettings] INI not found, using defaults: {}"sv, iniPath.string());
            return;
        }

        CSimpleIniA ini;
        ini.SetUnicode();
        SI_Error rc = ini.LoadFile(iniPath.string().c_str());

        if (rc < 0) {
            SKSE::log::error("[SlotSettings] Failed to load INI: {}"sv, iniPath.string());
            return;
        }

        SKSE::log::info("[SlotSettings] Loading from: {}"sv, iniPath.string());

        // Read page count
        size_t pageCount = static_cast<size_t>(ini.GetLongValue("Pages", "iPageCount", Defaults::PAGE_COUNT));
        pageCount = std::clamp(pageCount, size_t(1), MAX_PAGES);

        // Build configuration in a temporary vector for exception safety
        // This ensures m_pages remains valid if parsing fails partway through
        std::vector<PageConfig> newPages;
        newPages.reserve(pageCount);

        // Read each page configuration
        for (size_t p = 0; p < pageCount; ++p) {
            std::string pageSection = std::format("Page{}", p);
            PageConfig page;

            // Page name
            page.name = ini.GetValue(pageSection.c_str(), "sName", std::format("Page {}", p + 1).c_str());

            // Slot count for this page
            size_t slotCount = static_cast<size_t>(ini.GetLongValue(pageSection.c_str(), "iSlotCount",
                (p == 0) ? Defaults::SLOTS_PER_PAGE : 3));
            slotCount = std::clamp(slotCount, size_t(1), MAX_SLOTS_PER_PAGE);

            page.slots.reserve(slotCount);

            // Read each slot in this page
            for (size_t s = 0; s < slotCount; ++s) {
                std::string slotSection = std::format("Page{}.Slot{}", p, s);
                SlotConfig slot;

                // Get defaults for this slot
                if (p == 0 && s < std::size(Defaults::PAGE0_SLOTS)) {
                    const auto& def = Defaults::PAGE0_SLOTS[s];
                    slot.classification = def.classification;
                    slot.wildcardsEnabled = def.wildcardsEnabled;
                    slot.overrideFilter = def.overrideFilter;
                    slot.priority = def.priority;
                    slot.skipEquipped = def.skipEquipped;
                } else {
                    // Default for additional slots: Regular, descending priority
                    slot.classification = SlotClassification::Regular;
                    slot.wildcardsEnabled = true;
                    slot.overrideFilter = OverrideFilter::None;
                    slot.priority = static_cast<int8_t>(slotCount - s - 1);
                    slot.skipEquipped = false;
                }

                // Read from INI (section may not exist - uses defaults)
                const char* classStr = ini.GetValue(slotSection.c_str(), "sClassification",
                    ClassificationToIniString(slot.classification));
                slot.classification = ParseClassification(classStr);

                slot.wildcardsEnabled = ini.GetBoolValue(slotSection.c_str(), "bWildcardsEnabled",
                    slot.wildcardsEnabled);

                const char* overrideStr = ini.GetValue(slotSection.c_str(), "bOverridesEnabled",
                    OverrideFilterToIniString(slot.overrideFilter));
                slot.overrideFilter = ParseOverrideFilter(overrideStr);

                slot.priority = static_cast<int8_t>(ini.GetLongValue(slotSection.c_str(), "iPriority",
                    slot.priority));

                slot.skipEquipped = ini.GetBoolValue(slotSection.c_str(), "bSkipEquipped",
                    slot.skipEquipped);

                page.slots.push_back(slot);
            }

            newPages.push_back(std::move(page));

            // Log page summary
            std::string slotSummary;
            for (size_t s = 0; s < newPages.back().slots.size(); ++s) {
                if (s > 0) slotSummary += "|";
                slotSummary += std::format("{}:p{}:o{}",
                    SlotClassificationToString(newPages.back().slots[s].classification),
                    newPages.back().slots[s].priority,
                    OverrideFilterToString(newPages.back().slots[s].overrideFilter));
            }
            SKSE::log::info("[SlotSettings] Page {} '{}': {} slots [{}]"sv,
                p, newPages.back().name, newPages.back().slots.size(), slotSummary);
        }

        // Parsing succeeded - commit the new configuration under exclusive lock
        size_t committedCount;
        {
            std::unique_lock lock(m_mutex);
            m_pages = std::move(newPages);
            committedCount = m_pages.size();
        }
        SKSE::log::info("[SlotSettings] Loaded {} page(s)"sv, committedCount);
    }

    void SlotSettings::ResetToDefaults()
    {
        size_t slotCount;
        {
            std::unique_lock lock(m_mutex);
            m_pages.clear();
            m_pages.push_back(CreateDefaultPage(0));
            slotCount = m_pages[0].slots.size();
        }
        SKSE::log::info("[SlotSettings] Reset to defaults (1 page, {} slots)"sv, slotCount);
    }

    size_t SlotSettings::GetPageCount() const
    {
        std::shared_lock lock(m_mutex);
        return m_pages.size();
    }

    PageConfig SlotSettings::GetPage(size_t pageIndex) const
    {
        std::shared_lock lock(m_mutex);
        if (pageIndex >= m_pages.size()) {
            SKSE::log::warn("[SlotSettings] Page {} out of range (max {})"sv,
                pageIndex, m_pages.size() - 1);
            return m_pages.empty() ? PageConfig{} : m_pages[0];
        }
        return m_pages[pageIndex];
    }

    std::vector<SlotConfig> SlotSettings::GetSlotConfigs(size_t pageIndex) const
    {
        std::shared_lock lock(m_mutex);
        if (pageIndex >= m_pages.size()) {
            return m_pages.empty() ? std::vector<SlotConfig>{} : m_pages[0].slots;
        }
        return m_pages[pageIndex].slots;
    }

    std::string SlotSettings::GetPageName(size_t pageIndex) const
    {
        std::shared_lock lock(m_mutex);
        if (pageIndex >= m_pages.size()) {
            return "Page";
        }
        return m_pages[pageIndex].name;
    }

    std::vector<PageConfig> SlotSettings::GetAllPages() const
    {
        std::shared_lock lock(m_mutex);
        return m_pages;
    }

    PageConfig SlotSettings::CreateDefaultPage(size_t pageIndex)
    {
        PageConfig page;
        page.name = std::format("Page {}", pageIndex + 1);

        if (pageIndex == 0) {
            // First page uses full defaults
            page.slots.reserve(Defaults::SLOTS_PER_PAGE);
            for (size_t s = 0; s < Defaults::SLOTS_PER_PAGE; ++s) {
                const auto& def = Defaults::PAGE0_SLOTS[s];
                page.slots.push_back({
                    .classification = def.classification,
                    .wildcardsEnabled = def.wildcardsEnabled,
                    .overrideFilter = def.overrideFilter,
                    .skipEquipped = def.skipEquipped,
                    .priority = def.priority
                });
            }
        } else {
            // Additional pages get 3 Regular slots
            page.slots.reserve(3);
            for (size_t s = 0; s < 3; ++s) {
                page.slots.push_back({
                    .classification = SlotClassification::Regular,
                    .wildcardsEnabled = true,
                    .overrideFilter = OverrideFilter::None,
                    .skipEquipped = false,
                    .priority = static_cast<int8_t>(2 - s)
                });
            }
        }

        return page;
    }

    SlotClassification SlotSettings::ParseClassification(const std::string& str)
    {
        // Case-insensitive comparison
        std::string lower = str;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "damageany" || lower == "damage") return SlotClassification::DamageAny;
        if (lower == "healingany" || lower == "healing") return SlotClassification::HealingAny;
        if (lower == "buffsany" || lower == "buffs" || lower == "buff") return SlotClassification::BuffsAny;
        if (lower == "defensiveany" || lower == "defensive") return SlotClassification::DefensiveAny;
        if (lower == "summonsany" || lower == "summons" || lower == "summon") return SlotClassification::SummonsAny;
        if (lower == "utility") return SlotClassification::Utility;
        if (lower == "potionsany" || lower == "potions" || lower == "potion") return SlotClassification::PotionsAny;
        if (lower == "scrollsany" || lower == "scrolls" || lower == "scroll") return SlotClassification::ScrollsAny;
        if (lower == "spellsany" || lower == "spells" || lower == "spell") return SlotClassification::SpellsAny;
        if (lower == "spellsdestruction" || lower == "destruction") return SlotClassification::SpellsDestruction;
        if (lower == "spellsrestoration" || lower == "restoration") return SlotClassification::SpellsRestoration;
        if (lower == "spellsconjuration" || lower == "conjuration") return SlotClassification::SpellsConjuration;
        if (lower == "spellsillusion" || lower == "illusion") return SlotClassification::SpellsIllusion;
        if (lower == "spellsalteration" || lower == "alteration") return SlotClassification::SpellsAlteration;
        if (lower == "weaponsany" || lower == "weapons" || lower == "weapon") return SlotClassification::WeaponsAny;
        if (lower == "weaponsmelee" || lower == "melee") return SlotClassification::WeaponsMelee;
        if (lower == "weaponsranged" || lower == "ranged") return SlotClassification::WeaponsRanged;
        if (lower == "foodany" || lower == "food") return SlotClassification::FoodAny;
        if (lower == "ammoany" || lower == "ammo" || lower == "ammunition") return SlotClassification::AmmoAny;
        if (lower == "regular" || lower == "any" || lower == "all") return SlotClassification::Regular;

        // Parse error - log and default to Regular (open slot)
        SKSE::log::warn("[SlotSettings] Unknown classification '{}', defaulting to Regular"sv, str);
        return SlotClassification::Regular;
    }

    const char* SlotSettings::ClassificationToIniString(SlotClassification c)
    {
        switch (c) {
            case SlotClassification::DamageAny:   return "DamageAny";
            case SlotClassification::HealingAny:  return "HealingAny";
            case SlotClassification::BuffsAny:    return "BuffsAny";
            case SlotClassification::DefensiveAny: return "DefensiveAny";
            case SlotClassification::SummonsAny:  return "SummonsAny";
            case SlotClassification::Utility:     return "Utility";
            case SlotClassification::PotionsAny:  return "PotionsAny";
            case SlotClassification::ScrollsAny:  return "ScrollsAny";
            case SlotClassification::SpellsAny:   return "SpellsAny";
            case SlotClassification::SpellsDestruction: return "SpellsDestruction";
            case SlotClassification::SpellsRestoration: return "SpellsRestoration";
            case SlotClassification::SpellsConjuration: return "SpellsConjuration";
            case SlotClassification::SpellsIllusion:    return "SpellsIllusion";
            case SlotClassification::SpellsAlteration:  return "SpellsAlteration";
            case SlotClassification::WeaponsAny:   return "WeaponsAny";
            case SlotClassification::WeaponsMelee: return "WeaponsMelee";
            case SlotClassification::WeaponsRanged: return "WeaponsRanged";
            case SlotClassification::FoodAny:      return "FoodAny";
            case SlotClassification::AmmoAny:      return "AmmoAny";
            case SlotClassification::Regular:      return "Regular";
            default:                              return "Regular";
        }
    }

    OverrideFilter SlotSettings::ParseOverrideFilter(const std::string& str)
    {
        std::string lower = str;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "true" || lower == "any" || lower == "all") return OverrideFilter::Any;
        if (lower == "false" || lower == "none" || lower == "off") return OverrideFilter::None;
        if (lower == "hp" || lower == "health") return OverrideFilter::HP;
        if (lower == "mp" || lower == "magicka" || lower == "mana") return OverrideFilter::MP;
        if (lower == "sp" || lower == "stamina") return OverrideFilter::SP;

        SKSE::log::warn("[SlotSettings] Unknown override filter '{}', defaulting to Any"sv, str);
        return OverrideFilter::Any;
    }

    const char* SlotSettings::OverrideFilterToIniString(OverrideFilter f)
    {
        switch (f) {
            case OverrideFilter::None: return "false";
            case OverrideFilter::Any:  return "true";
            case OverrideFilter::HP:   return "HP";
            case OverrideFilter::MP:   return "MP";
            case OverrideFilter::SP:   return "SP";
            default:                   return "true";
        }
    }

}  // namespace Huginn::Slot
