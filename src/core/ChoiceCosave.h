#pragma once

// =============================================================================
// CHOICE COSAVE (R8.3) -- the THTA and BIAS records as byte blobs
// =============================================================================
// The new learner's state rides in the save beside the old learner's BNDW
// (persist/ChoiceSerializer.cpp writes these blobs with one WriteRecordData
// each). BNDW STAYS: R6's D2 reads the old learner's per-item state, which
// reaches back before the logs; the map's "THTA/BIAS replace BNDW" waits until
// D2 is retired.
//
// THTA, version 1 (theta: every weight with a stable key):
//   u32 version
//   u16 n, n bytes   the bootstrap's created_utc (as the file says it)
//   u16 n, n bytes   the bootstrap file's SHA-256 (hex)
//   u32 count
//   count x { u64 key, f64 mu, f64 var }    28 bytes each
// The entries are keyed weights the learner holds: theta, strength,
// recent_<family>, covered (core/Bootstrap.h StableKey). The plugin writes
// those that moved from the bootstrap (MergeTheta gives every other key the
// file's value on load); the codec takes any set. Outside-option entries would
// ride here too; format v1 gives u0 no precision, so there is no belief and
// nothing to write.
//
// BIAS, version 1 (b: one belief per item):
//   u32 version
//   u32 count
//   count x { u32 formID, f64 mu, f64 var, f32 n, u32 minutes, u8 flags }   29 bytes each
// minutes = PLAY minutes since the item was last chosen (the battery's clock,
// as BNDW v4); flags bit 0 = last seen as a favourite (the battery's life
// multiplier in Boost mode). The map planned {formID, b, n, minutes}: BIAS
// adds the variance (b is a belief, and the score's sigma reads it) and the
// favourite bit (without it a favourite's battery would run at 1x until its
// next pick).
//
// Little-endian, fixed widths, no padding (written field by field). A decode
// rejects a wrong version, a length that does not match the count exactly, a
// count above the cap, or a string longer than its cap; it drops (and counts)
// entries that are not finite or have a variance <= 0.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace Huginn::Core::ChoiceCosave
{
    inline constexpr std::uint32_t kThetaVersion = 1;
    inline constexpr std::uint32_t kBiasVersion = 1;

    inline constexpr std::uint32_t kMaxThetaEntries = 1'000'000;   // K*J is ~15k today
    inline constexpr std::uint32_t kMaxBiasEntries = 100'000;
    inline constexpr std::size_t kMaxText = 256;

    inline constexpr std::size_t kThetaStride = 8 + 8 + 8;
    inline constexpr std::size_t kBiasStride = 4 + 8 + 8 + 4 + 4 + 1;

    inline constexpr std::uint8_t kBiasFavorite = 1;

    struct ThetaEntry
    {
        std::uint64_t key = 0;
        double mu = 0.0;
        double var = 1.0;
        bool operator==(const ThetaEntry&) const = default;
    };

    struct ThetaRecord
    {
        std::string createdUtc;
        std::string sha256;
        std::vector<ThetaEntry> entries;
        std::size_t dropped = 0;   // decode only: entries not finite or var <= 0
    };

    struct BiasEntry
    {
        std::uint32_t form = 0;
        double mu = 0.0;
        double var = 0.25;
        float n = 0.0f;
        std::uint32_t minutes = 0;
        std::uint8_t flags = 0;
        bool operator==(const BiasEntry&) const = default;
    };

    struct BiasRecord
    {
        std::vector<BiasEntry> entries;
        std::size_t dropped = 0;   // decode only
    };

    [[nodiscard]] std::vector<std::byte> EncodeTheta(const ThetaRecord& r);
    [[nodiscard]] std::vector<std::byte> EncodeBias(const BiasRecord& r);

    /// `headerVersion`: the version SKSE's record header carries; it must
    /// equal the blob's own and kThetaVersion / kBiasVersion. `why` gets the
    /// reason for an empty result.
    [[nodiscard]] std::optional<ThetaRecord> DecodeTheta(std::span<const std::byte> blob, std::uint32_t headerVersion,
                                                         std::string* why = nullptr);
    [[nodiscard]] std::optional<BiasRecord> DecodeBias(std::span<const std::byte> blob, std::uint32_t headerVersion,
                                                       std::string* why = nullptr);

    /// The load's merge of a saved THTA onto the bootstrap: every saved entry
    /// whose key the build knows overrides the bootstrap's value; a key the
    /// save lacks keeps the file's value; a saved key the build no longer knows
    /// is dropped and counted.
    struct MergeReport
    {
        std::size_t applied = 0;    // saved entries the build knows
        std::size_t unknown = 0;    // saved keys the build no longer knows (dropped)
        std::size_t fromFile = 0;   // build keys the save lacks (the bootstrap's value)
        std::size_t duplicate = 0;  // a key saved twice: the last one wins
    };
    [[nodiscard]] std::vector<ThetaEntry> MergeTheta(std::span<const std::uint64_t> buildKeys,
                                                     std::span<const ThetaEntry> saved, MergeReport& rep);
}
