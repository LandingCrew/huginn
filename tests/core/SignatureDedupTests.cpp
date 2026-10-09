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
#include <cstdint>
#include <latch>
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
    //
    // Built to fail without the lock (0.23.17; PR #190's verifier found the
    // first version passed with the lock removed: one key, threads that started
    // one after another). All threads are released together by a latch, and
    // each one inserts keys of its own -- the map grows and rehashes under the
    // others -- while they all hit one shared key. Unlocked, a rehash races the
    // other threads' lookups and inserts: lost or doubled keys, a doubled
    // shared warning, or a crash.
    constexpr int kThreads = 8;
    constexpr std::uint32_t kOwnKeys = 4000;
    constexpr std::uint32_t kShared = 0x00012EB7;
    SignatureDedup d;
    std::latch start(kThreads);
    std::atomic<int> sharedWarned{ 0 };
    std::atomic<int> ownWarned{ 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            for (std::uint32_t i = 0; i < kOwnKeys; ++i) {
                const std::uint32_t own = 0x01000000u + static_cast<std::uint32_t>(t) * kOwnKeys + i;
                if (d.ShouldWarn(own, 7)) ownWarned.fetch_add(1);
                if (d.ShouldWarn(own, 7)) ownWarned.fetch_add(1);  // same signature: quiet
                if (d.ShouldWarn(kShared, 0xABCDEFull)) sharedWarned.fetch_add(1);
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(sharedWarned.load() == 1);
    CHECK(ownWarned.load() == kThreads * static_cast<int>(kOwnKeys));
    CHECK(d.Size() == kThreads * kOwnKeys + 1);
}
