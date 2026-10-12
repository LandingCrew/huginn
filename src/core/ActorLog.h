#pragma once

// =============================================================================
// ACTOR LOG -- who was in the fight, and what the scoring target was
// =============================================================================
// Two logs, both transitions only (CLAUDE.md "Log transitions, not ticks"):
//
// [Target]  One line whenever the SCORING target (TargetCollection::
//           ScoringTargetType's pick: the living hostile primary, else the
//           closest living hostile) changes actor or classified type. A type
//           change on the same actor says so -- that is a classifier flicker,
//           not a new enemy. In and out of fights.
//
// [Actors]  During a fight (the published combat flag), over the actors the
//           target poll already tracks -- no scan of its own:
//             +<ref> ...   the first time an actor is tracked this session
//             again <ref>  an actor seen before this session (or back after
//                          leaving this fight)
//             then by refID: hostility, type, died, revived, left tracking
//             (absent LEAVE_GRACE_SEC, so a range flap is not two lines)
//           and one summary line when the fight ends. Becoming or ceasing to
//           be the scoring target is the [Target] line, which names both
//           actors; it is not repeated here.
//
// Perceivable facts only (CLAUDE.md "Forbidden Information"): name, race,
// type, hostility, distance band, alive or dead, summon and commander. No
// level, inventory, spell list or hidden stat ever reaches these structs.
//
// The session's seen set is an LRU capped at Config::seenCap refIDs.
//
// Pure: standard library only (src/core/README.md). Single-threaded: the
// game side calls it from the target poll only.
// =============================================================================

#include "core/TargetType.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <list>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Huginn::Core::ActorLog
{
    using State::TargetType;

    // The evaluator's distance edges (GameState::distance: melee 256, mid
    // 768), read as close / mid / far.
    enum class Band : std::uint8_t { Close, Mid, Far };

    [[nodiscard]] constexpr std::string_view BandName(Band band) noexcept
    {
        switch (band) {
        case Band::Close: return "close";
        case Band::Mid:   return "mid";
        default:          return "far";
        }
    }

    /// distSq <= 0 is the target list's "no distance yet" sentinel: far.
    [[nodiscard]] constexpr Band BandOf(float distSq, float closeMaxSq, float midMaxSq) noexcept
    {
        if (!(distSq > 0.0f)) return Band::Far;
        if (distSq <= closeMaxSq) return Band::Close;
        if (distSq <= midMaxSq) return Band::Mid;
        return Band::Far;
    }

    /// Who an actor is. Read once per actor per fight, from the game side.
    struct Identity
    {
        std::string name;
        std::uint32_t baseID = 0;
        std::string raceEditorID;
        std::string raceName;
        bool summon = false;          // a commanded or summoned actor
        std::string commander;        // its commander's name; "" = none known
    };

    /// "-" (not a summon), "'<commander>'", or "?" (a summon with no commander known).
    [[nodiscard]] inline std::string SummonField(const Identity& id)
    {
        if (!id.summon) return "-";
        if (id.commander.empty()) return "?";
        return std::format("'{}'", id.commander);
    }

    /// One tracked actor on one poll.
    struct Observation
    {
        std::uint32_t refID = 0;
        bool hostile = false;
        bool dead = false;
        // Dead, yet the target list still counts it as a living hostile (an
        // entry the hostile scan stopped refreshing when it died).
        bool countedAlive = false;
        TargetType type = TargetType::None;
        Band band = Band::Far;
    };

    /// One `hg actors` row: "<ref> '<name>' race=<edid> type=<T> hostile=<0/1>
    /// dead=<0/1> summon=<..> band=<..>", " [scoring target]" on the one scoring sees.
    [[nodiscard]] inline std::string FormatRow(const Observation& o, const Identity& id, bool scoringTarget)
    {
        return std::format("{:08X} '{}' race={} type={} hostile={} dead={} summon={} band={}{}",
            o.refID, id.name, id.raceEditorID.empty() ? std::string_view("?") : std::string_view(id.raceEditorID),
            State::GetTargetTypeName(o.type), o.hostile ? 1 : 0, o.dead ? 1 : 0, SummonField(id),
            BandName(o.band), scoringTarget ? " [scoring target]" : "");
    }

    // =========================================================================
    // SEEN SET -- refIDs met this session, least recently met dropped first
    // =========================================================================
    class SeenSet
    {
    public:
        explicit SeenSet(std::size_t cap) : cap_(cap == 0 ? 1 : cap) {}

        /// Record `refID` as met now. True when it had been met before.
        bool Touch(std::uint32_t refID)
        {
            if (const auto it = pos_.find(refID); it != pos_.end()) {
                order_.splice(order_.begin(), order_, it->second);
                return true;
            }
            order_.push_front(refID);
            pos_[refID] = order_.begin();
            if (order_.size() > cap_) {
                pos_.erase(order_.back());
                order_.pop_back();
            }
            return false;
        }

        [[nodiscard]] bool Contains(std::uint32_t refID) const { return pos_.contains(refID); }
        [[nodiscard]] std::size_t Size() const noexcept { return order_.size(); }
        void Clear()
        {
            order_.clear();
            pos_.clear();
        }

    private:
        std::size_t cap_;
        std::list<std::uint32_t> order_;
        std::unordered_map<std::uint32_t, std::list<std::uint32_t>::iterator> pos_;
    };

    // =========================================================================
    // TRACKER -- the [Actors] lines
    // =========================================================================
    class Tracker
    {
    public:
        struct Config
        {
            std::size_t seenCap = 4096;
            double leaveGraceSec = 2.0;
        };

        Tracker() : Tracker(Config{}) {}
        explicit Tracker(Config config) : config_(config), seen_(config.seenCap) {}

        /// One target poll. `identify(refID) -> Identity` is called only for an
        /// actor not yet tracked in this fight. Lines (each with its "[Actors] "
        /// prefix) are appended to `out`. Outside a fight nothing is tracked.
        template <class IdentifyFn>
        void Poll(double nowSec, bool fighting, std::span<const Observation> polled,
                  std::uint32_t scoringRef, TargetType scoringType,
                  IdentifyFn&& identify, std::vector<std::string>& out)
        {
            if (!fighting) {
                if (inFight_) EndFight(nowSec, out);
                return;
            }
            if (!inFight_) {
                inFight_ = true;
                fightStartSec_ = nowSec;
                out.push_back(std::format("[Actors] fight started ({} tracked)", polled.size()));
            }

            ++pollStamp_;
            for (const auto& o : polled) {
                if (o.refID == 0) continue;
                auto it = actors_.find(o.refID);
                if (it == actors_.end()) {
                    Actor a;
                    a.id = identify(o.refID);
                    a.last = o;
                    a.everHostile = o.hostile;
                    a.died = o.dead;
                    a.stamp = pollStamp_;
                    const bool metBefore = seen_.Touch(o.refID);
                    out.push_back(metBefore ? AgainLine(o, a.id) : IdentityLine(o, a.id));
                    actors_.emplace(o.refID, std::move(a));
                    continue;
                }
                Actor& a = it->second;
                a.stamp = pollStamp_;
                if (a.left) {
                    // Back after its left line: a short reminder, then on as before.
                    a.left = false;
                    seen_.Touch(o.refID);
                    out.push_back(AgainLine(o, a.id));
                }
                a.absentSince = -1.0;
                Diff(a, o, out);
                a.last = o;
            }

            for (auto& [ref, a] : actors_) {
                if (a.stamp == pollStamp_ || a.left) continue;
                if (a.absentSince < 0.0) {
                    a.absentSince = nowSec;
                } else if (nowSec - a.absentSince >= config_.leaveGraceSec) {
                    a.left = true;
                    out.push_back(std::format("[Actors] {:08X} left tracking{}", ref, a.last.dead ? " (dead)" : ""));
                }
            }

            // Summary counts: a switch is a change between two different
            // actors, a flip a change between two real types; a gap with no
            // scoring target in between does not hide one.
            if (scoringRef != 0) {
                if (lastTargetRef_ != 0 && scoringRef != lastTargetRef_) ++targetSwitches_;
                lastTargetRef_ = scoringRef;
            }
            if (scoringType != TargetType::None) {
                if (lastTargetType_ != TargetType::None && scoringType != lastTargetType_) ++typeFlips_;
                lastTargetType_ = scoringType;
            }
        }

        /// A load (or a reset): the fight and the session are forgotten, quietly.
        void Reset()
        {
            ClearFight();
            seen_.Clear();
        }

        [[nodiscard]] bool InFight() const noexcept { return inFight_; }
        [[nodiscard]] std::size_t SeenCount() const noexcept { return seen_.Size(); }
        [[nodiscard]] std::size_t FightActorCount() const noexcept { return actors_.size(); }

    private:
        struct Actor
        {
            Identity id;
            Observation last;
            bool everHostile = false;
            bool died = false;
            bool left = false;
            double absentSince = -1.0;
            std::uint64_t stamp = 0;
        };

        static std::string IdentityLine(const Observation& o, const Identity& id)
        {
            return std::format("[Actors] +{:08X} '{}' race={} type={} hostile={} summon={} band={}",
                o.refID, id.name, id.raceEditorID.empty() ? std::string_view("?") : std::string_view(id.raceEditorID),
                State::GetTargetTypeName(o.type), o.hostile ? 1 : 0, SummonField(id), BandName(o.band));
        }

        static std::string AgainLine(const Observation& o, const Identity& id)
        {
            return std::format("[Actors] again {:08X} '{}' hostile={} band={}{}",
                o.refID, id.name, o.hostile ? 1 : 0, BandName(o.band), o.dead ? " (dead)" : "");
        }

        static void Diff(Actor& a, const Observation& o, std::vector<std::string>& out)
        {
            if (o.hostile != a.last.hostile) {
                out.push_back(std::format("[Actors] {:08X} hostile {}->{}", o.refID, a.last.hostile ? 1 : 0, o.hostile ? 1 : 0));
                a.everHostile |= o.hostile;
            }
            if (o.type != a.last.type) {
                out.push_back(std::format("[Actors] {:08X} type {}->{}", o.refID,
                    State::GetTargetTypeName(a.last.type), State::GetTargetTypeName(o.type)));
            }
            if (o.dead && !a.last.dead) {
                a.died = true;
                out.push_back(std::format("[Actors] {:08X} died{}", o.refID,
                    o.countedAlive ? " (the target list still counts it as a living hostile)" : ""));
            } else if (!o.dead && a.last.dead) {
                out.push_back(std::format("[Actors] {:08X} revived", o.refID));
            }
        }

        void EndFight(double nowSec, std::vector<std::string>& out)
        {
            std::array<std::size_t, 7> byType{};
            std::size_t summons = 0, hostile = 0, died = 0;
            for (const auto& entry : actors_) {
                const Actor& a = entry.second;
                const auto t = static_cast<std::size_t>(a.last.type);
                if (t < byType.size()) ++byType[t];
                summons += a.id.summon ? 1 : 0;
                hostile += a.everHostile ? 1 : 0;
                died += a.died ? 1 : 0;
            }
            std::string types;
            for (std::size_t t = 0; t < byType.size(); ++t) {
                if (byType[t] == 0) continue;
                types += std::format("{}{}={}", types.empty() ? "" : " ",
                    State::GetTargetTypeName(static_cast<TargetType>(t)), byType[t]);
            }
            const double secs = nowSec > fightStartSec_ ? nowSec - fightStartSec_ : 0.0;
            out.push_back(std::format(
                "[Actors] fight ended after {:.0f}s: {} actors ({}), {} summons, {} hostile, {} died, "
                "target switches={} type flips={}",
                secs, actors_.size(), types.empty() ? std::string("none") : types, summons, hostile, died,
                targetSwitches_, typeFlips_));
            ClearFight();
        }

        void ClearFight()
        {
            inFight_ = false;
            fightStartSec_ = 0.0;
            actors_.clear();
            lastTargetRef_ = 0;
            lastTargetType_ = TargetType::None;
            targetSwitches_ = 0;
            typeFlips_ = 0;
        }

        Config config_;
        SeenSet seen_;
        bool inFight_ = false;
        double fightStartSec_ = 0.0;
        std::uint64_t pollStamp_ = 0;
        std::unordered_map<std::uint32_t, Actor> actors_;
        std::uint32_t lastTargetRef_ = 0;
        TargetType lastTargetType_ = TargetType::None;
        std::size_t targetSwitches_ = 0;
        std::size_t typeFlips_ = 0;
    };

    // =========================================================================
    // TARGET WATCH -- the [Target] line
    // =========================================================================
    struct TargetInfo
    {
        std::uint32_t refID = 0;      // 0 = no scoring target
        std::string name;
        std::uint32_t baseID = 0;
        std::string raceEditorID;
        std::string raceName;
        TargetType type = TargetType::None;
        std::string rule;             // crosshair / sticky / closest
        bool hostile = false;
        bool dead = false;
        bool summon = false;
        std::string commander;
    };

    class TargetWatch
    {
    public:
        /// Has the scoring target changed actor or type since the last Commit?
        /// Cheap: the caller builds a TargetInfo only when it has.
        [[nodiscard]] bool Differs(std::uint32_t refID, TargetType type) const noexcept
        {
            return refID != current_.refID || type != current_.type;
        }

        /// Take `next` as the scoring target and return its [Target] line.
        [[nodiscard]] std::string Commit(TargetInfo next)
        {
            std::string line;
            if (next.refID == 0) {
                line = std::format("[Target] none (was {})", Short(current_));
            } else {
                Identity id{ next.name, next.baseID, next.raceEditorID, next.raceName, next.summon, next.commander };
                line = std::format("[Target] {:08X} '{}' base={:08X} race={} '{}' type={} rule={} hostile={} dead={} summon={}",
                    next.refID, next.name, next.baseID,
                    next.raceEditorID.empty() ? std::string_view("?") : std::string_view(next.raceEditorID),
                    next.raceName, State::GetTargetTypeName(next.type), next.rule,
                    next.hostile ? 1 : 0, next.dead ? 1 : 0, SummonField(id));
                if (current_.refID == next.refID) {
                    line += std::format(" -- same actor, type {}->{} (classifier flicker)",
                        State::GetTargetTypeName(current_.type), State::GetTargetTypeName(next.type));
                } else if (current_.refID != 0) {
                    line += std::format(" (was {})", Short(current_));
                }
            }
            current_ = std::move(next);
            return line;
        }

        [[nodiscard]] const TargetInfo& Current() const noexcept { return current_; }
        void Reset() { current_ = TargetInfo{}; }

    private:
        static std::string Short(const TargetInfo& t)
        {
            return std::format("{:08X} '{}' type={}", t.refID, t.name, State::GetTargetTypeName(t.type));
        }

        TargetInfo current_;
    };
}
