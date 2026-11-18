// Host tests for core/MiniJson.h: the strict JSON reader the R8 bootstrap
// reader (core/Bootstrap.h) sits on.

#include "core/MiniJson.h"

#include <doctest/doctest.h>

#include <ostream>

#include <cmath>
#include <string>

using namespace Huginn::Core;

namespace
{
    Json::Value Ok(std::string_view text)
    {
        auto r = Json::Parse(text);
        REQUIRE_MESSAGE(r.Ok(), r.error);
        return *r.value;
    }

    bool Bad(std::string_view text)
    {
        auto r = Json::Parse(text);
        return !r.Ok() && !r.error.empty();
    }
}

TEST_CASE("mini json: scalars")
{
    CHECK(Ok("null").IsNull());
    CHECK(Ok(" true ").AsBool());
    CHECK_FALSE(Ok("false").AsBool());
    CHECK(Ok("0").AsNumber() == 0.0);
    CHECK(Ok("-0").AsNumber() == 0.0);
    CHECK(Ok("12").IsIntegerText());
    CHECK_FALSE(Ok("1.0").IsIntegerText());
    CHECK_FALSE(Ok("1e2").IsIntegerText());
    CHECK(Ok("-12.5e-1").AsNumber() == -1.25);
    CHECK(Ok("1E+2").AsNumber() == 100.0);
    CHECK(Ok("\"a\\\"b\\\\c\\/d\\n\"").AsString() == "a\"b\\c/d\n");
}

TEST_CASE("mini json: numbers read as Python reads them (correctly rounded)")
{
    // 6 significant digits, as the bootstrap writes them
    CHECK(Ok("0.988677").AsNumber() == 0.988677);
    CHECK(Ok("73.4157").AsNumber() == 73.4157);
    CHECK(Ok("11.1111").AsNumber() == 11.1111);
    CHECK(Ok("2.69351").AsNumber() == 2.69351);
    CHECK(Ok("-1.23457e-05").AsNumber() == -1.23457e-05);
    CHECK(Ok("0.1").AsNumber() == 0.1);
    // an underflow is 0 (Python: float('1e-400') == 0.0); an overflow is an error
    CHECK(Ok("1e-400").AsNumber() == 0.0);
    CHECK(Ok("0.5e-400").AsNumber() == 0.0);
    CHECK(Bad("1e400"));
    CHECK(Bad("-1e999"));
}

TEST_CASE("mini json: strict number grammar")
{
    CHECK(Bad("+1"));
    CHECK(Bad("01"));
    CHECK(Bad(".5"));
    CHECK(Bad("5."));
    CHECK(Bad("1e"));
    CHECK(Bad("1e+"));
    CHECK(Bad("0x10"));
    CHECK(Bad("NaN"));
    CHECK(Bad("Infinity"));
    CHECK(Bad("-Infinity"));
    CHECK(Bad("-"));
}

TEST_CASE("mini json: strings and unicode escapes")
{
    CHECK(Ok("\"\\u0041\"").AsString() == "A");
    CHECK(Ok("\"\\u00e9\"").AsString() == "\xC3\xA9");
    CHECK(Ok("\"\\u20ac\"").AsString() == "\xE2\x82\xAC");
    CHECK(Ok("\"\\ud83d\\ude00\"").AsString() == "\xF0\x9F\x98\x80");
    CHECK(Bad("\"\\ud83d\""));
    CHECK(Bad("\"\\ude00\""));
    CHECK(Bad("\"\\ud83dx\""));
    CHECK(Bad("\"\\x41\""));
    CHECK(Bad("\"\\u12\""));
    CHECK(Bad("\"abc"));
    CHECK(Bad("\"a\nb\""));   // a raw control character
    // raw high bytes are copied (a cp1252 plugin name)
    CHECK(Ok("\"Bj\xF6rn\"").AsString() == "Bj\xF6rn");
}

TEST_CASE("mini json: arrays and objects")
{
    const auto v = Ok(R"({"a":[1,2,{"b":null}],"c":{},"d":[] , "e" : "x"})");
    REQUIRE(v.IsObject());
    CHECK(v.Size() == 4);
    REQUIRE(v.Find("a"));
    CHECK(v.Find("a")->Size() == 3);
    CHECK(v.Find("a")->Items()[2].Find("b")->IsNull());
    CHECK(v.Find("c")->IsObject());
    CHECK(v.Find("d")->IsArray());
    CHECK(v.Find("e")->AsString() == "x");
    CHECK(v.Find("zz") == nullptr);
    CHECK(Ok("[1]").Find("a") == nullptr);   // not an object
    // a repeated key: the last wins, as Python's json does
    CHECK(Ok(R"({"k":1,"k":2})").Find("k")->AsNumber() == 2.0);
    // members keep file order
    const auto o = Ok(R"({"z":1,"a":2})");
    CHECK(o.Members()[0].first == "z");
}

TEST_CASE("mini json: malformed structure")
{
    CHECK(Bad(""));
    CHECK(Bad("   "));
    CHECK(Bad("[1,]"));
    CHECK(Bad("[1 2]"));
    CHECK(Bad("{\"a\" 1}"));
    CHECK(Bad("{\"a\":1,}"));
    CHECK(Bad("{a:1}"));
    CHECK(Bad("[1] 2"));
    CHECK(Bad("tru"));
    CHECK(Bad("nul"));
    CHECK(Bad("[\"a\""));
    CHECK(Bad("{\"a\":1"));
}

TEST_CASE("mini json: nesting is limited, a BOM is skipped")
{
    std::string deep(Json::kMaxDepth, '[');
    deep += std::string(Json::kMaxDepth, ']');
    CHECK(Json::Parse(deep).Ok());
    std::string tooDeep(Json::kMaxDepth + 1, '[');
    tooDeep += std::string(Json::kMaxDepth + 1, ']');
    CHECK(Bad(tooDeep));
    // a hostile depth fails cleanly rather than overflowing the stack
    std::string hostile(100000, '[');
    CHECK(Bad(hostile));
    CHECK(Ok("\xEF\xBB\xBF{\"a\":1}").Find("a")->AsNumber() == 1.0);
}

TEST_CASE("mini json: the error names the offset")
{
    auto r = Json::Parse("[1, x]");
    REQUIRE_FALSE(r.Ok());
    CHECK(r.offset == 4);
    CHECK(r.error.find("offset 4") != std::string::npos);
}
