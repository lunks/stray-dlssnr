// mvec_decode.hpp - the UE 4.27 motion-vector decode pass: one compute shader that turns STRAY's
// encoded, SPARSE velocity buffer into the absolute colour-grid pixels DLSS-NR actually wants.
//
// This is README gap 2. It is modelled on hdr_codec.hpp line for line - HLSL as a string literal,
// D3DCompile to cs_5_0 at load through a LoadLibraryW'd d3dcompiler (never linked), a source-hash
// cache with a user override, its own textures, dispatched inside the existing capture/restore
// window - and it reuses that header's compile/cache machinery rather than duplicating it.
//
// =================================================================================================
// THERE ARE TWO HALVES AND THE SECOND ONE IS THE ONE PEOPLE GET WRONG
// =================================================================================================
//
// (a) THE DECODE.  UE 4.27 Engine/Shaders/Private/Common.ush:1537-1570  [WEB, three independent
//     mirrors of ++UE4+Release-4.27 agreeing verbatim; the SCALE is additionally [HW], see below]
//
//         EncodedV.xy = V.xy * (0.499f * 0.5f) + 32767.0f / 65535.0f          // encode
//         V.xy = EncodedV.xy * InvDiv - 32767.0f / 65535.0f * InvDiv          // decode
//         InvDiv = 1.0f / (0.499f * 0.5f)
//
//     InvDiv in float32 is 4.008016109466553f == 0x408041AB, which is BIT-IDENTICAL to
//     dxbc_tokens.hpp:138 kVelocityDecodeScaleBits - the constant this project's own Gate B
//     (shader_detect.hpp:616-655) matches INSIDE STRAY'S OWN DXBC, and which is a hard reject, so
//     the shipped game demonstrably carries it. [HW]
//
//     THE BIAS IS NOT 0.5. It is 32767/65535 = 0.49999237060546875f (0x3EFFFF00), and in the
//     decode it folds into a MAD constant (32767/65535)*InvDiv = 2.0039775371551514f
//     (0x4000412B, negated 0xC000412B / bytes 2B 41 00 C0). Both bit patterns were recomputed on
//     this host - see the replay named at the bottom of this comment. Epic's own comment at
//     Common.ush:1539 says why 0.499 and not 0.5: it keeps the CLEAR COLOUR (0,0) outside the
//     encodable range so it can be a sentinel. Verified numerically: over the whole documented
//     V in [-2,2] range the encoded .x lands in [0.00099236, 0.99899238], u16 [65, 65469], so
//     exactly-zero is unreachable for a texel UE actually wrote.
//
//     THE UNITS ARE NOT PIXELS. Common.ush:1535 - "velocity needs to support -2..2 screen space
//     range for x and y". VelocityCommon.ush:11-18 Calculate3DVelocity builds it as
//     ScreenPos - PrevScreenPos where ScreenPos is the post-perspective-divide, jitter-removed
//     NDC position. So the decoded value is an NDC DELTA, span 2.0, CURRENT MINUS PREVIOUS,
//     Y AXIS UP.
//
// (b) THE VELOCITY BUFFER IS SPARSE, AND THIS IS THE HALF THAT MATTERS MOST.
//
//     UE writes the velocity texture only where it decided to; everywhere else the texel is
//     literally the cleared (0,0,0,0) (VelocityRendering.cpp:363, FClearValueBinding::Transparent)
//     and UE's own TAA falls back to reconstructing camera motion by reprojecting depth through
//     View.ClipToPrevClip. The validity test is EXACT and is not a guess:
//
//         bool DynamicN = EncodedVelocity.x > 0.0;      TAAStandalone.usf:2004
//
//     RED CHANNEL, STRICT GREATER-THAN, ON THE RAW ENCODED SAMPLE. Two of the three researchers
//     independently found the identical spelling in six separate UE 4.27 consumers (TAAStandalone,
//     TAADilateVelocity, SSRTRayCast, PostProcessAmbientOcclusion, PostProcessVelocityFlatten,
//     SSDTemporalAccumulation). Do not test .y, do not use >=, do not test the DECODED value.
//
//     r.BasePassOutputsVelocity=1 IS SET IN STRAY'S Engine.ini [HW] AND IT DOES NOT MAKE THE
//     BUFFER DENSE. BasePassPixelShader.usf:979 zeroes GBuffer.Velocity, :985 gates the real value
//     on GetPrimitiveData(...).OutputVelocity > 0, and :997-1000 zeroes it again when
//     DrawsVelocity == 0; VelocityRendering.cpp:456-460 returns false for any primitive whose
//     LocalToWorld equals its previous transform ("Hasn't moved"). What the CVar changes is WHERE
//     moving geometry is rasterised - base-pass MRT instead of a separate pass, which is what lets
//     world-position-offset materials produce velocity at all - not WHETHER static geometry does.
//     Static opaque, sky, unmoved movables, translucency and particles all stay at exactly zero.
//
//     So a naive decode hands DLSS ZERO MOTION for the majority of the screen, which is WORSE than
//     the status quo. The reconstruction is UE's own, verbatim (TAACommon.ush:348-356 /
//     TAAStandalone.usf:1994-1997):
//
//         ThisClip   = float4(ScreenPos, DeviceZ, 1)
//         PrevClip   = mul(ThisClip, View.ClipToPrevClip)
//         PrevScreen = PrevClip.xy / PrevClip.w
//         BackN      = ScreenPos - PrevScreen
//
//     and it produces the SAME QUANTITY IN THE SAME UNITS as the decode - which is provable from
//     UE's own code rather than argued: TAAStandalone.usf assigns one into the other's variable
//     (`BackN`) with no conversion at :1997 and :2007.
//
// =================================================================================================
// THE OUTPUT CONTRACT, AND THE SIGN TRAP
// =================================================================================================
//
//     mvec_px = BackN * float2(-0.5 * ViewW, +0.5 * ViewH)      y-DOWN colour-grid pixels
//
//     so that  current_pixel + mvec == previous_pixel.
//
// THE X CHANNEL IS NEGATED AND THE Y CHANNEL IS NOT. That asymmetry is the single highest-risk
// line in this file. It is two flips that cancel on Y and compound on X:
//   * DLSS wants previous-minus-current; UE stores current-minus-previous  -> negates BOTH;
//   * UE ScreenPos is y-UP (Common.ush:1153-1156, ViewportUVToScreenPos = (2u-1, 1-2v)) while the
//     pixel grid is y-DOWN                                                 -> negates ONLY Y.
// A naive "flip the Y" gets BOTH signs wrong and still half-works, which is exactly the
// "it kind of works but smears" failure the task warned about.
//
// The same conversion is what NVIDIA's own UE plugin ships, VelocityCombine.usf:195-197 [WEB]:
//     float2 OutVelocity = Velocity * float2(0.5, -0.5) * View.ViewSizeAndInvSize.xy;
//     OutVelocityCombinedTexture[OutputPixelPos].xy = -OutVelocity;
// which is algebraically identical to the line above.
//
// The Y factor -0.5*H is corroborated five independent ways: Common.ush:1153-1156; Common.ush:1114
// (NDCPos.xy = (PixelPos*InvViewSize - 0.5) * float2(2,-2)); SceneView.cpp:2300-2305 with
// GProjectionSignY = 1.0f (RHI.cpp:834); this project's own ue4_jitter.hpp:594-601
// (px_y = ndc_y * h * -0.5f, backed by 79 passing assertions); and NVIDIA's line above.
//
// THE DLSS DIRECTION ITSELF (previous-minus-current) IS [WEB], NOT MEASURED HERE. It comes from
// the DLSS Programming Guide - "when the motion vector for the pixel is added to the pixel's
// current location, the result is the location the pixel occupied in the previous frame" - and
// from NVIDIA's own UE plugin doing exactly this conversion. It has NOT been confirmed against
// DLSS-NR (NGX feature 18) specifically. That is why it is exposed rather than welded in:
// mvec_scale_x = -1 and mvec_scale_y = -1 flip either axis through NGX's own MVecScaleX/Y with no
// rebuild, and NVIDIA documents those parameters as carrying sign.
//
// BUT NOTE WHAT A SINGLE-AXIS FLIP CAN AND CANNOT TEST. Getting the DIRECTION CONVENTION wrong
// negates BOTH axes at once, because it negates BackN itself. mvec_scale_x=-1 alone leaves Y
// inverted and mvec_scale_y=-1 alone leaves X inverted, so under a fully inverted guide BOTH
// single-axis runs look worse and BOTH match the "should be worse" expectation - which reads as
// confirmation of a binding that is in fact wrong on both axes. The configuration that
// discriminates is mvec_scale_x=-1 AND mvec_scale_y=-1 TOGETHER; if that is BETTER, the contract
// above must be negated HERE, in the shader, and not left corrected by the two keys. A single-axis
// flip tests a per-axis sign error and nothing more. See the A/B table in the README.
//
// =================================================================================================
// WHAT IS DELIBERATELY *NOT* IN HERE
// =================================================================================================
//   * NO JITTER ARITHMETIC, ANYWHERE. Both branches are already jitter-free at source:
//     VelocityCommon.ush:11-12 subtracts the current frame's jitter from the current position and
//     the PREVIOUS frame's jitter from the previous one before differencing, and
//     SceneView.cpp:2496-2499 builds ClipToPrevClip from ComputeInvProjectionNoAAMatrix() *
//     ComputeProjectionNoAAMatrix() - NoAA at BOTH ends - while TAAStandalone feeds it a ScreenPos
//     derived from the unjittered output pixel grid. Adding TemporalAAJitter here would inject a
//     per-frame +/-1.5px wobble into the STATIC WORLD ONLY, whose signature is static geometry
//     boiling at pixel level while moving objects look fine.
//   * NO depth_inverted. PosN.z is RAW DeviceZ in UE's own fallback; reversed-Z is already baked
//     into ClipToPrevClip, which was built from the same projections that produced the depth
//     buffer. depth_inverted is an NGX parameter and nothing else.
//   * NO .zw. VELOCITY_ENCODE_DEPTH packs a depth delta there on some paths; DLSS does not want it
//     and Common.ush:1552 writes zeros there on others. Reading it would be a coin flip.
//   * NO dilation by default. UE's own TAA dilates the velocity LOOKUP to the nearest-depth
//     neighbour in a cross (TAAStandalone.usf:1939-1983) for its own single-tap history, and
//     NVIDIA ships a dilated variant but defaults to the NON-dilated branch. DLSS does its own
//     neighbourhood work, so pre-dilating smears silhouettes. It is implemented behind
//     mvec_dilate=1 so it can be A/B'd, and it is off.
//
// =================================================================================================
// VALIDATED ON THIS HOST
// =================================================================================================
// The maths below was replayed natively in C++ before a line of it was shipped - the same thing
// that was done for the codec's identity property. Bit patterns, the encode/decode round trip
// through real unorm16 quantisation, the zero sentinel, and the camera reprojection against a
// synthetic reversed-Z ClipToPrevClip built from two real view-projection pairs, checked against
// ground truth computed independently from the two projections. Numbers are in the README.
// The one thing a host replay CANNOT settle is the DLSS sign convention; see above.

#pragma once

#include "reshade_compat.hpp"
#include "hdr_codec.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mvec_decode {

using namespace reshade::api;

// Same three levels hdr_codec uses, so one log functor serves both.
enum { log_info = hdr_codec::log_info, log_warn = hdr_codec::log_warn, log_error = hdr_codec::log_error };

// ---- g_flags bits ---------------------------------------------------------------------------
// Bit 0 is THE key A/B: with it clear the pass is a naive decode and an invalid texel becomes
// EXACTLY ZERO, which isolates half (a) from half (b) on hardware. Shipping it clear would hand
// DLSS zero motion for the whole static world, so it defaults set.
static constexpr uint32_t kFlagReconstruct   = 1u << 0;
static constexpr uint32_t kFlagDilate        = 1u << 1;
static constexpr uint32_t kFlagForceRecon    = 1u << 2;   // debug: ignore the velocity texture
static constexpr uint32_t kFlagForceDecode   = 1u << 3;   // debug: never reconstruct

// =============================================================================================
// The shader.
// =============================================================================================
//
// t0/t1 are THE GAME'S OWN SRVs, pushed straight back through push_descriptors. Nothing is
// created on a game resource, which is the standing rule (stray_dlssnr.cpp:1639-1643) - that rule
// is about CACHING a descriptor across frames, and these are consumed inside the very event in
// which the game bound them. It also means NO BARRIER is needed or issued on either: they are
// bound as SRVs to the compute shader that just executed, so they already carry
// NON_PIXEL_SHADER_RESOURCE, and a transition whose StateBefore cannot be derived exactly is a
// worse hazard than none (the same reasoning as stray_dlssnr.cpp:2594-2596).
static const char *const kDecodeSource = R"HLSL(
Texture2D<float4>   InVelocity : register(t0);   // the GAME's GBufferVelocityTexture, encoded
Texture2D<float4>   InDepth    : register(t1);   // the GAME's scene depth; .x is raw DeviceZ
RWTexture2D<float2> OutMVec    : register(u0);   // OURS, r16g16_float, colour-grid extent

cbuffer MvecArgs : register(b0)
{
	uint2  g_outSize;       // c0.xy  colour-grid extent == the dispatch domain
	uint2  g_velSize;       // c0.zw  velocity texture extent
	uint2  g_depthSize;     // c1.xy  depth texture extent
	float2 g_viewMin;       // c1.zw  ViewRectMin, colour-grid pixels  (ASSUMED (0,0), see host)
	float2 g_viewSize;      // c2.xy  ViewSizeAndInvSize.xy
	float2 g_invViewSize;   // c2.zw  1 / g_viewSize
	uint   g_flags;         // c3.x
	uint   g_pad0;          // c3.y
	uint   g_pad1;          // c3.z
	uint   g_pad2;          // c3.w
	// View.ClipToPrevClip, FOUR RAW float4 CB ROWS, row-major, NOT transposed on the CPU.
	// g_clipToPrevClip[r] is FMatrix::M[r][0..3].
	float4 g_clipToPrevClip[4];   // c4..c7
};

#define MVEC_FLAG_RECONSTRUCT   (1u << 0)
#define MVEC_FLAG_DILATE        (1u << 1)
#define MVEC_FLAG_FORCE_RECON   (1u << 2)
#define MVEC_FLAG_FORCE_DECODE  (1u << 3)

// UE 4.27 Common.ush:1556-1570. Written as the same EXPRESSIONS UE writes so any compiler folds
// them to the same immediates - InvDiv to 0x408041AB (which is the constant Gate B already
// matched in STRAY's own bytecode) and the folded MAD bias to 0x4000412B.
//
// DO NOT "simplify" 32767/65535 to 0.5. The whole point of 0.499 and 32767/65535 is that the
// cleared texel (0,0) stays OUTSIDE the encodable range so it can be the "nothing was written
// here" sentinel. Common.ush:1539.
static const float kMvInvDiv = 1.0f / (0.499f * 0.5f);
static const float kMvBias   = 32767.0f / 65535.0f;

float2 mvDecodeVelocityXY(float2 encodedXY)
{
	return encodedXY * kMvInvDiv - (kMvBias * kMvInvDiv);
}

// A bit-pattern test rather than isfinite(), for the same reason hdr_codec's nrAnyNotFinite is:
// no optimisation setting can fold it away. Exponent all-ones is inf or NaN, and we reject both.
bool mvAnyNotFinite(float2 v)
{
	const uint2 e = asuint(v) & 0x7F800000u;
	return any(e == 0x7F800000u);
}

// mul(v, M) for a ROW vector against a ROW-MAJOR M. Written out element by element on purpose:
// this reproduces UE's `mul(ThisClip, View.ClipToPrevClip)` from the CB's MEMORY LAYOUT alone, so
// no HLSL matrix-packing convention - this shader's or UE's - can silently change it.
// (UE compiles with D3D10_SHADER_PACK_MATRIX_ROW_MAJOR, D3DShaderCompiler.cpp:947-949, so CB row
// r really is M[r][0..3].)
float4 mvMulClipToPrevClip(float4 v)
{
	return v.x * g_clipToPrevClip[0]
	     + v.y * g_clipToPrevClip[1]
	     + v.z * g_clipToPrevClip[2]
	     + v.w * g_clipToPrevClip[3];
}

// Maps an output-grid pixel CENTRE onto a source texture of a possibly different extent.
// Proportional in BUFFER space, which is correct while ViewRectMin is (0,0) and every buffer
// covers the whole view - true of every extent measured in STRAY (all 1920x1080).
int2 mvRemap(uint2 px, uint2 dstExtent, uint2 srcExtent)
{
	const float2 uv = (float2(px) + 0.5f) / float2(max(dstExtent, uint2(1u, 1u)));
	const int2   mx = int2(max(srcExtent, uint2(1u, 1u))) - int2(1, 1);
	return clamp(int2(uv * float2(srcExtent)), int2(0, 0), mx);
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	const uint2 px = tid.xy;
	if (any(px >= g_outSize))
	{
		return;
	}

	const float2 centre = float2(px) + 0.5f;

	// UE ScreenPos of this pixel centre. Common.ush:1153-1156 ViewportUVToScreenPos: the UV is
	// y-DOWN and ScreenPos is y-UP, [-1,1] over the VIEW RECT. This is the UNJITTERED output-grid
	// position, exactly what TAAStandalone.usf:2237/:1888 feeds ClipToPrevClip. See the header
	// comment on why no jitter term belongs here.
	const float2 viewportUV = (centre - g_viewMin) * g_invViewSize;
	const float2 screenPos  = float2(2.0f * viewportUV.x - 1.0f, 1.0f - 2.0f * viewportUV.y);

	int2  vpx     = mvRemap(px, g_outSize, g_velSize);
	int2  dpx     = mvRemap(px, g_outSize, g_depthSize);
	float deviceZ = InDepth.Load(int3(dpx, 0)).x;   // .r - TAAStandalone.usf:1315

	// ---- optional: UE's AA_CROSS nearest-depth dilation ---------------------------------------
	// TAAStandalone.usf:1939-1983. It moves WHICH velocity texel is read and WHICH DeviceZ is
	// reprojected, and deliberately leaves ScreenPos alone (:1977 is commented out in the engine
	// source). DEFAULT OFF - see the header comment.
	if ((g_flags & MVEC_FLAG_DILATE) != 0u)
	{
		const int  C  = 2;                      // AA_CROSS
		const int2 lo = int2(0, 0);
		const int2 hi = int2(max(g_depthSize, uint2(1u, 1u))) - int2(1, 1);

		float4 d;
		d.x = InDepth.Load(int3(clamp(dpx + int2(-C, -C), lo, hi), 0)).x;
		d.y = InDepth.Load(int3(clamp(dpx + int2( C, -C), lo, hi), 0)).x;
		d.z = InDepth.Load(int3(clamp(dpx + int2(-C,  C), lo, hi), 0)).x;
		d.w = InDepth.Load(int3(clamp(dpx + int2( C,  C), lo, hi), 0)).x;

		// Reversed-Z: nearest is the LARGEST value. UE 4.27 is unconditionally reversed-Z
		// (SceneView.cpp:651 static_assert, and TAAStandalone.usf:1981 is `#error Fix me!` on the
		// non-inverted branch), so there is no non-inverted case to write here.
		int2 off  = int2(C, C);
		int  offX = C;
		if (d.x > d.y) offX  = -C;
		if (d.z > d.w) off.x = -C;
		const float dXY = max(d.x, d.y);
		const float dZW = max(d.z, d.w);
		if (dXY > dZW) { off.y = -C; off.x = offX; }
		const float dAll = max(dXY, dZW);
		if (dAll > deviceZ)
		{
			const int2 vhi = int2(max(g_velSize, uint2(1u, 1u))) - int2(1, 1);
			vpx     = clamp(vpx + off, lo, vhi);
			deviceZ = dAll;
		}
	}

	const float4 encoded = InVelocity.Load(int3(vpx, 0));

	// ==========================================================================================
	// THE BRANCH.  TAAStandalone.usf:2003-2009, and five other UE 4.27 consumers spell it the
	// same way:   if (EncodedVelocity.x > 0.0)
	// RED channel, STRICT >, on the RAW ENCODED sample. Not .y, not any(), not the decoded value.
	// ==========================================================================================
	bool dynamicN = (encoded.x > 0.0f);
	if ((g_flags & MVEC_FLAG_FORCE_RECON)  != 0u) dynamicN = false;
	if ((g_flags & MVEC_FLAG_FORCE_DECODE) != 0u) dynamicN = true;

	// BackN: an NDC delta, y-UP, CURRENT MINUS PREVIOUS. Both branches produce this same
	// quantity in these same units - UE assigns one into the other with no conversion.
	float2 backN = float2(0.0f, 0.0f);

	if (dynamicN)
	{
		backN = mvDecodeVelocityXY(encoded.xy);
		if (mvAnyNotFinite(backN))
		{
			backN = float2(0.0f, 0.0f);
		}
	}
	else if ((g_flags & MVEC_FLAG_RECONSTRUCT) != 0u)
	{
		// THE SPARSE FALLBACK - see the header comment. Without this the static world, the sky,
		// translucency and every unmoved movable get ZERO motion.
		//
		// DeviceZ == 0 is the far plane under reversed-Z and is NOT special-cased: the previous
		// frame's translation drops out there and the reprojection correctly reduces to
		// rotation-only, which IS the right sky motion vector. UE has no far-plane case either.
		const float4 thisClip = float4(screenPos, deviceZ, 1.0f);
		const float4 prevClip = mvMulClipToPrevClip(thisClip);

		// UE's own code does not guard this because it rejects the result downstream with an
		// off-screen test and we cannot. A point behind the PREVIOUS camera gives a huge or
		// infinite vector, and one infinity in the guide poisons the network's history for the
		// rest of the run. Written !( > ) so a NaN w takes this branch.
		if (!(prevClip.w > 1e-6f))
		{
			OutMVec[px] = float2(0.0f, 0.0f);
			return;
		}

		backN = screenPos - (prevClip.xy / prevClip.w);
		if (mvAnyNotFinite(backN))
		{
			backN = float2(0.0f, 0.0f);
		}
	}
	else
	{
		// mvec_reconstruct=0: the deliberate A/B baseline. Invalid texels are EXACTLY ZERO.
		OutMVec[px] = float2(0.0f, 0.0f);
		return;
	}

	// ---- THE OUTPUT CONTRACT ------------------------------------------------------------------
	//   mvec = (prev - curr) in y-DOWN pixels = backN * (-0.5*W, +0.5*H)
	// X IS NEGATED, Y IS NOT. See the header comment; getting this "obviously" right is the
	// documented failure mode.
	float2 mvec = float2(-0.5f * g_viewSize.x * backN.x,
	                      0.5f * g_viewSize.y * backN.y);

	// Nothing legitimate exceeds one screen of motion in a single frame, and the destination is
	// fp16: clamp rather than let an outlier reach an infinity the network then propagates.
	mvec = clamp(mvec, -g_viewSize, g_viewSize);

	OutMVec[px] = mvec;
}
)HLSL";

// =============================================================================================
// Root-constant block. Laid out so HLSL cbuffer packing is a straight dword-for-dword copy:
// every 2-vector pair fills one float4 register and the array starts on register 4 (byte 64).
// =============================================================================================
struct mvec_args
{
	uint32_t out_w = 0,     out_h = 0;
	uint32_t vel_w = 0,     vel_h = 0;
	uint32_t depth_w = 0,   depth_h = 0;
	float    view_min_x = 0.0f,  view_min_y = 0.0f;
	float    view_size_x = 0.0f, view_size_y = 0.0f;
	float    inv_view_x = 0.0f,  inv_view_y = 0.0f;
	uint32_t flags = 0;
	uint32_t pad0 = 0, pad1 = 0, pad2 = 0;
	float    clip[16] = {};
};
static_assert(sizeof(mvec_args) == 128, "mvec_args must be exactly 32 root constants");

static constexpr uint32_t kMvecConstantCount = 32;

// Same three root parameters, in the same order, as hdr_codec - so hdr_codec::kParamSrvTable etc.
// would work too; they are restated here so a reader of this file does not have to cross-check.
static constexpr uint32_t kParamSrvTable  = 0;
static constexpr uint32_t kParamUavTable  = 1;
static constexpr uint32_t kParamConstants = 2;

// =============================================================================================
// Build. Reuses hdr_codec's compile/cache/override machinery verbatim - the ONE thing that was
// added there is a feature-name parameter, so the messages say "motion-vector decode" instead of
// "HDR codec". full_source()/source_hash() are untouched, so the codec's cache FILENAMES are
// unchanged, which is the exact regression test for that edit.
//
// There is no shared prelude here: the shader is self-contained, so the hash is taken over the
// source exactly as written above.
// =============================================================================================
template <typename LogFn>
inline bool build(const std::wstring &dir, std::vector<uint8_t> &out, LogFn log)
{
	return hdr_codec::build_blob(dir, L"stray_dlssnr_mvec", "shader", "motion-vector decode",
	                             std::string(kDecodeSource), out, log);
}

// =============================================================================================
// Pipeline layout and PSO. Identical in shape to hdr_codec's, for the reasons documented there:
// SRVs and UAVs MUST be separate root parameters (one push_descriptors call fills exactly one),
// constant_range::binding MUST be 0, and shader_desc::entry_point MUST be nullptr on D3D12.
// =============================================================================================
struct pipelines
{
	pipeline_layout layout = { 0 };
	pipeline        pso    = { 0 };
	bool            ok     = false;
};

inline void destroy(device *dev, pipelines &p)
{
	if (dev == nullptr)
	{
		p = pipelines();
		return;
	}
	if (p.pso.handle    != 0) dev->destroy_pipeline(p.pso);
	if (p.layout.handle != 0) dev->destroy_pipeline_layout(p.layout);
	p = pipelines();
}

template <typename LogFn>
inline bool create(device *dev, const std::vector<uint8_t> &dxbc, pipelines &p, LogFn log)
{
	p = pipelines();
	if (dev == nullptr || dxbc.empty())
		return false;

	const char *stage = nullptr;
	if      (!hdr_codec::make_layout(dev, 2, kMvecConstantCount, p.layout)) stage = "create_pipeline_layout(mvec)";
	else if (!hdr_codec::make_pipeline(dev, p.layout, dxbc, p.pso))         stage = "create_pipeline(mvec)";

	if (stage != nullptr)
	{
		char buf[512];
		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: the motion-vector decode pass could not be built - %s failed. The pass stays "
			"OFF for this run and DLSSNR.MVec falls back to the GAME'S RAW ENCODED velocity buffer "
			"with the derived grid scale, i.e. EXACTLY the behaviour before this feature existed "
			"(README gap 2). Nothing else changes.", stage);
		log(log_error, buf);
		destroy(dev, p);
		return false;
	}

	p.ok = true;
	return true;
}

// =============================================================================================
// CPU-side helpers. These exist so the log can state a CHECKED number rather than a hope, and so
// the same arithmetic the shader will run can be exercised on this host.
// =============================================================================================

// Row-major, row-vector: out[c] = sum_r v[r] * m[4*r + c]. Byte-for-byte the shader's
// mvMulClipToPrevClip, so a disagreement between them is a code change, not a convention.
inline void clip_transform(const float m[16], const float v[4], float out[4])
{
	for (int c = 0; c < 4; ++c)
		out[c] = v[0] * m[c] + v[1] * m[4 + c] + v[2] * m[8 + c] + v[3] * m[12 + c];
}

inline bool finite32(float v)
{
	uint32_t b;
	std::memcpy(&b, &v, sizeof(b));
	return (b & 0x7F800000u) != 0x7F800000u;
}

// The pixel motion the SHADER will produce at one screen position, computed here with the
// identical formula. This is the load-bearing log line: it is the only thing that distinguishes
// "we read the right 64 bytes and interpreted them the right way" from "we read 64 plausible
// floats".
inline void reconstruct_px(const float m[16], float screen_x, float screen_y, float device_z,
                           float view_w, float view_h, float out_px[2])
{
	const float v[4] = { screen_x, screen_y, device_z, 1.0f };
	float p[4];
	clip_transform(m, v, p);
	if (!(p[3] > 1e-6f) || !finite32(p[0]) || !finite32(p[1]) || !finite32(p[3]))
	{
		out_px[0] = out_px[1] = 0.0f;
		return;
	}
	const float bx = screen_x - p[0] / p[3];
	const float by = screen_y - p[1] / p[3];
	out_px[0] = bx * (-0.5f * view_w);
	out_px[1] = by * ( 0.5f * view_h);
}

// A cheap plausibility filter on the four rows just read out of the game's constant buffer. It is
// NOT a proof of correctness - only the hardware A/B can be that - it is a filter that refuses
// obvious garbage (a row of NaNs, an all-zero block, a matrix that throws the frame centre off to
// infinity) before it can reach the guide.
inline bool clip_plausible(const float m[16], float view_w, float view_h)
{
	bool all_zero = true;
	for (int i = 0; i < 16; ++i)
	{
		if (!finite32(m[i]))
			return false;
		if (m[i] != 0.0f)
			all_zero = false;
	}
	if (all_zero)
		return false;

	// Reprojecting the frame centre and the four corners must stay on a plausible screen at a
	// mid-range depth. Corners under a fast camera can legitimately leave [-1,1], so the bound is
	// deliberately loose - this rejects nonsense, not aggression.
	static const float kSamples[5][2] = { { 0.0f, 0.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f },
	                                      { -1.0f, -1.0f }, { 1.0f, -1.0f } };
	for (const auto &s : kSamples)
	{
		const float v[4] = { s[0], s[1], 0.5f, 1.0f };
		float p[4];
		clip_transform(m, v, p);
		if (!finite32(p[0]) || !finite32(p[1]) || !finite32(p[3]))
			return false;
		if (!(p[3] > 1e-6f))
			continue;   // behind the previous camera is legitimate, and the shader writes 0 there
		const float sx = p[0] / p[3], sy = p[1] / p[3];
		if (!finite32(sx) || !finite32(sy))
			return false;
		if (sx < -64.0f || sx > 64.0f || sy < -64.0f || sy > 64.0f)
			return false;
	}
	(void)view_w; (void)view_h;
	return true;
}

} // namespace mvec_decode
