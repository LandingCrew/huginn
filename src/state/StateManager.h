#pragma once

#include "WorldState.h"
#include "PlayerActorState.h"
#include "TargetActorState.h"
#include "StateTypes.h"              // For HealthTrackingState
#include "StateManagerConstants.h"
#include "DamageEventSink.h"         // For instant damage classification (v0.6.8)
#include <array>
#include <atomic>
#include <shared_mutex>

namespace Huginn::State
{
   // =============================================================================
   // STATE MANAGER (v0.6.1)
   // =============================================================================
   // Central coordinator for all game state polling and management.
   // Replaces the sensor-based architecture (ContextSensor + 9 sensors) with
   // a coarse-grained state model (3 state structs: World, Player, Targets).
   //
   // ARCHITECTURE:
   // - 3 state models (WorldState, PlayerActorState, TargetCollection)
   // - 11 poll methods
   // - 3 locks (down from 7 sensor locks)
   //
   // THREAD SAFETY:
   // - Copy-out pattern for all state accessors (thread-safe reads)
   // - Short critical sections (minimize lock contention)
   //
   // PERFORMANCE TARGET:
   // - Update() ≤ 0.5ms per call
   // - Target tracking overhead ≤ 0.1ms
   // - Memory overhead ≤ 10KB
   //
   // USAGE:
   //   auto& mgr = StateManager::GetSingleton();
   //   mgr.Update(deltaMs);
   //   WorldState world = mgr.GetWorldState();  // Thread-safe copy-out
   //   PlayerActorState player = mgr.GetPlayerState();
   // =============================================================================

   class StateManager
   {
   public:
      // Singleton accessor
      static StateManager& GetSingleton() {
      static StateManager instance;
      return instance;
      }

      // Delete copy/move constructors
      StateManager(const StateManager&) = delete;
      StateManager(StateManager&&) = delete;
      StateManager& operator=(const StateManager&) = delete;
      StateManager& operator=(StateManager&&) = delete;

      // =============================================================================
      // UPDATE METHODS
      // =============================================================================

      // Update state based on elapsed time (milliseconds)
      // Polls each sensor if its timer has expired
      // PERFORMANCE: Target ≤ 0.5ms per call
      void Update(float deltaMs);

      // Force immediate update of all state (ignore timers)
      // Used for initialization or debug commands
      void ForceUpdate();

      // Reset all accumulated tracking state (damage history, elemental timers,
      // sub-threshold accumulators, target hysteresis, etc.)
      // Must be called on save load / new game to prevent stale death-state data
      // from persisting into the new session.
      void ResetTrackingState();

      // Stage 3b: Poll all sensors and return true if ANY state changed
      // Ignores timers - meant to be called when timer-based poll is due
      // Returns: true if any sensor detected a change, false if all unchanged
      [[nodiscard]] bool PollAll();

      // Stage 3c: Check if last Update() detected any state changes
      // Returns: true if last Update() changed any state, false otherwise
      // Used by UpdateLoop to skip expensive pipeline when state is stable
      [[nodiscard]] bool DidLastUpdateChangeState() const noexcept { return m_lastUpdateChanged; }

      // =============================================================================
      // COMBAT TRANSITION TRACKING
      // =============================================================================
      // Detected inside PollPlayerPosition (which already holds the lock and
      // knows exactly when isInCombat flips). Avoids the full PlayerActorState
      // copy that the old UpdateLoop combat tracking required.

      enum class CombatTransition : uint8_t { None, Entered, Exited };

      /// Consume and reset the combat transition flag (destructive read).
      /// Returns the transition that occurred since the last call.
      [[nodiscard]] CombatTransition ConsumeCombatTransition() noexcept
      {
         return m_combatTransition.exchange(CombatTransition::None, std::memory_order_acq_rel);
      }

      /// Lightweight combat state check (no full PlayerActorState copy).
      [[nodiscard]] bool IsInCombat() const noexcept
      {
         return m_isInCombat.load(std::memory_order_acquire);
      }

      // =============================================================================
      // STATE ACCESSORS (Thread-safe via copy-out pattern)
      // =============================================================================

      // Get world state (copy-out)
      [[nodiscard]] WorldState GetWorldState() const noexcept;

      // Get player state (copy-out)
      [[nodiscard]] PlayerActorState GetPlayerState() const noexcept;

      // Get all targets (copy-out)
      [[nodiscard]] TargetCollection GetTargets() const noexcept;

      // Get primary target only (copy-out)
      [[nodiscard]] std::optional<TargetActorState> GetPrimaryTarget() const;

      // Get health tracking state (copy-out)
      [[nodiscard]] HealthTrackingState GetHealthTracking() const noexcept;

      // Get stamina tracking state (copy-out) (v0.6.9)
      [[nodiscard]] StaminaTrackingState GetStaminaTracking() const noexcept;

      // Get magicka tracking state (copy-out) (v0.6.9)
      [[nodiscard]] MagickaTrackingState GetMagickaTracking() const noexcept;

      // =============================================================================
      // CONFIGURATION (Optional - defaults from StateManagerConstants.h)
      // =============================================================================

      // Set poll intervals (milliseconds)
      void SetWorldPollInterval(float ms) noexcept { m_worldPollInterval = ms; }
      void SetPlayerVitalsPollInterval(float ms) noexcept { m_playerVitalsPollInterval = ms; }
      void SetPlayerMagicEffectsPollInterval(float ms) noexcept { m_playerMagicEffectsPollInterval = ms; }
      void SetPlayerEquipmentPollInterval(float ms) noexcept { m_playerEquipmentPollInterval = ms; }
      void SetPlayerSurvivalPollInterval(float ms) noexcept { m_playerSurvivalPollInterval = ms; }
      void SetPlayerPositionPollInterval(float ms) noexcept { m_playerPositionPollInterval = ms; }
      void SetTargetsPollInterval(float ms) noexcept { m_targetsPollInterval = ms; }

      // Get poll intervals
      [[nodiscard]] float GetWorldPollInterval() const noexcept { return m_worldPollInterval; }
      [[nodiscard]] float GetPlayerVitalsPollInterval() const noexcept { return m_playerVitalsPollInterval; }
      [[nodiscard]] float GetPlayerMagicEffectsPollInterval() const noexcept { return m_playerMagicEffectsPollInterval; }
      [[nodiscard]] float GetPlayerEquipmentPollInterval() const noexcept { return m_playerEquipmentPollInterval; }
      [[nodiscard]] float GetPlayerSurvivalPollInterval() const noexcept { return m_playerSurvivalPollInterval; }
      [[nodiscard]] float GetPlayerPositionPollInterval() const noexcept { return m_playerPositionPollInterval; }
      [[nodiscard]] float GetTargetsPollInterval() const noexcept { return m_targetsPollInterval; }

   private:
      StateManager();
      ~StateManager() = default;

      // =============================================================================
      // POLL TABLE (Data-driven poll loop)
      // =============================================================================

      struct PollEntry {
         float* timer;
         float* interval;
         bool (StateManager::*poll)();
      };

      static constexpr size_t kPollEntryCount = 11;
      std::array<PollEntry, kPollEntryCount> GetPollTable() noexcept;

      // =============================================================================
      // HELPER METHODS
      // =============================================================================

      // Update state if changed (thread-safe with early-exit optimization)
      //
      // PERFORMANCE: Avoids copy assignment when state is unchanged, which is
      // beneficial for large state structs (TargetCollection can be several KB).
      //
      // THREAD SAFETY: Acquires unique lock for compare-and-swap. Holding the lock
      // during comparison prevents race conditions with concurrent readers.
      //
      // Template helper to eliminate code duplication across poll methods.
      // Returns true if state changed (optional - used for debug logging).
      template <typename StateType>
      bool UpdateStateIfChanged(std::shared_mutex& mutex, StateType& currentState,
                                const StateType& newState) noexcept {
      std::unique_lock lock(mutex);
      if (currentState != newState) {
        currentState = newState;
        return true;
      }
      return false;
      }

      // =============================================================================
      // SENSOR POLLING METHODS (Private implementation)
      // =============================================================================
      // Stage 3b: All poll methods now return bool indicating if state changed

      // World state polling (locks, ore veins, time, light)
      // Updates: WorldState
      // Returns: true if state changed
      [[nodiscard]] bool PollWorldObjects();

      // Player vitals polling (health, magicka, stamina)
      // Updates: PlayerActorState.vitals
      // Returns: true if state changed
      [[nodiscard]] bool PollPlayerVitals();

      // Player magic effects polling (damage effects + buffs)
      // Updates: PlayerActorState.effects, PlayerActorState.buffs
      // NOTE: Single iteration optimization (merged EffectsSensor + ActiveBuffsSensor)
      // Returns: true if state changed
      [[nodiscard]] bool PollPlayerMagicEffects();

      // Player equipment polling (weapons, ammo, charge)
      // Updates: PlayerActorState equipment fields
      // Returns: true if state changed
      [[nodiscard]] bool PollPlayerEquipment();

      // Player survival polling (hunger, cold, fatigue)
      // Updates: PlayerActorState survival fields
      // Returns: true if state changed
      [[nodiscard]] bool PollPlayerSurvival();

      // Player position polling (underwater, falling, combat, sneak, mounted)
      // Updates: PlayerActorState position fields
      // Returns: true if state changed
      [[nodiscard]] bool PollPlayerPosition();

      // Target tracking polling (multi-target detection, vitals, distance)
      // Updates: TargetCollection (primary + targets map)
      // Returns: true if state changed
      [[nodiscard]] bool PollTargets();

      // Health tracking polling (damage/healing detection and rate calculation)
      // Updates: HealthTrackingState
      // Returns: true if state changed
      [[nodiscard]] bool PollHealthTracking();

      // Player resistances polling (v0.6.6)
      // Updates: PlayerActorState.resistances
      // Returns: true if state changed
      [[nodiscard]] bool PollPlayerResistances();

      // Player stamina tracking polling (v0.6.9)
      // Updates: StaminaTrackingState
      // Returns: true if state changed
      [[nodiscard]] bool PollStaminaTracking();

      // Player magicka tracking polling (v0.6.9)
      // Updates: MagickaTrackingState
      // Returns: true if state changed
      [[nodiscard]] bool PollMagickaTracking();

      // =============================================================================
      // WORLD OBJECT DETECTION HELPERS (Private)
      // =============================================================================

      // Get crosshair reference object (null if none)
      [[nodiscard]] RE::TESObjectREFR* GetCrosshairReference() noexcept;

      // Detect lock target and update state
      void DetectLockTarget(RE::TESObjectREFR* crosshairRef, WorldState& state) noexcept;

      // Detect ore vein target and update state (uses caching)
      void DetectOreVeinTarget(RE::TESObjectREFR* crosshairRef, WorldState& state) noexcept;

      // Detect workstation target and update state
      void DetectWorkstationTarget(RE::TESObjectREFR* crosshairRef, WorldState& state) noexcept;

      // =============================================================================
      // TARGET MANAGEMENT HELPERS (Private)
      // =============================================================================

      // Helper to look up actor by FormID (for sticky target recovery)
      // NOTE: Caller must validate actor state (IsDead, IsDisabled, IsDeleted)
      // Returns: Actor pointer if found and successfully cast, nullptr otherwise
      [[nodiscard]] static RE::Actor* GetActorByFormID(RE::FormID formID) noexcept;

      // Remove target from collection
      void RemoveTarget(RE::FormID formID) noexcept;

      // Prune stale targets (out of range, dead, timeout)
      void PruneStaleTargets(float gameTime) noexcept;

      // Calculate target priority score
      [[nodiscard]] static float CalculateTargetPriority(const TargetActorState& target) noexcept;

      // Cached actor type lookup (avoids repeated race string matching per poll)
      // Falls back to ClassifyActor on cache miss, stores result for future polls
      [[nodiscard]] TargetType GetCachedActorType(RE::Actor* actor);

      // =============================================================================
      // STATE STORAGE (3 models)
      // =============================================================================

      WorldState m_worldState;
      PlayerActorState m_playerState;
      TargetCollection m_targets;
      HealthTrackingState m_healthTracking;
      StaminaTrackingState m_staminaTracking;   // v0.6.9
      MagickaTrackingState m_magickaTracking;   // v0.6.9

      // =============================================================================
      // THREAD SYNCHRONIZATION (3 locks - coarse-grained)
      // =============================================================================

      mutable std::shared_mutex m_worldMutex;    // Protects WorldState
      mutable std::shared_mutex m_playerMutex;   // Protects PlayerActorState
      mutable std::shared_mutex m_targetsMutex;  // Protects TargetCollection

      // =============================================================================
      // POLL TIMERS (7 float accumulators)
      // =============================================================================

      float m_worldObjectsTimer = 0.0f;
      float m_playerVitalsTimer = 0.0f;
      float m_playerMagicEffectsTimer = 0.0f;
      float m_playerEquipmentTimer = 0.0f;
      float m_playerSurvivalTimer = 0.0f;
      float m_playerPositionTimer = 0.0f;
      float m_targetsTimer = 0.0f;
      float m_healthTrackingTimer = 0.0f;
      float m_playerResistancesTimer = 0.0f;  // v0.6.6
      float m_staminaTrackingTimer = 0.0f;    // v0.6.9
      float m_magickaTrackingTimer = 0.0f;    // v0.6.9

      // =============================================================================
      // POLL INTERVALS (Configurable, defaults from StateManagerConstants.h)
      // =============================================================================

      float m_worldPollInterval = PollInterval::WORLD_MS;
      float m_playerVitalsPollInterval = PollInterval::PLAYER_VITALS_MS;
      float m_playerMagicEffectsPollInterval = PollInterval::PLAYER_MAGIC_EFFECTS_MS;
      float m_playerEquipmentPollInterval = PollInterval::PLAYER_EQUIPMENT_MS;
      float m_playerSurvivalPollInterval = PollInterval::PLAYER_SURVIVAL_MS;
      float m_playerPositionPollInterval = PollInterval::PLAYER_POSITION_MS;
      float m_targetsPollInterval = PollInterval::TARGETS_MS;
      float m_healthTrackingPollInterval = PollInterval::PLAYER_HEALTH_TRACKING_MS;
      float m_playerResistancesPollInterval = PollInterval::PLAYER_RESISTANCES_MS;  // v0.6.6
      float m_staminaTrackingPollInterval = PollInterval::PLAYER_STAMINA_TRACKING_MS;  // v0.6.9
      float m_magickaTrackingPollInterval = PollInterval::PLAYER_MAGICKA_TRACKING_MS;  // v0.6.9

      // =============================================================================
      // STATE CHANGE TRACKING (Stage 3c - Performance optimization)
      // =============================================================================

      // Tracks whether last Update() detected any state changes
      // Used by UpdateLoop to skip expensive pipeline when state is stable
      std::atomic<bool> m_lastUpdateChanged{true};  // Default true to run pipeline on first update

      // Combat transition tracking (set in PollPlayerPosition, consumed by UpdateLoop)
      std::atomic<CombatTransition> m_combatTransition{CombatTransition::None};
      std::atomic<bool> m_isInCombat{false};
      bool m_wasInCombat = false;  // Previous tick's combat state (single-writer in PollPlayerPosition)

      // =============================================================================
      // TARGET TRACKING STATE (Persistent across polls)
      // =============================================================================

      // Ore vein detection cache (avoid repeated keyword string matching)
      std::unordered_set<RE::FormID> m_oreVeinCache;
      std::unordered_set<RE::FormID> m_notOreVeinCache;

      // =============================================================================
      // CROSSHAIR HYSTERESIS STATE (v0.6.7 - Prevents target flickering)
      // =============================================================================
      // When crosshair raycast fails momentarily (due to movement, animation, etc.),
      // we keep the previous target "sticky" for a brief period to prevent UI flicker.
      // THREAD SAFETY: Protected by m_targetsMutex (accessed only from DetectPrimaryTarget,
      // which is called from PollTargets while holding the lock)

      RE::FormID m_stickyTargetFormID = 0;      // Last detected crosshair target
      float m_stickyTargetLastSeenTime = 0.0f;  // Game-time when crosshair last detected it

      // =============================================================================
      // ACTOR TYPE CACHE (Optimization: avoids repeated ClassifyActor string matching)
      // =============================================================================
      // Race never changes at runtime, so cache ClassifyActor results by FormID.
      // Cleared on save load (ResetTrackingState) for safety.

      std::unordered_map<RE::FormID, TargetType> m_actorTypeCache;

      // =============================================================================
      // ALLY DEDUPLICATION SET (Reusable across polls — avoids per-tick allocation)
      // =============================================================================
      // Pre-reserved member cleared each poll instead of constructing/destroying.

      std::unordered_set<RE::FormID> m_processedAllies;

      // =============================================================================
      // RACE FORMID CACHE (v0.6.6 - Performance optimization)
      // =============================================================================
      // Caches vampire/werewolf race FormIDs to avoid expensive std::strstr()
      // calls on race EditorIDs every poll cycle. Initialized on first use.

      std::unordered_set<RE::FormID> m_vampireRaceFormIDs;
      RE::FormID m_werewolfBeastFormID = 0;
      RE::FormID m_werewolfHumanFormID = 0;
      bool m_raceFormIDsCached = false;

      // Helper method to initialize race FormID cache (called once on first use)
      void CacheRaceFormIDs() noexcept;

      // =============================================================================
      // SURVIVAL MODE GLOBALS CACHE (v0.6.7 - CC Survival Mode integration)
      // =============================================================================
      // Cached TESGlobal pointers for CC Survival Mode values.
      // Reading globals directly is more reliable than parsing effect names.
      // FormIDs from ccqdrsse001-survivalmode.esl and Update.esm

      RE::TESGlobal* m_survivalColdNeedValue = nullptr;     // Cold value 0-1000
      RE::TESGlobal* m_survivalHungerNeedValue = nullptr;   // Hunger value 0-1000
      RE::TESGlobal* m_survivalExhaustionNeedValue = nullptr; // Exhaustion value 0-1000
      RE::TESGlobal* m_survivalModeEnabled = nullptr;       // Whether survival mode is active
      bool m_survivalGlobalsCached = false;

      // SMI-specific globals (SurvivalModeImproved.esp)
      bool m_smiInstalled = false;
      RE::TESGlobal* m_smiHungerStage = nullptr;       // 0xA14 - pre-computed hunger stage (0-5)
      RE::TESGlobal* m_smiExhaustionStage = nullptr;    // 0xA1C - pre-computed exhaustion stage (0-5)
      RE::TESGlobal* m_smiColdStage = nullptr;          // 0xD1E - pre-computed cold stage (0-5)
      RE::TESGlobal* m_smiHungerEnabled = nullptr;      // 0xF27 - per-need enable flag
      RE::TESGlobal* m_smiColdEnabled = nullptr;        // 0xF28 - per-need enable flag
      RE::TESGlobal* m_smiExhaustionEnabled = nullptr;  // 0xF29 - per-need enable flag

      // Native warmth function (CC Survival engine function)
      using GetWarmthRating_t = float(RE::Actor*);
      GetWarmthRating_t* m_getWarmthRating = nullptr;
      bool m_warmthFunctionCached = false;

      // Helper method to initialize survival mode globals cache
      void CacheSurvivalGlobals() noexcept;

      // =============================================================================
      // DAMAGE TRACKING STATE (Persistent across polls)
      // =============================================================================

      // Previous health value for delta calculation (-1 indicates not initialized)
      float m_previousHealth = -1.0f;

      // Previous damage rate for trend detection
      float m_previousDamageRate = 0.0f;

      // Previous healing rate for trend detection
      float m_previousHealingRate = 0.0f;

      // =============================================================================
      // STAMINA/MAGICKA TRACKING STATE (v0.6.9 - Persistent across polls)
      // =============================================================================

      // Previous stamina value for delta calculation (-1 indicates not initialized)
      float m_previousStamina = -1.0f;

      // Previous magicka value for delta calculation (-1 indicates not initialized)
      float m_previousMagicka = -1.0f;

      // Previous usage rates for trend detection
      float m_previousStaminaUsageRate = 0.0f;
      float m_previousMagickaUsageRate = 0.0f;

      // =============================================================================
      // SUB-THRESHOLD ACCUMULATORS (v0.12.x - Small Bleed Detection)
      // =============================================================================
      // Accumulate sub-threshold vital losses across ticks. When the running total
      // crosses the existing threshold, we emit an event and reset. Decays at 0.8×
      // per idle tick (~310ms half-life) to prevent jitter accumulation.
      // Thread safety: single-writer (only mutated from Poll*Tracking, called by Update).

      float m_accumulatedHealthDamage = 0.0f;
      float m_accumulatedMagickaUsage = 0.0f;
      float m_accumulatedStaminaUsage = 0.0f;

      // =============================================================================
      // SOURCE CLASSIFICATION HELPERS (v0.6.9)
      // =============================================================================

      // Classify stamina usage source based on player state
      [[nodiscard]] static StaminaUsageSource ClassifyStaminaUsage(RE::PlayerCharacter* player) noexcept;

      // Classify magicka usage source based on player casting state
      // Returns source type and sets outSpellID to the spell being cast (if any)
      [[nodiscard]] static MagickaUsageSource ClassifyMagickaUsage(RE::PlayerCharacter* player, RE::FormID& outSpellID) noexcept;
   };
}
