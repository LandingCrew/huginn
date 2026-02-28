#include "ConsoleCommands.h"
#include "UpdateLoop.h"

#include "Globals.h"
#include "learning/FeatureQLearner.h"
#include "learning/StateFeatures.h"
#include "learning/PipelineStateCache.h"
#include "state/StateManager.h"
#include "candidate/CandidateGenerator.h"
#include "override/OverrideManager.h"
#include "override/OverrideConfig.h"
#include "slot/SlotAllocator.h"
#include "slot/SlotLocker.h"
#include "slot/SlotSettings.h"
#include "learning/ScorerSettings.h"
#include "learning/LearningSettings.h"
#include "learning/ExternalEquipLearner.h"
#include "state/ContextWeightSettings.h"
#include "state/ContextWeightConfig.h"
#include "ui/IntuitionMenu.h"
#include "ui/IntuitionSettings.h"
#include "wheeler/WheelerSettings.h"
#include "wheeler/WheelerClient.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>

namespace Huginn::Console
{
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
      std::string_view name;      // "refresh", "reset qvalues", etc.
      std::string_view helpText;  // "Force immediate recommendation update"
      bool takesArg;              // true for "weights", "page"
      void (*execute)(std::string_view);  // raw fn pointer (zero overhead)
   };

   static void Cmd_ResetQValues(std::string_view /*arg*/)
   {
      if (!g_featureQLearner) {
      Print("FeatureQLearner not initialized (load a game first)");
      return;
      }

      size_t fqlItems = g_featureQLearner->GetItemCount();
      g_featureQLearner->Clear();

      // Unlock all slots so the next scoring cycle can reassign immediately
      Slot::SlotLocker::GetSingleton().Reset();

      auto msg = std::format("Learning data cleared ({} FQL items)", fqlItems);
      Print(msg.c_str());
      logger::info("[Console] {}"sv, msg);
   }

   static void Cmd_ResetAll(std::string_view /*arg*/)
   {
      // 1. Clear learning data (console-specific — init path restores from cosave)
      size_t fqlItems = 0;
      if (g_featureQLearner) {
      fqlItems = g_featureQLearner->GetItemCount();
      g_featureQLearner->Clear();
      }

      // 2. Rebuild all registries (console does full rebuild; init path reconciles)
      if (g_spellRegistry) g_spellRegistry->RebuildRegistry();
      if (g_itemRegistry) g_itemRegistry->RebuildRegistry();
      if (g_weaponRegistry) g_weaponRegistry->RebuildRegistry();
      if (g_scrollRegistry) g_scrollRegistry->RebuildRegistry();

      // 3. Reset all stateful pipeline subsystems (shared with InitializeGameSystems)
      ResetPipelineSubsystems();

      auto msg = std::format("Full reset complete (FQL: {} items, all subsystems reset)",
      fqlItems);
      Print(msg.c_str());
      logger::info("[Console] {}"sv, msg);
   }

   static void Cmd_Refresh(std::string_view /*arg*/)
   {
      // Unlock all slots so the re-score can freely reassign
      Slot::SlotLocker::GetSingleton().Reset();

      // Run one full update cycle immediately (deltaSeconds=0 since it's instant)
      OnUpdate(0.0f);

      Print("Recommendations refreshed");
      logger::info("[Console] Forced recommendation refresh"sv);
   }

   static void Cmd_Unlock(std::string_view /*arg*/)
   {
      auto& slotLocker = Slot::SlotLocker::GetSingleton();
      auto& slotAllocator = Slot::SlotAllocator::GetSingleton();
      const size_t slotCount = slotAllocator.GetSlotCount();
      size_t lockedCount = 0;
      for (size_t i = 0; i < slotCount; ++i) {
      if (slotLocker.IsSlotLocked(i)) {
        ++lockedCount;
      }
      }
      slotLocker.Reset();

      auto msg = std::format("All slot locks cleared ({} were active)", lockedCount);
      Print(msg.c_str());
      logger::info("[Console] {}"sv, msg);
   }

   static void Cmd_Status(std::string_view /*arg*/)
   {
      // Feature Q-learning
      if (g_featureQLearner) {
      auto msg = std::format("FQL: {} items, {} total trains",
        g_featureQLearner->GetItemCount(), g_featureQLearner->GetTotalTrainCount());
      Print(msg.c_str());
      }

      // Registries
      auto regMsg = std::format("Registries: {} spells, {} items, {} weapons, {} scrolls",
      g_spellRegistry ? g_spellRegistry->GetSpellCount() : 0,
      g_itemRegistry ? g_itemRegistry->GetItemCount() : 0,
      g_weaponRegistry ? g_weaponRegistry->GetWeaponCount() : 0,
      g_scrollRegistry ? g_scrollRegistry->GetScrollCount() : 0);
      Print(regMsg.c_str());

      // Pages (query before slot locks so we know the actual count)
      auto& slotAllocator = Slot::SlotAllocator::GetSingleton();

      // Slot locks
      auto& slotLocker = Slot::SlotLocker::GetSingleton();
      const size_t slotCount = slotAllocator.GetSlotCount();
      size_t lockedCount = 0;
      for (size_t i = 0; i < slotCount; ++i) {
      if (slotLocker.IsSlotLocked(i)) {
        ++lockedCount;
      }
      }
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
      if (!g_featureQLearner) {
         Print("FeatureQLearner not initialized (load a game first)");
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

      auto weights = g_featureQLearner->GetWeights(formID);
      uint32_t trains = g_featureQLearner->GetTrainCount(formID);

      if (trains == 0) {
         auto msg = std::format("{:08X} '{}': no training data", formID, name);
         Print(msg.c_str());
         return;
      }

      // Compute current Q-value using live state features
      auto& stateMgr = State::StateManager::GetSingleton();
      auto features = Learning::StateFeatures::FromState(
         stateMgr.GetPlayerState(), stateMgr.GetTargets());
      float qNow = g_featureQLearner->GetQValue(formID, features);
      float conf = g_featureQLearner->GetConfidence(formID);
      float ucb = g_featureQLearner->GetUCB(formID);

      // Header
      auto header = std::format("{:08X} '{}' ({} trains, Q={:.3f}, conf={:.2f}, ucb={:.2f}):",
         formID, name, trains, qNow, conf, ucb);
      Print(header.c_str());

      // Print each weight with its feature name (only non-negligible ones to console)
      for (size_t i = 0; i < Learning::StateFeatures::NUM_FEATURES; ++i) {
         auto line = std::format("  {:>12s}: {:+.4f}", kFeatureNames[i], weights[i]);
         Print(line.c_str());
         logger::info("[Weights] {:08X} {} = {:.4f}", formID, kFeatureNames[i], weights[i]);
      }
   }

   static void Cmd_Rebuild(std::string_view /*arg*/)
   {
      size_t spells = 0, items = 0, weapons = 0, scrolls = 0;

      if (g_spellRegistry) {
      g_spellRegistry->RebuildRegistry();
      spells = g_spellRegistry->GetSpellCount();
      }
      if (g_itemRegistry) {
      g_itemRegistry->RebuildRegistry();
      items = g_itemRegistry->GetItemCount();
      }
      if (g_weaponRegistry) {
      g_weaponRegistry->RebuildRegistry();
      weapons = g_weaponRegistry->GetWeaponCount();
      }
      if (g_scrollRegistry) {
      g_scrollRegistry->RebuildRegistry();
      scrolls = g_scrollRegistry->GetScrollCount();
      }

      auto msg = std::format("Registries rebuilt ({} spells, {} items, {} weapons, {} scrolls)",
      spells, items, weapons, scrolls);
      Print(msg.c_str());
      logger::info("[Console] {}"sv, msg);
   }

   static void Cmd_Reload(std::string_view /*arg*/)
   {
      const auto iniPath = std::filesystem::path("Data/SKSE/Plugins/Huginn.ini");

      // =====================================================================
      // Phase 1: Reload all settings from INI
      // =====================================================================

      // 1. SlotSettings first (page count may change, affects allocator + wheels)
      Slot::SlotSettings::GetSingleton().LoadFromFile(iniPath);
      Print("  [SlotSettings] reloaded");

      // 2. Scorer settings
      auto& scorerSettings = Scoring::ScorerSettings::GetSingleton();
      scorerSettings.LoadFromFile(iniPath);
      Print("  [ScorerSettings] reloaded");

      // 3. Context weight settings
      State::ContextWeightSettings::GetSingleton().LoadFromFile(iniPath);
      Print("  [ContextWeights] reloaded");

      // 4. Override settings
      Override::Settings::GetSingleton().LoadFromFile(iniPath);
      Print("  [Overrides] reloaded");

      // 4b. Learning settings
      Learning::LearningSettings::GetSingleton().LoadFromFile(iniPath);
      Print("  [Learning] reloaded");

      // 5. Wheeler settings (before wheel rebuild)
      Wheeler::WheelerSettings::GetSingleton().LoadFromFile(iniPath);
      Print("  [Wheeler] reloaded");

      // 6. Candidate config
      LoadCandidateConfigFromINI();
      Print("  [Candidates] reloaded");

      // =====================================================================
      // Phase 2: Apply side effects
      // =====================================================================
      // NOTE: Phase 1 settings (ScorerSettings, ContextWeightSettings, etc.)
      // are POD float/bool singletons. The update thread reads them without
      // locks — worst case is one frame with mixed old/new values, acceptable
      // for tuning. SlotSettings has its own shared_mutex (safe). The
      // remaining side effects below are ordered to avoid inconsistency.

      // 1. Apply scorer config + context weight config + wildcard config
      if (g_utilityScorer) {
      g_utilityScorer->SetConfig(scorerSettings.BuildConfig());
      g_utilityScorer->SetContextWeightConfig(State::ContextWeightSettings::GetSingleton().BuildConfig());
      LoadWildcardConfigFromINI(g_utilityScorer->GetWildcardManager());
      }

      // 1b. Apply learning config to ExternalEquipLearner
      Learning::ExternalEquipLearner::GetSingleton().SetConfig(
      Learning::LearningSettings::GetSingleton().BuildConfig());

      // 2. Re-initialize slot allocator FIRST (re-reads SlotSettings for new page count)
      Slot::SlotAllocator::GetSingleton().Initialize();

      // 3. Reset slot locker AFTER allocator (so it operates on correct slot count)
      auto& slotLocker = Slot::SlotLocker::GetSingleton();
      slotLocker.Reset();
      slotLocker.SetConfig(LoadSlotLockerConfigFromINI());

      // 4. Rebuild Wheeler wheels (if connected)
      auto& wheelerClient = Wheeler::WheelerClient::GetSingleton();
      if (wheelerClient.IsConnected()) {
      wheelerClient.DestroyRecommendationWheels();
      wheelerClient.CreateRecommendationWheels();
      Print("  [Wheeler] wheels rebuilt");
      }

      // 5. Reapply Intuition widget settings (position, alpha, scale)
      // IntuitionSettings loads from main INI (console reload doesn't have dMenu path)
      UI::IntuitionSettings::GetSingleton().LoadFromFile(iniPath);
      auto* menu = UI::IntuitionMenu::GetSingleton();
      if (menu) {
      menu->ReapplySettings(UI::IntuitionSettings::GetSingleton().BuildConfig());
      }
      Print("  [Widget] reloaded");

      // The next update tick (~100ms) will pick up all new settings.
      // SlotLocker::Reset() ensures the first tick after reload can freely
      // reassign all slots, so the player sees changes almost immediately.

      Print("All settings reloaded from Huginn.ini");
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
   // COMMAND TABLE + HELP
   // =========================================================================

   static const CommandEntry kCommands[] = {
      { "refresh",       "Force immediate recommendation update",       false, Cmd_Refresh },
      { "unlock",        "Clear all slot locks",                        false, Cmd_Unlock },
      { "status",        "Show system status",                          false, Cmd_Status },
      { "weights",       "Show FQL weight vector for FormID",           true,  Cmd_Weights },
      { "rebuild",       "Force rebuild all registries",                false, Cmd_Rebuild },
      { "reload",        "Hot-reload all settings from INI",            false, Cmd_Reload },
      { "page",          "Switch to page N (or show current)",          true,  Cmd_Page },
      { "reset qvalues", "Clear Q-learning tables",                    false, Cmd_ResetQValues },
      { "reset q",       "Clear Q-learning tables",                    false, Cmd_ResetQValues },
      { "reset all",     "Full system reset",                          false, Cmd_ResetAll },
   };

   static void Cmd_Help(std::string_view /*arg*/)
   {
      Print("Huginn console commands:");
      Print("  hg help             - Show this help message");
      for (const auto& cmd : kCommands) {
         if (cmd.name == "reset q") continue;  // Skip alias in help
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
      // a_scriptObj->text contains the full line, e.g. "hg reset qvalues".
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

      // Trim again after stripping prefix
      subcmd = NormalizeCommand(subcmd);

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
          auto prefix = std::string(cmd.name) + " ";
          if (subcmd.starts_with(prefix)) {
            cmd.execute(subcmd.substr(prefix.size()));
            handled = true;
            break;
          }
        }
      }
      if (!handled) {
        if (subcmd == "reset") {
          Print("Usage: hg reset <qvalues|all>");
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
      // e.g. "hg reset qvalues" → param1="reset", param2="qvalues"
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
