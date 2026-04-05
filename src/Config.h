#pragma once

namespace Huginn::Config
{
   // =============================================================================
   // TUNABLE PARAMETERS
   // =============================================================================
   // These parameters can be adjusted to tune system behavior without recompiling.
   // Future: Load from INI file or dMenu (v0.5.0)

   // -----------------------------------------------------------------------------
   // Spell Registry Configuration
   // -----------------------------------------------------------------------------

   // How often to check for newly learned spells (in milliseconds)
   // Lower = more responsive, Higher = better performance
   // Recommended: 3000-10000ms (3-10 seconds)
   inline constexpr float SPELL_RECONCILE_INTERVAL_MS = 5000.0f;

   // -----------------------------------------------------------------------------
   // State Evaluation Configuration
   // -----------------------------------------------------------------------------

   // How often to log game state in debug mode (in milliseconds)
   // Only affects debug builds
   inline constexpr float STATE_LOG_INTERVAL_MS = 1000.0f;

   // -----------------------------------------------------------------------------
   // Reward Shaping Configuration (v0.3.0+)
   // -----------------------------------------------------------------------------

   // Reward for equipping an item (positive reinforcement)
   // Recommended: 1.0 - 10.0
   inline constexpr float EQUIP_REWARD = 8.0f;

   // Reward for consuming a potion or scroll (strongest preference signal — finite resource committed)
   // Separate from EQUIP_REWARD so it can be tuned independently
   // Recommended: 3.0 - 10.0
   inline constexpr float CONSUME_REWARD = 5.0f;

   // -----------------------------------------------------------------------------
   // Update System Configuration (v0.5.0+)
   // -----------------------------------------------------------------------------

   // Update interval for state evaluation and widget refresh (in milliseconds)
   // Lower = more responsive, Higher = better performance
   // Recommended: 100-200ms (10-5 Hz)
   // Target: < 0.1ms per update check
   inline constexpr float UPDATE_INTERVAL_MS = 100.0f;

   // Wildcard cooldown: How long a wildcard spell persists before a new one can be selected
   // Higher = more stable recommendations, Lower = more variety
   // Recommended: 30-60 seconds
   inline constexpr float WILDCARD_COOLDOWN_SECONDS = 30.0f;

   // Thresholds (still compile-time; not in INI)
   inline constexpr float WEAPON_CHARGE_LOW_THRESHOLD = 0.2f;  // 20% - low charge warning

   // -----------------------------------------------------------------------------
   // State Manager Configuration (v0.6.1+)
   // -----------------------------------------------------------------------------

   // Maximum number of actors to track simultaneously
   // Balances multi-target awareness with performance
   // Typical combat: 3-5 enemies, Large battles: 10+ enemies
   // Stress testing: 50 (Phase 6 performance tuning)
   // Recommended: 8-12 (production), 50 (stress test)
   inline constexpr size_t MAX_TRACKED_TARGETS = 50;

   // Target detection range (in game units)
   // Beyond this range, actors are too far for meaningful interaction
   // Recommended: 1024-4096 (2048 matches "Ranged" distance bucket)
   inline constexpr float TARGET_DETECTION_RANGE = 2048.0f;

   // -----------------------------------------------------------------------------
   // Item Registry Configuration (v0.7.4+)
   // -----------------------------------------------------------------------------

   // How often to check for item count changes (delta scan)
   // Lower = more responsive potion tracking, Higher = better performance
   // Recommended: 500ms (2 Hz)
   inline constexpr float ITEM_COUNT_REFRESH_INTERVAL_MS = 500.0f;

   // How often to do full item reconciliation (add/remove items)
   // Recommended: 30000ms (30 seconds)
   inline constexpr float ITEM_RECONCILE_INTERVAL_MS = 30000.0f;

   // Maximum items to track (performance safety)
   // Typical player: 20-50, Hoarder: 200+
   inline constexpr size_t MAX_TRACKED_ITEMS = 500;

   // -----------------------------------------------------------------------------
   // Weapon Registry Configuration (v0.7.6+)
   // -----------------------------------------------------------------------------

   // How often to refresh weapon charge levels (delta scan)
   // Lower = more responsive charge tracking, Higher = better performance
   // Recommended: 500ms (2 Hz) - matches item refresh interval
   inline constexpr float WEAPON_REFRESH_INTERVAL_MS = 500.0f;

   // How often to do full weapon reconciliation (add/remove favorites)
   // Recommended: 30000ms (30 seconds) - matches item reconcile interval
   inline constexpr float WEAPON_RECONCILE_INTERVAL_MS = 30000.0f;

   // Minimum time to wait after save load before accessing extraLists (v0.7.9)
   // During this window, extraLists pointers may be stale/uninitialized
   // Accessing them causes EXCEPTION_ACCESS_VIOLATION crashes
   inline constexpr float EXTRALIST_STABILIZATION_MS = 500.0f;

   // Maximum favorited weapons to track
   // Typical player: 5-15 favorites, Collector: 30-50
   inline constexpr size_t MAX_TRACKED_WEAPONS = 100;

   // Maximum ammo types to track
   // Typical player: 5-10 ammo types
   inline constexpr size_t MAX_TRACKED_AMMO = 50;

   // -----------------------------------------------------------------------------
   // Spell Favorites Configuration (v0.7.8+)
   // -----------------------------------------------------------------------------

   // How often to refresh spell favorites status (delta scan)
   // Lower = more responsive favorites tracking, Higher = better performance
   // Recommended: 500ms (2 Hz) - matches weapon/item refresh interval
   inline constexpr float SPELL_FAVORITES_REFRESH_INTERVAL_MS = 500.0f;

   // -----------------------------------------------------------------------------
   // Negative Learning Configuration (v0.13.0+)
   // -----------------------------------------------------------------------------

   // Lazy weight decay: items unused for hours gradually lose learned preference
   inline constexpr float DECAY_RATE_PER_HOUR = 0.02f;       // 2%/hr exponential decay
   inline constexpr float DECAY_THRESHOLD_MINUTES = 5.0f;     // Don't decay if updated within 5 min

   // Misclick detection: rapid equip-then-switch penalizes the discarded item
   inline constexpr float MISCLICK_WINDOW_SECONDS = 3.0f;     // Max gap to count as misclick
   inline constexpr float MISCLICK_PENALTY = -3.0f;           // ~37.5% of EQUIP_REWARD

   // -----------------------------------------------------------------------------
   // Debug Configuration
   // -----------------------------------------------------------------------------

   // Debug UI positioning (only affects debug builds)
   inline constexpr float STATE_MANAGER_DEBUG_POS_X = 500.0f;  // Legacy, not used for initial position
   inline constexpr float STATE_MANAGER_DEBUG_POS_Y = 10.0f;
}
