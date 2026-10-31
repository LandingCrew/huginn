#pragma once

namespace Huginn::Learning::EquipCallerTrace
{
   // =========================================================================
   // EQUIP CALLER TRACE -- who changed the player's hands (0.24.26, debug)
   // =========================================================================
   // A Fireball came out of the right hand 3-6 game seconds after every load
   // of one save, and no Huginn path logged it (the 19 loads in the logs read
   // for this change). The TESEquipEvent for a hand item is sent
   // synchronously from the engine's equip path, so the stack of the sink
   // that hears it names whoever applied the change: Huginn.dll, Wheeler.dll,
   // the script VM, the game itself.
   //
   // For the player's hand items -- a spell or scroll, a weapon, a light, a
   // shield -- equipped or unequipped NOT by Huginn: one debug line with the
   // thread (and whether it is the game's main thread) and up to 16 frames
   // as module+offset. "By Huginn" is EquipSourceTracker's mark: the form
   // itself marked within 1500 ms (the HandModelProbe's window: a queued
   // equip lands when the animation in its way ends), or, for an unequip,
   // any Huginn equip marked in that window (the item a Huginn equip
   // displaced is not the form it marked).
   //
   // A queued equip is applied later by the engine's own queue: its stack
   // shows the queue, not the caller. The thread still tells a script or a
   // load-screen thread from the main thread.
   //
   // Deduplicated: per item and direction, a stack logged once is not logged
   // again until it differs (Core::SignatureDedup); and at most one line per
   // kMinIntervalMs overall, the skipped ones counted on the next line.
   // Nothing is captured unless debug logging is on.
   //
   // Called from SpellRegistry::ProcessEvent (spells and scrolls) and
   // ExternalEquipListener (everything else), on whichever thread sends the
   // event.
   // =========================================================================

   /// `form` is the event's base object, already looked up.
   void NoteIfExternal(const RE::TESEquipEvent& event, const RE::TESForm& form);
}
