#pragma once

#include <cmath>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace Huginn::Telemetry::Json
{
    // =========================================================================
    // JSON LINES WRITER HELPERS (decision log)
    // =========================================================================
    // Minimal, allocation-friendly JSON emission for the telemetry file. No
    // external JSON library is in vcpkg.json, and the decision log only ever
    // WRITES flat records, so a handful of append helpers is all it needs.
    //
    // Pure and header-only: no RE:: types, no IO, safe on any thread. Tested by
    // RunTelemetryFormatTests().
    // =========================================================================

    /// Append `s` as a quoted JSON string, escaping quote, backslash and every
    /// control character (< 0x20). Bytes >= 0x80 pass through untouched: plugin
    /// filenames are the only free text logged and the file is read as UTF-8.
    inline void AppendEscaped(std::string& out, std::string_view s)
    {
        static constexpr char kHex[] = "0123456789abcdef";
        out += '"';
        for (const char ch : s) {
            const auto u = static_cast<unsigned char>(ch);
            switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (u < 0x20) {
                    out += "\\u00";
                    out += kHex[(u >> 4) & 0xF];
                    out += kHex[u & 0xF];
                } else {
                    out += ch;
                }
                break;
            }
        }
        out += '"';
    }

    /// Append a float with 4 significant digits. NaN and +/-inf have no JSON
    /// spelling, so they become `null` rather than an unparseable token.
    inline void AppendFloat(std::string& out, float v)
    {
        if (!std::isfinite(v)) {
            out += "null";
            return;
        }
        std::format_to(std::back_inserter(out), "{:.4g}", v);
    }

    /// Append `"key":` (no leading comma).
    inline void AppendKey(std::string& out, std::string_view key)
    {
        AppendEscaped(out, key);
        out += ':';
    }

    /// Append an unsigned integer as a JSON string "0x..." (JSON has no hex
    /// numbers; tag bitfields are easier to read and diff this way).
    inline void AppendHexString(std::string& out, std::uint64_t v)
    {
        std::format_to(std::back_inserter(out), "\"0x{:X}\"", v);
    }

    // =========================================================================
    // Object builder: tracks the comma between members.
    // =========================================================================
    // Usage:
    //   Object o(out);          // writes '{'
    //   o.Str("t", "imp");
    //   o.Num("u", 1.25f);
    //   o.Close();              // writes '}'
    // Raw nested values: o.Key("p"); <append value to out>;
    // =========================================================================
    class Object
    {
    public:
        explicit Object(std::string& out) : m_out(out) { m_out += '{'; }

        Object(const Object&) = delete;
        Object& operator=(const Object&) = delete;

        /// Write the separator and `"key":`; the caller appends the value.
        void Key(std::string_view key)
        {
            if (!m_first) m_out += ',';
            m_first = false;
            AppendKey(m_out, key);
        }

        void Str(std::string_view key, std::string_view value) { Key(key); AppendEscaped(m_out, value); }
        void Num(std::string_view key, float value) { Key(key); AppendFloat(m_out, value); }
        void Int(std::string_view key, std::int64_t value) { Key(key); std::format_to(std::back_inserter(m_out), "{}", value); }
        void UInt(std::string_view key, std::uint64_t value) { Key(key); std::format_to(std::back_inserter(m_out), "{}", value); }
        void Bool(std::string_view key, bool value) { Key(key); m_out += value ? "true" : "false"; }
        void Null(std::string_view key) { Key(key); m_out += "null"; }
        void Hex(std::string_view key, std::uint64_t value) { Key(key); AppendHexString(m_out, value); }

        void Close() { m_out += '}'; }

        [[nodiscard]] std::string& Out() noexcept { return m_out; }

    private:
        std::string& m_out;
        bool m_first = true;
    };

}  // namespace Huginn::Telemetry::Json
