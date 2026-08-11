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

    bool CandidateGenerator::Update(float deltaSeconds)
    {
        return m_cooldownMgr.Update(deltaSeconds);
    }

    void CandidateGenerator::StartCooldown(RE::FormID formID, SourceType type)
    {
        m_cooldownMgr.StartCooldown(formID, type);
    }

    // =========================================================================
    // MAIN GENERATION PIPELINE
    // =========================================================================

    std::vector<CandidateVariant> CandidateGenerator::GenerateCandidates(
        const State::PlayerActorState& player,
        float currentMagicka)
    {
        if (!m_initialized) {
            logger::warn("CandidateGenerator::GenerateCandidates called before Initialize()");
            return {};
        }

        const auto startTime = std::chrono::high_resolution_clock::now();

        m_stats.Reset();
        m_gatherBuffer.clear();   // capacity is retained from previous calls

        // Gather candidates from all registries into the persistent buffer.
        // No relevance metadata is stamped here: the per-tick display reason is
        // derived by the pipeline from the scorer's context weights (#10).
        GatherSpellCandidates(m_gatherBuffer, player);
        GatherPotionCandidates(m_gatherBuffer, player);
        GatherScrollCandidates(m_gatherBuffer, player);
        GatherWeaponCandidates(m_gatherBuffer, player);
        GatherAmmoCandidates(m_gatherBuffer, player);
        GatherSoulGemCandidates(m_gatherBuffer, player);

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
    // SPELL GATHERING
    // =========================================================================

    void CandidateGenerator::GatherSpellCandidates(
        std::vector<CandidateVariant>& out,
        const State::PlayerActorState& player)
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
            out.push_back(std::move(candidate));
        });
        m_stats.spellsScanned = count;
    }

    // =========================================================================
    // POTION GATHERING
    // =========================================================================

    void CandidateGenerator::GatherPotionCandidates(
        std::vector<CandidateVariant>& out,
        const State::PlayerActorState& player)
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
        const State::PlayerActorState& player)
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
        const State::PlayerActorState& player)
    {
        if (!m_weaponRegistry || m_weaponRegistry->IsLoading()) {
            return;
        }

        // OPTIMIZATION: Zero-copy iteration via ForEach visitor pattern
        size_t count = 0;
        m_weaponRegistry->ForEachAmmo([&](const Weapon::InventoryAmmo& invAmmo) {
            ++count;
            AmmoCandidate candidate = AmmoCandidate::FromInventoryAmmo(invAmmo);

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
        const State::PlayerActorState& player)
    {
        if (!m_scrollRegistry || m_scrollRegistry->IsLoading()) {
            return;
        }

        // OPTIMIZATION: Zero-copy iteration via ForEach visitor pattern
        size_t count = 0;
        m_scrollRegistry->ForEachScroll([&](const Scroll::InventoryScroll& invScroll) {
            ++count;
            ScrollCandidate candidate = ScrollCandidate::FromInventoryScroll(invScroll);

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
        const State::PlayerActorState& player)
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

        // Every FILLED gem, not the largest one.
        //
        // This used to call GetBestSoulGem() and emit exactly one candidate,
        // picked by capacity. That is a hard-coded preference sitting in front
        // of the one component whose job is preferences: a Petty gem was never
        // a candidate, so it could never be scored, equipped from a slot, or
        // rewarded, and FeatureQLearner could not discover that a player tops up
        // with small gems and saves the Grands. Every other candidate type is
        // gathered wholesale and ranked; gems were the exception.
        //
        // The urgent path is untouched and still decisive —
        // OverrideManager::FindSoulGem() makes its own single pick, because when
        // a weapon dies mid-fight the answer is "the biggest one, now", and
        // overcharging is the right trade there.
        //
        // Empty gems stay out: they cannot recharge anything, so surfacing one
        // is offering an action that does nothing.
        //
        // Cost is small — gems stack, so this is one candidate per filled TYPE
        // (Petty/Lesser/Common/…), not per gem.
        size_t gathered = 0;
        m_itemRegistry->ForEachItem([&](const Item::InventoryItem& item) {
            if (item.data.type != Item::ItemType::SoulGem) return;
            if (item.count <= 0 || !item.data.isFilled) return;

            ItemCandidate candidate = ItemCandidate::FromInventoryItem(item);
            candidate.sourceType = SourceType::SoulGem;
            out.push_back(std::move(candidate));
            ++gathered;
        });
        m_stats.soulGemsScanned = gathered;
    }

    // =========================================================================
    // RELEVANCE SCORING - REMOVED (Stage 1g)
    // =========================================================================
    // All relevance scoring has been moved to ContextRuleEngine.
    // The old Compute*Relevance() methods have been removed.
    // baseRelevance is no longer set in Gather methods - it's now computed
    // by Context::WeightForCandidate() using ContextRuleEngine weights.

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
