// core/DecisionLog.h: the selection log v3 encoder.
//
// The golden file tests/core/fixtures/decisions/synthetic_v3.jsonl is what
// this encoder makes of the synthetic decisions below, byte for byte. The
// replay side reads the same file (tools/replay/test_replay_v3.py checks every
// value it decodes against the numbers written here), so the two ends of the
// format are pinned to one file. To regenerate after a deliberate format
// change: run the tests, copy <build>/tests/synthetic_v3.actual.jsonl over the
// fixture, and update the Python test's expectations.

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

        std::shared_ptr<const Context> press, menu, onset;
        std::vector<Decision> decisions;

        static Head MakeHead()
        {
            Head h;
            h.launch = "20261009-120000";
            h.list = "synthetic";
            h.build = "0.23.15 (test)";
            h.sourceNames = { "Spell", "Potion", "Scroll", "Weapon", "Ammo", "SoulGem", "Food", "Staff", "Apparel", "Torch" };
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
                p->rows.push_back(r);
            }
            {
                Row r;   // 3: worn boots, not a candidate
                r.form = 0x00013911;
                r.kind = static_cast<std::uint8_t>(Effect::Kind::Armour);
                r.flags = Flag::Held | Flag::Equipped;
                r.cap = &boots;
                p->rows.push_back(r);
            }
            {
                Row r;   // 4: an item outside the catalog
                r.form = 0xFF000801;
                r.flags = Flag::Held;
                p->rows.push_back(r);
            }
            press = p;

            auto m = std::make_shared<Context>(*p);   // the inventory opened a second later
            m->id = 2;
            m->utc = "2026-10-09 12:00:02.000";
            m->why = "menu";
            m->menu = "InventoryMenu";
            m->rows.resize(2);   // potion and spell only
            m->rows[1].flags = Flag::Eligible | Flag::Scored | Flag::Held;   // the wildcard ...
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
            decisions.push_back(d5);
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
            again.seq = 6;
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
    CHECK(n == 21);
}
