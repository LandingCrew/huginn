#include "WheelerConnection.h"

#include <Windows.h>
#include <spdlog/spdlog.h>

namespace Huginn::Wheeler
{
    // Log tag stays "[WheelerClient]" throughout this file even though the code
    // now lives in WheelerConnection. This extraction is meant to be provably
    // behaviour-preserving, and identical log output is how that gets checked:
    // a Debug session before and after should diff clean. Renaming the tag would
    // also break existing greps in docs/playtest and the soak tooling.

    WheelerConnection& WheelerConnection::GetSingleton()
    {
        static WheelerConnection instance;
        return instance;
    }

    bool WheelerConnection::TryConnect()
    {
        if (Api()) {
            return true;
        }

        // Try to get Wheeler.dll handle
        HMODULE hWheeler = GetModuleHandleA("Wheeler.dll");
        if (!hWheeler) {
            spdlog::debug("[WheelerClient] Wheeler.dll not loaded");
            return false;
        }

        spdlog::debug("[WheelerClient] Found Wheeler.dll at {:p}", static_cast<void*>(hWheeler));

        // Get the API interface
        using GetWheelerAPIFn = WheelerAPI::IWheelerAPI* (*)();
        auto GetWheelerAPI = reinterpret_cast<GetWheelerAPIFn>(
            GetProcAddress(hWheeler, "GetWheelerAPI"));

        if (!GetWheelerAPI) {
            spdlog::warn("[WheelerClient] GetWheelerAPI export not found - old Wheeler version?");
            return false;
        }

        auto* api = GetWheelerAPI();
        if (!api) {
            spdlog::warn("[WheelerClient] GetWheelerAPI() returned nullptr");
            return false;
        }

        if (api->version < WheelerAPI::API_VERSION_MIN) {
            spdlog::warn("[WheelerClient] API version too old: got {}, need >= {}",
                api->version, WheelerAPI::API_VERSION_MIN);
            return false;
        }

        // Deliberately a warning, not a reject. A future Wheeler that only APPENDS
        // to IWheelerAPI stays binary-compatible with everything we call, so
        // hard-failing would break forward compat for no reason. But one that
        // REORDERS the struct would sail through the >= MIN gate and we would call
        // through misaligned function pointers — a crash with no breadcrumb. This
        // line is that breadcrumb, and it is what makes API_VERSION_MAX mean
        // something (it had no references at all before).
        if (api->version > WheelerAPI::API_VERSION_MAX) {
            spdlog::warn("[WheelerClient] API version {} is newer than the {} this build knows; "
                         "assuming append-only compatibility",
                api->version, WheelerAPI::API_VERSION_MAX);
        }

        // Publish last. The version gate runs against the local pointer, so a
        // rejected API is never briefly visible to another thread — the old code
        // stored it and then reset it to nullptr, leaving a window in which
        // IsConnected() would have answered true for an unsupported version.
        m_api.store(api, std::memory_order_release);

        spdlog::info("[WheelerClient] Connected to Wheeler API v{} (v2={})",
            api->version, api->version >= 2 ? "yes" : "no");

        return true;
    }

    void WheelerConnection::RegisterCallbacks(WheelerAPI::ItemActivatedCallback itemCb,
                                              WheelerAPI::WheelStateCallback wheelCb,
                                              WheelerAPI::EditModeCallback editCb)
    {
        auto* api = Api();
        if (!api) {
            return;
        }

        api->RegisterItemActivatedCallback(itemCb);
        api->RegisterWheelStateCallback(wheelCb);
        api->RegisterEditModeCallback(editCb);

        spdlog::debug("[WheelerClient] Callbacks registered: ItemActivated={:p}, WheelState={:p}, EditMode={:p}",
            reinterpret_cast<void*>(itemCb),
            reinterpret_cast<void*>(wheelCb),
            reinterpret_cast<void*>(editCb));
    }

    void WheelerConnection::UnregisterCallbacks()
    {
        auto* api = Api();
        if (!api) {
            return;
        }

        spdlog::info("[WheelerClient] Unregistering callbacks...");

        api->UnregisterItemActivatedCallback();
        api->UnregisterWheelStateCallback();
        api->UnregisterEditModeCallback();

        spdlog::info("[WheelerClient] Callbacks unregistered");
    }

    void WheelerConnection::LogAPIInfo() const
    {
        auto* api = Api();
        if (!api) {
            spdlog::info("[WheelerClient] Not connected to Wheeler");
            return;
        }

        spdlog::info("[WheelerClient] API v{}, wheels={}, active={}",
            api->version, api->GetWheelCount(), api->GetActiveWheelIndex());
        spdlog::debug("[WheelerClient] Initialized={}, Open={}, EditMode={}, v2={}",
            api->IsInitialized(), api->IsWheelOpen(), api->IsInEditMode(),
            api->version >= 2);
        spdlog::debug("[WheelerClient] API fn ptrs: ItemCb={:p}, WheelStateCb={:p}, EditModeCb={:p}",
            reinterpret_cast<void*>(api->RegisterItemActivatedCallback),
            reinterpret_cast<void*>(api->RegisterWheelStateCallback),
            reinterpret_cast<void*>(api->RegisterEditModeCallback));
    }
}
