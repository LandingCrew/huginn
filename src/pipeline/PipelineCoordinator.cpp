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
#include <set>

#ifdef _DEBUG
#include "ui/UtilityScorerDebugWidget.h"
#endif

using namespace Huginn;

// =============================================================================
// VISUAL STATE COMPUTATION
// =============================================================================
// Enriches slot assignments with visual state flags based on lock timers,
// assignment history, and override priorities. Called after SlotLocker.ApplyLocks().
// =============================================================================

static void ComputeVisualStates(
    Slot::SlotAssignments& assignments,
    const Slot::SlotAssignments& rawAssignments,
    const Slot::SlotLocker& locker)
{
    // Expiring threshold: show pulse during last 40% of lock duration
    // (e.g., 3s lock → pulse starts at 1.2s remaining)
    const float lockDurationMs = locker.GetConfig().lockDurationMs;
    const float EXPIRING_THRESHOLD_MS = lockDurationMs * 0.4f;

    for (size_t i = 0; i < assignments.size(); ++i) {
        auto& assignment = assignments[i];

        // Priority 1: Override/Wildcard (highest visual priority)
        if (assignment.IsOverride()) {
            assignment.visualState = Slot::SlotVisualState::Override;
            continue;
        }
        if (assignment.IsWildcard()) {
            assignment.visualState = Slot::SlotVisualState::Wildcard;
            continue;
        }

        // Skip empty slots
        if (assignment.IsEmpty()) {
            assignment.visualState = Slot::SlotVisualState::Normal;
            continue;
        }

        // Priority 2: Confirmed (re-evaluated, same item)
        if (locker.WasConfirmed(assignment.slotIndex, assignment.formID)) {
            assignment.visualState = Slot::SlotVisualState::Confirmed;
            continue;
        }

        // Priority 3: Expiring (lock about to expire AND content will change)
        if (locker.IsSlotLocked(assignment.slotIndex)) {
            float remainingMs = locker.GetRemainingLockTime(assignment.slotIndex);

            if (remainingMs > 0.0f && remainingMs <= EXPIRING_THRESHOLD_MS) {
                // Check if content will actually change when lock expires
                bool contentWillChange = (i < rawAssignments.size() &&
                    rawAssignments[i].formID != 0 &&
                    rawAssignments[i].formID != assignment.formID);

                logger::trace("[VisualState] Slot {} expiring check: remainingMs={:.0f}, locked={:08X}, raw={:08X}, willChange={}",
                    i, remainingMs,
                    assignment.formID,
                    i < rawAssignments.size() ? rawAssignments[i].formID : 0,
                    contentWillChange);

                if (contentWillChange) {
                    assignment.visualState = Slot::SlotVisualState::Expiring;
                    logger::debug("[VisualState] Slot {} set to EXPIRING ({}ms remaining, {} -> {})",
                        i, remainingMs, assignment.name,
                        i < rawAssignments.size() ? rawAssignments[i].name : "empty");
                    continue;
                }
            }
        }

        // Default: normal state (no special effect)
        assignment.visualState = Slot::SlotVisualState::Normal;
    }

    // Log visual state summary (non-normal states only) — one condensed line
    std::string visualSummary;
    for (size_t i = 0; i < assignments.size(); ++i) {
        const auto& assignment = assignments[i];
        if (!assignment.IsEmpty() && assignment.visualState != Slot::SlotVisualState::Normal) {
            if (!visualSummary.empty()) visualSummary += ", ";
            visualSummary += fmt::format("{}:{}({})",
                i, Slot::SlotVisualStateToString(assignment.visualState), assignment.name);
        }
    }
    if (!visualSummary.empty()) {
        logger::debug("[VisualState] {}", visualSummary);
    }
}

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

    PipelineContext ctx;
    ctx.deltaMs = deltaMs;
    ctx.player = player;
    ctx.actorValue = actorValue;
    ctx.now = now;

    GatherState(ctx);

    if (CheckHashSkip(ctx, pageChanged)) {
        return false;  // Hash unchanged, pipeline skipped
    }

    LogStateTransition(ctx);
    EnrichElementalDamage(ctx);
    ScoreCandidates(ctx);
    AllocateAndLock(ctx);
    UpdateCaches(ctx);
    PushDisplay(ctx);

#ifndef NDEBUG
    UpdateDebugWidgets(ctx);
#endif

    return true;
}

// -----------------------------------------------------------------------------
// GatherState — Fetch all state snapshots from StateManager
// -----------------------------------------------------------------------------

void PipelineCoordinator::GatherState(PipelineContext& ctx)
{
    Huginn_ZONE_NAMED("Pipeline::GatherState");
    auto [currentState, playerState] = EvaluateCurrentGameState();
    ctx.currentState = currentState;
    ctx.playerState = playerState;

    auto& stateManager = State::StateManager::GetSingleton();

    ctx.healthTracking = stateManager.GetHealthTracking();
    ctx.stateHash = currentState.GetHash();

    // Check if elemental damage requires pipeline run despite unchanged hash
    constexpr float kElementalWindow = State::VitalTracking::ELEMENTAL_DAMAGE_ENRICHMENT_WINDOW;
    ctx.elementalDamageActive =
        ctx.healthTracking.timeSinceLastFire < kElementalWindow ||
        ctx.healthTracking.timeSinceLastFrost < kElementalWindow ||
        ctx.healthTracking.timeSinceLastShock < kElementalWindow;

    ctx.currentMagicka = ctx.actorValue->GetActorValue(RE::ActorValue::kMagicka);
    ctx.targets = stateManager.GetTargets();
    ctx.worldState = stateManager.GetWorldState();
    ctx.magickaTracking = stateManager.GetMagickaTracking();
    ctx.staminaTracking = stateManager.GetStaminaTracking();
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

    if (ctx.healthTracking.timeSinceLastFire < kElementalWindow) {
        ctx.playerState.effects.isOnFire = true;
        logger::debug("[Enrichment] Fire damage detected {:.1f}s ago → isOnFire=true",
            ctx.healthTracking.timeSinceLastFire);
    }
    if (ctx.healthTracking.timeSinceLastFrost < kElementalWindow) {
        ctx.playerState.effects.isFrozen = true;
        logger::debug("[Enrichment] Frost damage detected {:.1f}s ago → isFrozen=true",
            ctx.healthTracking.timeSinceLastFrost);
    }
    if (ctx.healthTracking.timeSinceLastShock < kElementalWindow) {
        ctx.playerState.effects.isShocked = true;
        logger::debug("[Enrichment] Shock damage detected {:.1f}s ago → isShocked=true",
            ctx.healthTracking.timeSinceLastShock);
    }
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

    // Apply slot locking for temporal stability
    auto& slotLocker = Slot::SlotLocker::GetSingleton();
    slotLocker.Update(ctx.deltaMs);
    ctx.assignments = slotLocker.ApplyLocks(ctx.rawAssignments, ctx.overrides);

    // Post-lock dedup: locked slots can reintroduce items that the allocator
    // already assigned elsewhere. Clear duplicates (keep first occurrence).
    {
        std::set<std::string_view> seenNames;
        for (auto& assignment : ctx.assignments) {
            if (assignment.IsEmpty()) continue;
            auto [it, inserted] = seenNames.insert(assignment.name);
            if (!inserted) {
                SKSE::log::debug("[Pipeline] Post-lock dedup: clearing duplicate '{}' from slot {}",
                    assignment.name, assignment.slotIndex);
                assignment = Slot::SlotAssignment::Empty(assignment.slotIndex, assignment.classification);
            }
        }
    }

    // Compute visual state for each slot
    ComputeVisualStates(ctx.assignments, ctx.rawAssignments, slotLocker);
}

// -----------------------------------------------------------------------------
// UpdateCaches — Pipeline state cache + EquipManager slot contents
// -----------------------------------------------------------------------------

void PipelineCoordinator::UpdateCaches(PipelineContext& ctx)
{
    // Cache pipeline state for external equip attribution
    auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
    Learning::PipelineStateCache::GetSingleton().Update(
        ctx.scoredCandidates, ctx.assignments,
        slotAllocator.GetCurrentPage());

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

    // Push to display backends
    Display::DisplayContext displayCtx{
        ctx.assignments, ctx.scoredCandidates, ctx.overrides,
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
