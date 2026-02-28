// =============================================================================
// StateManager_MagicEffects.cpp - Player magic effects and buffs polling
// =============================================================================
// Part of StateManager implementation split (v0.6.x Phase 6)
// Polls: damage effects, buffs, vampire/werewolf status
// Updates: PlayerActorState.effects, PlayerActorState.buffs, vampire/werewolf flags
// Includes: CacheRaceFormIDs() helper
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"
#include "../Profiling.h"

namespace Huginn::State
{
  // =============================================================================
  // RACE FORMID CACHING (v0.6.6)
  // =============================================================================
  // Performance optimization: Cache vampire/werewolf race FormIDs on first use
  // to avoid expensive std::strstr() calls on race EditorIDs every poll cycle.
  // =============================================================================

  void StateManager::CacheRaceFormIDs() noexcept
  {
    if (m_raceFormIDsCached) {
      return;  // Already cached
    }

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
      return;  // Can't cache yet - try again next poll
    }

    // Iterate through all races and cache vampire/werewolf FormIDs
    for (auto* race : dataHandler->GetFormArray<RE::TESRace>()) {
      if (!race) continue;

      const char* editorID = race->GetFormEditorID();
      if (!editorID) continue;

      // Check for vampire races
      if (std::strstr(editorID, "Vampire")) {
        m_vampireRaceFormIDs.insert(race->GetFormID());
        logger::trace("[StateManager] Cached vampire race: {} (0x{:08X})", editorID, race->GetFormID());
      }

      // Check for werewolf beast form race
      if (std::strstr(editorID, "WerewolfBeast")) {
        m_werewolfBeastFormID = race->GetFormID();
        logger::trace("[StateManager] Cached werewolf beast form race: {} (0x{:08X})", editorID, race->GetFormID());
      }
      // Check for werewolf human form (has "Werewolf" but not "Beast")
      else if (std::strstr(editorID, "Werewolf")) {
        m_werewolfHumanFormID = race->GetFormID();
        logger::trace("[StateManager] Cached werewolf human form race: {} (0x{:08X})", editorID, race->GetFormID());
      }
    }

    m_raceFormIDsCached = true;
    logger::info("[StateManager] Cached {} vampire races, werewolf beast=0x{:08X}, werewolf human=0x{:08X}",
      m_vampireRaceFormIDs.size(), m_werewolfBeastFormID, m_werewolfHumanFormID);
  }

  // =============================================================================
  // MAGIC EFFECTS POLLING
  // =============================================================================

  bool StateManager::PollPlayerMagicEffects()
  {
    Huginn_ZONE_NAMED("PollPlayerMagicEffects");
    // OPTIMIZATION: Single iteration for both effects and buffs
    // (merged EffectsSensor + ActiveBuffsSensor)

    ActorEffects newEffects;
    ActorBuffs newBuffs;

    // Early null checks - if any fail, we skip processing but still update state at end
    // (Consolidated from duplicate early-exit blocks - v0.6.1 code review fix)
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) [[unlikely]] {
      // Critical error - player doesn't exist, reset to defaults
      std::unique_lock lock(m_playerMutex);
      m_playerState.effects = newEffects;
      m_playerState.buffs = newBuffs;
      return false;
    }

    // BUG FIX (v0.6.1): Must use AsMagicTarget(), NOT As<RE::MagicTarget>()
    // As<>() returns nullptr for PlayerCharacter even though it implements MagicTarget.
    // AsMagicTarget() is a virtual function that correctly returns the interface.
    auto* magicTarget = player->AsMagicTarget();
    if (!magicTarget) [[unlikely]] {
      // Critical error - magic target interface unavailable, reset to defaults
      std::unique_lock lock(m_playerMutex);
      m_playerState.effects = newEffects;
      m_playerState.buffs = newBuffs;
      return false;
    }

    auto* activeEffects = magicTarget->GetActiveEffectList();

    // Single iteration over active effects (pattern from EffectsSensor.cpp)
    if (activeEffects) {
      for (auto* effect : *activeEffects) {
        if (!effect) continue;

        // Skip inactive effects
        if (effect->flags.any(RE::ActiveEffect::Flag::kInactive)) {
          continue;
        }

        auto* baseEffect = effect->GetBaseObject();
        if (!baseEffect) continue;

        // Skip passive ability effects (survival mode, racial abilities, perks)
        auto* spell = effect->spell;
        if (spell) {
          auto spellType = spell->GetSpellType();
          if (spellType == RE::MagicSystem::SpellType::kAbility ||
              spellType == RE::MagicSystem::SpellType::kAddiction) {
            continue;
          }
        }

        auto archetype = baseEffect->GetArchetype();
        auto primaryAV = baseEffect->data.primaryAV;
        auto resistAV = baseEffect->data.resistVariable;
        float magnitude = effect->magnitude;

#ifdef _DEBUG
        // Debug logging for buff detection
        // Wrapped in log level check to avoid GetFullName() overhead when trace disabled
        if (spdlog::default_logger()->should_log(spdlog::level::trace)) {
          bool isDetrimental = baseEffect->IsDetrimental();
          const char* effectName = baseEffect->GetFullName();
          logger::trace("[StateManager] Effect: '{}' | Archetype: {} | PrimaryAV: {} | ResistAV: {} | Detrimental: {} | Magnitude: {}",
            effectName ? effectName : "UNNAMED",
            static_cast<int>(archetype),
            static_cast<int>(primaryAV),
            static_cast<int>(resistAV),
            isDetrimental,
            magnitude);
        }
#endif

        // =============================================================================
        // REGEN BUFF/DEBUFF DETECTION (v0.6.6)
        // =============================================================================
        // Check for regen modifiers BEFORE archetype switch - these can appear
        // under multiple archetypes (ValueModifier, PeakValueModifier, etc.)
        // and we want to catch them all.
        bool isRegenEffect = false;

        // Health Regen
        if (primaryAV == RE::ActorValue::kHealRate ||
            primaryAV == RE::ActorValue::kHealRateMult) {
          isRegenEffect = true;
          if (baseEffect->IsDetrimental() || magnitude < 0.0f) {
            newBuffs.hasHealthRegenDebuff = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected HEALTH REGEN DEBUFF (mag: {})", magnitude);
#endif
          } else {
            newBuffs.hasHealthRegenBuff = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected HEALTH REGEN BUFF (mag: {})", magnitude);
#endif
          }
        }
        // Magicka Regen
        else if (primaryAV == RE::ActorValue::kMagickaRate ||
                 primaryAV == RE::ActorValue::kMagickaRateMult) {
          isRegenEffect = true;
          if (baseEffect->IsDetrimental() || magnitude < 0.0f) {
            newBuffs.hasMagickaRegenDebuff = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected MAGICKA REGEN DEBUFF (mag: {})", magnitude);
#endif
          } else {
            newBuffs.hasMagickaRegenBuff = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected MAGICKA REGEN BUFF (mag: {})", magnitude);
#endif
          }
        }
        // Stamina Regen
        else if (primaryAV == RE::ActorValue::kStaminaRate ||
                 primaryAV == RE::ActorValue::kStaminaRateMult) {
          isRegenEffect = true;
          if (baseEffect->IsDetrimental() || magnitude < 0.0f) {
            newBuffs.hasStaminaRegenDebuff = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected STAMINA REGEN DEBUFF (mag: {})", magnitude);
#endif
          } else {
            newBuffs.hasStaminaRegenBuff = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected STAMINA REGEN BUFF (mag: {})", magnitude);
#endif
          }
        }

        // =============================================================================
        // FORTIFY MAGIC SCHOOL DETECTION (v0.8.x)
        // =============================================================================
        // Check for Fortify School effects BEFORE archetype switch - these can appear
        // under kValueModifier or kPeakValueModifier (LORERIM uses kPeakValueModifier)
        // and we want to catch them all.
        bool isFortifySchoolEffect = false;

        if (!baseEffect->IsDetrimental() && magnitude > 0.0f) {
          // Base school AVs
          if (primaryAV == RE::ActorValue::kDestruction) {
            newBuffs.hasFortifyDestruction = true;
            isFortifySchoolEffect = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected FORTIFY DESTRUCTION (mag: {})", magnitude);
#endif
          } else if (primaryAV == RE::ActorValue::kConjuration) {
            newBuffs.hasFortifyConjuration = true;
            isFortifySchoolEffect = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected FORTIFY CONJURATION (mag: {})", magnitude);
#endif
          } else if (primaryAV == RE::ActorValue::kRestoration) {
            newBuffs.hasFortifyRestoration = true;
            isFortifySchoolEffect = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected FORTIFY RESTORATION (mag: {})", magnitude);
#endif
          } else if (primaryAV == RE::ActorValue::kAlteration) {
            newBuffs.hasFortifyAlteration = true;
            isFortifySchoolEffect = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected FORTIFY ALTERATION (mag: {})", magnitude);
#endif
          } else if (primaryAV == RE::ActorValue::kIllusion) {
            newBuffs.hasFortifyIllusion = true;
            isFortifySchoolEffect = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected FORTIFY ILLUSION (mag: {})", magnitude);
#endif
          } else if (primaryAV == RE::ActorValue::kEnchanting) {
            newBuffs.hasFortifyEnchanting = true;
            isFortifySchoolEffect = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected FORTIFY ENCHANTING (mag: {})", magnitude);
#endif
          }
          // LORERIM PowerModifier variants (AV 144-148 for schools)
          else if (primaryAV == RE::ActorValue::kDestructionPowerModifier) {
            newBuffs.hasFortifyDestruction = true;
            isFortifySchoolEffect = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected FORTIFY DESTRUCTION (LORERIM, mag: {})", magnitude);
#endif
          } else if (primaryAV == RE::ActorValue::kConjurationPowerModifier) {
            newBuffs.hasFortifyConjuration = true;
            isFortifySchoolEffect = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected FORTIFY CONJURATION (LORERIM, mag: {})", magnitude);
#endif
          } else if (primaryAV == RE::ActorValue::kRestorationPowerModifier) {
            newBuffs.hasFortifyRestoration = true;
            isFortifySchoolEffect = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected FORTIFY RESTORATION (LORERIM, mag: {})", magnitude);
#endif
          } else if (primaryAV == RE::ActorValue::kAlterationPowerModifier) {
            newBuffs.hasFortifyAlteration = true;
            isFortifySchoolEffect = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected FORTIFY ALTERATION (LORERIM, mag: {})", magnitude);
#endif
          } else if (primaryAV == RE::ActorValue::kIllusionPowerModifier) {
            newBuffs.hasFortifyIllusion = true;
            isFortifySchoolEffect = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected FORTIFY ILLUSION (LORERIM, mag: {})", magnitude);
#endif
          }
        }

        // Process effects by archetype (match ActiveBuffsSensor approach - don't rely on IsDetrimental flag)
        switch (archetype) {
          // BUFFS
          case RE::EffectSetting::Archetype::kInvisibility:
            newBuffs.isInvisible = true;
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected INVISIBILITY");
#endif
            break;

          case RE::EffectSetting::Archetype::kValueModifier:
          case RE::EffectSetting::Archetype::kPeakValueModifier:
            // Armor buffs: any DamageResist modifier = armor spell active
            // Simplified from magnitude-based level detection (v0.12.x) for mod compatibility.
            // Mods (Ordinator, Apocalypse, etc.) change armor spell magnitudes, breaking
            // threshold-based classification. The recommendation engine handles preference.
            if (primaryAV == RE::ActorValue::kDamageResist) {
              newBuffs.hasArmorBuff = true;
            }
            // Water Breathing
            else if (primaryAV == RE::ActorValue::kWaterBreathing) {
              newBuffs.hasWaterBreathing = true;
#ifdef _DEBUG
              logger::trace("[StateManager] --> Detected WATER BREATHING");
#endif
            }
            // =============================================================================
            // RESTORE-OVER-TIME DETECTION (v0.6.6)
            // =============================================================================
            // Soups/stews (e.g., Vegetable Soup) use "Restore 1 HP/sec for 720s" which is
            // kValueModifier + kHealth, NOT kHealRateMult. Treat these as regen buffs
            // since they provide continuous healing similar to regen effects.
            // =============================================================================
            else if (!baseEffect->IsDetrimental() && magnitude > 0.0f) {
              // Restore Health over time (soups, slow-heal potions)
              if (primaryAV == RE::ActorValue::kHealth) {
                newBuffs.hasHealthRegenBuff = true;
#ifdef _DEBUG
                logger::trace("[StateManager] --> Detected RESTORE HEALTH over time (mag: {})", magnitude);
#endif
              }
              // Restore Magicka over time
              else if (primaryAV == RE::ActorValue::kMagicka) {
                newBuffs.hasMagickaRegenBuff = true;
#ifdef _DEBUG
                logger::trace("[StateManager] --> Detected RESTORE MAGICKA over time (mag: {})", magnitude);
#endif
              }
              // Restore Stamina over time (Vegetable Soup also restores stamina)
              else if (primaryAV == RE::ActorValue::kStamina) {
                newBuffs.hasStaminaRegenBuff = true;
#ifdef _DEBUG
                logger::trace("[StateManager] --> Detected RESTORE STAMINA over time (mag: {})", magnitude);
#endif
              }
            }
            // Check for debuffs with ValueModifier/PeakValueModifier archetype
            // Note: Regen detection moved before archetype switch (v0.6.6)
            //
            // Guard against false positives from permanent/quasi-permanent effects
            // (enchantments, perks, standing stones, CC Survival Mode penalties).
            // Only classify as an active drain if:
            //   - Enchantments (kEnchantment): NEVER — gear effects aren't combat drains
            //   - Diseases/Poisons (kDisease, kPoison): ALWAYS — even though duration == 0
            //   - Other spell types: only if finite AND short duration (< MAX_DRAIN_DURATION)
            //     CC Survival Mode uses kSpell + duration=99999; standing stones similar.
            //   - No spell reference: only if finite + short duration (likely script-applied)
            else if (baseEffect->IsDetrimental()) {
              bool isActiveDrain = false;
              if (spell) {
                auto st = spell->GetSpellType();
                if (st == RE::MagicSystem::SpellType::kEnchantment) {
                  isActiveDrain = false;  // Gear effects are not combat drains
                } else if (st == RE::MagicSystem::SpellType::kDisease ||
                           st == RE::MagicSystem::SpellType::kPoison) {
                  isActiveDrain = true;   // Diseases/poisons always count
                } else {
                  // Short-lived combat effects only (excludes survival penalties, standing stones)
                  isActiveDrain = (effect->duration > 0.0f &&
                                   effect->duration < EffectDetection::MAX_DRAIN_DURATION);
                }
              } else {
                isActiveDrain = (effect->duration > 0.0f &&
                                 effect->duration < EffectDetection::MAX_DRAIN_DURATION);
              }

              if (isActiveDrain) {
                if (primaryAV == RE::ActorValue::kHealth) {
                  newEffects.hasHealthDrain = true;
                } else if (primaryAV == RE::ActorValue::kMagicka) {
                  newEffects.hasMagickaPoison = true;
                } else if (primaryAV == RE::ActorValue::kStamina) {
                  newEffects.hasStaminaPoison = true;
                }
              }
            }
            break;

          case RE::EffectSetting::Archetype::kDualValueModifier:
            // Muffle (Fame AV)
            if (primaryAV == RE::ActorValue::kFame) {
              newBuffs.hasMuffle = true;
#ifdef _DEBUG
              logger::trace("[StateManager] --> Detected MUFFLE");
#endif
            }
            break;

          case RE::EffectSetting::Archetype::kCloak:
            newBuffs.hasCloakActive = true;
            // Detect cloak type by resist variable
            if (resistAV == RE::ActorValue::kResistFire) {
              newBuffs.activeCloakType = EffectType::CloakFire;
            } else if (resistAV == RE::ActorValue::kResistFrost) {
              newBuffs.activeCloakType = EffectType::CloakFrost;
            } else if (resistAV == RE::ActorValue::kResistShock) {
              newBuffs.activeCloakType = EffectType::CloakShock;
            }
#ifdef _DEBUG
            logger::trace("[StateManager] --> Detected CLOAK (type: {})", static_cast<int>(newBuffs.activeCloakType));
#endif
            break;

          case RE::EffectSetting::Archetype::kSummonCreature:
            // NOTE: Don't set hasActiveSummon here - kSummonCreature archetype
            // shows active even if the summoned creature has died.
            // We check ProcessLists below to verify a living summon exists.
            break;

          // DEBUFFS - Check for damage over time effects
          default:
            if (baseEffect->IsDetrimental()) {
              // Check damage type by resist actor value
              if (resistAV == RE::ActorValue::kResistFire) {
                newEffects.isOnFire = true;
              } else if (resistAV == RE::ActorValue::kResistFrost) {
                newEffects.isFrozen = true;
              } else if (resistAV == RE::ActorValue::kResistShock) {
                newEffects.isShocked = true;
              } else if (resistAV == RE::ActorValue::kPoisonResist) {
                newEffects.isPoisoned = true;
              } else if (resistAV == RE::ActorValue::kResistDisease) {
                newEffects.isDiseased = true;
              }
            }
            break;
        }
      }
    }

    // Check for active summons by verifying living summoned creatures exist
    // (pattern from ActiveBuffsSensor - more accurate than kSummonCreature archetype)
    if (player) {
      auto* processLists = RE::ProcessLists::GetSingleton();
      if (processLists) {
        for (auto& actorHandle : processLists->highActorHandles) {
          auto actorPtr = actorHandle.get();
          if (!actorPtr) {
            continue;
          }
          auto* actor = actorPtr.get();
          if (!actor || actor->IsDead()) {
            continue;
          }

          // const: only comparing, not modifying the commanding actor
          const RE::Actor* commandingActor = actor->GetCommandingActor().get();
          if (commandingActor && commandingActor == player) {
            newBuffs.hasActiveSummon = true;
            break;
          }
        }
      }
    }

    // =============================================================================
    // VAMPIRE/WEREWOLF DETECTION (v0.6.6)
    // =============================================================================
    // Detect vampire stage and werewolf status for transformation context.
    // Uses cached FormIDs for performance (avoids std::strstr every poll).
    int newVampireStage = VampireThreshold::NOT_VAMPIRE;
    bool newIsWerewolf = false;
    bool newIsInBeastForm = false;

    // Ensure race FormIDs are cached (one-time initialization)
    if (!m_raceFormIDsCached) {
      const_cast<StateManager*>(this)->CacheRaceFormIDs();
    }

    // Vampire detection via Vampirism actor value
    // The Vampirism actor value ranges from 0-4 corresponding to vampire stages
    auto* actorValueOwner = player->AsActorValueOwner();
    if (actorValueOwner) {
      float vampirismValue = actorValueOwner->GetActorValue(RE::ActorValue::kVampirePerks);
      if (vampirismValue > 0.0f) {
        // Vampirism AV indicates vampire stage (1-4)
        newVampireStage = std::clamp(static_cast<int>(vampirismValue),
                                      VampireThreshold::STAGE_1,
                                      VampireThreshold::STAGE_4);
      }
    }

    // Alternative vampire detection: Check player race against cached vampire FormIDs
    // This handles cases where the actor value might not be set
    if (newVampireStage == VampireThreshold::NOT_VAMPIRE) {
      auto* race = player->GetRace();
      if (race && m_vampireRaceFormIDs.contains(race->GetFormID())) {
        // Found vampire race, assume stage 1 minimum
        newVampireStage = VampireThreshold::STAGE_1;
      }
    }

    // Werewolf detection: Check player race against cached werewolf FormIDs
    auto* race = player->GetRace();
    if (race) {
      RE::FormID raceFormID = race->GetFormID();

      // Check if currently in beast form
      if (m_werewolfBeastFormID != 0 && raceFormID == m_werewolfBeastFormID) {
        newIsWerewolf = true;
        newIsInBeastForm = true;
      }
      // Check for latent werewolf (not transformed)
      else if (m_werewolfHumanFormID != 0 && raceFormID == m_werewolfHumanFormID) {
        newIsWerewolf = true;
      }
    }

    // Update effects and buffs with change detection
    {
      std::unique_lock lock(m_playerMutex);
      bool effectsChanged = (m_playerState.effects != newEffects);
      bool buffsChanged = (m_playerState.buffs != newBuffs);
      bool vampireChanged = (m_playerState.vampireStage != newVampireStage);
      bool werewolfChanged = (m_playerState.isWerewolf != newIsWerewolf ||
                              m_playerState.isInBeastForm != newIsInBeastForm);

      if (effectsChanged) {
        m_playerState.effects = newEffects;
#ifdef _DEBUG
        logger::trace("[StateManager] PlayerEffects changed"sv);
#endif
      }
      if (buffsChanged) {
        m_playerState.buffs = newBuffs;
#ifdef _DEBUG
        logger::trace("[StateManager] PlayerBuffs changed"sv);
#endif
      }

      // Update vampire/werewolf state (v0.6.6)
      if (vampireChanged || werewolfChanged) {
        m_playerState.vampireStage = newVampireStage;
        m_playerState.isWerewolf = newIsWerewolf;
        m_playerState.isInBeastForm = newIsInBeastForm;
#ifdef _DEBUG
        logger::trace("[StateManager] Transformation state changed: vampire={} werewolf={} beastForm={}",
          newVampireStage, newIsWerewolf, newIsInBeastForm);
#endif
      }

      // Log state changes at debug level (useful for troubleshooting)
      if (effectsChanged || buffsChanged) {
        logger::debug("[StateManager] Magic state: armor={} cloak={} invis={} summon={} | fire={} poison={} frost={}",
          newBuffs.hasArmorBuff,
          newBuffs.hasCloakActive, newBuffs.isInvisible, newBuffs.hasActiveSummon,
          newEffects.isOnFire, newEffects.isPoisoned, newEffects.isFrozen);
      }

      // Stage 3b: Return true if any state changed
      return effectsChanged || buffsChanged || vampireChanged || werewolfChanged;
    }
  }

} // namespace Huginn::State
