// imvec_abi.hpp - types shared by the ABI probe's callee (MSVC, plays ReShade) and its caller
// (built by BOTH toolchains, plays the add-on).
//
// ImVec2 / ImVec4 are copied VERBATIM from Dear ImGui 1.92.5 (imgui.h:300-323), the version
// ReShade 6.8.0 is built against. The user-provided constexpr constructors and operator[] are not
// incidental - they are the entire reason this test exists. MSVC returns a class in RAX only if it
// is POD-like (no user-declared constructor); GCC returns it in RAX if it is TRIVIALLY COPYABLE.
// ImVec2 is 8 bytes, trivially copyable, AND has user-declared constructors, so it lands on
// opposite sides of the two rules. Do not "simplify" these declarations.
#pragma once

#include <stddef.h>
#include <stdarg.h>

struct ImVec2
{
	float x, y;
	constexpr ImVec2() : x(0.0f), y(0.0f) { }
	constexpr ImVec2(float _x, float _y) : x(_x), y(_y) { }
	float &operator[](size_t idx) { return ((float *)(void *)(char *)this)[idx]; }
	float  operator[](size_t idx) const { return ((const float *)(const void *)(const char *)this)[idx]; }
};

struct ImVec4
{
	float x, y, z, w;
	constexpr ImVec4() : x(0.0f), y(0.0f), z(0.0f), w(0.0f) { }
	constexpr ImVec4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) { }
};

// Sentinels. Chosen so every byte differs and so a pointer reinterpreted as two floats is
// obviously wrong rather than plausibly right.
#define ABI_V2_X 1.5f
#define ABI_V2_Y 2.5f
#define ABI_V4_X 1.25f
#define ABI_V4_Y 2.25f
#define ABI_V4_Z 3.25f
#define ABI_V4_W 4.25f
