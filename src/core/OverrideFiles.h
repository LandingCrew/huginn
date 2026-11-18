#pragma once

// =============================================================================
// EFFECT OVERRIDE FILES (R11) -- the per-load-order override layer, as a directory
// =============================================================================
// Doc 9's layer 2: an effect (MGEF) the catalog's rules read wrongly is pinned
// to an effects.csv column by its plugin and local FormID, never by a runtime
// FormID, a name or an editor ID:
//
//     [Huginn]
//     iFormatVersion = 1
//     [Overrides]
//     Skyrim.esm|0x3EB15 = restore_health
//     MyMod.esp|0x800    = resist_frost
//
// Since R11 there can be many such files, SPID `_DISTR.ini` style, so a mod
// author can ship the corrections for their own effects:
//   - every Data/SKSE/Plugins/Huginn/EffectOverrides/*.ini, in alphabetical
//     order (case-insensitive), then the player's own
//     Data/SKSE/Plugins/Huginn_EffectOverrides.ini LAST, so the player wins;
//   - a later file's entry for the same effect replaces an earlier one (and is
//     counted, so a conflict between two mods shows in the log);
//   - an entry for a plugin that is not loaded is skipped quietly (counted);
//   - a file that cannot be read (CSimpleIniA::LoadFile fails: an I/O error;
//     the INI reader takes almost any text), or says it is for a newer
//     format, is skipped whole and named in the log; the other files still
//     load. A malformed LINE is ignored and counted, not the file;
//   - the vocabulary is the effects.csv column ids (`hg dump all`'s
//     effectColumn); `iFormatVersion` absent reads as 1, the first format.
//
// The Huginn_Overrides.ini type/tag file is NOT this layer: it corrects the
// old spell and item classifiers, which since R11 feed only the D2 legacy
// feature (docs/architecture/9-implementation-map.md, Phase 10 as built).
//
// Pure: standard library only (src/core/README.md). The game side
// (effect/EffectCatalog.cpp) lists and reads the files and asks which
// plugins are loaded.
// =============================================================================

#include "core/EffectColumns.h"
#include "core/EffectRules.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Huginn::Core::Effect
{
    inline constexpr int kOverrideFormatVersion = 1;

    /// One file as the game side read it.
    struct OverrideFile
    {
        std::string name;                                         // for the log
        bool readOk = true;                                       // false: could not be read
        int version = 0;                                          // [Huginn] iFormatVersion; 0 = absent (reads as 1)
        bool player = false;                                      // the player's own file: applied last
        std::vector<std::pair<std::string, std::string>> lines;   // [Overrides] key = value, in file order
    };

    struct ParsedOverride
    {
        std::string plugin;
        std::uint32_t localId = 0;
        Col col = Col::_Count;
    };

    /// "<plugin>|<local FormID hex>" = "<column id>". nullopt (with `why`) for
    /// a line that does not read so. A FormID's load-order byte(s) are dropped
    /// (OverrideTable::LocalId), so "Skyrim.esm|0x00012FCC" and "|12FCC" agree.
    [[nodiscard]] inline std::optional<ParsedOverride> ParseOverrideLine(std::string_view key, std::string_view value,
                                                                         std::string* why = nullptr)
    {
        const auto fail = [why](const char* w) -> std::optional<ParsedOverride> {
            if (why) *why = w;
            return std::nullopt;
        };
        const auto trim = [](std::string_view s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
            return s;
        };
        key = trim(key);
        value = trim(value);
        const auto bar = key.find('|');
        if (bar == std::string_view::npos) return fail("want <plugin>|<local FormID hex> = <column>");
        const std::string_view plugin = trim(key.substr(0, bar));
        std::string_view hex = trim(key.substr(bar + 1));
        if (plugin.empty()) return fail("no plugin before '|'");
        if (hex.starts_with("0x") || hex.starts_with("0X")) hex.remove_prefix(2);
        std::uint32_t id = 0;
        const auto r = std::from_chars(hex.data(), hex.data() + hex.size(), id, 16);
        if (hex.empty() || r.ec != std::errc{} || r.ptr != hex.data() + hex.size()) return fail("bad FormID");
        const auto col = FromName(value);
        if (!col) return fail("not an effects.csv column id");
        return ParsedOverride{ std::string(plugin), OverrideTable::LocalId(id), *col };
    }

    struct OverrideFileReport
    {
        std::string name;
        bool skipped = false;          // the whole file (could not be read, or a newer format)
        std::string why;
        std::size_t loaded = 0;        // entries that went into the table
        std::size_t ignored = 0;       // lines that did not read
        std::size_t notLoaded = 0;     // entries for a plugin that is not loaded
        std::size_t replaced = 0;      // entries that replaced an earlier file's for the same effect
    };

    struct OverrideMerge
    {
        OverrideTable table;
        std::vector<OverrideFileReport> files;   // in the order applied
        std::size_t entries = 0;                 // distinct effects in the table
    };

    /// The order the files apply in: the mods' files alphabetically
    /// (case-insensitive, then by exact name), the player's file(s) last.
    [[nodiscard]] inline std::vector<std::size_t> OverrideOrder(const std::vector<OverrideFile>& files)
    {
        std::vector<std::size_t> order(files.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        const auto lower = [](const std::string& s) {
            std::string out(s);
            for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        };
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            if (files[a].player != files[b].player) return !files[a].player;
            const auto la = lower(files[a].name), lb = lower(files[b].name);
            return la != lb ? la < lb : files[a].name < files[b].name;
        });
        return order;
    }

    /// Merge the files in OverrideOrder; `pluginLoaded(name)` says whether a
    /// plugin is in the load order (an entry for one that is not is skipped).
    [[nodiscard]] inline OverrideMerge MergeOverrides(const std::vector<OverrideFile>& files,
                                                      const std::function<bool(std::string_view)>& pluginLoaded)
    {
        OverrideMerge out;
        std::map<std::pair<std::string, std::uint32_t>, std::size_t> seen;   // effect -> the file that set it
        for (const std::size_t fi : OverrideOrder(files)) {
            const auto& f = files[fi];
            OverrideFileReport rep;
            rep.name = f.name;
            if (!f.readOk) {
                rep.skipped = true;
                rep.why = "cannot be read";
            }
            else if (f.version > kOverrideFormatVersion) {
                rep.skipped = true;
                rep.why = "format version " + std::to_string(f.version) + " is newer than this Huginn reads (" +
                          std::to_string(kOverrideFormatVersion) + ")";
            }
            else if (f.version < 0) {
                rep.skipped = true;
                rep.why = "bad format version";
            }
            if (!rep.skipped) {
                for (const auto& [k, v] : f.lines) {
                    const auto p = ParseOverrideLine(k, v);
                    if (!p) {
                        ++rep.ignored;
                        continue;
                    }
                    if (pluginLoaded && !pluginLoaded(p->plugin)) {
                        ++rep.notLoaded;
                        continue;
                    }
                    std::string lp = p->plugin;
                    for (auto& c : lp) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    const auto key = std::make_pair(lp, p->localId);
                    if (const auto it = seen.find(key); it != seen.end() && it->second != fi) ++rep.replaced;
                    seen[key] = fi;
                    out.table.Add(p->plugin, p->localId, p->col);
                    ++rep.loaded;
                }
            }
            out.files.push_back(std::move(rep));
        }
        out.entries = out.table.Size();
        return out;
    }
}
