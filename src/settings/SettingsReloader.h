#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include <SimpleIni.h>

namespace Huginn::Settings
{
    /// Event sink for dMenu integration - handles settings hot-reload
    /// Listens for dmenu_updateSettings and dmenu_buttonCallback events
    ///
    /// Integration:
    /// - dmenu_updateSettings: Triggered when user changes any setting in dMenu UI
    /// - dmenu_buttonCallback: Triggered when user clicks action buttons
    ///
    /// Graceful degradation:
    /// - If dMenu not installed: No events fire, Huginn loads from Huginn.ini normally
    /// - If dMenu installed but no JSON: Huginn panel doesn't appear, still works from Huginn.ini
    /// - If dMenu installed with JSON: Full hot-reload via dMenu UI
    class SettingsReloader : public RE::BSTEventSink<SKSE::ModCallbackEvent>
    {
    public:
        /// Get singleton instance
        static SettingsReloader& GetSingleton();

        /// Register with SKSE ModCallbackEvent system (call at kDataLoaded)
        void Register();

        /// Check if currently registered
        [[nodiscard]] bool IsRegistered() const noexcept {
            return m_registered.load(std::memory_order_acquire);
        }

        /// Reload all settings from INI, then apply side effects.
        ///
        /// Single source of truth for a full settings reload. Both the dMenu
        /// event handler and the `hg reload` console command delegate here so
        /// they can never diverge (e.g. one path forgetting a settings class).
        ///
        /// The path argument is the dMenu-managed INI (Widget, Keybindings,
        /// Debug); non-dMenu sections always load from the main INI. Pass the
        /// main INI path for a console reload — the dMenu-managed sections then
        /// fall back to the main INI, which is the correct no-dMenu behavior.
        ///
        /// Thread Safety:
        /// - Called from the game thread (ModCallbackEvent handler, or console
        ///   command wrapped in UpdateHandler::RunExclusive)
        /// - POD settings singletons (ScorerSettings, ContextWeightSettings) read without locks
        /// - SlotSettings uses shared_mutex internally for safe concurrent access
        /// - Side effects are ordered to prevent inconsistent state
        /// - Worst case: one update frame with mixed old/new values (acceptable for tuning)
        void ReloadAllSettings(const std::filesystem::path& iniPath);

    protected:
        /// Process ModCallbackEvent - handles dmenu_updateSettings and dmenu_buttonCallback
        RE::BSEventNotifyControl ProcessEvent(
            const SKSE::ModCallbackEvent* event,
            RE::BSTEventSource<SKSE::ModCallbackEvent>* source) override;

    private:
        SettingsReloader() = default;
        ~SettingsReloader() override = default;

        SettingsReloader(const SettingsReloader&) = delete;
        SettingsReloader(SettingsReloader&&) = delete;
        SettingsReloader& operator=(const SettingsReloader&) = delete;
        SettingsReloader& operator=(SettingsReloader&&) = delete;

        /// Handle button callbacks (Reset Q-Table, Reset Defaults, Reload INI)
        void HandleButtonCallback(std::string_view buttonId);

        /// Reset all settings to compile-time defaults
        void ResetAllToDefaults();

        /// The inputs Wheeler wheel *creation* actually depends on: the wheel
        /// position and, per page, the name + slot count. Everything else Wheeler
        /// reads (subtext labels, auto-focus, post-activation) is applied per-tick
        /// and needs no rebuild. Captured before a reload and compared after to
        /// decide whether the wheels must be torn down and recreated.
        struct WheelLayout
        {
            int32_t wheelPosition = 0;
            std::vector<std::pair<std::string, size_t>> pages;
            bool operator==(const WheelLayout&) const = default;
        };

        /// Snapshot the current wheel layout (reads SlotSettings + WheelerSettings).
        [[nodiscard]] static WheelLayout CaptureWheelLayout();

        /// Apply side effects after settings have been reloaded/reset
        /// (scorer config, allocator, locker, wheels, widget).
        /// @param mainIni      When non-null, the already-parsed main INI is reused
        ///   for the wildcard + slot-locker loads (parse-once reload path). When
        ///   null (reset-to-defaults path), those two loaders parse Huginn.ini
        ///   themselves, preserving the prior behavior.
        /// @param beforeLayout Wheel layout captured before the reload/reset; the
        ///   Wheeler rebuild is skipped when the post-reload layout matches it.
        void ApplySideEffects(const CSimpleIniA* mainIni, const WheelLayout& beforeLayout);

        /// Registration state (atomic for thread safety)
        std::atomic<bool> m_registered{false};
    };
}
