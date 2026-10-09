#pragma once

namespace Huginn::UI
{
    class D3D11Hook
    {
    public:
        /// Call once from SKSEPlugin_Load (InstallHooks, which allocates the
        /// trampoline). PresentHook is a pass-through until ImGui is initialized.
        static bool Install();

        // Original function pointer - needs to be public for the hook function
        inline static REL::Relocation<void(std::uint32_t)> _originalPresent;
    };
}
