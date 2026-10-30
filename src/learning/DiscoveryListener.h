#pragma once

#include <cstdint>
#include <vector>

namespace Huginn::Learning::Discovery
{
    // =========================================================================
    // THE DISCOVERY BOOST'S GAME SIDE (0.24.14; the rule is core/DiscoveryBoost.h)
    // =========================================================================
    // Three sources feed ChoiceLearner's tracker:
    //
    //   - GAINS, items: a TESContainerChangedEvent that puts a form into the
    //     player's inventory from anywhere else (picked up, bought, looted,
    //     crafted, given by a script). An event, not an inventory diff: the
    //     registries' scans also see stacks appear when a load or the
    //     deferred apparel scan fills them, and those are not gains.
    //   - GAINS, spells: SpellRegistry::TakeNewlyLearned, drained every tick
    //     by the update loop (ForwardLearnedSpells). Its queue has LearnedAt's
    //     meaning: a spell added in play, never one a load or `hg rebuild`
    //     brought.
    //   - PRESENT AT LOAD (PresentForms, InitializeGameSystems step 6c after
    //     the cosave is applied): every form in the player's inventory changes
    //     list (whatever its count: an entry means the character held it) and
    //     base container, and every spell the registry holds. They enter the
    //     seen set with no boost. Read without touching extraLists (only the
    //     entry's object and the container object), so it is safe inside the
    //     post-load window (Util::IsExtraListStable) where walking extraLists
    //     can crash.
    //
    // Only the kinds the effect catalog describes and the pipeline ranks:
    // spells, scrolls, alchemy items, weapons, ammo, armour, soul gems,
    // lights (IsDiscoverableKind; the same set ChoiceLearner resolves the
    // bootstrap's items against).
    //
    // Registration: Register() adds the event sink once (Main.cpp, with the
    // other TESContainerChangedEvent sinks).
    // =========================================================================

    /// Adds the TESContainerChangedEvent sink (once).
    void Register();

    /// The forms present at a load (above).
    [[nodiscard]] std::vector<std::uint32_t> PresentForms();

    /// Hands the spell registry's newly learned spells to the learner
    /// (every tick, update thread).
    void ForwardLearnedSpells();
}
