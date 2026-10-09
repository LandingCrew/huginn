#pragma once

// =============================================================================
// SCRIPT NAMES -- the Papyrus scripts attached to magic effects (dump gap 6)
// =============================================================================
// A script-only effect with no description says nothing about itself except
// the scripts it runs (doc 9, "Dump gaps" 6). The game does not keep an MGEF's
// VMAD in memory (scripts are bound per active effect), so this reads the
// winning plugin's MGEF record from disk: its own read-only file handle (never
// the game's TESFile, which cell streaming uses), the top-level MGEF group
// only, uncompressed records only.
//
// For `hg dump all` (Debug builds) -- not on any per-tick path.
// =============================================================================

#include <string>
#include <unordered_map>
#include <vector>

namespace Huginn::Effect
{
    struct ScriptNameStats
    {
        std::size_t wanted = 0;
        std::size_t found = 0;       // MGEFs whose record was found
        std::size_t withScripts = 0;  // ...and had a VMAD with at least one script
        std::size_t filesRead = 0;
        std::size_t filesFailed = 0;
    };

    /// MGEF FormID -> "ScriptA;ScriptB" for each wanted effect whose winning
    /// record carries scripts.
    [[nodiscard]] std::unordered_map<RE::FormID, std::string> ReadScriptNames(
        const std::vector<const RE::EffectSetting*>& wanted, ScriptNameStats* stats = nullptr);
}
