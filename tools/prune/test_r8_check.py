"""Mutation probes for r8_check.py (R8.4): each one plants a way back for the pruned ranking
layer, in memory, and the check must FAIL on the file it planted it in. The unmutated tree must
PASS. Standard library only.

usage:  python -I tools/prune/test_r8_check.py

A probe is (name, path, anchor, line, mode): `line` goes after (mode 'after') the first line
containing `anchor`, or replaces that line (mode 'replace'); 'delete-next' drops the line after
the anchor and 'delete-block' the anchor's line through its closing brace. The probes cover the verifier's
dodges of the first version of the check (utility ranking again through a member read, the
facade names, a changed allowed form) and the earlier set (old-engine names in the live scorer,
the pipeline, the console, the Debug suite, the display).
"""
import io
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import r8_check as rc  # noqa: E402

PC = 'src/pipeline/PipelineCoordinator.cpp'
US = 'src/learning/UtilityScorer.cpp'
IM = 'src/ui/IntuitionMenu.cpp'
FALLBACK = 'ctx.scoredCandidates = ctx.legacy.FallbackRanking();'
SC = 'src/learning/ScoredCandidate.h'
SC_LESS = 'return Core::RanksBefore(score, TieBreakDps(), other.score, other.TieBreakDps());'
SC_SLOT = 'return isRememberedOnly ? Core::kUnrankedScore : score;'
D2 = 'C::SetLegacy(x.feat,'

PROBES = [
    # --- 0.24.17: the D2 engine in shadow is never read live (round 1: the verifier's aliases) ---
    ('0.24.17: A\'s mu ranks the live list in RankOnChoice', PC, 'legacyShadowFeat, ctx.legacyShadow);',
     '        for (size_t i = 0; i < rows; ++i) out[i].score = ctx.legacyShadow.mu[i];', 'after'),
    ('0.24.17: the live allocation ranks on A\'s mu', PC, 'Core::SlotAlloc::Input* const legacyShadowInput',
     '    for (auto& sc : ctx.scoredCandidates) sc.score = ctx.legacyShadow.mu[sc.runRow];', 'after'),
    ('0.24.17: the live mu column copied from A\'s (an allowed form changed)', PC,
     'e.legacyShadowMu = static_cast<float>(ctx.legacyShadow.mu[r]);',
     '            e.legacyShadowMu = static_cast<float>(ctx.legacyShadow.mu[r]); e.shadowMu = e.legacyShadowMu;',
     'replace'),
    ('0.24.17: a second read of A\'s mu in RankOnChoice', PC, 'legacyShadowFeat, ctx.legacyShadow);',
     '        const double probe = ctx.legacyShadow.mu.empty() ? 0.0 : ctx.legacyShadow.mu[0];', 'after'),
    ('0.24.17: an alias of the shadow run in ShadowLegacyPages', PC, 'if (!ctx.legacyShadow.ran) return;',
     '    const auto& sh = ctx.legacyShadow;', 'after'),
    ('0.24.17: the live scores from A\'s mu in ShadowLegacyPages', PC, 'if (!ctx.legacyShadow.ran) return;',
     '    for (auto& sc : ctx.scoredCandidates) if (sc.runRow < ctx.legacyShadow.mu.size()) '
     'sc.score = ctx.legacyShadow.mu[sc.runRow];', 'after'),
    ('0.24.17: the live page driven by A\'s page key', PC, 'if (!ctx.legacyShadow.ran) return;',
     '    if (!ctx.legacyShadow.keyA.empty() && ctx.legacyShadow.keyA[0] >= 0) ctx.assignments.clear();', 'after'),
    ('0.24.17: A\'s scores written into the ranked list after the pass', PC, 'Core::LegacyShadow::RanksAndPages(',
     '    ctx.scoredCandidates[0].score = ctx.legacyShadow.mu[0];', 'after'),
    ('0.24.17: B made A (the live feature with the switch literal true)', PC, D2,
     '        C::SetLegacy(x.feat, C::LegacyFeature(true, lg.Scored(), static_cast<double>(lg.lnLearnMult)));',
     'replace'),
    ('0.24.17: A\'s feature into the live extras', PC, 'if (shadowA) legacyShadowFeat[i] = ',
     '        if (shadowA) C::SetLegacy(x.feat, legacyShadowFeat[i]);', 'after'),
    ('0.24.17: the live pass scores on A\'s features', PC, 'C::ScoreForRanking(model, tick, attrs, extras, gateRows',
     '    for (size_t i = 0; i < legacyShadowFeat.size(); ++i) C::SetLegacy(extras[i].feat, legacyShadowFeat[i]); '
     'C::ScoreForRanking(model, tick, attrs, extras, gateRows, atLimit ? outIdx : SIZE_MAX, leftOut, rows, scores);',
     'replace'),
    ('0.24.17: the live pass on a revived untagged A extras (the verifier\'s; PRUNED_SCOPED)', PC,
     'C::ScoreForRanking(model, tick, attrs, extras, gateRows',
     '    C::ScoreForRanking(model, tick, attrs, shadowExtras, gateRows, atLimit ? outIdx : SIZE_MAX, leftOut, rows, scores);',
     'replace'),
    ('0.24.17: a copy of A\'s feature vector', PC, 'if (shadowA) legacyShadowFeat[i] = ',
     '        auto probeCopy = legacyShadowFeat;', 'after'),
    ('0.24.17: CaptureEligible writes the floors\' lm whatever the switch (REQUIRED)', PC, 'if (ctx.legacyLive) {',
     '            if (true) {', 'replace'),
    ('0.24.17: the learner reads A\'s page key', 'src/learning/ChoiceLearner.cpp', 'const bool scored = r.um == 0;',
     '            if (r.legacyShadowKeyA >= 0) f[static_cast<std::size_t>(C::Feature::eligible)] += 1.0;', 'after'),
    ('0.24.17: the learner reads B\'s stateless page key', 'src/learning/ChoiceLearner.cpp',
     'const bool scored = r.um == 0;', '            if (r.legacyShadowKeyB0 >= 0) f[0] += 1.0;', 'after'),
    ('0.24.17: the learner reads B\'s shadow rank', 'src/learning/ChoiceLearner.cpp', 'const bool scored = r.um == 0;',
     '            if (r.legacyShadowRankB > 0) f[0] += 1.0;', 'after'),
    ('0.24.17: the learner reads util again', 'src/learning/ChoiceLearner.cpp', 'const bool scored = r.um == 0;',
     '            const bool scored = std::isfinite(r.util);', 'replace'),
    ('0.24.17: the learner\'s um read swapped for another column (REQUIRED)', 'src/learning/ChoiceLearner.cpp',
     'const bool scored = r.um == 0;', '            const bool scored = r.lm != 0.0f;', 'replace'),
    ('0.24.17: the live learner\'s subscriber reads A\'s mu', 'src/learning/ChoiceSubscriber.h',
     'if (event.v3Decision) Feed(m_learner, *event.v3Decision);',
     '            if (event.v3Decision && event.v3Decision->ctx && !event.v3Decision->ctx->rows.empty() && '
     'event.v3Decision->ctx->rows[0].legacyShadowMu > 0.0f) return;', 'after'),
    ('0.24.17: the slot locker reads A\'s page key', 'src/slot/SlotLocker.cpp', '#include',
     'static int Probe(const Huginn::Learning::PipelineStateCache::EligibleRow& e) { return e.legacyShadowKeyA; }',
     'after'),
    ('0.24.17: a display backend reads the shadow run', 'src/display/WheelerBackend.cpp', '#include',
     'static bool Probe(const Huginn::Pipeline::PipelineContext& c) { return c.legacyShadow.ran; }', 'after'),
    ('0.24.17: the [Soak] tally\'s read of A\'s key used elsewhere in the log', 'src/learning/SelectionLogV3.cpp',
     'r.util = e.utility;', '            if (e.legacyShadowKeyA >= 0) r.flags |= Flag::Shown;', 'after'),
    # --- 0.24.17 (review round 2): the verifier's escapes ---
    ('0.24.17: an A score from the allocation input into the live list (old name; PRUNED_SCOPED)', PC, 'Core::LegacyShadow::RanksAndPages(liveOrder, liveMu, tie, legacyShadowPageInput, ctx.pageRunRows, ctx.legacyShadow);',
     '    if (!ctx.allocInput.candidates.empty()) ctx.scoredCandidates.front().score = ctx.allocInput.candidates.front().score;',
     'after'),
    ('0.24.17: an A score from the allocation input into the live list', PC, 'Core::LegacyShadow::RanksAndPages(liveOrder, liveMu, tie, legacyShadowPageInput, ctx.pageRunRows, ctx.legacyShadow);',
     '    if (!ctx.legacyShadowAllocInput.candidates.empty()) ctx.scoredCandidates.front().score = '
     'ctx.legacyShadowAllocInput.candidates.front().score;', 'after'),
    ('0.24.17: the allocation input read in DeriveDisplayLabels', PC,
     'void PipelineCoordinator::DeriveDisplayLabels(PipelineContext& ctx)',
     '{ const double aTop = ctx.legacyShadowAllocInput.candidates.empty() ? 0.0 : '
     'ctx.legacyShadowAllocInput.candidates[0].score; (void)aTop; }', 'after'),
    ('0.24.17: the page-input alias read after the pass', PC, 'Core::LegacyShadow::RanksAndPages(liveOrder, liveMu, tie, legacyShadowPageInput, ctx.pageRunRows, ctx.legacyShadow);',
     '    if (legacyShadowPageInput && !legacyShadowPageInput->candidates.empty()) ctx.scoredCandidates.front().score = '
     'legacyShadowPageInput->candidates[0].score;', 'after'),
    ('0.24.17: the page-input alias read after the pass (old name; PRUNED_SCOPED)', PC, 'Core::LegacyShadow::RanksAndPages(liveOrder, liveMu, tie, legacyShadowPageInput, ctx.pageRunRows, ctx.legacyShadow);',
     '    if (pageInput && !pageInput->candidates.empty()) ctx.scoredCandidates.front().score = pageInput->candidates[0].score;',
     'after'),
    ('0.24.17: the clear of the allocation input after the pass dropped (REQUIRED)', PC,
     'ctx.legacyShadowAllocInput = {};', '', 'replace'),
    ('0.24.17: legacyLive set from runLegacy (A live whenever the shadow runs)', PC,
     'ctx.legacyLive = legacyOn;', '    ctx.legacyLive = runLegacy;', 'replace'),
    ('0.24.17: legacyLive set true', PC, 'ctx.legacyLive = legacyOn;', '    ctx.legacyLive = true;', 'replace'),
    ('0.24.17: a second legacyLive assignment', PC, 'ctx.legacyLive = legacyOn;', '    ctx.legacyLive = shadowOn;', 'after'),
    ('0.24.17: RankOnChoice legacyOn = !ctx.legacyLive', PC,
     'const bool legacyOn = ctx.legacyLive;', '    const bool legacyOn = !ctx.legacyLive;', 'replace'),
    ('0.24.17: legacyLive defaults on in the run context', 'src/pipeline/PipelineCoordinator.h',
     'bool legacyLive = false;', '        bool legacyLive = true;', 'replace'),
    ('0.24.17: Reset no longer clears legacyLive (REQUIRED)', 'src/pipeline/PipelineCoordinator.h',
     '            legacyLive = false;', '', 'replace'),
    ('0.24.17: Reset no longer clears the allocation input (REQUIRED)', 'src/pipeline/PipelineCoordinator.h',
     '            legacyShadowAllocInput = {};', '', 'replace'),
    ('0.24.17: a leftover um copy after CaptureEligible\'s guard', PC,
     'if (static_cast<uint8_t>(lg.floor) != row.old.floor) ++floorMismatch;',
     '            row.utilMissing = lg.utilMissing ? 1 : 0;', 'after'),
    ('0.24.17: the [Soak] tally read for a live decision', 'src/telemetry/SoakMetrics.cpp',
     'legacyShadowTally = legacyShadowWindowTally;',
     '            if (legacyShadowWindowTally.keysOnA > legacyShadowWindowTally.keysOnB0) m_wildcardPicks.fetch_add(1);',
     'after'),
    ('0.24.17: B reads A\'s feature from the legacy row', PC, 'C::SetLegacy(x.feat, C::LegacyFeature(legacyOn, lg.Scored(), static_cast<double>(lg.lnLearnMult)));',
     '        x.feat[static_cast<size_t>(C::Feature::ln_learn_mult)] = static_cast<double>(lg.lnLearnMult);', 'after'),
    ('0.24.17: the legacy feature added to B\'s mu', PC, 'C::SetLegacy(x.feat, C::LegacyFeature(legacyOn, lg.Scored(), static_cast<double>(lg.lnLearnMult)));',
     '        x.bMu += 0.1 * static_cast<double>(lg.lnLearnMult);', 'after'),
    ('0.24.17: the old floor filters B\'s rows', PC, 'C::SetLegacy(x.feat, C::LegacyFeature(legacyOn, lg.Scored(), static_cast<double>(lg.lnLearnMult)));',
     '        if (static_cast<int>(lg.floor) != 0) x.covered = 1.0;', 'after'),
    ('0.24.17: the v3 row\'s um from the Scored flag (REQUIRED)', 'src/learning/SelectionLogV3.cpp',
     'r.um = e.utilMissing;', '            r.um = (r.flags & Flag::Scored) ? 0 : 1;', 'replace'),
    # --- R11: the retired classifiers come back into the live engine ---
    ('R11: a tag read back in SlotClassifier', 'src/slot/SlotClassifier.cpp', 'namespace Huginn::Slot',
     '    static bool Probe(const Candidate::SpellCandidate& s) { return Spell::HasTag(s.tags, Spell::SpellTag::RestoreHealth); }',
     'after'),
    ('R11: the summon filter by classifier type', 'src/candidate/CandidateFilters.cpp', 'if (f.Has(F::Summon) &&',
     '                    if (c.type == Spell::SpellType::Summon &&', 'replace'),
    ('R11: a potion styled by classifier type', 'src/slot/SlotUtils.h',
     'if (subject.kind == Core::Effect::Kind::Potion) {',
     '                    if (item.type == Item::ItemType::HealthPotion) {', 'replace'),
    ('R11: a classifier include in the console', 'src/console/ConsoleCommands.cpp', '#include "effect/Facets.h"',
     '#include "learning/item/ItemClassifier.h"', 'after'),
    ('R11: a per-type dump back', 'src/console/ConsoleCommands.cpp', 'namespace Huginn::Console',
     '   static void Cmd_DumpSpells(std::string_view);', 'after'),
    ('R11: a tagsExt read in the label', 'src/display/ExplanationLabel.h', 'return "Ally Support";',
     '            if (std::get<Candidate::SpellCandidate>(candidate).tagsExt != Spell::SpellTagExt{}) return "x";', 'after'),
    ('R11: an element read in the filters', 'src/candidate/CandidateFilters.cpp',
     'const bool isFood = subject.kind == Core::Effect::Kind::Food;',
     '        const auto probeElement = std::get<SpellCandidate>(candidate).element;', 'after'),
    ('R11: SlotClassifier no longer asks the facets (REQUIRED)', 'src/slot/SlotClassifier.cpp',
     'return Core::Facets::Matches(SubjectOf(candidate), classification);', '        return false;', 'replace'),
    ('R11: the filters no longer read the facets (REQUIRED)', 'src/candidate/CandidateFilters.cpp',
     'const auto subject = Effect::SubjectOf(candidate);', '        const Core::Facets::Subject subject{};', 'replace'),
    ('R11: a third soul gem line (budget)', 'src/candidate/CandidateGenerator.cpp',
     'if (item.data.type != Item::ItemType::SoulGem) return;',
     '            if (item.data.type == Item::ItemType::HealthPotion) return;', 'after'),
    ('R11: the potion pick by type back', 'src/override/OverrideManager.cpp', '#include "effect/Facets.h"',
     '// const auto pick = registry->GetBestPotion(type, 1.0f);', 'after'),
    ('R11: a classifier type read in the pipeline', PC, 'sc.hasSubject = true;',
     '        if (sc.Is<Candidate::ItemCandidate>() && sc.As<Candidate::ItemCandidate>().type == Item::ItemType::Poison) continue;',
     'after'),
    ('R11: a classifier header in a display backend', 'src/display/IntuitionBackend.cpp', '#include',
     '#include "spell/SpellClassifier.h"', 'after'),
    ('R11: the tag side-effect test back in the overrides', 'src/override/OverrideManager.cpp',
     '#include "effect/Facets.h"', '// if (!i.data.HasHarmfulSideEffects()) pure = &i;', 'after'),
    ('R11: a registry potion view by tag called from the overrides', 'src/override/OverrideManager.cpp',
     '#include "effect/Facets.h"',
     'static const auto* P(const Item::ItemRegistry& r) { return r.GetBestResistFirePotion(); }', 'after'),
    ('R11: the overrides stop asking the side-effect facets (REQUIRED)', 'src/override/OverrideManager.cpp',
     'return Core::Facets::HasSideEffect(ff.facets);', '        return false;', 'replace'),
    ('R11: the cloak guard dropped from the resist filter (REQUIRED)', 'src/candidate/CandidateFilters.cpp',
     'if (!Core::Facets::ResistJudgeable(facets)) return false;', '', 'replace'),
    ('R11: a restore potion ranked by the costliest effect again (REQUIRED)', 'src/override/OverrideManager.cpp',
     'const float restored = PotionRestoreWithin(',
     '            const float restored = item.data.RestoredWithin(windowSec);', 'replace'),
    ('R11: a registry scroll view by type called from the overrides', 'src/override/OverrideManager.cpp',
     '#include "effect/Facets.h"',
     'static const auto* S(const Scroll::ScrollRegistry& r) { return r.GetBestHealingScroll(); }', 'after'),
    ('R11: a comment sending authors to a deleted dump (STALE)', 'src/spell/SpellRegistry.cpp',
     'm_classifier.ReportOverrideUsage("after rebuild"sv);',
     '      // `hg dump spells` classifies the whole form array and is the complete check.', 'after'),
    ('R11: the old resist check back', 'src/candidate/CandidateFilters.h', '#pragma once',
     '// bool IsResistSpellRedundant(const SpellCandidate&) const;', 'after'),
    # --- round 5: the v3 row's `.util` and the old floors' keep flag are LEGACY ---
    ('R5 A: lm from the v3 row\'s util in the learner\'s RowFeatures', 'src/learning/ChoiceLearner.cpp',
     'const bool scored = r.um == 0;',
     '            f[static_cast<std::size_t>(C::Feature::ln_learn_mult)] = '
     'std::log(std::max(static_cast<double>(r.util), 1e-6));', 'after'),
    ('R5 E: the live scorer filters on the old floors\' keep flag', US,
     'std::stable_sort(scored.begin(), scored.end());',
     '            std::erase_if(scored, [](const ScoredCandidate& s) { return !s.legacyScored; });', 'after'),
    ('R5 F: RankOnChoice boosts the old floors\' rows', PC,
     'x.feat[static_cast<size_t>(C::Feature::eligible)] = 1.0;',
     '        if (lg.Scored()) x.bMu += 1.0;', 'after'),
    # --- round 4: ln_util is LEGACY; the live learner's path is not allowed whole ---
    ('R4: ln util back in the learner\'s RowFeatures', 'src/learning/ChoiceLearner.cpp',
     'if (std::isfinite(r.lm))',
     '                f[static_cast<std::size_t>(C::Feature::ln_util)] = '
     'std::log(std::max(static_cast<double>(r.util), 1e-6));', 'after'),
    ('R4: ln util back in RankOnChoice', PC, D2,
     '        if (sc.legacyScored) x.feat[static_cast<size_t>(C::Feature::ln_util)] = 1.0;', 'after'),
    ('R4: the bootstrap\'s ln util refusal dropped (continue deleted)', 'src/core/Bootstrap.cpp',
     'if (w != 0.0) ++rep.featuresRefused;', '', 'delete-next'),
    ('R4: the bootstrap\'s ln util refusal dropped (whole if deleted)', 'src/core/Bootstrap.cpp',
     'Choice::Feature::ln_util) {', '', 'delete-block'),
    ('R4: the v3 row\'s lm taken from the old utility', 'src/learning/SelectionLogV3.cpp',
     'r.util = e.utility;', '            r.util = e.utility; r.lm = std::log(e.utility);', 'replace'),
    ('R4: the old learner read in ChoiceSubscriber', 'src/learning/ChoiceSubscriber.h',
     'if (event.v3Decision) Feed(m_learner, *event.v3Decision);',
     '            if (event.v3Decision && g_featureBanditLearner) Feed(m_learner, *event.v3Decision);', 'replace'),
    ('R4: the alloc input\'s score from the old utility', 'src/slot/SlotSnapshot.cpp',
     'c.utility = sc.utility;', '        c.utility = sc.utility; c.score = std::log(sc.utility);', 'replace'),
    # --- round 3: ScoredCandidate.h and SlotScoreMath.h rank live (budgets, not allows) ---
    ('R3 must-fix 1.1: utility ranks the list again through operator<', SC, SC_LESS,
     '            return Core::RanksBefore(Core::BridgeScore(utility), TieBreakDps(), '
     'Core::BridgeScore(other.utility), other.TieBreakDps());', 'replace'),
    ('R3 must-fix 1.1b: operator< on the bare utility', SC, SC_LESS,
     '            return utility > other.utility;', 'replace'),
    ('R3 must-fix 1.2: SlotScore() on the bridged utility', SC, SC_SLOT,
     '            return isRememberedOnly ? Core::kUnrankedScore : Core::BridgeScore(utility);', 'replace'),
    ('R3 must-fix 1.3: a wrapper accessor on the old utility', SC, '// Check if this is a specific candidate type',
     '        [[nodiscard]] float OldU() const noexcept { return utility; }', 'after'),
    ('R3: a bridge call added to the live slot math', 'src/core/SlotScoreMath.h', 'namespace Huginn::Core',
     '    inline SlotScore ProbeScore(float u) noexcept { return BridgeScore(u); }', 'after'),
    ('R3 must-fix 2: the stale reason-from-weights comment back', 'src/candidate/CandidateGenerator.cpp',
     'No relevance metadata is stamped here',
     '        // No relevance metadata is stamped here: the per-tick display reason is\n'
     '        // derived by the pipeline from the scorer\'s context weights (#10).', 'replace'),
    ('R3: a qualified read (pointer to member) of utility', PC, FALLBACK,
     '        auto pm = &Scoring::ScoredCandidate::utility; '
     'for (auto& sc : ctx.scoredCandidates) sc.score = std::log(sc.*pm);', 'after'),
    ('R3: the cold-start flag read in RankOnChoice', PC, D2,
     '        if (sc.isColdStartBoosted) x.feat[0] += 1.0;', 'after'),
    ('R3: a Suppress skip put back in RankOnChoice', PC, D2,
     '        if (g_utilityScorer->GetConfig().favoritesMode == Scoring::FavoritesMode::Suppress '
     '&& sc.IsFavorited()) continue;', 'after'),

    # --- the R8.4 verifier's dodges: none renames anything ---
    ('a: utility ranks again in the live pipeline', PC, FALLBACK,
     '        for (auto& sc : ctx.scoredCandidates) sc.score = std::log(sc.utility);', 'after'),
    ('b: the ln_util feature back in RankOnChoice', PC, 'C::SetLegacy(x.feat,',
     '        x.feat[0] = std::log(std::max(sc.utility, 1e-6f));', 'after'),
    ('c: the Verbose context breakdown back', IM, 'const double s = assignment.score;',
     '            const float old = scored.breakdown.contextWeight + scored.breakdown.learningScore;', 'after'),
    ('d: utility x context weight in the live scorer', US, 'const auto& p = scored.choice;',
     '            const float u = scored.utility * bd.contextWeight;', 'after'),
    ('e: a second old-learner read in SoakMetrics', 'src/telemetry/SoakMetrics.cpp',
     'const auto oldLearner = Scoring::Legacy::Counts();',
     '        const auto again = Scoring::Legacy::Counts();', 'after'),
    ('f: the old learner clock called from a display backend', 'src/display/IntuitionBackend.cpp',
     '#include', '#include "learning/legacy/LegacyRun.h"', 'after'),
    ('g: using namespace Legacy, then bare facade calls', IM, 'namespace Huginn::UI',
     'using namespace Huginn::Scoring::Legacy;', 'after'),
    ('h: an allowed pipeline form changed (utility scaled)', PC, 'sc.utility = L->utility;',
     '            sc.utility = L->utility * 2.0f;', 'replace'),
    ('i: a breakdown part read in the slot locker', 'src/slot/SlotLocker.cpp', '#include',
     'static float Probe(const Huginn::Scoring::ScoredCandidate& c) { return c.breakdown.contextWeight; }', 'after'),
    ('j: a second fallback ranking call in the pipeline', PC, FALLBACK,
     '        ctx.scoredCandidates = ctx.legacy.FallbackRanking();', 'after'),
    ('k: a kept row read off the pipeline handle elsewhere', 'src/learning/ReachReporter.cpp', '#include',
     'static auto Probe(const Huginn::Pipeline::PipelineContext& ctx) { return ctx.legacy.KeptFor(0); }', 'after'),
    # --- the earlier set ---
    ('the old learner named in the live scorer', US, 'namespace Huginn::Scoring',
     '    static Learning::FeatureBanditLearner* s_probe = nullptr;', 'after'),
    ('topNCandidates read in the pipeline', PC, FALLBACK,
     '        const auto n = g_utilityScorer->GetConfig().topNCandidates;', 'after'),
    ('lambdaMax read in the pipeline', PC, FALLBACK,
     '        const auto l = g_utilityScorer->GetConfig().lambdaMax;', 'after'),
    ('RankOnUtility called in the pipeline', PC, FALLBACK,
     '        for (auto& sc : ctx.scoredCandidates) sc.RankOnUtility(1.0f);', 'after'),
    ('a dead accessor back in the pipeline', PC, FALLBACK,
     '        auto* fb = GetFeatureBanditLearner();', 'after'),
    ('a legacy global in a console command', 'src/console/ConsoleCommands.cpp', 'namespace Huginn::Console',
     '    static auto* s_probe = g_featureBanditLearner.get();', 'after'),
    ('a legacy global in the Debug suite', 'src/Tests.cpp', 'namespace',
     'static auto* s_probe = g_usageMemory.get();', 'after'),
    ('DisplayUtility in the HUD menu', IM, 'namespace Huginn::UI',
     '    static float Probe(float s) { return Huginn::Core::DisplayUtility(s); }', 'after'),
    ('g_legacyScorer in a display backend', 'src/display/WheelerBackend.cpp', '#include',
     'static bool Probe() { return g_legacyScorer != nullptr; }', 'after'),
    # --- PRUNED symbols coming back ---
    ('the potion tier preference back', US, 'namespace Huginn::Scoring',
     '    void ApplyPotionTierPreference();', 'after'),
    ('a Suppress branch back in the live scorer', US, 'const auto& p = scored.choice;',
     '            if (m_config.favoritesMode == FavoritesMode::Suppress) continue;', 'after'),
    ('the potion multiplier\'s own timer back', 'src/learning/PotionDiscriminator.h', '#pragma once',
     'inline float m_combatDuration = 0.0f;', 'after'),
    ('the elemental window back', 'src/pipeline/PipelineCoordinator.h', '#pragma once',
     'inline bool m_elementalWindowActive = false;', 'after'),
]
INI_PROBE = ('a pruned INI key back', 'configs/Huginn.ini', 'sPotionTierPreference = Lowest')


def mutate(text, anchor, line, mode):
    lines = text.splitlines()
    for i, ln in enumerate(lines):
        if anchor in ln:
            if mode == 'after':
                lines.insert(i + 1, line)
            elif mode == 'delete-next':        # drop the line after the anchor
                del lines[i + 1]
            elif mode == 'delete-block':       # drop the anchor's line through its closing brace
                indent = len(ln) - len(ln.lstrip())
                j = i + 1
                while not (lines[j].strip() == '}' and len(lines[j]) - len(lines[j].lstrip()) == indent):
                    j += 1
                del lines[i:j + 1]
            else:
                lines[i] = line
            return '\n'.join(lines) + '\n'
    raise AssertionError(f'anchor not found: {anchor!r}')


def run(tree, ini):
    sink = io.StringIO()
    bad, paths = rc.check(tree, ini, out=lambda s: sink.write(s + '\n'))
    return bad, paths, sink.getvalue()


def main():
    tree, ini = rc.load_tree(), rc.load_ini()
    failures = 0
    bad, _, report = run(tree, ini)
    if bad:
        print('FAIL baseline: the unmutated tree does not pass\n' + report)
        return 1
    print('ok   baseline: the unmutated tree passes')
    for name, path, anchor, line, mode in PROBES:
        if path not in tree:
            print(f'FAIL {name}: {path} is not in the scanned tree')
            failures += 1
            continue
        t2 = dict(tree)
        t2[path] = mutate(tree[path], anchor, line, mode)
        bad, paths, report = run(t2, ini)
        if bad and path in paths:
            print(f'ok   {name}: caught in {path}')
        else:
            print(f'FAIL {name}: r8_check passed it ({path})\n{report}')
            failures += 1
    name, path, line = INI_PROBE
    i2 = dict(ini)
    i2[path] = (ini.get(path) or '') + '\n' + line + '\n'
    bad, paths, _ = run(tree, i2)
    if bad and path in paths:
        print(f'ok   {name}: caught in {path}')
    else:
        print(f'FAIL {name}: r8_check passed it')
        failures += 1
    for f in rc.REQUIRED_INI:   # a moved or renamed INI / dMenu JSON must fail, not pass silently
        i2 = dict(ini)
        i2[f] = None
        bad, paths, _ = run(tree, i2)
        if bad and f in paths:
            print(f'ok   R3: {f} missing: caught')
        else:
            print(f'FAIL R3: {f} missing: r8_check passed it')
            failures += 1
    total = len(PROBES) + 1 + len(rc.REQUIRED_INI)
    print(f'{total - failures}/{total} probes caught' + ('' if not failures else f'; {failures} FAILED'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
