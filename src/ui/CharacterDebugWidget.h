#pragma once

// Only compile in debug mode
#ifdef _DEBUG

#include "../state/StateManager.h"
#include "../state/StateTypes.h"  // For HealthTrackingState
#include "ImGuiCommon.h"

namespace Huginn::UI
{
   /**
    * @brief Debug window: the player's own state (v0.6.1; 0.24.40 split off the
    * world around them into EnvironmentDebugWidget)
    *
    * PlayerActorState: vitals and their tracking, debuffs, buffs, resistances,
    * equipment, survival, transformation. Shared colours and helpers:
    * StateDebugCommon.h.
    *
    * USAGE:
    *   - Toggle visibility with CharacterDebugWidget::GetSingleton().ToggleVisible()
    *   - Auto-updates each frame when visible
    *   - Position persists across frames (draggable)
    */
   class CharacterDebugWidget
   {
   public:
      static CharacterDebugWidget& GetSingleton() noexcept;

      // Disable copy/move
      CharacterDebugWidget(const CharacterDebugWidget&) = delete;
      CharacterDebugWidget(CharacterDebugWidget&&) = delete;
      CharacterDebugWidget& operator=(const CharacterDebugWidget&) = delete;
      CharacterDebugWidget& operator=(CharacterDebugWidget&&) = delete;

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
      CharacterDebugWidget() = default;
      ~CharacterDebugWidget() = default;

      // Section renderers (matches ContextSensorDebugWidget structure)
      void DrawVitalsSection(const State::ActorVitals& vitals);
      void DrawEffectsSection(const State::ActorEffects& effects);
      void DrawActiveBuffsSection(const State::ActorBuffs& buffs);
      void DrawResistancesSection(const State::ActorResistances& resistances);  // v0.6.6
      void DrawEquipmentSection(const State::PlayerActorState& state);
      void DrawSurvivalSection(const State::PlayerActorState& state);
      void DrawTransformationSection(const State::PlayerActorState& state);  // v0.6.6
      void DrawHealthTrackingSection(const State::HealthTrackingState& state);
      void DrawStaminaTrackingSection(const State::StaminaTrackingState& state);   // v0.6.9
      void DrawMagickaTrackingSection(const State::MagickaTrackingState& state);   // v0.6.9

      bool m_isVisible = false;  // OFF until DebugSettings says otherwise.
      // Must match DebugDefaults::CHARACTER_VISIBLE. It did not: the
      // default here was true while the setting default was false, so the
      // widget rendered over the main menu and the whole loading sequence
      // and only went away at kPostLoadGame, when ApplyToWidgets() finally
      // ran -- or never, for a player with no dMenu INI to load.
      float m_posX = Config::CHARACTER_DEBUG_POS_X;
      float m_posY = Config::CHARACTER_DEBUG_POS_Y;
   };
}

#endif  // _DEBUG
