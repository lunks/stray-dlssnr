// stray_dlssnr.cpp - DLSS Neural Rendering (NGX feature 18) for STRAY (Unreal Engine 4.27.2,
// D3D12), as a ReShade 6.8 add-on.
//
// WHAT THIS IS
//   The read-only STRAY probe, with an NGX evaluate bolted on to the ONE dispatch it identified.
//   Everything the probe measured on the real game is kept verbatim - the DXBC token census, the
//   D3D12 descriptor shadow, the root-signature 1.1 / descriptor_table_with_flags deep copy at
//   the 40-byte stride, the SRV join, the C++ ABI thunks. None of that is rewritten; it works.
//
//   Added on top:
//     * a UAV join, so "which UAV is the TAA output?" is answered by resolution rather than left
//       open (the probe logged "outputs are UAVs, not tracked by this probe");
//     * loading nvngx_dlssnr.dll by hand through remix_nvngx.dll  (ngx_interop.hpp);
//     * a D3D12 command-list state save/restore around the evaluate (d3d12_state.hpp);
//     * an ini beside the add-on                                   (addon_config.hpp).
//
// WHERE THE PASS RUNS, AND WHY IT CANNOT RUN ANYWHERE ELSE
//   DLSS-NR keeps its own temporal history and exposes NO jitter parameter, so it must be fed
//   RESOLVED, de-jittered colour. That means AFTER the game's TAA, not instead of it. The game's
//   Dispatch is therefore always issued - unchanged, once, at exactly the point it would have
//   been - and the denoise runs after it, in the same command list.
//
//   ReShade's `dispatch` event is a PRE-hook and there is no post-dispatch event anywhere in the
//   API, so the add-on takes the dispatch over: it returns true (which stops ReShade issuing the
//   Dispatch a second time) and issues it itself through command_list::dispatch, which goes
//   straight to ID3D12GraphicsCommandList::Dispatch without re-entering the event. That is
//   RE-ISSUE, not suppression. The one visible consequence is that other add-ons registered after
//   this one do not see this particular dispatch event; see README "Interaction with other
//   add-ons".
//
// FAIL-OPEN CONTRACT
//   Every path that is not certain returns false, which leaves ReShade to issue the game's
//   Dispatch exactly as if this add-on were not loaded. Disabled in the ini, no nvngx_dlssnr.dll,
//   no remix_nvngx.dll, Init_Ext failed, the shader hash does not match, the SRV classes do not
//   form a quorum, the output UAV is unidentifiable, the state-restore plan is incomplete - all
//   of them are a strict no-op, and all of them say so in ReShade.log exactly once.
//
//   Once the handler has issued the dispatch itself it returns TRUE on every remaining path,
//   including the exception path, because the game's work has already been recorded and letting
//   ReShade issue it again would double it.
//
// Every handler body is wrapped so a C++ exception cannot escape into ReShade and terminate the
// process. If this add-on destabilises STRAY we lose the ability to test anything at all.

// ---- BEGIN overlay_ui hook ----
// MUST STAY ABOVE reshade_compat.hpp. include/reshade_overlay.hpp has NO include guard and its
// whole body is `#if defined(IMGUI_VERSION_NUM)`, so anything that pulls in reshade.hpp before
// <imgui.h> deletes namespace ImGui from this translation unit permanently - and reshade.hpp's
// own #pragma once means it never gets a second chance. src/overlay_imgui.hpp #errors if this
// moves, and a gating CI step fails the build if the wrong order ever compiles.
#include "overlay_ui.hpp"
// ---- END overlay_ui hook ----
#include "reshade_compat.hpp"

#include "shader_detect.hpp"
#include "descriptor_shadow.hpp"
#include "format_names.hpp"
#include "msvc_abi.hpp"

#include "addon_config.hpp"
#include "ngx_interop.hpp"
#include "d3d12_state.hpp"
#include "hdr_codec.hpp"

#include <d3d12.h>

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>

extern "C" __declspec(dllexport) const char *NAME        = "STRAY DLSS-NR";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
	"Runs NVIDIA DLSS Neural Rendering (NGX feature 18) on STRAY's resolved TAA output. Identifies "
	"the TAA pass by DXBC token analysis plus a D3D12 descriptor shadow, evaluates the "
	"nvngx_dlssnr.dll snippet directly through remix_nvngx.dll, and restores the command-list "
	"state NGX destroys. A strict no-op when disabled or when the snippet is absent.";
extern "C" __declspec(dllexport) const char *AUTHOR      = "stray-dlssnr";

using namespace reshade::api;

// =============================================================================================
// Tunables. All log volume is bounded by these; see README "Rate limiting".
// =============================================================================================
static constexpr uint32_t kMaxGateALogLines = 400; // detail lines for shaders passing the census
static constexpr uint32_t kMaxGateBLogLines = 200; // detail lines for 4.00801611f carriers
// Total SRV-table log blocks. Higher than the number of distinct shaders we expect to dump
// because a shader that resolves nothing is RETRIED - see kMaxSrvAttemptsPerShader.
static constexpr uint32_t kMaxSrvDumps      = 128;
// A first dispatch can legitimately resolve nothing: UE4 warms PSOs and dispatches during load
// before the descriptor shadow has seen a full frame of CopyDescriptors, the add-on may have
// attached after the views were created, and a root-signature change resets the binding shadow.
// Retiring a shader on that one sample would permanently destroy the probe's only chance to
// characterise it, so each shader gets a bounded number of tries and is written off only after.
static constexpr uint32_t kMaxSrvAttemptsPerShader = 8;
static constexpr uint32_t kStatsEveryFrames = 1800;

// =============================================================================================
// Logging
// =============================================================================================
static void logf(reshade::log::level level, const char *fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (n < 0)
		return;
	buf[sizeof(buf) - 1] = '\0';
	// ReShade passes our string through %s host-side, so '%' characters in it are safe.
	reshade::log::message(level, buf);
}

#define LOGI(...) logf(reshade::log::level::info,    __VA_ARGS__)
#define LOGW(...) logf(reshade::log::level::warning, __VA_ARGS__)
#define LOGE(...) logf(reshade::log::level::error,   __VA_ARGS__)

// Nothing may escape a callback into ReShade.
// Variadic on purpose: a body containing a top-level comma (e.g. "bool a = false, b = false;")
// would be split into several macro arguments by a single-parameter macro. __VA_ARGS__ rejoins
// them with the commas intact.
#define PROBE_GUARD_VOID(...)                                                                        \
	try { __VA_ARGS__ }                                                                              \
	catch (const std::exception &e) { LOGE("exception in %s: %s", __func__, e.what()); }              \
	catch (...) { LOGE("unknown exception in %s", __func__); }

// For the dispatch handler, which must report whether it already issued the game's Dispatch.
// The exception path returns 'retval' as it stood, NOT false: if the dispatch was issued before
// the throw, returning false would make ReShade issue it a second time.
#define PROBE_GUARD_RETURN(retval, ...)                                                              \
	try { __VA_ARGS__ }                                                                                \
	catch (const std::exception &e) { LOGE("exception in %s: %s", __func__, e.what()); }                \
	catch (...) { LOGE("unknown exception in %s", __func__); }                                          \
	return (retval);

#define PROBE_GUARD_FALSE(...)                                                                       \
	try { __VA_ARGS__ }                                                                              \
	catch (const std::exception &e) { LOGE("exception in %s: %s", __func__, e.what()); }              \
	catch (...) { LOGE("unknown exception in %s", __func__); }                                        \
	return false;

// =============================================================================================
// Shader analysis results, keyed by pipeline handle
// =============================================================================================
struct shader_record
{
	uint64_t             hash = 0;
	bool                 is_compute = false;
	bool                 passed_all_gates = false;   // full IsUE4TAACandidate
	bool                 interesting = false;        // passed_all_gates || found_velocity_constant
	// Container facts, cached alongside the analysis so a repeat sighting of the same bytecode
	// needs no re-parse at all.
	bool                 dxbc_valid = false;
	bool                 dxbc_is_dxil = false;
	uint8_t              sm_major = 0;
	uint8_t              sm_minor = 0;
	uint16_t             program_type = 0xFFFF;
	probe::TAAShaderInfo info;
};

struct pipeline_record
{
	pipeline_layout layout = { 0 };
	bool            has_shader = false;
	shader_record   shader;
};

// =============================================================================================
// Global (process-wide) probe state
// =============================================================================================
struct probe_globals
{
	std::mutex mutex;

	std::unordered_map<uint64_t, pipeline_record>  pipelines;      // pipeline.handle -> record
	// Analysis is cached by bytecode hash, so the O(size) token sweeps run ONCE per distinct
	// shader no matter how many PSOs UE4 builds from it.
	std::unordered_map<uint64_t, shader_record>    shader_cache;
	// SRV-dump bookkeeping. A hash is retired only once an attempt actually resolved something;
	// until then it is retried up to kMaxSrvAttemptsPerShader times.
	std::unordered_map<uint64_t, uint32_t>         srv_attempts;
	std::unordered_set<uint64_t>                   srv_resolved_hashes;

	// Gate histogram over every PS/CS seen.
	uint64_t n_shaders_seen        = 0;
	uint64_t n_not_dxbc            = 0;
	uint64_t n_dxil                = 0;
	uint64_t n_fail_census         = 0;
	uint64_t n_pass_census         = 0;
	uint64_t n_fail_velocity_const = 0;
	uint64_t n_pass_velocity_const = 0;
	uint64_t n_fail_loops          = 0;
	uint64_t n_fail_confidence     = 0;
	uint64_t n_pass_all            = 0;

	uint32_t gate_a_lines = 0;
	uint32_t gate_b_lines = 0;
	uint32_t srv_dumps    = 0;

	bool     logged_shader_model = false;
	bool     warned_index_dim    = false;
	bool     logged_non_d3d12    = false;

	uint64_t frame = 0;
	uint64_t last_stats_frame = 0;
};

static probe_globals g;

// Lock-free fast-path flags for the per-DRAW handler. dump_bindings is called on every draw,
// draw_indexed and dispatch from every UE4 parallel recording thread; it must not touch g.mutex
// in the steady state, and especially not once it can no longer produce output at all.
//   g_interesting_psos - how many entries in g.pipelines carry an interesting shader. While this
//                        is zero (the common case for most of a run) no draw needs the lock.
//   g_srv_work_done    - set when the dump budget is spent. From then on the handler is a load
//                        and a branch.
static std::atomic<uint32_t> g_interesting_psos{ 0 };
static std::atomic<bool>     g_srv_work_done{ false };

static uint32_t popcount64(uint64_t v)
{
	uint32_t n = 0;
	while (v != 0) { v &= (v - 1); ++n; }
	return n;
}

// FNV-1a over the whole DXBC container. Identity token only - never used as a security hash.
static uint64_t fnv1a64(const void *data, size_t size)
{
	const uint8_t *p = static_cast<const uint8_t *>(data);
	uint64_t h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < size; ++i)
	{
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

static const char *program_type_name(uint16_t t)
{
	switch (t)
	{
	case 0: return "PS";
	case 1: return "VS";
	case 2: return "GS";
	case 3: return "HS";
	case 4: return "DS";
	case 5: return "CS";
	default: return "??";
	}
}

static const char *param_type_name(pipeline_layout_param_type t)
{
	switch (t)
	{
	case pipeline_layout_param_type::descriptor_table:                       return "descriptor_table";
	case pipeline_layout_param_type::push_constants:                         return "push_constants";
	case pipeline_layout_param_type::push_descriptors:                       return "push_descriptors";
	case pipeline_layout_param_type::push_descriptors_with_ranges:           return "push_descriptors_with_ranges";
	case pipeline_layout_param_type::descriptor_table_with_flags:            return "descriptor_table_with_flags";
	case pipeline_layout_param_type::push_descriptors_with_ranges_and_flags: return "push_descriptors_with_ranges_and_flags";
	default: return "unknown";
	}
}

static const char *descriptor_type_name(descriptor_type t)
{
	// shader_resource_view == texture_shader_resource_view (2)
	// unordered_access_view == texture_unordered_access_view (3)
	switch (t)
	{
	case descriptor_type::sampler:                            return "sampler";
	case descriptor_type::sampler_with_resource_view:         return "sampler_with_resource_view";
	case descriptor_type::shader_resource_view:               return "srv(texture-or-generic)";
	case descriptor_type::unordered_access_view:              return "uav(texture-or-generic)";
	case descriptor_type::buffer_shader_resource_view:        return "srv(buffer)";
	case descriptor_type::buffer_unordered_access_view:       return "uav(buffer)";
	case descriptor_type::constant_buffer:                    return "cbv";
	case descriptor_type::shader_storage_buffer:              return "ssbo";
	case descriptor_type::acceleration_structure:             return "accel_struct";
	default: return "other";
	}
}

// Luma's format lists, reused verbatim (main.cpp:754-780). Their exact membership is the
// fingerprint we are testing, so they are not "improved" here.
enum class buffer_class { none, colour, depth, velocity };

static buffer_class classify_format(format f)
{
	switch (f)
	{
	case format::r11g11b10_float:
	case format::r16g16b16a16_float:
		return buffer_class::colour;

	case format::r32_float_x8_uint:
	case format::d32_float_s8_uint:
	case format::r24_g8_typeless:
	case format::r32_g8_typeless:
	case format::d24_unorm_s8_uint:
	case format::d16_unorm:
		return buffer_class::depth;

	case format::r16g16_unorm:
	case format::r16g16b16a16_unorm:
		return buffer_class::velocity;

	default:
		return buffer_class::none;
	}
}

static const char *buffer_class_name(buffer_class c)
{
	switch (c)
	{
	case buffer_class::colour:   return "COLOUR";
	case buffer_class::depth:    return "DEPTH";
	case buffer_class::velocity: return "VELOCITY";
	default: return "-";
	}
}

// =============================================================================================
// Device / command list lifetime
// =============================================================================================
// Forward declaration: the DLSS-NR bring-up is defined with the rest of the NR code, further
// down, but has to run from here.
static void nr_init_device(device *dev);
static void nr_destroy_device(device *dev);

static void on_init_device(device *dev)
{
	PROBE_GUARD_VOID({
		auto *sh = probe::pd_create<probe::device_shadow>(dev, probe::kDeviceShadowGuid);
		if (sh == nullptr)
			return;

		sh->is_d3d12 = (dev->get_api() == device_api::d3d12);

		LOGI("==================================================================");
		LOGI("STRAY DLSS-NR attached. device=0x%llx api=0x%x (%s) built against ReShade API v%u",
			(unsigned long long)dev->get_native(), (unsigned)dev->get_api(),
			sh->is_d3d12 ? "D3D12" : "NOT D3D12", (unsigned)RESHADE_API_VERSION);
		if (!sh->is_d3d12)
			LOGW("Device is not D3D12. The descriptor shadow is D3D12-specific and is DISABLED, so "
			     "DLSS-NR cannot run either; shader identification still runs. Launch STRAY with -dx12.");
		LOGI("==================================================================");

		nr_init_device(dev);
	})
}

static void on_destroy_device(device *dev)
{
	PROBE_GUARD_VOID({
		auto *sh = probe::pd_get<probe::device_shadow>(dev, probe::kDeviceShadowGuid);
		if (sh != nullptr)
		{
			std::unique_lock<std::shared_mutex> lock(sh->mutex);
			LOGI("shadow teardown: heaps=%zu layouts=%zu dropped_heap_growth=%llu copies_missing_src=%llu",
				sh->heaps.size(), sh->layouts.size(),
				(unsigned long long)sh->dropped_heap_growth.load(std::memory_order_relaxed),
				(unsigned long long)sh->copies_missing_src.load(std::memory_order_relaxed));
		}
		nr_destroy_device(dev);
		probe::pd_destroy<probe::device_shadow>(dev, probe::kDeviceShadowGuid);
	})
}

static void on_init_command_list(command_list *cmd)
{
	PROBE_GUARD_VOID({ (void)probe::pd_create<probe::cmd_shadow>(cmd, probe::kCmdShadowGuid); })
}

static void on_destroy_command_list(command_list *cmd)
{
	PROBE_GUARD_VOID({ probe::pd_destroy<probe::cmd_shadow>(cmd, probe::kCmdShadowGuid); })
}

static void on_reset_command_list(command_list *cmd)
{
	// Without this, stale bindings leak across frames on a recycled D3D12 allocator - which
	// would produce a confidently wrong answer rather than no answer.
	PROBE_GUARD_VOID({
		auto *cs = probe::pd_get<probe::cmd_shadow>(cmd, probe::kCmdShadowGuid);
		if (cs != nullptr)
			cs->reset();
	})
}

// =============================================================================================
// Pipeline layout shadow - and THE diagnostic line about the RS 1.1 defect
// =============================================================================================
static void on_init_pipeline_layout(device *dev, uint32_t param_count, const pipeline_layout_param *params, pipeline_layout layout)
{
	PROBE_GUARD_VOID({
		auto *sh = probe::pd_get<probe::device_shadow>(dev, probe::kDeviceShadowGuid);
		if (sh == nullptr || layout.handle == 0 || params == nullptr)
			return;

		// Deep copy OUTSIDE the lock. The source pointers are only valid inside this callback:
		// ReShade builds them in function-local std::vectors that die when it returns.
		std::vector<probe::layout_param> copied;
		probe::copy_layout_params(param_count, params, copied);

		std::unique_lock<std::shared_mutex> lock(sh->mutex);

		if (!sh->logged_rs_variant)
		{
			bool plain = false, flagged = false;
			for (uint32_t i = 0; i < param_count && i < probe::kMaxRootParams; ++i)
			{
				if (params[i].type == pipeline_layout_param_type::descriptor_table)
					plain = true;
				else if (params[i].type == pipeline_layout_param_type::descriptor_table_with_flags)
					flagged = true;
			}

			if (plain || flagged)
			{
				sh->logged_rs_variant = true;
				sh->saw_table_plain = plain;
				sh->saw_table_with_flags = flagged;

				LOGI("------------------------------------------------------------------");
				LOGI("ROOT SIGNATURE VARIANT (first pipeline layout with a descriptor table)");
				LOGI("  layout=0x%llx param_count=%u", (unsigned long long)layout.handle, param_count);
				if (flagged)
				{
					LOGI("  variant = descriptor_table_with_flags (enum 4)");
					LOGI("  => ReShade took the D3D_ROOT_SIGNATURE_VERSION_1_1/1_2 path.");
					LOGI("  => The upstream dangling-pointer/stride defect IS LIVE for this title.");
					LOGI("  => This probe deep-copies at the 40-byte stride, so it is unaffected.");
				}
				else
				{
					LOGI("  variant = descriptor_table (enum 0)");
					LOGI("  => ReShade took the D3D_ROOT_SIGNATURE_VERSION_1_0 path (28-byte stride).");
					LOGI("  => The upstream defect is NOT live here. This is UNEXPECTED for UE 4.27 "
					     "on vkd3d-proton, which negotiates RS 1.1; investigate before trusting it.");
				}
				LOGI("  sizeof(descriptor_range)=%zu sizeof(descriptor_range_with_flags)=%zu",
					sizeof(descriptor_range), sizeof(descriptor_range_with_flags));

				// Full param dump for the first layout - this is what tells us whether the
				// UE4 root-signature shape prediction holds.
				for (uint32_t i = 0; i < param_count && i < probe::kMaxRootParams; ++i)
				{
					const probe::layout_param &lp = copied[i];
					LOGI("  param[%u] type=%u (%s) ranges=%zu", i, (unsigned)lp.reported_type,
						param_type_name(lp.reported_type), lp.ranges.size());
					for (size_t k = 0; k < lp.ranges.size() && k < 8; ++k)
					{
						const descriptor_range_with_flags &r = lp.ranges[k];
						LOGI("      range[%zu] type=%u (%s) binding=%u dx_register=%u space=%u count=%u vis=0x%x",
							k, (unsigned)r.type, descriptor_type_name(r.type), r.binding,
							r.dx_register_index, r.dx_register_space, r.count, (unsigned)r.visibility);
					}
					// UE4 asserts exactly one range per table (D3D12RootSignature.cpp:554).
					if (lp.ranges.size() > 1 && lp.is_table)
						LOGW("      NOTE: %zu ranges in one table. UE 4.27 emits exactly 1; "
						     "either this is not a UE4 signature or the parse is wrong.", lp.ranges.size());
				}
				LOGI("------------------------------------------------------------------");
			}
		}

		sh->layouts[layout.handle] = std::move(copied);
	})
}

static void on_destroy_pipeline_layout(device *dev, pipeline_layout layout)
{
	PROBE_GUARD_VOID({
		auto *sh = probe::pd_get<probe::device_shadow>(dev, probe::kDeviceShadowGuid);
		if (sh == nullptr)
			return;
		std::unique_lock<std::shared_mutex> lock(sh->mutex);
		// erase, NOT clear-in-place: operator[] would insert an empty entry that a later lookup
		// would then index unchecked, and D3D12 recycles ID3D12RootSignature addresses.
		sh->layouts.erase(layout.handle);
	})
}

// =============================================================================================
// Shader identification at pipeline creation
// =============================================================================================
// Fold one OCCURRENCE of a shader into the gate histogram. g.mutex MUST be held.
// Counts occurrences, not distinct shaders, exactly as before the analysis cache was added.
static void fold_gate_counters_locked(const shader_record &rec)
{
	g.n_shaders_seen++;

	if (!rec.dxbc_valid)
	{
		if (rec.dxbc_is_dxil)
			g.n_dxil++;
		else
			g.n_not_dxbc++;
		return;
	}

	const probe::TAAShaderInfo &info = rec.info;
	const bool census_ok = (info.detected_2d_texture_float_count >= 4 && info.detected_3d_texture_float_count <= 1);
	if (!census_ok)
	{
		g.n_fail_census++;
		return;
	}

	g.n_pass_census++;
	if (!info.found_velocity_constant)
	{
		g.n_fail_velocity_const++;
		return;
	}

	g.n_pass_velocity_const++;
	if (info.loops_balanced_nonzero)
		g.n_fail_loops++;
	else if (!rec.passed_all_gates)
		g.n_fail_confidence++;
	else
		g.n_pass_all++;
}

// Everything that writes to the log on the FIRST sighting of a distinct shader. g.mutex held.
static void log_shader_detail_locked(const shader_record &rec)
{
	if (!rec.dxbc_valid)
	{
		if (rec.dxbc_is_dxil && g.gate_a_lines < kMaxGateALogLines)
		{
			g.gate_a_lines++;
			LOGW("shader 0x%016llx is DXIL (SM6). Token analysis does not apply - "
			     "UE 4.27 PCD3D_SM5 should be DXBC.", (unsigned long long)rec.hash);
		}
		return;
	}

	// FIX-5 diagnostic: settle the shader-model assumption by observation, once.
	if (!g.logged_shader_model)
	{
		g.logged_shader_model = true;
		LOGI("first DXBC chunk: shader model %u.%u, program_type=%u (%s). "
		     "Expected 5.0 for UE 4.27 PCD3D_SM5; 5.1 would invalidate the fixed token offsets.",
			rec.sm_major, rec.sm_minor, rec.program_type, program_type_name(rec.program_type));
	}

	const probe::TAAShaderInfo &info = rec.info;
	const bool census_ok = (info.detected_2d_texture_float_count >= 4 && info.detected_3d_texture_float_count <= 1);

	if ((!info.dcl_resource_index_dim_ok || !info.dcl_cbuffer_index_dim_ok) && !g.warned_index_dim)
	{
		g.warned_index_dim = true;
		LOGW("shader 0x%016llx has a declaration operand with an unexpected index dimension "
		     "(dcl_resource 1D ok=%d, dcl_constant_buffer 2D ok=%d). This is the SM 5.1 "
		     "signature: the fixed token offsets Luma uses would be WRONG. Treat every "
		     "register index below as suspect, and note that the SRV join will resolve NOTHING "
		     "for this shader rather than trust them.",
			(unsigned long long)rec.hash, (int)info.dcl_resource_index_dim_ok, (int)info.dcl_cbuffer_index_dim_ok);
	}

	// Two budgets: the census set is large, the 4.00801611f set is tiny and always worth a line.
	bool may_log = false;
	if (info.found_velocity_constant)
	{
		if (g.gate_b_lines < kMaxGateBLogLines) { g.gate_b_lines++; may_log = true; }
	}
	else if (census_ok)
	{
		if (g.gate_a_lines < kMaxGateALogLines) { g.gate_a_lines++; may_log = true; }
	}

	if (!may_log)
		return;

	LOGI("CAND hash=0x%016llx %s sm=%u.%u  tex2d=%d tex3d=%d out=%d maxreg=%d "
	     "srvmask=0x%016llx mrt=%d  vel_const=%s(pat%d)  loops_balanced=%d  confidence=%.1f  verdict=%s",
		(unsigned long long)rec.hash,
		rec.is_compute ? "CS" : program_type_name(rec.program_type),
		rec.sm_major, rec.sm_minor,
		info.detected_2d_texture_float_count, info.detected_3d_texture_float_count,
		info.output_count, info.max_texture_register,
		(unsigned long long)info.declared_srv_register_mask,
		(int)info.has_multiple_render_targets,
		info.found_velocity_constant ? "YES" : "no", info.velocity_constant_pattern,
		(int)info.loops_balanced_nonzero, info.confidence,
		rec.passed_all_gates ? "TAA-CANDIDATE" : (census_ok ? "rejected" : "rejected(census)"));

	if (rec.passed_all_gates && info.found_shader_info)
	{
		LOGI("     view-cbuffer: b%d size=%u float4s (%u bytes)  ClipToPrevClip row=%d (byte offset %d)",
			info.global_buffer_register_index, info.declared_cbuffer_size,
			info.declared_cbuffer_size * 16u, info.clip_to_prev_clip_start_index,
			info.clip_to_prev_clip_start_index >= 0 ? info.clip_to_prev_clip_start_index * 16 : -1);
	}
	else if (rec.passed_all_gates)
	{
		LOGI("     view-cbuffer: FindShaderInfo failed (no 4 consecutive .xywx/.xxyw cb loads); "
		     "b%d size=%u float4s", info.global_buffer_register_index, info.declared_cbuffer_size);
	}
}

// THREADING. UE4 compiles PSOs on a pool of async threads, so this runs concurrently with itself
// and with the per-draw handler, which shares g.mutex. The DXBC analysis - up to five byte-granular
// O(size) sweeps plus a full token walk - therefore runs with NO LOCK HELD, and its result is
// cached by bytecode hash so a repeat sighting costs one map lookup. Holding g.mutex across the
// analysis, and re-running it for every duplicate PSO built from the same shader, would stall
// command-list recording on every render thread behind PSO creation.
static void analyse_shader(const shader_desc &desc, bool subobject_is_compute, shader_record &out, bool &out_is_candidate)
{
	out_is_candidate = false;
	out = shader_record();
	out.hash = fnv1a64(desc.code, desc.code_size);
	out.is_compute = subobject_is_compute;

	// ---------------------------------------------------------------- already-analysed sighting
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		const auto it = g.shader_cache.find(out.hash);
		if (it != g.shader_cache.end())
		{
			out = it->second;
			fold_gate_counters_locked(out);
			out_is_candidate = out.interesting;
			return;
		}
	}

	// ---------------------------------------------------------------- analysis, NO LOCK HELD
	shader_record rec;
	rec.hash       = out.hash;
	rec.is_compute = out.is_compute;

	const probe::DxbcInfo dxbc = probe::StripDxbcContainer(desc.code, desc.code_size);
	rec.dxbc_valid   = dxbc.valid;
	rec.dxbc_is_dxil = dxbc.is_dxil;

	if (dxbc.valid)
	{
		rec.sm_major     = dxbc.sm_major;
		rec.sm_minor     = dxbc.sm_minor;
		rec.program_type = dxbc.program_type;
		// Program type from the bytecode is authoritative; the subobject kind is a cross-check.
		rec.is_compute   = (dxbc.program_type == 5);

		const bool passed = probe::IsUE4TAACandidate(dxbc.code, dxbc.size, rec.info);
		if (passed)
			probe::FindShaderInfo(dxbc.code, dxbc.size, rec.info);

		rec.passed_all_gates = passed;
		rec.interesting      = passed || rec.info.found_velocity_constant;
	}

	// ---------------------------------------------------------------- publish and log
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		// Two threads can analyse the same new hash at once. The analysis is a pure function of
		// the bytecode, so both produce the same record and whichever wins the insert is correct.
		const auto ins = g.shader_cache.emplace(rec.hash, rec);
		out = ins.first->second;
		fold_gate_counters_locked(out);
		if (ins.second)
			log_shader_detail_locked(out);
	}

	out_is_candidate = out.interesting;
}

static void on_init_pipeline(device *dev, pipeline_layout layout, uint32_t subobject_count, const pipeline_subobject *subobjects, pipeline pso)
{
	// init_pipeline (not create_pipeline) is used deliberately: it also fires for PSOs loaded
	// out of an ID3D12PipelineLibrary, which UE4 does whenever a PSO cache is present, and it
	// carries the pipeline handle needed to correlate with bind_pipeline.
	PROBE_GUARD_VOID({
		if (pso.handle == 0 || subobjects == nullptr)
			return;

		pipeline_record rec;
		rec.layout = layout;

		for (uint32_t i = 0; i < subobject_count; ++i)
		{
			const bool is_ps = (subobjects[i].type == pipeline_subobject_type::pixel_shader);
			const bool is_cs = (subobjects[i].type == pipeline_subobject_type::compute_shader);
			if (!is_ps && !is_cs)
				continue; // TAA in UE 4.27 is compute-only; PS is analysed for completeness
			if (subobjects[i].data == nullptr)
				continue;

			const auto *descs = static_cast<const shader_desc *>(subobjects[i].data);
			// count is 1 for graphics/compute PSOs but can exceed 1 for DXR state objects.
			for (uint32_t k = 0; k < subobjects[i].count; ++k)
			{
				if (descs[k].code == nullptr || descs[k].code_size < 32)
					continue;

				shader_record sr;
				bool candidate = false;
				analyse_shader(descs[k], is_cs, sr, candidate);

				// Keep the most interesting shader on this PSO.
				if (!rec.has_shader || (sr.interesting && !rec.shader.interesting) ||
					(sr.passed_all_gates && !rec.shader.passed_all_gates))
				{
					rec.has_shader = true;
					rec.shader = sr;
				}
			}
		}

		if (!rec.has_shader)
			return;

		std::lock_guard<std::mutex> lock(g.mutex);
		pipeline_record &slot = g.pipelines[pso.handle];
		const bool was_interesting = slot.has_shader && slot.shader.interesting;
		slot = rec;
		const bool now_interesting = rec.has_shader && rec.shader.interesting;
		// Keeps the per-draw fast path honest: while this counter is zero no draw can possibly
		// have anything to report, so no draw needs to take g.mutex at all.
		if (was_interesting != now_interesting)
		{
			if (now_interesting) g_interesting_psos.fetch_add(1, std::memory_order_relaxed);
			else                 g_interesting_psos.fetch_sub(1, std::memory_order_relaxed);
		}
	})
}

static void on_destroy_pipeline(device *, pipeline pso)
{
	PROBE_GUARD_VOID({
		std::lock_guard<std::mutex> lock(g.mutex);
		const auto it = g.pipelines.find(pso.handle);
		if (it == g.pipelines.end())
			return;
		if (it->second.has_shader && it->second.shader.interesting)
			g_interesting_psos.fetch_sub(1, std::memory_order_relaxed);
		g.pipelines.erase(it);
	})
}

// =============================================================================================
// Descriptor heap shadow. BOTH of these MUST return false.
// =============================================================================================
static bool on_update_descriptor_tables(device *dev, uint32_t count, const descriptor_table_update *updates)
{
	PROBE_GUARD_FALSE({
		auto *sh = probe::pd_get<probe::device_shadow>(dev, probe::kDeviceShadowGuid);
		if (sh == nullptr || !sh->is_d3d12 || updates == nullptr)
			return false;

		// PREPARE (get_descriptor_heap_offset, a virtual call back into ReShade) runs with NO
		// lock held; only the map writes are done under the exclusive lock, one acquisition per
		// chunk. Fixed-size chunk buffer, so this path never allocates.
		constexpr uint32_t kChunk = 64;
		probe::prepared_update staged[kChunk];
		for (uint32_t base = 0; base < count; base += kChunk)
		{
			const uint32_t n = (count - base < kChunk) ? (count - base) : kChunk;
			uint32_t m = 0;
			for (uint32_t i = 0; i < n; ++i)
			{
				if (probe::prepare_descriptor_update(*sh, dev, updates[base + i], staged[m]))
					m++;
			}
			if (m == 0)
				continue;
			std::unique_lock<std::shared_mutex> lock(sh->mutex);
			for (uint32_t j = 0; j < m; ++j)
				probe::apply_prepared_update(*sh, staged[j]);
		}
	})
}

static bool on_copy_descriptor_tables(device *dev, uint32_t count, const descriptor_table_copy *copies)
{
	PROBE_GUARD_FALSE({
		auto *sh = probe::pd_get<probe::device_shadow>(dev, probe::kDeviceShadowGuid);
		if (sh == nullptr || !sh->is_d3d12 || copies == nullptr)
			return false;

		// ReShade emits one descriptor_table_copy per DESCRIPTOR here (UE4's FD3D12DescriptorCache
		// passes pSrcDescriptorRangeSizes == nullptr, so every source range is size 1), which
		// means a 64-SRV table arrives as 64 entries on every draw from every recording thread.
		// So: no allocation per entry, and no ReShade call under our exclusive lock.
		constexpr uint32_t kChunk = 128;
		probe::prepared_copy staged[kChunk];
		for (uint32_t base = 0; base < count; base += kChunk)
		{
			const uint32_t n = (count - base < kChunk) ? (count - base) : kChunk;
			uint32_t m = 0;
			for (uint32_t i = 0; i < n; ++i)
			{
				if (probe::prepare_descriptor_copy(*sh, dev, copies[base + i], staged[m]))
					m++;
			}
			if (m == 0)
				continue;
			std::unique_lock<std::shared_mutex> lock(sh->mutex);
			for (uint32_t j = 0; j < m; ++j)
				probe::apply_prepared_copy(*sh, staged[j]);
		}
	})
}

// =============================================================================================
// Command-list binding shadow
// =============================================================================================
static void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pso)
{
	PROBE_GUARD_VOID({
		auto *cs = probe::pd_get<probe::cmd_shadow>(cmd, probe::kCmdShadowGuid);
		if (cs == nullptr)
			return;

		// DLSS-NR ADDITION. SetPipelineState and SetPipelineState1 are mutually exclusive on D3D12,
		// and the state restore has to replay whichever the application last used. ReShade reports
		// SetPipelineState with pipeline_stage::all and SetPipelineState1 with the ray-tracing
		// stages, which is the only signal available to tell the two apart.
		const bool is_state_object =
			((stages & pipeline_stage::all_ray_tracing) != 0) && ((stages & pipeline_stage::all_shader_stages) == 0);
		if (is_state_object)
		{
			cs->state_object = pso;
			cs->pso = pipeline{ 0 };
		}
		else
		{
			// D3D12 has one pipeline-state slot shared by Draw and Dispatch, and ReShade reports
			// SetPipelineState with pipeline_stage::all, so a single slot is correct.
			cs->pso = pso;
			cs->state_object = pipeline{ 0 };
		}

		cs->nr_checked = pipeline{ 0 };
		cs->nr_is_target = false;
		// Invalidate the memoised "is this PSO interesting" answer unconditionally rather than
		// relying on the handle differing: D3D12 recycles ID3D12PipelineState addresses, so the
		// same handle can be a different pipeline. This costs one lookup per SetPipelineState
		// instead of one per draw.
		cs->pso_checked = pipeline{ 0 };
		cs->pso_interesting = false;
	})
}

static void on_bind_descriptor_tables(command_list *cmd, shader_stage stages, pipeline_layout layout,
                                      uint32_t first, uint32_t count, const descriptor_table *tables,
                                      uint32_t /*dynamic_offset_count*/, const uint32_t * /*dynamic_offsets*/)
{
	PROBE_GUARD_VOID({
		auto *cs = probe::pd_get<probe::cmd_shadow>(cmd, probe::kCmdShadowGuid);
		if (cs == nullptr)
			return;

		const auto apply = [&](probe::pipe_bindings &b) {
			// A root-signature change invalidates every bound table. ReShade signals it as
			// count == 0 from SetGraphics/ComputeRootSignature.
			if (b.layout != layout)
				b.reset();
			b.layout = layout;

			if (layout.handle == 0 || count == 0 || tables == nullptr)
				return;
			if (static_cast<uint64_t>(first) + count > probe::kMaxRootParams)
				return;

			b.ensure(first + count);
			if (b.tables.size() < static_cast<size_t>(first) + count)
				return;

			for (uint32_t i = 0; i < count; ++i)
			{
				b.tables[first + i] = tables[i];
				b.is_root_descriptor[first + i] = false;

				// DLSS-NR ADDITION - the state restore. descriptor_table::handle on D3D12 IS the raw
				// D3D12_GPU_DESCRIPTOR_HANDLE::ptr that was passed to
				// SetGraphics/ComputeRootDescriptorTable, passed through unmodified by ReShade's
				// wrapper. That identity is not taken on trust: it is checked arithmetically against
				// the heap the table lives in, once, before the pass is allowed to run at all - see
				// probe::verify_table_handle_identity.
				b.arg_kind[first + i] = (tables[i].handle != 0) ? probe::root_arg_kind::table
				                                                : probe::root_arg_kind::none;
				b.arg_gpu[first + i]  = tables[i].handle;
			}
		};

		if ((stages & shader_stage::all_graphics) != 0) apply(cs->gfx);
		if ((stages & shader_stage::all_compute)  != 0) apply(cs->cmp);
	})
}

static void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout layout,
                                uint32_t layout_param, const descriptor_table_update &update)
{
	// Two jobs:
	//  1. Invalidate, so a stale table handle at this root parameter is not reused.
	//  2. Capture the View uniform buffer. On D3D12 every CBV is a ROOT DESCRIPTOR
	//     (MAX_ROOT_CBVS == MAX_CBS == 16, so GDescriptorTableCBVSlotMask == 0), which means it
	//     never enters a descriptor heap and is reachable ONLY through this event.
	//
	// SAFETY, verified in d3d12_command_list.cpp:
	//   constant_buffer            -> update.descriptors is &buffer_range (24 bytes) - safe
	//   buffer_shader_resource_view,
	//   buffer_unordered_access_view,
	//   acceleration_structure     -> update.descriptors is &BufferLocation, a bare 8-byte
	//                                 D3D12_GPU_VIRTUAL_ADDRESS on ReShade's stack.
	// Reading those as buffer_range or resource_view is a 16-byte stack OVERREAD. We never do.
	PROBE_GUARD_VOID({
		auto *cs = probe::pd_get<probe::cmd_shadow>(cmd, probe::kCmdShadowGuid);
		if (cs == nullptr || layout_param >= probe::kMaxRootParams)
			return;

		const auto apply = [&](probe::pipe_bindings &b) {
			if (b.layout != layout)
			{
				b.reset();
				b.layout = layout;
			}
			b.ensure(layout_param + 1);
			if (b.tables.size() <= layout_param)
				return;

			if (b.is_root_descriptor.size() <= layout_param || b.root_cbvs.size() <= layout_param)
				return;

			b.tables[layout_param] = { 0 };
			b.is_root_descriptor[layout_param] = true;
			b.arg_kind[layout_param] = probe::root_arg_kind::none;
			b.arg_gpu[layout_param]  = 0;

			if (update.type == descriptor_type::constant_buffer && update.descriptors != nullptr && update.count >= 1)
			{
				b.root_cbvs[layout_param].valid = true;
				b.root_cbvs[layout_param].range = *static_cast<const buffer_range *>(update.descriptors);
				// DLSS-NR ADDITION. The GPU virtual address is NOT computed here: ReShade gives a
				// (resource, offset) pair, and turning that into an address costs an
				// ID3D12Resource::GetGPUVirtualAddress call - which UE 4.27 would make us pay on every
				// draw, on every parallel recording thread. It is done once, at restore time, in
				// probe::capture_pipe.
				b.arg_kind[layout_param] = probe::root_arg_kind::cbv;
			}
			else
			{
				b.root_cbvs[layout_param].valid = false;

				// DLSS-NR ADDITION - root SRVs and root UAVs.
				//
				// SAFETY, verified in ReShade's d3d12_command_list.cpp: for these three descriptor
				// types update.descriptors points at a bare 8-byte D3D12_GPU_VIRTUAL_ADDRESS on
				// ReShade's own stack, NOT at a buffer_range. Reading 24 bytes there would be a stack
				// overread; exactly 8 are read.
				if (update.descriptors != nullptr && update.count >= 1 &&
				    (update.type == descriptor_type::buffer_shader_resource_view ||
				     update.type == descriptor_type::buffer_unordered_access_view ||
				     update.type == descriptor_type::acceleration_structure))
				{
					uint64_t va = 0;
					std::memcpy(&va, update.descriptors, sizeof(va));
					// Recorded even when va == 0. A null root SRV/UAV is a legal binding, and the whole
					// point of the shadow is that probe::capture_pipe can tell "the application bound
					// null here" apart from "no address was recovered" - the second of which now refuses
					// the plan outright, because replaying the root signature would leave the parameter
					// UNDEFINED rather than null.
					// An acceleration structure is bound through SetComputeRootShaderResourceView.
					b.arg_kind[layout_param] = (update.type == descriptor_type::buffer_unordered_access_view)
						? probe::root_arg_kind::uav : probe::root_arg_kind::srv;
					b.arg_gpu[layout_param] = va;
				}
			}
		};

		if ((stages & shader_stage::all_graphics) != 0) apply(cs->gfx);
		if ((stages & shader_stage::all_compute)  != 0) apply(cs->cmp);
	})
}

// DLSS-NR ADDITION. Root 32-bit constants. UE 4.27's D3D12 root signatures use root CBVs rather
// than root constants, so this is expected to fire zero times in STRAY - it exists so that a
// build which DOES use them is restored faithfully rather than silently losing bindings. An
// overflow of the (deliberately small) shadow latches pipe_bindings::consts_overflowed, and the
// DLSS-NR pass then refuses to run at all rather than replay an incomplete root state.
static void on_push_constants(command_list *cmd, shader_stage stages, pipeline_layout layout,
                              uint32_t layout_param, uint32_t first, uint32_t count, const void *values)
{
	PROBE_GUARD_VOID({
		auto *cs = probe::pd_get<probe::cmd_shadow>(cmd, probe::kCmdShadowGuid);
		if (cs == nullptr || layout_param >= probe::kMaxRootParams)
			return;

		const auto apply = [&](probe::pipe_bindings &b) {
			if (b.layout != layout)
			{
				b.reset();
				b.layout = layout;
			}
			b.ensure(layout_param + 1);
			if (b.arg_kind.size() <= layout_param)
				return;

			b.arg_kind[layout_param] = probe::root_arg_kind::constants;
			b.arg_gpu[layout_param]  = 0;
			b.record_constants(layout_param, first, count, values);
		};

		if ((stages & shader_stage::all_graphics) != 0) apply(cs->gfx);
		if ((stages & shader_stage::all_compute)  != 0) apply(cs->cmp);
	})
}

static void on_bind_render_targets_and_depth_stencil(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
	PROBE_GUARD_VOID({
		auto *cs = probe::pd_get<probe::cmd_shadow>(cmd, probe::kCmdShadowGuid);
		if (cs == nullptr)
			return;

		cs->rtv_count = (count > 8) ? 8 : count;
		for (uint32_t i = 0; i < 8; ++i)
			cs->rtvs[i] = (rtvs != nullptr && i < cs->rtv_count) ? rtvs[i] : resource_view{ 0 };
		cs->dsv = dsv;
	})
}

// =============================================================================================
// The join: at a draw/dispatch using a candidate shader, resolve and log every bound SRV.
// =============================================================================================
static void describe_view(device *dev, resource_view view, char *out, size_t out_size,
                          format *out_format, uint32_t *out_w, uint32_t *out_h)
{
	*out_format = format::unknown;
	*out_w = 0;
	*out_h = 0;

	if (view.handle == 0)
	{
		std::snprintf(out, out_size, "view=0x0 (unbound)");
		return;
	}

	// Resource views created by the application are only guaranteed valid DURING an event
	// callback, which is why everything is resolved to plain data here and now.
	const resource res = probe::abi_get_resource_from_view(dev, view);
	if (res.handle == 0)
	{
		// D3D12 allows resource views with no resource - a null descriptor. UE4 pads unused
		// table slots with exactly these.
		std::snprintf(out, out_size, "res=0x0 (NULL DESCRIPTOR)");
		return;
	}

	// These three go through probe::abi_* rather than the member functions directly: they are
	// the only ReShade virtuals this add-on calls that return a class type BY VALUE, and the
	// Microsoft and Itanium C++ ABIs disagree about how that is done. See msvc_abi.hpp.
	const resource_view_desc vd = probe::abi_get_resource_view_desc(dev, view);
	const resource_desc rd = probe::abi_get_resource_desc(dev, res);

	if (rd.type != resource_type::texture_2d && rd.type != resource_type::surface)
	{
		std::snprintf(out, out_size, "res=0x%llx NON-2D (resource_type=%u)",
			(unsigned long long)res.handle, (unsigned)rd.type);
		return;
	}

	*out_format = rd.texture.format;
	*out_w = rd.texture.width;
	*out_h = rd.texture.height;

	std::snprintf(out, out_size,
		"res=0x%llx  res_fmt=%s  view_fmt=%s  %ux%u  layers=%u mips=%u samples=%u  usage=0x%x",
		(unsigned long long)res.handle,
		probe::format_name(rd.texture.format),
		probe::format_name(vd.format),
		rd.texture.width, rd.texture.height,
		(unsigned)rd.texture.depth_or_layers, (unsigned)rd.texture.levels, (unsigned)rd.texture.samples,
		(unsigned)rd.usage);
}

static void dump_bindings(command_list *cmd, bool compute)
{
	// ------------------------------------------------------------------ lock-free fast path
	// This runs on EVERY draw, draw_indexed and dispatch, from every UE4 parallel recording
	// thread. Neither branch below touches a shared mutex, and between them they cover the
	// overwhelming majority of calls: before the first interesting PSO exists, and after the
	// dump budget is spent, there is provably nothing to do.
	if (g_srv_work_done.load(std::memory_order_relaxed))
		return;
	if (g_interesting_psos.load(std::memory_order_relaxed) == 0)
		return;

	auto *cs = probe::pd_get<probe::cmd_shadow>(cmd, probe::kCmdShadowGuid);
	if (cs == nullptr || cs->pso.handle == 0)
		return;

	// Memoised per command list, refreshed once per bind_pipeline rather than once per draw.
	// A D3D12 command list is recorded by one thread at a time, so this needs no lock.
	if (cs->pso_checked != cs->pso)
	{
		bool interesting = false;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			const auto it = g.pipelines.find(cs->pso.handle);
			interesting = (it != g.pipelines.end() && it->second.has_shader && it->second.shader.interesting);
		}
		cs->pso_checked     = cs->pso;
		cs->pso_interesting = interesting;
	}
	if (!cs->pso_interesting)
		return;

	device *const dev = cmd->get_device();
	if (dev == nullptr)
		return;

	auto *sh = probe::pd_get<probe::device_shadow>(dev, probe::kDeviceShadowGuid);
	if (sh == nullptr || !sh->is_d3d12)
		return;

	// ---------------------------------------------------- budget, retry accounting and lookup
	shader_record shader;
	pipeline_layout pso_layout = { 0 };
	uint32_t attempt = 0;
	{
		std::lock_guard<std::mutex> lock(g.mutex);

		const auto it = g.pipelines.find(cs->pso.handle);
		if (it == g.pipelines.end() || !it->second.has_shader)
			return;
		if (!it->second.shader.interesting)
			return;

		shader = it->second.shader;
		pso_layout = it->second.layout;

		// A shader is retired only once an attempt actually RESOLVED something. Retiring it on
		// the first attempt - which is what marking it dumped before the join would do - throws
		// away the probe's only chance to characterise it if that attempt happened to land
		// during PSO warm-up, before the descriptor shadow had seen a full frame of
		// CopyDescriptors. The failure is transient; the loss of the slot was not.
		//
		// Retirement is MONOTONE - srv_resolved_hashes and srv_attempts only ever grow - so once a
		// shader is written off, this command list can stop paying for it: clearing the per-list
		// memo turns "one g.mutex acquisition per dispatch of a retired candidate" into "one per
		// SetPipelineState". bind_pipeline re-arms the memo, so a wrong answer here can only cost
		// one extra lookup, never a missed dump.
		if (g.srv_resolved_hashes.find(shader.hash) != g.srv_resolved_hashes.end())
		{
			cs->pso_interesting = false;
			return;
		}
		if (g.srv_dumps >= kMaxSrvDumps)
		{
			g_srv_work_done.store(true, std::memory_order_relaxed);
			return;
		}
		uint32_t &tries = g.srv_attempts[shader.hash];
		if (tries >= kMaxSrvAttemptsPerShader)
		{
			cs->pso_interesting = false;
			return;
		}
		attempt = ++tries;
		g.srv_dumps++;
	}

	const probe::pipe_bindings &b = compute ? cs->cmp : cs->gfx;

	// The declared SRV range on UE4 is always MAX_SRVS (64) because vkd3d-proton reports
	// ResourceBindingTier 3, regardless of what the shader uses. Slots outside the shader's own
	// declaration hold STALE descriptors from unrelated earlier draws that shared the heap block.
	// We still walk them, but mark them, because a false positive there is the most likely way
	// this probe could mislead.
	//
	// Resolve (i.e. call get_resource_from_view / get_resource_desc) only for registers the
	// shader ACTUALLY DECLARES, as an exact bitmask rather than a ceiling - a sparse declaration
	// such as t0,t1,t2,t14 must not drag t3..t13 in with it. Those descriptors were written for
	// THIS draw and their resources are necessarily alive; everything else may already be
	// destroyed, and get_resource_desc dereferences the ID3D12Resource* directly.
	//
	// If the census is untrustworthy - nothing parsed, or the SM 5.1 index-dimension check failed
	// so every register index it produced is known-wrong - resolve NOTHING. There is no "modest
	// fixed window" that is any more justified than the numbers we just rejected.
	const bool census_usable = (shader.info.max_texture_register >= 0) && shader.info.dcl_resource_index_dim_ok;
	const uint64_t declared_mask = census_usable ? shader.info.declared_srv_register_mask : 0ull;

	std::vector<probe::resolved_srv> srvs;
	probe::resolve_bound_srvs(dev, *sh, b, declared_mask, probe::kMaxSrvWalk, srvs);

	LOGI("==================================================================");
	LOGI("SRV TABLE for %s shader 0x%016llx  (%s)  attempt %u/%u",
		compute ? "DISPATCH" : "DRAW", (unsigned long long)shader.hash,
		shader.passed_all_gates ? "TAA-CANDIDATE, all gates passed"
		                        : "velocity-constant carrier, did NOT pass all gates",
		attempt, kMaxSrvAttemptsPerShader);
	LOGI("  confidence=%.1f  tex2d_census=%d  max_texture_register=%d  declared_srv_mask=0x%016llx (%u regs)  "
	     "pso=0x%llx  pso_layout=0x%llx  bound_layout=0x%llx",
		shader.info.confidence, shader.info.detected_2d_texture_float_count,
		shader.info.max_texture_register, (unsigned long long)declared_mask, popcount64(declared_mask),
		(unsigned long long)cs->pso.handle, (unsigned long long)pso_layout.handle,
		(unsigned long long)b.layout.handle);

	if (!census_usable)
	{
		LOGW("  census UNUSABLE (max_texture_register=%d, dcl_resource_index_dim_ok=%d). Every "
		     "register index the token analysis produced is known-wrong, so NOTHING is resolved: "
		     "the slots below are listed for their heap positions only.",
			shader.info.max_texture_register, (int)shader.info.dcl_resource_index_dim_ok);
	}
	if (b.layout.handle == 0)
	{
		LOGW("  no root signature recorded on this command list for the %s pipe - "
		     "nothing to resolve. (Was reset_command_list missed, or the table bound before "
		     "the probe attached?)", compute ? "compute" : "graphics");
	}
	if (srvs.empty())
	{
		LOGW("  ZERO SRVs resolved. Most likely causes, in order: the descriptor shadow never "
		     "saw the CopyDescriptors that filled the shader-visible heap; the add-on attached "
		     "after the views were created; or this pipe's tables were never bound. This shader "
		     "will be retried on a later draw (attempt %u of %u used).",
			attempt, kMaxSrvAttemptsPerShader);
	}

	uint32_t n_colour = 0, n_depth = 0, n_velocity = 0, n_beyond = 0, n_other_space = 0;

	for (const probe::resolved_srv &r : srvs)
	{
		if (!r.safe_to_resolve)
		{
			// Occupied slot the shader never declared (or in a register space it cannot have
			// declared). Reported so the TIER_3 stale-descriptor effect is visible, but
			// deliberately NOT resolved.
			n_beyond++;
			if (r.dx_register_space != 0)
				n_other_space++;
			LOGI("  t%-3u space=%u rootparam=%u heapoff=%u  view=0x%llx  slot=%s  "
			     "[NOT DECLARED BY SHADER: stale, not resolved]",
				r.dx_register_index, r.dx_register_space, r.root_param, r.heap_offset,
				(unsigned long long)r.view.handle, descriptor_type_name(r.slot_type));
			continue;
		}

		char desc[512];
		format fmt = format::unknown;
		uint32_t w = 0, h = 0;
		describe_view(dev, r.view, desc, sizeof(desc), &fmt, &w, &h);

		const buffer_class bc = classify_format(fmt);
		if (bc == buffer_class::colour)   n_colour++;
		if (bc == buffer_class::depth)    n_depth++;
		if (bc == buffer_class::velocity) n_velocity++;

		LOGI("  t%-3u space=%u rootparam=%u heapoff=%u  view=0x%llx  slot=%s  class=%-8s  %s",
			r.dx_register_index, r.dx_register_space, r.root_param, r.heap_offset,
			(unsigned long long)r.view.handle, descriptor_type_name(r.slot_type),
			buffer_class_name(bc), desc);
	}

	const bool produced = (b.layout.handle != 0) && !srvs.empty();

	LOGI("  --- resolved over the shader's declared registers: colour=%u depth=%u velocity=%u  "
	     "(plus %u occupied slots the shader never declared, %u of them in a non-zero register "
	     "space, none resolved)",
		n_colour, n_depth, n_velocity, n_beyond, n_other_space);
	LOGI("  --- Luma quorum (colour>=2 && depth>=1 && velocity>=1): %s",
		(n_colour >= 2 && n_depth >= 1 && n_velocity >= 1) ? "MET" : "NOT MET");

	if (n_velocity > 1)
	{
		LOGW("  AMBIGUITY: %u velocity-format SRVs bound at once. UE4 creates "
		     "ImaginaryReflectionGBufferVelocity at the SAME r16g16b16a16_unorm and the same "
		     "extent (RayTracingReflections.cpp:851). Format alone CANNOT disambiguate these - "
		     "use the register index (TAA reads GBufferVelocityTexture) or resource identity.", n_velocity);
	}
	else if (n_velocity == 1)
	{
		LOGI("  --- exactly one velocity-format SRV in range: the ImaginaryReflectionGBufferVelocity "
		     "ambiguity is NOT live for this shader.");
	}

	// ---------------------------------------------------- root CBVs (the View uniform buffer)
	for (size_t p = 0; p < b.root_cbvs.size(); ++p)
	{
		if (!b.root_cbvs[p].valid)
			continue;
		const buffer_range &br = b.root_cbvs[p].range;
		uint64_t size = 0;
		if (br.buffer.handle != 0)
		{
			const resource_desc rd = probe::abi_get_resource_desc(dev, br.buffer);
			if (rd.type == resource_type::buffer)
				size = rd.buffer.size;
		}
		LOGI("  root CBV param=%zu  buffer=0x%llx offset=%llu buffer_size=%llu",
			p, (unsigned long long)br.buffer.handle,
			(unsigned long long)br.offset, (unsigned long long)size);
	}

	// ---------------------------------------------------- render targets / depth stencil
	if (!compute)
	{
		for (uint32_t i = 0; i < cs->rtv_count; ++i)
		{
			if (cs->rtvs[i].handle == 0)
				continue;
			char desc[512];
			format fmt; uint32_t w, h;
			describe_view(dev, cs->rtvs[i], desc, sizeof(desc), &fmt, &w, &h);
			LOGI("  RTV[%u] view=0x%llx  %s", i, (unsigned long long)cs->rtvs[i].handle, desc);
		}
		if (cs->dsv.handle != 0)
		{
			char desc[512];
			format fmt; uint32_t w, h;
			describe_view(dev, cs->dsv, desc, sizeof(desc), &fmt, &w, &h);
			LOGI("  DSV    view=0x%llx  %s", (unsigned long long)cs->dsv.handle, desc);
		}
	}
	else
	{
		LOGI("  (compute dispatch - render targets do not apply; outputs are UAVs, not tracked "
		     "by this probe)");
	}
	LOGI("==================================================================");

	// Retire the shader only now, and only if this attempt actually produced a resolution.
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (produced)
			g.srv_resolved_hashes.insert(shader.hash);
		if (g.srv_dumps >= kMaxSrvDumps)
			g_srv_work_done.store(true, std::memory_order_relaxed);
	}
}

// =============================================================================================
// DLSS-NR
//
// Everything above this line is the probe, unchanged in behaviour. Everything below is the part
// that actually evaluates NGX feature 18.
// =============================================================================================

static cfg::config  g_cfg;
static ngx::snippet g_snippet;

// True only once nvngx_dlssnr.dll has been loaded through remix_nvngx.dll AND
// NVSDK_NGX_D3D12_Init_Ext has succeeded. Read on the hot dispatch path before anything else, so
// a disabled or snippet-less install pays one relaxed atomic load per dispatch and nothing more.
static std::atomic<bool> g_nr_armed{ false };
// Set once the snippet module is loaded; cleared by the first render-thread pass.
static std::atomic<bool> g_nr_pending_init{ false };
static bool nr_lazy_ngx_init(device *dev);

// Bring-up diagnostic: the run path has a dozen silent early-returns, and "the pass simply does
// not run" is indistinguishable between them from the log. Reports each distinct reason once.
// Crash bisection: one line per stage, first pass only, so the LAST line in the log names the
// stage that died. ReShade's log flushes per message, so a hard crash still leaves the marker.
#define NR_STAGE(n) do { \
    static std::atomic<bool> s_st{ false }; bool e2_ = false; \
    if (s_st.compare_exchange_strong(e2_, true, std::memory_order_acq_rel)) \
        LOGI("DLSS-NR stage: %s", n); } while (0)

#define NR_BAIL(why) do { \
    static std::atomic<bool> s_said{ false }; bool e_ = false; \
    if (s_said.compare_exchange_strong(e_, true, std::memory_order_acq_rel)) \
        LOGW("DLSS-NR: pass did not run - %s", why); \
    return; } while (0)

// The graphics queue, stashed at init_command_queue. Needed for wait_idle before destroying a
// resource or releasing an NGX feature that in-flight work may still reference.
static std::atomic<command_queue *> g_queue{ nullptr };

// Private-data key for the per-device DLSS-NR state. Distinct from the probe's two keys and from
// anything RenoDX or Luma might use in the same process.
static const uint8_t kNrStateGuid[16] = {
	0x2e,0xa7,0x6b,0x50, 0x18,0xc4, 0x47,0x3f, 0xb2,0x66, 0x0d,0x95,0x3a,0xe8,0x71,0x4c };

struct nr_state
{
	std::mutex mutex;

	ID3D12Device         *d3d12  = nullptr;
	ngx::parameter_block *params = nullptr;
	void                 *feature = nullptr;   // NVSDK_NGX_Handle *

	// Our own DLSSNR.Output. Created UAV-capable and copy-source-capable at the colour extent.
	// See nr_ensure_output for why its format is not a constant, and why it is NOT out_fmt when
	// the codec is running.
	resource out_tex = { 0 };
	uint32_t out_w = 0, out_h = 0;
	// The TAA output UAV's OWN format - i.e. the format anything copied back into the frame must
	// have. orig_tex and result_tex are created in it.
	format   out_fmt = format::unknown;
	// The format out_tex - the network's target, InNeural at t2 - was ACTUALLY created in.
	//
	// Equal to out_fmt when the codec is off. When the codec is on it is forced to
	// r16g16b16a16_float, matching proxy_tex, and that equality is load-bearing rather than
	// cosmetic: the decode's identity property (hdr_codec.hpp) is the statement that InProxy and
	// InNeural hold IDENTICAL BIT PATTERNS when the network returns its input unchanged, and two
	// surfaces of different formats cannot. With an r11g11b10_float neural target a perfectly
	// identity network still yields a (neural - proxy) of up to ~2^-7 relative in R/G and ~2^-6
	// in B - a channel-asymmetric, per-pixel noise floor added to every pixel of the frame.
	// Remix allocates BOTH surfaces VK_FORMAT_R16G16B16A16_SFLOAT for exactly this reason
	// (rtx_neural_rendering.cpp:108 and :115).
	//
	// This is safe for the copy-back because with the codec on the copy source is result_tex,
	// which is out_fmt - out_tex is never the copy source on that path. It is also strictly
	// better for the evaluate: DLSSNR.Color (the proxy) and DLSSNR.Output now MATCH, where
	// leaving out_tex at an r11g11b10_float out_fmt would hand the snippet a mismatched pair.
	format   neural_fmt = format::unknown;

	// The guide (depth / motion vector) extent the current history was accumulated against.
	// Changing it - which UE4 does when screen percentage or the upscaler quality moves - shifts
	// DLSSNR.MVecScaleX/Y underneath a temporal history built on the old grid, and nothing else
	// notices. Latched here to force a single reset frame.
	uint32_t guide_w = 0, guide_h = 0;

	bool     need_reset = true;          // DLSSNR.Reset for the next evaluate
	bool     feature_failed = false;     // latched per (out_w,out_h); cleared when they move
	bool     pending_teardown = false;   // serviced on the next present, on the main thread
	uint64_t evaluate_count = 0;

	// -1 mismatch, 0 not yet checked, 1 verified. See probe::verify_table_handle_identity.
	int      table_identity = 0;
	// The same three-way answer for probe::verify_heap_is_native.
	int      heap_identity  = 0;

	// One-shot log latches. Every one of these guards a message that would otherwise be printed
	// on every single frame.
	bool logged_taa_found        = false;
	bool logged_srv_reject       = false;
	bool logged_uav_reject       = false;
	bool logged_uav_ambiguous    = false;
	bool logged_restore_reject   = false;
	bool logged_create_fail      = false;
	bool logged_eval_fail        = false;
	bool logged_table_identity   = false;
	bool logged_heap_identity    = false;
	bool logged_depth_format     = false;
	bool logged_mvec_format      = false;
	bool logged_feedback_loop    = false;
	bool logged_owned_throw      = false;
	bool logged_copy_fmt         = false;
	bool logged_identity         = false;
	bool logged_codec_off        = false;
	bool logged_codec_tex_fail   = false;
	bool logged_hist_active      = false;
	bool logged_hist_dropped     = false;
	bool logged_hist_odd_reg     = false;
	bool logged_hist_double_arm  = false;
	bool logged_hist_tex_fail    = false;

	// ---- the HDR colour codec (hdr_codec.hpp) ------------------------------------------------
	// The display-referred proxy the network is actually shown, and what is bound as DLSSNR.Color
	// when the codec is on. ALWAYS r16g16b16a16_float, never the TAA output's own format: the
	// network must not be handed an r11g11b10_float proxy, and nothing copies this texture, so the
	// "identical or same typeless family" rule that constrains out_tex does not apply to it.
	resource      proxy_tex  = { 0 };
	resource_view proxy_srv  = { 0 };
	resource_view proxy_uav  = { 0 };

	// The PRE-DENOISE TAA output of this frame. ONE texture, TWO jobs:
	//   * it is the decode's `original` - the untouched HDR image the residual is added onto;
	//   * it is the pristine copy the temporal-feedback fix writes back over the game's history
	//     buffer at the start of the NEXT accepted dispatch.
	// Both readers want exactly the same bytes, and the timing works out: the restore consumes
	// frame N's content at the top of frame N+1, before the save overwrites it.
	resource      orig_tex   = { 0 };
	resource_view orig_srv   = { 0 };

	// The decode's destination. A third texture is unavoidable: D3D12 cannot have one resource in
	// UNORDERED_ACCESS and in SHADER_RESOURCE at the same time, and the decode reads the original
	// while writing the result. The existing copy-back then puts this over the TAA output, so the
	// measured copy path is reused verbatim rather than replaced.
	resource      result_tex = { 0 };
	resource_view result_uav = { 0 };

	// An SRV on our own NGX output, so the decode can read the network's answer.
	resource_view out_srv    = { 0 };

	hdr_codec::pipelines codec;         // root signatures + PSOs, created once per device
	bool codec_textures_ok = false;     // proxy/result/views exist at (out_w, out_h)
	// Latched for the whole run: the DXBC could not be produced, or the root signature / PSO could
	// not be created. Nothing about a resolution change can undo that, so it is never cleared.
	bool codec_failed      = false;
	bool orig_ok           = false;     // orig_tex exists at (out_w, out_h)
	// Latched per (out_w, out_h) only, exactly like feature_failed: a create_resource that failed
	// at one resolution may well succeed at another, and retrying it every frame would be a
	// per-frame allocation attempt on the hot path.
	bool codec_tex_failed  = false;
	bool orig_failed       = false;

	// ---- temporal feedback fix ----------------------------------------------------------------
	// The resource orig_tex's pristine copy was taken FROM, i.e. the one UE 4.27 extracts as the
	// next frame's TAA history. Zero means "nothing pending"; it is one-shot in both directions,
	// so stale content is never restored twice.
	uint64_t pending_res = 0;
	uint32_t pending_w = 0, pending_h = 0;
	format   pending_fmt = format::unknown;
	// ATOMIC, and deliberately so: the periodic census in on_present reads these on the MAIN
	// thread while nr_try_run writes them on a recording thread. It must not take st->mutex to do
	// it - on_present already holds g.mutex there, and nr_try_run takes st->mutex and then g.mutex,
	// so a lock in the other order would be a textbook AB/BA deadlock between the two threads.
	std::atomic<uint64_t> hist_restored{ 0 };
	std::atomic<uint64_t> hist_dropped{ 0 };
	// Same reason: a snapshot of what the last accepted dispatch actually had available, so the
	// census can report it without reaching into mutex-protected state.
	std::atomic<bool> census_codec_on{ false };
	std::atomic<bool> census_orig_on{ false };

	// The last few resources the copy-back wrote the denoised image INTO. UE 4.27 ping-pongs the
	// TAA colour/history pair, so seeing this frame's output UAV in here is direct evidence that
	// what we wrote last time is being consumed as history now - the feedback loop documented in
	// README "Known gaps". Small and lossy on purpose: it exists to fire a one-shot warning, not
	// to be an accounting record.
	uint64_t copied_into[4] = { 0, 0, 0, 0 };
	uint32_t copied_into_next = 0;
};

// --------------------------------------------------------------------------------------------
// Small helpers
// --------------------------------------------------------------------------------------------
struct nr_view_info
{
	resource res = { 0 };
	format   fmt = format::unknown;
	uint32_t w = 0, h = 0;
	bool     ok = false;
};

// Resolves a view to its resource and that resource's real 2D description. Goes through the
// probe's abi_* thunks because get_resource_from_view and get_resource_desc are two of the three
// ReShade virtuals that return a class BY VALUE, where the Microsoft and Itanium C++ ABIs
// disagree. Both return a zeroed value if the ABI self-check did not pass, so 'ok' stays false
// and the pass simply does not run - it never resolves something wrong.
static nr_view_info nr_describe(device *dev, resource_view view)
{
	nr_view_info o;
	if (dev == nullptr || view.handle == 0)
		return o;

	const resource res = probe::abi_get_resource_from_view(dev, view);
	if (res.handle == 0)
		return o;

	const resource_desc rd = probe::abi_get_resource_desc(dev, res);
	if (rd.type != resource_type::texture_2d && rd.type != resource_type::surface)
		return o;

	o.res = res;
	o.fmt = rd.texture.format;
	o.w   = rd.texture.width;
	o.h   = rd.texture.height;
	o.ok  = (o.w != 0 && o.h != 0);
	return o;
}

static void nr_log_ngx(reshade::log::level lvl, const char *what, ngx::Result r)
{
	logf(lvl, "DLSS-NR: %s -> 0x%08x (%s)", what, (unsigned)r, ngx::result_to_string(r));
}

// --------------------------------------------------------------------------------------------
// Teardown. MUST run on a thread that is not recording the command list this feature was used
// on, and MUST idle the queue first: destroy_resource's contract is "make sure the resource is no
// longer in use on the GPU", and NGX handles are not reference counted either.
//
// The only callers are present, destroy_swapchain(resize) and destroy_device - all of them the
// main thread, outside command-list recording.
// --------------------------------------------------------------------------------------------
static void nr_release_feature_and_output(device *dev, nr_state &st, const char *why)
{
	command_queue *q = g_queue.load(std::memory_order_relaxed);
	if (q != nullptr)
		q->wait_idle();

	if (st.feature != nullptr)
	{
		if (g_snippet.release_feature != nullptr)
		{
			const ngx::Result r = g_snippet.release_feature(st.feature);
			if (ngx::failed(r))
				nr_log_ngx(reshade::log::level::warning, "ReleaseFeature", r);
		}
		st.feature = nullptr;
	}

	if (dev != nullptr)
	{
		// Views first: they are descriptors INTO ReShade's CPU pool that name these resources, and
		// leaking one leaks a pool slot for the life of the process. destroy_resource_view on a
		// zero handle is not defined to be safe, so every one is guarded.
		if (st.out_srv.handle    != 0) { dev->destroy_resource_view(st.out_srv);    st.out_srv    = { 0 }; }
		if (st.proxy_srv.handle  != 0) { dev->destroy_resource_view(st.proxy_srv);  st.proxy_srv  = { 0 }; }
		if (st.proxy_uav.handle  != 0) { dev->destroy_resource_view(st.proxy_uav);  st.proxy_uav  = { 0 }; }
		if (st.orig_srv.handle   != 0) { dev->destroy_resource_view(st.orig_srv);   st.orig_srv   = { 0 }; }
		if (st.result_uav.handle != 0) { dev->destroy_resource_view(st.result_uav); st.result_uav = { 0 }; }

		if (st.out_tex.handle    != 0) { dev->destroy_resource(st.out_tex);    st.out_tex    = { 0 }; }
		if (st.proxy_tex.handle  != 0) { dev->destroy_resource(st.proxy_tex);  st.proxy_tex  = { 0 }; }
		if (st.orig_tex.handle   != 0) { dev->destroy_resource(st.orig_tex);   st.orig_tex   = { 0 }; }
		if (st.result_tex.handle != 0) { dev->destroy_resource(st.result_tex); st.result_tex = { 0 }; }
	}

	// The pristine copy is gone, so nothing may be restored from it. Dropping this is what makes a
	// resolution change safe: the armed resource is about to be a different size.
	st.pending_res = 0;
	st.pending_w = st.pending_h = 0;
	st.pending_fmt = format::unknown;
	st.codec_textures_ok = false;
	st.orig_ok = false;
	// The per-resolution allocation latches are cleared, exactly like feature_failed below.
	// codec_failed is NOT: it records that the shaders themselves could not be built, which a
	// resolution change cannot undo.
	st.codec_tex_failed = false;
	st.orig_failed = false;

	st.out_w = st.out_h = 0;
	st.out_fmt = format::unknown;
	st.neural_fmt = format::unknown;
	st.guide_w = st.guide_h = 0;
	st.need_reset = true;
	st.feature_failed = false;
	st.evaluate_count = 0;

	if (why != nullptr)
		LOGI("DLSS-NR: released the feature and the output texture (%s).", why);
}

// --------------------------------------------------------------------------------------------
// The three textures the HDR codec and the temporal-feedback fix need, all at the SAME extent as
// out_tex, all owned by this add-on.
//
// NOTHING HERE EVER CREATES A VIEW ON A RESOURCE THE GAME OWNS. That is deliberate: a cached
// D3D12 descriptor naming a resource UE later releases is a dangling read with no diagnostic, and
// creating one per frame instead leaks a slot out of ReShade's CPU descriptor pool every frame.
// Reading the original out of our own copy costs one full-res CopyTextureRegion and removes the
// whole hazard - and that copy is needed for the temporal-feedback fix anyway.
//
// A failure here is NOT fatal: the caller drops the feature that needed the texture and the pass
// runs exactly as it does today.
static void nr_ensure_aux(device *dev, nr_state &st)
{
	const uint32_t w = st.out_w, h = st.out_h;
	if (w == 0 || h == 0 || st.out_fmt == format::unknown)
		return;

	const bool want_codec = g_cfg.hdr_codec && st.codec.ok && !st.codec_failed;
	// The pristine copy is needed by the codec (as `original`) AND by the feedback fix.
	const bool want_orig  = want_codec || (g_cfg.history_restore && g_cfg.copy_back);

	// ---- the pre-denoise original -----------------------------------------------------------
	if (want_orig && !st.orig_ok && !st.orig_failed)
	{
		const resource_desc d(w, h, 1, 1, st.out_fmt, 1, memory_heap::default_,
			resource_usage::copy_source | resource_usage::copy_dest | resource_usage::shader_resource);

		// Resting state COPY_SOURCE: that is the state the feedback fix's restore reads it in at
		// the top of the next frame, so the common path costs no transition at all.
		resource t = { 0 };
		if (!dev->create_resource(d, nullptr, resource_usage::copy_source, &t) || t.handle == 0)
		{
			st.orig_failed = true;
			if (!st.logged_hist_tex_fail)
			{
				st.logged_hist_tex_fail = true;
				LOGE("DLSS-NR: create_resource failed for the %ux%u %s pre-denoise copy. The HDR "
				     "codec and the temporal-feedback fix both need it, so BOTH are off for this "
				     "resolution; the denoise still runs and is still written back, exactly as it "
				     "did before either was added.", w, h, probe::format_name(st.out_fmt));
			}
		}
		else
		{
			st.orig_tex = t;
			st.orig_ok  = true;
			LOGI("DLSS-NR: created the pre-denoise copy, %ux%u %s (copy src/dst + SRV, "
			     "ID3D12Resource=0x%llx).", w, h, probe::format_name(st.out_fmt),
			     (unsigned long long)t.handle);
		}
	}

	// ---- the codec's proxy, result and the three SRVs ----------------------------------------
	if (!want_codec || st.codec_textures_ok || st.codec_tex_failed)
		return;
	if (!st.orig_ok)
		return;   // the decode has no `original` to add onto; orig_failed already said why

	const char *stage = nullptr;

	// The proxy is DLSSNR.Color, so it has to be at the colour (output) extent: the snippet
	// validates the Color rect against the Output rect and rejects the evaluate outright when
	// their dimensions differ. FP16 is what the RenoDX deployment uses for the same surface, and
	// it comfortably holds the [0,1] sRGB-encoded values the network expects.
	const resource_desc proxy_desc(w, h, 1, 1, format::r16g16b16a16_float, 1, memory_heap::default_,
		resource_usage::unordered_access | resource_usage::shader_resource);
	const resource_desc result_desc(w, h, 1, 1, st.out_fmt, 1, memory_heap::default_,
		resource_usage::unordered_access | resource_usage::copy_source);

	const resource_view_desc proxy_view(resource_view_type::texture_2d, format::r16g16b16a16_float, 0, 1, 0, 1);
	// format_to_default_typed is a no-op for the two colour-class formats this can be, but the
	// backend asserts a non-typeless view format and this costs nothing.
	const resource_view_desc tex_view(resource_view_type::texture_2d,
		format_to_default_typed(st.out_fmt), 0, 1, 0, 1);
	// The neural target is NOT necessarily out_fmt - nr_ensure_output forces it to
	// r16g16b16a16_float whenever the codec is on, so that InProxy and InNeural are the same
	// format and the decode's bit-exact identity is reachable in hardware and not only on paper.
	// A view must name the format its resource was actually created in.
	const resource_view_desc neural_view(resource_view_type::texture_2d,
		format_to_default_typed(st.neural_fmt), 0, 1, 0, 1);

	if      (st.proxy_tex.handle == 0 && !dev->create_resource(proxy_desc, nullptr, resource_usage::unordered_access, &st.proxy_tex))
		stage = "create_resource(proxy, r16g16b16a16_float, UAV|SRV)";
	else if (st.result_tex.handle == 0 && !dev->create_resource(result_desc, nullptr, resource_usage::unordered_access, &st.result_tex))
		stage = "create_resource(result, UAV|copy source)";
	// Pass EXACTLY resource_usage::shader_resource (0xC0) / unordered_access: create_resource_view
	// switches on the whole value, and shader_resource_pixel alone falls through and returns false.
	else if (!dev->create_resource_view(st.proxy_tex,  resource_usage::shader_resource,  proxy_view, &st.proxy_srv))
		stage = "create_resource_view(proxy SRV)";
	else if (!dev->create_resource_view(st.proxy_tex,  resource_usage::unordered_access, proxy_view, &st.proxy_uav))
		stage = "create_resource_view(proxy UAV)";
	else if (!dev->create_resource_view(st.orig_tex,   resource_usage::shader_resource,  tex_view,   &st.orig_srv))
		stage = "create_resource_view(original SRV)";
	else if (!dev->create_resource_view(st.out_tex,    resource_usage::shader_resource,  neural_view, &st.out_srv))
		stage = "create_resource_view(neural SRV)";
	else if (!dev->create_resource_view(st.result_tex, resource_usage::unordered_access, tex_view,   &st.result_uav))
		stage = "create_resource_view(result UAV)";

	if (stage != nullptr)
	{
		st.codec_tex_failed = true;
		if (!st.logged_codec_tex_fail)
		{
			st.logged_codec_tex_fail = true;
			LOGE("DLSS-NR: the HDR codec's resources could not be created at %ux%u - %s failed. The "
			     "codec is OFF for this resolution and the network is fed the raw linear TAA output "
			     "instead, which is the darkening described in README gap 1. Everything else - the "
			     "evaluate, the state restore, the copy-back - is unchanged.", w, h, stage);
		}
		return;
	}

	st.codec_textures_ok = true;
	LOGI("DLSS-NR: HDR codec resources ready at %ux%u - proxy r16g16b16a16_float, neural target "
	     "%s, result %s, SRVs on the original/proxy/neural, UAVs on the proxy/result.",
	     w, h, probe::format_name(st.neural_fmt), probe::format_name(st.out_fmt));

	// The identity property is a statement about BITS, so say out loud whether the two surfaces
	// it talks about actually have the same format at this resolution. If they somehow do not,
	// the residual carries a per-pixel quantisation floor and the "exact" claim is false.
	if (st.neural_fmt != format::r16g16b16a16_float)
	{
		LOGW("DLSS-NR: the neural target is %s but the proxy is r16g16b16a16_float. InProxy and "
		     "InNeural therefore CANNOT hold identical bit patterns, so the decode's residual "
		     "carries a per-pixel quantisation floor and the identity is NOT exact. This should "
		     "be impossible - nr_ensure_output forces r16g16b16a16_float whenever the codec is "
		     "on.", probe::format_name(st.neural_fmt));
	}
}

// --------------------------------------------------------------------------------------------
// Resource + feature creation, at the resolution the game just showed us.
// --------------------------------------------------------------------------------------------
// 'fmt' is the format of the TAA pass's OWN output UAV. It is recorded as st.out_fmt, and it is
// the format of everything that can ever be the SOURCE of the copy-back.
//
// WHICH FORMAT out_tex ITSELF GETS DEPENDS ON WHETHER THE CODEC IS RUNNING.
//
// CODEC OFF: out_tex IS the copy-back source, so it must be 'fmt' and nothing else. The copy-back
// is a CopyTextureRegion, and D3D12 requires source and destination formats to be identical or
// members of the same typeless family - so a texture created r16g16b16a16_float cannot be copied
// into an r11g11b10_float destination (8-byte vs 4-byte texel; on vkd3d-proton that is a
// vkCmdCopyImage between mismatched block sizes). And r11g11b10_float is a live possibility here:
// classify_format() admits it as a colour class, nr_pick_output_uav accepts a candidate on that
// class alone, and FTAAStandaloneCS's OutComputeTex is PF_FloatR11G11B10 in several UE 4.27
// configurations. The probe measured the SRVs (t5/t6 are r16g16b16a16_float); it never measured
// the UAV, so the output format is taken from the resource, not assumed.
//
// CODEC ON: out_tex is NOT the copy-back source - result_tex is (see the copy-back below, and
// nr_ensure_aux, which creates result_tex in out_fmt). out_tex is only ever the network's target
// and the decode's InNeural. So the format constraint that forces 'fmt' does not apply to it,
// and a DIFFERENT constraint takes over: the decode's identity property is the statement that
// InProxy and InNeural hold identical bit patterns when the network returns its input unchanged,
// and two surfaces of different formats cannot. An r11g11b10_float neural target against an
// r16g16b16a16_float proxy leaves a channel-asymmetric per-pixel quantisation floor in
// (neural - proxy) that is divided by s and added to every pixel of the frame. So it is forced
// to r16g16b16a16_float, matching the proxy and matching Remix, which allocates both surfaces
// VK_FORMAT_R16G16B16A16_SFLOAT (rtx_neural_rendering.cpp:108 and :115).
//
// The predicate below is process-constant: g_cfg is loaded once in nr_init_device and there is no
// overlay or reload, and the codec pipelines are created in nr_init_device too, so st.codec.ok
// and st.codec_failed are both settled before any dispatch reaches here. It therefore cannot
// disagree with itself between the creation of out_tex and the creation of out_srv.
static bool nr_ensure_output(device *dev, nr_state &st, uint32_t w, uint32_t h, format fmt)
{
	if (st.out_tex.handle != 0)
	{
		if (st.out_w == w && st.out_h == h && st.out_fmt == fmt)
		{
			// Cheap and idempotent: it returns immediately once everything wanted exists, and it
			// is the only path that can pick up an aux texture whose first creation attempt was
			// made before the codec pipelines had been built.
			nr_ensure_aux(dev, st);
			return true;
		}
		// A resolution OR FORMAT change. Do NOT destroy from here: this runs inside a dispatch
		// callback on a command-list recording thread, and destroy_resource requires the GPU to be
		// idle first. Defer to the next present and skip the pass this frame.
		st.pending_teardown = true;
		return false;
	}

	if (fmt == format::unknown)
		return false;

	// The snippet enforces exactly one cross-resource constraint - with both rects covering their
	// whole resource, Color and Output must have EQUAL dimensions, or the evaluate is rejected
	// with "Invalid Color/Output rect configuration" - which is why this is allocated at the
	// colour extent and nothing else.
	//
	// usage must be a SUPERSET of the initial state and of every state we will ever transition
	// to: create_resource asserts exactly that, and on D3D12 unordered_access in 'usage' is what
	// sets D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS. Without it NGX rejects the evaluate with
	// FAIL_RWFlagMissing.
	//
	// shader_resource is in the usage set purely so the HDR codec's decode can READ this texture
	// (it is the network's answer, InNeural at t2). On D3D12 that flag adds nothing to
	// D3D12_RESOURCE_DESC::Flags for a colour texture - DENY_SHADER_RESOURCE is only ever set on a
	// depth-stencil resource - so it changes nothing about what NGX sees here.
	// See the header comment: 'fmt' only when out_tex is itself the copy-back source.
	const bool codec_wanted = g_cfg.hdr_codec && st.codec.ok && !st.codec_failed;
	const format neural = codec_wanted ? format::r16g16b16a16_float : fmt;

	const resource_desc desc(
		w, h, 1, 1,
		neural,
		1,
		memory_heap::default_,
		resource_usage::unordered_access | resource_usage::copy_source | resource_usage::shader_resource);

	resource out = { 0 };
	if (!dev->create_resource(desc, nullptr, resource_usage::unordered_access, &out) || out.handle == 0)
	{
		LOGE("DLSS-NR: create_resource failed for the %ux%u %s output texture. The pass stays off.",
		     w, h, probe::format_name(neural));
		return false;
	}

	st.out_tex = out;
	st.out_w = w;
	st.out_h = h;
	st.out_fmt = fmt;
	st.neural_fmt = neural;
	st.need_reset = true;

	LOGI("DLSS-NR: created the output texture, %ux%u %s, UAV + SRV + copy source "
	     "(ID3D12Resource=0x%llx). The TAA output UAV is %s. %s",
	     w, h, probe::format_name(neural), (unsigned long long)out.handle,
	     probe::format_name(fmt),
	     codec_wanted
	        ? "The codec is on, so this texture is the network's target only - the copy-back "
	          "sources result_tex - and it is forced to r16g16b16a16_float to match the proxy, "
	          "which is what makes the decode's residual bit-exact."
	        : "The codec is off, so this texture IS the copy-back source and it carries the TAA "
	          "output UAV's own format.");

	nr_ensure_aux(dev, st);
	return true;
}

// CreateFeature records real GPU work (weight upload) onto the command list it is given, so it
// runs INSIDE the save/restore window like the evaluate does. Once per resolution, not per frame.
static bool nr_ensure_feature(nr_state &st, ID3D12GraphicsCommandList *cl, uint32_t w, uint32_t h)
{
	if (st.feature != nullptr)
		return true;
	if (st.feature_failed)
		return false;   // latched for this resolution; cleared when the resolution moves
	if (st.params == nullptr || g_snippet.create_feature == nullptr)
		return false;

	ngx::parameter_block *p = st.params;

	// DLSSNR.Width / DLSSNR.Height describe the resource bound as DLSSNR.Color. This pass does
	// not upscale, so the input and output grids are the same one.
	ngx::set_u32(p, ngx::kParamWidth,       w);
	ngx::set_u32(p, ngx::kParamHeight,      h);
	// Inert in this snippet build - neither string exists in nvngx_dlssnr.dll and CreateFeature
	// reads only Width/Height. Written for parity with the working deployment; nothing is
	// predicated on them.
	ngx::set_u32(p, ngx::kParamInputWidth,  w);
	ngx::set_u32(p, ngx::kParamInputHeight, h);
	ngx::set_u32(p, ngx::kParamEnabled,     1u);
	// Only preset 1 exists in this snippet build; anything else logs "preset %d is not available
	// in this DLL build" and loads the same weights anyway.
	ngx::set_u32(p, ngx::kParamRenderPreset, ngx::kOnlyPreset);
	// Dead: three sites read it and then unconditionally store 1.0f over the result.
	ngx::set_f32(p, ngx::kParamScalingRatio, 1.0f);
	// Generic NGX. Single-GPU node mask is 1. Not proven necessary for feature 18, but it is what
	// every D3D12 NGX feature sets and it is free.
	ngx::set_u32(p, ngx::kParamCreationNodeMask,   1u);
	ngx::set_u32(p, ngx::kParamVisibilityNodeMask, 1u);
	// Release the feature's video memory when it is released. Literal int, matching the SDK's
	// own usage.
	ngx::set_i32(p, ngx::kParamFreeMemOnRelease, 1);

	void *handle = nullptr;
	const ngx::Result r = g_snippet.create_feature(cl, ngx::kFeatureDLSSNR, p, &handle);
	if (ngx::failed(r) || handle == nullptr)
	{
		st.feature_failed = true;
		if (!st.logged_create_fail)
		{
			st.logged_create_fail = true;
			nr_log_ngx(reshade::log::level::error, "CreateFeature(feature 18)", r);
			if (r == ngx::Result_FAIL_PlatformError)
				LOGE("DLSS-NR: FAIL_PlatformError from a GATED export means the snippet's caller "
				     "check rejected us. Calls must arrive from a module whose path contains "
				     "\"nvngx.dll\" - i.e. through remix_nvngx.dll, with REAL calls and no tail "
				     "jumps in its forwarders.");
			LOGE("DLSS-NR: the feature is latched off for %ux%u and will only be retried if the "
			     "resolution changes.", w, h);
		}
		return false;
	}

	st.feature = handle;
	st.need_reset = true;
	LOGI("DLSS-NR: CreateFeature(feature 18) succeeded at %ux%u, preset %u.", w, h, ngx::kOnlyPreset);
	return true;
}

// --------------------------------------------------------------------------------------------
// Resource binding. Every resource is described by ITS OWN dimensions - the snippet validates
// each rect against that resource's real size and never compares a guide rect against the colour
// rect. The four Subrect suffixes carry NO dot before "Subrect"; that is not a typo.
// --------------------------------------------------------------------------------------------
static void nr_set_resource(ngx::parameter_block *p, const ngx::resource_param_names &n,
                            ID3D12Resource *res, uint32_t w, uint32_t h)
{
	ngx::set_res(p, n.resource.c_str(), res);
	ngx::set_u32(p, n.base_x.c_str(), 0u);
	ngx::set_u32(p, n.base_y.c_str(), 0u);
	ngx::set_u32(p, n.width.c_str(),  w);
	ngx::set_u32(p, n.height.c_str(), h);
}

// The parameter block outlives every evaluate - it is allocated once and reused - so an optional
// resource that is not bound THIS frame must be cleared explicitly. Leaving the previous frame's
// pointer there both dangles and, for the control mask specifically, keeps the snippet forcing
// UseAutoMask to 0 (which kills BOTH structure strengths) long after the mask went away.
//
// The null is written through the ID3D12Resource* slot, not the void* slot, so the parameter
// map's type tag stays consistent with the bound case.
static void nr_clear_resource(ngx::parameter_block *p, const ngx::resource_param_names &n)
{
	ngx::set_res(p, n.resource.c_str(), static_cast<ID3D12Resource *>(nullptr));
	ngx::set_u32(p, n.base_x.c_str(), 0u);
	ngx::set_u32(p, n.base_y.c_str(), 0u);
	ngx::set_u32(p, n.width.c_str(),  0u);
	ngx::set_u32(p, n.height.c_str(), 0u);
}

// --------------------------------------------------------------------------------------------
// "Which UAV is the TAA output?"
//
// The probe left this open. It is answered here by RESOLUTION, then narrowed by agreement with
// the colour buffer, and never by position alone:
//
//   1. only u-registers the shader actually DECLARES are resolved at all (dcl_uav_typed census),
//      because everything else in a Tier-3 UAV range holds a stale descriptor whose resource may
//      already be destroyed;
//   2. a candidate must be a 2D texture, in a colour-class format, at exactly the colour SRV's
//      extent, and must not be one of the pass's own inputs;
//   3. the register named by uav_output in the ini (default u0, which is FTAAStandaloneCS's
//      OutComputeTex) must itself be a candidate. If it is not, the pass REFUSES to run and the
//      log names every candidate it did find, so the ini can be corrected. Picking "the only
//      other one that fits" would be a guess, and a wrong guess here means writing the denoised
//      image over an unrelated render target.
//   4. if the configured register is a candidate but other candidates exist too, the ambiguity
//      is reported once and the configured register is used - deterministic and overridable,
//      rather than silent.
// --------------------------------------------------------------------------------------------
struct uav_candidate
{
	uint32_t     reg = 0;
	nr_view_info info;
	bool         is_candidate = false;
	const char  *why_not = "";
};

static bool nr_pick_output_uav(device *dev, nr_state &st,
                               const std::vector<probe::resolved_uav> &uavs,
                               const nr_view_info &colour,
                               const resource *inputs, size_t input_count,
                               nr_view_info &out_pick, uint32_t &out_reg)
{
	std::vector<uav_candidate> list;
	list.reserve(uavs.size());

	for (const probe::resolved_uav &u : uavs)
	{
		uav_candidate c;
		c.reg = u.dx_register_index;

		if (!u.safe_to_resolve)
		{
			c.why_not = "not declared by the shader (stale descriptor, deliberately not resolved)";
			list.push_back(c);
			continue;
		}

		c.info = nr_describe(dev, u.view);
		if (!c.info.ok)
		{
			c.why_not = "not a 2D texture, or the resource could not be described";
			list.push_back(c);
			continue;
		}

		if (classify_format(c.info.fmt) != buffer_class::colour)
		{
			c.why_not = "format is not a colour class (r16g16b16a16_float / r11g11b10_float)";
			list.push_back(c);
			continue;
		}
		if (c.info.w != colour.w || c.info.h != colour.h)
		{
			c.why_not = "extent differs from the colour SRV's";
			list.push_back(c);
			continue;
		}

		bool aliases_input = false;
		for (size_t i = 0; i < input_count; ++i)
		{
			if (inputs[i].handle != 0 && inputs[i].handle == c.info.res.handle)
				aliases_input = true;
		}
		if (aliases_input)
		{
			c.why_not = "same resource as one of the pass's own SRV inputs";
			list.push_back(c);
			continue;
		}

		c.is_candidate = true;
		list.push_back(c);
	}

	uint32_t n_candidates = 0;
	const uav_candidate *chosen = nullptr;
	for (const uav_candidate &c : list)
	{
		if (!c.is_candidate)
			continue;
		n_candidates++;
		if (c.reg == g_cfg.uav_output)
			chosen = &c;
	}

	if (chosen == nullptr)
	{
		if (!st.logged_uav_reject)
		{
			st.logged_uav_reject = true;
			LOGE("DLSS-NR: the configured output register u%u is not a usable TAA output, so the "
			     "pass will NOT run. %zu UAV slots were seen at this dispatch:",
			     g_cfg.uav_output, list.size());
			for (const uav_candidate &c : list)
			{
				if (c.is_candidate)
					LOGE("  u%-3u CANDIDATE  res=0x%llx %s %ux%u", c.reg,
					     (unsigned long long)c.info.res.handle, probe::format_name(c.info.fmt),
					     c.info.w, c.info.h);
				else
					LOGE("  u%-3u rejected   %s", c.reg, c.why_not);
			}
			LOGE("DLSS-NR: set uav_output=<register> in stray_dlssnr.ini to one of the CANDIDATE "
			     "rows above. Nothing is guessed here: writing the denoised image over the wrong "
			     "render target would look like a game bug, not an add-on bug.");
		}
		return false;
	}

	if (n_candidates > 1 && !st.logged_uav_ambiguous)
	{
		st.logged_uav_ambiguous = true;
		LOGW("DLSS-NR: AMBIGUOUS output UAV - %u of the bound UAVs are colour-class textures at "
		     "the colour extent. u%u (the configured uav_output) is being used. If the denoised "
		     "image appears in the wrong place, the other candidates are:", n_candidates, g_cfg.uav_output);
		for (const uav_candidate &c : list)
		{
			if (c.is_candidate && c.reg != g_cfg.uav_output)
				LOGW("  u%-3u res=0x%llx %s %ux%u", c.reg, (unsigned long long)c.info.res.handle,
				     probe::format_name(c.info.fmt), c.info.w, c.info.h);
		}
	}

	out_pick = chosen->info;
	out_reg  = chosen->reg;
	return true;
}

// --------------------------------------------------------------------------------------------
// The pass.
//
// OWNERSHIP OF THE DISPATCH IS REPORTED THROUGH 'issued', NOT THROUGH A RETURN VALUE.
//
// on_dispatch must tell ReShade whether the game's Dispatch has already been put on the command
// list; returning false makes ReShade issue it. A return value cannot carry that safely, because a
// return value only exists on a NORMAL return: if anything unwinds after cmd->dispatch(), the
// caller's `handled = nr_try_run(...)` assignment never happens, the guard sees the initialiser
// false, and ReShade issues the TAA Dispatch A SECOND TIME - against NGX's root signature, PSO and
// heaps, with no intervening barrier, and with the state restore skipped. That is a device removal.
//
// So 'issued' is set on the line immediately after the dispatch, BEFORE anything that can throw,
// and the caller passes the same variable it returns. Everything after that point is additionally
// wrapped so the restore and the closing barrier run on the exception path too - an unrestored
// command list is fatal regardless of what on_dispatch returns.
//
// Every early return leaves 'issued' false, which leaves ReShade to issue the dispatch - i.e. a
// strict no-op.
// --------------------------------------------------------------------------------------------
static void nr_try_run(command_list *cmd, uint32_t gx, uint32_t gy, uint32_t gz, bool &issued)
{
	// One-shot, render-thread NGX bring-up. See nr_lazy_ngx_init for why it cannot live in
	// init_device. Failure latches so a broken snippet costs one attempt, not one per frame.
	if (g_nr_pending_init.load(std::memory_order_acquire))
	{
		static std::atomic<bool> s_init_running{ false };
		bool expected = false;
		if (s_init_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		{
			g_nr_pending_init.store(false, std::memory_order_release);
			if (nr_lazy_ngx_init(cmd->get_device()))
				g_nr_armed.store(true, std::memory_order_release);
		}
	}

	if (!g_nr_armed.load(std::memory_order_relaxed))
		NR_BAIL("not armed - nr_lazy_ngx_init did not succeed");

	auto *cs = probe::pd_get<probe::cmd_shadow>(cmd, probe::kCmdShadowGuid);
	if (cs == nullptr || cs->pso.handle == 0)
		NR_BAIL("no command-list shadow, or no PSO bound");

	// ---------------------------------------------------------------- is this the TAA pass?
	//
	// Identification is the probe's, unchanged: the DXBC hash is the exact identifier, and the
	// memo is refreshed once per SetPipelineState rather than once per dispatch.
	if (cs->nr_checked != cs->pso)
	{
		bool is_target = false;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			const auto it = g.pipelines.find(cs->pso.handle);
			if (it != g.pipelines.end() && it->second.has_shader)
			{
				const shader_record &sr = it->second.shader;
				// A pinned hash is exact. With shader_hash=0 the census gates stand in for it,
				// but the SRV class quorum below is what actually decides either way - the
				// measured false positive 0x901e041a7cadc9db scores confidence 150 and would
				// pass any score-based test.
				is_target = sr.is_compute && sr.dxbc_valid &&
					(g_cfg.shader_hash != 0 ? (sr.hash == g_cfg.shader_hash) : sr.passed_all_gates);
			}
		}
		cs->nr_checked   = cs->pso;
		cs->nr_is_target = is_target;
	}
	if (!cs->nr_is_target)
		NR_BAIL("this dispatch is not the target shader");

	device *const dev = cmd->get_device();
	if (dev == nullptr)
		NR_BAIL("command_list::get_device() returned null");

	auto *sh = probe::pd_get<probe::device_shadow>(dev, probe::kDeviceShadowGuid);
	auto *st = probe::pd_get<nr_state>(dev, kNrStateGuid);
	if (sh == nullptr || !sh->is_d3d12 || st == nullptr || st->params == nullptr)
		NR_BAIL("device shadow or nr_state missing (params not allocated?)");
	if (st->pending_teardown)
		NR_BAIL("pending teardown - waiting for present");

	// One device, one TAA pass, one recording thread at a time - but the lock is cheap and this
	// runs once a frame, and it is what makes the one-shot log latches sound.
	std::lock_guard<std::mutex> lock(st->mutex);

	// ---- BEGIN overlay_ui hook ----
	// THE ONE PLACE THE OVERLAY REACHES THE RENDER PATH. On this thread, under the lock this pass
	// already holds, it copies its atomics into g_cfg ONCE - which is what makes the several
	// settings that are read more than once per pass (restore_graphics_root by capture_state and
	// again by restore_state, paper_white_scale twice inside one expression, copy_back at six
	// sites) coherent for the whole pass instead of a set of independently torn reads. It also
	// turns a changed depth convention or motion scale into one DLSSNR.Reset frame, drops
	// st->pending_res when the thing that armed it was toggled, and services the overlay's
	// "Reset NR feature" button through the existing deferred-teardown path. The full argument,
	// with line references, is in the header comment of src/overlay_ui.hpp.
	//
	// A plain `return`, NOT NR_BAIL: NR_BAIL's one-shot latch would burn itself on the first
	// toggle and never speak again. Returning here leaves 'issued' false, so ReShade issues the
	// game's own Dispatch - a strict no-op.
	if (!overlay_ui::begin_pass(g_cfg, st->need_reset, st->pending_res, st->pending_teardown,
	                            st->feature_failed, st->codec.ok, st->codec_failed, st->orig_ok))
		return;
	// ---- END overlay_ui hook ----

	// ---------------------------------------------------------------- resolve the SRVs
	shader_record shader;
	{
		std::lock_guard<std::mutex> glock(g.mutex);
		const auto it = g.pipelines.find(cs->pso.handle);
		if (it == g.pipelines.end() || !it->second.has_shader)
			NR_BAIL("pipeline record has no shader");
		shader = it->second.shader;
	}

	const bool census_usable = (shader.info.max_texture_register >= 0) && shader.info.dcl_resource_index_dim_ok;
	if (!census_usable)
		NR_BAIL("DXBC census unusable (max_texture_register / dcl_resource_index_dim)");

	std::vector<probe::resolved_srv> srvs;
	probe::resolve_bound_srvs(dev, *sh, cs->cmp, shader.info.declared_srv_register_mask, probe::kMaxSrvWalk, srvs);
	if (srvs.empty())
		NR_BAIL("resolve_bound_srvs returned nothing for this dispatch");

	nr_view_info depth, velocity, colour;
	uint32_t n_colour = 0, n_depth = 0, n_velocity = 0;

	// EVERY colour-class SRV bound at this dispatch, not just the configured one. The measured TAA
	// pass binds colour at t5 AND history at t6 (both r16g16b16a16_float, both 1920x1080), and the
	// output UAV must not be a resource the pass itself READS - so the alias exclusion has to see
	// all of them, not only g_cfg.srv_colour. (This does NOT catch the history FEEDBACK described
	// in README "Known gaps": UE ping-pongs the two history buffers, so the buffer we write is by
	// construction the one NOT bound as t6 this frame. That is detected separately, below.)
	std::vector<resource> colour_srvs;
	// The t-register each of those was found at, kept index-for-index in step with colour_srvs.
	// The temporal-feedback fix uses it to NAME the register it recognised the history at, so the
	// log reports an observation rather than an assumption.
	std::vector<uint32_t> colour_srv_regs;
	// The full resolved descriptor of each, also index-for-index. KEPT, not discarded: the
	// history restore's shape check has to compare the extent and format of the resource it
	// actually MATCHED against the extent and format the pristine copy was taken at. Comparing
	// the recorded pending_* against out_* instead is a tautology - both are written from
	// taa_out, and any change to either forces a teardown that clears pending_res - so it can
	// never reject anything, and a full-subresource CopyTextureRegion between mismatched extents
	// is invalid usage rather than an error return.
	std::vector<nr_view_info> colour_srv_vis;

	for (const probe::resolved_srv &r : srvs)
	{
		if (!r.safe_to_resolve)
			continue;

		const nr_view_info vi = nr_describe(dev, r.view);
		if (!vi.ok)
			continue;

		const buffer_class bc = classify_format(vi.fmt);
		if (bc == buffer_class::colour)   { n_colour++; colour_srvs.push_back(vi.res); colour_srv_regs.push_back(r.dx_register_index); colour_srv_vis.push_back(vi); }
		if (bc == buffer_class::depth)    n_depth++;
		if (bc == buffer_class::velocity) n_velocity++;

		if (r.dx_register_index == g_cfg.srv_depth    && bc == buffer_class::depth)    depth    = vi;
		if (r.dx_register_index == g_cfg.srv_velocity && bc == buffer_class::velocity) velocity = vi;
		if (r.dx_register_index == g_cfg.srv_colour   && bc == buffer_class::colour)   colour   = vi;
	}

	// THE DISCRIMINATOR. Not the confidence score: 0x901e041a7cadc9db was MEASURED in this game
	// at confidence 150 with colour=1 depth=2 velocity=0 - a depth-consuming pass, not TAA. What
	// separates the real TAA pass from it is the set of RESOLVED SRV CLASSES, so that is what is
	// tested, on the resources actually bound at this dispatch.
	const bool quorum = (n_velocity >= 1 && n_depth >= 1 && n_colour >= 1);
	if (!quorum || !depth.ok || !velocity.ok || !colour.ok)
	{
		if (!st->logged_srv_reject)
		{
			st->logged_srv_reject = true;
			LOGE("DLSS-NR: shader 0x%016llx matched, but its bound SRVs do not describe a TAA "
			     "pass, so the pass will NOT run. Resolved classes: colour=%u depth=%u "
			     "velocity=%u (need >=1 of each). Configured registers: depth=t%u %s, "
			     "velocity=t%u %s, colour=t%u %s.",
			     (unsigned long long)shader.hash, n_colour, n_depth, n_velocity,
			     g_cfg.srv_depth,    depth.ok    ? "resolved" : "NOT RESOLVED AS DEPTH",
			     g_cfg.srv_velocity, velocity.ok ? "resolved" : "NOT RESOLVED AS VELOCITY",
			     g_cfg.srv_colour,   colour.ok   ? "resolved" : "NOT RESOLVED AS COLOUR");
			LOGE("DLSS-NR: adjust srv_depth / srv_velocity / srv_colour in stray_dlssnr.ini, or "
			     "re-run the probe to re-measure this shader.");
		}
		return;
	}

	// ---------------------------------------------------------------- resolve the output UAV
	std::vector<probe::resolved_uav> uavs;
	probe::resolve_bound_uavs(dev, *sh, cs->cmp,
		shader.info.dcl_uav_index_dim_ok ? shader.info.declared_uav_register_mask : 0ull,
		probe::kMaxUavWalk, uavs);

	std::vector<resource> inputs = colour_srvs;   // every colour-class SRV, t5 and t6 alike
	inputs.push_back(depth.res);
	inputs.push_back(velocity.res);

	nr_view_info taa_out;
	uint32_t taa_out_reg = 0;
	if (!nr_pick_output_uav(dev, *st, uavs, colour, inputs.data(), inputs.size(), taa_out, taa_out_reg))
		return;

	// ---------------------------------------------------------------- our own output texture
	NR_STAGE("about to create/validate the output texture");
	if (!nr_ensure_output(dev, *st, taa_out.w, taa_out.h, taa_out.fmt))
		return;

	// ---------------------------------------------------------------- the state restore plan
	//
	// Built BEFORE anything is issued. If it is not complete the pass does not run at all: a
	// partial restore leaves the game running against state nobody can account for, which is
	// strictly worse than not injecting.
	NR_STAGE("about to capture D3D12 state");
	probe::restore_plan plan = probe::capture_state(dev, *sh, *cs, g_cfg.restore_graphics_root);
	if (!plan.complete)
	{
		if (!st->logged_restore_reject)
		{
			st->logged_restore_reject = true;
			LOGE("DLSS-NR: the D3D12 state-restore plan is INCOMPLETE, so the pass will NOT run: "
			     "%s.", plan.incomplete_reason != nullptr ? plan.incomplete_reason : "unknown");
			LOGE("DLSS-NR: NGX rebinds the descriptor heaps, the compute root signature and the "
			     "pipeline state on the command list it is given, and UE 4.27's state cache is "
			     "dirty-flag driven, so it will not repair any of it. Running without a complete "
			     "restore corrupts every later draw in the command list.");
		}
		return;
	}

	// The one assumption the table replay rests on, checked arithmetically against the heap
	// before a single table handle is ever fed back to D3D12.
	if (st->table_identity == 0)
	{
		std::vector<probe::bound_table_info> tables;
		probe::collect_bound_tables(dev, *sh, cs->cmp, tables);
		uint64_t expected = 0, actual = 0;
	NR_STAGE("about to verify descriptor-table identity");
		st->table_identity = probe::verify_table_handle_identity(st->d3d12, tables, &expected, &actual);

		if (st->table_identity == 1 && !st->logged_table_identity)
		{
			st->logged_table_identity = true;
			LOGI("DLSS-NR: descriptor_table handle identity VERIFIED - ReShade's "
			     "descriptor_table::handle is the raw D3D12_GPU_DESCRIPTOR_HANDLE::ptr "
			     "(0x%llx == heap start + offset * increment). The root-table replay is sound.",
			     (unsigned long long)actual);
		}
		else if (st->table_identity == -1 && !st->logged_table_identity)
		{
			st->logged_table_identity = true;
			LOGE("DLSS-NR: descriptor_table handle identity MISMATCH. Expected 0x%llx from the "
			     "heap, ReShade reports 0x%llx. Replaying that value as a GPU descriptor handle "
			     "would corrupt the game, so the pass is PERMANENTLY OFF for this run. This means "
			     "the ReShade build in use does not pass D3D12_GPU_DESCRIPTOR_HANDLE through "
			     "descriptor_table unchanged - re-check against that build's "
			     "d3d12_command_list.cpp.",
			     (unsigned long long)expected, (unsigned long long)actual);
		}
	}
	if (st->table_identity != 1)
		return;   // 0 means "could not check this time"; try again next frame

	// The heaps in the plan are fed to the RAW command list, so they must be the application's own
	// ID3D12DescriptorHeap objects, not ReShade wrappers around them. The arithmetic check above
	// cannot see the difference; this one can.
	if (st->heap_identity == 0)
	{
	NR_STAGE("about to verify descriptor-heap identity");
		st->heap_identity = probe::verify_heap_is_native(st->d3d12, plan.heaps[0]);
		if (st->heap_identity == 1 && !st->logged_heap_identity)
		{
			st->logged_heap_identity = true;
			LOGI("DLSS-NR: descriptor heap identity VERIFIED - reshade::api::descriptor_heap is the "
			     "application's own ID3D12DescriptorHeap, so SetDescriptorHeaps on the raw command "
			     "list is sound.");
		}
		else if (st->heap_identity == -1 && !st->logged_heap_identity)
		{
			st->logged_heap_identity = true;
			LOGE("DLSS-NR: descriptor heap identity MISMATCH - the heap ReShade reports belongs to a "
			     "different ID3D12Device than device::get_native(), which means it is a WRAPPER, not "
			     "the application's heap. Handing it to SetDescriptorHeaps on the raw command list "
			     "would be rejected by the runtime. The pass is PERMANENTLY OFF for this run.");
		}
	}
	if (st->heap_identity != 1)
		return;

	auto *const d3d12_cmd = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
	if (d3d12_cmd == nullptr)
		return;

	if (!st->logged_taa_found)
	{
		st->logged_taa_found = true;
		LOGI("==================================================================");
		LOGI("DLSS-NR: TAA pass located and accepted.");
		LOGI("  shader   0x%016llx (compute, sm %u.%u)", (unsigned long long)shader.hash,
		     shader.sm_major, shader.sm_minor);
		LOGI("  depth    t%-3u res=0x%llx %s %ux%u", g_cfg.srv_depth,
		     (unsigned long long)depth.res.handle, probe::format_name(depth.fmt), depth.w, depth.h);
		LOGI("  velocity t%-3u res=0x%llx %s %ux%u", g_cfg.srv_velocity,
		     (unsigned long long)velocity.res.handle, probe::format_name(velocity.fmt), velocity.w, velocity.h);
		LOGI("  colour   t%-3u res=0x%llx %s %ux%u  (an INPUT to TAA; not what is denoised)",
		     g_cfg.srv_colour, (unsigned long long)colour.res.handle, probe::format_name(colour.fmt),
		     colour.w, colour.h);
		LOGI("  output   u%-3u res=0x%llx %s %ux%u  (the RESOLVED colour; this is DLSSNR.Color)",
		     taa_out_reg, (unsigned long long)taa_out.res.handle, probe::format_name(taa_out.fmt),
		     taa_out.w, taa_out.h);
		LOGI("  resolved SRV classes at this dispatch: colour=%u depth=%u velocity=%u",
		     n_colour, n_depth, n_velocity);
		LOGI("==================================================================");
	}

	// The depth resource format is the one thing NGX reads straight off the ID3D12Resource, and
	// STRAY's is r32_g8_typeless sampled through an r32_float_x8_uint SRV. D3D12 NGX has no
	// channel through which to be told the view format, so a typeless planar resource may well be
	// rejected. Say so once, up front, so a later FAIL_UnsupportedInputFormat is not a mystery.
	if (!st->logged_depth_format &&
	    (depth.fmt == format::r32_g8_typeless || depth.fmt == format::r24_g8_typeless))
	{
		st->logged_depth_format = true;
		LOGW("DLSS-NR: DLSSNR.Depth is being bound to a TYPELESS PLANAR resource (%s). NGX reads "
		     "the format from the D3D12_RESOURCE_DESC and cannot be told the view format on "
		     "D3D12, so it may reject the evaluate with FAIL_UnsupportedInputFormat / "
		     "FAIL_UnsupportedFormat. If that happens, a depth conversion pass into a dedicated "
		     "R32_FLOAT texture is required - see README \"Known gaps\".", probe::format_name(depth.fmt));
	}

	// Now that the real velocity format is known, restate gap 2 against the measured resource.
	// A normalised-integer velocity buffer is the case where the encoding is definitely wrong for
	// NGX; a float one at least *might* already be in pixels.
	if (!st->logged_mvec_format &&
	    (velocity.fmt == format::r16g16b16a16_unorm || velocity.fmt == format::r16g16_unorm ||
	     velocity.fmt == format::r8g8b8a8_unorm     || velocity.fmt == format::r16g16b16a16_snorm))
	{
		st->logged_mvec_format = true;
		LOGW("DLSS-NR: DLSSNR.MVec is bound to a NORMALISED-INTEGER buffer (%s). Values in it are "
		     "in [0,1] (or [-1,1]) and carry UE4's encoding scale and bias, not absolute pixels, "
		     "which is what the snippet expects. MVecScale %.4f/%.4f corrects the GRID only. The "
		     "denoise will run and report success while the motion guide is meaningless - see "
		     "README \"Known gaps\", gap 2.",
		     probe::format_name(velocity.fmt),
		     (velocity.w != 0 ? static_cast<float>(taa_out.w) / static_cast<float>(velocity.w) : 1.0f),
		     (velocity.h != 0 ? static_cast<float>(taa_out.h) / static_cast<float>(velocity.h) : 1.0f));
	}

	// ---------------------------------------------------------------- BREAK THE TEMPORAL FEEDBACK
	//
	// UE 4.27 TemporalAA.cpp:696 is `NewHistoryTexture[0] = Outputs.SceneColor = NewHistoryTexture[0];`
	// - ONE resource wearing two hats - and :969 queues that same texture for extraction into
	// OutputHistory->RT[0], which comes back at :857 as the next frame's HistoryBuffer[0]. So the
	// resource we denoised last frame is bound as a colour SRV at THIS dispatch. Put the
	// PRE-DENOISE image back into it first, so the game's accumulator only ever blends its own
	// un-denoised results - while everything downstream of TAA still only ever sees denoised ones.
	//
	// The history weight is 0.96 per frame (r.TemporalAACurrentFrameWeight = .04, TemporalAA.cpp
	// :46-50), so without this the steady-state operator is roughly the denoiser applied 25 times,
	// not once. That is the "builds up over seconds" in the runtime warning below.
	//
	// THE TARGET IS VERIFIED, NOT ASSUMED, and the verification is against the resource that was
	// actually MATCHED at this dispatch - not against out_*, which is written from the same
	// taa_out that pending_* is and so can only ever compare equal. The armed resource must:
	//   * resolve as a colour-class SRV at this very dispatch, AND
	//   * be bound somewhere OTHER than srv_colour - a hit there is the pass's scene-colour
	//     INPUT for this frame, and writing the previous frame's image over it would freeze the
	//     picture, AND
	//   * still carry the extent and format the pristine copy was taken at.
	// If any of those fails, nothing is restored and the pristine copy is dropped. That is always
	// safe: it costs exactly one frame of the temporal feedback this fix exists to remove, and
	// hist_dropped counts it.
	//
	// StateBefore is exact, not guessed. D3D12Resources.h DetermineResourceStates gives a
	// UAV-and-SRV, non-RTV texture ReadableState = NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE,
	// and D3D12Commands.cpp RHIEndTransitionsWithoutFencing uses Resource->GetReadableState() for
	// EVERY readable access on the graphics context. That is bit-for-bit
	// reshade::api::resource_usage::shader_resource (0xC0), so this round trip is net-zero against
	// UE's own state tracker. CopyTextureRegion and ResourceBarrier set no binding state, so this
	// is safe between capture_state and the dispatch.
	if (g_cfg.history_restore && g_cfg.copy_back && st->pending_res != 0 && st->orig_ok)
	{
		// Find the armed resource among this dispatch's colour SRVs, and keep the descriptor of
		// the entry that MATCHED - not out_*, which is a tautology against pending_*.
		//
		// A match at srv_colour is NOT accepted. That register is the TAA pass's scene-colour
		// INPUT - this frame's freshly rendered image - not the history slot. Writing last
		// frame's pre-denoise TAA output over it, which is what the copy below does, would hand
		// the game's own TAA a stale frame as its CURRENT input and freeze/ghost the picture. So
		// the scan prefers any other register, and falls back to reporting the srv_colour hit
		// only so it can be refused by name. (srv_colour is by construction bound here: the pass
		// refuses to run unless `colour` resolves there.)
		int32_t found_reg = -1;
		nr_view_info found_vi;
		bool only_at_srv_colour = false;
		for (size_t i = 0; i < colour_srvs.size() && i < colour_srv_regs.size() &&
		                   i < colour_srv_vis.size(); ++i)
		{
			if (colour_srvs[i].handle != st->pending_res)
				continue;
			if (colour_srv_regs[i] == g_cfg.srv_colour)
			{
				only_at_srv_colour = true;   // remember it, but keep looking for a better slot
				continue;
			}
			found_reg = (int32_t)colour_srv_regs[i];
			found_vi  = colour_srv_vis[i];
			only_at_srv_colour = false;
			break;
		}

		// THE REAL SHAPE CHECK: the matched resource must still be exactly what the pristine copy
		// was taken from. pending_res is a raw ID3D12Resource address held across a frame, and
		// UE 4.27's render-target pool can free that element and hand the address back for a
		// differently sized colour texture - which the TAA pass does bind at more than one extent
		// whenever screen percentage is below 100.
		const bool shape_ok = (found_reg >= 0 &&
		                       found_vi.ok &&
		                       found_vi.w   == st->pending_w &&
		                       found_vi.h   == st->pending_h &&
		                       found_vi.fmt == st->pending_fmt);

		if (found_reg >= 0 && shape_ok)
		{
			// ONLY the history resource is transitioned. orig_tex already rests in COPY_SOURCE,
			// and a transition barrier whose StateBefore equals its StateAfter is an error in
			// D3D12, not a no-op - ReShade's barrier() turns a matching pair into a real
			// TRANSITION unless both states are unordered_access.
			const resource hist = { st->pending_res };

			cmd->barrier(hist, resource_usage::shader_resource, resource_usage::copy_dest);
			cmd->copy_texture_region(st->orig_tex, 0, nullptr, hist, 0, nullptr, filter_mode::min_mag_mip_point);
			cmd->barrier(hist, resource_usage::copy_dest, resource_usage::shader_resource);

			st->hist_restored.fetch_add(1, std::memory_order_relaxed);
			if (!st->logged_hist_active)
			{
				st->logged_hist_active = true;
				LOGI("DLSS-NR: HISTORY RESTORE ACTIVE - the pre-denoise TAA output has been written "
				     "back over res=0x%llx, which is resolved as a colour SRV at t%d on this very "
				     "dispatch, i.e. it IS this frame's HistoryBuffer[0]. UE 4.27's temporal "
				     "accumulator no longer sees a denoised frame; the feedback loop of README gap 5 "
				     "is BROKEN. Set history_restore=0 to A/B against the old behaviour. This "
				     "message is printed once.", (unsigned long long)st->pending_res, found_reg);
			}
		}
		else
		{
			st->hist_dropped.fetch_add(1, std::memory_order_relaxed);

			// The srv_colour case is REFUSED, not merely unmatched, and it is worth its own
			// message: it means the armed resource came back as this frame's scene-colour input.
			if (only_at_srv_colour && !st->logged_hist_odd_reg)
			{
				st->logged_hist_odd_reg = true;
				LOGW("DLSS-NR: HISTORY RESTORE REFUSED - the armed resource (res=0x%llx) is bound "
				     "at t%u at this dispatch, and t%u is the register configured as srv_colour, "
				     "i.e. the TAA pass's scene-colour INPUT for THIS frame, not the history slot "
				     "the UE 4.27 model predicts. Restoring there would overwrite this frame's "
				     "freshly rendered scene colour with the previous frame's image and the "
				     "game's own TAA would then blend that as its current input - a frozen, "
				     "ghosted frame. The pristine copy has been discarded instead, so one frame "
				     "of temporal feedback has occurred and nothing has been corrupted. If this "
				     "repeats, the UE 4.27 ping-pong model behind history_restore does not hold "
				     "for this build: check srv_colour in stray_dlssnr.ini, or set "
				     "history_restore=0. This message is printed once.",
				     (unsigned long long)st->pending_res, g_cfg.srv_colour, g_cfg.srv_colour);
			}
			else if (!st->logged_hist_dropped)
			{
				st->logged_hist_dropped = true;
				LOGW("DLSS-NR: HISTORY RESTORE SKIPPED - the resource denoised on an earlier frame "
				     "(res=0x%llx) is %s at this dispatch, so it is not this frame's history and the "
				     "pristine copy has been discarded. One frame of temporal feedback has occurred. "
				     "The periodic census carries the running counts. This message is printed once.",
				     (unsigned long long)st->pending_res,
				     found_reg < 0
				        ? "not resolved as a usable colour SRV"
				        : "bound but has a different extent/format than the copy was taken at");
			}
		}

		// One-shot either way: stale content must never be restored twice.
		st->pending_res = 0;
	}

	// ================================================================ FROM HERE WE OWN THE DISPATCH
	// Nothing below may return without leaving 'issued' true.

	// 1. The game's TAA, unchanged, at exactly the point it would have run. command_list::dispatch
	//    reaches ID3D12GraphicsCommandList::Dispatch directly and does NOT re-enter this event.
	NR_STAGE("about to issue the game dispatch");
	cmd->dispatch(gx, gy, gz);

	// OWNERSHIP IS TAKEN HERE, on the very next line, before anything that can allocate. See the
	// header comment on this function: if this were inferred from a normal return instead, any
	// throw below would make on_dispatch report false and ReShade would issue the Dispatch again.
	issued = true;

	// 2. The TAA output stops being a UAV and becomes something NGX can sample. This transition
	//    is itself the write-completion barrier; no separate UAV barrier is needed.
	//
	//    The game's depth and velocity SRVs are deliberately NOT transitioned: they are bound as
	//    SRVs to a compute shader right now, so they already include NON_PIXEL_SHADER_RESOURCE,
	//    and a transition whose StateBefore we cannot know exactly is a worse hazard than none.
	cmd->barrier(taa_out.res, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel);

	bool evaluated = false;

	// ---- HDR codec / feedback-fix bookkeeping for THIS dispatch -------------------------------
	// The codec runs only when everything it needs exists. Every one of these is a normal false,
	// never an error: with any of them off the pass behaves exactly as it did before the codec was
	// added, and the reason was logged once when it was decided.
	const bool want_codec = g_cfg.hdr_codec && !st->codec_failed && st->codec.ok &&
	                        st->codec_textures_ok && st->orig_ok;
	// The pristine copy is needed by the codec (it is the decode's `original`) AND by the
	// temporal-feedback fix (it is what gets written back over the history).
	const bool want_orig  = st->orig_ok && (want_codec || (g_cfg.history_restore && g_cfg.copy_back));

	// ONE value, computed ONCE on the CPU, written into BOTH root-constant blocks. A disagreement
	// between the encode's scale and the decode's is a correctness failure, not a tuning
	// difference: the decode subtracts a proxy that was built with a different s.
	//
	// Remix's own semantics (rtx_neural_rendering.cpp:473, with auto-exposure tracking off):
	// s = staticExposure / max(paperWhiteScale, 0.01) and the shaders do sceneLinear * s. So
	// paper_white_scale is a DIVISOR - raising it darkens the proxy. At the default of 1.0 this is
	// exactly 1.0 either way. The shaders clamp it again to [1e-6, 1e6].
	const float proxy_scale = 1.0f / (g_cfg.paper_white_scale > 0.01f ? g_cfg.paper_white_scale : 0.01f);
	const float transfer_strength = (g_cfg.transfer_strength < 0.0f) ? 0.0f
		: (g_cfg.transfer_strength > 1.0f ? 1.0f : g_cfg.transfer_strength);
	const float color_strength    = (g_cfg.color_strength < 0.0f) ? 0.0f
		: (g_cfg.color_strength > 1.0f ? 1.0f : g_cfg.color_strength);

	// Resources this pass moves OUT of their resting state inside the fenced window below. They
	// are put back unconditionally after the fence, so an escape cannot leave D3D12's view of a
	// resource disagreeing with ours and poison the next frame's barriers.
	bool orig_in_srv = false, proxy_in_srv = false, out_in_srv = false;
	// True once orig_tex actually holds THIS frame's pre-denoise TAA output. The feedback fix may
	// only arm on that, never on the intent to take the copy.
	bool orig_saved = false;
	// True once the proxy actually holds this frame's encode, i.e. the network is being shown the
	// proxy rather than the raw TAA output.
	bool codec_encoded = false;
	// True once the decode has actually written result_tex, i.e. the copy-back must take its
	// source from there rather than straight from the network's output.
	bool codec_used = false;

	// Published for the periodic census, which cannot take this state's mutex (see hist_restored).
	st->census_codec_on.store(want_codec, std::memory_order_relaxed);
	st->census_orig_on.store(st->orig_ok, std::memory_order_relaxed);

	if (!want_codec && g_cfg.hdr_codec && !st->logged_codec_off)
	{
		st->logged_codec_off = true;
		LOGW("DLSS-NR: hdr_codec=1 but the codec is NOT running (%s). The network is being fed the "
		     "raw linear TAA output, which is out-of-distribution for a display-referred network - "
		     "README gap 1, the \"a bit dark\" symptom. Everything else is unaffected. This message "
		     "is printed once.",
		     st->codec_failed ? "its shaders could not be compiled or its pipelines created"
		                      : (!st->orig_ok ? "the pre-denoise copy could not be allocated"
		                                      : "its textures could not be allocated"));
	}

	// THE THROWING WINDOW, FENCED OFF.
	//
	// Everything in here allocates: ngx::resource_param_names builds five std::strings on first
	// use, and every ngx::set_* bottoms out in an unordered_map insert keyed by a std::string.
	// (Both are now noexcept at the ngx_interop.hpp end, so this is belt and braces - but the
	// snippet's own CreateFeature/EvaluateFeature run in here too, and they are not our code.)
	// An escape past this point would skip probe::restore_state and the closing barrier below,
	// which is a corrupt command list no matter what on_dispatch returns.
	try
	{
	// ---- stage 1 of 3: the PRE-DENOISE ORIGINAL ------------------------------------------------
	// Taken before anything of ours writes anywhere. This is O_N: the decode's `original`, and the
	// pristine image the next frame's history restore puts back. taa_out is momentarily returned to
	// COPY_SOURCE and then to the SRV state NGX wants, which costs two extra transitions and leaves
	// the measured barrier above untouched.
	if (want_orig)
	{
		const resource       pair[2] = { taa_out.res, st->orig_tex };
		const resource_usage a[2]    = { resource_usage::shader_resource_non_pixel, resource_usage::copy_source };
		const resource_usage b[2]    = { resource_usage::copy_source,               resource_usage::copy_dest };
		const resource_usage c[2]    = { resource_usage::shader_resource_non_pixel,
		                                 want_codec ? resource_usage::shader_resource_non_pixel
		                                            : resource_usage::copy_source };

		cmd->barrier(2, pair, a, b);
		cmd->copy_texture_region(taa_out.res, 0, nullptr, st->orig_tex, 0, nullptr, filter_mode::min_mag_mip_point);
		cmd->barrier(2, pair, b, c);

		orig_in_srv = want_codec;
		orig_saved  = true;
	}

	// ---- stage 1 of 3 (cont.): the ENCODE -------------------------------------------------------
	//   proxy = SrgbEncode(SoftClip(original * s))
	// The network is a DISPLAY-REFERRED image network; this is what makes its input in-distribution.
	if (want_codec)
	{
		// THE CACHE SYNC, AND IT IS LOAD-BEARING.
		//
		// ReShade's command_list_impl caches _current_descriptor_heaps / _current_root_signature and
		// SKIPS a redundant SetDescriptorHeaps or SetComputeRootSignature. That cache is only written
		// when the APPLICATION goes through ReShade's wrapper - and NGX writes the RAW command list,
		// which ReShade never sees. So across an evaluate the cache goes stale, and the next
		// push_descriptors would issue SetComputeRootDescriptorTable with a GPU handle living in a
		// heap that is not bound: undefined behaviour, or a device removal.
		//
		// bind_descriptor_tables with count == 0 is ReShade's own escape hatch for exactly this:
		// d3d12_impl_command_list.cpp:538 and :549 both special-case `|| count == 0` to FORCE the
		// two calls, which re-issues them on the real list and leaves the cache equal to reality.
		cmd->bind_descriptor_tables(shader_stage::all_compute, st->codec.encode_layout, 0, 0, nullptr);
		cmd->bind_pipeline(pipeline_stage::all_compute, st->codec.encode_pso);

		descriptor_table_update srv_up = {};
		srv_up.binding = 0; srv_up.array_offset = 0; srv_up.count = 1;
		srv_up.type = descriptor_type::shader_resource_view;
		srv_up.descriptors = &st->orig_srv;
		cmd->push_descriptors(shader_stage::compute, st->codec.encode_layout, hdr_codec::kParamSrvTable, srv_up);

		descriptor_table_update uav_up = {};
		uav_up.binding = 0; uav_up.array_offset = 0; uav_up.count = 1;
		uav_up.type = descriptor_type::unordered_access_view;
		uav_up.descriptors = &st->proxy_uav;
		cmd->push_descriptors(shader_stage::compute, st->codec.encode_layout, hdr_codec::kParamUavTable, uav_up);

		hdr_codec::encode_args ea;
		ea.width = st->out_w; ea.height = st->out_h; ea.proxy_scale = proxy_scale; ea.pad0 = 0;
		cmd->push_constants(shader_stage::compute, st->codec.encode_layout, hdr_codec::kParamConstants,
		                    0, hdr_codec::kEncodeConstantCount, &ea);

		cmd->dispatch(hdr_codec::group_count(st->out_w), hdr_codec::group_count(st->out_h), 1);

		// The encode's write has to be visible to the snippet. In Remix this is the barrier set
		// flushed immediately before the evaluate (rtx_neural_rendering.cpp:355-358); here the
		// UNORDERED_ACCESS -> NON_PIXEL_SHADER_RESOURCE transition IS the write-completion barrier,
		// and it also puts the proxy in the state NGX reads Color in.
		cmd->barrier(st->proxy_tex, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel);
		proxy_in_srv  = true;
		codec_encoded = true;
	}

	// ---- stage 2 of 3: the NGX evaluate ---------------------------------------------------------
	if (nr_ensure_feature(*st, d3d12_cmd, st->out_w, st->out_h))
	{
		ngx::parameter_block *p = st->params;

		// Process-lifetime, not stack buffers: Set takes the name as a bare const char* and
		// nothing in the ABI promises the callee copies it before returning.
		static const ngx::resource_param_names s_colour(ngx::kParamColor);
		static const ngx::resource_param_names s_depth(ngx::kParamDepth);
		static const ngx::resource_param_names s_mvec(ngx::kParamMVec);
		static const ngx::resource_param_names s_output(ngx::kParamOutput);
		static const ngx::resource_param_names s_mask(ngx::kParamControlMask);

		// The snippet is fed the PROXY, not the raw TAA output: it is a display-referred image
		// network and the TAA output is unbounded linear radiance at this point in the frame.
		// (rtx_neural_rendering.cpp:289-292 makes exactly this substitution.) The proxy is at the
		// colour extent, so the Color/Output rect equality the snippet enforces still holds.
		auto *const colour_res  = reinterpret_cast<ID3D12Resource *>(
			codec_encoded ? st->proxy_tex.handle : taa_out.res.handle);
		auto *const depth_res   = reinterpret_cast<ID3D12Resource *>(depth.res.handle);
		auto *const mvec_res    = reinterpret_cast<ID3D12Resource *>(velocity.res.handle);
		auto *const output_res  = reinterpret_cast<ID3D12Resource *>(st->out_tex.handle);

		nr_set_resource(p, s_colour, colour_res, taa_out.w, taa_out.h);
		nr_set_resource(p, s_depth,  depth_res,  depth.w,   depth.h);
		nr_set_resource(p, s_mvec,   mvec_res,   velocity.w, velocity.h);
		nr_set_resource(p, s_output, output_res, st->out_w, st->out_h);
		// Written EVERY frame even though this add-on never binds one - see nr_clear_resource.
		nr_clear_resource(p, s_mask);

		// The guide grid moved under a history accumulated against the old one. Nothing else
		// notices, so force one reset frame.
		if (st->guide_w != velocity.w || st->guide_h != velocity.h)
		{
			if (st->guide_w != 0 || st->guide_h != 0)
				st->need_reset = true;   // a first frame is initialisation, not a reset
			st->guide_w = velocity.w;
			st->guide_h = velocity.h;
		}

		// The motion vectors live on the guide grid; the snippet works on the colour grid. This
		// is the ratio between the two. It CANNOT correct UE4's velocity ENCODING - see the
		// README - only the grid.
		const float scale_x = (g_cfg.mvec_scale_x != 0.0f) ? g_cfg.mvec_scale_x
			: (velocity.w != 0 ? static_cast<float>(taa_out.w) / static_cast<float>(velocity.w) : 1.0f);
		const float scale_y = (g_cfg.mvec_scale_y != 0.0f) ? g_cfg.mvec_scale_y
			: (velocity.h != 0 ? static_cast<float>(taa_out.h) / static_cast<float>(velocity.h) : 1.0f);

		ngx::set_u32(p, ngx::kParamEnabled,       1u);
		ngx::set_u32(p, ngx::kParamReset,         st->need_reset ? 1u : 0u);
		ngx::set_u32(p, ngx::kParamDepthInverted, g_cfg.depth_inverted ? 1u : 0u);
		ngx::set_f32(p, ngx::kParamMVecScaleX,    scale_x);
		ngx::set_f32(p, ngx::kParamMVecScaleY,    scale_y);
		// Gates BOTH structure strengths. Binding a ControlMask would force it to 0 inside the
		// snippet; this add-on binds none, so the two never conflict.
		ngx::set_u32(p, ngx::kParamUseAutoMask,   g_cfg.use_auto_mask ? 1u : 0u);

		ngx::set_f32(p, ngx::kParamIntensity,              g_cfg.intensity);
		ngx::set_f32(p, ngx::kParamLocalToneStrength,      g_cfg.local_tone_strength);
		ngx::set_f32(p, ngx::kParamLocalStructureStrength, g_cfg.local_structure_strength);
		// Negative means "inherit LocalStructureStrength". 0.0 is NOT neutral.
		ngx::set_f32(p, ngx::kParamSkinStructureStrength,  g_cfg.skin_structure_strength);
		ngx::set_u32(p, ngx::kParamStyle,                  g_cfg.style);

		const ngx::Result r = g_snippet.evaluate_feature(d3d12_cmd, st->feature, p, nullptr);

		// ---- BEGIN overlay_ui hook ----
		// Everything the overlay's status block needs, published as relaxed atomics that flow ONE way
		// - render thread to overlay - exactly as st->hist_restored and census_codec_on already do.
		// The overlay keeps its own evaluate counter rather than reading st->evaluate_count, which is
		// a plain uint64 under st->mutex. The timestamp taken inside is the point of the exercise: a
		// cumulative count cannot tell "14203 and climbing" from "14203 and stopped four minutes ago",
		// which is exactly the confusion that cost a whole play session.
		overlay_ui::publish_evaluate(
			static_cast<uint32_t>(r), ngx::result_to_string(r), !ngx::failed(r),
			st->out_w, st->out_h, probe::format_name(st->out_fmt), probe::format_name(st->neural_fmt),
			velocity.w, velocity.h, scale_x, scale_y, codec_encoded,
			st->hist_restored.load(std::memory_order_relaxed),
			st->hist_dropped.load(std::memory_order_relaxed));
		// ---- END overlay_ui hook ----
		if (ngx::failed(r))
		{
			if (!st->logged_eval_fail)
			{
				st->logged_eval_fail = true;
				nr_log_ngx(reshade::log::level::error, "EvaluateFeature", r);
				LOGE("DLSS-NR: the game's TAA still ran and the frame is unchanged; only the "
				     "denoise was skipped. This message is printed once.");
			}
		}
		else
		{
			evaluated = true;
			st->need_reset = false;
			st->evaluate_count++;

			// Without this the only positive evidence that the feature ran is the ABSENCE of an
			// error, and "running correctly" then looks exactly like "silently doing nothing".
			if (st->evaluate_count == 1 || st->evaluate_count == 100)
			{
				LOGI("DLSS-NR: evaluate #%llu OK. colour/output %ux%u, depth %ux%u (%s), "
				     "mvec %ux%u (%s), MVecScale %.4f/%.4f, DepthInverted=%d, UseAutoMask=%d, "
				     "Intensity=%.3f LocalTone=%.3f LocalStructure=%.3f SkinStructure=%.3f "
				     "Style=%u, copy_back=%d, hdr_codec=%d, history_restore=%d.",
				     (unsigned long long)st->evaluate_count, taa_out.w, taa_out.h,
				     depth.w, depth.h, probe::format_name(depth.fmt),
				     velocity.w, velocity.h, probe::format_name(velocity.fmt),
				     scale_x, scale_y, (int)g_cfg.depth_inverted, (int)g_cfg.use_auto_mask,
				     g_cfg.intensity, g_cfg.local_tone_strength, g_cfg.local_structure_strength,
				     g_cfg.skin_structure_strength, g_cfg.style, (int)g_cfg.copy_back,
				     (int)codec_encoded, (int)(g_cfg.history_restore && g_cfg.copy_back));
			}

			// THE IDENTITY PROPERTY, stated on the first evaluate that actually ran through the
			// codec. It is not a hope: it is algebra, and it is written out in full in the header
			// comment of hdr_codec.hpp.
			if (codec_encoded && !st->logged_identity)
			{
				st->logged_identity = true;
				LOGI("DLSS-NR: HDR CODEC ACTIVE. DLSSNR.Color is the display-referred PROXY "
				     "(res=0x%llx, r16g16b16a16_float), built as proxy = "
				     "SrgbEncode(SoftClip(original * s)) with s = 1/max(paper_white_scale, 0.01) = "
				     "%.6f from paper_white_scale=%.4f. The network's answer is carried back onto "
				     "the UNTOUCHED original as an additive residual, result = original + "
				     "(SrgbDecode(neural) - SrgbDecode(proxy)) / s, with alpha taken from the "
				     "ORIGINAL and never from the network.",
				     (unsigned long long)st->proxy_tex.handle, proxy_scale, g_cfg.paper_white_scale);
				LOGI("DLSS-NR: IDENTITY IS EXACT, algebraically. If the network returns its input "
				     "unchanged, InProxy (%s) and InNeural (%s) hold identical bit patterns, so "
				     "SrgbDecode(x) - SrgbDecode(x) is exactly +0.0, the residual is +0.0, "
				     "max(o + 0.0, 0) == o for o >= 0, the luminance ratio is L/L == 1.0 and every "
				     "lerp is lerp(a, a, t) == a. The pixel comes back BIT FOR BIT, for every value "
				     "of s, transfer_strength and color_strength. The two formats named here must "
				     "MATCH for that to hold in hardware rather than only on paper.",
				     probe::format_name(format::r16g16b16a16_float),
				     probe::format_name(st->neural_fmt));
				LOGI("DLSS-NR: the cheapest on-hardware check of the whole path is "
				     "transfer_strength=0, which is an exact bypass of the DENOISE (not of the "
				     "codec - the encode, the evaluate and the decode all still run). That run "
				     "must be PIXEL-IDENTICAL to a run with copy_back=0, or with the add-on "
				     "unloaded. It is NOT identical to hdr_codec=0: that is a different image "
				     "entirely - the raw linear TAA output bound as DLSSNR.Color and the "
				     "network's raw display-referred answer copied straight back, i.e. the "
				     "darkening this codec exists to fix.");
				LOGW("DLSS-NR: paper_white_scale=%.4f is UNCALIBRATED for STRAY - Remix's value is "
				     "calibrated against its own auto-exposure and ours is a plain constant. It "
				     "needs tuning on hardware: RAISE it if the image looks blown out (highlights "
				     "crushed into the soft-clip shoulder), LOWER it if the image looks black.",
				     g_cfg.paper_white_scale);
			}
		}
	}

	// ---- stage 3 of 3: the DECODE ---------------------------------------------------------------
	//   result = original + (SrgbDecode(neural) - SrgbDecode(proxy)) / s
	// Carries the network's change back onto the untouched HDR original. This REPLACES what would
	// otherwise be a straight RGBA copy of the neural target over the frame - that copy both
	// discarded the HDR range (the network's answer is display-referred) and overwrote the alpha
	// channel. The existing copy-back below is kept, but its SOURCE becomes result_tex.
	//
	// Gated on 'evaluated': if the evaluate did not run, out_tex holds whatever the last successful
	// one left there, and adding that as a residual would be worse than doing nothing.
	if (codec_encoded && evaluated)
	{
		// NGX wrote out_tex OUTSIDE anything that tracks it, so nothing knows the decode's read of
		// it has to wait. This transition is that dependency, and it is also what makes the
		// texture readable as an SRV. (rtx_neural_rendering.cpp:408-441 records the same two
		// hazards in Vulkan terms.)
		cmd->barrier(st->out_tex, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel);
		out_in_srv = true;

		// The cache sync again, and this is the call whose absence is a device removal: NGX has
		// just rebound the descriptor heaps and the compute root signature on the RAW list, and
		// ReShade's cache still names ours from the encode above.
		cmd->bind_descriptor_tables(shader_stage::all_compute, st->codec.decode_layout, 0, 0, nullptr);
		cmd->bind_pipeline(pipeline_stage::all_compute, st->codec.decode_pso);

		// t0 original, t1 proxy, t2 neural - one contiguous table, in declaration order.
		const resource_view decode_srvs[3] = { st->orig_srv, st->proxy_srv, st->out_srv };
		descriptor_table_update srv_up = {};
		srv_up.binding = 0; srv_up.array_offset = 0; srv_up.count = 3;
		srv_up.type = descriptor_type::shader_resource_view;
		srv_up.descriptors = decode_srvs;
		cmd->push_descriptors(shader_stage::compute, st->codec.decode_layout, hdr_codec::kParamSrvTable, srv_up);

		descriptor_table_update uav_up = {};
		uav_up.binding = 0; uav_up.array_offset = 0; uav_up.count = 1;
		uav_up.type = descriptor_type::unordered_access_view;
		uav_up.descriptors = &st->result_uav;
		cmd->push_descriptors(shader_stage::compute, st->codec.decode_layout, hdr_codec::kParamUavTable, uav_up);

		hdr_codec::decode_args da;
		da.width = st->out_w; da.height = st->out_h;
		// The IDENTICAL scale the encode used, from the same CPU float in the same frame.
		da.proxy_scale = proxy_scale;
		// Clamped on the CPU, exactly as Remix does (rtx_neural_rendering.cpp:525-526); the shader
		// does not re-clamp them.
		da.transfer_strength = transfer_strength;
		da.color_strength    = color_strength;
		da.pad0 = da.pad1 = da.pad2 = 0;
		cmd->push_constants(shader_stage::compute, st->codec.decode_layout, hdr_codec::kParamConstants,
		                    0, hdr_codec::kDecodeConstantCount, &da);

		cmd->dispatch(hdr_codec::group_count(st->out_w), hdr_codec::group_count(st->out_h), 1);

		codec_used = true;
	}
	}   // end of the throwing window. (Its body is deliberately NOT re-indented under the try:
	    // the try/catch is a fence added around existing code, and re-flowing 100 lines would
	    // bury the one change that matters in whitespace.)
	catch (const std::exception &e)
	{
		evaluated = false;
		if (!st->logged_owned_throw)
		{
			st->logged_owned_throw = true;
			LOGE("DLSS-NR: exception AFTER the game's Dispatch was issued: %s. The dispatch is not "
			     "re-issued, the state restore below still runs, and only the denoise is lost. "
			     "This message is printed once.", e.what());
		}
	}
	catch (...)
	{
		evaluated = false;
		if (!st->logged_owned_throw)
		{
			st->logged_owned_throw = true;
			LOGE("DLSS-NR: unknown exception AFTER the game's Dispatch was issued. The dispatch is "
			     "not re-issued, the state restore below still runs, and only the denoise is lost. "
			     "This message is printed once.");
		}
	}

	// 2b. Return every resource this pass moved out of its resting state, WHATEVER happened above
	//     - including the exception path. Barriers set no binding state, so their position either
	//     side of the restore is free; doing it here means a throw cannot leave D3D12's idea of a
	//     resource disagreeing with the state next frame's barriers will name as StateBefore.
	if (out_in_srv)
		cmd->barrier(st->out_tex, resource_usage::shader_resource_non_pixel, resource_usage::unordered_access);
	if (proxy_in_srv)
		cmd->barrier(st->proxy_tex, resource_usage::shader_resource_non_pixel, resource_usage::unordered_access);
	if (orig_in_srv)
		cmd->barrier(st->orig_tex, resource_usage::shader_resource_non_pixel, resource_usage::copy_source);

	// 2c. THE LAST CACHE SYNC, and only when we actually used push_descriptors. It forces
	//     SetDescriptorHeaps and SetComputeRootSignature onto the real list one more time, and -
	//     via the _previous_descriptor_heaps path at d3d12_impl_command_list.cpp:528-535 - leaves
	//     ReShade's cache naming the APPLICATION's heaps, which is what restore_state is about to
	//     put back. Without it the cache would end this window claiming ReShade's transient heap
	//     while the raw list holds UE's, and the next push_descriptors on this command list -
	//     ours next frame, or any other add-on's - would skip a SetDescriptorHeaps it needed.
	//
	//     If the codec never ran we never touched that cache, so there is nothing to re-sync and
	//     issuing this would be pure risk.
	if (codec_encoded)
		cmd->bind_descriptor_tables(shader_stage::all_compute, st->codec.decode_layout, 0, 0, nullptr);

	// 3. Put the command list back the way NGX found it. Unconditional: CreateFeature clobbers
	//    state too, so this has to run even when the evaluate itself was skipped - or threw.
	NR_STAGE("about to restore D3D12 state");
	probe::restore_state(d3d12_cmd, plan, g_cfg.restore_graphics_root);

	// 4. Carry the result back over the game's TAA output, and return every resource to the state
	//    the game's own barrier tracking believes it is in.
	//
	//    FORMAT. This tests the format of the resource that is ACTUALLY COPIED, which is not the
	//    same resource on the two paths:
	//      codec on  -> result_tex, created in st->out_fmt      (nr_ensure_aux)
	//      codec off -> out_tex,    created in st->neural_fmt   (nr_ensure_output)
	//    and with the codec on neural_fmt is forced to r16g16b16a16_float, which need NOT equal
	//    taa_out.fmt. Testing out_fmt on both paths would therefore wave through a copy out of an
	//    FP16 out_tex into an r11g11b10_float frame on the codec-on-but-not-used path. Both
	//    formats come from taa_out.fmt in the cases that matter, so this normally holds by
	//    construction - but it is checked because the failure mode of getting it wrong is a
	//    vkCmdCopyImage between mismatched block sizes, not an error return.
	const format src_fmt = codec_used ? st->out_fmt : st->neural_fmt;

	if (evaluated && g_cfg.copy_back && src_fmt != taa_out.fmt && !st->logged_copy_fmt)
	{
		// A SILENT skip is exactly the failure this codebase refuses to ship, so it is named.
		st->logged_copy_fmt = true;
		LOGE("DLSS-NR: the copy-back was SKIPPED - the source (%s, the %s) is %s but the TAA "
		     "output is %s, and CopyTextureRegion requires identical or same-family formats. The "
		     "denoise ran and the frame is unchanged.",
		     codec_used ? "result_tex" : "out_tex",
		     codec_used ? "decode's output" : "network's raw output",
		     probe::format_name(src_fmt), probe::format_name(taa_out.fmt));
		if (!codec_used && st->neural_fmt != st->out_fmt)
		{
			LOGE("DLSS-NR: the cause is that out_tex was created r16g16b16a16_float for the codec "
			     "(the proxy must match the neural target) but the codec is not running at this "
			     "dispatch, so out_tex became the copy source. Set hdr_codec=0 to get a copy-back "
			     "in the TAA output's own format; the earlier one-shot warning says why the codec "
			     "is off.");
		}
	}
	if (evaluated && g_cfg.copy_back && src_fmt == taa_out.fmt)
	{
		// THE FEEDBACK LOOP. In UE 4.27 AddTemporalAAPass extracts the compute pass's output into
		// OutputHistory->RT[0], so the resource we are about to overwrite IS next frame's TAA
		// history. The denoised image therefore re-enters both the game's temporal accumulator and
		// the snippet's own history. The alias check in nr_pick_output_uav cannot catch it: UE
		// ping-pongs the pair, so this frame's output is by construction NOT the buffer bound as
		// history this frame. What it IS, is a buffer we wrote on an earlier frame - which is what
		// this looks for. See README "Known gaps".
		if (!st->logged_feedback_loop)
		{
			for (uint64_t h : st->copied_into)
			{
				if (h != 0 && h == taa_out.res.handle)
				{
					st->logged_feedback_loop = true;
					LOGW("DLSS-NR: TEMPORAL FEEDBACK - the TAA output UAV at this dispatch "
					     "(res=0x%llx) is a resource this pass already wrote a denoised image into "
					     "on an earlier frame. UE 4.27 extracts the TAA compute output as the next "
					     "frame's history, so the denoise is compounding: its own output is being "
					     "fed back through the game's temporal accumulator AND through the "
					     "snippet's history. Expect over-smoothing and ghosting that builds up "
					     "over seconds. %s See README \"Known gaps\". This message "
					     "is printed once.", (unsigned long long)taa_out.res.handle,
					     (g_cfg.history_restore && st->orig_ok)
					        ? "history_restore=1 is MITIGATING this: the ping-pong the detector "
					          "reports is real, but the pre-denoise image is written back over the "
					          "history before the game reads it, so the loop is broken - watch the "
					          "periodic 'history restore' census line for applied/dropped counts."
					        : "Set history_restore=1 to break the loop, or copy_back=0 to run the "
					          "whole path without writing the result back.");
					break;
				}
			}
		}
		st->copied_into[st->copied_into_next] = taa_out.res.handle;
		st->copied_into_next = (st->copied_into_next + 1u) %
			(uint32_t)(sizeof(st->copied_into) / sizeof(st->copied_into[0]));

		// THE COPY SOURCE. With the codec on, the frame that goes back is the DECODE's output -
		// the untouched HDR original plus the network's residual - not the network's raw
		// display-referred answer. Both textures are ours and both rest in UNORDERED_ACCESS, so
		// the barrier pair below is identical either way. Their FORMATS differ (result_tex is
		// out_fmt, out_tex is neural_fmt) and the guard above tested the one selected here.
		const resource        src     = codec_used ? st->result_tex : st->out_tex;
		const resource        pair[2] = { src, taa_out.res };
		const resource_usage  from[2] = { resource_usage::unordered_access, resource_usage::shader_resource_non_pixel };
		const resource_usage  to[2]   = { resource_usage::copy_source,      resource_usage::copy_dest };

		cmd->barrier(2, pair, from, to);
		cmd->copy_texture_region(src, 0, nullptr, taa_out.res, 0, nullptr, filter_mode::min_mag_mip_point);
		cmd->barrier(2, pair, to, from);

		// ---- ARM THE TEMPORAL-FEEDBACK FIX -----------------------------------------------------
		// ONLY here, and only now: a denoised image has just been written into taa_out.res, which
		// UE 4.27 will extract as the next frame's history. orig_tex holds the PRE-denoise image of
		// that same resource, taken before any of this. If the evaluate did not run, or copy_back
		// is off, nothing was overwritten and there is nothing to undo - so the slot stays clear.
		if (g_cfg.history_restore && orig_saved)
		{
			if (st->pending_res != 0 && !st->logged_hist_double_arm)
			{
				st->logged_hist_double_arm = true;
				LOGW("DLSS-NR: a second accepted TAA dispatch reached the history-restore arming "
				     "point in the same frame, before the first one's pristine copy was consumed "
				     "(previous res=0x%llx, new res=0x%llx). Only the newer copy survives, so one "
				     "view's history keeps a denoised frame. This should not happen while "
				     "shader_hash pins a single permutation. This message is printed once.",
				     (unsigned long long)st->pending_res, (unsigned long long)taa_out.res.handle);
			}
			st->pending_res = taa_out.res.handle;
			st->pending_w   = taa_out.w;
			st->pending_h   = taa_out.h;
			st->pending_fmt = taa_out.fmt;
		}
	}

	// taa_out MUST go back to unordered_access: that is the state the game left it in and the
	// state its own tracking believes it is in.
	cmd->barrier(taa_out.res, resource_usage::shader_resource_non_pixel, resource_usage::unordered_access);
}

// --------------------------------------------------------------------------------------------
// Bring-up and teardown.
//
// All of this runs on the main thread: init_device / destroy_device are never called from a
// command-list recording thread, which is what makes the LoadLibraryW of a 166 MB snippet and the
// queue wait_idle acceptable here and nowhere else.
// --------------------------------------------------------------------------------------------
static void nr_init_device(device *dev)
{
	// The add-on's own directory, not the exe's: the snippet, the trampoline and the ini all
	// ship beside the .addon64.
	const std::wstring dir = ngx::module_directory_of(reinterpret_cast<const void *>(&nr_init_device));

	static bool s_config_loaded = false;
	if (!s_config_loaded)
	{
		s_config_loaded = true;
		LOGI("DLSS-NR: reading configuration from %sstray_dlssnr.ini", ngx::narrow(dir).c_str());
		cfg::load(g_cfg, dir, [](const char *line) { LOGI("%s", line); });
		// ---- BEGIN overlay_ui hook ----
		// Copy the live half of the freshly parsed ini into the overlay's atomics. Main thread,
		// before any dispatch and before any overlay draw, so nothing can observe a half-seeded
		// state. Also records the directory the Save button rewrites, and the baseline that the
		// "Revert to stray_dlssnr.ini" button and the dirty test compare against.
		overlay_ui::seed_from_config(g_cfg, dir);
		// ---- END overlay_ui hook ----
	}

	if (!g_cfg.enabled)
	{
		LOGI("DLSS-NR is DISABLED (enabled=0). The add-on is a strict no-op on the render path: "
		     "no snippet is loaded, no resource is created, and the game's dispatches are issued "
		     "by ReShade exactly as they would be with no add-on present.");
		return;
	}

	if (dev->get_api() != device_api::d3d12)
		return;

	// The snippet is process-wide and is loaded exactly once.
	static bool s_snippet_tried = false;
	if (!s_snippet_tried)
	{
		s_snippet_tried = true;
		if (!ngx::load_snippet(g_snippet, dir, g_cfg.require_trampoline))
		{
			// A missing snippet is the EXPECTED state for a stock install, and is not an error.
			LOGI("DLSS-NR not available: %s", g_snippet.not_available_reason.c_str());
			return;
		}
		LOGI("DLSS-NR: loaded nvngx_dlssnr.dll%s.",
			g_snippet.trampoline_module != nullptr
				? " and routed every call through remix_nvngx.dll"
				: " (WITHOUT remix_nvngx.dll - require_trampoline=0; every GATED export is "
				  "expected to return 0xbad00002)");
	}

	if (!g_snippet.available)
		return;

	// The NGX half is DELIBERATELY NOT DONE HERE.
	//
	// NVSDK_NGX_D3D12_Init_Ext hangs when called from init_device. Measured in STRAY: the log
	// stops between "loaded nvngx_dlssnr.dll" and the Init_Ext result, the process sits at ~2%
	// CPU, and the title never reaches its menu. init_device fires while the game is still
	// inside CreateDXGIFactory1 with a half-built device, and the snippet's D3D12 entry point
	// does not tolerate that. Note the Vulkan backend in our Remix build has no such problem -
	// it is initialised from a live render path, which is what this now imitates.
	//
	// So: load the module here (cheap, and it is the expensive-but-safe part), and defer every
	// call INTO it to the first dispatch, on the render thread, with a device the game has
	// finished building.
	g_nr_pending_init.store(true, std::memory_order_release);
	LOGI("DLSS-NR: snippet loaded. NGX initialisation deferred to the first render-thread "
	     "dispatch - calling Init_Ext from init_device hangs this title.");
}

// Runs once, on a command-list recording thread, from nr_try_run. Everything here needs a fully
// constructed device, which is exactly what init_device does not give us.
static bool nr_lazy_ngx_init(device *dev)
{
	const std::wstring dir = ngx::module_directory_of(reinterpret_cast<const void *>(&nr_init_device));

	auto *st = probe::pd_create<nr_state>(dev, kNrStateGuid);
	if (st == nullptr)
		return false;

	st->d3d12 = reinterpret_cast<ID3D12Device *>(dev->get_native());
	if (st->d3d12 == nullptr)
	{
		LOGE("DLSS-NR: device::get_native() returned null; cannot initialise NGX.");
		return false;
	}

	// The snippet resolves its weights out of its own embedded WEIGHTS_HT resource, so this path
	// is only used for the log file it writes. It must be WRITABLE, or Init_Ext fails with
	// FAIL_UnableToWriteToAppDataPath.
	const ngx::Result r = g_snippet.init_ext(g_cfg.app_id, dir.c_str(), st->d3d12, ngx::kVersionApi, nullptr);
	if (ngx::failed(r))
	{
		nr_log_ngx(reshade::log::level::error, "NVSDK_NGX_D3D12_Init_Ext", r);
		if (r == ngx::Result_FAIL_PlatformError)
			LOGE("DLSS-NR: Init_Ext is a GATED export. FAIL_PlatformError here almost certainly "
			     "means the snippet's caller check rejected the call. remix_nvngx.dll must be "
			     "present beside the add-on, and its forwarders must make REAL calls - a tail "
			     "jump reuses this add-on's return address and defeats the whole point.");
		if (r == ngx::Result_FAIL_UnableToWriteToAppDataPath)
			LOGE("DLSS-NR: the add-on's own directory is not writable, which is where the snippet "
			     "wants to put its log.");
		LOGE("DLSS-NR stays OFF. The game is untouched.");
		return false;
	}

	// Our own NVSDK_NGX_Parameter. The snippet exports no AllocateParameters on any backend, and
	// the SDK fallback would require the DRIVER's NGX runtime to have been initialised - which is
	// exactly the dependency this whole design exists to avoid. See ngx_interop.hpp for why the
	// vtable is laid out by hand.
	st->params = new (std::nothrow) ngx::parameter_block();
	if (st->params == nullptr)
	{
		LOGE("DLSS-NR: out of memory allocating the NGX parameter block. The pass stays off.");
		return false;
	}

	// ---- the HDR codec -----------------------------------------------------------------------
	// Built HERE, on the render thread, once, next to the LoadLibraryW of the snippet - never on a
	// command-list recording thread. A failure is survivable by construction: codec_failed latches,
	// the pass runs exactly as it did before the codec existed, and the reason is in the log.
	if (g_cfg.hdr_codec)
	{
		static hdr_codec::blobs s_blobs;      // process-wide: the source never changes
		static bool             s_blobs_tried = false;
		if (!s_blobs_tried)
		{
			s_blobs_tried = true;
			hdr_codec::build(dir, s_blobs, [](int lvl, const char *msg) {
				logf(lvl == hdr_codec::log_error ? reshade::log::level::error
				   : lvl == hdr_codec::log_warn  ? reshade::log::level::warning
				                                 : reshade::log::level::info, "%s", msg);
			});
		}

		if (!s_blobs.ok ||
		    !hdr_codec::create(dev, s_blobs, st->codec, [](int lvl, const char *msg) {
				logf(lvl == hdr_codec::log_error ? reshade::log::level::error
				   : lvl == hdr_codec::log_warn  ? reshade::log::level::warning
				                                 : reshade::log::level::info, "%s", msg);
			}))
		{
			st->codec_failed = true;
			LOGW("DLSS-NR: the HDR codec is OFF for this run (see the error above). The denoise "
			     "still runs and is still written back; the network is fed the raw linear TAA "
			     "output, which is README gap 1 - expect the darkening this codec exists to fix. "
			     "Set hdr_codec=0 to silence this.");
		}
		else
		{
			LOGI("DLSS-NR: HDR codec pipelines created (encode + decode, cs_5_0 DXBC, "
			     "[numthreads(16,16,1)]).");
		}
	}
	else
	{
		st->codec_failed = true;   // not a failure, but the same "do not use it" state
		LOGW("DLSS-NR: hdr_codec=0. DLSSNR.Color is bound to the RAW TAA output: linear, "
		     "unbounded, upstream of bloom, eye adaptation and the film tone curve. That is "
		     "out-of-distribution for a display-referred network - README gap 1.");
	}

	if (g_cfg.populate_parameters && g_snippet.populate_params != nullptr)
	{
		const ngx::Result pr = g_snippet.populate_params(st->params);
		nr_log_ngx(ngx::failed(pr) ? reshade::log::level::warning : reshade::log::level::info,
		           "PopulateParameters_Impl (populate_parameters=1)", pr);
	}

	LOGI("==================================================================");
	LOGI("DLSS-NR ARMED. feature id %u, preset %u (the only network in this snippet build).",
	     ngx::kFeatureDLSSNR, ngx::kOnlyPreset);
	LOGI("  target shader   0x%016llx%s", (unsigned long long)g_cfg.shader_hash,
	     g_cfg.shader_hash == 0 ? "  (0 = any shader passing all census gates - NOT recommended)" : "");
	LOGI("  registers       depth=t%u velocity=t%u colour=t%u output=u%u",
	     g_cfg.srv_depth, g_cfg.srv_velocity, g_cfg.srv_colour, g_cfg.uav_output);
	LOGI("  tuning          Intensity=%.3f LocalTone=%.3f LocalStructure=%.3f "
	     "SkinStructure=%.3f Style=%u UseAutoMask=%d",
	     g_cfg.intensity, g_cfg.local_tone_strength, g_cfg.local_structure_strength,
	     g_cfg.skin_structure_strength, g_cfg.style, (int)g_cfg.use_auto_mask);
	LOGI("  behaviour       copy_back=%d depth_inverted=%d restore_graphics_root=%d",
	     (int)g_cfg.copy_back, (int)g_cfg.depth_inverted, (int)g_cfg.restore_graphics_root);
	LOGI("  hdr codec       hdr_codec=%d paper_white_scale=%.4f (UNCALIBRATED) "
	     "transfer_strength=%.3f color_strength=%.3f",
	     (int)(g_cfg.hdr_codec && !st->codec_failed), g_cfg.paper_white_scale,
	     g_cfg.transfer_strength, g_cfg.color_strength);
	LOGI("  feedback fix    history_restore=%d (%s)", (int)g_cfg.history_restore,
	     !g_cfg.history_restore
	        ? "OFF: the denoised image is left in the TAA output, which UE 4.27 extracts as next "
	          "frame's history - TEMPORAL FEEDBACK IS UNMITIGATED, see README gap 5"
	        : (g_cfg.copy_back
	             ? "the pre-denoise TAA output is written back over the game's history before it is "
	               "read, so the game's accumulator never sees a denoised frame"
	             : "INERT with copy_back=0: nothing is written back, so there is nothing to undo"));
	if (!g_cfg.copy_back)
		LOGW("  copy_back=0: the denoised image is written to a texture NOTHING READS. The frame "
		     "will look exactly like stock TAA. This is the bring-up configuration - a frame that "
		     "still renders correctly is positive evidence that the state restore is faithful.");

	// The two KNOWN image-quality gaps, stated up front and unconditionally. Neither one stops
	// the pass and neither one shows up as an NGX error: they degrade the picture silently, which
	// is precisely why they are shouted about here rather than left in the README alone.
	// GAP 1 is now fixed when the codec is running, and only then. The message says which.
	if (!st->codec_failed)
		LOGI("DLSS-NR: GAP 1 ADDRESSED - the HDR codec is ON. DLSSNR.Color will be the "
		     "display-referred PROXY, not raw UE4 SceneColor: proxy = SrgbEncode(SoftClip(colour * "
		     "s)), and the network's answer comes back as an ADDITIVE RESIDUAL onto the untouched "
		     "HDR original, with alpha taken from the original. The scale s is the one thing here "
		     "with no calibrated value - see paper_white_scale.");
	else
		LOGW("DLSS-NR: KNOWN GAP - NO HDR CODEC. DLSS-NR is a DISPLAY-REFERRED image network, and "
		     "the resource being bound as DLSSNR.Color is UE4 SceneColor: linear, unbounded, upstream "
		     "of bloom, eye adaptation and the film tone curve. Feeding that to the network is "
		     "out-of-distribution and DARKENING is the expected response. The working Remix "
		     "deployment wraps the evaluate in an encode/decode pair (soft clip -> exact piecewise "
		     "sRGB -> network -> additive residual back onto the untouched HDR original); this run "
		     "is NOT doing that. See README \"Known gaps\", gap 1.");
	LOGW("DLSS-NR: KNOWN GAP - MOTION VECTOR ENCODING IS NOT CONVERTED. The snippet expects "
	     "absolute pixels on the colour grid, y-down. UE4 writes screen-space velocity packed "
	     "into the texture with a scale AND A BIAS, so a unorm velocity buffer is not in those "
	     "units at all. DLSSNR.MVecScaleX/Y can rescale a grid but cannot remove a bias, so it "
	     "cannot fix this. Symptom: ghosting or smearing that does not track camera motion. The "
	     "fix is a decode compute pass into an R16G16_FLOAT buffer - see README \"Known gaps\".");
	LOGI("==================================================================");
	return true;
}

static void nr_destroy_device(device *dev)
{
	auto *st = probe::pd_get<nr_state>(dev, kNrStateGuid);
	if (st == nullptr)
		return;

	g_nr_armed.store(false, std::memory_order_release);

	{
		std::lock_guard<std::mutex> lock(st->mutex);
		nr_release_feature_and_output(dev, *st, "device teardown");

		if (st->params != nullptr)
		{
			// Ours to delete, because it was ours to allocate. A block that had come from the
			// snippet would have to go back through the snippet's DestroyParameters, and the two
			// must never be crossed - but this snippet exports neither.
			delete st->params;
			st->params = nullptr;
		}

		// The root signatures and PSOs outlive the per-resolution textures, so they are released
		// here rather than in nr_release_feature_and_output. nr_release_feature_and_output has
		// already idled the queue.
		hdr_codec::destroy(dev, st->codec);

		if (g_snippet.shutdown1 != nullptr && st->d3d12 != nullptr)
		{
			const ngx::Result r = g_snippet.shutdown1(st->d3d12);
			if (ngx::failed(r))
				nr_log_ngx(reshade::log::level::warning, "NVSDK_NGX_D3D12_Shutdown1", r);
		}
		st->d3d12 = nullptr;
	}

	probe::pd_destroy<nr_state>(dev, kNrStateGuid);

	// Deliberately NOT FreeLibrary'ing the snippet here. It is process-wide, the process is on
	// its way out, and unloading a 166 MB module that may still hold worker threads during device
	// teardown buys nothing.
}

// Serviced from present, on the main thread, where idling the queue is safe.
static void nr_service_pending_teardown(device *dev)
{
	if (dev == nullptr)
		return;
	auto *st = probe::pd_get<nr_state>(dev, kNrStateGuid);
	if (st == nullptr || !st->pending_teardown)
		return;

	std::lock_guard<std::mutex> lock(st->mutex);
	if (!st->pending_teardown)
		return;

	nr_release_feature_and_output(dev, *st, "the TAA output resolution changed");
	st->pending_teardown = false;
	// Let every one-shot diagnostic speak again for the new resolution.
	st->logged_taa_found      = false;
	st->logged_srv_reject     = false;
	st->logged_uav_reject     = false;
	st->logged_uav_ambiguous  = false;
	st->logged_restore_reject = false;
	st->logged_create_fail    = false;
	st->logged_eval_fail      = false;
	st->logged_codec_off      = false;
	st->logged_codec_tex_fail = false;
	st->logged_hist_active    = false;
	st->logged_hist_dropped   = false;
	st->logged_hist_odd_reg   = false;
	st->logged_hist_tex_fail  = false;
	// The ring of resources the copy-back wrote into holds handles from the OLD resolution, whose
	// resources are on their way out; a recycled address could otherwise false-match. Clear it
	// with the latch it feeds.
	for (uint64_t &h : st->copied_into) h = 0;
	st->copied_into_next = 0;
	st->logged_feedback_loop  = false;
	// The identity statement is deliberately NOT reset: it is a property of the codec, not of the
	// resolution, and it has already been said once.
}

static void on_init_command_queue(command_queue *queue)
{
	PROBE_GUARD_VOID({
		if (queue == nullptr)
			return;
		// The graphics queue is the one the game's TAA command list is submitted on, and the one
		// that must be idle before an NGX feature or our output texture is destroyed.
		if ((queue->get_type() & command_queue_type::graphics) == 0)
			return;
		command_queue *expected = nullptr;
		g_queue.compare_exchange_strong(expected, queue, std::memory_order_relaxed);
	})
}

static void on_destroy_command_queue(command_queue *queue)
{
	PROBE_GUARD_VOID({
		// Fires BEFORE ID3D12CommandQueue::Release, so the pointer must be dropped here or a
		// later wait_idle would run on a dead queue.
		command_queue *expected = queue;
		g_queue.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed);
	})
}

static bool on_draw(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t)
{
	PROBE_GUARD_FALSE({ if (g_cfg.diagnostics) dump_bindings(cmd, false); })
}

static bool on_draw_indexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
{
	PROBE_GUARD_FALSE({ if (g_cfg.diagnostics) dump_bindings(cmd, false); })
}

// UE 4.27 TAA is compute-only: every entry point in TemporalAA.cpp is SF_Compute
// (FTAAStandaloneCS "MainCS"). This is the handler that matters, and the only one that ever
// returns true.
//
// TRUE HERE MEANS "the game's Dispatch has ALREADY been issued by nr_try_run", not "suppressed".
// See the file header. Every other path returns false, and ReShade then issues the Dispatch
// exactly as it would with no add-on loaded.
static bool on_dispatch(command_list *cmd, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
{
	// 'handled' is passed BY REFERENCE into nr_try_run, which sets it on the line after it issues
	// the game's Dispatch. It is deliberately NOT the function's return value: a return value only
	// materialises on a normal return, so an exception would leave it false and ReShade would issue
	// the Dispatch a second time. This is the shape PROBE_GUARD_RETURN's contract has always
	// described; it is now actually implemented.
	bool handled = false;
	PROBE_GUARD_RETURN(handled, {
		if (g_cfg.diagnostics)
			dump_bindings(cmd, true);
		nr_try_run(cmd, group_count_x, group_count_y, group_count_z, handled);
	})
}

// =============================================================================================
// Frame boundary: periodic census so nothing is invisible even when detail lines are budgeted
// =============================================================================================
static void on_present(command_queue *, swapchain *sc, const rect *, const rect *, uint32_t, const rect *)
{
	PROBE_GUARD_VOID({
		// A resolution change was noticed on a recording thread, which cannot idle the GPU or
		// destroy a resource. This is the main thread; do it here.
		if (sc != nullptr)
			nr_service_pending_teardown(sc->get_device());

		// Read the DLSS-NR counters BEFORE g.mutex is taken and WITHOUT st->mutex. nr_try_run
		// holds st->mutex and then takes g.mutex; acquiring them in the other order here would
		// deadlock. They are atomics for exactly this reason.
		nr_state *nst = (sc != nullptr) ? probe::pd_get<nr_state>(sc->get_device(), kNrStateGuid) : nullptr;
		const uint64_t nr_hist_applied = (nst != nullptr) ? nst->hist_restored.load(std::memory_order_relaxed) : 0;
		const uint64_t nr_hist_dropped = (nst != nullptr) ? nst->hist_dropped.load(std::memory_order_relaxed) : 0;
		const bool     nr_codec_on     = (nst != nullptr) && nst->census_codec_on.load(std::memory_order_relaxed);
		const bool     nr_orig_on      = (nst != nullptr) && nst->census_orig_on.load(std::memory_order_relaxed);

		std::lock_guard<std::mutex> lock(g.mutex);
		g.frame++;

		const bool first = (g.frame == 1);
		if (!first && (g.frame - g.last_stats_frame) < kStatsEveryFrames)
			return;
		g.last_stats_frame = g.frame;

		// The temporal-feedback fix's own accounting. 'applied' climbing at one per accepted
		// dispatch with 'dropped' flat is the success signature; 'dropped' climbing means the
		// resource we denoised is NOT turning up as a colour SRV at the next dispatch, i.e. the
		// UE 4.27 history model does not hold for this build and the loop is NOT broken.
		if (nst != nullptr && (nr_hist_applied != 0 || nr_hist_dropped != 0 || g_cfg.history_restore))
			LOGI("--- DLSS-NR history restore @ frame %llu: applied=%llu dropped=%llu "
			     "(history_restore=%d copy_back=%d hdr_codec_running=%d pristine=%s)",
			     (unsigned long long)g.frame,
			     (unsigned long long)nr_hist_applied, (unsigned long long)nr_hist_dropped,
			     // ---- BEGIN overlay_ui hook ----
			     // These two are LIVE now, and the value in g_cfg is written by the overlay snapshot on a
			     // RECORDING thread. This is the only place outside nr_try_run that reads a
			     // snapshot-written field, so it reads the atomic instead - which removes the add-on's
			     // last data race rather than leaving one behind for the sake of a log line.
			     (int)overlay_ui::live_history_restore(), (int)overlay_ui::live_copy_back(),
			     // ---- END overlay_ui hook ----
			     (int)nr_codec_on, nr_orig_on ? "allocated" : "MISSING");

		LOGI("--- probe census @ frame %llu: shaders=%llu (not_dxbc=%llu dxil=%llu) | "
		     "census fail=%llu pass=%llu | vel_const fail=%llu pass=%llu | "
		     "loops_rej=%llu conf_rej=%llu | PASSED_ALL=%llu | srv_dumps=%u/%u",
			(unsigned long long)g.frame,
			(unsigned long long)g.n_shaders_seen, (unsigned long long)g.n_not_dxbc, (unsigned long long)g.n_dxil,
			(unsigned long long)g.n_fail_census, (unsigned long long)g.n_pass_census,
			(unsigned long long)g.n_fail_velocity_const, (unsigned long long)g.n_pass_velocity_const,
			(unsigned long long)g.n_fail_loops, (unsigned long long)g.n_fail_confidence,
			(unsigned long long)g.n_pass_all, g.srv_dumps, kMaxSrvDumps);

		if (first && g.n_shaders_seen == 0)
			LOGW("no PS/CS seen by the first present. If this persists, init_pipeline is not "
			     "firing - check that this is a ReShade build WITH add-on support (RESHADE_ADDON=2).");
	})
}

// =============================================================================================
// Registration
// =============================================================================================
//
// ABI COMPATIBILITY IS NOT NEGOTIABLE, AND MUST NOT BE NEGOTIATED.
//
// An earlier version of this file walked the requested API version down from RESHADE_API_VERSION
// to 14 until ReShadeRegisterAddon accepted one. That is wrong, and quietly so:
//
//   * ReShadeRegisterAddon accepts ANY version <= its own (addon_manager.cpp: the test is
//     `api_version > RESHADE_API_VERSION` -> reject), so a false low number is taken silently.
//   * The declared version is an ABI SELECTOR, not a label. addon_manager.hpp dispatches
//     callbacks differently on it (`find_addon(...)->api_version < 20` picks the old void-return
//     signature for begin_render_pass/end_render_pass).
//   * addon_event has NO explicit enumerator values (reshade_events.hpp). Every event id this
//     add-on hands to ReShadeRegisterEvent is POSITIONAL and valid only for the exact header
//     revision it was compiled from. Registering under a lower number does not renumber them.
//
// So the downgrade bought nothing when the host was at our version, and when the host was older
// it turned a clean refusal into a run that decodes every event id and struct layout against the
// wrong ABI - the "confidently wrong answer rather than no answer" this whole probe is built to
// avoid. reshade.hpp's own register_addon refuses rather than downgrades; so do we.
//
// If registration fails, ReShade itself logs the reason ("the requested API version (%u) is not
// supported"), the add-on returns FALSE from DllMain and does not load. That is the correct,
// loud diagnosis. The fix is then to vendor the include/ tree from the ReShade release actually
// in use, so RESHADE_API_VERSION, the addon_event ordinals and the struct layouts all match.
static bool register_addon_strict(HMODULE addon_module)
{
	// Prime the static so log::message attributes messages to this add-on. Must happen before
	// anything else calls it with nullptr.
	reshade::internal::get_current_module_handle(addon_module);

	HMODULE reshade_module = reshade::internal::get_reshade_module_handle();
	if (reshade_module == nullptr)
		return false;

	const auto func = reinterpret_cast<bool (*)(void *, uint32_t)>(
		GetProcAddress(reshade_module, "ReShadeRegisterAddon"));
	if (func == nullptr)
		return false;

	return func(addon_module, RESHADE_API_VERSION);
}

static void register_events()
{
	using namespace reshade;

	register_event<addon_event::init_device>(&on_init_device);
	register_event<addon_event::destroy_device>(&on_destroy_device);

	register_event<addon_event::init_command_queue>(&on_init_command_queue);
	register_event<addon_event::destroy_command_queue>(&on_destroy_command_queue);

	register_event<addon_event::init_command_list>(&on_init_command_list);
	register_event<addon_event::destroy_command_list>(&on_destroy_command_list);
	register_event<addon_event::reset_command_list>(&on_reset_command_list);

	register_event<addon_event::init_pipeline_layout>(&on_init_pipeline_layout);
	register_event<addon_event::destroy_pipeline_layout>(&on_destroy_pipeline_layout);

	register_event<addon_event::init_pipeline>(&on_init_pipeline);
	register_event<addon_event::destroy_pipeline>(&on_destroy_pipeline);

	register_event<addon_event::update_descriptor_tables>(&on_update_descriptor_tables);
	register_event<addon_event::copy_descriptor_tables>(&on_copy_descriptor_tables);

	register_event<addon_event::bind_pipeline>(&on_bind_pipeline);
	register_event<addon_event::bind_descriptor_tables>(&on_bind_descriptor_tables);
	register_event<addon_event::push_descriptors>(&on_push_descriptors);
	register_event<addon_event::push_constants>(&on_push_constants);
	register_event<addon_event::bind_render_targets_and_depth_stencil>(&on_bind_render_targets_and_depth_stencil);

	register_event<addon_event::draw>(&on_draw);
	register_event<addon_event::draw_indexed>(&on_draw_indexed);
	register_event<addon_event::dispatch>(&on_dispatch);

	register_event<addon_event::present>(&on_present);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		// DllMain runs under the loader lock: no file I/O, no threads, no heavy init here.
		if (!register_addon_strict(hModule))
			return FALSE;
		register_events();
		LOGI("STRAY DLSS-NR registered at RESHADE_API_VERSION=%u.", (unsigned)RESHADE_API_VERSION);
		// ---- BEGIN overlay_ui hook ----
		// DllMain is the one point in this file that sits BELOW every global the overlay reports on,
		// so the status hook is installed here rather than scattered through init. It copies BY VALUE
		// and only the LOAD-ONLY half of g_cfg: the live half is written by the snapshot on a
		// recording thread, and handing the overlay a pointer would make reading one of those by
		// accident possible.
		overlay_ui::facts_hook() = [](overlay_ui::host_facts &f) {
			f.addon_name          = NAME;
			f.enabled_at_load     = g_cfg.enabled;
			f.diagnostics         = g_cfg.diagnostics;
			f.hdr_codec_at_load   = g_cfg.hdr_codec;
			f.shader_hash         = g_cfg.shader_hash;
			f.srv_depth           = g_cfg.srv_depth;
			f.srv_velocity        = g_cfg.srv_velocity;
			f.srv_colour          = g_cfg.srv_colour;
			f.uav_output          = g_cfg.uav_output;
			f.populate_parameters = g_cfg.populate_parameters;
			f.require_trampoline  = g_cfg.require_trampoline;
			f.app_id              = g_cfg.app_id;
			f.ini_found           = g_cfg.ini_found;
			f.snippet_loaded      = g_snippet.available;
			f.trampoline          = g_snippet.trampoline_module != nullptr;
			f.armed               = g_nr_armed.load(std::memory_order_relaxed);
			f.abi_thunks_active   = probe::msvc_abi_thunks_active();
			std::snprintf(f.snippet_reason, sizeof(f.snippet_reason), "%s",
				g_snippet.not_available_reason.c_str());
		};
		// Binding the ImGui table is GetProcAddress on an already-loaded module plus a call that
		// returns the address of a static table, so it is loader-lock safe. A ReShade build without
		// add-on ImGui support costs the overlay and nothing else; the denoise is unaffected.
		overlay_ui::install();
		// ---- END overlay_ui hook ----
		{
			// The C++ ABI self-check. See msvc_abi.hpp: the three device virtuals that return a
			// class by value are the only place where a non-MSVC build of this add-on and an
			// MSVC build of ReShade disagree, and they are on the SRV-dump path.
			size_t slots[3] = { 0, 0, 0 };
			const bool ok = probe::msvc_abi_self_check(slots);
			if (!probe::msvc_abi_thunks_active())
			{
				LOGI("C++ ABI: Microsoft (built with MSVC/clang-cl). By-value returns called directly.");
			}
			else if (ok)
			{
				LOGI("C++ ABI: Itanium/GNU build against an MSVC-built ReShade. Using explicit "
				     "out-parameter thunks for get_resource_desc / get_resource_from_view / "
				     "get_resource_view_desc at vtable byte offsets %zu / %zu / %zu.",
					slots[0], slots[1], slots[2]);
			}
			else
			{
				LOGE("C++ ABI: vtable offsets derived from the headers (%zu / %zu / %zu) do NOT "
				     "match the offsets measured for the Microsoft ABI (80 / 104 / 112). The "
				     "by-value-return thunks are DISABLED, so every resolved SRV will report as "
				     "unknown. Rebuild with clang targeting x86_64-pc-windows-msvc, or re-measure "
				     "the offsets against the vendored reshade_api_device.hpp.",
					slots[0], slots[1], slots[2]);
			}
		}
		break;
	case DLL_PROCESS_DETACH:
		// unregister_addon unregisters every event this add-on registered.
		reshade::unregister_addon(hModule);
		break;
	}
	return TRUE;
}
