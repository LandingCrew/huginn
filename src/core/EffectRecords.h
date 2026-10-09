#pragma once

// =============================================================================
// EFFECT RECORDS -- plain copies of the game data the effect mapper reads
// =============================================================================
// The reader (src/effect/EffectReader.cpp) fills these from game forms at
// kDataLoaded; the host tests and tools/effects fill them from `hg dump all`
// CSV rows. The mapper (core/EffectMapper.h) never sees a game type.
//
// Actor values are carried by their enum NAME ("Health", "FireResist",
// "OneHandedSkillAdvance"), as the dumps print them, not by number: the names
// are what the mapping rules in effects.csv are written in, and a dump row and
// a live form then reach the mapper in the same form.
//
// Engine numbers kept as plain ints (their meaning is the engine's):
//   archetype     MGEF archetype, 0 ValueModifier .. 46 VampireLord (kArch*)
//   delivery      0 Self, 1 Touch, 2 Aimed, 3 TargetActor, 4 TargetLocation
//   castingType   0 ConstantEffect, 1 FireAndForget, 2 Concentration, 3 Scroll
//   spellType     0 Spell, 2 Power, 3 LesserPower, 4 Ability, 11 Voice ...
//   weaponType    0 HandToHand, 1 Sword, 2 Dagger, 3 WarAxe, 4 Mace,
//                 5 Greatsword, 6 Battleaxe/Warhammer, 7 Bow, 8 Staff, 9 Crossbow
//   flags         MGEF data flags: 0x1 Hostile, 0x2 Recover, 0x4 Detrimental,
//                 0x8000 HideInUI
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Huginn::Core::Effect
{
    // MGEF archetypes the rules name (the engine's numbering).
    inline constexpr int kArchValueModifier = 0;
    inline constexpr int kArchScript = 1;
    inline constexpr int kArchDispel = 2;
    inline constexpr int kArchCureDisease = 3;
    inline constexpr int kArchAbsorb = 4;
    inline constexpr int kArchDualValueModifier = 5;
    inline constexpr int kArchCalm = 6;
    inline constexpr int kArchDemoralize = 7;
    inline constexpr int kArchFrenzy = 8;
    inline constexpr int kArchDisarm = 9;
    inline constexpr int kArchCommandSummoned = 10;
    inline constexpr int kArchInvisibility = 11;
    inline constexpr int kArchLight = 12;
    inline constexpr int kArchDarkness = 13;
    inline constexpr int kArchNightEye = 14;
    inline constexpr int kArchLock = 15;
    inline constexpr int kArchOpen = 16;
    inline constexpr int kArchBoundWeapon = 17;
    inline constexpr int kArchSummonCreature = 18;
    inline constexpr int kArchDetectLife = 19;
    inline constexpr int kArchTelekinesis = 20;
    inline constexpr int kArchParalysis = 21;
    inline constexpr int kArchReanimate = 22;
    inline constexpr int kArchSoulTrap = 23;
    inline constexpr int kArchTurnUndead = 24;
    inline constexpr int kArchGuide = 25;
    inline constexpr int kArchWerewolfFeed = 26;
    inline constexpr int kArchCureParalysis = 27;
    inline constexpr int kArchCureAddiction = 28;
    inline constexpr int kArchCurePoison = 29;
    inline constexpr int kArchConcussion = 30;
    inline constexpr int kArchValueAndParts = 31;
    inline constexpr int kArchAccumulateMagnitude = 32;
    inline constexpr int kArchStagger = 33;
    inline constexpr int kArchPeakValueModifier = 34;
    inline constexpr int kArchCloak = 35;
    inline constexpr int kArchWerewolf = 36;
    inline constexpr int kArchSlowTime = 37;
    inline constexpr int kArchRally = 38;
    inline constexpr int kArchEnhanceWeapon = 39;
    inline constexpr int kArchSpawnHazard = 40;
    inline constexpr int kArchEtherealize = 41;
    inline constexpr int kArchBanish = 42;
    inline constexpr int kArchSpawnScriptedRef = 43;
    inline constexpr int kArchDisguise = 44;
    inline constexpr int kArchGrabActor = 45;
    inline constexpr int kArchVampireLord = 46;

    inline constexpr std::uint32_t kFlagHostile = 0x1;
    inline constexpr std::uint32_t kFlagRecover = 0x2;
    inline constexpr std::uint32_t kFlagDetrimental = 0x4;
    inline constexpr std::uint32_t kFlagHideInUI = 0x8000;

    inline constexpr int kDeliverySelf = 0;
    inline constexpr int kDeliveryTouch = 1;
    inline constexpr int kDeliveryAimed = 2;
    inline constexpr int kDeliveryTargetActor = 3;
    inline constexpr int kDeliveryTargetLocation = 4;

    inline constexpr int kCastConstant = 0;
    inline constexpr int kCastFireAndForget = 1;
    inline constexpr int kCastConcentration = 2;

    inline constexpr int kSpellTypeSpell = 0;

    inline constexpr int kWeaponHandToHand = 0;
    inline constexpr int kWeaponBow = 7;
    inline constexpr int kWeaponStaff = 8;
    inline constexpr int kWeaponCrossbow = 9;

    /// One effect item on an item, or in a carried spell (a Cloak's or a
    /// hazard's payload). `effect` indexes the MagicEffectRecord table.
    struct EffectRow
    {
        std::uint32_t effect = 0;
        float magnitude = 0.0f;
        std::uint32_t duration = 0;  // seconds
        std::uint32_t area = 0;      // feet
        float cost = 0.0f;           // the effect item's own cost (dump gap 4); 0 if unknown
        std::uint32_t index = 0;     // position in the form's effect list (the dump's effectIndex)
    };

    /// One magic effect (MGEF) -- what is the same wherever it is used.
    struct MagicEffectRecord
    {
        std::uint32_t formId = 0;
        std::string plugin;          // defining plugin (override key)
        std::string name;
        int archetype = -1;
        std::string primaryAV;       // actor value enum names, "" for none
        std::string secondaryAV;
        std::string resistAV;
        int delivery = 0;
        int castingType = 0;
        float baseCost = 0.0f;
        std::uint32_t flags = 0;
        bool detrimental = false;
        bool hostile = false;
        std::vector<std::string> keywords;  // editor IDs
        std::string description;     // the game's text, <mag>/<dur> unfilled
        std::string school;          // associatedSkill enum name, "" if none or unknown (dump gap 1)
        int lightRadius = 0;         // a Light effect's light form radius (where its strength is); 0 if none/unknown
        // Cloak / SpawnHazard: the effects of the spell this effect carries
        // (dump gap 3). Empty when unknown (an old dump) or none.
        std::vector<EffectRow> payload;
        bool payloadKnown = false;

        [[nodiscard]] bool HiddenInUI() const noexcept { return (flags & kFlagHideInUI) != 0; }
        [[nodiscard]] bool Recover() const noexcept { return (flags & kFlagRecover) != 0; }
    };

    enum class Kind : std::uint8_t
    {
        Spell, Scroll, Potion, Poison, Food, Weapon, Ammo, Armour, SoulGem, Light, _Count
    };

    /// The dump's spelling of a kind ("Armor", as `hg dump all` prints it).
    [[nodiscard]] constexpr std::string_view KindName(Kind k) noexcept
    {
        switch (k) {
            case Kind::Spell: return "Spell";
            case Kind::Scroll: return "Scroll";
            case Kind::Potion: return "Potion";
            case Kind::Poison: return "Poison";
            case Kind::Food: return "Food";
            case Kind::Weapon: return "Weapon";
            case Kind::Ammo: return "Ammo";
            case Kind::Armour: return "Armor";
            case Kind::SoulGem: return "SoulGem";
            case Kind::Light: return "Light";
            default: return "";
        }
    }

    [[nodiscard]] constexpr std::optional<Kind> KindFromName(std::string_view s) noexcept
    {
        for (int i = 0; i < static_cast<int>(Kind::_Count); ++i) {
            if (KindName(static_cast<Kind>(i)) == s) return static_cast<Kind>(i);
        }
        return std::nullopt;
    }

    enum class ArmourWeight : std::uint8_t { Clothing, Light, Heavy };

    /// One item-like form: a spell, scroll, potion, poison, food, weapon
    /// (staves included), ammo, armour piece, soul gem or carried light.
    struct ItemRecord
    {
        Kind kind = Kind::Spell;
        std::uint32_t formId = 0;
        std::string plugin;
        std::string name;
        bool playable = true;
        int value = 0;
        float weight = 0.0f;
        std::vector<std::string> keywords;

        // Spells and scrolls
        int spellType = -1;
        int castingType = -1;
        int delivery = -1;
        float magickaCost = 0.0f;
        std::optional<bool> taughtByTome;  // unknown in dumps before 0.23.7

        // Weapons (and the damage of ammo)
        int weaponType = -1;
        bool twoHanded = false;
        float damage = 0.0f;
        float speed = 0.0f;
        float reach = 0.0f;
        float critDamage = 0.0f;

        // Ammo: the engine's NonBolt flag (dump gap 2); unknown in old dumps
        std::optional<bool> ammoNonBolt;

        // Armour
        std::uint32_t slotMask = 0;
        float armorRating = 0.0f;
        ArmourWeight armourWeight = ArmourWeight::Clothing;

        // Soul gems, lights
        int soulCapacity = 0;
        int soulContained = 0;
        int lightRadius = 0;

        // The enchantment of a weapon or armour piece
        bool enchanted = false;
        int enchantCastingType = -1;  // unknown in old dumps

        std::vector<EffectRow> effects;
    };

    /// The magic effects referenced by EffectRow::effect.
    using EffectTable = std::vector<MagicEffectRecord>;
}
