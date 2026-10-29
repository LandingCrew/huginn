#pragma once

#include "context/ContextReason.h"
#include "core/NeedIds.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Huginn::Display
{
    // =========================================================================
    // NEED REASON (R8.3) -- an explanation's need as a context reason
    // =========================================================================
    // From R8.3 the label and the reason hold read each item's explanation:
    // the largest theta[k][j] n_k a_j term of its choice score (core/
    // ChoiceScore.h). Its need k says why the item ranks where it does; this
    // table names that need with the old vocabulary (Context::ContextReason,
    // worded by ReasonLabel), so the subtexts read as before. A need with no
    // reason here (a loadout, combat timing, the target's type beyond the
    // three the old labels had) gives none, and the label ladder falls
    // through to "Ally Support" / "Favorite" / nothing.
    //
    // A need speaks only when it is ON: n_k >= kReasonNeedOn, the opportunity
    // counter's onset level (core/ChoiceUpdate.h). Without it a term of 0.04 x
    // a large theta could label a potion "Low HP" at 95% health.
    // =========================================================================

    inline constexpr double kReasonNeedOn = 0.5;

    [[nodiscard]] constexpr Context::ContextReason ReasonOfNeed(std::string_view need) noexcept
    {
        using R = Context::ContextReason;
        struct Pair { std::string_view need; R reason; };
        constexpr Pair kTable[] = {
            { "health_deficit", R::LowHealth },      { "health_falling", R::LowHealth },
            { "drain_health", R::LowHealth },        { "magicka_deficit", R::LowMagicka },
            { "magicka_burn", R::LowMagicka },       { "drain_magicka", R::LowMagicka },
            { "stamina_deficit", R::LowStamina },    { "stamina_burn", R::LowStamina },
            { "drain_stamina", R::LowStamina },      { "fire_damage_rate", R::OnFire },
            { "frost_damage_rate", R::TakingFrost }, { "shock_damage_rate", R::TakingShock },
            { "enemy_casting_fire", R::EnemyCasting }, { "enemy_casting_frost", R::EnemyCasting },
            { "enemy_casting_shock", R::EnemyCasting }, { "enemy_casting", R::EnemyCasting },
            { "poisoned", R::Poisoned },             { "diseased", R::Diseased },
            { "cold", R::Cold },                     { "warmth_deficit", R::Cold },
            { "hunger", R::Hungry },                 { "enemy_count", R::MultipleEnemies },
            { "surrounded", R::MultipleEnemies },    { "target_undead", R::TargetUndead },
            { "target_daedra", R::TargetDaedra },    { "target_dragon", R::TargetDragon },
            { "ally_injured", R::AllyInjured },      { "darkness", R::InDarkness },
            { "underwater", R::Underwater },         { "falling", R::Falling },
            { "lock_in_crosshair", R::LookingAtLock }, { "workstation_smithing", R::AtForge },
            { "workstation_enchanting", R::AtEnchanter }, { "workstation_alchemy", R::AtAlchemy },
            { "ore_vein_in_crosshair", R::LookingAtOre }, { "weapon_charge_deficit", R::WeaponLowCharge },
            { "ammo_low", R::NeedsAmmo },            { "sneaking", R::Sneaking },
        };
        for (const auto& p : kTable) {
            if (p.need == need) return p.reason;
        }
        return R::None;
    }

    /// The reason for need index k (the build's order), built once.
    [[nodiscard]] inline Context::ContextReason ReasonOfNeedIndex(std::size_t k) noexcept
    {
        static const auto kByIndex = [] {
            std::array<Context::ContextReason, Core::Needs::kNeedCount> a{};
            for (std::size_t i = 0; i < a.size(); ++i) a[i] = ReasonOfNeed(Core::Needs::kNeeds[i].id);
            return a;
        }();
        return k < kByIndex.size() ? kByIndex[k] : Context::ContextReason::None;
    }

    /// An explanation's reason: its need (-1 = "learned": none), when on.
    [[nodiscard]] inline Context::ContextReason ExplainedReason(int16_t need, double n) noexcept
    {
        if (need < 0 || !(n >= kReasonNeedOn)) return Context::ContextReason::None;
        return ReasonOfNeedIndex(static_cast<std::size_t>(need));
    }
}
