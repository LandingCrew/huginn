#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace Huginn::Settings
{
    /// Propagates settings the player changed in dMenu's in-game UI back into
    /// the main Huginn.ini.
    ///
    /// Huginn.ini is authoritative: it is loaded last and overlays dMenu's
    /// generated copy (see SettingsReloader::ReloadAllSettingsExclusive). That
    /// alone would make the dMenu UI useless — a slider would snap back on the
    /// very reload dMenu fires to announce the change. This closes the loop by
    /// writing the change into Huginn.ini first, so the reload that follows
    /// reads it back as the winning value.
    ///
    /// Only keys the player actually TOUCHED are written. dMenu regenerates its
    /// whole file from its JSON schema on every flush, so it also rewrites keys
    /// nobody touched, carrying whatever value dMenu last knew. Blindly copying
    /// all of them would silently revert hand edits to Huginn.ini — set
    /// iToggleWidgetKey by hand, then nudge an unrelated slider in-game, and the
    /// hand edit would be gone. So the propagation diffs the dMenu file against
    /// a snapshot of its previous contents and writes only what moved.
    class DMenuWriteBack
    {
    public:
        static DMenuWriteBack& GetSingleton();

        /// Read the dMenu INI and record it as the diff baseline. Call once the
        /// dMenu file is known to be current (startup, and after each write-back).
        void RefreshSnapshot();

        /// Diff the dMenu INI against the snapshot and write changed keys into
        /// the main INI, preserving its comments and layout. Refreshes the
        /// snapshot on the way out.
        /// @return number of keys written (0 = nothing changed / nothing to do)
        size_t PropagateChanges();

    private:
        DMenuWriteBack() = default;

        /// "Section/key" -> raw value string, as last seen in the dMenu INI.
        std::map<std::string, std::string> m_snapshot;
        bool m_haveSnapshot = false;
    };
}
