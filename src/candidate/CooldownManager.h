#pragma once

#include "CandidateTypes.h"
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <array>
#include <shared_mutex>  // Thread safety for concurrent read/write

namespace Huginn::Candidate
{
    /**
     * @brief Manages per-item cooldowns to prevent recommendation spam.
     *
     * When an item is used (spell cast, potion consumed, weapon equipped),
     * a cooldown is started. During the cooldown period, that item won't be
     * recommended again. This prevents the same item from flickering in/out
     * of recommendation slots.
     *
     * Cooldown durations are configurable per SourceType, allowing different
     * behaviors for spells (short cooldown) vs weapons (longer cooldown).
     */
    class CooldownManager
    {
    public:
        CooldownManager();

        /**
         * @brief Check if an item is currently on cooldown.
         * @param formID The form ID of the item
         * @param type The source type (Spell, Potion, etc.)
         * @return true if item is on cooldown and should not be recommended
         */
        [[nodiscard]] bool IsOnCooldown(RE::FormID formID, SourceType type) const noexcept;

        /**
         * @brief Start a cooldown for the specified item.
         * Called when an item is used/equipped.
         * @param formID The form ID of the item
         * @param type The source type (determines cooldown duration)
         */
        void StartCooldown(RE::FormID formID, SourceType type);

        /**
         * @brief Start a cooldown with a custom duration.
         * @param formID The form ID of the item
         * @param type The source type
         * @param durationSeconds Custom cooldown duration
         */
        void StartCooldown(RE::FormID formID, SourceType type, float durationSeconds);

        /**
         * @brief Update cooldown manager, expiring old entries.
         * Should be called each frame/update tick.
         * @param deltaSeconds Time since last update
         */
        void Update(float deltaSeconds);

        /**
         * @brief Set the default cooldown duration for a source type.
         * @param type The source type
         * @param seconds Cooldown duration in seconds
         */
        void SetDuration(SourceType type, float seconds);

        /**
         * @brief Get the default cooldown duration for a source type.
         * @param type The source type
         * @return Cooldown duration in seconds
         */
        [[nodiscard]] float GetDuration(SourceType type) const noexcept;

        /**
         * @brief Clear all cooldowns. Useful for testing or game load.
         */
        void Clear();

        /**
         * @brief Get the number of active (non-expired) cooldowns.
         */
        [[nodiscard]] size_t GetActiveCount() const noexcept;

        /**
         * @brief Get remaining cooldown time for an item.
         * @param formID The form ID of the item
         * @param type The source type
         * @return Remaining seconds, or 0.0f if not on cooldown
         */
        [[nodiscard]] float GetRemainingCooldown(RE::FormID formID, SourceType type) const noexcept;

        /**
         * @brief Cancel a cooldown early.
         * @param formID The form ID of the item
         * @param type The source type
         * @return true if a cooldown was cancelled
         */
        bool CancelCooldown(RE::FormID formID, SourceType type);

        /**
         * @brief Snapshot all active (non-expired) cooldown keys.
         * Acquires the lock once; callers can then check the returned set lock-free.
         */
        [[nodiscard]] std::unordered_set<uint64_t> GetActiveCooldownKeys() const;

        /**
         * @brief Create a unique key from formID and source type.
         */
        [[nodiscard]] static constexpr uint64_t MakeKey(RE::FormID formID, SourceType type) noexcept {
            return (static_cast<uint64_t>(type) << 32) | static_cast<uint64_t>(formID);
        }

    private:
        struct CooldownEntry {
            std::chrono::steady_clock::time_point expiryTime;
        };

        // Map from combined key (sourceType << 32 | formID) to cooldown entry
        std::unordered_map<uint64_t, CooldownEntry> m_cooldowns;

        // Default durations indexed by SourceType
        std::array<float, SOURCE_TYPE_COUNT> m_durations;

        // Timestamp of last cleanup pass
        std::chrono::steady_clock::time_point m_lastCleanup;
        static constexpr float CLEANUP_INTERVAL_SECONDS = 5.0f;

        /**
         * @brief Remove all expired entries from the map.
         */
        void CleanupExpired();

        /**
         * @brief Remove all expired entries (caller must hold m_mutex).
         * @note Internal helper - use CleanupExpired() for external calls.
         */
        void CleanupExpiredLocked();

        // Thread safety: protects m_cooldowns from concurrent access
        // Readers use shared_lock, writers use unique_lock
        mutable std::shared_mutex m_mutex;
    };

}  // namespace Huginn::Candidate
