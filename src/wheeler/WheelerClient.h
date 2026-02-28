#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Forward declare the API types we need
namespace WheelerAPI
{
    // Minimum API version we support (v1 = base Wheeler, v2 = Wheeler with subtext support)
    constexpr uint32_t API_VERSION_MIN = 1;
    constexpr uint32_t API_VERSION_MAX = 2;

    enum class Result : int32_t
    {
        OK = 0,
        InvalidWheelIndex = -1,
        InvalidEntryIndex = -2,
        InvalidItemIndex = -3,
        InvalidFormID = -4,
        FormNotFound = -5,
        UnsupportedFormType = -6,
        WheelNotEmpty = -7,
        LastWheel = -8,
        NotInitialized = -9,
        NotManagedWheel = -10,
        InEditMode = -11,
        EntryNotEmpty = -12,
        InternalError = -100
    };

    // v1 WheelConfig - compatible with C0kAdam's Wheeler API
    // This is the base struct that all Wheeler implementations support
    struct WheelConfigV1
    {
        int32_t numEntries;
        int32_t position;
        bool managed;
        const char* clientName;
        bool showLabel;  // If true, show clientName as label when viewing this wheel
    };

    // v2 WheelConfig - extended with styling options (Wheeler v2+ with subtext)
    // IMPORTANT: Only use this with API v2+, v1 servers won't recognize the extra fields
    struct WheelConfig
    {
        int32_t numEntries;
        int32_t position;
        bool managed;
        const char* clientName;
        bool showLabel;  // If true, show clientName as label when viewing this wheel

        // --- Label Styling (optional, 0 = use defaults) ---
        float labelFontSize;       // Font size for clientName label (default: 42)
        uint32_t labelColor;       // RGBA color for label (default: white)
        float labelOffsetY;        // Y offset below wheel indicator (default: 50)

        // --- Indicator Styling (optional) ---
        const char* indicatorText;        // Text on wheel indicator (default: "M", nullptr/"" = no indicator)
        uint32_t indicatorActiveColor;    // Color when wheel is active (default: cyan)
        uint32_t indicatorInactiveColor;  // Color when wheel is inactive (default: dim cyan)
    };

    // Configuration for entry subtext (v2)
    struct SubtextConfig
    {
        const char* text;    // The subtext to display (nullptr or "" to clear)
        float offsetX;       // X offset from entry center (default: 0)
        float offsetY;       // Y offset below item name (default: 20)
        float fontSize;      // Font size in pixels (default: 28, 0 = use default)
        uint32_t color;      // RGBA color (default: 0xB0FFFFFF = 70% white, 0 = use default)
    };

    enum class ChangeType : int32_t
    {
        ItemAdded,
        ItemRemoved,
        EntryAdded,
        EntryRemoved,
        ItemMoved
    };

    struct WheelChange
    {
        ChangeType type;
        int32_t wheelIndex;
        int32_t entryIndex;
        int32_t itemIndex;
        uint32_t formID;
    };

    using ItemActivatedCallback = void (*)(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex, uint32_t formID, bool isPrimary);
    using EditModeCallback = void (*)(bool entered, const WheelChange* changes, size_t changeCount);
    using WheelStateCallback = void (*)(int32_t wheelIndex, bool isOpen);

    struct IWheelerAPI
    {
        uint32_t version;

        bool (*IsInitialized)();
        bool (*IsInEditMode)();
        bool (*IsWheelOpen)();

        int32_t (*CreateManagedWheel)(const WheelConfig* config);
        Result (*DeleteManagedWheel)(int32_t wheelIndex);
        bool (*IsManagedWheel)(int32_t wheelIndex);

        int32_t (*GetWheelCount)();
        int32_t (*GetActiveWheelIndex)();
        Result (*SetActiveWheelIndex)(int32_t index);
        bool (*IsWheelEmpty)(int32_t wheelIndex);

        int32_t (*GetEntryCount)(int32_t wheelIndex);
        int32_t (*AddEntry)(int32_t wheelIndex);
        Result (*DeleteEntry)(int32_t wheelIndex, int32_t entryIndex);
        bool (*IsEntryEmpty)(int32_t wheelIndex, int32_t entryIndex);

        int32_t (*GetItemCount)(int32_t wheelIndex, int32_t entryIndex);
        int32_t (*AddItemByFormID)(int32_t wheelIndex, int32_t entryIndex, uint32_t formID, uint16_t uniqueID);
        Result (*RemoveItem)(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex);
        Result (*ClearEntry)(int32_t wheelIndex, int32_t entryIndex);
        uint32_t (*GetItemFormID)(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex);
        int32_t (*GetSelectedItemIndex)(int32_t wheelIndex, int32_t entryIndex);
        Result (*SetSelectedItemIndex)(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex);

        // Pass nullptr to unregister a previously registered callback
        void (*RegisterItemActivatedCallback)(ItemActivatedCallback callback);
        void (*RegisterEditModeCallback)(EditModeCallback callback);
        void (*RegisterWheelStateCallback)(WheelStateCallback callback);

        // Unregister callbacks (convenience)
        void (*UnregisterItemActivatedCallback)();
        void (*UnregisterEditModeCallback)();
        void (*UnregisterWheelStateCallback)();

        // --- v2 Only: Entry Subtext ---
        // Set subtext displayed below an entry's item name (managed wheels only)
        // NOTE: This function pointer is only valid when IWheelerAPI::version >= 2
        // Always check version before calling! On v1 APIs, this pointer may be garbage.
        Result (*SetManagedWheelEntrySubtext)(int32_t wheelIndex, int32_t entryIndex, const SubtextConfig* config);
    };
}

namespace Huginn::Wheeler
{
    class WheelerClient
    {
    public:
        static WheelerClient& GetSingleton();

        // Try to connect to Wheeler API, returns true if successful
        bool TryConnect();

        // Check if connected
        [[nodiscard]] bool IsConnected() const noexcept { return m_api != nullptr; }

        // Get API version (0 if not connected)
        [[nodiscard]] uint32_t GetAPIVersion() const noexcept { return m_api ? m_api->version : 0; }

        // Check if v2 features are available (subtext, custom styling)
        [[nodiscard]] bool SupportsV2Features() const noexcept { return m_api && m_api->version >= 2; }

        // Check if the Wheeler UI is currently open (any wheel visible)
        [[nodiscard]] bool IsWheelOpen() const noexcept { return m_api && m_api->IsWheelOpen(); }

        // Get API pointer (nullptr if not connected)
        [[nodiscard]] WheelerAPI::IWheelerAPI* GetAPI() const noexcept { return m_api; }

        // Log API info
        void LogAPIInfo();

        // ============================================================================
        // Spell Recommendation Wheel Management (v0.12.0: Multi-page support)
        // ============================================================================

        // Create recommendation wheels - one per page from SlotSettings
        // Call after SlotSettings is loaded and Wheeler is initialized
        // Returns true if at least one wheel was created
        bool CreateRecommendationWheels();

        // Tear down all recommendation wheels (deletes from Wheeler, then clears vector)
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

        // Check if at least one recommendation wheel exists
        [[nodiscard]] bool HasRecommendationWheels() const noexcept { return !m_pageWheels.empty() && m_pageWheels[0].wheelIndex >= 0; }

        // Get wheel index for a page (returns -1 if invalid)
        [[nodiscard]] int32_t GetWheelIndexForPage(size_t pageIndex) const;

        // Get current page's wheel index
        [[nodiscard]] int32_t GetCurrentWheelIndex() const;

        // Switch Wheeler's active wheel to match a given page index.
        // No-op if Wheeler is not connected or the page has no wheel.
        // Returns true if the wheel was switched successfully.
        bool SetActivePage(size_t pageIndex);

        // Legacy compatibility (returns page 0 wheel)
        [[nodiscard]] bool HasRecommendationWheel() const noexcept { return HasRecommendationWheels(); }
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

        // Get the slot index where the last activation occurred (-1 if none)
        [[nodiscard]] int GetLastActivatedSlot() const noexcept { return m_lastActivatedSlot; }

    private:
        WheelerClient() = default;

        // Maximum pages supported (matches SlotSettings::MAX_PAGES)
        static constexpr size_t MAX_PAGES = 10;

        WheelerAPI::IWheelerAPI* m_api = nullptr;

        // Per-page wheel data (v0.12.0 multi-page support)
        struct PageWheel
        {
            int32_t wheelIndex = -1;                    // Wheeler wheel index
            size_t slotCount = 0;                       // Number of slots for this page
            std::string pageName;                       // Page name (e.g., "Combat")
            std::string wheelLabel;                     // Full wheel label (e.g., "Huginn: Combat") - MUST stay alive for Wheeler API
            std::vector<RE::FormID> slotFormIDs;        // Cached FormIDs per slot
            std::vector<bool> slotWildcard;             // Wildcard flags per slot
            std::vector<uint16_t> slotUniqueIDs;        // Cached UniqueIDs per slot (for weapons)
            std::vector<std::string> slotSubtexts;       // Cached subtext labels per slot
            std::vector<uint8_t> slotRetries;            // Retry counter per slot (max MAX_SLOT_RETRIES)
            std::vector<bool> slotActivationEmptied;     // Activation-emptied flags (Empty policy)
        };
        std::vector<PageWheel> m_pageWheels;            // One wheel per page

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

        // Post-activation tracking (Part B)
        int m_lastActivatedSlot = -1;           // Slot index where last activation occurred

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
    };
}
