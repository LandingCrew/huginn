#pragma once

namespace Huginn::Util
{
    /// True once enough time has elapsed since the last game load for inventory
    /// extraLists to be stable. The engine is still finalizing extra data for
    /// ~Config::EXTRALIST_STABILIZATION_MS after a load; walking entry->extraLists
    /// earlier can crash. ALL extraList-reading scans gate on this: WeaponRegistry
    /// (RefreshCharges both overloads, ReconcileWeapons) and ItemRegistry's
    /// soul-gem scan. Callers skip the extraList-derived portion of their scan
    /// when not yet stable and pick it up on their next periodic pass.
    [[nodiscard]] bool IsExtraListStable() noexcept;
}
