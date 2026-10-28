#pragma once

// =============================================================================
// LATE CATALOG -- cap(i) for what the player holds that the build left out
// =============================================================================
// The effect catalog (effect/EffectCatalog) is built once, at the main menu,
// from the load order. Two kinds of item the player holds were never in it:
//
//   - forms made at runtime: a player-made potion or poison is a dynamic FF
//     form that does not exist at the main menu. In the 0.23.15-0.23.38 v3
//     log 27 of them sat in 28,303 eligible or shown rows with no cap(i)
//     (Potion of Restore Magicka FF0048C4, ...);
//   - spells the player knows that no tome teaches: rule 1 keeps a plain
//     spell only when a tome teaches it (the load order holds thousands of
//     NPC spells), so a spell learned another way -- LoreRim's Oakflesh on
//     Target (8D005B23, 4,141 rows), its Arcane Anchor -- had no cap(i)
//     either.
//
// So the catalog maps such a form the first time it is held, with the build's
// populations (0.23.41). What is decided here:
//
//   MapLate     whether a form missing from the build is worth mapping: a
//               runtime (FF) form, or a spell. Anything else the build saw
//               and left out on purpose (rule 1), and stays out.
//   MarkHeld    rule 1 for a spell the player knows: the tome test does not
//               apply; every other rule stands (InScope).
//   LateEntries one entry per FormID, made once (a "no entry" answer is
//               remembered too). On the next load an FF key is forgotten --
//               an FF ID names another potion in another save -- but kept
//               alive for the process, so an address handed out stays valid
//               and never names another entry (the v3 encoder keys catalog
//               caps by address). A load-order key keeps its answer
//               (0.23.45): its form cannot change between saves, and
//               re-mapping some 200 known abilities cost ~0.6 s per load.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include "EffectRecords.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Huginn::Core::Effect
{
    /// A FormID the game assigned at runtime (load index 0xFF).
    [[nodiscard]] constexpr bool IsDynamicFormId(std::uint32_t formId) noexcept { return (formId >> 24) == 0xFFu; }

    /// Whether a held form the build has no entry for is mapped now.
    [[nodiscard]] constexpr bool MapLate(std::uint32_t formId, bool isSpell) noexcept
    {
        return IsDynamicFormId(formId) || isSpell;
    }

    /// Rule 1 for an item the player holds or knows: a known spell is in
    /// scope whether or not a tome teaches it.
    inline void MarkHeld(ItemRecord& item) noexcept
    {
        if (item.kind == Kind::Spell) item.taughtByTome = true;
    }

    template <class Value>
    class LateEntries
    {
    public:
        /// The live entry of `key`, made by `make()` (returning
        /// std::unique_ptr<Value>, null for "no entry") on the first call of
        /// this load. Null when there is none.
        template <class Make>
        const Value* Get(std::uint32_t key, Make&& make)
        {
            if (const auto it = live_.find(key); it != live_.end()) return it->second.get();
            if (live_.size() >= kMaxLive) Forget();
            std::unique_ptr<Value> made = std::forward<Make>(make)();
            const Value* out = made.get();
            live_.emplace(key, std::move(made));
            ++made_;
            return out;
        }

        /// The live entry of `key` if one was made, without making one.
        [[nodiscard]] const Value* Peek(std::uint32_t key) const
        {
            const auto it = live_.find(key);
            return it == live_.end() ? nullptr : it->second.get();
        }

        /// Whether `key` has an answer this load: an entry, or a remembered
        /// "no entry" (Peek then reads null). A caller that would otherwise
        /// look the form up again stops here.
        [[nodiscard]] bool Contains(std::uint32_t key) const { return live_.contains(key); }

        /// Remember "no entry" for `key` this load, without a factory: a key
        /// the caller ruled out before mapping (not a late kind). A key that
        /// already has an answer keeps it.
        void RememberNone(std::uint32_t key)
        {
            if (live_.contains(key)) return;
            if (live_.size() >= kMaxLive) Forget();
            live_.emplace(key, nullptr);
        }

        /// Every key is forgotten (the kMaxLive bound). The entries are
        /// retired, not freed, so a pointer handed out stays valid.
        void Forget()
        {
            for (auto& [key, value] : live_) {
                if (value) retired_.push_back(std::move(value));
            }
            live_.clear();
        }

        /// A new load: only the keys `forget(key)` names are forgotten,
        /// retired as in Forget; the rest keep their answer (entry or none)
        /// at the same address. Returns how many were forgotten.
        template <class Pred>
        std::size_t ForgetIf(Pred&& forget)
        {
            std::size_t n = 0;
            for (auto it = live_.begin(); it != live_.end();) {
                if (!forget(it->first)) {
                    ++it;
                    continue;
                }
                if (it->second) retired_.push_back(std::move(it->second));
                it = live_.erase(it);
                ++n;
            }
            return n;
        }

        [[nodiscard]] std::size_t Live() const noexcept { return live_.size(); }
        [[nodiscard]] std::size_t Retired() const noexcept { return retired_.size(); }
        [[nodiscard]] std::size_t Made() const noexcept { return made_; }

        /// Keys remembered before they are all retired (a bound, not a working
        /// size: a save holds a few dozen player-made potions, a character a
        /// few hundred known spells).
        static constexpr std::size_t kMaxLive = 4096;

    private:
        std::unordered_map<std::uint32_t, std::unique_ptr<Value>> live_;
        std::vector<std::unique_ptr<Value>> retired_;
        std::size_t made_ = 0;
    };
}
