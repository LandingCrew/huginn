#include "UpdateHandler.h"
#include "../util/ScopedTimer.h"
#include "../input/InputHandler.h"
#include "../Profiling.h"

namespace Huginn::Update
{
   UpdateHandler* UpdateHandler::GetSingleton()
   {
      static UpdateHandler singleton;
      return &singleton;
   }

   bool UpdateHandler::Register()
   {
      auto inputMgr = RE::BSInputDeviceManager::GetSingleton();
      if (!inputMgr) {
      logger::error("[UpdateHandler] Failed to get input device manager"sv);
      return false;
      }

      inputMgr->AddEventSink(GetSingleton());
      logger::info("[UpdateHandler] Registered with input system (update interval: {:.0f}ms)"sv,
      Config::UPDATE_INTERVAL_MS);

      // Initialize last update time
      GetSingleton()->m_lastUpdate = std::chrono::steady_clock::now();

      return true;
   }

   void UpdateHandler::SetUpdateCallback(UpdateCallback callback)
   {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_updateCallback = std::move(callback);
   }

   RE::BSEventNotifyControl UpdateHandler::ProcessEvent(
      RE::InputEvent* const* a_event,
      [[maybe_unused]] RE::BSTEventSource<RE::InputEvent*>* a_eventSource)
   {
      // Input events fire every frame - perfect for frame-based updates
      // We throttle internally with time checks to avoid excessive updates

      if (!m_enabled.load(std::memory_order_acquire) || !a_event) {
      return RE::BSEventNotifyControl::kContinue;
      }

      // v0.6.0: Process button events for input handling
      // Cache InputHandler reference to avoid repeated singleton lookups
      auto& inputHandler = Input::InputHandler::GetSingleton();

#ifndef NDEBUG
      static bool loggedFirstEvent = false;
#endif

      for (auto* event = *a_event; event; event = event->next) {
      if (auto* button = event->AsButtonEvent()) {
#ifndef NDEBUG
        if (!loggedFirstEvent && button->IsDown()) {
           logger::debug("[UpdateHandler] First button event received - input system working"sv);
           loggedFirstEvent = true;
        }
#endif
        // Process button and consume event if handled
        if (inputHandler.ProcessButton(button)) {
           // Clear userEvent to mark as consumed - prevents game from processing
           auto* idEvent = event->AsIDEvent();
           if (idEvent) {
            idEvent->userEvent = "";
            logger::trace("[UpdateHandler] Consumed input event (code=0x{:02X})"sv,
              button->GetIDCode());
           }
        }
      }
      }

      // Update InputHandler state (for double-tap timeout handling)
      inputHandler.Update();

      Huginn_FRAME_MARK;

      // Check if enough time has elapsed
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastUpdate);

      if (elapsed.count() >= Config::UPDATE_INTERVAL_MS) {
      DoUpdate(now);  // Pass time_point to avoid redundant clock call
      }

      return RE::BSEventNotifyControl::kContinue;
   }

   void UpdateHandler::ForceUpdate()
   {
      if (m_enabled.load(std::memory_order_acquire)) {
      DoUpdate(std::chrono::steady_clock::now());
      }
   }

   void UpdateHandler::SetEnabled(bool enabled)
   {
      bool wasEnabled = m_enabled.exchange(enabled, std::memory_order_acq_rel);

      if (wasEnabled != enabled) {
      logger::debug("[UpdateHandler] Updates {} (was {})"sv,
        enabled ? "enabled" : "disabled",
        wasEnabled ? "enabled" : "disabled");

      if (enabled) {
        // Reset timer when re-enabling
        m_lastUpdate = std::chrono::steady_clock::now();
      }
      }
   }

   float UpdateHandler::GetTimeSinceLastUpdate() const noexcept
   {
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastUpdate);
      return static_cast<float>(elapsed.count());
   }

   void UpdateHandler::DoUpdate(std::chrono::steady_clock::time_point now)
   {
      SCOPED_TIMER("UpdateHandler::DoUpdate");

      // Type-safe time conversion using chrono duration cast
      auto delta = now - m_lastUpdate;
      float deltaSeconds = std::chrono::duration<float>(delta).count();

      // Lock for callback invocation (thread safety)
      {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_updateCallback) {
        m_updateCallback(deltaSeconds);
      }
      }

      // Update last update time
      m_lastUpdate = now;
   }
}
