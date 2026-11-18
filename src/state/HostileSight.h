#pragma once

// =============================================================================
// HOSTILE SIGHT (0.24.29) -- which combat hostiles the player has a line of sight to
// =============================================================================
// The fight's element (Core::Needs::ElementsShown) and enemy_casting_* read a
// hostile casting fire, frost or shock, or holding it ready in a raised hand.
// The player perceives those only by seeing the hostile (CLAUDE.md, the
// perception line): a frost mage fighting a follower behind a wall is not a
// frost fight yet. So those readings count only for a hostile the player has
// a line of sight to. Health lost to an element needs no sight.
//
// The test is the game's own, Actor::HasLineOfSight from the player, and it
// casts rays, so it runs where DropAheadProbe casts its own: on the main
// thread, from the PlayerCharacter::Update hook (DropAheadProbe::HookUpdate),
// at most every 100 ms. The state poll (job threads in gameplay) never casts:
// it names the hostiles it wants tested (Request: the combat hostiles showing
// an element this poll) and reads the hook's last answer (Sees). A hostile is
// first counted one hook pass after the poll first names it, about 100 ms.
//
// Off until a game is loaded; off in the main menu and in a loading screen.
// =============================================================================

#include <cstdint>
#include <vector>

namespace Huginn::State::HostileSight
{
   /// The state poll: the hostiles to test from now on (replaces the last list).
   void Request(std::vector<std::uint32_t> formIDs);

   /// The state poll: the last hook pass found a line of sight to this actor.
   [[nodiscard]] bool Sees(std::uint32_t formID);

   /// The PlayerCharacter::Update hook (main thread): test the requested
   /// hostiles, at most every 100 ms.
   void OnPlayerUpdate(RE::PlayerCharacter* player);

   /// kPostLoadGame / kNewGame (true), kPreLoadGame (false). Either way the
   /// requests and the answers are cleared.
   void SetGameLoaded(bool loaded) noexcept;
}
