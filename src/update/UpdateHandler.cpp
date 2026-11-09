#include "UpdateHandler.h"
#include "../util/ScopedTimer.h"
#include "../input/InputHandler.h"
#include "../Profiling.h"
#include "../learning/PlayerInputGate.h"

#include <format>

namespace Huginn::Update
{
   namespace
   {
      /// An event the player made: a button (down, held or up), the mouse
      /// moving, a stick pushed past the deadzone the game's own controls
      /// apply to it (0.23.55: a drifting stick at rest is not input), a typed
      /// character. Not a device connecting.
      bool IsPlayerInput(RE::InputEvent* event, float leftDeadzone, float rightDeadzone)
      {
         switch (event->GetEventType()) {
         case RE::INPUT_EVENT_TYPE::kButton:
         case RE::INPUT_EVENT_TYPE::kChar:
            return true;
         case RE::INPUT_EVENT_TYPE::kMouseMove: {
            const auto* move = static_cast<RE::MouseMoveEvent*>(event);
            return move->mouseInputX != 0 || move->mouseInputY != 0;
         }
         case RE::INPUT_EVENT_TYPE::kThumbstick: {
            const auto* stick = static_cast<RE::ThumbstickEvent*>(event);
            return Core::Needs::StickIsInput(stick->xValue, stick->yValue,
               stick->IsRight() ? rightDeadzone : leftDeadzone);
         }
         default:
            return false;
         }
      }

      bool IsVanillaHotkey(const RE::BSFixedString& userEvent)
      {
         const auto* names = RE::UserEvents::GetSingleton();
         if (!names || userEvent.empty()) return false;
         return userEvent == names->hotkey1 || userEvent == names->hotkey2 ||
                userEvent == names->hotkey3 || userEvent == names->hotkey4 ||
                userEvent == names->hotkey5 || userEvent == names->hotkey6 ||
                userEvent == names->hotkey7 || userEvent == names->hotkey8;
      }
   }

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

      // 0.23.51: when the player last touched anything (SecondsSinceInput),
      // whether or not updates are enabled.
      if (a_event) {
         const float leftDeadzone = m_leftStickDeadzone.load(std::memory_order_relaxed);
         const float rightDeadzone = m_rightStickDeadzone.load(std::memory_order_relaxed);
         for (auto* event = *a_event; event; event = event->next) {
            if (IsPlayerInput(event, leftDeadzone, rightDeadzone)) {
               m_lastInputTicks.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                  std::memory_order_relaxed);
               break;
            }
         }
      }

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
        // A vanilla favourites hotkey the player pressed: the equip that
        // follows is theirs (PlayerInputGate). Read BEFORE ProcessButton,
        // which clears userEvent on a key Huginn swallows -- and a swallowed
        // key never reaches the game, so it is skipped below.
        const bool vanillaHotkey = button->IsDown() && IsVanillaHotkey(button->QUserEvent());

        // Process button and consume event if handled
        if (inputHandler.ProcessButton(button)) {
           // Clear userEvent to mark as consumed - prevents game from processing
           auto* idEvent = event->AsIDEvent();
           if (idEvent) {
            idEvent->userEvent = "";
            logger::trace("[UpdateHandler] Consumed input event (code=0x{:02X})"sv,
              button->GetIDCode());
           }
        } else if (vanillaHotkey) {
           Learning::PlayerInputGate::GetSingleton().NoteVanillaHotkey();
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

   namespace
   {
      /// One float INI setting as the engine holds it (SkyrimPrefs.ini's
      /// collection first, then Skyrim.ini's, as the game resolves a name);
      /// false when it is not there or not a float.
      bool ReadFloatIniSetting(const char* name, float& out)
      {
         auto* setting = RE::GetINISetting(name);
         if (!setting || setting->GetType() != RE::Setting::Type::kFloat) return false;
         out = setting->GetFloat();
         return true;
      }
   }

   void UpdateHandler::SyncStickDeadzones(std::string_view why)
   {
      // The deadzone the game's controls apply to each stick: the left stick
      // moves (fMovementAxisDeadzone), the right one turns the view
      // (fHeadingAxisDeadzone). The gamepad device's own pair
      // (fLThumbDeadzone, fRThumbDeadzone) is logged beside them for
      // reference only: see LapHoldInputs.h and the schema doc, Laps.
      std::lock_guard lock(m_deadzoneMutex);
      float move = 0.0f, look = 0.0f, devL = -1.0f, devR = -1.0f;
      const bool moveRead = ReadFloatIniSetting("fMovementAxisDeadzone:Controls", move);
      const bool lookRead = ReadFloatIniSetting("fHeadingAxisDeadzone:Controls", look);
      ReadFloatIniSetting("fLThumbDeadzone:Controls", devL);
      ReadFloatIniSetting("fRThumbDeadzone:Controls", devR);
      const float left = Core::Needs::SaneStickDeadzone(moveRead, move);
      const float right = Core::Needs::SaneStickDeadzone(lookRead, look);
      m_leftStickDeadzone.store(left, std::memory_order_relaxed);
      m_rightStickDeadzone.store(right, std::memory_order_relaxed);
      m_lastDeadzoneSync = std::chrono::steady_clock::now();
      m_deadzonesSynced = true;

      // Log transitions, not ticks: only when a value (or whether it could be
      // read) changed since the last sync.
      const std::array<float, 4> seen{ moveRead ? move : -1.0f, lookRead ? look : -1.0f, devL, devR };
      if (seen == m_lastDeadzonesSeen) return;
      m_lastDeadzonesSeen = seen;
      auto describe = [](bool read, float raw, float used) {
         return read && raw == used ? std::format("{:.3f}", used)
              : read                ? std::format("{:.3f} (read {:.3f}, out of range)", used, raw)
                                    : std::format("{:.3f} (not found; default)", used);
      };
      logger::info("[UpdateHandler] stick input deadzones ({}): left {} (fMovementAxisDeadzone), right {} "
                   "(fHeadingAxisDeadzone); device fLThumbDeadzone {:.3f}, fRThumbDeadzone {:.3f} (reference)"sv,
         why, describe(moveRead, move, left), describe(lookRead, look, right), devL, devR);
   }

   double UpdateHandler::SecondsSinceInput(std::chrono::steady_clock::time_point now) const noexcept
   {
      const auto ticks = m_lastInputTicks.load(std::memory_order_relaxed);
      if (ticks == kNoInput) return std::numeric_limits<double>::infinity();
      const std::chrono::steady_clock::time_point last{ std::chrono::steady_clock::duration{ ticks } };
      return std::max(0.0, std::chrono::duration<double>(now - last).count());
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

   void UpdateHandler::DoUpdate(std::chrono::steady_clock::time_point now)
   {
      SCOPED_TIMER("UpdateHandler::DoUpdate");

      // 0.23.55: re-read the stick deadzones now and then -- a console
      // setini, or a mod or MCM that writes the setting, changes them
      // mid-session. Every 60 s: a change takes effect within one lap, and a
      // read is four name lookups in the engine's setting lists (well under a
      // millisecond). Only after the first sync (kDataLoaded): the settings
      // are not ours to read before the game has loaded them.
      bool resync = false;
      {
         std::lock_guard lock(m_deadzoneMutex);
         resync = m_deadzonesSynced && now - m_lastDeadzoneSync >= kDeadzoneResync;
      }
      if (resync) SyncStickDeadzones("periodic");

      // Type-safe time conversion using chrono duration cast
      auto delta = now - m_lastUpdate;
      float deltaSeconds = std::chrono::duration<float>(delta).count();

      // Clamp after alt-tab or long pauses. Normal interval is ~0.1s;
      // anything over 1s means the game was suspended. Large deltas would
      // cause timer expiry bursts and could confuse integration-based logic.
      static constexpr float MAX_DELTA_SECONDS = 1.0f;
      if (deltaSeconds > MAX_DELTA_SECONDS) {
         logger::warn("[UpdateHandler] Clamped deltaSeconds {:.1f}s → {:.1f}s (alt-tab or pause)"sv,
            deltaSeconds, MAX_DELTA_SECONDS);
         deltaSeconds = MAX_DELTA_SECONDS;
      }

      // Lock for callback invocation and the m_lastUpdate write (thread safety).
      {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_updateCallback) {
        m_updateCallback(deltaSeconds);
      }
      m_lastUpdate = now;
      }
   }
}
