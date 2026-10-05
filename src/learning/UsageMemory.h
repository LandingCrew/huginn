#pragma once

#include "Config.h"
#include "core/RingBuffer.h"
#include "state/GameState.h"
#include <array>
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
    // USAGE MEMORY - Short-term situational recall (Event-Driven Memory)
    // =========================================================================
    // Tracks recent item usage in a ring buffer. When the same item is used
    // multiple times in the same discretized game context, it receives an
    // additive recency boost to its learning score.
    //
    // Fed only CONFIRMED selections (UsageMemorySubscriber), so one decision
    // is one event. It used to detect misclicks too; the selection confirm
    // window replaced that (SelectionTracker).
    //
    // Design:
    // - Ring buffer of last 20 usage events (self-pruning, no timestamps needed)
    // - Boost fires at >= 3 matching uses (same formID + same contextHash)
    // - Additive to learningScore inside (1 + lambda * learningScore), so
    //   context weight still gates it to zero when irrelevant
    //
    // Thread safety:
    // - Internal mutex guards all access to the ring buffer
    // - One writer in practice (confirmed selections, update thread)
    // - One reader (update thread via UtilityScorer)
    // - Lock is lightweight: 20-element scan under lock is sub-microsecond
    // =========================================================================
    class UsageMemory
    {
    public:
        static constexpr size_t BUFFER_CAPACITY = 20;
        static constexpr size_t MATCH_THRESHOLD = 3;
        // Added to the learned score, so it is on the target's scale: 1.5 was
        // sized for the 0-8 target and, on 0-1, would exceed the whole learned
        // term (roadmap review note "Rescale what was sized for 0-8"). /8.
        static constexpr float  RECENCY_BOOST = 1.5f / 8.0f;

        UsageMemory() = default;

        // Record that the player used an item in the given game state.
        void RecordUsage(RE::FormID formID, const State::GameState& state)
        {
            std::unique_lock lock(m_mutex);
            m_buffer.push_back(UsageEvent{formID, state.GetHash(), std::chrono::steady_clock::now()});
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

        // ── Snapshot reader for amortized scoring loops ────────────────
        // Copies the ring buffer (≤20 events, ~320 bytes) under a brief
        // shared_lock, then scans the local copy with NO lock held. The old
        // approach held a shared_lock across the entire scoring pass, blocking
        // RecordUsage (equip-event path) until scoring finished.
        // Pre-computes contextHash once.
        class SnapshotReader
        {
        public:
            SnapshotReader(SnapshotReader&&) = default;

            [[nodiscard]] float GetRecencyBoost(RE::FormID formID) const
            {
                size_t matchCount = 0;
                for (size_t i = 0; i < m_count; ++i) {
                    const auto& event = m_events[i];
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
            explicit SnapshotReader(const UsageMemory& owner, uint32_t contextHash)
                : m_contextHash(contextHash)
            {
                std::shared_lock lock(owner.m_mutex);
                for (const auto& event : owner.m_buffer) {
                    m_events[m_count++] = event;
                }
            }

            std::array<UsageEvent, BUFFER_CAPACITY> m_events{};
            size_t m_count = 0;
            uint32_t m_contextHash;
        };

        [[nodiscard]] SnapshotReader AcquireReader(const State::GameState& state) const
        {
            return SnapshotReader(*this, state.GetHash());
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
