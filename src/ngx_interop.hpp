// ngx_interop.hpp - everything that talks to nvngx_dlssnr.dll.
//
// SCOPE: this header deliberately takes NO dependency on the NGX SDK drop and links against
// nothing. Not nvsdk_ngx_d.lib, not the driver's nvngx.dll. That is the entire point of the
// direct-load design: the snippet we ship is patched, and the driver's NGX runtime would
// Authenticode-verify it and then enforce NGXMinimumDriverVersion 615.00 / NGXGpuArchitecture
// Blackwell2 on top - neither of which this machine (RTX 4090, 610.43.02) satisfies, for a
// feature that in fact runs.
//
// Three consequences follow, and all three are implemented below rather than borrowed:
//   1. GetNGXResultAsString lives in nvsdk_ngx_d.lib. Hand-written switch instead.
//   2. NVSDK_NGX_Parameter_Set* live in nvsdk_ngx_d.lib. The vtable is called directly.
//   3. The snippet exports no AllocateParameters/DestroyParameters on any backend, and the SDK
//      fallback would drag the driver runtime back in. So the parameter block is OURS.
//
// ============================================================================================
// THE PARAMETER-BLOCK ABI, WHICH IS THE MOST DANGEROUS THING IN THIS FILE
// ============================================================================================
// NVSDK_NGX_Parameter is a pure-virtual C++ class with a 17-slot vtable (nvsdk_ngx_params.h:52-77).
// Eight Set overloads, eight Get overloads, Reset.
//
//   * MSVC lays out an OVERLOAD SET IN REVERSE DECLARATION ORDER. Recovered from the disassembly
//     of NVSDK_NGX_Parameter_SetD3d12Resource in nvsdk_ngx_d.lib, which is a thin vtable
//     dispatch: `mov rax,[rcx]` then `mov rbx,[rax+8]` - Set(const char*, ID3D12Resource*) is
//     declared SEVENTH and lands at slot ONE.
//   * GCC/clang targeting the Itanium ABI number an overload set in DECLARATION order.
//
// This add-on is cross-built with mingw-w64 g++, i.e. the Itanium ABI. Writing the class out as
// C++ virtuals here would put Set(ID3D12Resource*) at slot 6 - where the snippet expects
// Get(const char*, void**) - and the first resource bind would be a wild call with the wrong
// arity. So the vtable is BUILT BY HAND as an array of free functions in the MSVC order, and the
// object is a plain struct whose first member is the vtable pointer. That is correct under both
// toolchains and cannot silently drift.
//
// Parameter PASSING needs no fixing: every entry point below is (void* this, const char* name,
// scalar), which mingw and MSVC both pass as RCX / RDX / R8-or-XMM2 under the one Windows x64
// convention. The Get entry points return NVSDK_NGX_Result, a 32-bit enum, in EAX under both.
// Nothing here returns a class by value, which is the one case where the two ABIs diverge (see
// msvc_abi.hpp).
//
// ============================================================================================
// THE CALLER GATE
// ============================================================================================
// Every gated export in the snippet resolves its CALLER's module from the return address and
// requires "nvngx.dll" to be a substring of that module's path, else returns 0xbad00002
// (FAIL_PlatformError). Gated: CreateFeature, CreateFeature1, GetFeatureRequirements,
// GetScratchBufferSize, Init_Ext, Init_Ext2, PopulateParameters_Impl, ReleaseFeature, Shutdown,
// Shutdown1. Ungated: EvaluateFeature.
//
// So calls are routed through remix_nvngx.dll, whose filename contains that substring. Its
// forwarders must make a REAL call, never a tail jmp - a tail jmp reuses the caller's return
// address and puts the check straight back on this add-on. EvaluateFeature is forwarded too, even
// though it is ungated, so the whole feature lifetime is issued from one module identity.
//
// The trampoline is REQUIRED, not preferred. A resolve-time probe cannot detect the gate, because
// GetProcAddress succeeds and only the calls fail; so its absence is reported as a hard failure
// with the remedy in the log, and the add-on stays a no-op.

#pragma once

#include "reshade_compat.hpp"

#include <d3d12.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <mutex>
#include <unordered_map>

namespace ngx {

// ------------------------------------------------------------------------------------------
// Minimal NGX types. Values from nvsdk_ngx_defs.h; spelled out rather than included.
// ------------------------------------------------------------------------------------------
typedef uint32_t Result;

enum : Result
{
	Result_Success                       = 0x1,
	Result_Fail                          = 0xBAD00000u,
	Result_FAIL_FeatureNotSupported      = 0xBAD00001u,
	Result_FAIL_PlatformError            = 0xBAD00002u,
	Result_FAIL_FeatureAlreadyExists      = 0xBAD00003u,
	Result_FAIL_FeatureNotFound          = 0xBAD00004u,
	Result_FAIL_InvalidParameter         = 0xBAD00005u,
	Result_FAIL_ScratchBufferTooSmall    = 0xBAD00006u,
	Result_FAIL_NotInitialized           = 0xBAD00007u,
	Result_FAIL_UnsupportedInputFormat   = 0xBAD00008u,
	Result_FAIL_RWFlagMissing            = 0xBAD00009u,
	Result_FAIL_MissingInput             = 0xBAD0000Au,
	Result_FAIL_UnableToInitializeFeature= 0xBAD0000Bu,
	Result_FAIL_OutOfDate                = 0xBAD0000Cu,
	Result_FAIL_OutOfGPUMemory           = 0xBAD0000Du,
	Result_FAIL_UnsupportedFormat        = 0xBAD0000Eu,
	Result_FAIL_UnableToWriteToAppDataPath = 0xBAD0000Fu,
	Result_FAIL_UnsupportedParameter     = 0xBAD00010u,
	Result_FAIL_Denied                   = 0xBAD00011u,
	Result_FAIL_NotImplemented           = 0xBAD00012u,
};

// nvsdk_ngx_defs.h:178-179. Note SUCCESS is 0x1, not 0, and the test is on the high nibble field.
inline bool failed(Result r)  { return (r & 0xFFF00000u) == Result_Fail; }
inline bool succeeded(Result r) { return !failed(r); }

// NVSDK_NGX_VERSION_API_MACRO, nvsdk_ngx_defs.h:56 (NGX_VERSION_DOT 1.5.0).
static constexpr uint32_t kVersionApi = 0x0000015u;

// NGX feature id for DLSS Neural Rendering. NOT in the public SDK enum, and deliberately NOT
// written as NVSDK_NGX_Feature_Count (which happens to equal 18 today and would break silently
// the day NVIDIA adds a feature). Confirmed against nvngx_dlssnr.dll (DLSSNR 310.8.0): every
// *_GetFeatureRequirements builds its request with `mov dword [rsp+X], 0x12`.
static constexpr uint32_t kFeatureDLSSNR = 18u;

// The snippet ships exactly ONE network. Its weight registry is one entry wide and the accessor
// hardcodes `cmp rcx,1`; that entry declares preset 1, config "CC_SILVER_AARDWOLD". Any other
// value logs a fallback and loads the same weights. There is nothing to choose between, so no
// preset selector is exposed anywhere in this add-on.
static constexpr unsigned int kOnlyPreset = 1u;

inline const char *result_to_string(Result r)
{
	switch (r)
	{
	case Result_Success:                        return "Success";
	case Result_Fail:                           return "Fail";
	case Result_FAIL_FeatureNotSupported:       return "FAIL_FeatureNotSupported";
	case Result_FAIL_PlatformError:             return "FAIL_PlatformError (this is ALSO what the "
	                                                   "snippet's \"not called from NGX runtime\" "
	                                                   "caller check returns)";
	case Result_FAIL_FeatureAlreadyExists:      return "FAIL_FeatureAlreadyExists";
	case Result_FAIL_FeatureNotFound:           return "FAIL_FeatureNotFound";
	case Result_FAIL_InvalidParameter:          return "FAIL_InvalidParameter";
	case Result_FAIL_ScratchBufferTooSmall:     return "FAIL_ScratchBufferTooSmall";
	case Result_FAIL_NotInitialized:            return "FAIL_NotInitialized";
	case Result_FAIL_UnsupportedInputFormat:    return "FAIL_UnsupportedInputFormat (a bound "
	                                                   "resource has a DXGI format the feature "
	                                                   "will not take)";
	case Result_FAIL_RWFlagMissing:             return "FAIL_RWFlagMissing (DLSSNR.Output needs "
	                                                   "ALLOW_UNORDERED_ACCESS)";
	case Result_FAIL_MissingInput:              return "FAIL_MissingInput (Color/Depth/MVec/Output "
	                                                   "are all mandatory)";
	case Result_FAIL_UnableToInitializeFeature: return "FAIL_UnableToInitializeFeature";
	case Result_FAIL_OutOfDate:                 return "FAIL_OutOfDate";
	case Result_FAIL_OutOfGPUMemory:            return "FAIL_OutOfGPUMemory";
	case Result_FAIL_UnsupportedFormat:         return "FAIL_UnsupportedFormat";
	case Result_FAIL_UnableToWriteToAppDataPath:return "FAIL_UnableToWriteToAppDataPath";
	case Result_FAIL_UnsupportedParameter:      return "FAIL_UnsupportedParameter";
	case Result_FAIL_Denied:                    return "FAIL_Denied";
	case Result_FAIL_NotImplemented:            return "FAIL_NotImplemented";
	default:                                    return "unknown";
	}
}

// ------------------------------------------------------------------------------------------
// Parameter names. Read out of the snippet's own string table.
//
// NOTE THE SUBRECT SPELLING: "<Resource>SubrectBaseX", with NO dot before "Subrect". That is a
// genuine deviation from every other NGX feature (DLSS and DLSS-RR use
// "DLSS.Input.DiffuseAlbedo.Subrect.Base.X"). Do not "fix" it to match the SDK header style.
// ------------------------------------------------------------------------------------------
static constexpr const char *kParamColor       = "DLSSNR.Color";
static constexpr const char *kParamDepth       = "DLSSNR.Depth";
static constexpr const char *kParamMVec        = "DLSSNR.MVec";
static constexpr const char *kParamOutput      = "DLSSNR.Output";
static constexpr const char *kParamControlMask = "DLSSNR.ControlMask";

static constexpr const char *kParamWidth       = "DLSSNR.Width";
static constexpr const char *kParamHeight      = "DLSSNR.Height";
// INERT in this snippet build - neither string exists anywhere in nvngx_dlssnr.dll and
// CreateFeature reads only DLSSNR.Width/DLSSNR.Height. Written anyway, exactly as the working
// deployment does; never predicate behaviour on them.
static constexpr const char *kParamInputWidth  = "DLSSNR.InputWidth";
static constexpr const char *kParamInputHeight = "DLSSNR.InputHeight";
static constexpr const char *kParamEnabled     = "DLSSNR.Enabled";
static constexpr const char *kParamReset       = "DLSSNR.Reset";
static constexpr const char *kParamDepthInverted = "DLSSNR.DepthInverted";
static constexpr const char *kParamMVecScaleX  = "DLSSNR.MVecScaleX";
static constexpr const char *kParamMVecScaleY  = "DLSSNR.MVecScaleY";
// DEAD in this snippet build: three separate sites read it and then unconditionally store 1.0f
// over the result. Uniquely among the float parameters it has no "was it actually set" guard, so
// this write can never change anything. Written anyway, for parity with the working deployment.
static constexpr const char *kParamScalingRatio = "DLSSNR.ScalingRatio";
static constexpr const char *kParamRenderPreset = "DLSSNR.Hint.Render.Preset";
static constexpr const char *kParamUseAutoMask  = "DLSSNR.UseAutoMask";

static constexpr const char *kParamIntensity              = "DLSSNR.Intensity";
static constexpr const char *kParamLocalToneStrength      = "DLSSNR.LocalToneStrength";
static constexpr const char *kParamLocalStructureStrength = "DLSSNR.LocalStructureStrength";
static constexpr const char *kParamSkinStructureStrength  = "DLSSNR.SkinStructureStrength";
static constexpr const char *kParamStyle                  = "DLSSNR.Style";

// Generic NGX, nvsdk_ngx_defs.h:709-710, 758.
static constexpr const char *kParamCreationNodeMask       = "CreationNodeMask";
static constexpr const char *kParamVisibilityNodeMask     = "VisibilityNodeMask";
static constexpr const char *kParamFreeMemOnRelease       = "FreeMemOnReleaseFeature";

// The four Subrect suffixes for one resource, built ONCE and kept for the lifetime of the
// process: Set takes the name as a bare const char* and nothing in the ABI promises the callee
// copies it before returning.
struct resource_param_names
{
	std::string resource;
	std::string base_x, base_y, width, height;

	explicit resource_param_names(const char *name)
		: resource(name)
		, base_x(std::string(name) + "SubrectBaseX")
		, base_y(std::string(name) + "SubrectBaseY")
		, width (std::string(name) + "SubrectWidth")
		, height(std::string(name) + "SubrectHeight")
	{}
};

// ------------------------------------------------------------------------------------------
// The parameter block: a hand-laid-out NVSDK_NGX_Parameter.
// ------------------------------------------------------------------------------------------
struct parameter_block
{
	// MUST be the first member. This is the object the snippet receives as NVSDK_NGX_Parameter*.
	const void *const *vtable;

	struct value
	{
		enum kind_t : uint8_t { k_none, k_ull, k_float, k_double, k_uint, k_int, k_ptr } kind = k_none;
		unsigned long long ull = 0;
		float              f   = 0.0f;
		double             d   = 0.0;
		unsigned int       u   = 0;
		int                i   = 0;
		void              *p   = nullptr;
	};

	std::mutex mutex;
	std::unordered_map<std::string, value> map;

	parameter_block();
};

namespace detail {

// Every one of these is (void* this, const char* name, scalar) or (const void* this,
// const char* name, T* out). One Windows x64 convention, identical under both toolchains.

inline parameter_block *pb(void *self) { return static_cast<parameter_block *>(self); }
inline const parameter_block *pbc(const void *self) { return static_cast<const parameter_block *>(self); }

// NOEXCEPT ON PURPOSE, both of them.
//
// These two are the bodies of the vtable slots handed to nvngx_dlssnr.dll, so the snippet calls
// them DIRECTLY - and the snippet is MSVC-built C code with no unwind tables we can rely on.
// Letting a std::bad_alloc from the std::string construction or the unordered_map node insert
// propagate out of a Set would unwind through the snippet's frames, which is undefined behaviour.
// They are also called on the dispatch path AFTER the game's Dispatch has been re-issued, where
// an unwind would skip the state restore. So the allocation failure is contained here and turned
// into "the value was not recorded", which the Get side then reports as Result_Fail - exactly the
// answer the snippet's own `cmp eax,0xbad00000` fallback path is written for.
inline void store(void *self, const char *name, const parameter_block::value &v) noexcept
{
	if (self == nullptr || name == nullptr)
		return;
	try
	{
		parameter_block *b = pb(self);
		std::lock_guard<std::mutex> lock(b->mutex);
		b->map[std::string(name)] = v;
	}
	catch (...)
	{
		// Nothing sensible to do and nowhere safe to report it: the caller may be the snippet.
	}
}

inline bool load(const void *self, const char *name, parameter_block::value &out) noexcept
{
	if (self == nullptr || name == nullptr)
		return false;
	try
	{
		// const_cast only for the mutex; the map itself is read.
		parameter_block *b = const_cast<parameter_block *>(pbc(self));
		std::lock_guard<std::mutex> lock(b->mutex);
		const auto it = b->map.find(std::string(name));
		if (it == b->map.end())
			return false;
		out = it->second;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

// A numeric value stored through one Set overload must be readable through any numeric Get
// overload: the snippet is free to read DLSSNR.Width with Get(unsigned int*) or Get(int*), and
// the real NGX parameter map converts. Pointers are answered from ONE slot for all three pointer
// getters, exactly as recommended, so it cannot matter whether the snippet asks for
// ID3D12Resource** or void**.
inline unsigned long long as_ull(const parameter_block::value &v)
{
	switch (v.kind)
	{
	case parameter_block::value::k_ull:    return v.ull;
	case parameter_block::value::k_uint:   return v.u;
	case parameter_block::value::k_int:    return static_cast<unsigned long long>(static_cast<long long>(v.i));
	case parameter_block::value::k_float:  return static_cast<unsigned long long>(v.f);
	case parameter_block::value::k_double: return static_cast<unsigned long long>(v.d);
	case parameter_block::value::k_ptr:    return reinterpret_cast<unsigned long long>(v.p);
	default: return 0;
	}
}

inline double as_double(const parameter_block::value &v)
{
	switch (v.kind)
	{
	case parameter_block::value::k_ull:    return static_cast<double>(v.ull);
	case parameter_block::value::k_uint:   return static_cast<double>(v.u);
	case parameter_block::value::k_int:    return static_cast<double>(v.i);
	case parameter_block::value::k_float:  return static_cast<double>(v.f);
	case parameter_block::value::k_double: return v.d;
	default: return 0.0;
	}
}

inline void *as_ptr(const parameter_block::value &v)
{
	return (v.kind == parameter_block::value::k_ptr) ? v.p : nullptr;
}

// -------- Set (MSVC slots 0..7, i.e. REVERSE of the header's declaration order) --------

inline void set_voidptr(void *self, const char *name, void *val)
{
	parameter_block::value v; v.kind = parameter_block::value::k_ptr; v.p = val; store(self, name, v);
}
inline void set_d3d12(void *self, const char *name, ID3D12Resource *val)
{
	parameter_block::value v; v.kind = parameter_block::value::k_ptr; v.p = val; store(self, name, v);
}
inline void set_d3d11(void *self, const char *name, void *val)
{
	parameter_block::value v; v.kind = parameter_block::value::k_ptr; v.p = val; store(self, name, v);
}
inline void set_int(void *self, const char *name, int val)
{
	parameter_block::value v; v.kind = parameter_block::value::k_int; v.i = val; store(self, name, v);
}
inline void set_uint(void *self, const char *name, unsigned int val)
{
	parameter_block::value v; v.kind = parameter_block::value::k_uint; v.u = val; store(self, name, v);
}
inline void set_double(void *self, const char *name, double val)
{
	parameter_block::value v; v.kind = parameter_block::value::k_double; v.d = val; store(self, name, v);
}
inline void set_float(void *self, const char *name, float val)
{
	parameter_block::value v; v.kind = parameter_block::value::k_float; v.f = val; store(self, name, v);
}
inline void set_ull(void *self, const char *name, unsigned long long val)
{
	parameter_block::value v; v.kind = parameter_block::value::k_ull; v.ull = val; store(self, name, v);
}

// -------- Get (MSVC slots 8..15) --------
//
// WHAT AN ABSENT KEY MUST RETURN, AND WHY IT IS NOT FAIL_InvalidParameter.
//
// The snippet does not branch on "did this fail"; it branches on an EQUALITY test. Every
// parameter read in nvngx_dlssnr.dll is followed by `cmp eax,0xbad00000` and substitutes the
// snippet's own fallback when it matches (this is the same disassembly the defaults in
// addon_config.hpp are recovered from). 0xBAD00000 is NVSDK_NGX_Result_Fail, which is what a real
// NGX parameter map returns for a key that was never set.
//
// Returning FAIL_InvalidParameter (0xBAD00005) instead makes that compare miss, so the snippet
// takes the "the host supplied a value" branch for a parameter the host never supplied. That is
// harmless for the DLSSNR.* set - the add-on writes all of it, and the CreateFeature-time keys
// persist because reset() is never called - but it is live for every generic NGX capability or
// scratch key the snippet reads and this add-on does not write.
//
// So: Result_Fail for a miss, Result_FAIL_InvalidParameter ONLY for a null out-pointer, and the
// two conditions are tested separately so the distinction cannot be collapsed again by accident.
// (Note that failed() masks with 0xFFF00000, so both still read as "failed" to our own code.)
//
// *out IS DELIBERATELY LEFT UNTOUCHED ON A MISS, because that is what a real NGX parameter map
// does and the snippet was written and tested against one. Zeroing it instead would look tidier
// but would CLOBBER the pre-seeded default in the `T x = default; Get(name, &x);` pattern, and the
// pattern the disassembly actually shows - test the result, substitute an internal fallback - does
// not read the slot on a miss at all. Matching the reference implementation is the safe choice.

inline Result get_voidptr(const void *self, const char *name, void **out)
{
	parameter_block::value v;
	if (out == nullptr)
		return Result_FAIL_InvalidParameter;
	if (!load(self, name, v))
		return Result_Fail;   // a MISS, not an invalid argument - see the note above
	*out = as_ptr(v);
	return Result_Success;
}
inline Result get_d3d12(const void *self, const char *name, ID3D12Resource **out)
{
	parameter_block::value v;
	if (out == nullptr)
		return Result_FAIL_InvalidParameter;
	if (!load(self, name, v))
		return Result_Fail;   // a MISS, not an invalid argument - see the note above
	*out = static_cast<ID3D12Resource *>(as_ptr(v));
	return Result_Success;
}
inline Result get_d3d11(const void *self, const char *name, void **out)
{
	parameter_block::value v;
	if (out == nullptr)
		return Result_FAIL_InvalidParameter;
	if (!load(self, name, v))
		return Result_Fail;   // a MISS, not an invalid argument - see the note above
	*out = as_ptr(v);
	return Result_Success;
}
inline Result get_int(const void *self, const char *name, int *out)
{
	parameter_block::value v;
	if (out == nullptr)
		return Result_FAIL_InvalidParameter;
	if (!load(self, name, v))
		return Result_Fail;   // a MISS, not an invalid argument - see the note above
	*out = static_cast<int>(as_ull(v));
	return Result_Success;
}
inline Result get_uint(const void *self, const char *name, unsigned int *out)
{
	parameter_block::value v;
	if (out == nullptr)
		return Result_FAIL_InvalidParameter;
	if (!load(self, name, v))
		return Result_Fail;   // a MISS, not an invalid argument - see the note above
	*out = static_cast<unsigned int>(as_ull(v));
	return Result_Success;
}
inline Result get_double(const void *self, const char *name, double *out)
{
	parameter_block::value v;
	if (out == nullptr)
		return Result_FAIL_InvalidParameter;
	if (!load(self, name, v))
		return Result_Fail;   // a MISS, not an invalid argument - see the note above
	*out = as_double(v);
	return Result_Success;
}
inline Result get_float(const void *self, const char *name, float *out)
{
	parameter_block::value v;
	if (out == nullptr)
		return Result_FAIL_InvalidParameter;
	if (!load(self, name, v))
		return Result_Fail;   // a MISS, not an invalid argument - see the note above
	*out = static_cast<float>(as_double(v));
	return Result_Success;
}
inline Result get_ull(const void *self, const char *name, unsigned long long *out)
{
	parameter_block::value v;
	if (out == nullptr)
		return Result_FAIL_InvalidParameter;
	if (!load(self, name, v))
		return Result_Fail;   // a MISS, not an invalid argument - see the note above
	*out = as_ull(v);
	return Result_Success;
}

// -------- Reset (MSVC slot 16) --------
inline void reset(void *self)
{
	if (self == nullptr) return;
	parameter_block *b = pb(self);
	std::lock_guard<std::mutex> lock(b->mutex);
	b->map.clear();
}

// THE TABLE. Order is the MSVC vtable order recovered from nvsdk_ngx_d.lib's thin dispatchers;
// see the header comment. Editing this array is editing an ABI.
//
//   0  Set(const char*, void*)
//   1  Set(const char*, ID3D12Resource*)
//   2  Set(const char*, ID3D11Resource*)
//   3  Set(const char*, int)
//   4  Set(const char*, unsigned int)
//   5  Set(const char*, double)
//   6  Set(const char*, float)
//   7  Set(const char*, unsigned long long)
//   8  Get(const char*, void**)
//   9  Get(const char*, ID3D12Resource**)
//  10  Get(const char*, ID3D11Resource**)
//  11  Get(const char*, int*)
//  12  Get(const char*, unsigned int*)
//  13  Get(const char*, double*)
//  14  Get(const char*, float*)
//  15  Get(const char*, unsigned long long*)
//  16  Reset()
inline const void *const g_parameter_vtable[17] = {
	reinterpret_cast<const void *>(&set_voidptr),
	reinterpret_cast<const void *>(&set_d3d12),
	reinterpret_cast<const void *>(&set_d3d11),
	reinterpret_cast<const void *>(&set_int),
	reinterpret_cast<const void *>(&set_uint),
	reinterpret_cast<const void *>(&set_double),
	reinterpret_cast<const void *>(&set_float),
	reinterpret_cast<const void *>(&set_ull),
	reinterpret_cast<const void *>(&get_voidptr),
	reinterpret_cast<const void *>(&get_d3d12),
	reinterpret_cast<const void *>(&get_d3d11),
	reinterpret_cast<const void *>(&get_int),
	reinterpret_cast<const void *>(&get_uint),
	reinterpret_cast<const void *>(&get_double),
	reinterpret_cast<const void *>(&get_float),
	reinterpret_cast<const void *>(&get_ull),
	reinterpret_cast<const void *>(&reset),
};

} // namespace detail

inline parameter_block::parameter_block() : vtable(detail::g_parameter_vtable) {}

// Typed helpers for our own writes. EVERY call site casts explicitly, because overload selection
// here is silent and load-bearing: a bare 0 would be int (slot 3) not unsigned int (slot 4), and
// a bare 1.0 would be double (slot 5) not float (slot 6).
inline void set_u32   (parameter_block *p, const char *n, unsigned int v)   { detail::set_uint(p, n, v); }
inline void set_f32   (parameter_block *p, const char *n, float v)          { detail::set_float(p, n, v); }
inline void set_i32   (parameter_block *p, const char *n, int v)            { detail::set_int(p, n, v); }
inline void set_res   (parameter_block *p, const char *n, ID3D12Resource *v){ detail::set_d3d12(p, n, v); }

// ------------------------------------------------------------------------------------------
// The snippet.
// ------------------------------------------------------------------------------------------
typedef Result (__cdecl *PFN_Init_Ext)(unsigned long long app_id, const wchar_t *app_data_path,
                                       ID3D12Device *device, uint32_t sdk_version, const void *params);
typedef Result (__cdecl *PFN_Shutdown1)(ID3D12Device *device);
typedef Result (__cdecl *PFN_CreateFeature)(ID3D12GraphicsCommandList *cmd_list, uint32_t feature_id,
                                            void *params, void **out_handle);
typedef Result (__cdecl *PFN_ReleaseFeature)(void *handle);
typedef Result (__cdecl *PFN_EvaluateFeature)(ID3D12GraphicsCommandList *cmd_list, const void *handle,
                                              const void *params, void *progress_callback);
typedef Result (__cdecl *PFN_PopulateParameters_Impl)(void *params);
typedef void   (__cdecl *PFN_SetSnippet)(void *snippet_module);

struct snippet
{
	HMODULE snippet_module    = nullptr;
	HMODULE trampoline_module = nullptr;

	PFN_Init_Ext                init_ext         = nullptr;
	PFN_Shutdown1               shutdown1        = nullptr;
	PFN_CreateFeature           create_feature   = nullptr;
	PFN_ReleaseFeature          release_feature  = nullptr;
	PFN_EvaluateFeature         evaluate_feature = nullptr;
	PFN_PopulateParameters_Impl populate_params  = nullptr;

	bool        available = false;
	std::string not_available_reason;
	std::wstring directory;   // where the snippet and trampoline were found (with trailing sep)

	void unload()
	{
		init_ext = nullptr; shutdown1 = nullptr; create_feature = nullptr;
		release_feature = nullptr; evaluate_feature = nullptr; populate_params = nullptr;
		if (trampoline_module != nullptr) { FreeLibrary(trampoline_module); trampoline_module = nullptr; }
		if (snippet_module    != nullptr) { FreeLibrary(snippet_module);    snippet_module = nullptr; }
		available = false;
	}
};

inline std::wstring module_directory_of(const void *address_inside)
{
	HMODULE mod = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCWSTR>(address_inside), &mod))
		return std::wstring();

	wchar_t path[MAX_PATH + 1] = {};
	const DWORD len = GetModuleFileNameW(mod, path, MAX_PATH);
	if (len == 0 || len > MAX_PATH)
		return std::wstring();

	std::wstring full(path, len);
	const size_t sep = full.find_last_of(L"\\/");
	if (sep == std::wstring::npos)
		return std::wstring();
	return full.substr(0, sep + 1);
}

inline std::string narrow(const std::wstring &w)
{
	if (w.empty())
		return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	if (n <= 0)
		return std::string();
	std::string out(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n, nullptr, nullptr);
	return out;
}

// ------------------------------------------------------------------------------------------
// WHICH SNIPPET, AND THROUGH WHICH TRAMPOLINE SLOT.
//
// There are now two snippets in play and they must not be crossed:
//
//   DLSS-NR   nvngx_dlssnr.dll, NGX feature 18, ships today, the user plays on it.
//   DLSS-SR   nvngx_dlss.dll,   NGX feature 1.
//
// The caller gate is a property of the MODULE a call is ISSUED from, so both go through
// remix_nvngx.dll - but that module holds ONE set of forwarding pointers per slot, and calling
// RemixNgxTrampoline_SetSnippet twice would silently re-point DLSS-NR's own calls at the SR
// snippet. So the trampoline has two independent slots and each snippet claims one:
//
//   slot A   RemixNgxTrampoline_SetSnippet    exports NVSDK_NGX_D3D12_*      (DLSS-NR)
//   slot B   RemixNgxTrampoline_SetSnippetB   exports NVSDK_NGX_D3D12_B_*    (DLSS-SR)
//
// The SNIPPET side of the resolve always uses the real "NVSDK_NGX_D3D12_" names - the prefix is a
// property of the TRAMPOLINE's exports only.
struct snippet_spec
{
    const wchar_t *dll_name           = L"nvngx_dlssnr.dll";
    const char    *set_snippet_export = "RemixNgxTrampoline_SetSnippet";
    const char    *trampoline_prefix  = "NVSDK_NGX_D3D12_";
    // Names the feature in every diagnostic this loader emits, so the two never read alike.
    const char    *label              = "DLSS-NR";
};

inline snippet_spec spec_dlssnr()
{
    return snippet_spec{};
}

inline snippet_spec spec_dlsssr()
{
    snippet_spec s;
    s.dll_name           = L"nvngx_dlss.dll";
    s.set_snippet_export = "RemixNgxTrampoline_SetSnippetB";
    s.trampoline_prefix  = "NVSDK_NGX_D3D12_B_";
    s.label              = "DLSS-SR";
    return s;
}

// Loads the named snippet from the given directory and resolves the D3D12 surface through
// remix_nvngx.dll. Returns false with 'not_available_reason' filled in; the caller must treat
// that as FEATURE ABSENT, never as an error - a stock install has no snippet and must run
// completely untouched.
inline bool load_snippet(snippet &s, const std::wstring &directory, const snippet_spec &spec,
                         bool require_trampoline)
{
    s.directory = directory;

    if (directory.empty())
    {
        s.not_available_reason = "unable to determine the add-on's own module directory";
        return false;
    }

    const std::string dll_name_utf8 = narrow(std::wstring(spec.dll_name));

    const std::wstring snippet_path = directory + spec.dll_name;
    s.snippet_module = LoadLibraryW(snippet_path.c_str());
    if (s.snippet_module == nullptr)
    {
        s.not_available_reason = dll_name_utf8 + " was not found next to the add-on (" +
            narrow(directory) + "). This is the expected state for a stock install.";
        return false;
    }

    // Probe the SNIPPET'S OWN export table, not whatever module the calls end up going through.
    // The trampoline exports these names unconditionally, so resolving through it would report a
    // truncated or Vulkan-only snippet as perfectly fine.
    static const char *const kRequired[] = {
        "NVSDK_NGX_D3D12_Init_Ext",
        "NVSDK_NGX_D3D12_Shutdown1",
        "NVSDK_NGX_D3D12_CreateFeature",
        "NVSDK_NGX_D3D12_ReleaseFeature",
        "NVSDK_NGX_D3D12_EvaluateFeature",
    };
    for (const char *name : kRequired)
    {
        if (GetProcAddress(s.snippet_module, name) == nullptr)
        {
            s.not_available_reason = dll_name_utf8 + std::string(" does not export ") + name +
                ". This build of the snippet has no D3D12 backend.";
            s.unload();
            return false;
        }
    }

    const std::wstring trampoline_path = directory + L"remix_nvngx.dll";
    s.trampoline_module = LoadLibraryW(trampoline_path.c_str());

    HMODULE call_module = s.snippet_module;
    bool    slot_missing = false;
    if (s.trampoline_module != nullptr)
    {
        const auto set_snippet = reinterpret_cast<PFN_SetSnippet>(
            GetProcAddress(s.trampoline_module, spec.set_snippet_export));
        if (set_snippet != nullptr)
        {
            set_snippet(s.snippet_module);
            call_module = s.trampoline_module;
        }
        else
        {
            // The trampoline is present but does not carry THIS slot. That is an OUT-OF-DATE
            // remix_nvngx.dll, not a missing one, and saying so is the difference between a
            // 30-second fix and an afternoon: slot B was added when DLSS-SR was, so a trampoline
            // built before that has slot A only and DLSS-NR keeps working while DLSS-SR cannot.
            slot_missing = true;
            // Do NOT FreeLibrary here: slot A may already be live in this same module.
            s.trampoline_module = nullptr;
        }
    }

    if (call_module == s.snippet_module && require_trampoline)
    {
        s.not_available_reason = slot_missing
            ? (std::string("remix_nvngx.dll is present but does not export ") + spec.set_snippet_export +
               ", so it is an OUT-OF-DATE trampoline. Rebuild and redeploy remix_nvngx.dll from this "
               "tree - the second snippet slot was added alongside " + spec.label + ", and a "
               "trampoline built before that carries slot A (DLSS-NR) only. DLSS-NR is unaffected.")
            : (std::string("remix_nvngx.dll is missing or does not export ") + spec.set_snippet_export +
               ". It is REQUIRED: every gated snippet export resolves its caller's module from the "
               "return address and rejects anything whose path does not contain \"nvngx.dll\" with "
               "0xbad00002, and Init_Ext and CreateFeature are both gated. A resolve-time probe "
               "cannot detect this, because GetProcAddress succeeds and only the calls fail. Ship "
               "remix_nvngx.dll next to the add-on, or set require_trampoline=0 to try anyway.");
        s.unload();
        return false;
    }

    // The trampoline's export name carries the slot prefix; the snippet's never does.
    const auto resolve = [&](const char *base) -> FARPROC {
        FARPROC a = nullptr;
        if (call_module != s.snippet_module)
            a = GetProcAddress(call_module, (std::string(spec.trampoline_prefix) + base).c_str());
        if (a == nullptr)
            a = GetProcAddress(s.snippet_module, (std::string("NVSDK_NGX_D3D12_") + base).c_str());
        return a;
    };

    s.init_ext         = reinterpret_cast<PFN_Init_Ext>        (resolve("Init_Ext"));
    s.shutdown1        = reinterpret_cast<PFN_Shutdown1>       (resolve("Shutdown1"));
    s.create_feature   = reinterpret_cast<PFN_CreateFeature>   (resolve("CreateFeature"));
    s.release_feature  = reinterpret_cast<PFN_ReleaseFeature>  (resolve("ReleaseFeature"));
    s.evaluate_feature = reinterpret_cast<PFN_EvaluateFeature> (resolve("EvaluateFeature"));

    // Only claimed when the SNIPPET has it: the trampoline exports the name whether or not it
    // could forward it.
    if (GetProcAddress(s.snippet_module, "NVSDK_NGX_D3D12_PopulateParameters_Impl") != nullptr)
        s.populate_params = reinterpret_cast<PFN_PopulateParameters_Impl>(resolve("PopulateParameters_Impl"));

    if (s.init_ext == nullptr || s.shutdown1 == nullptr || s.create_feature == nullptr ||
        s.release_feature == nullptr || s.evaluate_feature == nullptr)
    {
        s.not_available_reason = "the resolved module does not export the full NVSDK_NGX_D3D12_* surface";
        s.unload();
        return false;
    }

    s.available = true;
    return true;
}

} // namespace ngx
