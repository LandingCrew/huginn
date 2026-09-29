#include "TelemetrySettings.h"
#include "IniLoad.h"

#include <algorithm>
#include <cctype>

namespace Huginn::Telemetry
{
    namespace
    {
        [[nodiscard]] std::uint32_t ReadClampedLong(const CSimpleIniA& ini, const char* section,
            const char* key, long defaultVal, long lo, long hi)
        {
            const long raw = ini.GetLongValue(section, key, defaultVal);
            const long clamped = std::clamp(raw, lo, hi);
            if (clamped != raw) {
                logger::warn("[TelemetrySettings] {} = {} out of range [{}, {}], clamped to {}"sv,
                    key, raw, lo, hi, clamped);
            }
            return static_cast<std::uint32_t>(clamped);
        }
    }

    void TelemetrySettings::LoadFromFile(const std::filesystem::path& iniPath)
    {
        CSimpleIniA ini;
        if (LoadIniFile(ini, iniPath, "TelemetrySettings"sv)) {
            LoadFromIni(ini);
        }
    }

    void TelemetrySettings::LoadFromIni(const CSimpleIniA& ini)
    {
        const char* section = "Telemetry";

        m_config.enabled = ini.GetBoolValue(section, "bEnabled", TelemetryDefaults::ENABLED);
        m_config.maxFileSizeMB = ReadClampedLong(ini, section, "iMaxFileSizeMB",
            TelemetryDefaults::MAX_FILE_SIZE_MB, 1, 512);
        m_config.maxRotatedFiles = ReadClampedLong(ini, section, "iMaxRotatedFiles",
            TelemetryDefaults::MAX_ROTATED_FILES, 0, 10);
        m_config.topCandidates = ReadClampedLong(ini, section, "iTopCandidates",
            TelemetryDefaults::TOP_CANDIDATES, 0, 50);
        m_config.fileName = SanitizeFileName(
            ini.GetValue(section, "sFileName", TelemetryDefaults::FILE_NAME));

        logger::info("[TelemetrySettings] Loaded: enabled={}, file={}, maxMB={}, rotated={}, topCandidates={}"sv,
            m_config.enabled, m_config.fileName, m_config.maxFileSizeMB,
            m_config.maxRotatedFiles, m_config.topCandidates);
    }

    void TelemetrySettings::ResetToDefaults()
    {
        m_config = TelemetryConfig{};
        logger::info("[TelemetrySettings] Reset to defaults (disabled)"sv);
    }

    TelemetryConfig TelemetrySettings::BuildConfig() const
    {
        return m_config;
    }

    std::string TelemetrySettings::SanitizeFileName(const std::string& raw)
    {
        std::string name;
        try {
            // filename() drops every directory component, so "..\\..\\x.jsonl"
            // and "C:\\Users\\me\\x.jsonl" both become "x.jsonl": the file can
            // only ever land in the SKSE log folder.
            name = std::filesystem::path(raw).filename().string();
        } catch (const std::exception&) {
            name.clear();  // non-representable characters: fall back below
        }

        // '%' would be read as a format directive by the game console's printf-
        // style Print (hg telemetry echoes the name), and control characters
        // have no place in a file name. Replace both rather than reject.
        for (char& ch : name) {
            const auto u = static_cast<unsigned char>(ch);
            if (ch == '%' || u < 0x20) {
                ch = '_';
            }
        }

        if (name.empty() || name == "." || name == "..") {
            // Never echo `raw`: with no file-name component (a value ending in
            // a separator, e.g. C:\Users\<name>\Documents\) it is a full
            // path, Windows user folder included, and players paste this log
            // into bug reports. Only the (empty, "." or "..") component and the
            // raw length go out.
            logger::warn("[TelemetrySettings] sFileName has no usable file-name component "
                "('{}', {} chars), using {}"sv,
                name, raw.size(), TelemetryDefaults::FILE_NAME);
            return TelemetryDefaults::FILE_NAME;
        }

        constexpr std::string_view kExt = ".jsonl";
        const bool hasExt = name.size() > kExt.size() &&
            std::equal(kExt.rbegin(), kExt.rend(), name.rbegin(), [](char a, char b) {
                return a == static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
            });
        if (!hasExt) {
            name += kExt;
        }
        return name;
    }

}  // namespace Huginn::Telemetry
