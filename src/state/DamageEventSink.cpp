#include "DamageEventSink.h"

namespace Huginn::State
{
   DamageEventSink& DamageEventSink::GetSingleton()
   {
      static DamageEventSink singleton;
      return singleton;
   }

   void DamageEventSink::Register()
   {
      if (m_registered.load(std::memory_order_acquire)) {
      logger::warn("[DamageEventSink] Already registered, skipping"sv);
      return;
      }

      auto* eventSource = RE::ScriptEventSourceHolder::GetSingleton();
      if (!eventSource) {
      logger::error("[DamageEventSink] Failed to get ScriptEventSourceHolder"sv);
      return;
      }

      eventSource->AddEventSink<RE::TESHitEvent>(this);
      m_registered.store(true, std::memory_order_release);

      logger::info("[DamageEventSink] Registered for TESHitEvent (instant damage classification)"sv);
   }

   void DamageEventSink::Unregister()
   {
      if (!m_registered.load(std::memory_order_acquire)) {
      return;
      }

      auto* eventSource = RE::ScriptEventSourceHolder::GetSingleton();
      if (eventSource) {
      eventSource->RemoveEventSink<RE::TESHitEvent>(this);
      }

      m_registered.store(false, std::memory_order_release);
      logger::info("[DamageEventSink] Unregistered from TESHitEvent"sv);
   }

   std::vector<QueuedDamageEvent> DamageEventSink::DrainQueue()
   {
      std::vector<QueuedDamageEvent> events;

      std::lock_guard lock(m_queueMutex);
      events.reserve(m_eventQueue.size());

      while (!m_eventQueue.empty()) {
      events.push_back(m_eventQueue.front());
      m_eventQueue.pop();
      }

      return events;
   }

   size_t DamageEventSink::GetQueueSize() const
   {
      std::lock_guard lock(m_queueMutex);
      return m_eventQueue.size();
   }

   RE::BSEventNotifyControl DamageEventSink::ProcessEvent(
      const RE::TESHitEvent* event,
      RE::BSTEventSource<RE::TESHitEvent>* /*source*/)
   {
      if (!event) {
      return RE::BSEventNotifyControl::kContinue;
      }

      // Only track hits to the player
      auto* player = RE::PlayerCharacter::GetSingleton();
      if (!player) {
      return RE::BSEventNotifyControl::kContinue;
      }

      // TESHitEvent::target is a NiPointer<TESObjectREFR>
      auto targetRef = event->target.get();
      if (!targetRef || targetRef != player) {
      return RE::BSEventNotifyControl::kContinue;
      }

      // Classify damage type from the hit event
      DamageType damageType = ClassifyHitEvent(event);

      // Only queue if we successfully classified a non-physical damage type
      // (Physical damage is the default fallback, so we don't need to queue it)
      if (damageType == DamageType::Physical) {
      return RE::BSEventNotifyControl::kContinue;
      }

      // Get current game time
      float gameTime = 0.0f;
      auto* calendar = RE::Calendar::GetSingleton();
      if (calendar) {
      gameTime = calendar->GetCurrentGameTime();
      }

      // Queue the damage event
      {
      std::lock_guard lock(m_queueMutex);

      // Prevent unbounded queue growth
      if (m_eventQueue.size() >= MAX_QUEUE_SIZE) {
        m_eventQueue.pop();  // Drop oldest event
      }

      m_eventQueue.emplace(gameTime, 0.0f, damageType);
      }

      logger::trace("[DamageEventSink] Queued {} damage from TESHitEvent"sv,
      GetDamageTypeName(damageType));

      return RE::BSEventNotifyControl::kContinue;
   }

   DamageType DamageEventSink::ClassifyHitEvent(const RE::TESHitEvent* event) const
   {
      if (!event) {
      return DamageType::Physical;
      }

      // Priority 1: Check spell from the hit event directly
      // TESHitEvent contains source FormID which can be a spell or projectile
      RE::FormID sourceFormID = event->source;
      if (sourceFormID != 0) {
      auto* form = RE::TESForm::LookupByID(sourceFormID);
      if (form) {
        // Check if source is a spell/enchantment
        if (auto* spell = form->As<RE::MagicItem>()) {
           DamageType type = ClassifySpellDamageType(spell);
           if (type != DamageType::Physical) {
            return type;
           }
        }

        // Check if source is a weapon with enchantment
        if (auto* weapon = form->As<RE::TESObjectWEAP>()) {
           auto* enchant = weapon->formEnchanting;
           if (enchant) {
            DamageType type = ClassifySpellDamageType(enchant);
            if (type != DamageType::Physical) {
              return type;
            }
           }
        }
      }
      }

      // Priority 2: Check projectile
      RE::FormID projectileFormID = event->projectile;
      if (projectileFormID != 0) {
      auto* projectileForm = RE::TESForm::LookupByID(projectileFormID);
      if (auto* projectile = projectileForm ? projectileForm->As<RE::BGSProjectile>() : nullptr) {
        // Check if projectile has an associated explosion with enchantment
        // explosionType is a BGSExplosion* pointer (not a FormID)
        auto* explosion = projectile->data.explosionType;
        if (explosion && explosion->formEnchanting) {
           DamageType type = ClassifySpellDamageType(explosion->formEnchanting);
           if (type != DamageType::Physical) {
            return type;
           }
        }
      }
      }

      // Priority 3: Check the aggressor's equipped spell (fallback for some edge cases)
      if (event->cause) {
      auto causeRef = event->cause.get();
      if (auto* aggressor = causeRef ? causeRef->As<RE::Actor>() : nullptr) {
        // Check equipped spell in each hand using Actor's GetEquippedObject
        auto* rightHandObj = aggressor->GetEquippedObject(false);  // false = right hand
        auto* leftHandObj = aggressor->GetEquippedObject(true);    // true = left hand

        for (auto* obj : {rightHandObj, leftHandObj}) {
           if (auto* spell = obj ? obj->As<RE::SpellItem>() : nullptr) {
            DamageType type = ClassifySpellDamageType(spell);
            if (type != DamageType::Physical) {
              return type;
            }
           }
        }
      }
      }

      return DamageType::Physical;
   }

   DamageType DamageEventSink::ClassifySpellDamageType(const RE::MagicItem* spell) const
   {
      if (!spell) {
      return DamageType::Physical;
      }

      // Check each effect in the spell
      for (const auto* effectItem : spell->effects) {
      if (!effectItem || !effectItem->baseEffect) {
        continue;
      }

      const auto* baseEffect = effectItem->baseEffect;

      // Only check detrimental (damaging) effects
      if (!baseEffect->IsDetrimental()) {
        continue;
      }

      // Classify by resist variable (most reliable method)
      auto resistAV = baseEffect->data.resistVariable;

      switch (resistAV) {
        case RE::ActorValue::kResistFire:
           return DamageType::Fire;
        case RE::ActorValue::kResistFrost:
           return DamageType::Frost;
        case RE::ActorValue::kResistShock:
           return DamageType::Shock;
        case RE::ActorValue::kPoisonResist:
           return DamageType::Poison;
        case RE::ActorValue::kResistDisease:
           return DamageType::Disease;
        case RE::ActorValue::kResistMagic:
           return DamageType::Magic;
        default:
           break;
      }

      // Fallback: Check for damage health archetype without specific resist
      if (baseEffect->data.primaryAV == RE::ActorValue::kHealth) {
        auto archetype = baseEffect->GetArchetype();
        if (archetype == RE::EffectSetting::Archetype::kValueModifier ||
            archetype == RE::EffectSetting::Archetype::kDualValueModifier) {
           // Generic magic damage (no element)
           return DamageType::Magic;
        }
      }
      }

      return DamageType::Physical;
   }
}
