// R8.2 replay equivalence (an R8 "Done when" item): the C++ core reproduces
// fitlib's per-row logits under the shipped bootstrap to 1e-5.
//
// The fixture is written by tools/fit/export_scores.py: logged decisions (the
// checked-in one is synthetic: two launches, menu co-picks, the bootstrap's own
// items through a form map, light and dynamic forms, a transient and a
// SimonRim pick that never count) with fitlib.Model.scores' item score and
// logit for every row of fitlib.build's choice sets. This test reads the
// shipped bootstrap with core/Bootstrap.h (names mapped, missing pairs at the
// prior, items resolved through the fixture's form map), rebuilds
// recent_<family> with the live tracker from EVERY logged pick -- applying
// Choice::UsablePick and the confirmMs subtraction (Choice::PickTime) itself, as
// ChoiceSubscriber does live, and checking both against fitlib's timeline and
// pick times -- (and checks the values against fitlib's), re-reads the curve needs through the
// file's curves, and scores every row with core/ChoiceScore.h. theta_covered
// is 0 (the bootstrap has no covered weight; the fit did not read covered).
//
// The mage's real log (not checked in) runs through the same code:
//   HUGINN_REPLAY_FIXTURE=<work>/replay_scores.jsonl  the fixture to read
//   HUGINN_REPLAY_OUT=<work>/replay_cpp.jsonl          C++ mu and logit per row
// then `python tools/fit/export_scores.py --compare <out> <fixture>` compares
// hit@8, page hit@1 and the NLL.

#include "core/Bootstrap.h"
#include "core/ChoiceScore.h"
#include "core/MiniJson.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Huginn::Core;
namespace C = Huginn::Core::Choice;

namespace
{
    std::string Env(const char* name)
    {
#ifdef _MSC_VER
        char* buf = nullptr;
        std::size_t len = 0;
        if (_dupenv_s(&buf, &len, name) != 0 || !buf) return {};
        std::string v(buf);
        std::free(buf);
        return v;
#else
        const char* v = std::getenv(name);
        return v ? std::string(v) : std::string{};
#endif
    }

    std::string ReadAll(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "cannot open " << path);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    }

    std::vector<C::SparseCol> Pairs(const Json::Value& v, const std::vector<int>& colMap)
    {
        std::vector<C::SparseCol> out;
        for (const auto& p : v.Items()) {
            const int j = static_cast<int>(p.Items()[0].AsNumber());
            if (j < 0 || j >= static_cast<int>(colMap.size()) || colMap[static_cast<std::size_t>(j)] < 0) continue;
            out.emplace_back(static_cast<std::uint16_t>(colMap[static_cast<std::size_t>(j)]), p.Items()[1].AsNumber());
        }
        return out;
    }
}

TEST_CASE("choice replay: the C++ score reproduces fitlib's per-row logits under the shipped bootstrap")
{
    std::string fixturePath = Env("HUGINN_REPLAY_FIXTURE");
    const bool external = !fixturePath.empty();
    if (!external) fixturePath = std::string(HUGINN_REPO_ROOT) + "/tests/core/fixtures/choice/replay_synthetic.jsonl";
    const std::string outPath = Env("HUGINN_REPLAY_OUT");

    std::ifstream in(fixturePath, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open " << fixturePath);
    std::string line;
    REQUIRE(std::getline(in, line));
    auto headParsed = Json::Parse(line);
    REQUIRE(headParsed.Ok());
    const Json::Value head = *headParsed.value;

    // The bootstrap, mapped by name; theta_covered 0 for the replay.
    std::string bpath = head.Find("bootstrap")->AsString();
    if (!(bpath.size() > 1 && (bpath[1] == ':' || bpath[0] == '/'))) bpath = std::string(HUGINN_REPO_ROOT) + "/" + bpath;
    const auto read = Bootstrap::Read(ReadAll(bpath));
    REQUIRE_MESSAGE(read.Ok(), (read.errors.empty() ? "" : read.errors[0]));
    Bootstrap::MapOptions opt;
    opt.coveredPrior = 0.0;
    const auto mapped = Bootstrap::MapToBuild(*read.doc, opt);
    const C::Model& model = mapped.model;

    // Items through the fixture's form map (the plugin side's LookupForm).
    std::map<std::pair<std::string, std::uint32_t>, Bootstrap::Found> forms;
    for (const auto& [form, v] : head.Find("items")->Members()) {
        const std::uint32_t id = static_cast<std::uint32_t>(std::stoul(form, nullptr, 16));
        const std::uint32_t local = static_cast<std::uint32_t>(std::stoul(v.Items()[1].AsString(), nullptr, 16));
        forms[{ Lower(v.Items()[0].AsString()), local }] = Bootstrap::Found{ id, true, v.Items()[2].AsString() };
    }
    Bootstrap::ItemReport irep;
    const auto items = Bootstrap::ResolveItems(*read.doc, [&](const std::string& p, std::uint32_t local) -> std::optional<Bootstrap::Found> {
        const auto it = forms.find({ Lower(p), local });
        if (it == forms.end()) return std::nullopt;
        return it->second;
    }, irep);
    std::unordered_map<std::uint32_t, double> b;
    for (const auto& it : items) b[it.form] = it.b;
    CHECK(irep.nameMismatch == 0);
    CHECK(irep.kindMismatch == 0);

    // The fixture's names -> the build's indices.
    std::vector<int> needMap, colMap;
    for (const auto& n : head.Find("needs")->Items()) {
        int k = -1;
        for (std::size_t i = 0; i < Needs::kNeedCount; ++i) {
            if (Needs::kNeeds[i].id == n.AsString()) k = static_cast<int>(i);
        }
        needMap.push_back(k);
    }
    for (const auto& c : head.Find("cols")->Items()) {
        int j = -1;
        for (std::size_t i = 0; i < Effect::kColumnCount; ++i) {
            if (Effect::kColumns[i].id == c.AsString()) j = static_cast<int>(i);
        }
        colMap.push_back(j);
    }
    std::unordered_map<std::string, std::size_t> needByName;
    for (std::size_t k = 0; k < Needs::kNeedCount; ++k) needByName.emplace(std::string(Needs::kNeeds[k].id), k);

    // Events: picks (the timeline) and decisions, in time order, a decision
    // before a pick of the same time (a pick at the decision's own time is not
    // "strictly before" it).
    struct Event
    {
        double ts = 0.0;
        bool pick = false;
        Json::Value v;
    };
    std::vector<Event> events;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto r = Json::Parse(line);
        REQUIRE(r.Ok());
        Event e;
        // the decision moment from the logged utc and confirmMs, as the live
        // subscriber computes it; checked against fitlib.pick_time below
        const Json::Value* utc = r.value->Find("utc");
        const Json::Value* cms = r.value->Find("cms");
        REQUIRE_MESSAGE((utc && cms), "fixture predates the raw pick fields: regenerate it with export_scores.py");
        e.pick = r.value->Find("t")->AsString() == "pick";
        const bool nothing = e.pick && r.value->Find("out")->AsString() == "nothing";
        e.ts = nothing ? utc->AsNumber() : C::PickTime(utc->AsNumber(), cms->AsNumber());
        e.v = std::move(*r.value);
        events.push_back(std::move(e));
    }
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& c) {
        return a.ts != c.ts ? a.ts < c.ts : (!a.pick && c.pick);
    });
    std::unordered_map<std::string, std::uint32_t> genOf;
    auto gen = [&](const std::string& lg) { return genOf.emplace(lg, static_cast<std::uint32_t>(genOf.size() + 1)).first->second; };

    std::ofstream out;
    if (!outPath.empty()) out.open(outPath, std::ios::binary);

    C::RecentTracker tracker;
    double maxMu = 0.0, maxLogit = 0.0, maxRec = 0.0, maxTs = 0.0;
    std::size_t decisions = 0, rows = 0, withRecent = 0, withB = 0, withLm = 0, offPage = 0;
    std::size_t picksUsed = 0, picksSkipped = 0, useDisagrees = 0;
    for (const auto& e : events) {
        const Json::Value& v = e.v;
        if (const Json::Value* ts = v.Find("ts"); ts && ts->IsNumber()) maxTs = std::max(maxTs, std::abs(e.ts - ts->AsNumber()));
        if (e.pick) {
            // fitlib.recent_families' exclusions, by the live test
            const bool usable = C::UsablePick(v.Find("out")->AsString() == "nothing", v.Find("ctx")->AsBool(),
                                              v.Find("chosen")->AsBool(), v.Find("name")->AsString(),
                                              v.Find("list")->AsString());
            useDisagrees += usable != v.Find("use")->AsBool() ? 1 : 0;
            if (!usable) {
                ++picksSkipped;
                continue;
            }
            ++picksUsed;
            C::RecentPick p;
            p.t = e.ts;
            p.gen = gen(v.Find("lg")->AsString());
            p.id = static_cast<std::uint64_t>(v.Find("id")->AsNumber());
            p.hasCap = v.Find("cap")->AsBool();
            for (const auto& c : v.Find("cols")->Items()) {
                const int j = colMap[static_cast<std::size_t>(c.AsNumber())];
                if (j >= 0) p.cols.push_back(static_cast<std::uint16_t>(j));
            }
            tracker.Add(std::move(p));
            continue;
        }
        ++decisions;
        // recent_<family> from the live tracker, checked against fitlib's
        const double id = v.Find("id")->AsNumber();
        const auto r = tracker.Values(model, e.ts, gen(v.Find("lg")->AsString()),
                                      id < 0 ? UINT64_MAX : static_cast<std::uint64_t>(id));
        const Json::Value* rec = v.Find("rec");
        bool anyRec = false;
        for (std::size_t f = 0; f < model.recent.size(); ++f) {
            const std::string name = std::string(Effect::kColumns[model.recent[f].col].id) + "@" + Bootstrap::TauText(model.recent[f].tau);
            const Json::Value* x = rec->Find(name);
            const double expect = x ? x->AsNumber() : 0.0;
            maxRec = std::max(maxRec, std::abs(r[f] - expect));
            anyRec |= r[f] != 0.0;
        }
        withRecent += anyRec ? 1 : 0;
        // the need vector, the curve needs re-read through the file's curves
        std::array<double, C::K> n{};
        for (const auto& p : v.Find("n")->Items()) {
            const auto it = needByName.find(p.Items()[0].AsString());
            if (it != needByName.end()) n[it->second] = p.Items()[1].AsNumber();
        }
        for (const auto& p : v.Find("in")->Items()) {
            const auto it = needByName.find(p.Items()[0].AsString());
            if (it == needByName.end()) continue;
            for (const auto& c : mapped.curves) {
                if (c.need == it->second) {
                    n[c.need] = 1.0 / (1.0 + std::exp(-c.slope * (p.Items()[1].AsNumber() - c.centre)));
                }
            }
        }
        const C::Tick tick = C::MakeTick(model, n, r);
        std::string muText, logitText;
        for (const auto& row : v.Find("rows")->Items()) {
            const auto& f = row.Items();
            const C::Attr at = C::MakeAttr(model.grading, Pairs(f[0], colMap), Pairs(f[1], colMap));
            C::RowExtra x;
            x.offPage = f[2].AsNumber() != 0.0;
            const std::uint32_t form = static_cast<std::uint32_t>(std::stoul(f[3].AsString(), nullptr, 16));
            if (const auto it = b.find(form); it != b.end()) {
                x.bMu = it->second;
                ++withB;
            }
            x.feat[static_cast<std::size_t>(C::Feature::ln_learn_mult)] = f[4].AsNumber();
            x.feat[static_cast<std::size_t>(C::Feature::util_missing)] = f[5].AsNumber();
            withLm += f[4].AsNumber() != 0.0 ? 1 : 0;
            offPage += x.offPage ? 1 : 0;
            const auto s = C::Score(model, tick, at, x, false);
            maxMu = std::max(maxMu, std::abs(s.mu - f[6].AsNumber()));
            maxLogit = std::max(maxLogit, std::abs(s.logit - f[7].AsNumber()));
            ++rows;
            if (out) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s%.12g", muText.empty() ? "" : ",", s.mu);
                muText += buf;
                std::snprintf(buf, sizeof(buf), "%s%.12g", logitText.empty() ? "" : ",", s.logit);
                logitText += buf;
            }
        }
        if (out) {
            out << "{\"i\":" << static_cast<long long>(v.Find("i")->AsNumber()) << ",\"mu\":[" << muText << "],\"logit\":["
                << logitText << "]}\n";
        }
    }
    MESSAGE("replay: " << decisions << " decisions, " << rows << " rows (" << offPage << " off the page, " << withB
                       << " with a b, " << withLm << " with ln learn mult), " << withRecent << " with a recent family; "
                       << "largest |mu - fitlib| " << maxMu << ", |logit - fitlib| " << maxLogit
                       << ", |recent - fitlib| " << maxRec << "; picks " << picksUsed << " usable, " << picksSkipped
                       << " excluded by UsablePick, largest |pick time - fitlib| " << maxTs);
    CHECK(useDisagrees == 0);
    CHECK(maxTs < 1e-6);
    CHECK(decisions == static_cast<std::size_t>(head.Find("decisions")->AsNumber()));
    CHECK(maxMu < 1e-5);
    CHECK(maxLogit < 1e-5);
    CHECK(maxRec < 1e-9);
    if (!external) {
        // the synthetic fixture exercises every term
        CHECK(withRecent > 0);
        CHECK(withB > 0);
        CHECK(withLm > 0);
        CHECK(offPage > 0);
        CHECK(picksSkipped >= 4);   // the transient, SimonRim, nothing and no-context picks
    }
}
