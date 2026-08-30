// overlay_imgui.hpp - make the ImGui call path safe under BOTH mingw-w64 and MSVC.
//
// Include this (not <imgui.h>) before anything that draws. It owns three things:
//   1. binding imgui_function_table_instance(), which NOTHING else in this add-on does;
//   2. a hard gate so no widget is drawn until that binding succeeded;
//   3. the small set of helpers that keep the whitelist in the CI-verified subset.
//
// ---------------------------------------------------------------------------------------------
// WHY THE TABLE IS NOT ALREADY BOUND
//
//   reshade.hpp binds it inside reshade::register_addon(), but only under
//   `#if defined(IMGUI_VERSION_NUM)`, and this add-on does not call register_addon at all - it
//   calls ReShadeRegisterAddon directly through register_addon_strict() so that a version
//   mismatch is a clean refusal rather than a silent downgrade. So the table pointer stays
//   nullptr, and the first ImGui:: call would be
//       imgui_function_table_instance()->Checkbox(...)
//   i.e. a load through a null pointer, inside the game's present path. Bind it explicitly.
//
//   Binding here rather than switching to reshade::register_addon() is also deliberate: that
//   function returns FALSE when the ImGui table cannot be fetched, which would turn "ReShade was
//   built without add-on ImGui support" into "the add-on does not load at all". This project has
//   already lost a play session to an add-on that silently did not load. Failing to get the table
//   must cost the user the overlay, never the DLSS-NR work.
//
// ---------------------------------------------------------------------------------------------
// THE BY-VALUE RETURN HAZARD, MEASURED
//
//   imgui_function_table_19250 is a table of FUNCTION POINTERS. Fourteen of its entries return
//   ImVec2 by value. ImVec2 is 8 bytes and trivially copyable, but it has user-declared
//   constexpr constructors, and that is exactly where the two compilers' rules part:
//
//     MSVC   returns a class in RAX only if it is POD-like (no user-declared constructor),
//            so it returns ImVec2 through a HIDDEN POINTER IN RCX.
//     GCC    returns a class in RAX if it is <= 8 bytes and TRIVIALLY COPYABLE,
//            so it returns ImVec2 PACKED IN RAX and passes no hidden pointer at all.
//
//   Measured in CI (.github/workflows/build.yml, job "abi"), run 33289065674:
//
//     MSVC  cb_ret_vec2:  mov dword ptr [rcx],3FC00000h / mov rax,rcx / mov dword ptr [rcx+4],...
//     GCC   local_ret_vec2:  movabs $0x402000003fc00000,%rax / ret
//
//   So a mingw caller invoking ReShade's GetCursorScreenPos() supplies no hidden pointer, and
//   ReShade stores eight bytes through whatever RCX happened to hold. The probe does exactly that
//   and dies with exit code -1073741819 (0xC0000005, ACCESS_VIOLATION). Those fourteen entries are
//   therefore OFF LIMITS while the shipping binary is a mingw build, and a CI step greps src/ to
//   keep them out. ColorConvertU32ToFloat4 is NOT one of them: it returns ImVec4, which is 16
//   bytes, and the "size is not 1/2/4/8 -> return in memory" rule has no POD-ness clause, so both
//   toolchains agree. That too is measured, not assumed.
//
//   HOW THIS DIFFERS FROM src/msvc_abi.hpp. That header is about non-static MEMBER functions,
//   where both ABIs use a hidden return pointer but put it in DIFFERENT REGISTERS (MSVC:
//   this=RCX, hidden=RDX; Itanium: hidden=RCX, this=RDX). Here the register is not in dispute -
//   for a FREE function, which is what a function pointer is, both ABIs put a hidden return
//   pointer in RCX. What is in dispute is WHETHER THERE IS ONE AT ALL.
//
//   That difference is smaller than it looks, because the REMEDY is the same in both cases: give
//   the call a signature both ABIs agree on. An MSVC function returning ImVec2 by value compiles
//   to precisely the code for `ImVec2 *f(ImVec2 *out)` - destination in RCX, returned in RAX -
//   which is what the measured disassembly above shows. So redeclaring the table entry in that
//   shape and calling it that way is correct from either toolchain. CI proves it against the real
//   MSVC by-value exports (RESULT thunk_ret_vec2 / thunk_ret_vec2_args / thunk_stack_canary), for
//   both the no-argument shape and the CalcTextSize shape, under both compilers.
//
//   by_value_ret() below is that thunk. It is the reason the fourteen entries are merely OFF THE
//   DEFAULT PATH rather than permanently unreachable. The UI in this add-on needs none of them,
//   so the CI grep keeps direct calls out of src/ entirely; use the thunk only if a genuine
//   layout need appears, and add a probe case for the exact shape first.
//
// Everything else in the table takes ImVec2/ImVec4 by CONST REFERENCE (a pointer, so
// convention-free). The only by-value ImVec2 PARAMETERS are the four Plot* entries, which the
// probe shows both toolchains agree on anyway; we do not use them.

#pragma once

// ---------------------------------------------------------------------------------------------
// INCLUDE ORDER IS LOAD-BEARING. Read this before moving the #include of this file.
//
//   include/reshade_overlay.hpp has NO include guard and no #pragma once. It is guarded only by
//   the #pragma once on include/reshade.hpp, which includes it at line 9. Its entire body sits
//   inside `#if defined(IMGUI_VERSION_NUM)`.
//
//   So if reshade.hpp (or src/reshade_compat.hpp, which includes it) is pulled in BEFORE
//   <imgui.h>, reshade_overlay.hpp expands to NOTHING, and reshade.hpp's #pragma once then
//   prevents it from ever being reconsidered - including from this header. There would be no
//   namespace ImGui and no imgui_function_table_instance() anywhere in the translation unit.
//
//   This header must therefore be included BEFORE reshade_compat.hpp / reshade.hpp. In
//   src/stray_dlssnr.cpp that means above the existing `#include "reshade_compat.hpp"`.
//   The check below turns a confusing cascade of "ImGui has not been declared" errors into one
//   line that says what to do.
// ---------------------------------------------------------------------------------------------
#if defined(RESHADE_API_VERSION) && !defined(IMGUI_VERSION_NUM)
#error "overlay_imgui.hpp must be included BEFORE reshade.hpp / reshade_compat.hpp. reshade_overlay.hpp has no include guard and compiles to nothing unless <imgui.h> was seen first, and reshade.hpp's #pragma once means it never gets a second chance."
#endif

// imgui.h must come first: reshade_overlay.hpp is wrapped in `#if defined(IMGUI_VERSION_NUM)` and
// defines nothing at all without it. Pinned to ocornut/imgui 3912b3d9 == 1.92.5 == 19250, which is
// the submodule ReShade 6.8.0 is built against; reshade_overlay.hpp #errors on any other value.
#include <imgui.h>

#include "reshade_compat.hpp"

#include <cstdarg>
#include <cstdio>

namespace overlay_imgui {

// Set once, at load, from the same ReShade module handle register_addon_strict() used.
inline bool &bound_flag()
{
	static bool bound = false;
	return bound;
}

/// Bind ImGui's function table. Returns false if this ReShade build cannot supply one, in which
/// case the caller must skip reshade::register_overlay and run headless. Never fatal.
inline bool bind_table(HMODULE reshade_module)
{
	if (reshade_module == nullptr)
		return false;

	const auto get_table = reinterpret_cast<const imgui_function_table *(*)(uint32_t)>(
		GetProcAddress(reshade_module, "ReShadeGetImGuiFunctionTable"));
	if (get_table == nullptr)
		return false; // ReShade built without add-on ImGui support.

	// A version other than ours returns nullptr. That is the SILENT failure mode - the compile
	// time #error in reshade_overlay.hpp only checks OUR header against OUR imgui.h, it cannot
	// see the ReShade DLL actually loaded in the game.
	const imgui_function_table *const table = get_table(IMGUI_VERSION_NUM);
	if (table == nullptr)
		return false;

	imgui_function_table_instance() = table;
	bound_flag() = true;
	return true;
}

/// True once bind_table() has succeeded. Every draw path must check this first.
inline bool available()
{
	return bound_flag() && imgui_function_table_instance() != nullptr;
}

/// Formatted text without crossing the boundary with a va_list.
///
/// ImGui::Text() is measured safe - the probe passes a va_list from a mingw caller into an
/// MSVC callee and gets "42/ok/3.50" back, because x86_64-w64-mingw32 uses the MS varargs
/// convention where va_list is a plain char*. This helper exists anyway: formatting on our side
/// and handing over a finished const char* removes the question entirely, and costs one stack
/// buffer. Prefer it.
inline void textf(const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (n < 0)
		return;
	buf[sizeof(buf) - 1] = '\0';
	ImGui::TextUnformatted(buf);
}

/// Same, in a colour. Takes ImVec4 by const reference, as the table does.
inline void textf_colored(const ImVec4 &col, const char *fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (n < 0)
		return;
	buf[sizeof(buf) - 1] = '\0';
	ImGui::PushStyleColor(ImGuiCol_Text, col);
	ImGui::TextUnformatted(buf);
	ImGui::PopStyleColor(1);
}

// ---------------------------------------------------------------------------------------------
// The escape hatch for the fourteen by-value ImVec2 returns. NOT needed by the current UI.
//
// Call as:  ImVec2 p = overlay_imgui::by_value_ret(t->GetCursorScreenPos);
// where `t` is table(). The entry is redeclared as `ImVec2 *(ImVec2 *)`, the shape MSVC actually
// compiled it to, which both ABIs agree on. Measured in CI under both toolchains; see the header
// comment. Anything with ARGUMENTS needs its own overload and its own probe case first - do not
// generalise this with a variadic template on the strength of the no-arg measurement alone.
// ---------------------------------------------------------------------------------------------
inline ImVec2 by_value_ret(ImVec2 (*entry)())
{
	ImVec2 out(0.0f, 0.0f);
	reinterpret_cast<ImVec2 *(*)(ImVec2 *)>(reinterpret_cast<void *>(entry))(&out);
	return out;
}

/// The bound table, or nullptr. Only needed to reach an entry through by_value_ret().
inline const imgui_function_table *table()
{
	return available() ? imgui_function_table_instance() : nullptr;
}

} // namespace overlay_imgui

// ImTextureID must be 8 bytes or reshade_overlay.hpp's own static_assert fires; imconfig.h in this
// tree sets that. Re-stating the version here means a wrong imgui.h fails in OUR file with OUR
// message rather than 400 lines into a vendored header.
static_assert(IMGUI_VERSION_NUM == 19250,
              "reshade_overlay.hpp for ReShade 6.8 requires ImGui 1.92.5 (19250) exactly; a "
              "mismatch is negotiated at runtime by bind_table() but must not differ at compile time.");
