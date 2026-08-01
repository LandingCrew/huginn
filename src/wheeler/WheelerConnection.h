#pragma once

#include "WheelerAPI.h"

#include <atomic>

namespace Huginn::Wheeler
{
    /// Owns the handle to Wheeler's exported API.
    ///
    /// This is the only place the API pointer is acquired, version-gated, or
    /// released. Everything else in Huginn asks this class for it. Splitting it
    /// out makes the publication rule explicit: the pointer is written on the
    /// SKSE message thread (TryConnect, from kPostLoadGame/kNewGame) and read
    /// from Wheeler's own callback thread, so it is atomic rather than a plain
    /// member. Previously the pointer's safety rested on the unwritten ordering
    /// "TryConnect always runs before RegisterCallbacks"; that ordering still
    /// holds, but it is no longer the only thing standing between us and a race.
    ///
    /// Load once per operation, not per call:
    ///     auto* api = conn.Api();
    ///     if (!api) return;
    /// Re-reading Api() inside a function would let a concurrent disconnect
    /// change the answer half-way through a multi-call sequence.
    class WheelerConnection
    {
    public:
        static WheelerConnection& GetSingleton();

        /// Locate Wheeler.dll, resolve GetWheelerAPI, and version-gate the
        /// result. Idempotent — returns true immediately if already connected.
        /// Does NOT register callbacks; the owner of the callback state does
        /// that (see WheelerClient::TryConnect).
        bool TryConnect();

        /// The API handle, or nullptr if not connected.
        [[nodiscard]] WheelerAPI::IWheelerAPI* Api() const noexcept
        {
            return m_api.load(std::memory_order_acquire);
        }

        [[nodiscard]] bool IsConnected() const noexcept { return Api() != nullptr; }

        /// API version, or 0 if not connected.
        [[nodiscard]] uint32_t Version() const noexcept
        {
            auto* api = Api();
            return api ? api->version : 0;
        }

        /// True if subtext and custom wheel styling are available.
        [[nodiscard]] bool SupportsV2Features() const noexcept { return Version() >= 2; }

        /// True if Wheeler currently has any wheel visible. Asks Wheeler
        /// directly rather than reading our own cached visibility flag.
        [[nodiscard]] bool IsWheelOpen() const noexcept
        {
            auto* api = Api();
            return api && api->IsWheelOpen();
        }

        /// Install Huginn's callback trampolines. Passing the function pointers
        /// in (rather than hard-coding them) keeps this class free of any
        /// knowledge of what Huginn does when a wheel opens or an item fires.
        void RegisterCallbacks(WheelerAPI::ItemActivatedCallback itemCb,
                               WheelerAPI::WheelStateCallback wheelCb,
                               WheelerAPI::EditModeCallback editCb);

        void UnregisterCallbacks();

        /// Dump version, wheel count, and trampoline addresses to the log.
        void LogAPIInfo() const;

    private:
        WheelerConnection() = default;

        std::atomic<WheelerAPI::IWheelerAPI*> m_api{nullptr};
    };
}
