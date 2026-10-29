// core/DecisionLog.h: the selection log v3 encoder.
//
// The golden file tests/core/fixtures/decisions/synthetic_v3.jsonl is what
// this encoder makes of the synthetic decisions below, byte for byte. The
// replay side reads the same file (tools/replay/test_replay_v3.py checks every
// value it decodes against the numbers written here), so the two ends of the
// format are pinned to one file. To regenerate after a deliberate format
// change: run the tests, copy <build>/tests/synthetic_v3.actual.jsonl over the
// fixture, and update the Python test's expectations. Before a deliberate
// format change, keep the old golden file beside it under the version that
// wrote it (synthetic_v3_0.23.52.jsonl: before 0.23.53's old-engine columns),
// so the readers keep reading it.

#include "core/DecisionLog.h"
#include "core/NeedIds.h"

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <ostream>
#include <sstream>
#include <string>

#ifndef HUGINN_REPO_ROOT
#error "HUGINN_REPO_ROOT must be defined (tests/CMakeLists.txt)"
#endif

using namespace Huginn::Core;
using namespace Huginn::Core::DecisionLog;

namespace
{
    Effect::Col C(std::string_view name)
    {
        const auto c = Effect::FromName(name);
        REQUIRE(c.has_value());
        return *c;
    }

    std::size_t N(Needs::NeedId id) { return static_cast<std::size_t>(id); }

    Effect::Cap Sorted(Effect::Cap cap)
    {
        std::sort(cap.begin(), cap.end(), [](const Effect::Value& a, const Effect::Value& b) { return a.col < b.col; });
        return cap;
    }

    // The synthetic session. Every number here is what test_replay_v3.py expects to read back.
    struct Synthetic
    {
        Effect::Cap potion = Sorted({ { C("restore"), 0.62f }, { C("restore_health"), 0.62f } });
        Effect::Cap fireSpell = Sorted({ { C("damage"), 0.4f }, { C("damage_health_fire"), 0.4f }, { C("school_destruction"), 1.0f } });
        Effect::Cap boots = Sorted({ { C("armour_rating"), 0.3f } });
        Effect::Cap soup = Sorted({ { C("survival"), 0.5f }, { C("survival_hunger"), 0.5f } });
        Effect::Cap familiar = Sorted({ { C("summon"), 0.6f }, { C("summon_creature"), 0.6f }, { C("school_conjuration"), 1.0f } });

        std::shared_ptr<const Context> press, menu, onset, lap;
        std::vector<Decision> decisions;

        static Head MakeHead()
        {
            Head h;
            h.launch = "20261009-120000";
            h.list = "synthetic";
            h.build = "0.23.15 (test)";
            h.sourceNames = { "Spell", "Potion", "Scroll", "Weapon", "Ammo", "SoulGem", "Food", "Staff", "Apparel", "Torch" };
            // 0.23.41: the [Needs] curves in force, one per need. Set here, not
            // from the csv defaults, so a default changing does not move the
            // golden file: step 0.5 everywhere but two.
            h.curves.assign(Needs::kNeedCount, Needs::Curve{ Needs::CurveKind::Step, 0.5f, 0.0f });
            h.curves[N(Needs::NeedId::health_deficit)] = { Needs::CurveKind::Logistic, 0.5f, 10.0f };
            h.curves[N(Needs::NeedId::enemy_count)] = { Needs::CurveKind::Linear, 1.0f, 5.0f };
            return h;
        }

        Decision Base(std::uint64_t seq, const char* utc) const
        {
            Decision d;
            d.seq = seq;
            d.utc = utc;
            d.launch = "20261009-120000";
            d.list = "synthetic";
            d.character = 0x3F3E2A8817962D59ull;
            d.gen = 1;
            return d;
        }

        Synthetic()
        {
            // An instance cap (a tempered sword), owned outside the catalog.
            auto sword = std::make_shared<const Effect::Cap>(Sorted({ { C("weapon_damage"), 0.55f } }));

            auto p = std::make_shared<Context>();
            p->id = 1;
            p->utc = "2026-10-09 12:00:01.000";
            p->why = "press";
            p->need[N(Needs::NeedId::health_deficit)] = 0.8808f;
            p->input[N(Needs::NeedId::health_deficit)] = 0.7f;
            p->need[N(Needs::NeedId::in_combat)] = 1.0f;
            p->input[N(Needs::NeedId::in_combat)] = 1.0f;
            p->pipeValid = true;
            p->page = 0;
            p->pageSlots = 8;
            p->pipeAgeMs = 40.0f;
            p->race = "NordRace";
            p->wildcardBase = 0.165f;
            p->wildcardMax = 0.5f;
            p->env.valid = true;          // 0.23.41: an interior at 13:30, raw light 0.3
            p->env.interior = true;
            p->env.hour = 13.5f;
            p->env.openDaylight = false;
            p->env.light = 0.3f;
            // 0.23.53: the old engine's run behind the eligible rows.
            p->old.valid = true;
            p->old.minCtx = 0.05f;
            p->old.minUtil = 0.1f;
            p->old.topN = 10;
            p->old.coldBoost = 0.2f;
            p->old.beta = 0.2f;
            p->old.lambdaMin = 0.5f;
            p->old.lambdaMax = 3.0f;
            p->old.n0 = 2.0f;
            p->old.favMode = "boost";
            p->old.favMin = 1.3f;
            p->old.favMax = 2.5f;
            p->old.scored = 2;
            p->old.totalTrains = 37.5f;
            p->old.phi.assign(18, 0.0f);
            p->old.phi[0] = 1.0f;     // bias
            p->old.phi[1] = 0.3f;     // health
            p->old.phi[17] = 0.25f;
            {
                Row r;   // 0: a potion on key 3
                r.form = 0x0003EADE;
                r.kind = static_cast<std::uint8_t>(Effect::Kind::Potion);
                r.src = 1;
                r.flags = Flag::Eligible | Flag::Scored | Flag::Held | Flag::Shown;
                r.slot = 2;
                r.util = 1.234f;
                r.cross[static_cast<std::size_t>(Cross::overshoot_health)] = -0.25f;
                r.cross[static_cast<std::size_t>(Cross::stack_count)] = 0.4553f;
                r.cap = &potion;
                // 0.23.53: scored by the main loop.
                r.old.est = 0.42f;
                r.old.alpha = 0.6667f;
                r.old.trains = 4.0f;
                r.old.ucb = 0.3105f;
                r.old.rec = 0.1875f;
                r.old.prior = 0.55f;
                r.old.ctx = 0.8f;
                r.old.floor = static_cast<std::uint8_t>(Floor::pass);
                p->rows.push_back(r);
            }
            {
                Row r;   // 1: a fire spell shown as a wildcard
                r.form = 0x00012FCD;
                r.kind = static_cast<std::uint8_t>(Effect::Kind::Spell);
                r.src = 0;
                r.flags = Flag::Eligible | Flag::Scored | Flag::Held | Flag::Shown | Flag::Wildcard;
                r.slot = 5;
                r.util = 0.31f;
                r.cross[static_cast<std::size_t>(Cross::school_fortified)] = 1.0f;
                r.wildcardP = 0.0825f;
                r.cap = &fireSpell;
                // 0.23.53: a favourite on vanilla hotkey 3 (index 2), rescued
                // by the cold start (context 0.02 under the 0.05 floor).
                r.flags |= Flag::Favorite;
                r.hotkey = 2;
                r.old.est = 0.0f;
                r.old.alpha = 0.0f;
                r.old.trains = 0.0f;
                r.old.ucb = 1.0f;
                r.old.rec = 0.0f;
                r.old.prior = 0.3f;
                r.old.ctx = 0.02f;
                r.old.floor = static_cast<std::uint8_t>(Floor::cold);
                p->rows.push_back(r);
            }
            {
                Row r;   // 2: a tempered sword stack below the floor (no util)
                r.form = 0x00012EB7;
                r.uid = 3;
                r.kind = static_cast<std::uint8_t>(Effect::Kind::Weapon);
                r.src = 3;
                r.flags = Flag::Eligible | Flag::Held;
                r.cross[static_cast<std::size_t>(Cross::weapon_charge)] = 0.5f;
                r.cap = sword.get();
                r.capOwner = sword;
                // 0.23.53: under the utility floor, with what it would have scored.
                r.old.est = -0.05f;
                r.old.alpha = 0.5f;
                r.old.trains = 2.5f;
                r.old.ucb = 0.8f;
                r.old.rec = 0.0f;
                r.old.prior = 0.7f;
                r.old.ctx = 0.06f;
                r.old.floor = static_cast<std::uint8_t>(Floor::util);
                r.old.wouldUtil = 0.0812f;
                p->rows.push_back(r);
            }
            {
                Row r;   // 3: worn boots, not a candidate
                r.form = 0x00013911;
                r.kind = static_cast<std::uint8_t>(Effect::Kind::Armour);
                r.flags = Flag::Held | Flag::Equipped;
                r.cap = &boots;
                // 0.23.53: held, never gathered: the learner's metrics only
                // (the row ends after rec).
                r.old.est = 0.0f;
                r.old.alpha = 0.0f;
                r.old.trains = 0.0f;
                r.old.ucb = 1.0f;
                r.old.rec = 0.0f;
                p->rows.push_back(r);
            }
            {
                Row r;   // 4: an item outside the catalog
                r.form = 0xFF000801;
                r.flags = Flag::Held | Flag::Favorite;   // 0.23.53: favourited on key 8,
                r.hotkey = 7;                            // nothing else: nine nulls before hk
                p->rows.push_back(r);
            }
            {
                // 5 (0.23.46): a summon spell the summon filter dropped at the
                // limit (2 of 2 up), covered 0.8176 = left(8 s), the oldest
                // summon's 8 s.
                Row r;
                r.form = 0x000640B6;
                r.kind = static_cast<std::uint8_t>(Effect::Kind::Spell);
                r.flags = Flag::Held;
                r.filteredBy = static_cast<std::uint8_t>(Filter::Reason::summon_limit);
                r.cross[static_cast<std::size_t>(Cross::covered)] = 0.8176f;
                r.coveredSec = 8.0f;
                r.cap = &familiar;
                // 0.23.53: a filter drop: the run's parts, no floor.
                r.old.est = 0.12f;
                r.old.alpha = 0.8f;
                r.old.trains = 8.0f;
                r.old.ucb = 0.2f;
                r.old.rec = 0.0f;
                r.old.prior = 0.6f;
                r.old.ctx = 0.4f;
                r.old.wouldUtil = 0.9f;
                p->rows.push_back(r);
            }
            // 0.23.46: the player's summons: a familiar (8 s left, cast 52 s
            // ago) and a thrall with no timer, against a limit of 2.
            p->summon.valid = true;
            p->summon.count = 2;
            p->summon.limit = 2.0f;
            p->summon.list.push_back({ 0x000640B6, 0xFF000ABC, Coverage::CommandedActor::Kind::Summon, 8.0f, 52.0f });
            p->summon.list.push_back({ 0x0007E8DF, 0xFF000ABD, Coverage::CommandedActor::Kind::Reanimate,
                                       Coverage::kNoTimer, 10.25f });
            press = p;

            auto m = std::make_shared<Context>(*p);   // the inventory opened a second later
            m->id = 2;
            m->utc = "2026-10-09 12:00:02.000";
            m->why = "menu";
            m->menu = "InventoryMenu";
            m->rows.resize(2);   // potion and spell only
            m->rows[1].flags = Flag::Eligible | Flag::Scored | Flag::Held | Flag::Favorite;   // the wildcard ...
            m->rows[1].slot = -1;                                             // ... no longer shown
            m->rows[1].wildcardP = kNone;
            menu = m;

            auto o = std::make_shared<Context>();
            o->id = 3;
            o->utc = "2026-10-09 12:01:00.000";
            o->why = "onset";
            o->need[N(Needs::NeedId::hunger)] = 0.71f;
            o->input[N(Needs::NeedId::hunger)] = 0.6f;
            o->pipeValid = true;
            o->page = 1;
            o->pageSlots = 4;
            o->pipeAgeMs = 1500.0f;
            o->heldFull = false;   // taken inside the post-load window
            {
                Row r;
                r.form = 0x0003AD72;
                r.kind = static_cast<std::uint8_t>(Effect::Kind::Food);
                r.src = 6;
                r.flags = Flag::Eligible | Flag::Held | Flag::Shown;
                r.slot = 0;
                r.util = 0.2f;
                r.cross[static_cast<std::size_t>(Cross::stack_count)] = 0.2277f;
                r.cap = &soup;
                o->rows.push_back(r);
            }
            onset = o;

            // 1. A key press: the potion, drunk.
            Decision d1 = Base(1, "2026-10-09 12:00:01.900");
            d1.outcome = Outcome::Key;
            d1.form = 0x0003EADE;
            d1.name = "Potion of Healing";
            d1.row = 0;
            d1.src = "Hotkey";
            d1.via = "key 3 (s2)";
            d1.how = "consumed";
            d1.kind = "consume";
            d1.confirmMs = 812.0f;
            d1.ctx = press;
            d1.ctxAgeMs = 12.0f;
            d1.open = { static_cast<std::uint8_t>(N(Needs::NeedId::health_deficit)), static_cast<std::uint8_t>(N(Needs::NeedId::in_combat)) };
            d1.rid = 41;   // 0.23.53: its v2 record's id
            decisions.push_back(d1);

            // 2. A menu pick of armour the context does not hold (the frozen learner skips armour).
            Decision d2 = Base(2, "2026-10-09 12:00:09.000");
            d2.outcome = Outcome::Menu;
            d2.form = 0x000136D5;
            d2.name = "\xC9p\xE9" "e Boots";   // cp1252, not UTF-8
            {
                Row r;
                r.form = 0x000136D5;
                r.kind = static_cast<std::uint8_t>(Effect::Kind::Armour);
                r.flags = Flag::Held | Flag::AddedAtPick;
                r.cap = &boots;   // same cap as the worn boots: shares its id
                d2.added.push_back(r);
            }
            d2.row = 2;   // ctx.rows has two
            d2.src = "External";
            d2.via = "inventory menu";
            d2.caseLabel = "A (not candidate)";
            d2.how = "still equipped";
            d2.kind = "equip";
            d2.confirmMs = 3000.0f;
            d2.learned = false;
            d2.skip = "armour";
            d2.ctx = menu;
            d2.ctxAgeMs = 4200.0f;
            decisions.push_back(d2);

            // 3. A second pick in the same menu visit: shares the context.
            Decision d3 = Base(3, "2026-10-09 12:00:10.000");
            d3.outcome = Outcome::Menu;
            d3.form = 0x00012FCD;
            d3.name = "Firebolt";
            d3.row = 1;
            d3.src = "External";
            d3.via = "inventory menu";
            d3.caseLabel = "C (near-miss)";
            d3.how = "still equipped";
            d3.kind = "equip";
            d3.confirmMs = 3000.0f;
            d3.ctx = menu;
            d3.ctxAgeMs = 5300.0f;
            d3.repeat = true;
            d3.rid = 42;
            decisions.push_back(d3);

            // 4. Nothing pressed while hungry.
            Decision d4 = Base(4, "2026-10-09 12:01:16.000");
            d4.outcome = Outcome::Nothing;
            d4.ctx = onset;
            d4.ctxAgeMs = 16000.0f;
            d4.need = static_cast<int>(N(Needs::NeedId::hunger));
            d4.durSec = 12.5;
            d4.peak = 0.71f;
            d4.onsetUtc = "2026-10-09 12:01:00.000";
            d4.episodeSec = 12.5;   // never lapped: the whole episode
            decisions.push_back(d4);

            // 5. A wheel pick in the first context (already defined).
            Decision d5 = Base(5, "2026-10-09 12:01:20.000");
            d5.outcome = Outcome::Wheel;
            d5.form = 0x00012FCD;
            d5.name = "Firebolt";
            d5.row = 1;
            d5.src = "Wheeler";
            d5.via = "wheel";
            d5.how = "still equipped";
            d5.kind = "equip";
            d5.confirmMs = 3000.0f;
            d5.ctx = press;
            d5.ctxAgeMs = 79000.0f;
            d5.preEquipped = true;   // the spell was already in the other hand
            d5.rid = 43;
            decisions.push_back(d5);

            // 6. (0.23.51) A lap: diseased since 12:02:00, its second stretch
            // (begun by the first lap, 12:03:30) passed 60 s with nothing
            // pressed. Joined to the context the lap took, with the episode's
            // onset kept.
            auto l = std::make_shared<Context>();
            l->id = 4;
            l->utc = "2026-10-09 12:03:30.000";
            l->why = "lap";
            l->need[N(Needs::NeedId::diseased)] = 1.0f;
            l->input[N(Needs::NeedId::diseased)] = 1.0f;
            l->pipeValid = true;
            l->page = 0;
            l->pageSlots = 8;
            l->pipeAgeMs = 60.0f;
            l->rows = onset->rows;   // the soup, shown
            lap = l;
            Decision d6 = Base(6, "2026-10-09 12:04:34.000");
            d6.outcome = Outcome::Nothing;
            d6.ctx = lap;
            d6.ctxAgeMs = 64000.0f;
            d6.need = static_cast<int>(N(Needs::NeedId::diseased));
            d6.durSec = 60.0;
            d6.peak = 1.0f;
            d6.onsetUtc = "2026-10-09 12:02:00.000";
            d6.lap = 1;
            d6.byLap = true;
            d6.episodeSec = 150.0;
            decisions.push_back(d6);
        }

        std::string Encode() const
        {
            Encoder e;
            std::string out = e.BeginSegment(MakeHead());
            for (const auto& d : decisions) out += e.Encode(d);
            // A new segment (a rotated file): the first context is defined again,
            // and cap ids start over.
            out += e.BeginSegment(MakeHead());
            Decision again = decisions[0];
            again.seq = 7;
            again.utc = "2026-10-09 13:00:00.000";
            out += e.Encode(again);
            return out;
        }
    };

    std::string ReadFile(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string s = ss.str();
        // .gitattributes keeps *.jsonl LF; a checkout that converted anyway
        // should still compare.
        std::erase(s, '\r');
        return s;
    }
}

TEST_CASE("decision log: JSON helpers")
{
    CHECK(JsonString("a\"b\\c\n") == "\"a\\\"b\\\\c\\n\"");
    CHECK(JsonString("\xC3\x89p\xC3\xA9" "e") == "\"\xC3\x89p\xC3\xA9" "e\"");   // valid UTF-8 kept
    CHECK(JsonString("\xC9p\xE9" "e") == "\"\\u00c9p\\u00e9e\"");             // cp1252 escaped
    std::string s;
    AppendNum(s, 0.123456f);
    CHECK(s == "0.1235");
    s.clear();
    AppendNum(s, kNone);
    CHECK(s == "null");
    s.clear();
    AppendNum(s, -0.0f);
    CHECK(s == "0");
    CHECK(FormHex(0x3EADE) == "0003EADE");
    CHECK(FormHex(0xFF000801) == "FF000801");
}

TEST_CASE("decision log: the run gate keeps a previous session's runs out")
{
    RunGate gate;
    CHECK(gate.IsCurrent(0));   // never reset: every run counts
    CHECK(gate.IsCurrent(7));
    gate.Reset(7);              // a load while the cache holds run 7 of the old save
    CHECK_FALSE(gate.IsCurrent(7));
    CHECK_FALSE(gate.IsCurrent(3));
    CHECK(gate.IsCurrent(8));   // the new session's first run
    gate.Reset(8);              // a second load in the same launch
    CHECK_FALSE(gate.IsCurrent(8));
    CHECK(gate.IsCurrent(9));
}

TEST_CASE("decision log: caps and contexts are defined once per segment")
{
    const Synthetic syn;
    Encoder e;
    (void)e.BeginSegment(Synthetic::MakeHead());
    const std::string first = e.Encode(syn.decisions[0]);
    CHECK(first.find(R"({"t":"ctx","id":1,)") != std::string::npos);
    CHECK(first.find(R"({"t":"cap","id":0,)") != std::string::npos);
    const std::string again = e.Encode(syn.decisions[4]);   // same context
    CHECK(again.find(R"("t":"ctx")") == std::string::npos);
    CHECK(again.find(R"("t":"cap")") == std::string::npos);
    CHECK(again.rfind(R"({"t":"dec")", 0) == 0);
    // The armour pick's added row reuses the boots' cap id (content-addressed).
    const std::string menu = e.Encode(syn.decisions[1]);
    CHECK(menu.find(R"("t":"cap")") == std::string::npos);
}

TEST_CASE("decision log: the old engine's columns end at the last one set (0.23.53)")
{
    const auto encodeRow = [](const Row& r) {
        Encoder e;
        (void)e.BeginSegment(Synthetic::MakeHead());
        auto ctx = std::make_shared<Context>();
        ctx->id = 9;
        ctx->rows.push_back(r);
        Decision d;
        d.ctx = ctx;
        const std::string out = e.Encode(d);
        const auto at = out.find(R"("rows":[[)");
        REQUIRE(at != std::string::npos);
        const auto end = out.find("]]}", at);
        return out.substr(at + 8, end + 1 - (at + 8));   // the row, brackets included
    };
    Row r;
    r.form = 0x12;
    r.flags = Flag::Held;
    // No old-engine part: the row ends after cr, as before 0.23.53.
    CHECK(encodeRow(r) == R"(["00000012",0,null,null,-1,4,-1,null,[],null,null,null])");
    r.old.est = 0.0f;
    r.old.alpha = 0.0f;
    r.old.trains = 0.0f;
    r.old.ucb = 1.0f;
    r.old.rec = 0.0f;
    CHECK(encodeRow(r) == R"(["00000012",0,null,null,-1,4,-1,null,[],null,null,null,0,0,0,1,0])");
    r.old.floor = static_cast<std::uint8_t>(Floor::ctx);   // a floor with no prior/cw: nulls before it
    CHECK(encodeRow(r) == R"(["00000012",0,null,null,-1,4,-1,null,[],null,null,null,0,0,0,1,0,null,null,2])");
    r.old.floor = kNoFloor;
    r.hotkey = 0;
    CHECK(encodeRow(r) == R"(["00000012",0,null,null,-1,4,-1,null,[],null,null,null,0,0,0,1,0,null,null,null,null,0])");
    // The head names the columns and the floors.
    const std::string head = Encoder().BeginSegment(Synthetic::MakeHead());
    CHECK(head.find(R"("fb","cr","est","a","n","ucb","rec","prior","cw","fl","wu","hk"])") != std::string::npos);
    CHECK(head.find(R"("addedAtPick":256,"favorite":512})") != std::string::npos);
    CHECK(head.find(R"("floors":["pass","cold","ctx","util","suppressed"])") != std::string::npos);
    // 0.23.55: covered(i)'s left(r) centre, [Scoring] fRefreshPriorSec.
    CHECK(head.find(R"(,"refreshPriorSec":5.000)") != std::string::npos);
    Head h = Synthetic::MakeHead();
    h.refreshPriorSec = 3.0f;
    CHECK(Encoder().BeginSegment(h).find(R"(,"refreshPriorSec":3.000)") != std::string::npos);
}

TEST_CASE("decision log: a pick carries the v2 record id; nothing carries none (0.23.53)")
{
    const Synthetic syn;
    Encoder e;
    (void)e.BeginSegment(Synthetic::MakeHead());
    CHECK(e.Encode(syn.decisions[0]).find(R"("seq":1,"rid":41,)") != std::string::npos);
    CHECK(e.Encode(syn.decisions[1]).find(R"("seq":2,"rid":null,)") != std::string::npos);   // the learner skipped it
    CHECK(e.Encode(syn.decisions[3]).find(R"("seq":4,"rid":null,)") != std::string::npos);   // nothing
}

TEST_CASE("decision log: the synthetic session matches the golden file")
{
    const Synthetic syn;
    const std::string actual = syn.Encode();
    const std::string goldenPath = std::string(HUGINN_REPO_ROOT) + "/tests/core/fixtures/decisions/synthetic_v3.jsonl";
    const std::string golden = ReadFile(goldenPath);
    if (actual != golden) {
        std::ofstream("synthetic_v3.actual.jsonl", std::ios::binary) << actual;
    }
    CHECK_MESSAGE(actual == golden, "encoder output differs from " << goldenPath
                                                                   << " (actual written to synthetic_v3.actual.jsonl in the working directory)");
    // Every line is one JSON object.
    std::istringstream lines(actual);
    std::string line;
    std::size_t n = 0;
    while (std::getline(lines, line)) {
        ++n;
        CHECK(line.front() == '{');
        CHECK(line.back() == '}');
    }
    CHECK(n == 25);   // 23 before 0.23.51's lap (a context and a decision); 21 before 0.23.46's summon row added a cap to each segment
}
