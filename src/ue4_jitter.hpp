#pragma once
// ==================================================================================================
// ue4_jitter.hpp - recover Unreal Engine 4.27's TAA sub-pixel jitter from the View uniform buffer,
//                  in the sign and units NVIDIA's DLSS Super Resolution EvaluateFeature wants.
//
// WHAT THIS IS FOR
//   DLSS-NR (NGX feature 18) consumes already-resolved colour and has no jitter parameter at all.
//   DLSS-SR must be told the sub-pixel offset of the current frame, in RENDER-RESOLUTION PIXELS.
//   UE 4.27 bakes that offset into the projection matrix CPU-side and ships it to the GPU inside
//   the View uniform buffer, which on D3D12 is bound as a ROOT CBV - so it never enters a
//   descriptor heap and is reachable only through ReShade's push_descriptors event, which the
//   DLSS-NR add-on already records at every TAA dispatch (stray_dlssnr.cpp:936-946).
//
//   This header turns those recorded bytes into a jitter value, or into a clearly-named failure.
//   It never guesses.
//
// READ, DO NOT WRITE.
//   UE 4.27 bakes jitter into ProjectionMatrix CPU-side and it propagates into
//   TranslatedWorldToClip, WorldToClip, SVPositionToTranslatedWorld, ScreenPositionScaleBias and
//   per-primitive VS constants. Rewriting the constant buffer in flight desynchronises a dozen
//   derived quantities. Everything here is const.
//
// LAYERING
//   * The maths and the parsing are PURE: no Windows headers, no D3D12, no ReShade, no exceptions,
//     no allocation. They operate on a plain const byte pointer plus a length. That is what makes
//     jitter_selftest.cpp runnable on macOS under plain clang++.
//   * The D3D12 plumbing (Map / memcpy / a map-once pool cache) is compiled only when
//     UE4_JITTER_WITH_D3D12 is 1, which defaults to 1 on _WIN32 and 0 everywhere else.
//
// THE CONVENTION, AND WHERE IT COMES FROM
//
//   1. Where the jitter lives.
//      Engine/Source/Runtime/Engine/Public/SceneView.h:421-432
//          void HackAddTemporalAAProjectionJitter(const FVector2D& InTemporalAAProjectionJitter)
//          {
//              ensure(TemporalAAProjectionJitter.X == 0.0f && TemporalAAProjectionJitter.Y == 0.0f);
//              TemporalAAProjectionJitter = InTemporalAAProjectionJitter;
//              ProjectionMatrix.M[2][0] += TemporalAAProjectionJitter.X;
//              ProjectionMatrix.M[2][1] += TemporalAAProjectionJitter.Y;
//              InvProjectionMatrix = InvertProjectionMatrix(ProjectionMatrix);
//              RecomputeDerivedMatrices();
//          }
//      Those two matrix elements are the ONLY thing the jitter touches.
//
//   2. The pixel -> NDC scale, and the Y negation.
//      Engine/Source/Runtime/Renderer/Private/SceneVisibility.cpp:3329-3334
//          View.TemporalJitterPixels.X = SampleX;
//          View.TemporalJitterPixels.Y = SampleY;
//          View.ViewMatrices.HackAddTemporalAAProjectionJitter(
//              FVector2D(SampleX *  2.0f / View.ViewRect.Width(),
//                        SampleY * -2.0f / View.ViewRect.Height()));
//      so   M[2][0] = SampleX * ( 2 / W)          M[2][1] = SampleY * (-2 / H)
//      with W,H the RENDER view rect (post screen-percentage), which is what DLSS wants.
//
//   3. The float4 that carries it to the shader.
//      SceneView.h:624-625 declares, adjacent:
//          VIEW_UNIFORM_BUFFER_MEMBER(FMatrix,  ClipToPrevClip)
//          VIEW_UNIFORM_BUFFER_MEMBER(FVector4, TemporalAAJitter)
//      Engine/Source/Runtime/Engine/Private/SceneView.cpp:2500-2502
//          ViewUniformShaderParameters.TemporalAAJitter = FVector4(
//              InViewMatrices.GetTemporalAAJitter().X,     InViewMatrices.GetTemporalAAJitter().Y,
//              InPrevViewMatrices.GetTemporalAAJitter().X, InPrevViewMatrices.GetTemporalAAJitter().Y);
//      .xy is the CURRENT frame's NDC jitter, .zw the PREVIOUS frame's. NDC, not pixels.
//
//   4. The same numbers, already in pixels, two rows later in the same buffer.
//      Engine/Source/Runtime/Renderer/Private/SceneRendering.cpp:1517-1521
//          ViewUniformShaderParameters.TemporalAAParams = FVector4(
//              TemporalJitterIndex, TemporalJitterSequenceLength,
//              TemporalJitterPixels.X, TemporalJitterPixels.Y);
//      guarded immediately above by ensureMsgf(0 <= Index < Length) and (Length == 1 || TAA).
//
//   5. What DLSS is asking for.
//      nvsdk_ngx_helpers.h:411-414 (NVSDK_NGX_D3D12_DLSS_Eval_Params):
//          float InJitterOffsetX;   /* Jitter offset must be in input/render pixel space */
//          float InJitterOffsetY;
//      forwarded verbatim to "Jitter.Offset.X" / "Jitter.Offset.Y" at nvsdk_ngx_helpers.h:490-491.
//      The header does not state a sign convention. NVIDIA's own UE plugin does:
//          DLSS/Private/DLSSUpscaler.cpp:738   DLSSArguments.JitterOffset = Inputs.TemporalJitterPixels;
//          NGXD3D12RHI/Private/NGXD3D12RHI.cpp:213-214
//              EvalParams.InJitterOffsetX = InArguments.JitterOffset.X;
//              EvalParams.InJitterOffsetY = InArguments.JitterOffset.Y;
//      NO sign flip and NO scaling. Streamline's UE plugin does the same.
//
//   THEREFORE, and this is the whole contract of this header:
//
//       InJitterOffsetX = TemporalJitterPixels.X = TemporalAAParams.z =  TemporalAAJitter.x * W *  0.5f
//       InJitterOffsetY = TemporalJitterPixels.Y = TemporalAAParams.w =  TemporalAAJitter.y * H * -0.5f
//
//   The Y factor is negative because HackAddTemporalAAProjectionJitter applied -2/H; undoing it
//   restores UE's top-left-origin, Y-down pixel jitter, which is the space TAAStandalone.usf:1910
//   itself works in (`float2 PPCo = ViewportUV * InputViewSize.xy + TemporalJitterPixels;`) and the
//   space NVIDIA's VelocityCombine.usf uses. Both values are in [-0.75, +0.75] px in UE 4.27
//   (see kMaxSamplePixels below).
//
//   result::jitter_px is the value to hand DLSS. It is taken from TemporalAAParams (the engine's
//   own float, no arithmetic) whenever that row validates, and the NDC-derived value is then only
//   a cross-check. See status_tier.
//
// HOW A MEMBER IS LOCATED - CONTENT FIRST, OFFSETS SECOND
//   Shipping DXBC has reflection stripped (D3DShaderCompiler.cpp:796-805 passes
//   D3DCOMPILER_STRIP_REFLECTION_DATA), so there are no cbuffer member names at runtime. Nothing
//   here relies on a hardcoded offset being right; it relies on a content signature being found and
//   then uses the offsets only as *predictions* that must themselves check out.
//
//   THE SIGNATURE. ViewToClip (row 28) and ViewToClipNoAA (row 32) are adjacent declarations, and
//   ViewToClipNoAA is by construction the same matrix with the jitter removed:
//       SceneView.cpp:2413-2414   ViewToClip     = InViewMatrices.GetProjectionMatrix();
//                                 ViewToClipNoAA = InViewMatrices.GetProjectionNoAAMatrix();
//       SceneVisibility.cpp:3160  View.ViewMatrices.SaveProjectionNoAAMatrix();   // BEFORE HackAdd
//   So somewhere in the buffer there are two 4x4 matrices, 64 bytes apart, that are bit-identical
//   in 14 of their 16 elements, whose shared elements match UE's FPerspectiveMatrix skeleton
//   exactly (row0 = (Sx,0,0,0), row1 = (0,Sy,0,0), row2 = (jx,jy,Zw,1), row3 = (0,0,Zn,0)), and
//   where the SECOND one has exactly 0 in the two elements the first one may differ in. That is 12
//   exact structural constants plus 14 bit-exact equalities plus an adjacency - it is not something
//   16 bytes of unrelated float data produce by accident, and this header refuses to proceed if it
//   finds the signature more than once.
//
//   Everything else (ViewSizeAndInvSize, TemporalAAJitter, TemporalAAParams, ClipToPrevClip) is
//   PREDICTED from that anchor by the stock 4.27.2 deltas and then independently validated. The
//   member table is generated at compile time from the C++ declaration order
//   (ShaderParameters.cpp:213-224 emits explicit PrePadding so HLSL offsets == C++ offsets), so the
//   deltas are fixed for a given engine build - but they are NOT invariant across a licensee edit
//   to VIEW_UNIFORM_BUFFER_MEMBER_TABLE, which is exactly why they are checked rather than trusted.
//
// STOCK 4.27.2 LAYOUT - verified twice, independently
//   Read directly out of VIEW_UNIFORM_BUFFER_MEMBER_TABLE (SceneView.h:582-774) and recomputed by a
//   layout script over the same declaration list. Byte offsets, and float4 row = offset/16:
//       ViewToClip                    448   row  28
//       ViewToClipNoAA                512   row  32
//       ClipToPrevClip               1952   row 122   <-- the STRAY probe measured exactly this,
//                                                          by pure DXBC instruction analysis
//                                                          (shader_detect.hpp:703-776), with no
//                                                          reflection names involved
//       TemporalAAJitter             2016   row 126
//       ViewRectMin                  2064   row 129
//       ViewSizeAndInvSize           2080   row 130   (ends at 2096 - which is why the probe's
//                                                      `dcl_constantbuffer cb1[131]` reads 131:
//                                                      that is the highest row the SHADER indexes,
//                                                      NOT the buffer size)
//       LightProbeSizeRatioAndInvSizeRatio 2096 row 131  (== (1,1,1,1); a decoy for a naive
//                                                          "z == 1/x" scan - rejected here by the
//                                                          minimum-extent test)
//       BufferSizeAndInvSize         2112   row 132   (a REAL decoy: same (W,H,1/W,1/H) shape, and
//                                                      identical to ViewSizeAndInvSize whenever the
//                                                      scene buffer extent equals the view rect.
//                                                      This is the reason the view-size row is
//                                                      derived from the projection anchor and only
//                                                      scanned for as a diagnostic.)
//       TemporalAAParams             2432   row 152
//       (end of the constant portion) 5232   row 327
//
// WHAT IS VERIFIED AND WHAT IS NOT
//   VERIFIED IN SOURCE (UE 4.27.2, ++UE4+Release-4.27, read this session):
//     - every line and offset quoted above.
//   VERIFIED ON HARDWARE (STRAY, RTX 4090, Proton, by the DLSS-NR add-on):
//     - ClipToPrevClip at row 122 in STRAY's own TAA shader, by DXBC analysis. That is one
//       independent confirmation that STRAY's View UB is the stock table, at least up to row 126.
//     - that the root CBV for the View UB is captured at every TAA dispatch, with a real
//       (ID3D12Resource*, offset) pair into an 8 MiB upload pool.
//   ASSUMED, NOT MEASURED:
//     - that STRAY's table is stock PAST row 130 (TemporalAAParams at row 152 in particular).
//       Nothing here depends on that being true: if row 152 does not validate, the result is a
//       named failure, and status_tier::projection_only still yields a correct jitter from the
//       matrix pair alone.
//     - that STRAY's main view uses a centred perspective projection. An off-centre projection
//       (VR eye offset, a shadow or ortho matrix) would break is_ue4_projection_shape. Not a
//       concern for a single-view console-style title, but say it out loud.
//     - that a self-Map of the upload pool is safe under vkd3d-proton. Established elsewhere
//       (d3d12_resource_Map keeps no refcount, calls no vkMapMemory, and returns the persistent
//       mem.cpu_address; UPLOAD memory is unconditionally HOST_COHERENT so the invalidate/flush
//       both early-out) - but never exercised by this code on hardware.
//
// COST
//   The upload pool is WRITE-COMBINED, and on a 24 GB card under vkd3d-proton it is very likely
//   DEVICE_LOCAL | HOST_VISIBLE, i.e. ReBAR VRAM: a CPU read is a PCIe transaction. So:
//     - discover() reads the whole 5232-byte constant block. Call it ONCE per (shader, extent),
//       not per frame.
//     - evaluate() reads exactly five 16-byte rows through a caller-supplied reader - 80 bytes,
//       five cache lines. That is the per-frame path.
//     - NEVER call either from on_push_descriptors. UE 4.27 issues one of those per draw per root
//       CBV, on every parallel recording thread. Capture the buffer_range there; read here, once,
//       after the TAA gate has fired. (This is the same reasoning stray_dlssnr.cpp:940 already
//       applies to GetGPUVirtualAddress.)
//     - On write-combined memory MOVNTDQA (_mm_stream_load_si128 + _mm_lfence) pulls a whole
//       64-byte line through a fill buffer and is worth roughly 4x on the 5232-byte read. It is
//       deliberately NOT used here: mingw's baseline is not SSE4.1 and the discovery read happens
//       once. If discover() ever shows up in a profile, that is the knob.
//
// BUILD
//   C++17. Compiles clean under `x86_64-w64-mingw32-g++ -std=gnu++17 -O2 -Wall -Wextra` (the
//   add-on's own flags, build.sh:44-58) and under `clang++ -std=c++17 -Wall -Wextra` on macOS.
//   No ReShade header is included, and none may be: the point of the split is that the maths is
//   testable without a graphics API.
// ==================================================================================================

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef UE4_JITTER_WITH_D3D12
#  if defined(_WIN32)
#    define UE4_JITTER_WITH_D3D12 1
#  else
#    define UE4_JITTER_WITH_D3D12 0
#  endif
#endif

#if UE4_JITTER_WITH_D3D12
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <d3d12.h>
#  include <mutex>
#  include <unordered_map>
#endif

namespace ue4jitter {

// ==================================================================================================
// Layout constants (stock UE 4.27.2). Rows are float4 rows: byte offset / 16.
// ==================================================================================================

inline constexpr uint32_t kBytesPerRow = 16;

inline constexpr int32_t  kRowViewToClip         = 28;
inline constexpr int32_t  kRowViewToClipNoAA     = 32;
inline constexpr int32_t  kRowClipToPrevClip     = 122;
inline constexpr int32_t  kRowTemporalAAJitter   = 126;
inline constexpr int32_t  kRowViewSizeAndInvSize = 130;
inline constexpr int32_t  kRowTemporalAAParams   = 152;

// Size of the CONSTANT portion of FViewUniformShaderParameters. The resource ReShade's
// resolve_gpu_address hands back is the 8 MiB upload POOL, not this buffer, so this is the bound
// the caller must impose on its own read - never the pool size.
inline constexpr uint32_t kViewCbConstantBytes = 5232;
inline constexpr uint32_t kViewCbConstantRows  = kViewCbConstantBytes / kBytesPerRow; // 327

// Deltas from the ViewToClip anchor. These, not the absolute rows above, are what the code uses.
inline constexpr int32_t kDeltaNoAA     = kRowViewToClipNoAA     - kRowViewToClip; //   4
inline constexpr int32_t kDeltaClip     = kRowClipToPrevClip     - kRowViewToClip; //  94
inline constexpr int32_t kDeltaJitter   = kRowTemporalAAJitter   - kRowViewToClip; //  98
inline constexpr int32_t kDeltaViewSize = kRowViewSizeAndInvSize - kRowViewToClip; // 102
inline constexpr int32_t kDeltaParams   = kRowTemporalAAParams   - kRowViewToClip; // 124

// Largest |SampleX| / |SampleY| any UE 4.27 jitter sequence produces, in pixels.
// SceneVisibility.cpp:3225-3325 enumerates every pattern:
//     mobile          8/16   = 0.500
//     2xMSAA          4/16   = 0.250
//     3xMSAA          2/3    = 0.667   <-- the maximum, and the reason this is not 0.5
//     4xMSAA          6/16   = 0.375
//     5-sample        1/2    = 0.500
//     TemporalUpscale Halton(i,2) - 0.5 in [-0.5, 0.5]
//     default (8)     Box-Muller, explicitly windowed to [-0.5, 0.5]
// The MainUpsampling config this project targets is the Halton branch, so 0.5 would do - 0.75 is
// chosen so that flipping r.TemporalAASamples in a test does not turn into a mystery rejection.
inline constexpr float kMaxSamplePixels = 0.75f;

// Relative tolerance on the two reciprocals in ViewSizeAndInvSize (~2 ULP at float precision).
// UE computes them as `1.0f / float(Width)` (SceneView.cpp:2292), which is exactly what is
// recomputed here, so exact equality would very probably hold - a couple of ULP of slack costs
// nothing and survives a differently-configured floating-point build.
inline constexpr float kReciprocalTolerance = 2.4e-7f;

// Absolute tolerance, in pixels, on the TemporalAAParams.zw <-> NDC cross-check. The engine
// computes the NDC value as (Sample * 2.0f) / W - two roundings away from Sample * W * 0.5f - so
// the round trip is good to a few times 1e-7 at |Sample| <= 0.75. 1e-4 is a deliberately loose
// "these are the same number" test, not a precision claim.
inline constexpr float kPixelCrossCheckTolerance = 1e-4f;

// ==================================================================================================
// Status and check reporting
// ==================================================================================================

enum class status : uint32_t
{
	ok = 0,

	fail_bad_arguments,          // null pointer, or fewer than 9 float4 rows supplied
	fail_no_projection_pair,     // the ViewToClip / ViewToClipNoAA signature is not in the buffer
	fail_ambiguous_projection,   // it is in the buffer more than once - refuse rather than pick
	fail_no_view_size,           // ViewSizeAndInvSize is not where the anchor predicts, and no
	                             // caller-supplied extent was available to stand in for it
	fail_extent_mismatch,        // the buffer's view rect is not the extent the caller expected
	fail_jitter_out_of_range,    // |NDC jitter| exceeds kMaxSamplePixels * 2 / extent
	fail_jitter_row_mismatch,    // TemporalAAJitter.xy != the projection matrix's own m20/m21
	fail_params_row_invalid,     // TemporalAAParams.xy is not a plausible (index, length)
	fail_params_row_mismatch,    // TemporalAAParams.zw is not the NDC jitter converted to pixels
	fail_layout_not_stock,       // an anchor validated but a predicted row did not, and the
	                             // config did not permit a degraded tier
	fail_reader,                 // the caller's row reader refused a row

	status_count
};

inline const char *status_text(status s)
{
	switch (s)
	{
	case status::ok:                        return "ok";
	case status::fail_bad_arguments:        return "bad arguments (null buffer, or fewer than 9 float4 rows)";
	case status::fail_no_projection_pair:   return "no ViewToClip/ViewToClipNoAA pair found - this is not a UE4 View uniform buffer, or the projection is not a centred perspective";
	case status::fail_ambiguous_projection: return "more than one ViewToClip/ViewToClipNoAA pair found - refusing to choose";
	case status::fail_no_view_size:         return "ViewSizeAndInvSize is not at the row the projection anchor predicts, and no render extent was supplied to stand in for it";
	case status::fail_extent_mismatch:      return "the View uniform buffer's view rect does not match the render extent the caller expected";
	case status::fail_jitter_out_of_range:  return "the NDC jitter exceeds what any UE 4.27 sample pattern can produce at this extent";
	case status::fail_jitter_row_mismatch:  return "TemporalAAJitter.xy does not equal the projection matrix's own M[2][0]/M[2][1]";
	case status::fail_params_row_invalid:   return "TemporalAAParams.xy is not a plausible (jitter index, sequence length)";
	case status::fail_params_row_mismatch:  return "TemporalAAParams.zw is not TemporalAAJitter.xy converted to render pixels";
	case status::fail_layout_not_stock:     return "the projection anchor validated but a predicted member row did not - this View uniform buffer is not the stock 4.27 table";
	case status::fail_reader:               return "the caller's row reader refused a row";
	default:                                return "unknown";
	}
}

// How much of the buffer actually corroborated the answer. Strictly decreasing confidence.
enum class status_tier : uint32_t
{
	none = 0,
	// Everything agreed: matrix pair, ViewSizeAndInvSize, TemporalAAJitter, TemporalAAParams.
	// jitter_px is the ENGINE'S OWN float (TemporalAAParams.zw), unmodified.
	full = 1,
	// Matrix pair + ViewSizeAndInvSize + TemporalAAJitter agreed; TemporalAAParams did not
	// validate. jitter_px is computed from the NDC value. Requires config::require_params == false.
	no_params = 2,
	// Only the matrix pair was found; the view extent came from the caller. jitter_px is computed
	// from m20/m21 and the caller's extent. Requires config::allow_projection_only == true.
	// This is the tier Luma's D3D11 path operates at, and it is still correct - it just has no
	// second opinion. Use it as a diagnostic, not as a shipping default.
	projection_only = 3,
};

inline const char *tier_text(status_tier t)
{
	switch (t)
	{
	case status_tier::full:            return "full (matrix pair + ViewSizeAndInvSize + TemporalAAJitter + TemporalAAParams all agree)";
	case status_tier::no_params:       return "no_params (TemporalAAParams did not validate; jitter derived from NDC)";
	case status_tier::projection_only: return "projection_only (matrix pair only; extent supplied by the caller)";
	default:                           return "none";
	}
}

// Individual predicates, reported as a bitmask so a partial failure is diagnosable from one log
// line rather than from a bisect.
namespace check {
inline constexpr uint32_t projection_pair    = 1u << 0; // the adjacent ViewToClip/NoAA signature
inline constexpr uint32_t pair_unique        = 1u << 1; // exactly one such pair in the buffer
inline constexpr uint32_t view_size_row      = 1u << 2; // (W,H,1/W,1/H) at anchor + 102
inline constexpr uint32_t extent_matches     = 1u << 3; // W,H agree with the caller's expectation
inline constexpr uint32_t jitter_in_range    = 1u << 4; // |j| <= kMaxSamplePixels * 2 / extent
inline constexpr uint32_t jitter_row_echo    = 1u << 5; // TemporalAAJitter.xy == (m20, m21)
inline constexpr uint32_t no_aa_is_zero      = 1u << 6; // ViewToClipNoAA M[2][0..1] == 0
inline constexpr uint32_t params_row_sane    = 1u << 7; // 0 <= index < length, both integral
inline constexpr uint32_t params_row_matches = 1u << 8; // params.zw == NDC converted to pixels
inline constexpr uint32_t clip_row_agrees    = 1u << 9; // anchor + 94 == the probe's DXBC-derived
                                                        // ClipToPrevClip row (only run when the
                                                        // caller supplies it)
} // namespace check

// ==================================================================================================
// Configuration
// ==================================================================================================

struct config
{
	// The render extent DLSS will be fed. 0 means "unknown - take it from the buffer".
	//
	// Supply it whenever you have it: it is the only thing that can catch "we read the right shape
	// out of the WRONG constant buffer". If expected_is_texture_extent is true it is compared
	// against QuantizeSceneBufferSize(view rect) - RenderUtils.cpp:1480-1490 rounds the scene
	// buffer extent UP to a multiple of 4, so at r.ScreenPercentage=58.8 the view rect is 1130x636
	// while the texture is 1132x636 and a naive equality test would reject the right buffer.
	uint32_t expected_render_width      = 0;
	uint32_t expected_render_height     = 0;
	bool     expected_is_texture_extent = true;

	// The probe's DXBC-derived ClipToPrevClip row (shader_detect.hpp
	// TAAShaderInfo::clip_to_prev_clip_start_index). < 0 means "not supplied". When supplied it is
	// checked against anchor + 94, which is a completely independent corroboration of the layout:
	// one number from instruction analysis, one from a content signature.
	int32_t  dxbc_clip_to_prev_clip_row = -1;

	// Hard bound on the scan and on every row read. Never let this exceed the number of rows the
	// caller actually copied out of the pool.
	uint32_t max_rows = kViewCbConstantRows;

	// Tier gates. Both default to the strict setting: a failure is reported rather than a
	// less-corroborated number.
	bool require_params          = true;   // false permits status_tier::no_params
	bool allow_projection_only   = false;  // true permits status_tier::projection_only

	// Largest sample offset, in pixels, the jitter is allowed to have. See kMaxSamplePixels.
	float max_sample_pixels = kMaxSamplePixels;
};

// The row indices discovered once and then reused every frame. All absolute, all in float4 rows
// from the start of the View constant buffer.
struct layout
{
	bool    valid = false;
	int32_t row_view_to_clip     = -1;  // anchor
	int32_t row_view_to_clip_noaa= -1;  // anchor + 4
	int32_t row_clip_to_prev_clip= -1;  // anchor + 94   (predicted; not read by this header)
	int32_t row_jitter           = -1;  // anchor + 98
	int32_t row_view_size        = -1;  // anchor + 102, or -1 when the extent came from the caller
	int32_t row_params           = -1;  // anchor + 124, or -1 when it did not validate

	// Cached from discovery so the per-frame path does not have to re-derive them. The view rect
	// cannot change without a new discovery pass, because a resolution change reallocates
	// everything the add-on keys on anyway.
	uint32_t render_width  = 0;
	uint32_t render_height = 0;

	status_tier tier = status_tier::none;
};

struct result
{
	status      st   = status::fail_bad_arguments;
	status_tier tier = status_tier::none;

	uint32_t checks_run    = 0;   // bitmask of check:: values that were evaluated
	uint32_t checks_passed = 0;   // bitmask of check:: values that held

	// The two numbers to hand DLSS. Render-pixel space, UE / NVIDIA sign convention, |v| <= 0.75.
	float jitter_px_x = 0.0f;
	float jitter_px_y = 0.0f;

	// The NDC values, straight out of the buffer, and the pixel values derived from them. When
	// tier == full, jitter_px is TemporalAAParams.zw and jitter_px_from_ndc is the cross-check;
	// the two agree to kPixelCrossCheckTolerance. Below full, they are the same number.
	float jitter_ndc_x = 0.0f;
	float jitter_ndc_y = 0.0f;
	float jitter_px_from_ndc_x = 0.0f;
	float jitter_px_from_ndc_y = 0.0f;

	// TemporalAAJitter.zw - the PREVIOUS frame's NDC jitter. Only meaningful at tier full/no_params.
	float prev_jitter_ndc_x = 0.0f;
	float prev_jitter_ndc_y = 0.0f;
	bool  have_prev_jitter  = false;

	// TemporalAAParams.xy. Only meaningful at tier full.
	uint32_t jitter_index           = 0;
	uint32_t jitter_sequence_length = 0;

	uint32_t render_width  = 0;
	uint32_t render_height = 0;

	// The jitter is EXACTLY zero. Legitimate (r.TemporalAASamples == 1, or TAA off), and a correct
	// answer - but if it is true every frame then the game is not jittering and DLSS-SR will
	// produce a soft, aliased image with no error anywhere. Worth a one-shot warning.
	bool jitter_is_zero = false;

	// TemporalAAJitter.zw == .xy, bit for bit. SceneVisibility.cpp:3337-3342 and :3396-3398 make
	// PrevViewMatrices a copy of ViewMatrices on every frame where bResetCamera is true, so this
	// is a complete reset signal - it covers bCameraCut, first frame, time reset, large camera
	// movement and bForceCameraVisibilityReset, none of which reach the View UB's own CameraCut
	// float (SceneView.cpp:2524 sets that from View.bCameraCut alone). Feed it to
	// NVSDK_NGX_Parameter_Reset. Only meaningful at tier full/no_params.
	bool reset_signalled = false;

	// The discovered layout, so a caller can cache it. Same content as the `layout` out-parameter.
	layout discovered;
};

// ==================================================================================================
// Pure float helpers. No <cmath>: isfinite/fabs under an aggressive -ffast-math would be a silent
// hole in the validation, and bit inspection is both free and immune to it.
// ==================================================================================================

inline bool finite_f(float v)
{
	uint32_t bits;
	std::memcpy(&bits, &v, sizeof(bits));
	return (bits & 0x7f800000u) != 0x7f800000u;   // rejects inf and NaN; denormals are fine
}

inline float abs_f(float v) { return v < 0.0f ? -v : v; }

inline bool near_rel(float a, float b, float rel)
{
	if (!finite_f(a) || !finite_f(b))
		return false;
	return abs_f(a - b) <= rel * abs_f(b);
}

inline bool near_abs(float a, float b, float tol)
{
	if (!finite_f(a) || !finite_f(b))
		return false;
	return abs_f(a - b) <= tol;
}

// Row-major 4x4, matching the cbuffer: m[r * 4 + c] == FMatrix::M[r][c].
//
// Row-major is not an assumption. D3DShaderCompiler.cpp:947-949 compiles every UE shader with
//     D3D10_SHADER_PACK_MATRIX_ROW_MAJOR   // "Unpack uniform matrices as row-major to match the CPU layout."
// so M[2][0] is at matrix_base + 32 bytes and M[2][1] at +36.
struct mat4 { float m[16]; };

inline void load_row(const uint8_t *cb, int32_t row, float out[4])
{
	std::memcpy(out, cb + static_cast<size_t>(row) * kBytesPerRow, 4 * sizeof(float));
}

inline void load_mat(const uint8_t *cb, int32_t row, mat4 &out)
{
	std::memcpy(out.m, cb + static_cast<size_t>(row) * kBytesPerRow, 16 * sizeof(float));
}

// UE's FPerspectiveMatrix / FReversedZPerspectiveMatrix skeleton:
//     row 0 = (Sx,  0,  0,  0)
//     row 1 = ( 0, Sy,  0,  0)
//     row 2 = (jx, jy, Zw,  1)      <- jx/jy are the jitter, and are the ONLY free elements here
//     row 3 = ( 0,  0, Zn,  0)
// Twelve of the sixteen elements are structural constants. Note this deliberately REJECTS an
// orthographic or off-centre projection: for STRAY's single perspective main view that is the
// right trade, because a false accept here is a wrong jitter and a false reject is a clean bail.
inline bool is_ue4_projection_shape(const mat4 &p)
{
	for (int i = 0; i < 16; ++i)
		if (!finite_f(p.m[i]))
			return false;

	if (p.m[1] != 0.0f || p.m[2]  != 0.0f || p.m[3]  != 0.0f) return false; // row 0
	if (p.m[4] != 0.0f || p.m[6]  != 0.0f || p.m[7]  != 0.0f) return false; // row 1
	if (p.m[11] != 1.0f)                                      return false; // row 2 w
	if (p.m[12] != 0.0f || p.m[13] != 0.0f || p.m[15] != 0.0f) return false; // row 3

	// Sx = MultFOVX / tan(HalfFOVX), Sy likewise. Anything outside this is not a camera.
	const float sx = abs_f(p.m[0]);
	const float sy = abs_f(p.m[5]);
	if (sx < 0.01f || sx > 100.0f) return false;
	if (sy < 0.01f || sy > 100.0f) return false;

	return true;
}

// The signature described in the file header: `a` at row R, `b` at row R+4, identical except that
// `b` has exactly zero in M[2][0] and M[2][1].
inline bool is_projection_pair(const mat4 &a, const mat4 &b)
{
	if (b.m[8] != 0.0f || b.m[9] != 0.0f)
		return false;
	if (!is_ue4_projection_shape(b) || !is_ue4_projection_shape(a))
		return false;
	for (int i = 0; i < 16; ++i)
	{
		if (i == 8 || i == 9)
			continue;
		if (a.m[i] != b.m[i])
			return false;
	}
	return true;
}

// (W, H, 1/W, 1/H) with W,H integral and the reciprocals real.
// SceneView.cpp:2292:
//     ViewSizeAndInvSize = FVector4(Width, Height, 1.0f / float(Width), 1.0f / float(Height));
// The 64-pixel floor rejects LightProbeSizeRatioAndInvSizeRatio, which is literally (1,1,1,1)
// (SceneView.cpp:2295) and would otherwise pass the reciprocal test trivially.
inline bool is_view_size_row(const float r[4], uint32_t &w, uint32_t &h)
{
	for (int i = 0; i < 4; ++i)
		if (!finite_f(r[i]))
			return false;
	if (r[0] < 64.0f || r[0] > 32768.0f) return false;
	if (r[1] < 64.0f || r[1] > 32768.0f) return false;

	const float fw = static_cast<float>(static_cast<int32_t>(r[0]));
	const float fh = static_cast<float>(static_cast<int32_t>(r[1]));
	if (fw != r[0] || fh != r[1])
		return false;
	if (!near_rel(r[2], 1.0f / fw, kReciprocalTolerance)) return false;
	if (!near_rel(r[3], 1.0f / fh, kReciprocalTolerance)) return false;

	w = static_cast<uint32_t>(fw);
	h = static_cast<uint32_t>(fh);
	return true;
}

// RenderUtils.cpp:1480-1490, verbatim.
inline uint32_t quantize_scene_buffer_dim(uint32_t v)
{
	const uint32_t dividable_by = 4;
	return (v + dividable_by - 1) & ~(dividable_by - 1);
}

// THE CONVERSION. See the file header for the derivation and the citations.
//     x_px =  ndc_x * W *  0.5      y_px = ndc_y * H * -0.5
inline void ndc_jitter_to_pixels(float ndc_x, float ndc_y, uint32_t w, uint32_t h,
                                 float &px_x, float &px_y)
{
	px_x = ndc_x * static_cast<float>(w) *  0.5f;
	px_y = ndc_y * static_cast<float>(h) * -0.5f;
}

// ==================================================================================================
// The validation core.
//
// Both entry points funnel through finish(), so the expensive discovery path and the cheap
// per-frame path apply exactly the same predicates to exactly the same five rows. There is no
// second, laxer copy of the rules.
// ==================================================================================================

// The five float4 rows any answer is built from. 80 bytes.
struct raw_rows
{
	float proj_row2 [4] = { 0, 0, 0, 0 };  // ViewToClip     M[2][0..3]  -> .x/.y are the jitter
	float noaa_row2 [4] = { 0, 0, 0, 0 };  // ViewToClipNoAA M[2][0..3]  -> .x/.y must be zero
	float view_size [4] = { 0, 0, 0, 0 };  // ViewSizeAndInvSize
	float jitter    [4] = { 0, 0, 0, 0 };  // TemporalAAJitter  (curr.xy, prev.zw)
	float params    [4] = { 0, 0, 0, 0 };  // TemporalAAParams  (index, length, px, py)

	bool has_view_size = false;
	bool has_jitter    = false;
	bool has_params    = false;
};

inline bool finish(const raw_rows &rows, const layout &lay, const config &cfg, result &out)
{
	out = result{};
	out.discovered = lay;
	out.tier       = status_tier::none;

	// ---------------------------------------------------------------- the anchor still holds
	//
	// On the cheap per-frame path this is what catches "the cached layout no longer applies" - a
	// different shader, a different constant buffer, or an engine that moved under us. The correct
	// response to fail_layout_not_stock here is to throw the layout away and run discover() again,
	// NOT to relax anything.
	out.checks_run |= check::no_aa_is_zero;
	if (rows.noaa_row2[0] != 0.0f || rows.noaa_row2[1] != 0.0f ||
	    !finite_f(rows.proj_row2[0]) || !finite_f(rows.proj_row2[1]))
	{
		out.st = status::fail_layout_not_stock;
		return false;
	}
	out.checks_passed |= check::no_aa_is_zero;

	out.jitter_ndc_x = rows.proj_row2[0];
	out.jitter_ndc_y = rows.proj_row2[1];
	out.jitter_is_zero = (out.jitter_ndc_x == 0.0f && out.jitter_ndc_y == 0.0f);

	// ---------------------------------------------------------------- the render extent
	uint32_t w = 0, h = 0;
	bool extent_from_buffer = false;

	if (rows.has_view_size && is_view_size_row(rows.view_size, w, h))
	{
		out.checks_run    |= check::view_size_row;
		out.checks_passed |= check::view_size_row;
		extent_from_buffer = true;
	}
	else if (rows.has_view_size)
	{
		out.checks_run |= check::view_size_row;   // ran and failed
	}

	if (!extent_from_buffer)
	{
		if (cfg.expected_render_width == 0 || cfg.expected_render_height == 0)
		{
			out.st = status::fail_no_view_size;
			return false;
		}
		w = cfg.expected_render_width;
		h = cfg.expected_render_height;
	}

	out.render_width  = w;
	out.render_height = h;

	// Compare the buffer's view rect against what the caller expects. QuantizeSceneBufferSize
	// rounds the TEXTURE extent up to a multiple of 4, so at some screen percentages the texture
	// is up to 3 pixels wider than the view rect it holds; comparing the two naively rejects the
	// right buffer. expected_is_texture_extent selects the right comparison.
	if (extent_from_buffer && cfg.expected_render_width != 0 && cfg.expected_render_height != 0)
	{
		out.checks_run |= check::extent_matches;
		const bool match = cfg.expected_is_texture_extent
			? (quantize_scene_buffer_dim(w) == cfg.expected_render_width &&
			   quantize_scene_buffer_dim(h) == cfg.expected_render_height)
			: (w == cfg.expected_render_width && h == cfg.expected_render_height);
		if (!match)
		{
			out.st = status::fail_extent_mismatch;
			return false;
		}
		out.checks_passed |= check::extent_matches;
	}

	// ---------------------------------------------------------------- magnitude bound
	//
	// |M[2][0]| = |SampleX * 2 / W| <= max_sample_pixels * 2 / W. This is the half of Luma's
	// ProjectionHasJitter that actually discriminates once the shape test has run, and it is the
	// one predicate that catches "we matched a projection-shaped matrix belonging to some other
	// view" (a shadow cascade, an off-centre eye) whose skew is larger than any jitter can be.
	{
		out.checks_run |= check::jitter_in_range;
		const float bound_x = cfg.max_sample_pixels * 2.0f / static_cast<float>(w) * 1.0009765625f;
		const float bound_y = cfg.max_sample_pixels * 2.0f / static_cast<float>(h) * 1.0009765625f;
		if (abs_f(out.jitter_ndc_x) > bound_x || abs_f(out.jitter_ndc_y) > bound_y)
		{
			out.st = status::fail_jitter_out_of_range;
			return false;
		}
		out.checks_passed |= check::jitter_in_range;
	}

	ndc_jitter_to_pixels(out.jitter_ndc_x, out.jitter_ndc_y, w, h,
	                     out.jitter_px_from_ndc_x, out.jitter_px_from_ndc_y);
	out.jitter_px_x = out.jitter_px_from_ndc_x;
	out.jitter_px_y = out.jitter_px_from_ndc_y;

	// ---------------------------------------------------------------- TemporalAAJitter
	//
	// TemporalAAJitter.xy and ProjectionMatrix.M[2][0..1] are the SAME float: HackAdd… stores the
	// value it added into TemporalAAProjectionJitter, and SceneView.cpp:2500 copies that member
	// straight into the float4. Value equality, not a tolerance - if these differ at all, the two
	// rows are not the two members they are supposed to be.
	bool jitter_row_ok = false;
	if (rows.has_jitter)
	{
		out.checks_run |= check::jitter_row_echo;
		if (rows.jitter[0] == out.jitter_ndc_x && rows.jitter[1] == out.jitter_ndc_y)
		{
			out.checks_passed |= check::jitter_row_echo;
			jitter_row_ok = true;

			out.prev_jitter_ndc_x = rows.jitter[2];
			out.prev_jitter_ndc_y = rows.jitter[3];
			out.have_prev_jitter  = true;

			// TemporalAAJitter.zw is PrevViewMatrices' jitter, and on every frame where
			// bResetCamera holds, SceneVisibility.cpp:3396-3398 assigns PrevViewInfo = the current
			// frame's matrices - so .zw == .xy exactly. That is the whole reset signal.
			out.reset_signalled = (rows.jitter[2] == out.jitter_ndc_x &&
			                       rows.jitter[3] == out.jitter_ndc_y);
		}
	}

	// ---------------------------------------------------------------- TemporalAAParams
	bool params_ok = false;
	if (rows.has_params && jitter_row_ok)
	{
		out.checks_run |= check::params_row_sane;

		const float idx = rows.params[0];
		const float len = rows.params[1];
		const bool integral =
			finite_f(idx) && finite_f(len) &&
			idx == static_cast<float>(static_cast<int32_t>(idx)) &&
			len == static_cast<float>(static_cast<int32_t>(len));

		// SceneRendering.cpp:1513-1516 ensures exactly this, one statement before the assignment.
		if (integral && len >= 1.0f && len <= 4096.0f && idx >= 0.0f && idx < len)
		{
			out.checks_passed |= check::params_row_sane;
			out.jitter_index           = static_cast<uint32_t>(idx);
			out.jitter_sequence_length = static_cast<uint32_t>(len);

			out.checks_run |= check::params_row_matches;
			if (near_abs(rows.params[2], out.jitter_px_from_ndc_x, kPixelCrossCheckTolerance) &&
			    near_abs(rows.params[3], out.jitter_px_from_ndc_y, kPixelCrossCheckTolerance))
			{
				out.checks_passed |= check::params_row_matches;
				params_ok = true;

				// PREFER THE ENGINE'S OWN FLOAT. TemporalAAParams.zw is TemporalJitterPixels
				// verbatim, which is exactly what NVIDIA's DLSS plugin passes to
				// InJitterOffsetX/Y. The NDC path stays as jitter_px_from_ndc, as the cross-check
				// that just passed.
				out.jitter_px_x = rows.params[2];
				out.jitter_px_y = rows.params[3];
			}
		}
	}

	// ---------------------------------------------------------------- the probe cross-check
	if (cfg.dxbc_clip_to_prev_clip_row >= 0 && lay.row_clip_to_prev_clip >= 0)
	{
		out.checks_run |= check::clip_row_agrees;
		if (lay.row_clip_to_prev_clip == cfg.dxbc_clip_to_prev_clip_row)
			out.checks_passed |= check::clip_row_agrees;
		// Deliberately NOT fatal on its own. The probe's index comes from a backwards window scan
		// over cb element indices (shader_detect.hpp:757-771) and reports the FIRST four-consecutive
		// window it finds from the end; a shader that reads a second 4x4 from the View UB could
		// legitimately move it. A disagreement is a loud warning, not a refusal - and every other
		// predicate here is stronger than this one.
	}

	// ---------------------------------------------------------------- tier and verdict
	if (extent_from_buffer && jitter_row_ok && params_ok)
	{
		out.tier = status_tier::full;
		out.st   = status::ok;
		out.discovered.tier = out.tier;
		return true;
	}

	if (extent_from_buffer && jitter_row_ok)
	{
		if (cfg.require_params)
		{
			// Name which half failed, so the log says something actionable.
			out.st = (out.checks_run & check::params_row_sane) &&
			         !(out.checks_passed & check::params_row_sane)
			             ? status::fail_params_row_invalid
			             : ((out.checks_run & check::params_row_matches) &&
			                !(out.checks_passed & check::params_row_matches)
			                    ? status::fail_params_row_mismatch
			                    : status::fail_layout_not_stock);
			return false;
		}
		out.tier = status_tier::no_params;
		out.st   = status::ok;
		out.discovered.tier = out.tier;
		return true;
	}

	// The jitter echo was run and failed - that is a hard stop at any tier that reads rows past the
	// anchor, because it means the predicted TemporalAAJitter row is not TemporalAAJitter.
	if ((out.checks_run & check::jitter_row_echo) && !(out.checks_passed & check::jitter_row_echo))
	{
		if (!cfg.allow_projection_only)
		{
			out.st = status::fail_jitter_row_mismatch;
			return false;
		}
	}
	else if (extent_from_buffer && !rows.has_jitter && cfg.require_params)
	{
		out.st = status::fail_layout_not_stock;
		return false;
	}

	if (!cfg.allow_projection_only)
	{
		out.st = status::fail_layout_not_stock;
		return false;
	}

	// projection_only. Correct, but corroborated by nothing except the matrix pair itself.
	out.have_prev_jitter = false;
	out.reset_signalled  = false;
	out.tier = status_tier::projection_only;
	out.st   = status::ok;
	out.discovered.tier = out.tier;
	return true;
}

// ==================================================================================================
// Entry point 1 - DISCOVERY. Reads the whole constant block. Call once per (shader, extent).
//
//   cb_bytes  a CPU-readable copy of the View constant buffer, starting at its own byte 0. That is
//             `pool_base + buffer_range.offset` - see read_view_cb() below, and note that
//             buffer_range.size from ReShade is hard-coded to UINT64_MAX and carries no
//             information (d3d12_command_list.cpp:645-647), so the length must come from the
//             caller: min(kViewCbConstantBytes, pool_size - offset).
//   size      how many bytes of that are readable.
// ==================================================================================================
inline bool discover(const void *cb_bytes, size_t size, const config &cfg, layout &lay, result &out)
{
	lay = layout{};
	out = result{};

	if (cb_bytes == nullptr || size < 9 * kBytesPerRow)
	{
		out.st = status::fail_bad_arguments;
		return false;
	}

	const uint8_t *const cb = static_cast<const uint8_t *>(cb_bytes);

	uint32_t rows_available = static_cast<uint32_t>(size / kBytesPerRow);
	if (rows_available > cfg.max_rows)
		rows_available = cfg.max_rows;
	if (rows_available < 9)
	{
		out.st = status::fail_bad_arguments;
		return false;
	}

	// ---------------------------------------------------------------- find the projection pair
	int32_t  anchor = -1;
	uint32_t matches = 0;
	for (int32_t r = 0; r + 8 <= static_cast<int32_t>(rows_available); ++r)
	{
		mat4 a, b;
		load_mat(cb, r, a);
		load_mat(cb, r + kDeltaNoAA, b);
		if (!is_projection_pair(a, b))
			continue;
		if (matches == 0)
			anchor = r;
		matches++;
		if (matches > 1)
			break;   // ambiguity is decided; no reason to keep scanning a WC page
	}

	out.checks_run |= check::projection_pair | check::pair_unique;
	if (matches == 0)
	{
		out.st = status::fail_no_projection_pair;
		return false;
	}
	out.checks_passed |= check::projection_pair;
	if (matches > 1)
	{
		out.st = status::fail_ambiguous_projection;
		return false;
	}
	out.checks_passed |= check::pair_unique;

	// ---------------------------------------------------------------- predict the rest
	lay.valid                 = true;
	lay.row_view_to_clip      = anchor;
	lay.row_view_to_clip_noaa = anchor + kDeltaNoAA;
	lay.row_clip_to_prev_clip = anchor + kDeltaClip;
	lay.row_jitter            = (anchor + kDeltaJitter   < static_cast<int32_t>(rows_available)) ? anchor + kDeltaJitter   : -1;
	lay.row_view_size         = (anchor + kDeltaViewSize < static_cast<int32_t>(rows_available)) ? anchor + kDeltaViewSize : -1;
	lay.row_params            = (anchor + kDeltaParams   < static_cast<int32_t>(rows_available)) ? anchor + kDeltaParams   : -1;

	raw_rows rows;
	std::memcpy(rows.proj_row2, cb + (static_cast<size_t>(anchor) + 2) * kBytesPerRow, 16);
	std::memcpy(rows.noaa_row2, cb + (static_cast<size_t>(anchor) + kDeltaNoAA + 2) * kBytesPerRow, 16);
	if (lay.row_view_size >= 0) { load_row(cb, lay.row_view_size, rows.view_size); rows.has_view_size = true; }
	if (lay.row_jitter    >= 0) { load_row(cb, lay.row_jitter,    rows.jitter);    rows.has_jitter    = true; }
	if (lay.row_params    >= 0) { load_row(cb, lay.row_params,    rows.params);    rows.has_params    = true; }

	const uint32_t scan_checks_run    = out.checks_run;
	const uint32_t scan_checks_passed = out.checks_passed;

	const bool ok = finish(rows, lay, cfg, out);

	// finish() resets `out`, so fold the scan's own two checks back in.
	out.checks_run    |= scan_checks_run;
	out.checks_passed |= scan_checks_passed;

	if (!ok)
	{
		lay.valid = false;
		return false;
	}

	// Rows that did not contribute are cleared, so a cached layout can never make the per-frame
	// path read something the discovery pass did not stand behind.
	if (!(out.checks_passed & check::view_size_row))   lay.row_view_size = -1;
	if (!(out.checks_passed & check::jitter_row_echo)) lay.row_jitter    = -1;
	if (!(out.checks_passed & check::params_row_matches)) lay.row_params = -1;

	lay.render_width  = out.render_width;
	lay.render_height = out.render_height;
	lay.tier          = out.tier;
	out.discovered    = lay;
	return true;
}

// ==================================================================================================
// Entry point 2 - THE PER-FRAME PATH. Five 16-byte reads through a caller-supplied row reader.
//
// RowReader is any callable  bool(int32_t row, float out[4])  that returns false if it will not or
// cannot serve that row. Keeping it a template rather than a function pointer is what lets the
// D3D12 side read straight out of a mapped write-combined page with no indirection, and lets the
// self-test drive the same code from a plain array.
// ==================================================================================================
template <typename RowReader>
inline bool evaluate(RowReader &&read_row, const layout &lay, const config &cfg, result &out)
{
	out = result{};
	if (!lay.valid || lay.row_view_to_clip < 0 || lay.row_view_to_clip_noaa < 0)
	{
		out.st = status::fail_bad_arguments;
		return false;
	}

	raw_rows rows;
	if (!read_row(lay.row_view_to_clip + 2, rows.proj_row2) ||
	    !read_row(lay.row_view_to_clip_noaa + 2, rows.noaa_row2))
	{
		out.st = status::fail_reader;
		return false;
	}
	if (lay.row_view_size >= 0)
	{
		if (!read_row(lay.row_view_size, rows.view_size)) { out.st = status::fail_reader; return false; }
		rows.has_view_size = true;
	}
	if (lay.row_jitter >= 0)
	{
		if (!read_row(lay.row_jitter, rows.jitter)) { out.st = status::fail_reader; return false; }
		rows.has_jitter = true;
	}
	if (lay.row_params >= 0)
	{
		if (!read_row(lay.row_params, rows.params)) { out.st = status::fail_reader; return false; }
		rows.has_params = true;
	}

	return finish(rows, lay, cfg, out);
}

// A row reader over a flat CPU-readable copy. Bounds-checked; never reads past `rows`.
struct bytes_row_reader
{
	const uint8_t *base = nullptr;
	int32_t        rows = 0;

	bool operator()(int32_t row, float out[4]) const
	{
		if (base == nullptr || row < 0 || row >= rows)
			return false;
		load_row(base, row, out);
		return true;
	}
};

// Convenience: discover + evaluate over one buffer. This is the bring-up path, not the shipping
// path - the shipping path caches the layout and reads 80 bytes a frame.
inline bool extract(const void *cb_bytes, size_t size, const config &cfg, layout &lay, result &out)
{
	return discover(cb_bytes, size, cfg, lay, out);
}

// ==================================================================================================
// Cross-frame validator.
//
// TemporalAAJitter.zw is the previous frame's .xy (SceneView.cpp:2500-2502). So on a normal frame
//     jitter[n].zw == jitter[n-1].xy      bit for bit
// and nothing else in the buffer has that property. If it holds you are certainly reading the right
// member of the right buffer, and when it does NOT hold because .zw == .xy instead, that is the
// reset frame (result::reset_signalled). Free, and stronger than anything a single frame can say.
//
// Note the one legitimate way this reports a break with nothing wrong: a frame that never reached
// the TAA pass at all (a paused or menu frame that skipped the dispatch), so the add-on simply did
// not sample the sequence. Treat a break as "reset DLSS", not as "the extraction is broken".
// ==================================================================================================
class echo_validator
{
public:
	enum class verdict { first_frame, continuous, reset_frame, broken };

	verdict submit(const result &r)
	{
		if (r.st != status::ok || !r.have_prev_jitter)
		{
			have_prev_ = false;
			return verdict::broken;
		}

		verdict v;
		if (!have_prev_)
			v = verdict::first_frame;
		else if (r.prev_jitter_ndc_x == prev_x_ && r.prev_jitter_ndc_y == prev_y_)
			v = r.reset_signalled ? verdict::reset_frame : verdict::continuous;
		else
			v = verdict::broken;

		prev_x_ = r.jitter_ndc_x;
		prev_y_ = r.jitter_ndc_y;
		have_prev_ = true;
		return v;
	}

	void forget() { have_prev_ = false; }

	static const char *verdict_text(verdict v)
	{
		switch (v)
		{
		case verdict::first_frame: return "first_frame";
		case verdict::continuous:  return "continuous";
		case verdict::reset_frame: return "reset_frame";
		default:                   return "BROKEN - this frame's TemporalAAJitter.zw is not last frame's .xy";
		}
	}

private:
	float prev_x_ = 0.0f;
	float prev_y_ = 0.0f;
	bool  have_prev_ = false;
};

// ==================================================================================================
// One-line diagnostic. Safe to call on a failure; every field it names is either valid or zero.
// ==================================================================================================
inline void describe(const result &r, char *buf, size_t n)
{
	if (buf == nullptr || n == 0)
		return;
	std::snprintf(buf, n,
		"status=%s tier=%s jitter_px=(%.6f, %.6f) ndc=(%.9f, %.9f) from_ndc=(%.6f, %.6f) "
		"extent=%ux%u index=%u/%u prev_ndc=(%.9f, %.9f) reset=%d zero=%d checks=%03x/%03x rows{proj=%d noaa=%d clip=%d jitter=%d size=%d params=%d}",
		status_text(r.st), tier_text(r.tier),
		static_cast<double>(r.jitter_px_x), static_cast<double>(r.jitter_px_y),
		static_cast<double>(r.jitter_ndc_x), static_cast<double>(r.jitter_ndc_y),
		static_cast<double>(r.jitter_px_from_ndc_x), static_cast<double>(r.jitter_px_from_ndc_y),
		r.render_width, r.render_height, r.jitter_index, r.jitter_sequence_length,
		static_cast<double>(r.prev_jitter_ndc_x), static_cast<double>(r.prev_jitter_ndc_y),
		r.reset_signalled ? 1 : 0, r.jitter_is_zero ? 1 : 0,
		r.checks_passed, r.checks_run,
		r.discovered.row_view_to_clip, r.discovered.row_view_to_clip_noaa,
		r.discovered.row_clip_to_prev_clip, r.discovered.row_jitter,
		r.discovered.row_view_size, r.discovered.row_params);
	buf[n - 1] = '\0';
}

// ==================================================================================================
// D3D12 PLUMBING. Compiled only when UE4_JITTER_WITH_D3D12 is 1 (the default on _WIN32).
//
// Nothing above this line touches it, which is the point: the maths is testable on a machine with
// no Windows SDK.
//
// WHAT RESHADE HANDS THE ADD-ON, AND WHAT IT MEANS
//   on_push_descriptors already stores a full buffer_range per root parameter
//   (stray_dlssnr.cpp:936-946, descriptor_shadow.hpp:114). Three properties of it, all verified in
//   ReShade's own source, decide the code below:
//
//   1. buffer_range.buffer.handle IS the ID3D12Resource*, with no encoding.
//      d3d12_impl_type_convert.hpp:182  `inline auto to_handle(ID3D12Resource *ptr)
//                                        { return api::resource { reinterpret_cast<uintptr_t>(ptr) }; }`
//      The add-on already relies on this at d3d12_state.hpp:335.
//
//   2. buffer_range.size is ALWAYS UINT64_MAX and carries no information.
//      d3d12_command_list.cpp:645-647 sets it unconditionally. Never use it as a bound.
//
//   3. The resource is the 8 MiB upload POOL, not the constant buffer. resolve_gpu_address
//      (d3d12_impl_device.cpp:2196) returns whichever registered buffer CONTAINS the address, and
//      UE sub-allocates: D3D12RHIPrivate.h:114 `#define DEFAULT_CONTEXT_UPLOAD_POOL_SIZE (8*1024*1024)`
//      = 8388608, which is exactly the buffer_size the probe logged. So `offset` is the offset of
//      the View CB inside that pool, and the readable window is
//          [offset, offset + min(kViewCbConstantBytes, pool_size - offset))
//      and NOTHING outside it. Other constant buffers live on either side of it and the CPU is
//      writing them concurrently.
//
// WHICH ROOT PARAMETER IS THE VIEW UNIFORM BUFFER
//   Do not hardcode param 4. The add-on's deep copy of the pipeline layout already carries the
//   shader register: on_init_pipeline_layout (stray_dlssnr.cpp:396) keeps
//   probe::layout_param::ranges, and ReShade fills range.dx_register_index from
//   D3D12_ROOT_DESCRIPTOR::ShaderRegister (d3d12_device.cpp:3230, and the root-signature-1.1 branch
//   at :3243). The View UB is the root parameter whose single range is
//       type == constant_buffer && dx_register_index == 1        (b1; b0 is $Globals)
//   That is a structural answer, not a content guess. Reading $Globals instead would be the worst
//   possible failure: it accumulates into a persistent shadow array with
//   CurrentUpdateSize = Max(...), so it carries STALE BYTES from earlier passes and would produce
//   plausible-looking numbers from the wrong frame.
//
// WHY A RECORD-TIME READ IS THE RIGHT FRAME'S DATA
//   D3D12UniformBuffer.cpp:11-61 memcpys the contents ONCE, at buffer creation, strictly before any
//   SetComputeRootConstantBufferView can name that address; FD3D12FastConstantAllocator::Allocate
//   (D3D12Allocation.cpp:1717) is a monotone bump within a page, so the sub-range is never
//   revisited; and page/block reuse is gated on FrameFence.IsFenceComplete (D3D12Allocation.cpp:358,
//   :420, :1626, :1680), so memory bound into an in-flight command list cannot be recycled.
//   Separately, NGX consumes jitter as a CPU-side float parameter at record time - so the game's
//   dispatch and our evaluate are fed from the same record-time snapshot by construction. No fence,
//   no readback, no frame-lag reasoning enters.
//
// THE OBLIGATION THAT COMES WITH pool_map_cache
//   It caches a raw ID3D12Resource*. The add-on does NOT currently register
//   addon_event::destroy_resource. It must, and it must call forget() there, or a
//   destroyed-and-reallocated pool at the same address is a use-after-free. This is the same hazard
//   class ReShade guards inside unregister_resource with its identity check
//   (d3d12_impl_device.cpp:2128).
// ==================================================================================================

#if UE4_JITTER_WITH_D3D12

// A single bounded read out of an UPLOAD buffer, with the narrowest ranges D3D12 can express.
//
// pReadRange narrows what the runtime must invalidate; pWrittenRange = {0,0} (End <= Begin) means
// "nothing was written", which suppresses the flush on every runtime. ReShade's own
// device::map_buffer_region cannot express either - it passes nullptr for both
// (d3d12_impl_device.cpp:630), i.e. "the entire 8 MiB subresource" - which is why this is written
// natively rather than through the API.
//
// On vkd3d-proton both ranges are dead code anyway: UPLOAD memory is unconditionally
// HOST_COHERENT on every path through vkd3d_memory_info_upload_hvv_memory_properties
// (resource.c:10991-11040), so d3d12_resource_invalidate_range / _flush_range early-out, and
// d3d12_resource_Map reduces to handing back the persistent mem.cpu_address with no refcount and no
// vkMapMemory. On native D3D12 the memory is write-combined, which has no cache to invalidate, so
// the cost is small there too. The narrow ranges cost nothing and are correct on both.
//
// RE-ENTRANCY, and why this is a deliberate choice rather than an oversight:
//   ReShade installs a hook on ID3D12Resource::Map ONLY if some add-on registered
//   addon_event::map_buffer_region (d3d12_device.cpp:2305). This add-on registers none, so today
//   res->Map() is the raw driver entry point. But that is a property of the whole PROCESS: a
//   co-loaded RenoDX or Luma that wants map events would turn this into a GetDevice QI + GetDesc +
//   a broadcast into every add-on, on a hot recording thread (d3d12_resource.cpp:43). If that ever
//   becomes a real configuration, switch to the map-once shape below - after the first frame it
//   makes no Map call at all - or route through device::map_buffer_region, which deliberately
//   bypasses ReShade's own hook via hooks::call_vtable and would need no ABI thunk (it returns
//   bool, not an aggregate; see msvc_abi.hpp and build.sh:26-38).
inline bool read_view_cb(ID3D12Resource *res, uint64_t offset, uint32_t bytes, void *dst)
{
	if (res == nullptr || dst == nullptr || bytes == 0)
		return false;

	void *base = nullptr;
	const D3D12_RANGE read_range = {
		static_cast<SIZE_T>(offset),
		static_cast<SIZE_T>(offset + bytes)
	};
	if (FAILED(res->Map(0, &read_range, &base)) || base == nullptr)
		return false;

	// Map returns the base of the SUBRESOURCE, never the base of pReadRange.
	std::memcpy(dst, static_cast<const uint8_t *>(base) + offset, bytes);

	const D3D12_RANGE written_nothing = { 0, 0 };
	res->Unmap(0, &written_nothing);
	return true;
}

// The raw mapper used by pool_map_cache when the caller supplies none.
inline bool raw_map_whole_pool(ID3D12Resource *res, void **out_base)
{
	if (res == nullptr || out_base == nullptr)
		return false;
	void *p = nullptr;
	if (FAILED(res->Map(0, nullptr, &p)) || p == nullptr)
		return false;
	*out_base = p;
	return true;
}

// Map each upload pool ONCE and never unmap it.
//
// This removes Map from the per-frame path entirely, which removes the co-loaded-add-on hook
// question after the first call, and on vkd3d there is no refcount to hold open. Holding an
// outstanding Map is legal D3D12 and is exactly what UE itself does (D3D12Resources.h:173-187,
// D3D12Allocation.cpp:177 - it maps each pool once with ReadRange = nullptr and refcounts it
// itself). On native D3D12 the only consequence is that the application's final Unmap will not
// drop the count to zero.
class pool_map_cache
{
public:
	template <typename Mapper>
	void *base(ID3D12Resource *res, Mapper &&mapper)
	{
		if (res == nullptr)
			return nullptr;

		{
			std::lock_guard<std::mutex> lock(mutex_);
			const auto it = mapped_.find(res);
			if (it != mapped_.end())
				return it->second;
		}

		// Mapped outside the lock: on a first-frame race two threads may both map, which is
		// harmless (vkd3d hands back the same persistent address; native D3D12 refcounts).
		void *p = nullptr;
		if (!mapper(res, &p) || p == nullptr)
			return nullptr;

		std::lock_guard<std::mutex> lock(mutex_);
		const auto ins = mapped_.emplace(res, p);
		return ins.first->second;
	}

	void *base(ID3D12Resource *res) { return base(res, &raw_map_whole_pool); }

	// MUST be called from addon_event::destroy_resource. See the section comment above.
	void forget(ID3D12Resource *res)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		mapped_.erase(res);
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		mapped_.clear();
	}

	size_t size() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return mapped_.size();
	}

private:
	mutable std::mutex mutex_;
	std::unordered_map<ID3D12Resource *, void *> mapped_;
};

// A row reader straight over the mapped (write-combined, very possibly ReBAR VRAM) page.
//
// One 16-byte memcpy per row - one cache-line fill each, five per frame. Do NOT dereference this
// pointer repeatedly for individual floats: every load from write-combined memory is uncached, so
// four float loads cost four times what one 16-byte copy costs. finish() does all its arithmetic on
// the copies, which is why the reader shape is a copy-out and not a `const float*`.
struct mapped_row_reader
{
	const uint8_t *cb_base = nullptr;   // pool_base + buffer_range.offset
	int32_t        rows    = 0;         // min(kViewCbConstantRows, (pool_size - offset) / 16)

	bool operator()(int32_t row, float out[4]) const
	{
		if (cb_base == nullptr || row < 0 || row >= rows)
			return false;
		std::memcpy(out, cb_base + static_cast<size_t>(row) * kBytesPerRow, 4 * sizeof(float));
		return true;
	}
};

#endif // UE4_JITTER_WITH_D3D12

} // namespace ue4jitter
