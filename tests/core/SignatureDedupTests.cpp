// core/SignatureDedup.h: the inventory duplicate warning's dedup (0.23.16).
//
// What is tested here: one log per (key, signature), a changed signature logs
// again, and concurrent callers on many threads log a signature exactly once.
// What is not: that the game holds a single instance. Util::ShouldWarnDuplicate
// (util/InventoryUtil.h) takes RE types, so it cannot be built on the host; it
// is a non-template inline function with external linkage, and such a
// function's function-local static is one object in the whole program
// ([dcl.inline]/6), whichever translation unit or GetInventorySafe<Filter>
// instantiation calls it.

#include "core/SignatureDedup.h"

#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

using Huginn::Core::SignatureDedup;

TEST_CASE("signature dedup: once per key and signature, again when it changes")
{
    SignatureDedup d;
    CHECK(d.ShouldWarn(0x00012EB7, 42));
    CHECK_FALSE(d.ShouldWarn(0x00012EB7, 42));
    CHECK_FALSE(d.ShouldWarn(0x00012EB7, 42));
    CHECK(d.ShouldWarn(0x00012EB7, 43));   // the numbers changed: log
    CHECK_FALSE(d.ShouldWarn(0x00012EB7, 43));
    CHECK(d.ShouldWarn(0x00012EB7, 42));   // and back: log again
    CHECK(d.ShouldWarn(0x0001397E, 42));   // another form is its own key
    CHECK(d.Size() == 2);
}

TEST_CASE("signature dedup: one shared instance logs a signature once across threads")
{
    // The LoreRim R3 session: the same Iron Sword signature logged 8 times,
    // once per job thread, by a thread_local map.
    SignatureDedup d;
    std::atomic<int> warned{ 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 2000; ++i) {
                if (d.ShouldWarn(0x00012EB7, 0xABCDEFull)) warned.fetch_add(1);
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(warned.load() == 1);
}
