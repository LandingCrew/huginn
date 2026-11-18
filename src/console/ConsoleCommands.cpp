#include "ConsoleCommands.h"
#include "update/UpdateHandler.h"

#include "Globals.h"
#include "learning/FeatureBanditLearner.h"
#include "learning/ChoiceLearner.h"
#include "learning/legacy/LegacyLearnFeature.h"
#include "learning/StateFeatures.h"
#include "state/StateManager.h"
#include "state/DropAheadProbe.h"
#include "state/HandModelProbe.h"
#include "Profiling.h"
#include "core/ActorTypeClassifier.h"
#include "slot/SlotAllocator.h"
#include "slot/SlotLocker.h"
#include "slot/SlotSettings.h"
#include "spell/SpellRegistry.h"
#include "effect/Facets.h"
#include "core/Bootstrap.h"
#include "util/InventoryUtil.h"
#include "util/FormRead.h"
#include "effect/CrossFeatures.h"
#include "effect/EffectCatalog.h"
#include "effect/EffectDump.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include "settings/SettingsReloader.h"
#include "persist/ChoiceSerializer.h"
#include "pipeline/PipelineCoordinator.h"
#include "needs/NeedSnapshotBuilder.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <string>
#include <string_view>

namespace Huginn::Console
{
   using Util::AvName;
   using Util::CsvQuote;
   using Util::KeywordList;
   using Util::PluginOf;

   // =========================================================================
   // HELPERS
   // =========================================================================

   static void Print(const char* text)
   {
      if (auto* console = RE::ConsoleLog::GetSingleton()) {
      console->Print(text);
      }
   }

   /// @brief Trim leading/trailing whitespace and lower-case a string in-place.
   static std::string NormalizeCommand(std::string_view input)
   {
      // Skip leading whitespace
      auto start = input.find_first_not_of(" \t");
      if (start == std::string_view::npos) return {};
      auto end = input.find_last_not_of(" \t");
      std::string result(input.substr(start, end - start + 1));
      std::transform(result.begin(), result.end(), result.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return result;
   }

   // =========================================================================
   // SUBCOMMAND HANDLERS
   // =========================================================================

   static void Cmd_Help(std::string_view /*arg*/);  // Forward declaration (defined after kCommands)

   // =========================================================================
   // COMMAND TABLE
   // =========================================================================

   struct CommandEntry {
      std::string_view name;      // "refresh", "reset weights", etc.
      std::string_view helpText;  // "Force immediate recommendation update"
      bool takesArg;              // true for "weights", "page"
      void (*execute)(std::string_view);  // raw fn pointer (zero overhead)
   };

   static void Cmd_ResetWeights(std::string_view /*arg*/)
   {
      // Shared with the dMenu "reset learning data" button; serializes itself
      // via RunExclusive and resets SlotLocker alongside the weight table.
      const auto reset = Settings::SettingsReloader::ResetLearningData();
      if (!reset) {
         Print("No learning to reset (load a game first)");
         return;
      }

      auto msg = std::format("Learning data cleared ({} learner items; {})", reset->learnerItems, reset->choice);
      Print(msg.c_str());
      logger::info("[Console] {}"sv, msg);
   }

   struct RegistryCounts {
      size_t spells = 0, items = 0, weapons = 0, scrolls = 0, apparel = 0;
   };

   static RegistryCounts RebuildRegistries()
   {
      RegistryCounts c;
      if (g_spellRegistry) {
         g_spellRegistry->RebuildRegistry();
         c.spells = g_spellRegistry->GetSpellCount();
      }
      if (g_itemRegistry) {
         g_itemRegistry->RebuildRegistry();
         c.items = g_itemRegistry->GetItemCount();
      }
      if (g_weaponRegistry) {
         g_weaponRegistry->RebuildRegistry();
         c.weapons = g_weaponRegistry->GetWeaponCount();
      }
      if (g_scrollRegistry) {
         g_scrollRegistry->RebuildRegistry();
         c.scrolls = g_scrollRegistry->GetScrollCount();
      }
      if (g_apparelRegistry) {
         // RebuildRegistry() alone would leave this EMPTY: for apparel it only
         // clears, because a load-path scan cannot read player enchantments (see
         // ApparelRegistry.h). Reconcile immediately afterwards so `hg rebuild`
         // ends with a populated registry like every other line it prints —
         // otherwise the command silently contradicts the apparel count that
         // `hg status` reports, and the player is told to wait 30s by nothing.
         g_apparelRegistry->RebuildRegistry();
         g_apparelRegistry->ReconcileApparel();
         c.apparel = g_apparelRegistry->GetApparelCount();
      }
      return c;
   }

   static size_t CountLockedSlots()
   {
      auto& slotLocker = Slot::SlotLocker::GetSingleton();
      const size_t slotCount = Slot::SlotAllocator::GetSingleton().GetSlotCount();
      size_t count = 0;
      for (size_t i = 0; i < slotCount; ++i) {
         if (slotLocker.IsSlotLocked(i)) {
            ++count;
         }
      }
      return count;
   }

   static void Cmd_ResetAll(std::string_view /*arg*/)
   {
      // Run under the update mutex to prevent data races with the update loop.
      // Without this, the console thread could reset subsystems mid-update.
      Huginn::Update::UpdateHandler::GetSingleton()->RunExclusive([&] {
         // 1. Clear learning data. A reload of the same character keeps this
         //    cleared state (the tick below moves the learning clock past any
         //    older save); a different character or a later save restores
         //    from it.
         size_t learnerItems = 0;
         if (g_featureBanditLearner) {
            learnerItems = g_featureBanditLearner->GetItemCount();
            g_featureBanditLearner->Clear();
         }
         // R8.3: theta back to the bootstrap, b cleared (as `hg reset weights`);
         // with the model off, the THTA / BIAS carried from the load emptied.
         const char* choice = Persist::ResetChoiceLearning(g_activeCharacterID.load(std::memory_order_relaxed));
         // Here, as ResetLearningData (which this cannot call: it takes
         // RunExclusive itself), whether or not either learner exists; the
         // old learner's Clear above ticked it too (twice in all, until R12).
         Core::LearningClock::Note(&g_learningClock, Core::LearningClock::Event::Reset);

         // 2. Rebuild all registries (console does full rebuild; init path reconciles)
         RebuildRegistries();

         // 3. Reset all stateful pipeline subsystems (shared with InitializeGameSystems)
         ResetPipelineSubsystems();

         auto msg = std::format("Full reset complete (old learner: {} items; {}; all subsystems reset)",
            learnerItems, choice);
         Print(msg.c_str());
         logger::info("[Console] {}"sv, msg);
      });
   }

   static void Cmd_Refresh(std::string_view /*arg*/)
   {
      // Unlock all slots so the re-score can freely reassign
      Slot::SlotLocker::GetSingleton().Reset();

      // Run one full update cycle immediately via UpdateHandler (serialized by its mutex)
      Huginn::Update::UpdateHandler::GetSingleton()->ForceUpdate();

      Print("Recommendations refreshed");
      logger::info("[Console] Forced recommendation refresh"sv);
   }

   static void Cmd_Recs(std::string_view arg)
   {
      int n = 10;
      if (!arg.empty()) {
         auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), n);
         if (ec != std::errc{} || n < 1) {
            Print("Usage: hg recs [N]  (N = 1-50, default 10)");
            return;
         }
         n = std::min(n, 50);
      }

      // Queue a full-detail dump, then force a pipeline pass so it logs now.
      // MarkPageDirty bypasses both skip gates; the dump fires inside ForceUpdate.
      Pipeline::PipelineCoordinator::GetSingleton().RequestRecommendationDump(
         static_cast<size_t>(n));
      Slot::SlotAllocator::GetSingleton().MarkPageDirty();
      Huginn::Update::UpdateHandler::GetSingleton()->ForceUpdate();

      auto msg = std::format("Top {} recommendations dumped to Huginn log", n);
      Print(msg.c_str());
   }

   // R3: the live need vector -- every need whose 0.05-step level is not 0,
   // its value and the sensor reading that went into its curve. Read from the
   // state as it stands now (not the last pipeline run), so it is current in a
   // quiet scene too. To the console and the log.
   static void Cmd_Needs(std::string_view /*arg*/)
   {
      Huginn_ZONE_NAMED("hg needs");
      const auto live = Needs::ReadLiveNeeds();
      const auto lines = Needs::FormatNeeds(live.vector);
      Print(std::format("Huginn needs ({} of {} non-zero; the choice score reads them):",
         lines.size(), Core::Needs::kNeedCount).c_str());
      logger::info("[Console] hg needs: {} non-zero"sv, lines.size());
      for (const auto& line : lines) {
         Print(("  " + line).c_str());
         logger::info("[Console]   {}"sv, line);
      }
      const auto& s = live.snapshot;
      // The drop reading's age counts unpaused time only (the probe hook does
      // not run while the game is paused, as it is with this console open).
      const auto sensors = State::StateManager::GetSingleton().GetNeedSensors();
      const auto probe = State::DropAheadProbe::Latest();
      const std::string dropAge = sensors.dropAgeSec < 0.0
                                      ? std::string("no reading")
                                      : std::format("reading {:.2f}s old unpaused, {}", sensors.dropAgeSec,
                                                    State::DropAheadProbe::StatusName(probe.status));
      // Deep water ahead comes from the same probe pass (0.23.19): the
      // deepest water under the probes; a landing at least the safe depth
      // deep is no drop.
      const auto extra = std::format("  drop ahead {}, deep water ahead {} ({}) | encumbrance {:.2f} | families 0x{:X} | combat {} ({}) | light {:.2f} held (raw {:.2f}){}",
         s.dropAhead < 0.0f ? std::string("not measured") : std::format("{:.0f} units", s.dropAhead),
         s.waterDepthAhead < 0.0f ? std::string("not measured")
                                  : std::format("{:.0f} units (safe landing >= {:.0f})", s.waterDepthAhead,
                                                Core::Needs::kSafeLandingDepth),
         dropAge,
         s.encumbrance, s.families, s.inCombat ? "on" : "off",
         s.inCombat ? std::format("{:.1f}s in", s.combatStartAgo)
                    : (s.combatEndAgo >= Core::Needs::kNever ? std::string("never since load")
                                                             : std::format("ended {:.1f}s ago", s.combatEndAgo)),
         s.light, State::StateManager::GetSingleton().GetWorldState().lightLevel,
         s.openDaylight ? " (open daylight)" : "");
      Print(extra.c_str());
      logger::info("[Console] {}"sv, extra);
      // time_to_edge and edge_approach (0.23.50, core/EdgeApproach.h): the
      // readings behind them, and the probe pass's proximity.
      const auto proximity = sensors.dropAgeSec < 0.0 ? std::string("-") : std::format("{:.2f}", probe.edgeProximity);
      const auto edge = std::format("  edge {} | speed toward it {:.0f} units/game s | proximity {} | approach {:+.2f}/game s (window {:.1f} game s)",
         s.edgeDistance < 0.0f ? std::string("none (or not measured)")
                               : std::format("{:.0f} units (a drop >= {:.0f})", s.edgeDistance, Core::Needs::kEdgeDropThreshold),
         s.edgeSpeed, proximity, s.edgeApproach, Core::Needs::EdgeApproachTracker::kWindowSec);
      Print(edge.c_str());
      logger::info("[Console] {}"sv, edge);
      // The survival stages behind hunger and thirst, and whether a survival
      // stage ability is what holds regen_suppressed on (0.23.31).
      const auto survival = std::format("  survival {} | hunger stage {} | thirst {} | regen suppressed {} (by a survival stage: {}) | health regen suppressed {} (by a survival stage: {})",
         s.survival ? "on" : "off", s.hungerStage,
         sensors.thirstDetected ? std::format("stage {} (DVSMP)", s.thirstStage) : std::string("no thirst system"),
         s.regenSuppressed ? "yes" : "no", sensors.survivalRegen.Any() ? "yes" : "no",
         s.healthRegenSuppressed ? "yes" : "no", sensors.survivalRegen.healthBlocked ? "yes" : "no");
      Print(survival.c_str());
      logger::info("[Console] {}"sv, survival);
   }

   // 0.23.39: the actors the target poll tracks right now, with the fields
   // the [Actors] log uses -- name, race, type, hostility, alive or dead,
   // summon and commander, distance band; the scoring target marked.
   // Perceivable facts only (CLAUDE.md "Forbidden Information"). To the
   // console and the log.
   static void Cmd_Actors(std::string_view /*arg*/)
   {
      const auto rows = State::StateManager::GetSingleton().DescribeTrackedActors();
      for (size_t i = 0; i < rows.size(); ++i) {
         const std::string line = i == 0 ? "Huginn actors: " + rows[i] : "  " + rows[i];
         Print(line.c_str());
         logger::info("[Console] hg actors: {}"sv, i == 0 ? rows[i] : line);
      }
   }

   // 0.23.44: the weapon model in each hand, live -- run it when a weapon
   // shows equipped but not in the hand. Where each biped's part for the
   // weapon hangs, culled or not, and the swap that put it there
   // (state/HandModelProbe.h). To the console and the log.
   static void Cmd_Hands(std::string_view /*arg*/)
   {
      const auto rows = State::HandModelProbe::DescribeHands(RE::PlayerCharacter::GetSingleton());
      for (size_t i = 0; i < rows.size(); ++i) {
         const std::string line = i == 0 ? "Huginn hands: " + rows[i] : "  " + rows[i];
         Print(line.c_str());
         logger::info("[Console] hg hands: {}"sv, i == 0 ? rows[i] : line);
      }
   }

   static void Cmd_Unlock(std::string_view /*arg*/)
   {
      size_t lockedCount = CountLockedSlots();
      Slot::SlotLocker::GetSingleton().Reset();

      auto msg = std::format("All slot locks cleared ({} were active)", lockedCount);
      Print(msg.c_str());
      logger::info("[Console] {}"sv, msg);
   }

   static void Cmd_Status(std::string_view /*arg*/)
   {
      // R8.3: what ranks, and the choice learner's counts (the smoke test
      // compares these across a save and a reload).
      {
         std::string line;
         if (g_choiceLearner && g_choiceLearner->Ready()) {
            const auto st = g_choiceLearner->GetStats();
            const auto [utc, sha] = g_choiceLearner->FileIdentity();
            // 0.24.17: engine B (no D2 feature) live by default, D2 in shadow.
            const char* const engines = Scoring::Legacy::Enabled() ? "on (D2 live: engine A; no shadow)"
                                      : Scoring::Legacy::ShadowEnabled() ? "off (engine B live), D2 in shadow"
                                                                         : "off (engine B live), no shadow";
            line = std::format("Ranking: choice score (bootstrap {} {}); theta {} entries, {} moved from the bootstrap; "
                               "b {} items; {} picks learned; legacy feature {}",
               utc.empty() ? "?" : utc, sha.substr(0, 12), st.thetaEntries, st.thetaMoved, st.bEntries,
               st.picksLearned, engines);
         } else {
            line = "Ranking: OLD UTILITY (FALLBACK) -- Huginn_Bootstrap.json missing or invalid, see the log";
         }
         Print(line.c_str());
         logger::info("[Console] hg status: {}"sv, line);
      }
      // 0.24.14: the discovery boost (core/DiscoveryBoost.h).
      if (g_choiceLearner) {
         const auto ds = g_choiceLearner->GetDiscoveryStats();
         const auto p = g_choiceLearner->GetSettings().discovery;
         const std::string line = std::format("Discovery boost: {} (x{:.2f}, halving every {:.0f} min of play); {} forms "
                                              "known to this character, {} boosted now{}",
            p.enabled ? "on" : "OFF", p.varMult, p.halfLifeSec / 60.0, ds.seen, ds.boosted,
            ds.live ? "" : " (a load under way: gains not counted)");
         Print(line.c_str());
         logger::info("[Console] hg status: {}"sv, line);
      }

      // The old learner: D2's input, not the ranking's model
      if (g_featureBanditLearner) {
      auto msg = std::format("Old learner (D2 input): {} items, {} total trains, {} forgotten, {:.1f}h of play",
        g_featureBanditLearner->GetItemCount(), g_featureBanditLearner->GetTotalTrainCount(),
        g_featureBanditLearner->GetForgottenTotal(), g_featureBanditLearner->GetPlaySeconds() / 3600.0);
      Print(msg.c_str());
      }

      // Registries
      auto regMsg = std::format("Registries: {} spells, {} items, {} weapons, {} scrolls, {} craft apparel",
      g_spellRegistry ? g_spellRegistry->GetSpellCount() : 0,
      g_itemRegistry ? g_itemRegistry->GetItemCount() : 0,
      g_weaponRegistry ? g_weaponRegistry->GetWeaponCount() : 0,
      g_scrollRegistry ? g_scrollRegistry->GetScrollCount() : 0,
      // #65. Worth reading as a diagnostic: 0 here on a character who owns
      // fortify gear means classification rejected it, which is the first thing
      // to check when apparel never surfaces at a bench.
      g_apparelRegistry ? g_apparelRegistry->GetApparelCount() : 0);
      Print(regMsg.c_str());

      // The weapon registry to the log, in full. The console cannot hold it,
      // and the count above cannot answer the question this exists for: the
      // registry tracks inventory STACKS, so two lines sharing a FormID with
      // different uids is a tempered weapon and its plain twin being kept
      // apart, and the `dmg=6.0 (base 4.0 x1.50)` pair is the temper model
      // laid next to what the game shows in the inventory.
      if (g_weaponRegistry) {
      g_weaponRegistry->LogAllWeapons();
      Print("Weapon registry dumped to the Huginn log");
      }

      // Scrolls too. LogAllScrolls has existed and gone uncalled, and the
      // reason to want it is that a scroll is not always a scroll: LoreRim
      // ships throwing knives as ScrollItem forms, so "is my 45-stack of Iron
      // Throwing Knives typed Damage" is a question about this registry and
      // there was no way to ask it.
      if (g_scrollRegistry) {
      g_scrollRegistry->LogAllScrolls();
      Print("Scroll registry dumped to the Huginn log");
      }

      // Pages (query before slot locks so we know the actual count)
      auto& slotAllocator = Slot::SlotAllocator::GetSingleton();

      // Slot locks
      size_t lockedCount = CountLockedSlots();
      auto slotMsg = std::format("Page: {} of {} ('{}'), {} slots, {} locked",
      slotAllocator.GetCurrentPage() + 1, slotAllocator.GetPageCount(),
      slotAllocator.GetCurrentPageName(),
      slotAllocator.GetSlotCount(), lockedCount);
      Print(slotMsg.c_str());
   }

   // Feature names for weight display (matches StateFeatures::ToArray() order)
   static constexpr const char* kFeatureNames[] = {
      "healthPct", "magickaPct", "staminaPct",
      "inCombat", "isSneaking", "distNorm",
      "tgtNone", "tgtHumanoid", "tgtUndead", "tgtBeast", "tgtConstruct", "tgtDragon", "tgtDaedra",
      "melee", "bow", "spell", "shield",
      "bias"
   };
   static_assert(std::size(kFeatureNames) == Learning::StateFeatures::NUM_FEATURES);

   static void Cmd_Weights(std::string_view arg)
   {
      if (!g_featureBanditLearner) {
         Print("FeatureBanditLearner not initialized (load a game first)");
         return;
      }

      if (arg.empty()) {
         Print("Usage: hg weights <hex FormID>");
         Print("  Example: hg weights 12FCC");
         return;
      }

      // Parse hex FormID
      RE::FormID formID = 0;
      auto result = std::from_chars(arg.data(), arg.data() + arg.size(), formID, 16);
      if (result.ec != std::errc{}) {
         Print("Invalid FormID. Use hex format, e.g.: hg weights 12FCC");
         return;
      }

      // Resolve the form for display
      auto* form = RE::TESForm::LookupByID(formID);
      const char* name = form ? form->GetName() : "???";

      auto weights = g_featureBanditLearner->GetWeights(formID);
      const float trains = g_featureBanditLearner->GetTrainCount(formID);

      if (!g_featureBanditLearner->HasItem(formID)) {
         auto msg = std::format("{:08X} '{}': no training data", formID, name);
         Print(msg.c_str());
         return;
      }

      // Compute current reward estimate using live state features
      auto& stateMgr = State::StateManager::GetSingleton();
      auto features = Learning::StateFeatures::FromState(
         stateMgr.GetPlayerState(), stateMgr.GetTargets());
      float qNow = g_featureBanditLearner->GetRewardEstimate(formID, features);
      float conf = g_featureBanditLearner->GetConfidence(formID);
      float ucb = g_featureBanditLearner->GetUCB(formID);

      // Header
      auto header = std::format("{:08X} '{}' ({:.2f} trains, {:.0f}% kept, est={:.3f}, conf={:.2f}, ucb={:.2f}):",
         formID, name, trains, 100.0f * g_featureBanditLearner->GetRetention(formID), qNow, conf, ucb);
      Print(header.c_str());

      // Print each weight with its feature name (only non-negligible ones to console)
      for (size_t i = 0; i < Learning::StateFeatures::NUM_FEATURES; ++i) {
         auto line = std::format("  {:>12s}: {:+.4f}", kFeatureNames[i], weights[i]);
         Print(line.c_str());
         logger::debug("[Weights] {:08X} {} = {:.4f}", formID, kFeatureNames[i], weights[i]);
      }
   }

   // =========================================================================
   // THETA (R11, map Phase 9): the choice learner from the console
   // =========================================================================
   // `hg theta` with no argument: what ranks, and how far the live weights
   // moved from the bootstrap (core/ThetaDrift.h, the [Soak] drift fields).
   // `hg theta <FormID>`: one item -- its b, its cap(i), the need x column
   // terms the live need vector gives it now (the [Recs] terms, all of them),
   // and its largest weights under any need. `hg dump theta` writes every
   // moved theta weight and every b to Huginn_Theta.csv. To the console and
   // the log. (`hg weights` shows the OLD learner, kept for D2.)

   static bool OpenDumpFile(std::string_view fileName, std::ofstream& out, std::filesystem::path& filePath);

   static std::string NeedColName(std::size_t k, std::size_t j)
   {
      return std::format("{}x{}",
         k < Core::Needs::kNeedCount ? Core::Needs::kNeeds[k].id : std::string_view("?"),
         j < Core::Effect::kColumnCount ? Core::Effect::kColumns[j].id : std::string_view("?"));
   }

   static void PrintAndLog(const std::string& line)
   {
      Print(line.c_str());
      logger::info("[Console] {}"sv, line);
   }

   static void Cmd_ThetaSummary()
   {
      if (!g_choiceLearner || !g_choiceLearner->Ready()) {
         PrintAndLog("hg theta: the choice learner is off (no usable Huginn_Bootstrap.json); ranking on the old utility");
         return;
      }
      const auto st = g_choiceLearner->GetStats();
      const auto d = g_choiceLearner->GetDrift();
      const auto [utc, sha] = g_choiceLearner->FileIdentity();
      PrintAndLog(std::format("hg theta: bootstrap {} {}; {} keyed weights, {} moved; {} picks learned, {} onsets ({} answered)",
         utc.empty() ? "?" : utc, sha.substr(0, 12), st.thetaEntries, st.thetaMoved, st.picksLearned, st.onsets,
         st.onsetsCounted));
      std::string at = "-";
      if (d.theta.maxSlot < Core::Choice::K * Core::Choice::J) {
         at = NeedColName(d.theta.maxSlot / Core::Choice::J, d.theta.maxSlot % Core::Choice::J);
      }
      PrintAndLog(std::format("  theta: {} of {} means moved, mean |d| {:.4f}, rms {:.4f}, largest {:+.4f} at {}; "
                              "{} variances moved, mean ln(var/boot) {:+.3f}",
         d.theta.meanMoved, d.theta.weights, d.theta.meanAbsDelta, d.theta.rmsDelta, d.theta.maxDelta, at,
         d.theta.varMoved, d.theta.meanLnVar));
      if (d.strength.weights > 0) {
         PrintAndLog(std::format("  strength: {} means moved, mean |d| {:.4f}, largest {:+.4f}",
            d.strength.meanMoved, d.strength.meanAbsDelta, d.strength.maxDelta));
      }
      PrintAndLog(std::format("  b: {} items, mean |mu| {:.3f}, largest {:.3f} ({:08X}); covered {:+.3f} (prior {:+.3f})",
         d.b.entries, d.b.meanAbsMu, d.b.maxAbsMu, d.b.maxForm, d.covered.mu, d.coveredBoot.mu));
      // The outside option at the live need vector (outcomes "three").
      if (const auto snap = g_choiceLearner->GetSnapshot(); snap && snap->model.three) {
         const auto live = Needs::ReadLiveNeeds();
         const auto n = snap->Needs(live.vector.value, live.vector.input);
         // The tick's own u0 (intercept + need terms + recent terms), as the
         // ranking pass computes it.
         const double nowSec = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
         const auto recent = g_choiceLearner->RecentValues(*snap, nowSec, g_loadGeneration.load(std::memory_order_relaxed));
         const auto tick = Core::Choice::MakeTick(snap->model, n, recent);
         PrintAndLog(std::format("  outside option now: u0 = {:+.3f} (intercept {:+.3f})", tick.u0, snap->model.u0Bias));
      }
      Print("  hg theta <FormID> for one item; hg dump theta writes every moved weight");
   }

   static void Cmd_Theta(std::string_view arg)
   {
      if (arg.empty()) {
         Cmd_ThetaSummary();
         return;
      }
      if (!g_choiceLearner || !g_choiceLearner->Ready()) {
         PrintAndLog("hg theta: the choice learner is off (no usable Huginn_Bootstrap.json)");
         return;
      }
      std::string_view hex = arg;
      if (hex.starts_with("0x") || hex.starts_with("0X")) hex.remove_prefix(2);
      RE::FormID formID = 0;
      if (std::from_chars(hex.data(), hex.data() + hex.size(), formID, 16).ec != std::errc{}) {
         Print("Usage: hg theta [hex FormID]  (e.g. hg theta 12FCC)");
         return;
      }
      auto* form = RE::TESForm::LookupByID(formID);
      const char* name = form ? form->GetName() : "???";
      const auto snap = g_choiceLearner->GetSnapshot();
      if (!snap) return;

      // b, as the score reads it now (the battery applied; not the favourite
      // life, which needs the candidate).
      const auto bIt = snap->b.find(formID);
      const auto bRead = snap->B(formID, false, g_choiceLearner->GetPlaySeconds());
      if (bIt != snap->b.end()) {
         PrintAndLog(std::format("{:08X} '{}': b mu {:+.3f} sd {:.3f} (read now {:+.3f}, kept {:.0f}%), {:.1f} picks, "
                                 "last {:.0f} min of play ago",
            formID, name, bIt->second.mu, std::sqrt(std::max(bIt->second.var, 0.0)), bRead.mu, 100.0 * bRead.retention,
            bIt->second.n, std::max(0.0, g_choiceLearner->GetPlaySeconds() - bIt->second.chosenAt) / 60.0));
      } else {
         PrintAndLog(std::format("{:08X} '{}': no b entry (prior: mu 0, sd {:.2f})", formID, name,
            std::sqrt(Core::Choice::kPriorVarB)));
      }
      // 0.24.14: newly gained? (the wildcard draw reads b's variance x m)
      if (const auto [mult, age] = g_choiceLearner->DiscoveryOf(formID); age >= 0.0) {
         PrintAndLog(std::format("  discovery boost: gained {:.0f} min of play ago, b's variance x{:.2f} in the "
                                 "wildcard draw{}", age / 60.0, mult,
            g_choiceLearner->GetSettings().discovery.enabled ? "" : " (bDiscoveryBoost off: x1 in effect)"));
      }

      auto& catalog = Effect::EffectCatalog::GetSingleton();
      const auto* entry = catalog.Ready() ? catalog.FindOrMapId(formID) : nullptr;
      if (!entry) {
         PrintAndLog("  no catalog entry: the score reads only b for it");
         return;
      }
      PrintAndLog(std::format("  {} cap: {}", Core::Effect::KindName(entry->kind), Core::Effect::FormatCap(entry->cap)));
      PrintAndLog(std::format("  facets: {}", Core::Facets::Names(Core::Facets::FromCap(entry->cap, entry->kind))));
      const auto attr = Core::Choice::MakeAttr(snap->model.grading, &entry->cap,
         std::array<float, Core::DecisionLog::kCrossCount>{});
      if (snap->model.theta.size() != Core::Choice::K * Core::Choice::J) return;

      // The terms now: n_k * mu_kj * a_j over the needs that are on.
      const auto live = Needs::ReadLiveNeeds();
      const auto n = snap->Needs(live.vector.value, live.vector.input);
      struct Term { std::size_t k, j; double value, mu, var, a; };
      std::vector<Term> now;
      for (std::size_t k = 0; k < Core::Choice::K; ++k) {
         if (n[k] == 0.0) continue;
         for (const auto& [j, a] : attr.a) {
            const auto& w = snap->model.Th(k, j);
            const double v = n[k] * w.mu * a;
            if (v != 0.0) now.push_back(Term{ k, j, v, w.mu, w.var, a });
         }
      }
      std::sort(now.begin(), now.end(), [](const Term& x, const Term& y) { return std::fabs(x.value) > std::fabs(y.value); });
      double needMu = 0.0;
      for (const auto& t : now) needMu += t.value;
      PrintAndLog(std::format("  now: need part {:+.3f} from {} term(s){}", needMu, now.size(),
         now.empty() ? " (no need it answers is on)" : ":"));
      for (std::size_t i = 0; i < now.size() && i < 10; ++i) {
         const auto& t = now[i];
         PrintAndLog(std::format("    {} = n {:.2f} * theta {:+.3f} (sd {:.3f}) * a {:.2f} = {:+.3f}",
            NeedColName(t.k, t.j), n[t.k], t.mu, std::sqrt(std::max(t.var, 0.0)), t.a, t.value));
      }

      // Its largest weights under any need (theta mu * a, the need at 1).
      std::vector<Term> any;
      for (std::size_t k = 0; k < Core::Choice::K; ++k) {
         for (const auto& [j, a] : attr.a) {
            const auto& w = snap->model.Th(k, j);
            if (w.mu != 0.0) any.push_back(Term{ k, j, w.mu * a, w.mu, w.var, a });
         }
      }
      std::sort(any.begin(), any.end(), [](const Term& x, const Term& y) { return std::fabs(x.value) > std::fabs(y.value); });
      PrintAndLog(std::format("  largest weights for it (need at 1): {} of {}", std::min<std::size_t>(any.size(), 8), any.size()));
      for (std::size_t i = 0; i < any.size() && i < 8; ++i) {
         const auto& t = any[i];
         PrintAndLog(std::format("    {} theta {:+.3f} (sd {:.3f}) * a {:.2f} = {:+.3f}",
            NeedColName(t.k, t.j), t.mu, std::sqrt(std::max(t.var, 0.0)), t.a, t.value));
      }
   }

   static void Cmd_DumpTheta(std::string_view /*arg*/)
   {
      if (!g_choiceLearner || !g_choiceLearner->Ready()) {
         Print("The choice learner is off (no usable Huginn_Bootstrap.json)");
         return;
      }
      std::ofstream out;
      std::filesystem::path filePath;
      if (!OpenDumpFile("Huginn_Theta.csv"sv, out, filePath)) return;
      const auto rows = g_choiceLearner->ThetaRows(/*movedOnly=*/true);
      out << "block,need,column,mu,var,bootMu,bootVar,formID,name,n,minutesSinceChosen\n";
      for (const auto& r : rows) {
         out << std::format("theta,{},{},{:.6g},{:.6g},{:.6g},{:.6g},,,,\n", Core::Needs::kNeeds[r.need].id,
            Core::Effect::kColumns[r.col].id, r.live.mu, r.live.var, r.boot.mu, r.boot.var);
      }
      const auto bs = g_choiceLearner->ExportB();
      for (const auto& b : bs) {
         auto* form = RE::TESForm::LookupByID(b.form);
         const char* name = form ? form->GetName() : nullptr;
         out << std::format("b,,,{:.6g},{:.6g},0,{:.6g},{:08X},{},{:.2f},{}\n", b.mu, b.var, Core::Choice::kPriorVarB,
            b.form, CsvQuote(name ? name : ""), b.n, b.minutesSinceChosen);
      }
      out.close();
      PrintAndLog(std::format("Wrote {} moved theta weights and {} b entries to Huginn_Theta.csv", rows.size(), bs.size()));
   }

   static void Cmd_Rebuild(std::string_view /*arg*/)
   {
      // Run under the update mutex to prevent data races with the update loop.
      Huginn::Update::UpdateHandler::GetSingleton()->RunExclusive([&] {
         auto c = RebuildRegistries();

         auto msg = std::format("Registries rebuilt ({} spells, {} items, {} weapons, {} scrolls, "
            "{} craft apparel)",
            c.spells, c.items, c.weapons, c.scrolls, c.apparel);
         Print(msg.c_str());
         logger::info("[Console] {}"sv, msg);
      });
   }

   static void Cmd_Reload(std::string_view /*arg*/)
   {
      // Delegate to SettingsReloader — the single source of truth for a full
      // reload — so the console path can't diverge from the dMenu path (which
      // previously happened: this command silently skipped Keybindings/Debug
      // and read the widget settings from the wrong INI).
      //
      // GetDMenuIniPath() resolves [Widget]/[Debug] to the dMenu INI when dMenu
      // is installed, so `hg reload` doesn't reset them. [Keybindings] and every
      // other section are read from the main INI inside ReloadAllSettings.
      //
      // ReloadAllSettings serializes itself via RunExclusive — wrapping the
      // call here again would deadlock (the update mutex is not re-entrant).
      Settings::SettingsReloader::GetSingleton().ReloadAllSettings(GetDMenuIniPath());

      // ReloadAllSettings already emits an in-game notification; add console
      // feedback for the `hg reload` invoker.
      Print("All settings reloaded from INI");
      logger::info("[Console] Full settings reload completed"sv);
   }

   static void Cmd_Page(std::string_view arg)
   {
      auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
      const size_t pageCount = slotAllocator.GetPageCount();

      if (arg.empty()) {
      auto msg = std::format("Current page: {} of {} ('{}')",
        slotAllocator.GetCurrentPage() + 1, pageCount,
        slotAllocator.GetCurrentPageName());
      Print(msg.c_str());
      return;
      }

      // Parse page number
      size_t pageNum = 0;
      auto [ptr, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), pageNum);
      if (ec != std::errc{} || pageNum < 1 || pageNum > pageCount) {
      auto msg = std::format("Invalid page number. Use 1-{}", pageCount);
      Print(msg.c_str());
      return;
      }

      slotAllocator.SetCurrentPage(pageNum - 1);  // User-facing is 1-based
      Slot::SlotLocker::GetSingleton().Reset();    // Unlock so new page can populate

      auto msg = std::format("Switched to page {} ('{}')",
      pageNum, slotAllocator.GetCurrentPageName());
      Print(msg.c_str());
      logger::info("[Console] {}"sv, msg);
   }

   // =========================================================================
   // DUMPS
   // =========================================================================
   // R11: the per-type dumps (spells, food, potions, scrolls, weapons,
   // apparel, diseases) are gone with the classifiers they printed:
   // `hg dump all` is the one load-order dump, in every build since R11 (mod
   // authors write effect overrides from it), with the effect catalog's view
   // and the slot class derived from it.

   // Shared by hg dump theta, hg dump weights and hg dump races: open
   // <SKSE log dir>/<fileName>, or say why not on the console and return false.
   static bool OpenDumpFile(std::string_view fileName, std::ofstream& out,
                            std::filesystem::path& filePath)
   {
      const auto logDir = SKSE::log::log_directory();
      if (!logDir) {
         Print("No SKSE log directory - cannot write the dump");
         return false;
      }
      filePath = *logDir / fileName;
      out.open(filePath, std::ios::trunc);
      if (!out) {
         const auto msg = std::format("Could not open {} for writing", fileName);
         Print(msg.c_str());
         logger::error("[Console] Failed to open {} for writing"sv, filePath.string());
         return false;
      }
      return true;
   }

   // `hg dump all` -- every item-like form in the LOAD ORDER in ONE schema, a
   // row per item x effect, and since 0.23.12 the effect catalog's view of it
   // (R2): each row's effect column, each item's cap(i), today's slot class,
   // and the doc 9 dump gaps. The writer and the column list live in
   // effect/EffectDump.*; tests/tools/EffectReport.cpp re-maps a dump on the
   // host. Ingredients are left out: Huginn dropped them (the user,
   // 2026-10-07) -- they matter only at an alchemy lab.
   static void Cmd_DumpAll(std::string_view /*arg*/)
   {
      Huginn_ZONE_NAMED("hg dump all");
      const auto logDir = SKSE::log::log_directory();
      if (!logDir) {
         Print("No SKSE log directory - cannot write the dump");
         return;
      }
      const auto filePath = *logDir / "Huginn_All.csv";
      std::string summary;
      const bool ok = Effect::WriteDumpAll(filePath, summary);
      Print(summary.c_str());
      if (ok) logger::info("[Console] {} -> {}"sv, summary, filePath.string());
      else logger::error("[Console] dump all failed: {}"sv, summary);
   }

#ifndef NDEBUG
   // `hg dump weights` -- every item the OLD learner holds (kept for the D2
   // legacy feature, BNDW), one row each: what it is, how much it has been
   // trained, how stale it is, and its full weight vector.
   //
   // R11: `kind` is the effect catalog's form kind and `class` its facets
   // (core/CapFacets.h); the old classifiers no longer describe items here.
   static void Cmd_DumpWeights(std::string_view /*arg*/)
   {
      if (!g_featureBanditLearner) {
         Print("FeatureBanditLearner not initialized (load a game first)");
         return;
      }
      std::ofstream out;
      std::filesystem::path filePath;
      if (!OpenDumpFile("Huginn_Weights.csv"sv, out, filePath)) return;

      // Copy out under the learner's lock, classify outside it.
      std::vector<Learning::FeatureBanditLearner::SerializedEntry> entries;
      uint32_t totalTrains = 0;
      g_featureBanditLearner->ExportData(
         [&entries](Learning::FeatureBanditLearner::SerializedEntry entry) {
            entries.push_back(entry);
         },
         totalTrains);


      out << "formID,plugin,name,kind,class,trainCount,confidence,minutesSinceChosen,retention,weightNorm";
      for (const char* feature : kFeatureNames) out << ',' << feature;
      out << '\n';

      size_t unresolved = 0;
      for (const auto& entry : entries) {
         std::string_view kind = "unresolved"sv, cls = "-"sv;
         std::string clsOwned;  // for class strings built at runtime
         auto* form = RE::TESForm::LookupByID(entry.formID);
         const char* name = form ? form->GetName() : nullptr;

         if (!form) {
            // Pseudo-items (Unarmed and the like) and forms from a plugin
            // that is no longer loaded.
            ++unresolved;
         } else if (const auto ff = Effect::FacetsOfForm(entry.formID); ff.known) {
            kind = Core::Effect::KindName(ff.kind);
            clsOwned = Core::Facets::Names(ff.facets);
            cls = clsOwned;
         } else if (form->As<RE::SpellItem>() || form->As<RE::ScrollItem>() || form->As<RE::AlchemyItem>() ||
                    form->As<RE::TESObjectWEAP>() || form->As<RE::TESAmmo>() || form->As<RE::TESSoulGem>() ||
                    form->As<RE::TESObjectARMO>() || form->As<RE::TESObjectLIGH>()) {
            kind = "uncatalogued"sv;
         } else {
            clsOwned = std::format("formType {}", static_cast<int>(form->GetFormType()));
            kind = "Other"sv;
            cls = clsOwned;
         }

         float norm = 0.0f;
         for (const float w : entry.weights) norm += w * w;

         out << std::format("{:08X},{},{},{},{},{:.2f},{:.3f},{},{:.3f},{:.4f}",
            entry.formID,
            CsvQuote(PluginOf(form)),
            CsvQuote(name ? name : ""),
            kind, cls,
            entry.trainCount,
            g_featureBanditLearner->GetConfidence(entry.formID),
            entry.minutesSinceChosen,
            g_featureBanditLearner->GetRetention(entry.formID),
            std::sqrt(norm));
         for (const float w : entry.weights) out << std::format(",{:.4f}", w);
         out << '\n';
      }
      out.close();

      auto msg = std::format("Wrote {} learner entries to Huginn_Weights.csv ({} total trains, {} unresolved)",
         entries.size(), totalTrains, unresolved);
      Print(msg.c_str());
      logger::info("[Console] {} -> {}"sv, msg, filePath.string());
   }

   // `hg cap <FormID>` -- one item's cap(i) from the effect catalog: the
   // static columns, and for each stack the player carries its per-instance
   // cap (tempered / player-enchanted) and the runtime cross-features. To the
   // console and the log. Since R8.3 (0.24.0) these are the choice score's
   // attributes (theta n a; `hg theta <FormID>` shows the score).
   static void Cmd_Cap(std::string_view arg)
   {
      std::string_view hex = arg;
      if (hex.starts_with("0x") || hex.starts_with("0X")) hex.remove_prefix(2);
      RE::FormID parsed = 0;
      const auto pr = std::from_chars(hex.data(), hex.data() + hex.size(), parsed, 16);
      const std::optional<RE::FormID> id = (!hex.empty() && pr.ec == std::errc{} && pr.ptr == hex.data() + hex.size())
         ? std::optional<RE::FormID>(parsed) : std::nullopt;
      if (!id) {
         Print("Usage: hg cap <FormID>   (hex, e.g. hg cap 0003EADE)");
         return;
      }
      auto& catalog = Effect::EffectCatalog::GetSingleton();
      if (!catalog.Ready()) {
         Print("The effect catalog is still building");
         return;
      }
      auto* form = RE::TESForm::LookupByID(*id);
      const char* name = form ? form->GetName() : nullptr;
      // 0.23.41: a player-made potion or a spell no tome teaches is mapped
      // late (EffectCatalog::FindOrMap), as the selection log does.
      // 0.24.30: an item the player has is graded among what they have
      // (held); its record's grade, within the load order, follows.
      const auto* record = catalog.FindRecord(*id);
      const auto* held = catalog.FindHeld(*id);
      const auto* entry = catalog.FindOrMapId(*id);
      auto line = std::format("[cap] {:08X} {}{} : {}", *id, name ? name : "?",
         held ? " (held: among what the player has)" : entry && !record ? " (late entry)" : "",
         entry ? Core::Effect::FormatCap(entry->cap) : std::string("not in the catalog (out of scope or unknown)"));
      Print(line.c_str());
      logger::info("{}"sv, line);
      if (held && record) {
         const auto rline = std::format("[cap]   record, within the load order: {}", Core::Effect::FormatCap(record->cap));
         Print(rline.c_str());
         logger::info("{}"sv, rline);
      }
      if (!entry || !form) return;

      auto* player = RE::PlayerCharacter::GetSingleton();
      // 0.23.30: rows that count only with a perk (beside cap(i), not in it),
      // and whether the player has it; then the cap with the ones met.
      if (!entry->perkRows.empty()) {
         for (const auto& prow : entry->perkRows) {
            std::string perks;
            bool met = !prow.perks.empty();
            for (const auto& clause : prow.perks) {
               bool any = false;
               std::string alts;
               for (const auto perkId : clause) {
                  const bool has = Effect::HasPerkId(player, perkId);
                  any = any || has;
                  const auto* perk = perkId ? RE::TESForm::LookupByID<RE::BGSPerk>(perkId) : nullptr;
                  const char* pn = perk ? perk->GetName() : nullptr;
                  alts += std::format("{}{:08X}{}{} ({})", alts.empty() ? "" : " or ", perkId, pn && *pn ? " " : "",
                     pn ? pn : "", has ? "has" : "lacks");
               }
               met = met && any;
               perks += (perks.empty() ? "" : " and ") + alts;
            }
            const auto msg = std::format("[cap]   perk row: {}={:.4g} needs {} -> {}", Core::Effect::Name(prow.col), prow.v,
               perks, met ? "counts" : "does not count");
            Print(msg.c_str());
            logger::info("{}"sv, msg);
         }
         const auto adjusted = Effect::PerkAdjustedCap(*entry, player);
         const auto msg = std::format("[cap]   with the player's perks: {}", Core::Effect::FormatCap(adjusted));
         Print(msg.c_str());
         logger::info("{}"sv, msg);
      }

      auto* bound = form->As<RE::TESBoundObject>();
      if (!player || !bound) return;
      const auto state = State::StateManager::GetSingleton().GetPlayerState();

      // Every stack of it the player carries (inventory entries are per stack).
      auto inventory = Util::GetInventorySafe(player, [bound](RE::TESBoundObject& o) { return &o == bound; });
      std::int32_t stacks = 0;
      for (auto& [obj, data] : inventory) {
         const auto& [count, entryData] = data;
         std::vector<RE::ExtraDataList*> lists;
         if (entryData && entryData->extraLists) {
            for (auto* xl : *entryData->extraLists) lists.push_back(xl);
         }
         if (lists.empty()) lists.push_back(nullptr);
         for (auto* xl : lists) {
            ++stacks;
            Effect::StackInfo stack;
            stack.count = xl ? xl->GetCount() : count;
            if (xl) {
               if (const auto* xc = xl->GetByType<RE::ExtraCharge>()) stack.charge = xc->charge;
            }
            if (const auto* ench = bound->As<RE::TESEnchantableForm>(); ench && ench->amountofEnchantment > 0) {
               stack.maxCharge = static_cast<float>(ench->amountofEnchantment);
            }
            const RE::ExtraEnchantment* xe = xl ? xl->GetByType<RE::ExtraEnchantment>() : nullptr;
            if (xe && xe->charge > 0) stack.maxCharge = static_cast<float>(xe->charge);
            if (stack.maxCharge > 0.0f && !(xl && xl->HasType<RE::ExtraCharge>())) stack.charge = stack.maxCharge;

            const auto instance = catalog.InstanceEntry(bound, xl);
            const auto& use = instance ? *instance : *entry;
            const auto f = Effect::ComputeRuntimeFeatures(use, state, stack);
            auto msg = std::format(
               "[cap]   stack {} x{}{}: overshoot_health={:.3f} overshoot_magicka={:.3f} overshoot_stamina={:.3f} "
               "weapon_charge={:.3f} stack_count={:.3f} ammo_matches_launcher={:.0f} school_fortified={:.0f}",
               stacks, stack.count, instance ? " (per-instance cap: " + Core::Effect::FormatCap(instance->cap) + ")" : std::string{},
               f.overshootHealth, f.overshootMagicka, f.overshootStamina, f.weaponCharge, f.stackCount,
               f.ammoMatchesLauncher, f.schoolFortified);
            Print(msg.c_str());
            logger::info("{}"sv, msg);
         }
      }
      if (stacks == 0) Print("[cap]   not carried: no runtime features");
   }

   // `hg dump races` -- every race in the LOAD ORDER with its keywords and how
   // many NPC records use it, plus the ActorType* keywords those NPCs carry
   // themselves. The input for widening the target-type need past vanilla's
   // six (LoreRim's creature mods add many more). Type and race are on screen,
   // so inside the perception line.
   //
   // Two readings per race, both from ActorTypeClassifier (the code the game
   // runs): huginnRace from the race alone, and huginnReading for a typical
   // actor of the race -- the race plus the keywords carried by more than half
   // of its NPC records, the race map's rule for per-actor families
   // (target_types.csv). tools/races/check_race_reading.py compares
   // huginnReading with docs/architecture/9-data/race_map.csv.
   static void Cmd_DumpRaces(std::string_view /*arg*/)
   {
      auto* dataHandler = RE::TESDataHandler::GetSingleton();
      if (!dataHandler) {
         Print("Data handler unavailable");
         return;
      }
      std::ofstream out;
      std::filesystem::path filePath;
      if (!OpenDumpFile("Huginn_Races.csv"sv, out, filePath)) return;

      struct Usage {
         size_t npcs = 0;
         size_t uniques = 0;
         std::vector<std::string> samples;
         std::map<std::string, size_t> npcKeywords;  // ActorType* on the NPC record
         size_t npcVampire = 0;                      // "Vampire" on the NPC record (classifier reads it)
      };
      std::unordered_map<const RE::TESRace*, Usage> usage;
      for (auto* npc : dataHandler->GetFormArray<RE::TESNPC>()) {
         if (!npc) continue;
         const auto* race = npc->GetRace();
         if (!race) continue;
         auto& u = usage[race];
         ++u.npcs;
         if (npc->IsUnique()) ++u.uniques;
         const char* name = npc->GetName();
         if (name && *name && u.samples.size() < 5 &&
             std::ranges::find(u.samples, std::string(name)) == u.samples.end()) {
            u.samples.emplace_back(name);
         }
         for (std::uint32_t i = 0; i < npc->numKeywords; ++i) {
            const auto* kw = npc->keywords[i];
            const char* id = kw ? kw->GetFormEditorID() : nullptr;
            if (id && std::string_view(id).starts_with("ActorType")) ++u.npcKeywords[id];
            if (id && std::string_view(id) == "Vampire"sv) ++u.npcVampire;
         }
      }

      out << "formID,plugin,winningPlugin,editorID,name,playable,child,flies,swims,"
             "keywords,npcCount,uniqueNpcCount,npcActorTypeKeywords,sampleNPCs,huginnRace,huginnReading\n";
      size_t written = 0, used = 0;
      for (auto* race : dataHandler->GetFormArray<RE::TESRace>()) {
         if (!race) continue;
         const auto it = usage.find(race);
         const Usage empty{};
         const auto& u = it != usage.end() ? it->second : empty;
         std::string npcKw;
         for (const auto& [kw, n] : u.npcKeywords) {
            if (!npcKw.empty()) npcKw += ';';
            npcKw += std::format("{}={}", kw, n);
         }
         std::string samples;
         for (const auto& s : u.samples) {
            if (!samples.empty()) samples += ';';
            samples += s;
         }
         const auto* lastFile = race->GetFile(-1);
         const char* edid = race->GetFormEditorID();
         const char* name = race->GetName();
         const auto flags = race->data.flags;

         // A typical actor of this race: the keywords on most of its NPC records.
         auto onMost = [&](std::string_view kw) {
            size_t n = 0;
            if (kw == "Vampire"sv) {
               n = u.npcVampire;
            } else if (const auto k = u.npcKeywords.find(std::string(kw)); k != u.npcKeywords.end()) {
               n = k->second;
            }
            return u.npcs > 0 && n * 2 > u.npcs;
         };
         auto raceHas = [&](std::string_view kw) { return race->HasKeywordString(kw); };
         const std::string_view edidView = edid ? std::string_view{ edid } : std::string_view{};
         const bool flies = flags.all(RE::RACE_DATA::Flag::kFlies);
         const auto readRace = State::ActorTypeClassifier::Classify(edidView, flies, raceHas,
            [](std::string_view) { return false; });
         const auto readTypical = State::ActorTypeClassifier::Classify(edidView, flies, raceHas, onMost);

         out << std::format("{:08X},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
            race->GetFormID(), CsvQuote(PluginOf(race)),
            CsvQuote(lastFile ? lastFile->GetFilename() : ""sv),
            CsvQuote(edid ? edid : ""), CsvQuote(name ? name : ""),
            flags.all(RE::RACE_DATA::Flag::kPlayable) ? 1 : 0,
            flags.all(RE::RACE_DATA::Flag::kChild) ? 1 : 0,
            flags.all(RE::RACE_DATA::Flag::kFlies) ? 1 : 0,
            flags.all(RE::RACE_DATA::Flag::kSwims) ? 1 : 0,
            CsvQuote(KeywordList(race)), u.npcs, u.uniques, CsvQuote(npcKw), CsvQuote(samples),
            State::GetTargetTypeName(readRace), State::GetTargetTypeName(readTypical));
         ++written;
         if (u.npcs > 0) ++used;
      }
      out.close();

      auto msg = std::format("Wrote {} races to Huginn_Races.csv - {} used by an NPC record", written, used);
      Print(msg.c_str());
      logger::info("[Console] {} -> {}"sv, msg, filePath.string());
   }
#endif  // !NDEBUG

   // =========================================================================
   // COMMAND TABLE + HELP
   // =========================================================================

   static const CommandEntry kCommands[] = {
      { "refresh",       "Force immediate recommendation update",       false, Cmd_Refresh },
      { "recs",          "Dump top-N recommendation breakdown to log",  true,  Cmd_Recs },
      { "needs",         "Print the live need vector (non-zero needs)", false, Cmd_Needs },
      { "actors",        "List the tracked actors (race, type, hostility, summon)", false, Cmd_Actors },
      { "hands",         "Show where each hand's weapon model hangs (invisible weapons)", false, Cmd_Hands },
      { "unlock",        "Clear all slot locks",                        false, Cmd_Unlock },
      { "status",        "Show system status",                          false, Cmd_Status },
      { "theta",         "Show the choice learner (drift), or one item's b and terms: hg theta [FormID]", true, Cmd_Theta },
      { "dump theta",    "Write every moved theta weight and every b to Huginn_Theta.csv", false, Cmd_DumpTheta },
      { "weights",       "Show the OLD learner's weights for FormID (kept for D2)", true,  Cmd_Weights },
      { "rebuild",       "Force rebuild all registries",                false, Cmd_Rebuild },
      { "reload",        "Hot-reload all settings from INI",            false, Cmd_Reload },
      { "page",          "Switch to page N (or show current)",          true,  Cmd_Page },
      { "dump all",      "Write every item and effect, with the effect catalog's view, to Huginn_All.csv", false, Cmd_DumpAll },
#ifndef NDEBUG
      { "dump weights",  "Write every OLD learner entry to Huginn_Weights.csv (debug builds)", false, Cmd_DumpWeights },
      { "cap",           "Show one item's effect columns and runtime cross-features (debug builds)", true, Cmd_Cap },
      { "dump races",    "Write every race, its keywords and NPC count to Huginn_Races.csv (debug builds)", false, Cmd_DumpRaces },
#endif
      { "reset weights", "Clear learned item weights",                  false, Cmd_ResetWeights },
      { "reset w",       "Clear learned item weights",                  false, Cmd_ResetWeights },
      { "reset all",     "Full system reset",                          false, Cmd_ResetAll },
   };

   static void Cmd_Help(std::string_view /*arg*/)
   {
      Print("Huginn console commands:");
      Print("  hg help             - Show this help message");
      for (const auto& cmd : kCommands) {
         if (cmd.name == "reset w") continue;  // Skip alias in help
         auto line = std::format("  hg {:<16s} - {}", cmd.name, cmd.helpText);
         Print(line.c_str());
      }
   }

   // =========================================================================
   // EXECUTE HANDLER
   // =========================================================================

   static bool Execute(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*,
      RE::TESObjectREFR*, RE::TESObjectREFR*, RE::Script* a_scriptObj, RE::ScriptLocals*,
      double&, std::uint32_t&)
   {
      // Parse from the raw command text (reliable, unlike chunk pointer arithmetic).
      // a_scriptObj->text contains the full line, e.g. "hg reset weights".
      // The 2-param registration ensures the console parser accepts multi-word input;
      // we do our own tokenization here for robustness.
      std::string subcmd;
      if (a_scriptObj && a_scriptObj->text) {
      subcmd = NormalizeCommand(a_scriptObj->text);
      }

      // Strip the command name prefix ("huginn ..." or "hg ...")
      if (subcmd.starts_with("huginn ")) {
      subcmd = subcmd.substr(7);
      } else if (subcmd.starts_with("hg ")) {
      subcmd = subcmd.substr(3);
      } else if (subcmd == "huginn" || subcmd == "hg") {
      subcmd.clear();
      }

      // The prefix strip can leave one leading space (e.g. "hg  reset" -> " reset").
      // The text is already lowercased + end-trimmed from the first NormalizeCommand,
      // so only a leading-whitespace trim is needed here — not a second full pass.
      if (auto lead = subcmd.find_first_not_of(" \t"); lead == std::string::npos) {
      subcmd.clear();
      } else if (lead > 0) {
      subcmd.erase(0, lead);
      }

      // Dispatch subcommands — table-driven lookup
      if (subcmd.empty() || subcmd == "help") {
      Cmd_Help("");
      } else {
      bool handled = false;
      for (const auto& cmd : kCommands) {
        if (subcmd == cmd.name) {
          cmd.execute("");
          handled = true;
          break;
        }
        if (cmd.takesArg) {
          auto nameLen = std::string_view(cmd.name).size();
          if (subcmd.size() > nameLen && subcmd[nameLen] == ' ' && subcmd.starts_with(cmd.name)) {
            cmd.execute(subcmd.substr(nameLen + 1));
            handled = true;
            break;
          }
        }
      }
      if (!handled) {
        if (subcmd == "reset") {
          Print("Usage: hg reset <weights|all>");
        } else {
          auto msg = std::format("Unknown command: '{}'. Type 'hg help' for available commands.", subcmd);
          Print(msg.c_str());
        }
      }
      }

      return true;
   }

   // =========================================================================
   // REGISTRATION
   // =========================================================================

   void Register()
   {
      // Find an unused console command to replace
      auto* cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand("ToggleDebugText");
      if (!cmd) {
      logger::error("[Console] Failed to locate ToggleDebugText command for replacement"sv);
      return;
      }

      // Two optional string params: "hg <command> <argument>"
      // e.g. "hg reset weights" → param1="reset", param2="weights"
      static RE::SCRIPT_PARAMETER params[] = {
      { "Command", RE::SCRIPT_PARAM_TYPE::kChar, true },
      { "Argument", RE::SCRIPT_PARAM_TYPE::kChar, true }
      };

      cmd->functionName = "Huginn";
      cmd->shortName = "hg";
      cmd->helpString = "Huginn commands. Type 'hg help' for usage.";
      cmd->referenceFunction = false;
      cmd->params = params;
      cmd->numParams = 2;
      cmd->executeFunction = Execute;
      cmd->editorFilter = false;

      logger::info("[Console] Registered 'Huginn' / 'hg' console command"sv);
   }
}
