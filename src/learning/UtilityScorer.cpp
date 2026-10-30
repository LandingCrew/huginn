#include "UtilityScorer.h"
#include "state/StateManager.h"                 // GetPlayerVitals: vital envelopes tick every frame
#include "Profiling.h"
#include "core/EffectColumns.h"   // [Recs]: the explanation's column names (R8.3)
#include "core/NeedIds.h"         // ... and need names
#include <algorithm>
#include <chrono>

namespace Huginn::Scoring
{
    UtilityScorer::UtilityScorer(const ScorerConfig& config)
        : m_config(config)
    {
        m_wildcardMgr.SetEnabled(true);  // Always enabled by default
    }

    // R8.4: the legacy pass, the old per-candidate score and the members it
    // used (the old learner, prior, correlations, potion multiplier, context
    // rules) moved to the legacy module (src/learning/legacy/) unchanged in
    // arithmetic; tools/prune/r8_check.py keeps them out of this file.

    void UtilityScorer::RankAndApplyWildcards(ScoredCandidateList& scored, const WildcardPage& displayPage)
    {
        // The whole list in rank order (R7), on SlotScore's score (R8.3: the
        // choice score mu; ln utility in the fallback ranking) -- until
        // ApplyWildcards below swaps a wildcard up into a rank position. The
        // slot code takes the FIRST match in list order for a job key or an
        // uncapped Regular key, so the tail must be sorted too. Stable, so
        // exact ties (same score, same DPS) keep the order the candidates were
        // generated in. Cost (R7 review, 500 rows): about 42 us a pass.
        if (!scored.empty()) {
            Huginn_ZONE_NAMED("Score::Sort");
            std::stable_sort(scored.begin(), scored.end());
        }

        // Apply wildcards for exploration, keyed and sized against the page
        // THIS tick allocates. WildcardManager works on rank POSITIONS (a draw
        // from below the slot, a swap up into it), never on score values, so
        // it is sign-safe as it stands (R8.3); R10 replaces it.
        {
            Huginn_ZONE_NAMED("Score::Wildcards");
            m_wildcardMgr.ApplyWildcards(scored, displayPage);
        }
    }

    bool UtilityScorer::Update(float deltaSeconds)
    {
        (void)deltaSeconds;   // R8.1: the potion multiplier's combat timer, its one reader, is gone
        bool forceRun = m_wildcardMgr.UpdateExpiry();

        // Vital envelopes tick every frame, not just when the pipeline runs: a
        // hold ending, or a potion, changes the context weights without
        // changing anything the skip gate hashes. Release -> one re-run.
        const auto vitals = State::StateManager::GetSingleton().GetPlayerVitals();
        const auto now = Context::VitalEnvelope::Clock::now();
        forceRun |= m_magickaEnvelope.Tick(vitals.magicka, now);
        forceRun |= m_staminaEnvelope.Tick(vitals.stamina, now);
        return forceRun;
    }

    void UtilityScorer::Reset()
    {
        m_wildcardMgr.Reset();
        m_magickaEnvelope.Reset();
        m_staminaEnvelope.Reset();
    }

    // =========================================================================
    // DEBUG LOGGING
    // =========================================================================

    void UtilityScorer::LogTopCandidates(
        const ScoredCandidateList& ranked, size_t count, bool detail, bool force, bool fallback) const
    {
        size_t numToLog = std::min(count, ranked.size());

        if (!force) {
            // Dedup: skip the dump unless the ranking's membership/order changed.
            // Utilities are continuous in the vitals, so they drift every scoring
            // run — including them here would defeat the dedup. Forced dumps
            // (hg recs) bypass and don't update the signature.
            // NOTE: Single-threaded (called from update thread only)
            static std::string s_lastSignature{"<none>"};
            std::string signature;
            for (size_t i = 0; i < numToLog; ++i) {
                signature += fmt::format("{}|", ranked[i].GetName());
            }
            if (signature == s_lastSignature) {
                return;
            }
            s_lastSignature = std::move(signature);
        }

        if (numToLog == 0) {
            logger::info("[Recs] No candidates to display"sv);
            return;
        }

        // R8.3: the ranking is the choice score mu. Each line: mu (and sd),
        // the three largest theta[need][col] n a terms (the first is the
        // explanation), b, and the D2 legacy feature (lm = ln learn mult on a
        // row the old floors kept, "um" when they did not). R8.4: no old
        // utility on these lines; with `detail` (hg recs), the inputs lm was
        // computed from on a kept row (ctx, λ, learn and its parts). The
        // fallback ranking (no bootstrap) prints the old line, flagged.
        // Phase 9 / R11 expand this.
        logger::info("[Recs] === Top {}/{} candidates | {} ==="sv, numToLog, ranked.size(),
            fallback ? "FALLBACK: s = ln u, u = ctx*(1+λ*learn)*mults"
                     : "mu = Σ θ·n·cap + b + w·lm (+ recent, covered); terms need×col");

        for (size_t i = 0; i < numToLog; ++i) {
            const auto& scored = ranked[i];
            const auto& bd = scored.breakdown;
            const auto& p = scored.choice;

            std::string terms;
            for (const auto& t : p.top) {
                if (t.need < 0) break;
                const auto ni = static_cast<size_t>(t.need);
                const auto ci = static_cast<size_t>(t.col);
                terms += fmt::format("{}{}×{}={:+.3f}", terms.empty() ? "" : " ",
                    ni < Core::Needs::kNeedCount ? Core::Needs::kNeeds[ni].id : std::string_view("?"),
                    ci < Core::Effect::kColumnCount ? Core::Effect::kColumns[ci].id : std::string_view("?"), t.value);
            }
            if (terms.empty()) terms = "learned";
            const std::string legacy = !scored.legacyScored ? std::string(" | lm: under the old floors")
                                     : detail               ? fmt::format(" | lm from {}", bd.ToDetailString())
                                                            : std::string{};

            if (fallback) {
                logger::info("[Recs] {}. {} ({}) s={:+.3f} u={:.3f} | {}{}{}"sv,
                    i + 1, scored.GetName(), Candidate::SourceTypeToString(scored.GetSourceType()),
                    scored.score, scored.utility,
                    detail ? bd.ToDetailString() : bd.ToCompactString(),
                    scored.isWildcard ? " [WC]" : "",
                    scored.isColdStartBoosted ? " [COLD]" : "");
                continue;
            }
            logger::info("[Recs] {}. {} ({}) mu={:+.3f} sd={:.3f} | {} | need={:+.3f} b={:+.3f} lm={:+.3f}{}{}{}{}{}"sv,
                i + 1,
                scored.GetName(),
                Candidate::SourceTypeToString(scored.GetSourceType()),
                scored.score, std::sqrt(std::max(p.var, 0.0)),
                terms, p.needMu, p.b, p.lnLearnMult, p.utilMissing != 0.0 ? " um" : "",
                p.covered > 0.0 ? fmt::format(" covered={:.2f}", p.covered) : std::string{},
                legacy,
                scored.isWildcard ? " [WC]" : "",
                scored.isColdStartBoosted ? " [COLD]" : "");
        }
    }

}  // namespace Huginn::Scoring
