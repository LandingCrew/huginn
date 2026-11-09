#include "MiniJson.h"

#include <charconv>
#include <cmath>
#include <string>
#include <system_error>

namespace Huginn::Core::Json
{
    struct Value::Payload
    {
        std::string string;
        std::vector<Value> items;
        std::vector<std::pair<std::string, Value>> members;
    };

    namespace
    {
        const std::string kEmptyString;
        const std::vector<Value> kEmptyItems;
        const std::vector<std::pair<std::string, Value>> kEmptyMembers;
    }

    Value::Value(const Value& o)
        : type_(o.type_), bool_(o.bool_), integerText_(o.integerText_), number_(o.number_),
          p_(o.p_ ? std::make_unique<Payload>(*o.p_) : nullptr)
    {
    }

    Value& Value::operator=(const Value& o)
    {
        if (this != &o) {
            type_ = o.type_;
            bool_ = o.bool_;
            integerText_ = o.integerText_;
            number_ = o.number_;
            p_ = o.p_ ? std::make_unique<Payload>(*o.p_) : nullptr;
        }
        return *this;
    }

    Value::Value() noexcept = default;
    Value::Value(Value&&) noexcept = default;
    Value& Value::operator=(Value&&) noexcept = default;
    Value::~Value() = default;

    const std::string& Value::AsString() const noexcept
    {
        return (type_ == Type::String && p_) ? p_->string : kEmptyString;
    }

    const std::vector<Value>& Value::Items() const noexcept
    {
        return (type_ == Type::Array && p_) ? p_->items : kEmptyItems;
    }

    const std::vector<std::pair<std::string, Value>>& Value::Members() const noexcept
    {
        return (type_ == Type::Object && p_) ? p_->members : kEmptyMembers;
    }

    std::size_t Value::Size() const noexcept
    {
        return type_ == Type::Array ? Items().size() : type_ == Type::Object ? Members().size() : 0;
    }

    const Value* Value::Find(std::string_view key) const noexcept
    {
        if (type_ != Type::Object || !p_) return nullptr;
        for (auto it = p_->members.rbegin(); it != p_->members.rend(); ++it) {
            if (it->first == key) return &it->second;
        }
        return nullptr;
    }

    Value Value::Bool(bool b)
    {
        Value v;
        v.type_ = Type::Bool;
        v.bool_ = b;
        return v;
    }

    Value Value::Number(double d, bool integerText)
    {
        Value v;
        v.type_ = Type::Number;
        v.number_ = d;
        v.integerText_ = integerText;
        return v;
    }

    Value Value::String(std::string s)
    {
        Value v;
        v.type_ = Type::String;
        v.p_ = std::make_unique<Payload>();
        v.p_->string = std::move(s);
        return v;
    }

    Value Value::Array(std::vector<Value> items)
    {
        Value v;
        v.type_ = Type::Array;
        v.p_ = std::make_unique<Payload>();
        v.p_->items = std::move(items);
        return v;
    }

    Value Value::Object(std::vector<std::pair<std::string, Value>> members)
    {
        Value v;
        v.type_ = Type::Object;
        v.p_ = std::make_unique<Payload>();
        v.p_->members = std::move(members);
        return v;
    }

    namespace
    {
        class Parser
        {
        public:
            explicit Parser(std::string_view text) : t_(text) {}

            ParseResult Run()
            {
                ParseResult r;
                if (t_.size() >= 3 && static_cast<unsigned char>(t_[0]) == 0xEF &&
                    static_cast<unsigned char>(t_[1]) == 0xBB && static_cast<unsigned char>(t_[2]) == 0xBF) {
                    p_ = 3;
                }
                SkipWs();
                Value v;
                if (!ParseValue(v, 0)) {
                    r.error = err_;
                    r.offset = errAt_;
                    return r;
                }
                SkipWs();
                if (p_ != t_.size()) {
                    Fail("end of text expected after the value");
                    r.error = err_;
                    r.offset = errAt_;
                    return r;
                }
                r.value = std::move(v);
                return r;
            }

        private:
            std::string_view t_;
            std::size_t p_ = 0;
            std::string err_;
            std::size_t errAt_ = 0;

            bool Fail(std::string_view what)
            {
                if (err_.empty()) {
                    errAt_ = p_;
                    err_ = "offset " + std::to_string(p_) + ": " + std::string(what);
                }
                return false;
            }

            void SkipWs() noexcept
            {
                while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_;
            }

            bool Literal(std::string_view word)
            {
                if (t_.substr(p_, word.size()) != word) return Fail("invalid literal");
                p_ += word.size();
                return true;
            }

            bool ParseValue(Value& out, int depth)
            {
                if (p_ >= t_.size()) return Fail("a value expected, found the end of the text");
                const char c = t_[p_];
                switch (c) {
                case '{': return ParseObject(out, depth + 1);
                case '[': return ParseArray(out, depth + 1);
                case '"': {
                    std::string s;
                    if (!ParseString(s)) return false;
                    out = Value::String(std::move(s));
                    return true;
                }
                case 't':
                    if (!Literal("true")) return false;
                    out = Value::Bool(true);
                    return true;
                case 'f':
                    if (!Literal("false")) return false;
                    out = Value::Bool(false);
                    return true;
                case 'n':
                    if (!Literal("null")) return false;
                    out = Value::Null();
                    return true;
                default:
                    if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
                    return Fail("a value expected");
                }
            }

            bool ParseObject(Value& out, int depth)
            {
                if (depth > kMaxDepth) return Fail("nesting too deep");
                ++p_;   // '{'
                std::vector<std::pair<std::string, Value>> members;
                SkipWs();
                if (p_ < t_.size() && t_[p_] == '}') {
                    ++p_;
                    out = Value::Object(std::move(members));
                    return true;
                }
                while (true) {
                    SkipWs();
                    if (p_ >= t_.size() || t_[p_] != '"') return Fail("a string key expected");
                    std::string key;
                    if (!ParseString(key)) return false;
                    SkipWs();
                    if (p_ >= t_.size() || t_[p_] != ':') return Fail("':' expected");
                    ++p_;
                    SkipWs();
                    Value v;
                    if (!ParseValue(v, depth)) return false;
                    members.emplace_back(std::move(key), std::move(v));
                    SkipWs();
                    if (p_ >= t_.size()) return Fail("',' or '}' expected, found the end of the text");
                    if (t_[p_] == ',') {
                        ++p_;
                        continue;
                    }
                    if (t_[p_] == '}') {
                        ++p_;
                        break;
                    }
                    return Fail("',' or '}' expected");
                }
                out = Value::Object(std::move(members));
                return true;
            }

            bool ParseArray(Value& out, int depth)
            {
                if (depth > kMaxDepth) return Fail("nesting too deep");
                ++p_;   // '['
                std::vector<Value> items;
                SkipWs();
                if (p_ < t_.size() && t_[p_] == ']') {
                    ++p_;
                    out = Value::Array(std::move(items));
                    return true;
                }
                while (true) {
                    SkipWs();
                    Value v;
                    if (!ParseValue(v, depth)) return false;
                    items.push_back(std::move(v));
                    SkipWs();
                    if (p_ >= t_.size()) return Fail("',' or ']' expected, found the end of the text");
                    if (t_[p_] == ',') {
                        ++p_;
                        continue;
                    }
                    if (t_[p_] == ']') {
                        ++p_;
                        break;
                    }
                    return Fail("',' or ']' expected");
                }
                out = Value::Array(std::move(items));
                return true;
            }

            bool ParseNumber(Value& out)
            {
                const std::size_t start = p_;
                bool integer = true;
                if (t_[p_] == '-') ++p_;
                if (p_ >= t_.size()) return Fail("a digit expected");
                if (t_[p_] == '0') {
                    ++p_;
                    if (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') return Fail("a leading zero is not JSON");
                }
                else if (t_[p_] >= '1' && t_[p_] <= '9') {
                    while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
                }
                else {
                    return Fail("a digit expected");
                }
                if (p_ < t_.size() && t_[p_] == '.') {
                    integer = false;
                    ++p_;
                    if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9') return Fail("a digit expected after '.'");
                    while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
                }
                if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
                    integer = false;
                    ++p_;
                    if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
                    if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9') return Fail("a digit expected in the exponent");
                    while (p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9') ++p_;
                }
                double d = 0.0;
                const char* first = t_.data() + start;
                const char* last = t_.data() + p_;
                const auto res = std::from_chars(first, last, d);
                if (res.ec == std::errc::result_out_of_range || !std::isfinite(d)) {
                    // from_chars reports overflow AND underflow as out of range.
                    // An underflow (1e-400, 0.000...001) is a finite value
                    // Python reads as 0.0: a negative exponent, or no exponent
                    // and a zero integer part. Anything else overflowed.
                    const std::string_view num(first, static_cast<std::size_t>(last - first));
                    const auto e = num.find_first_of("eE");
                    const bool negExp = e != std::string_view::npos && e + 1 < num.size() && num[e + 1] == '-';
                    const std::string_view mant = num.substr(num[0] == '-' ? 1 : 0, e == std::string_view::npos
                                                                                       ? std::string_view::npos
                                                                                       : e - (num[0] == '-' ? 1 : 0));
                    const bool zeroInt = mant.starts_with("0");
                    if (!(negExp || (e == std::string_view::npos && zeroInt))) {
                        p_ = start;
                        return Fail("number out of range");
                    }
                    d = num[0] == '-' ? -0.0 : 0.0;
                }
                else if (res.ec != std::errc{} || res.ptr != last) {
                    p_ = start;
                    return Fail("invalid number");
                }
                out = Value::Number(d, integer);
                return true;
            }

            static void AppendUtf8(std::string& s, std::uint32_t cp)
            {
                if (cp < 0x80) {
                    s += static_cast<char>(cp);
                }
                else if (cp < 0x800) {
                    s += static_cast<char>(0xC0 | (cp >> 6));
                    s += static_cast<char>(0x80 | (cp & 0x3F));
                }
                else if (cp < 0x10000) {
                    s += static_cast<char>(0xE0 | (cp >> 12));
                    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    s += static_cast<char>(0x80 | (cp & 0x3F));
                }
                else {
                    s += static_cast<char>(0xF0 | (cp >> 18));
                    s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    s += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }

            bool Hex4(std::uint32_t& out)
            {
                if (p_ + 4 > t_.size()) return Fail("four hex digits expected after \\u");
                std::uint32_t v = 0;
                for (int i = 0; i < 4; ++i) {
                    const char c = t_[p_ + static_cast<std::size_t>(i)];
                    v <<= 4;
                    if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
                    else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
                    else return Fail("four hex digits expected after \\u");
                }
                p_ += 4;
                out = v;
                return true;
            }

            bool ParseString(std::string& out)
            {
                ++p_;   // opening quote
                while (true) {
                    if (p_ >= t_.size()) return Fail("unterminated string");
                    const char c = t_[p_];
                    if (c == '"') {
                        ++p_;
                        return true;
                    }
                    if (static_cast<unsigned char>(c) < 0x20) return Fail("a control character inside a string");
                    if (c != '\\') {
                        out += c;
                        ++p_;
                        continue;
                    }
                    ++p_;
                    if (p_ >= t_.size()) return Fail("unterminated escape");
                    const char e = t_[p_++];
                    switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        std::uint32_t cp = 0;
                        if (!Hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (p_ + 2 > t_.size() || t_[p_] != '\\' || t_[p_ + 1] != 'u') {
                                return Fail("an unpaired high surrogate");
                            }
                            p_ += 2;
                            std::uint32_t lo = 0;
                            if (!Hex4(lo)) return false;
                            if (lo < 0xDC00 || lo > 0xDFFF) return Fail("an unpaired high surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return Fail("an unpaired low surrogate");
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default:
                        --p_;
                        return Fail("an unknown escape");
                    }
                }
            }
        };
    }

    ParseResult Parse(std::string_view text)
    {
        return Parser(text).Run();
    }
}
