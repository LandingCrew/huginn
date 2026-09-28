#pragma once

#include "SlotConfig.h"
#include <SimpleIni.h>
#include <atomic>
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
        void LoadFromIni(const CSimpleIniA& ini);

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

        /// Whether an item that is still recommended should keep the slot it
        /// was in, instead of being re-seated by rank every pass.
        ///
        /// Read from `[SlotLocker] bKeepSlotPositions`, which is a section this
        /// class otherwise has nothing to do with. It lives there because it is
        /// the same concern the rest of that section configures -- stability of
        /// what the player is looking at -- and because SlotSettings is the one
        /// thing every allocation path already consults, so it hot-reloads with
        /// the layout and needs no second wiring.
        [[nodiscard]] bool KeepSlotPositions() const noexcept
        {
            return m_keepSlotPositions.load(std::memory_order_acquire);
        }

        /// Whether an item still on screen holds its slot against newcomers
        /// until one beats it by ChallengerMargin(). Needs KeepSlotPositions:
        /// "its slot" is its seat. `[SlotLocker] bHoldSeatedItems`.
        [[nodiscard]] bool HoldSeatedItems() const noexcept
        {
            return m_holdSeatedItems.load(std::memory_order_acquire);
        }

        /// How much better, as a fraction, a challenger must score than the
        /// item holding a slot to take it: 0.25 = 25% better.
        /// `[SlotLocker] fChallengerMargin`.
        [[nodiscard]] float ChallengerMargin() const noexcept
        {
            return m_challengerMargin.load(std::memory_order_acquire);
        }

        /// How long a slot holds what pressing it took off (Remembrance.h), in
        /// ms; 0 = off everywhere. `[SlotLocker] fRemembranceDurationMs`. The
        /// per-slot switch is SlotConfig::remembrance (`bRemembrance`).
        [[nodiscard]] float RemembranceDurationMs() const noexcept
        {
            return m_remembranceDurationMs.load(std::memory_order_acquire);
        }

        /// Cap on a Remembrance hold whose item does not fit the pressed key's
        /// class (a dagger on an attack-magic key): it still shows there, but
        /// briefly. `[SlotLocker] fRemembranceMismatchDurationMs`.
        [[nodiscard]] float RemembranceMismatchDurationMs() const noexcept
        {
            return m_remembranceMismatchMs.load(std::memory_order_acquire);
        }

        /// Where a remembered item that does not fit the pressed key goes.
        /// false (`sRemembranceTarget = Pressed`, default): the pressed key,
        /// capped at RemembranceMismatchDurationMs. true (`Job`): the first
        /// empty swap-back key whose class accepts it, else the pressed key.
        [[nodiscard]] bool RemembranceToJobKey() const noexcept
        {
            return m_remembranceToJobKey.load(std::memory_order_acquire);
        }

        /// Whether a key with a class that would otherwise be BLANK takes a
        /// matching item from a Regular key (the bow moves from an "anything"
        /// key to the empty Weapon key). Off by default: the slot hold wins,
        /// and the item stays put. `[SlotLocker] bFillJobKeysFromRegular`.
        [[nodiscard]] bool FillJobKeysFromRegular() const noexcept
        {
            return m_fillJobKeysFromRegular.load(std::memory_order_acquire);
        }

        /// Monotonic generation counter — bumped on every config change
        /// (LoadFromFile / ResetToDefaults). Consumers can cache config copies
        /// and cheaply detect staleness without re-copying every access.
        [[nodiscard]] uint32_t GetGeneration() const noexcept
        {
            return m_generation.load(std::memory_order_acquire);
        }

    private:
        SlotSettings() { ResetToDefaults(); }

        mutable std::shared_mutex m_mutex;
        std::vector<PageConfig> m_pages;
        std::atomic<uint32_t> m_generation{0};
        std::atomic<bool> m_keepSlotPositions{true};
        std::atomic<bool> m_holdSeatedItems{true};
        std::atomic<float> m_challengerMargin{0.25f};
        std::atomic<float> m_remembranceDurationMs{15000.0f};
        std::atomic<float> m_remembranceMismatchMs{5000.0f};
        std::atomic<bool> m_fillJobKeysFromRegular{false};
        std::atomic<bool> m_remembranceToJobKey{false};

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
        inline constexpr size_t SLOTS_PER_PAGE = 8;

        // Default slot configurations for Page 0
        struct SlotDefault
        {
            SlotClassification classification;
            bool wildcardsEnabled;
            OverrideFilter overrideFilter;
            int8_t priority;
            bool skipEquipped;
        };

        // One job per key, the same layout the shipped INI's page 0 uses. With
        // skip-equipped on, key 1 is "the other weapon", and Remembrance makes
        // it a toggle between the two.
        inline constexpr SlotDefault PAGE0_SLOTS[] = {
            { SlotClassification::WeaponsAny,   true, OverrideFilter::HP,    7, true },  // 1 Weapon
            { SlotClassification::DamageMagic,  true, OverrideFilter::MP,    6, true },  // 2 Attack magic
            { SlotClassification::HealingAny,   true, OverrideFilter::SP,    5, true },  // 3 Heal
            { SlotClassification::DefensiveAny, true, OverrideFilter::None,  4, true },  // 4 Defend
            { SlotClassification::BuffsAny,     true, OverrideFilter::None,  3, true },  // 5 Buff
            { SlotClassification::PotionsAny,   true, OverrideFilter::None,  2, true },  // 6 Potion
            { SlotClassification::Regular,      true, OverrideFilter::Other, 1, true },  // 7 Situational (Other-only home for soul-gem/ammo/drowning)
            { SlotClassification::Regular,      true, OverrideFilter::None,  0, true },  // 8 Wildcard
        };
        static_assert(std::size(PAGE0_SLOTS) == SLOTS_PER_PAGE);
    }

}  // namespace Huginn::Slot
