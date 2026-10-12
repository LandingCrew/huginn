#pragma once

// =============================================================================
// ACTOR LOG -- who was in the fight, and what the scoring target was
// =============================================================================
// Two logs, both transitions only (CLAUDE.md "Log transitions, not ticks"):
//
// [Target]  One line whenever the SCORING target (TargetCollection::
//           ScoringTargetType's pick: the living hostile primary, else the
//           closest living hostile) changes actor or classified type and the
//           new pick holds TargetWatch::DWELL_SEC (or the change has lasted
//           CEILING_SEC through flicker); shorter ones are counted on the next
//           line. A type change on the same actor says so -- that is
//           a classifier flicker, not a new enemy. In and out of fights.
//
// [Actors]  During a fight (the published combat flag), over the actors the
//           target poll already tracks -- no scan of its own:
//             +<ref> ...   the first time an actor is tracked this session
//             again <ref>  an actor seen before this session (or back after
//                          leaving this fight)
//             then by refID: hostility, type, died, revived, left tracking
//             (absent LEAVE_GRACE_SEC, so a range flap is not two lines)
//           and one summary line when the fight ends (identity and transition
//           lines capped per fight, Config::lineCap). Becoming or ceasing to
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
            // Identity and transition lines per fight. A combat flag stuck on
            // while walking a city would log every passer-by within 512 units;
            // past the cap they are counted, and the summary says how many.
            std::size_t lineCap = 80;
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
                    // Met this session only once its identity line has been
                    // shown: one the cap held back comes out in a later fight.
                    const bool metBefore = seen_.Contains(o.refID);
                    const bool shown = Emit(metBefore ? AgainLine(o, a.id) : IdentityLine(o, a.id), out);
                    if (shown || metBefore) seen_.Touch(o.refID);
                    actors_.emplace(o.refID, std::move(a));
                    continue;
                }
                Actor& a = it->second;
                a.stamp = pollStamp_;
                if (a.left) {
                    // Back after its left line: a short reminder, then on as before.
                    a.left = false;
                    seen_.Touch(o.refID);
                    Emit(AgainLine(o, a.id), out);
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
                    Emit(std::format("[Actors] {:08X} left tracking{}", ref, a.last.dead ? " (dead)" : ""), out);
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

        // Every identity and transition line goes through here: the per-fight cap.
        // True when the line went out (false: held back by the cap).
        bool Emit(std::string line, std::vector<std::string>& out)
        {
            if (linesThisFight_ < config_.lineCap) {
                ++linesThisFight_;
                out.push_back(std::move(line));
                return true;
            }
            if (suppressed_++ == 0) {
                out.push_back(std::format("[Actors] cap reached ({} lines this fight), further lines suppressed",
                    config_.lineCap));
            }
            return false;
        }

        void Diff(Actor& a, const Observation& o, std::vector<std::string>& out)
        {
            if (o.hostile != a.last.hostile) {
                Emit(std::format("[Actors] {:08X} hostile {}->{}", o.refID, a.last.hostile ? 1 : 0, o.hostile ? 1 : 0), out);
                a.everHostile |= o.hostile;
            }
            if (o.type != a.last.type) {
                Emit(std::format("[Actors] {:08X} type {}->{}", o.refID,
                    State::GetTargetTypeName(a.last.type), State::GetTargetTypeName(o.type)), out);
            }
            if (o.dead && !a.last.dead) {
                a.died = true;
                Emit(std::format("[Actors] {:08X} died{}", o.refID,
                    o.countedAlive ? " (the target list still counts it as a living hostile)" : ""), out);
            } else if (!o.dead && a.last.dead) {
                Emit(std::format("[Actors] {:08X} revived", o.refID), out);
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
                "target switches={} type flips={}{}",
                secs, actors_.size(), types.empty() ? std::string("none") : types, summons, hostile, died,
                targetSwitches_, typeFlips_,
                suppressed_ ? std::format(", {} lines suppressed", suppressed_) : std::string()));
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
            linesThisFight_ = 0;
            suppressed_ = 0;
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
        std::size_t linesThisFight_ = 0;   // identity + transition lines (Emit)
        std::size_t suppressed_ = 0;       // lines past Config::lineCap
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
        std::string rule;             // crosshair / sticky / closest / held
        bool hostile = false;
        bool dead = false;
        bool summon = false;
        std::string commander;
    };

    // The [Target] line waits for a pick to hold DWELL_SEC (3 target polls)
    // before it logs it. With the crosshair off the hostiles, the scoring
    // target is the closest one by raw distance, re-read every 100 ms, so two
    // bandits at about the same range alternate A/B/A/B at the poll rate --
    // ten lines a second for nothing anyone could act on. A pick that does
    // not last is counted, not logged, and the count rides on the next line:
    // "(+4 brief switches)". A type change on the same actor waits the same
    // way. The per-fight summary ([Actors] Tracker) counts every switch and
    // flip, brief or not.
    //
    // The dwell alone could hold a lasting change back forever: every new
    // candidate restarts its clock, so A/B flicker -- or a hostile at the edge
    // of range, there two polls and gone one -- never commits, and the line
    // kept saying "none" all fight. So a second clock runs from the first
    // poll the pick differs from the logged one; after CEILING_SEC the pick
    // of that moment is logged whatever it is. A return to the logged pick
    // stops that clock only once it has held DWELL_SEC itself, so a one-poll
    // excursion is quiet and a persistent flicker logs at most about one
    // line per CEILING_SEC. A brief count older than BRIEF_STALE_SEC is
    // dropped, so flicker from minutes ago never rides on a new line.
    class TargetWatch
    {
    public:
        static constexpr double DWELL_SEC = 0.3;
        static constexpr double CEILING_SEC = 1.0;
        static constexpr double BRIEF_STALE_SEC = 5.0;

        /// One target poll's pick. True when it differs from the last logged
        /// one and has held DWELL_SEC: the caller builds a TargetInfo and
        /// calls Commit. Cheap otherwise.
        [[nodiscard]] bool Observe(double nowSec, std::uint32_t refID, TargetType type) noexcept
        {
            if (brief_ > 0 && nowSec - lastBriefSec_ > BRIEF_STALE_SEC) {
                brief_ = 0;
            }
            if (refID == current_.refID && type == current_.type) {
                // Back to the logged pick: any candidate in between was brief.
                if (hasCandidate_) {
                    CountBrief(nowSec);
                    hasCandidate_ = false;
                }
                // The ceiling clock stops only once the logged pick holds again.
                if (sameSince_ < 0.0) sameSince_ = nowSec;
                if (differsSince_ >= 0.0 && nowSec - sameSince_ >= DWELL_SEC - kPollJitterSec) {
                    differsSince_ = -1.0;
                }
                return false;
            }
            sameSince_ = -1.0;
            if (differsSince_ < 0.0) differsSince_ = nowSec;
            if (hasCandidate_ && refID == candidateRef_ && type == candidateType_) {
                // A small tolerance: polls land at ~100 ms, not exactly on it.
                if (nowSec - candidateSince_ >= DWELL_SEC - kPollJitterSec) return true;
            } else {
                if (hasCandidate_) CountBrief(nowSec);   // replaced before it held
                hasCandidate_ = true;
                candidateRef_ = refID;
                candidateType_ = type;
                candidateSince_ = nowSec;
            }
            return nowSec - differsSince_ >= CEILING_SEC - kPollJitterSec;
        }

        /// Take `next` as the logged scoring target and return its [Target] line.
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
            if (brief_ > 0) {
                line += std::format(" (+{} brief switch{})", brief_, brief_ == 1 ? "" : "es");
                brief_ = 0;
            }
            hasCandidate_ = false;
            differsSince_ = -1.0;
            sameSince_ = -1.0;
            current_ = std::move(next);
            return line;
        }

        [[nodiscard]] const TargetInfo& Current() const noexcept { return current_; }
        [[nodiscard]] std::size_t PendingBrief() const noexcept { return brief_; }
        void Reset()
        {
            current_ = TargetInfo{};
            hasCandidate_ = false;
            differsSince_ = -1.0;
            sameSince_ = -1.0;
            brief_ = 0;
        }

    private:
        static constexpr double kPollJitterSec = 0.02;

        void CountBrief(double nowSec) noexcept
        {
            ++brief_;
            lastBriefSec_ = nowSec;
        }

        static std::string Short(const TargetInfo& t)
        {
            return std::format("{:08X} '{}' type={}", t.refID, t.name, State::GetTargetTypeName(t.type));
        }

        TargetInfo current_;
        bool hasCandidate_ = false;
        std::uint32_t candidateRef_ = 0;
        TargetType candidateType_ = TargetType::None;
        double candidateSince_ = 0.0;
        double differsSince_ = -1.0;   // first poll the pick differed from current_; < 0 = it does not
        double sameSince_ = -1.0;      // first poll back on current_; < 0 = not on it
        std::size_t brief_ = 0;
        double lastBriefSec_ = 0.0;
    };
}
