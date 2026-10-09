#pragma once

// =============================================================================
// SELECTION LOG V3 (R4) -- the game side of the decision log
// =============================================================================
// Writes Huginn_Selections_v3.jsonl (SKSE log folder): one record per player
// decision with the need vector, every item the player could have chosen
// (one row per item, sparse cap(i), the runtime cross-features), the page as
// shown and an explicit outcome -- key, wheel, menu or nothing. Format:
// core/DecisionLog.h; schema: docs/architecture/9-selection-log-v3.md.
//
// LOGGING ONLY. Nothing here is read by scoring, the candidates, the slots,
// the frozen learner or the soak telemetry, and nothing here opens either
// pipeline skip gate. The old Huginn_Selections.jsonl (v2) is written as
// before.
//
// Where the parts come from:
//   - every update tick (Tick): the latest need vector (needs/NeedMonitor),
//     the eligible candidates of the last pipeline run with their static cap
//     (effect/EffectCatalog) and the runtime cross-features for this tick
//     (effect/CrossFeatures over the live player state), and the need
//     episodes (core/NeedEpisodes.h);
//   - a context is taken when one is needed: at a press, when a selection
//     menu opens, at an episode's onset. It adds the page as shown
//     (PipelineStateCache), every held item (inventory and known spells in
//     the catalog, per-instance caps for tempered and player-enchanted gear;
//     cached for a second) and the hostile target's race;
//   - the outcome: SelectionTracker's confirmed selections (key, wheel,
//     menu), the picks the frozen learner drops (a stale pipeline cache,
//     learning from outside equips switched off, armour; recorded here only,
//     learned = 0), and the unanswered episodes (nothing).
//
// Menu picks are joined to the context taken when the menu OPENED (the page
// the player looked at before reaching into the menu, and the situation then;
// inside a pausing menu the world is frozen and the page is hidden), and
// carry its age. They are never dropped for staleness.
//
// Threads: Tick runs on the update loop (a job thread in gameplay, the main
// thread in menus: UpdateLoop.cpp, THREADS above OnUpdate); presses come from
// the input sink, the TESEquipEvent sinks and Wheeler's callback; the menu
// sink on whatever thread the game sends MenuOpenCloseEvent from (not
// traced; this said "the UI thread", unverified). Records are formatted and
// written by a background thread: the caller builds the record (copies) and
// queues it.
// =============================================================================

#include "DecisionCapture.h"
#include "EquipEvent.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Huginn::Learning::SelectionLogV3
{
    /// kDataLoaded: the menu sink (selection menus opening and closing).
    void Register();

    /// The update loop, every tick (after the need monitor, before
    /// SelectionTracker::Update): this tick's rows and cross-features, episode
    /// onsets and ends.
    void Tick(std::chrono::steady_clock::time_point now);

    /// The update loop, every tick, AFTER SelectionTracker::Update: ended
    /// episodes are judged here, so a selection that confirms on the same tick
    /// answers its episode first; the v3-only picks confirm here.
    void TickAfterSelections(std::chrono::steady_clock::time_point now);

    /// A press of `formID`: the context it is joined to, the open episodes,
    /// the time, and whether the item was equipped before the press.
    [[nodiscard]] DecisionCapture CaptureForPress(RE::FormID formID, EquipSource source, std::string_view via);

    /// SelectionTracker confirmed `event` (the press was at `selectedAt`).
    void OnConfirmed(const EquipEvent& event, std::chrono::steady_clock::time_point selectedAt, std::string_view how);

    /// A pick with player input behind it that the frozen learner does not
    /// take (`skip`: "stale", "disabled", "armour"). Held here until it
    /// confirms as SelectionTracker would (consumed, or still equipped 3 s
    /// later), then written with learned = 0.
    void OnUnlearnedPick(RE::FormID formID, std::string via, std::string_view skip, std::string caseLabel);

    /// A real consumption: confirms such a pick. False when none was pending.
    bool OnConsumed(RE::FormID formID);

    /// SelectionTracker took (or withdrew) a selection of `formID`: a v3-only
    /// pick of it is the same selection and is dropped.
    void OnTrackerSelect(RE::FormID formID);

    /// kPreLoadGame: inventory extra data may not be read until OnLoadFinished
    /// (the held rows are read without it, `heldFull: 0`).
    void OnLoadStarting();
    /// kPostLoadGame / kNewGame, after the game systems are initialized (the
    /// post-load window, Util::IsExtraListStable, has been stamped by then).
    void OnLoadFinished();

    /// A game load (or `hg reset all`): episodes, pending picks, the menu's
    /// context and the cached held set are forgotten.
    void Reset();

    struct Stats
    {
        uint64_t key = 0, wheel = 0, menu = 0, nothing = 0;   // records queued
        uint64_t unlearned = 0;                               // of which learned = 0
        uint64_t written = 0, bytes = 0;                      // by the writer
        uint64_t writtenKey = 0, writtenWheel = 0, writtenMenu = 0, writtenNothing = 0;
        uint64_t dropped = 0;                                 // queue full: never written
        uint64_t contextsBuilt = 0, heldReads = 0;
        double buildMeanMs = 0.0, buildMaxMs = 0.0, heldReadMaxMs = 0.0;
        uint64_t ctxWritten = 0, capsWritten = 0;
        uint64_t episodesDroppedShort = 0;
        uint64_t ticks = 0;
        double tickMeanUs = 0.0, tickMaxUs = 0.0;
        size_t lastRows = 0;                                  // rows of the last record queued
        size_t tickRows = 0;                                  // eligible rows the last tick computed
    };
    [[nodiscard]] Stats GetStats();

    /// The file being written (empty before the first record).
    [[nodiscard]] std::filesystem::path FilePath();

    /// Block until the writer has written everything queued (test mode).
    bool Flush(std::chrono::milliseconds timeout);

    /// Test mode (Debug; bDecisionSession): a scripted session that writes a
    /// record of every outcome it can, then calls `done` on its own thread
    /// with an empty reason (pass) or why it failed. SelectionLogV3Session.cpp.
    /// In Release it calls `done("")` at once.
    void StartTestSession(void (*done)(const char* failReason));
}
