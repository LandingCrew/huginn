#pragma once

#include <optional>

#include "SlotAssignment.h"
#include "SlotSettings.h"
#include "override/OverrideConditions.h"
#include "telemetry/SoakMetrics.h"
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>

namespace Huginn::Slot
{
    // =============================================================================
    // SLOT LOCK CONFIGURATION
    // =============================================================================
    // Configures the temporal stability behavior for slot locking.
    // These settings can be tuned via INI file for player preference.
    // =============================================================================

    struct SlotLockConfig
    {
        float lockDurationMs = 3000.0f;      // Default lock duration (3 seconds). Set to 0 to disable locking.
        float minLockDurationMs = 500.0f;    // Minimum time before lock can break
        bool lockOnFill = true;              // Lock when slot fills from empty
        bool overridesBreakLock = true;      // Allow high-priority overrides to break locks
        int immediateBreakPriority = 50;     // Overrides at or above this priority bypass minLockDurationMs
        // In combat, the lock a key gets instead of lockDurationMs (core/CombatLock.h):
        // -1 = freeze until combat ends, 0 = use lockDurationMs, else ms (<= 120000).
        float combatLockDurationMs = -1.0f;
        // In combat with no enemy tracked for this long, the combat lock lets go
        // as if combat had ended (core::CombatPresence). 0 = follow the game's
        // combat flag alone. Seconds, 0-60.
        float combatNoEnemyReleaseSec = 5.0f;
    };

    // =============================================================================
    // LOCKED SLOT STATE
    // =============================================================================
    // Tracks the lock state for a single slot, including the locked assignment
    // and remaining lock duration.
    // =============================================================================

    struct LockedSlot
    {
        // The locked assignment. INVARIANT: the embedded candidate's name view
        // is blanked on storage (see TruncateCandidateViews) — registry
        // reconcile can invalidate it between pipeline runs. Read the owned
        // assignment.name instead.
        SlotAssignment assignment;
        float remainingMs = 0.0f;            // Time remaining on lock
        float totalDurationMs = 0.0f;        // Original lock duration (for elapsed calculation)
        bool isLocked = false;               // Whether this slot is currently locked

        // Track previous assignment for "confirmed" detection
        RE::FormID previousFormID = 0;       // FormID from last frame
        bool hadContent = false;             // Was non-empty last frame

        // Taken under the combat lock (fCombatLockDurationMs): `frozen` = no
        // timer at all until combat ends. SetInCombat(false) turns these back
        // into normal locks timed from the end of combat.
        bool combatLock = false;
        bool frozen = false;
        // When the lock was taken: a frozen lock's timer does not run, so its
        // age (for fMinLockDurationMs) comes from here.
        std::chrono::steady_clock::time_point lockedAt{};

        // Sticky post-activation lock (LockSlotForActivation): a deliberate 10s
        // hold that OnItemUsed must not break when asked to respect it, so a
        // just-activated item stays visible even after it's consumed.
        bool isActivationLock = false;

        // Churn telemetry. What the player was SHOWN in this slot last run --
        // post-dedup, which `assignment` above is not: an unlocked slot stores
        // the allocator's pick even when dedup then cleared it.
        RE::FormID shownFormID = 0;
        uint16_t shownUniqueID = 0;
        bool shownEmpty = true;
        bool shownWildcard = false;
        bool shownRemembered = false;
        std::string shownName;
        // When the shown NAME arrived, and what the key showed before it: the
        // key's age as the player sees it. Stamped on every visible change --
        // page switches and the post-load baseline included, same-name swaps
        // not. Read by tenure and by GetKeyAge.
        std::chrono::steady_clock::time_point shownSince{};
        std::string prevName;

        // Why this slot's last lock let go (expiry, OnItemUsed, a page switch,
        // an override break), held until the slot next locks, so the change
        // that lock release allowed can be attributed to it. Unheld = nothing
        // recorded.
        Telemetry::SlotChange releaseCause = Telemetry::SlotChange::Unheld;
    };

    // =============================================================================
    // SLOT LOCKER
    // =============================================================================
    // Provides temporal stability for slot assignments by "locking" slots for a
    // configurable duration after they receive new content.
    //
    // PROBLEM SOLVED:
    //   Recommendations disappear when context changes, creating frustrating UX:
    //   - Player underwater -> Waterbreathing appears -> Player surfaces to cast
    //     -> Context gone -> Spell disappears before player can use it
    //
    // SOLUTION:
    //   Lock slot assignments for a configurable duration (default 3 seconds),
    //   allowing the player time to act on recommendations even if the triggering
    //   context changes.
    //
    // PIPELINE POSITION:
    //   SlotAllocator (stateless) -> [SlotLocker (stateful)] -> Widget/Wheeler
    //
    // THREAD SAFETY:
    //   SlotLocker is primarily accessed from the update loop's thread (Update,
    //   ApplyLocks): a game job thread in gameplay, the main thread in menus
    //   (UpdateLoop.cpp, THREADS above OnUpdate). Wheeler callbacks may call
    //   OnItemUsed / LockSlotForActivation from a different thread.  All public
    //   methods are guarded by m_mutex.
    // =============================================================================

    class SlotLocker
    {
    public:
        static SlotLocker& GetSingleton();

        /// How long key `slotIndex` of the displayed page has shown its item,
        /// and what it showed before -- one locked read, so the two always
        /// describe the same change. nullopt before the key's first fill.
        struct KeyAge
        {
            float ageSec = 0.0f;
            std::string wasName;
        };
        [[nodiscard]] std::optional<KeyAge> GetKeyAge(size_t slotIndex) const;

        /// What OnItemUsed is told about the item. Gone: none is left (consumed
        /// to the last one, dropped, sold, a stale stack) -- always breaks the
        /// lock. Used: activated or one consumed, maybe more left -- breaks a
        /// normal lock so the slot can refresh, but not a combat lock, which
        /// gives way only when the item is gone.
        enum class UseKind : uint8_t { Gone, Used };

        /// Per-run facts ApplyLocks cannot see for itself.
        struct RunContext
        {
            /// Key i is the key the player pressed for the Remembrance hold
            /// shown on it (Remembrance::Entry shown on its own slot). Under a
            /// combat lock only such a hold may take a locked key.
            std::array<bool, MAX_SLOTS_PER_PAGE> remembranceOnPressedKey{};

            /// Game probe for a combat-locked key's item, asked on every run
            /// in combat (core::DecideCombatLockItemRelease): gone = a spell or
            /// shout no longer known; equipped = in the player's hands / worn.
            struct ItemState
            {
                bool gone = false;
                bool equipped = false;
            };
            std::function<ItemState(RE::FormID formID, uint16_t uniqueID)> probeItem;
        };

        // =========================================================================
        // CONFIGURATION
        // =========================================================================

        /// Set lock configuration (call during initialization or INI reload)
        void SetConfig(const SlotLockConfig& config);

        /// Get current lock configuration (a copy, read under m_mutex: SetConfig
        /// can run from the dMenu reload while the update thread reads it).
        [[nodiscard]] SlotLockConfig GetConfig() const;

        // =========================================================================
        // MAIN API
        // =========================================================================

        /// Update lock timers. Called unconditionally from UpdateSubsystems every
        /// tick (NOT from behind the pipeline-skip gate) so locks decay in
        /// wall-clock time.
        /// @param deltaMs Milliseconds since last update
        /// @return true if any lock expired this update — the caller must force a
        ///         pipeline run (MarkPageDirty) so the freed slot's content swaps
        ///         without waiting for an unrelated state change
        [[nodiscard]] bool Update(float deltaMs);

        /// Tell the locker whether the player is in combat (every tick, before
        /// Update). Entering combat: locks taken from now on use
        /// fCombatLockDurationMs, and the next ApplyLocks locks every key that
        /// shows an item. Leaving it: combat locks fall back to the normal timer
        /// counted from now (core/CombatLock.h). @return true if that released a
        /// lock (normal locking off) -- the caller must force a pipeline run.
        [[nodiscard]] bool SetInCombat(bool inCombat);

        /// Restart every lock's timer at the full duration, as if each key had
        /// just changed. Called when the Wheeler wheel closes: the widget is
        /// hidden while the wheel is open, so a key that changed then is first
        /// SEEN at the close -- and its lock, started when it changed, used to
        /// run out under three seconds after the player first saw it.
        void RestartLocks();

        /// Apply locking logic to raw assignments from SlotAllocator
        /// @param newAssignments Fresh assignments from SlotAllocator
        /// @param overrides Active overrides (for priority-based lock breaking)
        /// @param scored This run's full scored list, used only for churn
        ///   telemetry: the score of an item a slot just dropped, which the
        ///   assignments no longer carry. Empty = no challenger ratios.
        /// @return Stable assignments with locks applied
        [[nodiscard]] SlotAssignments ApplyLocks(
            const SlotAssignments& newAssignments,
            const Override::OverrideCollection& overrides,
            std::span<const Scoring::ScoredCandidate> scored = {},
            const RunContext& run = {});

        // =========================================================================
        // MANUAL LOCK CONTROL
        // =========================================================================

        /// Lock a specific slot for a duration
        void LockSlot(size_t slotIndex, float durationMs);

        /// Lock a slot after activation (Sticky post-activation policy)
        /// Uses a longer duration (10s) so the activated item stays visible
        void LockSlotForActivation(size_t slotIndex);

        /// Unlock a specific slot immediately
        void UnlockSlot(size_t slotIndex);

        /// Unlock all slots (e.g., on page change)
        void UnlockAll();

        // =========================================================================
        // QUERY METHODS
        // =========================================================================

        /// Read-only view of a single slot's lock state (one entry per slot).
        struct SlotLockView
        {
            bool isLocked = false;
            float remainingMs = 0.0f;
            RE::FormID previousFormID = 0;
            bool hadContent = false;
            bool frozen = false;   ///< no timer (combat freeze): remainingMs means nothing
        };

        /// Snapshot all slots' lock state under a SINGLE mutex acquisition.
        /// Prefer this over per-slot IsSlotLocked/GetRemainingLockTime/WasConfirmed
        /// when a caller needs several slots' state in one pass (e.g. visual-state
        /// computation), which otherwise takes up to 3 lock/unlock pairs per slot.
        [[nodiscard]] std::array<SlotLockView, MAX_SLOTS_PER_PAGE> GetLockSnapshot() const;

        /// Check if a slot is currently locked
        [[nodiscard]] bool IsSlotLocked(size_t slotIndex) const;

        /// Get remaining lock time for a slot (0 if not locked)
        [[nodiscard]] float GetRemainingLockTime(size_t slotIndex) const;

        /// Check if slot content was confirmed (re-evaluated to same FormID)
        [[nodiscard]] bool WasConfirmed(size_t slotIndex, RE::FormID currentFormID) const;

        // =========================================================================
        // EVENT HANDLERS
        // =========================================================================

        /// Called when player uses an item (Wheeler/keyboard), or an item leaves
        /// inventory (consume/drop, from the delta-scan path). Breaks the lock on
        /// any slot containing this item so it can repopulate.
        /// @param respectActivationLock When true, does NOT break a Sticky
        ///   activation lock (LockSlotForActivation) — the delta scan passes true
        ///   so a consumed sticky item still honors its 10s visibility window.
        void OnItemUsed(RE::FormID formID, bool respectActivationLock = false, UseKind kind = UseKind::Gone);

        /// Same, for one inventory STACK rather than every stack of a form.
        ///
        /// A player carrying a tempered Iron Dagger and a plain one has two
        /// registry entries and two candidates that share a FormID. When one of
        /// them leaves, breaking both locks would evict the copy still in the
        /// pack; breaking neither leaves the slot naming a stack that is gone,
        /// which the equip path then cannot honour. So the departure names the
        /// stack.
        /// @param uniqueID The stack's ExtraUniqueID. 0 means "no instance to
        ///   speak of" — plain stock, or a category like potions and ammo where
        ///   the form IS the identity — and breaks every lock on the form, which
        ///   is what the form-wide overload above does.
        void OnItemUsed(RE::FormID formID, uint16_t uniqueID, bool respectActivationLock,
            UseKind kind = UseKind::Gone);

        /// Reset all locks (call on save load or game state reset)
        void Reset();

    private:
        SlotLocker() = default;
        ~SlotLocker() = default;
        SlotLocker(const SlotLocker&) = delete;
        SlotLocker& operator=(const SlotLocker&) = delete;

        // =========================================================================
        // CONSTANTS
        // =========================================================================

        static constexpr size_t MAX_SLOTS = MAX_SLOTS_PER_PAGE;  // From SlotSettings.h — single source of truth
        static constexpr float ACTIVATION_LOCK_MS = 10000.0f;  // 10s lock for Sticky policy

        // =========================================================================
        // STATE
        // =========================================================================

        SlotLockConfig m_config;                            // Current configuration
        std::array<LockedSlot, MAX_SLOTS> m_lockedSlots;   // Lock state per slot
        mutable std::mutex m_mutex;                         // Thread safety for callback access

        // True until the first ApplyLocks after construction or Reset(): that
        // run records what is shown without counting it as churn. A save load
        // or cell transition repopulating every slot is not the player
        // watching keys change.
        bool m_churnBaseline = true;

        // The combat flag SetInCombat last saw. Guarded by m_mutex.
        bool m_inCombat = false;

        // =========================================================================
        // INTERNAL HELPERS
        // =========================================================================

        /// Determine if a new assignment should trigger a lock
        [[nodiscard]] bool ShouldLock(
            const SlotAssignment& oldAssign,
            const SlotAssignment& newAssign) const;

        /// Why an existing lock lets go this run (None = it holds).
        enum class BreakReason : uint8_t { None, Expired, Override, Remembrance, PinnedEnded, Restyle };

        /// Determine if an existing lock should be broken, and why. Under a
        /// combat lock the rules are core::DecideCombatLockBreak's.
        [[nodiscard]] BreakReason ShouldBreakLock(
            const LockedSlot& lock,
            const SlotAssignment& newAssign,
            const Override::OverrideCollection& overrides,
            bool remembranceOnPressedKey) const;

        /// Lock `slot` on `assignment` now, with the duration combat calls for.
        /// Caller holds m_mutex.
        void LockNow(LockedSlot& slot, const SlotAssignment& assignment);

        /// Post-lock dedup: a locked slot can hold an item the allocator also
        /// placed in another (unlocked) slot this frame. Clears duplicate names,
        /// PREFERRING to keep the locked occurrence so a lower-index unlocked
        /// duplicate can't evict locked content. Caller must hold m_mutex.
        /// Also refills a slot it empties with the item the allocator wanted
        /// in a locked slot (shown nowhere else), and releases the emptied
        /// slot's lock. Not const: it may unlock.
        /// A locked key that already showed the item last run (its incumbent)
        /// beats any other copy, locked or not: the other copy is the item
        /// arriving somewhere it was not, and letting a lower index win evicted
        /// the incumbent -- two changes for none. The refilled slot is LOCKED
        /// (0.23.37): an unlocked refill changed again on the next run, 0.1-0.7 s
        /// later (2026-10-11 08:56:21.327, 09:20:33.462).
        void DedupePreferLocked(SlotAssignments& result, const SlotAssignments& wanted);
    };

}  // namespace Huginn::Slot
