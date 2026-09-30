#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace Huginn::Telemetry
{
    // =========================================================================
    // STABLE ITEM KEY (decision log)
    // =========================================================================
    // A runtime FormID's top byte is the originating plugin's load-order index,
    // so the same Fireball is 0x0001C789 for everyone but a mod's Fireball
    // variant is 0x2A000D62 for one player and 0x31000D62 for the next. To
    // aggregate files from many players the log keys items by
    //   <originating plugin filename> "|" <load-order-independent local id>
    //
    //   Regular plugin (index 0x00..0xFD): local = id & 0x00FFFFFF, 6 hex digits
    //   Light plugin   (top byte 0xFE):    local = id & 0x00000FFF, 3 hex digits
    //                                      (bits 12..23 are the ESL slot index,
    //                                      which is load-order dependent)
    //   Runtime-created (top byte 0xFF):   no stable identity ("~dyn") -- player-
    //                                      brewed potions, enchanted copies, ...
    //
    // Pure: takes a plain uint32_t (RE::FormID is one) so it is testable without
    // a game. The plugin filename is resolved by DecisionLog on the producer
    // thread (form->GetFile(0)) and cached. See docs/architecture/9-telemetry.md.
    // =========================================================================

    struct LocalId
    {
        bool dynamic = false;   // 0xFF: runtime-created, no stable key
        bool light = false;     // 0xFE: ESL / light plugin
        std::uint32_t local = 0;
    };

    [[nodiscard]] constexpr LocalId SplitFormID(std::uint32_t id) noexcept
    {
        const std::uint32_t top = id >> 24;
        if (top == 0xFF) {
            return { true, false, id & 0x00FFFFFFu };
        }
        if (top == 0xFE) {
            return { false, true, id & 0x00000FFFu };
        }
        return { false, false, id & 0x00FFFFFFu };
    }

    /// Placeholder plugin name when a static form reports no originating file.
    inline constexpr std::string_view UNKNOWN_PLUGIN = "?";
    /// Key for runtime-created forms.
    inline constexpr std::string_view DYNAMIC_KEY = "~dyn";

    [[nodiscard]] inline std::string FormatItemKey(std::string_view pluginFile, LocalId id)
    {
        if (id.dynamic) {
            return std::string(DYNAMIC_KEY);
        }
        const std::string_view plugin = pluginFile.empty() ? UNKNOWN_PLUGIN : pluginFile;
        return id.light
            ? std::format("{}|{:03X}", plugin, id.local)
            : std::format("{}|{:06X}", plugin, id.local);
    }

    // Compile-time checks (runtime ones live in RunTelemetryFormatTests).
    static_assert(SplitFormID(0x00012FCDu).local == 0x012FCDu && !SplitFormID(0x00012FCDu).light);
    static_assert(SplitFormID(0xFE012ABCu).light && SplitFormID(0xFE012ABCu).local == 0xABCu);
    static_assert(SplitFormID(0xFF000800u).dynamic);

}  // namespace Huginn::Telemetry
