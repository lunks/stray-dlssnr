// remix_nvngx.cpp - the module every call into nvngx_dlssnr.dll is issued from.
//
// WHY THIS FILE EXISTS AT ALL
//   Every GATED export in the snippet resolves its CALLER's module from the return address and
//   requires "nvngx.dll" to appear in that module's path; anything else is rejected with
//   0xbad00002 (NVSDK_NGX_Result_FAIL_PlatformError, which the SDK documents only as "an error
//   occurred within the underlying platform"). Gated:
//
//     CreateFeature, CreateFeature1, GetFeatureRequirements, GetScratchBufferSize, Init_Ext,
//     Init_Ext2, PopulateParameters_Impl, ReleaseFeature, Shutdown, Shutdown1
//
//   Ungated: EvaluateFeature, GetFeatureDeviceExtensionRequirements,
//            GetFeatureInstanceExtensionRequirements.
//
//   Init_Ext and CreateFeature are both gated, so without this module the add-on cannot even
//   initialise. This module's FILENAME contains "nvngx.dll", so calls forwarded from here pass.
//
//   EvaluateFeature is forwarded through here anyway, even though it is ungated, so that the
//   whole feature lifetime is issued from one module identity.
//
//   It is REQUIRED IN PRACTICE, not a belt-and-braces extra. A resolve-time probe cannot detect
//   the gate, because GetProcAddress succeeds and only the calls fail - which is why the add-on
//   refuses to run without this module rather than "trying anyway and seeing".
//
// THE ONE LINE THAT MUST NEVER BE "CLEANED UP"
//   g_forwarded_call_count. Every forwarder is written as
//
//       const NgxResult result = g_fn(args...);
//       ++g_forwarded_call_count;
//       return result;
//
//   and NEVER as `return g_fn(args...);`. A tail call reuses THIS function's return address,
//   which puts the snippet's caller check straight back on whoever called us - the ReShade
//   add-on - and defeats the entire point of the module. The volatile store after the call is
//   what stops the compiler making that transformation; build.sh additionally passes
//   -fno-optimize-sibling-calls, and the build verifies `call` / no trailing `jmp` in the
//   disassembly of each forwarder.
//
// NO NGX HEADER DEPENDENCY
//   Every parameter forwarded here is either a 64-bit handle or a 32-bit enum, so opaque void* /
//   unsigned int is ABI-identical to the real declarations and this file compiles against
//   nothing but <windows.h>. That is deliberate: a shim that needed the SDK drop would be a
//   second place for the SDK-vs-snippet ABI split (NGX_SNIPPET_BUILD) to go wrong.
//
// D3D12 ARITIES, which differ from the Vulkan ones this shim was originally written for:
//   Init_Ext loses two parameters - there is no VkInstance and no VkPhysicalDevice, so it is
//   (appId, appDataPath, ID3D12Device*, sdkVersion, params). Shutdown1 takes an ID3D12Device*.
//   CreateFeature and EvaluateFeature take an ID3D12GraphicsCommandList*.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

typedef unsigned int NgxResult;
typedef unsigned int NgxVersion;
typedef unsigned int NgxFeature;

// NVSDK_NGX_Result_FAIL_NotInitialized. Returned when a forwarder was never resolved, so a
// missing export in the snippet surfaces as a clean NGX failure rather than a null call.
constexpr NgxResult kNgxResultFailNotInitialized = 0xbad00007u;

typedef NgxResult (__cdecl *PFN_Init_Ext)(unsigned long long, const wchar_t *, void *, NgxVersion, const void *);
typedef NgxResult (__cdecl *PFN_Shutdown1)(void *);
typedef NgxResult (__cdecl *PFN_CreateFeature)(void *, NgxFeature, void *, void **);
typedef NgxResult (__cdecl *PFN_ReleaseFeature)(void *);
typedef NgxResult (__cdecl *PFN_EvaluateFeature)(void *, const void *, const void *, void *);
typedef NgxResult (__cdecl *PFN_AllocateParameters)(void **);
typedef NgxResult (__cdecl *PFN_DestroyParameters)(void *);
typedef NgxResult (__cdecl *PFN_PopulateParameters_Impl)(void *);
typedef NgxResult (__cdecl *PFN_GetFeatureRequirements)(void *, const void *, void *);

PFN_Init_Ext                g_init_ext           = nullptr;
PFN_Shutdown1               g_shutdown1          = nullptr;
PFN_CreateFeature           g_create_feature     = nullptr;
PFN_ReleaseFeature          g_release_feature    = nullptr;
PFN_EvaluateFeature         g_evaluate_feature   = nullptr;
PFN_AllocateParameters      g_allocate_params    = nullptr;
PFN_DestroyParameters       g_destroy_params     = nullptr;
PFN_PopulateParameters_Impl g_populate_params    = nullptr;
PFN_GetFeatureRequirements  g_feature_reqs       = nullptr;

// Written after every forwarded call. The only purpose of this store is to keep the compiler from
// turning `return g_xxx(...)` into a tail call: a tail call reuses the caller's return address,
// which would put the snippet's caller check back on the ReShade add-on and defeat the entire
// point of this module.
volatile long g_forwarded_call_count = 0;

} // namespace

extern "C" {

// Handed the already-loaded nvngx_dlssnr.dll by the add-on, which has already probed the
// snippet's own export table. Resolving here rather than LoadLibrary'ing a second copy keeps the
// module identity and the version check in one place.
__declspec(dllexport) void __cdecl RemixNgxTrampoline_SetSnippet(void *snippet_module)
{
	HMODULE m = static_cast<HMODULE>(snippet_module);
	if (m == nullptr)
	{
		g_init_ext = nullptr; g_shutdown1 = nullptr; g_create_feature = nullptr;
		g_release_feature = nullptr; g_evaluate_feature = nullptr;
		g_allocate_params = nullptr; g_destroy_params = nullptr;
		g_populate_params = nullptr; g_feature_reqs = nullptr;
		return;
	}

	g_init_ext         = reinterpret_cast<PFN_Init_Ext>               (GetProcAddress(m, "NVSDK_NGX_D3D12_Init_Ext"));
	g_shutdown1        = reinterpret_cast<PFN_Shutdown1>              (GetProcAddress(m, "NVSDK_NGX_D3D12_Shutdown1"));
	g_create_feature   = reinterpret_cast<PFN_CreateFeature>          (GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature"));
	g_release_feature  = reinterpret_cast<PFN_ReleaseFeature>         (GetProcAddress(m, "NVSDK_NGX_D3D12_ReleaseFeature"));
	g_evaluate_feature = reinterpret_cast<PFN_EvaluateFeature>        (GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature"));
	g_allocate_params  = reinterpret_cast<PFN_AllocateParameters>     (GetProcAddress(m, "NVSDK_NGX_D3D12_AllocateParameters"));
	g_destroy_params   = reinterpret_cast<PFN_DestroyParameters>      (GetProcAddress(m, "NVSDK_NGX_D3D12_DestroyParameters"));
	g_populate_params  = reinterpret_cast<PFN_PopulateParameters_Impl>(GetProcAddress(m, "NVSDK_NGX_D3D12_PopulateParameters_Impl"));
	g_feature_reqs     = reinterpret_cast<PFN_GetFeatureRequirements> (GetProcAddress(m, "NVSDK_NGX_D3D12_GetFeatureRequirements"));
}

// ---- gated -------------------------------------------------------------------------------

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_Init_Ext(
	unsigned long long app_id, const wchar_t *app_data_path, void *device,
	NgxVersion sdk_version, const void *parameters)
{
	if (g_init_ext == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_init_ext(app_id, app_data_path, device, sdk_version, parameters);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_Shutdown1(void *device)
{
	if (g_shutdown1 == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_shutdown1(device);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_CreateFeature(
	void *cmd_list, NgxFeature feature_id, void *parameters, void **out_handle)
{
	if (g_create_feature == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_create_feature(cmd_list, feature_id, parameters, out_handle);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_ReleaseFeature(void *handle)
{
	if (g_release_feature == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_release_feature(handle);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_PopulateParameters_Impl(void *parameters)
{
	if (g_populate_params == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_populate_params(parameters);
	++g_forwarded_call_count;
	return result;
}

// Forwarded for completeness. NOTE: this is the export that enforces NGXMinimumDriverVersion and
// NGXGpuArchitecture, i.e. the very check the direct-load design exists to avoid. The add-on does
// NOT call it, and must not start: on an RTX 4090 with driver 610.43.02 it reports a patched
// DLSS-NR snippet as unsupported for a feature that in fact runs.
__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_GetFeatureRequirements(
	void *adapter, const void *discovery_info, void *out_supported)
{
	if (g_feature_reqs == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_feature_reqs(adapter, discovery_info, out_supported);
	++g_forwarded_call_count;
	return result;
}

// ---- ungated, forwarded anyway so the whole lifetime comes from one module ----------------

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_EvaluateFeature(
	void *cmd_list, const void *handle, const void *parameters, void *progress_callback)
{
	if (g_evaluate_feature == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_evaluate_feature(cmd_list, handle, parameters, progress_callback);
	++g_forwarded_call_count;
	return result;
}

// ---- parameter block ---------------------------------------------------------------------
//
// The snippet exports NEITHER of these on ANY backend - only PopulateParameters_Impl - so these
// two forwarders always return FAIL_NotInitialized in practice. They exist so that a future
// snippet build which does export them needs no change here, and so that the add-on's
// "does the SNIPPET have them?" probe (which deliberately asks the snippet, never this module)
// is not the only thing standing between a stub and a silent failure.

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_AllocateParameters(void **out_parameters)
{
	if (g_allocate_params == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_allocate_params(out_parameters);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_DestroyParameters(void *parameters)
{
	if (g_destroy_params == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_destroy_params(parameters);
	++g_forwarded_call_count;
	return result;
}

} // extern "C"


// =============================================================================================
// SLOT B - a SECOND, INDEPENDENT snippet routed through this same module.
//
// WHY: the caller gate is a property of the MODULE the call is issued from, and this module's
// filename is what satisfies it. But the forwarders above hold ONE set of function pointers, so
// calling RemixNgxTrampoline_SetSnippet twice would silently re-point DLSS-NR's own calls at the
// SR snippet - the shipping feature would break with no diagnostic whatsoever.
//
// So SR gets its own slot, its own pointers and its own exports. Slot A's pointers, exports and
// generated code are UNTOUCHED, which is what makes "dlss_sr = 0 is bit-identical to today" true
// on this side of the boundary as well: with SR off, RemixNgxTrampoline_SetSnippetB is never
// called, every g_b_* stays null, and not one instruction below ever executes.
//
// The forwarders are byte-identical in shape to slot A's, INCLUDING the volatile counter store
// after the call. Same rule, same reason: never `return g_b_xxx(...)`. A tail jump reuses the
// caller's return address and puts the snippet's "am I being called from nvngx.dll" check straight
// back on the ReShade add-on. build.sh and the MSVC CI job both disassemble these by name.
// =============================================================================================

namespace {

PFN_Init_Ext                g_b_init_ext         = nullptr;
PFN_Shutdown1               g_b_shutdown1        = nullptr;
PFN_CreateFeature           g_b_create_feature   = nullptr;
PFN_ReleaseFeature          g_b_release_feature  = nullptr;
PFN_EvaluateFeature         g_b_evaluate_feature = nullptr;
PFN_AllocateParameters      g_b_allocate_params  = nullptr;
PFN_DestroyParameters       g_b_destroy_params   = nullptr;
PFN_PopulateParameters_Impl g_b_populate_params  = nullptr;
PFN_GetFeatureRequirements  g_b_feature_reqs     = nullptr;

} // namespace

extern "C" {

__declspec(dllexport) void __cdecl RemixNgxTrampoline_SetSnippetB(void *snippet_module)
{
	HMODULE m = static_cast<HMODULE>(snippet_module);
	if (m == nullptr)
	{
		g_b_init_ext = nullptr; g_b_shutdown1 = nullptr; g_b_create_feature = nullptr;
		g_b_release_feature = nullptr; g_b_evaluate_feature = nullptr;
		g_b_allocate_params = nullptr; g_b_destroy_params = nullptr;
		g_b_populate_params = nullptr; g_b_feature_reqs = nullptr;
		return;
	}

	g_b_init_ext         = reinterpret_cast<PFN_Init_Ext>               (GetProcAddress(m, "NVSDK_NGX_D3D12_Init_Ext"));
	g_b_shutdown1        = reinterpret_cast<PFN_Shutdown1>              (GetProcAddress(m, "NVSDK_NGX_D3D12_Shutdown1"));
	g_b_create_feature   = reinterpret_cast<PFN_CreateFeature>          (GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature"));
	g_b_release_feature  = reinterpret_cast<PFN_ReleaseFeature>         (GetProcAddress(m, "NVSDK_NGX_D3D12_ReleaseFeature"));
	g_b_evaluate_feature = reinterpret_cast<PFN_EvaluateFeature>        (GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature"));
	g_b_allocate_params  = reinterpret_cast<PFN_AllocateParameters>     (GetProcAddress(m, "NVSDK_NGX_D3D12_AllocateParameters"));
	g_b_destroy_params   = reinterpret_cast<PFN_DestroyParameters>      (GetProcAddress(m, "NVSDK_NGX_D3D12_DestroyParameters"));
	g_b_populate_params  = reinterpret_cast<PFN_PopulateParameters_Impl>(GetProcAddress(m, "NVSDK_NGX_D3D12_PopulateParameters_Impl"));
	g_b_feature_reqs     = reinterpret_cast<PFN_GetFeatureRequirements> (GetProcAddress(m, "NVSDK_NGX_D3D12_GetFeatureRequirements"));
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_B_Init_Ext(
	unsigned long long app_id, const wchar_t *app_data_path, void *device,
	NgxVersion sdk_version, const void *parameters)
{
	if (g_b_init_ext == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_b_init_ext(app_id, app_data_path, device, sdk_version, parameters);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_B_Shutdown1(void *device)
{
	if (g_b_shutdown1 == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_b_shutdown1(device);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_B_CreateFeature(
	void *cmd_list, NgxFeature feature_id, void *parameters, void **out_handle)
{
	if (g_b_create_feature == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_b_create_feature(cmd_list, feature_id, parameters, out_handle);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_B_ReleaseFeature(void *handle)
{
	if (g_b_release_feature == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_b_release_feature(handle);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_B_PopulateParameters_Impl(void *parameters)
{
	if (g_b_populate_params == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_b_populate_params(parameters);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_B_GetFeatureRequirements(
	void *adapter, const void *discovery_info, void *out_supported)
{
	if (g_b_feature_reqs == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_b_feature_reqs(adapter, discovery_info, out_supported);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_B_EvaluateFeature(
	void *cmd_list, const void *handle, const void *parameters, void *progress_callback)
{
	if (g_b_evaluate_feature == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_b_evaluate_feature(cmd_list, handle, parameters, progress_callback);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_B_AllocateParameters(void **out_parameters)
{
	if (g_b_allocate_params == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_b_allocate_params(out_parameters);
	++g_forwarded_call_count;
	return result;
}

__declspec(dllexport) NgxResult __cdecl NVSDK_NGX_D3D12_B_DestroyParameters(void *parameters)
{
	if (g_b_destroy_params == nullptr)
		return kNgxResultFailNotInitialized;

	const NgxResult result = g_b_destroy_params(parameters);
	++g_forwarded_call_count;
	return result;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
	return TRUE;
}
