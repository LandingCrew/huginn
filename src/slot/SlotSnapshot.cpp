#include "SlotSnapshot.h"
#include "Remembrance.h"
#include "SlotClassCap.h"
#include "SlotClassifier.h"
#include "SlotSettings.h"
#include "override/OverrideConfig.h"
#include <atomic>
#include <fstream>
#include <mutex>

namespace Huginn::Slot
{
    namespace SA = Core::SlotAlloc;

    static_assert(SA::kMaxSlots == MAX_SLOTS_PER_PAGE);
    static_assert(SLOT_CLASSIFICATION_COUNT <= 32, "the core's class mask is 32 bits");
    static_assert(SLOT_CLASSIFICATION_COUNT <= SA::kMaxClasses);
    static_assert(static_cast<uint8_t>(AssignmentType::Empty) == static_cast<uint8_t>(SA::Kind::Empty) &&
                  static_cast<uint8_t>(AssignmentType::Normal) == static_cast<uint8_t>(SA::Kind::Normal) &&
                  static_cast<uint8_t>(AssignmentType::Override) == static_cast<uint8_t>(SA::Kind::Override) &&
                  static_cast<uint8_t>(AssignmentType::Wildcard) == static_cast<uint8_t>(SA::Kind::Wildcard) &&
                  static_cast<uint8_t>(AssignmentType::Remembered) == static_cast<uint8_t>(SA::Kind::Remembered),
        "Core::SlotAlloc::Kind mirrors AssignmentType");
    static_assert(std::is_same_v<std::chrono::steady_clock::duration, std::chrono::nanoseconds>,
        "the core counts steady_clock in nanoseconds; awaySec must come out bit-identical");

    Core::SlotAlloc::Settings ReadAllocSettings()
    {
        const auto& s = SlotSettings::GetSingleton();
        SA::Settings out;
        out.keepSlotPositions = s.KeepSlotPositions();
        out.holdSeatedItems = s.HoldSeatedItems();
        out.challengerMargin = s.ChallengerMargin();
        out.fillJobKeysFromRegular = s.FillJobKeysFromRegular();
        out.classDiscount = s.ClassRepeatDiscount();
        out.classFree = s.ClassFreeSlots();
        out.homeKeyMemorySec = s.HomeKeyMemorySec();
        out.returnToHomeKey = s.ReturnToHomeKey();
        out.remembranceToJob = s.RemembranceToJobKey();
        return out;
    }

    Core::SlotAlloc::Input BuildAllocInput(
        size_t pageIndex,
        std::chrono::steady_clock::time_point now,
        const std::vector<SlotConfig>& slotConfigs,
        const Scoring::ScoredCandidateList& candidates,
        const Override::OverrideCollection& overrides,
        const State::PlayerActorState* player,
        const Core::SlotAlloc::Settings& settings,
        std::vector<size_t>* overrideIndex)
    {
        SA::Input in;
        in.pageIndex = static_cast<uint32_t>(pageIndex);
        in.nowNs = ToCoreNs(now);
        in.settings = settings;

        // The layout, and which classes a candidate must be tested against:
        // only those a slot of this page has (Matches is asked of nothing else).
        uint32_t classesUsed = 0;
        bool anyRegular = false;
        in.slots.reserve(slotConfigs.size());
        for (const auto& cfg : slotConfigs) {
            SA::SlotRec r;
            r.classIndex = static_cast<uint8_t>(cfg.classification);
            r.regular = cfg.classification == SlotClassification::Regular;
            r.wildcardsEnabled = cfg.wildcardsEnabled;
            r.skipEquipped = cfg.skipEquipped;
            r.remembrance = cfg.remembrance;
            r.priority = cfg.priority;
            for (uint8_t cat = 0; cat <= static_cast<uint8_t>(Override::OverrideCategory::Other); ++cat) {
                if (AcceptsOverride(cfg.overrideFilter, static_cast<Override::OverrideCategory>(cat))) {
                    r.overrideAccept |= static_cast<uint8_t>(1u << cat);
                }
            }
            classesUsed |= 1u << r.classIndex;
            anyRegular = anyRegular || r.regular;
            in.slots.push_back(r);
        }
        // The class of every candidate only matters on a Regular key under an
        // active cap (SlotClassCap classified lazily; the same answer).
        const bool needCapClass = anyRegular && Core::ClassCapActive(settings.classDiscount);

        auto maskOf = [classesUsed](const auto& candidate) {
            uint32_t mask = 0;
            for (uint32_t k = 0; k < SLOT_CLASSIFICATION_COUNT; ++k) {
                if ((classesUsed >> k) & 1u) {
                    if (SlotClassifier::Matches(candidate, static_cast<SlotClassification>(k))) {
                        mask |= 1u << k;
                    }
                }
            }
            return mask;
        };

        in.candidates.reserve(candidates.size());
        for (const auto& sc : candidates) {
            const auto& base = Candidate::GetBase(sc.candidate);
            SA::CandidateRec c;
            c.formID = sc.GetFormID();
            c.uniqueID = sc.GetUniqueID();
            c.dedupKey = base.GetDeduplicationKey();
            c.name = std::string(sc.GetName());
            c.utility = sc.utility;
            c.score = sc.SlotScore();
            c.tieBreak = sc.TieBreakDps();
            c.isWildcard = sc.isWildcard;
            c.isRememberedOnly = sc.isRememberedOnly;
            c.isEquipped = base.isEquipped;
            c.playerEquipped = player && player->IsItemEquipped(base.formID);
            c.matchMask = maskOf(sc);
            c.capClass = needCapClass ? static_cast<uint8_t>(SlotClassCap::ClassOf(sc)) : 0;
            in.candidates.push_back(std::move(c));
        }

        using OC = Override::OverrideCondition;
        in.overridesActive = overrides.HasActiveOverride();
        for (size_t k = 0; k < overrides.activeOverrides.size(); ++k) {
            const auto& ov = overrides.activeOverrides[k];
            if (!ov.candidate) {
                continue;
            }
            const auto& base = Candidate::GetBase(*ov.candidate);
            SA::OverrideRec o;
            o.formID = Candidate::GetFormID(*ov.candidate);
            o.uniqueID = Candidate::GetUniqueID(*ov.candidate);
            o.dedupKey = base.GetDeduplicationKey();
            o.name = std::string(Candidate::GetName(*ov.candidate));
            o.condition = static_cast<uint8_t>(ov.condition);
            o.category = static_cast<uint8_t>(ov.category);
            o.pinnedToSlot =
                (ov.condition == OC::CriticalHealth && Override::Config::PIN_HEALTH_TO_SLOT()) ||
                (ov.condition == OC::CriticalMagicka && Override::Config::PIN_MAGICKA_TO_SLOT()) ||
                (ov.condition == OC::CriticalStamina && Override::Config::PIN_STAMINA_TO_SLOT());
            o.isEquipped = base.isEquipped;
            o.playerEquipped = player && player->IsItemEquipped(base.formID);
            o.matchMask = maskOf(*ov.candidate);
            if (needCapClass) {
                Scoring::ScoredCandidate sc;
                sc.candidate = *ov.candidate;
                o.capClass = static_cast<uint8_t>(SlotClassCap::ClassOf(sc));
            }
            in.overrides.push_back(std::move(o));
            if (overrideIndex) {
                overrideIndex->push_back(k);
            }
        }

        const auto held = Remembrance::GetSingleton().GetPage(pageIndex);
        for (size_t j = 0; j < MAX_SLOTS_PER_PAGE; ++j) {
            in.holds[j] = { held[j].Active(), held[j].formID };
        }
        return in;
    }

    std::vector<Core::SlotAlloc::PageSlot> PageOf(const SlotAssignments& assignments)
    {
        std::vector<SA::PageSlot> page;
        page.reserve(assignments.size());
        for (const auto& a : assignments) {
            SA::PageSlot p;
            p.kind = static_cast<SA::Kind>(a.type);
            p.seatMoved = a.seatMoved;
            if (!a.IsEmpty() && a.candidate) {
                p.formID = a.formID;
                p.uniqueID = a.uniqueID;
                p.key = Candidate::GetBase(a.candidate->candidate).GetDeduplicationKey();
                p.name = a.name;
            }
            page.push_back(std::move(p));
        }
        return page;
    }

    // =========================================================================
    // CAPTURE (Debug)
    // =========================================================================
    namespace Capture
    {
#ifndef NDEBUG
        namespace
        {
            std::atomic<bool> g_enabled{ false };
            std::atomic<size_t> g_count{ 0 };
            std::mutex g_fileMutex;
            std::ofstream g_file;
            SA::SnapshotDictionary g_dict;   // the file's ITEM table
        }

        bool Enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

        void SetEnabled(bool on)
        {
            std::lock_guard lock(g_fileMutex);
            if (on == g_enabled.load()) {
                return;
            }
            if (on) {
                auto dir = SKSE::log::log_directory();
                if (!dir) {
                    SKSE::log::error("[SlotCapture] no SKSE log folder; capture stays off");
                    return;
                }
                const auto path = *dir / "Huginn_SlotSnapshots.txt";
                g_file.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
                if (!g_file) {
                    SKSE::log::error("[SlotCapture] cannot write {}", path.string());
                    return;
                }
                g_file << "# Huginn slot snapshots (src/core/SlotSnapshotIO.h), version " << Plugin::VERSION.string()
                       << "\n";
                g_count = 0;
                g_dict = {};
                g_enabled = true;
                SKSE::log::info("[SlotCapture] capturing every allocation to {}", path.string());
            } else {
                g_enabled = false;
                g_file.flush();
                g_file.close();
                SKSE::log::info("[SlotCapture] stopped after {} snapshot(s)", g_count.load());
            }
        }

        void Write(const Core::SlotAlloc::Snapshot& snap)
        {
            if (!Enabled()) {
                return;
            }
            std::lock_guard lock(g_fileMutex);
            if (!g_enabled.load() || !g_file) {
                return;
            }
            g_file << SA::WriteSnapshot(snap, &g_dict);
            g_file.flush();   // the test run ends with TerminateProcess
            ++g_count;
        }

        size_t Count() noexcept { return g_count.load(); }

        namespace
        {
            thread_local const char* t_tag = nullptr;
        }
        void SetThreadTag(const char* tag) noexcept { t_tag = tag; }
        const char* ThreadTag() noexcept { return t_tag ? t_tag : "tick"; }
#else
        bool Enabled() noexcept { return false; }
        void SetEnabled(bool) {}
        void Write(const Core::SlotAlloc::Snapshot&) {}
        size_t Count() noexcept { return 0; }
        void SetThreadTag(const char*) noexcept {}
        const char* ThreadTag() noexcept { return "tick"; }
        void NoteRealList(size_t, const Scoring::ScoredCandidateList&, const std::vector<SlotConfig>&) {}
        void StartSession(int, void (*done)()) { if (done) done(); }
#endif
    }  // namespace Capture
}  // namespace Huginn::Slot
