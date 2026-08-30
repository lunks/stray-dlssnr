// rt_census.hpp - the DXR dispatch census.
//
// WHAT THIS ANSWERS
//   D1 argues, from UE 4.27 source and from Stray's cooked config, that ray tracing is live in
//   this title and that the engine defaults enable THREE effects, not one: Shadows + Reflections
//   + Ambient Occlusion. That is an argument. This header measures it.
//
//   One log read must answer: which RT effects were compiled, whether DispatchRays actually
//   happens, how often, at what resolution, against which shader binding table geometry.
//
// THE GATE - READ THIS FIRST
//   Everything here is behind the ini key `rt_census`, which DEFAULTS TO 0.
//
//   WHAT EXECUTES WHEN rt_census = 0, exhaustively:
//     * on_init_pipeline   -> note_pipeline() : one relaxed atomic load, then return. Not on any
//                             per-frame path; fires once per PSO/collection creation.
//     * on_bind_pipeline   -> note_state_object_bind() : one relaxed atomic load, then return.
//                             Only reached on SetPipelineState1.
//     * DispatchRays       -> on_dispatch_rays() : one relaxed atomic load, then `return false`.
//                             ReShade then issues the game's DispatchRays exactly as it would
//                             with no add-on present.
//     * on_present         -> maybe_report() : one relaxed atomic load, then return.
//     * on_destroy_device  -> report(final) : one relaxed atomic load, then return.
//   No allocation, no lock, no log line, no resource, no barrier, no state change. The census
//   allocates NOTHING at any time, on or off: every table below is a fixed-size array.
//
//   The one honest cost of `rt_census = 0` is that the `dispatch_rays` event stays REGISTERED.
//   That is deliberate. Registration cannot be deferred to config-load time without a data race:
//   ReShade's D3D12 DispatchRays hook calls invoke_addon_event<dispatch_rays> UNCONDITIONALLY
//   (reshade/source/d3d12/d3d12_command_list.cpp:1147, behind plain `#if RESHADE_ADDON`, with no
//   has_addon_event() guard), so it reads addon_event_list[90] on the recording thread while a
//   late ReShadeRegisterEvent would be push_back-ing into that same vector. Registering in
//   DllMain, as every other event here is, makes the list immutable for the process lifetime.
//   The measured delta with the census off is therefore ONE extra indirect call inside a loop
//   that already runs. Nothing in ReShade keys off dispatch_rays having a listener - verified by
//   grepping the whole vendored source for `dispatch_rays`.
//
//   Contrast build_acceleration_structure, which is NOT registered and must not be: ReShade
//   allocates a std::vector and converts every geometry desc on every AS build whenever that
//   event has any listener at all (d3d12_command_list.cpp:1053-1077).
//
// HOW EFFECTS ARE DISCRIMINATED - and the correction to D1's plan
//   D1 proposed: read the raygen entry-point name at init_pipeline, and map the FINAL ray tracing
//   PSO to those names through the `libraries` subobject. The first half is right. THE SECOND
//   HALF DOES NOT WORK ON THIS RESHADE, and the census is built around that.
//
//   [SRC] UE 4.27 compiles ONE D3D12 COLLECTION per RT shader
//     (D3D12RayTracing.cpp:617-631: CreateRayTracingStateObject(..., 1 library, ...,
//      D3D12_STATE_OBJECT_TYPE_COLLECTION)), then links the final pipeline from EXISTING_COLLECTION
//     subobjects ONLY - passing `{} // Libraries` and `{} // HitGroups`
//     (D3D12RayTracing.cpp:1976-1987), or AddToStateObject with the same shape (:1957-1971).
//   [SRC] ReShade bails out of the whole event for exactly that shape: in
//     invoke_create_and_init_pipeline_event, `if (internal_desc.Type ==
//     D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) { if (shader_groups.empty()) return false; }`
//     (d3d12_device.cpp:2733-2736). With no DXIL_LIBRARY and no HIT_GROUP subobject,
//     shader_groups IS empty, so init_pipeline NEVER FIRES for UE 4.27's linked RTPSO.
//
//   So a bound RTPSO handle can never be resolved to a name through this API. The census
//   therefore measures two sets and reports both, letting one log read join them:
//
//     A. THE COMPILED SET - every RT entry-point name seen at init_pipeline. These arrive on the
//        COLLECTIONS, which do carry a DXIL_LIBRARY with an RDAT part, and ReShade hands us the
//        UNMANGLED name verbatim (d3d12_device.cpp:2489: `shader_desc.entry_point = string_table
//        + function_info->unmangled_name`; UE sets ExportToRename to the ORIGINAL entry name at
//        D3D12RayTracing.cpp:177-179, which is the field ReShade matches on). This set is
//        EVIDENCE OF EFFECT ENABLEMENT, because every FDeferredShadingSceneRenderer::PrepareRayTracingXxx
//        self-gates on its ShouldRenderXxx before adding a raygen shader - verified for
//        reflections (RayTracingReflections.cpp:434), AO (RayTracingAmbientOcclusion.cpp:108) and
//        shadows (RayTracingShadows.cpp:183-188). A name only exists here if some pass asked for it.
//
//     B. THE DISPATCHED SET - one bucket per distinct DispatchRays signature, with counts,
//        extents and SBT geometry. This is what proves an effect actually RAN, not merely
//        compiled.
//
//   THE JOIN, done by eye from the summary block. UE gathers the raygen table in a FIXED ORDER
//   (DeferredShadingRenderer.cpp:1125-1136):
//        Reflections, SingleLayerWaterReflections, Shadows, AmbientOcclusion, SkyLight,
//        GlobalIllumination, GIPlugin, Translucency, Debug, PathTracing
//   and that array becomes the pipeline's raygen shader table in order, so the census's
//   `rg_slot` (below) is an index into exactly that sequence, restricted to the effects that
//   were enabled. Slot 0 belongs to the first enabled effect in that list.
//
// THE BUCKET KEY IS ADDRESS-FREE, ON PURPOSE
//   On D3D12 the dispatch_rays event passes all four resource handles as ZERO and the raw GPU
//   virtual addresses in the *_offset arguments (d3d12_command_list.cpp:1147-1165). A GVA is not
//   a stable identity: UE rebuilds the shader binding table buffer whenever its contents change,
//   and the new buffer lands wherever the allocator puts it, so keying buckets on raygen_offset
//   makes a fresh bucket every time the SBT moves.
//
//   [SRC] Every table in one SBT is `ShaderTableAddress + <constant layout offset>`
//   (D3D12RayTracing.cpp:1455-1481), and RayGenRecordStride is
//   D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT = 64 (:1508). So
//
//       rg_rel = (int64_t)raygen_offset - (int64_t)miss_offset
//              = RayGenShaderTableOffset - MissShaderTableOffset + 64 * RayGenShaderIndex
//
//   is INDEPENDENT OF THE BUFFER'S BASE ADDRESS and differs by exactly 64 per raygen slot. The
//   bucket key is that value plus the SBT geometry (sizes and strides), never an address.
//   `rg_slot` is then (rg_rel - min rg_rel over buckets sharing a layout) / 64.
//
// SBT GEOMETRY AS A SECOND, INDEPENDENT SIGNAL
//   [SRC] D3D12RayTracing.cpp:1470-1483. hit_group_stride == 0 with hit_group_size == 64 means
//   GetDispatchRaysDesc was called with bAllowHitGroupIndexing = false: a pipeline with a single
//   default hit record, i.e. the materials-off shadow path or a deferred-material gather. A
//   non-zero stride with size/stride in the hundreds or thousands is the full material SBT.
//   miss_size / miss_stride recovers NumMissRecords.
//
// EXTENT AS A THIRD SIGNAL
//   Shadows, AO and debug dispatch at View.ViewRect; reflections, GI and sky light at the
//   ScreenPercentage-scaled extent. `height == 1` with a large width is unmistakable: that is a
//   sorted deferred-material gather, dispatched as a 1-D list.
//
// CONVENTIONS
//   Follows hdr_codec.hpp: header-only, a LogFn functor rather than a hard dependency on the
//   add-on's logger, one-shot latches for anything that could repeat per frame, fixed-size
//   storage, and no path that can throw into a ReShade callback (no allocation, no std::string,
//   no iterator invalidation - only snprintf into stack buffers).

#pragma once

#include "reshade_compat.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace rt_census {

using namespace reshade::api;

// Log levels handed to the caller's log functor: 0 = info, 1 = warning, 2 = error.
enum { log_info = 0, log_warn = 1, log_error = 2 };

// =============================================================================================
// Bounds. Every table is fixed size; the census never allocates.
// =============================================================================================
static constexpr uint32_t kMaxNames         = 96;  // distinct RT entry-point names remembered
static constexpr uint32_t kNameChars        = 64;  // longest UE 4.27 RT export is 36 chars
static constexpr uint32_t kMaxBuckets       = 64;  // distinct DispatchRays signatures
static constexpr uint32_t kMaxStateObjects  = 16;  // distinct RTPSO handles remembered
static constexpr uint32_t kMaxPipelineLines = 24;  // one-shot init_pipeline detail lines
static constexpr uint32_t kMaxDispatchLines = 32;  // one-shot per-new-bucket detail lines

enum rt_kind : uint32_t
{
	rk_raygen = 0,
	rk_miss,
	rk_closest_hit,
	rk_any_hit,
	rk_intersection,
	rk_callable,
	rk_count
};

static const char *kind_name(uint32_t k)
{
	switch (k)
	{
	case rk_raygen:       return "raygen";
	case rk_miss:         return "miss";
	case rk_closest_hit:  return "closesthit";
	case rk_any_hit:      return "anyhit";
	case rk_intersection: return "intersect";
	case rk_callable:     return "callable";
	default:              return "?";
	}
}

// =============================================================================================
// Name -> effect. Every entry below was read out of the UE 4.27.2 tree, not guessed:
//   grep -rn SF_RayGen Engine/Source | grep -oE '"[A-Za-z0-9_]+", *SF_RayGen'
// yields exactly these thirteen raygen entry points and no others, engine-wide. [SRC]
// The miss and hit-group names come from the same sweep over SF_RayMiss / SF_RayHitGroup, plus
// RayTracingMaterialHitShaders.cpp:224-234 for the material hit shaders.
// =============================================================================================
struct effect_row { const char *name; const char *effect; };

static const effect_row kRaygenEffects[] = {
	{ "OcclusionRGS",                         "RT SHADOWS  (one dispatch per shadow-casting light)" },
	{ "AmbientOcclusionRGS",                  "RT AMBIENT OCCLUSION" },
	{ "RayTracingReflectionsRGS",             "RT REFLECTIONS (classic path)" },
	{ "RayTracingDeferredReflectionsRGS",     "RT REFLECTIONS (experimental deferred path)" },
	{ "GlobalIlluminationRGS",                "RT GLOBAL ILLUMINATION (brute force)" },
	{ "RayTracingCreateGatherPointsRGS",      "RT GI final gather - create gather points" },
	{ "RayTracingCreateGatherPointsTraceRGS", "RT GI final gather - gather point trace" },
	{ "RayTracingFinalGatherRGS",             "RT GI final gather" },
	{ "SkyLightRGS",                          "RT SKY LIGHT" },
	{ "RayTracingPrimaryRaysRGS",             "RT TRANSLUCENCY / primary rays" },
	{ "RayTracingDebugMainRGS",               "RT DEBUG view" },
	{ "RayTracingBarycentricsMainRGS",        "RT barycentrics debug" },
	{ "PathTracingMainRG",                    "PATH TRACER" },
};

static const effect_row kOtherEffects[] = {
	{ "RayTracingLightingMS",  "miss shader: RT reflections lighting" },
	{ "DeferredMaterialMS",    "miss shader: deferred material gather" },
	{ "DeferredMaterialCHS",   "hit group: deferred material gather" },
	{ "MaterialCHS",           "hit group: FULL material closest hit" },
	{ "MaterialAHS",           "hit group: FULL material any hit (masked/translucent)" },
	{ "OpaqueShadowCHS",       "hit group: TRIVIAL hit shader (FTrivialMaterialCHS)" },
};

// Returns the effect label for a name, or nullptr when the name is not a known UE 4.27 one.
// The census reports unknown names verbatim rather than silently dropping them - a name we do
// not recognise is information, not noise.
static const char *effect_for(const char *name)
{
	if (name == nullptr || name[0] == '\0')
		return nullptr;
	for (const effect_row &r : kRaygenEffects)
		if (std::strcmp(r.name, name) == 0)
			return r.effect;
	for (const effect_row &r : kOtherEffects)
		if (std::strcmp(r.name, name) == 0)
			return r.effect;
	return nullptr;
}

// =============================================================================================
// State
// =============================================================================================
struct name_rec
{
	char     name[kNameChars] = {};
	uint32_t kind             = rk_count;
	uint64_t seen             = 0;  // how many collections carried this name
};

struct bucket
{
	bool     used            = false;

	// --- the address-free key ---
	int64_t  rg_rel          = 0;   // raygen_offset - miss_offset; see the header comment
	uint64_t raygen_size     = 0;
	uint64_t miss_size       = 0;
	uint64_t miss_stride     = 0;
	uint64_t hit_size        = 0;
	uint64_t hit_stride      = 0;
	uint64_t callable_size   = 0;
	uint64_t callable_stride = 0;

	// --- measurements ---
	uint64_t count           = 0;
	uint64_t frames_seen     = 0;
	uint64_t first_frame     = 0;
	uint64_t last_frame      = 0;
	uint32_t w_min = 0xFFFFFFFFu, w_max = 0, h_min = 0xFFFFFFFFu, h_max = 0, d_min = 0xFFFFFFFFu, d_max = 0;
	uint64_t max_per_frame   = 0;
	uint64_t count_this_frame = 0;

	// PSO identity: the ID3D12StateObject bound by the SetPipelineState1 immediately preceding
	// this dispatch (UE emits the two back to back - D3D12RayTracing.cpp:3936-3937).
	uint64_t state_object    = 0;
	bool     state_object_varied = false;
	// The raw address of the last raygen record, kept for the log only. NEVER part of the key.
	uint64_t last_raygen_gva = 0;
};

struct state
{
	// THE GATE. Every entry point loads this first and returns when it is false.
	std::atomic<bool>     enabled{ false };
	std::atomic<uint32_t> report_every{ 600 };

	// FRAME NUMBERING IS 1-BASED, AND THAT IS LOAD-BEARING. Ray tracing is recorded BEFORE the
	// present that ends the frame, so the first frame's dispatches arrive while this counter is
	// still at its initial value. With a 0-based counter those dispatches would land on the same
	// value that bucket::last_frame and dispatch_frame_mark are default-initialised to, the
	// "is this a new frame" test would be false, and the whole first frame would be counted as
	// zero frames - which is exactly what the self-test caught. Starting at 1 makes 0 mean
	// "never seen" and nothing else.
	std::atomic<uint64_t> frame{ 1 };
	std::atomic<uint64_t> last_report_frame{ 0 };

	// ---- init_pipeline side (UE compiles collections on task threads, so this is contended) ---
	std::mutex names_mutex;
	name_rec   names[kMaxNames];
	uint32_t   n_names            = 0;
	uint64_t   n_names_dropped    = 0;
	uint64_t   n_rt_pipelines     = 0;  // init_pipeline calls carrying >= 1 RT shader subobject
	uint64_t   n_rt_shader_descs  = 0;
	uint64_t   n_lib_pipelines    = 0;  // init_pipeline calls carrying a `libraries` subobject
	uint64_t   n_group_pipelines  = 0;  // ... carrying a `shader_groups` subobject
	uint32_t   pipeline_lines     = 0;
	uint32_t   max_payload_size   = 0;
	uint32_t   max_attribute_size = 0;
	uint32_t   max_recursion      = 0;
	bool       logged_first_rt_pipeline = false;

	// ---- bind_pipeline side ----
	std::atomic<uint64_t> n_state_object_binds{ 0 };

	// ---- dispatch side ----
	std::mutex dispatch_mutex;
	bucket     buckets[kMaxBuckets];
	uint32_t   n_buckets          = 0;
	uint64_t   n_buckets_dropped  = 0;
	uint64_t   n_dispatches       = 0;
	uint64_t   n_dispatch_frames  = 0;   // frames in which at least one DispatchRays happened
	uint64_t   dispatches_this_frame = 0;
	uint64_t   max_dispatches_frame = 0;
	uint64_t   dispatch_frame_mark = 0;
	uint32_t   dispatch_lines     = 0;
	uint64_t   state_objects[kMaxStateObjects] = {};
	uint32_t   n_state_objects    = 0;
	bool       logged_first_dispatch = false;
};

inline state &get()
{
	static state s;
	return s;
}

inline bool on()
{
	return get().enabled.load(std::memory_order_relaxed);
}

// =============================================================================================
// Arming. Called once, from init_device, AFTER the ini has been read.
// =============================================================================================
template <typename LogFn>
inline void arm(bool enable, uint32_t report_every_frames, LogFn log)
{
	state &s = get();
	s.report_every.store(report_every_frames != 0 ? report_every_frames : 600, std::memory_order_relaxed);
	s.enabled.store(enable, std::memory_order_relaxed);

	char buf[512];
	if (enable)
	{
		std::snprintf(buf, sizeof(buf),
			"RT CENSUS is ON (rt_census=1). Read-only: it registers dispatch_rays, reads DXR "
			"sub-objects at init_pipeline, and counts SetPipelineState1. It creates no resource, "
			"issues no command, and suppresses nothing - the dispatch_rays handler always returns "
			"false. Summary every %u frames and at destroy_device.",
			(unsigned)s.report_every.load(std::memory_order_relaxed));
		log(log_info, buf);
	}
	else
	{
		log(log_info,
			"RT CENSUS is OFF (rt_census=0, the default). Every entry point returns after one "
			"relaxed atomic load: nothing is counted, named, logged or allocated. NOTE that the "
			"probe census line's dxil= counter counts PIXEL AND COMPUTE shaders only and is NOT "
			"evidence about ray tracing either way - set rt_census=1 to measure DXR.");
	}
}

// =============================================================================================
// init_pipeline. Call this BEFORE the PS/CS filter, so DXR sub-objects stop being invisible.
// =============================================================================================
namespace detail {

// Copies at most kNameChars-1 bytes and always terminates. entry_point points into the caller's
// DXIL blob and is valid only for the duration of the callback, so it MUST be copied here.
inline void copy_name(char (&dst)[kNameChars], const char *src)
{
	if (src == nullptr)
	{
		std::snprintf(dst, kNameChars, "<null entry_point>");
		return;
	}
	uint32_t i = 0;
	for (; i + 1 < kNameChars && src[i] != '\0'; ++i)
		dst[i] = src[i];
	dst[i] = '\0';
}

// names_mutex must be held.
inline void record_name_locked(state &s, uint32_t kind, const char *entry_point)
{
	char name[kNameChars];
	copy_name(name, entry_point);

	for (uint32_t i = 0; i < s.n_names; ++i)
	{
		if (s.names[i].kind == kind && std::strcmp(s.names[i].name, name) == 0)
		{
			s.names[i].seen++;
			return;
		}
	}
	if (s.n_names >= kMaxNames)
	{
		s.n_names_dropped++;
		return;
	}
	name_rec &r = s.names[s.n_names++];
	std::memcpy(r.name, name, sizeof(name));
	r.kind = kind;
	r.seen = 1;
}

inline uint32_t kind_of(pipeline_subobject_type t)
{
	switch (t)
	{
	case pipeline_subobject_type::raygen_shader:       return rk_raygen;
	case pipeline_subobject_type::miss_shader:         return rk_miss;
	case pipeline_subobject_type::closest_hit_shader:  return rk_closest_hit;
	case pipeline_subobject_type::any_hit_shader:      return rk_any_hit;
	case pipeline_subobject_type::intersection_shader: return rk_intersection;
	case pipeline_subobject_type::callable_shader:     return rk_callable;
	default:                                           return rk_count;
	}
}

} // namespace detail

template <typename LogFn>
inline void note_pipeline(uint32_t subobject_count, const pipeline_subobject *subobjects,
                          pipeline pso, LogFn log)
{
	state &s = get();
	if (!s.enabled.load(std::memory_order_relaxed))
		return;                                   // <-- the strict no-op, one relaxed load
	if (subobjects == nullptr || subobject_count == 0)
		return;

	// Summarise on the stack first, so the lock is held for as little as possible and so a
	// pipeline with no RT content costs nothing but the scan.
	uint32_t per_kind[rk_count] = {};
	uint32_t n_libraries = 0, n_groups = 0;
	uint32_t payload = 0, attrib = 0, recursion = 0;
	bool has_rt = false;

	for (uint32_t i = 0; i < subobject_count; ++i)
	{
		const pipeline_subobject &so = subobjects[i];

		if (so.type == pipeline_subobject_type::libraries) { n_libraries += so.count; continue; }
		if (so.type == pipeline_subobject_type::shader_groups) { n_groups += so.count; continue; }
		if (so.type == pipeline_subobject_type::max_payload_size && so.data != nullptr && so.count >= 1)
			{ payload = *static_cast<const uint32_t *>(so.data); continue; }
		if (so.type == pipeline_subobject_type::max_attribute_size && so.data != nullptr && so.count >= 1)
			{ attrib = *static_cast<const uint32_t *>(so.data); continue; }
		if (so.type == pipeline_subobject_type::max_recursion_depth && so.data != nullptr && so.count >= 1)
			{ recursion = *static_cast<const uint32_t *>(so.data); continue; }

		const uint32_t k = detail::kind_of(so.type);
		if (k == rk_count)
			continue;
		has_rt = true;
		if (so.data != nullptr)
			per_kind[k] += so.count;
	}

	if (!has_rt && n_libraries == 0 && n_groups == 0)
		return;                                   // an ordinary graphics/compute PSO

	// Second pass under the lock, recording the names themselves.
	bool  first_line = false;
	char  line[768];
	line[0] = '\0';
	{
		std::lock_guard<std::mutex> lock(s.names_mutex);

		if (has_rt) s.n_rt_pipelines++;
		if (n_libraries) s.n_lib_pipelines++;
		if (n_groups)    s.n_group_pipelines++;
		if (payload)   s.max_payload_size   = payload   > s.max_payload_size   ? payload   : s.max_payload_size;
		if (attrib)    s.max_attribute_size = attrib    > s.max_attribute_size ? attrib    : s.max_attribute_size;
		if (recursion) s.max_recursion      = recursion > s.max_recursion      ? recursion : s.max_recursion;

		for (uint32_t i = 0; i < subobject_count; ++i)
		{
			const pipeline_subobject &so = subobjects[i];
			const uint32_t k = detail::kind_of(so.type);
			if (k == rk_count || so.data == nullptr)
				continue;

			const auto *descs = static_cast<const shader_desc *>(so.data);
			for (uint32_t j = 0; j < so.count; ++j)
			{
				s.n_rt_shader_descs++;
				detail::record_name_locked(s, k, descs[j].entry_point);
			}
		}

		if (!s.logged_first_rt_pipeline)
		{
			s.logged_first_rt_pipeline = true;
			first_line = true;
		}

		if (s.pipeline_lines < kMaxPipelineLines)
		{
			s.pipeline_lines++;
			// Name the FIRST raygen/miss/hit name on this state object; a UE collection carries
			// at most three exports (CHS + AHS + IS), so this is the whole story for raygen and
			// miss collections and the headline for hit groups.
			const char *headline = "-";
			for (uint32_t i = 0; i < subobject_count && std::strcmp(headline, "-") == 0; ++i)
			{
				const uint32_t k = detail::kind_of(subobjects[i].type);
				if (k == rk_count || subobjects[i].data == nullptr || subobjects[i].count == 0)
					continue;
				const auto *descs = static_cast<const shader_desc *>(subobjects[i].data);
				if (descs[0].entry_point != nullptr)
					headline = descs[0].entry_point;
			}
			const char *eff = effect_for(headline);
			std::snprintf(line, sizeof(line),
				"  RT pipeline #%llu pso=0x%llx  rgs=%u miss=%u chs=%u ahs=%u is=%u call=%u "
				"libraries=%u groups=%u payload=%u attrib=%u recursion=%u  first=\"%s\"%s%s",
				(unsigned long long)s.n_rt_pipelines, (unsigned long long)pso.handle,
				per_kind[rk_raygen], per_kind[rk_miss], per_kind[rk_closest_hit],
				per_kind[rk_any_hit], per_kind[rk_intersection], per_kind[rk_callable],
				n_libraries, n_groups, payload, attrib, recursion,
				headline,
				eff != nullptr ? "  -> " : "", eff != nullptr ? eff : "");
		}
	}

	if (first_line)
		log(log_info,
			"RT CENSUS: the FIRST ray tracing state object reached init_pipeline. DXR is live in "
			"this process and ReShade's DXIL RDAT reflection is working, so the entry-point names "
			"below are the engine's own.");
	if (line[0] != '\0')
		log(log_info, line);
}

// =============================================================================================
// bind_pipeline. Call from the SetPipelineState1 branch of on_bind_pipeline.
// =============================================================================================
inline void note_state_object_bind(pipeline pso)
{
	state &s = get();
	if (!s.enabled.load(std::memory_order_relaxed))
		return;                                   // <-- the strict no-op, one relaxed load
	s.n_state_object_binds.fetch_add(1, std::memory_order_relaxed);
	(void)pso;   // the handle is recorded at the dispatch, where it is attributable to an effect
}

// =============================================================================================
// dispatch_rays. The handler MUST return false: returning true SUPPRESSES the game's DispatchRays
// (d3d12_command_list.cpp:1147, `if (invoke_addon_event<dispatch_rays>(...)) return;`).
// =============================================================================================
template <typename LogFn>
inline void note_dispatch(uint64_t state_object,
                          uint64_t raygen_offset, uint64_t raygen_size,
                          uint64_t miss_offset, uint64_t miss_size, uint64_t miss_stride,
                          uint64_t hit_offset, uint64_t hit_size, uint64_t hit_stride,
                          uint64_t callable_offset, uint64_t callable_size, uint64_t callable_stride,
                          uint32_t width, uint32_t height, uint32_t depth,
                          LogFn log)
{
	state &s = get();
	if (!s.enabled.load(std::memory_order_relaxed))
		return;                                   // <-- the strict no-op, one relaxed load

	(void)hit_offset;
	(void)callable_offset;

	const uint64_t frame = s.frame.load(std::memory_order_relaxed);
	const int64_t  rg_rel = static_cast<int64_t>(raygen_offset) - static_cast<int64_t>(miss_offset);

	char line[768];
	line[0] = '\0';
	bool first = false;

	{
		std::lock_guard<std::mutex> lock(s.dispatch_mutex);

		s.n_dispatches++;
		if (frame != s.dispatch_frame_mark)
		{
			s.dispatch_frame_mark = frame;
			s.dispatches_this_frame = 0;
			s.n_dispatch_frames++;
		}
		s.dispatches_this_frame++;
		if (s.dispatches_this_frame > s.max_dispatches_frame)
			s.max_dispatches_frame = s.dispatches_this_frame;

		if (!s.logged_first_dispatch)
		{
			s.logged_first_dispatch = true;
			first = true;
		}

		// Remember the distinct RTPSOs, bounded.
		if (state_object != 0)
		{
			bool known = false;
			for (uint32_t i = 0; i < s.n_state_objects; ++i)
				if (s.state_objects[i] == state_object) { known = true; break; }
			if (!known && s.n_state_objects < kMaxStateObjects)
				s.state_objects[s.n_state_objects++] = state_object;
		}

		bucket *b = nullptr;
		for (uint32_t i = 0; i < s.n_buckets; ++i)
		{
			bucket &c = s.buckets[i];
			if (c.rg_rel == rg_rel && c.raygen_size == raygen_size &&
				c.miss_size == miss_size && c.miss_stride == miss_stride &&
				c.hit_size == hit_size && c.hit_stride == hit_stride &&
				c.callable_size == callable_size && c.callable_stride == callable_stride)
			{
				b = &c;
				break;
			}
		}

		bool is_new = false;
		if (b == nullptr)
		{
			if (s.n_buckets >= kMaxBuckets)
			{
				s.n_buckets_dropped++;
			}
			else
			{
				b = &s.buckets[s.n_buckets++];
				b->used            = true;
				b->rg_rel          = rg_rel;
				b->raygen_size     = raygen_size;
				b->miss_size       = miss_size;
				b->miss_stride     = miss_stride;
				b->hit_size        = hit_size;
				b->hit_stride      = hit_stride;
				b->callable_size   = callable_size;
				b->callable_stride = callable_stride;
				b->first_frame     = frame;
				b->state_object    = state_object;
				is_new             = true;
			}
		}

		if (b != nullptr)
		{
			b->count++;
			if (b->last_frame != frame)
			{
				b->frames_seen++;
				b->last_frame = frame;
				if (b->count_this_frame > b->max_per_frame)
					b->max_per_frame = b->count_this_frame;
				b->count_this_frame = 0;
			}
			b->count_this_frame++;
			if (b->count_this_frame > b->max_per_frame)
				b->max_per_frame = b->count_this_frame;

			if (width  < b->w_min) b->w_min = width;
			if (width  > b->w_max) b->w_max = width;
			if (height < b->h_min) b->h_min = height;
			if (height > b->h_max) b->h_max = height;
			if (depth  < b->d_min) b->d_min = depth;
			if (depth  > b->d_max) b->d_max = depth;
			b->last_raygen_gva = raygen_offset;
			if (state_object != 0 && b->state_object != 0 && state_object != b->state_object)
				b->state_object_varied = true;
			if (state_object != 0)
				b->state_object = state_object;

			if (is_new && s.dispatch_lines < kMaxDispatchLines)
			{
				s.dispatch_lines++;
				std::snprintf(line, sizeof(line),
					"  RT dispatch: NEW signature @ frame %llu  rg_rel=%+lld extent=%ux%ux%u "
					"raygen(size=%llu gva=0x%llx) miss(size=%llu stride=%llu) "
					"hit(size=%llu stride=%llu) callable(size=%llu stride=%llu) rtpso=0x%llx  [%s]",
					(unsigned long long)frame, (long long)rg_rel, width, height, depth,
					(unsigned long long)raygen_size, (unsigned long long)raygen_offset,
					(unsigned long long)miss_size, (unsigned long long)miss_stride,
					(unsigned long long)hit_size, (unsigned long long)hit_stride,
					(unsigned long long)callable_size, (unsigned long long)callable_stride,
					(unsigned long long)state_object,
					(hit_stride == 0)
						? "no hit-group indexing: single default hit record"
						: ((height == 1) ? "1-D: sorted deferred-material gather"
						                 : "full material SBT"));
			}
		}
	}

	if (first)
		log(log_info,
			"RT CENSUS: the FIRST DispatchRays was observed. Ray tracing is not merely enabled in "
			"this build, it is EXECUTING. Effect attribution follows in the periodic summary.");
	if (line[0] != '\0')
		log(log_info, line);
}

// =============================================================================================
// The summary block. ONE read of this answers D1.
// =============================================================================================
template <typename LogFn>
inline void report(bool final_report, LogFn log)
{
	state &s = get();
	if (!s.enabled.load(std::memory_order_relaxed))
		return;                                   // <-- the strict no-op, one relaxed load

	char buf[900];
	// frame names the frame BEING RECORDED, so the number of completed presents is one less.
	const uint64_t presented = s.frame.load(std::memory_order_relaxed) - 1;

	std::snprintf(buf, sizeof(buf),
		"================ RT CENSUS %s after %llu presented frames ================",
		final_report ? "FINAL (destroy_device)" : "summary", (unsigned long long)presented);
	log(log_info, buf);

	// ---- A. the compiled set -----------------------------------------------------------------
	{
		std::lock_guard<std::mutex> lock(s.names_mutex);

		std::snprintf(buf, sizeof(buf),
			"  pipelines: rt_state_objects=%llu (with libraries=%llu, with shader_groups=%llu) "
			"rt_shader_descs=%llu distinct_names=%u dropped=%llu | "
			"max_payload=%u max_attrib=%u max_recursion=%u | SetPipelineState1 binds=%llu",
			(unsigned long long)s.n_rt_pipelines, (unsigned long long)s.n_lib_pipelines,
			(unsigned long long)s.n_group_pipelines, (unsigned long long)s.n_rt_shader_descs,
			s.n_names, (unsigned long long)s.n_names_dropped,
			s.max_payload_size, s.max_attribute_size, s.max_recursion,
			(unsigned long long)s.n_state_object_binds.load(std::memory_order_relaxed));
		log(log_info, buf);

		if (s.n_rt_pipelines == 0)
		{
			log(log_warn,
				"  COMPILED SET: EMPTY. No ray tracing sub-object has reached init_pipeline. "
				"Either the title built no RT state object at all, or this ReShade is a "
				"RESHADE_ADDON==1 build (CreateStateObject's event is behind #if RESHADE_ADDON >= 2 "
				"while DispatchRays is not), or every DXIL library lacked an RDAT part - ReShade "
				"skips the whole event in that case. Check the dispatch count below to tell the "
				"first case from the other two.");
		}
		else
		{
			log(log_info, "  COMPILED SET - RT entry points seen at init_pipeline (this is EFFECT ENABLEMENT:");
			log(log_info, "  every UE 4.27 PrepareRayTracingXxx self-gates before adding its raygen shader):");
			for (uint32_t pass = 0; pass < 2; ++pass)
			{
				for (uint32_t i = 0; i < s.n_names; ++i)
				{
					const name_rec &r = s.names[i];
					const bool is_rg = (r.kind == rk_raygen);
					if ((pass == 0) != is_rg)
						continue;
					const char *eff = effect_for(r.name);
					std::snprintf(buf, sizeof(buf), "    %-10s %-40s x%-6llu %s%s",
						kind_name(r.kind), r.name, (unsigned long long)r.seen,
						eff != nullptr ? "-> " : "-> (not a known UE 4.27 RT entry point)",
						eff != nullptr ? eff : "");
					log(log_info, buf);
				}
			}
		}
	}

	// ---- B. the dispatched set ---------------------------------------------------------------
	{
		std::lock_guard<std::mutex> lock(s.dispatch_mutex);

		std::snprintf(buf, sizeof(buf),
			"  dispatches: total=%llu over %llu frames (peak %llu in one frame) | "
			"signatures=%u dropped=%llu | distinct RTPSOs bound at dispatch=%u",
			(unsigned long long)s.n_dispatches, (unsigned long long)s.n_dispatch_frames,
			(unsigned long long)s.max_dispatches_frame, s.n_buckets,
			(unsigned long long)s.n_buckets_dropped, s.n_state_objects);
		log(log_info, buf);

		if (s.n_dispatches == 0)
		{
			log(log_warn,
				"  DISPATCHED SET: EMPTY. Zero DispatchRays so far. If the compiled set above is "
				"NON-empty this is the expensive failure mode D1 flagged: the TLAS is built every "
				"frame and nothing traces against it.");
		}
		else
		{
			// rg_slot is only meaningful within one SBT layout, so the minimum is taken over the
			// buckets that share the layout (everything but rg_rel).
			log(log_info,
				"  DISPATCHED SET - one row per DispatchRays signature. rg_slot indexes UE's raygen "
				"shader table, whose order is fixed (DeferredShadingRenderer.cpp:1125-1136):");
			log(log_info,
				"    Reflections, WaterReflections, Shadows, AmbientOcclusion, SkyLight, GI, "
				"GIPlugin, Translucency, Debug, PathTracing - restricted to the ENABLED ones.");

			for (uint32_t i = 0; i < s.n_buckets; ++i)
			{
				const bucket &b = s.buckets[i];

				int64_t min_rel = b.rg_rel;
				for (uint32_t j = 0; j < s.n_buckets; ++j)
				{
					const bucket &c = s.buckets[j];
					if (c.raygen_size == b.raygen_size && c.miss_size == b.miss_size &&
						c.miss_stride == b.miss_stride && c.hit_size == b.hit_size &&
						c.hit_stride == b.hit_stride && c.callable_size == b.callable_size &&
						c.callable_stride == b.callable_stride && c.rg_rel < min_rel)
						min_rel = c.rg_rel;
				}
				const long long slot = (b.raygen_size != 0)
					? static_cast<long long>((b.rg_rel - min_rel) / static_cast<int64_t>(b.raygen_size))
					: -1;

				const uint64_t miss_records = (b.miss_stride != 0) ? (b.miss_size / b.miss_stride) : 0;
				const uint64_t hit_records  = (b.hit_stride  != 0) ? (b.hit_size  / b.hit_stride)  : 0;

				std::snprintf(buf, sizeof(buf),
					"    #%-2u rg_slot=%-3lld count=%-9llu frames=%-7llu peak/frame=%-4llu "
					"extent=%ux%ux%u%s",
					i, slot, (unsigned long long)b.count, (unsigned long long)b.frames_seen,
					(unsigned long long)b.max_per_frame,
					b.w_max, b.h_max, b.d_max,
					(b.w_min != b.w_max || b.h_min != b.h_max) ? " (VARIES)" : "");
				log(log_info, buf);

				std::snprintf(buf, sizeof(buf),
					"        rg_rel=%+lld raygen_size=%llu miss(size=%llu stride=%llu -> %llu records) "
					"hit(size=%llu stride=%llu -> %llu records) callable(size=%llu stride=%llu) "
					"rtpso=0x%llx%s",
					(long long)b.rg_rel, (unsigned long long)b.raygen_size,
					(unsigned long long)b.miss_size, (unsigned long long)b.miss_stride,
					(unsigned long long)miss_records,
					(unsigned long long)b.hit_size, (unsigned long long)b.hit_stride,
					(unsigned long long)hit_records,
					(unsigned long long)b.callable_size, (unsigned long long)b.callable_stride,
					(unsigned long long)b.state_object,
					b.state_object_varied ? " (VARIES)" : "");
				log(log_info, buf);

				const char *shape =
					(b.hit_stride == 0)
						? "hit_stride=0 -> bAllowHitGroupIndexing=FALSE: one default hit record. "
						  "Materials-off shadows or a deferred-material gather."
						: (b.h_max == 1
							? "height=1 -> a 1-D sorted deferred-material gather list."
							: "full material SBT, 2-D screen dispatch.");
				std::snprintf(buf, sizeof(buf), "        shape: %s", shape);
				log(log_info, buf);

				if (b.w_min != 0xFFFFFFFFu && b.w_max != 0 && b.h_max > 1)
				{
					std::snprintf(buf, sizeof(buf),
						"        extent range: w=[%u..%u] h=[%u..%u] d=[%u..%u]  (ViewRect-sized "
						"means shadows/AO/debug; a smaller extent means ScreenPercentage-scaled: "
						"reflections/GI/skylight)",
						b.w_min, b.w_max, b.h_min, b.h_max, b.d_min, b.d_max);
					log(log_info, buf);
				}
			}
		}
	}

	log(log_info, "================ end RT CENSUS ================");
}

// Called from on_present. Bumps the frame counter and emits the summary every N frames.
template <typename LogFn>
inline void on_frame(LogFn log)
{
	state &s = get();
	if (!s.enabled.load(std::memory_order_relaxed))
		return;                                   // <-- the strict no-op, one relaxed load

	// fetch_add returns the frame that just ENDED, which is the one to report on; the counter
	// then names the frame now being recorded.
	const uint64_t f = s.frame.fetch_add(1, std::memory_order_relaxed);
	const uint32_t every = s.report_every.load(std::memory_order_relaxed);
	const uint64_t last = s.last_report_frame.load(std::memory_order_relaxed);
	if (f - last < every)
		return;
	s.last_report_frame.store(f, std::memory_order_relaxed);
	report(false, log);
}

} // namespace rt_census
