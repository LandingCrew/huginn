#include "DropAheadProbe.h"

#include <atomic>
#include <cmath>
#include <thread>

namespace Huginn::State::DropAheadProbe
{
   namespace
   {
      std::atomic<std::thread::id> g_mainThread{};

      // A ray that hits an actor (the player's own body included, for the
      // waist-height picks), a dropped item or clutter is cast on from just
      // past the hit. Bounded so a crowd cannot cost much; running out makes
      // the probe unknown, never a drop.
      constexpr int kMaxRecasts = 8;

      struct Cast
      {
         enum class Kind { Hit, Clear, Exhausted } kind = Kind::Clear;
         float distance = 0.0f;  // along the ray to the ground hit
      };

      // From `from` along the unit vector `dir` for `length` units. Caller
      // holds the world's read lock.
      Cast CastSegment(RE::bhkWorld* world, Core::Needs::Vec3 from, Core::Needs::Vec3 dir, float length,
                       int& rejected)
      {
         const float scale = RE::bhkWorld::GetWorldScale();
         static const std::uint32_t kFilter = [] {
            // The LOS layer: what line-of-sight picks use; it collides with
            // statics and terrain (and actors and clutter, cast through below).
            // A fresh system group so no body's group filters the ray out.
            auto* filter = RE::bhkCollisionFilter::GetSingleton();
            const std::uint32_t group = filter ? filter->GetNewSystemGroup() : 0;
            return (group << 16) | static_cast<std::uint32_t>(RE::COL_LAYER::kLOS);
         }();

         float travelled = 0.0f;
         for (int attempt = 0; attempt <= kMaxRecasts; ++attempt) {
            const float remaining = length - travelled;
            if (remaining <= 1.0f) return { Cast::Kind::Clear, length };
            const Core::Needs::Vec3 a{ from.x + dir.x * travelled, from.y + dir.y * travelled,
                                       from.z + dir.z * travelled };
            const Core::Needs::Vec3 b{ a.x + dir.x * remaining, a.y + dir.y * remaining, a.z + dir.z * remaining };
            RE::bhkPickData pick{};
            pick.rayInput.from = RE::hkVector4(a.x * scale, a.y * scale, a.z * scale, 0.0f);
            pick.rayInput.to = RE::hkVector4(b.x * scale, b.y * scale, b.z * scale, 0.0f);
            pick.rayInput.enableShapeCollectionFilter = false;
            pick.rayInput.filterInfo = kFilter;
            world->PickObject(pick);
            if (!pick.rayOutput.HasHit()) return { Cast::Kind::Clear, length };
            const float at = travelled + remaining * pick.rayOutput.hitFraction;
            const auto* collidable = pick.rayOutput.rootCollidable;
            if (collidable && IsGroundLayer(collidable->GetCollisionLayer())) {
               return { Cast::Kind::Hit, at };
            }
            ++rejected;
            travelled = at + 1.0f;  // just past the reject
         }
         return { Cast::Kind::Exhausted, 0.0f };
      }
   }

   void NoteMainThread() noexcept { g_mainThread.store(std::this_thread::get_id()); }

   bool OnMainThread() noexcept
   {
      const auto main = g_mainThread.load();
      return main != std::thread::id{} && main == std::this_thread::get_id();
   }

   std::string_view StatusName(Status s) noexcept
   {
      switch (s) {
      case Status::Measured: return "measured";
      case Status::AllUnknown: return "all probes unknown";
      case Status::NotMainThread: return "not the main thread";
      case Status::No3D: return "no player 3D";
      case Status::NoCell: return "no parent cell";
      case Status::NoWorld: return "no physics world";
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

   Result Measure(RE::PlayerCharacter* player, Core::Needs::Vec3 velocity, const Core::Needs::DropProbeConfig& cfg)
   {
      if (!OnMainThread()) {
         Result r;
         r.status = Status::NotMainThread;
         return r;
      }
      return MeasureInTask(player, velocity, cfg);
   }

   Result MeasureInTask(RE::PlayerCharacter* player, Core::Needs::Vec3 velocity, const Core::Needs::DropProbeConfig& cfg)
   {
      Result r;
      if (!player || !player->Get3D()) {
         r.status = Status::No3D;
         return r;
      }
      auto* cell = player->GetParentCell();
      if (!cell) {
         r.status = Status::NoCell;
         return r;
      }
      auto* world = cell->GetbhkWorld();
      if (!world) {
         r.status = Status::NoWorld;
         return r;
      }

      const RE::NiPoint3 pos = player->GetPosition();
      const Core::Needs::Vec3 feet{ pos.x, pos.y, pos.z };
      r.dir = Core::Needs::ProbeDirection(velocity, player->GetAngleZ(), cfg.minMoveSpeed);
      const auto starts = Core::Needs::ProbeStarts(feet, r.dir, cfg);
      const Core::Needs::Vec3 down{ 0.0f, 0.0f, -1.0f };
      const Core::Needs::Vec3 ahead{ r.dir.x, r.dir.y, 0.0f };

      {
         RE::BSReadLockGuard lock(world->worldLock);
         Core::Needs::Vec3 previous = Core::Needs::ProbeOrigin(feet, cfg);
         bool blocked = false;
         for (std::size_t i = 0; i < starts.size(); ++i) {
            auto& h = r.hits[i];
            if (!blocked) {
               const float reach = std::hypot(starts[i].x - previous.x, starts[i].y - previous.y);
               const Cast c = CastSegment(world, previous, ahead, reach, r.rejectedHits);
               blocked = c.kind != Cast::Kind::Clear;
            }
            if (blocked) {
               h.known = false;
               ++r.unknownProbes;
               continue;
            }
            const Cast c = CastSegment(world, starts[i], down, cfg.rayLength, r.rejectedHits);
            if (c.kind == Cast::Kind::Exhausted) {
               h.known = false;
               ++r.unknownProbes;
            } else {
               h.hit = c.kind == Cast::Kind::Hit;
               h.hitZ = starts[i].z - c.distance;
            }
            previous = starts[i];
         }
      }

      // Water at each probe point: the cell's water at that XY (a placed
      // water object or the cell plane). Outside the lock: no physics read.
      for (std::size_t i = 0; i < starts.size(); ++i) {
         float waterZ = 0.0f;
         RE::NiPoint3 at{ starts[i].x, starts[i].y, starts[i].z };
         if (r.hits[i].known && cell->GetWaterHeight(at, waterZ) && std::isfinite(waterZ) && waterZ > -1.0e6f) {
            r.hits[i].waterKnown = true;
            r.hits[i].waterZ = waterZ;
         }
      }

      r.drop = Core::Needs::DropAhead(feet.z, r.hits, cfg);
      r.status = r.drop < 0.0f ? Status::AllUnknown : Status::Measured;
      return r;
   }
}
