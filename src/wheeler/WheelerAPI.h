#pragma once

#include <cstddef>  // size_t (EditModeCallback)
#include <cstdint>

// Wheeler's exported ABI, mirrored from C0kAdam's Wheeler API headers.
//
// Declaration-only: nothing here is Huginn's to change. It lives in its own
// header so the pieces that talk to Wheeler (WheelerConnection) and the pieces
// that only manage wheels (WheelerClient) can include what they actually need.
namespace WheelerAPI
{
    // Minimum API version we support (v1 = base Wheeler, v2 = subtext, v3 = batch
    // delete-by-client + managed metadata on the wheel)
    constexpr uint32_t API_VERSION_MIN = 1;
    constexpr uint32_t API_VERSION_MAX = 3;

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

        // --- v3 Only: Batch delete by client ---
        // Delete ALL managed wheels whose clientName matches, in one shift-safe pass.
        // NOTE: Only valid when IWheelerAPI::version >= 3 — check before calling.
        // Returns the number of wheels deleted (>= 0), or a negative Result on error.
        int32_t (*DeleteManagedWheelsForClient)(const char* clientName);
    };
}
