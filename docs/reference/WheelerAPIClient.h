// =============================================================================
// Wheeler API Client Reference Implementation (v2)
// =============================================================================
//
// This is a complete, copy-paste-ready client implementation for integrating
// with Wheeler's external API. Drop this file into your SKSE plugin project.
//
// Requirements:
// - Wheeler.dll with API support (v2.0.10+)
// - Windows.h for GetModuleHandle/GetProcAddress
//
// Usage:
//   1. Copy this file to your project
//   2. Call WheelerClient::TryConnect() after SKSE loads (kPostLoad or kDataLoaded)
//   3. Call WheelerClient::CreateWheel() after game load (kPostLoadGame)
//   4. Call WheelerClient::UpdateItems() whenever your recommendations change
//
// v2 Features:
//   - Customizable wheel indicator text and colors
//   - Customizable label styling (font size, color, offset)
//   - Per-entry subtext (e.g., "Wildcard", "Low Magicka", etc.)
//
// Known Limitations / Future Work:
//   - Custom decals/art for wheel indicators not yet supported
//   - Custom entry icons not yet supported
//
// =============================================================================

#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace WheelerAPI
{
    // =========================================================================
    // API Version - Clients should check this matches or exceeds their needs
    // =========================================================================
    constexpr uint32_t API_VERSION = 2;

    // =========================================================================
    // Result Codes
    // =========================================================================
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

    // =========================================================================
    // Configuration for creating a managed wheel
    // =========================================================================
    struct WheelConfig
    {
        int32_t numEntries;       // Number of empty entry slots to create
        int32_t position;         // Position in wheel list (-1 = append to end)
        bool managed;             // If true, wheel is not saved to user config
        const char* clientName;   // Name displayed in "[Managed By: ...]" label
        bool showLabel;           // If true, shows the managed label when wheel is active

        // --- Label Styling (optional, 0 = use defaults) ---
        float labelFontSize;       // Font size for clientName label (default: 42)
        uint32_t labelColor;       // RGBA color for label (default: white)
        float labelOffsetY;        // Y offset below wheel indicator (default: 50)

        // --- Indicator Styling (optional) ---
        const char* indicatorText;        // Text on wheel indicator (default: "M", nullptr/"" = no indicator)
        uint32_t indicatorActiveColor;    // Color when wheel is active (default: cyan)
        uint32_t indicatorInactiveColor;  // Color when wheel is inactive (default: dim cyan)
    };

    // =========================================================================
    // Configuration for entry subtext (v2)
    // =========================================================================
    struct SubtextConfig
    {
        const char* text;    // The subtext to display (nullptr or "" to clear)
        float offsetX;       // X offset from entry center (default: 0)
        float offsetY;       // Y offset below item name (default: 20)
        float fontSize;      // Font size in pixels (default: 28, 0 = use default)
        uint32_t color;      // RGBA color (default: 0xB0FFFFFF = 70% white, 0 = use default)
    };

    // =========================================================================
    // Change tracking for edit mode callbacks
    // =========================================================================
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

    // =========================================================================
    // Callback function types
    // =========================================================================

    // Called when user activates an item from the wheel
    // isPrimary: true for left-click/primary action, false for right-click/secondary
    using ItemActivatedCallback = void (*)(int32_t wheelIndex, int32_t entryIndex,
                                           int32_t itemIndex, uint32_t formID, bool isPrimary);

    // Called when user enters/exits edit mode
    // changes array is only valid during the callback - copy if you need to store
    using EditModeCallback = void (*)(bool entered, const WheelChange* changes, size_t changeCount);

    // Called when wheel UI opens or closes
    // wheelIndex: which wheel is now active (-1 if closed)
    using WheelStateCallback = void (*)(int32_t wheelIndex, bool isOpen);

    // =========================================================================
    // The API Interface - function pointers provided by Wheeler.dll
    // =========================================================================
    struct IWheelerAPI
    {
        uint32_t version;

        // --- State Queries ---
        bool (*IsInitialized)();
        bool (*IsInEditMode)();
        bool (*IsWheelOpen)();

        // --- Managed Wheel Operations ---
        int32_t (*CreateManagedWheel)(const WheelConfig* config);
        Result (*DeleteManagedWheel)(int32_t wheelIndex);
        bool (*IsManagedWheel)(int32_t wheelIndex);

        // --- Wheel Queries ---
        int32_t (*GetWheelCount)();
        int32_t (*GetActiveWheelIndex)();
        Result (*SetActiveWheelIndex)(int32_t index);
        bool (*IsWheelEmpty)(int32_t wheelIndex);

        // --- Entry Operations ---
        int32_t (*GetEntryCount)(int32_t wheelIndex);
        int32_t (*AddEntry)(int32_t wheelIndex);
        Result (*DeleteEntry)(int32_t wheelIndex, int32_t entryIndex);
        bool (*IsEntryEmpty)(int32_t wheelIndex, int32_t entryIndex);

        // --- Item Operations ---
        int32_t (*GetItemCount)(int32_t wheelIndex, int32_t entryIndex);
        int32_t (*AddItemByFormID)(int32_t wheelIndex, int32_t entryIndex,
                                    uint32_t formID, uint16_t uniqueID);
        Result (*RemoveItem)(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex);
        Result (*ClearEntry)(int32_t wheelIndex, int32_t entryIndex);
        uint32_t (*GetItemFormID)(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex);
        int32_t (*GetSelectedItemIndex)(int32_t wheelIndex, int32_t entryIndex);
        Result (*SetSelectedItemIndex)(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex);

        // --- Callbacks ---
        void (*RegisterItemActivatedCallback)(ItemActivatedCallback callback);
        void (*RegisterEditModeCallback)(EditModeCallback callback);
        void (*RegisterWheelStateCallback)(WheelStateCallback callback);

        // --- Unregister Callbacks ---
        void (*UnregisterItemActivatedCallback)();
        void (*UnregisterEditModeCallback)();
        void (*UnregisterWheelStateCallback)();

        // --- v2: Entry Subtext ---
        // Set subtext displayed below an entry's item name (managed wheels only)
        Result (*SetManagedWheelEntrySubtext)(int32_t wheelIndex, int32_t entryIndex, const SubtextConfig* config);
    };

}  // namespace WheelerAPI


// =============================================================================
// Reference Client Implementation
// =============================================================================
//
// This is a ready-to-use singleton client. Customize as needed for your plugin.
//
// Key features:
// - Lazy connection to Wheeler API
// - Managed wheel creation with customizable config
// - Optimized item updates (only changes slots when content differs)
// - Per-entry subtext support (v2)
// - Thread-safe singleton pattern
//
// =============================================================================

class WheelerClient
{
public:
    // =========================================================================
    // Singleton access
    // =========================================================================
    static WheelerClient& GetSingleton()
    {
        static WheelerClient instance;
        return instance;
    }

    // =========================================================================
    // Connection Management
    // =========================================================================

    /// Try to connect to Wheeler API
    /// @return true if connected successfully, false if Wheeler not available
    bool TryConnect()
    {
        if (m_api) {
            return true;  // Already connected
        }

        // Try to get Wheeler.dll handle
        HMODULE hWheeler = GetModuleHandleA("Wheeler.dll");
        if (!hWheeler) {
            // Wheeler.dll not loaded - this is normal if user doesn't have Wheeler
            return false;
        }

        // Get the API entry point
        using GetWheelerAPIFn = WheelerAPI::IWheelerAPI* (*)();
        auto GetWheelerAPI = reinterpret_cast<GetWheelerAPIFn>(
            GetProcAddress(hWheeler, "GetWheelerAPI"));

        if (!GetWheelerAPI) {
            // Old version of Wheeler without API support
            return false;
        }

        m_api = GetWheelerAPI();
        if (!m_api) {
            // GetWheelerAPI() returned nullptr
            return false;
        }

        // Version check
        if (m_api->version < WheelerAPI::API_VERSION) {
            // API version too old
            m_api = nullptr;
            return false;
        }

        return true;
    }

    /// Check if connected to Wheeler API
    bool IsConnected() const { return m_api != nullptr; }

    /// Get raw API pointer (nullptr if not connected)
    WheelerAPI::IWheelerAPI* GetAPI() const { return m_api; }

    // =========================================================================
    // Wheel Management
    // =========================================================================

    /// Create a managed wheel for this client with default styling
    /// @param clientName Name to display in "[Managed By: ...]" label
    /// @param numSlots Number of item slots (entries) to create
    /// @param showLabel Whether to show the managed label when viewing this wheel
    /// @return true if wheel created or already exists
    bool CreateWheel(const char* clientName, int32_t numSlots = 3, bool showLabel = true)
    {
        WheelerAPI::WheelConfig config = {
            .numEntries = numSlots,
            .position = -1,  // Append to end
            .managed = true,
            .clientName = clientName,
            .showLabel = showLabel,
            // Use defaults for all styling
            .labelFontSize = 0,
            .labelColor = 0,
            .labelOffsetY = 0,
            .indicatorText = nullptr,  // Use default "M"
            .indicatorActiveColor = 0,
            .indicatorInactiveColor = 0
        };

        return CreateWheelWithConfig(&config);
    }

    /// Create a managed wheel with full configuration
    /// @param config Full wheel configuration including styling
    /// @return true if wheel created or already exists
    bool CreateWheelWithConfig(const WheelerAPI::WheelConfig* config)
    {
        if (!m_api) {
            return false;
        }

        if (!m_api->IsInitialized()) {
            // Wheeler not ready yet - try again later
            return false;
        }

        // Already have a wheel?
        if (m_wheelIndex >= 0) {
            return true;
        }

        m_wheelIndex = m_api->CreateManagedWheel(config);
        if (m_wheelIndex < 0) {
            // Creation failed - error code in m_wheelIndex
            return false;
        }

        // Initialize slot tracking
        m_currentSlotFormIDs.resize(static_cast<size_t>(config->numEntries), 0);

        return true;
    }

    /// Check if we have a managed wheel
    bool HasWheel() const { return m_wheelIndex >= 0; }

    /// Get the wheel index
    int32_t GetWheelIndex() const { return m_wheelIndex; }

    /// Delete the managed wheel
    void DeleteWheel()
    {
        if (m_api && m_wheelIndex >= 0) {
            m_api->DeleteManagedWheel(m_wheelIndex);
            m_wheelIndex = -1;
            m_currentSlotFormIDs.clear();
        }
    }

    // =========================================================================
    // Item Management
    // =========================================================================

    /// Update the wheel with new items
    /// Only modifies slots that have changed, minimizing overhead.
    /// @param formIDs Vector of FormIDs to display (up to slot count)
    void UpdateItems(const std::vector<uint32_t>& formIDs)
    {
        if (!m_api || m_wheelIndex < 0) {
            return;
        }

        int32_t entryCount = m_api->GetEntryCount(m_wheelIndex);
        if (entryCount <= 0) {
            return;
        }

        size_t maxSlots = static_cast<size_t>(entryCount);

        for (size_t i = 0; i < maxSlots; ++i) {
            uint32_t newFormID = (i < formIDs.size()) ? formIDs[i] : 0;
            uint32_t currentFormID = (i < m_currentSlotFormIDs.size()) ? m_currentSlotFormIDs[i] : 0;

            // Skip if unchanged
            if (newFormID == currentFormID) {
                continue;
            }

            int32_t entryIndex = static_cast<int32_t>(i);

            // Clear existing item in this entry
            m_api->ClearEntry(m_wheelIndex, entryIndex);

            // Add new item if we have one
            if (newFormID != 0) {
                m_api->AddItemByFormID(m_wheelIndex, entryIndex, newFormID, 0);
            }

            // Track the update
            if (i < m_currentSlotFormIDs.size()) {
                m_currentSlotFormIDs[i] = newFormID;
            }
        }
    }

    /// Clear all items from the wheel
    void ClearAllItems()
    {
        if (!m_api || m_wheelIndex < 0) {
            return;
        }

        int32_t entryCount = m_api->GetEntryCount(m_wheelIndex);
        for (int32_t i = 0; i < entryCount; ++i) {
            m_api->ClearEntry(m_wheelIndex, i);
        }

        // Reset tracking
        std::fill(m_currentSlotFormIDs.begin(), m_currentSlotFormIDs.end(), 0);
    }

    // =========================================================================
    // Entry Subtext (v2)
    // =========================================================================

    /// Set subtext for an entry (displayed below item name)
    /// @param entryIndex Which entry slot to set subtext for
    /// @param text The text to display (nullptr or "" to clear)
    void SetEntrySubtext(int32_t entryIndex, const char* text)
    {
        if (!m_api || m_wheelIndex < 0) {
            return;
        }

        WheelerAPI::SubtextConfig config = {
            .text = text,
            .offsetX = 0,
            .offsetY = 20,
            .fontSize = 0,  // Use default
            .color = 0      // Use default
        };

        m_api->SetManagedWheelEntrySubtext(m_wheelIndex, entryIndex, &config);
    }

    /// Set subtext for an entry with full configuration
    /// @param entryIndex Which entry slot to set subtext for
    /// @param config Full subtext configuration
    void SetEntrySubtextWithConfig(int32_t entryIndex, const WheelerAPI::SubtextConfig* config)
    {
        if (!m_api || m_wheelIndex < 0) {
            return;
        }

        m_api->SetManagedWheelEntrySubtext(m_wheelIndex, entryIndex, config);
    }

    /// Clear subtext for an entry
    /// @param entryIndex Which entry slot to clear subtext from
    void ClearEntrySubtext(int32_t entryIndex)
    {
        SetEntrySubtext(entryIndex, nullptr);
    }

    // =========================================================================
    // Callbacks
    // =========================================================================

    /// Register callback for when user activates an item
    void RegisterItemActivatedCallback(WheelerAPI::ItemActivatedCallback callback)
    {
        if (m_api) {
            m_api->RegisterItemActivatedCallback(callback);
        }
    }

    /// Register callback for edit mode enter/exit
    void RegisterEditModeCallback(WheelerAPI::EditModeCallback callback)
    {
        if (m_api) {
            m_api->RegisterEditModeCallback(callback);
        }
    }

    /// Register callback for wheel open/close
    void RegisterWheelStateCallback(WheelerAPI::WheelStateCallback callback)
    {
        if (m_api) {
            m_api->RegisterWheelStateCallback(callback);
        }
    }

    /// Unregister all callbacks
    void UnregisterAllCallbacks()
    {
        if (m_api) {
            m_api->UnregisterItemActivatedCallback();
            m_api->UnregisterEditModeCallback();
            m_api->UnregisterWheelStateCallback();
        }
    }

    // =========================================================================
    // Cleanup
    // =========================================================================

    /// Full cleanup - call on plugin shutdown
    void Shutdown()
    {
        UnregisterAllCallbacks();
        DeleteWheel();
        m_api = nullptr;
    }

private:
    WheelerClient() = default;
    ~WheelerClient() = default;
    WheelerClient(const WheelerClient&) = delete;
    WheelerClient& operator=(const WheelerClient&) = delete;

    WheelerAPI::IWheelerAPI* m_api = nullptr;
    int32_t m_wheelIndex = -1;
    std::vector<uint32_t> m_currentSlotFormIDs;
};


// =============================================================================
// Usage Example
// =============================================================================
/*

// In your SKSE plugin message handler:
void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
{
    switch (a_msg->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        // Good time to try connecting to Wheeler
        WheelerClient::GetSingleton().TryConnect();
        break;

    case SKSE::MessagingInterface::kPostLoadGame:
        // Game loaded - create our recommendation wheel
        {
            auto& client = WheelerClient::GetSingleton();
            if (!client.IsConnected()) {
                client.TryConnect();
            }
            if (client.IsConnected()) {
                // Option 1: Simple creation with defaults
                client.CreateWheel("My Plugin", 4, true);

                // Option 2: Full config with custom styling
                WheelerAPI::WheelConfig config = {
                    .numEntries = 4,
                    .position = -1,
                    .managed = true,
                    .clientName = "My Plugin",
                    .showLabel = true,
                    .labelFontSize = 36,
                    .labelColor = 0xFFFFFFFF,  // White
                    .labelOffsetY = 60,
                    .indicatorText = "P",  // Custom indicator
                    .indicatorActiveColor = 0xFF00FF00,   // Green when active
                    .indicatorInactiveColor = 0x8000FF00  // Dim green when inactive
                };
                client.CreateWheelWithConfig(&config);
            }
        }
        break;
    }
}

// In your update loop (10 Hz recommended):
void OnGameUpdate()
{
    static auto lastUpdate = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();

    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count() < 100) {
        return;  // Throttle to 10 Hz
    }
    lastUpdate = now;

    auto& client = WheelerClient::GetSingleton();
    if (!client.HasWheel()) {
        return;
    }

    // Get your recommended items (spells, weapons, etc.)
    std::vector<uint32_t> recommendedFormIDs = GetMyRecommendations();

    // Update the wheel
    client.UpdateItems(recommendedFormIDs);

    // Optionally set subtext for special items (v2 feature)
    for (size_t i = 0; i < recommendedFormIDs.size(); ++i) {
        if (IsSpecialItem(recommendedFormIDs[i])) {
            client.SetEntrySubtext(static_cast<int32_t>(i), "Special");
        } else {
            client.ClearEntrySubtext(static_cast<int32_t>(i));
        }
    }
}

// On plugin shutdown:
void OnPluginShutdown()
{
    WheelerClient::GetSingleton().Shutdown();
}

*/
