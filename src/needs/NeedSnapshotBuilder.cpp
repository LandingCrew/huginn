#include "NeedSnapshotBuilder.h"

#include "Globals.h"
#include "NeedSettings.h"
#include "Profiling.h"
#include "core/BenchKind.h"
#include "state/StateManager.h"

namespace Huginn::Needs
{
   namespace
   {
      using Core::Needs::School;
      using Core::Needs::SchoolBit;

      std::uint32_t SchoolOf(RE::FormID spellID)
      {
         if (spellID == 0) return 0;
         auto* form = RE::TESForm::LookupByID(spellID);
         auto* spell = form ? form->As<RE::SpellItem>() : nullptr;
         if (!spell) return 0;
         switch (spell->GetAssociatedSkill()) {
         case RE::ActorValue::kDestruction: return SchoolBit(School::Destruction);
         case RE::ActorValue::kConjuration: return SchoolBit(School::Conjuration);
         case RE::ActorValue::kRestoration: return SchoolBit(School::Restoration);
         case RE::ActorValue::kAlteration: return SchoolBit(School::Alteration);
         case RE::ActorValue::kIllusion: return SchoolBit(School::Illusion);
         default: return 0;
         }
      }

      // The bench by its workbench keyword (StateManager::BenchKindOf), not
      // the old engine's CraftSkillForWorkstation, which reads every
      // create-object bench (forge, cooking pot and spit, smelter, tanning
      // rack) as smithing and still feeds scoring that way (frozen).
      int WorkstationOf(const State::NeedSensorState& n)
      {
         return Core::Needs::NeedWorkstation(static_cast<Core::Needs::BenchKind>(n.bench));
      }
   }

   Core::Needs::NeedSnapshot BuildNeedSnapshot(const NeedSources& src)
   {
      using Core::Needs::SecondsSince;
      const auto& p = src.player;
      const auto& n = src.sensors;
      const double now = src.nowSec;
      Core::Needs::NeedSnapshot s;

      // Vitals
      s.health = p.vitals.health;
      s.magicka = p.vitals.magicka;
      s.stamina = p.vitals.stamina;
      s.magickaHeld = src.magickaHeld;
      s.staminaHeld = src.staminaHeld;
      s.maxHealth = p.vitals.maxHealth;
      s.maxMagicka = p.vitals.maxMagicka;
      s.maxStamina = p.vitals.maxStamina;
      s.damageRate = src.health.damageRate;
      s.healingRate = src.health.healingRate;
      s.magickaUsageRate = src.magicka.usage.rate;
      s.magickaRegenRate = src.magicka.regen.rate;
      s.staminaUsageRate = src.stamina.usage.rate;
      s.staminaRegenRate = src.stamina.regen.rate;
      s.restoreHealthPending = n.restoreHealthPending;
      s.restoreMagickaPending = n.restoreMagickaPending;
      s.restoreStaminaPending = n.restoreStaminaPending;

      // Elemental threat
      s.dmgFire = n.dmgFire.At(now);
      s.dmgFrost = n.dmgFrost.At(now);
      s.dmgShock = n.dmgShock.At(now);
      s.dmgMagic = n.dmgMagic.At(now);
      s.dmgPhysical = n.dmgPhysical.At(now);
      s.castFireAgo = SecondsSince(n.castFireAt, now);
      s.castFrostAgo = SecondsSince(n.castFrostAt, now);
      s.castShockAgo = SecondsSince(n.castShockAt, now);

      // Status
      s.poisoned = p.effects.isPoisoned;
      s.diseased = p.effects.isDiseased;
      s.drainHealth = p.effects.hasHealthDrain;
      s.drainMagicka = p.effects.hasMagickaPoison;
      s.drainStamina = p.effects.hasStaminaPoison;
      s.regenSuppressed = p.buffs.HasAnyRegenDebuff();
      s.encumbrance = n.encumbrance;
      s.vampireStage = p.vampireStage;

      // Survival
      s.survival = p.survivalModeActive;
      s.hungerStage = p.hungerLevel;
      s.coldStage = p.coldLevel;
      s.fatigueStage = p.fatigueLevel;
      s.hungerRaw = n.hungerRaw;
      s.coldRaw = n.coldRaw;
      s.fatigueRaw = n.fatigueRaw;
      s.warmth = p.warmthRating;

      // Combat (the published, debounced flag)
      s.inCombat = p.isInCombat;
      s.combatStartAgo = SecondsSince(n.combatStartAt, now);
      s.combatEndAgo = SecondsSince(n.combatEndAt, now);
      s.enemyCount = src.targets.GetEnemyCount();
      s.enemiesNear256 = src.targets.CountHostilesInRange(256.0f);
      if (const auto closest = src.targets.GetClosestEnemy()) {
         s.closestEnemy = closest->GetDistanceToPlayer();
      }
      s.anyCasting = src.targets.cachedAnyCasting;
      {
         std::optional<State::TargetActorState> target;
         const auto& primary = src.targets.primary;
         if (primary.has_value() && primary->isHostile && !primary->isDead) {
            target = primary;
         } else {
            target = src.targets.GetClosestEnemy();
         }
         if (target) {
            s.targetCaster = target->isMage;
            s.targetHealth = target->vitals.health;
         }
      }
      s.targetArcher = n.targetArcher;
      s.soleHostileTtk = n.soleHostileTtk;
      s.families = n.families;
      s.hostileSummoned = n.hostileSummoned;

      // Ally
      s.followerPresent = src.targets.GetFollowerCount() > 0;
      s.followerBleedout = n.followerBleedout;

      // Environment
      s.light = src.world.lightLevel;
      s.openDaylight = n.openDaylight;
      s.underwater = p.isUnderwater;
      s.submergedFor = (p.isUnderwater && n.submergedAt >= 0.0) ? SecondsSince(n.submergedAt, now) : 0.0f;
      s.swimming = p.isSwimming;
      s.fallDepth = p.fallDepth;
      s.dropAhead = n.dropAhead;
      s.waterDepthAhead = n.waterDepthAhead;
      s.lock = src.world.isLookingAtLock;
      s.workstation = WorkstationOf(n);
      s.oreVein = src.world.isLookingAtOreVein;
      s.merchant = n.merchant;

      // Equipment
      s.enchantedWeapon = p.hasEnchantedWeapon;
      s.weaponCharge = p.weaponChargePercent;
      s.bow = p.hasBowEquipped;
      s.crossbow = p.hasCrossbowEquipped;
      s.arrows = p.arrowCount;
      s.bolts = p.boltCount;
      s.melee = p.hasMeleeEquipped;
      s.spell = p.hasSpellEquipped;
      s.staff = p.hasStaffEquipped;
      s.shield = p.hasShieldEquipped;
      s.oneHanded = p.hasOneHandedEquipped;
      s.twoHanded = p.hasTwoHandedEquipped;
      s.handSchools = SchoolOf(p.rightHandSpell) | SchoolOf(p.leftHandSpell);

      // Activity
      s.sneaking = p.isSneaking;
      s.mounted = p.isMounted;
      return s;
   }

   LiveNeeds ReadLiveNeeds()
   {
      Huginn_ZONE_NAMED("Needs::ReadLiveNeeds");
      auto& sm = State::StateManager::GetSingleton();
      const auto player = sm.GetPlayerState();
      const auto targets = sm.GetTargets();
      const auto world = sm.GetWorldState();
      const auto health = sm.GetHealthTracking();
      const auto magicka = sm.GetMagickaTracking();
      const auto stamina = sm.GetStaminaTracking();
      const auto sensors = sm.GetNeedSensors();
      const float magickaHeld = g_utilityScorer ? g_utilityScorer->HeldMagicka(player.vitals.magicka) : player.vitals.magicka;
      const float staminaHeld = g_utilityScorer ? g_utilityScorer->HeldStamina(player.vitals.stamina) : player.vitals.stamina;
      LiveNeeds out;
      out.snapshot = BuildNeedSnapshot(NeedSources{ player, targets, world, health, magicka, stamina, sensors,
                                                    magickaHeld, staminaHeld, State::NeedClock::Now() });
      {
         Huginn_ZONE_NAMED("Needs::EvaluateNeeds");
         out.vector = Core::Needs::EvaluateNeeds(out.snapshot, NeedSettings::GetSingleton().GetCurves());
      }
      return out;
   }

   std::vector<std::string> FormatNeeds(const Core::Needs::NeedVector& v)
   {
      std::vector<std::string> lines;
      for (std::size_t i = 0; i < Core::Needs::kNeedCount; ++i) {
         if (Core::Needs::SignatureLevel(v.value[i]) == 0) continue;
         const auto& info = Core::Needs::kNeeds[i];
         lines.push_back(fmt::format("{:<24} {:.2f}  (input {:g}){}", info.id, v.value[i], v.input[i],
            info.deferred ? "  [deferred: no sensor]" : ""));
      }
      return lines;
   }

   std::string NeedsLine(const Core::Needs::NeedVector& v)
   {
      std::string out;
      for (std::size_t i = 0; i < Core::Needs::kNeedCount; ++i) {
         if (Core::Needs::SignatureLevel(v.value[i]) == 0) continue;
         if (!out.empty()) out += ' ';
         out += fmt::format("{}={:.2f}", Core::Needs::kNeeds[i].id, v.value[i]);
      }
      return out.empty() ? std::string("(none)") : out;
   }
}
