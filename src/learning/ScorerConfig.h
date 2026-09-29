#pragma once

namespace Huginn::Scoring
{
    // =============================================================================
    // FAVORITES MODE - How to handle player-favorited items in scoring
    // =============================================================================
    enum class FavoritesMode : uint8_t
    {
        Boost,      // Favorited items get utility multiplied by favoritesBoostMin-Max
        Off,        // Ignore favorites flag entirely (neutral scoring)
        Suppress    // Skip favorited items from recommendations (they're already accessible)
    };

    // =============================================================================
    // POTION TIER PREFERENCE - which strength of the same potion ranks first
    // =============================================================================
    enum class PotionTierPreference : uint8_t
    {
        Higher,     // Use the best one now (greedy)
        None,       // No ordering: natural scores stand, and the slot hold keeps near-ties still
        Lower       // Save the good ones for later
    };

    // Utility ratio between adjacent strengths of one potion under Higher/Lower.
    // Deliberately larger than the slot challenger margin (default 25%), so the
    // tier rule decides which strength shows and a hold cannot keep the wrong one.
    inline constexpr float POTION_TIER_STEP = 1.5f;

    // =============================================================================
    // FIT MODE - whether the item-context fit multiplier (FitScorer) is used
    // =============================================================================
    enum class FitMode : uint8_t
    {
        Off = 0,     // Not computed; breakdown fields stay neutral
        Shadow = 1,  // Computed and recorded in the breakdown / [Recs], NOT applied
        Apply = 2    // Multiplied into the utility (ComputeUtility)
    };

    [[nodiscard]] constexpr const char* FitModeToString(FitMode mode) noexcept
    {
        switch (mode) {
            case FitMode::Off:    return "Off";
            case FitMode::Shadow: return "Shadow";
            case FitMode::Apply:  return "Apply";
            default:              return "Unknown";
        }
    }

    // =============================================================================
    // SCORER CONFIGURATION - Tunable parameters for the utility scoring system
    // =============================================================================
    // Future: Load from INI file or dMenu (v1.0)
    struct ScorerConfig
    {
        // ---------------------------------------------------------------------
        // Core Scoring Parameters
        // ---------------------------------------------------------------------

        // Confidence-adaptive lambda
        // λ(confidence) = lambdaMin + confidence × (lambdaMax - lambdaMin)
        // At low confidence (cold start), context dominates; at high confidence, learning amplifies
        float lambdaMin = 0.5f;   // Learning weight at zero confidence (cold start)
        float lambdaMax = 3.0f;   // Learning weight at full confidence (well-trained)

        // Alpha decay rate: How confidence affects learning vs prior balance
        // α = confidence (from FeatureBanditLearner), used in: α*R + (1-α)*prior
        // This is automatic based on visit counts, not configurable

        // Beta (β): Exploration weight for UCB bonus
        // Higher = more exploration of untried items
        // Range: 0.0 - 1.0, Recommended: 0.15 - 0.3
        float explorationWeight = 0.2f;

        // ---------------------------------------------------------------------
        // Favorites System
        // ---------------------------------------------------------------------

        // How to handle favorited items
        FavoritesMode favoritesMode = FavoritesMode::Boost;

        // Boost range for favorited items (when mode = Boost)
        // Utility is multiplied by a value in [favoritesBoostMin, favoritesBoostMax]
        // Scaled by item's utility rank (top items get higher boost)
        float favoritesBoostMin = 1.3f;
        float favoritesBoostMax = 2.5f;

        // ---------------------------------------------------------------------
        // Correlation Bonuses
        // ---------------------------------------------------------------------

        // Bow + Arrows correlation
        float bowArrowBonus = 2.0f;

        // Crossbow + Bolts correlation
        float crossbowBoltBonus = 2.0f;

        // Melee + No Shield → Defensive items (wards, shields)
        float meleeDefensiveBonus = 1.5f;

        // Silver weapon + Undead target
        float silverUndeadBonus = 2.0f;

        // Fortify School synergy (v0.8.x)
        // When player has Fortify Destruction active, Destruction spells get bonus
        // This is a real game mechanic - fortify potions boost spell effectiveness
        float fortifySchoolBonus = 2.0f;

        // Staff equipped + low magicka → Staff spells bonus
        float staffLowMagickaBonus = 1.5f;

        // Two-handed + enemies behind → suggest defensive
        float twoHandedDefensiveBonus = 1.2f;

        // ---------------------------------------------------------------------
        // Potion Discrimination
        // ---------------------------------------------------------------------

        // Combat start window (seconds): Regen potions get bonus in first N seconds
        float combatStartWindow = 10.0f;

        // Regen potion bonus during combat start
        float regenPotionCombatStartMult = 1.5f;

        // Flat restore bonus when resource < 30%
        float flatRestoreLowResourceMult = 1.5f;

        // Which strength of the same potion ranks first -- see
        // UtilityScorer::ApplyPotionTierPreference
        PotionTierPreference potionTierPreference = PotionTierPreference::Higher;

        // ---------------------------------------------------------------------
        // Item-context fit (FitScorer)
        // ---------------------------------------------------------------------

        // Off / Shadow (compute + record, don't apply) / Apply. Shadow by
        // default so shipping it changes no ranking and soak runs stay valid.
        FitMode fitMode = FitMode::Shadow;

        // Affordability ramp: fitAffordMin at exactly one cast left, 1.0 at
        // fitAffordFullCasts or more. Below one cast: Penalize policy =>
        // fitUnaffordableMult; Allow/Disallow => hold at fitAffordMin.
        float fitAffordMin = 0.7f;
        float fitAffordFullCasts = 3.0f;
        float fitUnaffordableMult = 0.3f;

        // Concentration spells cost magicka per second; one "cast" is taken
        // to be this many seconds of sustain.
        float fitConcentrationSecondsPerCast = 2.0f;

        // Aimed spell whose projectile range is shorter than the nearest enemy.
        float fitOutOfRangeMult = 0.6f;

        // Output clamp. Max 1.0 means fit only ever demotes -- deliberate: it
        // breaks ties among near-equal items and cannot promote one.
        float fitClampMin = 0.2f;
        float fitClampMax = 1.0f;

        // ---------------------------------------------------------------------
        // Scoring Thresholds
        // ---------------------------------------------------------------------

        // Minimum utility score to include in results
        // Items below this are filtered out (irrelevant)
        float minimumUtility = 0.1f;

        // Minimum context weight to consider item relevant
        // Items with lower context weight are filtered out early
        float minimumContextWeight = 0.05f;

        // Cold-start UCB context boost: when too few candidates pass minimumUtility,
        // untried items get their context weight raised to coldStartUCBBoost × UCB.
        // UCB = 1.0 for unvisited items, decays with visits. Set to 0.0 to disable.
        float coldStartUCBBoost = 0.2f;

        // ---------------------------------------------------------------------
        // Performance
        // ---------------------------------------------------------------------

        // Maximum candidates to score per cycle
        // 0 = no limit
        size_t maxCandidatesPerCycle = 500;

        // Number of top candidates to fully sort (rest are partial sorted)
        // Must be >= max slots per page (10) so all slots get correctly ranked fills
        size_t topNCandidates = 10;

        // ---------------------------------------------------------------------
        // Helper Methods
        // ---------------------------------------------------------------------

        // Get favorites multiplier for an item based on its rank
        // rank 0 = top item (gets max boost), higher ranks get less boost
        [[nodiscard]] float GetFavoritesMultiplier(size_t rank, size_t totalItems) const noexcept
        {
            if (favoritesMode != FavoritesMode::Boost || totalItems == 0) {
                return 1.0f;
            }
            // Linear interpolation from max to min based on rank position
            float t = (totalItems > 1) ? static_cast<float>(rank) / static_cast<float>(totalItems - 1) : 0.0f;
            return favoritesBoostMax - t * (favoritesBoostMax - favoritesBoostMin);
        }
    };

    // =============================================================================
    // DEFAULT CONFIGURATION
    // =============================================================================
    inline constexpr ScorerConfig DefaultScorerConfig{};

}  // namespace Huginn::Scoring
