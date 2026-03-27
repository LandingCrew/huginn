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
    // AFFORDABILITY FILTER
    // =========================================================================
    bool CandidateFilters::PassesAffordabilityFilter(
        const CandidateVariant& candidate,
        float currentMagicka) const
    {
        return std::visit([currentMagicka, policy = m_config.uncastableSpellPolicy](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, SpellCandidate>) {
                // Look up the actual spell form to compute effective cost
                // (baseCost is raw form cost — doesn't include perk/enchantment reductions)
                auto* form = RE::TESForm::LookupByID(c.formID);
                auto* spell = form ? form->As<RE::SpellItem>() : nullptr;
                // Player should never be null here (guarded by UpdateLoop actorValue check),
                // but defensively fall back to baseCost if form lookup or player fails
                auto* player = RE::PlayerCharacter::GetSingleton();

                float effectiveCost = (spell && player)
                    ? spell->CalculateMagickaCost(player)
                    : static_cast<float>(c.baseCost);

                if (c.isConcentration && effectiveCost <= 0.0f) {
                    effectiveCost = static_cast<float>(c.baseCost);
                }

                if (currentMagicka >= effectiveCost) {
                    logger::trace("Affordable spell passed: {} [{:X}] cost={:.0f} magicka={:.0f}",
                        c.name, c.formID, effectiveCost, currentMagicka);
                    return true;  // Can afford — always keep
                }

                // Can't afford — policy determines behavior
                // Allow and Penalize both keep the spell; Disallow filters it out
                logger::trace("Unaffordable spell {} (policy={}): {} [{:X}] cost={:.0f} magicka={:.0f}",
                    policy == UncastableSpellPolicy::Disallow ? "filtered" : "kept",
                    ToString(policy), c.name, c.formID, effectiveCost, currentMagicka);
                return policy != UncastableSpellPolicy::Disallow;
            }
            else if constexpr (std::is_same_v<T, ItemCandidate>) {
                // Items require count > 0
                return c.count > 0;
            }
            else if constexpr (std::is_same_v<T, ScrollCandidate>) {
                // Scrolls require count > 0
                return c.count > 0;
            }
            else if constexpr (std::is_same_v<T, AmmoCandidate>) {
                // Ammo requires count > 0
                return c.count > 0;
            }
            else if constexpr (std::is_same_v<T, WeaponCandidate>) {
                // Weapons are always "affordable" (no cost to equip)
                return true;
            }
            else {
                return true;
            }
        }, candidate);
    }

    // =========================================================================
    // EQUIPPED FILTER
    // =========================================================================
    bool CandidateFilters::PassesEquippedFilter(
        const CandidateVariant& candidate,
        const State::PlayerActorState& player) const
    {
        return std::visit([&player](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, SpellCandidate>) {
                // Check if spell is equipped in either hand
                return !player.IsSpellEquipped(c.formID);
            }
            else if constexpr (std::is_same_v<T, WeaponCandidate>) {
                // Weapons pass the global filter — equipped-skip is now per-slot
                // via bSkipEquipped in SlotAllocator::FindBestCandidate()
                return true;
            }
            else if constexpr (std::is_same_v<T, AmmoCandidate>) {
                // Check if ammo is already equipped
                return !c.isEquipped;
            }
            else {
                // Items and scrolls can't be "equipped" in the traditional sense
                return true;
            }
        }, candidate);
    }

    // =========================================================================
    // COOLDOWN FILTER
    // =========================================================================
    bool CandidateFilters::PassesCooldownFilter(const CandidateVariant& candidate) const
    {
        const auto& base = GetBase(candidate);
        return !m_cooldownMgr.IsOnCooldown(base.formID, base.sourceType);
    }

    // =========================================================================
    // ACTIVE BUFF FILTER
    // =========================================================================
    bool CandidateFilters::PassesActiveBuffFilter(
        const CandidateVariant& candidate,
        const State::PlayerActorState& player) const
    {
        // Early exit if filtering is disabled
        if (!m_config.filterActiveBuffs && !m_config.filterRedundantResists) {
            return true;
        }

        return std::visit([&](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, ItemCandidate>) {
                // Check resist potions against current resistances
                if (m_config.filterRedundantResists) {
                    if (IsResistPotionRedundant(c, player.resistances)) {
                        return false;
                    }
                }

                // Check buff potions against active buffs
                if (m_config.filterActiveBuffs) {
                    if (Item::HasTag(c.tags, Item::ItemTag::Waterbreathing) &&
                        player.buffs.hasWaterBreathing) {
                        return false;
                    }
                    if (Item::HasTag(c.tags, Item::ItemTag::Invisibility) &&
                        player.buffs.isInvisible) {
                        return false;
                    }
                }

                return true;
            }
            else if constexpr (std::is_same_v<T, SpellCandidate>) {
                if (m_config.filterActiveBuffs) {
                    // Filter waterbreathing spell if already has effect
                    // Note: Would need a specific tag for waterbreathing spells
                    // For now, check utility spells with Stealth tag (approximation)
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Invisibility) &&
                        player.buffs.isInvisible) {
                        return false;
                    }
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Muffle) &&
                        player.buffs.hasMuffle) {
                        return false;
                    }

                    // Filter armor spells if any armor buff is already active
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Armor) &&
                        player.buffs.hasArmorBuff) {
                        return false;
                    }

                    // Filter summon spells if already have an active summon
                    if (c.type == Spell::SpellType::Summon &&
                        player.buffs.hasActiveSummon) {
                        return false;
                    }
                }

                // Check resist spells against current resistances
                if (m_config.filterRedundantResists) {
                    if (IsResistSpellRedundant(c, player.resistances)) {
                        return false;
                    }
                }

                return true;
            }
            else if constexpr (std::is_same_v<T, ScrollCandidate>) {
                if (m_config.filterActiveBuffs) {
                    // Similar logic as spells, using scroll tags (which alias spell tags)
                    // ScrollTag is an alias for Spell::SpellTag, so use Spell::HasTag
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Invisibility) &&
                        player.buffs.isInvisible) {
                        return false;
                    }
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Muffle) &&
                        player.buffs.hasMuffle) {
                        return false;
                    }
                }
                return true;
            }
            else {
                // Weapons and ammo don't have buff effects
                return true;
            }
        }, candidate);
    }

    // =========================================================================
    // FULL VITALS FILTER
    // =========================================================================
    bool CandidateFilters::PassesFullVitalsFilter(
        const CandidateVariant& candidate,
        const State::PlayerActorState& player) const
    {
        return std::visit([&](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, ItemCandidate>) {
                // Food and alcohol are consumed for survival/roleplay, not just
                // restore effects — never filter them by full vitals.
                if (c.type == Item::ItemType::Food ||
                    c.type == Item::ItemType::Alcohol) {
                    return true;
                }

                // Filter health potions when health is full
                if (m_config.filterHealingWhenFull &&
                    Item::HasTag(c.tags, Item::ItemTag::RestoreHealth) &&
                    player.vitals.health >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON) {
                    return false;
                }

                // Filter magicka potions when magicka is full
                if (m_config.filterMagickaWhenFull &&
                    Item::HasTag(c.tags, Item::ItemTag::RestoreMagicka) &&
                    player.vitals.magicka >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON) {
                    return false;
                }

                // Filter stamina potions when stamina is full
                if (m_config.filterStaminaWhenFull &&
                    Item::HasTag(c.tags, Item::ItemTag::RestoreStamina) &&
                    player.vitals.stamina >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON) {
                    return false;
                }

                return true;
            }
            else if constexpr (std::is_same_v<T, SpellCandidate>) {
                // Filter healing spells when health is full
                if (m_config.filterHealingWhenFull &&
                    Spell::HasTag(c.tags, Spell::SpellTag::RestoreHealth) &&
                    player.vitals.health >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON) {
                    return false;
                }

                // Filter magicka restoration (rare, but some mods have them)
                if (m_config.filterMagickaWhenFull &&
                    Spell::HasTag(c.tags, Spell::SpellTag::RestoreMagicka) &&
                    player.vitals.magicka >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON) {
                    return false;
                }

                return true;
            }
            else if constexpr (std::is_same_v<T, ScrollCandidate>) {
                // Similar logic for scrolls (ScrollTag is alias for Spell::SpellTag)
                if (m_config.filterHealingWhenFull &&
                    Spell::HasTag(c.tags, Spell::SpellTag::RestoreHealth) &&
                    player.vitals.health >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON) {
                    return false;
                }

                return true;
            }
            else {
                // Weapons and ammo aren't affected by vitals
                return true;
            }
        }, candidate);
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
        size_t weaponsFilteredByRelevance = 0;
        size_t weaponsDeduplicated = 0;
        size_t weaponsPassed = 0;

        for (auto& c : gatherBuffer) {
            const bool isWeapon = std::holds_alternative<WeaponCandidate>(c);
            if (isWeapon) ++weaponsBefore;

            // 1. Affordability (cheapest — just a comparison)
            if (!PassesAffordabilityFilter(c, currentMagicka)) {
                ++stats.filteredByAffordability;
                if (isWeapon) ++weaponsFilteredByAffordability;
                continue;
            }
            // 2. Equipped
            if (!PassesEquippedFilter(c, player)) {
                ++stats.filteredByEquipped;
                if (isWeapon) ++weaponsFilteredByEquipped;
                continue;
            }
            // 3. Cooldown (uses pre-snapshotted set — no lock per candidate)
            {
                const auto& base = GetBase(c);
                if (activeCooldowns.contains(CooldownManager::MakeKey(base.formID, base.sourceType))) {
                    logger::trace("Cooldown filtered: {} [{:X}]", base.name, base.formID);
                    ++stats.filteredByCooldown;
                    if (isWeapon) ++weaponsFilteredByCooldown;
                    continue;
                }
            }
            // 4. Full Vitals
            if (!PassesFullVitalsFilter(c, player)) {
                ++stats.filteredByFullVitals;
                if (isWeapon) ++weaponsFilteredByFullVitals;
                continue;
            }
            // 5. Active Buff (more expensive — checks multiple buff states)
            if (!PassesActiveBuffFilter(c, player)) {
                ++stats.filteredByActiveBuff;
                if (isWeapon) ++weaponsFilteredByActiveBuff;
                continue;
            }
            // 6. Relevance - DISABLED (Stage 1g)
            // Relevance filtering has been moved to UtilityScorer via minimumContextWeight.
            // The old baseRelevance field is no longer set by CandidateGenerator.
            // All context weight filtering now happens in UtilityScorer::ScoreCandidates().
            // if (!PassesRelevanceFilter(c)) {
            //     ++stats.filteredByRelevance;
            //     if (isWeapon) ++weaponsFilteredByRelevance;
            //     continue;
            // }
            // 7. Deduplication (inline — avoids a second erase_if pass)
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
            size_t weaponsBeforeTrunc = weaponsPassed;
            output.resize(m_config.maxCandidatesAfterFilter);
            // Recount weapons only if truncation happened (rare path)
            size_t weaponsAfter = 0;
            for (const auto& c : output) {
                if (std::holds_alternative<WeaponCandidate>(c)) ++weaponsAfter;
            }
            weaponsPassed = weaponsAfter;
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
