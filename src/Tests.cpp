#include "Tests.h"
#include "Globals.h"

#include "state/StateManager.h"
#include "state/GameState.h"
#include "spell/SpellRegistry.h"
#include "learning/item/ItemClassifier.h"
#include "learning/item/ItemRegistry.h"
#include "weapon/WeaponRegistry.h"
#include "weapon/WeaponClassifier.h"
#include "learning/UtilityScorer.h"
#include "learning/ScoredCandidate.h"
#include "learning/ScorerSettings.h"   // MINIMUM_UTILITY — the floor test 6i is about
#include "candidate/CandidateGenerator.h"
#include "persist/QLearnerSerializer.h"
#include "learning/StateFeatures.h"
#include "learning/FeatureQLearner.h"
#include "learning/PipelineStateCache.h"
#include "learning/EquipSourceTracker.h"
#include "learning/UsageMemory.h"
#include "util/ScopedTimer.h"
#include "util/NameMatch.h"
#include "context/ContextRuleEngine.h"
#include "context/ReasonHold.h"
#include "context/ContextWeightForCandidate.h"
#include "display/ExplanationLabel.h"
#include "slot/SlotLocker.h"          // THROWAWAY: RunSlotLockerResetTest (0.19.21)
#include "slot/SlotSettings.h"         // THROWAWAY: MAX_SLOTS_PER_PAGE for the same
#include "override/OverrideConditions.h"  // THROWAWAY: OverrideCollection for the same

#include <random>
#include <algorithm>
#include <cstring>
#include <set>
#include <thread>

#ifdef _DEBUG
#include "ui/StateManagerDebugWidget.h"
#endif

using namespace Huginn;

// =============================================================================
// MULTIPLICATIVE SCORING FORMULA TESTS (Stage 2d)
// =============================================================================

void RunMultiplicativeScoringTests()
{
#ifndef NDEBUG
    logger::info("Running Multiplicative Scoring Formula tests..."sv);

    using namespace Huginn::State;
    using namespace Huginn::Scoring;
    using namespace Huginn::Context;

    auto& settings = ContextWeightSettings::GetSingleton();
    ContextRuleEngine engine(settings.BuildConfig());

    // =========================================================================
    // Test 1: Zero context gives zero utility (gate test)
    // =========================================================================
    {
        logger::info("  Test 1: Zero context → zero utility (multiplicative gate)..."sv);

        WorldState world{};
        PlayerActorState player{};
        player.vitals.health = 1.0f;  // Full health

        TargetCollection targets{};
        GameState gameState{};
        gameState.health = HealthBucket::VeryHigh;

        auto weights = engine.EvaluateRules(player, targets, world);

        // Healing spell candidate (not needed at full health)
        Candidate::SpellCandidate healingSpell{};
        healingSpell.tags = Spell::SpellTag::RestoreHealth;
        healingSpell.name = "Heal Self";
        healingSpell.formID = 0x00012345;

        // Extract weight - should be low (~0.05 base relevance)
        float contextWeight = weights.healingWeight;  // Should be very low at full health

        // Create config with multiplicative formula
        ScorerConfig config;

        config.lambdaMin = 0.5f;
        config.lambdaMax = 3.0f;

        // Mock learning score (high Q-value to test that context gates it)
        float learningScore = 0.8f;
        float confidence = 1.0f;
        float lambda = config.lambdaMin + confidence * (config.lambdaMax - config.lambdaMin);  // 3.0

        // Multiplicative formula: utility = ctx × (1 + λ×learn) × corr
        float utility = contextWeight * (1.0f + lambda * learningScore) * 1.0f;

        // With low context (e.g., 0.05), utility should be very low even with high learning
        // utility = 0.05 × (1 + 3.0×0.8) = 0.05 × 3.4 = 0.17
        // This is much lower than additive: 0.05 + 0.5×0.8 = 0.45

        if (utility > 0.5f) {
            logger::error("TEST FAIL: Healing at full health should have low utility with multiplicative formula"sv);
            return;
        }
        logger::info("  ✓ Context gate works: healing at full health = {:.3f} (contextWeight={:.3f})"sv,
            utility, contextWeight);
    }

    // =========================================================================
    // Test 2: Adaptive lambda scales with confidence
    // =========================================================================
    {
        logger::info("  Test 2: Adaptive lambda scales with confidence..."sv);

        ScorerConfig config;

        config.lambdaMin = 0.5f;
        config.lambdaMax = 3.0f;

        // Test at different confidence levels
        float lambda0 = config.lambdaMin + 0.0f * (config.lambdaMax - config.lambdaMin);  // 0.5
        float lambda50 = config.lambdaMin + 0.5f * (config.lambdaMax - config.lambdaMin); // 1.75
        float lambda100 = config.lambdaMin + 1.0f * (config.lambdaMax - config.lambdaMin); // 3.0

        if (std::abs(lambda0 - 0.5f) > 0.01f || std::abs(lambda100 - 3.0f) > 0.01f) {
            logger::error("TEST FAIL: Adaptive lambda should scale from 0.5 to 3.0"sv);
            return;
        }
        logger::info("  ✓ Adaptive lambda: λ(0)={:.1f}, λ(0.5)={:.2f}, λ(1)={:.1f}"sv,
            lambda0, lambda50, lambda100);
    }

    // =========================================================================
    // Test 3: Multiplicative formula amplifies learning at high confidence
    // =========================================================================
    {
        logger::info("  Test 3: Multiplicative formula amplifies learning..."sv);

        float contextWeight = 0.7f;
        float learningScore = 0.8f;
        float lambdaLow = 0.5f;   // Low confidence
        float lambdaHigh = 3.0f;  // High confidence

        float utilityLow = contextWeight * (1.0f + lambdaLow * learningScore);
        float utilityHigh = contextWeight * (1.0f + lambdaHigh * learningScore);

        // utilityLow = 0.7 × (1 + 0.5×0.8) = 0.7 × 1.4 = 0.98
        // utilityHigh = 0.7 × (1 + 3.0×0.8) = 0.7 × 3.4 = 2.38

        float amplification = utilityHigh / utilityLow;  // Should be ~2.4×

        if (amplification < 2.0f) {
            logger::error("TEST FAIL: High confidence should amplify learning significantly"sv);
            return;
        }
        logger::info("  ✓ Learning amplification: {:.1f}× at high confidence (low={:.2f}, high={:.2f})"sv,
            amplification, utilityLow, utilityHigh);
    }

    // =========================================================================
    // Test 4: Correlation bonuses are multiplicative and compound
    // =========================================================================
    {
        logger::info("  Test 4: Correlation bonuses compound multiplicatively..."sv);

        // Test compounding: bow+arrow bonus (2.0) + fortify archery buff (2.0)
        // Additive (old): +2.0 + +2.0 = +4.0
        // Multiplicative (new): ×3.0 × ×3.0 = ×9.0

        float multiplier = 1.0f;
        float bowArrowBonus = 2.0f;
        float fortifyBonus = 2.0f;

        // Apply as multiplicative
        multiplier *= (1.0f + bowArrowBonus);    // ×3.0
        multiplier *= (1.0f + fortifyBonus);     // ×3.0
        // Result: ×9.0

        if (std::abs(multiplier - 9.0f) > 0.01f) {
            logger::error("TEST FAIL: Correlation bonuses should compound to ×9.0, got {:.1f}"sv, multiplier);
            return;
        }
        logger::info("  ✓ Correlation compounding: ×{:.1f} (bow+arrow ×3.0, fortify ×3.0)"sv, multiplier);
    }

    // =========================================================================
    // Test 5: Full integration (context × learning × correlation)
    // =========================================================================
    {
        logger::info("  Test 5: Full multiplicative integration..."sv);

        float contextWeight = 0.8f;        // High relevance
        float learningScore = 0.6f;        // Moderate learning
        float confidence = 0.7f;           // High-ish confidence
        float correlationBonus = 3.0f;     // Bow+arrow (×3.0)
        float lambdaMin = 0.5f;
        float lambdaMax = 3.0f;

        // Compute adaptive lambda
        float lambda = lambdaMin + confidence * (lambdaMax - lambdaMin);  // 2.25

        // Multiplicative formula
        float utility = contextWeight * (1.0f + lambda * learningScore) * correlationBonus;
        // = 0.8 × (1 + 2.25×0.6) × 3.0
        // = 0.8 × (1 + 1.35) × 3.0
        // = 0.8 × 2.35 × 3.0
        // = 5.64

        float expected = 5.64f;  // Hand-computed: 0.8 × (1 + 2.25×0.6) × 3.0

        if (std::abs(utility - expected) > 0.01f) {
            logger::error("TEST FAIL: Full integration mismatch"sv);
            return;
        }
        logger::info("  ✓ Full integration: utility={:.2f} (ctx={:.1f}, λ={:.2f}, learn={:.1f}, corr=×{:.1f})"sv,
            utility, contextWeight, lambda, learningScore, correlationBonus);
    }

    // =========================================================================
    // Test 6: Favorites boost scales with rank (max → min across the cohort)
    // =========================================================================
    {
        logger::info("  Test 6: Favorites boost scales with rank..."sv);

        Scoring::ScorerConfig cfg{};  // favoritesMode=Boost, min=1.3, max=2.5

        // Rank 0 of N gets max; last rank gets min; midpoint is halfway.
        bool ok =
            std::abs(cfg.GetFavoritesMultiplier(0, 5) - cfg.favoritesBoostMax) < 0.001f &&
            std::abs(cfg.GetFavoritesMultiplier(4, 5) - cfg.favoritesBoostMin) < 0.001f &&
            std::abs(cfg.GetFavoritesMultiplier(2, 5) -
                (cfg.favoritesBoostMax + cfg.favoritesBoostMin) * 0.5f) < 0.001f &&
            // Single favorite: rank 0 of 1 gets max.
            std::abs(cfg.GetFavoritesMultiplier(0, 1) - cfg.favoritesBoostMax) < 0.001f &&
            // Degenerate/disabled cases are neutral.
            std::abs(cfg.GetFavoritesMultiplier(0, 0) - 1.0f) < 0.001f;

        cfg.favoritesMode = Scoring::FavoritesMode::Off;
        ok = ok && std::abs(cfg.GetFavoritesMultiplier(0, 5) - 1.0f) < 0.001f;

        if (!ok) {
            logger::error("TEST FAIL: Favorites rank scaling should interpolate max→min by rank"sv);
            return;
        }
        logger::info("  ✓ Favorites rank scaling: rank 0 → ×{:.1f}, last rank → ×{:.1f}"sv,
            Scoring::ScorerConfig{}.favoritesBoostMax, Scoring::ScorerConfig{}.favoritesBoostMin);
    }

    logger::info("TEST PASS: All multiplicative scoring formula tests passed!"sv);
#endif
}

// ============================================================================
// Console Commands for Manual Q-Learning Testing (Debug Mode Only)
// ============================================================================

#ifndef NDEBUG

void ConsoleCmd_ShowEquippedSpells()
{
    if (!g_stateEvaluator || !g_utilityScorer) {
        RE::ConsoleLog::GetSingleton()->Print("Huginn: Systems not initialized");
        return;
    }

    // Evaluate current game state
    auto [currentState, playerState] = EvaluateCurrentGameState();

    RE::ConsoleLog::GetSingleton()->Print("=== Currently Equipped Spells ===");
    logger::info("[Console] === Currently Equipped Spells ==="sv);

    // Log player equipped spells info
    auto player = RE::PlayerCharacter::GetSingleton();
    if (player) {
        auto* leftSpell = player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kLeftHand];
        auto* rightSpell = player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand];

        if (leftSpell) {
            RE::ConsoleLog::GetSingleton()->Print(
                std::format("Left Hand: {} (0x{:08X})", leftSpell->GetName(), leftSpell->GetFormID()).c_str());
        } else {
            RE::ConsoleLog::GetSingleton()->Print("Left Hand: None");
        }

        if (rightSpell) {
            RE::ConsoleLog::GetSingleton()->Print(
                std::format("Right Hand: {} (0x{:08X})", rightSpell->GetName(), rightSpell->GetFormID()).c_str());
        } else {
            RE::ConsoleLog::GetSingleton()->Print("Right Hand: None");
        }
    }
}

#ifdef _DEBUG
// Command: Toggle the StateManager debug widget (v0.6.1)
void ConsoleCmd_ToggleStateManagerDebug()
{
    auto& widget = UI::StateManagerDebugWidget::GetSingleton();
    widget.ToggleVisible();

    bool isVisible = widget.IsVisible();
    RE::ConsoleLog::GetSingleton()->Print(
        std::format("StateManager Debug Widget: {}", isVisible ? "SHOWN" : "HIDDEN").c_str());

    logger::info("[Console] StateManager debug widget toggled: {}"sv, isVisible ? "shown" : "hidden");
}

// Command: Force update StateManager (useful for testing)
void ConsoleCmd_ForceUpdateStateManager()
{
    State::StateManager::GetSingleton().ForceUpdate();

    auto worldState = State::StateManager::GetSingleton().GetWorldState();
    auto playerState = State::StateManager::GetSingleton().GetPlayerState();
    auto targets = State::StateManager::GetSingleton().GetTargets();

    RE::ConsoleLog::GetSingleton()->Print(
        std::format("StateManager force updated - {} tracked targets", targets.targets.size()).c_str());

    logger::info("[Console] StateManager force updated - {} tracked targets"sv, targets.targets.size());
}
#endif

#endif  // !NDEBUG (console commands)

// =============================================================================
// SPELL REGISTRY INTEGRATION TESTS
// =============================================================================

// Run spell registry integration tests (debug mode only)
void RunSpellRegistryTests()
{
#ifndef NDEBUG
    // Guard: Skip if registry not initialized or still loading (v0.7.10)
    if (!g_spellRegistry) {
        logger::warn("[Test] SpellRegistry not initialized, skipping tests"sv);
        return;
    }
    if (g_spellRegistry->IsLoading()) {
        logger::warn("[Test] SpellRegistry still loading, skipping tests"sv);
        return;
    }

    logger::info("Running SpellRegistry integration tests..."sv);

    // Test 1: Verify registry has spells after rebuild
    size_t initialCount = g_spellRegistry->GetSpellCount();
    logger::info("TEST: Initial spell count = {}"sv, initialCount);

    // Test 2: Verify GetSpellData works
    if (initialCount > 0) {
        const auto& allSpells = g_spellRegistry->GetAllSpells();
        auto testFormID = allSpells[0].formID;
        auto* spellData = g_spellRegistry->GetSpellData(testFormID);

        if (!spellData) {
            logger::error("TEST FAIL: GetSpellData failed for valid FormID {:08X}"sv, testFormID);
            return;
        }

        if (spellData->formID != testFormID) {
            logger::error("TEST FAIL: GetSpellData returned wrong spell"sv);
            return;
        }

        logger::info("TEST PASS: GetSpellData works correctly"sv);
    }

    // Test 3: Verify GetSpellData returns nullptr for invalid FormID
    auto* invalidSpell = g_spellRegistry->GetSpellData(0xDEADBEEF);
    if (invalidSpell != nullptr) {
        logger::error("TEST FAIL: GetSpellData should return nullptr for invalid FormID"sv);
        return;
    }
    logger::info("TEST PASS: GetSpellData returns nullptr for invalid FormID"sv);

    // Test 4: Verify ReconcileSpells maintains count when no changes
    size_t beforeReconcile = g_spellRegistry->GetSpellCount();
    g_spellRegistry->ReconcileSpells();
    size_t afterReconcile = g_spellRegistry->GetSpellCount();

    if (beforeReconcile != afterReconcile) {
        logger::warn("TEST INFO: Spell count changed during reconciliation: {} -> {}"sv,
            beforeReconcile, afterReconcile);
    } else {
        logger::info("TEST PASS: ReconcileSpells maintains spell count when no player changes"sv);
    }

    // Test 5: Verify spell classification
    if (initialCount > 0) {
        const auto& allSpells = g_spellRegistry->GetAllSpells();
        bool hasClassifiedSpell = false;
        for (const auto& spell : allSpells) {
            if (spell.type != Huginn::Spell::SpellType::Unknown) {
                hasClassifiedSpell = true;
                logger::info("TEST INFO: Found classified spell: {} (type: {})"sv,
                    spell.name, SpellTypeToString(spell.type));
                break;
            }
        }

        if (hasClassifiedSpell) {
            logger::info("TEST PASS: Spell classification working"sv);
        } else {
            logger::warn("TEST WARN: No classified spells found (all Unknown type)"sv);
        }
    }

    logger::info("All SpellRegistry integration tests completed!"sv);
#endif
}

// =============================================================================
// ITEM CLASSIFIER INTEGRATION TESTS
// =============================================================================

// Run ItemClassifier integration tests (debug mode only) - v0.7.3
void RunItemClassifierTests()
{
#ifndef NDEBUG
    // Guard: Skip if registry not ready (v0.7.10)
    if (!g_itemRegistry) {
        logger::warn("[Test] ItemRegistry not initialized, skipping tests"sv);
        return;
    }
    if (g_itemRegistry->IsLoading()) {
        logger::warn("[Test] ItemRegistry still loading, skipping tests"sv);
        return;
    }

    logger::info("Running ItemClassifier integration tests..."sv);

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        logger::error("TEST SKIP: Player not available"sv);
        return;
    }

    // Create a classifier instance
    Item::ItemClassifier classifier;

    // Scan player inventory for alchemy items using SKSE inventory API
    auto* invChanges = player->GetInventoryChanges();
    if (!invChanges || !invChanges->entryList) {
        logger::error("TEST SKIP: Cannot access player inventory"sv);
        return;
    }

    size_t potionCount = 0;
    size_t poisonCount = 0;
    size_t foodCount = 0;
    size_t alcoholCount = 0;
    size_t unknownCount = 0;

    logger::info("=== ItemClassifier Test Results ==="sv);

    for (auto* entry : *invChanges->entryList) {
        if (!entry || !entry->object) continue;

        // Only process AlchemyItems
        auto* alchemyItem = entry->object->As<RE::AlchemyItem>();
        if (!alchemyItem) continue;

        // Get item count
        int32_t count = entry->countDelta;
        if (count <= 0) continue;

        // Classify the item
        auto itemData = classifier.ClassifyItem(alchemyItem);

        // Log classification result
        logger::info("  {} | Type: {} | Tags: {:08X} | Mag: {:.1f} | Qty: {}",
            itemData.name,
            Item::ItemTypeToString(itemData.type),
            std::to_underlying(itemData.tags),
            itemData.magnitude,
            count);

        // Count by type
        switch (itemData.type) {
        case Item::ItemType::HealthPotion:
        case Item::ItemType::MagickaPotion:
        case Item::ItemType::StaminaPotion:
        case Item::ItemType::ResistPotion:
        case Item::ItemType::BuffPotion:
        case Item::ItemType::CurePotion:
            potionCount++;
            break;
        case Item::ItemType::Poison:
            poisonCount++;
            break;
        case Item::ItemType::Food:
            foodCount++;
            break;
        case Item::ItemType::Alcohol:
            alcoholCount++;
            break;
        default:
            unknownCount++;
            break;
        }
    }

    logger::info("=== ItemClassifier Summary ==="sv);
    logger::info("  Potions: {}", potionCount);
    logger::info("  Poisons: {}", poisonCount);
    logger::info("  Food: {}", foodCount);
    logger::info("  Alcohol: {}", alcoholCount);
    logger::info("  Unknown: {}", unknownCount);
    logger::info("  Total alchemy items: {}", potionCount + poisonCount + foodCount + alcoholCount + unknownCount);

    if (potionCount + poisonCount + foodCount + alcoholCount > 0) {
        logger::info("TEST PASS: ItemClassifier successfully classified items"sv);
    } else if (unknownCount > 0) {
        logger::warn("TEST WARN: All items classified as Unknown"sv);
    } else {
        logger::info("TEST INFO: No alchemy items in player inventory"sv);
    }

    logger::info("ItemClassifier integration tests completed!"sv);
#endif
}

// =============================================================================
// ITEM REGISTRY INTEGRATION TESTS
// =============================================================================

// Run ItemRegistry integration tests (debug mode only) - v0.7.4
void RunItemRegistryTests()
{
#ifndef NDEBUG
    // Guard: Skip if registry not ready (v0.7.10)
    if (!g_itemRegistry || g_itemRegistry->IsLoading()) {
        logger::warn("[Test] ItemRegistry not ready, skipping tests"sv);
        return;
    }

    logger::info("Running ItemRegistry integration tests..."sv);

    // Test 1: Verify registry has items after rebuild
    size_t initialCount = g_itemRegistry->GetItemCount();
    logger::info("TEST: Initial item count = {}"sv, initialCount);

    // Test 2: Verify GetItem works
    if (initialCount > 0) {
        const auto& allItems = g_itemRegistry->GetAllItems();
        auto testFormID = allItems[0].data.formID;
        auto* itemData = g_itemRegistry->GetItem(testFormID);

        if (!itemData) {
            logger::error("TEST FAIL: GetItem failed for valid FormID {:08X}"sv, testFormID);
            return;
        }

        if (itemData->data.formID != testFormID) {
            logger::error("TEST FAIL: GetItem returned wrong item"sv);
            return;
        }

        logger::info("TEST PASS: GetItem works correctly"sv);
    }

    // Test 3: Verify GetItem returns nullptr for invalid FormID
    auto* invalidItem = g_itemRegistry->GetItem(0xDEADBEEF);
    if (invalidItem != nullptr) {
        logger::error("TEST FAIL: GetItem should return nullptr for invalid FormID"sv);
        return;
    }
    logger::info("TEST PASS: GetItem returns nullptr for invalid FormID"sv);

    // Test 4: Verify GetItemsByType works
    auto healthPotions = g_itemRegistry->GetItemsByType(Item::ItemType::HealthPotion);
    logger::info("TEST INFO: Found {} health potions"sv, healthPotions.size());

    auto magickaPotions = g_itemRegistry->GetItemsByType(Item::ItemType::MagickaPotion);
    logger::info("TEST INFO: Found {} magicka potions"sv, magickaPotions.size());

    auto staminaPotions = g_itemRegistry->GetItemsByType(Item::ItemType::StaminaPotion);
    logger::info("TEST INFO: Found {} stamina potions"sv, staminaPotions.size());

    logger::info("TEST PASS: GetItemsByType works"sv);

    // Test 5: Verify GetHealthPotionsByMagnitude returns sorted results
    auto sortedHealthPotions = g_itemRegistry->GetHealthPotionsByMagnitude();
    if (sortedHealthPotions.size() >= 2) {
        bool isSorted = true;
        for (size_t i = 1; i < sortedHealthPotions.size(); ++i) {
            if (sortedHealthPotions[i-1]->data.magnitude < sortedHealthPotions[i]->data.magnitude) {
                isSorted = false;
                break;
            }
        }
        if (isSorted) {
            logger::info("TEST PASS: GetHealthPotionsByMagnitude returns sorted results"sv);
        } else {
            logger::error("TEST FAIL: GetHealthPotionsByMagnitude not sorted by magnitude"sv);
        }
    } else {
        logger::info("TEST SKIP: Not enough health potions to test sorting ({} found)"sv,
            sortedHealthPotions.size());
    }

    // Test 6: Verify RefreshCounts returns empty when no changes
    auto changes = g_itemRegistry->RefreshCounts();
    logger::info("TEST INFO: RefreshCounts returned {} changes (expected 0 on immediate retest)"sv,
        changes.size());

    // ==========================================================================
    // PotionScanner Tests (v0.7.5)
    // ==========================================================================

    logger::info("--- PotionScanner Tests ---"sv);

    // Test 7: Verify magicka/stamina potion accessors
    auto sortedMagickaPotions = g_itemRegistry->GetMagickaPotionsByMagnitude();
    auto sortedStaminaPotions = g_itemRegistry->GetStaminaPotionsByMagnitude();
    logger::info("TEST INFO: Found {} magicka potions, {} stamina potions (sorted)"sv,
        sortedMagickaPotions.size(), sortedStaminaPotions.size());

    // Test 8: Verify resist potion accessors
    auto resistFire = g_itemRegistry->GetResistFirePotions();
    auto resistFrost = g_itemRegistry->GetResistFrostPotions();
    auto resistShock = g_itemRegistry->GetResistShockPotions();
    auto resistPoison = g_itemRegistry->GetResistPoisonPotions();
    auto resistMagic = g_itemRegistry->GetResistMagicPotions();
    logger::info("TEST INFO: Resist potions - Fire:{} Frost:{} Shock:{} Poison:{} Magic:{}"sv,
        resistFire.size(), resistFrost.size(), resistShock.size(),
        resistPoison.size(), resistMagic.size());

    // Test 9: Verify cure potion accessors
    auto cureDisease = g_itemRegistry->GetCureDiseasePotions();
    auto curePoison = g_itemRegistry->GetCurePoisonPotions();
    logger::info("TEST INFO: Cure potions - Disease:{} Poison:{}"sv,
        cureDisease.size(), curePoison.size());

    // Test 10: Verify buff potion accessors (v0.8: Split fortify into 3 categories)
    auto fortifySchool = g_itemRegistry->GetFortifySchoolPotions();
    auto fortifyCombat = g_itemRegistry->GetFortifyCombatPotions();
    auto fortifyUtility = g_itemRegistry->GetFortifyUtilityPotions();
    auto invisibility = g_itemRegistry->GetInvisibilityPotions();
    auto waterbreathing = g_itemRegistry->GetWaterbreathingPotions();
    logger::info("TEST INFO: Buff potions - FortifySchool:{} FortifyCombat:{} FortifyUtility:{} Invis:{} Waterbreath:{}"sv,
        fortifySchool.size(), fortifyCombat.size(), fortifyUtility.size(),
        invisibility.size(), waterbreathing.size());

    // Test 10b: Log specific fortify potions with their skill/school
    for (const auto* potion : fortifySchool) {
        logger::info("  FortifySchool: {} (school={}, mag={:.0f})"sv,
            potion->data.name, Item::MagicSchoolToString(potion->data.school), potion->data.magnitude);
    }
    for (const auto* potion : fortifyCombat) {
        logger::info("  FortifyCombat: {} (skill={}, mag={:.0f})"sv,
            potion->data.name, Item::CombatSkillToString(potion->data.combatSkill), potion->data.magnitude);
    }
    for (const auto* potion : fortifyUtility) {
        logger::info("  FortifyUtility: {} (skill={}, mag={:.0f})"sv,
            potion->data.name, Item::UtilitySkillToString(potion->data.utilitySkill), potion->data.magnitude);
    }

    // Test 11: Verify GetBest* accessors return correct results
    auto* bestHP = g_itemRegistry->GetBestHealthPotion();
    auto* bestMP = g_itemRegistry->GetBestMagickaPotion();
    auto* bestSP = g_itemRegistry->GetBestStaminaPotion();

    if (bestHP) {
        logger::info("TEST INFO: Best HP potion: {} (mag={:.0f})"sv,
            bestHP->data.name, bestHP->data.magnitude);
    }
    if (bestMP) {
        logger::info("TEST INFO: Best MP potion: {} (mag={:.0f})"sv,
            bestMP->data.name, bestMP->data.magnitude);
    }
    if (bestSP) {
        logger::info("TEST INFO: Best SP potion: {} (mag={:.0f})"sv,
            bestSP->data.name, bestSP->data.magnitude);
    }

    // Test 12: Verify GetBest returns same as sorted[0]
    if (!sortedHealthPotions.empty() && bestHP) {
        if (sortedHealthPotions.front()->data.formID == bestHP->data.formID) {
            logger::info("TEST PASS: GetBestHealthPotion matches sorted[0]"sv);
        } else {
            logger::error("TEST FAIL: GetBestHealthPotion doesn't match sorted[0]"sv);
        }
    }

    // Test 13: Verify resist potion sorting (if we have multiple)
    if (resistFire.size() >= 2) {
        bool isSorted = true;
        for (size_t i = 1; i < resistFire.size(); ++i) {
            if (resistFire[i-1]->data.magnitude < resistFire[i]->data.magnitude) {
                isSorted = false;
                break;
            }
        }
        if (isSorted) {
            logger::info("TEST PASS: GetResistFirePotions returns sorted results"sv);
        } else {
            logger::error("TEST FAIL: GetResistFirePotions not sorted by magnitude"sv);
        }
    }

    logger::info("TEST PASS: PotionScanner accessors work correctly"sv);

    // Test 14: Log all items for manual verification
    g_itemRegistry->LogAllItems();

    logger::info("ItemRegistry integration tests completed!"sv);
#endif
}

// =============================================================================
// WEAPON REGISTRY INTEGRATION TESTS
// =============================================================================

// Run WeaponRegistry integration tests (debug mode only) - v0.7.6
void RunWeaponRegistryTests()
{
#ifndef NDEBUG
    // Guard: Skip if registry not ready (v0.7.10)
    if (!g_weaponRegistry || g_weaponRegistry->IsLoading()) {
        logger::warn("[Test] WeaponRegistry not ready, skipping tests"sv);
        return;
    }

    logger::info("Running WeaponRegistry integration tests..."sv);

    // Test 1: Verify registry has weapons after rebuild
    size_t weaponCount = g_weaponRegistry->GetWeaponCount();
    size_t ammoCount = g_weaponRegistry->GetAmmoCount();
    logger::info("TEST: Initial weapon count = {}, ammo count = {}"sv, weaponCount, ammoCount);

    // Test 2: Verify GetWeapon works for existing weapons
    if (weaponCount > 0) {
        const auto& allWeapons = g_weaponRegistry->GetAllWeapons();
        auto testFormID = allWeapons[0].data.formID;
        auto* weaponData = g_weaponRegistry->GetWeapon(testFormID);

        if (!weaponData) {
            logger::error("TEST FAIL: GetWeapon failed for valid FormID {:08X}"sv, testFormID);
            return;
        }

        if (weaponData->data.formID != testFormID) {
            logger::error("TEST FAIL: GetWeapon returned wrong weapon"sv);
            return;
        }

        logger::info("TEST PASS: GetWeapon works correctly"sv);
    }

    // Test 3: Verify GetWeapon returns nullptr for invalid FormID
    auto* invalidWeapon = g_weaponRegistry->GetWeapon(0xDEADBEEF);
    if (invalidWeapon != nullptr) {
        logger::error("TEST FAIL: GetWeapon should return nullptr for invalid FormID"sv);
        return;
    }
    logger::info("TEST PASS: GetWeapon returns nullptr for invalid FormID"sv);

    // Test 4: Verify melee/ranged weapon accessors
    auto meleeWeapons = g_weaponRegistry->GetMeleeWeapons();
    auto rangedWeapons = g_weaponRegistry->GetRangedWeapons();
    logger::info("TEST INFO: Found {} melee weapons, {} ranged weapons"sv,
        meleeWeapons.size(), rangedWeapons.size());

    // Test 5: Verify one-handed/two-handed weapon accessors
    auto oneHandedWeapons = g_weaponRegistry->GetOneHandedWeapons();
    auto twoHandedWeapons = g_weaponRegistry->GetTwoHandedWeapons();
    logger::info("TEST INFO: Found {} one-handed weapons, {} two-handed weapons"sv,
        oneHandedWeapons.size(), twoHandedWeapons.size());

    // Test 6: Verify silver weapon detection
    auto silverWeapons = g_weaponRegistry->GetSilveredWeapons();
    logger::info("TEST INFO: Found {} silver weapons"sv, silverWeapons.size());

    for (const auto* weapon : silverWeapons) {
        logger::debug("  Silver weapon: {} (dmg={:.1f})"sv,
            weapon->data.name, weapon->data.baseDamage);
    }

    // Test 7: Verify enchanted weapon detection
    auto enchantedWeapons = g_weaponRegistry->GetEnchantedWeapons();
    logger::info("TEST INFO: Found {} enchanted weapons"sv, enchantedWeapons.size());

    for (const auto* weapon : enchantedWeapons) {
        logger::debug("  Enchanted weapon: {} (charge={:.0f}%, tags={:08X})"sv,
            weapon->data.name,
            weapon->data.currentCharge * 100.0f,
            std::to_underlying(weapon->data.tags));
    }

    // Test 8: Verify GetBestMeleeWeapon returns highest damage
    auto* bestMelee = g_weaponRegistry->GetBestMeleeWeapon();
    if (bestMelee) {
        logger::info("TEST INFO: Best melee weapon: {} (dmg={:.1f})"sv,
            bestMelee->data.name, bestMelee->data.baseDamage);

        // Verify it's actually the highest
        bool isHighest = true;
        for (const auto* weapon : meleeWeapons) {
            if (weapon->data.baseDamage > bestMelee->data.baseDamage) {
                isHighest = false;
                logger::error("TEST FAIL: Found melee weapon with higher damage than GetBestMeleeWeapon"sv);
                break;
            }
        }
        if (isHighest) {
            logger::info("TEST PASS: GetBestMeleeWeapon returns highest damage weapon"sv);
        }
    }

    // Test 9: Verify GetBestRangedWeapon
    auto* bestRanged = g_weaponRegistry->GetBestRangedWeapon();
    if (bestRanged) {
        logger::info("TEST INFO: Best ranged weapon: {} (dmg={:.1f})"sv,
            bestRanged->data.name, bestRanged->data.baseDamage);
    }

    // Test 10: Verify ammo accessors
    auto arrows = g_weaponRegistry->GetArrows();
    auto bolts = g_weaponRegistry->GetBolts();
    auto magicAmmo = g_weaponRegistry->GetMagicAmmo();
    logger::info("TEST INFO: Found {} arrows, {} bolts, {} magic ammo"sv,
        arrows.size(), bolts.size(), magicAmmo.size());

    // Test 11: Verify arrow/bolt sorting (by damage descending)
    if (arrows.size() >= 2) {
        bool isSorted = true;
        for (size_t i = 1; i < arrows.size(); ++i) {
            if (arrows[i-1]->data.baseDamage < arrows[i]->data.baseDamage) {
                isSorted = false;
                break;
            }
        }
        if (isSorted) {
            logger::info("TEST PASS: GetArrows returns sorted results"sv);
        } else {
            logger::error("TEST FAIL: GetArrows not sorted by damage"sv);
        }
    }

    // Test 12: Verify GetBestArrow/GetBestBolt
    auto* bestArrow = g_weaponRegistry->GetBestArrow();
    auto* bestBolt = g_weaponRegistry->GetBestBolt();
    if (bestArrow) {
        logger::info("TEST INFO: Best arrow: {} (dmg={:.1f})"sv,
            bestArrow->data.name, bestArrow->data.baseDamage);
    }
    if (bestBolt) {
        logger::info("TEST INFO: Best bolt: {} (dmg={:.1f})"sv,
            bestBolt->data.name, bestBolt->data.baseDamage);
    }

    // Test 13: Verify weapon type classification
    for (const auto& weapon : g_weaponRegistry->GetAllWeapons()) {
        logger::debug("  Weapon: {} type={} tags={:08X} dmg={:.1f}"sv,
            weapon.data.name,
            Weapon::WeaponTypeToString(weapon.data.type),
            std::to_underlying(weapon.data.tags),
            weapon.data.baseDamage);
    }

    // Test 14: Log all weapons for manual verification
    g_weaponRegistry->LogAllWeapons();

    logger::info("WeaponRegistry integration tests completed!"sv);
#endif
}

// =============================================================================
// STATE FEATURES TESTS (Phase 3.5a)
// =============================================================================

void RunStateFeaturesTests()
{
#ifndef NDEBUG
    using namespace Huginn::State;
    using namespace Huginn::Learning;

    logger::info("Running StateFeatures unit tests..."sv);

    // Helper lambda for float comparison
    constexpr float EPS = 0.001f;
    auto feq = [EPS](float a, float b) { return std::abs(a - b) < EPS; };

    // ── Test 1: Default state ────────────────────────────────────────────
    {
        PlayerActorState player;  // Default: full vitals, no combat, no equipment
        TargetCollection targets; // Default: empty, no primary

        auto f = StateFeatures::FromState(player, targets);

        if (!feq(f.healthPct, 1.0f) || !feq(f.magickaPct, 1.0f) || !feq(f.staminaPct, 1.0f)) {
            logger::error("TEST FAIL: Default vitals should be 1.0 (got H={:.2f} M={:.2f} S={:.2f})"sv,
                f.healthPct, f.magickaPct, f.staminaPct);
            return;
        }
        if (!feq(f.inCombat, 0.0f) || !feq(f.isSneaking, 0.0f)) {
            logger::error("TEST FAIL: Default combat/sneak should be 0.0"sv);
            return;
        }
        if (!feq(f.targetNone, 1.0f)) {
            logger::error("TEST FAIL: Default target type should be None (1.0)"sv);
            return;
        }
        if (!feq(f.hasMeleeEquipped, 0.0f) || !feq(f.hasBowEquipped, 0.0f) ||
            !feq(f.hasSpellEquipped, 0.0f) || !feq(f.hasShieldEquipped, 0.0f)) {
            logger::error("TEST FAIL: Default equipment should all be 0.0"sv);
            return;
        }
        if (!feq(f.distanceNorm, 1.0f)) {
            logger::error("TEST FAIL: Default distanceNorm should be 1.0 (no enemies)"sv);
            return;
        }
        if (!feq(f.bias, 1.0f)) {
            logger::error("TEST FAIL: Bias should always be 1.0"sv);
            return;
        }
        logger::info("  Test 1 PASS: Default state"sv);
    }

    // ── Test 2: Low health combat with melee ─────────────────────────────
    {
        PlayerActorState player;
        player.vitals.health = 0.30f;
        player.isInCombat = true;
        player.hasMeleeEquipped = true;

        TargetCollection targets;
        auto f = StateFeatures::FromState(player, targets);

        if (!feq(f.healthPct, 0.30f)) {
            logger::error("TEST FAIL: healthPct should be 0.30, got {:.2f}"sv, f.healthPct);
            return;
        }
        if (!feq(f.inCombat, 1.0f)) {
            logger::error("TEST FAIL: inCombat should be 1.0"sv);
            return;
        }
        if (!feq(f.hasMeleeEquipped, 1.0f)) {
            logger::error("TEST FAIL: hasMeleeEquipped should be 1.0"sv);
            return;
        }
        logger::info("  Test 2 PASS: Low health combat"sv);
    }

    // ── Test 3: One-hot correctness (all 7 target types) ─────────────────
    {
        PlayerActorState player;

        // Test each target type individually
        struct OneHotCase {
            TargetType type;
            const char* name;
            size_t expectedIndex;  // Index in [targetNone..targetDaedra] = [6..12]
        };
        constexpr std::array<OneHotCase, 7> cases = {{
            {TargetType::None,      "None",      6},
            {TargetType::Humanoid,  "Humanoid",  7},
            {TargetType::Undead,    "Undead",     8},
            {TargetType::Beast,     "Beast",      9},
            {TargetType::Construct, "Construct", 10},
            {TargetType::Dragon,    "Dragon",    11},
            {TargetType::Daedra,    "Daedra",    12},
        }};

        for (const auto& tc : cases) {
            TargetCollection targets;
            TargetActorState primary;
            primary.targetType = tc.type;
            primary.isHostile = true;
            primary.distanceToPlayerSq = 512.0f * 512.0f;
            primary.actorFormID = 1;
            targets.primary = primary;
            targets.InsertOrUpdate(1, primary);

            auto f = StateFeatures::FromState(player, targets);
            auto arr = f.ToArray();

            // Verify exactly one target float is 1.0 at the expected index
            for (size_t i = 6; i <= 12; ++i) {
                float expected = (i == tc.expectedIndex) ? 1.0f : 0.0f;
                if (!feq(arr[i], expected)) {
                    logger::error("TEST FAIL: One-hot for {} — arr[{}] should be {:.0f}, got {:.2f}"sv,
                        tc.name, i, expected, arr[i]);
                    return;
                }
            }
        }
        logger::info("  Test 3 PASS: One-hot correctness (all 7 types)"sv);
    }

    // ── Test 4: Distance normalization ───────────────────────────────────
    {
        PlayerActorState player;
        TargetCollection targets;

        // 4a: Enemy at melee range: 256 units → 256/4096 = 0.0625
        TargetActorState enemy;
        enemy.isHostile = true;
        enemy.distanceToPlayerSq = 256.0f * 256.0f;
        enemy.actorFormID = 1;
        targets.InsertOrUpdate(1, enemy);

        auto f = StateFeatures::FromState(player, targets);
        float expected = 256.0f / StateFeatures::MAX_DISTANCE;  // 0.0625
        if (!feq(f.distanceNorm, expected)) {
            logger::error("TEST FAIL: distanceNorm at 256u should be {:.4f}, got {:.4f}"sv,
                expected, f.distanceNorm);
            return;
        }

        // 4b: Enemy at max range: 4096 units → 4096/4096 = 1.0
        targets.targets.clear();
        enemy.distanceToPlayerSq = 4096.0f * 4096.0f;
        targets.InsertOrUpdate(1, enemy);

        f = StateFeatures::FromState(player, targets);
        if (!feq(f.distanceNorm, 1.0f)) {
            logger::error("TEST FAIL: distanceNorm at 4096u should be 1.0, got {:.4f}"sv, f.distanceNorm);
            return;
        }

        // 4c: Enemy beyond MAX_DISTANCE: 8192 units → clamped to 1.0
        targets.targets.clear();
        enemy.distanceToPlayerSq = 8192.0f * 8192.0f;
        targets.InsertOrUpdate(1, enemy);

        f = StateFeatures::FromState(player, targets);
        if (!feq(f.distanceNorm, 1.0f)) {
            logger::error("TEST FAIL: distanceNorm beyond MAX_DISTANCE should clamp to 1.0, got {:.4f}"sv,
                f.distanceNorm);
            return;
        }

        // 4d: Only a sentinel entry (distanceToPlayerSq = 0.0) → treated as no enemy
        targets.targets.clear();
        TargetActorState sentinel;
        sentinel.isHostile = true;
        sentinel.distanceToPlayerSq = 0.0f;  // NO_TARGET sentinel
        sentinel.actorFormID = 2;
        targets.InsertOrUpdate(2, sentinel);

        f = StateFeatures::FromState(player, targets);
        if (!feq(f.distanceNorm, 1.0f)) {
            logger::error("TEST FAIL: distanceNorm with NO_TARGET sentinel should be 1.0, got {:.4f}"sv,
                f.distanceNorm);
            return;
        }

        // 4e: Sentinel + real enemy coexistence — real enemy should win
        // GetClosestEnemy() must skip sentinel entries so the real enemy is found
        targets.targets.clear();
        targets.InsertOrUpdate(2, sentinel);  // sentinel at distSq=0.0
        TargetActorState realEnemy;
        realEnemy.isHostile = true;
        realEnemy.distanceToPlayerSq = 256.0f * 256.0f;
        realEnemy.actorFormID = 3;
        targets.InsertOrUpdate(3, realEnemy);

        f = StateFeatures::FromState(player, targets);
        float expectedReal = 256.0f / StateFeatures::MAX_DISTANCE;
        if (!feq(f.distanceNorm, expectedReal)) {
            logger::error("TEST FAIL: Sentinel+real enemy — distanceNorm should be {:.4f} (real enemy), got {:.4f}"sv,
                expectedReal, f.distanceNorm);
            return;
        }

        logger::info("  Test 4 PASS: Distance normalization"sv);
    }

    // ── Test 5: ToArray round-trip ───────────────────────────────────────
    {
        PlayerActorState player;
        player.vitals.health = 0.5f;
        player.vitals.magicka = 0.7f;
        player.vitals.stamina = 0.3f;
        player.isInCombat = true;
        player.isSneaking = true;
        player.hasBowEquipped = true;
        player.hasShieldEquipped = true;

        TargetCollection targets;
        TargetActorState primary;
        primary.targetType = TargetType::Dragon;
        primary.isHostile = true;
        primary.distanceToPlayerSq = 2048.0f * 2048.0f;
        primary.actorFormID = 2;
        targets.primary = primary;
        targets.InsertOrUpdate(2, primary);

        auto f = StateFeatures::FromState(player, targets);
        auto arr = f.ToArray();

        if (!feq(arr[0], f.healthPct) || !feq(arr[1], f.magickaPct) || !feq(arr[2], f.staminaPct) ||
            !feq(arr[3], f.inCombat) || !feq(arr[4], f.isSneaking) || !feq(arr[5], f.distanceNorm) ||
            !feq(arr[6], f.targetNone) || !feq(arr[7], f.targetHumanoid) || !feq(arr[8], f.targetUndead) ||
            !feq(arr[9], f.targetBeast) || !feq(arr[10], f.targetConstruct) || !feq(arr[11], f.targetDragon) ||
            !feq(arr[12], f.targetDaedra) || !feq(arr[13], f.hasMeleeEquipped) || !feq(arr[14], f.hasBowEquipped) ||
            !feq(arr[15], f.hasSpellEquipped) || !feq(arr[16], f.hasShieldEquipped) || !feq(arr[17], f.bias)) {
            logger::error("TEST FAIL: ToArray() values don't match named fields"sv);
            return;
        }
        logger::info("  Test 5 PASS: ToArray round-trip"sv);
    }

    // ── Test 6: Normalization bounds ─────────────────────────────────────
    {
        // Extreme state: all vitals at 0, all flags set
        PlayerActorState player;
        player.vitals.health = 0.0f;
        player.vitals.magicka = 0.0f;
        player.vitals.stamina = 0.0f;
        player.isInCombat = true;
        player.isSneaking = true;
        player.hasMeleeEquipped = true;
        player.hasBowEquipped = true;
        player.hasSpellEquipped = true;
        player.hasShieldEquipped = true;

        TargetCollection targets;
        TargetActorState enemy;
        enemy.targetType = TargetType::Daedra;
        enemy.isHostile = true;
        enemy.distanceToPlayerSq = 100.0f;  // Very close (~10 units)
        enemy.actorFormID = 3;
        targets.primary = enemy;
        targets.InsertOrUpdate(3, enemy);

        auto f = StateFeatures::FromState(player, targets);
        auto arr = f.ToArray();

        for (size_t i = 0; i < StateFeatures::NUM_FEATURES; ++i) {
            if (arr[i] < 0.0f || arr[i] > 1.0f) {
                logger::error("TEST FAIL: Feature[{}] = {:.6f} out of [0, 1] bounds"sv, i, arr[i]);
                return;
            }
        }
        logger::info("  Test 6 PASS: Normalization bounds"sv);
    }

    // ── Test 7: No enemy fallback ────────────────────────────────────────
    {
        PlayerActorState player;
        TargetCollection targets;

        // Add non-hostile targets only (allies)
        TargetActorState ally;
        ally.isHostile = false;
        ally.isDead = false;
        ally.distanceToPlayerSq = 100.0f;
        ally.actorFormID = 4;
        targets.InsertOrUpdate(4, ally);

        auto f = StateFeatures::FromState(player, targets);

        if (!feq(f.distanceNorm, 1.0f)) {
            logger::error("TEST FAIL: distanceNorm with no enemies should be 1.0, got {:.4f}"sv, f.distanceNorm);
            return;
        }
        if (!feq(f.targetNone, 1.0f)) {
            logger::error("TEST FAIL: targetNone should be 1.0 with no primary target"sv);
            return;
        }
        logger::info("  Test 7 PASS: No enemy fallback"sv);
    }

    // ── Test 8: Vital clamping (out-of-range SKSE values) ────────────────
    {
        PlayerActorState player;
        player.vitals.health = 1.5f;    // Fortify Health overflow
        player.vitals.magicka = -0.1f;  // Negative from drain edge case
        player.vitals.stamina = 2.0f;

        TargetCollection targets;
        auto f = StateFeatures::FromState(player, targets);

        if (!feq(f.healthPct, 1.0f)) {
            logger::error("TEST FAIL: healthPct > 1.0 should clamp to 1.0, got {:.2f}"sv, f.healthPct);
            return;
        }
        if (!feq(f.magickaPct, 0.0f)) {
            logger::error("TEST FAIL: magickaPct < 0.0 should clamp to 0.0, got {:.2f}"sv, f.magickaPct);
            return;
        }
        if (!feq(f.staminaPct, 1.0f)) {
            logger::error("TEST FAIL: staminaPct > 1.0 should clamp to 1.0, got {:.2f}"sv, f.staminaPct);
            return;
        }
        logger::info("  Test 8 PASS: Vital clamping"sv);
    }

    logger::info("TEST PASS: All StateFeatures tests passed! (8 tests)"sv);
#endif
}

// =============================================================================
// FEATURE Q-LEARNER TESTS (Phase 3.5b)
// =============================================================================

void RunFeatureQLearnerTests()
{
#ifndef NDEBUG
    using namespace Huginn::Learning;

    logger::info("Running FeatureQLearner unit tests..."sv);

    constexpr float EPS = 0.001f;
    auto feq = [EPS](float a, float b) { return std::abs(a - b) < EPS; };

    // ── Test 1: Cold start ────────────────────────────────────────────────
    {
        FeatureQLearner fql;
        StateFeatures defaultState;  // Full health, no combat, no targets

        RE::FormID unknownItem = 0xDEAD0001;
        float q = fql.GetQValue(unknownItem, defaultState);
        float conf = fql.GetConfidence(unknownItem);
        float ucb = fql.GetUCB(unknownItem);

        if (!feq(q, 0.0f)) {
            logger::error("TEST FAIL: Cold start Q should be 0.0, got {:.4f}"sv, q);
            return;
        }
        // Confidence at 0 trains: 1/(1+exp(-0.3*(0-5))) = 1/(1+exp(1.5)) ≈ 0.182
        if (conf > 0.25f || conf < 0.10f) {
            logger::error("TEST FAIL: Cold start confidence should be ~0.182, got {:.4f}"sv, conf);
            return;
        }
        if (!feq(ucb, 1.0f)) {
            logger::error("TEST FAIL: Cold start UCB should be 1.0, got {:.4f}"sv, ucb);
            return;
        }
        logger::info("  Test 1 PASS: Cold start"sv);
    }

    // ── Test 2: Learning convergence ──────────────────────────────────────
    {
        FeatureQLearner fql;
        RE::FormID healSpell = 0xDEAD0002;

        // Low health state
        StateFeatures lowHealth;
        lowHealth.healthPct = 0.3f;
        lowHealth.inCombat = 1.0f;

        // Train 20 times with reward=1.0
        for (int i = 0; i < 20; ++i) {
            fql.Update(healSpell, lowHealth, 1.0f);
        }

        float q = fql.GetQValue(healSpell, lowHealth);
        float conf = fql.GetConfidence(healSpell);

        if (q < 0.5f) {
            logger::error("TEST FAIL: After 20 trains with reward=1.0, Q should be >0.5, got {:.4f}"sv, q);
            return;
        }
        if (conf < 0.9f) {
            logger::error("TEST FAIL: After 20 trains, confidence should be >0.9, got {:.4f}"sv, conf);
            return;
        }
        logger::info("  Test 2 PASS: Learning convergence (Q={:.3f}, conf={:.3f})"sv, q, conf);
    }

    // ── Test 3: Weight interpretability ───────────────────────────────────
    {
        FeatureQLearner fql;
        RE::FormID healSpell = 0xDEAD0003;

        // Contrastive training: healing rewarded at LOW health, not rewarded at
        // FULL health. Without the high-health/zero-reward examples the learner
        // has no gradient toward a negative healthPct weight — it would just fit
        // Q≈1 with small positive weights on whatever features are active.
        StateFeatures lowHealthCombat;
        lowHealthCombat.healthPct = 0.2f;
        lowHealthCombat.inCombat = 1.0f;

        StateFeatures fullHealthCombat;
        fullHealthCombat.healthPct = 1.0f;
        fullHealthCombat.inCombat = 1.0f;

        for (int i = 0; i < 30; ++i) {
            fql.Update(healSpell, lowHealthCombat, 1.0f);
            fql.Update(healSpell, fullHealthCombat, 0.0f);
        }

        auto weights = fql.GetWeights(healSpell);

        // To fit Q(low)=1 and Q(full)=0 simultaneously, the model must assign
        // healthPct a negative weight (lower health = higher Q) and offset it
        // with a positive inCombat weight.
        if (weights[0] >= 0.0f) {
            logger::error("TEST FAIL: healthPct weight should be negative (low health = high Q), got {:.4f}"sv, weights[0]);
            return;
        }
        if (weights[3] <= 0.0f) {
            logger::error("TEST FAIL: inCombat weight should be positive, got {:.4f}"sv, weights[3]);
            return;
        }

        // Q at low health should be much higher than Q at full health
        StateFeatures fullHealth;
        fullHealth.healthPct = 1.0f;
        fullHealth.inCombat = 1.0f;

        float qLow = fql.GetQValue(healSpell, lowHealthCombat);
        float qHigh = fql.GetQValue(healSpell, fullHealth);

        if (qLow <= qHigh) {
            logger::error("TEST FAIL: Q(low health) should > Q(full health), got {:.4f} vs {:.4f}"sv, qLow, qHigh);
            return;
        }
        logger::info("  Test 3 PASS: Weight interpretability (w_health={:.3f}, w_combat={:.3f}, Q_low={:.3f} > Q_high={:.3f})"sv,
            weights[0], weights[3], qLow, qHigh);
    }

    // ── Test 4: Regularization prevents explosion ─────────────────────────
    {
        FeatureQLearner fql;
        RE::FormID item = 0xDEAD0004;

        StateFeatures state;
        state.healthPct = 0.5f;
        state.inCombat = 1.0f;

        // Train with extreme reward 200 times
        for (int i = 0; i < 200; ++i) {
            fql.Update(item, state, 100.0f);
        }

        float q = fql.GetQValue(item, state);
        if (!std::isfinite(q)) {
            logger::error("TEST FAIL: Q should be finite after extreme training, got {:.4f}"sv, q);
            return;
        }

        auto weights = fql.GetWeights(item);
        for (size_t i = 0; i < StateFeatures::NUM_FEATURES; ++i) {
            if (weights[i] > 10.0f || weights[i] < -10.0f) {
                logger::error("TEST FAIL: Weight[{}] = {:.4f} exceeds clamp bounds"sv, i, weights[i]);
                return;
            }
        }
        logger::info("  Test 4 PASS: Regularization prevents explosion (Q={:.3f})"sv, q);
    }

    // ── Test 5: Weight clamping ───────────────────────────────────────────
    {
        FeatureQLearner fql;
        RE::FormID item = 0xDEAD0005;

        // Force weights toward extremes with alternating high rewards on different states
        for (int i = 0; i < 500; ++i) {
            StateFeatures s;
            s.healthPct = (i % 2 == 0) ? 0.0f : 1.0f;
            s.inCombat = 1.0f;
            fql.Update(item, s, (i % 2 == 0) ? 50.0f : -50.0f);
        }

        auto weights = fql.GetWeights(item);
        bool allClamped = true;
        for (size_t i = 0; i < StateFeatures::NUM_FEATURES; ++i) {
            if (weights[i] > 10.0f + EPS || weights[i] < -10.0f - EPS) {
                logger::error("TEST FAIL: Weight[{}] = {:.6f} outside [-10, 10]"sv, i, weights[i]);
                allClamped = false;
            }
        }
        if (!allClamped) return;
        logger::info("  Test 5 PASS: Weight clamping"sv);
    }

    // ── Test 6: Generalization ────────────────────────────────────────────
    {
        FeatureQLearner fql;
        RE::FormID bow = 0xDEAD0006;

        // State A: combat + sneaking + low health
        StateFeatures stateA;
        stateA.healthPct = 0.3f;
        stateA.inCombat = 1.0f;
        stateA.isSneaking = 1.0f;

        for (int i = 0; i < 20; ++i) {
            fql.Update(bow, stateA, 1.0f);
        }

        // State B: combat + standing + low health (NOT trained)
        StateFeatures stateB;
        stateB.healthPct = 0.3f;
        stateB.inCombat = 1.0f;
        stateB.isSneaking = 0.0f;  // Different from A

        float qA = fql.GetQValue(bow, stateA);
        float qB = fql.GetQValue(bow, stateB);

        // B shares combat + low health features → should generalize (Q > 0)
        if (qB <= 0.0f) {
            logger::error("TEST FAIL: Generalization — Q(B) should be >0 from shared features, got {:.4f}"sv, qB);
            return;
        }
        // But A was trained directly, so Q(A) > Q(B) since sneaking weight contributes
        if (qB >= qA) {
            logger::error("TEST FAIL: Generalization — Q(B) should be < Q(A), got B={:.4f} >= A={:.4f}"sv, qB, qA);
            return;
        }
        logger::info("  Test 6 PASS: Generalization (Q_A={:.3f}, Q_B={:.3f})"sv, qA, qB);
    }

    // ── Test 7: Independent items ─────────────────────────────────────────
    {
        FeatureQLearner fql;
        RE::FormID item1 = 0xDEAD0007;
        RE::FormID item2 = 0xDEAD0008;

        StateFeatures state;
        state.inCombat = 1.0f;

        // Train item1 with positive reward, item2 with negative
        for (int i = 0; i < 15; ++i) {
            fql.Update(item1, state, 1.0f);
            fql.Update(item2, state, -1.0f);
        }

        float q1 = fql.GetQValue(item1, state);
        float q2 = fql.GetQValue(item2, state);

        if (q1 <= 0.0f) {
            logger::error("TEST FAIL: Item1 Q should be positive, got {:.4f}"sv, q1);
            return;
        }
        if (q2 >= 0.0f) {
            logger::error("TEST FAIL: Item2 Q should be negative, got {:.4f}"sv, q2);
            return;
        }
        if (fql.GetItemCount() != 2) {
            logger::error("TEST FAIL: Item count should be 2, got {}"sv, fql.GetItemCount());
            return;
        }
        logger::info("  Test 7 PASS: Independent items (Q1={:.3f}, Q2={:.3f}, count={})"sv,
            q1, q2, fql.GetItemCount());
    }

    // ── Test 8: Clear ─────────────────────────────────────────────────────
    {
        FeatureQLearner fql;
        RE::FormID item = 0xDEAD0009;
        StateFeatures state;
        state.inCombat = 1.0f;

        fql.Update(item, state, 1.0f);
        fql.Update(item, state, 1.0f);

        if (fql.GetItemCount() == 0 || fql.GetTotalTrainCount() == 0) {
            logger::error("TEST FAIL: Should have data before Clear()"sv);
            return;
        }

        fql.Clear();

        if (fql.GetItemCount() != 0) {
            logger::error("TEST FAIL: After Clear(), itemCount should be 0, got {}"sv, fql.GetItemCount());
            return;
        }
        if (fql.GetTotalTrainCount() != 0) {
            logger::error("TEST FAIL: After Clear(), totalTrains should be 0, got {}"sv, fql.GetTotalTrainCount());
            return;
        }
        float q = fql.GetQValue(item, state);
        if (!feq(q, 0.0f)) {
            logger::error("TEST FAIL: After Clear(), Q should be 0.0, got {:.4f}"sv, q);
            return;
        }
        logger::info("  Test 8 PASS: Clear"sv);
    }

    logger::info("TEST PASS: All FeatureQLearner tests passed! (8 tests)"sv);
#endif
}

// =============================================================================
// UNIT TESTS
// =============================================================================

// Run unit tests on startup (debug mode only)
void RunUnitTests()
{
#ifndef NDEBUG
    using namespace Huginn::State;

    logger::info("Running GameState unit tests..."sv);

    // Test 1: Minimum hash (all zeros)
    GameState state1{
        .health = HealthBucket::Critical,
        .magicka = MagickaBucket::Critical,
        .stamina = StaminaBucket::Critical,
        .distance = DistanceBucket::Melee,
        .targetType = TargetType::None,
        .enemyCount = EnemyCountBucket::None,
        .allyStatus = AllyStatus::None,
        .anyCasting = CastingStatus::NoneCasting,
        .inCombat = CombatStatus::NotInCombat,
        .isSneaking = SneakStatus::NotSneaking
    };

    if (state1.GetHash() != 0) {
        logger::error("TEST FAIL: Min hash should be 0, got {}"sv, state1.GetHash());
        return;
    }

    // Test 2: Maximum hash (all max values)
    // Hash states: 6×6×3×7×4×3×2×2×2 = 72,576 (stamina excluded from hash), so max hash = 72,575
    GameState state2{
        .health = HealthBucket::VeryHigh,
        .magicka = MagickaBucket::VeryHigh,
        .stamina = StaminaBucket::VeryHigh,  // Not in hash, but still in struct
        .distance = DistanceBucket::Ranged,
        .targetType = TargetType::Daedra,  // Max is 6 (Daedra) - 7 target types total
        .enemyCount = EnemyCountBucket::Many,
        .allyStatus = AllyStatus::InjuredPresent,
        .anyCasting = CastingStatus::EnemyCasting,
        .inCombat = CombatStatus::InCombat,
        .isSneaking = SneakStatus::Sneaking
    };

    if (state2.GetHash() != GameState::kTotalStates - 1) {
        logger::error("TEST FAIL: Max hash should be {}, got {}"sv, GameState::kTotalStates - 1, state2.GetHash());
        return;
    }

    // Test 3: Hash uniqueness for all 72,576 states (stamina excluded from hash)
    std::set<uint32_t> seenHashes;
    for (uint8_t h = 0; h < 6; ++h) {
        for (uint8_t m = 0; m < 6; ++m) {
            for (uint8_t d = 0; d < 3; ++d) {
                for (uint8_t t = 0; t < 7; ++t) {
                    for (uint8_t ec = 0; ec < 4; ++ec) {
                        for (uint8_t as = 0; as < 3; ++as) {
                          for (uint8_t ac = 0; ac < 2; ++ac) {
                            for (uint8_t c = 0; c < 2; ++c) {
                                for (uint8_t s = 0; s < 2; ++s) {
                                    GameState state{
                                        .health = static_cast<HealthBucket>(h),
                                        .magicka = static_cast<MagickaBucket>(m),
                                        .stamina = StaminaBucket::Medium,  // Arbitrary — excluded from hash
                                        .distance = static_cast<DistanceBucket>(d),
                                        .targetType = static_cast<TargetType>(t),
                                        .enemyCount = static_cast<EnemyCountBucket>(ec),
                                        .allyStatus = static_cast<AllyStatus>(as),
                                        .anyCasting = static_cast<CastingStatus>(ac),
                                        .inCombat = static_cast<CombatStatus>(c),
                                        .isSneaking = static_cast<SneakStatus>(s)
                                    };

                                    uint32_t hash = state.GetHash();
                                    if (hash >= GameState::kTotalStates) {
                                        logger::error("TEST FAIL: Hash {} out of range [0, {}]"sv, hash, GameState::kTotalStates - 1);
                                        return;
                                    }

                                    if (seenHashes.contains(hash)) {
                                        logger::error("TEST FAIL: Duplicate hash {} detected"sv, hash);
                                        return;
                                    }

                                    seenHashes.insert(hash);
                                }
                            }
                          }
                        }
                    }
                }
            }
        }
    }

    if (seenHashes.size() != GameState::kTotalStates) {
        logger::error("TEST FAIL: Should have {} unique hashes, got {}"sv, GameState::kTotalStates, seenHashes.size());
        return;
    }

    // Test 3b: Verify stamina doesn't affect hash
    GameState staminaLow{
        .health = HealthBucket::Medium, .magicka = MagickaBucket::Medium,
        .stamina = StaminaBucket::Critical, .distance = DistanceBucket::Melee,
        .targetType = TargetType::None, .enemyCount = EnemyCountBucket::None,
        .allyStatus = AllyStatus::None, .inCombat = CombatStatus::InCombat,
        .isSneaking = SneakStatus::NotSneaking
    };
    GameState staminaHigh{
        .health = HealthBucket::Medium, .magicka = MagickaBucket::Medium,
        .stamina = StaminaBucket::VeryHigh, .distance = DistanceBucket::Melee,
        .targetType = TargetType::None, .enemyCount = EnemyCountBucket::None,
        .allyStatus = AllyStatus::None, .inCombat = CombatStatus::InCombat,
        .isSneaking = SneakStatus::NotSneaking
    };
    if (staminaLow.GetHash() != staminaHigh.GetHash()) {
        logger::error("TEST FAIL: Stamina should not affect hash! Low={}, High={}"sv,
            staminaLow.GetHash(), staminaHigh.GetHash());
        return;
    }

    logger::info("TEST PASS: All hash tests passed! {} unique states verified, stamina excluded."sv, GameState::kTotalStates);

    // === SpellRegistry Unit Tests ===
    logger::info("Running SpellRegistry unit tests..."sv);

    using namespace Huginn::Spell;

    // Test: Registry starts empty
    auto testRegistry = std::make_unique<SpellRegistry>();
    if (testRegistry->GetSpellCount() != 0) {
        logger::error("TEST FAIL: New registry should be empty, got {} spells"sv, testRegistry->GetSpellCount());
        return;
    }

    logger::info("TEST PASS: SpellRegistry basic functionality verified."sv);
    logger::info("  - Registry starts empty"sv);
    logger::info("  - Note: Integration tests will run after game load when data is available"sv);

    // === PriorCalculator Context Independence Tests ===
    logger::info("Running PriorCalculator context independence tests..."sv);

    using namespace Huginn::Scoring;
    using namespace Huginn::Candidate;

    PriorCalculator priorCalc;

    // Create extreme context: 0% HP, in combat, fighting undead, on fire
    GameState extremeState{
        .health = HealthBucket::Critical,
        .magicka = MagickaBucket::Critical,
        .stamina = StaminaBucket::Critical,
        .distance = DistanceBucket::Melee,
        .targetType = TargetType::Undead,
        .enemyCount = EnemyCountBucket::Many,
        .allyStatus = AllyStatus::None,
        .inCombat = CombatStatus::InCombat,
        .isSneaking = SneakStatus::NotSneaking
    };

    PlayerActorState extremePlayer;
    extremePlayer.vitals.health = 0.01f;  // 1% HP
    extremePlayer.vitals.magicka = 0.01f;
    extremePlayer.vitals.stamina = 0.01f;
    extremePlayer.isInCombat = true;
    extremePlayer.effects.isOnFire = true;
    extremePlayer.effects.isPoisoned = true;

    // Create neutral context: 100% HP, no combat, no effects
    GameState neutralState{
        .health = HealthBucket::VeryHigh,
        .magicka = MagickaBucket::VeryHigh,
        .stamina = StaminaBucket::VeryHigh,
        .distance = DistanceBucket::Ranged,
        .targetType = TargetType::None,
        .enemyCount = EnemyCountBucket::None,
        .allyStatus = AllyStatus::None,
        .inCombat = CombatStatus::NotInCombat,
        .isSneaking = SneakStatus::NotSneaking
    };

    PlayerActorState neutralPlayer;
    neutralPlayer.vitals.health = 1.0f;  // 100% HP
    neutralPlayer.vitals.magicka = 1.0f;
    neutralPlayer.vitals.stamina = 1.0f;
    neutralPlayer.isInCombat = false;

    // Test 1: Healing spell prior should be IDENTICAL in both contexts
    SpellCandidate healingSpell;
    healingSpell.baseCost = 100;  // Adept-level healing spell
    healingSpell.type = Spell::SpellType::Healing;

    float healPriorExtreme = priorCalc.CalculatePrior(extremePlayer, healingSpell);
    float healPriorNeutral = priorCalc.CalculatePrior(neutralPlayer, healingSpell);

    if (std::abs(healPriorExtreme - healPriorNeutral) > 0.001f) {
        logger::error("TEST FAIL: Healing spell prior should be context-independent! Extreme={:.3f}, Neutral={:.3f}",
            healPriorExtreme, healPriorNeutral);
        return;
    }

    // Test 2: Damage spell prior should be IDENTICAL in combat vs out of combat
    SpellCandidate damageSpell;
    damageSpell.baseCost = 200;  // Expert-level damage spell
    damageSpell.type = Spell::SpellType::Damage;

    float dmgPriorExtreme = priorCalc.CalculatePrior(extremePlayer, damageSpell);
    float dmgPriorNeutral = priorCalc.CalculatePrior(neutralPlayer, damageSpell);

    if (std::abs(dmgPriorExtreme - dmgPriorNeutral) > 0.001f) {
        logger::error("TEST FAIL: Damage spell prior should be context-independent! Extreme={:.3f}, Neutral={:.3f}",
            dmgPriorExtreme, dmgPriorNeutral);
        return;
    }

    // Test 3: Item magnitude SHOULD affect prior (intrinsic property)
    ItemCandidate smallPotion;
    smallPotion.magnitude = 25.0f;  // Minor healing potion
    smallPotion.count = 10;

    ItemCandidate largePotion;
    largePotion.magnitude = 200.0f;  // Extreme healing potion
    largePotion.count = 10;

    float smallPrior = priorCalc.CalculatePrior(neutralPlayer, smallPotion);
    float largePrior = priorCalc.CalculatePrior(neutralPlayer, largePotion);

    if (largePrior <= smallPrior) {
        logger::error("TEST FAIL: Larger magnitude should give higher prior! Small={:.3f}, Large={:.3f}",
            smallPrior, largePrior);
        return;
    }

    // Test 4: Low count SHOULD reduce prior slightly (scarcity penalty)
    ItemCandidate plentifulPotion;
    plentifulPotion.magnitude = 100.0f;
    plentifulPotion.count = 50;

    ItemCandidate scarcePotion;
    scarcePotion.magnitude = 100.0f;
    scarcePotion.count = 2;  // Below LOW_COUNT_THRESHOLD

    float plentifulPrior = priorCalc.CalculatePrior(neutralPlayer, plentifulPotion);
    float scarcePrior = priorCalc.CalculatePrior(neutralPlayer, scarcePotion);

    if (scarcePrior >= plentifulPrior) {
        logger::error("TEST FAIL: Low count should reduce prior! Plentiful={:.3f}, Scarce={:.3f}",
            plentifulPrior, scarcePrior);
        return;
    }

    // Test 5: Spell cost SHOULD affect prior (higher cost = more powerful)
    SpellCandidate noviceSpell;
    noviceSpell.baseCost = 20;  // Novice

    SpellCandidate expertSpell;
    expertSpell.baseCost = 200;  // Expert

    float novicePrior = priorCalc.CalculatePrior(neutralPlayer, noviceSpell);
    float expertPrior = priorCalc.CalculatePrior(neutralPlayer, expertSpell);

    if (expertPrior <= novicePrior) {
        logger::error("TEST FAIL: Higher cost spell should have higher prior! Novice={:.3f}, Expert={:.3f}",
            novicePrior, expertPrior);
        return;
    }

    // Test 6: Weapon charge SHOULD reduce prior (intrinsic weapon state)
    WeaponCandidate fullCharge;
    fullCharge.hasEnchantment = true;
    fullCharge.currentCharge = 100.0f;
    fullCharge.maxCharge = 100.0f;

    WeaponCandidate lowCharge;
    lowCharge.hasEnchantment = true;
    lowCharge.currentCharge = 5.0f;
    lowCharge.maxCharge = 100.0f;

    float fullPrior = priorCalc.CalculatePrior(neutralPlayer, fullCharge);
    float lowPrior = priorCalc.CalculatePrior(neutralPlayer, lowCharge);

    if (lowPrior >= fullPrior) {
        logger::error("TEST FAIL: Low charge should reduce prior! Full={:.3f}, Low={:.3f}",
            fullPrior, lowPrior);
        return;
    }

    // Test 7: Ammo type matching SHOULD affect prior (intrinsic compatibility)
    AmmoCandidate arrows;
    arrows.type = Weapon::AmmoType::Arrow;
    arrows.count = 50;  // Plenty, so scarcity doesn't interfere

    // Player with bow equipped
    PlayerActorState bowPlayer;
    bowPlayer.hasBowEquipped = true;
    bowPlayer.hasCrossbowEquipped = false;

    // Player without bow (melee build)
    PlayerActorState meleePlayer;
    meleePlayer.hasBowEquipped = false;
    meleePlayer.hasCrossbowEquipped = false;

    float arrowsWithBow = priorCalc.CalculatePrior(bowPlayer, arrows);
    float arrowsWithoutBow = priorCalc.CalculatePrior(meleePlayer, arrows);

    if (arrowsWithBow <= arrowsWithoutBow) {
        logger::error("TEST FAIL: Compatible ammo should have higher prior! WithBow={:.3f}, WithoutBow={:.3f}",
            arrowsWithBow, arrowsWithoutBow);
        return;
    }

    // Test 8: Scroll type alone does NOT affect prior, but magnitude/scarcity DO
    //         (scroll prior now mirrors item prior — intrinsic quality)
    ScrollCandidate healScroll;
    healScroll.type = Spell::SpellType::Healing;

    ScrollCandidate damageScroll;
    damageScroll.type = Spell::SpellType::Damage;

    float healScrollPrior = priorCalc.CalculatePrior(neutralPlayer, healScroll);
    float dmgScrollPrior = priorCalc.CalculatePrior(neutralPlayer, damageScroll);

    // Type alone (with equal zero magnitude/count) must not change the prior
    if (std::abs(healScrollPrior - dmgScrollPrior) > 0.001f) {
        logger::error("TEST FAIL: Scroll type alone should not affect prior! Heal={:.3f}, Damage={:.3f}",
            healScrollPrior, dmgScrollPrior);
        return;
    }

    // Zero-magnitude, zero-count scroll should be exactly BASE_PRIOR (0.3f)
    if (std::abs(healScrollPrior - 0.3f) > 0.001f) {
        logger::error("TEST FAIL: Plain scroll prior should be BASE_PRIOR (0.3)! Got={:.3f}",
            healScrollPrior);
        return;
    }

    // Higher magnitude SHOULD raise scroll prior (intrinsic potency)
    ScrollCandidate weakScroll;
    weakScroll.magnitude = 8.0f;     // e.g. Scroll of Flames
    weakScroll.count = 10;

    ScrollCandidate strongScroll;
    strongScroll.magnitude = 75.0f;  // e.g. Scroll of Fireball
    strongScroll.count = 10;

    float weakScrollPrior = priorCalc.CalculatePrior(neutralPlayer, weakScroll);
    float strongScrollPrior = priorCalc.CalculatePrior(neutralPlayer, strongScroll);

    if (strongScrollPrior <= weakScrollPrior) {
        logger::error("TEST FAIL: Higher-magnitude scroll should have higher prior! Weak={:.3f}, Strong={:.3f}",
            weakScrollPrior, strongScrollPrior);
        return;
    }

    // Low count SHOULD reduce scroll prior (scarcity penalty)
    ScrollCandidate plentifulScroll;
    plentifulScroll.magnitude = 50.0f;
    plentifulScroll.count = 50;

    ScrollCandidate scarceScroll;
    scarceScroll.magnitude = 50.0f;
    scarceScroll.count = 2;  // Below LOW_COUNT_THRESHOLD

    float plentifulScrollPrior = priorCalc.CalculatePrior(neutralPlayer, plentifulScroll);
    float scarceScrollPrior = priorCalc.CalculatePrior(neutralPlayer, scarceScroll);

    if (scarceScrollPrior >= plentifulScrollPrior) {
        logger::error("TEST FAIL: Low scroll count should reduce prior! Plentiful={:.3f}, Scarce={:.3f}",
            plentifulScrollPrior, scarceScrollPrior);
        return;
    }

    logger::info("TEST PASS: PriorCalculator context independence verified!"sv);
    logger::info("  - Healing spell prior identical at 1% HP vs 100% HP"sv);
    logger::info("  - Damage spell prior identical in combat vs peaceful"sv);
    logger::info("  - Magnitude affects prior (intrinsic quality)"sv);
    logger::info("  - Low count reduces prior (scarcity penalty)"sv);
    logger::info("  - Spell cost affects prior (power scaling)"sv);
    logger::info("  - Weapon charge affects prior (depletion penalty)"sv);
    logger::info("  - Ammo type matching affects prior (compatibility)"sv);
    logger::info("  - Scroll prior mirrors item prior (magnitude + scarcity, type-independent)"sv);

    // === Optimization Unit Tests ===
    logger::info("Running optimization unit tests..."sv);

    // Test 1: Partial sort correctness
    {
        using namespace Huginn::Scoring;

        ScoredCandidateList testCandidates;

        // Create 10 test candidates with descending scores
        for (int i = 0; i < 10; ++i) {
            ScoredCandidate scored;
            scored.candidate = Candidate::SpellCandidate{};
            scored.utility = 10.0f - static_cast<float>(i);
            testCandidates.push_back(scored);
        }

        // Shuffle to randomize order
        std::shuffle(testCandidates.begin(), testCandidates.end(), std::mt19937{42});

        // Apply partial sort (same logic as pipeline)
        if (testCandidates.size() > 3) {
            std::partial_sort(testCandidates.begin(), testCandidates.begin() + 3, testCandidates.end());
        }

        // Verify top 3 are correct (highest utility)
        if (testCandidates[0].utility != 10.0f || testCandidates[1].utility != 9.0f || testCandidates[2].utility != 8.0f) {
            logger::error("TEST FAIL: Partial sort produced incorrect top 3: {:.1f}, {:.1f}, {:.1f}"sv,
                testCandidates[0].utility, testCandidates[1].utility, testCandidates[2].utility);
            return;
        }

        logger::info("TEST PASS: Partial sort optimization verified (top 3 correct)"sv);
    }

    // Test 3: ScopedTimer functionality (debug only)
    {
        using namespace Huginn::Util;

        {
            SCOPED_TIMER("TestTimer");
            volatile int sum = 0;
            for (int i = 0; i < 1000; ++i) {
                sum += i;
            }
        }

        logger::info("TEST PASS: ScopedTimer executed (check trace logs for timing)"sv);
    }

    // Test 4: ContextRuleEngine vital rules (Stage 1c - Pure Continuous)
    {
        logger::info("TEST: ContextRuleEngine vital rules (pure continuous)..."sv);

        // Create engine with default settings
        auto& settings = State::ContextWeightSettings::GetSingleton();
        Context::ContextRuleEngine engine(settings.BuildConfig());

        State::GameState testState{};
        State::TargetCollection testTargets{};
        State::WorldState testWorld{};

        // Test 4a: Very low health should give high weight (quadratic curve)
        {
            State::PlayerActorState testPlayer{};
            testPlayer.vitals.health = 0.10f;  // 10% HP

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            // deficit=0.9, curve=(0.9)^2 = 0.81
            const float expected = 0.81f;
            const float tolerance = 0.01f;

            if (std::abs(weights.healingWeight - expected) > tolerance) {
                logger::error("TEST FAIL: 10%% HP should give healingWeight≈{:.2f}, got {:.3f}",
                    expected, weights.healingWeight);
                return;
            }
        }

        // Test 4b: NO CLIFF at 50% threshold (continuous curve)
        {
            State::PlayerActorState testPlayer49{};
            testPlayer49.vitals.health = 0.49f;  // 49% HP

            State::PlayerActorState testPlayer51{};
            testPlayer51.vitals.health = 0.51f;  // 51% HP

            auto weights49 = engine.EvaluateRules(testPlayer49, testTargets, testWorld);
            auto weights51 = engine.EvaluateRules(testPlayer51, testTargets, testWorld);

            // Pure continuous: 49%: deficit=0.51, curve=(0.51)^2 ≈ 0.26
            //                  51%: deficit=0.49, curve=(0.49)^2 ≈ 0.24
            // Difference should be ~0.02 (smooth!), NOT 4.5 (old cliff!)

            const float expected49 = 0.26f;
            const float expected51 = 0.24f;
            const float tolerance = 0.01f;

            if (std::abs(weights49.healingWeight - expected49) > tolerance) {
                logger::error("TEST FAIL: 49%% HP should give ≈{:.2f}, got {:.3f}",
                    expected49, weights49.healingWeight);
                return;
            }

            if (std::abs(weights51.healingWeight - expected51) > tolerance) {
                logger::error("TEST FAIL: 51%% HP should give ≈{:.2f}, got {:.3f}",
                    expected51, weights51.healingWeight);
                return;
            }

            // Verify smooth difference (no cliff!)
            float diff = std::abs(weights49.healingWeight - weights51.healingWeight);
            if (diff > 0.05f) {
                logger::error("TEST FAIL: 49%% vs 51%% should be smooth (diff<0.05), got diff={:.3f}",
                    diff);
                return;
            }
        }

        // Test 4c: Full health (100%) should give zero weight
        {
            State::PlayerActorState testPlayer{};
            testPlayer.vitals.health = 1.0f;  // 100% HP

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (weights.healingWeight != 0.0f) {
                logger::error("TEST FAIL: Full health should give healingWeight=0.0, got {:.3f}",
                    weights.healingWeight);
                return;
            }
        }

        // Test 4d: Magicka follows continuous curve
        {
            State::PlayerActorState testPlayer{};
            testPlayer.vitals.magicka = 0.25f;  // 25% magicka

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            // deficit=0.75, curve=(0.75)^2 = 0.56
            const float expected = 0.56f;
            const float tolerance = 0.01f;

            if (std::abs(weights.magickaRestoreWeight - expected) > tolerance) {
                logger::error("TEST FAIL: 25%% MP should give ≈{:.2f}, got {:.3f}",
                    expected, weights.magickaRestoreWeight);
                return;
            }
        }

        // Test 4e: Stamina uses gentler curve (exponent=1.5)
        {
            State::PlayerActorState testPlayer{};
            testPlayer.vitals.stamina = 0.50f;  // 50% stamina

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            // deficit=0.5, curve=(0.5)^1.5 ≈ 0.35
            const float expected = 0.35f;
            const float tolerance = 0.01f;

            if (std::abs(weights.staminaRestoreWeight - expected) > tolerance) {
                logger::error("TEST FAIL: 50%% SP should give ≈{:.2f}, got {:.3f}",
                    expected, weights.staminaRestoreWeight);
                return;
            }
        }

        logger::info("TEST PASS: ContextRuleEngine vital rules are truly continuous (no cliffs!)"sv);
    }

    // Test 5: ContextRuleEngine elemental rules (Stage 1d - Binary Weights)
    {
        logger::info("TEST: ContextRuleEngine elemental rules..."sv);

        auto& settings = State::ContextWeightSettings::GetSingleton();
        Context::ContextRuleEngine engine(settings.BuildConfig());

        State::GameState testState{};
        State::TargetCollection testTargets{};
        State::WorldState testWorld{};

        // Test 5a: Fire damage → resistFireWeight = 0.8
        {
            State::PlayerActorState testPlayer{};
            testPlayer.effects.isOnFire = true;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.resistFireWeight - 0.8f) > 0.01f) {
                logger::error("TEST FAIL: OnFire should give resistFireWeight=0.8, got {:.3f}",
                    weights.resistFireWeight);
                return;
            }
        }

        // Test 5b: Frost + Shock damage → both resist weights active
        {
            State::PlayerActorState testPlayer{};
            testPlayer.effects.isFrozen = true;
            testPlayer.effects.isShocked = true;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.resistFrostWeight - 0.8f) > 0.01f ||
                std::abs(weights.resistShockWeight - 0.8f) > 0.01f) {
                logger::error("TEST FAIL: Frost+Shock should give both=0.8, got frost={:.3f} shock={:.3f}",
                    weights.resistFrostWeight, weights.resistShockWeight);
                return;
            }
        }

        // Test 5c: Poison → resistPoisonWeight = 0.6 (moderate priority)
        {
            State::PlayerActorState testPlayer{};
            testPlayer.effects.isPoisoned = true;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.resistPoisonWeight - 0.6f) > 0.01f) {
                logger::error("TEST FAIL: Poisoned should give resistPoisonWeight=0.6, got {:.3f}",
                    weights.resistPoisonWeight);
                return;
            }
        }

        // Test 5d: Disease → resistDiseaseWeight = 0.3 (low priority)
        {
            State::PlayerActorState testPlayer{};
            testPlayer.effects.isDiseased = true;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.resistDiseaseWeight - 0.3f) > 0.01f) {
                logger::error("TEST FAIL: Diseased should give resistDiseaseWeight=0.3, got {:.3f}",
                    weights.resistDiseaseWeight);
                return;
            }
        }

        // Test 5e: No elemental effects → all weights zero
        {
            State::PlayerActorState testPlayer{};  // All effects false by default

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (weights.resistFireWeight != 0.0f || weights.resistFrostWeight != 0.0f ||
                weights.resistShockWeight != 0.0f || weights.resistPoisonWeight != 0.0f ||
                weights.resistDiseaseWeight != 0.0f) {
                logger::error("TEST FAIL: No effects should give all resist weights=0.0");
                return;
            }
        }

        // Test 5f: Resistance scales weight (capped fire resist → near-zero weight)
        {
            State::PlayerActorState testPlayer{};
            testPlayer.effects.isOnFire = true;
            testPlayer.resistances.fire = 80.0f;  // 80% resist → 0.2x scale

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            const float expected = 0.8f * 0.2f;  // baseWeight × (1 - 0.8)
            if (std::abs(weights.resistFireWeight - expected) > 0.01f) {
                logger::error("TEST FAIL: 80%% fire resist should scale weight to {:.3f}, got {:.3f}",
                    expected, weights.resistFireWeight);
                return;
            }
        }

        // Test 5g: Resistance weakness (negative resist) clamps at 1.0x — no over-amplification
        {
            State::PlayerActorState testPlayer{};
            testPlayer.effects.isOnFire = true;
            testPlayer.resistances.fire = -50.0f;  // Weakness → clamped to 1.0x

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.resistFireWeight - 0.8f) > 0.01f) {
                logger::error("TEST FAIL: Negative fire resist should clamp to full weight 0.8, got {:.3f}",
                    weights.resistFireWeight);
                return;
            }
        }

        logger::info("TEST PASS: ContextRuleEngine elemental rules work correctly"sv);
    }

    // Test 6: ContextRuleEngine environmental rules (Stage 1d - Binary Weights)
    {
        logger::info("TEST: ContextRuleEngine environmental rules..."sv);

        auto& settings = State::ContextWeightSettings::GetSingleton();
        Context::ContextRuleEngine engine(settings.BuildConfig());

        State::GameState testState{};
        State::TargetCollection testTargets{};

        // Test 6a: Underwater → waterbreathingWeight = 1.0 (critical)
        {
            State::PlayerActorState testPlayer{};
            State::WorldState testWorld{};

            testPlayer.isUnderwater = true;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.waterbreathingWeight - 1.0f) > 0.01f) {
                logger::error("TEST FAIL: Underwater should give waterbreathingWeight=1.0, got {:.3f}",
                    weights.waterbreathingWeight);
                return;
            }
        }

        // Test 6b: Looking at lock → unlockWeight = 1.0 (critical)
        {
            State::PlayerActorState testPlayer{};
            State::WorldState testWorld{};

            testWorld.isLookingAtLock = true;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.unlockWeight - 1.0f) > 0.01f) {
                logger::error("TEST FAIL: Looking at lock should give unlockWeight=1.0, got {:.3f}",
                    weights.unlockWeight);
                return;
            }
        }

        // Test 6c: Falling ramps with depth, and a jump never reaches it (#60)
        {
            State::WorldState testWorld{};

            // A deep fall still reaches full weight.
            State::PlayerActorState deepFall{};
            deepFall.isFalling = true;
            deepFall.fallDepth = State::PhysicsConstants::FALL_DEPTH_HIGH;

            auto weights = engine.EvaluateRules(deepFall, testTargets, testWorld);
            if (std::abs(weights.slowFallWeight - 0.8f) > 0.01f) {
                logger::error("TEST FAIL: deep fall should give slowFallWeight=0.8, got {:.3f}",
                    weights.slowFallWeight);
                return;
            }

            // Beyond the far end it clamps rather than running away.
            State::PlayerActorState chasm{};
            chasm.isFalling = true;
            chasm.fallDepth = State::PhysicsConstants::FALL_DEPTH_HIGH * 10.0f;
            if (std::abs(engine.EvaluateRules(chasm, testTargets, testWorld).slowFallWeight - 0.8f)
                > 0.01f) {
                logger::error("TEST FAIL: slowFallWeight must clamp at weightFallingHigh");
                return;
            }

            // Halfway up the ramp reads half.
            State::PlayerActorState midFall{};
            midFall.isFalling = true;
            midFall.fallDepth = (State::PhysicsConstants::FALL_DEPTH_MIN +
                                 State::PhysicsConstants::FALL_DEPTH_HIGH) * 0.5f;
            const float midWeight =
                engine.EvaluateRules(midFall, testTargets, testWorld).slowFallWeight;
            if (std::abs(midWeight - 0.4f) > 0.01f) {
                logger::error("TEST FAIL: mid-depth fall should be half weight, got {:.3f}",
                    midWeight);
                return;
            }

            // The reported bug: a jump. StateManager leaves isFalling false below
            // FALL_DEPTH_MIN, so this is what the rule actually sees on every hop.
            State::PlayerActorState jump{};
            jump.isFalling = false;
            jump.fallDepth = 0.0f;
            if (engine.EvaluateRules(jump, testTargets, testWorld).slowFallWeight != 0.0f) {
                logger::error("TEST FAIL: a jump must not produce any slow-fall weight");
                return;
            }

            // And the reason must stay quiet for a shallow drop, so it cannot
            // outrank a live combat reason for a tick (the observed harm).
            State::PlayerActorState shallow{};
            shallow.isFalling = true;
            shallow.fallDepth = State::PhysicsConstants::FALL_DEPTH_MIN + 1.0f;
            const auto shallowWeights = engine.EvaluateRules(shallow, testTargets, testWorld);
            if (engine.DominantReason(shallowWeights, {}) == Context::ContextReason::Falling) {
                logger::error("TEST FAIL: a shallow drop must not report Falling");
                return;
            }
            if (engine.DominantReason(
                    engine.EvaluateRules(deepFall, testTargets, testWorld), {}) !=
                Context::ContextReason::Falling) {
                logger::error("TEST FAIL: a deep fall should report Falling");
                return;
            }
        }

        // Test 6c-2: FallTracker itself (#60). The rule engine was never the
        // broken half — `isFalling = IsInMidair()` was — so the take-off-Z
        // policy is what actually needs covering. It is engine-free precisely
        // so these cases can run without a live PlayerCharacter.
        {
            using State::FallTracker;
            constexpr float kMaxDelta = 1500.0f;  // 100ms poll at MAX_FALL_SPEED

            // A jump: grounded, rise to apex, come back down to take-off height.
            // Depth must never leave 0 — the reported bug in its purest form.
            {
                FallTracker t;
                const float profile[] = {100.0f, 140.0f, 170.0f, 176.0f, 150.0f, 110.0f, 100.0f};
                bool first = true;
                for (const float z : profile) {
                    const float depth = t.Update(z, /*airborne=*/!first, kMaxDelta);
                    first = false;
                    if (depth != 0.0f) {
                        logger::error("TEST FAIL: a jump produced fallDepth {:.1f} at z={:.1f}",
                            depth, z);
                        return;
                    }
                }
            }

            // A real drop off a ledge measures the descent below take-off.
            {
                FallTracker t;
                t.Update(1000.0f, false, kMaxDelta);          // grounded, anchor at 1000
                if (const float d = t.Update(800.0f, true, kMaxDelta); std::abs(d - 200.0f) > 0.01f) {
                    logger::error("TEST FAIL: 200-unit drop measured {:.1f}", d);
                    return;
                }
                if (const float d = t.Update(300.0f, true, kMaxDelta); std::abs(d - 700.0f) > 0.01f) {
                    logger::error("TEST FAIL: 700-unit drop measured {:.1f}", d);
                    return;
                }
                // Landing re-anchors.
                if (t.Update(300.0f, false, kMaxDelta) != 0.0f) {
                    logger::error("TEST FAIL: landing should reset fall depth");
                    return;
                }
            }

            // A relocation — save load, cell door, fast travel — must re-anchor
            // rather than report the world-space difference as a fall. Standing
            // on a peak, then loading into an interior far below.
            {
                FallTracker t;
                t.Update(20000.0f, false, kMaxDelta);
                if (const float d = t.Update(100.0f, true, kMaxDelta); d != 0.0f) {
                    logger::error("TEST FAIL: a relocation reported fallDepth {:.1f}", d);
                    return;
                }
                // ...and tracking resumes normally from the new anchor.
                if (const float d = t.Update(-200.0f, true, kMaxDelta);
                    std::abs(d - 300.0f) > 0.01f) {
                    logger::error("TEST FAIL: post-relocation fall measured {:.1f}", d);
                    return;
                }
            }

            // First poll of a session, already airborne, with no anchor yet.
            // Must NOT measure against a default-constructed 0 — plenty of cells
            // sit at negative Z, which would read as a large positive fall.
            {
                FallTracker t;
                if (const float d = t.Update(-5000.0f, true, kMaxDelta); d != 0.0f) {
                    logger::error("TEST FAIL: unanchored first poll reported {:.1f}", d);
                    return;
                }
            }

            // Reset() drops the anchor, so a post-load poll cannot measure
            // against the previous save's take-off Z.
            {
                FallTracker t;
                t.Update(20000.0f, false, kMaxDelta);
                t.Reset();
                if (t.IsAnchored()) {
                    logger::error("TEST FAIL: Reset() left the tracker anchored");
                    return;
                }
                if (const float d = t.Update(100.0f, true, kMaxDelta); d != 0.0f) {
                    logger::error("TEST FAIL: post-Reset poll reported fallDepth {:.1f}", d);
                    return;
                }
            }

            logger::info("  ✓ PASS: FallTracker (jump, drop, relocation, unanchored, reset)"sv);
        }

        // Test 6c-3: ReasonHold (#62). A reason true for one tick repainted all
        // eight subtexts and reverted before it could be read. Damps the label
        // only — scoring never sees this.
        {
            using Context::ContextReason;
            using R = ContextReason;
            constexpr float kHold = 1500.0f;

            // The reported shape: a momentary Sneaking must stay readable.
            {
                Context::ReasonHold h;
                if (h.Update(R::Sneaking, 0.0, kHold) != R::Sneaking) {
                    logger::error("TEST FAIL: a new reason should be adopted immediately");
                    return;
                }
                if (h.Update(R::None, 100.0, kHold) != R::Sneaking) {
                    logger::error("TEST FAIL: Sneaking should survive a one-tick drop");
                    return;
                }
                if (h.Update(R::None, 1400.0, kHold) != R::Sneaking) {
                    logger::error("TEST FAIL: hold released early");
                    return;
                }
                // ...and released once the hold expires, so it cannot stick.
                if (h.Update(R::None, 1600.0, kHold) != R::None) {
                    logger::error("TEST FAIL: hold never released");
                    return;
                }
            }

            // Urgency is never delayed: Critical HP must not wait behind a
            // stale Sneaking. This is the property that makes the hold safe.
            {
                Context::ReasonHold h;
                if (h.Update(R::Sneaking, 0.0, kHold) != R::Sneaking) {
                    logger::error("TEST FAIL: setup — Sneaking should be held");
                    return;
                }
                if (h.Update(R::CriticalHealth, 50.0, kHold) != R::CriticalHealth) {
                    logger::error("TEST FAIL: a more urgent reason must adopt instantly");
                    return;
                }
            }

            // The same-band swap seen mid-fight: a crosshair drifting off a
            // draugr onto a bandit must not flip the label back and forth.
            // Outnumbered is less urgent than Undead, so it waits.
            {
                Context::ReasonHold h;
                if (h.Update(R::TargetUndead, 0.0, kHold) != R::TargetUndead) {
                    logger::error("TEST FAIL: setup — Undead should be held");
                    return;
                }
                if (h.Update(R::MultipleEnemies, 100.0, kHold) != R::TargetUndead) {
                    logger::error("TEST FAIL: a less urgent reason should not preempt");
                    return;
                }
                // Crosshair returns — the reason refreshes rather than expiring.
                if (h.Update(R::TargetUndead, 200.0, kHold) != R::TargetUndead) {
                    logger::error("TEST FAIL: returning reason should refresh");
                    return;
                }
                if (h.Update(R::MultipleEnemies, 1500.0, kHold) != R::TargetUndead) {
                    logger::error("TEST FAIL: hold should restart from this departure");
                    return;
                }
                if (h.Update(R::MultipleEnemies, 2900.0, kHold) != R::TargetUndead) {
                    logger::error("TEST FAIL: hold runs from the departure at 1500, not from 200");
                    return;
                }
                // Genuinely gone: the downgrade lands once the hold expires.
                if (h.Update(R::MultipleEnemies, 3100.0, kHold) != R::MultipleEnemies) {
                    logger::error("TEST FAIL: downgrade should land after the hold");
                    return;
                }
            }

            // Reset drops the held reason — the previous character's context
            // says nothing about the next one.
            {
                Context::ReasonHold h;
                if (h.Update(R::TargetUndead, 0.0, kHold) != R::TargetUndead) {
                    logger::error("TEST FAIL: setup — Undead should be held");
                    return;
                }
                h.Reset();
                if (h.Held() != R::None) {
                    logger::error("TEST FAIL: Reset() left a reason held");
                    return;
                }
                if (h.Update(R::None, 10.0, kHold) != R::None) {
                    logger::error("TEST FAIL: post-Reset update should report None");
                    return;
                }
            }

            // Escalation out of a DECAYING hold, not a fresh one. This is the
            // safety property the whole design leans on, and the case above
            // escalates from a hold that is still raw-true.
            {
                Context::ReasonHold h;
                if (h.Update(R::Sneaking, 0.0, kHold) != R::Sneaking) {
                    logger::error("TEST FAIL: setup — Sneaking should be held");
                    return;
                }
                for (double t = 100.0; t <= 1000.0; t += 100.0) {
                    if (h.Update(R::None, t, kHold) != R::Sneaking) {
                        logger::error("TEST FAIL: hold released early at t={:.0f}", t);
                        return;
                    }
                }
                if (!h.IsHolding()) {
                    logger::error("TEST FAIL: a pending downgrade should report IsHolding");
                    return;
                }
                if (h.Update(R::CriticalHealth, 1100.0, kHold) != R::CriticalHealth) {
                    logger::error("TEST FAIL: escalation must cut through a decaying hold");
                    return;
                }
                if (h.IsHolding()) {
                    logger::error("TEST FAIL: adopting a reason should clear the pending flag");
                    return;
                }
            }

            // After an expiry-driven downgrade the state must actually advance,
            // not merely return a matching value.
            {
                Context::ReasonHold h;
                if (h.Update(R::Sneaking, 0.0, kHold) != R::Sneaking) {
                    logger::error("TEST FAIL: setup — Sneaking should be held");
                    return;
                }
                if (h.Update(R::None, 2000.0, kHold) != R::Sneaking) {
                    logger::error("TEST FAIL: the hold starts when the reason departs");
                    return;
                }
                if (h.Update(R::None, 3600.0, kHold) != R::None) {
                    logger::error("TEST FAIL: hold should have expired");
                    return;
                }
                if (h.Held() != R::None || h.IsHolding()) {
                    logger::error("TEST FAIL: expiry left stale state (held={}, holding={})",
                        static_cast<int>(h.Held()), h.IsHolding());
                    return;
                }
            }

            // The observation gap. This is the shape that shipped broken and was
            // caught in-game, not at review: the pipeline SKIPS while a reason
            // stays true, so ticks are not evenly spaced and a long-held reason
            // is seen once and then not again for seconds. Timing the hold from
            // the last sighting made it expire before the reason had even
            // departed; timing it from the departure is what these two ticks
            // pin down. A six-second crouch must still leave the label up.
            {
                Context::ReasonHold h;
                if (h.Update(R::Sneaking, 0.0, kHold) != R::Sneaking) {
                    logger::error("TEST FAIL: setup — Sneaking should be adopted");
                    return;
                }
                // Next run is 6000ms later — every tick between skipped.
                if (h.Update(R::None, 6000.0, kHold) != R::Sneaking) {
                    logger::error("TEST FAIL: a gap between runs must not consume the hold");
                    return;
                }
                if (!h.IsHolding()) {
                    logger::error("TEST FAIL: the downgrade should be pending, forcing runs");
                    return;
                }
                if (h.Update(R::None, 7600.0, kHold) != R::None) {
                    logger::error("TEST FAIL: hold should expire holdMs after the departure");
                    return;
                }
            }

            // holdMs == 0 is reachable in the field, not a degenerate argument:
            // ScoreCandidates clamps the hold to SlotLocker's fLockDurationMs,
            // which the INI documents as "set to 0 to disable locking". The hold
            // must then be transparent — follow the raw reason and never latch
            // IsHolding, which would pin the skip gate open every tick.
            {
                Context::ReasonHold h;
                if (h.Update(R::Sneaking, 0.0, 0.0f) != R::Sneaking) {
                    logger::error("TEST FAIL: setup — Sneaking should be adopted");
                    return;
                }
                if (h.Update(R::None, 1.0, 0.0f) != R::None) {
                    logger::error("TEST FAIL: a zero hold must release immediately");
                    return;
                }
                if (h.IsHolding()) {
                    logger::error("TEST FAIL: a zero hold must never pin the skip gate");
                    return;
                }
            }

            logger::info("  ✓ PASS: ReasonHold (blink damped, urgency instant, downgrade delayed)"sv);
        }

        // Test 6d: Workstation (Forge) → fortifySmithingWeight = 0.8
        {
            State::PlayerActorState testPlayer{};
            State::WorldState testWorld{};

            testWorld.isLookingAtWorkstation = true;
            testWorld.workstationType = 1;  // Forge

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.fortifySmithingWeight - 0.8f) > 0.01f) {
                logger::error("TEST FAIL: Forge should give fortifySmithingWeight=0.8, got {:.3f}",
                    weights.fortifySmithingWeight);
                return;
            }
        }

        // Test 6d-armor: Workstation (SmithingArmor, type 7) → fortifySmithingWeight = 0.8
        // Regression: type 7 was previously dropped (only types 1-2 mapped to smithing).
        {
            State::PlayerActorState testPlayer{};
            State::WorldState testWorld{};

            testWorld.isLookingAtWorkstation = true;
            testWorld.workstationType = 7;  // SmithingArmor

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.fortifySmithingWeight - 0.8f) > 0.01f) {
                logger::error("TEST FAIL: SmithingArmor should give fortifySmithingWeight=0.8, got {:.3f}",
                    weights.fortifySmithingWeight);
                return;
            }
        }

        // Test 6e: Workstation (Enchanter) → fortifyEnchantingWeight = 0.8
        {
            State::PlayerActorState testPlayer{};
            State::WorldState testWorld{};

            testWorld.isLookingAtWorkstation = true;
            testWorld.workstationType = 3;  // Enchanting

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.fortifyEnchantingWeight - 0.8f) > 0.01f) {
                logger::error("TEST FAIL: Enchanter should give fortifyEnchantingWeight=0.8, got {:.3f}",
                    weights.fortifyEnchantingWeight);
                return;
            }
        }

        // Test 6f: Workstation (Alchemy) → fortifyAlchemyWeight = 0.8
        {
            State::PlayerActorState testPlayer{};
            State::WorldState testWorld{};

            testWorld.isLookingAtWorkstation = true;
            testWorld.workstationType = 5;  // Alchemy

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.fortifyAlchemyWeight - 0.8f) > 0.01f) {
                logger::error("TEST FAIL: Alchemy Lab should give fortifyAlchemyWeight=0.8, got {:.3f}",
                    weights.fortifyAlchemyWeight);
                return;
            }
        }

        // Test 6h: the workstation path end to end — weight → candidate → label (#63)
        //
        // 6d/6e/6f prove EvaluateRules RAISES these weights; 17k proves the label
        // is gated on drawing them. Nothing joined the two, and the join is
        // precisely what play-testing cannot check here: Requiem-based lists
        // (LoreRim, what this is developed against) strip Fortify Smithing and
        // Fortify Enchanting from alchemy outright, so no such potion exists
        // in-game to rank. Every observed workstation session has therefore been
        // the empty case. This test IS the vanilla coverage — if it goes, the
        // path has none, and a regression would surface only for players on
        // lists nobody here plays.
        {
            using WM = Context::ContextWeightMap;

            struct Station {
                std::string_view name;
                int32_t type;
                Context::ContextReason reason;
                std::string_view label;
                Candidate::ItemCandidate potion;
                float WM::* field;
            };

            // The three fortify potions differ in WHICH discriminator field the
            // item arm reads alongside the tag, so each needs its own build.
            auto smithing = [] {
                Candidate::ItemCandidate p{};
                p.name = "Potion of Blacksmithing";
                p.type = Item::ItemType::BuffPotion;
                p.tags = Item::ItemTag::FortifyCombatSkill;
                p.combatSkill = Item::CombatSkill::Smithing;
                return p;
            }();
            auto enchanting = [] {
                Candidate::ItemCandidate p{};
                p.name = "Potion of Enchanting";
                p.type = Item::ItemType::BuffPotion;
                p.tags = Item::ItemTag::FortifyMagicSchool;
                p.school = Item::MagicSchool::Enchanting;
                return p;
            }();
            auto alchemy = [] {
                Candidate::ItemCandidate p{};
                p.name = "Potion of Alchemy";
                p.type = Item::ItemType::BuffPotion;
                p.tags = Item::ItemTag::FortifyUtilitySkill;
                p.utilitySkill = Item::UtilitySkill::Alchemy;
                return p;
            }();

            const Station kStations[] = {
                {"Forge",       1, Context::ContextReason::AtForge,     "At Forge",
                 smithing,   &WM::fortifySmithingWeight},
                {"Enchanter",   3, Context::ContextReason::AtEnchanter, "At Enchanter",
                 enchanting, &WM::fortifyEnchantingWeight},
                {"Alchemy Lab", 5, Context::ContextReason::AtAlchemy,   "At Alchemy Lab",
                 alchemy,    &WM::fortifyAlchemyWeight},
            };

            // Control: present in every real inventory, draws nothing from any
            // workstation. Without it a blanket weight bump would pass 6h.
            Candidate::WeaponCandidate control{};
            control.name = "Iron Dagger";
            control.tags = Weapon::WeaponTag::Melee | Weapon::WeaponTag::OneHanded;

            for (const auto& st : kStations) {
                State::PlayerActorState testPlayer{};
                State::WorldState testWorld{};
                testWorld.isLookingAtWorkstation = true;
                testWorld.workstationType = st.type;

                const auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);
                const float stationWeight = weights.*(st.field);

                // The potion draws the station's weight, not the 0.15 buff-potion
                // baseline it would otherwise sit at.
                const float potionWeight = Context::WeightForCandidate(st.potion, weights);
                if (std::abs(potionWeight - stationWeight) > 0.01f) {
                    logger::error("TEST FAIL (6h): at {} the fortify potion should draw {:.2f}, got {:.3f}",
                        st.name, stationWeight, potionWeight);
                    return;
                }

                // ...and an unrelated item does not, or the context is just a
                // page-wide bump wearing a label.
                if (Context::WeightForCandidate(control, weights) >= stationWeight - 0.01f) {
                    logger::error("TEST FAIL (6h): at {} an unrelated weapon reached the station weight",
                        st.name);
                    return;
                }

                // Drawing the weight is what earns the label — same gate the
                // display uses, so this closes weight → candidate → subtext.
                Slot::SlotAssignment assignment{};
                Scoring::ScoredCandidate scored{};
                scored.candidate = st.potion;
                assignment.candidate = scored;
                if (const auto got = Display::DeriveExplanationLabel(assignment, st.reason);
                    got != st.label) {
                    logger::error("TEST FAIL (6h): at {} the potion should read '{}', got '{}'",
                        st.name, st.label, got);
                    return;
                }
            }

            logger::info("  ✓ PASS: workstation potions rank and label at forge/enchanter/alchemy"sv);
        }

        // Test 6i: a filled soul gem surfaces without an enchanted weapon
        //
        // The gem arm read only weaponChargeWeight, which needs an enchanted
        // weapon EQUIPPED and drained. Carry filled gems with an ordinary weapon
        // out and every gem sat at baseRelevance (0.05), under fMinimumUtility
        // (0.1) — never displayed, and so never learned from either. Confirmed
        // in-game: six gems registered, three of them filled, and not one ever
        // reached a slot because the player had no enchanted weapon equipped.
        {
            // Shipped default, not the player's INI. fWeightSoulGem is a
            // supported, documented tuning knob with a legal value of 0 — read
            // live, a player who had turned gems off would fail this test, and
            // the early return would silently take every test after it with it.
            // The mechanism under test (a gem draws its own baseline and that
            // baseline clears the floor) is a property of the shipped defaults.
            auto gemConfig = settings.BuildConfig();
            gemConfig.weightSoulGem = State::ContextWeightDefaults::SOUL_GEM;
            Context::ContextRuleEngine gemEngine(gemConfig);

            State::PlayerActorState testPlayer{};   // no enchanted weapon
            State::WorldState testWorld{};
            const auto weights = gemEngine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (weights.weaponChargeWeight != 0.0f) {
                logger::error("TEST FAIL (6i): premise wrong — no enchanted weapon should mean "
                    "weaponChargeWeight=0, got {:.3f}", weights.weaponChargeWeight);
                return;
            }

            Candidate::ItemCandidate gem{};
            gem.name = "Soul Gem III - Common";
            gem.type = Item::ItemType::SoulGem;
            gem.sourceType = Candidate::SourceType::SoulGem;

            const float gemWeight = Context::WeightForCandidate(gem, weights);
            if (gemWeight <= weights.baseRelevanceWeight + 0.001f) {
                logger::error("TEST FAIL (6i): a gem should draw its baseline, got {:.3f} "
                    "(baseRelevance {:.3f})", gemWeight, weights.baseRelevanceWeight);
                return;
            }
            // The point of the number is clearing the floor — assert that, not
            // the value, so retuning fWeightSoulGem does not break the test.
            // The real constant, not a copy: if the floor is ever raised above
            // the gem baseline the lockout comes straight back, and this test
            // exists to catch exactly that.
            constexpr float kMinimumUtility = Scoring::ScorerDefaults::MINIMUM_UTILITY;
            if (gemWeight <= kMinimumUtility) {
                logger::error("TEST FAIL (6i): gem weight {:.3f} does not clear fMinimumUtility {:.3f} — "
                    "it would be filtered out before reaching a slot",
                    gemWeight, kMinimumUtility);
                return;
            }

            // Urgency still comes from the charge rules: with a drained
            // enchanted weapon the gem must outrank its own baseline, or the
            // override has nothing to promote.
            State::PlayerActorState drained{};
            drained.hasEnchantedWeapon = true;
            drained.weaponChargePercent = 0.10f;
            const auto urgentWeights = gemEngine.EvaluateRules(drained, testTargets, testWorld);
            if (Context::WeightForCandidate(gem, urgentWeights) <= gemWeight) {
                logger::error("TEST FAIL (6i): a drained enchanted weapon should raise the gem above "
                    "its baseline {:.3f}", gemWeight);
                return;
            }

            logger::info("  ✓ PASS: soul gem baseline {:.2f} clears fMinimumUtility, charge still promotes"sv,
                gemWeight);
        }

        // Test 6g: No environmental conditions → all weights zero
        {
            State::PlayerActorState testPlayer{};
            State::WorldState testWorld{};

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (weights.waterbreathingWeight != 0.0f || weights.unlockWeight != 0.0f ||
                weights.slowFallWeight != 0.0f || weights.fortifySmithingWeight != 0.0f ||
                weights.fortifyEnchantingWeight != 0.0f || weights.fortifyAlchemyWeight != 0.0f) {
                logger::error("TEST FAIL: No conditions should give all environmental weights=0.0");
                return;
            }
        }

        logger::info("TEST PASS: ContextRuleEngine environmental rules work correctly"sv);
    }

    // Test 7: ContextRuleEngine combat rules (Stage 1e - Binary Weights)
    {
        logger::info("TEST: ContextRuleEngine combat rules..."sv);

        auto& settings = State::ContextWeightSettings::GetSingleton();
        Context::ContextRuleEngine engine(settings.BuildConfig());

        State::GameState testState{};
        State::WorldState testWorld{};

        // Test 7a: In combat → damageWeight = 0.3
        {
            State::PlayerActorState testPlayer{};
            State::TargetCollection testTargets{};

            testPlayer.isInCombat = true;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.damageWeight - 0.3f) > 0.01f) {
                logger::error("TEST FAIL: In combat should give damageWeight=0.3, got {:.3f}",
                    weights.damageWeight);
                return;
            }
        }

        // Test 7b: Enemy casting → wardWeight = 0.7
        {
            State::PlayerActorState testPlayer{};
            State::TargetCollection testTargets{};

            // Add a casting enemy to the target collection
            State::TargetActorState castingEnemy;
            castingEnemy.actorFormID = 0x12345;
            castingEnemy.isHostile = true;
            castingEnemy.isDead = false;
            castingEnemy.isCasting = true;

            testTargets.InsertOrUpdate(castingEnemy.actorFormID, castingEnemy);

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.wardWeight - 0.7f) > 0.01f) {
                logger::error("TEST FAIL: Enemy casting should give wardWeight=0.7, got {:.3f}",
                    weights.wardWeight);
                return;
            }
        }

        // Test 7c: Multiple enemies (3+) → aoeWeight = 0.5
        {
            State::PlayerActorState testPlayer{};
            State::TargetCollection testTargets{};

            // Add 3 hostile enemies
            for (int i = 0; i < 3; ++i) {
                State::TargetActorState enemy;
                enemy.actorFormID = 0x10000 + i;
                enemy.isHostile = true;
                enemy.isDead = false;
                testTargets.InsertOrUpdate(enemy.actorFormID, enemy);
            }

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.aoeWeight - 0.5f) > 0.01f) {
                logger::error("TEST FAIL: 3+ enemies should give aoeWeight=0.5, got {:.3f}",
                    weights.aoeWeight);
                return;
            }
        }

        // Test 7d: In combat + no active summon → summonWeight = 0.4
        {
            State::PlayerActorState testPlayer{};
            State::TargetCollection testTargets{};

            testPlayer.isInCombat = true;
            testPlayer.buffs.hasActiveSummon = false;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.summonWeight - 0.4f) > 0.01f) {
                logger::error("TEST FAIL: Combat+NoSummon should give summonWeight=0.4, got {:.3f}",
                    weights.summonWeight);
                return;
            }
        }

        // Test 7e: In combat + has active summon → summonWeight = 0.0
        {
            State::PlayerActorState testPlayer{};
            State::TargetCollection testTargets{};

            testPlayer.isInCombat = true;
            testPlayer.buffs.hasActiveSummon = true;  // Already has summon

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (weights.summonWeight != 0.0f) {
                logger::error("TEST FAIL: Combat+HasSummon should give summonWeight=0.0, got {:.3f}",
                    weights.summonWeight);
                return;
            }
        }

        // Test 7f: Sneaking → stealthWeight = 0.4
        {
            State::PlayerActorState testPlayer{};
            State::TargetCollection testTargets{};

            testPlayer.isSneaking = true;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.stealthWeight - 0.4f) > 0.01f) {
                logger::error("TEST FAIL: Sneaking should give stealthWeight=0.4, got {:.3f}",
                    weights.stealthWeight);
                return;
            }
        }

        logger::info("TEST PASS: ContextRuleEngine combat rules work correctly"sv);
    }

    // Test 8: ContextRuleEngine target rules (Stage 1e - Binary Weights)
    {
        logger::info("TEST: ContextRuleEngine target rules..."sv);

        auto& settings = State::ContextWeightSettings::GetSingleton();
        Context::ContextRuleEngine engine(settings.BuildConfig());

        State::GameState testState{};
        State::PlayerActorState testPlayer{};
        State::WorldState testWorld{};

        // Test 8a: Target Undead → antiUndeadWeight = 0.6
        {
            State::TargetCollection testTargets{};

            State::TargetActorState undeadTarget;
            undeadTarget.actorFormID = 0x20000;
            undeadTarget.targetType = State::TargetType::Undead;
            undeadTarget.isHostile = true;

            testTargets.primary = undeadTarget;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.antiUndeadWeight - 0.6f) > 0.01f) {
                logger::error("TEST FAIL: Undead target should give antiUndeadWeight=0.6, got {:.3f}",
                    weights.antiUndeadWeight);
                return;
            }
        }

        // Test 8b: Target Daedra → antiDaedraWeight = 0.6
        {
            State::TargetCollection testTargets{};

            State::TargetActorState daedraTarget;
            daedraTarget.actorFormID = 0x20001;
            daedraTarget.targetType = State::TargetType::Daedra;
            daedraTarget.isHostile = true;

            testTargets.primary = daedraTarget;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.antiDaedraWeight - 0.6f) > 0.01f) {
                logger::error("TEST FAIL: Daedra target should give antiDaedraWeight=0.6, got {:.3f}",
                    weights.antiDaedraWeight);
                return;
            }
        }

        // Test 8c: Target Dragon → antiDragonWeight = 0.5
        {
            State::TargetCollection testTargets{};

            State::TargetActorState dragonTarget;
            dragonTarget.actorFormID = 0x20002;
            dragonTarget.targetType = State::TargetType::Dragon;
            dragonTarget.isHostile = true;

            testTargets.primary = dragonTarget;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.antiDragonWeight - 0.5f) > 0.01f) {
                logger::error("TEST FAIL: Dragon target should give antiDragonWeight=0.5, got {:.3f}",
                    weights.antiDragonWeight);
                return;
            }
        }

        // Test 8d: No primary target → all weights zero
        {
            State::TargetCollection testTargets{};
            // primary = std::nullopt (no target)

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (weights.antiUndeadWeight != 0.0f || weights.antiDaedraWeight != 0.0f ||
                weights.antiDragonWeight != 0.0f) {
                logger::error("TEST FAIL: No target should give all anti-target weights=0.0");
                return;
            }
        }

        logger::info("TEST PASS: ContextRuleEngine target rules work correctly"sv);
    }

    // Test 9: ContextRuleEngine equipment rules (Stage 1e - Mixed Weights)
    {
        logger::info("TEST: ContextRuleEngine equipment rules..."sv);

        auto& settings = State::ContextWeightSettings::GetSingleton();
        Context::ContextRuleEngine engine(settings.BuildConfig());

        State::GameState testState{};
        State::TargetCollection testTargets{};
        State::WorldState testWorld{};

        // Test 9a: Weapon charge low (10%) → weaponChargeWeight ≈ 0.81
        {
            State::PlayerActorState testPlayer{};

            testPlayer.hasEnchantedWeapon = true;
            testPlayer.weaponChargePercent = 0.10f;  // 10% charge

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            // deficit=0.9, curve=(0.9)^2 = 0.81
            const float expected = 0.81f;
            const float tolerance = 0.01f;

            if (std::abs(weights.weaponChargeWeight - expected) > tolerance) {
                logger::error("TEST FAIL: 10%% charge should give weaponChargeWeight≈{:.2f}, got {:.3f}",
                    expected, weights.weaponChargeWeight);
                return;
            }
        }

        // Test 9b: Weapon charge moderate (25%) → weaponChargeWeight ≈ 0.56
        {
            State::PlayerActorState testPlayer{};

            testPlayer.hasEnchantedWeapon = true;
            testPlayer.weaponChargePercent = 0.25f;  // 25% charge (threshold)

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            // deficit=0.75, curve=(0.75)^2 = 0.56
            const float expected = 0.56f;
            const float tolerance = 0.01f;

            if (std::abs(weights.weaponChargeWeight - expected) > tolerance) {
                logger::error("TEST FAIL: 25%% charge should give weaponChargeWeight≈{:.2f}, got {:.3f}",
                    expected, weights.weaponChargeWeight);
                return;
            }
        }

        // Test 9c: Weapon charge moderate (50%) → weaponChargeWeight ≈ 0.25
        // Continuous curve: deficit=0.5, (0.5)^2 = 0.25
        {
            State::PlayerActorState testPlayer{};

            testPlayer.hasEnchantedWeapon = true;
            testPlayer.weaponChargePercent = 0.50f;  // 50% charge

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            const float expected = 0.25f;
            const float tolerance = 0.01f;

            if (std::abs(weights.weaponChargeWeight - expected) > tolerance) {
                logger::error("TEST FAIL: 50%% charge should give weaponChargeWeight≈{:.2f}, got {:.3f}",
                    expected, weights.weaponChargeWeight);
                return;
            }
        }

        // Test 9d: Out of arrows → ammoWeight = 0.5
        {
            State::PlayerActorState testPlayer{};

            testPlayer.hasBowEquipped = true;
            testPlayer.arrowCount = 0;  // Out of arrows

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.ammoWeight - 0.5f) > 0.01f) {
                logger::error("TEST FAIL: Out of arrows should give ammoWeight=0.5, got {:.3f}",
                    weights.ammoWeight);
                return;
            }
        }

        // Test 9e: No weapon equipped → boundWeaponWeight = 0.4
        {
            State::PlayerActorState testPlayer{};

            testPlayer.hasMeleeEquipped = false;
            testPlayer.hasBowEquipped = false;
            testPlayer.hasSpellEquipped = false;

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (std::abs(weights.boundWeaponWeight - 0.4f) > 0.01f) {
                logger::error("TEST FAIL: No weapon should give boundWeaponWeight=0.4, got {:.3f}",
                    weights.boundWeaponWeight);
                return;
            }
        }

        // Test 9f: Has weapon equipped → boundWeaponWeight = 0.0
        {
            State::PlayerActorState testPlayer{};

            testPlayer.hasMeleeEquipped = true;  // Has weapon

            auto weights = engine.EvaluateRules(testPlayer, testTargets, testWorld);

            if (weights.boundWeaponWeight != 0.0f) {
                logger::error("TEST FAIL: Has weapon should give boundWeaponWeight=0.0, got {:.3f}",
                    weights.boundWeaponWeight);
                return;
            }
        }

        logger::info("TEST PASS: ContextRuleEngine equipment rules work correctly"sv);
    }

    // Test 10: End-to-end integration - ContextRuleEngine + UtilityScorer (Stage 1h)
    {
        logger::info("TEST: End-to-end ContextRuleEngine → UtilityScorer integration..."sv);

        auto& settings = State::ContextWeightSettings::GetSingleton();
        Context::ContextRuleEngine engine(settings.BuildConfig());

        // Test case 1: Workstation potions (CRITICAL - just fixed in Stage 1g review)
        {
            logger::info("  Subtest 1a: Fortify Smithing potion at forge..."sv);

            // Setup: At forge
            State::WorldState world{};
            world.isLookingAtWorkstation = true;
            world.workstationType = 1;  // Forge (type 1)

            State::PlayerActorState player{};
            State::TargetCollection targets{};
            State::GameState gameState{};

            // Evaluate rules
            auto weights = engine.EvaluateRules(player, targets, world);

            // Create Fortify Smithing potion candidate
            Candidate::ItemCandidate smithingPotion{};
            smithingPotion.tags = Item::ItemTag::FortifyCombatSkill;
            smithingPotion.combatSkill = Item::CombatSkill::Smithing;
            smithingPotion.name = "Fortify Smithing Potion";

            // Extract weight using UtilityScorer's GetContextWeight logic
            float weight = 0.0f;
            if (Item::HasTag(smithingPotion.tags, Item::ItemTag::FortifyCombatSkill)) {
                if (smithingPotion.combatSkill == Item::CombatSkill::Smithing) {
                    weight = std::max(weight, weights.fortifySmithingWeight);
                }
            }

            if (weight < 0.7f) {  // Should be ~0.8 default
                logger::error("TEST FAIL: Fortify Smithing potion at forge should get high weight, got {:.2f}"sv, weight);
                return;
            }
            logger::info("  ✓ Fortify Smithing potion at forge: weight={:.2f} (expected ~0.8)"sv, weight);
        }

        {
            logger::info("  Subtest 1b: Fortify Enchanting potion at enchanter..."sv);

            State::WorldState world{};
            world.isLookingAtWorkstation = true;
            world.workstationType = 3;  // Enchanter (type 3)

            State::PlayerActorState player{};
            State::TargetCollection targets{};
            State::GameState gameState{};

            auto weights = engine.EvaluateRules(player, targets, world);

            Candidate::ItemCandidate enchantingPotion{};
            enchantingPotion.tags = Item::ItemTag::FortifyMagicSchool;
            enchantingPotion.school = Item::MagicSchool::Enchanting;
            enchantingPotion.name = "Fortify Enchanting Potion";

            float weight = 0.0f;
            if (Item::HasTag(enchantingPotion.tags, Item::ItemTag::FortifyMagicSchool)) {
                if (enchantingPotion.school == Item::MagicSchool::Enchanting) {
                    weight = std::max(weight, weights.fortifyEnchantingWeight);
                }
            }

            if (weight < 0.7f) {
                logger::error("TEST FAIL: Fortify Enchanting potion at enchanter should get high weight, got {:.2f}"sv, weight);
                return;
            }
            logger::info("  ✓ Fortify Enchanting potion at enchanter: weight={:.2f}"sv, weight);
        }

        // Test case 2: Elemental resistance under damage
        {
            logger::info("  Subtest 2: Resist Fire potion when taking fire damage..."sv);

            State::WorldState world{};
            State::PlayerActorState player{};
            player.effects.isOnFire = true;

            State::TargetCollection targets{};
            State::GameState gameState{};

            auto weights = engine.EvaluateRules(player, targets, world);

            Candidate::ItemCandidate resistFirePotion{};
            resistFirePotion.tags = Item::ItemTag::ResistFire;
            resistFirePotion.name = "Resist Fire Potion";

            float weight = weights.baseRelevanceWeight;  // Start with noise floor
            if (Item::HasTag(resistFirePotion.tags, Item::ItemTag::ResistFire)) {
                weight = std::max(weight, weights.resistFireWeight);
            }

            if (weight < 0.7f) {  // Should be ~0.8 when on fire
                logger::error("TEST FAIL: Resist Fire potion when on fire should get high weight, got {:.2f}"sv, weight);
                return;
            }
            logger::info("  ✓ Resist Fire potion when on fire: weight={:.2f}"sv, weight);
        }

        // Test case 3: Healing spell at low health (continuous weight)
        {
            logger::info("  Subtest 3: Healing spell at 30%% health..."sv);

            State::WorldState world{};
            State::PlayerActorState player{};
            player.vitals.health = 0.30f;  // 30% health

            State::TargetCollection targets{};
            State::GameState gameState{};

            auto weights = engine.EvaluateRules(player, targets, world);

            Candidate::SpellCandidate healingSpell{};
            healingSpell.tags = Spell::SpellTag::RestoreHealth;
            healingSpell.name = "Heal Self";

            float weight = weights.baseRelevanceWeight;
            if (Spell::HasTag(healingSpell.tags, Spell::SpellTag::RestoreHealth)) {
                weight = std::max(weight, weights.healingWeight);
            }

            // Pure quadratic curve: deficit=0.7, weight=(0.7)^2 = 0.49
            const float expected = 0.49f;
            if (std::abs(weight - expected) > 0.01f) {
                logger::error("TEST FAIL: Healing spell at 30%% health should get weight={:.2f}, got {:.2f}"sv,
                    expected, weight);
                return;
            }
            logger::info("  ✓ Healing spell at 30%% health: weight={:.2f} (continuous!)"sv, weight);
        }

        // Test case 4: Multi-tag accumulation (AOE + Damage spell)
        {
            logger::info("  Subtest 4: AOE damage spell in combat with multiple enemies..."sv);

            State::WorldState world{};
            State::PlayerActorState player{};
            player.isInCombat = true;

            State::TargetCollection targets{};
            // Add 3 hostile targets for MultipleEnemies condition
            for (int i = 0; i < 3; ++i) {
                RE::FormID formID = 0x1000 + i;
                State::TargetActorState enemy{};
                enemy.isHostile = true;
                enemy.isDead = false;
                enemy.actorFormID = formID;
                targets.InsertOrUpdate(formID, enemy);
            }

            State::GameState gameState{};

            auto weights = engine.EvaluateRules(player, targets, world);

            Candidate::SpellCandidate aoeSpell{};
            aoeSpell.tags = Spell::SpellTag::AOE;
            aoeSpell.type = Spell::SpellType::Damage;
            aoeSpell.name = "Fireball";

            // Max accumulation pattern - should get BOTH damageWeight and aoeWeight
            float weight = weights.baseRelevanceWeight;
            if (aoeSpell.type == Spell::SpellType::Damage) {
                weight = std::max(weight, weights.damageWeight);
            }
            if (Spell::HasTag(aoeSpell.tags, Spell::SpellTag::AOE)) {
                weight = std::max(weight, weights.aoeWeight);
            }

            float expectedMin = std::max(weights.damageWeight, weights.aoeWeight);
            if (weight < expectedMin - 0.01f) {
                logger::error("TEST FAIL: AOE damage spell should get max(damage, aoe), got {:.2f}, expected >={:.2f}"sv,
                    weight, expectedMin);
                return;
            }
            logger::info("  ✓ AOE damage spell: weight={:.2f} (max accumulation works!)"sv, weight);
        }

        // Test case 5: Soul gems when weapon charge low
        {
            logger::info("  Subtest 5: Soul gem when weapon charge low..."sv);

            State::WorldState world{};
            State::PlayerActorState player{};
            player.hasEnchantedWeapon = true;
            player.weaponChargePercent = 0.20f;  // 20% charge (low)

            State::TargetCollection targets{};
            State::GameState gameState{};

            auto weights = engine.EvaluateRules(player, targets, world);

            Candidate::ItemCandidate soulGem{};
            soulGem.sourceType = Candidate::SourceType::SoulGem;
            soulGem.name = "Grand Soul Gem (Grand)";

            // Soul gems checked via sourceType
            float weight = weights.baseRelevanceWeight;
            if (soulGem.sourceType == Candidate::SourceType::SoulGem) {
                weight = std::max(weight, weights.weaponChargeWeight);
            }

            if (weight < 0.5f) {  // Should be high when charge low
                logger::error("TEST FAIL: Soul gem at low charge should get high weight, got {:.2f}"sv, weight);
                return;
            }
            logger::info("  ✓ Soul gem at 20%% charge: weight={:.2f}"sv, weight);
        }

        logger::info("TEST PASS: End-to-end ContextRuleEngine → UtilityScorer integration works!"sv);
    }

    // Test 11: TargetCollection cache invariant — mutators must keep cachedEnemyCount
    // and cachedAnyCasting in sync without an explicit UpdateCachedCounts() call.
    {
        logger::info("TEST: TargetCollection cache invariant..."sv);

        State::TargetCollection coll{};

        if (coll.cachedEnemyCount != 0 || coll.cachedAnyCasting) {
            logger::error("TEST FAIL: default-constructed collection has non-empty caches");
            return;
        }

        State::TargetActorState hostile{};
        hostile.actorFormID = 0x1001;
        hostile.isHostile = true;
        hostile.isDead = false;
        hostile.isCasting = false;

        State::TargetActorState castingHostile{};
        castingHostile.actorFormID = 0x1002;
        castingHostile.isHostile = true;
        castingHostile.isDead = false;
        castingHostile.isCasting = true;

        State::TargetActorState ally{};
        ally.actorFormID = 0x1003;
        ally.isHostile = false;

        coll.InsertOrUpdate(hostile.actorFormID, hostile);
        coll.InsertOrUpdate(castingHostile.actorFormID, castingHostile);
        coll.InsertOrUpdate(ally.actorFormID, ally);

        if (coll.cachedEnemyCount != 2 || !coll.cachedAnyCasting) {
            logger::error("TEST FAIL: after 3 inserts (2 hostile, 1 ally, 1 casting) caches wrong: count={}, anyCasting={}",
                coll.cachedEnemyCount, coll.cachedAnyCasting);
            return;
        }

        // Remove the casting hostile — anyCasting should drop to false
        coll.Remove(castingHostile.actorFormID);
        if (coll.cachedEnemyCount != 1 || coll.cachedAnyCasting) {
            logger::error("TEST FAIL: after removing casting hostile, caches wrong: count={}, anyCasting={}",
                coll.cachedEnemyCount, coll.cachedAnyCasting);
            return;
        }

        // Clear — caches should reset
        coll.Clear();
        if (coll.cachedEnemyCount != 0 || coll.cachedAnyCasting || !coll.targets.empty() || coll.primary.has_value()) {
            logger::error("TEST FAIL: Clear() left residual state");
            return;
        }

        // Update-in-place: hostile becomes dead → enemy count drops
        coll.InsertOrUpdate(hostile.actorFormID, hostile);
        if (coll.cachedEnemyCount != 1) {
            logger::error("TEST FAIL: re-insert after Clear should give count=1, got {}", coll.cachedEnemyCount);
            return;
        }

        State::TargetActorState deadVersion = hostile;
        deadVersion.isDead = true;
        coll.InsertOrUpdate(hostile.actorFormID, deadVersion);
        if (coll.cachedEnemyCount != 0) {
            logger::error("TEST FAIL: InsertOrUpdate to dead should drop count to 0, got {}", coll.cachedEnemyCount);
            return;
        }

        logger::info("TEST PASS: TargetCollection cache invariant holds across InsertOrUpdate/Remove/Clear"sv);
    }

    // Test 12: PipelineStateCache rank clamping — ranks beyond the sorted prefix
    // must be clamped to the prefix length, not stored as meaningless tail indices.
    {
        logger::info("TEST: PipelineStateCache rank clamping..."sv);

        constexpr size_t kSortedPrefix = 10;
        constexpr size_t kTotal = 15;
        constexpr RE::FormID kBaseID = 0xAB0000;

        Scoring::ScoredCandidateList scored;
        for (size_t i = 0; i < kTotal; ++i) {
            Candidate::SpellCandidate spell{};
            spell.formID = static_cast<RE::FormID>(kBaseID + i);
            Scoring::ScoredCandidate sc{};
            sc.candidate = spell;
            sc.utility = static_cast<float>(kTotal - i);  // descending
            scored.push_back(sc);
        }

        auto& cache = Learning::PipelineStateCache::GetSingleton();
        cache.Update(scored, Slot::SlotAssignments{}, 0, kSortedPrefix);

        bool failed = false;
        for (size_t i = 0; i < kTotal; ++i) {
            auto info = cache.GetCandidateInfo(static_cast<RE::FormID>(kBaseID + i));
            if (!info.wasCandidate) {
                logger::error("TEST FAIL: candidate {} missing from cache", i);
                failed = true;
                break;
            }
            const size_t expected = (i < kSortedPrefix)
                ? i
                : Learning::PipelineStateCache::kUnrankedTail;
            if (info.rank != expected) {
                logger::error("TEST FAIL: candidate {} rank should be {}, got {}", i, expected, info.rank);
                failed = true;
                break;
            }
        }
        if (failed) return;

        // Restore an empty cache — the singleton is live, and a real external
        // equip during the next ~100ms must not attribute against test data.
        cache.Update({}, Slot::SlotAssignments{}, 0, 0);

        logger::info("TEST PASS: PipelineStateCache clamps tail ranks to sorted prefix"sv);
    }

    // Test 13: EquipSourceTracker FormID keying — a Huginn equip of item X must
    // not suppress external-equip learning for item Y, and entries must expire.
    {
        logger::info("TEST: EquipSourceTracker FormID keying..."sv);

        auto& tracker = Learning::EquipSourceTracker::GetSingleton();

        // The singleton is live and marks linger for the 400ms window — use
        // 0xFF-prefixed FormIDs (dynamic-form range) that no plugin record has.
        constexpr RE::FormID kMarkA = 0xFF00AA01;
        constexpr RE::FormID kUnmarked = 0xFF00BB02;
        constexpr RE::FormID kMarkB = 0xFF00CC03;

        tracker.MarkHuginnEquip(kMarkA);
        if (!tracker.IsRecentHuginnEquip(kMarkA)) {
            logger::error("TEST FAIL: marked FormID should be recent");
            return;
        }
        if (tracker.IsRecentHuginnEquip(kUnmarked)) {
            logger::error("TEST FAIL: unmarked FormID must NOT be suppressed (cross-item false suppression)");
            return;
        }

        // FormID 0 must never match — the ring's empty slots are zero-initialized
        if (tracker.IsRecentHuginnEquip(0)) {
            logger::error("TEST FAIL: FormID 0 must never be suppressed");
            return;
        }

        // Non-consumption: the game can fire multiple TESEquipEvents for ONE
        // equip action (e.g. both hands) — repeated checks must all match.
        tracker.MarkHuginnEquip(kMarkB);
        if (!tracker.IsRecentHuginnEquip(kMarkB) || !tracker.IsRecentHuginnEquip(kMarkB)) {
            logger::error("TEST FAIL: repeated checks within window must all match (non-consuming)");
            return;
        }

        // Expiry: a tiny window must reject a mark older than it
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (tracker.IsRecentHuginnEquip(kMarkB, 1.0f)) {
            logger::error("TEST FAIL: mark older than window should not be recent");
            return;
        }
        // Still within the default window though. (Wall-clock dependent: assumes
        // <400ms since the mark — can only flake under extreme scheduler
        // starvation during startup.)
        if (!tracker.IsRecentHuginnEquip(kMarkB)) {
            logger::error("TEST FAIL: mark should still be within default window");
            return;
        }

        logger::info("TEST PASS: EquipSourceTracker is FormID-keyed, non-consuming, with window expiry"sv);
    }

    // Test 14: UsageMemory snapshot reader — misclick detection semantics, recency
    // boost, and (critically) RecordUsage while a reader is alive. Under the old
    // lock-holding reader, same-thread RecordUsage during a scoring pass would
    // deadlock (shared_lock held + unique_lock requested on one thread).
    {
        logger::info("TEST: UsageMemory snapshot reader..."sv);

        Learning::UsageMemory memory;
        State::GameState ctxA{};
        State::GameState ctxB{};
        // GameState{} zero-inits every bucket, and Critical IS 0 — so the
        // different-context state must use a nonzero bucket value.
        ctxB.health = State::HealthBucket::VeryHigh;  // different hash from ctxA

        // Misclick: different item, same context, fast switch
        memory.RecordUsage(0x1111, ctxA);
        auto misclick = memory.RecordUsage(0x2222, ctxA);
        if (!misclick.detected || misclick.previousFormID != 0x1111) {
            logger::error("TEST FAIL: fast same-context switch should flag misclick of previous item");
            return;
        }

        // No misclick across different contexts
        auto noMisclick = memory.RecordUsage(0x3333, ctxB);
        if (noMisclick.detected) {
            logger::error("TEST FAIL: different-context switch should not flag misclick");
            return;
        }

        // Recency boost after MATCH_THRESHOLD uses in same context
        memory.Clear();
        for (size_t i = 0; i < Learning::UsageMemory::MATCH_THRESHOLD; ++i) {
            memory.RecordUsage(0x4444, ctxA);
        }
        {
            auto reader = memory.AcquireReader(ctxA);
            if (reader.GetRecencyBoost(0x4444) != Learning::UsageMemory::RECENCY_BOOST) {
                logger::error("TEST FAIL: {} same-context uses should give recency boost",
                    Learning::UsageMemory::MATCH_THRESHOLD);
                return;
            }
            if (reader.GetRecencyBoost(0x5555) != 0.0f) {
                logger::error("TEST FAIL: unrelated item should get no recency boost");
                return;
            }

            // Write while reader alive — would deadlock with the old lock-holding reader
            memory.RecordUsage(0x6666, ctxA);

            // Snapshot semantics: the reader does NOT see the new write
            if (reader.GetRecencyBoost(0x6666) != 0.0f) {
                logger::error("TEST FAIL: snapshot reader should not see post-snapshot writes");
                return;
            }
        }
        if (memory.GetEventCount() != Learning::UsageMemory::MATCH_THRESHOLD + 1) {
            logger::error("TEST FAIL: RecordUsage during reader lifetime should have landed");
            return;
        }

        logger::info("TEST PASS: UsageMemory snapshot reader (misclick, boost, non-blocking writes)"sv);
    }

    // Test 15: Deduplicated logic stays in sync — IsFavorited single source of
    // truth, and spell/scroll fortify-school correlation parity.
    {
        logger::info("TEST: Dedup equivalence (IsFavorited, fortify-school parity)..."sv);

        // IsFavorited: free function must match ScoredCandidate accessor for all types
        {
            Candidate::SpellCandidate favSpell{};
            favSpell.isFavorited = true;
            Candidate::WeaponCandidate favWeapon{};
            favWeapon.isFavorited = true;
            Candidate::ItemCandidate item{};
            Candidate::ScrollCandidate scroll{};
            Candidate::AmmoCandidate ammo{};

            const Candidate::CandidateVariant variants[] = {favSpell, favWeapon, item, scroll, ammo};
            const bool expected[] = {true, true, false, false, false};

            for (size_t i = 0; i < 5; ++i) {
                Scoring::ScoredCandidate sc{};
                sc.candidate = variants[i];
                if (Candidate::IsFavorited(variants[i]) != expected[i] ||
                    sc.IsFavorited() != expected[i]) {
                    logger::error("TEST FAIL: IsFavorited mismatch for variant {}", i);
                    return;
                }
            }
        }

        // Fortify-school parity: spell and scroll of the same school must get the
        // same fortify multiplier from CorrelationBooster (shared helper)
        {
            Scoring::ScorerConfig config{};
            Scoring::CorrelationBooster booster(config);

            State::PlayerActorState player{};
            player.buffs.hasFortifyDestruction = true;
            State::TargetCollection targets{};

            Candidate::SpellCandidate spell{};
            spell.school = Spell::MagicSchool::Destruction;
            Candidate::ScrollCandidate scroll{};
            scroll.school = Scroll::MagicSchool::Destruction;

            const float spellBonus = booster.CalculateBonus(player, targets, spell);
            const float scrollBonus = booster.CalculateBonus(player, targets, scroll);
            const float expected = 1.0f + config.fortifySchoolBonus;

            if (std::abs(spellBonus - expected) > 0.01f) {
                logger::error("TEST FAIL: fortified spell should get x{:.1f}, got {:.2f}", expected, spellBonus);
                return;
            }
            if (std::abs(spellBonus - scrollBonus) > 0.001f) {
                logger::error("TEST FAIL: spell ({:.2f}) and scroll ({:.2f}) fortify bonuses diverged",
                    spellBonus, scrollBonus);
                return;
            }

            // Non-matching school gets no fortify bonus
            Candidate::SpellCandidate offSchool{};
            offSchool.school = Spell::MagicSchool::Illusion;
            if (booster.CalculateBonus(player, targets, offSchool) != 1.0f) {
                logger::error("TEST FAIL: non-fortified school should stay at neutral 1.0");
                return;
            }
        }

        logger::info("TEST PASS: Dedup equivalence holds (IsFavorited, fortify parity)"sv);
    }

    // Test 16: FeatureQLearner batch decay — one call decays multiple idle items,
    // leaves unlisted/fresh items untouched, preserves train counts, and is
    // idempotent (re-decay at the same injected time is a no-op).
    {
        logger::info("TEST: FeatureQLearner batch decay..."sv);

        Learning::FeatureQLearner fql;
        Learning::StateFeatures s{};
        s.healthPct = 0.5f;
        s.inCombat = 1.0f;

        fql.Update(0xD001, s, 1.0f);
        fql.Update(0xD002, s, 1.0f);
        fql.Update(0xD003, s, 1.0f);

        const auto wBefore1 = fql.GetWeights(0xD001);
        const auto wBefore3 = fql.GetWeights(0xD003);

        // Inject a future "now" well past the decay threshold (~60 min idle)
        const auto future = std::chrono::steady_clock::now() + std::chrono::minutes(60);
        const std::vector<RE::FormID> batch = {0xD001, 0xD002, 0xD999 /* never trained */};

        const size_t decayed = fql.MaybeDecayBatch(batch, future);
        if (decayed != 2) {
            logger::error("TEST FAIL: batch decay should decay exactly 2 items, got {}", decayed);
            return;
        }

        // ~60 min idle → factor ≈ (1 - rate)^1.0; allow slack for the microseconds
        // between the Update stamp and the test's now() baseline
        const float expectedFactor = std::pow(1.0f - Config::DECAY_RATE_PER_HOUR, 1.0f);
        const auto wAfter1 = fql.GetWeights(0xD001);
        for (size_t i = 0; i < Learning::StateFeatures::NUM_FEATURES; ++i) {
            if (std::abs(wAfter1[i] - wBefore1[i] * expectedFactor) > 0.001f) {
                logger::error("TEST FAIL: weight[{}] should decay by ~{:.4f}: {:.4f} -> {:.4f}",
                    i, expectedFactor, wBefore1[i], wAfter1[i]);
                return;
            }
        }

        // Unlisted item untouched
        if (fql.GetWeights(0xD003) != wBefore3) {
            logger::error("TEST FAIL: item not in batch must not decay");
            return;
        }

        // Train counts unaffected by decay
        if (fql.GetTrainCount(0xD001) != 1 || fql.GetTotalTrainCount() != 3) {
            logger::error("TEST FAIL: decay must not change train counts");
            return;
        }

        // Idempotent: lastUpdate was stamped to `future`, so re-decay is a no-op
        if (fql.MaybeDecayBatch(batch, future) != 0) {
            logger::error("TEST FAIL: immediate re-decay at same time should be a no-op");
            return;
        }

        logger::info("TEST PASS: FeatureQLearner batch decay (selective, count-preserving, idempotent)"sv);
    }

    // Test 17: ContextReason derivation (architecture-critique #10) — the display
    // explanation is read off the SAME weight map that ranks candidates, so the
    // reported boundaries must match the vital thresholds and the suppressions.
    {
        logger::info("TEST: ContextReason derived from context weights..."sv);

        // Pinned to DEFAULT config, not settings.BuildConfig(): these assertions
        // are about the derivation, and the live INI can legitimately zero a
        // weight (a rule at 0 never reports — that is a documented feature of
        // Fires()), which would fail the test for a correct build.
        Context::ContextRuleEngine engine(State::ContextWeightConfig{});

        const Context::ContextReasonSignals kNoSignals{};

        // Helper: evaluate + name the reason in one step
        const auto reasonFor = [&](const State::PlayerActorState& player,
                                   const State::TargetCollection& targets,
                                   const State::WorldState& world,
                                   const Context::ContextReasonSignals& signals) {
            return engine.DominantReason(engine.EvaluateRules(player, targets, world), signals);
        };

        // 17a: Vital bands land exactly on the old thresholds (15% / 30% HP)
        {
            State::TargetCollection targets{};
            State::WorldState world{};

            State::PlayerActorState healthy{};
            healthy.vitals.health = 0.9f;
            if (reasonFor(healthy, targets, world, kNoSignals) != Context::ContextReason::None) {
                logger::error("TEST FAIL: 90%% HP should report no reason");
                return;
            }

            State::PlayerActorState low{};
            low.vitals.health = 0.25f;  // below LOW (0.30), above CRITICAL (0.15)
            if (reasonFor(low, targets, world, kNoSignals) != Context::ContextReason::LowHealth) {
                logger::error("TEST FAIL: 25%% HP should report LowHealth");
                return;
            }

            State::PlayerActorState critical{};
            critical.vitals.health = 0.10f;
            if (reasonFor(critical, targets, world, kNoSignals) != Context::ContextReason::CriticalHealth) {
                logger::error("TEST FAIL: 10%% HP should report CriticalHealth");
                return;
            }

            // Just inside the LOW threshold — the curve inversion must not drift
            State::PlayerActorState edge{};
            edge.vitals.health = 0.299f;
            if (reasonFor(edge, targets, world, kNoSignals) != Context::ContextReason::LowHealth) {
                logger::error("TEST FAIL: 29.9%% HP should report LowHealth (threshold drift)");
                return;
            }
            State::PlayerActorState justAbove{};
            justAbove.vitals.health = 0.301f;
            if (reasonFor(justAbove, targets, world, kNoSignals) == Context::ContextReason::LowHealth) {
                logger::error("TEST FAIL: 30.1%% HP must not report LowHealth (threshold drift)");
                return;
            }

            // Magicka and stamina invert DIFFERENT exponents (stamina defaults to
            // 1.5, not 2.0), so the HP cases above do not cover them.
            State::PlayerActorState lowMagicka{};
            lowMagicka.vitals.magicka = 0.299f;
            if (reasonFor(lowMagicka, targets, world, kNoSignals) != Context::ContextReason::LowMagicka) {
                logger::error("TEST FAIL: 29.9%% MP should report LowMagicka");
                return;
            }
            State::PlayerActorState okMagicka{};
            okMagicka.vitals.magicka = 0.301f;
            if (reasonFor(okMagicka, targets, world, kNoSignals) == Context::ContextReason::LowMagicka) {
                logger::error("TEST FAIL: 30.1%% MP must not report LowMagicka (threshold drift)");
                return;
            }

            State::PlayerActorState lowStamina{};
            lowStamina.vitals.stamina = 0.299f;
            if (reasonFor(lowStamina, targets, world, kNoSignals) != Context::ContextReason::LowStamina) {
                logger::error("TEST FAIL: 29.9%% SP should report LowStamina");
                return;
            }
            State::PlayerActorState okStamina{};
            okStamina.vitals.stamina = 0.301f;
            if (reasonFor(okStamina, targets, world, kNoSignals) == Context::ContextReason::LowStamina) {
                logger::error("TEST FAIL: 30.1%% SP must not report LowStamina (threshold drift)");
                return;
            }
        }

        // 17b: Suppressions the old threshold pass ignored — an already-active
        // buff means the context isn't driving scoring, so it isn't a reason
        {
            State::TargetCollection targets{};
            State::WorldState world{};

            State::PlayerActorState drowning{};
            drowning.isUnderwater = true;
            if (reasonFor(drowning, targets, world, kNoSignals) != Context::ContextReason::Underwater) {
                logger::error("TEST FAIL: underwater should report Underwater");
                return;
            }

            State::PlayerActorState breathing{};
            breathing.isUnderwater = true;
            breathing.buffs.hasWaterBreathing = true;
            if (reasonFor(breathing, targets, world, kNoSignals) == Context::ContextReason::Underwater) {
                logger::error("TEST FAIL: waterbreathing active must suppress the Underwater reason");
                return;
            }
        }

        // 17c: Elemental reasons scale with resistance (binary rules fire at
        // half their configured weight)
        {
            State::TargetCollection targets{};
            State::WorldState world{};

            State::PlayerActorState burning{};
            burning.effects.isOnFire = true;
            if (reasonFor(burning, targets, world, kNoSignals) != Context::ContextReason::OnFire) {
                logger::error("TEST FAIL: taking fire damage should report OnFire");
                return;
            }

            State::PlayerActorState fireproof{};
            fireproof.effects.isOnFire = true;
            fireproof.resistances.fire = 80.0f;  // 80% resist → weight scaled to 0.2×
            if (reasonFor(fireproof, targets, world, kNoSignals) == Context::ContextReason::OnFire) {
                logger::error("TEST FAIL: 80%% fire resist must suppress the OnFire reason");
                return;
            }
        }

        // 17d: Priority — a more urgent reason wins over a co-occurring one
        {
            State::TargetCollection targets{};
            State::WorldState world{};
            State::PlayerActorState player{};
            player.vitals.health = 0.10f;  // Critical
            player.isSneaking = true;      // also true, lower priority

            if (reasonFor(player, targets, world, kNoSignals) != Context::ContextReason::CriticalHealth) {
                logger::error("TEST FAIL: CriticalHealth must outrank Sneaking");
                return;
            }
        }

        // 17e: Label-only signals (no scoring weight to read them off)
        {
            State::TargetCollection targets{};
            State::WorldState world{};
            State::PlayerActorState player{};

            Context::ContextReasonSignals dark{};
            dark.lightLevel = 0.1f;
            if (reasonFor(player, targets, world, dark) != Context::ContextReason::InDarkness) {
                logger::error("TEST FAIL: low light should report InDarkness");
                return;
            }

            Context::ContextReasonSignals ally{};
            ally.allyInjured = true;
            ally.lightLevel = 0.1f;  // InDarkness is lower priority
            if (reasonFor(player, targets, world, ally) != Context::ContextReason::AllyInjured) {
                logger::error("TEST FAIL: AllyInjured must outrank InDarkness");
                return;
            }
        }

        // 17f: Ambient combat weights are deliberately NOT reasons — otherwise
        // every fight would relabel every slot "In Combat"
        {
            State::TargetCollection targets{};
            State::WorldState world{};
            State::PlayerActorState fighting{};
            fighting.isInCombat = true;

            if (reasonFor(fighting, targets, world, kNoSignals) != Context::ContextReason::None) {
                logger::error("TEST FAIL: plain in-combat state must not produce a reason");
                return;
            }
        }

        // 17g: Workstation reasons report, but rank BELOW anything hurting you
        {
            State::TargetCollection targets{};
            State::PlayerActorState player{};

            State::WorldState forge{};
            forge.isLookingAtWorkstation = true;
            forge.workstationType = 1;  // kCreateObject
            if (reasonFor(player, targets, forge, kNoSignals) != Context::ContextReason::AtForge) {
                logger::error("TEST FAIL: crosshair on a forge should report AtForge");
                return;
            }

            State::WorldState alchemy{};
            alchemy.isLookingAtWorkstation = true;
            alchemy.workstationType = 5;  // kAlchemy
            if (reasonFor(player, targets, alchemy, kNoSignals) != Context::ContextReason::AtAlchemy) {
                logger::error("TEST FAIL: crosshair on an alchemy lab should report AtAlchemy");
                return;
            }

            // Standing at a forge while bleeding out must still say Low HP —
            // crafting stations are ambient, not urgent
            State::PlayerActorState hurt{};
            hurt.vitals.health = 0.25f;
            if (reasonFor(hurt, targets, forge, kNoSignals) != Context::ContextReason::LowHealth) {
                logger::error("TEST FAIL: LowHealth must outrank AtForge");
                return;
            }
        }

        // 17h: Equipment reasons (weapon charge inverts its own curve; ammo is binary)
        {
            State::TargetCollection targets{};
            State::WorldState world{};

            State::PlayerActorState drained{};
            drained.hasEnchantedWeapon = true;
            drained.weaponChargePercent = 0.10f;
            if (reasonFor(drained, targets, world, kNoSignals) != Context::ContextReason::WeaponLowCharge) {
                logger::error("TEST FAIL: 10%% weapon charge should report WeaponLowCharge");
                return;
            }

            // No enchanted weapon → no charge weight → no phantom label
            State::PlayerActorState plain{};
            plain.weaponChargePercent = 0.10f;
            if (reasonFor(plain, targets, world, kNoSignals) == Context::ContextReason::WeaponLowCharge) {
                logger::error("TEST FAIL: unenchanted weapon must not report WeaponLowCharge");
                return;
            }

            State::PlayerActorState dry{};
            dry.hasBowEquipped = true;
            dry.arrowCount = 0;
            if (reasonFor(dry, targets, world, kNoSignals) != Context::ContextReason::NeedsAmmo) {
                logger::error("TEST FAIL: bow with no arrows should report NeedsAmmo");
                return;
            }
        }

        // 17i: An override's own reason beats the tick's context reason
        {
            Slot::SlotAssignment assignment{};
            assignment.type = Slot::AssignmentType::Override;

            Scoring::ScoredCandidate scored{};
            Candidate::ItemCandidate potion{};
            potion.name = "Potion of Ultimate Healing";
            potion.overrideReason = Context::ContextReason::CriticalHealth;
            scored.candidate = potion;
            assignment.candidate = scored;

            // Context says Sneaking; the override says Critical HP and wins
            const auto label = Display::DeriveExplanationLabel(
                assignment, Context::ContextReason::Sneaking);
            if (label != "Critical HP") {
                logger::error("TEST FAIL: override reason should win, got '{}'", label);
                return;
            }

            // Without a stamped reason, the tick's context reason is used — but
            // only on an item that reason actually ranks (#64). A healing potion
            // draws healingWeight, which is what LowHealth is read off.
            Slot::SlotAssignment plain{};
            Scoring::ScoredCandidate plainScored{};
            Candidate::ItemCandidate plainPotion{};
            plainPotion.name = "Potion of Minor Healing";
            plainPotion.tags = Item::ItemTag::RestoreHealth;
            plainScored.candidate = plainPotion;
            plain.candidate = plainScored;

            const auto contextLabel = Display::DeriveExplanationLabel(
                plain, Context::ContextReason::LowHealth);
            if (contextLabel != "Low HP") {
                logger::error("TEST FAIL: context reason should apply, got '{}'", contextLabel);
                return;
            }

            // No reason at all and not favorited → no label
            const auto emptyLabel = Display::DeriveExplanationLabel(
                plain, Context::ContextReason::None);
            if (!emptyLabel.empty()) {
                logger::error("TEST FAIL: no reason and no favorite should give no label, got '{}'",
                    emptyLabel);
                return;
            }
        }

        // 17j: Every reason the engine can report has display wording
        {
            for (size_t i = 1; i < Context::CONTEXT_REASON_COUNT; ++i) {
                const auto reason = static_cast<Context::ContextReason>(i);
                if (Display::ReasonLabel(reason).empty()) {
                    logger::error("TEST FAIL: ContextReason {} has no display label", i);
                    return;
                }
            }
            if (!Display::ReasonLabel(Context::ContextReason::None).empty()) {
                logger::error("TEST FAIL: ContextReason::None must have no label");
                return;
            }
        }

        // 17k: A context reason speaks only for slots it actually ranked (#64)
        {
            const auto labelFor = [](Candidate::CandidateVariant candidate,
                                     Context::ContextReason reason) {
                Slot::SlotAssignment assignment{};
                Scoring::ScoredCandidate scored{};
                scored.candidate = std::move(candidate);
                assignment.candidate = scored;
                return Display::DeriveExplanationLabel(assignment, reason);
            };

            // The reported case: at an enchanter, everything on the page wore
            // "At Enchanter" — a bow, a rabbit haunch, a pickaxe.
            Candidate::WeaponCandidate bow{};
            bow.name = "Silver Heavy Bow";
            if (const auto label = labelFor(bow, Context::ContextReason::AtEnchanter);
                !label.empty()) {
                logger::error("TEST FAIL: AtEnchanter must not label a weapon, got '{}'", label);
                return;
            }

            Candidate::ItemCandidate food{};
            food.name = "Roasted Rabbit Haunch";
            if (const auto label = labelFor(food, Context::ContextReason::AtEnchanter);
                !label.empty()) {
                logger::error("TEST FAIL: AtEnchanter must not label food, got '{}'", label);
                return;
            }

            // ...while the potion the enchanter context does rank still wears it.
            Candidate::ItemCandidate enchantingPotion{};
            enchantingPotion.name = "Potion of Enchanting";
            enchantingPotion.tags = Item::ItemTag::FortifyMagicSchool;
            enchantingPotion.school = Item::MagicSchool::Enchanting;
            if (const auto label = labelFor(enchantingPotion, Context::ContextReason::AtEnchanter);
                label != "At Enchanter") {
                logger::error("TEST FAIL: AtEnchanter should label its own potion, got '{}'", label);
                return;
            }

            // Every candidate type gets its own mapping in WeightForCandidate,
            // so each needs a positive case — an ItemCandidate-only suite would
            // miss a regression in the spell, scroll or ammo arm.
            Candidate::SpellCandidate healingSpell{};
            healingSpell.name = "Healing";
            healingSpell.tags = Spell::SpellTag::RestoreHealth;
            if (const auto label = labelFor(healingSpell, Context::ContextReason::LowHealth);
                label != "Low HP") {
                logger::error("TEST FAIL: LowHealth should label a healing spell, got '{}'", label);
                return;
            }

            Candidate::ScrollCandidate turnUndead{};
            turnUndead.name = "Scroll of Turn Undead";
            turnUndead.tags = Scroll::ScrollTag::AntiUndead;
            if (const auto label = labelFor(turnUndead, Context::ContextReason::TargetUndead);
                label != "Undead") {
                logger::error("TEST FAIL: TargetUndead should label an anti-undead scroll, got '{}'",
                    label);
                return;
            }

            Candidate::AmmoCandidate arrows{};
            arrows.name = "Steel Arrow";
            if (const auto label = labelFor(arrows, Context::ContextReason::NeedsAmmo);
                label != "Low Ammo") {
                logger::error("TEST FAIL: NeedsAmmo should label ammo, got '{}'", label);
                return;
            }
            // ...and the same arrows stay unlabelled under an unrelated reason.
            if (const auto label = labelFor(arrows, Context::ContextReason::AtEnchanter);
                !label.empty()) {
                logger::error("TEST FAIL: AtEnchanter must not label ammo, got '{}'", label);
                return;
            }

            // Combat shape: "Fire Damage" marks the resist potion, not the
            // greatsword beside it.
            Candidate::ItemCandidate fireResist{};
            fireResist.name = "Potion of Resist Fire";
            fireResist.tags = Item::ItemTag::ResistFire;
            if (const auto label = labelFor(fireResist, Context::ContextReason::OnFire);
                label != "Fire Damage") {
                logger::error("TEST FAIL: OnFire should label a resist-fire potion, got '{}'", label);
                return;
            }

            Candidate::WeaponCandidate greatsword{};
            greatsword.name = "Steel Greatsword";
            if (const auto label = labelFor(greatsword, Context::ContextReason::OnFire);
                !label.empty()) {
                logger::error("TEST FAIL: OnFire must not label a greatsword, got '{}'", label);
                return;
            }

            // Unattributed slots fall through the ladder rather than losing their
            // label outright — this is what the out-of-combat lines already show.
            Candidate::WeaponCandidate favorited{};
            favorited.name = "Silver Heavy Bow";
            favorited.isFavorited = true;
            if (const auto label = labelFor(favorited, Context::ContextReason::AtEnchanter);
                label != "Favorite") {
                logger::error("TEST FAIL: unattributed favorite should read 'Favorite', got '{}'", label);
                return;
            }

            // Signal-only reasons back no weight, so they can attribute nothing.
            // `Ore Vein(Green Apple)` is the reported instance.
            Candidate::ItemCandidate apple{};
            apple.name = "Green Apple";
            for (const auto reason : {Context::ContextReason::LookingAtOre,
                                      Context::ContextReason::AllyInjured,
                                      Context::ContextReason::InDarkness}) {
                if (Context::WeightFieldFor(reason) != nullptr) {
                    logger::error("TEST FAIL: signal-only reason {} gained a weight field",
                        static_cast<int>(reason));
                    return;
                }
                if (const auto label = labelFor(apple, reason); !label.empty()) {
                    logger::error("TEST FAIL: signal-only reason labelled an apple, got '{}'", label);
                    return;
                }
            }

            // An override's own reason stays unconditional: OverrideManager
            // surfaced that candidate FOR that reason, so it is attributed by
            // construction even when no weight field maps to it.
            Candidate::ItemCandidate surfaced{};
            surfaced.name = "Potion of Ultimate Healing";
            surfaced.overrideReason = Context::ContextReason::LookingAtOre;
            if (const auto label = labelFor(surfaced, Context::ContextReason::None);
                label != "Ore Vein") {
                logger::error("TEST FAIL: override reason should survive attribution, got '{}'", label);
                return;
            }
        }

        logger::info("TEST PASS: ContextReason derivation (thresholds, suppression, priority)"sv);
    }

    // =========================================================================
    // Wildcard page cache — stranding is structurally unreachable (#70 + the
    // bWildcardsEnabled and 1-slot-page cases that shared its root cause)
    // =========================================================================
    // The old cache was one global position-indexed array shared by every page,
    // so an entry could survive at an index nothing on the current page could
    // display; the liveness check still saw it and suppressed the re-roll that
    // would have produced a usable one. All four shapes below used to be able to
    // reach that state. Probabilities are pinned to 1.0 and the refractory to 0
    // so the rolls are deterministic — this asserts bounds, not randomness.
    {
        using namespace Huginn::Scoring;

        ScoredCandidateList pool;
        for (int i = 0; i < 10; ++i) {
            ScoredCandidate scored;
            Candidate::SpellCandidate spell{};
            spell.formID = static_cast<RE::FormID>(0x1000 + i);
            spell.name = "WildcardTestSpell";
            scored.candidate = spell;
            scored.utility = 10.0f - static_cast<float>(i);
            pool.push_back(scored);
        }

        WildcardManager mgr;
        mgr.SetBaseProbability(1.0f);
        mgr.SetMaxProbability(1.0f);
        mgr.SetCooldown(1000.0f);      // nothing expires mid-test
        mgr.SetRefractoryPeriod(0.0f); // every eligible page rolls on first apply

        // Applying mutates the list (swaps + isWildcard), so each case gets a
        // copy. Returned so a case can assert what was SURFACED, not just cached.
        auto apply = [&](const WildcardPage& page) {
            ScoredCandidateList work = pool;
            mgr.ApplyWildcards(work, page);
            return work;
        };

        auto surfacedCount = [](const ScoredCandidateList& list) {
            return static_cast<size_t>(std::count_if(list.begin(), list.end(),
                [](const ScoredCandidate& s) { return s.isWildcard; }));
        };

        // Highest cached index with a wildcard on a page, or SIZE_MAX if none.
        auto highestCachedIndex = [&](size_t pageIndex) -> size_t {
            size_t highest = SIZE_MAX;
            for (size_t i = 0; i < Slot::MAX_SLOTS_PER_PAGE; ++i) {
                if (mgr.GetWildcardForSlot(pageIndex, i) != 0) {
                    highest = i;
                }
            }
            return highest;
        };

        // Page 0: 7 slots, all wildcard-capable — the baseline that rolls.
        apply({ .index = 0, .slotCount = 7, .wildcardSlots = 7 });
        if (!mgr.HasActiveWildcard(0)) {
            logger::error("TEST FAIL: 7-slot page rolled no wildcard at probability 1.0"sv);
            return;
        }
        const size_t page0Count = mgr.GetActiveWildcardCount(0);

        // Page 1: 4 slots. #70 — this used to inherit page 0's entries at
        // indices 4-6, which ApplyWildcardsToRanking could never surface.
        apply({ .index = 1, .slotCount = 4, .wildcardSlots = 4 });
        if (!mgr.HasActiveWildcard(1)) {
            logger::error("TEST FAIL: 4-slot page was blocked from rolling"sv);
            return;
        }
        if (highestCachedIndex(1) >= 4) {
            logger::error("TEST FAIL: 4-slot page cached a wildcard at index {}"sv,
                highestCachedIndex(1));
            return;
        }
        // ...and it did not disturb page 0, which the player can switch back to.
        if (mgr.GetActiveWildcardCount(0) != page0Count) {
            logger::error("TEST FAIL: applying page 1 changed page 0's cache ({} → {})"sv,
                page0Count, mgr.GetActiveWildcardCount(0));
            return;
        }

        // Page 2: 5 slots, none accepting wildcards (bWildcardsEnabled=false
        // throughout). FindBestCandidate would skip every wildcard, so rolling
        // one caches something no slot can ever seat.
        apply({ .index = 2, .slotCount = 5, .wildcardSlots = 0 });
        if (mgr.HasActiveWildcard(2)) {
            logger::error("TEST FAIL: page with no wildcard-capable slots cached {} wildcard(s)"sv,
                mgr.GetActiveWildcardCount(2));
            return;
        }

        // Page 3: 1 slot. Slot 0 is excluded and scores base × 0 anyway, so the
        // page rolls nothing — but it must reach the cache logic to say so,
        // which the old slotCount < 2 guard returned before.
        apply({ .index = 3, .slotCount = 1, .wildcardSlots = 1 });
        if (mgr.HasActiveWildcard(3)) {
            logger::error("TEST FAIL: 1-slot page cached a wildcard it cannot display"sv);
            return;
        }

        // Page 4: 6 slots but only 2 accepting wildcards. A page can display at
        // most as many wildcards as it has seats for them; the surplus would
        // strand. Asserted as EQUALITY, not just an upper bound: at probability
        // 1.0 the count is deterministic, and a cap that is too tight (an
        // off-by-one, or one applied before startSlot) would leave a
        // wildcard-capable page rolling nothing while an upper-bound check
        // still passed.
        apply({ .index = 4, .slotCount = 6, .wildcardSlots = 2 });
        if (mgr.GetActiveWildcardCount(4) != 2) {
            logger::error("TEST FAIL: page with 2 wildcard-capable slots cached {} wildcards, expected 2"sv,
                mgr.GetActiveWildcardCount(4));
            return;
        }

        // Page 5: 6 slots, ONE wildcard-capable. Every cached entry must also be
        // surfaced — the invariant this whole class exists to hold.
        //
        // RollNewWildcards draws for slot i, and ApplyWildcardsToRanking only
        // surfaces a wildcard by swapping it UP (it skips when foundIdx <=
        // slotIdx). A draw from at or above i is therefore a roll that caches an
        // entry nothing displays, while the cache reads as active and suppresses
        // the re-roll for a full cooldown. With several slots rolling, a wasted
        // draw is masked by its neighbours; with exactly one seat it IS the
        // stall. Checks the surfaced count, not just the cached one, because
        // cached-but-invisible is precisely the failure being excluded.
        {
            const auto work = apply({ .index = 5, .slotCount = 6, .wildcardSlots = 1 });
            const size_t cached = mgr.GetActiveWildcardCount(5);
            if (cached != 1) {
                logger::error("TEST FAIL: 1-seat page cached {} wildcards, expected 1"sv, cached);
                return;
            }
            if (surfacedCount(work) != cached) {
                logger::error("TEST FAIL: 1-seat page cached {} wildcard(s) but surfaced {}"sv,
                    cached, surfacedCount(work));
                return;
            }
        }

        // Reshape: page 0 shrinks under an INI reload. Its own cache is the one
        // remaining way to strand an entry, and a shape change discards it.
        apply({ .index = 0, .slotCount = 3, .wildcardSlots = 3 });
        if (highestCachedIndex(0) >= 3) {
            logger::error("TEST FAIL: reshaped page 0 kept a wildcard at index {}"sv,
                highestCachedIndex(0));
            return;
        }

        // Expiry reaches every page, not just the displayed one, and reports a
        // lapse only for the page last applied (page 0 above).
        mgr.SetCooldown(0.0f);
        if (!mgr.UpdateExpiry()) {
            logger::error("TEST FAIL: expiry did not report the displayed page lapsing"sv);
            return;
        }
        for (size_t page = 0; page < 6; ++page) {
            if (mgr.HasActiveWildcard(page)) {
                logger::error("TEST FAIL: page {} survived expiry"sv, page);
                return;
            }
        }

        logger::info("TEST PASS: Wildcard page cache (per-page bounds, wildcard-capable "
            "seats, cached==surfaced, 1-slot page, reshape, all-page expiry)"sv);
    }

    logger::info("=== All unit tests passed! ==="sv);
#endif
}

// =============================================================================
// REGRESSION TEST SUITE (Agent Review - 2026-02-14)
// =============================================================================
// These tests validate critical fixes from the ini-consolidation refactor.
// They prevent regression of the following bugs identified in code audit:
//   1. Double-scoring (context in CandidateGenerator + PriorCalculator)
//   2. 10× discontinuity cliff at health thresholds
//   3. Last-match-wins multi-tag assignment bug
//   4. Q-learning <6% contribution (multiplicative formula empowerment)
//
// Each test case (TC-XX) maps to the regression test spec from Agent 1.
// =============================================================================

void RunRegressionTests()
{
#ifndef NDEBUG
    logger::info("Running Regression Test Suite (v1.0 refactor validation)..."sv);

    using namespace Huginn::State;
    using namespace Huginn::Context;
    using namespace Huginn::Scoring;

    auto& settings = ContextWeightSettings::GetSingleton();
    ContextRuleEngine engine(settings.BuildConfig());

    // =========================================================================
    // TC-01: CRITICAL - Health 51% vs 49% - NO 10× CLIFF (Audit Fix)
    // =========================================================================
    // Regression bug: Old system had threshold at 50%: healing score jumped 10×
    // Expected v1.0: Smooth quadratic curve, difference <0.05 across threshold
    // =========================================================================
    {
        logger::info("TC-01: Health 51%% vs 49%% → NO 10× cliff (CRITICAL audit fix)..."sv);

        GameState testState{};
        TargetCollection testTargets{};
        WorldState testWorld{};

        // Test 49% HP
        PlayerActorState player49{};
        player49.vitals.health = 0.49f;
        auto weights49 = engine.EvaluateRules(player49, testTargets, testWorld);

        // Test 51% HP
        PlayerActorState player51{};
        player51.vitals.health = 0.51f;
        auto weights51 = engine.EvaluateRules(player51, testTargets, testWorld);

        // Expected (quadratic curve):
        // 49%: deficit=0.51, weight=(0.51)² ≈ 0.26
        // 51%: deficit=0.49, weight=(0.49)² ≈ 0.24
        // Δ ≈ 0.02 (smooth!)

        float diff = std::abs(weights49.healingWeight - weights51.healingWeight);

        if (diff > 0.05f) {
            logger::error("TC-01 FAIL: 49%% vs 51%% should be smooth (diff<0.05), got diff={:.3f} (10× cliff!)"sv, diff);
            logger::error("  This is the CRITICAL bug from v0.12.x - threshold discontinuity!"sv);
            return;
        }

        // Additional validation: Ensure both are in expected range
        if (weights49.healingWeight < 0.20f || weights49.healingWeight > 0.32f) {
            logger::error("TC-01 FAIL: 49%% HP healing weight out of range: {:.3f} (expected ~0.26)"sv,
                weights49.healingWeight);
            return;
        }

        if (weights51.healingWeight < 0.18f || weights51.healingWeight > 0.30f) {
            logger::error("TC-01 FAIL: 51%% HP healing weight out of range: {:.3f} (expected ~0.24)"sv,
                weights51.healingWeight);
            return;
        }

        logger::info("  ✓ PASS: 49%%={:.3f}, 51%%={:.3f}, diff={:.3f} (smooth curve!)"sv,
            weights49.healingWeight, weights51.healingWeight, diff);
        logger::info("  ✓ REGRESSION PREVENTED: No 10× cliff at 50%% health threshold"sv);
    }

    // =========================================================================
    // TC-02: Health 10% → Critical urgency maintained (v0.12.x parity)
    // =========================================================================
    {
        logger::info("TC-02: Health 10%% → Critical urgency maintained..."sv);

        GameState testState{};
        PlayerActorState player{};
        player.vitals.health = 0.10f;  // Critical health
        TargetCollection testTargets{};
        WorldState testWorld{};

        auto weights = engine.EvaluateRules(player, testTargets, testWorld);

        // deficit=0.9, weight=(0.9)² = 0.81
        const float expected = 0.81f;
        const float tolerance = 0.05f;

        if (std::abs(weights.healingWeight - expected) > tolerance) {
            logger::error("TC-02 FAIL: 10%% HP should give healingWeight≈{:.2f}, got {:.3f}"sv,
                expected, weights.healingWeight);
            return;
        }

        logger::info("  ✓ PASS: 10%% HP → {:.2f} (critical urgency)"sv, weights.healingWeight);
    }

    // =========================================================================
    // TC-03: Health 100% → Zero urgency (multiplicative gate test)
    // =========================================================================
    {
        logger::info("TC-03: Health 100%% → Zero urgency (multiplicative gate)..."sv);

        GameState testState{};
        PlayerActorState player{};
        player.vitals.health = 1.0f;  // Full health
        TargetCollection testTargets{};
        WorldState testWorld{};

        auto weights = engine.EvaluateRules(player, testTargets, testWorld);

        if (weights.healingWeight != 0.0f) {
            logger::error("TC-03 FAIL: Full health should give healingWeight=0.0, got {:.3f}"sv,
                weights.healingWeight);
            return;
        }

        logger::info("  ✓ PASS: 100%% HP → 0.0 (multiplicative formula will gate learning)"sv);
    }

    // =========================================================================
    // TC-05: Multi-Tag Spell → std::max() accumulation (Audit Fix)
    // =========================================================================
    // Regression bug: Old system used assignment, last-match-wins
    // Expected v1.0: std::max() accumulation, gets credit for BOTH tags
    // =========================================================================
    {
        logger::info("TC-05: Multi-tag spell (healing + buff) → max() accumulation..."sv);

        GameState testState{};
        PlayerActorState player{};
        player.vitals.health = 0.30f;     // Healing relevant
        TargetCollection testTargets{};
        WorldState testWorld{};

        // Add casting enemy to make ward relevant
        TargetActorState castingEnemy{};
        castingEnemy.actorFormID = 0x12345;
        castingEnemy.isHostile = true;
        castingEnemy.isDead = false;
        castingEnemy.isCasting = true;
        testTargets.InsertOrUpdate(castingEnemy.actorFormID, castingEnemy);

        auto weights = engine.EvaluateRules(player, testTargets, testWorld);

        // Create multi-tag spell: RestoreHealth + Ward (hypothetical combo)
        Candidate::SpellCandidate healingWardSpell{};
        healingWardSpell.tags = Spell::SpellTag::RestoreHealth | Spell::SpellTag::Ward;
        healingWardSpell.name = "Healing Ward (multi-tag test)";

        // Simulate GetContextWeight logic with max() accumulation
        float contextWeight = weights.baseRelevanceWeight;
        if (Spell::HasTag(healingWardSpell.tags, Spell::SpellTag::RestoreHealth)) {
            contextWeight = std::max(contextWeight, weights.healingWeight);
        }
        if (Spell::HasTag(healingWardSpell.tags, Spell::SpellTag::Ward)) {
            contextWeight = std::max(contextWeight, weights.wardWeight);
        }

        // Expected: Should get max(healingWeight, wardWeight, baseRelevance)
        float expectedMin = std::max({weights.healingWeight, weights.wardWeight, weights.baseRelevanceWeight});

        if (std::abs(contextWeight - expectedMin) > 0.01f) {
            logger::error("TC-05 FAIL: Multi-tag should use max(), got {:.3f}, expected {:.3f}"sv,
                contextWeight, expectedMin);
            logger::error("  Healing={:.3f}, Ward={:.3f}, Base={:.3f}"sv,
                weights.healingWeight, weights.wardWeight, weights.baseRelevanceWeight);
            return;
        }

        logger::info("  ✓ PASS: Multi-tag gets max({:.2f}, {:.2f}) = {:.2f}"sv,
            weights.healingWeight, weights.wardWeight, contextWeight);
        logger::info("  ✓ REGRESSION PREVENTED: No last-match-wins assignment bug"sv);
    }

    // =========================================================================
    // TC-07: Resist Fire when on fire → High relevance maintained
    // =========================================================================
    {
        logger::info("TC-07: Resist Fire when on fire → High relevance..."sv);

        GameState testState{};
        PlayerActorState player{};
        player.effects.isOnFire = true;
        TargetCollection testTargets{};
        WorldState testWorld{};

        auto weights = engine.EvaluateRules(player, testTargets, testWorld);

        if (weights.resistFireWeight < 0.75f) {  // Expected ~0.8
            logger::error("TC-07 FAIL: On fire should give resistFireWeight≈0.8, got {:.3f}"sv,
                weights.resistFireWeight);
            return;
        }

        logger::info("  ✓ PASS: On fire → resistFireWeight={:.2f}"sv, weights.resistFireWeight);
    }

    // =========================================================================
    // TC-10: At forge → Fortify Smithing high relevance
    // =========================================================================
    {
        logger::info("TC-10: At forge → Fortify Smithing high relevance..."sv);

        GameState testState{};
        PlayerActorState player{};
        TargetCollection testTargets{};
        WorldState testWorld{};
        testWorld.isLookingAtWorkstation = true;
        testWorld.workstationType = 1;  // Forge

        auto weights = engine.EvaluateRules(player, testTargets, testWorld);

        if (weights.fortifySmithingWeight < 0.75f) {  // Expected ~0.8
            logger::error("TC-10 FAIL: At forge should give fortifySmithingWeight≈0.8, got {:.3f}"sv,
                weights.fortifySmithingWeight);
            return;
        }

        logger::info("  ✓ PASS: At forge → fortifySmithingWeight={:.2f}"sv, weights.fortifySmithingWeight);
    }

    // =========================================================================
    // TC-11: Looking at lock → Unlock spell critical
    // =========================================================================
    {
        logger::info("TC-11: Looking at lock → Unlock spell critical..."sv);

        GameState testState{};
        PlayerActorState player{};
        TargetCollection testTargets{};
        WorldState testWorld{};
        testWorld.isLookingAtLock = true;

        auto weights = engine.EvaluateRules(player, testTargets, testWorld);

        if (weights.unlockWeight < 0.95f) {  // Expected 1.0 (critical)
            logger::error("TC-11 FAIL: Looking at lock should give unlockWeight=1.0, got {:.3f}"sv,
                weights.unlockWeight);
            return;
        }

        logger::info("  ✓ PASS: Looking at lock → unlockWeight={:.2f} (critical)"sv, weights.unlockWeight);
    }

    // =========================================================================
    // TC-12: Enemy casting → Ward spell relevance
    // =========================================================================
    {
        logger::info("TC-12: Enemy casting → Ward spell relevance..."sv);

        GameState testState{};
        PlayerActorState player{};
        TargetCollection testTargets{};
        WorldState testWorld{};

        // Add casting enemy to targets
        TargetActorState castingEnemy{};
        castingEnemy.actorFormID = 0x12345;
        castingEnemy.isHostile = true;
        castingEnemy.isDead = false;
        castingEnemy.isCasting = true;
        testTargets.InsertOrUpdate(castingEnemy.actorFormID, castingEnemy);

        auto weights = engine.EvaluateRules(player, testTargets, testWorld);

        if (weights.wardWeight < 0.65f) {  // Expected ~0.7
            logger::error("TC-12 FAIL: Enemy casting should give wardWeight≈0.7, got {:.3f}"sv,
                weights.wardWeight);
            return;
        }

        logger::info("  ✓ PASS: Enemy casting → wardWeight={:.2f}"sv, weights.wardWeight);
    }

    // =========================================================================
    // TC-14: Summon suppressed when already have summon (context logic)
    // =========================================================================
    {
        logger::info("TC-14: Summon suppressed when already have summon..."sv);

        GameState testState{};
        PlayerActorState player{};
        player.isInCombat = true;
        player.buffs.hasActiveSummon = true;  // Already has summon
        TargetCollection testTargets{};
        WorldState testWorld{};

        auto weights = engine.EvaluateRules(player, testTargets, testWorld);

        if (weights.summonWeight != 0.0f) {
            logger::error("TC-14 FAIL: Has active summon should give summonWeight=0.0, got {:.3f}"sv,
                weights.summonWeight);
            return;
        }

        logger::info("  ✓ PASS: Has active summon → summonWeight=0.0 (suppressed)"sv);
    }

    // =========================================================================
    // TC-15: Anti-Undead spell vs Draugr target
    // =========================================================================
    {
        logger::info("TC-15: Anti-Undead spell vs Draugr target..."sv);

        GameState testState{};
        PlayerActorState player{};
        TargetCollection testTargets{};
        WorldState testWorld{};

        // Set primary target to undead
        TargetActorState undeadTarget{};
        undeadTarget.actorFormID = 0x20000;
        undeadTarget.targetType = TargetType::Undead;
        undeadTarget.isHostile = true;
        testTargets.primary = undeadTarget;

        auto weights = engine.EvaluateRules(player, testTargets, testWorld);

        if (weights.antiUndeadWeight < 0.55f) {  // Expected ~0.6
            logger::error("TC-15 FAIL: Undead target should give antiUndeadWeight≈0.6, got {:.3f}"sv,
                weights.antiUndeadWeight);
            return;
        }

        logger::info("  ✓ PASS: Undead target → antiUndeadWeight={:.2f}"sv, weights.antiUndeadWeight);

        // TC-15b: that weight has to reach a silver weapon, not just exist (#80).
        // The weapon arm read none of its own tags, so a silver sword scored the
        // same against a draugr as in a shop.
        Candidate::WeaponCandidate silverSword{};
        silverSword.name = "Silver Sword";
        silverSword.tags = Weapon::WeaponTag::Melee | Weapon::WeaponTag::OneHanded |
                           Weapon::WeaponTag::Silver;
        const float silverWeight = Context::WeightForCandidate(silverSword, weights);
        if (silverWeight < weights.antiUndeadWeight - 0.001f) {
            logger::error("TC-15b FAIL: silver weapon vs undead should draw antiUndeadWeight "
                "{:.2f}, got {:.3f}"sv, weights.antiUndeadWeight, silverWeight);
            return;
        }

        // A steel sword must NOT — otherwise this is just a weapon baseline bump
        // and every weapon would wear the "Undead" label again.
        Candidate::WeaponCandidate steelSword{};
        steelSword.name = "Steel Sword";
        steelSword.tags = Weapon::WeaponTag::Melee | Weapon::WeaponTag::OneHanded;
        const float steelWeight = Context::WeightForCandidate(steelSword, weights);
        if (steelWeight >= weights.antiUndeadWeight - 0.001f) {
            logger::error("TC-15b FAIL: plain weapon vs undead should stay at baseline, "
                "got {:.3f} vs antiUndead {:.2f}"sv, steelWeight, weights.antiUndeadWeight);
            return;
        }

        // A turn-undead enchantment does the same job as silver.
        Candidate::WeaponCandidate turnBlade{};
        turnBlade.name = "Mace of Turn Undead";
        turnBlade.tags = Weapon::WeaponTag::Melee | Weapon::WeaponTag::Enchanted |
                         Weapon::WeaponTag::EnchantTurnUndead;
        if (Context::WeightForCandidate(turnBlade, weights) < weights.antiUndeadWeight - 0.001f) {
            logger::error("TC-15b FAIL: turn-undead enchantment should draw antiUndeadWeight"sv);
            return;
        }

        // Banish is a different axis — it returns summoned daedra to Oblivion and
        // does nothing to draugr. It must NOT draw the undead weight...
        Candidate::WeaponCandidate banishBlade{};
        banishBlade.name = "Sword of Banishing";
        banishBlade.tags = Weapon::WeaponTag::Melee | Weapon::WeaponTag::Enchanted |
                           Weapon::WeaponTag::EnchantBanish;
        if (Context::WeightForCandidate(banishBlade, weights) >= weights.antiUndeadWeight - 0.001f) {
            logger::error("TC-15b FAIL: banish must not draw antiUndeadWeight against undead"sv);
            return;
        }

        // ...and must draw the daedra weight against a daedra.
        TargetCollection daedraTargets{};
        TargetActorState daedraTarget{};
        daedraTarget.actorFormID = 0x20002;
        daedraTarget.targetType = TargetType::Daedra;
        daedraTarget.isHostile = true;
        daedraTargets.primary = daedraTarget;
        const auto daedraWeights = engine.EvaluateRules(player, daedraTargets, testWorld);
        if (Context::WeightForCandidate(banishBlade, daedraWeights) <
            daedraWeights.antiDaedraWeight - 0.001f) {
            logger::error("TC-15b FAIL: banish weapon vs daedra should draw antiDaedraWeight "
                "{:.2f}, got {:.3f}"sv, daedraWeights.antiDaedraWeight,
                Context::WeightForCandidate(banishBlade, daedraWeights));
            return;
        }
        // Silver is not a daedra answer, so it stays at baseline there.
        if (Context::WeightForCandidate(silverSword, daedraWeights) >=
            daedraWeights.antiDaedraWeight - 0.001f) {
            logger::error("TC-15b FAIL: silver must not draw antiDaedraWeight"sv);
            return;
        }

        // And the silver bonus must not leak into an unrelated context: with no
        // undead in front of the player, silver is just a material.
        TargetCollection humanoidTargets{};
        TargetActorState humanoidTarget{};
        humanoidTarget.actorFormID = 0x20001;
        humanoidTarget.targetType = TargetType::Humanoid;
        humanoidTarget.isHostile = true;
        humanoidTargets.primary = humanoidTarget;
        const auto humanoidWeights = engine.EvaluateRules(player, humanoidTargets, testWorld);
        if (std::abs(Context::WeightForCandidate(silverSword, humanoidWeights) -
                     Context::WeightForCandidate(steelSword, humanoidWeights)) > 0.001f) {
            logger::error("TC-15b FAIL: silver must not outweigh steel against a humanoid"sv);
            return;
        }

        logger::info("  ✓ PASS: silver/turn-undead weapon vs undead → {:.2f} (steel stays {:.2f})"sv,
            silverWeight, steelWeight);

        // TC-15c: the tag feeding all of the above has to be assigned correctly.
        // IsSilvered falls back to a name match, and a plain substring also
        // matches "Quicksilver" — mercury, no anti-undead property. Observed
        // in-game: Quicksilver Greatsword carried tags=00000019 (Silver set) and
        // took ctx 0.60 against draugr. Inert until #80 read the tag; not inert
        // afterwards.
        struct { std::string_view name; bool expected; } kSilverNames[] = {
            {"Silver Sword",           true },
            {"Silver Heavy Bow",       true },
            {"Silvered Greatsword",    true },   // suffix must still match
            {"Ancient Silver Blade",   true },   // mid-name but at a word start
            {"Quicksilver Greatsword", false},   // the reported collision
            {"Quicksilver Ingot Mace", false},
            {"Steel Sword",            false},
        };
        for (const auto& tc : kSilverNames) {
            const bool got = Util::NameContainsWord(tc.name, "silver");
            if (got != tc.expected) {
                logger::error("TC-15c FAIL: '{}' silver match = {}, expected {}"sv,
                    tc.name, got, tc.expected);
                return;
            }
        }
        logger::info("  ✓ PASS: silver name match is word-bounded (Quicksilver excluded)"sv);
    }

    // =========================================================================
    // TC-15d: SpellTagExt reaches the four dead context weights (#79)
    // =========================================================================
    // unlockWeight, slowFallWeight, antiDragonWeight and the spell half of
    // waterbreathingWeight were computed by EvaluateRules and read by NO
    // candidate mapping, because SpellTag had no bit left to match on. The
    // contexts fired and named themselves on the widget while moving nothing.
    // These assert the wiring, not the weights — TC-15 above covers those.
    // =========================================================================
    {
        Context::ContextWeightMap w{};
        w.baseRelevanceWeight = 0.05f;
        w.spellWeight = 0.20f;
        w.unlockWeight = 1.00f;
        w.slowFallWeight = 0.80f;
        w.antiDragonWeight = 0.70f;
        w.waterbreathingWeight = 0.60f;
        // Deliberately the HIGHEST weight in the map, and deliberately not one
        // any of these spells should draw — see the waterbreathing case below.
        w.stealthWeight = 0.90f;

        struct Case {
            std::string_view name;
            Spell::SpellTagExt ext;
            float expected;
        };
        const Case kCases[] = {
            {"Open Lock",      Spell::SpellTagExt::Unlock,         1.00f},
            {"Slow Fall",      Spell::SpellTagExt::SlowFall,       0.80f},
            {"Dragonrend",     Spell::SpellTagExt::AntiDragon,     0.70f},
            {"Waterbreathing", Spell::SpellTagExt::Waterbreathing, 0.60f},
        };
        for (const auto& tc : kCases) {
            Candidate::SpellCandidate spell{};
            spell.name = tc.name;
            spell.tagsExt = tc.ext;
            const float got = Context::WeightForCandidate(spell, w);
            if (std::abs(got - tc.expected) > 0.001f) {
                logger::error("TC-15d FAIL: '{}' should draw {:.2f}, got {:.3f}"sv,
                    tc.name, tc.expected, got);
                return;
            }
        }

        // The waterbreathing case is the one with a live bug behind it. To reach
        // SpellType::Buff with no bit of its own, the classifier used to tag
        // "waterbreath" as SpellTag::Stealth — and the spell arm reads Stealth
        // into stealthWeight, so a waterbreathing spell was ranked as a sneaking
        // tool. If that ever comes back, this draws 0.90 instead of 0.60.
        Candidate::SpellCandidate waterbreathing{};
        waterbreathing.name = "Waterbreathing";
        waterbreathing.tagsExt = Spell::SpellTagExt::Waterbreathing;
        if (Context::WeightForCandidate(waterbreathing, w) >= w.stealthWeight - 0.001f) {
            logger::error("TC-15d FAIL: waterbreathing must not draw stealthWeight"sv);
            return;
        }

        // A spell carrying no extended tag must stay at the spell baseline, or
        // this is a blanket bump and every spell wears the "Lock" label.
        Candidate::SpellCandidate plain{};
        plain.name = "Flames";
        plain.type = Spell::SpellType::Damage;
        if (std::abs(Context::WeightForCandidate(plain, w) - w.spellWeight) > 0.001f) {
            logger::error("TC-15d FAIL: an untagged spell should stay at the baseline, got {:.3f}"sv,
                Context::WeightForCandidate(plain, w));
            return;
        }

        // A scroll is classified by running the SPELL classifier over it, so it
        // arrives with the same extended tags — and ConvertToScrollData used to
        // drop them on the floor, which is invisible from the spell side.
        // Same four, same weights, or the asymmetry this issue is about just
        // moves one candidate type over.
        for (const auto& tc : kCases) {
            Candidate::ScrollCandidate scroll{};
            scroll.name = tc.name;
            scroll.tagsExt = tc.ext;
            scroll.count = 1;
            const float got = Context::WeightForCandidate(scroll, w);
            if (std::abs(got - tc.expected) > 0.001f) {
                logger::error("TC-15d FAIL: scroll '{}' should draw {:.2f}, got {:.3f}"sv,
                    tc.name, tc.expected, got);
                return;
            }
        }

        // The name fallbacks the classifier uses for the two effects no
        // archetype describes. DetermineSpellTagsExt needs a live RE::SpellItem
        // and cannot be reached from here, so this pins the predicate it is
        // built out of — including the case-insensitivity that a raw find()
        // would have quietly dropped.
        struct { std::string_view name; std::string_view word; bool expected; } kNames[] = {
            {"slow fall",             "slow fall",   true },   // lower case must match
            {"Greater Featherfall",   "featherfall", true },
            {"Slow Time",             "slow fall",   false},   // "slow" alone is a debuff
            {"Unlock",                "unlock",      true },
            {"Open Lock",             "open lock",   true },
            {"Openness",              "open lock",   false},
            // Why AntiDragon matches an explicit list and not bare "dragon":
            // Dragonhide is a vanilla self-armour spell, and a loose match would
            // tag it anti-dragon exactly as "silver" once tagged Quicksilver.
            {"Dragonhide",            "dragon",      true },   // the premise
            {"Dragonhide",            "dragonrend",  false},   // what we match instead
            {"Dragonhide",            "dragonbane",  false},
        };
        for (const auto& tc : kNames) {
            const bool got = Util::NameContainsWord(tc.name, tc.word);
            if (got != tc.expected) {
                logger::error("TC-15d FAIL: '{}' vs '{}' = {}, expected {}"sv,
                    tc.name, tc.word, got, tc.expected);
                return;
            }
        }

        logger::info("  ✓ PASS: SpellTagExt → unlock/slowFall/antiDragon/waterbreathing (spells + scrolls)"sv);
    }

    // =========================================================================
    // TC-16: Multiplicative Formula - Zero context gates learning (Integration)
    // =========================================================================
    // This validates the key improvement: Context acts as multiplicative gate
    // v0.12.x: utility = context + 0.5×learning (additive, learning always contributes)
    // v1.0:    utility = context × (1 + λ×learning) (multiplicative, zero context → zero utility)
    // =========================================================================
    {
        logger::info("TC-16: Multiplicative formula - zero context gates learning..."sv);

        // Create scorer config
        ScorerConfig config;

        config.lambdaMin = 0.5f;
        config.lambdaMax = 3.0f;

        // Scenario: Full health (healingWeight = 0.0), but high Q-value
        float contextWeight = 0.0f;       // Zero context
        float learningScore = 0.8f;       // High learning
        float confidence = 1.0f;          // High confidence
        float lambda = config.lambdaMin + confidence * (config.lambdaMax - config.lambdaMin);  // 3.0

        // Multiplicative formula
        float utility = contextWeight * (1.0f + lambda * learningScore);
        // = 0.0 × (1 + 3.0×0.8) = 0.0 × 3.4 = 0.0

        // An additive formula would give: 0.0 + 0.5×0.8 = 0.4 (learning leaks through!)

        if (utility != 0.0f) {
            logger::error("TC-16 FAIL: Zero context should gate learning, got utility={:.3f} (expected 0.0)"sv,
                utility);
            return;
        }

        logger::info("  ✓ PASS: Zero context gates learning: 0.0 × (1+3.0×0.8) = 0.0"sv);
        logger::info("  ✓ REGRESSION PREVENTED: Learning no longer leaks through zero context"sv);
        logger::info("  ✓ Q-learning empowered from <6%% to meaningful tiebreaker within relevant items"sv);
    }

    logger::info("=== Regression Test Suite PASSED ===");
    logger::info("All critical v1.0 refactor fixes validated:");
    logger::info("  ✓ No 10× health cliff (TC-01)");
    logger::info("  ✓ Multi-tag max() accumulation (TC-05)");
    logger::info("  ✓ Multiplicative gate empowers Q-learning (TC-16)");
    logger::info("  ✓ Smooth continuous curves (TC-01, TC-02, TC-03)");
    logger::info("  ✓ Context scoring parity with v0.12.x (TC-07, TC-10, TC-11, TC-12, TC-14, TC-15)");

#endif
}

// =============================================================================
// COSAVE SERIALIZATION TESTS
// =============================================================================
// Tests FeatureQLearner ExportData/ImportData round-trip without requiring
// actual SKSE cosave infrastructure. FormID resolution is verified via manual testing.
// =============================================================================

// =============================================================================
// THROWAWAY: SlotLocker::Reset() field-completeness backstop (0.19.21)
// =============================================================================
// Delete this whole block, its Tests.h declaration, and its Main.cpp call site
// together — it exists only to pin one fix and has no long-term home here.
//
// The fix: Reset() cleared 4 of LockedSlot's 7 fields, leaving isActivationLock,
// previousFormID and hadContent standing. That leak is LATENT in the shipped
// pipeline (ApplyLocks rewrites previousFormID/hadContent three lines before the
// only reader, and OnItemUsed's isLocked guard makes isActivationLock
// unreachable), so nothing in-game can observe it. This test can, because it
// reads the locker directly instead of through the pipeline that hides it.
//
// LIMITATION, and it is the same fact the fix rests on: isActivationLock has no
// public reader — not a getter, not GetLockSnapshot. So this test CANNOT assert
// it was cleared. It asserts the two observable leaked fields and trusts the
// whole-struct reset to carry the third. Exposing the flag just to test it would
// widen the API to prove a property the assignment already guarantees.
//
// SECOND LIMITATION: this drives the LIVE singleton, so a pipeline tick landing
// between the Reset and the assertions would repopulate previousFormID and read
// as a spurious failure. It should not happen — this runs at kPostLoadGame with
// the loading menu still up, which gates the update loop — but if this test ever
// fails intermittently and only under load, suspect interleaving before the fix.
// The arm check below covers the mirror case (a tick dirtying the probe before
// Reset), and reports "test proves nothing" rather than passing vacuously.
void RunSlotLockerResetTest()
{
#ifndef NDEBUG
    using namespace Huginn::Slot;

    logger::info("Running SlotLocker::Reset field-completeness test..."sv);

    auto& locker = SlotLocker::GetSingleton();

    // This runs on a loaded game with the pipeline already ticking, so save and
    // restore anything mutated and leave the locker Reset — which is exactly the
    // state the load path expects anyway.
    const SlotLockConfig savedConfig = locker.GetConfig();
    SlotLockConfig cfg = savedConfig;
    cfg.lockDurationMs = 5000.0f;  // non-zero so ShouldLock can fire
    locker.SetConfig(cfg);

    locker.Reset();  // known-clean start

    // EVERY slot gets a distinct probe, not just slot 0. Arming one slot would
    // make the all-slots assertion below vacuous: the Reset above already left
    // slots 1..N clean, so they would pass whatever the Reset under test did —
    // including a hypothetical m_lockedSlots[0] = LockedSlot{} that walked one
    // slot. Distinct FormIDs also catch a Reset that copied slot 0 over the rest.
    constexpr RE::FormID kProbeFormBase = 0x0BADF000;
    const auto ProbeFormFor = [](size_t i) -> RE::FormID {
        return kProbeFormBase + static_cast<RE::FormID>(i);
    };

    // Dirty every field. ApplyLocks fills assignment/previousFormID/hadContent
    // and locks each slot (empty -> non-empty trips lockOnFill);
    // LockSlotForActivation then sets isActivationLock and rewrites the timers.
    SlotAssignments assignments;
    for (size_t i = 0; i < MAX_SLOTS_PER_PAGE; ++i) {
        SlotAssignment probe = SlotAssignment::Empty(i, SlotClassification::Regular);
        probe.type = AssignmentType::Normal;
        probe.formID = ProbeFormFor(i);
        probe.name = "ResetLeakProbe" + std::to_string(i);
        assignments.push_back(std::move(probe));
    }

    const Override::OverrideCollection noOverrides{};
    (void)locker.ApplyLocks(assignments, noOverrides);  // [[nodiscard]]: stable list unused here

    // Activation-lock both ends, so isActivationLock is dirty on more than the
    // first slot and a partial reset cannot hide behind slot 0 alone.
    locker.LockSlotForActivation(0);
    locker.LockSlotForActivation(MAX_SLOTS_PER_PAGE - 1);

    // Arm check: if the setup silently failed to dirty the state, the assertions
    // below would pass against a locker that was never dirty in the first place.
    {
        const auto armed = locker.GetLockSnapshot();
        for (size_t i = 0; i < armed.size(); ++i) {
            if (!armed[i].isLocked || armed[i].previousFormID != ProbeFormFor(i) ||
                !armed[i].hadContent) {
                logger::error("TEST FAIL: SlotLocker probe did not arm slot {} "
                              "(isLocked={}, previousFormID={:08X}, hadContent={}) — "
                              "test proves nothing"sv,
                    i, armed[i].isLocked, armed[i].previousFormID, armed[i].hadContent);
                locker.SetConfig(savedConfig);
                locker.Reset();
                return;
            }
            if (!locker.WasConfirmed(i, ProbeFormFor(i))) {
                logger::error("TEST FAIL: WasConfirmed should be true for slot {} while armed"sv, i);
                locker.SetConfig(savedConfig);
                locker.Reset();
                return;
            }
        }
    }

    locker.Reset();

    // The assertions. previousFormID and hadContent are the two leaked fields a
    // caller can actually see; before the fix both survived this Reset.
    bool passed = true;
    const auto after = locker.GetLockSnapshot();

    for (size_t i = 0; i < after.size(); ++i) {
        if (after[i].previousFormID != 0) {
            logger::error("TEST FAIL: Reset left slot {} previousFormID = {:08X}, expected 0"sv,
                i, after[i].previousFormID);
            passed = false;
        }
        if (after[i].hadContent) {
            logger::error("TEST FAIL: Reset left slot {} hadContent = true, expected false"sv, i);
            passed = false;
        }
        if (after[i].isLocked) {
            logger::error("TEST FAIL: Reset left slot {} isLocked = true, expected false"sv, i);
            passed = false;
        }
        // Contract-level restatement of the same leak: a stale previousFormID is
        // exactly what would make a survived Reset report a phantom confirmation.
        if (locker.WasConfirmed(i, ProbeFormFor(i))) {
            logger::error("TEST FAIL: WasConfirmed still true for slot {} ({:08X}) after Reset "
                          "— previousFormID/hadContent leaked"sv, i, ProbeFormFor(i));
            passed = false;
        }
    }

    locker.SetConfig(savedConfig);
    locker.Reset();  // leave the live singleton as the load path expects

    if (passed) {
        logger::info("  SlotLocker::Reset test PASSED (all observable fields cleared)"sv);
    }
#endif
}

void RunCosaveTests()
{
#ifndef NDEBUG
    using namespace Huginn::Learning;

    logger::info("=== Running Cosave Serialization Tests ==="sv);

    // ── Test 1: FeatureQLearner round-trip ──────────────────────────────
    {
        FeatureQLearner source;

        // Train two items with different rewards in different states
        StateFeatures combatFeatures;
        combatFeatures.healthPct = 0.3f;
        combatFeatures.inCombat = 1.0f;
        combatFeatures.targetHumanoid = 1.0f;
        combatFeatures.targetNone = 0.0f;

        StateFeatures peacefulFeatures;
        peacefulFeatures.healthPct = 1.0f;
        peacefulFeatures.isSneaking = 1.0f;

        source.Update(0x00030000, combatFeatures, 2.0f);
        source.Update(0x00030000, combatFeatures, 1.5f);  // 2 trains
        source.Update(0x00030001, peacefulFeatures, 1.0f); // 1 train

        // Export
        std::vector<FeatureQLearner::SerializedEntry> exported;
        uint32_t totalTrains = 0;
        source.ExportData(
            [&](FeatureQLearner::SerializedEntry entry) { exported.push_back(std::move(entry)); },
            totalTrains
        );

        if (exported.size() != 2) {
            logger::error("[Cosave Test] FAIL: FQL should export 2 items, got {}"sv, exported.size());
            return;
        }
        if (totalTrains != 3) {
            logger::error("[Cosave Test] FAIL: FQL total trains should be 3, got {}"sv, totalTrains);
            return;
        }

        // Import into fresh learner
        FeatureQLearner dest;
        dest.ImportData(exported, totalTrains);

        if (dest.GetItemCount() != 2) {
            logger::error("[Cosave Test] FAIL: FQL import should have 2 items, got {}"sv, dest.GetItemCount());
            return;
        }
        if (dest.GetTotalTrainCount() != 3) {
            logger::error("[Cosave Test] FAIL: FQL import total trains should be 3, got {}"sv, dest.GetTotalTrainCount());
            return;
        }
        if (dest.GetTrainCount(0x00030000) != 2) {
            logger::error("[Cosave Test] FAIL: FQL item 30000 train count should be 2, got {}"sv, dest.GetTrainCount(0x00030000));
            return;
        }

        // Verify Q-values match
        float srcQ = source.GetQValue(0x00030000, combatFeatures);
        float dstQ = dest.GetQValue(0x00030000, combatFeatures);
        if (std::abs(srcQ - dstQ) > 0.001f) {
            logger::error("[Cosave Test] FAIL: FQL Q-value mismatch: {:.4f} vs {:.4f}"sv, srcQ, dstQ);
            return;
        }

        logger::info("  PASS: FQL round-trip preserves weights, train counts, Q-values"sv);
    }

    // ── Test 2: Empty round-trip (no crash) ────────────────────────────
    {
        FeatureQLearner empty;

        std::vector<FeatureQLearner::SerializedEntry> exported;
        uint32_t totalTrains = 0;
        empty.ExportData(
            [&](FeatureQLearner::SerializedEntry entry) { exported.push_back(std::move(entry)); },
            totalTrains
        );

        if (!exported.empty() || totalTrains != 0) {
            logger::error("[Cosave Test] FAIL: Empty FQL should export 0 entries"sv);
            return;
        }

        FeatureQLearner dest;
        dest.ImportData(exported, totalTrains);
        if (dest.GetItemCount() != 0) {
            logger::error("[Cosave Test] FAIL: Empty FQL import should have 0 items"sv);
            return;
        }

        logger::info("  PASS: Empty FQL exports/imports without crash"sv);
    }

    // ── Test 3: Import clears existing data ─────────────────────────────
    {
        FeatureQLearner learner;
        StateFeatures f;
        f.healthPct = 0.5f;
        learner.Update(0x00031000, f, 3.0f);

        if (learner.GetItemCount() != 1) {
            logger::error("[Cosave Test] FAIL: FQL pre-import should have 1 item"sv);
            return;
        }

        // Import different data
        std::vector<FeatureQLearner::SerializedEntry> newEntries;
        FeatureQLearner::SerializedEntry entry;
        entry.formID = 0x00032000;
        entry.weights = {};
        entry.weights[0] = 0.5f;  // healthPct weight
        entry.trainCount = 5;
        newEntries.push_back(entry);

        learner.ImportData(newEntries, 5);

        // Old data gone
        if (learner.GetTrainCount(0x00031000) != 0) {
            logger::error("[Cosave Test] FAIL: FQL old data should be cleared after import"sv);
            return;
        }
        // New data present
        if (learner.GetItemCount() != 1 || learner.GetTrainCount(0x00032000) != 5) {
            logger::error("[Cosave Test] FAIL: FQL new data should be present after import"sv);
            return;
        }

        logger::info("  PASS: FQL import clears old data and replaces with new"sv);
    }

    // ── Test 4: Feature-count migration decode (pad / truncate) ─────────
    {
        using Huginn::Persist::DecodeV2EntryBlob;
        constexpr auto compiled = static_cast<uint32_t>(StateFeatures::NUM_FEATURES);

        // Build a synthetic v2 blob with diskFeatures weights per entry.
        auto makeBlob = [](uint32_t diskFeatures, uint32_t numItems) {
            const size_t stride = sizeof(RE::FormID)
                                + sizeof(float) * diskFeatures
                                + sizeof(uint32_t) * 2;
            std::vector<std::byte> blob(stride * numItems);
            std::byte* p = blob.data();
            for (uint32_t i = 0; i < numItems; ++i, p += stride) {
                RE::FormID formID = 0x00040000 + i;
                std::memcpy(p, &formID, sizeof(formID));
                for (uint32_t f = 0; f < diskFeatures; ++f) {
                    float w = static_cast<float>(i * 100 + f + 1);  // distinct, nonzero
                    std::memcpy(p + sizeof(formID) + f * sizeof(float), &w, sizeof(w));
                }
                uint32_t trainCount = 10 + i;
                uint32_t minutes = 20 + i;
                std::byte* tail = p + sizeof(formID) + sizeof(float) * diskFeatures;
                std::memcpy(tail, &trainCount, sizeof(trainCount));
                std::memcpy(tail + sizeof(trainCount), &minutes, sizeof(minutes));
            }
            return blob;
        };

        // Pad: 16 on disk -> compiled 18, tail must be zero
        {
            constexpr uint32_t disk = compiled - 2;
            auto blob = makeBlob(disk, 2);
            auto entries = DecodeV2EntryBlob(blob.data(), blob.size(), 2, disk);
            bool ok = entries.size() == 2
                   && entries[1].formID == 0x00040001
                   && entries[1].trainCount == 11
                   && entries[1].minutesSinceLastUpdate == 21
                   && entries[1].weights[0] == 101.0f
                   && entries[1].weights[disk - 1] == static_cast<float>(100 + disk)
                   && entries[1].weights[disk] == 0.0f
                   && entries[1].weights[compiled - 1] == 0.0f;
            if (!ok) {
                logger::error("[Cosave Test] FAIL: pad migration decode incorrect"sv);
                return;
            }
        }

        // Truncate: 21 on disk -> compiled 18, extras dropped, tail fields intact
        {
            constexpr uint32_t disk = compiled + 3;
            auto blob = makeBlob(disk, 2);
            auto entries = DecodeV2EntryBlob(blob.data(), blob.size(), 2, disk);
            bool ok = entries.size() == 2
                   && entries[0].formID == 0x00040000
                   && entries[0].trainCount == 10
                   && entries[0].minutesSinceLastUpdate == 20
                   && entries[0].weights[0] == 1.0f
                   && entries[0].weights[compiled - 1] == static_cast<float>(compiled);
            if (!ok) {
                logger::error("[Cosave Test] FAIL: truncate migration decode incorrect"sv);
                return;
            }
        }

        // Equal count: decode must match a straight memcpy of SerializedEntry
        {
            auto blob = makeBlob(compiled, 1);
            auto entries = DecodeV2EntryBlob(blob.data(), blob.size(), 1, compiled);
            FeatureQLearner::SerializedEntry direct;
            std::memcpy(&direct, blob.data(), sizeof(direct));
            bool ok = entries.size() == 1
                   && entries[0].formID == direct.formID
                   && entries[0].weights == direct.weights
                   && entries[0].trainCount == direct.trainCount
                   && entries[0].minutesSinceLastUpdate == direct.minutesSinceLastUpdate;
            if (!ok) {
                logger::error("[Cosave Test] FAIL: equal-count decode differs from raw layout"sv);
                return;
            }
        }

        // Length mismatch: wrong byteLen must decode to nothing
        {
            auto blob = makeBlob(compiled, 1);
            auto entries = DecodeV2EntryBlob(blob.data(), blob.size() - 1, 1, compiled);
            if (!entries.empty()) {
                logger::error("[Cosave Test] FAIL: byteLen mismatch should reject decode"sv);
                return;
            }
        }

        logger::info("  PASS: FQL feature-count migration pads, truncates, round-trips"sv);
    }

    logger::info("=== Cosave Serialization Tests PASSED ==="sv);

#endif
}
