#include "SettingsReloader.h"
#include "learning/SelectionTracker.h"
#include "learning/EquipEventBus.h"

#include "Globals.h"
#include "update/UpdateHandler.h"
#include "slot/SlotAllocator.h"
#include "slot/SlotLocker.h"
#include "slot/Remembrance.h"
#include "slot/SlotSettings.h"
#include "learning/ScorerSettings.h"
#include "learning/legacy/LegacyScorer.h"   // g_legacyScorer (R8.4)
#include "learning/LearningSettings.h"
#include "learning/ExternalEquipLearner.h"
#include "context/ContextWeightSettings.h"
#include "needs/NeedSettings.h"
#include "context/ContextWeightConfig.h"
#include "override/OverrideConfig.h"
#include "wheeler/WheelerSettings.h"
#include "wheeler/WheelerClient.h"
#include "ui/IntuitionSettings.h"
#include "ui/IntuitionMenu.h"
#include "ui/DebugSettings.h"
#include "input/KeybindingSettings.h"
#include "input/EquipSettings.h"
#include "input/InputHandler.h"
#include "candidate/CandidateGenerator.h"
#include "settings/ShippedDefaults.h"
#include "persist/ChoiceSerializer.h"

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

        // Each INI is parsed ONCE, as two layers (settings/ShippedDefaults.h):
        // the shipped file, then the player's over it, so a key the player's
        // file leaves out -- or a missing file -- reads the shipped value.
        // The main INI holds every section but [Widget]/[Debug]; dMenu's INI
        // holds those two, over the dMenu descriptor's defaults (dMenu's
        // flush_ini writes only the sections it tracks, so the two files stay
        // separate).
        CSimpleIniA mainIni;
        LoadMainIni(mainIni, "ReloadMain"sv);
        CSimpleIniA dMenuIni;
        LoadDMenuIni(dMenuIni, dMenuIniPath, "ReloadDMenu"sv);

        ApplyIni(mainIni, dMenuIni, /*resetToDefaults=*/false);

        logger::info("[SettingsReloader] Reload complete"sv);
        RE::DebugNotification("Huginn: Settings reloaded");
    }

    void SettingsReloader::ApplyIni(const CSimpleIniA& mainIni, const CSimpleIniA& dMenuIni, bool resetToDefaults)
    {
        // Snapshot the wheel layout BEFORE reloading so ApplySideEffects can tell
        // whether a Wheeler rebuild is actually needed (vs. e.g. a scoring tweak).
        const WheelLayout wheelLayoutBefore = CaptureWheelLayout();


        // =====================================================================
        // Phase 1: every settings class from the parsed INIs
        // =====================================================================
        // 1. SlotSettings first (page count may change, affects allocator + wheels)
        Slot::SlotSettings::GetSingleton().LoadFromIni(mainIni);
        logger::debug("[SettingsReloader]   [SlotSettings] loaded"sv);

        // 2. Scorer settings
        Scoring::ScorerSettings::GetSingleton().LoadFromIni(mainIni);
        logger::debug("[SettingsReloader]   [ScorerSettings] loaded"sv);

        // 3. Context weight settings
        State::ContextWeightSettings::GetSingleton().LoadFromIni(mainIni);
        logger::debug("[SettingsReloader]   [ContextWeights] loaded"sv);

        // 3b. Need curves (R3; the pipeline reads them every gathered tick)
        Needs::NeedSettings::GetSingleton().LoadFromIni(mainIni);
        logger::debug("[SettingsReloader]   [Needs] loaded"sv);

        // 4. Override settings
        Override::Settings::GetSingleton().LoadFromIni(mainIni);
        logger::debug("[SettingsReloader]   [Overrides] loaded"sv);

        // 4b. Learning settings
        Learning::LearningSettings::GetSingleton().LoadFromIni(mainIni);
        logger::debug("[SettingsReloader]   [Learning] loaded"sv);

        // 5. Wheeler settings (before wheel rebuild)
        Wheeler::WheelerSettings::GetSingleton().LoadFromIni(mainIni);
        logger::debug("[SettingsReloader]   [Wheeler] loaded"sv);

        // 6. Candidate config
        LoadCandidateConfigFromINI(mainIni);
        // ...and push it into the generator, which keeps its own copy.
        // Reloading the global alone left the generator on its old values;
        // see the matching call in InitializeGameSystems. Safe here because
        // this runs under UpdateHandler::RunExclusive, which is the "update
        // loop is paused" precondition this method documents.
        Candidate::CandidateGenerator::GetSingleton().RefreshConfigFromGlobal();
        logger::debug("[SettingsReloader]   [Candidates] loaded"sv);

        // 7-9. Widget / Debug (dMenu's) and Keybindings (ours).
        //
        // One key, one home. dMenu owns [Widget] and [Debug] and stores them in
        // its own file; the main Huginn.ini does not define those sections. It
        // owns [Keybindings], which dMenu no longer declares. Nothing in one
        // file overrides the other, so a setting changed in the dMenu UI is not
        // overwritten on the reload dMenu fires to announce it.
        UI::IntuitionSettings::GetSingleton().LoadFromIni(dMenuIni);
        UI::DebugSettings::GetSingleton().LoadFromIni(dMenuIni);
        logger::debug("[SettingsReloader]   [Widget]/[Debug] loaded"sv);

        Input::KeybindingSettings keybindings;
        keybindings.LoadFromIni(mainIni);
        Input::InputHandler::GetSingleton().SetKeyCodes(keybindings);
        logger::debug("[SettingsReloader]   [Keybindings] loaded"sv);

        // [Equip] (0.24.26): read at the next hand equip.
        Input::EquipSettings::GetSingleton().LoadFromIni(mainIni);
        logger::debug("[SettingsReloader]   [Equip] loaded"sv);

        // Read-only comes from [Widget] (dMenu's file), not from the keybindings
        // it suppresses -- so it must be pushed AFTER the dMenu INI is loaded
        // above, or a toggle made in dMenu would not take effect until restart.
        Input::InputHandler::GetSingleton().SetReadOnly(
            UI::IntuitionSettings::GetSingleton().IsReadOnly());

        // =====================================================================
        // Phase 2: Apply side effects (reuse the already-parsed main INI)
        // =====================================================================
        ApplySideEffects(mainIni, wheelLayoutBefore, resetToDefaults);
    }

    void SettingsReloader::HandleButtonCallback(std::string_view buttonId)
    {
        // NOTE: Button IDs use lowercase snake_case to match dMenu event naming convention
        // (dmenu_updateSettings, dmenu_buttonCallback, etc.)

        if (buttonId == "Huginn_reset_widget"sv) {
            // The same reset as the key (iResetWidgetKey), for players who
            // would rather click than learn a key.
            UI::IntuitionMenu::ResetWidget();
            RE::DebugNotification("Huginn: Widget reset");
        }
        else if (buttonId == "Huginn_reset_weights"sv) {
            logger::info("[SettingsReloader] Resetting learning data"sv);
            if (const auto reset = ResetLearningData()) {
                logger::info("[SettingsReloader] Learning data reset complete ({} learner items; {})"sv,
                    reset->learnerItems, reset->choice);
                const auto msg = std::format("Huginn: Learning reset ({})", reset->choice);
                RE::DebugNotification(msg.c_str());
            } else {
                logger::warn("[SettingsReloader] No game loaded yet: no learning to reset"sv);
            }
        }
        else if (buttonId == "Huginn_reset_defaults"sv) {
            logger::info("[SettingsReloader] Resetting every setting to the shipped defaults"sv);
            ResetAllToDefaults();   // notifies
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

    std::optional<SettingsReloader::LearningReset> SettingsReloader::ResetLearningData()
    {
        // Deferred passed-over updates and the repeat window belong to the
        // learning being reset.
        Learning::EquipEventBus::GetSingleton().Reset();
        Learning::SelectionTracker::GetSingleton().ForgetRepeats();
        // No game loaded yet (both learners are built at the first load):
        // nothing to reset.
        auto* learner = g_featureBanditLearner.get();
        if (!learner && !g_choiceLearner) {
            return std::nullopt;
        }

        LearningReset reset;
        Update::UpdateHandler::GetSingleton()->RunExclusive([&] {
            if (learner) {
                reset.learnerItems = learner->GetItemCount();
                learner->Clear();
            }

            // R8.3: the choice learner too -- theta back to the bootstrap and b
            // cleared (no sBootstrapBias seed: a reset forgets this character's
            // b, the file's included); 0.24.18: with the model off, the THTA /
            // BIAS carried from the load emptied. The old learner above is
            // reset as before: it feeds the D2 legacy feature.
            reset.choice = Persist::ResetChoiceLearning(g_activeCharacterID.load(std::memory_order_relaxed));
            // 0.24.22: the reset ticks the learning clock here -- not in
            // ResetToBootstrap (a load's too, and a no-op with the model off)
            // -- so a reload of an older save of this character keeps the
            // reset (core/LearningClock.h). With the old learner present its
            // Clear above ticked it too: twice in all, until R12.
            Core::LearningClock::Note(&g_learningClock, Core::LearningClock::Event::Reset);

            // Unlock all slots so the next scoring cycle can reassign immediately —
            // otherwise locked slots keep pinning recommendations scored by the
            // just-cleared table for the remainder of their lock duration.
            Slot::SlotLocker::GetSingleton().Reset();
        });
        return reset;
    }

    void SettingsReloader::ResetAllToDefaults()
    {
        // The big red button: every setting back to the shipped Huginn.ini and
        // the dMenu descriptor's defaults, in memory AND on disk, so the next
        // game load (or reload, or dMenu change) reads the same thing -- the
        // game behaves as a fresh install of the shipped files would. Learned
        // data (THTA/BIAS/BNDW) is not a setting: Reset Learned Weights.
        //
        // The files first, outside the update mutex (plain file I/O; nothing
        // the pipeline reads). A failed write leaves that file as it was and
        // the reset holds for this session only; the notification says so.
        const RestoreResult written = RestoreShippedFiles();

        // Same serialization rationale as ReloadAllSettings.
        Update::UpdateHandler::GetSingleton()->RunExclusive([&] {
            ResetAllToDefaultsExclusive();
        });

        using Outcome = RestoreResult::Outcome;
        if (!written.Ok()) {
            RE::DebugNotification("Huginn: Settings reset for this session only: could not write the INI (see Huginn.log)");
        } else if (written.main == Outcome::Written || written.dMenu == Outcome::Written) {
            RE::DebugNotification("Huginn: Settings back to the shipped defaults (old files kept as .bak)");
        } else {
            RE::DebugNotification("Huginn: Settings back to the shipped defaults");
        }
        if (written.dMenu == Outcome::Written) {
            // dMenu read its INI when the game started and shows (and on its
            // next save, writes back) what it read then. Its panel catches up
            // at the next game start; until then, changing a Huginn widget
            // setting in it would write the old [Widget]/[Debug] values back.
            RE::DebugNotification("Huginn: Restart the game before changing the widget panel");
        }
    }

    void SettingsReloader::ResetAllToDefaultsExclusive()
    {
        // The shipped layers alone -- not re-read from disk, so a file that
        // could not be written still resets this session.
        CSimpleIniA mainIni;
        LoadShippedMainIni(mainIni);
        CSimpleIniA dMenuIni;
        LoadShippedDMenuIni(dMenuIni);

        ApplyIni(mainIni, dMenuIni, /*resetToDefaults=*/true);

        logger::info("[SettingsReloader] Reset to the shipped defaults complete"sv);
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

    void SettingsReloader::ApplySideEffects(const CSimpleIniA& mainIni, const WheelLayout& beforeLayout,
                                           bool resetToDefaults)
    {
        // REQUIRES: update mutex held (RunExclusive) — reached only via
        // ApplyIni (reload and reset). That lock is
        // what makes the Phase 1 setting reassignments and the allocator/locker/
        // wheel mutations below safe against a mid-tick pipeline read; dMenu's
        // ModCallbackEvent gives no thread guarantee. SlotSettings additionally
        // has its own shared_mutex. Side effects are ordered to avoid inconsistency.

        // 1. Apply scorer config + context weight config + wildcard config
        auto* utilityScorer = g_utilityScorer.get();
        if (g_legacyScorer) {
            g_legacyScorer->SetConfig(Scoring::ScorerSettings::GetSingleton().BuildConfig());
            g_legacyScorer->SetContextWeightConfig(State::ContextWeightSettings::GetSingleton().BuildConfig());
        }
        if (utilityScorer) {
            utilityScorer->SetConfig(Scoring::ScorerSettings::GetSingleton().BuildConfig());
            LoadWildcardConfigFromINI(utilityScorer->GetWildcardManager(), mainIni);
        }

        // 1b. Apply learning config to ExternalEquipLearner and the learner's
        // useful life
        Learning::ExternalEquipLearner::GetSingleton().SetConfig(
            Learning::LearningSettings::GetSingleton().BuildConfig());
        if (g_featureBanditLearner) {
            g_featureBanditLearner->SetMemoryLife(Learning::LearningSettings::GetSingleton().BuildConfig().memory);
        }
        // R8.2: the new learner's keys ([Learning] sBootstrapBias and
        // fThetaProcessNoise, [Scoring] fCoveredPrior, bLegacyLearnFeature and
        // (0.24.17) bShadowLegacy,
        // [Favorites] fFavoriteLifeMult; 0.24.14: bDiscoveryBoost,
        // fDiscoveryBoostVarMult, fDiscoveryBoostHalfLifeSec, live at the next
        // pipeline run) and its battery. sBootstrapBias and
        // fCoveredPrior take effect at the next load or `hg reset all`.
        if (g_choiceLearner) {
            const auto cs = Learning::ChoiceSettings::FromIni(mainIni);
            g_choiceLearner->SetSettings(cs);
            // R8.3: a live score input. A change logs one line and re-scores
            // the page now (the switch moves no sensor).
            if (Scoring::Legacy::SetEnabled(cs.legacyLearnFeature) != cs.legacyLearnFeature) {
                Slot::SlotAllocator::GetSingleton().MarkPageDirty();
            }
            // 0.24.17: the D2 shadow. Shows nothing, but a run now puts it on
            // (or takes it off) the logged rows at once.
            if (Scoring::Legacy::SetShadowEnabled(cs.shadowLegacy) != cs.shadowLegacy) {
                Slot::SlotAllocator::GetSingleton().MarkPageDirty();
            }
            g_choiceLearner->SetMemoryLife(Learning::LearningSettings::GetSingleton().BuildConfig().memory);
            if (utilityScorer) {
                g_choiceLearner->SetFavoritesBoost(utilityScorer->GetConfig().favoritesMode == Scoring::FavoritesMode::Boost);
            }
        }

        // 2. Re-initialize slot allocator FIRST (re-reads SlotSettings for new page count)
        Slot::SlotAllocator::GetSingleton().Initialize();

        // 3. Reset slot locker AFTER allocator (so it operates on correct slot count)
        auto& slotLocker = Slot::SlotLocker::GetSingleton();
        slotLocker.Reset();
        slotLocker.SetConfig(LoadSlotLockerConfigFromINI(mainIni));
        // Holds name page/slot indices of the layout that just went away.
        Slot::Remembrance::GetSingleton().Reset();

        // 4. Wheeler wheels — rebuild ONLY if the wheel structure actually changed.
        // Wheel creation depends solely on the page layout (count/name/slot count)
        // and the wheel position; subtext labels, auto-focus, etc. are read per-tick
        // and need no rebuild. Tearing valid wheels down on every reload (e.g. a
        // scoring tweak) is wasteful cross-DLL work, so compare and skip when equal.
        auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
        if (wheelerClient.IsConnected()) {
            const WheelLayout afterLayout = CaptureWheelLayout();
            // The REBUILD keys on the whole layout — a renamed page or a changed
            // slot count needs new wheels. The FORGET must not: WheelLayout also
            // carries every page's name and slot count, so keying the forget on
            // the same comparison threw away a position the player had set by
            // dragging whenever they edited something unrelated (iPage2Slots, a
            // page rename) and ran `hg reload` — the wheels jumped back to
            // sWheelPosition and the player had never said to move them. Only a
            // changed sWheelPosition is them re-stating where the wheels belong.
            // Reset to Defaults restates everything: the wheels are rebuilt at
            // sWheelPosition and a position the player dragged them to this
            // session is forgotten, as a fresh start would have it.
            const bool positionRestated =
                resetToDefaults || afterLayout.wheelPosition != beforeLayout.wheelPosition;
            if (resetToDefaults || afterLayout != beforeLayout) {
                // AFTER the destroy, not before. DestroyWheels() re-reads the
                // live indices and can record the anchor again, so forgetting
                // first is undone in the same breath — the forget has to land in
                // the window between teardown and creation. Observed 2026-08-28
                // with the calls the other way round: `Forgetting remembered
                // wheel position 1` at 14:42:13.256, `Remembering wheel position
                // 1 (was -1)` in the same millisecond, and the wheels recreated
                // at 1/2/3 with sWheelPosition = 3.
                wheelerClient.DestroyRecommendationWheels();
                if (positionRestated) {
                    wheelerClient.ForgetWheelPositionMemory();
                }
                wheelerClient.CreateRecommendationWheels();
                logger::debug("[SettingsReloader]   [Wheeler] wheels rebuilt (layout changed, "
                              "position restated: {})"sv, positionRestated);
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
        } else if (intuitionConfig.enabled) {
            // No singleton because bEnabled was false at load: Show() early-returns
            // on !IsEnabled() (IntuitionMenu.cpp:35), so the menu was never built and
            // ReapplySettings has nothing to talk to. The player has just turned the
            // widget ON, and Show() is the only way to construct it — without this the
            // toggle appears to do nothing until the next loading screen.
            //
            logger::info("[SettingsReloader]   [Widget] enabled with no menu — showing"sv);
            UI::IntuitionMenu::Show();
        } else {
            logger::debug("[SettingsReloader]   [Widget] IntuitionMenu unavailable, skipping ReapplySettings"sv);
        }

        // 6. Apply debug widget visibility (Phase 2 — DebugSettings::LoadFromIni is a
        //    pure loader; this is the side-effect step for both reload and reset).
        UI::DebugSettings::GetSingleton().ApplyToWidgets();

        // The next update tick (~100ms) will pick up all new settings.
        // SlotLocker::Reset() ensures the first tick after reload can freely
        // reassign all slots, so the player sees changes almost immediately.
    }
}
