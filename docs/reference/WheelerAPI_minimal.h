// =============================================================================
// Wheeler API - Minimal Types Header (v2)
// =============================================================================
//
// This header contains only the types needed to interface with Wheeler.dll.
// Use this if you want to write your own client implementation rather than
// using the reference WheelerAPIClient.h.
//
// Usage:
//   1. Copy this file to your project
//   2. Get Wheeler.dll handle with GetModuleHandleA("Wheeler.dll")
//   3. Get API pointer: GetProcAddress(hWheeler, "GetWheelerAPI")
//   4. Cast and call to get IWheelerAPI*
//
// =============================================================================

#pragma once
#include <cstdint>

namespace WheelerAPI
{
    /// API version - check this against IWheelerAPI::version
    constexpr uint32_t API_VERSION = 2;

    /// Result codes returned by API functions
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

    /// Configuration for creating managed wheels
    struct WheelConfig
    {
        int32_t numEntries;       ///< Number of empty entry slots to create
        int32_t position;         ///< Where to insert (-1 = append to end)
        bool managed;             ///< If true, not saved to user config
        const char* clientName;   ///< Name for "[Managed By: ...]" label
        bool showLabel;           ///< Show the managed label when viewing this wheel

        // --- Label Styling (optional, 0 = use defaults) ---
        float labelFontSize;       ///< Font size for clientName label (default: 42)
        uint32_t labelColor;       ///< RGBA color for label (default: white)
        float labelOffsetY;        ///< Y offset below wheel indicator (default: 50)

        // --- Indicator Styling (optional) ---
        const char* indicatorText;        ///< Text on wheel indicator (default: "M", nullptr/"" = no indicator)
        uint32_t indicatorActiveColor;    ///< Color when wheel is active (default: cyan)
        uint32_t indicatorInactiveColor;  ///< Color when wheel is inactive (default: dim cyan)
    };

    /// Configuration for entry subtext (v2)
    struct SubtextConfig
    {
        const char* text;    ///< The subtext to display (nullptr or "" to clear)
        float offsetX;       ///< X offset from entry center (default: 0)
        float offsetY;       ///< Y offset below item name (default: 20)
        float fontSize;      ///< Font size in pixels (default: 28, 0 = use default)
        uint32_t color;      ///< RGBA color (default: 0xB0FFFFFF = 70% white, 0 = use default)
    };

    /// Change types reported during edit mode exit
    enum class ChangeType : int32_t
    {
        ItemAdded,
        ItemRemoved,
        EntryAdded,
        EntryRemoved,
        ItemMoved
    };

    /// Individual change record
    struct WheelChange
    {
        ChangeType type;
        int32_t wheelIndex;
        int32_t entryIndex;
        int32_t itemIndex;
        uint32_t formID;
    };

    /// Callback: item activated from wheel
    using ItemActivatedCallback = void (*)(int32_t wheelIndex, int32_t entryIndex,
                                           int32_t itemIndex, uint32_t formID, bool isPrimary);
    /// Callback: edit mode entered or exited
    using EditModeCallback = void (*)(bool entered, const WheelChange* changes, size_t changeCount);

    /// Callback: wheel UI opened or closed
    using WheelStateCallback = void (*)(int32_t wheelIndex, bool isOpen);

    /// The API interface - all Wheeler functionality accessed through this struct
    struct IWheelerAPI
    {
        uint32_t version;

        // State queries
        bool (*IsInitialized)();
        bool (*IsInEditMode)();
        bool (*IsWheelOpen)();

        // Managed wheel operations
        int32_t (*CreateManagedWheel)(const WheelConfig* config);
        Result (*DeleteManagedWheel)(int32_t wheelIndex);
        bool (*IsManagedWheel)(int32_t wheelIndex);

        // Wheel queries
        int32_t (*GetWheelCount)();
        int32_t (*GetActiveWheelIndex)();
        Result (*SetActiveWheelIndex)(int32_t index);
        bool (*IsWheelEmpty)(int32_t wheelIndex);

        // Entry operations
        int32_t (*GetEntryCount)(int32_t wheelIndex);
        int32_t (*AddEntry)(int32_t wheelIndex);
        Result (*DeleteEntry)(int32_t wheelIndex, int32_t entryIndex);
        bool (*IsEntryEmpty)(int32_t wheelIndex, int32_t entryIndex);

        // Item operations
        int32_t (*GetItemCount)(int32_t wheelIndex, int32_t entryIndex);
        int32_t (*AddItemByFormID)(int32_t wheelIndex, int32_t entryIndex,
                                    uint32_t formID, uint16_t uniqueID);
        Result (*RemoveItem)(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex);
        Result (*ClearEntry)(int32_t wheelIndex, int32_t entryIndex);
        uint32_t (*GetItemFormID)(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex);
        int32_t (*GetSelectedItemIndex)(int32_t wheelIndex, int32_t entryIndex);
        Result (*SetSelectedItemIndex)(int32_t wheelIndex, int32_t entryIndex, int32_t itemIndex);

        // Callbacks (pass nullptr to unregister)
        void (*RegisterItemActivatedCallback)(ItemActivatedCallback callback);
        void (*RegisterEditModeCallback)(EditModeCallback callback);
        void (*RegisterWheelStateCallback)(WheelStateCallback callback);

        // Explicit unregister (convenience)
        void (*UnregisterItemActivatedCallback)();
        void (*UnregisterEditModeCallback)();
        void (*UnregisterWheelStateCallback)();

        // --- v2: Entry Subtext ---
        /// Set subtext displayed below an entry's item name (managed wheels only)
        Result (*SetManagedWheelEntrySubtext)(int32_t wheelIndex, int32_t entryIndex, const SubtextConfig* config);
    };

}  // namespace WheelerAPI

// =============================================================================
// Quick Start Example
// =============================================================================
/*

#include <Windows.h>
#include "WheelerAPI_minimal.h"

WheelerAPI::IWheelerAPI* g_api = nullptr;
int32_t g_wheelIndex = -1;

bool InitWheelerAPI()
{
    HMODULE hWheeler = GetModuleHandleA("Wheeler.dll");
    if (!hWheeler) return false;

    using Fn = WheelerAPI::IWheelerAPI* (*)();
    auto GetAPI = (Fn)GetProcAddress(hWheeler, "GetWheelerAPI");
    if (!GetAPI) return false;

    g_api = GetAPI();
    return g_api && g_api->version >= WheelerAPI::API_VERSION;
}

bool CreateMyWheel()
{
    if (!g_api || !g_api->IsInitialized()) return false;

    WheelerAPI::WheelConfig cfg = {
        .numEntries = 3,
        .position = -1,
        .managed = true,
        .clientName = "My Plugin",
        .showLabel = true,
        // Optional styling (0 = use defaults)
        .labelFontSize = 0,
        .labelColor = 0,
        .labelOffsetY = 0,
        .indicatorText = "P",  // Custom indicator instead of "M"
        .indicatorActiveColor = 0,
        .indicatorInactiveColor = 0
    };

    g_wheelIndex = g_api->CreateManagedWheel(&cfg);
    return g_wheelIndex >= 0;
}

// Set subtext on an entry (v2 feature)
void SetEntryLabel(int32_t entryIndex, const char* label)
{
    if (!g_api || g_wheelIndex < 0) return;

    WheelerAPI::SubtextConfig subtext = {
        .text = label,
        .offsetX = 0,
        .offsetY = 20,
        .fontSize = 0,  // Use default
        .color = 0      // Use default
    };

    g_api->SetManagedWheelEntrySubtext(g_wheelIndex, entryIndex, &subtext);
}

*/
