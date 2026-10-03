#include "SelectionLog.h"
#include "FeatureBanditLearner.h"
#include "ShadowArm.h"
#include "UtilityScorer.h"
#include "Globals.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

        // ── Predictions, under ONE learner lock ─────────────────────────────
        // The learner's press-time estimate for the chosen item, every shown
        // slot and every scored candidate. Read before the dispatch updates the
        // learner, so these are the values the selection is about to correct.
        // One shared lock for the lot (LockedReader) rather than one per item:
        // a few hundred 18-float dot products is microseconds; a few hundred
        // lock round-trips on the game thread was the cost (#163 review).
        struct Predictions
        {
            float chosen = 0.0f;
            std::vector<float> shown;    // Parallel to snapshot.shown
            std::vector<float> scores;   // Parallel to snapshot.scores
        };

        Predictions Predict(const EquipEvent& event)
        {
            Predictions p;
            p.shown.assign(event.shown.shown.size(), 0.0f);
            p.scores.assign(event.shown.scores.size(), 0.0f);
            if (!g_featureBanditLearner) return p;

            const auto phi = event.features.ToArray();
            const auto reader = g_featureBanditLearner->AcquireReader();
            p.chosen = reader.GetMetrics(event.formID, phi).rewardEstimate;
            for (size_t i = 0; i < event.shown.shown.size(); ++i) {
                p.shown[i] = reader.GetMetrics(event.shown.shown[i].formID, phi).rewardEstimate;
            }
            for (size_t i = 0; i < event.shown.scores.size(); ++i) {
                p.scores[i] = reader.GetMetrics(event.shown.scores[i].formID, phi).rewardEstimate;
            }
            return p;
        }

        // ── 1. The debug log ─────────────────────────────────────────────────
        void WriteReadable(const EquipEvent& event, const char* how, float reward, const Predictions& pred)
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
                scorePart, pred.chosen,
                chosen ? Slot::SlotClassificationToString(chosen->need) : "-"sv,
                overPart, snap.shown.size(), snap.page, snap.ageMs, event.loadGeneration);

            for (size_t i = 0; i < snap.shown.size(); ++i) {
                const auto& s = snap.shown[i];
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
                    pred.shown[i]);
            }
        }

        // ── 2. Huginn_Selections.jsonl ───────────────────────────────────────
        // Everything the record needs, captured on the game thread: the event
        // (its snapshot already a copy), the name (a form lookup, so not done
        // off-thread), the predictions, the wildcard odds and the time.
        struct Record
        {
            EquipEvent event;
            std::string name;
            std::string how;
            float reward = 0.0f;
            std::chrono::sys_time<std::chrono::milliseconds> utc{};
            float wildcardBase = 0.0f;
            float wildcardMax = 0.0f;
            std::vector<float> predictions;   // Parallel to event.shown.scores
        };

        // Pure formatting -- runs on the writer thread.
        std::string FormatRecord(const Record& r)
        {
            const auto& event = r.event;
            const auto& snap = event.shown;
            std::string line;
            line.reserve(256 + snap.scores.size() * 120);

            // v2 (0.22.10): "launch" and "list" say which game launch and which
            // modlist the record came from -- one file collects every launch of
            // every instance.
            line += std::format(R"({{"v":2,"utc":"{:%F %T}","launch":"{}","list":{},"gen":{},"form":"{:08X}","name":{},"src":"{}","via":{},)",
                r.utc, g_launchStamp, JsonString(g_listName), event.loadGeneration, event.formID, JsonString(r.name),
                EquipSourceToString(event.source), JsonString(event.via));
            line += std::format(R"("case":{},"kind":"{}","reward":{:.2f},"how":"{}","confirmMs":{:.0f},)",
                JsonString(event.attribution), SelectionKindToString(event.kind), r.reward, r.how, event.confirmMs);
            line += std::format(R"("pipeline":{},"page":{},"ageMs":{:.0f},"sortedPrefix":{},)",
                snap.valid, snap.page, snap.ageMs, snap.sortedPrefix);
            line += std::format(R"("wildcard":{{"base":{:.3f},"max":{:.3f}}},)", r.wildcardBase, r.wildcardMax);

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
                const auto& row = snap.scores[i];
                const auto& b = row.breakdown;
                const long long rank = row.rank >= PipelineStateCache::kUnrankedTail ? -1 : static_cast<long long>(row.rank);
                line += std::format(R"({}["{:08X}","{}","{}",{},{},{},{},{},{},{},)",
                    i ? "," : "", row.formID, Candidate::SourceTypeToString(row.sourceType),
                    Slot::SlotClassificationToString(row.need), rank, Num(row.utility),
                    Num(b.contextWeight), Num(b.rewardEstimate), Num(b.prior), Num(b.ucb), Num(b.confidence));
                line += std::format("{},{},{},{},{},{},{},{},{}]",
                    Num(b.learningScore), Num(b.lambda), Num(b.recencyBoost), Num(b.correlationBonus),
                    Num(b.potionMultiplier), Num(b.favoritesMultiplier),
                    row.isWildcard ? 1 : 0, row.isColdStartBoosted ? 1 : 0,
                    Num(i < r.predictions.size() ? r.predictions[i] : 0.0f));
            }
            line += "]}\n";
            return line;
        }

        // Formats and appends records on its own thread, so a confirmation costs
        // the game thread a copy and a queue push -- not a ~13 KB string build
        // and a file open/append/close (#163 review; a 50-hour soak run does a
        // few of these a minute, in combat).
        //
        // The stream stays open and is flushed after every record. What a crash
        // can lose: records still queued (normally none, at most a few), and the
        // record of the selection whose dispatch crashed -- it is queued just
        // before the learner update runs. The trade for keeping the write off
        // the game thread. A failed write closes the stream so the next record
        // reopens it, rather than leaving it failed for the rest of the run
        // (#164 review). The writer is never
        // destroyed: a thread joined from a static destructor runs inside
        // DLL_PROCESS_DETACH at exit, where joining can deadlock, so it is
        // leaked and its thread detached -- process exit ends both.
        class RecordWriter
        {
        public:
            static RecordWriter& Get()
            {
                static auto* writer = new RecordWriter();
                return *writer;
            }

            void Enqueue(Record record)
            {
                {
                    std::lock_guard lock(m_mutex);
                    // Bounded: if the writer has stalled (disk gone, AV scan)
                    // drop rather than grow without limit on the game thread.
                    if (m_queue.size() >= kMaxQueued) {
                        if (!m_warnedFull) {
                            logger::warn("[Selection] JSONL writer is {} records behind -- dropping records"sv,
                                kMaxQueued);
                            m_warnedFull = true;
                        }
                        return;
                    }
                    m_queue.push_back(std::move(record));
                }
                m_cv.notify_one();
            }

        private:
            static constexpr size_t kMaxQueued = 64;

            RecordWriter()
            {
                std::thread([this] { Run(); }).detach();
            }

            void Run()
            {
                for (;;) {
                    Record record;
                    {
                        std::unique_lock lock(m_mutex);
                        m_cv.wait(lock, [this] { return !m_queue.empty(); });
                        record = std::move(m_queue.front());
                        m_queue.pop_front();
                    }
                    Append(FormatRecord(record));
                }
            }

            void Append(const std::string& line)
            {
                if (!m_out.is_open()) {
                    const auto logDir = SKSE::log::log_directory();
                    if (logDir) {
                        m_out.open(*logDir / "Huginn_Selections.jsonl", std::ios::app | std::ios::binary);
                    }
                    if (!m_out.is_open()) {
                        if (!m_warnedOpen) {
                            logger::error("[Selection] Cannot open Huginn_Selections.jsonl for append"sv);
                            m_warnedOpen = true;
                        }
                        return;
                    }
                }
                m_out << line;
                m_out.flush();
                if (!m_out) {
                    // Disk full, a file locked by a backup or AV scan... Do not
                    // stay failed: close, and the next record reopens the file.
                    if (!m_warnedWrite) {
                        logger::error("[Selection] Write to Huginn_Selections.jsonl failed -- reopening on the "
                                      "next record (one record lost)"sv);
                        m_warnedWrite = true;
                    }
                    m_out.close();
                    m_out.clear();
                }
            }

            std::mutex m_mutex;
            std::condition_variable m_cv;
            std::deque<Record> m_queue;
            bool m_warnedFull = false;

            // Writer thread only.
            std::ofstream m_out;
            bool m_warnedOpen = false;
            bool m_warnedWrite = false;
        };
    }

    void SelectionLog::Write(const EquipEvent& event, const char* how)
    {
        const float reward = RewardFor(event.kind);
        Predictions pred = Predict(event);
        WriteReadable(event, how, reward, pred);
        ShadowArm::Record(event);   // THROWAWAY: soak-run A|B arm, Debug only

        Record record;
        record.event = event;
        record.name = FormName(event.formID);
        record.how = how;
        record.reward = reward;
        record.utc = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
        if (g_utilityScorer) {
            const auto& wc = g_utilityScorer->GetWildcardManager();
            record.wildcardBase = wc.GetBaseProbability();
            record.wildcardMax = wc.GetMaxProbability();
        }
        record.predictions = std::move(pred.scores);
        RecordWriter::Get().Enqueue(std::move(record));
    }
}
