#include "MiniRegex.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Huginn::Core
{
    namespace
    {
        // =====================================================================
        // Byte sets
        // =====================================================================
        using Set = std::array<std::uint64_t, 4>;

        void Add(Set& s, unsigned char c) noexcept { s[c >> 6] |= (std::uint64_t{ 1 } << (c & 63)); }
        [[nodiscard]] bool Has(const Set& s, unsigned char c) noexcept { return (s[c >> 6] >> (c & 63)) & 1u; }
        void AddRange(Set& s, unsigned char a, unsigned char b) noexcept
        {
            for (unsigned v = a; v <= b; ++v) Add(s, static_cast<unsigned char>(v));
        }
        void Invert(Set& s) noexcept
        {
            for (auto& w : s) w = ~w;
        }
        void Merge(Set& s, const Set& o) noexcept
        {
            for (std::size_t i = 0; i < s.size(); ++i) s[i] |= o[i];
        }

        [[nodiscard]] bool IsWordByte(unsigned char c) noexcept
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }

        [[nodiscard]] Set WordSet() noexcept
        {
            Set s{};
            AddRange(s, 'a', 'z');
            AddRange(s, 'A', 'Z');
            AddRange(s, '0', '9');
            Add(s, '_');
            return s;
        }
        [[nodiscard]] Set DigitSet() noexcept
        {
            Set s{};
            AddRange(s, '0', '9');
            return s;
        }
        [[nodiscard]] Set SpaceSet() noexcept
        {
            Set s{};
            for (const char c : std::string_view(" \t\n\r\f\v")) Add(s, static_cast<unsigned char>(c));
            return s;
        }
        [[nodiscard]] Set AnySet() noexcept
        {
            Set s{};
            Invert(s);
            s['\n' >> 6] &= ~(std::uint64_t{ 1 } << ('\n' & 63));
            return s;
        }
        [[nodiscard]] Set OneByte(unsigned char c) noexcept
        {
            Set s{};
            Add(s, c);
            return s;
        }

        /// The class escape's set (\w \d \s and their negations); false if `e`
        /// is not one.
        [[nodiscard]] bool ClassEscape(char e, Set& out) noexcept
        {
            switch (e) {
                case 'w': out = WordSet(); return true;
                case 'd': out = DigitSet(); return true;
                case 's': out = SpaceSet(); return true;
                case 'W': out = WordSet(); Invert(out); return true;
                case 'D': out = DigitSet(); Invert(out); return true;
                case 'S': out = SpaceSet(); Invert(out); return true;
                default: return false;
            }
        }

        /// A literal escape (\. \( \- ... \n \t \r). False for any other letter
        /// or digit, so a typo or a backreference is an error, not a literal.
        [[nodiscard]] bool LiteralEscape(char e, unsigned char& out) noexcept
        {
            if (e == 'n') { out = '\n'; return true; }
            if (e == 't') { out = '\t'; return true; }
            if (e == 'r') { out = '\r'; return true; }
            const auto u = static_cast<unsigned char>(e);
            if (IsWordByte(u)) return false;
            out = u;
            return true;
        }

        // =====================================================================
        // The parsed pattern (compile time only)
        // =====================================================================
        struct Node;
        using Seq = std::vector<Node*>;

        struct Node
        {
            enum class Kind : std::uint8_t { Atom, WordBoundary, NotWordBoundary, Begin, End, Group, Repeat };
            enum class Look : std::uint8_t { None, Ahead, NegAhead, Behind, NegBehind };

            Kind kind = Kind::Atom;
            Set set{};                 // Atom
            std::vector<Seq*> alts;    // Group
            int capture = -1;
            Look look = Look::None;
            Node* child = nullptr;     // Repeat
            int min = 0;
            int max = -1;              // -1 = unbounded
            bool greedy = true;
        };

        struct Arena
        {
            std::vector<std::unique_ptr<Node>> nodes;
            std::vector<std::unique_ptr<Seq>> seqs;
            Node* NewNode(Node::Kind k)
            {
                nodes.push_back(std::make_unique<Node>());
                nodes.back()->kind = k;
                return nodes.back().get();
            }
            Seq* NewSeq()
            {
                seqs.push_back(std::make_unique<Seq>());
                return seqs.back().get();
            }
        };

        // =====================================================================
        // Parser (Python's sre_parse rules for the supported subset)
        // =====================================================================
        class Parser
        {
        public:
            Parser(Arena& a, std::string_view p, std::string& error) : a_(a), p_(p), error_(error) {}

            Node* Parse(std::size_t& groups)
            {
                if (p_.find('\0') != std::string_view::npos) {
                    Fail("NUL byte in pattern");
                    return nullptr;
                }
                auto* root = a_.NewNode(Node::Kind::Group);
                if (!ParseAlternatives(*root)) return nullptr;
                if (i_ != p_.size()) {
                    Fail("unbalanced ')'");
                    return nullptr;
                }
                groups = groups_;
                return root;
            }

        private:
            Arena& a_;
            std::string_view p_;
            std::string& error_;
            std::size_t i_ = 0;
            std::size_t groups_ = 0;

            bool Fail(const char* why)
            {
                if (error_.empty()) {
                    error_ = std::string(why) + " at " + std::to_string(i_) + " in /" + std::string(p_) + "/";
                }
                return false;
            }

            [[nodiscard]] bool AtEnd() const noexcept { return i_ >= p_.size(); }
            [[nodiscard]] char Peek(std::size_t ahead = 0) const noexcept
            {
                return i_ + ahead < p_.size() ? p_[i_ + ahead] : '\0';
            }
            [[nodiscard]] bool IsDigitAt(std::size_t at) const noexcept
            {
                return at < p_.size() && p_[at] >= '0' && p_[at] <= '9';
            }

            bool ParseAlternatives(Node& group)
            {
                for (;;) {
                    auto* seq = a_.NewSeq();
                    if (!ParseSequence(*seq)) return false;
                    group.alts.push_back(seq);
                    if (!AtEnd() && Peek() == '|') {
                        ++i_;
                        continue;
                    }
                    return true;
                }
            }

            bool ParseSequence(Seq& seq)
            {
                while (!AtEnd() && Peek() != '|' && Peek() != ')') {
                    Node* atom = nullptr;
                    if (!ParseAtom(atom)) return false;
                    if (!ParseQuantifier(atom)) return false;
                    seq.push_back(atom);
                }
                return true;
            }

            /// At a '{': does a Python brace quantifier follow? Fills bounds.
            /// "{}" and anything unclosed are literals; "{,}" is {0,inf}.
            bool BraceQuantifier(std::size_t at, int& mn, int& mx, std::size_t& endAt)
            {
                if (at >= p_.size() || p_[at] != '{') return false;
                std::size_t j = at + 1;
                if (j < p_.size() && p_[j] == '}') return false;
                std::string lo, hi;
                while (IsDigitAt(j)) lo += p_[j++];
                bool comma = false;
                if (j < p_.size() && p_[j] == ',') {
                    comma = true;
                    ++j;
                    while (IsDigitAt(j)) hi += p_[j++];
                }
                if (j >= p_.size() || p_[j] != '}') return false;
                if (!comma) hi = lo;
                if (lo.size() > 6 || hi.size() > 6) return false;
                mn = lo.empty() ? 0 : std::stoi(lo);
                mx = hi.empty() ? -1 : std::stoi(hi);
                endAt = j + 1;
                return true;
            }

            bool ParseAtom(Node*& out)
            {
                const char c = Peek();
                if (c == '(') return ParseGroup(out);
                if (c == '[') return ParseClass(out);
                if (c == '*' || c == '+' || c == '?') return Fail("nothing to repeat");
                {
                    int mn = 0, mx = 0;
                    std::size_t endAt = 0;
                    if (c == '{' && BraceQuantifier(i_, mn, mx, endAt)) return Fail("nothing to repeat");
                }
                if (c == '.') {
                    ++i_;
                    out = a_.NewNode(Node::Kind::Atom);
                    out->set = AnySet();
                    return true;
                }
                if (c == '^') {
                    ++i_;
                    out = a_.NewNode(Node::Kind::Begin);
                    return true;
                }
                if (c == '$') {
                    ++i_;
                    out = a_.NewNode(Node::Kind::End);
                    return true;
                }
                if (c == '\\') {
                    ++i_;
                    if (AtEnd()) return Fail("trailing backslash");
                    const char e = p_[i_++];
                    if (e == 'b') { out = a_.NewNode(Node::Kind::WordBoundary); return true; }
                    if (e == 'B') { out = a_.NewNode(Node::Kind::NotWordBoundary); return true; }
                    Set s{};
                    if (ClassEscape(e, s)) {
                        out = a_.NewNode(Node::Kind::Atom);
                        out->set = s;
                        return true;
                    }
                    unsigned char lit = 0;
                    if (!LiteralEscape(e, lit)) return Fail("unsupported escape");
                    out = a_.NewNode(Node::Kind::Atom);
                    out->set = OneByte(lit);
                    return true;
                }
                ++i_;
                out = a_.NewNode(Node::Kind::Atom);
                out->set = OneByte(static_cast<unsigned char>(c));
                return true;
            }

            bool ParseGroup(Node*& out)
            {
                ++i_;  // '('
                auto* g = a_.NewNode(Node::Kind::Group);
                if (Peek() == '?') {
                    const char a = Peek(1);
                    const char b = Peek(2);
                    if (a == ':') { i_ += 2; }
                    else if (a == '=') { i_ += 2; g->look = Node::Look::Ahead; }
                    else if (a == '!') { i_ += 2; g->look = Node::Look::NegAhead; }
                    else if (a == '<' && b == '=') { i_ += 3; g->look = Node::Look::Behind; }
                    else if (a == '<' && b == '!') { i_ += 3; g->look = Node::Look::NegBehind; }
                    else return Fail("unsupported group");
                }
                else {
                    g->capture = static_cast<int>(groups_++);
                }
                if (!ParseAlternatives(*g)) return false;
                if (AtEnd() || Peek() != ')') return Fail("missing ')'");
                ++i_;
                out = g;
                return true;
            }

            bool ParseClass(Node*& out)
            {
                ++i_;  // '['
                Set s{};
                bool negate = false;
                if (Peek() == '^') {
                    negate = true;
                    ++i_;
                }
                bool first = true;
                while (!AtEnd() && (Peek() != ']' || first)) {
                    first = false;
                    unsigned char lo = 0;
                    const char c = p_[i_++];
                    if (c == '\\') {
                        if (AtEnd()) return Fail("trailing backslash in class");
                        const char e = p_[i_++];
                        Set es{};
                        if (ClassEscape(e, es)) {
                            Merge(s, es);
                            continue;
                        }
                        if (!LiteralEscape(e, lo)) return Fail("unsupported escape in class");
                    }
                    else {
                        lo = static_cast<unsigned char>(c);
                    }
                    if (Peek() == '-' && i_ + 1 < p_.size() && p_[i_ + 1] != ']') {
                        ++i_;
                        unsigned char hi = 0;
                        const char h = p_[i_++];
                        if (h == '\\') {
                            if (AtEnd()) return Fail("trailing backslash in class");
                            if (!LiteralEscape(p_[i_++], hi)) return Fail("bad range end");
                        }
                        else {
                            hi = static_cast<unsigned char>(h);
                        }
                        if (hi < lo) return Fail("bad range");
                        AddRange(s, lo, hi);
                    }
                    else {
                        Add(s, lo);
                    }
                }
                if (AtEnd()) return Fail("missing ']'");
                ++i_;  // ']'
                if (negate) Invert(s);
                out = a_.NewNode(Node::Kind::Atom);
                out->set = s;
                return true;
            }

            bool ParseQuantifier(Node*& atom)
            {
                if (AtEnd()) return true;
                int mn = 0;
                int mx = -1;
                const char c = Peek();
                if (c == '*') { ++i_; mn = 0; mx = -1; }
                else if (c == '+') { ++i_; mn = 1; mx = -1; }
                else if (c == '?') { ++i_; mn = 0; mx = 1; }
                else if (c == '{') {
                    std::size_t endAt = 0;
                    if (!BraceQuantifier(i_, mn, mx, endAt)) return true;  // a literal '{'
                    i_ = endAt;
                    if (mx >= 0 && mx < mn) return Fail("min repeat greater than max repeat");
                }
                else {
                    return true;
                }
                const auto k = atom->kind;
                if (k == Node::Kind::Begin || k == Node::Kind::End || k == Node::Kind::WordBoundary ||
                    k == Node::Kind::NotWordBoundary || (k == Node::Kind::Group && atom->look != Node::Look::None)) {
                    return Fail("nothing to repeat");
                }
                auto* r = a_.NewNode(Node::Kind::Repeat);
                r->child = atom;
                r->min = mn;
                r->max = mx;
                if (Peek() == '?') {
                    ++i_;
                    r->greedy = false;
                }
                // A second quantifier (`a**`, `x{2}{3}`, possessive `a*+`) is an error,
                // as in Python (which reads `*+` as possessive: outside the subset).
                int m2 = 0, x2 = 0;
                std::size_t e2 = 0;
                if (Peek() == '*' || Peek() == '+' || Peek() == '?' || (Peek() == '{' && BraceQuantifier(i_, m2, x2, e2))) {
                    return Fail("multiple repeat");
                }
                atom = r;
                return true;
            }
        };

        // =====================================================================
        // Analysis: width (lookbehind) and first bytes (search prefilter)
        // =====================================================================
        struct Width
        {
            long long min = 0;
            long long max = 0;  // -1 = unbounded
        };

        Width WidthOf(const Node& n);
        Width WidthOfSeq(const Seq& s)
        {
            Width w;
            for (const auto* n : s) {
                const Width c = WidthOf(*n);
                w.min += c.min;
                w.max = (w.max < 0 || c.max < 0) ? -1 : w.max + c.max;
            }
            return w;
        }
        Width WidthOf(const Node& n)
        {
            switch (n.kind) {
                case Node::Kind::Atom: return { 1, 1 };
                case Node::Kind::Group: {
                    if (n.look != Node::Look::None) return { 0, 0 };
                    Width w{ -1, 0 };
                    for (const auto* alt : n.alts) {
                        const Width a = WidthOfSeq(*alt);
                        w.min = w.min < 0 ? a.min : std::min(w.min, a.min);
                        w.max = (w.max < 0 || a.max < 0) ? -1 : std::max(w.max, a.max);
                    }
                    if (w.min < 0) w.min = 0;
                    return w;
                }
                case Node::Kind::Repeat: {
                    const Width c = WidthOf(*n.child);
                    Width w;
                    w.min = c.min * n.min;
                    w.max = (c.max < 0 || n.max < 0) ? -1 : c.max * n.max;
                    return w;
                }
                default: return { 0, 0 };
            }
        }

        bool FirstOfNode(const Node& n, Set& first);
        /// Adds the bytes a non-empty match of `s` can start with; true if `s`
        /// can match the empty string. Zero-width assertions are transparent.
        bool FirstOfSeq(const Seq& s, Set& first)
        {
            for (const auto* n : s) {
                if (!FirstOfNode(*n, first)) return false;
            }
            return true;
        }
        bool FirstOfNode(const Node& n, Set& first)
        {
            switch (n.kind) {
                case Node::Kind::Atom: Merge(first, n.set); return false;
                case Node::Kind::Group: {
                    if (n.look != Node::Look::None) return true;
                    bool empty = false;
                    for (const auto* alt : n.alts) {
                        if (FirstOfSeq(*alt, first)) empty = true;
                    }
                    return empty;
                }
                case Node::Kind::Repeat: return FirstOfNode(*n.child, first) || n.min == 0;
                default: return true;
            }
        }

        // =====================================================================
        // Program
        // =====================================================================
        enum class Op : std::uint8_t
        {
            Atom,      // one byte in `set`
            AtomRep,   // a run of `set`, min..max, greedy or lazy
            Split,     // try x, then y
            Jmp,       // go to x
            Save,      // slot x = position
            Mark,      // loop register x = position
            Progress,  // fail if position == loop register x (an empty iteration)
            Bol, Eol, WordB, NotWordB,
            Look,      // run program x as lookaround `look` (width for behind)
            Match
        };

        struct Inst
        {
            Op op = Op::Match;
            Set set{};
            int x = 0;
            int y = 0;
            int min = 0;
            int max = -1;
            bool greedy = true;
            Node::Look look = Node::Look::None;
            int width = 0;
        };

        using Program = std::vector<Inst>;
    }

    struct MiniRegex::Impl
    {
        std::vector<Program> programs;  // [0] the pattern; the rest lookaround bodies
        std::size_t groups = 0;
        int marks = 0;
        Set first{};
        bool canBeEmpty = true;
        bool anchored = false;
    };

    namespace
    {
        class Compiler
        {
        public:
            Compiler(MiniRegex::Impl& impl, std::string& error) : impl_(impl), error_(error) {}

            bool CompileRoot(const Node& root)
            {
                if (!Validate(root)) return false;
                impl_.programs.emplace_back();
                if (!EmitAlternatives(0, root)) return false;
                Emit(0, Inst{ Op::Match });
                return true;
            }

        private:
            MiniRegex::Impl& impl_;
            std::string& error_;

            /// Rules checked over the whole pattern before emitting, so a part
            /// that emits nothing (inside {0}) is checked too: a repeated group
            /// must not be able to match empty (Python iterates such loops
            /// differently), and a lookbehind must be fixed-width.
            bool Validate(const Node& n)
            {
                if (n.kind == Node::Kind::Repeat) {
                    if (n.child->kind != Node::Kind::Atom && WidthOf(*n.child).min == 0) {
                        error_ = "a repeated group can match empty (outside the subset)";
                        return false;
                    }
                    return Validate(*n.child);
                }
                if (n.kind == Node::Kind::Group) {
                    if (n.look == Node::Look::Behind || n.look == Node::Look::NegBehind) {
                        Width w{ -1, 0 };
                        for (const auto* alt : n.alts) {
                            const Width a = WidthOfSeq(*alt);
                            if (w.min < 0) w = a;
                            else if (a.min != w.min || a.max != w.max) w.max = -2;
                        }
                        if (w.max < 0 || w.min != w.max) {
                            error_ = "look-behind requires fixed-width pattern";
                            return false;
                        }
                    }
                    for (const auto* alt : n.alts) {
                        for (const auto* c : *alt) {
                            if (!Validate(*c)) return false;
                        }
                    }
                }
                return true;
            }

            int Emit(std::size_t prog, Inst in)
            {
                impl_.programs[prog].push_back(in);
                return static_cast<int>(impl_.programs[prog].size() - 1);
            }
            int Here(std::size_t prog) const { return static_cast<int>(impl_.programs[prog].size()); }
            Inst& At(std::size_t prog, int pc) { return impl_.programs[prog][static_cast<std::size_t>(pc)]; }

            bool EmitSeq(std::size_t prog, const Seq& s)
            {
                for (const auto* n : s) {
                    if (!EmitNode(prog, *n)) return false;
                }
                return true;
            }

            bool EmitAlternatives(std::size_t prog, const Node& g)
            {
                std::vector<int> jumps;
                for (std::size_t k = 0; k < g.alts.size(); ++k) {
                    const bool last = k + 1 == g.alts.size();
                    int split = -1;
                    if (!last) split = Emit(prog, Inst{ Op::Split });
                    if (split >= 0) At(prog, split).x = Here(prog);
                    if (!EmitSeq(prog, *g.alts[k])) return false;
                    if (!last) {
                        jumps.push_back(Emit(prog, Inst{ Op::Jmp }));
                        At(prog, split).y = Here(prog);
                    }
                }
                for (const int j : jumps) At(prog, j).x = Here(prog);
                return true;
            }

            bool EmitNode(std::size_t prog, const Node& n)
            {
                switch (n.kind) {
                    case Node::Kind::Atom: {
                        Inst in{ Op::Atom };
                        in.set = n.set;
                        Emit(prog, in);
                        return true;
                    }
                    case Node::Kind::Begin: Emit(prog, Inst{ Op::Bol }); return true;
                    case Node::Kind::End: Emit(prog, Inst{ Op::Eol }); return true;
                    case Node::Kind::WordBoundary: Emit(prog, Inst{ Op::WordB }); return true;
                    case Node::Kind::NotWordBoundary: Emit(prog, Inst{ Op::NotWordB }); return true;
                    case Node::Kind::Group: {
                        if (n.look != Node::Look::None) {
                            Inst in{ Op::Look };
                            in.look = n.look;
                            if (n.look == Node::Look::Behind || n.look == Node::Look::NegBehind) {
                                Width w{ -1, 0 };
                                for (const auto* alt : n.alts) {
                                    const Width a = WidthOfSeq(*alt);
                                    if (w.min < 0) w = a;
                                    else if (a.min != w.min || a.max != w.max) w.max = -2;
                                }
                                if (w.max < 0 || w.min != w.max) {
                                    error_ = "look-behind requires fixed-width pattern";
                                    return false;
                                }
                                in.width = static_cast<int>(w.min);
                            }
                            const std::size_t sub = impl_.programs.size();
                            impl_.programs.emplace_back();
                            in.x = static_cast<int>(sub);
                            if (!EmitAlternatives(sub, n)) return false;
                            Emit(sub, Inst{ Op::Match });
                            Emit(prog, in);
                            return true;
                        }
                        if (n.capture >= 0) {
                            Inst s{ Op::Save };
                            s.x = 2 * n.capture;
                            Emit(prog, s);
                        }
                        if (!EmitAlternatives(prog, n)) return false;
                        if (n.capture >= 0) {
                            Inst s{ Op::Save };
                            s.x = 2 * n.capture + 1;
                            Emit(prog, s);
                        }
                        return true;
                    }
                    case Node::Kind::Repeat: return EmitRepeat(prog, n);
                }
                return false;
            }

            bool EmitRepeat(std::size_t prog, const Node& r)
            {
                const Node& c = *r.child;
                if (c.kind == Node::Kind::Atom) {
                    Inst in{ Op::AtomRep };
                    in.set = c.set;
                    in.min = r.min;
                    in.max = r.max;
                    in.greedy = r.greedy;
                    Emit(prog, in);
                    return true;
                }
                if (r.min > 1000 || r.max > 1000) {
                    error_ = "repeat count too large for a group";
                    return false;
                }
                for (int i = 0; i < r.min; ++i) {
                    if (!EmitNode(prog, c)) return false;
                }
                if (r.max < 0) {
                    // L1: Split L2, L3; L2: Mark; child; Progress; Jmp L1; L3:
                    const int reg = impl_.marks++;
                    const int split = Emit(prog, Inst{ Op::Split });
                    const int body = Here(prog);
                    Inst mark{ Op::Mark };
                    mark.x = reg;
                    Emit(prog, mark);
                    if (!EmitNode(prog, c)) return false;
                    Inst progress{ Op::Progress };
                    progress.x = reg;
                    Emit(prog, progress);
                    Inst jmp{ Op::Jmp };
                    jmp.x = split;
                    Emit(prog, jmp);
                    const int out = Here(prog);
                    At(prog, split).x = r.greedy ? body : out;
                    At(prog, split).y = r.greedy ? out : body;
                    return true;
                }
                // Up to (max - min) optional copies; giving one up skips the rest.
                std::vector<int> splits;
                for (int i = r.min; i < r.max; ++i) {
                    splits.push_back(Emit(prog, Inst{ Op::Split }));
                    if (!EmitNode(prog, c)) return false;
                }
                const int out = Here(prog);
                for (std::size_t k = 0; k < splits.size(); ++k) {
                    const int body = splits[k] + 1;
                    At(prog, splits[k]).x = r.greedy ? body : out;
                    At(prog, splits[k]).y = r.greedy ? out : body;
                }
                return true;
            }
        };

        // =====================================================================
        // The VM: backtracking over an explicit stack
        // =====================================================================
        class Vm
        {
        public:
            Vm(const MiniRegex::Impl& impl, std::string_view text) : impl_(impl), t_(text) {}

            /// Run program `prog` from `start`. `requiredEnd` >= 0 forces where
            /// the match ends (lookbehind). On success `end` is the end.
            bool Run(std::size_t prog, std::size_t start, std::vector<std::ptrdiff_t>& slots,
                     std::vector<std::ptrdiff_t>& marks, std::ptrdiff_t requiredEnd, std::size_t& end)
            {
                // One backtrack stack per lookaround depth, reused across runs
                // and start positions (no allocation per call once warm).
                const std::size_t depth = depth_++;
                struct DepthGuard
                {
                    std::size_t& d;
                    ~DepthGuard() { --d; }
                } guard{ depth_ };
                if (pool_.size() <= depth) pool_.emplace_back();
                auto& stack = pool_[depth];
                stack.clear();
                const Program& code = impl_.programs[prog];
                int pc = 0;
                std::size_t pos = start;
                const std::size_t n = t_.size();

                for (;;) {
                    if (++steps_ > kStepBudget) {
                        exceeded_ = true;
                        return false;
                    }
                    bool ok = true;
                    const Inst& in = code[static_cast<std::size_t>(pc)];
                    switch (in.op) {
                        case Op::Atom:
                            if (pos < n && Has(in.set, static_cast<unsigned char>(t_[pos]))) {
                                ++pos;
                                ++pc;
                            }
                            else {
                                ok = false;
                            }
                            break;
                        case Op::AtomRep: {
                            std::size_t c = 0;
                            const std::size_t cap = in.max < 0 ? n - pos : std::min<std::size_t>(n - pos, static_cast<std::size_t>(in.max));
                            while (c < cap && Has(in.set, static_cast<unsigned char>(t_[pos + c]))) ++c;
                            const auto mn = static_cast<std::size_t>(in.min);
                            if (c < mn) {
                                ok = false;
                                break;
                            }
                            // Rep frame: idx = the count to try next, old = the longest run.
                            if (in.greedy) {
                                if (c > mn) stack.push_back({ Bt::K::Rep, pc, pos, static_cast<int>(c - 1), static_cast<std::ptrdiff_t>(c) });
                                pos += c;
                            }
                            else {
                                if (c > mn) stack.push_back({ Bt::K::Rep, pc, pos, static_cast<int>(mn + 1), static_cast<std::ptrdiff_t>(c) });
                                pos += mn;
                            }
                            ++pc;
                            break;
                        }
                        case Op::Split:
                            stack.push_back({ Bt::K::Branch, in.y, pos, 0, 0 });
                            pc = in.x;
                            break;
                        case Op::Jmp: pc = in.x; break;
                        case Op::Save: {
                            auto& s = slots[static_cast<std::size_t>(in.x)];
                            stack.push_back({ Bt::K::UndoSlot, 0, 0, in.x, s });
                            s = static_cast<std::ptrdiff_t>(pos);
                            ++pc;
                            break;
                        }
                        case Op::Mark: {
                            auto& m = marks[static_cast<std::size_t>(in.x)];
                            stack.push_back({ Bt::K::UndoMark, 0, 0, in.x, m });
                            m = static_cast<std::ptrdiff_t>(pos);
                            ++pc;
                            break;
                        }
                        case Op::Progress:
                            // An iteration that matched nothing ends the loop (the
                            // Split before it already offered the exit here).
                            if (marks[static_cast<std::size_t>(in.x)] == static_cast<std::ptrdiff_t>(pos)) ok = false;
                            else ++pc;
                            break;
                        case Op::Bol:
                            if (pos == 0) ++pc;
                            else ok = false;
                            break;
                        case Op::Eol:
                            if (pos == n || (pos + 1 == n && t_[pos] == '\n')) ++pc;
                            else ok = false;
                            break;
                        case Op::WordB:
                            if (AtBoundary(pos)) ++pc;
                            else ok = false;
                            break;
                        case Op::NotWordB:
                            if (n > 0 && !AtBoundary(pos)) ++pc;
                            else ok = false;
                            break;
                        case Op::Look: {
                            const bool behind = in.look == Node::Look::Behind || in.look == Node::Look::NegBehind;
                            const bool negative = in.look == Node::Look::NegAhead || in.look == Node::Look::NegBehind;
                            if (scratch_.size() <= depth) scratch_.emplace_back();
                            auto& subSlots = scratch_[depth].first;
                            auto& subMarks = scratch_[depth].second;
                            subSlots = slots;
                            subMarks = marks;
                            bool found = false;
                            std::size_t subEnd = 0;
                            if (!behind) {
                                found = Run(static_cast<std::size_t>(in.x), pos, subSlots, subMarks, -1, subEnd);
                            }
                            else if (pos >= static_cast<std::size_t>(in.width)) {
                                found = Run(static_cast<std::size_t>(in.x), pos - static_cast<std::size_t>(in.width), subSlots,
                                            subMarks, static_cast<std::ptrdiff_t>(pos), subEnd);
                            }
                            if (exceeded_) return false;
                            if (found == negative) {
                                ok = false;
                                break;
                            }
                            if (!negative) {
                                // A positive look keeps the groups it set (as in Python),
                                // undone on backtracking like any other write.
                                for (std::size_t i = 0; i < slots.size(); ++i) {
                                    if (subSlots[i] != slots[i]) {
                                        stack.push_back({ Bt::K::UndoSlot, 0, 0, static_cast<int>(i), slots[i] });
                                        slots[i] = subSlots[i];
                                    }
                                }
                            }
                            ++pc;
                            break;
                        }
                        case Op::Match:
                            if (requiredEnd >= 0 && static_cast<std::ptrdiff_t>(pos) != requiredEnd) {
                                ok = false;
                                break;
                            }
                            end = pos;
                            return true;
                    }
                    if (ok) continue;

                    // Backtrack.
                    bool resumed = false;
                    while (!stack.empty() && !resumed) {
                        const Bt b = stack.back();
                        stack.pop_back();
                        switch (b.k) {
                            case Bt::K::UndoSlot: slots[static_cast<std::size_t>(b.idx)] = b.old; break;
                            case Bt::K::UndoMark: marks[static_cast<std::size_t>(b.idx)] = b.old; break;
                            case Bt::K::Branch:
                                pc = b.pc;
                                pos = b.pos;
                                resumed = true;
                                break;
                            case Bt::K::Rep: {
                                const Inst& r = code[static_cast<std::size_t>(b.pc)];
                                const auto count = static_cast<std::size_t>(b.idx);
                                const auto longest = static_cast<std::size_t>(b.old);
                                const auto mn = static_cast<std::size_t>(r.min);
                                if (r.greedy) {
                                    if (count > mn) stack.push_back({ Bt::K::Rep, b.pc, b.pos, static_cast<int>(count - 1), b.old });
                                }
                                else if (count < longest) {
                                    stack.push_back({ Bt::K::Rep, b.pc, b.pos, static_cast<int>(count + 1), b.old });
                                }
                                pos = b.pos + count;
                                pc = b.pc + 1;
                                resumed = true;
                                break;
                            }
                        }
                    }
                    if (!resumed) return false;
                }
            }

        private:
            struct Bt
            {
                enum class K : std::uint8_t { Branch, UndoSlot, UndoMark, Rep } k;
                int pc;
                std::size_t pos;
                int idx;
                std::ptrdiff_t old;
            };

            const MiniRegex::Impl& impl_;
            std::string_view t_;
            // deques: a nested run may add a level without moving the outer ones
            std::deque<std::vector<Bt>> pool_;
        public:
            // A search that runs this many instructions gives up: no match, and
            // MiniRegex::BudgetExceeded() counts it (the caller logs it).
            static constexpr std::size_t kStepBudget = 4'000'000;
            std::size_t steps_ = 0;
            bool exceeded_ = false;
        private:
            std::deque<std::pair<std::vector<std::ptrdiff_t>, std::vector<std::ptrdiff_t>>> scratch_;
            std::size_t depth_ = 0;

            [[nodiscard]] bool AtBoundary(std::size_t pos) const noexcept
            {
                const bool before = pos > 0 && IsWordByte(static_cast<unsigned char>(t_[pos - 1]));
                const bool after = pos < t_.size() && IsWordByte(static_cast<unsigned char>(t_[pos]));
                return before != after;
            }
        };
    }

    // =========================================================================
    // MiniRegex
    // =========================================================================
    MiniRegex::MiniRegex() = default;
    MiniRegex::MiniRegex(std::string_view pattern) { Compile(pattern); }
    MiniRegex::~MiniRegex() = default;
    MiniRegex::MiniRegex(MiniRegex&&) noexcept = default;
    MiniRegex& MiniRegex::operator=(MiniRegex&&) noexcept = default;

    namespace
    {
        std::atomic<std::size_t> g_budgetExceeded{ 0 };
    }

    std::size_t MiniRegex::BudgetExceeded() noexcept { return g_budgetExceeded.load(std::memory_order_relaxed); }

    bool MiniRegex::Valid() const noexcept { return impl_ != nullptr && error_.empty(); }
    std::size_t MiniRegex::GroupCount() const noexcept { return impl_ ? impl_->groups : 0; }

    bool MiniRegex::Compile(std::string_view pattern)
    {
        pattern_.assign(pattern);
        error_.clear();
        impl_.reset();
        Arena arena;
        std::size_t groups = 0;
        Parser parser(arena, pattern_, error_);
        const Node* root = parser.Parse(groups);
        if (!root) {
            if (error_.empty()) error_ = "invalid pattern";
            return false;
        }
        auto impl = std::make_unique<Impl>();
        impl->groups = groups;
        Compiler compiler(*impl, error_);
        if (!compiler.CompileRoot(*root)) {
            if (error_.empty()) error_ = "invalid pattern";
            error_ += " in /" + pattern_ + "/";
            return false;
        }
        impl->canBeEmpty = FirstOfNode(*root, impl->first);
        impl->anchored = !root->alts.empty();
        for (const auto* alt : root->alts) {
            if (alt->empty() || (*alt)[0]->kind != Node::Kind::Begin) impl->anchored = false;
        }
        impl_ = std::move(impl);
        return true;
    }

    bool MiniRegex::Search(std::string_view text, Match* match) const { return SearchImpl(text, match, nullptr); }

    bool MiniRegex::ContainsOrBudget(std::string_view text) const
    {
        bool budget = false;
        return SearchImpl(text, nullptr, &budget) || budget;
    }

    bool MiniRegex::SearchImpl(std::string_view text, Match* match, bool* budget) const
    {
        if (budget) *budget = false;
        if (!Valid()) return false;
        const Impl& impl = *impl_;
        Vm vm(impl, text);
        std::vector<std::ptrdiff_t> slots(impl.groups * 2, -1);
        std::vector<std::ptrdiff_t> marks(static_cast<std::size_t>(impl.marks), -1);
        const std::size_t last = impl.anchored ? 0 : text.size();
        for (std::size_t start = 0; start <= last; ++start) {
            if (!impl.canBeEmpty &&
                (start >= text.size() || !Has(impl.first, static_cast<unsigned char>(text[start])))) {
                continue;
            }
            std::fill(slots.begin(), slots.end(), -1);
            std::fill(marks.begin(), marks.end(), -1);
            std::size_t end = 0;
            const bool hit = vm.Run(0, start, slots, marks, -1, end);
            if (vm.exceeded_) {
                g_budgetExceeded.fetch_add(1, std::memory_order_relaxed);
                if (budget) *budget = true;
                return false;
            }
            if (hit) {
                if (match) {
                    match->whole.begin = static_cast<std::ptrdiff_t>(start);
                    match->whole.end = static_cast<std::ptrdiff_t>(end);
                    match->groups.assign(impl.groups, Span{});
                    for (std::size_t g = 0; g < impl.groups; ++g) {
                        match->groups[g].begin = slots[2 * g];
                        match->groups[g].end = slots[2 * g + 1];
                    }
                }
                return true;
            }
        }
        return false;
    }

    std::string_view MiniRegex::Match::Group(std::string_view text, std::size_t n) const
    {
        const Span* s = nullptr;
        if (n == 0) s = &whole;
        else if (n <= groups.size()) s = &groups[n - 1];
        if (!s || !s->Matched()) return {};
        return text.substr(static_cast<std::size_t>(s->begin), static_cast<std::size_t>(s->end - s->begin));
    }
}
