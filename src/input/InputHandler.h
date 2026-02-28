#pragma once

#include "../Config.h"
// Note: std headers (functional, array, chrono) come from PCH via RE/Skyrim.h

namespace Huginn::Input
{
   /**
    * @brief Hand to equip spell to
    */
   enum class EquipHand
   {
      Right,  // Single tap
      Left,   // Double tap
      Both    // Hold
   };

   /**
    * @brief Callback when a slot key is pressed
    * @param slotIndex 0-based slot index (0, 1, 2 for slots 1, 2, 3)
    * @param hand Which hand to equip to
    */
   using SlotCallback = std::function<void(size_t slotIndex, EquipHand hand)>;

   /**
    * @brief Callback when a cycle key is pressed
    * @param isPrevious true for cycle previous, false for cycle next
    * @param isHold true if key was held (reload/flush action)
    */
   using CycleCallback = std::function<void(bool isPrevious, bool isHold)>;

   /**
    * @brief Input handler for widget keybindings
    *
    * Detects key presses for:
    * - [1-0] Equip Slots 1-10 (tap=right, double-tap=left, hold=both)
    * - [-] Cycle Previous (tap) / Reload (hold)
    * - [=] Cycle Next (tap) / Flush (hold)
    *
    * Key detection modes:
    * - Tap: Press and release within threshold
    * - Double-tap: Two taps within double-tap window
    * - Hold: Press and hold beyond threshold
    *
    * TODO: This should be a fallback when Wheeler is not available.
    *       When Wheeler is installed, use Wheeler's wheel interface instead.
    *       Only enable keyboard input if WheelerClient::IsConnected() == false.
    */
   class InputHandler
   {
   public:
      static InputHandler& GetSingleton();

      /**
       * @brief Process a button event from the input system
       * @param button The button event to process
       * @return true if the event was consumed (handled by this handler)
       */
      bool ProcessButton(RE::ButtonEvent* button);

      /**
       * @brief Set callback for slot key presses
       * @param callback Function to call when a slot key is pressed
       */
      void SetSlotCallback(SlotCallback callback) { m_slotCallback = std::move(callback); }

      /**
       * @brief Set callback for cycle key presses
       * @param callback Function to call when a cycle key is pressed
       */
      void SetCycleCallback(CycleCallback callback) { m_cycleCallback = std::move(callback); }

      /**
       * @brief Enable or disable input handling
       * @param enabled true to process keys, false to ignore
       */
      void SetEnabled(bool enabled) { m_enabled = enabled; }

      /**
       * @brief Check if input handling is enabled
       * @return true if enabled
       */
      bool IsEnabled() const { return m_enabled; }

      /**
       * @brief Set hold threshold for equip keys
       * @param seconds Time in seconds to trigger hold action
       */
      void SetHoldThreshold(float seconds) { m_holdThreshold = seconds; }

      /**
       * @brief Set double-tap window
       * @param seconds Maximum time between taps for double-tap
       */
      void SetDoubleTapWindow(float seconds) { m_doubleTapWindow = seconds; }

      /**
       * @brief Set the key codes for each action
       * Defaults: 1-0 = slots 1-10, - = prev, = = next (DirectInput scancodes)
       */
      void SetKeyCodes(uint32_t slot1, uint32_t slot2, uint32_t slot3, uint32_t slot4,
             uint32_t slot5, uint32_t slot6, uint32_t slot7, uint32_t slot8,
             uint32_t slot9, uint32_t slot10, uint32_t cyclePrev, uint32_t cycleNext);

      /**
       * @brief Called each frame to handle deferred double-tap detection
       * Must be called from the update loop
       */
      void Update();

   private:
      InputHandler();
      ~InputHandler() = default;
      InputHandler(const InputHandler&) = delete;
      InputHandler& operator=(const InputHandler&) = delete;

      /// Process equip key (1, 2, or 3)
      void HandleEquipKey(int slotIndex, RE::ButtonEvent* button);

      /// Process cycle key (4 or 5)
      void HandleCycleKey(int index, RE::ButtonEvent* button);

      /// Callbacks
      SlotCallback m_slotCallback;
      CycleCallback m_cycleCallback;

      /// Enabled state
      bool m_enabled = true;

      /// Key codes for each action
      /// Default: 1-0 = slots (2-11), - = prev (12), = = next (13)
      std::array<uint32_t, 12> m_keyCodes;

      /// Timing thresholds
      float m_holdThreshold = 0.4f;      // Seconds to trigger hold
      float m_doubleTapWindow = 0.3f;    // Seconds for double-tap detection
      float m_cycleHoldThreshold = 1.0f; // Seconds for cycle hold (reload/flush)

      /// State tracking for cycle keys
      struct CycleKeyState
      {
      std::chrono::steady_clock::time_point pressTime;
      bool isHeld = false;
      bool holdTriggered = false;
      };
      std::array<CycleKeyState, 2> m_cycleState;

      /// State tracking for equip keys
      struct EquipKeyState
      {
      std::chrono::steady_clock::time_point pressTime;
      std::chrono::steady_clock::time_point lastTapTime;
      bool isPressed = false;
      bool holdTriggered = false;
      bool waitingForDoubleTap = false;
      bool pendingTapAction = false;  // Deferred tap waiting to fire
      };
      std::array<EquipKeyState, 10> m_equipState;

      /// Current time helper (for consistent timing within single frame)
      std::chrono::steady_clock::time_point m_frameTime;
   };
}
