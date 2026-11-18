#ifdef _DEBUG

#include "AbortTrace.h"
#include "StackTrace.h"

#include <array>
#include <atomic>
#include <csignal>
#include <format>
#include <string>

#include <Windows.h>
#include <DbgHelp.h>

#pragma comment(lib, "dbghelp.lib")

namespace Huginn::Util::AbortTrace
{
   namespace
   {
      constexpr DWORD kMaxFrames = 48;

      std::atomic<bool> g_tracing{ false };

      // The handler Install() replaced: the debug CRT keeps one SIGABRT
      // handler for the whole process, so another plugin on it may have
      // installed one first. OnAbort hands over to it when done.
      using Handler = void (*)(int);
      std::atomic<Handler> g_previous{ nullptr };

      void Flush()
      {
         if (auto logger = spdlog::default_logger()) {
            logger->flush();
         }
      }

      HMODULE ThisModule()
      {
         HMODULE module = nullptr;
         const auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
         GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&ThisModule), &module);
         return module;
      }

      // DbgHelp over Huginn.dll alone: a session of our own (a duplicated
      // process handle, so another plugin's SymInitialize on the
      // pseudo-handle is not reused), only our module's symbols loaded.
      // SymSetOptions is process-wide, so the options in force before are
      // put back when the session ends. DbgHelp is not thread-safe and
      // nothing here serializes with another plugin's use of it; this runs
      // once, on an abort that is about to end the process.
      class Symbols
      {
      public:
         Symbols()
         {
            const HANDLE self = GetCurrentProcess();
            if (!DuplicateHandle(self, self, self, &m_process, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
               m_process = nullptr;
               return;
            }
            m_module = ThisModule();
            std::array<wchar_t, MAX_PATH> path{};
            if (!m_module || !GetModuleFileNameW(m_module, path.data(), static_cast<DWORD>(path.size()))) {
               CloseHandle(m_process);
               m_process = nullptr;
               return;
            }
            // The PDB beside the DLL first (the deployed pair), then the
            // build path the DLL records.
            std::wstring dir = path.data();
            if (const auto slash = dir.find_last_of(L"\\/"); slash != std::wstring::npos) {
               dir.resize(slash);
            }
            m_oldOptions = SymGetOptions();
            m_optionsSet = true;
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
            if (!SymInitializeW(m_process, dir.c_str(), FALSE)) {
               CloseHandle(m_process);
               m_process = nullptr;
               return;
            }
            m_base = reinterpret_cast<DWORD64>(m_module);
            const DWORD64 loaded = SymLoadModuleExW(m_process, nullptr, path.data(), nullptr, m_base, 0, nullptr, 0);
            m_loaded = loaded != 0 || GetLastError() == ERROR_SUCCESS;
         }

         ~Symbols()
         {
            if (m_process) {
               SymCleanup(m_process);
               CloseHandle(m_process);
            }
            if (m_optionsSet) {
               SymSetOptions(m_oldOptions);
            }
         }

         Symbols(const Symbols&) = delete;
         Symbols& operator=(const Symbols&) = delete;

         bool Ready() const { return m_process && m_loaded; }

         bool Owns(void* address) const
         {
            HMODULE module = nullptr;
            const auto flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
            return GetModuleHandleExW(flags, static_cast<LPCWSTR>(address), &module) && module == m_module;
         }

         // " Function+0x12 (file.cpp:123)"; empty if DbgHelp has nothing.
         std::string Describe(void* address) const
         {
            const auto addr = reinterpret_cast<DWORD64>(address);
            alignas(SYMBOL_INFO) std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> buffer{};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer.data());
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 displacement = 0;
            if (!SymFromAddr(m_process, addr, &displacement, symbol)) {
               return {};
            }
            std::string out = std::format(" {}+0x{:X}", symbol->Name, displacement);
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddr64(m_process, addr, &lineDisplacement, &line) && line.FileName) {
               std::string_view file = line.FileName;
               if (const auto slash = file.find_last_of("\\/"); slash != std::string_view::npos) {
                  file.remove_prefix(slash + 1);
               }
               out += std::format(" ({}:{})", file, line.LineNumber);
            }
            return out;
         }

      private:
         HANDLE m_process = nullptr;
         HMODULE m_module = nullptr;
         DWORD64 m_base = 0;
         DWORD m_oldOptions = 0;
         bool m_optionsSet = false;
         bool m_loaded = false;
      };

      void Trace();

      // Logs, then hands over to the handler Install() replaced, if any.
      //
      // Known risk, accepted for a Debug-only diagnostic: the handler logs
      // through spdlog and allocates. An abort raised while this thread
      // holds the logger's sink mutex (an assert inside a formatter or sink)
      // would deadlock here instead of ending the process.
      void OnAbort(int a_signal)
      {
         if (!g_tracing.exchange(true)) {
            Trace();
         }
         const Handler previous = g_previous.load();
         if (previous && previous != &OnAbort) {
            previous(a_signal);
         }
      }

      void Trace()
      {

         std::array<void*, kMaxFrames> frames{};
         const USHORT count = RtlCaptureStackBackTrace(0, kMaxFrames, frames.data(), nullptr);

         const auto* main = RE::Main::GetSingleton();
         const DWORD thread = GetCurrentThreadId();
         std::string stack;
         for (USHORT i = 0; i < count; ++i) {
            if (!stack.empty()) stack += " < ";
            stack += StackTrace::ModuleOffset(frames[i]);
         }
         logger::critical("[AbortTrace] abort (a failed assert's Abort, or abort()) on thread {}{} | stack: {}"sv,
            thread, main && main->threadID == thread ? " (the game's main thread)" : "",
            stack.empty() ? std::string("(none captured)") : stack);
         Flush();

         const Symbols symbols;
         if (!symbols.Ready()) {
            logger::critical("[AbortTrace] Huginn's symbols did not load (error {}); the line above is the trace"sv,
               GetLastError());
            Flush();
            return;
         }
         for (USHORT i = 0; i < count; ++i) {
            const std::string where = symbols.Owns(frames[i]) ? symbols.Describe(frames[i]) : std::string();
            logger::critical("[AbortTrace]   #{:<2} {}{}"sv, i, StackTrace::ModuleOffset(frames[i]), where);
         }
         Flush();
      }
   }

   void Install()
   {
      const Handler previous = std::signal(SIGABRT, &OnAbort);
      if (previous == SIG_ERR) {
         logger::error("[AbortTrace] SIGABRT handler not installed: a failed assert's stack will not be logged"sv);
         return;
      }
      const bool chained = previous != SIG_DFL && previous != SIG_IGN && previous != &OnAbort;
      g_previous.store(chained ? previous : nullptr);
      logger::info("[AbortTrace] SIGABRT handler installed (a failed assert's Abort logs its stack){}"sv,
         chained ? "; another module's handler runs after it" : "");
   }
}

#endif  // _DEBUG
