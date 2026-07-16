#pragma once

#include "OverrideConditions.h"
#include "OverrideConfig.h"
#include "state/PlayerActorState.h"
#include "state/WorldState.h"
#include <unordered_map>
#include <string>
#include <string_view>

// Forward declarations
namespace Huginn::Item { class ItemRegistry; }
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
    // - EvaluateOverrides() reports which conditions are urgent right now as an
    //   OverrideCollection; it does not touch the scored candidate list.
    // - The pipeline hands that collection to SlotAllocator, which places the
    //   override items into type-matched slots, and to SlotLocker, which lets
    //   high-priority overrides break existing slot locks. It also flows into
    //   DisplayContext, where the display/Wheeler path uses GetTopOverride()
    //   for urgent auto-focus and subtext labels.
    // - Hysteresis prevents flickering when values hover near thresholds.
    //
    // FLOW:
    //   EvaluateOverrides -> OverrideCollection -> SlotAllocator / SlotLocker
    //                                              -> DisplayContext (backends)
    //
    // USAGE:
    //   auto overrides = overrideMgr.EvaluateOverrides(playerState, worldState);
    //   slotAllocator.AllocateSlots(candidates, overrides, ...);
    // =============================================================================

    class OverrideManager
    {
    public:
        static OverrideManager& GetSingleton();

        // Initialize with registry references (call in kPostLoadGame)
        void Initialize(Item::ItemRegistry& itemRegistry, Weapon::WeaponRegistry& weaponRegistry);

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
         * @brief Update internal timers (call from main update loop)
         * @param deltaMs Milliseconds since last update
         * @return true if an active latch crossed its minimum-duration window
         *         this tick — deactivation is decided only inside pipeline runs,
         *         so the caller must force one (MarkPageDirty) or the latch
         *         lingers on-screen while the pipeline is idle
         */
        [[nodiscard]] bool Update(float deltaMs);

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
         * @brief Find waterbreathing potion (spells surface via normal scoring)
         * @return CandidateVariant for waterbreathing potion, or nullopt if none available
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
        Weapon::WeaponRegistry* m_weaponRegistry = nullptr;

        // Hysteresis tracking for each condition
        std::unordered_map<std::string, HysteresisState> m_hysteresisStates;

        // Last active override count (for diagnostics)
        size_t m_lastActiveCount = 0;

        // Initialization flag
        bool m_initialized = false;
    };

}  // namespace Huginn::Override
