#pragma once

#include "state/GameState.h"
#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"
#include "state/WorldState.h"
#include "state/StateTypes.h"
#include "learning/ScoredCandidate.h"
#include "learning/UtilityScorer.h"   // Scoring::RecsFloors (the [Recs] lines' lm note)
#include "learning/PipelineStateCache.h"   // EligibleRow (R4, the selection log v3)
#include "learning/legacy/LegacyRun.h"   // Legacy::Run (R8.4: the pipeline's one handle on the D2 feature)
#include "core/ChoiceLive.h"                      // the ranking pass's rows (R8.3)
#include "core/SlotAllocCore.h"                   // the live allocation's input, for the D2 shadow's pages (0.24.17)
#include "core/LegacyShadowPass.h"                // the D2 shadow's run (0.24.17)
#include "candidate/CandidateTypes.h"
#include "context/ContextReason.h"     // Context::ContextReason (per-tick display reason)
#include "context/ReasonHold.h"        // Context::ReasonHold (label stability, #62)
#include "override/OverrideConditions.h"
#include "slot/SlotAssignment.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Huginn::Pipeline
{
    // =========================================================================
    // PipelineContext — Data bundle shared between pipeline steps.
    // =========================================================================
    // Replaces all local variables that were scattered across the inner pipeline
    // block of OnUpdate(). Each step reads inputs from earlier steps and writes
    // outputs for later steps — the struct makes this data flow explicit.
    // =========================================================================

    struct PipelineContext
    {
        // Inputs (set by RunPipeline before first step)
        float deltaMs = 0.0f;
        RE::PlayerCharacter* player = nullptr;
        RE::ActorValueOwner* actorValue = nullptr;
        std::chrono::steady_clock::time_point now{};

        // State snapshots (GatherState)
        State::GameState currentState{};
        State::PlayerActorState playerState{};
        State::TargetCollection targets{};
        State::WorldState worldState{};
        // Health tracking only: the elemental-damage enrichment window reads it
        // (the legacy context rules' isOnFire and the like; R8.3: no longer a
        // skip-gate term, the need signature took that over).
        State::HealthTrackingState healthTracking{};
        float currentMagicka = 0.0f;
        uint32_t stateHash = 0;
        // R8.3: the need vector's 0.05-step signature (NeedMonitor,
        // core/NeedEvaluator.h SignatureHash), compared with the last run's
        // in CheckHashSkip: a step on any need re-scores. It replaced the
        // ambient signature (darkness, hunger and cold are needs) and the
        // elemental window (the damage-rate needs decay with time).
        uint64_t needSignature = 0;
        // Falling is not in the GameState hash either — see the bypass in
        // CheckHashSkip and m_wasFalling below (#60).
        bool fallingActive = false;
        // Nor is being underwater (#61). It only started mattering when the
        // sensor began firing at all; before that the gap was invisible.
        bool underwaterActive = false;
        // Nor is standing at a crafting station (#63/#65). Same shape as the two
        // above, and the one that reaches a player: fortify gear and fortify
        // potions rank on the workstation_* needs (and pass the no_workstation
        // gate) only while a bench is up, so without this the workstation
        // context could only surface when some unrelated dimension happened to
        // move at the same moment.
        bool workstationActive = false;
        // Each vital's zone against its override's thresholds
        // (Core::OverrideZone; 2 bits each: health, magicka, stamina). Compared
        // like needSignature: a crossing of an override threshold always
        // gets a run, whatever the buckets or the locks are doing.
        uint8_t overrideSignature = 0;
        // R8.1 (0.23.55): the active-buff blocks due for their refresh
        // (ActorBuffs::refreshDue). Compared like needSignature: a block
        // entering its last fRefreshPriorSec seconds moves no hashed bucket,
        // and the filters only let its item through on a run.
        Core::Refresh::DueBits refreshDue = 0;
        // 0.24.29: the crosshair lock's level (Gate::Inputs::lockLevel,
        // Lock::kNoLock for none). Compared like refreshDue: the need reads
        // only 0/1, so a Novice door beside an Expert chest moves nothing
        // hashed, and lock_out_of_reach only redraws on a run.
        int lockLevel = -1;
        // R8.1: seconds since the current fight began, kNever out of combat
        // (core/CombatOnset.h): the combat_onset clock, read once per run for
        // the legacy pass's potion multiplier.
        float combatOnsetAgo = 0.0f;

        // Pipeline outputs (built by successive steps)
        std::vector<Scoring::ScoredCandidate> scoredCandidates;
        Override::OverrideCollection overrides;
        Slot::SlotAssignments rawAssignments;
        Slot::SlotAssignments assignments;

        // The reason label for THIS tick (R8.3): the most urgent context reason
        // among the explanations (the largest theta n cap term) of the items
        // the allocated page shows, through the reason hold. Derived in
        // DeriveDisplayLabels; consumed by the subtext labels, which speak it
        // only for an item whose own explanation is that reason.
        Context::ContextReason contextReason = Context::ContextReason::None;

        // R8.3: the ranking pass. choiceLive = the bootstrap is read and the
        // pipeline ranks on mu; else the fallback ranks on the old utility.
        bool choiceLive = false;
        // Index-aligned with the candidates (and so with `eligible`): each
        // row's attributes and its ranking-pass score.
        std::vector<Core::Choice::Attr> choiceAttrs;
        std::vector<Core::Choice::LiveScore> choiceScores;
        std::shared_ptr<const Learning::ShadowRun> choiceRun;

        // Display page resolved once per tick by ResolveDisplayPage, then used by
        // allocation, caches, and push so the WHOLE tick stays on one page even if
        // an off-thread page switch (Wheeler callback / page-cycle keys / console)
        // lands mid-tick. displaySlotCount is derived from displayPageIndex, so it
        // always matches `assignments`.
        size_t displayPageIndex = 0;
        size_t displayPageCount = 0;
        size_t displaySlotCount = 0;
        // R10: the page's wildcard key (SlotAllocator::GetWildcardSeat),
        // SIZE_MAX when it has none. Snapshotted alongside the rest so the
        // wildcard cache is keyed by one coherent description of the page --
        // see Scoring::WildcardPage.
        size_t displayWildcardSeat = SIZE_MAX;
        std::string displayPageName;
        // The display page feeds a Wheeler wheel, so weapons and armour with
        // no uniqueID stay off it this tick (display/WheelUidFilter.h). Read
        // once, in ScoreCandidates, so the wildcard roll, the allocation and
        // the lock check agree.
        bool hideUidlessGear = false;
        // The display page's candidates without that gear, when any was left
        // out (CandidatesForWheelPage). Capacity kept across ticks.
        Scoring::ScoredCandidateList pageCandidatesScratch;

        // R4: every eligible candidate of this run (no floor), copied out for
        // the selection log v3. A read of what ScoreCandidates produced; nothing
        // downstream of scoring reads it.
        std::vector<Learning::PipelineStateCache::EligibleRow> eligible;
        // 0.23.46: every gathered candidate the filters dropped, with the
        // filter that dropped it (the selection log v3's `filtered_by`).
        std::vector<Core::Filter::Dropped> filtered;
        // 0.23.53: the old engine's view of this run (scorer parameters, the
        // learner's state features, the score parts of the filter drops), for
        // the selection log v3 only. The eligible rows carry their own parts.
        Learning::PipelineStateCache::OldScoreRun oldScore;
        // R8.4: this run's legacy pass (learning/legacy/LegacyRun.h), the D2
        // feature's input: legacy.ran = it ran ([Scoring] bLegacyLearnFeature,
        // or the fallback, which needs the utility); the floors' verdict per
        // candidate (index-aligned with the candidates, and so with
        // `eligible`), the row each candidate reads (itself, or the kept one of
        // its dedup key), the kept rows and the weights the pass used.
        Scoring::Legacy::Run legacy;
        // 0.24.17: [Scoring] bLegacyLearnFeature as this run read it (the
        // live score reads the D2 feature: engine A live), read once in
        // ScoreCandidates so a hot reload cannot split a run.
        bool legacyLive = false;
        // What the old floors were this run, for the [Recs] lines' lm note
        // (none: no legacy pass; live: engine A or the fallback; shadow: D2
        // beside engine B).
        Scoring::RecsFloors recsFloors = Scoring::RecsFloors::none;
        // 0.24.17: the D2 engine in shadow (core/LegacyShadowPass.h): engine
        // A's mu, ranks and stateless page keys, index-aligned with the
        // candidates (and so with `eligible`). Ran when B is live, the choice
        // score ranks and [Scoring] bShadowLegacy is on. Written only through
        // the pure pass (RankOnChoice, ShadowLegacyPages), copied onto the
        // eligible rows, read by nothing live (tools/prune/r8_check.py tags
        // every line that names it).
        Core::LegacyShadow::Run legacyShadow;
        // ...and the live allocation's input, handed back by the allocator
        // for the shadow's stateless pages (core/SlotAllocStateless.h), with
        // each of its candidates' run row (ScoredCandidate::runRow). Filled
        // only on a run the shadow runs on. The pure pass builds both
        // stateless pages in place on it, A's last, so afterwards it holds
        // A's order and A's mu: legacyShadow-prefixed for that reason
        // (tools/prune/r8_check.py tags it), and cleared right after the pass
        // and on every Reset.
        Core::SlotAlloc::Input legacyShadowAllocInput;
        bool haveAllocInput = false;
        std::vector<uint32_t> pageRunRows;
        // 0.24.29: in combat, the allocator builds the memoryless page (the
        // combat upsert's challengers) in place on that input; it is B's
        // stateless page, and the shadow takes it instead of building it
        // again. Written by every allocation.
        bool statelessBBuilt = false;
        std::vector<uint32_t> statelessBOrder;
        Core::SlotAlloc::StatelessPage statelessB;

        /// Reset all fields for reuse, preserving allocated container capacity
        /// (except the legacy pass's: since R8.4 the pass returns a fresh Run
        /// that the pipeline move-assigns each run, a few small vectors).
        void Reset()
        {
            deltaMs = 0.0f;
            player = nullptr;
            actorValue = nullptr;
            now = {};

            currentState = {};
            playerState = {};
            targets.primary.reset();
            targets.targets.clear();
            worldState = {};
            healthTracking = {};
            currentMagicka = 0.0f;
            stateHash = 0;
            needSignature = 0;
            fallingActive = false;
            underwaterActive = false;
            workstationActive = false;
            refreshDue = 0;
            lockLevel = -1;
            combatOnsetAgo = 0.0f;

            scoredCandidates.clear();
            overrides.activeOverrides.clear();
            rawAssignments.clear();
            assignments.clear();
            contextReason = Context::ContextReason::None;
            displayPageIndex = 0;
            displayPageCount = 0;
            displaySlotCount = 0;
            displayWildcardSeat = SIZE_MAX;
            displayPageName.clear();
            hideUidlessGear = false;
            pageCandidatesScratch.clear();
            eligible.clear();
            filtered.clear();
            oldScore.run = {};
            oldScore.stateHash = 0;
            oldScore.dropped.clear();
            oldScore.shadow.reset();
            legacy.Clear();
            legacyLive = false;
            legacyShadow.Clear();
            legacyShadowAllocInput = {};
            recsFloors = Scoring::RecsFloors::none;
            haveAllocInput = false;
            pageRunRows.clear();
            statelessBBuilt = false;
            statelessBOrder.clear();
            statelessB = {};
            choiceLive = false;
            choiceAttrs.clear();
            choiceScores.clear();
            choiceRun.reset();
        }
    };

    // =========================================================================
    // PipelineCoordinator — Orchestrates the recommendation pipeline.
    // =========================================================================
    // Extracted from OnUpdate() lines 290-452. Each step method maps 1:1 to
    // a block of the original code. RunPipeline() calls them in sequence.
    //
    // Singleton — same lifetime as the process (matches the file-scope statics
    // and function-local statics it replaces).
    // =========================================================================

    class PipelineCoordinator
    {
    public:
        static PipelineCoordinator& GetSingleton();

        /// Run the full recommendation pipeline.
        /// @param deltaMs      Frame delta in milliseconds
        /// @param player       Player singleton (already null-checked by caller)
        /// @param now          Current time point
        /// @param pageChanged  Page-dirty flag: set by page cycling, a missed
        ///                     page change, Wheeler close, or an inventory change.
        ///                     Forces the pipeline run (bypasses hash-skip).
        /// @return true if the pipeline ran (false if hash-skipped)
        bool RunPipeline(float deltaMs, RE::PlayerCharacter* player,
                         std::chrono::steady_clock::time_point now,
                         bool pageChanged);

        /// True when the last completed run left behind state the GameState hash
        /// cannot see, so the pipeline must run again even though no sensor
        /// reported a change. ONE list, consulted by BOTH skip gates: the outer
        /// one in RunPipelineIfNeeded (which returns before RunPipeline is ever
        /// called) and CheckHashSkip below.
        ///
        /// Latches that have needed this: falling (#60), a pending reason
        /// downgrade (#62), underwater (#61), a learner update, and (R8.3) the
        /// need signature moving a step since the last run -- which replaced
        /// the elemental window here; 0.24.29, a combat challenger's wait
        /// maturing (CombatUpsertWakeDue). A latch wired into only the inner
        /// gate is silently dead in a static scene, which is exactly where the
        /// reason hold needed it. Add the next one here, not at a call site.
        ///
        /// Underwater is the cautionary one. It was missing from the hash the
        /// whole time and nobody noticed, because the sensor never fired: #61's
        /// water-height bug hid a second bug behind it. Both in-game
        /// confirmations came from `Sneaking → Underwater` transitions, where
        /// isSneaking — which IS hashed — moved the hash for an unrelated
        /// reason. Submerge without touching a hashed field and the tick skips.
        ///
        /// Reads members written by the update thread; called from that same
        /// thread only.
        [[nodiscard]] bool NeedsForcedRun() const noexcept
        {
            return m_wasFalling || m_wasUnderwater || m_reasonHold.IsHolding() || LearnerWeightsChanged() ||
                   NeedSignatureMoved() || CombatUpsertWakeDue();
        }

        /// Drop everything that describes the character being unloaded. The held
        /// context reason and the [Context] log baseline say nothing about the
        /// next character (#62); the hash/state baselines below are worse — a
        /// load into a state that happens to hash the same as the pre-load one
        /// would let the FIRST post-load pass hash-skip, leaving the previous
        /// character's recommendations on the wheel. ResetTrackingState opens
        /// the outer gate for that pass, but nothing was reopening the inner one.
        ///
        /// THREAD SAFETY: NOT uniformly serialized. The console path
        /// (`hg reset all`) runs under UpdateHandler::RunExclusive; the save-load
        /// path (InitializeGameSystems → ResetPipelineSubsystems) does not, so
        /// that one can land while the update thread is inside the pipeline.
        /// Same exposure as the SlotLocker/SlotAllocator resets beside it, and
        /// the worst case is one tick reading a mix of both sides — a stale
        /// label or one redundant pipeline run, not a torn structure.
        void ResetCrossSaveState()
        {
            m_reasonHold.Reset();
            m_lastLoggedReason.reset();

            m_lastPipelineHash = UINT32_MAX;  // Force the first post-load run
            m_lastLoggedState = {};
            m_wasFalling = false;
            m_wasUnderwater = false;
            m_lastNeedSignature = 0;
            m_lastOverrideSignature = 0xFF;
        }

        /// Queue a one-shot full-detail recommendation dump (console `hg recs`).
        /// Logged by the next pipeline pass; caller should MarkPageDirty() +
        /// ForceUpdate() to make that pass happen immediately.
        /// Thread-safe (atomic) — callable from the console handler.
        void RequestRecommendationDump(size_t topN)
        {
            m_recDumpRequest.store(topN, std::memory_order_relaxed);
        }

    private:
        PipelineCoordinator() = default;
        ~PipelineCoordinator() = default;
        PipelineCoordinator(const PipelineCoordinator&) = delete;
        PipelineCoordinator& operator=(const PipelineCoordinator&) = delete;

        // Pipeline steps (called in order by RunPipeline)
        void GatherState(PipelineContext& ctx);
        /// Sync the allocator's active page to the display's desired page BEFORE
        /// allocation and snapshot it onto ctx, so allocation/locks/caches/push
        /// are all consistent for one page. Returns true if the page changed
        /// (folds into the skip decision).
        bool ResolveDisplayPage(PipelineContext& ctx);
        bool CheckHashSkip(PipelineContext& ctx, bool pageChanged);

        /// The fifth latch (see NeedsForcedRun): the learner was rewarded since
        /// the last committed run, so the ranking has moved even though no
        /// sensor did (the old learner only while a run reads it: engine A
        /// live, D2 in shadow, or the fallback). Out of line so this header
        /// need not pull in Globals.h.
        /// Non-consuming — BOTH skip gates ask, and only the run itself clears
        /// it, in CheckHashSkip's commit path.
        [[nodiscard]] static bool LearnerWeightsChanged() noexcept;
        /// R8.3: the need signature NeedMonitor holds now differs from the one
        /// the last committed run scored with (out of line: NeedMonitor.h).
        [[nodiscard]] bool NeedSignatureMoved() const noexcept;
        /// 0.24.29 (review of PR #254): a waiting combat challenger's time
        /// gates (fCombatWantedSec, its recent departure, the page's spacing)
        /// opened since the last run (Slot::SlotLocker::CombatUpsertWakeDue).
        /// The wanted clock moves only on a run, and a quiet fight -- its
        /// element held, nothing hashed changing -- skips them, so a matured
        /// wait sat until an unrelated change. One run per maturing wait: the
        /// run sets the next time. Out of line: SlotLocker.h.
        [[nodiscard]] static bool CombatUpsertWakeDue() noexcept;
        void LogStateTransition(PipelineContext& ctx);
        void EnrichElementalDamage(PipelineContext& ctx);
        void ScoreCandidates(PipelineContext& ctx);
        /// R8.3: the ranking pass -- one ScoredCandidate per candidate, mu as
        /// its score (core/ChoiceLive.h), the legacy pass's utility and parts
        /// on the rows the old floors kept (ctx.legacy). Fills ctx.choiceAttrs /
        /// choiceScores / choiceRun.
        Scoring::ScoredCandidateList RankOnChoice(PipelineContext& ctx,
            const std::vector<Candidate::CandidateVariant>& candidates);
        /// R4: copy every generated candidate into ctx.eligible (reads only).
        /// 0.23.53: with the old engine's score parts of every candidate,
        /// whatever the floors, and of the filter drops (ctx.oldScore), from
        /// Legacy::OldScoreCapture over ctx.legacy (R8.4).
        void CaptureEligible(PipelineContext& ctx, const std::vector<Candidate::CandidateVariant>& candidates);
        /// Allocate + lock + visual states. Returns false (abandon the tick) if an
        /// off-thread page switch landed after ResolveDisplayPage's snapshot —
        /// locking the stale page would leak page-blind locks across the switch.
        bool AllocateAndLock(PipelineContext& ctx);
        /// 0.24.17: the D2 engine in shadow, after the allocation: each
        /// candidate's rank under A and under B, its key on A's and on B's
        /// stateless page (core/SlotAllocStateless.h, from the live
        /// allocation's input), copied onto the eligible rows. Reads the run;
        /// writes only ctx.legacyShadow and the eligible rows' shadow fields.
        void ShadowLegacyPages(PipelineContext& ctx);
        void UpdateCaches(PipelineContext& ctx);
        /// R8.3 (was R8.2's ScoreShadow): after the allocation, what the page
        /// adds to each eligible row's logged score -- the slot feature of its
        /// key and kappa off the page (core/ChoiceLive.h AfterAllocation) --
        /// and the opportunity counter. The ranking itself ran in
        /// ScoreCandidates.
        void LogChoiceRows(PipelineContext& ctx);
        void PushDisplay(PipelineContext& ctx);
        void LogRecommendations(PipelineContext& ctx);
        // Ground truth for what the player is actually shown: derives the
        // subtext for every slot on the live page onto ctx.assignments (so
        // `hg recs` prints it) and logs the set on change. Wheeler re-derives
        // under its own [Subtexts] toggles rather than inheriting these.
        void DeriveDisplayLabels(PipelineContext& ctx);
        /// R8.3: the tick's reason from the explanations of what the page
        /// shows, through the hold (in place of the old weights' reason).
        void UpdateContextReason(PipelineContext& ctx);

#ifndef NDEBUG
        void UpdateDebugWidgets(PipelineContext& ctx);
#endif

        // Reusable pipeline context — cleared each frame, preserves container capacity
        PipelineContext m_ctx;

        // Member state (converted from static locals in OnUpdate)
        uint32_t m_lastPipelineHash = UINT32_MAX;  // Force first run

        // Same problem, same shape: isFalling is not in the GameState hash,
        // so a fall that changes nothing else is invisible to the skip check
        // and never gets scored. The falling edge needs one more run to clear
        // slow-fall scoring once the player lands (#60).
        bool m_wasFalling = false;

        // Same shape again for underwater (#61): surfacing must get one more run
        // to clear the water-breathing weight and stand the drowning override
        // down, and neither edge moves a hashed bucket.
        bool m_wasUnderwater = false;

        // needSignature at the last run (R8.3; it replaced the ambient
        // signature of /code-review #152). Compared, not OR-ed in: a run
        // whenever it changes, in either direction. Atomic: NeedsForcedRun
        // reads it, and both gates run on the update thread, but `hg recs`
        // and the harness tick on their caller's thread under the same mutex.
        std::atomic<uint64_t> m_lastNeedSignature{ 0 };
        uint8_t m_lastOverrideSignature = 0xFF;   // never a real signature: first run always counts
        Core::Refresh::DueBits m_lastRefreshDue = 0;   // R8.1: refreshDue at the last run
        int m_lastLockLevel = -1;                        // 0.24.29: lockLevel at the last run

        // Holds the displayed reason so a momentary one stays readable.
        // Label-only (R8.3: fed by the page's explanations, UpdateContextReason);
        // the ranking never reads it.
        Context::ReasonHold m_reasonHold;

        // What the last [Context] line reported. Both halves, because the hold
        // makes them diverge and the raw one is the only record of what actually
        // drove the ranking — deduping on the displayed label alone would drop
        // every raw change that a hold was masking.
        struct LoggedReason
        {
            Context::ContextReason raw = Context::ContextReason::None;
            Context::ContextReason shown = Context::ContextReason::None;
            bool operator==(const LoggedReason&) const = default;
        };
        // Empty before the first line and after a load, so the first transition
        // of a new character is never diffed against the previous one's reason.
        // (An optional rather than a value + "dirty" flag: there is no reason
        // value that can stand for "nothing logged yet" — None is a real one.)
        std::optional<LoggedReason> m_lastLoggedReason;

        State::GameState m_lastLoggedState{};

        // Recommendation logging (both build configs)
        std::chrono::steady_clock::time_point m_lastRecLog{};
        std::atomic<size_t> m_recDumpRequest{0};  // pending `hg recs` top-N (0 = none)

#ifndef NDEBUG
        // Wheeler state validation throttle (debug builds)
        std::chrono::steady_clock::time_point m_lastDebugLog{};
#endif
    };

}  // namespace Huginn::Pipeline
