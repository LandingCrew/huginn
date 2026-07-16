// Only compile in debug mode
#ifdef _DEBUG

#include "RegistryDebugWidget.h"
#include "ImGuiCommon.h"
#include "../spell/SpellRegistry.h"
#include "../spell/SpellData.h"
#include "../learning/item/ItemRegistry.h"
#include "../learning/item/ItemData.h"
#include "../weapon/WeaponRegistry.h"
#include "../weapon/WeaponData.h"
#include "../scroll/ScrollRegistry.h"
#include "../scroll/ScrollData.h"
#include "Config.h"
#include <algorithm>
#include <format>
// Note: Huginn::VERSION_STRING is available via PCH.h -> Plugin.h

#include "../Globals.h"

namespace Huginn::UI
{
   // =============================================================================
   // COLOR CONSTANTS (reused from StateManagerDebugWidget)
   // =============================================================================

   namespace Colors
   {
      constexpr ImVec4 HEADER_YELLOW{0.8f, 0.8f, 0.2f, 1.0f};
      constexpr ImVec4 ACTIVE_INDICATOR{1.0f, 0.8f, 0.2f, 1.0f};
      constexpr ImVec4 INACTIVE_GRAY{0.5f, 0.5f, 0.5f, 1.0f};

      // Vital colors
      constexpr ImVec4 HEALTH_LOW{1.0f, 0.7f, 0.2f, 1.0f};
      constexpr ImVec4 MAGICKA_LOW{0.2f, 0.5f, 1.0f, 1.0f};
      constexpr ImVec4 STAMINA_LOW{0.2f, 1.0f, 0.2f, 1.0f};

      // Spell type colors
      constexpr ImVec4 SPELL_DAMAGE{1.0f, 0.4f, 0.2f, 1.0f};     // Red-orange
      constexpr ImVec4 SPELL_HEALING{0.2f, 0.8f, 0.4f, 1.0f};    // Green
      constexpr ImVec4 SPELL_DEFENSIVE{0.6f, 0.6f, 0.9f, 1.0f};  // Light blue
      constexpr ImVec4 SPELL_UTILITY{0.8f, 0.8f, 0.4f, 1.0f};    // Yellow
      constexpr ImVec4 SPELL_SUMMON{0.8f, 0.4f, 0.8f, 1.0f};     // Purple
      constexpr ImVec4 SPELL_BUFF{0.4f, 0.7f, 1.0f, 1.0f};       // Sky blue
      constexpr ImVec4 SPELL_DEBUFF{0.9f, 0.5f, 0.5f, 1.0f};     // Salmon

      // Item type colors
      constexpr ImVec4 EFFECT_POISON{0.5f, 1.0f, 0.2f, 1.0f};
      constexpr ImVec4 BUFF_COLOR{0.3f, 0.6f, 1.0f, 1.0f};

      // Weapon type colors (v0.7.6)
      constexpr ImVec4 WEAPON_MELEE{0.9f, 0.5f, 0.3f, 1.0f};    // Orange
      constexpr ImVec4 WEAPON_RANGED{0.3f, 0.7f, 0.9f, 1.0f};   // Light blue
      constexpr ImVec4 WEAPON_SILVER{0.8f, 0.8f, 0.9f, 1.0f};   // Silver
      constexpr ImVec4 WEAPON_ENCHANTED{0.9f, 0.4f, 0.9f, 1.0f}; // Purple
      constexpr ImVec4 AMMO_COLOR{0.6f, 0.8f, 0.4f, 1.0f};      // Olive

      // Element colors (v0.7.7 - for scrolls)
      constexpr ImVec4 ELEMENT_FIRE{1.0f, 0.3f, 0.1f, 1.0f};    // Bright red
      constexpr ImVec4 ELEMENT_FROST{0.4f, 0.8f, 1.0f, 1.0f};   // Ice blue
      constexpr ImVec4 ELEMENT_SHOCK{0.8f, 0.6f, 1.0f, 1.0f};   // Purple
   }

   RegistryDebugWidget& RegistryDebugWidget::GetSingleton() noexcept
   {
      static RegistryDebugWidget instance;
      return instance;
   }

   void RegistryDebugWidget::Draw()
   {
      if (!m_isVisible) {
      return;
      }

      // FIX (v0.7.18): Throttle expensive cache updates to 250ms intervals
      // This eliminates per-frame O(n) operations that caused instant stutter
      if (ShouldUpdateCache()) {
      UpdateSpellCache(g_spellRegistry.get());
      UpdateWeaponCache(g_weaponRegistry.get());
      UpdateItemCache(g_itemRegistry.get());
      UpdateScrollCache(g_scrollRegistry.get());
      }

      ImGui::SetNextWindowPos(ImVec2(m_posX, m_posY), ImGuiCond_FirstUseEver);
      ImGui::SetNextWindowBgAlpha(0.85f);
      ImGui::SetNextWindowCollapsed(false, ImGuiCond_FirstUseEver);

      ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_AlwaysAutoResize;

      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));

      // Build window title with current version
      static const std::string windowTitle = std::format("Registry [v{} DEBUG]", Huginn::VERSION_STRING);

      if (ImGui::Begin(windowTitle.c_str(), &m_isVisible, flags)) {
      // Save position when moved
      ImVec2 windowPos = ImGui::GetWindowPos();
      m_posX = windowPos.x;
      m_posY = windowPos.y;

      // Spell Registry Section
      static const std::string spellHeader = std::format("Spell Registry | Refresh: {:.0f}ms | Reconcile: {:.0f}s",
        Config::SPELL_FAVORITES_REFRESH_INTERVAL_MS,
        Config::SPELL_RECONCILE_INTERVAL_MS / 1000.0f);
      if (ImGui::CollapsingHeader(spellHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawSpellRegistrySection(g_spellRegistry.get());
      }

      // Item Registry Section
      static const std::string itemHeader = std::format("Item Registry | Refresh: {:.0f}ms | Reconcile: {:.0f}s",
        Config::ITEM_COUNT_REFRESH_INTERVAL_MS,
        Config::ITEM_RECONCILE_INTERVAL_MS / 1000.0f);
      if (ImGui::CollapsingHeader(itemHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawItemRegistrySection(g_itemRegistry.get());
      }

      // Weapon Registry Section (v0.7.6)
      static const std::string weaponHeader = std::format("Weapon Registry | Refresh: {:.0f}ms | Reconcile: {:.0f}s",
        Config::WEAPON_REFRESH_INTERVAL_MS,
        Config::WEAPON_RECONCILE_INTERVAL_MS / 1000.0f);
      if (ImGui::CollapsingHeader(weaponHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawWeaponRegistrySection(g_weaponRegistry.get());
      }

      // Scroll Registry Section (v0.7.7)
      static const std::string scrollHeader = std::format("Scroll Registry | Refresh: {:.0f}ms | Reconcile: {:.0f}s",
        Config::ITEM_COUNT_REFRESH_INTERVAL_MS,
        Config::ITEM_RECONCILE_INTERVAL_MS / 1000.0f);
      if (ImGui::CollapsingHeader(scrollHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawScrollRegistrySection(g_scrollRegistry.get());
      }

      }
      ImGui::End();

      ImGui::PopStyleVar(3);
   }

   // =============================================================================
   // SPELL REGISTRY SECTION
   // =============================================================================

   void RegistryDebugWidget::DrawSpellRegistrySection(const Spell::SpellRegistry* registry)
   {
      ImGui::Indent(8.0f);

      if (!registry) {
      ImGui::TextDisabled("SpellRegistry not initialized");
      ImGui::Unindent(8.0f);
      return;
      }

      if (registry->IsLoading()) {
      ImGui::TextColored(Colors::ACTIVE_INDICATOR, "Loading...");
      ImGui::Unindent(8.0f);
      return;
      }

      // FIX (v0.7.18): Use cached values instead of per-frame queries
      // Cache is updated every 250ms in Draw() via ShouldUpdateCache()
      ImGui::Text("Total Spells: %zu", m_spellCache.totalSpells);

      if (m_spellCache.totalSpells == 0) {
      ImGui::TextDisabled("No spells registered");
      ImGui::Unindent(8.0f);
      return;
      }

      // Display in 2-column layout (wider columns for readability)
      constexpr float COL_WIDTH = 130.0f;

      ImGui::TextColored(Colors::SPELL_DAMAGE, "Damage: %zu", m_spellCache.damageCount);
      ImGui::SameLine(COL_WIDTH);
      ImGui::TextColored(Colors::SPELL_HEALING, "Healing: %zu", m_spellCache.healingCount);

      ImGui::TextColored(Colors::SPELL_DEFENSIVE, "Defensive: %zu", m_spellCache.defensiveCount);
      ImGui::SameLine(COL_WIDTH);
      ImGui::TextColored(Colors::SPELL_UTILITY, "Utility: %zu", m_spellCache.utilityCount);

      ImGui::TextColored(Colors::SPELL_SUMMON, "Summon: %zu", m_spellCache.summonCount);
      ImGui::SameLine(COL_WIDTH);
      ImGui::TextColored(Colors::SPELL_BUFF, "Buff: %zu", m_spellCache.buffCount);

      ImGui::TextColored(Colors::SPELL_DEBUFF, "Debuff: %zu", m_spellCache.debuffCount);

      // Favorited spells (v0.7.8) - equipped/active spells
      if (m_spellCache.favoritedCount > 0) {
      ImGui::Separator();
      ImGui::TextColored(Colors::ACTIVE_INDICATOR, "Favorited: %zu", m_spellCache.favoritedCount);
      ImGui::TextDisabled("  (Currently equipped)");
      }

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // ITEM REGISTRY SECTION
   // =============================================================================

   void RegistryDebugWidget::DrawItemRegistrySection(const Item::ItemRegistry* registry)
   {
      ImGui::Indent(8.0f);

      if (!registry) {
      ImGui::TextDisabled("ItemRegistry not initialized");
      ImGui::Unindent(8.0f);
      return;
      }

      if (registry->IsLoading()) {
      ImGui::TextColored(Colors::ACTIVE_INDICATOR, "Loading...");
      ImGui::Unindent(8.0f);
      return;
      }

      const size_t totalItems = registry->GetItemCount();
      ImGui::Text("Tracked Items: %zu", totalItems);

      if (totalItems == 0) {
      ImGui::TextDisabled("No alchemy items in inventory");
      ImGui::Unindent(8.0f);
      return;
      }

      // FIX (v0.7.18): Only fetch detailed item data when section is expanded
      // These calls return vectors and are expensive - don't call every frame
      if (ImGui::TreeNode("Item Details")) {
      // Count by type using PotionScanner accessors
      auto healthPotions = registry->GetHealthPotionsByMagnitude();
      auto magickaPotions = registry->GetMagickaPotionsByMagnitude();
      auto staminaPotions = registry->GetStaminaPotionsByMagnitude();
      auto poisons = registry->GetItemsByType(Item::ItemType::Poison);
      auto food = registry->GetItemsByType(Item::ItemType::Food);
      auto buffPotions = registry->GetItemsByType(Item::ItemType::BuffPotion);

      // Resist potion breakdown
      auto resistFire = registry->GetResistFirePotions();
      auto resistFrost = registry->GetResistFrostPotions();
      auto resistShock = registry->GetResistShockPotions();
      auto resistPoison = registry->GetResistPoisonPotions();
      auto resistMagic = registry->GetResistMagicPotions();

      // Cure potions
      auto cureDisease = registry->GetCureDiseasePotions();
      auto curePoison = registry->GetCurePoisonPotions();

      // Potions summary (2-column layout)
      ImGui::TextColored(Colors::HEADER_YELLOW, "Potions:");
      ImGui::Text("  HP: %zu  MP: %zu  SP: %zu",
        healthPotions.size(), magickaPotions.size(), staminaPotions.size());
      ImGui::Text("  Buff: %zu  Poison: %zu  Food: %zu",
        buffPotions.size(), poisons.size(), food.size());

      // Resist breakdown
      ImGui::TextColored(Colors::HEADER_YELLOW, "Resist:");
      ImGui::Text("  Fire: %zu  Frost: %zu  Shock: %zu",
        resistFire.size(), resistFrost.size(), resistShock.size());
      ImGui::Text("  Poison: %zu  Magic: %zu",
        resistPoison.size(), resistMagic.size());

      // Cure breakdown
      ImGui::TextColored(Colors::HEADER_YELLOW, "Cures:");
      ImGui::Text("  Disease: %zu  Poison: %zu",
        cureDisease.size(), curePoison.size());

      // Soul Gems (v0.7.8)
      ImGui::TextColored(Colors::HEADER_YELLOW, "Soul Gems:");
      auto soulGems = registry->GetSoulGems();
      ImGui::Text("  Total: %zu", soulGems.size());

      // Show best soul gem if available (cached copy — see UpdateItemCache)
      if (m_itemCache.bestSoulGem.valid) {
        ImGui::Text("  Best: %s (%.0f)",
           m_itemCache.bestSoulGem.name.c_str(),
           m_itemCache.bestSoulGem.value);
      }

      ImGui::TreePop();  // Close "Item Details" TreeNode
      }

      ImGui::Separator();

      // Show best potions from the 250ms snapshot (owned copies — render thread
      // must never dereference registry-internal pointers, see UpdateItemCache)
      ImGui::TextColored(Colors::HEADER_YELLOW, "Best Potions:");

      auto showBestPotion = [](const char* label, const BestEntrySnap& best, ImVec4 color) {
      if (!best.valid) {
        ImGui::TextDisabled("  %s: None", label);
        return;
      }
      ImGui::TextColored(color, "  %s:", label);
      ImGui::SameLine();
      ImGui::Text("%s (%.0f) x%d", best.name.c_str(), best.value, best.count);
      };

      showBestPotion("HP", m_itemCache.bestHealth, Colors::HEALTH_LOW);
      showBestPotion("MP", m_itemCache.bestMagicka, Colors::MAGICKA_LOW);
      showBestPotion("SP", m_itemCache.bestStamina, Colors::STAMINA_LOW);
      showBestPotion("R.Fire", m_itemCache.bestResistFire, Colors::SPELL_DAMAGE);
      showBestPotion("R.Frost", m_itemCache.bestResistFrost, Colors::SPELL_DEFENSIVE);
      showBestPotion("R.Shock", m_itemCache.bestResistShock, Colors::ACTIVE_INDICATOR);

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // WEAPON REGISTRY SECTION (v0.7.6)
   // =============================================================================

   void RegistryDebugWidget::DrawWeaponRegistrySection(const Weapon::WeaponRegistry* registry)
   {
      ImGui::Indent(8.0f);

      if (!registry) {
      ImGui::TextDisabled("WeaponRegistry not initialized");
      ImGui::Unindent(8.0f);
      return;
      }

      if (registry->IsLoading()) {
      ImGui::TextColored(Colors::ACTIVE_INDICATOR, "Loading...");
      ImGui::Unindent(8.0f);
      return;
      }

      // FIX (v0.7.18): Use cached values instead of per-frame queries
      // Cache is updated every 250ms in Draw() via ShouldUpdateCache()
      ImGui::Text("Weapons: %zu | Ammo: %zu", m_weaponCache.weaponCount, m_weaponCache.ammoCount);
      ImGui::SameLine();
      if (m_weaponCache.favoritedCount > 0) {
      ImGui::TextColored(Colors::ACTIVE_INDICATOR, "| Favorited: %zu", m_weaponCache.favoritedCount);
      } else {
      ImGui::TextColored(Colors::HEALTH_LOW, "| No favorited weapons");
      }

      if (m_weaponCache.weaponCount == 0 && m_weaponCache.ammoCount == 0) {
      ImGui::TextDisabled("No weapons or ammo tracked");
      ImGui::Unindent(8.0f);
      return;
      }

      ImGui::Separator();

      // Weapon breakdown - FIX (v0.7.18): Only fetch data when section is expanded
      // These calls return vectors and are expensive - don't call every frame
      if (m_weaponCache.weaponCount > 0 && ImGui::TreeNode("Weapon Details")) {
      auto meleeWeapons = registry->GetMeleeWeapons();
      auto rangedWeapons = registry->GetRangedWeapons();
      auto silverWeapons = registry->GetSilveredWeapons();
      auto enchantedWeapons = registry->GetEnchantedWeapons();
      auto lowChargeWeapons = registry->GetWeaponsNeedingCharge();

      constexpr float COL_WIDTH = 130.0f;

      ImGui::TextColored(Colors::HEADER_YELLOW, "Weapons:");
      ImGui::TextColored(Colors::WEAPON_MELEE, "  Melee: %zu", meleeWeapons.size());
      ImGui::SameLine(COL_WIDTH);
      ImGui::TextColored(Colors::WEAPON_RANGED, "Ranged: %zu", rangedWeapons.size());

      ImGui::TextColored(Colors::WEAPON_SILVER, "  Silver: %zu", silverWeapons.size());
      ImGui::SameLine(COL_WIDTH);
      ImGui::TextColored(Colors::WEAPON_ENCHANTED, "Enchanted: %zu", enchantedWeapons.size());

      if (!lowChargeWeapons.empty()) {
        ImGui::TextColored(Colors::HEALTH_LOW, "  Low Charge: %zu", lowChargeWeapons.size());
      }

      // Best weapons from the 250ms snapshot (owned copies — see UpdateWeaponCache)
      ImGui::TextColored(Colors::HEADER_YELLOW, "Best:");

      auto showBestWeapon = [](const char* label, const BestEntrySnap& best, ImVec4 color) {
        if (!best.valid) {
           ImGui::TextDisabled("  %s: None", label);
           return;
        }
        ImGui::TextColored(color, "  %s:", label);
        ImGui::SameLine();
        if (best.hasEnchantment) {
           ImGui::Text("%s (%.0f dmg, %.0f%%)",
            best.name.c_str(),
            best.value,
            best.charge * 100.0f);
        } else {
           ImGui::Text("%s (%.0f dmg)",
            best.name.c_str(),
            best.value);
        }
      };

      showBestWeapon("Melee", m_weaponCache.bestMelee, Colors::WEAPON_MELEE);
      showBestWeapon("Ranged", m_weaponCache.bestRanged, Colors::WEAPON_RANGED);

      if (m_weaponCache.bestSilver.valid) {
        showBestWeapon("Silver", m_weaponCache.bestSilver, Colors::WEAPON_SILVER);
      }

      ImGui::TreePop();  // Close "Weapon Details" TreeNode
      }

      // Ammo breakdown - FIX (v0.7.18): Only fetch data when section is expanded
      if (m_weaponCache.ammoCount > 0 && ImGui::TreeNode("Ammo Details")) {
      ImGui::Separator();

      auto arrows = registry->GetArrows();
      auto bolts = registry->GetBolts();
      auto magicAmmo = registry->GetMagicAmmo();

      ImGui::TextColored(Colors::HEADER_YELLOW, "Ammo:");
      ImGui::TextColored(Colors::AMMO_COLOR, "  Arrows: %zu  Bolts: %zu  Magic: %zu",
        arrows.size(), bolts.size(), magicAmmo.size());

      // Best ammo from the 250ms snapshot (owned copies — see UpdateWeaponCache)
      auto showBestAmmo = [](const char* label, const BestEntrySnap& best, ImVec4 color) {
        if (!best.valid) {
           return;
        }
        ImGui::TextColored(color, "  Best %s:", label);
        ImGui::SameLine();
        ImGui::Text("%s (%.0f dmg) x%d",
           best.name.c_str(),
           best.value,
           best.count);
      };

      showBestAmmo("Arrow", m_weaponCache.bestArrow, Colors::AMMO_COLOR);
      showBestAmmo("Bolt", m_weaponCache.bestBolt, Colors::AMMO_COLOR);

      ImGui::TreePop();  // Close "Ammo Details" TreeNode
      }

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // SCROLL REGISTRY SECTION (v0.7.7)
   // =============================================================================

   void RegistryDebugWidget::DrawScrollRegistrySection(const Scroll::ScrollRegistry* registry)
   {
      ImGui::Indent(8.0f);

      if (!registry) {
      ImGui::TextDisabled("ScrollRegistry not initialized");
      ImGui::Unindent(8.0f);
      return;
      }

      const size_t totalScrolls = registry->GetScrollCount();
      ImGui::Text("Total Scrolls: %zu", totalScrolls);

      if (totalScrolls == 0) {
      ImGui::TextDisabled("No scrolls in inventory");
      ImGui::Unindent(8.0f);
      return;
      }

      // FIX (v0.7.18): Only fetch detailed scroll data when section is expanded
      // These calls return vectors and are expensive - don't call every frame
      if (ImGui::TreeNode("Scroll Details")) {
      // Count by type
      auto damageScrolls = registry->GetDamageScrolls();
      auto healingScrolls = registry->GetHealingScrolls();
      auto defensiveScrolls = registry->GetDefensiveScrolls();
      auto utilityScrolls = registry->GetUtilityScrolls();
      auto summonScrolls = registry->GetSummonScrolls();

      // Count by element
      auto fireScrolls = registry->GetFireScrolls();
      auto frostScrolls = registry->GetFrostScrolls();
      auto shockScrolls = registry->GetShockScrolls();

      // Type breakdown
      constexpr float COL_WIDTH = 130.0f;

      ImGui::TextColored(Colors::HEADER_YELLOW, "By Type:");
      ImGui::TextColored(Colors::SPELL_DAMAGE, "  Damage: %zu", damageScrolls.size());
      ImGui::SameLine(COL_WIDTH);
      ImGui::TextColored(Colors::SPELL_HEALING, "Healing: %zu", healingScrolls.size());

      ImGui::TextColored(Colors::SPELL_DEFENSIVE, "  Defensive: %zu", defensiveScrolls.size());
      ImGui::SameLine(COL_WIDTH);
      ImGui::TextColored(Colors::SPELL_UTILITY, "Utility: %zu", utilityScrolls.size());

      ImGui::TextColored(Colors::SPELL_SUMMON, "  Summon: %zu", summonScrolls.size());

      // Element breakdown
      ImGui::TextColored(Colors::HEADER_YELLOW, "By Element:");
      ImGui::TextColored(Colors::ELEMENT_FIRE, "  Fire: %zu", fireScrolls.size());
      ImGui::SameLine(COL_WIDTH);
      ImGui::TextColored(Colors::ELEMENT_FROST, "Frost: %zu", frostScrolls.size());

      ImGui::TextColored(Colors::ELEMENT_SHOCK, "  Shock: %zu", shockScrolls.size());

      ImGui::TreePop();  // Close "Scroll Details" TreeNode
      }

      // Best scrolls from the 250ms snapshot (owned copies — see UpdateScrollCache)
      ImGui::Separator();
      ImGui::TextColored(Colors::HEADER_YELLOW, "Best Scrolls:");

      auto showBestScroll = [](const char* label, const BestEntrySnap& best, ImVec4 color) {
      if (!best.valid) {
        ImGui::TextDisabled("  %s: None", label);
        return;
      }
      ImGui::TextColored(color, "  %s:", label);
      ImGui::SameLine();
      ImGui::Text("%s (%.0f) x%d",
        best.name.c_str(),
        best.value,
        best.count);
      };

      showBestScroll("Damage", m_scrollCache.bestDamage, Colors::SPELL_DAMAGE);
      showBestScroll("Healing", m_scrollCache.bestHealing, Colors::SPELL_HEALING);
      showBestScroll("Fire", m_scrollCache.bestFire, Colors::ELEMENT_FIRE);
      showBestScroll("Frost", m_scrollCache.bestFrost, Colors::ELEMENT_FROST);
      showBestScroll("Shock", m_scrollCache.bestShock, Colors::ELEMENT_SHOCK);

      ImGui::Unindent(8.0f);
   }

   // =============================================================================
   // CACHE HELPERS (v0.7.18 - throttle expensive operations)
   // =============================================================================

   bool RegistryDebugWidget::ShouldUpdateCache()
   {
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastCacheUpdate);

      if (elapsed.count() >= static_cast<int64_t>(CACHE_UPDATE_INTERVAL_MS)) {
      m_lastCacheUpdate = now;
      return true;
      }
      return false;
   }

   void RegistryDebugWidget::UpdateSpellCache(const Spell::SpellRegistry* registry)
   {
      if (!registry || registry->IsLoading()) {
      return;
      }

      m_spellCache.totalSpells = registry->GetSpellCount();
      m_spellCache.damageCount = registry->GetSpellCountByType(Spell::SpellType::Damage);
      m_spellCache.healingCount = registry->GetSpellCountByType(Spell::SpellType::Healing);
      m_spellCache.defensiveCount = registry->GetSpellCountByType(Spell::SpellType::Defensive);
      m_spellCache.utilityCount = registry->GetSpellCountByType(Spell::SpellType::Utility);
      m_spellCache.summonCount = registry->GetSpellCountByType(Spell::SpellType::Summon);
      m_spellCache.buffCount = registry->GetSpellCountByType(Spell::SpellType::Buff);
      m_spellCache.debuffCount = registry->GetSpellCountByType(Spell::SpellType::Debuff);
      m_spellCache.favoritedCount = registry->GetFavoritedSpells().size();
   }

   void RegistryDebugWidget::UpdateWeaponCache(const Weapon::WeaponRegistry* registry)
   {
      if (!registry || registry->IsLoading()) {
      return;
      }

      WeaponCacheData fresh;
      fresh.weaponCount = registry->GetWeaponCount();
      fresh.ammoCount = registry->GetAmmoCount();

      // Single pass per container under the registry lock (ForEach* holds it):
      // favorited count + best picks copied by value, so the render path never
      // reads registry-internal pointers. Selection mirrors
      // WeaponRegistry::GetBestMeleeWeapon/RangedWeapon/SilveredWeapon/Arrow/Bolt.
      registry->ForEachWeapon([&fresh](const Weapon::InventoryWeapon& weapon) {
      if (weapon.isFavorited) {
        ++fresh.favoritedCount;
      }
      auto consider = [&weapon](BestEntrySnap& slot) {
        if (weapon.data.baseDamage > slot.value) {
           slot.valid = true;
           slot.name = weapon.data.name;
           slot.value = weapon.data.baseDamage;
           slot.hasEnchantment = weapon.data.hasEnchantment;
           slot.charge = weapon.data.currentCharge;
        }
      };
      if (HasTag(weapon.data.tags, Weapon::WeaponTag::Melee)) {
        consider(fresh.bestMelee);
      }
      if (HasTag(weapon.data.tags, Weapon::WeaponTag::Ranged)) {
        consider(fresh.bestRanged);
      }
      if (HasTag(weapon.data.tags, Weapon::WeaponTag::Silver)) {
        consider(fresh.bestSilver);
      }
      });

      registry->ForEachAmmo([&fresh](const Weapon::InventoryAmmo& ammo) {
      if (ammo.count <= 0) {
        return;
      }
      auto consider = [&ammo](BestEntrySnap& slot) {
        if (ammo.data.baseDamage > slot.value) {
           slot.valid = true;
           slot.name = ammo.data.name;
           slot.value = ammo.data.baseDamage;
           slot.count = ammo.count;
        }
      };
      if (ammo.data.type == Weapon::AmmoType::Arrow) {
        consider(fresh.bestArrow);
      } else if (ammo.data.type == Weapon::AmmoType::Bolt) {
        consider(fresh.bestBolt);
      }
      });

      m_weaponCache = std::move(fresh);
   }

   void RegistryDebugWidget::UpdateItemCache(const Item::ItemRegistry* registry)
   {
      if (!registry || registry->IsLoading()) {
      return;
      }

      // Single pass under the registry lock (ForEachItem holds it): best picks
      // copied by value for render-thread display. Selection mirrors
      // ItemRegistry::GetBest*Potion (max magnitude, count > 0) and
      // GetBestSoulGem (filled only).
      ItemCacheData fresh;
      registry->ForEachItem([&fresh](const Item::InventoryItem& item) {
      if (item.count <= 0) {
        return;
      }
      auto consider = [&item](BestEntrySnap& slot) {
        if (item.data.magnitude > slot.value) {
           slot.valid = true;
           slot.name = item.data.name;
           slot.value = item.data.magnitude;
           slot.count = item.count;
        }
      };
      switch (item.data.type) {
      case Item::ItemType::HealthPotion:
        consider(fresh.bestHealth);
        break;
      case Item::ItemType::MagickaPotion:
        consider(fresh.bestMagicka);
        break;
      case Item::ItemType::StaminaPotion:
        consider(fresh.bestStamina);
        break;
      case Item::ItemType::SoulGem:
        if (item.data.isFilled) {
           consider(fresh.bestSoulGem);
        }
        break;
      default:
        break;
      }
      if (HasTag(item.data.tags, Item::ItemTag::ResistFire)) {
        consider(fresh.bestResistFire);
      }
      if (HasTag(item.data.tags, Item::ItemTag::ResistFrost)) {
        consider(fresh.bestResistFrost);
      }
      if (HasTag(item.data.tags, Item::ItemTag::ResistShock)) {
        consider(fresh.bestResistShock);
      }
      });

      m_itemCache = std::move(fresh);
   }

   void RegistryDebugWidget::UpdateScrollCache(const Scroll::ScrollRegistry* registry)
   {
      if (!registry || registry->IsLoading()) {
      return;
      }

      // Single pass under the registry lock (ForEachScroll holds it): best picks
      // copied by value for render-thread display. Selection mirrors
      // ScrollRegistry::GetBest* (max magnitude, count > 0).
      ScrollCacheData fresh;
      registry->ForEachScroll([&fresh](const Scroll::InventoryScroll& scroll) {
      if (scroll.count <= 0) {
        return;
      }
      auto consider = [&scroll](BestEntrySnap& slot) {
        if (scroll.data.magnitude > slot.value) {
           slot.valid = true;
           slot.name = scroll.data.name;
           slot.value = scroll.data.magnitude;
           slot.count = scroll.count;
        }
      };
      if (scroll.data.type == Scroll::ScrollType::Damage) {
        consider(fresh.bestDamage);
      } else if (scroll.data.type == Scroll::ScrollType::Healing) {
        consider(fresh.bestHealing);
      }
      if (HasTag(scroll.data.tags, Scroll::ScrollTag::Fire)) {
        consider(fresh.bestFire);
      }
      if (HasTag(scroll.data.tags, Scroll::ScrollTag::Frost)) {
        consider(fresh.bestFrost);
      }
      if (HasTag(scroll.data.tags, Scroll::ScrollTag::Shock)) {
        consider(fresh.bestShock);
      }
      });

      m_scrollCache = std::move(fresh);
   }
}

#endif  // _DEBUG
