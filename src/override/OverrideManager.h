#pragma once

#include "OverrideConditions.h"
#include "OverrideConfig.h"
#include "state/PlayerActorState.h"
#include "state/WorldState.h"
#include "learning/ScoredCandidate.h"
#include <unordered_map>
#include <string>
#include <string_view>

// Forward declarations
namespace Huginn::Item { class ItemRegistry; }
namespace Huginn::Spell { class SpellRegistry; }
namespace Huginn::Weapon { class WeaponRegistry; }

namespace Huginn::Override
{
    // =============================================================================
    // OVERRIDE MANAGER
    // =============================================================================
    // Priority-based safety override system that forces critical items into
    // recommendation slots when urgent conditions are met.
    //
    // DESIGN PRINCIPLES:
    // - Overrides inject into the scored candidate list, not the widget directly
    // - This ensures consistent behavior across Widget AND Wheeler paths
    // - Hysteresis prevents flickering when values hover near thresholds
    // - Items with overrides get massive utility boost to guarantee top slots
    //
    // INJECTION POINT:
    //   CandidateGenerator -> UtilityScorer -> [OverrideManager] -> Widget/Wheeler
    //
    // USAGE:
    //   auto overrides = overrideMgr.EvaluateOverrides(playerState, worldState);
    //   scoredCandidates = overrideMgr.ApplyOverrides(std::move(scoredCandidates), overrides);
    // =============================================================================

    class OverrideManager
    {
    public:
        static OverrideManager& GetSingleton();

        // Initialize with registry references (call in kPostLoadGame)
        void Initialize(Item::ItemRegistry& itemRegistry, Spell::SpellRegistry& spellRegistry,
                        Weapon::WeaponRegistry& weaponRegistry);

        // Reset state (e.g., on save load)
        void Reset();

        // =============================================================================
        // MAIN API
        // =============================================================================

        /**
         * @brief Evaluate all override conditions against current state
         * @param player Current player state (vitals, buffs, equipment)
         * @param world Current world state (underwater, etc.)
         * @return Collection of active overrides, sorted by priority
         */
        [[nodiscard]] OverrideCollection EvaluateOverrides(
            const State::PlayerActorState& player,
            const State::WorldState& world);

        /**
         * @brief Apply active overrides to scored candidate list
         * @param candidates Scored candidates from UtilityScorer
         * @param overrides Active overrides from EvaluateOverrides
         * @return Modified candidate list with override items at top
         *
         * Override items are injected with boosted utility scores to ensure
         * they appear in the top slots. Original candidates are shifted down.
         */
        [[nodiscard]] Scoring::ScoredCandidateList ApplyOverrides(
            Scoring::ScoredCandidateList candidates,
            const OverrideCollection& overrides);

        /**
         * @brief Update internal timers (call from main update loop)
         * @param deltaMs Milliseconds since last update
         */
        void Update(float deltaMs);

        // =============================================================================
        // DEBUG / DIAGNOSTICS
        // =============================================================================

        /**
         * @brief Check if a specific override is currently active
         * @param overrideName Name of the override ("CriticalHealth", "Drowning", "WeaponCharge")
         */
        [[nodiscard]] bool IsOverrideActive(std::string_view overrideName) const;

        /**
         * @brief Get the count of currently active overrides
         */
        [[nodiscard]] size_t GetActiveOverrideCount() const noexcept { return m_lastActiveCount; }

    private:
        OverrideManager() = default;
        ~OverrideManager() = default;
        OverrideManager(const OverrideManager&) = delete;
        OverrideManager& operator=(const OverrideManager&) = delete;

        // =============================================================================
        // CONDITION EVALUATORS
        // =============================================================================
        // Each evaluator checks one specific override condition and returns
        // an OverrideResult with the appropriate candidate (if available).
        // =============================================================================

        /**
         * @brief Check for critical health condition (HP < 10%)
         * @return OverrideResult with best health potion, or inactive if condition not met
         */
        [[nodiscard]] std::optional<OverrideResult> EvaluateCriticalHealth(
            const State::PlayerActorState& player);

        /**
         * @brief Check for drowning condition (underwater without waterbreathing)
         * @return OverrideResult with waterbreathing item, or inactive if condition not met
         */
        [[nodiscard]] std::optional<OverrideResult> EvaluateDrowning(
            const State::PlayerActorState& player);

        /**
         * @brief Check for empty weapon condition (enchanted weapon at 0% charge)
         * @return OverrideResult with best soul gem, or inactive if condition not met
         */
        [[nodiscard]] std::optional<OverrideResult> EvaluateWeaponCharge(
            const State::PlayerActorState& player);

        /**
         * @brief Check for low ammo condition (bow/crossbow equipped, ammo below threshold)
         * @return OverrideResult with best ammo from inventory, or inactive if condition not met
         */
        [[nodiscard]] std::optional<OverrideResult> EvaluateLowAmmo(
            const State::PlayerActorState& player);

        /**
         * @brief Check for critical magicka condition (MP < 10%)
         * @return OverrideResult with best magicka potion, or inactive if condition not met
         */
        [[nodiscard]] std::optional<OverrideResult> EvaluateCriticalMagicka(
            const State::PlayerActorState& player);

        /**
         * @brief Check for critical stamina condition (SP < 10%)
         * @return OverrideResult with best stamina potion, or inactive if condition not met
         */
        [[nodiscard]] std::optional<OverrideResult> EvaluateCriticalStamina(
            const State::PlayerActorState& player);

        // =============================================================================
        // ITEM FINDERS
        // =============================================================================
        // Query existing registries for the appropriate override item.
        // Returns nullopt if no suitable item is available in inventory.
        // =============================================================================

        /**
         * @brief Find best health potion in inventory
         * @return ItemCandidate for health potion, or nullopt if none available
         */
        [[nodiscard]] std::optional<Candidate::CandidateVariant> FindHealthPotion() const;

        /**
         * @brief Find waterbreathing item (potion or spell)
         * @return CandidateVariant for waterbreathing item, or nullopt if none available
         */
        [[nodiscard]] std::optional<Candidate::CandidateVariant> FindWaterbreathingItem() const;

        /**
         * @brief Find best soul gem for weapon recharging
         * @return ItemCandidate for soul gem, or nullopt if none available
         */
        [[nodiscard]] std::optional<Candidate::CandidateVariant> FindSoulGem() const;

        /**
         * @brief Find best ammo in inventory (arrows or bolts)
         * @param isBow True for arrows (bow), false for bolts (crossbow)
         * @return AmmoCandidate for best ammo, or nullopt if none available
         */
        [[nodiscard]] std::optional<Candidate::CandidateVariant> FindBestAmmo(bool isBow) const;

        /**
         * @brief Find best magicka potion in inventory
         * @return ItemCandidate for magicka potion, or nullopt if none available
         */
        [[nodiscard]] std::optional<Candidate::CandidateVariant> FindMagickaPotion() const;

        /**
         * @brief Find best stamina potion in inventory
         * @return ItemCandidate for stamina potion, or nullopt if none available
         */
        [[nodiscard]] std::optional<Candidate::CandidateVariant> FindStaminaPotion() const;

        // =============================================================================
        // HYSTERESIS HELPERS
        // =============================================================================

        /**
         * @brief Check hysteresis state for a named condition
         * @param name Condition identifier
         * @param conditionMet Raw condition check result
         * @param config Hysteresis configuration
         * @return True if override should be active (considering hysteresis)
         */
        [[nodiscard]] bool CheckHysteresis(
            std::string_view name,
            bool conditionMet,
            const HysteresisConfig& config);

        /**
         * @brief Check hysteresis for threshold-based conditions
         * @param name Condition identifier
         * @param currentValue Current value (e.g., health percentage)
         * @param config Hysteresis configuration with thresholds
         * @return True if override should be active (considering hysteresis)
         */
        [[nodiscard]] bool CheckThresholdHysteresis(
            std::string_view name,
            float currentValue,
            const HysteresisConfig& config);

        // =============================================================================
        // STATE
        // =============================================================================

        // Registry references (set during Initialize)
        Item::ItemRegistry* m_itemRegistry = nullptr;
        Spell::SpellRegistry* m_spellRegistry = nullptr;
        Weapon::WeaponRegistry* m_weaponRegistry = nullptr;

        // Hysteresis tracking for each condition
        std::unordered_map<std::string, HysteresisState> m_hysteresisStates;

        // Last active override count (for diagnostics)
        size_t m_lastActiveCount = 0;

        // Initialization flag
        bool m_initialized = false;
    };

}  // namespace Huginn::Override
