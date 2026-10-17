#pragma once

// =============================================================================
// HAND MODEL PROBE -- the game half of core/HandModelCheck.h (0.23.44)
// =============================================================================
// Diagnostic only: nothing reads it but the log and `hg hands`. For each hand
// holding a weapon with a model, finds the biped part the engine attached for
// it (BipedAnim::objects[i] with item == the weapon, its partClone) and judges
// it (Core::JudgePart). Drawn, it reads the biped of the current view (the
// first-person one in first person: that is the model the player sees).
// Sheathed, it reads the third-person one.
//
// Where it runs: from the PlayerCharacter::Update hook DropAheadProbe installs,
// on the main thread (the scene graph is the main thread's), every frame for
// the hand contents and every 250 ms for the models. On between kPostLoadGame
// / kNewGame and kPreLoadGame, and never while a LoadingMenu is open.
//
// Logs, under [HandModel]:
//   - debug, each change of the weapon in a hand, with what it replaced,
//     whether Huginn made the equip (key or wheel), the pose, and how long
//     before it the player last cast or attacked -- the base rate the faults
//     are read against;
//   - warn, a fault that held FaultWatch::kOnsetSec, with both bipeds' parts
//     and that same swap context;
//   - info, its end: the model back, or the weapon swapped out.
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
}
