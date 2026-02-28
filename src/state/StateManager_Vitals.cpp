// =============================================================================
// StateManager_Vitals.cpp - Player vitals polling
// =============================================================================
// Part of StateManager implementation split (v0.6.x Phase 6)
// Polls: health, magicka, stamina percentages and max values
// Updates: PlayerActorState.vitals
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"
#include "../Profiling.h"

namespace Huginn::State
{
   bool StateManager::PollPlayerVitals()
   {
      Huginn_ZONE_NAMED("PollPlayerVitals");
      // Unconditional first-call logging to diagnose poll issues
      static int pollCount = 0;
      pollCount++;
      if (pollCount <= 3) {
      logger::info("[StateManager] PollPlayerVitals() called (count={})"sv, pollCount);
      }

      // Pattern from VitalsSensor.cpp
      ActorVitals newVitals;

      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      return UpdateStateIfChanged(m_playerMutex, m_playerState.vitals, newVitals);
      }

      auto* actorValueOwner = player->AsActorValueOwner();
      if (!actorValueOwner) {
      return UpdateStateIfChanged(m_playerMutex, m_playerState.vitals, newVitals);
      }

      // Get current values
      float currentHealth = actorValueOwner->GetActorValue(RE::ActorValue::kHealth);
      float currentMagicka = actorValueOwner->GetActorValue(RE::ActorValue::kMagicka);
      float currentStamina = actorValueOwner->GetActorValue(RE::ActorValue::kStamina);

      // Get temporary modifiers (potion/spell buffs)
      float tempHealth = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kHealth);
      float tempMagicka = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kMagicka);
      float tempStamina = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kStamina);

      // Get permanent actor value
      float permHealth = actorValueOwner->GetPermanentActorValue(RE::ActorValue::kHealth);
      float permMagicka = actorValueOwner->GetPermanentActorValue(RE::ActorValue::kMagicka);
      float permStamina = actorValueOwner->GetPermanentActorValue(RE::ActorValue::kStamina);

      // Get survival mode penalty amounts
      float survivalHealthPenalty = actorValueOwner->GetActorValue(RE::ActorValue::kVariable04);
      float survivalMagickaPenalty = actorValueOwner->GetActorValue(RE::ActorValue::kVariable03);
      float survivalStaminaPenalty = actorValueOwner->GetActorValue(RE::ActorValue::kVariable02);

      // Calculate base max values (before survival debuffs)
      newVitals.baseMaxHealth = tempHealth + permHealth + survivalHealthPenalty;
      newVitals.baseMaxMagicka = tempMagicka + permMagicka + survivalMagickaPenalty;
      newVitals.baseMaxStamina = tempStamina + permStamina + survivalStaminaPenalty;

      // Get effective max values (with survival debuffs)
      float damageHealth = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth);
      float damageMagicka = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka);
      float damageStamina = player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina);

      // Data validation
      if (!std::isfinite(damageHealth) || !std::isfinite(damageMagicka) || !std::isfinite(damageStamina)) {
      return UpdateStateIfChanged(m_playerMutex, m_playerState.vitals, newVitals);
      }

      // Effective max = current - damage
      newVitals.maxHealth = currentHealth - damageHealth;
      newVitals.maxMagicka = currentMagicka - damageMagicka;
      newVitals.maxStamina = currentStamina - damageStamina;

      // Calculate percentages
      if (std::isfinite(newVitals.maxHealth) && newVitals.maxHealth > 0.0f) {
      newVitals.health = std::clamp(currentHealth / newVitals.maxHealth, 0.0f, 1.0f);
      }
      if (std::isfinite(newVitals.maxMagicka) && newVitals.maxMagicka > 0.0f) {
      newVitals.magicka = std::clamp(currentMagicka / newVitals.maxMagicka, 0.0f, 1.0f);
      }
      if (std::isfinite(newVitals.maxStamina) && newVitals.maxStamina > 0.0f) {
      newVitals.stamina = std::clamp(currentStamina / newVitals.maxStamina, 0.0f, 1.0f);
      }

      // Stage 3b: Return change detection flag
      return UpdateStateIfChanged(m_playerMutex, m_playerState.vitals, newVitals);
   }

} // namespace Huginn::State
