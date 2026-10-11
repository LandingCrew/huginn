#include "EffectConditions.h"

#include <algorithm>
#include <charconv>

namespace Huginn::Core::Effect
{
    namespace
    {
        bool IsSpace(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

        std::string_view Trim(std::string_view s) noexcept
        {
            while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
            while (!s.empty() && IsSpace(s.back())) s.remove_suffix(1);
            return s;
        }

        bool IsOpChar(char c) noexcept { return c == '=' || c == '!' || c == '<' || c == '>'; }

        /// One printed condition: [T.]Name(params)<op><value>.
        bool ParseOne(std::string_view s, Condition& c)
        {
            s = Trim(s);
            if (s.substr(0, 2) == "T.") {
                c.onTarget = true;
                s.remove_prefix(2);
            }
            const auto open = s.find('(');
            if (open == std::string_view::npos || open == 0) return false;
            // The parameters end at the last ')' before the operator; a
            // parameter never holds one (labels, "<type N>", numbers).
            std::size_t opAt = std::string_view::npos;
            for (std::size_t i = open + 1; i < s.size(); ++i) {
                if (s[i] == ')' && i + 1 < s.size() && IsOpChar(s[i + 1])) {
                    opAt = i + 1;
                    break;
                }
            }
            if (opAt == std::string_view::npos) return false;
            c.function = std::string(s.substr(0, open));
            const auto params = s.substr(open + 1, opAt - 1 - (open + 1));
            c.param = std::string(Trim(params.substr(0, params.find(','))));
            std::size_t valAt = opAt;
            while (valAt < s.size() && IsOpChar(s[valAt]) && valAt - opAt < 2) ++valAt;
            c.op = std::string(s.substr(opAt, valAt - opAt));
            static constexpr std::string_view kOps[] = { "==", "!=", ">", ">=", "<", "<=" };
            if (std::find(std::begin(kOps), std::end(kOps), c.op) == std::end(kOps)) return false;
            const auto value = Trim(s.substr(valAt));
            if (value.empty()) return false;
            float v = 0.0f;
            const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
            if (r.ec == std::errc{} && r.ptr == value.data() + value.size()) {
                c.value = v;
            }
            else {
                c.global = true;  // a global's label: the value is not in the text
            }
            return true;
        }

        /// A true test of a 0/1 function: ==1, !=0, >=1, >0.
        bool TrueTest(const Condition& c) noexcept
        {
            if (c.global) return false;
            return (c.op == "==" && c.value == 1.0f) || (c.op == "!=" && c.value == 0.0f) ||
                   (c.op == ">=" && c.value == 1.0f) || (c.op == ">" && c.value == 0.0f);
        }

        /// "000153D2" -> 0x000153D2; anything not 8 hex digits -> 0 (a label).
        std::uint32_t PerkId(std::string_view p) noexcept
        {
            if (p.size() != 8) return 0;
            std::uint32_t v = 0;
            const auto r = std::from_chars(p.data(), p.data() + p.size(), v, 16);
            return (r.ec == std::errc{} && r.ptr == p.data() + p.size()) ? v : 0;
        }

        bool PlayerPerk(const Condition& c, bool subjectIsUser) noexcept
        {
            return c.function == "HasPerk" && (c.onTarget || subjectIsUser) && TrueTest(c);
        }

        bool Random(const Condition& c) noexcept { return c.function == "GetRandomPercent" && !c.global; }

        bool TargetFamily(const Condition& c) noexcept
        {
            if (c.onTarget || !TrueTest(c)) return false;
            if (c.function == "IsUndead" || c.function == "GetIsRace") return true;
            if (c.function != "HasKeyword") return false;
            if (c.param == "Vampire") return true;
            return c.param.find("ActorType") != std::string::npos && c.param != "ActorTypeNPC";
        }
    }

    std::vector<Condition> ParseConditions(std::string_view text)
    {
        std::vector<Condition> out;
        text = Trim(text);
        while (!text.empty()) {
            const auto andAt = text.find(" AND ");
            const auto orAt = text.find(" OR ");
            const auto at = std::min(andAt, orAt);
            Condition c;
            const bool ok = ParseOne(text.substr(0, at), c);
            if (at == std::string_view::npos) {
                if (ok) out.push_back(std::move(c));
                break;
            }
            c.orNext = at == orAt;
            if (ok) out.push_back(std::move(c));
            text.remove_prefix(at + (at == orAt ? 4 : 5));
        }
        return out;
    }

    std::vector<Clause> ReadClauses(const std::vector<Condition>& conditions)
    {
        std::vector<Clause> out;
        bool open = false;
        for (const auto& c : conditions) {
            if (!open) out.emplace_back();
            out.back().push_back(c);
            open = c.orNext;
        }
        return out;
    }

    float RandomChance(std::string_view op, float value) noexcept
    {
        // GetRandomPercent returns an integer 0..99.
        float p = 1.0f;
        if (op == "<") p = value / 100.0f;
        else if (op == "<=") p = (value + 1.0f) / 100.0f;
        else if (op == ">") p = (99.0f - value) / 100.0f;
        else if (op == ">=") p = (100.0f - value) / 100.0f;
        else if (op == "==") p = (value >= 0.0f && value <= 99.0f) ? 0.01f : 0.0f;
        else if (op == "!=") p = (value >= 0.0f && value <= 99.0f) ? 0.99f : 1.0f;
        return std::clamp(p, 0.0f, 1.0f);
    }

    Gate ReadGate(std::string_view conditions, bool subjectIsUser)
    {
        Gate g;
        if (Trim(conditions).empty()) return g;
        for (const auto& clause : ReadClauses(ParseConditions(conditions))) {
            if (clause.empty()) continue;
            if (std::all_of(clause.begin(), clause.end(), [&](const Condition& c) { return PlayerPerk(c, subjectIsUser); })) {
                std::vector<std::uint32_t> ids;
                for (const auto& c : clause) ids.push_back(PerkId(c.param));
                g.perks.push_back(std::move(ids));
            }
            else if (std::all_of(clause.begin(), clause.end(), Random)) {
                float miss = 1.0f;
                for (const auto& c : clause) miss *= 1.0f - RandomChance(c.op, c.value);
                g.chance *= 1.0f - miss;
            }
            else if (std::all_of(clause.begin(), clause.end(), TargetFamily)) {
                g.targetFamily = true;
            }
        }
        return g;
    }

    void MergeGate(Gate& into, const Gate& other)
    {
        into.chance *= other.chance;
        into.perks.insert(into.perks.end(), other.perks.begin(), other.perks.end());
        into.targetFamily = into.targetFamily || other.targetFamily;
    }

    bool TestsSubjectDead(std::string_view conditions)
    {
        if (conditions.find("GetDead") == std::string_view::npos) return false;
        for (const auto& c : ParseConditions(conditions)) {
            if (c.function == "GetDead" && !c.onTarget && TrueTest(c)) return true;
        }
        return false;
    }
}
