#include "Globals.h"

#include "state/StateManager.h"
#include "candidate/CandidateConfig.h"
#include "candidate/CandidateGenerator.h"
#include "override/OverrideManager.h"
#include "slot/SlotAllocator.h"
#include "slot/Remembrance.h"
#include "input/HandEquipGate.h"
#include "learning/SelectionTracker.h"
#include "learning/SelectionLogV3.h"
#include "pipeline/PipelineCoordinator.h"
#include "learning/legacy/LegacyScorer.h"   // g_legacyScorer's destructor
#include "core/WildcardGate.h"   // GateSettings: the [Wildcards] clocks' defaults
#include "core/WildcardPick.h"   // Params: the [Wildcards] odds' defaults
#include "core/ChallengerRule.h"   // the combat upsert confidence range (0.24.13)

#include <algorithm>
#include <filesystem>

using namespace Huginn;

// =============================================================================
// GLOBAL INSTANCE DEFINITIONS
// =============================================================================

std::chrono::high_resolution_clock::time_point g_pluginLoadStart;

// State evaluator instance
std::unique_ptr<Huginn::State::StateEvaluator> g_stateEvaluator;

// Spell registry instance
std::unique_ptr<Huginn::Spell::SpellRegistry> g_spellRegistry;

// Feature-based bandit learner - Linear function approximation
std::unique_ptr<Huginn::Learning::FeatureBanditLearner> g_featureBanditLearner;

// Usage memory (v0.13.x) - Short-term situational recall for recency boosting
std::unique_ptr<Huginn::Learning::UsageMemory> g_usageMemory;
std::unique_ptr<Huginn::Learning::ChoiceLearner> g_choiceLearner;

// Utility scoring system (v0.9.0) - Unified scorer for all candidate types
std::unique_ptr<Huginn::Scoring::UtilityScorer> g_utilityScorer;

// R8.4: the legacy scorer (the D2 feature's input; learning/legacy/LegacyScorer.h)
std::unique_ptr<Huginn::Scoring::Legacy::LegacyScorer> g_legacyScorer;

// Item registry instance (v0.7.4)
std::unique_ptr<Huginn::Item::ItemRegistry> g_itemRegistry;

// Weapon registry instance (v0.7.6)
std::unique_ptr<Huginn::Weapon::WeaponRegistry> g_weaponRegistry;

// Scroll registry instance (v0.7.7)
std::unique_ptr<Huginn::Scroll::ScrollRegistry> g_scrollRegistry;
std::unique_ptr<Huginn::Apparel::ApparelRegistry> g_apparelRegistry;

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
    apparelReconcile.Reset(now);
}

// Global game load timestamp for extraLists stabilization guard (v0.7.9)
std::atomic<std::chrono::steady_clock::time_point> g_lastGameLoad{};
std::atomic<uint32_t> g_loadGeneration{ 0 };
std::string g_launchStamp;
std::string g_listName;
std::atomic<uint64_t> g_activeCharacterID{ 0 };
Huginn::Core::LearningClock::Clock g_learningClock;

// Degraded mode flag - tracks if core update system failed to initialize
// (Issue #4: Silent failure when UpdateHandler::Register() fails leaves plugin non-functional)
std::atomic<bool> g_updateSystemFailed{false};

// =============================================================================
// GLOBAL ACCESSORS
// =============================================================================
namespace Huginn {
    State::StateEvaluator* GetStateEvaluator() { return g_stateEvaluator.get(); }
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
    // R8.3: NOT the choice learner. A load restores it from the cosave
    // (Persist::ResolveLoadedChoice, InitializeGameSystems step 6c, before
    // this runs at step 8), and `hg reset all` / `hg reset weights` / the
    // dMenu reset return it to the bootstrap themselves
    // (SettingsReloader::ResetLearningData).

    Override::OverrideManager::GetSingleton().Reset();
    Candidate::CandidateGenerator::GetSingleton().Reset();

    // Reset() (not Initialize()) — resets to page 0 AND clears the log-dedup
    // caches so first-time "no candidate"/unplaced-override messages aren't
    // suppressed across a game load.
    Slot::SlotAllocator::GetSingleton().Reset();
    // The locks only: Reset keeps the [SlotLocker] config, which the caller
    // owns (InitializeGameSystems loads it from the INI it parsed; a hot
    // reload or Reset to Defaults sets it). `hg reset all` keeps the config in
    // force: re-reading Huginn.ini here brought the file's lock settings back
    // over a Reset to Defaults that could not write it (0.24.18).
    Slot::SlotLocker::GetSingleton().Reset();
    Slot::Remembrance::GetSingleton().Reset();

    // An equip parked for a busy hand belongs to the abandoned session too
    // (0.24.26); dropped first, so its selection is withdrawn before the
    // tracker forgets everything anyway.
    Input::HandEquipGate::GetSingleton().Drop("reset (a game load or hg reset all)", /*force=*/true);
    // A selection pending across a load belongs to the abandoned session.
    Learning::SelectionTracker::GetSingleton().Clear();
    // R4: the v3 log's episodes, pending picks and menu context likewise.
    Learning::SelectionLogV3::Reset();

    Pipeline::PipelineCoordinator::GetSingleton().ResetCrossSaveState();
    // The bucket hysteresis remembers the last character's HP/MP/SP and
    // distance bands; the next one is bucketed from its own raw values.
    if (g_stateEvaluator) {
        g_stateEvaluator->ResetBucketMemory();
    }

    State::StateManager::GetSingleton().ResetTrackingState();
}

// =============================================================================
// INI Loading Helpers
// =============================================================================

std::filesystem::path GetMainIniPath() {
  return std::filesystem::path("Data/SKSE/Plugins/Huginn.ini");
}

std::filesystem::path GetDMenuOwnIniPath() {
  return std::filesystem::path("Data/SKSE/Plugins/dmenu/customSettings/ini/Huginn.ini");
}

std::filesystem::path GetDMenuIniPath() {
  const auto dmenuIniPath = GetDMenuOwnIniPath();

  try {
    if (std::filesystem::exists(dmenuIniPath)) {
      logger::debug("[Globals] Using dmenu INI: {}"sv, dmenuIniPath.string());
      return dmenuIniPath;
    }
  } catch (const std::filesystem::filesystem_error& e) {
    logger::warn("[Globals] Filesystem error checking dmenu INI: {}"sv, e.what());
  }

  logger::debug("[Globals] dMenu INI not found, falling back to main INI"sv);
  return GetMainIniPath();
}

bool LoadIniFile(CSimpleIniA& out, const std::filesystem::path& path, std::string_view tag,
                 IniMissing missing) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    if (missing == IniMissing::Warn) {
      logger::warn("[{}] INI file not found, using defaults: {}"sv, tag, path.string());
    } else {
      logger::info("[{}] INI file not found, using defaults: {}"sv, tag, path.string());
    }
    return false;
  }

  out.SetUnicode();
  if (out.LoadFile(path.string().c_str()) < 0) {
    logger::error("[{}] Failed to load INI file: {}"sv, tag, path.string());
    return false;
  }
  return true;
}

void LoadCandidateConfigFromINI(const CSimpleIniA& ini) {
  const char* section = "Candidates";

  // Uncastable spell policy (case-insensitive)
  const char* policyStr = ini.GetValue(section, "sUncastableSpellPolicy", "Disallow");
  std::string lower(policyStr);
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  // "Penalize" was removed: it never had a penalty behind it and behaved as
  // Allow, so an INI that still says it keeps the behaviour it always had.
  if (lower == "allow" || lower == "penalize") {
    if (lower == "penalize")
      logger::info("[Candidates] sUncastableSpellPolicy = Penalize is no longer supported; using Allow"sv);
    Candidate::g_candidateConfig.uncastableSpellPolicy =
      Candidate::UncastableSpellPolicy::Allow;
  } else {
    Candidate::g_candidateConfig.uncastableSpellPolicy =
      Candidate::UncastableSpellPolicy::Disallow;
  }

  // Soul gem recharge toggle
  Candidate::g_candidateConfig.enableSoulGemRecharge =
    ini.GetBoolValue(section, "bEnableSoulGemRecharge", true);

  logger::info("[CandidateConfig] Uncastable spell policy: {}, soul gem recharge: {}",
    Candidate::ToString(Candidate::g_candidateConfig.uncastableSpellPolicy),
    Candidate::g_candidateConfig.enableSoulGemRecharge ? "enabled" : "disabled");
}

Slot::SlotLockConfig LoadSlotLockerConfigFromINI(const CSimpleIniA& ini) {
  Slot::SlotLockConfig config;

  const char* section = "SlotLocker";
  config.lockDurationMs = static_cast<float>(
    ini.GetDoubleValue(section, "fLockDurationMs", 3000.0));
  config.minLockDurationMs = static_cast<float>(
    ini.GetDoubleValue(section, "fMinLockDurationMs", 500.0));
  config.lockOnFill = ini.GetBoolValue(section, "bLockOnFill", true);
  config.overridesBreakLock = ini.GetBoolValue(section, "bOverridesBreakLock", true);
  config.immediateBreakPriority = static_cast<int>(
    ini.GetLongValue(section, "iImmediateBreakPriority", 50));
  // -1 (freeze until combat ends) or 0-120000; anything negative reads as -1,
  // a NaN as the default.
  {
    const double combat = ini.GetDoubleValue(section, "fCombatLockDurationMs", -1.0);
    config.combatLockDurationMs = !std::isfinite(combat) ? -1.0f
      : combat < 0.0 ? -1.0f
      : static_cast<float>(std::min(combat, 120000.0));
    const double noEnemy = ini.GetDoubleValue(section, "fCombatNoEnemyReleaseSec", 5.0);
    config.combatNoEnemyReleaseSec = !std::isfinite(noEnemy) ? 5.0f
      : static_cast<float>(std::clamp(noEnemy, 0.0, 60.0));
    // Combat upsert (core/CombatUpsert.h): 0-20 (0 = off), cooldown 0-300 s.
    const double upsertMult = ini.GetDoubleValue(section, "fCombatUpsertMarginMult", 3.0);
    config.combatUpsertMarginMult = !std::isfinite(upsertMult) ? 3.0f
      : static_cast<float>(std::clamp(upsertMult, 0.0, 20.0));
    const double upsertCooldown = ini.GetDoubleValue(section, "fCombatUpsertCooldownSec", 10.0);
    config.combatUpsertCooldownSec = !std::isfinite(upsertCooldown) ? 10.0f
      : static_cast<float>(std::clamp(upsertCooldown, 0.0, 300.0));
    // 0.24.13: the upsert's confidence under the choice ranking, clamped to
    // the challenger rule's range (core/ChallengerRule.h) as
    // fChallengerConfidence is; NaN or +-inf reads as the default.
    const double upsertConfidence = ini.GetDoubleValue(section, "fCombatChallengerConfidence", 0.995);
    config.combatChallengerConfidence = !std::isfinite(upsertConfidence) ? 0.995f
      : static_cast<float>(std::clamp(upsertConfidence, Core::Challenger::kMinConfidence,
          Core::Challenger::kMaxConfidence));
  }

  logger::info("[SlotLockerConfig] duration={:.0f}ms, min={:.0f}ms, lockOnFill={}, overridesBreak={}, breakPri={}, combat={}",
    config.lockDurationMs, config.minLockDurationMs,
    config.lockOnFill, config.overridesBreakLock, config.immediateBreakPriority,
    config.combatLockDurationMs < 0.0f ? std::string("freeze")
      : std::format("{:.0f}ms", config.combatLockDurationMs));
  logger::info("[SlotLockerConfig] combat lock lets go after {:.1f}s with no enemy (0 = never)",
    config.combatNoEnemyReleaseSec);
  if (config.combatUpsertMarginMult > 0.0f) {
    logger::info("[SlotLockerConfig] combat upsert: a challenger better than a combat-locked key's item with P > {:.3f} (z {:.3f}; with no belief, beating it by {:.1f} x ln(1 + fChallengerMargin)) takes it, at most once per key per {:.0f}s",
      config.combatChallengerConfidence,
      Core::Challenger::ZForConfidence(static_cast<double>(config.combatChallengerConfidence)),
      config.combatUpsertMarginMult, config.combatUpsertCooldownSec);
  } else {
    logger::info("[SlotLockerConfig] combat upsert: off");
  }

  return config;
}

void LoadWildcardConfigFromINI(Scoring::WildcardManager& wildcardMgr, const CSimpleIniA& ini) {
  const char* section = "Wildcards";
  // `ini` is the main INI over the shipped Huginn.ini (Settings::LoadMainIni),
  // or the shipped file alone on dMenu's Reset to Defaults, so these keys are
  // always there. The core structs' defaults (equal to the shipped values)
  // apply only to a key in neither file.
  constexpr Core::Wildcard::Params odds{};
  constexpr Core::Wildcard::GateSettings clocks{};

  wildcardMgr.SetBaseProbability(static_cast<float>(
    ini.GetDoubleValue(section, "fBaseProbability", odds.baseRoll)));
  wildcardMgr.SetMaxProbability(static_cast<float>(
    ini.GetDoubleValue(section, "fMaxProbability", odds.maxRoll)));
  // R10: the random term's share of the draw (core/WildcardPick.h).
  wildcardMgr.SetUniformShare(static_cast<float>(
    ini.GetDoubleValue(section, "fUniformShare", odds.uniformShare)));
  wildcardMgr.SetCooldown(static_cast<float>(
    ini.GetDoubleValue(section, "fCooldownSeconds", clocks.cooldownSec)));
  wildcardMgr.SetRefractoryPeriod(static_cast<float>(
    ini.GetDoubleValue(section, "fRefractorySeconds", clocks.refractorySec)));

  logger::info("[WildcardConfig] roll {:.3f}..{:.2f} (base..max), uniform share {:.2f}, cooldown={:.0f}s, "
    "refractory={:.0f}s",
    wildcardMgr.GetBaseProbability(), wildcardMgr.GetMaxProbability(), wildcardMgr.GetUniformShare(),
    wildcardMgr.GetCooldown(), wildcardMgr.GetRefractoryPeriod());
}
