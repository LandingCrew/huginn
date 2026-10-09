#pragma once

// =============================================================================
// EFFECT DUMP -- `hg dump all`, the catalog view (Debug builds)
// =============================================================================
// One row per item x effect, as before 0.23.12 (every old column, same order,
// so the doc 9 analysis scripts keep working), then the dump gaps of doc 9 and
// the catalog's view of each row and item:
//
//   ammoNonBolt, enchantCasting      gap 2; the enchantment's casting type
//   effectPlugin                     the MGEF's defining plugin (override key)
//   effectCost                       gap 4: the effect item's own cost
//   effectSchool                     gap 1: MGEF associatedSkill
//   effectLightRadius                a Light effect's light form radius (its strength)
//   effectText                       gap 5: the description, <mag>/<dur>/<area> filled
//   effectScripts                    gap 6: Papyrus scripts on the MGEF (from the plugin file)
//   effectConditions                 gap 8: MGEF and effect-item conditions
//   payloadOf, payloadSpell          gap 3: a Cloak/hazard's carried spell, as extra
//                                    rows under its wrapper (payloadOf = the wrapper's effectIndex)
//   effectColumn, effectRoute,       the catalog: the row's column and how it was
//   effectKept                       found, and whether it counts in cap(i)
//   inScope, slotClass, cap          per item, on its first row only: in the
//                                    catalog's scope; today's slot class (the
//                                    legacy classifiers + SlotClassifier); cap(i)
//
// Gap 7 ("taught by shout") is moot: shouts are out of scope (2026-10-08).
// tests/tools/EffectReport.cpp (huginn_effect_report) re-maps the raw columns
// on the host and checks them against effectColumn and cap.
// =============================================================================

#include <chrono>
#include <filesystem>
#include <string>

namespace Huginn::Effect
{
    /// Write the dump to `path`. False (with `summary` saying why) on failure;
    /// `summary` is the one-line result either way.
    /// `wait` is how long to wait for the catalog's worker (0 = not at all:
    /// the console says "not ready" instead of blocking the main thread).
    bool WriteDumpAll(const std::filesystem::path& path, std::string& summary,
                      std::chrono::milliseconds wait = std::chrono::milliseconds(0));
}
