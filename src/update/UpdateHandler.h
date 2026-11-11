#pragma once

#include "../Config.h"
#include "../core/LapHoldInputs.h"
// Note: std headers (chrono, functional) come from PCH via RE/Skyrim.h
#include <array>
#include <atomic>
#include <mutex>
#include <string_view>

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

      /// 0.24.29: ms the update loop has lost to suspensions since launch
      /// (each gap over the 1 s clamp, less the 1 s it counts). The combat
      /// upsert's wanted clock does not count them.
      [[nodiscard]] std::int64_t SuspendedMs() const noexcept { return m_suspendedMs.load(std::memory_order_relaxed); }

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
       * @brief Execute a callable while holding the update mutex.
       * Use this to safely mutate state that the update loop reads (e.g.,
       * registry rebuilds, pipeline resets) from another thread (console).
       */
      template <typename Fn>
      void RunExclusive(Fn&& fn)
      {
         std::lock_guard<std::mutex> lock(m_mutex);
         std::forward<Fn>(fn)();
      }

      /**
       * @brief RunExclusive without waiting: run `fn` under the update mutex
       * if it is free right now, and return whether it ran.
       * For the main thread (the hand equip gate, 0.24.26), which must not
       * block on a tick running elsewhere: a busy mutex means try again next
       * frame. Never call it from inside a tick (the mutex is not recursive).
       */
      template <typename Fn>
      bool TryRunExclusive(Fn&& fn)
      {
         std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
         if (!lock.owns_lock()) return false;
         std::forward<Fn>(fn)();
         return true;
      }

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
       * @brief Seconds since the player's latest input (0.23.51)
       *
       * Any key, mouse or gamepad button event (down, held or released), the
       * mouse moving, a stick off centre, a typed character: noted by
       * ProcessEvent for every event batch, enabled or not. +infinity before
       * the first. The need episodes' lap clock holds once this reaches
       * lapIdleSec (core/NeedEpisodes.h, SelectionLogV3).
       */
      [[nodiscard]] double SecondsSinceInput(std::chrono::steady_clock::time_point now) const noexcept;

      /**
       * @brief Re-read the stick deadzones from the game's INI settings (0.23.55)
       *
       * A thumbstick event counts as input (SecondsSinceInput) only when its
       * deflection lies beyond the deadzone the game's controls apply to that
       * stick: fMovementAxisDeadzone:Controls for the left,
       * fHeadingAxisDeadzone:Controls for the right (core/LapHoldInputs.h;
       * 0.15 when a setting cannot be read). Called at kDataLoaded, at every
       * kNewGame / kPostLoadGame, and by DoUpdate every 60 s. Logs the values
       * at info when they change. Runs on the message handler's thread and on
       * whichever thread runs the update tick (a game job thread in gameplay;
       * see THREADS in UpdateLoop.cpp); serialized by a mutex. The values reach
       * ProcessEvent through atomics.
       */
      void SyncStickDeadzones(std::string_view why);

   private:
      UpdateHandler() = default;
      UpdateHandler(const UpdateHandler&) = delete;
      UpdateHandler(UpdateHandler&&) = delete;
      ~UpdateHandler() override = default;

      UpdateHandler& operator=(const UpdateHandler&) = delete;
      UpdateHandler& operator=(UpdateHandler&&) = delete;

      /// Last update time point
      std::chrono::steady_clock::time_point m_lastUpdate;

      /// 0.24.29: the time the loop lost to suspensions (the clamp in
      /// DoUpdate: alt-tab, the console, a pause), ms since launch.
      std::atomic<std::int64_t> m_suspendedMs{ 0 };

      /// The latest player input, steady_clock ticks since its epoch
      /// (kNoInput before the first). Written by ProcessEvent (game threads),
      /// read by the update loop.
      static constexpr std::int64_t kNoInput = INT64_MIN;
      std::atomic<std::int64_t> m_lastInputTicks{ kNoInput };

      /// The stick deadzones IsPlayerInput applies (SyncStickDeadzones writes,
      /// ProcessEvent reads); the fallback until the first sync.
      std::atomic<float> m_leftStickDeadzone{ Core::Needs::kFallbackStickDeadzone };
      std::atomic<float> m_rightStickDeadzone{ Core::Needs::kFallbackStickDeadzone };
      /// SyncStickDeadzones' bookkeeping, under m_deadzoneMutex (the load
      /// messages and the update loop run on the main thread, but DoUpdate is
      /// also reachable through ForceUpdate).
      mutable std::mutex m_deadzoneMutex;
      static constexpr std::chrono::seconds kDeadzoneResync{ 60 };
      bool m_deadzonesSynced = false;
      std::chrono::steady_clock::time_point m_lastDeadzoneSync{};
      std::array<float, 4> m_lastDeadzonesSeen{ -2.0f, -2.0f, -2.0f, -2.0f };   // never a value read: the first sync logs

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
