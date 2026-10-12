// Host tests for src/core/ReachLog.h.

#include "core/ReachLog.h"

#include <doctest/doctest.h>

#include <ostream>
#include <string>

using namespace Huginn::Core::Reach;

TEST_CASE("reach: kind from the form")
{
    CHECK(ClassifyKind(FormClass::Spell, false, false) == Kind::Spell);
    CHECK(ClassifyKind(FormClass::Scroll, false, false) == Kind::Scroll);
    CHECK(ClassifyKind(FormClass::Alchemy, false, false) == Kind::Potion);
    CHECK(ClassifyKind(FormClass::Alchemy, true, false) == Kind::Food);
    CHECK(ClassifyKind(FormClass::Alchemy, false, true) == Kind::Poison);
    CHECK(ClassifyKind(FormClass::Alchemy, true, true) == Kind::Poison);   // poison first
    CHECK(ClassifyKind(FormClass::Weapon, false, false) == Kind::Weapon);
    CHECK(ClassifyKind(FormClass::Ammo, false, false) == Kind::Ammo);
    CHECK(ClassifyKind(FormClass::Armor, false, false) == Kind::Gear);
    CHECK(ClassifyKind(FormClass::Light, false, false) == Kind::Other);
    CHECK(ClassifyKind(FormClass::SoulGem, false, false) == Kind::Other);
    CHECK(ClassifyKind(FormClass::Other, true, true) == Kind::Other);   // the flags mean nothing off alchemy

    CHECK(KindName(Kind::Spell) == "spell");
    CHECK(KindName(Kind::Gear) == "gear");
    CHECK(KindName(Kind::Count) == "other");
}

TEST_CASE("reach: menu vias are the game menus, nothing else")
{
    CHECK(IsMenuVia("inventory menu"));
    CHECK(IsMenuVia("favorites menu"));
    CHECK(IsMenuVia("magic menu"));
    CHECK(IsMenuVia("menu (just closed)"));
    CHECK_FALSE(IsMenuVia("vanilla hotkey"));
    CHECK_FALSE(IsMenuVia("own wheel"));
    CHECK_FALSE(IsMenuVia("Wheeler wheel"));
    CHECK_FALSE(IsMenuVia(""));
}

TEST_CASE("reach: gear counts only enchanted, through a menu, not loading, once per equip")
{
    GearGate gate;
    GearEquip ring{ 0x0001'2345, true, false, "inventory menu" };

    SUBCASE("an enchanted ring through the inventory counts, once")
    {
        CHECK(gate.Counts(ring, 0));
        CHECK_FALSE(gate.Counts(ring, 10));                         // the doubled event
        CHECK_FALSE(gate.Counts(ring, GearGate::REPEAT_MS - 1));
        CHECK(gate.Counts(ring, GearGate::REPEAT_MS + 10));         // put on again later
    }
    SUBCASE("plain clothes are dressing")
    {
        GearEquip shirt = ring;
        shirt.enchanted = false;
        CHECK_FALSE(gate.Counts(shirt, 0));
    }
    SUBCASE("no player input, a hotkey or a wheel: not a menu equip")
    {
        GearEquip e = ring;
        e.via = "";
        CHECK_FALSE(gate.Counts(e, 0));   // an outfit or a script
        e.via = "vanilla hotkey";
        CHECK_FALSE(gate.Counts(e, 0));
        e.via = "own wheel";
        CHECK_FALSE(gate.Counts(e, 0));
        e.via = "favorites menu";
        CHECK(gate.Counts(e, 0));
    }
    SUBCASE("during a load")
    {
        GearEquip e = ring;
        e.loading = true;
        CHECK_FALSE(gate.Counts(e, 0));
        e.loading = false;
        CHECK(gate.Counts(e, 0));         // a refused equip does not start the repeat window
    }
    SUBCASE("two different pieces are two reaches")
    {
        GearEquip amulet = ring;
        amulet.formID = 0x0001'2346;
        CHECK(gate.Counts(ring, 0));
        CHECK(gate.Counts(amulet, 1));
    }
    SUBCASE("Reset forgets the repeat window")
    {
        CHECK(gate.Counts(ring, 0));
        gate.Reset();
        CHECK(gate.Counts(ring, 1));
    }
}

TEST_CASE("reach: the [Reach] line")
{
    Line in;
    in.name = "Fast Healing";
    in.formID = 0x0002'F3B8;
    in.kind = Kind::Spell;
    in.via = "magic menu";
    in.inFight = true;
    in.caseLabel = "B-low (low rank)";
    in.scored = true;
    in.ranked = true;
    in.rank = 14;
    in.utility = 0.834f;
    in.pageKnown = true;
    in.page = 0;
    in.keys = { { 0, "Oakflesh on Self" }, { 2, "Conjure Skeletal Hero" } };

    CHECK(FormatLine(in) ==
          "[Reach] 'Fast Healing' 0002F3B8 kind=spell via=magic menu fight=yes case=B-low (low rank) "
          "rank=14 util=0.83 | page 0: 0=Oakflesh on Self 2=Conjure Skeletal Hero");

    SUBCASE("past the sorted prefix: rank=tail")
    {
        in.ranked = false;
        CHECK(FormatLine(in).find(" rank=tail util=0.83 |") != std::string::npos);
    }
    SUBCASE("not scored at all")
    {
        in.scored = false;
        in.caseLabel = "A (not candidate)";
        in.inFight = false;
        const auto line = FormatLine(in);
        CHECK(line.find(" fight=no case=A (not candidate) rank=- util=- |") != std::string::npos);
    }
    SUBCASE("gear: no case label, no page cached yet")
    {
        in.kind = Kind::Gear;
        in.caseLabel = "";
        in.scored = false;
        in.pageKnown = false;
        in.keys.clear();
        CHECK(FormatLine(in) ==
              "[Reach] 'Fast Healing' 0002F3B8 kind=gear via=magic menu fight=yes case=- rank=- util=- | page ?");
    }
    SUBCASE("an empty page")
    {
        in.keys.clear();
        CHECK(FormatLine(in).ends_with("| page 0: (empty)"));
    }
}

TEST_CASE("reach: the heartbeat split")
{
    Split split;
    CHECK(FormatSplit(split.Take()) == "reach fight=0 calm=0");

    split.Record(true, Kind::Spell);
    split.Record(true, Kind::Spell);
    split.Record(true, Kind::Potion);
    split.Record(false, Kind::Spell);
    split.Record(false, Kind::Spell);
    split.Record(false, Kind::Food);
    split.Record(false, Kind::Food);
    split.Record(false, Kind::Gear);
    const auto c = split.Take();
    CHECK(c.fight == 3);
    CHECK(c.calm == 5);
    CHECK(FormatSplit(c) == "reach fight=3 calm=5 (spell=4 potion=1 food=2 gear=1)");

    // Read-and-reset.
    CHECK(FormatSplit(split.Take()) == "reach fight=0 calm=0");
}

TEST_CASE("reach: the gear gate's memory is bounded")
{
    GearGate gate;
    for (std::uint32_t id = 1; id <= GearGate::kMaxRemembered; ++id) {
        CHECK(gate.Counts(GearEquip{ id, true, false, "inventory menu" }, 0));
    }
    CHECK(gate.Remembered() == GearGate::kMaxRemembered);
    // Full: the next new piece clears the memory, then is remembered alone.
    CHECK(gate.Counts(GearEquip{ 1000, true, false, "inventory menu" }, 1));
    CHECK(gate.Remembered() == 1);
    // Clearing only ends repeat windows early: piece 1 counts again.
    CHECK(gate.Counts(GearEquip{ 1, true, false, "inventory menu" }, 2));
    // ... and piece 1000's window survived the clear (it came after it).
    CHECK_FALSE(gate.Counts(GearEquip{ 1000, true, false, "inventory menu" }, 3));
}

TEST_CASE("reach: is the piece just put on enchanted?")
{
    StackFacts f;
    CHECK(ClassifyEnchant(f) == Enchant::No);                     // a plain piece, no stacks

    f.baseEnchanted = true;                                       // a ring of Conjuration off the shelf
    f.plainStacks = 3;
    CHECK(ClassifyEnchant(f) == Enchant::Yes);

    SUBCASE("the worn stack decides when one is marked")
    {
        StackFacts g;
        g.enchantedStacks = 1;
        g.plainStacks = 1;
        g.wornKnown = true;
        g.wornEnchanted = false;                                  // the plain Gold Ring went on
        CHECK(ClassifyEnchant(g) == Enchant::No);
        g.wornEnchanted = true;
        CHECK(ClassifyEnchant(g) == Enchant::Yes);
    }
    SUBCASE("no stack marked worn: only when every copy is enchanted")
    {
        StackFacts g;
        g.enchantedStacks = 1;                                    // the only copy, player-enchanted
        CHECK(ClassifyEnchant(g) == Enchant::Yes);
        g.enchantedStacks = 2;                                    // two, both enchanted
        CHECK(ClassifyEnchant(g) == Enchant::Yes);
        g.looseCopies = 1;                                        // plus a plain one with no extra data
        CHECK(ClassifyEnchant(g) == Enchant::Ambiguous);
        g.looseCopies = 0;
        g.plainStacks = 1;                                        // plus a plain stack
        CHECK(ClassifyEnchant(g) == Enchant::Ambiguous);
        g.enchantedStacks = 0;                                    // nothing enchanted at all
        CHECK(ClassifyEnchant(g) == Enchant::No);
    }
}
