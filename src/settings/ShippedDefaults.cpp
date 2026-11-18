#include "ShippedDefaults.h"

#include "IniOverlay.h"
#include "Globals.h"                 // GetMainIniPath, GetDMenuOwnIniPath
#include "core/DMenuDefaults.h"
#include "huginn/ShippedFiles.h"     // generated: cmake/EmbedShipped.cmake

#include <ctime>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>

namespace Huginn::Settings
{
    namespace
    {
        /// The file's bytes; nullopt when it does not exist or cannot be read.
        std::optional<std::string> ReadFileBytes(const std::filesystem::path& path)
        {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec) || ec) {
                return std::nullopt;
            }
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                return std::nullopt;
            }
            std::string bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
            if (in.bad()) {
                return std::nullopt;
            }
            return bytes;
        }

        bool WriteFileBytes(const std::filesystem::path& path, std::string_view bytes)
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.close();
            return !out.fail();
        }

        /// Replace `path` with `bytes` without ever leaving it half-written:
        /// write <path>.tmp, then rename it over `path` (MoveFileEx with
        /// replace). Under MO2 the rename goes through usvfs, which supports
        /// it (the result lands in the file's mod folder or in Overwrite;
        /// either way the game sees it). If the rename fails anyway, the file
        /// is written in place: the backup, made before this, still holds the
        /// old one.
        bool ReplaceFileBytes(const std::filesystem::path& path, std::string_view bytes)
        {
            auto tmp = path;
            tmp += ".tmp";
            std::error_code ec;
            if (WriteFileBytes(tmp, bytes)) {
                std::filesystem::rename(tmp, path, ec);
                if (!ec) {
                    return true;
                }
                logger::warn("[ShippedDefaults] Could not rename {} over {} ({}); writing it in place"sv,
                    tmp.string(), path.string(), ec.message());
            }
            std::filesystem::remove(tmp, ec);
            return WriteFileBytes(path, bytes);
        }

        /// <path>.<YYYYMMDD-HHMMSS>.bak, local time; a free name if a reset
        /// in the same second already took it. Backups are never deleted:
        /// the button is pressed rarely and each one is a file the player
        /// may want back.
        std::filesystem::path BackupPath(const std::filesystem::path& path)
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
            localtime_s(&local, &now);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
            std::error_code ec;
            for (int n = 1;; ++n) {
                auto bak = path;
                bak += n == 1 ? std::format(".{}.bak", stamp) : std::format(".{}-{}.bak", stamp, n);
                if (!std::filesystem::exists(bak, ec) || n >= 100) {
                    return bak;
                }
            }
        }

        void LogOverlay(const OverlayResult& r, std::string_view tag, const std::filesystem::path& path)
        {
            if (r.shipped < 0) {
                logger::error("[{}] The compiled-in defaults did not parse (SimpleIni error {})"sv, tag,
                    static_cast<int>(r.shipped));
            }
            if (r.player < 0) {
                logger::error("[{}] Failed to parse {} (SimpleIni error {}): the shipped defaults apply"sv, tag,
                    path.string(), static_cast<int>(r.player));
            }
        }

        /// Back up `path` to a timestamped .bak if it differs from `shipped`,
        /// then write `shipped`. `sameContent` decides "differs" (bytes for
        /// Huginn.ini; key by key for dMenu's INI, which dMenu re-saves in
        /// its own layout).
        RestoreResult::Outcome RestoreOne(const std::filesystem::path& path, std::string_view shipped,
                                          bool createIfAbsent, std::string_view what,
                                          bool (*sameContent)(std::string_view, std::string_view))
        {
            std::error_code ec;
            const bool exists = std::filesystem::exists(path, ec) && !ec;
            if (!exists && !createIfAbsent) {
                logger::info("[ShippedDefaults] {}: {} does not exist, nothing to reset"sv, what, path.string());
                return RestoreResult::Outcome::Absent;
            }
            if (exists) {
                const auto current = ReadFileBytes(path);
                if (!current) {
                    logger::error("[ShippedDefaults] {}: could not read {}; left as it is"sv, what, path.string());
                    return RestoreResult::Outcome::Failed;
                }
                if (sameContent(*current, shipped)) {
                    logger::info("[ShippedDefaults] {}: {} is already the shipped file"sv, what, path.string());
                    return RestoreResult::Outcome::AlreadyShipped;
                }
                const auto bak = BackupPath(path);
                if (!WriteFileBytes(bak, *current)) {
                    logger::error("[ShippedDefaults] {}: could not write the backup {}; {} left as it is"sv, what,
                        bak.string(), path.string());
                    return RestoreResult::Outcome::Failed;
                }
                logger::info("[ShippedDefaults] {}: the old file is kept as {}"sv, what, bak.string());
            } else {
                std::filesystem::create_directories(path.parent_path(), ec);
            }
            if (!ReplaceFileBytes(path, shipped)) {
                logger::error("[ShippedDefaults] {}: could not write {} (if the backup was made, it holds the old "
                              "file)"sv, what, path.string());
                return RestoreResult::Outcome::Failed;
            }
            logger::info("[ShippedDefaults] {}: wrote the shipped defaults to {} ({} bytes)"sv, what, path.string(),
                shipped.size());
            return RestoreResult::Outcome::Written;
        }
    }

    std::string_view ShippedMainIni() noexcept
    {
        return Shipped::MainIniText();
    }

    const std::string& ShippedDMenuIni()
    {
        // Built on first use. The descriptor is the repo's Huginn.json, which
        // huginn_core_tests parses with this same function, so a failure here
        // is a broken build, not a player's mistake; log it and fall back to
        // nothing (each [Widget]/[Debug] key then takes its loader constant).
        static const std::string text = [] {
            const auto r = Core::DMenu::DefaultsFromDescriptor(Shipped::DMenuJsonText());
            if (!r.Ok()) {
                logger::error("[ShippedDefaults] The compiled-in dMenu descriptor is unusable: {}"sv, r.error);
                return std::string{};
            }
            return Core::DMenu::ToIniText(r.entries);
        }();
        return text;
    }

    bool LoadMainIni(CSimpleIniA& out, std::string_view tag)
    {
        const auto path = GetMainIniPath();
        const auto bytes = ReadFileBytes(path);
        if (!bytes) {
            logger::warn("[{}] {} not found: every setting takes the shipped Huginn.ini's value"sv, tag,
                path.string());
        }
        const auto r = LoadOverlay(out, ShippedMainIni(), bytes ? std::string_view(*bytes) : std::string_view{});
        LogOverlay(r, tag, path);
        return bytes.has_value() && r.player >= 0;
    }

    bool LoadDMenuIni(CSimpleIniA& out, const std::filesystem::path& path, std::string_view tag)
    {
        const auto bytes = ReadFileBytes(path);
        if (!bytes) {
            logger::info("[{}] {} not found: [Widget] and [Debug] take the dMenu descriptor's defaults"sv, tag,
                path.string());
        }
        const auto r = LoadOverlay(out, ShippedDMenuIni(), bytes ? std::string_view(*bytes) : std::string_view{});
        LogOverlay(r, tag, path);
        return bytes.has_value() && r.player >= 0;
    }

    void LoadShippedMainIni(CSimpleIniA& out)
    {
        LogOverlay(LoadOverlay(out, ShippedMainIni(), {}), "ShippedDefaults"sv, GetMainIniPath());
    }

    void LoadShippedDMenuIni(CSimpleIniA& out)
    {
        LogOverlay(LoadOverlay(out, ShippedDMenuIni(), {}), "ShippedDefaults"sv, GetDMenuOwnIniPath());
    }

    RestoreResult RestoreShippedFiles()
    {
        RestoreResult r;
        // The main INI is written even when absent: a fresh install has one.
        // Huginn.ini byte for byte: the shipped file's comments are part of
        // what a reset restores.
        r.main = RestoreOne(GetMainIniPath(), ShippedMainIni(), /*createIfAbsent=*/true, "Huginn.ini"sv,
            [](std::string_view a, std::string_view b) { return a == b; });
        // dMenu's INI only when dMenu has made one. dMenu writes it itself, at
        // the descriptor's defaults, the first time it runs with Huginn.json,
        // and creating it for a player without dMenu would only add a file.
        const auto& dMenuText = ShippedDMenuIni();
        if (dMenuText.empty()) {
            r.dMenu = RestoreResult::Outcome::Failed;   // logged in ShippedDMenuIni
        } else {
            // dMenu's INI key by key (core/DMenuDefaults.h SameIniContent: a
            // BOM, CRLF, spacing, order and comments do not count), so a file
            // dMenu re-saved at the defaults is "already shipped" and gets no
            // backup.
            r.dMenu = RestoreOne(GetDMenuOwnIniPath(), dMenuText, /*createIfAbsent=*/false, "dMenu INI"sv,
                [](std::string_view a, std::string_view b) { return Core::DMenu::SameIniContent(a, b); });
        }
        return r;
    }
}
