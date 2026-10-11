#pragma once

// =============================================================================
// EFFECT CONDITIONS -- when an effect row fires, from its conditions (0.23.30)
// =============================================================================
// An effect row in Skyrim carries two condition lists: the MGEF's own and the
// effect item's (both must pass). The reader (src/effect/EffectReader.cpp)
// prints each the way `hg dump all` prints its `effectConditions` column,
// mgef[...] and item[...]:
//
//   T.HasPerk(000153D2)==1 AND GetRandomPercent()>=75 OR IsUndead()==1
//
// "T." runs the function on the condition's Target, which for a magic effect
// is its CASTER (or a weapon's wielder); no prefix runs it on the Subject,
// the actor the effect lands on (the caster too, for a self-delivered effect).
// A perk is always printed as its FormID (hex), so the game can look it up.
//
// Precedence is the engine's: OR binds before AND, so the list above is
// (HasPerk) AND (Random OR IsUndead) -- a run of conditions joined by OR is one
// clause, and the clauses are ANDed (ReadClauses).
//
// What the mapper reads from a row's clauses (ReadGate) -- a clause counts
// only when EVERY condition in it is of one kind:
//   perk    every condition is HasPerk(P) true (==1, !=0, >=1, >0) on the
//           player: run on the caster ("T."), or on the subject when the
//           effect lands on its user (the caller says, subjectIsUser). The row
//           fires only when the player has one of the clause's perks. A false
//           test (HasPerk==0: "without the perk") is no gate: that row is the
//           default the perk row replaces.
//   random  every condition is GetRandomPercent() compared with a literal;
//           the clause's chance is P(any of them), each a fresh roll
//           (GetRandomPercent returns 0..99): <N is N/100, <=N (N+1)/100, >N
//           (99-N)/100, >=N (100-N)/100, ==N 1/100, !=N 99/100. The row's
//           chance is the product over its random clauses.
//   target  every condition is a positive test of the subject's family: a
//           HasKeyword(ActorType*) other than ActorTypeNPC, HasKeyword(Vampire),
//           IsUndead(), GetIsRace(R). On a row that lands on others it is a
//           BANE (Requiem's triple shock damage to constructs, "deals 20
//           magic damage to undead"); the mapper decides what that means.
// Everything else (levels, actor values, magic effects present, dual casting,
// globals ...) is not read: such a row counts as it always did.
//
// Over-time effects re-roll: the engine re-tests an active effect's
// conditions while it runs (about once a second), so a GetRandomPercent gate
// on a timed effect turns it on and off; the share of its duration it is
// active is the same chance p, so a timed row scales by p exactly as an
// instant one (an expected value, not the distribution).
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Huginn::Core::Effect
{
    struct Condition
    {
        std::string function;    // "HasPerk", "GetRandomPercent", ...
        bool onTarget = false;   // "T.": run on the target (a magic effect's caster)
        std::string param;       // the first parameter as printed, "" if none
        std::string op;          // "==", "!=", ">", ">=", "<", "<="
        float value = 0.0f;      // the literal compared with (0 when `global`)
        bool global = false;     // compared with a global: value unknown
        bool orNext = false;     // OR-ed with the next condition
    };

    /// The conditions of one printed list (the inside of mgef[...] or
    /// item[...]). A malformed condition is skipped, never guessed.
    [[nodiscard]] std::vector<Condition> ParseConditions(std::string_view text);

    using Clause = std::vector<Condition>;

    /// The engine's precedence: a run joined by OR is one clause; the clauses
    /// are ANDed.
    [[nodiscard]] std::vector<Clause> ReadClauses(const std::vector<Condition>& conditions);

    /// The chance that one GetRandomPercent() <op> <value> passes, in [0, 1].
    [[nodiscard]] float RandomChance(std::string_view op, float value) noexcept;

    /// What a row's conditions say about when it fires (see the header).
    struct Gate
    {
        float chance = 1.0f;                            // product over the random clauses
        std::vector<std::vector<std::uint32_t>> perks;  // per perk clause: any of these (0 = a perk not printed as a FormID)
        bool targetFamily = false;                      // a clause requires the subject to be of a family

        [[nodiscard]] bool PerkGated() const noexcept { return !perks.empty(); }
        [[nodiscard]] bool Empty() const noexcept { return chance >= 1.0f && perks.empty() && !targetFamily; }
    };

    /// The gate of one printed list. `subjectIsUser`: the effect lands on its
    /// user (self-delivered, a summon, a bound weapon), so an unprefixed
    /// HasPerk tests the player too.
    [[nodiscard]] Gate ReadGate(std::string_view conditions, bool subjectIsUser);

    /// Both lists must pass: chances multiply, perk clauses and target tests add up.
    void MergeGate(Gate& into, const Gate& other);

    /// Does any condition of the list test GetDead()==1 on the subject (an
    /// effect that acts on a corpse)?
    [[nodiscard]] bool TestsSubjectDead(std::string_view conditions);
}
