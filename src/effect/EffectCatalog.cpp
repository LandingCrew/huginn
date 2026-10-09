#include "EffectCatalog.h"

#include "EffectReader.h"
#include "IniLoad.h"
#include "Profiling.h"
#include "core/MiniRegex.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

namespace Huginn::Effect
{
    namespace
    {
        using namespace Core::Effect;

        /// Builds the catalog the first time the main menu opens: after every
        /// plugin's kDataLoaded handler and the tasks they queued, so keyword
        /// distributors (KID, SPID) have finished. Stays registered; fires once.
        class MainMenuBuild final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                                  RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (a_event && a_event->opening && a_event->menuName == RE::MainMenu::MENU_NAME) Fire();
                return RE::BSEventNotifyControl::kContinue;
            }

            void Fire()
            {
                if (fired_.exchange(true)) return;
                logger::info("[EffectCatalog] main menu opened: building"sv);
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([]() { EffectCatalog::GetSingleton().Build(); });
                }
                else {
                    EffectCatalog::GetSingleton().Build();
                }
            }

        private:
            std::atomic<bool> fired_{ false };
        };

        MainMenuBuild g_mainMenuBuild;

        void LogBudgetHits(std::string_view where)
        {
            static std::atomic<std::size_t> logged{ 0 };
            const auto hits = Core::MiniRegex::BudgetExceeded();
            auto seen = logged.load();
            while (hits > seen) {
                if (logged.compare_exchange_weak(seen, hits)) {
                    logger::warn("[EffectCatalog] {} rule-pattern search(es) hit the regex step budget ({} in all, "
                                 "last in {}); a veto pattern read it as a veto, any other as no match"sv,
                                 hits - seen, hits, where);
                    break;
                }
            }
        }

        CatalogEntry MakeEntry(const ItemMapping& m, Cap cap, const Populations& pops)
        {
            CatalogEntry e;
            e.kind = m.kind;
            e.cap = std::move(cap);
            e.restoreAmount = m.restoreAmount;
            e.fullRestore = m.fullRestore;
            e.school = PrimarySchool(m, pops);
            return e;
        }
    }

    EffectCatalog& EffectCatalog::GetSingleton()
    {
        static EffectCatalog instance;
        return instance;
    }

    void EffectCatalog::LoadOverrides()
    {
        CSimpleIniA ini;
        if (!LoadIniFile(ini, std::filesystem::path("Data/SKSE/Plugins/Huginn_EffectOverrides.ini"),
                         "EffectCatalog"sv)) {
            return;
        }
        CSimpleIniA::TNamesDepend keys;
        ini.GetAllKeys("Overrides", keys);
        std::size_t bad = 0;
        for (const auto& k : keys) {
            const std::string key = k.pItem ? k.pItem : "";
            const char* value = ini.GetValue("Overrides", k.pItem, "");
            const auto bar = key.find('|');
            const auto col = FromName(value ? std::string_view(value) : std::string_view{});
            if (bar == std::string::npos || !col) {
                ++bad;
                logger::warn("[EffectCatalog] override '{}' = '{}' ignored: want <plugin>|<local FormID hex> = <column>"sv,
                    key, value ? value : "");
                continue;
            }
            std::string hex = key.substr(bar + 1);
            if (hex.starts_with("0x") || hex.starts_with("0X")) hex = hex.substr(2);
            std::uint32_t local = 0;
            const auto r = std::from_chars(hex.data(), hex.data() + hex.size(), local, 16);
            if (r.ec != std::errc{} || r.ptr != hex.data() + hex.size()) {
                ++bad;
                logger::warn("[EffectCatalog] override '{}' ignored: bad FormID"sv, key);
                continue;
            }
            overrides_.Add(std::string_view(key).substr(0, bar), local, *col);
        }
        logger::info("[EffectCatalog] {} effect override(s) loaded, {} ignored"sv, overrides_.Size(), bad);
    }

    void EffectCatalog::ScheduleBuild()
    {
        // Not a task queued from kDataLoaded: SKSE may run tasks queued during
        // its task pass in the same pass, so "a frame later" is not a promise.
        // The main menu opening is: it comes after every kDataLoaded handler.
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            Build();
            return;
        }
        ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_mainMenuBuild);
        logger::info("[EffectCatalog] build waits for the main menu to open"sv);
        if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) g_mainMenuBuild.Fire();
    }

    void EffectCatalog::Build()
    {
        if (built_) return;
        built_ = true;
        const auto t0 = std::chrono::steady_clock::now();

        // Forms are read here, on the calling (main) thread. The mapping is
        // pure code over plain records, so it runs on a worker: in a Debug
        // build it takes seconds on LoreRim, which the main thread should not
        // wait for. Nothing reads the catalog before Ready().
        logger::info("[EffectCatalog] reading the load order"sv);
        std::shared_ptr<ReadResult> read;
        try {
            Huginn_ZONE_NAMED("EffectCatalog::Read (main thread)");
            LoadOverrides();
            read = std::make_shared<ReadResult>(EffectReader{}.ReadLoadOrder());
        }
        catch (const std::exception& e) {
            failed_.store(true, std::memory_order_release);
            logger::error("[EffectCatalog] reading the load order failed: {}; the catalog stays empty"sv, e.what());
            return;
        }
        formsRead_ = read->items.size();
        const double readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        std::thread([this, read, readMs, t0]() {
            Huginn_SET_THREAD("Huginn effect catalog");
            try {
                Huginn_ZONE_NAMED("EffectCatalog::Map (worker)");
                const auto t1 = std::chrono::steady_clock::now();
                // Plain records only from here: the RE pointers in `read` are not touched.
                BuildResult result = BuildCaps(read->items, read->effects, &overrides_);
                for (std::size_t i = 0; i < result.mappings.size(); ++i) {
                    const auto& m = result.mappings[i];
                    if (!m.inScope) continue;
                    entries_.emplace(m.formId, MakeEntry(m, std::move(result.caps[i]), result.pops));
                }
                for (std::size_t i = 0; i < read->effects.size(); ++i) {
                    classes_.emplace(read->effects[i].formId, result.classes[i]);
                }
                pops_ = std::move(result.pops);
                tally_ = result.tally;
                const auto t2 = std::chrono::steady_clock::now();
                buildMs_ = std::chrono::duration<double, std::milli>(t2 - t0).count();
                const std::size_t effects = read->effects.size();
                ready_.store(true, std::memory_order_release);
                LogBudgetHits("the build"sv);

                logger::info("[EffectCatalog] {} of {} forms in scope, {} magic effects; coverage {:.2f}% ({} of {} "
                             "visible effect rows mapped; {} helper, {} wrapper, {} carrier rows not counted); read {:.0f} ms "
                             "(main thread), map {:.0f} ms (worker)"sv,
                    entries_.size(), formsRead_, effects, 100.0 * Coverage(), tally_.mapped, tally_.counted,
                    tally_.helper, tally_.wrapperUnknown, tally_.carrier, readMs,
                    std::chrono::duration<double, std::milli>(t2 - t1).count());
            }
            catch (const std::exception& e) {
                failed_.store(true, std::memory_order_release);
                logger::error("[EffectCatalog] mapping failed: {}; the catalog stays empty"sv, e.what());
            }
            catch (...) {
                failed_.store(true, std::memory_order_release);
                logger::error("[EffectCatalog] mapping failed (unknown exception); the catalog stays empty"sv);
            }
        }).detach();
    }

    bool EffectCatalog::WaitUntilReady(std::chrono::milliseconds timeout) const
    {
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (!Ready()) {
            if (Failed() || std::chrono::steady_clock::now() >= until) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }

    const CatalogEntry* EffectCatalog::Find(RE::FormID formID) const
    {
        if (!Ready()) return nullptr;
        const auto it = entries_.find(formID);
        return it == entries_.end() ? nullptr : &it->second;
    }

    const EffectClass* EffectCatalog::ClassOf(RE::FormID mgef) const
    {
        if (!Ready()) return nullptr;
        const auto it = classes_.find(mgef);
        return it == classes_.end() ? nullptr : &it->second;
    }

    std::optional<CatalogEntry> EffectCatalog::InstanceEntry(const RE::TESBoundObject* base,
                                                             const RE::ExtraDataList* extra) const
    {
        if (!Ready() || !base || !extra) return std::nullopt;
        const bool isWeapon = base->Is(RE::FormType::Weapon);
        if (!isWeapon && !base->Is(RE::FormType::Armor)) return std::nullopt;

        // Tempering: ExtraHealth is the stack's quality multiplier, 1.0 when
        // untempered; a present-but-zero reading means unfilled, i.e. 1.0 (see
        // WeaponRegistry). Weapons only. Tempering adds damage (Core::Effect::
        // TemperedWeaponDamage; WeaponData.h measured LoreRim at +2 for 1.2).
        float temper = 1.0f;
        if (isWeapon) {
            if (const auto* h = extra->GetByType<RE::ExtraHealth>(); h && h->health > 0.0f) temper = h->health;
        }
        const auto* xe = extra->GetByType<RE::ExtraEnchantment>();
        const RE::EnchantmentItem* playerEnchantment = xe ? xe->enchantment : nullptr;
        if (std::fabs(temper - 1.0f) < 1e-4f && !playerEnchantment) return std::nullopt;

        EffectReader reader;
        ReadResult rr;
        if (!reader.ReadForm(base, playerEnchantment, rr) || rr.items.empty()) return std::nullopt;
        auto& item = rr.items.front();
        item.damage = TemperedWeaponDamage(item.damage, temper);
        auto classes = ClassifyAll(rr.effects, &overrides_);
        ResolveZeroMagnitudes(rr.items, rr.effects, classes);
        const ItemMapping m = MapItem(item, rr.effects, classes);
        LogBudgetHits("a per-instance entry"sv);
        return MakeEntry(m, Grade(m, pops_), pops_);
    }
}
