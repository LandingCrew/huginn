#include "CandidateFilters.h"
#include "spell/SpellData.h"  // For Spell::HasTag (ScrollTag is alias)
#include "core/Coverage.h"     // SummonLimitReached (0.23.46)
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
    std::optional<Core::Filter::Reason> CandidateFilters::RunVisitorFilters(
        const CandidateVariant& candidate,
        const State::PlayerActorState& player,
        float currentMagicka) const
    {
        const auto policy = m_config.uncastableSpellPolicy;
        const bool checkBuffs = m_config.filterActiveBuffs;
        const bool checkResists = m_config.filterRedundantResists;

        // 0.23.46: each drop names its filter (Core::Filter::Reason) for the
        // selection log v3's `filtered_by`; which candidates are dropped is
        // unchanged except the summon test, which counts against the limit.
        using R = Core::Filter::Reason;
        return std::visit([&](const auto& c) -> std::optional<R> {
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
                        return R::affordability;
                }

                // 2. Equipped
                if (player.IsSpellEquipped(c.formID))
                    return R::equipped;

                // 3. Full Vitals -- the CASTER's, so not for a heal aimed at
                // someone else, nor one that works over time (an aura is cast
                // ahead of the hit).
                if (m_config.filterHealingWhenFull &&
                    Spell::HasTag(c.tags, Spell::SpellTag::RestoreHealth) &&
                    !Spell::HasTagExt(c.tagsExt, Spell::SpellTagExt::TargetsOthers) &&
                    !Spell::HasTagExt(c.tagsExt, Spell::SpellTagExt::HealsOverTime) &&
                    player.vitals.health >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON)
                    return R::full_health;
                if (m_config.filterMagickaWhenFull &&
                    Spell::HasTag(c.tags, Spell::SpellTag::RestoreMagicka) &&
                    player.vitals.magicka >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON)
                    return R::full_magicka;

                // 4. Active Buff
                if (checkBuffs) {
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Invisibility) && player.buffs.isInvisible)
                        return R::invisibility;
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Muffle) && player.buffs.hasMuffle)
                        return R::muffle;
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Armor) && player.buffs.hasArmorBuff)
                        return R::armor;
                    // A summon only once the player's summon LIMIT is reached
                    // (0.23.46). It hid every summon spell while any one was
                    // up, so under Twin Souls (limit 2) the second summon was
                    // never offered (doc 11, step 4). With the vanilla limit
                    // of 1 this is the old test exactly.
                    if (c.type == Spell::SpellType::Summon &&
                        Core::Coverage::SummonLimitReached(player.buffs.summonCount, player.buffs.summonLimit))
                        return R::summon_limit;
                    // Warming aura already running: do not offer it again.
                    // Warm FOOD stays: it restores cold outright.
                    if (Spell::HasTagExt(c.tagsExt, Spell::SpellTagExt::Warming) &&
                        player.buffs.hasWarmingEffect)
                        return R::warming;
                }
                if (checkResists && IsResistSpellRedundant(c, player.resistances))
                    return R::resist;

                return std::nullopt;
            }
            // =================================================================
            // ITEM (Potion, Food, SoulGem)
            // =================================================================
            else if constexpr (std::is_same_v<T, ItemCandidate>) {
                // 1. Affordability (count > 0)
                if (c.count <= 0)
                    return R::affordability;

                // 3. Full Vitals (skip for food/alcohol)
                if (c.type != Item::ItemType::Food && c.type != Item::ItemType::Alcohol) {
                    if (m_config.filterHealingWhenFull &&
                        Item::HasTag(c.tags, Item::ItemTag::RestoreHealth) &&
                        player.vitals.health >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON)
                        return R::full_health;
                    if (m_config.filterMagickaWhenFull &&
                        Item::HasTag(c.tags, Item::ItemTag::RestoreMagicka) &&
                        player.vitals.magicka >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON)
                        return R::full_magicka;
                    if (m_config.filterStaminaWhenFull &&
                        Item::HasTag(c.tags, Item::ItemTag::RestoreStamina) &&
                        player.vitals.stamina >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON)
                        return R::full_stamina;
                }

                // 4. Active Buff. Not the resist check for food/alcohol: it
                // is eaten for hunger, and a stew's side resist (LoreRim) let
                // a poison-immunity necklace strip every stew from the list.
                if (checkResists &&
                    c.type != Item::ItemType::Food && c.type != Item::ItemType::Alcohol &&
                    IsResistPotionRedundant(c, player.resistances))
                    return R::resist;
                if (checkBuffs) {
                    if (Item::HasTag(c.tags, Item::ItemTag::Waterbreathing) && player.buffs.hasWaterBreathing)
                        return R::waterbreathing;
                    if (Item::HasTag(c.tags, Item::ItemTag::Invisibility) && player.buffs.isInvisible)
                        return R::invisibility;
                }

                return std::nullopt;
            }
            // =================================================================
            // SCROLL
            // =================================================================
            else if constexpr (std::is_same_v<T, ScrollCandidate>) {
                // 1. Affordability (count > 0)
                if (c.count <= 0)
                    return R::affordability;

                // 3. Full Vitals (as for spells: not for others, not over time)
                if (m_config.filterHealingWhenFull &&
                    Spell::HasTag(c.tags, Spell::SpellTag::RestoreHealth) &&
                    !Spell::HasTagExt(c.tagsExt, Spell::SpellTagExt::TargetsOthers) &&
                    !Spell::HasTagExt(c.tagsExt, Spell::SpellTagExt::HealsOverTime) &&
                    player.vitals.health >= State::DefaultState::FULL_VITAL - FULL_VITAL_EPSILON)
                    return R::full_health;

                // 4. Active Buff
                if (checkBuffs) {
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Invisibility) && player.buffs.isInvisible)
                        return R::invisibility;
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Muffle) && player.buffs.hasMuffle)
                        return R::muffle;
                }

                return std::nullopt;
            }
            // =================================================================
            // AMMO
            // =================================================================
            else if constexpr (std::is_same_v<T, AmmoCandidate>) {
                if (c.count <= 0) return R::affordability;
                if (c.isEquipped) return R::equipped;
                return std::nullopt;
            }
            // =================================================================
            // WEAPON
            // =================================================================
            else if constexpr (std::is_same_v<T, WeaponCandidate>) {
                // Weapons pass all visitor filters (equipped-skip is per-slot in SlotAllocator)
                return std::nullopt;
            }
            // =================================================================
            // TORCH
            // =================================================================
            else if constexpr (std::is_same_v<T, TorchCandidate>) {
                if (c.count <= 0) return R::affordability;
                if (c.isEquipped) return R::equipped;
                return std::nullopt;
            }
            else {
                return std::nullopt;
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
            else if constexpr (std::is_same_v<T, TorchCandidate>) { return c.count > 0; }
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
            else if constexpr (std::is_same_v<T, TorchCandidate>) { return !c.isEquipped; }
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
        const auto result = RunVisitorFilters(candidate, player, 999999.0f);
        return !result || !Core::Filter::IsActiveBuff(*result);
    }

    bool CandidateFilters::PassesFullVitalsFilter(
        const CandidateVariant& candidate, const State::PlayerActorState& player) const
    {
        const auto result = RunVisitorFilters(candidate, player, 999999.0f);
        return !result || !Core::Filter::IsFullVital(*result);
    }

    // =========================================================================
    // BATCH FILTER APPLICATION
    // =========================================================================
    void CandidateFilters::ApplyAllFilters(
        std::vector<CandidateVariant>& gatherBuffer,
        std::vector<CandidateVariant>& output,
        const State::PlayerActorState& player,
        float currentMagicka,
        FilterStats& stats,
        std::span<const RE::FormID> heldIDs,
        std::vector<CandidateVariant>* heldUnaffordable,
        std::vector<Core::Filter::Dropped>* dropped)
    {
        stats.Reset();
        if (dropped) dropped->clear();
        // 0.23.46: every drop, with its reason, for the selection log v3 only.
        const auto recordDrop = [dropped](const CandidateVariant& c, Core::Filter::Reason reason) {
            if (!dropped) return;
            const auto& base = GetBase(c);
            dropped->push_back(Core::Filter::Dropped{ base.formID, base.uniqueID, reason });
        };
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
                using R = Core::Filter::Reason;
                const auto result = RunVisitorFilters(c, player, currentMagicka);
                if (result) {
                    recordDrop(c, *result);   // before the hold's aside list may move it
                    switch (*result) {
                        case R::affordability:
                            ++stats.filteredByAffordability;
                            if (isWeapon) ++weaponsFilteredByAffordability;
                            // A Remembrance hold's own item, set aside rather
                            // than dropped: the hold shows what the player took
                            // off even at 13 magicka, where the key otherwise
                            // blinked to a potion until magicka ticked back
                            // (2026-09-30). Aside, not passed, so the ranking
                            // can never put an unaffordable spell on ANOTHER key.
                            if (heldUnaffordable &&
                                std::ranges::find(heldIDs, GetBase(c).formID) != heldIDs.end()) {
                                heldUnaffordable->push_back(std::move(c));
                            }
                            break;
                        case R::equipped:
                            ++stats.filteredByEquipped;
                            if (isWeapon) ++weaponsFilteredByEquipped;
                            break;
                        case R::full_health:
                        case R::full_magicka:
                        case R::full_stamina:
                            ++stats.filteredByFullVitals;
                            if (isWeapon) ++weaponsFilteredByFullVitals;
                            break;
                        default:   // the active-buff filters (Core::Filter::IsActiveBuff)
                            ++stats.filteredByActiveBuff;
                            if (isWeapon) ++weaponsFilteredByActiveBuff;
                            break;
                    }
                    continue;
                }
            }

            // 5. Cooldown (uses pre-snapshotted set — no lock per candidate)
            {
                const auto& base = GetBase(c);
                if (activeCooldowns.contains(CooldownManager::MakeKey(base.formID, base.sourceType))) {
                    logger::trace("Cooldown filtered: {} [{:X}]", base.name, base.formID);
                    recordDrop(c, Core::Filter::Reason::cooldown);
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
                recordDrop(c, Core::Filter::Reason::duplicate);
                ++stats.filteredByDuplication;
                if (isWeapon) ++weaponsDeduplicated;
                // Trace, not warn: a duplicate is a normal outcome, and this
                // fires per candidate per tick. The per-pass count is already
                // reported as "{} dupe" in PrintStats.
                logger::trace("[Dedup] COLLISION: '{}' formID={:08X} sourceType={} key={:016X} isWeapon={}",
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
            for (size_t i = m_config.maxCandidatesAfterFilter; i < output.size(); ++i) {
                recordDrop(output[i], Core::Filter::Reason::truncated);
            }
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
                "(afford={}, equip={}, cool={}, vital={}, buff={}, dedup={})",
                weaponsBefore, weaponsPassed,
                weaponsFilteredByAffordability, weaponsFilteredByEquipped,
                weaponsFilteredByCooldown, weaponsFilteredByFullVitals,
                weaponsFilteredByActiveBuff, weaponsDeduplicated);
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

        // Defensive only. A Defensive spell's element is what it protects
        // against, so already having that resistance makes it redundant.
        //
        // Buff was accepted here too, for the same stale reason as in
        // ContextWeightForCandidate: before #128 an elemental resist spell was
        // typed Buff. It is Defensive now, and the Buffs that carry an element
        // carry it to describe damage they DEAL. Suppressing Strider's Shroud
        // because the player already resists poison withheld a poison aura on
        // the grounds that they did not need protection it was not offering.
        if (spell.type == Spell::SpellType::Defensive) {
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
