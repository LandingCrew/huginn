#pragma once

// =============================================================================
// NEED SNAPSHOT -- every sensor reading the need vector is computed from
// =============================================================================
// A plain record: the game fills one per update tick (needs/NeedMonitor.cpp,
// through needs/NeedSnapshotBuilder.cpp) and the need vector is a pure function of it
// (core/NeedEvaluator.h). The same record is written as text
// (core/NeedSnapshotIO.h) by the Debug capture and read back by the host tests
// (tests/core/fixtures/needs/), so a recorded game state replays to a vector
// without the game.
//
// Field names are the names needs.csv's r3_input column uses. Units:
// fractions are 0-1, distances Skyrim units, times real seconds (steady
// clock), amounts actor-value points. NEVER (1e6 s, eleven days) stands for
// "has not happened since the load"; -1 for "no reading" where noted.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>

namespace Huginn::Core::Needs
{
    /// "Never since the load", for the seconds-ago timers.
    inline constexpr float kNever = 1.0e6f;

    /// The decay constant of the per-element damage sums (needs.csv: "sum of
    /// fire HP lost, exponential decay tau~3 s").
    inline constexpr float kDamageDecayTauSec = 3.0f;

    /// Families bit order (needs.csv rows target_humanoid .. target_element_shock).
    enum class Family : std::uint8_t
    {
        Humanoid, Undead, Daedra, Dragon, Construct, Animal, Arthropod, Troll, Giant, Werebeast, Monster,
        Spectral, ElementFire, ElementFrost, ElementShock,
        _Count
    };
    inline constexpr std::uint32_t FamilyBit(Family f) noexcept { return 1u << static_cast<unsigned>(f); }

    /// handSchools bit order (needs.csv rows loadout_destruction .. loadout_illusion).
    enum class School : std::uint8_t { Destruction, Conjuration, Restoration, Alteration, Illusion, _Count };
    inline constexpr std::uint32_t SchoolBit(School s) noexcept { return 1u << static_cast<unsigned>(s); }

    /// workstation values.
    enum class Workstation : int { None = 0, Smithing = 1, Enchanting = 2, Alchemy = 3 };

    struct NeedSnapshot
    {
        // --- Vitals ---------------------------------------------------------
        float health = 1.0f;          // fraction of the effective max
        float magicka = 1.0f;
        float stamina = 1.0f;
        float magickaHeld = 1.0f;     // magicka as the scorer's VitalEnvelope holds it
        float staminaHeld = 1.0f;
        float maxHealth = 100.0f;     // effective max, points
        float maxMagicka = 100.0f;
        float maxStamina = 100.0f;
        float damageRate = 0.0f;      // HP/s (HealthTrackingState)
        float healingRate = 0.0f;     // HP/s
        float magickaUsageRate = 0.0f;  // points/s (MagickaTrackingState)
        float magickaRegenRate = 0.0f;
        float staminaUsageRate = 0.0f;  // points/s (StaminaTrackingState)
        float staminaRegenRate = 0.0f;
        float restoreHealthPending = 0.0f;   // remaining magnitude x duration of over-time restores, points
        float restoreMagickaPending = 0.0f;
        float restoreStaminaPending = 0.0f;

        // --- Elemental threat -----------------------------------------------
        float dmgFire = 0.0f;         // HP lost to the element, summed with decay tau 3 s
        float dmgFrost = 0.0f;
        float dmgShock = 0.0f;
        float dmgMagic = 0.0f;
        float dmgPhysical = 0.0f;
        float castFireAgo = kNever;   // s since a living hostile was seen casting the element
        float castFrostAgo = kNever;
        float castShockAgo = kNever;

        // --- Status ---------------------------------------------------------
        bool poisoned = false;
        bool diseased = false;
        bool drainHealth = false;
        bool drainMagicka = false;
        bool drainStamina = false;
        bool regenSuppressed = false;
        float encumbrance = 0.0f;     // inventory weight / carry weight
        int vampireStage = 0;         // 0 = not a vampire, 1-4

        // --- Survival -------------------------------------------------------
        bool survival = false;        // CC Survival / SMI active
        int hungerStage = 0;          // 0-5 (SMI or CC rank)
        int coldStage = 0;
        int fatigueStage = 0;         // can be negative (rested)
        float hungerRaw = -1.0f;      // CC meter 0-1000, -1 when not read (SMI)
        float coldRaw = -1.0f;
        float fatigueRaw = -1.0f;
        float warmth = 0.0f;          // CC warmth rating

        // --- Combat ---------------------------------------------------------
        bool inCombat = false;        // debounced, as published
        float combatStartAgo = kNever;  // s since in_combat last went true
        float combatEndAgo = kNever;    // s since in_combat last went false
        int enemyCount = 0;           // living hostiles tracked
        int enemiesNear256 = 0;       // living hostiles within 256 units
        float closestEnemy = -1.0f;   // units to the closest living hostile, -1 none
        bool anyCasting = false;      // a living hostile casting (debounced)
        bool targetCaster = false;    // the scoring target has a spell in a hand
        bool targetArcher = false;    // the scoring target has a bow or crossbow equipped
        float targetHealth = -1.0f;   // the scoring target's health fraction, -1 none
        float soleHostileTtk = -1.0f; // s to kill the only hostile at its bar's rate, -1 n/a
        std::uint32_t families = 0;   // Family bits, held for the fight
        bool hostileSummoned = false; // a living hostile is a summon

        // --- Ally -----------------------------------------------------------
        bool followerPresent = false;
        bool followerBleedout = false;  // a follower down on one knee

        // --- Environment ----------------------------------------------------
        float light = 1.0f;           // game light on the player, 0-1
        bool openDaylight = false;    // outdoors, full sky, climate daytime
        bool underwater = false;
        float submergedFor = 0.0f;    // s under water (0 when not)
        bool swimming = false;
        float fallDepth = 0.0f;       // units below the take-off point
        float dropAhead = -1.0f;      // units from the feet down to the surface ahead, -1 not measured
        float waterDepthAhead = -1.0f;  // deepest water under the drop probes, units; 0 none, -1 not measured
        bool lock = false;            // crosshair on a locked object
        int workstation = 0;          // Workstation
        bool oreVein = false;
        bool merchant = false;        // crosshair on an actor who offers services

        // --- Equipment ------------------------------------------------------
        bool enchantedWeapon = false;
        float weaponCharge = 1.0f;    // fraction
        bool bow = false;
        bool crossbow = false;
        int arrows = 0;               // equipped ammo count
        int bolts = 0;
        bool melee = false;
        bool spell = false;
        bool staff = false;
        bool shield = false;
        bool oneHanded = false;
        bool twoHanded = false;
        std::uint32_t handSchools = 0;  // School bits of the spells in either hand

        // --- Activity -------------------------------------------------------
        bool sneaking = false;
        bool mounted = false;
    };

    // ------------------------------------------------------------------------
    // Field table: name <-> member, for the text record (NeedSnapshotIO) and
    // for the tests. Order is the struct's.
    // ------------------------------------------------------------------------
    using FieldPtr = std::variant<float NeedSnapshot::*, int NeedSnapshot::*, bool NeedSnapshot::*,
                                  std::uint32_t NeedSnapshot::*>;

    struct Field
    {
        std::string_view name;
        FieldPtr ptr;
    };

#define HUGINN_NEED_FIELD(f) Field{ #f, &NeedSnapshot::f }
    inline const auto kFields = std::to_array<Field>({
        HUGINN_NEED_FIELD(health), HUGINN_NEED_FIELD(magicka), HUGINN_NEED_FIELD(stamina),
        HUGINN_NEED_FIELD(magickaHeld), HUGINN_NEED_FIELD(staminaHeld),
        HUGINN_NEED_FIELD(maxHealth), HUGINN_NEED_FIELD(maxMagicka), HUGINN_NEED_FIELD(maxStamina),
        HUGINN_NEED_FIELD(damageRate), HUGINN_NEED_FIELD(healingRate),
        HUGINN_NEED_FIELD(magickaUsageRate), HUGINN_NEED_FIELD(magickaRegenRate),
        HUGINN_NEED_FIELD(staminaUsageRate), HUGINN_NEED_FIELD(staminaRegenRate),
        HUGINN_NEED_FIELD(restoreHealthPending), HUGINN_NEED_FIELD(restoreMagickaPending),
        HUGINN_NEED_FIELD(restoreStaminaPending),
        HUGINN_NEED_FIELD(dmgFire), HUGINN_NEED_FIELD(dmgFrost), HUGINN_NEED_FIELD(dmgShock),
        HUGINN_NEED_FIELD(dmgMagic), HUGINN_NEED_FIELD(dmgPhysical),
        HUGINN_NEED_FIELD(castFireAgo), HUGINN_NEED_FIELD(castFrostAgo), HUGINN_NEED_FIELD(castShockAgo),
        HUGINN_NEED_FIELD(poisoned), HUGINN_NEED_FIELD(diseased), HUGINN_NEED_FIELD(drainHealth),
        HUGINN_NEED_FIELD(drainMagicka), HUGINN_NEED_FIELD(drainStamina), HUGINN_NEED_FIELD(regenSuppressed),
        HUGINN_NEED_FIELD(encumbrance), HUGINN_NEED_FIELD(vampireStage),
        HUGINN_NEED_FIELD(survival), HUGINN_NEED_FIELD(hungerStage), HUGINN_NEED_FIELD(coldStage),
        HUGINN_NEED_FIELD(fatigueStage), HUGINN_NEED_FIELD(hungerRaw), HUGINN_NEED_FIELD(coldRaw),
        HUGINN_NEED_FIELD(fatigueRaw), HUGINN_NEED_FIELD(warmth),
        HUGINN_NEED_FIELD(inCombat), HUGINN_NEED_FIELD(combatStartAgo), HUGINN_NEED_FIELD(combatEndAgo),
        HUGINN_NEED_FIELD(enemyCount), HUGINN_NEED_FIELD(enemiesNear256), HUGINN_NEED_FIELD(closestEnemy),
        HUGINN_NEED_FIELD(anyCasting), HUGINN_NEED_FIELD(targetCaster), HUGINN_NEED_FIELD(targetArcher),
        HUGINN_NEED_FIELD(targetHealth), HUGINN_NEED_FIELD(soleHostileTtk), HUGINN_NEED_FIELD(families),
        HUGINN_NEED_FIELD(hostileSummoned),
        HUGINN_NEED_FIELD(followerPresent), HUGINN_NEED_FIELD(followerBleedout),
        HUGINN_NEED_FIELD(light), HUGINN_NEED_FIELD(openDaylight), HUGINN_NEED_FIELD(underwater),
        HUGINN_NEED_FIELD(submergedFor), HUGINN_NEED_FIELD(swimming), HUGINN_NEED_FIELD(fallDepth),
        HUGINN_NEED_FIELD(dropAhead), HUGINN_NEED_FIELD(waterDepthAhead), HUGINN_NEED_FIELD(lock),
        HUGINN_NEED_FIELD(workstation), HUGINN_NEED_FIELD(oreVein), HUGINN_NEED_FIELD(merchant),
        HUGINN_NEED_FIELD(enchantedWeapon), HUGINN_NEED_FIELD(weaponCharge), HUGINN_NEED_FIELD(bow),
        HUGINN_NEED_FIELD(crossbow), HUGINN_NEED_FIELD(arrows), HUGINN_NEED_FIELD(bolts),
        HUGINN_NEED_FIELD(melee), HUGINN_NEED_FIELD(spell), HUGINN_NEED_FIELD(staff), HUGINN_NEED_FIELD(shield),
        HUGINN_NEED_FIELD(oneHanded), HUGINN_NEED_FIELD(twoHanded), HUGINN_NEED_FIELD(handSchools),
        HUGINN_NEED_FIELD(sneaking), HUGINN_NEED_FIELD(mounted),
    });
#undef HUGINN_NEED_FIELD
}
