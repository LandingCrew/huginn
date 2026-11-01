#include "StackTrace.h"

#include <array>
#include <format>

#include <Windows.h>

namespace Huginn::Util::StackTrace
{
   std::string ModuleOffset(void* address)
   {
      HMODULE module = nullptr;
      const auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
      if (!GetModuleHandleExW(flags, static_cast<LPCWSTR>(address), &module) || !module) {
         return std::format("?+0x{:X}", reinterpret_cast<std::uintptr_t>(address));
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
      return std::format("{}+0x{:X}", name.empty() ? std::string("?") : name, offset);
   }
}
