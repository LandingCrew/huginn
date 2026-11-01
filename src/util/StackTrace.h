#pragma once

#include <string>

namespace Huginn::Util::StackTrace
{
   // "Module.dll+0x1A2B3C" for a code address; "?+0x..." outside any module
   // (generated code). No module is pinned. Shared by the equip caller trace
   // and the abort trace.
   std::string ModuleOffset(void* address);
}
