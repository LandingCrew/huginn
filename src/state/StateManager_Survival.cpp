// =============================================================================
// StateManager_Survival.cpp - Player survival mode polling
// =============================================================================
// Part of StateManager implementation split (v0.6.x Phase 6)
// Polls: hunger, cold, fatigue levels (CC Survival Mode)
// Updates: PlayerActorState survival fields
// Includes: CacheSurvivalGlobals() helper
//
// SMI (Survival Mode Improved SKSE) compatibility:
// - Reads pre-computed stage globals instead of raw 0-1000 values
// - Respects per-need enable/disable flags
// - Falls back to vanilla CC threshold conversion when SMI is not installed
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"
#include "../Profiling.h"

namespace Huginn::State
{
   // =============================================================================
   // SURVIVAL MODE GLOBALS CACHE (v0.6.7, enhanced for SMI)
   // Cache TESGlobal pointers for CC Survival Mode and SMI.
   // FormIDs from: https://github.com/colinswrath/Survival-Mode-Improved-SKSE
   // =============================================================================

   void StateManager::CacheSurvivalGlobals() noexcept
   {
      if (m_survivalGlobalsCached) {
      return;  // Already cached
      }

      auto* dataHandler = RE::TESDataHandler::GetSingleton();
      if (!dataHandler) {
      return;  // Can't cache yet - try again next poll
      }

      // =========================================================================
      // CC Survival Mode globals (ccqdrsse001-survivalmode.esl)
      // =========================================================================

      constexpr std::string_view survivalPlugin = "ccqdrsse001-survivalmode.esl";

      // FormIDs from CC Survival Mode (via Survival Mode Improved SKSE FormLoader.h)
      constexpr RE::FormID coldNeedValueFormID = 0x81B;       // Cold (0-1000)
      constexpr RE::FormID hungerNeedValueFormID = 0x81A;     // Hunger (0-1000)
      constexpr RE::FormID exhaustionNeedValueFormID = 0x816; // Exhaustion (0-1000)
      constexpr RE::FormID survivalEnabledFormID = 0x826;     // Survival mode toggle

      m_survivalColdNeedValue = dataHandler->LookupForm<RE::TESGlobal>(coldNeedValueFormID, survivalPlugin);
      m_survivalHungerNeedValue = dataHandler->LookupForm<RE::TESGlobal>(hungerNeedValueFormID, survivalPlugin);
      m_survivalExhaustionNeedValue = dataHandler->LookupForm<RE::TESGlobal>(exhaustionNeedValueFormID, survivalPlugin);
      m_survivalModeEnabled = dataHandler->LookupForm<RE::TESGlobal>(survivalEnabledFormID, survivalPlugin);

      m_survivalGlobalsCached = true;

      logger::info("[StateManager] Survival globals cache: Cold={}, Hunger={}, Exhaustion={}, Enabled={}",
      m_survivalColdNeedValue ? "found" : "null",
      m_survivalHungerNeedValue ? "found" : "null",
      m_survivalExhaustionNeedValue ? "found" : "null",
      m_survivalModeEnabled ? "found" : "null");

      // =========================================================================
      // SMI (Survival Mode Improved SKSE) detection
      // SMI provides pre-computed stage globals (0-5 integer) and per-need
      // enable/disable flags, which are more reliable than raw value thresholds.
      // =========================================================================

      constexpr std::string_view smiPlugin = "SurvivalModeImproved.esp";

      // Pre-computed stage globals (integer 0-5, matching our SurvivalThreshold constants)
      m_smiHungerStage = dataHandler->LookupForm<RE::TESGlobal>(0xA14, smiPlugin);
      m_smiExhaustionStage = dataHandler->LookupForm<RE::TESGlobal>(0xA1C, smiPlugin);
      m_smiColdStage = dataHandler->LookupForm<RE::TESGlobal>(0xD1E, smiPlugin);

      // Per-need enable/disable flags (1.0 = enabled, 0.0 = disabled)
      m_smiHungerEnabled = dataHandler->LookupForm<RE::TESGlobal>(0xF27, smiPlugin);
      m_smiColdEnabled = dataHandler->LookupForm<RE::TESGlobal>(0xF28, smiPlugin);
      m_smiExhaustionEnabled = dataHandler->LookupForm<RE::TESGlobal>(0xF29, smiPlugin);

      m_smiInstalled = (m_smiHungerStage || m_smiExhaustionStage || m_smiColdStage);

      if (m_smiInstalled) {
      logger::info("[StateManager] SMI detected: HungerStage={}, ColdStage={}, ExhaustionStage={}",
          m_smiHungerStage ? "found" : "null",
          m_smiColdStage ? "found" : "null",
          m_smiExhaustionStage ? "found" : "null");
      } else {
      logger::info("[StateManager] SMI not installed");
      }

      // =========================================================================
      // Native warmth function (CC Survival engine function)
      // GetWarmthRating returns the player's current warmth from gear/buffs.
      // More accurate than reading kVariable01 which may not be the warmth AV.
      // =========================================================================

      const auto warmthAddr = REL::RelocationID(25834, 26394).address();
      if (warmthAddr) {
      m_getWarmthRating = reinterpret_cast<GetWarmthRating_t*>(warmthAddr);
      m_warmthFunctionCached = true;
      logger::info("[StateManager] Native warmth function cached");
      } else {
      m_warmthFunctionCached = false;
      logger::info("[StateManager] Native warmth function not available");
      }
   }

   // =============================================================================
   // SURVIVAL MODE POLLING
   // =============================================================================

   bool StateManager::PollPlayerSurvival()
   {
      Huginn_ZONE_NAMED("PollPlayerSurvival");
      // Initialize cached survival globals if needed
      CacheSurvivalGlobals();

      int newHungerLevel = DefaultState::NEUTRAL_SURVIVAL;
      int newColdLevel = DefaultState::NEUTRAL_SURVIVAL;
      int newFatigueLevel = DefaultState::NEUTRAL_SURVIVAL;
      bool newSurvivalModeActive = false;
      float newWarmthRating = 0.0f;

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      std::unique_lock lock(m_playerMutex);
      if (m_playerState.hungerLevel != newHungerLevel ||
          m_playerState.coldLevel != newColdLevel ||
          m_playerState.fatigueLevel != newFatigueLevel ||
          m_playerState.survivalModeActive != newSurvivalModeActive ||
          std::abs(m_playerState.warmthRating - newWarmthRating) >= 1.0f) {
        m_playerState.hungerLevel = newHungerLevel;
        m_playerState.coldLevel = newColdLevel;
        m_playerState.fatigueLevel = newFatigueLevel;
        m_playerState.survivalModeActive = newSurvivalModeActive;
        m_playerState.warmthRating = newWarmthRating;
      }
      return false;
      }

      // =============================================================================
      // SURVIVAL MODE ENABLED CHECK
      // Single source of truth: the Survival_ModeEnabled global (0x826).
      // Do NOT set survivalModeActive from individual need values — stale values
      // from a previous session would cause false positives.
      // =============================================================================

      if (m_survivalModeEnabled && m_survivalModeEnabled->value >= 1.0f) {
      newSurvivalModeActive = true;
      }

      // Only read need levels if survival mode is actually enabled
      if (newSurvivalModeActive) {

      // ====================================================================
      // SMI PATH: Read pre-computed stages from SMI globals
      // SMI stores integer stages (0-5) that match our SurvivalThreshold
      // constants directly. Respect per-need enable/disable flags.
      // ====================================================================

      if (m_smiInstalled) {

          // Hunger: read stage if hunger tracking is enabled
          if (m_smiHungerStage) {
          if (!m_smiHungerEnabled || m_smiHungerEnabled->value >= 1.0f) {
              newHungerLevel = static_cast<int>(m_smiHungerStage->value);
          }
          // else: hunger disabled in SMI, leave at NEUTRAL
          }

          // Cold: read stage if cold tracking is enabled
          if (m_smiColdStage) {
          if (!m_smiColdEnabled || m_smiColdEnabled->value >= 1.0f) {
              newColdLevel = static_cast<int>(m_smiColdStage->value);
          }
          }

          // Exhaustion: read stage if exhaustion tracking is enabled
          if (m_smiExhaustionStage) {
          if (!m_smiExhaustionEnabled || m_smiExhaustionEnabled->value >= 1.0f) {
              newFatigueLevel = static_cast<int>(m_smiExhaustionStage->value);
          }
          }

      } else {

          // ====================================================================
          // VANILLA CC FALLBACK: Convert raw 0-1000 values to stages
          // Thresholds from UESP: https://en.uesp.net/wiki/Skyrim:Cold
          // ====================================================================

          // Cold (0-1000 range)
          if (m_survivalColdNeedValue) {
          float coldValue = m_survivalColdNeedValue->value;
          if (coldValue >= 800.0f) {
              newColdLevel = SurvivalThreshold::COLD_NUMB;        // 800-1000
          } else if (coldValue >= 500.0f) {
              newColdLevel = SurvivalThreshold::COLD_FREEZING;    // 500-799
          } else if (coldValue >= 300.0f) {
              newColdLevel = SurvivalThreshold::COLD_VERY_COLD;   // 300-499
          } else if (coldValue >= 120.0f) {
              newColdLevel = SurvivalThreshold::COLD_CHILLY;      // 120-299
          } else if (coldValue >= 50.0f) {
              newColdLevel = SurvivalThreshold::COLD_COMFORTABLE; // 50-119
          }
          // 0-49 = Warm (default NEUTRAL_SURVIVAL = 0)
          }

          // Hunger (0-1000 range)
          // Thresholds from UESP: https://en.uesp.net/wiki/Skyrim:Hunger
          if (m_survivalHungerNeedValue) {
          float hungerValue = m_survivalHungerNeedValue->value;
          if (hungerValue >= 770.0f) {
              newHungerLevel = SurvivalThreshold::HUNGER_STARVING;   // 770-1000
          } else if (hungerValue >= 520.0f) {
              newHungerLevel = SurvivalThreshold::HUNGER_FAMISHED;   // 520-769
          } else if (hungerValue >= 340.0f) {
              newHungerLevel = SurvivalThreshold::HUNGER_HUNGRY;     // 340-519
          } else if (hungerValue >= 160.0f) {
              newHungerLevel = SurvivalThreshold::HUNGER_PECKISH;    // 160-339
          } else if (hungerValue >= 80.0f) {
              newHungerLevel = SurvivalThreshold::HUNGER_FED;        // 80-159 (Satisfied)
          }
          // 0-79 = Well Fed (default HUNGER_WELL_FED = 0)
          }

          // Exhaustion (0-1000 range)
          if (m_survivalExhaustionNeedValue) {
          float fatigueValue = m_survivalExhaustionNeedValue->value;
          if (fatigueValue >= 600.0f) {
              newFatigueLevel = SurvivalThreshold::FATIGUE_DEBILITATED; // 600+ (critical)
          } else if (fatigueValue >= 450.0f) {
              newFatigueLevel = SurvivalThreshold::FATIGUE_WEARY;       // 450-599
          } else if (fatigueValue >= 300.0f) {
              newFatigueLevel = SurvivalThreshold::FATIGUE_TIRED;       // 300-449
          } else if (fatigueValue >= 150.0f) {
              newFatigueLevel = SurvivalThreshold::FATIGUE_SLIGHTLY_TIRED; // 150-299
          }
          // 0-149 = Refreshed (default FATIGUE_REFRESHED = 0)
          }
      }

      // ====================================================================
      // WARMTH RATING
      // Use native GetWarmthRating function for accurate warmth from gear/buffs.
      // Only read when survival mode is active.
      // ====================================================================

      if (m_warmthFunctionCached && m_getWarmthRating) {
          newWarmthRating = m_getWarmthRating(player);
      }
      }

      // Stage 3b: Update survival state with change detection
      {
      std::unique_lock lock(m_playerMutex);
      bool changed = (m_playerState.hungerLevel != newHungerLevel ||
                      m_playerState.coldLevel != newColdLevel ||
                      m_playerState.fatigueLevel != newFatigueLevel ||
                      m_playerState.survivalModeActive != newSurvivalModeActive ||
                      std::abs(m_playerState.warmthRating - newWarmthRating) >= 1.0f);

      if (changed) {
        m_playerState.hungerLevel = newHungerLevel;
        m_playerState.coldLevel = newColdLevel;
        m_playerState.fatigueLevel = newFatigueLevel;
        m_playerState.survivalModeActive = newSurvivalModeActive;
        m_playerState.warmthRating = newWarmthRating;
#ifdef _DEBUG
        logger::trace("[StateManager] PlayerSurvival changed: Hunger={}, Cold={}, Fatigue={}, Warmth={:.0f}, SMI={}"sv,
           newHungerLevel, newColdLevel, newFatigueLevel, newWarmthRating, m_smiInstalled);
#endif
      }
      return changed;
      }
   }

} // namespace Huginn::State
