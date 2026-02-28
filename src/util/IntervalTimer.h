#pragma once

#include <chrono>

namespace Huginn::Util
{
    /// @brief Lightweight timer for periodic operations.
    /// Encapsulates the "check elapsed, reset if due" pattern.
    /// All methods take a `now` parameter to ensure consistent timestamps within a frame.
    struct IntervalTimer {
        std::chrono::steady_clock::time_point lastRun{};

        /// @brief Returns true if intervalMs has elapsed since last reset. Does NOT reset.
        /// Use this when you need to check multiple timers before deciding which to reset.
        [[nodiscard]] bool IsDue(
            std::chrono::steady_clock::time_point now, float intervalMs) const noexcept
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRun);
            return elapsed.count() >= static_cast<int64_t>(intervalMs);
        }

        /// @brief Returns true and resets if intervalMs has elapsed. Combines check + reset.
        [[nodiscard]] bool CheckAndReset(
            std::chrono::steady_clock::time_point now, float intervalMs) noexcept
        {
            if (IsDue(now, intervalMs)) {
                lastRun = now;
                return true;
            }
            return false;
        }

        /// @brief Reset to current time.
        void Reset() noexcept { lastRun = std::chrono::steady_clock::now(); }

        /// @brief Reset to a specific timestamp.
        void Reset(std::chrono::steady_clock::time_point now) noexcept { lastRun = now; }
    };
}
