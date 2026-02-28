#pragma once

#include "../Config.h"
// Note: std headers (chrono, functional) come from PCH via RE/Skyrim.h
#include <atomic>
#include <mutex>

namespace Huginn::Update
{
   /**
    * @brief Time-based update handler using frame events
    *
    * This class provides reliable, time-based polling for game state evaluation
    * and widget updates. It uses SKSE's InputEvent as a frame-based trigger
    * (fires every frame when input is processed) with internal time throttling
    * to ensure consistent update intervals regardless of framerate.
    *
    * Key Features:
    * - Framerate-independent updates (uses real-time delta tracking)
    * - Configurable update interval (default: 100ms = 10 Hz)
    * - No multi-minute gaps (unlike MenuOpenCloseEvent)
    * - Low overhead (< 0.1ms per update check target)
    * - Fires continuously during gameplay (not just menu events)
    *
    * @note This replaces the old MenuOpenCloseEvent-based system from v0.4.x
    *       which had unacceptable multi-minute gaps when no menus were opened.
    */
   class UpdateHandler : public RE::BSTEventSink<RE::InputEvent*>
   {
   public:
      /**
       * @brief Callback function type for update notifications
       * @param deltaSeconds Time elapsed since last update (in seconds)
       */
      using UpdateCallback = std::function<void(float deltaSeconds)>;

      /**
       * @brief Get the singleton instance
       * @return Pointer to the singleton instance
       */
      static UpdateHandler* GetSingleton();

      /**
       * @brief Register this handler with SKSE's event system
       * @return true if registration succeeded, false otherwise
       */
      static bool Register();

      /**
       * @brief Set the callback function to be called on each update
       * @param callback Function to call with delta time in seconds
       */
      void SetUpdateCallback(UpdateCallback callback);

      /**
       * @brief Process frame event (called by SKSE event system)
       * @param a_event The input event (can be any input)
       * @param a_eventSource Event source (unused)
       * @return BSEventNotifyControl::kContinue to allow other sinks to process
       */
      RE::BSEventNotifyControl ProcessEvent(
      RE::InputEvent* const* a_event,
      RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override;

      /**
       * @brief Force an immediate update (bypasses throttling)
       * Useful for testing or important state changes
       */
      void ForceUpdate();

      /**
       * @brief Enable or disable updates
       * @param enabled true to enable updates, false to disable
       */
      void SetEnabled(bool enabled);

      /**
       * @brief Check if updates are currently enabled
       * @return true if enabled, false otherwise
       */
      [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled.load(std::memory_order_acquire); }

      /**
       * @brief Get the configured update interval in milliseconds
       * @return Update interval in milliseconds
       */
      [[nodiscard]] float GetUpdateInterval() const noexcept { return Config::UPDATE_INTERVAL_MS; }

      /**
       * @brief Get the time since last update in milliseconds
       * @return Milliseconds elapsed since last update
       */
      [[nodiscard]] float GetTimeSinceLastUpdate() const noexcept;

   private:
      UpdateHandler() = default;
      UpdateHandler(const UpdateHandler&) = delete;
      UpdateHandler(UpdateHandler&&) = delete;
      ~UpdateHandler() override = default;

      UpdateHandler& operator=(const UpdateHandler&) = delete;
      UpdateHandler& operator=(UpdateHandler&&) = delete;

      /// Last update time point
      std::chrono::steady_clock::time_point m_lastUpdate;

      /// Update callback function
      UpdateCallback m_updateCallback;

      /// Mutex for callback protection (thread safety)
      mutable std::mutex m_mutex;

      /// Whether updates are enabled (atomic for thread-safe access)
      std::atomic<bool> m_enabled{true};

      /// Internal update implementation
      /// @param now Current time point (passed from ProcessEvent to avoid redundant clock calls)
      void DoUpdate(std::chrono::steady_clock::time_point now);
   };
}
