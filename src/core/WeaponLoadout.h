#pragma once

// =============================================================================
// WEAPON LOADOUT (0.23.51) -- a quarterstaff is not a two-handed loadout
// =============================================================================
// The engine has no quarterstaff type. Requiem / LoreRim quarterstaffs (and
// Animated Armoury's) are two-handed warhammers (WEAPON_TYPE kTwoHandAxe)
// with keywords, so loadout_two_handed read the mage's walking staff as a
// two-handed loadout: it was on at 27.7% of the mage's picks and in 33% of the logged contexts
// (4,706 of 14,246, 2026-10-15..28), and in 3,991 of those 4,706 the weapon
// in hand was the Staff of Wandering Stars (runtime FECCC01D, then FECCF01D
// as the light-plugin slots moved; LoreRim Traits.esp 00001D,
// LoreTraits_StaffofWanderingStars: AnimationType TwoHandAxe, skill
// TwoHanded). A two-handed quarterstaff is its own loadout need
// (loadout_quarterstaff) and leaves loadout_two_handed off, so the fit can
// give it its own pairs instead of a greatsword's.
//
// The keywords, from the LoreRim effect dump (Huginn_All.csv, written
// 2026-10-10, read 2026-10-28; weapons counted once per formID + plugin):
// every one of the 967 two-handed weapons that carry any of them carries
// OCF_WeapTypeQuarterstaff2H (the OCF tag; 5 carry nothing else); 957 also
// carry Requiem's WeapTypeQuarterstaff and 829 Animated Armoury's
// WeapTypeQtrStaff. All 967 are typed warhammer (kTwoHandAxe); most also
// carry the vanilla WeapTypeWarhammer, which says nothing here. One is
// ambiguous: Vigilant's "Heretic's Staff" (Vigilant.esm 305056E7) carries
// WeapTypeBattleaxe and OCF_WeapTypeBattleaxe2H next to
// OCF_WeapTypeQuarterstaff2H, and reads as a quarterstaff (any keyword is
// enough); a single item, accepted. Magic staves
// (WeapTypeStaff, OCF_WeapTypeStaff1H, kStaff) are a different thing and
// carry none of these. The OCF_WeapTypeQuarterstaff1H weapons (7, typed as
// one-handed swords and maces) stay one-handed: only a two-handed weapon is
// tested (StateManager_Equipment tests the right hand's kTwoHandSword /
// kTwoHandAxe). By design that includes a one-handed weapon carrying
// Requiem's WeapTypeQuarterstaff (the dump has one, a 1H sword named
// "Quarterstaff"): it stays loadout_one_handed. Any one keyword is enough, so a load order with only one of the
// three mods still reads its quarterstaffs.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <array>
#include <string_view>

namespace Huginn::Core::Loadout
{
    inline constexpr std::array<std::string_view, 3> kQuarterstaffKeywords{
        "OCF_WeapTypeQuarterstaff2H",   // One Click Framework's sub-type tag
        "WeapTypeQuarterstaff",         // Requiem.esp
        "WeapTypeQtrStaff",             // NewArmoury.esp (Animated Armoury)
    };

    /// A two-handed weapon is a quarterstaff when it carries any of the
    /// keywords; hasKeyword(editorID) answers for the weapon form.
    template <class HasKeyword>
    [[nodiscard]] bool IsQuarterstaff(bool twoHanded, HasKeyword&& hasKeyword)
    {
        if (!twoHanded) return false;
        for (const auto kw : kQuarterstaffKeywords) {
            if (hasKeyword(kw)) return true;
        }
        return false;
    }

    /// loadout_two_handed's reading: a two-handed melee weapon that is not a
    /// quarterstaff.
    [[nodiscard]] constexpr bool TwoHandedLoadout(bool twoHanded, bool quarterstaff) noexcept
    {
        return twoHanded && !quarterstaff;
    }
}
