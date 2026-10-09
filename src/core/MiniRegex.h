#pragma once

// =============================================================================
// MINI REGEX -- a small backtracking regex for the effect mapper's tables
// =============================================================================
// The effect mapper (core/EffectRules.cpp) classifies a magic effect by its
// keywords, its English name and its description through ordered pattern
// tables, ported from the Python reference extractor that measured coverage
// (doc 9, "Needs and effects, enumerated"; tools/effects/reference/).
// std::regex is not used: it has no lookbehind, and MSVC's implementation is
// slow in Debug builds (which the user plays) and recurses once per character,
// which can overflow the stack on a long description.
//
// Semantics: Python's `re` on BYTES (ASCII classes), which is what the tables
// were checked against (tests/core/fixtures/regex_oracle.csv). Supported:
//   literals; `.` (not '\n'); classes `[a-z]`, `[^...]`; escapes \b \B \w \W
//   \d \D \s \S \n \t \r and escaped punctuation; groups `(...)` (capturing),
//   `(?:...)`; lookaround `(?=...)`, `(?!...)`, `(?<=...)`, `(?<!...)`
//   (lookbehind fixed-width, as in Python); alternation `|`; quantifiers `?`,
//   `*`, `+`, `{n}`, `{n,}`, `{,m}`, `{n,m}`, `{,}`, greedy or lazy; anchors
//   `^` and `$` (end, or before a final '\n').
// Anything else is a COMPILE ERROR rather than a silent difference: backrefs,
// named groups, flags, possessive quantifiers, a repeated repeat (`a**`,
// `x{2}{3}`), variable-width lookbehind (anywhere, even inside `{0}`), a
// repeated group that can match empty (`(?:|a)+`, `(\b)*`: Python iterates
// those differently), a NUL byte in the pattern, an escape it does not know. The tables lower-case their input and are written in lower
// case (keyword tables are matched as written).
//
// Engine: the pattern compiles to a small instruction list run by a
// backtracking VM with an explicit, heap-allocated backtrack stack, so the C++
// stack depth does not grow with the text or with how often a group repeats
// (only with how deeply lookarounds nest in the pattern). A quantified single
// atom counts its run and backtracks by count; a lookbehind is tried at the one
// start its fixed width allows; a search skips start bytes no match can begin
// with. A search has a step budget (4M instructions): past it, it answers "no
// match" and BudgetExceeded() counts it, so a pathological pattern/text pair
// cannot hang the catalog's worker.
//
// Speed: still slower than Python's sre on some nested quantifiers (e.g.
// `.{0,40}.{0,40}.{0,40}z` on a long run of x, or `(a|b)*c` on 100k
// characters, which the budget stops). No rule pattern has that shape.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Huginn::Core
{
    class MiniRegex
    {
    public:
        struct Span
        {
            std::ptrdiff_t begin = -1;
            std::ptrdiff_t end = -1;
            [[nodiscard]] bool Matched() const noexcept { return begin >= 0 && end >= begin; }
        };

        struct Match
        {
            Span whole;
            std::vector<Span> groups;  // capture groups 1..n at index 0..n-1

            /// Text of capture group `n` (1-based; 0 = the whole match), or ""
            /// if it did not take part.
            [[nodiscard]] std::string_view Group(std::string_view text, std::size_t n) const;
        };

        MiniRegex();
        explicit MiniRegex(std::string_view pattern);
        ~MiniRegex();
        MiniRegex(MiniRegex&&) noexcept;
        MiniRegex& operator=(MiniRegex&&) noexcept;
        MiniRegex(const MiniRegex&) = delete;
        MiniRegex& operator=(const MiniRegex&) = delete;

        /// Compile; false (and Error() says why) on a pattern outside the subset.
        bool Compile(std::string_view pattern);

        [[nodiscard]] bool Valid() const noexcept;
        [[nodiscard]] const std::string& Error() const noexcept { return error_; }
        [[nodiscard]] const std::string& Pattern() const noexcept { return pattern_; }
        [[nodiscard]] std::size_t GroupCount() const noexcept;

        /// Leftmost match anywhere in `text` (Python re.search). False on an
        /// invalid pattern.
        [[nodiscard]] bool Search(std::string_view text, Match* match = nullptr) const;

        /// Shorthand for Search(text) != false.
        [[nodiscard]] bool Contains(std::string_view text) const { return Search(text, nullptr); }

        /// How many searches (process-wide, all patterns) gave up at the step
        /// budget and answered "no match". Callers log it; 0 is the norm -- no
        /// rule table pattern comes near it on real text.
        [[nodiscard]] static std::size_t BudgetExceeded() noexcept;

        struct Impl;  // the compiled program (MiniRegex.cpp)

    private:
        std::string pattern_;
        std::string error_;
        std::unique_ptr<Impl> impl_;
    };
}
