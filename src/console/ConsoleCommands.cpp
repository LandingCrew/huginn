#include "ConsoleCommands.h"
#include "update/UpdateHandler.h"

#include "Globals.h"
#include "learning/FeatureBanditLearner.h"
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
#include "spell/SpellRegistry.h"
#include <fstream>
#include <unordered_set>
#include "context/ContextWeightSettings.h"
#include "context/ContextWeightConfig.h"
#include "ui/IntuitionMenu.h"
#include "ui/IntuitionSettings.h"
#include "wheeler/WheelerSettings.h"
#include "wheeler/WheelerClient.h"
#include "settings/SettingsReloader.h"
#include "pipeline/PipelineCoordinator.h"

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
      std::string_view name;      // "refresh", "reset weights", etc.
      std::string_view helpText;  // "Force immediate recommendation update"
      bool takesArg;              // true for "weights", "page"
      void (*execute)(std::string_view);  // raw fn pointer (zero overhead)
   };

   static void Cmd_ResetWeights(std::string_view /*arg*/)
   {
      // Shared with the dMenu "reset learning data" button; serializes itself
      // via RunExclusive and resets SlotLocker alongside the weight table.
      const auto learnerItems = Settings::SettingsReloader::ResetLearningData();
      if (!learnerItems) {
         Print("FeatureBanditLearner not initialized (load a game first)");
         return;
      }

      auto msg = std::format("Learning data cleared ({} learner items)", *learnerItems);
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
         // 1. Clear learning data (console-specific — init path restores from cosave)
         size_t learnerItems = 0;
         if (g_featureBanditLearner) {
            learnerItems = g_featureBanditLearner->GetItemCount();
            g_featureBanditLearner->Clear();
         }

         // 2. Rebuild all registries (console does full rebuild; init path reconciles)
         RebuildRegistries();

         // 3. Reset all stateful pipeline subsystems (shared with InitializeGameSystems)
         ResetPipelineSubsystems();

         auto msg = std::format("Full reset complete (Learner: {} items, all subsystems reset)",
            learnerItems);
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
      // Feature contextual bandit
      if (g_featureBanditLearner) {
      auto msg = std::format("Learner: {} items, {} total trains",
        g_featureBanditLearner->GetItemCount(), g_featureBanditLearner->GetTotalTrainCount());
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
      uint32_t trains = g_featureBanditLearner->GetTrainCount(formID);

      if (trains == 0) {
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
      auto header = std::format("{:08X} '{}' ({} trains, est={:.3f}, conf={:.2f}, ucb={:.2f}):",
         formID, name, trains, qNow, conf, ucb);
      Print(header.c_str());

      // Print each weight with its feature name (only non-negligible ones to console)
      for (size_t i = 0; i < Learning::StateFeatures::NUM_FEATURES; ++i) {
         auto line = std::format("  {:>12s}: {:+.4f}", kFeatureNames[i], weights[i]);
         Print(line.c_str());
         logger::debug("[Weights] {:08X} {} = {:.4f}", formID, kFeatureNames[i], weights[i]);
      }
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
   // SPELL CATALOGUE DUMP — DEBUG BUILDS ONLY, AND TEMPORARY
   // =========================================================================
   // THROWAWAY (0.20.40). Delete this function and its kCommands entry together
   // once the curated test-spell lists exist and the classifier gaps it found
   // are filed. It is a workbench tool, not a feature: it writes a 400 KB file
   // from a console command and nothing in the plugin reads it back.
   //
   // Debug-gated rather than shipped-and-undocumented, so a release build has no
   // command that writes half a megabyte to the player's SKSE folder.
   //
   // Writes every castable spell in the LOAD ORDER to a CSV, with Huginn's own
   // classification beside each one.
   //
   // Two uses. Building test characters: pick FormIDs out of the CSV and feed
   // them to `player.addspell` in a console batch file, instead of guessing at
   // IDs that may not exist in this load order. And checking the classifier
   // against a big modlist: sort by huginnType and the Unknowns are the spells
   // no rule caught, which is the list worth fixing.
   //
   // Spells only -- abilities, diseases, enchantments and the rest of the SPEL
   // record's other uses are not things a player can be given. Powers and lesser
   // powers are kept, marked as such, since they ARE grantable.
#ifndef NDEBUG
   static void Cmd_DumpSpells(std::string_view /*arg*/)
   {
      auto* dataHandler = RE::TESDataHandler::GetSingleton();
      if (!dataHandler) {
         Print("Data handler unavailable");
         return;
      }
      if (!g_spellRegistry) {
         Print("Spell registry not initialized - load a game first");
         return;
      }

      const auto logDir = SKSE::log::log_directory();
      if (!logDir) {
         Print("No SKSE log directory - cannot write the dump");
         return;
      }
      const auto filePath = *logDir / "Huginn_Spells.csv";

      std::ofstream out(filePath, std::ios::trunc);
      if (!out) {
         Print("Could not open Huginn_Spells.csv for writing");
         logger::error("[Console] Failed to open {} for writing"sv, filePath.string());
         return;
      }

      // Quote every name: spell names carry commas often enough, and a quote
      // occasionally ("Conjure Dremora Lord \"Grand\"" exists in some mods).
      auto csvQuote = [](std::string_view text) {
         std::string quoted;
         quoted.reserve(text.size() + 2);
         quoted += '"';
         for (const char c : text) {
            if (c == '"') quoted += '"';  // doubled, per RFC 4180
            quoted += c;
         }
         quoted += '"';
         return quoted;
      };

      auto castTypeName = [](RE::MagicSystem::SpellType type) -> std::string_view {
         switch (type) {
         case RE::MagicSystem::SpellType::kSpell:        return "Spell"sv;
         case RE::MagicSystem::SpellType::kPower:        return "Power"sv;
         case RE::MagicSystem::SpellType::kLesserPower:  return "LesserPower"sv;
         default:                                        return "Other"sv;
         }
      };

      const auto& classifier = g_spellRegistry->GetClassifier();
      auto* player = RE::PlayerCharacter::GetSingleton();

      // Which spells can a player actually LEARN? A spell tome teaching it is
      // the only honest answer available here, and it is the column that makes
      // this dump worth reading: a load order this size is mostly NPC and
      // creature spells that happen to be kSpell, so an unclassified count over
      // the whole array says nothing about what a player would ever be offered.
      std::unordered_set<RE::FormID> taughtByTome;
      for (auto* book : dataHandler->GetFormArray<RE::TESObjectBOOK>()) {
         if (!book || !book->TeachesSpell()) {
            continue;
         }
         if (auto* taught = book->GetSpell()) {
            taughtByTome.insert(taught->GetFormID());
         }
      }

      out << "formID,name,castType,huginnType,school,element,tags,tagsExt,"
             "cost,concentration,range,known,tome,hostile,detrimental,recover,archetype,primaryAV,secondaryAV,delivery,castingType,effects,retry,assocForm,assocKind\n";

      size_t written = 0;
      size_t skipped = 0;
      size_t unknownType = 0;
      size_t unknownLearnable = 0;
      size_t learnableCount = 0;
      for (auto* spell : dataHandler->GetFormArray<RE::SpellItem>()) {
         if (!spell) {
            continue;
         }
         const auto castType = spell->GetSpellType();
         if (castType != RE::MagicSystem::SpellType::kSpell &&
             castType != RE::MagicSystem::SpellType::kPower &&
             castType != RE::MagicSystem::SpellType::kLesserPower) {
            ++skipped;
            continue;
         }
         const char* rawName = spell->GetName();
         if (!rawName || !*rawName) {
            ++skipped;  // unnamed: a template or a scripted internal, not castable content
            continue;
         }

         const auto data = classifier.ClassifySpell(spell);
         const bool learnable = taughtByTome.contains(spell->GetFormID());

         // The inputs DetermineSpellType actually reads, dumped raw. Names would
         // need a 50-case switch in a throwaway command; the numbers map back to
         // the CommonLibSSE enums offline, and grouping by them is the point --
         // "these 40 spells are archetype 27" is a rule, "Ash Rune, Bend Time,
         // Burden" is a list.
         int archetype = -1, primaryAV = -1, secondaryAV = -1, effectCount = 0;
         bool hostile = false, detrimental = false, recover = false;

         // Report the effect the CLASSIFIER used, not merely the costliest one.
         //
         // ClassifySpell retries with the costliest non-script effect when the
         // costliest is a script, so for those ~66 spells the dump used to print
         // archetype=1 beside a type derived from a different effect entirely --
         // and "group by archetype to find the rule" then mis-attributed that
         // whole cohort. This command exists to drive exactly that analysis.
         // Ask the classifier rather than restating its condition; the two
         // disagreed, and the dump was the one that was wrong.
         const int usedRetry = classifier.RetryDecidedType(spell) ? 1 : 0;

         // A cloak and a hazard both delegate their behaviour to another form,
         // and the classifier follows that link to type them. When its answer
         // looks wrong -- Guardian Circle as a Buff, Holy Fire as a Debuff --
         // the first question is whether the link points where the plugin
         // records say it does. Dump it rather than infer it.
         RE::FormID assocForm = 0;
         std::string_view assocKind = "-"sv;

         auto* chosen = classifier.GetCostliestEffect(spell);
         if (chosen && chosen->baseEffect &&
             chosen->baseEffect->GetArchetype() == RE::EffectSetting::Archetype::kScript) {
            if (auto* readable = classifier.GetCostliestNonScriptEffect(spell)) {
               chosen = readable;
            }
         }
         if (chosen) {
            if (auto* setting = chosen->baseEffect) {
               archetype = static_cast<int>(setting->GetArchetype());
               primaryAV = static_cast<int>(setting->data.primaryAV);
               secondaryAV = static_cast<int>(setting->data.secondaryAV);
               hostile = setting->data.flags.any(
                  RE::EffectSetting::EffectSettingData::Flag::kHostile);
               detrimental = setting->data.flags.any(
                  RE::EffectSetting::EffectSettingData::Flag::kDetrimental);
               recover = setting->data.flags.any(
                  RE::EffectSetting::EffectSettingData::Flag::kRecover);

               if (auto* linked = setting->data.associatedForm) {
                  assocForm = linked->GetFormID();
                  if (linked->As<RE::SpellItem>())        assocKind = "SPEL"sv;
                  else if (linked->As<RE::BGSHazard>())   assocKind = "HAZD"sv;
                  else                                    assocKind = "other"sv;
               }
            }
         }
         effectCount = static_cast<int>(spell->effects.size());
         if (data.type == Spell::SpellType::Unknown) {
            ++unknownType;
            if (learnable) {
               ++unknownLearnable;
            }
         }

         out << std::format("{:08X},{},{},{},{},{},{:08X},{:04X},{},{},{:.0f},{},{},{},{},{},{},{},{},{},{},{},{},{:08X},{}\n",
            spell->GetFormID(),
            csvQuote(rawName),
            castTypeName(castType),
            Spell::SpellTypeToString(data.type),
            Spell::MagicSchoolToString(data.school),
            Spell::ElementTypeToString(data.element),
            static_cast<uint32_t>(data.tags),
            static_cast<uint16_t>(data.tagsExt),
            data.baseCost,
            data.isConcentration ? 1 : 0,
            data.range,
            (player && player->HasSpell(spell)) ? 1 : 0,
            learnable ? 1 : 0,
            hostile ? 1 : 0,
            detrimental ? 1 : 0,
            recover ? 1 : 0,
            archetype,
            primaryAV,
            secondaryAV,
            static_cast<int>(spell->GetDelivery()),
            static_cast<int>(spell->GetCastingType()),
            effectCount,
            usedRetry,
            assocForm,
            assocKind);
         ++written;
         if (learnable) {
            ++learnableCount;
         }
      }
      out.close();

      // This pass touched every spell in the load order, so it is the only
      // reconciliation that can honestly say an override matched nothing.
      classifier.ReportOverrideUsage("after dump (whole load order)"sv);

      auto msg = std::format(
         "Wrote {} spells to Huginn_Spells.csv - {} learnable from tomes, {} of those unclassified "
         "({} unclassified overall, {} non-spell forms skipped)",
         written, learnableCount, unknownLearnable, unknownType, skipped);
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
      { "unlock",        "Clear all slot locks",                        false, Cmd_Unlock },
      { "status",        "Show system status",                          false, Cmd_Status },
      { "weights",       "Show learner weight vector for FormID",          true,  Cmd_Weights },
      { "rebuild",       "Force rebuild all registries",                false, Cmd_Rebuild },
      { "reload",        "Hot-reload all settings from INI",            false, Cmd_Reload },
      { "page",          "Switch to page N (or show current)",          true,  Cmd_Page },
#ifndef NDEBUG
      { "dump spells",   "Write every castable spell to Huginn_Spells.csv (debug builds)", false, Cmd_DumpSpells },
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
