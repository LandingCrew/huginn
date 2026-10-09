#pragma once

// =============================================================================
// EFFECT READER -- game forms -> the plain records of core/EffectRecords.h
// =============================================================================
// The game half of the effect extractor (implementation map, Phase 1): it
// only READS forms; every rule lives in src/core/ (EffectRules, EffectMapper),
// host-tested. What it reads is what `hg dump all` prints, column for column,
// so a dump re-read on the host rebuilds the same records
// (tools: huginn_effect_report checks the in-game catalog against it).
//
// Forms read (the load order, not the player's inventory: percentiles need
// the whole population): spells, scrolls, potions/poisons/food, weapons
// (staves included), ammo, armour, soul gems and carried lights, each with a
// name. Ingredients are out (the user, 2026-10-07). Scope is decided later,
// by Core::Effect::InScope.
//
// Per magic effect, beyond the old dump: the school (associatedSkill), the
// per-effect cost, and for a Cloak or SpawnHazard the effects of the spell it
// carries (associatedForm, or the hazard's spell), one level deep.
//
// Plain form reads, no writes. Not main-thread only: ReadLoadOrder runs on
// the main thread (the catalog build, a task queued when the main menu opens,
// traced; kNewGame / kPostLoadGame if it never opened; kDataLoaded only with
// no UI) and from the console (`hg dump all`); ReadForm also runs on the
// update loop's thread, for a per-instance cap (EffectCatalog::InstanceEntry
// from SelectionLogV3's held set) -- a game job thread in gameplay
// (UpdateLoop.cpp, THREADS above OnUpdate) -- and from the console (`hg cap`).
// =============================================================================

#include "core/EffectRecords.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Huginn::Effect
{
    struct ReadResult
    {
        std::vector<Core::Effect::ItemRecord> items;
        Core::Effect::EffectTable effects;
        // Index-aligned with `items` / `effects`, for the dump.
        std::vector<const RE::TESBoundObject*> forms;
        std::vector<const RE::EffectSetting*> mgefs;
        // Per item, its effect items in order (index-aligned with
        // items[i].effects), for the conditions column.
        std::vector<std::vector<const RE::Effect*>> effectItems;
        // Per effect, the spell carried by a Cloak/SpawnHazard, if any.
        std::vector<const RE::MagicItem*> payloadSpells;
        // Per effect, the payload's effect items (index-aligned with
        // effects[i].payload).
        std::vector<std::vector<const RE::Effect*>> payloadItems;
        // MGEF -> its index in `effects`; whether its payload has been read.
        std::unordered_map<const RE::EffectSetting*, std::uint32_t> effectIndex;
        std::vector<bool> payloadFilled;
    };

    class EffectReader
    {
    public:
        /// Every named item-like form in the load order.
        [[nodiscard]] ReadResult ReadLoadOrder();

        /// One form (and, for a weapon or armour stack, a player enchantment
        /// that replaces the form's own), into `out` with its effects appended
        /// to `out`'s table. False for a form of no kind the extractor reads.
        bool ReadForm(const RE::TESBoundObject* form, const RE::EnchantmentItem* playerEnchantment, ReadResult& out);

    private:
        std::unordered_set<RE::FormID> taughtByTome_;
        bool tomesRead_ = false;

        void ReadTomes();
        std::uint32_t EffectIndex(const RE::EffectSetting* mgef, ReadResult& out, bool withPayload);
        void AppendEffects(const RE::BSTArray<RE::Effect*>& effects, Core::Effect::ItemRecord& item,
                           std::vector<const RE::Effect*>& items, ReadResult& out);
    };
}
