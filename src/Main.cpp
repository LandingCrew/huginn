#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/spdlog.h>

#include "Config.h"
#include "Globals.h"
#include "UpdateLoop.h"
#include "Tests.h"

#include "state/StateManager.h"
#include "state/DamageEventSink.h"
#include "learning/InventoryExitTracker.h"
#include "spell/SpellRegistry.h"
#include "learning/item/ItemRegistry.h"
#include "weapon/WeaponRegistry.h"
#include "util/ExtraListStability.h"
#include "scroll/ScrollRegistry.h"
#include "learning/FeatureQLearner.h"
#include "learning/StateFeatures.h"
#include "learning/UtilityScorer.h"
#include "update/UpdateHandler.h"
#include "input/InputHandler.h"
#include "input/KeybindingSettings.h"
#include "input/EquipManager.h"
#include "ui/ImGuiRenderer.h"
#include "ui/WelcomeBanner.h"
#include "ui/D3D11Hook.h"
#include "ui/IntuitionMenu.h"
#include "ui/IntuitionSettings.h"
#include "ui/DebugSettings.h"
#include "ui/HudVisibilityManager.h"

#ifdef _DEBUG
#include "ui/StateManagerDebugWidget.h"
#include "ui/DebugInputHook.h"
#endif

#include "wheeler/WheelerClient.h"
#include "wheeler/WheelerSettings.h"
#include "candidate/CandidateGenerator.h"
#include "candidate/CandidateConfig.h"
#include "override/OverrideManager.h"
#include "slot/SlotAllocator.h"
#include "slot/SlotLocker.h"
#include "slot/SlotSettings.h"
#include "learning/ScorerSettings.h"
#include "learning/LearningSettings.h"
#include "learning/ExternalEquipListener.h"
#include "learning/ExternalEquipLearner.h"
#include "context/ContextWeightSettings.h"
#include "context/ContextWeightConfig.h"
#include "settings/SettingsReloader.h"
#include "console/ConsoleCommands.h"
#include "persist/QLearnerSerializer.h"
#include "learning/EquipEventBus.h"
#include "learning/EquipSubscribers.h"

using namespace Huginn;

// Event registration guard - ensures single TESEquipEvent registration across kNewGame/kPostLoadGame
// (Issue #3: Event sources can be recreated on game transitions, causing duplicate registrations)
static std::atomic<bool> g_equipEventRegistered{false};

// =============================================================================
// HELPER: INI path accessors
// =============================================================================
// Main INI contains all sections (Scoring, ContextWeights, Slot, Override, etc.)
// dMenu INI contains only dMenu-managed sections (Widget, Keybindings, Debug).
// dMenu's flush_ini() creates a fresh CSimpleIniA and writes only its tracked
// sections, so pointing it at the main INI would wipe all other sections.
// Instead, dMenu writes to its own INI, and we read from both:
//   - Non-dMenu settings: always from main INI
//   - dMenu-managed settings: from dMenu INI if it exists, else main INI
// =============================================================================
// GetMainIniPath() and GetDMenuIniPath() are defined in Globals.cpp (shared
// with the INI loaders there and with the `hg reload` console command).

// =============================================================================
// CONSOLIDATED GAME INIT - Shared logic for kNewGame and kPostLoadGame
// =============================================================================
// Both paths need the same subsystems initialized in the same order.
// Differences are gated on isNewGame:
//   - Registries: new game always rebuilds; load game reconciles if registry exists
//   - Load game calls Reset() on CandidateGenerator, UtilityScorer, OverrideManager
//   - Load game does TryConnect() + DestroyRecommendationWheels() before creating wheels
//   - Load game runs debug integration tests
// =============================================================================
static void InitializeGameSystems(bool isNewGame)
{
    // ── Stamp the load time FIRST ───────────────────────────────────────
    // Util::IsExtraListStable() measures from this stamp. The reconcile
    // scans below (step 3) run inside the post-load extra-data danger
    // window; if the stamp were written after them (or only at step 9),
    // they would measure against the PREVIOUS load's stamp — or the
    // default epoch on the first load — and the gate could never report
    // "unstable" on the exact path it exists to protect.
    g_lastGameLoad = std::chrono::steady_clock::now();
    logger::info("Game load timestamp recorded ({})"sv,
        isNewGame ? "kNewGame" : "kPostLoadGame");

    // ── 0. Parse the main INI ONCE, distribute to every settings loader ──
    // Each settings class used to re-open and re-parse Huginn.ini independently
    // (9+ full parses per game load). Parse it a single time here and hand the
    // parsed CSimpleIniA to each loader via LoadFromIni. When the file is
    // missing, haveMainIni is false and every loader keeps its current values —
    // matching the old per-call "not found, using defaults" early-return.
    CSimpleIniA mainIni;
    const bool haveMainIni = LoadIniFile(mainIni, GetMainIniPath(), "InitMain"sv);

    // ── 1. INI settings (before anything that depends on them) ──────────
    {
        auto& wheelerSettings = Wheeler::WheelerSettings::GetSingleton();
        if (haveMainIni) wheelerSettings.LoadFromIni(mainIni);
    }
    {
        auto& slotSettings = Slot::SlotSettings::GetSingleton();
        if (haveMainIni) slotSettings.LoadFromIni(mainIni);
    }

    // ── 2. Wheeler setup ────────────────────────────────────────────────
    {
        auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();

        // On save load, retry connection and destroy stale wheels
        if (!isNewGame) {
            if (!wheelerClient.IsConnected()) {
                wheelerClient.TryConnect();
            }
            if (wheelerClient.IsConnected()) {
                // Force clean slate - old wheel indices may be stale after save reload
                wheelerClient.DestroyRecommendationWheels();
            }
        }

        if (wheelerClient.IsConnected()) {
            wheelerClient.LogAPIInfo();
            if (wheelerClient.CreateRecommendationWheels()) {
                logger::info("Wheeler recommendation wheels created ({})"sv,
                    isNewGame ? "new game" : "save load");
            }
        }
    }

    // ── 3. Registries ───────────────────────────────────────────────────
    // SpellRegistry
    if (!g_spellRegistry) {
        g_spellRegistry = std::make_unique<Huginn::Spell::SpellRegistry>();
    }
    if (isNewGame) {
        g_spellRegistry->RebuildRegistry();
    } else {
        logger::info("Force reconciling spell registry on save load"sv);
        g_spellRegistry->ReconcileSpells();
    }

    // Issue #3: Use atomic flag to ensure single TESEquipEvent registration
    if (!g_equipEventRegistered.exchange(true)) {
        auto* eventSource = RE::ScriptEventSourceHolder::GetSingleton();
        if (eventSource) {
            eventSource->GetEventSource<RE::TESEquipEvent>()->AddEventSink(g_spellRegistry.get());
            eventSource->GetEventSource<RE::TESEquipEvent>()->AddEventSink(
                &Learning::ExternalEquipListener::GetSingleton());
            logger::info("SpellRegistry + ExternalEquipListener registered for TESEquipEvent notifications"sv);
        }
    }

    // ItemRegistry (v0.7.4)
    if (!g_itemRegistry) {
        g_itemRegistry = std::make_unique<Huginn::Item::ItemRegistry>();
    }
    if (isNewGame) {
        g_itemRegistry->RebuildRegistry();
    } else {
        logger::info("Force reconciling item registry on save load"sv);
        g_itemRegistry->ReconcileItems();
    }

    // WeaponRegistry (v0.7.6)
    if (!g_weaponRegistry) {
        g_weaponRegistry = std::make_unique<Huginn::Weapon::WeaponRegistry>();
    }
    // Track whether this pass could read extraLists: RebuildRegistry never
    // does (favorites/charge deferred by design), and a save-load reconcile
    // runs inside the stabilization window. Captured BEFORE the call so a
    // mid-scan stability flip errs toward the harmless extra retry. Used at
    // step 9 to prime the reconcile timer for a short retry.
    bool weaponScanDeferred = true;
    if (isNewGame) {
        g_weaponRegistry->RebuildRegistry();
    } else {
        weaponScanDeferred = !Huginn::Util::IsExtraListStable();
        logger::info("Force reconciling weapon registry on save load"sv);
        g_weaponRegistry->ReconcileWeapons();
    }

    // ScrollRegistry (v0.7.7)
    if (!g_scrollRegistry) {
        g_scrollRegistry = std::make_unique<Huginn::Scroll::ScrollRegistry>(g_spellRegistry->GetClassifier());
    }
    if (isNewGame) {
        g_scrollRegistry->RebuildRegistry();
    } else {
        logger::info("Force reconciling scroll registry on save load"sv);
        g_scrollRegistry->ReconcileScrolls();
    }

    // ── 4. CandidateConfig + CandidateGenerator ────────────────────────
    if (haveMainIni) LoadCandidateConfigFromINI(mainIni);
    {
        auto& candidateGen = Candidate::CandidateGenerator::GetSingleton();
        if (!candidateGen.IsInitialized()) {
            candidateGen.Initialize(*g_spellRegistry, *g_itemRegistry,
                                    *g_weaponRegistry, *g_scrollRegistry);
        }
    }

    // ── 5. FeatureQLearner + UtilityScorer + ScorerSettings ──────────────
    if (!g_featureQLearner) {
        g_featureQLearner = std::make_unique<Huginn::Learning::FeatureQLearner>();
        logger::info("FeatureQLearner initialized"sv);
    }

    // Apply any pending FQL cosave data (load game only — new game starts fresh)
    if (!isNewGame && Persist::HasPendingFQLData()) {
        if (Persist::ApplyPendingFQLData(*g_featureQLearner)) {
            logger::info("FeatureQLearner restored from cosave ({} items)"sv, g_featureQLearner->GetItemCount());
        }
    }

    if (!g_usageMemory) {
        g_usageMemory = std::make_unique<Huginn::Learning::UsageMemory>();
        logger::info("UsageMemory initialized"sv);
    }

    // ── 5b. EquipEventBus subscribers (once-only registration) ────────
    {
        static std::optional<Learning::FQLSubscriber> s_fqlSub;
        static std::optional<Learning::UsageMemorySubscriber> s_usageMemSub;
        static std::optional<Learning::CooldownSubscriber> s_cooldownSub;

        if (!s_fqlSub.has_value()) {
            s_fqlSub.emplace(*g_featureQLearner);
            s_usageMemSub.emplace(*g_usageMemory, *g_featureQLearner);
            s_cooldownSub.emplace();
            auto& bus = Learning::EquipEventBus::GetSingleton();
            bus.Subscribe(&*s_fqlSub);
            bus.Subscribe(&*s_usageMemSub);
            bus.Subscribe(&*s_cooldownSub);
            logger::info("EquipEventBus subscribers registered"sv);
        }
    }

    if (!g_utilityScorer) {
        g_utilityScorer = std::make_unique<Huginn::Scoring::UtilityScorer>(*g_featureQLearner, *g_usageMemory);
        logger::info("UtilityScorer initialized"sv);
    }

    {
        auto& scorerSettings = Scoring::ScorerSettings::GetSingleton();
        if (haveMainIni) scorerSettings.LoadFromIni(mainIni);
        g_utilityScorer->SetConfig(scorerSettings.BuildConfig());
    }

    // ── 6. ContextWeightSettings + WildcardConfig ──────────────────────
    if (haveMainIni) State::ContextWeightSettings::GetSingleton().LoadFromIni(mainIni);
    g_utilityScorer->SetContextWeightConfig(State::ContextWeightSettings::GetSingleton().BuildConfig());

    // ── 6b. LearningSettings ────────────────────────────────────────────
    if (haveMainIni) Learning::LearningSettings::GetSingleton().LoadFromIni(mainIni);
    Learning::ExternalEquipLearner::GetSingleton().SetConfig(
        Learning::LearningSettings::GetSingleton().BuildConfig());
    if (haveMainIni) LoadWildcardConfigFromINI(g_utilityScorer->GetWildcardManager(), mainIni);

    // ── 7. OverrideManager ─────────────────────────────────────────────
    {
        auto& settings = Override::Settings::GetSingleton();
        if (haveMainIni) settings.LoadFromIni(mainIni);

        auto& overrideMgr = Override::OverrideManager::GetSingleton();
        overrideMgr.Initialize(*g_itemRegistry, *g_weaponRegistry);
        logger::info("OverrideManager initialized ({})"sv,
            isNewGame ? "new game" : "save load");
    }

    // ── 7b. Slot allocator init + override placeability validation ─────
    // Must run after both SlotSettings (step 1) and Override::Settings
    // (step 7) are loaded so the placeability check sees the final config.
    // The reload path gets the same call via SettingsReloader::ApplySideEffects.
    Slot::SlotAllocator::GetSingleton().Initialize();

    // ── 8. Reset all stateful pipeline subsystems ────────────────────
    // Shared with Cmd_ResetAll — single source of truth for pipeline resets.
    ResetPipelineSubsystems();

    // ── 9. Timer reset ─────────────────────────────────────────────────
    // (g_lastGameLoad is stamped at the top of this function — the step 3
    // reconciles need it fresh before they run.)
    g_registryTimers.ResetAll(std::chrono::steady_clock::now());

    // The load-time weapon pass couldn't read extraLists (see step 3), so
    // favorites are unknown and enchanted weapons assumed full charge. Prime
    // the reconcile timer to come due just after stabilization instead of a
    // full interval — otherwise favorited weapons vanish from recommendations
    // for ~30 s after every load. Must run AFTER ResetAll, which would
    // otherwise overwrite the primed timestamp.
    if (weaponScanDeferred) {
        g_registryTimers.weaponReconcile.Reset(
            std::chrono::steady_clock::now() - std::chrono::milliseconds(static_cast<int64_t>(
                Config::WEAPON_RECONCILE_INTERVAL_MS - Config::WEAPON_RECONCILE_RETRY_MS)));
    }

    // ── 10. StateManager force update (debug only) ────────────────────
    // ResetTrackingState() already called by ResetPipelineSubsystems() above.
#ifdef _DEBUG
    State::StateManager::GetSingleton().ForceUpdate();
    logger::info("StateManager initial update complete ({})"sv,
        isNewGame ? "new game" : "save load");
#endif

    // ── 11. IntuitionMenu settings + show ──────────────────────────────
    // Parse the dMenu INI once (falls back to the main INI inside GetDMenuIniPath),
    // then distribute to the dMenu-managed loaders.
    CSimpleIniA dmenuIni;
    const bool haveDMenuIni = LoadIniFile(dmenuIni, GetDMenuIniPath(), "InitDMenu"sv);
    if (haveDMenuIni) UI::IntuitionSettings::GetSingleton().LoadFromIni(dmenuIni);
    UI::IntuitionMenu::Show();

    // ── 11b. Debug widget visibility (debug builds only) ───────────────
    if (haveDMenuIni) {
        auto& debugSettings = UI::DebugSettings::GetSingleton();
        debugSettings.LoadFromIni(dmenuIni);
        debugSettings.ApplyToWidgets();  // Phase-2 apply (LoadFromIni is now a pure loader)
    }

    // ── 12. Debug integration tests (load game only) ───────────────────
#ifndef NDEBUG
    if (!isNewGame) {
        g_spellRegistry->LogAllSpells();
        RunSpellRegistryTests();
        RunItemClassifierTests();
        RunItemRegistryTests();
        RunWeaponRegistryTests();
        RunMultiplicativeScoringTests();  // Stage 2d: Test multiplicative scoring formula
        RunRegressionTests();             // Regression suite for v1.0 refactor validation
        RunCosaveTests();                 // FeatureQLearner serialization round-trip
        RunStateFeaturesTests();          // Phase 3.5a: StateFeatures extraction tests
        RunFeatureQLearnerTests();        // Phase 3.5b: Feature-based Q-learner tests
        logger::info("Debug build ready. Console command functions available for hotkey integration"sv);
    }
#endif
}

// =============================================================================
// kDataLoaded handler — one-time engine/UI wiring (hooks, ImGui, input, update)
// =============================================================================
// Extracted from MessageHandler for readability. Guarded with a static flag so a
// duplicate kDataLoaded dispatch (SKSE guarantees one, but modded messaging can
// perturb it) can't re-install render/input hooks, leak the old g_stateEvaluator,
// or double-register menus and update callbacks.
static void OnDataLoaded()
{
    static bool s_dataLoadedDone = false;
    if (s_dataLoadedDone) {
        logger::warn("kDataLoaded fired again — ignoring (Huginn already initialized)"sv);
        return;
    }
    s_dataLoadedDone = true;

    auto timeElapsed = std::chrono::high_resolution_clock::now() - start;
    logger::info("time to main menu {}"sv, std::chrono::duration_cast<std::chrono::milliseconds>(timeElapsed).count());

    logger::info("Data loaded - Huginn ready"sv);

    // Register console commands (replaces unused command table entry)
    Huginn::Console::Register();

    // Install D3D11 render hook for ImGui
    if (UI::D3D11Hook::Install()) {
        logger::info("D3D11 render hook installed"sv);
    } else {
        logger::error("Failed to install D3D11 render hook"sv);
    }

#ifdef _DEBUG
    // Install input dispatch hook for interactive debug widgets (Home key toggle)
    if (UI::DebugInputHook::Install()) {
        logger::info("Debug input hook installed (Home key to toggle interaction)"sv);
    } else {
        logger::error("Failed to install debug input hook"sv);
    }
#endif

    // Initialize ImGui renderer
    if (UI::ImGuiRenderer::GetSingleton().Initialize()) {
        // Show welcome banner via ImGui
        UI::WelcomeBanner::GetSingleton().Show();
        logger::info("ImGui welcome banner triggered"sv);

#ifdef _DEBUG
        // Debug widget visibility is now controlled by DebugSettings (loaded from INI)
        // See InitializeGameSystems() -> DebugSettings::LoadFromFile()
        logger::info("Debug widgets will be initialized from INI settings (debug build)"sv);
#endif
    } else {
        // Fallback to DebugNotification
        std::string versionMsg = std::format("{} v{}", Plugin::NAME, Plugin::VERSION.string());
        RE::DebugNotification(versionMsg.c_str());
        logger::warn("ImGui init failed, using DebugNotification fallback: {}"sv, versionMsg);
    }

    // Initialize StateEvaluator
    g_stateEvaluator = std::make_unique<Huginn::State::StateEvaluator>();
    g_lastStateLog = std::chrono::steady_clock::now();
    logger::info("StateEvaluator initialized"sv);

    // Register DamageEventSink for instant damage type classification (v0.6.8)
    State::DamageEventSink::GetSingleton().Register();

    // Register InventoryExitTracker so drop/sell/store count decreases
    // are not rewarded as consumption (v0.7.22)
    Learning::InventoryExitTracker::GetSingleton().Register();

    // Register SettingsReloader for dMenu integration (v0.13.0)
    Settings::SettingsReloader::GetSingleton().Register();

    // Load debug widget visibility early so dMenu changes apply before game load
    UI::DebugSettings::GetSingleton().LoadFromFile(GetDMenuIniPath());

    // Try to connect to Wheeler API
    auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
    if (wheelerClient.TryConnect()) {
        wheelerClient.LogAPIInfo();
    } else {
        logger::info("Wheeler not available (will retry on game load)"sv);
    }

    // Run unit tests. This NDEBUG guard MUST stay in sync with src/CMakeLists.txt,
    // which excludes Tests.cpp from non-Debug configs (HEADER_FILE_ONLY). If a config
    // ever leaves NDEBUG undefined while still excluding Tests.cpp, this call becomes
    // an unresolved symbol. Standard MSVC Debug/Release presets keep them aligned.
#ifndef NDEBUG
    RunUnitTests();
#endif

    // Register UpdateHandler
    if (Update::UpdateHandler::Register()) {
        Update::UpdateHandler::GetSingleton()->SetUpdateCallback(OnUpdate);
        logger::info("Update system registered ({}ms interval)"sv, Config::UPDATE_INTERVAL_MS);
    } else {
        // Issue #4: Mark system as degraded so OnUpdate knows to warn periodically
        logger::error("[CRITICAL] Failed to register update handler - recommendations will not work!"sv);
        g_updateSystemFailed.store(true, std::memory_order_release);
        // Best-effort in-game surfacing. This fires at kDataLoaded (main menu), where
        // the HUD may not render a notification — so the CRITICAL log line above stays
        // the reliable signal; the notification is a bonus when a HUD is present.
        RE::DebugNotification("Huginn: update system failed to start - recommendations disabled. See log.");
    }

    // Register IntuitionMenu (Scaleform HUD widget)
    UI::IntuitionMenu::Register();

    // Register HUD visibility manager (auto-hide widget in menus)
    UI::HudVisibilityManager::Register();

    // Setup input callbacks for equip actions
    {
        auto& inputHandler = Input::InputHandler::GetSingleton();
        auto& equipManager = Input::EquipManager::GetSingleton();

        // Load keybindings from INI and configure InputHandler
        Input::KeybindingSettings keybindings;
        keybindings.LoadFromFile(GetDMenuIniPath());
        inputHandler.SetKeyCodes(keybindings);

        // Slot key callback: equip spell/item from slot
        inputHandler.SetSlotCallback([&equipManager](size_t slotIndex, Input::EquipHand hand) {
            equipManager.EquipSlot(slotIndex, hand);
        });

        // Cycle key callback: cycle pages (v0.12.0 multi-page support)
        inputHandler.SetCycleCallback([](bool isPrevious, bool isHold) {
            if (isHold) {
                // Hold action: reload/flush (future feature)
                logger::info("[Input] Cycle {} (hold - reload/flush)"sv,
                    isPrevious ? "previous" : "next");
                return;
            }

            // Tap action: switch pages
            auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
            if (isPrevious) {
                slotAllocator.PreviousPage();
            } else {
                slotAllocator.NextPage();
            }

            // Sync Wheeler wheel to match the new page (no-op if Wheeler not installed)
            auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
            wheelerClient.SetActivePage(slotAllocator.GetCurrentPage());

            // Widget will update automatically on next frame via the update handler
            logger::info("[Input] Switched to page {} '{}'"sv,
                slotAllocator.GetCurrentPage(),
                slotAllocator.GetCurrentPageName());
        });

        // Equip callback: publish to EquipEventBus (subscribers handle FQL + UsageMemory)
        // MarkHuginnEquip is already called in EquipManager.cpp before this callback
        equipManager.SetEquipCallback([](RE::FormID formID, bool wasRecommended) {
            Learning::EquipEventBus::GetSingleton().Publish(
                formID, Learning::EquipSource::Hotkey, 1.0f, wasRecommended);
        });

        logger::info("Input handler and equip manager initialized (keys 1-5)"sv);
    }
}

void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
    switch (a_msg->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        OnDataLoaded();
        break;
    case SKSE::MessagingInterface::kNewGame:
        logger::info("New game started"sv);
        InitializeGameSystems(/*isNewGame=*/true);
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
        logger::info("Game loaded"sv);
        InitializeGameSystems(/*isNewGame=*/false);
        break;
    default:
        break;
    }
}

void OpenLog()
{
    auto path = SKSE::log::log_directory();

    if (!path)
        return;

#ifndef NDEBUG
    *path /= "_Huginn_Debug.log";  // Underscore prefix sorts to top in debug builds
#else
    *path /= "Huginn.log";
#endif

    std::vector<spdlog::sink_ptr> sinks{
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true),
        std::make_shared<spdlog::sinks::msvc_sink_mt>()
    };

    auto logger_obj = std::make_shared<spdlog::logger>("global", sinks.begin(), sinks.end());

#ifndef NDEBUG
    logger_obj->set_level(spdlog::level::debug);
    logger_obj->flush_on(spdlog::level::debug);
#else
    logger_obj->set_level(spdlog::level::info);
    logger_obj->flush_on(spdlog::level::info);
#endif

    spdlog::set_default_logger(std::move(logger_obj));
    spdlog::set_pattern("[%Y-%m-%d %T.%e][%-16s:%-4#][%L]: %v");
}

extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() {
    SKSE::PluginVersionData v;

    v.PluginVersion(Plugin::VERSION);
    v.PluginName(Plugin::NAME);
    v.AuthorName("Huginn Team");
    v.UsesAddressLibrary(true);
    v.HasNoStructUse(true);
    v.CompatibleVersions({ SKSE::RUNTIME_SSE_LATEST });

    return v;
}();

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = Plugin::NAME.data();
    a_info->version = Plugin::VERSION[0];

    if (a_skse->IsEditor()) {
        logger::critical("Loaded in editor, marking as incompatible"sv);
        return false;
    }

    const auto ver = a_skse->RuntimeVersion();
    if (ver < SKSE::RUNTIME_SSE_1_5_39) {
        logger::critical(FMT_STRING("Unsupported runtime version {}"), ver.string());
        return false;
    }

    return true;
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    start = std::chrono::high_resolution_clock::now();
    OpenLog();

#ifdef Huginn_TRACY_ENABLED
    constexpr auto tracyTag = " [TRACY]"sv;
#else
    constexpr auto tracyTag = ""sv;
#endif
#ifndef NDEBUG
    logger::info("{} v{} ({}) [DEBUG BUILD]{} Loading"sv, Huginn::PROJECT_NAME, Huginn::VERSION_STRING, Huginn::GIT_COMMIT, tracyTag);
#else
    logger::info("{} v{} ({}) [RELEASE]{} Loading"sv, Huginn::PROJECT_NAME, Huginn::VERSION_STRING, Huginn::GIT_COMMIT, tracyTag);
#endif

    SKSE::Init(a_skse);

    // Register cosave serialization (must be before any save/load events)
    Persist::RegisterSerialization();
    logger::info("Cosave serialization registered"sv);

    const auto ver = REL::Module::get().version();
    logger::info("Skyrim runtime version: {}.{}.{}.{}"sv, ver.major(), ver.minor(), ver.patch(), ver.build());

    const auto messaging = SKSE::GetMessagingInterface();
    if (!messaging->RegisterListener("SKSE", MessageHandler)) {
        logger::error("Failed to register messaging interface listener"sv);
        return false;
    }

    logger::info("Huginn loaded successfully"sv);
    return true;
}
