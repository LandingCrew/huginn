#pragma once

#include <chrono>
#include <memory>
#include <atomic>

#include "state/StateEvaluator.h"
#include "state/GameState.h"
#include "state/PlayerActorState.h"
#include "spell/SpellRegistry.h"
#include "learning/item/ItemRegistry.h"
#include "weapon/WeaponRegistry.h"
#include "scroll/ScrollRegistry.h"
#include "learning/FeatureQLearner.h"
#include "learning/UsageMemory.h"
#include "learning/UtilityScorer.h"
#include "slot/SlotLocker.h"
#include "util/IntervalTimer.h"

// =============================================================================
// GLOBAL INSTANCES - Declared here, defined in Globals.cpp
// =============================================================================

extern std::chrono::high_resolution_clock::time_point start;

// Core system instances
extern std::unique_ptr<Huginn::State::StateEvaluator> g_stateEvaluator;
extern std::unique_ptr<Huginn::Spell::SpellRegistry> g_spellRegistry;
extern std::unique_ptr<Huginn::Learning::FeatureQLearner> g_featureQLearner;
extern std::unique_ptr<Huginn::Learning::UsageMemory> g_usageMemory;
extern std::unique_ptr<Huginn::Scoring::UtilityScorer> g_utilityScorer;
extern std::unique_ptr<Huginn::Item::ItemRegistry> g_itemRegistry;
extern std::unique_ptr<Huginn::Weapon::WeaponRegistry> g_weaponRegistry;
extern std::unique_ptr<Huginn::Scroll::ScrollRegistry> g_scrollRegistry;

// Registry maintenance timers (grouped via IntervalTimer)
struct RegistryTimers {
    Huginn::Util::IntervalTimer spellReconcile;
    Huginn::Util::IntervalTimer spellFavorites;
    Huginn::Util::IntervalTimer itemDelta;
    Huginn::Util::IntervalTimer itemReconcile;
    Huginn::Util::IntervalTimer weaponCharge;
    Huginn::Util::IntervalTimer weaponReconcile;
    Huginn::Util::IntervalTimer scrollDelta;
    Huginn::Util::IntervalTimer scrollReconcile;

    void ResetAll() noexcept;
    void ResetAll(std::chrono::steady_clock::time_point now) noexcept;
};
extern RegistryTimers g_registryTimers;

// Non-registry timers (kept separate — different lifecycle)
extern std::chrono::steady_clock::time_point g_lastStateLog;
extern std::chrono::steady_clock::time_point g_lastGameLoad;

// Flags
extern bool g_hasShownWelcomeNotification;
extern std::atomic<bool> g_updateSystemFailed;

// =============================================================================
// HELPER: Evaluate Current Game State
// =============================================================================

/// @brief Helper struct to return both GameState and PlayerActorState together
/// @details Reduces code duplication across multiple call sites
struct EvaluatedGameState {
  Huginn::State::GameState gameState;
  Huginn::State::PlayerActorState playerState;
};

/// @brief Fetches state from StateManager and evaluates current GameState
/// @return Struct containing evaluated GameState and PlayerActorState
/// @note C++17 structured bindings: auto [state, player] = EvaluateCurrentGameState();
[[nodiscard]] EvaluatedGameState EvaluateCurrentGameState();

/// @brief Reset all stateful pipeline subsystems to a clean state.
/// @details Shared by InitializeGameSystems() and Cmd_ResetAll to prevent
/// the reset and init sequences from silently diverging.
/// Resets: UtilityScorer, UsageMemory, OverrideManager, CandidateGenerator,
///         SlotAllocator, SlotLocker (with INI config), StateManager tracking.
void ResetPipelineSubsystems();

/// @brief Load [Candidates] section settings from Huginn.ini into g_candidateConfig.
/// @note Called during both kNewGame and kPostLoadGame events.
void LoadCandidateConfigFromINI();

/// @brief Load [Wildcards] section settings from Huginn.ini into the WildcardManager.
/// @note Called during both kNewGame and kPostLoadGame events, after UtilityScorer is created.
void LoadWildcardConfigFromINI(Huginn::Scoring::WildcardManager& wildcardMgr);

/// @brief Load [SlotLocker] section settings from Huginn.ini.
/// @note Called during kNewGame, kPostLoadGame, and hg reload.
[[nodiscard]] Huginn::Slot::SlotLockConfig LoadSlotLockerConfigFromINI();

// =============================================================================
// GLOBAL ACCESSORS - Safe access to global systems from other translation units
// =============================================================================
namespace Huginn {
    State::StateEvaluator* GetStateEvaluator();
    Learning::FeatureQLearner* GetFeatureQLearner();
    Learning::UsageMemory* GetUsageMemory();
}
