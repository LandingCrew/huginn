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
//   nothing  a need episode ended with no selection (core/NeedEpisodes.h)
//
// The schema, field by field, with units and versioning:
// docs/architecture/selection-log-v3.md. tools/replay/replay.py reads it.
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
// This file is the pure part: the plain records, the encoder that turns them
// into lines (tracking which caps and contexts the current segment has
// defined) and the JSON helpers. The game side (learning/SelectionLogV3*)
// builds the records and writes the lines on a background thread.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "EffectMapper.h"
#include "NeedEvaluator.h"

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
        _Count
    };
    inline constexpr std::size_t kCrossCount = static_cast<std::size_t>(Cross::_Count);
    /// The effects.csv column each cross-feature fills (same names).
    inline constexpr std::array<Effect::Col, kCrossCount> kCrossColumns{
        Effect::Col::overshoot_health, Effect::Col::overshoot_magicka, Effect::Col::overshoot_stamina,
        Effect::Col::weapon_charge,    Effect::Col::stack_count,       Effect::Col::ammo_matches_launcher,
        Effect::Col::school_fortified,
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
    }

    inline constexpr std::uint8_t kNoKind = 255;    // not in the effect catalog
    inline constexpr std::uint8_t kNoSource = 255;  // not a candidate of the old engine
    inline constexpr float kNone = std::numeric_limits<float>::quiet_NaN();

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
        std::string why;            // "press", "menu" (a selection menu opened), "onset"
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
        std::vector<Row> rows;
    };

    /// One decision.
    struct Decision
    {
        std::uint64_t seq = 0;      // per launch, in write order
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
        // nothing only
        int need = -1;
        double durSec = 0.0;
        float peak = 0.0f;
        std::string onsetUtc;
    };

    struct Head
    {
        std::string launch;
        std::string list;
        std::string build;                        // "0.23.15 (sha)"
        std::vector<std::string> sourceNames;     // the old engine's source types, by index
        float onset = 0.5f, expiry = 0.25f;       // the episode thresholds in force
        double minSec = 1.0, graceSec = 3.5;
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
