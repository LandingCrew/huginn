#include "WheelerClient.h"
#include "WheelSync.h"
#include "WheelerConnection.h"
#include "WheelerSettings.h"

#include <optional>
#include <spdlog/spdlog.h>

#include "../learning/EquipSourceTracker.h"
#include "../learning/EquipEventBus.h"
#include "../slot/SlotAllocator.h"
#include "../slot/SlotLocker.h"
#include "../candidate/CandidateGenerator.h"
#include "../candidate/CandidateTypes.h"
#include "../ui/IntuitionMenu.h"

namespace Huginn::Wheeler
{
    // Log tag "[WheelerClient]" is used by all three classes of the split, so a
    // Debug session before and after the refactor diffs clean. See the note at
    // the top of WheelSync.cpp.

    WheelerClient& WheelerClient::GetSingleton()
    {
        static WheelerClient instance;
        return instance;
    }

    bool WheelerClient::TryConnect()
    {
        // Acquiring and version-gating the handle is WheelerConnection's job;
        // deciding what runs when Wheeler talks back is ours, so the callback
        // registration stays here.
        if (!WheelerConnection::GetSingleton().TryConnect()) {
            return false;
        }

        RegisterCallbacks();
        return true;
    }

    // ============================================================================
    // Callback Registration
    // ============================================================================

    void WheelerClient::RegisterCallbacks()
    {
        WheelerConnection::GetSingleton().RegisterCallbacks(
            &WheelerClient::OnItemActivated,
            &WheelerClient::OnWheelStateChanged,
            &WheelerClient::OnEditModeChanged);
    }

    void WheelerClient::UnregisterCallbacks()
    {
        WheelerConnection::GetSingleton().UnregisterCallbacks();
    }

    void WheelerClient::LogAPIInfo()
    {
        WheelerConnection::GetSingleton().LogAPIInfo();
    }

    // ============================================================================
    // THREAD SAFETY: Wheeler callbacks may fire synchronously from Wheeler API
    // calls (e.g., SetActiveWheelIndex triggers OnWheelStateChanged). To avoid
    // deadlock: NEVER call Wheeler API functions while holding m_callbackMutex.
    // Defer API calls outside the lock (see OnWheelStateChanged pattern).
    //
    // WheelSync takes its own lock on every public method, so calling it from
    // here is fine — that is the documented m_callbackMutex → m_pageDataMutex
    // order. It must never call back into this class.
    // ============================================================================

    // ============================================================================
    // Static Callback Trampolines
    // ============================================================================

    void WheelerClient::OnItemActivated(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex, uint32_t formID, bool isPrimary)
    {
        spdlog::info("[WheelerClient] Callback from API - wheel selection made - wheel={}, entry={}, item={}, formID={:08X}, primary={}",
            wheelIndex, entryIndex, itemIndex, formID, isPrimary);

        auto& client = GetSingleton();
        auto& wheels = WheelSync::GetSingleton();

        // Deferred Wheeler API calls — populated inside mutex, executed outside.
        // THREAD SAFETY: Wheeler API calls can trigger synchronous callbacks,
        // so they must NEVER run while holding m_callbackMutex.
        std::optional<WheelSync::DeferredEntryClear> deferredEmpty;

        int pageIndex = -1;
        auto policy = PostActivationPolicy::Backfill;

        {
            std::lock_guard<std::mutex> lock(client.m_callbackMutex);

            // Check if this is one of our managed wheels
            pageIndex = wheels.FindPageForWheel(wheelIndex);
            if (pageIndex < 0) {
                spdlog::debug("[WheelerClient] ItemActivated on non-Huginn wheel {}, ignoring", wheelIndex);
                return;
            }

            spdlog::info("[WheelerClient] Item activated on page {} wheel: entry={}, item={}, formID={:08X}, primary={}",
                pageIndex, entryIndex, itemIndex, formID, isPrimary);

            // Mark that an item was activated while wheel was open (prevents skip penalty)
            client.m_itemActivatedWhileOpen = true;
            spdlog::debug("[WheelerClient] Set m_itemActivatedWhileOpen=true for wheel {} (page {})", wheelIndex, pageIndex);

            // Post-activation policy determines how slot behaves after use
            policy = WheelerSettings::GetSingleton().GetPostActivationPolicy();

            if (policy == PostActivationPolicy::Sticky) {
                // Sticky: Keep the activated item visible — apply long lock, skip cooldown
                Slot::SlotLocker::GetSingleton().LockSlotForActivation(static_cast<size_t>(entryIndex));
                spdlog::info("[WheelerClient] Sticky policy: slot {} locked for activation", entryIndex);
            } else if (policy == PostActivationPolicy::Empty) {
                // Empty: Mark as activation-emptied + clear cached state. The
                // bounds check lives inside WheelSync under its own lock —
                // DestroyWheels can clear the page list between an unlocked
                // check and the index. The Wheeler API calls (ClearEntry,
                // SetEntrySubtext) are deferred to outside the mutex.
                Slot::SlotLocker::GetSingleton().OnItemUsed(static_cast<RE::FormID>(formID));
                deferredEmpty = wheels.MarkActivationEmptied(
                    static_cast<size_t>(pageIndex), static_cast<size_t>(entryIndex));
                spdlog::info("[WheelerClient] Empty policy: slot {} marked activation-emptied (API calls deferred)", entryIndex);
            } else {
                // Backfill (default): Break lock so slot repopulates on next update cycle
                Slot::SlotLocker::GetSingleton().OnItemUsed(static_cast<RE::FormID>(formID));
            }

            // Mark as Huginn-mediated equip (for external equip detection)
            Learning::EquipSourceTracker::GetSingleton().MarkHuginnEquip(
                static_cast<RE::FormID>(formID));

            // Start cooldown so the consumed item is filtered out on the next update cycle (~100ms)
            // Without this, the item stays recommended until RefreshCounts detects count=0 (up to 500ms)
            // Skip cooldown for Sticky policy — the item should remain visible
            if (policy != PostActivationPolicy::Sticky) {
                auto& candidateGen = Candidate::CandidateGenerator::GetSingleton();
                if (candidateGen.IsInitialized()) {
                    Candidate::SourceType sourceType = Candidate::SourceType::Spell;
                    if (auto* form = RE::TESForm::LookupByID(formID)) {
                        if (form->Is(RE::FormType::AlchemyItem)) sourceType = Candidate::SourceType::Potion;
                        else if (form->Is(RE::FormType::Weapon))  sourceType = Candidate::SourceType::Weapon;
                        else if (form->Is(RE::FormType::Scroll)) sourceType = Candidate::SourceType::Scroll;
                    }
                    candidateGen.StartCooldown(formID, sourceType);
                    spdlog::info("[WheelerClient] Started cooldown for {:08X} (type {})", formID, static_cast<int>(sourceType));
                }
            } else {
                spdlog::debug("[WheelerClient] Skipping cooldown for Sticky policy");
            }
        }  // m_callbackMutex released here

        // Execute deferred Wheeler API calls OUTSIDE the mutex (safe from deadlock).
        // Validate wheel is still managed — another thread (e.g. save/load) may have
        // destroyed wheels between mutex release and here.
        auto* api = WheelerConnection::GetSingleton().Api();
        if (deferredEmpty && api) {
            if (api->IsManagedWheel(deferredEmpty->wheelIndex)) {
                api->ClearEntry(deferredEmpty->wheelIndex, deferredEmpty->entryIndex);
                wheels.SetEntrySubtext(deferredEmpty->wheelIndex, deferredEmpty->entryIndex, "Equipped");
                spdlog::debug("[WheelerClient] Empty policy: deferred ClearEntry + SetEntrySubtext executed for wheel {} entry {}",
                    deferredEmpty->wheelIndex, deferredEmpty->entryIndex);
            } else {
                spdlog::warn("[WheelerClient] Deferred empty action skipped — wheel {} no longer managed",
                    deferredEmpty->wheelIndex);
            }
        }

        // Publish to EquipEventBus OUTSIDE the mutex (subscribers handle FQL + UsageMemory).
        // Lock ordering: bus acquires StateManager shared locks in BuildEvent, then bus m_mutex,
        // then subscriber internal locks — all outside m_callbackMutex.
        if (pageIndex >= 0) {
            Learning::EquipEventBus::GetSingleton().Publish(
                formID, Learning::EquipSource::Wheeler, 1.0f, /*wasRecommended=*/true);
        }
    }

    void WheelerClient::OnWheelStateChanged(int32_t wheelIndex, bool isOpen)
    {
        auto& client = GetSingleton();
        int32_t autoFocusTarget = -1;

        {
            std::lock_guard<std::mutex> lock(client.m_callbackMutex);

            spdlog::info("[WheelerClient] WheelStateChanged: wheel={}, isOpen={}", wheelIndex, isOpen);

            if (isOpen) {
                autoFocusTarget = client.HandleWheelOpened(wheelIndex);
            } else {
                client.HandleWheelClosed(wheelIndex);
            }
        }

        // Apply auto-focus OUTSIDE the mutex to avoid re-entrant callback deadlock.
        // SetActiveWheelIndex may fire OnWheelStateChanged synchronously.
        if (auto* api = WheelerConnection::GetSingleton().Api(); autoFocusTarget >= 0 && api) {
            api->SetActiveWheelIndex(autoFocusTarget);
        }
    }

    void WheelerClient::OnEditModeChanged(bool entered, const WheelerAPI::WheelChange* changes, size_t changeCount)
    {
        // Log but don't act - Huginn doesn't need to respond to edit mode
        spdlog::debug("[WheelerClient] EditModeChanged: entered={}, changeCount={}", entered, changeCount);
    }

    // ============================================================================
    // Wheel State Change Helpers (Extracted from OnWheelStateChanged)
    // ============================================================================

    int32_t WheelerClient::HandleWheelOpened(int32_t wheelIndex)
    {
        // Detect if this is a fresh open vs. scrolling between wheels.
        // Wheeler fires OnWheelStateChanged(newWheel, true) for both cases.
        // m_wheelVisible is true if we already had a wheel open (= scrolling).
        bool isFreshOpen = !m_wheelVisible;

        m_wheelVisible = true;
        if (isFreshOpen) {
            m_itemActivatedWhileOpen = false;  // Reset activation tracking
            spdlog::debug("[WheelerClient] Fresh open: reset m_itemActivatedWhileOpen=false");

            // Observer notification: hide IntuitionMenu when Wheeler opens
            // (SetVisible defers to UI thread via AddUITask — safe from callback thread)
            if (auto* intuition = UI::IntuitionMenu::GetSingleton()) {
                intuition->SetVisible(false);
                spdlog::debug("[WheelerClient] Notified IntuitionMenu: SetVisible(false)");
            }
        } else {
            spdlog::debug("[WheelerClient] Scroll to wheel {} (not a fresh open)", wheelIndex);
        }

        // ONE locked query for page index, page name, and the auto-focus target.
        // This used to be a hand-inlined loop, because the function held
        // m_pageDataMutex itself and calling FindPageForWheel would have
        // recursed into it. Now the mutex belongs to WheelSync and is never held
        // here — but three separate calls would be worse than the inlining was,
        // since a teardown could land between them and make the answers
        // disagree. One call, one lock, one consistent snapshot.
        //
        // Called with m_callbackMutex held; WheelSync takes m_pageDataMutex
        // inside. Lock ordering: callbackMutex (outer) → pageDataMutex (inner) ✓
        const auto info = WheelSync::GetSingleton().DescribeOpenedWheel(wheelIndex);

        // Auto-focus: only on FRESH open (not when scrolling between wheels).
        // If the player opened Wheeler on a non-Huginn wheel, return target for deferred focus.
        // The caller applies SetActiveWheelIndex OUTSIDE the mutex to avoid re-entrant deadlock.
        //
        // BEHAVIOUR CHANGE (deliberate): this used to read m_pageWheels[0]
        // directly and give up if page 0 held a wheelIndex=-1 placeholder, so a
        // zero-slot or failed page 0 disabled auto-focus entirely even when
        // later pages had real wheels. firstValidWheel is the first page that
        // actually has one — matching what TryUrgentAutoFocus and HasAnyWheel
        // already do, both of which carry comments explaining why keying on
        // page 0 is wrong. Only observable when page 0 is a placeholder.
        int32_t autoFocusTarget = -1;
        auto& wheelerSettings = WheelerSettings::GetSingleton();
        if (isFreshOpen && wheelerSettings.GetAutoFocusOnOpen() && info.pageIndex < 0 &&
            info.firstValidWheel >= 0) {
            autoFocusTarget = info.firstValidWheel;
            spdlog::info("[WheelerClient] Auto-focus requested: Huginn wheel {} (opened on non-Huginn wheel {})",
                autoFocusTarget, wheelIndex);
        }

        if (info.pageIndex < 0) {
            spdlog::debug("[WheelerClient] Wheel {} is not an Huginn wheel", wheelIndex);
            return autoFocusTarget;
        }

        spdlog::info("[WheelerClient] Page {} '{}' wheel opened", info.pageIndex, info.pageName);

        // Observer notification: sync SlotAllocator page when Wheeler scrolls to our wheel.
        // SetCurrentPage sets m_pageChanged=true, which makes the pipeline run on the next
        // tick and update IntuitionMenu with the correct page's slot assignments.
        Slot::SlotAllocator::GetSingleton().SetCurrentPage(static_cast<size_t>(info.pageIndex));

        return -1;  // Already on our wheel, no auto-focus needed
    }

    void WheelerClient::HandleWheelClosed(int32_t wheelIndex)
    {
        // Don't set m_wheelVisible = false here — defer to CheckPendingWheelClose().
        // Keeping it true lets HandleWheelOpened correctly detect scroll-vs-fresh-open
        // when Wheeler fires close+open in rapid succession during page scrolling.
        //
        // Don't call IsWheelOpen() here — Wheeler fires this callback BEFORE updating
        // its own state, so IsWheelOpen() still returns true at this point.
        // CheckPendingWheelClose() will query it on the update thread where it's accurate.

        m_pendingWheelClose = true;
        m_pendingCloseWheelIndex = wheelIndex;

        int pageIndex = WheelSync::GetSingleton().FindPageForWheel(wheelIndex);
        spdlog::info("[WheelerClient] Wheel {} close callback (page={}, activated={}) — deferred to update tick",
            wheelIndex, pageIndex, m_itemActivatedWhileOpen.load());
    }

    bool WheelerClient::CheckPendingWheelClose()
    {
        if (!m_pendingWheelClose.load(std::memory_order_relaxed)) {
            return false;
        }

        // Now running on the update thread — IsWheelOpen() is accurate here.
        bool stillOpen = IsWheelOpen();
        if (stillOpen) {
            // Wheeler scrolled to another wheel (close A → open B).
            // The pending flag was set by close-A, but open-B already fired,
            // so the wheel is still visible. Just clear the flag.
            m_pendingWheelClose = false;
            spdlog::debug("[WheelerClient] CheckPendingWheelClose: wheel still open (scroll), clearing flag");
            return false;
        }

        // Wheel is truly closed — consume the flag and process the close.
        m_pendingWheelClose = false;
        m_wheelVisible = false;

        // Sync SlotAllocator page to match the wheel that was last closed.
        // This handles the case where the m_pageChanged flag from SetCurrentPage()
        // was consumed while the wheel was still open (pipeline skips UI updates
        // when wheel is open), and also the case where Wheeler doesn't fire
        // separate close+open callbacks during scrolling.
        int32_t closedWheelIndex = m_pendingCloseWheelIndex.exchange(-1);
        if (closedWheelIndex >= 0) {
            int pageIndex = WheelSync::GetSingleton().FindPageForWheel(closedWheelIndex);
            if (pageIndex >= 0) {
                Slot::SlotAllocator::GetSingleton().SetCurrentPage(static_cast<size_t>(pageIndex));
                spdlog::debug("[WheelerClient] CheckPendingWheelClose: synced page to {} (wheel {})",
                    pageIndex, closedWheelIndex);
            }
        }

        // Clear activation-emptied flags (allows Empty policy slots to repopulate)
        WheelSync::GetSingleton().ClearAllActivationEmptied();

        // Show IntuitionMenu now that Wheeler is fully closed.
        // MarkPageDirty forces the pipeline to run on this tick, pushing
        // the current page's slot assignments to IntuitionMenu.
        if (auto* intuition = UI::IntuitionMenu::GetSingleton()) {
            intuition->SetVisible(true);
            spdlog::debug("[WheelerClient] CheckPendingWheelClose: SetVisible(true)");
        }
        Slot::SlotAllocator::GetSingleton().MarkPageDirty();
        spdlog::debug("[WheelerClient] CheckPendingWheelClose: MarkPageDirty for IntuitionMenu refresh");

        return true;
    }
}
