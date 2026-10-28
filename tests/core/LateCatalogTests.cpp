// Host tests for src/core/LateCatalog.h: which held forms the effect catalog
// maps after its build (player-made potions, spells no tome teaches) and the
// per-load entry table (0.23.41).

#include "core/EffectMapper.h"
#include "core/LateCatalog.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace Huginn::Core::Effect;

namespace
{
    // A plain fire-and-forget spell with one visible effect, the way the
    // reader would record Oakflesh on Target.
    struct SpellFixture
    {
        EffectTable effects;
        ItemRecord spell;

        SpellFixture()
        {
            MagicEffectRecord m;
            m.formId = 0x8D005B20u;
            m.name = "Oakflesh";
            m.primaryAV = "DamageResist";
            effects.push_back(m);
            spell.kind = Kind::Spell;
            spell.formId = 0x8D005B23u;
            spell.name = "Oakflesh on Target";
            spell.spellType = kSpellTypeSpell;
            spell.castingType = kCastFireAndForget;
            spell.magickaCost = 100.0f;
            EffectRow row;
            row.effect = 0;
            row.magnitude = 40.0f;
            row.duration = 60;
            spell.effects.push_back(row);
        }
    };
}

TEST_CASE("late catalog: runtime forms and spells are mapped late, other misses stay out")
{
    CHECK(IsDynamicFormId(0xFF0048C4u));        // a player-made Potion of Restore Magicka
    CHECK_FALSE(IsDynamicFormId(0xFE62CA7Fu));  // an ESL form: in the load order, built
    CHECK_FALSE(IsDynamicFormId(0x0003EADEu));
    CHECK(MapLate(0xFF0048C4u, false));
    CHECK(MapLate(0x8D005B23u, true));          // a spell the player knows
    CHECK_FALSE(MapLate(0x0001397Eu, false));   // a static weapon the build left out (rule 1)
}

TEST_CASE("late catalog: a known spell no tome teaches is in scope once held; the other rules stand")
{
    SpellFixture f;
    f.spell.taughtByTome = false;                 // the build's reading: an NPC spell
    CHECK_FALSE(InScope(f.spell, f.effects));
    MarkHeld(f.spell);
    CHECK(InScope(f.spell, f.effects));

    // Rule 1's other tests are not lifted: an ability, or a spell whose every
    // effect is hidden, stays out.
    SpellFixture ability;
    ability.spell.spellType = 4;   // ability
    MarkHeld(ability.spell);
    CHECK_FALSE(InScope(ability.spell, ability.effects));
    SpellFixture hidden;
    hidden.effects[0].flags = kFlagHideInUI;
    MarkHeld(hidden.spell);
    CHECK_FALSE(InScope(hidden.spell, hidden.effects));

    // A potion is untouched.
    ItemRecord potion;
    potion.kind = Kind::Potion;
    potion.playable = true;
    potion.name = "Potion of Restore Magicka";
    MarkHeld(potion);
    CHECK_FALSE(potion.taughtByTome.has_value());
    CHECK(InScope(potion, f.effects));
}

TEST_CASE("late catalog: one entry per key per load, a 'none' remembered, addresses kept past a load")
{
    LateEntries<std::string> t;
    int made = 0;
    auto make = [&made](const char* text) {
        return [&made, text]() {
            ++made;
            return text ? std::make_unique<std::string>(text) : std::unique_ptr<std::string>{};
        };
    };
    const std::string* a = t.Get(0xFF000001u, make("Potion of Healing"));
    REQUIRE(a != nullptr);
    CHECK(*a == "Potion of Healing");
    CHECK(t.Get(0xFF000001u, make("never made")) == a);   // made once
    CHECK(t.Get(0xFF000002u, make(nullptr)) == nullptr);  // "no entry"...
    CHECK(t.Get(0xFF000002u, make("never made")) == nullptr);   // ...remembered
    CHECK(made == 2);
    CHECK(t.Peek(0xFF000001u) == a);
    // A remembered "none" is an answer: Contains says so, Peek reads null.
    CHECK(t.Contains(0xFF000001u));
    CHECK(t.Contains(0xFF000002u));
    CHECK(t.Peek(0xFF000002u) == nullptr);
    CHECK_FALSE(t.Contains(0xFF000003u));

    // A new load: the same FF ID can name another potion. The old entry is
    // retired, not freed: the pointer handed out still reads the old value.
    t.Forget();
    CHECK(t.Live() == 0);
    CHECK(t.Retired() == 1);
    CHECK(t.Peek(0xFF000001u) == nullptr);
    const std::string* b = t.Get(0xFF000001u, make("Potion of Fortify Two-Handed"));
    REQUIRE(b != nullptr);
    CHECK(b != a);
    CHECK(*a == "Potion of Healing");
    CHECK(*b == "Potion of Fortify Two-Handed");
    CHECK(t.Made() == 3);
}

TEST_CASE("late catalog: a key ruled out before mapping is remembered as none and short-circuits")
{
    LateEntries<std::string> t;
    int made = 0;
    // The pattern of EffectCatalog::FindOrMapId: answer from the table when
    // it has one; else look the form up and either map it or remember none.
    auto findOrMapId = [&](std::uint32_t key, bool mapLate, int& lookups) -> const std::string* {
        if (t.Contains(key)) return t.Peek(key);
        ++lookups;
        if (mapLate) {
            return t.Get(key, [&made]() { ++made; return std::unique_ptr<std::string>{}; });   // out of scope
        }
        t.RememberNone(key);
        return nullptr;
    };
    int lookups = 0;
    for (int run = 0; run < 50; ++run) {          // fifty pipeline runs
        CHECK(findOrMapId(0x0001397Eu, false, lookups) == nullptr);   // a static form left out
        CHECK(findOrMapId(0x00012FCDu, true, lookups) == nullptr);    // a power: mapped once, out of scope
    }
    CHECK(lookups == 2);   // one lookup each, not one per run
    CHECK(made == 1);

    // RememberNone never overwrites an answer.
    const std::string* p = t.Get(0xFF000010u, []() { return std::make_unique<std::string>("Potion"); });
    t.RememberNone(0xFF000010u);
    CHECK(t.Peek(0xFF000010u) == p);
    // A load forgets the remembered none too.
    t.Forget();
    CHECK_FALSE(t.Contains(0x0001397Eu));
}

TEST_CASE("late catalog: a load forgets only FF keys; load-order answers keep their address (0.23.45)")
{
    LateEntries<std::string> t;
    auto entry = [](const char* text) { return [text]() { return std::make_unique<std::string>(text); }; };
    const std::string* potion = t.Get(0xFF0048C4u, entry("Potion of Restore Magicka"));   // player-made
    const std::string* oakflesh = t.Get(0x8D005B23u, entry("Oakflesh on Target"));       // no tome teaches it
    t.RememberNone(0x00012FCDu);   // an ability: ruled out before mapping
    t.RememberNone(0xFF0048C5u);   // an FF none
    REQUIRE(t.Live() == 4);

    const std::size_t forgotten = t.ForgetIf([](std::uint32_t key) { return IsDynamicFormId(key); });
    CHECK(forgotten == 2);
    CHECK(t.Live() == 2);
    CHECK(t.Retired() == 1);   // the FF entry is retired, not freed
    CHECK_FALSE(t.Contains(0xFF0048C4u));
    CHECK_FALSE(t.Contains(0xFF0048C5u));
    CHECK(t.Contains(0x00012FCDu));          // the static none stands
    CHECK(t.Peek(0x00012FCDu) == nullptr);
    CHECK(t.Peek(0x8D005B23u) == oakflesh);  // same address, not re-made
    CHECK(*potion == "Potion of Restore Magicka");

    // The next load's FF form with the same ID is made afresh.
    const std::string* next = t.Get(0xFF0048C4u, entry("Potion of Fortify Two-Handed"));
    CHECK(next != potion);
    CHECK(t.Get(0x8D005B23u, entry("never made")) == oakflesh);
    CHECK(t.Made() == 3);
}

TEST_CASE("late catalog: a spell's type and casting type rule it out exactly as InScope does (0.23.45)")
{
    // EffectCatalog::FindOrMap answers "none" from these two fields without
    // reading the form; that is only sound if InScope agrees for every pair.
    for (int spellType = 0; spellType <= 13; ++spellType) {
        for (int castingType = 0; castingType <= 3; ++castingType) {
            SpellFixture f;
            f.spell.spellType = spellType;
            f.spell.castingType = castingType;
            MarkHeld(f.spell);
            const bool shape = SpellShapeInScope(spellType, castingType);
            CAPTURE(spellType);
            CAPTURE(castingType);
            CHECK(InScope(f.spell, f.effects) == shape);   // the fixture passes every later test
        }
    }
    CHECK_FALSE(SpellShapeInScope(4, kCastConstant));   // an ability
    CHECK_FALSE(SpellShapeInScope(2, kCastFireAndForget));   // a power
    CHECK(SpellShapeInScope(kSpellTypeSpell, kCastConcentration));
}
