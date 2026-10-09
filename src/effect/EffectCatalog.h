#pragma once

// =============================================================================
// EFFECT CATALOG -- cap(i) for every item in the load order (R2)
// =============================================================================
// Built once, when the main menu first opens (so after every kDataLoaded
// handler, the keyword distributors' included): EffectReader reads the load order into plain
// records, Core::Effect::BuildCaps maps and grades them (percentiles need the
// whole load order, not the player's inventory), and the catalog keeps the
// static cap of every in-scope item, keyed by FormID.
//
// NOTHING READS IT FOR SCORING YET (R2 changes no scores). Today its readers
// are `hg dump all` (the catalog view) and `hg cap <FormID>` (one item, its
// per-instance cap and its runtime cross-features). The scorer (R8) and the
// selection log (R4) are its future readers.
//
// Static vs runtime (implementation map, Phase 1): the catalog holds what is
// fixed per base form. A tempered or player-enchanted weapon (or a
// player-enchanted armour piece) gets a per-instance cap from InstanceEntry,
// graded against the same populations. The five runtime columns (overshoot_*,
// weapon_charge, stack_count, ammo_matches_launcher, school_fortified) are
// computed from the live state (effect/CrossFeatures.h).
//
// Overrides (doc 9's layer 2): an optional Data/SKSE/Plugins/
// Huginn_EffectOverrides.ini, section [Overrides], keys "<plugin>|<local
// FormID hex>" = <effects.csv column id>; checked before any other rule. None
// ships yet.
//
// Built once: game data changed at runtime after that -- a Papyrus
// AddKeywordToForm or SetNthEffectMagnitude after a save loads -- is not seen;
// there is no rebuild. Keyword distributors (KID, SPID) finish before the build.
//
// Threads: the forms are read on the main thread (a task queued at the main menu); the mapping
// runs on a worker thread (seconds in a Debug build on LoreRim) and publishes
// with Ready() (release/acquire). Before Ready() every accessor answers
// "nothing"; after it the catalog is immutable, so readers need no lock.
// =============================================================================

#include "core/EffectMapper.h"

#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <unordered_map>

namespace Huginn::Effect
{
    struct CatalogEntry
    {
        Core::Effect::Kind kind = Core::Effect::Kind::Spell;
        Core::Effect::Cap cap;
        // For overshoot_*: the absolute amount restored per vital (health,
        // magicka, stamina), and whether that restore is a full-restore sentinel.
        std::array<float, 3> restoreAmount{};
        std::array<bool, 3> fullRestore{};
        Core::Effect::School school = Core::Effect::School::None;
    };

    class EffectCatalog
    {
    public:
        static EffectCatalog& GetSingleton();

        /// Read the load order (on the calling thread) and map it (on a worker
        /// thread). Once; later calls are ignored. Ready() turns true when the
        /// worker is done.
        void Build();

        /// From kDataLoaded: build when the main menu first opens (a main-thread
        /// task then), which is after every plugin's kDataLoaded handler and
        /// the tasks they queued, keyword distributors included. kNewGame /
        /// kPostLoadGame call Build() too, for a setup that skips the main menu
        /// (a no-op once built). With no UI singleton it builds now.
        void ScheduleBuild();

        /// The worker failed (an exception); the catalog stays not ready.
        [[nodiscard]] bool Failed() const noexcept { return failed_.load(std::memory_order_acquire); }

        [[nodiscard]] bool Ready() const noexcept { return ready_.load(std::memory_order_acquire); }

        /// Block up to `timeout` for the worker (the console and test mode).
        bool WaitUntilReady(std::chrono::milliseconds timeout) const;

        /// The static entry of an in-scope base form, or nullptr.
        [[nodiscard]] const CatalogEntry* Find(RE::FormID formID) const;

        /// The per-instance entry of an inventory stack whose ExtraHealth
        /// (tempering) or ExtraEnchantment (a player enchantment) changes what
        /// the base form says; nullopt for a plain stack (use Find).
        [[nodiscard]] std::optional<CatalogEntry> InstanceEntry(const RE::TESBoundObject* base,
                                                                const RE::ExtraDataList* extra) const;

        /// The column an MGEF maps to (the dump's per-row view), or nullptr.
        [[nodiscard]] const Core::Effect::EffectClass* ClassOf(RE::FormID mgef) const;

        [[nodiscard]] const Core::Effect::Populations& Pops() const noexcept { return pops_; }
        [[nodiscard]] const Core::Effect::OverrideTable& Overrides() const noexcept { return overrides_; }
        [[nodiscard]] const Core::Effect::RowTally& Tally() const noexcept { return tally_; }
        [[nodiscard]] double Coverage() const noexcept
        {
            return tally_.counted > 0 ? static_cast<double>(tally_.mapped) / tally_.counted : 1.0;
        }
        [[nodiscard]] std::size_t Size() const noexcept { return entries_.size(); }
        [[nodiscard]] std::size_t FormsRead() const noexcept { return formsRead_; }
        [[nodiscard]] double BuildMs() const noexcept { return buildMs_; }

    private:
        EffectCatalog() = default;
        void LoadOverrides();

        std::unordered_map<RE::FormID, CatalogEntry> entries_;
        std::unordered_map<RE::FormID, Core::Effect::EffectClass> classes_;
        Core::Effect::Populations pops_;
        Core::Effect::OverrideTable overrides_;
        Core::Effect::RowTally tally_;
        std::size_t formsRead_ = 0;
        double buildMs_ = 0.0;
        std::atomic<bool> ready_{ false };
        std::atomic<bool> failed_{ false };
        bool built_ = false;
    };
}
