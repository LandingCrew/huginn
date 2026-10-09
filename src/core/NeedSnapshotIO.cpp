#include "NeedSnapshotIO.h"

#include <charconv>
#include <cstdint>
#include <system_error>
#include <type_traits>

namespace Huginn::Core::Needs
{
    namespace
    {
        std::string FloatText(float v)
        {
            char buf[32];
            const auto r = std::to_chars(buf, buf + sizeof(buf), v);
            return std::string(buf, r.ptr);
        }

        std::string FieldText(const NeedSnapshot& s, const FieldPtr& ptr)
        {
            return std::visit(
                [&](auto member) -> std::string {
                    using T = std::remove_cvref_t<decltype(s.*member)>;
                    if constexpr (std::is_same_v<T, float>) {
                        return FloatText(s.*member);
                    } else if constexpr (std::is_same_v<T, bool>) {
                        return (s.*member) ? "1" : "0";
                    } else {
                        return std::to_string(s.*member);
                    }
                },
                ptr);
        }

        bool ParseField(NeedSnapshot& s, const FieldPtr& ptr, std::string_view text)
        {
            const char* first = text.data();
            const char* last = text.data() + text.size();
            return std::visit(
                [&](auto member) -> bool {
                    using T = std::remove_cvref_t<decltype(s.*member)>;
                    if constexpr (std::is_same_v<T, bool>) {
                        if (text == "0") { s.*member = false; return true; }
                        if (text == "1") { s.*member = true; return true; }
                        return false;
                    } else {
                        T v{};
                        const auto r = std::from_chars(first, last, v);
                        if (r.ec != std::errc{} || r.ptr != last) return false;
                        s.*member = v;
                        return true;
                    }
                },
                ptr);
        }

        const Field* FindField(std::string_view name)
        {
            for (const auto& f : kFields) {
                if (f.name == name) return &f;
            }
            return nullptr;
        }

        std::string_view Trim(std::string_view t)
        {
            while (!t.empty() && (t.front() == ' ' || t.front() == '\t' || t.front() == '\r')) t.remove_prefix(1);
            while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.remove_suffix(1);
            return t;
        }
    }

    std::string WriteSnapshot(std::string_view label, const NeedSnapshot& s)
    {
        std::string out = "snapshot ";
        out += label;
        out += '\n';
        for (const auto& f : kFields) {
            out += f.name;
            out += ' ';
            out += FieldText(s, f.ptr);
            out += '\n';
        }
        out += "end\n";
        return out;
    }

    std::vector<LabeledSnapshot> ReadSnapshots(std::string_view text, std::string& error)
    {
        error.clear();
        std::vector<LabeledSnapshot> out;
        bool inBlock = false;
        LabeledSnapshot current;
        std::size_t lineNo = 0;
        std::size_t pos = 0;
        auto fail = [&](std::string_view why) {
            error = "line " + std::to_string(lineNo) + ": " + std::string(why);
            return out;
        };
        while (pos <= text.size()) {
            const std::size_t nl = text.find('\n', pos);
            const std::string_view raw = text.substr(pos, nl == std::string_view::npos ? text.size() - pos : nl - pos);
            pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
            ++lineNo;
            const std::string_view line = Trim(raw);
            if (line.empty() || line.front() == '#') continue;
            if (!inBlock) {
                if (line.substr(0, 9) != "snapshot " && line != "snapshot") {
                    return fail("expected 'snapshot <label>'");
                }
                current = LabeledSnapshot{};
                current.label = std::string(Trim(line.substr(8)));
                inBlock = true;
                continue;
            }
            if (line == "end") {
                out.push_back(std::move(current));
                inBlock = false;
                continue;
            }
            const std::size_t sp = line.find_first_of(" \t");
            if (sp == std::string_view::npos) return fail("expected 'name value'");
            const std::string_view name = line.substr(0, sp);
            const std::string_view value = Trim(line.substr(sp + 1));
            const Field* f = FindField(name);
            if (!f) return fail("unknown field '" + std::string(name) + "'");
            if (!ParseField(current.snapshot, f->ptr, value)) {
                return fail("bad value '" + std::string(value) + "' for " + std::string(name));
            }
        }
        if (inBlock) {
            error = "snapshot '" + current.label + "' has no 'end'";
        }
        return out;
    }

    std::string DiffSnapshots(const NeedSnapshot& a, const NeedSnapshot& b)
    {
        std::string out;
        for (const auto& f : kFields) {
            const auto ta = FieldText(a, f.ptr);
            const auto tb = FieldText(b, f.ptr);
            if (ta != tb) {
                out += std::string(f.name) + " " + ta + " " + tb + "\n";
            }
        }
        return out;
    }
}
