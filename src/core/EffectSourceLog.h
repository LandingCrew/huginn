#pragma once

// =============================================================================
// EFFECT SOURCE LOG -- which active effect turned a status need on (0.23.41)
// =============================================================================
// regen_suppressed stayed on for 10-20 minutes at a time with no survival
// stage in force, and drain_health went on 340 times in three days of play
// (median 5 s), and the log could not say why: the effect walk logs nothing
// above trace. The walk now keeps the first effect that sets each flag, and
// the poll logs one line when the flag turns on (naming that effect) and one
// when it turns off -- transitions, not ticks (CLAUDE.md "Logging
// Principles").
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace Huginn::Core::Needs
{
    /// The active effect that set a flag: its magic effect and the spell (or
    /// potion, poison, disease, enchantment) that applied it.
    struct EffectSource
    {
        std::string effect;            // the magic effect's name ("" = unnamed)
        std::uint32_t effectID = 0;    // the magic effect's FormID
        std::string spell;             // the spell's name
        std::uint32_t spellID = 0;     // the spell's FormID, 0 = none (script-applied)
        std::string_view spellType;    // "spell", "disease", "potion", ... ("" = unknown)
        float magnitude = 0.0f;
        float duration = 0.0f;         // seconds, the effect's whole duration

        [[nodiscard]] bool Known() const noexcept { return effectID != 0 || spellID != 0; }
    };

    /// "<effect> [<MGEF>] from <spell> [<spell id>, <type>], magnitude M over D s".
    [[nodiscard]] inline std::string DescribeSource(const EffectSource& s)
    {
        const std::string_view effect = s.effect.empty() ? std::string_view("(unnamed)") : std::string_view(s.effect);
        std::string out = std::format("{} [{:08X}]", effect, s.effectID);
        if (s.spellID != 0) {
            const std::string_view spell = s.spell.empty() ? std::string_view("(unnamed)") : std::string_view(s.spell);
            out += std::format(" from {} [{:08X}{}{}]", spell, s.spellID, s.spellType.empty() ? "" : ", ", s.spellType);
        }
        else {
            out += " (no spell: script-applied)";
        }
        out += std::format(", magnitude {:g} over {:g} s", s.magnitude, s.duration);
        return out;
    }

    /// A step flag's transitions: Update returns the line to log when the flag
    /// turns on (naming the source) or off, and nothing on a poll that keeps it.
    class SourceWatch
    {
    public:
        explicit SourceWatch(std::string_view need) : m_need(need) {}

        [[nodiscard]] std::optional<std::string> Update(bool on, const EffectSource& source)
        {
            if (on == m_on) return std::nullopt;
            m_on = on;
            if (!on) return std::format("{} off", m_need);
            if (!source.Known()) return std::format("{} on (source not read)", m_need);
            return std::format("{} on: {}", m_need, DescribeSource(source));
        }

        /// A load: the next poll's state is reported as a transition if on.
        void Reset() noexcept { m_on = false; }

        [[nodiscard]] bool On() const noexcept { return m_on; }

    private:
        std::string_view m_need;
        bool m_on = false;
    };
}
