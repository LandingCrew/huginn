#include "DropAheadProbe.h"

#include "NeedSensorState.h"  // NeedClock

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

namespace Huginn::State::DropAheadProbe
{
   namespace
   {
      std::atomic<bool> g_gameLoaded{ false };

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

      void Store(Status status, float drop, double atSec)
      {
         std::lock_guard lock(g_mutex);
         g_latest = { status, drop, atSec };
      }

      void LogTransition(Status status, std::string_view detail)
      {
         if (static_cast<int>(status) == g_lastLoggedStatus) return;
         g_lastLoggedStatus = static_cast<int>(status);
         logger::info("[DropAhead] {}{}"sv, StatusName(status), detail);
      }

      void OnPlayerUpdate(RE::PlayerCharacter* player)
      {
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
            Store(s, -1.0f, nowSec);
            LogTransition(s, "");
         };
         if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
            g_lastPosAt = -1.0;  // a load: the next position is no movement
            return skip(Status::Loading);
         }
         if (!player->Get3D()) return skip(Status::No3D);
         auto* cell = player->GetParentCell();
         if (!cell) return skip(Status::NoCell);
         if (!cell->IsAttached()) return skip(Status::CellDetached);
         const RE::NiPointer<RE::bhkWorld> world(cell->GetbhkWorld());
         if (!world) return skip(Status::NoWorld);

         const RE::NiPoint3 pos = player->GetPosition();
         const Core::Needs::Vec3 feet{ pos.x, pos.y, pos.z };
         const float dt = g_lastPosAt < 0.0 ? 0.0f : static_cast<float>(nowSec - g_lastPosAt);
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
            RE::BSReadLockGuard lock(world->worldLock);
            hits = Core::Needs::ProbeAll(feet, dir, cfg, cast);
         }
         // Water at each reached point: the cell's water at that XY (a placed
         // water object or the cell plane). Outside the lock: no physics read.
         const auto starts = Core::Needs::ProbeStarts(feet, dir, cfg);
         int unknown = 0;
         for (std::size_t i = 0; i < hits.size(); ++i) {
            if (!hits[i].known) {
               ++unknown;
               continue;
            }
            float waterZ = 0.0f;
            RE::NiPoint3 at{ starts[i].x, starts[i].y, starts[i].z };
            if (cell->GetWaterHeight(at, waterZ) && std::isfinite(waterZ) && waterZ > -1.0e6f) {
               hits[i].waterKnown = true;
               hits[i].waterZ = waterZ;
            }
         }
         const float drop = Core::Needs::DropAhead(feet.z, hits, cfg);
         const Status status = drop < 0.0f ? Status::AllUnknown : Status::Measured;
         Store(status, drop, nowSec);
         if (static_cast<int>(status) == g_lastLoggedStatus) return;
         LogTransition(status, fmt::format(": drop {:.0f} | dir ({:.2f}, {:.2f}) | hits {}{}:{:.0f} {}{}:{:.0f} "
                                           "{}{}:{:.0f} | unknown {} rejected {} | feet z {:.0f}",
            drop, dir.x, dir.y, hits[0].known ? "" : "?", hits[0].hit, hits[0].hitZ, hits[1].known ? "" : "?",
            hits[1].hit, hits[1].hitZ, hits[2].known ? "" : "?", hits[2].hit, hits[2].hitZ, unknown, cast.rejected,
            feet.z));
      }

      void HookUpdate(RE::PlayerCharacter* a_this, float a_delta)
      {
         g_originalUpdate(a_this, a_delta);
         OnPlayerUpdate(a_this);
      }
   }

   bool InstallPlayerUpdateHook()
   {
      REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
      g_originalUpdate = vtbl.write_vfunc(0xAD, HookUpdate);
      return g_originalUpdate.address() != 0;
   }

   void SetGameLoaded(bool loaded) noexcept { g_gameLoaded.store(loaded, std::memory_order_release); }

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
