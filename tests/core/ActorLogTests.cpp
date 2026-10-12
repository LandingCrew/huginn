// Host tests for src/core/ActorLog.h.

#include "core/ActorLog.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
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
