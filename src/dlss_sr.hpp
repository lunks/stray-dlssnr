// dlss_sr.hpp - DLSS Super Resolution (NGX feature 1, nvngx_dlss.dll) for STRAY.
//
// SELF-CONTAINED, in the same sense hdr_codec.hpp and mvec_decode.hpp are: everything that is
// specific to the SR snippet lives here - the feature id, the parameter names, the create/evaluate
// contracts, the SR-owned textures, the jitter source and the failure interpretation. The host
// (stray_dlssnr.cpp) supplies the resources it has already resolved and calls four functions.
// Nothing in here is reachable unless the host's `dlss_sr` ini key is 1.
//
// It DUPLICATES NOTHING. Motion vectors come from mvec_decode.hpp's pipeline (this header owns
// only the render-extent target texture it writes into, because SR's guide lives on the RENDER
// grid while NR's lives on the OUTPUT grid). Jitter comes from ue4_jitter.hpp, unchanged, through
// the thin per-frame wrapper at the bottom of this file.
//
// ============================================================================================
// WHERE THE NAMES AND NUMBERS COME FROM
// ============================================================================================
// Every parameter name below was verified by this project, against the shipped binary, as an
// EXACTLY NUL-DELIMITED string occurring exactly once in nvngx_dlss.dll:
//
//     python3 -c "d=open('nvngx_dlss.dll','rb').read(); print(d.count(b'\0DLSS.Use.HW.Depth\0'))"
//
// [SRC nvngx_dlss.dll 310.8.0.0, sha256 c85f971ce023c9f3492fc7455f0b01a24ba18ea39636407a846902c
// 4360b0b7e, PDB .../snippets/rel_310_8/source/features/dlaa/...]. All 63 names this add-on can
// emit were checked that way, and all 63 are present. THE SDK HEADERS WERE NOT USED AS A SOURCE
// FOR ANY NAME - that is the discipline that recovered the DLSSNR.* set, and it is the one that
// catches NR's undotted `DLSSNRColorSubrectBaseX` quirk, which does NOT carry over: SR spells
// every subrect with dots. Do not "fix" either spelling to match the other.
//
// The behavioural claims are from disassembly of the same binary, and each is cited at its use.
// The two most expensive-if-wrong:
//
//   * Jitter.Offset.X / Jitter.Offset.Y are UNCONDITIONALLY REQUIRED at Evaluate. They sit in the
//     same hard-gated block as Color/MotionVectors/Depth/Output, with no flag, mode or feature-id
//     guard, and a miss returns FAIL_InvalidParameter (0xBAD00005) [SRC dlaa.cpp:798/799 via
//     EvaluateFeature 0x18003b50f / 0x18003b53d]. There is no "run without jitter" configuration.
//
//   * DLSS.Use.HW.Depth is read at CREATE and never at evaluate; it is latched into the creation
//     struct at +0x24 and its ABSENT branch stores 0 = Linear [SRC Get at 0x18003e9b4, miss ->
//     r14d zeroed at 0x18003e7e2; consumed 0x18003a68a -> instance +0x19c2 -> network config key
//     "HW_Depth" at 0x180089203]. STRAY's t0 is a r32_g8_typeless HARDWARE depth-stencil, so the
//     correct value is 1, setting it at Evaluate is a NO-OP, and getting it wrong produces NO
//     diagnostic of any kind. It is set explicitly here, at create, always.
//
// ============================================================================================
// THE PRESET IS CHOSEN BY RATIO, NOT BY PerfQualityValue
// ============================================================================================
// FillCreationParams computes ratio = (float)Width / (float)OutWidth and selects the render-preset
// hint slot from THAT, with thresholds 0.40 / 0.54 / 0.62 / 0.70 / 0.99 [SRC 0x18003ea8a-
// 0x18003eafd against the constants at 0x18012cf38, 0x18012f140..0x18012f14c]. So at 1920->3840
// (ratio 0.50) it is DLSS.Hint.Render.Preset.Performance that takes effect, whatever
// PerfQualityValue says. preset_key_for_ratio() below is that table, and the host logs which key
// it wrote so the two can never silently disagree.
//
// ============================================================================================
// PARAMETERS THAT ARE PRESENT, READ, AND THEN DISCARDED - do not build on them
// ============================================================================================
//   Sharpness       read with default 0.5f, then UNCONDITIONALLY overwritten from the per-app
//                   config field +0x3c whose constructor default is 0.0f [SRC read 0x18003b5ad,
//                   overwrite 0x18003d043, default 0x1800406a4].
//   DoSharpening    create-flag bit 5, stripped because the app config's sharpening-mode field
//                   +0x44 defaults to 2 = "clear the bit and warn" [SRC 0x1800406ae, 0x18003ac35].
//   DLSS.Pre.Exposure / DLSS.Exposure.Scale
//                   both forced to 1.0f whenever IsHDR (create-flag bit 0) is CLEAR or the value
//                   is <= 0 [SRC 0x18003cc47 / 0x18003cc51 / 0x18003cc62]. So the exposure
//                   contract is coupled to a CREATE-time flag, which is not documented anywhere.
//                   BUT DO NOT READ THAT AS "IsHDR IS THE EXPOSURE GATE" - see the note below;
//                   this coupling is a side effect, not what the flag is for. It also means
//                   setting IsHDR costs nothing here: neither key is written by this header and
//                   the snippet's own miss-default for both is 1.0f
//                   [SRC 0x18003ca9d / 0x18003cac4].
// None of them is written by this header.
//
// ============================================================================================
// WHAT IsHDR ACTUALLY DOES - it selects a NETWORK
// ============================================================================================
// IsHDR is a discriminator on the trained kernel, in exactly the same class as DepthInverted and
// MVLowRes, and not a hint about exposure:
//   * paired _hdr_/_ldr_ CUDA kernels exist for every other combination -
//     hiluma_engine_{input,output}_depth{inv,reg}_mv{lo,hi}_{hdr,ldr}_v{1,2}_rel and the
//     _max_v2_ variants, 44 names at file offsets 0x12f9f8-0x1301b8, plus
//     cuda_engine_{input,output}_kernel_rel_{hdr,ldr}_* at 0x12f710-0x12f970;
//   * the loader stores a descriptor word at [rbp+0x38] immediately before passing each name.
//     For the INPUT set the bytes are, LSB first, {v2, IsHDR, MVLowRes, DepthInverted} -
//     depthinv_mvlo_hdr_v2 = 0x01010101, depthinv_mvlo_ldr_v2 = 0x01010001,
//     depthreg_mvhi_ldr_v2 = 0x00000001 [SRC .text 0x18004f87d-0x18004fc2e]. The OUTPUT set
//     spends the low two bytes on {max, v2} and carries DepthInverted in the byte at [rbp+0x3c]
//     instead - 1 for every depthinv_ name, r12b (zeroed at 0x18004ee0b) for every depthreg_ one
//     [SRC .text 0x18004fc64-0x18005082f];
//   * CreateDlssInstance prints the three together: "HDR %d / Motion Vectors LowRes %d / Motion
//     Vectors Jittered %d / Depth Inverted %d" [SRC 0x12dfe0-0x12e098].
// So it must be set from the real property of the colour buffer being bound, the same way the
// other two are. Getting it wrong runs an out-of-distribution network and returns Success.
//
// ============================================================================================
// APP ID
// ============================================================================================
// There is no app-id gate. Two ids DO trigger hardcoded per-game hacks on the SR path and must be
// avoided: 100233611 "Control" forces DoSharpening on AND multiplies Jitter.Offset.X by
// renderWidth * 0.5 [SRC 0x18003abec, 0x18003d067-0x18003d08b] - which would destroy the jitter -
// and 100323611 "F1 2020" fetches TransparencyMask [SRC 0x18003cadb]. The add-on's existing
// app_id 0x24480451 (608470609) appears NOWHERE in the binary, raw or XOR-obfuscated (full-file
// byte scan), so it collides with neither and receives no app-specific behaviour. Keep it.

#pragma once

#include "reshade_compat.hpp"
#include "ngx_interop.hpp"
#include "ue4_jitter.hpp"

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dlss_sr {

using namespace reshade::api;

// Same three levels hdr_codec and mvec_decode use, so one log functor serves all three.
enum { log_info = 0, log_warn = 1, log_error = 2 };

// =============================================================================================
// Feature id, flags and enums
// =============================================================================================

// NVSDK_NGX_Feature_SuperSampling. Deliberately spelled as a literal rather than borrowed from an
// SDK enum, exactly as kFeatureDLSSNR is. Corroborated in the binary: CreateFeature branches on
// `cmp dword [rsp+0x64], 0xd` for RayReconstruction (13) and the DLAA/SuperSampling path is the
// fall-through [SRC 0x18003ac4e].
static constexpr uint32_t kFeatureSuperSampling = 1u;

// DLSS.Feature.Create.Flags. The bit<->name binding is MEASURED, not assumed: CreateDlssInstance
// shifts and masks each bit immediately before printing its own name for it
// [SRC 0x18003a338-0x18003a449, dlaa.cpp:2127-2133].
static constexpr uint32_t kFlagIsHDR          = 1u << 0;  // NETWORK SELECTOR; also gates Pre.Exposure/Exposure.Scale
static constexpr uint32_t kFlagMVLowRes       = 1u << 1;
static constexpr uint32_t kFlagMVJittered     = 1u << 2;
static constexpr uint32_t kFlagDepthInverted  = 1u << 3;
// bit 4 is never read anywhere in the binary.
static constexpr uint32_t kFlagDoSharpening   = 1u << 5;  // INERT - stripped at create
static constexpr uint32_t kFlagAutoExposure   = 1u << 6;
static constexpr uint32_t kFlagAlphaUpscaling = 1u << 7;

// PerfQualityValue. Read out of the name/value array the snippet builds at 0x18003a454, so these
// are the snippet's own numbers rather than the SDK header's.
enum perf_quality : uint32_t
{
	perf_max_perf           = 0,   // the DEFAULT when the parameter is absent - a trap for DLAA
	perf_balanced           = 1,
	perf_max_quality        = 2,
	perf_ultra_performance  = 3,
	perf_ultra_quality      = 4,
	perf_dlaa               = 5,
};

inline const char *perf_quality_name(uint32_t v)
{
	switch (v)
	{
	case perf_max_perf:          return "MaxPerf (0)";
	case perf_balanced:          return "Balanced (1)";
	case perf_max_quality:       return "MaxQuality (2)";
	case perf_ultra_performance: return "UltraPerformance (3)";
	case perf_ultra_quality:     return "UltraQuality (4)";
	case perf_dlaa:              return "DLAA (5)";
	default:                     return "OUT OF RANGE - CreateFeature will reject this";
	}
}

// =============================================================================================
// Parameter names. Every one verified NUL-delimited and unique in nvngx_dlss.dll (see the header
// comment). Grouped exactly as the snippet groups them, with the create/evaluate split marked -
// getting that split wrong is the "the flag does not work" failure (see DLSS.Use.HW.Depth).
// =============================================================================================

// ---- CreateFeature: HARD-REQUIRED. A miss returns FAIL_InvalidParameter and logs
// "error: could not find %s parameter" [SRC FillCreationParams 0x18003e760, dlaa.cpp:1844-1847].
static constexpr const char *kParamWidth     = "Width";      // RENDER width
static constexpr const char *kParamHeight    = "Height";     // RENDER height
static constexpr const char *kParamOutWidth  = "OutWidth";   // OUTPUT width
static constexpr const char *kParamOutHeight = "OutHeight";  // OUTPUT height

// ---- CreateFeature: optional, silent defaults.
static constexpr const char *kParamPerfQuality        = "PerfQualityValue";             // default 0 = MaxPerf
static constexpr const char *kParamCreateFlags        = "DLSS.Feature.Create.Flags";    // default 0
static constexpr const char *kParamUseHwDepth         = "DLSS.Use.HW.Depth";            // default 0 = LINEAR
static constexpr const char *kParamEnableOutSubrects  = "DLSS.Enable.Output.Subrects";  // default 0
static constexpr const char *kParamCreationNodeMask   = "CreationNodeMask";             // default 1
static constexpr const char *kParamVisibilityNodeMask = "VisibilityNodeMask";           // default 1
static constexpr const char *kParamFreeMemOnRelease   = "FreeMemOnReleaseFeature";      // default 0

// The six render-preset hint slots. Only ONE of them is consulted, and which one is decided by
// Width/OutWidth - see preset_key_for_ratio().
static constexpr const char *kParamPresetDLAA             = "DLSS.Hint.Render.Preset.DLAA";
static constexpr const char *kParamPresetQuality          = "DLSS.Hint.Render.Preset.Quality";
static constexpr const char *kParamPresetBalanced         = "DLSS.Hint.Render.Preset.Balanced";
static constexpr const char *kParamPresetPerformance      = "DLSS.Hint.Render.Preset.Performance";
static constexpr const char *kParamPresetUltraPerformance = "DLSS.Hint.Render.Preset.UltraPerformance";
static constexpr const char *kParamPresetUltraQuality     = "DLSS.Hint.Render.Preset.UltraQuality";

// ---- EvaluateFeature: HARD-REQUIRED. Get must succeed (else FAIL_InvalidParameter) AND the four
// resources must be non-NULL (else FAIL_MissingInput 0xBAD0000A) [SRC dlaa.cpp:788-799, :1057].
static constexpr const char *kParamColor         = "Color";
static constexpr const char *kParamMotionVectors = "MotionVectors";
static constexpr const char *kParamDepth         = "Depth";
static constexpr const char *kParamOutput        = "Output";
static constexpr const char *kParamJitterX       = "Jitter.Offset.X";
static constexpr const char *kParamJitterY       = "Jitter.Offset.Y";

// ---- EvaluateFeature: optional, silent defaults.
static constexpr const char *kParamReset            = "Reset";                 // forced to 1 on the first evaluate
static constexpr const char *kParamMvScaleX         = "MV.Scale.X";            // default 1.0f
static constexpr const char *kParamMvScaleY         = "MV.Scale.Y";            // default 1.0f
static constexpr const char *kParamMvOffsetX        = "MV.Offset.X";           // default 0.0f, DIVIDED by MV.Scale.X
static constexpr const char *kParamMvOffsetY        = "MV.Offset.Y";           // default 0.0f
static constexpr const char *kParamFrameTimeDeltaMs = "FrameTimeDeltaInMsec";  // default 0.0f
static constexpr const char *kParamRenderSubrectW   = "DLSS.Render.Subrect.Dimensions.Width";
static constexpr const char *kParamRenderSubrectH   = "DLSS.Render.Subrect.Dimensions.Height";
static constexpr const char *kParamColorSubrectX    = "DLSS.Input.Color.Subrect.Base.X";
static constexpr const char *kParamColorSubrectY    = "DLSS.Input.Color.Subrect.Base.Y";
static constexpr const char *kParamDepthSubrectX    = "DLSS.Input.Depth.Subrect.Base.X";
static constexpr const char *kParamDepthSubrectY    = "DLSS.Input.Depth.Subrect.Base.Y";
static constexpr const char *kParamMvSubrectX       = "DLSS.Input.MV.Subrect.Base.X";
static constexpr const char *kParamMvSubrectY       = "DLSS.Input.MV.Subrect.Base.Y";
static constexpr const char *kParamOutSubrectX      = "DLSS.Output.Subrect.Base.X";
static constexpr const char *kParamOutSubrectY      = "DLSS.Output.Subrect.Base.Y";

// ---- Init_Ext: routes the SNIPPET'S OWN log into ours. Read by NGXDLAA::Init out of the
// parameter block during Init_Ext [SRC 0x18003efe5-0x18003f083]. This is the single cheapest
// diagnostic available on this path, because most SR failures are one 0xBAD000xx and no other
// signal - the snippet's own message carries its dlaa.cpp line number.
static constexpr const char *kParamLogCallback      = "Log.Callback";
static constexpr const char *kParamMinLogLevel      = "Minimum.Logging.Level";
static constexpr const char *kParamDisableOtherSinks = "Disable.Other.Logging.Sinks";

// ---- DLSS_GetOptimalSettings, through the callback PopulateParameters_Impl installs. USE A
// SCRATCH BLOCK: here Width/Height are the DISPLAY dims and OutWidth/OutHeight are the RETURNED
// optimal RENDER dims - the exact opposite of their meaning at CreateFeature [SRC 0x1800c1720,
// "Input DisplayWidth: %d" at 0x1800c1812 vs "Output OptimalWidth: %d" at 0x1800c1979]. Sharing
// one block between the two calls silently corrupts the create parameters.
static constexpr const char *kParamOptimalCallback  = "DLSSOptimalSettingsCallback";
static constexpr const char *kParamDynMaxRenderW    = "DLSS.Get.Dynamic.Max.Render.Width";
static constexpr const char *kParamDynMaxRenderH    = "DLSS.Get.Dynamic.Max.Render.Height";
static constexpr const char *kParamDynMinRenderW    = "DLSS.Get.Dynamic.Min.Render.Width";
static constexpr const char *kParamDynMinRenderH    = "DLSS.Get.Dynamic.Min.Render.Height";

// =============================================================================================
// Which render-preset hint slot the snippet will actually read, given the ratio it computes.
// =============================================================================================
inline const char *preset_key_for_ratio(float ratio, const char **out_label)
{
	const char *label = nullptr;
	const char *key   = nullptr;
	if      (ratio <  0.40f) { key = kParamPresetUltraPerformance; label = "UltraPerformance"; }
	else if (ratio <  0.54f) { key = kParamPresetPerformance;      label = "Performance"; }
	else if (ratio <  0.62f) { key = kParamPresetBalanced;         label = "Balanced"; }
	else if (ratio <  0.70f) { key = kParamPresetQuality;          label = "Quality"; }
	else if (ratio <  0.99f) { key = kParamPresetUltraQuality;     label = "UltraQuality"; }
	else                     { key = kParamPresetDLAA;             label = "DLAA"; }
	if (out_label != nullptr)
		*out_label = label;
	return key;
}

// =============================================================================================
// Failure interpretation. ngx::result_to_string() names the code; this names what it means HERE,
// on this path, with the specific check that produces it. Each string is traceable to a single
// site in the disassembly, which is what makes it actionable rather than decorative.
// =============================================================================================
inline const char *explain_result(ngx::Result r)
{
	switch (r)
	{
	case ngx::Result_FAIL_PlatformError:
		return "PlatformError. At Init_Ext this is the CALLER GATE: the snippet resolves its "
		       "caller's module from the return address and requires \"nvngx.dll\" in the path, so "
		       "the call must arrive through remix_nvngx.dll's slot-B forwarders with REAL calls "
		       "and no tail jumps. At Evaluate it means a resource-description query failed on "
		       "Color or Output.";
	case ngx::Result_FAIL_InvalidParameter:
		return "InvalidParameter. At CreateFeature: one of the four HARD-REQUIRED keys "
		       "(Width/Height/OutWidth/OutHeight) was not readable, or a dimension check failed "
		       "(min 32x32; Width<=OutWidth and Height<=OutHeight; PerfQualityValue outside 0..5). "
		       "At Evaluate: one of Color/MotionVectors/Depth/Output/Jitter.Offset.X/"
		       "Jitter.Offset.Y missed, or a Tex2D / subrect-bounds check failed on Color or "
		       "Output. Note JITTER IS UNCONDITIONALLY REQUIRED - it is in the same gate as the "
		       "four resources.";
	case ngx::Result_FAIL_NotInitialized:
		return "NotInitialized. The device was never registered by a successful Init_Ext on THIS "
		       "snippet, or the trampoline's slot-B pointers are null (an out-of-date "
		       "remix_nvngx.dll returns exactly this code from every forwarder).";
	case ngx::Result_FAIL_RWFlagMissing:
		return "RWFlagMissing. The resource bound as Output has no ALLOW_UNORDERED_ACCESS flag. "
		       "This is a DISTINCT code and it is the easiest one to assert on: if the add-on's "
		       "own output texture is the Output, its usage set is wrong; if u0 is bound directly "
		       "(sr_direct_output=1), the game's TAA output UAV is unexpectedly not UAV-capable.";
	case ngx::Result_FAIL_MissingInput:
		return "MissingInput. One of Color / MotionVectors / Depth / Output resolved to a NULL "
		       "pointer. The key was present but its value was null - which for this add-on means "
		       "a resource handle was 0 at bind time.";
	case ngx::Result_FAIL_OutOfGPUMemory:
		return "OutOfGPUMemory. At CreateFeature this is also what instance-table exhaustion "
		       "returns. At 4K output the feature's own allocation is several times what the "
		       "1080p DLSS-NR path ever asked for.";
	case ngx::Result_FAIL_UnsupportedInputFormat:
	case ngx::Result_FAIL_UnsupportedFormat:
		return "The snippet will not take the DXGI format of one of the bound resources. The "
		       "prime suspect is Depth: STRAY's t0 is r32_g8_typeless (a TYPELESS PLANAR "
		       "resource), NGX reads the format off the D3D12_RESOURCE_DESC and there is no "
		       "channel on D3D12 through which to tell it the view format. Second suspect is the "
		       "decoded r16g16_float motion guide.";
	case ngx::Result_FAIL_FeatureNotSupported:
		return "FeatureNotSupported. Feature id 1 was rejected by this snippet build.";
	case ngx::Result_FAIL_OutOfDate:
		return "OutOfDate. On the optimal-settings path this is what a NULL "
		       "DLSSOptimalSettingsCallback returns - call PopulateParameters_Impl on the scratch "
		       "block first.";
	default:
		return "";
	}
}

// =============================================================================================
// SR-OWNED RESOURCES
//
// Two textures, and both exist for a reason DLSS-NR does not have:
//
//   out_tex   the network's Output, at the OUTPUT extent. NR's out_tex is at the colour extent
//             because NR does not upscale; SR's cannot be. Only created when sr_direct_output=0.
//             Created UAV (or Evaluate returns FAIL_RWFlagMissing) + copy_source (for the
//             copy-back) + shader_resource (free on D3D12 for a colour texture).
//
//   mvec_tex  the decoded motion guide, at the RENDER extent. THIS IS THE ONE THAT IS EASY TO GET
//             WRONG. mvec_decode.hpp emits absolute pixels on "the colour grid", and for SR the
//             colour input is the TAA pass's INPUT (t5, render resolution), not its output. A
//             guide allocated at the output extent would be read by DLSS at 2x the correct scale
//             in each axis under Performance mode, with no error anywhere. The PIPELINE is
//             mvec_decode's, unchanged and shared with the NR path; only this target is ours.
// =============================================================================================
struct resources
{
	resource      out_tex   = { 0 };
	uint32_t      out_w     = 0, out_h = 0;
	format        out_fmt   = format::unknown;
	bool          out_failed = false;      // latched per (out_w, out_h)

	resource      mvec_tex  = { 0 };
	resource_view mvec_uav  = { 0 };
	uint32_t      mvec_w    = 0, mvec_h = 0;
	bool          mvec_failed = false;     // latched per (mvec_w, mvec_h)
};

template <typename LogFn>
inline void destroy_resources(device *dev, resources &r, LogFn log)
{
	(void)log;
	if (dev == nullptr)
	{
		r = resources();
		return;
	}
	if (r.mvec_uav.handle != 0) dev->destroy_resource_view(r.mvec_uav);
	if (r.mvec_tex.handle != 0) dev->destroy_resource(r.mvec_tex);
	if (r.out_tex.handle  != 0) dev->destroy_resource(r.out_tex);
	r = resources();
}

// Returns false and leaves the pass off. A false here NEVER suppresses the game's dispatch: the
// host decides that only after a successful evaluate.
template <typename LogFn>
inline bool ensure_output(device *dev, resources &r, uint32_t w, uint32_t h, format fmt, LogFn log)
{
	if (dev == nullptr || w == 0 || h == 0 || fmt == format::unknown)
		return false;
	if (r.out_tex.handle != 0)
		return (r.out_w == w && r.out_h == h && r.out_fmt == fmt);
	if (r.out_failed)
		return false;

	const resource_desc desc(
		w, h, 1, 1, fmt, 1, memory_heap::default_,
		resource_usage::unordered_access | resource_usage::copy_source | resource_usage::shader_resource);

	resource out = { 0 };
	if (!dev->create_resource(desc, nullptr, resource_usage::unordered_access, &out) || out.handle == 0)
	{
		r.out_failed = true;
		char buf[320];
		std::snprintf(buf, sizeof(buf),
			"DLSS-SR: create_resource FAILED for the %ux%u Output texture. The SR pass stays off "
			"at this resolution. At 4K this is the first place a memory problem shows up.", w, h);
		log(log_error, buf);
		return false;
	}

	r.out_tex = out;
	r.out_w   = w;
	r.out_h   = h;
	r.out_fmt = fmt;
	return true;
}

// The decoded motion guide, at the RENDER extent. r16g16_float, matching mvec_decode's
// RWTexture2D<float2> output declaration exactly.
template <typename LogFn>
inline bool ensure_mvec(device *dev, resources &r, uint32_t w, uint32_t h, LogFn log)
{
	if (dev == nullptr || w == 0 || h == 0)
		return false;
	if (r.mvec_tex.handle != 0 && r.mvec_uav.handle != 0)
		return (r.mvec_w == w && r.mvec_h == h);
	if (r.mvec_failed)
		return false;

	const resource_desc desc(
		w, h, 1, 1, format::r16g16_float, 1, memory_heap::default_,
		resource_usage::unordered_access | resource_usage::shader_resource);

	resource tex = { 0 };
	if (!dev->create_resource(desc, nullptr, resource_usage::unordered_access, &tex) || tex.handle == 0)
	{
		r.mvec_failed = true;
		char buf[320];
		std::snprintf(buf, sizeof(buf),
			"DLSS-SR: create_resource FAILED for the %ux%u r16g16_float motion guide. The decode "
			"is off; MotionVectors falls back to the game's raw encoded velocity.", w, h);
		log(log_error, buf);
		return false;
	}

	const resource_view_desc uav(resource_view_type::texture_2d, format::r16g16_float, 0, 1, 0, 1);
	resource_view view = { 0 };
	if (!dev->create_resource_view(tex, resource_usage::unordered_access, uav, &view) || view.handle == 0)
	{
		dev->destroy_resource(tex);
		r.mvec_failed = true;
		log(log_error, "DLSS-SR: create_resource_view FAILED for the motion guide's UAV. The "
		               "decode is off; MotionVectors falls back to the game's raw encoded velocity.");
		return false;
	}

	r.mvec_tex = tex;
	r.mvec_uav = view;
	r.mvec_w   = w;
	r.mvec_h   = h;
	return true;
}

// =============================================================================================
// THE FEATURE
// =============================================================================================
struct create_desc
{
	uint32_t render_w = 0, render_h = 0;   // Width  / Height   - the RENDER view rect
	uint32_t out_w    = 0, out_h    = 0;   // OutWidth / OutHeight - the OUTPUT view rect
	uint32_t perf_quality = perf_max_perf;
	uint32_t flags        = 0;             // DLSS.Feature.Create.Flags
	bool     hw_depth     = true;          // DLSS.Use.HW.Depth - CREATE ONLY
	uint32_t preset       = 0;             // 0 = auto; written into the ratio-selected slot only
};

struct evaluate_desc
{
	ID3D12Resource *color  = nullptr;
	ID3D12Resource *mvec   = nullptr;
	ID3D12Resource *depth  = nullptr;
	ID3D12Resource *output = nullptr;

	// The RENDER view rect for this frame. Defaults to the create-time Width/Height inside the
	// snippet when absent, so writing it explicitly is how a dynamic-resolution frame is expressed.
	uint32_t render_w = 0, render_h = 0;

	float    jitter_x = 0.0f, jitter_y = 0.0f;   // MANDATORY - see the header comment
	bool     reset    = false;
	float    mv_scale_x = 1.0f, mv_scale_y = 1.0f;
	float    frame_time_ms = 0.0f;
};

struct feature
{
	ngx::parameter_block *params = nullptr;   // OURS. The snippet exports no AllocateParameters.
	void                 *handle = nullptr;   // NVSDK_NGX_Handle *

	// What the live feature was created with. A change in any of them forces a rebuild, because
	// OutWidth/OutHeight are latched at create and there is no evaluate-time output extent.
	uint32_t render_w = 0, render_h = 0, out_w = 0, out_h = 0;
	uint32_t create_flags = 0, perf_quality = 0;
	bool     hw_depth = false;

	bool     failed   = false;   // latched for this (render, out) pair
	bool     need_reset = true;

	// The one-shot log latches. Same rule as everywhere else in this codebase: a message that
	// could print every frame gets a latch, and the latch is named after the message.
	bool logged_create_fail  = false;
	bool logged_eval_fail    = false;
	bool logged_preset       = false;
	bool logged_first_eval   = false;
	bool logged_jitter_zero  = false;
	bool logged_no_jitter    = false;
	bool logged_out_extent   = false;
	bool logged_hw_depth     = false;
};

// True when the currently live feature already matches this description.
inline bool feature_matches(const feature &f, const create_desc &c)
{
	return f.handle != nullptr &&
	       f.render_w == c.render_w && f.render_h == c.render_h &&
	       f.out_w == c.out_w && f.out_h == c.out_h &&
	       f.create_flags == c.flags && f.perf_quality == c.perf_quality &&
	       f.hw_depth == c.hw_depth;
}

// Releases the feature. The CALLER must have idled the queue first - CreateFeature and
// EvaluateFeature both record real GPU work, so in-flight work can still reference it.
inline void release_feature(const ngx::snippet &sn, feature &f)
{
	if (f.handle != nullptr && sn.release_feature != nullptr)
		sn.release_feature(f.handle);
	f.handle     = nullptr;
	f.need_reset = true;
	f.render_w = f.render_h = f.out_w = f.out_h = 0;
}

// CreateFeature records weight upload onto the command list it is given, so it must run inside the
// host's capture/restore window exactly as the DLSS-NR path's does.
template <typename LogFn>
inline bool create_feature(const ngx::snippet &sn, feature &f, ID3D12GraphicsCommandList *cl,
                           const create_desc &c, LogFn log)
{
	if (feature_matches(f, c))
		return true;
	if (f.failed)
		return false;
	if (f.params == nullptr || sn.create_feature == nullptr || cl == nullptr)
		return false;

	// A live feature with different dimensions must go first, and the caller has already idled.
	if (f.handle != nullptr)
		release_feature(sn, f);

	ngx::parameter_block *p = f.params;

	// The four HARD-REQUIRED keys. The snippet reads Width with Get(int*) at create and with
	// Get(unsigned int*) in the optimal-settings path, so the block's numeric coercion is
	// load-bearing here - it is provided by ngx_interop.hpp's as_ull() and covered by its own
	// comment there.
	ngx::set_u32(p, kParamWidth,     c.render_w);
	ngx::set_u32(p, kParamHeight,    c.render_h);
	ngx::set_u32(p, kParamOutWidth,  c.out_w);
	ngx::set_u32(p, kParamOutHeight, c.out_h);

	// PerfQualityValue defaults to 0 = MaxPerf when absent, which happens to be right for
	// 1920->3840 and is exactly wrong the moment anyone tries DLAA. Always written.
	ngx::set_u32(p, kParamPerfQuality, c.perf_quality);
	ngx::set_u32(p, kParamCreateFlags, c.flags);

	// CREATE-TIME ONLY, and silent if wrong. See the header comment.
	ngx::set_u32(p, kParamUseHwDepth, c.hw_depth ? 1u : 0u);

	// This add-on always binds Output with subrect base (0,0), so output subrects stay disabled.
	// With them disabled the snippet REJECTS a non-zero DLSS.Output.Subrect.Base.* at evaluate
	// [SRC dlaa.cpp:1120/1122], which is the check that keeps the two consistent.
	ngx::set_u32(p, kParamEnableOutSubrects, 0u);

	ngx::set_u32(p, kParamCreationNodeMask,   1u);
	ngx::set_u32(p, kParamVisibilityNodeMask, 1u);
	ngx::set_i32(p, kParamFreeMemOnRelease,   1);

	// The preset hint goes into the slot the RATIO selects, and only that one. Writing all six
	// would be harmless but would make the log lie about which one the snippet reads.
	const float ratio = (c.out_w != 0)
		? static_cast<float>(c.render_w) / static_cast<float>(c.out_w) : 1.0f;
	const char *preset_label = nullptr;
	const char *preset_key   = preset_key_for_ratio(ratio, &preset_label);
	if (c.preset != 0)
		ngx::set_u32(p, preset_key, c.preset);

	if (!f.logged_preset)
	{
		f.logged_preset = true;
		char buf[700];
		std::snprintf(buf, sizeof(buf),
			"DLSS-SR: CreateFeature parameters - render %ux%u -> output %ux%u (ratio %.4f), "
			"PerfQualityValue=%s, Create.Flags=0x%02x [IsHDR=%d MVLowRes=%d MVJittered=%d "
			"DepthInverted=%d AutoExposure=%d], DLSS.Use.HW.Depth=%d. The snippet picks its render "
			"preset slot BY RATIO, not by PerfQualityValue, so the key that will actually be read "
			"is \"%s\" (%s) and this build %s.",
			c.render_w, c.render_h, c.out_w, c.out_h, (double)ratio,
			perf_quality_name(c.perf_quality), c.flags,
			(int)((c.flags & kFlagIsHDR) != 0), (int)((c.flags & kFlagMVLowRes) != 0),
			(int)((c.flags & kFlagMVJittered) != 0), (int)((c.flags & kFlagDepthInverted) != 0),
			(int)((c.flags & kFlagAutoExposure) != 0), (int)c.hw_depth,
			preset_key, preset_label,
			c.preset != 0 ? "wrote a hint into it" : "left it at 0 = auto");
		log(log_info, buf);
	}

	void *handle = nullptr;
	const ngx::Result r = sn.create_feature(cl, kFeatureSuperSampling, p, &handle);
	if (ngx::failed(r) || handle == nullptr)
	{
		f.failed = true;
		if (!f.logged_create_fail)
		{
			f.logged_create_fail = true;
			char buf[1400];
			std::snprintf(buf, sizeof(buf),
				"DLSS-SR: CreateFeature(feature %u) FAILED: 0x%08x %s. %s THE WHOLE DLSS-SR PASS IS "
				"NOW OFF - not just the feature: the host bails at the top of its dispatch hook "
				"rather than paying for the jitter read, the motion-vector decode and the state "
				"restore every frame for a feature that cannot exist. It is latched for render "
				"%ux%u -> output %ux%u and is retried when the colour-input or output extent "
				"moves. The game's own TAA is untouched.",
				kFeatureSuperSampling, (unsigned)r, ngx::result_to_string(r), explain_result(r),
				c.render_w, c.render_h, c.out_w, c.out_h);
			log(log_error, buf);
		}
		return false;
	}

	f.handle       = handle;
	f.render_w     = c.render_w;
	f.render_h     = c.render_h;
	f.out_w        = c.out_w;
	f.out_h        = c.out_h;
	f.create_flags = c.flags;
	f.perf_quality = c.perf_quality;
	f.hw_depth     = c.hw_depth;
	f.need_reset   = true;

	char buf[400];
	std::snprintf(buf, sizeof(buf),
		"DLSS-SR: CreateFeature(feature %u) SUCCEEDED. render %ux%u -> output %ux%u, "
		"PerfQualityValue=%s, DLSS.Use.HW.Depth=%d.",
		kFeatureSuperSampling, c.render_w, c.render_h, c.out_w, c.out_h,
		perf_quality_name(c.perf_quality), (int)c.hw_depth);
	log(log_info, buf);
	return true;
}

// Writes the evaluate parameters and calls the snippet. Returns the raw NGX result so the caller
// can implement its own fallback ladder on the specific code.
inline ngx::Result evaluate_feature(const ngx::snippet &sn, feature &f,
                                    ID3D12GraphicsCommandList *cl, const evaluate_desc &e)
{
	if (f.handle == nullptr || f.params == nullptr || sn.evaluate_feature == nullptr || cl == nullptr)
		return ngx::Result_FAIL_NotInitialized;

	ngx::parameter_block *p = f.params;

	ngx::set_res(p, kParamColor,         e.color);
	ngx::set_res(p, kParamMotionVectors, e.mvec);
	ngx::set_res(p, kParamDepth,         e.depth);
	ngx::set_res(p, kParamOutput,        e.output);

	// MANDATORY. Not conditional on anything - see the header comment.
	ngx::set_f32(p, kParamJitterX, e.jitter_x);
	ngx::set_f32(p, kParamJitterY, e.jitter_y);

	// Every subrect base is explicitly zeroed rather than left to the snippet's default. The
	// parameter block outlives the evaluate and is reused, so an unset key is not the same thing
	// as an absent one on the second frame.
	ngx::set_u32(p, kParamColorSubrectX, 0u);
	ngx::set_u32(p, kParamColorSubrectY, 0u);
	ngx::set_u32(p, kParamDepthSubrectX, 0u);
	ngx::set_u32(p, kParamDepthSubrectY, 0u);
	ngx::set_u32(p, kParamMvSubrectX,    0u);
	ngx::set_u32(p, kParamMvSubrectY,    0u);
	ngx::set_u32(p, kParamOutSubrectX,   0u);
	ngx::set_u32(p, kParamOutSubrectY,   0u);

	// The RENDER VIEW RECT, which is not the same thing as the colour texture's extent whenever
	// QuantizeSceneBufferSize rounded up (RenderUtils.cpp:1480-1490 rounds to a multiple of 4).
	// The snippet defaults these to the create-time Width/Height when absent; writing them is what
	// makes the difference visible in the log instead of silent in the pixels.
	ngx::set_u32(p, kParamRenderSubrectW, e.render_w);
	ngx::set_u32(p, kParamRenderSubrectH, e.render_h);

	ngx::set_u32(p, kParamReset, (e.reset || f.need_reset) ? 1u : 0u);

	ngx::set_f32(p, kParamMvScaleX, e.mv_scale_x);
	ngx::set_f32(p, kParamMvScaleY, e.mv_scale_y);
	// MV.Offset.X is DIVIDED by MV.Scale.X inside the snippet [SRC 0x18003d35c], so a non-zero
	// offset is not a plain translation. This add-on has no use for one; it is written as an
	// explicit zero so a stale value can never survive in the reused block.
	ngx::set_f32(p, kParamMvOffsetX, 0.0f);
	ngx::set_f32(p, kParamMvOffsetY, 0.0f);

	ngx::set_f32(p, kParamFrameTimeDeltaMs, e.frame_time_ms);

	return sn.evaluate_feature(cl, f.handle, p, nullptr);
}

// =============================================================================================
// JITTER
//
// A thin per-frame wrapper over ue4_jitter.hpp. It owns NOTHING that header already owns - no
// signature, no row arithmetic, no tier policy. What it adds is the three things a caller needs
// and the header deliberately does not provide: a per-resolution layout cache, bounded retries so
// a wedged discovery cannot Map the game's upload pool once per frame forever, and the one-shot
// latches that keep a per-frame failure from becoming a per-frame log line.
//
// The reset signal is DERIVED, not read. View.CameraCut carries only View.bCameraCut and misses
// three of the four conditions the engine actually resets on; result::reset_signalled is the
// TemporalAAJitter.zw == .xy compare, which covers all four
// [SRC SceneVisibility.cpp:3337-3342, :3396-3398 vs SceneView.cpp:2524].
// =============================================================================================
struct jitter_source
{
	ue4jitter::layout        layout;
	ue4jitter::echo_validator echo;
	bool     layout_ok      = false;
	bool     layout_failed  = false;    // permanent for this resolution
	uint32_t discover_tries = 0;

	// The last accepted answer. Kept across a transient read failure for exactly the same reason
	// the ClipToPrevClip matrix is: a one-frame-stale sub-pixel offset is a small bounded error,
	// and swapping to (0,0) mid-run is not.
	float    jitter_x = 0.0f, jitter_y = 0.0f;
	bool     have_jitter = false;
	uint32_t view_w = 0, view_h = 0;
	bool     view_rect_measured = false;
	bool     reset_signalled = false;
	uint32_t fail_streak = 0;

	bool logged_located    = false;
	bool logged_failed     = false;
	bool logged_zero       = false;
	bool logged_tier       = false;
	bool logged_echo_break = false;
};

// pool/offset/avail describe the game's own upload pool and where the View CB starts inside it -
// the host has already resolved and bounds-checked them. expect_w/h are the colour texture extent,
// which ue4_jitter reconciles against the view rect through QuantizeSceneBufferSize.
//
// Returns true when jitter_x/jitter_y are worth using. A false is never fatal to the host: the SR
// evaluate REQUIRES jitter, so the host must refuse the pass rather than send (0,0) - which is a
// legitimate value the network cannot distinguish from "we could not read it".
template <typename LogFn>
inline bool update_jitter(jitter_source &js, ID3D12Resource *pool, uint64_t offset, uint64_t avail,
                          uint32_t expect_w, uint32_t expect_h, int32_t dxbc_clip_row,
                          bool allow_projection_only, LogFn log)
{
	if (js.layout_failed || pool == nullptr)
		return false;

	ue4jitter::config c;
	c.expected_render_width      = expect_w;
	c.expected_render_height     = expect_h;
	c.expected_is_texture_extent = true;
	c.dxbc_clip_to_prev_clip_row = dxbc_clip_row;
	// STRICTER THAN THE ClipToPrevClip PATH ON PURPOSE. There the clip row is the payload and the
	// jitter gates are incidental; here the JITTER is the payload, so TemporalAAParams must
	// validate unless the caller has explicitly opted down a tier.
	c.require_params        = !allow_projection_only;
	c.allow_projection_only = allow_projection_only;

	const uint32_t rows_avail = static_cast<uint32_t>(
		(avail / ue4jitter::kBytesPerRow) > ue4jitter::kViewCbConstantRows
			? ue4jitter::kViewCbConstantRows : (avail / ue4jitter::kBytesPerRow));
	if (rows_avail == 0)
	{
		js.layout_failed = true;
		return false;
	}
	c.max_rows = rows_avail;

	ue4jitter::result res;

	if (!js.layout_ok)
	{
		if (++js.discover_tries > 8)
		{
			js.layout_failed = true;
			if (!js.logged_failed)
			{
				js.logged_failed = true;
				log(log_error, "DLSS-SR: View uniform buffer discovery failed 8 times running, so "
				               "the jitter cannot be recovered. DLSS-SR REQUIRES jitter - "
				               "Jitter.Offset.X/Y are in the same hard gate as the four resources "
				               "- so the SR pass is OFF for this resolution and the game's own TAA "
				               "runs untouched. This message is printed once.");
			}
			return false;
		}

		const uint32_t want_bytes = rows_avail * ue4jitter::kBytesPerRow;
		std::vector<uint8_t> cb(want_bytes);
		if (!ue4jitter::read_view_cb(pool, offset, want_bytes, cb.data()))
			return false;   // transient; retried up to the bound above

		ue4jitter::layout lay;
		if (!ue4jitter::discover(cb.data(), cb.size(), c, lay, res) || !lay.valid)
		{
			js.layout_failed = true;
			if (!js.logged_failed)
			{
				js.logged_failed = true;
				char desc[700], buf[1100];
				ue4jitter::describe(res, desc, sizeof(desc));
				std::snprintf(buf, sizeof(buf),
					"DLSS-SR: View uniform buffer discovery FAILED, so the jitter cannot be "
					"recovered and the SR pass is OFF for this resolution. %s "
					"(checks_run=0x%04x checks_passed=0x%04x, %u bytes read at offset %llu). "
					"If the tier is the only thing that failed, sr_jitter_projection_only=1 "
					"accepts the weakest tier - it is still the correct number, it just has no "
					"second opinion. This message is printed once.",
					desc, res.checks_run, res.checks_passed, want_bytes,
					(unsigned long long)offset);
				log(log_error, buf);
			}
			return false;
		}

		js.layout    = lay;
		js.layout_ok = true;

		if (!js.logged_located)
		{
			js.logged_located = true;
			char desc[700], buf[1100];
			ue4jitter::describe(res, desc, sizeof(desc));
			std::snprintf(buf, sizeof(buf),
				"DLSS-SR: View uniform buffer LOCATED for JITTER. %s. Rows: ViewToClip anchor %d, "
				"TemporalAAJitter %d, ViewSizeAndInvSize %d, TemporalAAParams %d, ClipToPrevClip "
				"%d. View rect %ux%u (the colour TEXTURE extent given to discovery was %ux%u; a "
				"difference here is QuantizeSceneBufferSize rounding up and the VIEW RECT is what "
				"DLSS is fed). checks_run=0x%04x checks_passed=0x%04x.",
				desc, lay.row_view_to_clip, lay.row_jitter, lay.row_view_size, lay.row_params,
				lay.row_clip_to_prev_clip, lay.render_width, lay.render_height, expect_w, expect_h,
				res.checks_run, res.checks_passed);
			log(log_info, buf);
		}
	}

	// ---- the per-frame path: five 16-byte reads straight out of the pool ----------------------
	//
	// FIVE Map/Unmap pairs, not one, and that is deliberate. ue4_jitter offers a mapped_row_reader
	// that would need only one - but it is fed by pool_map_cache, which caches a raw
	// ID3D12Resource* and therefore OBLIGES the caller to register addon_event::destroy_resource
	// and call forget() there, or a destroyed-and-reallocated pool at the same address is a
	// use-after-free with no diagnostic. This add-on registers no such event, so it pays five
	// pointer hand-backs a frame (on vkd3d-proton UPLOAD memory is unconditionally HOST_COHERENT,
	// so Map reduces to returning the persistent cpu_address - no vkMapMemory, no refcount) and
	// the whole hazard class simply does not exist. Same trade, same reasoning, as
	// nr_update_clip_to_prev_clip's per-frame 64-byte read.
	uint8_t row_buf[ue4jitter::kBytesPerRow];
	bool    read_failed = false;
	auto reader = [&](int32_t row, float out[4]) -> bool {
		if (row < 0 || static_cast<uint32_t>(row) >= rows_avail)
			return false;
		const uint64_t byte_off = offset + static_cast<uint64_t>(row) * ue4jitter::kBytesPerRow;
		if (!ue4jitter::read_view_cb(pool, byte_off, ue4jitter::kBytesPerRow, row_buf))
			return false;
		std::memcpy(out, row_buf, 4 * sizeof(float));
		return true;
	};

	if (!ue4jitter::evaluate(reader, js.layout, c, res) || res.st != ue4jitter::status::ok)
	{
		read_failed = true;
		js.echo.forget();
		if (++js.fail_streak >= 30)
		{
			js.layout_failed = true;
			js.have_jitter   = false;
			if (!js.logged_failed)
			{
				js.logged_failed = true;
				char desc[700], buf[1000];
				ue4jitter::describe(res, desc, sizeof(desc));
				std::snprintf(buf, sizeof(buf),
					"DLSS-SR: the per-frame jitter read failed 30 frames running, so the SR pass "
					"is OFF for this resolution and the game's own TAA runs untouched. %s. This "
					"message is printed once.", desc);
				log(log_error, buf);
			}
		}
		// KEEP the last good value for a bounded number of frames, exactly as the DLSS-NR path
		// keeps the last good ClipToPrevClip.
		return js.have_jitter && !js.layout_failed;
	}

	js.fail_streak = 0;

	const ue4jitter::echo_validator::verdict v = js.echo.submit(res);
	if (v == ue4jitter::echo_validator::verdict::broken && !js.logged_echo_break)
	{
		js.logged_echo_break = true;
		log(log_warn, "DLSS-SR: the cross-frame jitter echo BROKE - this frame's "
		              "TemporalAAJitter.zw is not last frame's .xy. That is expected on a frame "
		              "that never reached the TAA pass (a paused or menu frame), and it is "
		              "treated as \"reset DLSS\", not as \"the extraction is broken\". If it "
		              "repeats every frame the layout is wrong. This message is printed once.");
	}

	if (!js.logged_tier)
	{
		js.logged_tier = true;
		char desc[700], buf[900];
		ue4jitter::describe(res, desc, sizeof(desc));
		std::snprintf(buf, sizeof(buf),
			"DLSS-SR: FIRST JITTER READ. %s. echo=%s. At tier `full` the two numbers handed to "
			"Jitter.Offset.X/Y are the ENGINE'S OWN float (TemporalAAParams.zw) with no arithmetic "
			"at all, and the NDC route is the cross-check that had to agree to 1e-4. A flipped Y "
			"sign is the single most likely bug in this feature and it is SILENT - "
			"sr_jitter_scale_y=-1 is the no-rebuild A/B.", desc,
			ue4jitter::echo_validator::verdict_text(v));
		log(log_info, buf);
	}

	if (res.jitter_is_zero && !js.logged_zero)
	{
		js.logged_zero = true;
		log(log_warn, "DLSS-SR: the recovered jitter is EXACTLY ZERO. That is a legitimate answer "
		              "(r.TemporalAASamples=1, or TAA off), but if it holds every frame the game "
		              "is not jittering and DLSS-SR will produce a soft, aliased image with no "
		              "error reported anywhere. Check that r.TemporalAA.Upsampling=1 actually "
		              "took. This message is printed once.");
	}

	js.jitter_x           = res.jitter_px_x;
	js.jitter_y           = res.jitter_px_y;
	js.have_jitter        = true;
	js.view_w             = res.render_width;
	js.view_h             = res.render_height;
	js.view_rect_measured = (res.checks_passed & ue4jitter::check::view_size_row) != 0;
	js.reset_signalled    = res.reset_signalled;
	(void)read_failed;
	return true;
}

} // namespace dlss_sr
