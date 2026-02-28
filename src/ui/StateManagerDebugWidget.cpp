// Only compile in debug mode
#ifdef _DEBUG

#include "StateManagerDebugWidget.h"
#include "../state/StateConstants.h"          // For threshold constants
#include "../state/StateTypes.h"              // For HealthTrackingState
#include <format>
// Note: Huginn::VERSION_STRING is available via PCH.h -> Plugin.h

namespace Huginn::UI
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

   constexpr const char* GetFatigueLevelName(int level) noexcept
   {
      switch (level) {
         case 5: return "Exhausted";
         case 4: return "Very Tired";
         case 3: return "Tired";
         case 2: return "Weary";
         case 1: return "Slightly Tired";
         default: return "Rested";
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

   StateManagerDebugWidget& StateManagerDebugWidget::GetSingleton() noexcept
   {
      static StateManagerDebugWidget instance;
      return instance;
   }

   void StateManagerDebugWidget::Draw()
   {
      if (!m_isVisible) {
         return;
      }

      auto& stateManager = State::StateManager::GetSingleton();

      // Right-align the window (pivot 1.0 = anchor from right edge)
      const ImGuiIO& io = ImGui::GetIO();
      const float margin = 10.0f;
      ImGui::SetNextWindowPos(
         ImVec2(io.DisplaySize.x - margin, m_posY),
         ImGuiCond_FirstUseEver,
         ImVec2(1.0f, 0.0f)  // Pivot: top-right corner
      );
      ImGui::SetNextWindowBgAlpha(0.85f);
      ImGui::SetNextWindowCollapsed(false, ImGuiCond_FirstUseEver);

      ImGuiWindowFlags flags =
         ImGuiWindowFlags_NoSavedSettings |
         ImGuiWindowFlags_AlwaysAutoResize;

      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));

      // Build window title with current version (static to avoid per-frame allocation)
      static const std::string windowTitle = std::format("StateManager [v{} DEBUG]", Huginn::VERSION_STRING);

      if (ImGui::Begin(windowTitle.c_str(), &m_isVisible, flags)) {
         // Save position when moved
         ImVec2 windowPos = ImGui::GetWindowPos();
         m_posX = windowPos.x;
         m_posY = windowPos.y;

         // Get current states (thread-safe copies)
         auto worldState = stateManager.GetWorldState();
         auto playerState = stateManager.GetPlayerState();
         auto targets = stateManager.GetTargets();

         // =============================================================================
         // SECTIONS (matches ContextSensorDebugWidget structure)
         // FIX (v0.7.18): Use static const headers to avoid 13 std::format allocations per frame
         // =============================================================================

         // Vitals (HP/MP/SP)
         static const std::string vitalsHeader = std::format("Vitals (HP/MP/SP) | Poll: {:.0f}ms", State::PollInterval::PLAYER_VITALS_MS);
         if (ImGui::CollapsingHeader(vitalsHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawVitalsSection(playerState.vitals);
         }
         // Health Tracking (v0.6.2, v0.6.9 - Renamed)
         static const std::string healthTrackingHeader = std::format("Health Tracking | Poll: {:.0f}ms", State::PollInterval::PLAYER_HEALTH_TRACKING_MS);
         if (ImGui::CollapsingHeader(healthTrackingHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        auto healthTracking = stateManager.GetHealthTracking();
        DrawHealthTrackingSection(healthTracking);
         }

         // Stamina Tracking (v0.6.9)
         static const std::string staminaTrackingHeader = std::format("Stamina Tracking | Poll: {:.0f}ms", State::PollInterval::PLAYER_STAMINA_TRACKING_MS);
         if (ImGui::CollapsingHeader(staminaTrackingHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        auto staminaTracking = stateManager.GetStaminaTracking();
        DrawStaminaTrackingSection(staminaTracking);
         }

         // Magicka Tracking (v0.6.9)
         static const std::string magickaTrackingHeader = std::format("Magicka Tracking | Poll: {:.0f}ms", State::PollInterval::PLAYER_MAGICKA_TRACKING_MS);
         if (ImGui::CollapsingHeader(magickaTrackingHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        auto magickaTracking = stateManager.GetMagickaTracking();
        DrawMagickaTrackingSection(magickaTracking);
         }
         
         // Debuffs (Damage DOTs)
         static const std::string debuffsHeader = std::format("Debuffs | Poll: {:.0f}ms", State::PollInterval::PLAYER_MAGIC_EFFECTS_MS);
         if (ImGui::CollapsingHeader(debuffsHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawEffectsSection(playerState.effects);
         }

         // Buffs
         static const std::string buffsHeader = std::format("Buffs | Poll: {:.0f}ms", State::PollInterval::PLAYER_MAGIC_EFFECTS_MS);
         if (ImGui::CollapsingHeader(buffsHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawActiveBuffsSection(playerState.buffs);
         }

         // Resistances (v0.6.6)
         static const std::string resistancesHeader = std::format("Resistances | Poll: {:.0f}ms", State::PollInterval::PLAYER_RESISTANCES_MS);
         if (ImGui::CollapsingHeader(resistancesHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawResistancesSection(playerState.resistances);
         }

         // Environment (time, light, position state)
         static const std::string environmentHeader = std::format("Environment | Poll: {:.0f}ms", State::PollInterval::WORLD_MS);
         if (ImGui::CollapsingHeader(environmentHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawEnvironmentSection(worldState, playerState);
         }

         // Crosshair (locks, ore, primary target)
         static const std::string crosshairHeader = std::format("Crosshair | Poll: {:.0f}ms", State::PollInterval::WORLD_MS);
         if (ImGui::CollapsingHeader(crosshairHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawCrosshairSection(worldState, targets);
         }

         // Equipment
         static const std::string equipmentHeader = std::format("Equipment | Poll: {:.0f}ms", State::PollInterval::PLAYER_EQUIPMENT_MS);
         if (ImGui::CollapsingHeader(equipmentHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawEquipmentSection(playerState);
         }

         // Survival
         static const std::string survivalHeader = std::format("Survival | Poll: {:.0f}ms", State::PollInterval::PLAYER_SURVIVAL_MS);
         if (ImGui::CollapsingHeader(survivalHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawSurvivalSection(playerState);
         }

         // Transformation (v0.6.6 - vampire/werewolf state)
         static const std::string transformationHeader = std::format("Transformation | Poll: {:.0f}ms", State::PollInterval::PLAYER_RESISTANCES_MS);
         if (ImGui::CollapsingHeader(transformationHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawTransformationSection(playerState);
         }

         // Combat Awareness (derived from target tracking)
         static const std::string combatHeader = std::format("Combat Awareness | Poll: {:.0f}ms", State::PollInterval::TARGETS_MS);
         if (ImGui::CollapsingHeader(combatHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawCombatAwarenessSection(targets);
         }
      }
      ImGui::End();

      ImGui::PopStyleVar(3);
   }

   // =============================================================================
   // VITALS SECTION (matches ContextSensorDebugWidget)
   // =============================================================================

   void StateManagerDebugWidget::DrawVitalsSection(const State::ActorVitals& vitals)
   {
      ImGui::Indent(8.0f);

      // Calculate current values from percentage and effective max
      const float currentHealth = vitals.health * vitals.maxHealth;
      const float currentMagicka = vitals.magicka * vitals.maxMagicka;
      const float currentStamina = vitals.stamina * vitals.maxStamina;

      // Health bar - choose color based on severity
      ImVec4 healthColor;
      if (vitals.IsHealthCritical()) {
         healthColor = Colors::HEALTH_CRITICAL;  // Bright red
      } else if (vitals.IsHealthLow()) {
         healthColor = Colors::HEALTH_LOW;  // Orange
      } else {
         healthColor = Colors::HEALTH_VERY_LOW;  // Dark red
      }
      DrawVitalBarWithDebuff("Health", currentHealth, vitals.maxHealth, vitals.baseMaxHealth,
                             healthColor, Colors::HEALTH_DEBUFF);

      // Magicka bar
      ImVec4 magickaColor = vitals.IsMagickaLow() ?
         Colors::MAGICKA_LOW :     // Bright blue (low)
         Colors::MAGICKA_NORMAL;   // Dark blue
      DrawVitalBarWithDebuff("Magicka", currentMagicka, vitals.maxMagicka, vitals.baseMaxMagicka,
                             magickaColor, Colors::MAGICKA_DEBUFF);

      // Stamina bar
      ImVec4 staminaColor = vitals.IsStaminaLow() ?
         Colors::STAMINA_LOW :     // Bright green (low)
         Colors::STAMINA_NORMAL;   // Dark green
      DrawVitalBarWithDebuff("Stamina", currentStamina, vitals.maxStamina, vitals.baseMaxStamina,
                             staminaColor, Colors::STAMINA_DEBUFF);

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // DEBUFFS SECTION (matches ContextSensorDebugWidget)
   // =============================================================================

   void StateManagerDebugWidget::DrawEffectsSection(const State::ActorEffects& effects)
   {
      ImGui::Indent(8.0f);

      // Damage over time effects - 2-column layout (v0.6.7)
      DrawIndicatorPair("On Fire", effects.isOnFire, Colors::DEBUFF_COLOR,
                        "Poisoned", effects.isPoisoned, Colors::DEBUFF_COLOR, Layout::COLUMN_WIDTH);
      DrawIndicatorPair("Frozen", effects.isFrozen, Colors::DEBUFF_COLOR,
                        "Shocked", effects.isShocked, Colors::DEBUFF_COLOR, Layout::COLUMN_WIDTH);
      DrawIndicator("Diseased", effects.isDiseased, Colors::DEBUFF_COLOR);

      ImGui::Separator();

      // Vital drains - horizontal row (v0.6.7)
      DrawIndicatorRow3("HP Drain", effects.hasHealthDrain, Colors::DEBUFF_COLOR,
                        "MP Drain", effects.hasMagickaPoison, Colors::DEBUFF_COLOR,
                        "SP Drain", effects.hasStaminaPoison, Colors::DEBUFF_COLOR, Layout::INDICATOR_SPACING);

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // BUFFS SECTION (matches ContextSensorDebugWidget)
   // =============================================================================

   void StateManagerDebugWidget::DrawActiveBuffsSection(const State::ActorBuffs& buffs)
   {
      ImGui::Indent(8.0f);

      // Row 1: Armor buff | Cloak - 2-column layout (v0.6.7)
      DrawIndicator("Armor Buff", buffs.hasArmorBuff, Colors::BUFF_COLOR);
      ImGui::SameLine(Layout::COLUMN_WIDTH);
      if (buffs.hasCloakActive) {
         ImGui::TextColored(Colors::BUFF_COLOR, "[X] Cloak: %s", GetCloakTypeName(buffs.activeCloakType));
      } else {
         ImGui::TextColored(Colors::INACTIVE_INDICATOR, "[ ] Cloak");
      }

      // Row 2: Invisibility | Summon
      DrawIndicatorPair("Invisible", buffs.isInvisible, Colors::BUFF_COLOR,
                        "Summon", buffs.hasActiveSummon, Colors::BUFF_COLOR, Layout::COLUMN_WIDTH);

      // Row 3: Muffle | Waterbreath
      DrawIndicatorPair("Muffle", buffs.hasMuffle, Colors::BUFF_COLOR,
                        "Waterbreath", buffs.hasWaterBreathing, Colors::BUFF_COLOR, Layout::COLUMN_WIDTH);

      ImGui::Separator();

      // Regen buffs/debuffs - compact 2-row layout (v0.6.7)
      ImGui::TextColored(Colors::HEADER_YELLOW, "Regen:");
      ImGui::SameLine();
      DrawIndicatorRow3("HP+", buffs.hasHealthRegenBuff, Colors::BUFF_COLOR,
                        "MP+", buffs.hasMagickaRegenBuff, Colors::BUFF_COLOR,
                        "SP+", buffs.hasStaminaRegenBuff, Colors::BUFF_COLOR, Layout::INDICATOR_SPACING);
      ImGui::TextColored(Colors::INACTIVE_INDICATOR, "      ");  // Alignment spacer
      ImGui::SameLine();
      DrawIndicatorRow3("HP-", buffs.hasHealthRegenDebuff, Colors::DEBUFF_COLOR,
                        "MP-", buffs.hasMagickaRegenDebuff, Colors::DEBUFF_COLOR,
                        "SP-", buffs.hasStaminaRegenDebuff, Colors::DEBUFF_COLOR, Layout::INDICATOR_SPACING);

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // RESISTANCES SECTION (v0.6.6, v0.6.9 - Compact 2-column layout)
   // =============================================================================

   void StateManagerDebugWidget::DrawResistancesSection(const State::ActorResistances& resistances)
   {
      ImGui::Indent(8.0f);

      // Row 1: Fire | Frost | Shock
      ImVec4 fireColor = resistances.HasHighFireResist() ?
         (resistances.IsFireResistCapped() ? Colors::EFFECT_FROST : Colors::ACTIVE_INDICATOR) :
         Colors::INACTIVE_GRAY;
      ImGui::TextColored(fireColor, "Fire: %.0f%%", resistances.fire);

      ImGui::SameLine(90.0f);
      ImVec4 frostColor = resistances.HasHighFrostResist() ?
         (resistances.IsFrostResistCapped() ? Colors::EFFECT_FROST : Colors::ACTIVE_INDICATOR) :
         Colors::INACTIVE_GRAY;
      ImGui::TextColored(frostColor, "Frost: %.0f%%", resistances.frost);

      ImGui::SameLine(180.0f);
      ImVec4 shockColor = resistances.HasHighShockResist() ?
         (resistances.IsShockResistCapped() ? Colors::EFFECT_FROST : Colors::ACTIVE_INDICATOR) :
         Colors::INACTIVE_GRAY;
      ImGui::TextColored(shockColor, "Shock: %.0f%%", resistances.shock);

      // Row 2: Poison | Magic
      ImVec4 poisonColor = resistances.HasHighPoisonResist() ?
         Colors::ACTIVE_INDICATOR : Colors::INACTIVE_GRAY;
      ImGui::TextColored(poisonColor, "Poison: %.0f%%", resistances.poison);

      ImGui::SameLine(90.0f);
      ImVec4 magicColor = resistances.HasHighMagicResist() ?
         Colors::ACTIVE_INDICATOR : Colors::INACTIVE_GRAY;
      ImGui::TextColored(magicColor, "Magic: %.0f%%", resistances.magic);

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // EQUIPMENT SECTION (matches ContextSensorDebugWidget)
   // =============================================================================

   void StateManagerDebugWidget::DrawEquipmentSection(const State::PlayerActorState& state)
   {
      ImGui::Indent(8.0f);

      // Weapon type summary - single line (v0.6.7)
      const char* weaponType = "Unarmed";
      if (state.hasBowEquipped) weaponType = "Bow";
      else if (state.hasCrossbowEquipped) weaponType = "Crossbow";
      else if (state.hasMeleeEquipped) weaponType = "Melee";
      else if (state.hasStaffEquipped) weaponType = "Staff";

      ImGui::Text("Weapon: %s", weaponType);
      // Ammo count inline if ranged
      if (state.hasBowEquipped && state.arrowCount > 0) {
         ImGui::SameLine();
         ImGui::TextDisabled("(%d arrows)", state.arrowCount);
      } else if (state.hasCrossbowEquipped && state.boltCount > 0) {
         ImGui::SameLine();
         ImGui::TextDisabled("(%d bolts)", state.boltCount);
      }

      // Equipped FormIDs (show weapon OR spell for each hand)
      ImGui::Text("R-Hand: 0x%08X", state.rightHandWeapon ? state.rightHandWeapon : state.rightHandSpell);
      ImGui::Text("L-Hand: 0x%08X", state.leftHandWeapon ? state.leftHandWeapon : state.leftHandSpell);
      if (state.equippedShield) {
         ImGui::Text("Shield: 0x%08X", state.equippedShield);
      }

      ImGui::Separator();

      // Enchanted weapon charge
      if (state.hasEnchantedWeapon) {
         ImVec4 chargeColor = state.weaponChargePercent < 0.2f ?
        Colors::DRAIN_HEALTH :  // Low charge
        Colors::EFFECT_FROST;   // Normal
         char chargeOverlay[32];
         snprintf(chargeOverlay, sizeof(chargeOverlay), "%.0f%%", state.weaponChargePercent * 100.0f);
         DrawProgressBar("Weapon Charge", state.weaponChargePercent, chargeColor, chargeOverlay);
      } else {
         ImGui::TextDisabled("No enchantment");
      }

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // SURVIVAL SECTION (matches ContextSensorDebugWidget)
   // =============================================================================

   void StateManagerDebugWidget::DrawSurvivalSection(const State::PlayerActorState& state)
   {
      ImGui::Indent(8.0f);

      if (!state.survivalModeActive) {
         ImGui::TextDisabled("Survival Mode: DISABLED");
      } else {
         ImGui::TextColored(Colors::HEADER_YELLOW, "Survival Mode: ACTIVE");

         // Hunger (levels 0-5)
         ImVec4 hungerColor = Colors::INACTIVE_GRAY;
         switch (state.hungerLevel) {
        case 5: hungerColor = Colors::HEALTH_CRITICAL; break;
        case 4: hungerColor = Colors::HUNGER_VERY_HUNGRY; break;
        case 3: hungerColor = Colors::ACTIVE_INDICATOR; break;
        case 2: hungerColor = Colors::HUNGER_PECKISH; break;
        case 1: hungerColor = Colors::HUNGER_SLIGHTLY_HUNGRY; break;
         }
         ImGui::TextColored(hungerColor, "Hunger: %s (lvl %d)", GetHungerLevelName(state.hungerLevel), state.hungerLevel);

         // Cold (levels 0-5)
         ImVec4 coldColor = Colors::INACTIVE_GRAY;
         switch (state.coldLevel) {
        case 5: coldColor = Colors::HEALTH_CRITICAL; break;  // Numb (critical)
        case 4: coldColor = Colors::EFFECT_FROST; break;
        case 3: coldColor = Colors::COLD_VERY_COLD; break;
        case 2: coldColor = Colors::COLD_COLD; break;
        case 1: coldColor = Colors::COLD_CHILLY; break;
         }
         ImGui::TextColored(coldColor, "Cold: %s (lvl %d)", GetColdLevelName(state.coldLevel), state.coldLevel);

         // Fatigue (levels 0-4, max is DEBILITATED=4)
         ImVec4 fatigueColor = Colors::INACTIVE_GRAY;
         switch (state.fatigueLevel) {
        case 4: fatigueColor = Colors::FATIGUE_EXHAUSTED; break;       // Debilitated (most severe)
        case 3: fatigueColor = Colors::FATIGUE_VERY_TIRED; break;      // Weary
        case 2: fatigueColor = Colors::FATIGUE_TIRED; break;           // Tired
        case 1: fatigueColor = Colors::FATIGUE_SLIGHTLY_TIRED; break;  // Slightly Tired
         }
         ImGui::TextColored(fatigueColor, "Fatigue: %s (lvl %d)", GetFatigueLevelName(state.fatigueLevel), state.fatigueLevel);

         // Warmth (v0.6.6)
         // Warmth affects how quickly you get cold - higher = better
         ImVec4 warmthColor = Colors::INACTIVE_GRAY;
         if (state.warmthRating >= 200.0f) {
        warmthColor = Colors::ACTIVE_INDICATOR;  // Well protected
         } else if (state.warmthRating >= 100.0f) {
        warmthColor = Colors::COLD_CHILLY;  // Moderate protection
         } else if (state.warmthRating > 0.0f) {
        warmthColor = Colors::COLD_VERY_COLD;  // Low protection
         }
         ImGui::TextColored(warmthColor, "Warmth: %.0f", state.warmthRating);
      }

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // TRANSFORMATION SECTION (v0.6.6)
   // =============================================================================
   // Displays vampire and werewolf transformation state separately from survival.

   void StateManagerDebugWidget::DrawTransformationSection(const State::PlayerActorState& state)
   {
      ImGui::Indent(8.0f);

      // Vampire state
      if (state.IsVampire()) {
         ImVec4 vampireColor = state.IsSunVulnerable() ?
        Colors::HEALTH_CRITICAL : Colors::DEBUFF_COLOR;
         ImGui::TextColored(vampireColor, "Vampire Stage: %d%s",
        state.vampireStage,
        state.IsSunVulnerable() ? " (Sun Vulnerable!)" : "");
      } else {
         ImGui::TextDisabled("Vampire: No");
      }

      // Werewolf state
      DrawIndicator("Werewolf", state.isWerewolf, Colors::BUFF_ARMOR);
      if (state.isWerewolf) {
         ImGui::Indent(16.0f);
         DrawIndicator("Beast Form", state.isInBeastForm, Colors::HEALTH_CRITICAL);
         ImGui::Unindent(16.0f);
      }

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // ENVIRONMENT SECTION (v0.6.9 - Compact 2-column layout)
   // =============================================================================
   // Combines WorldState (time/light/interior) + PlayerActorState (position flags)
   // =============================================================================

   void StateManagerDebugWidget::DrawEnvironmentSection(const State::WorldState& world, const State::PlayerActorState& player)
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

   void StateManagerDebugWidget::DrawCrosshairSection(const State::WorldState& world, const State::TargetCollection& targets)
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
         ImGui::Text("Level: %d", target.level);
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

   void StateManagerDebugWidget::DrawCombatAwarenessSection(const State::TargetCollection& targets)
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
      for (const auto& [formID, target] : targets.targets) {
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
                           formID, target.vitals.health * 100.0f, target.GetDistanceToPlayer());
         }
      }
      if (followerCount == 0) {
         ImGui::TextDisabled("  No followers");
      }

      // v0.6.12: Debug - show all tracked targets
      ImGui::Separator();
      ImGui::Text("All Tracked (%zu):", targets.targets.size());
      for (const auto& [formID, target] : targets.targets) {
         const char* srcName = "?";
         switch (target.source) {
        case State::TargetSource::Crosshair: srcName = "XHR"; break;
        case State::TargetSource::CombatPrimary: srcName = "CMB"; break;
        case State::TargetSource::NearbyEnemy: srcName = "ENM"; break;
        case State::TargetSource::NearbyAlly: srcName = "ALY"; break;
        default: break;
         }
         const char* iff = target.isHostile ? "H" : (target.isFollower ? "F" : "A");
         ImGui::Text("  %08X %s %s %.0f", formID, srcName, iff, target.GetDistanceToPlayer());
      }

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // HEALTH TRACKING SECTION (v0.6.9 - Renamed from Damage Tracking)
   // =============================================================================

   void StateManagerDebugWidget::DrawHealthTrackingSection(const State::HealthTrackingState& state)
   {
      ImGui::Indent(8.0f);

      // Damage section
      ImGui::TextColored(Colors::HEADER_YELLOW, "Recent Damage:");

      if (state.IsTakingDamage()) {
         ImGui::Text("Damage Taken: %.1f HP (last 2s)", state.recentDamageTaken);
         ImGui::Text("Damage Rate: %.1f HP/s", state.damageRate);

         if (state.takingMagicDamage) {
        ImGui::Text("Magic Damage: %.0f%%", state.magicDamagePercent * 100.0f);
         }

         // v0.6.7: Show last damage type
         if (state.lastDamageType != State::DamageType::Unknown) {
        ImGui::Text("Last Type: %s", State::GetDamageTypeName(state.lastDamageType));
         }

         // v0.6.7: Show recent elemental damage indicators (compact horizontal row)
         const bool recentFire = state.TookDamageTypeRecently(State::DamageType::Fire, 5.0f);
         const bool recentFrost = state.TookDamageTypeRecently(State::DamageType::Frost, 5.0f);
         const bool recentShock = state.TookDamageTypeRecently(State::DamageType::Shock, 5.0f);
         const bool recentPoison = state.TookDamageTypeRecently(State::DamageType::Poison, 5.0f);

         if (recentFire || recentFrost || recentShock || recentPoison) {
        DrawIndicatorRow3(
           "Fire", recentFire, Colors::EFFECT_FIRE,
           "Frost", recentFrost, Colors::EFFECT_FROST,
           "Shock", recentShock, Colors::EFFECT_SHOCK);
        DrawIndicator("Poison", recentPoison, Colors::EFFECT_POISON);
         }

         if (state.IsCriticalDamage()) {
        ImGui::TextColored(Colors::HEALTH_CRITICAL, "[!] CRITICAL DAMAGE");
         } else if (state.IsUnderSustainedAttack()) {
        ImGui::TextColored(Colors::HEALTH_LOW, "[!] Sustained Attack");
         }

         if (state.damageIncreasing) {
        ImGui::TextColored(Colors::HEALTH_LOW, "Trend: Increasing");
         } else if (state.damageDecreasing) {
        ImGui::TextColored(Colors::STAMINA_NORMAL, "Trend: Decreasing");
         }
      } else {
         ImGui::TextDisabled("No recent damage (%.1fs)", state.timeSinceLastHit);
      }

      ImGui::Spacing();

      // Healing section
      ImGui::TextColored(Colors::HEADER_YELLOW, "Recent Healing:");

      if (state.IsActivelyHealing()) {
         ImGui::Text("Healing Received: %.1f HP (last 2s)", state.recentHealingReceived);
         ImGui::Text("Healing Rate: %.1f HP/s", state.healingRate);

         ImGui::Text("Sources: Potion %.0f%% | Spell %.0f%% | Regen %.0f%%",
        state.potionHealingPercent * 100.0f,
        state.spellHealingPercent * 100.0f,
        state.naturalRegenPercent * 100.0f);

         const float netChange = state.GetNetHealthChange();
         if (netChange > 0.0f) {
        ImGui::TextColored(Colors::STAMINA_NORMAL, "Net: +%.1f HP/s (gaining)", netChange);
         } else if (netChange < 0.0f) {
        ImGui::TextColored(Colors::HEALTH_LOW, "Net: %.1f HP/s (losing)", netChange);
         }
      } else {
         ImGui::TextDisabled("No recent healing (%.1fs)", state.timeSinceLastHeal);
      }

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // STAMINA TRACKING SECTION (v0.6.9)
   // =============================================================================

   void StateManagerDebugWidget::DrawStaminaTrackingSection(const State::StaminaTrackingState& state)
   {
      ImGui::Indent(8.0f);

      // Usage section
      ImGui::TextColored(Colors::HEADER_YELLOW, "Stamina Usage:");

      if (state.usage.IsActive(2.0f)) {
         ImGui::Text("Usage Rate: %.1f/s", state.usage.rate);
         ImGui::Text("Recent Usage: %.1f (last 2s)", state.usage.recentAmount);

         // Source breakdown
         if (state.powerAttackPercent > 0.1f || state.sprintPercent > 0.1f) {
        ImGui::Text("Sources: PowerAtk %.0f%% | Sprint %.0f%%",
           state.powerAttackPercent * 100.0f,
           state.sprintPercent * 100.0f);
         }

         if (state.IsUsingStaminaHeavily()) {
        ImGui::TextColored(Colors::HEALTH_LOW, "[!] Heavy Usage");
         }

         // Trend
         if (state.usage.isIncreasing) {
        ImGui::TextColored(Colors::HEALTH_LOW, "Trend: Increasing");
         } else if (state.usage.isDecreasing) {
        ImGui::TextColored(Colors::STAMINA_NORMAL, "Trend: Decreasing");
         }
      } else {
         ImGui::TextDisabled("No recent usage (%.1fs)", state.usage.timeSinceLast);
      }

      ImGui::Spacing();

      // Regen section
      ImGui::TextColored(Colors::HEADER_YELLOW, "Stamina Regen:");

      if (state.regen.IsActive(2.0f)) {
         ImGui::Text("Regen Rate: %.1f/s", state.regen.rate);

         const float netChange = state.GetNetStaminaChange();
         if (netChange > 0.0f) {
        ImGui::TextColored(Colors::STAMINA_NORMAL, "Net: +%.1f/s (recovering)", netChange);
         } else if (netChange < 0.0f) {
        ImGui::TextColored(Colors::HEALTH_LOW, "Net: %.1f/s (draining)", netChange);
         }
      } else {
         ImGui::TextDisabled("No recent regen (%.1fs)", state.regen.timeSinceLast);
      }

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // MAGICKA TRACKING SECTION (v0.6.9)
   // =============================================================================

   void StateManagerDebugWidget::DrawMagickaTrackingSection(const State::MagickaTrackingState& state)
   {
      ImGui::Indent(8.0f);

      // Usage section
      ImGui::TextColored(Colors::HEADER_YELLOW, "Magicka Usage:");

      if (state.IsActivelyCasting()) {
         ImGui::Text("Usage Rate: %.1f/s", state.usage.rate);
         ImGui::Text("Recent Usage: %.1f (last 2s)", state.usage.recentAmount);

         // Casting state indicators
         DrawIndicatorPair("Channeling", state.isChanneling, Colors::BUFF_COLOR,
                           "Ward Active", state.isHoldingWard, Colors::BUFF_COLOR, Layout::COLUMN_WIDTH);

         // Source breakdown
         if (state.instantCastPercent > 0.1f || state.concentrationPercent > 0.1f) {
        ImGui::Text("Sources: Instant %.0f%% | Conc %.0f%%",
           state.instantCastPercent * 100.0f,
           state.concentrationPercent * 100.0f);
         }

         if (state.IsUsingMagickaHeavily()) {
        ImGui::TextColored(Colors::HEALTH_LOW, "[!] Heavy Usage");
         }

         // Trend
         if (state.usage.isIncreasing) {
        ImGui::TextColored(Colors::HEALTH_LOW, "Trend: Increasing");
         } else if (state.usage.isDecreasing) {
        ImGui::TextColored(Colors::STAMINA_NORMAL, "Trend: Decreasing");
         }
      } else {
         ImGui::TextDisabled("No recent casting (%.1fs)", state.usage.timeSinceLast);
      }

      ImGui::Spacing();

      // Regen section
      ImGui::TextColored(Colors::HEADER_YELLOW, "Magicka Regen:");

      if (state.regen.IsActive(2.0f)) {
         ImGui::Text("Regen Rate: %.1f/s", state.regen.rate);

         const float netChange = state.GetNetMagickaChange();
         if (netChange > 0.0f) {
        ImGui::TextColored(Colors::MAGICKA_NORMAL, "Net: +%.1f/s (recovering)", netChange);
         } else if (netChange < 0.0f) {
        ImGui::TextColored(Colors::HEALTH_LOW, "Net: %.1f/s (draining)", netChange);
         }
      } else {
         ImGui::TextDisabled("No recent regen (%.1fs)", state.regen.timeSinceLast);
      }

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // HELPER METHODS
   // =============================================================================

   void StateManagerDebugWidget::DrawIndicator(const char* label, bool active, ImVec4 activeColor) const
   {
      ImVec4 color = active ? activeColor : Colors::INACTIVE_INDICATOR;
      ImGui::TextColored(color, "[%c] %s", active ? 'X' : ' ', label);
   }

   void StateManagerDebugWidget::DrawProgressBar(const char* label, float value, ImVec4 color, const char* overlay) const
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

   void StateManagerDebugWidget::DrawVitalBarWithDebuff(const char* label, float current, float effectiveMax, float baseMax,
                                                        ImVec4 normalColor, ImVec4 debuffColor) const
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

   void StateManagerDebugWidget::DrawIndicatorPair(
      const char* label1, bool active1, ImVec4 activeColor1,
      const char* label2, bool active2, ImVec4 activeColor2,
      float columnWidth) const
   {
      ImVec4 color1 = active1 ? activeColor1 : Colors::INACTIVE_INDICATOR;
      ImGui::TextColored(color1, "[%c] %s", active1 ? 'X' : ' ', label1);

      ImGui::SameLine(columnWidth);

      if (label2 && label2[0] != '\0') {
         ImVec4 color2 = active2 ? activeColor2 : Colors::INACTIVE_INDICATOR;
         ImGui::TextColored(color2, "[%c] %s", active2 ? 'X' : ' ', label2);
      }
   }

   void StateManagerDebugWidget::DrawIndicatorRow3(
      const char* label1, bool active1, ImVec4 activeColor1,
      const char* label2, bool active2, ImVec4 activeColor2,
      const char* label3, bool active3, ImVec4 activeColor3,
      float spacing) const
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
