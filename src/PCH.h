#pragma once

// Prevent Windows.h from defining min/max macros that conflict with std::min/std::max
// This allows using std::max directly instead of (std::max) workaround
#ifndef NOMINMAX
#define NOMINMAX
#endif

#define IMGUI_DEFINE_MATH_OPERATORS

#pragma warning(push)
#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#pragma warning(disable: 4702)
#include <SimpleIni.h>

#ifdef NDEBUG
#   include <spdlog/sinks/basic_file_sink.h>
#else
#   include <spdlog/sinks/msvc_sink.h>
#endif
#pragma warning(pop)

#include <algorithm>
#include <unordered_set>

using namespace std::literals;

namespace logger = SKSE::log;

namespace util
{
   using SKSE::stl::report_and_fail;
}

namespace std
{
   template <class T>
   struct hash<RE::BSPointerHandle<T>>
   {
      uint32_t operator()(const RE::BSPointerHandle<T>& a_handle) const
      {
      uint32_t nativeHandle = const_cast<RE::BSPointerHandle<T>*>(&a_handle)->native_handle();
      return nativeHandle;
      }
   };
}

#define DLLEXPORT __declspec(dllexport)

#define RELOCATION_OFFSET(SE, AE) REL::VariantOffset(SE, AE, 0).offset()

// Logging macros - CommonLibSSE NG 3.7.0 removed these
#define INFO(...)     logger::info(__VA_ARGS__)
#define ERROR(...)    logger::error(__VA_ARGS__)
#define WARN(...)     logger::warn(__VA_ARGS__)
#define DEBUG(...)    logger::debug(__VA_ARGS__)
#define TRACE(...)    logger::trace(__VA_ARGS__)
#define CRITICAL(...) logger::critical(__VA_ARGS__)
#define ASSERT(condition) \
    do { \
        if (!(condition)) { \
            logger::critical("Assertion failed: " #condition); \
        } \
    } while (0)

#include "Plugin.h"
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#define IM_PI 3.14159265358979323846f

#ifdef SKYRIM_AE
#    define VAR_NUM(se, ae) ae
#else
#    define VAR_NUM(se, ae) se
#endif
