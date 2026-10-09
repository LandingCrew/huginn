#include "NeedEvaluator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace Huginn::Core::Needs
{
    namespace
    {
        float B(bool v) noexcept { return v ? 1.0f : 0.0f; }

        float PerMax(float amount, float max) noexcept { return max > 0.0f ? amount / max : 0.0f; }

        // Remaining over-time restore against the points missing. Under one
        // point missing there is nothing to cover: 0, not "covered" (0.23.16;
        // it read 1.00 at full health with any restore ticking, the old
        // deficit floor of 1 point turning every pending restore into 10).
        float Pending(float pending, float fraction, float max) noexcept
        {
            if (!(pending > 0.0f)) return 0.0f;
            const float deficit = (1.0f - fraction) * max;
            if (!(deficit >= 1.0f)) return 0.0f;
            return std::min(pending / deficit, 10.0f);
        }

        float Survival(const NeedSnapshot& s, float raw, int stage) noexcept
        {
            if (!s.survival) return 0.0f;
            if (raw >= 0.0f) return raw / 1000.0f;
            return static_cast<float>(std::clamp(stage, 0, 5)) / 5.0f;
        }

        float Distance(const NeedSnapshot& s) noexcept { return s.closestEnemy < 0.0f ? 4096.0f : s.closestEnemy; }

        float FamilyIn(const NeedSnapshot& s, Family f) noexcept { return B((s.families & FamilyBit(f)) != 0); }

        float SchoolIn(const NeedSnapshot& s, School sc) noexcept { return B((s.handSchools & SchoolBit(sc)) != 0); }

        constexpr float kAmmoCap = 25.0f;        // needs.csv ammo_low: "1 - count / cap (cap~25)"
        constexpr float kBreathSec = 20.0f;      // needs.csv underwater: "seconds submerged / ~breath time"
        constexpr float kWarmthHigh = 200.0f;    // WarmthThreshold::HIGH (StateConstants.h)
        constexpr int kSunVulnerableStage = 3;   // VampireThreshold::SUN_VULNERABLE_STAGE

        // needs.csv r3_input, one case per need. Keep the order of the csv.
        float InputFor(NeedId id, const NeedSnapshot& s) noexcept
        {
            using N = NeedId;
            switch (id) {
                // --- Vitals
                case N::health_deficit: return 1.0f - s.health;
                case N::magicka_deficit: return 1.0f - s.magickaHeld;
                case N::stamina_deficit: return 1.0f - s.staminaHeld;
                case N::health_falling: return PerMax(s.damageRate - s.healingRate, s.maxHealth);
                case N::magicka_burn: return PerMax(s.magickaUsageRate - s.magickaRegenRate, s.maxMagicka);
                case N::stamina_burn: return PerMax(s.staminaUsageRate - s.staminaRegenRate, s.maxStamina);
                case N::restore_pending_health: return Pending(s.restoreHealthPending, s.health, s.maxHealth);
                case N::restore_pending_magicka: return Pending(s.restoreMagickaPending, s.magicka, s.maxMagicka);
                case N::restore_pending_stamina: return Pending(s.restoreStaminaPending, s.stamina, s.maxStamina);
                // --- Elemental threat
                case N::fire_damage_rate: return PerMax(s.dmgFire, s.maxHealth);
                case N::frost_damage_rate: return PerMax(s.dmgFrost, s.maxHealth);
                case N::shock_damage_rate: return PerMax(s.dmgShock, s.maxHealth);
                case N::magic_damage_rate: return PerMax(s.dmgMagic, s.maxHealth);
                case N::physical_damage_rate: return PerMax(s.dmgPhysical, s.maxHealth);
                case N::enemy_casting_fire: return s.castFireAgo;
                case N::enemy_casting_frost: return s.castFrostAgo;
                case N::enemy_casting_shock: return s.castShockAgo;
                // --- Status
                case N::poisoned: return B(s.poisoned);
                case N::diseased: return B(s.diseased);
                case N::drain_health: return B(s.drainHealth);
                case N::drain_magicka: return B(s.drainMagicka);
                case N::drain_stamina: return B(s.drainStamina);
                case N::regen_suppressed: return B(s.regenSuppressed);
                case N::healing_blocked: return 0.0f;  // deferred
                case N::encumbrance: return s.encumbrance;
                case N::vampire_sun_exposure: return B(s.vampireStage >= kSunVulnerableStage && s.openDaylight);
                // --- Survival
                case N::hunger: return Survival(s, s.hungerRaw, s.hungerStage);
                case N::thirst: return 0.0f;  // deferred
                case N::cold: return Survival(s, s.coldRaw, s.coldStage);
                case N::fatigue: return Survival(s, s.fatigueRaw, s.fatigueStage);
                case N::warmth_deficit:
                    return s.survival ? std::clamp(1.0f - s.warmth / kWarmthHigh, 0.0f, 1.0f) : 0.0f;
                // --- Enemy
                case N::in_combat: return B(s.inCombat);
                case N::combat_onset: return s.inCombat ? s.combatStartAgo : kNever;
                case N::enemy_count: return static_cast<float>(s.enemyCount) / 6.0f;
                case N::enemy_close: return Distance(s);
                case N::enemy_mid: return Distance(s);
                case N::enemy_far: return s.closestEnemy < 0.0f ? 0.0f : s.closestEnemy;   // no hostile: 0, not far
                case N::surrounded: return static_cast<float>(s.enemiesNear256) / 3.0f;
                case N::enemy_casting: return B(s.anyCasting);
                case N::target_caster: return B(s.targetCaster);
                case N::target_archer: return B(s.targetArcher);
                case N::target_health_low: return s.targetHealth < 0.0f ? 0.0f : 1.0f - s.targetHealth;
                case N::boss_fight: return (s.enemyCount == 1 && s.soleHostileTtk >= 0.0f) ? s.soleHostileTtk : 0.0f;
                case N::target_magicka_low: return 0.0f;  // deferred (perception line)
                case N::target_stamina_low: return 0.0f;  // deferred (perception line)
                // --- Target type
                case N::target_humanoid: return FamilyIn(s, Family::Humanoid);
                case N::target_undead: return FamilyIn(s, Family::Undead);
                case N::target_daedra: return FamilyIn(s, Family::Daedra);
                case N::target_dragon: return FamilyIn(s, Family::Dragon);
                case N::target_construct: return FamilyIn(s, Family::Construct);
                case N::target_animal: return FamilyIn(s, Family::Animal);
                case N::target_arthropod: return FamilyIn(s, Family::Arthropod);
                case N::target_troll: return FamilyIn(s, Family::Troll);
                case N::target_giant: return FamilyIn(s, Family::Giant);
                case N::target_werebeast: return FamilyIn(s, Family::Werebeast);
                case N::target_monster: return FamilyIn(s, Family::Monster);
                case N::target_spectral: return FamilyIn(s, Family::Spectral);
                case N::target_element_fire: return FamilyIn(s, Family::ElementFire);
                case N::target_element_frost: return FamilyIn(s, Family::ElementFrost);
                case N::target_element_shock: return FamilyIn(s, Family::ElementShock);
                case N::target_summoned: return B(s.hostileSummoned);
                // --- Ally
                case N::ally_injured: return B(s.followerBleedout);
                case N::follower_present: return B(s.followerPresent);
                // --- Environment
                case N::darkness: return s.openDaylight ? 0.0f : 1.0f - s.light;
                case N::underwater: return s.underwater ? s.submergedFor / kBreathSec : 0.0f;
                case N::swimming: return B(s.swimming);
                case N::falling: return s.fallDepth;
                case N::drop_ahead: return s.dropAhead < 0.0f ? 0.0f : s.dropAhead;
                case N::deep_water_ahead: return s.waterDepthAhead < 0.0f ? 0.0f : s.waterDepthAhead;
                case N::lock_in_crosshair: return B(s.lock);
                case N::workstation_smithing: return B(s.workstation == static_cast<int>(Workstation::Smithing));
                case N::workstation_enchanting: return B(s.workstation == static_cast<int>(Workstation::Enchanting));
                case N::workstation_alchemy: return B(s.workstation == static_cast<int>(Workstation::Alchemy));
                case N::ore_vein_in_crosshair: return B(s.oreVein);
                case N::merchant_in_crosshair: return B(s.merchant);
                // --- Equipment
                case N::weapon_charge_deficit: return s.enchantedWeapon ? 1.0f - s.weaponCharge : 0.0f;
                case N::ammo_low:
                    if (s.bow) return std::max(0.0f, 1.0f - static_cast<float>(s.arrows) / kAmmoCap);
                    if (s.crossbow) return std::max(0.0f, 1.0f - static_cast<float>(s.bolts) / kAmmoCap);
                    return 0.0f;
                case N::hands_empty: return B(!(s.melee || s.bow || s.crossbow || s.spell || s.staff));
                // --- Loadout
                case N::loadout_one_handed: return B(s.oneHanded);
                case N::loadout_two_handed: return B(s.twoHanded);
                case N::loadout_archery: return B(s.bow || s.crossbow);
                case N::loadout_staff: return B(s.staff);
                case N::loadout_shield: return B(s.shield);
                case N::loadout_destruction: return SchoolIn(s, School::Destruction);
                case N::loadout_conjuration: return SchoolIn(s, School::Conjuration);
                case N::loadout_restoration: return SchoolIn(s, School::Restoration);
                case N::loadout_alteration: return SchoolIn(s, School::Alteration);
                case N::loadout_illusion: return SchoolIn(s, School::Illusion);
                // --- Activity
                case N::sneaking: return B(s.sneaking);
                case N::sneak_detected: return 0.0f;  // deferred
                case N::mounted: return B(s.mounted);
                case N::downtime: return (!s.inCombat && s.enemyCount == 0) ? s.combatEndAgo : 0.0f;
                case N::combat_ended_recent: return !s.inCombat ? s.combatEndAgo : kNever;
                case N::_Count: break;
            }
            return std::numeric_limits<float>::quiet_NaN();  // a need with no case: the tests catch it
        }
    }

    CurveTable DefaultCurves() noexcept
    {
        CurveTable t{};
        for (std::size_t i = 0; i < kNeedCount; ++i) t[i] = kNeeds[i].curve;
        return t;
    }

    NeedArray NeedInputs(const NeedSnapshot& s) noexcept
    {
        NeedArray in{};
        for (std::size_t i = 0; i < kNeedCount; ++i) in[i] = InputFor(static_cast<NeedId>(i), s);
        return in;
    }

    NeedVector EvaluateNeeds(const NeedSnapshot& s, const CurveTable& curves) noexcept
    {
        NeedVector v;
        v.input = NeedInputs(s);
        for (std::size_t i = 0; i < kNeedCount; ++i) v.value[i] = Evaluate(curves[i], v.input[i]);
        return v;
    }

    std::uint8_t SignatureLevel(float v) noexcept
    {
        if (!(v > 0.0f)) return 0;
        const float level = std::floor(v * static_cast<float>(kSignatureLevels) + 0.5f);
        return static_cast<std::uint8_t>(std::clamp(level, 0.0f, static_cast<float>(kSignatureLevels)));
    }

    NeedSignature Signature(const NeedArray& values) noexcept
    {
        NeedSignature sig{};
        for (std::size_t i = 0; i < kNeedCount; ++i) sig[i] = SignatureLevel(values[i]);
        return sig;
    }

    bool SignatureMoved(const NeedSignature& logged, const NeedSignature& now, int levels) noexcept
    {
        for (std::size_t i = 0; i < kNeedCount; ++i) {
            if ((logged[i] == 0) != (now[i] == 0)) return true;
            if (std::abs(static_cast<int>(now[i]) - static_cast<int>(logged[i])) >= levels) return true;
        }
        return false;
    }

    NeedSnapshot Advance(const NeedSnapshot& s, float dtSec) noexcept
    {
        NeedSnapshot a = s;
        if (!(dtSec > 0.0f)) return a;
        const auto later = [dtSec](float ago) { return std::min(ago + dtSec, 2.0f * kNever); };
        a.castFireAgo = later(s.castFireAgo);
        a.castFrostAgo = later(s.castFrostAgo);
        a.castShockAgo = later(s.castShockAgo);
        a.combatStartAgo = later(s.combatStartAgo);
        a.combatEndAgo = later(s.combatEndAgo);
        if (s.underwater) a.submergedFor = later(s.submergedFor);
        const float k = std::exp(-dtSec / kDamageDecayTauSec);
        a.dmgFire = s.dmgFire * k;
        a.dmgFrost = s.dmgFrost * k;
        a.dmgShock = s.dmgShock * k;
        a.dmgMagic = s.dmgMagic * k;
        a.dmgPhysical = s.dmgPhysical * k;
        return a;
    }

    bool TimeDriven(const NeedSnapshot& s, const CurveTable& curves) noexcept
    {
        const auto now = Signature(EvaluateNeeds(s, curves).value);
        const auto settled = Signature(EvaluateNeeds(Advance(s, kNever), curves).value);
        return now != settled;
    }
}
