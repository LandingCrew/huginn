#pragma once

#include "state/GameState.h"
#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"
#include "state/WorldState.h"
#include "state/StateTypes.h"
#include "learning/ScoredCandidate.h"
#include "override/OverrideConditions.h"
#include "slot/SlotAssignment.h"

#include <chrono>
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
        State::HealthTrackingState healthTracking{};
        State::MagickaTrackingState magickaTracking{};
        State::StaminaTrackingState staminaTracking{};
        float currentMagicka = 0.0f;
        uint32_t stateHash = 0;
        bool elementalDamageActive = false;

        // Pipeline outputs (built by successive steps)
        std::vector<Scoring::ScoredCandidate> scoredCandidates;
        Override::OverrideCollection overrides;
        Slot::SlotAssignments rawAssignments;
        Slot::SlotAssignments assignments;
        bool hasUrgentOverride = false;
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
        /// @param pageChanged  True if the user cycled pages (forces pipeline run)
        /// @return true if the pipeline ran (false if hash-skipped)
        bool RunPipeline(float deltaMs, RE::PlayerCharacter* player,
                         std::chrono::steady_clock::time_point now,
                         bool pageChanged);

    private:
        PipelineCoordinator() = default;
        ~PipelineCoordinator() = default;
        PipelineCoordinator(const PipelineCoordinator&) = delete;
        PipelineCoordinator& operator=(const PipelineCoordinator&) = delete;

        // Pipeline steps (called in order by RunPipeline)
        void GatherState(PipelineContext& ctx);
        bool CheckHashSkip(PipelineContext& ctx, bool pageChanged);
        void LogStateTransition(PipelineContext& ctx);
        void EnrichElementalDamage(PipelineContext& ctx);
        void ScoreCandidates(PipelineContext& ctx);
        void AllocateAndLock(PipelineContext& ctx);
        void UpdateCaches(PipelineContext& ctx);
        void PushDisplay(PipelineContext& ctx);

#ifndef NDEBUG
        void UpdateDebugWidgets(PipelineContext& ctx);
#endif

        // Member state (converted from static locals in OnUpdate)
        uint32_t m_lastPipelineHash = UINT32_MAX;  // Force first run
        State::GameState m_lastLoggedState{};

#ifndef NDEBUG
        std::chrono::steady_clock::time_point m_lastDebugLog{};
#endif
    };

}  // namespace Huginn::Pipeline
