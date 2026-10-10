#include "DropAheadProbe.h"

#include "NeedSensorState.h"  // NeedClock
#include "Profiling.h"
#include "StateConstants.h"  // PhysicsConstants: HEAD_HEIGHT, INVALID_WATER_HEIGHT_VALUE
#include "core/WaterSelfCheck.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

namespace Huginn::State::DropAheadProbe
{
   namespace
   {
      std::atomic<bool> g_gameLoaded{ false };
      std::atomic<std::uint32_t> g_measuredCount{ 0 };
      // Set by the hook on the first XCLW "use the default" sentinel it reads,
      // cleared by SetGameLoaded(true): one log line per game load (0.23.22).
      std::atomic<bool> g_loggedFirstSentinel{ false };
      // Bumped by every SetGameLoaded (any thread); the hook sees the change
      // and clears its own per-load state (the false-water blacklist, the
      // hold, the swim-check log flags) on its own thread, so that state is
      // never touched from two threads (0.23.23).
      std::atomic<std::uint32_t> g_loadGeneration{ 0 };

      std::mutex g_mutex;
      Reading g_latest;

      // Hook-thread only (PlayerCharacter::Update): the throttle, the last
      // position for the movement direction, and the transition log.
      std::chrono::steady_clock::time_point g_lastProbe{};
      Core::Needs::Vec3 g_lastPos{};
      double g_lastPosAt = -1.0;
      int g_lastLoggedStatus = -1;
      bool g_loggedThread = false;
      std::size_t g_hookThread = 0;

      // The water self-check (0.23.23, core/WaterSelfCheck.h). Hook-thread
      // only, like the throttle above; reset per game load through
      // g_loadGeneration.
      std::uint32_t g_seenGeneration = 0;
      Core::Needs::WaterPlaneBlacklist g_falseWater;
      Core::Needs::FalseWaterHold g_falseWaterHold;
      bool g_swimMatchLogged = false;
      bool g_swimMismatchLogged = false;

      constexpr std::chrono::milliseconds kInterval{ 100 };

      // A down ray that hits an actor, a dropped item or clutter is cast on
      // from just past the hit, at most this many times; running out makes
      // the probe unknown, never a drop.
      constexpr int kMaxRecasts = 8;

      using Update_t = void(RE::PlayerCharacter*, float);
      REL::Relocation<Update_t> g_originalUpdate;

      std::uint32_t RayFilter(RE::PlayerCharacter* player)
      {
         // The LOS layer, in the PLAYER's own system group, so the player's
         // own bodies never block a ray that starts inside them.
         std::uint32_t info = 0;
         player->GetCollisionFilterInfo(info);
         return (info & 0xFFFF0000u) | static_cast<std::uint32_t>(RE::COL_LAYER::kLOS);
      }

      // One Havok pick; caller holds the world's read lock.
      bool Pick(RE::bhkWorld* world, std::uint32_t filter, Core::Needs::Vec3 a, Core::Needs::Vec3 b,
                float& fraction, RE::COL_LAYER& layer)
      {
         const float scale = RE::bhkWorld::GetWorldScale();
         RE::bhkPickData pick{};
         pick.rayInput.from = RE::hkVector4(a.x * scale, a.y * scale, a.z * scale, 0.0f);
         pick.rayInput.to = RE::hkVector4(b.x * scale, b.y * scale, b.z * scale, 0.0f);
         pick.rayInput.enableShapeCollectionFilter = false;
         pick.rayInput.filterInfo = filter;
         world->PickObject(pick);
         if (!pick.rayOutput.HasHit()) return false;
         fraction = pick.rayOutput.hitFraction;
         const auto* collidable = pick.rayOutput.rootCollidable;
         layer = collidable ? collidable->GetCollisionLayer() : RE::COL_LAYER::kUnidentified;
         return true;
      }

      // The ray cast core's ProbeAll runs on. Horizontal: any hit blocks.
      // Down: cast on through non-ground layers.
      struct HavokCast
      {
         RE::bhkWorld* world;
         std::uint32_t filter;
         int rejected = 0;

         Core::Needs::RayResult operator()(Core::Needs::Vec3 from, Core::Needs::Vec3 dir, float length,
                                           Core::Needs::RayKind kind)
         {
            using Core::Needs::RayResult;
            float travelled = 0.0f;
            for (int attempt = 0; attempt <= kMaxRecasts; ++attempt) {
               const float remaining = length - travelled;
               if (remaining <= 1.0f) return { RayResult::Outcome::Clear, length };
               const Core::Needs::Vec3 a{ from.x + dir.x * travelled, from.y + dir.y * travelled,
                                          from.z + dir.z * travelled };
               const Core::Needs::Vec3 b{ a.x + dir.x * remaining, a.y + dir.y * remaining, a.z + dir.z * remaining };
               float fraction = 0.0f;
               RE::COL_LAYER layer{};
               if (!Pick(world, filter, a, b, fraction, layer)) return { RayResult::Outcome::Clear, length };
               const float at = travelled + remaining * fraction;
               if (kind == Core::Needs::RayKind::Horizontal || IsGroundLayer(layer)) {
                  return { RayResult::Outcome::Hit, at };
               }
               ++rejected;
               travelled = at + 1.0f;
            }
            return { RayResult::Outcome::Exhausted, 0.0f };
         }
      };

      // The cell whose water lies at a probe point. In an interior the
      // player's parent cell is the cell. Outdoors a point up to 280 units
      // ahead can lie in the neighbouring exterior cell, and only that cell's
      // water is there: reading the player's cell would put a lake cell's
      // surface over the next, lower, dry cell and zero a real cliff (0.23.19
      // fix round). TES::GetCell (CommonLib v3.7.0 include/RE/T/TES.h:72,
      // `TESObjectCELL* GetCell(const NiPoint3& a_position) const`) finds the
      // exterior cell at a position. A cell not found, interior, or not
      // attached (not loaded): the water there is unknown.
      enum class WaterCell : std::uint8_t { Own, Neighbour, Unknown };

      RE::TESObjectCELL* CellForWater(RE::TESObjectCELL* parent, const RE::NiPoint3& at, WaterCell& which)
      {
         which = WaterCell::Unknown;
         if (parent->IsInteriorCell()) {
            which = WaterCell::Own;
            return parent;
         }
         auto* tes = RE::TES::GetSingleton();
         auto* cell = tes ? tes->GetCell(at) : nullptr;
         if (!cell || !cell->IsExteriorCell() || !cell->IsAttached()) return nullptr;
         which = cell == parent ? WaterCell::Own : WaterCell::Neighbour;
         return cell;
      }

      // One point's water, as the game returned it and as it was resolved.
      struct PointWater
      {
         RE::TESObjectCELL* cell = nullptr;  // the cell read (CellForWater); null: water unknown
         WaterCell which = WaterCell::Unknown;
         float raw = std::numeric_limits<float>::quiet_NaN();  // what GetWaterHeight returned; NaN: it returned false
         bool askedDefault = false;
         float resolvedDefault = std::numeric_limits<float>::quiet_NaN();
         Core::Needs::WaterHeightRead water;  // resolved (Core::Needs::ResolveWaterHeight)
         bool blacklisted = false;  // resolved onto a plane proven false this load: water unknown

         [[nodiscard]] bool Usable() const noexcept { return water.known && !blacklisted; }
         [[nodiscard]] std::uint32_t CellId() const noexcept { return cell ? cell->GetFormID() : 0; }
      };

      // The water of the cell the point lies in (CellForWater), at that XY (a
      // placed water object or the cell plane). The XCLW "use the worldspace
      // default" sentinel is resolved to that default (0.23.21: the sea and
      // many lakes rely on it); a height the game uses for "no water"
      // (-infinity, a sentinel with no usable default) is not water
      // (Core::Needs::ResolveWaterHeight). Water on a plane the self-check
      // proved false this load (g_falseWater) is unknown. Hook thread only.
      PointWater ReadWater(RE::TESObjectCELL* parent, const RE::NiPoint3& at)
      {
         PointWater out;
         out.cell = CellForWater(parent, at, out.which);
         if (!out.cell) return out;
         float waterZ = 0.0f;
         if (!out.cell->GetWaterHeight(at, waterZ)) return out;
         out.raw = waterZ;
         // The sentinel (XCLW "use the worldspace default") in place of
         // a height: resolve it. CommonLib v3.7.0
         // src/RE/T/TESObjectCELL.cpp:88-100 GetExteriorWaterHeight
         // (field reads in CommonLib, no relocated engine call) returns -FLT_MAX
         // (NI_INFINITY = FLT_MAX, include/RE/N/NiMath.h:5) for a cell
         // without the kHasWater flag or an interior; the cell's own
         // XCLW when below 2147483600; else the worldspace's
         // GetDefaultWaterHeight (src/RE/T/TESWorldSpace.cpp:17-24: the
         // DNAM default water of the worldspace, or of its parent while
         // it uses the parent's land data), or -FLT_MAX with no
         // worldspace. -FLT_MAX, and a default that is itself no
         // height, end as water unknown (Core::Needs::ResolveWaterHeight).
         std::optional<float> resolvedDefault;
         if (Core::Needs::IsDefaultWaterSentinel(waterZ) && out.cell->IsExteriorCell()) {
            resolvedDefault = out.cell->GetExteriorWaterHeight();
            out.resolvedDefault = *resolvedDefault;
            out.askedDefault = true;
         }
         // The status line that marks a resolved default is rate-limited
         // (one per 5 s, on a status change), so the first sentinel of
         // each game load gets its own line: whether the engine hands
         // the sentinel back at all is open (implementation map, Known
         // limits). One line per load: a transition, not a tick.
         if (Core::Needs::IsDefaultWaterSentinel(waterZ) &&
             !g_loggedFirstSentinel.exchange(true, std::memory_order_relaxed)) {
            logger::debug("[DropAhead] first water-default sentinel this game load: raw {:.0f} -> {}"sv, waterZ,
               resolvedDefault ? fmt::format("cell or worldspace default {:g}{}", *resolvedDefault,
                                     Core::Needs::IsUsableWaterHeight(*resolvedDefault) ? "" : " (no usable height: water unknown)")
                               : std::string("not asked (an interior: water unknown)"));
         }
         out.water = Core::Needs::ResolveWaterHeight(waterZ, resolvedDefault);
         out.blacklisted = out.water.known && g_falseWater.Matches(out.CellId(), out.water.z);
         return out;
      }

      // "cell 0001A2B3 'EditorID' (exterior 12,-4)", for the one-off lines.
      std::string DescribeCell(RE::TESObjectCELL* cell)
      {
         if (!cell) return "cell ?";
         std::string out = fmt::format("cell {:08X}", cell->GetFormID());
         if (const char* edid = cell->GetFormEditorID(); edid && *edid) out += fmt::format(" '{}'", edid);
         if (cell->IsExteriorCell()) {
            if (const auto* c = cell->GetCoordinates()) out += fmt::format(" (exterior {},{})", c->cellX, c->cellY);
         } else {
            out += " (interior)";
         }
         return out;
      }

      // The water self-check (0.23.23, core/WaterSelfCheck.h): the probe's
      // water read at the player's own XY, against what the engine says of
      // the player. Run every probe, before the airborne / swimming /
      // mounted skips. Cost, reasoned, not measured: on the ground, one more
      // TES::GetCell and TESObjectCELL::GetWaterHeight -- the same pair the
      // probe already makes for each of up to three points -- every 100 ms,
      // outside the physics lock; swimming, the same until both swim lines
      // of this load are written (and while a blacklisted plane exists);
      // airborne or mounted, nothing. Its own Tracy zone tells a trace.
      void SelfCheckWater(RE::PlayerCharacter* player, RE::TESObjectCELL* parent, const Core::Needs::Vec3& feet,
                          bool airborne, bool swimming, bool mounted, double nowSec)
      {
         const bool groundCheck = !airborne && !mounted && !swimming;
         const bool swimCheck =
            swimming && (!g_swimMatchLogged || !g_swimMismatchLogged || g_falseWater.Size() > 0);
         if (!groundCheck) g_falseWaterHold.Reset();
         if (!groundCheck && !swimCheck) return;

         Huginn_ZONE_NAMED("DropAhead::OwnWater");
         const auto origin = Core::Needs::ProbeOrigin(feet, Core::Needs::DropProbeConfig{});
         const auto own = ReadWater(parent, RE::NiPoint3{ origin.x, origin.y, origin.z });
         // The engine's own water for the player (relevantWaterHeight, else
         // the parent cell's plane) and the head test, exactly as
         // StateManager::PollPlayerPosition decides `underwater`; read here,
         // on the main thread, rather than copied from the StateManager.
         //
         // The blind spot (core/WaterSelfCheck.h): GetWaterHeight falls back
         // to parentCell->GetExteriorWaterHeight() when relevantWaterHeight
         // is -infinity (CommonLib v3.7.0 src/RE/T/TESObjectREFR.cpp:496-508),
         // so a dry player under the exterior cell's OWN plane (its XCLW or
         // the worldspace default) reads `underwater` here and the
         // contradiction below never fires for that plane. It still catches
         // a placed water object, a height in a cell without kHasWater and an
         // interior plane (GetExteriorWaterHeight is -infinity indoors). The
         // StateManager logs the blind spot's own symptom (underwater, not
         // swimming, the water far over the head) once per load.
         //
         // HEAD_HEIGHT is a fixed 120 (StateConstants.h), not scaled by race
         // or GetScale(), the same as the StateManager's test, which this one
         // must match, so it is left unscaled here. It errs safe: the test
         // reads "under" once the engine's water is 120 over the feet, and a
         // player read as under is skipped, so wherever the probe's plane is
         // the engine's water no scale can make the check fire (the 150 of
         // kFalseWaterDepth counts from the feet and is over 120). Scale
         // matters only where the two differ (a placed water object the
         // engine does not hand back): a ~1.5x-scale player swims with the
         // feet ~135 under the surface (the logged 83-100 times 1.5), close
         // to 150. A swim is skipped by the engine's swim flag either way;
         // only a dry moment that deep held 0.5 s (wading out) could fire.
         // Not measured with a scaled player.
         const float engineWater = player->GetWaterHeight();
         const bool engineWaterUsable = engineWater > PhysicsConstants::INVALID_WATER_HEIGHT_VALUE;
         const bool underwater = engineWaterUsable && feet.z + PhysicsConstants::HEAD_HEIGHT < engineWater;
         const std::string engineText =
            engineWaterUsable ? fmt::format("{:.0f}", engineWater) : fmt::format("none ({:g})", engineWater);

         if (swimming) {
            using Core::Needs::SwimWaterCheck;
            // A blacklisted plane the player swims in, at the engine's own
            // height: real after all. Off the list.
            if (own.blacklisted &&
                Core::Needs::CheckSwimWater(true, true, own.water.z, engineWater) == SwimWaterCheck::Match &&
                g_falseWater.Remove(own.CellId(), own.water.z)) {
               logger::info("[DropAhead] false water withdrawn: {} plane z {:.0f} is the water the player swims in "
                            "(the engine's water for the player {}); trusted again"sv,
                  DescribeCell(own.cell), own.water.z, engineText);
            }
            const auto check = Core::Needs::CheckSwimWater(true, own.water.known, own.water.z, engineWater);
            if (check == SwimWaterCheck::Match && !g_swimMatchLogged) {
               g_swimMatchLogged = true;
               logger::debug("[DropAhead] water check: swimming at ({:.0f}, {:.0f}, {:.0f}), the probe's water at the "
                             "player {:.0f} ({}) matches the engine's {} (once per load)"sv,
                  feet.x, feet.y, feet.z, own.water.z, DescribeCell(own.cell), engineText);
            } else if (check == SwimWaterCheck::Mismatch && !g_swimMismatchLogged) {
               g_swimMismatchLogged = true;
               logger::debug("[DropAhead] water check MISMATCH: swimming at ({:.0f}, {:.0f}, {:.0f}), the probe's water "
                             "at the player {:.0f} ({}) differs from the engine's {} by {:.0f} (once per load)"sv,
                  feet.x, feet.y, feet.z, own.water.z, DescribeCell(own.cell), engineText,
                  std::fabs(own.water.z - engineWater));
            }
            return;
         }

         Core::Needs::OwnWaterSample sample;
         sample.waterKnown = own.Usable();
         sample.waterZ = own.water.z;
         sample.feetZ = feet.z;
         sample.swimming = swimming;
         sample.underwater = underwater;
         sample.airborne = airborne;
         sample.mounted = mounted;
         const bool contradicts = Core::Needs::WaterContradictsPlayer(sample);
         if (!g_falseWaterHold.Update(nowSec, contradicts, { own.CellId(), own.water.z })) return;
         const double held = g_falseWaterHold.HeldSec(nowSec);
         const auto added = g_falseWater.Add({ own.CellId(), own.water.z });
         // Known: on the list already. FullAgain: the list is full and this
         // load's line saying so is written (once per load, not per hold).
         if (added == Core::Needs::WaterPlaneBlacklist::AddResult::Known ||
             added == Core::Needs::WaterPlaneBlacklist::AddResult::FullAgain) {
            return;
         }
         logger::info("[DropAhead] false water: {} plane z {:.0f} is {:.0f} above the feet at ({:.0f}, {:.0f}, {:.0f}) "
                      "but the player is not in water (on the ground, not swimming, head not under the engine's water "
                      "for the player: {}; held {:.1f} s){}"sv,
            DescribeCell(own.cell), own.water.z, own.water.z - feet.z, feet.x, feet.y, feet.z, engineText, held,
            added == Core::Needs::WaterPlaneBlacklist::AddResult::Added
               ? std::string("; water from that cell at that height is unknown until the next load")
               : fmt::format("; the blacklist is full ({} planes), so it stays trusted (said once: a false "
                             "plane that does not fit is not logged again until the next load)",
                             Core::Needs::WaterPlaneBlacklist::kCapacity));
      }

      void Store(Status status, float drop, float waterDepth, double atSec)
      {
         std::lock_guard lock(g_mutex);
         g_latest = { status, drop, waterDepth, atSec };
      }

      // The status line (0.23.16): debug, and at most one per
      // kTransitionLogGapSec. Walking along an edge flips measured <-> every
      // probe unknown several times a second: 376 info lines in 18 minutes of
      // the LoreRim R3 session. A line says how many changes it stands for.
      constexpr double kTransitionLogGapSec = 5.0;
      double g_lastTransitionLogAt = -1.0e9;
      int g_lastSeenStatus = -1;
      std::uint32_t g_changesSinceLog = 0;

      bool WantTransitionLog(Status status, double nowSec)
      {
         if (static_cast<int>(status) != g_lastSeenStatus) {
            g_lastSeenStatus = static_cast<int>(status);
            ++g_changesSinceLog;
         }
         if (static_cast<int>(status) == g_lastLoggedStatus) return false;
         return nowSec - g_lastTransitionLogAt >= kTransitionLogGapSec;
      }

      void LogTransition(Status status, double nowSec, std::string_view detail)
      {
         g_lastLoggedStatus = static_cast<int>(status);
         g_lastTransitionLogAt = nowSec;
         logger::debug("[DropAhead] {}{}{}"sv, StatusName(status), detail,
            g_changesSinceLog > 1 ? fmt::format(" ({} status changes since the last line)", g_changesSinceLog)
                                  : std::string());
         g_changesSinceLog = 0;
      }

      void OnPlayerUpdate(RE::PlayerCharacter* player)
      {
         Huginn_ZONE_NAMED("DropAhead::PlayerUpdateHook");
         if (!g_gameLoaded.load(std::memory_order_acquire) || !player) return;
         if (const auto gen = g_loadGeneration.load(std::memory_order_acquire); gen != g_seenGeneration) {
            // A game load (SetGameLoaded): what was proven false belongs to the last one.
            g_seenGeneration = gen;
            g_falseWater.Clear();
            g_falseWaterHold.Reset();
            g_swimMatchLogged = false;
            g_swimMismatchLogged = false;
         }
         const auto now = std::chrono::steady_clock::now();
         if (now - g_lastProbe < kInterval) return;
         g_lastProbe = now;
         const double nowSec = NeedClock::Seconds(now);

         // The thread, once, and again whenever it changes (evidence that the
         // hook runs on one thread, the main one, throughout).
         const std::size_t thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
         if (!g_loggedThread || thread != g_hookThread) {
            logger::info("[DropAhead] {} PlayerCharacter::Update on thread {:x}"sv,
               g_loggedThread ? "THREAD CHANGED: now probing from" : "probing from", thread);
            g_loggedThread = true;
            g_hookThread = thread;
         }

         auto skip = [&](Status s) {
            // A skip before the self-check ran (a load, no 3D, no cell ...)
            // breaks its hold; airborne / swimming / mounted it ran already.
            if (s != Status::Airborne && s != Status::Swimming && s != Status::Mounted) g_falseWaterHold.Reset();
            Store(s, -1.0f, -1.0f, nowSec);
            if (WantTransitionLog(s, nowSec)) LogTransition(s, nowSec, "");
         };
         if (auto* ui = RE::UI::GetSingleton()) {
            // Quit to the main menu: the player singleton outlives the world
            // (CLAUDE.md memory: player teardown is not a null pointer). Off
            // until the next kPostLoadGame / kNewGame turns it back on.
            if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
               g_gameLoaded.store(false, std::memory_order_release);
               g_lastPosAt = -1.0;
               Store(Status::NotLoaded, -1.0f, -1.0f, -1.0);  // no reading: nothing for a paused age to keep
               if (WantTransitionLog(Status::NotLoaded, nowSec)) LogTransition(Status::NotLoaded, nowSec, "");
               return;
            }
            if (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
               g_lastPosAt = -1.0;  // a load: the next position is no movement
               return skip(Status::Loading);
            }
         }
         if (!player->Get3D()) return skip(Status::No3D);
         auto* cell = player->GetParentCell();
         if (!cell) return skip(Status::NoCell);
         if (!cell->IsAttached()) return skip(Status::CellDetached);
         const RE::NiPointer<RE::bhkWorld> world(cell->GetbhkWorld());
         if (!world) return skip(Status::NoWorld);

         const RE::NiPoint3 pos = player->GetPosition();
         const Core::Needs::Vec3 feet{ pos.x, pos.y, pos.z };
         // A teleport (coc, a load door, fast travel) is no heading: the old
         // to new position would aim the probes anywhere. Use the facing.
         float dt = g_lastPosAt < 0.0 ? 0.0f : static_cast<float>(nowSec - g_lastPosAt);
         if (dt > 0.0f && Core::Needs::IsTeleport(g_lastPos, feet)) {
            logger::debug("[DropAhead] a {:.0f}-unit jump: a teleport, heading from the facing"sv,
               std::hypot(feet.x - g_lastPos.x, feet.y - g_lastPos.y, feet.z - g_lastPos.z));
            dt = 0.0f;
         }
         const auto velocity = Core::Needs::HorizontalVelocity(g_lastPos, feet, dt);
         g_lastPos = feet;
         g_lastPosAt = nowSec;

         // Airborne the feet are on nothing (the fall has its own need),
         // swimming the surface is the water, mounted the player's Z is the
         // saddle, not the horse's feet. The water self-check runs first: it
         // needs these states, and swimming is where it confirms a reading.
         const bool airborne = player->IsInMidair();
         const auto* actorState = player->AsActorState();
         const bool swimming = actorState && actorState->IsSwimming();
         const bool mounted = player->IsOnMount();
         SelfCheckWater(player, cell, feet, airborne, swimming, mounted, nowSec);
         if (airborne) return skip(Status::Airborne);
         if (swimming) return skip(Status::Swimming);
         if (mounted) return skip(Status::Mounted);

         const Core::Needs::DropProbeConfig cfg;
         const auto dir = Core::Needs::ProbeDirection(velocity, player->GetAngleZ(), cfg.minMoveSpeed);
         HavokCast cast{ world.get(), RayFilter(player) };
         std::array<Core::Needs::ProbeHit, 3> hits{};
         {
            Huginn_ZONE_NAMED("DropAhead::ProbeAll (ray casts)");
            RE::BSReadLockGuard lock(world->worldLock);
            hits = Core::Needs::ProbeAll(feet, dir, cfg, cast);
         }
         // Water at each reached point: the water of the cell the point lies in
         // (ReadWater), at that XY. Sampled at the ray's real start -- the
         // point's XY, which ProbeAll shares with ProbeStarts, and the Z it
         // recorded -- so the read and the ray agree; for a water plane the Z
         // does not matter. Outside the lock: no physics read. The down ray
         // does not stop at water (IsGroundLayer), so its hit is the bed and
         // water above it gives the depth (core MeasureAhead). Water on a
         // plane the self-check proved false this load is unknown (0.23.23).
         const auto starts = Core::Needs::ProbeStarts(feet, dir, cfg);
         int unknown = 0;
         int neighbourCells = 0;
         int unknownCells = 0;
         std::array<PointWater, 3> water{};  // what the game returned, for the log
         for (std::size_t i = 0; i < hits.size(); ++i) {
            if (!hits[i].known) {
               ++unknown;
               continue;
            }
            const RE::NiPoint3 at{ starts[i].x, starts[i].y, hits[i].startZ.value_or(starts[i].z) };
            water[i] = ReadWater(cell, at);
            if (water[i].which == WaterCell::Neighbour) ++neighbourCells;
            if (!water[i].cell) {
               ++unknownCells;
               continue;
            }
            hits[i].waterKnown = water[i].Usable();
            hits[i].waterZ = hits[i].waterKnown ? water[i].water.z : 0.0f;
         }
         // One pass, two readings: the drop (water deep enough to land in is
         // no drop) and the deepest water ahead (deep_water_ahead).
         const auto ahead = Core::Needs::MeasureAhead(feet.z, hits, cfg);
         const float drop = ahead.drop;
         const Status status = drop < 0.0f ? Status::AllUnknown : Status::Measured;
         Store(status, drop, ahead.waterDepth, nowSec);
         if (status == Status::Measured) g_measuredCount.fetch_add(1, std::memory_order_relaxed);
         if (!WantTransitionLog(status, nowSec)) return;
         // Per probe: the hit Z (or "void"), the RAW water height the game
         // returned ("-" when none; the sentinel shows here as the huge
         // number it is, followed by "(default <z>)" with the resolved
         // default -- the cell's own XCLW or the worldspace default, as
         // GetExteriorWaterHeight returns either -- "(default none <z>)"
         // when that was no usable height, or
         // "(default not asked)" in an interior; then "(false: ignored)" when
         // it lies on a plane the self-check proved false this load) and the
         // depth that came of it, so a line shows why a drop was discounted,
         // whether the water reading was false, and which path the water took.
         auto probe = [&](std::size_t i) -> std::string {
            const auto& h = hits[i];
            if (!h.known) return "?";
            const std::string hit = h.hit ? fmt::format("{:.0f}", h.hitZ) : std::string("void");
            const auto& w = water[i];
            std::string text = std::isnan(w.raw) ? std::string("-") : fmt::format("{:.0f}", w.raw);
            if (!std::isnan(w.raw) && Core::Needs::IsDefaultWaterSentinel(w.raw)) {
               if (!w.askedDefault) {
                  text += " (default not asked)";
               } else if (Core::Needs::IsUsableWaterHeight(w.resolvedDefault)) {
                  text += fmt::format(" (default {:.0f})", w.resolvedDefault);
               } else {
                  text += fmt::format(" (default none {:g})", w.resolvedDefault);
               }
            }
            if (w.blacklisted) text += " (false: ignored)";
            return fmt::format("hit {} water {} depth {:.0f}", hit, text,
                               Core::Needs::WaterDepthAtProbe(feet.z, h, cfg));
         };
         LogTransition(status, nowSec, fmt::format(": drop {:.0f} | deep water {:.0f} (safe landing >= {:.0f}) | dir "
                                           "({:.2f}, {:.2f}) | probes [{}] [{}] [{}] | water cells: {} neighbour, "
                                           "{} unknown | unknown {} rejected {} | feet z {:.0f}",
            drop, ahead.waterDepth, Core::Needs::kSafeLandingDepth, dir.x, dir.y, probe(0), probe(1), probe(2),
            neighbourCells, unknownCells, unknown, cast.rejected, feet.z));
      }

      void HookUpdate(RE::PlayerCharacter* a_this, float a_delta)
      {
         g_originalUpdate(a_this, a_delta);
         // Nothing of ours may unwind into the game's frames.
         try {
            OnPlayerUpdate(a_this);
         } catch (...) {
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true)) {
               logger::error("[DropAhead] an exception in the probe was caught (logged once); drop ahead unmeasured"sv);
            }
            Store(Status::NotLoaded, -1.0f, -1.0f, -1.0);
         }
      }
   }

   bool InstallPlayerUpdateHook()
   {
      REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
      g_originalUpdate = vtbl.write_vfunc(0xAD, HookUpdate);
      return g_originalUpdate.address() != 0;
   }

   void SetGameLoaded(bool loaded) noexcept
   {
      if (loaded) g_loggedFirstSentinel.store(false, std::memory_order_relaxed);
      // The hook clears the false-water blacklist on its own thread when it
      // sees this change (g_loadGeneration).
      g_loadGeneration.fetch_add(1, std::memory_order_acq_rel);
      g_gameLoaded.store(loaded, std::memory_order_release);
      Store(Status::NotLoaded, -1.0f, -1.0f, -1.0);
   }

   std::uint32_t MeasuredCount() noexcept { return g_measuredCount.load(std::memory_order_relaxed); }

   Reading Latest() noexcept
   {
      std::lock_guard lock(g_mutex);
      return g_latest;
   }

   std::string_view StatusName(Status s) noexcept
   {
      switch (s) {
      case Status::Measured: return "measured";
      case Status::AllUnknown: return "measured, every probe unknown";
      case Status::NotLoaded: return "skipped: no game loaded";
      case Status::Loading: return "skipped: loading";
      case Status::No3D: return "skipped: no player 3D";
      case Status::NoCell: return "skipped: no parent cell";
      case Status::CellDetached: return "skipped: cell not attached";
      case Status::NoWorld: return "skipped: no physics world";
      case Status::Airborne: return "skipped: airborne";
      case Status::Swimming: return "skipped: swimming";
      case Status::Mounted: return "skipped: mounted";
      }
      return "?";
   }

   bool IsGroundLayer(RE::COL_LAYER layer) noexcept
   {
      switch (layer) {
      case RE::COL_LAYER::kStatic:
      case RE::COL_LAYER::kAnimStatic:
      case RE::COL_LAYER::kTransparent:
      case RE::COL_LAYER::kTrees:
      case RE::COL_LAYER::kProps:
      case RE::COL_LAYER::kTerrain:
      case RE::COL_LAYER::kGround:
         return true;
      default:
         return false;
      }
   }
}
