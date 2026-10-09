#pragma once

// Thin wrapper around Tracy profiler macros.
// When Huginn_TRACY_ENABLED is defined (via CMake -DHuginn_TRACY=ON),
// these expand to real Tracy instrumentation. Otherwise they are no-ops.

#ifdef Huginn_TRACY_ENABLED
#   include <tracy/Tracy.hpp>

#   define Huginn_ZONE          ZoneScoped
#   define Huginn_ZONE_NAMED(n) ZoneScopedN(n)
    // A second named zone in a scope that already has one (Huginn_ZONE_NAMED
    // declares a fixed variable name, so two in one scope do not compile):
    // `var` names this zone's variable. It ends with the enclosing scope.
#   define Huginn_ZONE_NAMED_VAR(var, n) ZoneNamedN(var, n, true)
#   define Huginn_FRAME_MARK    FrameMark
#   define Huginn_SET_THREAD(n) tracy::SetThreadName(n)

    // Time-series plot for long-play drift/leak watching (candidate counts,
    // learned-item growth, accept-rate). `name` MUST be a persistent string
    // literal — Tracy keys plots by pointer identity.
#   define Huginn_PLOT(name, val) TracyPlot(name, val)

    // Targeted memory tracking — use around specific allocations to detect leaks.
    // Usage: auto* p = new Foo(); Huginn_ALLOC(p, sizeof(Foo));
    //        Huginn_FREE(p); delete p;
#   define Huginn_ALLOC(ptr, size) TracyAlloc(ptr, size)
#   define Huginn_FREE(ptr)        TracyFree(ptr)
#else
#   define Huginn_ZONE          ((void)0)
#   define Huginn_ZONE_NAMED(n) ((void)0)
#   define Huginn_ZONE_NAMED_VAR(var, n) ((void)0)
#   define Huginn_FRAME_MARK    ((void)0)
#   define Huginn_SET_THREAD(n) ((void)0)
#   define Huginn_PLOT(name, val) ((void)0)
#   define Huginn_ALLOC(ptr, size) ((void)0)
#   define Huginn_FREE(ptr)        ((void)0)
#endif
