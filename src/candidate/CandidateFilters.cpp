#include "CandidateFilters.h"
#include "spell/SpellData.h"  // For Spell::HasTag (ScrollTag is alias)
#include "core/Coverage.h"     // SummonLimitReached (0.23.46)
#include "core/RefreshPass.h"  // the refresh pass (R8.1)
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
    // HARD GATES (R8.1, 0.23.55) -- the old engine's context zeros
    // =========================================================================
    // Map Phase 6 item 1. Each test is a Core::Gate predicate (core/HardGates.h)
    // on facts Context::HardGateInputs read from the state, the same facts
    // EvaluateRules raises the old weights by, so a gate drops exactly what the
    // old context weight zeroed (WeightForCandidate's 0 arms) and what the
    // Suppress skip in UtilityScorer used to skip. Run in the visitor after
    // full vitals and before the active-buff filters.
    // =========================================================================
    namespace
    {
        using R = Core::Filter::Reason;

        [[nodiscard]] Core::Gate::Crafts CraftsOf(const Apparel::CraftMagnitudes& m) noexcept
        {
            return Core::Gate::Crafts{
                .smithing = m.Fortifies(Apparel::CraftSkill::Smithing),
                .enchanting = m.Fortifies(Apparel::CraftSkill::Enchanting),
                .alchemy = m.Fortifies(Apparel::CraftSkill::Alchemy),
            };
        }

        template <class T>
        [[nodiscard]] std::optional<R> HardGate(const T& c, const Core::Gate::Inputs& g) noexcept
        {
            // A favourite under Suppress (IsFavoritedCandidate: the one test
            // Candidate::IsFavorited and the scorer read too).
            if (Core::Gate::FavoriteSuppressed(IsFavoritedCandidate(c), g.suppressFavorites))
                return R::favorite_suppressed;
            if constexpr (std::is_same_v<T, SpellCandidate>) {
                if (Core::Gate::NoFollower(Spell::HasTagExt(c.tagsExt, Spell::SpellTagExt::TargetsOthers), g.followerNear))
                    return R::no_follower;
            }
            else if constexpr (std::is_same_v<T, ScrollCandidate>) {
                if (Core::Gate::NoFollower(Spell::HasTagExt(c.tagsExt, Spell::SpellTagExt::TargetsOthers), g.followerNear))
                    return R::no_follower;
            }
            else if constexpr (std::is_same_v<T, ApparelCandidate>) {
                if (Core::Gate::NoWorkstation(CraftsOf(c.magnitudes), g.station))
                    return R::no_workstation;
            }
            else if constexpr (std::is_same_v<T, TorchCandidate>) {
                if (Core::Gate::NotDark(g.darknessApplies))
                    return R::not_dark;
            }
            return std::nullopt;
        }

        // R8.1 refresh pass: the block is in its last fRefreshPriorSec seconds.
        [[nodiscard]] bool Due(const State::PlayerActorState& player, Core::Refresh::Block b) noexcept
        {
            return Core::Refresh::IsDue(player.buffs.refreshDue, b);
        }
    }

    // =========================================================================
    // CONSOLIDATED VISITOR — single std::visit dispatch per candidate
    // Runs affordability, equipped, full vitals, the hard gates and active buff
    // checks. Returns early on the first failure.
    // =========================================================================
    std::optional<Core::Filter::Reason> CandidateFilters::RunVisitorFilters(
        const CandidateVariant& candidate,
        const State::PlayerActorState& player,
        float currentMagicka,
        const Core::Gate::Inputs& gates) const
    {
        const auto policy = m_config.uncastableSpellPolicy;
        const bool checkBuffs = m_config.filterActiveBuffs;
        const bool checkResists = m_config.filterRedundantResists;
        using B = Core::Refresh::Block;

        // 0.23.46: each drop names its filter (Core::Filter::Reason) for the
        // selection log v3's `filtered_by`; which candidates are dropped is
        // unchanged except the summon test, which counts against the limit.
        // R8.1 (0.23.55): the hard gates drop what the old context weight
        // zeroed, and each active-buff filter lets its item through while the
        // buff that blocks it is due for its refresh (ActorBuffs::refreshDue).
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

                // 4. Hard gates (R8.1): a suppressed favourite, a spell for
                // someone else with no follower near.
                if (const auto gate = HardGate(c, gates)) return gate;

                // 5. Active Buff (R8.1: each passes in its buff's last seconds)
                if (checkBuffs) {
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Invisibility) && player.buffs.isInvisible &&
                        !Due(player, B::invisibility))
                        return R::invisibility;
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Muffle) && player.buffs.hasMuffle &&
                        !Due(player, B::muffle))
                        return R::muffle;
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Armor) && player.buffs.hasArmorBuff &&
                        !Due(player, B::armor))
                        return R::armor;
                    // A summon only once the player's summon LIMIT is reached
                    // (0.23.46). It hid every summon spell while any one was
                    // up, so under Twin Souls (limit 2) the second summon was
                    // never offered (doc 11, step 4). With the vanilla limit
                    // of 1 this is the old test exactly. R8.1: offered again
                    // once a summon is about to time out and free a place.
                    if (c.type == Spell::SpellType::Summon &&
                        Core::Coverage::SummonLimitReached(player.buffs.summonCount, player.buffs.summonLimit) &&
                        !Due(player, B::summon_limit))
                        return R::summon_limit;
                    // Warming aura already running: do not offer it again.
                    // Warm FOOD stays: it restores cold outright.
                    if (Spell::HasTagExt(c.tagsExt, Spell::SpellTagExt::Warming) &&
                        player.buffs.hasWarmingEffect && !Due(player, B::warming))
                        return R::warming;
                }
                if (checkResists && IsResistSpellRedundant(c, player.resistances, player.buffs.refreshDue))
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

                // 4. Hard gates: none applies to an item.

                // 5. Active Buff. Not the resist check for food/alcohol: it
                // is eaten for hunger, and a stew's side resist (LoreRim) let
                // a poison-immunity necklace strip every stew from the list.
                if (checkResists &&
                    c.type != Item::ItemType::Food && c.type != Item::ItemType::Alcohol &&
                    IsResistPotionRedundant(c, player.resistances, player.buffs.refreshDue))
                    return R::resist;
                if (checkBuffs) {
                    if (Item::HasTag(c.tags, Item::ItemTag::Waterbreathing) && player.buffs.hasWaterBreathing &&
                        !Due(player, B::waterbreathing))
                        return R::waterbreathing;
                    if (Item::HasTag(c.tags, Item::ItemTag::Invisibility) && player.buffs.isInvisible &&
                        !Due(player, B::invisibility))
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

                // 4. Hard gates (R8.1): a scroll for someone else, no follower near.
                if (const auto gate = HardGate(c, gates)) return gate;

                // 5. Active Buff
                if (checkBuffs) {
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Invisibility) && player.buffs.isInvisible &&
                        !Due(player, B::invisibility))
                        return R::invisibility;
                    if (Spell::HasTag(c.tags, Spell::SpellTag::Muffle) && player.buffs.hasMuffle &&
                        !Due(player, B::muffle))
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
                // Weapons pass the other visitor filters (equipped-skip is
                // per-slot in SlotAllocator); a suppressed favourite is gated.
                return HardGate(c, gates);
            }
            // =================================================================
            // TORCH
            // =================================================================
            else if constexpr (std::is_same_v<T, TorchCandidate>) {
                if (c.count <= 0) return R::affordability;
                if (c.isEquipped) return R::equipped;
                // R8.1: no torch while darkness does not apply.
                return HardGate(c, gates);
            }
            // =================================================================
            // APPAREL (R8.1: no arm before; the gather skips worn pieces)
            // =================================================================
            else if constexpr (std::is_same_v<T, ApparelCandidate>) {
                // No station up for any craft the piece fortifies.
                return HardGate(c, gates);
            }
            else {
                static_assert(always_false_v<T>, "Unhandled CandidateVariant alternative in RunVisitorFilters");
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

    // =========================================================================
    // BATCH FILTER APPLICATION
    // =========================================================================
    void CandidateFilters::ApplyAllFilters(
        std::vector<CandidateVariant>& gatherBuffer,
        std::vector<CandidateVariant>& output,
        const State::PlayerActorState& player,
        float currentMagicka,
        const Core::Gate::Inputs& gates,
        FilterStats& stats,
        std::span<const RE::FormID> heldIDs,
        std::vector<CandidateVariant>* heldAside,
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
        size_t weaponsFilteredByHardGate = 0;
        size_t weaponsDeduplicated = 0;
        size_t weaponsPassed = 0;

        for (auto& c : gatherBuffer) {
            const bool isWeapon = std::holds_alternative<WeaponCandidate>(c);
            if (isWeapon) ++weaponsBefore;

            // 1-5. Consolidated visitor: affordability, equipped, full vitals,
            //       the hard gates, active buff. One std::visit dispatch.
            {
                using R = Core::Filter::Reason;
                const auto result = RunVisitorFilters(c, player, currentMagicka, gates);
                if (result) {
                    recordDrop(c, *result);   // before the hold's aside list may move it
                    // A Remembrance hold's own item, set aside rather than
                    // dropped: the hold shows what the player took off even
                    // at 13 magicka, where the key otherwise blinked to a
                    // potion until magicka ticked back (2026-09-30). Aside, not
                    // passed, so the ranking can never put it on ANOTHER key.
                    // R8.1: a hard gate's item too -- a torch taken off in
                    // daylight, a ring away from the forge. Until 0.23.55 those
                    // passed the filters (the context weight dropped them in
                    // the scorer), so the hold could still find them.
                    const auto setAsideForHold = [&] {
                        if (heldAside && std::ranges::find(heldIDs, GetBase(c).formID) != heldIDs.end()) {
                            heldAside->push_back(std::move(c));
                        }
                    };
                    switch (*result) {
                        case R::affordability:
                            ++stats.filteredByAffordability;
                            if (isWeapon) ++weaponsFilteredByAffordability;
                            setAsideForHold();
                            break;
                        case R::no_follower:
                        case R::no_workstation:
                        case R::not_dark:
                        case R::favorite_suppressed:
                            ++stats.filteredByHardGate;
                            if (isWeapon) ++weaponsFilteredByHardGate;
                            // Not while on cooldown: 0.23.54 dropped such an
                            // item at the cooldown stage, so the hold never saw
                            // it. A torch taken off in daylight within its
                            // cooldown now reads as in the dark, where the
                            // same torch drops at the cooldown stage below.
                            {
                                const auto& gated = GetBase(c);
                                if (!activeCooldowns.contains(CooldownManager::MakeKey(gated.formID, gated.sourceType)))
                                    setAsideForHold();
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
                "(afford={}, equip={}, cool={}, vital={}, buff={}, gate={}, dedup={})",
                weaponsBefore, weaponsPassed,
                weaponsFilteredByAffordability, weaponsFilteredByEquipped,
                weaponsFilteredByCooldown, weaponsFilteredByFullVitals,
                weaponsFilteredByActiveBuff, weaponsFilteredByHardGate, weaponsDeduplicated);
            lastWpnBefore = weaponsBefore;
            lastWpnAfter = weaponsPassed;
        }
    }

    // =========================================================================
    // HELPER: RESIST POTION REDUNDANCY CHECK
    // =========================================================================
    // R8.1: an element whose resistance block is due for its refresh (it falls
    // to the threshold within fRefreshPriorSec, ActorBuffs::refreshDue) does
    // not make the potion redundant.
    bool CandidateFilters::IsResistPotionRedundant(
        const ItemCandidate& item,
        const State::ActorResistances& resistances,
        Core::Refresh::DueBits refreshDue) const
    {
        const float threshold = m_config.resistThresholdToFilter;
        using E = Core::Refresh::Element;
        const auto blocked = [&](float resist, E e) {
            return resist > threshold && !Core::Refresh::IsDue(refreshDue, Core::Refresh::ResistBlock(e));
        };

        if (Item::HasTag(item.tags, Item::ItemTag::ResistFire) && blocked(resistances.fire, E::fire)) {
            return true;
        }
        if (Item::HasTag(item.tags, Item::ItemTag::ResistFrost) && blocked(resistances.frost, E::frost)) {
            return true;
        }
        if (Item::HasTag(item.tags, Item::ItemTag::ResistShock) && blocked(resistances.shock, E::shock)) {
            return true;
        }
        if (Item::HasTag(item.tags, Item::ItemTag::ResistPoison) && blocked(resistances.poison, E::poison)) {
            return true;
        }
        if (Item::HasTag(item.tags, Item::ItemTag::ResistMagic) && blocked(resistances.magic, E::magic)) {
            return true;
        }

        return false;
    }

    // =========================================================================
    // HELPER: RESIST SPELL REDUNDANCY CHECK
    // =========================================================================
    bool CandidateFilters::IsResistSpellRedundant(
        const SpellCandidate& spell,
        const State::ActorResistances& resistances,
        Core::Refresh::DueBits refreshDue) const
    {
        const float threshold = m_config.resistThresholdToFilter;
        using E = Core::Refresh::Element;
        const auto blocked = [&](float resist, E e) {
            return resist > threshold && !Core::Refresh::IsDue(refreshDue, Core::Refresh::ResistBlock(e));
        };

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
                    return blocked(resistances.fire, E::fire);
                case Spell::ElementType::Frost:
                    return blocked(resistances.frost, E::frost);
                case Spell::ElementType::Shock:
                    return blocked(resistances.shock, E::shock);
                case Spell::ElementType::Poison:
                    return blocked(resistances.poison, E::poison);
                default:
                    break;
            }
        }

        return false;
    }

}  // namespace Huginn::Candidate
