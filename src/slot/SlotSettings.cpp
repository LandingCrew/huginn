#include "SlotSettings.h"
#include "IniLoad.h"
#include <SimpleIni.h>
#include <algorithm>
#include <array>
#include <numeric>
#include <set>
#include "SlotClassifier.h"
#include "candidate/CandidateTypes.h"

namespace Huginn::Slot
{
    // Tripwire: a new SlotClassification needs a parse alias + ToIniString case
    // below (ParseClassification / ClassificationToIniString), else it can't be
    // configured from INI and round-trips to Regular.
    static_assert(SLOT_CLASSIFICATION_COUNT == 24,
        "SlotClassification changed — update ParseClassification and ClassificationToIniString");

    void SlotSettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        CSimpleIniA ini;
        if (LoadIniFile(ini, iniPath, "SlotSettings"sv)) {
            SKSE::log::info("[SlotSettings] Loading from: {}"sv, iniPath.string());
            LoadFromIni(ini);
        }
    }

    void SlotSettings::LoadFromIni(const CSimpleIniA& ini)
    {
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
            // Page 0 is the flagship "Huginn" page (Defaults::PAGE0_SLOTS).
            page.name = ini.GetValue(pageSection.c_str(), "sName",
                p == 0 ? "Huginn" : std::format("Page {}", p + 1).c_str());

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
                    slot.skipEquipped = true;
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

                slot.remembrance = ini.GetBoolValue(slotSection.c_str(), "bRemembrance",
                    slot.remembrance);

                page.slots.push_back(slot);
            }

            // The slot the critical-health override lands in is an emergency
            // key: it takes no wildcards, whatever the INI says. Between
            // emergencies the shipped key 1 was handed an Axe, a Dagger and a
            // Sword as exploration picks (2026-09-27 20:40:48, 20:41:13,
            // 20:43:51). Found the way SlotAllocator places the override, so
            // the protected slot is the one it actually lands in
            // (/code-review #147): priority order, first slot that accepts a
            // health override AND whose classification takes the item (a
            // potion, or -- the spell fallback -- a restore spell), else the
            // first that accepts it at all. Both items are probed. Decided
            // here so every reader (fill, refill, hold, the wildcard-capable
            // count) agrees.
            {
                auto& slots = page.slots;
                std::vector<size_t> order(slots.size());
                std::iota(order.begin(), order.end(), size_t{ 0 });
                std::stable_sort(order.begin(), order.end(), [&slots](size_t a, size_t b) {
                    return slots[a].priority > slots[b].priority;
                });
                const auto acceptsHealth = [](OverrideFilter f) {
                    return f == OverrideFilter::HP || f == OverrideFilter::Any;
                };

                Candidate::ItemCandidate potion{};
                potion.type = Item::ItemType::HealthPotion;
                potion.tags = Item::ItemTag::RestoreHealth;
                Candidate::SpellCandidate spell{};
                spell.type = Spell::SpellType::Healing;
                spell.tags = Spell::SpellTag::RestoreHealth;
                const std::array<Candidate::CandidateVariant, 2> probes{ potion, spell };

                std::set<size_t> landing;
                for (const auto& probe : probes) {
                    size_t firstAccepting = SIZE_MAX;
                    size_t firstMatching = SIZE_MAX;
                    for (const size_t s : order) {
                        if (!acceptsHealth(slots[s].overrideFilter)) continue;
                        if (firstAccepting == SIZE_MAX) firstAccepting = s;
                        if (SlotClassifier::Matches(probe, slots[s].classification)) {
                            firstMatching = s;
                            break;
                        }
                    }
                    const size_t lands = firstMatching != SIZE_MAX ? firstMatching : firstAccepting;
                    if (lands != SIZE_MAX) landing.insert(lands);
                }
                for (const size_t s : landing) {
                    if (slots[s].wildcardsEnabled) {
                        slots[s].wildcardsEnabled = false;
                        SKSE::log::info("[SlotSettings] Page{}.Slot{} hosts health overrides: wildcards off"sv, p, s);
                    }
                }
            }

            newPages.push_back(std::move(page));

            // Log page summary. The `w` flag and the wildcard-capable count are
            // not cosmetic: WildcardManager caps a page's rolls at the number of
            // slots accepting wildcards (SlotAllocator::GetWildcardSlotCount ->
            // PipelineContext::displayWildcardSlots), so that count is the only
            // thing separating "this page rolled nothing because it is unlucky"
            // from "because it has no seat for a wildcard" — and without it here
            // neither is distinguishable from a log.
            std::string slotSummary;
            size_t wildcardCapable = 0;
            for (size_t s = 0; s < newPages.back().slots.size(); ++s) {
                const auto& slot = newPages.back().slots[s];
                if (slot.wildcardsEnabled) {
                    ++wildcardCapable;
                }
                if (s > 0) slotSummary += "|";
                // `:r0` only when remembrance is off: it is on by default,
                // and the common line stays as it was.
                slotSummary += std::format("{}:p{}:o{}:w{}{}",
                    SlotClassificationToString(slot.classification),
                    slot.priority,
                    OverrideFilterToString(slot.overrideFilter),
                    slot.wildcardsEnabled ? 1 : 0,
                    slot.remembrance ? "" : ":r0");
            }
            SKSE::log::info("[SlotSettings] Page {} '{}': {} slots, {} wildcard-capable [{}]"sv,
                p, newPages.back().name, newPages.back().slots.size(),
                wildcardCapable, slotSummary);
        }

        // Slot-position stickiness. Read here rather than in the SlotLocker
        // loader (see KeepSlotPositions) so every allocation path picks it up
        // from the config snapshot it already takes.
        const bool keepPositions = ini.GetBoolValue("SlotLocker", "bKeepSlotPositions", true);
        m_keepSlotPositions.store(keepPositions, std::memory_order_release);
        SKSE::log::info("[SlotSettings] Keep slot positions: {}"sv, keepPositions ? "on" : "off");

        const bool hold = ini.GetBoolValue("SlotLocker", "bHoldSeatedItems", true);
        const float margin = std::clamp(
            static_cast<float>(ini.GetDoubleValue("SlotLocker", "fChallengerMargin", 0.25)), 0.0f, 10.0f);
        m_holdSeatedItems.store(hold, std::memory_order_release);
        m_challengerMargin.store(margin, std::memory_order_release);
        SKSE::log::info("[SlotSettings] Hold seated items: {} (challenger margin {:.0f}%)"sv,
            hold ? "on" : "off", margin * 100.0f);

        const float needDiscount = std::clamp(
            static_cast<float>(ini.GetDoubleValue("SlotLocker", "fNeedRepeatDiscount", 0.5)), 0.0f, 1.0f);
        const auto needFree = static_cast<uint32_t>(std::clamp(
            ini.GetLongValue("SlotLocker", "iNeedFreeSlots", 3), 1L, static_cast<long>(MAX_SLOTS_PER_PAGE)));
        m_needRepeatDiscount.store(needDiscount, std::memory_order_release);
        m_needFreeSlots.store(needFree, std::memory_order_release);
        if (needDiscount < 1.0f) {
            SKSE::log::info("[SlotSettings] Need cap: on Regular keys, items of one need past the first {} at x{:.2f} each"sv,
                needFree, needDiscount);
        } else {
            SKSE::log::info("[SlotSettings] Need cap: off"sv);
        }

        const float remembranceMs = std::max(0.0f,
            static_cast<float>(ini.GetDoubleValue("SlotLocker", "fRemembranceDurationMs", 15000.0)));
        m_remembranceDurationMs.store(remembranceMs, std::memory_order_release);
        const float mismatchMs = std::max(0.0f,
            static_cast<float>(ini.GetDoubleValue("SlotLocker", "fRemembranceMismatchDurationMs", 5000.0)));
        m_remembranceMismatchMs.store(mismatchMs, std::memory_order_release);
        SKSE::log::info("[SlotSettings] Remembrance: {} ({:.0f}s when it does not fit the key's class)"sv,
            remembranceMs > 0.0f ? std::format("{:.0f}s", remembranceMs / 1000.0f) : std::string("off"),
            mismatchMs / 1000.0f);

        {
            std::string target = ini.GetValue("SlotLocker", "sRemembranceTarget", "Pressed");
            std::transform(target.begin(), target.end(), target.begin(), ::tolower);
            const bool toJob = target == "job";
            if (!toJob && target != "pressed") {
                SKSE::log::warn("[SlotSettings] Unknown sRemembranceTarget '{}', using Pressed"sv, target);
            }
            m_remembranceToJobKey.store(toJob, std::memory_order_release);
            SKSE::log::info("[SlotSettings] Remembrance target: {}"sv,
                toJob ? "the key whose class fits (Job)" : "the key you pressed (Pressed)");
        }

        const bool pullFromRegular = ini.GetBoolValue("SlotLocker", "bFillJobKeysFromRegular", false);
        m_fillJobKeysFromRegular.store(pullFromRegular, std::memory_order_release);
        SKSE::log::info("[SlotSettings] Fill empty job keys from Regular keys: {}"sv,
            pullFromRegular ? "on" : "off");

        // Parsing succeeded - commit the new configuration under exclusive lock
        size_t committedCount;
        {
            std::unique_lock lock(m_mutex);
            m_pages = std::move(newPages);
            committedCount = m_pages.size();
        }
        m_generation.fetch_add(1, std::memory_order_release);
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
        m_keepSlotPositions.store(true, std::memory_order_release);
        m_holdSeatedItems.store(true, std::memory_order_release);
        m_challengerMargin.store(0.25f, std::memory_order_release);
        m_needRepeatDiscount.store(0.5f, std::memory_order_release);
        m_needFreeSlots.store(3, std::memory_order_release);
        m_generation.fetch_add(1, std::memory_order_release);
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
        page.name = pageIndex == 0 ? std::string("Huginn") : std::format("Page {}", pageIndex + 1);

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
                    .skipEquipped = true,
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
        if (lower == "damagemagic" || lower == "magicdamage" || lower == "attackmagic") return SlotClassification::DamageMagic;
        if (lower == "poisonsany" || lower == "poisons" || lower == "poison") return SlotClassification::PoisonsAny;
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
        if (lower == "alcoholany" || lower == "alcohol" || lower == "drinks") return SlotClassification::AlcoholAny;
        if (lower == "ammoany" || lower == "ammo" || lower == "ammunition") return SlotClassification::AmmoAny;
        if (lower == "apparelany" || lower == "apparel" || lower == "armor") return SlotClassification::ApparelAny;
        if (lower == "regular" || lower == "any" || lower == "all") return SlotClassification::Regular;

        // Parse error - log and default to Regular (open slot)
        SKSE::log::warn("[SlotSettings] Unknown classification '{}', defaulting to Regular"sv, str);
        return SlotClassification::Regular;
    }

    const char* SlotSettings::ClassificationToIniString(SlotClassification c)
    {
        switch (c) {
            case SlotClassification::DamageAny:   return "DamageAny";
            case SlotClassification::DamageMagic: return "DamageMagic";
            case SlotClassification::PoisonsAny:  return "PoisonsAny";
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
            case SlotClassification::AlcoholAny:   return "AlcoholAny";
            case SlotClassification::AmmoAny:      return "AmmoAny";
            case SlotClassification::ApparelAny:   return "ApparelAny";
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
        if (lower == "other" || lower == "misc") return OverrideFilter::Other;

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
            case OverrideFilter::Other: return "Other";
            default:                   return "true";
        }
    }

}  // namespace Huginn::Slot
