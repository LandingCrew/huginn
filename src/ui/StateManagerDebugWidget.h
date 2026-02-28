#pragma once

// Only compile in debug mode
#ifdef _DEBUG

#include "../state/StateManager.h"
#include "../state/StateTypes.h"  // For HealthTrackingState
#include <imgui.h>

namespace Huginn::UI
{
   /**
    * @brief Debug widget to display StateManager output (v0.6.1)
    *
    * Shows all three state models in a collapsible ImGui window:
    *   - WorldState (time, lighting, locks, ore veins, workstations)
    *   - PlayerActorState (vitals, effects, buffs, equipment, survival, position)
    *   - TargetCollection (primary target + tracked targets with aggregate queries)
    *
    * USAGE:
    *   - Toggle visibility with StateManagerDebugWidget::GetSingleton().ToggleVisible()
    *   - Auto-updates each frame when visible
    *   - Position persists across frames (draggable)
    */
   class StateManagerDebugWidget
   {
   public:
      static StateManagerDebugWidget& GetSingleton() noexcept;

      // Disable copy/move
      StateManagerDebugWidget(const StateManagerDebugWidget&) = delete;
      StateManagerDebugWidget(StateManagerDebugWidget&&) = delete;
      StateManagerDebugWidget& operator=(const StateManagerDebugWidget&) = delete;
      StateManagerDebugWidget& operator=(StateManagerDebugWidget&&) = delete;

      /**
       * @brief Draw the debug widget (called each frame)
       */
      void Draw();

      /**
       * @brief Show or hide the widget
       */
      void SetVisible(bool visible) { m_isVisible = visible; }

      /**
       * @brief Check if widget is visible
       */
      [[nodiscard]] bool IsVisible() const { return m_isVisible; }

      /**
       * @brief Toggle visibility
       */
      void ToggleVisible() { m_isVisible = !m_isVisible; }

   private:
      StateManagerDebugWidget() = default;
      ~StateManagerDebugWidget() = default;

      // Section renderers (matches ContextSensorDebugWidget structure)
      void DrawVitalsSection(const State::ActorVitals& vitals);
      void DrawEffectsSection(const State::ActorEffects& effects);
      void DrawActiveBuffsSection(const State::ActorBuffs& buffs);
      void DrawResistancesSection(const State::ActorResistances& resistances);  // v0.6.6
      void DrawEnvironmentSection(const State::WorldState& world, const State::PlayerActorState& player);
      void DrawCrosshairSection(const State::WorldState& world, const State::TargetCollection& targets);
      void DrawEquipmentSection(const State::PlayerActorState& state);
      void DrawSurvivalSection(const State::PlayerActorState& state);
      void DrawTransformationSection(const State::PlayerActorState& state);  // v0.6.6
      void DrawCombatAwarenessSection(const State::TargetCollection& targets);
      void DrawHealthTrackingSection(const State::HealthTrackingState& state);
      void DrawStaminaTrackingSection(const State::StaminaTrackingState& state);   // v0.6.9
      void DrawMagickaTrackingSection(const State::MagickaTrackingState& state);   // v0.6.9

      // Helpers
      void DrawIndicator(const char* label, bool active, ImVec4 activeColor = ImVec4(0.2f, 1.0f, 0.2f, 1.0f)) const;
      void DrawProgressBar(const char* label, float value, ImVec4 color, const char* overlay = nullptr) const;
      void DrawVitalBarWithDebuff(const char* label, float current, float effectiveMax, float baseMax,
                                  ImVec4 normalColor, ImVec4 debuffColor) const;

      // Compact layout helpers (v0.6.7)
      void DrawIndicatorPair(const char* label1, bool active1, ImVec4 activeColor1,
                             const char* label2, bool active2, ImVec4 activeColor2,
                             float columnWidth = 100.0f) const;
      void DrawIndicatorRow3(const char* label1, bool active1, ImVec4 activeColor1,
                             const char* label2, bool active2, ImVec4 activeColor2,
                             const char* label3, bool active3, ImVec4 activeColor3,
                             float spacing = 12.0f) const;

      bool m_isVisible = true;  // Visible by default in Debug builds
      float m_posX = Config::STATE_MANAGER_DEBUG_POS_X;
      float m_posY = Config::STATE_MANAGER_DEBUG_POS_Y;
   };
}

#endif  // _DEBUG
