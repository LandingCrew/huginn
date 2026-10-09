#include "DecisionLog.h"

#include "NeedIds.h"

#include <charconv>
#include <cmath>

namespace Huginn::Core::DecisionLog
{
    namespace
    {
        constexpr std::array<std::string_view, kCrossCount> kCrossNames{
            "overshoot_health", "overshoot_magicka", "overshoot_stamina", "weapon_charge",
            "stack_count",      "ammo_matches_launcher", "school_fortified",
        };

        bool IsValidUtf8(std::string_view text)
        {
            for (std::size_t i = 0; i < text.size();) {
                const auto c = static_cast<unsigned char>(text[i]);
                const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
                if (n == 0 || i + n > text.size()) return false;
                for (std::size_t k = 1; k < n; ++k) {
                    if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
                }
                i += n;
            }
            return true;
        }

        void AppendInt(std::string& out, long long v)
        {
            char buf[24];
            const auto r = std::to_chars(buf, buf + sizeof(buf), v);
            out.append(buf, r.ptr);
        }

        void AppendUInt(std::string& out, unsigned long long v)
        {
            char buf[24];
            const auto r = std::to_chars(buf, buf + sizeof(buf), v);
            out.append(buf, r.ptr);
        }

        /// Fixed-point with `decimals` places (durations, milliseconds); NaN
        /// and infinity are `null`.
        void AppendFixed(std::string& out, double v, int decimals)
        {
            if (!std::isfinite(v)) {
                out += "null";
                return;
            }
            char buf[48];
            const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, decimals);
            out.append(buf, r.ptr);
        }

        void AppendSeconds(std::string& out, double v) { AppendFixed(out, v, 3); }
        void AppendMs(std::string& out, double v) { AppendFixed(out, v, 0); }

        /// [[index,value],...] over the non-zero entries.
        template <std::size_t N>
        void AppendSparse(std::string& out, const std::array<float, N>& values)
        {
            out += '[';
            bool first = true;
            for (std::size_t i = 0; i < N; ++i) {
                const float v = values[i];
                if (v == 0.0f) continue;   // NaN != 0: a NaN is written (as null), never hidden
                if (!first) out += ',';
                first = false;
                out += '[';
                AppendInt(out, static_cast<long long>(i));
                out += ',';
                AppendNum(out, v);
                out += ']';
            }
            out += ']';
        }

        template <class Names>
        void AppendNameList(std::string& out, const Names& names)
        {
            out += '[';
            bool first = true;
            for (const auto& n : names) {
                if (!first) out += ',';
                first = false;
                out += JsonString(n);
            }
            out += ']';
        }

        std::string CapText(const Effect::Cap& cap)
        {
            std::string out;
            out += '[';
            bool first = true;
            for (const auto& v : cap) {
                if (!first) out += ',';
                first = false;
                out += '[';
                AppendInt(out, static_cast<long long>(v.col));
                out += ',';
                AppendNum(out, v.v);
                out += ']';
            }
            out += ']';
            return out;
        }
    }

    std::string_view OutcomeName(Outcome o) noexcept
    {
        switch (o) {
            case Outcome::Key: return "key";
            case Outcome::Wheel: return "wheel";
            case Outcome::Menu: return "menu";
            case Outcome::Nothing: return "nothing";
        }
        return "?";
    }

    std::string JsonString(std::string_view text)
    {
        static constexpr char kHex[] = "0123456789abcdef";
        const bool utf8 = IsValidUtf8(text);
        std::string out;
        out.reserve(text.size() + 2);
        out += '"';
        for (const char c : text) {
            const auto u = static_cast<unsigned char>(c);
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (u < 0x20 || (u >= 0x80 && !utf8)) {
                        out += "\\u00";
                        out += kHex[u >> 4];
                        out += kHex[u & 0xF];
                    }
                    else {
                        out += c;
                    }
            }
        }
        out += '"';
        return out;
    }

    void AppendNum(std::string& out, float v)
    {
        if (!std::isfinite(v)) {
            out += "null";
            return;
        }
        if (v == 0.0f) {   // also -0
            out += '0';
            return;
        }
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, 4);
        out.append(buf, r.ptr);
    }

    std::string FormHex(std::uint32_t form)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string s(8, '0');
        for (int i = 7; i >= 0; --i) {
            s[static_cast<std::size_t>(i)] = kHex[form & 0xF];
            form >>= 4;
        }
        return s;
    }

    std::string Encoder::BeginSegment(const Head& head)
    {
        capByText_.clear();
        capByStatic_.clear();
        ctxDefined_.clear();

        std::string out;
        out.reserve(8192);
        out += R"({"t":"head","v":)";
        AppendInt(out, kVersion);
        out += R"(,"launch":)";
        out += JsonString(head.launch);
        out += R"(,"list":)";
        out += JsonString(head.list);
        out += R"(,"build":)";
        out += JsonString(head.build);

        out += R"(,"cols":[)";
        for (std::size_t i = 0; i < Effect::kColumnCount; ++i) {
            if (i) out += ',';
            out += JsonString(Effect::kColumns[i].id);
        }
        out += R"(],"needs":[)";
        for (std::size_t i = 0; i < Needs::kNeedCount; ++i) {
            if (i) out += ',';
            out += JsonString(Needs::kNeeds[i].id);
        }
        out += R"(],"cross":)";
        AppendNameList(out, kCrossNames);
        out += R"(,"kinds":[)";
        for (int i = 0; i < static_cast<int>(Effect::Kind::_Count); ++i) {
            if (i) out += ',';
            out += JsonString(Effect::KindName(static_cast<Effect::Kind>(i)));
        }
        out += R"(],"src":)";
        AppendNameList(out, head.sourceNames);
        out += R"(,"row":["form","uid","kind","src","cap","flags","slot","util","x","wp"])";
        out += R"(,"flags":{"eligible":1,"scored":2,"held":4,"equipped":8,"shown":16,"wildcard":32,)"
               R"("override":64,"remembered":128,"addedAtPick":256})";
        out += R"(,"episode":{"onset":)";
        AppendNum(out, head.onset);
        out += R"(,"expiry":)";
        AppendNum(out, head.expiry);
        out += R"(,"minSec":)";
        AppendSeconds(out, head.minSec);
        out += R"(,"graceSec":)";
        AppendSeconds(out, head.graceSec);
        out += "}}\n";
        return out;
    }

    int Encoder::CapId(const Effect::Cap* cap, std::string& defs)
    {
        if (!cap) return -1;
        std::string text = CapText(*cap);
        const auto [it, fresh] = capByText_.try_emplace(std::move(text), static_cast<int>(capByText_.size()));
        if (fresh) {
            defs += R"({"t":"cap","id":)";
            AppendInt(defs, it->second);
            defs += R"(,"c":)";
            defs += it->first;
            defs += "}\n";
        }
        return it->second;
    }

    void Encoder::AppendRow(std::string& out, const Row& r, std::string& defs)
    {
        int cap = -1;
        if (r.cap) {
            if (!r.capOwner) {
                // A catalog-owned cap: immutable for the process, so its address
                // is a safe key. Per-instance caps are owned elsewhere and an
                // address can be reused after one is freed: those go by text.
                if (const auto it = capByStatic_.find(r.cap); it != capByStatic_.end()) {
                    cap = it->second;
                }
                else {
                    cap = CapId(r.cap, defs);
                    capByStatic_.emplace(r.cap, cap);
                }
            }
            else {
                cap = CapId(r.cap, defs);
            }
        }
        out += "[\"";
        out += FormHex(r.form);
        out += "\",";
        AppendUInt(out, r.uid);
        out += ',';
        if (r.kind == kNoKind) out += "null";
        else AppendInt(out, r.kind);
        out += ',';
        if (r.src == kNoSource) out += "null";
        else AppendInt(out, r.src);
        out += ',';
        AppendInt(out, cap);
        out += ',';
        AppendUInt(out, r.flags);
        out += ',';
        AppendInt(out, r.slot);
        out += ',';
        AppendNum(out, r.util);
        out += ',';
        AppendSparse(out, r.cross);
        out += ',';
        AppendNum(out, r.wildcardP);
        out += ']';
    }

    std::string Encoder::Encode(const Decision& d)
    {
        std::string defs;   // cap lines, then the context line
        std::string ctxLine;
        std::string dec;
        dec.reserve(512);

        std::uint64_t ctxId = 0;
        if (d.ctx) {
            const Context& c = *d.ctx;
            ctxId = c.id;
            if (ctxDefined_.insert(c.id).second) {
                ctxLine.reserve(64 + c.rows.size() * 48);
                ctxLine += R"({"t":"ctx","id":)";
                AppendUInt(ctxLine, c.id);
                ctxLine += R"(,"utc":)";
                ctxLine += JsonString(c.utc);
                ctxLine += R"(,"why":)";
                ctxLine += JsonString(c.why);
                if (!c.menu.empty()) {
                    ctxLine += R"(,"menu":)";
                    ctxLine += JsonString(c.menu);
                }
                ctxLine += R"(,"need":)";
                AppendSparse(ctxLine, c.need);
                ctxLine += R"(,"in":)";
                AppendSparse(ctxLine, c.input);
                ctxLine += R"(,"pipe":{"ok":)";
                ctxLine += c.pipeValid ? '1' : '0';
                ctxLine += R"(,"page":)";
                AppendInt(ctxLine, c.page);
                ctxLine += R"(,"slots":)";
                AppendInt(ctxLine, c.pageSlots);
                ctxLine += R"(,"ageMs":)";
                AppendMs(ctxLine, c.pipeAgeMs);
                ctxLine += R"(},"race":)";
                ctxLine += c.race.empty() ? std::string("null") : JsonString(c.race);
                ctxLine += R"(,"wc":{"base":)";
                AppendNum(ctxLine, c.wildcardBase);
                ctxLine += R"(,"max":)";
                AppendNum(ctxLine, c.wildcardMax);
                ctxLine += R"(},"rows":[)";
                for (std::size_t i = 0; i < c.rows.size(); ++i) {
                    if (i) ctxLine += ',';
                    AppendRow(ctxLine, c.rows[i], defs);
                }
                ctxLine += "]}\n";
            }
        }

        dec += R"({"t":"dec","v":)";
        AppendInt(dec, kVersion);
        dec += R"(,"seq":)";
        AppendUInt(dec, d.seq);
        dec += R"(,"utc":)";
        dec += JsonString(d.utc);
        dec += R"(,"launch":)";
        dec += JsonString(d.launch);
        dec += R"(,"list":)";
        dec += JsonString(d.list);
        dec += R"(,"char":")";
        {
            static constexpr char kHex[] = "0123456789ABCDEF";
            for (int s = 60; s >= 0; s -= 4) dec += kHex[(d.character >> s) & 0xF];
        }
        dec += R"(","gen":)";
        AppendUInt(dec, d.gen);
        dec += R"(,"out":")";
        dec += OutcomeName(d.outcome);
        dec += R"(","form":)";
        if (d.outcome == Outcome::Nothing || d.form == 0) {
            dec += "null";
        }
        else {
            dec += '"';
            dec += FormHex(d.form);
            dec += '"';
        }
        dec += R"(,"name":)";
        dec += d.name.empty() ? std::string("null") : JsonString(d.name);
        dec += R"(,"row":)";
        AppendInt(dec, d.row);
        dec += R"(,"add":[)";
        for (std::size_t i = 0; i < d.added.size(); ++i) {
            if (i) dec += ',';
            AppendRow(dec, d.added[i], defs);
        }
        dec += R"(],"src":)";
        dec += JsonString(d.src);
        dec += R"(,"via":)";
        dec += JsonString(d.via);
        dec += R"(,"case":)";
        dec += JsonString(d.caseLabel);
        dec += R"(,"how":)";
        dec += JsonString(d.how);
        dec += R"(,"kind":)";
        dec += JsonString(d.kind);
        dec += R"(,"confirmMs":)";
        AppendMs(dec, d.confirmMs);
        dec += R"(,"repeat":)";
        dec += d.repeat ? '1' : '0';
        dec += R"(,"learned":)";
        dec += d.learned ? '1' : '0';
        dec += R"(,"skip":)";
        dec += JsonString(d.skip);
        dec += R"(,"ctx":)";
        if (d.ctx) AppendUInt(dec, ctxId);
        else dec += "null";
        dec += R"(,"ctxAgeMs":)";
        AppendMs(dec, d.ctxAgeMs);
        dec += R"(,"open":[)";
        for (std::size_t i = 0; i < d.open.size(); ++i) {
            if (i) dec += ',';
            AppendInt(dec, d.open[i]);
        }
        dec += R"(],"ep":)";
        if (d.outcome == Outcome::Nothing && d.need >= 0 && static_cast<std::size_t>(d.need) < Needs::kNeedCount) {
            dec += R"({"need":)";
            dec += JsonString(Needs::kNeeds[static_cast<std::size_t>(d.need)].id);
            dec += R"(,"i":)";
            AppendInt(dec, d.need);
            dec += R"(,"durSec":)";
            AppendSeconds(dec, d.durSec);
            dec += R"(,"peak":)";
            AppendNum(dec, d.peak);
            dec += R"(,"onset":)";
            dec += JsonString(d.onsetUtc);
            dec += '}';
        }
        else {
            dec += "null";
        }
        dec += "}\n";

        std::string out;
        out.reserve(defs.size() + ctxLine.size() + dec.size());
        out += defs;
        out += ctxLine;
        out += dec;
        return out;
    }
}
