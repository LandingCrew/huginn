// Host tests for src/core/OpenWheelWrites.h.

#include "core/OpenWheelWrites.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Huginn::Core;

namespace
{
    WheelPageContents Page(std::vector<uint32_t> ids)
    {
        WheelPageContents p;
        p.formIDs = ids;
        p.wildcard.assign(ids.size(), false);
        p.uniqueIDs.assign(ids.size(), 0);
        p.subtexts.assign(ids.size(), std::string{});
        return p;
    }
}

TEST_CASE("open wheel: only the writable entry changes; the rest keep the wheel's content")
{
    // The wheel holds A B C D; the allocator re-seated everything and put the
    // urgent potion P on slot 1.
    const auto cached = Page({ 0xA, 0xB, 0xC, 0xD });
    auto incoming = Page({ 0xC, 0xF00D, 0xA, 0xE });
    incoming.subtexts = { "Favorite", "Low MP", "", "" };
    const auto written = RestrictToWritable(incoming, cached, { false, true, false, false });
    CHECK(written == std::vector<size_t>{ 1 });
    CHECK(incoming.formIDs == std::vector<uint32_t>{ 0xA, 0xF00D, 0xC, 0xD });
    CHECK(incoming.subtexts == std::vector<std::string>{ "", "Low MP", "", "" });
}

TEST_CASE("open wheel: a writable entry already on the wheel writes nothing")
{
    const auto cached = Page({ 0xA, 0xF00D });
    auto incoming = Page({ 0xB, 0xF00D });
    CHECK(RestrictToWritable(incoming, cached, { false, true }).empty());
    CHECK(incoming.formIDs == std::vector<uint32_t>{ 0xA, 0xF00D });
}

TEST_CASE("open wheel: nothing writable freezes the whole page")
{
    const auto cached = Page({ 0xA, 0xB, 0xC });
    auto incoming = Page({ 0xC, 0xB, 0xA });
    incoming.wildcard = { true, true, true };
    incoming.uniqueIDs = { 7, 8, 9 };
    CHECK(RestrictToWritable(incoming, cached, {}).empty());
    CHECK(incoming.formIDs == cached.formIDs);
    CHECK(incoming.wildcard == cached.wildcard);
    CHECK(incoming.uniqueIDs == cached.uniqueIDs);
}

TEST_CASE("open wheel: a subtext or uniqueID change on a writable entry counts as a write")
{
    const auto cached = Page({ 0xF00D });
    auto incoming = Page({ 0xF00D });
    incoming.subtexts = { "Low HP" };
    CHECK(RestrictToWritable(incoming, cached, { true }) == std::vector<size_t>{ 0 });
    auto again = Page({ 0xF00D });
    again.uniqueIDs = { 3 };
    CHECK(RestrictToWritable(again, cached, { true }) == std::vector<size_t>{ 0 });
}

TEST_CASE("open wheel: entries past the cache keep the incoming value")
{
    const auto cached = Page({ 0xA });
    auto incoming = Page({ 0xB, 0xC });
    const auto written = RestrictToWritable(incoming, cached, { false, false });
    CHECK(incoming.formIDs == std::vector<uint32_t>{ 0xA, 0xC });
    CHECK(written.empty());   // index 1 is not writable; nothing to compare it with
}
