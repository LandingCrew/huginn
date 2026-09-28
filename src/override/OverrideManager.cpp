#include "OverrideManager.h"
#include "learning/item/ItemRegistry.h"
#include "learning/item/ItemData.h"
#include "weapon/WeaponRegistry.h"
#include "spell/SpellRegistry.h"
#include "Globals.h"                   // g_spellRegistry: spell fallback

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

    void OverrideManager::Initialize(Item::ItemRegistry& itemRegistry, Weapon::WeaponRegistry& weaponRegistry)
    {
        m_itemRegistry = &itemRegistry;
        m_weaponRegistry = &weaponRegistry;
        m_initialized = true;

        logger::info("[OverrideManager] Initialized with registries"sv);
    }

    void OverrideManager::Reset()
    {
        m_hysteresisStates.clear();
        m_lastActiveCount = 0;
        m_spellIncumbent.fill(0);

        logger::debug("[OverrideManager] Reset hysteresis states"sv);
    }

    bool OverrideManager::Update(float deltaMs)
    {
        // Guard: Skip if not initialized
        if (!m_initialized) {
            return false;
        }

        // Update active durations for all hysteresis states. Deactivation is
        // decided only inside pipeline runs (CheckHysteresis), so the tick
        // where a latch crosses its minimum-duration window must force a run —
        // otherwise a latched override stays on-screen indefinitely while the
        // pipeline is idle (e.g. critical-health after an insta-heal).
        bool anyLatchClearable = false;
        for (auto& [name, state] : m_hysteresisStates) {
            if (state.isActive) {
                const bool wasBelowMin = state.activeDurationMs < state.minDurationMs;
                state.activeDurationMs += deltaMs;
                if (wasBelowMin && state.activeDurationMs >= state.minDurationMs) {
                    anyLatchClearable = true;
                }
            }
        }
        return anyLatchClearable;
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
            m_spellIncumbent[0] = 0;  // emergency over: the next one picks afresh
            return std::nullopt;
        }

        // Find best health potion
        auto candidate = FindHealthPotion();

        OverrideResult result;
        result.priority = Priority::CRITICAL_HEALTH;
        result.category = OverrideCategory::HP;
        result.condition = OverrideCondition::CriticalHealth;
        result.reason = "CRITICAL: Need Health Potion!";
        result.candidate = std::move(candidate);

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

        // Find waterbreathing potion (spells surface via normal scoring)
        auto candidate = FindWaterbreathingItem();

        OverrideResult result;
        result.priority = Priority::DROWNING;
        result.category = OverrideCategory::Other;
        result.condition = OverrideCondition::Drowning;
        result.reason = "DROWNING: Need Waterbreathing!";
        result.candidate = std::move(candidate);

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

        // Check if weapon charge is depleted (with hysteresis)
        if (!CheckThresholdHysteresis("WeaponCharge", player.weaponChargePercent, config)) {
            return std::nullopt;
        }

        // Find best soul gem
        auto candidate = FindSoulGem();

        // Warn once per episode, not per tick — this evaluator re-runs every
        // pipeline tick while the override is latched
        static bool s_warnedNoSoulGem = false;
        if (!candidate.has_value()) {
            if (!s_warnedNoSoulGem) {
                logger::warn("[OverrideManager] WeaponCharge: No soul gem found in inventory!"sv);
                s_warnedNoSoulGem = true;
            }
        } else {
            s_warnedNoSoulGem = false;
        }

        OverrideResult result;
        result.priority = Priority::WEAPON_EMPTY;
        result.category = OverrideCategory::Other;
        result.condition = OverrideCondition::WeaponCharge;
        result.reason = "WEAPON EMPTY: Need Soul Gem!";
        result.candidate = std::move(candidate);

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

        // Current ammo count. 0 means either an empty quiver or no ammo equipped
        // at all, and LowAmmo wants to fire on both -- there is no negative
        // sentinel, whatever the comment that used to sit here said. It claimed
        // -1, and the `>= 0` guard it justified was load-bearing for the wrong
        // reason: until v0.21.11 these fields held InventoryEntryData::countDelta
        // rather than a count, so a player five arrows into their starting
        // eighteen arrived here at -5 and this line turned it into 0, which then
        // tripped the hysteresis and announced "LOW AMMO: -5 arrows remaining"
        // over a quiver holding thirteen. StateManager answers with a real count
        // now, clamped at its source.
        int32_t rawCount = player.hasBowEquipped ? player.arrowCount : player.boltCount;
        float currentAmmo = static_cast<float>(rawCount);

        // Check threshold with hysteresis
        if (!CheckThresholdHysteresis("LowAmmo", currentAmmo, config)) {
            return std::nullopt;
        }

        // Find best ammo in inventory
        auto candidate = FindBestAmmo(player.hasBowEquipped);

        OverrideResult result;
        result.priority = Priority::LOW_AMMO;
        result.category = OverrideCategory::Other;
        result.condition = OverrideCondition::LowAmmo;
        // 0 with nothing in the quiver is not "0 remaining": the player may be
        // carrying a hundred, just none equipped (2026-09-28 16:02:59).
        const char* ammoWord = player.hasBowEquipped ? "arrows" : "bolts";
        result.reason = player.equippedAmmoFormID == 0
            ? std::format("NO {} EQUIPPED", player.hasBowEquipped ? "ARROWS" : "BOLTS")
            : std::format("LOW AMMO: {} {} remaining", rawCount, ammoWord);
        result.candidate = std::move(candidate);

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
            m_spellIncumbent[1] = 0;  // emergency over: the next one picks afresh
            return std::nullopt;
        }

        // Find best magicka potion
        auto candidate = FindMagickaPotion();

        OverrideResult result;
        result.priority = Priority::CRITICAL_MAGICKA;
        result.category = OverrideCategory::MP;
        result.condition = OverrideCondition::CriticalMagicka;
        result.reason = "CRITICAL: Need Magicka Potion!";
        result.candidate = std::move(candidate);

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
            m_spellIncumbent[2] = 0;  // emergency over: the next one picks afresh
            return std::nullopt;
        }

        // Find best stamina potion
        auto candidate = FindStaminaPotion();

        OverrideResult result;
        result.priority = Priority::CRITICAL_STAMINA;
        result.category = OverrideCategory::SP;
        result.condition = OverrideCondition::CriticalStamina;
        result.reason = "CRITICAL: Need Stamina Potion!";
        result.candidate = std::move(candidate);

        return result;
    }

    // =============================================================================
    // ITEM FINDERS
    // =============================================================================

    // Shared selection policy: prefer pure, optionally fall back to impure
    static const Item::InventoryItem* SelectPotion(
        const Item::ItemRegistry::BestPotionPick& pick,
        bool allowImpure)
    {
        if (pick.pure) return pick.pure;
        return allowImpure ? pick.any : nullptr;
    }

    // Per-finder dedup state so each vitals finder logs independently
    struct PotionLogState
    {
        bool loggedNone = false;      // "No potions available" latch
        RE::FormID lastLogged = 0;    // Last potion logged (suppress per-frame spam)
    };

    // Shared body for the three vitals potion finders (health/magicka/stamina):
    // single-pass registry pick, pure-preferred selection, deduped logging
    // No potion for a critical vital: the best restore SPELL the player can
    // cast on THEMSELVES and afford right now. Deterministic like the potion
    // pick -- no scoring, no learner: the one that restores most within the
    // same urgent window (a concentration spell counts its magnitude per
    // second over the window). Self-delivery only: Heal Other and Healing
    // Hands restore someone else. SpellData carries neither delivery nor
    // magnitude, so the form is looked up here -- only while an override is
    // active with no potion, which is rare.
    static std::optional<Candidate::CandidateVariant> FindRestoreSpell(
        Spell::SpellTag tag,
        RE::ActorValue av,
        std::string_view label,
        Context::ContextReason reason,
        PotionLogState& logState,
        RE::FormID& incumbentSlot)
    {
        if (!g_spellRegistry || g_spellRegistry->IsLoading()) {
            return std::nullopt;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return std::nullopt;
        }
        const float magicka = player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka);
        const float window = Defaults::URGENT_RESTORE_WINDOW_SEC;

        std::optional<Spell::SpellData> best;
        float bestRestored = 0.0f;
        float bestCost = 0.0f;

        // The spell already on the key keeps it while it is still castable.
        // Re-picking the best every pass swapped the emergency key between
        // Healing and Fast Healing every couple of seconds as magicka rose and
        // fell (2026-09-27 20:41:04-12) -- each pick right, the key never
        // still. It changes only when the incumbent becomes unaffordable, or a
        // potion comes back (the finder never gets here then).
        const RE::FormID incumbentID = incumbentSlot;
        std::optional<Spell::SpellData> incumbent;
        float incumbentRestored = 0.0f;
        float incumbentCost = 0.0f;
        g_spellRegistry->ForEachSpell([&](const Spell::SpellData& sd) {
            if (!Spell::HasTag(sd.tags, tag)) return;
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(sd.formID);
            if (!spell || spell->GetDelivery() != RE::MagicSystem::Delivery::kSelf) return;
            float cost = spell->CalculateMagickaCost(player);
            // The engine can report <= 0 for concentration spells; use the
            // base cost then, as CandidateGenerator does -- otherwise a heal
            // the player cannot cast passes as "cost 0" (/code-review, #144).
            if (sd.isConcentration && cost <= 0.0f) {
                cost = static_cast<float>(sd.baseCost);
            }
            if (cost > magicka) return;
            // A concentration spell's cost is per second: credit only the
            // seconds the player can pay for, not the whole window.
            const float castSeconds = (sd.isConcentration && cost > 0.0f)
                ? std::min(window, magicka / cost) : window;

            float restored = 0.0f;
            for (const auto* effect : spell->effects) {
                if (!effect || !effect->baseEffect || effect->baseEffect->IsHostile()) continue;
                if (effect->baseEffect->data.primaryAV != av) continue;
                const float mag = effect->effectItem.magnitude;
                const float dur = static_cast<float>(effect->effectItem.duration);
                restored = std::max(restored,
                    sd.isConcentration ? mag * castSeconds : Item::RestoredWithin(mag, dur, window));
            }
            if (restored > 0.0f && sd.formID == incumbentID) {
                incumbent = sd;
                incumbentRestored = restored;
                incumbentCost = cost;
            }
            if (restored > bestRestored) {
                bestRestored = restored;
                bestCost = cost;
                best = sd;   // a copy: registry entries must not outlive the visit
            }
        });
        if (incumbent) {
            best = incumbent;
            bestRestored = incumbentRestored;
            bestCost = incumbentCost;
        }

        if (!best) {
            return std::nullopt;
        }
        incumbentSlot = best->formID;
        if (best->formID != logState.lastLogged) {
            logger::info("[OverrideManager] {}: no potion, spell '{}' FormID={:08X} (restores {:.0f} in {:.0f}s, cost {:.0f} of {:.0f} magicka)"sv,
                label, best->name, best->formID, bestRestored, window, bestCost, magicka);
            logState.lastLogged = best->formID;
        }

        // Build the candidate from the REGISTRY's entry, not from `best`: a
        // candidate's name is a view into the registry's string, like every
        // other candidate's. Built from the local copy, the view dangled the
        // moment this returned and the health key showed a row of garbage
        // glyphs (2026-09-27 20:04:55, v0.21.37).
        std::optional<Candidate::CandidateVariant> result;
        const RE::FormID bestID = best->formID;
        g_spellRegistry->ForEachSpell([&](const Spell::SpellData& sd) {
            if (result || sd.formID != bestID) return;
            auto candidate = Candidate::SpellCandidate::FromSpellData(sd);
            candidate.effectiveCost = bestCost;
            candidate.overrideReason = reason;
            result = std::move(candidate);
        });
        return result;
    }

    static std::optional<Candidate::CandidateVariant> FindVitalsPotion(
        const Item::ItemRegistry* registry,
        Item::ItemType type,
        std::string_view label,
        Context::ContextReason reason,
        PotionLogState& logState)
    {
        if (!registry) {
            logger::debug("[OverrideManager] {}: ItemRegistry not available"sv, label);
            return std::nullopt;
        }

        const auto pick = registry->GetBestPotion(type, Defaults::URGENT_RESTORE_WINDOW_SEC);
        const auto* bestPotion = SelectPotion(pick, Config::ALLOW_IMPURE_POTIONS());
        if (!bestPotion) {
            if (!logState.loggedNone) {
                logger::debug("[OverrideManager] {}: No potions available"sv, label);
                logState.loggedNone = true;
            }
            return std::nullopt;
        }
        logState.loggedNone = false;  // Reset when potions become available

        // Only log when selected potion changes (suppress per-frame spam)
        if (bestPotion->data.formID != logState.lastLogged) {
            bool isPure = !bestPotion->data.HasHarmfulSideEffects();
            logger::info("[OverrideManager] {}: '{}' FormID={:08X} count={}{}"sv,
                label, bestPotion->data.name, bestPotion->data.formID, bestPotion->count,
                isPure ? "" : " (has side effects)");
            logState.lastLogged = bestPotion->data.formID;
        }

        auto candidate = Candidate::ItemCandidate::FromInventoryItem(*bestPotion);
        candidate.overrideReason = reason;

        return candidate;
    }

    std::optional<Candidate::CandidateVariant> OverrideManager::FindStaminaPotion() const
    {
        // Pick order: potion, then (optionally) a self-cast restore spell.
        static PotionLogState s_logState;
        auto potion = FindVitalsPotion(m_itemRegistry, Item::ItemType::StaminaPotion,
            "FindStaminaPotion"sv, Context::ContextReason::LowStamina, s_logState);
        if (potion || !Config::STAMINA_SPELL_FALLBACK()) {
            m_spellIncumbent[2] = 0;  // a potion (or no fallback) resets the spell
            return potion;
        }
        static PotionLogState s_spellLog;
        return FindRestoreSpell(Spell::SpellTag::RestoreStamina, RE::ActorValue::kStamina,
            "FindStaminaPotion"sv, Context::ContextReason::LowStamina, s_spellLog, m_spellIncumbent[2]);
    }

    std::optional<Candidate::CandidateVariant> OverrideManager::FindBestAmmo(bool isBow) const
    {
        if (!m_weaponRegistry) {
            logger::debug("[OverrideManager] FindBestAmmo: WeaponRegistry not available"sv);
            return std::nullopt;
        }

        // Single-pass max-damage scan (no per-tick allocation/sort)
        const auto* bestAmmo = isBow ? m_weaponRegistry->GetBestArrow()
                                     : m_weaponRegistry->GetBestBolt();

        static bool s_loggedNoAmmo = false;
        if (!bestAmmo || bestAmmo->count <= 0) {
            if (!s_loggedNoAmmo) {
                logger::debug("[OverrideManager] FindBestAmmo: No {} available"sv,
                    isBow ? "arrows" : "bolts");
                s_loggedNoAmmo = true;
            }
            return std::nullopt;
        }
        s_loggedNoAmmo = false;  // Reset when ammo becomes available

        // Only log when selected ammo changes (this runs per-tick while latched)
        static RE::FormID s_lastLoggedAmmo = 0;
        if (bestAmmo->data.formID != s_lastLoggedAmmo) {
            logger::debug("[OverrideManager] FindBestAmmo: Found {} (count={})"sv,
                bestAmmo->data.name, bestAmmo->count);
            s_lastLoggedAmmo = bestAmmo->data.formID;
        }

        auto candidate = Candidate::AmmoCandidate::FromInventoryAmmo(*bestAmmo);
        candidate.overrideReason = Context::ContextReason::NeedsAmmo;

        return candidate;
    }

    std::optional<Candidate::CandidateVariant> OverrideManager::FindHealthPotion() const
    {
        // Pick order: potion, then (optionally) a self-cast restore spell.
        static PotionLogState s_logState;
        auto potion = FindVitalsPotion(m_itemRegistry, Item::ItemType::HealthPotion,
            "FindHealthPotion"sv, Context::ContextReason::CriticalHealth, s_logState);
        if (potion || !Config::HEALTH_SPELL_FALLBACK()) {
            m_spellIncumbent[0] = 0;  // a potion (or no fallback) resets the spell
            return potion;
        }
        static PotionLogState s_spellLog;
        return FindRestoreSpell(Spell::SpellTag::RestoreHealth, RE::ActorValue::kHealth,
            "FindHealthPotion"sv, Context::ContextReason::CriticalHealth, s_spellLog, m_spellIncumbent[0]);
    }

    std::optional<Candidate::CandidateVariant> OverrideManager::FindMagickaPotion() const
    {
        // Pick order: potion, then (optionally) a self-cast restore spell.
        static PotionLogState s_logState;
        auto potion = FindVitalsPotion(m_itemRegistry, Item::ItemType::MagickaPotion,
            "FindMagickaPotion"sv, Context::ContextReason::LowMagicka, s_logState);
        if (potion || !Config::MAGICKA_SPELL_FALLBACK()) {
            m_spellIncumbent[1] = 0;  // a potion (or no fallback) resets the spell
            return potion;
        }
        static PotionLogState s_spellLog;
        return FindRestoreSpell(Spell::SpellTag::RestoreMagicka, RE::ActorValue::kMagicka,
            "FindMagickaPotion"sv, Context::ContextReason::LowMagicka, s_spellLog, m_spellIncumbent[1]);
    }

    std::optional<Candidate::CandidateVariant> OverrideManager::FindWaterbreathingItem() const
    {
        // Check for waterbreathing potion in inventory
        // Note: Waterbreathing spells exist but there's no dedicated SpellTag for them.
        // The CandidateGenerator already handles waterbreathing spells via contextual
        // relevance when underwater, so we focus on potions here.
        if (m_itemRegistry) {
            // Single-pass longest-duration scan (no per-tick allocation/sort)
            const auto* bestPotion = m_itemRegistry->GetBestWaterbreathingPotion();
            if (bestPotion) {
                auto candidate = Candidate::ItemCandidate::FromInventoryItem(*bestPotion);
                candidate.overrideReason = Context::ContextReason::Underwater;
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

        // soul=, not capacity=: magnitude is the SOUL_LEVEL held, which is what
        // decides the recharge. The two coincide for a grand soul in a grand
        // gem, so the old wording read correctly by accident.
        logger::debug("[OverrideManager] FindSoulGem: Found {} (soul={:.0f}, filled={})"sv,
            bestGem->data.name, bestGem->data.magnitude, bestGem->data.isFilled);

        // Convert to ItemCandidate
        auto candidate = Candidate::ItemCandidate::FromInventoryItem(*bestGem);
        candidate.overrideReason = Context::ContextReason::WeaponLowCharge;

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
                state.minDurationMs = config.minDurationMs;
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
                state.minDurationMs = config.minDurationMs;
                logger::debug("[OverrideManager] {} activated (value={:.2f} < {:.2f})"sv,
                    name, currentValue, config.activationThreshold);
                return true;
            }
            return false;
        }

        // Currently active - deactivate only once the value clears the gap
        // above the activation threshold (latch; prevents flapping in the band)
        const float deactivationThreshold = config.activationThreshold + config.hysteresisGap;
        if (currentValue >= deactivationThreshold &&
            state.activeDurationMs >= config.minDurationMs) {
            state.isActive = false;
            state.activeDurationMs = 0.0f;
            logger::debug("[OverrideManager] {} deactivated (value={:.2f} >= {:.2f})"sv,
                name, currentValue, deactivationThreshold);
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
