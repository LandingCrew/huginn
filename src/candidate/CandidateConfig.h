#pragma once

#include <cstdint>

namespace Huginn::Candidate
{
    // Forward declaration - full definition in CandidateTypes.h
    enum class SourceType : uint8_t;

    /// Policy for handling spells the player can't currently afford (insufficient magicka).
    /// Applied in two phases during CandidateFilters::ApplyAllFilters():
    ///   1. PassesAffordabilityFilter() - Disallow removes spells, Penalize/Allow keep them
    ///   2. ApplyAffordabilityPenalties() - Penalize reduces baseRelevance by ratio
    enum class UncastableSpellPolicy : uint8_t {
        Disallow,   // Filter out uncastable spells (current behavior)
        Penalize,   // Keep but reduce relevance by magicka shortfall ratio
        Allow       // Keep at full relevance (no penalty)
    };

    /// Canonical string representation for logging.
    [[nodiscard]] constexpr const char* ToString(UncastableSpellPolicy policy) noexcept {
        switch (policy) {
            case UncastableSpellPolicy::Disallow:  return "Disallow";
            case UncastableSpellPolicy::Penalize:  return "Penalize";
            case UncastableSpellPolicy::Allow:     return "Allow";
            default: return "Unknown";
        }
    }

    /**
     * @brief Configuration for candidate generation and filtering.
     *
     * This struct holds user preferences that affect how candidates are
     * selected and ranked. It controls:
     * - Source preferences (spell vs potion vs scroll)
     * - Potion category preferences
     * - Active buff filtering behavior
     * - Cooldown durations
     * - Relevance thresholds
     *
     * Future: Will be loaded from INI file via dMenu integration (v0.11.0)
     */
    struct CandidateConfig
    {
        // =============================================================================
        // ACTIVE BUFF FILTERING
        // Controls filtering of items whose effects are already active.
        // =============================================================================

        /// Filter out resist items (potions AND spells) if player already has high resistance.
        /// Prevents recommending "Resist Fire" when player has 75% fire resist.
        bool filterRedundantResists = true;

        /// Resistance threshold (percentage) above which resist items are filtered.
        /// If fire resist >= this value, don't recommend more fire resist items.
        float resistThresholdToFilter = 50.0f;

        /// Filter items whose buff is already active on player.
        /// Examples: waterbreathing, invisibility, muffle
        bool filterActiveBuffs = true;

        /// Filter healing items when health is already full.
        bool filterHealingWhenFull = true;

        /// Filter magicka potions when magicka is already full.
        bool filterMagickaWhenFull = true;

        /// Filter stamina potions when stamina is already full.
        bool filterStaminaWhenFull = true;

        // =============================================================================
        // UNCASTABLE SPELL HANDLING
        // Controls what happens to spells the player can't currently afford.
        // =============================================================================

        /// Policy for spells the player lacks magicka to cast.
        UncastableSpellPolicy uncastableSpellPolicy = UncastableSpellPolicy::Disallow;

        /// Minimum penalty multiplier for Penalize mode (floor for ratio).
        /// Prevents completely zeroing out high-cost spells.
        /// 0.05 = even wildly expensive spells retain 5% of their relevance.
        float uncastablePenaltyFloor = 0.05f;

        // =============================================================================
        // SOUL GEM RECHARGING
        // =============================================================================

        /// Enable soul gem recommendations and one-click weapon recharging.
        /// When true, filled soul gems appear as candidates when weapon charge
        /// is low, and activating them recharges the equipped weapon.
        bool enableSoulGemRecharge = true;

        // =============================================================================
        // COOLDOWN DURATIONS
        // How long after using an item before it can be recommended again.
        // =============================================================================

        /// Cooldown for spells (seconds)
        float spellCooldown = 2.0f;

        /// Cooldown for potions (seconds)
        float potionCooldown = 3.0f;

        /// Cooldown for scrolls (seconds)
        float scrollCooldown = 2.0f;

        /// Cooldown for weapons (seconds)
        float weaponCooldown = 5.0f;

        /// Cooldown for ammo (seconds)
        float ammoCooldown = 1.0f;

        /// Cooldown for soul gems (seconds)
        float soulGemCooldown = 3.0f;

        /// Cooldown for food (seconds)
        float foodCooldown = 3.0f;

        // =============================================================================
        // CANDIDATE LIMITS
        // =============================================================================

        /// Maximum number of candidates to keep after filtering.
        /// Prevents performance issues with very large inventories.
        /// NOTE: Set high enough to include weapons (gathered last in pipeline)
        uint32_t maxCandidatesAfterFilter = 500;

        // =============================================================================
        // METHODS
        // =============================================================================

        /// Get cooldown duration for a specific source type (type-safe version)
        [[nodiscard]] float GetCooldownForSourceType(SourceType sourceType) const noexcept
        {
            switch (sourceType) {
                case SourceType::Spell:   return spellCooldown;
                case SourceType::Potion:  return potionCooldown;
                case SourceType::Scroll:  return scrollCooldown;
                case SourceType::Weapon:  return weaponCooldown;
                case SourceType::Ammo:    return ammoCooldown;
                case SourceType::SoulGem: return soulGemCooldown;
                case SourceType::Food:    return foodCooldown;
                case SourceType::Staff:   return spellCooldown;  // Staff uses spell cooldown
                default: return 2.0f;  // Fallback for _Count or unknown
            }
        }

        /// Reset all settings to defaults
        void ResetToDefaults()
        {
            *this = CandidateConfig{};
        }

        // Future: Load from INI file
        // static CandidateConfig LoadFromINI(const char* path);
        // void SaveToINI(const char* path) const;
    };

    // =============================================================================
    // GLOBAL CONFIG INSTANCE
    // Single source of truth for candidate configuration.
    //
    // THREAD SAFETY:
    // - Reads are safe from any thread (struct contains only POD types)
    // - Writes must occur ONLY when the update loop is paused:
    //   1. During plugin initialization (before game load)
    //   2. In menu callbacks when recommendations are suspended
    // - DO NOT modify while CandidateGenerator::GenerateCandidates() is running
    //
    // CandidateGenerator maintains its own copy of config at initialization.
    // To apply config changes mid-session, call CandidateGenerator::Initialize()
    // again or implement a RefreshConfig() method.
    //
    // Future: v0.11.0 will add dMenu integration with proper synchronization.
    // =============================================================================
    inline CandidateConfig g_candidateConfig;

}  // namespace Huginn::Candidate
