// nr_probe.hpp - an IN-RUN instrument that measures what the network actually did to the image,
// and sweeps a tuning parameter against that measurement inside a SINGLE launch.
//
// WHY THIS EXISTS
// ===============
// The open question is not whether a tuning value reaches NGX - the getter trace already proves
// that ("DLSSNR.Intensity -> HIT, returned 0.0200"). It is whether the network ACTS on it. That
// cannot be answered from the parameter block, and answering it from screenshots failed twice:
//
//   - the frame is dark enough in STRAY's slums that a real change is not visible by eye;
//   - a cold relaunch per value moves the cat, so scene difference and parameter effect land in
//     the same number. A same-config control run measured that confound at mean|diff| 0.874 over
//     a 160x90 luma grid, which is LARGER than the intensity effect it was supposed to resolve.
//
// So the measurement is moved onto the GPU, beside the evaluate, where both sides of the network
// exist in the SAME FRAME: orig_tex (the pre-denoise TAA output, i.e. what the network was given)
// and out_tex (what it returned). Comparing those two removes the scene, the camera, the cat and
// the HDR graft from the comparison in one step - none of them can differ between two textures
// sampled from one frame.
//
// AND THE SWEEP IS IN-RUN. Config is read once at device init (s_config_loaded), so an ini edit
// costs a relaunch, and a relaunch costs scene reproducibility - the exact confound above. The
// probe therefore drives the parameter itself, holding each value for a fixed number of frames
// with the camera parked. Every step sees the same content, so a difference between steps is the
// parameter and nothing else.
//
// WHAT IT CANNOT TELL YOU
// =======================
// A null result here is "the network's OUTPUT did not change", not "the value was ignored". Those
// coincide only if the content actually exercises the parameter. That distinction is why the
// summary line reports the per-step spread against the SAME-VALUE repeat baked into the sweep
// table: step 0 and the final step run identical settings, so their difference is this run's own
// noise floor, and no other step counts as a signal unless it beats it. This is the same control
// the screenshot sweeps used, moved inside the run where it is far tighter.

#pragma once

#include "reshade_compat.hpp"
#include "hdr_codec.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace reshade::api;

namespace nr_probe
{

// The accumulator is four uints in one buffer: sum(luma_in), sum(luma_out), sum(|luma_out -
// luma_in|), sample_count. Fixed point because a typed r32_uint UAV is the only thing that gives
// us InterlockedAdd without a structured-buffer view.
static constexpr uint32_t kSlots       = 4;
static constexpr uint32_t kSlotBytes   = kSlots * sizeof(uint32_t);
static constexpr uint32_t kThreads     = 8;

// Sample every 8th texel on both axes. At 1920x1080 that is 240x135 = 32400 samples, which is
// plenty for a mean and keeps the fixed-point sum inside uint32: 32400 * 1024 = 33.2M, and the
// three accumulators are each bounded by that. A full-res sum would overflow.
static constexpr uint32_t kStride      = 8;
static constexpr uint32_t kFixedScale  = 1024;

// Root constants, in 32-bit values.
static constexpr uint32_t kConstantCount = 4;
struct args
{
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t scale;
};

// Table indices must match hdr_codec::make_layout's parameter order.
static constexpr uint32_t kParamSrvTable  = 0;
static constexpr uint32_t kParamUavTable  = 1;
static constexpr uint32_t kParamConstants = 2;

inline uint32_t group_count(uint32_t extent)
{
	const uint32_t sampled = (extent + kStride - 1u) / kStride;
	return (sampled + kThreads - 1u) / kThreads;
}

// t0 is what the network was handed, t1 is what it returned. Both are sampled with Load, not a
// sampler: these are exact texel reads and no filtering may enter the statistic.
static const char *const kShaderSource = R"HLSL(
Texture2D<float4> t_in  : register(t0);
Texture2D<float4> t_out : register(t1);
RWBuffer<uint>    stats : register(u0);

cbuffer Args : register(b0)
{
    uint g_width;
    uint g_height;
    uint g_stride;
    uint g_scale;
};

float luma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 p = id.xy * g_stride;
    if (p.x >= g_width || p.y >= g_height)
        return;

    float3 a = t_in.Load(int3(p, 0)).rgb;
    float3 b = t_out.Load(int3(p, 0)).rgb;

    float la = luma(a);
    float lb = luma(b);

    // saturate before the fixed-point cast: scene-linear HDR carries values above 1 and, on the
    // residual path, below 0. An unsaturated cast of a negative float to uint wraps to ~4e9 and
    // would poison the sum with a single texel.
    uint ua = (uint)(saturate(la) * g_scale);
    uint ub = (uint)(saturate(lb) * g_scale);
    uint ud = (uint)(saturate(abs(lb - la)) * g_scale);

    uint prev;
    InterlockedAdd(stats[0], ua,  prev);
    InterlockedAdd(stats[1], ub,  prev);
    InterlockedAdd(stats[2], ud,  prev);
    InterlockedAdd(stats[3], 1u,  prev);
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// The sweep table.
//
// The TARGET is structure strength. use_auto_mask is swept alongside it because the binary note
// in addon_config.hpp records that use_auto_mask only SELECTS between the struct field at +0xe8
// and a -1.0f constant for those same two slots - so with the wrong auto_mask the strength we
// write is never the value consumed, and a sweep of the strength alone would read as inert for a
// reason that has nothing to do with the gates.
//
// STEP 0 AND THE LAST STEP ARE IDENTICAL ON PURPOSE. Their difference is this run's noise floor.
// ---------------------------------------------------------------------------------------------
struct sweep_point
{
	const char *label;
	uint32_t    use_auto_mask;
	float       local_structure;
	float       skin_structure;
};

static const sweep_point kSweep[] = {
	{ "baseline      mask=0 local=0.0", 0u, 0.0f, -1.0f },
	{ "local high    mask=0 local=1.0", 0u, 1.0f, -1.0f },
	{ "mask on       mask=1 local=0.0", 1u, 0.0f, -1.0f },
	{ "mask on high  mask=1 local=1.0", 1u, 1.0f, -1.0f },
	{ "REPEAT of 0   mask=0 local=0.0", 0u, 0.0f, -1.0f },
};
static constexpr uint32_t kSweepCount = sizeof(kSweep) / sizeof(kSweep[0]);

struct step_result
{
	double mean_in   = 0.0;
	double mean_out  = 0.0;
	double mean_diff = 0.0;
	uint64_t samples = 0;
	bool   valid     = false;
};

// GPU objects. Owned by the caller's per-device state; built once and destroyed on teardown.
struct pipeline_set
{
	pipeline_layout layout   = { 0 };
	pipeline        pso      = { 0 };
	resource        stats_buf = { 0 };   // r32_uint UAV, GPU only
	resource_view   stats_uav = { 0 };
	resource        readback  = { 0 };   // gpu_to_cpu, copy_dest
	bool            ready     = false;
	bool            failed    = false;
};

// Sweep sequencing. All of this lives on the render thread that runs the evaluate.
struct run_state
{
	bool     active          = false;
	uint32_t step            = 0;
	uint32_t frames_in_step  = 0;
	bool     awaiting_copy   = false;
	uint32_t copy_age        = 0;
	bool     complete        = false;
	step_result results[kSweepCount];
};

// Same compile/cache machinery as the other two passes: HLSL string -> D3DCompile(cs_5_0) at
// load, through a LoadLibraryW'd d3dcompiler that is never linked, with a source-hash cache.
template <typename LogFn>
inline bool build_shader(const std::wstring &dir, std::vector<uint8_t> &out, LogFn log)
{
	return hdr_codec::build_blob(dir, L"stray_dlssnr_probe", "shader", "NR probe statistics",
	                             std::string(kShaderSource), out, log);
}

inline bool build(device *dev, const std::vector<uint8_t> &dxbc, pipeline_set &p)
{
	const char *stage = nullptr;

	if      (!hdr_codec::make_layout(dev, 2, kConstantCount, p.layout)) stage = "create_pipeline_layout(probe)";
	else if (!hdr_codec::make_pipeline(dev, p.layout, dxbc, p.pso))     stage = "create_pipeline(probe)";

	if (stage == nullptr)
	{
		// unordered_access | copy_source: written by the shader, then copied to the readback.
		const resource_desc bd(kSlotBytes, memory_heap::gpu_only,
		                       resource_usage::unordered_access | resource_usage::copy_source);
		if (!dev->create_resource(bd, nullptr, resource_usage::unordered_access, &p.stats_buf) ||
		    p.stats_buf.handle == 0)
			stage = "create_resource(probe stats)";
	}

	if (stage == nullptr)
	{
		// offset and size are in BYTES for a typed buffer view, not elements: passing kSlots here
		// would build a 4-byte, one-element UAV and the atomics on stats[1..3] would land out of
		// bounds. reshade_api_resource.hpp documents the unit explicitly.
		const resource_view_desc vd(format::r32_uint, 0, kSlotBytes);
		if (!dev->create_resource_view(p.stats_buf, resource_usage::unordered_access, vd, &p.stats_uav) ||
		    p.stats_uav.handle == 0)
			stage = "create_resource_view(probe stats UAV)";
	}

	if (stage == nullptr)
	{
		const resource_desc rd(kSlotBytes, memory_heap::gpu_to_cpu, resource_usage::copy_dest);
		if (!dev->create_resource(rd, nullptr, resource_usage::copy_dest, &p.readback) ||
		    p.readback.handle == 0)
			stage = "create_resource(probe readback)";
	}

	if (stage != nullptr)
	{
		p.failed = true;
		return false;
	}

	p.ready = true;
	return true;
}

inline void destroy(device *dev, pipeline_set &p)
{
	if (p.stats_uav.handle != 0) { dev->destroy_resource_view(p.stats_uav); p.stats_uav = { 0 }; }
	if (p.stats_buf.handle != 0) { dev->destroy_resource(p.stats_buf);      p.stats_buf = { 0 }; }
	if (p.readback.handle  != 0) { dev->destroy_resource(p.readback);       p.readback  = { 0 }; }
	if (p.pso.handle       != 0) { dev->destroy_pipeline(p.pso);            p.pso       = { 0 }; }
	if (p.layout.handle    != 0) { dev->destroy_pipeline_layout(p.layout);  p.layout    = { 0 }; }
	p.ready = false;
}

// Zero the accumulator. Called once at the START of a step, not per frame: the step's statistic
// is the mean over its whole hold, which is what averages out the residual frame-to-frame motion
// (idle animation, a flickering screen) that a single frame would carry into the comparison.
inline void clear(command_list *cmd, pipeline_set &p)
{
	const uint32_t zero[4] = { 0u, 0u, 0u, 0u };
	cmd->clear_unordered_access_view_uint(p.stats_uav, zero);
}

// Both SRVs must already be in shader_resource_non_pixel. The caller runs this immediately after
// the codec's decode, which has already transitioned out_tex for exactly that reason.
inline void dispatch(command_list *cmd, pipeline_set &p, resource_view in_srv, resource_view out_srv,
                     uint32_t width, uint32_t height)
{
	cmd->bind_descriptor_tables(shader_stage::all_compute, p.layout, 0, 0, nullptr);
	cmd->bind_pipeline(pipeline_stage::all_compute, p.pso);

	const resource_view srvs[2] = { in_srv, out_srv };
	descriptor_table_update srv_up = {};
	srv_up.binding = 0; srv_up.array_offset = 0; srv_up.count = 2;
	srv_up.type = descriptor_type::shader_resource_view;
	srv_up.descriptors = srvs;
	cmd->push_descriptors(shader_stage::compute, p.layout, kParamSrvTable, srv_up);

	descriptor_table_update uav_up = {};
	uav_up.binding = 0; uav_up.array_offset = 0; uav_up.count = 1;
	uav_up.type = descriptor_type::unordered_access_view;
	uav_up.descriptors = &p.stats_uav;
	cmd->push_descriptors(shader_stage::compute, p.layout, kParamUavTable, uav_up);

	args a = {};
	a.width = width; a.height = height; a.stride = kStride; a.scale = kFixedScale;
	cmd->push_constants(shader_stage::compute, p.layout, kParamConstants, 0, kConstantCount, &a);

	cmd->dispatch(group_count(width), group_count(height), 1);
}

inline void copy_to_readback(command_list *cmd, pipeline_set &p)
{
	cmd->barrier(p.stats_buf, resource_usage::unordered_access, resource_usage::copy_source);
	cmd->copy_buffer_region(p.stats_buf, 0, p.readback, 0, kSlotBytes);
	cmd->barrier(p.stats_buf, resource_usage::copy_source, resource_usage::unordered_access);
}

// Read the four counters. Only safe once the copy has actually retired - the caller gates this on
// a frame delay rather than a fence, because a fence wait on the render thread for a diagnostic
// would be a stall the shipping path must never pay.
inline bool read(device *dev, pipeline_set &p, step_result &out)
{
	void *data = nullptr;
	if (!dev->map_buffer_region(p.readback, 0, kSlotBytes, map_access::read_only, &data) || data == nullptr)
		return false;

	uint32_t v[kSlots] = { 0, 0, 0, 0 };
	std::memcpy(v, data, sizeof(v));
	dev->unmap_buffer_region(p.readback);

	if (v[3] == 0)
		return false;

	const double n = static_cast<double>(v[3]);
	const double s = static_cast<double>(kFixedScale);
	out.samples   = v[3];
	out.mean_in   = static_cast<double>(v[0]) / n / s;
	out.mean_out  = static_cast<double>(v[1]) / n / s;
	out.mean_diff = static_cast<double>(v[2]) / n / s;
	out.valid     = true;
	return true;
}

// ---------------------------------------------------------------------------------------------
// The sweep, driven one evaluate at a time on the render thread.
//
// Ordering inside a step matters and is the reason this is one function rather than three calls
// at the site: the accumulator must be cleared BEFORE the step's first dispatch and copied AFTER
// its last, and getting that wrong silently mixes two steps' pixels into one statistic - which
// would read as "the parameter did nothing", the exact false negative this instrument exists to
// avoid.
//
// The readback is deliberately NOT fenced. It is read kReadbackDelay evaluates after the copy was
// recorded, by which point the copy has long retired; a real fence wait here would stall the
// render thread for a diagnostic, which the shipping path must never do.
// ---------------------------------------------------------------------------------------------
static constexpr uint32_t kReadbackDelay = 4;

template <typename LogFn>
inline void frame(device *dev, command_list *cmd, pipeline_set &p, run_state &r,
                  resource_view in_srv, resource_view out_srv,
                  uint32_t width, uint32_t height, uint32_t frames_per_step, LogFn log)
{
	if (!r.active || r.complete || !p.ready)
		return;

	// A pending readback from the step that just ended, now old enough to be safe to map.
	if (r.awaiting_copy)
	{
		if (++r.copy_age >= kReadbackDelay)
		{
			step_result res;
			if (read(dev, p, res))
			{
				r.results[r.step] = res;
				log("DLSS-NR probe: step %u/%u [%s] mean_in=%.5f mean_out=%.5f mean|out-in|=%.5f "
				    "over %llu samples.",
				    r.step + 1u, kSweepCount, kSweep[r.step].label,
				    res.mean_in, res.mean_out, res.mean_diff,
				    (unsigned long long)res.samples);
			}
			else
			{
				log("DLSS-NR probe: step %u/%u [%s] READBACK FAILED - this step has no result and "
				    "is excluded from the verdict.", r.step + 1u, kSweepCount, kSweep[r.step].label);
			}

			r.awaiting_copy = false;
			r.copy_age      = 0;
			r.step++;
			r.frames_in_step = 0;

			if (r.step >= kSweepCount)
			{
				r.complete = true;

				// The verdict. Step 0 and the last step are the SAME settings, so their difference
				// is this run's noise floor; nothing else counts unless it beats that.
				const step_result &a = r.results[0];
				const step_result &z = r.results[kSweepCount - 1u];
				if (a.valid && z.valid)
				{
					const double floor_diff = (a.mean_diff > z.mean_diff)
						? a.mean_diff - z.mean_diff : z.mean_diff - a.mean_diff;
					log("DLSS-NR probe: NOISE FLOOR (step 1 vs step %u, identical settings) = "
					    "%.6f in mean|out-in|.", kSweepCount, floor_diff);

					double best = 0.0;
					uint32_t best_i = 0;
					for (uint32_t i = 1; i + 1u < kSweepCount; ++i)
					{
						if (!r.results[i].valid) continue;
						const double d = (r.results[i].mean_diff > a.mean_diff)
							? r.results[i].mean_diff - a.mean_diff
							: a.mean_diff - r.results[i].mean_diff;
						if (d > best) { best = d; best_i = i; }
					}
					log("DLSS-NR probe: LARGEST effect vs baseline = %.6f at step %u [%s].",
					    best, best_i + 1u, kSweep[best_i].label);
					log("DLSS-NR probe: VERDICT - structure strength %s on this model. %s",
					    (best > floor_diff * 2.0) ? "IS LIVE" : "is INERT",
					    (best > floor_diff * 2.0)
						? "The network's output moved with the parameter by more than twice this "
						  "run's own noise, so the two dynamic_cast gates pass and the control works."
						: "The network's output did not move with the parameter by more than twice "
						  "this run's own noise. Combined with the getter trace showing the value "
						  "arrives, that places the block INSIDE the snippet - consistent with the "
						  "CCNetwork / CCTinlayoutFusedPreBlockSwin1HLayer gates in addon_config.hpp "
						  "not being satisfied by CC_SILVER_AARDWOLD.");
				}
				else
				{
					log("DLSS-NR probe: VERDICT UNAVAILABLE - the baseline or its repeat did not "
					    "produce a reading, so there is no noise floor to judge against.");
				}
				log("DLSS-NR probe: PROBE COMPLETE");
			}
		}
		return;   // no accumulation while a step's result is in flight
	}

	if (r.frames_in_step == 0)
		clear(cmd, p);

	dispatch(cmd, p, in_srv, out_srv, width, height);
	r.frames_in_step++;

	if (r.frames_in_step >= frames_per_step)
	{
		copy_to_readback(cmd, p);
		r.awaiting_copy = true;
		r.copy_age      = 0;
	}
}

} // namespace nr_probe
