#pragma once

// =============================================================================
// SIGNATURE DEDUP -- warn once per (key, signature), from any thread
// =============================================================================
// A diagnostic that fires on every scan (the inventory duplicate warning,
// util/InventoryUtil.h) should log only when what it reports changes: per
// key, the last signature logged is kept, and a call with the same signature
// is quiet. Thread-safe: the inventory scans run on rotating job threads.
//
// The game side holds ONE instance for the whole process
// (Util::ShouldWarnDuplicate, a non-template inline function with a
// function-local static). 0.23.16's first version put the statics inside the
// template GetInventorySafe<Filter>, which gave each call site's filter
// lambda a map of its own.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace Huginn::Core
{
    class SignatureDedup
    {
    public:
        /// True when `signature` differs from the last one recorded for `key`
        /// (or `key` is new): the caller should log, and it is recorded. False
        /// when it is the same: stay quiet.
        bool ShouldWarn(std::uint32_t key, std::uint64_t signature)
        {
            std::lock_guard lock(mutex_);
            auto [slot, fresh] = last_.try_emplace(key, signature);
            if (fresh) return true;
            if (slot->second == signature) return false;
            slot->second = signature;
            return true;
        }

        [[nodiscard]] std::size_t Size() const
        {
            std::lock_guard lock(mutex_);
            return last_.size();
        }

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::uint32_t, std::uint64_t> last_;
    };
}
