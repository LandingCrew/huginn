#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/ChallengerRule.h"
#include "core/SlotClassCapMath.h"
#include "core/SlotScoreMath.h"
#include "core/WildcardPick.h"

// =============================================================================
// SLOT ALLOCATION CORE (pure; host-tested in tests/core/SlotAlloc*Tests.cpp)
// =============================================================================
// The decision logic of Slot::SlotAllocator (src/slot/SlotAllocator.cpp) over
// plain records: overrides, Remembrance holds, the wildcard key (R10), the
// slot hold, the rank-ordered fill, the class cap, seating and home keys,
// filling job keys from Regular keys. SlotAllocator builds an Input from the game's types (SlotSnapshot.h),
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
// sets. The comments that say WHY a rule exists, and which play-test made it,
// moved here with the code (more in docs/architecture/5-slots.md).
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
// and optionally (R9, LogScorePolicy):
//   bool  ExceedsBelief(Score c, Score h, double sigmaDelta);   the Bayesian
//         challenger rule (core/ChallengerRule.h), used by the hold when the
//         Input carries the scorer's beliefs for both rows
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
        double sd = 0.0;                  // the score's sd (choice posterior; 0 in the fallback): the wildcard draw (R10)
        bool isRememberedOnly = false;
        bool isEquipped = false;          // CandidateBase::isEquipped
        bool playerEquipped = false;      // player->IsItemEquipped(formID)
        std::uint32_t matchMask = 0;      // bit k: SlotClassifier::Matches(c, k), for the layout's classes
        std::uint8_t capClass = 0;        // SlotClassCap::ClassOf(c)
        /// R9: this row's index in Input::beliefs (the ranking run's belief
        /// table); kNone = no belief (the fallback ranking, a remembered-only
        /// row, a test that states utilities). Not in the snapshot format.
        std::uint32_t beliefRow = kNone;
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
        /// R9, the challenger rule with beliefs (core/ChallengerRule.h):
        /// tau (fChallengerConfidence) and ln m (fChallengerMinGap). Not in
        /// the snapshot format: the recorded snapshots carry no beliefs, so
        /// their hold is the fixed margin above, which these do not touch.
        float challengerConfidence = static_cast<float>(Challenger::kDefaultConfidence);
        float challengerMinGap = 0.0f;
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

        /// R10: the page's wildcard key (the seat: SeatOf). WildcardManager
        /// keeps the cache and the clocks; the allocation places and draws.
        struct WildcardIn
        {
            std::uint32_t pinned = kNone;   // the cached wildcard's row (a candidate index), shown on the seat
            bool roll = false;              // nothing cached and the clocks allow a roll: roll for one
            double uRoll = 1.0;             // uniforms in [0, 1): the roll, then the draw
            double uDraw = 0.0;
            Core::Wildcard::Params params;
            /// Forms a slot lock holds on some key of this page (SlotLocker:
            /// a lock shows its item whatever the allocation says). Never
            /// drawn: the post-lock dedup would clear the wildcard as a second
            /// copy, and the rows just under the cut -- the likeliest draws --
            /// are the ones that just left a key and are still held there.
            std::vector<std::uint32_t> heldForms;
        } wildcard;

        /// R9: the ranking run's beliefs, which CandidateRec::beliefRow
        /// indexes. Null (the fallback ranking, the golden snapshots): the
        /// hold is the fixed margin, ln(1 + challengerMargin). Not owned; the
        /// caller keeps it alive for the allocation (SlotSnapshot.cpp: the
        /// scored list holds it).
        const Challenger::BeliefSource* beliefs = nullptr;
    };

    struct SlotState
    {
        Kind kind = Kind::Empty;
        std::uint32_t src = kNone;        // candidate index, or override index for Kind::Override
        bool seatMoved = false;
        friend bool operator==(const SlotState&, const SlotState&) = default;

        [[nodiscard]] bool IsEmpty() const noexcept { return kind == Kind::Empty; }
        /// Placed by a rule for its slot, not by rank: an override, a
        /// Remembrance hold, and (R10) the wildcard on its seat. Seating never
        /// moves one, and one claims no seat (RecordSeating).
        [[nodiscard]] bool IsPinned() const noexcept
        {
            return kind == Kind::Override || kind == Kind::Remembered || kind == Kind::Wildcard;
        }
    };

    /// What became of the cached wildcard (Input::WildcardIn::pinned).
    enum class PinFate : std::uint8_t
    {
        None,       // nothing was pinned
        Placed,     // shown on the seat
        SeatBusy,   // the seat (or the item) is taken by an override or a Remembrance hold this run: kept
        Refused,    // the seat no longer takes it (its class, or it is equipped under skip-equipped): drop it
    };

    /// R10: the wildcard pass's result.
    struct WildcardOut
    {
        std::size_t seat = npos;          // the page's wildcard key, npos when it has none
        PinFate pin = PinFate::None;
        bool rollAsked = false;           // Input::WildcardIn::roll, and the seat showed a ranked row (or none)
        bool rolled = false;              // ...and uRoll came up under pRoll
        std::uint32_t drawn = kNone;      // the row drawn and shown on the seat (rolled with a pool)
        // The seat's displaced row, when it outranked the weakest ranked row
        // on another key that takes it: it moved to that key (RehomeFor) and
        // that row left the page instead. npos / kNone when nothing moved.
        std::size_t movedTo = npos;       // the key the displaced row moved to
        std::uint32_t moved = kNone;      // the displaced row (a candidate index)
        std::uint32_t left = kNone;       // the row that left the page in its place
        std::uint32_t pool = 0;           // rows it was drawn from
        double pRoll = 0.0;               // P(roll) this run (WildcardPick RollProbability)
        double pDraw = 0.0;               // P(the draw shows the drawn row's form | rolled)
        double weight = 0.0;              // the drawn row's w, rel and sd
        double rel = 0.0;
        double sd = 0.0;
        double topWeight = 0.0;           // the pool's largest w
        [[nodiscard]] double Propensity() const noexcept { return pRoll * pDraw; }
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
        HoldGaveWay,              // a: slot, b: holder, c: challenger; x/y: their raw scores, z/w: their caps; capped; sd (R9)
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
        double sd = -1.0;                 // HoldGaveWay: sigma_Delta under the challenger rule; < 0 = the fixed margin decided
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
        WildcardOut wildcard;
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
        double minGap = 0.0;   // R9: ln m of the challenger rule
        double z = 0.0;        // R9: Phi^-1(tau)

        [[nodiscard]] static LogScorePolicy From(const Settings& s) noexcept
        {
            return { ClampClassCapDiscount(s.classDiscount), s.classFree, LogHoldMargin(s.challengerMargin),
                Challenger::ClampMinGap(static_cast<double>(s.challengerMinGap)),
                Challenger::ZForConfidence(static_cast<double>(s.challengerConfidence)) };
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
        [[nodiscard]] bool ExceedsBelief(Score c, Score h, double sigmaDelta) const noexcept
        {
            return Challenger::Exceeds(c, h, sigmaDelta, minGap, z);
        }
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

    /// R10: the page's wildcard key (map Phase 8: "a reserved wildcard slot
    /// per page, placed explicitly instead of a rank swap"). The LAST key in
    /// fill order (PriorityOrder: lowest iPriority, ties to the highest slot
    /// index) of those with bWildcardsEnabled -- on the shipped layouts key 8,
    /// the "Wildcard" key the Jobs page already names. Never the first key in
    /// fill order (the old "slot 0 is never a wildcard" rule): a page whose
    /// only wildcard-enabled key is its top one, or a one-key page, has none.
    /// npos when there is none. A player picks the key by turning
    /// bWildcardsEnabled off on the keys after it, or off everywhere.
    [[nodiscard]] inline std::size_t SeatOf(const std::vector<SlotRec>& slots)
    {
        std::array<std::size_t, kMaxSlots> order{};
        const std::size_t n = PriorityOrder(slots, order);
        for (std::size_t k = n; k-- > 1;) {
            if (slots[order[k]].wildcardsEnabled) return order[k];
        }
        return npos;
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
                    // R10: a wildcard is a placement (the seat), not a row's flag.
                    v = { c.formID, &c.name, c.dedupKey, c.matchMask, c.capClass,
                        s.kind == Kind::Wildcard, c.isEquipped, c.playerEquipped };
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
            /// item, uncapped, would have taken the slot from the holder
            /// (under the hold's own test, TakesKey).
            void DropSkipsSince(std::size_t mark, std::uint32_t holder, Score holderScore)
            {
                if (mark >= m_cap.skipped.size()) return;
                std::size_t w = mark;
                for (std::size_t r = mark; r < m_cap.skipped.size(); ++r) {
                    const std::uint32_t i = m_cap.skipped[r];
                    if (TakesKey(i, m_p.Raw(m_in.candidates[i]), holder, holderScore, nullptr)) {
                        m_cap.skipped[w] = m_cap.skipped[r];
                        m_cap.skippedIDs[w] = m_cap.skippedIDs[r];
                        ++w;
                    }
                }
                m_cap.skipped.resize(w);
                m_cap.skippedIDs.resize(w);
            }

            /// The slot hold's test: does challenger c (at score cs, its cap
            /// applied) take the key of holder h (at hs)? R9: when the Input
            /// carries the scorer's beliefs for both rows, the Bayesian
            /// challenger rule, mu_c - mu_i > ln m + z sigma_Delta
            /// (core/ChallengerRule.h), and *sd receives sigma_Delta; else
            /// (the fallback ranking, the golden snapshots, a policy without
            /// the rule) the fixed margin, c - h > ln(1 + fChallengerMargin).
            /// The class cap's terms are deterministic: they shift the two
            /// scores and add nothing to sigma_Delta.
            [[nodiscard]] bool TakesKey(std::uint32_t c, Score cs, std::uint32_t h, Score hs, double* sd) const
            {
                if constexpr (requires(const Policy& p, Score a, double v) { p.ExceedsBelief(a, a, v); }) {
                    if (m_in.beliefs) {
                        const std::uint32_t rc = m_in.candidates[c].beliefRow;
                        const std::uint32_t rh = m_in.candidates[h].beliefRow;
                        if (rc != kNone && rh != kNone) {
                            const double s = Challenger::SigmaDelta(m_in.beliefs->DeltaVar(rc, rh));
                            if (s == s) {
                                if (sd) *sd = s;
                                return m_p.ExceedsBelief(cs, hs, s);
                            }
                        }
                    }
                }
                return m_p.Exceeds(cs, hs);
            }

            std::size_t ComputePriorityOrder() { return PriorityOrder(m_in.slots, m_order); }

            /// SlotAllocator::FindBestCandidate (0.23.9): the best candidate for
            /// a slot. With the class cap on and a Regular slot, "best" is the
            /// highest score after the cap's term; otherwise the first match
            /// in list order -- the scorer's rank order, which is why the
            /// scorer sorts the WHOLE list since R7 (past a top-10 prefix the
            /// first match was not the best one).
            ///
            /// Checks, in order: rows only a Remembrance hold may place;
            /// anything already shown, by formID and by NAME (two enchanted
            /// copies with different formIDs and one name); the slot's class;
            /// and, under skipEquipped, both the candidate's own isEquipped
            /// (registry scan) and the player state's 100 ms equipment poll, to
            /// cover the gap between the two. R10: no wildcard rule here any
            /// more -- the wildcard is placed on its seat by its own pass
            /// (PlaceCachedWildcard, DrawWildcard), never found by rank.
            [[nodiscard]] std::optional<std::uint32_t> FindBest(const SlotRec& slot,
                const std::set<std::uint32_t>& ids, const std::set<std::string_view>& names,
                bool skipEquipped, bool useCap)
            {
                const bool capped = useCap && m_cap.active && slot.regular;
                std::uint32_t first = kNone;
                std::uint32_t best = kNone;
                Score bestScore{};
                const auto& cands = m_in.candidates;
                for (std::uint32_t i = 0; i < cands.size(); ++i) {
                    const auto& c = cands[i];
                    if (c.isRememberedOnly) continue;
                    if (ids.contains(c.formID)) continue;
                    if (names.contains(c.name)) continue;
                    if (!MaskMatches(c.matchMask, slot)) continue;
                    if (skipEquipped && (c.isEquipped || c.playerEquipped)) continue;
                    if (!capped) {
                        return i;
                    }
                    // Under the class cap: the first match wins outright
                    // unless its class is already full on the page; then
                    // every later match is weighed with its own cap term, to
                    // the end of the list.
                    if (best == kNone) {
                        const Cap cap = CapOf(c.capClass);
                        if (!m_p.IsCapped(cap)) {
                            return i;
                        }
                        first = best = i;
                        bestScore = m_p.Apply(m_p.Raw(c), cap);
                        continue;
                    }
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

            /// SlotAllocator::FindItemSlot: the slot on this page that shows the
            /// item with `key` -- its seat, or failing that where it stood last
            /// pass. npos if none (or seating is off, or the layout generation
            /// moved on).
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
            void PlaceCachedWildcard();
            void RehomeSeatOwner();
            void DrawWildcard();
            /// The key the seat's displaced row `d` should move to, or npos.
            [[nodiscard]] std::size_t RehomeFor(std::uint32_t d);

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

            // PASS 1w (R10): the cached wildcard on its seat, pinned like a
            // Remembrance hold -- after the overrides and holds, which outrank
            // it, before the slot hold and the fill, which then never see the
            // seat or the item. Replaces the rank swap and its special cases
            // in the hold and the fill (map Phase 8).
            PlaceCachedWildcard();

            // The class cap counts what overrides, Remembrance and the wildcard placed.
            Recount();

            // PASS 1c: the slot hold.
            if (set.keepSlotPositions && set.holdSeatedItems) {
                HoldIncumbents(priorityCount);
            }

            // PASS 2: the rank-ordered fill.
            Fill(priorityCount, /*refill=*/false);

            if (!set.keepSlotPositions && set.fillJobKeysFromRegular) {
                // Without seating there is no pass 4, so refill the Regular key
                // the pull just emptied here, or it stays blank (/code-review #151).
                PullIntoEmptyJobKeys(priorityCount);
                Recount();
                Fill(priorityCount, /*refill=*/true);
            }
            if (!set.keepSlotPositions) {
                // PASS 4w (R10): no seating below; the page is final here.
                DrawWildcard();
            }

            // PASS 3: seat returning items where they were (anti-juggling).
            // Passes 1 and 2 decided WHICH items the player sees; this decides
            // WHERE. Without it, one item arriving or leaving shifts every
            // item below it by a slot -- the recommendations stay right and
            // every key under the player's fingers changes meaning.
            if (set.keepSlotPositions) {
                ApplySeating();
                // Optional: a key with a job that would be blank takes a
                // matching item off a Regular key. After seating, so seating
                // does not undo it; before the refill, which fills the
                // Regular key.
                if (set.fillJobKeysFromRegular) {
                    PullIntoEmptyJobKeys(priorityCount);
                }
                // PASS 4: refill what pass 3 vacated. An item moving back to
                // its own seat can leave the slot it was sitting in empty, and
                // a gap in the middle of the widget is a worse trade than the
                // shuffle seating exists to prevent. The dedup sets are still
                // pass 2's, so this cannot re-place an item already shown.
                Recount();
                Fill(priorityCount, /*refill=*/true);
                // PASS 4p (R10): the pinned wildcard does not push the seat's
                // owner off the page when it outranks the weakest row shown.
                RehomeSeatOwner();
                // PASS 4w (R10): a roll draws the wildcard from what the
                // finished page does not show and puts it on the seat. Before
                // the record, so the seat's displaced owner is remembered as
                // departed (home keys bring it back when the wildcard ends)
                // and the wildcard, pinned, claims no seat.
                DrawWildcard();
                // PASS 5: remember the result for next time.
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

                // If the item is ALREADY on this page, the override marks that
                // slot -- label and pulse on the key the player already knows
                // -- instead of putting a second copy in its configured slot.
                // The copy was the worst churn left: dedup bounced the potion
                // between the two slots four times a second, and the item the
                // configured slot held was pushed out and evicted another
                // (2026-09-27 19:03:07, 19:08:56).
                //
                // Except the VITALS, which by default keep their configured
                // slots: in an emergency the key is muscle memory (health
                // 2026-09-27, the user's call; magicka and stamina when the
                // flagship page gave every key a job, 2026-09-28). [Overrides]
                // bPin{Health,Magicka,Stamina}ToSlot. The quieter prompts --
                // ammo, soul gem, drowning -- always mark in place.
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

        // PASS 1a: Remembrance -- what pressing a slot took off, held there.
        // A rule, not a ranking: it takes the slot the player pressed whatever
        // that slot's classification or skipEquipped says, and only an
        // override (placed above) outranks it. sRemembranceTarget = Job sends
        // an item that does not fit the pressed key to the first empty
        // swap-back key whose class fits it (a dagger taken off by the
        // attack-magic key goes to the Weapon key). Two sweeps, so a routed
        // item cannot take a key another hold was pressed on: the ones
        // staying put are placed first.
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
                            // Another hold was pressed there, and that key is
                            // its fallback: taking it could leave that item
                            // nowhere (/code-review #151).
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

        // PASS 1c: the slot hold. Before the rank-ordered fill, keep each
        // seated item in its own seat unless the slot no longer accepts it or
        // the best challenger FOR THAT SLOT beats it (TakesKey): from R9 by
        // the Bayesian challenger rule, mu_c - mu_i > ln m + z sigma_Delta,
        // when the scorer's beliefs are known; else by the fixed margin,
        // score difference > ln(1 + fChallengerMargin) (the old u_c > m u_i).
        // Only the best challenger is asked, as before R9: a lower one with
        // a narrower sigma_Delta that would clear the line waits until the
        // best one stops standing in its way (the conservative reading; the
        // no-swap-back argument holds either way).
        //
        // Two problems, one pass. Near-tied items were trading slots every
        // time a lock expired -- most slot changes in play were under 10%
        // better. And the fill runs in slot-priority order, so a high-priority
        // slot could take an item out of its seat further down (a WeaponsAny
        // slot pulling the bow out of slot 5 whenever the axe in slot 1 left),
        // which seating could not undo. Held items go into the assigned sets,
        // so the fill never sees them.
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

            // Phase A: which seat owners are still candidates, and still
            // allowed in their seat? An item the slot has given up -- now
            // equipped under skip-equipped, or no longer of its class -- is
            // not held, however well it scores: holding it would
            // keep something the slot itself just rejected. Priority order,
            // the order the fill uses: when two held slots want the same
            // challenger, the one the fill would have served first gets it.
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

            // Owners whose seat an override (or a remembered item) is sitting
            // in stay out of the challengers. Free to challenge, the Iron
            // Dagger an override pushed out of slot 1 beat Sparks in slot 4,
            // Sparks landed in slot 3, and it all ran backwards when the
            // override ended -- one override, six slot changes (2026-09-27
            // 19:08:30-37). It is still held where it stands (the guests
            // below), and goes home when the override ends. R10: the pinned
            // wildcard displaces its seat's owner the same way; an owner it
            // keeps off the page entirely is put back, in place of the weakest
            // row shown, by RehomeSeatOwner (pass 4p) when it outranks it.
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

            // Guests: an item standing in a slot that is not its seat, because
            // an override occupies the seat. Without this it was a free
            // challenger every pass -- the mace whose seat the health potion
            // held beat Wine in slot 5, then the sword in slot 1, then Wine
            // again, for half a minute (2026-09-26 14:28:49-14:29:20). Held
            // where it stands, under the same test, until the override ends
            // and seating takes it home. The displaced owners above are these
            // items, and are in the excluded sets: a guest is judged against
            // what is SHOWN, not against the exclusion (Raw Crab Meat waiting
            // for an override on its home key, LoreRim 2026-10-06 21:07:58).
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

            // The class cap over the holders TOGETHER, by score, before any is
            // judged. Counted slot by slot instead, a challenger for an early
            // slot could not see the holders of its class further down: the
            // axe took slot 0 while the sword and mace still held slots 1 and
            // 4, and the page showed four weapons (vanilla, 2026-10-06
            // 17:13:24). Each holder's cap is its rank in its class; a holder
            // being judged is taken out of the count, so a challenger of the
            // same class is weighed as it would be.
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

            // Phase B: each holder against the best challenger for its own
            // slot. Challengers exclude every other holder and every
            // challenger already given a slot this pass. A winner is placed
            // on the spot and reserved: leaving the placement to the fill let
            // two held slots lose to the SAME challenger (2026-09-26 14:04:35,
            // slots 3 and 6 both yielding to one Resist Cold). And the loser
            // gives up its seat, or "you keep your seat while you are on
            // screen" hands it straight back and the release repeats every run.
            for (std::size_t t = 0; t < tentativeCount; ++t) {
                const auto [j, item] = tentative[t];
                const auto& config = slots[j];
                const auto& it = cands[item];

                // R10: no wildcard case here. The wildcard is pinned on its
                // seat (pass 1w) before this pass, so it is never a holder or
                // a challenger: the old rank-swapped wildcard, low-scored by
                // design, needed an exemption from this comparison (2026-09-27
                // 19:02:40-46), and the pinned one cannot reach it.

                // On a Regular key both sides are weighed after the class cap:
                // a holder past its class's free count holds at its capped
                // score, or a crowd that formed before the cap applied would
                // be held there for good.
                m_cap.Remove(it.capClass);
                const std::size_t skipMark = m_cap.skipped.size();
                const auto challenger = FindBest(config, excludedIDs, excludedNames, config.skipEquipped,
                    /*useCap=*/true);
                const bool capped = config.regular;
                const Cap itemCap = capped ? holderCap[t] : m_p.NoCap();
                const Cap challengerCap = capped && challenger ? CapOf(cands[*challenger].capClass) : m_p.NoCap();
                const Score itemScore = m_p.Apply(m_p.Raw(it), itemCap);

                if (challenger) {
                    const auto& ch = cands[*challenger];
                    const Score challengerScore = m_p.Apply(m_p.Raw(ch), challengerCap);
                    double sd = -1.0;
                    if (TakesKey(*challenger, challengerScore, item, itemScore, &sd)) {
                        m_cap.Add(ch.capClass);
                        Event e{ EventKind::HoldGaveWay, static_cast<std::uint32_t>(j), item, *challenger };
                        e.x = Policy::ToDouble(m_p.Raw(it));
                        e.y = Policy::ToDouble(m_p.Raw(ch));
                        e.z = Policy::ToDouble(itemCap);
                        e.w = Policy::ToDouble(challengerCap);
                        e.sd = sd;
                        e.capped = m_p.IsCapped(itemCap) || m_p.IsCapped(challengerCap);
                        Note(e);

                        Place(j, Kind::Normal, *challenger);
                        Assign(ch.formID, ch.name);
                        excludedIDs.insert(ch.formID);
                        excludedNames.insert(ch.name);

                        // The loser is free again -- the fill may show it
                        // elsewhere -- but not as this seat's owner. A guest's
                        // seat is somewhere else and not its to give up. An
                        // owner an override displaced stays out of the
                        // challengers even so (code review of #179).
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

                Place(j, Kind::Normal, item);
                // A home claimant arriving at its key from where it waited:
                // the lock still showing it there may let go (SlotLocker).
                {
                    const std::uint64_t key = it.dedupKey;
                    m_state[j].seatMoved = claims[j] == key && placed[j] != key;
                }
                Assign(it.formID, it.name);
                m_cap.Add(it.capClass);   // back in the count it left to be judged
                // The search was hypothetical: an item it passed for the cap
                // was kept off by the cap only if, uncapped, it would have
                // taken the slot from this holder.
                DropSkipsSince(skipMark, item, itemScore);
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

                // Passes 2b and 4 (refill) note nothing; pass 2 notes a key
                // nothing fits. R10: no wildcard re-run -- the fill places
                // ranked rows only (the wildcard has its own passes).
                const auto best = FindBest(config, m_ids, m_names, config.skipEquipped, /*useCap=*/true);
                if (!best) {
                    if (!refill) Note({ EventKind::NoCandidate, static_cast<std::uint32_t>(idx) });
                    continue;
                }
                const auto& c = m_in.candidates[*best];
                Place(idx, Kind::Normal, *best);
                Assign(c.formID, c.name);
                m_cap.Add(c.capClass);
            }
        }

        // Put items back in the slots they were in last pass, where the layout
        // still allows it. Runs AFTER the rank-ordered fill, so it never
        // changes WHICH items are shown -- only where they sit. Overrides and
        // Remembrance holds are pinned (placed by a rule for that slot). It can
        // leave a slot empty, by moving its occupant back to the seat it
        // wants; pass 4 refills those, so seating never opens a hole in the
        // middle of the widget.
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

            // Which slot does this item want? At most one seat per key and one
            // key per seat, so no two items can want the same slot -- the
            // mapping is injective by construction, and nothing arbitrates.
            auto seatWantedBy = [&](std::uint64_t key) -> std::size_t {
                if (key == 0) return npos;
                for (std::size_t j = 0; j < slotCount; ++j) {
                    if (seats[j] == key) return j;
                }
                return npos;
            };
            // An override sits where the override pass put it, a remembered
            // item under the key the player pressed: nothing moves them, and
            // nothing moves into them.
            auto movable = [&](std::size_t idx) { return !m_state[idx].IsPinned(); };
            // Only a home claimant's move is marked (code review of #179):
            // every other seating move waits out the lock on its old slot.
            auto moveTo = [&](std::size_t from, std::size_t to) {
                const bool claimant = claims[to] != 0 && claims[to] == KeyOf(m_state[from]);
                m_state[to] = m_state[from];
                m_state[to].seatMoved = claimant;
                m_state[from] = SlotState{};
            };

            // Phase 0: home keys. A seat is freed the moment its owner leaves
            // the screen, so an item that drops off for a few seconds came
            // back to whatever key was open: 1,073 of 1,365 returns within ten
            // minutes landed on a different key (21 logs, 2026-09-30 to
            // 10-06), most of them within a minute.
            //
            // A RETURNER is on screen with no seat and left a slot of this
            // page within fHomeKeyMemorySec. It takes that slot's seat back
            // from the item that filled the gap (the user's rule, 2026-10-06:
            // whoever arrived after the returner left only filled the gap),
            // and the phases below move it there -- into the slot if empty,
            // else by the phase-2 swap. Only where the move can happen: the
            // home slot takes the returner and its occupant fits the
            // returner's slot; otherwise the returner keeps the key it landed
            // on, which becomes its home -- a recommendation is never hidden
            // to wait for a key. Two returners for one slot: the later leaver
            // wins.
            //
            // Blocked only by an override or a Remembrance hold, the returner
            // gets the right of first refusal (the user, 2026-10-06): it
            // claims the seat and waits, shown where it landed; the slot hold
            // seats it when the slot frees. 17 of 22 misses on LoreRim's first
            // run were this case. Not when the pinned item IS that slot's seat
            // owner -- an override marking its own seat (code review of #179):
            // taking the seat would make the override jump mid-emergency.
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
                // Written back now, not left to RecordSeating: that keeps "you
                // keep your seat while on screen", and the gap-filler -- still
                // on screen -- would otherwise keep the seat it just lost.
                // Every returner's departure is spent, whatever the outcome
                // (code review of #179): one kept would make the item a
                // returner again on every pass while it stays seatless.
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

            // Phase 1: moves into empty seats, repeated. The common case is a
            // chain, not a swap: one item leaves, everything below it moves up
            // a slot, and putting them back starts with the last one dropping
            // into the hole at the bottom, which frees the seat the one above
            // it wants, and so on up. One round per slot is enough for any
            // chain; the loop stops as soon as nothing moved.
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

            // Phase 2: two items holding each other's seats -- what a pure
            // reorder of an unchanged set looks like; a chain cannot resolve
            // it. Repeated: a swap can throw an item BELOW the index the sweep
            // has passed (2026-09-19 23:53:38, the Iron Sword left in the
            // wrong slot for 3.5 s). It terminates: a swap fires only when the
            // item at i can reach its OWN seat, and the seat map is injective,
            // so each swap raises the number of items in their own seat.
            // Only when BOTH are legal in the other's slot.
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

        // PASS 5: remember where everything ended up, for the next allocation
        // of this page. The rule is one sentence: you keep your seat for as
        // long as you are on screen, and you only get a new one if you do not
        // have one. Stated the other way round -- "record where everything
        // ended up" -- every reason an item could not reach its seat this pass
        // became a permanent move: an override pins a slot for a second and
        // the item that lives there is rehomed for the session, and the
        // cascade reached items no override touched (the BOW, 2026-09-19
        // 23:31:23-26). Both loops write at most one slot per key and one key
        // per slot, so the map stays injective (ApplySeating relies on it).
        // An item that left the screen entirely is in neither loop, so its
        // seat is not carried over -- which is how a seat is ever freed.
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
            // Anyone on screen but not in their own seat keeps the claim they
            // had (override guests included). An override that marked its
            // item's OWN seat keeps it too, or the seat would lapse and the
            // override jump to its configured slot.
            for (std::size_t i = 0; i < slotCount; ++i) {
                const std::uint64_t key = KeyOf(m_state[i]);
                if (key == 0) continue;
                const std::size_t home = previousSeatOf(key);
                if (home != npos && (home != i || m_state[i].IsPinned())) {
                    seats[home] = key;
                }
            }
            // Anyone in their own seat keeps it, and an item with no seat
            // takes the one it is sitting in -- unless someone displaced still
            // owns it. A pinned item claims nothing: it is passing through.
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

            // An item that left the screen entirely is remembered against the
            // slot it owned, for home keys -- or, with no seat, the slot it
            // stood in last pass: an item the slot hold outranked loses its
            // seat on the spot, and without the fallback the item home keys
            // exist for was never remembered (code review of #179).
            auto onScreen = [&](std::uint64_t key) {
                return std::any_of(m_state.begin(), m_state.end(),
                    [&](const SlotState& s) { return KeyOf(s) == key; });
            };
            // Not from a slot a home claimant has claimed: an item that lost
            // the seat to the claim only filled the gap, and remembering it
            // there let it come back and take the seat from the claimant
            // still waiting for it (RunHomeKeyTest, 2026-10-06 21:56:10).
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

            // A home claim ends when its item sits in the slot, or loses the seat.
            for (std::size_t j = 0; j < slotCount; ++j) {
                if (claims[j] != 0 && (seats[j] != claims[j] || KeyOf(m_state[j]) == claims[j])) {
                    claims[j] = 0;
                }
            }

            m_out.generationMatches = true;
            mem.seats = seats;
            mem.lastPlaced = placedNow;
        }

        // [SlotLocker] bFillJobKeysFromRegular: an empty key with a class takes
        // the best-scoring matching item standing on a Regular key (never a
        // pinned item or a wildcard).
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

        // PASS 1w (R10): the cached wildcard on its seat. WildcardManager
        // holds which item it is (for fCooldownSeconds) and names its row; this
        // places it, pinned, where the seat is still free. It does not place a
        // row the seat no longer takes (its class; equipped under
        // skip-equipped: the player has it in hand, so the exploration is
        // over) -- PinFate::Refused, and the manager drops it rather than let
        // a cache nothing can show block the next roll. An override or a
        // Remembrance hold on the seat, or showing the item, wins for this run
        // (SeatBusy): the manager keeps the wildcard for when they let go.
        // The seat's owner is not pushed off the page by it while it outranks
        // the weakest row shown (RehomeSeatOwner, after the refill).
        template <class Policy>
        void Engine<Policy>::PlaceCachedWildcard()
        {
            auto& w = m_out.wildcard;
            w.seat = SeatOf(m_in.slots);
            const auto& wi = m_in.wildcard;
            if (wi.pinned == kNone) return;
            if (w.seat == npos || w.seat >= m_state.size() || wi.pinned >= m_in.candidates.size()) {
                w.pin = PinFate::Refused;
                return;
            }
            const auto& c = m_in.candidates[wi.pinned];
            if (!m_state[w.seat].IsEmpty() || m_ids.contains(c.formID) || m_names.contains(c.name)) {
                w.pin = PinFate::SeatBusy;
                return;
            }
            const SlotState s{ Kind::Wildcard, wi.pinned, false };
            if (c.isRememberedOnly || !SlotAccepts(m_in.slots[w.seat], s)) {
                w.pin = PinFate::Refused;
                return;
            }
            Place(w.seat, Kind::Wildcard, wi.pinned);
            Assign(c.formID, c.name);
            w.pin = PinFate::Placed;
        }

        // The key the seat's displaced row d moves to, or npos (it leaves the
        // page). The wildcard takes the seat, not the place of whatever row
        // happens to sit there: with seating, the seat's owner is whoever got
        // there first and the challenger rule keeps it (R9), so it can be the
        // page's best row -- which the old engine never let a wildcard take
        // ("slot 0 is always excluded (top-scored pick)"). So d moves to the
        // key of the weakest ranked row that takes it, and that row leaves
        // instead, when d outranks it (each after the class cap, with both
        // out of the count, as the slot hold weighs a holder). One key
        // changes besides the seat, and the page loses its weakest row, not
        // an arbitrary one. A key whose row a slot lock holds anyway
        // (WildcardIn::heldForms) is passed over: the lock would keep that
        // row and d would be shown nowhere until it let go.
        template <class Policy>
        std::size_t Engine<Policy>::RehomeFor(std::uint32_t d)
        {
            const auto& w = m_out.wildcard;
            const auto& slots = m_in.slots;
            const auto& cands = m_in.candidates;
            const std::size_t slotCount = std::min(m_state.size(), std::min(slots.size(), kMaxSlots));
            if (d >= cands.size()) return npos;
            const SlotState moving{ Kind::Normal, d, false };
            const auto& held = m_in.wildcard.heldForms;

            // The weakest: lowest raw score; ties to the key filled later.
            std::size_t to = npos;
            for (std::size_t k = slotCount; k-- > 0;) {
                const std::size_t j = m_order[k];
                if (j >= slotCount || j == w.seat || m_state[j].kind != Kind::Normal) continue;
                const auto& row = cands[m_state[j].src];
                if (std::find(held.begin(), held.end(), row.formID) != held.end()) continue;
                if (!SlotAccepts(slots[j], moving)) continue;
                if (to == npos || m_p.RawGreater(m_p.Raw(cands[m_state[to].src]), m_p.Raw(row))) to = j;
            }
            if (to == npos) return npos;

            const auto& dc = cands[d];
            const auto& wc = cands[m_state[to].src];
            Recount();
            const bool dOnPage = m_state[w.seat].kind == Kind::Normal && m_state[w.seat].src == d;
            m_cap.Remove(wc.capClass);
            if (dOnPage) m_cap.Remove(dc.capClass);
            const bool capped = slots[to].regular;
            const Cap dCap = capped ? CapOf(dc.capClass) : m_p.NoCap();
            const Cap wCap = capped ? CapOf(wc.capClass) : m_p.NoCap();
            Recount();
            return m_p.Greater(m_p.Apply(m_p.Raw(dc), dCap), m_p.Apply(m_p.Raw(wc), wCap)) ? to : npos;
        }

        // PASS 4p (R10): the pinned wildcard and the seat's owner. Pinned
        // before the hold, the wildcard keeps the seat's owner out of it, and
        // the hold keeps that owner from challenging any other key (an owner
        // whose seat is pinned is a displaced owner, HoldIncumbents): with
        // the other keys held, it had no way back on the page, best row or
        // not. On the run a roll draws, DrawWildcard moves it itself, and on
        // later runs it is held where it moved to (a guest); this catches the
        // rest -- the owner the page does not show, while it outranks the
        // weakest row shown. Seating only: without it there is no owner, and
        // the fill already gave the other keys the best rows.
        template <class Policy>
        void Engine<Policy>::RehomeSeatOwner()
        {
            auto& w = m_out.wildcard;
            if (w.pin != PinFate::Placed || w.seat >= kMaxSlots) return;
            if (!m_in.memoryAvailable || !m_out.generationMatches) return;
            const std::uint64_t key = m_out.memory.seats[w.seat];
            if (key == 0) return;
            const auto& cands = m_in.candidates;
            std::uint32_t d = kNone;
            for (std::uint32_t i = 0; i < cands.size(); ++i) {
                if (!cands[i].isRememberedOnly && cands[i].dedupKey == key) { d = i; break; }
            }
            if (d == kNone || m_ids.contains(cands[d].formID) || m_names.contains(cands[d].name)) return;
            const std::size_t to = RehomeFor(d);
            if (to == npos) return;
            w.movedTo = to;
            w.moved = d;
            w.left = m_state[to].src;
            Place(to, Kind::Normal, d);
            Assign(cands[d].formID, cands[d].name);
        }

        // PASS 4w (R10): the roll. With nothing cached and the clocks open
        // (WildcardManager), on a finished page whose seat shows a ranked row
        // or nothing: the pool is every ranked row the page does not show
        // (by formID and by name, as the fill dedups) that the seat takes,
        // less what a slot lock holds (WildcardIn::heldForms). Each weighs
        // rel x sd against the row the wildcard puts off the page: the seat's
        // occupant, or the weaker row whose key the occupant moves to
        // (RehomeFor) (core/WildcardPick.h). The roll comes up with P(roll)
        // from the pool's top weight, and the draw puts the row it picks on
        // the seat. The scores the weights read are the rows' own (mu), never
        // the policy's capped ones: the class cap shapes the page, not the
        // wildcard's odds.
        template <class Policy>
        void Engine<Policy>::DrawWildcard()
        {
            auto& w = m_out.wildcard;
            const auto& wi = m_in.wildcard;
            if (!wi.roll || w.seat == npos || w.seat >= m_state.size() || w.pin == PinFate::Placed) return;
            const SlotState occupant = m_state[w.seat];
            if (occupant.IsPinned()) return;   // an override or a hold has the seat: roll on a later run
            w.rollAsked = true;

            constexpr double kHuge = std::numeric_limits<double>::infinity();
            double muCut = -kHuge;   // an empty seat with nothing ranked shown: no cut
            std::size_t moveTo = npos;
            if (occupant.kind == Kind::Normal) {
                moveTo = RehomeFor(occupant.src);
                const std::uint32_t leaving = moveTo == npos ? occupant.src : m_state[moveTo].src;
                muCut = m_in.candidates[leaving].score;
            } else {
                double lowest = kHuge;
                for (const auto& s : m_state) {
                    if (s.kind == Kind::Normal) lowest = std::min(lowest, m_in.candidates[s.src].score);
                }
                if (lowest < kHuge) muCut = lowest;
            }

            std::set<std::uint32_t> shownIDs;
            std::set<std::string_view> shownNames;
            for (const auto& s : m_state) {
                if (s.IsEmpty()) continue;
                const ItemView v = View(s);
                shownIDs.insert(v.formID);
                shownNames.insert(*v.name);
            }
            const auto& held = wi.heldForms;

            const auto& seat = m_in.slots[w.seat];
            std::vector<std::uint32_t> rows;
            std::vector<std::uint32_t> forms;
            std::vector<double> rels;
            std::vector<double> weights;
            for (std::uint32_t i = 0; i < m_in.candidates.size(); ++i) {
                const auto& c = m_in.candidates[i];
                if (c.isRememberedOnly || !std::isfinite(c.score)) continue;
                if (shownIDs.contains(c.formID) || shownNames.contains(c.name)) continue;
                if (std::find(held.begin(), held.end(), c.formID) != held.end()) continue;
                if (!SlotAccepts(seat, SlotState{ Kind::Normal, i, false })) continue;
                const double rel = Core::Wildcard::Relevance(c.score, muCut);
                rows.push_back(i);
                forms.push_back(c.formID);
                rels.push_back(rel);
                weights.push_back(Core::Wildcard::Weight(rel, c.sd));
            }
            for (const double x : weights) w.topWeight = std::max(w.topWeight, x);
            w.pool = static_cast<std::uint32_t>(rows.size());
            w.pRoll = Core::Wildcard::RollProbability(wi.params, w.topWeight);
            w.rolled = wi.uRoll < w.pRoll;
            if (!w.rolled || rows.empty()) return;

            const auto probs = Core::Wildcard::Distribution(weights, wi.params.uniformShare);
            const std::size_t k = Core::Wildcard::Draw(probs, wi.uDraw);
            if (k >= rows.size()) return;
            w.drawn = rows[k];
            w.pDraw = Core::Wildcard::FormShare(probs, forms, forms[k]);
            w.weight = weights[k];
            w.rel = rels[k];
            w.sd = m_in.candidates[rows[k]].sd;
            if (moveTo != npos) {
                w.movedTo = moveTo;
                w.moved = occupant.src;
                w.left = m_state[moveTo].src;
                Place(moveTo, Kind::Normal, occupant.src);
            }
            Place(w.seat, Kind::Wildcard, rows[k]);
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
