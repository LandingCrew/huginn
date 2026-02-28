#pragma once

#include "Config.h"
#include "core/RingBuffer.h"
#include "state/GameState.h"
#include <chrono>
#include <mutex>
#include <shared_mutex>

namespace Huginn::Learning
{
    // =========================================================================
    // USAGE EVENT - Single record of "player used item X in context Y"
    // =========================================================================
    struct UsageEvent
    {
        RE::FormID   formID = 0;         // What was used
        uint32_t     contextHash = 0;    // GameState::GetHash() at time of use
        std::chrono::steady_clock::time_point timestamp{};  // When it was used
    };

    // =========================================================================
    // MISCLICK RESULT - Returned by RecordUsage for rapid equip-then-switch
    // =========================================================================
    struct MisclickResult
    {
        bool detected = false;
        RE::FormID previousFormID = 0;
    };

    // =========================================================================
    // USAGE MEMORY - Short-term situational recall (Event-Driven Memory)
    // =========================================================================
    // Tracks recent item usage in a ring buffer. When the same item is used
    // multiple times in the same discretized game context, it receives an
    // additive recency boost to its learning score.
    //
    // Also detects misclicks: if the player switches to a different item
    // within MISCLICK_WINDOW_SECONDS in the same context, the previous item
    // likely wasn't intentional.
    //
    // Design:
    // - Ring buffer of last 20 usage events (self-pruning, no timestamps needed)
    // - Boost fires at >= 3 matching uses (same formID + same contextHash)
    // - Additive to learningScore inside (1 + lambda * learningScore), so
    //   context weight still gates it to zero when irrelevant
    //
    // Thread safety:
    // - Internal mutex guards all access to the ring buffer
    // - Multiple writers (Wheeler callback, equip callback, ExternalEquipLearner)
    // - One reader (update thread via UtilityScorer)
    // - Lock is lightweight: 20-element scan under lock is sub-microsecond
    // =========================================================================
    class UsageMemory
    {
    public:
        static constexpr size_t BUFFER_CAPACITY = 20;
        static constexpr size_t MATCH_THRESHOLD = 3;
        static constexpr float  RECENCY_BOOST = 1.5f;

        UsageMemory() = default;

        // Record that the player used an item in the given game state.
        // Returns misclick detection result: if the previous event was a
        // different item in the same context within MISCLICK_WINDOW_SECONDS,
        // the previous item is flagged as a likely misclick.
        MisclickResult RecordUsage(RE::FormID formID, const State::GameState& state)
        {
            std::unique_lock lock(m_mutex);

            MisclickResult result;
            auto now = std::chrono::steady_clock::now();
            uint32_t hash = state.GetHash();

            // Check for misclick: different item, same context, within time window
            if (!m_buffer.empty()) {
                const auto& last = m_buffer.back();
                if (last.formID != formID && last.contextHash == hash) {
                    float elapsed = std::chrono::duration<float>(now - last.timestamp).count();
                    if (elapsed < Config::MISCLICK_WINDOW_SECONDS) {
                        result.detected = true;
                        result.previousFormID = last.formID;
                    }
                }
            }

            m_buffer.push_back(UsageEvent{formID, hash, now});
            return result;
        }

        // Get recency boost for an item in the current context.
        // Returns RECENCY_BOOST if >= MATCH_THRESHOLD matching events exist, else 0.
        [[nodiscard]] float GetRecencyBoost(RE::FormID formID, const State::GameState& state) const
        {
            std::shared_lock lock(m_mutex);
            uint32_t hash = state.GetHash();
            size_t matchCount = 0;

            for (const auto& event : m_buffer) {
                if (event.formID == formID && event.contextHash == hash) {
                    if (++matchCount >= MATCH_THRESHOLD) {
                        return RECENCY_BOOST;
                    }
                }
            }

            return 0.0f;
        }

        // ── Locked reader for amortized scoring loops ────────────────
        // Acquires mutex once; caller loops N candidates under it.
        // Pre-computes contextHash once.
        class LockedReader
        {
        public:
            LockedReader(LockedReader&&) = default;

            [[nodiscard]] float GetRecencyBoost(RE::FormID formID) const
            {
                size_t matchCount = 0;
                for (const auto& event : m_owner.m_buffer) {
                    if (event.formID == formID && event.contextHash == m_contextHash) {
                        if (++matchCount >= MATCH_THRESHOLD) {
                            return RECENCY_BOOST;
                        }
                    }
                }
                return 0.0f;
            }

        private:
            friend class UsageMemory;
            explicit LockedReader(const UsageMemory& owner, uint32_t contextHash)
                : m_owner(owner), m_lock(owner.m_mutex), m_contextHash(contextHash) {}

            const UsageMemory& m_owner;
            std::shared_lock<std::shared_mutex> m_lock;
            uint32_t m_contextHash;
        };

        [[nodiscard]] LockedReader AcquireReader(const State::GameState& state) const
        {
            return LockedReader(*this, state.GetHash());
        }

        // Clear all usage history (e.g., on save load)
        void Clear()
        {
            std::unique_lock lock(m_mutex);
            m_buffer.clear();
        }

        // Debug: number of events currently in the buffer
        [[nodiscard]] size_t GetEventCount() const noexcept
        {
            std::shared_lock lock(m_mutex);
            return m_buffer.size();
        }

        // Copy-out snapshot for debug display (thread-safe)
        [[nodiscard]] std::vector<UsageEvent> GetSnapshot() const
        {
            std::shared_lock lock(m_mutex);
            std::vector<UsageEvent> snapshot;
            snapshot.reserve(m_buffer.size());
            for (const auto& event : m_buffer)
                snapshot.push_back(event);
            return snapshot;
        }

    private:
        mutable std::shared_mutex m_mutex;
        RingBuffer<UsageEvent, BUFFER_CAPACITY> m_buffer;
    };

}  // namespace Huginn::Learning
