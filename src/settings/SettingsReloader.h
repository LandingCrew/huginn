#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
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
    /// Ownership (exclusive — nothing is defined in both files):
    /// - dMenu INI: [Widget], [Debug]
    /// - Main Huginn.ini: [Keybindings] + every other section
    /// Each is read over its shipped defaults (settings/ShippedDefaults.h): a
    /// key the file leaves out reads the shipped value.
    ///
    /// Graceful degradation:
    /// - If dMenu not installed: no events fire; [Widget]/[Debug] take the dMenu
    ///   descriptor's defaults (configuring the widget UI requires dMenu),
    ///   everything else loads from Huginn.ini as normal
    /// - If dMenu installed but no JSON: Huginn panel doesn't appear, same as above
    /// - If dMenu installed with JSON: full hot-reload via dMenu UI
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
        /// The path argument is the dMenu-managed INI, which supplies [Widget]
        /// and [Debug]; every other section — [Keybindings] included — always
        /// loads from the main INI. Callers pass GetDMenuIniPath(), which falls
        /// back to the main INI when dMenu is absent.
        ///
        /// Thread Safety:
        /// - Serializes itself against the update loop via
        ///   UpdateHandler::RunExclusive — callers must NOT wrap this call in
        ///   RunExclusive themselves (the mutex is not re-entrant; a caller-side
        ///   wrap deadlocks). ModCallbackEvent senders (dMenu) may dispatch from
        ///   any thread, so the callee-side lock is load-bearing, not belt-and-braces.
        /// - POD settings singletons (ScorerSettings, ContextWeightSettings) read without locks
        /// - SlotSettings uses shared_mutex internally for safe concurrent access
        /// - Side effects are ordered to prevent inconsistent state
        void ReloadAllSettings(const std::filesystem::path& iniPath);

        /// Clear the FeatureBanditLearner AND reset SlotLocker, under the update mutex.
        ///
        /// Single source of truth shared by `hg reset weights` and the dMenu
        /// "reset learning data" button — both must reset SlotLocker too, or
        /// locked slots keep pinning recommendations scored by the just-cleared
        /// table. Do not call from inside RunExclusive (not re-entrant).
        ///
        /// The choice learner too (Persist::ResetChoiceLearning): theta back
        /// to the bootstrap and b cleared, or, with the model off, the
        /// records carried from the load emptied.
        ///
        /// Ticks the learning clock (core/LearningClock.h, 0.24.22), so a
        /// reload of an older save keeps the reset: once at the reset, and
        /// once more in the old learner's Clear while it exists (until R12).
        ///
        /// @return The old learner's items cleared and what the choice reset
        ///         did (for the player's message), or nullopt before the first
        ///         game load (no learner exists: nothing was reset).
        struct LearningReset
        {
            size_t learnerItems = 0;
            const char* choice = "";
        };
        static std::optional<LearningReset> ResetLearningData();

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

        /// Handle button callbacks (Reset Learned Weights, Reset Defaults, Reload INI)
        void HandleButtonCallback(std::string_view buttonId);

        /// Reset every setting to the shipped defaults (Reset to Defaults):
        /// writes the shipped Huginn.ini (and dMenu's INI, at the descriptor's
        /// defaults, when it exists) over the player's, keeping each old file as
        /// <name>.<stamp>.bak, then loads the shipped layers alone. Learned data is not
        /// touched. Wraps itself in UpdateHandler::RunExclusive like
        /// ReloadAllSettings (the file writes happen before, outside the lock).
        void ResetAllToDefaults();

        /// Reload implementation. REQUIRES: update mutex held (RunExclusive).
        void ReloadAllSettingsExclusive(const std::filesystem::path& dMenuIniPath);

        /// Reset implementation. REQUIRES: update mutex held (RunExclusive).
        void ResetAllToDefaultsExclusive();

        /// Load every settings class from the parsed INIs (Phase 1), then
        /// ApplySideEffects (Phase 2). Shared by reload and reset, so the two
        /// cannot load different sets of settings. REQUIRES: update mutex held.
        /// @param resetToDefaults Reset to Defaults: also rebuild the Wheeler
        ///   wheels at sWheelPosition and clear the widget's hide latch.
        void ApplyIni(const CSimpleIniA& mainIni, const CSimpleIniA& dMenuIni, bool resetToDefaults);

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
        /// @param mainIni      The already-parsed main INI (the player's over the
        ///   shipped one, or the shipped one alone on a reset), reused for the
        ///   wildcard, slot-locker and choice-learner loads.
        /// @param beforeLayout Wheel layout captured before the reload/reset; the
        ///   Wheeler rebuild is skipped when the post-reload layout matches it
        ///   (never on a reset).
        void ApplySideEffects(const CSimpleIniA& mainIni, const WheelLayout& beforeLayout,
                              bool resetToDefaults);

        /// Registration state (atomic for thread safety)
        std::atomic<bool> m_registered{false};
    };
}
