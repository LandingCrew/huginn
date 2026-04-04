#include "CandidateFilters.h"
#include "spell/SpellData.h"  // For Spell::HasTag (ScrollTag is alias)
#include <algorithm>

namespace Huginn::Candidate
{
    // Epsilon for "nearly full" vital comparisons (avoids floating-point edge cases)
    static constexpr float FULL_VITAL_EPSILON = 0.01f;
    CandidateFilters::CandidateFilters(CooldownManager& cooldownMgr, const CandidateConfig& config)
        : m_cooldownMgr(cooldownMgr)
        , m_config(config)
    {
    }

    // =========================================================================
    // CONSOLIDATED VISITOR — single std::visit dispatch per candidate
    // Runs affordability, equipped, full vitals, and active buff checks.
    // Returns early on first failure (cheapest filters first).
    // =========================================================================
    CandidateFilters::FilterResult CandidateFilters::RunVisitorFilters(
        const CandidateVariant& candidate,
        const State::PlayerActorState& player,
        float currentMagicka) const
    {
        const auto policy = m_config.uncastableSpellPolicy;
        const bool checkBuffs = m_config.filterActiveBuffs;
        const bool checkResists = m_config.filterRedundantResists;

        return std::visit([&](const auto& c) -> FilterResult {
            using T = std::decay_t<decltype(c)>;

            // =================================================================
            // SPELL
            // =================================================================
            if constexpr (std::is_same_v<T, SpellCandidate>) {
                // 1. Affordability
                if (currentMagicka < c.effectiveCost) {
                    logger::trace("Unaffordable spell {} (policy={}): {} [{:X}] cost={:.0f} magicka={:.0f}",
                        policy == UncastableSpellPolicy::Disallow ? "filtered" : "kept",
                        ToString(policy), c.name, c.formID, c.effectiveCost, currentMagicka);
                    if (policy == UncastableSpellPolicy::Disallow)
                        return FilterResult::Affordability;
                }

                // 2. Equipped
                if (player.IsSpellEquipped(c.formID))
                    return FilterResult::Equipped;

                // 3. Full Vitals
                if (m_config.filterHealingWhenFull &&
                    Spell::HasTag(c.tags, Spell::SpellTag::RestoreHealth) &&
                    player.vitals.health >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON)
                    return FilterResult::FullVitals;
                if (m_config.filterMagickaWhenFull &&
                    Spell::HasTag(c.tags, Spell::SpellTag::RestoreMagicka) &&
                    player.vitals.magicka >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON)
                    return FilterResult::FullVitals;

                // 4. Active Buff
                if (checkBuffs) {
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Invisibility) && player.buffs.isInvisible)
                        return FilterResult::ActiveBuff;
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Muffle) && player.buffs.hasMuffle)
                        return FilterResult::ActiveBuff;
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Armor) && player.buffs.hasArmorBuff)
                        return FilterResult::ActiveBuff;
                    if (c.type == Spell::SpellType::Summon && player.buffs.hasActiveSummon)
                        return FilterResult::ActiveBuff;
                }
                if (checkResists && IsResistSpellRedundant(c, player.resistances))
                    return FilterResult::ActiveBuff;

                return FilterResult::Passed;
            }
            // =================================================================
            // ITEM (Potion, Food, SoulGem)
            // =================================================================
            else if constexpr (std::is_same_v<T, ItemCandidate>) {
                // 1. Affordability (count > 0)
                if (c.count <= 0)
                    return FilterResult::Affordability;

                // 3. Full Vitals (skip for food/alcohol)
                if (c.type != Item::ItemType::Food && c.type != Item::ItemType::Alcohol) {
                    if (m_config.filterHealingWhenFull &&
                        Item::HasTag(c.tags, Item::ItemTag::RestoreHealth) &&
                        player.vitals.health >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON)
                        return FilterResult::FullVitals;
                    if (m_config.filterMagickaWhenFull &&
                        Item::HasTag(c.tags, Item::ItemTag::RestoreMagicka) &&
                        player.vitals.magicka >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON)
                        return FilterResult::FullVitals;
                    if (m_config.filterStaminaWhenFull &&
                        Item::HasTag(c.tags, Item::ItemTag::RestoreStamina) &&
                        player.vitals.stamina >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON)
                        return FilterResult::FullVitals;
                }

                // 4. Active Buff
                if (checkResists && IsResistPotionRedundant(c, player.resistances))
                    return FilterResult::ActiveBuff;
                if (checkBuffs) {
                    if (Item::HasTag(c.tags, Item::ItemTag::Waterbreathing) && player.buffs.hasWaterBreathing)
                        return FilterResult::ActiveBuff;
                    if (Item::HasTag(c.tags, Item::ItemTag::Invisibility) && player.buffs.isInvisible)
                        return FilterResult::ActiveBuff;
                }

                return FilterResult::Passed;
            }
            // =================================================================
            // SCROLL
            // =================================================================
            else if constexpr (std::is_same_v<T, ScrollCandidate>) {
                // 1. Affordability (count > 0)
                if (c.count <= 0)
                    return FilterResult::Affordability;

                // 3. Full Vitals
                if (m_config.filterHealingWhenFull &&
                    Spell::HasTag(c.tags, Spell::SpellTag::RestoreHealth) &&
                    player.vitals.health >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON)
                    return FilterResult::FullVitals;

                // 4. Active Buff
                if (checkBuffs) {
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Invisibility) && player.buffs.isInvisible)
                        return FilterResult::ActiveBuff;
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Muffle) && player.buffs.hasMuffle)
                        return FilterResult::ActiveBuff;
                }

                return FilterResult::Passed;
            }
            // =================================================================
            // AMMO
            // =================================================================
            else if constexpr (std::is_same_v<T, AmmoCandidate>) {
                if (c.count <= 0) return FilterResult::Affordability;
                if (c.isEquipped) return FilterResult::Equipped;
                return FilterResult::Passed;
            }
            // =================================================================
            // WEAPON
            // =================================================================
            else if constexpr (std::is_same_v<T, WeaponCandidate>) {
                // Weapons pass all visitor filters (equipped-skip is per-slot in SlotAllocator)
                return FilterResult::Passed;
            }
            else {
                return FilterResult::Passed;
            }
        }, candidate);
    }

    // Keep individual filters as public API for external callers
    bool CandidateFilters::PassesAffordabilityFilter(
        const CandidateVariant& candidate, float currentMagicka) const
    {
        // Simplified — delegates to consolidated visitor for spell logic,
        // but external callers may use this standalone
        return std::visit([currentMagicka, policy = m_config.uncastableSpellPolicy](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, SpellCandidate>) {
                if (currentMagicka >= c.effectiveCost) return true;
                return policy != UncastableSpellPolicy::Disallow;
            }
            else if constexpr (std::is_same_v<T, ItemCandidate>) { return c.count > 0; }
            else if constexpr (std::is_same_v<T, ScrollCandidate>) { return c.count > 0; }
            else if constexpr (std::is_same_v<T, AmmoCandidate>) { return c.count > 0; }
            else { return true; }
        }, candidate);
    }

    bool CandidateFilters::PassesEquippedFilter(
        const CandidateVariant& candidate, const State::PlayerActorState& player) const
    {
        return std::visit([&player](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, SpellCandidate>) { return !player.IsSpellEquipped(c.formID); }
            else if constexpr (std::is_same_v<T, AmmoCandidate>) { return !c.isEquipped; }
            else { return true; }
        }, candidate);
    }

    bool CandidateFilters::PassesCooldownFilter(const CandidateVariant& candidate) const
    {
        const auto& base = GetBase(candidate);
        return !m_cooldownMgr.IsOnCooldown(base.formID, base.sourceType);
    }

    // NOTE: These standalone shims delegate to the consolidated RunVisitorFilters,
    // which returns the *first* failing filter (early-exit). If a candidate fails an
    // earlier filter (e.g. Equipped), the specific check here is never reached and the
    // shim returns true. This is acceptable: such candidates would be removed by the
    // earlier filter anyway in the main ApplyAllFilters pipeline.
    bool CandidateFilters::PassesActiveBuffFilter(
        const CandidateVariant& candidate, const State::PlayerActorState& player) const
    {
        if (!m_config.filterActiveBuffs && !m_config.filterRedundantResists) return true;
        auto result = RunVisitorFilters(candidate, player, 999999.0f);
        return result != FilterResult::ActiveBuff;
    }

    bool CandidateFilters::PassesFullVitalsFilter(
        const CandidateVariant& candidate, const State::PlayerActorState& player) const
    {
        auto result = RunVisitorFilters(candidate, player, 999999.0f);
        return result != FilterResult::FullVitals;
    }

    // =========================================================================
    // BATCH FILTER APPLICATION
    // =========================================================================
    void CandidateFilters::ApplyAllFilters(
        std::vector<CandidateVariant>& gatherBuffer,
        std::vector<CandidateVariant>& output,
        const State::PlayerActorState& player,
        float currentMagicka,
        FilterStats& stats)
    {
        stats.Reset();
        stats.inputCount = gatherBuffer.size();

        // Safety: Validate vector integrity before filtering.
        // External heap corruption (e.g., from engine bugs on BSJobs threads) can
        // stomp on vector internals. Bail out early if the vector looks corrupted.
        if (gatherBuffer.size() > gatherBuffer.capacity() || gatherBuffer.capacity() > 100000) {
            logger::error("[CandidateFilters] Vector integrity check failed: size={}, capacity={} - skipping filters",
                gatherBuffer.size(), gatherBuffer.capacity());
            gatherBuffer.clear();
            stats.outputCount = 0;
            return;
        }

        // Prepare output vector and dedup set
        output.clear();
        output.reserve(gatherBuffer.size());
        m_deduplicationSet.clear();

        // Snapshot active cooldowns once (avoids per-candidate shared_lock acquisition)
        const auto activeCooldowns = m_cooldownMgr.GetActiveCooldownKeys();

        // Single forward pass: filter + dedup, moving survivors into output.
        // Each candidate is moved at most once (into output). No in-place
        // erase_if means no variant move-assignment chains that trigger
        // string proxy swaps on freed memory.
        size_t weaponsBefore = 0;
        size_t weaponsFilteredByEquipped = 0;
        size_t weaponsFilteredByAffordability = 0;
        size_t weaponsFilteredByCooldown = 0;
        size_t weaponsFilteredByFullVitals = 0;
        size_t weaponsFilteredByActiveBuff = 0;
        size_t weaponsDeduplicated = 0;
        size_t weaponsPassed = 0;

        for (auto& c : gatherBuffer) {
            const bool isWeapon = std::holds_alternative<WeaponCandidate>(c);
            if (isWeapon) ++weaponsBefore;

            // 1-4. Consolidated visitor: affordability, equipped, full vitals, active buff
            //       Single std::visit dispatch instead of 4 separate ones.
            {
                auto result = RunVisitorFilters(c, player, currentMagicka);
                if (result != FilterResult::Passed) {
                    switch (result) {
                        case FilterResult::Affordability:
                            ++stats.filteredByAffordability;
                            if (isWeapon) ++weaponsFilteredByAffordability;
                            break;
                        case FilterResult::Equipped:
                            ++stats.filteredByEquipped;
                            if (isWeapon) ++weaponsFilteredByEquipped;
                            break;
                        case FilterResult::FullVitals:
                            ++stats.filteredByFullVitals;
                            if (isWeapon) ++weaponsFilteredByFullVitals;
                            break;
                        case FilterResult::ActiveBuff:
                            ++stats.filteredByActiveBuff;
                            if (isWeapon) ++weaponsFilteredByActiveBuff;
                            break;
                        default: break;
                    }
                    continue;
                }
            }

            // 5. Cooldown (uses pre-snapshotted set — no lock per candidate)
            {
                const auto& base = GetBase(c);
                if (activeCooldowns.contains(CooldownManager::MakeKey(base.formID, base.sourceType))) {
                    logger::trace("Cooldown filtered: {} [{:X}]", base.name, base.formID);
                    ++stats.filteredByCooldown;
                    if (isWeapon) ++weaponsFilteredByCooldown;
                    continue;
                }
            }

            // 6. Deduplication (inline — avoids a second erase_if pass)
            const auto& base = GetBase(c);
            const uint64_t key = base.GetDeduplicationKey();
            auto [iter, inserted] = m_deduplicationSet.insert(key);
            if (!inserted) {
                ++stats.filteredByDuplication;
                if (isWeapon) ++weaponsDeduplicated;
                logger::warn("[Dedup] COLLISION: '{}' formID={:08X} sourceType={} key={:016X} isWeapon={}",
                    base.name, base.formID, static_cast<int>(base.sourceType), key, isWeapon);
                continue;
            }

            // Passed all filters — move into output (single move per survivor)
            if (isWeapon) ++weaponsPassed;
            output.push_back(std::move(c));
        }

        // Limit output size if configured
        if (m_config.maxCandidatesAfterFilter > 0 &&
            output.size() > m_config.maxCandidatesAfterFilter) {
            size_t beforeTrunc = output.size();
            size_t weaponsBeforeTrunc = weaponsPassed;
            output.resize(m_config.maxCandidatesAfterFilter);
            // Recount weapons only if truncation happened (rare path)
            size_t weaponsAfter = 0;
            for (const auto& c : output) {
                if (std::holds_alternative<WeaponCandidate>(c)) ++weaponsAfter;
            }
            weaponsPassed = weaponsAfter;
            logger::warn("[CandidateFilters] Truncation dropped {} candidates ({} -> {}, limit={})",
                beforeTrunc - m_config.maxCandidatesAfterFilter, beforeTrunc, m_config.maxCandidatesAfterFilter, m_config.maxCandidatesAfterFilter);
        }

        stats.outputCount = output.size();

        // Log cooldown stats (only when count changes)
        static size_t lastCooldownFiltered = 0;
        if (stats.filteredByCooldown != lastCooldownFiltered) {
            logger::debug("[CandidateFilters] Cooldowns: {} filtered ({} active cooldowns)",
                stats.filteredByCooldown, activeCooldowns.size());
            lastCooldownFiltered = stats.filteredByCooldown;
        }

        // Log affordability stats (only when counts change)
        static size_t lastAffordFiltered = 0, lastInput = 0;
        if (stats.filteredByAffordability != lastAffordFiltered || stats.inputCount != lastInput) {
            logger::debug("[CandidateFilters] Affordability: {}/{} spells filtered (policy={}, magicka={:.0f})",
                stats.filteredByAffordability, stats.inputCount,
                ToString(m_config.uncastableSpellPolicy), currentMagicka);
            lastAffordFiltered = stats.filteredByAffordability;
            lastInput = stats.inputCount;
        }

        // Log weapon filter stats (only when count changes)
        static size_t lastWpnBefore = 0, lastWpnAfter = 0;
        if (weaponsBefore != lastWpnBefore || weaponsPassed != lastWpnAfter) {
            logger::info("[CandidateFilters] Weapons: {} in -> {} out "
                "(afford={}, equip={}, cool={}, vital={}, buff={}, rel={}, dedup={})",
                weaponsBefore, weaponsPassed,
                weaponsFilteredByAffordability, weaponsFilteredByEquipped,
                weaponsFilteredByCooldown, weaponsFilteredByFullVitals,
                weaponsFilteredByActiveBuff, weaponsFilteredByRelevance,
                weaponsDeduplicated);
            lastWpnBefore = weaponsBefore;
            lastWpnAfter = weaponsPassed;
        }
    }

    // =========================================================================
    // HELPER: RESIST POTION REDUNDANCY CHECK
    // =========================================================================
    bool CandidateFilters::IsResistPotionRedundant(
        const ItemCandidate& item,
        const State::ActorResistances& resistances) const
    {
        const float threshold = m_config.resistThresholdToFilter;

        if (Item::HasTag(item.tags, Item::ItemTag::ResistFire) &&
            resistances.fire > threshold) {
            return true;
        }
        if (Item::HasTag(item.tags, Item::ItemTag::ResistFrost) &&
            resistances.frost > threshold) {
            return true;
        }
        if (Item::HasTag(item.tags, Item::ItemTag::ResistShock) &&
            resistances.shock > threshold) {
            return true;
        }
        if (Item::HasTag(item.tags, Item::ItemTag::ResistPoison) &&
            resistances.poison > threshold) {
            return true;
        }
        if (Item::HasTag(item.tags, Item::ItemTag::ResistMagic) &&
            resistances.magic > threshold) {
            return true;
        }

        return false;
    }

    // =========================================================================
    // HELPER: RESIST SPELL REDUNDANCY CHECK
    // =========================================================================
    bool CandidateFilters::IsResistSpellRedundant(
        const SpellCandidate& spell,
        const State::ActorResistances& resistances) const
    {
        const float threshold = m_config.resistThresholdToFilter;

        // Check element type for resist spells (usually type == Defensive or Buff)
        if (spell.type == Spell::SpellType::Defensive || spell.type == Spell::SpellType::Buff) {
            // Use element to determine resist type
            switch (spell.element) {
                case Spell::ElementType::Fire:
                    return resistances.fire > threshold;
                case Spell::ElementType::Frost:
                    return resistances.frost > threshold;
                case Spell::ElementType::Shock:
                    return resistances.shock > threshold;
                case Spell::ElementType::Poison:
                    return resistances.poison > threshold;
                default:
                    break;
            }
        }

        return false;
    }

}  // namespace Huginn::Candidate
