#include "PipelineCoordinator.h"
#include "Globals.h"
#include "../Profiling.h"

#include "state/GameState.h"       // DiffGameState (inline free function)
#include "state/StateManager.h"
#include "state/StateConstants.h"
#include "candidate/CandidateGenerator.h"
#include "context/ContextRuleEngine.h"   // Context::ContextReasonSignals (#10)
#include "override/OverrideManager.h"
#include "slot/SlotAllocator.h"
#include "slot/SlotLocker.h"
#include "slot/SlotUtils.h"
#include "input/EquipManager.h"
#include "wheeler/WheelerClient.h"  // debug-only: ValidateWheelState in UpdateDebugWidgets
#include "learning/PipelineStateCache.h"
#include "telemetry/SoakMetrics.h"
#include "ui/DebugSettings.h"
#include "display/IDisplayBackend.h"
#include "display/ExplanationLabel.h"  // ReasonLabel for the [Context] transition log
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

    // Resolve the display page up-front so allocation, caches, and push all
    // operate on ONE page. A Wheeler page change usually already set the
    // allocator page via OnWheelStateChanged (which also raised pageChanged);
    // this catches a polled drift where that callback didn't fire, and folds
    // into the skip decision so a page-only change still forces a run.
    const bool pageResynced = ResolveDisplayPage(m_ctx);

    if (CheckHashSkip(m_ctx, pageChanged || pageResynced)) {
        return false;  // Hash unchanged, pipeline skipped
    }

    LogStateTransition(m_ctx);
    EnrichElementalDamage(m_ctx);
    ScoreCandidates(m_ctx);
    if (!AllocateAndLock(m_ctx)) {
        // A page switch landed mid-tick; abandon rather than lock/cache/push the
        // stale page. The switch raised m_pageChanged, so next tick re-runs on it.
        return false;
    }
    DeriveDisplayLabels(m_ctx);
    UpdateCaches(m_ctx);
    PushDisplay(m_ctx);
    LogRecommendations(m_ctx);

#ifndef NDEBUG
    UpdateDebugWidgets(m_ctx);
#endif

    // Soak telemetry: this was a real recompute — record candidate/display counts
    // and whether a safety override took the top slot.
    size_t displayedCount = 0;
    for (const auto& assignment : m_ctx.assignments) {
        if (!assignment.IsEmpty() && assignment.formID != 0) {
            ++displayedCount;
        }
    }
    Telemetry::SoakMetrics::GetSingleton().RecordPipelineRun(
        m_ctx.scoredCandidates.size(), displayedCount,
        m_ctx.overrides.GetTopOverride() != nullptr);

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
    // Run during the elemental window AND once on its falling edge — the
    // enriched flags aren't in the hash, so the close of the window is
    // otherwise invisible and stale fire/frost/shock scoring would persist.
    const bool elementalBypass = ctx.elementalDamageActive || m_wasElementalDamageActive;

    if (ctx.stateHash == m_lastPipelineHash && !pageChanged && !elementalBypass) {
        // Keep cache timestamp fresh so external equip events aren't rejected as stale
        Learning::PipelineStateCache::GetSingleton().RefreshTimestamp();
        return true;  // Skip
    }
    m_lastPipelineHash = ctx.stateHash;
    m_wasElementalDamageActive = ctx.elementalDamageActive;
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

    auto candidates = candidateGen.GenerateCandidates(ctx.playerState, ctx.currentMagicka);

    Context::ContextWeightMap contextWeights{};
    ctx.scoredCandidates = g_utilityScorer->ScoreCandidates(
        candidates, ctx.currentState, ctx.playerState, ctx.targets, ctx.worldState,
        &contextWeights);

    // Name the dominant reason once per tick, off the weights the ranking just
    // used — the display explanation can't disagree with the scoring (#10).
    // The two world facts below have no scoring weight to read them off.
    ctx.contextReason = g_utilityScorer->DominantContextReason(
        contextWeights,
        Context::ContextReasonSignals{
            .allyInjured = ctx.targets.HasInjuredFollower(),
            .lookingAtOre = ctx.worldState.isLookingAtOreVein,
            .lightLevel = ctx.worldState.lightLevel,
        });

    // The reason drives a display string only, so nothing else in the log
    // reveals it — log the transition (not the tick) so a session can be
    // verified from the log instead of by watching the wheel. Same cadence as
    // the [Pipeline] state transition line: only fires on a threshold cross.
    //
    // The vitals ride along because they are the ASSERTABLE part: the
    // continuous rules invert their curve at fixed percentages (crit 15%, low
    // 30% HP/MP/SP, charge 25%), so a reader can take this line alone and
    // confirm the reported reason matches the numbers that produced it.
    //
    // Level is debug, not info: this is a behavior diagnostic, and behavior
    // soak runs are Debug builds (perf runs are Release+Tracy, where these
    // lines would only be noise). Meant to be read by an LLM or a log tool.
    // NOTE: Single-threaded (pipeline runs on the update thread only).
    static Context::ContextReason s_lastReason = Context::ContextReason::None;
    if (ctx.contextReason != s_lastReason) {
        const auto label = Display::ReasonLabel(ctx.contextReason);
        const auto prev = Display::ReasonLabel(s_lastReason);
        const auto& vitals = ctx.playerState.vitals;
        // One decimal, not zero: the thresholds these lines exist to verify sit
        // ON integer percentages (15 / 25 / 30). At {:.0f} a true 15.4% prints
        // as "15%", so the one reading that decides whether the boundary held is
        // exactly the reading the format destroys.
        logger::debug("[Context] Reason: {} → {} | hp={:.1f}% mp={:.1f}% sp={:.1f}% charge={}"sv,
            prev.empty() ? "(none)"sv : prev,
            label.empty() ? "(none)"sv : label,
            vitals.health * 100.0f, vitals.magicka * 100.0f, vitals.stamina * 100.0f,
            ctx.playerState.hasEnchantedWeapon
                ? fmt::format("{:.1f}%", ctx.playerState.weaponChargePercent * 100.0f)
                : "n/a");
        s_lastReason = ctx.contextReason;
    }
}

// -----------------------------------------------------------------------------
// AllocateAndLock — Override evaluation, slot allocation, locking, visual states
// -----------------------------------------------------------------------------

bool PipelineCoordinator::AllocateAndLock(PipelineContext& ctx)
{
    Huginn_ZONE_NAMED("Pipeline::AllocateAndLock");
    // Evaluate safety overrides (critical health, drowning, weapon charge)
    auto& overrideMgr = Override::OverrideManager::GetSingleton();
    ctx.overrides = overrideMgr.EvaluateOverrides(ctx.playerState, ctx.worldState);

    // Multi-page classification-based slot assignment. Allocate for the page
    // snapshotted by ResolveDisplayPage (not m_currentPage), so an off-thread
    // switch mid-tick can't produce assignments for a different page than the
    // caches/push use.
    auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
    ctx.rawAssignments = slotAllocator.AllocateSlotsForPage(
        ctx.displayPageIndex, ctx.scoredCandidates, ctx.overrides, ctx.playerState, ctx.worldState);

    // If an off-thread page mutation landed since the snapshot (Wheeler callback,
    // page-cycle keys, `hg page`, or a full SlotAllocator::Reset), abandon before
    // locking. SlotLocker is keyed by slot index only, so applying locks for THIS
    // page now would pin its content into the new page's slots: the switch's own
    // UnlockAll already fired, and next tick's ResolveDisplayPage sees the allocator
    // already moved, so it won't UnlockAll again — the stale locks would survive for
    // the lock duration.
    //
    // The abandoned tick is NOT side-effect-free (EvaluateOverrides above advanced
    // hysteresis; CheckHashSkip already committed the hash), so we OWN the re-run
    // via MarkPageDirty rather than trusting the mutator to have raised the
    // page-dirty flag — SlotAllocator::Reset() notably does not.
    //
    // NOTE: this check is not atomic with ApplyLocks; a switch landing in the few
    // instructions between them still leaks one page-blind lock. That shrinks the
    // window from ~1 ms to a handful of instructions — the airtight fix is
    // page-tagged locks, not worth it here.
    const size_t livePage = slotAllocator.GetCurrentPage();  // read once: compare AND log
    if (livePage != ctx.displayPageIndex) {
        slotAllocator.MarkPageDirty();  // guarantee the re-run; don't infer it
        // Counter surfaces in the info-level [Soak] heartbeat, so the race path is
        // observable on Release long-play runs (the debug line is Debug-only).
        Telemetry::SoakMetrics::GetSingleton().RecordPageRaceBail();
        logger::debug("[Pipeline] Page changed mid-tick ({} -> {}); abandoning tick to avoid stale locks"sv,
            ctx.displayPageIndex, livePage);
        return false;
    }

    // Apply slot locking for temporal stability. ApplyLocks also dedups
    // post-lock (locked slots can reintroduce an item the allocator placed
    // elsewhere), preferring to keep locked content. Lock-timer decay lives in
    // UpdateSubsystems (unconditional per tick), not here behind the skip gate.
    auto& slotLocker = Slot::SlotLocker::GetSingleton();
    ctx.assignments = slotLocker.ApplyLocks(ctx.rawAssignments, ctx.overrides);

    // Compute visual state for each slot
    Slot::ComputeVisualStates(ctx.assignments, ctx.rawAssignments, slotLocker);
    return true;
}

// -----------------------------------------------------------------------------
// UpdateCaches — Pipeline state cache + EquipManager slot contents
// -----------------------------------------------------------------------------

void PipelineCoordinator::UpdateCaches(PipelineContext& ctx)
{
    // Cache pipeline state for external equip attribution.
    // sortedPrefix = topNCandidates: only that prefix of scoredCandidates is in
    // utility order (partial_sort); see PipelineStateCache::Update. Page comes
    // from the tick snapshot so it matches ctx.assignments exactly.
    Learning::PipelineStateCache::GetSingleton().Update(
        ctx.scoredCandidates, ctx.assignments,
        ctx.displayPageIndex,
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
    // The active page was already resolved before AllocateAndLock (see
    // ResolveDisplayPage), so ctx.assignments are lock-stabilized, visual-stated,
    // and cached for the SAME page every backend is about to render. No mid-tick
    // re-sync here: doing it after AllocateAndLock/UpdateCaches would push raw,
    // unlocked assignments and leave PipelineStateCache/EquipManager stale.
    //
    // Backend-specific concerns (Wheeler's urgent-override auto-focus, page
    // ownership) now live in the backends themselves — the coordinator only
    // drives the generic IDisplayBackend contract.
    //
    // Page state comes from the tick snapshot (ResolveDisplayPage), NOT re-read
    // from the allocator here — re-reading would reopen a torn-page window if an
    // off-thread switch landed between allocation and push. displayPageName backs
    // the string_view for the push's duration (it lives on the reused m_ctx).
    Display::DisplayContext displayCtx{
        .assignments = ctx.assignments,
        .scoredCandidates = ctx.scoredCandidates,
        .overrides = ctx.overrides,
        .playerState = ctx.playerState,
        .worldState = ctx.worldState,
        .pageIndex = ctx.displayPageIndex,
        .pageCount = ctx.displayPageCount,
        .slotCount = ctx.displaySlotCount,
        .pageName = ctx.displayPageName,
        .contextReason = ctx.contextReason,
        .now = ctx.now,
    };
    for (auto* backend : s_displayBackends) {
        if (backend->IsEnabled()) backend->Push(displayCtx);
    }
}

// -----------------------------------------------------------------------------
// ResolveDisplayPage — Sync allocator's active page to the display's desired page
// -----------------------------------------------------------------------------
// Runs BEFORE AllocateAndLock so the whole tick is page-consistent. Asks each
// enabled backend which page it wants (IDisplayBackend::GetDesiredPage) and takes
// the first opinion — today only Wheeler drives the page (via its managed active
// wheel); a second paged display could participate without touching this code.
//
// Wheeler normally drives the page via OnWheelStateChanged → SetCurrentPage on
// its callback thread; this poll catches drift where the allocator page and the
// backend's reported page diverge (callback missed / state-hash run while the
// player sits on another page). SetCurrentPage is a no-op unless the page
// actually differs, and on a real change it clears stale locks — correct here,
// before ApplyLocks runs for the new page.
//
// The resolved page is snapshotted onto the context so allocation, caches, and
// push all read ONE page for the whole tick — an off-thread switch landing
// mid-tick can't tear assignments from their page metadata (it just raises
// m_pageChanged and re-runs next tick). Returns true only if the page actually
// changed, so a page-only change forces a run without pinning the skip gate open.

bool PipelineCoordinator::ResolveDisplayPage(PipelineContext& ctx)
{
    auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
    const size_t pageCount = slotAllocator.GetPageCount();

    int desiredPage = -1;
    for (auto* backend : s_displayBackends) {
        if (!backend->IsEnabled()) continue;
        const int p = backend->GetDesiredPage();
        if (p >= 0) { desiredPage = p; break; }
    }

    // Only apply a valid, actually-different page. SetCurrentPage clamps an
    // out-of-range index (SlotAllocator.cpp) — which would never converge here,
    // warn-spamming ~10/s and pinning pageResynced=true (hash-skip disabled). A
    // transient over-range desire is reachable while SettingsReloader shrinks
    // pages before its wheels are rebuilt, so guard rather than trust the caller.
    // Dedup the out-of-range warn across ticks, re-armed on the falling edge (a
    // valid page seen) so a page that goes bad again later re-logs.
    static int s_lastBadPage = -1;
    bool changed = false;
    if (desiredPage >= 0 && desiredPage < static_cast<int>(pageCount)) {
        const size_t before = slotAllocator.GetCurrentPage();
        slotAllocator.SetCurrentPage(static_cast<size_t>(desiredPage));
        changed = slotAllocator.GetCurrentPage() != before;
        s_lastBadPage = -1;  // re-arm
    } else if (desiredPage >= 0 && desiredPage != s_lastBadPage) {
        // Keep the ignored out-of-range request diagnosable without per-tick spam.
        logger::debug("[Pipeline] Desired display page {} out of range (pages={}), ignoring"sv,
            desiredPage, pageCount);
        s_lastBadPage = desiredPage;
    }

    // Snapshot the resolved page for the whole tick. Both slot count and name are
    // by-index (not read off m_currentPage), so all four fields describe the same
    // page even if a switch lands between these statements — and they match the
    // page AllocateSlotsForPage(displayPageIndex) produces below.
    ctx.displayPageIndex = slotAllocator.GetCurrentPage();
    ctx.displayPageCount = pageCount;
    ctx.displaySlotCount = slotAllocator.GetSlotCount(ctx.displayPageIndex);
    ctx.displayPageName  = slotAllocator.GetPageName(ctx.displayPageIndex);

    return changed;
}

// -----------------------------------------------------------------------------
// LogRecommendations — Top-N recommendation logging (both build configs)
// -----------------------------------------------------------------------------
// Periodic path: throttled + membership-deduped, gated by DebugSettings
// recLogVerbosity (0=off, 1=compact, 2=detail). Dump path: one-shot full-detail
// dump queued by `hg recs`, bypasses verbosity/throttle/dedup and also logs the
// current slot assignments (safe: SlotAssignment.name is an owned string).

void PipelineCoordinator::LogRecommendations(PipelineContext& ctx)
{
    if (size_t dumpN = m_recDumpRequest.exchange(0, std::memory_order_relaxed); dumpN > 0) {
        g_utilityScorer->LogTopCandidates(ctx.scoredCandidates, dumpN,
            /*detail=*/true, /*force=*/true);

        // Label with the tick's snapshot page, not a live re-read, so the dump
        // matches the assignments it prints.
        logger::info("[Recs] Slots (page {} '{}'):"sv,
            ctx.displayPageIndex, ctx.displayPageName);
        for (const auto& a : ctx.assignments) {
            if (a.IsEmpty() || a.formID == 0) continue;
            logger::info("[Recs]   slot {}: {} ({:08X}) u={:.3f}{}"sv,
                a.slotIndex, a.name, a.formID, a.utility,
                a.subtextLabel.empty() ? "" : fmt::format(" [{}]", a.subtextLabel));
        }
        return;  // The dump covers this tick; skip the periodic log
    }

    const int verbosity = UI::DebugSettings::GetSingleton().recLogVerbosity;
    if (verbosity <= 0) return;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(ctx.now - m_lastRecLog);
    if (elapsed.count() >= 5000) {
        g_utilityScorer->LogTopCandidates(ctx.scoredCandidates, 5,
            /*detail=*/verbosity >= 2, /*force=*/false);
        m_lastRecLog = ctx.now;
    }
}

// -----------------------------------------------------------------------------
// DeriveDisplayLabels — What the player is actually shown (both build configs)
// -----------------------------------------------------------------------------
// Derives the explanation subtext once, on the tick's own assignments, so that
//   (a) `hg recs` prints the real label instead of an empty string, and
//   (b) the displayed surface is verifiable from a log alone — no reading
//       labels off the wheel by hand, and no dependency on Wheeler at all.
//
// This MUTATES ctx.assignments; the [Subtext] line below is the diagnostic half.
// The labels stamped here are the coordinator's own view. Wheeler does NOT
// inherit them: WheelerBackend clears subtextLabel and re-derives under its own
// [Subtexts] toggles (override / lock timer / wildcard / explanation), so the
// INI toggles keep working and the wildcard label keeps its slot. Both sides
// call the same pure Display::DeriveExplanationLabel, so the explanation text
// itself cannot diverge. Transition-gated: one line when the label set changes.

void PipelineCoordinator::DeriveDisplayLabels(PipelineContext& ctx)
{
    // Filling subtextLabel is NOT diagnostics: it is what `hg recs` prints, so
    // it happens every tick at every log level. Only the summary line below is
    // debug-gated.
    for (auto& assignment : ctx.assignments) {
        assignment.subtextLabel = Display::DeriveExplanationLabel(assignment, ctx.contextReason);
    }

    // Behavior diagnostic, read by an LLM or a log tool rather than by eye.
    // Bail before building the string in Release, where debug is filtered out
    // and the summary could never be emitted anyway.
    if (!spdlog::default_logger()->should_log(spdlog::level::debug)) {
        return;
    }

    std::string summary;
    for (const auto& assignment : ctx.assignments) {
        if (assignment.IsEmpty() || assignment.formID == 0 || assignment.subtextLabel.empty()) {
            continue;
        }
        if (!summary.empty()) summary += ' ';
        summary += fmt::format("{}={}({})",
            assignment.slotIndex, assignment.subtextLabel, assignment.name);
    }

    // NOTE: Single-threaded (pipeline runs on the update thread only).
    static std::string s_lastSummary;
    if (summary == s_lastSummary) {
        return;
    }
    s_lastSummary = summary;

    if (summary.empty()) {
        logger::debug("[Subtext] page {} — no labels"sv, ctx.displayPageIndex);
    } else {
        logger::debug("[Subtext] page {} | {}"sv, ctx.displayPageIndex, summary);
    }
}

// -----------------------------------------------------------------------------
// UpdateDebugWidgets — Wheeler validation + UtilityScorerDebugWidget (debug only)
// -----------------------------------------------------------------------------

#ifndef NDEBUG
void PipelineCoordinator::UpdateDebugWidgets(PipelineContext& ctx)
{
    // Validate Wheeler state periodically
    auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(ctx.now - m_lastDebugLog);
    if (elapsed.count() >= 5000) {
        wheelerClient.ValidateWheelState();
        m_lastDebugLog = ctx.now;
    }

#ifdef _DEBUG  // Stricter than NDEBUG: ImGui widgets only in MSVC debug builds
    // Update UtilityScorer debug widget with per-slot page-aware data (tick snapshot)
    {
        size_t coldStartCount = 0;
        for (const auto& sc : ctx.scoredCandidates) {
            if (sc.isColdStartBoosted) ++coldStartCount;
        }
        UI::UtilityScorerDebugWidget::GetSingleton().UpdateSlotData(
            ctx.scoredCandidates,
            ctx.assignments,
            ctx.displayPageIndex,
            ctx.displayPageName,
            ctx.displayPageCount,
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
