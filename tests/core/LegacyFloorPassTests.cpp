// R8.2: the old pass's floor loop and cold-start rescue moved from
// UtilityScorer::ScoreCandidates into learning/legacy/LegacyLearnFeature.h
// (FloorPass) "unchanged in arithmetic". This differential test keeps a
// verbatim copy of the R8.1 loop (UtilityScorer.cpp at 1c98f24, the main loop
// and the cold start, with the game's types replaced by plain stand-ins and
// the scorer's own ScoreCandidateInternal / ComputeUtility by one shared
// function) and checks that FloorPass builds the same list -- same rows, same
// order, bit-identical utilities and cold-start flags -- over many random
// candidate pools and floor settings, and that the per-candidate floors it
// reports are the ones PipelineCoordinator::CaptureEligible derives on its own
// (scored: pass or cold; else ctx when under the context floor and not a
// favourite, else util). It also pins the D2 feature: ln(max(1 + lambda learn,
// 1e-6)) on pass / cold rows, util missing on the others, and the switch.
//
// Header-only and host-compilable: FloorPass is a template over the candidate
// and scored types (src/Profiling.h's zones are no-ops without Tracy).

#include "learning/legacy/LegacyLearnFeature.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <ostream>
#include <random>
#include <vector>

namespace
{
    using Huginn::Core::DecisionLog::Floor;
    namespace L = Huginn::Scoring::Legacy;

    struct Metrics
    {
        float rewardEstimate = 0.0f;
        float ucb = 0.0f;
        float confidence = 0.0f;
    };

    struct Cand
    {
        std::uint32_t form = 0;
        bool fav = false;
        bool gated = false;
        float ctx = 0.0f;
        Metrics m;
        float rec = 0.0f;
        float prior = 0.0f;
        float corr = 1.0f, potion = 1.0f, favMult = 1.0f;
    };

    struct Breakdown
    {
        float contextWeight = 0.0f, rewardEstimate = 0.0f, prior = 0.0f, ucb = 0.0f, confidence = 0.0f;
        float learningScore = 0.0f, lambda = 0.0f, recencyBoost = 0.0f;
        float correlationBonus = 1.0f, potionMultiplier = 1.0f, favoritesMultiplier = 1.0f;
    };

    struct Scored
    {
        Breakdown breakdown;
        float utility = 0.0f;
        bool isColdStartBoosted = false;
        std::uint32_t form = 0;
        [[nodiscard]] std::uint32_t GetFormID() const { return form; }
    };

    struct Config
    {
        float minimumContextWeight = 0.05f;
        float minimumUtility = 0.1f;
        std::size_t topNCandidates = 10;
        float coldStartUCBBoost = 0.2f;
        float explorationWeight = 0.2f, lambdaMin = 0.5f, lambdaMax = 3.0f;
    };

    // The scorer's arithmetic (ComputeAdaptiveLambda, ScoreCandidateInternal,
    // ComputeUtility), shared by both paths.
    float Lambda(const Config& c, float conf) { return c.lambdaMin + conf * (c.lambdaMax - c.lambdaMin); }
    float Utility(const Config& c, const Breakdown& b)
    {
        const float lambda = Lambda(c, b.confidence);
        return b.contextWeight * (1.0f + lambda * b.learningScore) * b.correlationBonus * b.potionMultiplier *
               b.favoritesMultiplier;
    }
    Scored Internal(const Config& c, const Cand& cand, float contextWeight, const Metrics& m, float rec)
    {
        Scored r;
        r.form = cand.form;
        r.breakdown.contextWeight = contextWeight;
        r.breakdown.rewardEstimate = m.rewardEstimate;
        r.breakdown.ucb = m.ucb;
        r.breakdown.confidence = m.confidence;
        r.breakdown.prior = cand.prior;
        const float alpha = r.breakdown.confidence;
        r.breakdown.learningScore = alpha * r.breakdown.rewardEstimate + (1.0f - alpha) * r.breakdown.prior +
                                    c.explorationWeight * r.breakdown.ucb;
        r.breakdown.lambda = Lambda(c, alpha);
        r.breakdown.recencyBoost = rec;
        r.breakdown.learningScore += rec;
        r.breakdown.correlationBonus = cand.corr;
        r.breakdown.potionMultiplier = cand.potion;
        r.breakdown.favoritesMultiplier = cand.favMult;
        r.utility = Utility(c, r.breakdown);
        return r;
    }

    // ---- R8.1's loop, verbatim but for the stand-ins (UtilityScorer.cpp @ 1c98f24)
    std::vector<Scored> OldPass(const std::vector<Cand>& candidates, const Config& m_config)
    {
        std::vector<Scored> scored;
        float maxUtilitySeen = 0.0f;
        for (const auto& candidate : candidates) {
            const bool favorited = candidate.fav;
            float contextWeight = candidate.ctx;
            if (contextWeight < m_config.minimumContextWeight && !favorited) {
                continue;
            }
            std::uint32_t formID = candidate.form;
            const auto metrics = candidate.m;
            (void)formID;
            float recencyBoost = candidate.rec;
            Scored result = Internal(m_config, candidate, contextWeight, metrics, recencyBoost);
            maxUtilitySeen = std::max(maxUtilitySeen, result.utility);
            if (result.utility >= m_config.minimumUtility) {
                scored.push_back(std::move(result));
            }
        }
        if (scored.size() < m_config.topNCandidates && m_config.coldStartUCBBoost > 0.0f) {
            const auto alreadyScored = [&scored](std::uint32_t id) {
                for (const auto& s : scored) {
                    if (s.GetFormID() == id) return true;
                }
                return false;
            };
            std::size_t coldStartCount = 0;
            for (const auto& candidate : candidates) {
                std::uint32_t formID = candidate.form;
                if (alreadyScored(formID)) continue;
                float contextWeight = candidate.ctx;
                if (candidate.gated && contextWeight < m_config.minimumContextWeight) {
                    continue;
                }
                auto metrics = candidate.m;
                float boostedContext = std::max(contextWeight, m_config.coldStartUCBBoost * metrics.ucb);
                if (boostedContext < m_config.minimumContextWeight) continue;
                float recencyBoost = candidate.rec;
                Scored result = Internal(m_config, candidate, contextWeight, metrics, recencyBoost);
                result.breakdown.contextWeight = boostedContext;
                result.utility = Utility(m_config, result.breakdown);
                if (result.utility >= m_config.minimumUtility) {
                    result.isColdStartBoosted = true;
                    scored.push_back(std::move(result));
                    ++coldStartCount;
                }
                if (scored.size() >= m_config.topNCandidates) break;
            }
            (void)coldStartCount;
        }
        (void)maxUtilitySeen;
        return scored;
    }

    struct Hooks
    {
        const Config& c;
        bool Favorited(const Cand& x) const { return x.fav; }
        float ContextWeight(const Cand& x) const { return x.ctx; }
        std::uint32_t FormID(const Cand& x) const { return x.form; }
        // the stand-in reads metrics off the candidate: the hook gets the form only
        const std::vector<Cand>* pool = nullptr;
        Metrics LearnerMetrics(std::uint32_t id) const
        {
            for (const auto& x : *pool) {
                if (x.form == id) return x.m;
            }
            return {};
        }
        float Recency(std::uint32_t id) const
        {
            for (const auto& x : *pool) {
                if (x.form == id) return x.rec;
            }
            return 0.0f;
        }
        Scored Score(const Cand& x, float ctx, const Metrics& m, float rec) const { return Internal(c, x, ctx, m, rec); }
        float Utility(const Breakdown& b) const { return ::Utility(c, b); }
        bool HardGated(const Cand& x) const { return x.gated; }
        float Ucb(const Metrics& m) const { return m.ucb; }
    };
}

TEST_CASE("legacy floor pass: the moved loop builds R8.1's list bit for bit, and reports CaptureEligible's floors")
{
    std::mt19937 rng(20261029);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    std::size_t pools = 0, coldRuns = 0, coldRows = 0, rowsChecked = 0;
    for (int trial = 0; trial < 3000; ++trial) {
        Config c;
        c.minimumContextWeight = 0.02f + 0.1f * u01(rng);
        c.minimumUtility = 0.05f + 0.3f * u01(rng);
        c.topNCandidates = 1 + static_cast<std::size_t>(rng() % 15);
        c.coldStartUCBBoost = (rng() % 5 == 0) ? 0.0f : 0.5f * u01(rng);
        std::vector<Cand> pool;
        const int n = static_cast<int>(rng() % 40);
        for (int i = 0; i < n; ++i) {
            Cand x;
            // forms repeat now and then (weapon stacks share a FormID): the
            // cold start's FormID dedup must behave the same
            x.form = 0x1000u + static_cast<std::uint32_t>(rng() % 30);
            x.fav = rng() % 7 == 0;
            x.gated = rng() % 6 == 0;
            x.ctx = (rng() % 3 == 0) ? 0.08f * u01(rng) : u01(rng);
            x.m.rewardEstimate = 2.0f * u01(rng) - 1.0f;
            x.m.confidence = u01(rng);
            x.m.ucb = 1.5f * u01(rng);
            x.rec = (rng() % 4 == 0) ? 0.5f * u01(rng) : 0.0f;
            x.prior = u01(rng);
            x.corr = 0.5f + u01(rng);
            x.potion = (rng() % 5 == 0) ? 1.5f : 1.0f;
            x.favMult = x.fav ? 2.5f : 1.0f;
            pool.push_back(x);
        }
        // the stand-in hooks look metrics up by form: keep one set per form
        for (auto& x : pool) {
            for (const auto& y : pool) {
                if (y.form == x.form) {
                    x.m = y.m;
                    x.rec = y.rec;
                    break;
                }
            }
        }
        const auto expect = OldPass(pool, c);
        std::vector<Scored> got;
        std::vector<L::Row> rows;
        Hooks h{ c, &pool };
        const L::FloorParams p{ c.minimumContextWeight, c.minimumUtility, c.topNCandidates, c.coldStartUCBBoost };
        const auto res = L::FloorPass(pool, p, h, got, &rows);
        ++pools;
        coldRuns += res.coldStartRan ? 1 : 0;
        coldRows += res.coldStartCount;
        REQUIRE(got.size() == expect.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK(got[i].form == expect[i].form);
            CHECK(std::bit_cast<std::uint32_t>(got[i].utility) == std::bit_cast<std::uint32_t>(expect[i].utility));
            CHECK(got[i].isColdStartBoosted == expect[i].isColdStartBoosted);
            CHECK(std::bit_cast<std::uint32_t>(got[i].breakdown.contextWeight) ==
                  std::bit_cast<std::uint32_t>(expect[i].breakdown.contextWeight));
        }
        // the floors, as CaptureEligible derives them
        REQUIRE(rows.size() == pool.size());
        std::vector<bool> taken(got.size(), false);
        for (std::size_t i = 0; i < pool.size(); ++i) {
            const auto& r = rows[i];
            ++rowsChecked;
            if (r.Scored()) {
                CHECK_FALSE(r.utilMissing);
                // it is one of the kept rows (by form and the flag)
                bool found = false;
                for (std::size_t k = 0; k < got.size() && !found; ++k) {
                    if (taken[k] || got[k].form != pool[i].form) continue;
                    if (got[k].isColdStartBoosted != (r.floor == Floor::cold)) continue;
                    taken[k] = found = true;
                    CHECK(r.lnLearnMult == L::LnLearnMult(got[k].breakdown.lambda, got[k].breakdown.learningScore));
                }
                CHECK(found);
            }
            else {
                CHECK(r.utilMissing);
                CHECK(r.lnLearnMult == 0.0f);
                const Floor f = (pool[i].ctx < c.minimumContextWeight && !pool[i].fav) ? Floor::ctx : Floor::util;
                CHECK(r.floor == f);
            }
        }
        CHECK(std::all_of(taken.begin(), taken.end(), [](bool t) { return t; }));
    }
    MESSAGE("floor pass: " << pools << " pools, " << rowsChecked << " candidates, cold start in " << coldRuns
                           << " (" << coldRows << " rows rescued)");
    CHECK(coldRows > 0);
}

TEST_CASE("legacy feature: ln learn mult, util missing and the switch")
{
    CHECK(L::LnLearnMult(0.5f, 0.0f) == 0.0f);
    CHECK(L::LnLearnMult(2.0f, 0.5f) == doctest::Approx(std::log(2.0)));
    CHECK(L::LnLearnMult(3.0f, -1.0f) == doctest::Approx(std::log(1e-6)));   // floored at 1e-6
    L::Row pass{ Floor::pass, 0.75f, false };
    L::Row cold{ Floor::cold, 0.25f, false };
    L::Row ctx{ Floor::ctx, 0.0f, true };
    CHECK(L::Feature(pass, true).lnLearnMult == 0.75);
    CHECK(L::Feature(pass, true).utilMissing == 0.0);
    CHECK(L::Feature(cold, true).lnLearnMult == 0.25);
    CHECK(L::Feature(ctx, true).lnLearnMult == 0.0);
    CHECK(L::Feature(ctx, true).utilMissing == 1.0);
    // off: 0 and 1 on every row (plain theta + b + recent)
    CHECK(L::Feature(pass, false).lnLearnMult == 0.0);
    CHECK(L::Feature(pass, false).utilMissing == 1.0);
}
