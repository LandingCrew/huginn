#include "InputHandler.h"

namespace Huginn::Input
{
   InputHandler& InputHandler::GetSingleton()
   {
      static InputHandler singleton;
      return singleton;
   }

   InputHandler::InputHandler()
   {
      // Default key codes (DirectInput scancodes)
      // 1=2, 2=3, 3=4, 4=5, 5=6, 6=7, 7=8, 8=9, 9=10, 0=11, -=12, ==13
      // Layout: [slot1-10, cyclePrev, cycleNext]
      // Keys 1-0 = equip slots 1-10, - = prev page, = = next page
      m_keyCodes = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };

      // Initialize state
      m_frameTime = std::chrono::steady_clock::now();
   }

   void InputHandler::SetKeyCodes(uint32_t slot1, uint32_t slot2, uint32_t slot3, uint32_t slot4,
                   uint32_t slot5, uint32_t slot6, uint32_t slot7, uint32_t slot8,
                   uint32_t slot9, uint32_t slot10, uint32_t cyclePrev, uint32_t cycleNext)
   {
      m_keyCodes[0] = slot1;
      m_keyCodes[1] = slot2;
      m_keyCodes[2] = slot3;
      m_keyCodes[3] = slot4;
      m_keyCodes[4] = slot5;
      m_keyCodes[5] = slot6;
      m_keyCodes[6] = slot7;
      m_keyCodes[7] = slot8;
      m_keyCodes[8] = slot9;
      m_keyCodes[9] = slot10;
      m_keyCodes[10] = cyclePrev;
      m_keyCodes[11] = cycleNext;

      logger::debug("[InputHandler] Key codes set: slots={},{},{},{},{},{},{},{},{},{} cycle={},{}"sv,
      slot1, slot2, slot3, slot4, slot5, slot6, slot7, slot8, slot9, slot10, cyclePrev, cycleNext);
   }

   bool InputHandler::ProcessButton(RE::ButtonEvent* button)
   {
      if (!m_enabled || !button) {
      return false;
      }

      // Only process keyboard events - ignore mouse, gamepad, etc.
      if (button->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
      return false;
      }

      // Log config once on first call
      static bool loggedConfig = false;
      if (!loggedConfig) {
      logger::info("[InputHandler] Key codes: [1-10]={},{},{},{},{},{},{},{},{},{} [-=]={},{}"sv,
        m_keyCodes[0], m_keyCodes[1], m_keyCodes[2], m_keyCodes[3], m_keyCodes[4],
        m_keyCodes[5], m_keyCodes[6], m_keyCodes[7], m_keyCodes[8], m_keyCodes[9],
        m_keyCodes[10], m_keyCodes[11]);
      logger::info("[InputHandler] Thresholds: hold={:.2f}s doubleTap={:.2f}s cycleHold={:.2f}s"sv,
        m_holdThreshold, m_doubleTapWindow, m_cycleHoldThreshold);
      loggedConfig = true;
      }

      // Update frame time
      m_frameTime = std::chrono::steady_clock::now();

      uint32_t keyCode = button->GetIDCode();
      uint32_t device = static_cast<uint32_t>(button->GetDevice());

      // Check if it's one of our keys
      // Layout: [slot1-10, cyclePrev, cycleNext]
      for (size_t i = 0; i < m_keyCodes.size(); ++i) {
      if (keyCode == m_keyCodes[i]) {
        if (button->IsDown()) {
           logger::debug("[InputHandler] KEY PRESS: code={} (0x{:02X}) action={}"sv,
            keyCode, keyCode, i);
        }
        if (i < 10) {
           // Equip keys (index 0-9 -> slot 0-9)
           HandleEquipKey(static_cast<int>(i), button);
        } else {
           // Cycle keys (index 10-11 -> 0=prev, 1=next)
           HandleCycleKey(static_cast<int>(i - 10), button);
        }
        return true;  // Event consumed
      }
      }

      return false;  // Not our key
   }

   void InputHandler::HandleCycleKey(int index, RE::ButtonEvent* button)
   {
      auto& state = m_cycleState[index];

      if (button->IsDown()) {
      state.pressTime = m_frameTime;
      state.isHeld = true;
      state.holdTriggered = false;
      }
      else if (button->IsHeld()) {
      if (state.isHeld && !state.holdTriggered) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
           m_frameTime - state.pressTime);
        float elapsedSec = elapsed.count() / 1000.0f;

        if (elapsedSec >= m_cycleHoldThreshold) {
           // Hold triggered - reload or flush
           state.holdTriggered = true;
           logger::debug("[InputHandler] Cycle key {} held (reload/flush)"sv, index + 1);

           if (m_cycleCallback) {
            m_cycleCallback(index == 0, true);  // isHold = true
           }
        }
      }
      }
      else if (button->IsUp()) {
      if (state.isHeld && !state.holdTriggered) {
        // Normal tap - cycle prev/next
        logger::debug("[InputHandler] Cycle key {} tapped"sv, index + 1);

        if (m_cycleCallback) {
           m_cycleCallback(index == 0, false);  // isHold = false
        }
      }
      state.isHeld = false;
      }
   }

   void InputHandler::HandleEquipKey(int slotIndex, RE::ButtonEvent* button)
   {
      auto& state = m_equipState[slotIndex];

      if (button->IsDown()) {
      logger::debug("[InputHandler] Slot {} KEY DOWN"sv, slotIndex + 1);
      state.pressTime = m_frameTime;
      state.isPressed = true;
      state.holdTriggered = false;
      }
      else if (button->IsHeld()) {
      if (state.isPressed && !state.holdTriggered) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
           m_frameTime - state.pressTime);
        float elapsedSec = elapsed.count() / 1000.0f;

        logger::trace("[InputHandler] Slot {} HELD elapsed={:.3f}s threshold={:.3f}s"sv,
           slotIndex + 1, elapsedSec, m_holdThreshold);

        if (elapsedSec >= m_holdThreshold) {
           // Hold triggered - equip both hands
           state.holdTriggered = true;
           state.waitingForDoubleTap = false;
           state.pendingTapAction = false;  // Cancel any pending tap
           logger::info("[InputHandler] Slot {} HOLD TRIGGERED -> equip both hands"sv, slotIndex + 1);

           if (m_slotCallback) {
            m_slotCallback(slotIndex, EquipHand::Both);
           }
        }
      }
      }
      else if (button->IsUp()) {
      logger::debug("[InputHandler] Slot {} KEY UP isPressed={} holdTriggered={}"sv,
        slotIndex + 1, state.isPressed, state.holdTriggered);

      if (state.isPressed && !state.holdTriggered) {
        // Key was released before hold threshold
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
           m_frameTime - state.lastTapTime);
        float sinceLastTap = elapsed.count() / 1000.0f;

        logger::debug("[InputHandler] Slot {} UP: waitingForDoubleTap={} pendingTap={} sinceLastTap={:.3f}s window={:.3f}s"sv,
           slotIndex + 1, state.waitingForDoubleTap, state.pendingTapAction, sinceLastTap, m_doubleTapWindow);

        if (state.pendingTapAction && sinceLastTap < m_doubleTapWindow) {
           // Double tap detected - equip left hand ONLY
           state.waitingForDoubleTap = false;
           state.pendingTapAction = false;  // Cancel pending right-hand
           logger::info("[InputHandler] Slot {} DOUBLE-TAP -> equip left hand ONLY"sv, slotIndex + 1);

           if (m_slotCallback) {
            m_slotCallback(slotIndex, EquipHand::Left);
           }
        } else {
           // First tap - defer action, wait for potential double tap
           state.lastTapTime = m_frameTime;
           state.waitingForDoubleTap = true;
           state.pendingTapAction = true;  // Will fire in Update() if no double-tap
           logger::debug("[InputHandler] Slot {} TAP pending (waiting {:.0f}ms for double-tap)"sv,
            slotIndex + 1, m_doubleTapWindow * 1000.0f);
           // DO NOT call callback here - wait for timeout in Update()
        }
      }
      state.isPressed = false;
      }
   }

   void InputHandler::Update()
   {
      // Update frame time
      m_frameTime = std::chrono::steady_clock::now();

      // Check for pending tap actions that need to fire
      for (size_t i = 0; i < m_equipState.size(); ++i) {
      auto& state = m_equipState[i];

      if (state.pendingTapAction) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
           m_frameTime - state.lastTapTime);
        float elapsedSec = elapsed.count() / 1000.0f;

        if (elapsedSec > m_doubleTapWindow) {
           // Double-tap window expired - fire the single tap action
           state.pendingTapAction = false;
           state.waitingForDoubleTap = false;
           logger::info("[InputHandler] Slot {} TAP (deferred) -> equip right hand"sv, i + 1);

           if (m_slotCallback) {
            m_slotCallback(i, EquipHand::Right);
           }
        }
      } else if (state.waitingForDoubleTap) {
        // Clear stale waiting state (no pending action)
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
           m_frameTime - state.lastTapTime);
        float elapsedSec = elapsed.count() / 1000.0f;

        if (elapsedSec > m_doubleTapWindow) {
           state.waitingForDoubleTap = false;
        }
      }
      }
   }
}
