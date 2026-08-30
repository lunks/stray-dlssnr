// include_order_negative.cpp - a compile that MUST FAIL.
//
// reshade_overlay.hpp has no include guard; reshade.hpp (#pragma once) includes it, and its body
// is wrapped in `#if defined(IMGUI_VERSION_NUM)`. Pull reshade_compat.hpp in first and the whole
// ImGui surface silently vanishes from the translation unit, with no second chance. src/
// overlay_imgui.hpp carries an #error for exactly this. CI compiles this file and fails the build
// if it SUCCEEDS, so that guard cannot rot.
#include "../src/reshade_compat.hpp"
#include "../src/overlay_imgui.hpp"

int main() { return 0; }
