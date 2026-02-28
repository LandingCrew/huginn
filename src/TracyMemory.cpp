// =============================================================================
// TracyMemory.cpp - Global new/delete overrides for Tracy memory profiling
// =============================================================================
// When Huginn_TRACY_ENABLED is defined, this overrides operator new/delete
// to report all heap allocations to Tracy's Memory panel.
// Only one translation unit should define these — this is that file.
//
// MSVC gives each DLL its own CRT heap by default, so these overrides
// only affect allocations originating from Huginn.dll code.
// =============================================================================

#ifdef Huginn_TRACY_ENABLED

#include <tracy/Tracy.hpp>
#include <cstdlib>

void* operator new(std::size_t size)
{
    auto* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    TracyAlloc(ptr, size);
    return ptr;
}

void operator delete(void* ptr) noexcept
{
    TracyFree(ptr);
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    TracyFree(ptr);
    std::free(ptr);
}

void* operator new[](std::size_t size)
{
    auto* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    TracyAlloc(ptr, size);
    return ptr;
}

void operator delete[](void* ptr) noexcept
{
    TracyFree(ptr);
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    TracyFree(ptr);
    std::free(ptr);
}

#endif // Huginn_TRACY_ENABLED
