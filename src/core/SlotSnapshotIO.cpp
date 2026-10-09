#include "core/SlotSnapshotIO.h"

#include <bit>
#include <charconv>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Huginn::Core::SlotAlloc
{
    namespace
    {
        [[nodiscard]] PageSlot SlotOf(const Input& in, const SlotState& s)
        {
            PageSlot p;
            p.kind = s.kind;
            p.seatMoved = s.seatMoved;
            if (s.kind == Kind::Empty) {
                return p;
            }
            if (s.kind == Kind::Override) {
                const auto& o = in.overrides[s.src];
                p.formID = o.formID;
                p.uniqueID = o.uniqueID;
                p.key = o.dedupKey;
                p.name = o.name;
            } else {
                const auto& c = in.candidates[s.src];
                p.formID = c.formID;
                p.uniqueID = c.uniqueID;
                p.key = c.dedupKey;
                p.name = c.name;
            }
            return p;
        }

        [[nodiscard]] std::string Num(float v) { return std::format("{}", v); }
        [[nodiscard]] std::string Num(double v) { return std::format("{}", v); }
        [[nodiscard]] char Bit(bool b) { return b ? '1' : '0'; }

        /// "~" when the score is the bridge of the utility to the last bit
        /// (every score R7 records), else the score itself.
        [[nodiscard]] std::string ScoreToken(const CandidateRec& c)
        {
            const SlotScore bridged = c.isRememberedOnly ? kUnrankedScore : BridgeScore(c.utility);
            if (std::bit_cast<std::uint64_t>(bridged) == std::bit_cast<std::uint64_t>(c.score)) {
                return "~";
            }
            return Num(c.score);
        }

        void WriteMemory(std::string& out, const PageMemory& m, std::string_view memTag, std::string_view depTag)
        {
            out += memTag;
            for (const auto k : m.seats) out += std::format(" {:x}", k);
            out += " |";
            for (const auto k : m.lastPlaced) out += std::format(" {:x}", k);
            out += " |";
            for (const auto k : m.homeClaims) out += std::format(" {:x}", k);
            out += '\n';
            for (std::size_t j = 0; j < kMaxSlots; ++j) {
                const auto& row = m.departed[j];
                bool any = false;
                for (const auto& d : row) any = any || d.key != 0 || d.leftAtNs != 0;
                if (!any) continue;
                out += std::format("{} {}", depTag, j);
                for (const auto& d : row) out += std::format(" {:x} {}", d.key, d.leftAtNs);
                out += '\n';
            }
        }

        // --- reading ---------------------------------------------------------

        struct Tokens
        {
            std::vector<std::string_view> t;
            std::size_t i = 1;   // t[0] is the line's tag
            bool ok = true;

            [[nodiscard]] std::string_view Next()
            {
                if (i >= t.size()) {
                    ok = false;
                    return {};
                }
                return t[i++];
            }
            [[nodiscard]] bool Done() const { return i == t.size(); }

            template <class T>
            T Int(int base = 10)
            {
                const auto s = Next();
                T v{};
                const auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
                if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) ok = false;
                return v;
            }
            template <class T>
            T Real()
            {
                const auto s = Next();
                T v{};
                const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
                if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) ok = false;
                return v;
            }
            bool Flag()
            {
                const auto s = Next();
                if (s == "1") return true;
                if (s != "0") ok = false;
                return false;
            }
            /// key=value with the given key.
            template <class T>
            T Field(std::string_view key, int base = 10)
            {
                const auto s = Next();
                if (s.size() <= key.size() || s.substr(0, key.size()) != key || s[key.size()] != '=') {
                    ok = false;
                    return T{};
                }
                const auto v = s.substr(key.size() + 1);
                T out{};
                const auto r = std::from_chars(v.data(), v.data() + v.size(), out, base);
                if (r.ec != std::errc{} || r.ptr != v.data() + v.size()) ok = false;
                return out;
            }
            std::string Name()
            {
                std::string out;
                if (!UnescapeName(Next(), out)) ok = false;
                return out;
            }
        };

        [[nodiscard]] Tokens Split(std::string_view line)
        {
            Tokens tk;
            std::size_t p = 0;
            while (p < line.size()) {
                while (p < line.size() && line[p] == ' ') ++p;
                if (p >= line.size()) break;
                std::size_t q = p;
                while (q < line.size() && line[q] != ' ') ++q;
                tk.t.push_back(line.substr(p, q - p));
                p = q;
            }
            return tk;
        }

        bool ReadMemoryLine(Tokens& tk, PageMemory& m)
        {
            for (auto& k : m.seats) k = tk.Int<std::uint64_t>(16);
            if (tk.Next() != "|") return false;
            for (auto& k : m.lastPlaced) k = tk.Int<std::uint64_t>(16);
            if (tk.Next() != "|") return false;
            for (auto& k : m.homeClaims) k = tk.Int<std::uint64_t>(16);
            return tk.ok && tk.Done();
        }

        bool ReadDepLine(Tokens& tk, PageMemory& m)
        {
            const auto j = tk.Int<std::size_t>();
            if (!tk.ok || j >= kMaxSlots) return false;
            for (auto& d : m.departed[j]) {
                d.key = tk.Int<std::uint64_t>(16);
                d.leftAtNs = tk.Int<std::int64_t>();
            }
            return tk.ok && tk.Done();
        }
    }  // namespace

    std::vector<PageSlot> PageOf(const Input& in, const Output& out)
    {
        std::vector<PageSlot> page;
        page.reserve(out.slots.size());
        for (const auto& s : out.slots) {
            page.push_back(SlotOf(in, s));
        }
        return page;
    }

    std::vector<PageEvent> EventsOf(const Input& in, const Output& out)
    {
        auto key = [&](std::uint32_t src, bool isOverride) -> std::uint64_t {
            return isOverride ? in.overrides[src].dedupKey : in.candidates[src].dedupKey;
        };
        std::vector<PageEvent> events;
        events.reserve(out.events.size());
        for (const auto& e : out.events) {
            PageEvent p;
            switch (e.kind) {
            case EventKind::OverrideMarked:   p = { "OVM", { in.overrides[e.a].formID, e.b } }; break;
            case EventKind::OverridePlaced:   p = { "OVP", { in.overrides[e.a].formID, e.b } }; break;
            case EventKind::OverrideFallback: p = { "OVF", { in.overrides[e.a].formID, e.b } }; break;
            case EventKind::OverrideUnplaced: p = { "OVU", { in.overrides[e.a].formID, e.b } }; break;
            case EventKind::OverridesInactive: p = { "OVI", {} }; break;
            case EventKind::HoldNotCandidate: p = { "HNC", { e.a, e.b } }; break;
            case EventKind::HoldShown:        p = { "HSH", { e.a, e.b, e.c } }; break;
            case EventKind::NoCandidate:      p = { "NOC", { e.a } }; break;
            case EventKind::HoldGaveWay:
                p = { "HGW", { e.a, key(e.b, false), key(e.c, false), e.capped ? 1u : 0u } };
                break;
            case EventKind::PulledToJobKey:   p = { "PUL", { e.a, e.b, key(e.c, false) } }; break;
            case EventKind::Returner:
                p = { "RET", { e.key, e.b, e.c, static_cast<std::uint64_t>(e.why),
                                 e.d != kNone ? key(e.d, e.y != 0.0) : 0, e.d != kNone ? e.e : kNone },
                    e.f };
                break;
            }
            events.push_back(std::move(p));
        }
        return events;
    }

    void SetResult(Snapshot& snap, const Output& out)
    {
        snap.hasResult = true;
        snap.page = PageOf(snap.in, out);
        snap.memoryAfter = out.memory;
        snap.generationAfter = out.generationMatches;
        snap.clearedAllPages = out.clearedAllPages;
        snap.keptOff = KeptOff(snap.in, out);
        snap.hasEvents = true;
        snap.events = EventsOf(snap.in, out);
    }

    std::string EscapeName(std::string_view name)
    {
        std::string out = "=";
        for (const char ch : name) {
            const auto u = static_cast<unsigned char>(ch);
            if (u <= 0x20 || u >= 0x7F || ch == '%' || ch == '=') {
                out += std::format("%{:02X}", static_cast<unsigned>(u));
            } else {
                out += ch;
            }
        }
        return out;
    }

    bool UnescapeName(std::string_view token, std::string& out)
    {
        out.clear();
        if (token.empty() || token[0] != '=') return false;
        for (std::size_t i = 1; i < token.size(); ++i) {
            if (token[i] != '%') {
                out += token[i];
                continue;
            }
            if (i + 2 >= token.size()) return false;   // needs two hex digits after the '%'
            unsigned v = 0;
            const auto r = std::from_chars(token.data() + i + 1, token.data() + i + 3, v, 16);
            if (r.ec != std::errc{} || r.ptr != token.data() + i + 3) return false;
            out += static_cast<char>(v);
            i += 2;
        }
        return true;
    }

    std::string WriteSnapshot(const Snapshot& snap, SnapshotDictionary* dict)
    {
        const auto& in = snap.in;
        const auto& s = in.settings;
        std::string defs;   // ITEM lines this snapshot adds to the dictionary
        std::string out;
        out.reserve(256 + in.candidates.size() * 96);
        out += std::format("SNAP {} page={} now={} mem={} gen={} ovr={}\n", EscapeName(snap.tag), in.pageIndex,
            in.nowNs, Bit(in.memoryAvailable), Bit(in.generationMatches), Bit(in.overridesActive));
        out += std::format("SET {} {} {} {} {} {} {} {} {}\n", Bit(s.keepSlotPositions), Bit(s.holdSeatedItems),
            Num(s.challengerMargin), Bit(s.fillJobKeysFromRegular), Num(s.classDiscount), s.classFree,
            Num(s.homeKeyMemorySec), Bit(s.returnToHomeKey), Bit(s.remembranceToJob));
        for (const auto& sl : in.slots) {
            out += std::format("SLOT {} {} {} {} {} {} {}\n", sl.classIndex, Bit(sl.regular), Bit(sl.wildcardsEnabled),
                Bit(sl.skipEquipped), Bit(sl.remembrance), static_cast<int>(sl.priority), sl.overrideAccept);
        }
        for (const auto& c : in.candidates) {
            const unsigned flags = (c.isWildcard ? 1u : 0u) | (c.isRememberedOnly ? 2u : 0u) |
                                   (c.isEquipped ? 4u : 0u) | (c.playerEquipped ? 8u : 0u);
            if (dict) {
                const std::string item = std::format("{:x} {} {:x} {} {:x} {} {}", c.formID, c.uniqueID, c.dedupKey,
                    Num(c.tieBreak), c.matchMask, c.capClass, EscapeName(c.name));
                auto [it, added] = dict->items.try_emplace(item, dict->next);
                if (added) {
                    defs += std::format("ITEM {} {}\n", dict->next, item);
                    ++dict->next;
                }
                out += std::format("C {} {} {} {}\n", it->second, Num(c.utility), ScoreToken(c), flags);
            } else {
                out += std::format("CAND {:x} {} {:x} {} {} {} {} {:x} {} {}\n", c.formID, c.uniqueID, c.dedupKey,
                    Num(c.utility), ScoreToken(c), Num(c.tieBreak), flags, c.matchMask, c.capClass, EscapeName(c.name));
            }
        }
        for (const auto& o : in.overrides) {
            out += std::format("OVR {:x} {} {:x} {} {} {} {} {} {:x} {} {}\n", o.formID, o.uniqueID, o.dedupKey,
                o.condition, o.category, Bit(o.pinnedToSlot), Bit(o.isEquipped), Bit(o.playerEquipped), o.matchMask,
                o.capClass, EscapeName(o.name));
        }
        for (std::size_t j = 0; j < kMaxSlots; ++j) {
            if (in.holds[j].active) {
                out += std::format("HOLD {} {:x}\n", j, in.holds[j].formID);
            }
        }
        WriteMemory(out, in.memory, "MEM", "DEP");
        if (snap.hasResult) {
            out += std::format("OUT gen={} cleared={}\n", Bit(snap.generationAfter), Bit(snap.clearedAllPages));
            for (const auto& p : snap.page) {
                out += std::format("A {} {:x} {} {:x} {} {}\n", static_cast<int>(p.kind), p.formID, p.uniqueID, p.key,
                    Bit(p.seatMoved), EscapeName(p.name));
            }
            WriteMemory(out, snap.memoryAfter, "OMEM", "ODEP");
            out += "KEPT";
            for (const auto id : snap.keptOff) out += std::format(" {:x}", id);
            out += '\n';
            if (snap.hasEvents) {
                out += "EVENTS\n";
                for (const auto& e : snap.events) {
                    out += std::format("EV {} {}", e.code, Num(e.f));
                    for (const auto v : e.v) out += std::format(" {:x}", v);
                    out += '\n';
                }
            }
        }
        out += "END\n";
        return defs + out;
    }

    bool ReadSnapshots(std::string_view text, std::vector<Snapshot>& out, std::string* error)
    {
        std::size_t lineNo = 0;
        auto fail = [&](std::string_view why) {
            if (error) *error = std::format("line {}: {}", lineNo, why);
            return false;
        };

        Snapshot cur;
        bool inSnap = false;
        std::vector<CandidateRec> items;
        std::size_t pos = 0;
        while (pos < text.size()) {
            std::size_t end = text.find('\n', pos);
            if (end == std::string_view::npos) end = text.size();
            std::string_view line = text.substr(pos, end - pos);
            pos = end + 1;
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || line[0] == '#') continue;

            Tokens tk = Split(line);
            const auto tag = tk.t[0];
            if (tag == "ITEM") {
                // The file's item dictionary: a candidate's fixed fields, once.
                if (inSnap) return fail("ITEM inside a snapshot");
                const auto id = tk.Int<std::size_t>();
                if (!tk.ok || id != items.size()) return fail("ITEM ids must run 0, 1, 2, ...");
                CandidateRec c;
                c.formID = tk.Int<std::uint32_t>(16);
                c.uniqueID = tk.Int<std::uint16_t>();
                c.dedupKey = tk.Int<std::uint64_t>(16);
                c.tieBreak = tk.Real<float>();
                c.matchMask = tk.Int<std::uint32_t>(16);
                c.capClass = tk.Int<std::uint8_t>();
                c.name = tk.Name();
                if (!tk.ok || !tk.Done()) return fail("bad ITEM");
                items.push_back(std::move(c));
                continue;
            }
            if (tag == "SNAP") {
                if (inSnap) return fail("SNAP inside a snapshot");
                cur = Snapshot{};
                inSnap = true;
                if (!UnescapeName(tk.Next(), cur.tag)) return fail("bad tag");
                cur.in.pageIndex = tk.Field<std::uint32_t>("page");
                cur.in.nowNs = tk.Field<std::int64_t>("now");
                cur.in.memoryAvailable = tk.Field<int>("mem") != 0;
                cur.in.generationMatches = tk.Field<int>("gen") != 0;
                cur.in.overridesActive = tk.Field<int>("ovr") != 0;
                if (!tk.ok || !tk.Done()) return fail("bad SNAP");
                continue;
            }
            if (!inSnap) return fail("line outside a snapshot");

            auto& in = cur.in;
            if (tag == "SET") {
                auto& s = in.settings;
                s.keepSlotPositions = tk.Flag();
                s.holdSeatedItems = tk.Flag();
                s.challengerMargin = tk.Real<float>();
                s.fillJobKeysFromRegular = tk.Flag();
                s.classDiscount = tk.Real<float>();
                s.classFree = tk.Int<std::uint32_t>();
                s.homeKeyMemorySec = tk.Real<float>();
                s.returnToHomeKey = tk.Flag();
                s.remembranceToJob = tk.Flag();
            } else if (tag == "SLOT") {
                SlotRec sl;
                sl.classIndex = tk.Int<std::uint8_t>();
                sl.regular = tk.Flag();
                sl.wildcardsEnabled = tk.Flag();
                sl.skipEquipped = tk.Flag();
                sl.remembrance = tk.Flag();
                sl.priority = static_cast<std::int8_t>(tk.Int<int>());
                sl.overrideAccept = tk.Int<std::uint8_t>();
                in.slots.push_back(sl);
            } else if (tag == "CAND" || tag == "C") {
                CandidateRec c;
                std::string_view scoreTok;
                unsigned flags = 0;
                if (tag == "C") {
                    const auto ref = tk.Int<std::size_t>();
                    if (!tk.ok || ref >= items.size()) return fail("C refers to an unknown ITEM");
                    c = items[ref];
                    c.utility = tk.Real<float>();
                    scoreTok = tk.Next();
                    flags = tk.Int<unsigned>();
                } else {
                    c.formID = tk.Int<std::uint32_t>(16);
                    c.uniqueID = tk.Int<std::uint16_t>();
                    c.dedupKey = tk.Int<std::uint64_t>(16);
                    c.utility = tk.Real<float>();
                    scoreTok = tk.Next();
                    c.tieBreak = tk.Real<float>();
                    flags = tk.Int<unsigned>();
                    c.matchMask = tk.Int<std::uint32_t>(16);
                    c.capClass = tk.Int<std::uint8_t>();
                    c.name = tk.Name();
                }
                c.isWildcard = (flags & 1u) != 0;
                c.isRememberedOnly = (flags & 2u) != 0;
                c.isEquipped = (flags & 4u) != 0;
                c.playerEquipped = (flags & 8u) != 0;
                if (scoreTok == "~") {
                    c.score = c.isRememberedOnly ? kUnrankedScore : BridgeScore(c.utility);
                } else {
                    const auto r = std::from_chars(scoreTok.data(), scoreTok.data() + scoreTok.size(), c.score);
                    if (r.ec != std::errc{} || r.ptr != scoreTok.data() + scoreTok.size()) return fail("bad score");
                }
                in.candidates.push_back(std::move(c));
            } else if (tag == "OVR") {
                OverrideRec o;
                o.formID = tk.Int<std::uint32_t>(16);
                o.uniqueID = tk.Int<std::uint16_t>();
                o.dedupKey = tk.Int<std::uint64_t>(16);
                o.condition = tk.Int<std::uint8_t>();
                o.category = tk.Int<std::uint8_t>();
                o.pinnedToSlot = tk.Flag();
                o.isEquipped = tk.Flag();
                o.playerEquipped = tk.Flag();
                o.matchMask = tk.Int<std::uint32_t>(16);
                o.capClass = tk.Int<std::uint8_t>();
                o.name = tk.Name();
                in.overrides.push_back(std::move(o));
            } else if (tag == "HOLD") {
                const auto j = tk.Int<std::size_t>();
                const auto id = tk.Int<std::uint32_t>(16);
                if (!tk.ok || j >= kMaxSlots) return fail("bad HOLD");
                in.holds[j] = { true, id };
            } else if (tag == "MEM") {
                if (!ReadMemoryLine(tk, in.memory)) return fail("bad MEM");
            } else if (tag == "DEP") {
                if (!ReadDepLine(tk, in.memory)) return fail("bad DEP");
            } else if (tag == "OUT") {
                cur.hasResult = true;
                cur.generationAfter = tk.Field<int>("gen") != 0;
                cur.clearedAllPages = tk.Field<int>("cleared") != 0;
            } else if (tag == "A") {
                PageSlot p;
                const int kind = tk.Int<int>();
                if (kind < 0 || kind > static_cast<int>(Kind::Remembered)) return fail("bad kind");
                p.kind = static_cast<Kind>(kind);
                p.formID = tk.Int<std::uint32_t>(16);
                p.uniqueID = tk.Int<std::uint16_t>();
                p.key = tk.Int<std::uint64_t>(16);
                p.seatMoved = tk.Flag();
                p.name = tk.Name();
                cur.page.push_back(std::move(p));
            } else if (tag == "OMEM") {
                if (!ReadMemoryLine(tk, cur.memoryAfter)) return fail("bad OMEM");
            } else if (tag == "ODEP") {
                if (!ReadDepLine(tk, cur.memoryAfter)) return fail("bad ODEP");
            } else if (tag == "KEPT") {
                while (!tk.Done() && tk.ok) cur.keptOff.push_back(tk.Int<std::uint32_t>(16));
            } else if (tag == "EVENTS") {
                cur.hasEvents = true;
            } else if (tag == "EV") {
                PageEvent e;
                e.code = std::string(tk.Next());
                e.f = tk.Real<float>();
                while (!tk.Done() && tk.ok) e.v.push_back(tk.Int<std::uint64_t>(16));
                if (e.code.size() != 3) return fail("bad EV code");
                cur.events.push_back(std::move(e));
            } else if (tag == "END") {
                out.push_back(std::move(cur));
                cur = Snapshot{};
                inSnap = false;
                continue;
            } else {
                return fail(std::format("unknown line '{}'", tag));
            }
            if (!tk.ok || !tk.Done()) return fail(std::format("bad {} line", tag));
        }
        if (inSnap) return fail("snapshot without END");
        return true;
    }
}  // namespace Huginn::Core::SlotAlloc
