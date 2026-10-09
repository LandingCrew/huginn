// Host tests for src/core/SlotSnapshotIO.h -- the slot snapshot text format
// the golden test's fixtures are written in.

#include "SlotAllocTestSupport.h"
#include "core/SlotAllocCore.h"
#include "core/SlotSnapshotIO.h"

#include <doctest/doctest.h>

#include <limits>
#include <string>
#include <vector>

using namespace Huginn::Test;

TEST_CASE("slot snapshot io: names survive escaping")
{
    for (const std::string name : { std::string("Iron Sword"), std::string(""), std::string("100% =Fire="),
             std::string("\xC3\x89lixir"), std::string("tab\there"), std::string("%41") }) {
        std::string back;
        REQUIRE(SA::UnescapeName(SA::EscapeName(name), back));
        CHECK(back == name);
        CHECK(SA::EscapeName(name).find(' ') == std::string::npos);
    }
    std::string out;
    CHECK_FALSE(SA::UnescapeName("noequals", out));
    CHECK_FALSE(SA::UnescapeName("=%4", out));
    CHECK_FALSE(SA::UnescapeName("=%zz", out));
}

TEST_CASE("slot snapshot io: a snapshot reads back to the same input and result")
{
    std::size_t checked = 0;
    for (std::uint32_t seed = 1; seed <= 300; ++seed) {
        SyntheticGenerator gen(seed, SyntheticSpec{ .exoticSettings = true });
        const auto settings = gen.RandomSettings();
        const auto layout = gen.RandomLayout();
        SA::PageMemory memory;
        for (int p = 0; p < 3; ++p) {
            SA::Snapshot snap;
            snap.tag = "synthetic test";
            snap.in = gen.Next(settings, layout, memory, true);
            if (gen.Chance(20) && !snap.in.candidates.empty()) {
                snap.in.candidates[0].score = -3.25;   // a score that is not the bridge
            }
            const auto out = SA::Allocate(snap.in);
            SA::SetResult(snap, out);
            memory = out.memory;

            for (const bool useDict : { false, true }) {
                SA::SnapshotDictionary dict;
                const std::string text = SA::WriteSnapshot(snap, useDict ? &dict : nullptr);
                std::vector<SA::Snapshot> back;
                std::string error;
                REQUIRE_MESSAGE(SA::ReadSnapshots(text, back, &error), error);
                REQUIRE(back.size() == 1);
                const auto& b = back[0];
                CHECK(b.tag == snap.tag);
                // Written again, it is the same text: every field round-trips.
                SA::SnapshotDictionary dict2;
                CHECK(SA::WriteSnapshot(b, useDict ? &dict2 : nullptr) == text);
                // And it allocates to the same result.
                CHECK(DiffResult(snap, b.in, SA::Allocate(b.in)).empty());
                ++checked;
            }
        }
    }
    MESSAGE("round trips: ", checked);
}

TEST_CASE("slot snapshot io: the item dictionary spans snapshots")
{
    SyntheticGenerator gen(99);
    const auto settings = gen.RandomSettings();
    const auto layout = gen.RandomLayout();
    SA::SnapshotDictionary dict;
    std::string file;
    std::vector<SA::Snapshot> written;
    for (int p = 0; p < 5; ++p) {
        SA::Snapshot snap;
        snap.tag = "t";
        snap.in = gen.Next(settings, layout, {}, true);
        SA::SetResult(snap, SA::Allocate(snap.in));
        file += SA::WriteSnapshot(snap, &dict);
        written.push_back(std::move(snap));
    }
    std::vector<SA::Snapshot> back;
    std::string error;
    REQUIRE_MESSAGE(SA::ReadSnapshots(file, back, &error), error);
    REQUIRE(back.size() == written.size());
    for (std::size_t i = 0; i < back.size(); ++i) {
        CHECK(SA::WriteSnapshot(back[i]) == SA::WriteSnapshot(written[i]));
    }
}

TEST_CASE("slot snapshot io: malformed text is refused with a line number")
{
    std::vector<SA::Snapshot> out;
    std::string error;
    CHECK_FALSE(SA::ReadSnapshots("SNAP =x page=0 now=0 mem=1 gen=1 ovr=0\nSLOT 23 1 1 1 1 0\nEND\n", out, &error));
    CHECK(error.find("line 2") != std::string::npos);
    CHECK_FALSE(SA::ReadSnapshots("SNAP =x page=0 now=0 mem=1 gen=1 ovr=0\n", out, &error));   // no END
    CHECK_FALSE(SA::ReadSnapshots("C 0 1 ~ 0\n", out, &error));                                  // outside a snapshot
    CHECK_FALSE(SA::ReadSnapshots("SNAP =x page=0 now=0 mem=1 gen=1 ovr=0\nC 5 1 ~ 0\nEND\n", out, &error));   // unknown item
}
