#include "SlotLocker.h"
#include "override/OverrideConditions.h"
#include <spdlog/spdlog.h>

namespace Huginn::Slot
{
    SlotLocker& SlotLocker::GetSingleton()
    {
        static SlotLocker instance;
        return instance;
    }

    // =========================================================================
    // CONFIGURATION
    // =========================================================================

    void SlotLocker::SetConfig(const SlotLockConfig& config)
    {
        m_config = config;
        spdlog::info("[SlotLocker] Config updated: lockDuration={:.0f}ms, minDuration={:.0f}ms, "
                     "lockOnFill={}, overridesBreakLock={}, immediateBreakPriority={}",
            m_config.lockDurationMs,
            m_config.minLockDurationMs,
            m_config.lockOnFill,
            m_config.overridesBreakLock,
            m_config.immediateBreakPriority);
    }

    // =========================================================================
    // MAIN API
    // =========================================================================

    void SlotLocker::Update(float deltaMs)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Decay lock timers for all slots
        for (auto& slot : m_lockedSlots) {
            if (slot.isLocked && slot.remainingMs > 0.0f) {
                slot.remainingMs -= deltaMs;

                if (slot.remainingMs <= 0.0f) {
                    slot.isLocked = false;
                    slot.remainingMs = 0.0f;
                    spdlog::debug("[SlotLocker] Lock expired for slot with FormID {:08X}",
                        slot.assignment.formID);
                }
            }
        }
    }

    SlotAssignments SlotLocker::ApplyLocks(
        const SlotAssignments& newAssignments,
        const Override::OverrideCollection& overrides)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        SlotAssignments result;
        result.reserve(newAssignments.size());

        for (size_t i = 0; i < newAssignments.size() && i < MAX_SLOTS; ++i) {
            const auto& newAssign = newAssignments[i];
            auto& lockedSlot = m_lockedSlots[i];

            if (lockedSlot.isLocked) {
                // Slot is currently locked - check if lock should break
                if (ShouldBreakLock(lockedSlot, newAssign, overrides)) {
                    // Break the lock
                    spdlog::debug("[SlotLocker] Slot {} lock broken (was {:08X})",
                        i, lockedSlot.assignment.formID);
                    lockedSlot.isLocked = false;
                    lockedSlot.remainingMs = 0.0f;
                    // Fall through to consider new assignment
                } else {
                    // Keep the locked assignment
                    // Update FormID history for confirmed detection (locked path)
                    lockedSlot.previousFormID = lockedSlot.assignment.formID;
                    lockedSlot.hadContent = !lockedSlot.assignment.IsEmpty();

                    result.push_back(lockedSlot.assignment);
                    continue;
                }
            }

            // No active lock (or lock was just broken) - process new assignment
            if (ShouldLock(lockedSlot.assignment, newAssign)) {
                // Lock the new assignment
                lockedSlot.assignment = newAssign;
                lockedSlot.remainingMs = m_config.lockDurationMs;
                lockedSlot.totalDurationMs = m_config.lockDurationMs;
                lockedSlot.isLocked = true;

                spdlog::debug("[SlotLocker] Slot {} locked for {:.0f}ms: {} ({:08X})",
                    i, m_config.lockDurationMs, newAssign.name, newAssign.formID);
            } else {
                // No lock needed - just track the assignment for comparison next frame
                lockedSlot.assignment = newAssign;
            }

            // Update FormID history for confirmed detection
            lockedSlot.previousFormID = lockedSlot.assignment.formID;
            lockedSlot.hadContent = !lockedSlot.assignment.IsEmpty();

            result.push_back(newAssign);
        }

        return result;
    }

    // =========================================================================
    // MANUAL LOCK CONTROL
    // =========================================================================

    void SlotLocker::LockSlot(size_t slotIndex, float durationMs)
    {
        if (slotIndex >= MAX_SLOTS) {
            spdlog::warn("[SlotLocker] LockSlot: index {} out of range", slotIndex);
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        auto& slot = m_lockedSlots[slotIndex];
        slot.remainingMs = durationMs;
        slot.totalDurationMs = durationMs;
        slot.isLocked = true;

        spdlog::debug("[SlotLocker] Slot {} manually locked for {:.0f}ms", slotIndex, durationMs);
    }

    void SlotLocker::LockSlotForActivation(size_t slotIndex)
    {
        if (slotIndex >= MAX_SLOTS) {
            spdlog::warn("[SlotLocker] LockSlotForActivation: index {} out of range", slotIndex);
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        auto& slot = m_lockedSlots[slotIndex];
        slot.remainingMs = ACTIVATION_LOCK_MS;
        slot.totalDurationMs = ACTIVATION_LOCK_MS;
        slot.isLocked = true;

        spdlog::info("[SlotLocker] Slot {} activation-locked for {:.0f}ms (Sticky policy)",
            slotIndex, ACTIVATION_LOCK_MS);
    }

    void SlotLocker::UnlockSlot(size_t slotIndex)
    {
        if (slotIndex >= MAX_SLOTS) {
            spdlog::warn("[SlotLocker] UnlockSlot: index {} out of range", slotIndex);
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        auto& slot = m_lockedSlots[slotIndex];
        if (slot.isLocked) {
            spdlog::debug("[SlotLocker] Slot {} manually unlocked (was {:08X})",
                slotIndex, slot.assignment.formID);
            slot.isLocked = false;
            slot.remainingMs = 0.0f;
        }
    }

    void SlotLocker::UnlockAll()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (size_t i = 0; i < MAX_SLOTS; ++i) {
            auto& slot = m_lockedSlots[i];
            if (slot.isLocked) {
                slot.isLocked = false;
                slot.remainingMs = 0.0f;
            }
        }
        spdlog::debug("[SlotLocker] All slots unlocked");
    }

    // =========================================================================
    // QUERY METHODS
    // =========================================================================

    bool SlotLocker::IsSlotLocked(size_t slotIndex) const
    {
        if (slotIndex >= MAX_SLOTS) {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lockedSlots[slotIndex].isLocked;
    }

    float SlotLocker::GetRemainingLockTime(size_t slotIndex) const
    {
        if (slotIndex >= MAX_SLOTS) {
            return 0.0f;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto& slot = m_lockedSlots[slotIndex];
        return slot.isLocked ? slot.remainingMs : 0.0f;
    }

    bool SlotLocker::WasConfirmed(size_t slotIndex, RE::FormID currentFormID) const
    {
        if (slotIndex >= MAX_SLOTS || currentFormID == 0) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const auto& slot = m_lockedSlots[slotIndex];

        // Confirmed = had content before AND same FormID now
        return slot.hadContent && slot.previousFormID == currentFormID;
    }

    // =========================================================================
    // EVENT HANDLERS
    // =========================================================================

    void SlotLocker::OnItemUsed(RE::FormID formID)
    {
        if (formID == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // Find and unlock any slot containing this item
        for (size_t i = 0; i < MAX_SLOTS; ++i) {
            auto& slot = m_lockedSlots[i];
            if (slot.isLocked && slot.assignment.formID == formID) {
                spdlog::info("[SlotLocker] Slot {} unlocked - item {:08X} was used",
                    i, formID);
                slot.isLocked = false;
                slot.remainingMs = 0.0f;
                // Don't break - item might be in multiple slots (unlikely but safe)
            }
        }
    }

    void SlotLocker::Reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& slot : m_lockedSlots) {
            slot.isLocked = false;
            slot.remainingMs = 0.0f;
            slot.totalDurationMs = 0.0f;
            slot.assignment = SlotAssignment::Empty(0, SlotClassification::Regular);
        }
        spdlog::info("[SlotLocker] Reset complete");
    }

    // =========================================================================
    // INTERNAL HELPERS
    // =========================================================================

    bool SlotLocker::ShouldLock(
        const SlotAssignment& oldAssign,
        const SlotAssignment& newAssign) const
    {
        // Locking disabled if duration is 0
        if (m_config.lockDurationMs <= 0.0f) {
            return false;
        }

        // Never lock empty slots
        if (newAssign.IsEmpty()) {
            return false;
        }

        // Lock when slot fills from empty
        if (m_config.lockOnFill && oldAssign.IsEmpty() && !newAssign.IsEmpty()) {
            return true;
        }

        // Lock on any content change (if duration > 0, we lock all changes)
        if (oldAssign.formID != newAssign.formID && newAssign.formID != 0) {
            return true;
        }

        return false;
    }

    bool SlotLocker::ShouldBreakLock(
        const LockedSlot& lock,
        const SlotAssignment& newAssign,
        const Override::OverrideCollection& overrides) const
    {
        // Always break if lock timer expired
        if (lock.remainingMs <= 0.0f) {
            return true;
        }

        // HIGH-PRIORITY OVERRIDES bypass the minimum lock duration entirely.
        // Safety overrides (health potions, drowning) should never be delayed
        // by anti-flicker timers — the player needs them immediately.
        if (m_config.overridesBreakLock &&
            newAssign.type == AssignmentType::Override &&
            overrides.HasActiveOverride())
        {
            for (const auto& ovr : overrides.activeOverrides) {
                if (ovr.active && ovr.priority >= m_config.immediateBreakPriority) {
                    spdlog::debug("[SlotLocker] Immediate lock break for override: {} (priority={})",
                        ovr.reason, ovr.priority);
                    return true;
                }
            }
        }

        // Calculate elapsed time since lock was created
        float elapsedMs = lock.totalDurationMs - lock.remainingMs;

        // Don't break before minimum duration (prevents flicker for non-override changes)
        if (elapsedMs < m_config.minLockDurationMs) {
            return false;
        }

        // Keep the lock - let it expire naturally or be broken by:
        // 1. Timer expiration (checked at top of function)
        // 2. High-priority overrides (checked above, bypasses minLockDurationMs)
        // 3. OnItemUsed() callback when player uses the item
        // Note: We intentionally do NOT break when newAssign is empty - that's
        // the "context window" use case (e.g., surfacing while Waterbreathing is shown)
        return false;
    }

}  // namespace Huginn::Slot
