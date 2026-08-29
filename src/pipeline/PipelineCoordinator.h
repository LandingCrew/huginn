#pragma once

#include "state/GameState.h"
#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"
#include "state/WorldState.h"
#include "state/StateTypes.h"
#include "learning/ScoredCandidate.h"
#include "candidate/CandidateTypes.h"
#include "context/ContextReason.h"     // Context::ContextReason (per-tick display reason)
#include "context/ReasonHold.h"        // Context::ReasonHold (label stability, #62)
#include "override/OverrideConditions.h"
#include "slot/SlotAssignment.h"

#include <atomic>
#include <chrono>
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
        // Health tracking only: the elemental-damage enrichment window reads it.
        // Magicka/stamina tracking used to be snapshotted here for the relevance
        // tags; nothing consumes them since the display reason moved onto the
        // context weights (#10), so they are no longer polled per tick.
        State::HealthTrackingState healthTracking{};
        float currentMagicka = 0.0f;
        uint32_t stateHash = 0;
        bool elementalDamageActive = false;
        // Falling is not in the GameState hash either — see the bypass in
        // CheckHashSkip and m_wasFalling below (#60).
        bool fallingActive = false;
        // Nor is being underwater (#61). It only started mattering when the
        // sensor began firing at all; before that the gap was invisible.
        bool underwaterActive = false;

        // Pipeline outputs (built by successive steps)
        std::vector<Scoring::ScoredCandidate> scoredCandidates;
        Override::OverrideCollection overrides;
        Slot::SlotAssignments rawAssignments;
        Slot::SlotAssignments assignments;

        // Dominant reason for THIS tick, derived by ScoreCandidates from the very
        // weight map the scorer ranked against, and consumed by the Wheeler
        // subtext label. One encoding — ContextRuleEngine's curves — for both
        // scoring and display (#10). The ContextWeightMap itself stays local to
        // that step (hence only ContextReason.h here); nothing downstream needs it.
        Context::ContextReason contextReason = Context::ContextReason::None;

        // Display page resolved once per tick by ResolveDisplayPage, then used by
        // allocation, caches, and push so the WHOLE tick stays on one page even if
        // an off-thread page switch (Wheeler callback / page-cycle keys / console)
        // lands mid-tick. displaySlotCount is derived from displayPageIndex, so it
        // always matches `assignments`.
        size_t displayPageIndex = 0;
        size_t displayPageCount = 0;
        size_t displaySlotCount = 0;
        // How many of displaySlotCount accept wildcards. Snapshotted alongside
        // the rest so the wildcard cache is keyed and bounded by one coherent
        // description of the page — see Scoring::WildcardPage.
        size_t displayWildcardSlots = 0;
        std::string displayPageName;

        /// Reset all fields for reuse, preserving allocated container capacity.
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
            elementalDamageActive = false;
            fallingActive = false;
            underwaterActive = false;

            scoredCandidates.clear();
            overrides.activeOverrides.clear();
            rawAssignments.clear();
            assignments.clear();
            contextReason = Context::ContextReason::None;
            displayPageIndex = 0;
            displayPageCount = 0;
            displaySlotCount = 0;
            displayWildcardSlots = 0;
            displayPageName.clear();
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
        /// Four latches have needed this so far — the elemental window, falling
        /// (#60), a pending reason downgrade (#62) and underwater (#61) — and
        /// the first two were each wired into one gate at a time. A latch wired
        /// into only the inner gate is silently dead in a static scene, which is
        /// exactly where the reason hold needed it. Add the fifth here, not at a
        /// call site.
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
            return m_wasElementalDamageActive || m_wasFalling || m_wasUnderwater ||
                   m_reasonHold.IsHolding();
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
            m_wasElementalDamageActive = false;
            m_wasFalling = false;
            m_wasUnderwater = false;
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
        void LogStateTransition(PipelineContext& ctx);
        void EnrichElementalDamage(PipelineContext& ctx);
        void ScoreCandidates(PipelineContext& ctx);
        /// Allocate + lock + visual states. Returns false (abandon the tick) if an
        /// off-thread page switch landed after ResolveDisplayPage's snapshot —
        /// locking the stale page would leak page-blind locks across the switch.
        bool AllocateAndLock(PipelineContext& ctx);
        void UpdateCaches(PipelineContext& ctx);
        void PushDisplay(PipelineContext& ctx);
        void LogRecommendations(PipelineContext& ctx);
        // Ground truth for what the player is actually shown: derives the
        // subtext for every slot on the live page onto ctx.assignments (so
        // `hg recs` prints it) and logs the set on change. Wheeler re-derives
        // under its own [Subtexts] toggles rather than inheriting these.
        void DeriveDisplayLabels(PipelineContext& ctx);

#ifndef NDEBUG
        void UpdateDebugWidgets(PipelineContext& ctx);
#endif

        // Reusable pipeline context — cleared each frame, preserves container capacity
        PipelineContext m_ctx;

        // Member state (converted from static locals in OnUpdate)
        uint32_t m_lastPipelineHash = UINT32_MAX;  // Force first run
        // Elemental window state at the last non-skipped run: the falling edge
        // needs one more run to clear the enriched flags (isOnFire etc. are not
        // part of the GameState hash), or fire-scored recommendations persist
        // after the window closes.
        bool m_wasElementalDamageActive = false;

        // Same problem, same shape: isFalling is not in the GameState hash,
        // so a fall that changes nothing else is invisible to the skip check
        // and never gets scored. The falling edge needs one more run to clear
        // slow-fall scoring once the player lands (#60).
        bool m_wasFalling = false;

        // Same shape again for underwater (#61): surfacing must get one more run
        // to clear the water-breathing weight and stand the drowning override
        // down, and neither edge moves a hashed bucket.
        bool m_wasUnderwater = false;

        // Holds the displayed reason so a momentary one stays readable.
        // Label-only: ScoreCandidates above it always sees the raw weights.
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
