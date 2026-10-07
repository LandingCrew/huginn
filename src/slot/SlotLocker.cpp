#include "SlotLocker.h"
#include "override/OverrideConditions.h"
#include "SlotClassifier.h"
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

        // An override showing an item in one slot releases any OTHER slot
        // still locked on that item, in the same pass. Otherwise the lock
        // keeps the old copy up, dedup clears one of the two, and the potion
        // bounced between them four times a second (2026-09-27 19:03:07). The
        // allocator never places an override's item twice; only a lock can.
        // A remembered item likewise: the thing you just took off belongs
        // under the key you pressed, not under a lock somewhere else.
        // And an item SEATING moved (SlotAssignment::seatMoved): a home-key
        // return swaps the gap-filler out of the returner's key, and the
        // gap-filler, there under three seconds, was nearly always still
        // locked -- the returner showed on the wrong key first, and the swap
        // landed a lock later, three visible changes for one (LoreRim
        // 2026-10-06 20:47:10, 20:50:17). Not for the hold's own moves: those
        // wait out the lock as before.
        for (size_t j = 0; j < newAssignments.size() && j < MAX_SLOTS; ++j) {
            auto& held = m_lockedSlots[j];
            if (!held.isLocked || held.assignment.IsEmpty()) continue;
            for (size_t i = 0; i < newAssignments.size() && i < MAX_SLOTS; ++i) {
                const auto& ovr = newAssignments[i];
                if (i != j && (ovr.IsPinned() || ovr.seatMoved) && ovr.formID == held.assignment.formID &&
                    ovr.uniqueID == held.assignment.uniqueID) {
                    const auto cause = ovr.IsOverride()   ? Telemetry::SlotChange::Override
                                     : ovr.IsRemembered() ? Telemetry::SlotChange::Remembrance
                                                          : Telemetry::SlotChange::Seated;
                    spdlog::debug("[SlotLocker] Slot {} lock released: its item moved to {} slot {}",
                        j, Telemetry::SlotChangeName(cause), i);
                    held.isLocked = false;
                    held.remainingMs = 0.0f;
                    held.releaseCause = cause;
                    break;
                }
            }
        }

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
        DedupePreferLocked(result, newAssignments);

        // Churn: compare what is shown now against what was shown last run.
        const auto runNow = std::chrono::steady_clock::now();
        for (size_t i = 0; i < result.size() && i < MAX_SLOTS; ++i) {
            const auto& shown = result[i];
            auto& slot = m_lockedSlots[i];
            const bool nowEmpty = shown.IsEmpty();
            const bool changed = nowEmpty != slot.shownEmpty ||
                (!nowEmpty && (shown.formID != slot.shownFormID || shown.uniqueID != slot.shownUniqueID));
            // Counted only when the player could SEE it: the slot emptied or
            // filled, or the name on it changed. Two forms with one name -- the
            // per-hand Unarmed pseudo-items, a stack swapped for its twin --
            // read 'Unarmed' -> 'Unarmed' and were counted as churn. The identity
            // is still recorded below, so the next real change compares right.
            const bool visible = nowEmpty != slot.shownEmpty ||
                (!nowEmpty && shown.name != slot.shownName);

            if (changed && visible && !m_churnBaseline) {
                const auto cause = Telemetry::ClassifySlotChange(slot.shownEmpty, nowEmpty,
                    filledBeforeDedup[i] && nowEmpty, shown.IsOverride(),
                    shown.IsWildcard() || slot.shownWildcard, slot.releaseCause,
                    shown.IsRemembered() || slot.shownRemembered);

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
                // Tenure: how long the item being replaced was on the slot.
                float tenureSec = -1.0f;
                if (!slot.shownEmpty && slot.shownSince != std::chrono::steady_clock::time_point{}) {
                    tenureSec = std::chrono::duration<float>(runNow - slot.shownSince).count();
                }
                changes[changeCount++] = { i, cause, ratio, tenureSec };

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
            // The key's age restarts on what the player can SEE change -- a
            // same-name swap keeps it, a page switch or the baseline fill
            // after a load starts it.
            if (visible) {
                slot.prevName = slot.shownEmpty ? std::string{} : slot.shownName;
                slot.shownSince = runNow;
            }
            if (changed || visible) {
                slot.shownEmpty = nowEmpty;
                slot.shownWildcard = !nowEmpty && shown.IsWildcard();
                slot.shownRemembered = !nowEmpty && shown.IsRemembered();
                slot.shownFormID = nowEmpty ? 0 : shown.formID;
                slot.shownUniqueID = nowEmpty ? 0 : shown.uniqueID;
                slot.shownName = nowEmpty ? std::string{} : shown.name;
            }

            // A held lock means the next change is not "because the lock let
            // go" -- it has to let go again first. A page switch explains one
            // run only: everything on the new page arrives in that run.
            //
            // Used, Override and Remembrance are one-run causes too. A slot that stays
            // unlocked showing the same item -- a potion with count left, an
            // override that handed back the same form, or any slot with
            // locking disabled -- never relocks, so without this the event
            // would take the credit for whatever re-rank moves the slot
            // minutes later, and keep that change out of the ratio. What is
            // left afterwards is an unheld slot: Expired if locking is on
            // (its lock is gone, however it went), Unheld if it is off.
            if (slot.isLocked || slot.releaseCause == Telemetry::SlotChange::Page) {
                slot.releaseCause = Telemetry::SlotChange::Unheld;
            } else if (slot.releaseCause == Telemetry::SlotChange::Used ||
                       slot.releaseCause == Telemetry::SlotChange::Override ||
                       slot.releaseCause == Telemetry::SlotChange::Remembrance ||
                       slot.releaseCause == Telemetry::SlotChange::Seated) {
                slot.releaseCause = m_config.lockDurationMs > 0.0f
                    ? Telemetry::SlotChange::Expired : Telemetry::SlotChange::Unheld;
            }
        }
        m_churnBaseline = false;

        // Under m_mutex, which is safe: SoakMetrics takes only its own mutex
        // and never calls back into the locker.
        Telemetry::SoakMetrics::GetSingleton().RecordSlotChanges(
            std::span{ changes.data(), changeCount }, runNow);

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

    std::optional<SlotLocker::KeyAge> SlotLocker::GetKeyAge(size_t slotIndex) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (slotIndex >= MAX_SLOTS) return std::nullopt;
        const auto& slot = m_lockedSlots[slotIndex];
        if (slot.shownSince == std::chrono::steady_clock::time_point{}) return std::nullopt;
        return KeyAge{ std::chrono::duration<float>(std::chrono::steady_clock::now() - slot.shownSince).count(),
                       slot.prevName };
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

        // A Remembrance hold is the player's own undo and may land on a key
        // other than the one pressed (sRemembranceTarget = Job): it takes the
        // key now, like an override, rather than after its lock runs out.
        if (newAssign.IsRemembered() && newAssign.formID != lock.assignment.formID) {
            return true;
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

    void SlotLocker::DedupePreferLocked(SlotAssignments& result, const SlotAssignments& wanted)
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
        std::array<bool, MAX_SLOTS> cleared{};
        for (size_t i = 0; i < result.size() && i < MAX_SLOTS; ++i) {
            auto& assignment = result[i];
            if (assignment.IsEmpty()) continue;

            const bool isLocked = m_lockedSlots[i].isLocked;
            if (!isLocked && lockedNames.contains(assignment.name)) {
                spdlog::debug("[SlotLocker] Post-lock dedup: clearing unlocked '{}' from slot {} (locked elsewhere)",
                    assignment.name, assignment.slotIndex);
                assignment = SlotAssignment::Empty(assignment.slotIndex, assignment.classification);
                cleared[i] = true;
                continue;
            }

            auto [it, inserted] = seen.insert(assignment.name);
            if (!inserted) {
                spdlog::debug("[SlotLocker] Post-lock dedup: clearing duplicate '{}' from slot {}",
                    assignment.name, assignment.slotIndex);
                assignment = SlotAssignment::Empty(assignment.slotIndex, assignment.classification);
                cleared[i] = true;
            }
        }

        // Third pass: don't leave a hole. A duplicate exists because a lock
        // kept an item in one slot while the allocator moved it to another --
        // so the item the allocator WANTED in that locked slot is on screen
        // nowhere. Put it in the slot dedup just emptied. Before this the slot
        // stayed blank, and stayed LOCKED on an item it could not show, until
        // the other lock expired: slot 6 blank for 0.9 s at 2026-09-27
        // 19:42:21 while Minor Magicka, wanted in locked slot 3, was shown
        // nowhere (up to fLockDurationMs in general).
        std::set<std::string_view> shown;
        for (size_t i = 0; i < result.size() && i < MAX_SLOTS; ++i) {
            if (!result[i].IsEmpty()) shown.insert(result[i].name);
        }
        for (size_t j = 0; j < result.size() && j < MAX_SLOTS; ++j) {
            if (!cleared[j]) continue;

            // The cleared slot holds nothing: whatever its lock was holding,
            // it can no longer show it.
            auto& slotJ = m_lockedSlots[j];
            slotJ.isLocked = false;
            slotJ.remainingMs = 0.0f;
            slotJ.isActivationLock = false;

            // The slot's OWN pick first: when the emptied slot is itself the
            // one whose lock kept the old copy, what the allocator wanted
            // there is the natural fill (slot 4 locked on a dagger the
            // allocator had moved to slot 0 -- it wanted Ale in 4, and 4
            // sat blank for half a second, 2026-09-27 20:55:36). Then items
            // other locks kept off screen -- never from a slot cleared in
            // this same pass, which holds nothing any more.
            for (size_t n = 0; n <= wanted.size() && n <= MAX_SLOTS; ++n) {
                const size_t i = (n == 0) ? j : n - 1;
                if (i >= wanted.size() || i >= MAX_SLOTS) continue;
                if (n > 0 && (i == j || !m_lockedSlots[i].isLocked || cleared[i])) continue;
                const auto& homeless = wanted[i];
                // Overrides, wildcards and remembered items were placed
                // deliberately; never moved.
                if (homeless.IsEmpty() || homeless.IsPinned() || homeless.IsWildcard() ||
                    !homeless.candidate || shown.contains(homeless.name)) {
                    continue;
                }
                // The locker cannot see bSkipEquipped, so never fill with an
                // equipped item: a slot configured to exclude it would show it.
                if (Candidate::GetBase(homeless.candidate->candidate).isEquipped) {
                    continue;
                }
                if (!SlotClassifier::Matches(*homeless.candidate, result[j].classification)) {
                    continue;
                }
                const auto classification = result[j].classification;
                result[j] = homeless;
                result[j].slotIndex = j;
                result[j].classification = classification;
                shown.insert(homeless.name);
                // slotJ.assignment deliberately still records what the lock /
                // allocator gave slot j, not the fill. Recording the fill made
                // the next run see the allocator's unchanged pick as new
                // content, lock it again, and -- with j below i -- let it win
                // dedup and evict the ORIGINAL lock, leaving that slot blank
                // (/code-review on #146). Unlocked, slot j settles on its own.
                if (slotJ.shownName != homeless.name) {
                    if (i == j) {
                        spdlog::debug("[SlotLocker] Post-lock dedup: slot {} filled with its own pick '{}'", j, homeless.name);
                    } else {
                        spdlog::debug("[SlotLocker] Post-lock dedup: slot {} filled with '{}', which locked slot {} kept off screen",
                            j, homeless.name, i);
                    }
                }
                break;
            }
        }
    }

}  // namespace Huginn::Slot
