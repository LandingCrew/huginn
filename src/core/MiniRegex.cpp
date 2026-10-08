#include "MiniRegex.h"

namespace Huginn::Core
{
    // =========================================================================
    // The parsed pattern
    // =========================================================================
    struct MiniRegex::Node
    {
        enum class Kind : std::uint8_t
        {
            Char, Any, Class,                    // single-byte atoms
            WordBoundary, NotWordBoundary, Begin, End,  // zero-width assertions
            Group, Repeat
        };
        enum class Look : std::uint8_t { None, Ahead, NegAhead, Behind, NegBehind };

        Kind kind = Kind::Char;
        unsigned char ch = 0;
        std::array<std::uint64_t, 4> set{};  // Class: bit per byte value

        // Group
        std::vector<Seq*> alts;
        int capture = -1;  // 0-based capture index, -1 if not capturing
        Look look = Look::None;

        // Repeat
        Node* child = nullptr;
        Seq* single = nullptr;  // {child}, for matching a non-atom child
        int min = 0;
        int max = -1;  // -1 = unbounded
        bool greedy = true;

        [[nodiscard]] bool IsAtom() const noexcept
        {
            return kind == Kind::Char || kind == Kind::Any || kind == Kind::Class;
        }
    };

    namespace
    {
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

        /// A literal escape (\. \( \- ... \n \t). False for a letter or digit
        /// with no meaning here, so a typo is an error rather than a literal.
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
    }

    // =========================================================================
    // Parser
    // =========================================================================
    class MiniRegexParser
    {
    public:
        MiniRegexParser(MiniRegex& re, std::string_view p) : re_(re), p_(p) {}

        bool Parse()
        {
            auto* root = NewNode(MiniRegex::Node::Kind::Group);
            if (!ParseAlternatives(*root)) return false;
            if (i_ != p_.size()) return Fail("unbalanced ')'");
            re_.root_ = root;
            return true;
        }

    private:
        using Node = MiniRegex::Node;
        using Kind = Node::Kind;

        MiniRegex& re_;
        std::string_view p_;
        std::size_t i_ = 0;

        bool Fail(const char* why)
        {
            if (re_.error_.empty()) {
                re_.error_ = std::string(why) + " at " + std::to_string(i_) + " in /" + std::string(p_) + "/";
            }
            return false;
        }

        Node* NewNode(Kind k)
        {
            re_.nodes_.push_back(std::make_unique<Node>());
            auto* n = re_.nodes_.back().get();
            n->kind = k;
            return n;
        }
        MiniRegex::Seq* NewSeq()
        {
            re_.seqs_.push_back(std::make_unique<MiniRegex::Seq>());
            return re_.seqs_.back().get();
        }

        [[nodiscard]] bool AtEnd() const noexcept { return i_ >= p_.size(); }
        [[nodiscard]] char Peek(std::size_t ahead = 0) const noexcept
        {
            return i_ + ahead < p_.size() ? p_[i_ + ahead] : '\0';
        }

        // alternatives := seq ('|' seq)*  -- into group.alts
        bool ParseAlternatives(Node& group)
        {
            for (;;) {
                auto* seq = NewSeq();
                if (!ParseSequence(*seq)) return false;
                group.alts.push_back(seq);
                if (!AtEnd() && Peek() == '|') {
                    ++i_;
                    continue;
                }
                return true;
            }
        }

        bool ParseSequence(MiniRegex::Seq& seq)
        {
            while (!AtEnd() && Peek() != '|' && Peek() != ')') {
                Node* atom = nullptr;
                if (!ParseAtom(atom)) return false;
                if (!ParseQuantifier(atom)) return false;
                seq.push_back(atom);
            }
            return true;
        }

        bool ParseAtom(Node*& out)
        {
            const char c = Peek();
            if (c == '(') return ParseGroup(out);
            if (c == '[') return ParseClass(out);
            if (c == '.') {
                ++i_;
                out = NewNode(Kind::Any);
                return true;
            }
            if (c == '^') {
                ++i_;
                out = NewNode(Kind::Begin);
                return true;
            }
            if (c == '$') {
                ++i_;
                out = NewNode(Kind::End);
                return true;
            }
            if (c == '*' || c == '+' || c == '?') return Fail("nothing to repeat");
            if (c == '\\') {
                ++i_;
                if (AtEnd()) return Fail("trailing backslash");
                const char e = p_[i_++];
                if (e == 'b') { out = NewNode(Kind::WordBoundary); return true; }
                if (e == 'B') { out = NewNode(Kind::NotWordBoundary); return true; }
                Set s{};
                if (ClassEscape(e, s)) {
                    out = NewNode(Kind::Class);
                    out->set = s;
                    return true;
                }
                unsigned char lit = 0;
                if (!LiteralEscape(e, lit)) return Fail("unsupported escape");
                out = NewNode(Kind::Char);
                out->ch = lit;
                return true;
            }
            ++i_;
            out = NewNode(Kind::Char);
            out->ch = static_cast<unsigned char>(c);
            return true;
        }

        bool ParseGroup(Node*& out)
        {
            ++i_;  // '('
            auto* g = NewNode(Kind::Group);
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
                g->capture = static_cast<int>(re_.groupCount_++);
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
                char c = p_[i_++];
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
                if (Peek() == '-' && Peek(1) != ']' && Peek(1) != '\0') {
                    ++i_;
                    unsigned char hi = 0;
                    char h = p_[i_++];
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
            out = NewNode(Kind::Class);
            out->set = s;
            return true;
        }

        bool ParseNumber(int& out)
        {
            if (AtEnd() || Peek() < '0' || Peek() > '9') return false;
            int v = 0;
            while (!AtEnd() && Peek() >= '0' && Peek() <= '9') {
                v = v * 10 + (Peek() - '0');
                if (v > 100000) return false;
                ++i_;
            }
            out = v;
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
                // {n}, {n,}, {n,m}; anything else is a literal '{' (as in Python)
                const std::size_t save = i_;
                ++i_;
                int a = 0;
                if (!ParseNumber(a)) { i_ = save; return true; }
                int b = a;
                if (Peek() == ',') {
                    ++i_;
                    if (Peek() == '}') b = -1;
                    else if (!ParseNumber(b)) { i_ = save; return true; }
                }
                if (Peek() != '}') { i_ = save; return true; }
                ++i_;
                if (b >= 0 && b < a) return Fail("bad repeat bounds");
                mn = a;
                mx = b;
            }
            else {
                return true;
            }
            const auto k = atom->kind;
            if (k == Kind::Begin || k == Kind::End || k == Kind::WordBoundary || k == Kind::NotWordBoundary ||
                (k == Kind::Group && atom->look != Node::Look::None)) {
                return Fail("cannot repeat an assertion");
            }
            auto* r = NewNode(Kind::Repeat);
            r->child = atom;
            r->min = mn;
            r->max = mx;
            if (Peek() == '?') {
                ++i_;
                r->greedy = false;
            }
            // A second quantifier ("a**", possessive "a*+") is outside the subset.
            if (Peek() == '*' || Peek() == '+' || Peek() == '?') return Fail("multiple repeat");
            r->single = NewSeq();
            r->single->push_back(atom);
            atom = r;
            return true;
        }
    };

    // =========================================================================
    // Search prefilter: which bytes can start a match
    // =========================================================================
    namespace
    {
        using RNode = MiniRegex::Node;
        bool FirstOfNode(const RNode& n, Set& first);

        /// Adds to `first` the bytes a non-empty match of s[from..] can start
        /// with; true if s[from..] can match the empty string. Zero-width
        /// assertions are transparent (they never add a byte).
        bool FirstOfSeq(const MiniRegex::Seq& s, Set& first)
        {
            for (const auto* n : s) {
                if (!FirstOfNode(*n, first)) return false;
            }
            return true;
        }

        bool FirstOfNode(const RNode& n, Set& first)
        {
            switch (n.kind) {
                case RNode::Kind::Char: Add(first, n.ch); return false;
                case RNode::Kind::Any: {
                    Set all{};
                    Invert(all);
                    Merge(first, all);
                    return false;
                }
                case RNode::Kind::Class: Merge(first, n.set); return false;
                case RNode::Kind::WordBoundary:
                case RNode::Kind::NotWordBoundary:
                case RNode::Kind::Begin:
                case RNode::Kind::End: return true;
                case RNode::Kind::Group: {
                    if (n.look != RNode::Look::None) return true;
                    bool empty = false;
                    for (const auto* alt : n.alts) {
                        if (FirstOfSeq(*alt, first)) empty = true;
                    }
                    return empty;
                }
                case RNode::Kind::Repeat: return FirstOfNode(*n.child, first) || n.min == 0;
            }
            return true;
        }
    }

    // =========================================================================
    // Matcher: continuation-passing backtracking
    // =========================================================================
    class MiniRegexMatcher
    {
    public:
        using Node = MiniRegex::Node;
        using Kind = Node::Kind;
        using Seq = MiniRegex::Seq;

        MiniRegexMatcher(const MiniRegex& re, std::string_view text) : re_(re), t_(text)
        {
            caps_.assign(re.groupCount_, MiniRegex::Span{});
        }

        bool SearchAll(MiniRegex::Match* match)
        {
            const std::size_t last = re_.anchored_ ? 0 : t_.size();
            for (std::size_t start = 0; start <= last; ++start) {
                if (!re_.canBeEmpty_ &&
                    (start >= t_.size() || !Has(re_.first_, static_cast<unsigned char>(t_[start])))) {
                    continue;
                }
                for (auto& c : caps_) c = MiniRegex::Span{};
                Cont accept{};
                accept.kind = ContKind::Accept;
                if (MatchGroupAlternatives(*re_.root_, start, &accept)) {
                    if (match) {
                        match->whole.begin = static_cast<std::ptrdiff_t>(start);
                        match->whole.end = static_cast<std::ptrdiff_t>(end_);
                        match->groups = caps_;
                    }
                    return true;
                }
            }
            return false;
        }

    private:
        enum class ContKind : std::uint8_t { Seq, Close, Rep, Accept, AcceptAt, AcceptAny };

        struct Cont
        {
            ContKind kind = ContKind::Accept;
            const Seq* seq = nullptr;  // Seq
            std::size_t idx = 0;
            int group = -1;            // Close
            std::size_t groupBegin = 0;
            const Node* rep = nullptr; // Rep
            int count = 0;
            std::size_t pos = 0;       // Rep: where this iteration began; AcceptAt: the required end
            const Cont* next = nullptr;
        };

        const MiniRegex& re_;
        std::string_view t_;
        std::vector<MiniRegex::Span> caps_;
        std::size_t end_ = 0;

        [[nodiscard]] bool Atom(const Node& n, std::size_t pos) const noexcept
        {
            if (pos >= t_.size()) return false;
            const auto c = static_cast<unsigned char>(t_[pos]);
            switch (n.kind) {
                case Kind::Char: return c == n.ch;
                case Kind::Any: return c != '\n';
                case Kind::Class: return Has(n.set, c);
                default: return false;
            }
        }

        [[nodiscard]] bool AtBoundary(std::size_t pos) const noexcept
        {
            const bool before = pos > 0 && IsWordByte(static_cast<unsigned char>(t_[pos - 1]));
            const bool after = pos < t_.size() && IsWordByte(static_cast<unsigned char>(t_[pos]));
            return before != after;
        }

        bool Run(const Cont* k, std::size_t pos)
        {
            switch (k->kind) {
                case ContKind::Accept:
                    end_ = pos;
                    return true;
                case ContKind::AcceptAny:
                    return true;
                case ContKind::AcceptAt:
                    return pos == k->pos;
                case ContKind::Seq:
                    return MatchSeq(*k->seq, k->idx, pos, k->next);
                case ContKind::Close: {
                    auto& cap = caps_[static_cast<std::size_t>(k->group)];
                    const auto saved = cap;
                    cap.begin = static_cast<std::ptrdiff_t>(k->groupBegin);
                    cap.end = static_cast<std::ptrdiff_t>(pos);
                    if (Run(k->next, pos)) return true;
                    cap = saved;
                    return false;
                }
                case ContKind::Rep: {
                    // An iteration that matched nothing ends the loop (as in Python).
                    if (pos == k->pos) return Run(k->next, pos);
                    return RepeatGeneral(*k->rep, k->count, pos, k->next);
                }
            }
            return false;
        }

        bool MatchGroupAlternatives(const Node& g, std::size_t pos, const Cont* k)
        {
            for (const auto* alt : g.alts) {
                if (MatchSeq(*alt, 0, pos, k)) return true;
            }
            return false;
        }

        bool Look(const Node& g, std::size_t pos)
        {
            const auto savedCaps = caps_;
            bool found = false;
            if (g.look == Node::Look::Ahead || g.look == Node::Look::NegAhead) {
                Cont any{};
                any.kind = ContKind::AcceptAny;
                found = MatchGroupAlternatives(g, pos, &any);
            }
            else {
                Cont at{};
                at.kind = ContKind::AcceptAt;
                at.pos = pos;
                for (std::size_t j = pos + 1; j-- > 0 && !found;) {
                    found = MatchGroupAlternatives(g, j, &at);
                }
            }
            const bool negative = g.look == Node::Look::NegAhead || g.look == Node::Look::NegBehind;
            if (!found || negative) caps_ = savedCaps;  // a failed or negative look binds nothing
            return found != negative;
        }

        bool MatchSeq(const Seq& s, std::size_t idx, std::size_t pos, const Cont* k)
        {
            while (idx < s.size()) {
                const Node& n = *s[idx];
                switch (n.kind) {
                    case Kind::Char:
                    case Kind::Any:
                    case Kind::Class:
                        if (!Atom(n, pos)) return false;
                        ++pos;
                        ++idx;
                        continue;
                    case Kind::WordBoundary:
                        if (!AtBoundary(pos)) return false;
                        ++idx;
                        continue;
                    case Kind::NotWordBoundary:
                        if (AtBoundary(pos)) return false;
                        ++idx;
                        continue;
                    case Kind::Begin:
                        if (pos != 0) return false;
                        ++idx;
                        continue;
                    case Kind::End:
                        if (pos != t_.size()) return false;
                        ++idx;
                        continue;
                    case Kind::Group: {
                        if (n.look != Node::Look::None) {
                            if (!Look(n, pos)) return false;
                            ++idx;
                            continue;
                        }
                        Cont after{};
                        after.kind = ContKind::Seq;
                        after.seq = &s;
                        after.idx = idx + 1;
                        after.next = k;
                        if (n.capture < 0) return MatchGroupAlternatives(n, pos, &after);
                        Cont close{};
                        close.kind = ContKind::Close;
                        close.group = n.capture;
                        close.groupBegin = pos;
                        close.next = &after;
                        return MatchGroupAlternatives(n, pos, &close);
                    }
                    case Kind::Repeat: {
                        Cont after{};
                        after.kind = ContKind::Seq;
                        after.seq = &s;
                        after.idx = idx + 1;
                        after.next = k;
                        return Repeat(n, pos, &after);
                    }
                }
                return false;
            }
            return Run(k, pos);
        }

        bool Repeat(const Node& r, std::size_t pos, const Cont* k)
        {
            if (r.child->IsAtom()) {
                // Count the run first, then try the continuation at each length:
                // one stack frame per try, not one per character.
                std::size_t c = 0;
                while ((r.max < 0 || c < static_cast<std::size_t>(r.max)) && Atom(*r.child, pos + c)) ++c;
                if (c < static_cast<std::size_t>(r.min)) return false;
                if (r.greedy) {
                    for (std::size_t cc = c + 1; cc-- > static_cast<std::size_t>(r.min);) {
                        if (Run(k, pos + cc)) return true;
                    }
                }
                else {
                    for (std::size_t cc = static_cast<std::size_t>(r.min); cc <= c; ++cc) {
                        if (Run(k, pos + cc)) return true;
                    }
                }
                return false;
            }
            return RepeatGeneral(r, 0, pos, k);
        }

        bool RepeatGeneral(const Node& r, int count, std::size_t pos, const Cont* k)
        {
            const bool canMore = r.max < 0 || count < r.max;
            const bool enough = count >= r.min;
            Cont again{};
            again.kind = ContKind::Rep;
            again.rep = &r;
            again.count = count + 1;
            again.pos = pos;
            again.next = k;
            if (r.greedy) {
                if (canMore && MatchSeq(*r.single, 0, pos, &again)) return true;
                return enough && Run(k, pos);
            }
            if (enough && Run(k, pos)) return true;
            return canMore && MatchSeq(*r.single, 0, pos, &again);
        }
    };

    // =========================================================================
    // MiniRegex
    // =========================================================================
    MiniRegex::MiniRegex() = default;
    MiniRegex::MiniRegex(std::string_view pattern) { Compile(pattern); }
    MiniRegex::~MiniRegex() = default;
    MiniRegex::MiniRegex(MiniRegex&&) noexcept = default;
    MiniRegex& MiniRegex::operator=(MiniRegex&&) noexcept = default;

    bool MiniRegex::Compile(std::string_view pattern)
    {
        pattern_.assign(pattern);
        error_.clear();
        nodes_.clear();
        seqs_.clear();
        root_ = nullptr;
        groupCount_ = 0;
        MiniRegexParser parser(*this, pattern_);
        if (!parser.Parse()) {
            root_ = nullptr;
            if (error_.empty()) error_ = "invalid pattern";
            return false;
        }
        first_ = {};
        canBeEmpty_ = FirstOfNode(*root_, first_);
        anchored_ = !root_->alts.empty();
        for (const auto* alt : root_->alts) {
            if (alt->empty() || (*alt)[0]->kind != Node::Kind::Begin) anchored_ = false;
        }
        return true;
    }

    bool MiniRegex::Search(std::string_view text, Match* match) const
    {
        if (!Valid()) return false;
        MiniRegexMatcher m(*this, text);
        return m.SearchAll(match);
    }

    std::string_view MiniRegex::Match::Group(std::string_view text, std::size_t n) const
    {
        if (n == 0) {
            return whole.Matched() ? text.substr(static_cast<std::size_t>(whole.begin),
                                                 static_cast<std::size_t>(whole.end - whole.begin))
                                   : std::string_view{};
        }
        if (n > groups.size()) return {};
        const auto& g = groups[n - 1];
        if (!g.Matched()) return {};
        return text.substr(static_cast<std::size_t>(g.begin), static_cast<std::size_t>(g.end - g.begin));
    }
}
