#include "WheelerClient.h"
#include "WheelSync.h"
#include "WheelerConnection.h"
#include "WheelerSettings.h"

#include <optional>
#include <spdlog/spdlog.h>
#include <utility>

// Deliberately includes nothing from slot/, learning/, candidate/ or ui/.
// Everything this file used to call directly in those directories now arrives
// as an injected Environment, wired at the composition root (Main.cpp). See the
// Environment comment in WheelerClient.h for why.

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
    // Environment
    // ============================================================================

    void WheelerClient::SetEnvironment(Environment env)
    {
        // Validate before storing. A half-wired Environment is worse than none:
        // the wheel would keep working visually while quietly failing to break
        // locks or train the learner, which reads as a subtle scoring bug rather
        // than a wiring bug.
        if (!env.Complete()) {
            spdlog::error("[WheelerClient] SetEnvironment rejected — not every effect was wired; "
                          "keeping the previous environment");
            return;
        }

        m_env = std::move(env);
        spdlog::info("[WheelerClient] Environment wired");
    }

    bool WheelerClient::EnvironmentReady() const
    {
        // Complete(), not a single sentinel field: the gate and SetEnvironment
        // must agree, and a hand-maintained field list in two places drifts.
        if (m_env.Complete()) {
            return true;
        }
        // Rate-limited: this fires from a Wheeler callback, so an unwired build
        // would otherwise emit one line per wheel interaction. Atomic because
        // the three gate sites span Wheeler's callback thread and the update
        // thread — a plain bool here is a data race for one duplicated line.
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            spdlog::error("[WheelerClient] Wheeler callback fired before SetEnvironment — "
                          "wheel interactions will do nothing. Check Main.cpp wiring.");
        }
        return false;
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
        if (!client.EnvironmentReady()) {
            return;
        }
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
                client.m_env.lockSlotForActivation(static_cast<size_t>(entryIndex));
                spdlog::info("[WheelerClient] Sticky policy: slot {} locked for activation", entryIndex);
            } else if (policy == PostActivationPolicy::Empty) {
                // Empty: Mark as activation-emptied + clear cached state. The
                // bounds check lives inside WheelSync under its own lock —
                // DestroyWheels can clear the page list between an unlocked
                // check and the index. The Wheeler API calls (ClearEntry,
                // SetEntrySubtext) are deferred to outside the mutex.
                client.m_env.onItemUsed(static_cast<RE::FormID>(formID));
                deferredEmpty = wheels.MarkActivationEmptied(
                    static_cast<size_t>(pageIndex), static_cast<size_t>(entryIndex));
                spdlog::info("[WheelerClient] Empty policy: slot {} marked activation-emptied (API calls deferred)", entryIndex);
            } else {
                // Backfill (default): Break lock so slot repopulates on next update cycle
                client.m_env.onItemUsed(static_cast<RE::FormID>(formID));
            }

            // Mark as Huginn-mediated equip (for external equip detection)
            client.m_env.markHuginnEquip(static_cast<RE::FormID>(formID));

            // Start cooldown so the consumed item is filtered out on the next update cycle (~100ms)
            // Without this, the item stays recommended until RefreshCounts detects count=0 (up to 500ms)
            // Skip cooldown for Sticky policy — the item should remain visible.
            // The form-type classification moved to the provider: deciding that
            // an AlchemyItem is a Potion is candidate/'s vocabulary, not ours.
            if (policy != PostActivationPolicy::Sticky) {
                client.m_env.startCooldown(static_cast<RE::FormID>(formID));
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
                wheels.SetEntrySubtext(api, deferredEmpty->wheelIndex, deferredEmpty->entryIndex, "Equipped");
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
        // No pageIndex guard: the locked block above returns early when the
        // wheel isn't ours, so reaching here means pageIndex >= 0.
        client.m_env.publishWheelerEquip(static_cast<RE::FormID>(formID));
    }

    void WheelerClient::OnWheelStateChanged(int32_t wheelIndex, bool isOpen)
    {
        auto& client = GetSingleton();
        if (!client.EnvironmentReady()) {
            return;
        }
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
        spdlog::debug("[WheelerClient] EditModeChanged: entered={}, changeCount={}", entered, changeCount);

        if (entered) {
            return;  // nothing has moved yet; the player is only opening the editor
        }

        // NOT re-armed here any more. Edit-mode exit is the only in-session
        // point where wheel ORDER can change, so re-arming here was the honest
        // way to catch a player who newly strands a wheel after being warned —
        // but "auto-focus skips wheels ahead of ours" is one fact about the
        // feature, not a per-layout event, and repeating it every time the
        // player leaves the editor is noise. Observed 2026-08-29: four warns in
        // nine minutes across three edit-mode exits, all saying the same thing.
        // One per run; the per-open `Auto-focus requested` info line still
        // records every occurrence.

        // Edit mode is where the player reorders, adds and deletes wheels, and
        // every one of those shifts the indices we stored at creation. Nothing
        // else re-queries the layout, so without this our subtext writes keep
        // going to the OLD index — which by then may be someone else's wheel.
        // They are accepted there, because IsManagedWheel() cannot tell whose
        // wheel it is; that is why this has to be a trigger and not a check.
        //
        // The payload is deliberately ignored. EditModeCallback is declared to
        // carry a WheelChange list, but Wheeler::exitEditMode() always passes
        // (nullptr, 0) — upstream has this as an open TODO. Every observed
        // reorder reported changeCount=0 while the indices had in fact moved,
        // so gating on the payload would gate on a constant. Re-resolving
        // unconditionally is the only strategy that works against Wheeler as it
        // actually behaves; it is one cheap read per page and it logs only when
        // something moved.
        //
        // Takes no m_callbackMutex: this touches no wheel-session state and goes
        // straight to WheelSync, which preserves the documented one-way
        // callbackMutex -> pageDataMutex ordering.
        GetSingleton().ReResolveWheelIndices();
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
            // (the provider defers to the UI thread via AddUITask — safe from
            // the callback thread)
            m_env.setWidgetVisible(false);
            spdlog::debug("[WheelerClient] Notified IntuitionMenu: SetVisible(false)");
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

            // Warn only when a wheel is actually SKIPPED — that is, when it sits
            // BEFORE ours (wheelIndex < autoFocusTarget). Wheeler navigates left
            // to right, so a wheel ahead of our block is the one the redirect
            // jumps over: the player scrolls right from our first wheel and never
            // passes it, and it reads as Huginn having eaten it.
            //
            // A wheel BEHIND ours is a different situation and must not warn.
            // The redirect still moves the player off it, but it stays reachable
            // by scrolling right in the ordinary way — nothing is skipped.
            // Observed 2026-08-29: Huginn at 0/1/2, the player opened on wheel 3
            // and got the stranding warning about a wheel that was never
            // stranded. This gate is what the earlier version deliberately left
            // out, on the theory that being redirected at all was the same
            // surprise; it is not, and the ungated message was wrong in the more
            // common direction.
            //
            // Once per RUN. See m_autoFocusStrandWarned for why not per open,
            // and OnEditModeChanged for why it is no longer re-armed per layout.
            const bool skipsWheel = wheelIndex < autoFocusTarget;
            if (skipsWheel && !m_autoFocusStrandWarned.exchange(true)) {
                spdlog::warn("[WheelerClient] Auto-focus is skipping wheel {}, which sits before Huginn's "
                             "first wheel ({}). Wheeler opens on it and bAutoFocusOnOpen=true redirects "
                             "past it, so it is skipped on every open and can only be reached by scrolling "
                             "back. Set bAutoFocusOnOpen=false under [Wheeler] in Huginn.ini to open where "
                             "Wheeler left off.",
                    wheelIndex, autoFocusTarget);

                // Callback thread: RE::DebugNotification queues onto the UI
                // message list and is not safe to call from here directly.
                // Same deferral the widget uses two blocks up.
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddUITask([]() {
                        // Not "your own wheel": the skipped wheel is whatever sits
                        // before ours, which is not always one the player made.
                        RE::DebugNotification("Huginn: auto-focus is skipping a wheel before ours on open "
                                              "(set bAutoFocusOnOpen=false in Huginn.ini to stop this)");
                    });
                }
            }
        }

        if (info.pageIndex < 0) {
            spdlog::debug("[WheelerClient] Wheel {} is not an Huginn wheel", wheelIndex);
            return autoFocusTarget;
        }

        spdlog::info("[WheelerClient] Page {} '{}' wheel opened", info.pageIndex, info.pageName);

        // Observer notification: sync the current page when Wheeler scrolls to our wheel.
        // The provider sets m_pageChanged=true, which makes the pipeline run on the next
        // tick and update IntuitionMenu with the correct page's slot assignments.
        m_env.setCurrentPage(static_cast<size_t>(info.pageIndex));

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

        // Gate here too: this one runs on the update thread, not a callback, but
        // it still drives setCurrentPage / setWidgetVisible / markPageDirty.
        if (!EnvironmentReady()) {
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
                m_env.setCurrentPage(static_cast<size_t>(pageIndex));
                spdlog::debug("[WheelerClient] CheckPendingWheelClose: synced page to {} (wheel {})",
                    pageIndex, closedWheelIndex);
            }
        }

        // Clear activation-emptied flags (allows Empty policy slots to repopulate)
        WheelSync::GetSingleton().ClearAllActivationEmptied();

        // Show IntuitionMenu now that Wheeler is fully closed.
        // markPageDirty forces the pipeline to run on this tick, pushing
        // the current page's slot assignments to IntuitionMenu.
        m_env.setWidgetVisible(true);
        spdlog::debug("[WheelerClient] CheckPendingWheelClose: SetVisible(true)");
        m_env.markPageDirty();
        spdlog::debug("[WheelerClient] CheckPendingWheelClose: MarkPageDirty for IntuitionMenu refresh");

        return true;
    }
}
