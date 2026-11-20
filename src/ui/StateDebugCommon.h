#pragma once

// Only compile in debug mode
#ifdef _DEBUG

// =============================================================================
// THE STATE DEBUG WINDOWS' SHARED PARTS (0.24.40)
// =============================================================================
// The colours, the enum names and the drawing helpers of the two windows the
// old StateManager window was split into: CharacterDebugWidget (the player)
// and EnvironmentDebugWidget (the world around them).
// =============================================================================

#include "../state/StateManager.h"
#include "../state/StateConstants.h"
#include "ImGuiCommon.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace Huginn::UI::StateDebug
{
   // =============================================================================
   // COLOR CONSTANTS (ImGui colors)
   // =============================================================================

   namespace Colors
   {
      // UI colors
      constexpr ImVec4 HEADER_YELLOW{0.8f, 0.8f, 0.2f, 1.0f};
      constexpr ImVec4 ACTIVE_INDICATOR{1.0f, 0.8f, 0.2f, 1.0f};
      constexpr ImVec4 INACTIVE_GRAY{0.5f, 0.5f, 0.5f, 1.0f};

      // Health colors
      constexpr ImVec4 HEALTH_CRITICAL{1.0f, 0.2f, 0.2f, 1.0f};     // Red
      constexpr ImVec4 HEALTH_LOW{1.0f, 0.7f, 0.2f, 1.0f};          // Orange
      constexpr ImVec4 HEALTH_VERY_LOW{0.8f, 0.2f, 0.2f, 1.0f};     // Dark red
      constexpr ImVec4 HEALTH_DEBUFF{1.0f, 0.5f, 0.2f, 0.6f};       // Orange (debuffed portion)

      // Magicka colors
      constexpr ImVec4 MAGICKA_LOW{0.2f, 0.5f, 1.0f, 1.0f};         // Bright blue
      constexpr ImVec4 MAGICKA_NORMAL{0.2f, 0.4f, 0.8f, 1.0f};      // Dark blue
      constexpr ImVec4 MAGICKA_DEBUFF{0.5f, 0.4f, 0.8f, 0.6f};      // Purple (debuffed portion)

      // Stamina colors
      constexpr ImVec4 STAMINA_LOW{0.2f, 1.0f, 0.2f, 1.0f};         // Bright green
      constexpr ImVec4 STAMINA_NORMAL{0.2f, 0.8f, 0.2f, 1.0f};      // Dark green
      constexpr ImVec4 STAMINA_DEBUFF{0.6f, 0.8f, 0.2f, 0.6f};      // Yellow-green (debuffed portion)

      // Effect colors
      constexpr ImVec4 EFFECT_FIRE{1.0f, 0.5f, 0.1f, 1.0f};         // Orange
      constexpr ImVec4 EFFECT_POISON{0.5f, 1.0f, 0.2f, 1.0f};       // Green
      constexpr ImVec4 EFFECT_FROST{0.5f, 0.8f, 1.0f, 1.0f};        // Light blue
      constexpr ImVec4 EFFECT_SHOCK{0.8f, 0.8f, 1.0f, 1.0f};        // Purple-blue
      constexpr ImVec4 EFFECT_DISEASE{0.7f, 0.4f, 0.2f, 1.0f};      // Brown

      // Drain colors
      constexpr ImVec4 DRAIN_HEALTH{1.0f, 0.3f, 0.3f, 1.0f};        // Red
      constexpr ImVec4 DRAIN_MAGICKA{0.3f, 0.5f, 1.0f, 1.0f};       // Blue
      constexpr ImVec4 DRAIN_STAMINA{0.3f, 1.0f, 0.3f, 1.0f};       // Green

      // Buff/Debuff colors (unified color scheme)
      constexpr ImVec4 BUFF_COLOR{0.3f, 0.6f, 1.0f, 1.0f};          // Blue (all buffs)
      constexpr ImVec4 DEBUFF_COLOR{1.0f, 0.9f, 0.2f, 1.0f};        // Yellow (all debuffs)

      // Legacy buff colors (kept for backward compatibility if needed)
      constexpr ImVec4 BUFF_ARMOR{0.8f, 0.6f, 0.2f, 1.0f};          // Gold
      constexpr ImVec4 BUFF_CLOAK{0.8f, 0.4f, 1.0f, 1.0f};          // Purple
      constexpr ImVec4 BUFF_INVISIBILITY{0.5f, 0.5f, 0.8f, 1.0f};   // Light blue-gray
      constexpr ImVec4 BUFF_SUMMON{0.8f, 0.2f, 0.8f, 1.0f};         // Magenta

      // Weapon charge colors
      constexpr ImVec4 CHARGE_LOW{1.0f, 0.3f, 0.3f, 1.0f};          // Red
      constexpr ImVec4 CHARGE_NORMAL{0.5f, 0.8f, 1.0f, 1.0f};       // Blue

      // Survival hunger colors (red spectrum)
      constexpr ImVec4 HUNGER_RAVENOUS{1.0f, 0.2f, 0.2f, 1.0f};
      constexpr ImVec4 HUNGER_VERY_HUNGRY{1.0f, 0.5f, 0.2f, 1.0f};
      constexpr ImVec4 HUNGER_HUNGRY{1.0f, 0.8f, 0.2f, 1.0f};
      constexpr ImVec4 HUNGER_PECKISH{0.8f, 0.8f, 0.5f, 1.0f};
      constexpr ImVec4 HUNGER_SLIGHTLY_HUNGRY{0.6f, 0.6f, 0.5f, 1.0f};

      // Survival cold colors (blue spectrum)
      constexpr ImVec4 COLD_FREEZING{0.5f, 0.8f, 1.0f, 1.0f};
      constexpr ImVec4 COLD_VERY_COLD{0.6f, 0.8f, 1.0f, 1.0f};
      constexpr ImVec4 COLD_COLD{0.7f, 0.8f, 1.0f, 1.0f};
      constexpr ImVec4 COLD_CHILLY{0.8f, 0.9f, 1.0f, 1.0f};

      // Survival fatigue colors (purple spectrum)
      constexpr ImVec4 FATIGUE_EXHAUSTED{0.6f, 0.4f, 0.8f, 1.0f};
      constexpr ImVec4 FATIGUE_VERY_TIRED{0.6f, 0.5f, 0.7f, 1.0f};
      constexpr ImVec4 FATIGUE_TIRED{0.7f, 0.6f, 0.7f, 1.0f};
      constexpr ImVec4 FATIGUE_WEARY{0.7f, 0.7f, 0.7f, 1.0f};
      constexpr ImVec4 FATIGUE_SLIGHTLY_TIRED{0.8f, 0.8f, 0.8f, 1.0f};

      // Position state colors
      constexpr ImVec4 UNDERWATER{0.3f, 0.5f, 1.0f, 1.0f};
      constexpr ImVec4 SWIMMING{0.4f, 0.7f, 1.0f, 1.0f};
      constexpr ImVec4 FALLING{1.0f, 0.8f, 0.2f, 1.0f};
      constexpr ImVec4 OVERENCUMBERED{1.0f, 0.5f, 0.2f, 1.0f};
      constexpr ImVec4 MOUNTED{0.7f, 0.5f, 0.3f, 1.0f};

      // Target state colors
      constexpr ImVec4 CASTING{0.8f, 0.5f, 1.0f, 1.0f};

      // Indicator colors
      constexpr ImVec4 INACTIVE_INDICATOR{0.4f, 0.4f, 0.4f, 1.0f};
   }

   // =============================================================================
   // LAYOUT CONSTANTS (v0.6.7 space efficiency)
   // =============================================================================

   namespace Layout
   {
      constexpr float COLUMN_WIDTH = 130.0f;      // Width for 2-column grid
      constexpr float INDICATOR_SPACING = 12.0f;  // Spacing for horizontal rows
   }

   // =============================================================================
   // ENUM-TO-STRING MAPPING HELPERS
   // =============================================================================

   constexpr const char* GetLockLevelName(int lockLevel) noexcept
   {
      switch (lockLevel) {
         case State::LockLevel::NOVICE: return "Novice";
         case State::LockLevel::APPRENTICE: return "Apprentice";
         case State::LockLevel::ADEPT: return "Adept";
         case State::LockLevel::EXPERT: return "Expert";
         case State::LockLevel::MASTER: return "Master";
         case State::LockLevel::REQUIRES_KEY: return "Requires Key";
         default: return "Unknown";
      }
   }

   constexpr const char* GetCloakTypeName(State::EffectType cloakType) noexcept
   {
      switch (cloakType) {
         case State::EffectType::CloakFire: return "Fire";
         case State::EffectType::CloakFrost: return "Frost";
         case State::EffectType::CloakShock: return "Shock";
         default: return "Unknown";
      }
   }

   constexpr const char* GetHungerLevelName(int level) noexcept
   {
      // Names from CC Survival Mode (UESP)
      switch (level) {
         case 5: return "Starving";
         case 4: return "Famished";
         case 3: return "Hungry";
         case 2: return "Peckish";
         case 1: return "Satisfied";
         default: return "Well Fed";
      }
   }

   constexpr const char* GetColdLevelName(int level) noexcept
   {
      switch (level) {
         case 5: return "Numb";
         case 4: return "Freezing";
         case 3: return "Very Cold";
         case 2: return "Chilly";
         case 1: return "Comfortable";
         default: return "Warm";
      }
   }

   // Names taken from SurvivalThreshold's own constants, which this table used
   // to contradict: ranks 2 and 3 were swapped, so FATIGUE_TIRED printed
   // "Weary" and FATIGUE_WEARY -- the Weary rank itself -- printed
   // "Tired". FATIGUE_DEBILITATED, the critical state, printed "Very Tired",
   // and case 5 was dead code because the vanilla path tops out at 4.
   //
   // That swap is what made the widget look like it was speaking some other
   // mod's vocabulary. It was speaking its own, one row out.
   //
   // Negative ranks are the well-rested bonuses (FATIGUE_LOVERS_COMFORT = -3),
   // which the default arm still covers.
   constexpr const char* GetFatigueLevelName(int level) noexcept
   {
      switch (level) {
         case 4: return "Debilitated";     // FATIGUE_DEBILITATED - critical
         case 3: return "Weary";           // FATIGUE_WEARY
         case 2: return "Tired";           // FATIGUE_TIRED
         case 1: return "Slightly Tired";  // FATIGUE_SLIGHTLY_TIRED
         case 0: return "Refreshed";       // FATIGUE_REFRESHED
         default: return "Well Rested";    // negative: bonus states
      }
   }

   constexpr const char* GetWorkstationTypeName(uint8_t type) noexcept
   {
      // Values from RE::TESFurniture::WorkBenchData::BenchType
      switch (type) {
         case 1: return "Forge";
         case 2: return "Smithing";
         case 3: return "Enchanting";
         case 4: return "Enchant Exp";
         case 5: return "Alchemy";
         case 6: return "Alchemy Exp";
         case 7: return "Tanning";
         case 8: return "Smelter";
         case 9: return "Cooking";
         default: return "Unknown";
      }
   }

   // =============================================================================
   // HELPER METHODS
   // =============================================================================

   inline void DrawIndicator(const char* label, bool active, ImVec4 activeColor = ImVec4(0.2f, 1.0f, 0.2f, 1.0f))
   {
      ImVec4 color = active ? activeColor : Colors::INACTIVE_INDICATOR;
      ImGui::TextColored(color, "[%c] %s", active ? 'X' : ' ', label);
   }

   inline void DrawProgressBar(const char* label, float value, ImVec4 color, const char* overlay = nullptr)
   {
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);

      if (overlay) {
         ImGui::ProgressBar(value, ImVec2(150.0f, 12.0f), overlay);
      } else {
         char buf[32];
         snprintf(buf, sizeof(buf), "%.0f%%", value * 100.0f);
         ImGui::ProgressBar(value, ImVec2(150.0f, 12.0f), buf);
      }

      ImGui::PopStyleColor();

      ImGui::SameLine(0.0f, 10.0f);
      ImGui::Text("%s", label);
   }

   inline void DrawVitalBarWithDebuff(const char* label, float current, float effectiveMax, float baseMax,
                                      ImVec4 normalColor, ImVec4 debuffColor)
   {
      // Bar dimensions
      constexpr float BAR_WIDTH = 150.0f;
      constexpr float BAR_HEIGHT = 12.0f;

      // Calculate proportions
      const float effectiveRatio = (baseMax > 0.0f) ? (effectiveMax / baseMax) : 1.0f;
      const float currentRatio = (effectiveMax > 0.0f) ? std::clamp(current / effectiveMax, 0.0f, 1.0f) : 0.0f;
      const float hasDebuff = (baseMax - effectiveMax) > 1.0f;

      // Get draw position
      ImVec2 pos = ImGui::GetCursorScreenPos();
      ImDrawList* drawList = ImGui::GetWindowDrawList();

      // Background (dark gray)
      drawList->AddRectFilled(
         pos,
         ImVec2(pos.x + BAR_WIDTH, pos.y + BAR_HEIGHT),
         IM_COL32(40, 40, 40, 255),
         2.0f  // Rounding
      );

      // Draw debuff portion (effectiveMax to baseMax) - right side of bar
      if (hasDebuff) {
         const float debuffStart = effectiveRatio * BAR_WIDTH;
         drawList->AddRectFilled(
        ImVec2(pos.x + debuffStart, pos.y),
        ImVec2(pos.x + BAR_WIDTH, pos.y + BAR_HEIGHT),
        ImGui::ColorConvertFloat4ToU32(debuffColor),
        2.0f
         );
      }

      // Draw filled portion (0 to current) within the effective range
      const float filledWidth = currentRatio * effectiveRatio * BAR_WIDTH;
      if (filledWidth > 0.0f) {
         drawList->AddRectFilled(
        pos,
        ImVec2(pos.x + filledWidth, pos.y + BAR_HEIGHT),
        ImGui::ColorConvertFloat4ToU32(normalColor),
        2.0f
         );
      }

      // Draw border
      drawList->AddRect(
         pos,
         ImVec2(pos.x + BAR_WIDTH, pos.y + BAR_HEIGHT),
         IM_COL32(80, 80, 80, 255),
         2.0f
      );

      // Draw separator line at effectiveMax boundary if debuffed
      if (hasDebuff) {
         const float separatorX = pos.x + effectiveRatio * BAR_WIDTH;
         drawList->AddLine(
        ImVec2(separatorX, pos.y),
        ImVec2(separatorX, pos.y + BAR_HEIGHT),
        IM_COL32(255, 255, 255, 180),
        1.0f
         );
      }

      // Draw overlay text (centered)
      char overlay[48];
      if (hasDebuff) {
         snprintf(overlay, sizeof(overlay), "%.0f/%.0f (%.0f)", current, effectiveMax, baseMax);
      } else {
         snprintf(overlay, sizeof(overlay), "%.0f/%.0f", current, effectiveMax);
      }

      ImVec2 textSize = ImGui::CalcTextSize(overlay);
      ImVec2 textPos = ImVec2(
         pos.x + (BAR_WIDTH - textSize.x) * 0.5f,
         pos.y + (BAR_HEIGHT - textSize.y) * 0.5f
      );

      // Text shadow for readability
      drawList->AddText(ImVec2(textPos.x + 1, textPos.y + 1), IM_COL32(0, 0, 0, 200), overlay);
      drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), overlay);

      // Advance cursor past the bar
      ImGui::Dummy(ImVec2(BAR_WIDTH, BAR_HEIGHT));

      // Label
      ImGui::SameLine(0.0f, 10.0f);
      ImGui::Text("%s", label);
   }

   // =============================================================================
   // COMPACT LAYOUT HELPERS (v0.6.7)
   // =============================================================================

   inline void DrawIndicatorPair(
      const char* label1, bool active1, ImVec4 activeColor1,
      const char* label2, bool active2, ImVec4 activeColor2,
      float columnWidth = 100.0f)
   {
      ImVec4 color1 = active1 ? activeColor1 : Colors::INACTIVE_INDICATOR;
      ImGui::TextColored(color1, "[%c] %s", active1 ? 'X' : ' ', label1);

      ImGui::SameLine(columnWidth);

      if (label2 && label2[0] != '\0') {
         ImVec4 color2 = active2 ? activeColor2 : Colors::INACTIVE_INDICATOR;
         ImGui::TextColored(color2, "[%c] %s", active2 ? 'X' : ' ', label2);
      }
   }

   inline void DrawIndicatorRow3(
      const char* label1, bool active1, ImVec4 activeColor1,
      const char* label2, bool active2, ImVec4 activeColor2,
      const char* label3, bool active3, ImVec4 activeColor3,
      float spacing = 12.0f)
   {
      ImVec4 color1 = active1 ? activeColor1 : Colors::INACTIVE_INDICATOR;
      ImGui::TextColored(color1, "[%c] %s", active1 ? 'X' : ' ', label1);

      ImGui::SameLine(0.0f, spacing);
      ImVec4 color2 = active2 ? activeColor2 : Colors::INACTIVE_INDICATOR;
      ImGui::TextColored(color2, "[%c] %s", active2 ? 'X' : ' ', label2);

      ImGui::SameLine(0.0f, spacing);
      ImVec4 color3 = active3 ? activeColor3 : Colors::INACTIVE_INDICATOR;
      ImGui::TextColored(color3, "[%c] %s", active3 ? 'X' : ' ', label3);
   }
}

#endif  // _DEBUG
