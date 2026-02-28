#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/spdlog.h>

#include "Config.h"
#include "Globals.h"
#include "UpdateLoop.h"
#include "Tests.h"

#include "state/StateManager.h"
#include "state/DamageEventSink.h"
#include "spell/SpellRegistry.h"
#include "learning/item/ItemRegistry.h"
#include "weapon/WeaponRegistry.h"
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
#include "state/ContextWeightSettings.h"
#include "state/ContextWeightConfig.h"
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
static std::filesystem::path GetMainIniPath()
{
    return std::filesystem::path("Data/SKSE/Plugins/Huginn.ini");
}

static std::filesystem::path GetDMenuIniPath()
{
    const auto dmenuIniPath = std::filesystem::path("Data/SKSE/Plugins/dmenu/customSettings/ini/Huginn.ini");

    try {
        if (std::filesystem::exists(dmenuIniPath)) {
            logger::debug("[Main] Using dmenu INI: {}"sv, dmenuIniPath.string());
            return dmenuIniPath;
        }
    } catch (const std::filesystem::filesystem_error& e) {
        logger::warn("[Main] Filesystem error checking dmenu INI: {}"sv, e.what());
    }

    logger::debug("[Main] dMenu INI not found, falling back to main INI"sv);
    return GetMainIniPath();
}

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
    // ── 1. INI settings (before anything that depends on them) ──────────
    {
        auto& wheelerSettings = Wheeler::WheelerSettings::GetSingleton();
        wheelerSettings.LoadFromFile(GetMainIniPath());
    }
    {
        auto& slotSettings = Slot::SlotSettings::GetSingleton();
        slotSettings.LoadFromFile(GetMainIniPath());
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
        g_spellRegistry->RebuildRegistry();
    } else if (!isNewGame) {
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
    if (isNewGame) {
        g_weaponRegistry->RebuildRegistry();
    } else {
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
    LoadCandidateConfigFromINI();
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
        scorerSettings.LoadFromFile(GetMainIniPath());
        g_utilityScorer->SetConfig(scorerSettings.BuildConfig());
    }

    // ── 6. ContextWeightSettings + WildcardConfig ──────────────────────
    State::ContextWeightSettings::GetSingleton().LoadFromFile(GetMainIniPath());
    g_utilityScorer->SetContextWeightConfig(State::ContextWeightSettings::GetSingleton().BuildConfig());

    // ── 6b. LearningSettings ────────────────────────────────────────────
    Learning::LearningSettings::GetSingleton().LoadFromFile(GetMainIniPath());
    Learning::ExternalEquipLearner::GetSingleton().SetConfig(
        Learning::LearningSettings::GetSingleton().BuildConfig());
    LoadWildcardConfigFromINI(g_utilityScorer->GetWildcardManager());

    // ── 7. OverrideManager ─────────────────────────────────────────────
    {
        auto& settings = Override::Settings::GetSingleton();
        settings.LoadFromFile(GetMainIniPath());

        auto& overrideMgr = Override::OverrideManager::GetSingleton();
        overrideMgr.Initialize(*g_itemRegistry, *g_spellRegistry, *g_weaponRegistry);
        logger::info("OverrideManager initialized ({})"sv,
            isNewGame ? "new game" : "save load");
    }

    // ── 8. Reset all stateful pipeline subsystems ────────────────────
    // Shared with Cmd_ResetAll — single source of truth for pipeline resets.
    ResetPipelineSubsystems();

    // ── 9. Timer reset ─────────────────────────────────────────────────
    auto now = std::chrono::steady_clock::now();
    g_registryTimers.ResetAll(now);
    g_lastGameLoad = now;
    logger::info("Game load timestamp recorded ({})"sv,
        isNewGame ? "kNewGame" : "kPostLoadGame");

    // ── 10. StateManager force update (debug only) ────────────────────
    // ResetTrackingState() already called by ResetPipelineSubsystems() above.
#ifdef _DEBUG
    State::StateManager::GetSingleton().ForceUpdate();
    logger::info("StateManager initial update complete ({})"sv,
        isNewGame ? "new game" : "save load");
#endif

    // ── 11. IntuitionMenu settings + show ──────────────────────────────
    UI::IntuitionSettings::GetSingleton().LoadFromFile(GetDMenuIniPath());
    UI::IntuitionMenu::Show();

    // ── 11b. Debug widget visibility (debug builds only) ───────────────
    UI::DebugSettings::GetSingleton().LoadFromFile(GetDMenuIniPath());

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

void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
    switch (a_msg->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        {
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

            // Run unit tests
            RunUnitTests();

            // Register UpdateHandler
            if (Update::UpdateHandler::Register()) {
                Update::UpdateHandler::GetSingleton()->SetUpdateCallback(OnUpdate);
                logger::info("Update system registered ({}ms interval)"sv, Config::UPDATE_INTERVAL_MS);
            } else {
                // Issue #4: Mark system as degraded so OnUpdate knows to warn periodically
                logger::error("[CRITICAL] Failed to register update handler - recommendations will not work!"sv);
                g_updateSystemFailed.store(true, std::memory_order_release);
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
                inputHandler.SetKeyCodes(
                    keybindings.slot1Key, keybindings.slot2Key, keybindings.slot3Key,
                    keybindings.slot4Key, keybindings.slot5Key, keybindings.slot6Key,
                    keybindings.slot7Key, keybindings.slot8Key, keybindings.slot9Key,
                    keybindings.slot10Key, keybindings.prevPageKey, keybindings.nextPageKey
                );

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

            break;
        }
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

#ifndef NDEBUG
    logger::info("{} v{} ({}) [DEBUG BUILD] Loading"sv, Huginn::PROJECT_NAME, Huginn::VERSION_STRING, Huginn::GIT_COMMIT);
#else
    logger::info("{} v{} ({}) Loading"sv, Huginn::PROJECT_NAME, Huginn::VERSION_STRING, Huginn::GIT_COMMIT);
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
