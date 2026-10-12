#pragma once

// =============================================================================
// REACH LOG -- one line per reach-in, and the heartbeat's split of them
// =============================================================================
// A reach-in is the objective metric: the player went past Huginn, into a game
// menu, a vanilla hotkey or one of their own Wheeler wheels, to equip or use
// something. SelectionTracker counts one when an outside selection CONFIRMS
// (the same place the soak heartbeat's reachIns comes from, so the [Reach]
// lines and reachIns agree); enchanted gear put on through a menu counts too
// (kind=gear), though it stays out of reachIns and the learner (R4: armour
// waits for R8).
//
// Here: what kind of item it was, which equips of gear count, the [Reach]
// line, and the heartbeat's "reach fight=.. calm=.. (kind=..)" tail. The game
// side pulls the few fields out of the form and calls in.
//
// Pure: standard library only (src/core/README.md).
// =============================================================================

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Huginn::Core::Reach
{
    enum class Kind : std::uint8_t
    {
        Spell, Potion, Food, Poison, Scroll, Weapon, Ammo, Gear, Other,
        Count
    };

    [[nodiscard]] constexpr std::string_view KindName(Kind kind) noexcept
    {
        switch (kind) {
        case Kind::Spell:  return "spell";
        case Kind::Potion: return "potion";
        case Kind::Food:   return "food";
        case Kind::Poison: return "poison";
        case Kind::Scroll: return "scroll";
        case Kind::Weapon: return "weapon";
        case Kind::Ammo:   return "ammo";
        case Kind::Gear:   return "gear";
        default:           return "other";
        }
    }

    /// The form's type, as far as the kind needs it.
    enum class FormClass : std::uint8_t
    {
        Spell, Scroll, Alchemy, Weapon, Ammo, Armor, Light, SoulGem, Other
    };

    /// An alchemy item is a poison, food or a potion, poison first (the game's
    /// own flag order: a poison flagged food is still applied to a blade). A
    /// staff is a weapon. Torches and soul gems are "other".
    [[nodiscard]] constexpr Kind ClassifyKind(FormClass form, bool food, bool poison) noexcept
    {
        switch (form) {
        case FormClass::Spell:   return Kind::Spell;
        case FormClass::Scroll:  return Kind::Scroll;
        case FormClass::Alchemy: return poison ? Kind::Poison : food ? Kind::Food : Kind::Potion;
        case FormClass::Weapon:  return Kind::Weapon;
        case FormClass::Ammo:    return Kind::Ammo;
        case FormClass::Armor:   return Kind::Gear;
        default:                 return Kind::Other;
        }
    }

    /// The vias (PlayerInputGate::Explain) that mean a game menu: the only
    /// ones a gear reach may come through. A vanilla hotkey or a wheel are not.
    [[nodiscard]] constexpr bool IsMenuVia(std::string_view via) noexcept
    {
        return via == "inventory menu" || via == "favorites menu" || via == "magic menu" ||
               via == "menu (just closed)";
    }

    // -------------------------------------------------------------------------
    // GEAR: is the piece just put on enchanted?
    // -------------------------------------------------------------------------
    // The equip event names the base form, not the stack. A base enchantment
    // settles it. A player enchantment lives on one stack's extra data, so:
    // the stack marked worn decides when one is; otherwise only when every
    // copy of the form is enchanted (a plain Gold Ring put on while an
    // enchanted one sits in the bag must not count). Mixed stacks with none
    // marked worn: Ambiguous, not counted.
    enum class Enchant : std::uint8_t { Yes, No, Ambiguous };

    struct StackFacts
    {
        bool baseEnchanted = false;     // the base form's own enchantment
        bool wornKnown = false;         // a stack of the form is marked worn
        bool wornEnchanted = false;     // ... and it carries a player enchantment
        int enchantedStacks = 0;        // stacks with a player enchantment
        int plainStacks = 0;            // stacks with extra data but no enchantment
        int looseCopies = 0;            // copies with no extra data at all (plain)
    };

    [[nodiscard]] constexpr Enchant ClassifyEnchant(const StackFacts& f) noexcept
    {
        if (f.baseEnchanted) return Enchant::Yes;
        if (f.wornKnown) return f.wornEnchanted ? Enchant::Yes : Enchant::No;
        if (f.enchantedStacks == 0) return Enchant::No;
        if (f.plainStacks == 0 && f.looseCopies <= 0) return Enchant::Yes;
        return Enchant::Ambiguous;
    }

    // -------------------------------------------------------------------------
    // GEAR: which armour equips are reaches
    // -------------------------------------------------------------------------
    // Contextual gear only: an enchanted piece (a ring of Conjuration, an
    // amulet, a circlet, armour with a skill or resist enchantment), put on
    // through a menu, not during a load, and not Huginn's own equip (the
    // listener drops those before this). Plain clothes and armour are
    // dressing. A second event for the same piece inside REPEAT_MS (both
    // equip slots, a doubled event) is the same equip.
    struct GearEquip
    {
        std::uint32_t formID = 0;
        bool enchanted = false;      // the base form's enchantment, or a player one on the stack
        bool loading = false;        // a load in progress, or inside the post-load window
        std::string_view via;        // PlayerInputGate::Explain; empty = no player input
    };

    class GearGate
    {
    public:
        static constexpr std::int64_t REPEAT_MS = 1500;
        static constexpr std::size_t kMaxRemembered = 64;

        /// True when this equip counts as a gear reach. Thread-safe.
        [[nodiscard]] bool Counts(const GearEquip& e, std::int64_t nowMs)
        {
            if (e.formID == 0 || !e.enchanted || e.loading || !IsMenuVia(e.via)) return false;
            std::lock_guard lock(mutex_);
            if (const auto it = last_.find(e.formID); it != last_.end() && nowMs - it->second < REPEAT_MS) {
                return false;
            }
            if (last_.size() >= kMaxRemembered) last_.clear();   // bounded; clearing only ends repeat windows early
            last_[e.formID] = nowMs;
            return true;
        }

        void Reset()
        {
            std::lock_guard lock(mutex_);
            last_.clear();
        }

        [[nodiscard]] std::size_t Remembered()
        {
            std::lock_guard lock(mutex_);
            return last_.size();
        }

    private:
        std::mutex mutex_;
        std::unordered_map<std::uint32_t, std::int64_t> last_;
    };

    // -------------------------------------------------------------------------
    // THE [Reach] LINE
    // -------------------------------------------------------------------------
    struct Line
    {
        std::string_view name;
        std::uint32_t formID = 0;
        Kind kind = Kind::Other;
        std::string_view via;
        bool inFight = false;
        std::string_view caseLabel;   // ExternalEquipLearner's A-E label ("" = none)
        bool scored = false;          // Huginn scored it in the run the pick saw
        bool ranked = false;          // its rank is known (inside the sorted prefix), else "tail"
        std::size_t rank = 0;         // 0-based, as the selection log prints it
        float utility = 0.0f;
        bool pageKnown = false;       // a pipeline run had been cached
        std::size_t page = 0;
        std::vector<std::pair<std::size_t, std::string>> keys;   // slot index, name: the page as shown
    };

    /// "[Reach] 'Fast Healing' 0002F3B8 kind=spell via=magic menu fight=no
    ///  case=B-low (low rank) rank=14 util=0.83 | page 0: 0=Oakflesh 1=Lesser Ward"
    [[nodiscard]] inline std::string FormatLine(const Line& in)
    {
        std::string out = std::format("[Reach] '{}' {:08X} kind={} via={} fight={} case={}",
            in.name.empty() ? std::string_view("?") : in.name, in.formID, KindName(in.kind),
            in.via.empty() ? std::string_view("?") : in.via, in.inFight ? "yes" : "no",
            in.caseLabel.empty() ? std::string_view("-") : in.caseLabel);
        if (!in.scored) {
            out += " rank=- util=-";
        } else if (in.ranked) {
            out += std::format(" rank={} util={:.2f}", in.rank, in.utility);
        } else {
            out += std::format(" rank=tail util={:.2f}", in.utility);
        }
        if (!in.pageKnown) {
            out += " | page ?";
            return out;
        }
        out += std::format(" | page {}:", in.page);
        if (in.keys.empty()) {
            out += " (empty)";
        }
        for (const auto& [slot, name] : in.keys) {
            out += std::format(" {}={}", slot, name);
        }
        return out;
    }

    // -------------------------------------------------------------------------
    // THE HEARTBEAT SPLIT
    // -------------------------------------------------------------------------
    // Read-and-reset per heartbeat window; Record from any thread (a
    // selection confirms on the update thread, gear arrives on the equip
    // event's thread).
    class Split
    {
    public:
        void Record(bool inFight, Kind kind) noexcept
        {
            (inFight ? fight_ : calm_).fetch_add(1, std::memory_order_relaxed);
            const auto k = static_cast<std::size_t>(kind);
            if (k < kinds_.size()) kinds_[k].fetch_add(1, std::memory_order_relaxed);
        }

        struct Counts
        {
            std::uint32_t fight = 0;
            std::uint32_t calm = 0;
            std::array<std::uint32_t, static_cast<std::size_t>(Kind::Count)> kinds{};
        };

        [[nodiscard]] Counts Take() noexcept
        {
            Counts c;
            c.fight = fight_.exchange(0, std::memory_order_relaxed);
            c.calm = calm_.exchange(0, std::memory_order_relaxed);
            for (std::size_t k = 0; k < kinds_.size(); ++k) {
                c.kinds[k] = kinds_[k].exchange(0, std::memory_order_relaxed);
            }
            return c;
        }

    private:
        std::atomic<std::uint32_t> fight_{ 0 };
        std::atomic<std::uint32_t> calm_{ 0 };
        std::array<std::atomic<std::uint32_t>, static_cast<std::size_t>(Kind::Count)> kinds_{};
    };

    /// "reach fight=3 calm=5 (spell=4 food=2 potion=1 gear=1)": the kinds in
    /// enum order, only those seen; "reach fight=0 calm=0" for a quiet window.
    [[nodiscard]] inline std::string FormatSplit(const Split::Counts& c)
    {
        std::string out = std::format("reach fight={} calm={}", c.fight, c.calm);
        std::string kinds;
        for (std::size_t k = 0; k < c.kinds.size(); ++k) {
            if (c.kinds[k] == 0) continue;
            kinds += std::format("{}{}={}", kinds.empty() ? "" : " ", KindName(static_cast<Kind>(k)), c.kinds[k]);
        }
        if (!kinds.empty()) out += " (" + kinds + ")";
        return out;
    }
}
