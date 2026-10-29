#pragma once

// =============================================================================
// CHOICE SHADOW (R8.2) -- the new model's tick of one pipeline run
// =============================================================================
// PipelineCoordinator::ScoreShadow builds one ShadowRun per pipeline run, after
// the old scoring and the allocation (the ranking is untouched): the learner's
// snapshot, the shadow need vector (the live curve outputs, the bootstrap's
// curves re-read from their inputs), recent_<family> now, and the tick (v, W).
// It scores every eligible row with it; the run is cached beside the eligible
// rows (PipelineStateCache::OldScoreRun::shadow) so the selection log v3
// scores the held rows the run never gathered with the SAME tick when a
// context is taken (ScoreLoggedRow).
// =============================================================================

#include "ChoiceLearner.h"
#include "core/ChoiceScore.h"
#include "core/DecisionLog.h"

#include <cmath>
#include <memory>

namespace Huginn::Learning
{
    struct ShadowRun
    {
        std::shared_ptr<const ChoiceLearner::Snapshot> snap;
        Core::Choice::Tick tick;
        double playNow = 0.0;
    };

    /// mu, sd and the explanation of a logged row the run did not score (a
    /// held row it never gathered): no legacy feature (ln learn mult 0, util
    /// missing 1, as the fit reads a row the old engine did not score), the
    /// row's own cap, cross-features and covered (no upgrade gate here: the
    /// held rows' summon spells are candidates when castable). Writes the
    /// row's mu, sd, lm, um and ex.
    inline void ScoreLoggedRow(const ShadowRun& run, Core::DecisionLog::Row& r)
    {
        namespace C = Core::Choice;
        namespace DL = Core::DecisionLog;
        if (!run.snap) return;
        const auto& m = run.snap->model;
        const C::Attr at = C::MakeAttr(m.grading, r.cap, r.cross);
        C::RowExtra x;
        x.covered = r.cross[static_cast<std::size_t>(DL::Cross::covered)];
        const auto b = run.snap->B(r.form, (r.flags & DL::Flag::Favorite) != 0, run.playNow);
        x.bMu = b.mu;
        x.bVar = b.var;
        x.feat[static_cast<std::size_t>(C::Feature::util_missing)] = 1.0;
        if ((r.flags & DL::Flag::Shown) && r.slot >= 0 && r.slot < 8) {
            x.feat[static_cast<std::size_t>(C::Feature::slot0) + static_cast<std::size_t>(r.slot)] = 1.0;
        }
        x.offPage = !(r.flags & DL::Flag::Shown);
        const auto s = C::Score(m, run.tick, at, x);
        r.mu = static_cast<float>(s.mu);
        r.sd = static_cast<float>(s.Sd());
        r.lm = 0.0f;
        r.um = 1;
        r.exNeed = s.Learned() ? DL::kExLearned : s.exNeed;
        r.exCol = s.Learned() ? static_cast<std::int16_t>(-1) : s.exCol;
    }
}
