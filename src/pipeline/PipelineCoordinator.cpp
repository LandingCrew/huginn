#include "PipelineCoordinator.h"
#include "Globals.h"
#include "../Profiling.h"

#include "state/GameState.h"       // DiffGameState (inline free function)
#include "state/StateManager.h"
#include "state/StateConstants.h"
#include "candidate/CandidateGenerator.h"
#include "context/ContextFacts.h"   // the hard gates' facts and the skip gate's (R8.4)
#include "override/OverrideManager.h"
#include "override/OverrideConfig.h"
#include "core/BucketHysteresis.h"
#include "core/CombatLock.h"
#include "core/CombatOnset.h"   // the one combat clock (R8.1)
#include "util/InventoryUtil.h"
#include "slot/SlotAllocator.h"
#include "slot/SlotLocker.h"
#include "slot/SlotSettings.h"   // bucket hysteresis bands
#include "slot/Remembrance.h"
#include "slot/SlotUtils.h"
#include "input/EquipManager.h"
#include "wheeler/WheelerClient.h"  // IsWheelOpen (combat upsert); ValidateWheelState in UpdateDebugWidgets
#include "learning/PipelineStateCache.h"
#include "learning/ChoiceLearner.h"
#include "learning/ChoiceShadow.h"
#include "core/ChoiceBeliefs.h"   // the run's beliefs for the challenger rule (R9)
#include "core/ChoiceLive.h"
#include "core/Coverage.h"
#include "core/LegacyShadowPass.h"    // the D2 shadow (0.24.17)
#include "effect/CrossFeatures.h"
#include "effect/EffectCatalog.h"
#include "effect/Facets.h"
#include "needs/NeedMonitor.h"
#include "telemetry/SoakMetrics.h"
#include "ui/DebugSettings.h"
#include "display/IDisplayBackend.h"
#include "display/ExplanationLabel.h"  // ReasonLabel for the [Context] transition log
#include "display/NeedReason.h"        // an explanation's need as a context reason (R8.3)
#include "display/WheelerBackend.h"
#include "display/IntuitionBackend.h"
#include "display/WheelUidFilter.h"   // uid-less weapons off a wheel page

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
    ShadowLegacyPages(m_ctx);   // 0.24.17: D2 in shadow (no-op unless it runs)
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
    // "An override took a slot", not "an override condition was true". A
    // condition can hold with nothing to offer — drowning with no waterbreathing
    // potion is the ordinary case — and counting those inflates the soak
    // override rate with ticks where the player was shown nothing.
    const auto* topOverride = m_ctx.overrides.GetTopOverride();
    const bool wildcardShown = std::ranges::any_of(m_ctx.assignments,
        [](const Slot::SlotAssignment& a) { return a.IsWildcard(); });
    Telemetry::SoakMetrics::GetSingleton().RecordPipelineRun(
        m_ctx.scoredCandidates.size(), displayedCount,
        topOverride != nullptr && topOverride->candidate.has_value(), wildcardShown);

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

    // Evaluate GameState from the already-fetched snapshots (no extra copies).
    // The one committing evaluation: the HP/MP/SP and distance buckets carry
    // hysteresis (fVitalBucketHysteresis / fDistanceBucketHysteresis), so a
    // value wobbling on a boundary stops re-scoring the page; the memory moves
    // here only, once per run. Overrides still read the raw vitals.
    const auto& slotSettings = Slot::SlotSettings::GetSingleton();
    ctx.currentState = g_stateEvaluator->EvaluateAndCommit(
        ctx.playerState, ctx.targets,
        State::StateEvaluator::BucketBands{
            .vitalBand = slotSettings.VitalBucketHysteresis(),
            .distanceFraction = slotSettings.DistanceBucketHysteresis(),
        });
    ctx.stateHash = ctx.currentState.GetHash();

    // R8.3: the need signature of this tick (NeedMonitor ran just before the
    // gates). It replaced the elemental window and the ambient signature as
    // skip-gate terms: fire/frost/shock damage rates decay with time, and
    // darkness, hunger and cold are needs.
    ctx.needSignature = Needs::NeedMonitor::GetSingleton().LatestSignatureHash();

    // Falling is not in the GameState hash either, so a fall that changes
    // nothing else is invisible to the
    // skip check. That was masked while isFalling came from IsInMidair(), since
    // jumps mostly happen mid-combat where the hash is moving anyway. Restricted
    // to real falls (#60), the context is usually a solitary event — stepping
    // off a ledge changes no bucket at all — and would never be scored.
    ctx.fallingActive = ctx.playerState.isFalling;

    // Underwater, for the same reason (#61). Submerging changes no hashed
    // bucket — swimming is not sneaking, combat or a target — so without this
    // the tick that flips it can hash-skip and the context never gets named.
    ctx.underwaterActive = ctx.playerState.isUnderwater;

    // Standing at a crafting station, for the same reason (#63/#65). Walking up
    // to a forge changes no hashed bucket — a bench is not a target, an enemy or
    // a vital — so the tick that flips it hash-skips and the fortify weights
    // never reach a slot.
    //
    // This is why the workstation context looked inert in play while unit test
    // 6h passed: 6h calls the context rules directly and never goes through the
    // skip gate. Observed 2026-09-08 — the player stood at an alchemy lab with a
    // recognised craft item in the registry and the pipeline last scored 17
    // seconds before they arrived.
    //
    // Held ACTIVE rather than edge-latched, matching underwaterActive: a player
    // can stand at a bench for minutes, and the slot has to stay populated the
    // whole time, not just on the tick they walk up.
    //
    // Gated on the bench mapping to a craft skill (CraftSkillForWorkstation,
    // the mapping the no_workstation gate reads through
    // Core::Gate::CraftOfBench), not merely on being furniture with a bench
    // type. Since R8.3 the workstation_* needs read the bench by its keyword
    // (core/BenchKind.h) and open the gate through the need signature on their
    // own; this flag keeps the gate's drops current while the player stands
    // there. For a modded bench outside that mapping the gate has nothing to
    // change, and holding
    // this true would run the full gather/score/allocate path every 100 ms for
    // as long as the player's crosshair rested on it — exactly the workload the
    // skip gate exists to avoid.
    ctx.workstationActive = ctx.worldState.isLookingAtWorkstation &&
        Context::CraftSkillForWorkstation(ctx.worldState.workstationType) !=
            Apparel::CraftSkill::None;

    // The third instance of this shape — a pending reason downgrade (#62) — has
    // no ctx field: unlike these two it has no "current" reading to take here,
    // because ReasonHold only advances after the allocation (R8.3: UpdateContextReason), below the skip check.
    // Both gates read the latch straight off the member via NeedsForcedRun().

    ctx.currentMagicka = ctx.actorValue->GetActorValue(RE::ActorValue::kMagicka);

    // R8.1: the refresh pass's bits, as the magic-effects poll left them.
    ctx.refreshDue = ctx.playerState.buffs.refreshDue;

    // R8.1: the one combat clock (combat_onset's), for the discriminator.
    ctx.combatOnsetAgo = Core::Needs::CombatOnsetAgo(ctx.playerState.isInCombat,
        stateManager.GetCombatStartAt(), State::NeedClock::Now());

    // Override thresholds on the RAW vitals (see PipelineContext::overrideSignature).
    {
        namespace OC = Override::Config;
        const auto& v = ctx.playerState.vitals;
        const int hp = Core::OverrideZone(v.health, OC::CRITICAL_HEALTH_THRESHOLD(), OC::CRITICAL_HEALTH_HYSTERESIS());
        const int mp = Core::OverrideZone(v.magicka, OC::CRITICAL_MAGICKA_THRESHOLD(), OC::CRITICAL_MAGICKA_HYSTERESIS());
        const int sp = Core::OverrideZone(v.stamina, OC::CRITICAL_STAMINA_THRESHOLD(), OC::CRITICAL_STAMINA_HYSTERESIS());
        ctx.overrideSignature = static_cast<uint8_t>(hp | (mp << 2) | (sp << 4));
    }
}

// -----------------------------------------------------------------------------
// CheckHashSkip — Returns true if the pipeline should be skipped
// -----------------------------------------------------------------------------

bool PipelineCoordinator::CheckHashSkip(PipelineContext& ctx, bool pageChanged)
{
    // Rising edge: state gathered THIS tick that the hash cannot see -- a fall
    // in progress, being underwater, standing at a bench. The falling edges
    // live in NeedsForcedRun(), which grants the one further run each needs to
    // clear itself. A pending reason downgrade is latch-only and rides along
    // there too -- see NeedsForcedRun.
    //
    // All bounded: a fall is over in about a second, and a hold expires within
    // REASON_HOLD_MS / UPDATE_INTERVAL_MS runs (~15) and self-clears the moment
    // the downgrade lands.
    const bool unhashedStateActive = ctx.fallingActive || ctx.underwaterActive || ctx.workstationActive;

    // One run on each change, either way. R8.3: the need signature -- any
    // need moving a 0.05 step (core/NeedEvaluator.h), which is the point now
    // that the score reads the needs; it replaced the ambient signature
    // (darkness, hunger, cold) and the elemental window (the damage-rate
    // needs decay with time, so their close moves it too). An override
    // threshold crossed. R8.1: a buff entering (or leaving) its refresh
    // window, so the active-buff filter lets its item through.
    const bool signatureChanged = ctx.needSignature != m_lastNeedSignature.load(std::memory_order_relaxed) ||
                                ctx.overrideSignature != m_lastOverrideSignature ||
                                ctx.refreshDue != m_lastRefreshDue;

    if (ctx.stateHash == m_lastPipelineHash && !pageChanged &&
        !unhashedStateActive && !signatureChanged && !NeedsForcedRun()) {
        // Keep cache timestamp fresh so external equip events aren't rejected as stale
        Learning::PipelineStateCache::GetSingleton().RefreshTimestamp();
        return true;  // Skip
    }
    m_lastPipelineHash = ctx.stateHash;
    m_wasFalling = ctx.fallingActive;
    m_wasUnderwater = ctx.underwaterActive;
    m_lastNeedSignature.store(ctx.needSignature, std::memory_order_relaxed);
    m_lastOverrideSignature = ctx.overrideSignature;
    m_lastRefreshDue = ctx.refreshDue;

    // Commit point for the learner latch: everything below this line scores and
    // publishes, so whatever reward set the flag is about to reach the widget.
    // Cleared here rather than in NeedsForcedRun() because that one is asked by
    // both gates and by neither exclusively — a consuming read there would let
    // the outer gate eat the flag and the inner gate skip anyway.
    // The old learner's (a pick it trained on moves the D2 feature).
    Scoring::Legacy::ClearWeightsChanged();
    // R8.3: and the choice learner's (theta or b moved: the ranking did).
    if (g_choiceLearner) {
        g_choiceLearner->ClearWeightsChanged();
    }

    return false;  // Don't skip
}

bool PipelineCoordinator::LearnerWeightsChanged() noexcept
{
    return Scoring::Legacy::WeightsChanged() ||
           (g_choiceLearner && g_choiceLearner->WeightsChanged());
}

bool PipelineCoordinator::NeedSignatureMoved() const noexcept
{
    return Needs::NeedMonitor::GetSingleton().LatestSignatureHash() !=
           m_lastNeedSignature.load(std::memory_order_relaxed);
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
// ScoreCandidates — Generate, then rank every candidate on the choice score
// -----------------------------------------------------------------------------
// R8.3 (0.24.0): THE CUTOVER. Every candidate that passes the filters is
// ranked on mu (core/ChoiceScore.h through core/ChoiceLive.h): theta^T n over
// its cap(i), b, recent_<family>, covered(i), and the D2 legacy feature
// (ln learn mult, w = 1.36 in the R6 bootstrap) on the rows the old floors
// keep. The old floors no longer drop rows from the ranking; they only decide
// where that feature applies. The legacy pass (the old context rules, learner,
// prior, correlations, potion multiplier and floors; learning/legacy/) runs
// only for it, behind [Scoring] bLegacyLearnFeature (0.24.17: default off,
// engine B; and for the D2 shadow, bShadowLegacy). R8.4: this file names the
// old engine in exactly one call (the pass, below); everything else it reads
// of it comes through learning/legacy/LegacyRun.h (tools/prune/r8_check.py).
//
// Without a usable bootstrap the pipeline ranks on the old utility instead
// (ln u, the R7 bridge) over the rows the floors keep: the FALLBACK, so the
// game stays playable; `hg status` and an error line say so. It has no
// favourites rank scaling and no potion tier preference any more, and since
// R8.4 it is built in the legacy module (Legacy::Run::FallbackRanking).

namespace
{
    /// The stack facts the effect catalog's runtime cross-features read
    /// (count, charge), as the selection log v3 reads them off a candidate.
    struct StackFacts
    {
        int32_t count = 0;
        bool enchanted = false;
        float chargeFraction = 0.0f;
    };

    StackFacts StackOf(const Candidate::CandidateVariant& c)
    {
        StackFacts s;
        std::visit([&s](const auto& cand) {
            using T = std::decay_t<decltype(cand)>;
            if constexpr (std::is_same_v<T, Candidate::ItemCandidate> || std::is_same_v<T, Candidate::AmmoCandidate> ||
                          std::is_same_v<T, Candidate::ScrollCandidate> || std::is_same_v<T, Candidate::TorchCandidate>) {
                s.count = cand.count;
            }
            else if constexpr (std::is_same_v<T, Candidate::WeaponCandidate>) {
                s.enchanted = cand.hasEnchantment;
                s.chargeFraction = cand.currentCharge;   // already current / max (WeaponRegistry)
            }
        }, c);
        return s;
    }

    /// cap(i)'s attributes and the runtime cross-features of one catalog
    /// entry for a stack: what the choice score ranks on (R8.3) and the
    /// selection log v3 records.
    Core::Choice::Attr AttrOf(const Core::Choice::Model& model, const Effect::CatalogEntry& entry,
        const State::PlayerActorState& player, const StackFacts& sf)
    {
        namespace DL = Core::DecisionLog;
        Effect::StackInfo stack;
        stack.count = sf.count;
        if (sf.enchanted) {
            stack.charge = sf.chargeFraction;
            stack.maxCharge = 1.0f;
        }
        const auto f = Effect::ComputeRuntimeFeatures(entry, player, stack);
        std::array<float, DL::kCrossCount> cross{};
        cross[static_cast<size_t>(DL::Cross::overshoot_health)] = f.overshootHealth;
        cross[static_cast<size_t>(DL::Cross::overshoot_magicka)] = f.overshootMagicka;
        cross[static_cast<size_t>(DL::Cross::overshoot_stamina)] = f.overshootStamina;
        cross[static_cast<size_t>(DL::Cross::weapon_charge)] = f.weaponCharge;
        cross[static_cast<size_t>(DL::Cross::stack_count)] = f.stackCount;
        cross[static_cast<size_t>(DL::Cross::ammo_matches_launcher)] = f.ammoMatchesLauncher;
        cross[static_cast<size_t>(DL::Cross::school_fortified)] = f.schoolFortified;
        return Core::Choice::MakeAttr(model.grading, &entry.cap, cross);
    }
}

void PipelineCoordinator::ScoreCandidates(PipelineContext& ctx)
{
    Huginn_ZONE_NAMED("Pipeline::ScoreCandidates");
    auto& candidateGen = Candidate::CandidateGenerator::GetSingleton();

    // Held items the affordability filter or a hard gate would drop come back
    // separately, for the hold alone (see the Remembrance loop below).
    const auto heldIDs = Slot::Remembrance::GetSingleton().ActiveFormIDs();
    std::vector<Candidate::CandidateVariant> heldAside;
    // R8.1 (0.23.55): the hard gates read the same state the legacy context
    // rules do (Context::HardGateInputs), and the favourites mode the scorer
    // used to skip Suppress favourites by.
    const auto gates = Context::HardGateInputs(ctx.playerState, ctx.targets, ctx.worldState,
        g_utilityScorer->GetConfig().favoritesMode == Scoring::FavoritesMode::Suppress);
    auto candidates = candidateGen.GenerateCandidates(ctx.playerState, ctx.currentMagicka, gates,
        heldIDs, &heldAside, &ctx.filtered);

    // Read once for the tick: the wildcard key's plan and the allocation in
    // AllocateAndLock must agree on it.
    ctx.hideUidlessGear = Display::HidesUidlessGear(ctx.displayPageIndex);

    // Which model ranks, and whether the legacy pass runs. Logged on change.
    // 0.24.17: engine B (no D2 feature) is the default live engine and D2
    // (engine A) runs in shadow beside it: the legacy pass runs for the live
    // score (bLegacyLearnFeature on), for the shadow (off, bShadowLegacy on),
    // or for the fallback, which ranks on the old utility.
    ctx.choiceLive = g_choiceLearner && g_choiceLearner->Ready();
    const bool legacyOn = Scoring::Legacy::Enabled();
    const bool shadowOn = !legacyOn && ctx.choiceLive && Scoring::Legacy::ShadowEnabled();
    const bool runLegacy = legacyOn || shadowOn || !ctx.choiceLive;   // the fallback needs the utility
    ctx.legacyLive = legacyOn;
    {
        // NOTE: Single-threaded (pipeline runs on the update thread only)
        static int s_lastMode = -1;
        const int mode = (ctx.choiceLive ? 1 : 0) | (legacyOn ? 2 : 0) | (shadowOn ? 4 : 0);
        if (mode != s_lastMode) {
            if (!ctx.choiceLive) {
                logger::error("[Pipeline] Ranking on the OLD UTILITY (fallback): the bootstrap is missing or invalid "
                              "(see the [Choice] line at load)"sv);
            }
            else {
                logger::info("[Pipeline] Ranking on the choice score{}"sv,
                    legacyOn   ? " with the D2 legacy feature (engine A: bLegacyLearnFeature on; no shadow)"
                    : shadowOn ? " without the legacy feature (engine B: bLegacyLearnFeature off), D2 in shadow "
                                 "(bShadowLegacy on: logged, never shown)"
                               : " without the legacy feature (engine B: bLegacyLearnFeature off), no shadow "
                                 "(bShadowLegacy off)");
            }
            s_lastMode = mode;
        }
    }

    // The legacy pass: the old floors per candidate (the D2 feature), the row
    // each candidate reads (its own, or the kept one of its dedup key) and the
    // rows they keep with their utility. Off: none of it runs (no context
    // rules, prior, correlation or potion work). THE one call into the old
    // engine (R8.4, tools/prune/r8_check.py).
    if (runLegacy) {
        ctx.legacy = Scoring::Legacy::LegacyPass(candidates, ctx.currentState, ctx.playerState, ctx.targets, ctx.worldState, ctx.combatOnsetAgo);
    }
    else {
        ctx.legacy.Skip(candidates.size());
    }

    if (ctx.choiceLive) {
        ctx.scoredCandidates = RankOnChoice(ctx, candidates);
    }
    else {
        // FALLBACK (no bootstrap, or an invalid one; logged above as an
        // error): the old ranking, ln utility over the rows the floors kept.
        // It lives in the legacy module (Legacy::Run::FallbackRanking, R8.4):
        // nothing in this file turns a utility into a rank score.
        ctx.scoredCandidates = ctx.legacy.FallbackRanking();
    }

    // The whole list in rank order. R10: no wildcard swap here; the
    // wildcard has a key of its own, placed and drawn by the allocation
    // (AllocateAndLock: WildcardManager::Plan, the allocation, Commit).
    g_utilityScorer->Rank(ctx.scoredCandidates);

    // A Remembrance hold shows what the player took off whether or not the
    // ranking kept it. Since R8.3 every candidate is ranked, so this adds
    // only a held item the filters dropped (an unaffordable spell, from the
    // aside list; R8.1: a held item a hard gate dropped: a torch in daylight,
    // a ring away from the forge) -- and, in the fallback, one under the old
    // floors. Added unranked, at the end, flagged so only the hold may place
    // it.
    for (const RE::FormID id : heldIDs) {
        const bool ranked = std::ranges::any_of(ctx.scoredCandidates,
            [id](const Scoring::ScoredCandidate& sc) { return sc.GetFormID() == id; });
        if (ranked) {
            continue;
        }
        auto addHeld = [&](const std::vector<Candidate::CandidateVariant>& pool) {
            for (const auto& c : pool) {
                if (Candidate::GetFormID(c) == id) {
                    // No score: the flag makes its slot score -inf
                    // (ScoredCandidate::SlotScore), below any ranked row's
                    // whatever its sign.
                    Scoring::ScoredCandidate sc;
                    sc.candidate = c;
                    sc.isRememberedOnly = true;
                    ctx.scoredCandidates.push_back(std::move(sc));
                    return true;
                }
            }
            return false;
        };
        if (!addHeld(candidates)) {
            addHeld(heldAside);
        }
    }

    // R11: what the effect catalog says of each row (core/CapFacets.h), once
    // per run, for the slot classes, the class cap, the label and the display
    // (they used to read the old classifiers' tags off the candidate).
    for (auto& sc : ctx.scoredCandidates) {
        sc.subject = Effect::SubjectOf(sc.candidate);
        sc.hasSubject = true;
    }

    CaptureEligible(ctx, candidates);

    // R8.3: the reason label is derived from the explanations of what the
    // page shows, after the allocation (DeriveDisplayLabels).
}

// -----------------------------------------------------------------------------
// RankOnChoice — R8.3: one row per candidate, ranked on mu
// -----------------------------------------------------------------------------
// The tick (v, W) once from the learner's snapshot and the need vector the
// model reads (the live curve outputs, the bootstrap's curves re-read); per
// candidate cap(i) and the runtime cross-features, covered(i) with doc 11's
// greedy upgrade gate at the summon limit, b with its battery, and the D2
// feature under the switch; then core/ChoiceLive.h's ranking pass. No
// position features and no kappa here (core/ChoiceLive.h): the page is the
// allocation's output. A candidate with no catalog entry (the catalog is
// built at the main menu; a form it does not describe) has no need terms:
// it ranks on b and the legacy feature alone.

Scoring::ScoredCandidateList PipelineCoordinator::RankOnChoice(PipelineContext& ctx,
    const std::vector<Candidate::CandidateVariant>& candidates)
{
    Huginn_ZONE_NAMED("Pipeline::RankOnChoice");
    namespace C = Core::Choice;
    const auto t0 = std::chrono::steady_clock::now();

    auto snap = g_choiceLearner->GetSnapshot();
    const auto& model = snap->model;
    std::array<double, C::K> n{};
    if (const auto live = Needs::NeedMonitor::GetSingleton().Latest()) {
        n = snap->Needs(live->vector.value, live->vector.input);
    }
    const double nowSec = std::chrono::duration<double>(ctx.now.time_since_epoch()).count();
    const uint32_t gen = g_loadGeneration.load(std::memory_order_relaxed);
    const auto recent = g_choiceLearner->RecentValues(*snap, nowSec, gen);
    auto run = std::make_shared<Learning::ShadowRun>();
    run->snap = snap;
    run->tick = C::MakeTick(model, n, recent);
    run->playNow = g_choiceLearner->GetPlaySeconds();
    const auto& tick = run->tick;

    auto& catalog = Effect::EffectCatalog::GetSingleton();
    auto& sm = State::StateManager::GetSingleton();
    const auto coverage = sm.GetCoverage();
    const float refreshPriorSec = sm.GetRefreshPriorSec();
    // What the live score reads (0.24.17: engine B by default), and whether
    // the D2 shadow scores the run beside it (ScoreCandidates decided both).
    const bool legacyOn = ctx.legacyLive;
    const bool shadowA = !legacyOn && ctx.legacy.ran;

    const size_t rows = candidates.size();
    std::vector<C::Attr> attrs(rows);
    std::vector<C::RowExtra> extras(rows);
    std::vector<C::GateRow> gateRows(rows);
    const bool atLimit = coverage.valid &&
                         Core::Coverage::SummonLimitReached(coverage.active.summons, coverage.active.summonLimit);
    const uint32_t outSource = atLimit ? Core::Coverage::OldestSource(coverage.commanded) : 0;
    double leftOut = 1.0;
    size_t outIdx = SIZE_MAX;

    // 0.24.17: the D2 feature engine A reads on each row (the shadow).
    std::vector<C::LegacyValues> legacyShadowFeat(shadowA ? rows : 0);

    Scoring::ScoredCandidateList out;
    out.reserve(rows + 8);
    for (size_t i = 0; i < rows; ++i) {
        const auto& c = candidates[i];
        const RE::FormID form = Candidate::GetFormID(c);
        Scoring::ScoredCandidate sc;
        sc.candidate = c;
        sc.runRow = static_cast<uint32_t>(i);
        // The legacy pass's utility and parts on a row the floors kept.
        const Scoring::Legacy::Row lg = ctx.legacy.RowFor(i);
        if (const auto* L = ctx.legacy.KeptFor(i)) {
            sc.utility = L->utility;
            sc.breakdown = L->breakdown;
            sc.isColdStartBoosted = L->isColdStartBoosted;
            sc.legacyScored = true;
        }

        auto& x = extras[i];
        if (const auto* entry = catalog.FindOrMapId(form)) {
            attrs[i] = AttrOf(model, *entry, ctx.playerState, StackOf(c));
            if (coverage.valid) {
                const auto parts = Core::Coverage::CoveredParts(entry->cap, coverage.active, refreshPriorSec);
                x.covered = std::max(parts.other, parts.summonValue);
                if (parts.summon && parts.atLimit) {
                    leftOut = parts.summonLeft;
                    if (form == outSource) {
                        if (outIdx == SIZE_MAX) outIdx = i;   // a refresh: the count stands
                    }
                    else {
                        gateRows[i] = C::GateRow{ true, parts.other };
                    }
                }
            }
        }
        C::SetLegacy(x.feat, C::LegacyFeature(legacyOn, lg.Scored(), static_cast<double>(lg.lnLearnMult)));
        // Engine A's: the same call with the switch on, as the live score
        // reads it under bLegacyLearnFeature = 1 (only A's mu reads it).
        if (shadowA) legacyShadowFeat[i] = C::LegacyFeature(true, lg.Scored(), static_cast<double>(lg.lnLearnMult));
        // R8.4: no `ln util` feature. D2 reads only ln learn mult and util
        // missing; the old utility does not enter the choice score (the
        // bootstrap reader zeroes an `ln util` weight, core/Bootstrap.cpp).
        x.feat[static_cast<size_t>(C::Feature::eligible)] = 1.0;
        const auto b = snap->B(form, Candidate::IsFavorited(c), run->playNow);
        x.bMu = b.mu;
        x.bVar = b.var;
        x.offPage = false;
        out.push_back(std::move(sc));
    }
    // a_out's source is usually not a row at the limit (the summon_limit
    // filter drops summon spells there, but in the refresh window): its s-hat
    // is then read off its catalog entry -- what the model would score that
    // summon with nothing up (doc 11 decision 2) -- as an extra entry the gate
    // compares against but nothing ranks.
    if (atLimit && outIdx == SIZE_MAX && outSource != 0) {
        if (const auto* entry = catalog.FindOrMapId(outSource)) {
            attrs.push_back(C::MakeAttr(model.grading, &entry->cap, std::array<float, Core::DecisionLog::kCrossCount>{}));
            C::RowExtra x;
            const auto b = snap->B(outSource, false, run->playNow);
            x.bMu = b.mu;
            x.bVar = b.var;
            x.feat[static_cast<size_t>(C::Feature::util_missing)] = 1.0;   // not scored by the old floors
            extras.push_back(x);
            gateRows.push_back(C::GateRow{});
            outIdx = attrs.size() - 1;
        }
    }

    // 0.24.17, engine A in shadow (core/LegacyShadowPass.h ScoreMu): B's
    // extras, read BEFORE the live pass (its upgrade gate writes covered into
    // `extras`, and A's gate must read A's s-hat), copied inside the pure
    // pass with the D2 feature put back, then the same gate and score:
    // exactly the mu the live pass computes with bLegacyLearnFeature on. The
    // gate's comparison entry past the rows keeps util missing 1 in both (the
    // old floors never scored it). Nothing reads A's mu but the pure pass's
    // pages and the log.
    if (shadowA) {
        Huginn_ZONE_NAMED("Pipeline::ShadowLegacyMu");
        Core::LegacyShadow::ScoreMu(model, tick, attrs, extras, gateRows, atLimit ? outIdx : SIZE_MAX, leftOut, rows,
            legacyShadowFeat, ctx.legacyShadow);
    }

    std::vector<C::LiveScore> scores;
    C::ScoreForRanking(model, tick, attrs, extras, gateRows, atLimit ? outIdx : SIZE_MAX, leftOut, rows, scores);

    // R9: the run's beliefs, for the slot hold's challenger rule -- each
    // row's attributes, covered after the upgrade gate and b's variance,
    // beside the tick's column variances (core/ChoiceBeliefs.h). One table,
    // shared by every row, so sigma_Delta between any two rows of the run
    // reads the same tick the scores did. b is keyed by form (snap->B above),
    // so the form is each row's b key: two stacks of one form share one b.
    auto beliefs = std::make_shared<C::BeliefTable>(tick, model.covered.var);
    beliefs->Reserve(rows);
    for (size_t i = 0; i < rows; ++i) {
        beliefs->Add(attrs[i], extras[i].covered, extras[i].bVar, out[i].GetFormID());
    }
    std::shared_ptr<const C::BeliefTable> sharedBeliefs = std::move(beliefs);

    for (size_t i = 0; i < rows; ++i) {
        auto& sc = out[i];
        const auto& ls = scores[i];
        sc.score = ls.s.mu;
        auto& p = sc.choice;
        p.beliefs = sharedBeliefs;
        p.beliefRow = static_cast<uint32_t>(i);
        p.var = ls.s.var;
        p.needMu = ls.needMu;
        p.b = extras[i].bMu;
        p.bVar = extras[i].bVar;
        // 0.24.14: a newly gained item's boost, for the wildcard draw only
        // (SlotSnapshot reads WildcardVar; core/DiscoveryBoost.h says why not
        // the score's variance).
        p.discovery = snap->DiscoveryMult(sc.GetFormID(), run->playNow);
        p.lnLearnMult = extras[i].feat[static_cast<size_t>(C::Feature::ln_learn_mult)];
        p.utilMissing = extras[i].feat[static_cast<size_t>(C::Feature::util_missing)];
        p.covered = extras[i].covered;
        for (size_t t = 0; t < p.top.size(); ++t) {
            p.top[t] = Scoring::ScoredCandidate::ChoiceParts::Term{ ls.top[t].need, ls.top[t].col, ls.top[t].value };
        }
        const int16_t k = ls.top[0].need;
        p.explainedNeed = k >= 0 ? n[static_cast<size_t>(k)] : 0.0;
        sc.explainedReason = Display::ExplainedReason(k, p.explainedNeed);
    }

    attrs.resize(rows);   // the gate's comparison entry is not a row
    ctx.choiceAttrs = std::move(attrs);
    ctx.choiceScores = std::move(scores);
    ctx.choiceRun = run;
    ctx.oldScore.shadow = std::move(run);   // the v3 log scores held rows with the same tick

    [[maybe_unused]] const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    Huginn_PLOT("Choice us per run", static_cast<int64_t>(us));
    Huginn_PLOT("Choice rows per run", static_cast<int64_t>(rows));
    return out;
}

// -----------------------------------------------------------------------------
// CaptureEligible — R4: every generated candidate, for the selection log v3
// -----------------------------------------------------------------------------
// The choice set the fit needs is every item the player could pick, not the
// ones that cleared the old engine's floors (the context and utility floors).
// So the list CandidateGenerator produced is copied out here
// -- one row per item (a weapon stack is its own row) -- with the utility of the
// rows the scorer kept and the propensity of a wildcard. Reads only: the scored
// list, the candidates and the wildcard cache are not touched.
//
// 0.23.53: and the old engine's score parts of every candidate, whatever the
// floors (Core::DecisionLog::OldScore): the R6 component test needs the old
// learner's estimate, confidence, evidence, UCB, recency and prior on the rows
// the floors dropped too, and which floor dropped them, to tell the floors'
// share of the gap from stale imputation. R8.4: the parts come from the legacy
// module (Legacy::OldScoreCapture, learning/legacy/LegacyRun.cpp, moved there
// unchanged): a scored row takes the parts its utility was computed from (its
// breakdown); an unscored one, and every candidate the filters dropped, is
// put through the old scorer's own per-candidate scoring again with no floor,
// and its floor is read off the floor pass's own tests.
// Logging only: nothing here changes what was scored.
//
// R8.3: every candidate is ranked now, so "scored" in this log keeps its old
// meaning -- the OLD floors kept the row (ScoredCandidate::legacyScored) --
// and `util` is that row's legacy utility (NaN on the others), not anything
// the ranking read. The choice score's mu, sd and explanation come from the
// ranking pass (ctx.choiceScores, index-aligned); LogChoiceRows adds the
// page's part after the allocation. With [Scoring] bLegacyLearnFeature off
// lm is 0 and um 1 on every row (what the live score read), whether or not
// the legacy pass ran for the D2 shadow (0.24.17); with both it and
// bShadowLegacy off the legacy pass did not run: no old parts are captured
// (run.valid false, as the v3 reader already handles).

void PipelineCoordinator::CaptureEligible(PipelineContext& ctx,
    const std::vector<Candidate::CandidateVariant>& candidates)
{
    Huginn_ZONE_NAMED("Pipeline::CaptureEligible");
    // First legacy-scored row per dedup key (type, uniqueID, formID) wins, as
    // the cache's FormID index does. R10: the wildcard propensity is set after
    // the allocation, which draws the wildcard (AllocateAndLock).
    std::unordered_map<uint64_t, const Scoring::ScoredCandidate*> scoredByKey;
    scoredByKey.reserve(ctx.scoredCandidates.size());
    for (const auto& sc : ctx.scoredCandidates) {
        if (sc.isRememberedOnly) continue;   // added for the hold only: not scored
        const auto key = Candidate::GetBase(sc.candidate).GetDeduplicationKey();
        if (sc.legacyScored) scoredByKey.try_emplace(key, &sc);
    }

    // 0.23.53: the old engine's view of the run (see above). R8.3: only when
    // the legacy pass ran.
    namespace DL = Core::DecisionLog;
    auto& old = ctx.oldScore;
    old.dropped.clear();
    old.stateHash = ctx.currentState.GetHash();
    old.run = {};
    const Scoring::Legacy::OldScoreCapture capture(ctx.legacy, ctx.currentState, ctx.playerState,
        ctx.targets, ctx.worldState, ctx.combatOnsetAgo);
    const bool withOld = capture.Valid();
    if (withOld) {
        old.run = capture.Head(static_cast<int>(std::ranges::count_if(ctx.scoredCandidates,
            [](const Scoring::ScoredCandidate& sc) { return sc.legacyScored && !sc.isRememberedOnly; })));
    }

    ctx.eligible.reserve(candidates.size());
    // R8.2: the D2 feature off the same pass (index-aligned with candidates),
    // and a check that the moved floor loop reports the floors this capture
    // derives on its own (they are the same tests; a mismatch is a bug).
    const bool haveLegacy = ctx.legacy.ran && ctx.legacy.rows.size() == candidates.size() &&
                            ctx.legacy.rowOf.size() == candidates.size();
    // Two candidates with one dedup key (type, uid, form) read the same scored
    // row here (scoredByKey), but the floor pass's cold start scores only the
    // first (alreadyScored by form): the second reads the legacy row of the
    // candidate that was kept (Legacy::Run::RowFor, R8.3: worked out once by
    // the pass, so the ranking reads the same row), and its floor, lm and um
    // agree with its utility.
    const bool haveChoice = ctx.choiceLive && ctx.choiceScores.size() == candidates.size();
    size_t floorMismatch = 0;
    size_t candIndex = 0;
    for (const auto& c : candidates) {
        const size_t ci = candIndex++;
        const auto& base = Candidate::GetBase(c);
        Learning::PipelineStateCache::EligibleRow row;
        row.formID = base.formID;
        row.uniqueID = base.uniqueID;
        row.sourceType = base.sourceType;
        std::visit([&row](const auto& cand) {
            using T = std::decay_t<decltype(cand)>;
            if constexpr (std::is_same_v<T, Candidate::ItemCandidate> || std::is_same_v<T, Candidate::AmmoCandidate> ||
                          std::is_same_v<T, Candidate::ScrollCandidate> || std::is_same_v<T, Candidate::TorchCandidate>) {
                row.count = cand.count;
            }
            else if constexpr (std::is_same_v<T, Candidate::WeaponCandidate>) {
                row.enchanted = cand.hasEnchantment;
                row.chargeFraction = cand.currentCharge;   // already current / max (WeaponRegistry)
            }
        }, c);
        const Scoring::ScoredCandidate* scored = nullptr;
        if (const auto it = scoredByKey.find(base.GetDeduplicationKey()); it != scoredByKey.end()) {
            scored = it->second;
            row.utility = it->second->utility;
        }
        if (withOld) {
            row.old = capture.Explain(c, true, scored);
        }
        else {
            row.old.favorite = Candidate::IsFavorited(c);   // the v3 Favorite flag reads it
        }
        // lm / um: what the live score read (0.24.17: with engine B live and
        // the legacy pass run for the D2 shadow, that is 0 / 1, whatever the
        // floors kept; ChoiceLearner trains on these).
        row.lnLearnMult = 0.0f;
        row.utilMissing = 1;
        if (haveLegacy) {
            const auto lg = ctx.legacy.RowFor(ci);
            if (ctx.legacyLive) {
                row.lnLearnMult = lg.Scored() ? lg.lnLearnMult : 0.0f;
                row.utilMissing = lg.utilMissing ? 1 : 0;
            }
            if (static_cast<uint8_t>(lg.floor) != row.old.floor) ++floorMismatch;
        }
        if (haveChoice) {
            // the ranking's mu; LogChoiceRows adds the page's part
            const auto& ls = ctx.choiceScores[ci];
            row.shadowMu = static_cast<float>(ls.s.mu);
            row.shadowSd = static_cast<float>(ls.s.Sd());
            row.shadowLogit = static_cast<float>(ls.s.mu);
            row.exNeed = ls.s.Learned() ? DL::kExLearned : ls.s.exNeed;
            row.exCol = ls.s.Learned() ? static_cast<int16_t>(-1) : ls.s.exCol;
        }
        ctx.eligible.push_back(row);
    }
    if (floorMismatch) {
        // NOTE: Single-threaded (pipeline runs on the update thread only)
        static auto s_lastWarn = std::chrono::steady_clock::time_point{};
        if (ctx.now - s_lastWarn > std::chrono::seconds(30)) {
            logger::warn("[Legacy] {} of {} candidates: the floor pass and the capture disagree on the floor (a bug)"sv,
                floorMismatch, candidates.size());
            s_lastWarn = ctx.now;
        }
    }

    // 0.23.53: the candidates the filters dropped (one row per item; a
    // duplicate is the eligible row's own item and is skipped).
    if (withOld && !ctx.filtered.empty()) {
        Huginn_ZONE_NAMED("Pipeline::CaptureEligible::Dropped");
        const auto stackKey = [](RE::FormID form, uint16_t uid) { return (static_cast<uint64_t>(uid) << 32) | form; };
        std::unordered_set<uint64_t> wanted;
        wanted.reserve(ctx.filtered.size());
        for (const auto& d : ctx.filtered) {
            if (d.reason != Core::Filter::Reason::duplicate) wanted.insert(stackKey(d.form, d.uid));
        }
        for (const auto& c : Candidate::CandidateGenerator::GetSingleton().LastGathered()) {
            const auto& base = Candidate::GetBase(c);
            if (wanted.erase(stackKey(base.formID, base.uniqueID)) == 0) continue;   // not dropped, or done
            Learning::PipelineStateCache::EligibleRow row;
            row.formID = base.formID;
            row.uniqueID = base.uniqueID;
            row.sourceType = base.sourceType;
            row.old = capture.Explain(c, false, nullptr);
            old.dropped.push_back(row);
        }
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
    //
    // A page that feeds a Wheeler wheel is allocated without the weapons and
    // armour that have no uniqueID: the wheel cannot show them, and the key
    // takes the next item instead of a blank tile (display/WheelUidFilter.h).
    // Only the allocation's input: the scored list itself stays whole for the
    // lock's refresh and telemetry, `hg recs` and the selection log.
    auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
    const auto& pageCandidates = Display::CandidatesForWheelPage(ctx.displayPageIndex, ctx.hideUidlessGear,
        ctx.scoredCandidates, ctx.pageCandidatesScratch);
    // R10, the wildcard key: the manager says what this page's key should do
    // (pin the cached wildcard, or roll), the allocation places it or draws
    // one from what the finished page does not show, and the manager keeps
    // the result. In combat (the lock's notion of it) none is placed.
    auto& wildcards = g_utilityScorer->GetWildcardManager();
    const Scoring::WildcardPage wildcardPage{
        .index = ctx.displayPageIndex,
        .slotCount = ctx.displaySlotCount,
        .seat = ctx.displayWildcardSeat,
        .inCombat = Slot::SlotLocker::GetSingleton().IsInCombat(),
        .hideUidlessGear = ctx.hideUidlessGear,
        .display = true,
    };
    // Plan steps the gate's clocks before the page-race check below (expiry,
    // the combat drop and refractory restart, a reshape, the uid drop); only
    // Commit waits for it. So a tick abandoned after asking for a roll leaves
    // the window open, and the next run rolls with fresh uniforms: with page
    // races (about none in play) the logged per-window propensity runs a
    // little low against the true chance of a wildcard in that window.
    // (0.24.6: likewise a draw the slot locks hid, after ApplyLocks below.)
    auto wildcardRequest = wildcards.Plan(wildcardPage, ctx.scoredCandidates);
    // A roll on a key the lock still holds would draw a wildcard the player
    // does not see (ApplyLocks keeps the locked item), yet cache, log and
    // count it: the roll waits for a run with the seat unlocked, in the same
    // window (not spent: Commit sees no rollAsked). And what a lock holds on
    // any key is kept out of the draw: the post-lock dedup would clear the
    // wildcard as a second copy, and the rows just under the cut -- the
    // likeliest draws -- are the ones that just left a key and are still
    // held there. 0.24.6: and what a lock holds by NAME -- the post-lock
    // dedup compares names (core/DedupRules.h), so a row named like a locked
    // key's item of another form was cleared just the same. The held keys
    // (and their forms and names) are passed on a pinned run
    // too: the seat's displaced row never moves onto a locked key, where
    // ApplyLocks would show the locked item and the moved row nowhere. A
    // lock with no time left lets go in ApplyLocks this run.
    if (wildcardRequest.roll || wildcardRequest.pinForm != 0) {
        const auto locks = Slot::SlotLocker::GetSingleton().GetLockSnapshot();
        const auto holding = [](const Slot::SlotLocker::SlotLockView& v) {
            return v.isLocked && (v.frozen || v.remainingMs > 0.0f);
        };
        const size_t seat = ctx.displayWildcardSeat;
        if (wildcardRequest.roll && seat < locks.size() && holding(locks[seat])) {
            wildcardRequest.roll = false;
        } else {
            for (size_t s = 0; s < locks.size() && s < ctx.displaySlotCount && s < 32; ++s) {
                if (!holding(locks[s])) continue;
                wildcardRequest.heldKeys |= 1u << s;
                if (locks[s].previousFormID != 0) wildcardRequest.heldForms.push_back(locks[s].previousFormID);
                if (!locks[s].name.empty()) wildcardRequest.heldNames.push_back(locks[s].name);
            }
        }
    }
    Slot::WildcardResult wildcardResult;
    // 0.24.17: on a run the D2 shadow runs on, the allocator hands back its
    // input (it reads nothing of it), and each page row's run row, for the
    // shadow's stateless pages (ShadowLegacyPages).
    Core::SlotAlloc::Input* const legacyShadowInput = ctx.legacyShadow.ran ? &ctx.legacyShadowAllocInput : nullptr;
    if (legacyShadowInput) {
        *legacyShadowInput = {};   // an allocation of a page with no keys leaves it empty
        ctx.pageRunRows.clear();
        for (const auto& sc : pageCandidates) ctx.pageRunRows.push_back(sc.isRememberedOnly ? UINT32_MAX : sc.runRow);
    }
    ctx.rawAssignments = slotAllocator.AllocateSlotsForPage(
        ctx.displayPageIndex, pageCandidates, ctx.overrides, ctx.playerState, ctx.worldState,
        &wildcardRequest, &wildcardResult, legacyShadowInput);
    ctx.haveAllocInput = legacyShadowInput != nullptr && !ctx.legacyShadowAllocInput.slots.empty() &&
                         ctx.legacyShadowAllocInput.candidates.size() == ctx.pageRunRows.size();
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
    // Which keys show a Remembrance hold for the key the player pressed: under
    // a combat lock only those may take a locked key.
    Slot::SlotLocker::RunContext lockRun;
    {
        const auto pressedHolds = Slot::Remembrance::GetSingleton().GetPage(ctx.displayPageIndex);
        for (size_t s = 0; s < pressedHolds.size() && s < lockRun.remembranceOnPressedKey.size(); ++s) {
            const auto& hold = pressedHolds[s];
            lockRun.remembranceOnPressedKey[s] = hold.Active() && (hold.shownSlot == SIZE_MAX || hold.shownSlot == s);
            // The combat upsert leaves alone a key with a hold of its own and
            // the key showing it.
            if (hold.Active()) {
                lockRun.remembranceHoldOnKey[s] = true;
                if (hold.shownSlot < lockRun.remembranceHoldOnKey.size()) {
                    lockRun.remembranceHoldOnKey[hold.shownSlot] = true;
                }
            }
        }
    }
    // Combat upsert inputs (core/CombatUpsert.h): none while the wheel is
    // open, none on a key inside press grace, a barrier on the hold's margin.
    lockRun.wheelOpen = Wheeler::WheelerClient::GetSingleton().IsWheelOpen();
    lockRun.pressGraceMs = Slot::SlotSettings::GetSingleton().PressGraceMs();
    lockRun.challengerMargin = Slot::SlotSettings::GetSingleton().ChallengerMargin();
    lockRun.challengerZ = Core::Challenger::ZForConfidence(Slot::SlotSettings::GetSingleton().ChallengerConfidence());
    lockRun.challengerMinGap = Core::Challenger::ClampMinGap(Slot::SlotSettings::GetSingleton().ChallengerMinGap());
    lockRun.testAllChallengers = Slot::SlotSettings::GetSingleton().TestAllChallengers();
    // A combat-locked key's item, checked against the game every run: a spell
    // or shout the player no longer knows, or an item now equipped / worn.
    // The worn stack's uniqueID in one hand (the list carrying ExtraWorn, else
    // the first, as WeaponRegistry::RefreshCharges reads it); -1 if none.
    const auto wornUid = [](RE::PlayerCharacter* p, bool leftHand) -> int {
        RE::InventoryEntryData* entry = p->GetEquippedEntryData(leftHand);
        if (!entry || !entry->extraLists) return -1;
        RE::ExtraDataList* worn = nullptr;
        for (auto* xl : *entry->extraLists) {
            if (!xl) continue;
            if (!worn) worn = xl;
            if (xl->HasType<RE::ExtraWorn>() || xl->HasType<RE::ExtraWornLeft>()) { worn = xl; break; }
        }
        const auto* xid = worn ? worn->GetByType<RE::ExtraUniqueID>() : nullptr;
        return xid ? static_cast<int>(xid->uniqueID) : -1;
    };
    lockRun.probeItem = [player = ctx.player, wornUid](RE::FormID formID, uint16_t uniqueID) {
        Slot::SlotLocker::RunContext::ItemState st;
        auto* form = RE::TESForm::LookupByID(formID);
        if (!form || !player) {
            st.gone = form == nullptr;
            return st;
        }
        if (form->Is(RE::FormType::Spell)) {
            auto* spell = form->As<RE::SpellItem>();
            st.gone = spell && !player->HasSpell(spell);
            const auto& sel = player->GetActorRuntimeData().selectedSpells;
            st.equipped = sel[RE::Actor::SlotTypes::kLeftHand] == spell ||
                          sel[RE::Actor::SlotTypes::kRightHand] == spell;
        } else if (form->Is(RE::FormType::Shout)) {
            st.gone = !player->HasShout(form->As<RE::TESShout>());
        } else if (form->Is(RE::FormType::SoulGem)) {
            // A reusable gem (Azura's Star) emptied keeps its count -- no
            // inventory event says it left -- and the gather skips empty gems.
            // Not gathered this run = nothing on the key a press can use.
            st.gone = !Candidate::CandidateGenerator::GetSingleton().FindGathered(formID, uniqueID).has_value();
        } else if (form->Is(RE::FormType::Ammo)) {
            const auto* ammo = player->GetCurrentAmmo();
            st.equipped = ammo && ammo->GetFormID() == formID;
        } else if (form->Is(RE::FormType::Armor)) {
            // A named stack: worn only if THAT stack carries ExtraWorn (a walk
            // of the one inventory entry; armour keys are rare in combat).
            if (uniqueID == 0) {
                st.equipped = player->GetWornArmor(formID) != nullptr;
            } else {
                auto* bound = form->As<RE::TESBoundObject>();
                const auto inv = Util::GetInventorySafe(player,
                    [bound](RE::TESBoundObject& obj) { return std::addressof(obj) == bound; });
                for (const auto& [obj, data] : inv) {
                    const auto& entry = data.second;
                    if (!entry || !entry->extraLists) continue;
                    for (auto* xl : *entry->extraLists) {
                        if (!xl) continue;
                        const auto* xid = xl->GetByType<RE::ExtraUniqueID>();
                        if (xid && xid->uniqueID == uniqueID &&
                            (xl->HasType<RE::ExtraWorn>() || xl->HasType<RE::ExtraWornLeft>())) {
                            st.equipped = true;
                        }
                    }
                }
            }
        } else if (form->Is(RE::FormType::Weapon) || form->Is(RE::FormType::Light) || form->Is(RE::FormType::Scroll)) {
            for (const bool leftHand : { true, false }) {
                const auto* obj = player->GetEquippedObject(leftHand);
                const bool formMatches = obj && obj->GetFormID() == formID;
                if (Core::EquippedInstanceMatches(formMatches, uniqueID,
                        formMatches && uniqueID != 0 ? wornUid(player, leftHand) : -1)) {
                    st.equipped = true;
                }
            }
        }
        return st;
    };
    // A held key's current count/charge, from the generator's pre-filter
    // gather of this run (the potion just drunk is on cooldown and, at full
    // health, filtered -- it is still gathered).
    lockRun.lookupItem = [](RE::FormID formID, uint16_t uniqueID) {
        return Candidate::CandidateGenerator::GetSingleton().FindGathered(formID, uniqueID);
    };
    // A lock taken on uid-less gear before the page hid it -- in the second
    // after a load, before the registry read the extra data, or before
    // Wheeler connected -- lets go, or it holds a blank wheel tile (all fight,
    // under the combat freeze). So does a weapon locked in that second whose
    // record has since gained its real uniqueID: the lock still names uid 0.
    if (ctx.hideUidlessGear) {
        lockRun.unshowable = [](const Slot::SlotAssignment& held) {
            return held.candidate && Display::IsUidlessGear(held.candidate->candidate);
        };
    }
    ctx.assignments = slotLocker.ApplyLocks(ctx.rawAssignments, ctx.overrides, ctx.scoredCandidates, lockRun);

    // R10: the wildcard result is kept only for an allocation that is used --
    // after the page-race check above, so an abandoned tick neither caches a
    // drawn wildcard (nor logs or counts it) nor spends the window's roll.
    // 0.24.6: and after ApplyLocks, so a draw is cached, logged ('[Wildcard]
    // Page P key K') and counted (the heartbeat's rolled=) only when the page
    // SHOWS it on the seat. Committed before ApplyLocks (R10's verifier), a
    // draw the locker hid -- the seat's lock keeping its old item, or the
    // post-lock dedup clearing the wildcard for a lock's copy of its name --
    // was cached for its cooldown and counted although the player saw no
    // wildcard. The draw leaves out what a lock holds (above), so this is the
    // backstop for a lock kept or taken inside ApplyLocks itself: such a draw
    // counts for nothing and the roll waits (Core::Wildcard::CommitFate::
    // NotShown). ApplyLocks reads nothing of the manager's, so the order is
    // free to change.
    bool drawnShown = true;
    if (wildcardResult.drawn) {
        const size_t seat = wildcardResult.core.seat;
        const Slot::SlotAssignment* shown = seat < ctx.assignments.size() ? &ctx.assignments[seat] : nullptr;
        drawnShown = Core::Wildcard::DrawnShown(shown != nullptr && shown->IsWildcard(),
            shown ? shown->formID : 0, shown ? shown->uniqueID : uint16_t{ 0 },
            wildcardResult.drawn->GetFormID(), wildcardResult.drawn->GetUniqueID());
    }
    wildcards.Commit(wildcardPage, wildcardResult, drawnShown);
    // The v3 log's propensity (wp) on the eligible row the page's wildcard is
    // (its form and stack), NaN on every other: read after the commit, so a
    // wildcard drawn on this run carries its own.
    {
        const RE::FormID wildForm = wildcards.GetWildcard(ctx.displayPageIndex);
        const float wildP = wildcards.GetWildcardPropensity(ctx.displayPageIndex, wildForm);
        const uint16_t wildUid = wildcards.GetWildcardUniqueID(ctx.displayPageIndex);
        // Its own stack; failing that (the allocation pinned another stack of
        // the form, SlotAllocator), every stack of the form.
        const bool stackListed = std::ranges::any_of(ctx.eligible,
            [&](const auto& row) { return row.formID == wildForm && row.uniqueID == wildUid; });
        for (auto& row : ctx.eligible) {
            const bool it = wildForm != 0 && row.formID == wildForm && (!stackListed || row.uniqueID == wildUid);
            row.wildcardPropensity = it ? wildP : std::numeric_limits<float>::quiet_NaN();
        }
    }

    // Compute visual state for each slot
    Slot::ComputeVisualStates(ctx.assignments, ctx.rawAssignments, slotLocker);

    // A Remembrance hold near its end pulses like an expiring lock, so the
    // player sees the swap-back is about to go. Found by where it is shown.
    const auto holds = Slot::Remembrance::GetSingleton().GetPage(ctx.displayPageIndex);
    for (auto& assignment : ctx.assignments) {
        if (!assignment.IsRemembered()) continue;
        for (size_t s = 0; s < holds.size(); ++s) {
            const auto& hold = holds[s];
            const size_t shown = hold.shownSlot != SIZE_MAX ? hold.shownSlot : s;
            if (hold.Active() && shown == assignment.slotIndex && hold.expiring) {
                assignment.visualState = Slot::SlotVisualState::Expiring;
            }
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// ShadowLegacyPages -- 0.24.17: the D2 engine in shadow, after the allocation
// -----------------------------------------------------------------------------
// RankOnChoice left A's mu per candidate in the shadow run. Here, off the
// same run and touching nothing the player sees (the work is the pure pass,
// core/LegacyShadowPass.h RanksAndPages, on const views of the live run):
//   - each candidate's rank under A and under B (core/ShadowCompare.h RanksOf:
//     the live sort's order, ties broken as B broke them);
//   - its key on A's page and on B's page, both built WITHOUT memory from the
//     live allocation's own input (core/SlotAllocStateless.h): the same
//     layout, candidates (the wheel page's uid filter applied) and overrides,
//     no slot hold, seating, Remembrance hold, wildcard or lock. B's
//     stateless page is the control: A's page differs from it only by the
//     ranking, while the live page also carries the memory;
// then copies them onto the eligible rows (the v3 log's amu, ar, br, ak, bk).
// The live slot state is not touched: the stateless pages run the pure core
// on a copy of the input (no seating memory, no events reported, no capture).

void PipelineCoordinator::ShadowLegacyPages(PipelineContext& ctx)
{
    if (!ctx.legacyShadow.ran) return;
    Huginn_ZONE_NAMED("Pipeline::ShadowLegacyPages");
    const auto t0 = std::chrono::steady_clock::now();
    const size_t rows = ctx.choiceScores.size();

    // B's order, scores and tie-breaks off the ranked list (remembered-only
    // rows were added after the sort and are not ranked): the live run's
    // values, read here and handed to the pure pass as const.
    std::vector<uint32_t> liveOrder;
    liveOrder.reserve(rows);
    std::vector<double> liveMu(rows, Core::kUnrankedScore);
    std::vector<float> tie(rows, 0.0f);
    for (const auto& sc : ctx.scoredCandidates) {
        if (sc.isRememberedOnly || sc.runRow >= rows) continue;
        liveOrder.push_back(sc.runRow);
        liveMu[sc.runRow] = sc.score;
        tie[sc.runRow] = sc.TieBreakDps();
    }
    // The allocation's handed-over input is spent by the pass (it builds both
    // stateless pages in place on it).
    Core::SlotAlloc::Input* const legacyShadowPageInput = ctx.haveAllocInput ? &ctx.legacyShadowAllocInput : nullptr;
    ctx.haveAllocInput = false;
    Core::LegacyShadow::RanksAndPages(liveOrder, liveMu, tie, legacyShadowPageInput, ctx.pageRunRows, ctx.legacyShadow);
    // The pass left A's order and A's mu in the input: drop it now, so
    // nothing after this point (nor the next run) can read it.
    ctx.legacyShadowAllocInput = {};

    // Onto the eligible rows (index-aligned with the candidates), for the v3 log.
    if (ctx.eligible.size() == ctx.legacyShadow.mu.size() && ctx.legacyShadow.keyA.size() == ctx.eligible.size()) {
        for (size_t r = 0; r < ctx.eligible.size(); ++r) {
            auto& e = ctx.eligible[r];
            e.legacyShadowMu = static_cast<float>(ctx.legacyShadow.mu[r]);
            e.legacyShadowRankA = ctx.legacyShadow.rankA[r];
            e.legacyShadowRankB = ctx.legacyShadow.rankB[r];
            e.legacyShadowKeyA = ctx.legacyShadow.keyA[r];
            e.legacyShadowKeyB0 = ctx.legacyShadow.keyB0[r];
        }
    }

    [[maybe_unused]] const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    Huginn_PLOT("Shadow pages us per run", static_cast<int64_t>(us));
}

// -----------------------------------------------------------------------------
// LogChoiceRows — R8.3: the page's part of each logged score (was ScoreShadow)
// -----------------------------------------------------------------------------
// The ranking ran in ScoreCandidates (RankOnChoice); CaptureEligible copied
// its mu, sd and explanation onto the eligible rows. After the allocation the
// page adds, for the v3 log, what a pick's choice set reads (the learner's
// view, fitlib.build's): the slot feature of the key a row landed on, and
// kappa off the page (core/ChoiceLive.h AfterAllocation). With the R6
// bootstrap (no slot weights) the logged mu is the ranked mu. Also the
// opportunity counter: a need's onset with an answering item on this page
// grows its variances.

void PipelineCoordinator::LogChoiceRows(PipelineContext& ctx)
{
    if (!ctx.choiceLive || !ctx.choiceRun || !g_choiceLearner) return;
    Huginn_ZONE_NAMED("Pipeline::LogChoiceRows");
    namespace C = Core::Choice;
    const auto& model = ctx.choiceRun->snap->model;

    // The page just allocated: a row is on it when an assignment shows its
    // form with its unique ID, or either side has none.
    auto onPage = [&ctx](RE::FormID form, uint16_t uid) -> int {
        for (const auto& a : ctx.assignments) {
            if (a.type == Slot::AssignmentType::Empty || a.formID != form) continue;
            if (a.uniqueID == uid || a.uniqueID == 0 || uid == 0) return static_cast<int>(a.slotIndex);
        }
        return -1;
    };

    const bool aligned = ctx.choiceScores.size() == ctx.eligible.size() &&
                         ctx.choiceAttrs.size() == ctx.eligible.size();
    std::vector<C::Attr> page;
    for (size_t i = 0; i < ctx.eligible.size() && aligned; ++i) {
        auto& e = ctx.eligible[i];
        const int slot = onPage(e.formID, e.uniqueID);
        const bool offPage = slot < 0;
        const auto logged = C::AfterAllocation(model, ctx.choiceScores[i].s.mu, offPage, slot);
        e.shadowMu = static_cast<float>(logged.mu);
        e.shadowLogit = static_cast<float>(logged.logit);
        if (!offPage) page.push_back(ctx.choiceAttrs[i]);
    }

    // The opportunity counter (onsets, not seconds), beside the cache update.
    g_choiceLearner->OnPipelineRun(ctx.choiceRun->tick.n, page);
}

// -----------------------------------------------------------------------------
// UpdateCaches — Pipeline state cache + EquipManager slot contents
// -----------------------------------------------------------------------------

void PipelineCoordinator::UpdateCaches(PipelineContext& ctx)
{
    Huginn_ZONE_NAMED("Pipeline::UpdateCaches");
    // R8.3: the page's part of the logged choice scores, on the run's
    // eligible rows before they move into the cache (and the opportunity
    // counter beside the cache).
    LogChoiceRows(ctx);
    // Cache pipeline state for external equip attribution.
    // sortedPrefix = kReportedRankPrefix: ranks are reported for that prefix
    // only. The whole list is in score order since R7 (UtilityScorer sorts it
    // all), but attribution classifies past the prefix as a far miss, and that
    // stays as it was; see PipelineStateCache::Update. R8.4: a constant of its
    // own (10, the old iTopNCandidates default the logs were written with),
    // no longer the legacy floors' iTopNCandidates. Page comes from the tick
    // snapshot so it matches ctx.assignments exactly.
    Learning::PipelineStateCache::GetSingleton().Update(
        ctx.scoredCandidates, ctx.assignments,
        ctx.displayPageIndex,
        Learning::PipelineStateCache::kReportedRankPrefix,
        std::move(ctx.eligible), ctx.displaySlotCount,    // R4: the selection log v3's rows
        std::move(ctx.filtered),                          // 0.23.46: and why the others were dropped
        std::move(ctx.oldScore));                         // 0.23.53: the old engine's view of the run

    // Cache slot contents for EquipManager (keyboard equip hotkeys)
    auto& equipMgr = Input::EquipManager::GetSingleton();
    for (const auto& assignment : ctx.assignments) {
        equipMgr.SetSlotContent(ctx.displayPageIndex, assignment.slotIndex, Slot::ToSlotContent(assignment));
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
    ctx.displayWildcardSeat = slotAllocator.GetWildcardSeat(ctx.displayPageIndex);
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
    // R11: the run's outside option (outcomes "three"), for the [Recs] header.
    const double outsideU0 = ctx.choiceRun && ctx.choiceRun->snap && ctx.choiceRun->snap->model.three
                                 ? ctx.choiceRun->tick.u0
                                 : std::numeric_limits<double>::quiet_NaN();
    if (size_t dumpN = m_recDumpRequest.exchange(0, std::memory_order_relaxed); dumpN > 0) {
        g_utilityScorer->LogTopCandidates(ctx.scoredCandidates, dumpN,
            /*detail=*/true, /*force=*/true, /*fallback=*/!ctx.choiceLive, outsideU0);

        // Label with the tick's snapshot page, not a live re-read, so the dump
        // matches the assignments it prints.
        logger::info("[Recs] Slots (page {} '{}'):"sv,
            ctx.displayPageIndex, ctx.displayPageName);
        for (const auto& a : ctx.assignments) {
            if (a.IsEmpty() || a.formID == 0) continue;
            // An override carries only the +inf "pinned" score. R8.4: the
            // score alone (mu, or s = ln u in the fallback), no old utility.
            logger::info("[Recs]   slot {}: {} ({:08X}) {}{}"sv,
                a.slotIndex, a.name, a.formID,
                a.IsOverride() ? std::string("override")
                               : fmt::format("{}={:+.3f}", ctx.choiceLive ? "mu" : "s", a.score),
                a.subtextLabel.empty() ? "" : fmt::format(" [{}]", a.subtextLabel));
        }
        return;  // The dump covers this tick; skip the periodic log
    }

    const int verbosity = UI::DebugSettings::GetSingleton().recLogVerbosity;
    if (verbosity <= 0) return;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(ctx.now - m_lastRecLog);
    if (elapsed.count() >= 5000) {
        g_utilityScorer->LogTopCandidates(ctx.scoredCandidates, 5,
            /*detail=*/verbosity >= 2, /*force=*/false, /*fallback=*/!ctx.choiceLive, outsideU0);
        m_lastRecLog = ctx.now;
    }
}

// -----------------------------------------------------------------------------
// UpdateContextReason — R8.3: the tick's reason, from the page's explanations
// -----------------------------------------------------------------------------
// In place of the old weights' dominant reason (R8.4: deleted with it):
// each ranked row carries its explanation, the largest theta[k][j] n_k a_j
// term of its score, mapped to a context reason when its need k is one
// (display/NeedReason.h) and on (n_k >= 0.5). The tick's raw reason is the
// most urgent of those among the items the allocated page shows (the
// ContextReason order is the display priority, as before), so the label can
// only name a reason the page answers. Then the hold (#62) damps it, and the
// [Context] line logs the transition. The fallback ranking (no bootstrap) has
// no explanations: no situational reason.

void PipelineCoordinator::UpdateContextReason(PipelineContext& ctx)
{
    Context::ContextReason rawReason = Context::ContextReason::None;
    for (const auto& a : ctx.assignments) {
        if (a.IsEmpty() || a.IsOverride() || a.IsRemembered() || !a.candidate) continue;
        const auto r = a.candidate->explainedReason;
        if (r == Context::ContextReason::None) continue;
        if (rawReason == Context::ContextReason::None || r < rawReason) rawReason = r;
    }

    // Damp the LABEL only (#62): a reason true for one tick would repaint all
    // eight subtexts and revert before it could be read. The hold releases a
    // reason slowly and adopts a more urgent one instantly (ReasonHold).
    // ctx.now, not a fresh clock read: the hold is argued against SlotLocker's
    // duration, so both must be measured off the same clock reading.
    const double nowMs = std::chrono::duration<double, std::milli>(ctx.now.time_since_epoch()).count();

    // REASON_HOLD_MS sits below the slot content lock -- the label must not
    // outlive the contents it explains -- but that lock is fLockDurationMs, an
    // INI value the player can lower or set to 0. Clamp instead of asserting
    // the default: with locking off the honest hold is zero.
    const float lockMs = Slot::SlotLocker::GetSingleton().GetConfig().lockDurationMs;
    const float holdMs = std::min(Config::REASON_HOLD_MS, std::max(lockMs, 0.0f));
    ctx.contextReason = m_reasonHold.Update(rawReason, nowMs, holdMs);

    // The reason drives a display string only, so log the transition (not the
    // tick), deduped on BOTH halves: the raw one is the only record of what
    // the page's explanations said while a hold masks it. Debug level: a
    // behaviour diagnostic for Debug soak runs.
    // NOTE: Single-threaded (pipeline runs on the update thread only).
    const LoggedReason nowLogged{ .raw = rawReason, .shown = ctx.contextReason };
    if (!m_lastLoggedReason || *m_lastLoggedReason != nowLogged) {
        const auto labelOr = [](Context::ContextReason r) {
            const auto label = Display::ReasonLabel(r);
            return label.empty() ? "(none)"sv : label;
        };
        const auto& vitals = ctx.playerState.vitals;
        const auto rawNote = rawReason == ctx.contextReason
            ? std::string{}
            : fmt::format(" (raw {})", labelOr(rawReason));
        logger::debug("[Context] Reason: {} → {}{} | hp={:.1f}% mp={:.1f}% sp={:.1f}% charge={}"sv,
            m_lastLoggedReason ? labelOr(m_lastLoggedReason->shown) : "(none)"sv,
            labelOr(ctx.contextReason),
            rawNote,
            vitals.health * 100.0f, vitals.magicka * 100.0f, vitals.stamina * 100.0f,
            ctx.playerState.hasEnchantedWeapon
                ? fmt::format("{:.1f}%", ctx.playerState.weaponChargePercent * 100.0f)
                : "n/a");
        m_lastLoggedReason = nowLogged;
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
    UpdateContextReason(ctx);

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
        // Not during Wheeler edit mode: the player is moving entries and
        // Huginn has no signal that indices moved until it exits, so every
        // desync reported there is stale by construction (~11 warns in one
        // session, 2026-08-29) and the exit re-resolve fixes them anyway.
        if (!wheelerClient.IsInEditMode()) {
            wheelerClient.ValidateWheelState();
        }
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
    }
#endif
}
#endif

}  // namespace Huginn::Pipeline
