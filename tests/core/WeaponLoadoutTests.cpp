// core/WeaponLoadout.h: which two-handed weapon is a quarterstaff. The keyword
// lists are the runtime keywords the LoreRim effect dump (Huginn_All.csv,
// 2026-10-28) recorded for each weapon, copied verbatim.

#include "core/WeaponLoadout.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace Huginn::Core::Loadout;

namespace
{
    std::vector<std::string> Split(std::string_view list)
    {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= list.size()) {
            const auto end = std::min(list.find(';', start), list.size());
            out.emplace_back(list.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }

    bool Quarterstaff(bool twoHanded, std::string_view keywords)
    {
        const auto kws = Split(keywords);
        return IsQuarterstaff(twoHanded, [&](std::string_view kw) {
            return std::find(kws.begin(), kws.end(), kw) != kws.end();
        });
    }
}

TEST_CASE("quarterstaff: the mage's Staff of Wandering Stars (LoreRim Traits.esp 00001D, a warhammer)")
{
    constexpr std::string_view kStaffOfWanderingStars =
        "WeapMaterialSteel;WeapTypeWarhammer;VendorItemWeapon;VendorItemStaff;MagicDisallowEnchanting;WeapTypeQtrStaff;"
        "REQ_DamageType_Blunt;WeapTypeQuarterstaff;RFTI_Exclusions_NoDamageRescale;RFTI_Exclusions_NoWeaponReachRescale;"
        "OCF_EquipTypeBothHands;Traits_GollumWeaponHeavy;OCF_WeapTypeQuarterstaff2H;OCF_WeapBlunt2H;OCF_WeapMelee;"
        "BoobiesWeapTypeQuarterstaff;LoreBox_LorerimSTEELWEAPONS;DBWR_Quarterstaff;ABC_I4WIO_Quarterstaff;"
        "Traits_OldFashionWeapon";
    CHECK(Quarterstaff(true, kStaffOfWanderingStars));
    CHECK_FALSE(TwoHandedLoadout(true, Quarterstaff(true, kStaffOfWanderingStars)));
}

TEST_CASE("quarterstaff: any one keyword is enough (Vigilant's Pope Megus' Staff has only the OCF tag)")
{
    CHECK(Quarterstaff(true,
        "WeapMaterialGlass;WeapTypeWarhammer;VendorItemWeapon;zzzCHSpMKeyBaneUndead;REQ_DamageType_Blunt;"
        "RFTI_Exclusions_NoDamageRescale;RFTI_Exclusions_NoWeaponReachRescale;OCF_EquipTypeBothHands;"
        "Traits_GollumWeaponHeavy;OCF_WeapTypeQuarterstaff2H;OCF_WeapBlunt2H;OCF_WeapMelee;BoobiesWeapTypeQuarterstaff;"
        "LoreBox_LorerimGLASSWEAPONS;ABC_I4WIO_Quarterstaff"));
    CHECK(Quarterstaff(true, "WeapTypeWarhammer;WeapTypeQuarterstaff"));   // Requiem's alone
    CHECK(Quarterstaff(true, "WeapTypeWarhammer;WeapTypeQtrStaff"));       // Animated Armoury's alone
}

TEST_CASE("quarterstaff: a warhammer, a greatsword and a magic staff are not")
{
    constexpr std::string_view kAegisbane =   // Skyrim.esm 000D2AFE, a real warhammer
        "WeapMaterialIron;WeapTypeWarhammer;VendorItemWeapon;REQ_DamageType_Blunt;RFTI_Exclusions_NoDamageRescale;"
        "RFTI_Exclusions_NoWeaponReachRescale;OCF_EquipTypeBothHands;Traits_GollumWeaponHeavy;OCF_WeapTypeWarhammer2H;"
        "OCF_WeapBlunt2H;OCF_WeapMelee;LoreBox_LorerimIRONWEAPONS;Traits_OldFashionWeapon";
    CHECK_FALSE(Quarterstaff(true, kAegisbane));
    CHECK(TwoHandedLoadout(true, Quarterstaff(true, kAegisbane)));
    CHECK_FALSE(Quarterstaff(true,   // Skyrim.esm 00017288
        "WeapMaterialDaedric;WeapTypeGreatsword;VendorItemWeapon;REQ_DamageType_Slash;RFTI_Exclusions_NoDamageRescale;"
        "RFTI_Exclusions_NoWeaponReachRescale;OCF_EquipTypeBothHands;Traits_GollumWeaponHeavy;OCF_WeapTypeGreatsword2H;"
        "OCF_WeapBladed2H;OCF_WeapMelee;LoreBox_LorerimDAEDRICWEAPONS"));
    // A magic staff (kStaff; the engine's poll never asks, but the keywords say no too).
    CHECK_FALSE(Quarterstaff(true, "WeapTypeStaff;VendorItemStaff;OCF_EquipTypeRightHand;OCF_WeapTypeStaff1H;OCF_WeapTypeWand1H"));
}

TEST_CASE("quarterstaff: a one-handed weapon is never tested (Sword of Amazement, OCF_WeapTypeQuarterstaff1H)")
{
    constexpr std::string_view kSwordOfAmazement =   // Skyrim.esm 00013135, a one-handed sword
        "WeapTypeStaff;VendorItemWeapon;OCF_EquipTypeRightHand;Traits_GollumWeaponHeavy;OCF_WeapTypeQuarterstaff1H;"
        "OCF_WeapBlunt1H;OCF_WeapTypeSword1H;OCF_WeapBladed1H;OCF_WeapMelee;ABC_I4WIO_Quarterstaff";
    CHECK_FALSE(Quarterstaff(false, kSwordOfAmazement));
    // Even a one-handed weapon that carries a two-handed keyword (one in the dump has Requiem's).
    CHECK_FALSE(Quarterstaff(false, "WeapTypeQuarterstaff;OCF_WeapTypeQuarterstaff1H"));
}

TEST_CASE("two-handed loadout: no two-handed weapon is never one")
{
    CHECK_FALSE(TwoHandedLoadout(false, false));
    CHECK_FALSE(TwoHandedLoadout(false, true));
    CHECK(TwoHandedLoadout(true, false));
    CHECK_FALSE(TwoHandedLoadout(true, true));
}
