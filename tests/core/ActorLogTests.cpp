// Host tests for src/core/ActorLog.h.

#include "core/ActorLog.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

using namespace Huginn::Core::ActorLog;
using Huginn::State::TargetType;

namespace
{
    Observation Obs(std::uint32_t ref, bool hostile, TargetType type, Band band = Band::Mid, bool dead = false)
    {
        Observation o;
        o.refID = ref;
        o.hostile = hostile;
        o.type = type;
        o.band = band;
        o.dead = dead;
        return o;
    }

    struct Fixture
    {
        Tracker tracker;
        std::map<std::uint32_t, Identity> ids;
        int identifyCalls = 0;

        explicit Fixture(Tracker::Config config = {}) : tracker(config) {}

        std::vector<std::string> Poll(double now, bool fighting, const std::vector<Observation>& obs,
                                      std::uint32_t target = 0, TargetType type = TargetType::None)
        {
            std::vector<std::string> out;
            tracker.Poll(now, fighting, obs, target, type,
                [&](std::uint32_t ref) {
                    ++identifyCalls;
                    auto it = ids.find(ref);
                    return it != ids.end() ? it->second : Identity{ "?", 0, "", "", false, "" };
                },
                out);
            return out;
        }
    };

    bool Contains(const std::vector<std::string>& lines, const std::string& needle)
    {
        for (const auto& l : lines) {
            if (l.find(needle) != std::string::npos) return true;
        }
        return false;
    }
}

TEST_CASE("actor log: distance bands are the evaluator's edges")
{
    constexpr float closeSq = 256.0f * 256.0f;
    constexpr float midSq = 768.0f * 768.0f;
    CHECK(BandOf(100.0f * 100.0f, closeSq, midSq) == Band::Close);
    CHECK(BandOf(closeSq, closeSq, midSq) == Band::Close);
    CHECK(BandOf(500.0f * 500.0f, closeSq, midSq) == Band::Mid);
    CHECK(BandOf(2000.0f * 2000.0f, closeSq, midSq) == Band::Far);
    CHECK(BandOf(0.0f, closeSq, midSq) == Band::Far);   // the "no distance yet" sentinel
    CHECK(BandName(Band::Close) == "close");
}

TEST_CASE("actor log: summon field and hg actors row")
{
    Identity id{ "Flame Atronach", 0x204C7, "AtronachFlameRace", "Flame Atronach", true, "Bandit Mage" };
    CHECK(SummonField(id) == "'Bandit Mage'");
    id.commander.clear();
    CHECK(SummonField(id) == "?");
    id.summon = false;
    CHECK(SummonField(id) == "-");

    Identity bandit{ "Bandit", 0x3DF08, "NordRace", "Nord", false, "" };
    CHECK(FormatRow(Obs(0xFF001234, true, TargetType::Humanoid, Band::Close), bandit, true) ==
          "FF001234 'Bandit' race=NordRace type=Humanoid hostile=1 dead=0 summon=- band=close [scoring target]");
}

TEST_CASE("actor log: nothing is tracked or logged outside a fight")
{
    Fixture f;
    CHECK(f.Poll(0.0, false, { Obs(1, false, TargetType::Humanoid) }).empty());
    CHECK(f.identifyCalls == 0);
    CHECK_FALSE(f.tracker.InFight());
    CHECK(f.tracker.SeenCount() == 0);
}

TEST_CASE("actor log: identity once, then transitions only")
{
    Fixture f;
    f.ids[0xA1] = Identity{ "Bandit Marauder", 0x3DF08, "NordRace", "Nord", false, "" };

    auto lines = f.Poll(0.0, true, { Obs(0xA1, true, TargetType::Humanoid, Band::Mid) });
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "[Actors] fight started (1 tracked)");
    CHECK(lines[1] == "[Actors] +000000A1 'Bandit Marauder' race=NordRace type=Humanoid hostile=1 summon=- band=mid");

    // Same state, a different band: nothing (distance is not a transition).
    CHECK(f.Poll(0.1, true, { Obs(0xA1, true, TargetType::Humanoid, Band::Close) }).empty());
    CHECK(f.identifyCalls == 1);

    lines = f.Poll(0.2, true, { Obs(0xA1, false, TargetType::Humanoid) });
    CHECK(lines == std::vector<std::string>{ "[Actors] 000000A1 hostile 1->0" });

    lines = f.Poll(0.3, true, { Obs(0xA1, false, TargetType::Daedra) });
    CHECK(lines == std::vector<std::string>{ "[Actors] 000000A1 type Humanoid->Daedra" });

    auto dead = Obs(0xA1, false, TargetType::Daedra, Band::Mid, true);
    dead.countedAlive = true;
    lines = f.Poll(0.4, true, { dead });
    CHECK(lines == std::vector<std::string>{ "[Actors] 000000A1 died (the target list still counts it as a living hostile)" });
    CHECK(f.Poll(0.5, true, { dead }).empty());   // once

    lines = f.Poll(0.6, true, { Obs(0xA1, false, TargetType::Daedra) });
    CHECK(lines == std::vector<std::string>{ "[Actors] 000000A1 revived" });
}

TEST_CASE("actor log: leaving needs the grace; a flap inside it is silent")
{
    Fixture f({ 4096, 2.0 });
    f.ids[1] = Identity{ "Wolf", 0x23AB7, "WolfRace", "Wolf", false, "" };
    f.Poll(0.0, true, { Obs(1, true, TargetType::Beast) });

    CHECK(f.Poll(0.1, true, {}).empty());          // gone one poll
    CHECK(f.Poll(1.0, true, { Obs(1, true, TargetType::Beast) }).empty());   // back inside the grace
    CHECK(f.Poll(1.1, true, {}).empty());
    CHECK(f.Poll(3.0, true, {}).empty());          // 1.9 s absent
    auto lines = f.Poll(3.2, true, {});
    CHECK(lines == std::vector<std::string>{ "[Actors] 00000001 left tracking" });
    CHECK(f.Poll(9.0, true, {}).empty());          // once

    // Back after its left line: a short "again", no new identity read.
    lines = f.Poll(10.0, true, { Obs(1, true, TargetType::Beast, Band::Far) });
    CHECK(lines == std::vector<std::string>{ "[Actors] again 00000001 'Wolf' hostile=1 band=far" });
    CHECK(f.identifyCalls == 1);
}

TEST_CASE("actor log: an actor met earlier in the session gets a short line in the next fight")
{
    Fixture f;
    f.ids[7] = Identity{ "Lydia", 0xA2C94, "NordRace", "Nord", false, "" };
    f.Poll(0.0, true, { Obs(7, false, TargetType::Humanoid) });
    auto lines = f.Poll(30.0, false, {});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].starts_with("[Actors] fight ended after 30s: 1 actors (Humanoid=1)"));

    lines = f.Poll(100.0, true, { Obs(7, false, TargetType::Humanoid, Band::Close) });
    REQUIRE(lines.size() == 2);
    CHECK(lines[1] == "[Actors] again 00000007 'Lydia' hostile=0 band=close");
    CHECK(f.tracker.SeenCount() == 1);

    // A load forgets the session: FF refIDs mean other actors in another save.
    f.tracker.Reset();
    CHECK(f.tracker.SeenCount() == 0);
    CHECK_FALSE(f.tracker.InFight());
    lines = f.Poll(200.0, true, { Obs(7, false, TargetType::Humanoid) });
    CHECK(Contains(lines, "[Actors] +00000007 'Lydia'"));
}

TEST_CASE("actor log: the seen set is bounded, least recently met dropped first")
{
    SeenSet seen(3);
    CHECK_FALSE(seen.Touch(1));
    CHECK_FALSE(seen.Touch(2));
    CHECK_FALSE(seen.Touch(3));
    CHECK(seen.Touch(1));          // 1 is now the most recent
    CHECK_FALSE(seen.Touch(4));    // drops 2
    CHECK(seen.Size() == 3);
    CHECK_FALSE(seen.Contains(2));
    CHECK(seen.Contains(1));
    CHECK(seen.Contains(3));
    CHECK(seen.Contains(4));

    Fixture f({ 2, 2.0 });
    f.Poll(0.0, true, { Obs(1, true, TargetType::Humanoid), Obs(2, true, TargetType::Humanoid),
                        Obs(3, true, TargetType::Humanoid) });
    CHECK(f.tracker.SeenCount() == 2);
}

TEST_CASE("actor log: target switches and type flips in the summary")
{
    Fixture f;
    f.ids[1] = Identity{ "Bandit", 0x1, "NordRace", "Nord", false, "" };
    f.ids[2] = Identity{ "Dremora", 0x2, "DremoraRace", "Dremora", true, "Bandit Conjurer" };
    const std::vector<Observation> both{ Obs(1, true, TargetType::Humanoid), Obs(2, true, TargetType::Daedra) };

    f.Poll(0.0, true, both, 1, TargetType::Humanoid);
    f.Poll(0.1, true, both, 2, TargetType::Daedra);       // switch + flip
    f.Poll(0.2, true, both, 0, TargetType::None);         // a gap hides nothing
    f.Poll(0.3, true, both, 1, TargetType::Humanoid);     // switch + flip
    f.Poll(0.4, true, both, 1, TargetType::Humanoid);
    auto lines = f.Poll(65.0, false, {});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "[Actors] fight ended after 65s: 2 actors (Humanoid=1 Daedra=1), 1 summons, 2 hostile, 0 died, "
                      "target switches=2 type flips=2");
}

TEST_CASE("actor log: a 5-minute fight with six bandits and a summon stays inside the line budget")
{
    // Six bandits arrive in two waves, a conjurer's Dremora joins, the
    // follower is there all along; bandits die one by one, the corpses leave
    // tracking as the player moves on. Polled at 10 Hz like PollTargets.
    Fixture f;
    f.ids[100] = Identity{ "Lydia", 0xA2C94, "NordRace", "Nord", false, "" };
    for (std::uint32_t b = 1; b <= 6; ++b) f.ids[b] = Identity{ "Bandit", 0x3DF08, "NordRace", "Nord", false, "" };
    f.ids[50] = Identity{ "Dremora Churl", 0x16F80, "DremoraRace", "Dremora", true, "Bandit Conjurer" };

    std::size_t total = 0;
    for (int tick = 0; tick <= 3000; ++tick) {
        const double t = tick * 0.1;
        std::vector<Observation> obs{ Obs(100, false, TargetType::Humanoid, Band::Close) };
        for (std::uint32_t b = 1; b <= 6; ++b) {
            const double arrive = b <= 3 ? 0.0 : 60.0;
            const double dies = 40.0 * b;
            const double gone = dies + 20.0;
            if (t < arrive || t >= gone) continue;
            obs.push_back(Obs(b, true, TargetType::Humanoid, (tick / 7 + b) % 2 ? Band::Close : Band::Mid, t >= dies));
        }
        if (t >= 90.0 && t < 150.0) obs.push_back(Obs(50, true, TargetType::Daedra, Band::Mid));
        // The scoring target hops between the two closest every ~4 s.
        const std::uint32_t target = (tick / 40) % 2 ? 1 + (tick / 80) % 6 : 50;
        total += f.Poll(t, true, obs, target, target == 50 ? TargetType::Daedra : TargetType::Humanoid).size();
    }
    total += f.Poll(301.0, false, {}).size();
    // start 1 + identities 8 + deaths 6 + left 7 + summary 1 = 23
    CHECK(total >= 20);
    CHECK(total <= 40);
}

TEST_CASE("actor log: identity and transition lines are capped per fight; the summary counts the rest")
{
    Tracker::Config config;
    config.lineCap = 3;
    Fixture f(config);
    std::vector<Observation> crowd;
    for (std::uint32_t r = 1; r <= 6; ++r) crowd.push_back(Obs(r, false, TargetType::Humanoid));
    auto lines = f.Poll(0.0, true, crowd);
    // started + 3 identities + the one cap line; 3 more identities suppressed
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "[Actors] fight started (6 tracked)");
    CHECK(lines[4] == "[Actors] cap reached (3 lines this fight), further lines suppressed");
    CHECK(f.Poll(1.0, true, {}).empty());
    lines = f.Poll(4.0, true, {});   // six left lines, all suppressed, no second cap line
    CHECK(lines.empty());
    lines = f.Poll(5.0, false, {});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].ends_with("target switches=0 type flips=0, 9 lines suppressed"));

    // The next fight starts with a fresh budget. Actor 1's identity line went
    // out, so it is "again"; actor 4's was held back by the cap, so it was
    // never marked as met and gets its identity line now.
    lines = f.Poll(10.0, true, { Obs(1, true, TargetType::Humanoid), Obs(4, false, TargetType::Humanoid) });
    REQUIRE(lines.size() == 3);
    CHECK(lines[1].starts_with("[Actors] again 00000001 "));
    CHECK(lines[2].starts_with("[Actors] +00000004 "));
    // ... and the held-back count did not carry into this fight's summary.
    lines = f.Poll(20.0, false, {});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].ends_with("target switches=0 type flips=0"));
}

namespace
{
    TargetInfo Info(std::uint32_t ref, const char* name, TargetType type, const char* rule = "closest")
    {
        TargetInfo t;
        t.refID = ref;
        t.name = name;
        t.baseID = 0x3DF08;
        t.raceEditorID = "NordRace";
        t.raceName = "Nord";
        t.type = type;
        t.rule = rule;
        t.hostile = ref != 0;
        return t;
    }

    // Polls `watch` at 10 Hz from `t` with (ref, type) until it asks for a
    // Commit; returns the time it did, or -1 within `maxPolls`.
    double PollUntilDue(TargetWatch& watch, double t, std::uint32_t ref, TargetType type, int maxPolls = 10)
    {
        for (int i = 0; i < maxPolls; ++i, t += 0.1) {
            if (watch.Observe(t, ref, type)) return t;
        }
        return -1.0;
    }
}

TEST_CASE("target watch: a new pick is logged once it has held about 300 ms (3 polls)")
{
    TargetWatch w;
    CHECK_FALSE(w.Observe(0.0, 0, TargetType::None));   // no target, as at start: nothing
    CHECK_FALSE(w.Observe(1.0, 0xA1, TargetType::Humanoid));
    CHECK_FALSE(w.Observe(1.1, 0xA1, TargetType::Humanoid));
    CHECK_FALSE(w.Observe(1.2, 0xA1, TargetType::Humanoid));
    CHECK(w.Observe(1.3, 0xA1, TargetType::Humanoid));
    CHECK(w.Commit(Info(0xA1, "Bandit", TargetType::Humanoid, "crosshair")) ==
          "[Target] 000000A1 'Bandit' base=0003DF08 race=NordRace 'Nord' type=Humanoid rule=crosshair hostile=1 dead=0 summon=-");
    // Logged: the same pick again is quiet.
    CHECK_FALSE(w.Observe(1.4, 0xA1, TargetType::Humanoid));
    CHECK_FALSE(w.Observe(9.0, 0xA1, TargetType::Humanoid));
}

TEST_CASE("target watch: Differs-by-type -- a type change on the same actor is a change, with its own line")
{
    TargetWatch w;
    REQUIRE(PollUntilDue(w, 0.0, 0xA1, TargetType::Humanoid) > 0.0);
    (void)w.Commit(Info(0xA1, "Bandit", TargetType::Humanoid));

    const double due = PollUntilDue(w, 5.0, 0xA1, TargetType::Daedra);
    REQUIRE(due > 0.0);
    CHECK(due == doctest::Approx(5.3));
    CHECK(w.Commit(Info(0xA1, "Bandit", TargetType::Daedra)) ==
          "[Target] 000000A1 'Bandit' base=0003DF08 race=NordRace 'Nord' type=Daedra rule=closest hostile=1 dead=0 "
          "summon=- -- same actor, type Humanoid->Daedra (classifier flicker)");
}

TEST_CASE("target watch: a switch names the previous target; losing it says none")
{
    TargetWatch w;
    REQUIRE(PollUntilDue(w, 0.0, 0xA1, TargetType::Humanoid) > 0.0);
    (void)w.Commit(Info(0xA1, "Bandit", TargetType::Humanoid));

    REQUIRE(PollUntilDue(w, 2.0, 0xB2, TargetType::Daedra) > 0.0);
    CHECK(w.Commit(Info(0xB2, "Dremora", TargetType::Daedra)).ends_with(" (was 000000A1 'Bandit' type=Humanoid)"));

    REQUIRE(PollUntilDue(w, 4.0, 0, TargetType::None) > 0.0);
    CHECK(w.Commit(TargetInfo{}) == "[Target] none (was 000000B2 'Dremora' type=Daedra)");
    CHECK_FALSE(w.Observe(6.0, 0, TargetType::None));
}

TEST_CASE("target watch: one-poll excursions from the logged pick are quiet, and the next line counts them")
{
    TargetWatch w;
    REQUIRE(PollUntilDue(w, 0.0, 0xA1, TargetType::Humanoid) > 0.0);
    (void)w.Commit(Info(0xA1, "Bandit", TargetType::Humanoid));

    // B for one poll, then A long enough to hold again (0.3 s), five times.
    double t = 1.0;
    for (int k = 0; k < 5; ++k) {
        CHECK_FALSE(w.Observe(t, 0xB2, TargetType::Humanoid));
        t += 0.1;
        for (int i = 0; i < 4; ++i, t += 0.1) CHECK_FALSE(w.Observe(t, 0xA1, TargetType::Humanoid));
    }
    CHECK(w.PendingBrief() == 5);
    // A third pick replacing a pending one before it held is brief too.
    CHECK_FALSE(w.Observe(t, 0xB2, TargetType::Humanoid));
    CHECK_FALSE(w.Observe(t + 0.1, 0xC3, TargetType::Beast));
    CHECK(w.PendingBrief() == 6);
    // C3 holds: logged, with the count, which then resets.
    REQUIRE(PollUntilDue(w, t + 0.2, 0xC3, TargetType::Beast) > 0.0);
    CHECK(w.Commit(Info(0xC3, "Wolf", TargetType::Beast)).ends_with(" (was 000000A1 'Bandit' type=Humanoid) (+6 brief switches)"));
    CHECK(w.PendingBrief() == 0);

    // One brief switch reads singular.
    CHECK_FALSE(w.Observe(20.0, 0xA1, TargetType::Humanoid));
    for (double u = 20.1; u < 20.55; u += 0.1) CHECK_FALSE(w.Observe(u, 0xC3, TargetType::Beast));
    REQUIRE(PollUntilDue(w, 21.0, 0, TargetType::None) > 0.0);
    CHECK(w.Commit(TargetInfo{}).ends_with(" (+1 brief switch)"));
}

namespace
{
    // Feeds `pick(i)` at 10 Hz from `t0` for `seconds`; returns the times a
    // Commit was asked for (and commits each, so the watch moves on).
    template <class PickFn>
    std::vector<double> Drive(TargetWatch& w, double t0, double seconds, PickFn&& pick)
    {
        std::vector<double> commits;
        const int polls = static_cast<int>(seconds * 10.0 + 0.5);
        for (int i = 0; i < polls; ++i) {
            const double t = t0 + i * 0.1;
            const auto [ref, type] = pick(i);
            if (w.Observe(t, ref, type)) {
                commits.push_back(t);
                (void)w.Commit(ref ? Info(ref, "X", type) : TargetInfo{});
            }
        }
        return commits;
    }
}

TEST_CASE("target watch: the ceiling -- a lasting change through flicker is logged within CEILING_SEC")
{
    using Pick = std::pair<std::uint32_t, TargetType>;
    const Pick a{ 0xA1, TargetType::Humanoid };
    const Pick b{ 0xB2, TargetType::Daedra };
    const Pick none{ 0, TargetType::None };

    SUBCASE("A/B every poll, from none")
    {
        TargetWatch w;
        const auto commits = Drive(w, 0.0, 20.0, [&](int i) { return i % 2 ? b : a; });
        REQUIRE_FALSE(commits.empty());
        // Literal times: the ceiling is 1.0 s, and the commit is the poll at it.
        CHECK(commits.front() >= 0.95);
        CHECK(commits.front() <= 1.05);
        // Rate-capped: at most about one line per ceiling window.
        CHECK(commits.size() <= 21);
        CHECK(w.Current().refID != 0);   // never "none" all fight
    }
    SUBCASE("A,A,B,B runs, from none")
    {
        TargetWatch w;
        const auto commits = Drive(w, 0.0, 20.0, [&](int i) { return (i / 2) % 2 ? b : a; });
        REQUIRE_FALSE(commits.empty());
        CHECK(commits.front() >= 0.95);
        CHECK(commits.front() <= 1.05);
        CHECK(commits.size() <= 21);
    }
    SUBCASE("an edge-of-range hostile: there two polls, gone one")
    {
        TargetWatch w;
        const auto commits = Drive(w, 0.0, 30.0, [&](int i) { return i % 3 == 2 ? none : a; });
        REQUIRE_FALSE(commits.empty());
        CHECK(commits.front() >= 0.95);                              // the first poll with A at or past 1.0 s
        CHECK(commits.front() <= 1.15);
        CHECK(commits.size() <= 31);
    }
    SUBCASE("a stable new pick still logs at the dwell, not the ceiling")
    {
        TargetWatch w;
        const auto commits = Drive(w, 0.0, 2.0, [&](int) { return a; });
        REQUIRE(commits.size() == 1);
        CHECK(commits.front() == doctest::Approx(0.3));
    }
}

TEST_CASE("target watch: the dwell boundary is DWELL_SEC (with the poll tolerance)")
{
    TargetWatch w;
    CHECK_FALSE(w.Observe(1.0, 0xA1, TargetType::Humanoid));
    CHECK_FALSE(w.Observe(1.0 + TargetWatch::DWELL_SEC - 0.05, 0xA1, TargetType::Humanoid));
    CHECK(w.Observe(1.0 + TargetWatch::DWELL_SEC, 0xA1, TargetType::Humanoid));
}

TEST_CASE("target watch: the candidate is actor AND type -- a type change restarts the dwell")
{
    TargetWatch w;
    CHECK_FALSE(w.Observe(1.0, 0xA1, TargetType::Humanoid));
    CHECK_FALSE(w.Observe(1.1, 0xA1, TargetType::Daedra));   // a new candidate, the first one brief
    CHECK(w.PendingBrief() == 1);
    CHECK_FALSE(w.Observe(1.3, 0xA1, TargetType::Daedra));   // held 0.2 s, not 0.4
    CHECK(w.Observe(1.4, 0xA1, TargetType::Daedra));
}

TEST_CASE("target watch: Reset drops a pending candidate and the ceiling clock")
{
    SUBCASE("the candidate")
    {
        TargetWatch w;
        CHECK_FALSE(w.Observe(1.0, 0xA1, TargetType::Humanoid));
        CHECK_FALSE(w.Observe(1.1, 0xA1, TargetType::Humanoid));
        w.Reset();
        CHECK_FALSE(w.Observe(1.3, 0xA1, TargetType::Humanoid));   // a fresh candidate, not 0.3 s old
        CHECK_FALSE(w.Observe(1.5, 0xA1, TargetType::Humanoid));
        CHECK(w.Observe(1.6, 0xA1, TargetType::Humanoid));
    }
    SUBCASE("the ceiling clock: after Reset only a full 1.0 s of flicker commits")
    {
        TargetWatch w;
        CHECK_FALSE(w.Observe(0.0, 0xA1, TargetType::Humanoid));   // the ceiling clock starts at 0.0
        CHECK_FALSE(w.Observe(0.1, 0xB2, TargetType::Humanoid));
        w.Reset();
        // A/B flicker from 0.5: no candidate ever holds, so only the ceiling
        // can commit -- at 1.5 if Reset cleared the clock, at 1.0 if not.
        double t = 0.5;
        for (int i = 0; t < 1.45; ++i, t += 0.1) {
            CHECK_FALSE(w.Observe(t, i % 2 ? 0xB2u : 0xA1u, TargetType::Humanoid));
        }
        CHECK(w.Observe(1.5, 0xA1, TargetType::Humanoid));
    }
}

TEST_CASE("target watch: BRIEF_STALE_SEC at its boundary, from the newest brief switch")
{
    TargetWatch w;
    REQUIRE(PollUntilDue(w, 0.0, 0xA1, TargetType::Humanoid) > 0.0);
    (void)w.Commit(Info(0xA1, "Bandit", TargetType::Humanoid));
    CHECK_FALSE(w.Observe(1.0, 0xB2, TargetType::Humanoid));
    CHECK_FALSE(w.Observe(1.1, 0xA1, TargetType::Humanoid));   // the brief switch, at 1.1
    REQUIRE(w.PendingBrief() == 1);
    CHECK_FALSE(w.Observe(6.0, 0xA1, TargetType::Humanoid));   // 4.9 s after it: kept
    CHECK(w.PendingBrief() == 1);
    CHECK_FALSE(w.Observe(6.2, 0xA1, TargetType::Humanoid));   // 5.1 s after it: dropped
    CHECK(w.PendingBrief() == 0);
}

TEST_CASE("target watch: a brief count older than BRIEF_STALE_SEC does not ride on a later line")
{
    TargetWatch w;
    REQUIRE(PollUntilDue(w, 0.0, 0xA1, TargetType::Humanoid) > 0.0);
    (void)w.Commit(Info(0xA1, "Bandit", TargetType::Humanoid));
    CHECK_FALSE(w.Observe(1.0, 0xB2, TargetType::Humanoid));
    for (double u = 1.1; u < 1.55; u += 0.1) CHECK_FALSE(w.Observe(u, 0xA1, TargetType::Humanoid));
    CHECK(w.PendingBrief() == 1);

    // Minutes later, a real change: the old flicker is not on its line.
    REQUIRE(PollUntilDue(w, 120.0, 0xC3, TargetType::Beast) > 0.0);
    CHECK(w.PendingBrief() == 0);
    const auto line = w.Commit(Info(0xC3, "Wolf", TargetType::Beast));
    CHECK(line.ends_with(" (was 000000A1 'Bandit' type=Humanoid)"));
}

TEST_CASE("target watch: a type flicker shorter than the dwell is not logged; Reset forgets everything")
{
    TargetWatch w;
    REQUIRE(PollUntilDue(w, 0.0, 0xA1, TargetType::Humanoid) > 0.0);
    (void)w.Commit(Info(0xA1, "Bandit", TargetType::Humanoid));
    CHECK_FALSE(w.Observe(1.0, 0xA1, TargetType::Daedra));
    CHECK_FALSE(w.Observe(1.1, 0xA1, TargetType::Daedra));
    CHECK_FALSE(w.Observe(1.2, 0xA1, TargetType::Humanoid));
    CHECK(w.PendingBrief() == 1);

    w.Reset();
    CHECK(w.PendingBrief() == 0);
    CHECK(w.Current().refID == 0);
    CHECK_FALSE(w.Observe(2.0, 0, TargetType::None));
}

TEST_CASE("actor log: the fight summary counts every switch and flip, brief ones included")
{
    // The [Target] dwell does not touch the summary: A/B/A/B at the poll rate
    // logs no [Target] line, and the summary still counts each change.
    Fixture f;
    const std::vector<Observation> both{ Obs(1, true, TargetType::Humanoid), Obs(2, true, TargetType::Daedra) };
    for (int i = 0; i < 6; ++i) {
        f.Poll(i * 0.1, true, both, i % 2 ? 2u : 1u, i % 2 ? TargetType::Daedra : TargetType::Humanoid);
    }
    const auto lines = f.Poll(10.0, false, {});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].ends_with("target switches=5 type flips=5"));
}

TEST_CASE("actor log: an actor capped in a fight, that leaves and returns while capped, stays unmet")
{
    Tracker::Config config;
    config.lineCap = 1;
    Fixture f(config);
    // Fight 1: X's identity line uses the budget; Y's is held back.
    auto lines = f.Poll(0.0, true, { Obs(0x10, true, TargetType::Humanoid), Obs(0x20, true, TargetType::Humanoid) });
    CHECK(Contains(lines, "[Actors] +00000010"));
    CHECK_FALSE(Contains(lines, "+00000020"));
    // Y leaves (its left line held back too) and comes back, still capped.
    f.Poll(0.1, true, { Obs(0x10, true, TargetType::Humanoid) });
    f.Poll(3.0, true, { Obs(0x10, true, TargetType::Humanoid) });
    lines = f.Poll(4.0, true, { Obs(0x10, true, TargetType::Humanoid), Obs(0x20, true, TargetType::Humanoid) });
    CHECK(lines.empty());
    f.Poll(10.0, false, {});

    // Fight 2: Y was never shown, so it gets its identity line now.
    lines = f.Poll(20.0, true, { Obs(0x20, true, TargetType::Humanoid) });
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].starts_with("[Actors] +00000020 "));
}

TEST_CASE("actor log: an actor already met keeps its seen entry fresh while the cap holds its line")
{
    Tracker::Config config;
    config.lineCap = 1;
    config.seenCap = 2;
    Fixture f(config);
    f.Poll(0.0, true, { Obs(1, false, TargetType::Humanoid) });                  // 1 met
    f.Poll(1.0, false, {});
    // Fight 2: 2's identity line uses the budget; 1's "again" is held back,
    // but 1 is touched after 2 -- the most recently met.
    auto lines = f.Poll(2.0, true, { Obs(2, false, TargetType::Humanoid), Obs(1, false, TargetType::Humanoid) });
    CHECK(Contains(lines, "[Actors] +00000002"));
    CHECK_FALSE(Contains(lines, "again 00000001"));
    f.Poll(3.0, false, {});
    // Fight 3: 3 is met, and the seen set (2 entries) drops its oldest: 2, not 1.
    f.Poll(4.0, true, { Obs(3, false, TargetType::Humanoid) });
    f.Poll(5.0, false, {});
    lines = f.Poll(6.0, true, { Obs(1, false, TargetType::Humanoid) });
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].starts_with("[Actors] again 00000001 "));
    f.Poll(7.0, false, {});
    lines = f.Poll(8.0, true, { Obs(2, false, TargetType::Humanoid) });
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].starts_with("[Actors] +00000002 "));
}
