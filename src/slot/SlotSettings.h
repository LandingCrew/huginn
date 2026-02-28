#pragma once

#include "SlotConfig.h"
#include <filesystem>
#include <shared_mutex>
#include <vector>
#include <string>

namespace Huginn::Slot
{
    // =============================================================================
    // CONSTANTS
    // =============================================================================

    inline constexpr size_t MAX_PAGES = 10;
    inline constexpr size_t MAX_SLOTS_PER_PAGE = 10;

    // =============================================================================
    // PAGE CONFIGURATION
    // =============================================================================

    struct PageConfig
    {
        std::string name = "Page";
        std::vector<SlotConfig> slots;
    };

    // =============================================================================
    // SLOT SETTINGS - INI-based multi-page configuration
    // =============================================================================
    // Loads slot layout from Huginn.ini [Pages], [PageN], [PageN.SlotM] sections.
    // Falls back to defaults if INI is missing or invalid.
    //
    // Example INI:
    //   [Pages]
    //   iPageCount=2
    //
    //   [Page0]
    //   sName=Combat
    //   iSlotCount=3
    //
    //   [Page0.Slot0]
    //   sClassification=DamageAny
    //   bWildcardsEnabled=true
    //   bOverridesEnabled=true
    //   iPriority=2
    // =============================================================================

    class SlotSettings
    {
    public:
        static SlotSettings& GetSingleton()
        {
            static SlotSettings instance;
            return instance;
        }

        /// Load settings from INI file
        void LoadFromFile(const std::filesystem::path& iniPath);

        /// Reset to default configuration
        void ResetToDefaults();

        /// Get page count
        [[nodiscard]] size_t GetPageCount() const;

        /// Get a specific page configuration (returns copy for thread safety)
        [[nodiscard]] PageConfig GetPage(size_t pageIndex) const;

        /// Get slot configs for a specific page (returns copy for thread safety)
        [[nodiscard]] std::vector<SlotConfig> GetSlotConfigs(size_t pageIndex) const;

        /// Get page name (returns copy for thread safety)
        [[nodiscard]] std::string GetPageName(size_t pageIndex) const;

        /// Get all pages (returns copy for thread safety)
        [[nodiscard]] std::vector<PageConfig> GetAllPages() const;

    private:
        SlotSettings() { ResetToDefaults(); }

        mutable std::shared_mutex m_mutex;
        std::vector<PageConfig> m_pages;

        /// Parse classification string to enum (logs warning on error, returns Regular)
        [[nodiscard]] static SlotClassification ParseClassification(const std::string& str);

        /// Classification enum to string (for INI writing/logging)
        [[nodiscard]] static const char* ClassificationToIniString(SlotClassification c);

        /// Parse override filter string (supports true/false for backward compat + HP/MP/SP)
        [[nodiscard]] static OverrideFilter ParseOverrideFilter(const std::string& str);

        /// Override filter to INI string (true/false for backward compat)
        [[nodiscard]] static const char* OverrideFilterToIniString(OverrideFilter f);

        /// Create default page configuration
        [[nodiscard]] static PageConfig CreateDefaultPage(size_t pageIndex);
    };

    // =============================================================================
    // DEFAULT CONFIGURATION
    // =============================================================================

    namespace Defaults
    {
        inline constexpr size_t PAGE_COUNT = 1;
        inline constexpr size_t SLOTS_PER_PAGE = 7;

        // Default slot configurations for Page 0
        struct SlotDefault
        {
            SlotClassification classification;
            bool wildcardsEnabled;
            OverrideFilter overrideFilter;
            int8_t priority;
            bool skipEquipped;
        };

        inline constexpr SlotDefault PAGE0_SLOTS[] = {
            { SlotClassification::DamageAny,  true, OverrideFilter::Any,  6, false },  // Slot 0
            { SlotClassification::WeaponsAny, true, OverrideFilter::Any,  5, false },  // Slot 1
            { SlotClassification::BuffsAny,   true, OverrideFilter::Any,  4, false },  // Slot 2
            { SlotClassification::Regular,    true, OverrideFilter::None, 3, false },  // Slot 3
            { SlotClassification::Regular,    true, OverrideFilter::None, 2, false },  // Slot 4
            { SlotClassification::Regular,    true, OverrideFilter::None, 1, false },  // Slot 5
            { SlotClassification::Regular,    true, OverrideFilter::None, 0, false },  // Slot 6
        };
    }

}  // namespace Huginn::Slot
