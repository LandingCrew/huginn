// Host tests for core/Bootstrap.h: the R8 bootstrap reader (format version 1,
// docs/architecture/9-bootstrap-format.md). The golden fixtures are the ones
// tools/fit/test_fit.py writes and validates; each reader rule gets a negative
// case here, and the name mapping's prior fill is checked pair by pair.

#include "core/Bootstrap.h"
#include "core/ObviousPairs.h"
#include "core/Sha256.h"

#include <doctest/doctest.h>

#include <ostream>

#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

using namespace Huginn::Core;
namespace C = Huginn::Core::Choice;

namespace
{
    std::string ReadFile(const std::string& rel)
    {
        std::ifstream in(std::string(HUGINN_REPO_ROOT) + "/" + rel, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "cannot open " << rel);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::string Minimal() { return ReadFile("tests/core/fixtures/bootstrap/bootstrap_v1_minimal.json"); }
    std::string Full() { return ReadFile("tests/core/fixtures/bootstrap/bootstrap_v1_full.json"); }

    std::string Replace(std::string s, std::string_view from, std::string_view to)
    {
        const auto at = s.find(from);
        REQUIRE_MESSAGE(at != std::string::npos, "fixture lacks: " << from);
        s.replace(at, from.size(), to);
        return s;
    }

    bool Rejected(const std::string& text, std::string_view expect)
    {
        const auto r = Bootstrap::Read(text);
        if (r.Ok()) return false;
        for (const auto& e : r.errors) {
            if (e.find(expect) != std::string::npos) return true;
        }
        std::string all;
        for (const auto& e : r.errors) all += e + "; ";
        FAIL_CHECK("rejected, but not for '" << expect << "': " << all);
        return true;
    }

    std::size_t Need(Needs::NeedId id) { return static_cast<std::size_t>(id); }
    std::size_t Col(Effect::Col c) { return static_cast<std::size_t>(c); }
}

TEST_CASE("bootstrap: the golden fixtures read and map")
{
    SUBCASE("minimal (graded, picks)")
    {
        const auto r = Bootstrap::Read(Minimal());
        REQUIRE_MESSAGE(r.Ok(), (r.errors.empty() ? "" : r.errors[0]));
        const auto& d = *r.doc;
        CHECK(d.createdUtc == "2026-10-28 12:00:00");
        CHECK(d.SourceCharId() == 1);
        CHECK(d.outcomes == "picks");
        CHECK(d.grading == C::Grading::Graded);
        CHECK(d.kappa == 2.5);
        CHECK(!d.hasRecent);
        CHECK(d.items.empty());
        const auto m = Bootstrap::MapToBuild(d);
        CHECK(m.report.needsMapped == 2);
        CHECK(m.report.colsMapped == 2);
        CHECK(m.report.needsPrior == Needs::kNeedCount - 2);
        CHECK(m.report.colsPrior == Effect::kColumnCount - 2);
        const auto hd = Need(Needs::NeedId::health_deficit);
        const auto enc = Need(Needs::NeedId::encumbrance);
        CHECK(m.model.Th(hd, Col(Effect::Col::restore_health)).mu == 1.0);
        CHECK(m.model.Th(hd, Col(Effect::Col::restore_health)).var == doctest::Approx(1.0 / 1.5));
        CHECK(m.model.Th(enc, Col(Effect::Col::utility_carry_weight)).var == doctest::Approx(0.8));
        CHECK(m.model.Th(hd, Col(Effect::Col::utility_carry_weight)).mu == 0.0);
        CHECK(m.model.Th(hd, Col(Effect::Col::utility_carry_weight)).var == doctest::Approx(1.0 / 11.1111));
        CHECK(m.model.kappa == 2.5);
        CHECK(m.model.recent.empty());
        CHECK(m.model.strength.empty());
        CHECK(!m.model.three);
    }
    SUBCASE("full (presence+strength, three outcomes, recent, features, items, a curve)")
    {
        const auto r = Bootstrap::Read(Full());
        REQUIRE_MESSAGE(r.Ok(), (r.errors.empty() ? "" : r.errors[0]));
        const auto& d = *r.doc;
        CHECK(d.grading == C::Grading::PresenceStrength);
        CHECK(d.hasU0);
        CHECK(d.u0Bias == -1.5);
        CHECK(d.items.size() == 3);
        CHECK(d.items[1].localId == 0x03EAF3);
        CHECK(d.items[1].picks == 12);
        CHECK(d.curves.size() == 1);
        const auto m = Bootstrap::MapToBuild(d);
        const auto hd = Need(Needs::NeedId::health_deficit);
        const auto hunger = Need(Needs::NeedId::hunger);
        CHECK(m.model.three);
        CHECK(m.model.u0Bias == -1.5);
        CHECK(m.model.u0Need[hunger] == -0.5);
        REQUIRE(m.model.strength.size() == C::K * C::J);
        CHECK(m.model.strength[hd * C::J + Col(Effect::Col::restore_health)].mu == 0.5);
        CHECK(m.model.strength[hd * C::J + Col(Effect::Col::restore_health)].var == doctest::Approx(1.0 / 15.0));
        CHECK(m.model.Th(hd, Col(Effect::Col::summon)).mu == -0.125);
        CHECK(m.model.w[static_cast<std::size_t>(C::Feature::ln_util)] == 0.875);
        CHECK(m.model.w[static_cast<std::size_t>(C::Feature::util_missing)] == -2.25);
        CHECK(m.model.w[static_cast<std::size_t>(C::Feature::ln_learn_mult)] == 0.0);
        // recent: the file's two rows, then every build family at both taus at the prior
        const auto fams = Bootstrap::RecentFamilyColumns();
        CHECK(m.report.recentMapped == 2);
        CHECK(m.report.recentPrior == 2 * fams.size() - 2);
        REQUIRE(m.model.recent.size() == 2 * fams.size());
        CHECK(m.model.recent[0].col == Col(Effect::Col::summon));
        CHECK(m.model.recent[0].tau == 5.0);
        CHECK(m.model.recent[0].theta[Col(Effect::Col::summon)].mu == -0.75);
        CHECK(m.model.recent[0].theta[Col(Effect::Col::summon)].var == doctest::Approx(1.0 / 14.0));
        CHECK(m.model.recent[0].strength[Col(Effect::Col::summon)].mu == 0.125);
        CHECK(m.model.recent[0].u0 == 0.0625);
        CHECK(m.model.recent[1].theta[Col(Effect::Col::restore_health)].mu == 0.25);
        CHECK(m.model.recent.back().theta[0].mu == 0.0);
        CHECK(m.model.recent.back().theta[0].var == doctest::Approx(0.09));
        CHECK(m.model.window == 30.0);
        REQUIRE(m.curves.size() == 1);
        CHECK(m.curves[0].need == Need(Needs::NeedId::encumbrance));
        CHECK(m.curves[0].centre == 0.9);
        CHECK(m.curves[0].slope == 40.0);
    }
}

TEST_CASE("bootstrap: the shipped file reads and maps by name")
{
    const auto text = ReadFile("configs/Huginn_Bootstrap.json");
    const auto r = Bootstrap::Read(text);
    REQUIRE_MESSAGE(r.Ok(), (r.errors.empty() ? "" : r.errors[0]));
    const auto& d = *r.doc;
    CHECK(d.spec == "theta + b + recent + learn mult (D2)");
    CHECK(d.SourceCharId() == 0xFEFD1B039AC78799ull);
    CHECK(d.items.size() == 459);
    const auto m = Bootstrap::MapToBuild(d);
    CHECK(m.report.needsUnknown.empty());
    CHECK(m.report.colsUnknown.empty());
    // the file predates three of the build's needs (0.23.50-0.23.51): they take the prior
    CHECK(m.report.needsPrior == 3);
    CHECK(m.report.colsPrior == 0);
    CHECK(m.report.featuresWeighted == 2);
    CHECK(m.model.w[static_cast<std::size_t>(C::Feature::ln_learn_mult)] == 1.36013);
    CHECK(m.model.w[static_cast<std::size_t>(C::Feature::util_missing)] == 0.107469);
    CHECK(m.model.kappa == 2.69351);
    // the file's columns are not in the build's order: mapped by name
    // (the file's col 25 is restore_health, the build's 25 is self_harm)
    CHECK(d.cols[25] == "restore_health");
    const auto hd = Need(Needs::NeedId::health_deficit);
    const auto fileNeed = static_cast<std::size_t>(std::find(d.needs.begin(), d.needs.end(), "health_deficit") - d.needs.begin());
    CHECK(m.model.Th(hd, Col(Effect::Col::restore_health)).mu == d.theta[fileNeed * d.cols.size() + 25]);
    // a need the file lacks: its whole row at the prior
    const auto qs = Need(Needs::NeedId::loadout_quarterstaff);
    for (std::size_t j = 0; j < C::J; ++j) {
        const auto p = Obvious::PriorOf(Needs::NeedId::loadout_quarterstaff, static_cast<Effect::Col>(j));
        CHECK(m.model.Th(qs, j).mu == static_cast<double>(p.sign));
        CHECK(m.model.Th(qs, j).var == doctest::Approx(p.isObvious ? 1.0 : 0.09));
    }
    REQUIRE(m.curves.size() == 1);
    CHECK(m.curves[0].need == Need(Needs::NeedId::encumbrance));
    CHECK(m.model.covered.mu == -1.0);
    CHECK(m.model.covered.var == 1.0);
}

TEST_CASE("bootstrap: name mapping -- unknown names skipped, missing pairs at the effects.csv prior")
{
    // An unknown need and an unknown column in the file: skipped.
    auto text = Replace(Minimal(), "\"needs\": [\"health_deficit\",\"encumbrance\"]",
                        "\"needs\": [\"health_deficit\",\"no_such_need\"]");
    text = Replace(text, "\"cols\": [\"restore_health\",\"utility_carry_weight\"]",
                   "\"cols\": [\"no_such_col\",\"utility_carry_weight\"]");
    const auto r = Bootstrap::Read(text);
    REQUIRE(r.Ok());
    const auto m = Bootstrap::MapToBuild(*r.doc);
    REQUIRE(m.report.needsUnknown.size() == 1);
    CHECK(m.report.needsUnknown[0] == "no_such_need");
    REQUIRE(m.report.colsUnknown.size() == 1);
    CHECK(m.report.colsUnknown[0] == "no_such_col");
    const auto hd = Need(Needs::NeedId::health_deficit);
    // health_deficit x utility_carry_weight came from the file (0, precision 11.1111)
    CHECK(m.model.Th(hd, Col(Effect::Col::utility_carry_weight)).var == doctest::Approx(1.0 / 11.1111));
    // health_deficit x restore_health is no longer in the file: its obvious prior
    const auto p = Obvious::PriorOf(Needs::NeedId::health_deficit, Effect::Col::restore_health);
    REQUIRE(p.isObvious);
    CHECK(m.model.Th(hd, Col(Effect::Col::restore_health)).mu == 1.0);
    CHECK(m.model.Th(hd, Col(Effect::Col::restore_health)).var == 1.0);
    // encumbrance is no longer in the file: its row is the prior (utility_carry_weight is obvious)
    const auto enc = Need(Needs::NeedId::encumbrance);
    CHECK(m.model.Th(enc, Col(Effect::Col::utility_carry_weight)).mu ==
          static_cast<double>(Obvious::PriorOf(Needs::NeedId::encumbrance, Effect::Col::utility_carry_weight).sign));
    // the three signs of the prior
    const auto cro = Need(Needs::NeedId::combat_regen_ok);
    CHECK(m.model.Th(cro, Col(Effect::Col::kind_food)).mu == -1.0);
    CHECK(m.model.Th(cro, Col(Effect::Col::kind_food)).var == 1.0);
    CHECK(m.model.Th(hd, Col(Effect::Col::overshoot_health)).mu == 0.0);
    CHECK(m.model.Th(hd, Col(Effect::Col::overshoot_health)).var == 1.0);
    const auto rs = Need(Needs::NeedId::regen_suppressed);
    CHECK(m.model.Th(rs, Col(Effect::Col::restore)).mu == 1.0);
    CHECK(m.model.Th(rs, Col(Effect::Col::damage)).mu == 0.0);
    CHECK(m.model.Th(rs, Col(Effect::Col::damage)).var == doctest::Approx(0.09));
}

TEST_CASE("bootstrap: reader rules, one negative case each")
{
    const std::string mini = Minimal();
    const std::string full = Full();
    // 0. not JSON
    CHECK(Rejected("{\"format\": ", "not JSON"));
    // 1. format and version
    CHECK(Rejected(Replace(mini, "\"huginn.bootstrap\"", "\"other\""), "format"));
    CHECK(Rejected(Replace(mini, "\"version\": 1", "\"version\": 2"), "version"));
    CHECK(Rejected(Replace(mini, "\"version\": 1,", ""), "version"));
    // 2. grading and outcomes
    CHECK(Rejected(Replace(mini, "\"grading\": \"graded\"", "\"grading\": \"steep\""), "model.grading"));
    CHECK(Rejected(Replace(mini, "\"outcomes\": \"picks\"", "\"outcomes\": \"two\""), "model.outcomes"));
    // 3. needs and cols
    CHECK(Rejected(Replace(mini, "[\"health_deficit\",\"encumbrance\"]", "[\"health_deficit\",\"health_deficit\"]"),
                   "needs: duplicate"));
    CHECK(Rejected(Replace(mini, "[\"restore_health\",\"utility_carry_weight\"]", "[\"restore_health\",\"\"]"),
                   "cols: a list of names"));
    CHECK(Rejected(Replace(mini, "[\"health_deficit\",\"encumbrance\"]", "\"health_deficit\""), "needs: a list"));
    // 4. theta, precision, strength, bias
    CHECK(Rejected(Replace(mini, "[1.0,0.0],", "[1.0],"), "theta[0]"));
    CHECK(Rejected(Replace(mini, "[1.5,11.1111]", "[0,11.1111]"), "above 0"));
    CHECK(Rejected(Replace(mini, "[1.5,11.1111]", "[-1.5,11.1111]"), "above 0"));
    CHECK(Rejected(Replace(mini, "\"theta_strength\": null", "\"theta_strength\": [[0,0],[0,0]]"),
                   "only with grading presence+strength"));
    CHECK(Rejected(Replace(full, "\"theta_strength\": [", "\"theta_strength_x\": ["), "theta_strength: missing"));
    CHECK(Rejected(Replace(mini, "\"theta_bias\": null", "\"theta_bias\": [1.0]"), "theta_bias"));
    CHECK(Rejected(Replace(full, "\"theta_bias\": null", "\"theta_bias\": [0,0,0,0]"), "not with grading presence+strength"));
    CHECK(Rejected(Replace(mini, "[1.0,0.0],", "[1.0,true],"), "not all finite numbers"));
    // 5. kappa
    CHECK(Rejected(Replace(mini, "\"kappa\": 2.5", "\"kappa\": \"2.5\""), "kappa"));
    // 6. u0
    CHECK(Rejected(Replace(mini, "\"u0\": null", "\"u0\": {\"bias\": 0, \"need\": {}}"), "u0: null unless"));
    CHECK(Rejected(Replace(full, "\"bias\": -1.5,", ""), "u0: {bias, need}"));
    CHECK(Rejected(Replace(full, "\"hunger\": -0.5", "\"thirst\": -0.5"), "u0.need"));
    // 7. recent
    CHECK(Rejected(Replace(full, "\"window_s\": 30.0,", ""), "recent: window_s"));
    CHECK(Rejected(Replace(full, "\"family\": \"summon\",\n    \"tau_s\": 5.0", "\"family\": \"nope\",\n    \"tau_s\": 5.0"),
                   "family: a name from cols"));
    CHECK(Rejected(Replace(full, "\"tau_s\": 5.0,", "\"tau_s\": 7.0,"), "tau_s: one of taus_s"));
    CHECK(Rejected(Replace(full, "\"precision\": [11.1111,11.1111,11.1111,14.0]", "\"precision\": [0,11.1111,11.1111,14.0]"),
                   "precision: above 0"));
    CHECK(Rejected(Replace(full, "\"theta\": [0.0,0.0,0.0,-0.75]", "\"theta\": [0.0,0.0,-0.75]"), "theta: 4 numbers"));
    CHECK(Rejected(Replace(full, ",\n    \"u0\": 0.0625", ""), "u0: present exactly"));
    // 8. features
    CHECK(Rejected(Replace(full, "\"name\": \"ln util\"", "\"name\": \"ln utility\""), "features[0]"));
    CHECK(Rejected(Replace(full, "\"w\": 0.875", "\"w\": null"), "features[0]"));
    // 9. items
    CHECK(Rejected(Replace(full, "\"local\": \"00087E\"", "\"local\": \"00087e\""), "items[0].local"));
    CHECK(Rejected(Replace(full, "\"local\": \"00087E\"", "\"local\": \"87E\""), "items[0].local"));
    CHECK(Rejected(Replace(full, "\"plugin\": \"ccBGSSSE001-Fish.esm\"", "\"plugin\": \"\""), "items[0].plugin"));
    CHECK(Rejected(Replace(full, "\"precision\": 4.5", "\"precision\": 0"), "items[0].precision"));
    CHECK(Rejected(Replace(full, "\"b\": 0.125", "\"b\": \"x\""), "items[0]: b and precision"));
    CHECK(Rejected(Replace(full, "\"plugin\": \"Update.esm\",\n   \"local\": \"000801\"",
                           "\"plugin\": \"skyrim.ESM\",\n   \"local\": \"03EAF3\""),
                   "duplicate plugin + local"));
    // 10. curves
    CHECK(Rejected(Replace(full, "[\"logistic\",0.9,40.0]", "[\"logistic\",0.9,0]"), "curves.encumbrance"));
    CHECK(Rejected(Replace(full, "[\"logistic\",0.9,40.0]", "[\"linear\",0.9,40.0]"), "curves.encumbrance"));
    CHECK(Rejected(Replace(full, "\"encumbrance\": [\"logistic\"", "\"thirst\": [\"logistic\""), "curves.thirst"));
    // a field the reader does not know is allowed
    CHECK(Bootstrap::Read(Replace(mini, "\"kappa\": 2.5", "\"kappa\": 2.5, \"future_field\": [1, {\"x\": 2}]")).Ok());
}

TEST_CASE("bootstrap: items resolve through the plugin side, mismatches skipped and counted")
{
    auto text = Replace(Full(), "\"plugin\": \"Update.esm\",\n   \"local\": \"000801\"",
                        "\"plugin\": \"Update.esm\",\n   \"local\": \"000802\"");
    const auto r = Bootstrap::Read(text);
    REQUIRE(r.Ok());
    // Fish: resolves; Skyrim potion: wrong name in the game; Update 000802: gone
    std::map<std::pair<std::string, std::uint32_t>, Bootstrap::Found> game{
        { { "ccBGSSSE001-Fish.esm", 0x87E }, { 0xFE01A87E, true, "Roasted Carp" } },
        { { "Skyrim.esm", 0x03EAF3 }, { 0x0003EAF3, true, "Potion of Minor Stamina" } },
    };
    Bootstrap::ItemReport rep;
    const auto items = Bootstrap::ResolveItems(*r.doc, [&](const std::string& p, std::uint32_t local) -> std::optional<Bootstrap::Found> {
        const auto it = game.find({ p, local });
        if (it == game.end()) return std::nullopt;
        return it->second;
    }, rep);
    REQUIRE(items.size() == 1);
    CHECK(items[0].form == 0xFE01A87E);
    CHECK(items[0].b == 0.125);
    CHECK(items[0].var == doctest::Approx(1.0 / 4.5));
    CHECK(items[0].picks == 2);
    CHECK(rep.resolved == 1);
    CHECK(rep.nameMismatch == 1);
    CHECK(rep.unresolved == 1);
    // a form type the catalog does not describe; two keys landing on one form
    game[{ "Skyrim.esm", 0x03EAF3 }] = { 0x0003EAF3, false, "Potion of Minor Healing" };
    game[{ "Update.esm", 0x000802 }] = { 0xFE01A87E, true, "Feather on Self" };
    Bootstrap::ItemReport rep2;
    const auto items2 = Bootstrap::ResolveItems(*r.doc, [&](const std::string& p, std::uint32_t local) -> std::optional<Bootstrap::Found> {
        const auto it = game.find({ p, local });
        if (it == game.end()) return std::nullopt;
        return it->second;
    }, rep2);
    CHECK(items2.size() == 1);
    CHECK(rep2.kindMismatch == 1);
    CHECK(rep2.duplicateForm == 1);
}

TEST_CASE("bootstrap: names agree exactly, or on their ASCII bytes when either is not ASCII")
{
    CHECK(Bootstrap::NamesAgree("Roasted Carp", "Roasted Carp"));
    CHECK(Bootstrap::NamesAgree("", "anything"));
    CHECK_FALSE(Bootstrap::NamesAgree("Roasted Carp", "Roasted Salmon"));
    CHECK_FALSE(Bootstrap::NamesAgree("Roasted Carp", "roasted carp"));
    // cp1252 o-umlaut in the game, U+FFFD (the CSV reader's replacement) in the file
    CHECK(Bootstrap::NamesAgree("Bj\xEF\xBF\xBDrn's Axe", "Bj\xF6rn's Axe"));
    CHECK_FALSE(Bootstrap::NamesAgree("Bj\xEF\xBF\xBDrn's Sword", "Bj\xF6rn's Axe"));
}

TEST_CASE("bootstrap: stable keys and the SHA-256 the log head carries")
{
    using Bootstrap::StableKey;
    CHECK(StableKey("theta", "health_deficit", "restore_health") == StableKey("theta", "health_deficit", "restore_health"));
    CHECK(StableKey("theta", "health_deficit", "restore_health") != StableKey("theta", "restore_health", "health_deficit"));
    CHECK(StableKey("theta", "ab", "c") != StableKey("theta", "a", "bc"));
    CHECK(StableKey("theta", "a", "b") != StableKey("strength", "a", "b"));
    CHECK(Bootstrap::TauText(5.0) == "5");
    CHECK(Bootstrap::TauText(30.0) == "30");
    CHECK(Bootstrap::TauText(2.5) == "2.5");
    // FIPS 180-4 vectors
    CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(Sha256Hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // 55 / 56 / 64 bytes: the padding's block boundary
    CHECK(Sha256Hex(std::string(55, 'a')).size() == 64);
    CHECK(Sha256Hex(std::string(56, 'a')) != Sha256Hex(std::string(55, 'a')));
}
