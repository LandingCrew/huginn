#include "DropAheadProbe.h"

#include "NeedSensorState.h"  // NeedClock
#include "Profiling.h"

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
         // saddle, not the horse's feet.
         if (player->IsInMidair()) return skip(Status::Airborne);
         if (auto* st = player->AsActorState(); st && st->IsSwimming()) return skip(Status::Swimming);
         if (player->IsOnMount()) return skip(Status::Mounted);

         const Core::Needs::DropProbeConfig cfg;
         const auto dir = Core::Needs::ProbeDirection(velocity, player->GetAngleZ(), cfg.minMoveSpeed);
         HavokCast cast{ world.get(), RayFilter(player) };
         std::array<Core::Needs::ProbeHit, 3> hits{};
         {
            Huginn_ZONE_NAMED("DropAhead::ProbeAll (ray casts)");
            RE::BSReadLockGuard lock(world->worldLock);
            hits = Core::Needs::ProbeAll(feet, dir, cfg, cast);
         }
         // Water at each reached point: the water of the cell the point lies
         // in (CellForWater), at that XY (a placed water object or the cell
         // plane). Sampled at the ray's real start -- the point's XY, which
         // ProbeAll shares with ProbeStarts, and the Z it recorded -- so the
         // read and the ray agree; for a water plane the Z does not matter.
         // Outside the lock: no physics read. The down ray does not stop at
         // water (IsGroundLayer), so its hit is the bed and water above it
         // gives the depth (core MeasureAhead). A height the game uses for
         // "no water" (the XCLW default sentinel, -infinity) is not water
         // (Core::Needs::IsUsableWaterHeight).
         const auto starts = Core::Needs::ProbeStarts(feet, dir, cfg);
         int unknown = 0;
         int neighbourCells = 0;
         int unknownCells = 0;
         constexpr float kNoRead = std::numeric_limits<float>::quiet_NaN();
         std::array<float, 3> rawWater{ kNoRead, kNoRead, kNoRead };  // what the game returned, for the log
         for (std::size_t i = 0; i < hits.size(); ++i) {
            if (!hits[i].known) {
               ++unknown;
               continue;
            }
            const RE::NiPoint3 at{ starts[i].x, starts[i].y, hits[i].startZ.value_or(starts[i].z) };
            WaterCell which = WaterCell::Unknown;
            auto* waterCell = CellForWater(cell, at, which);
            if (which == WaterCell::Neighbour) ++neighbourCells;
            if (!waterCell) {
               ++unknownCells;
               continue;
            }
            float waterZ = 0.0f;
            if (waterCell->GetWaterHeight(at, waterZ)) {
               rawWater[i] = waterZ;
               if (Core::Needs::IsUsableWaterHeight(waterZ)) {
                  hits[i].waterKnown = true;
                  hits[i].waterZ = waterZ;
               }
            }
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
         // returned ("-" when none; a rejected sentinel shows here as the huge
         // number it is) and the depth that came of it, so a line shows both
         // why a drop was discounted and whether the water reading was false.
         auto probe = [&](std::size_t i) -> std::string {
            const auto& h = hits[i];
            if (!h.known) return "?";
            const std::string hit = h.hit ? fmt::format("{:.0f}", h.hitZ) : std::string("void");
            const std::string water = std::isnan(rawWater[i]) ? std::string("-") : fmt::format("{:.0f}", rawWater[i]);
            return fmt::format("hit {} water {} depth {:.0f}", hit, water,
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
