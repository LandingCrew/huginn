#include "HandModelProbe.h"

#include "DropAheadProbe.h"  // GameSeconds: the game clock
#include "Profiling.h"
#include "core/HandModelCheck.h"
#include "learning/EquipSourceTracker.h"

#include <array>
#include <atomic>
#include <format>
#include <functional>
#include <string_view>
#include <thread>

namespace Huginn::State::HandModelProbe
{
   namespace
   {
      std::atomic<bool> g_gameLoaded{ false };
      // Bumped by every SetGameLoaded; the hook resets its own state when it
      // sees the change, so that state is only ever touched on its thread.
      std::atomic<std::uint32_t> g_loadGeneration{ 0 };
      // The hook's thread; DescribeHands reads the tracked state only there.
      std::atomic<std::size_t> g_hookThread{ 0 };

      // Game seconds (DropAheadProbe::GameSeconds): they stop while the game
      // is paused, so a fault pending when a menu opens does not pass the
      // onset on the first tick after it closes.
      constexpr double kJudgeIntervalSec = 0.25;
      // How far back "last cast" / "last attack" look before calling it none.
      constexpr double kRecentSec = 30.0;
      // How far back a Huginn equip mark is looked for. The mark lands in the
      // same frame as the equip (EquipManager before it, the Wheeler callback
      // a millisecond after it, both before the next PlayerCharacter::Update),
      // so the window only has to cover the probe's own frame.
      constexpr float kHuginnEquipWindowMs = 1500.0f;
      // The parent walk's bound: the XPMSSE chain from WEAPON to the root is
      // well under this.
      constexpr int kMaxDepth = 256;

      enum Side : int { kRight = 0, kLeft = 1 };
      constexpr std::array<std::string_view, 2> kSideName{ "right", "left" };
      constexpr std::uint8_t kView1st = 1;
      constexpr std::uint8_t kView3rd = 3;

      struct Swap
      {
         std::string fromName;  // "" = empty hand
         double at = -1.0;      // game seconds; -1 = in the hand since the load
         bool huginn = false;
         Core::HandPose pose = Core::HandPose::Sheathed;
         double sinceCast = -1.0;    // seconds before the swap; -1 = none in kRecentSec
         double sinceAttack = -1.0;
      };

      struct Hand
      {
         RE::FormID object = 0;   // whatever the hand holds (weapon, spell, shield ...)
         std::string objectName;  // taken when it entered the hand
         Swap swap;
         Core::FaultWatch watch;
      };

      // Hook thread (main) only.
      std::array<Hand, 2> g_hands;
      std::uint32_t g_seenGeneration = 0;
      bool g_firstFrame = true;
      double g_loadedAt = -1.0;
      double g_lastJudge = -1.0;
      double g_lastCastAt = -1.0;
      double g_lastAttackAt = -1.0;

      std::size_t ThisThread() { return std::hash<std::thread::id>{}(std::this_thread::get_id()); }

      // The weapon in a hand, if it has a model to show; null otherwise
      // (spells, shields, torches, fists, model-less "unarmed" forms). The
      // left hand of a two-hander or bow, or a second copy of the right
      // hand's form, reads as null: one part, judged once, for the right hand
      // (so a missing left copy of a dual-wielded form is not seen).
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

      // No world to read: a load, the main menu (quit to the menu leaves the
      // player singleton alive, with no message to say so), no player 3D.
      bool WorldReady(RE::PlayerCharacter* player)
      {
         if (auto* ui = RE::UI::GetSingleton()) {
            if (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) return false;
         }
         return player->Get3D() != nullptr;
      }

      // ---- reading the biped ---------------------------------------------

      struct PartView
      {
         int slot = -1;
         Core::PartReading reading;
         std::string chain;   // the first parents, nearest first
         std::string culledAt;  // the culled node, if any
         bool reachedBipedRoot = false;  // passed the biped's own root (seen when not on the skeleton)
      };

      PartView ReadPart(const RE::BIPOBJECT& obj, int slot, const RE::NiAVObject* root3D, const RE::NiAVObject* bipedRoot)
      {
         PartView v;
         v.slot = slot;
         const RE::NiAVObject* part = obj.partClone.get();
         if (!part) return v;
         v.reading.found = true;
         if (part->GetAppCulled()) {
            v.reading.culled = true;
            v.culledAt = "the part";
         }
         int depth = 0;
         for (const RE::NiNode* node = part->parent; node && depth < kMaxDepth; node = node->parent, ++depth) {
            v.reading.attached = true;
            const std::string_view name = node->name.c_str();
            if (name == "WEAPON" || name == "SHIELD") v.reading.inHandNode = true;
            if (node->GetAppCulled() && !v.reading.culled) {
               v.reading.culled = true;
               v.culledAt = name.empty() ? std::string("(unnamed)") : std::string(name);
            }
            if (depth < 3) {
               if (!v.chain.empty()) v.chain += " < ";
               v.chain += name.empty() ? "(unnamed)" : std::string(name);
            }
            if (node == bipedRoot) v.reachedBipedRoot = true;
            if (node == root3D) {
               // Above here is the cell's scene graph, not the player. Only
               // the view's 3D counts: a biped root that is not under it is
               // a stale skeleton, the case this is here to catch.
               v.reading.onSkeleton = true;
               break;
            }
         }
         return v;
      }

      struct BipedView
      {
         bool hasBiped = false;
         std::vector<PartView> parts;  // every slot whose item is the weapon (or its first-person model)
         std::string weaponSlots;      // when no part: what the weapon slots hold instead
      };

      // `otherWeapon`: the other hand's weapon. Variants (an enchanted copy)
      // share their template's first-person model object, so a match on that
      // object alone would take the other hand's part for this one's.
      BipedView ReadBiped(RE::PlayerCharacter* player, bool firstPerson, const RE::TESObjectWEAP* weapon,
         const RE::TESObjectWEAP* otherWeapon)
      {
         BipedView v;
         const auto& biped = player->GetBiped(firstPerson);
         if (!biped) return v;
         v.hasBiped = true;
         const RE::NiAVObject* root3D = player->Get3D(firstPerson);
         const RE::TESForm* model1st = weapon->firstPersonModelObject;
         if (otherWeapon && otherWeapon->firstPersonModelObject == weapon->firstPersonModelObject) model1st = nullptr;
         for (int i = 0; i < RE::BIPED_OBJECTS::kTotal; ++i) {
            const auto* item = biped->objects[i].item;
            if (item && (item == weapon || item == model1st)) {
               v.parts.push_back(ReadPart(biped->objects[i], i, root3D, biped->root));
            }
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

      Core::ModelState Judge(const BipedView& v)
      {
         auto best = Core::ModelState::NoPart;
         for (const auto& p : v.parts) {
            const auto s = Core::JudgePart(p.reading);
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
            out += std::format("slot {} {}{}{}", p.slot,
               p.reading.attached ? "under " + p.chain : std::string("attached to nothing"),
               p.reading.attached && !p.reading.onSkeleton
                  ? (p.reachedBipedRoot ? ", under the biped's root but not the player's 3D" : ", not under the player's 3D")
                  : "",
               p.reading.culled ? ", culled at " + p.culledAt : std::string());
         }
         return out;
      }

      std::string Ago(double sec)
      {
         return sec < 0.0 ? std::format("none in the {:.0f} s before", kRecentSec) : std::format("{:.1f} s before", sec);
      }

      std::string DescribeSwap(const Swap& s, double nowSec)
      {
         if (s.at < 0.0) return "in the hand since the load";
         return std::format("in the hand {:.1f} s: swapped in from {} by {}, pose {}, last cast {}, last attack {}",
            nowSec - s.at, s.fromName.empty() ? std::string("an empty hand") : s.fromName,
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

            Swap swap;
            if (!g_firstFrame) {
               swap.fromName = hand.objectName;
               swap.at = nowSec;
               swap.huginn = id && Learning::EquipSourceTracker::GetSingleton().IsRecentHuginnEquip(id, kHuginnEquipWindowMs);
               swap.pose = pose;
               swap.sinceCast = SinceOrNone(g_lastCastAt, nowSec);
               swap.sinceAttack = SinceOrNone(g_lastAttackAt, nowSec);
            }
            hand.object = id;
            hand.objectName = obj ? FormLabel(obj) : std::string();
            hand.swap = std::move(swap);

            if (ModelledWeapon(obj, objects[1 - side], side)) {
               if (hand.swap.at < 0.0) {
                  logger::debug("[HandModel] {} hand at the load: {}"sv, kSideName[side], hand.objectName);
               } else {
                  logger::debug("[HandModel] {} hand: {} -> {} | by {} | pose {} | last cast {} | last attack {}"sv,
                     kSideName[side], hand.swap.fromName.empty() ? std::string("nothing") : hand.swap.fromName,
                     hand.objectName, hand.swap.huginn ? "Huginn" : "other", PoseName(pose),
                     Ago(hand.swap.sinceCast), Ago(hand.swap.sinceAttack));
               }
            }
         }
         g_firstFrame = false;
      }

      void LogEnded(int side, const Core::FaultWatch::Event& ev, std::string_view how)
      {
         logger::info("[HandModel] {} hand: the '{}' fault ended, {} after {:.1f} s ({} sheathes in between)"sv,
            kSideName[side], Core::ModelStateName(ev.state), how, ev.faultSec, ev.poseChanges);
      }

      void JudgeHands(RE::PlayerCharacter* player, double nowSec, Core::HandPose pose)
      {
         using K = Core::FaultWatch::EventKind;
         std::array<const RE::TESForm*, 2> objects{ player->GetEquippedObject(false), player->GetEquippedObject(true) };
         const bool firstPerson = InFirstPerson();
         const std::uint8_t view = firstPerson ? kView1st : kView3rd;
         for (int side = 0; side < 2; ++side) {
            auto& hand = g_hands[side];
            const auto* weapon = ModelledWeapon(objects[side], objects[1 - side], side);
            if (!weapon) {
               if (const auto ev = hand.watch.Update(0, pose, view, Core::ModelState::Ok, nowSec); ev.kind == K::SwappedOut) {
                  // The left hand also reads null when the right holds the same form.
                  LogEnded(side, ev, side == kLeft && objects[side] && objects[side] == objects[kRight]
                                        ? "no longer judged (the right hand holds the same form)"
                                        : "the weapon swapped out");
               }
               continue;
            }
            const auto* otherWeapon = ModelledWeapon(objects[1 - side], objects[side], 1 - side);

            // Only a drawn weapon is judged: skip the scene-graph reads otherwise.
            BipedView judged;
            auto state = Core::ModelState::Ok;
            if (pose == Core::HandPose::Drawn) {
               judged = ReadBiped(player, firstPerson, weapon, otherWeapon);
               state = Judge(judged);
            }
            const auto ev = hand.watch.Update(weapon->GetFormID(), pose, view, state, nowSec);
            if (ev.kind == K::Onset) {
               const BipedView other = ReadBiped(player, !firstPerson, weapon, otherWeapon);
               logger::warn("[HandModel] {} hand: {} is equipped and drawn, but its {}-person model is {} | 1st: {} | 3rd: {} | {} | {:.0f} s since the load"sv,
                  kSideName[side], hand.objectName, firstPerson ? "first" : "third", Core::ModelStateName(ev.state),
                  Describe(firstPerson ? judged : other), Describe(firstPerson ? other : judged),
                  DescribeSwap(hand.swap, nowSec), g_loadedAt >= 0.0 ? nowSec - g_loadedAt : -1.0);
            } else if (ev.kind == K::Recovered) {
               LogEnded(side, ev, "the model back in the hand");
            } else if (ev.kind == K::SwappedOut) {
               LogEnded(side, ev, "the weapon swapped out");
            }
         }
      }
   }

   void OnPlayerUpdate(RE::PlayerCharacter* player)
   {
      Huginn_ZONE_NAMED("HandModel::PlayerUpdateHook");
      if (!g_gameLoaded.load(std::memory_order_acquire) || !player) return;
      g_hookThread.store(ThisThread(), std::memory_order_relaxed);
      const double nowSec = DropAheadProbe::GameSeconds();
      if (const auto gen = g_loadGeneration.load(std::memory_order_acquire); gen != g_seenGeneration) {
         g_seenGeneration = gen;
         for (auto& hand : g_hands) hand = Hand{};
         g_firstFrame = true;
         g_loadedAt = nowSec;
         g_lastJudge = -1.0;
         g_lastCastAt = -1.0;
         g_lastAttackAt = -1.0;
      }
      if (!WorldReady(player)) return;

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
      if (!player || !WorldReady(player)) {
         rows.emplace_back("no world loaded");
         return rows;
      }
      // The tracked state (faults, swaps) belongs to the hook's thread; the
      // console is expected to run there too (the main thread), and reads
      // the live parts either way.
      const bool hookThread = ThisThread() == g_hookThread.load(std::memory_order_relaxed);
      const auto pose = PoseOf(player);
      const double nowSec = DropAheadProbe::GameSeconds();
      const bool firstPerson = InFirstPerson();
      rows.push_back(std::format("{}, {} person{}", PoseName(pose), firstPerson ? "first" : "third",
         hookThread ? "" : " (not on the probe's thread: fault and swap state not read)"));
      std::array<const RE::TESForm*, 2> objects{ player->GetEquippedObject(false), player->GetEquippedObject(true) };
      for (int side = 0; side < 2; ++side) {
         const auto* weapon = ModelledWeapon(objects[side], objects[1 - side], side);
         if (!weapon) {
            rows.push_back(std::format("{}: {} (not judged)", kSideName[side], FormLabel(objects[side])));
            continue;
         }
         const auto* otherWeapon = ModelledWeapon(objects[1 - side], objects[side], 1 - side);
         const BipedView first = ReadBiped(player, true, weapon, otherWeapon);
         const BipedView third = ReadBiped(player, false, weapon, otherWeapon);
         const std::string judged = pose == Core::HandPose::Drawn
            ? std::string(Core::ModelStateName(Judge(firstPerson ? first : third)))
            : std::format("not judged while {}", PoseName(pose));
         std::string tracked;
         if (hookThread) {
            const auto& hand = g_hands[side];
            tracked = std::format(" (probe: {}) | {}",
               hand.watch.Reported() ? "fault reported" : hand.watch.Pending() ? "fault pending" : "no fault",
               DescribeSwap(hand.swap, nowSec));
         }
         rows.push_back(std::format("{}: {} | {}{} | 1st: {} | 3rd: {}", kSideName[side], FormLabel(weapon), judged,
            tracked, Describe(first), Describe(third)));
      }
      return rows;
   }
}
