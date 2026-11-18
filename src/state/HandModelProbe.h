#pragma once

// =============================================================================
// HAND MODEL PROBE -- the game half of core/HandModelCheck.h (0.23.44)
// =============================================================================
// Diagnostic only: nothing reads it but the log and `hg hands`. For each hand
// holding a weapon with a model, finds the biped part the engine attached for
// it (BipedAnim::objects[i] with item == the weapon or its first-person model
// object, its partClone, and the parents up to the player's 3D root) and
// judges it (Core::JudgePart) while the weapon is drawn, in the biped of the
// current view (the first-person one in first person: the model the player
// sees). Sheathed is not judged. When the other hand holds the same form or
// a variant sharing that model object, a part goes to the hand whose node
// (WEAPON right, SHIELD left) it hangs under (Core::PartIsHands).
//
// Where it runs: from the PlayerCharacter::Update hook DropAheadProbe installs,
// on the main thread (the scene graph is the main thread's), every frame for
// the hand contents and every 250 game-ms for the models; timed on the game
// clock (DropAheadProbe::GameSeconds), which stops while paused. On between
// kPostLoadGame / kNewGame and kPreLoadGame, and never while a LoadingMenu or
// the MainMenu is open or the player has no 3D; the first frame after such a
// gap restarts the pending (unreported) fault and stuck timers.
//
// Logs, under [HandModel]:
//   - debug, each change of the weapon in a hand, with what it replaced,
//     whether Huginn made the equip (key or wheel), the pose, and how long
//     before it the player last cast or attacked -- the base rate the faults
//     are read against;
//   - warn, a fault that held FaultWatch::kOnsetSec, with both bipeds' parts
//     and that same swap context;
//   - info, its end: the model back, or the weapon swapped out;
//   - warn / info (0.24.26), the behaviour graph against the hand
//     (Core::JudgeGraphHand): drawn and settled (not equipping) for
//     kGraphMismatchOnsetSec with the graph's hand type clearly wrong for
//     what the hand holds -- a spell under a type other than 9, a weapon
//     under 9 or 0, an empty hand under 9 -- with the last swap, who made
//     it, and the last cast; then its end.
// =============================================================================

#include <string>
#include <vector>

namespace Huginn::State::HandModelProbe
{
   /// Called from the PlayerCharacter::Update hook, after the game's update.
   void OnPlayerUpdate(RE::PlayerCharacter* player);

   /// kPostLoadGame / kNewGame: on (and the load time is noted). kPreLoadGame:
   /// off. Either way what was tracked is dropped.
   void SetGameLoaded(bool loaded) noexcept;

   /// `hg hands`: both hands, live -- the weapon, the pose, the view, each
   /// biped's parts for it and where they hang. Main thread (the console).
   [[nodiscard]] std::vector<std::string> DescribeHands(RE::PlayerCharacter* player);

   /// One line for the debug log just before a Huginn hand equip (0.24.26):
   /// both casters' states and current spells, the weapon state, the
   /// behaviour graph's equip and hand variables, and the calling thread
   /// beside the game's main thread id. Main thread (the hand equip gate).
   [[nodiscard]] std::string DescribeForEquip(RE::PlayerCharacter* player);
}
