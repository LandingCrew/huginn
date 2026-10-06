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

   // How long a context reason stays on the wheel after it stops being true (#62).
   // Only damps a DOWNGRADE — a more urgent reason is adopted instantly — so this
   // is the longest a stale label can linger, not a delay on real news.
   // Why 1500: a momentary crouch or a crosshair crossing a draugr is true for
   // ~100ms, and 15× that is comfortably long enough to read. Also below
   // SlotLocker's DEFAULT 3000ms content lock, so the label cannot outlive the
   // slot contents it is explaining — but that lock is fLockDurationMs, which
   // the player can lower or zero out, so this is a ceiling and not the whole
   // guarantee. ScoreCandidates clamps the effective hold to the live lock
   // duration; keep the two arguments together if this number changes.
   inline constexpr float REASON_HOLD_MS = 1500.0f;

   // -----------------------------------------------------------------------------
   // Reward Shaping Configuration (v0.3.0+)
   // -----------------------------------------------------------------------------

   // The choice target (roadmap Phase 3 #1, 0.23.0). The learner learns "was
   // this chosen": 1 for the item the player confirmed -- equip or consume
   // alike, one selection one target -- and 0 for an item shown on the page
   // for the same need and passed over. It replaced 8 (equip) / 5 (consume),
   // which regressed every used item onto a fixed number with no contrast and
   // let a trained item's learned boost reach ~24x: the soak run's crowding
   // (docs/playtest/Soak-2026-10-LoreRim.md). On 0-1 the boost tops out near
   // 1 + lambdaMax = 4x.
   inline constexpr float CHOICE_TARGET = 1.0f;
   inline constexpr float PASSED_OVER_TARGET = 0.0f;

   // A passed-over item is weaker evidence than a chosen one (the player may
   // not have looked), so its update is a quarter step -- the best of 0.25 /
   // 0.5 / 1.0 in tools/replay over the soak run -- and it does not count as a
   // train: confidence climbs only on choices.
   inline constexpr float PASSED_OVER_STEP = 0.25f;

   // One decision, one reward: EQUIPPING the same item again within this
   // window teaches nothing -- the main weapon taken back after every scroll is
   // not a new choice (the user, 2026-10-02: "not really, or a really weak
   // reward"). The soak's top item, Soul Sword, held 16% of all trains from
   // exactly this. Equips only: two drinks of one potion are two decisions.
   // Decided once, in SelectionTracker::Confirm, and honoured by every
   // subscriber (the learner AND the recency memory) via EquipEvent::repeatPick.
   inline constexpr float REPEAT_PICK_WINDOW_SEC = 30.0f;

   // A passed-over update waits this long, and is cancelled if its item is the
   // NEXT pick: companions -- circlet then ring, Oakflesh then Muffle, sword
   // then off-hand dagger -- share a need class but are worn or cast together,
   // so the one picked second was not passed over (code review of #172; the
   // roadmap's "Substitutes, not complements").
   inline constexpr float PASSED_OVER_DELAY_SEC = 10.0f;

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

   // Retry delay for a weapon reconcile whose load-time pass couldn't read
   // extraLists (rebuild never does; a post-load reconcile runs inside the
   // stabilization window). Must exceed EXTRALIST_STABILIZATION_MS so the
   // primed retry runs stable and recovers favorites/charge promptly instead
   // of waiting a full WEAPON_RECONCILE_INTERVAL_MS.
   inline constexpr float WEAPON_RECONCILE_RETRY_MS = 1000.0f;

   // Same idea for apparel (#65), and for a stronger reason. WeaponRegistry does
   // a DEGRADED scan on the load path — weapons are present, just without
   // favorites or charge. ApparelRegistry does none at all: a piece is classified
   // by its enchantment, a player enchantment lives in extraLists, so a load-path
   // scan would reject every player-made item permanently. RebuildRegistry
   // therefore only clears, and the registry is EMPTY until the first reconcile.
   // Without priming that is a full 30 s after every save load, during which
   // walking to a workstation shows nothing — observed 2026-09-08: load at
   // 22:17:11, first reconcile 22:17:41, slot populated 22:17:43.
   inline constexpr float APPAREL_RECONCILE_RETRY_MS = 1000.0f;

   // Minimum time to wait after save load before accessing extraLists (v0.7.9)
   // During this window, extraLists pointers may be stale/uninitialized
   // Accessing them causes EXCEPTION_ACCESS_VIOLATION crashes
   inline constexpr float EXTRALIST_STABILIZATION_MS = 500.0f;


   // How long a consumable selection waits for its count to drop before it
   // is dropped unconfirmed (SelectionTracker).
   //
   // A selection is an event, a consumption is a COUNT CHANGE noticed by the
   // 2 Hz delta scan. Measured gap between Huginn using a potion and the scan
   // seeing it go: 1.33 s (2026-09-21 18:05:21.397 -> 18:05:22.730). Too short
   // and real drinks go unconfirmed; too long and a count drop from something
   // else (a script, a second potion of the same form) inherits a stale pick.
   inline constexpr float CONSUMPTION_HUGINN_WINDOW_MS = 2500.0f;

   // How long an equip selection must survive to confirm: a weapon, spell or
   // ring still on the player this long after the pick is a choice; one
   // swapped away inside it was not (this replaced misclick detection).
   inline constexpr float SELECTION_CONFIRM_MS = 3000.0f;

   // How recent a vanilla hotkey press or an own-wheel pick must be for an
   // outside equip to count as the player's (PlayerInputGate). Equips land in
   // the same frame or the next few, so this is generous.
   inline constexpr float PLAYER_INPUT_WINDOW_MS = 1000.0f;

   // How long after the inventory / favorites / magic menu CLOSES an outside
   // equip still counts as made from it. LoreRim closes the inventory the
   // moment the player drinks (a drinking animation), and a potion's equip
   // event lands ~0.85 s after the act -- so by then the menu is gone. Wider
   // than PLAYER_INPUT_WINDOW_MS to leave room for a frame hitch.
   inline constexpr float MENU_CLOSE_INPUT_WINDOW_MS = 2000.0f;

   // Teardown guard, for the UNLOAD side of a session. Quitting to the main
   // menu fires no SKSE message, so there is nothing to start a timer from: the
   // only evidence is the shape of the scan itself. When the player's container
   // is torn down, one delta scan sees every tracked stack drop to zero and
   // would report the whole inventory as consumed (observed 2026-08-17: 72
   // phantom +5.0 rewards and 71 -3.0 misclick penalties in a 10 ms window).
   //
   // The ratio is measured against LIVE entries (count > 0), not registry size —
   // see the call site in UpdateLoop.cpp for why a stale zero-count tail would
   // defeat it.
   //
   // The floor only has to exclude ordinary use, and nobody drinks three distinct
   // stacks to exhaustion inside one 500 ms scan. It was 8 first, which made the
   // guard dead code for ScrollRegistry: a player rarely holds eight scroll types
   // at once, so the floor alone could never be met there and every phantom
   // scroll reward would still have landed.
   inline constexpr size_t TEARDOWN_MIN_DROPS = 3;
   inline constexpr float TEARDOWN_DROP_RATIO = 0.5f;

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


   // -----------------------------------------------------------------------------
   // Debug Configuration
   // -----------------------------------------------------------------------------

   // Debug UI positioning (only affects debug builds)
   inline constexpr float STATE_MANAGER_DEBUG_POS_X = 500.0f;  // Legacy, not used for initial position
   inline constexpr float STATE_MANAGER_DEBUG_POS_Y = 10.0f;

   // -----------------------------------------------------------------------------
   // Long-Play Soak Telemetry (v0.18.x)
   // -----------------------------------------------------------------------------

   // How often to emit the [Soak] heartbeat summary line (recommendation accept
   // rate, recompute/tick counts, learned-item growth, tick perf).
   // Long enough that it never counts as log spam, short enough to chart drift
   // across a multi-hour session. Recommended: 5 min.
   inline constexpr float SOAK_HEARTBEAT_INTERVAL_MS = 300000.0f;
}
