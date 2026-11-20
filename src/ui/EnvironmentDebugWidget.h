#pragma once

// Only compile in debug mode
#ifdef _DEBUG

#include "../state/StateManager.h"
#include "ImGuiCommon.h"

namespace Huginn::UI
{
   /**
    * @brief Debug window: the world around the player (0.24.40, split from the
    * old StateManager window; the player's own state is CharacterDebugWidget)
    *
    * WorldState (time, light, location) and the player's position flags; the
    * crosshair (locks, ore, workstations, the NPC target); combat awareness
    * (TargetCollection); the movement sensors (the drop / edge probe, and the
    * switch for its on-screen rays, ProbeOverlay). Shared colours and helpers:
    * StateDebugCommon.h.
    */
   class EnvironmentDebugWidget
   {
   public:
      static EnvironmentDebugWidget& GetSingleton() noexcept;

      EnvironmentDebugWidget(const EnvironmentDebugWidget&) = delete;
      EnvironmentDebugWidget(EnvironmentDebugWidget&&) = delete;
      EnvironmentDebugWidget& operator=(const EnvironmentDebugWidget&) = delete;
      EnvironmentDebugWidget& operator=(EnvironmentDebugWidget&&) = delete;

      /// Draw the window (called each frame; nothing while hidden).
      void Draw();

      void SetVisible(bool visible) { m_isVisible = visible; }
      [[nodiscard]] bool IsVisible() const { return m_isVisible; }
      void ToggleVisible() { m_isVisible = !m_isVisible; }

   private:
      EnvironmentDebugWidget() = default;
      ~EnvironmentDebugWidget() = default;

      void DrawEnvironmentSection(const State::WorldState& world, const State::PlayerActorState& player);
      void DrawCrosshairSection(const State::WorldState& world, const State::TargetCollection& targets);
      void DrawCombatAwarenessSection(const State::TargetCollection& targets);
      void DrawMovementSensorsSection();

      // First placed left of the Character window, which sits at the right edge.
      static constexpr float kRightOffset = 420.0f;
      static constexpr float kPosY = 10.0f;

      bool m_isVisible = false;  // OFF until DebugSettings says otherwise (DebugDefaults::ENVIRONMENT_VISIBLE)
   };
}

#endif  // _DEBUG
