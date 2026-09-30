#pragma once

#include <SimpleIni.h>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Huginn::Telemetry
{
    // =========================================================================
    // DEFAULT VALUES (compile-time constants)
    // =========================================================================
    namespace TelemetryDefaults
    {
        inline constexpr bool   ENABLED = false;               // opt-in, always
        inline constexpr long   MAX_FILE_SIZE_MB = 16;         // clamp [1, 512]
        inline constexpr long   MAX_ROTATED_FILES = 3;         // clamp [0, 10]
        inline constexpr long   TOP_CANDIDATES = 20;           // clamp [0, 50]
        inline constexpr const char* FILE_NAME = "Huginn_Telemetry.jsonl";
    }

    /// Snapshot handed to DecisionLog::ApplyConfig.
    struct TelemetryConfig
    {
        bool enabled = TelemetryDefaults::ENABLED;
        std::uint32_t maxFileSizeMB = static_cast<std::uint32_t>(TelemetryDefaults::MAX_FILE_SIZE_MB);
        std::uint32_t maxRotatedFiles = static_cast<std::uint32_t>(TelemetryDefaults::MAX_ROTATED_FILES);
        std::uint32_t topCandidates = static_cast<std::uint32_t>(TelemetryDefaults::TOP_CANDIDATES);
        std::string fileName = TelemetryDefaults::FILE_NAME;  // bare filename, no directory

        bool operator==(const TelemetryConfig&) const = default;
    };

    // =========================================================================
    // TELEMETRY SETTINGS
    // =========================================================================
    // Loads the [Telemetry] section of Huginn.ini: the opt-in offline decision
    // log (DecisionLog). Same shape as ScorerSettings -- LoadFromIni for the
    // parse-once path, ResetToDefaults, BuildConfig -- and reloaded by
    // SettingsReloader (`hg reload` / dMenu). See docs/architecture/9-telemetry.md.
    //
    // sFileName is sanitised to a bare filename: the file is ALWAYS written in
    // the SKSE log folder, never at a path the INI names.
    // =========================================================================
    class TelemetrySettings
    {
    public:
        static TelemetrySettings& GetSingleton()
        {
            static TelemetrySettings instance;
            return instance;
        }

        /// Parse `iniPath` from disk, then load [Telemetry].
        void LoadFromFile(const std::filesystem::path& iniPath);
        /// Load [Telemetry] from an already-parsed INI (parse-once path).
        void LoadFromIni(const CSimpleIniA& ini);
        void ResetToDefaults();

        [[nodiscard]] TelemetryConfig BuildConfig() const;

        /// Reduce a user-supplied name to a safe bare filename ending in .jsonl.
        /// Returns the default name (and warns) for empty / "." / ".." / invalid.
        /// Public and static so the unit tests can pin it.
        [[nodiscard]] static std::string SanitizeFileName(const std::string& raw);

    private:
        TelemetrySettings() = default;
        ~TelemetrySettings() = default;
        TelemetrySettings(const TelemetrySettings&) = delete;
        TelemetrySettings& operator=(const TelemetrySettings&) = delete;

        TelemetryConfig m_config{};
    };

}  // namespace Huginn::Telemetry
