#pragma once

// =============================================================================
// DUMP CSV -> EFFECT RECORDS (host only: tests and tools, never the plugin)
// =============================================================================
// Reads an `hg dump all` CSV (any version: 0.23.6 without winningPlugin, the
// 0.23.7 one with taughtByTome and effectDescription, or 0.23.10's catalog
// view with the dump-gap columns) into the plain records the effect mapper
// takes. A dump is untrusted data: every field is parsed, nothing is
// evaluated; a malformed number reads as 0.
//
// Row layout: one row per item x effect; an item with no effects has one row
// with the effect block empty. 0.23.10 adds payload rows (`payloadOf` = the
// effectIndex of the Cloak/hazard row they belong to): they become that
// effect's payload, not the item's own effects.
// =============================================================================

#include "core/EffectRecords.h"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Huginn::Test
{
    /// Split one CSV record (RFC 4180 quoting; no embedded newlines -- the
    /// dumps fold them).
    inline std::vector<std::string> SplitCsv(std::string_view line)
    {
        std::vector<std::string> out;
        std::string cur;
        bool quoted = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (quoted) {
                if (c == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') {
                        cur += '"';
                        ++i;
                    }
                    else {
                        quoted = false;
                    }
                }
                else {
                    cur += c;
                }
            }
            else if (c == '"') {
                quoted = true;
            }
            else if (c == ',') {
                out.push_back(std::move(cur));
                cur.clear();
            }
            else if (c != '\r') {
                cur += c;
            }
        }
        out.push_back(std::move(cur));
        return out;
    }

    inline std::vector<std::string> SplitList(std::string_view s, char sep = ';')
    {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= s.size()) {
            const auto end = s.find(sep, start);
            const auto part = s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            if (!part.empty()) out.emplace_back(part);
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        return out;
    }

    inline std::uint32_t ParseHex(std::string_view s)
    {
        std::uint32_t v = 0;
        std::from_chars(s.data(), s.data() + s.size(), v, 16);
        return v;
    }
    inline int ParseInt(std::string_view s)
    {
        int v = 0;
        if (!s.empty() && s.front() == '+') s.remove_prefix(1);
        std::from_chars(s.data(), s.data() + s.size(), v);
        return v;
    }
    inline float ParseFloat(std::string_view s)
    {
        float v = 0.0f;
        std::from_chars(s.data(), s.data() + s.size(), v);
        return v;
    }
    inline std::uint32_t ParseU32(std::string_view s)
    {
        // Durations and areas are written as integers; tolerate "3.0".
        const float f = ParseFloat(s);
        return f > 0.0f ? static_cast<std::uint32_t>(f) : 0u;
    }

    /// One parsed dump: the records plus the in-game view's columns, when the
    /// dump has them (0.23.10).
    struct Dump
    {
        std::vector<Core::Effect::ItemRecord> items;
        Core::Effect::EffectTable effects;
        std::vector<std::string> header;
        bool hasTome = false;
        bool hasDescription = false;
        bool hasPayload = false;  // 0.23.10 catalog view
        // Per item (index-aligned with items): the in-game cap ("col=value;...")
        // and scope flag, as the game wrote them.
        std::vector<std::string> gameCap;
        std::vector<std::string> gameInScope;
        // Per item: each direct effect row's in-game column and route.
        std::vector<std::vector<std::string>> gameRowColumn;
        std::vector<std::vector<std::string>> gameRowRoute;
        // Per item: the legacy slot class the game wrote ("" if none).
        std::vector<std::string> gameSlotClass;
        std::size_t rows = 0;
    };

    class DumpReader
    {
    public:
        bool Load(const std::string& path, Dump& out, std::string* error = nullptr)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                if (error) *error = "cannot open " + path;
                return false;
            }
            std::string line;
            if (!std::getline(in, line)) {
                if (error) *error = "empty file";
                return false;
            }
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);  // BOM
            out.header = SplitCsv(line);
            col_.clear();
            for (std::size_t i = 0; i < out.header.size(); ++i) col_[out.header[i]] = i;
            out.hasTome = Has("taughtByTome");
            out.hasDescription = Has("effectDescription");
            out.hasPayload = Has("payloadOf");

            std::string lastKey;
            std::unordered_map<std::string, std::uint32_t> rowEffectByIndex;  // effectIndex -> mgef, current item
            while (std::getline(in, line)) {
                if (line.empty() || line == "\r") continue;
                const auto f = SplitCsv(line);
                ++out.rows;
                const std::string key = Get(f, "kind") + ":" + Get(f, "formID");
                if (key != lastKey) {
                    lastKey = key;
                    rowEffectByIndex.clear();
                    StartItem(f, out);
                }
                auto& item = out.items.back();
                if (Get(f, "effectFormID").empty()) continue;
                const std::uint32_t mi = Effect(f, out);
                Core::Effect::EffectRow row;
                row.effect = mi;
                row.magnitude = ParseFloat(Get(f, "magnitude"));
                row.duration = ParseU32(Get(f, "duration"));
                row.area = ParseU32(Get(f, "area"));
                // Before 0.23.7 `effectCost` was the MGEF's base cost; the per-effect
                // cost exists only next to `effectBaseCost`.
                if (Has("effectBaseCost")) row.cost = ParseFloat(Get(f, "effectCost"));
                const std::string payloadOf = Get(f, "payloadOf");
                if (!payloadOf.empty()) {
                    // A payload row: it belongs to its wrapper's MGEF. The dump
                    // repeats the payload under every item carrying the wrapper;
                    // keep the first item's copy only.
                    const auto it = rowEffectByIndex.find(payloadOf);
                    if (it != rowEffectByIndex.end()) {
                        auto& owner = payloadOwner_[it->second];
                        const auto itemIndex = static_cast<std::int64_t>(out.items.size() - 1);
                        if (owner < 0) owner = itemIndex;
                        if (owner == itemIndex) out.effects[it->second].payload.push_back(row);
                    }
                    continue;
                }
                item.effects.push_back(row);
                out.gameRowColumn.back().push_back(Get(f, "effectColumn"));
                out.gameRowRoute.back().push_back(Get(f, "effectRoute"));
                rowEffectByIndex[Get(f, "effectIndex")] = mi;
            }
            return true;
        }

    private:
        std::unordered_map<std::string, std::size_t> col_;
        std::unordered_map<std::uint32_t, std::uint32_t> mgefIndex_;
        std::unordered_map<std::uint32_t, std::int64_t> payloadOwner_;  // mgef index -> item that filled its payload

        [[nodiscard]] bool Has(const std::string& name) const { return col_.contains(name); }
        [[nodiscard]] std::string Get(const std::vector<std::string>& f, const std::string& name) const
        {
            const auto it = col_.find(name);
            if (it == col_.end() || it->second >= f.size()) return {};
            return f[it->second];
        }

        void StartItem(const std::vector<std::string>& f, Dump& out)
        {
            using namespace Core::Effect;
            ItemRecord it;
            const auto kind = KindFromName(Get(f, "kind"));
            it.kind = kind.value_or(Kind::_Count);
            it.formId = ParseHex(Get(f, "formID"));
            it.plugin = Get(f, "plugin");
            it.name = Get(f, "name");
            it.playable = Get(f, "playable") != "0";
            it.value = ParseInt(Get(f, "value"));
            it.weight = ParseFloat(Get(f, "weight"));
            it.keywords = SplitList(Get(f, "keywords"));
            const std::string st = Get(f, "spellType");
            if (!st.empty()) it.spellType = ParseInt(st);
            const std::string ct = Get(f, "castingType");
            if (!ct.empty()) it.castingType = ParseInt(ct);
            const std::string dl = Get(f, "delivery");
            if (!dl.empty()) it.delivery = ParseInt(dl);
            it.magickaCost = ParseFloat(Get(f, "magickaCost"));
            const std::string tome = Get(f, "taughtByTome");
            if (out.hasTome && it.kind == Kind::Spell) it.taughtByTome = tome == "1";
            const std::string wt = Get(f, "weaponType");
            if (!wt.empty()) it.weaponType = ParseInt(wt);
            it.twoHanded = Get(f, "twoHanded") == "1";
            it.damage = ParseFloat(Get(f, "damage"));
            it.speed = ParseFloat(Get(f, "speed"));
            it.reach = ParseFloat(Get(f, "reach"));
            it.critDamage = ParseFloat(Get(f, "critDamage"));
            const std::string nb = Get(f, "ammoNonBolt");
            if (!nb.empty()) it.ammoNonBolt = nb == "1";
            it.slotMask = ParseHex(Get(f, "armorSlots"));
            it.armorRating = ParseFloat(Get(f, "armorRating"));
            const std::string at = Get(f, "armorType");
            it.armourWeight = at == "Heavy" ? ArmourWeight::Heavy : at == "Light" ? ArmourWeight::Light : ArmourWeight::Clothing;
            it.soulCapacity = ParseInt(Get(f, "soulCapacity"));
            it.soulContained = ParseInt(Get(f, "soulContained"));
            it.lightRadius = ParseInt(Get(f, "lightRadius"));
            it.enchanted = !Get(f, "enchantmentCharge").empty() || !Get(f, "enchantment").empty();
            const std::string ec = Get(f, "enchantCasting");
            if (!ec.empty()) it.enchantCastingType = ParseInt(ec);
            out.items.push_back(std::move(it));
            out.gameCap.push_back(Get(f, "cap"));
            out.gameInScope.push_back(Get(f, "inScope"));
            out.gameSlotClass.push_back(Get(f, "slotClass"));
            out.gameRowColumn.emplace_back();
            out.gameRowRoute.emplace_back();
        }

        std::uint32_t Effect(const std::vector<std::string>& f, Dump& out)
        {
            const std::uint32_t id = ParseHex(Get(f, "effectFormID"));
            const auto it = mgefIndex_.find(id);
            if (it != mgefIndex_.end()) return it->second;
            Core::Effect::MagicEffectRecord m;
            m.formId = id;
            m.plugin = Get(f, "effectPlugin");
            m.name = Get(f, "effectName");
            const std::string a = Get(f, "archetype");
            m.archetype = a.empty() ? -1 : ParseInt(a);
            m.primaryAV = Get(f, "primaryAV");
            m.secondaryAV = Get(f, "secondaryAV");
            m.resistAV = Get(f, "resistAV");
            m.delivery = ParseInt(Get(f, "effectDelivery"));
            m.castingType = ParseInt(Get(f, "effectCasting"));
            const std::string bc = Has("effectBaseCost") ? Get(f, "effectBaseCost") : Get(f, "effectCost");
            m.baseCost = ParseFloat(bc);
            m.flags = ParseHex(Get(f, "effectFlags"));
            m.detrimental = Get(f, "detrimental") == "1";
            m.hostile = Get(f, "hostile") == "1";
            m.keywords = SplitList(Get(f, "effectKeywords"));
            m.description = Get(f, "effectDescription");
            m.school = Get(f, "effectSchool");
            m.payloadKnown = out.hasPayload;
            const auto index = static_cast<std::uint32_t>(out.effects.size());
            out.effects.push_back(std::move(m));
            mgefIndex_[id] = index;
            payloadOwner_[index] = -1;
            return index;
        }
    };
}
