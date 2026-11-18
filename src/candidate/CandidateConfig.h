#pragma once

#include <cstdint>

namespace Huginn::Candidate
{
    // Forward declaration - full definition in CandidateTypes.h
    enum class SourceType : uint8_t;

    /// Policy for handling spells the player can't currently afford (insufficient magicka).
    /// Disallow removes them in the filter visitor (RunVisitorFilters, Reason::affordability); Allow keeps them.
    /// A legacy INI value of "Penalize" loads as Allow -- it never had a penalty behind it.
    enum class UncastableSpellPolicy : uint8_t {
        Disallow,   // Filter out uncastable spells (current behavior)
        Allow       // Keep at full relevance (no penalty)
    };

    /// Canonical string representation for logging.
    [[nodiscard]] constexpr const char* ToString(UncastableSpellPolicy policy) noexcept {
        switch (policy) {
            case UncastableSpellPolicy::Disallow:  return "Disallow";
            case UncastableSpellPolicy::Allow:     return "Allow";
            default: return "Unknown";
        }
    }

    /// 0.24.29: which resist potions of one element are offered when no
    /// potion's resist runs (core/ResistPotions.h). They do not stack.
    enum class ResistPotionPick : uint8_t {
        Greedy,   // only the strongest on hand (default)
        All       // every one; the score decides
    };

    [[nodiscard]] constexpr const char* ToString(ResistPotionPick pick) noexcept {
        switch (pick) {
            case ResistPotionPick::Greedy: return "Greedy";
            case ResistPotionPick::All:    return "All";
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

        /// Filter resist items that would not stack (core/ResistPotions.h): a
        /// resist potion while a potion's resist to its element runs, a resist
        /// spell while its own effect runs, and the weaker potions of an
        /// element (sResistPotionPick = Greedy). 0.24.29 removed the 50 %
        /// threshold on the player's total resistance.
        bool filterRedundantResists = true;

        /// 0.24.29 ([Candidates] sResistPotionPick): Greedy offers only the
        /// strongest resist potion per element. Either way, none while a
        /// potion's resist to that element runs.
        ResistPotionPick resistPotionPick = ResistPotionPick::Greedy;

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
