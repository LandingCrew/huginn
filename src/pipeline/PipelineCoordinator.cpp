#include "PipelineCoordinator.h"
#include "Globals.h"
#include "../Profiling.h"

#include "state/GameState.h"       // DiffGameState (inline free function)
#include "state/StateManager.h"
#include "state/StateConstants.h"
#include "candidate/CandidateGenerator.h"
#include "override/OverrideManager.h"
#include "slot/SlotAllocator.h"
#include "slot/SlotLocker.h"
#include "slot/SlotUtils.h"
#include "input/EquipManager.h"
#include "wheeler/WheelerClient.h"
#include "wheeler/WheelerSettings.h"
#include "learning/PipelineStateCache.h"
#include "display/IDisplayBackend.h"
#include "display/WheelerBackend.h"
#include "display/IntuitionBackend.h"

#ifdef _DEBUG
#include "ui/UtilityScorerDebugWidget.h"
#endif

using namespace Huginn;

// =============================================================================
// DISPLAY BACKENDS
// =============================================================================
// Registered display backends — each receives slot assignments per frame.
// Adding a new display target: implement IDisplayBackend, add instance here.
// =============================================================================

static Display::WheelerBackend  s_wheelerBackend;
static Display::IntuitionBackend s_intuitionBackend;

static Display::IDisplayBackend* s_displayBackends[] = {
    &s_wheelerBackend,
    &s_intuitionBackend,
};

// =============================================================================
// PIPELINE COORDINATOR
// =============================================================================

namespace Huginn::Pipeline
{

PipelineCoordinator& PipelineCoordinator::GetSingleton()
{
    static PipelineCoordinator instance;
    return instance;
}

bool PipelineCoordinator::RunPipeline(
    float deltaMs,
    RE::PlayerCharacter* player,
    std::chrono::steady_clock::time_point now,
    bool pageChanged)
{
    auto* actorValue = player->AsActorValueOwner();
    if (!actorValue) {
        static bool s_warned = false;
        if (!s_warned) {
            logger::warn("[Pipeline] Player AsActorValueOwner() returned null — pipeline disabled"sv);
            s_warned = true;
        }
        return false;
    }

    Huginn_ZONE_NAMED("RunPipeline");

    m_ctx.Reset();
    m_ctx.deltaMs = deltaMs;
    m_ctx.player = player;
    m_ctx.actorValue = actorValue;
    m_ctx.now = now;

    GatherState(m_ctx);

    if (CheckHashSkip(m_ctx, pageChanged)) {
        return false;  // Hash unchanged, pipeline skipped
    }

    LogStateTransition(m_ctx);
    EnrichElementalDamage(m_ctx);
    ScoreCandidates(m_ctx);
    AllocateAndLock(m_ctx);
    UpdateCaches(m_ctx);
    PushDisplay(m_ctx);

#ifndef NDEBUG
    UpdateDebugWidgets(m_ctx);
#endif

    return true;
}

// -----------------------------------------------------------------------------
// GatherState — Fetch all state snapshots from StateManager
// -----------------------------------------------------------------------------

void PipelineCoordinator::GatherState(PipelineContext& ctx)
{
    Huginn_ZONE_NAMED("Pipeline::GatherState");
    auto& stateManager = State::StateManager::GetSingleton();

    // Fetch each state snapshot once (single lock acquisition each)
    ctx.worldState = stateManager.GetWorldState();
    ctx.playerState = stateManager.GetPlayerState();
    ctx.targets = stateManager.GetTargets();
    ctx.healthTracking = stateManager.GetHealthTracking();
    ctx.magickaTracking = stateManager.GetMagickaTracking();
    ctx.staminaTracking = stateManager.GetStaminaTracking();

    // Evaluate GameState from the already-fetched snapshots (no extra copies)
    ctx.currentState = g_stateEvaluator->EvaluateCurrentState(
        ctx.worldState, ctx.playerState, ctx.targets);
    ctx.stateHash = ctx.currentState.GetHash();

    // Check if elemental damage requires pipeline run despite unchanged hash
    constexpr float kElementalWindow = State::VitalTracking::ELEMENTAL_DAMAGE_ENRICHMENT_WINDOW;
    ctx.elementalDamageActive =
        ctx.healthTracking.timeSinceLastFire < kElementalWindow ||
        ctx.healthTracking.timeSinceLastFrost < kElementalWindow ||
        ctx.healthTracking.timeSinceLastShock < kElementalWindow;

    ctx.currentMagicka = ctx.actorValue->GetActorValue(RE::ActorValue::kMagicka);
}

// -----------------------------------------------------------------------------
// CheckHashSkip — Returns true if the pipeline should be skipped
// -----------------------------------------------------------------------------

bool PipelineCoordinator::CheckHashSkip(PipelineContext& ctx, bool pageChanged)
{
    if (ctx.stateHash == m_lastPipelineHash && !pageChanged && !ctx.elementalDamageActive) {
        // Keep cache timestamp fresh so external equip events aren't rejected as stale
        Learning::PipelineStateCache::GetSingleton().RefreshTimestamp();
        return true;  // Skip
    }
    m_lastPipelineHash = ctx.stateHash;
    return false;  // Don't skip
}

// -----------------------------------------------------------------------------
// LogStateTransition — Log what changed (not every tick)
// -----------------------------------------------------------------------------

void PipelineCoordinator::LogStateTransition(PipelineContext& ctx)
{
    if (ctx.stateHash != m_lastLoggedState.GetHash()) {
        auto changes = DiffGameState(m_lastLoggedState, ctx.currentState);
        logger::info("[Pipeline] State transition (hash={}) — scoring | {}", ctx.stateHash, changes);
        m_lastLoggedState = ctx.currentState;
    }
}

// -----------------------------------------------------------------------------
// EnrichElementalDamage — Bridge instant-hit detection → effect flags
// -----------------------------------------------------------------------------

void PipelineCoordinator::EnrichElementalDamage(PipelineContext& ctx)
{
    constexpr float kElementalWindow = State::VitalTracking::ELEMENTAL_DAMAGE_ENRICHMENT_WINDOW;

    // Log the enrichment window opening, not every tick inside it
    // NOTE: Single-threaded (called from update thread only)
    static bool s_wasOnFire = false, s_wasFrozen = false, s_wasShocked = false;

    const bool fireActive = ctx.healthTracking.timeSinceLastFire < kElementalWindow;
    if (fireActive) {
        ctx.playerState.effects.isOnFire = true;
        if (!s_wasOnFire) {
            logger::debug("[Enrichment] Fire damage detected {:.1f}s ago → isOnFire=true",
                ctx.healthTracking.timeSinceLastFire);
        }
    }
    s_wasOnFire = fireActive;

    const bool frostActive = ctx.healthTracking.timeSinceLastFrost < kElementalWindow;
    if (frostActive) {
        ctx.playerState.effects.isFrozen = true;
        if (!s_wasFrozen) {
            logger::debug("[Enrichment] Frost damage detected {:.1f}s ago → isFrozen=true",
                ctx.healthTracking.timeSinceLastFrost);
        }
    }
    s_wasFrozen = frostActive;

    const bool shockActive = ctx.healthTracking.timeSinceLastShock < kElementalWindow;
    if (shockActive) {
        ctx.playerState.effects.isShocked = true;
        if (!s_wasShocked) {
            logger::debug("[Enrichment] Shock damage detected {:.1f}s ago → isShocked=true",
                ctx.healthTracking.timeSinceLastShock);
        }
    }
    s_wasShocked = shockActive;
}

// -----------------------------------------------------------------------------
// ScoreCandidates — Generate and score all candidates
// -----------------------------------------------------------------------------

void PipelineCoordinator::ScoreCandidates(PipelineContext& ctx)
{
    Huginn_ZONE_NAMED("Pipeline::ScoreCandidates");
    auto& candidateGen = Candidate::CandidateGenerator::GetSingleton();

    auto candidates = candidateGen.GenerateCandidates(
        ctx.worldState, ctx.playerState, ctx.targets, ctx.currentMagicka,
        ctx.healthTracking, ctx.magickaTracking, ctx.staminaTracking);

    ctx.scoredCandidates = g_utilityScorer->ScoreCandidates(
        candidates, ctx.currentState, ctx.playerState, ctx.targets, ctx.worldState);
}

// -----------------------------------------------------------------------------
// AllocateAndLock — Override evaluation, slot allocation, locking, visual states
// -----------------------------------------------------------------------------

void PipelineCoordinator::AllocateAndLock(PipelineContext& ctx)
{
    Huginn_ZONE_NAMED("Pipeline::AllocateAndLock");
    // Evaluate safety overrides (critical health, drowning, weapon charge)
    auto& overrideMgr = Override::OverrideManager::GetSingleton();
    ctx.overrides = overrideMgr.EvaluateOverrides(ctx.playerState, ctx.worldState);

    // Multi-page classification-based slot assignment
    auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
    ctx.rawAssignments = slotAllocator.AllocateSlots(
        ctx.scoredCandidates, ctx.overrides, ctx.playerState, ctx.worldState);

    // Apply slot locking for temporal stability. ApplyLocks also dedups
    // post-lock (locked slots can reintroduce an item the allocator placed
    // elsewhere), preferring to keep locked content.
    auto& slotLocker = Slot::SlotLocker::GetSingleton();
    slotLocker.Update(ctx.deltaMs);
    ctx.assignments = slotLocker.ApplyLocks(ctx.rawAssignments, ctx.overrides);

    // Compute visual state for each slot
    Slot::ComputeVisualStates(ctx.assignments, ctx.rawAssignments, slotLocker);
}

// -----------------------------------------------------------------------------
// UpdateCaches — Pipeline state cache + EquipManager slot contents
// -----------------------------------------------------------------------------

void PipelineCoordinator::UpdateCaches(PipelineContext& ctx)
{
    // Cache pipeline state for external equip attribution.
    // sortedPrefix = topNCandidates: only that prefix of scoredCandidates is in
    // utility order (partial_sort); see PipelineStateCache::Update.
    auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
    Learning::PipelineStateCache::GetSingleton().Update(
        ctx.scoredCandidates, ctx.assignments,
        slotAllocator.GetCurrentPage(),
        g_utilityScorer->GetConfig().topNCandidates);

    // Cache slot contents for EquipManager (keyboard equip hotkeys)
    auto& equipMgr = Input::EquipManager::GetSingleton();
    for (const auto& assignment : ctx.assignments) {
        equipMgr.SetSlotContent(assignment.slotIndex, Slot::ToSlotContent(assignment));
    }
}

// -----------------------------------------------------------------------------
// PushDisplay — Auto-focus + display backend push
// -----------------------------------------------------------------------------

void PipelineCoordinator::PushDisplay(PipelineContext& ctx)
{
    Huginn_ZONE_NAMED("Pipeline::PushDisplay");
    // Compute urgent override state for Wheeler gating
    const auto* topOverride = ctx.overrides.GetTopOverride();
    int autoFocusThreshold = Wheeler::WheelerSettings::GetSingleton().GetAutoFocusMinPriority();
    ctx.hasUrgentOverride = topOverride && topOverride->priority >= autoFocusThreshold;

    // If urgent override + wheel open, try to auto-focus to Huginn wheel
    auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
    if (ctx.hasUrgentOverride && wheelerClient.IsWheelOpen()) {
        wheelerClient.TryUrgentAutoFocus(topOverride->priority);
    }

    // Sync display page with Wheeler's active managed page.
    // If Wheeler is viewing a different page than the allocator, re-sync and
    // re-allocate so all backends see consistent assignments.
    Slot::SlotAssignments displayAssignments;
    const Slot::SlotAssignments* assignmentsPtr = &ctx.assignments;

    auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
    if (wheelerClient.IsConnected()) {
        int wheelerPage = wheelerClient.GetActiveManagedPage();
        int currentPage = static_cast<int>(slotAllocator.GetCurrentPage());
        if (wheelerPage >= 0 && wheelerPage != currentPage) {
            slotAllocator.SetCurrentPage(static_cast<size_t>(wheelerPage));
            displayAssignments = slotAllocator.AllocateSlotsForPage(
                static_cast<size_t>(wheelerPage), ctx.scoredCandidates,
                ctx.overrides, ctx.playerState, ctx.worldState);
            assignmentsPtr = &displayAssignments;
        }
    }

    // Push to display backends
    Display::DisplayContext displayCtx{
        *assignmentsPtr, ctx.scoredCandidates, ctx.overrides,
        ctx.playerState, ctx.worldState, ctx.hasUrgentOverride, ctx.now
    };
    for (auto* backend : s_displayBackends) {
        if (backend->IsEnabled()) backend->Push(displayCtx);
    }
}

// -----------------------------------------------------------------------------
// UpdateDebugWidgets — Debug logging + UtilityScorerDebugWidget (debug only)
// -----------------------------------------------------------------------------

#ifndef NDEBUG
void PipelineCoordinator::UpdateDebugWidgets(PipelineContext& ctx)
{
    // Log top 5 candidates periodically + validate Wheeler state
    auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(ctx.now - m_lastDebugLog);
    if (elapsed.count() >= 5000) {
        g_utilityScorer->LogTopCandidates(ctx.scoredCandidates, 5);
        wheelerClient.ValidateWheelState();
        m_lastDebugLog = ctx.now;
    }

#ifdef _DEBUG  // Stricter than NDEBUG: ImGui widgets only in MSVC debug builds
    // Update UtilityScorer debug widget with per-slot page-aware data
    {
        auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
        size_t coldStartCount = 0;
        for (const auto& sc : ctx.scoredCandidates) {
            if (sc.isColdStartBoosted) ++coldStartCount;
        }
        UI::UtilityScorerDebugWidget::GetSingleton().UpdateSlotData(
            ctx.scoredCandidates,
            ctx.assignments,
            slotAllocator.GetCurrentPage(),
            slotAllocator.GetCurrentPageName(),
            slotAllocator.GetPageCount(),
            coldStartCount);
        if (g_usageMemory) {
            UI::UtilityScorerDebugWidget::GetSingleton().UpdateUsageMemory(
                g_usageMemory->GetSnapshot(), ctx.stateHash);
        }
    }
#endif
}
#endif

}  // namespace Huginn::Pipeline
