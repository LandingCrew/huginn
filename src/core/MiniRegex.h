#pragma once

// =============================================================================
// MINI REGEX -- a small backtracking regex for the effect mapper's tables
// =============================================================================
// The effect mapper (core/EffectRules.cpp) classifies a magic effect by its
// keywords, its English name and its description through ordered pattern
// tables, ported from the Python reference extractor that measured coverage
// (doc 9, "Needs and effects, enumerated"). std::regex is not used: it has no
// lookbehind, and MSVC's implementation is slow in Debug builds (which the
// user plays) and recurses once per character, which can overflow the stack
// on a long description.
//
// Supported (a subset of Python's `re`, enough for the tables):
//   literals; `.`; classes `[a-z]`, `[^...]`; escapes \b \B \w \W \d \D \s \S
//   and escaped punctuation; groups `(...)` (capturing), `(?:...)`;
//   lookaround `(?=...)`, `(?!...)`, `(?<=...)`, `(?<!...)`; alternation `|`;
//   quantifiers `?`, `*`, `+`, `{n}`, `{n,}`, `{n,m}`, each greedy or lazy
//   (a trailing `?`); anchors `^` and `$`.
// Not supported: backreferences, named groups, flags, Unicode classes. The
// engine is case-sensitive and byte-based: the tables lower-case their input
// and are written in lower case (keyword tables are matched as written).
//
// \w is [A-Za-z0-9_]. A word boundary \b is between a word and a non-word
// byte (or the text's edge), as in Python's ASCII mode.
//
// Backtracking depth is bounded by the pattern's nesting, not the text's
// length: a run of single atoms is matched in a loop, and a quantified single
// atom counts its matches first and then tries the continuation at each count.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
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

            /// Text of capture group `n` (1-based), or "" if it did not take part.
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

        [[nodiscard]] bool Valid() const noexcept { return root_ != nullptr && error_.empty(); }
        [[nodiscard]] const std::string& Error() const noexcept { return error_; }
        [[nodiscard]] const std::string& Pattern() const noexcept { return pattern_; }
        [[nodiscard]] std::size_t GroupCount() const noexcept { return groupCount_; }

        /// Leftmost match anywhere in `text` (Python re.search). False on an
        /// invalid pattern.
        [[nodiscard]] bool Search(std::string_view text, Match* match = nullptr) const;

        /// Shorthand for Search(text) != false.
        [[nodiscard]] bool Contains(std::string_view text) const { return Search(text, nullptr); }

        struct Node;  // the parsed pattern (MiniRegex.cpp)
        using Seq = std::vector<Node*>;

    private:
        std::string pattern_;
        std::string error_;
        std::vector<std::unique_ptr<Node>> nodes_;
        std::vector<std::unique_ptr<Seq>> seqs_;
        Node* root_ = nullptr;  // a capture-less group holding the alternatives
        std::size_t groupCount_ = 0;
        // Search prefilter: the bytes a match can start with (when it cannot be
        // empty), and whether every alternative is anchored at ^.
        std::array<std::uint64_t, 4> first_{};
        bool canBeEmpty_ = true;
        bool anchored_ = false;

        friend class MiniRegexParser;
        friend class MiniRegexMatcher;
    };
}
