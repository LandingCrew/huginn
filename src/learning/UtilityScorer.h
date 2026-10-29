#pragma once

#include "ScorerConfig.h"
#include "context/VitalEnvelope.h"
#include "ScoredCandidate.h"
#include "WildcardManager.h"

#include <vector>

namespace Huginn::Scoring
{
    // =============================================================================
    // UTILITY SCORER -- the live scorer's duties (R8.4: OFF the D2 allowlist)
    // =============================================================================
    // Since the cutover (R8.3) nothing here ranks on utility, and since R8.4
    // nothing here computes it: the legacy pass moved to the legacy module
    // (src/learning/legacy/), and tools/prune/r8_check.py keeps the old engine
    // out of this file. What is left:
    //   - the rank sort and the wildcards (RankAndApplyWildcards, on
    //     SlotScore(): the choice score mu; R10 replaces the wildcards);
    //   - the magicka / stamina envelopes the needs (and the legacy pass) read;
    //   - the [Recs] lines;
    //   - the [Scoring] config snapshot (ScorerSettings), of which the live
    //     pipeline reads only favoritesMode (the favorite_suppressed filter,
    //     the choice learner's battery).
    // =============================================================================

    class UtilityScorer
    {
    public:
        explicit UtilityScorer(const ScorerConfig& config = DefaultScorerConfig);
        ~UtilityScorer() = default;

        // R8.3: the whole list in rank order on SlotScore()'s score (stable;
        // R7 -- the slot fill reads past the top N), then the wildcards.
        // @param displayPage The page THIS tick allocates, from the pipeline
        //        snapshot (PipelineContext::displayPageIndex / displaySlotCount
        //        / displayWildcardSlots): wildcard eligibility is page-relative,
        //        so it comes from the tick's snapshot (critique #10).
        void RankAndApplyWildcards(ScoredCandidateList& scored, const WildcardPage& displayPage);

        // Configuration access
        [[nodiscard]] const ScorerConfig& GetConfig() const noexcept { return m_config; }
        void SetConfig(const ScorerConfig& config) { m_config = config; }

        // Component access (for advanced configuration)
        [[nodiscard]] WildcardManager& GetWildcardManager() noexcept { return m_wildcardMgr; }

        // R8.1 (0.23.55): OnCombatStart / OnCombatEnd are gone with the
        // potion multiplier's own combat timer; the legacy pass takes the
        // combat_onset clock instead.
        // @return true if a displayed exploration wildcard expired this tick
        //         (caller must force a pipeline run so the slot content swaps)
        [[nodiscard]] bool Update(float deltaSeconds);

        // Reset state (e.g., on save load)
        void Reset();

        // R3, read-only: magicka / stamina as the envelopes hold them (what the
        // magicka_deficit / stamina_deficit needs read, needs.csv, and the
        // legacy pass's context rules). No state change; the envelopes tick
        // in Update as before.
        [[nodiscard]] float HeldMagicka(float raw) const noexcept { return m_magickaEnvelope.Value(raw); }
        [[nodiscard]] float HeldStamina(float raw) const noexcept { return m_staminaEnvelope.Value(raw); }

        // Recommendation logging (release-available; see DebugSettings::recLogVerbosity).
        // detail: append the inputs the D2 feature was computed from, as the
        // row's logged breakdown carries them, to each line.
        // force: bypass the membership-change dedup (on-demand `hg recs` dumps).
        // fallback (R8.3): the list is the fallback ranking (no bootstrap,
        // Legacy::Run::FallbackRanking).
        void LogTopCandidates(const ScoredCandidateList& ranked, size_t count = 5,
            bool detail = false, bool force = false, bool fallback = false) const;

    private:
        ScorerConfig m_config;
        WildcardManager m_wildcardMgr;

        // The hold on magicka / stamina: the needs read it (HeldMagicka /
        // HeldStamina), and the legacy pass scores its context rules on it.
        // Update thread, like the rest of scoring.
        Context::VitalEnvelope m_magickaEnvelope;
        Context::VitalEnvelope m_staminaEnvelope;
    };

}  // namespace Huginn::Scoring
