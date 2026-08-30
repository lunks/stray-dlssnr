// imvec_callee.cpp - ALWAYS built with MSVC. This DLL stands in for ReShade.
//
// Every export here is shaped like a real entry of imgui_function_table_19250. The caller
// (abi/imvec_caller.cpp, built by BOTH toolchains) loads this DLL and calls through function
// pointers with the table's exact declared types, which is precisely what an add-on does.

#include "imvec_abi.hpp"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define API extern "C" __declspec(dllexport)

// ---------------------------------------------------------------------------------------------
// Returning an aggregate BY VALUE. 15 entries of the real table do this.
// ---------------------------------------------------------------------------------------------

// Shape of: ImVec2(*GetCursorScreenPos)(), GetWindowPos, GetContentRegionAvail, GetItemRectMin...
API ImVec2 cb_ret_vec2(void)
{
	return ImVec2(ABI_V2_X, ABI_V2_Y);
}

// Shape of: ImVec2(*CalcTextSize)(const char*, const char*, bool, float)
API ImVec2 cb_ret_vec2_args(const char *a, const char *b, bool c, float d)
{
	(void)a; (void)b; (void)c;
	return ImVec2(ABI_V2_X + d, ABI_V2_Y + d);
}

// Shape of: ImVec4(*ColorConvertU32ToFloat4)(ImU32). 16 bytes, so a different ABI rule applies.
API ImVec4 cb_ret_vec4(unsigned int in)
{
	(void)in;
	return ImVec4(ABI_V4_X, ABI_V4_Y, ABI_V4_Z, ABI_V4_W);
}

// ---------------------------------------------------------------------------------------------
// The SAFE substitutes and the ordinary widget shapes we intend to whitelist.
// ---------------------------------------------------------------------------------------------

// Shape of: const ImVec4&(*GetStyleColorVec4)(ImGuiCol). Returns a REFERENCE, i.e. a pointer.
static const ImVec4 g_style_col(ABI_V4_X, ABI_V4_Y, ABI_V4_Z, ABI_V4_W);
API const ImVec4 &cb_ret_vec4_ref(int idx)
{
	(void)idx;
	return g_style_col;
}

// Shape of: bool(*Checkbox)(const char* label, bool* v)
API bool cb_checkbox(const char *label, bool *v)
{
	if (label == nullptr || strcmp(label, "chk") != 0)
		return false;
	if (v == nullptr)
		return false;
	*v = !*v;
	return true;
}

// Shape of: bool(*SliderFloat)(const char*, float*, float, float, const char*, ImGuiSliderFlags)
// Mixed pointer/float/int argument slots: the interesting part is that a float argument occupies
// the XMM register of its POSITIONAL slot, leaving the matching integer register unused.
API bool cb_sliderfloat(const char *label, float *v, float v_min, float v_max, const char *fmt, int flags)
{
	if (label == nullptr || strcmp(label, "sld") != 0)
		return false;
	if (fmt == nullptr || strcmp(fmt, "%.3f") != 0)
		return false;
	if (v == nullptr || flags != 7)
		return false;
	// Report every scalar back through the out pointer so a mis-assigned register is visible.
	*v = v_min * 100.0f + v_max;
	return true;
}

// Shape of: bool(*Combo)(const char*, int*, const char* const[], int, int)
API bool cb_combo(const char *label, int *cur, const char *const items[], int count, int popup_max)
{
	if (label == nullptr || cur == nullptr || items == nullptr)
		return false;
	if (count != 3 || popup_max != -1)
		return false;
	if (strcmp(items[2], "c") != 0)
		return false;
	*cur = 2;
	return true;
}

// Shape of: void(*TextV)(const char* fmt, va_list args). va_list crosses the boundary.
static char g_textv[256];
API void cb_textv(const char *fmt, va_list args)
{
	vsnprintf(g_textv, sizeof(g_textv), fmt, args);
}
API const char *cb_textv_result(void)
{
	return g_textv;
}

// Shape of: void(*PlotLines)(..., ImVec2 graph_size, int stride) - an aggregate BY VALUE as a
// PARAMETER. Only the four Plot* entries do this.
API bool cb_take_vec2_byval(const char *label, ImVec2 sz, int stride)
{
	if (label == nullptr || strcmp(label, "plt") != 0)
		return false;
	return sz.x == ABI_V2_X && sz.y == ABI_V2_Y && stride == 42;
}

// The out-parameter form we would fall back to. A free function taking (T*) is RCX under both
// ABIs, so this is the shape that is safe by construction.
API void cb_out_vec2(ImVec2 *out)
{
	if (out != nullptr)
		*out = ImVec2(ABI_V2_X, ABI_V2_Y);
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
