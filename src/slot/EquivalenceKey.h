#pragma once

#include "SlotAssignment.h"
#include "candidate/CandidateTypes.h"
#include "learning/FitScorer.h"        // Scoring::CastsLeft
#include "learning/ScoredCandidate.h"
#include "spell/SpellData.h"
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace Huginn::Slot
{
    // =============================================================================
    // EQUIVALENCE KEY (per-page equivalence cap)
    // =============================================================================
    // Two candidates with the same key are near-interchangeable from the
    // player's seat: Flames and a Scroll of Flames, or two Novice self-cast
    // frost cloaks from different mods. Within one category the context
    // weight is identical and the prior differs by at most ~0.1, so untrained
    // near-duplicates tie and can fill several slots of one page between them.
    // `[SlotLocker] bCapEquivalents` lets the allocator show at most
    // `iMaxPerEquivalenceKey` of them per page (see SlotAllocator).
    //
    // Same idea as the potion family key in
    // UtilityScorer::ApplyPotionTierPreference, applied to spells and scrolls.
    // It neither reuses nor alters that function: potions get no key here,
    // because tier preference already keeps one potion per family on top.
    //
    // Everything in the key is something the player can see -- the effect,
    // its element, how it is cast, and the skill level printed in the magic
    // menu (or, failing that, the cost). Nothing new is sensed.
    //
    // Key fields:
    //   type     SpellType. Unknown => no key (fails safe: never capped).
    //   tags     SpellTag minus EQUIV_TAG_IGNORE. `type` alone is too coarse:
    //            Buff/None/Self/Apprentice would merge Oakflesh, Muffle and
    //            Courage. The ignored bits describe delivery, which has its own
    //            field, and would split a spell from its own scroll.
    //   tagsExt  SpellTagExt, whole.
    //   element  ElementType.
    //   delivery DeliveryBucket (Self / Touch / Ranged).
    //   tier     0 Novice .. 4 Master; see TierBand.
    // =============================================================================

    /// Delivery, coarsened. Aimed, TargetActor and TargetLocation all mean
    /// "cast at something over there" to the player.
    enum class DeliveryBucket : uint8_t
    {
        Unknown = 0,
        Self,
        Touch,
        Ranged  // Aimed | TargetActor | TargetLocation
    };

    [[nodiscard]] inline constexpr std::string_view DeliveryBucketToString(DeliveryBucket d) noexcept
    {
        switch (d) {
            case DeliveryBucket::Self:   return "Self";
            case DeliveryBucket::Touch:  return "Touch";
            case DeliveryBucket::Ranged: return "Ranged";
            default:                     return "Unknown";
        }
    }

    struct EquivalenceKey
    {
        Spell::SpellType type = Spell::SpellType::Unknown;
        uint32_t tags = 0;       // SpellTag bits, EQUIV_TAG_IGNORE cleared
        uint16_t tagsExt = 0;    // SpellTagExt bits
        Spell::ElementType element = Spell::ElementType::None;
        DeliveryBucket delivery = DeliveryBucket::Unknown;
        uint8_t tier = 0;        // 0 Novice .. 4 Master

        bool operator==(const EquivalenceKey&) const = default;
    };

    /// Stable text form of a key: "type|tagsHex|tagsExtHex|element|delivery|tier",
    /// e.g. "Damage|0x12|0x0|Fire|Ranged|0". Used by the decision log ("eqk");
    /// changing the format is a telemetry schema change.
    [[nodiscard]] inline std::string EquivalenceKeyToString(const EquivalenceKey& key)
    {
        return std::format("{}|0x{:X}|0x{:X}|{}|{}|{}",
            Spell::SpellTypeToString(key.type), key.tags, key.tagsExt,
            Spell::ElementTypeToString(key.element), DeliveryBucketToString(key.delivery),
            static_cast<unsigned>(key.tier));
    }

    /// Tag bits that describe HOW a spell is delivered, not WHAT it does.
    /// Cleared before comparing, so a concentration spell and a
    /// fire-and-forget scroll of the same effect still share a key; delivery
    /// is compared separately through DeliveryBucket.
    inline constexpr uint32_t EQUIV_TAG_IGNORE =
        std::to_underlying(Spell::SpellTag::Ranged) |
        std::to_underlying(Spell::SpellTag::Melee) |
        std::to_underlying(Spell::SpellTag::Concentration);

    /// Base-cost band edges used when a spell carries no skill level.
    /// `[SlotLocker] sEquivalenceCostBands`; must be ascending.
    using CostBandEdges = std::array<uint32_t, 4>;
    inline constexpr CostBandEdges DEFAULT_COST_BAND_EDGES{ 40, 100, 250, 600 };

    /// Tier band, 0 (Novice) .. 4 (Master).
    ///
    /// Skill level first: the costliest effect's minimumSkill is the level the
    /// magic menu and the tome show (0/25/50/75/100 in vanilla), so
    /// skillLevel/25 IS the tier the player reads. But vanilla Novice is 0 and
    /// mods often leave the field at 0, so 0 cannot tell "Novice" from "not
    /// set" -- for those the base cost decides: the band is the number of
    /// edges at or below the cost. With the default edges a 14-point Flames is
    /// band 0 and a 700-point spell band 4.
    [[nodiscard]] inline constexpr uint8_t TierBand(
        uint8_t skillLevel, uint32_t baseCost, const CostBandEdges& edges) noexcept
    {
        if (skillLevel > 0) {
            const unsigned band = static_cast<unsigned>(skillLevel) / 25u;
            return static_cast<uint8_t>(band > 4u ? 4u : band);
        }
        uint8_t band = 0;
        for (const uint32_t edge : edges) {
            if (edge <= baseCost) {
                ++band;
            }
        }
        return band;
    }

    [[nodiscard]] inline constexpr DeliveryBucket ToDeliveryBucket(Spell::SpellDelivery d) noexcept
    {
        switch (d) {
            case Spell::SpellDelivery::Self:           return DeliveryBucket::Self;
            case Spell::SpellDelivery::Touch:          return DeliveryBucket::Touch;
            case Spell::SpellDelivery::Aimed:
            case Spell::SpellDelivery::TargetActor:
            case Spell::SpellDelivery::TargetLocation: return DeliveryBucket::Ranged;
            default:                                   return DeliveryBucket::Unknown;
        }
    }

    /// Key for a spell or scroll; nullopt for anything else (potions, food,
    /// soul gems, weapons, staves, ammo, apparel) and for a spell whose type
    /// the classifier could not decide.
    [[nodiscard]] inline std::optional<EquivalenceKey> MakeEquivalenceKey(
        const Candidate::CandidateVariant& candidate, const CostBandEdges& edges) noexcept
    {
        return std::visit([&edges](const auto& c) -> std::optional<EquivalenceKey> {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, Candidate::SpellCandidate> ||
                          std::is_same_v<T, Candidate::ScrollCandidate>) {
                if (c.type == Spell::SpellType::Unknown) {
                    return std::nullopt;
                }
                EquivalenceKey key;
                key.type = c.type;
                key.tags = std::to_underlying(c.tags) & ~EQUIV_TAG_IGNORE;
                key.tagsExt = std::to_underlying(c.tagsExt);
                key.element = c.element;
                key.delivery = ToDeliveryBucket(c.delivery);
                key.tier = TierBand(c.skillLevel, c.baseCost, edges);
                return key;
            } else {
                return std::nullopt;
            }
        }, candidate);
    }

    [[nodiscard]] inline std::optional<EquivalenceKey> MakeEquivalenceKey(
        const Scoring::ScoredCandidate& sc, const CostBandEdges& edges) noexcept
    {
        return MakeEquivalenceKey(sc.candidate, edges);
    }

    /// How many of `placed` carry `key`. Every placed kind counts: overrides,
    /// Remembrance holds, held seats, wildcards.
    [[nodiscard]] inline uint32_t CountWithEquivalenceKey(
        const SlotAssignments& placed, const EquivalenceKey& key, const CostBandEdges& edges) noexcept
    {
        uint32_t count = 0;
        for (const auto& a : placed) {
            if (a.IsEmpty() || !a.candidate) continue;
            if (const auto other = MakeEquivalenceKey(*a.candidate, edges); other && *other == key) {
                ++count;
            }
        }
        return count;
    }

    /// True if placing `c` would exceed the cap: `placed` already shows
    /// `maxPerKey` items with its key. Always false for a candidate with no key.
    [[nodiscard]] inline bool ExceedsEquivalenceCap(
        const SlotAssignments& placed, const Scoring::ScoredCandidate& c,
        uint32_t maxPerKey, const CostBandEdges& edges) noexcept
    {
        const auto key = MakeEquivalenceKey(c, edges);
        if (!key) {
            return false;
        }
        return CountWithEquivalenceKey(placed, *key, edges) >= maxPerKey;
    }

    /// Spell vs scroll of the same key: the spell while the player can cast
    /// it now (at least one cast left), else the scroll. Shares FitScorer's
    /// casts-left helper so both read affordability the same way. For a
    /// concentration spell effectiveCost is per second and this reads "can
    /// sustain it for a second" (one-second cast, independent of
    /// [Scoring] fFitConcentrationSecondsPerCast, so the cap does not change
    /// with a scoring knob). A spell with no cost (<= 0) is always preferred.
    [[nodiscard]] inline constexpr bool PreferSpellOverScroll(
        float currentMagicka, float spellEffectiveCost) noexcept
    {
        return Scoring::CastsLeft(currentMagicka, spellEffectiveCost,
                                  /*isConcentration=*/false, /*secondsPerCast=*/1.0f) >= 1.0f;
    }

}  // namespace Huginn::Slot
