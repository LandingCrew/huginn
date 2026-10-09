#include "TestHarness.h"

#ifndef NDEBUG

#include "IniLoad.h"
#include "slot/SlotSnapshot.h"
#include "effect/EffectDump.h"
#include "pipeline/PipelineCoordinator.h"
#include "slot/SlotAllocator.h"
#include "update/UpdateHandler.h"

#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Huginn::TestHarness
{
    namespace
    {
        constexpr auto kFlagFileName = "Huginn_TestMode.ini";
        // From the main menu opening to the load call: lets the menu (and any
        // mod that hooks it) finish settling before a load is asked for.
        constexpr auto kAutoLoadDelay = std::chrono::seconds(3);
        constexpr int kDefaultLoadTimeoutSec = 300;

        struct SuiteCounts
        {
            uint32_t errorLines = 0;
            std::string skipReason;   // the first MarkSkipped reason; empty = not skipped
        };

        // The suite running on THIS thread, or null. A thread_local, so lines
        // other threads log during a suite are never counted against it.
        thread_local SuiteCounts* t_current = nullptr;

        class CountingSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex>
        {
        protected:
            void sink_it_(const spdlog::details::log_msg& msg) override
            {
                SuiteCounts* counts = t_current;
                if (!counts) {
                    return;
                }
                if (msg.level >= spdlog::level::err) {
                    ++counts->errorLines;
                }
            }
            void flush_() override {}
        };

        struct Tally
        {
            uint32_t suites = 0;
            uint32_t passed = 0;
            uint32_t failed = 0;
            uint32_t skipped = 0;
            uint32_t failLines = 0;
            std::vector<std::string> failedSuites;
            std::vector<std::string> skippedSuites;

            void Add(const Tally& o)
            {
                suites += o.suites;
                passed += o.passed;
                failed += o.failed;
                skipped += o.skipped;
                failLines += o.failLines;
                failedSuites.insert(failedSuites.end(), o.failedSuites.begin(), o.failedSuites.end());
                skippedSuites.insert(skippedSuites.end(), o.skippedSuites.begin(), o.skippedSuites.end());
            }

            [[nodiscard]] static std::string Join(const std::vector<std::string>& names)
            {
                std::string out;
                for (const auto& n : names) {
                    if (!out.empty()) out += ',';
                    out += n;
                }
                return out.empty() ? "-" : out;
            }

            [[nodiscard]] std::string Counts() const
            {
                return std::format(
                    "suites={} passed={} failed={} skipped={} fail_lines={} failed_suites={} skipped_suites={}",
                    suites, passed, failed, skipped, failLines, Join(failedSuites), Join(skippedSuites));
            }
        };

        // Suites run on the main thread (kDataLoaded, kPostLoadGame), one at a
        // time, but the load watchdog's Finish runs on its own thread and reads
        // g_total: every touch of the tallies holds g_tallyMutex.
        std::mutex g_tallyMutex;
        Tally g_phase;
        Tally g_total;

        bool g_active = false;
        std::string g_saveName;
        // Test mode only: write `hg dump all` to this file in the SKSE log
        // folder after the main-menu suites (R2: fresh dumps from the runner).
        std::string g_dumpAllName;
        int g_loadTimeoutSec = kDefaultLoadTimeoutSec;
        int g_captureSlotsSec = 0;   // iCaptureSlotsSec: slot snapshots after the load suites (R7)
        int g_captureNeeds = 0;      // iCaptureNeeds: need snapshots, at most this many (R3)
        int g_dumpRecsAfterSec = 0;  // iDumpRecsAfterSec: a recs dump after the load suites, then end
        std::atomic<bool> g_loadRequested{ false };
        std::atomic<bool> g_loadArrived{ false };
        std::atomic<bool> g_finished{ false };

        std::string ReadEnv(const char* name)
        {
            char* buf = nullptr;
            size_t len = 0;
            std::string out;
            if (_dupenv_s(&buf, &len, name) == 0 && buf) {
                out = buf;
            }
            std::free(buf);
            return out;
        }

        bool Truthy(std::string_view v)
        {
            return v == "1" || v == "true" || v == "TRUE" || v == "True" || v == "yes" || v == "on";
        }

        std::string StripEss(std::string name)
        {
            constexpr std::string_view ext = ".ess";
            if (name.size() > ext.size()) {
                std::string tail = name.substr(name.size() - ext.size());
                for (auto& ch : tail) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (tail == ext) name.resize(name.size() - ext.size());
            }
            return name;
        }

        [[noreturn]] void Quit(bool pass)
        {
            spdlog::default_logger()->flush();
            // Hard exit, not Main::quitGame: a modded game's normal shutdown can
            // hang or crash, and a test run must neither wait on it nor save.
            SKSE::WinAPI::TerminateProcess(SKSE::WinAPI::GetCurrentProcess(), pass ? 0u : 1u);
        }

        // Exactly once per launch: the DONE line, then the process ends.
        void Finish(std::string_view reason)
        {
            std::string counts;
            bool pass = false;
            {
                std::scoped_lock lock(g_tallyMutex);
                // Under the lock: the main thread and the watchdog can both get
                // here; the first one through logs and ends the process.
                if (g_finished.exchange(true)) {
                    return;
                }
                pass = g_total.failed == 0 && reason.empty();
                counts = g_total.Counts();
            }
            logger::info("[HuginnTest] DONE result={} {} reason={}"sv,
                pass ? "PASS"sv : "FAIL"sv, counts, reason.empty() ? "-"sv : reason);
            Quit(pass);
        }

        void RequestLoad()
        {
            if (g_loadRequested.exchange(true)) {
                return;
            }
            std::thread([]() {
                std::this_thread::sleep_for(kAutoLoadDelay);
                SKSE::GetTaskInterface()->AddTask([]() {
                    auto* manager = RE::BGSSaveLoadManager::GetSingleton();
                    if (!manager) {
                        logger::error("[HuginnTest] no BGSSaveLoadManager; cannot load '{}'"sv, g_saveName);
                        Finish("no-save-manager");
                        return;
                    }
                    logger::info("[HuginnTest] loading save '{}'"sv, g_saveName);
                    // a_checkForMods = false: a plugin-list mismatch would
                    // otherwise stop the load on a dialog nobody is there to click.
                    manager->Load(g_saveName.c_str(), false);
                });

                // Watchdog: a missing save just leaves the game at the menu, with
                // no kPostLoadGame. Say so instead of waiting on the runner. It
                // stands down once kPostLoadGame arrives (the suites then decide).
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(g_loadTimeoutSec);
                while (std::chrono::steady_clock::now() < deadline) {
                    if (g_finished.load() || g_loadArrived.load()) {
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                if (!g_finished.load() && !g_loadArrived.load()) {
                    logger::error("[HuginnTest] save '{}' did not load within {}s"sv, g_saveName, g_loadTimeoutSec);
                    Finish("load-timeout");
                }
            }).detach();
        }

        class MainMenuWatcher final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (a_event && a_event->opening && a_event->menuName == RE::MainMenu::MENU_NAME) {
                    RequestLoad();
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        void ArmAutoLoad()
        {
            static MainMenuWatcher watcher;
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                logger::error("[HuginnTest] no RE::UI; cannot watch for the main menu"sv);
                Finish("no-ui");
                return;
            }
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&watcher);
            logger::info("[HuginnTest] will load '{}' once the main menu opens"sv, g_saveName);
            if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
                RequestLoad();
            }
        }
    }  // namespace

    std::shared_ptr<spdlog::sinks::sink> MakeCountingSink()
    {
        return std::make_shared<CountingSink>();
    }

    void ReadTestMode()
    {
        std::string source;
        if (Truthy(ReadEnv("HUGINN_TEST_MODE"))) {
            g_active = true;
            g_saveName = ReadEnv("HUGINN_TEST_SAVE");
            g_captureSlotsSec = std::atoi(ReadEnv("HUGINN_CAPTURE_SLOTS").c_str());
            g_captureNeeds = std::atoi(ReadEnv("HUGINN_CAPTURE_NEEDS").c_str());
            source = "environment";
        }

        // The one-shot file. Deleted whatever it says, so a run that died
        // before cleaning up cannot turn the next ordinary launch into a test.
        if (auto dir = SKSE::log::log_directory()) {
            const auto path = *dir / kFlagFileName;
            std::error_code ec;
            if (std::filesystem::exists(path, ec)) {
                CSimpleIniA ini;
                ini.SetUnicode();
                const bool parsed = LoadIniFile(ini, path, "HuginnTest"sv);
                std::filesystem::remove(path, ec);
                if (parsed) {
                    // 64-bit: GetLongValue is a 32-bit long on Windows.
                    long long expires = 0;
                    if (const char* raw = ini.GetValue("Test", "iExpiresUnix", nullptr); raw && *raw) {
                        expires = std::strtoll(raw, nullptr, 10);
                    }
                    const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    if (expires != 0 && now > expires) {
                        logger::warn("[HuginnTest] ignored {}: expired {}s ago (deleted)"sv, kFlagFileName, now - expires);
                    } else if (ini.GetBoolValue("Test", "bEnabled", false)) {
                        g_active = true;
                        if (const char* save = ini.GetValue("Test", "sSaveName", nullptr); save && *save) {
                            g_saveName = save;
                        }
                        g_loadTimeoutSec = static_cast<int>(
                            ini.GetLongValue("Test", "iLoadTimeoutSec", kDefaultLoadTimeoutSec));
                        g_captureSlotsSec = static_cast<int>(ini.GetLongValue("Test", "iCaptureSlotsSec", 0));
                        g_captureNeeds = static_cast<int>(ini.GetLongValue("Test", "iCaptureNeeds", 0));
                        g_dumpRecsAfterSec = static_cast<int>(ini.GetLongValue("Test", "iDumpRecsAfterSec", 0));
                        if (const char* dump = ini.GetValue("Test", "sDumpAll", nullptr); dump && *dump) {
                            g_dumpAllName = dump;
                        }
                        source = source.empty() ? "file" : source + "+file";
                    }
                }
            }
        }

        if (g_active) {
            g_saveName = StripEss(g_saveName);
            if (g_loadTimeoutSec <= 0) g_loadTimeoutSec = kDefaultLoadTimeoutSec;
            logger::info("[HuginnTest] test mode ON (from {}): save='{}', load timeout {}s; the game ends after the suites"sv,
                source, g_saveName, g_loadTimeoutSec);
            if (g_captureSlotsSec > 0) {
                logger::info("[HuginnTest] after the load suites: {}s of slot capture (Huginn_SlotSnapshots.txt)"sv,
                    g_captureSlotsSec);
            }
            if (g_captureNeeds > 0) {
                logger::info("[HuginnTest] need capture: up to {} snapshot(s) (Huginn_NeedSnapshots.txt)"sv,
                    g_captureNeeds);
            }
        }
    }

    bool Active() noexcept
    {
        return g_active;
    }

    int NeedCaptureLimit() noexcept
    {
        return g_captureNeeds;
    }

    void OnGameLoaded() noexcept
    {
        g_loadArrived.store(true);
    }

    void MarkSkipped(std::string_view reason)
    {
        if (SuiteCounts* counts = t_current; counts && counts->skipReason.empty()) {
            counts->skipReason = reason.empty() ? std::string("unspecified") : std::string(reason);
        }
    }

    void RunSuite(const char* name, void (*suite)())
    {
        SuiteCounts counts;
        t_current = &counts;
        bool threw = false;
        try {
            suite();
        } catch (const std::exception& e) {
            t_current = nullptr;
            logger::error("[HuginnTest] suite {} threw: {}"sv, name, e.what());
            threw = true;
        } catch (...) {
            t_current = nullptr;
            logger::error("[HuginnTest] suite {} threw a non-std exception"sv, name);
            threw = true;
        }
        t_current = nullptr;

        // A failure outranks a skip: a suite that logged an error and then
        // skipped the rest still failed.
        std::string_view outcome;
        {
            std::scoped_lock lock(g_tallyMutex);
            ++g_phase.suites;
            g_phase.failLines += counts.errorLines;
            if (threw || counts.errorLines > 0) {
                ++g_phase.failed;
                g_phase.failedSuites.emplace_back(name);
                outcome = "FAILED"sv;
            } else if (!counts.skipReason.empty()) {
                ++g_phase.skipped;
                g_phase.skippedSuites.emplace_back(name);
                outcome = "SKIPPED"sv;
            } else {
                ++g_phase.passed;
                outcome = "passed"sv;
            }
        }
        if (counts.skipReason.empty()) {
            logger::info("[HuginnTest] suite {} {} ({} error line(s))"sv, name, outcome, counts.errorLines);
        } else {
            logger::info("[HuginnTest] suite {} {} ({} error line(s); skipped: {})"sv,
                name, outcome, counts.errorLines, counts.skipReason);
        }
    }

    namespace
    {
        // A plain file name only: no folder, nothing outside the log folder.
        void DumpAllIfAsked()
        {
            if (g_dumpAllName.empty()) return;
            const auto dir = SKSE::log::log_directory();
            const bool plain = g_dumpAllName.find_first_of("/\\:") == std::string::npos && g_dumpAllName != "." &&
                               g_dumpAllName != "..";
            if (!dir || !plain) {
                logger::error("[HuginnTest] dump all skipped: bad file name '{}' or no log folder"sv, g_dumpAllName);
                return;
            }
            std::string summary;
            // After the save has loaded: the catalog was built when the main
            // menu opened (after the keyword distributors), and its worker has
            // had the whole load to finish; wait for it if not.
            const bool ok = Effect::WriteDumpAll(*dir / g_dumpAllName, summary, std::chrono::seconds(120));
            if (ok) logger::info("[HuginnTest] dump all: {}"sv, summary);
            else logger::error("[HuginnTest] dump all failed: {}"sv, summary);
        }
    }

    void EndPhase(Phase phase, bool gameLoaded)
    {
        const auto phaseName = phase == Phase::Menu ? "menu"sv : "load"sv;
        std::string counts;
        {
            std::scoped_lock lock(g_tallyMutex);
            counts = g_phase.Counts();
            g_total.Add(g_phase);
            g_phase = {};
        }
        logger::info("[HuginnTest] RESULT phase={} {}"sv, phaseName, counts);

        if (!g_active || g_finished.load()) {
            return;
        }
        if (phase == Phase::Menu) {
            if (g_saveName.empty()) {
                if (!g_dumpAllName.empty()) {
                    logger::error("[HuginnTest] sDumpAll needs a save (sSaveName): the dump runs after the load"sv);
                }
                Finish({});
            } else {
                ArmAutoLoad();
            }
            return;
        }
        if (!g_loadRequested.load()) {
            // A load the harness did not ask for (the player's own, before the
            // auto-load fired): its suites are tallied, the run goes on.
            return;
        }
        if (gameLoaded) DumpAllIfAsked();
        if (gameLoaded && g_dumpRecsAfterSec > 0 && g_captureSlotsSec <= 0) {
            // A recommendation dump after N idle seconds (the `hg recs 40`
            // path), then end: two builds on one save can be compared.
            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::seconds(g_dumpRecsAfterSec));
                SKSE::GetTaskInterface()->AddTask([]() {
                    logger::info("[HuginnTest] dumping recommendations"sv);
                    Pipeline::PipelineCoordinator::GetSingleton().RequestRecommendationDump(40);
                    Slot::SlotAllocator::GetSingleton().MarkPageDirty();
                    Update::UpdateHandler::GetSingleton()->ForceUpdate();
                });
                std::this_thread::sleep_for(std::chrono::seconds(3));
                Finish({});
            }).detach();
            return;
        }
        if (gameLoaded && g_captureSlotsSec > 0) {
            // Slot snapshots for the golden test (SlotCapture.cpp): play a
            // scripted session while every allocation is recorded, then end.
            Slot::Capture::StartSession(g_captureSlotsSec, []() { Finish({}); });
            return;
        }
        Finish(gameLoaded ? std::string_view{} : "load-failed"sv);
    }
}  // namespace Huginn::TestHarness

#else  // NDEBUG: no suites, no test mode

namespace Huginn::TestHarness
{
    std::shared_ptr<spdlog::sinks::sink> MakeCountingSink() { return nullptr; }
    void ReadTestMode() {}
    bool Active() noexcept { return false; }
    int NeedCaptureLimit() noexcept { return 0; }
    void OnGameLoaded() noexcept {}
    void MarkSkipped(std::string_view) {}
    void RunSuite(const char*, void (*)()) {}
    void EndPhase(Phase, bool) {}
}

#endif
