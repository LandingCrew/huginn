#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/SlotClassCapMath.h"
#include "core/SlotScoreMath.h"

// =============================================================================
// SLOT ALLOCATION CORE (pure; host-tested in tests/core/SlotAlloc*Tests.cpp)
// =============================================================================
// The decision logic of Slot::SlotAllocator (src/slot/SlotAllocator.cpp) over
// plain records: overrides, Remembrance holds, the slot hold, the rank-ordered
// fill, the class cap, seating and home keys, filling job keys from Regular
// keys. SlotAllocator builds an Input from the game's types (SlotSnapshot.h),
// runs this, and turns the Output back into SlotAssignments; logging,
// telemetry and the Remembrance side effects stay on the game side, fed by the
// Output's events.
//
// Moved here in R7 (engine rewrite, map Phase 5) so the slot code could be made
// sign-safe and PROVEN unchanged on the host: the algorithm is a template over
// a score policy. The game runs LogScorePolicy (scores of any sign, additive
// class cap, the hold as a difference against ln m). The old multiplicative
// arithmetic is a second policy in the tests (tests/core/LegacySlotPolicy.h),
// checked against outputs the old game code recorded in play
// (tests/core/fixtures/slots/), and the two policies must give identical pages
// on every recorded and synthetic snapshot (SlotAllocGoldenTests.cpp).
//
// The structure is a line-by-line port of SlotAllocator 0.23.9: the same
// passes in the same order, the same std::sort/std::stable_sort calls on the
// same inputs (their tie orders are part of the behaviour), the same dedup
// sets. Comments that explain WHY a rule exists stay in SlotAllocator.cpp and
// docs/architecture/5-slots.md; the ones here say what maps to what.
//
// A Policy provides:
//   using Score, Cap;
//   bool  CapActive() const;            the class cap is on
//   Score Raw(const CandidateRec&);     the candidate's own score
//   Cap   NoCap(); Cap CapFor(uint32_t shownOfItsClass);
//   bool  IsCapped(Cap);                the cap lowers the score
//   Score Apply(Score, Cap);
//   bool  Greater(Score a, Score b);    a capped score beats another (the fill's scan)
//   bool  RawGreater(Score a, Score b); strict order on raw scores (sorts, best stack)
//   bool  Exceeds(Score c, Score h);    the slot hold: c beats h by the margin
//   double ToDouble(Score), ToDouble(Cap);   for the events (logs)
// =============================================================================

namespace Huginn::Core::SlotAlloc
{
    inline constexpr std::size_t kMaxSlots = 10;      // Slot::MAX_SLOTS_PER_PAGE (static_assert in SlotSnapshot.cpp)
    inline constexpr std::size_t kHomeMemory = 3;     // SlotAllocator::HOME_MEMORY_PER_SLOT
    inline constexpr std::size_t kMaxClasses = 32;    // >= SLOT_CLASSIFICATION_COUNT (static_assert)
    inline constexpr std::size_t npos = static_cast<std::size_t>(-1);
    inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;

    /// Slot::AssignmentType, same order (static_assert in SlotSnapshot.cpp).
    enum class Kind : std::uint8_t
    {
        Empty,
        Normal,
        Override,
        Wildcard,
        Remembered,
    };

    struct SlotRec
    {
        std::uint8_t classIndex = 0;      // Slot::SlotClassification
        bool regular = true;              // classIndex == Regular
        bool wildcardsEnabled = true;
        bool skipEquipped = true;
        bool remembrance = true;
        std::int8_t priority = 0;
        std::uint8_t overrideAccept = 0;  // bit c: AcceptsOverride(filter, category c)
    };

    struct CandidateRec
    {
        std::uint32_t formID = 0;
        std::uint16_t uniqueID = 0;
        std::uint64_t dedupKey = 0;
        std::string name;
        float utility = 0.0f;             // the old engine's value (the legacy policy, logs)
        SlotScore score = 0.0;            // what the slot code ranks on
        float tieBreak = 0.0f;            // ScoredCandidate::TieBreakDps (the scorer's sort only)
        bool isWildcard = false;
        bool isRememberedOnly = false;
        bool isEquipped = false;          // CandidateBase::isEquipped
        bool playerEquipped = false;      // player->IsItemEquipped(formID)
        std::uint32_t matchMask = 0;      // bit k: SlotClassifier::Matches(c, k), for the layout's classes
        std::uint8_t capClass = 0;        // SlotClassCap::ClassOf(c)
    };

    struct OverrideRec
    {
        std::uint32_t formID = 0;
        std::uint16_t uniqueID = 0;
        std::uint64_t dedupKey = 0;
        std::string name;
        std::uint8_t condition = 0;       // Override::OverrideCondition (events only)
        std::uint8_t category = 0;        // Override::OverrideCategory
        bool pinnedToSlot = false;        // a vital pinned to its configured slot ([Overrides] bPin*ToSlot)
        bool isEquipped = false;
        bool playerEquipped = false;
        std::uint32_t matchMask = 0;
        std::uint8_t capClass = 0;
    };

    /// One Remembrance hold (Remembrance::Entry), per slot of the page.
    struct HoldRec
    {
        bool active = false;
        std::uint32_t formID = 0;
    };

    struct Settings
    {
        bool keepSlotPositions = true;
        bool holdSeatedItems = true;
        float challengerMargin = 0.5f;
        bool fillJobKeysFromRegular = false;
        float classDiscount = 0.5f;
        std::uint32_t classFree = 3;
        float homeKeyMemorySec = 60.0f;
        bool returnToHomeKey = true;
        bool remembranceToJob = false;
    };

    struct Departure
    {
        std::uint64_t key = 0;
        std::int64_t leftAtNs = 0;        // steady_clock, nanoseconds since its epoch; 0 = never
        friend bool operator==(const Departure&, const Departure&) = default;
    };

    /// One page's seating memory (SlotAllocator's m_seating, m_lastPlaced,
    /// m_departed, m_homeClaims rows).
    struct PageMemory
    {
        std::array<std::uint64_t, kMaxSlots> seats{};
        std::array<std::uint64_t, kMaxSlots> lastPlaced{};
        std::array<std::array<Departure, kHomeMemory>, kMaxSlots> departed{};
        std::array<std::uint64_t, kMaxSlots> homeClaims{};
        friend bool operator==(const PageMemory&, const PageMemory&) = default;
    };

    struct Input
    {
        std::uint32_t pageIndex = 0;
        std::int64_t nowNs = 0;
        bool memoryAvailable = true;      // pageIndex < MAX_PAGES
        bool generationMatches = true;    // the seating memory belongs to this layout generation
        Settings settings;
        std::vector<SlotRec> slots;
        std::vector<CandidateRec> candidates;   // in the scorer's order
        bool overridesActive = false;           // OverrideCollection::HasActiveOverride()
        std::vector<OverrideRec> overrides;     // active overrides that carry a candidate, in order
        std::array<HoldRec, kMaxSlots> holds{};
        PageMemory memory;
    };

    struct SlotState
    {
        Kind kind = Kind::Empty;
        std::uint32_t src = kNone;        // candidate index, or override index for Kind::Override
        bool seatMoved = false;
        friend bool operator==(const SlotState&, const SlotState&) = default;

        [[nodiscard]] bool IsEmpty() const noexcept { return kind == Kind::Empty; }
        [[nodiscard]] bool IsPinned() const noexcept { return kind == Kind::Override || kind == Kind::Remembered; }
    };

    enum class EventKind : std::uint8_t
    {
        OverrideMarked,           // a: override, b: slot (marks the slot already showing it)
        OverridePlaced,           // a: override, b: slot
        OverrideFallback,         // a: override, b: slot
        OverrideUnplaced,         // a: override, b: 1 = the page has an accepting slot (all busy)
        OverridesInactive,        // no override is active (the log dedup resets)
        HoldNotCandidate,         // a: slot pressed, b: formID -- a Remembrance hold whose item is not a candidate
        HoldShown,                // a: slot pressed, b: slot shown on, c: 1 = its class does not fit there
        NoCandidate,              // a: slot
        HoldGaveWay,              // a: slot, b: holder, c: challenger; x/y: their raw scores, z/w: their caps; capped
        PulledToJobKey,           // a: job slot, b: from Regular slot, c: candidate
        Returner,                 // a: candidate now showing it, b: home slot, c: slot now, f: away s, why, key, d/e: displaced cand/slot
    };

    /// Why a returner is not on its home key (SlotAllocator's Away).
    enum class Away : std::uint8_t { None, Off, Taken, Waits, Class, Owned };

    struct Event
    {
        EventKind kind{};
        std::uint32_t a = 0, b = 0, c = 0, d = kNone, e = kNone;
        double x = 0.0, y = 0.0, z = 0.0, w = 0.0;
        float f = 0.0f;
        bool capped = false;
        Away why = Away::None;
        std::uint64_t key = 0;
    };

    struct Output
    {
        std::vector<SlotState> slots;
        PageMemory memory;
        bool generationMatches = false;   // after the run (seating stamps it)
        bool clearedAllPages = false;     // a stale generation: every page's memory is cleared
        bool seatingRecorded = false;     // RecordSeating ran
        std::array<std::uint64_t, kMaxSlots> seatsBeforeRecord{};   // for the [Seating] change log
        std::vector<std::uint32_t> skipped;   // candidates the class cap passed over (SlotClassCap's notes)
        std::vector<Event> events;
    };

    // =========================================================================
    // The score policy the game runs: any sign, additive class cap.
    // =========================================================================
    struct LogScorePolicy
    {
        using Score = SlotScore;
        using Cap = SlotScore;   // an additive term, <= 0

        float discount = 1.0f;
        std::uint32_t freePerClass = 0;
        double logMargin = 0.0;

        [[nodiscard]] static LogScorePolicy From(const Settings& s) noexcept
        {
            return { ClampClassCapDiscount(s.classDiscount), s.classFree, LogHoldMargin(s.challengerMargin) };
        }
        [[nodiscard]] bool CapActive() const noexcept { return ClassCapActive(discount); }
        [[nodiscard]] Score Raw(const CandidateRec& c) const noexcept { return c.score; }
        [[nodiscard]] Cap NoCap() const noexcept { return 0.0; }
        [[nodiscard]] Cap CapFor(std::uint32_t shown) const noexcept { return ClassCapTerm(discount, freePerClass, shown); }
        [[nodiscard]] bool IsCapped(Cap c) const noexcept { return c < 0.0; }
        [[nodiscard]] Score Apply(Score s, Cap c) const noexcept { return s + c; }
        [[nodiscard]] bool Greater(Score a, Score b) const noexcept { return ScoreGreater(a, b); }
        [[nodiscard]] bool RawGreater(Score a, Score b) const noexcept { return a > b; }
        [[nodiscard]] bool Exceeds(Score c, Score h) const noexcept { return ScoreExceedsByMargin(c, h, logMargin); }
        [[nodiscard]] static double ToDouble(double v) noexcept { return v; }
    };

    /// SlotAllocator::ComputePriorityOrder: slot indices by priority, highest
    /// first, ties in slot order (stable). Returns the count (at most kMaxSlots).
    [[nodiscard]] inline std::size_t PriorityOrder(const std::vector<SlotRec>& slots,
        std::array<std::size_t, kMaxSlots>& order)
    {
        const std::size_t n = std::min(slots.size(), kMaxSlots);
        for (std::size_t i = 0; i < n; ++i) order[i] = i;
        std::stable_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(n),
            [&slots](std::size_t a, std::size_t b) { return slots[a].priority > slots[b].priority; });
        return n;
    }

    // =========================================================================
    // The algorithm
    // =========================================================================
    namespace Detail
    {
        /// The facts about an assignment's item that slot acceptance and
        /// seating read. An override's item, or a candidate's.
        struct ItemView
        {
            std::uint32_t formID = 0;
            const std::string* name = nullptr;
            std::uint64_t key = 0;
            std::uint32_t matchMask = 0;
            std::uint8_t capClass = 0;
            bool isWildcard = false;
            bool isEquipped = false;
            bool playerEquipped = false;
        };

        template <class Policy>
        class Engine
        {
        public:
            using Score = typename Policy::Score;
            using Cap = typename Policy::Cap;

            Engine(const Input& in, const Policy& policy) :
                m_in(in), m_p(policy), m_cap(policy.CapActive())
            {
                m_out.memory = in.memory;
                m_out.generationMatches = in.generationMatches;
                m_state.assign(in.slots.size(), SlotState{});
            }

            Output Run();

            /// PullIntoEmptyJobKeys on a page built elsewhere, in the caller's
            /// slot order (the in-game fill-job-keys test drives it with a
            /// made-up page).
            std::vector<SlotState> PullOnly(std::vector<SlotState> state,
                const std::array<std::size_t, kMaxSlots>& order, std::size_t priorityCount, std::vector<Event>* events)
            {
                m_state = std::move(state);
                m_order = order;
                PullIntoEmptyJobKeys(std::min(priorityCount, kMaxSlots));
                if (events) {
                    *events = std::move(m_out.events);
                }
                return std::move(m_state);
            }

        private:
            // --- the class cap's counting (SlotClassCap) ---------------------
            struct ClassCounter
            {
                bool active = false;
                std::array<std::uint8_t, kMaxClasses> onPage{};
                std::vector<std::uint32_t> skipped;   // candidate indices
                std::vector<std::uint32_t> skippedIDs;

                [[nodiscard]] std::uint32_t Shown(std::uint8_t cls) const noexcept { return onPage[cls % kMaxClasses]; }
                void Add(std::uint8_t cls) noexcept
                {
                    if (!active) return;
                    auto& n = onPage[cls % kMaxClasses];
                    if (n < UINT8_MAX) ++n;
                }
                void Remove(std::uint8_t cls) noexcept
                {
                    if (!active) return;
                    auto& n = onPage[cls % kMaxClasses];
                    if (n > 0) --n;
                }
            };

            [[nodiscard]] ItemView View(const SlotState& s) const
            {
                ItemView v;
                if (s.kind == Kind::Override) {
                    const auto& o = m_in.overrides[s.src];
                    v = { o.formID, &o.name, o.dedupKey, o.matchMask, o.capClass, false, o.isEquipped, o.playerEquipped };
                } else {
                    const auto& c = m_in.candidates[s.src];
                    // A remembered copy is placed with isWildcard cleared.
                    v = { c.formID, &c.name, c.dedupKey, c.matchMask, c.capClass,
                        s.kind == Kind::Remembered ? false : c.isWildcard, c.isEquipped, c.playerEquipped };
                }
                return v;
            }

            [[nodiscard]] std::uint64_t KeyOf(const SlotState& s) const
            {
                return s.IsEmpty() ? 0 : View(s).key;
            }

            [[nodiscard]] static bool MaskMatches(std::uint32_t mask, const SlotRec& slot) noexcept
            {
                return slot.classIndex < 32 && ((mask >> slot.classIndex) & 1u) != 0;
            }

            /// SlotAllocator::SlotAccepts.
            [[nodiscard]] bool SlotAccepts(const SlotRec& slot, const SlotState& s) const
            {
                if (s.IsEmpty()) {
                    return true;
                }
                const ItemView v = View(s);
                if (!MaskMatches(v.matchMask, slot)) return false;
                if (v.isWildcard && !slot.wildcardsEnabled) return false;
                if (slot.skipEquipped && (v.isEquipped || v.playerEquipped)) return false;
                return true;
            }

            void Note(Event e) { m_out.events.push_back(e); }

            void Recount()
            {
                if (!m_cap.active) return;
                m_cap.onPage.fill(0);
                for (const auto& s : m_state) {
                    if (!s.IsEmpty()) m_cap.Add(View(s).capClass);
                }
            }

            [[nodiscard]] Cap CapOf(std::uint8_t cls) const
            {
                // SlotClassCap::Factor: 1 (no cap) when the cap is off.
                return m_cap.active ? m_p.CapFor(m_cap.Shown(cls)) : m_p.NoCap();
            }

            void NoteSkipped(std::uint32_t i)
            {
                const std::uint32_t id = m_in.candidates[i].formID;
                if (std::find(m_cap.skippedIDs.begin(), m_cap.skippedIDs.end(), id) == m_cap.skippedIDs.end()) {
                    m_cap.skipped.push_back(i);
                    m_cap.skippedIDs.push_back(id);
                }
            }

            /// SlotClassCap::DropSkipsSince: keep a note only if the skipped
            /// item, uncapped, would have taken the slot from the holder.
            void DropSkipsSince(std::size_t mark, Score holderScore)
            {
                if (mark >= m_cap.skipped.size()) return;
                std::size_t w = mark;
                for (std::size_t r = mark; r < m_cap.skipped.size(); ++r) {
                    const std::uint32_t i = m_cap.skipped[r];
                    if (m_p.Exceeds(m_p.Raw(m_in.candidates[i]), holderScore)) {
                        m_cap.skipped[w] = m_cap.skipped[r];
                        m_cap.skippedIDs[w] = m_cap.skippedIDs[r];
                        ++w;
                    }
                }
                m_cap.skipped.resize(w);
                m_cap.skippedIDs.resize(w);
            }

            std::size_t ComputePriorityOrder() { return PriorityOrder(m_in.slots, m_order); }

            /// SlotAllocator::FindBestCandidate. `cap` null = no class cap.
            [[nodiscard]] std::optional<std::uint32_t> FindBest(const SlotRec& slot,
                const std::set<std::uint32_t>& ids, const std::set<std::string_view>& names,
                bool skipEquipped, bool skipWildcards, bool useCap)
            {
                const bool capped = useCap && m_cap.active && slot.regular;
                std::uint32_t first = kNone;
                std::uint32_t best = kNone;
                Score bestScore{};
                const auto& cands = m_in.candidates;
                for (std::uint32_t i = 0; i < cands.size(); ++i) {
                    const auto& c = cands[i];
                    if (skipWildcards && c.isWildcard) continue;
                    if (c.isRememberedOnly) continue;
                    if (ids.contains(c.formID)) continue;
                    if (names.contains(c.name)) continue;
                    if (!MaskMatches(c.matchMask, slot)) continue;
                    if (skipEquipped && (c.isEquipped || c.playerEquipped)) continue;
                    if (!capped) {
                        return i;
                    }
                    if (best == kNone) {
                        const Cap cap = CapOf(c.capClass);
                        if (c.isWildcard || !m_p.IsCapped(cap)) {
                            return i;
                        }
                        first = best = i;
                        bestScore = m_p.Apply(m_p.Raw(c), cap);
                        continue;
                    }
                    if (c.isWildcard) continue;
                    const Score effective = m_p.Apply(m_p.Raw(c), CapOf(c.capClass));
                    if (m_p.Greater(effective, bestScore)) {
                        best = i;
                        bestScore = effective;
                    }
                }
                if (best == kNone) {
                    return std::nullopt;
                }
                if (best != first) {
                    NoteSkipped(first);
                }
                return best;
            }

            /// SlotAllocator::FindItemSlot.
            [[nodiscard]] std::size_t FindItemSlot(std::uint64_t key, std::size_t slotCount) const
            {
                if (key == 0 || !m_in.memoryAvailable || !m_in.settings.keepSlotPositions) return npos;
                if (!m_out.generationMatches) return npos;
                const std::size_t n = std::min(slotCount, kMaxSlots);
                for (std::size_t j = 0; j < n; ++j) {
                    if (m_out.memory.seats[j] == key) return j;
                }
                for (std::size_t j = 0; j < n; ++j) {
                    if (m_out.memory.lastPlaced[j] == key) return j;
                }
                return npos;
            }

            void Place(std::size_t slot, Kind kind, std::uint32_t src)
            {
                m_state[slot] = SlotState{ kind, src, false };
            }

            void Assign(std::uint32_t formID, const std::string& name)
            {
                m_ids.insert(formID);
                m_names.insert(name);
            }

            void PassOverrides(std::size_t priorityCount);
            void PassRemembrance(std::size_t priorityCount);
            void HoldIncumbents(std::size_t priorityCount);
            void Fill(std::size_t priorityCount, bool refill);
            void ApplySeating();
            void RecordSeating();
            void PullIntoEmptyJobKeys(std::size_t priorityCount);

            const Input& m_in;
            const Policy& m_p;
            ClassCounter m_cap;
            Output m_out;
            std::vector<SlotState> m_state;
            std::array<std::size_t, kMaxSlots> m_order{};
            std::set<std::uint32_t> m_ids;
            std::set<std::string_view> m_names;
        };

        // ---------------------------------------------------------------------
        // AllocateSlotsInternal
        // ---------------------------------------------------------------------
        template <class Policy>
        Output Engine<Policy>::Run()
        {
            if (m_in.slots.empty()) {
                m_out.slots = m_state;
                return m_out;
            }
            const std::size_t priorityCount = ComputePriorityOrder();
            const auto& set = m_in.settings;

            // PASS 1 / 1b: overrides.
            if (m_in.overridesActive) {
                PassOverrides(priorityCount);
            } else {
                Note({ EventKind::OverridesInactive });
            }

            // PASS 1a: Remembrance.
            PassRemembrance(priorityCount);

            // The class cap counts what overrides and Remembrance placed.
            Recount();

            // PASS 1c: the slot hold.
            if (set.keepSlotPositions && set.holdSeatedItems) {
                HoldIncumbents(priorityCount);
            }

            // PASS 2: the rank-ordered fill.
            Fill(priorityCount, /*refill=*/false);

            if (!set.keepSlotPositions && set.fillJobKeysFromRegular) {
                PullIntoEmptyJobKeys(priorityCount);
                Recount();
                Fill(priorityCount, /*refill=*/true);
            }

            // PASS 3-5: seating, the optional pull, the refill, the record.
            if (set.keepSlotPositions) {
                ApplySeating();
                if (set.fillJobKeysFromRegular) {
                    PullIntoEmptyJobKeys(priorityCount);
                }
                Recount();
                Fill(priorityCount, /*refill=*/true);
                RecordSeating();
            }

            m_out.slots = m_state;
            m_out.skipped = m_cap.skipped;
            return m_out;
        }

        template <class Policy>
        void Engine<Policy>::PassOverrides(std::size_t priorityCount)
        {
            const auto& slots = m_in.slots;
            const auto& ovs = m_in.overrides;
            for (std::uint32_t o = 0; o < ovs.size(); ++o) {
                const auto& ov = ovs[o];
                if (m_ids.contains(ov.formID)) continue;

                // Already on this page: mark that slot (unless a vital pinned
                // to its configured slot).
                if (const std::size_t home = ov.pinnedToSlot ? npos
                            : FindItemSlot(ov.dedupKey, std::min(slots.size(), kMaxSlots));
                    home != npos && m_state[home].IsEmpty()) {
                    const SlotState marked{ Kind::Override, o, false };
                    if (SlotAccepts(slots[home], marked)) {
                        m_state[home] = marked;
                        Assign(ov.formID, ov.name);
                        Note({ EventKind::OverrideMarked, o, static_cast<std::uint32_t>(home) });
                        continue;
                    }
                }

                // The first override-enabled slot that matches its type.
                for (std::size_t k = 0; k < priorityCount; ++k) {
                    const std::size_t idx = m_order[k];
                    const auto& config = slots[idx];
                    if (((config.overrideAccept >> ov.category) & 1u) == 0) continue;
                    if (!m_state[idx].IsEmpty()) continue;
                    if (MaskMatches(ov.matchMask, config)) {
                        Place(idx, Kind::Override, o);
                        Assign(ov.formID, ov.name);
                        Note({ EventKind::OverridePlaced, o, static_cast<std::uint32_t>(idx) });
                        break;
                    }
                }
            }

            // PASS 1b: the first empty slot that accepts the category.
            for (std::uint32_t o = 0; o < ovs.size(); ++o) {
                const auto& ov = ovs[o];
                if (m_ids.contains(ov.formID)) continue;
                bool placed = false;
                bool sawAcceptingSlot = false;
                for (std::size_t k = 0; k < priorityCount; ++k) {
                    const std::size_t idx = m_order[k];
                    if (((slots[idx].overrideAccept >> ov.category) & 1u) == 0) continue;
                    sawAcceptingSlot = true;
                    if (!m_state[idx].IsEmpty()) continue;
                    Place(idx, Kind::Override, o);
                    Assign(ov.formID, ov.name);
                    placed = true;
                    Note({ EventKind::OverrideFallback, o, static_cast<std::uint32_t>(idx) });
                    break;
                }
                if (!placed) {
                    Note({ EventKind::OverrideUnplaced, o, sawAcceptingSlot ? 1u : 0u });
                }
            }
        }

        template <class Policy>
        void Engine<Policy>::PassRemembrance(std::size_t priorityCount)
        {
            const auto& slots = m_in.slots;
            const auto& held = m_in.holds;
            const bool toJob = m_in.settings.remembranceToJob;
            const std::size_t n = std::min(slots.size(), kMaxSlots);
            for (int sweep = 0; sweep < 2; ++sweep) {
                for (std::size_t j = 0; j < n; ++j) {
                    const auto& entry = held[j];
                    if (!entry.active || !slots[j].remembrance) continue;
                    if (m_ids.contains(entry.formID)) continue;

                    // The best-scoring stack of a form owned twice.
                    std::uint32_t found = kNone;
                    for (std::uint32_t i = 0; i < m_in.candidates.size(); ++i) {
                        const auto& c = m_in.candidates[i];
                        if (c.formID == entry.formID &&
                            (found == kNone || m_p.RawGreater(m_p.Raw(c), m_p.Raw(m_in.candidates[found])))) {
                            found = i;
                        }
                    }
                    if (found == kNone) {
                        Note({ EventKind::HoldNotCandidate, static_cast<std::uint32_t>(j), entry.formID });
                        continue;
                    }
                    const auto& fc = m_in.candidates[found];
                    auto fits = [&](std::size_t k) {
                        return slots[k].regular || MaskMatches(fc.matchMask, slots[k]);
                    };
                    std::size_t target = j;
                    if (toJob && !fits(j)) {
                        if (sweep == 0) continue;
                        for (std::size_t k = 0; k < priorityCount; ++k) {
                            const std::size_t t = m_order[k];
                            if (t >= n || t == j || !slots[t].remembrance) continue;
                            if (slots[t].regular) continue;
                            if (held[t].active) continue;
                            if (!m_state[t].IsEmpty() || !fits(t)) continue;
                            target = t;
                            break;
                        }
                    } else if (sweep == 1) {
                        continue;
                    }
                    if (!m_state[target].IsEmpty()) continue;

                    Note({ EventKind::HoldShown, static_cast<std::uint32_t>(j), static_cast<std::uint32_t>(target),
                        fits(target) ? 0u : 1u });
                    Place(target, Kind::Remembered, found);
                    m_ids.insert(entry.formID);
                    m_names.insert(fc.name);
                }
            }
        }

        template <class Policy>
        void Engine<Policy>::HoldIncumbents(std::size_t priorityCount)
        {
            if (!m_in.memoryAvailable) return;
            const auto& slots = m_in.slots;
            const auto& cands = m_in.candidates;
            const std::size_t slotCount = std::min(m_state.size(), std::min(slots.size(), kMaxSlots));
            if (!m_out.generationMatches) return;
            const auto seats = m_out.memory.seats;
            const auto placed = m_out.memory.lastPlaced;
            const auto claims = m_out.memory.homeClaims;

            struct Tentative { std::size_t slot; std::uint32_t item; };
            std::array<Tentative, kMaxSlots> tentative{};
            std::size_t tentativeCount = 0;
            std::set<std::uint32_t> excludedIDs = m_ids;
            std::set<std::string_view> excludedNames = m_names;

            // Phase A: seat owners still candidates, still allowed in their seat.
            for (std::size_t k = 0; k < priorityCount; ++k) {
                const std::size_t j = m_order[k];
                if (j >= slotCount) continue;
                if (!m_state[j].IsEmpty() || seats[j] == 0) continue;
                std::uint32_t owner = kNone;
                for (std::uint32_t i = 0; i < cands.size(); ++i) {
                    if (!cands[i].isRememberedOnly && cands[i].dedupKey == seats[j]) { owner = i; break; }
                }
                if (owner == kNone || m_ids.contains(cands[owner].formID) || m_names.contains(cands[owner].name)) {
                    continue;
                }
                if (!SlotAccepts(slots[j], SlotState{ Kind::Normal, owner, false })) continue;
                tentative[tentativeCount++] = { j, owner };
                excludedIDs.insert(cands[owner].formID);
                excludedNames.insert(cands[owner].name);
            }

            // Owners whose seat a pinned item is sitting in stay out of the challengers.
            std::set<std::uint32_t> displacedOwners;
            for (std::size_t j = 0; j < slotCount; ++j) {
                if (!m_state[j].IsPinned() || seats[j] == 0) continue;
                if (KeyOf(m_state[j]) == seats[j]) continue;
                for (std::uint32_t i = 0; i < cands.size(); ++i) {
                    if (cands[i].dedupKey == seats[j]) {
                        excludedIDs.insert(cands[i].formID);
                        excludedNames.insert(cands[i].name);
                        displacedOwners.insert(cands[i].formID);
                        break;
                    }
                }
            }

            // Guests: held where they stand.
            for (std::size_t k = 0; k < priorityCount; ++k) {
                const std::size_t j = m_order[k];
                if (j >= slotCount || !m_state[j].IsEmpty() || placed[j] == 0) continue;
                bool taken = false;
                for (std::size_t t = 0; t < tentativeCount; ++t) {
                    taken = taken || tentative[t].slot == j;
                }
                if (taken) continue;
                std::uint32_t guest = kNone;
                for (std::uint32_t i = 0; i < cands.size(); ++i) {
                    if (!cands[i].isRememberedOnly && cands[i].dedupKey == placed[j]) { guest = i; break; }
                }
                if (guest == kNone) continue;
                const auto& g = cands[guest];
                if (displacedOwners.contains(g.formID)) {
                    if (m_ids.contains(g.formID) || m_names.contains(g.name)) continue;
                } else if (excludedIDs.contains(g.formID) || excludedNames.contains(g.name)) {
                    continue;
                }
                if (!SlotAccepts(slots[j], SlotState{ Kind::Normal, guest, false })) continue;
                tentative[tentativeCount++] = { j, guest };
                excludedIDs.insert(g.formID);
                excludedNames.insert(g.name);
            }

            // One slot-priority order for owners and guests.
            std::array<std::size_t, kMaxSlots> rank{};
            for (std::size_t k = 0; k < priorityCount; ++k) {
                if (m_order[k] < kMaxSlots) rank[m_order[k]] = k;
            }
            std::sort(tentative.begin(), tentative.begin() + static_cast<std::ptrdiff_t>(tentativeCount),
                [&rank](const Tentative& a, const Tentative& b) { return rank[a.slot] < rank[b.slot]; });

            // The class cap over the holders together, by score.
            std::array<Cap, kMaxSlots> holderCap{};
            holderCap.fill(m_p.NoCap());
            {
                std::array<std::size_t, kMaxSlots> byScore{};
                for (std::size_t t = 0; t < tentativeCount; ++t) byScore[t] = t;
                std::stable_sort(byScore.begin(), byScore.begin() + static_cast<std::ptrdiff_t>(tentativeCount),
                    [&](std::size_t a, std::size_t b) {
                        return m_p.RawGreater(m_p.Raw(cands[tentative[a].item]), m_p.Raw(cands[tentative[b].item]));
                    });
                for (std::size_t i = 0; i < tentativeCount; ++i) {
                    const auto& [slot, item] = tentative[byScore[i]];
                    if (slots[slot].regular) {
                        holderCap[byScore[i]] = CapOf(cands[item].capClass);
                    }
                    m_cap.Add(cands[item].capClass);
                }
            }

            // Phase B: each holder against the best challenger for its slot.
            for (std::size_t t = 0; t < tentativeCount; ++t) {
                const auto [j, item] = tentative[t];
                const auto& config = slots[j];
                const auto& it = cands[item];

                if (it.isWildcard) {
                    Place(j, Kind::Wildcard, item);
                    Assign(it.formID, it.name);
                    continue;
                }

                m_cap.Remove(it.capClass);
                const std::size_t skipMark = m_cap.skipped.size();
                const auto challenger = FindBest(config, excludedIDs, excludedNames, config.skipEquipped,
                    /*skipWildcards=*/!config.wildcardsEnabled, /*useCap=*/true);
                const bool capped = config.regular;
                const Cap itemCap = capped ? holderCap[t] : m_p.NoCap();
                const Cap challengerCap = capped && challenger ? CapOf(cands[*challenger].capClass) : m_p.NoCap();
                const Score itemScore = m_p.Apply(m_p.Raw(it), itemCap);

                if (challenger) {
                    const auto& ch = cands[*challenger];
                    const Score challengerScore = m_p.Apply(m_p.Raw(ch), challengerCap);
                    if (m_p.Exceeds(challengerScore, itemScore)) {
                        m_cap.Add(ch.capClass);
                        Event e{ EventKind::HoldGaveWay, static_cast<std::uint32_t>(j), item, *challenger };
                        e.x = Policy::ToDouble(m_p.Raw(it));
                        e.y = Policy::ToDouble(m_p.Raw(ch));
                        e.z = Policy::ToDouble(itemCap);
                        e.w = Policy::ToDouble(challengerCap);
                        e.capped = m_p.IsCapped(itemCap) || m_p.IsCapped(challengerCap);
                        Note(e);

                        Place(j, ch.isWildcard ? Kind::Wildcard : Kind::Normal, *challenger);
                        Assign(ch.formID, ch.name);
                        excludedIDs.insert(ch.formID);
                        excludedNames.insert(ch.name);

                        if (!displacedOwners.contains(it.formID)) {
                            excludedIDs.erase(it.formID);
                            excludedNames.erase(it.name);
                        }
                        const std::uint64_t loserKey = it.dedupKey;
                        if (seats[j] == loserKey && m_out.generationMatches && m_out.memory.seats[j] == loserKey) {
                            m_out.memory.seats[j] = 0;
                        }
                        continue;
                    }
                }

                Place(j, it.isWildcard ? Kind::Wildcard : Kind::Normal, item);
                {
                    const std::uint64_t key = it.dedupKey;
                    m_state[j].seatMoved = claims[j] == key && placed[j] != key;
                }
                Assign(it.formID, it.name);
                m_cap.Add(it.capClass);
                DropSkipsSince(skipMark, itemScore);
            }
        }

        template <class Policy>
        void Engine<Policy>::Fill(std::size_t priorityCount, bool refill)
        {
            const auto& slots = m_in.slots;
            for (std::size_t k = 0; k < priorityCount; ++k) {
                const std::size_t idx = m_order[k];
                const auto& config = slots[idx];
                if (!m_state[idx].IsEmpty()) continue;

                if (refill) {
                    // Passes 2b and 4: wildcards only where the slot takes them.
                    const auto pick = FindBest(config, m_ids, m_names, config.skipEquipped,
                        /*skipWildcards=*/!config.wildcardsEnabled, /*useCap=*/true);
                    if (!pick) continue;
                    const auto& c = m_in.candidates[*pick];
                    Place(idx, c.isWildcard ? Kind::Wildcard : Kind::Normal, *pick);
                    Assign(c.formID, c.name);
                    m_cap.Add(c.capClass);
                    continue;
                }

                auto best = FindBest(config, m_ids, m_names, config.skipEquipped, /*skipWildcards=*/false, /*useCap=*/true);
                if (!best) {
                    Note({ EventKind::NoCandidate, static_cast<std::uint32_t>(idx) });
                    continue;
                }
                Kind kind = m_in.candidates[*best].isWildcard ? Kind::Wildcard : Kind::Normal;
                if (kind == Kind::Wildcard && !config.wildcardsEnabled) {
                    best = FindBest(config, m_ids, m_names, config.skipEquipped, /*skipWildcards=*/true, /*useCap=*/true);
                    kind = Kind::Normal;
                }
                if (best) {
                    const auto& c = m_in.candidates[*best];
                    Place(idx, kind, *best);
                    Assign(c.formID, c.name);
                    m_cap.Add(c.capClass);
                }
            }
        }

        template <class Policy>
        void Engine<Policy>::ApplySeating()
        {
            if (!m_in.memoryAvailable) return;
            const auto& slots = m_in.slots;
            const std::size_t slotCount = std::min(m_state.size(), std::min(slots.size(), kMaxSlots));
            if (slotCount == 0) return;

            if (!m_out.generationMatches) {
                // A layout reload: every page's memory starts over from this pass.
                m_out.memory = PageMemory{};
                m_out.clearedAllPages = true;
                m_out.generationMatches = true;
                return;
            }
            auto seats = m_out.memory.seats;
            const auto departed = m_out.memory.departed;
            auto claims = m_out.memory.homeClaims;

            auto seatWantedBy = [&](std::uint64_t key) -> std::size_t {
                if (key == 0) return npos;
                for (std::size_t j = 0; j < slotCount; ++j) {
                    if (seats[j] == key) return j;
                }
                return npos;
            };
            auto movable = [&](std::size_t idx) { return !m_state[idx].IsPinned(); };
            auto moveTo = [&](std::size_t from, std::size_t to) {
                const bool claimant = claims[to] != 0 && claims[to] == KeyOf(m_state[from]);
                m_state[to] = m_state[from];
                m_state[to].seatMoved = claimant;
                m_state[from] = SlotState{};
            };

            // Phase 0: home keys.
            struct Returner
            {
                std::uint64_t key;
                std::size_t at;
                std::size_t home;
                float awaySec;
                Away why;
                std::uint64_t displaced = 0;
            };
            std::array<Returner, kMaxSlots> returners{};
            std::size_t returnerCount = 0;
            const float memorySec = m_in.settings.homeKeyMemorySec;
            if (memorySec > 0.0f) {
                const std::chrono::nanoseconds now{ m_in.nowNs };
                for (std::size_t i = 0; i < slotCount; ++i) {
                    const std::uint64_t key = KeyOf(m_state[i]);
                    if (key == 0 || !movable(i) || seatWantedBy(key) != npos) continue;
                    std::size_t home = npos;
                    std::chrono::nanoseconds leftAt{ 0 };
                    for (std::size_t j = 0; j < slotCount; ++j) {
                        for (const auto& d : departed[j]) {
                            const std::chrono::nanoseconds dLeft{ d.leftAtNs };
                            if (d.key == key && (home == npos || dLeft > leftAt)) {
                                home = j;
                                leftAt = dLeft;
                            }
                        }
                    }
                    const float awaySec = std::chrono::duration<float>(now - leftAt).count();
                    if (home == npos || awaySec > memorySec) continue;
                    returners[returnerCount++] = { key, i, home, awaySec, Away::None, 0 };
                }
                std::sort(returners.begin(), returners.begin() + static_cast<std::ptrdiff_t>(returnerCount),
                    [](const Returner& a, const Returner& b) { return a.awaySec < b.awaySec; });
            }

            bool seatsReclaimed = false;
            {
                std::array<bool, kMaxSlots> claimed{};
                const bool returnHome = m_in.settings.returnToHomeKey;
                for (std::size_t r = 0; r < returnerCount; ++r) {
                    auto& ret = returners[r];
                    const std::size_t h = ret.home;
                    if (!returnHome) { ret.why = Away::Off; continue; }
                    if (claimed[h]) { ret.why = Away::Taken; continue; }
                    if (ret.at != h) {
                        if (!SlotAccepts(slots[h], m_state[ret.at])) {
                            ret.why = Away::Class;
                            continue;
                        }
                        if (!movable(h)) {
                            if (KeyOf(m_state[h]) == seats[h]) {
                                ret.why = Away::Owned;
                                continue;
                            }
                            ret.why = Away::Waits;
                        } else if (!m_state[h].IsEmpty() && !SlotAccepts(slots[ret.at], m_state[h])) {
                            ret.why = Away::Class;
                            continue;
                        }
                        if (movable(h)) {
                            ret.displaced = KeyOf(m_state[h]);
                        }
                    }
                    claimed[h] = true;
                    seats[h] = ret.key;
                    claims[h] = ret.key;
                    seatsReclaimed = true;
                }
            }
            if (returnerCount > 0) {
                // m_out.generationMatches is true here.
                if (seatsReclaimed) {
                    m_out.memory.seats = seats;
                    m_out.memory.homeClaims = claims;
                }
                for (std::size_t r = 0; r < returnerCount; ++r) {
                    for (auto& slot : m_out.memory.departed) {
                        for (auto& d : slot) {
                            if (d.key == returners[r].key) d = {};
                        }
                    }
                }
            }

            // Phase 1: moves into empty seats, repeated.
            for (std::size_t round = 0; round < slotCount; ++round) {
                bool moved = false;
                for (std::size_t i = 0; i < slotCount; ++i) {
                    if (m_state[i].IsEmpty() || !movable(i)) continue;
                    const std::size_t want = seatWantedBy(KeyOf(m_state[i]));
                    if (want == npos || want == i || want >= slotCount) continue;
                    if (!m_state[want].IsEmpty() || !movable(want)) continue;
                    if (!SlotAccepts(slots[want], m_state[i])) continue;
                    moveTo(i, want);
                    moved = true;
                }
                if (!moved) break;
            }

            // Phase 2: two items holding each other's seats.
            for (std::size_t round = 0; round < slotCount; ++round) {
                bool swapped = false;
                for (std::size_t i = 0; i < slotCount; ++i) {
                    if (m_state[i].IsEmpty() || !movable(i)) continue;
                    const std::size_t want = seatWantedBy(KeyOf(m_state[i]));
                    if (want == npos || want == i || want >= slotCount) continue;
                    if (m_state[want].IsEmpty() || !movable(want)) continue;
                    if (!SlotAccepts(slots[want], m_state[i])) continue;
                    if (!SlotAccepts(slots[i], m_state[want])) continue;
                    const bool claimant = claims[want] != 0 && claims[want] == KeyOf(m_state[i]);
                    std::swap(m_state[i], m_state[want]);
                    m_state[i].seatMoved = claimant;
                    m_state[want].seatMoved = claimant;
                    swapped = true;
                }
                if (!swapped) break;
            }

            // Where each returner ended up (telemetry and [HomeKey] lines).
            for (std::size_t r = 0; r < returnerCount; ++r) {
                const auto& ret = returners[r];
                std::size_t nowSlot = npos;
                for (std::size_t i = 0; i < slotCount; ++i) {
                    if (KeyOf(m_state[i]) == ret.key) { nowSlot = i; break; }
                }
                if (nowSlot == npos) continue;
                Event e{ EventKind::Returner, m_state[nowSlot].src, static_cast<std::uint32_t>(ret.home),
                    static_cast<std::uint32_t>(nowSlot) };
                e.f = ret.awaySec;
                e.why = ret.why;
                e.key = ret.key;
                e.x = m_state[nowSlot].kind == Kind::Override ? 1.0 : 0.0;
                for (std::size_t i = 0; ret.displaced != 0 && i < slotCount; ++i) {
                    if (KeyOf(m_state[i]) == ret.displaced) {
                        e.d = m_state[i].src;
                        e.e = static_cast<std::uint32_t>(i);
                        e.y = m_state[i].kind == Kind::Override ? 1.0 : 0.0;
                        break;
                    }
                }
                Note(e);
            }
        }

        template <class Policy>
        void Engine<Policy>::RecordSeating()
        {
            if (!m_in.memoryAvailable) return;
            const std::size_t slotCount = std::min(m_state.size(), kMaxSlots);
            auto& mem = m_out.memory;
            const auto previous = mem.seats;
            m_out.seatsBeforeRecord = previous;
            m_out.seatingRecorded = true;

            auto previousSeatOf = [&](std::uint64_t key) -> std::size_t {
                if (key == 0) return npos;
                for (std::size_t j = 0; j < slotCount; ++j) {
                    if (previous[j] == key) return j;
                }
                return npos;
            };

            std::array<std::uint64_t, kMaxSlots> seats{};
            for (std::size_t i = 0; i < slotCount; ++i) {
                const std::uint64_t key = KeyOf(m_state[i]);
                if (key == 0) continue;
                const std::size_t home = previousSeatOf(key);
                if (home != npos && (home != i || m_state[i].IsPinned())) {
                    seats[home] = key;
                }
            }
            for (std::size_t i = 0; i < slotCount; ++i) {
                if (m_state[i].IsPinned()) continue;
                const std::uint64_t key = KeyOf(m_state[i]);
                if (key == 0) continue;
                const std::size_t home = previousSeatOf(key);
                if (home == i) {
                    seats[i] = key;
                } else if (home == npos && seats[i] == 0) {
                    seats[i] = key;
                }
            }

            std::array<std::uint64_t, kMaxSlots> placedNow{};
            for (std::size_t i = 0; i < slotCount; ++i) {
                if (!m_state[i].IsPinned()) {
                    placedNow[i] = KeyOf(m_state[i]);
                }
            }

            auto onScreen = [&](std::uint64_t key) {
                return std::any_of(m_state.begin(), m_state.end(),
                    [&](const SlotState& s) { return KeyOf(s) == key; });
            };
            auto& claims = mem.homeClaims;
            auto remember = [&](std::size_t j, std::uint64_t key) {
                if (claims[j] != 0 && claims[j] != key) return;
                auto& ring = mem.departed[j];
                std::move_backward(ring.begin(), ring.end() - 1, ring.end());
                ring[0] = { key, m_in.nowNs };
            };
            for (std::size_t j = 0; j < slotCount; ++j) {
                if (previous[j] != 0 && !onScreen(previous[j])) remember(j, previous[j]);
            }
            const auto stood = mem.lastPlaced;
            for (std::size_t j = 0; j < slotCount; ++j) {
                const std::uint64_t key = stood[j];
                if (key == 0 || onScreen(key)) continue;
                const auto prevEnd = previous.begin() + static_cast<std::ptrdiff_t>(slotCount);
                if (std::find(previous.begin(), prevEnd, key) != prevEnd) continue;
                remember(j, key);
            }

            for (std::size_t j = 0; j < slotCount; ++j) {
                if (claims[j] != 0 && (seats[j] != claims[j] || KeyOf(m_state[j]) == claims[j])) {
                    claims[j] = 0;
                }
            }

            m_out.generationMatches = true;
            mem.seats = seats;
            mem.lastPlaced = placedNow;
        }

        template <class Policy>
        void Engine<Policy>::PullIntoEmptyJobKeys(std::size_t priorityCount)
        {
            const auto& slots = m_in.slots;
            const std::size_t slotCount = std::min(m_state.size(), std::min(slots.size(), kMaxSlots));
            for (std::size_t k = 0; k < priorityCount; ++k) {
                const std::size_t i = m_order[k];
                if (i >= slotCount || !m_state[i].IsEmpty()) continue;
                if (slots[i].regular) continue;

                std::size_t from = npos;
                for (std::size_t j = 0; j < slotCount; ++j) {
                    const auto& a = m_state[j];
                    if (j == i || a.IsEmpty() || a.IsPinned() || a.kind == Kind::Wildcard) continue;
                    if (!slots[j].regular) continue;
                    if (!SlotAccepts(slots[i], a)) continue;
                    if (from == npos ||
                        m_p.RawGreater(m_p.Raw(m_in.candidates[a.src]), m_p.Raw(m_in.candidates[m_state[from].src]))) {
                        from = j;
                    }
                }
                if (from == npos) continue;

                Note({ EventKind::PulledToJobKey, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(from),
                    m_state[from].src });
                m_state[i] = m_state[from];
                m_state[from] = SlotState{};
            }
        }
    }  // namespace Detail

    /// One allocation of one page.
    template <class Policy>
    [[nodiscard]] Output Allocate(const Input& in, const Policy& policy)
    {
        Detail::Engine<Policy> engine(in, policy);
        return engine.Run();
    }

    /// The game's allocation: scores of any sign.
    [[nodiscard]] inline Output Allocate(const Input& in)
    {
        const LogScorePolicy policy = LogScorePolicy::From(in.settings);
        return Allocate(in, policy);
    }

    /// PullIntoEmptyJobKeys alone, on a page built by the caller, in its slot
    /// order. `in` gives the layout and the candidates the states point at.
    template <class Policy>
    [[nodiscard]] std::vector<SlotState> PullIntoEmptyJobKeys(const Input& in, std::vector<SlotState> state,
        const std::array<std::size_t, kMaxSlots>& order, std::size_t priorityCount, const Policy& policy,
        std::vector<Event>* events = nullptr)
    {
        Detail::Engine<Policy> engine(in, policy);
        return engine.PullOnly(std::move(state), order, priorityCount, events);
    }

    /// The candidates the class cap kept off the page: noted as passed over
    /// and not shown in the end (SlotClassCap::Summary's set), as formIDs,
    /// sorted.
    [[nodiscard]] inline std::vector<std::uint32_t> KeptOff(const Input& in, const Output& out)
    {
        std::vector<std::uint32_t> kept;
        for (const std::uint32_t i : out.skipped) {
            const std::uint32_t id = in.candidates[i].formID;
            const bool shown = std::any_of(out.slots.begin(), out.slots.end(), [&](const SlotState& s) {
                if (s.IsEmpty()) return false;
                const std::uint32_t formID = s.kind == Kind::Override ? in.overrides[s.src].formID : in.candidates[s.src].formID;
                return formID == id;
            });
            if (!shown) kept.push_back(id);
        }
        std::sort(kept.begin(), kept.end());
        return kept;
    }
}  // namespace Huginn::Core::SlotAlloc
