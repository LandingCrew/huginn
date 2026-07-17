#pragma once

#include "SlotAssignment.h"
#include "SlotSettings.h"
#include "override/OverrideConditions.h"
#include <array>
#include <cstdint>
#include <mutex>

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

        // Sticky post-activation lock (LockSlotForActivation): a deliberate 10s
        // hold that OnItemUsed must not break when asked to respect it, so a
        // just-activated item stays visible even after it's consumed.
        bool isActivationLock = false;
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
    //   SlotLocker is primarily accessed from the main update thread (Update,
    //   ApplyLocks), but Wheeler callbacks may call OnItemUsed / LockSlotForActivation
    //   from a different thread.  All public methods are guarded by m_mutex.
    // =============================================================================

    class SlotLocker
    {
    public:
        static SlotLocker& GetSingleton();

        // =========================================================================
        // CONFIGURATION
        // =========================================================================

        /// Set lock configuration (call during initialization or INI reload)
        void SetConfig(const SlotLockConfig& config);

        /// Get current lock configuration
        [[nodiscard]] const SlotLockConfig& GetConfig() const noexcept { return m_config; }

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

        /// Apply locking logic to raw assignments from SlotAllocator
        /// @param newAssignments Fresh assignments from SlotAllocator
        /// @param overrides Active overrides (for priority-based lock breaking)
        /// @return Stable assignments with locks applied
        [[nodiscard]] SlotAssignments ApplyLocks(
            const SlotAssignments& newAssignments,
            const Override::OverrideCollection& overrides);

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
        void OnItemUsed(RE::FormID formID, bool respectActivationLock = false);

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

        // =========================================================================
        // INTERNAL HELPERS
        // =========================================================================

        /// Determine if a new assignment should trigger a lock
        [[nodiscard]] bool ShouldLock(
            const SlotAssignment& oldAssign,
            const SlotAssignment& newAssign) const;

        /// Determine if an existing lock should be broken
        [[nodiscard]] bool ShouldBreakLock(
            const LockedSlot& lock,
            const SlotAssignment& newAssign,
            const Override::OverrideCollection& overrides) const;

        /// Post-lock dedup: a locked slot can hold an item the allocator also
        /// placed in another (unlocked) slot this frame. Clears duplicate names,
        /// PREFERRING to keep the locked occurrence so a lower-index unlocked
        /// duplicate can't evict locked content. Caller must hold m_mutex.
        void DedupePreferLocked(SlotAssignments& result) const;
    };

}  // namespace Huginn::Slot
