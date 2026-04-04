#include "CandidateGenerator.h"
#include <algorithm>

namespace Huginn::Candidate
{
    // =========================================================================
    // SINGLETON
    // =========================================================================

    CandidateGenerator& CandidateGenerator::GetSingleton()
    {
        static CandidateGenerator instance;
        return instance;
    }

    CandidateGenerator::CandidateGenerator()
    {
        // Reserve gather buffer for typical inventory sizes.
        // This buffer persists across calls (never moved-from), so the
        // capacity is retained and only grows if a player has a huge inventory.
        m_gatherBuffer.reserve(1024);
    }

    // =========================================================================
    // INITIALIZATION
    // =========================================================================

    void CandidateGenerator::Initialize(
        Spell::SpellRegistry& spellRegistry,
        Item::ItemRegistry& itemRegistry,
        Weapon::WeaponRegistry& weaponRegistry,
        Scroll::ScrollRegistry& scrollRegistry)
    {
        m_spellRegistry = &spellRegistry;
        m_itemRegistry = &itemRegistry;
        m_weaponRegistry = &weaponRegistry;
        m_scrollRegistry = &scrollRegistry;

        // Initialize cooldown durations from config
        m_cooldownMgr.SetDuration(SourceType::Spell, m_config.spellCooldown);
        m_cooldownMgr.SetDuration(SourceType::Potion, m_config.potionCooldown);
        m_cooldownMgr.SetDuration(SourceType::Scroll, m_config.scrollCooldown);
        m_cooldownMgr.SetDuration(SourceType::Weapon, m_config.weaponCooldown);
        m_cooldownMgr.SetDuration(SourceType::Ammo, m_config.ammoCooldown);
        m_cooldownMgr.SetDuration(SourceType::SoulGem, m_config.soulGemCooldown);
        m_cooldownMgr.SetDuration(SourceType::Food, m_config.foodCooldown);

        // Create filters
        m_filters = std::make_unique<CandidateFilters>(m_cooldownMgr, m_config);

        m_initialized = true;

        logger::info("CandidateGenerator initialized");
    }

    void CandidateGenerator::Reset()
    {
        m_cooldownMgr.Clear();
        m_stats.Reset();
        m_gatherBuffer.clear();
    }

    void CandidateGenerator::RefreshConfigFromGlobal() noexcept
    {
        // Copy global config to local snapshot.
        // Safe because writes only occur when the update loop is paused.
        m_config = g_candidateConfig;

        // Re-sync cooldown durations with new config values
        m_cooldownMgr.SetDuration(SourceType::Spell, m_config.spellCooldown);
        m_cooldownMgr.SetDuration(SourceType::Potion, m_config.potionCooldown);
        m_cooldownMgr.SetDuration(SourceType::Scroll, m_config.scrollCooldown);
        m_cooldownMgr.SetDuration(SourceType::Weapon, m_config.weaponCooldown);
        m_cooldownMgr.SetDuration(SourceType::Ammo, m_config.ammoCooldown);
        m_cooldownMgr.SetDuration(SourceType::SoulGem, m_config.soulGemCooldown);
        m_cooldownMgr.SetDuration(SourceType::Food, m_config.foodCooldown);
    }

    // =========================================================================
    // UPDATE
    // =========================================================================

    void CandidateGenerator::Update(float deltaSeconds)
    {
        m_cooldownMgr.Update(deltaSeconds);
    }

    void CandidateGenerator::StartCooldown(RE::FormID formID, SourceType type)
    {
        m_cooldownMgr.StartCooldown(formID, type);
    }

    // =========================================================================
    // MAIN GENERATION PIPELINE
    // =========================================================================

    std::vector<CandidateVariant> CandidateGenerator::GenerateCandidates(
        const State::WorldState& world,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        float currentMagicka,
        const State::HealthTrackingState& healthTracking,
        const State::MagickaTrackingState& magickaTracking,
        const State::StaminaTrackingState& staminaTracking)
    {
        if (!m_initialized) {
            logger::warn("CandidateGenerator::GenerateCandidates called before Initialize()");
            return {};
        }

        const auto startTime = std::chrono::high_resolution_clock::now();

        m_stats.Reset();
        m_gatherBuffer.clear();   // capacity is retained from previous calls

        // Step 1: Compute relevance tags from game state (including vital rate tracking)
        RelevanceTag contextTags = ComputeRelevanceTags(
            world, player, targets, healthTracking, magickaTracking, staminaTracking);

        // Step 2: Gather candidates from all registries into persistent buffer
        GatherSpellCandidates(m_gatherBuffer, player, contextTags);
        GatherPotionCandidates(m_gatherBuffer, player, contextTags);
        GatherScrollCandidates(m_gatherBuffer, player, contextTags);
        GatherWeaponCandidates(m_gatherBuffer, player, contextTags);
        GatherAmmoCandidates(m_gatherBuffer, player, contextTags);
        GatherSoulGemCandidates(m_gatherBuffer, player, contextTags);

        // Step 3: Filter gathered candidates into a local output vector.
        // Survivors are moved from m_gatherBuffer into output (one move per
        // candidate, no in-place erase_if chains).  m_gatherBuffer retains
        // its allocated capacity for the next call.
        std::vector<CandidateVariant> output;
        m_filters->ApplyAllFilters(m_gatherBuffer, output, player, currentMagicka, m_stats.filterStats);

        // Calculate generation time
        const auto endTime = std::chrono::high_resolution_clock::now();
        m_stats.generationTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        return output;  // NRVO — no move required
    }

    // =========================================================================
    // RELEVANCE TAG COMPUTATION
    // =========================================================================

    RelevanceTag CandidateGenerator::ComputeRelevanceTags(
        const State::WorldState& world,
        const State::PlayerActorState& player,
        const State::TargetCollection& targets,
        const State::HealthTrackingState& healthTracking,
        const State::MagickaTrackingState& magickaTracking,
        const State::StaminaTrackingState& staminaTracking) const
    {
        RelevanceTag tags = RelevanceTag::None;

        // Vital-based tags
        if (player.vitals.IsHealthLow()) {
            tags |= RelevanceTag::LowHealth;
        }
        if (player.vitals.IsHealthCritical()) {
            tags |= RelevanceTag::CriticalHealth;
        }
        if (player.vitals.IsMagickaLow()) {
            tags |= RelevanceTag::LowMagicka;
        }
        if (player.vitals.IsStaminaLow()) {
            tags |= RelevanceTag::LowStamina;
        }

        // Effect-based tags
        if (player.effects.isOnFire) {
            tags |= RelevanceTag::OnFire;
        }
        if (player.effects.isPoisoned) {
            tags |= RelevanceTag::Poisoned;
        }
        if (player.effects.isDiseased) {
            tags |= RelevanceTag::Diseased;
        }
        if (player.effects.isFrozen) {
            tags |= RelevanceTag::TakingFrost;
        }
        if (player.effects.isShocked) {
            tags |= RelevanceTag::TakingShock;
        }

        // Environment-based tags
        if (player.isUnderwater) {
            tags |= RelevanceTag::Underwater;
        }
        if (world.isLookingAtLock) {
            tags |= RelevanceTag::LookingAtLock;
        }
        if (world.isLookingAtOreVein) {
            tags |= RelevanceTag::LookingAtOre;
        }
        if (world.lightLevel < 0.3f) {
            tags |= RelevanceTag::InDarkness;
        }
        if (player.isFalling) {
            tags |= RelevanceTag::Falling;
        }

        // Workstation-based tags
        // Mapping: RE::TESFurniture::WorkBenchData::BenchType → RelevanceTag
        //   1=Forge, 2=Smithing       → AtForge    (Fortify Smithing)
        //   3=Enchanting, 4=EnchantExp → AtEnchanter (Fortify Enchanting)
        //   5=Alchemy, 6=AlchemyExp   → AtAlchemy   (Fortify Alchemy)
        //   7=Tanning, 8=Smelter, 9=Cooking → no matching fortify potion
        if (world.isLookingAtWorkstation) {
            switch (world.workstationType) {
                case 1: case 2: tags |= RelevanceTag::AtForge; break;
                case 3: case 4: tags |= RelevanceTag::AtEnchanter; break;
                case 5: case 6: tags |= RelevanceTag::AtAlchemy; break;
            }
        }

        // Combat-based tags
        if (player.isInCombat) {
            tags |= RelevanceTag::InCombat;
        }

        // Use TargetCollection query methods for enemy/ally counts
        const int enemyCount = targets.GetEnemyCount();
        if (enemyCount > 0) {
            tags |= RelevanceTag::EnemyNearby;
        }
        if (enemyCount >= 3) {
            tags |= RelevanceTag::MultipleEnemies;
        }
        if (targets.HasInjuredFollower()) {
            tags |= RelevanceTag::AllyInjured;
        }

        // Equipment-based tags
        if (player.IsWeaponChargeLow()) {
            tags |= RelevanceTag::WeaponLowCharge;
        }
        if (player.IsOutOfArrows() || player.IsOutOfBolts()) {
            tags |= RelevanceTag::NeedsAmmo;
        }
        if (!player.hasMeleeEquipped && !player.hasSpellEquipped && !player.hasBowEquipped) {
            tags |= RelevanceTag::NoWeapon;
        }

        // Target-type tags - check primary target if present
        if (targets.primary.has_value()) {
            if (targets.primary->targetType == State::TargetType::Undead) {
                tags |= RelevanceTag::TargetUndead;
            }
            if (targets.primary->targetType == State::TargetType::Dragon) {
                tags |= RelevanceTag::TargetDragon;
            }
        }

        // Stealth-based tags
        if (player.isSneaking) {
            tags |= RelevanceTag::Sneaking;
        }

        // Rate-based tags (v0.12.x - sub-threshold vital tracking)
        // These fire before vitals cross percentage thresholds, enabling early-warning recommendations
        if (healthTracking.damageRate > 2.0f) {
            tags |= RelevanceTag::HealthDeclining;
        }
        if (magickaTracking.IsMagickaDraining()) {  // usage.rate > regen.rate + 5
            tags |= RelevanceTag::MagickaDraining;
        }
        if (staminaTracking.IsStaminaDraining()) {  // usage.rate > regen.rate + 5
            tags |= RelevanceTag::StaminaDraining;
        }

        return tags;
    }

    // =========================================================================
    // SPELL GATHERING
    // =========================================================================

    void CandidateGenerator::GatherSpellCandidates(
        std::vector<CandidateVariant>& out,
        const State::PlayerActorState& player,
        RelevanceTag contextTags)
    {
        if (!m_spellRegistry || m_spellRegistry->IsLoading()) {
            return;
        }

        // OPTIMIZATION: Zero-copy iteration via ForEach visitor pattern
        size_t count = 0;
        auto* playerRef = RE::PlayerCharacter::GetSingleton();
        m_spellRegistry->ForEachSpell([&](const Spell::SpellData& spellData) {
            ++count;
            SpellCandidate candidate = SpellCandidate::FromSpellData(spellData);

            // Cache effective cost (perk/enchant-adjusted) to avoid form lookup in filter
            auto* form = RE::TESForm::LookupByID(candidate.formID);
            auto* spellItem = form ? form->As<RE::SpellItem>() : nullptr;
            candidate.effectiveCost = (spellItem && playerRef)
                ? spellItem->CalculateMagickaCost(playerRef)
                : static_cast<float>(candidate.baseCost);
            if (candidate.isConcentration && candidate.effectiveCost <= 0.0f) {
                candidate.effectiveCost = static_cast<float>(candidate.baseCost);
            }

            candidate.isEquipped = player.IsSpellEquipped(candidate.formID);
            candidate.relevanceTags = contextTags;

            out.push_back(std::move(candidate));
        });
        m_stats.spellsScanned = count;
    }

    // =========================================================================
    // POTION GATHERING
    // =========================================================================

    void CandidateGenerator::GatherPotionCandidates(
        std::vector<CandidateVariant>& out,
        const State::PlayerActorState& player,
        RelevanceTag contextTags)
    {
        if (!m_itemRegistry) {
            logger::warn("[CandidateGenerator] GatherPotionCandidates: m_itemRegistry is null");
            return;
        }
        if (m_itemRegistry->IsLoading()) {
            logger::debug("[CandidateGenerator] GatherPotionCandidates: registry is loading, skipping");
            return;
        }

        // OPTIMIZATION: Zero-copy iteration via ForEach visitor pattern
        size_t count = 0;
        m_itemRegistry->ForEachItem([&](const Item::InventoryItem& invItem) {
            // Skip soul gems (handled separately)
            if (invItem.data.type == Item::ItemType::SoulGem) {
                return;  // continue to next item
            }
            ++count;

            ItemCandidate candidate = ItemCandidate::FromInventoryItem(invItem);

            // Set relevance tags (used by filters)
            candidate.relevanceTags = contextTags;
            // Stage 1g: baseRelevance removed - now computed by ContextRuleEngine
            // Stage 1g: preference multipliers removed - can be added to ContextWeightSettings if needed

            out.push_back(std::move(candidate));
        });
        m_stats.potionsScanned = count;

        // Log item candidate count (transition-only)
        static size_t lastCount = SIZE_MAX;
        if (count != lastCount) {
            logger::info("[CandidateGenerator] GatherPotionCandidates: {} items scanned from registry", count);
            lastCount = count;
        }
    }

    // =========================================================================
    // WEAPON GATHERING
    // =========================================================================

    void CandidateGenerator::GatherWeaponCandidates(
        std::vector<CandidateVariant>& out,
        const State::PlayerActorState& player,
        RelevanceTag contextTags)
    {
        if (!m_weaponRegistry) {
            logger::warn("[CandidateGenerator] GatherWeaponCandidates: m_weaponRegistry is null");
            return;
        }
        if (m_weaponRegistry->IsLoading()) {
            logger::debug("[CandidateGenerator] GatherWeaponCandidates: registry is loading, skipping");
            return;
        }

        // OPTIMIZATION: Zero-copy iteration via ForEach visitor pattern
        size_t count = 0;
        size_t favoritedCount = 0;
        m_weaponRegistry->ForEachWeapon([&](const Weapon::InventoryWeapon& invWeapon) {
            ++count;
            if (invWeapon.isFavorited) ++favoritedCount;
            WeaponCandidate candidate = WeaponCandidate::FromInventoryWeapon(invWeapon);

            // Set relevance tags (used by filters)
            candidate.relevanceTags = contextTags;
            // Stage 1g: baseRelevance removed - now computed by ContextRuleEngine

            out.push_back(std::move(candidate));
        });
        m_stats.weaponsScanned = count;

        // Debug: Log weapon candidate generation (only when count changes)
        static size_t lastCount = 0;
        static size_t lastFav = 0;
        if (count != lastCount || favoritedCount != lastFav) {
            logger::info("[CandidateGenerator] GatherWeaponCandidates: {} weapons ({} favorited)",
                count, favoritedCount);
            lastCount = count;
            lastFav = favoritedCount;
        }
    }

    // =========================================================================
    // AMMO GATHERING
    // =========================================================================

    void CandidateGenerator::GatherAmmoCandidates(
        std::vector<CandidateVariant>& out,
        const State::PlayerActorState& player,
        RelevanceTag contextTags)
    {
        if (!m_weaponRegistry || m_weaponRegistry->IsLoading()) {
            return;
        }

        // OPTIMIZATION: Zero-copy iteration via ForEach visitor pattern
        size_t count = 0;
        m_weaponRegistry->ForEachAmmo([&](const Weapon::InventoryAmmo& invAmmo) {
            ++count;
            AmmoCandidate candidate = AmmoCandidate::FromInventoryAmmo(invAmmo);

            // Set relevance tags (used by filters)
            candidate.relevanceTags = contextTags;
            // Stage 1g: baseRelevance removed - now computed by ContextRuleEngine

            out.push_back(std::move(candidate));
        });
        m_stats.ammoScanned = count;
    }

    // =========================================================================
    // SCROLL GATHERING
    // =========================================================================

    void CandidateGenerator::GatherScrollCandidates(
        std::vector<CandidateVariant>& out,
        const State::PlayerActorState& player,
        RelevanceTag contextTags)
    {
        if (!m_scrollRegistry || m_scrollRegistry->IsLoading()) {
            return;
        }

        // OPTIMIZATION: Zero-copy iteration via ForEach visitor pattern
        size_t count = 0;
        m_scrollRegistry->ForEachScroll([&](const Scroll::InventoryScroll& invScroll) {
            ++count;
            ScrollCandidate candidate = ScrollCandidate::FromInventoryScroll(invScroll);

            // Set relevance tags (used by filters)
            candidate.relevanceTags = contextTags;
            // Stage 1g: baseRelevance removed - now computed by ContextRuleEngine
            // Stage 1g: scroll preference multiplier removed - can be added to ContextWeightSettings if needed

            out.push_back(std::move(candidate));
        });
        m_stats.scrollsScanned = count;
    }

    // =========================================================================
    // SOUL GEM GATHERING
    // =========================================================================

    void CandidateGenerator::GatherSoulGemCandidates(
        std::vector<CandidateVariant>& out,
        const State::PlayerActorState& player,
        RelevanceTag contextTags)
    {
        if (!m_itemRegistry) {
            logger::warn("[CandidateGenerator] GatherSoulGemCandidates: m_itemRegistry is null");
            return;
        }
        if (m_itemRegistry->IsLoading()) {
            logger::debug("[CandidateGenerator] GatherSoulGemCandidates: registry is loading, skipping");
            return;
        }

        if (!m_config.enableSoulGemRecharge) {
            return;
        }

        // Get the single best soul gem (largest capacity) — zero allocation.
        // Soul gems are informational — they tell the player their weapon needs
        // recharging. Only one recommendation is needed.
        const auto* bestGem = m_itemRegistry->GetBestSoulGem();
        m_stats.soulGemsScanned = bestGem ? 1 : 0;

        if (bestGem && bestGem->count > 0) {
            ItemCandidate candidate = ItemCandidate::FromInventoryItem(*bestGem);
            candidate.sourceType = SourceType::SoulGem;
            candidate.relevanceTags = contextTags;
            out.push_back(std::move(candidate));
        }
    }

    // =========================================================================
    // RELEVANCE SCORING - REMOVED (Stage 1g)
    // =========================================================================
    // All relevance scoring has been moved to ContextRuleEngine.
    // The old Compute*Relevance() methods have been removed.
    // baseRelevance is no longer set in Gather methods - it's now computed
    // by UtilityScorer::GetContextWeight() using ContextRuleEngine weights.

    // =========================================================================
    // DEBUG LOGGING
    // =========================================================================

    void CandidateGenerator::LogCandidates(const std::vector<CandidateVariant>& candidates) const
    {
        logger::info("=== CandidateGenerator: {} candidates ===", candidates.size());
        logger::info("NOTE: baseRelevance is deprecated (Stage 1g) - relevance now computed by ContextRuleEngine");

        for (size_t i = 0; i < candidates.size() && i < 20; ++i) {
            const auto& base = GetBase(candidates[i]);
            logger::info("  [{}] {} ({})",
                i,
                base.name,
                SourceTypeToString(base.sourceType));
        }

        if (candidates.size() > 20) {
            logger::info("  ... and {} more", candidates.size() - 20);
        }

        logger::info("=== Generation Stats ===");
        logger::info("  Scanned: {} spells, {} potions, {} weapons, {} scrolls",
            m_stats.spellsScanned, m_stats.potionsScanned,
            m_stats.weaponsScanned, m_stats.scrollsScanned);
        logger::info("  Filtered: {} afford, {} equipped, {} cooldown, {} buff, {} dupe",
            m_stats.filterStats.filteredByAffordability,
            m_stats.filterStats.filteredByEquipped,
            m_stats.filterStats.filteredByCooldown,
            m_stats.filterStats.filteredByActiveBuff,
            m_stats.filterStats.filteredByDuplication);
        logger::info("  Time: {:.2f}ms", m_stats.generationTimeMs);
    }

}  // namespace Huginn::Candidate
