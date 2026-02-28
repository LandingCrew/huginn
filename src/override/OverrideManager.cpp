#include "OverrideManager.h"
#include "learning/item/ItemRegistry.h"
#include "learning/item/ItemData.h"
#include "spell/SpellRegistry.h"
#include "weapon/WeaponRegistry.h"
#include <algorithm>

namespace Huginn::Override
{
    // =============================================================================
    // SINGLETON
    // =============================================================================

    OverrideManager& OverrideManager::GetSingleton()
    {
        static OverrideManager instance;
        return instance;
    }

    // =============================================================================
    // LIFECYCLE
    // =============================================================================

    void OverrideManager::Initialize(Item::ItemRegistry& itemRegistry, Spell::SpellRegistry& spellRegistry,
                                     Weapon::WeaponRegistry& weaponRegistry)
    {
        m_itemRegistry = &itemRegistry;
        m_spellRegistry = &spellRegistry;
        m_weaponRegistry = &weaponRegistry;
        m_initialized = true;

        logger::info("[OverrideManager] Initialized with registries"sv);
    }

    void OverrideManager::Reset()
    {
        m_hysteresisStates.clear();
        m_lastActiveCount = 0;

        logger::debug("[OverrideManager] Reset hysteresis states"sv);
    }

    void OverrideManager::Update(float deltaMs)
    {
        // Guard: Skip if not initialized
        if (!m_initialized) {
            return;
        }

        // Update active durations for all hysteresis states
        for (auto& [name, state] : m_hysteresisStates) {
            if (state.isActive) {
                state.activeDurationMs += deltaMs;
            }
        }
    }

    // =============================================================================
    // MAIN EVALUATION
    // =============================================================================

    OverrideCollection OverrideManager::EvaluateOverrides(
        const State::PlayerActorState& player,
        [[maybe_unused]] const State::WorldState& world)
    {
        OverrideCollection result;

        if (!m_initialized) {
            return result;
        }

        // Evaluate each override condition (v0.10.0: Config values are now runtime-configurable)
        if (Config::ENABLE_CRITICAL_HEALTH()) {
            if (auto override = EvaluateCriticalHealth(player)) {
                result.activeOverrides.push_back(std::move(*override));
            }
        }

        if (Config::ENABLE_DROWNING()) {
            if (auto override = EvaluateDrowning(player)) {
                result.activeOverrides.push_back(std::move(*override));
            }
        }

        if (Config::ENABLE_WEAPON_CHARGE()) {
            if (auto override = EvaluateWeaponCharge(player)) {
                result.activeOverrides.push_back(std::move(*override));
            }
        }

        if (Config::ENABLE_LOW_AMMO()) {
            if (auto override = EvaluateLowAmmo(player)) {
                result.activeOverrides.push_back(std::move(*override));
            }
        }

        if (Config::ENABLE_CRITICAL_MAGICKA()) {
            if (auto override = EvaluateCriticalMagicka(player)) {
                result.activeOverrides.push_back(std::move(*override));
            }
        }

        if (Config::ENABLE_CRITICAL_STAMINA()) {
            if (auto override = EvaluateCriticalStamina(player)) {
                result.activeOverrides.push_back(std::move(*override));
            }
        }

        // Sort by priority (highest first)
        result.SortByPriority();

        // Track for diagnostics - only log when count changes
        size_t currentCount = result.GetCount();
        if (currentCount != m_lastActiveCount) {
            if (currentCount > 0) {
                logger::debug("[OverrideManager] {} active override(s)"sv, currentCount);
            } else if (m_lastActiveCount > 0) {
                logger::debug("[OverrideManager] Overrides cleared"sv);
            }
            m_lastActiveCount = currentCount;
        }

        return result;
    }

    // =============================================================================
    // APPLY OVERRIDES
    // =============================================================================

    Scoring::ScoredCandidateList OverrideManager::ApplyOverrides(
        Scoring::ScoredCandidateList candidates,
        const OverrideCollection& overrides)
    {
        if (!overrides.HasActiveOverride()) {
            return candidates;  // No overrides - return unchanged
        }

        // Build list of override candidates with boosted utility
        std::vector<Scoring::ScoredCandidate> overrideCandidates;
        overrideCandidates.reserve(overrides.GetCount());

        for (const auto& override : overrides.activeOverrides) {
            if (!override.candidate.has_value()) {
                // Condition met but no item available - log warning
                logger::warn("[OverrideManager] Override active but no item: {}"sv, override.reason);
                continue;
            }

            // Get name before any moves for safe logging
            const std::string candidateName = Candidate::GetName(override.candidate.value());

            // Create scored candidate with massive utility boost
            Scoring::ScoredCandidate scored;
            scored.candidate = override.candidate.value();  // Copy, not move
            scored.utility = Config::OVERRIDE_UTILITY_BOOST + static_cast<float>(override.priority);
            scored.isWildcard = false;

            // Set breakdown to show it's an override
            scored.breakdown.contextWeight = Config::OVERRIDE_UTILITY_BOOST;
            scored.breakdown.qValue = 0.0f;
            scored.breakdown.prior = 0.0f;
            scored.breakdown.ucb = 0.0f;
            scored.breakdown.confidence = 1.0f;
            scored.breakdown.learningScore = 0.0f;
            scored.breakdown.correlationBonus = 0.0f;
            scored.breakdown.potionMultiplier = 1.0f;
            scored.breakdown.favoritesMultiplier = 1.0f;

            logger::debug("[OverrideManager] Injecting override: {} (priority {})"sv,
                candidateName, override.priority);

            overrideCandidates.push_back(std::move(scored));
        }

        // Insert override candidates at the front
        // This ensures they get the top slots after the list is sorted
        candidates.insert(candidates.begin(),
            std::make_move_iterator(overrideCandidates.begin()),
            std::make_move_iterator(overrideCandidates.end()));

        // Re-sort to ensure proper ordering (overrides have highest utility)
        std::sort(candidates.begin(), candidates.end());

        return candidates;
    }

    // =============================================================================
    // CONDITION EVALUATORS
    // =============================================================================

    std::optional<OverrideResult> OverrideManager::EvaluateCriticalHealth(
        const State::PlayerActorState& player)
    {
        // v0.10.0: Non-static to allow runtime config changes
        const HysteresisConfig config{
            Config::CRITICAL_HEALTH_THRESHOLD(),
            Config::CRITICAL_HEALTH_HYSTERESIS(),
            Config::MIN_OVERRIDE_DURATION_MS()
        };

        // Check if health is below critical threshold (with hysteresis)
        if (!CheckThresholdHysteresis("CriticalHealth", player.vitals.health, config)) {
            return std::nullopt;
        }

        // Find best health potion
        auto candidate = FindHealthPotion();

        OverrideResult result;
        result.active = true;
        result.priority = Priority::CRITICAL_HEALTH;
        result.category = OverrideCategory::HP;
        result.reason = "CRITICAL: Need Health Potion!";
        result.candidate = std::move(candidate);
        result.targetSlot = 0;

        return result;
    }

    std::optional<OverrideResult> OverrideManager::EvaluateDrowning(
        const State::PlayerActorState& player)
    {
        // v0.10.0: Non-static to allow runtime config changes
        const HysteresisConfig config{
            0.0f,  // Not threshold-based
            0.0f,
            Config::MIN_OVERRIDE_DURATION_MS()
        };

        // Check if underwater without waterbreathing buff
        bool conditionMet = player.isUnderwater && !player.buffs.hasWaterBreathing;

        if (!CheckHysteresis("Drowning", conditionMet, config)) {
            return std::nullopt;
        }

        // Find waterbreathing item (potion or spell)
        auto candidate = FindWaterbreathingItem();

        OverrideResult result;
        result.active = true;
        result.priority = Priority::DROWNING;
        result.category = OverrideCategory::Other;
        result.reason = "DROWNING: Need Waterbreathing!";
        result.candidate = std::move(candidate);
        result.targetSlot = 0;

        return result;
    }

    std::optional<OverrideResult> OverrideManager::EvaluateWeaponCharge(
        const State::PlayerActorState& player)
    {
        // v0.10.0: Non-static to allow runtime config changes
        const HysteresisConfig config{
            Config::WEAPON_CHARGE_THRESHOLD(),
            Config::WEAPON_CHARGE_HYSTERESIS(),
            Config::MIN_OVERRIDE_DURATION_MS()
        };

        // Only check if player has an enchanted weapon equipped
        if (!player.hasEnchantedWeapon) {
            // Clear hysteresis state if no enchanted weapon
            auto it = m_hysteresisStates.find("WeaponCharge");
            if (it != m_hysteresisStates.end()) {
                it->second.isActive = false;
                it->second.activeDurationMs = 0.0f;
            }
            return std::nullopt;
        }

        // Debug: Log weapon charge state
        logger::debug("[OverrideManager] WeaponCharge check: hasEnchanted={}, charge={:.2f}%"sv,
            player.hasEnchantedWeapon, player.weaponChargePercent * 100.0f);

        // Check if weapon charge is depleted (with hysteresis)
        if (!CheckThresholdHysteresis("WeaponCharge", player.weaponChargePercent, config)) {
            return std::nullopt;
        }

        // Find best soul gem
        auto candidate = FindSoulGem();

        if (!candidate.has_value()) {
            logger::warn("[OverrideManager] WeaponCharge: No soul gem found in inventory!"sv);
        }

        OverrideResult result;
        result.active = true;
        result.priority = Priority::WEAPON_EMPTY;
        result.category = OverrideCategory::Other;
        result.reason = "WEAPON EMPTY: Need Soul Gem!";
        result.candidate = std::move(candidate);
        result.targetSlot = 0;

        return result;
    }

    std::optional<OverrideResult> OverrideManager::EvaluateLowAmmo(
        const State::PlayerActorState& player)
    {
        const HysteresisConfig config{
            Config::LOW_AMMO_THRESHOLD(),
            Config::LOW_AMMO_HYSTERESIS(),
            Config::MIN_OVERRIDE_DURATION_MS()
        };

        // Only check if bow or crossbow equipped
        if (!player.hasBowEquipped && !player.hasCrossbowEquipped) {
            // Clear hysteresis state if no ranged weapon
            auto it = m_hysteresisStates.find("LowAmmo");
            if (it != m_hysteresisStates.end()) {
                it->second.isActive = false;
                it->second.activeDurationMs = 0.0f;
            }
            return std::nullopt;
        }

        // Get current ammo count (arrowCount/boltCount are -1 when no ammo equipped)
        int32_t rawCount = player.hasBowEquipped ? player.arrowCount : player.boltCount;
        float currentAmmo = rawCount >= 0 ? static_cast<float>(rawCount) : 0.0f;

        // Check threshold with hysteresis
        if (!CheckThresholdHysteresis("LowAmmo", currentAmmo, config)) {
            return std::nullopt;
        }

        // Find best ammo in inventory
        auto candidate = FindBestAmmo(player.hasBowEquipped);

        OverrideResult result;
        result.active = true;
        result.priority = Priority::LOW_AMMO;
        result.category = OverrideCategory::Other;
        result.reason = player.hasBowEquipped
            ? std::format("LOW AMMO: {} arrows remaining", rawCount)
            : std::format("LOW AMMO: {} bolts remaining", rawCount);
        result.candidate = std::move(candidate);
        result.targetSlot = 0;

        return result;
    }

    std::optional<OverrideResult> OverrideManager::EvaluateCriticalMagicka(
        const State::PlayerActorState& player)
    {
        const HysteresisConfig config{
            Config::CRITICAL_MAGICKA_THRESHOLD(),
            Config::CRITICAL_MAGICKA_HYSTERESIS(),
            Config::MIN_OVERRIDE_DURATION_MS()
        };

        // Check if magicka is below critical threshold (with hysteresis)
        if (!CheckThresholdHysteresis("CriticalMagicka", player.vitals.magicka, config)) {
            return std::nullopt;
        }

        // Find best magicka potion
        auto candidate = FindMagickaPotion();

        OverrideResult result;
        result.active = true;
        result.priority = Priority::CRITICAL_MAGICKA;
        result.category = OverrideCategory::MP;
        result.reason = "CRITICAL: Need Magicka Potion!";
        result.candidate = std::move(candidate);
        result.targetSlot = 0;

        return result;
    }

    std::optional<OverrideResult> OverrideManager::EvaluateCriticalStamina(
        const State::PlayerActorState& player)
    {
        const HysteresisConfig config{
            Config::CRITICAL_STAMINA_THRESHOLD(),
            Config::CRITICAL_STAMINA_HYSTERESIS(),
            Config::MIN_OVERRIDE_DURATION_MS()
        };

        // Check if stamina is below critical threshold (with hysteresis)
        if (!CheckThresholdHysteresis("CriticalStamina", player.vitals.stamina, config)) {
            return std::nullopt;
        }

        // Find best stamina potion
        auto candidate = FindStaminaPotion();

        OverrideResult result;
        result.active = true;
        result.priority = Priority::CRITICAL_STAMINA;
        result.category = OverrideCategory::SP;
        result.reason = "CRITICAL: Need Stamina Potion!";
        result.candidate = std::move(candidate);
        result.targetSlot = 0;

        return result;
    }

    // =============================================================================
    // ITEM FINDERS
    // =============================================================================

    // Helper: Reject potions with harmful side effects (Skooma, etc.)
    // Emergency overrides should only use pure restore potions.
    static bool HasHarmfulSideEffects(const Item::ItemData& data)
    {
        // Hostile flag covers most cases (poisons, etc.)
        if (data.isHostile) {
            return true;
        }

        // Check primary tags for direct damage effects.
        // Skooma has DamageHealth alongside RestoreStamina but isn't flagged as poison,
        // so isHostile misses it. This catches mixed-effect potions.
        const auto primaryHarmfulMask =
            Item::ItemTag::DamageHealth | Item::ItemTag::DamageMagicka | Item::ItemTag::DamageStamina;

        if (Item::HasTag(data.tags, primaryHarmfulMask)) {
            return true;
        }

        // Check extended tags for debuff effects that can exist on "positive" potions
        // (Ravage, regen damage, elemental weakness)
        const auto extHarmfulMask =
            Item::ItemTagExt::RavageHealth | Item::ItemTagExt::RavageMagicka | Item::ItemTagExt::RavageStamina |
            Item::ItemTagExt::DamageHealthRegen | Item::ItemTagExt::DamageMagickaRegen | Item::ItemTagExt::DamageStaminaRegen |
            Item::ItemTagExt::WeaknessElement;

        return Item::HasTagExt(data.tagsExt, extHarmfulMask);
    }

    // Shared two-pass potion selection: prefer pure, optionally fall back to impure
    static const Item::InventoryItem* FindBestPotionFiltered(
        const std::vector<const Item::InventoryItem*>& potions,
        bool allowImpure)
    {
        const Item::InventoryItem* bestPure = nullptr;
        const Item::InventoryItem* bestAny = nullptr;
        for (const auto* potion : potions) {
            if (potion->count <= 0) continue;
            if (!bestAny) bestAny = potion;  // First available = highest magnitude
            if (!bestPure && !HasHarmfulSideEffects(potion->data)) {
                bestPure = potion;
                break;  // Found a pure one, no need to keep looking
            }
        }
        if (bestPure) return bestPure;
        return allowImpure ? bestAny : nullptr;
    }

    std::optional<Candidate::CandidateVariant> OverrideManager::FindStaminaPotion() const
    {
        if (!m_itemRegistry) {
            logger::debug("[OverrideManager] FindStaminaPotion: ItemRegistry not available"sv);
            return std::nullopt;
        }

        auto potions = m_itemRegistry->GetStaminaPotionsByMagnitude(0);
        const auto* bestPotion = FindBestPotionFiltered(potions, Config::ALLOW_IMPURE_POTIONS());
        static bool s_loggedNoStaminaPotion = false;
        if (!bestPotion) {
            if (!s_loggedNoStaminaPotion) {
                logger::debug("[OverrideManager] FindStaminaPotion: No stamina potions available"sv);
                s_loggedNoStaminaPotion = true;
            }
            return std::nullopt;
        }
        s_loggedNoStaminaPotion = false;  // Reset when potions become available

        // Only log when selected potion changes (suppress per-frame spam)
        static RE::FormID lastLoggedFormID = 0;
        if (bestPotion->data.formID != lastLoggedFormID) {
            bool isPure = !HasHarmfulSideEffects(bestPotion->data);
            logger::info("[OverrideManager] FindStaminaPotion: '{}' FormID={:08X} count={}{}"sv,
                bestPotion->data.name, bestPotion->data.formID, bestPotion->count,
                isPure ? "" : " (has side effects)");
            lastLoggedFormID = bestPotion->data.formID;
        }

        auto candidate = Candidate::ItemCandidate::FromInventoryItem(*bestPotion);
        // Stage 1g: baseRelevance removed - no longer used (utility set directly in ScoredCandidate)
        // candidate.baseRelevance = Config::OVERRIDE_UTILITY_BOOST;
        candidate.relevanceTags = Candidate::RelevanceTag::LowStamina;

        return candidate;
    }

    std::optional<Candidate::CandidateVariant> OverrideManager::FindBestAmmo(bool isBow) const
    {
        if (!m_weaponRegistry) {
            logger::debug("[OverrideManager] FindBestAmmo: WeaponRegistry not available"sv);
            return std::nullopt;
        }

        auto ammoList = isBow ? m_weaponRegistry->GetArrows(1) : m_weaponRegistry->GetBolts(1);

        if (ammoList.empty() || ammoList[0]->count <= 0) {
            logger::debug("[OverrideManager] FindBestAmmo: No {} available"sv,
                isBow ? "arrows" : "bolts");
            return std::nullopt;
        }

        logger::debug("[OverrideManager] FindBestAmmo: Found {} (count={})"sv,
            ammoList[0]->data.name, ammoList[0]->count);

        auto candidate = Candidate::AmmoCandidate::FromInventoryAmmo(*ammoList[0]);
        // Stage 1g: baseRelevance removed - no longer used (utility set directly in ScoredCandidate)
        // candidate.baseRelevance = Config::OVERRIDE_UTILITY_BOOST;
        candidate.relevanceTags = Candidate::RelevanceTag::NeedsAmmo;

        return candidate;
    }

    std::optional<Candidate::CandidateVariant> OverrideManager::FindHealthPotion() const
    {
        if (!m_itemRegistry) {
            logger::debug("[OverrideManager] FindHealthPotion: ItemRegistry not available"sv);
            return std::nullopt;
        }

        auto potions = m_itemRegistry->GetHealthPotionsByMagnitude(0);
        const auto* bestPotion = FindBestPotionFiltered(potions, Config::ALLOW_IMPURE_POTIONS());
        static bool s_loggedNoHealthPotion = false;
        if (!bestPotion) {
            if (!s_loggedNoHealthPotion) {
                logger::debug("[OverrideManager] FindHealthPotion: No health potions available"sv);
                s_loggedNoHealthPotion = true;
            }
            return std::nullopt;
        }
        s_loggedNoHealthPotion = false;  // Reset when potions become available

        // Only log when selected potion changes (suppress per-frame spam)
        static RE::FormID lastLoggedFormID = 0;
        if (bestPotion->data.formID != lastLoggedFormID) {
            bool isPure = !HasHarmfulSideEffects(bestPotion->data);
            logger::info("[OverrideManager] FindHealthPotion: '{}' FormID={:08X} count={}{}"sv,
                bestPotion->data.name, bestPotion->data.formID, bestPotion->count,
                isPure ? "" : " (has side effects)");
            lastLoggedFormID = bestPotion->data.formID;
        }

        auto candidate = Candidate::ItemCandidate::FromInventoryItem(*bestPotion);
        // Stage 1g: baseRelevance removed - no longer used (utility set directly in ScoredCandidate)
        // candidate.baseRelevance = Config::OVERRIDE_UTILITY_BOOST;
        candidate.relevanceTags = Candidate::RelevanceTag::CriticalHealth;

        return candidate;
    }

    std::optional<Candidate::CandidateVariant> OverrideManager::FindMagickaPotion() const
    {
        if (!m_itemRegistry) {
            logger::debug("[OverrideManager] FindMagickaPotion: ItemRegistry not available"sv);
            return std::nullopt;
        }

        auto potions = m_itemRegistry->GetMagickaPotionsByMagnitude(0);
        const auto* bestPotion = FindBestPotionFiltered(potions, Config::ALLOW_IMPURE_POTIONS());
        static bool s_loggedNoMagickaPotion = false;
        if (!bestPotion) {
            if (!s_loggedNoMagickaPotion) {
                logger::debug("[OverrideManager] FindMagickaPotion: No magicka potions available"sv);
                s_loggedNoMagickaPotion = true;
            }
            return std::nullopt;
        }
        s_loggedNoMagickaPotion = false;  // Reset when potions become available

        // Only log when selected potion changes (suppress per-frame spam)
        static RE::FormID lastLoggedFormID = 0;
        if (bestPotion->data.formID != lastLoggedFormID) {
            bool isPure = !HasHarmfulSideEffects(bestPotion->data);
            logger::info("[OverrideManager] FindMagickaPotion: '{}' FormID={:08X} count={}{}"sv,
                bestPotion->data.name, bestPotion->data.formID, bestPotion->count,
                isPure ? "" : " (has side effects)");
            lastLoggedFormID = bestPotion->data.formID;
        }

        auto candidate = Candidate::ItemCandidate::FromInventoryItem(*bestPotion);
        // Stage 1g: baseRelevance removed - no longer used (utility set directly in ScoredCandidate)
        // candidate.baseRelevance = Config::OVERRIDE_UTILITY_BOOST;
        candidate.relevanceTags = Candidate::RelevanceTag::LowMagicka;

        return candidate;
    }

    std::optional<Candidate::CandidateVariant> OverrideManager::FindWaterbreathingItem() const
    {
        // Check for waterbreathing potion in inventory
        // Note: Waterbreathing spells exist but there's no dedicated SpellTag for them.
        // The CandidateGenerator already handles waterbreathing spells via contextual
        // relevance when underwater, so we focus on potions here.
        if (m_itemRegistry) {
            auto potions = m_itemRegistry->GetWaterbreathingPotions(1);
            if (!potions.empty() && potions[0]->count > 0) {
                auto candidate = Candidate::ItemCandidate::FromInventoryItem(*potions[0]);
                // Stage 1g: baseRelevance removed - no longer used (utility set directly in ScoredCandidate)
        // candidate.baseRelevance = Config::OVERRIDE_UTILITY_BOOST;
                candidate.relevanceTags = Candidate::RelevanceTag::Underwater;
                return candidate;
            }
        }

        // If no potion available, return nullopt - waterbreathing spells will still
        // appear via normal scoring if the player has them
        return std::nullopt;
    }

    std::optional<Candidate::CandidateVariant> OverrideManager::FindSoulGem() const
    {
        if (!m_itemRegistry) {
            logger::debug("[OverrideManager] FindSoulGem: ItemRegistry not available"sv);
            return std::nullopt;
        }

        // v0.10.0: GetBestSoulGem now only returns FILLED soul gems
        const auto* bestGem = m_itemRegistry->GetBestSoulGem();
        static bool s_loggedNoSoulGem = false;
        if (!bestGem || bestGem->count <= 0) {
            if (!s_loggedNoSoulGem) {
                logger::debug("[OverrideManager] FindSoulGem: No filled soul gems available"sv);
                s_loggedNoSoulGem = true;
            }
            return std::nullopt;
        }
        s_loggedNoSoulGem = false;  // Reset when gems become available

        logger::debug("[OverrideManager] FindSoulGem: Found {} (capacity={:.0f}, filled={})"sv,
            bestGem->data.name, bestGem->data.magnitude, bestGem->data.isFilled);

        // Convert to ItemCandidate
        auto candidate = Candidate::ItemCandidate::FromInventoryItem(*bestGem);
        // Stage 1g: baseRelevance removed - no longer used (utility set directly in ScoredCandidate)
        // candidate.baseRelevance = Config::OVERRIDE_UTILITY_BOOST;
        candidate.relevanceTags = Candidate::RelevanceTag::WeaponLowCharge;

        return candidate;
    }

    // =============================================================================
    // HYSTERESIS HELPERS
    // =============================================================================

    bool OverrideManager::CheckHysteresis(
        std::string_view name,
        bool conditionMet,
        const HysteresisConfig& config)
    {
        std::string key(name);
        auto& state = m_hysteresisStates[key];

        if (conditionMet) {
            if (!state.isActive) {
                // Activate
                state.isActive = true;
                state.activeDurationMs = 0.0f;
                logger::debug("[OverrideManager] {} activated"sv, name);
            }
            return true;
        }

        // Condition not met - check if we should deactivate
        if (state.isActive) {
            // Stay active if minimum duration not met
            if (state.activeDurationMs < config.minDurationMs) {
                return true;
            }

            // Deactivate
            state.isActive = false;
            state.activeDurationMs = 0.0f;
            logger::debug("[OverrideManager] {} deactivated"sv, name);
        }

        return false;
    }

    bool OverrideManager::CheckThresholdHysteresis(
        std::string_view name,
        float currentValue,
        const HysteresisConfig& config)
    {
        std::string key(name);
        auto& state = m_hysteresisStates[key];

        if (!state.isActive) {
            // Not active - check activation threshold
            if (currentValue < config.activationThreshold) {
                state.isActive = true;
                state.activeDurationMs = 0.0f;
                logger::debug("[OverrideManager] {} activated (value={:.2f} < {:.2f})"sv,
                    name, currentValue, config.activationThreshold);
                return true;
            }
            return false;
        }

        // Currently active - check deactivation threshold
        if (currentValue >= config.deactivationThreshold &&
            state.activeDurationMs >= config.minDurationMs) {
            state.isActive = false;
            state.activeDurationMs = 0.0f;
            logger::debug("[OverrideManager] {} deactivated (value={:.2f} >= {:.2f})"sv,
                name, currentValue, config.deactivationThreshold);
            return false;
        }

        // Stay active
        return true;
    }

    // =============================================================================
    // DIAGNOSTICS
    // =============================================================================

    bool OverrideManager::IsOverrideActive(std::string_view overrideName) const
    {
        std::string key(overrideName);
        auto it = m_hysteresisStates.find(key);
        if (it == m_hysteresisStates.end()) {
            return false;
        }
        return it->second.isActive;
    }

}  // namespace Huginn::Override
