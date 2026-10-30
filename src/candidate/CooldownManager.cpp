#include "CooldownManager.h"
#include "CandidateConfig.h"

namespace Huginn::Candidate
{
    // Thread safety macros for clarity
    #define READ_LOCK std::shared_lock lock(m_mutex)
    #define WRITE_LOCK std::unique_lock lock(m_mutex)
    CooldownManager::CooldownManager()
        : m_lastCleanup(std::chrono::steady_clock::now())
        , m_lastExpiryScan(std::chrono::steady_clock::now())
    {
        // Initialize durations from CandidateConfig defaults (single source of truth)
        constexpr CandidateConfig defaults{};
        m_durations[static_cast<size_t>(SourceType::Spell)]   = defaults.spellCooldown;
        m_durations[static_cast<size_t>(SourceType::Potion)]  = defaults.potionCooldown;
        m_durations[static_cast<size_t>(SourceType::Scroll)]  = defaults.scrollCooldown;
        m_durations[static_cast<size_t>(SourceType::Weapon)]  = defaults.weaponCooldown;
        m_durations[static_cast<size_t>(SourceType::Ammo)]    = defaults.ammoCooldown;
        m_durations[static_cast<size_t>(SourceType::SoulGem)] = defaults.soulGemCooldown;
        m_durations[static_cast<size_t>(SourceType::Food)]    = defaults.foodCooldown;
        m_durations[static_cast<size_t>(SourceType::Staff)]   = defaults.spellCooldown;  // Intentional: staves share spell cooldown

        // Reserve space for typical usage
        m_cooldowns.reserve(32);
    }

    bool CooldownManager::IsOnCooldown(RE::FormID formID, SourceType type) const noexcept
    {
        READ_LOCK;
        const uint64_t key = MakeKey(formID, type);
        auto it = m_cooldowns.find(key);

        if (it == m_cooldowns.end()) {
            return false;
        }

        // Check if cooldown has expired
        return std::chrono::steady_clock::now() < it->second.expiryTime;
    }

    std::unordered_set<uint64_t> CooldownManager::GetActiveCooldownKeys() const
    {
        READ_LOCK;
        std::unordered_set<uint64_t> active;
        auto now = std::chrono::steady_clock::now();
        for (const auto& [key, entry] : m_cooldowns) {
            if (now < entry.expiryTime) {
                active.insert(key);
            }
        }
        return active;
    }

    void CooldownManager::StartCooldown(RE::FormID formID, SourceType type)
    {
        StartCooldown(formID, type, GetDuration(type));
    }

    void CooldownManager::StartCooldown(RE::FormID formID, SourceType type, float durationSeconds)
    {
        if (durationSeconds <= 0.0f) {
            return;  // No cooldown to start
        }

        const uint64_t key = MakeKey(formID, type);
        const auto now = std::chrono::steady_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(durationSeconds)
        );

        WRITE_LOCK;
        m_cooldowns[key] = CooldownEntry{ now + duration };
    }

    bool CooldownManager::Update([[maybe_unused]] float deltaSeconds)
    {
        // Note: deltaSeconds is unused - we use steady_clock for time tracking.
        // Parameter kept for API consistency with other Update() methods.

        const auto now = std::chrono::steady_clock::now();

        // Detect entries that crossed into expired since the last scan: expiry
        // is otherwise lazy, and an expired item needs one pipeline run to
        // resurface (caller MarkPageDirty's on true).
        bool anyExpired = false;
        {
            READ_LOCK;
            for (const auto& [key, entry] : m_cooldowns) {
                if (entry.expiryTime > m_lastExpiryScan && entry.expiryTime <= now) {
                    anyExpired = true;
                    break;
                }
            }
        }
        m_lastExpiryScan = now;

        // Periodically clean up expired entries to prevent unbounded growth
        const auto elapsed = std::chrono::duration<float>(now - m_lastCleanup).count();

        if (elapsed >= CLEANUP_INTERVAL_SECONDS) {
            WRITE_LOCK;
            CleanupExpiredLocked();  // Call lockless version since we already hold lock
            m_lastCleanup = now;
        }

        return anyExpired;
    }

    void CooldownManager::SetDuration(SourceType type, float seconds)
    {
        const size_t index = static_cast<size_t>(type);
        if (index < m_durations.size()) {
            m_durations[index] = seconds;
        }
    }

    float CooldownManager::GetDuration(SourceType type) const noexcept
    {
        const size_t index = static_cast<size_t>(type);
        if (index < m_durations.size()) {
            return m_durations[index];
        }
        return 2.0f;  // Fallback default
    }

    void CooldownManager::Clear()
    {
        WRITE_LOCK;
        m_cooldowns.clear();
    }

    void CooldownManager::CleanupExpiredLocked()
    {
        // Caller holds the write lock (unique on m_mutex): this erases.
        const auto now = std::chrono::steady_clock::now();

        // C++20 erase_if for efficient removal
        std::erase_if(m_cooldowns, [&now](const auto& pair) {
            return now >= pair.second.expiryTime;
        });
    }

    #undef READ_LOCK
    #undef WRITE_LOCK

}  // namespace Huginn::Candidate
