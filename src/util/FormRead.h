#pragma once

// =============================================================================
// FORM READING HELPERS -- small read-only views of game forms, shared
// =============================================================================
// Moved out of ConsoleCommands.cpp (R2, 0.23.12) so the effect reader
// (src/effect/) and the dumps read forms the same way:
//   AvName      -- an actor value's enum name ("Health", "OneHandedSkillAdvance")
//   KeywordList -- a form's keyword editor IDs, ';'-joined
//   Keywords    -- the same, as a vector
//   PluginOf    -- the plugin that DEFINES a form (GetFile(0))
//   CsvQuote    -- one CSV field: always quoted, quotes doubled, newlines folded
//
// Read-only: nothing here writes to a form. Call on the main thread (the
// console, kDataLoaded) like any other form read.
// =============================================================================

#include <string>
#include <string_view>
#include <vector>

namespace Huginn::Util
{
    /// The actor value's enum name, or "" for none / out of range.
    [[nodiscard]] inline std::string_view AvName(RE::ActorValue av)
    {
        using namespace std::literals;
        if (av == RE::ActorValue::kNone || av >= RE::ActorValue::kTotal) return ""sv;
        const auto* list = RE::ActorValueList::GetSingleton();
        const auto* info = list ? list->GetActorValue(av) : nullptr;
        return info && info->enumName ? std::string_view(info->enumName) : ""sv;
    }

    /// Every keyword editor ID on the form, in form order; unnamed keywords
    /// are skipped (the game drops editor IDs without powerofthree's Tweaks,
    /// so a list can come back shorter than numKeywords).
    [[nodiscard]] inline std::vector<std::string> Keywords(const RE::BGSKeywordForm* form)
    {
        std::vector<std::string> out;
        if (!form) return out;
        out.reserve(form->numKeywords);
        for (std::uint32_t i = 0; i < form->numKeywords; ++i) {
            const auto* kw = form->keywords[i];
            const char* id = kw ? kw->GetFormEditorID() : nullptr;
            if (!id || !*id) continue;
            out.emplace_back(id);
        }
        return out;
    }

    /// The keyword editor IDs, ';'-joined (the dumps' format).
    [[nodiscard]] inline std::string KeywordList(const RE::BGSKeywordForm* form)
    {
        std::string list;
        if (!form) return list;
        for (std::uint32_t i = 0; i < form->numKeywords; ++i) {
            const auto* kw = form->keywords[i];
            const char* id = kw ? kw->GetFormEditorID() : nullptr;
            if (!id || !*id) continue;
            if (!list.empty()) list += ';';
            list += id;
        }
        return list;
    }

    /// The plugin that defines the form (its first file), or "".
    [[nodiscard]] inline std::string_view PluginOf(const RE::TESForm* form)
    {
        using namespace std::literals;
        const auto* file = form ? form->GetFile(0) : nullptr;
        return file ? file->GetFilename() : ""sv;
    }

    /// The plugin whose record wins (its last file), or "".
    [[nodiscard]] inline std::string_view WinningPluginOf(const RE::TESForm* form)
    {
        using namespace std::literals;
        const auto* file = form ? form->GetFile(-1) : nullptr;
        return file ? file->GetFilename() : ""sv;
    }

    /// One CSV field: always quoted, quotes doubled, newlines folded to a space
    /// so line-counting tools stay honest.
    [[nodiscard]] inline std::string CsvQuote(std::string_view text)
    {
        std::string quoted;
        quoted.reserve(text.size() + 2);
        quoted += '"';
        for (const char c : text) {
            if (c == '\r' || c == '\n') { quoted += ' '; continue; }
            if (c == '"') quoted += '"';
            quoted += c;
        }
        quoted += '"';
        return quoted;
    }
}
