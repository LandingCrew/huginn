#pragma once

// =============================================================================
// BOOTSTRAP (R8.2) -- the reader of the R8 bootstrap file, format version 1
// =============================================================================
// The offline fit (roadmap R6, tools/fit/export.py) hands theta, kappa, the
// recent_<family> rows, the feature weights, the per-item b and the fitted
// response curves to the new learner as one JSON file:
// docs/architecture/9-bootstrap-format.md. This is its reader:
//
//   Read          parse (core/MiniJson.h) and apply the format's reader rules
//                 1-10, exactly as tools/fit/bootstrap.validate checks them.
//                 Any violation rejects the whole file (the caller logs one
//                 error and leaves the new model off).
//   MapToBuild    names to the build's indices (format doc, "Names"): a need
//                 or column the build does not know is skipped; a pair the
//                 file lacks takes its prior from effects.csv
//                 (core/ObviousPairs.h: theta0 = sign, precision 1 for an
//                 obvious pair; 0 and 1/0.3^2 = 11.1111 otherwise). The same
//                 for the recent_<family> rows (the build's families and kind_
//                 columns at each of the file's time constants), u0's need
//                 weights and the curves (a need without a curve keeps its
//                 [Needs] line). theta_covered is not in format v1: it starts
//                 at the caller's prior ([Scoring] fCoveredPrior, precision 1).
//   ResolveItems  each item through the caller's resolver (the plugin side:
//                 TESDataHandler::LookupForm(local, plugin)), skipping and
//                 counting an item that does not resolve, whose form is not a
//                 kind the catalog describes, whose name disagrees with the
//                 file's, or whose form another item already took.
//
// Precision is 1 / sigma^2: each weight's variance is 1 / precision.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "core/ChoiceScore.h"
#include "core/MiniJson.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace Huginn::Core::Bootstrap
{
    inline constexpr std::string_view kFormat = "huginn.bootstrap";
    inline constexpr int kVersion = 1;
    /// The fit's priors (fitlib.Model): sigma_obv = 1, sigma_dense = 0.3, sigma_b = 0.5.
    inline constexpr double kPrecisionObvious = 1.0;
    inline constexpr double kPrecisionDense = 1.0 / (0.3 * 0.3);
    inline constexpr double kVarB = 0.5 * 0.5;

    struct Item
    {
        std::string plugin;            // the index owner's file name
        std::string local;             // 6 upper-case hex digits
        std::uint32_t localId = 0;     // its value
        std::string name;              // for the cross-check
        double b = 0.0;
        double precision = 1.0;
        int picks = 0;
    };

    struct RecentFeature
    {
        std::string family;
        double tau = 0.0;
        std::vector<double> theta;      // J_file
        std::vector<double> precision;  // J_file
        std::vector<double> strength;   // J_file with presence+strength (format v1 has no strength precision here)
        bool hasU0 = false;
        double u0 = 0.0;
    };

    struct CurveSpec
    {
        std::string need;
        double centre = 0.0;
        double slope = 0.0;
    };

    /// The file as read, by the file's own names (validated).
    struct Document
    {
        std::string createdUtc;         // may be empty
        std::string sourceChar;         // 16 hex digits, or empty (null: several characters)
        std::string spec;               // model.spec
        std::string outcomes;           // "picks" / "three"
        Choice::Grading grading = Choice::Grading::Graded;
        std::vector<std::string> needs, cols;
        std::vector<double> theta, precision;                    // K_file * J_file
        std::vector<double> strength, strengthPrecision;         // the same with presence+strength
        std::vector<double> bias;                                 // J_file or empty
        double kappa = 0.0;
        bool hasU0 = false;
        double u0Bias = 0.0;
        std::vector<std::pair<std::string, double>> u0Need;
        bool hasRecent = false;
        double window = 0.0;
        std::vector<double> taus;
        std::vector<RecentFeature> recent;
        std::vector<std::pair<std::string, double>> features;
        std::vector<Item> items;
        std::vector<CurveSpec> curves;

        /// source.char as a number (0 when absent or not 16 hex digits).
        [[nodiscard]] std::uint64_t SourceCharId() const noexcept;
    };

    struct ReadResult
    {
        std::optional<Document> doc;
        std::vector<std::string> errors;   // the parse error, or every rule violated
        [[nodiscard]] bool Ok() const noexcept { return doc.has_value(); }
    };

    [[nodiscard]] ReadResult Read(std::string_view text);

    /// What MapToBuild did, for the one info line.
    struct MapReport
    {
        std::size_t needsMapped = 0, colsMapped = 0;
        std::vector<std::string> needsUnknown, colsUnknown;   // file names the build lacks (skipped)
        std::size_t needsPrior = 0, colsPrior = 0;            // build names the file lacks (prior)
        std::size_t recentMapped = 0, recentPrior = 0, recentUnknown = 0;
        std::size_t curvesMapped = 0, curvesUnknown = 0;
        std::size_t featuresWeighted = 0;
    };

    struct BuildCurve
    {
        std::size_t need = 0;   // the build's need index
        double centre = 0.0;
        double slope = 0.0;
    };

    struct MapOptions
    {
        double coveredPrior = -1.0;       // [Scoring] fCoveredPrior
        double coveredPrecision = 1.0;
    };

    struct Mapped
    {
        Choice::Model model;
        std::vector<BuildCurve> curves;
        MapReport report;
    };

    /// The need vector the model reads (format doc, curves): each need's
    /// logged value, except that a need the file gives a curve is re-read from
    /// its logged input through that curve, 1 / (1 + exp(-slope (x - centre))).
    /// A non-finite input keeps the logged value. The one copy of this rule:
    /// the learner's scorer and its pick update and the replay test call it.
    template <class Values, class Inputs>
    [[nodiscard]] std::array<double, Choice::K> CurveNeeds(const std::vector<BuildCurve>& curves,
                                                           const Values& value, const Inputs& input)
    {
        std::array<double, Choice::K> n{};
        for (std::size_t k = 0; k < Choice::K; ++k) n[k] = static_cast<double>(value[k]);
        for (const auto& c : curves) {
            if (c.need >= Choice::K) continue;
            const double x = static_cast<double>(input[c.need]);
            if (!std::isfinite(x)) continue;
            n[c.need] = 1.0 / (1.0 + std::exp(-c.slope * (x - c.centre)));
        }
        return n;
    }

    [[nodiscard]] Mapped MapToBuild(const Document& doc, const MapOptions& opt = {});

    /// The model a build would start from with no file at all: every pair at
    /// its prior, no recent rows, kappa 0, no feature weights. Only for tests
    /// and comparisons: the plugin does not run the new model without a file.
    [[nodiscard]] Choice::Model PriorModel(const MapOptions& opt = {});

    // ---- items ---------------------------------------------------------------
    /// What a resolver reports for (plugin, local): nothing (the plugin or the
    /// record is gone), or the runtime FormID, whether its form type is a kind
    /// the effect catalog describes, and its name.
    struct Found
    {
        std::uint32_t form = 0;
        bool kindOk = false;
        std::string name;
    };

    struct ResolvedItem
    {
        std::uint32_t form = 0;
        double b = 0.0;
        double var = kVarB;
        int picks = 0;
    };

    struct ItemReport
    {
        std::size_t resolved = 0, unresolved = 0, kindMismatch = 0, nameMismatch = 0, duplicateForm = 0;
    };

    /// Do the file's name and the game's agree? Exactly, or, when either
    /// holds a byte >= 0x80 (a cp1252 name the dump's CSV reader replaced, or
    /// the log escaped), on their ASCII bytes alone. An empty file name is not
    /// checked.
    [[nodiscard]] bool NamesAgree(std::string_view file, std::string_view game) noexcept;

    template <class Resolver>
    [[nodiscard]] std::vector<ResolvedItem> ResolveItems(const Document& doc, Resolver&& resolve, ItemReport& rep)
    {
        std::vector<ResolvedItem> out;
        out.reserve(doc.items.size());
        for (const auto& it : doc.items) {
            const std::optional<Found> f = resolve(it.plugin, it.localId);
            if (!f || f->form == 0) {
                ++rep.unresolved;
                continue;
            }
            if (!f->kindOk) {
                ++rep.kindMismatch;
                continue;
            }
            if (!NamesAgree(it.name, f->name)) {
                ++rep.nameMismatch;
                continue;
            }
            bool dup = false;
            for (const auto& o : out) dup |= o.form == f->form;
            if (dup) {
                ++rep.duplicateForm;
                continue;
            }
            out.push_back(ResolvedItem{ f->form, it.b, 1.0 / it.precision, it.picks });
            ++rep.resolved;
        }
        return out;
    }

    // ---- [Learning] sBootstrapBias (roadmap R6 open point 4) -----------------
    /// Whether the file's per-item b seeds the learner: always, never, or only
    /// when the save's character (the cosave's HCID) is the file's
    /// source.char ("match", the default: b is one character's habits and may
    /// not transfer to another; theta is meant to).
    enum class BiasMode : std::uint8_t { Match, Always, Never };

    /// "match" / "always" / "never" (any case); anything else is Match.
    [[nodiscard]] BiasMode ParseBiasMode(std::string_view text) noexcept;
    [[nodiscard]] std::string_view BiasModeName(BiasMode m) noexcept;
    /// Under `mode`, does a save of character `saveChar` take b from a file
    /// fitted on `fileChar` (0: unknown or several)?
    [[nodiscard]] constexpr bool UseFileBias(BiasMode mode, std::uint64_t saveChar, std::uint64_t fileChar) noexcept
    {
        switch (mode) {
        case BiasMode::Always: return true;
        case BiasMode::Never: return false;
        case BiasMode::Match: return saveChar != 0 && fileChar != 0 && saveChar == fileChar;
        }
        return false;
    }

    // ---- the build's families (recent_<family>) ------------------------------
    /// fitlib.recent_families' columns: effects.csv's family level, then the
    /// kind_* columns, in column order.
    [[nodiscard]] std::vector<std::uint16_t> RecentFamilyColumns();

    // ---- stable keys (the learner's entries, never positions) ---------------
    /// FNV-1a 64 of the parts joined by 0x1F: theta (need, column),
    /// strength (need, column), recent (family, tau as "%g"), covered.
    [[nodiscard]] std::uint64_t StableKey(std::string_view kind, std::string_view a = {}, std::string_view b = {}) noexcept;
    [[nodiscard]] std::string TauText(double tau);
}
