// Only compile in debug mode
#ifdef _DEBUG

#include "CharacterDebugWidget.h"
#include "StateDebugCommon.h"
#include "../state/StateConstants.h"          // For threshold constants
#include "../state/StateTypes.h"              // For HealthTrackingState
#include <format>
// Note: Huginn::VERSION_STRING is available via PCH.h -> Plugin.h

namespace Huginn::UI
{
   using namespace StateDebug;

   CharacterDebugWidget& CharacterDebugWidget::GetSingleton() noexcept
   {
      static CharacterDebugWidget instance;
      return instance;
   }

   void CharacterDebugWidget::Draw()
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
      static const std::string windowTitle = std::format("Character [v{} DEBUG]", Huginn::VERSION_STRING);

      if (ImGui::Begin(windowTitle.c_str(), &m_isVisible, flags)) {
         // Save position when moved
         ImVec2 windowPos = ImGui::GetWindowPos();
         m_posX = windowPos.x;
         m_posY = windowPos.y;

         // Get current states (thread-safe copies)
         auto playerState = stateManager.GetPlayerState();

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
      }
      ImGui::End();

      ImGui::PopStyleVar(3);
   }

   // =============================================================================
   // VITALS SECTION (matches ContextSensorDebugWidget)
   // =============================================================================

   void CharacterDebugWidget::DrawVitalsSection(const State::ActorVitals& vitals)
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

   void CharacterDebugWidget::DrawEffectsSection(const State::ActorEffects& effects)
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

   void CharacterDebugWidget::DrawActiveBuffsSection(const State::ActorBuffs& buffs)
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

      // Row 4: Night Eye | Light (both silence the darkness weight)
      DrawIndicatorPair("Night Eye", buffs.hasNightEye, Colors::BUFF_COLOR,
                        "Light", buffs.hasLightSpell, Colors::BUFF_COLOR, Layout::COLUMN_WIDTH);

      // Row 5: Warming (silences the cold weight)
      DrawIndicatorPair("Warming", buffs.hasWarmingEffect, Colors::BUFF_COLOR,
                        "", false, Colors::BUFF_COLOR, Layout::COLUMN_WIDTH);

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

   void CharacterDebugWidget::DrawResistancesSection(const State::ActorResistances& resistances)
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

   void CharacterDebugWidget::DrawEquipmentSection(const State::PlayerActorState& state)
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

   void CharacterDebugWidget::DrawSurvivalSection(const State::PlayerActorState& state)
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

   void CharacterDebugWidget::DrawTransformationSection(const State::PlayerActorState& state)
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
   // HEALTH TRACKING SECTION (v0.6.9 - Renamed from Damage Tracking)
   // =============================================================================

   void CharacterDebugWidget::DrawHealthTrackingSection(const State::HealthTrackingState& state)
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

   void CharacterDebugWidget::DrawStaminaTrackingSection(const State::StaminaTrackingState& state)
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

   void CharacterDebugWidget::DrawMagickaTrackingSection(const State::MagickaTrackingState& state)
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

}

#endif  // _DEBUG
