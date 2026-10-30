#include "DMenuDefaults.h"

#include "core/MiniJson.h"

#include <cmath>
#include <cstdio>
#include <format>
#include <map>

namespace Huginn::Core::DMenu
{
    namespace
    {
        using Json::Value;

        // C's "%f" of a float promoted to double: what std::to_string(float)
        // returns, and so what dMenu writes for a slider.
        std::string PrintfF(float v)
        {
            char buf[64];
            const int n = std::snprintf(buf, sizeof(buf), "%f", static_cast<double>(v));
            return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string{};
        }

        bool Walk(const Value& entries, std::vector<Entry>& out, std::string& error)
        {
            if (!entries.IsArray()) {
                error = "an \"entries\" / \"data\" member is not an array";
                return false;
            }
            for (const auto& e : entries.Items()) {
                if (!e.IsObject()) {
                    error = "an entry is not an object";
                    return false;
                }
                const Value* typeV = e.Find("type");
                const std::string type = typeV && typeV->IsString() ? typeV->AsString() : std::string{};
                if (type == "group") {
                    const Value* inner = e.Find("entries");
                    if (inner && !Walk(*inner, out, error)) return false;
                    continue;
                }
                const Value* ini = e.Find("ini");
                if (!ini) continue;   // a button, or text: nothing stored
                const Value* sec = ini->Find("section");
                const Value* id = ini->Find("id");
                if (!sec || !sec->IsString() || !id || !id->IsString() || sec->AsString().empty() ||
                    id->AsString().empty()) {
                    error = std::format("a \"{}\" entry's \"ini\" has no section or id", type);
                    return false;
                }
                const Value* def = e.Find("default");
                Entry entry{ sec->AsString(), id->AsString(), {} };
                if (type == "checkbox") {
                    if (def && !def->IsBool()) {
                        error = std::format("[{}] {}: a checkbox default must be true or false", entry.section, entry.key);
                        return false;
                    }
                    entry.value = def && def->AsBool() ? "true" : "false";
                } else if (type == "slider") {
                    if (def && (!def->IsNumber() || !std::isfinite(def->AsNumber()))) {
                        error = std::format("[{}] {}: a slider default must be a number", entry.section, entry.key);
                        return false;
                    }
                    entry.value = PrintfF(def ? static_cast<float>(def->AsNumber()) : 0.0f);
                } else if (type == "dropdown" || type == "keymap") {
                    if (def && (!def->IsNumber() || !def->IsIntegerText())) {
                        error = std::format("[{}] {}: a {} default must be an integer", entry.section, entry.key, type);
                        return false;
                    }
                    entry.value = std::to_string(def ? static_cast<long long>(def->AsNumber()) : 0LL);
                } else if (type == "textbox") {
                    if (def && !def->IsString()) {
                        error = std::format("[{}] {}: a textbox default must be a string", entry.section, entry.key);
                        return false;
                    }
                    entry.value = def ? def->AsString() : std::string{};
                } else {
                    error = std::format("[{}] {}: entry type \"{}\" has no known serialization", entry.section,
                        entry.key, type);
                    return false;
                }
                out.push_back(std::move(entry));
            }
            return true;
        }
    }

    DefaultsResult DefaultsFromDescriptor(std::string_view json)
    {
        DefaultsResult r;
        const auto parsed = Json::Parse(json);
        if (!parsed.Ok()) {
            r.error = "the descriptor is not JSON: " + parsed.error;
            return r;
        }
        const Value* data = parsed.value->Find("data");
        if (!data) {
            r.error = "the descriptor has no \"data\" array";
            return r;
        }
        if (!Walk(*data, r.entries, r.error)) {
            r.entries.clear();
        }
        return r;
    }

    std::string ToIniText(const std::vector<Entry>& entries)
    {
        std::vector<std::string> sections;
        for (const auto& e : entries) {
            bool seen = false;
            for (const auto& s : sections) seen = seen || s == e.section;
            if (!seen) sections.push_back(e.section);
        }
        std::string text;
        for (const auto& s : sections) {
            if (!text.empty()) text += '\n';
            text += '[' + s + "]\n";
            for (const auto& e : entries) {
                if (e.section == s) text += e.key + " = " + e.value + '\n';
            }
        }
        return text;
    }

    namespace
    {
        std::string_view TrimView(std::string_view v)
        {
            const auto b = v.find_first_not_of(" \t\r");
            if (b == std::string_view::npos) return {};
            const auto e = v.find_last_not_of(" \t\r");
            return v.substr(b, e - b + 1);
        }

        using IniMap = std::map<std::string, std::map<std::string, std::string>>;

        IniMap ParseIni(std::string_view text)
        {
            if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
            IniMap out;
            std::string section;
            while (!text.empty()) {
                const auto nl = text.find('\n');
                const std::string_view line = TrimView(text.substr(0, nl));
                text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
                if (line.empty() || line.front() == ';' || line.front() == '#') continue;
                if (line.front() == '[' && line.back() == ']') {
                    section = std::string(TrimView(line.substr(1, line.size() - 2)));
                    out[section];
                    continue;
                }
                const auto eq = line.find('=');
                if (eq == std::string_view::npos) {
                    out[section][std::string(line)];   // a bare key: present, empty
                    continue;
                }
                out[section][std::string(TrimView(line.substr(0, eq)))] = std::string(TrimView(line.substr(eq + 1)));
            }
            return out;
        }
    }

    bool SameIniContent(std::string_view a, std::string_view b)
    {
        return ParseIni(a) == ParseIni(b);
    }
}
