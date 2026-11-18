#pragma once

// =============================================================================
// MINI JSON -- a small strict JSON reader for the R8 bootstrap file
// =============================================================================
// The bootstrap file (docs/architecture/9-bootstrap-format.md) is JSON, and
// vcpkg.json carries no JSON library: src/core is standard library only
// (src/core/README.md), so this is a small reader in MiniRegex's pattern: one
// header, one source, host tests (tests/core/MiniJsonTests.cpp).
//
// What it reads: RFC 8259 JSON text into a DOM (Value). Strict:
//   - one value, surrounded only by JSON whitespace (space, tab, CR, LF); a
//     UTF-8 byte-order mark at the start is skipped (Windows editors write one);
//   - numbers in JSON's grammar only: no leading '+', no leading zeros, no
//     '.5' or '5.', no hex, no NaN or Infinity (the format writes finite
//     numbers only). A number is parsed with std::from_chars, which rounds
//     correctly, so a value reads the same double as Python's float() reads
//     (the fit's own reader); one that overflows a double is an error;
//   - strings: the JSON escapes, \uXXXX with surrogate pairs, written out as
//     UTF-8; an unpaired surrogate is an error; a raw control character
//     (below 0x20) is an error; other bytes are copied as they are (plugin
//     names can be cp1252: not re-validated as UTF-8);
//   - objects keep their keys in file order; a key that repeats keeps every
//     entry and Find returns the LAST one, as Python's json module does;
//   - nesting is limited (kMaxDepth), so a hostile file cannot overflow the
//     stack: the parser recurses once per level.
// An error names the byte offset and what was expected; the DOM is not
// returned then.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Huginn::Core::Json
{
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    class Value
    {
    public:
        Value() noexcept;                     // defined where Payload is complete
        Value(const Value& o);
        Value& operator=(const Value& o);
        Value(Value&&) noexcept;              // defined where Payload is complete
        Value& operator=(Value&&) noexcept;
        ~Value();

        [[nodiscard]] Type GetType() const noexcept { return type_; }
        [[nodiscard]] bool IsNull() const noexcept { return type_ == Type::Null; }
        [[nodiscard]] bool IsBool() const noexcept { return type_ == Type::Bool; }
        [[nodiscard]] bool IsNumber() const noexcept { return type_ == Type::Number; }
        [[nodiscard]] bool IsString() const noexcept { return type_ == Type::String; }
        [[nodiscard]] bool IsArray() const noexcept { return type_ == Type::Array; }
        [[nodiscard]] bool IsObject() const noexcept { return type_ == Type::Object; }

        [[nodiscard]] bool AsBool() const noexcept { return bool_; }
        [[nodiscard]] double AsNumber() const noexcept { return number_; }
        /// The number was written as an integer (no fraction, no exponent).
        [[nodiscard]] bool IsIntegerText() const noexcept { return integerText_; }
        [[nodiscard]] const std::string& AsString() const noexcept;
        [[nodiscard]] const std::vector<Value>& Items() const noexcept;
        [[nodiscard]] const std::vector<std::pair<std::string, Value>>& Members() const noexcept;
        [[nodiscard]] std::size_t Size() const noexcept;

        /// The member `key` of an object (the last one when the key repeats);
        /// nullptr when absent or when this is not an object.
        [[nodiscard]] const Value* Find(std::string_view key) const noexcept;

        // Builders (the parser's; tests may use them too).
        static Value Null() { return Value{}; }
        static Value Bool(bool b);
        static Value Number(double v, bool integerText);
        static Value String(std::string s);
        static Value Array(std::vector<Value> items);
        static Value Object(std::vector<std::pair<std::string, Value>> members);

    private:
        // Strings, arrays and objects keep their contents behind one pointer,
        // so a number or a bool (most of a bootstrap file: theta is 94 x 243
        // numbers twice) is a few plain bytes and owns no container. In a Debug
        // build every container allocates a checking proxy when it is made,
        // which made the shipped file's parse ~0.7 s there (R8.2).
        struct Payload;
        Type type_ = Type::Null;
        bool bool_ = false;
        bool integerText_ = false;
        double number_ = 0.0;
        std::unique_ptr<Payload> p_;
    };

    inline constexpr int kMaxDepth = 64;

    struct ParseResult
    {
        std::optional<Value> value;   // empty on an error
        std::string error;            // what went wrong, with the byte offset
        std::size_t offset = 0;       // where (bytes from the start of the text)
        [[nodiscard]] bool Ok() const noexcept { return value.has_value(); }
    };

    [[nodiscard]] ParseResult Parse(std::string_view text);
}
