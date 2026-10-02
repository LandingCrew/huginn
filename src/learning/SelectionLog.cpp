#include "SelectionLog.h"
#include "FeatureBanditLearner.h"
#include "UtilityScorer.h"
#include "Globals.h"

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

        std::string JsonString(std::string_view text)
        {
            std::string out;
            out.reserve(text.size() + 2);
            out += '"';
            for (const char c : text) {
                switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                    } else {
                        out += c;
                    }
                }
            }
            out += '"';
            return out;
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
                logger::info("[Selection]   s{} {} {:08X} '{}'{} need={} rank={} util={:.2f} ctx={:.2f} pred={:.2f}"sv,
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
                line += std::format("{}{:.4g}", i ? "," : "", phi[i]);
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
                line += std::format(R"({}["{:08X}","{}","{}",{},{:.4g},{:.4g},{:.4g},{:.4g},{:.4g},{:.4g},)",
                    i ? "," : "", r.formID, Candidate::SourceTypeToString(r.sourceType),
                    Slot::SlotClassificationToString(r.need), rank, r.utility,
                    b.contextWeight, b.rewardEstimate, b.prior, b.ucb, b.confidence);
                line += std::format("{:.4g},{:.4g},{:.4g},{:.4g},{:.4g},{:.4g},{},{},{:.4g}]",
                    b.learningScore, b.lambda, b.recencyBoost, b.correlationBonus,
                    b.potionMultiplier, b.favoritesMultiplier,
                    r.isWildcard ? 1 : 0, r.isColdStartBoosted ? 1 : 0,
                    Predict(r.formID, event.features));
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
