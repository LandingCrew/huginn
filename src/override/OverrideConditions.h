#pragma once

#include "candidate/CandidateTypes.h"
#include <optional>
#include <string>
#include <vector>

namespace Huginn::Override
{
    // =============================================================================
    // PRIORITY CONSTANTS (higher = more urgent)
    // =============================================================================
    // These values determine which override takes precedence when multiple
    // conditions are active simultaneously. Higher priority overrides
    // claim slot 1 (the top/most visible slot).
    // =============================================================================

    namespace Priority
    {
        inline constexpr int CRITICAL_HEALTH = 100;  // HP < 10% - highest priority
        inline constexpr int CRITICAL_MAGICKA = 70;  // MP < 10% - magicka potion
        inline constexpr int CRITICAL_STAMINA = 60;  // SP < 10% - stamina potion
        inline constexpr int DROWNING = 50;          // Underwater without waterbreathing
        inline constexpr int LOW_AMMO = 40;          // Bow/crossbow equipped, ammo running low
        inline constexpr int WEAPON_EMPTY = 35;      // Enchanted weapon at 0% charge
        // FALLING = 40 deferred (Slow Fall not in vanilla Skyrim)
    }

    // =============================================================================
    // OVERRIDE CATEGORY
    // =============================================================================
    // Classifies each override by the resource it addresses.
    // Used by OverrideFilter to route overrides to specific slots.
    // =============================================================================

    enum class OverrideCategory : uint8_t
    {
        HP,      // CriticalHealth
        MP,      // CriticalMagicka
        SP,      // CriticalStamina
        Other    // Drowning, LowAmmo, WeaponCharge
    };

    [[nodiscard]] inline constexpr std::string_view OverrideCategoryToString(OverrideCategory c) noexcept
    {
        switch (c) {
            case OverrideCategory::HP:    return "HP";
            case OverrideCategory::MP:    return "MP";
            case OverrideCategory::SP:    return "SP";
            case OverrideCategory::Other: return "Other";
            default:                      return "Unknown";
        }
    }

    // =============================================================================
    // HYSTERESIS CONFIGURATION
    // =============================================================================
    // Prevents flickering when values hover near thresholds.
    // Once an override activates, it stays active until the value crosses
    // a higher deactivation threshold (or minimum duration expires).
    //
    // Example: Health override
    //   - Activates when health < 10%
    //   - Deactivates when health >= 15% (hysteresis gap prevents rapid toggle)
    //   - Stays active for at least 2 seconds (prevents split-second flicker)
    // =============================================================================

    struct HysteresisConfig
    {
        float activationThreshold;     // When to activate (e.g., 0.10 for 10%)
        float deactivationThreshold;   // When to deactivate (e.g., 0.15 for 15%)
        float minDurationMs;           // Minimum time active once triggered
    };

    // =============================================================================
    // HYSTERESIS STATE (internal tracking)
    // =============================================================================
    // Tracks the current state of a specific override condition's hysteresis.
    // =============================================================================

    struct HysteresisState
    {
        bool isActive = false;         // Currently active?
        float activeDurationMs = 0.0f; // How long has it been active?
    };

    // =============================================================================
    // OVERRIDE RESULT
    // =============================================================================
    // Result of evaluating a single override condition.
    // Contains everything needed to inject the override into the candidate list.
    // =============================================================================

    struct OverrideResult
    {
        bool active = false;           // Is this override condition met?
        int priority = 0;              // Priority for slot allocation (higher = slot 1)
        OverrideCategory category = OverrideCategory::Other;  // Resource category for slot filtering
        std::string reason;            // Human-readable reason (for logging/debug)

        // The candidate to inject (nullopt if condition met but no item available)
        std::optional<Candidate::CandidateVariant> candidate;

        // Target slot (0 = slot 1, highest priority)
        int targetSlot = 0;
    };

    // =============================================================================
    // OVERRIDE COLLECTION
    // =============================================================================
    // Collection of all currently active overrides, sorted by priority.
    // =============================================================================

    struct OverrideCollection
    {
        std::vector<OverrideResult> activeOverrides;

        // Check if any override is active
        [[nodiscard]] bool HasActiveOverride() const noexcept
        {
            return !activeOverrides.empty();
        }

        // Get the highest priority override (or nullptr if none)
        [[nodiscard]] const OverrideResult* GetTopOverride() const noexcept
        {
            if (activeOverrides.empty()) {
                return nullptr;
            }
            return &activeOverrides[0];  // Already sorted by priority
        }

        // Get override count
        [[nodiscard]] size_t GetCount() const noexcept
        {
            return activeOverrides.size();
        }

        // Sort overrides by priority (descending - highest first)
        void SortByPriority()
        {
            std::sort(activeOverrides.begin(), activeOverrides.end(),
                [](const OverrideResult& a, const OverrideResult& b) {
                    return a.priority > b.priority;
                });
        }
    };

}  // namespace Huginn::Override
