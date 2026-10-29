#include "EffectCatalog.h"

#include "EffectReader.h"
#include "IniLoad.h"
#include "Profiling.h"
#include "core/MiniRegex.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <thread>

namespace Huginn::Effect
{
    namespace
    {
        using namespace Core::Effect;

        /// One slice of the main menu's load-order read. The read finishes at
        /// once when a game starts loading, so a quick Continue costs what is
        /// left of the ~3 s in the loading screen instead of a frozen menu.
        ///
        /// 12 ms since 0.23.48 (was 4). At 4 ms the LoreRim main menu read
        /// too little before the player loaded: the log of 2026-10-28
        /// 21:37:40.669 reads "load order read: 33375 forms, 10031 magic
        /// effects; 3176 ms on the main thread in 228 slice(s), longest
        /// 2016.4 ms, 7.0 s from start to end; the last 2016 ms at once
        /// (kPreLoadGame)" -- 227 slices (one a frame, ~33 a second over the
        /// 7 s, ~46 over the ~4.9 s before the load) read ~1.2 of the 3.2 s,
        /// and the other 2 s held the main thread at kPreLoadGame. The main
        /// menu needs no smooth frame rate: a 12 ms slice reads three times
        /// as much per frame (~270 slices for ~3.2 s); per second somewhat
        /// less than three times, by however much the longer slice slows the
        /// menu's frames (not measured).
        constexpr auto kReadSliceBudget = std::chrono::milliseconds(12);

        /// Set when a slice ended with forms left to read; the next Present
        /// (EffectCatalog::OnFrame) clears it and queues the next slice.
        std::atomic<bool> g_sliceWanted{ false };

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
                    tasks->AddTask([]() { EffectCatalog::GetSingleton().BuildInSlices(); });
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
            e.perkRows = GradePerkRows(m, pops);
            return e;
        }
    }

    struct EffectCatalog::PendingRead
    {
        EffectReader reader;
        ReadResult result;
        Core::SliceCursor cursor;
        std::chrono::steady_clock::time_point t0;
        double busyMs = 0.0;      // main-thread time, every slice
        double longestMs = 0.0;   // the longest slice
        std::size_t slices = 0;
    };

    EffectCatalog::EffectCatalog() = default;
    EffectCatalog::~EffectCatalog() = default;

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

    void EffectCatalog::Build(std::string_view why)
    {
        std::lock_guard lock(readMutex_);
        if (!StartReadLocked()) return;
        ReadLocked(false, why);
    }

    void EffectCatalog::BuildInSlices()
    {
        std::lock_guard lock(readMutex_);
        if (!StartReadLocked()) return;
        ReadLocked(true, {});
    }

    void EffectCatalog::FinishPendingRead(std::string_view why)
    {
        std::lock_guard lock(readMutex_);
        if (pending_) ReadLocked(false, why);
    }

    void EffectCatalog::RunSlice()
    {
        std::lock_guard lock(readMutex_);
        if (pending_) ReadLocked(true, {});
    }

    void EffectCatalog::OnFrame()
    {
        if (!g_sliceWanted.load(std::memory_order_relaxed)) return;
        if (!g_sliceWanted.exchange(false, std::memory_order_acq_rel)) return;
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;   // not before SKSE hands it out; the load finishes the read
        try {
            tasks->AddTask([]() { EffectCatalog::GetSingleton().RunSlice(); });
        }
        catch (...) {
            g_sliceWanted.store(true, std::memory_order_release);   // the next frame tries again
        }
    }

    bool EffectCatalog::StartReadLocked()
    {
        if (built_) return pending_ != nullptr;   // a sliced read under way goes on
        built_ = true;
        logger::info("[EffectCatalog] reading the load order"sv);
        pending_ = std::make_unique<PendingRead>();
        pending_->t0 = std::chrono::steady_clock::now();
        return true;
    }

    void EffectCatalog::ReadLocked(bool sliced, std::string_view why)
    {
        auto& p = *pending_;
        const auto s0 = std::chrono::steady_clock::now();
        bool done = false;
        // The forms are read here, on the calling (main) thread, the per-form
        // reads unchanged (EffectReader::ReadForm); only where the read stops
        // and resumes is new. The overrides are part of the first slice.
        auto read = [&](const std::function<bool()>& stop) {
            if (p.slices == 0) LoadOverrides();
            return p.reader.ReadLoadOrderSlice(p.result, p.cursor, stop);
        };
        try {
            if (sliced) {
                Huginn_ZONE_NAMED("EffectCatalog::ReadSlice (main thread)");
                const auto until = s0 + kReadSliceBudget;
                done = read([until]() { return std::chrono::steady_clock::now() >= until; });
            }
            else {
                Huginn_ZONE_NAMED("EffectCatalog::ReadRest (main thread)");
                done = read([]() { return false; });
            }
        }
        catch (const std::exception& e) {
            failed_.store(true, std::memory_order_release);
            pending_.reset();
            g_sliceWanted.store(false, std::memory_order_release);
            logger::error("[EffectCatalog] reading the load order failed: {}; the catalog stays empty"sv, e.what());
            return;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
        p.busyMs += ms;
        p.longestMs = std::max(p.longestMs, ms);
        ++p.slices;
        if (!done) {
            g_sliceWanted.store(true, std::memory_order_release);   // OnFrame queues the next slice
            return;
        }
        FinishReadLocked(sliced ? std::string{} : std::format("; the last {:.0f} ms at once ({})", ms, why));
    }

    void EffectCatalog::FinishReadLocked(std::string_view lastAtOnce)
    {
        auto p = std::move(pending_);
        g_sliceWanted.store(false, std::memory_order_release);
        const auto t0 = p->t0;
        const double readMs = p->busyMs;   // main-thread time, every slice
        auto read = std::make_shared<ReadResult>(std::move(p->result));
        formsRead_ = read->items.size();
        logger::info("[EffectCatalog] load order read: {} forms, {} magic effects; {:.0f} ms on the main thread in {} "
                     "slice(s), longest {:.1f} ms, {:.1f} s from start to end{}"sv,
            formsRead_, read->effects.size(), readMs, p->slices, p->longestMs,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), lastAtOnce);
        p.reset();

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

    std::unique_ptr<CatalogEntry> EffectCatalog::MapLateForm(const RE::TESBoundObject* form)
    {
        if (!lateReader_) lateReader_ = std::make_unique<EffectReader>();
        ReadResult rr;
        if (!lateReader_->ReadForm(form, nullptr, rr) || rr.items.empty()) return nullptr;
        auto& item = rr.items.front();
        MarkHeld(item);   // held or known: rule 1's tome test does not apply
        auto classes = ClassifyAll(rr.effects, &overrides_);
        ResolveZeroMagnitudes(rr.items, rr.effects, classes);
        const ItemMapping m = MapItem(item, rr.effects, classes);
        LogBudgetHits("a late entry"sv);
        if (!m.inScope) {
            logger::debug("[EffectCatalog] {:08X} '{}' held but out of scope: no late entry"sv, item.formId, item.name);
            return nullptr;
        }
        auto entry = std::make_unique<CatalogEntry>(MakeEntry(m, Grade(m, pops_), pops_));
        logger::debug("[EffectCatalog] late entry for {:08X} '{}' ({}, {}): {} column(s)"sv, item.formId, item.name,
            KindName(item.kind), IsDynamicFormId(item.formId) ? "made at runtime" : "no tome teaches it",
            entry->cap.size());
        return entry;
    }

    const CatalogEntry* EffectCatalog::FindOrMap(const RE::TESBoundObject* form)
    {
        if (!form || !Ready()) return nullptr;
        const RE::FormID id = form->GetFormID();
        if (const auto* entry = Find(id)) return entry;
        const bool isSpell = form->GetFormType() == RE::FormType::Spell;
        if (!MapLate(id, isSpell)) return nullptr;
        std::lock_guard lock(lateMutex_);
        if (isSpell) {
            // 0.23.45: an ability, power or disease is never in scope (rule
            // 1), so no read: the ~200 abilities a character knows cost ~2.6
            // ms each to read and map, all at once after a load.
            const auto* s = static_cast<const RE::SpellItem*>(form);
            if (!SpellShapeInScope(static_cast<int>(s->GetSpellType()), static_cast<int>(s->GetCastingType()))) {
                late_.RememberNone(id);
                return nullptr;
            }
        }
        try {
            return late_.Get(id, [&]() { return MapLateForm(form); });
        }
        catch (const std::exception& e) {
            logger::warn("[EffectCatalog] late entry for {:08X} failed: {}"sv, id, e.what());
            return late_.Get(id, []() { return std::unique_ptr<CatalogEntry>{}; });
        }
    }

    const CatalogEntry* EffectCatalog::FindOrMapId(RE::FormID formID)
    {
        if (!Ready()) return nullptr;
        if (const auto* entry = Find(formID)) return entry;
        {
            // A remembered answer (entry or none) without a form lookup: the
            // eligible list is re-read every pipeline run, and an out-of-scope
            // key (a power, a static form the build left out) must not cost a
            // LookupByID each time.
            std::lock_guard lock(lateMutex_);
            if (late_.Contains(formID)) return late_.Peek(formID);
        }
        const auto* form = RE::TESForm::LookupByID(formID);
        const auto* bound = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (!bound) return nullptr;   // not remembered: the ID may name a form made later this load
        const auto* entry = FindOrMap(bound);   // remembers its answer when it maps
        if (!entry) {
            // Also the forms MapLate leaves out, which FindOrMap does not map.
            std::lock_guard lock(lateMutex_);
            late_.RememberNone(formID);
        }
        return entry;
    }

    void EffectCatalog::ForgetLate()
    {
        std::lock_guard lock(lateMutex_);
        // Only FF keys: a load-order form is the same form in every save of
        // this launch, so its answer (entry or none) stands (0.23.45).
        const std::size_t forgotten = late_.ForgetIf([](std::uint32_t key) { return IsDynamicFormId(key); });
        if (forgotten > 0 || late_.Live() > 0) {
            logger::debug("[EffectCatalog] load: {} late FF key(s) forgotten, {} load-order key(s) kept ({} made "
                          "since launch)"sv, forgotten, late_.Live(), late_.Made());
        }
    }

    std::size_t EffectCatalog::LateLive() const
    {
        std::lock_guard lock(lateMutex_);
        return late_.Live();
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
