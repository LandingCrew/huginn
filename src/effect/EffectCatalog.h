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
// Its readers (R11): the choice scorer and the selection log v3 (cap(i) per
// candidate, PipelineCoordinator), the slot classes, the candidate filters, the
// override finders and the label (through core/CapFacets.h and
// effect/Facets.h, in place of the old classifiers' tags), `hg dump all`, `hg
// cap <FormID>` and `hg theta <FormID>`.
//
// Static vs runtime (implementation map, Phase 1): the catalog holds what is
// fixed per base form. A tempered or player-enchanted weapon (or a
// player-enchanted armour piece) gets a per-instance cap from InstanceEntry,
// graded against the same populations. The five runtime columns (overshoot_*,
// weapon_charge, stack_count, ammo_matches_launcher, school_fortified) are
// computed from the live state (effect/CrossFeatures.h).
//
// Overrides (doc 9's layer 2; a directory since R11, core/OverrideFiles.h):
// every Data/SKSE/Plugins/Huginn/EffectOverrides/*.ini, alphabetically, then
// the player's Data/SKSE/Plugins/Huginn_EffectOverrides.ini last; section
// [Overrides], keys "<plugin>|<local FormID hex>" = <effects.csv column id>,
// [Huginn] iFormatVersion = 1. Checked before any other rule. None ships.
//
// Built once: game data changed at runtime after that -- a Papyrus
// AddKeywordToForm or SetNthEffectMagnitude after a save loads -- is not seen;
// there is no rebuild. Keyword distributors (KID, SPID) finish before the build.
//
// Threads: the forms are read on the main thread; the mapping runs on a worker
// thread (seconds in a Debug build on LoreRim) and publishes with Ready()
// (release/acquire). Before Ready() every accessor answers "nothing"; after it
// the catalog is immutable, so readers need no lock.
//
// The read in slices (0.23.47): in one go it held the main thread 3.1-3.25 s
// at the main menu on LoreRim (Tracy, five traces, 2026-10-16/17), the largest
// stall left. It still reads every form on the main thread -- the same reads,
// in the same order (core/SlicedWalk.h) -- but in slices of kReadSliceBudget,
// one SKSE task per frame: the Present hook (OnFrame) queues the next slice
// once the last one has run, so a task pass that runs the tasks queued during
// it cannot chain two slices into one frame. Not moved to a thread of its own:
// the read calls engine code (CalculateMagickaCost, GetArmorRating, editor-ID
// lookups, the condition function table) and walks TESDataHandler's arrays,
// and other plugins may still touch forms at the main menu; on the main
// thread none of that can race. What is left of the read when a game starts
// loading is read at once (FinishPendingRead at kPreLoadGame, Build at
// kNewGame / kPostLoadGame), before the save touches any form, so the records
// are those of the main menu, as in one go.
// =============================================================================

#include "core/EffectMapper.h"
#include "core/LateCatalog.h"

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Huginn::Effect
{
    class EffectReader;

    struct CatalogEntry
    {
        Core::Effect::Kind kind = Core::Effect::Kind::Spell;
        Core::Effect::Cap cap;
        // For overshoot_*: the absolute amount restored per vital (health,
        // magicka, stamina), and whether that restore is a full-restore sentinel.
        std::array<float, 3> restoreAmount{};
        std::array<bool, 3> fullRestore{};
        Core::Effect::School school = Core::Effect::School::None;
        // 0.23.30: rows that fire only with a perk, graded, NOT in `cap`: the
        // catalog is built at the main menu, before there is a player, so a
        // perk the character has is added at runtime (CrossFeatures.h
        // PerkAdjustedCap; `hg cap` prints them).
        std::vector<Core::Effect::PerkValue> perkRows;
    };

    /// 0.24.30: one effect of a spell, potion or food as the player would
    /// cast or take it now (effect/LiveValues.h): `index` is its place in the
    /// item's effect list.
    struct LiveRow
    {
        std::uint32_t index = 0;
        float magnitude = 0.0f;
        float duration = 0.0f;   // seconds
    };

    /// 0.24.30: an item's numbers as the player has them now: a spell's,
    /// potion's or food's effect rows, or a weapon's damage or an armour
    /// piece's rating (< 0: the record's). Default: the record's numbers.
    struct LiveNumbers
    {
        std::vector<LiveRow> rows;
        float damage = -1.0f;
        float armorRating = -1.0f;
    };

    class EffectCatalog
    {
    public:
        static EffectCatalog& GetSingleton();

        /// Read the load order (on the calling thread) and map it (on a worker
        /// thread). Once; later calls are ignored -- except that a call while
        /// the main menu's sliced read is under way reads the rest of it now.
        /// Ready() turns true when the worker is done. `why` names the caller
        /// in the log when it ends a sliced read.
        void Build(std::string_view why = "a direct build");

        /// From the task queued when the main menu opens: start the read in
        /// slices of kReadSliceBudget, one per frame (OnFrame), then map it as
        /// Build does. A no-op once a build has started.
        void BuildInSlices();

        /// kPreLoadGame: read what is left of a sliced read now, before the
        /// save changes any form. A no-op with none under way (a build that
        /// never started is left to kPostLoadGame's Build, as before).
        void FinishPendingRead(std::string_view why);

        /// From the Present hook, once a frame, any thread: queue the next
        /// slice as an SKSE task if the last one has run. One atomic load
        /// when there is nothing to do.
        static void OnFrame();

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

        /// The entry of an in-scope base form, or nullptr: the held entry
        /// (graded among what the player has, below) while there is one,
        /// else the record's.
        [[nodiscard]] const CatalogEntry* Find(RE::FormID formID) const;
        /// The record's entry only, graded within the load order (`hg dump
        /// all`, the build's own questions).
        [[nodiscard]] const CatalogEntry* FindRecord(RE::FormID formID) const;
        /// The held entry only (nullptr before the held grading is published,
        /// or for an item the player does not have).
        [[nodiscard]] const CatalogEntry* FindHeld(RE::FormID formID) const;

        // 0.24.30: the held grading. Every spell the player knows and item
        // they carry is mapped from its numbers as the player has them
        // (effect/LiveValues.h: perks, skill, Fortify school, tempering; the
        // record's for what is not read) and graded among those same items
        // (Core::Effect::HeldPopulations): its effects against what the
        // player has, a weapon's damage and an armour piece's rating within
        // the load order. The update loop's thread; `generation` is the
        // load's (LiveValues), and a call for another load is dropped
        // before it touches the form.
        //
        /// Map one held item from `numbers`. True when its mapping changed
        /// (a new item, or its numbers moved); `read` is set when the form
        /// was read (not a set of numbers seen before for it).
        bool SetHeld(const RE::TESBoundObject* form, const LiveNumbers& numbers, std::uint32_t generation,
                     bool& read);
        /// The player no longer has it.
        bool DropHeld(RE::FormID formID, std::uint32_t generation);
        /// Grade every held item among them all and publish the entries,
        /// when a held mapping changed since the last time. True when it
        /// published. A published generation is freed once two later ones
        /// and kRetireAfter have passed (a pipeline run, or the selection
        /// log's tick, may still hold an entry).
        bool RegradeHeld(std::size_t* graded = nullptr);
        /// A held mapping changed since the last RegradeHeld (any thread).
        [[nodiscard]] bool HeldDirty() const noexcept { return heldDirty_.load(std::memory_order_acquire); }
        /// A load (`generation` is the next one): the held items and their
        /// entries go -- another save's perks, and an FF form names another
        /// item there.
        void ClearLive(std::uint32_t generation);

        /// Find, else -- for a form the player holds or knows that the build
        /// never saw or left out by the tome rule -- an entry mapped now
        /// (0.23.41, core/LateCatalog.h): a runtime FF form (a player-made
        /// potion or poison) or a spell no tome teaches. Graded against the
        /// build's populations, made once (the "no entry" answer too; a spell
        /// whose type alone rules it out, an ability, is answered without a
        /// read), an FF one forgotten on the next load (ForgetLate), its
        /// address valid for the process. Reads the form: the update loop's
        /// thread or the main thread, as InstanceEntry.
        [[nodiscard]] const CatalogEntry* FindOrMap(const RE::TESBoundObject* form);
        /// The same by FormID (LookupByID); nullptr for an unknown ID.
        [[nodiscard]] const CatalogEntry* FindOrMapId(RE::FormID formID);

        /// A game load (or `hg reset all`): late FF entries are forgotten --
        /// an FF ID names another potion in another save. Their memory is
        /// kept. A load-order key keeps its answer: its form is the same form
        /// in every save of this launch.
        void ForgetLate();

        /// Late keys live now (load-order keys kept across loads, FF keys since
        /// the last load), and entries made in all.
        [[nodiscard]] std::size_t LateLive() const;

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

    private:
        EffectCatalog();
        ~EffectCatalog();
        void LoadOverrides();

        // The read in slices (0.23.47), under readMutex_ (the main thread's
        // tasks and the load messages; a lock so a game job thread running an
        // SKSE task could not race it either). `built_` is under it too.
        struct PendingRead;
        bool StartReadLocked();
        void ReadLocked(bool sliced, std::string_view why);
        void FinishReadLocked(std::string_view lastAtOnce);
        void RunSlice();
        std::mutex readMutex_;
        std::unique_ptr<PendingRead> pending_;

        std::unordered_map<RE::FormID, CatalogEntry> entries_;
        std::unordered_map<RE::FormID, Core::Effect::EffectClass> classes_;
        Core::Effect::Populations pops_;
        Core::Effect::OverrideTable overrides_;
        Core::Effect::RowTally tally_;
        std::size_t formsRead_ = 0;
        std::atomic<bool> ready_{ false };
        std::atomic<bool> failed_{ false };
        bool built_ = false;

        // Late entries (0.23.41): guarded by lateMutex_; the reader keeps the
        // tome list it reads once.
        [[nodiscard]] std::unique_ptr<CatalogEntry> MapLateForm(const RE::TESBoundObject* form);
        mutable std::mutex lateMutex_;
        Core::Effect::LateEntries<CatalogEntry> late_;
        std::unique_ptr<EffectReader> lateReader_;

        // The held grading (0.24.30). Published: the entries' map and the
        // held population under liveMutex_ (Find takes it shared, and only
        // while anyLive_). Made: the mappings, under liveMakeMutex_ (the
        // update loop's thread; a load's ClearLive waits for a read under way).
        using Generation = std::vector<std::unique_ptr<CatalogEntry>>;
        mutable std::shared_mutex liveMutex_;
        std::unordered_map<RE::FormID, const CatalogEntry*> live_;
        std::shared_ptr<const Core::Effect::Populations> heldPops_;
        std::atomic<bool> anyLive_{ false };

        struct HeldMapping
        {
            LiveNumbers numbers;
            Core::Effect::ItemMapping mapping;
        };
        struct HeldItem
        {
            std::size_t current = 0;            // into seen
            std::vector<HeldMapping> seen;      // a conditional perk flips an item between a few states: each mapped once
        };
        std::mutex liveMakeMutex_;
        std::unordered_map<RE::FormID, HeldItem> held_;
        std::uint32_t liveGeneration_ = 0;
        std::atomic<bool> heldDirty_{ false };
        std::unique_ptr<EffectReader> liveReader_;
        std::unique_ptr<Generation> published_;
        struct Retired
        {
            std::chrono::steady_clock::time_point at;
            std::unique_ptr<Generation> entries;
        };
        std::deque<Retired> retired_;
        static constexpr auto kRetireAfter = std::chrono::seconds(60);
        void RetireLocked(std::unique_ptr<Generation> gen);
    };
}
