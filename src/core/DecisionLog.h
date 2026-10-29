#pragma once

// =============================================================================
// DECISION LOG (R4) -- selection log v3: the records and their text form
// =============================================================================
// What the offline fit (R6) and the new learner (R8) read: one record per
// player decision, with the situation (need vector), every item the player
// could have chosen (one row per item, sparse cap(i), the runtime
// cross-features), the page as shown and the explicit outcome:
//
//   key      a Huginn key on the page          (doc 9: a choice from A)
//   wheel    an entry on Huginn's Wheeler wheel (the same page, another device)
//   menu     anything else the player selected: the inventory, magic or
//            favourites menu, a vanilla favourites hotkey, the player's own
//            Wheeler wheels -- a pick from H \ A at the menu cost kappa
//   nothing  a need episode ended with no selection (core/NeedEpisodes.h);
//            since 0.23.51 also each lapped stretch of a still-open episode
//            with no selection inside it (ep.why = "lap")
//
// The schema, field by field, with units and versioning:
// docs/architecture/9-selection-log-v3.md. tools/replay/replay.py reads it.
//
// Four line types, one JSON object per line (JSON Lines):
//   head  first line of every file segment: the version, the column, need,
//         cross-feature, kind and source names the ids below index into
//   cap   one cap(i), by a segment-local id; written once per segment before
//         the first context that uses it (caps are content-addressed: two
//         items with the same effects share one id, and a dynamic FormID
//         that changes meaning after a reload cannot mislabel one)
//   ctx   one logged context: the need vector, the page and the rows;
//         written once per segment before the first decision that uses it
//         (the presses of one menu visit share the menu's context; the
//         "nothing" records of needs that started on one tick share the onset's)
//   dec   one decision, referring to its context by id
//
// 0.23.53: every held or eligible row also carries the old engine's score
// parts whatever its floors (OldScore.h: the learner's est, confidence,
// evidence and UCB, the recency boost, the prior, the context weight, the
// floor, an unscored candidate's would-be utility), the favourite bit and the
// vanilla hotkey; a context the run's scorer parameters and state features
// ("old"); a pick the id of its v2 record ("rid").
//
// This file is the pure part: the plain records, the encoder that turns them
// into lines (tracking which caps and contexts the current segment has
// defined) and the JSON helpers. The game side (learning/SelectionLogV3*)
// builds the records and writes the lines on a background thread.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "Coverage.h"
#include "EffectMapper.h"
#include "FilterReason.h"
#include "NeedEvaluator.h"
#include "OldScore.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Huginn::Core::DecisionLog
{
    inline constexpr int kVersion = 3;

    enum class Outcome : std::uint8_t { Key, Wheel, Menu, Nothing };
    [[nodiscard]] std::string_view OutcomeName(Outcome o) noexcept;

    // --- The runtime cross-features (core/CrossFeatures.h), a row's "x" -----
    enum class Cross : std::uint8_t
    {
        overshoot_health,
        overshoot_magicka,
        overshoot_stamina,
        weapon_charge,
        stack_count,
        ammo_matches_launcher,
        school_fortified,
        /// 0.23.46: how much of the item's effect is already active on the
        /// player (core/Coverage.h; doc 11 step 3). Logged only, and not an
        /// effects.csv column: R8 gives it one shared weight, not a pair.
        covered,
        _Count
    };
    inline constexpr std::size_t kCrossCount = static_cast<std::size_t>(Cross::_Count);
    /// The effects.csv column each cross-feature fills (same names);
    /// Col::_Count for one with no column (covered).
    inline constexpr std::array<Effect::Col, kCrossCount> kCrossColumns{
        Effect::Col::overshoot_health, Effect::Col::overshoot_magicka, Effect::Col::overshoot_stamina,
        Effect::Col::weapon_charge,    Effect::Col::stack_count,       Effect::Col::ammo_matches_launcher,
        Effect::Col::school_fortified, Effect::Col::_Count,
    };

    // --- A row's flags --------------------------------------------------------
    namespace Flag
    {
        inline constexpr std::uint16_t Eligible = 1;       // a candidate of the last pipeline run (no floor)
        inline constexpr std::uint16_t Scored = 2;         // ...that also passed the old engine's floors (util set)
        inline constexpr std::uint16_t Held = 4;           // carried, or a spell the player knows
        inline constexpr std::uint16_t Equipped = 8;       // in a hand, worn, or the nocked ammo
        inline constexpr std::uint16_t Shown = 16;         // on the page the player saw (slot = its key)
        inline constexpr std::uint16_t Wildcard = 32;      // shown as a wildcard (wp = its propensity)
        inline constexpr std::uint16_t Override = 64;      // shown by an override
        inline constexpr std::uint16_t Remembered = 128;   // shown by a Remembrance hold
        inline constexpr std::uint16_t AddedAtPick = 256;  // the chosen item, missing from the context
        /// 0.23.53: in the player's favourites (an item's ExtraHotkey, a spell
        /// in MagicFavorites), or a candidate the old engine read as favourited.
        inline constexpr std::uint16_t Favorite = 512;
    }

    inline constexpr std::uint8_t kNoKind = 255;    // not in the effect catalog
    inline constexpr std::uint8_t kNoSource = 255;  // not a candidate of the old engine
    inline constexpr std::uint8_t kNoFilter = 255;  // not dropped by a candidate filter
    inline constexpr float kNone = std::numeric_limits<float>::quiet_NaN();
    inline constexpr std::int8_t kNoHotkey = -1;

    /// One item the player could have chosen.
    struct Row
    {
        std::uint32_t form = 0;
        std::uint16_t uid = 0;              // the stack's ExtraUniqueID (weapons, armour), else 0
        std::uint8_t kind = kNoKind;        // Effect::Kind
        std::uint8_t src = kNoSource;       // the old engine's candidate source type (names in the head)
        std::uint16_t flags = 0;
        std::int8_t slot = -1;              // the key it was shown on, -1 = off the page
        float util = kNone;                 // the old engine's utility (Scored rows), else none
        std::array<float, kCrossCount> cross{};
        float wildcardP = kNone;            // P(this item was rolled as the wildcard it is shown as)
        /// 0.23.46, row column "fb": the candidate filter that dropped the item
        /// in the last pipeline run (a Core::Filter::Reason), for a row that
        /// is not eligible; kNoFilter otherwise (eligible, or never gathered).
        std::uint8_t filteredBy = kNoFilter;
        /// 0.23.46, row column "cr": the remaining seconds of the effect that
        /// gave `covered` (Coverage::Result::remaining; -1 = no timer); none
        /// when the row is not covered.
        float coveredSec = kNone;
        /// 0.23.53: the old engine's score parts (row columns est..wu, fl).
        OldScore old;
        /// 0.23.53, row column "hk": the vanilla favourites hotkey bound to
        /// the item (0-7 = keys 1-8), kNoHotkey when none.
        std::int8_t hotkey = kNoHotkey;
        /// cap(i): the catalog's static entry (immutable, process lifetime) or
        /// a per-instance entry owned by capOwner. nullptr = not in the catalog.
        const Effect::Cap* cap = nullptr;
        std::shared_ptr<const Effect::Cap> capOwner;
    };

    /// A situation as logged: what the player saw and could choose from.
    struct Context
    {
        std::uint64_t id = 0;       // unique within the launch
        std::string utc;            // "YYYY-MM-DD HH:MM:SS.mmm" when taken
        double tSec = 0.0;          // steady seconds when taken (the caller's clock; not written)
        std::string why;            // "press", "menu" (a selection menu opened), "onset", "lap" (0.23.51)
        std::string menu;           // the menu's name for why = "menu"
        Needs::NeedArray need{};    // the need vector (curve outputs, 0..1)
        Needs::NeedArray input{};   // what went into each curve (needs.csv r3_input)
        bool pipeValid = false;     // a pipeline run had been cached at all
        int page = -1;              // the page shown
        int pageSlots = 0;          // keys on that page
        float pipeAgeMs = 0.0f;     // how old that page was when the context was taken
        std::string race;           // the hostile primary target's race editor ID, if any
        float wildcardBase = 0.0f;  // the wildcard settings (base, max probability per slot)
        float wildcardMax = 0.0f;
        /// The held rows were read in full: per-stack unique IDs, per-instance
        /// caps, charges and worn armour. False inside the post-load window in
        /// which inventory extra data must not be read (Util::IsExtraListStable):
        /// then the held rows are one plain row per base form, "equipped" is
        /// read from the hands and the nocked ammo only, and armour reads unworn.
        bool heldFull = true;
        /// Where and when (0.23.41): what the darkness need's input cannot
        /// say, for the fit to condition on. Written as "env" when valid.
        struct Env
        {
            bool valid = false;
            bool interior = false;      // an interior cell
            float hour = 0.0f;          // game hour, 0-24
            bool openDaylight = false;  // outdoors, full sky, climate daytime (darkness reads 0)
            float light = -1.0f;        // the raw light on the player, 0-1, before the hold
        } env;
        /// The player's summons (0.23.46; doc 11 steps 3-4): the count the
        /// summon filter compares against the limit, the commanded-actor limit
        /// the perks give, and every living commanded actor with the spell or
        /// item behind it. Written as "summon" when valid.
        struct Summon
        {
            bool valid = false;
            int count = 0;                    // living actors the player commands (the filter's count)
            float limit = 1.0f;               // the commanded-actor limit (perk entry point)
            std::vector<Coverage::CommandedActor> list;
        } summon;
        /// The old engine's run behind the eligible rows (0.23.53, OldScore.h);
        /// written as "old" when valid.
        OldRun old;
        /// 0.23.55 (R8.1): the left(r) centre ([Scoring] fRefreshPriorSec)
        /// the rows' covered(i) was computed with, read when the context was
        /// taken. Not written: the writer starts a new segment when it differs
        /// from the head's, so a hot reload between capture and write cannot
        /// put a row under a head naming another centre.
        float refreshPriorSec = 5.0f;
        std::vector<Row> rows;
    };

    /// One decision.
    struct Decision
    {
        std::uint64_t seq = 0;      // per launch, in write order (the writer assigns it)
        std::string utc;            // when the record was made (confirmation; expiry + grace for nothing)
        std::string launch;         // UTC start of the game launch, "YYYYMMDD-HHMMSS"
        std::string list;           // modlist folder
        std::uint64_t character = 0;
        std::uint32_t gen = 0;      // the load within the launch
        Outcome outcome = Outcome::Nothing;
        std::uint32_t form = 0;     // the item chosen (0 for nothing)
        std::string name;           // its name
        int row = -1;               // its row: an index into ctx.rows, then into `added`
        std::vector<Row> added;     // rows not in the context (the chosen item, when missing)
        std::string src;            // the old label: Hotkey / Wheeler / External
        std::string via;            // how: "key 3 (s2)", "inventory menu", "own wheel"...
        std::string caseLabel;      // the old A-E attribution of an outside pick
        std::string how;            // what confirmed it: "consumed", "still equipped", "used"
        std::string kind;           // "consume" / "equip"
        float confirmMs = 0.0f;     // press -> confirmation
        bool repeat = false;        // the same item equipped again within the repeat window
        bool learned = true;        // the frozen learner took it
        std::string skip;           // why it did not: "stale", "disabled", "armour"
        std::shared_ptr<const Context> ctx;
        float ctxAgeMs = 0.0f;      // press time minus the context's time
        double pressSec = 0.0;      // steady seconds at the press (not written; the caller's)
        std::vector<std::uint8_t> open;  // needs with an open episode at the press
        /// Was the chosen item equipped BEFORE the press? A press's context is
        /// taken after its own equip, so the chosen row's Equipped flag can
        /// show the pick itself; this is the state from the last update tick
        /// before the press (hands and nocked ammo; 0 for anything else, which
        /// an equip event shows was not worn).
        bool preEquipped = false;
        /// 0.23.53: the record id shared with this pick's v2 record
        /// (Huginn_Selections.jsonl "rid", per launch: join on launch + rid).
        /// 0 = no v2 record (nothing, a pick the frozen learner skipped, test mode).
        std::uint64_t rid = 0;
        // nothing only. A record stands for one STRETCH of a need episode
        // (core/NeedEpisodes.h "lap", 0.23.51): the whole episode when it
        // never lapped.
        int need = -1;
        double durSec = 0.0;        // the stretch's length
        float peak = 0.0f;          // the highest value inside the stretch
        std::string onsetUtc;       // the EPISODE's onset (the lap keeps it)
        int lap = 0;                // 0 = the stretch from the onset; n = begun by the n-th lap
        bool byLap = false;         // the stretch ended at a lap (the need still on), not at the expiry
        double episodeSec = 0.0;    // the episode's onset to the stretch's end
    };

    struct Head
    {
        std::string launch;
        std::string list;
        std::string build;                        // "0.23.15 (sha)"
        std::vector<std::string> sourceNames;     // the old engine's source types, by index
        float onset = 0.5f, expiry = 0.25f;       // the episode thresholds in force
        double minSec = 1.0, graceSec = 4.0, answerSlackSec = 0.5;
        // 0.23.16: a death drops the open episodes and those in their grace
        // (EpisodeTracker::Abandon). Written as "deathDrops":1; a head without
        // it (0.23.15) recorded `nothing` for episodes a death cut short.
        bool deathDrops = true;
        /// 0.23.51: the lap grid (core/NeedEpisodes.h "lap"), written as
        /// "lapSec", "lapMaxStepSec" and "lapIdleSec" when lapSec > 0. A head
        /// without them (before 0.23.51) judged every episode whole: no lap
        /// records.
        double lapSec = 60.0, lapMaxStepSec = 1.0, lapIdleSec = 30.0;
        /// The [Needs] curves in force, index-aligned with the head's "needs"
        /// (0.23.41): written as "curves" when not empty, so a fit knows which
        /// curve made each logged value. A change of curves mid-launch (a hot
        /// reload) starts a new segment.
        std::vector<Needs::Curve> curves;
        /// 0.23.55 (R8.1): [Scoring] fRefreshPriorSec, the centre of
        /// covered(i)'s left(r) prior (and the refresh pass's window), written
        /// as "refreshPriorSec". A change mid-launch (a hot reload) starts a
        /// new segment, as the curves do. A head without it (before 0.23.55)
        /// centred left(r) at 5 s.
        float refreshPriorSec = 5.0f;
    };

    // --- JSON helpers ---------------------------------------------------------
    /// A JSON string literal. Text that is not valid UTF-8 (plugin strings are
    /// often cp1252) has every byte >= 0x80 escaped as its Latin-1 code point.
    [[nodiscard]] std::string JsonString(std::string_view text);
    /// A float, 4 significant digits; NaN and infinity are `null`.
    void AppendNum(std::string& out, float v);
    [[nodiscard]] std::string FormHex(std::uint32_t form);  // "0003EADE"

    // --- The encoder ----------------------------------------------------------
    /// Turns records into lines for one file segment. Not thread-safe: the
    /// writer thread owns one.
    /// Whether a pipeline run belongs to the current game session. A load
    /// (or `hg reset all`) arms the gate at the cache's generation then: runs
    /// up to it described the previous save, and a context must not log their
    /// page or candidates as this session's.
    struct RunGate
    {
        std::uint64_t resetAt = 0;
        bool armed = false;
        void Reset(std::uint64_t currentGeneration) noexcept
        {
            resetAt = currentGeneration;
            armed = true;
        }
        [[nodiscard]] bool IsCurrent(std::uint64_t generation) const noexcept { return !armed || generation > resetAt; }
    };

    class Encoder
    {
    public:
        /// Start a segment: the head line. Forgets every cap and context id.
        [[nodiscard]] std::string BeginSegment(const Head& head);

        /// The lines for one decision: each cap and the context it needs that
        /// this segment has not defined yet, then the decision.
        [[nodiscard]] std::string Encode(const Decision& d);

        [[nodiscard]] std::size_t CapsDefined() const noexcept { return capByText_.size(); }

    private:
        int CapId(const Effect::Cap* cap, std::string& out);
        void AppendRow(std::string& out, const Row& r, std::string& defs);

        std::unordered_map<std::string, int> capByText_;
        std::unordered_map<const Effect::Cap*, int> capByStatic_;  // catalog-owned caps only
        std::unordered_set<std::uint64_t> ctxDefined_;
    };
}
