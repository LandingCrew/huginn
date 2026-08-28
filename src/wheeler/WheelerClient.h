#pragma once

#include "WheelerAPI.h"
#include "WheelerConnection.h"
#include "WheelSync.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Huginn::Wheeler
{
    /// Huginn's face to Wheeler: owns the callback trampolines and the state
    /// that only makes sense while a wheel is on screen, and forwards
    /// everything else to the two classes that own it.
    ///
    /// After step 2 of the split this class owns exactly one thing — the
    /// wheel-session state below (m_wheelVisible and friends, under
    /// m_callbackMutex). The API handle belongs to WheelerConnection; the
    /// wheels themselves belong to WheelSync.
    ///
    /// LOCK ORDERING: m_callbackMutex (outer) → WheelSync's m_pageDataMutex
    /// (inner). Now expressed structurally: this class calls WheelSync,
    /// WheelSync never calls back. Do not invert it.
    ///
    /// NOT YET SEPARATED (step 3): the callbacks below still reach outward into
    /// SlotLocker, SlotAllocator, CandidateGenerator, EquipSourceTracker,
    /// EquipEventBus, IntuitionMenu and WheelerSettings. Those reaches are the
    /// remaining coupling, and cutting them means injecting the effects rather
    /// than calling them — the same treatment ExternalEquipLearner got.
    class WheelerClient
    {
    public:
        static WheelerClient& GetSingleton();

        // ============================================================================
        // Connection (forwards to WheelerConnection)
        //
        // Only the questions callers actually ask are forwarded — IsConnected()
        // (6 sites in display/, settings/, Main.cpp) and IsWheelOpen() (4 sites)
        // — so those callers keep talking to one Wheeler object rather than
        // learning which third of the split answers which question.
        //
        // GetAPI(), GetAPIVersion() and SupportsV2Features() are deliberately
        // NOT forwarded: they had no callers, and GetAPI() in particular handed
        // the raw handle to anyone who asked, which is the boundary
        // WheelerConnection exists to draw.
        // ============================================================================

        // Connect to Wheeler and install our callback trampolines.
        // Returns true if connected.
        //
        // Safe to call repeatedly: WheelerConnection::TryConnect() early-returns
        // on an existing handle, and re-registering is a no-op overwrite —
        // Wheeler stores one callback per type, and we hand it the same three
        // trampoline addresses every time. A repeat call does re-emit the
        // "Callbacks registered" debug line; no caller does this today
        // (Main.cpp:447 is the first call; the retry at Main.cpp:127 sits behind
        // a !IsConnected() guard).
        bool TryConnect();

        // Check if connected
        [[nodiscard]] bool IsConnected() const noexcept
        {
            return WheelerConnection::GetSingleton().IsConnected();
        }

        // Check if the Wheeler UI is currently open (any wheel visible)
        [[nodiscard]] bool IsWheelOpen() const noexcept
        {
            return WheelerConnection::GetSingleton().IsWheelOpen();
        }

        /// True while the player is rearranging wheels in Wheeler's editor.
        /// Gates the display push — see WheelerBackend::Push.
        [[nodiscard]] bool IsInEditMode() const noexcept
        {
            return WheelerConnection::GetSingleton().IsInEditMode();
        }

        // Log API info
        void LogAPIInfo();

        // Register callbacks with Wheeler API
        void RegisterCallbacks();

        // Unregister callbacks. No caller today; kept as the counterpart to
        // RegisterCallbacks so a shutdown path has something to call — dropping
        // a teardown hook is not the same trade as dropping a dead getter.
        void UnregisterCallbacks();

        // ============================================================================
        // Wheel management (forwards to WheelSync)
        //
        // Trimmed to what is actually called. Removed rather than moved into
        // WheelSync, since moving dead code just relocates it: both
        // UpdateRecommendations overloads, GetCurrentWheelIndex,
        // ClearActivationEmptied, IsWheelVisible, GetItemTypeName, and the
        // GetRecommendationWheelIndex / GetPrimaryWheelIndex /
        // GetAlternateWheelIndex legacy aliases. All had zero callers.
        //
        // HasRecommendationWheels/HasRecommendationWheel were two names for one
        // question; the alias survives because both call sites use it.
        // ============================================================================

        // Create recommendation wheels - one per page from SlotSettings.
        // Call after SlotSettings is loaded and Wheeler is initialized.
        // Returns true if at least one wheel was created.
        bool CreateRecommendationWheels() { return WheelSync::GetSingleton().CreateWheels(); }

        // Tear down all recommendation wheels.
        void DestroyRecommendationWheels() { WheelSync::GetSingleton().DestroyWheels(); }

        /// Drop the remembered wheel position so the next create honours
        /// sWheelPosition. See WheelSync::ForgetRememberedPosition.
        void ForgetWheelPositionMemory() { WheelSync::GetSingleton().ForgetRememberedPosition(); }

        // Update wheel for a SPECIFIC page (for cross-page updates)
        void UpdateRecommendationsForPage(size_t pageIndex,
                                          const std::vector<RE::FormID>& spellFormIDs,
                                          const std::vector<bool>& isWildcard,
                                          const std::vector<uint16_t>& uniqueIDs = {},
                                          const std::vector<std::string>& subtexts = {})
        {
            WheelSync::GetSingleton().UpdatePage(pageIndex, spellFormIDs, isWildcard, uniqueIDs, subtexts);
        }

        // Check if at least one recommendation wheel exists.
        [[nodiscard]] bool HasRecommendationWheel() const
        {
            return WheelSync::GetSingleton().HasAnyWheel();
        }

        // #76: rebuild if every page has been invalidated. Must be called BEFORE
        // HasRecommendationWheel() gates the push — in that state it returns
        // false, so anything downstream of it is unreachable.
        bool RecoverInvalidatedWheels() { return WheelSync::GetSingleton().RecoverInvalidatedWheels(); }

        // Re-derive wheel indices from client labels after Wheeler may have
        // reindexed. See WheelSync::ReResolveWheelIndices.
        bool ReResolveWheelIndices() { return WheelSync::GetSingleton().ReResolveWheelIndices(); }

        // Get wheel index for a page (returns -1 if invalid)
        [[nodiscard]] int32_t GetWheelIndexForPage(size_t pageIndex) const
        {
            return WheelSync::GetSingleton().WheelIndexForPage(pageIndex);
        }

        // Switch Wheeler's active wheel to match a given page index.
        bool SetActivePage(size_t pageIndex) { return WheelSync::GetSingleton().SetActivePage(pageIndex); }

        // Attempt to auto-focus Wheeler to a Huginn wheel when an urgent
        // override fires. Returns true if auto-focus was triggered.
        bool TryUrgentAutoFocus(int overridePriority)
        {
            return WheelSync::GetSingleton().TryUrgentAutoFocus(overridePriority);
        }

        // Verify cached FormIDs match Wheeler's actual state. Debug builds only.
        void ValidateWheelState() const { WheelSync::GetSingleton().ValidateWheelState(); }

        // Get the Huginn page index of the currently active Wheeler wheel.
        // Returns -1 if Wheeler is not open, not connected, or the active wheel
        // is not one of our managed wheels.
        [[nodiscard]] int GetActiveManagedPage() const
        {
            return WheelSync::GetSingleton().ActiveManagedPage();
        }

        // Check if a slot is marked activation-emptied (Empty policy)
        [[nodiscard]] bool IsSlotActivationEmptied(size_t pageIndex, size_t slotIndex) const
        {
            return WheelSync::GetSingleton().IsSlotActivationEmptied(pageIndex, slotIndex);
        }

        // ============================================================================
        // Wheel session state (owned here)
        // ============================================================================

        // Process a deferred wheel-close event.  Called from the update loop
        // where IsWheelOpen() is accurate (unlike inside the callback).
        // Returns true if the wheel was truly closed and IntuitionMenu was shown.
        bool CheckPendingWheelClose();

        // ============================================================================
        // Environment (step 3: injected effects)
        //
        // When the player touches the wheel, most of Huginn needs to hear about
        // it — locks break, the page syncs, a cooldown starts, the learner gets
        // an equip event, the widget hides. Those used to be direct calls, which
        // made wheeler/ depend on slot/, learning/, candidate/ and ui/: a
        // low-level integration reaching up into pipeline policy, the same shape
        // #10a and #10b removed elsewhere.
        //
        // They are now supplied by the composition root (Main.cpp). This class
        // decides WHAT happened and WHEN, and knows nothing about who cares.
        //
        // THREAD SAFETY — two call contexts, and three callbacks see both:
        //
        //   Wheeler's callback thread, WITH m_callbackMutex held:
        //     lockSlotForActivation, onItemUsed, markHuginnEquip, startCooldown,
        //     setWidgetVisible and setCurrentPage (via HandleWheelOpened).
        //     Exactly where the direct calls used to sit, so no lock order
        //     changed. None of these may call back into WheelerClient, and none
        //     may call the Wheeler API — a synchronous callback would re-enter
        //     the mutex.
        //
        //   The update thread, holding NO lock:
        //     setCurrentPage, setWidgetVisible and markPageDirty, all via
        //     CheckPendingWheelClose.
        //
        //   No lock, callback thread:
        //     publishWheelerEquip — its subscribers take StateManager and bus
        //     locks of their own.
        //
        // So setCurrentPage and setWidgetVisible can genuinely run concurrently
        // with their callback-thread counterparts, and a provider must be safe
        // under concurrent invocation rather than assuming serialization.
        // Today's both are: SlotAllocator's page state is atomic, and
        // IntuitionMenu::SetVisible defers to the UI thread via AddUITask.
        // ============================================================================
        struct Environment
        {
            /// Sticky policy: hold this slot so the activated item stays visible.
            std::function<void(size_t slotIndex)> lockSlotForActivation;

            /// Empty/Backfill: the player used this item; break its slot lock.
            std::function<void(RE::FormID)> onItemUsed;

            /// Attribute the coming equip to Huginn, not to the player's own UI.
            std::function<void(RE::FormID)> markHuginnEquip;

            /// Suppress this item from the next few candidate passes. The caller
            /// owns the form-type classification — it is the side that knows
            /// what a SourceType is.
            std::function<void(RE::FormID)> startCooldown;

            /// Announce a Wheeler-mediated equip to the learning subscribers.
            /// Called WITHOUT m_callbackMutex held.
            std::function<void(RE::FormID)> publishWheelerEquip;

            /// Show or hide the Intuition widget as the wheel opens and closes.
            std::function<void(bool visible)> setWidgetVisible;

            /// Follow Wheeler's active wheel with Huginn's current page.
            std::function<void(size_t pageIndex)> setCurrentPage;

            /// Force the pipeline to run so the widget repaints on wheel close.
            std::function<void()> markPageDirty;

            /// Every effect wired. Lives on the struct, immediately below the
            /// fields, so the list cannot drift: SetEnvironment validates with
            /// it and EnvironmentReady() gates with it, and adding a ninth
            /// callback without extending this fails to compile nothing but is
            /// impossible to overlook — the list is right here. A half-wired
            /// Environment that slipped past both would throw
            /// std::bad_function_call from a Wheeler callback, which in an SKSE
            /// plugin is a crash to desktop, not a log line.
            [[nodiscard]] bool Complete() const noexcept
            {
                return lockSlotForActivation && onItemUsed && markHuginnEquip &&
                       startCooldown && publishWheelerEquip && setWidgetVisible &&
                       setCurrentPage && markPageDirty;
            }
        };

        /// Install the effects. Validated on assignment: a partially wired
        /// Environment is rejected whole rather than half-applied, because a
        /// missing callback would show up as a wheel that silently stops
        /// affecting anything rather than as an error.
        void SetEnvironment(Environment env);

    private:
        WheelerClient() = default;

        /// Written once by SetEnvironment on the main thread at kDataLoaded,
        /// before TryConnect installs the callbacks; read afterwards from both
        /// Wheeler's callback thread and the update thread, unsynchronized.
        /// Safe only because of that ordering — writing it again while the game
        /// is running would race two reader threads, so don't. (Same invariant
        /// as ExternalEquipLearner's, with one more reader.)
        Environment m_env;

        /// True once every callback is wired. Checked as the FIRST statement of
        /// each Wheeler callback: a wiring fault must not get halfway through an
        /// activation, and must be loud rather than silent.
        [[nodiscard]] bool EnvironmentReady() const;

        // Callback state
        std::atomic<bool> m_wheelVisible{false};
        std::atomic<bool> m_pendingWheelClose{false};
        std::atomic<int32_t> m_pendingCloseWheelIndex{-1};  // Wheel index from the last close callback
        std::atomic<bool> m_itemActivatedWhileOpen{false};  // Track if player activated an item while wheel was open

        /// One-shot latch for the auto-focus stranding warning. Auto-focus
        /// redirects EVERY fresh open that lands on a non-Huginn wheel, so
        /// warning per open would spam a notification onto the screen the
        /// player is trying to use. The condition is a property of the wheel
        /// LAYOUT, not of any one open, so once is the right number — and the
        /// existing per-open `Auto-focus requested` info line still records
        /// every occurrence for the log. Re-armed on edit-mode exit, which is
        /// the only in-session point where the layout can change.
        std::atomic<bool> m_autoFocusStrandWarned{false};

        mutable std::mutex m_callbackMutex;     // Protects callback state (mutable for const methods)

        // Static callback trampolines (call into singleton instance)
        static void OnItemActivated(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex, uint32_t formID, bool isPrimary);
        static void OnWheelStateChanged(int32_t wheelIndex, bool isOpen);
        static void OnEditModeChanged(bool entered, const WheelerAPI::WheelChange* changes, size_t changeCount);

        // Wheel state change helpers (extracted from OnWheelStateChanged).
        // HandleWheelOpened returns a wheel index to auto-focus (-1 if none).
        // The caller must apply SetActiveWheelIndex OUTSIDE m_callbackMutex to
        // avoid re-entrant callback deadlock.
        int32_t HandleWheelOpened(int32_t wheelIndex);
        void HandleWheelClosed(int32_t wheelIndex);
    };
}
