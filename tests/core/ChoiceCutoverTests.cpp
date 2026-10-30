// Host tests for R8.3, the cutover: the THTA and BIAS cosave codec
// (core/ChoiceCosave.h: round trip, version rejection, an unknown key dropped,
// the bootstrap merge), the need signature that joins the skip gate
// (core/NeedEvaluator.h: kNever inputs, a boundary step), the ranking's order
// (core/SlotScoreMath.h RanksBefore: score first, ties keep generation order
// under std::stable_sort), the legacy switch (core/ChoiceLive.h: off gives
// constant features and a score that ignores the old learner), and the
// ranking pass (core/ChoiceLive.h: top terms, positions and kappa only after
// the allocation), the blob caps the plugin checks before a read, and the
// CriticalHealth label (display/NeedReason.h).

#include "core/Bootstrap.h"
#include "core/ChoiceCosave.h"
#include "core/ChoiceLive.h"
#include "core/ChoiceScore.h"
#include "core/NeedEvaluator.h"
#include "core/NeedSnapshot.h"
#include "core/SlotScoreMath.h"
#include "display/NeedReason.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ostream>
#include <string>
#include <vector>

using namespace Huginn::Core;
namespace CC = Huginn::Core::ChoiceCosave;
namespace C = Huginn::Core::Choice;

namespace
{
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    constexpr double kInf = std::numeric_limits<double>::infinity();

    CC::ThetaRecord SampleTheta()
    {
        CC::ThetaRecord r;
        r.createdUtc = "2026-10-29 12:29:59";
        r.sha256 = std::string(64, 'a');
        r.entries = { { 0x1111111111111111ull, 0.25, 0.09 }, { 0x2222ull, -1.5, 1.0 }, { 0xFFFFFFFFFFFFFFFFull, 3.0, 1e-6 } };
        return r;
    }

    CC::BiasRecord SampleBias()
    {
        CC::BiasRecord r;
        r.entries = { { 0x0001A2B3u, 0.5, 0.2, 3.5f, 42u, 0 }, { 0xFF000801u, -0.25, 0.25, 1.0f, 0u, CC::kBiasFavorite } };
        return r;
    }

    template <class T>
    void Poke(std::vector<std::byte>& blob, std::size_t at, T v)
    {
        REQUIRE(at + sizeof(T) <= blob.size());
        std::memcpy(blob.data() + at, &v, sizeof(T));
    }

    C::Model Blank()
    {
        C::Model m;
        m.theta.assign(C::K * C::J, C::Weight{ 0.0, 1.0 });
        m.covered = C::Weight{ 0.0, 1.0 };
        return m;
    }

    constexpr auto kHD = static_cast<std::size_t>(Needs::NeedId::health_deficit);
    constexpr auto kMD = static_cast<std::size_t>(Needs::NeedId::magicka_deficit);
    constexpr auto kRH = static_cast<std::uint16_t>(Effect::Col::restore_health);
    constexpr auto kRM = static_cast<std::uint16_t>(Effect::Col::restore_magicka);
}

// ---------------------------------------------------------------------------
// The cosave codec
// ---------------------------------------------------------------------------

TEST_CASE("cosave THTA: a round trip keeps every entry and the bootstrap identity to the bit")
{
    const auto r = SampleTheta();
    const auto blob = CC::EncodeTheta(r);
    CHECK(blob.size() == 4 + 2 + r.createdUtc.size() + 2 + r.sha256.size() + 4 + r.entries.size() * CC::kThetaStride);
    std::string why;
    const auto back = CC::DecodeTheta(blob, CC::kThetaVersion, &why);
    REQUIRE_MESSAGE(back.has_value(), why);
    CHECK(back->createdUtc == r.createdUtc);
    CHECK(back->sha256 == r.sha256);
    CHECK(back->entries == r.entries);
    CHECK(back->dropped == 0);
}

TEST_CASE("cosave BIAS: a round trip keeps formID, mu, var, n, minutes and the favourite bit")
{
    const auto r = SampleBias();
    const auto blob = CC::EncodeBias(r);
    CHECK(blob.size() == 8 + r.entries.size() * CC::kBiasStride);
    const auto back = CC::DecodeBias(blob, CC::kBiasVersion);
    REQUIRE(back.has_value());
    CHECK(back->entries == r.entries);
    CHECK((back->entries[1].flags & CC::kBiasFavorite) != 0);
}

TEST_CASE("cosave: an empty learner writes and reads back empty records")
{
    CC::ThetaRecord t;
    const auto tb = CC::DecodeTheta(CC::EncodeTheta(t), CC::kThetaVersion);
    REQUIRE(tb.has_value());
    CHECK(tb->entries.empty());
    CHECK(tb->createdUtc.empty());
    const auto bb = CC::DecodeBias(CC::EncodeBias(CC::BiasRecord{}), CC::kBiasVersion);
    REQUIRE(bb.has_value());
    CHECK(bb->entries.empty());
}

TEST_CASE("cosave: a version this build does not read is rejected, in the blob or in the record header")
{
    auto blob = CC::EncodeTheta(SampleTheta());
    std::string why;
    CHECK_FALSE(CC::DecodeTheta(blob, CC::kThetaVersion + 1, &why).has_value());   // the SKSE header says 2
    CHECK(why.find("version") != std::string::npos);
    Poke<std::uint32_t>(blob, 0, CC::kThetaVersion + 1);                             // the blob says 2
    CHECK_FALSE(CC::DecodeTheta(blob, CC::kThetaVersion + 1).has_value());
    CHECK_FALSE(CC::DecodeTheta(blob, CC::kThetaVersion).has_value());

    auto bias = CC::EncodeBias(SampleBias());
    CHECK_FALSE(CC::DecodeBias(bias, 0).has_value());
    Poke<std::uint32_t>(bias, 0, 7u);
    CHECK_FALSE(CC::DecodeBias(bias, CC::kBiasVersion).has_value());
}

TEST_CASE("cosave: a length that does not match the count, a truncated blob or a count over the cap is rejected")
{
    const auto good = CC::EncodeTheta(SampleTheta());
    auto shortBlob = good;
    shortBlob.pop_back();
    CHECK_FALSE(CC::DecodeTheta(shortBlob, CC::kThetaVersion).has_value());
    auto longBlob = good;
    longBlob.push_back(std::byte{ 0 });
    CHECK_FALSE(CC::DecodeTheta(longBlob, CC::kThetaVersion).has_value());
    CHECK_FALSE(CC::DecodeTheta(std::span<const std::byte>(good.data(), 3), CC::kThetaVersion).has_value());
    CHECK_FALSE(CC::DecodeTheta(std::span<const std::byte>{}, CC::kThetaVersion).has_value());

    // a count past the cap, whatever the bytes after it
    auto huge = CC::EncodeBias(CC::BiasRecord{});
    Poke<std::uint32_t>(huge, 4, CC::kMaxBiasEntries + 1);
    std::string why;
    CHECK_FALSE(CC::DecodeBias(huge, CC::kBiasVersion, &why).has_value());
    CHECK(why.find("cap") != std::string::npos);

    // a string length past its cap
    auto badText = good;
    Poke<std::uint16_t>(badText, 4, static_cast<std::uint16_t>(CC::kMaxText + 1));
    CHECK_FALSE(CC::DecodeTheta(badText, CC::kThetaVersion).has_value());
}

TEST_CASE("cosave: the blob caps the plugin checks before reading hold the largest valid record, and no more")
{
    // Both strings at their cap and no entries: the header part of the cap.
    CC::ThetaRecord t;
    t.createdUtc.assign(CC::kMaxText, 'u');
    t.sha256.assign(CC::kMaxText, 'f');
    const auto header = CC::EncodeTheta(t);
    CHECK(header.size() + static_cast<std::size_t>(CC::kMaxThetaEntries) * CC::kThetaStride == CC::kMaxThetaBlobBytes);
    CHECK(CC::EncodeBias(CC::BiasRecord{}).size() + static_cast<std::size_t>(CC::kMaxBiasEntries) * CC::kBiasStride ==
          CC::kMaxBiasBlobBytes);
    // A record length must fit SKSE's u32.
    static_assert(CC::kMaxThetaBlobBytes < 0xFFFFFFFFull);
    static_assert(CC::kMaxBiasBlobBytes < 0xFFFFFFFFull);
}

TEST_CASE("cosave: entries that are not finite or have var <= 0 are dropped and counted, the rest kept")
{
    CC::ThetaRecord t = SampleTheta();
    t.entries.push_back({ 0x3333ull, kNaN, 1.0 });
    t.entries.push_back({ 0x4444ull, 1.0, 0.0 });
    t.entries.push_back({ 0x5555ull, 1.0, -kInf });
    const auto back = CC::DecodeTheta(CC::EncodeTheta(t), CC::kThetaVersion);
    REQUIRE(back.has_value());
    CHECK(back->entries.size() == 3);
    CHECK(back->dropped == 3);

    CC::BiasRecord b = SampleBias();
    b.entries.push_back({ 0x0102u, 0.1, 0.0, 1.0f, 0u, 0 });                                       // var 0
    b.entries.push_back({ 0x0103u, 0.1, 0.1, std::numeric_limits<float>::infinity(), 0u, 0 });     // n inf
    b.entries.push_back({ 0u, 0.1, 0.1, 1.0f, 0u, 0 });                                            // no form
    const auto bb = CC::DecodeBias(CC::EncodeBias(b), CC::kBiasVersion);
    REQUIRE(bb.has_value());
    CHECK(bb->entries.size() == 2);
    CHECK(bb->dropped == 3);
}

TEST_CASE("cosave merge: saved keys override the bootstrap, missing keys keep the file's, unknown keys are dropped and counted")
{
    const std::vector<std::uint64_t> buildKeys{ 10, 20, 30, 40 };
    const std::vector<CC::ThetaEntry> saved{ { 20, 1.0, 0.5 }, { 99, 7.0, 1.0 }, { 40, -2.0, 0.25 }, { 98, 1.0, 1.0 } };
    CC::MergeReport rep;
    const auto merged = CC::MergeTheta(buildKeys, saved, rep);
    CHECK(rep.applied == 2);
    CHECK(rep.fromFile == 2);    // 10 and 30 keep the bootstrap's value
    CHECK(rep.unknown == 2);     // 98 and 99 this build no longer knows
    CHECK(rep.duplicate == 0);
    REQUIRE(merged.size() == 2);
    CHECK(merged[0] == CC::ThetaEntry{ 20, 1.0, 0.5 });   // in the build's key order
    CHECK(merged[1] == CC::ThetaEntry{ 40, -2.0, 0.25 });
}

TEST_CASE("cosave merge: a key saved twice keeps the last value; an empty save keeps every file value")
{
    const std::vector<std::uint64_t> buildKeys{ 1, 2 };
    CC::MergeReport rep;
    const auto merged = CC::MergeTheta(buildKeys, std::vector<CC::ThetaEntry>{ { 1, 0.1, 1.0 }, { 1, 0.9, 0.5 } }, rep);
    CHECK(rep.duplicate == 1);
    CHECK(rep.applied == 1);
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].mu == 0.9);
    const auto none = CC::MergeTheta(buildKeys, std::vector<CC::ThetaEntry>{}, rep);
    CHECK(none.empty());
    CHECK(rep.fromFile == 2);
    CHECK(rep.unknown == 0);
}

TEST_CASE("cosave merge: a save from before the alcohol column (0.24.8) keeps its weights; the new column takes the file's")
{
    // THTA is keyed by name (Bootstrap::StableKey), never by position: inserting
    // alcohol after kind_light moved every later column's index, not its key.
    using Bootstrap::StableKey;
    const std::string_view need = Needs::kNeeds[0].id;
    std::vector<std::uint64_t> buildKeys;
    std::vector<CC::ThetaEntry> saved;   // what a 0.24.8 save holds: every column but alcohol
    for (std::size_t j = 0; j < C::J; ++j) {
        const auto key = StableKey("theta", need, Effect::kColumns[j].id);
        buildKeys.push_back(key);
        if (Effect::kColumns[j].id != "alcohol") saved.push_back(CC::ThetaEntry{ key, 0.25 + 0.001 * j, 0.5 });
    }
    CC::MergeReport rep;
    const auto merged = CC::MergeTheta(buildKeys, saved, rep);
    CHECK(rep.applied == C::J - 1);
    CHECK(rep.fromFile == 1);    // alcohol: the bootstrap's value (the prior: the file has no such column)
    CHECK(rep.unknown == 0);
    REQUIRE(merged.size() == C::J - 1);
    const auto alcoholKey = StableKey("theta", need, "alcohol");
    CHECK(std::none_of(merged.begin(), merged.end(), [&](const CC::ThetaEntry& e) { return e.key == alcoholKey; }));
    // school_alteration moved from index 192 to 193; its saved weight still lands on it.
    const auto school = StableKey("theta", need, "school_alteration");
    const auto at = std::find_if(merged.begin(), merged.end(), [&](const CC::ThetaEntry& e) { return e.key == school; });
    REQUIRE(at != merged.end());
    CHECK(at->mu == doctest::Approx(0.25 + 0.001 * static_cast<double>(Effect::Index(Effect::Col::school_alteration))));
}

// ---------------------------------------------------------------------------
// The need signature (the skip gate's key)
// ---------------------------------------------------------------------------

TEST_CASE("need signature: kNever inputs read their settled curve value, and the hash is stable across calls")
{
    // Every seconds-ago timer at kNever (the default snapshot): the decays sit
    // at 0, so nothing time-driven is left and the signature has settled.
    Needs::NeedSnapshot s;
    const auto curves = Needs::DefaultCurves();
    const auto v = Needs::EvaluateNeeds(s, curves);
    const auto sig = Needs::Signature(v.value);
    CHECK(sig[static_cast<std::size_t>(Needs::NeedId::enemy_casting_fire)] == 0);
    CHECK(sig[static_cast<std::size_t>(Needs::NeedId::combat_onset)] == 0);
    CHECK_FALSE(Needs::TimeDriven(s, curves));
    // Advance a kNever snapshot by kNever again: the same signature.
    const auto later = Needs::Signature(Needs::EvaluateNeeds(Needs::Advance(s, Needs::kNever), curves).value);
    CHECK(Needs::SignatureHash(later) == Needs::SignatureHash(sig));
    CHECK(Needs::SignatureHash(sig) == Needs::SignatureHash(Needs::Signature(v.value)));
}

TEST_CASE("need signature: one 0.05 step on one need changes the hash; a wobble inside a step does not")
{
    Needs::NeedArray a{};
    a[kHD] = 0.0249f;   // level 0
    Needs::NeedArray b = a;
    b[kHD] = 0.0251f;   // level 1: the step boundary at 0.025
    CHECK(Needs::SignatureLevel(0.0249f) == 0);
    CHECK(Needs::SignatureLevel(0.0251f) == 1);
    CHECK(Needs::SignatureHash(Needs::Signature(a)) != Needs::SignatureHash(Needs::Signature(b)));

    Needs::NeedArray c = a;
    c[kHD] = 0.01f;     // still level 0
    CHECK(Needs::SignatureHash(Needs::Signature(a)) == Needs::SignatureHash(Needs::Signature(c)));

    // the same step on a different need is a different key
    Needs::NeedArray d = a;
    d[kMD] = 0.0251f;
    CHECK(Needs::SignatureHash(Needs::Signature(b)) != Needs::SignatureHash(Needs::Signature(d)));

    // NaN and negatives read level 0 (no step, no fault)
    Needs::NeedArray e = a;
    e[kHD] = std::numeric_limits<float>::quiet_NaN();
    CHECK(Needs::SignatureHash(Needs::Signature(e)) == Needs::SignatureHash(Needs::Signature(a)));
}

// ---------------------------------------------------------------------------
// The ranking's order (ScoredCandidate::operator<)
// ---------------------------------------------------------------------------

TEST_CASE("rank order: higher score first, any sign; exact ties keep generation order under stable_sort")
{
    struct Row
    {
        double score;
        float dps;
        int gen;
    };
    std::vector<Row> rows{
        { -2.0, 0.0f, 0 }, { 0.5, 0.0f, 1 }, { -2.0, 0.0f, 2 }, { -0.1, 0.0f, 3 },
        { 0.5, 0.0f, 4 }, { -2.0, 0.0f, 5 }, { -kInf, 0.0f, 6 }, { 0.5, 12.0f, 7 },
    };
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row& a, const Row& b) { return RanksBefore(a.score, a.dps, b.score, b.dps); });
    std::vector<int> order;
    for (const auto& r : rows) order.push_back(r.gen);
    // 0.5 with the weapon DPS first, then the 0.5 ties in generation order,
    // -0.1, the -2 ties in generation order, -inf last
    CHECK(order == std::vector<int>{ 7, 1, 4, 3, 0, 2, 5, 6 });
}

TEST_CASE("rank order: a NaN score ranks as -inf, so the comparator stays a strict weak order")
{
    CHECK(RanksBefore(-5.0, 0.0f, kNaN, 0.0f));
    CHECK_FALSE(RanksBefore(kNaN, 0.0f, -5.0, 0.0f));
    CHECK_FALSE(RanksBefore(kNaN, 0.0f, kNaN, 0.0f));          // irreflexive
    CHECK_FALSE(RanksBefore(kNaN, 0.0f, -kInf, 0.0f));         // NaN == -inf: a tie
    CHECK_FALSE(RanksBefore(-kInf, 0.0f, kNaN, 0.0f));
    CHECK(RanksBefore(kNaN, 1.0f, -kInf, 0.0f));               // ... broken by the DPS
}

// ---------------------------------------------------------------------------
// The legacy switch and the ranking pass
// ---------------------------------------------------------------------------

TEST_CASE("legacy switch: on, a kept row reads its ln learn mult and util missing 0; off, every row reads 0 and 1")
{
    CHECK(C::LegacyFeature(true, true, 0.75).lnLearnMult == 0.75);
    CHECK(C::LegacyFeature(true, true, 0.75).utilMissing == 0.0);
    CHECK(C::LegacyFeature(true, false, 0.75).lnLearnMult == 0.0);   // under the floors: no feature
    CHECK(C::LegacyFeature(true, false, 0.75).utilMissing == 1.0);
    for (const bool scored : { true, false }) {
        for (const double lm : { -3.0, 0.0, 0.4, 2.5 }) {
            const auto v = C::LegacyFeature(false, scored, lm);
            CHECK(v.lnLearnMult == 0.0);
            CHECK(v.utilMissing == 1.0);
        }
    }
}

TEST_CASE("legacy switch off: the ranking ignores the old learner -- rows that differ only in it score alike")
{
    C::Model m = Blank();
    m.w[static_cast<std::size_t>(C::Feature::ln_learn_mult)] = 1.36;
    m.w[static_cast<std::size_t>(C::Feature::util_missing)] = 0.107;
    m.Th(kHD, kRH) = { 2.0, 0.5 };
    std::array<double, C::K> n{};
    n[kHD] = 0.7;
    const C::Tick t = C::MakeTick(m, n, {});
    const std::vector<C::Attr> attrs{ C::MakeAttr(m.grading, std::vector<C::SparseCol>{ { kRH, 0.5 } }, {}),
                                      C::MakeAttr(m.grading, std::vector<C::SparseCol>{ { kRH, 0.5 } }, {}),
                                      C::MakeAttr(m.grading, std::vector<C::SparseCol>{ { kRH, 0.5 } }, {}) };
    const struct { bool scored; double lm; } legacy[] = { { true, 1.2 }, { true, -0.4 }, { false, 0.0 } };
    for (const bool on : { true, false }) {
        std::vector<C::RowExtra> x(3);
        for (std::size_t i = 0; i < 3; ++i) C::SetLegacy(x[i].feat, C::LegacyFeature(on, legacy[i].scored, legacy[i].lm));
        std::vector<C::LiveScore> out;
        C::ScoreForRanking(m, t, attrs, x, std::vector<C::GateRow>(3), SIZE_MAX, 1.0, 3, out);
        REQUIRE(out.size() == 3);
        const double need = 0.7 * 2.0 * 0.5;
        if (on) {
            CHECK(out[0].s.mu == doctest::Approx(need + 1.36 * 1.2));
            CHECK(out[1].s.mu == doctest::Approx(need + 1.36 * -0.4));
            CHECK(out[2].s.mu == doctest::Approx(need + 0.107));
        }
        else {
            CHECK(out[0].s.mu == doctest::Approx(need + 0.107));
            CHECK(out[1].s.mu == out[0].s.mu);
            CHECK(out[2].s.mu == out[0].s.mu);
        }
    }
}

TEST_CASE("ranking pass: the top three terms descend, the first is Explain's pair, and needMu is the need part")
{
    C::Model m = Blank();
    m.Th(kHD, kRH) = { 2.0, 0.5 };
    m.Th(kMD, kRM) = { 3.0, 0.5 };
    m.Th(kHD, kRM) = { 0.5, 0.5 };
    m.Th(kMD, kRH) = { -1.0, 0.5 };   // a negative term is never listed
    std::array<double, C::K> n{};
    n[kHD] = 0.8;
    n[kMD] = 0.5;
    const C::Tick t = C::MakeTick(m, n, {});
    const C::Attr at = C::MakeAttr(m.grading, std::vector<C::SparseCol>{ { kRH, 0.6 }, { kRM, 0.4 } }, {});
    const auto top = C::Top3(m, t, at);
    // terms: HD x RH 0.96, MD x RM 0.6, HD x RM 0.16, MD x RH -0.3
    REQUIRE(top[0].Valid());
    CHECK(top[0].need == static_cast<std::int16_t>(kHD));
    CHECK(top[0].col == static_cast<std::int16_t>(kRH));
    CHECK(top[0].value == doctest::Approx(0.96));
    CHECK(top[1].value == doctest::Approx(0.6));
    CHECK(top[2].value == doctest::Approx(0.16));
    C::RowScore ex;
    C::Explain(m, t, at, ex);
    CHECK(ex.exNeed == top[0].need);
    CHECK(ex.exCol == top[0].col);

    std::vector<C::RowExtra> x(1);
    std::vector<C::LiveScore> out;
    C::ScoreForRanking(m, t, std::vector<C::Attr>{ at }, x, std::vector<C::GateRow>(1), SIZE_MAX, 1.0, 1, out);
    CHECK(out[0].needMu == doctest::Approx(0.96 + 0.6 + 0.16 - 0.3));
    CHECK(out[0].s.exNeed == top[0].need);
}

TEST_CASE("ranking pass: tied terms keep the lower (need, column), as Explain; no positive term is 'learned'")
{
    C::Model m = Blank();
    m.Th(kHD, kRH) = { 1.0, 0.5 };
    m.Th(kMD, kRH) = { 1.0, 0.5 };
    std::array<double, C::K> n{};
    n[kHD] = 0.5;
    n[kMD] = 0.5;
    const C::Tick t = C::MakeTick(m, n, {});
    const C::Attr at = C::MakeAttr(m.grading, std::vector<C::SparseCol>{ { kRH, 1.0 } }, {});
    const auto top = C::Top3(m, t, at);
    CHECK(top[0].need == static_cast<std::int16_t>(std::min(kHD, kMD)));
    CHECK(top[1].need == static_cast<std::int16_t>(std::max(kHD, kMD)));
    CHECK_FALSE(top[2].Valid());

    const C::Attr none = C::MakeAttr(m.grading, std::vector<C::SparseCol>{ { kRM, 1.0 } }, {});
    CHECK_FALSE(C::Top3(m, t, none)[0].Valid());
}

TEST_CASE("ranking pass: positions and kappa enter only after the allocation; a NaN mu ranks as -inf")
{
    C::Model m = Blank();
    m.kappa = 2.5;
    m.w[static_cast<std::size_t>(C::Feature::slot0) + 3] = 0.4;
    CHECK(C::WeighsSlots(m));
    const auto onKey3 = C::AfterAllocation(m, 1.0, false, 3);
    CHECK(onKey3.mu == doctest::Approx(1.4));
    CHECK(onKey3.logit == doctest::Approx(1.4));
    const auto offPage = C::AfterAllocation(m, 1.0, true, 3);   // off the page: no slot feature, kappa
    CHECK(offPage.mu == doctest::Approx(1.0));
    CHECK(offPage.logit == doctest::Approx(1.0 - 2.5));
    CHECK(C::AfterAllocation(m, 1.0, false, 9).mu == doctest::Approx(1.0));   // past key 7

    C::Model nanModel = Blank();
    nanModel.w[static_cast<std::size_t>(C::Feature::ln_learn_mult)] = 1.0;
    std::vector<C::RowExtra> x(1);
    x[0].feat[static_cast<std::size_t>(C::Feature::ln_learn_mult)] = kNaN;
    std::vector<C::LiveScore> out;
    const C::Tick t = C::MakeTick(nanModel, {}, {});
    C::ScoreForRanking(nanModel, t, std::vector<C::Attr>(1), x, std::vector<C::GateRow>(1), SIZE_MAX, 1.0, 1, out);
    CHECK(out[0].s.mu == -kInf);
}

TEST_CASE("need reason: health_deficit names LowHealth while on and CriticalHealth from kCriticalHealthNeed")
{
    using R = Huginn::Context::ContextReason;
    const auto hd = static_cast<std::int16_t>(Needs::Index(Needs::NeedId::health_deficit));
    CHECK(Huginn::Display::ExplainedReason(hd, 0.49) == R::None);
    CHECK(Huginn::Display::ExplainedReason(hd, 0.5) == R::LowHealth);
    CHECK(Huginn::Display::ExplainedReason(hd, 0.969) == R::LowHealth);
    CHECK(Huginn::Display::ExplainedReason(hd, Huginn::Display::kCriticalHealthNeed) == R::CriticalHealth);
    CHECK(Huginn::Display::ExplainedReason(hd, 1.0) == R::CriticalHealth);
    // The default curve puts the old 15% health line just past the threshold.
    const auto atCritical = Needs::Evaluate(Needs::Info(Needs::NeedId::health_deficit).curve, 0.85f);
    CHECK(atCritical >= Huginn::Display::kCriticalHealthNeed);
    CHECK(Needs::Evaluate(Needs::Info(Needs::NeedId::health_deficit).curve, 0.8f) < Huginn::Display::kCriticalHealthNeed);
    // Only the deficit: a fast drop or a drain stays LowHealth however high.
    const auto hf = static_cast<std::int16_t>(Needs::Index(Needs::NeedId::health_falling));
    CHECK(Huginn::Display::ExplainedReason(hf, 1.0) == R::LowHealth);
    CHECK(Huginn::Display::ExplainedReason(-1, 1.0) == R::None);
}
