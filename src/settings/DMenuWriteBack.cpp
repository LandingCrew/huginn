#include "DMenuWriteBack.h"

#include "Globals.h"

#include <SimpleIni.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

namespace Huginn::Settings
{
    namespace
    {
        /// The only sections dMenu owns. Anything else in its file is ignored
        /// rather than trusted — a stray section must never reach Huginn.ini.
        constexpr std::string_view kOwnedSections[] = { "Widget", "Keybindings", "Debug" };

        bool IsOwnedSection(const char* section)
        {
            if (!section) return false;
            return std::any_of(std::begin(kOwnedSections), std::end(kOwnedSections),
                [section](std::string_view s) { return s == section; });
        }

        /// dMenu writes every value as a float — "iSlot1Key = 2.000000",
        /// "bEnabled = 1.000000". Those parse correctly (GetLongValue stops at
        /// the '.', GetBoolValue reads the leading digit), but copying them into
        /// a documented, hand-edited INI would be vandalism. Normalize back to
        /// the form the key's Hungarian prefix implies.
        std::string Normalize(const std::string& key, const std::string& value)
        {
            if (key.empty() || value.empty()) return value;

            switch (key.front()) {
            case 'i': {
                char* end = nullptr;
                const double d = std::strtod(value.c_str(), &end);
                if (end == value.c_str()) return value;  // unparseable, pass through
                return std::to_string(static_cast<long>(d));
            }
            case 'b': {
                char* end = nullptr;
                const double d = std::strtod(value.c_str(), &end);
                if (end != value.c_str()) return d != 0.0 ? "true" : "false";
                std::string lowered = value;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lowered == "true" || lowered == "false") return lowered;
                return value;
            }
            case 'f': {
                // Trim dMenu's trailing zeros: 28.000000 -> 28, 12.500000 -> 12.5
                if (value.find('.') == std::string::npos) return value;
                std::string trimmed = value;
                trimmed.erase(trimmed.find_last_not_of('0') + 1);
                if (!trimmed.empty() && trimmed.back() == '.') trimmed.pop_back();
                return trimmed.empty() ? value : trimmed;
            }
            default:
                return value;
            }
        }

        /// Flatten the owned sections of an INI into "Section/key" -> value.
        std::map<std::string, std::string> Flatten(const CSimpleIniA& ini)
        {
            std::map<std::string, std::string> out;
            CSimpleIniA::TNamesDepend sections;
            ini.GetAllSections(sections);
            for (const auto& sec : sections) {
                if (!IsOwnedSection(sec.pItem)) continue;
                CSimpleIniA::TNamesDepend keys;
                if (!ini.GetAllKeys(sec.pItem, keys)) continue;
                for (const auto& key : keys) {
                    const char* raw = ini.GetValue(sec.pItem, key.pItem, nullptr);
                    if (!raw) continue;
                    out.emplace(std::string(sec.pItem) + "/" + key.pItem, raw);
                }
            }
            return out;
        }

        bool LoadIni(CSimpleIniA& ini, const std::filesystem::path& path)
        {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec) || ec) return false;
            ini.SetUnicode();
            ini.SetMultiKey(false);
            return ini.LoadFile(path.string().c_str()) == SI_OK;
        }
    }

    DMenuWriteBack& DMenuWriteBack::GetSingleton()
    {
        static DMenuWriteBack singleton;
        return singleton;
    }

    void DMenuWriteBack::RefreshSnapshot()
    {
        const auto dmenuPath = GetDMenuIniPath();
        if (dmenuPath == GetMainIniPath()) {
            // No dMenu file on disk — GetDMenuIniPath falls back to the main INI.
            // Snapshotting that would diff Huginn.ini against itself.
            return;
        }

        CSimpleIniA ini;
        if (!LoadIni(ini, dmenuPath)) {
            logger::debug("[DMenuWriteBack] Snapshot skipped, could not read {}"sv, dmenuPath.string());
            return;
        }
        m_snapshot = Flatten(ini);
        m_haveSnapshot = true;
        logger::debug("[DMenuWriteBack] Snapshot taken: {} key(s)"sv, m_snapshot.size());
    }

    size_t DMenuWriteBack::PropagateChanges()
    {
        const auto dmenuPath = GetDMenuIniPath();
        const auto mainPath = GetMainIniPath();
        if (dmenuPath == mainPath) {
            return 0;  // no dMenu file; nothing to propagate from
        }

        CSimpleIniA dmenuIni;
        if (!LoadIni(dmenuIni, dmenuPath)) {
            logger::warn("[DMenuWriteBack] Could not read dMenu INI {}, skipping write-back"sv,
                dmenuPath.string());
            return 0;
        }
        const auto current = Flatten(dmenuIni);

        if (!m_haveSnapshot) {
            // First event of the session with no baseline: adopt one rather than
            // treating every key as changed, which would copy dMenu's whole file
            // over Huginn.ini.
            m_snapshot = current;
            m_haveSnapshot = true;
            logger::info("[DMenuWriteBack] No baseline yet — adopted current dMenu state, nothing written"sv);
            return 0;
        }

        // Only keys that MOVED since the snapshot. Absent-before counts as moved
        // (dMenu started writing a key it did not write previously).
        std::map<std::string, std::string> changed;
        for (const auto& entry : current) {
            const auto prev = m_snapshot.find(entry.first);
            if (prev == m_snapshot.end() || prev->second != entry.second) {
                changed.emplace(entry.first, entry.second);
            }
        }
        if (changed.empty()) {
            m_snapshot = current;
            return 0;
        }

        CSimpleIniA mainIni;
        if (!LoadIni(mainIni, mainPath)) {
            logger::warn("[DMenuWriteBack] Main INI {} not readable — {} dMenu change(s) will not persist"sv,
                mainPath.string(), changed.size());
            m_snapshot = current;
            return 0;
        }

        size_t written = 0;
        for (const auto& entry : changed) {
            const auto slash = entry.first.find('/');
            if (slash == std::string::npos) continue;
            const std::string section = entry.first.substr(0, slash);
            const std::string key = entry.first.substr(slash + 1);
            const std::string value = Normalize(key, entry.second);

            const char* existing = mainIni.GetValue(section.c_str(), key.c_str(), nullptr);
            if (existing && value == existing) continue;  // already agrees

            if (mainIni.SetValue(section.c_str(), key.c_str(), value.c_str()) < 0) {
                logger::warn("[DMenuWriteBack] Failed to set [{}] {} = {}"sv, section, key, value);
                continue;
            }
            logger::info("[DMenuWriteBack] [{}] {}: {} -> {}"sv,
                section, key, existing ? existing : "(unset)", value);
            ++written;
        }

        if (written > 0) {
            // SimpleIni preserves file/section/key comments across a
            // LoadFile -> SetValue -> SaveFile round trip, so the ~800 lines of
            // documentation in Huginn.ini survive.
            // a_bAddSignature=false: SaveFile prepends a UTF-8 BOM by default,
            // which would silently change byte 0 of a file that never had one.
            // Verified by round-tripping configs/Huginn.ini: with the default
            // the only content difference was that added BOM.
            if (mainIni.SaveFile(mainPath.string().c_str(), /*a_bAddSignature=*/false) != SI_OK) {
                logger::error("[DMenuWriteBack] Failed to save {} — {} change(s) lost"sv,
                    mainPath.string(), written);
                written = 0;
            } else {
                logger::info("[DMenuWriteBack] Wrote {} dMenu change(s) into {}"sv,
                    written, mainPath.string());
            }
        }

        m_snapshot = current;
        return written;
    }
}
