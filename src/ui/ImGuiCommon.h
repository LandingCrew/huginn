#pragma once

// Central ImGui include for Huginn's UI translation units.
//
// IMGUI_DEFINE_MATH_OPERATORS must be defined *before* imgui.h and stay
// consistent across every TU that includes it (otherwise ImVec2 operator
// overloads appear in some TUs and not others — an ODR hazard). It lives here,
// not per-file. These used to sit in PCH.h and were parsed by every translation
// unit in the project; only src/ui/ needs ImGui, so they're scoped here instead.
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#   define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "imgui.h"
