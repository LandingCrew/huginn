#include "HandModelProbe.h"

#include "NeedSensorState.h"  // NeedClock
#include "Profiling.h"
#include "core/HandModelCheck.h"
#include "learning/EquipSourceTracker.h"

#include <array>
#include <atomic>
#include <format>
#include <string_view>

namespace Huginn::State::HandModelProbe
{
   namespace
   {
      std::atomic<bool> g_gameLoaded{ false };
      // Bumped by every SetGameLoaded; the hook resets its own state when it
      // sees the change, so that state is only ever touched on its thread.
      std::atomic<std::uint32_t> g_loadGeneration{ 0 };

      constexpr double kJudgeIntervalSec = 0.25;
      // How far back "last cast" / "last attack" look before calling it none.
      constexpr double kRecentSec = 30.0;
      // EquipSourceTracker marks precede the hand change (Huginn's equip) or
      // follow it by a frame (the Wheeler callback); this covers both.
      constexpr float kHuginnEquipWindowMs = 1500.0f;

      enum Side : int { kRight = 0, kLeft = 1 };
      constexpr std::array<std::string_view, 2> kSideName{ "right", "left" };

      struct Swap
      {
         std::string fromName;  // "" = empty hand
         RE::FormID from = 0;
         double at = -1.0;      // NeedClock seconds; -1 = in the hand since the load
         bool huginn = false;
         Core::HandPose pose = Core::HandPose::Sheathed;
         double sinceCast = -1.0;    // seconds before the swap; -1 = none in kRecentSec
         double sinceAttack = -1.0;
      };

      struct Hand
      {
         RE::FormID object = 0;  // whatever the hand holds (weapon, spell, shield ...)
         Swap swap;
         Core::FaultWatch watch;
      };

      // Hook thread (main) only; DescribeHands reads it from the console,
      // also the main thread.
      std::array<Hand, 2> g_hands;
      std::uint32_t g_seenGeneration = 0;
      bool g_firstFrame = true;
      double g_loadedAt = -1.0;
      double g_lastJudge = -1.0;
      double g_lastCastAt = -1.0;
      double g_lastAttackAt = -1.0;

      // The weapon in a hand, if it has a model to show; null otherwise
      // (spells, shields, torches, fists, model-less "unarmed" forms). The
      // left hand of a two-hander, or a second copy of the right hand's
      // form, reads as null: one part, judged once, for the right hand.
      const RE::TESObjectWEAP* ModelledWeapon(const RE::TESForm* obj, const RE::TESForm* other, int side)
      {
         const auto* weapon = obj ? obj->As<RE::TESObjectWEAP>() : nullptr;
         if (!weapon) return nullptr;
         if (side == kLeft && obj == other) return nullptr;
         const char* model = weapon->GetModel();
         return model && *model ? weapon : nullptr;
      }

      Core::HandPose PoseOf(RE::PlayerCharacter* player)
      {
         switch (player->AsActorState()->GetWeaponState()) {
         case RE::WEAPON_STATE::kDrawn: return Core::HandPose::Drawn;
         case RE::WEAPON_STATE::kSheathed: return Core::HandPose::Sheathed;
         default: return Core::HandPose::Moving;
         }
      }

      std::string_view PoseName(Core::HandPose pose)
      {
         switch (pose) {
         case Core::HandPose::Drawn: return "drawn";
         case Core::HandPose::Sheathed: return "sheathed";
         case Core::HandPose::Moving: return "drawing/sheathing";
         }
         return "?";
      }

      bool InFirstPerson()
      {
         auto* camera = RE::PlayerCamera::GetSingleton();
         return camera && camera->IsInFirstPerson();
      }

      std::string FormLabel(const RE::TESForm* form)
      {
         if (!form) return "nothing";
         const char* name = form->GetName();
         return std::format("'{}' ({:08X})", name && *name ? name : "?", form->GetFormID());
      }

      // ---- reading the biped ---------------------------------------------

      struct PartView
      {
         int slot = -1;
         Core::PartReading reading;
         std::string chain;  // the first parents, nearest first
      };

      PartView ReadPart(const RE::BIPOBJECT& obj, int slot)
      {
         PartView v;
         v.slot = slot;
         const RE::NiAVObject* part = obj.partClone.get();
         if (!part) return v;
         v.reading.found = true;
         v.reading.culled = part->GetAppCulled();
         int depth = 0;
         for (const RE::NiNode* node = part->parent; node && depth < 8; node = node->parent, ++depth) {
            v.reading.attached = true;
            const std::string_view name = node->name.c_str();
            if (name == "WEAPON" || name == "SHIELD") v.reading.inHandNode = true;
            if (depth < 3) {
               if (!v.chain.empty()) v.chain += " < ";
               v.chain += name.empty() ? "(unnamed)" : std::string(name);
            }
         }
         return v;
      }

      struct BipedView
      {
         bool hasBiped = false;
         std::vector<PartView> parts;  // every slot whose item is the weapon
         std::string weaponSlots;      // when no part: what the weapon slots hold instead
      };

      BipedView ReadBiped(RE::PlayerCharacter* player, bool firstPerson, const RE::TESForm* weapon)
      {
         BipedView v;
         const auto& biped = player->GetBiped(firstPerson);
         if (!biped) return v;
         v.hasBiped = true;
         for (int i = 0; i < RE::BIPED_OBJECTS::kTotal; ++i) {
            if (biped->objects[i].item == weapon) v.parts.push_back(ReadPart(biped->objects[i], i));
         }
         if (v.parts.empty()) {
            // A stale part (the last weapon's) would show here.
            for (int i = RE::BIPED_OBJECTS::kHandToHandMelee; i <= RE::BIPED_OBJECTS::kQuiver; ++i) {
               if (const auto* item = biped->objects[i].item) {
                  if (!v.weaponSlots.empty()) v.weaponSlots += ", ";
                  v.weaponSlots += std::format("{} {}", i, FormLabel(item));
               }
            }
         }
         return v;
      }

      Core::ModelState Judge(const BipedView& v, Core::HandPose pose)
      {
         auto best = Core::ModelState::NoPart;
         for (const auto& p : v.parts) {
            const auto s = Core::JudgePart(p.reading, pose);
            if (Core::ModelStateRank(s) < Core::ModelStateRank(best)) best = s;
         }
         return best;
      }

      std::string Describe(const BipedView& v)
      {
         if (!v.hasBiped) return "no biped";
         if (v.parts.empty()) {
            return v.weaponSlots.empty() ? std::string("no part (weapon slots empty)")
                                         : std::format("no part (weapon slots hold {})", v.weaponSlots);
         }
         std::string out;
         for (const auto& p : v.parts) {
            if (!out.empty()) out += "; ";
            if (!p.reading.found) {
               out += std::format("slot {} without a model", p.slot);
               continue;
            }
            out += std::format("slot {} {}{}", p.slot,
               p.reading.attached ? "under " + p.chain : std::string("attached to nothing"),
               p.reading.culled ? ", culled" : "");
         }
         return out;
      }

      // Drawn, the model the player sees is the current view's; sheathed,
      // the first-person biped's sheath is never on screen, so the third.
      bool JudgedView(Core::HandPose pose) { return pose == Core::HandPose::Drawn && InFirstPerson(); }

      std::string Ago(double sec)
      {
         return sec < 0.0 ? std::format("none in the {:.0f} s before", kRecentSec) : std::format("{:.1f} s before", sec);
      }

      std::string DescribeSwap(const Swap& s, double nowSec)
      {
         if (s.at < 0.0) return "in the hand since the load";
         return std::format("in the hand {:.1f} s: swapped in from {} by {}, pose {}, last cast {}, last attack {}",
            nowSec - s.at, s.from ? s.fromName : std::string("an empty hand"),
            s.huginn ? "Huginn (key or wheel)" : "something else (menu, favorites, another mod, the game)",
            PoseName(s.pose), Ago(s.sinceCast), Ago(s.sinceAttack));
      }

      // ---- the hook --------------------------------------------------------

      double SinceOrNone(double at, double nowSec)
      {
         return at >= 0.0 && nowSec - at <= kRecentSec ? nowSec - at : -1.0;
      }

      void TrackActivity(RE::PlayerCharacter* player, double nowSec)
      {
         for (const auto source : { RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand }) {
            const auto* caster = player->GetMagicCaster(source);
            if (!caster) continue;
            const auto state = caster->state.get();
            if (state == RE::MagicCaster::State::kCharging || state == RE::MagicCaster::State::kCasting) {
               g_lastCastAt = nowSec;
            }
         }
         if (player->AsActorState()->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone) g_lastAttackAt = nowSec;
      }

      void TrackSwaps(RE::PlayerCharacter* player, double nowSec, Core::HandPose pose)
      {
         std::array<const RE::TESForm*, 2> objects{ player->GetEquippedObject(false), player->GetEquippedObject(true) };
         for (int side = 0; side < 2; ++side) {
            auto& hand = g_hands[side];
            const auto* obj = objects[side];
            const RE::FormID id = obj ? obj->GetFormID() : 0;
            if (id == hand.object && !g_firstFrame) continue;

            const RE::TESForm* before = hand.object ? RE::TESForm::LookupByID(hand.object) : nullptr;
            Swap swap;
            if (!g_firstFrame) {
               swap.from = hand.object;
               swap.fromName = FormLabel(before);
               swap.at = nowSec;
               swap.huginn = id && Learning::EquipSourceTracker::GetSingleton().IsRecentHuginnEquip(id, kHuginnEquipWindowMs);
               swap.pose = pose;
               swap.sinceCast = SinceOrNone(g_lastCastAt, nowSec);
               swap.sinceAttack = SinceOrNone(g_lastAttackAt, nowSec);
            }
            hand.object = id;
            hand.swap = std::move(swap);

            if (ModelledWeapon(obj, objects[1 - side], side)) {
               if (hand.swap.at < 0.0) {
                  logger::debug("[HandModel] {} hand at the load: {}"sv, kSideName[side], FormLabel(obj));
               } else {
                  logger::debug("[HandModel] {} hand: {} -> {} | by {} | pose {} | last cast {} | last attack {}"sv,
                     kSideName[side], hand.swap.from ? hand.swap.fromName : std::string("nothing"), FormLabel(obj),
                     hand.swap.huginn ? "Huginn" : "other", PoseName(pose), Ago(hand.swap.sinceCast),
                     Ago(hand.swap.sinceAttack));
               }
            }
         }
         g_firstFrame = false;
      }

      void JudgeHands(RE::PlayerCharacter* player, double nowSec, Core::HandPose pose)
      {
         std::array<const RE::TESForm*, 2> objects{ player->GetEquippedObject(false), player->GetEquippedObject(true) };
         for (int side = 0; side < 2; ++side) {
            auto& hand = g_hands[side];
            const auto* weapon = ModelledWeapon(objects[side], objects[1 - side], side);
            if (!weapon) {
               if (const auto ev = hand.watch.Update(0, pose, Core::ModelState::Ok, nowSec);
                   ev.kind == Core::FaultWatch::EventKind::SwappedOut) {
                  logger::info("[HandModel] {} hand: the {} model fault ended with the weapon swapped out after {:.1f} s ({} draw/sheathe changes)"sv,
                     kSideName[side], Core::ModelStateName(ev.state), ev.faultSec, ev.poseChanges);
               }
               continue;
            }

            // Moving is not judged: skip the scene-graph reads.
            const bool firstPerson = JudgedView(pose);
            BipedView judged;
            auto state = Core::ModelState::Ok;
            if (pose != Core::HandPose::Moving) {
               judged = ReadBiped(player, firstPerson, weapon);
               state = Judge(judged, pose);
            }
            const auto ev = hand.watch.Update(weapon->GetFormID(), pose, state, nowSec);
            using K = Core::FaultWatch::EventKind;
            if (ev.kind == K::Onset) {
               const BipedView other = ReadBiped(player, !firstPerson, weapon);
               const double sinceLoad = g_loadedAt >= 0.0 ? nowSec - g_loadedAt : -1.0;
               logger::warn("[HandModel] {} hand: {} is equipped, but its model is {} | {}, {} person | judged {}: {} | {}: {} | {} | {:.0f} s since the load"sv,
                  kSideName[side], FormLabel(weapon), Core::ModelStateName(ev.state), PoseName(pose),
                  InFirstPerson() ? "first" : "third", firstPerson ? "1st" : "3rd", Describe(judged),
                  firstPerson ? "3rd" : "1st", Describe(other), DescribeSwap(hand.swap, nowSec), sinceLoad);
            } else if (ev.kind == K::Recovered) {
               logger::info("[HandModel] {} hand: {} model back after {:.1f} s ({}; {} draw/sheathe changes)"sv,
                  kSideName[side], FormLabel(weapon), ev.faultSec, Core::ModelStateName(ev.state), ev.poseChanges);
            } else if (ev.kind == K::SwappedOut) {
               logger::info("[HandModel] {} hand: the {} model fault ended with the weapon swapped out after {:.1f} s ({} draw/sheathe changes)"sv,
                  kSideName[side], Core::ModelStateName(ev.state), ev.faultSec, ev.poseChanges);
            }
         }
      }
   }

   void OnPlayerUpdate(RE::PlayerCharacter* player)
   {
      Huginn_ZONE_NAMED("HandModel::PlayerUpdateHook");
      if (!g_gameLoaded.load(std::memory_order_acquire) || !player) return;
      const double nowSec = NeedClock::Now();
      if (const auto gen = g_loadGeneration.load(std::memory_order_acquire); gen != g_seenGeneration) {
         g_seenGeneration = gen;
         for (auto& hand : g_hands) hand = Hand{};
         g_firstFrame = true;
         g_loadedAt = nowSec;
         g_lastJudge = -1.0;
         g_lastCastAt = -1.0;
         g_lastAttackAt = -1.0;
      }
      if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return;
      if (!player->Get3D()) return;

      const auto pose = PoseOf(player);
      TrackActivity(player, nowSec);
      TrackSwaps(player, nowSec, pose);
      if (g_lastJudge >= 0.0 && nowSec - g_lastJudge < kJudgeIntervalSec) return;
      g_lastJudge = nowSec;
      JudgeHands(player, nowSec, pose);
   }

   void SetGameLoaded(bool loaded) noexcept
   {
      g_loadGeneration.fetch_add(1, std::memory_order_acq_rel);
      g_gameLoaded.store(loaded, std::memory_order_release);
   }

   std::vector<std::string> DescribeHands(RE::PlayerCharacter* player)
   {
      std::vector<std::string> rows;
      if (!player) {
         rows.emplace_back("no player");
         return rows;
      }
      const auto pose = PoseOf(player);
      const double nowSec = NeedClock::Now();
      rows.push_back(std::format("{}, {} person", PoseName(pose), InFirstPerson() ? "first" : "third"));
      std::array<const RE::TESForm*, 2> objects{ player->GetEquippedObject(false), player->GetEquippedObject(true) };
      for (int side = 0; side < 2; ++side) {
         const auto* weapon = ModelledWeapon(objects[side], objects[1 - side], side);
         if (!weapon) {
            rows.push_back(std::format("{}: {} (not judged)", kSideName[side], FormLabel(objects[side])));
            continue;
         }
         const BipedView first = ReadBiped(player, true, weapon);
         const BipedView third = ReadBiped(player, false, weapon);
         std::string judged = "not judged while drawing/sheathing";
         if (pose != Core::HandPose::Moving) {
            judged = std::string(Core::ModelStateName(Judge(JudgedView(pose) ? first : third, pose)));
         }
         const auto& watch = g_hands[side].watch;
         rows.push_back(std::format("{}: {} | {} (probe: {}) | 1st: {} | 3rd: {} | {}", kSideName[side],
            FormLabel(weapon), judged,
            watch.Reported() ? "fault reported" : watch.Pending() ? "fault pending" : "no fault",
            Describe(first), Describe(third), DescribeSwap(g_hands[side].swap, nowSec)));
      }
      return rows;
   }
}
