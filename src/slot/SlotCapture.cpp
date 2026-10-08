#include "SlotSnapshot.h"

// =============================================================================
// SLOT CAPTURE SESSION (Debug, test mode only) -- recorded snapshots for R7
// =============================================================================
// The golden test (tests/core/SlotAllocGoldenTests.cpp) replays allocations
// the game recorded. A test save standing still produces few, and all alike,
// so in test mode with iCaptureSlotsSec = N (TestHarness.h; run_tests.py
// --capture-slots N) the harness, instead of ending the game after the
// after-load suites, plays N seconds of scripted input while every allocation
// is written out:
//
//   - a slot pressed every few seconds, through the hotkey's own path
//     (EquipManager::EquipSlot): spells and weapons go into hands, Remembrance
//     holds start, potions and food are used up;
//   - the page flipped forward and back (SlotAllocator::NextPage/PreviousPage);
//   - health, magicka and stamina dropped to 12% for a few seconds, so the
//     critical-vital overrides fire with the save's real potions, then
//     restored;
//   - the pipeline forced to re-run between steps (MarkPageDirty).
//
// Then a PERTURBATION CAMPAIGN on page 9 (no layout uses it, so no real page's
// seating is touched): the real candidate lists seen in the session (their
// real slot classes, names, dedup keys), re-scored and re-ordered at random --
// ties, zeros, 2x and 1.5x relations, small nudges, wildcards, remembered-only
// rows, overrides -- on the real layouts and on made-up ones, a few passes per
// sequence so the slot hold, seating and home keys act on what the previous
// pass left. Each pass goes through SlotAllocator::AllocateForTest, which the
// capture hook records like any other allocation (tag "campaign").
//
// Nothing here runs outside test mode, and nothing is saved: the harness ends
// the process when it is done.
// =============================================================================

#ifndef NDEBUG

#include "SlotAllocator.h"
#include "SlotSettings.h"
#include "input/EquipHand.h"
#include "input/EquipManager.h"
#include <atomic>
#include <mutex>
#include <random>
#include <thread>

namespace Huginn::Slot::Capture
{
    namespace
    {
        constexpr size_t kCampaignPage = MAX_PAGES - 1;
        constexpr int kSequences = 160;

        struct RealList
        {
            Scoring::ScoredCandidateList list;
            std::vector<SlotConfig> layout;
        };
        std::mutex g_listMutex;
        std::array<RealList, MAX_PAGES> g_real;
        std::vector<RealList> g_history;   // a few distinct lists over the session
        std::atomic<bool> g_sessionStarted{ false };

        // Vitals the session lowered, to put back.
        std::array<float, 3> g_lowered{};

        RE::ActorValue VitalOf(size_t i)
        {
            static constexpr RE::ActorValue kVitals[] = { RE::ActorValue::kHealth, RE::ActorValue::kMagicka,
                RE::ActorValue::kStamina };
            return kVitals[i % 3];
        }

        void LowerVital(size_t i)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            auto* av = player->AsActorValueOwner();
            const auto v = VitalOf(i);
            const float cur = av->GetActorValue(v);
            const float max = av->GetPermanentActorValue(v);
            const float target = 0.12f * max;
            if (max > 0.0f && cur > target) {
                av->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, v, -(cur - target));
                g_lowered[i % 3] += cur - target;
                SKSE::log::info("[SlotCapture] lowered vital {} to 12% for the overrides", i % 3);
            }
        }

        void RestoreVital(size_t i)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || g_lowered[i % 3] <= 0.0f) return;
            player->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, VitalOf(i), g_lowered[i % 3]);
            g_lowered[i % 3] = 0.0f;
        }

        void PressSlot(uint32_t r)
        {
            auto& allocator = SlotAllocator::GetSingleton();
            const size_t count = allocator.GetSlotCount();
            if (count == 0) return;
            const size_t slot = r % count;
            const bool ok = Input::EquipManager::GetSingleton().EquipSlot(slot, Input::EquipHand::Right);
            SKSE::log::info("[SlotCapture] pressed slot {} ({})", slot, ok ? "equipped" : "nothing to equip");
        }

        // One scripted step, on the main thread.
        void PlayStep(size_t step, uint32_t r)
        {
            auto& allocator = SlotAllocator::GetSingleton();
            switch (step % 12) {
            case 0: PressSlot(r); break;
            case 1: LowerVital(0); break;          // health: CriticalHealth
            case 2: PressSlot(r); break;
            case 3: RestoreVital(0); LowerVital(1); break;   // magicka
            case 4: allocator.NextPage(); break;
            case 5: RestoreVital(1); LowerVital(2); break;   // stamina
            case 6: PressSlot(r); break;
            case 7: RestoreVital(2); allocator.PreviousPage(); break;
            case 8: PressSlot(r >> 8); break;
            case 9: PressSlot(r >> 16); break;
            case 10: LowerVital(0); LowerVital(1); break;
            case 11: RestoreVital(0); RestoreVital(1); break;
            default: break;
            }
            allocator.MarkPageDirty();
        }

        // --- the campaign ------------------------------------------------------

        std::vector<SlotConfig> MadeUpLayout(size_t which, std::mt19937& rng)
        {
            using SC = SlotClassification;
            std::vector<SlotConfig> out;
            auto add = [&](SC c, int8_t prio, OverrideFilter f = OverrideFilter::None) {
                SlotConfig s;
                s.classification = c;
                s.priority = prio;
                s.overrideFilter = f;
                s.skipEquipped = false;
                s.wildcardsEnabled = true;
                out.push_back(s);
            };
            switch (which % 4) {
            case 0:   // the shipped shape: eight Regular keys
                for (int i = 0; i < 8; ++i) add(SC::Regular, static_cast<int8_t>(7 - i), i == 0 ? OverrideFilter::Any : OverrideFilter::None);
                break;
            case 1:   // keys with jobs, priorities out of slot order
                add(SC::DamageAny, 5); add(SC::HealingAny, 9, OverrideFilter::HP); add(SC::Regular, 3);
                add(SC::Regular, 3); add(SC::PotionsAny, 7, OverrideFilter::Any); add(SC::WeaponsAny, 6);
                add(SC::Regular, 1); add(SC::FoodAny, 2); add(SC::Regular, 0); add(SC::Regular, 3, OverrideFilter::Other);
                break;
            case 2:   // equal priorities: slot order decides
                for (int i = 0; i < 6; ++i) add(SC::Regular, 0, i % 2 ? OverrideFilter::Any : OverrideFilter::None);
                break;
            default:  // a mixed page
                add(SC::SpellsAny, 4); add(SC::WeaponsMelee, 4); add(SC::Regular, 2, OverrideFilter::Any);
                add(SC::Regular, 2); add(SC::BuffsAny, 1); add(SC::Utility, 1); add(SC::Regular, 0);
                add(SC::AmmoAny, 0, OverrideFilter::Other);
                break;
            }
            std::uniform_int_distribution<int> d100(0, 99);
            for (auto& s : out) {
                if (d100(rng) < 15) s.wildcardsEnabled = !s.wildcardsEnabled;
                if (d100(rng) < 15) s.skipEquipped = !s.skipEquipped;
            }
            return out;
        }

        Scoring::ScoredCandidateList Mutate(const Scoring::ScoredCandidateList& base, std::mt19937& rng)
        {
            std::uniform_int_distribution<int> d100(0, 99);
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            static constexpr float kRound[] = { 0.1f, 0.2f, 0.25f, 0.4f, 0.5f, 0.8f, 1.0f, 1.5f, 2.0f, 3.0f };

            Scoring::ScoredCandidateList list;
            Scoring::ScoredCandidateList dropped;
            for (const auto& c : base) {
                if (c.isRememberedOnly) continue;
                auto copy = c;
                copy.isWildcard = false;
                (d100(rng) < 85 ? list : dropped).push_back(std::move(copy));
            }

            const int mode = d100(rng) % 7;
            for (auto& c : list) {
                switch (mode) {
                case 0: break;                                                          // as scored
                case 1: c.utility = 0.05f + 2.95f * unit(rng); break;                   // anything
                case 2: c.utility = kRound[static_cast<size_t>(d100(rng)) % 10]; break;  // ties, 2x, 1.5x
                case 3: c.utility *= 0.7f + 0.7f * unit(rng); break;                    // nudges around the hold
                case 4: {                                                               // zeros and dust
                    const int r = d100(rng);
                    if (r < 20) c.utility = 0.0f;
                    else if (r < 30) c.utility = 1e-30f;
                    break;
                }
                case 5: if (d100(rng) < 50) c.utility *= 0.5f; break;                   // halves: the cap's 0.5
                default: if (d100(rng) < 40) c.utility *= 1.5f; break;                  // the hold's 1.5
                }
            }

            // The scorer's order: sorted, or only the top 10 sorted.
            const int order = d100(rng) % 3;
            if (order == 0 || list.size() <= 10) {
                std::stable_sort(list.begin(), list.end());
            } else {
                std::partial_sort(list.begin(), list.begin() + 10, list.end());
                if (order == 1) {
                    std::shuffle(list.begin() + 10, list.end(), rng);
                } else {
                    std::reverse(list.begin() + 10, list.end());
                }
            }

            // Wildcards swapped up into a rank position, as WildcardManager does.
            if (list.size() > 3 && d100(rng) < 30) {
                const int n = 1 + d100(rng) % 2;
                for (int k = 0; k < n; ++k) {
                    const size_t from = 2 + static_cast<size_t>(d100(rng)) % (list.size() - 2);
                    const size_t to = 1 + static_cast<size_t>(d100(rng)) % std::min<size_t>(5, from);
                    list[from].isWildcard = true;
                    std::swap(list[from], list[to]);
                }
            }

            // Remembered-only rows, at the end, as PipelineCoordinator adds them.
            if (!dropped.empty() && d100(rng) < 25) {
                auto r = dropped[static_cast<size_t>(d100(rng)) % dropped.size()];
                r.utility = 0.0f;
                r.isRememberedOnly = true;
                list.push_back(std::move(r));
            }
            return list;
        }

        Override::OverrideCollection MakeOverrides(const Scoring::ScoredCandidateList& base, std::mt19937& rng)
        {
            Override::OverrideCollection out;
            std::uniform_int_distribution<int> d100(0, 99);
            if (base.empty() || d100(rng) >= 35) {
                return out;
            }
            using OC = Override::OverrideCondition;
            using Cat = Override::OverrideCategory;
            const int n = 1 + d100(rng) % 2;
            for (int k = 0; k < n; ++k) {
                const auto& pick = base[static_cast<size_t>(d100(rng)) % base.size()];
                Override::OverrideResult r;
                switch (d100(rng) % 5) {
                case 0: r.category = Cat::HP; r.condition = OC::CriticalHealth; r.priority = 100; break;
                case 1: r.category = Cat::MP; r.condition = OC::CriticalMagicka; r.priority = 90; break;
                case 2: r.category = Cat::SP; r.condition = OC::CriticalStamina; r.priority = 80; break;
                case 3: r.category = Cat::Other; r.condition = OC::Drowning; r.priority = 95; break;
                default: r.category = Cat::Other; r.condition = OC::LowAmmo; r.priority = 50; break;
                }
                r.reason = "capture campaign";
                r.candidate = pick.candidate;
                out.activeOverrides.push_back(std::move(r));
            }
            out.SortByPriority();
            return out;
        }

        void RunCampaign()
        {
            std::vector<RealList> lists;
            {
                std::lock_guard lock(g_listMutex);
                for (const auto& r : g_real) {
                    if (!r.list.empty()) lists.push_back(r);
                }
                for (const auto& r : g_history) lists.push_back(r);
            }
            if (lists.empty()) {
                SKSE::log::warn("[SlotCapture] no real candidate list was seen; no campaign");
                return;
            }
            auto& allocator = SlotAllocator::GetSingleton();
            const uint32_t generation = allocator.CurrentGenerationForTest();
            std::mt19937 rng(0x52370001u);
            const size_t before = Count();
            SetThreadTag("campaign");
            for (int s = 0; s < kSequences; ++s) {
                const auto& base = lists[static_cast<size_t>(rng()) % lists.size()];
                const bool realLayout = (rng() % 3) == 0 && !base.layout.empty();
                const auto layout = realLayout ? base.layout : MadeUpLayout(rng(), rng);
                const int passes = 2 + static_cast<int>(rng() % 3);
                for (int p = 0; p < passes; ++p) {
                    const auto list = Mutate(base.list, rng);
                    const auto overrides = MakeOverrides(base.list, rng);
                    (void)allocator.AllocateForTest(kCampaignPage, generation, layout, list, overrides);
                }
            }
            SetThreadTag(nullptr);
            SKSE::log::info("[SlotCapture] campaign: {} snapshot(s) from {} list(s)", Count() - before, lists.size());
        }
    }  // namespace

    void NoteRealList(size_t pageIndex, const Scoring::ScoredCandidateList& candidates,
        const std::vector<SlotConfig>& slotConfigs)
    {
        if (!Enabled() || pageIndex >= MAX_PAGES || pageIndex == kCampaignPage || candidates.empty()) {
            return;
        }
        std::lock_guard lock(g_listMutex);
        auto& slot = g_real[pageIndex];
        // Keep an earlier list too when this one differs in size: the session's
        // presses and drinks change the inventory, and the campaign wants both.
        if (!slot.list.empty() && slot.list.size() != candidates.size() && g_history.size() < 6) {
            g_history.push_back(slot);
        }
        slot.list = candidates;
        slot.layout = slotConfigs;
    }

    void StartSession(int seconds, void (*done)())
    {
        if (g_sessionStarted.exchange(true)) {
            return;
        }
        SetEnabled(true);
        std::thread([seconds, done]() {
            using namespace std::chrono_literals;
            SKSE::log::info("[SlotCapture] session: {}s of scripted play, then the campaign", seconds);
            std::this_thread::sleep_for(3s);   // let the first passes after the load settle
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            std::mt19937 rng(0x52370002u);
            size_t step = 0;
            while (std::chrono::steady_clock::now() < end) {
                const uint32_t r = rng();
                const size_t s = step++;
                SKSE::GetTaskInterface()->AddTask([s, r]() { PlayStep(s, r); });
                std::this_thread::sleep_for(1500ms);
            }
            SKSE::GetTaskInterface()->AddTask([]() {
                for (size_t i = 0; i < 3; ++i) RestoreVital(i);
            });
            std::this_thread::sleep_for(1s);
            const size_t real = Count();
            RunCampaign();
            SKSE::log::info("[SlotCapture] session done: {} snapshot(s) ({} from play)", Count(), real);
            SetEnabled(false);
            if (done) {
                done();
            }
        }).detach();
    }
}  // namespace Huginn::Slot::Capture

#endif  // NDEBUG
