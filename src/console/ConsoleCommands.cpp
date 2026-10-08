#include "ConsoleCommands.h"
#include "update/UpdateHandler.h"

#include "Globals.h"
#include "learning/FeatureBanditLearner.h"
#include "learning/StateFeatures.h"
#include "learning/PipelineStateCache.h"
#include "state/StateManager.h"
#include "core/ActorTypeClassifier.h"
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
#include "learning/item/ItemClassifier.h"
#include "scroll/ScrollClassifier.h"
#include "apparel/ApparelClassifier.h"
#include "weapon/WeaponClassifier.h"
#include "util/InventoryUtil.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
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
         // 1. Clear learning data. A reload of the same character keeps this
         //    cleared state (Clear ticks the learning clock past any older
         //    save); a different character or a later save restores from it.
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
      auto msg = std::format("Learner: {} items, {} total trains, {} forgotten, {:.1f}h of play",
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
            // A newline inside a quoted field is legal RFC 4180 and illegal to
            // most of what actually reads these files -- awk, sort, Select-String
            // all count lines, and one multi-line description would silently
            // shift every count in an audit. Authored descriptions do contain
            // them. Fold to a space.
            if (c == '\r' || c == '\n') {
               quoted += ' ';
               continue;
            }
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
         case RE::MagicSystem::SpellType::kScroll:       return "Scroll"sv;
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
             "cost,concentration,range,known,tome,hostile,detrimental,recover,archetype,primaryAV,secondaryAV,delivery,castingType,effects,retry,assocForm,assocKind,assocSpell,evidence,description,resistedElement\n";

      size_t written = 0;
      size_t skipped = 0;
      size_t unknownType = 0;
      size_t unknownLearnable = 0;
      size_t learnableCount = 0;
      // Both form arrays. GetFormArray keys on T::FORMTYPE, and ScrollItem's is
      // FormType::Scroll where SpellItem's is FormType::Spell -- so widening
      // the cast-type filter above was necessary and not sufficient, and the
      // first dump after it came back byte-identical. ScrollItem IS-A
      // SpellItem, so the pointers go in the same list and the loop body does
      // not care which array they came from.
      std::vector<RE::SpellItem*> toDump;
      const auto& spellForms = dataHandler->GetFormArray<RE::SpellItem>();
      const auto& scrollForms = dataHandler->GetFormArray<RE::ScrollItem>();
      toDump.reserve(spellForms.size() + scrollForms.size());
      for (auto* form : spellForms) toDump.push_back(form);
      for (auto* form : scrollForms) toDump.push_back(form);

      for (auto* spell : toDump) {
         if (!spell) {
            continue;
         }
         // Scrolls too, since v0.21.7. ScrollItem IS-A SpellItem and
         // ScrollClassifier delegates straight to SpellClassifier, so a scroll
         // is classified by exactly these rules -- and LoreRim ships throwing
         // knives as scrolls, which come out Debuff while their own tooltip
         // says "deals 24 physical damage". There was no way to ask which rule
         // did that: this command filtered them out, and the scroll registry
         // dump prints the answer without the inputs.
         const auto castType = spell->GetSpellType();
         if (castType != RE::MagicSystem::SpellType::kSpell &&
             castType != RE::MagicSystem::SpellType::kPower &&
             castType != RE::MagicSystem::SpellType::kLesserPower &&
             castType != RE::MagicSystem::SpellType::kScroll) {
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
         //
         // And only when an EFFECT decided the type at all. RetryDecidedType
         // and the chosen-effect block below both reproduce the
         // DetermineSpellType path and neither knows about the layers above it,
         // so a spell typed from the override file, from tags or from its name
         // still printed a full archetype/AV/flag row describing an effect that
         // decided nothing -- and "group by archetype to find the rule" then
         // mis-attributes that cohort. Which is the exact failure this column
         // was added to fix, one layer up.
         const bool typedFromEffect =
            data.typeEvidence == Spell::TypeEvidence::Archetype ||
            data.typeEvidence == Spell::TypeEvidence::Applied ||
            data.typeEvidence == Spell::TypeEvidence::SchoolOnly ||
            data.typeEvidence == Spell::TypeEvidence::SchoolGuess;

         const int usedRetry =
            (typedFromEffect && classifier.RetryDecidedType(spell)) ? 1 : 0;

         // A cloak and a hazard both delegate their behaviour to another form,
         // and the classifier follows that link to type them. When its answer
         // looks wrong -- Guardian Circle as a Buff, Holy Fire as a Debuff --
         // the first question is whether the link points where the plugin
         // records say it does. Dump it rather than infer it.
         RE::FormID assocForm = 0;
         RE::FormID assocSpell = 0;
         std::string_view assocKind = "-"sv;

         auto* chosen = typedFromEffect ? classifier.GetCostliestEffect(spell) : nullptr;
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
                  if (linked->As<RE::SpellItem>()) {
                     assocKind = "SPEL"sv;
                  } else if (auto* hazard = linked->As<RE::BGSHazard>()) {
                     // The chain is effect -> hazard -> spell, and the peek reads
                     // the far end. Guardian Circle and Supernova both reach a
                     // real hazard and still come back Unknown, so the open
                     // question is whether that last link is set at all.
                     assocKind = "HAZD"sv;
                     if (hazard->data.spell) {
                        assocSpell = hazard->data.spell->GetFormID();
                     }
                  } else {
                     assocKind = "other"sv;
                  }
               }
            }
         }
         effectCount = static_cast<int>(spell->effects.size());
         // The authored text, from EVERY effect, joined in record order.
         //
         // This is the only place a spell says what it DOES in words a player
         // reads, and on the evidence it outranks the effect data. Guardian
         // Circle and Circle of Protection are identical in every field this
         // classifier reads -- same archetype, same flags, same actor value,
         // both reaching a kTurnUndead spell through a hazard -- and only the
         // text says one of them also heals 20 health per second. Circle of
         // Death reads as a non-hostile health value modifier, which is the
         // single most reliable rule in the classifier, and its text says it
         // instantly kills.
         //
         // Dumped as a CORPUS, not read by the classifier. A rule that can
         // overturn effect data across 1,107 spells gets written from the
         // strings that are actually there, and measured, before it is written
         // from memory.
         std::string description;
         for (const auto* effect : spell->effects) {
            if (!effect || !effect->baseEffect) continue;
            const char* text = effect->baseEffect->magicItemDescription.c_str();
            if (!text || !*text) continue;
            if (!description.empty()) description += " | ";
            description += text;
         }

         if (data.type == Spell::SpellType::Unknown) {
            ++unknownType;
            if (learnable) {
               ++unknownLearnable;
            }
         }

         out << std::format("{:08X},{},{},{},{},{},{:08X},{:04X},{},{},{:.0f},{},{},{},{},{},{},{},{},{},{},{},{},{:08X},{},{:08X},{},{},{}\n",
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
            assocKind,
            assocSpell,
            csvQuote(Spell::TypeEvidenceToString(data.typeEvidence)),
            csvQuote(description),
            // What the spell protects against, by its resist actor value --
            // the only source a Defensive spell's element may come from. Beside
            // `element`, it shows which spells lost a name-derived element.
            Spell::ElementTypeToString(Spell::SpellClassifier::ResistedElement(spell)));
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

   // `hg dump food` -- every food and drink in the LOAD ORDER to a CSV, with
   // the survival tags Huginn gives it and every effect's raw inputs beside
   // them: display name, editor ID (empty without po3 Tweaks), archetype,
   // actor values and keywords.
   //
   // Survival tagging has to work across load orders that mark hunger and
   // warmth three different ways -- LoreRim on effect keywords, vanilla CC by
   // an unkeyworded effect, mods by their own conventions -- and a registry
   // dump only shows what one character carries (2026-09-29).
   //
   // A fresh classifier, not the registry's: the dump must not touch the
   // registry's cache, and overrides are beside the point here. Classifying
   // logs a debug line per effect, so expect the log to grow by a few
   // thousand lines.
   static void Cmd_DumpFood(std::string_view /*arg*/)
   {
      auto* dataHandler = RE::TESDataHandler::GetSingleton();
      if (!dataHandler) {
         Print("Data handler unavailable");
         return;
      }
      const auto logDir = SKSE::log::log_directory();
      if (!logDir) {
         Print("No SKSE log directory - cannot write the dump");
         return;
      }
      const auto filePath = *logDir / "Huginn_Food.csv";
      std::ofstream out(filePath, std::ios::trunc);
      if (!out) {
         Print("Could not open Huginn_Food.csv for writing");
         logger::error("[Console] Failed to open {} for writing"sv, filePath.string());
         return;
      }

      // Same quoting rules as the spell dump.
      auto csvQuote = [](std::string_view text) {
         std::string quoted;
         quoted.reserve(text.size() + 2);
         quoted += '"';
         for (const char c : text) {
            if (c == '\r' || c == '\n') { quoted += ' '; continue; }
            if (c == '"') quoted += '"';
            quoted += c;
         }
         quoted += '"';
         return quoted;
      };

      Item::ItemClassifier classifier;
      out << "formID,plugin,name,huginnType,hunger,cold,tags,effects\n";

      size_t written = 0, hunger = 0, cold = 0;
      for (auto* item : dataHandler->GetFormArray<RE::AlchemyItem>()) {
         if (!item || !item->IsFood()) continue;
         const char* rawName = item->GetName();
         if (!rawName || !*rawName) continue;

         const auto data = classifier.ClassifyItem(item);
         const bool isHunger = Item::HasTag(data.tags, Item::ItemTag::SatisfiesHunger);
         const bool isCold = Item::HasTag(data.tags, Item::ItemTag::SatisfiesCold);

         // One field per item: effects joined by " / ", each as
         // 'name'{edid} arch/pAV [kw;kw].
         std::string effects;
         for (const auto* effect : item->effects) {
            if (!effect || !effect->baseEffect) continue;
            const auto* base = effect->baseEffect;
            std::string keywords;
            for (uint32_t k = 0; k < base->GetNumKeywords(); ++k) {
               if (auto kw = base->GetKeywordAt(k); kw && *kw) {
                  if (!keywords.empty()) keywords += ';';
                  keywords += (*kw)->GetFormEditorID();
               }
            }
            if (!effects.empty()) effects += " / ";
            const char* full = base->GetFullName();
            effects += std::format("'{}'{{{}}} {}/{} [{}]",
               full ? full : "", base->GetFormEditorID(),
               static_cast<int>(base->GetArchetype()),
               static_cast<int>(base->data.primaryAV), keywords);
         }

         const auto* file = item->GetFile(0);
         out << std::format("{:08X},{},{},{},{},{},{:08X},{}\n",
            item->GetFormID(),
            csvQuote(file ? file->GetFilename() : ""sv),
            csvQuote(rawName),
            Item::ItemTypeToString(data.type),
            isHunger ? 1 : 0,
            isCold ? 1 : 0,
            static_cast<uint32_t>(data.tags),
            csvQuote(effects));
         ++written;
         if (isHunger) ++hunger;
         if (isCold) ++cold;
      }
      out.close();

      auto msg = std::format("Wrote {} foods/drinks to Huginn_Food.csv - {} satisfy hunger, {} satisfy cold",
         written, hunger, cold);
      Print(msg.c_str());
      logger::info("[Console] {} -> {}"sv, msg, filePath.string());
   }

   // Shared by the three dumps below: open <SKSE log dir>/<fileName>, or say
   // why not on the console and return false.
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

   // Same quoting rules as the spell dump: always quoted, quotes doubled,
   // newlines folded to a space so line-counting tools stay honest.
   static std::string CsvQuote(std::string_view text)
   {
      std::string quoted;
      quoted.reserve(text.size() + 2);
      quoted += '"';
      for (const char c : text) {
         if (c == '\r' || c == '\n') { quoted += ' '; continue; }
         if (c == '"') quoted += '"';
         quoted += c;
      }
      quoted += '"';
      return quoted;
   }

   static std::string_view PluginOf(const RE::TESForm* form)
   {
      const auto* file = form ? form->GetFile(0) : nullptr;
      return file ? file->GetFilename() : ""sv;
   }

   // The one sub-classification an item carries, whichever field its type
   // fills: element for resists, school/skill for fortifies. "-" if none.
   static std::string_view ItemSubclass(const Item::ItemData& data)
   {
      if (data.element != Item::ElementType::None)      return Item::ElementTypeToString(data.element);
      if (data.school != Item::MagicSchool::None)       return Item::MagicSchoolToString(data.school);
      if (data.combatSkill != Item::CombatSkill::None)  return Item::CombatSkillToString(data.combatSkill);
      if (data.utilitySkill != Item::UtilitySkill::None) return Item::UtilitySkillToString(data.utilitySkill);
      return "-"sv;
   }

   // `hg dump diseases` -- every disease, plus every other spell with an
   // effect resisted by Resist Disease, one row each: whether Huginn's
   // isDiseased check can see it, and whether it is on the player now.
   //
   // Why (2026-10-03): Huginn flagged no disease on LoreRim in two sessions.
   // StateManager_MagicEffects sets isDiseased only for a detrimental effect
   // resisted by Resist Disease that reaches the walk's DEFAULT branch, and
   // the value-modifier archetypes -- how most diseases work -- have branches
   // of their own that break out first. `huginnSees` mirrors that walk, so
   // the dump shows what the check misses before the rule changes.
   static void Cmd_DumpDiseases(std::string_view /*arg*/)
   {
      auto* dataHandler = RE::TESDataHandler::GetSingleton();
      if (!dataHandler) {
         Print("Data handler unavailable");
         return;
      }
      std::ofstream out;
      std::filesystem::path filePath;
      if (!OpenDumpFile("Huginn_Diseases.csv"sv, out, filePath)) return;

      // The spells behind the player's live active effects.
      std::unordered_set<RE::FormID> onPlayer;
      if (auto* player = RE::PlayerCharacter::GetSingleton()) {
         if (auto* target = player->AsMagicTarget()) {
            if (auto* list = target->GetActiveEffectList()) {
               for (auto* ae : *list) {
                  if (ae && ae->spell && !ae->flags.any(RE::ActiveEffect::Flag::kInactive)) {
                     onPlayer.insert(ae->spell->GetFormID());
                  }
               }
            }
         }
      }

      // Mirrors the active-effect walk: these archetypes take their own
      // branch and never reach the disease check.
      auto reachesDiseaseCheck = [](const RE::EffectSetting* base) {
         using A = RE::EffectSetting::Archetype;
         switch (base->GetArchetype()) {
         case A::kNightEye:
         case A::kLight:
         case A::kInvisibility:
         case A::kValueModifier:
         case A::kPeakValueModifier:
         case A::kDualValueModifier:
         case A::kCloak:
         case A::kSummonCreature:
            return false;
         default:
            return base->IsDetrimental() &&
                   base->data.resistVariable == RE::ActorValue::kResistDisease;
         }
      };

      out << "formID,plugin,name,diseaseType,spellType,huginnSees,onPlayer,effects\n";

      size_t written = 0, diseases = 0, seen = 0, active = 0;
      std::string missedNow;
      for (auto* spell : dataHandler->GetFormArray<RE::SpellItem>()) {
         if (!spell) continue;
         const auto spellType = spell->GetSpellType();
         const bool isDisease = spellType == RE::MagicSystem::SpellType::kDisease;

         bool resistedByDisease = false;
         bool sees = false;
         std::string effects;
         for (const auto* effect : spell->effects) {
            if (!effect || !effect->baseEffect) continue;
            const auto* base = effect->baseEffect;
            if (base->data.resistVariable == RE::ActorValue::kResistDisease) resistedByDisease = true;
            if (reachesDiseaseCheck(base)) sees = true;

            std::string keywords;
            for (uint32_t k = 0; k < base->GetNumKeywords(); ++k) {
               if (auto kw = base->GetKeywordAt(k); kw && *kw) {
                  if (!keywords.empty()) keywords += ';';
                  keywords += (*kw)->GetFormEditorID();
               }
            }
            if (!effects.empty()) effects += " / ";
            const char* full = base->GetFullName();
            effects += std::format("'{}'{{{}}} arch={} pAV={} resist={} det={} [{}]",
               full ? full : "", base->GetFormEditorID(),
               static_cast<int>(base->GetArchetype()),
               static_cast<int>(base->data.primaryAV),
               static_cast<int>(base->data.resistVariable),
               base->IsDetrimental() ? 1 : 0, keywords);
         }
         if (!isDisease && !resistedByDisease) continue;

         // The walk skips abilities and addictions before any effect.
         if (spellType == RE::MagicSystem::SpellType::kAbility ||
             spellType == RE::MagicSystem::SpellType::kAddiction) {
            sees = false;
         }
         const bool now = onPlayer.contains(spell->GetFormID());
         const char* name = spell->GetName();

         out << std::format("{:08X},{},{},{},{},{},{},{}\n",
            spell->GetFormID(), CsvQuote(PluginOf(spell)), CsvQuote(name ? name : ""),
            isDisease ? 1 : 0, static_cast<int>(spellType),
            sees ? 1 : 0, now ? 1 : 0, CsvQuote(effects));
         ++written;
         if (isDisease) ++diseases;
         if (sees) ++seen;
         if (now) {
            ++active;
            if (!sees) {
               if (!missedNow.empty()) missedNow += ", ";
               missedNow += name ? name : "?";
            }
         }
      }
      out.close();

      auto msg = std::format("Wrote {} spells to Huginn_Diseases.csv - {} diseases, {} Huginn can see, {} on you now",
         written, diseases, seen, active);
      Print(msg.c_str());
      logger::info("[Console] {} -> {}"sv, msg, filePath.string());
      if (!missedNow.empty()) {
         auto missed = std::format("On you now but invisible to Huginn: {}", missedNow);
         Print(missed.c_str());
         logger::info("[Console] {}"sv, missed);
      }
   }

   // `hg dump weights` -- every item the learner holds, one row each: what
   // it is (kind / class / subclass), how much it has been trained, how
   // stale it is, and its full weight vector.
   //
   // The pre-check for pooling (roadmap, "Share learning across similar
   // items"): pooling seeds an item from its class, which only helps if the
   // classes actually differ from each other. Group this file by class and
   // subclass and compare -- the potion classes are the doubtful ones.
   //
   // Classified with fresh classifiers (the spell registry's when loaded, for
   // its overrides), so the registries' caches are untouched.
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

      Spell::SpellClassifier fallbackSpells;
      const auto& spellClassifier = g_spellRegistry ? g_spellRegistry->GetClassifier() : fallbackSpells;
      const Scroll::ScrollClassifier scrollClassifier(spellClassifier);
      Item::ItemClassifier itemClassifier;
      Weapon::WeaponClassifier weaponClassifier;

      out << "formID,plugin,name,kind,class,subclass,trainCount,confidence,minutesSinceChosen,retention,weightNorm";
      for (const char* feature : kFeatureNames) out << ',' << feature;
      out << '\n';

      size_t unresolved = 0;
      for (const auto& entry : entries) {
         std::string_view kind = "unresolved"sv, cls = "-"sv, sub = "-"sv;
         std::string clsOwned, subOwned;  // for classifier strings built at runtime
         auto* form = RE::TESForm::LookupByID(entry.formID);
         const char* name = form ? form->GetName() : nullptr;

         if (!form) {
            // Pseudo-items (Unarmed and the like) and forms from a plugin
            // that is no longer loaded.
            ++unresolved;
         } else if (auto* scroll = form->As<RE::ScrollItem>()) {
            const auto data = scrollClassifier.ClassifyScroll(scroll);
            kind = "Scroll"sv;
            cls = Spell::SpellTypeToString(data.type);
            sub = Spell::ElementTypeToString(data.element);
         } else if (auto* spell = form->As<RE::SpellItem>()) {
            const auto data = spellClassifier.ClassifySpell(spell);
            kind = "Spell"sv;
            cls = Spell::SpellTypeToString(data.type);
            sub = data.element != Spell::ElementType::None
               ? Spell::ElementTypeToString(data.element)
               : Spell::MagicSchoolToString(data.school);
         } else if (auto* alchemy = form->As<RE::AlchemyItem>()) {
            const auto data = itemClassifier.ClassifyItem(alchemy);
            kind = alchemy->IsFood() ? "Food"sv : (data.isHostile ? "Poison"sv : "Potion"sv);
            cls = Item::ItemTypeToString(data.type);
            sub = ItemSubclass(data);
         } else if (auto* weapon = form->As<RE::TESObjectWEAP>()) {
            kind = "Weapon"sv;
            cls = Weapon::WeaponTypeToString(weaponClassifier.ClassifyWeapon(weapon).type);
         } else if (auto* ammo = form->As<RE::TESAmmo>()) {
            kind = "Ammo"sv;
            cls = Weapon::AmmoTypeToString(weaponClassifier.ClassifyAmmo(ammo).type);
         } else if (form->As<RE::TESSoulGem>()) {
            kind = "SoulGem"sv;
         } else if (form->As<RE::TESObjectARMO>()) {
            kind = "Apparel"sv;
         } else if (form->As<RE::TESObjectLIGH>()) {
            kind = "Light"sv;
         } else {
            clsOwned = std::format("formType {}", static_cast<int>(form->GetFormType()));
            kind = "Other"sv;
            cls = clsOwned;
         }

         float norm = 0.0f;
         for (const float w : entry.weights) norm += w * w;

         out << std::format("{:08X},{},{},{},{},{},{:.2f},{:.3f},{},{:.3f},{:.4f}",
            entry.formID,
            CsvQuote(PluginOf(form)),
            CsvQuote(name ? name : ""),
            kind, cls, sub,
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

   // `hg dump potions` -- every potion and poison in the LOAD ORDER (food has
   // its own dump), with the classification Huginn gives it, the numbers that
   // tell two potions of one class apart (magnitude, duration, value), how
   // many the player carries and how often the learner has trained on it.
   // The question it answers: how differentiated are potions, really?
   static void Cmd_DumpPotions(std::string_view /*arg*/)
   {
      auto* dataHandler = RE::TESDataHandler::GetSingleton();
      if (!dataHandler) {
         Print("Data handler unavailable");
         return;
      }
      std::ofstream out;
      std::filesystem::path filePath;
      if (!OpenDumpFile("Huginn_Potions.csv"sv, out, filePath)) return;

      auto* player = RE::PlayerCharacter::GetSingleton();
      Item::ItemClassifier classifier;
      out << "formID,plugin,name,type,subclass,hostile,magnitude,duration,value,tags,tagsExt,"
             "playerCount,trainCount,effects\n";

      size_t written = 0, unknown = 0, carried = 0;
      for (auto* item : dataHandler->GetFormArray<RE::AlchemyItem>()) {
         if (!item || item->IsFood()) continue;
         const char* rawName = item->GetName();
         if (!rawName || !*rawName) continue;

         const auto data = classifier.ClassifyItem(item);
         const auto count = player ? Util::GetItemCountSafe(player, item) : 0;

         // 'name' magnitude/duration per effect, joined by " / ".
         std::string effects;
         for (const auto* effect : item->effects) {
            if (!effect || !effect->baseEffect) continue;
            const char* full = effect->baseEffect->GetFullName();
            if (!effects.empty()) effects += " / ";
            effects += std::format("'{}' {:g}/{}s", full ? full : "",
               effect->effectItem.magnitude, effect->effectItem.duration);
         }

         out << std::format("{:08X},{},{},{},{},{},{:g},{:g},{},{:08X},{:04X},{},{},{}\n",
            item->GetFormID(),
            CsvQuote(PluginOf(item)),
            CsvQuote(rawName),
            Item::ItemTypeToString(data.type),
            ItemSubclass(data),
            data.isHostile ? 1 : 0,
            data.magnitude,
            data.duration,
            data.value,
            static_cast<uint32_t>(data.tags),
            static_cast<uint32_t>(data.tagsExt),
            count,
            g_featureBanditLearner ? g_featureBanditLearner->GetTrainCount(item->GetFormID()) : 0.0f,
            CsvQuote(effects));
         ++written;
         if (data.type == Item::ItemType::Unknown) ++unknown;
         if (count > 0) ++carried;
      }
      out.close();

      auto msg = std::format("Wrote {} potions/poisons to Huginn_Potions.csv - {} unclassified, {} carried",
         written, unknown, carried);
      Print(msg.c_str());
      logger::info("[Console] {} -> {}"sv, msg, filePath.string());
   }

   // `hg dump scrolls` -- every scroll in the LOAD ORDER, as the scroll
   // registry sees it. `hg dump spells` already lists scrolls (since v0.21.8)
   // with the classifier's INPUTS; this one is the scroll side of the
   // cold-start question instead: what ScrollData keeps (magnitude, duration,
   // cost), how many the player carries, and whether the learner has ever
   // trained on one.
   static void Cmd_DumpScrolls(std::string_view /*arg*/)
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
      std::ofstream out;
      std::filesystem::path filePath;
      if (!OpenDumpFile("Huginn_Scrolls.csv"sv, out, filePath)) return;

      auto* player = RE::PlayerCharacter::GetSingleton();
      const Scroll::ScrollClassifier classifier(g_spellRegistry->GetClassifier());
      out << "formID,plugin,name,type,school,element,tags,tagsExt,magnitude,duration,baseCost,"
             "playerCount,trainCount\n";

      size_t written = 0, unknown = 0, carried = 0;
      for (auto* scroll : dataHandler->GetFormArray<RE::ScrollItem>()) {
         if (!scroll) continue;
         const char* rawName = scroll->GetName();
         if (!rawName || !*rawName) continue;

         const auto data = classifier.ClassifyScroll(scroll);
         const auto count = player ? Util::GetItemCountSafe(player, scroll) : 0;

         out << std::format("{:08X},{},{},{},{},{},{:08X},{:04X},{:g},{:g},{},{},{}\n",
            scroll->GetFormID(),
            CsvQuote(PluginOf(scroll)),
            CsvQuote(rawName),
            Spell::SpellTypeToString(data.type),
            Spell::MagicSchoolToString(data.school),
            Spell::ElementTypeToString(data.element),
            static_cast<uint32_t>(data.tags),
            static_cast<uint16_t>(data.tagsExt),
            data.magnitude,
            data.duration,
            data.baseCost,
            count,
            g_featureBanditLearner ? g_featureBanditLearner->GetTrainCount(scroll->GetFormID()) : 0.0f);
         ++written;
         if (data.type == Spell::SpellType::Unknown) ++unknown;
         if (count > 0) ++carried;
      }
      out.close();

      auto msg = std::format("Wrote {} scrolls to Huginn_Scrolls.csv - {} unclassified, {} carried",
         written, unknown, carried);
      Print(msg.c_str());
      logger::info("[Console] {} -> {}"sv, msg, filePath.string());
   }

   // An enchantment's effects as 'name' AV=n, joined by " / " -- the inputs
   // the weapon and apparel classifiers read.
   static std::string EnchantmentEffects(const RE::EnchantmentItem* enchantment)
   {
      std::string effects;
      if (!enchantment) return effects;
      for (const auto* effect : enchantment->effects) {
         if (!effect || !effect->baseEffect) continue;
         const char* full = effect->baseEffect->GetFullName();
         if (!effects.empty()) effects += " / ";
         effects += std::format("'{}' AV={}", full ? full : "",
            static_cast<int>(effect->baseEffect->data.primaryAV));
      }
      return effects;
   }

   // `hg dump weapons` -- every weapon and ammo in the LOAD ORDER with the
   // classifier's verdict and its inputs (base damage, speed, enchantment
   // effects), carried and train counts. The roadmap's "no dump at all"
   // entry: WeaponClassifier's rules had only been checked against what one
   // character carried.
   static void Cmd_DumpWeapons(std::string_view /*arg*/)
   {
      auto* dataHandler = RE::TESDataHandler::GetSingleton();
      if (!dataHandler) {
         Print("Data handler unavailable");
         return;
      }
      std::ofstream out;
      std::filesystem::path filePath;
      if (!OpenDumpFile("Huginn_Weapons.csv"sv, out, filePath)) return;

      auto* player = RE::PlayerCharacter::GetSingleton();
      Weapon::WeaponClassifier classifier;
      out << "formID,plugin,name,kind,type,tags,baseDamage,speed,enchantment,playerCount,trainCount,effects\n";

      auto trains = [](RE::FormID id) {
         return g_featureBanditLearner ? g_featureBanditLearner->GetTrainCount(id) : 0.0f;
      };
      size_t weapons = 0, ammo = 0, unknown = 0;
      for (auto* weapon : dataHandler->GetFormArray<RE::TESObjectWEAP>()) {
         if (!weapon) continue;
         const char* rawName = weapon->GetName();
         if (!rawName || !*rawName) continue;
         const auto data = classifier.ClassifyWeapon(weapon);
         const auto* enchantment = weapon->formEnchanting;
         out << std::format("{:08X},{},{},Weapon,{},{:08X},{:g},{:g},{},{},{},{}\n",
            weapon->GetFormID(), CsvQuote(PluginOf(weapon)), CsvQuote(rawName),
            Weapon::WeaponTypeToString(data.type), static_cast<uint32_t>(data.tags),
            data.baseDamage, data.speed,
            CsvQuote(enchantment && enchantment->GetName() ? enchantment->GetName() : ""),
            player ? Util::GetItemCountSafe(player, weapon) : 0, trains(weapon->GetFormID()),
            CsvQuote(EnchantmentEffects(enchantment)));
         ++weapons;
         if (data.type == Weapon::WeaponType::Unknown) ++unknown;
      }
      for (auto* item : dataHandler->GetFormArray<RE::TESAmmo>()) {
         if (!item) continue;
         const char* rawName = item->GetName();
         if (!rawName || !*rawName) continue;
         const auto data = classifier.ClassifyAmmo(item);
         out << std::format("{:08X},{},{},Ammo,{},{:08X},{:g},,,{},{},\n",
            item->GetFormID(), CsvQuote(PluginOf(item)), CsvQuote(rawName),
            Weapon::AmmoTypeToString(data.type), static_cast<uint32_t>(data.tags),
            data.baseDamage,
            player ? Util::GetItemCountSafe(player, item) : 0, trains(item->GetFormID()));
         ++ammo;
      }
      out.close();

      auto msg = std::format("Wrote {} weapons and {} ammo to Huginn_Weapons.csv - {} weapons unclassified",
         weapons, ammo, unknown);
      Print(msg.c_str());
      logger::info("[Console] {} -> {}"sv, msg, filePath.string());
   }

   // `hg dump apparel` -- every armour piece in the LOAD ORDER that carries an
   // enchantment, with the craft skill and slot ApparelClassifier gives it
   // and the enchantment's effects beside them. Unenchanted gear is skipped:
   // apparel is classified BY its enchantment, so it has nothing to show.
   // Base-form enchantments only; a player-applied one lives on the stack.
   static void Cmd_DumpApparel(std::string_view /*arg*/)
   {
      auto* dataHandler = RE::TESDataHandler::GetSingleton();
      if (!dataHandler) {
         Print("Data handler unavailable");
         return;
      }
      std::ofstream out;
      std::filesystem::path filePath;
      if (!OpenDumpFile("Huginn_Apparel.csv"sv, out, filePath)) return;

      auto* player = RE::PlayerCharacter::GetSingleton();
      Apparel::ApparelClassifier classifier;
      out << "formID,plugin,name,slot,craftSkill,magnitude,enchantment,playerCount,effects\n";

      size_t written = 0, craft = 0;
      for (auto* armor : dataHandler->GetFormArray<RE::TESObjectARMO>()) {
         if (!armor || !armor->formEnchanting) continue;
         const char* rawName = armor->GetName();
         if (!rawName || !*rawName) continue;
         auto* enchantment = armor->formEnchanting;
         const auto data = classifier.ClassifyApparel(armor, enchantment);
         out << std::format("{:08X},{},{},{},{},{:g},{},{},{}\n",
            armor->GetFormID(), CsvQuote(PluginOf(armor)), CsvQuote(rawName),
            Apparel::ApparelSlotToString(data.slot), Apparel::CraftSkillToString(data.craftSkill),
            data.magnitude,
            CsvQuote(enchantment->GetName() ? enchantment->GetName() : ""),
            player ? Util::GetItemCountSafe(player, armor) : 0,
            CsvQuote(EnchantmentEffects(enchantment)));
         ++written;
         if (data.IsCraftRelevant()) ++craft;
      }
      out.close();

      auto msg = std::format("Wrote {} enchanted apparel pieces to Huginn_Apparel.csv - {} fortify a craft",
         written, craft);
      Print(msg.c_str());
      logger::info("[Console] {} -> {}"sv, msg, filePath.string());
   }

   // `hg dump all` -- every item-like form in the LOAD ORDER in ONE schema, a
   // row per item x effect, read straight off the game data with no Huginn
   // classifier in the way. The input for designing the effect vector of
   // docs/architecture/9-context-as-learner-input.md: which effects really
   // occur, and what weapons and armour look like by their stats. One-time
   // research tool; the per-type dumps above stay the classifier views.
   // Ingredients are left out: Huginn dropped them (the user, 2026-10-07) --
   // they matter only at an alchemy lab.
   static std::string_view AvName(RE::ActorValue av)
   {
      if (av == RE::ActorValue::kNone || av >= RE::ActorValue::kTotal) return ""sv;
      const auto* list = RE::ActorValueList::GetSingleton();
      const auto* info = list ? list->GetActorValue(av) : nullptr;
      return info && info->enumName ? std::string_view(info->enumName) : ""sv;
   }

   static std::string KeywordList(const RE::BGSKeywordForm* form)
   {
      std::string list;
      if (!form) return list;
      for (std::uint32_t i = 0; i < form->numKeywords; ++i) {
         const auto* kw = form->keywords[i];
         const char* id = kw ? kw->GetFormEditorID() : nullptr;
         if (!id || !*id) continue;
         if (!list.empty()) list += ';';
         list += id;
      }
      return list;
   }

   static void Cmd_DumpAll(std::string_view /*arg*/)
   {
      auto* dataHandler = RE::TESDataHandler::GetSingleton();
      if (!dataHandler) {
         Print("Data handler unavailable");
         return;
      }
      std::ofstream out;
      std::filesystem::path filePath;
      if (!OpenDumpFile("Huginn_All.csv"sv, out, filePath)) return;

      auto* player = RE::PlayerCharacter::GetSingleton();

      // Spells a tome teaches: the line between a player spell and one only
      // NPCs cast, which spellType alone cannot draw.
      std::unordered_set<RE::FormID> taughtByTome;
      for (auto* book : dataHandler->GetFormArray<RE::TESObjectBOOK>()) {
         if (const auto* taught = book ? book->GetSpell() : nullptr) {
            taughtByTome.insert(taught->GetFormID());
         }
      }

      out << "kind,formID,plugin,winningPlugin,name,playable,value,weight,playerCount,keywords,"
             "spellType,castingType,delivery,magickaCost,taughtByTome,"
             "weaponType,twoHanded,damage,speed,reach,critDamage,"
             "armorSlots,armorRating,armorType,"
             "soulCapacity,soulContained,lightRadius,"
             "enchantment,enchantmentCharge,"
             "effectIndex,effectFormID,effectName,archetype,primaryAV,secondaryAV,resistAV,"
             "effectDelivery,effectCasting,magnitude,duration,area,effectBaseCost,detrimental,hostile,effectFlags,effectKeywords,effectDescription\n";

      // Columns up to (not including) the effect block. Each form fills its
      // own stat columns and leaves the rest empty.
      struct Base {
         std::string_view kind;
         const RE::TESBoundObject* form;
         std::string keywords;
         std::string magic;    // spellType..taughtByTome (5 columns)
         std::string weapon;   // weaponType..critDamage (6 columns)
         std::string armor;    // armorSlots..armorType (3 columns)
         std::string misc;     // soulCapacity..lightRadius (3 columns)
         const RE::EnchantmentItem* enchantment = nullptr;
         std::uint16_t charge = 0;
      };
      static constexpr std::string_view kNoMagic = ",,,,"sv;
      static constexpr std::string_view kNoWeapon = ",,,,,"sv;
      static constexpr std::string_view kNoArmor = ",,"sv;
      static constexpr std::string_view kNoMisc = ",,"sv;
      static constexpr std::string_view kNoEffect = ",,,,,,,,,,,,,,,,,"sv;  // 18 columns

      size_t forms = 0, rows = 0;
      auto prefix = [&](const Base& b) {
         const char* rawName = b.form->GetName();
         const auto* lastFile = b.form->GetFile(-1);
         // A spell's "gold value" is its magicka cost, and spells and ammo
         // report weight -1: leave both blank rather than mislead.
         const bool isSpell = b.form->Is(RE::FormType::Spell);
         const float weight = b.form->GetWeight();
         return std::format("{},{:08X},{},{},{},{},{},{},{},{},{},{},{},{},{},{},",
            b.kind, b.form->GetFormID(), CsvQuote(PluginOf(b.form)),
            CsvQuote(lastFile ? lastFile->GetFilename() : ""sv), CsvQuote(rawName ? rawName : ""),
            b.form->GetPlayable() ? 1 : 0,
            isSpell ? std::string{} : std::to_string(b.form->GetGoldValue()),
            weight < 0.0f ? std::string{} : std::format("{:g}", weight),
            player ? Util::GetItemCountSafe(player, b.form) : 0,
            CsvQuote(b.keywords),
            b.magic.empty() ? kNoMagic : std::string_view(b.magic),
            b.weapon.empty() ? kNoWeapon : std::string_view(b.weapon),
            b.armor.empty() ? kNoArmor : std::string_view(b.armor),
            b.misc.empty() ? kNoMisc : std::string_view(b.misc),
            CsvQuote(b.enchantment && b.enchantment->GetName() ? b.enchantment->GetName() : ""),
            b.enchantment ? std::to_string(b.charge) : std::string{});
      };
      auto effectCols = [](size_t i, const RE::Effect* e) {
         const auto* m = e->baseEffect;
         const char* full = m->GetFullName();
         // The description is the game's own text for the effect, with
         // <mag>/<dur> unfilled: the one readable account of a script-only
         // effect (LoreRim Arcaneum's spell text is this field).
         return std::format("{},{:08X},{},{},{},{},{},{},{},{:g},{},{},{:g},{},{},{:08X},{},{}",
            i, m->GetFormID(), CsvQuote(full ? full : ""),
            static_cast<int>(m->data.archetype),
            AvName(m->data.primaryAV), AvName(m->data.secondaryAV), AvName(m->data.resistVariable),
            static_cast<int>(m->data.delivery), static_cast<int>(m->data.castingType),
            e->effectItem.magnitude, e->effectItem.duration, e->effectItem.area, m->data.baseCost,
            m->IsDetrimental() ? 1 : 0, m->IsHostile() ? 1 : 0,
            static_cast<std::uint32_t>(m->data.flags.underlying()),
            CsvQuote(KeywordList(m)),
            CsvQuote(m->magicItemDescription.c_str() ? m->magicItemDescription.c_str() : ""));
      };
      // One row per effect of `effects` (the item's own, or its
      // enchantment's); one row with the effect block empty if it has none.
      auto emit = [&](const Base& b, const RE::BSTArray<RE::Effect*>* effects) {
         const auto head = prefix(b);
         size_t written = 0;
         if (effects) {
            for (size_t i = 0; i < effects->size(); ++i) {
               const auto* e = (*effects)[i];
               if (!e || !e->baseEffect) continue;
               out << head << effectCols(i, e) << '\n';
               ++written;
            }
         }
         if (written == 0) {
            out << head << kNoEffect << '\n';
            written = 1;
         }
         rows += written;
         ++forms;
      };
      auto named = [](const RE::TESForm* f) {
         const char* n = f ? f->GetName() : nullptr;
         return n && *n;
      };
      // Magicka cost for spells only: a scroll costs nothing to cast, and
      // CalculateMagickaCost on one returned garbage (up to 37.7M on vanilla).
      auto magicCols = [&](const RE::MagicItem* m, bool isSpell) {
         return std::format("{},{},{},{},{}",
            static_cast<int>(m->GetSpellType()), static_cast<int>(m->GetCastingType()),
            static_cast<int>(m->GetDelivery()),
            isSpell ? std::format("{:g}", m->CalculateMagickaCost(nullptr)) : std::string{},
            isSpell ? (taughtByTome.contains(m->GetFormID()) ? "1" : "0") : "");
      };

      for (auto* s : dataHandler->GetFormArray<RE::SpellItem>()) {
         if (!named(s)) continue;
         Base b{ "Spell", s, KeywordList(s) };
         b.magic = magicCols(s, true);
         emit(b, &s->effects);
      }
      for (auto* s : dataHandler->GetFormArray<RE::ScrollItem>()) {
         if (!named(s)) continue;
         Base b{ "Scroll", s, KeywordList(s) };
         b.magic = magicCols(s, false);
         emit(b, &s->effects);
      }
      for (auto* a : dataHandler->GetFormArray<RE::AlchemyItem>()) {
         if (!named(a)) continue;
         Base b{ a->IsPoison() ? "Poison"sv : a->IsFood() ? "Food"sv : "Potion"sv, a, KeywordList(a) };
         emit(b, &a->effects);
      }
      for (auto* w : dataHandler->GetFormArray<RE::TESObjectWEAP>()) {
         if (!named(w)) continue;
         Base b{ "Weapon", w, KeywordList(w) };
         const bool twoHanded = w->IsTwoHandedSword() || w->IsTwoHandedAxe() || w->IsBow() || w->IsCrossbow();
         b.weapon = std::format("{},{},{},{:g},{:g},{}",
            static_cast<int>(w->GetWeaponType()), twoHanded ? 1 : 0, w->GetAttackDamage(),
            w->weaponData.speed, w->weaponData.reach, w->criticalData.damage);
         b.enchantment = w->formEnchanting;
         b.charge = w->amountofEnchantment;
         emit(b, b.enchantment ? &b.enchantment->effects : nullptr);
      }
      for (auto* a : dataHandler->GetFormArray<RE::TESAmmo>()) {
         if (!named(a)) continue;
         Base b{ "Ammo", a, KeywordList(a) };
         b.weapon = std::format(",,{:g},,,", a->data.damage);
         emit(b, nullptr);
      }
      for (auto* a : dataHandler->GetFormArray<RE::TESObjectARMO>()) {
         if (!named(a)) continue;
         Base b{ "Armor", a, KeywordList(a) };
         b.armor = std::format("{:08X},{:g},{}",
            static_cast<std::uint32_t>(a->GetSlotMask()), a->GetArmorRating(),
            a->IsHeavyArmor() ? "Heavy"sv : a->IsLightArmor() ? "Light"sv : "None"sv);
         b.enchantment = a->formEnchanting;
         b.charge = a->amountofEnchantment;
         emit(b, b.enchantment ? &b.enchantment->effects : nullptr);
      }
      for (auto* g : dataHandler->GetFormArray<RE::TESSoulGem>()) {
         if (!named(g)) continue;
         Base b{ "SoulGem", g, KeywordList(g) };
         b.misc = std::format("{},{},", static_cast<int>(g->GetMaximumCapacity()),
            static_cast<int>(g->GetContainedSoul()));
         emit(b, nullptr);
      }
      for (auto* l : dataHandler->GetFormArray<RE::TESObjectLIGH>()) {
         if (!named(l) || !l->CanBeCarried()) continue;
         Base b{ "Light", l, {} };
         b.misc = std::format(",,{}", l->data.radius);
         emit(b, nullptr);
      }
      out.close();

      auto msg = std::format("Wrote {} forms as {} rows to Huginn_All.csv", forms, rows);
      Print(msg.c_str());
      logger::info("[Console] {} -> {}"sv, msg, filePath.string());
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
      { "unlock",        "Clear all slot locks",                        false, Cmd_Unlock },
      { "status",        "Show system status",                          false, Cmd_Status },
      { "weights",       "Show learner weight vector for FormID",          true,  Cmd_Weights },
      { "rebuild",       "Force rebuild all registries",                false, Cmd_Rebuild },
      { "reload",        "Hot-reload all settings from INI",            false, Cmd_Reload },
      { "page",          "Switch to page N (or show current)",          true,  Cmd_Page },
#ifndef NDEBUG
      { "dump spells",   "Write every castable spell to Huginn_Spells.csv (debug builds)", false, Cmd_DumpSpells },
      { "dump food",     "Write every food and drink to Huginn_Food.csv (debug builds)", false, Cmd_DumpFood },
      { "dump potions",  "Write every potion and poison to Huginn_Potions.csv (debug builds)", false, Cmd_DumpPotions },
      { "dump scrolls",  "Write every scroll to Huginn_Scrolls.csv (debug builds)", false, Cmd_DumpScrolls },
      { "dump weights",  "Write every learner entry to Huginn_Weights.csv (debug builds)", false, Cmd_DumpWeights },
      { "dump weapons",  "Write every weapon and ammo to Huginn_Weapons.csv (debug builds)", false, Cmd_DumpWeapons },
      { "dump apparel",  "Write every enchanted armour piece to Huginn_Apparel.csv (debug builds)", false, Cmd_DumpApparel },
      { "dump diseases", "Write every disease to Huginn_Diseases.csv (debug builds)", false, Cmd_DumpDiseases },
      { "dump all",      "Write every item, spell and effect in one schema to Huginn_All.csv (debug builds)", false, Cmd_DumpAll },
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
