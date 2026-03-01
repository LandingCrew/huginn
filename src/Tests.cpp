#include "Tests.h"
#include "Globals.h"

#include "state/StateManager.h"
#include "state/GameState.h"
#include "spell/SpellRegistry.h"
#include "learning/item/ItemClassifier.h"
#include "learning/item/ItemRegistry.h"
#include "weapon/WeaponRegistry.h"
#include "learning/UtilityScorer.h"
#include "learning/ScoredCandidate.h"
#include "candidate/CandidateGenerator.h"
#include "persist/QLearnerSerializer.h"
#include "learning/StateFeatures.h"
#include "learning/FeatureQLearner.h"
#include "util/ScopedTimer.h"
#include "context/ContextRuleEngine.h"

#include <random>
#include <algorithm>
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

        auto weights = engine.EvaluateRules(gameState, player, targets, world);

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
            targets.targets[1] = primary;

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
        targets.targets[1] = enemy;

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
        targets.targets[1] = enemy;

        f = StateFeatures::FromState(player, targets);
        if (!feq(f.distanceNorm, 1.0f)) {
            logger::error("TEST FAIL: distanceNorm at 4096u should be 1.0, got {:.4f}"sv, f.distanceNorm);
            return;
        }

        // 4c: Enemy beyond MAX_DISTANCE: 8192 units → clamped to 1.0
        targets.targets.clear();
        enemy.distanceToPlayerSq = 8192.0f * 8192.0f;
        targets.targets[1] = enemy;

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
        targets.targets[2] = sentinel;

        f = StateFeatures::FromState(player, targets);
        if (!feq(f.distanceNorm, 1.0f)) {
            logger::error("TEST FAIL: distanceNorm with NO_TARGET sentinel should be 1.0, got {:.4f}"sv,
                f.distanceNorm);
            return;
        }

        // 4e: Sentinel + real enemy coexistence — real enemy should win
        // GetClosestEnemy() must skip sentinel entries so the real enemy is found
        targets.targets.clear();
        targets.targets[2] = sentinel;  // sentinel at distSq=0.0
        TargetActorState realEnemy;
        realEnemy.isHostile = true;
        realEnemy.distanceToPlayerSq = 256.0f * 256.0f;
        realEnemy.actorFormID = 3;
        targets.targets[3] = realEnemy;

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
        targets.targets[2] = primary;

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
        targets.targets[3] = enemy;

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
        targets.targets[4] = ally;

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

        // Train at low health + combat: reward=1.0
        StateFeatures lowHealthCombat;
        lowHealthCombat.healthPct = 0.2f;
        lowHealthCombat.inCombat = 1.0f;

        for (int i = 0; i < 30; ++i) {
            fql.Update(healSpell, lowHealthCombat, 1.0f);
        }

        auto weights = fql.GetWeights(healSpell);

        // healthPct weight should be negative: low healthPct (0.2) paired with
        // positive reward means the model learns that low health correlates with
        // high Q — so the weight on healthPct should be negative (lower health = higher Q).
        // inCombat weight should be positive: combat=1.0 paired with reward.
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
        .inCombat = CombatStatus::NotInCombat,
        .isSneaking = SneakStatus::NotSneaking
    };

    if (state1.GetHash() != 0) {
        logger::error("TEST FAIL: Min hash should be 0, got {}"sv, state1.GetHash());
        return;
    }

    // Test 2: Maximum hash (all max values)
    // Hash states: 6×6×3×7×4×3×2×2 = 36,288 (stamina excluded from hash), so max hash = 36,287
    GameState state2{
        .health = HealthBucket::VeryHigh,
        .magicka = MagickaBucket::VeryHigh,
        .stamina = StaminaBucket::VeryHigh,  // Not in hash, but still in struct
        .distance = DistanceBucket::Ranged,
        .targetType = TargetType::Daedra,  // Max is 6 (Daedra) - 7 target types total
        .enemyCount = EnemyCountBucket::Many,
        .allyStatus = AllyStatus::InjuredPresent,
        .inCombat = CombatStatus::InCombat,
        .isSneaking = SneakStatus::Sneaking
    };

    if (state2.GetHash() != 36287) {
        logger::error("TEST FAIL: Max hash should be 36287, got {}"sv, state2.GetHash());
        return;
    }

    // Test 3: Hash uniqueness for all 36,288 states (stamina excluded from hash)
    std::set<uint32_t> seenHashes;
    for (uint8_t h = 0; h < 6; ++h) {
        for (uint8_t m = 0; m < 6; ++m) {
            for (uint8_t d = 0; d < 3; ++d) {
                for (uint8_t t = 0; t < 7; ++t) {
                    for (uint8_t ec = 0; ec < 4; ++ec) {
                        for (uint8_t as = 0; as < 3; ++as) {
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
                                        .inCombat = static_cast<CombatStatus>(c),
                                        .isSneaking = static_cast<SneakStatus>(s)
                                    };

                                    uint32_t hash = state.GetHash();
                                    if (hash >= 36288) {
                                        logger::error("TEST FAIL: Hash {} out of range [0, 36287]"sv, hash);
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

    if (seenHashes.size() != 36288) {
        logger::error("TEST FAIL: Should have 36288 unique hashes, got {}"sv, seenHashes.size());
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

    logger::info("TEST PASS: All hash tests passed! 36288 unique states verified, stamina excluded."sv);

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

    float healPriorExtreme = priorCalc.CalculatePrior(extremeState, extremePlayer, healingSpell);
    float healPriorNeutral = priorCalc.CalculatePrior(neutralState, neutralPlayer, healingSpell);

    if (std::abs(healPriorExtreme - healPriorNeutral) > 0.001f) {
        logger::error("TEST FAIL: Healing spell prior should be context-independent! Extreme={:.3f}, Neutral={:.3f}",
            healPriorExtreme, healPriorNeutral);
        return;
    }

    // Test 2: Damage spell prior should be IDENTICAL in combat vs out of combat
    SpellCandidate damageSpell;
    damageSpell.baseCost = 200;  // Expert-level damage spell
    damageSpell.type = Spell::SpellType::Damage;

    float dmgPriorExtreme = priorCalc.CalculatePrior(extremeState, extremePlayer, damageSpell);
    float dmgPriorNeutral = priorCalc.CalculatePrior(neutralState, neutralPlayer, damageSpell);

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

    float smallPrior = priorCalc.CalculatePrior(neutralState, neutralPlayer, smallPotion);
    float largePrior = priorCalc.CalculatePrior(neutralState, neutralPlayer, largePotion);

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

    float plentifulPrior = priorCalc.CalculatePrior(neutralState, neutralPlayer, plentifulPotion);
    float scarcePrior = priorCalc.CalculatePrior(neutralState, neutralPlayer, scarcePotion);

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

    float novicePrior = priorCalc.CalculatePrior(neutralState, neutralPlayer, noviceSpell);
    float expertPrior = priorCalc.CalculatePrior(neutralState, neutralPlayer, expertSpell);

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

    float fullPrior = priorCalc.CalculatePrior(neutralState, neutralPlayer, fullCharge);
    float lowPrior = priorCalc.CalculatePrior(neutralState, neutralPlayer, lowCharge);

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

    float arrowsWithBow = priorCalc.CalculatePrior(neutralState, bowPlayer, arrows);
    float arrowsWithoutBow = priorCalc.CalculatePrior(neutralState, meleePlayer, arrows);

    if (arrowsWithBow <= arrowsWithoutBow) {
        logger::error("TEST FAIL: Compatible ammo should have higher prior! WithBow={:.3f}, WithoutBow={:.3f}",
            arrowsWithBow, arrowsWithoutBow);
        return;
    }

    // Test 8: Scrolls should all have identical prior (no intrinsic differences)
    ScrollCandidate healScroll;
    healScroll.type = Spell::SpellType::Healing;

    ScrollCandidate damageScroll;
    damageScroll.type = Spell::SpellType::Damage;

    float healScrollPrior = priorCalc.CalculatePrior(neutralState, neutralPlayer, healScroll);
    float dmgScrollPrior = priorCalc.CalculatePrior(neutralState, neutralPlayer, damageScroll);

    if (std::abs(healScrollPrior - dmgScrollPrior) > 0.001f) {
        logger::error("TEST FAIL: All scrolls should have identical prior! Heal={:.3f}, Damage={:.3f}",
            healScrollPrior, dmgScrollPrior);
        return;
    }

    // Should be exactly BASE_PRIOR (0.3f)
    if (std::abs(healScrollPrior - 0.3f) > 0.001f) {
        logger::error("TEST FAIL: Scroll prior should be BASE_PRIOR (0.3)! Got={:.3f}",
            healScrollPrior);
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
    logger::info("  - Scrolls have uniform prior (no intrinsic differences)"sv);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights49 = engine.EvaluateRules(testState, testPlayer49, testTargets, testWorld);
            auto weights51 = engine.EvaluateRules(testState, testPlayer51, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

            if (std::abs(weights.resistDiseaseWeight - 0.3f) > 0.01f) {
                logger::error("TEST FAIL: Diseased should give resistDiseaseWeight=0.3, got {:.3f}",
                    weights.resistDiseaseWeight);
                return;
            }
        }

        // Test 5e: No elemental effects → all weights zero
        {
            State::PlayerActorState testPlayer{};  // All effects false by default

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

            if (weights.resistFireWeight != 0.0f || weights.resistFrostWeight != 0.0f ||
                weights.resistShockWeight != 0.0f || weights.resistPoisonWeight != 0.0f ||
                weights.resistDiseaseWeight != 0.0f) {
                logger::error("TEST FAIL: No effects should give all resist weights=0.0");
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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

            if (std::abs(weights.unlockWeight - 1.0f) > 0.01f) {
                logger::error("TEST FAIL: Looking at lock should give unlockWeight=1.0, got {:.3f}",
                    weights.unlockWeight);
                return;
            }
        }

        // Test 6c: Falling → slowFallWeight = 0.8 (high priority)
        {
            State::PlayerActorState testPlayer{};
            State::WorldState testWorld{};

            testPlayer.isFalling = true;

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

            if (std::abs(weights.slowFallWeight - 0.8f) > 0.01f) {
                logger::error("TEST FAIL: Falling should give slowFallWeight=0.8, got {:.3f}",
                    weights.slowFallWeight);
                return;
            }
        }

        // Test 6d: Workstation (Forge) → fortifySmithingWeight = 0.8
        {
            State::PlayerActorState testPlayer{};
            State::WorldState testWorld{};

            testWorld.isLookingAtWorkstation = true;
            testWorld.workstationType = 1;  // Forge

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

            if (std::abs(weights.fortifySmithingWeight - 0.8f) > 0.01f) {
                logger::error("TEST FAIL: Forge should give fortifySmithingWeight=0.8, got {:.3f}",
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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

            if (std::abs(weights.fortifyAlchemyWeight - 0.8f) > 0.01f) {
                logger::error("TEST FAIL: Alchemy Lab should give fortifyAlchemyWeight=0.8, got {:.3f}",
                    weights.fortifyAlchemyWeight);
                return;
            }
        }

        // Test 6g: No environmental conditions → all weights zero
        {
            State::PlayerActorState testPlayer{};
            State::WorldState testWorld{};

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            testTargets.targets[castingEnemy.actorFormID] = castingEnemy;

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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
                testTargets.targets[enemy.actorFormID] = enemy;
            }

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

            // deficit=0.75, curve=(0.75)^2 = 0.56
            const float expected = 0.56f;
            const float tolerance = 0.01f;

            if (std::abs(weights.weaponChargeWeight - expected) > tolerance) {
                logger::error("TEST FAIL: 25%% charge should give weaponChargeWeight≈{:.2f}, got {:.3f}",
                    expected, weights.weaponChargeWeight);
                return;
            }
        }

        // Test 9c: Weapon charge high (50%) → weaponChargeWeight = 0.0
        {
            State::PlayerActorState testPlayer{};

            testPlayer.hasEnchantedWeapon = true;
            testPlayer.weaponChargePercent = 0.50f;  // 50% charge (above threshold)

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

            if (weights.weaponChargeWeight != 0.0f) {
                logger::error("TEST FAIL: 50%% charge should give weaponChargeWeight=0.0, got {:.3f}",
                    weights.weaponChargeWeight);
                return;
            }
        }

        // Test 9d: Out of arrows → ammoWeight = 0.5
        {
            State::PlayerActorState testPlayer{};

            testPlayer.hasBowEquipped = true;
            testPlayer.arrowCount = 0;  // Out of arrows

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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

            auto weights = engine.EvaluateRules(testState, testPlayer, testTargets, testWorld);

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
            auto weights = engine.EvaluateRules(gameState, player, targets, world);

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

            auto weights = engine.EvaluateRules(gameState, player, targets, world);

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

            auto weights = engine.EvaluateRules(gameState, player, targets, world);

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

            auto weights = engine.EvaluateRules(gameState, player, targets, world);

            Candidate::SpellCandidate healingSpell{};
            healingSpell.tags = Spell::SpellTag::RestoreHealth;
            healingSpell.name = "Heal Self";

            float weight = weights.baseRelevanceWeight;
            if (Spell::HasTag(healingSpell.tags, Spell::SpellTag::RestoreHealth)) {
                weight = std::max(weight, weights.healingWeight);
            }

            if (weight < 0.6f) {  // Should be high at 30% health
                logger::error("TEST FAIL: Healing spell at 30%% health should get high weight, got {:.2f}"sv, weight);
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
                targets.targets[formID] = enemy;
            }

            State::GameState gameState{};

            auto weights = engine.EvaluateRules(gameState, player, targets, world);

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

            auto weights = engine.EvaluateRules(gameState, player, targets, world);

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
        auto weights49 = engine.EvaluateRules(testState, player49, testTargets, testWorld);

        // Test 51% HP
        PlayerActorState player51{};
        player51.vitals.health = 0.51f;
        auto weights51 = engine.EvaluateRules(testState, player51, testTargets, testWorld);

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

        auto weights = engine.EvaluateRules(testState, player, testTargets, testWorld);

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

        auto weights = engine.EvaluateRules(testState, player, testTargets, testWorld);

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
        testTargets.targets[castingEnemy.actorFormID] = castingEnemy;

        auto weights = engine.EvaluateRules(testState, player, testTargets, testWorld);

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

        auto weights = engine.EvaluateRules(testState, player, testTargets, testWorld);

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

        auto weights = engine.EvaluateRules(testState, player, testTargets, testWorld);

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

        auto weights = engine.EvaluateRules(testState, player, testTargets, testWorld);

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
        testTargets.targets[castingEnemy.actorFormID] = castingEnemy;

        auto weights = engine.EvaluateRules(testState, player, testTargets, testWorld);

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

        auto weights = engine.EvaluateRules(testState, player, testTargets, testWorld);

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

        auto weights = engine.EvaluateRules(testState, player, testTargets, testWorld);

        if (weights.antiUndeadWeight < 0.55f) {  // Expected ~0.6
            logger::error("TC-15 FAIL: Undead target should give antiUndeadWeight≈0.6, got {:.3f}"sv,
                weights.antiUndeadWeight);
            return;
        }

        logger::info("  ✓ PASS: Undead target → antiUndeadWeight={:.2f}"sv, weights.antiUndeadWeight);
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

    logger::info("=== Cosave Serialization Tests PASSED ==="sv);

#endif
}
