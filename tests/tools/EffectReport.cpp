// =============================================================================
// huginn_effect_report -- run the effect mapper over an `hg dump all` CSV
// =============================================================================
// The host side of R2's done-criteria (docs/roadmap.md): coverage of the
// mapper on a whole load order, the coverage diff against today's slot
// classes, and -- on a 0.23.10 dump -- a check that the in-game catalog equals
// what the host mapper makes of the same rows.
//
//   huginn_effect_report <Huginn_All.csv> [options]
//     --name <label>            label for the report (default: the file name)
//     --min-coverage <pct>      exit 1 if coverage is below
//     --classes <csv>           legacy slot classes: formID,kind,slotClass rows
//                               (when the dump has no slotClass column)
//     --diff-out <csv>          write the coverage diff (one row per item)
//     --mgef-out <csv>          write every MGEF's column and route
//     --caps-out <csv>          write every in-scope item's cap
//     --unmapped-out <csv>      write the unmapped visible effects, by rows
//
// Exit: 0 ok, 1 below --min-coverage or the in-game catalog disagrees, 2 usage.
// The dump is untrusted data: parsed, never evaluated.
// =============================================================================

#include "DumpCsv.h"
#include "core/EffectMapper.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Huginn::Core::Effect;
using Huginn::Test::Dump;
using Huginn::Test::DumpReader;

namespace
{
    std::string Hex(std::uint32_t v)
    {
        char b[16];
        std::snprintf(b, sizeof(b), "%08X", v);
        return b;
    }

    std::string Quote(std::string_view s)
    {
        std::string q = "\"";
        for (const char c : s) {
            if (c == '"') q += '"';
            q += c;
        }
        return q + "\"";
    }

    bool EffectBasedClass(std::string_view c)
    {
        return c == "DamageAny" || c == "HealingAny" || c == "BuffsAny" || c == "DefensiveAny" ||
               c == "SummonsAny" || c == "Utility" || c == "DamageMagic" || c == "PoisonsAny";
    }

    int Usage()
    {
        std::cerr << "usage: huginn_effect_report <Huginn_All.csv> [--name L] [--min-coverage PCT] [--classes CSV]\n"
                     "       [--diff-out CSV] [--mgef-out CSV] [--caps-out CSV] [--unmapped-out CSV]\n";
        return 2;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) return Usage();
    std::string path = argv[1];
    std::string name = path.substr(path.find_last_of("/\\") + 1);
    double minCoverage = -1.0;
    std::string classesPath, diffOut, mgefOut, capsOut, unmappedOut;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };
        if (a == "--name") name = next();
        else if (a == "--min-coverage") minCoverage = std::atof(next().c_str());
        else if (a == "--classes") classesPath = next();
        else if (a == "--diff-out") diffOut = next();
        else if (a == "--mgef-out") mgefOut = next();
        else if (a == "--caps-out") capsOut = next();
        else if (a == "--unmapped-out") unmappedOut = next();
        else return Usage();
    }

    Dump dump;
    DumpReader reader;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    if (!reader.Load(path, dump, &err)) {
        std::cerr << "error: " << err << "\n";
        return 2;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const BuildResult r = BuildCaps(dump.items, dump.effects, nullptr);
    const auto t2 = std::chrono::steady_clock::now();
    // The classification step alone, timed again for the report.
    const auto classesAgain = ClassifyAll(dump.effects, nullptr);
    const auto t3 = std::chrono::steady_clock::now();
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

    std::size_t inScope = 0;
    std::map<std::string, std::pair<int, int>> byKind;  // counted, mapped
    std::map<std::string, int> byRoute;                  // counted rows by route
    std::map<std::string, int> unmapped;                 // "archetype|name" -> rows
    for (std::size_t i = 0; i < r.mappings.size(); ++i) {
        const auto& m = r.mappings[i];
        if (!m.inScope) continue;
        ++inScope;
        auto& k = byKind[std::string(KindName(m.kind))];
        k.first += m.tally.counted;
        k.second += m.tally.mapped;
        const auto& item = dump.items[i];
        for (std::size_t j = 0; j < m.outcomes.size(); ++j) {
            const auto& o = m.outcomes[j];
            if (!o.counted) continue;
            const std::string route = o.mapped ? std::string(RouteName(o.cls.route)) : "unmapped";
            ++byRoute[route];
            if (!o.mapped) {
                const auto& e = dump.effects[item.effects[j].effect];
                ++unmapped[std::to_string(e.archetype) + "|" + e.name];
            }
        }
    }

    std::printf("== %s\n", name.c_str());
    std::printf("dump: %zu rows, %zu forms, %zu effects (tome column: %s, descriptions: %s, catalog view: %s)\n",
                dump.rows, dump.items.size(), dump.effects.size(), dump.hasTome ? "yes" : "no",
                dump.hasDescription ? "yes" : "no", dump.hasPayload ? "yes" : "no");
    std::printf("in scope: %zu items\n", inScope);
    const auto& t = r.tally;
    std::printf("effect rows: %d (visible %d, hidden %d, hidden kept %d)\n", t.rows, t.visible, t.hidden, t.hiddenKept);
    std::printf("visible: helper %d, wrapper without payload %d, counted %d, mapped %d\n", t.helper, t.wrapperUnknown,
                t.counted, t.mapped);
    std::printf("COVERAGE %.2f%% (%d of %d visible effect rows)\n", 100.0 * r.Coverage(), t.mapped, t.counted);
    std::printf("by route:");
    for (const auto& [k, v] : byRoute) std::printf(" %s=%d", k.c_str(), v);
    std::printf("\nby kind:");
    for (const auto& [k, v] : byKind) {
        std::printf(" %s=%.2f%%(%d)", k.c_str(), v.first ? 100.0 * v.second / v.first : 100.0, v.first);
    }
    std::printf("\ntiming: parse %.0f ms, classify+map+grade %.0f ms (classify alone %.0f ms, %zu effects)\n",
                ms(t0, t1), ms(t1, t2), ms(t2, t3), classesAgain.size());

    std::vector<std::pair<int, std::string>> top;
    for (const auto& [k, v] : unmapped) top.emplace_back(v, k);
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    std::printf("top unmapped (archetype|name: rows):\n");
    for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 25); ++i) {
        std::printf("  %5d  %s\n", top[i].first, top[i].second.c_str());
    }

    int status = 0;

    // ---- the in-game catalog vs the host mapper ----------------------------
    if (dump.hasPayload) {
        std::size_t compared = 0, capDiff = 0, scopeDiff = 0, rowDiff = 0;
        std::vector<std::string> examples;
        for (std::size_t i = 0; i < r.mappings.size(); ++i) {
            const bool hostIn = r.mappings[i].inScope;
            const bool gameIn = dump.gameInScope[i] == "1";
            if (hostIn != gameIn) {
                ++scopeDiff;
                if (examples.size() < 10) examples.push_back("scope " + Hex(dump.items[i].formId));
                continue;
            }
            if (!hostIn) continue;
            ++compared;
            const std::string host = FormatCap(r.caps[i]);
            if (host != dump.gameCap[i]) {
                ++capDiff;
                if (examples.size() < 10) {
                    examples.push_back("cap " + Hex(dump.items[i].formId) + " game[" + dump.gameCap[i] + "] host[" + host + "]");
                }
            }
            const auto& o = r.mappings[i].outcomes;
            for (std::size_t j = 0; j < o.size() && j < dump.gameRowColumn[i].size(); ++j) {
                const std::string hc = o[j].cls.Mapped() ? std::string(Name(o[j].cls.col)) : std::string{};
                if (hc != dump.gameRowColumn[i][j]) ++rowDiff;
            }
        }
        std::printf("in-game catalog vs host: %zu items compared, %zu cap differences, %zu scope differences, %zu row-column differences\n",
                    compared, capDiff, scopeDiff, rowDiff);
        for (const auto& e : examples) std::printf("  %s\n", e.c_str());
        if (capDiff || scopeDiff || rowDiff) status = 1;
    }

    // ---- coverage diff: a slot class today, nothing in cap(i) --------------
    std::unordered_map<std::uint32_t, std::string> legacy;
    if (!classesPath.empty()) {
        std::ifstream in(classesPath, std::ios::binary);
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            const auto f = Huginn::Test::SplitCsv(line);
            if (f.size() >= 3) legacy[Huginn::Test::ParseHex(f[0])] = f[2];
        }
    }
    const bool haveClasses = !legacy.empty() || std::any_of(dump.gameSlotClass.begin(), dump.gameSlotClass.end(),
                                                           [](const std::string& s) { return !s.empty(); });
    if (haveClasses) {
        std::ofstream diff;
        if (!diffOut.empty()) {
            diff.open(diffOut, std::ios::binary);
            diff << "formID,kind,plugin,name,slotClass,effectBased,cap\n";
        }
        std::map<std::string, int> perClass;
        int withClass = 0, listed = 0, listedEffectBased = 0;
        for (std::size_t i = 0; i < r.mappings.size(); ++i) {
            if (!r.mappings[i].inScope) continue;
            const auto& it = dump.items[i];
            std::string cls = dump.gameSlotClass[i];
            if (cls.empty()) {
                const auto f = legacy.find(it.formId);
                if (f != legacy.end()) cls = f->second;
            }
            if (cls.empty() || cls == "Regular" || cls == "-") continue;
            ++withClass;
            if (DescribesItem(r.caps[i], it.kind)) continue;
            ++listed;
            const bool eb = EffectBasedClass(cls);
            if (eb) ++listedEffectBased;
            ++perClass[cls];
            if (diff) {
                diff << Hex(it.formId) << ',' << KindName(it.kind) << ',' << Quote(it.plugin) << ',' << Quote(it.name)
                     << ',' << cls << ',' << (eb ? 1 : 0) << ',' << Quote(FormatCap(r.caps[i])) << '\n';
            }
        }
        std::printf("coverage diff: %d in-scope items have a slot class today; %d of them have no description in cap(i) (%d in an effect-based class)\n",
                    withClass, listed, listedEffectBased);
        for (const auto& [k, v] : perClass) std::printf("  %-14s %d\n", k.c_str(), v);
    }

    if (!mgefOut.empty()) {
        std::ofstream out(mgefOut, std::ios::binary);
        out << "effectFormID,effectName,archetype,primaryAV,detrimental,hidden,column,column2,route\n";
        for (std::size_t i = 0; i < dump.effects.size(); ++i) {
            const auto& e = dump.effects[i];
            const auto& c = r.classes[i];
            out << Hex(e.formId) << ',' << Quote(e.name) << ',' << e.archetype << ',' << e.primaryAV << ','
                << (e.detrimental ? 1 : 0) << ',' << (e.HiddenInUI() ? 1 : 0) << ',' << Name(c.col) << ','
                << Name(c.col2) << ',' << RouteName(c.route) << '\n';
        }
    }
    if (!capsOut.empty()) {
        std::ofstream out(capsOut, std::ios::binary);
        out << "kind,formID,name,cap\n";
        for (std::size_t i = 0; i < r.mappings.size(); ++i) {
            if (!r.mappings[i].inScope) continue;
            out << KindName(dump.items[i].kind) << ',' << Hex(dump.items[i].formId) << ',' << Quote(dump.items[i].name)
                << ',' << Quote(FormatCap(r.caps[i])) << '\n';
        }
    }
    if (!unmappedOut.empty()) {
        std::ofstream out(unmappedOut, std::ios::binary);
        out << "rows,archetype,effectName\n";
        for (const auto& [v, k] : top) {
            const auto bar = k.find('|');
            out << v << ',' << k.substr(0, bar) << ',' << Quote(k.substr(bar + 1)) << '\n';
        }
    }

    if (minCoverage >= 0.0 && 100.0 * r.Coverage() < minCoverage) {
        std::printf("FAIL: coverage %.2f%% below %.2f%%\n", 100.0 * r.Coverage(), minCoverage);
        status = 1;
    }
    return status;
}
