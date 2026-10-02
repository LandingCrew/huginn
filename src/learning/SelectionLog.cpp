#include "SelectionLog.h"
#include "FeatureBanditLearner.h"
#include "UtilityScorer.h"
#include "Globals.h"

#include <cmath>
#include <format>
#include <fstream>
#include <mutex>
#include <string>

namespace Huginn::Learning
{
    namespace
    {
        using Row = PipelineStateCache::ScoreRow;

        std::string RankString(const Row* row)
        {
            if (!row) return "-";
            if (row->rank >= PipelineStateCache::kUnrankedTail) return "tail";
            return std::to_string(row->rank);
        }

        std::string FormName(RE::FormID formID)
        {
            const auto* form = RE::TESForm::LookupByID(formID);
            const char* name = form ? form->GetName() : nullptr;
            return (name && *name) ? name : "?";
        }

        std::string_view TypeMark(Slot::AssignmentType type)
        {
            switch (type) {
            case Slot::AssignmentType::Override:   return " [O]";
            case Slot::AssignmentType::Wildcard:   return " [W]";
            case Slot::AssignmentType::Remembered: return " [R]";
            default:                               return "";
            }
        }

        // Plugin strings are not always UTF-8: many are cp1252 (an 'Épée' is one
        // byte 0xC9, not two). A record holding such bytes raw is invalid JSON
        // and a json.loads harness would drop it, so a string that is not valid
        // UTF-8 has every high byte escaped as its cp1252/Latin-1 code point.
        bool IsValidUtf8(std::string_view text)
        {
            for (size_t i = 0; i < text.size();) {
                const auto c = static_cast<unsigned char>(text[i]);
                size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
                if (n == 0 || i + n > text.size()) return false;
                for (size_t k = 1; k < n; ++k) {
                    if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
                }
                i += n;
            }
            return true;
        }

        std::string JsonString(std::string_view text)
        {
            const bool utf8 = IsValidUtf8(text);
            std::string out;
            out.reserve(text.size() + 2);
            out += '"';
            for (const char c : text) {
                const auto u = static_cast<unsigned char>(c);
                switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (u < 0x20 || (u >= 0x80 && !utf8)) {
                        out += std::format("\\u{:04x}", static_cast<unsigned>(u));
                    } else {
                        out += c;
                    }
                }
            }
            out += '"';
            return out;
        }

        // JSON has no NaN or infinity; {:.4g} would print a bare "nan".
        std::string Num(float v)
        {
            return std::isfinite(v) ? std::format("{:.4g}", v) : std::string("null");
        }

        float Predict(RE::FormID formID, const StateFeatures& features)
        {
            return g_featureBanditLearner ? g_featureBanditLearner->GetRewardEstimate(formID, features) : 0.0f;
        }

        // ── 1. The debug log ─────────────────────────────────────────────────
        void WriteReadable(const EquipEvent& event, const char* how, float reward)
        {
            const auto& snap = event.shown;
            const Row* chosen = snap.Find(event.formID);

            // What the choice displaced: the best-ranked item shown for the same
            // need that was not chosen.
            const PipelineStateCache::ShownSlot* over = nullptr;
            const Row* overRow = nullptr;
            if (chosen) {
                for (const auto& s : snap.shown) {
                    if (s.formID == event.formID) continue;
                    const Row* row = snap.Find(s.formID);
                    if (!row || row->need != chosen->need) continue;
                    if (!overRow || row->rank < overRow->rank) {
                        over = &s;
                        overRow = row;
                    }
                }
            }

            const std::string scorePart = chosen
                ? std::format("rank={} util={:.2f} ctx={:.2f}", RankString(chosen), chosen->utility,
                      chosen->breakdown.contextWeight)
                : std::string("not a candidate");
            const std::string overPart = over
                ? std::format("{:08X} '{}'", over->formID, over->name)
                : std::string("-");

            logger::info("[Selection] Confirmed {:08X} '{}' src={} via={}{}{} kind={} reward={:+.1f} ({}, {:.0f}ms) "
                         "{} pred={:.2f} need={} over={} shown={} page={} age={:.0f}ms gen={}"sv,
                event.formID, FormName(event.formID), EquipSourceToString(event.source), event.via,
                event.attribution.empty() ? ""sv : " case="sv, event.attribution,
                SelectionKindToString(event.kind), reward, how, event.confirmMs,
                scorePart, Predict(event.formID, event.features),
                chosen ? Slot::SlotClassificationToString(chosen->need) : "-"sv,
                overPart, snap.shown.size(), snap.page, snap.ageMs, event.loadGeneration);

            for (const auto& s : snap.shown) {
                const Row* row = snap.Find(s.formID);
                const char mark = (s.formID == event.formID) ? '*'
                    : (chosen && row && row->need == chosen->need) ? '='
                    : ' ';
                // debug, not info: one line per shown slot is per-item detail
                // (CLAUDE.md logging levels). The JSONL record has it all.
                logger::debug("[Selection]   s{} {} {:08X} '{}'{} need={} rank={} util={:.2f} ctx={:.2f} pred={:.2f}"sv,
                    s.slotIndex, mark, s.formID, s.name, TypeMark(s.type),
                    row ? Slot::SlotClassificationToString(row->need) : "-"sv,
                    RankString(row), row ? row->utility : 0.0f,
                    row ? row->breakdown.contextWeight : 0.0f,
                    Predict(s.formID, event.features));
            }
        }

        // ── 2. Huginn_Selections.jsonl ───────────────────────────────────────
        void WriteRecord(const EquipEvent& event, const char* how, float reward)
        {
            static std::mutex s_fileMutex;
            static bool s_warned = false;

            const auto logDir = SKSE::log::log_directory();
            if (!logDir) return;

            const auto& snap = event.shown;
            std::string line;
            line.reserve(256 + snap.scores.size() * 120);

            const auto utc = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
            line += std::format(R"({{"v":1,"utc":"{:%F %T}","gen":{},"form":"{:08X}","name":{},"src":"{}","via":{},)",
                utc, event.loadGeneration, event.formID, JsonString(FormName(event.formID)),
                EquipSourceToString(event.source), JsonString(event.via));
            line += std::format(R"("case":{},"kind":"{}","reward":{:.2f},"how":"{}","confirmMs":{:.0f},)",
                JsonString(event.attribution), SelectionKindToString(event.kind), reward, how, event.confirmMs);
            line += std::format(R"("pipeline":{},"page":{},"ageMs":{:.0f},"sortedPrefix":{},)",
                snap.valid, snap.page, snap.ageMs, snap.sortedPrefix);

            float wcBase = 0.0f, wcMax = 0.0f;
            if (g_utilityScorer) {
                const auto& wc = g_utilityScorer->GetWildcardManager();
                wcBase = wc.GetBaseProbability();
                wcMax = wc.GetMaxProbability();
            }
            line += std::format(R"("wildcard":{{"base":{:.3f},"max":{:.3f}}},)", wcBase, wcMax);

            line += R"("phi":[)";
            const auto phi = event.features.ToArray();
            for (size_t i = 0; i < phi.size(); ++i) {
                line += std::format("{}{}", i ? "," : "", Num(phi[i]));
            }
            line += "],";

            line += R"("shown":[)";
            for (size_t i = 0; i < snap.shown.size(); ++i) {
                const auto& s = snap.shown[i];
                line += std::format(R"({}[{},"{:08X}","{}"])", i ? "," : "", s.slotIndex, s.formID,
                    Slot::AssignmentTypeToString(s.type));
            }
            line += "],";

            line += R"("cols":["form","type","need","rank","util","ctx","est","prior","ucb","alpha",)"
                    R"("learn","lambda","rec","corr","potion","fav","wildcard","coldStart","pred"],"cands":[)";
            for (size_t i = 0; i < snap.scores.size(); ++i) {
                const auto& r = snap.scores[i];
                const auto& b = r.breakdown;
                const long long rank = r.rank >= PipelineStateCache::kUnrankedTail ? -1 : static_cast<long long>(r.rank);
                line += std::format(R"({}["{:08X}","{}","{}",{},{},{},{},{},{},{},)",
                    i ? "," : "", r.formID, Candidate::SourceTypeToString(r.sourceType),
                    Slot::SlotClassificationToString(r.need), rank, Num(r.utility),
                    Num(b.contextWeight), Num(b.rewardEstimate), Num(b.prior), Num(b.ucb), Num(b.confidence));
                line += std::format("{},{},{},{},{},{},{},{},{}]",
                    Num(b.learningScore), Num(b.lambda), Num(b.recencyBoost), Num(b.correlationBonus),
                    Num(b.potionMultiplier), Num(b.favoritesMultiplier),
                    r.isWildcard ? 1 : 0, r.isColdStartBoosted ? 1 : 0,
                    Num(Predict(r.formID, event.features)));
            }
            line += "]}\n";

            std::lock_guard lock(s_fileMutex);
            std::ofstream out(*logDir / "Huginn_Selections.jsonl", std::ios::app | std::ios::binary);
            if (!out) {
                if (!s_warned) {
                    logger::error("[Selection] Cannot open Huginn_Selections.jsonl for append"sv);
                    s_warned = true;
                }
                return;
            }
            out << line;
        }
    }

    void SelectionLog::Write(const EquipEvent& event, const char* how)
    {
        const float reward = RewardFor(event.kind);
        WriteReadable(event, how, reward);
        WriteRecord(event, how, reward);
    }
}
