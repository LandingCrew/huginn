#include "EquipCallerTrace.h"
#include "EquipSourceTracker.h"
#include "core/SignatureDedup.h"

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

      // "Module.dll+0x1A2B3C" for a code address; "?+0x..." outside any
      // module (generated code). UNCHANGED_REFCOUNT: no module is pinned.
      std::string Frame(void* address, std::uint64_t& hash)
      {
         HMODULE module = nullptr;
         const auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
         if (!GetModuleHandleExW(flags, static_cast<LPCWSTR>(address), &module) || !module) {
            const auto raw = reinterpret_cast<std::uintptr_t>(address);
            hash = (hash ^ raw) * 1099511628211ull;
            return std::format("?+0x{:X}", raw);
         }
         std::array<wchar_t, MAX_PATH> path{};
         const DWORD len = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
         std::string name;
         for (DWORD i = len; i > 0; --i) {
            if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
               for (DWORD j = i; j < len; ++j) name += static_cast<char>(path[j] < 0x80 ? path[j] : L'?');
               break;
            }
         }
         if (name.empty()) {
            for (DWORD j = 0; j < len; ++j) name += static_cast<char>(path[j] < 0x80 ? path[j] : L'?');
         }
         const auto offset = reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module);
         // The signature: the module by its base and the offset in it, so the
         // same call path hashes the same in one session.
         hash = (hash ^ reinterpret_cast<std::uintptr_t>(module)) * 1099511628211ull;
         hash = (hash ^ offset) * 1099511628211ull;
         return std::format("{}+0x{:X}", name.empty() ? std::string("?") : name, offset);
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
      if (!spdlog::default_logger()->should_log(spdlog::level::debug)) return;
      if (!IsHandItem(form)) return;

      const auto& tracker = EquipSourceTracker::GetSingleton();
      const RE::FormID formID = form.GetFormID();
      if (tracker.IsRecentHuginnEquip(formID, kHuginnWindowMs)) return;
      if (!event.equipped && tracker.HadRecentHuginnEquip(kHuginnWindowMs)) return;

      // Skip this function's own frame; the sink that called it is the
      // first one shown.
      std::array<void*, kMaxFrames> frames{};
      const USHORT count = RtlCaptureStackBackTrace(1, kMaxFrames, frames.data(), nullptr);
      std::uint64_t hash = 14695981039346656037ull;
      std::string stack;
      for (USHORT i = 0; i < count; ++i) {
         if (!stack.empty()) stack += " < ";
         stack += Frame(frames[i], hash);
      }

      // The rate limit first, and a line it holds back is not recorded in the
      // dedup: the same stack can still be logged later.
      const auto now = NowMs();
      if (now - g_lastLogMs.load(std::memory_order_relaxed) < kMinIntervalMs) {
         g_skipped.fetch_add(1, std::memory_order_relaxed);
         return;
      }
      const std::uint32_t key = formID ^ (event.equipped ? 0u : 0x80000000u);
      if (!Dedup().ShouldWarn(key, hash)) return;
      g_lastLogMs.store(now, std::memory_order_relaxed);
      const std::uint32_t skipped = g_skipped.exchange(0, std::memory_order_relaxed);

      const auto* main = RE::Main::GetSingleton();
      const DWORD thread = GetCurrentThreadId();
      const char* name = form.GetName();
      logger::debug("[EquipCaller] {} '{}' ({:08X}) not by Huginn | thread {}{} | stack: {}{}"sv,
         event.equipped ? "equipped" : "unequipped", name && *name ? name : "?", formID, thread,
         main && main->threadID == thread ? " (the game's main thread)" : "",
         stack.empty() ? std::string("(none captured)") : stack,
         skipped > 0 ? std::format(" | {} line(s) held back by the rate limit since the last", skipped) : std::string());
   }
}
