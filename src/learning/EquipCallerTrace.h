#pragma once

namespace Huginn::Learning::EquipCallerTrace
{
   // =========================================================================
   // EQUIP CALLER TRACE -- who emptied the player's hands (0.24.26, debug)
   // =========================================================================
   // A Fireball came out of the right hand 3-6 game seconds after every load
   // of one save, and no Huginn path logged it (the 19 loads in the logs read
   // for this change). The TESEquipEvent for a hand item is sent
   // synchronously from the engine's equip path, so the stack of the sink
   // that hears it names whoever applied the change: Huginn.dll, Wheeler.dll,
   // the script VM, the game itself.
   //
   // For the player's hand items -- a spell or scroll, a weapon, a light, a
   // shield -- UNequipped not by Huginn: one debug line with the thread (and
   // whether it is the game's main thread) and up to 16 frames as
   // module+offset. Equips are not traced: an outside equip is already
   // attributed (external-equip learning, the hand probe's swap line). "By
   // Huginn" is EquipSourceTracker's mark: the form itself marked within
   // 1500 ms (the HandModelProbe's window: a queued equip lands when the
   // animation in its way ends), or any Huginn HAND equip marked in that
   // window (what a Huginn equip displaced is not the form it marked; a
   // potion press displaces nothing, so it does not count).
   //
   // A queued unequip is applied later by the engine's own queue: its stack
   // shows the queue, not the caller. The thread still tells a script or a
   // load-screen thread from the main thread.
   //
   // Cheap first: nothing unless debug logging is on; at most one line per
   // kMinIntervalMs overall (checked before the stack is captured; the lines
   // held back are counted on the next one); per item, a stack logged once
   // is not logged again until it differs (Core::SignatureDedup over the
   // raw return addresses, before any module name is resolved).
   //
   // Called from SpellRegistry::ProcessEvent (spells and scrolls) and
   // ExternalEquipListener (everything else), on whichever thread sends the
   // event.
   // =========================================================================

   /// `form` is the event's base object, already looked up.
   void NoteIfExternal(const RE::TESEquipEvent& event, const RE::TESForm& form);
}
