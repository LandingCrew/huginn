// Only compile in debug mode
#ifdef _DEBUG

#include "EnvironmentDebugWidget.h"
#include "DebugSettings.h"
#include "StateDebugCommon.h"
#include "../state/DropAheadProbe.h"
#include "../state/StateConstants.h"
#include <format>

namespace Huginn::UI
{
   using namespace StateDebug;

   EnvironmentDebugWidget& EnvironmentDebugWidget::GetSingleton() noexcept
   {
      static EnvironmentDebugWidget instance;
      return instance;
   }

   void EnvironmentDebugWidget::Draw()
   {
      if (!m_isVisible) {
         return;
      }

      auto& stateManager = State::StateManager::GetSingleton();

      // Top-right, left of the Character window's usual place (pivot: top-right corner).
      const ImGuiIO& io = ImGui::GetIO();
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kRightOffset, kPosY), ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
      ImGui::SetNextWindowBgAlpha(0.85f);
      ImGui::SetNextWindowCollapsed(false, ImGuiCond_FirstUseEver);

      const ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));

      static const std::string windowTitle = std::format("Environment [v{} DEBUG]", Huginn::VERSION_STRING);

      if (ImGui::Begin(windowTitle.c_str(), &m_isVisible, flags)) {
         auto worldState = stateManager.GetWorldState();
         auto playerState = stateManager.GetPlayerState();
         auto targets = stateManager.GetTargets();

         static const std::string environmentHeader = std::format("Environment | Poll: {:.0f}ms", State::PollInterval::WORLD_MS);
         if (ImGui::CollapsingHeader(environmentHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            DrawEnvironmentSection(worldState, playerState);
         }

         static const std::string crosshairHeader = std::format("Crosshair | Poll: {:.0f}ms", State::PollInterval::WORLD_MS);
         if (ImGui::CollapsingHeader(crosshairHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            DrawCrosshairSection(worldState, targets);
         }

         static const std::string combatHeader = std::format("Combat Awareness | Poll: {:.0f}ms", State::PollInterval::TARGETS_MS);
         if (ImGui::CollapsingHeader(combatHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            DrawCombatAwarenessSection(targets);
         }

         if (ImGui::CollapsingHeader("Movement Sensors | drop / edge probe, 100ms", ImGuiTreeNodeFlags_DefaultOpen)) {
            DrawMovementSensorsSection();
         }
      }
      ImGui::End();

      ImGui::PopStyleVar(3);
   }

   // =============================================================================
   // MOVEMENT SENSORS SECTION (0.24.40)
   // =============================================================================
   // The drop / edge probe's last reading (DropAheadProbe: drop_ahead,
   // deep_water_ahead, time_to_edge, edge_approach), and the switch for its
   // on-screen rays (ProbeOverlay).
   // =============================================================================

   void EnvironmentDebugWidget::DrawMovementSensorsSection()
   {
      ImGui::Indent(8.0f);

      const auto r = State::DropAheadProbe::Latest();
      const bool measured = r.status == State::DropAheadProbe::Status::Measured;
      const auto status = State::DropAheadProbe::StatusName(r.status);
      ImGui::Text("Probe:");
      ImGui::SameLine();
      ImGui::TextColored(measured ? Colors::ACTIVE_INDICATOR : Colors::INACTIVE_GRAY, "%.*s",
         static_cast<int>(status.size()), status.data());

      if (r.drop >= 0.0f) {
         ImGui::Text("Drop ahead: %.0f", r.drop);
      } else {
         ImGui::TextDisabled("Drop ahead: not measured");
      }
      ImGui::SameLine(Layout::COLUMN_WIDTH + 20.0f);
      if (r.waterDepth > 0.0f) {
         ImGui::TextColored(Colors::DRAIN_MAGICKA, "Water ahead: %.0f deep", r.waterDepth);
      } else if (r.waterDepth == 0.0f) {
         ImGui::Text("Water ahead: none");
      } else {
         ImGui::TextDisabled("Water ahead: -");
      }

      if (r.edgeDistance >= 0.0f) {
         ImGui::TextColored(Colors::HEALTH_CRITICAL, "Edge: %.0f ahead", r.edgeDistance);
      } else {
         ImGui::TextDisabled("Edge: none known");
      }
      ImGui::SameLine(Layout::COLUMN_WIDTH + 20.0f);
      ImGui::Text("Speed to it: %.0f/game s", r.edgeSpeed);
      ImGui::Text("Proximity: %.2f", r.edgeProximity);
      ImGui::SameLine(Layout::COLUMN_WIDTH + 20.0f);
      ImGui::Text("Approach: %+.2f/game s", r.edgeApproach);

      ImGui::Separator();
      bool draw = State::DropAheadProbe::DebugDrawOn();
      if (ImGui::Checkbox("Draw probe rays", &draw)) {
         // this session; [Debug] bDrawEnvProbe sets it at load and on a reload
         DebugSettings::GetSingleton().drawEnvProbe = draw;
         State::DropAheadProbe::SetDebugDraw(draw);
      }
      ImGui::TextColored(ImVec4(1.0f, 0.24f, 0.24f, 1.0f), "fall");
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.24f, 0.59f, 1.0f, 1.0f), "water");
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.31f, 0.86f, 0.31f, 1.0f), "fine");
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.94f, 0.78f, 0.16f, 1.0f), "blocked");
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.59f, 0.59f, 0.59f, 1.0f), "skipped / unreached");

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // ENVIRONMENT SECTION (v0.6.9 - Compact 2-column layout)
   // =============================================================================
   // Combines WorldState (time/light/interior) + PlayerActorState (position flags)
   // =============================================================================

   void EnvironmentDebugWidget::DrawEnvironmentSection(const State::WorldState& world, const State::PlayerActorState& player)
   {
      ImGui::Indent(8.0f);

      // Time and lighting - compact single line
      ImGui::Text("Time: %.0f:00 (%s)", world.timeOfDay,
         world.IsNightTime() ? "Night" : "Day");
      ImGui::SameLine(Layout::COLUMN_WIDTH);
      ImGui::Text("Light: %.2f (%s)", world.lightLevel,
         world.IsDark() ? "Dark" : world.IsWellLit() ? "Bright" : "Mod");

      ImGui::Text("Location: %s", world.isInterior ? "Interior" : "Exterior");

      ImGui::Separator();

      // Position states - 2-column layout
      DrawIndicatorPair("Underwater", player.isUnderwater, Colors::DRAIN_MAGICKA,
                        "Swimming", player.isSwimming, Colors::SWIMMING, Layout::COLUMN_WIDTH);
      DrawIndicatorPair("Falling", player.isFalling, Colors::ACTIVE_INDICATOR,
                        "Encumbered", player.isOverencumbered, Colors::HUNGER_VERY_HUNGRY, Layout::COLUMN_WIDTH);
      DrawIndicatorPair("Sneaking", player.isSneaking, Colors::BUFF_INVISIBILITY,
                        "In Combat", player.isInCombat, Colors::HEALTH_CRITICAL, Layout::COLUMN_WIDTH);
      DrawIndicatorPair("Mounted", player.isMounted, Colors::MOUNTED,
                        "On Dragon", player.isMountedOnDragon, Colors::BUFF_SUMMON, Layout::COLUMN_WIDTH);

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // CROSSHAIR SECTION (matches ContextSensorDebugWidget)
   // =============================================================================
   // Combines WorldState (locks/ore/workstations) + TargetCollection (primary target)
   // =============================================================================

   void EnvironmentDebugWidget::DrawCrosshairSection(const State::WorldState& world, const State::TargetCollection& targets)
   {
      ImGui::Indent(8.0f);

      // World objects - horizontal layout (v0.6.7)
      if (world.isLookingAtLock) {
         ImGui::TextColored(Colors::ACTIVE_INDICATOR, "[X] Lock (%s)", GetLockLevelName(world.lockLevel));
      } else {
         ImGui::TextColored(Colors::INACTIVE_INDICATOR, "[ ] Lock");
      }
      ImGui::SameLine(0.0f, Layout::INDICATOR_SPACING);
      DrawIndicator("Ore", world.isLookingAtOreVein, Colors::ACTIVE_INDICATOR);
      ImGui::SameLine(0.0f, Layout::INDICATOR_SPACING);
      if (world.isLookingAtWorkstation) {
         ImGui::TextColored(Colors::ACTIVE_INDICATOR, "[X] %s", GetWorkstationTypeName(world.workstationType));
      } else {
         ImGui::TextColored(Colors::INACTIVE_INDICATOR, "[ ] Workstation");
      }

      ImGui::Separator();

      // Primary target NPC (from TargetCollection)
      // Always show section (static layout prevents flickering when target appears/disappears)
      ImGui::TextColored(targets.primary ? Colors::ACTIVE_INDICATOR : Colors::INACTIVE_GRAY, "NPC Target:");
      ImGui::Indent(8.0f);

      if (targets.primary) {
         const auto& target = *targets.primary;

         ImGui::Text("FormID: 0x%08X", target.actorFormID);
         ImGui::Text("Distance: %.0f units", target.GetDistanceToPlayer());
         ImGui::Text("Type: %s", State::GetTargetTypeName(target.targetType));

         // v0.6.12: IFF (Identification Friend or Foe) display
         {
        const char* iffLabel = target.isHostile ? "Hostile" : (target.isFollower ? "Follower" : "Allied");
        ImVec4 iffColor = target.isHostile ? Colors::DRAIN_HEALTH :
                          (target.isFollower ? Colors::BUFF_COLOR : Colors::ACTIVE_INDICATOR);
        ImGui::Text("IFF:");
        ImGui::SameLine();
        ImGui::TextColored(iffColor, "%s", iffLabel);
         }
         ImGui::SameLine();
         DrawIndicator("Dead", target.isDead, Colors::INACTIVE_GRAY);
         ImGui::SameLine();
         DrawIndicator("Casting", target.isCasting, Colors::CASTING);

         DrawIndicator("Staggered", target.isStaggered, Colors::ACTIVE_INDICATOR);

         // v0.6.11: Dragon/Mage indicators
         const bool isDragon = (target.targetType == State::TargetType::Dragon);
         DrawIndicatorPair("Dragon", isDragon, Colors::EFFECT_FIRE,
                           "Mage", target.isMage, Colors::CASTING, Layout::COLUMN_WIDTH);

         // v0.6.11: Vitals freshness indicator (shows if secondary target vitals are stale)
         // Primary targets always have fresh vitals; secondary targets may have cached vitals
         const bool isPrimary = (target.source == State::TargetSource::Crosshair ||
                                 target.source == State::TargetSource::CombatPrimary);
         if (!isPrimary) {
        ImGui::TextDisabled("(Vitals: cached @ 500ms)");
         }

         // Target vitals bar
         ImVec4 healthColor = target.IsTargetHealthCritical() ?
        Colors::HEALTH_CRITICAL :
        Colors::HEALTH_VERY_LOW;
         DrawProgressBar("Target HP", target.vitals.health, healthColor);
      } else {
         ImGui::TextDisabled("None");
      }

      ImGui::Unindent(8.0f);

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // COMBAT AWARENESS SECTION (matches ContextSensorDebugWidget)
   // =============================================================================
   // Derived from TargetCollection aggregate queries
   // =============================================================================

   void EnvironmentDebugWidget::DrawCombatAwarenessSection(const State::TargetCollection& targets)
   {
      ImGui::Indent(8.0f);

      // Enemy count
      const int enemyCount = targets.GetEnemyCount();
      ImGui::Text("Enemy Count: %d", enemyCount);

      // Closest enemy
      if (enemyCount > 0) {
         const auto closestEnemy = targets.GetClosestEnemy();
         if (closestEnemy) {
        const float dist = closestEnemy->GetDistanceToPlayer();
        ImGui::Text("Nearest Enemy: %.1f units", dist);
         }

         // Casting enemies
         const int castingCount = targets.CountCastingEnemies();
         DrawIndicator("Enemy Casting", castingCount > 0, Colors::EFFECT_SHOCK);
      } else {
         ImGui::TextDisabled("No nearby enemies");
      }

      ImGui::Separator();

      // Combat warnings
      DrawIndicator("Outnumbered", targets.IsOutnumbered(), Colors::HEALTH_CRITICAL);
      DrawIndicator("Enemy in Melee Range", targets.CountEnemiesInMeleeRange() > 0, Colors::HEALTH_LOW);

      ImGui::Separator();

      // Follower tracking (v0.6.10, enhanced v0.6.12)
      ImGui::TextColored(Colors::HEADER_YELLOW, "Followers:");
      int followerCount = 0;
      for (const auto& target : targets.targets) {
         if (target.isFollower && !target.isDead) {
        ++followerCount;
        // Show HP bar color based on health level
        ImVec4 hpColor = Colors::STAMINA_NORMAL;  // Green for healthy
        if (target.vitals.health < 0.25f) {
           hpColor = Colors::HEALTH_CRITICAL;  // Red for critical
        } else if (target.vitals.health < 0.50f) {
           hpColor = Colors::HEALTH_LOW;  // Orange for low
        } else if (target.vitals.health < 0.75f) {
           hpColor = Colors::ACTIVE_INDICATOR;  // Yellow for medium
        }
        ImGui::TextColored(hpColor, "  %08X: %.0f%% HP (%.0f units)",
                           target.actorFormID, target.vitals.health * 100.0f, target.GetDistanceToPlayer());
         }
      }
      if (followerCount == 0) {
         ImGui::TextDisabled("  No followers");
      }

      // v0.6.12: Debug - show all tracked targets
      ImGui::Separator();
      ImGui::Text("All Tracked (%zu):", targets.targets.size());
      for (const auto& target : targets.targets) {
         const char* srcName = "?";
         switch (target.source) {
        case State::TargetSource::Crosshair: srcName = "XHR"; break;
        case State::TargetSource::CombatPrimary: srcName = "CMB"; break;
        case State::TargetSource::NearbyEnemy: srcName = "ENM"; break;
        case State::TargetSource::NearbyAlly: srcName = "ALY"; break;
        default: break;
         }
         const char* iff = target.isHostile ? "H" : (target.isFollower ? "F" : "A");
         ImGui::Text("  %08X %s %s %.0f", target.actorFormID, srcName, iff, target.GetDistanceToPlayer());
      }

      ImGui::Unindent(8.0f);
   }

}

#endif  // _DEBUG
