#include "LegacyRun.h"

#include "Globals.h"                       // the old learner and UsageMemory (D2's inputs)
#include "context/ContextWeightForCandidate.h"
#include "learning/FeatureBanditLearner.h"
#include "learning/StateFeatures.h"
#include "learning/UsageMemory.h"
#include "LegacyScorer.h"
#include "state/GameState.h"
#include "state/PlayerActorState.h"
#include "state/TargetActorState.h"
#include "state/WorldState.h"

#include <array>
#include <numeric>
#include <optional>

namespace Huginn::Scoring::Legacy
{
    // -------------------------------------------------------------------------
    // Run
    // -------------------------------------------------------------------------

    void Run::Clear()
    {
        ran = false;
        kept.clear();
        rows.clear();
        rowOf.clear();
        weights = {};
    }

    void Run::Skip(std::size_t candidates)
    {
        Clear();
        rows.assign(candidates, Row{});
        rowOf.resize(candidates);
        std::iota(rowOf.begin(), rowOf.end(), std::size_t{ 0 });
    }

    void Run::ResolveRows(const std::vector<Candidate::CandidateVariant>& candidates)
    {
        // R8.3's rule (moved from PipelineCoordinator::ScoreCandidates
        // unchanged; pure since R8.4: ResolveRowOf, LegacyLearnFeature.h).
        std::vector<uint64_t> keys(candidates.size());
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            keys[i] = Candidate::GetBase(candidates[i]).GetDeduplicationKey();
        }
        ResolveRowOf(keys, rows, rowOf);
    }

    Row Run::RowFor(std::size_t i) const noexcept
    {
        return RowAt(rowOf, rows, i);
    }

    const ScoredCandidate* Run::KeptFor(std::size_t i) const noexcept
    {
        const int k = KeptIndexAt(rowOf, rows, i, kept.size());
        return k < 0 ? nullptr : &kept[static_cast<std::size_t>(k)];
    }

    ScoredCandidateList Run::FallbackRanking() const
    {
        // Moved from PipelineCoordinator::ScoreCandidates (R8.3) unchanged: a
        // copy, because the rows index the kept list.
        ScoredCandidateList out = kept;
        for (auto& sc : out) {
            sc.legacyScored = true;
            sc.fallbackRanked = true;   // R8.4: Verbose labels the score s, not mu
            sc.RankOnUtility(sc.utility);
        }
        return out;
    }

    // -------------------------------------------------------------------------
    // OldScoreCapture -- moved from PipelineCoordinator::CaptureEligible (0.23.53)
    // -------------------------------------------------------------------------
    // The floor of a row is read off the same tests the floor pass applies:
    // the context floor (favourites exempt), else the utility floor (the cold
    // start rescues only what it keeps, which is kept). R8.1 (0.23.55): no
    // longer "suppressed favourite" first: such a favourite is a filter drop
    // now (favorite_suppressed), never an eligible row, so Floor::suppressed
    // stays in the format for older files and is not written.

    struct OldScoreCapture::Impl
    {
        const LegacyScorer* scorer;   // g_legacyScorer; null leaves the capture invalid
        const Run& run;
        const State::GameState& state;
        const State::PlayerActorState& player;
        const State::TargetCollection& targets;
        const State::WorldState& world;
        float combatOnsetAgo;
        std::array<float, Learning::StateFeatures::NUM_FEATURES> phi{};
        std::optional<Learning::FeatureBanditLearner::LockedReader> learner;
        std::optional<Learning::UsageMemory::SnapshotReader> usage;
    };

    OldScoreCapture::OldScoreCapture(const Run& run,
        const State::GameState& state, const State::PlayerActorState& player,
        const State::TargetCollection& targets, const State::WorldState& world,
        float combatOnsetAgo)
        : m(new Impl{ g_legacyScorer.get(), run, state, player, targets, world, combatOnsetAgo, {}, {}, {} })   // in place: the readers hold locks
    {
        m->phi = Learning::StateFeatures::FromState(player, targets).ToArray();
        if (!Valid()) return;
        if (g_featureBanditLearner) m->learner.emplace(g_featureBanditLearner->AcquireReader());
        if (g_usageMemory) m->usage.emplace(g_usageMemory->AcquireReader(state));
    }

    OldScoreCapture::~OldScoreCapture() = default;

    bool OldScoreCapture::Valid() const noexcept { return m->run.ran && m->scorer != nullptr; }

    Core::DecisionLog::OldRun OldScoreCapture::Head(int scored) const
    {
        Core::DecisionLog::OldRun run{};
        if (!Valid()) return run;
        const auto& cfg = m->scorer->GetConfig();
        run.valid = true;
        run.minCtx = cfg.minimumContextWeight;
        run.minUtil = cfg.minimumUtility;
        run.topN = static_cast<int>(cfg.topNCandidates);
        run.coldBoost = cfg.coldStartUCBBoost;
        run.beta = cfg.explorationWeight;
        run.lambdaMin = cfg.lambdaMin;
        run.lambdaMax = cfg.lambdaMax;
        run.n0 = Learning::FeatureBanditLearner::PriorPseudoObservations();
        run.favMode = cfg.favoritesMode == FavoritesMode::Boost ? "boost"
                    : cfg.favoritesMode == FavoritesMode::Off   ? "off"
                                                                : "suppress";
        // R8.4 (0.24.1): the favourites multiplier is uniform (no rank
        // scaling), so the head's range is [max, max].
        run.favMin = cfg.favoritesBoostMax;
        run.favMax = cfg.favoritesBoostMax;
        run.scored = scored;
        run.totalTrains = m->learner ? m->learner->TotalTrains() : 0.0f;
        run.phi.assign(m->phi.begin(), m->phi.end());
        return run;
    }

    Core::DecisionLog::OldScore OldScoreCapture::Explain(const Candidate::CandidateVariant& c, bool eligible,
        const ScoredCandidate* kept) const
    {
        namespace DL = Core::DecisionLog;
        DL::OldScore o;
        if (!Valid()) return o;
        const auto& cfg = m->scorer->GetConfig();
        const RE::FormID form = Candidate::GetFormID(c);
        o.favorite = Candidate::IsFavorited(c);
        o.ctx = Context::WeightForCandidate(c, m->run.weights);   // before a cold-start boost
        Learning::FeatureItemMetrics metrics{ 0.0f, 1.0f, 0.0f };
        if (m->learner) metrics = m->learner->GetMetrics(form, m->phi);
        o.trains = metrics.trains;
        if (kept) {
            const auto& b = kept->breakdown;
            o.est = b.rewardEstimate;
            o.alpha = b.confidence;
            o.ucb = b.ucb;
            o.rec = b.recencyBoost;
            o.prior = b.prior;
            o.floor = static_cast<uint8_t>(kept->isColdStartBoosted ? DL::Floor::cold : DL::Floor::pass);
            return o;
        }
        const float rec = m->usage ? m->usage->GetRecencyBoost(form) : 0.0f;
        o.est = metrics.rewardEstimate;
        o.alpha = metrics.confidence;
        o.ucb = metrics.ucb;
        o.rec = rec;
        const auto ex = m->scorer->ExplainCandidate(c, m->state, m->player, m->targets, m->combatOnsetAgo,
            o.ctx, metrics, rec);
        o.prior = ex.breakdown.prior;
        o.wouldUtil = ex.utility;
        if (eligible) {
            const DL::Floor f = (o.ctx < cfg.minimumContextWeight && !o.favorite) ? DL::Floor::ctx : DL::Floor::util;
            o.floor = static_cast<uint8_t>(f);
        }
        return o;
    }

    // -------------------------------------------------------------------------
    // The old learner's clock, sweep and counts
    // -------------------------------------------------------------------------

    bool HasLearner() noexcept { return g_featureBanditLearner != nullptr; }

    void AdvancePlayTime(float deltaSeconds)
    {
        if (g_featureBanditLearner) g_featureBanditLearner->AdvancePlayTime(deltaSeconds);
    }

    void MaybeForgetFaded()
    {
        if (g_featureBanditLearner) g_featureBanditLearner->MaybeForgetFaded();
    }

    LearnerCounts Counts()
    {
        LearnerCounts c;
        if (g_featureBanditLearner) {
            c.items = g_featureBanditLearner->GetItemCount();
            c.trains = g_featureBanditLearner->GetTotalTrainCount();
            c.forgot = g_featureBanditLearner->GetForgottenTotal();
        }
        return c;
    }

    bool WeightsChanged() noexcept
    {
        return g_featureBanditLearner && g_featureBanditLearner->WeightsChanged();
    }

    void ClearWeightsChanged() noexcept
    {
        if (g_featureBanditLearner) g_featureBanditLearner->ClearWeightsChanged();
    }
}
