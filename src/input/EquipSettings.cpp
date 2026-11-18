#include "EquipSettings.h"
#include <SimpleIni.h>

namespace Huginn::Input
{
   EquipSettings& EquipSettings::GetSingleton()
   {
      static EquipSettings singleton;
      return singleton;
   }

   // The shipped Huginn.ini carries the default; the constant applies only to
   // a key in neither file.
   void EquipSettings::LoadFromIni(const CSimpleIniA& ini)
   {
      const bool redraw = ini.GetBoolValue("Equip", "bRedrawAfterHandEquip", true);
      const bool was = m_redraw.exchange(redraw, std::memory_order_relaxed);
      logger::info("[EquipSettings] Loaded: bRedrawAfterHandEquip={}{}"sv, redraw,
         was != redraw ? " (changed)" : "");
   }
}
