// =============================================================================
// StateManager_ActorLog.cpp - the [Target] and [Actors] logs (0.23.39)
// =============================================================================
// Part of the StateManager implementation split. Logging only: nothing here
// feeds a score. The pure state (what changed, the fight summary, the seen
// set) is core/ActorLog.h; this file reads the actors' identities -- name,
// base form, race, summon and commander, all perceivable (CLAUDE.md
// "Forbidden Information"; no level, inventory or spell list) -- and logs.
//
// [Target]: on a change of the scoring target's actor or type that holds
//           0.3 s (core/ActorLog.h TargetWatch).
// [Actors]: who was in the fight, transitions only; see core/ActorLog.h.
// `hg actors`: DescribeTrackedActors.
// =============================================================================

#include "../PCH.h"
#include "StateManager.h"
#include "StateConstants.h"

namespace Huginn::State
{
   namespace
   {
      std::string SafeString(const char* s)
      {
         return (s && *s) ? std::string(s) : std::string();
      }

      // Who an actor is, perceivably. Rare: once per actor per fight, once per
      // scoring-target change, once per `hg actors` row.
      Core::ActorLog::Identity Identify(RE::FormID refID)
      {
         Core::ActorLog::Identity id;
         auto* actor = RE::TESForm::LookupByID<RE::Actor>(refID);
         if (!actor) {
            id.name = "?";
            return id;
         }
         id.name = SafeString(actor->GetDisplayFullName());
         if (id.name.empty()) id.name = "?";
         if (const auto* base = actor->GetActorBase()) {
            id.baseID = base->GetFormID();
         }
         if (const auto* race = actor->GetRace()) {
            id.raceEditorID = SafeString(race->GetFormEditorID());
            id.raceName = SafeString(race->GetFullName());
         }
         id.summon = actor->IsCommandedActor() || actor->IsSummoned();
         if (id.summon) {
            if (const auto commander = actor->GetCommandingActor()) {
               id.commander = SafeString(commander->GetDisplayFullName());
            }
         }
         return id;
      }
   }

   void StateManager::LogTargetsAndActors(double nowSec, bool fighting, const ScoringPick& pick)
   {
      // Logged once the new pick has held TargetWatch::DWELL_SEC; the brief
      // ones are counted onto the next line.
      if (m_targetWatch.Observe(nowSec, pick.refID, pick.type)) {
         Core::ActorLog::TargetInfo info;
         info.refID = pick.refID;
         info.type = pick.type;
         if (pick.refID != 0) {
            const auto id = Identify(pick.refID);
            info.name = id.name;
            info.baseID = id.baseID;
            info.raceEditorID = id.raceEditorID;
            info.raceName = id.raceName;
            info.summon = id.summon;
            info.commander = id.commander;
            info.rule = pick.rule;
            info.hostile = pick.hostile;
            info.dead = pick.dead;
         }
         logger::info("{}"sv, m_targetWatch.Commit(std::move(info)));
      }

      if (!fighting && !m_actorLog.InFight()) {
         return;
      }
      std::vector<std::string> lines;
      m_actorLog.Poll(nowSec, fighting, m_actorObs, pick.refID, pick.type, Identify, lines);
      for (const auto& line : lines) {
         logger::info("{}"sv, line);
      }
   }

   std::vector<std::string> StateManager::DescribeTrackedActors() const
   {
      const TargetCollection targets = GetTargets();

      // The scoring target, picked as ScoringTargetType picks it.
      RE::FormID scoringRef = 0;
      if (targets.primary.has_value() && targets.primary->isHostile && !targets.primary->isDead) {
         scoringRef = targets.primary->actorFormID;
      } else if (const auto enemy = targets.GetClosestEnemy()) {
         scoringRef = enemy->actorFormID;
      }

      std::vector<std::string> rows;
      rows.reserve(targets.targets.size() + 1);
      rows.push_back(std::format("{} tracked actor(s), combat {}, scoring target {}",
         targets.targets.size(), IsInCombat() ? "on" : "off",
         scoringRef ? std::format("{:08X}", scoringRef) : std::string("none")));
      for (const auto& t : targets.targets) {
         Core::ActorLog::Observation o;
         o.refID = t.actorFormID;
         o.hostile = t.isHostile;
         o.type = t.targetType;
         o.band = Core::ActorLog::BandOf(t.distanceToPlayerSq,
            DistanceThresholds::EVAL_MELEE_MAX_SQ, DistanceThresholds::EVAL_MID_MAX_SQ);
         const auto* actor = RE::TESForm::LookupByID<RE::Actor>(t.actorFormID);
         o.dead = t.isDead || (actor && actor->IsDead());
         rows.push_back(Core::ActorLog::FormatRow(o, Identify(t.actorFormID), t.actorFormID == scoringRef));
      }
      return rows;
   }
}
