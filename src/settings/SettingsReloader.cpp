#include "SettingsReloader.h"

#include "Globals.h"
#include "slot/SlotAllocator.h"
#include "slot/SlotLocker.h"
#include "slot/SlotSettings.h"
#include "learning/ScorerSettings.h"
#include "learning/LearningSettings.h"
#include "learning/ExternalEquipLearner.h"
#include "state/ContextWeightSettings.h"
#include "state/ContextWeightConfig.h"
#include "override/OverrideConfig.h"
#include "wheeler/WheelerSettings.h"
#include "wheeler/WheelerClient.h"
#include "ui/IntuitionSettings.h"
#include "ui/IntuitionMenu.h"
#include "ui/DebugSettings.h"
#include "input/KeybindingSettings.h"
#include "input/InputHandler.h"

#include <filesystem>

namespace Huginn::Settings
{
    SettingsReloader& SettingsReloader::GetSingleton()
    {
        static SettingsReloader singleton;
        return singleton;
    }

    void SettingsReloader::Register()
    {
        if (m_registered.load(std::memory_order_acquire)) {
            logger::warn("[SettingsReloader] Already registered, skipping"sv);
            return;
        }

        auto* eventSource = SKSE::GetModCallbackEventSource();
        if (!eventSource) {
            logger::error("[SettingsReloader] Failed to get ModCallbackEventSource"sv);
            return;
        }

        eventSource->AddEventSink(this);
        m_registered.store(true, std::memory_order_release);

        logger::info("[SettingsReloader] Registered for dMenu events (dmenu_updateSettings, dmenu_buttonCallback)"sv);
    }

    void SettingsReloader::Unregister()
    {
        if (!m_registered.load(std::memory_order_acquire)) {
            return;
        }

        auto* eventSource = SKSE::GetModCallbackEventSource();
        if (eventSource) {
            eventSource->RemoveEventSink(this);
        }

        m_registered.store(false, std::memory_order_release);
        logger::info("[SettingsReloader] Unregistered from dMenu events"sv);
    }

    RE::BSEventNotifyControl SettingsReloader::ProcessEvent(
        const SKSE::ModCallbackEvent* event,
        RE::BSTEventSource<SKSE::ModCallbackEvent>* /*source*/)
    {
        if (!event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // Handle dmenu_updateSettings event (triggered when user changes settings in dMenu)
        if (event->eventName == "dmenu_updateSettings"sv) {
            // strArg contains the mod name - only process if it's "Huginn"
            if (event->strArg == "Huginn"sv) {
                logger::info("[SettingsReloader] dmenu_updateSettings received for Huginn"sv);

                // dMenu stores settings in Data/SKSE/Plugins/dmenu/customSettings/ini/Huginn.ini
                const auto dmenuIniPath = std::filesystem::path("Data/SKSE/Plugins/dmenu/customSettings/ini/Huginn.ini");
                ReloadAllSettings(dmenuIniPath);
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        // Handle dmenu_buttonCallback event (triggered when user clicks action buttons)
        if (event->eventName == "dmenu_buttonCallback"sv) {
            logger::info("[SettingsReloader] dmenu_buttonCallback received: {}"sv, event->strArg.c_str());
            HandleButtonCallback(event->strArg.c_str());
            return RE::BSEventNotifyControl::kContinue;
        }

        return RE::BSEventNotifyControl::kContinue;
    }

    void SettingsReloader::ReloadAllSettings(const std::filesystem::path& dMenuIniPath)
    {
        logger::info("[SettingsReloader] Reloading (dMenu INI: {})"sv, dMenuIniPath.string());

        // dMenu INI only contains dMenu-managed sections (Widget, Keybindings, Debug).
        // Non-dMenu settings always load from the main INI to avoid reading defaults
        // from an incomplete file (dMenu's flush_ini creates a fresh CSimpleIniA).
        const auto mainIniPath = std::filesystem::path("Data/SKSE/Plugins/Huginn.ini");

        // Graceful degradation: fall back to main INI if dMenu path doesn't exist
        auto dMenuPath = dMenuIniPath;
        try {
            if (!std::filesystem::exists(dMenuIniPath)) {
                dMenuPath = mainIniPath;
                logger::warn("[SettingsReloader] dMenu INI not found at {}, falling back to Huginn.ini"sv,
                             dMenuIniPath.string());
            }
        } catch (const std::filesystem::filesystem_error& e) {
            logger::error("[SettingsReloader] Filesystem error checking dMenu INI: {} - falling back to Huginn.ini"sv,
                          e.what());
            dMenuPath = mainIniPath;
        }

        // =====================================================================
        // Phase 1: Reload all settings from INI
        // =====================================================================
        // Non-dMenu settings load from main INI (Scoring, ContextWeights, etc.)
        // dMenu-managed settings load from dMenu INI (Widget, Keybindings, Debug)

        // 1. SlotSettings first (page count may change, affects allocator + wheels)
        Slot::SlotSettings::GetSingleton().LoadFromFile(mainIniPath);
        logger::debug("[SettingsReloader]   [SlotSettings] reloaded"sv);

        // 2. Scorer settings
        auto& scorerSettings = Scoring::ScorerSettings::GetSingleton();
        scorerSettings.LoadFromFile(mainIniPath);
        logger::debug("[SettingsReloader]   [ScorerSettings] reloaded"sv);

        // 3. Context weight settings
        State::ContextWeightSettings::GetSingleton().LoadFromFile(mainIniPath);
        logger::debug("[SettingsReloader]   [ContextWeights] reloaded"sv);

        // 4. Override settings
        Override::Settings::GetSingleton().LoadFromFile(mainIniPath);
        logger::debug("[SettingsReloader]   [Overrides] reloaded"sv);

        // 4b. Learning settings
        Learning::LearningSettings::GetSingleton().LoadFromFile(mainIniPath);
        logger::debug("[SettingsReloader]   [Learning] reloaded"sv);

        // 5. Wheeler settings (before wheel rebuild)
        Wheeler::WheelerSettings::GetSingleton().LoadFromFile(mainIniPath);
        logger::debug("[SettingsReloader]   [Wheeler] reloaded"sv);

        // 6. Candidate config (already hardcodes main INI internally)
        LoadCandidateConfigFromINI();
        logger::debug("[SettingsReloader]   [Candidates] reloaded"sv);

        // 7. Intuition widget settings (dMenu-managed)
        UI::IntuitionSettings::GetSingleton().LoadFromFile(dMenuPath);
        logger::debug("[SettingsReloader]   [Widget] reloaded"sv);

        // 8. Keybindings (dMenu-managed)
        Input::KeybindingSettings keybindings;
        keybindings.LoadFromFile(dMenuPath);
        auto& inputHandler = Input::InputHandler::GetSingleton();
        inputHandler.SetKeyCodes(
            keybindings.slot1Key, keybindings.slot2Key, keybindings.slot3Key,
            keybindings.slot4Key, keybindings.slot5Key, keybindings.slot6Key,
            keybindings.slot7Key, keybindings.slot8Key, keybindings.slot9Key,
            keybindings.slot10Key, keybindings.prevPageKey, keybindings.nextPageKey
        );
        logger::debug("[SettingsReloader]   [Keybindings] reloaded"sv);

        // 9. Debug widget visibility (dMenu-managed)
        UI::DebugSettings::GetSingleton().LoadFromFile(dMenuPath);
        logger::debug("[SettingsReloader]   [Debug] reloaded"sv);

        // =====================================================================
        // Phase 2: Apply side effects
        // =====================================================================
        ApplySideEffects();

        logger::info("[SettingsReloader] Reload complete"sv);
        RE::DebugNotification("Huginn: Settings reloaded");
    }

    void SettingsReloader::HandleButtonCallback(std::string_view buttonId)
    {
        // NOTE: Button IDs use lowercase snake_case to match dMenu event naming convention
        // (dmenu_updateSettings, dmenu_buttonCallback, etc.)

        if (buttonId == "Huginn_reset_qtable"sv) {
            logger::info("[SettingsReloader] Resetting learning data"sv);
            auto* fql = g_featureQLearner.get();
            if (fql) {
                fql->Clear();
                logger::info("[SettingsReloader] FeatureQLearner reset complete"sv);
                RE::DebugNotification("Huginn: Learning data reset");
            } else {
                logger::warn("[SettingsReloader] g_featureQLearner is null, cannot reset"sv);
            }
        }
        else if (buttonId == "Huginn_reset_defaults"sv) {
            logger::info("[SettingsReloader] Resetting all settings to defaults"sv);
            ResetAllToDefaults();
            RE::DebugNotification("Huginn: Settings reset to defaults");
        }
        else if (buttonId == "Huginn_reload_ini"sv) {
            logger::info("[SettingsReloader] Manually reloading from Huginn.ini"sv);
            ReloadAllSettings("Data/SKSE/Plugins/Huginn.ini");
            RE::DebugNotification("Huginn: Settings reloaded from Huginn.ini");
        }
        else {
            logger::warn("[SettingsReloader] Unknown button callback: {}"sv, buttonId);
            RE::DebugNotification("Huginn: Unknown action (check logs)");
        }
    }

    void SettingsReloader::ResetAllToDefaults()
    {
        // Reset all settings to compile-time defaults
        // Each Settings class has a ResetToDefaults() method

        Slot::SlotSettings::GetSingleton().ResetToDefaults();
        Scoring::ScorerSettings::GetSingleton().ResetToDefaults();
        State::ContextWeightSettings::GetSingleton().ResetToDefaults();
        Override::Settings::GetSingleton().ResetToDefaults();
        Learning::LearningSettings::GetSingleton().ResetToDefaults();
        logger::debug("[SettingsReloader]   [Learning] reset to defaults"sv);
        Wheeler::WheelerSettings::GetSingleton().ResetToDefaults();
        UI::IntuitionSettings::GetSingleton().ResetToDefaults();
        UI::DebugSettings::GetSingleton().ResetToDefaults();

        // Reset keybindings to defaults
        Input::KeybindingSettings keybindings;
        keybindings.ResetToDefaults();
        auto& inputHandler = Input::InputHandler::GetSingleton();
        inputHandler.SetKeyCodes(
            keybindings.slot1Key, keybindings.slot2Key, keybindings.slot3Key,
            keybindings.slot4Key, keybindings.slot5Key, keybindings.slot6Key,
            keybindings.slot7Key, keybindings.slot8Key, keybindings.slot9Key,
            keybindings.slot10Key, keybindings.prevPageKey, keybindings.nextPageKey
        );

        logger::debug("[SettingsReloader]   All settings reset to compile-time defaults"sv);

        // Apply side effects (same as reload)
        ApplySideEffects();

        logger::info("[SettingsReloader] Reset to defaults complete"sv);
    }

    void SettingsReloader::ApplySideEffects()
    {
        // NOTE: Phase 1 settings (ScorerSettings, ContextWeightSettings, etc.)
        // are POD float/bool singletons. Both UpdateHandler and SettingsReloader
        // run on the game thread, so no race conditions. "Mixed values" refers to
        // settings changing between update ticks (tick boundaries), which is
        // acceptable for tuning. SlotSettings has its own shared_mutex (safe).
        // The remaining side effects below are ordered to avoid inconsistency.

        // 1. Apply scorer config + context weight config + wildcard config
        auto* utilityScorer = g_utilityScorer.get();
        if (utilityScorer) {
            utilityScorer->SetConfig(Scoring::ScorerSettings::GetSingleton().BuildConfig());
            utilityScorer->SetContextWeightConfig(State::ContextWeightSettings::GetSingleton().BuildConfig());
            LoadWildcardConfigFromINI(utilityScorer->GetWildcardManager());
        }

        // 1b. Apply learning config to ExternalEquipLearner
        Learning::ExternalEquipLearner::GetSingleton().SetConfig(
            Learning::LearningSettings::GetSingleton().BuildConfig());

        // 2. Re-initialize slot allocator FIRST (re-reads SlotSettings for new page count)
        Slot::SlotAllocator::GetSingleton().Initialize();

        // 3. Reset slot locker AFTER allocator (so it operates on correct slot count)
        auto& slotLocker = Slot::SlotLocker::GetSingleton();
        slotLocker.Reset();
        slotLocker.SetConfig(LoadSlotLockerConfigFromINI());

        // 4. Rebuild Wheeler wheels (if connected)
        auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
        if (wheelerClient.IsConnected()) {
            wheelerClient.DestroyRecommendationWheels();
            wheelerClient.CreateRecommendationWheels();
            logger::debug("[SettingsReloader]   [Wheeler] wheels rebuilt"sv);
        }

        // 5. Reapply Intuition widget settings (position, alpha, scale)
        auto intuitionConfig = UI::IntuitionSettings::GetSingleton().BuildConfig();
        auto* menu = UI::IntuitionMenu::GetSingleton();
        if (menu) {
            menu->ReapplySettings(intuitionConfig);
        } else {
            logger::debug("[SettingsReloader]   [Widget] IntuitionMenu unavailable, skipping ReapplySettings"sv);
        }

        // The next update tick (~100ms) will pick up all new settings.
        // SlotLocker::Reset() ensures the first tick after reload can freely
        // reassign all slots, so the player sees changes almost immediately.
    }
}
