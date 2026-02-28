#pragma once

namespace Huginn::Console
{
   /// @brief Register the "Huginn" / "hg" console command by replacing an unused
   ///        Skyrim console command in the script function table.
   /// @note  Call once from kDataLoaded, before any game data is loaded.
   void Register();
}
