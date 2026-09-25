#include "SlotLocker.h"
#include "override/OverrideConditions.h"
#include <chrono>
#include <set>
#include <span>
#include <spdlog/spdlog.h>

namespace Huginn::Slot
{
    static_assert(Telemetry::SLOT_CHURN_SLOTS == MAX_SLOTS_PER_PAGE,
        "SoakMetrics' per-slot churn history must cover every slot on a page");

    namespace
    {
        // Truncate the registry-string borrow when an assignment enters
        // cross-tick storage: the embedded candidate's name is a string_view
        // into registry-owned strings, which registry reconcile/swap-pop can
        // invalidate between pipeline runs (the moved last entry relocates
        // too, not just the removed one). Every reader of locked-slot content
        // (ApplyLocks re-emission, ToSlotContent, DeriveExplanationLabel,
        // WheelerBackend, IntuitionMenu::BuildSlotDetail) uses the owned
        // SlotAssignment::name or POD candidate fields, so the view must stay
        // empty while stored here — never read candidate name from a
        // LockedSlot.
        void TruncateCandidateViews(SlotAssignment& stored) noexcept
        {
            if (stored.candidate) {
                Candidate::GetBase(stored.candidate->candidate).name = {};
            }
        }
    }

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

    bool SlotLocker::Update(float deltaMs)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Decay lock timers for all slots
        bool anyExpired = false;
        for (auto& slot : m_lockedSlots) {
            if (slot.isLocked && slot.remainingMs > 0.0f) {
                slot.remainingMs -= deltaMs;

                if (slot.remainingMs <= 0.0f) {
                    slot.isLocked = false;
                    slot.remainingMs = 0.0f;
                    slot.releaseCause = Telemetry::SlotChange::Expired;
                    anyExpired = true;
                    spdlog::debug("[SlotLocker] Lock expired for slot with FormID {:08X}",
                        slot.assignment.formID);
                }
            }
        }
        return anyExpired;
    }

    SlotAssignments SlotLocker::ApplyLocks(
        const SlotAssignments& newAssignments,
        const Override::OverrideCollection& overrides,
        std::span<const Scoring::ScoredCandidate> scored)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        SlotAssignments result;
        std::array<Telemetry::SlotChangeEvent, MAX_SLOTS> changes{};
        size_t changeCount = 0;
        result.reserve(newAssignments.size());

        for (size_t i = 0; i < newAssignments.size() && i < MAX_SLOTS; ++i) {
            const auto& newAssign = newAssignments[i];
            auto& lockedSlot = m_lockedSlots[i];

            if (lockedSlot.isLocked) {
                // Slot is currently locked - check if lock should break
                if (ShouldBreakLock(lockedSlot, newAssign, overrides)) {
                    // ShouldBreakLock lets go for exactly two reasons: the timer
                    // (normally already caught by Update) or an override.
                    const bool expired = lockedSlot.remainingMs <= 0.0f;
                    lockedSlot.releaseCause = expired
                        ? Telemetry::SlotChange::Expired : Telemetry::SlotChange::Override;
                    spdlog::debug("[SlotLocker] Slot {} lock broken (was {:08X}): {}",
                        i, lockedSlot.assignment.formID, expired ? "expired" : "override");
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
                TruncateCandidateViews(lockedSlot.assignment);
                lockedSlot.remainingMs = m_config.lockDurationMs;
                lockedSlot.totalDurationMs = m_config.lockDurationMs;
                lockedSlot.isLocked = true;
                lockedSlot.isActivationLock = false;

                spdlog::debug("[SlotLocker] Slot {} locked for {:.0f}ms: {} ({:08X})",
                    i, m_config.lockDurationMs, newAssign.name, newAssign.formID);
            } else {
                // No lock needed - just track the assignment for comparison next frame
                lockedSlot.assignment = newAssign;
                TruncateCandidateViews(lockedSlot.assignment);
            }

            // Update FormID history for confirmed detection
            lockedSlot.previousFormID = lockedSlot.assignment.formID;
            lockedSlot.hadContent = !lockedSlot.assignment.IsEmpty();

            result.push_back(newAssign);
        }

        // Which slots had content going into dedup, so a slot dedup empties
        // can be told apart from one the allocator left empty.
        std::array<bool, MAX_SLOTS> filledBeforeDedup{};
        for (size_t i = 0; i < result.size() && i < MAX_SLOTS; ++i) {
            filledBeforeDedup[i] = !result[i].IsEmpty();
        }

        // Locks can reintroduce an item the allocator placed in another slot —
        // dedup here (where lock state is known) so locked content always wins.
        DedupePreferLocked(result);

        // Churn: compare what is shown now against what was shown last run.
        for (size_t i = 0; i < result.size() && i < MAX_SLOTS; ++i) {
            const auto& shown = result[i];
            auto& slot = m_lockedSlots[i];
            const bool nowEmpty = shown.IsEmpty();
            const bool changed = nowEmpty != slot.shownEmpty ||
                (!nowEmpty && (shown.formID != slot.shownFormID || shown.uniqueID != slot.shownUniqueID));

            if (changed && !m_churnBaseline) {
                const auto cause = Telemetry::ClassifySlotChange(slot.shownEmpty, nowEmpty,
                    filledBeforeDedup[i] && nowEmpty, shown.IsOverride(), slot.releaseCause);

                // Challenger ratio, for the changes a margin would govern.
                auto ratio = Telemetry::ChallengerRatio::NotApplicable;
                float incumbentUtility = -1.0f;  // < 0 = no longer a candidate
                if (!scored.empty() &&
                    (cause == Telemetry::SlotChange::Expired || cause == Telemetry::SlotChange::Unheld)) {
                    for (const auto& sc : scored) {
                        if (sc.GetFormID() == slot.shownFormID && sc.GetUniqueID() == slot.shownUniqueID) {
                            incumbentUtility = sc.utility;
                            break;
                        }
                    }
                    ratio = Telemetry::BucketChallengerRatio(shown.utility, incumbentUtility);
                }
                changes[changeCount++] = { i, cause, ratio };

                const std::string_view from = slot.shownEmpty ? std::string_view{} : std::string_view{ slot.shownName };
                const std::string_view to = nowEmpty ? std::string_view{} : std::string_view{ shown.name };
                if (ratio == Telemetry::ChallengerRatio::NotApplicable) {
                    spdlog::debug("[SlotChurn] Slot {}: '{}' -> '{}' ({})", i, from, to,
                        Telemetry::SlotChangeName(cause));
                } else if (incumbentUtility < 0.0f) {
                    spdlog::debug("[SlotChurn] Slot {}: '{}' -> '{}' ({}, u={:.3f}, incumbent gone)",
                        i, from, to, Telemetry::SlotChangeName(cause), shown.utility);
                } else {
                    spdlog::debug("[SlotChurn] Slot {}: '{}' -> '{}' ({}, u={:.3f} vs {:.3f}, x{:.2f})",
                        i, from, to, Telemetry::SlotChangeName(cause), shown.utility, incumbentUtility,
                        incumbentUtility > 0.0f ? shown.utility / incumbentUtility : 0.0f);
                }
            }
            if (changed) {
                slot.shownEmpty = nowEmpty;
                slot.shownFormID = nowEmpty ? 0 : shown.formID;
                slot.shownUniqueID = nowEmpty ? 0 : shown.uniqueID;
                slot.shownName = nowEmpty ? std::string{} : shown.name;
            }

            // A held lock means the next change is not "because the lock let
            // go" -- it has to let go again first. A page switch explains one
            // run only: everything on the new page arrives in that run.
            if (slot.isLocked || slot.releaseCause == Telemetry::SlotChange::Page) {
                slot.releaseCause = Telemetry::SlotChange::Unheld;
            }
        }
        m_churnBaseline = false;

        // Under m_mutex, which is safe: SoakMetrics takes only its own mutex
        // and never calls back into the locker.
        Telemetry::SoakMetrics::GetSingleton().RecordSlotChanges(
            std::span{ changes.data(), changeCount }, std::chrono::steady_clock::now());

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
        slot.isActivationLock = false;

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
        slot.isActivationLock = true;

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
            // Every slot, locked or not: the whole page's content is about to
            // be replaced, and the player asked for that. Only caller is
            // SlotAllocator::SetCurrentPage.
            slot.releaseCause = Telemetry::SlotChange::Page;
        }
        spdlog::debug("[SlotLocker] All slots unlocked");
    }

    // =========================================================================
    // QUERY METHODS
    // =========================================================================

    std::array<SlotLocker::SlotLockView, SlotLocker::MAX_SLOTS> SlotLocker::GetLockSnapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::array<SlotLockView, MAX_SLOTS> snapshot;
        for (size_t i = 0; i < MAX_SLOTS; ++i) {
            const auto& slot = m_lockedSlots[i];
            snapshot[i] = SlotLockView{
                .isLocked = slot.isLocked,
                .remainingMs = slot.isLocked ? slot.remainingMs : 0.0f,
                .previousFormID = slot.previousFormID,
                .hadContent = slot.hadContent,
            };
        }
        return snapshot;
    }

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

    void SlotLocker::OnItemUsed(RE::FormID formID, bool respectActivationLock)
    {
        OnItemUsed(formID, 0, respectActivationLock);
    }

    void SlotLocker::OnItemUsed(RE::FormID formID, uint16_t uniqueID, bool respectActivationLock)
    {
        if (formID == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // Find and unlock any slot containing this item
        for (size_t i = 0; i < MAX_SLOTS; ++i) {
            auto& slot = m_lockedSlots[i];
            // uniqueID 0 is "every stack of this form", which is both the old
            // behaviour and the right one for anything the player cannot tell
            // apart. A named stack matches only itself: evicting the tempered
            // dagger's twin because THIS one was sold is the mistake this
            // parameter exists to stop.
            const bool sameItem = slot.assignment.formID == formID &&
                (uniqueID == 0 || slot.assignment.uniqueID == uniqueID);
            if (!sameItem) {
                continue;
            }
            if (!slot.isLocked) {
                // Nothing to break, but the item leaving is still why this
                // slot is about to change -- a lock that had already expired
                // would otherwise take the credit.
                slot.releaseCause = Telemetry::SlotChange::Used;
                continue;
            }
            // Preserve Sticky's deliberate 10s hold: the inventory delta-scan
            // path (respectActivationLock) must not evict a just-activated item
            // the instant it's consumed — that's exactly the case Sticky exists
            // for. It still expires on its own timer.
            if (respectActivationLock && slot.isActivationLock) {
                continue;
            }
            spdlog::info("[SlotLocker] Slot {} unlocked - item {:08X}/uid{} was used",
                i, formID, slot.assignment.uniqueID);
            slot.isLocked = false;
            slot.remainingMs = 0.0f;
            slot.isActivationLock = false;
            slot.releaseCause = Telemetry::SlotChange::Used;
            // Don't break - item might be in multiple slots (unlikely but safe)
        }
    }

    void SlotLocker::Reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Whole-struct reset, deliberately. Clearing fields one by one leaked
        // isActivationLock, previousFormID and hadContent across a save load.
        //
        // That leak was LATENT, not a bug anyone could observe. OnItemUsed
        // guards on slot.isLocked && before it consults isActivationLock (:284),
        // and every path that re-sets isLocked writes the flag anyway (:117,
        // :157, :174); previousFormID/hadContent have one reader outside this
        // class (SlotUtils.h ComputeVisualStates), and PipelineCoordinator runs
        // ApplyLocks three lines upstream of it, which rewrites both for every
        // slot. Do not re-file this as a Confirmed-flash or stuck-lock defect
        // without re-checking those consumers first.
        //
        // Fixed regardless, because both safeties are arrangement rather than
        // construction: the "isActivationLock is only meaningful while isLocked"
        // invariant is unenforced, and previousFormID is safe only while every
        // reader sits downstream of ApplyLocks. Assigning a default-constructed
        // LockedSlot means a field added later cannot reintroduce the omission.
        for (auto& slot : m_lockedSlots) {
            slot = LockedSlot{};
        }
        m_churnBaseline = true;
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
        //
        // Only the override actually targeting THIS slot may break its lock:
        // match by FormID so a high-priority HP override can't break an
        // unrelated slot's lock (e.g., a BuffsAny slot whose new assignment
        // happens to be flagged Override on a different page layout).
        if (m_config.overridesBreakLock &&
            newAssign.type == AssignmentType::Override &&
            overrides.HasActiveOverride())
        {
            for (const auto& ovr : overrides.activeOverrides) {
                if (!ovr.candidate) continue;
                if (ovr.priority < m_config.immediateBreakPriority) continue;
                if (Candidate::GetFormID(*ovr.candidate) != newAssign.formID) continue;

                spdlog::debug("[SlotLocker] Immediate lock break for override: {} (priority={})",
                    ovr.reason, ovr.priority);
                return true;
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

    void SlotLocker::DedupePreferLocked(SlotAssignments& result) const
    {
        // `result` is built 1:1 per slot by ApplyLocks, so result[i] corresponds
        // to m_lockedSlots[i] (i == slotIndex). The index-parallel access below
        // relies on that invariant.

        // First pass: collect names held by LOCKED slots — these win ties.
        std::set<std::string_view> lockedNames;
        for (size_t i = 0; i < result.size() && i < MAX_SLOTS; ++i) {
            if (!result[i].IsEmpty() && m_lockedSlots[i].isLocked) {
                lockedNames.insert(result[i].name);
            }
        }

        // Second pass: clear duplicates. An unlocked slot whose name is held by a
        // locked slot is cleared outright; otherwise keep first occurrence.
        std::set<std::string_view> seen;
        for (size_t i = 0; i < result.size() && i < MAX_SLOTS; ++i) {
            auto& assignment = result[i];
            if (assignment.IsEmpty()) continue;

            const bool isLocked = m_lockedSlots[i].isLocked;
            if (!isLocked && lockedNames.contains(assignment.name)) {
                spdlog::debug("[SlotLocker] Post-lock dedup: clearing unlocked '{}' from slot {} (locked elsewhere)",
                    assignment.name, assignment.slotIndex);
                assignment = SlotAssignment::Empty(assignment.slotIndex, assignment.classification);
                continue;
            }

            auto [it, inserted] = seen.insert(assignment.name);
            if (!inserted) {
                spdlog::debug("[SlotLocker] Post-lock dedup: clearing duplicate '{}' from slot {}",
                    assignment.name, assignment.slotIndex);
                assignment = SlotAssignment::Empty(assignment.slotIndex, assignment.classification);
            }
        }
    }

}  // namespace Huginn::Slot
