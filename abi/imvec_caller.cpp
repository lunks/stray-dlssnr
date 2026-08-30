// imvec_caller.cpp - built with BOTH MSVC and mingw-w64 g++. This EXE stands in for the add-on.
//
// It loads abi_callee.dll (always MSVC, standing in for ReShade) and calls through function
// pointers whose types are copied from imgui_function_table_19250. The MSVC build is the control:
// it MUST pass everything. The mingw build is the question being asked.
//
// The central trick is a discriminator that CANNOT itself corrupt memory. To learn whether a
// compiler returns ImVec2 through a hidden pointer or in RAX, we call the function through a
// deliberately mistyped `void(*)(ImVec2*)` pointer, handing it a real buffer:
//
//   * if the callee uses the hidden-pointer convention, the pointer we passed in RCX is exactly
//     the hidden return slot it expects, and our buffer is filled in;
//   * if the callee returns in RAX, it ignores RCX entirely and our buffer is untouched.
//
// Both outcomes are memory-safe, so the discriminator can be run unconditionally. Only once it
// says the two conventions AGREE do we perform an actual by-value call (mode "direct"), and that
// runs in its own process so a fault is recorded rather than hiding the other results.

#include "imvec_abi.hpp"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#  define NOINLINE __declspec(noinline)
#  define TOOLCHAIN "MSVC"
#else
#  define NOINLINE __attribute__((noinline))
#  define TOOLCHAIN "mingw-w64 g++"
#endif

// ---------------------------------------------------------------------------------------------
// Exact parameter/return types lifted from include/reshade_overlay.hpp.
// ---------------------------------------------------------------------------------------------
typedef ImVec2 (*fn_ret_vec2)(void);
typedef ImVec2 (*fn_ret_vec2_args)(const char *, const char *, bool, float);
typedef ImVec4 (*fn_ret_vec4)(unsigned int);
typedef const ImVec4 &(*fn_ret_vec4_ref)(int);
typedef bool (*fn_checkbox)(const char *, bool *);
typedef bool (*fn_sliderfloat)(const char *, float *, float, float, const char *, int);
typedef bool (*fn_combo)(const char *, int *, const char *const[], int, int);
typedef void (*fn_textv)(const char *, va_list);
typedef const char *(*fn_textv_result)(void);
typedef bool (*fn_take_vec2_byval)(const char *, ImVec2, int);
typedef void (*fn_out_vec2)(ImVec2 *);

// The mistyped view used by the discriminator.
typedef void (*fn_as_out)(ImVec2 *);

static int g_fail = 0;
static void report(const char *name, bool ok, const char *detail)
{
	printf("RESULT %-28s %-4s %s\n", name, ok ? "PASS" : "FAIL", detail);
	if (!ok)
		g_fail = 1;
}

// A function compiled by THIS toolchain with the same signature, so we can ask what convention
// this compiler emits for the callee side.
static NOINLINE ImVec2 local_ret_vec2(void)
{
	return ImVec2(ABI_V2_X, ABI_V2_Y);
}

// Returns true if calling `p` as void(*)(ImVec2*) fills the buffer, i.e. the callee reads a hidden
// return pointer out of RCX.
static bool uses_hidden_return_pointer(void *p)
{
	ImVec2 buf(-99.0f, -99.0f);
	fn_as_out f = (fn_as_out)p;
	f(&buf);
	return buf.x == ABI_V2_X && buf.y == ABI_V2_Y;
}

static HMODULE g_dll;
static void *sym(const char *n)
{
	void *p = (void *)GetProcAddress(g_dll, n);
	if (p == nullptr) {
		printf("RESULT %-28s FAIL missing export\n", n);
		g_fail = 1;
	}
	return p;
}

int main(int argc, char **argv)
{
	const char *mode = (argc > 1) ? argv[1] : "report";
	printf("== ABI probe: caller built with %s, mode=%s ==\n", TOOLCHAIN, mode);

	g_dll = LoadLibraryA("abi_callee.dll");
	if (g_dll == nullptr) {
		printf("RESULT load_callee               FAIL LoadLibraryA failed (%lu)\n", GetLastError());
		return 2;
	}

	// -----------------------------------------------------------------------------------------
	// 1. The discriminator. Safe under every outcome.
	// -----------------------------------------------------------------------------------------
	void *p_ret_vec2 = sym("cb_ret_vec2");
	if (p_ret_vec2 == nullptr)
		return 2;

	// Defeat any devirtualisation of the local call.
	static void *volatile p_local = (void *)&local_ret_vec2;

	const bool msvc_hidden  = uses_hidden_return_pointer(p_ret_vec2);
	const bool local_hidden = uses_hidden_return_pointer((void *)p_local);

	printf("INFO  ImVec2 return convention: MSVC callee=%s, %s caller-side=%s\n",
	       msvc_hidden ? "hidden-pointer(memory)" : "register(RAX)",
	       TOOLCHAIN,
	       local_hidden ? "hidden-pointer(memory)" : "register(RAX)");

	// Emitted as VERDICT, not RESULT: this is the question being ASKED, not a gating check.
	// The gating checks are the whitelist candidates below, which must pass on both toolchains.
	const bool agree = (msvc_hidden == local_hidden);
	printf("VERDICT imvec2_return_convention %s %s\n",
	       agree ? "AGREE" : "DISAGREE",
	       agree ? "a by-value ImVec2 return is ABI-correct here"
	             : "a by-value ImVec2 return is UNSAFE - it must go through an out-parameter");

	if (strcmp(mode, "report") != 0 && strcmp(mode, "direct") != 0) {
		printf("unknown mode\n");
		return 2;
	}

	// -----------------------------------------------------------------------------------------
	// 2. Everything that does NOT return an aggregate by value. These are the whitelist
	//    candidates, and they must pass under both toolchains.
	// -----------------------------------------------------------------------------------------
	{
		fn_checkbox f = (fn_checkbox)sym("cb_checkbox");
		bool v = false;
		const bool r = f && f("chk", &v);
		report("Checkbox_shape", r && v == true, r ? "returned true, flipped the bool" : "bad");
	}
	{
		fn_sliderfloat f = (fn_sliderfloat)sym("cb_sliderfloat");
		float v = 0.0f;
		const bool r = f && f("sld", &v, 0.25f, 8.0f, "%.3f", 7);
		// 0.25 * 100 + 8 == 33
		report("SliderFloat_shape", r && v == 33.0f, r ? "float args landed in the right slots" : "bad");
	}
	{
		fn_combo f = (fn_combo)sym("cb_combo");
		const char *items[] = { "a", "b", "c" };
		int cur = 0;
		const bool r = f && f("cmb", &cur, items, 3, -1);
		report("Combo_shape", r && cur == 2, r ? "string array + ints intact" : "bad");
	}
	{
		fn_textv f = (fn_textv)sym("cb_textv");
		fn_textv_result g = (fn_textv_result)sym("cb_textv_result");
		if (f && g) {
			// Exactly what ReShade's inline ImGui::Text() does: va_start here, consumed there.
			struct H { static void call(fn_textv fn, const char *fmt, ...) {
				va_list a; va_start(a, fmt); fn(fmt, a); va_end(a);
			} };
			H::call(f, "%d/%s/%.2f", 42, "ok", 3.5f);
			const char *s = g();
			report("TextV_va_list", strcmp(s, "42/ok/3.50") == 0, s);
		}
	}
	{
		fn_ret_vec4_ref f = (fn_ret_vec4_ref)sym("cb_ret_vec4_ref");
		if (f) {
			const ImVec4 &c = f(3);
			report("ImVec4_return_by_REFERENCE", c.x == ABI_V4_X && c.w == ABI_V4_W, "reference return");
		}
	}
	{
		// ImVec4 is 16 bytes. The MS x64 ABI returns any aggregate whose size is not 1/2/4/8 in
		// memory, and that rule has no POD-ness clause, so both compilers should agree here even
		// though they disagree about ImVec2. Worth proving rather than asserting.
		fn_ret_vec4 f = (fn_ret_vec4)sym("cb_ret_vec4");
		if (f) {
			ImVec4 c = f(0u);
			report("ImVec4_return_by_VALUE",
			       c.x == ABI_V4_X && c.y == ABI_V4_Y && c.z == ABI_V4_Z && c.w == ABI_V4_W,
			       "16-byte aggregate returned by value");
		}
	}
	{
		fn_take_vec2_byval f = (fn_take_vec2_byval)sym("cb_take_vec2_byval");
		const bool r = f && f("plt", ImVec2(ABI_V2_X, ABI_V2_Y), 42);
		report("ImVec2_PARAM_by_value", r, "8-byte aggregate passed by value (Plot* shape)");
	}
	{
		fn_out_vec2 f = (fn_out_vec2)sym("cb_out_vec2");
		ImVec2 v(-1.0f, -1.0f);
		if (f) f(&v);
		report("out_param_substitute", v.x == ABI_V2_X && v.y == ABI_V2_Y,
		       "the safe replacement for a by-value return");
	}

	// -----------------------------------------------------------------------------------------
	// 3. The real thing. Only in mode "direct", in its own process, with a stack canary either
	//    side so silent corruption is caught as well as a fault.
	// -----------------------------------------------------------------------------------------
	if (strcmp(mode, "direct") == 0) {
		printf("INFO  performing an ACTUAL by-value ImVec2 return call...\n");
		fflush(stdout);

		volatile unsigned long long canary[16];
		for (int i = 0; i < 16; ++i)
			canary[i] = 0xC0FFEE0000000000ULL | (unsigned long long)i;

		fn_ret_vec2 f = (fn_ret_vec2)p_ret_vec2;
		ImVec2 v = f();

		bool canary_ok = true;
		for (int i = 0; i < 16; ++i)
			if (canary[i] != (0xC0FFEE0000000000ULL | (unsigned long long)i))
				canary_ok = false;

		unsigned long long raw = 0;
		memcpy(&raw, &v, sizeof(v));
		char detail[160];
		snprintf(detail, sizeof(detail), "got (%g, %g) raw=0x%016llx expected (%g, %g) raw=0x%08x%08x",
		         (double)v.x, (double)v.y, raw, (double)ABI_V2_X, (double)ABI_V2_Y,
		         0x40200000u, 0x3fc00000u);
		report("direct_by_value_call", v.x == ABI_V2_X && v.y == ABI_V2_Y, detail);
		report("stack_canary_after_call", canary_ok, canary_ok ? "intact" : "CLOBBERED");

		{
			fn_ret_vec2_args g2 = (fn_ret_vec2_args)sym("cb_ret_vec2_args");
			if (g2) {
				ImVec2 v2 = g2("t", nullptr, false, 1.0f);
				report("direct_by_value_call_args",
				       v2.x == ABI_V2_X + 1.0f && v2.y == ABI_V2_Y + 1.0f, "CalcTextSize shape");
			}
		}
	}

	printf("== %s: %s ==\n", TOOLCHAIN, g_fail ? "FAILURES PRESENT" : "all checks passed");
	return g_fail;
}
