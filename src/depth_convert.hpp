// depth_convert.hpp - README gap 3 and README gap 4 in ONE compute pass. It converts STRAY's
// TYPELESS, PLANAR depth-stencil into a dedicated, TYPED r32_float texture this add-on owns and
// binds as DLSSNR.Depth, and - while it is already reading every texel - it MEASURES which way
// round the depth convention actually is instead of asserting it.
//
// This is modelled on mvec_decode.hpp line for line, which is itself modelled on hdr_codec.hpp:
// HLSL as a string literal, D3DCompile to cs_5_0 at load through a LoadLibraryW'd d3dcompiler
// (never linked), a source-hash cache with a user override, its own texture, dispatched inside the
// existing capture/restore window - and it reuses those headers' compile/cache/layout machinery
// rather than duplicating it.
//
// =================================================================================================
// GAP 3 - WHY THE GAME'S OWN DEPTH RESOURCE CANNOT BE HANDED TO NGX
// =================================================================================================
//
// STRAY's TAA pass reads scene depth at t0 through an r32_float_x8_uint SRV over an r32_g8_typeless
// RESOURCE. That SRV is correct and fully typed - .x is DeviceZ, which is what TAAStandalone.usf
// :1315 reads - and it is what mvec_decode.hpp already consumes. The problem is one level down:
//
//   * On D3D12, NGX is handed a bare ID3D12Resource* and NOTHING ELSE. ngx_interop.hpp's set_res()
//     has no view-format channel because the ABI has none; the snippet reads the format straight
//     off D3D12_RESOURCE_DESC::Format. What it therefore sees for DLSSNR.Depth today is
//     DXGI_FORMAT_R32G8X24_TYPELESS - a typeless, TWO-PLANE format, not a depth value.
//     stray_dlssnr.cpp already WARNS about exactly this before the first evaluate.
//   * The Vulkan deployments this project cross-checks against do not have the problem, because
//     there the image VIEW carries the format.
//   * DLSS-SR does not have it either, and for a reason that is worth stating because it is the
//     counter-example that could otherwise be misread as evidence: the SR snippet has a SEPARATE
//     create-time parameter, DLSS.Use.HW.Depth, through which it can be TOLD the input is a
//     hardware depth-stencil (addon_config.hpp, sr_hw_depth). The DLSS-NR snippet exposes no such
//     key. So "DLSS-SR takes STRAY's depth resource happily" says nothing about DLSS-NR.
//   * The one known-working independent DLSS-NR deployment (DLSS5-Feeder + the renodx DLSS 5
//     add-on) NEVER hands NGX a game-owned typeless resource: it renders depth into its OWN R32F
//     target and hard-REJECTS any frame whose depth is not exactly r32_float. On the same machine
//     and the same game its log reports `depth R32_FLOAT (reversed)` and the feature initialises.
//
// A conversion pass is therefore the ONLY available fix - there is no channel to convert through.
//
// WHAT THIS IS AND IS NOT EVIDENCE FOR. Our evaluate returns Success today, so NGX is not
// REJECTING the typeless resource outright. What we measured instead is that the tuning parameters
// have no effect on the output while a getter trace proves the values reach the network - and a
// misread depth channel gives the denoiser a degenerate confidence/edge signal, which fits. That is
// a HYPOTHESIS, not a proof, and the only thing that can settle it is depth_convert=0 vs 1 on
// hardware. Both are one ini key apart, which is why this ships as a key rather than welded in.
//
// =================================================================================================
// GAP 4 - THE STATISTIC, AND WHY IT IS IN THIS SHADER RATHER THAN ITS OWN
// =================================================================================================
//
// depth_inverted has always been an INFERENCE ("UE 4.27 renders reversed-Z"), never a measurement
// against STRAY, and it is the OPPOSITE of the value the working Remix deployment uses. Getting it
// wrong produces no diagnostic anywhere: the evaluate still succeeds and the image is merely wrong.
//
// The two conventions are trivially separable from the DEPTH DISTRIBUTION ITSELF, because both are
// hyperbolic in view depth and the projection puts the mass at opposite ends:
//
//     reversed-Z  (UE 4.27):  near -> 1, far -> 0, and DeviceZ ~= Near/Z for an infinite far plane
//     standard-Z:             near -> 0, far -> 1, and DeviceZ ~= 1 - Near/Z
//
// So under reversed-Z, DeviceZ > 0.75 requires Z < 1.33 * Near - about 13 cm at UE's default near
// plane - which essentially nothing in a frame satisfies, while DeviceZ < 0.25 (Z > 4 * Near) is
// essentially the whole frame. Under standard-Z the two are exactly swapped. The test is therefore
// a COUNT of texels below 0.25 against a count above 0.75, and it is symmetric: neither convention
// is privileged and the same code decides both.
//
// It lives in THIS shader and not its own because this pass already reads every depth texel, and a
// second dispatch over the same texture would be a second full-resolution read for a number that is
// latched once per run. Only every 8th texel on each axis is sampled - 240x135 = 32400 samples at
// 1920x1080 - which is the same stride, and the same uint32 overflow argument, as nr_probe.hpp.
//
// WHAT THE STATISTIC REFUSES TO DECIDE FROM, and this is the half that matters:
//   * a frame with NO DEPTH RANGE. A cleared buffer, a loading screen or a sky-only view is a
//     CONSTANT, and a constant is consistent with both conventions - a reversed-Z clear is all
//     0.0 and a standard-Z clear is all 1.0, so the counts alone would confidently return the
//     clear value's convention having seen no geometry at all. The window is therefore rejected
//     unless max(DeviceZ) - min(DeviceZ) exceeds kMinSpread.
//   * a frame whose depth is not a normalised depth value at all. If more than kMaxBadFraction of
//     the samples are outside [0,1] or non-finite, the SRV we are reading is not what we think it
//     is, the whole gap-3 premise is wrong, and this says so loudly instead of deciding.
//   * a distribution that is not one-sided. Both counts must clear kMinSideFraction of the valid
//     samples AND beat the other side by kSideRatio.
//
// AND IT IS LATCHED, NOT PER-FRAME. A verdict that flipped mid-run would change the meaning of
// every accumulated frame in NGX's temporal history - strictly worse than a wrong constant. One
// window of kWindowFrames frames produces one verdict; the first verdict wins for the run. If a
// window declines, another is tried, up to kMaxWindows, after which the pass says so once and the
// configured value stands.
//
// =================================================================================================
// WHAT IS DELIBERATELY *NOT* IN HERE
// =================================================================================================
//   * NO LINEARISATION. The snippet wants DeviceZ, and mvec_decode.hpp's own reprojection reads
//     the same raw DeviceZ. Converting to view depth here would silently break both.
//   * NO REVERSED-Z FLIP. The output is DeviceZ VERBATIM. Flipping it here would DOUBLE-APPLY with
//     DLSSNR.DepthInverted, which is the NGX parameter that exists for exactly this and nothing
//     else - the same rule mvec_decode.hpp states for its own reprojection.
//   * NO FILTERING. Load(), never a sampler: a filtered depth texel is a blend across a silhouette
//     and is not a depth at all.
//   * NO STENCIL. Plane 1 is not read and is not forwarded. DLSS-NR has no stencil input.

#pragma once

#include "reshade_compat.hpp"
#include "hdr_codec.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace depth_convert {

using namespace reshade::api;

// Same three levels hdr_codec uses, so one log functor serves every pass in this add-on.
enum { log_info = hdr_codec::log_info, log_warn = hdr_codec::log_warn, log_error = hdr_codec::log_error };

// ---- the statistic's accumulator ---------------------------------------------------------------
// Eight uints in one r32_uint buffer, because a typed r32_uint UAV is the only thing that gives us
// InterlockedAdd/InterlockedMax without a structured-buffer view. Same shape, and the same reason,
// as nr_probe.hpp's four.
static constexpr uint32_t kStatSamples = 0;   // sampled texels, valid or not
static constexpr uint32_t kStatLo      = 1;   // count of DeviceZ <  kLoThreshold
static constexpr uint32_t kStatHi      = 2;   // count of DeviceZ >  kHiThreshold
static constexpr uint32_t kStatBad     = 3;   // count outside [0,1], or non-finite
static constexpr uint32_t kStatSum     = 4;   // sum of DeviceZ * kFixedScale, over VALID samples
static constexpr uint32_t kStatMax     = 5;   // max of DeviceZ * kQuantise
static constexpr uint32_t kStatMaxComp = 6;   // max of (kQuantise - DeviceZ * kQuantise)
static constexpr uint32_t kStatSlots   = 8;   // 7 used, rounded up; slot 7 is reserved and stays 0
static constexpr uint32_t kStatBytes   = kStatSlots * sizeof(uint32_t);

// Every 8th texel on both axes. At 1920x1080 that is 240x135 = 32400 samples, which is far more
// than enough for a count-based verdict and keeps the fixed-point sum inside uint32:
// 32400 * 1024 = 33.2M. A full-resolution sum would overflow, and so would a larger scale.
static constexpr uint32_t kStride     = 8;
static constexpr uint32_t kFixedScale = 1024;
// The min/max quantiser. BOTH extremes are accumulated with InterlockedMax - the minimum as
// max(kQuantise - q) - because clear_unordered_access_view_uint writes ONE value to every element
// of a typed buffer UAV, so there is no way to seed a slot with 0xFFFFFFFF for an InterlockedMin.
// Zero is the correct seed for both of these.
static constexpr uint32_t kQuantise = 65535;

// =============================================================================================
// The shader.
//
// t0 is THE GAME'S OWN DEPTH SRV, pushed straight back through push_descriptors - the same borrow
// mvec_decode already makes, under the same standing rule (stray_dlssnr.cpp:1639-1643): that rule
// is about CACHING a descriptor across frames, and this one is consumed inside the very event in
// which the game bound it. It also means NO BARRIER is issued on it: it is bound as an SRV to the
// compute shader that just executed, so it already carries NON_PIXEL_SHADER_RESOURCE, and a
// transition whose StateBefore cannot be derived exactly is a worse hazard than none.
//
// u0 is OUR r32_float target. u1 is the statistic. They share ONE descriptor table because one
// push_descriptors call fills exactly one root parameter; hdr_codec::make_layout takes the UAV
// count for this.
// =============================================================================================
static const char *const kConvertSource = R"HLSL(
Texture2D<float4>  InDepth  : register(t0);   // the GAME's scene depth; .x is raw DeviceZ
RWTexture2D<float> OutDepth : register(u0);   // OURS, r32_float, colour-grid extent
RWBuffer<uint>     Stats    : register(u1);   // OURS, r32_uint, kStatSlots elements

cbuffer DepthArgs : register(b0)
{
	uint2 g_outSize;      // c0.xy  dispatch domain == the extent OutDepth was created at
	uint2 g_srcSize;      // c0.zw  the game's depth extent
	uint  g_measure;      // c1.x   0 = convert only, no atomics are issued at all
	uint  g_stride;       // c1.y   sample every g_stride'th texel on both axes
	uint  g_fixedScale;   // c1.z
	uint  g_quantise;     // c1.w
};

// Maps an output-grid pixel CENTRE onto a source texture of a possibly different extent.
// Byte-for-byte mvec_decode.hpp's mvRemap, and correct for the same reason: proportional in BUFFER
// space while ViewRectMin is (0,0) and every buffer covers the whole view - true of every extent
// measured in STRAY (colour, depth and velocity are all 1920x1080), where it is the IDENTITY.
int2 dzRemap(uint2 px, uint2 dstExtent, uint2 srcExtent)
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

	const int2  spx = dzRemap(px, g_outSize, g_srcSize);
	const float z   = InDepth.Load(int3(spx, 0)).x;   // .r - TAAStandalone.usf:1315

	// A bit-pattern test rather than isfinite(), for the same reason hdr_codec's nrAnyNotFinite
	// and mvec_decode's mvAnyNotFinite are: no optimisation setting can fold it away. Exponent
	// all-ones is inf or NaN, and we reject both.
	const bool nonFinite = ((asuint(z) & 0x7F800000u) == 0x7F800000u);
	// The ORDER matters: every comparison against a NaN is false, so the range test alone would
	// pass a NaN through as "good".
	const bool bad = nonFinite || (z < 0.0f) || (z > 1.0f);

	// DeviceZ IS [0,1] by construction, so the saturate is free insurance and never fires on a
	// real texel. A non-finite value becomes 0 rather than saturate(NaN), which is not defined to
	// produce anything in particular. This is the ONLY place the value is touched at all: it is
	// written VERBATIM otherwise, because DLSSNR.DepthInverted - not this shader - is what carries
	// the reversed-Z convention.
	OutDepth[px] = nonFinite ? 0.0f : saturate(z);

	// ---- the gap-4 statistic ------------------------------------------------------------------
	// Skipped ENTIRELY once the verdict is latched: g_measure is 0 from then on, so the shipping
	// steady state issues no atomics at all. See the header comment for what the counters mean.
	if (g_measure != 0u && (px.x % g_stride) == 0u && (px.y % g_stride) == 0u)
	{
		InterlockedAdd(Stats[0], 1u);
		if (bad)
		{
			InterlockedAdd(Stats[3], 1u);
		}
		else
		{
			if (z < 0.25f) InterlockedAdd(Stats[1], 1u);
			if (z > 0.75f) InterlockedAdd(Stats[2], 1u);
			InterlockedAdd(Stats[4], (uint)(z * (float)g_fixedScale));
			const uint q = (uint)(z * (float)g_quantise);
			InterlockedMax(Stats[5], q);
			InterlockedMax(Stats[6], g_quantise - q);
		}
	}
}
)HLSL";

// The two thresholds the shader counts against, restated on the CPU so the log can print what was
// measured against what. They are NOT tunable: 0.25 and 0.75 in DeviceZ are Z > 4*Near and
// Z < 1.33*Near respectively, and moving either would move the wrong one of the two counts.
//
// THEY ARE NOT PASSED TO THE SHADER, they are DUPLICATED there as literals - deliberately, so the
// verdict cannot be changed by a root constant at runtime. The cost is that these two lines and the
// two comparisons in main() must be edited together: if they ever disagree, nothing breaks and the
// log simply reports a threshold that was not the one applied. Nothing else reads them.
static constexpr float kLoThreshold = 0.25f;
static constexpr float kHiThreshold = 0.75f;

// =============================================================================================
// Root-constant block. Laid out so HLSL cbuffer packing is a straight dword-for-dword copy: the
// two uint2s fill register c0 exactly and the four scalars fill c1.
// =============================================================================================
struct depth_args
{
	uint32_t out_w = 0,  out_h = 0;
	uint32_t src_w = 0,  src_h = 0;
	uint32_t measure = 0;
	uint32_t stride = 0;
	uint32_t fixed_scale = 0;
	uint32_t quantise = 0;
};
static_assert(sizeof(depth_args) == 32, "depth_args must be exactly 8 root constants");

static constexpr uint32_t kDepthConstantCount = 8;

// Same three root parameters, in the same order, as hdr_codec and mvec_decode. Restated here so a
// reader of this file does not have to cross-check.
static constexpr uint32_t kParamSrvTable  = 0;
static constexpr uint32_t kParamUavTable  = 1;
static constexpr uint32_t kParamConstants = 2;

// =============================================================================================
// Build. Reuses hdr_codec's compile/cache/override machinery verbatim, with this feature's own
// name in the messages. The shader is self-contained - there is no shared prelude - so the hash is
// taken over the source exactly as written above, and the on-disk cache is
// stray_dlssnr_depth.<hash>.dxbc.
// =============================================================================================
template <typename LogFn>
inline bool build(const std::wstring &dir, std::vector<uint8_t> &out, LogFn log)
{
	return hdr_codec::build_blob(dir, L"stray_dlssnr_depth", "shader", "depth conversion",
	                             std::string(kConvertSource), out, log);
}

// =============================================================================================
// Pipeline layout, PSO and the statistic's two buffers.
//
// THE STATISTIC'S BUFFERS ARE PART OF THE PASS, NOT AN OPTIONAL EXTRA, and deliberately so. The
// root signature declares a TWO-descriptor UAV table, so every push_descriptors for this pass must
// supply both; a pass that had to cope with a missing stats UAV would either push a null descriptor
// or need a second root signature. They are 32 bytes each - a device that cannot allocate them is
// not going to run a denoiser - so a failure here fails the whole pass, which lands on exactly
// today's behaviour and is the softest possible outcome anyway.
// =============================================================================================
struct pipelines
{
	pipeline_layout layout    = { 0 };
	pipeline        pso       = { 0 };
	resource        stats_buf = { 0 };   // r32_uint UAV, default_ heap
	resource_view   stats_uav = { 0 };
	resource        readback  = { 0 };   // readback heap, copy_dest
	bool            ok        = false;
};

inline void destroy(device *dev, pipelines &p)
{
	if (dev == nullptr)
	{
		p = pipelines();
		return;
	}
	// Views before resources, and every one guarded: destroy_resource_view on a zero handle is not
	// defined to be safe, and a leaked view leaks a slot out of ReShade's CPU descriptor pool for
	// the life of the process.
	if (p.stats_uav.handle != 0) dev->destroy_resource_view(p.stats_uav);
	if (p.stats_buf.handle != 0) dev->destroy_resource(p.stats_buf);
	if (p.readback.handle  != 0) dev->destroy_resource(p.readback);
	if (p.pso.handle       != 0) dev->destroy_pipeline(p.pso);
	if (p.layout.handle    != 0) dev->destroy_pipeline_layout(p.layout);
	p = pipelines();
}

template <typename LogFn>
inline bool create(device *dev, const std::vector<uint8_t> &dxbc, pipelines &p, LogFn log)
{
	p = pipelines();
	if (dev == nullptr || dxbc.empty())
		return false;

	const char *stage = nullptr;

	// ONE SRV (t0), TWO UAVs (u0 target, u1 statistic), eight root constants.
	if      (!hdr_codec::make_layout(dev, 1, kDepthConstantCount, p.layout, 2)) stage = "create_pipeline_layout(depth)";
	else if (!hdr_codec::make_pipeline(dev, p.layout, dxbc, p.pso))             stage = "create_pipeline(depth)";

	if (stage == nullptr)
	{
		// unordered_access | copy_source: written by the shader, then copied to the readback.
		// memory_heap::default_ and ::readback, NOT the gpu_only / gpu_to_cpu spellings: those two
		// are [[deprecated]] aliases for exactly these values in reshade_api_resource.hpp:193-195,
		// and the committed code in this tree (nr_ensure_aux) already uses the current names.
		const resource_desc bd(kStatBytes, memory_heap::default_,
		                       resource_usage::unordered_access | resource_usage::copy_source);
		if (!dev->create_resource(bd, nullptr, resource_usage::unordered_access, &p.stats_buf) ||
		    p.stats_buf.handle == 0)
			stage = "create_resource(depth stats)";
	}

	if (stage == nullptr)
	{
		// offset and size are in BYTES for a buffer view, not elements: passing kStatSlots here
		// would build an 8-byte, two-element UAV and the atomics on Stats[2..6] would land out of
		// bounds. reshade_api_resource.hpp documents the unit explicitly, and this project has
		// already been bitten by the assumption once.
		const resource_view_desc vd(format::r32_uint, 0, kStatBytes);
		if (!dev->create_resource_view(p.stats_buf, resource_usage::unordered_access, vd, &p.stats_uav) ||
		    p.stats_uav.handle == 0)
			stage = "create_resource_view(depth stats UAV)";
	}

	if (stage == nullptr)
	{
		const resource_desc rd(kStatBytes, memory_heap::readback, resource_usage::copy_dest);
		if (!dev->create_resource(rd, nullptr, resource_usage::copy_dest, &p.readback) ||
		    p.readback.handle == 0)
			stage = "create_resource(depth readback)";
	}

	if (stage != nullptr)
	{
		char buf[640];
		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: the depth conversion pass could not be built - %s failed. The pass stays OFF "
			"for this run and DLSSNR.Depth falls back to the GAME'S OWN r32_g8_typeless depth "
			"resource, i.e. EXACTLY the behaviour before this feature existed (README gap 3). "
			"depth_detect cannot run either, so DLSSNR.DepthInverted stays at its configured value "
			"(README gap 4). Nothing else changes.", stage);
		log(log_error, buf);
		destroy(dev, p);
		return false;
	}

	p.ok = true;
	return true;
}

// =============================================================================================
// GAP 4: the verdict, and the evidence it was reached on.
// =============================================================================================

enum class verdict { undecided = 0, reversed = 1, standard = 2 };

// One window's counters, already turned into the units a human reads.
struct evidence
{
	uint64_t samples = 0;    // sampled texels in the window, valid or not
	uint64_t lo      = 0;    // ... of which below kLoThreshold
	uint64_t hi      = 0;    // ... of which above kHiThreshold
	uint64_t bad     = 0;    // ... of which not a normalised depth value at all
	double   mean    = 0.0;  // over the VALID samples
	double   min_z   = 0.0;
	double   max_z   = 0.0;
	bool     valid   = false;
};

// The thresholds the verdict is taken against. Every one of them is a REFUSAL threshold: they are
// what makes "declines to decide" the default answer rather than the exception.
static constexpr uint64_t kMinSamples      = 1024;   // a window smaller than this proves nothing
static constexpr double   kMaxBadFraction  = 0.01;   // >1% not-a-depth means we read the wrong thing
static constexpr double   kMinSpread       = 0.05;   // max - min; a constant frame decides nothing
static constexpr double   kMinSideFraction = 0.25;   // the winning side must be a quarter of the frame
static constexpr double   kSideRatio       = 16.0;   // ... and beat the other side by this much

// Turn one window's counters into a verdict. PURE - no device, no state - so the rule can be read,
// argued with and replayed on a host without a GPU in the room. 'why' always comes back naming the
// reason, including on success.
inline verdict decide(const evidence &e, const char **why)
{
	static const char *s_unused = "";
	if (why == nullptr)
		why = &s_unused;

	if (!e.valid || e.samples < kMinSamples)
	{
		*why = "the window produced too few samples to mean anything";
		return verdict::undecided;
	}

	const double n = static_cast<double>(e.samples);
	if (static_cast<double>(e.bad) > kMaxBadFraction * n)
	{
		*why = "more than 1% of the sampled texels are NOT a normalised depth value (outside [0,1] "
		       "or non-finite), so the SRV being read is not scene depth and the whole premise of "
		       "this pass is wrong";
		return verdict::undecided;
	}

	// Named n_valid, not `valid`, so it cannot be mistaken for evidence::valid two lines up.
	const double n_valid = n - static_cast<double>(e.bad);
	if (n_valid < static_cast<double>(kMinSamples))
	{
		*why = "almost every sampled texel was rejected as not-a-depth";
		return verdict::undecided;
	}

	// The degenerate-frame guard. A cleared, sky-only or loading frame is a CONSTANT, and a
	// constant is consistent with BOTH conventions - see the header comment.
	if ((e.max_z - e.min_z) < kMinSpread)
	{
		*why = "the frame carries no depth range at all (a cleared, sky-only or loading frame), and "
		       "a constant depth buffer is consistent with either convention";
		return verdict::undecided;
	}

	const double lo = static_cast<double>(e.lo);
	const double hi = static_cast<double>(e.hi);

	if (lo >= kMinSideFraction * n_valid && lo > kSideRatio * (hi + 1.0))
	{
		*why = "the depth mass sits at the LOW end, which under a hyperbolic projection is the FAR "
		       "field - near maps to 1, so this is reversed-Z";
		return verdict::reversed;
	}
	if (hi >= kMinSideFraction * n_valid && hi > kSideRatio * (lo + 1.0))
	{
		*why = "the depth mass sits at the HIGH end, which under a hyperbolic projection is the FAR "
		       "field - near maps to 0, so this is standard-Z";
		return verdict::standard;
	}

	*why = "the distribution is not one-sided enough to separate the two conventions";
	return verdict::undecided;
}

// =============================================================================================
// The detector's sequencing. All of it lives on the render thread that runs the dispatch.
// =============================================================================================
static constexpr uint32_t kWindowFrames  = 60;   // one window ~= one second at 60 fps
static constexpr uint32_t kReadbackDelay = 4;    // dispatches, NOT a fence - see read() below
static constexpr uint32_t kMaxWindows    = 8;

struct detector
{
	bool     armed            = false;   // depth_detect=1 and the pass exists
	uint32_t frames_in_window = 0;
	bool     awaiting_copy    = false;
	uint32_t copy_age         = 0;
	uint32_t windows_tried    = 0;
	// LATCHED FOR THE RUN once it leaves undecided. See the header comment on why a mid-run flip
	// is worse than a wrong constant.
	verdict  latched          = verdict::undecided;
	// True once there is nothing left to measure - either latched, or kMaxWindows gave up.
	bool     done             = false;
	evidence proof;                      // the window the verdict was taken on
};

// Whether THIS dispatch should accumulate. Kept as its own predicate so the caller can also use it
// to decide whether the pass needs to run at all when depth_convert=0.
inline bool measuring(const pipelines &p, const detector &d)
{
	return p.ok && d.armed && !d.done && !d.awaiting_copy;
}

// Read the counters back. Only safe once the copy has actually retired - the caller gates this on
// a frame delay rather than a fence, because a fence wait on the render thread for a diagnostic
// would be a stall the shipping path must never pay. Exactly nr_probe.hpp's discipline.
inline bool read(device *dev, const pipelines &p, evidence &out)
{
	void *data = nullptr;
	if (dev == nullptr || p.readback.handle == 0)
		return false;
	if (!dev->map_buffer_region(p.readback, 0, kStatBytes, map_access::read_only, &data) || data == nullptr)
		return false;

	uint32_t v[kStatSlots] = {};
	std::memcpy(v, data, sizeof(v));
	dev->unmap_buffer_region(p.readback);

	if (v[kStatSamples] == 0)
		return false;

	out = evidence();
	out.samples = v[kStatSamples];
	out.lo      = v[kStatLo];
	out.hi      = v[kStatHi];
	out.bad     = v[kStatBad];

	const uint64_t valid = (out.samples > out.bad) ? (out.samples - out.bad) : 0ull;
	out.mean  = (valid != 0ull)
		? static_cast<double>(v[kStatSum]) / static_cast<double>(valid) / static_cast<double>(kFixedScale)
		: 0.0;
	// kStatMaxComp holds max(kQuantise - q), so the minimum comes back as kQuantise minus it. Both
	// extremes are accumulated with InterlockedMax for the seeding reason stated at kQuantise.
	out.max_z = static_cast<double>(v[kStatMax]) / static_cast<double>(kQuantise);
	out.min_z = static_cast<double>(kQuantise - v[kStatMaxComp]) / static_cast<double>(kQuantise);
	out.valid = true;
	return true;
}

// Zero the accumulator at the START of a window.
//
// clear_unordered_access_view_uint requires the resource to be in unordered_access, which is the
// state stats_buf was created in and rests in. If ReShade's D3D12 backend cannot service it the
// call is a silent no-op, and that failure mode is BENIGN here: the counters then accumulate across
// windows, which can only reinforce whatever verdict the first window would have produced, and the
// spread and ratio tests are unaffected. Nothing is decided on the sum, which is the only counter
// that could eventually overflow.
inline void clear(command_list *cmd, const pipelines &p)
{
	const uint32_t zero[4] = { 0u, 0u, 0u, 0u };
	cmd->clear_unordered_access_view_uint(p.stats_uav, zero);
}

inline void copy_to_readback(command_list *cmd, const pipelines &p)
{
	cmd->barrier(p.stats_buf, resource_usage::unordered_access, resource_usage::copy_source);
	cmd->copy_buffer_region(p.stats_buf, 0, p.readback, 0, kStatBytes);
	cmd->barrier(p.stats_buf, resource_usage::copy_source, resource_usage::unordered_access);
}

// =============================================================================================
// THE DISPATCH, WITH THE WINDOW SEQUENCING INSIDE IT.
//
// Ordering inside a window matters and is the reason this is one function rather than three calls
// at the site - the same reasoning nr_probe.hpp gives: the accumulator must be cleared BEFORE the
// window's first dispatch and copied AFTER its last, and getting that wrong silently mixes two
// windows' texels into one statistic. Here that would not merely blur a number, it would let a
// half-window decide the depth convention for the run.
//
// THE CALLER OWNS THE BARRIER ON OutDepth afterwards, and only that. It is the caller that knows
// which resting state it wants the texture returned to, and it is the caller's restore block - the
// one that runs even on the exception path - that guarantees it gets there. Nothing else this
// function touches outlives the call.
//
// The caller also issues the descriptor-heap cache sync BEFORE calling in, because only the caller
// knows whether an earlier pass this frame already did. There is a SECOND one inside, immediately
// after the accumulator clear, which the caller's cannot stand in for; the comment there says why.
// =============================================================================================
inline void dispatch(command_list *cmd, const pipelines &p, detector &d,
                     resource_view src_srv, resource_view dst_uav,
                     uint32_t out_w, uint32_t out_h, uint32_t src_w, uint32_t src_h)
{
	const bool measure = measuring(p, d);

	if (measure && d.frames_in_window == 0)
	{
		clear(cmd, p);
		// AND RE-SYNC THE CACHE AFTER IT. clear_unordered_access_view_uint needs a SHADER-VISIBLE
		// descriptor for the UAV, so ReShade's D3D12 backend services it out of a heap of its own -
		// which means it can leave the raw command list, and ReShade's own _current_descriptor_heaps
		// cache, naming something other than what the push_descriptors below is about to assume.
		// count == 0 is ReShade's escape hatch and FORCES both SetDescriptorHeaps and
		// SetComputeRootSignature. The caller issues one of these too, BEFORE this function; that
		// one cannot cover a call made after it. This costs one redundant pair of calls once per
		// window of kWindowFrames frames and closes the hole for good.
		cmd->bind_descriptor_tables(shader_stage::all_compute, p.layout, 0, 0, nullptr);
	}

	cmd->bind_pipeline(pipeline_stage::all_compute, p.pso);

	descriptor_table_update srv_up = {};
	srv_up.binding = 0; srv_up.array_offset = 0; srv_up.count = 1;
	srv_up.type = descriptor_type::shader_resource_view;
	srv_up.descriptors = &src_srv;
	cmd->push_descriptors(shader_stage::compute, p.layout, kParamSrvTable, srv_up);

	// u0 target, u1 statistic - ONE contiguous table, in declaration order. Both are always
	// supplied even when the statistic is idle: the root signature declares two, and create()
	// guarantees the stats UAV exists whenever the pass does.
	const resource_view uavs[2] = { dst_uav, p.stats_uav };
	descriptor_table_update uav_up = {};
	uav_up.binding = 0; uav_up.array_offset = 0; uav_up.count = 2;
	uav_up.type = descriptor_type::unordered_access_view;
	uav_up.descriptors = uavs;
	cmd->push_descriptors(shader_stage::compute, p.layout, kParamUavTable, uav_up);

	depth_args da;
	da.out_w = out_w;  da.out_h = out_h;
	da.src_w = src_w;  da.src_h = src_h;
	da.measure     = measure ? 1u : 0u;
	da.stride      = kStride;
	da.fixed_scale = kFixedScale;
	da.quantise    = kQuantise;
	cmd->push_constants(shader_stage::compute, p.layout, kParamConstants, 0, kDepthConstantCount, &da);

	cmd->dispatch(hdr_codec::group_count(out_w), hdr_codec::group_count(out_h), 1);

	if (measure && ++d.frames_in_window >= kWindowFrames)
	{
		copy_to_readback(cmd, p);
		d.awaiting_copy    = true;
		d.copy_age         = 0;
		d.frames_in_window = 0;
	}
}

// =============================================================================================
// The readback poll. Call it BEFORE dispatch(), on the same thread, once per dispatch.
//
// Returns true exactly once - on the dispatch at which the run's verdict was settled, either to a
// convention or to "gave up" - so the caller can print the one line that says what happens next
// without needing a latch of its own.
// =============================================================================================
template <typename LogFn>
inline bool poll(device *dev, const pipelines &p, detector &d, LogFn log)
{
	if (!p.ok || !d.armed || d.done || !d.awaiting_copy)
		return false;
	if (++d.copy_age < kReadbackDelay)
		return false;

	d.awaiting_copy = false;
	d.copy_age      = 0;
	d.windows_tried++;

	char buf[1024];

	evidence e;
	if (!read(dev, p, e))
	{
		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: depth_detect window %u/%u - the readback produced nothing. No verdict from "
			"this window.", d.windows_tried, kMaxWindows);
		log(log_warn, buf);
	}
	else
	{
		const char *why = "";
		const verdict v = decide(e, &why);

		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: depth_detect window %u/%u over %llu sampled texels - below %.2f: %llu, "
			"above %.2f: %llu, not-a-depth: %llu, mean %.5f, range [%.5f, %.5f].",
			d.windows_tried, kMaxWindows, (unsigned long long)e.samples,
			(double)kLoThreshold, (unsigned long long)e.lo,
			(double)kHiThreshold, (unsigned long long)e.hi,
			(unsigned long long)e.bad, e.mean, e.min_z, e.max_z);
		log(log_info, buf);

		if (v != verdict::undecided)
		{
			d.latched = v;
			d.done    = true;
			d.proof   = e;
			std::snprintf(buf, sizeof(buf),
				"DLSS-NR: depth_detect MEASURED %s - %s. This is LATCHED for the run: a verdict "
				"that flipped mid-run would change the meaning of every frame already in NGX's "
				"temporal history, which is strictly worse than a wrong constant.",
				(v == verdict::reversed) ? "REVERSED-Z (near = 1, far = 0)"
				                         : "STANDARD-Z (near = 0, far = 1)", why);
			log(log_info, buf);
			return true;
		}

		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: depth_detect window %u/%u DECLINES to decide - %s.",
			d.windows_tried, kMaxWindows, why);
		log(log_warn, buf);
	}

	if (d.windows_tried >= kMaxWindows)
	{
		d.done = true;
		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: depth_detect gave up after %u windows of %u frames without a confident "
			"reading. DLSSNR.DepthInverted keeps the value depth_inverted holds, which is the "
			"behaviour before this feature existed (README gap 4). This is not an error - it is the "
			"detector refusing to guess.", kMaxWindows, kWindowFrames);
		log(log_warn, buf);
		return true;
	}
	return false;
}

} // namespace depth_convert
