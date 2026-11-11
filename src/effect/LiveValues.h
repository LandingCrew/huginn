#pragma once

// =============================================================================
// LIVE VALUES (0.24.30) -- the player's items as they have them now
// =============================================================================
// The effect catalog grades an item from its record: Fire Shell on Self is
// 8 % for 60 s there, 20.7 % for 205 s on its tooltip at the player's skill
// and perks (Requiem scales both). This keeps the catalog on the second
// number for every spell the player knows and every potion, food, weapon and
// armour piece they carry:
//
//   spells, potions, food: each effect's magnitude and duration as the game
//     applies them (effect/SpellPower.h: the Mod Spell Magnitude / Duration
//     entry points and the school's power modifier). A poison is not read.
//   weapons, armour: the damage or rating the inventory shows for the best
//     stack carried, over the same item with no stack data, times the
//     record's value: tempering stays, and the player's skill and the perks
//     every item of a type shares divide out -- the catalog grades a weapon
//     among its own type and an armour piece among its slot and weight, so a
//     shared multiplier changes no order and would only push every grade to
//     the top of a population of records.
//
//   main thread (OnPlayerUpdate, from the PlayerCharacter::Update hook): a
//     fingerprint of what drives a spell's power -- level, unspent perk
//     points, the five magic skills, their power modifiers (Fortify
//     <School>), the number of spells known. When it changes, and after a
//     load, every item is read again, kItemsPerFrame a frame; when only the
//     count moved, only the new ones. A quiet sweep every kSweepEvery
//     catches the rest: an entry point's own conditions (Faction Ranks'
//     Mages Guild Boon, x1.1 from a kill to the next cast), a stack
//     tempered, a potion picked up. An item whose numbers moved is handed
//     to the update loop; the catalog maps each distinct set once. A quiet
//     sweep that moved something logs what changed since the last one (the
//     perk entries' owner conditions, the effects on the player, the hands;
//     at debug).
//   update loop (ApplyPending, at the start of a pipeline run): re-grades up to
//     a budget of handed-over items into the catalog's live entries
//     (EffectCatalog::SetLive), which every lookup reads in place of the
//     record's; true when one changed, so the run is not skipped.
//   self-check (main thread, once a second): a spell the player cast, or a
//     potion or food they took, running on them carries the game's own
//     magnitude and duration; each (item, effect) is compared with ours once
//     a session and logged, match or not ([LiveValues] check).
//
// What is not read: scrolls and staffs (no caster skill), poisons, ammunition,
// a cloak's carried spell, a weapon's or armour's enchantment (its record's).
// =============================================================================

#include <cstddef>

namespace Huginn::Effect::LiveValues
{
    /// Main thread, once a frame. Cheap when nothing changed (a dozen actor
    /// value reads).
    void OnPlayerUpdate(RE::PlayerCharacter* player);

    /// kPreLoadGame: false (stop, forget); kNewGame / kPostLoadGame: true (read
    /// everything again). Clears the catalog's live entries both ways.
    void SetGameLoaded(bool loaded) noexcept;

    /// The update loop: re-grade up to `budget` items whose numbers moved.
    /// True when a live entry changed.
    bool ApplyPending(std::size_t budget);
}
