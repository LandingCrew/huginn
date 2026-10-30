#pragma once

// =============================================================================
// dMENU DEFAULTS -- the values dMenu writes for a fresh install (0.24.10)
// =============================================================================
// dMenu owns [Widget] and [Debug]: it reads its descriptor
// (Data/SKSE/Plugins/dmenu/customSettings/Huginn.json) and, when its INI does
// not exist yet, writes every setting at the descriptor's "default" into it
// (docs/architecture/7-dmenu-integration.md). Those defaults are therefore
// the shipped [Widget] and [Debug] values, and this turns the descriptor into
// exactly that INI:
//   - checkbox: "true" / "false" (a missing default is false);
//   - slider:   the default as a float, printed as C's "%f" ("28.000000"),
//               what dMenu's std::to_string(float) writes (missing: 0);
//   - dropdown: the default's integer INDEX ("0"), not its label (missing: 0);
//   - textbox:  the string; keymap: the integer.
// Groups are walked; buttons (no "ini") are skipped. Any other type that names
// an "ini" key is an error: Huginn's descriptor has none, and a silent skip
// would leave a setting with no shipped default.
//
// Used two ways (settings/ShippedDefaults.cpp): as the bottom layer every
// [Widget]/[Debug] read is overlaid on, and as the file dMenu's Reset to
// Defaults writes in dMenu's place.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <string>
#include <string_view>
#include <vector>

namespace Huginn::Core::DMenu
{
    struct Entry
    {
        std::string section;
        std::string key;
        std::string value;   // as dMenu serializes it
    };

    struct DefaultsResult
    {
        std::vector<Entry> entries;   // descriptor order
        std::string error;            // empty on success
        [[nodiscard]] bool Ok() const noexcept { return error.empty(); }
    };

    /// @brief Every setting's default in a dMenu descriptor, as dMenu writes it.
    [[nodiscard]] DefaultsResult DefaultsFromDescriptor(std::string_view json);

    /// @brief The entries as an INI: one [section] per section, in first-seen
    /// order, each key on its own `key = value` line; LF line ends.
    [[nodiscard]] std::string ToIniText(const std::vector<Entry>& entries);
}
