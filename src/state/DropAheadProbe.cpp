#include "DropAheadProbe.h"

#include <atomic>
#include <thread>

namespace Huginn::State::DropAheadProbe
{
   namespace
   {
      std::atomic<std::thread::id> g_mainThread{};

      // A ray that hits an actor, a dropped item or a trigger is cast on from
      // just below the hit; a few rejects at most in practice (a follower and
      // the clutter on a table), so this bounds the cost, not the reach.
      constexpr int kMaxRecasts = 4;

      struct Cast
      {
         bool hit = false;
         float hitZ = 0.0f;
      };

      // Straight down from `from` for `length` units. Caller holds the world's
      // read lock.
      Cast CastDown(RE::bhkWorld* world, Core::Needs::Vec3 from, float length, int& rejected)
      {
         const float scale = RE::bhkWorld::GetWorldScale();
         static const std::uint32_t kFilter = [] {
            // The LOS layer: what line-of-sight picks use; it collides with
            // statics and terrain. A fresh system group so the ray belongs to
            // no body's group (nothing is skipped because of the group).
            auto* filter = RE::bhkCollisionFilter::GetSingleton();
            const std::uint32_t group = filter ? filter->GetNewSystemGroup() : 0;
            return (group << 16) | static_cast<std::uint32_t>(RE::COL_LAYER::kLOS);
         }();

         float top = from.z;
         float remaining = length;
         for (int attempt = 0; attempt <= kMaxRecasts && remaining > 1.0f; ++attempt) {
            RE::bhkPickData pick{};
            pick.rayInput.from = RE::hkVector4(from.x * scale, from.y * scale, top * scale, 0.0f);
            pick.rayInput.to = RE::hkVector4(from.x * scale, from.y * scale, (top - remaining) * scale, 0.0f);
            pick.rayInput.enableShapeCollectionFilter = false;
            pick.rayInput.filterInfo = kFilter;
            world->PickObject(pick);
            if (!pick.rayOutput.HasHit()) return {};
            const float hitZ = top - remaining * pick.rayOutput.hitFraction;
            const auto* collidable = pick.rayOutput.rootCollidable;
            if (collidable && IsGroundLayer(collidable->GetCollisionLayer())) {
               return { true, hitZ };
            }
            ++rejected;
            const float next = hitZ - 1.0f;  // just below the reject
            remaining -= top - next;
            top = next;
         }
         return {};
      }
   }

   void NoteMainThread() noexcept { g_mainThread.store(std::this_thread::get_id()); }

   bool OnMainThread() noexcept
   {
      const auto main = g_mainThread.load();
      return main != std::thread::id{} && main == std::this_thread::get_id();
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

   std::optional<Result> Measure(RE::PlayerCharacter* player, Core::Needs::Vec3 velocity,
      const Core::Needs::DropProbeConfig& cfg)
   {
      if (!player || !OnMainThread() || !player->Get3D()) return std::nullopt;
      auto* cell = player->GetParentCell();
      if (!cell) return std::nullopt;
      auto* world = cell->GetbhkWorld();
      if (!world) return std::nullopt;

      const RE::NiPoint3 pos = player->GetPosition();
      const Core::Needs::Vec3 feet{ pos.x, pos.y, pos.z };
      Result r;
      r.dir = Core::Needs::ProbeDirection(velocity, player->GetAngleZ(), cfg.minMoveSpeed);
      const auto starts = Core::Needs::ProbeStarts(feet, r.dir, cfg);

      {
         RE::BSReadLockGuard lock(world->worldLock);
         for (std::size_t i = 0; i < starts.size(); ++i) {
            const Cast c = CastDown(world, starts[i], cfg.rayLength, r.rejectedHits);
            r.hits[i].hit = c.hit;
            r.hits[i].hitZ = c.hitZ;
         }
      }

      // Water at each probe point: the cell's water at that XY (a placed
      // water object or the cell plane). Outside the lock: no physics read.
      for (std::size_t i = 0; i < starts.size(); ++i) {
         float waterZ = 0.0f;
         RE::NiPoint3 at{ starts[i].x, starts[i].y, starts[i].z };
         if (cell->GetWaterHeight(at, waterZ) && std::isfinite(waterZ) && waterZ > -1.0e6f) {
            r.hits[i].waterKnown = true;
            r.hits[i].waterZ = waterZ;
         }
      }

      r.drop = Core::Needs::DropAhead(feet.z, r.hits, cfg);
      return r;
   }
}
