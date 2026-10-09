#pragma once

// Shared by the slot allocation host tests: fixture loading, result
// comparison, the boundary classifier and the synthetic snapshot generator.

#include "LegacySlotPolicy.h"
#include "core/SlotAllocCore.h"
#include "core/SlotSnapshotIO.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace Huginn::Test
{
    // SlotClassification::Regular (src/slot/SlotConfig.h): the last real class.
    inline constexpr std::uint8_t kRegular = 23;

    struct FixtureFile
    {
        std::string name;
        std::vector<SA::Snapshot> snaps;
    };

    /// Every tests/core/fixtures/slots/*.txt, sorted by name.
    inline std::vector<FixtureFile> LoadSlotFixtures(std::string* error)
    {
        std::vector<FixtureFile> out;
        const std::filesystem::path dir = std::filesystem::path(HUGINN_REPO_ROOT) / "tests/core/fixtures/slots";
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.path().extension() == ".txt") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            std::ifstream in(f, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            FixtureFile ff;
            ff.name = f.filename().string();
            std::string err;
            if (!SA::ReadSnapshots(ss.str(), ff.snaps, &err)) {
                if (error) *error = ff.name + ": " + err;
                return {};
            }
            out.push_back(std::move(ff));
        }
        return out;
    }

    inline std::string Describe(const SA::PageSlot& p)
    {
        if (p.kind == SA::Kind::Empty) return "-";
        static constexpr const char* kKinds[] = { "E", "N", "O", "W", "R" };
        return std::format("{}:{:x}/{:x}{}", kKinds[static_cast<int>(p.kind)], p.formID, p.key, p.seatMoved ? "*" : "");
    }

    inline std::string Describe(const std::vector<SA::PageSlot>& page)
    {
        std::string s;
        for (const auto& p : page) {
            if (!s.empty()) s += ' ';
            s += Describe(p);
        }
        return s;
    }

    /// Where a core result differs from a recorded one; empty when identical.
    inline std::string DiffResult(const SA::Snapshot& rec, const SA::Input& in, const SA::Output& out)
    {
        std::string why;
        const auto page = SA::PageOf(in, out);
        if (page != rec.page) {
            why += std::format("page\n    recorded {}\n    core     {}\n", Describe(rec.page), Describe(page));
        }
        if (out.memory.seats != rec.memoryAfter.seats) why += "seats ";
        if (out.memory.lastPlaced != rec.memoryAfter.lastPlaced) why += "lastPlaced ";
        if (out.memory.homeClaims != rec.memoryAfter.homeClaims) why += "homeClaims ";
        if (out.memory.departed != rec.memoryAfter.departed) why += "departed ";
        if (out.generationMatches != rec.generationAfter) why += "generation ";
        if (out.clearedAllPages != rec.clearedAllPages) why += "clearedAll ";
        if (SA::KeptOff(in, out) != rec.keptOff) why += "keptOff ";
        if (rec.hasEvents) {
            const auto events = SA::EventsOf(in, out);
            if (events != rec.events) {
                why += std::format("events ({} recorded, {} from the core", rec.events.size(), events.size());
                for (std::size_t i = 0; i < std::max(events.size(), rec.events.size()); ++i) {
                    const bool same = i < events.size() && i < rec.events.size() && events[i] == rec.events[i];
                    if (!same) {
                        auto show = [](const std::vector<SA::PageEvent>& v, std::size_t k) {
                            if (k >= v.size()) return std::string("-");
                            std::string s = v[k].code;
                            for (const auto x : v[k].v) s += std::format(" {:x}", x);
                            return s;
                        };
                        why += std::format("; first difference #{}: recorded {} vs core {}", i, show(rec.events, i), show(events, i));
                        break;
                    }
                }
                why += ") ";
            }
        }
        return why;
    }

    /// Where two core results differ; empty when identical.
    inline std::string DiffOutputs(const SA::Input& in, const SA::Output& a, const SA::Output& b)
    {
        SA::Snapshot rec;
        rec.in = in;
        SA::SetResult(rec, a);
        return DiffResult(rec, in, b);
    }

    /// One float ulp at |x| (the gap to the next float away from zero).
    inline double FloatUlp(float x)
    {
        const float ax = std::fabs(x);
        return static_cast<double>(std::nextafter(ax, std::numeric_limits<float>::infinity()) - ax);
    }

    /// A disagreement explained by the old arithmetic's float rounding: its two
    /// operands within a few ulps, or one of them underflowed (0 or denormal in
    /// float while the log side is a finite score).
    inline bool IsRoundingBoundary(const Disagreement& d)
    {
        const float m = std::max(std::fabs(d.oldA), std::fabs(d.oldB));
        if (std::fabs(static_cast<double>(d.oldA) - d.oldB) <= 4.0 * FloatUlp(m)) {
            return true;
        }
        const auto underflowed = [](float u, double s) {
            return std::fabs(u) < std::numeric_limits<float>::min() && s > -std::numeric_limits<double>::infinity();
        };
        if (d.op != Disagreement::Op::IsCapped && (underflowed(d.oldA, d.newA) || underflowed(d.oldB, d.newB))) {
            return true;
        }
        if (d.op == Disagreement::Op::IsCapped) {
            // The old factor underflowed to exactly 1? Never: d^k < 1 for d < 1.
            return false;
        }
        return false;
    }

    inline std::string Describe(const Disagreement& d)
    {
        static constexpr const char* kOps[] = { "IsCapped", "Greater", "RawGreater", "Exceeds" };
        return std::format("{}: old {} vs {} -> {}; new {} vs {} -> {}", kOps[static_cast<int>(d.op)], d.oldA, d.oldB,
            d.oldAnswer, d.newA, d.newB, !d.oldAnswer);
    }

    // =========================================================================
    // Coverage: what a set of runs actually exercised
    // =========================================================================
    struct Coverage
    {
        std::map<std::string, std::size_t> counts;

        void Add(const SA::Input& in, const SA::Output& out)
        {
            static constexpr const char* kEvents[] = { "override marked in place", "override placed", "override fallback",
                "override unplaced", "overrides inactive", "hold item not a candidate", "Remembrance hold shown",
                "slot with no candidate", "slot hold: holder gave way", "job key pulled from Regular", "returner" };
            for (const auto& e : out.events) {
                ++counts[kEvents[static_cast<int>(e.kind)]];
                if (e.kind == SA::EventKind::HoldGaveWay && e.capped) ++counts["slot hold: gave way under the cap"];
                if (e.kind == SA::EventKind::Returner) {
                    static constexpr const char* kWhy[] = { "home", "home keys off", "taken", "waits", "class", "owned" };
                    const bool home = e.b == e.c;
                    ++counts[std::string("returner: ") + (home ? "home" : kWhy[static_cast<int>(e.why)])];
                }
            }
            if (!out.skipped.empty()) ++counts["class cap passed an item over"];
            if (!SA::KeptOff(in, out).empty()) ++counts["class cap kept an item off"];
            if (out.clearedAllPages) ++counts["stale generation cleared"];
            for (const auto& s : out.slots) {
                if (s.seatMoved) ++counts["seatMoved"];
                if (s.kind == SA::Kind::Wildcard) ++counts["wildcard shown"];
                if (s.kind == SA::Kind::Remembered) ++counts["remembered shown"];
                if (s.kind == SA::Kind::Override) ++counts["override shown"];
            }
            if (in.candidates.size() > 10) ++counts["more than 10 candidates"];
            bool negativeScore = false;
            for (const auto& c : in.candidates) negativeScore = negativeScore || (c.score < 0.0 && c.score > -1e300);
            if (negativeScore) ++counts["a negative (finite) score"];
            ++counts["passes"];
        }

        [[nodiscard]] std::string Report() const
        {
            std::string s;
            for (const auto& [k, v] : counts) s += std::format("\n    {:<36} {}", k, v);
            return s;
        }
    };

    // =========================================================================
    // Synthetic snapshots
    // =========================================================================
    struct SyntheticSpec
    {
        bool exoticSettings = false;   // vary the cap, the margin, the switches
        int formPool = 24;             // distinct formIDs (so passes share items)
        int maxCandidates = 40;
    };

    class SyntheticGenerator
    {
    public:
        explicit SyntheticGenerator(std::uint32_t seed, SyntheticSpec spec = {}) : m_rng(seed), m_spec(spec) {}

        [[nodiscard]] SA::Settings RandomSettings()
        {
            SA::Settings s;   // the shipped defaults: d 0.5, 3 free, margin 0.5, 60 s home keys
            if (!m_spec.exoticSettings) {
                s.fillJobKeysFromRegular = Chance(30);
                s.remembranceToJob = Chance(30);
                return s;
            }
            static constexpr float kDiscounts[] = { 0.0f, 0.25f, 0.3f, 0.5f, 0.6f, 0.75f, 0.9f, 1.0f, -0.0f };
            static constexpr float kMargins[] = { 0.0f, 0.1f, 0.25f, 0.5f, 0.33f, 1.0f, 2.0f };
            static constexpr float kHome[] = { 0.0f, 0.5f, 5.0f, 60.0f };
            s.classDiscount = Pick(kDiscounts);
            s.classFree = static_cast<std::uint32_t>(Int(0, 5));
            s.challengerMargin = Pick(kMargins);
            s.homeKeyMemorySec = Pick(kHome);
            s.keepSlotPositions = Chance(85);
            s.holdSeatedItems = Chance(85);
            s.fillJobKeysFromRegular = Chance(40);
            s.returnToHomeKey = Chance(80);
            s.remembranceToJob = Chance(40);
            return s;
        }

        [[nodiscard]] std::vector<SA::SlotRec> RandomLayout()
        {
            std::vector<SA::SlotRec> slots(static_cast<std::size_t>(Int(1, 10)));
            for (auto& s : slots) {
                s.classIndex = Chance(55) ? kRegular : static_cast<std::uint8_t>(Int(0, 7));
                s.regular = s.classIndex == kRegular;
                s.wildcardsEnabled = Chance(80);
                s.skipEquipped = Chance(30);
                s.remembrance = Chance(85);
                s.priority = static_cast<std::int8_t>(Int(-1, 3));
                for (int c = 0; c < 4; ++c) {
                    if (Chance(30)) s.overrideAccept |= static_cast<std::uint8_t>(1u << c);
                }
            }
            return slots;
        }

        [[nodiscard]] float RandomUtility()
        {
            static constexpr float kRound[] = { 0.1f, 0.2f, 0.25f, 0.4f, 0.5f, 0.75f, 0.8f, 1.0f, 1.5f, 2.0f, 2.25f, 3.0f };
            switch (Int(0, 9)) {
            case 0: case 1: case 2: return Pick(kRound);                     // exact ties, 2x, 1.5x
            case 3: return 0.0f;                                              // unranked-looking
            case 4: return Chance(50) ? 1e-30f : 1e-40f;                     // dust, a denormal
            case 5: return static_cast<float>(Int(1, 5000)) * 1e-3f;         // a grid: more ties
            default: return std::uniform_real_distribution<float>(0.05f, 3.0f)(m_rng);
            }
        }

        [[nodiscard]] SA::CandidateRec RandomCandidate()
        {
            SA::CandidateRec c;
            c.formID = 0x0A000000u + static_cast<std::uint32_t>(Int(1, m_spec.formPool));
            c.uniqueID = Chance(15) ? static_cast<std::uint16_t>(Int(1, 3)) : 0;
            c.dedupKey = (static_cast<std::uint64_t>(c.uniqueID) << 32) | c.formID;
            // Some names shared by different forms (enchanted twins): name dedup.
            c.name = Chance(10) ? "Twin" : std::format("Item{:x}", c.formID);
            c.utility = RandomUtility();
            c.score = Core::BridgeScore(c.utility);
            c.tieBreak = Chance(20) ? static_cast<float>(Int(1, 4)) : 0.0f;
            c.isWildcard = Chance(5);
            c.isEquipped = Chance(8);
            c.playerEquipped = Chance(8);
            c.matchMask = MaskFor(c.formID);
            c.capClass = static_cast<std::uint8_t>(c.formID % 5);
            return c;
        }

        /// One pass's input, continuing from `memory`.
        [[nodiscard]] SA::Input Next(const SA::Settings& settings, const std::vector<SA::SlotRec>& layout,
            const SA::PageMemory& memory, bool generationMatches)
        {
            SA::Input in;
            in.pageIndex = static_cast<std::uint32_t>(Int(0, 9));
            m_now += static_cast<std::int64_t>(Int(0, 30)) * 100'000'000LL;   // 0-3 s
            if (Chance(5)) m_now += 90'000'000'000LL;                         // past the home-key memory
            in.nowNs = m_now;
            in.memoryAvailable = !Chance(2);
            in.generationMatches = generationMatches && !Chance(3);
            in.settings = settings;
            in.slots = layout;
            in.memory = memory;

            const int n = Int(0, m_spec.maxCandidates);
            for (int i = 0; i < n; ++i) {
                in.candidates.push_back(RandomCandidate());
            }
            // The scorer's order: sorted (ties in arrival order), only a prefix
            // sorted, or none at all.
            const int order = Int(0, 9);
            auto byUtility = [](const SA::CandidateRec& a, const SA::CandidateRec& b) {
                if (a.utility != b.utility) return a.utility > b.utility;
                return a.tieBreak > b.tieBreak;
            };
            if (order < 6) {
                std::stable_sort(in.candidates.begin(), in.candidates.end(), byUtility);
            } else if (order < 9 && in.candidates.size() > 10) {
                std::partial_sort(in.candidates.begin(), in.candidates.begin() + 10, in.candidates.end(), byUtility);
                std::shuffle(in.candidates.begin() + 10, in.candidates.end(), m_rng);
            }
            // Remembered-only rows at the end.
            if (Chance(25)) {
                auto r = RandomCandidate();
                r.utility = 0.0f;
                r.isRememberedOnly = true;
                r.isWildcard = false;
                r.score = Core::kUnrankedScore;
                in.candidates.push_back(std::move(r));
            }

            in.overridesActive = Chance(25);
            if (in.overridesActive) {
                const int k = Int(0, 3);
                for (int i = 0; i < k; ++i) {
                    SA::OverrideRec o;
                    o.formID = 0x0A000000u + static_cast<std::uint32_t>(Int(1, m_spec.formPool));
                    o.dedupKey = o.formID;
                    o.name = std::format("Item{:x}", o.formID);
                    o.condition = static_cast<std::uint8_t>(Int(1, 6));
                    o.category = static_cast<std::uint8_t>(Int(0, 3));
                    o.pinnedToSlot = Chance(30);
                    o.isEquipped = Chance(5);
                    o.matchMask = MaskFor(o.formID);
                    o.capClass = static_cast<std::uint8_t>(o.formID % 5);
                    in.overrides.push_back(std::move(o));
                }
            }
            for (std::size_t j = 0; j < SA::kMaxSlots; ++j) {
                if (Chance(15)) {
                    in.holds[j] = { true, 0x0A000000u + static_cast<std::uint32_t>(Int(1, m_spec.formPool)) };
                }
            }
            return in;
        }

        bool Chance(int percent) { return Int(0, 99) < percent; }
        int Int(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(m_rng); }
        template <class T, std::size_t N>
        T Pick(const T (&a)[N]) { return a[static_cast<std::size_t>(Int(0, static_cast<int>(N) - 1))]; }

    private:
        // A form's classes are a property of the form: the same in every pass.
        std::uint32_t MaskFor(std::uint32_t formID)
        {
            std::uint32_t h = formID * 2654435761u;
            std::uint32_t mask = 1u << kRegular;
            for (int k = 0; k < 8; ++k) {
                if (((h >> (k * 3)) & 7u) < 2u) mask |= 1u << k;
            }
            return mask;
        }

        std::mt19937 m_rng;
        SyntheticSpec m_spec;
        std::int64_t m_now = 1'000'000'000'000LL;
    };
}  // namespace Huginn::Test
