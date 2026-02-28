#pragma once

#include <atomic>
#include <filesystem>

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

        /// Unregister from event system
        void Unregister();

        /// Check if currently registered
        [[nodiscard]] bool IsRegistered() const noexcept {
            return m_registered.load(std::memory_order_acquire);
        }

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

        /// Reload all settings from INI file (reuses ConsoleCommands.cpp:263-339 pattern)
        ///
        /// Thread Safety:
        /// - Called from game thread (ModCallbackEvent handler)
        /// - POD settings singletons (ScorerSettings, ContextWeightSettings) read without locks
        /// - SlotSettings uses shared_mutex internally for safe concurrent access
        /// - Side effects are ordered to prevent inconsistent state
        /// - Worst case: one update frame with mixed old/new values (acceptable for tuning)
        void ReloadAllSettings(const std::filesystem::path& iniPath);

        /// Handle button callbacks (Reset Q-Table, Reset Defaults, Reload INI)
        void HandleButtonCallback(std::string_view buttonId);

        /// Reset all settings to compile-time defaults
        void ResetAllToDefaults();

        /// Apply side effects after settings have been reloaded/reset
        /// (scorer config, allocator, locker, wheels, widget)
        void ApplySideEffects();

        /// Registration state (atomic for thread safety)
        std::atomic<bool> m_registered{false};
    };
}
