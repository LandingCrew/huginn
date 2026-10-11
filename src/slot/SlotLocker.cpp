#include "SlotLocker.h"
#include "override/OverrideConditions.h"
#include "SlotClassifier.h"
#include "core/CombatLock.h"
#include "core/DedupRules.h"
#include <chrono>
#include <cmath>
#include <optional>
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

        [[nodiscard]] bool SameItem(const SlotAssignment& a, const SlotAssignment& b) noexcept
        {
            if (a.IsEmpty() || b.IsEmpty()) {
                return a.IsEmpty() == b.IsEmpty();
            }
            return a.formID == b.formID && a.uniqueID == b.uniqueID;
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
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = config;
        spdlog::info("[SlotLocker] Config updated: lockDuration={:.0f}ms, minDuration={:.0f}ms, "
                     "lockOnFill={}, overridesBreakLock={}, immediateBreakPriority={}, combatLock={}",
            m_config.lockDurationMs,
            m_config.minLockDurationMs,
            m_config.lockOnFill,
            m_config.overridesBreakLock,
            m_config.immediateBreakPriority,
            m_config.combatLockDurationMs < 0.0f ? std::string("freeze")
                : m_config.combatLockDurationMs == 0.0f ? std::string("normal lock")
                : std::format("{:.0f}ms", m_config.combatLockDurationMs));
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
            if (slot.isLocked && !slot.frozen && slot.remainingMs > 0.0f) {
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

    SlotLockConfig SlotLocker::GetConfig() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config;
    }

    bool SlotLocker::SetInCombat(bool inCombat)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (inCombat == m_inCombat) {
            return false;
        }
        m_inCombat = inCombat;
        if (inCombat) {
            // The locks already running stay as they are; ApplyLocks locks
            // every key showing an item under the combat setting from the next
            // run on (the sweep at its end).
            spdlog::debug("[SlotLocker] Combat: keys lock {} while it lasts",
                m_config.combatLockDurationMs < 0.0f ? std::string("until it ends")
                : std::format("for {:.0f}ms", m_config.combatLockDurationMs));
            return false;
        }
        // Combat over: no re-shuffle on the last kill. Each combat lock falls
        // back to the normal timer counted from now (or what it had left, if
        // shorter), and is a normal lock from here on.
        bool released = false;
        size_t converted = 0;
        for (auto& slot : m_lockedSlots) {
            if (!slot.combatLock) continue;
            if (slot.isLocked) {
                const float left = Core::RemainingAfterCombat(slot.remainingMs, slot.frozen, m_config.lockDurationMs);
                if (left > 0.0f) {
                    slot.remainingMs = left;
                    slot.totalDurationMs = left;
                    ++converted;
                } else {
                    slot.isLocked = false;
                    slot.remainingMs = 0.0f;
                    slot.releaseCause = Telemetry::SlotChange::Expired;
                    released = true;
                }
            }
            slot.combatLock = false;
            slot.frozen = false;
        }
        spdlog::debug("[SlotLocker] Combat over: {} combat lock(s) now run the normal {:.0f}ms timer",
            converted, m_config.lockDurationMs);
        return released;
    }

    void SlotLocker::RestartLocks()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto d = Core::EffectiveLockDuration(m_inCombat, m_config.lockDurationMs, m_config.combatLockDurationMs);
        for (auto& slot : m_lockedSlots) {
            if (!slot.isLocked || slot.frozen || slot.isActivationLock) continue;
            const float full = slot.combatLock ? d.ms : m_config.lockDurationMs;
            if (slot.remainingMs < full) {
                slot.remainingMs = full;
                slot.totalDurationMs = full;
            }
        }
    }

    void SlotLocker::LockNow(LockedSlot& slot, const SlotAssignment& assignment)
    {
        const auto d = Core::EffectiveLockDuration(m_inCombat, m_config.lockDurationMs, m_config.combatLockDurationMs);
        slot.assignment = assignment;
        TruncateCandidateViews(slot.assignment);
        if (!Core::LockingEnabled(d)) {
            return;
        }
        slot.remainingMs = d.ms;
        slot.totalDurationMs = d.ms;
        slot.isLocked = true;
        slot.isActivationLock = false;
        slot.combatLock = d.combat;
        slot.frozen = d.frozen;
        slot.lockedAt = std::chrono::steady_clock::now();
    }

    SlotAssignments SlotLocker::ApplyLocks(
        const SlotAssignments& newAssignments,
        const Override::OverrideCollection& overrides,
        std::span<const Scoring::ScoredCandidate> scored,
        const RunContext& run)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto lockSpec = Core::EffectiveLockDuration(m_inCombat, m_config.lockDurationMs, m_config.combatLockDurationMs);

        SlotAssignments result;
        std::array<Telemetry::SlotChangeEvent, MAX_SLOTS> changes{};
        size_t changeCount = 0;
        result.reserve(newAssignments.size());

        // A combat-locked key whose item stopped being usable without an
        // inventory event: a spell or shout no longer known, or an item now
        // equipped with no Remembrance hold on the key (pressed into an empty
        // hand, or equipped from the inventory) -- the frozen key would show
        // it for the rest of the fight. Released to normal fill; the sweep at
        // the end re-locks whatever the key shows next.
        if (m_inCombat && run.probeItem) {
            for (size_t i = 0; i < std::min(newAssignments.size(), MAX_SLOTS); ++i) {
                auto& slot = m_lockedSlots[i];
                if (!slot.isLocked || !slot.combatLock || slot.assignment.IsEmpty() || slot.assignment.formID == 0) {
                    continue;
                }
                auto state = run.probeItem(slot.assignment.formID, slot.assignment.uniqueID);
                // Gone only if this run did not score it either: a spell the
                // game call misses (a racial power, say) but the generator still
                // produces is plainly known, and releasing it would re-lock the
                // same spell every run.
                if (state.gone) {
                    for (const auto& sc : scored) {
                        if (sc.GetFormID() == slot.assignment.formID) {
                            state.gone = false;
                            break;
                        }
                    }
                }
                const auto release = Core::DecideCombatLockItemRelease(state.gone, state.equipped,
                    slot.assignment.IsPinned(), SameItem(newAssignments[i], slot.assignment));
                if (release == Core::CombatLockItemRelease::None) continue;
                spdlog::debug("[SlotLocker] Slot {} combat lock released: '{}' {}", i, slot.assignment.name,
                    release == Core::CombatLockItemRelease::Gone ? "is no longer known" : "is equipped");
                slot.isLocked = false;
                slot.remainingMs = 0.0f;
                slot.combatLock = false;
                slot.frozen = false;
                slot.releaseCause = Telemetry::SlotChange::Used;
            }
        }

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
        //
        // A seating move releases only if the item will SHOW at its
        // destination: that slot is unlocked, or its own lock is letting go
        // in this pass too (a swap, a chain). Into a slot still locked on
        // something else, releasing would hide the item until that lock ran
        // out (code review of #179); the old slot keeps showing it instead.
        const size_t n = std::min(newAssignments.size(), MAX_SLOTS);
        std::array<size_t, MAX_SLOTS> dest{};
        dest.fill(SIZE_MAX);
        for (size_t j = 0; j < n; ++j) {
            const auto& held = m_lockedSlots[j];
            if (!held.isLocked || held.assignment.IsEmpty()) continue;
            for (size_t i = 0; i < n; ++i) {
                const auto& ovr = newAssignments[i];
                if (i != j && (ovr.IsPinned() || ovr.seatMoved) && ovr.formID == held.assignment.formID &&
                    ovr.uniqueID == held.assignment.uniqueID) {
                    // Under a combat lock an item does not move between keys:
                    // a home-key return or re-seat waits for combat to end
                    // (2026-10-11: 15 of 55 fast changes in two fights were
                    // these moves). An override or Remembrance still moves it.
                    if (!ovr.IsPinned() && held.combatLock && m_inCombat) {
                        break;
                    }
                    dest[j] = i;
                    break;
                }
            }
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t j = 0; j < n; ++j) {
                const size_t i = dest[j];
                if (i == SIZE_MAX || newAssignments[i].IsPinned()) continue;
                if (m_lockedSlots[i].isLocked && dest[i] == SIZE_MAX) {
                    dest[j] = SIZE_MAX;
                    changed = true;
                }
            }
        }
        for (size_t j = 0; j < n; ++j) {
            const size_t i = dest[j];
            if (i == SIZE_MAX) continue;
            auto& held = m_lockedSlots[j];
            const auto& ovr = newAssignments[i];
            const auto cause = ovr.IsOverride()   ? Telemetry::SlotChange::Override
                             : ovr.IsRemembered() ? Telemetry::SlotChange::Remembrance
                                                  : Telemetry::SlotChange::Seated;
            spdlog::debug("[SlotLocker] Slot {} lock released: its item moved to {} slot {}",
                j, Telemetry::SlotChangeName(cause), i);
            held.isLocked = false;
            held.remainingMs = 0.0f;
            held.releaseCause = cause;
        }

        for (size_t i = 0; i < newAssignments.size() && i < MAX_SLOTS; ++i) {
            const auto& newAssign = newAssignments[i];
            auto& lockedSlot = m_lockedSlots[i];

            if (lockedSlot.isLocked) {
                // Slot is currently locked - check if lock should break
                const auto why = ShouldBreakLock(lockedSlot, newAssign, overrides,
                    i < run.remembranceOnPressedKey.size() && run.remembranceOnPressedKey[i]);
                if (why != BreakReason::None) {
                    // Named for what actually let go. This line said "override"
                    // for every break that was not a timer, so a Remembrance
                    // hold taking its key read as an override breaking an
                    // unrelated key's lock (2026-10-11 08:57:37.747, slot 3).
                    const char* whyName = "expired";
                    switch (why) {
                    case BreakReason::Expired:
                        lockedSlot.releaseCause = Telemetry::SlotChange::Expired;
                        break;
                    case BreakReason::Override:
                        lockedSlot.releaseCause = Telemetry::SlotChange::Override;
                        whyName = "override on this key";
                        break;
                    case BreakReason::Remembrance:
                        lockedSlot.releaseCause = Telemetry::SlotChange::Remembrance;
                        whyName = "Remembrance";
                        break;
                    case BreakReason::PinnedEnded:
                        lockedSlot.releaseCause = Telemetry::SlotChange::Expired;
                        whyName = "its override or Remembrance ended";
                        break;
                    case BreakReason::Restyle:
                        // Same item, no longer (or now) an override /
                        // Remembrance item: the run takes the new assignment
                        // and the combat sweep below re-locks it as that.
                        whyName = "same item, override/Remembrance state changed";
                        break;
                    case BreakReason::None:
                        break;
                    }
                    spdlog::debug("[SlotLocker] Slot {} lock broken (was {:08X}): {}",
                        i, lockedSlot.assignment.formID, whyName);
                    lockedSlot.isLocked = false;
                    lockedSlot.remainingMs = 0.0f;
                    lockedSlot.combatLock = false;
                    lockedSlot.frozen = false;
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
                LockNow(lockedSlot, newAssign);
                if (lockSpec.frozen) {
                    spdlog::debug("[SlotLocker] Slot {} locked until combat ends: {} ({:08X})",
                        i, newAssign.name, newAssign.formID);
                } else {
                    spdlog::debug("[SlotLocker] Slot {} locked for {:.0f}ms: {} ({:08X})",
                        i, lockSpec.ms, newAssign.name, newAssign.formID);
                }
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

        // Combat sweep: in combat every key that shows an item is locked under
        // the combat setting -- the ones that entered combat unlocked (a lock
        // that ran out while the hold kept the item), a dedup refill, anything
        // the paths above left unlocked. Otherwise such a key stays open to
        // every challenger for the whole fight. Empty keys stay unlocked and
        // fill normally.
        if (m_inCombat && lockSpec.combat) {
            for (size_t i = 0; i < result.size() && i < MAX_SLOTS; ++i) {
                auto& slot = m_lockedSlots[i];
                if (result[i].IsEmpty() || slot.isLocked) continue;
                LockNow(slot, result[i]);
                slot.previousFormID = slot.assignment.formID;
                slot.hadContent = true;
                spdlog::debug("[SlotLocker] Slot {} combat-locked {}: {}", i,
                    lockSpec.frozen ? std::string("until combat ends") : std::format("for {:.0f}ms", lockSpec.ms),
                    result[i].name);
            }
        }

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
                // On scores (any sign): the incumbent's is empty when it is
                // no longer a candidate -- no score value stands for "gone".
                auto ratio = Telemetry::ChallengerRatio::NotApplicable;
                std::optional<Core::SlotScore> incumbentScore;
                if (!scored.empty() &&
                    (cause == Telemetry::SlotChange::Expired || cause == Telemetry::SlotChange::Unheld)) {
                    for (const auto& sc : scored) {
                        if (sc.GetFormID() == slot.shownFormID && sc.GetUniqueID() == slot.shownUniqueID) {
                            incumbentScore = sc.SlotScore();
                            break;
                        }
                    }
                    ratio = Telemetry::BucketChallengerRatio(shown.score, incumbentScore);
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
                } else if (!incumbentScore) {
                    spdlog::debug("[SlotChurn] Slot {}: '{}' -> '{}' ({}, s={:+.3f}, incumbent gone)",
                        i, from, to, Telemetry::SlotChangeName(cause), shown.score);
                } else {
                    // The difference is the log of the old ratio: exp() of it
                    // is always positive, whatever the scores' signs.
                    const double delta = shown.score - *incumbentScore;
                    spdlog::debug("[SlotChurn] Slot {}: '{}' -> '{}' ({}, s={:+.3f} vs {:+.3f}, {:+.3f} = x{:.2f})",
                        i, from, to, Telemetry::SlotChangeName(cause), shown.score, *incumbentScore, delta,
                        std::exp(delta));
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
        slot.combatLock = false;
        slot.frozen = false;
        slot.lockedAt = std::chrono::steady_clock::now();

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
        slot.combatLock = false;
        slot.frozen = false;
        slot.lockedAt = std::chrono::steady_clock::now();

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
            slot.combatLock = false;
            slot.frozen = false;
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
            slot.combatLock = false;
            slot.frozen = false;
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
                .frozen = slot.isLocked && slot.frozen,
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

    void SlotLocker::OnItemUsed(RE::FormID formID, bool respectActivationLock, UseKind kind)
    {
        OnItemUsed(formID, 0, respectActivationLock, kind);
    }

    void SlotLocker::OnItemUsed(RE::FormID formID, uint16_t uniqueID, bool respectActivationLock, UseKind kind)
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
            // A combat lock gives way only when the item is GONE: a potion with
            // more in the stack stays on its key for the rest of the fight.
            if (kind == UseKind::Used && slot.combatLock && m_inCombat) {
                continue;
            }
            spdlog::info("[SlotLocker] Slot {} unlocked - item {:08X}/uid{} was used",
                i, formID, slot.assignment.uniqueID);
            slot.isLocked = false;
            slot.remainingMs = 0.0f;
            slot.isActivationLock = false;
            slot.combatLock = false;
            slot.frozen = false;
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
        m_inCombat = false;
        spdlog::info("[SlotLocker] Reset complete");
    }

    // =========================================================================
    // INTERNAL HELPERS
    // =========================================================================

    bool SlotLocker::ShouldLock(
        const SlotAssignment& oldAssign,
        const SlotAssignment& newAssign) const
    {
        // Locking disabled if the duration in force is 0 (the combat lock can
        // be on while fLockDurationMs is 0)
        if (!Core::LockingEnabled(Core::EffectiveLockDuration(
                m_inCombat, m_config.lockDurationMs, m_config.combatLockDurationMs))) {
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

    SlotLocker::BreakReason SlotLocker::ShouldBreakLock(
        const LockedSlot& lock,
        const SlotAssignment& newAssign,
        const Override::OverrideCollection& overrides,
        bool remembranceOnPressedKey) const
    {
        // The active override, if any, whose item this slot's new assignment
        // is. Only the override actually targeting THIS slot may break its
        // lock: match by FormID so a high-priority HP override can't break an
        // unrelated slot's lock (e.g., a BuffsAny slot whose new assignment
        // happens to be flagged Override on a different page layout).
        int overridePriority = -1;
        if (newAssign.type == AssignmentType::Override && overrides.HasActiveOverride()) {
            for (const auto& ovr : overrides.activeOverrides) {
                if (ovr.candidate && Candidate::GetFormID(*ovr.candidate) == newAssign.formID) {
                    overridePriority = std::max(overridePriority, ovr.priority);
                }
            }
        }

        // Under a combat lock: core/CombatLock.h decides. No challenger margin,
        // no timer for a freeze.
        if (lock.combatLock && m_inCombat) {
            Core::CombatLockBreakInput in;
            in.frozen = lock.frozen;
            in.remainingMs = lock.remainingMs;
            in.elapsedMs = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - lock.lockedAt).count();
            in.minLockMs = m_config.minLockDurationMs;
            in.overridesBreakLock = m_config.overridesBreakLock;
            in.immediateBreakPriority = m_config.immediateBreakPriority;
            in.newIsOverrideForThisKey = overridePriority >= 0;
            in.overridePriority = overridePriority;
            in.newIsRemembered = newAssign.IsRemembered();
            in.remembranceOnPressedKey = remembranceOnPressedKey;
            in.newSameItem = SameItem(newAssign, lock.assignment);
            in.lockedIsPinned = lock.assignment.IsPinned();
            in.newIsPinned = newAssign.IsPinned();
            switch (Core::DecideCombatLockBreak(in)) {
            case Core::CombatLockBreak::Hold:        return BreakReason::None;
            case Core::CombatLockBreak::Expired:     return BreakReason::Expired;
            case Core::CombatLockBreak::Override:    return BreakReason::Override;
            case Core::CombatLockBreak::Remembrance: return BreakReason::Remembrance;
            case Core::CombatLockBreak::PinnedEnded: return BreakReason::PinnedEnded;
            case Core::CombatLockBreak::Restyle:     return BreakReason::Restyle;
            }
            return BreakReason::None;
        }

        // Always break if lock timer expired
        if (lock.remainingMs <= 0.0f) {
            return BreakReason::Expired;
        }

        // HIGH-PRIORITY OVERRIDES bypass the minimum lock duration entirely.
        // Safety overrides (health potions, drowning) should never be delayed
        // by anti-flicker timers — the player needs them immediately.
        if (m_config.overridesBreakLock && overridePriority >= m_config.immediateBreakPriority) {
            spdlog::debug("[SlotLocker] Immediate lock break for override on this key (priority={})",
                overridePriority);
            return BreakReason::Override;
        }

        // A Remembrance hold is the player's own undo and may land on a key
        // other than the one pressed (sRemembranceTarget = Job): it takes the
        // key now, like an override, rather than after its lock runs out.
        if (newAssign.IsRemembered() && newAssign.formID != lock.assignment.formID) {
            return BreakReason::Remembrance;
        }

        // Keep the lock - let it expire naturally or be broken by:
        // 1. Timer expiration (checked at top of function)
        // 2. High-priority overrides (checked above, bypasses minLockDurationMs)
        // 3. OnItemUsed() callback when player uses the item
        // Note: We intentionally do NOT break when newAssign is empty - that's
        // the "context window" use case (e.g., surfacing while Waterbreathing is shown)
        // (fMinLockDurationMs gates nothing here any more than it did before:
        // below the immediate priority an override waits for the timer.)
        return BreakReason::None;
    }

    void SlotLocker::DedupePreferLocked(SlotAssignments& result, const SlotAssignments& wanted)
    {
        // `result` is built 1:1 per slot by ApplyLocks, so result[i] corresponds
        // to m_lockedSlots[i] (i == slotIndex). The index-parallel access below
        // relies on that invariant.

        // Who is cleared: core/DedupRules.h (pinned items never lose to a
        // non-pinned copy; a locked incumbent beats a newcomer; an unlocked
        // copy of a locked key's name goes; otherwise the first stays).
        std::vector<Core::DedupRow> rows(std::min(result.size(), MAX_SLOTS));
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& a = result[i];
            const auto& slot = m_lockedSlots[i];
            rows[i].empty = a.IsEmpty();
            if (rows[i].empty) continue;
            rows[i].name = a.name;
            rows[i].pinned = a.IsPinned();
            rows[i].locked = slot.isLocked;
            rows[i].incumbent = slot.isLocked && !slot.shownEmpty &&
                slot.shownFormID == a.formID && slot.shownUniqueID == a.uniqueID;
        }
        const auto verdicts = Core::DedupDecide(rows);
        std::array<bool, MAX_SLOTS> cleared{};
        for (size_t i = 0; i < verdicts.size(); ++i) {
            if (verdicts[i] == Core::DedupClear::Keep) continue;
            auto& assignment = result[i];
            const char* why = "duplicate";
            switch (verdicts[i]) {
            case Core::DedupClear::PinnedDuplicate:    why = "second copy of a pinned item"; break;
            case Core::DedupClear::PinnedElsewhere:    why = "an override/Remembrance item shows it"; break;
            case Core::DedupClear::IncumbentElsewhere: why = "locked slot already showing it"; break;
            case Core::DedupClear::LockedElsewhere:    why = "unlocked, locked elsewhere"; break;
            default: break;
            }
            spdlog::debug("[SlotLocker] Post-lock dedup: clearing '{}' from slot {} ({})",
                assignment.name, assignment.slotIndex, why);
            assignment = SlotAssignment::Empty(assignment.slotIndex, assignment.classification);
            cleared[i] = true;
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
            slotJ.combatLock = false;
            slotJ.frozen = false;

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
                // Locked on what it now shows (0.23.37). Left unlocked, it
                // changed again on the next run, 0.1-0.7 s later (2026-10-11
                // 08:56:21.327 -> .995, 09:20:33.462 -> .570). The incumbent
                // rule above is what makes this safe: the next run re-shows the
                // fill through the lock, and an allocator copy of it elsewhere
                // loses to it instead of evicting it.
                LockNow(slotJ, result[j]);
                // Before 0.23.37 slotJ.assignment kept what the lock / allocator
                // gave slot j and the slot stayed unlocked: recording the fill
                // without a lock made the next run see the allocator's pick as
                // new content, lock it, and -- with j below i -- evict the
                // ORIGINAL lock (/code-review on #146). Locking the fill and
                // preferring the incumbent copy in dedup closes both.
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
