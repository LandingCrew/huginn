#include "Globals.h"

#include "state/StateManager.h"
#include "candidate/CandidateConfig.h"
#include "candidate/CandidateGenerator.h"
#include "override/OverrideManager.h"
#include "slot/SlotAllocator.h"

#include <algorithm>
#include <filesystem>

using namespace Huginn;

// =============================================================================
// GLOBAL INSTANCE DEFINITIONS
// =============================================================================

std::chrono::high_resolution_clock::time_point start;

// State evaluator instance
std::unique_ptr<Huginn::State::StateEvaluator> g_stateEvaluator;

// Spell registry instance
std::unique_ptr<Huginn::Spell::SpellRegistry> g_spellRegistry;

// Feature-based Q-learner - Linear function approximation
std::unique_ptr<Huginn::Learning::FeatureQLearner> g_featureQLearner;

// Usage memory (v0.13.x) - Short-term situational recall for recency boosting
std::unique_ptr<Huginn::Learning::UsageMemory> g_usageMemory;

// Utility scoring system (v0.9.0) - Unified scorer for all candidate types
std::unique_ptr<Huginn::Scoring::UtilityScorer> g_utilityScorer;

// Timer for periodic state logging
std::chrono::steady_clock::time_point g_lastStateLog;

// Item registry instance (v0.7.4)
std::unique_ptr<Huginn::Item::ItemRegistry> g_itemRegistry;

// Weapon registry instance (v0.7.6)
std::unique_ptr<Huginn::Weapon::WeaponRegistry> g_weaponRegistry;

// Scroll registry instance (v0.7.7)
std::unique_ptr<Huginn::Scroll::ScrollRegistry> g_scrollRegistry;

// Registry maintenance timers (grouped via IntervalTimer)
RegistryTimers g_registryTimers;

void RegistryTimers::ResetAll() noexcept {
    auto now = std::chrono::steady_clock::now();
    ResetAll(now);
}

void RegistryTimers::ResetAll(std::chrono::steady_clock::time_point now) noexcept {
    spellReconcile.Reset(now);
    spellFavorites.Reset(now);
    itemDelta.Reset(now);
    itemReconcile.Reset(now);
    weaponCharge.Reset(now);
    weaponReconcile.Reset(now);
    scrollDelta.Reset(now);
    scrollReconcile.Reset(now);
}

// Global game load timestamp for extraLists stabilization guard (v0.7.9)
std::chrono::steady_clock::time_point g_lastGameLoad;

// Track whether we've shown the welcome notification
bool g_hasShownWelcomeNotification = false;

// Degraded mode flag - tracks if core update system failed to initialize
// (Issue #4: Silent failure when UpdateHandler::Register() fails leaves plugin non-functional)
std::atomic<bool> g_updateSystemFailed{false};

// =============================================================================
// GLOBAL ACCESSORS
// =============================================================================
namespace Huginn {
    State::StateEvaluator* GetStateEvaluator() { return g_stateEvaluator.get(); }
    Learning::FeatureQLearner* GetFeatureQLearner() { return g_featureQLearner.get(); }
    Learning::UsageMemory* GetUsageMemory() { return g_usageMemory.get(); }
}

// =============================================================================
// SHARED RESET: Pipeline Subsystems
// =============================================================================
// Single source of truth for resetting all stateful pipeline components.
// Called by InitializeGameSystems() on every game load/new game, and by
// Cmd_ResetAll for the console "oc reset all" command.
// Adding a new stateful subsystem? Add its reset here — both paths get it.

void ResetPipelineSubsystems() {
    if (g_utilityScorer) g_utilityScorer->Reset();
    if (g_usageMemory) g_usageMemory->Clear();

    Override::OverrideManager::GetSingleton().Reset();
    Candidate::CandidateGenerator::GetSingleton().Reset();

    Slot::SlotAllocator::GetSingleton().Initialize();
    auto& slotLocker = Slot::SlotLocker::GetSingleton();
    slotLocker.Reset();
    slotLocker.SetConfig(LoadSlotLockerConfigFromINI());

    State::StateManager::GetSingleton().ResetTrackingState();
}

// =============================================================================
// HELPER: Evaluate Current Game State
// =============================================================================

EvaluatedGameState EvaluateCurrentGameState() {
  auto& stateMgr = State::StateManager::GetSingleton();
  auto world = stateMgr.GetWorldState();
  auto playerState = stateMgr.GetPlayerState();
  auto targets = stateMgr.GetTargets();

  return {
    g_stateEvaluator->EvaluateCurrentState(world, playerState, targets),
    playerState
  };
}

// =============================================================================
// INI Loading Helpers
// =============================================================================

void LoadCandidateConfigFromINI() {
  const auto iniPath = std::filesystem::path("Data/SKSE/Plugins/Huginn.ini");
  if (!std::filesystem::exists(iniPath)) return;

  CSimpleIniA ini;
  ini.SetUnicode();
  if (ini.LoadFile(iniPath.string().c_str()) < 0) return;

  const char* section = "Candidates";

  // Uncastable spell policy (case-insensitive)
  const char* policyStr = ini.GetValue(section, "sUncastableSpellPolicy", "Disallow");
  std::string lower(policyStr);
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  if (lower == "allow") {
    Candidate::g_candidateConfig.uncastableSpellPolicy =
      Candidate::UncastableSpellPolicy::Allow;
  } else if (lower == "penalize") {
    Candidate::g_candidateConfig.uncastableSpellPolicy =
      Candidate::UncastableSpellPolicy::Penalize;
  } else {
    Candidate::g_candidateConfig.uncastableSpellPolicy =
      Candidate::UncastableSpellPolicy::Disallow;
  }

  Candidate::g_candidateConfig.uncastablePenaltyFloor = static_cast<float>(
    ini.GetDoubleValue(section, "fUncastablePenaltyFloor", 0.05));

  // Soul gem recharge toggle
  Candidate::g_candidateConfig.enableSoulGemRecharge =
    ini.GetBoolValue(section, "bEnableSoulGemRecharge", true);

  logger::info("[CandidateConfig] Uncastable spell policy: {}, penalty floor: {:.2f}, soul gem recharge: {}",
    Candidate::ToString(Candidate::g_candidateConfig.uncastableSpellPolicy),
    Candidate::g_candidateConfig.uncastablePenaltyFloor,
    Candidate::g_candidateConfig.enableSoulGemRecharge ? "enabled" : "disabled");
}

Slot::SlotLockConfig LoadSlotLockerConfigFromINI() {
  const auto iniPath = std::filesystem::path("Data/SKSE/Plugins/Huginn.ini");
  Slot::SlotLockConfig config;

  if (!std::filesystem::exists(iniPath)) return config;

  CSimpleIniA ini;
  ini.SetUnicode();
  if (ini.LoadFile(iniPath.string().c_str()) < 0) return config;

  const char* section = "SlotLocker";
  config.lockDurationMs = static_cast<float>(
    ini.GetDoubleValue(section, "fLockDurationMs", 3000.0));
  config.minLockDurationMs = static_cast<float>(
    ini.GetDoubleValue(section, "fMinLockDurationMs", 500.0));
  config.lockOnFill = ini.GetBoolValue(section, "bLockOnFill", true);
  config.overridesBreakLock = ini.GetBoolValue(section, "bOverridesBreakLock", true);
  config.immediateBreakPriority = static_cast<int>(
    ini.GetLongValue(section, "iImmediateBreakPriority", 50));

  logger::info("[SlotLockerConfig] duration={:.0f}ms, min={:.0f}ms, lockOnFill={}, overridesBreak={}, breakPri={}",
    config.lockDurationMs, config.minLockDurationMs,
    config.lockOnFill, config.overridesBreakLock, config.immediateBreakPriority);

  return config;
}

void LoadWildcardConfigFromINI(Scoring::WildcardManager& wildcardMgr) {
  const auto iniPath = std::filesystem::path("Data/SKSE/Plugins/Huginn.ini");
  if (!std::filesystem::exists(iniPath)) return;

  CSimpleIniA ini;
  ini.SetUnicode();
  if (ini.LoadFile(iniPath.string().c_str()) < 0) return;

  const char* section = "Wildcards";

  wildcardMgr.SetBaseProbability(static_cast<float>(
    ini.GetDoubleValue(section, "fBaseProbability", 0.165)));
  wildcardMgr.SetMaxProbability(static_cast<float>(
    ini.GetDoubleValue(section, "fMaxProbability", 0.5)));
  wildcardMgr.SetCooldown(static_cast<float>(
    ini.GetDoubleValue(section, "fCooldownSeconds", 30.0)));
  wildcardMgr.SetRefractoryPeriod(static_cast<float>(
    ini.GetDoubleValue(section, "fRefractorySeconds", 5.0)));

  logger::info("[WildcardConfig] base={:.3f}, max={:.2f}, cooldown={:.0f}s, refractory={:.0f}s",
    wildcardMgr.GetBaseProbability(), wildcardMgr.GetMaxProbability(),
    wildcardMgr.GetCooldown(), wildcardMgr.GetRefractoryPeriod());
}
