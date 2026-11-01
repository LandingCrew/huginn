#include "HandModelProbe.h"

#include "DropAheadProbe.h"  // GameSeconds: the game clock
#include "Profiling.h"
#include "core/HandEquipGate.h"  // CastPhaseName, CasterStateName
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
      // How far back a Huginn equip mark is looked for -- wider than
      // EquipSourceTracker::DEFAULT_WINDOW_MS (400 ms) on purpose. The mark
      // lands as the equip is asked for, but the hand changes when the
      // engine applies it: an equip queued behind an attack, a cast or a
      // draw lands when that animation ends, and EquipManager's hand swap
      // retries over up to two more frames. At 400 ms such a swap would read
      // "something else", the wrong answer to the question this line is
      // for. The mark is per form, so a wider window only misreads an equip
      // of the same form by another source within 1.5 s of a Huginn press
      // of it; the tracker's ring (8 marks) bounds it too.
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
         RE::WEAPON_STATE state = RE::WEAPON_STATE::kSheathed;
         double sinceCast = -1.0;    // seconds before the swap; -1 = none in kRecentSec
         std::string castWhat;       // the hand and caster state of that last cast
         double sinceAttack = -1.0;
         // The hand went empty because what it held was used up or removed:
         // the last scroll of a stack cast, a spell no longer known. The
         // graph may read magic for a moment after (JudgeGraph).
         bool emptiedByUse = false;
      };

      // How long after a hand is emptied by use an empty hand under the
      // magic hand type is not judged (JudgeGraph).
      constexpr double kEmptiedByUseGraceSec = 3.0;

      struct Hand
      {
         RE::FormID object = 0;   // whatever the hand holds (weapon, spell, shield ...)
         std::string objectName;  // taken when it entered the hand
         Swap swap;
         Core::FaultWatch watch;
         Core::GraphHandWatch graph{ Core::kGraphMismatchOnsetSec };  // the graph's hand type vs the hand (0.24.26)
      };

      // Hook thread (main) only.
      std::array<Hand, 2> g_hands;
      std::uint32_t g_seenGeneration = 0;
      bool g_firstFrame = true;
      double g_loadedAt = -1.0;
      double g_lastJudge = -1.0;
      double g_lastCastAt = -1.0;
      std::string_view g_lastCastWhat;  // its caster state (CastPhaseName)
      int g_lastCastSide = 0;
      double g_lastAttackAt = -1.0;
      Core::PoseStuckWatch g_stuck;
      // A frame without a world (WorldReady false) since the last one with
      // one: the next ready frame restarts the watches' pending timers.
      bool g_worldGap = false;

      std::size_t ThisThread() { return std::hash<std::thread::id>{}(std::this_thread::get_id()); }

      // The weapon in a hand, if it has a model to show; null otherwise
      // (spells, shields, torches, fists, model-less "unarmed" forms). The
      // left hand of a two-hander or bow, or a second copy of the right
      // hand's form, reads as null: judged once, for the right hand, on the
      // part under WEAPON (ReadBiped sets the left copy's aside by its node,
      // so a missing left copy of a dual-wielded form is not seen).
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

      RE::WEAPON_STATE WeaponState(RE::PlayerCharacter* player) { return player->AsActorState()->GetWeaponState(); }

      std::string_view WeaponStateName(RE::WEAPON_STATE state)
      {
         switch (state) {
         case RE::WEAPON_STATE::kSheathed: return "sheathed";
         case RE::WEAPON_STATE::kWantToDraw: return "want-to-draw";
         case RE::WEAPON_STATE::kDrawing: return "drawing";
         case RE::WEAPON_STATE::kDrawn: return "drawn";
         case RE::WEAPON_STATE::kWantToSheathe: return "want-to-sheathe";
         case RE::WEAPON_STATE::kSheathing: return "sheathing";
         }
         return "?";
      }

      // A caster state that is part of a cast, or "" when idle. The names
      // (and why 3 is "charged, held" and 4 "just released") live in
      // core/HandEquipGate.h; since 0.24.26 states 7-9 (CommonLib: unnamed,
      // "Interrupt", "Interrupt/Deselect") are named, not counted as idle.
      std::string_view CastPhaseName(RE::MagicCaster::State state)
      {
         return Core::CastPhaseName(static_cast<std::uint32_t>(state));
      }

      // The behaviour graph's view of the equip and the hands -- what the
      // weapon state machine waits on. "-" when the graph has no such variable.
      std::string GraphState(RE::PlayerCharacter* player)
      {
         auto b = [&](const char* name) -> std::string {
            bool v = false;
            return player->GetGraphVariableBool(name, v) ? (v ? "1" : "0") : "-";
         };
         auto i = [&](const char* name) -> std::string {
            std::int32_t v = 0;
            return player->GetGraphVariableInt(name, v) ? std::to_string(v) : "-";
         };
         return std::format("IsEquipping {} IsUnequipping {} IsAttacking {} IsCastingRight {} IsCastingLeft {} "
                            "iRightHandType {} iLeftHandType {} iEquippedItemState {}",
            b("IsEquipping"), b("IsUnequipping"), b("IsAttacking"), b("IsCastingRight"), b("IsCastingLeft"),
            i("iRightHandType"), i("iLeftHandType"), i("iEquippedItemState"));
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

      // Held as one item in both hands: the engine reports it in both, and
      // it hangs from one node (a drawn bow from SHIELD, the rest WEAPON).
      bool BothHanded(const RE::TESObjectWEAP* weapon)
      {
         return weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow();
      }

      // The node the hand's weapon hangs from when drawn, the same name in
      // both skeletons: WEAPON for the right hand, SHIELD for the left.
      Core::HandNode SideNode(int side) { return side == kRight ? Core::HandNode::Weapon : Core::HandNode::Shield; }

      // The node a part must hang under to be in the hand (Core::InOwnHandNode):
      // its own side's for a one-hander; either for a two-hander, bow or
      // staff, as before the sides were told apart.
      Core::HandNode InHandAccept(const RE::TESObjectWEAP* weapon, int side)
      {
         return (BothHanded(weapon) || weapon->IsStaff()) ? Core::HandNode::None : SideNode(side);
      }

      struct PartView
      {
         int slot = -1;
         Core::PartReading reading;
         Core::HandNode handNode = Core::HandNode::None;  // the nearest hand node among its parents
         std::string chain;   // the first parents, nearest first
         std::string culledAt;  // the culled node, if any
         bool reachedBipedRoot = false;  // passed the biped's own root (seen when not on the skeleton)
      };

      PartView ReadPart(const RE::BIPOBJECT& obj, int slot, const RE::NiAVObject* root3D, const RE::NiAVObject* bipedRoot,
         Core::HandNode accept)
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
            if (v.handNode == Core::HandNode::None) {
               if (name == "WEAPON") v.handNode = Core::HandNode::Weapon;
               else if (name == "SHIELD") v.handNode = Core::HandNode::Shield;
            }
            if (depth < 3) {
               if (!v.chain.empty()) v.chain += " < ";
               v.chain += name.empty() ? "(unnamed)" : std::string(name);
            }
            if (node == bipedRoot) v.reachedBipedRoot = true;
            if (node == root3D) {
               // Above here is the cell's scene graph, not the player. Only
               // the view's 3D counts: a biped root that is not under it is
               // a stale skeleton, the case this is here to catch. The root
               // itself is not tested for culling: only nodes strictly below
               // it count (a culled player root would read every part culled).
               v.reading.onSkeleton = true;
               break;
            }
            if (node->GetAppCulled() && !v.reading.culled) {
               v.reading.culled = true;
               v.culledAt = name.empty() ? std::string("(unnamed)") : std::string(name);
            }
         }
         v.reading.inHandNode = Core::InOwnHandNode(v.handNode, accept);
         return v;
      }

      struct BipedView
      {
         bool hasBiped = false;
         std::vector<PartView> parts;  // every slot whose item is the weapon (or its first-person model), this hand's
         int otherHandParts = 0;       // matches set aside as the other hand's (Core::PartIsHands)
         std::string weaponSlots;      // when no part: what the weapon slots hold instead
      };

      // `otherObject`: what the other hand holds, as the game reports it.
      // A part matches on the weapon or its first-person model object. The
      // match is ambiguous when the other hand holds the same form (two Iron
      // Daggers) or a variant sharing that model object (an enchanted copy):
      // the part is then sorted by the hand node it hangs under. Before, a
      // shared model object was not matched at all (a false "missing" for
      // both hands when the first-person biped holds the STAT) and the same
      // form was matched for both (the healthy left copy hid a missing right
      // one).
      BipedView ReadBiped(RE::PlayerCharacter* player, bool firstPerson, const RE::TESObjectWEAP* weapon, int side,
         const RE::TESForm* otherObject)
      {
         BipedView v;
         const auto& biped = player->GetBiped(firstPerson);
         if (!biped) return v;
         v.hasBiped = true;
         const RE::NiAVObject* root3D = player->Get3D(firstPerson);
         const RE::TESForm* model1st = weapon->firstPersonModelObject;
         const auto* otherWeapon = otherObject ? otherObject->As<RE::TESObjectWEAP>() : nullptr;
         // A two-hander or bow is one item reported in both hands, not two.
         const bool sameForm = otherObject == weapon && !BothHanded(weapon);
         const bool sharedModel = model1st && otherWeapon && otherWeapon != weapon &&
                                  otherWeapon->firstPersonModelObject == model1st;
         const auto own = SideNode(side);
         const auto accept = InHandAccept(weapon, side);
         for (int i = 0; i < RE::BIPED_OBJECTS::kTotal; ++i) {
            const auto* item = biped->objects[i].item;
            if (!item || (item != weapon && item != model1st)) continue;
            const bool ambiguous = sameForm || (item == model1st && sharedModel);
            PartView part = ReadPart(biped->objects[i], i, root3D, biped->root, accept);
            if (!Core::PartIsHands(part.handNode, own, ambiguous)) {
               ++v.otherHandParts;
               continue;
            }
            v.parts.push_back(std::move(part));
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
         const std::string others = v.otherHandParts > 0
            ? std::format(" ({} more under the other hand's node, counted as that hand's)", v.otherHandParts)
            : std::string();
         if (v.parts.empty()) {
            return (v.weaponSlots.empty() ? std::string("no part (weapon slots empty)")
                                          : std::format("no part (weapon slots hold {})", v.weaponSlots)) + others;
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
         return out + others;
      }

      std::string Ago(double sec)
      {
         return sec < 0.0 ? std::format("none in the {:.0f} s before", kRecentSec) : std::format("{:.1f} s before", sec);
      }

      std::string CastAgo(const Swap& s)
      {
         return s.sinceCast < 0.0 || s.castWhat.empty() ? Ago(s.sinceCast)
                                                        : std::format("{} ({})", Ago(s.sinceCast), s.castWhat);
      }

      std::string DescribeSwap(const Swap& s, double nowSec)
      {
         if (s.at < 0.0) return "in the hand since the load";
         return std::format("in the hand {:.1f} s: swapped in from {} by {}, pose {}, last cast {}, last attack {}",
            nowSec - s.at, s.fromName.empty() ? std::string("an empty hand") : s.fromName,
            s.huginn ? "Huginn (key or wheel)" : "something else (menu, favorites, another mod, the game)",
            WeaponStateName(s.state), CastAgo(s), Ago(s.sinceAttack));
      }

      // ---- the hook --------------------------------------------------------

      double SinceOrNone(double at, double nowSec)
      {
         return at >= 0.0 && nowSec - at <= kRecentSec ? nowSec - at : -1.0;
      }

      void TrackActivity(RE::PlayerCharacter* player, double nowSec)
      {
         // Every frame, before TrackSwaps: on the swap's own frame the caster
         // may already be reset, so the line reads what this kept from the
         // frame before.
         for (const auto source : { RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand }) {
            const auto* caster = player->GetMagicCaster(source);
            if (!caster) continue;
            if (const auto phase = CastPhaseName(caster->state.get()); !phase.empty()) {
               g_lastCastAt = nowSec;
               g_lastCastWhat = phase;
               g_lastCastSide = source == RE::MagicSystem::CastingSource::kRightHand ? kRight : kLeft;
            }
         }
         if (player->AsActorState()->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone) g_lastAttackAt = nowSec;
      }

      void TrackSwaps(RE::PlayerCharacter* player, double nowSec, RE::WEAPON_STATE state)
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
               swap.state = state;
               swap.sinceCast = SinceOrNone(g_lastCastAt, nowSec);
               if (swap.sinceCast >= 0.0) swap.castWhat = std::format("{} hand {}", kSideName[g_lastCastSide], g_lastCastWhat);
               swap.sinceAttack = SinceOrNone(g_lastAttackAt, nowSec);
               if (id == 0 && hand.object != 0) {
                  auto* before = RE::TESForm::LookupByID(hand.object);
                  auto* spell = before ? before->As<RE::SpellItem>() : nullptr;
                  swap.emptiedByUse = before && (before->Is(RE::FormType::Scroll) ||
                                                 (before->Is(RE::FormType::Spell) && spell && !player->HasSpell(spell)));
               }
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
                     hand.objectName, hand.swap.huginn ? "Huginn" : "other", WeaponStateName(state),
                     CastAgo(hand.swap), Ago(hand.swap.sinceAttack));
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

      // ---- the behaviour graph against the hand (0.24.26) ---------------

      // What the hand holds, for Core::JudgeGraphHand. Not judged (Other): a
      // scroll (its graph type is not checked here), a torch, a shield, the
      // unarmed weapon form (graph type 0 by right), and the left hand of a
      // two-hander, bow or crossbow (the engine reports it in both hands).
      Core::HandHeld HeldKind(const RE::TESForm* obj, const RE::TESForm* other, int side)
      {
         if (!obj) return Core::HandHeld::Empty;
         if (obj->Is(RE::FormType::Spell)) return Core::HandHeld::Spell;
         if (const auto* weapon = obj->As<RE::TESObjectWEAP>()) {
            if (weapon->IsHandToHandMelee()) return Core::HandHeld::Other;
            if (side == kLeft && obj == other && BothHanded(weapon)) return Core::HandHeld::Other;
            return Core::HandHeld::Weapon;
         }
         return Core::HandHeld::Other;
      }

      std::string LastCastText(double nowSec)
      {
         const double since = SinceOrNone(g_lastCastAt, nowSec);
         return since < 0.0 || g_lastCastWhat.empty()
            ? Ago(since)
            : std::format("{} ({} hand {})", Ago(since), kSideName[g_lastCastSide], g_lastCastWhat);
      }

      // Drawn and settled: the weapon state drawn and the graph neither
      // equipping nor unequipping. The watch sees anything else as not drawn,
      // which restarts an unreported mismatch (Core::BasicFaultWatch).
      void JudgeGraph(RE::PlayerCharacter* player, double nowSec)
      {
         using K = Core::GraphHandWatch::EventKind;
         bool equipping = false;
         bool unequipping = false;
         player->GetGraphVariableBool("IsEquipping", equipping);
         player->GetGraphVariableBool("IsUnequipping", unequipping);
         const bool drawn = WeaponState(player) == RE::WEAPON_STATE::kDrawn;
         const auto pose = !drawn                      ? Core::HandPose::Sheathed
                         : (equipping || unequipping) ? Core::HandPose::Moving
                                                      : Core::HandPose::Drawn;
         std::array<const RE::TESForm*, 2> objects{ player->GetEquippedObject(false), player->GetEquippedObject(true) };
         constexpr std::array<const char*, 2> kTypeVar{ "iRightHandType", "iLeftHandType" };
         for (int side = 0; side < 2; ++side) {
            auto& hand = g_hands[side];
            const auto held = HeldKind(objects[side], objects[1 - side], side);
            std::int32_t type = 0;
            const bool read = player->GetGraphVariableInt(kTypeVar[side], type);
            // Key 0 = not judged: it ends a reported mismatch ("swapped out").
            const std::uint32_t key = (held == Core::HandHeld::Other || !read) ? 0
                                    : held == Core::HandHeld::Empty            ? Core::kEmptyHandKey
                                                                               : objects[side]->GetFormID();
            auto state = read ? Core::JudgeGraphHand(held, type) : Core::GraphHand::Agrees;
            // A scroll stack cast to nothing, or a spell removed: the graph
            // gets a moment to follow before an empty hand under magic counts.
            if (state == Core::GraphHand::EmptyAsMagic && hand.swap.emptiedByUse && hand.swap.at >= 0.0 &&
                nowSec - hand.swap.at < kEmptiedByUseGraceSec) {
               state = Core::GraphHand::Agrees;
            }
            const auto ev = hand.graph.Update(key, pose, 0, state, nowSec);
            if (ev.kind == K::Onset) {
               logger::warn("[HandModel] {} hand: the behaviour graph disagrees with the hand for {:.1f} s while drawn "
                            "and settled -- {} | holds {} | graph: {} | {} | last cast now {} | {:.0f} s since the load"sv,
                  kSideName[side], ev.faultSec, Core::GraphHandName(ev.state), FormLabel(objects[side]),
                  GraphState(player), DescribeSwap(hand.swap, nowSec), LastCastText(nowSec),
                  g_loadedAt >= 0.0 ? nowSec - g_loadedAt : -1.0);
            } else if (ev.kind == K::Recovered || ev.kind == K::SwappedOut) {
               logger::info("[HandModel] {} hand: the graph mismatch ({}) ended, {} after {:.1f} s ({} sheathes in between) | graph: {}"sv,
                  kSideName[side], Core::GraphHandName(ev.state),
                  ev.kind == K::Recovered ? "the graph back in step with the hand"
                                          : "the hand's content changed (or is no longer judged)",
                  ev.faultSec, ev.poseChanges, GraphState(player));
            }
         }
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
               const RE::FormID watched = hand.watch.FormID();
               if (const auto ev = hand.watch.Update(0, pose, view, Core::ModelState::Ok, nowSec); ev.kind == K::SwappedOut) {
                  // The left hand also reads null when the right holds the
                  // same form -- but only the faulted weapon itself, now in
                  // both hands, is "no longer judged". A two-hander or bow
                  // that replaced it is a swap.
                  const bool stillHeld = side == kLeft && objects[side] && objects[side] == objects[kRight] &&
                                         objects[side]->GetFormID() == watched;
                  LogEnded(side, ev, stillHeld ? "no longer judged (the right hand holds the same form)"
                                               : "the weapon swapped out");
               }
               continue;
            }
            const auto* otherObject = objects[1 - side];

            // Only a drawn weapon is judged: skip the scene-graph reads otherwise.
            BipedView judged;
            auto state = Core::ModelState::Ok;
            if (pose == Core::HandPose::Drawn) {
               judged = ReadBiped(player, firstPerson, weapon, side, otherObject);
               state = Judge(judged);
            }
            const auto ev = hand.watch.Update(weapon->GetFormID(), pose, view, state, nowSec);
            if (ev.kind == K::Onset) {
               const BipedView other = ReadBiped(player, !firstPerson, weapon, side, otherObject);
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

   namespace
   {
      // Both hands' parts, where they hang, for the stuck line.
      std::string DescribeHandParts(RE::PlayerCharacter* player)
      {
         std::array<const RE::TESForm*, 2> objects{ player->GetEquippedObject(false), player->GetEquippedObject(true) };
         std::string out;
         for (int side = 0; side < 2; ++side) {
            if (!out.empty()) out += " | ";
            const auto* weapon = ModelledWeapon(objects[side], objects[1 - side], side);
            if (!weapon) {
               out += std::format("{}: {}", kSideName[side], FormLabel(objects[side]));
               continue;
            }
            const auto* other = objects[1 - side];
            out += std::format("{}: {} 1st: {}; 3rd: {}; {}", kSideName[side], g_hands[side].objectName,
               Describe(ReadBiped(player, true, weapon, side, other)),
               Describe(ReadBiped(player, false, weapon, side, other)),
               DescribeSwap(g_hands[side].swap, DropAheadProbe::GameSeconds()));
         }
         return out;
      }

      void WatchStuck(RE::PlayerCharacter* player, double nowSec, RE::WEAPON_STATE state)
      {
         using K = Core::PoseStuckWatch::EventKind;
         const auto ev = g_stuck.Update(static_cast<std::uint8_t>(state), nowSec);
         if (ev.kind == K::Stuck) {
            logger::warn("[HandModel] weapon state stuck in {}, {:.1f} s without settling drawn or sheathed ({} person) | graph: {} | {} | {:.0f} s since the load"sv,
               WeaponStateName(state), ev.heldSec, InFirstPerson() ? "first" : "third", GraphState(player),
               DescribeHandParts(player), g_loadedAt >= 0.0 ? nowSec - g_loadedAt : -1.0);
         } else if (ev.kind == K::Settled) {
            // Timed from the last settled state: a draw that flapped between
            // moving states counts whole (Core::PoseStuckWatch).
            logger::info("[HandModel] weapon state settled {} after {:.1f} s unsettled (reported stuck in {}) | graph: {}"sv,
               WeaponStateName(state), ev.heldSec, WeaponStateName(static_cast<RE::WEAPON_STATE>(ev.stuckIn)),
               GraphState(player));
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
         g_lastCastWhat = {};
         g_lastAttackAt = -1.0;
         g_stuck.Reset();
         g_worldGap = false;
      }
      if (!WorldReady(player)) {
         g_worldGap = true;
         return;
      }
      if (g_worldGap) {
         // Back from a door, fast travel or a stretch without the player's 3D
         // (no load message): the game clock ran on through it, so pending
         // onsets start over here. Reported faults stand (Restart).
         g_worldGap = false;
         for (auto& hand : g_hands) {
            hand.watch.Restart();
            hand.graph.Restart();
         }
         g_stuck.Restart();
      }

      const auto state = WeaponState(player);
      const auto pose = PoseOf(player);
      TrackActivity(player, nowSec);
      TrackSwaps(player, nowSec, state);
      WatchStuck(player, nowSec, state);
      if (g_lastJudge >= 0.0 && nowSec - g_lastJudge < kJudgeIntervalSec) return;
      g_lastJudge = nowSec;
      JudgeHands(player, nowSec, pose);
      JudgeGraph(player, nowSec);
   }

   void SetGameLoaded(bool loaded) noexcept
   {
      g_loadGeneration.fetch_add(1, std::memory_order_acq_rel);
      g_gameLoaded.store(loaded, std::memory_order_release);
   }

   std::string DescribeForEquip(RE::PlayerCharacter* player)
   {
      if (!player) return "no player";
      auto caster = [&](RE::MagicSystem::CastingSource source) -> std::string {
         const auto* c = player->GetMagicCaster(source);
         if (!c) return "no caster";
         const auto state = static_cast<std::uint32_t>(c->state.get());
         const auto* spell = c->currentSpell;
         return std::format("{} ({})", Core::CasterStateName(state), spell ? FormLabel(spell) : std::string("no spell"));
      };
      const auto* main = RE::Main::GetSingleton();
      return std::format("casters: right {}, left {} | weapon state {} | graph: {} | thread {} (the game's main thread: {})",
         caster(RE::MagicSystem::CastingSource::kRightHand), caster(RE::MagicSystem::CastingSource::kLeftHand),
         WeaponStateName(WeaponState(player)), GraphState(player), SKSE::WinAPI::GetCurrentThreadID(),
         main ? std::to_string(main->threadID) : std::string("?"));
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
      rows.push_back(std::format("{}{}, {} person{} | graph: {}", WeaponStateName(WeaponState(player)),
         hookThread ? std::format(" for {:.1f} s", g_stuck.HeldSec(nowSec)) : std::string(),
         firstPerson ? "first" : "third",
         hookThread ? "" : " (not on the probe's thread: fault and swap state not read)", GraphState(player)));
      std::array<const RE::TESForm*, 2> objects{ player->GetEquippedObject(false), player->GetEquippedObject(true) };
      for (int side = 0; side < 2; ++side) {
         const auto* weapon = ModelledWeapon(objects[side], objects[1 - side], side);
         if (!weapon) {
            rows.push_back(std::format("{}: {} (not judged)", kSideName[side], FormLabel(objects[side])));
            continue;
         }
         const auto* otherObject = objects[1 - side];
         const BipedView first = ReadBiped(player, true, weapon, side, otherObject);
         const BipedView third = ReadBiped(player, false, weapon, side, otherObject);
         const std::string judged = pose == Core::HandPose::Drawn
            ? std::string(Core::ModelStateName(Judge(firstPerson ? first : third)))
            : std::format("not judged while {}", WeaponStateName(WeaponState(player)));
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
