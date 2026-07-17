#include "util/ExtraListStability.h"
#include "Config.h"
#include "Globals.h"

namespace Huginn::Util
{
    bool IsExtraListStable() noexcept
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_lastGameLoad);
        return elapsed.count() >= static_cast<int64_t>(Config::EXTRALIST_STABILIZATION_MS);
    }
}
