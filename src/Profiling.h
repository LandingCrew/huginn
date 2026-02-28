#pragma once

// Thin wrapper around Tracy profiler macros.
// When Huginn_TRACY_ENABLED is defined (via CMake -DHuginn_TRACY=ON),
// these expand to real Tracy instrumentation. Otherwise they are no-ops.

#ifdef Huginn_TRACY_ENABLED
#   include <tracy/Tracy.hpp>

#   define Huginn_ZONE          ZoneScoped
#   define Huginn_ZONE_NAMED(n) ZoneScopedN(n)
#   define Huginn_FRAME_MARK    FrameMark
#   define Huginn_SET_THREAD(n) tracy::SetThreadName(n)

    // Targeted memory tracking — use around specific allocations to detect leaks.
    // Usage: auto* p = new Foo(); Huginn_ALLOC(p, sizeof(Foo));
    //        Huginn_FREE(p); delete p;
#   define Huginn_ALLOC(ptr, size) TracyAlloc(ptr, size)
#   define Huginn_FREE(ptr)        TracyFree(ptr)
#else
#   define Huginn_ZONE          ((void)0)
#   define Huginn_ZONE_NAMED(n) ((void)0)
#   define Huginn_FRAME_MARK    ((void)0)
#   define Huginn_SET_THREAD(n) ((void)0)
#   define Huginn_ALLOC(ptr, size) ((void)0)
#   define Huginn_FREE(ptr)        ((void)0)
#endif
