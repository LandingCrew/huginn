#include "SelectionLogV3.h"

// =============================================================================
// SELECTION LOG V3 TEST SESSION (Debug, test mode only) -- R4's unattended run
// =============================================================================
// After the after-load suites (TestHarness: bDecisionSession=1, run_tests.py
// --decision-session), a scripted session that makes a record of every
// outcome it can without a player, in the test file
// Huginn_Selections_v3_test.jsonl (never the player's log):
//
//   nothing  health dropped to 30% for 4 s and restored, nothing pressed:
//            a health_deficit episode that ends unanswered;
//   key      three Huginn keys pressed through the hotkey's own path
//            (EquipManager::EquipSlot);
//   wheel    if Wheeler is connected and the page has a wheel: an item of
//            the page equipped as Wheeler would, then Huginn's own activation
//            handler run (WheelerClient::SimulateItemActivatedForTest).
//            Wheeler's UI is not driven;
//   menu     the inventory opened (UIMessageQueue), a carried armour piece
//            and a weapon (or potion) off the page equipped from it, the menu
//            closed. The menu sink logs the update-tick measurement.
//
// Then it waits for the writer, logs one summary line and fails the run
// (an error line) when key, menu or nothing is missing, or wheel when a wheel
// existed. Nothing is saved: the harness ends the process.
// =============================================================================

#ifndef NDEBUG

#include "Globals.h"
#include "PipelineStateCache.h"
#include "UtilityScorer.h"
#include "input/EquipHand.h"
#include "input/EquipManager.h"
#include "slot/SlotAllocator.h"
#include "util/InventoryUtil.h"
#include "wheeler/WheelSync.h"
#include "wheeler/WheelerClient.h"

#include <thread>

#ifdef GetObject
#    undef GetObject   // windows.h's GetObjectA would replace BGSDefaultObjectManager::GetObject
#endif

namespace Huginn::Learning::SelectionLogV3
{
    namespace
    {
        using namespace std::chrono_literals;

        // Touched only from the session's SKSE tasks (Task, below), one at a time
        // and seconds apart. Not "the main thread", as this said: in gameplay
        // SKSE tasks appear to drain on job threads (DropAheadProbe.h; inferred),
        // in a pausing menu on the main thread (UpdateLoop.cpp, THREADS above
        // OnUpdate).
        float g_healthLowered = 0.0f;
        std::atomic<bool> g_wheelAttempted{ false };   // set in a task, read by the session thread

        void Task(void (*fn)())
        {
            if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([fn]() { fn(); });
        }

        void LowerHealth()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            auto* av = player->AsActorValueOwner();
            const float cur = av->GetActorValue(RE::ActorValue::kHealth);
            const float max = av->GetPermanentActorValue(RE::ActorValue::kHealth);
            const float target = 0.3f * max;
            if (max > 0.0f && cur > target) {
                av->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -(cur - target));
                g_healthLowered = cur - target;
            }
            logger::info("[DecisionSession] health {:.0f} -> {:.0f} of {:.0f}: a health_deficit episode, nothing pressed"sv,
                cur, target, max);
        }

        void RestoreHealth()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || g_healthLowered <= 0.0f) return;
            player->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth,
                g_healthLowered);
            g_healthLowered = 0.0f;
            logger::info("[DecisionSession] health restored: the episode ends"sv);
        }

        void PressSlot(size_t which)
        {
            auto& allocator = Slot::SlotAllocator::GetSingleton();
            const size_t count = allocator.GetSlotCount();
            if (count == 0) return;
            const size_t slot = which % count;
            const bool ok = Input::EquipManager::GetSingleton().EquipSlot(slot, Input::EquipHand::Right);
            logger::info("[DecisionSession] pressed key s{} ({})"sv, slot, ok ? "equipped" : "nothing to equip");
        }

        RE::BGSEquipSlot* RightHand()
        {
            auto* dom = RE::BGSDefaultObjectManager::GetSingleton();
            return dom ? dom->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kRightHandEquip) : nullptr;
        }

        void WheelPick()
        {
            g_wheelAttempted.store(false);
            auto& client = Wheeler::WheelerClient::GetSingleton();
            if (!client.IsConnected()) {
                logger::info("[DecisionSession] Wheeler not connected: no wheel pick"sv);
                return;
            }
            const size_t page = Slot::SlotAllocator::GetSingleton().GetCurrentPage();
            const int32_t wheel = Wheeler::WheelSync::GetSingleton().WheelIndexForPage(page);
            if (wheel < 0) {
                logger::info("[DecisionSession] page {} has no Wheeler wheel: no wheel pick"sv, page);
                return;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* equip = RE::ActorEquipManager::GetSingleton();
            if (!player || !equip) return;
            const auto shown = PipelineStateCache::GetSingleton().TakeShown();
            for (const auto& s : shown.shown) {
                // A Remembrance hold put back is the player's undo, not a pick
                // (WheelerClient withdraws it); an override is not the ranking's.
                if (s.type == Slot::AssignmentType::Remembered || s.type == Slot::AssignmentType::Override) continue;
                auto* form = RE::TESForm::LookupByID(s.formID);
                if (!form || player->GetEquippedObject(false) == form || player->GetEquippedObject(true) == form) continue;
                if (auto* weapon = form->As<RE::TESObjectWEAP>(); weapon && !weapon->IsBow() && !weapon->IsCrossbow()) {
                    equip->EquipObject(player, weapon);
                }
                else if (auto* spell = form->As<RE::SpellItem>(); spell && !form->Is(RE::FormType::Scroll)) {
                    equip->EquipSpell(player, spell, RightHand());
                }
                else {
                    continue;
                }
                g_wheelAttempted.store(true);
                logger::info("[DecisionSession] wheel {} entry {}: {:08X} '{}' equipped, Huginn's activation handler run"sv,
                    wheel, s.slotIndex, s.formID, form->GetName());
                Wheeler::WheelerClient::SimulateItemActivatedForTest(wheel, static_cast<int32_t>(s.slotIndex), s.formID);
                return;
            }
            logger::info("[DecisionSession] no weapon or spell on page {} to pick from the wheel"sv, page);
        }

        // Wildcards on every eligible slot, so the logged pages carry wildcard
        // rows and their propensities (the shipped odds show one ~20% of the
        // time). Test mode only; nothing is saved.
        float g_wcBase = 0.0f, g_wcMax = 0.0f, g_wcRefractory = 0.0f;   // session tasks only (see above)

        void ForceWildcards()
        {
            if (!g_utilityScorer) return;
            auto& wc = g_utilityScorer->GetWildcardManager();
            g_wcBase = wc.GetBaseProbability();
            g_wcMax = wc.GetMaxProbability();
            g_wcRefractory = wc.GetRefractoryPeriod();
            wc.SetBaseProbability(1.0f);
            wc.SetMaxProbability(1.0f);
            wc.SetRefractoryPeriod(0.0f);
            Slot::SlotAllocator::GetSingleton().MarkPageDirty();
            logger::info("[DecisionSession] wildcards forced on (probability 1, no refractory)"sv);
        }

        void RestoreWildcards()
        {
            if (!g_utilityScorer) return;
            auto& wc = g_utilityScorer->GetWildcardManager();
            wc.SetBaseProbability(g_wcBase);
            wc.SetMaxProbability(g_wcMax);
            wc.SetRefractoryPeriod(g_wcRefractory);
        }

        void OpenInventory()
        {
            if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                q->AddMessage(RE::InventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
                logger::info("[DecisionSession] opening the inventory menu"sv);
            }
        }

        void CloseInventory()
        {
            if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                q->AddMessage(RE::InventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                logger::info("[DecisionSession] closing the inventory menu"sv);
            }
        }

        bool IsShown(RE::FormID formID)
        {
            for (const auto& s : PipelineStateCache::GetSingleton().TakeShown().shown) {
                if (s.formID == formID) return true;
            }
            return false;
        }

        /// Equip, from the open menu, an item of `type` the player carries, does
        /// not wear and the page does not show.
        bool MenuEquip(RE::FormType type)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* equip = RE::ActorEquipManager::GetSingleton();
            if (!player || !equip) return false;
            auto* ui = RE::UI::GetSingleton();
            const bool open = ui && ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME);
            auto inventory = Util::GetInventorySafe(player, [type](RE::TESBoundObject& o) { return o.GetFormType() == type; });
            for (auto& [obj, data] : inventory) {
                auto& [count, entry] = data;
                if (!obj || count <= 0 || IsShown(obj->GetFormID())) continue;
                if (type == RE::FormType::Weapon) {
                    const auto* w = obj->As<RE::TESObjectWEAP>();
                    if (!w || w->IsBow() || w->IsCrossbow() || player->GetEquippedObject(false) == obj ||
                        player->GetEquippedObject(true) == obj) {
                        continue;
                    }
                }
                bool worn = false;
                if (entry && entry->extraLists) {
                    for (auto* xl : *entry->extraLists) {
                        if (xl && (xl->HasType<RE::ExtraWorn>() || xl->HasType<RE::ExtraWornLeft>())) worn = true;
                    }
                }
                if (worn) continue;
                const char* name = obj->GetName();
                equip->EquipObject(player, obj);
                logger::info("[DecisionSession] from the inventory menu ({}): {:08X} '{}' equipped"sv,
                    open ? "open" : "NOT open", obj->GetFormID(), name ? name : "?");
                return true;
            }
            return false;
        }

        void MenuEquipArmour()
        {
            if (!MenuEquip(RE::FormType::Armor)) logger::info("[DecisionSession] no unworn armour to equip"sv);
        }

        void MenuEquipOther()
        {
            if (!MenuEquip(RE::FormType::Weapon) && !MenuEquip(RE::FormType::AlchemyItem)) {
                logger::info("[DecisionSession] no off-page weapon or potion to equip"sv);
            }
        }
    }

    void StartTestSession(void (*done)(const char* failReason))
    {
        std::thread([done]() {
            const auto before = GetStats();
            std::this_thread::sleep_for(4s);    // let the first passes after the load settle

            Task(&LowerHealth);
            std::this_thread::sleep_for(4s);
            Task(&RestoreHealth);
            std::this_thread::sleep_for(6s);    // the 4 s grace, and a tick or two

            Task(&ForceWildcards);
            std::this_thread::sleep_for(1s);
            Task([]() { PressSlot(0); });
            std::this_thread::sleep_for(4s);
            Task([]() { PressSlot(1); });
            std::this_thread::sleep_for(4s);
            Task([]() { PressSlot(2); });
            std::this_thread::sleep_for(5s);

            Task(&WheelPick);
            std::this_thread::sleep_for(5s);
            Task(&RestoreWildcards);

            Task(&OpenInventory);
            std::this_thread::sleep_for(2s);
            Task(&MenuEquipArmour);
            std::this_thread::sleep_for(1s);
            Task(&MenuEquipOther);
            std::this_thread::sleep_for(3s);    // inside the menu: the confirm window runs
            Task(&CloseInventory);
            std::this_thread::sleep_for(6s);    // confirmations after the close

            const bool flushed = Flush(std::chrono::seconds(10));
            const auto s = GetStats();
            // What reached the file, not what was queued (a full queue drops).
            const uint64_t key = s.writtenKey - before.writtenKey, wheel = s.writtenWheel - before.writtenWheel,
                           menu = s.writtenMenu - before.writtenMenu, nothing = s.writtenNothing - before.writtenNothing;
            std::error_code ec;
            const auto path = FilePath();
            const auto size = path.empty() ? 0 : std::filesystem::file_size(path, ec);
            logger::info("[HuginnTest] decision session: key={} wheel={} menu={} nothing={} written (queued {}, dropped {}, "
                         "unlearned={}) bytes={} contexts={} caps={} file={} ({} bytes); tick {:.1f} us mean, {:.1f} us "
                         "max over {} tick(s), {} eligible row(s) last tick; context build {:.2f} ms mean, {:.2f} ms max "
                         "over {} ({} held read(s), {:.2f} ms max); {} short episode(s) dropped"sv,
                key, wheel, menu, nothing,
                (s.key + s.wheel + s.menu + s.nothing) - (before.key + before.wheel + before.menu + before.nothing),
                s.dropped - before.dropped, s.unlearned - before.unlearned, s.bytes - before.bytes,
                s.ctxWritten - before.ctxWritten, s.capsWritten - before.capsWritten, path.filename().string(), size,
                s.tickMeanUs, s.tickMaxUs, s.ticks, s.tickRows, s.buildMeanMs, s.buildMaxMs, s.contextsBuilt,
                s.heldReads, s.heldReadMaxMs, s.episodesDroppedShort);
            // A failure must reach the DONE line: this thread's error lines are
            // not counted (no suite runs on it), so the reason goes to done().
            const char* fail = "";
            if (!flushed) fail = "decision-flush-timeout";
            else if (key == 0) fail = "decision-no-key";
            else if (menu == 0) fail = "decision-no-menu";
            else if (nothing == 0) fail = "decision-no-nothing";
            else if (wheel == 0 && g_wheelAttempted.load()) fail = "decision-no-wheel";
            if (*fail) logger::error("[HuginnTest] TEST FAIL: decision session: {}"sv, fail);
            done(fail);
        }).detach();
    }
}

#else

namespace Huginn::Learning::SelectionLogV3
{
    void StartTestSession(void (*done)(const char*)) { done(""); }
}

#endif
