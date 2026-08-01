#pragma once

#include "WheelerAPI.h"
#include "WheelerConnection.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Huginn::Wheeler
{
    class WheelerClient
    {
    public:
        static WheelerClient& GetSingleton();

        // ============================================================================
        // Connection (forwarding facade)
        //
        // The API handle lives in WheelerConnection. Only the questions callers
        // actually ask are forwarded — IsConnected() (6 sites in display/,
        // settings/, Main.cpp) and IsWheelOpen() (4 sites) — so those callers
        // keep talking to one Wheeler object rather than learning which half of
        // the split answers which question.
        //
        // GetAPI(), GetAPIVersion() and SupportsV2Features() are deliberately
        // NOT forwarded. They had no callers, and GetAPI() in particular handed
        // the raw handle to anyone who asked — exactly the boundary
        // WheelerConnection exists to draw. Anything inside this class that
        // needs the handle uses the private Api() shorthand below; anything
        // outside it should be asking WheelerConnection directly.
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

        // Log API info
        void LogAPIInfo();

        // ============================================================================
        // Spell Recommendation Wheel Management (v0.12.0: Multi-page support)
        // ============================================================================

        // Create recommendation wheels - one per page from SlotSettings
        // Call after SlotSettings is loaded and Wheeler is initialized
        // Returns true if at least one wheel was created
        bool CreateRecommendationWheels();

        // Tear down all recommendation wheels: detaches the records under
        // m_pageDataMutex, then issues the Wheeler delete calls with no lock
        // held (deletes may fire synchronous callbacks that re-acquire it).
        // Do NOT call while already holding m_pageDataMutex; use
        // DetachWheelsLocked() + IssueWheelDeletes() internally instead.
        void DestroyRecommendationWheels();

        // Update the wheel for the CURRENT page with new recommendations
        // Called from update handler after slot allocation
        void UpdateRecommendations(const std::vector<RE::FormID>& spellFormIDs);

        // Update with wildcard flags - displays "Wildcard" subtext on appropriate slots
        void UpdateRecommendations(const std::vector<RE::FormID>& spellFormIDs,
                                   const std::vector<bool>& isWildcard,
                                   const std::vector<uint16_t>& uniqueIDs = {});

        // Update wheel for a SPECIFIC page (for cross-page updates)
        void UpdateRecommendationsForPage(size_t pageIndex,
                                          const std::vector<RE::FormID>& spellFormIDs,
                                          const std::vector<bool>& isWildcard,
                                          const std::vector<uint16_t>& uniqueIDs = {},
                                          const std::vector<std::string>& subtexts = {});

        // Check if at least one recommendation wheel exists.
        // Scans all pages rather than only page 0: a failed/zero-slot page 0 is
        // stored as a wheelIndex=-1 placeholder, but later pages may hold real
        // wheels — the backend should still run for those.
        // Locks m_pageDataMutex (like its sibling accessors) — CreateRecommendationWheels/
        // DestroyRecommendationWheels can reallocate or clear m_pageWheels from another
        // thread, so an unlocked iteration here would be UB. Not noexcept: lock may throw.
        [[nodiscard]] bool HasRecommendationWheels() const;

        // Get wheel index for a page (returns -1 if invalid)
        [[nodiscard]] int32_t GetWheelIndexForPage(size_t pageIndex) const;

        // Get current page's wheel index
        [[nodiscard]] int32_t GetCurrentWheelIndex() const;

        // Switch Wheeler's active wheel to match a given page index.
        // No-op if Wheeler is not connected or the page has no wheel.
        // Returns true if the wheel was switched successfully.
        bool SetActivePage(size_t pageIndex);

        // Legacy compatibility (returns page 0 wheel)
        [[nodiscard]] bool HasRecommendationWheel() const { return HasRecommendationWheels(); }
        [[nodiscard]] int32_t GetRecommendationWheelIndex() const noexcept { return GetWheelIndexForPage(0); }
        [[nodiscard]] int32_t GetPrimaryWheelIndex() const noexcept { return GetWheelIndexForPage(0); }
        [[nodiscard]] int32_t GetAlternateWheelIndex() const noexcept { return GetWheelIndexForPage(1); }

        // ============================================================================
        // Callback Handlers (called by Wheeler)
        // ============================================================================

        // Register callbacks with Wheeler API
        void RegisterCallbacks();

        // Unregister callbacks (call on shutdown)
        void UnregisterCallbacks();

        // Check if our wheel is currently visible
        [[nodiscard]] bool IsWheelVisible() const noexcept { return m_wheelVisible; }

        // ============================================================================
        // Urgent Auto-Focus (A1: Override while Wheeler open)
        // ============================================================================

        // Attempt to auto-focus Wheeler to Huginn wheel when an urgent override fires.
        // Returns true if auto-focus was triggered.
        // THREAD SAFETY: Calls SetActiveWheelIndex outside any mutex.
        bool TryUrgentAutoFocus(int overridePriority);

        // ============================================================================
        // State Validation (A2: Debug only)
        // ============================================================================

        // Verify cached FormIDs match Wheeler's actual state.
        // Debug builds only — logs mismatches at warn level. Zero overhead in Release.
        void ValidateWheelState() const;

        // ============================================================================
        // Active Managed Wheel Tracking
        // ============================================================================

        // Get the Huginn page index of the currently active Wheeler wheel.
        // Returns -1 if Wheeler is not open, not connected, or the active wheel
        // is not one of our managed wheels.
        [[nodiscard]] int GetActiveManagedPage() const;

        // Process a deferred wheel-close event.  Called from the update loop
        // where IsWheelOpen() is accurate (unlike inside the callback).
        // Returns true if the wheel was truly closed and IntuitionMenu was shown.
        bool CheckPendingWheelClose();

        // ============================================================================
        // Post-Activation Policy (Part B)
        // ============================================================================

        // Check if a slot is marked activation-emptied (Empty policy)
        [[nodiscard]] bool IsSlotActivationEmptied(size_t pageIndex, size_t slotIndex) const;

        // Clear activation-emptied flags for a page (called when new candidates arrive)
        void ClearActivationEmptied(size_t pageIndex);

    private:
        WheelerClient() = default;

        // Maximum pages supported (matches SlotSettings::MAX_PAGES)
        static constexpr size_t MAX_PAGES = 10;

        // Shorthand for the handle owned by WheelerConnection. Load it ONCE per
        // function into a local — re-reading it mid-sequence would let a
        // concurrent disconnect change the answer between two API calls that
        // were meant to see the same Wheeler.
        //
        // The rule is per-function, not transitive: SetEntrySubtext and
        // ClearEntrySubtext load their own handle, so a caller that hoists api
        // and then calls them does not extend its snapshot into them. Making
        // that transitive means passing the handle down as a parameter, which
        // belongs with the step-2 WheelSync extraction that rewrites these call
        // sites anyway.
        [[nodiscard]] static WheelerAPI::IWheelerAPI* Api() noexcept
        {
            return WheelerConnection::GetSingleton().Api();
        }

        // Per-page wheel data (v0.12.0 multi-page support)
        struct PageWheel
        {
            int32_t wheelIndex = -1;                    // Wheeler wheel index
            size_t slotCount = 0;                       // Number of slots for this page
            std::string pageName;                       // Page name (e.g., "Combat")
            // Strings whose c_str() is exported to Wheeler are indefinite borrows:
            // Wheeler stores the pointer and reads it while rendering, so the
            // buffer must stay alive AND address-stable until replaced/cleared
            // through the API. Heap-owned (unique_ptr) storage survives PageWheel
            // moves and m_pageWheels reallocation; a plain std::string does not
            // (MSVC SSO relocates short-string bytes on every move).
            std::unique_ptr<std::string> wheelLabel;    // Full wheel label (e.g., "Huginn: Combat"); non-null iff a wheel was ever created for this record (survives index invalidation — Wheeler may still hold the pointer)
            std::vector<RE::FormID> slotFormIDs;        // Cached FormIDs per slot
            std::vector<bool> slotWildcard;             // Wildcard flags per slot
            std::vector<uint16_t> slotUniqueIDs;        // Cached UniqueIDs per slot (for weapons)
            std::vector<std::unique_ptr<std::string>> slotSubtexts;  // Cached FINAL subtext labels (wildcard-applied); exported to Wheeler (see above); null ≙ empty
            std::vector<std::string> slotRawSubtexts;    // Cached RAW incoming subtexts (for the content-unchanged early-out)
            std::vector<uint8_t> slotRetries;            // Retry counter per slot (max MAX_SLOT_RETRIES)
            std::vector<bool> slotActivationEmptied;     // Activation-emptied flags (Empty policy)
        };
        std::vector<PageWheel> m_pageWheels;            // One wheel per page

        // Negative cache for AddItemByFormID rejects. After MAX_SLOT_RETRIES
        // consecutive failures of the same (formID, uniqueID), further attempts
        // are suppressed for ADD_FAIL_COOLDOWN — slot churn (lock expiry,
        // re-allocation) otherwise restarts the retry cycle every few seconds
        // (observed: 569 API rejects in 11 min from one bad save entry that
        // Wheeler answers with UnsupportedFormType). Keyed globally, not
        // per-page: the same combo fails identically on every wheel.
        // GUARDED_BY(m_pageDataMutex).
        std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> m_addFailCooldowns;
        static constexpr std::chrono::seconds ADD_FAIL_COOLDOWN{30};
        [[nodiscard]] static constexpr uint64_t AddFailKey(RE::FormID formID, uint16_t uniqueID) noexcept
        {
            return (static_cast<uint64_t>(formID) << 16) | uniqueID;
        }

        // Helper to set/clear entry subtext
        void SetEntrySubtext(int32_t wheelIndex, int32_t entryIndex, const char* text);
        void ClearEntrySubtext(int32_t wheelIndex, int32_t entryIndex);

        // Callback state
        std::atomic<bool> m_wheelVisible{false};
        std::atomic<bool> m_pendingWheelClose{false};
        std::atomic<int32_t> m_pendingCloseWheelIndex{-1};  // Wheel index from the last close callback
        std::atomic<bool> m_itemActivatedWhileOpen{false};  // Track if player activated an item while wheel was open
        mutable std::mutex m_callbackMutex;     // Protects callback state (mutable for const methods)

        // Protects m_pageWheels data (FormIDs, subtexts, flags).
        // Lock ordering: m_callbackMutex (outer) → m_pageDataMutex (inner).
        // Callbacks hold both; update-loop functions hold only m_pageDataMutex.
        mutable std::mutex m_pageDataMutex;

        // Static callback trampolines (call into singleton instance)
        static void OnItemActivated(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex, uint32_t formID, bool isPrimary);
        static void OnWheelStateChanged(int32_t wheelIndex, bool isOpen);
        static void OnEditModeChanged(bool entered, const WheelerAPI::WheelChange* changes, size_t changeCount);

        // Helper methods
        void UpdatePageWheel(size_t pageIndex, const std::vector<RE::FormID>& spellFormIDs,
                             const std::vector<bool>& isWildcard,
                             const std::vector<uint16_t>& uniqueIDs,
                             const std::vector<std::string>& subtexts);

        // Get human-readable item type name for logging
        static const char* GetItemTypeName(RE::FormID formID);

        // Wheel state change helpers (extracted from OnWheelStateChanged)
        // HandleWheelOpened returns a wheel index to auto-focus (-1 if none).
        // The caller must apply SetActiveWheelIndex OUTSIDE the mutex to avoid
        // re-entrant callback deadlock.
        int32_t HandleWheelOpened(int32_t wheelIndex);
        void HandleWheelClosed(int32_t wheelIndex);

        // Find which page a wheel belongs to (-1 if not found)
        [[nodiscard]] int FindPageForWheel(int32_t wheelIndex) const;

        // Check if a wheel belongs to Huginn
        [[nodiscard]] bool IsOurWheel(int32_t wheelIndex) const;

        // Detach all page-wheel records for teardown. REQUIRES: m_pageDataMutex
        // held. The returned vector keeps the subtext strings alive (addresses
        // stable — the vector move steals the buffer) until IssueWheelDeletes
        // has told Wheeler to drop its references.
        [[nodiscard]] std::vector<PageWheel> DetachWheelsLocked();

        // Issue the cross-DLL subtext-clear + wheel-delete calls for detached
        // wheels. External teardown paths call this WITHOUT m_pageDataMutex held
        // (a delete may fire a synchronous callback that re-acquires it);
        // CreateRecommendationWheels calls it under the lock on the documented
        // assumption that already-invalidated wheels fire no callbacks.
        void IssueWheelDeletes(std::vector<PageWheel> staleWheels);
    };
}
