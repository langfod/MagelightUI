#pragma once
//
// Precompiled header for the host plugin (Magelight.dll).
//
// CommonLib must be included before ANY Windows header, or REX aborts the
// translation unit (REX/W32/BASE.h hard-errors on _INC_WINAPIFAMILY). That rule
// was previously a convention each source file had to remember; as a PCH it is
// structural — MSVC injects this ahead of every translation unit in the target,
// so a source file cannot get the order wrong.
//
// std::literals is what SKSEPluginInfo's `.Name = "Magelight"sv` needs.
//
// Not used by MagelightGPU: the driver has no CommonLib dependency and must not
// acquire one (the LGPL seam, CLAUDE.md invariant 4).

#include <RE/Skyrim.h>
#include "SKSE/SKSE.h"

using namespace std::literals;