#include "EquipCallerTrace.h"
#include "EquipSourceTracker.h"
#include "core/SignatureDedup.h"
#include "util/StackTrace.h"

#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <string>

#include <Windows.h>

namespace Huginn::Learning::EquipCallerTrace
{
   namespace
   {
      // The HandModelProbe's Huginn-equip window (kHuginnEquipWindowMs there):
      // the mark lands as the equip is asked for, the event when the engine
      // applies it.
      constexpr float kHuginnWindowMs = 1500.0f;
      constexpr DWORD kMaxFrames = 16;
      constexpr std::int64_t kMinIntervalMs = 500;

      // A hand item: a spell or scroll (not a power, an ability or a
      // shout), a weapon, a light, a shield.
      bool IsHandItem(const RE::TESForm& form)
      {
         switch (form.GetFormType()) {
         case RE::FormType::Spell:
         case RE::FormType::Scroll: {
            const auto* spell = form.As<RE::SpellItem>();
            if (!spell) return false;
            const auto type = spell->GetSpellType();
            return type == RE::MagicSystem::SpellType::kSpell || type == RE::MagicSystem::SpellType::kScroll;
         }
         case RE::FormType::Weapon:
         case RE::FormType::Light:
            return true;
         case RE::FormType::Armor: {
            const auto* armor = form.As<RE::TESObjectARMO>();
            return armor && armor->IsShield();
         }
         default:
            return false;
         }
      }

      Core::SignatureDedup& Dedup()
      {
         static Core::SignatureDedup dedup;
         return dedup;
      }

      std::atomic<std::int64_t> g_lastLogMs{ INT64_MIN / 2 };
      std::atomic<std::uint32_t> g_skipped{ 0 };

      std::int64_t NowMs()
      {
         return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
      }
   }

   void NoteIfExternal(const RE::TESEquipEvent& event, const RE::TESForm& form)
   {
      // Unequips only: an equip not by Huginn is already attributed (the
      // TESEquipEvent sinks' external-equip learning, the probe's swap line).
      if (event.equipped) return;
      if (!spdlog::default_logger()->should_log(spdlog::level::debug)) return;
      if (!IsHandItem(form)) return;

      const auto& tracker = EquipSourceTracker::GetSingleton();
      const RE::FormID formID = form.GetFormID();
      if (tracker.IsRecentHuginnEquip(formID, kHuginnWindowMs)) return;
      if (tracker.HadRecentHuginnHandEquip(kHuginnWindowMs)) return;

      // The rate limit before anything is captured; a line it holds back is
      // counted, and not recorded in the dedup (the same stack can still be
      // logged later).
      const auto now = NowMs();
      if (now - g_lastLogMs.load(std::memory_order_relaxed) < kMinIntervalMs) {
         g_skipped.fetch_add(1, std::memory_order_relaxed);
         return;
      }

      // The raw return addresses (a frame walk, microseconds) are the
      // dedup's signature: the same call path gives the same addresses in a
      // session. Module names are resolved only for a stack that will be
      // logged. Skip this function's own frame; the sink that called it is
      // the first one shown.
      std::array<void*, kMaxFrames> frames{};
      const USHORT count = RtlCaptureStackBackTrace(1, kMaxFrames, frames.data(), nullptr);
      std::uint64_t hash = 14695981039346656037ull;
      for (USHORT i = 0; i < count; ++i) {
         hash = (hash ^ reinterpret_cast<std::uintptr_t>(frames[i])) * 1099511628211ull;
      }
      if (!Dedup().ShouldWarn(formID, hash)) return;
      g_lastLogMs.store(now, std::memory_order_relaxed);
      const std::uint32_t skipped = g_skipped.exchange(0, std::memory_order_relaxed);

      std::string stack;
      for (USHORT i = 0; i < count; ++i) {
         if (!stack.empty()) stack += " < ";
         stack += Util::StackTrace::ModuleOffset(frames[i]);
      }

      const auto* main = RE::Main::GetSingleton();
      const DWORD thread = GetCurrentThreadId();
      const char* name = form.GetName();
      logger::debug("[EquipCaller] unequipped '{}' ({:08X}) not by Huginn | thread {}{} | stack: {}{}"sv,
         name && *name ? name : "?", formID, thread,
         main && main->threadID == thread ? " (the game's main thread)" : "",
         stack.empty() ? std::string("(none captured)") : stack,
         skipped > 0 ? std::format(" | {} line(s) held back by the rate limit since the last", skipped) : std::string());
   }
}
