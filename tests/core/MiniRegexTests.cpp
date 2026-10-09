// Host tests for core/MiniRegex.h: the regex subset the effect rules use.
// Hand cases here; tests/core/fixtures/regex_oracle.csv compares the engine
// with Python's `re` on every rule-table pattern over real effect names and
// descriptions (the last test case here).

#include "DumpCsv.h"
#include "core/EffectRules.h"
#include "core/MiniRegex.h"

#include <doctest/doctest.h>

#include <chrono>
#include <fstream>
#include <map>
#include <ostream>
#include <set>
#include <string>

using Huginn::Core::MiniRegex;

namespace
{
    // The matched text, or "<none>".
    std::string Find(const char* pattern, std::string_view text)
    {
        MiniRegex re(pattern);
        REQUIRE_MESSAGE(re.Valid(), re.Error());
        MiniRegex::Match m;
        if (!re.Search(text, &m)) return "<none>";
        return std::string(m.Group(text, 0));
    }
}

TEST_CASE("mini regex: literals, any, classes")
{
    CHECK(Find("abc", "xxabcxx") == "abc");
    CHECK(Find("a.c", "a-c") == "a-c");
    CHECK(Find("[a-c]+", "zzbcaz") == "bca");
    CHECK(Find("[^a-c]+", "abxyc") == "xy");
    CHECK(Find(R"(\d+)", "lvl 42!") == "42");
    CHECK(Find(R"(\w+)", "--foo_1 bar") == "foo_1");
    CHECK(Find(R"(\s)", "a b") == " ");
    CHECK(Find(R"(\(hidden\))", "x (hidden) y") == "(hidden)");
    CHECK(Find(R"([.!;])", "end; more") == ";");
    CHECK(Find("abc", "ab") == "<none>");
}

TEST_CASE("mini regex: anchors and word boundaries")
{
    CHECK(Find("^fear", "fear the night") == "fear");
    CHECK(Find("^fear", "no fear") == "<none>");
    CHECK(Find("dispel$", "x dispel") == "dispel");
    CHECK(Find("dispel$", "dispel x") == "<none>");
    CHECK(Find(R"(\bice\b)", "nice price") == "<none>");
    CHECK(Find(R"(\bice\b)", "ice spike") == "ice");
    CHECK(Find(R"(MagicSlow\b)", "A;MagicSlow;B") == "MagicSlow");
    CHECK(Find(R"(MagicSlow\b)", "MagicSlowTime") == "<none>");
    CHECK(Find("^$", "") == "");
    CHECK(Find(R"(\Bce)", "ice") == "ce");
}

TEST_CASE("mini regex: quantifiers, greedy and lazy")
{
    CHECK(Find("ab*", "abbbc") == "abbb");
    CHECK(Find("ab+?", "abbbc") == "ab");
    CHECK(Find("ab?c", "ac") == "ac");
    CHECK(Find("a{2,3}", "aaaa") == "aaa");
    CHECK(Find("a{2}", "aaaa") == "aa");
    CHECK(Find("a{2,}", "aaaa") == "aaaa");
    CHECK(Find("x.{0,3}y", "x12y") == "x12y");
    CHECK(Find("x.{0,3}y", "x1234y") == "<none>");
    CHECK(Find("(ab)+", "ababab") == "ababab");
    CHECK(Find("(a|ab)*c", "abac") == "abac");
    CHECK(Find("a{", "a{") == "a{");  // not a quantifier: a literal brace
    CHECK(Find("armou?r", "armor") == "armor");
}

TEST_CASE("mini regex: alternation picks the leftmost match, then the first branch")
{
    CHECK(Find("cat|category", "category") == "cat");
    CHECK(Find("category|cat", "category") == "category");
    CHECK(Find("dog|cat", "a cat and a dog") == "cat");
}

TEST_CASE("mini regex: capture groups")
{
    const std::string text = "fortify magicka 50";
    MiniRegex re("^fortify (health|magicka|stamina)");
    MiniRegex::Match m;
    REQUIRE(re.Search(text, &m));
    CHECK(m.Group(text, 1) == "magicka");
    CHECK(re.GroupCount() == 1);

    MiniRegex alt("(a)|(b)");
    REQUIRE(alt.Search("b", &m));
    CHECK_FALSE(m.groups[0].Matched());
    CHECK(m.Group("b", 2) == "b");

    // Non-capturing groups do not count.
    MiniRegex nc(R"(\b(one|block(?:ing)?) (cost|are))");
    CHECK(nc.GroupCount() == 2);
    const std::string t2 = "blocking are";
    REQUIRE(nc.Search(t2, &m));
    CHECK(m.Group(t2, 1) == "blocking");
}

TEST_CASE("mini regex: lookahead and lookbehind")
{
    CHECK(Find(R"(slow\b(?! time))", "slow time") == "<none>");
    CHECK(Find(R"(slow\b(?! time))", "slows foes, slow down") == "slow");
    CHECK(Find("damage(?! taken)", "damage taken, damage dealt") == "damage");
    CHECK(Find("(?<!more )damage", "more damage") == "<none>");
    CHECK(Find("(?<!more )damage", "deals damage") == "damage");
    CHECK(Find("(?<=x)y", "ay xy") == "y");
    CHECK(Find("(?=ab)a", "aab") == "a");
    // The reference's DMG rule shape.
    const char* dmg = R"(\b(deal|deals)\b.{0,40}(?<!more )(?<!extra )damage(?! taken))";
    CHECK(Find(dmg, "deals N fire damage") == "deals N fire damage");
    CHECK(Find(dmg, "deals more damage") == "<none>");
}

TEST_CASE("mini regex: invalid patterns are reported, not thrown")
{
    for (const char* bad : { "(", "a)", "[abc", "*a", "a**", "\\q", "(?P<n>x)", "a{3,2}", "\\", "x{2}{3}", "a*+",
                             "{2}", "\\1", "(?<=a+)b", "(?<=ab|c)d", "\\b*" }) {
        MiniRegex re;
        CHECK_FALSE(re.Compile(bad));
        CHECK_FALSE(re.Valid());
        CHECK_FALSE(re.Error().empty());
        CHECK_FALSE(re.Search("anything"));
    }
    MiniRegex nul;
    CHECK_FALSE(nul.Compile(std::string_view("a\0b", 3)));  // a NUL byte is rejected
}

TEST_CASE("mini regex: Python's edge rules -- $, \\B, braces, lookahead groups")
{
    CHECK(Find("a$", "a\n") == "a");             // $ also before a final newline
    CHECK(Find("a$", "a\nb") == "<none>");
    CHECK_FALSE(MiniRegex(R"(\B)").Contains(""));  // \B never matches an empty text (Python 3.13)
    CHECK(MiniRegex(R"(\B)").Contains("ab"));
    CHECK(Find("a{,3}", "aaaaa") == "aaa");     // {,m} is {0,m}
    CHECK(Find("a{,}", "aaaa") == "aaaa");      // {,} is {0,inf}
    CHECK(Find("a{}", "a{}") == "a{}");         // {} is a literal
    CHECK(Find("a{x}", "a{x}") == "a{x}");
    // A positive lookahead keeps its group.
    const std::string t = "foobar";
    MiniRegex la("(?=(foo))f");
    MiniRegex::Match m;
    REQUIRE(la.Search(t, &m));
    CHECK(m.Group(t, 1) == "foo");
    // ...and a failed path does not leave a stale one behind.
    MiniRegex stale("(?:(?=(a))b|c)");
    REQUIRE(stale.Search("ac", &m));
    CHECK_FALSE(m.groups[0].Matched());
}

TEST_CASE("mini regex: long input does not blow the stack")
{
    // Group repetition is iterative (an explicit backtrack stack): these
    // overflowed the old recursive matcher in Debug at ~1,000 characters.
    const std::string ab(20000, 'a');
    std::string abab;
    for (int i = 0; i < 10000; ++i) abab += "ab";
    std::string words;
    for (int i = 0; i < 4000; ++i) words += "word ";
    CHECK(Find("(?:ab)*$", abab).size() == abab.size());
    CHECK(Find("(?:a)*$", ab).size() == ab.size());
    CHECK(MiniRegex("(a|b)+c").Contains(abab + "c"));
    CHECK_FALSE(MiniRegex("(a|b)+c").Contains(abab));
    CHECK(MiniRegex(R"((?:\w+ )*x)").Contains(words + "x"));
    // A lookbehind is tried at the one start its fixed width allows: the DMG
    // rule on 40 KB of text stays fast.
    std::string prose;
    while (prose.size() < 40000) prose += "the target takes more damage over time and deals no ";
    const auto t0 = std::chrono::steady_clock::now();
    const MiniRegex dmg(R"(\b(deal|deals|dealing|does|inflicts?|inflicting)\b.{0,40}(?<!more )(?<!less )(?<!extra )(?<!additional )(?<!double )damage(?! taken))");
    REQUIRE(dmg.Valid());
    (void)dmg.Contains(prose);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 2000.0);

    // One stack frame per quantifier try, not per character.
    const std::string longText(200000, 'a');
    CHECK(Find("a*b", longText + "b").size() == longText.size() + 1);
    CHECK(MiniRegex(R"(\b(deal|deals)\b.{0,40}damage)").Contains(longText) == false);
}

// Every rule-table pattern against Python's `re` on real effect names,
// normalised descriptions and keyword lists (tests/core/fixtures/
// regex_oracle.csv, written by Python from the effect fixtures): same match or
// not, same span, same first group.
TEST_CASE("mini regex: agrees with Python re on the rule tables (oracle fixture)")
{
    std::ifstream in(std::string(HUGINN_REPO_ROOT) + "/tests/core/fixtures/regex_oracle.csv", std::ios::binary);
    REQUIRE(in.good());
    std::string line;
    REQUIRE(std::getline(in, line));
    std::map<std::string, MiniRegex> compiled;
    std::set<std::string> covered;
    int rows = 0, matches = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto f = Huginn::Test::SplitCsv(line);
        REQUIRE(f.size() == 6);
        auto it = compiled.find(f[0]);
        if (it == compiled.end()) it = compiled.emplace(f[0], MiniRegex(f[0])).first;
        REQUIRE_MESSAGE(it->second.Valid(), it->second.Error());
        covered.insert(f[0]);
        MiniRegex::Match m;
        const bool got = it->second.Search(f[1], &m);
        INFO("pattern /" << f[0] << "/ text '" << f[1] << "'");
        CHECK(got == (f[2] == "1"));
        if (got && f[2] == "1") {
            ++matches;
            CHECK(m.whole.begin == std::stoi(f[3]));
            CHECK(m.whole.end == std::stoi(f[4]));
            CHECK(std::string(m.Group(f[1], 1)) == f[5]);
        }
        ++rows;
    }
    // Every pattern the rules use is in the fixture: a changed table means
    // regenerating it (docs/testing/TESTING-INDEX.md, section 0).
    for (const auto& p : Huginn::Core::Effect::RulePatterns()) {
        INFO("pattern not in regex_oracle.csv: /" << p << "/");
        CHECK(covered.contains(p));
    }
    MESSAGE("regex oracle: " << rows << " rows, " << matches << " matches, " << compiled.size() << " patterns");
}

TEST_CASE("mini regex: verifier round 2 -- empty-matchable loops and dead lookbehinds refuse; a step budget")
{
    for (const char* bad : { "(?:|a)+", "(a|)+b", "(\\b)*a", "(?:(?<=a+)x){0}y", "(a?)*" }) {
        const MiniRegex re(bad);
        CHECK_MESSAGE(!re.Valid(), bad);
    }
    CHECK(MiniRegex("(?:ab)+").Valid());
    CHECK(MiniRegex("(?<=ab|cd)x").Valid());
    const std::size_t before = MiniRegex::BudgetExceeded();
    const MiniRegex slow("(a|b)*c");
    const std::string text(200000, 'a');
    CHECK_FALSE(slow.Contains(text));
    CHECK(MiniRegex::BudgetExceeded() > before);
}
