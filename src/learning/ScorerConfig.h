#pragma once

namespace Huginn::Scoring
{
    // =============================================================================
    // FAVORITES MODE - How to handle player-favorited items in scoring
    // =============================================================================
    enum class FavoritesMode : uint8_t
    {
        Boost,      // Favorited items get utility multiplied by favoritesBoostMax (the legacy floors' input)
        Off,        // Ignore favorites flag entirely (neutral scoring)
        Suppress    // Skip favorited items from recommendations (they're already accessible)
    };

    // R8.4 (0.24.1): the potion tier preference (which strength of one potion
    // ranked first, by a fixed utility step) is gone with its INI key. It
    // only ever reordered the utility ranking, which nothing ranks on since
    // R8.3; the choice score orders strengths through cap(i).

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

        // Boost for favorited items (when mode = Boost): a favourite's utility
        // is multiplied by favoritesBoostMax. R8.4 (0.24.1): only the uniform
        // multiplier the old floors test against is left (the "provisional"
        // value the floor pass always used); the rank scaling that replaced it
        // after the floors, and its lower end, are gone. A legacy feature
        // input only: the choice score's favourite term is the battery on b
        // ([Favorites] fFavoriteLifeMult).
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

        // How many top candidates count as ranked: the cold-start floor (fewer
        // scored than this triggers it) and the ranks the selection log reports.
        // The scorer sorts the WHOLE list since R7 (0.23.11); this no longer
        // limits the sort.
        size_t topNCandidates = 10;

        // ---------------------------------------------------------------------
        // Helper Methods
        // ---------------------------------------------------------------------

        // The favourites multiplier in the legacy utility: favoritesBoostMax
        // for a favourite in Boost mode, else 1. (Suppress favourites never
        // reach scoring: the favorite_suppressed filter drops them, R8.1.)
        [[nodiscard]] float FavoritesMultiplier(bool favorited) const noexcept
        {
            return (favoritesMode == FavoritesMode::Boost && favorited) ? favoritesBoostMax : 1.0f;
        }
    };

    // =============================================================================
    // DEFAULT CONFIGURATION
    // =============================================================================
    inline constexpr ScorerConfig DefaultScorerConfig{};

}  // namespace Huginn::Scoring
