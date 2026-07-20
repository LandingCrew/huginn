#include "SettingsReloader.h"

#include "Globals.h"
#include "update/UpdateHandler.h"
#include "slot/SlotAllocator.h"
#include "slot/SlotLocker.h"
#include "slot/SlotSettings.h"
#include "learning/ScorerSettings.h"
#include "learning/LearningSettings.h"
#include "learning/ExternalEquipLearner.h"
#include "context/ContextWeightSettings.h"
#include "context/ContextWeightConfig.h"
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
                ReloadAllSettings(GetDMenuIniPath());
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
        // ModCallbackEvent senders (dMenu) may dispatch from any thread, and
        // Phase 1 reassigns non-POD settings (std::string members) the pipeline
        // reads per tick. Serialize against the update loop here, in the callee,
        // so no caller can forget it.
        Update::UpdateHandler::GetSingleton()->RunExclusive([&] {
            ReloadAllSettingsExclusive(dMenuIniPath);
        });
    }

    void SettingsReloader::ReloadAllSettingsExclusive(const std::filesystem::path& dMenuIniPath)
    {
        logger::info("[SettingsReloader] Reloading (dMenu INI: {})"sv, dMenuIniPath.string());

        // dMenu INI only contains dMenu-managed sections (Widget, Keybindings, Debug).
        // Non-dMenu settings always load from the main INI to avoid reading defaults
        // from an incomplete file (dMenu's flush_ini creates a fresh CSimpleIniA).
        const auto mainIniPath = GetMainIniPath();

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

        // Snapshot the wheel layout BEFORE reloading so ApplySideEffects can tell
        // whether a Wheeler rebuild is actually needed (vs. e.g. a scoring tweak).
        const WheelLayout wheelLayoutBefore = CaptureWheelLayout();

        // =====================================================================
        // Phase 1: Reload all settings from INI
        // =====================================================================
        // Non-dMenu settings load from main INI (Scoring, ContextWeights, etc.)
        // dMenu-managed settings load from dMenu INI (Widget, Keybindings, Debug).
        // Each INI is parsed ONCE here and handed to every loader via LoadFromIni,
        // instead of each loader re-opening and re-parsing the file itself.
        CSimpleIniA mainIni;
        const bool haveMain = LoadIniFile(mainIni, mainIniPath, "ReloadMain"sv);
        CSimpleIniA dMenuIni;
        const bool haveDMenu = LoadIniFile(dMenuIni, dMenuPath, "ReloadDMenu"sv);

        if (haveMain) {
            // 1. SlotSettings first (page count may change, affects allocator + wheels)
            Slot::SlotSettings::GetSingleton().LoadFromIni(mainIni);
            logger::debug("[SettingsReloader]   [SlotSettings] reloaded"sv);

            // 2. Scorer settings
            Scoring::ScorerSettings::GetSingleton().LoadFromIni(mainIni);
            logger::debug("[SettingsReloader]   [ScorerSettings] reloaded"sv);

            // 3. Context weight settings
            State::ContextWeightSettings::GetSingleton().LoadFromIni(mainIni);
            logger::debug("[SettingsReloader]   [ContextWeights] reloaded"sv);

            // 4. Override settings
            Override::Settings::GetSingleton().LoadFromIni(mainIni);
            logger::debug("[SettingsReloader]   [Overrides] reloaded"sv);

            // 4b. Learning settings
            Learning::LearningSettings::GetSingleton().LoadFromIni(mainIni);
            logger::debug("[SettingsReloader]   [Learning] reloaded"sv);

            // 5. Wheeler settings (before wheel rebuild)
            Wheeler::WheelerSettings::GetSingleton().LoadFromIni(mainIni);
            logger::debug("[SettingsReloader]   [Wheeler] reloaded"sv);

            // 6. Candidate config
            LoadCandidateConfigFromINI(mainIni);
            logger::debug("[SettingsReloader]   [Candidates] reloaded"sv);
        }

        // 7. Intuition widget settings (dMenu-managed)
        Input::KeybindingSettings keybindings;
        if (haveDMenu) {
            UI::IntuitionSettings::GetSingleton().LoadFromIni(dMenuIni);
            logger::debug("[SettingsReloader]   [Widget] reloaded"sv);

            // 8. Keybindings (dMenu-managed)
            keybindings.LoadFromIni(dMenuIni);
            logger::debug("[SettingsReloader]   [Keybindings] reloaded"sv);

            // 9. Debug widget visibility (dMenu-managed)
            UI::DebugSettings::GetSingleton().LoadFromIni(dMenuIni);
            logger::debug("[SettingsReloader]   [Debug] reloaded"sv);
        }
        // Apply keybindings (defaults if the dMenu INI was absent — matches the
        // prior LoadFromFile-on-missing-file behavior of leaving defaults).
        Input::InputHandler::GetSingleton().SetKeyCodes(keybindings);

        // =====================================================================
        // Phase 2: Apply side effects (reuse the already-parsed main INI)
        // =====================================================================
        ApplySideEffects(haveMain ? &mainIni : nullptr, wheelLayoutBefore);

        logger::info("[SettingsReloader] Reload complete"sv);
        RE::DebugNotification("Huginn: Settings reloaded");
    }

    void SettingsReloader::HandleButtonCallback(std::string_view buttonId)
    {
        // NOTE: Button IDs use lowercase snake_case to match dMenu event naming convention
        // (dmenu_updateSettings, dmenu_buttonCallback, etc.)

        if (buttonId == "Huginn_reset_qtable"sv) {
            logger::info("[SettingsReloader] Resetting learning data"sv);
            if (const auto fqlItems = ResetLearningData()) {
                logger::info("[SettingsReloader] Learning data reset complete ({} FQL items)"sv, *fqlItems);
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
            logger::info("[SettingsReloader] Manually reloading from INI"sv);
            // ReloadAllSettings already emits "Huginn: Settings reloaded";
            // no second notification here (avoids a double toast).
            // dMenu-managed sections still come from the dMenu INI (when present)
            // so a manual reload doesn't reset dMenu-managed customizations.
            ReloadAllSettings(GetDMenuIniPath());
        }
        else {
            logger::warn("[SettingsReloader] Unknown button callback: {}"sv, buttonId);
            RE::DebugNotification("Huginn: Unknown action (check logs)");
        }
    }

    std::optional<size_t> SettingsReloader::ResetLearningData()
    {
        auto* fql = g_featureQLearner.get();
        if (!fql) {
            return std::nullopt;
        }

        size_t fqlItems = 0;
        Update::UpdateHandler::GetSingleton()->RunExclusive([&] {
            fqlItems = fql->GetItemCount();
            fql->Clear();

            // Unlock all slots so the next scoring cycle can reassign immediately —
            // otherwise locked slots keep pinning recommendations scored by the
            // just-cleared table for the remainder of their lock duration.
            Slot::SlotLocker::GetSingleton().Reset();
        });
        return fqlItems;
    }

    void SettingsReloader::ResetAllToDefaults()
    {
        // Same serialization rationale as ReloadAllSettings.
        Update::UpdateHandler::GetSingleton()->RunExclusive([&] {
            ResetAllToDefaultsExclusive();
        });
    }

    void SettingsReloader::ResetAllToDefaultsExclusive()
    {
        // Reset all settings to compile-time defaults
        // Each Settings class has a ResetToDefaults() method

        // Snapshot before resetting so the Wheeler rebuild is skipped if the
        // defaults happen to match the current wheel layout.
        const WheelLayout wheelLayoutBefore = CaptureWheelLayout();

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
        inputHandler.SetKeyCodes(keybindings);

        logger::debug("[SettingsReloader]   All settings reset to compile-time defaults"sv);

        // Apply side effects (same as reload)
        ApplySideEffects(nullptr, wheelLayoutBefore);

        logger::info("[SettingsReloader] Reset to defaults complete"sv);
    }

    SettingsReloader::WheelLayout SettingsReloader::CaptureWheelLayout()
    {
        WheelLayout layout;
        layout.wheelPosition = Wheeler::WheelerSettings::GetSingleton().GetAPIPosition();

        auto& slotSettings = Slot::SlotSettings::GetSingleton();
        const size_t pageCount = slotSettings.GetPageCount();
        layout.pages.reserve(pageCount);
        for (size_t p = 0; p < pageCount; ++p) {
            const auto page = slotSettings.GetPage(p);
            layout.pages.emplace_back(page.name, page.slots.size());
        }
        return layout;
    }

    void SettingsReloader::ApplySideEffects(const CSimpleIniA* mainIni, const WheelLayout& beforeLayout)
    {
        // REQUIRES: update mutex held (RunExclusive) — reached only via
        // ReloadAllSettingsExclusive / ResetAllToDefaultsExclusive. That lock is
        // what makes the Phase 1 setting reassignments and the allocator/locker/
        // wheel mutations below safe against a mid-tick pipeline read; dMenu's
        // ModCallbackEvent gives no thread guarantee. SlotSettings additionally
        // has its own shared_mutex. Side effects are ordered to avoid inconsistency.

        // 1. Apply scorer config + context weight config + wildcard config
        auto* utilityScorer = g_utilityScorer.get();
        if (utilityScorer) {
            utilityScorer->SetConfig(Scoring::ScorerSettings::GetSingleton().BuildConfig());
            utilityScorer->SetContextWeightConfig(State::ContextWeightSettings::GetSingleton().BuildConfig());
            // Reuse the caller's parsed INI when available (reload path); otherwise
            // parse Huginn.ini here (reset-to-defaults path).
            if (mainIni) {
                LoadWildcardConfigFromINI(utilityScorer->GetWildcardManager(), *mainIni);
            } else {
                LoadWildcardConfigFromINI(utilityScorer->GetWildcardManager());
            }
        }

        // 1b. Apply learning config to ExternalEquipLearner
        Learning::ExternalEquipLearner::GetSingleton().SetConfig(
            Learning::LearningSettings::GetSingleton().BuildConfig());

        // 2. Re-initialize slot allocator FIRST (re-reads SlotSettings for new page count)
        Slot::SlotAllocator::GetSingleton().Initialize();

        // 3. Reset slot locker AFTER allocator (so it operates on correct slot count)
        auto& slotLocker = Slot::SlotLocker::GetSingleton();
        slotLocker.Reset();
        slotLocker.SetConfig(mainIni ? LoadSlotLockerConfigFromINI(*mainIni)
                                     : LoadSlotLockerConfigFromINI());

        // 4. Wheeler wheels — rebuild ONLY if the wheel structure actually changed.
        // Wheel creation depends solely on the page layout (count/name/slot count)
        // and the wheel position; subtext labels, auto-focus, etc. are read per-tick
        // and need no rebuild. Tearing valid wheels down on every reload (e.g. a
        // scoring tweak) is wasteful cross-DLL work, so compare and skip when equal.
        auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
        if (wheelerClient.IsConnected()) {
            if (CaptureWheelLayout() != beforeLayout) {
                wheelerClient.DestroyRecommendationWheels();
                wheelerClient.CreateRecommendationWheels();
                logger::debug("[SettingsReloader]   [Wheeler] wheels rebuilt (layout changed)"sv);
            } else {
                // Structure unchanged: don't tear down valid wheels. Create is
                // idempotent — it no-ops when wheels are valid, or recreates them
                // if they went missing (e.g. a prior save/load invalidation).
                wheelerClient.CreateRecommendationWheels();
                logger::debug("[SettingsReloader]   [Wheeler] layout unchanged, skipped rebuild"sv);
            }
        }

        // 5. Reapply Intuition widget settings (position, alpha, scale)
        auto intuitionConfig = UI::IntuitionSettings::GetSingleton().BuildConfig();
        auto* menu = UI::IntuitionMenu::GetSingleton();
        if (menu) {
            menu->ReapplySettings(intuitionConfig);
        } else {
            logger::debug("[SettingsReloader]   [Widget] IntuitionMenu unavailable, skipping ReapplySettings"sv);
        }

        // 6. Apply debug widget visibility (Phase 2 — DebugSettings::LoadFromIni is a
        //    pure loader now; this is the side-effect step for both reload and reset).
        UI::DebugSettings::GetSingleton().ApplyToWidgets();

        // The next update tick (~100ms) will pick up all new settings.
        // SlotLocker::Reset() ensures the first tick after reload can freely
        // reassign all slots, so the player sees changes almost immediately.
    }
}
