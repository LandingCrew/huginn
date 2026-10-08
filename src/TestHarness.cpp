#include "TestHarness.h"

#ifndef NDEBUG

#include "IniLoad.h"

#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
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
            uint32_t skipLines = 0;
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
                } else if (msg.level == spdlog::level::warn) {
                    // Tests.cpp's own phrase ("[Test] ItemRegistry not ready,
                    // skipping tests"). Not bare "skipping": registries warn
                    // "... skipping" about single forms (a weapon that fails to
                    // classify), which is no reason to call a suite skipped.
                    const std::string_view text(msg.payload.data(), msg.payload.size());
                    if (text.find("skipping tests") != std::string_view::npos) {
                        ++counts->skipLines;
                    }
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

            void Add(const Tally& o)
            {
                suites += o.suites;
                passed += o.passed;
                failed += o.failed;
                skipped += o.skipped;
                failLines += o.failLines;
                failedSuites.insert(failedSuites.end(), o.failedSuites.begin(), o.failedSuites.end());
            }

            [[nodiscard]] std::string Counts() const
            {
                std::string names;
                for (const auto& n : failedSuites) {
                    if (!names.empty()) names += ',';
                    names += n;
                }
                return std::format("suites={} passed={} failed={} skipped={} fail_lines={} failed_suites={}",
                    suites, passed, failed, skipped, failLines, names.empty() ? "-" : names);
            }
        };

        // Suites run on the main thread (kDataLoaded, kPostLoadGame), one at a
        // time; only the watchdog thread reads g_finished.
        Tally g_phase;
        Tally g_total;

        bool g_active = false;
        std::string g_saveName;
        int g_loadTimeoutSec = kDefaultLoadTimeoutSec;
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
            if (g_finished.exchange(true)) {
                return;
            }
            const bool pass = g_total.failed == 0 && reason.empty();
            logger::info("[HuginnTest] DONE result={} {} reason={}"sv,
                pass ? "PASS"sv : "FAIL"sv, g_total.Counts(), reason.empty() ? "-"sv : reason);
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
                    const long long expires = ini.GetLongValue("Test", "iExpiresUnix", 0);
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
        }
    }

    bool Active() noexcept
    {
        return g_active;
    }

    void OnGameLoaded() noexcept
    {
        g_loadArrived.store(true);
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

        ++g_phase.suites;
        g_phase.failLines += counts.errorLines;
        std::string_view outcome;
        if (threw || counts.errorLines > 0) {
            ++g_phase.failed;
            g_phase.failedSuites.emplace_back(name);
            outcome = "FAILED"sv;
        } else if (counts.skipLines > 0) {
            ++g_phase.skipped;
            outcome = "SKIPPED"sv;
        } else {
            ++g_phase.passed;
            outcome = "passed"sv;
        }
        logger::info("[HuginnTest] suite {} {} ({} error line(s))"sv, name, outcome, counts.errorLines);
    }

    void EndPhase(Phase phase, bool gameLoaded)
    {
        const auto phaseName = phase == Phase::Menu ? "menu"sv : "load"sv;
        logger::info("[HuginnTest] RESULT phase={} {}"sv, phaseName, g_phase.Counts());
        g_total.Add(g_phase);
        g_phase = {};

        if (!g_active || g_finished.load()) {
            return;
        }
        if (phase == Phase::Menu) {
            if (g_saveName.empty()) {
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
        Finish(gameLoaded ? std::string_view{} : "load-failed"sv);
    }
}  // namespace Huginn::TestHarness

#else  // NDEBUG: no suites, no test mode

namespace Huginn::TestHarness
{
    std::shared_ptr<spdlog::sinks::sink> MakeCountingSink() { return nullptr; }
    void ReadTestMode() {}
    bool Active() noexcept { return false; }
    void OnGameLoaded() noexcept {}
    void RunSuite(const char*, void (*)()) {}
    void EndPhase(Phase, bool) {}
}

#endif
