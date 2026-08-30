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
#include "mvec_decode.hpp"
// README gap 3 (the typeless, planar depth resource NGX cannot read the format of) and README
// gap 4 (depth_inverted was inferred, never measured), in one compute pass. Included after
// hdr_codec.hpp for the same reason mvec_decode.hpp is: it reuses that header's compile cache and
// its pipeline-layout helpers rather than duplicating them.
#include "depth_convert.hpp"
// Diagnostic only, and off unless nr_probe=1: measures the network's own input against its own
// output in the same frame, and sweeps structure strength inside a single run. Included after
// hdr_codec.hpp because it reuses that header's compile cache and pipeline helpers.
#include "nr_probe.hpp"
// VENDORED VERBATIM from ../stray-sr-design/ue4_jitter.hpp - see README §8. It is copied rather
// than reached for with -I because build.sh must keep working for a tree shipped WITHOUT the
// sibling design directory, and because an edit outside src/ would change the shipped binary with
// no diff inside it. Only the View-uniform-buffer discovery, its validation and read_view_cb are
// used here; the jitter half is DLSS-SR's and is untouched.
#include "ue4_jitter.hpp"
#include "dlss_sr.hpp"
#include "rt_census.hpp"

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
	"Runs NVIDIA DLSS Neural Rendering (NGX feature 18) on STRAY's resolved TAA output, and - "
	"behind the dlss_sr ini key, default OFF - DLSS Super Resolution (NGX feature 1) in place of "
	"the pass itself. Identifies the TAA pass by DXBC token analysis plus a D3D12 descriptor "
	"shadow, evaluates the nvngx_dlssnr.dll / nvngx_dlss.dll snippets directly through "
	"remix_nvngx.dll, and restores the command-list state NGX destroys. A strict no-op when "
	"disabled or when the snippet is absent.";
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

// The functor rt_census.hpp logs through. A free function, not a lambda, so every call site can
// pass the same one without capturing anything. Levels are rt_census::log_{info,warn,error}.
static void rt_census_log(int level, const char *msg)
{
	switch (level)
	{
	case rt_census::log_warn:  LOGW("%s", msg); break;
	case rt_census::log_error: LOGE("%s", msg); break;
	default:                   LOGI("%s", msg); break;
	}
}

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
		// The last chance to report: DLL_PROCESS_DETACH runs under the loader lock and is no
		// place to take two mutexes and write dozens of log lines. A hard kill loses this block,
		// which is exactly why the periodic summary exists as well.
		rt_census::report(true, &rt_census_log);
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

	// DLSS-NR ADDITION. The DECODE BIAS, measured rather than assumed. mvec_decode.hpp's whole
	// constant set rests on STRAY's velocity decode being stock UE 4.27, and Gate B only ever
	// checked the SCALE. This line is what makes that a measurement. It gates nothing.
	if (info.found_velocity_constant)
	{
		if (info.found_velocity_bias)
			LOGI("     velocity decode: scale 4.00801611f (0x408041AB) AND bias "
			     "(32767/65535)*InvDiv = 2.00397754f (%s form, 0x%08X) both present. This game's "
			     "DecodeVelocityFromTexture is STOCK UE 4.27, so mvec_decode's constants are "
			     "MEASURED, not inferred.",
			     info.velocity_bias_form == 1 ? "negated/mad" : "positive",
			     info.velocity_bias_form == 1 ? kVelocityDecodeNegBiasBits
			                                  : kVelocityDecodeBiasBits);
		else
			LOGW("     velocity decode: the scale 4.00801611f is present but the folded decode "
			     "BIAS immediate (0x4000412B / 0xC000412B, = (32767/65535)*InvDiv) is NOT. Gate B "
			     "does not care - it never did - but mvec_decode assumes a STOCK "
			     "DecodeVelocityFromTexture, and this is the one thing that would say otherwise. "
			     "It may simply mean a different compiler folded the mad differently. If "
			     "mvec_decode=1 produces motion that is coherent but wrong by a CONSTANT OFFSET, "
			     "re-derive the bias from this shader's own bytecode before trusting it.");
	}

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

		// DXR FIRST, AND BEFORE THE PS/CS FILTER BELOW.
		//
		// The loop that follows skips every sub-object that is not a pixel or compute shader, so
		// raygen_shader / miss_shader / closest_hit_shader / any_hit_shader / intersection_shader /
		// callable_shader / libraries / shader_groups all fall through it and NOTHING about ray
		// tracing ever reaches g.n_dxil. That is why the census's `dxil=` counter has never been
		// evidence about DXR in either direction: it counts PS and CS only, and a DXIL ray tracing
		// library is not either of those. rt_census::note_pipeline is the fix; the census line in
		// on_present now says so out loud rather than leaving `dxil=0` to be misread as "no RT".
		//
		// A strict no-op when rt_census=0: one relaxed atomic load, then return.
		rt_census::note_pipeline(subobject_count, subobjects, pso, &rt_census_log);

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
			// Tier 0 of the census: SetPipelineState1 is only ever used for ray tracing state
			// objects, so this is a free count of "RT pipelines were bound". Strict no-op when
			// rt_census=0 - one relaxed atomic load.
			rt_census::note_state_object_bind(pso);
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

// DLSS SUPER RESOLUTION. A SECOND, INDEPENDENT snippet (nvngx_dlss.dll, NGX feature 1) routed
// through remix_nvngx.dll's SLOT B. Slot A stays DLSS-NR's, untouched - see ngx_interop.hpp
// snippet_spec. With dlss_sr=0 this module is never loaded, g_sr_snippet stays default-constructed
// and g_sr_armed is never set, so not one instruction of the SR path executes.
static ngx::snippet g_sr_snippet;
static std::atomic<bool> g_sr_armed{ false };

// True only once nvngx_dlssnr.dll has been loaded through remix_nvngx.dll AND
// NVSDK_NGX_D3D12_Init_Ext has succeeded. Read on the hot dispatch path before anything else, so
// a disabled or snippet-less install pays one relaxed atomic load per dispatch and nothing more.
static std::atomic<bool> g_nr_armed{ false };
// Set once the snippet module is loaded; cleared by the first render-thread pass.
static std::atomic<bool> g_nr_pending_init{ false };

// ---- DID Init_Ext ALREADY RUN AND FAIL? ------------------------------------------------------
// nr_try_run's deferred initialiser is a ONE-SHOT PER PROCESS: s_init_running is set before the
// attempt and is never cleared, on failure or otherwise. That is deliberate - the only thing this
// project has measured about Init_Ext's fragility is that it HANGS when called at a moment the
// snippet does not tolerate (see nr_lazy_ngx_init's header), and a hang is not a failure that
// degrades, so retrying it in-process is not a trade this add-on makes.
//
// The consequence has to be VISIBLE rather than silent. Without these two the panel showed
// "STANDBY - NGX has not been initialised yet ... this clears itself as soon as the game renders"
// for ever, and re-ticking `enabled` logged "reconfigure APPLIED" for a request that reached
// nothing at all. With them, the status block says NGX INITIALISATION FAILED with the NGX result
// code and that a relaunch is required, and the service reports the reconfigure as FAILED.
// Written once on the render thread before g_nr_armed could ever be set; read on the present
// thread. Atomics because of that crossing, relaxed because nothing is ordered against them.
static std::atomic<bool>     g_nr_init_failed{ false };
static std::atomic<uint32_t> g_nr_init_result{ 0 };

// ---- HAS nr_lazy_ngx_init FINISHED, WHATEVER IT DECIDED? -------------------------------------
// SEPARATE FROM g_nr_init_failed ON PURPOSE, and the separation is the fix for a real defect.
//
// nr_service_reconfigure's "initialisation is in flight" branch needs one thing: is lazy init
// still running? It used to ask g_nr_init_failed, which was a sound proxy only while every
// record_init_failure was immediately followed by `return false`. DLSS-SR broke that: the two
// calls in the DLSS-NR block now RECORD AND CARRY ON, because the SR half may still arm. That
// opened a window - the codec D3DCompile, the mvec D3DCompile and SR's Init_Ext into a 59 MB DLL,
// i.e. exactly the several hundred milliseconds the branch exists to cover - in which
// g_nr_init_failed was true while init was still running, so the branch was skipped, the drained
// action bits were dropped on the floor and the rebuild gate was opened early.
//
// The opposite hole was just as real: a pure-SR run (dlss_nr=0) whose SR Init_Ext fails records
// NO failure at all, so g_nr_init_failed stayed false for ever, the branch matched for ever, and
// every later request was queued into a pending_work that only a true init_complete can drain.
// The panel sat at REBUILDING for the rest of the session.
//
// This flag answers only the question actually being asked. It is set from a scope guard so that
// EVERY exit of nr_lazy_ngx_init sets it, including one a later edit adds without reading this.
// RELEASE-stored, ACQUIRE-loaded, and the guard is declared before the init lock so it fires
// after st->mutex is released: when the service observes it, the state is settled and free.
static std::atomic<bool>     g_nr_init_settled{ false };

static bool nr_lazy_ngx_init(device *dev);
// Defined with the rest of the DLSS-SR pass; forward-declared because the teardown path above it
// hands it to dlss_sr::destroy_resources.
static void sr_log(int lvl, const char *msg);

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

// The bits of nr_state::pending_work. The overlay's own a_* bits are copied straight into this
// word by the service's take_reconfigure call, so the two enumerations must agree - and rather
// than restate them (two lists that must match by hand is how they stop matching), the render
// side simply reuses overlay_ui's. kTeardown is the one bit this file raises on its own, from
// nr_ensure_output, when the TAA output's resolution or format moves.
enum : uint32_t { kTeardown = overlay_ui::a_teardown };

struct nr_state
{
	std::mutex mutex;

	// ---- IS THIS OBJECT SAFE FOR ANOTHER THREAD TO TOUCH YET? ----------------------------------
	//
	// probe::pd_create PUBLISHES through set_private_data on its FIRST statement
	// (descriptor_shadow.hpp:892-901), so nr_lazy_ngx_init hands this object to the whole process
	// before it has initialised a single field of it - and it then spends hundreds of milliseconds
	// in Init_Ext and in up to two runtime D3DCompile calls, on a command-list RECORDING thread,
	// filling params / codec / mvec / serviced_populate_parameters.
	//
	// That used to be safe by construction rather than by design: the present-thread servicer
	// acted only on a `pending_teardown` bool that could only be raised from inside an accepted
	// pass, i.e. strictly after lazy init had returned. nr_service_reconfigure acts on ANY overlay
	// request, so that exclusion is gone - and the overlay is exactly what the user is touching
	// while a from-the-panel arm is initialising. The unguarded window let the service run
	// nr_release_feature_and_output over fields the recording thread was writing, and reach
	// nr_build_codec_pipelines concurrently with the same call on the other thread: two threads
	// resizing the same std::vector<uint8_t> blob and assigning the same five-field pipelines
	// struct, which is heap corruption in the user's game rather than an error return.
	//
	// RELEASE-stored as the LAST act of a successful nr_lazy_ngx_init; ACQUIRE-loaded by
	// nr_service_reconfigure, which treats a state that is not ready EXACTLY as it treats a state
	// that does not exist. nr_lazy_ngx_init additionally holds `mutex` for everything after
	// pd_create, so a service call that arrives one instruction after the flag is set still
	// serialises rather than interleaves.
	std::atomic<bool> init_complete{ false };

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

	// CAMERA-CUT DETECTION. UE4 assigns PrevViewMatrices = ViewMatrices on any frame where
	// bResetCamera holds (camera cut, level load, time reset, large camera movement,
	// bForceCameraVisibilityReset), so View.TemporalAAJitter.zw becomes bitwise equal to .xy.
	// ue4_jitter.hpp has computed exactly this as result::reset_signalled since it was written;
	// nothing consumed it until now. STRAY is full of hard cutscene transitions, and without this
	// the NR temporal history carries stale samples across them and ghosts for several frames.
	// The reference add-on gets the equivalent for free by inheriting the host game's reset flag.
	// ATOMICS ON PURPOSE, exactly as mvec_frames/mvec_cb_reuse are: the on_present census reads
	// these and must NOT take st->mutex, because nr_try_run takes st->mutex then g.mutex and the
	// reverse order would deadlock. Do not "simplify" these back into plain fields.
	std::atomic<bool>     jitter_cut_ok{ false };  // jitter row readable AND validated this run
	std::atomic<uint64_t> camera_cuts{ 0 };        // cuts signalled, for the census
	bool     logged_cut_source = false;
	bool     feature_failed = false;     // latched per (out_w,out_h); cleared when they move
	uint64_t evaluate_count = 0;

	// ---- NGX getter trace ------------------------------------------------------------------
	// The five NGX tuning parameters as they were on the evaluate we last traced. A change in any
	// of them arms the trace for exactly ONE evaluate, which is the frame the user's slider drag
	// lands on - the only frame where "did the snippet read my new value?" is the open question.
	// Seeded to NaN-ish sentinels so the first evaluate always traces once.
	float    traced_intensity        = -1e30f;
	float    traced_local_tone       = -1e30f;
	float    traced_local_structure  = -1e30f;
	float    traced_skin_structure   = -1e30f;
	uint32_t traced_use_auto_mask    = 0xFFFFFFFFu;
	uint32_t traced_style            = 0xFFFFFFFFu;
	// Rate limit: a slider drag is continuous, and one trace block per frame for a second would
	// be thousands of lines. At most one every 30 evaluates, which is well under a drag's length
	// and still catches the settled value.
	uint64_t last_trace_evaluate     = 0;

	// ---- THE DEFERRED-RECONFIGURE MASK --------------------------------------------------------
	// This replaces the plain `bool pending_teardown`, and the change is a fix rather than a
	// generalisation. That bool was READ UNLOCKED at :2739 and from the present thread, while being
	// WRITTEN under this mutex by nr_ensure_output and by the service. For a single bit whose only
	// stale reading costs one deferred frame that was benign. For a MULTI-BIT mask it is not: a
	// torn or stale read drops a request permanently, and a request dropped permanently is exactly
	// the "control that lies" this whole ladder exists to remove.
	//
	// OR-MERGED, NEVER ASSIGNED. nr_ensure_output raises kTeardown on a resolution change and a
	// user reconfigure can land in the same frame; an assignment would silently drop one of them.
	// The service consumes with fetch_and(0).
	std::atomic<uint32_t> pending_work{ 0 };

	// The two consumers of the overlay's epochs, each with its own seen-state. PER DEVICE, in here,
	// deliberately - overlay_ui::begin_pass used to keep these in FUNCTION-LOCAL statics, which are
	// process-wide, so with two D3D12 devices the edge for a given change would be consumed by
	// whichever device's pass ran first and the other would miss it. At three rungs that cost one
	// reset frame; at five it costs a whole rebuild.
	overlay_ui::seen_epochs seen_pass;      // begin_pass, on the recording thread
	overlay_ui::seen_epochs seen_service;   // nr_service_reconfigure, on the present thread

	// The scratch buffer begin_pass builds its snapshot in before committing it to g_cfg. It
	// lives here, beside seen_pass, for the same per-device reason and under the same mutex: the
	// snapshot is taken under the overlay's seqlock and DISCARDED if the panel moved underneath
	// it, which needs somewhere to build that is not g_cfg itself. A local would heap-allocate
	// cfg::config's std::string on the render thread every frame; this one is assigned into and
	// reuses its capacity.
	cfg::config cfg_scratch;

	// What the service has actually DONE, so it can tell an edge from a steady state. Set to the
	// value that was in force when NGX was armed, and updated by the service on each change.
	bool     serviced_populate_parameters = false;

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
	bool logged_output_binding   = false;
	bool logged_copy_fmt         = false;
	bool logged_identity         = false;
	// The graft-mode log line is NOT a one-shot: hdr_graft is a tier-0 root constant whose entire
	// point is that the user flips it mid-session from the overlay, and a run-lifetime latch would
	// leave ReShade.log ASSERTING a graft that is no longer in effect for the rest of the session.
	// 0xFFFFFFFF is "nothing logged yet", which no real mode value can collide with.
	uint32_t logged_graft        = 0xFFFFFFFFu;
	bool logged_codec_off        = false;
	bool logged_codec_tex_fail   = false;
	bool logged_hist_active      = false;
	bool logged_hist_dropped     = false;
	bool logged_hist_odd_reg     = false;
	bool logged_hist_double_arm  = false;
	bool logged_hist_tex_fail    = false;
	bool logged_mvec_active      = false;
	bool logged_mvec_off         = false;
	bool logged_mvec_tex_fail    = false;
	bool logged_mvec_no_viewcb   = false;
	bool logged_mvec_clip_bad    = false;
	bool logged_mvec_clip_row    = false;
	bool logged_mvec_decode_only = false;
	bool logged_mvec_pinned_row  = false;
	bool logged_depth_tex_fail   = false;
	bool logged_depth_active     = false;
	bool logged_depth_off        = false;
	bool logged_depth_det_result = false;
	bool logged_depth_det_stand  = false;

	// ---- the depth conversion pass (depth_convert.hpp) ----------------------------------------
	// Our own DLSSNR.Depth: DeviceZ VERBATIM in a TYPED r32_float texture, at the colour extent.
	// The point is the FORMAT, not the values - NGX reads the format off the D3D12_RESOURCE_DESC
	// and the game's depth resource is r32_g8_typeless, which is not a depth value to anything
	// reading it that way (README gap 3).
	//
	// THE EXTENT IS THE COLOUR GRID, not the game's depth extent, for the same reason mvec_tex is:
	// it puts colour, motion guide and depth on one grid, which is the only configuration in which
	// the three rects NGX validates against each other cannot disagree. In STRAY all three are
	// 1920x1080 today, so the remap inside the shader is the identity and this costs nothing.
	//
	// No SRV is ever created on it: NGX consumes it as a raw ID3D12Resource*, exactly like out_tex
	// and mvec_tex. shader_resource is nonetheless in its usage set because create_resource asserts
	// that usage is a superset of every state the resource is transitioned to, and this one is
	// handed to NGX in SHADER_RESOURCE_NON_PIXEL.
	resource      depth_tex = { 0 };
	resource_view depth_uav = { 0 };

	depth_convert::pipelines depth_conv;
	depth_convert::detector  depth_det;
	bool depth_ok         = false;   // depth_tex + its UAV exist at (out_w, out_h)
	// Latched for the WHOLE RUN, exactly like codec_failed and mvec_failed: the DXBC could not be
	// produced, or the root signature / PSO / statistics buffers could not be created. A resolution
	// change cannot undo that.
	bool depth_failed     = false;
	// Latched per (out_w, out_h) only, exactly like mvec_tex_failed.
	bool depth_tex_failed = false;
	// NGX REJECTED OUR DEPTH. The mirror of mvec_eval_rejected, and it exists for the same reason:
	// without a rung for it a persistent EvaluateFeature failure would leave `evaluated` false
	// every frame, so the copy-back never runs and the user gets stock TAA - no denoise at all -
	// for the whole session, recoverable only by editing the ini and restarting. This latch drops
	// DLSSNR.Depth back to exactly the pre-conversion binding. Run-latched: a resolution change
	// cannot make a rejected format acceptable.
	//
	// It is a REAL possibility in BOTH directions and that is worth stating plainly. r32_float is
	// what the one known-working DLSS-NR deployment insists on, so this should be strictly more
	// acceptable than what we bind today - but "should" is not "measured", and this add-on has
	// already been surprised once by a format the snippet would not take.
	bool depth_eval_rejected = false;
	uint32_t depth_eval_fail_streak = 0;
	// Set once the live DepthInverted control has been seen to differ from what the ini (or the
	// built-in default) seeded. The control MUST win from that moment on - a live control that
	// silently does nothing is the exact defect this tree has caught in itself twice - so the
	// measurement stands down for the rest of the run. Zero-cost: two comparisons per accepted
	// dispatch, against a value captured at config load (g_depth_inverted_at_load).
	bool depth_det_stood_down  = false;

	// Census, read from on_present WITHOUT this mutex - same reason as hist_restored: on_present
	// holds g.mutex there, and nr_try_run takes st->mutex and then g.mutex, so a lock in the other
	// order would be a textbook AB/BA deadlock between the two threads.
	std::atomic<uint64_t> depth_frames{ 0 };            // frames the conversion pass actually ran
	std::atomic<bool>     census_depth_bound{ false };  // ... of which it was also BOUND as DLSSNR.Depth
	std::atomic<uint32_t> census_depth_verdict{ 0 };    // depth_convert::verdict, as its underlying value
	std::atomic<bool>     census_depth_inverted{ true };// what nr_depth_inverted_value last resolved to

	// ---- the motion-vector decode (mvec_decode.hpp) -------------------------------------------
	// Our own DLSSNR.MVec: absolute pixels on the COLOUR grid, y-down. r16g16_float, at the colour
	// extent, and NOT the velocity extent - the whole point is that the guide now lives on the
	// same grid as the colour, which is what lets MVecScaleX/Y be forced to exactly 1.0.
	//
	// No SRV is ever created on it: NGX consumes it as a raw ID3D12Resource*, exactly like
	// out_tex. shader_resource is nonetheless in its usage set for the same reason it is on
	// out_tex - on D3D12 that flag adds nothing to D3D12_RESOURCE_DESC::Flags for a colour texture.
	resource      mvec_tex = { 0 };
	resource_view mvec_uav = { 0 };

	mvec_decode::pipelines mvec;
	// Diagnostic, built only when nr_probe=1. Failure to build disables the probe and is never
	// fatal: the render path must not care whether an instrument exists.
	nr_probe::pipeline_set probe;
	nr_probe::run_state    probe_run;
	bool mvec_ok         = false;   // mvec_tex + its UAV exist at (out_w, out_h)
	// Latched for the WHOLE RUN, exactly like codec_failed: the DXBC could not be produced, or the
	// root signature / PSO could not be created. A resolution change cannot undo that.
	bool mvec_failed     = false;
	// Latched per (out_w, out_h) only, exactly like codec_tex_failed.
	bool mvec_tex_failed = false;
	// NGX REJECTED THE DECODED GUIDE. Binding our r16g16_float texture instead of the game's
	// r16g16b16a16_unorm one changes both the resource AND the DXGI format handed to the snippet,
	// and D3D12 acceptance of a 2-channel guide has NOT been measured on this hardware - the code
	// already anticipates exactly this class of rejection for the typeless depth resource
	// (FAIL_UnsupportedInputFormat). Without a rung for it, a persistent EvaluateFeature failure
	// left `evaluated` false every frame, so the copy-back never ran and the user got stock TAA -
	// no denoise at all - for the whole session, recoverable only by editing the ini and
	// restarting. This latch is the missing rung: it drops the guide back to exactly the
	// pre-decode binding, which the guide-reset latch then covers with one Reset frame.
	// Run-latched, like mvec_failed: a resolution change cannot make a rejected format acceptable.
	bool mvec_eval_rejected = false;
	uint32_t mvec_eval_fail_streak = 0;

	// ---- the View uniform buffer, for ClipToPrevClip -----------------------------------------
	// Discovered ONCE per resolution by ue4_jitter.hpp's content signature over a CPU copy of the
	// game's own View CB, and cross-checked against the probe's INDEPENDENT DXBC-derived row.
	ue4jitter::layout view_layout;
	bool     view_layout_ok     = false;
	// Per-resolution latch. Discovery reads 5232 bytes out of an upload pool; retrying it on every
	// frame after it has failed would be a per-frame Map on the hot path.
	bool     view_layout_failed = false;
	uint32_t view_discover_tries = 0;
	// The LAST GOOD ClipToPrevClip, four raw CB rows, row-major, no transpose. Kept across a
	// failed per-frame read rather than dropped: flipping the whole binding on one bad Map would
	// change the guide under NGX's temporal history mid-run, which is worse than a one-frame-stale
	// reprojection.
	float    clip_to_prev[16] = {};
	bool     clip_ok = false;
	uint32_t clip_fail_streak = 0;
	// From the View CB when ViewSizeAndInvSize validated, otherwise the TAA output extent.
	float    view_size[2] = { 0.0f, 0.0f };
	bool     view_size_measured = false;
	// The resource ACTUALLY bound as DLSSNR.MVec last frame. The guide-reset latch keys on this as
	// well as on the extent: the fallback ladder can swap the bound resource at a CONSTANT extent,
	// which the old guide_w/guide_h comparison cannot see, leaving NGX's history accumulated
	// against a different grid.
	uint64_t mvec_bound_res = 0;

	// Census, read from on_present WITHOUT this mutex - same reason as hist_restored.
	std::atomic<uint64_t> mvec_frames{ 0 };     // frames the decode pass actually ran
	std::atomic<uint64_t> mvec_cb_reuse{ 0 };   // frames that reused the last good matrix
	// 0 = raw passthrough (today's behaviour), 1 = decode only, 2 = decode + reconstruction.
	std::atomic<uint32_t> census_mvec_mode{ 0 };

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
	// Diagnostic only (nr_probe): lets the probe read out_tex the way NGX wrote it, as a UAV,
	// before the decode's barrier turns it into an SRV.
	resource_view out_uav    = { 0 };

	hdr_codec::pipelines codec;         // root signatures + PSOs, created once per device
	bool codec_textures_ok = false;     // proxy/result/views exist at (out_w, out_h)
	// Latched for the whole run: the DXBC could not be produced, or the root signature / PSO could
	// not be created. Nothing about a resolution change can undo that, so it is never cleared.
	bool codec_failed      = false;
	// Whether the DECODE BLOB IN HAND can do hdr_graft = 1. False when hdr_codec::build had to
	// fall back to the survival source because the compile with the reference graft failed here;
	// the CPU then pins the mode to 0 rather than dispatching into a stub that would silently
	// return the original. Set once, from the build site, and never cleared.
	bool codec_graft_ok    = true;
	// True when the decode came from a user-supplied stray_dlssnr_decode.dxbc, which may predate
	// g_hdrGraft and read the constant not at all. We cannot force anything in that case - the
	// blob may well be this exact source, precompiled elsewhere - so it is REPORTED, not acted on.
	bool codec_overridden  = false;
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

	// ---- DLSS SUPER RESOLUTION (dlss_sr.hpp) --------------------------------------------------
	// Every one of these is default-constructed and stays that way unless g_cfg.dlss_sr is 1.
	// The SR feature owns its OWN parameter block (sr_feat.params) - sharing the DLSS-NR block
	// would mix two disjoint key sets in one map and, worse, `Width`/`Height` mean different
	// things to the two snippets.
	dlss_sr::resources     sr_res;
	dlss_sr::feature       sr_feat;
	dlss_sr::jitter_source sr_jitter;

	// The (render, output) pair the live SR feature was built for. A change in either routes
	// through pending_teardown - never through a release on a recording thread, which cannot idle
	// the GPU.
	uint32_t sr_render_w = 0, sr_render_h = 0, sr_out_w = 0, sr_out_h = 0;

	// The COARSE geometry key, recorded UNCONDITIONALLY every frame the SR pass reaches the
	// resolution latch - whether or not anything was ever allocated at it. sr_render_w/sr_out_w
	// above are written ONLY on the create-SUCCESS branch and are therefore still 0 after a
	// failed create, so they cannot be compared against to notice a move. This pair can.
	uint32_t sr_seen_col_w = 0, sr_seen_col_h = 0, sr_seen_out_w = 0, sr_seen_out_h = 0;

	// RUN-latched. Set when EvaluateFeature fails 8 frames running: at that point every further
	// attempt is a per-frame call into a 59 MB DLL for a result we already have, and - critically
	// - under sr_suppress_taa=1 it is also 8 frames of the game's own TAA running as the fallback.
	// Latching keeps the frame correct and stops the log filling.
	bool sr_latched_off = false;
	uint32_t sr_eval_fail_streak = 0;
	// The decoded guide was the one input this build changed. If the evaluate keeps failing with
	// it bound, give it back before giving up entirely - the same rung the DLSS-NR path has.
	bool sr_mvec_rejected = false;

	// Census, read from on_present WITHOUT this mutex - same reason as hist_restored above.
	// sr_evaluates is incremented ONLY on the line after EvaluateFeature returned Success, so a
	// non-zero value here is proof the feature RAN and not merely that it linked.
	std::atomic<uint64_t> sr_evaluates{ 0 };
	std::atomic<uint64_t> sr_suppressed{ 0 };
	std::atomic<uint64_t> sr_mvec_frames{ 0 };
	std::atomic<uint32_t> sr_census_render_w{ 0 }, sr_census_render_h{ 0 };
	std::atomic<uint32_t> sr_census_out_w{ 0 }, sr_census_out_h{ 0 };

	// ---- CHAIN MODE (dlss_chain) ---------------------------------------------------------------
	// True for the whole of an accepted dispatch that is running the chain, and read by
	// nr_ensure_aux, which allocates a DIFFERENT set of textures for it: no pre-denoise copy (the
	// decode's `original` is the game's own t5 SRV, borrowed for the event exactly as the velocity
	// and depth SRVs already are) and no second motion-vector target (both networks read the one
	// DLSS-SR already decodes into). Set at the top of the chain's own precondition block and
	// cleared by nr_release_feature_and_output, so a teardown cannot leave it lying.
	bool chain_active = false;
	// RUN-latched after 8 consecutive DLSS-NR EvaluateFeature failures inside the chain. The frame
	// stays correct - DLSS-SR then runs on the game's raw colour, which is exactly dlss_sr=1 - but
	// paying for an encode, an evaluate into a 166 MB DLL and a decode every frame for an answer
	// nothing can use is not something to do silently forever.
	bool chain_nr_off = false;
	uint32_t chain_nr_fail_streak = 0;
	// PROOF OF LIFE. Incremented ONLY on the line where BOTH EvaluateFeature calls have returned
	// Success on the same dispatch, so a non-zero value is evidence that the chain RAN - not that
	// it compiled, linked and was configured. The periodic census prints it.
	std::atomic<uint64_t> chain_evaluates{ 0 };

	bool logged_chain_unavailable  = false;
	bool logged_chain_not_upsampling = false;
	bool logged_chain_codec_off    = false;
	bool logged_chain_banner       = false;
	bool logged_chain_nr_fail      = false;
	bool logged_chain_sr_only      = false;
	bool logged_chain_out_fail     = false;

	bool logged_sr_banner      = false;
	bool logged_sr_no_jitter   = false;
	bool logged_sr_out_extent  = false;
	bool logged_sr_mvec_off    = false;
	bool logged_sr_owned_throw = false;
	bool logged_sr_copy_fmt    = false;
	bool logged_sr_direct      = false;
	bool logged_sr_suppress    = false;
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
// THE PIPELINE BUILDERS, AND WHY THEIR SOURCE BLOBS ARE AT FILE SCOPE.
//
// These two used to be function-local statics inside nr_lazy_ngx_init:
//
//     static hdr_codec::blobs     s_blobs;
//     static std::vector<uint8_t> s_mvec_dxbc;
//
// No rebuild path can reach a function-local static. Leaving them there and adding a rebuild
// would have produced code that compiles cleanly, is called, and silently has nothing to build
// from - which is exactly the "a feature that compiles is not a feature that runs" failure this
// tree has already shipped once. They are process-wide because the SOURCE never changes: the HLSL
// is embedded, and building it twice would LoadLibraryW d3dcompiler_47.dll and run D3DCompile
// again for an identical result.
//
// TRANSACTIONAL, and that is requirement 3 ("a reconfigure that fails leaves the previous working
// state") satisfied structurally rather than by care. hdr_codec::create does `p = pipelines()` as
// its FIRST statement, and mvec_decode::create the same - so calling either on a LIVE struct
// leaks the old root signature and PSO, each of which holds a reference on the ID3D12Device (the
// exact leak documented at nr_destroy_device). Building into a LOCAL and only then destroying and
// moving avoids the leak AND means a failed build leaves the previous pipelines running, logs
// once, and reports the reason - never a half-applied state.
// --------------------------------------------------------------------------------------------
// The compiled shader blobs, built once per process and then read-only.
//
// TWO THREADS REACH THESE, which is why they have a lock of their own. nr_build_codec_pipelines
// and nr_build_mvec_pipeline are called from nr_lazy_ngx_init on a command-list RECORDING thread
// and from nr_service_reconfigure on the PRESENT thread. nr_state::init_complete already stops
// those two overlapping for one device; g_blob_mutex removes the single-device assumption from the
// argument entirely, because it is these FILE-SCOPE objects - not anything in nr_state - that a
// second device would collide on. hdr_codec::build resizes two std::vector<uint8_t> and writes a
// .dxbc cache file; a plain bool guard around that was a data race, not a one-shot.
//
// LOCK ORDER: taken while st->mutex is held, never the other way round, and nothing under it takes
// any other lock. It is a leaf.
static std::mutex           g_blob_mutex;
static hdr_codec::blobs     g_codec_blobs;
static bool                 g_codec_blobs_tried = false;
static std::vector<uint8_t> g_mvec_dxbc;
static bool                 g_mvec_dxbc_tried = false;
static std::vector<uint8_t> g_depth_dxbc;
static bool                 g_depth_dxbc_tried = false;

// What depth_inverted was the instant cfg::load returned - i.e. what the ini said, or the built-in
// default if it said nothing. WRITTEN ONCE, from nr_init_device, before any overlay control or any
// begin_pass snapshot can have touched g_cfg; read from the render thread thereafter.
//
// It exists because the DepthInverted control is LIVE and the gap-4 measurement must never fight
// it. A latch-time snapshot of g_cfg.depth_inverted cannot do this job: a user who moves the
// control BEFORE the verdict lands would have their choice recorded as the baseline and then
// silently overridden by the measurement a second later. Comparing against the load-time value has
// no such window - any deviation at all is a human, whenever it happened.
static bool                 g_depth_inverted_at_load = true;

// ---- THE SNIPPET'S OWN LOG -----------------------------------------------------------------
// Called BY nvngx_dlssnr.dll, on whatever thread it likes, once ngx_logging=1 puts a
// FeatureCommonInfo in front of Init_Ext. Everything here must be safe from a foreign thread and
// must not throw back across that boundary - logf is a vsnprintf into a stack buffer plus
// ReShade's own log call, and there is nothing else in the body for a reason.
static void __cdecl nr_ngx_log_callback(const char *message, ngx::LoggingLevel level, uint32_t source)
{
	if (message == nullptr)
		return;
	logf(level == ngx::LoggingLevel_Off ? reshade::log::level::debug : reshade::log::level::info,
	     "[NGX snippet, feature %u] %s", source, message);
}

// Built once and kept alive for the whole process: NGX is handed a POINTER to this and the
// documented lifetime is "must outlive the runtime", so a stack temporary here would be a
// dangling read on the first message rather than an immediate crash.
static ngx::FeatureCommonInfo g_ngx_common_info;

static const void *nr_ngx_common_info()
{
	if (g_cfg.ngx_logging == 0)
		return nullptr;   // exactly the pre-existing behaviour
	g_ngx_common_info.LoggingInfo.LoggingCallback          = &nr_ngx_log_callback;
	g_ngx_common_info.LoggingInfo.MinimumLoggingLevel      = ngx::LoggingLevel_Verbose;
	// Leave the snippet's own sinks alone: if our struct layout is wrong, its file log is the
	// only channel left that could tell us so.
	g_ngx_common_info.LoggingInfo.DisableOtherLoggingSinks = false;
	return &g_ngx_common_info;
}

static void nr_pipeline_log(int lvl, const char *msg)
{
	logf(lvl == hdr_codec::log_error ? reshade::log::level::error
	   : lvl == hdr_codec::log_warn  ? reshade::log::level::warning
	                                 : reshade::log::level::info, "%s", msg);
}

// The add-on's own directory - where the snippet, the trampoline, the ini and the shader sources
// all live. Derived from this module's own address, not the exe's.
static std::wstring nr_addon_dir()
{
	return ngx::module_directory_of(reinterpret_cast<const void *>(&nr_pipeline_log));
}

/// Build (or rebuild) the HDR codec's two pipelines into st.codec. Returns false and leaves
/// st.codec exactly as it was on any failure.
///
/// TWO CALLERS, TWO THREADS, and the contract is about WHEN rather than about which thread. It
/// LoadLibraryW's d3dcompiler_47.dll and runs D3DCompile to cs_5_0, so it may only be called at a
/// point where a multi-hundred-millisecond stall is acceptable: nr_lazy_ngx_init (once, on a
/// recording thread, during the arm the overlay's tooltip already promises will cost a frame) and
/// nr_service_reconfigure (on the present thread). It must NEVER be called from the steady-state
/// dispatch path. The caller must hold st->mutex; the file-scope blobs have g_blob_mutex of their
/// own, because those are shared across devices and st->mutex is not.
static bool nr_build_codec_pipelines(device *dev, nr_state &st, const std::wstring &dir)
{
	if (dev == nullptr || st.codec_failed)
		return false;
	if (st.codec.ok)
		return true;   // already built; nothing to do and nothing to leak

	bool blobs_ok = false;
	bool has_graft = false, overridden = false;
	{
		std::lock_guard<std::mutex> blob_lock(g_blob_mutex);
		if (!g_codec_blobs_tried)
		{
			g_codec_blobs_tried = true;
			hdr_codec::build(dir, g_codec_blobs, &nr_pipeline_log);
		}
		blobs_ok   = g_codec_blobs.ok;
		// WHAT THE BLOB IN HAND CAN ACTUALLY DO. Both are settled by build() before any dispatch,
		// and both are read UNDER g_blob_mutex rather than after it: they live in the same
		// file-scope object build() writes, so reading them outside the lock would be the same data
		// race the lock was added for.
		has_graft  = g_codec_blobs.decode_has_graft;
		overridden = g_codec_blobs.decode_overridden;
	}

	// Published to the overlay here rather than at the create below, so the combo and its warning
	// line are correct from the first frame the panel is opened - including on the failure path,
	// where there is no create to publish from. st.* is written under st->mutex, which this
	// function's contract already requires the caller to hold; publish_codec_build stores two
	// relaxed atomics and takes no lock, which is what the overlay's CI gate requires of it.
	st.codec_graft_ok   = has_graft;
	st.codec_overridden = overridden;
	overlay_ui::publish_codec_build(has_graft, overridden);

	// create() only READS the blobs, and nothing ever writes them again once tried is set, so the
	// lock is not held across it - it creates D3D12 objects and can be slow.
	hdr_codec::pipelines fresh;
	if (!blobs_ok || !hdr_codec::create(dev, g_codec_blobs, fresh, &nr_pipeline_log))
	{
		// create() has already destroyed whatever it managed to make of `fresh`, so there is
		// nothing to clean up here and st.codec is untouched.
		st.codec_failed = true;
		LOGW("DLSS-NR: the HDR codec could not be built (see the error above). The denoise still "
		     "runs and is still written back; the network is fed the raw linear TAA output, which "
		     "is README gap 1 - expect the darkening this codec exists to fix. This is a REAL "
		     "build failure and it is latched for the run: untick and re-tick HDR codec and it "
		     "will not retry, because a D3DCompile that failed once will fail again.");
		return false;
	}

	// Only now. The queue was idled by nr_release_feature_and_output before this ran (the codec
	// toggle always raises a teardown), and st.codec is empty on every path that reaches here, so
	// this destroy is a no-op guard rather than a real release - kept because a future caller
	// that skipped the teardown would otherwise leak silently.
	hdr_codec::destroy(dev, st.codec);
	st.codec = fresh;
	LOGI("DLSS-NR: HDR codec pipelines created (encode + decode, cs_5_0 DXBC, "
	     "[numthreads(16,16,1)]).");
	// The two ways hdr_graft=1 can be unavailable while every other status line says the codec is
	// fine. Both are stated HERE, once per successful build, because both are properties of the
	// BLOB rather than of the setting - the user can move the selector all they like and neither
	// will change until the .dxbc or the compiler does.
	if (!st.codec_graft_ok)
		LOGW("DLSS-NR: this decode was built WITHOUT the reference graft, so hdr_graft is PINNED "
		     "TO 0 for this run whatever the ini or the overlay say. The default additive graft - "
		     "the one this add-on ships and the image you already have - is completely unaffected; "
		     "only the hdr_graft=1 experiment is unavailable.");
	if (st.codec_overridden)
		LOGW("DLSS-NR: the decode is a USER-SUPPLIED stray_dlssnr_decode.dxbc. If that blob was not "
		     "built from this add-on's shader source it does not read the hdr_graft root constant "
		     "at all, and the graft selector will do NOTHING while the overlay and the lines below "
		     "still report the mode you picked. Delete the .dxbc if you want hdr_graft to be "
		     "honoured.");
	return true;
}

/// The same shape for the motion-vector decode.
static bool nr_build_mvec_pipeline(device *dev, nr_state &st, const std::wstring &dir)
{
	if (dev == nullptr || st.mvec_failed)
		return false;
	if (st.mvec.ok)
		return true;

	bool dxbc_ok = false;
	{
		std::lock_guard<std::mutex> blob_lock(g_blob_mutex);
		if (!g_mvec_dxbc_tried)
		{
			g_mvec_dxbc_tried = true;
			mvec_decode::build(dir, g_mvec_dxbc, &nr_pipeline_log);
		}
		dxbc_ok = !g_mvec_dxbc.empty();
	}

	mvec_decode::pipelines fresh;
	if (!dxbc_ok || !mvec_decode::create(dev, g_mvec_dxbc, fresh, &nr_pipeline_log))
	{
		st.mvec_failed = true;
		LOGW("DLSS-NR: the motion-vector decode could not be built (see the error above). "
		     "DLSSNR.MVec falls back to the game's RAW ENCODED velocity with the derived grid "
		     "scale - README gap 2 stands unmitigated. Nothing else changes. This is a REAL build "
		     "failure and it is latched for the run.");
		return false;
	}

	mvec_decode::destroy(dev, st.mvec);
	st.mvec = fresh;
	LOGI("DLSS-NR: motion-vector decode pipeline created (cs_5_0 DXBC, [numthreads(16,16,1)]). "
	     "UE 4.27 velocity decode + camera-motion reconstruction from depth through "
	     "View.ClipToPrevClip, writing absolute colour-grid pixels.");
	return true;
}

/// The same shape again for the depth conversion pass (README gap 3) and the depth-convention
/// measurement it carries (README gap 4). Every failure here is SOFT: depth_failed latches for the
/// run, DLSSNR.Depth stays on the game's own r32_g8_typeless resource - bit-for-bit the behaviour
/// before this feature existed - and DLSSNR.DepthInverted keeps whatever depth_inverted says.
static bool nr_build_depth_pipeline(device *dev, nr_state &st, const std::wstring &dir)
{
	if (dev == nullptr || st.depth_failed)
		return false;
	if (st.depth_conv.ok)
		return true;

	bool dxbc_ok = false;
	{
		std::lock_guard<std::mutex> blob_lock(g_blob_mutex);
		if (!g_depth_dxbc_tried)
		{
			g_depth_dxbc_tried = true;
			depth_convert::build(dir, g_depth_dxbc, &nr_pipeline_log);
		}
		dxbc_ok = !g_depth_dxbc.empty();
	}

	depth_convert::pipelines fresh;
	if (!dxbc_ok || !depth_convert::create(dev, g_depth_dxbc, fresh, &nr_pipeline_log))
	{
		st.depth_failed = true;
		LOGW("DLSS-NR: the depth conversion pass could not be built (see the error above). "
		     "DLSSNR.Depth falls back to the GAME'S OWN r32_g8_typeless depth resource - README "
		     "gap 3 stands unmitigated - and depth_detect measures nothing, so DLSSNR.DepthInverted "
		     "keeps the configured value (README gap 4). Nothing else changes. This is a REAL build "
		     "failure and it is latched for the run.");
		return false;
	}

	depth_convert::destroy(dev, st.depth_conv);
	st.depth_conv = fresh;
	// The detector is armed from the ini here and only here, so a run that turns depth_detect off
	// never allocates a window and never issues a single atomic.
	st.depth_det = depth_convert::detector();
	st.depth_det.armed = g_cfg.depth_detect;
	LOGI("DLSS-NR: depth conversion pipeline created (cs_5_0 DXBC, [numthreads(16,16,1)]). "
	     "The game's depth is read through ITS OWN typed r32_float_x8_uint SRV and DeviceZ is "
	     "written VERBATIM into a private r32_float texture - nothing is linearised and nothing is "
	     "flipped. depth_detect=%d.", (int)g_cfg.depth_detect);
	return true;
}

/// The NR probe's statistics pass. Diagnostic, so EVERY failure here is soft: the probe turns
/// itself off and the render path is untouched. It is never built unless nr_probe=1.
static std::vector<uint8_t> g_probe_dxbc;
static bool                 g_probe_dxbc_tried = false;

static bool nr_build_probe_pipeline(device *dev, nr_state &st, const std::wstring &dir)
{
	if (dev == nullptr || g_cfg.nr_probe == 0 || st.probe.failed)
		return false;
	if (st.probe.ready)
		return true;

	bool dxbc_ok = false;
	{
		std::lock_guard<std::mutex> blob_lock(g_blob_mutex);
		if (!g_probe_dxbc_tried)
		{
			g_probe_dxbc_tried = true;
			nr_probe::build_shader(dir, g_probe_dxbc, &nr_pipeline_log);
		}
		dxbc_ok = !g_probe_dxbc.empty();
	}

	nr_probe::pipeline_set fresh;
	if (!dxbc_ok || !nr_probe::build(dev, g_probe_dxbc, fresh))
	{
		st.probe.failed = true;
		LOGW("DLSS-NR: the nr_probe statistics pass could not be built. The PROBE is off for this "
		     "run; the denoise, the codec and the write-back are all completely unaffected - this "
		     "is an instrument, not a render stage.");
		return false;
	}

	nr_probe::destroy(dev, st.probe);
	st.probe = fresh;
	st.probe_run = nr_probe::run_state();
	st.probe_run.active = true;
	LOGI("DLSS-NR: nr_probe ARMED - %u steps x %u frames, sweeping use_auto_mask and "
	     "local/skin structure strength. THE INI VALUES FOR THOSE THREE KEYS ARE IGNORED while "
	     "the probe runs. Park the camera and do not move: every step must see the same content "
	     "or the comparison is measuring the scene instead of the parameter.",
	     nr_probe::kSweepCount, g_cfg.nr_probe_frames);
	return true;
}

// The snippet is process-wide and is loaded at most once per successful attempt. Hoisted out of
// nr_init_device's function-local static for the same reason the blobs were: the reconfigure
// service has to be able to run this path when `enabled` goes 0 -> 1 or require_trampoline goes
// 1 -> 0, and a function-local static is unreachable from anywhere else.
static bool g_snippet_tried = false;

/// Load the snippet and ARM the deferred NGX initialisation. This is the SHIPPING STARTUP PATH,
/// not a new one - the same two steps nr_init_device takes at every normal launch - which is what
/// makes it safe to reach from the overlay.
///
/// NO RACE WITH THE RENDER THREAD, and the argument is short: every render-thread read of
/// g_snippet is downstream of `g_nr_armed`, which nr_try_run tests on its second line and which
/// is only ever set after nr_lazy_ngx_init has succeeded. This function refuses to run while
/// g_nr_armed is true, so the recording thread is provably at its NR_BAIL("not armed") return
/// while g_snippet is being written.
///
/// MAIN/PRESENT THREAD ONLY: ngx::load_snippet LoadLibraryW's a 166 MB module. The overlay's
/// tooltip says so, because the user will see the frame it costs.
static bool nr_arm_snippet(const std::wstring &dir, const char *why)
{
	if (g_nr_armed.load(std::memory_order_acquire))
		return true;   // already armed; nothing to do, and writing g_snippet now would race

	if (!g_snippet.available)
	{
		g_snippet_tried = true;
		if (!ngx::load_snippet(g_snippet, dir, ngx::spec_dlssnr(),
		                       overlay_ui::live_require_trampoline()))
		{
			// load_snippet has already called unload() on every failure path, so g_snippet is
			// clean and a later attempt (say, after the user unticks require_trampoline) starts
			// from nothing rather than from a half-loaded module.
			LOGI("DLSS-NR not available (%s): %s", why, g_snippet.not_available_reason.c_str());
			return false;
		}
		LOGI("DLSS-NR: loaded nvngx_dlssnr.dll%s (%s).",
			g_snippet.trampoline_module != nullptr
				? " and routed every call through remix_nvngx.dll"
				: " WITHOUT remix_nvngx.dll - require_trampoline=0; every GATED export is "
				  "expected to return 0xbad00002",
			why);
	}

	// The same store nr_init_device makes at every launch. nr_try_run's existing deferred
	// initialiser consumes it on the next dispatch and calls Init_Ext there, on the render
	// thread, with a device the game has finished building - which is the only place this title
	// tolerates it.
	g_nr_pending_init.store(true, std::memory_order_release);
	LOGI("DLSS-NR: NGX initialisation armed; it happens on the next render-thread dispatch.");
	return true;
}

/// Load the DLSS-SR snippet through the trampoline's SLOT B and arm the SAME deferred
/// initialiser. nvngx_dlss.dll is an independent 59 MB module with its own gated exports; slot B
/// exists because remix_nvngx.dll holds ONE set of forwarding pointers per slot and calling
/// RemixNgxTrampoline_SetSnippet twice would silently re-point DLSS-NR's calls at it.
///
/// MAIN THREAD, AT LAUNCH, AND NOWHERE ELSE - which is a narrower contract than nr_arm_snippet's
/// and is the reason `dlss_sr` is a launch-time arm rather than a live one. nr_arm_snippet may be
/// called from nr_service_reconfigure because the ONLY render-thread reader of g_snippet is
/// downstream of g_nr_armed, which that function refuses to run under. No such argument exists
/// here: sr_try_run reads g_sr_snippet on the recording thread whenever the branch is taken, and
/// the SR half of nr_lazy_ngx_init is a one-shot that has already run by the time a checkbox can
/// be clicked. Making the ON direction live would mean a SECOND NVSDK_NGX_D3D12_Init_Ext in the
/// session, and the one measured fact this project has about Init_Ext's fragility is that it can
/// HANG (see the header of nr_lazy_ngx_init). So the overlay reports 0 -> 1 as RELAUNCH REQUIRED,
/// with that reason, instead of claiming an arm it cannot make. The 1 -> 0 direction is fully
/// live and needs none of this.
///
/// It arms g_nr_pending_init rather than a one-shot of its own because nr_lazy_ngx_init does BOTH
/// halves - without this store a dlss_nr=0, dlss_sr=1 run would load nvngx_dlss.dll and then
/// never call into it.
static bool nr_arm_sr_snippet(const std::wstring &dir, const char *why, const char *asked_for)
{
	if (g_sr_snippet.available)
		return true;

	if (!ngx::load_snippet(g_sr_snippet, dir, ngx::spec_dlsssr(),
	                       overlay_ui::live_require_trampoline()))
	{
		// load_snippet has already called unload() on every failure path, so g_sr_snippet is clean.
		LOGE("DLSS-SR not available (%s): %s", why, g_sr_snippet.not_available_reason.c_str());
		// `asked_for` rather than a hardcoded "dlss_sr=1". The caller already computes which key
		// brought it here and passes it in `why`; this second line used to name dlss_sr
		// unconditionally and so contradicted the line immediately above it on every
		// dlss_sr=0, dlss_chain=1 run - the one configuration the chain is tested on.
		LOGE("DLSS-SR: %s was asked for and cannot be honoured. The game renders exactly as "
		     "it would with the add-on unloaded (or with DLSS-NR alone, if dlss_nr=1).", asked_for);
		return false;
	}
	LOGI("DLSS-SR: loaded nvngx_dlss.dll%s (%s).",
		g_sr_snippet.trampoline_module != nullptr
			? " and routed every call through remix_nvngx.dll's slot B"
			: " WITHOUT remix_nvngx.dll - require_trampoline=0; every GATED export is expected to "
			  "return 0xbad00002",
		why);

	g_nr_pending_init.store(true, std::memory_order_release);
	LOGI("DLSS-SR: NGX initialisation armed; it happens on the next render-thread dispatch, in the "
	     "same nr_lazy_ngx_init call that brings up DLSS-NR.");
	return true;
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
		if (st.out_uav.handle    != 0) { dev->destroy_resource_view(st.out_uav);    st.out_uav    = { 0 }; }
		if (st.proxy_srv.handle  != 0) { dev->destroy_resource_view(st.proxy_srv);  st.proxy_srv  = { 0 }; }
		if (st.proxy_uav.handle  != 0) { dev->destroy_resource_view(st.proxy_uav);  st.proxy_uav  = { 0 }; }
		if (st.orig_srv.handle   != 0) { dev->destroy_resource_view(st.orig_srv);   st.orig_srv   = { 0 }; }
		if (st.result_uav.handle != 0) { dev->destroy_resource_view(st.result_uav); st.result_uav = { 0 }; }
		if (st.mvec_uav.handle   != 0) { dev->destroy_resource_view(st.mvec_uav);   st.mvec_uav   = { 0 }; }
		if (st.depth_uav.handle  != 0) { dev->destroy_resource_view(st.depth_uav);  st.depth_uav  = { 0 }; }

		if (st.out_tex.handle    != 0) { dev->destroy_resource(st.out_tex);    st.out_tex    = { 0 }; }
		if (st.proxy_tex.handle  != 0) { dev->destroy_resource(st.proxy_tex);  st.proxy_tex  = { 0 }; }
		if (st.orig_tex.handle   != 0) { dev->destroy_resource(st.orig_tex);   st.orig_tex   = { 0 }; }
		if (st.result_tex.handle != 0) { dev->destroy_resource(st.result_tex); st.result_tex = { 0 }; }
		if (st.mvec_tex.handle   != 0) { dev->destroy_resource(st.mvec_tex);   st.mvec_tex   = { 0 }; }
		if (st.depth_tex.handle  != 0) { dev->destroy_resource(st.depth_tex);  st.depth_tex  = { 0 }; }
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

	// The mvec pass's per-resolution state. mvec_failed is NOT cleared, for exactly the same
	// reason codec_failed is not: it records that the SHADER could not be built, which a
	// resolution change cannot undo. The View-CB layout is per-resolution because discovery
	// validates against the render extent, and the cached matrix belongs to that layout.
	st.mvec_ok = false;
	st.mvec_tex_failed = false;
	st.mvec_eval_fail_streak = 0;
	st.depth_eval_fail_streak = 0;
	st.view_layout = ue4jitter::layout{};
	st.view_layout_ok = false;
	st.view_layout_failed = false;
	st.view_discover_tries = 0;
	st.clip_ok = false;
	st.clip_fail_streak = 0;
	st.view_size[0] = st.view_size[1] = 0.0f;
	st.view_size_measured = false;
	// The guide is about to be a different resource; the reset latch must not compare against a
	// handle from the old resolution, whose address UE's pool can hand back for something else.
	st.mvec_bound_res = 0;

	// The depth pass's per-resolution state. depth_failed is NOT cleared, for exactly the same
	// reason mvec_failed is not: it records that the SHADER could not be built, which a resolution
	// change cannot undo.
	st.depth_ok = false;
	st.depth_tex_failed = false;

	// THE MEASURED CONVENTION IS DELIBERATELY *NOT* CLEARED. It is a property of the renderer's
	// projection - UE builds the same reversed-Z matrices at every resolution - so a verdict taken
	// at 1920x1080 is exactly as true at 2560x1440, and re-measuring would only give the run a
	// second chance to disagree with itself. What IS cleared is the IN-FLIGHT window: its copy was
	// recorded on a command list against a texture this function has just destroyed, and half a
	// window's texels must never be allowed to settle the verdict (depth_convert.hpp says why).
	st.depth_det.frames_in_window = 0;
	st.depth_det.awaiting_copy    = false;
	st.depth_det.copy_age         = 0;

	// Chain mode's per-geometry state. chain_nr_off is RUN-latched and deliberately NOT cleared,
	// exactly like sr_latched_off: it records that NGX refused the evaluate, which a resolution
	// change cannot undo.
	st.chain_active = false;
	st.chain_nr_fail_streak = 0;

	st.out_w = st.out_h = 0;
	st.out_fmt = format::unknown;
	st.neural_fmt = format::unknown;
	st.guide_w = st.guide_h = 0;
	st.need_reset = true;
	st.feature_failed = false;
	st.evaluate_count = 0;

	// ---- DLSS SUPER RESOLUTION ----------------------------------------------------------------
	// The queue was idled at the top of this function, which is the precondition for releasing an
	// NGX feature that in-flight work may still reference. OutWidth/OutHeight are latched at
	// CREATE and there is no evaluate-time output extent, so a geometry change means a full
	// rebuild and there is nothing finer-grained to preserve. Every one of these is a no-op when
	// dlss_sr=0: the handle is null, both textures are null, and the jitter source is already
	// default-constructed.
	dlss_sr::release_feature(g_sr_snippet, st.sr_feat);
	dlss_sr::destroy_resources(dev, st.sr_res, &sr_log);
	st.sr_render_w = st.sr_render_h = st.sr_out_w = st.sr_out_h = 0;
	st.sr_feat.failed = false;          // per-geometry, exactly like feature_failed above
	st.sr_eval_fail_streak = 0;
	// The one-shot ERROR latches that PAIR with the two counters above. Clearing the latch while
	// leaving the "printed once" flag set would make the retry at the new geometry silent: the
	// create would fail again, or the evaluate streak rebuild, with nothing in the log - and the
	// message the previous failure printed promises exactly this retry. The INFO latches
	// (logged_preset and the host's own) are deliberately left alone; only the errors reset.
	st.sr_feat.logged_create_fail = false;
	st.sr_feat.logged_eval_fail   = false;
	st.sr_feat.need_reset = true;
	// The jitter layout is per-resolution: discovery validates the view rect against the render
	// extent, so a layout accepted at one resolution says nothing about the next.
	st.sr_jitter = dlss_sr::jitter_source();
	// sr_latched_off and sr_mvec_rejected are RUN-latched and deliberately NOT cleared: they
	// record that NGX refused something, which a resolution change cannot undo.

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
	//
	// CHAIN MODE WANTS NEITHER. The decode's `original` there is the game's own t5 SRV, consumed
	// inside the event and never stored - the same borrow the motion-vector decode already makes
	// of the game's velocity and depth descriptors, and it keeps the standing "nothing here ever
	// CREATES a view on a resource the game owns" rule intact for the same reason. And the
	// temporal-feedback fix does not apply: it exists to undo the DLSS-NR copy-back writing a
	// denoised image into a buffer UE extracts as TAA history, and chain mode performs no
	// copy-back at all (its result is at the render extent; u0 is at the output extent).
	const bool want_orig  = !st.chain_active &&
	                        (want_codec || (g_cfg.history_restore && g_cfg.copy_back));

	// ---- the DECODED MOTION-VECTOR TARGET -----------------------------------------------------
	//
	// FIRST, deliberately: everything below this point has early `return`s (the codec's at the
	// "want_codec" test and at !orig_ok), and a block placed after them would be silently skipped
	// whenever the codec is off - which is exactly the configuration in which the motion-vector
	// decode still has to work.
	//
	// A failure here is NOT fatal and is NOT an error for the pass: DLSSNR.MVec falls back to the
	// game's raw encoded velocity with the derived grid scale, i.e. bit-for-bit what the add-on
	// did before this feature existed.
	//
	// NOT IN CHAIN MODE. There the two networks read ONE guide - the one DLSS-SR's own decode
	// writes, at the render extent, which chain mode has made equal to this extent - so a second
	// identical r16g16_float texture that nothing would ever read is pure waste.
	if (!st.chain_active &&
	    g_cfg.mvec_decode && st.mvec.ok && !st.mvec_failed && !st.mvec_ok && !st.mvec_tex_failed)
	{
		const resource_desc d(w, h, 1, 1, format::r16g16_float, 1, memory_heap::default_,
			resource_usage::unordered_access | resource_usage::shader_resource);
		const resource_view_desc v(resource_view_type::texture_2d, format::r16g16_float, 0, 1, 0, 1);

		resource t = { 0 };
		// EXACTLY resource_usage::unordered_access on the view: create_resource_view switches on
		// the whole value (see the note beside the codec's views below).
		if (!dev->create_resource(d, nullptr, resource_usage::unordered_access, &t) || t.handle == 0 ||
		    !dev->create_resource_view(t, resource_usage::unordered_access, v, &st.mvec_uav))
		{
			if (t.handle != 0) { dev->destroy_resource(t); }
			if (st.mvec_uav.handle != 0) { dev->destroy_resource_view(st.mvec_uav); st.mvec_uav = { 0 }; }
			st.mvec_tex_failed = true;
			if (!st.logged_mvec_tex_fail)
			{
				st.logged_mvec_tex_fail = true;
				LOGE("DLSS-NR: could not create the %ux%u r16g16_float motion-vector target. The "
				     "decode is OFF for this resolution and DLSSNR.MVec falls back to the GAME'S "
				     "RAW ENCODED velocity buffer with the derived grid scale - i.e. EXACTLY the "
				     "pre-decode behaviour, README gap 2. Nothing else changes.", w, h);
			}
		}
		else
		{
			st.mvec_tex = t;
			st.mvec_ok  = true;
			LOGI("DLSS-NR: motion-vector target ready, %ux%u r16g16_float (UAV, "
			     "ID3D12Resource=0x%llx). It rests in UNORDERED_ACCESS and is handed to NGX in "
			     "SHADER_RESOURCE_NON_PIXEL.", w, h, (unsigned long long)t.handle);
		}
	}

	// ---- the CONVERTED DEPTH TARGET -----------------------------------------------------------
	//
	// SECOND, and for the same reason the motion-vector target is first: everything below this
	// point has early `return`s at the "want_codec" test and at !orig_ok, and a block placed after
	// them would be silently skipped whenever the codec is off - which is exactly a configuration
	// in which the depth conversion still has to work.
	//
	// A failure here is NOT fatal and is NOT an error for the pass: DLSSNR.Depth falls back to the
	// game's own r32_g8_typeless resource, i.e. bit-for-bit what the add-on did before this feature
	// existed (README gap 3).
	//
	// THE PASS IS ALLOCATED WHENEVER EITHER KEY WANTS IT. depth_convert=0 with depth_detect=1 still
	// runs it until the convention is settled and then stops - the measurement is the more valuable
	// of the two and must not be hostage to the binding A/B. Once the detector is done and
	// depth_convert is off, nothing here allocates and nothing dispatches.
	//
	// NOT IN CHAIN MODE. Chain mode binds the game's own depth to DLSSNR.Depth and this pass does
	// not cover it - see the SCOPE note in addon_config.hpp - so a texture nothing would ever read
	// is pure waste there.
	if (!st.chain_active &&
	    (g_cfg.depth_convert || (g_cfg.depth_detect && !st.depth_det.done)) &&
	    st.depth_conv.ok && !st.depth_failed && !st.depth_ok && !st.depth_tex_failed)
	{
		const resource_desc d(w, h, 1, 1, format::r32_float, 1, memory_heap::default_,
			resource_usage::unordered_access | resource_usage::shader_resource);
		const resource_view_desc v(resource_view_type::texture_2d, format::r32_float, 0, 1, 0, 1);

		resource t = { 0 };
		// EXACTLY resource_usage::unordered_access on the view: create_resource_view switches on
		// the whole value (see the note beside the codec's views below).
		if (!dev->create_resource(d, nullptr, resource_usage::unordered_access, &t) || t.handle == 0 ||
		    !dev->create_resource_view(t, resource_usage::unordered_access, v, &st.depth_uav))
		{
			if (t.handle != 0) { dev->destroy_resource(t); }
			if (st.depth_uav.handle != 0) { dev->destroy_resource_view(st.depth_uav); st.depth_uav = { 0 }; }
			st.depth_tex_failed = true;
			if (!st.logged_depth_tex_fail)
			{
				st.logged_depth_tex_fail = true;
				LOGE("DLSS-NR: could not create the %ux%u r32_float depth target. The conversion is "
				     "OFF for this resolution and DLSSNR.Depth falls back to the GAME'S OWN "
				     "r32_g8_typeless resource - i.e. EXACTLY the pre-conversion behaviour, README "
				     "gap 3 - and depth_detect measures nothing this resolution. Nothing else "
				     "changes.", w, h);
			}
		}
		else
		{
			st.depth_tex = t;
			st.depth_ok  = true;
			LOGI("DLSS-NR: depth target ready, %ux%u r32_float (UAV, ID3D12Resource=0x%llx). It "
			     "rests in UNORDERED_ACCESS and is handed to NGX in SHADER_RESOURCE_NON_PIXEL.",
			     w, h, (unsigned long long)t.handle);
		}
	}

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
	if (!st.chain_active && !st.orig_ok)
		return;   // the decode has no `original` to add onto; orig_failed already said why

	const char *stage = nullptr;

	// The proxy is DLSSNR.Color, so it has to be at the colour (output) extent: the snippet
	// validates the Color rect against the Output rect and rejects the evaluate outright when
	// their dimensions differ. FP16 is what the RenoDX deployment uses for the same surface, and
	// it comfortably holds the [0,1] sRGB-encoded values the network expects.
	const resource_desc proxy_desc(w, h, 1, 1, format::r16g16b16a16_float, 1, memory_heap::default_,
		resource_usage::unordered_access | resource_usage::shader_resource);
	// In chain mode result_tex is not a copy source at all - it is DLSS-SR's COLOUR INPUT, read by
	// the snippet as an SRV - so shader_resource joins the usage set. create_resource asserts that
	// usage is a superset of every state the resource is ever transitioned to, and chain mode
	// transitions this one to shader_resource_non_pixel. The bit is added ONLY on that path, so
	// the DLSS-NR allocation is unchanged.
	const resource_desc result_desc(w, h, 1, 1, st.out_fmt, 1, memory_heap::default_,
		st.chain_active
			? (resource_usage::unordered_access | resource_usage::copy_source | resource_usage::shader_resource)
			: (resource_usage::unordered_access | resource_usage::copy_source));

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
	// No orig_tex in chain mode, so no view on it: the decode reads the game's t5 SRV instead.
	else if (!st.chain_active &&
	         !dev->create_resource_view(st.orig_tex,   resource_usage::shader_resource,  tex_view,   &st.orig_srv))
		stage = "create_resource_view(original SRV)";
	else if (!dev->create_resource_view(st.out_tex,    resource_usage::shader_resource,  neural_view, &st.out_srv))
		stage = "create_resource_view(neural SRV)";
	else if (g_cfg.nr_probe != 0 &&
	         !dev->create_resource_view(st.out_tex,    resource_usage::unordered_access, neural_view, &st.out_uav))
		stage = "create_resource_view(neural UAV, probe only)";
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
		     "be the deliberate neural_format override (1 = r10g10b10a2_unorm, what both "
		     "reference implementations use; 2 = follow out_fmt). At neural_format=0 it would "
		     "be impossible, because nr_ensure_output forces r16g16b16a16_float there.",
		     probe::format_name(st.neural_fmt));
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
// THE PREDICATE BELOW IS NO LONGER PROCESS-CONSTANT, AND THIS PARAGRAPH USED TO SAY IT WAS.
//
// It read: "g_cfg is loaded once in nr_init_device and there is no overlay or reload, and the
// codec pipelines are created in nr_init_device too, so st.codec.ok and st.codec_failed are both
// settled before any dispatch reaches here." Every clause of that is now false - hdr_codec is a
// live setting, the pipelines can be built by nr_service_reconfigure, and g_cfg is written once
// per pass by overlay_ui::begin_pass. Leaving the sentence standing would have been worse than
// the bug it described, because the next reader would build on it.
//
// WHAT ACTUALLY HOLDS NOW, and it is enough. `codec_wanted` is read twice in this function - once
// to choose `neural` and once, through st.neural_fmt, when nr_ensure_aux creates out_srv - and
// both reads happen inside ONE call, under st->mutex, against the g_cfg the pass snapshotted on
// its first line. So it still cannot disagree with itself within a pass.
//
// ACROSS passes it CAN change, and that is the point: a live hdr_codec toggle raises the rebuild
// rung, the service releases out_tex and zeroes out_w/out_h, and the next accepted dispatch
// re-enters this function on the CREATE branch below and re-decides `neural` against the new
// value. The alternative - flipping the flag with out_tex still alive - is what the copy-back's
// format guard would then silently reject, which is why a live change goes through the teardown
// and not through the `if (st.out_tex.handle != 0)` fast path above.
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
		//
		// fetch_or, NOT a store: a user reconfigure can land in the same frame as a resolution
		// change, and a store here would silently discard whatever the overlay had asked for.
		st.pending_work.fetch_or(kTeardown, std::memory_order_relaxed);
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
	// neural_format selects what the codec-on target actually is. See addon_config.hpp: the
	// historical r16g16b16a16_float is the configuration in which NGX returns Success and writes
	// NOTHING (measured two ways), while both reference implementations use r10g10b10a2_unorm.
	const format codec_neural =
		  (g_cfg.neural_format == 1) ? format::r10g10b10a2_unorm
		: (g_cfg.neural_format == 2) ? fmt
		                             : format::r16g16b16a16_float;
	const format neural = codec_wanted ? codec_neural : fmt;

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

	// PARITY WITH THE REFERENCE ADD-ON. renodx writes PerfQualityValue = 0 at create
	// [BIN 0x18000dfd5], and unlike DLSS.Feature.Create.Flags - which has ZERO occurrences in
	// nvngx_dlssnr.dll and is therefore inapplicable to feature 18 - "PerfQualityValue" IS an
	// exact string in this snippet. Feature 18 does not upscale, so this is almost certainly
	// inert; it is written anyway as zero-cost insurance against the snippet reading a key we
	// leave absent. 0 = MaxPerf, which is the reference's value.
	ngx::set_i32(p, ngx::kParamPerfQualityValue, 0);

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

// --------------------------------------------------------------------------------------------
// Dump one armed evaluate's worth of snippet-side Gets.
//
// THE FIVE KEYS ARE PRINTED WHETHER OR NOT THEY WERE READ, and that is the entire point: the
// interesting outcome for a control that does nothing is the key that never appears, and a
// listing that only shows what happened cannot show an absence. Everything else is summarised.
//
// Measured against the deployed snippet (nvngx_dlssnr.dll md5 eea91faf55a8993656c66815f0497b3b),
// NVSDK_NGX_D3D12_EvaluateFeature reaches exactly one function that reads these keys
// [BIN 0x1800159c0 -> 0x180018620 -> 0x180019f30], and that function issues 60 Gets per evaluate
// (counted by walking every guard-dispatched vtable call in 0x180019f30..0x18001ac2a).
// A trace that reports far fewer, or that reports these five as absent, is the finding.
static void nr_log_get_trace(const nr_state &st, ngx::parameter_block &p)
{
	static const char *const kWatched[] = {
		ngx::kParamIntensity,
		ngx::kParamLocalToneStrength,
		ngx::kParamLocalStructureStrength,
		ngx::kParamSkinStructureStrength,
		ngx::kParamUseAutoMask,
		ngx::kParamStyle,
		ngx::kParamControlMask,   // gates BOTH structure strengths inside the snippet
	};

	const int seen   = p.get_trace.seen();
	const int stored = p.get_trace.stored();
	LOGI("DLSS-NR: NGX getter trace, evaluate #%llu - the snippet issued %d Get call(s) against "
	     "our parameter block%s. This records the CALLEE's reads; it is evidence the value "
	     "REACHED the network, NOT that the network acted on it.",
	     (unsigned long long)st.evaluate_count + 1, seen,
	     (seen > stored) ? " (listing truncated)" : "");

	for (const char *want : kWatched)
	{
		const ngx::parameter_block::trace::record *found = nullptr;
		for (int i = 0; i < stored; ++i)
		{
			if (std::strcmp(p.get_trace.records[i].key, want) == 0)
			{
				found = &p.get_trace.records[i];
				break;
			}
		}
		if (found == nullptr)
		{
			LOGW("DLSS-NR:   %-32s NOT READ by the snippet during this evaluate. A control whose "
			     "key the snippet never asks for cannot be live, whatever we write.", want);
		}
		else if (!found->hit)
		{
			LOGE("DLSS-NR:   %-32s %s -> MISS. The snippet asked and our block had no such key, so "
			     "it substituted its own fallback. This is OUR bug: either the key is not written "
			     "on the evaluate path, or it is spelled differently from the snippet's.",
			     want, ngx::get_slot_name(found->slot));
		}
		else if (found->slot == 8 || found->slot == 9 || found->slot == 10)
		{
			LOGI("DLSS-NR:   %-32s %s -> HIT, stored as %s, returned %p.",
			     want, ngx::get_slot_name(found->slot),
			     ngx::value_kind_name(found->kind), found->pointer);
		}
		else
		{
			LOGI("DLSS-NR:   %-32s %s -> HIT, stored as %s, returned %.4f.",
			     want, ngx::get_slot_name(found->slot),
			     ngx::value_kind_name(found->kind), found->numeric);
		}
	}

	// Every key the snippet asked for and we did not have. These are the ones where it falls back
	// to an internal default, and the list is short enough to print in full.
	int misses = 0;
	for (int i = 0; i < stored; ++i)
		if (!p.get_trace.records[i].hit)
			++misses;
	if (misses > 0)
	{
		LOGI("DLSS-NR:   %d of %d Get(s) MISSED. Each is a key the snippet reads and this add-on "
		     "does not write, so the snippet used its own fallback:", misses, stored);
		for (int i = 0; i < stored; ++i)
			if (!p.get_trace.records[i].hit)
				LOGI("DLSS-NR:     miss  %-40s %s",
				     p.get_trace.records[i].key, ngx::get_slot_name(p.get_trace.records[i].slot));
	}
}

// The parameter block outlives every evaluate - it is allocated once and reused - so an optional
// resource that is not bound THIS frame must be cleared explicitly. Leaving the previous frame's
// pointer there both dangles and, for the control mask specifically, keeps the snippet forcing
// UseAutoMask to 0 (which kills BOTH structure strengths) long after the mask went away.
//
// The null is written through the ID3D12Resource* slot, not the void* slot, so the parameter
// map's type tag stays consistent with the bound case.
//
// MEASURED, because an earlier note here rested on an assumption that turned out to be wrong.
// It was suspected that writing DLSSNR.ControlMask as a PRESENT-BUT-NULL entry - which renodx
// never does, it omits the key entirely - would read back as Success and so make the snippet
// treat a mask as bound, forcing UseAutoMask off and bypassing both structure strengths. It does
// not, and the reason is that the snippet zeroes the field before it ever reads the key:
//
//   [BIN 0x180019f7d]  movups xmmword ptr [rdi+0x60], xmm0    ; xmm0 = 0, on entry, every call
//   [BIN 0x18001a3f0]  lea    r8,  [rdi+0x60]                 ; out-pointer for the mask
//   [BIN 0x18001a3f4]  lea    rdx, [rip+0x94a15]              ; "DLSSNR.ControlMask"
//   [BIN 0x18001a3fe]  mov    rax, qword ptr [rax+0x40]       ; slot 8, Get(const char*, void**)
//   [BIN 0x18001a40d]  cmp    eax, 0xbad00000
//   [BIN 0x18001a412]  je     0x18001a4c6                     ; a MISS skips the whole sub-block
//   [BIN 0x18001aa4b]  cmp    qword ptr [rdi+0x60], 0
//   [BIN 0x18001aa50]  je     0x18001aa59                     ; NULL -> the force-off is skipped
//   [BIN 0x18001aa52]  mov    dword ptr [rdi+0xf0], r12d      ; non-null -> UseAutoMask := 0
//
// Absent leaves the zero in place; present-but-null stores a zero over a zero. Both reach
// 0x18001aa4b with [rdi+0x60] == 0 and neither takes the force-off. The two are exactly
// equivalent, so this write is safe - and removing it to match renodx would change nothing.
// tools/ngx_paramblock_selftest.cpp checks both, and checks that a genuinely non-null mask DOES
// trigger the bypass, so the mechanism stays covered.
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

//
// THE EXTENT TEST, AND WHY IT IS NOT ONE TEST.
//
// For DLSS-NR the output UAV is at the COLOUR SRV's extent, because that pass does not upscale.
// Equality there is exact and load-bearing, and it is what ships. For DLSS-SR it is WRONG: `colour`
// is srv_colour = t5 = InputSceneColor at the RENDER resolution and u0 is at the OUTPUT resolution,
// so under any real upscale the correct answer is the one the equality test rejects and the pass
// never runs at all.
//
// So SR passes the output extent it derived from the dispatch's own group counts
// (TemporalAA.cpp:958 dispatches GetGroupCount(PracticableDestRect.Size(), GTemporalAATileSizeX),
// and GetGroupCount is DivideAndRoundUp, so OutW is in (tile*gx - tile, tile*gx]), and a candidate
// is accepted when it is AT LEAST that big in both axes. "At least" rather than "exactly" because
// QuantizeSceneBufferSize rounds the TEXTURE extent up to a multiple of 4 while the value derived
// from group counts is the VIEW RECT - the two are equal at every ratio this add-on targets, and
// where they are not, the texture is the larger.
//
// want_out_w/h == 0 selects the DLSS-NR rule, unchanged. That is what dlss_sr=0 passes, so this
// function's behaviour with SR off is bit-identical to before.
static bool nr_pick_output_uav(device *dev, nr_state &st,
                               const std::vector<probe::resolved_uav> &uavs,
                               const nr_view_info &colour,
                               const resource *inputs, size_t input_count,
                               uint32_t want_out_w, uint32_t want_out_h,
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
		if (want_out_w == 0 || want_out_h == 0)
		{
			// DLSS-NR: the output is at the colour extent, exactly.
			if (c.info.w != colour.w || c.info.h != colour.h)
			{
				c.why_not = "extent differs from the colour SRV's";
				list.push_back(c);
				continue;
			}
		}
		else
		{
			// DLSS-SR: the output is at the OUTPUT extent, which is >= the colour extent.
			if (c.info.w < want_out_w || c.info.h < want_out_h)
			{
				c.why_not = "extent is smaller than the output extent derived from the group counts";
				list.push_back(c);
				continue;
			}
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
// THE VIEW UNIFORM BUFFER -> View.ClipToPrevClip.
//
// This is the input the SPARSE half of the motion-vector decode needs, and getting it wrong is
// silent: a wrong row reprojects the whole static world through an unrelated matrix and produces
// confident, coherent, completely incorrect motion. So it is read the careful way.
//
// WHY A CPU READBACK AND NOT A ROOT CBV
//   Binding the game's own b1 straight to our shader is one line cheaper and it is the wrong
//   trade here. It cannot run a content signature, so the ClipToPrevClip row would be TRUSTED
//   rather than validated; a root CBV carries no size, so a bad row index reads unrelated live
//   data; and the documented worst case - landing on $Globals at b0, which accumulates a
//   persistent shadow array and therefore carries STALE BYTES from earlier passes - produces
//   plausible numbers from the wrong frame with no diagnostic at all
//   (ue4_jitter.hpp:1149-1152). A 64-byte read per frame buys, instead:
//     * a 26-constraint content signature that finds the ViewToClip / ViewToClipNoAA pair and
//       derives the anchor from it (ue4_jitter.hpp), and
//     * anchor + 94 cross-checked against this project's OWN, completely independent,
//       DXBC-instruction-analysis row (shader_detect.hpp FindShaderInfo). Two derivations from
//       two different kinds of evidence, and they must AGREE.
//   Both give 122 for STRAY.
//
// WHICH ROOT PARAMETER. Structurally, never by index - the probe observed the View CB at root
// parameter 3 AND at 4. On D3D12 every CBV is a ROOT DESCRIPTOR, which ReShade reports as
// pipeline_layout_param_type::push_descriptors carrying a single inline descriptor_range whose
// dx_register_index is D3D12_ROOT_DESCRIPTOR::ShaderRegister. So the View UB is the parameter
// whose single range is (constant_buffer, space 0, register == the bN slot the shader's own
// dcl_constant_buffer census named as its LARGEST cbuffer). Using the DXBC-derived slot rather
// than a hardcoded b1 is what keeps this honest if a permutation ever numbers it differently;
// b1 is only the fallback.
//
// pool_map_cache is DELIBERATELY NOT USED. It caches a raw ID3D12Resource*, which obliges the
// caller to register addon_event::destroy_resource and call forget() there or a
// destroyed-and-reallocated pool at the same address is a use-after-free
// (ue4_jitter.hpp:1164-1169). This add-on registers no such event. read_view_cb caches nothing,
// so that entire hazard class simply does not exist, for one Map/Unmap per frame - which under
// vkd3d is a pointer hand-back with no vkMapMemory and no refcount.
//
// [ASSUMED] ue4_jitter.hpp's D3D12 layer has never run on hardware. Every return value it gives
// is treated as untrusted here: a false anywhere only ever walks down the fallback ladder.
// --------------------------------------------------------------------------------------------
static bool nr_find_view_cb(probe::device_shadow &sh, const probe::pipe_bindings &b,
                            int32_t want_register, buffer_range &out)
{
	std::shared_lock<std::shared_mutex> lock(sh.mutex);
	const auto it = sh.layouts.find(b.layout.handle);
	if (it == sh.layouts.end())
		return false;

	const std::vector<probe::layout_param> &params = it->second;
	for (size_t p = 0; p < params.size() && p < b.root_cbvs.size(); ++p)
	{
		const probe::layout_param &lp = params[p];
		// A root CBV: not a bindable table, exactly one inline range, of constant_buffer type.
		if (lp.is_table || lp.ranges.size() != 1)
			continue;
		if (lp.ranges[0].type != descriptor_type::constant_buffer)
			continue;
		if (lp.ranges[0].dx_register_space != 0)
			continue;
		if (static_cast<int32_t>(lp.ranges[0].dx_register_index) != want_register)
			continue;
		if (!b.root_cbvs[p].valid || b.root_cbvs[p].range.buffer.handle == 0)
			continue;

		out = b.root_cbvs[p].range;
		return true;
	}
	return false;
}

// Reads four consecutive float4 CB rows into m[16], row-major, NOT transposed. m[4*r + c] is
// FMatrix::M[r][c] - which is what the shader's mvMulClipToPrevClip consumes, and what UE's
// mul(v, M) means for a row-major-packed matrix (/Zpr, D3DShaderCompiler.cpp:947-949).
static bool nr_read_clip_rows(ID3D12Resource *pool, uint64_t cb_offset, uint32_t row, float m[16])
{
	const uint64_t byte_off = cb_offset + static_cast<uint64_t>(row) * ue4jitter::kBytesPerRow;
	return ue4jitter::read_view_cb(pool, byte_off, 4u * ue4jitter::kBytesPerRow, m);
}

// Returns true when st.clip_to_prev holds a matrix worth reprojecting through. Every false path
// has already logged its reason exactly once and leaves the caller on the fallback ladder.
static bool nr_update_clip_to_prev_clip(device *dev, probe::device_shadow &sh, const probe::cmd_shadow &cs,
                                        nr_state &st, const shader_record &shader,
                                        uint32_t taa_w, uint32_t taa_h)
{
	// LATCHED FOR THIS RESOLUTION, AND THE LATCH IS PERMANENT - so it must land on the documented
	// ladder rung (raw), not on the last good matrix. Returning st.clip_ok here meant that a latch
	// which fired AFTER a good frame re-enabled full mode on the very next frame and reprojected
	// the whole static world through a FROZEN matrix for the rest of the run - confident, coherent
	// and completely wrong, which is exactly what this feature must never ship, and invisible
	// because the once-only message said the reconstruction was off while it was still running.
	// The bounded last-good-matrix behaviour lives ONLY on the transient per-frame read failure
	// below, which does not set this latch until it has failed 30 frames running.
	if (st.view_layout_failed)
		return false;

	// The bN slot of the shader's largest declared constant buffer IS the View uniform buffer
	// (FindLargestCBufferDeclaration). Fall back to b1 only when the census did not resolve it.
	const int32_t want_reg = (shader.info.global_buffer_register_index >= 0)
		? shader.info.global_buffer_register_index : 1;

	buffer_range br = {};
	if (!nr_find_view_cb(sh, cs.cmp, want_reg, br))
	{
		st.view_layout_failed = true;
		st.clip_ok = false;   // permanent latch: drop to raw, never freeze the last matrix
		if (!st.logged_mvec_no_viewcb)
		{
			st.logged_mvec_no_viewcb = true;
			LOGW("DLSS-NR: no root CBV at b%d was captured for this dispatch, so View.ClipToPrevClip "
			     "cannot be read and the CAMERA-MOTION RECONSTRUCTION is off. With "
			     "mvec_reconstruct=1 the whole motion-vector decode falls back to today's raw "
			     "encoded velocity rather than hand DLSS zero motion for every static pixel; with "
			     "mvec_reconstruct=0 the decode still runs and invalid texels stay zero, which is "
			     "what you asked for. This message is printed once.", want_reg);
		}
		return false;
	}

	auto *const pool = reinterpret_cast<ID3D12Resource *>(br.buffer.handle);

	// buffer_range.size is hard-coded to UINT64_MAX by ReShade and carries NO information
	// (d3d12_command_list.cpp:645-647). The real bound comes from the resource itself - and it is
	// UE's 8 MiB fast-constant upload POOL, not the constant buffer, so `offset` is where the View
	// CB starts inside it and nothing outside [offset, offset + block) may be read.
	const resource_desc pd = probe::abi_get_resource_desc(dev, br.buffer);
	if (pd.type != resource_type::buffer || pd.buffer.size == 0 || br.offset >= pd.buffer.size)
	{
		st.view_layout_failed = true;
		st.clip_ok = false;   // permanent latch: drop to raw, never freeze the last matrix
		if (!st.logged_mvec_no_viewcb)
		{
			st.logged_mvec_no_viewcb = true;
			LOGW("DLSS-NR: the b%d root CBV does not resolve to a readable buffer (type=%d "
			     "size=%llu offset=%llu), so View.ClipToPrevClip cannot be read and the camera-motion "
			     "reconstruction is off. This message is printed once.",
			     want_reg, (int)pd.type, (unsigned long long)pd.buffer.size,
			     (unsigned long long)br.offset);
		}
		return false;
	}

	const uint64_t avail = pd.buffer.size - br.offset;

	// ---------------------------------------------------------------- discovery, once per resolution
	if (!st.view_layout_ok)
	{
		// Bounded so a wedged discovery cannot Map the pool once per frame forever.
		if (++st.view_discover_tries > 8)
		{
			st.view_layout_failed = true;
			st.clip_ok = false;   // permanent latch: drop to raw, never freeze the last matrix
			return false;
		}

		const uint32_t want_bytes = (avail < ue4jitter::kViewCbConstantBytes)
			? static_cast<uint32_t>(avail) : ue4jitter::kViewCbConstantBytes;

		std::vector<uint8_t> cb(want_bytes);
		if (!ue4jitter::read_view_cb(pool, br.offset, want_bytes, cb.data()))
			return false;   // transient; retried up to the bound above

		ue4jitter::config c;
		c.expected_render_width      = taa_w;
		c.expected_render_height     = taa_h;
		c.expected_is_texture_extent = true;
		// The probe's INDEPENDENT answer, from instruction analysis of this very shader. Supplying
		// it arms check::clip_row_agrees.
		c.dxbc_clip_to_prev_clip_row = shader.info.clip_to_prev_clip_start_index;
		// We want the ANCHOR, not the jitter. discover() sets row_clip_to_prev_clip unconditionally
		// from the anchor and never clears it, so the weakest tier still yields the row - while
		// checks_passed still reports every stronger predicate for the log. DLSS-NR takes no
		// jitter parameter, so the jitter-specific gates are not ours to enforce.
		c.require_params        = false;
		c.allow_projection_only = true;

		ue4jitter::layout lay;
		ue4jitter::result res;
		const bool ok = ue4jitter::discover(cb.data(), cb.size(), c, lay, res);

		char desc[640];
		ue4jitter::describe(res, desc, sizeof(desc));

		if (!ok || !lay.valid || lay.row_clip_to_prev_clip < 0)
		{
			st.view_layout_failed = true;
			st.clip_ok = false;   // permanent latch: drop to raw, never freeze the last matrix
			if (!st.logged_mvec_clip_bad)
			{
				st.logged_mvec_clip_bad = true;
				LOGW("DLSS-NR: View uniform buffer discovery FAILED, so View.ClipToPrevClip cannot "
				     "be located and the camera-motion reconstruction is off. %s "
				     "(checks_run=0x%04x checks_passed=0x%04x, %u bytes read at offset %llu of an "
				     "%llu-byte upload pool). See the fallback note in the once-only message that "
				     "follows. This message is printed once.",
				     desc, res.checks_run, res.checks_passed, want_bytes,
				     (unsigned long long)br.offset, (unsigned long long)pd.buffer.size);
			}
			return false;
		}

		// CALLER-SIDE POLICY, and it is deliberately STRICTER than the header's.
		// ue4_jitter.hpp records clip_row_agrees but does not treat a disagreement as fatal,
		// because for JITTER the clip row is incidental. Here the clip row IS the payload, so a
		// disagreement between the content signature and the DXBC instruction analysis means one
		// of them is describing a buffer or a shader we have misidentified - and reprojecting the
		// world through the wrong four rows is exactly the silent failure this feature must not
		// ship.
		if (shader.info.clip_to_prev_clip_start_index >= 0 &&
		    (res.checks_passed & ue4jitter::check::clip_row_agrees) == 0)
		{
			st.view_layout_failed = true;
			st.clip_ok = false;   // permanent latch: drop to raw, never freeze the last matrix
			if (!st.logged_mvec_clip_row)
			{
				st.logged_mvec_clip_row = true;
				LOGE("DLSS-NR: the two independent derivations of the View.ClipToPrevClip row "
				     "DISAGREE - the View constant buffer's own content signature says row %d "
				     "(projection anchor %d + 94) and this game's TAA bytecode says row %d. One of "
				     "them is describing something we have misidentified, and reprojecting the "
				     "static world through the wrong four rows would produce confident, coherent, "
				     "COMPLETELY WRONG motion. The camera-motion reconstruction is therefore "
				     "REFUSED. Set mvec_clip_row=<row> to override this deliberately, or "
				     "mvec_decode=0 for today's behaviour. This message is printed once.",
				     lay.row_clip_to_prev_clip, lay.row_view_to_clip,
				     shader.info.clip_to_prev_clip_start_index);
			}
			return false;
		}

		st.view_layout    = lay;
		st.view_layout_ok = true;

		// ViewSizeAndInvSize, when it validated. Otherwise the TAA output extent, which is what
		// discovery was told to expect and what every measured extent in STRAY equals.
		st.view_size_measured = (res.checks_passed & ue4jitter::check::view_size_row) != 0;
		st.view_size[0] = static_cast<float>(res.render_width  != 0 ? res.render_width  : taa_w);
		st.view_size[1] = static_cast<float>(res.render_height != 0 ? res.render_height : taa_h);

		LOGI("DLSS-NR: View uniform buffer LOCATED. b%d, root CBV -> ID3D12Resource=0x%llx + "
		     "offset %llu in an %llu-byte upload pool. %s (checks_run=0x%04x checks_passed=0x%04x). "
		     "ViewToClip anchor row %d; ClipToPrevClip row %d (anchor + 94); the probe's "
		     "INDEPENDENT DXBC-derived row is %d - %s. View rect %.0fx%.0f (%s).",
		     want_reg, (unsigned long long)br.buffer.handle, (unsigned long long)br.offset,
		     (unsigned long long)pd.buffer.size, desc, res.checks_run, res.checks_passed,
		     lay.row_view_to_clip, lay.row_clip_to_prev_clip,
		     shader.info.clip_to_prev_clip_start_index,
		     shader.info.clip_to_prev_clip_start_index < 0
		        ? "NOT AVAILABLE, so the cross-check could not run"
		        : "THEY AGREE",
		     st.view_size[0], st.view_size[1],
		     st.view_size_measured ? "read from ViewSizeAndInvSize and validated"
		                           : "ASSUMED equal to the TAA output extent - ViewSizeAndInvSize "
		                             "did not validate");
	}

	// ---------------------------------------------------------------- the per-frame 64-byte read
	int32_t row = st.view_layout.row_clip_to_prev_clip;
	if (g_cfg.mvec_clip_row != 0)
	{
		row = static_cast<int32_t>(g_cfg.mvec_clip_row);
		if (!st.logged_mvec_pinned_row)
		{
			st.logged_mvec_pinned_row = true;
			LOGW("DLSS-NR: mvec_clip_row=%u is PINNED in the ini, so the discovered row %d is being "
			     "OVERRIDDEN and the content signature that produced it is bypassed. This is the "
			     "same posture as shader_hash=0: deterministic, overridable, and unvalidated. Set "
			     "mvec_clip_row=0 to go back to the two-way-cross-checked row.",
			     g_cfg.mvec_clip_row, st.view_layout.row_clip_to_prev_clip);
		}
	}
	if (row < 0 || static_cast<uint64_t>(row + 4) * ue4jitter::kBytesPerRow > avail)
	{
		st.view_layout_failed = true;
		st.clip_ok = false;   // permanent latch: drop to raw, never freeze the last matrix
		return false;
	}

	// ------------------------------------------------------------------ camera-cut detection
	// A second 16-byte read out of the SAME mapped upload pool, one row, only when the jitter row
	// validated during discovery. Deliberately BEFORE the clip read and independent of it: a cut
	// is worth signalling even on a frame whose ClipToPrevClip read then fails, because Reset is
	// how NGX is told to discard history, and a stale history is exactly what a cut produces.
	// Failure here is silent and costs nothing - need_reset simply is not raised.
	if (st.view_layout.row_jitter >= 0 &&
	    static_cast<uint64_t>(st.view_layout.row_jitter + 1) * ue4jitter::kBytesPerRow <= avail)
	{
		float j[4];
		const uint64_t joff = br.offset +
			static_cast<uint64_t>(st.view_layout.row_jitter) * ue4jitter::kBytesPerRow;
		if (ue4jitter::read_view_cb(pool, joff, ue4jitter::kBytesPerRow, j))
		{
			st.jitter_cut_ok.store(true, std::memory_order_relaxed);
			// Bitwise equality, not a tolerance: UE4 COPIES the matrices on a reset frame, so the
			// two float pairs are the same bits. A tolerance would fire on any slow pan.
			if (j[2] == j[0] && j[3] == j[1])
			{
				st.need_reset = true;
				st.camera_cuts.fetch_add(1, std::memory_order_relaxed);
				if (!st.logged_cut_source)
				{
					st.logged_cut_source = true;
					LOGI("DLSS-NR: camera-cut detection is LIVE - View.TemporalAAJitter.zw == .xy "
					     "at row %d, so UE4 reset the view this frame and DLSSNR.Reset is being "
					     "pulsed. This is the signal the reference add-on gets for free from its "
					     "host game's reset flag. Logged once; the running count is in the census.",
					     st.view_layout.row_jitter);
				}
			}
		}
	}

	float m[16];
	if (!nr_read_clip_rows(pool, br.offset, static_cast<uint32_t>(row), m))
	{
		// KEEP THE LAST GOOD MATRIX. A one-frame-stale reprojection is a small, bounded error;
		// swapping the bound guide resource mid-run because one Map failed would change the grid
		// under NGX's temporal history, which is not.
		st.mvec_cb_reuse.fetch_add(1, std::memory_order_relaxed);
		if (++st.clip_fail_streak >= 30)
		{
			st.view_layout_failed = true;
			st.clip_ok = false;
			if (!st.logged_mvec_clip_bad)
			{
				st.logged_mvec_clip_bad = true;
				LOGE("DLSS-NR: 30 consecutive failures reading View.ClipToPrevClip out of the "
				     "upload pool. The camera-motion reconstruction is off for this resolution and "
				     "the motion-vector guide falls back as described in the once-only message "
				     "above. This message is printed once.");
			}
		}
		return st.clip_ok;
	}

	if (g_cfg.mvec_clip_transpose)
	{
		float t[16];
		for (int r = 0; r < 4; ++r)
			for (int c = 0; c < 4; ++c)
				t[4 * r + c] = m[4 * c + r];
		std::memcpy(m, t, sizeof(m));
	}

	if (!mvec_decode::clip_plausible(m, st.view_size[0], st.view_size[1]))
	{
		st.mvec_cb_reuse.fetch_add(1, std::memory_order_relaxed);
		if (++st.clip_fail_streak >= 30)
		{
			st.view_layout_failed = true;
			st.clip_ok = false;
			if (!st.logged_mvec_clip_bad)
			{
				st.logged_mvec_clip_bad = true;
				LOGE("DLSS-NR: the four rows read as View.ClipToPrevClip failed the plausibility "
				     "check 30 frames running (non-finite, all-zero, or throwing the frame centre "
				     "off to infinity). The camera-motion reconstruction is off for this "
				     "resolution. If mvec_clip_transpose or mvec_clip_row was set, clear it. This "
				     "message is printed once.");
			}
		}
		return st.clip_ok;
	}

	// ONE counter covers BOTH failure modes - the 64-byte read and the plausibility test - so it
	// may only be cleared once a matrix has been fully ACCEPTED. Clearing it on a merely SUCCESSFUL
	// READ (which is every frame: a 64-byte Map of an UPLOAD-heap pool essentially always works)
	// made the plausibility path's `++streak >= 30` evaluate 1 >= 30 forever. The give-up latch was
	// unreachable, and with it the one LOGE that names mvec_clip_transpose / mvec_clip_row as the
	// cause - so a mispinned row failed silently, every frame, for the whole run.
	st.clip_fail_streak = 0;

	std::memcpy(st.clip_to_prev, m, sizeof(st.clip_to_prev));
	st.clip_ok = true;
	return true;
}

// --------------------------------------------------------------------------------------------
// THE HDR CODEC'S TWO DISPATCHES, IN ONE PLACE.
//
// Lifted out of the DLSS-NR pass unchanged so chain mode runs the SAME encode and the SAME decode
// with the SAME s. A divergence between two copies would be a CORRECTNESS failure rather than a
// tuning difference - the decode subtracts a proxy that was built with s, and hdr_codec.hpp's
// identity property is only exact while the two agree - so there is one copy and no way to
// disagree.
//
// The only thing that differs between the callers is the SOURCE of `original`:
//   DLSS-NR alone   st.orig_srv, an SRV on our own pristine copy of the TAA output.
//   chain mode      the GAME'S OWN t5 descriptor, borrowed for this event and never stored -
//                   exactly as the motion-vector decode already borrows the game's velocity and
//                   depth SRVs. No copy, no barrier, and no view created on a game resource.
//
// Both dispatch over st.out_w x st.out_h, the extent every codec texture is allocated at.
// --------------------------------------------------------------------------------------------
static void nr_codec_encode(command_list *cmd, nr_state &st, resource_view src_srv,
                            float proxy_scale, bool &proxy_in_srv)
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
	cmd->bind_descriptor_tables(shader_stage::all_compute, st.codec.encode_layout, 0, 0, nullptr);
	cmd->bind_pipeline(pipeline_stage::all_compute, st.codec.encode_pso);

	descriptor_table_update srv_up = {};
	srv_up.binding = 0; srv_up.array_offset = 0; srv_up.count = 1;
	srv_up.type = descriptor_type::shader_resource_view;
	srv_up.descriptors = &src_srv;
	cmd->push_descriptors(shader_stage::compute, st.codec.encode_layout, hdr_codec::kParamSrvTable, srv_up);

	descriptor_table_update uav_up = {};
	uav_up.binding = 0; uav_up.array_offset = 0; uav_up.count = 1;
	uav_up.type = descriptor_type::unordered_access_view;
	uav_up.descriptors = &st.proxy_uav;
	cmd->push_descriptors(shader_stage::compute, st.codec.encode_layout, hdr_codec::kParamUavTable, uav_up);

	hdr_codec::encode_args ea;
	ea.width = st.out_w; ea.height = st.out_h; ea.proxy_scale = proxy_scale; ea.pad0 = 0;
	cmd->push_constants(shader_stage::compute, st.codec.encode_layout, hdr_codec::kParamConstants,
	                    0, hdr_codec::kEncodeConstantCount, &ea);

	cmd->dispatch(hdr_codec::group_count(st.out_w), hdr_codec::group_count(st.out_h), 1);

	// The encode's write has to be visible to the snippet. In Remix this is the barrier set
	// flushed immediately before the evaluate (rtx_neural_rendering.cpp:355-358); here the
	// UNORDERED_ACCESS -> NON_PIXEL_SHADER_RESOURCE transition IS the write-completion barrier,
	// and it also puts the proxy in the state NGX reads Color in.
	cmd->barrier(st.proxy_tex, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel);
	proxy_in_srv  = true;
}

static void nr_codec_decode(command_list *cmd, nr_state &st, resource_view original_srv,
                            float proxy_scale, float transfer_strength, float color_strength,
                            uint32_t graft_mode, bool &out_in_srv)
{
	// ---- THE NR PROBE, BEFORE THE BARRIER (nr_probe=1 only) ---------------------------------
	// Deliberately ahead of the transition below: out_tex is still in unordered_access, exactly
	// as NGX left it, so this reads the texture through a UAV by the same route NGX wrote it.
	//
	// Reading it AFTER the barrier, through st.out_srv, returned EXACTLY zero on validated
	// gameplay content (IN [0.786 .. 4912.7], OUT [0.0 .. 0.0], 1.7M samples/step, means
	// agreeing with the extremes). That is either an empty texture or a broken SRV/state, and
	// the two are indistinguishable from that side. This dispatch is the same texture in the
	// same frame by the other route, which separates them.
	if (g_cfg.nr_probe != 0 && st.probe.ready)
	{
		nr_probe::frame(cmd->get_device(), cmd, st.probe, st.probe_run,
		                original_srv, st.out_srv, g_cfg.nr_probe_selftest != 0, st.out_w, st.out_h,
		                g_cfg.nr_probe_frames, g_cfg.nr_probe_warmup,
		                [](const char *fmt, auto... args) {
			                logf(reshade::log::level::info, fmt, args...);
		                });
	}

	// NGX wrote out_tex OUTSIDE anything that tracks it, so nothing knows the decode's read of
	// it has to wait. This transition is that dependency, and it is also what makes the
	// texture readable as an SRV. (rtx_neural_rendering.cpp:408-441 records the same two
	// hazards in Vulkan terms.)
	cmd->barrier(st.out_tex, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel);
	out_in_srv = true;

	// The cache sync again, and this is the call whose absence is a device removal: NGX has
	// just rebound the descriptor heaps and the compute root signature on the RAW list, and
	// ReShade's cache still names ours from the encode above.
	cmd->bind_descriptor_tables(shader_stage::all_compute, st.codec.decode_layout, 0, 0, nullptr);
	cmd->bind_pipeline(pipeline_stage::all_compute, st.codec.decode_pso);

	// t0 original, t1 proxy, t2 neural - one contiguous table, in declaration order.
	const resource_view decode_srvs[3] = { original_srv, st.proxy_srv, st.out_srv };
	descriptor_table_update srv_up = {};
	srv_up.binding = 0; srv_up.array_offset = 0; srv_up.count = 3;
	srv_up.type = descriptor_type::shader_resource_view;
	srv_up.descriptors = decode_srvs;
	cmd->push_descriptors(shader_stage::compute, st.codec.decode_layout, hdr_codec::kParamSrvTable, srv_up);

	descriptor_table_update uav_up = {};
	uav_up.binding = 0; uav_up.array_offset = 0; uav_up.count = 1;
	uav_up.type = descriptor_type::unordered_access_view;
	uav_up.descriptors = &st.result_uav;
	cmd->push_descriptors(shader_stage::compute, st.codec.decode_layout, hdr_codec::kParamUavTable, uav_up);

	hdr_codec::decode_args da;
	da.width = st.out_w; da.height = st.out_h;
	// The IDENTICAL scale the encode used, from the same CPU float in the same frame.
	da.proxy_scale = proxy_scale;
	// Clamped on the CPU, exactly as Remix does (rtx_neural_rendering.cpp:525-526); the shader
	// does not re-clamp them.
	da.transfer_strength = transfer_strength;
	da.color_strength    = color_strength;
	// The ONLY thing that selects the renodx graft. It reaches the shader here and nowhere
	// else: no pipeline is rebuilt and no feature is recreated, so if this dispatch runs at
	// all, the mode is in effect. Threaded through the extraction rather than read from
	// g_cfg here, because the chain path calls this with the same value the encode used.
	da.graft_mode        = graft_mode;
	da.pad1 = da.pad2 = 0;
	cmd->push_constants(shader_stage::compute, st.codec.decode_layout, hdr_codec::kParamConstants,
	                    0, hdr_codec::kDecodeConstantCount, &da);

	cmd->dispatch(hdr_codec::group_count(st.out_w), hdr_codec::group_count(st.out_h), 1);

	// ---- THE NR PROBE (nr_probe=1 only; a strict no-op otherwise) ---------------------------
	//
	// Placed HERE, and nowhere else, for one reason: at this point BOTH sides of the network
	// exist in the SAME FRAME and both are already in shader_resource_non_pixel - `original_srv`
	// is what the network was given, `st.out_srv` is what it returned, and the decode above has
	// just transitioned out_tex for exactly that read. Measuring them against each other removes
	// the scene, the camera, the cat and the HDR graft from the comparison in one step, which is
	// what every screenshot comparison failed to do: a cold relaunch per value moved the cat, and
	// that scene difference measured LARGER than the effect it was supposed to resolve.
	//
	// The probe also DRIVES use_auto_mask / local_structure / skin_structure itself (see
	// nr_evaluate), holding each setting for nr_probe_frames frames, so every step sees the same
	// content and repeats its own baseline at the end as a noise floor.
}

// --------------------------------------------------------------------------------------------
// THE DLSS-NR KEY SET AND THE EVALUATE, IN ONE PLACE.
//
// TWO callers now: the DLSS-NR pass, and chain mode (dlss_chain=1), which runs the same network
// on the SAME command list a few lines before DLSS-SR's. This is one function rather than two
// copies because a divergence between them would be SILENT - an absent key is not an error to
// NGX, it is a default, and that is exactly how a control in this tree once shipped as a control
// that did nothing.
//
// Everything geometric arrives through nr_eval_args, because the two callers' geometry is not the
// same. DLSS-NR alone denoises the TAA pass's OUTPUT (taa_out - which in the MainUpsampling
// permutation chain mode requires is 3840x2160). Chained, it denoises the TAA pass's INPUT at the
// render extent, so that DLSS-SR upscales a DENOISED image instead of a noisy one.
//
// The guide-reset latch lives in here, keyed on the bound RESOURCE as well as the extent, so both
// callers get it - and so the first chained frame, where the guide changes from st.mvec_tex (or
// the game's raw velocity) to the shared st.sr_res.mvec_tex, correctly pulses one DLSSNR.Reset.
// --------------------------------------------------------------------------------------------
// --------------------------------------------------------------------------------------------
// README GAP 4: WHICH VALUE DLSSNR.DepthInverted ACTUALLY GETS.
//
// THE ORDER IS: A HUMAN, THEN THE MEASUREMENT, THEN THE DEFAULT. In one place, because a
// divergence between the value SENT and the value REPORTED is exactly the silent failure this
// add-on's own log lines exist to prevent - the evaluate line and the census line below both call
// this rather than reading g_cfg, so what is printed is what was written.
//
//   1. depth_inverted appeared in stray_dlssnr.ini    -> the ini value, whatever was measured.
//      A line that says depth_inverted=1 is a CHOICE, and the same value arrived at by default is
//      not; cfg::depth_inverted_pinned is the only thing that can tell those apart, which is the
//      whole reason it exists.
//   2. the user moved the DepthInverted control after the measurement latched -> the control.
//      A live control that silently does nothing is a defect this tree has already caught in
//      itself twice, and it is not going to ship a third.
//   3. the measurement latched a convention -> the measurement.
//   4. nothing measured (depth_detect=0, the pass could not be built, every window declined,
//      chain mode, or it simply has not finished yet) -> depth_inverted, i.e. today's behaviour.
//
// PURE, and it touches nothing but g_cfg and two latched bools - which is what lets the periodic
// census on the PRESENT thread call it beside its other unlocked g_cfg reads without inverting any
// lock order. The two bools it reads are written once each, on the render thread, and a census line
// that raced one of them would print the value from either side of a single transition. Reporting
// the old value for one census interval is the entire failure mode.
static bool nr_depth_inverted_value(const nr_state &st)
{
	if (st.depth_det.latched == depth_convert::verdict::undecided ||
	    g_cfg.depth_inverted_pinned || st.depth_det_stood_down)
		return g_cfg.depth_inverted;
	return (st.depth_det.latched == depth_convert::verdict::reversed);
}

struct nr_eval_args
{
	ID3D12Resource *color  = nullptr;  uint32_t color_w = 0, color_h = 0;
	ID3D12Resource *depth  = nullptr;  uint32_t depth_w = 0, depth_h = 0;
	ID3D12Resource *mvec   = nullptr;  uint32_t mvec_w  = 0, mvec_h  = 0;
	ID3D12Resource *output = nullptr;  uint32_t out_w   = 0, out_h   = 0;
	float scale_x = 1.0f, scale_y = 1.0f;
};

// out_reset reports the DLSSNR.Reset value that was actually sent, which is the one thing a caller
// cannot recompute afterwards: st.need_reset is cleared on success.
static ngx::Result nr_evaluate(nr_state &st, ID3D12GraphicsCommandList *cl, const nr_eval_args &a,
                               bool &out_reset)
{
	if (st.params == nullptr || st.feature == nullptr || g_snippet.evaluate_feature == nullptr || cl == nullptr)
		return ngx::Result_FAIL_NotInitialized;

	ngx::parameter_block *p = st.params;

	// Process-lifetime, not stack buffers: Set takes the name as a bare const char* and
	// nothing in the ABI promises the callee copies it before returning.
	static const ngx::resource_param_names s_colour(ngx::kParamColor);
	static const ngx::resource_param_names s_depth(ngx::kParamDepth);
	static const ngx::resource_param_names s_mvec(ngx::kParamMVec);
	static const ngx::resource_param_names s_output(ngx::kParamOutput);
	static const ngx::resource_param_names s_mask(ngx::kParamControlMask);

	nr_set_resource(p, s_colour, a.color,  a.color_w, a.color_h);
	nr_set_resource(p, s_depth,  a.depth,  a.depth_w, a.depth_h);
	nr_set_resource(p, s_mvec,   a.mvec,   a.mvec_w,  a.mvec_h);
	nr_set_resource(p, s_output, a.output, a.out_w,   a.out_h);
	// BOUNDARY EVIDENCE, once per run. Every explanation tried for the unwritten output assumes
	// the resource handed to NGX here is the same object the add-on later reads back; nothing
	// has verified it. Compare this pointer against the one logged when out_tex was created.
	if (!st.logged_output_binding)
	{
		st.logged_output_binding = true;
		LOGI("DLSS-NR: DLSSNR.Output <- ID3D12Resource=0x%llx at %ux%u. out_tex handle=0x%llx. "
		     "If these name different objects, NGX is writing something this add-on never reads.",
		     (unsigned long long)reinterpret_cast<uintptr_t>(a.output), a.out_w, a.out_h,
		     (unsigned long long)st.out_tex.handle);
	}
	// Written EVERY frame even though this add-on never binds one - see nr_clear_resource.
	nr_clear_resource(p, s_mask);

	// The guide grid moved under a history accumulated against the old one. Nothing else
	// notices, so force one reset frame.
	//
	// THE RESOURCE IS PART OF THE KEY, not just the extent. The fallback ladder can swap
	// DLSSNR.MVec between our decoded texture and the game's raw velocity mid-run, and in
	// STRAY both are 1920x1080 - so an extent-only test would see NO change while the guide's
	// UNITS changed from absolute pixels to encoded unorm. That is precisely the case that
	// needs a reset, and it is the one an extent-only latch cannot see.
	const uint64_t mvec_res_key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(a.mvec));
	if (st.guide_w != a.mvec_w || st.guide_h != a.mvec_h || st.mvec_bound_res != mvec_res_key)
	{
		if (st.guide_w != 0 || st.guide_h != 0)
			st.need_reset = true;   // a first frame is initialisation, not a reset
		st.guide_w = a.mvec_w;
		st.guide_h = a.mvec_h;
		st.mvec_bound_res = mvec_res_key;
	}

	out_reset = st.need_reset;

	ngx::set_u32(p, ngx::kParamEnabled,       1u);
	ngx::set_u32(p, ngx::kParamReset,         st.need_reset ? 1u : 0u);
	// NOT g_cfg.depth_inverted directly - README gap 4. See nr_depth_inverted_value above for the
	// precedence, and note that with depth_detect=0 or nothing measured it IS g_cfg.depth_inverted,
	// bit for bit.
	ngx::set_u32(p, ngx::kParamDepthInverted, nr_depth_inverted_value(st) ? 1u : 0u);
	ngx::set_f32(p, ngx::kParamMVecScaleX,    a.scale_x);
	ngx::set_f32(p, ngx::kParamMVecScaleY,    a.scale_y);
	// SELECTS what BOTH structure strengths become, and does not by itself make them live.
	// With UseAutoMask == 0 the snippet substitutes the constant -1.0f [BIN 0x18001aa84, loading
	// 0x1800afc40] into both effective slots [BIN +0xf8 skin, +0xfc local]. With it set, +0xfc
	// takes LocalStructureStrength and +0xf8 takes SkinStructureStrength unless that is negative,
	// in which case it inherits LocalStructureStrength [BIN 0x18001aa6d comiss / 0x18001aa70 jae
	// / 0x18001aa72]. Either way the pair is only READ behind two dynamic_cast null tests
	// [BIN 0x18002253f and 0x18003f5f3] - see the tuning-knob note in addon_config.hpp. If those
	// fail, this key and both strengths are inert together.
	//
	// Binding a ControlMask would ALSO force it to 0 [BIN 0x18001aa4b cmp / 0x18001aa52]; this
	// add-on binds none. See nr_clear_resource for why writing the mask as an explicit null does
	// not count as bound.
	// THE PROBE OWNS THESE THREE KEYS WHILE IT RUNS. It is sweeping them to find out whether the
	// snippet's two dynamic_cast gates pass on this model, so the ini values are deliberately
	// ignored - taking them here instead would make every step identical and the sweep would
	// report "inert" no matter what the network does.
	const bool  probe_drives = st.probe_run.active && !st.probe_run.complete && st.probe.ready;
	const auto &sw           = nr_probe::kSweep[st.probe_run.step < nr_probe::kSweepCount
	                                            ? st.probe_run.step : 0u];

	ngx::set_u32(p, ngx::kParamUseAutoMask,
	             probe_drives ? sw.use_auto_mask : (g_cfg.use_auto_mask ? 1u : 0u));

	ngx::set_f32(p, ngx::kParamIntensity,              g_cfg.intensity);
	ngx::set_f32(p, ngx::kParamLocalToneStrength,      g_cfg.local_tone_strength);
	ngx::set_f32(p, ngx::kParamLocalStructureStrength,
	             probe_drives ? sw.local_structure : g_cfg.local_structure_strength);
	// Negative means "inherit LocalStructureStrength". 0.0 is NOT neutral.
	ngx::set_f32(p, ngx::kParamSkinStructureStrength,
	             probe_drives ? sw.skin_structure : g_cfg.skin_structure_strength);
	ngx::set_u32(p, ngx::kParamStyle,                  g_cfg.style);
	// ---- BEGIN overlay_ui hook ----
	// The overlay's "NR UI Correction" checkbox. Per-evaluate, exactly like Style, and NOT
	// baked at create time - and st.params is the SAME parameter block nr_ensure_feature
	// writes into, so a later CreateFeature sees it as well. DLSSNR.UICorrection is a real
	// parameter of THIS snippet build: exactly one exact-line match in nvngx_dlssnr.dll's
	// string table, against ZERO for DLSSNR.Upscaling. Without this write the checkbox would
	// be a control that does nothing - the user would A/B it, see no change, and record a
	// false negative. Default 0, which is also the snippet's own fallback, so an untouched
	// install is bit-identical to one without this line.
	ngx::set_u32(p, ngx::kParamUICorrection,           g_cfg.ui_correction);

	// PARITY. renodx writes both indicator-axis keys = 0 [BIN 0x180014adb / 0x180014aee] and
	// both strings exist in the snippet. These condition the NGX debug indicator overlay,
	// which is registry-armed and off by default, so this is purely defensive: it stops the
	// snippet reading an absent key if the indicator is ever enabled on this machine.
	ngx::set_u32(p, ngx::kParamIndicatorInvertX, 0u);
	ngx::set_u32(p, ngx::kParamIndicatorInvertY, 0u);
	// ---- END overlay_ui hook ----

	return g_snippet.evaluate_feature(cl, st.feature, p, nullptr);
}

// =============================================================================================
// DLSS SUPER RESOLUTION - the pass.
//
// Reached ONLY from nr_try_run, and only when g_cfg.dlss_sr is 1. Everything it needs has already
// been established by the shared code above it: the shader is the target, the SRV class quorum
// passed, the output UAV is resolved, the descriptor-table and heap identities are VERIFIED, and
// probe::capture_state has produced a COMPLETE restore plan. This function owns the window between
// that capture and probe::restore_state, and nothing else.
//
// ---------------------------------------------------------------------------------------------
// THE DISPATCH CONTRACT, WHICH IS INVERTED FROM DLSS-NR'S
// ---------------------------------------------------------------------------------------------
// `issued == true` means "the game's Dispatch is already on the command list, do not issue it".
//
//   sr_suppress_taa = 0   we re-issue the game's TAA exactly where it would have run and set
//                         `issued` on the very next line, before anything that can throw - the
//                         DLSS-NR contract, byte for byte. DLSS then writes on top. One wasted
//                         dispatch, and a completely safe bring-up shape.
//
//   sr_suppress_taa = 1   we do NOT issue it, and `issued` is set LAST: after EvaluateFeature
//                         returned Success AND after probe::restore_state has run. Any bail, any
//                         throw, any NGX failure therefore leaves it false, ReShade issues the
//                         game's own TAAU, and that shader writes EVERY pixel of the output view
//                         rect with no read-modify-write (TemporalAA.usf:2268-2281) - so it
//                         overwrites whatever partial work we recorded and the frame is CORRECT.
//                         A failed frame costs one wasted dispatch, not a garbage frame.
//
// The same PROBE_GUARD_RETURN wrapper serves both, for opposite reasons: under DLSS-NR an escape
// must preserve a possibly-true value, under suppression an escape must leave false.
//
// ONE INTERACTION THAT IS NEW AND THAT NOBODY ELSE CAN SEE: reshade::invoke_addon_event does not
// short-circuit - it ORs every callback with no break - so a `true` from this add-on suppresses
// the dispatch for EVERY co-loaded add-on too, and they are given no way to learn it. DLSS-NR
// never had this, because it re-issued. Documented in the README.
// =============================================================================================

#define SR_BAIL(why) do { \
    static std::atomic<bool> s_said{ false }; bool e_ = false; \
    if (s_said.compare_exchange_strong(e_, true, std::memory_order_acq_rel)) \
        LOGW("DLSS-SR: pass did not run - %s", why); \
    return; } while (0)

#define SR_STAGE(n) do { \
    static std::atomic<bool> s_st{ false }; bool e2_ = false; \
    if (s_st.compare_exchange_strong(e2_, true, std::memory_order_acq_rel)) \
        LOGI("DLSS-SR stage: %s", n); } while (0)

// One log functor for dlss_sr.hpp, matching the ones hdr_codec and mvec_decode are given.
static void sr_log(int lvl, const char *msg)
{
	logf(lvl == dlss_sr::log_error ? reshade::log::level::error
	   : lvl == dlss_sr::log_warn  ? reshade::log::level::warning
	                               : reshade::log::level::info, "%s", msg);
}

// Locates the game's View uniform buffer for THIS dispatch and bounds-checks it. Identical
// reasoning to nr_update_clip_to_prev_clip's opening, and deliberately the same code path:
// nr_find_view_cb matches the root CBV by the bN slot the shader's own dcl_constant_buffer census
// named as its largest cbuffer, never by root-parameter index, and b1 is only the fallback. That
// is what retires the "$Globals at b0 carries stale bytes from earlier passes" hazard by
// construction rather than by convention.
static bool sr_find_view_pool(device *dev, probe::device_shadow &sh, const probe::cmd_shadow &cs,
                              const shader_record &shader,
                              ID3D12Resource *&out_pool, uint64_t &out_offset, uint64_t &out_avail)
{
	const int32_t want_reg = (shader.info.global_buffer_register_index >= 0)
		? shader.info.global_buffer_register_index : 1;

	buffer_range br = {};
	if (!nr_find_view_cb(sh, cs.cmp, want_reg, br))
		return false;

	// buffer_range.size is hard-coded to UINT64_MAX by ReShade and carries NO information, so the
	// real bound comes from the resource - and that resource is UE's fast-constant upload POOL,
	// not the constant buffer.
	const resource_desc pd = probe::abi_get_resource_desc(dev, br.buffer);
	if (pd.type != resource_type::buffer || pd.buffer.size == 0 || br.offset >= pd.buffer.size)
		return false;

	out_pool   = reinterpret_cast<ID3D12Resource *>(br.buffer.handle);
	out_offset = br.offset;
	out_avail  = pd.buffer.size - br.offset;
	return out_pool != nullptr;
}

static void sr_try_run(command_list *cmd, device *dev, probe::device_shadow &sh,
                       const probe::cmd_shadow &cs, nr_state &st, const shader_record &shader,
                       const nr_view_info &colour, const nr_view_info &depth,
                       const nr_view_info &velocity,
                       resource_view vel_view, resource_view depth_view,
                       resource_view colour_view,
                       const nr_view_info &taa_out, uint32_t taa_out_reg,
                       probe::restore_plan &plan, ID3D12GraphicsCommandList *d3d12_cmd,
                       uint32_t gx, uint32_t gy, uint32_t gz,
                       uint32_t want_out_w, uint32_t want_out_h, bool chain, bool &issued)
{
	// CHAIN MODE is DLSS-NR spliced into this function at four points, not a third pass. Building
	// it as its own function would have duplicated the jitter read, the coarse geometry key, the
	// per-geometry create latch, the render view rect, the resolution latch and the whole
	// suppression/ownership contract - six things whose divergence would be silent. Every splice
	// is marked "CHAIN MODE"; with dlss_chain=0 `chain` is false and each one is a predictable
	// branch and nothing else.
	//
	// Published for nr_ensure_aux, which allocates a different set of textures on this path. It is
	// assigned on EVERY entry, in both directions, so a run that stops chaining cannot leave a
	// stale true behind for the DLSS-NR path to read.
	st.chain_active = chain;
	// SUPPRESSING WITHOUT WRITING IS THE ONE COMBINATION THAT PRODUCES A GARBAGE FRAME, and it is
	// reachable from the ini: sr_suppress_taa=1 with sr_direct_output=0 and sr_copy_back=0 would
	// stop the game's TAA from running while DLSS wrote into a texture nothing reads, leaving u0
	// holding whatever was last in it. That is not a rung on any ladder, so it is REFUSED rather
	// than obeyed - suppression is dropped and the game's TAAU keeps running.
	const bool suppress = g_cfg.sr_suppress_taa && (g_cfg.sr_direct_output || g_cfg.sr_copy_back);
	if (g_cfg.sr_suppress_taa && !suppress && !st.logged_sr_suppress)
	{
		st.logged_sr_suppress = true;
		LOGE("DLSS-SR: sr_suppress_taa=1 is being IGNORED because nothing would write the output - "
		     "sr_direct_output=0 and sr_copy_back=0 together mean DLSS evaluates into a texture "
		     "nothing reads. Suppressing the game's TAA on top of that would leave the frame "
		     "holding whatever was last in u0. The game's TAA is still being issued. Set "
		     "sr_copy_back=1 (or sr_direct_output=1) first - see STAGING-sr.md rungs 3 and 4.");
	}

	if (!g_sr_armed.load(std::memory_order_relaxed))
		SR_BAIL("not armed - the SR snippet did not load, or its Init_Ext failed");
	if (st.sr_latched_off)
		SR_BAIL("latched OFF for this run after 8 consecutive EvaluateFeature failures");
	if (st.sr_feat.params == nullptr)
		SR_BAIL("the SR parameter block was never allocated");

	// ---------------------------------------------------------------- the OUTPUT extent
	if (want_out_w == 0 || want_out_h == 0)
		SR_BAIL("the output extent could not be derived (group counts were zero and no sr_out_width/height was pinned)");

	if (!st.logged_sr_out_extent)
	{
		st.logged_sr_out_extent = true;
		const uint32_t tile = (g_cfg.sr_group_tile != 0) ? g_cfg.sr_group_tile : 8u;
		LOGI("DLSS-SR: OUTPUT EXTENT %ux%u, %s. The dispatch's group counts are %ux%ux%u at tile "
		     "size %u, so the engine's DestRect is in (%u, %u] x (%u, %u] - and the u%u texture "
		     "this pass chose is %ux%u %s. Colour INPUT (t%u) is %ux%u, i.e. a %.3fx by %.3fx "
		     "upscale. If those numbers are not what you configured, pin sr_out_width / "
		     "sr_out_height rather than trusting this derivation.",
		     want_out_w, want_out_h,
		     (g_cfg.sr_out_width != 0 || g_cfg.sr_out_height != 0)
		        ? "PINNED in stray_dlssnr.ini" : "derived from the dispatch's own group counts",
		     gx, gy, gz, tile,
		     (tile * gx > tile ? tile * gx - tile : 0u), tile * gx,
		     (tile * gy > tile ? tile * gy - tile : 0u), tile * gy,
		     taa_out_reg, taa_out.w, taa_out.h, probe::format_name(taa_out.fmt),
		     g_cfg.srv_colour, colour.w, colour.h,
		     colour.w != 0 ? (double)want_out_w / (double)colour.w : 0.0,
		     colour.h != 0 ? (double)want_out_h / (double)colour.h : 0.0);
	}

	// ---------------------------------------------------------------- the COARSE geometry key
	//
	// Recorded UNCONDITIONALLY, and consulted BEFORE any per-geometry failure latch.
	//
	// The finer-grained geometry_moved test further down keys on state that EXISTS - a live
	// feature handle, an allocated output texture, an allocated mvec texture - and with
	// sr_direct_output=1 no add-on output texture is ever allocated, so with the mvec texture
	// also absent (sr_mvec_decode=0, or the shared pipeline failed to build) there is NONE. A
	// CreateFeature that failed at extent A would then latch sr_feat.failed with no clause able
	// to notice a move to extent B, and DLSS-SR would be off for the rest of the process with no
	// further diagnostic - while the error it printed says it will be retried when the geometry
	// moves. nr_ensure_output had this property unconditionally on the DLSS-NR path; dlss_sr=1
	// skips that call, so the property has to be restored here.
	//
	// The key is the COLOUR INPUT extent and the OUTPUT extent, both of which are known before
	// the jitter read. render_w below can only differ from colour.w by the view rect, which is a
	// sub-texture refinement of the same movement, and the finer test still catches that.
	const uint32_t prev_col_w = st.sr_seen_col_w, prev_col_h = st.sr_seen_col_h;
	const uint32_t prev_out_w = st.sr_seen_out_w, prev_out_h = st.sr_seen_out_h;
	const bool key_moved = (prev_out_w != 0) &&
		(prev_col_w != colour.w || prev_col_h != colour.h ||
		 prev_out_w != want_out_w || prev_out_h != want_out_h);
	st.sr_seen_col_w = colour.w;   st.sr_seen_col_h = colour.h;
	st.sr_seen_out_w = want_out_w; st.sr_seen_out_h = want_out_h;
	if (key_moved)
	{
		LOGI("DLSS-SR: the geometry moved (colour input %ux%u -> %ux%u, output %ux%u -> %ux%u). "
		     "Everything per-geometry - the feature, both add-on textures, and every failure "
		     "latch on them - is queued for release on the next present; this frame runs the "
		     "game's own TAA untouched.",
		     prev_col_w, prev_col_h, colour.w, colour.h,
		     prev_out_w, prev_out_h, want_out_w, want_out_h);
		// kTeardown, not the pending_teardown bool this used to set: the reconfigure ladder replaced
		// that single flag with an ACTION MASK, and fetch_or rather than a store is the whole point -
		// a user reconfigure can land in the same frame as a geometry change, and a store would
		// silently discard whatever the overlay had asked for. nr_service_reconfigure consumes the
		// bit on the next present, on the main thread, where idling the queue is legal, and hands it
		// to the same nr_release_feature_and_output this always used - which releases the SR feature
		// and both SR textures along with DLSS-NR's.
		st.pending_work.fetch_or(kTeardown, std::memory_order_relaxed);
		return;
	}

	// THE PER-GEOMETRY CREATE LATCH, and it gates the WHOLE PASS rather than just create_feature.
	//
	// dlss_sr::create_feature short-circuits on f.failed, but everything ABOVE it in this
	// function would still run every frame for a feature that can never exist: sr_find_view_pool
	// plus five Map/Unmap round trips of UE's upload pool in update_jitter, the ClipToPrevClip
	// read, a full render-extent mvec_decode dispatch, two barriers on the guide, and
	// probe::restore_state. The frames stay correct - issued is left false and ReShade re-issues
	// the game's TAAU - but the cost is permanent and silent. Bail here instead, AFTER the key
	// check above so a genuine geometry change still queues the teardown that clears the latch.
	if (st.sr_feat.failed)
		SR_BAIL("CreateFeature failed for this geometry, so the WHOLE PASS is off until the "
		        "colour-input or output extent moves - see the CreateFeature error above");

	// ---------------------------------------------------------------- JITTER, and it is MANDATORY
	//
	// Jitter.Offset.X/Y sit in the same hard gate as Color/MotionVectors/Depth/Output. There is no
	// "run without jitter" configuration, and sending (0,0) would be WORSE than refusing: zero is
	// a legitimate value (r.TemporalAASamples=1) that the snippet cannot distinguish from "the
	// host could not read it", so a silent shimmer is exactly what it would produce.
	SR_STAGE("about to read the View uniform buffer for jitter");
	ID3D12Resource *pool = nullptr;
	uint64_t cb_offset = 0, cb_avail = 0;
	bool have_jitter = false;
	if (sr_find_view_pool(dev, sh, cs, shader, pool, cb_offset, cb_avail))
	{
		have_jitter = dlss_sr::update_jitter(st.sr_jitter, pool, cb_offset, cb_avail,
		                                     colour.w, colour.h,
		                                     shader.info.clip_to_prev_clip_start_index,
		                                     g_cfg.sr_jitter_projection_only, &sr_log);
	}
	if (!have_jitter)
	{
		if (!st.logged_sr_no_jitter)
		{
			st.logged_sr_no_jitter = true;
			LOGE("DLSS-SR: the sub-pixel jitter could not be recovered from the View uniform "
			     "buffer, so the SR pass will NOT run. Jitter.Offset.X and Jitter.Offset.Y are "
			     "UNCONDITIONALLY REQUIRED by nvngx_dlss.dll - they sit in the same hard gate as "
			     "Color/MotionVectors/Depth/Output and a miss returns FAIL_InvalidParameter - and "
			     "supplying (0,0) instead would be a legitimate-looking value the network cannot "
			     "tell apart from a read failure, which is a silent shimmer rather than an error. "
			     "The game's own TAA is untouched. The message above names the failing predicate; "
			     "sr_jitter_projection_only=1 accepts the weakest tier. This message is printed "
			     "once.");
		}
		return;
	}

	const float jitter_x = st.sr_jitter.jitter_x * g_cfg.sr_jitter_scale_x;
	const float jitter_y = st.sr_jitter.jitter_y * g_cfg.sr_jitter_scale_y;

	// ---------------------------------------------------------------- CHAIN MODE: ONE camera cut, BOTH networks
	//
	// Both features want a Reset on a camera cut, and the two detectors are NOT equally reachable.
	// DLSS-NR's (TemporalAAJitter.zw == .xy, bitwise) lives inside nr_update_clip_to_prev_clip,
	// which is only called when the motion-vector decode is wanted - so with sr_mvec_decode=0 it
	// never fires at all. DLSS-SR's is computed UNCONDITIONALLY by update_jitter, which has just
	// run. So the unconditional one is the source, it is read here - BEFORE either evaluate - and
	// it is latched into both features' sticky flags so that a cut on a frame whose evaluate fails
	// is carried forward rather than lost.
	if (chain && st.sr_jitter.reset_signalled)
	{
		st.need_reset         = true;   // DLSS-NR: read by nr_evaluate
		st.sr_feat.need_reset = true;   // DLSS-SR: OR'd into DLSS.Reset by dlss_sr::evaluate_feature
	}

	// ---------------------------------------------------------------- the RENDER view rect
	//
	// QuantizeSceneBufferSize rounds the scene buffer's TEXTURE extent up to a multiple of 4 while
	// the view rect is a CeilToInt of the resolution fraction, so at e.g. r.ScreenPercentage=58.8
	// the view rect is 1130 wide and the texture is 1132. Feeding DLSS the texture extent there
	// mis-scales the image by ~0.2% and puts two columns of uninitialised texels inside the
	// sampled region - a silent failure. ue4_jitter's ViewSizeAndInvSize is the view rect, and it
	// is used whenever it validated.
	uint32_t render_w = colour.w, render_h = colour.h;
	if (g_cfg.sr_use_view_rect && st.sr_jitter.view_rect_measured &&
	    st.sr_jitter.view_w != 0 && st.sr_jitter.view_h != 0 &&
	    st.sr_jitter.view_w <= colour.w && st.sr_jitter.view_h <= colour.h)
	{
		render_w = st.sr_jitter.view_w;
		render_h = st.sr_jitter.view_h;
	}

	if (render_w == 0 || render_h == 0 || render_w > want_out_w || render_h > want_out_h)
	{
		SR_BAIL("the render extent is zero, or larger than the output extent - CreateFeature "
		        "rejects Width > OutWidth outright");
	}

	// ---------------------------------------------------------------- resolution latch
	//
	// OutWidth/OutHeight are latched into the feature at CREATE and there is no evaluate-time
	// output extent, so any movement means a rebuild - and a rebuild needs the GPU idle, which a
	// recording thread cannot do. Route it through the existing pending_teardown, serviced on the
	// next present on the main thread.
	const bool geometry_moved =
		(st.sr_feat.handle != nullptr &&
		 (st.sr_render_w != render_w || st.sr_render_h != render_h ||
		  st.sr_out_w != want_out_w || st.sr_out_h != want_out_h)) ||
		// The TEXTURES too, and this is the one that bites without the feature ever existing: a
		// CreateFeature that failed at extent A leaves out_tex allocated at A, and ensure_output
		// then refuses at extent B forever with no way back. Both are per-geometry and both are
		// released together by nr_release_feature_and_output.
		(st.sr_res.out_tex.handle != 0 &&
		 (st.sr_res.out_w != want_out_w || st.sr_res.out_h != want_out_h ||
		  st.sr_res.out_fmt != taa_out.fmt)) ||
		(st.sr_res.mvec_tex.handle != 0 &&
		 (st.sr_res.mvec_w != colour.w || st.sr_res.mvec_h != colour.h));
	if (geometry_moved)
	{
		LOGI("DLSS-SR: the geometry moved (render %ux%u -> %ux%u, output %ux%u -> %ux%u). The "
		     "feature is queued for release on the next present; this frame runs the game's own "
		     "TAA untouched.",
		     st.sr_render_w, st.sr_render_h, render_w, render_h,
		     st.sr_out_w, st.sr_out_h, want_out_w, want_out_h);
		// Same seam, same reason as the coarse key check above.
		st.pending_work.fetch_or(kTeardown, std::memory_order_relaxed);
		return;
	}

	// ---------------------------------------------------------------- create-param latch
	//
	// THE SAME SEAM AS THE TWO ABOVE, FOR THE CREATE PARAMS THAT ARE NOT EXTENTS. The descriptor is
	// built HERE rather than beside CreateFeature so that this test can run BEFORE stage 1 records
	// anything - the chained frame's DLSS-NR half included - and so that the values compared are
	// bit-identical to the ones create_feature will be handed.
	//
	// WHY THIS TEST HAS TO EXIST. dlss_sr::feature_matches (src/dlss_sr.hpp) compares create_flags,
	// perf_quality AND hw_depth as well as the four extents, and create_feature's answer to a
	// mismatch on a LIVE handle is release_feature - whose header states the contract verbatim:
	// "The CALLER must have idled the queue first - CreateFeature and EvaluateFeature both record
	// real GPU work, so in-flight work can still reference it." sr_try_run runs on a COMMAND-LIST
	// RECORDING THREAD, inside the host's capture/restore window, and cannot idle a queue. Before
	// these keys were live the value could not move under a running feature, so the shape was
	// unreachable; every one of them is live now - begin_pass writes them into g_cfg from the
	// overlay's atomics on this very thread - so ticking IsHDR would have destroyed a feature the
	// GPU was still reading from.
	//
	// The only wait_idle() in the tree is the one nr_release_feature_and_output takes on the MAIN
	// thread from on_present, and kTeardown is how a recording thread reaches it. So a create-param
	// edit now takes the IDENTICAL path a resolution change does, and the "Recreate the SR feature"
	// button remains a second, equivalent route to the same seam rather than the only one.
	//
	// sr_render_preset is deliberately NOT covered, because feature_matches does not compare it:
	// a preset change cannot make create_feature release anything, so it stays a button-only key.
	dlss_sr::create_desc cd;
	cd.render_w = render_w;
	cd.render_h = render_h;
	cd.out_w    = want_out_w;
	cd.out_h    = want_out_h;
	cd.perf_quality = g_cfg.sr_perf_quality;
	cd.hw_depth     = g_cfg.sr_hw_depth;
	cd.preset       = g_cfg.sr_render_preset;
	cd.flags = (g_cfg.sr_hdr             ? dlss_sr::kFlagIsHDR          : 0u)
	         | (g_cfg.sr_mv_lowres       ? dlss_sr::kFlagMVLowRes       : 0u)
	         | (g_cfg.sr_mv_jittered     ? dlss_sr::kFlagMVJittered     : 0u)
	         | (g_cfg.sr_depth_inverted  ? dlss_sr::kFlagDepthInverted  : 0u)
	         | (g_cfg.sr_auto_exposure   ? dlss_sr::kFlagAutoExposure   : 0u)
	         | (g_cfg.sr_alpha_upscaling ? dlss_sr::kFlagAlphaUpscaling : 0u);

	// handle != nullptr is what makes this terminate: the teardown NULLS the handle, so the very
	// next pass falls straight through to create_feature and builds the feature the new values ask
	// for. It can never queue a second teardown for the same edit.
	if (st.sr_feat.handle != nullptr && !dlss_sr::feature_matches(st.sr_feat, cd))
	{
		LOGI("DLSS-SR: a CREATE-LATCHED parameter moved (PerfQualityValue %u -> %u, "
		     "DLSS.Use.HW.Depth %d -> %d, Create.Flags 0x%02x -> 0x%02x). These are latched into "
		     "the feature at CreateFeature and there is no evaluate-time equivalent, so the "
		     "feature is queued for release on the next present - the same seam a resolution "
		     "change uses, and the only one that can idle the queue first. This frame runs the "
		     "game's own TAA untouched; the next accepted dispatch creates the feature with the "
		     "new values.",
		     (unsigned)st.sr_feat.perf_quality, (unsigned)cd.perf_quality,
		     (int)st.sr_feat.hw_depth, (int)cd.hw_depth,
		     (unsigned)st.sr_feat.create_flags, (unsigned)cd.flags);
		st.pending_work.fetch_or(kTeardown, std::memory_order_relaxed);
		return;
	}

	// ---------------------------------------------------------------- SR-owned resources
	SR_STAGE("about to create/validate the SR resources");
	const bool direct = g_cfg.sr_direct_output;
	if (!direct && !dlss_sr::ensure_output(dev, st.sr_res, want_out_w, want_out_h, taa_out.fmt, &sr_log))
		SR_BAIL("the add-on's own SR output texture could not be created (see the error above)");
	if (direct && !st.logged_sr_direct)
	{
		st.logged_sr_direct = true;
		LOGI("DLSS-SR: sr_direct_output=1. The game's own TAA output UAV (u%u, res=0x%llx, %s, "
		     "%ux%u) is bound DIRECTLY as DLSS's Output. No add-on texture is allocated and no "
		     "copy-back happens - which at 4K removes a full-extent copy per frame. The resource "
		     "carries TexCreate_UAV from RDG so FAIL_RWFlagMissing cannot occur, and it is already "
		     "resting in UNORDERED_ACCESS, which is the state NGX wants, so both of the DLSS-NR "
		     "path's taa_out barriers disappear as well.",
		     taa_out_reg, (unsigned long long)taa_out.res.handle,
		     probe::format_name(taa_out.fmt), taa_out.w, taa_out.h);
	}

	// ---------------------------------------------------------------- the motion guide
	//
	// mvec_decode.hpp's PIPELINE, unchanged and shared with the DLSS-NR path. Only the TARGET is
	// SR's own, and it is allocated at the RENDER extent rather than the output extent - because
	// SR's colour input is the TAA pass's INPUT (t5), not its output. A guide at the output extent
	// would be read by DLSS at 2x the correct scale per axis under Performance mode, silently.
	// THE LADDER, and it is the DLSS-NR path's ladder rung for rung. A missing or unvalidated
	// View.ClipToPrevClip falls back to RAW, never to decode-only: decode-only hands DLSS ZERO
	// motion for every texel UE did not write - the entire static world, the sky, translucency -
	// which is the failure this pass exists to prevent and is strictly worse than a uniformly-
	// wrong guide. Decode-only is reachable ONLY because sr_mvec_reconstruct=0 asked for it.
	//
	// This is not hypothetical here. The clip failure is a PERMANENTLY LATCHED, once-logged
	// condition (view_layout_failed) with several reachable triggers, and the likeliest one is
	// specific to the configuration DLSS-SR targets: the clip-row disagreement test compares the
	// View-CB content signature against shader.info.clip_to_prev_clip_start_index, which is DXBC
	// instruction analysis of THIS shader - and under r.TemporalAA.Upsampling=1 that is a
	// different permutation (MainUpsampling) than the one the test was validated against. SR's
	// jitter uses its own cache with its own tier policy, so jitter can validate while the clip
	// path latches off, and the pass still runs.
	enum class sr_mvec_mode { raw = 0, decode_only = 1, full = 2 };
	sr_mvec_mode run_mvec = sr_mvec_mode::raw;

	const bool mvec_wanted = g_cfg.sr_mvec_decode && st.mvec.ok && !st.mvec_failed &&
	                         !st.sr_mvec_rejected &&
	                         vel_view.handle != 0 && depth_view.handle != 0;
	if (mvec_wanted && dlss_sr::ensure_mvec(dev, st.sr_res, colour.w, colour.h, &sr_log))
	{
		// Read unconditionally, exactly as the DLSS-NR path does, so the once-logged diagnostic
		// inside it fires whichever rung is taken.
		const bool clip = nr_update_clip_to_prev_clip(dev, sh, cs, st, shader, colour.w, colour.h);

		if (!g_cfg.sr_mvec_reconstruct)
			run_mvec = sr_mvec_mode::decode_only;
		else if (clip && st.clip_ok)
			run_mvec = sr_mvec_mode::full;
		// else: raw. nr_update_clip_to_prev_clip already logged exactly why, once.
	}

	if (run_mvec != sr_mvec_mode::full && !st.logged_sr_mvec_off)
	{
		st.logged_sr_mvec_off = true;
		if (run_mvec == sr_mvec_mode::raw)
			LOGW("DLSS-SR: the motion guide is the game's RAW ENCODED velocity buffer. UE4 writes "
			     "screen-space velocity with a scale AND a bias, so a normalised-integer velocity "
			     "buffer is not in absolute pixels at all and MV.Scale.X/Y can rescale a grid but "
			     "cannot remove a bias. For DLSS-NR that was survivable; for DLSS-SR it is not, "
			     "because there is no second temporal filter behind it - expect ghosting and "
			     "smearing that does not track camera motion. Check sr_mvec_decode, and the "
			     "mvec_decode / View.ClipToPrevClip messages above for the specific reason. This "
			     "message is printed once.");
		else
			LOGW("DLSS-SR: sr_mvec_reconstruct=0. The velocity texture is decoded correctly, but "
			     "every INVALID texel - which under r.BasePassOutputsVelocity=1 is still the whole "
			     "static world, the sky, translucency and every movable that did not move - is "
			     "written as EXACTLY ZERO. That is a bring-up A/B for isolating the decode from "
			     "the camera reconstruction, and it is WORSE than sr_mvec_decode=0 for actual "
			     "play. This message is printed once.");
	}

	// ================================================================ CHAIN MODE: the DLSS-NR half's CPU preconditions
	//
	// Everything chain mode needs that can fail WITHOUT GPU consequence fails HERE, above the
	// ownership point. A failure drops the DLSS-NR half only: the frame is then exactly a
	// dlss_sr=1 frame - complete, correct, upscaled, not denoised - which is strictly better than
	// bailing and handing the frame back to the game's own TAAU. That is a deliberate choice and
	// it is the one place this implementation departs from the design it was built from.
	bool  chain_run   = chain;
	bool  chain_codec = false;
	float chain_proxy_scale = 1.0f, chain_transfer = 0.0f, chain_colour_strength = 0.0f;
	// Same selector the NR-alone path uses, computed here because the chain path has its own
	// copies of every codec constant. codec_graft_ok is the build-time gate: with the renodx
	// graft shader absent, mode 1 must fall back to 0 rather than dispatch a shader that is
	// not there.
	uint32_t chain_graft_mode = 0u;
	bool  chain_encoded = false, chain_nr_ok = false, chain_decoded = false;
	bool  chain_proxy_in_srv = false, chain_out_in_srv = false, chain_result_in_srv = false;
	bool  chain_nr_reset = false;
	float chain_nr_scale_x = 1.0f, chain_nr_scale_y = 1.0f;

	if (chain_run && st.chain_nr_off)
		chain_run = false;   // run-latched after 8 failed DLSS-NR evaluates; already logged once

	if (chain_run)
	{
		// THE GEOMETRY MOVE, which is the one idea the whole chain rests on.
		//
		// DLSS-NR's textures are allocated at the COLOUR extent here, NOT at taa_out's. Chained,
		// the image DLSS-NR denoises is the TAA pass's INPUT (t5, render resolution): in the
		// MainUpsampling permutation there is no resolved image at the render resolution anywhere
		// in the frame, and denoising at 4K after the upscale would defeat the ordering this whole
		// feature exists for. Every downstream DLSS-NR site reads st.out_w/out_h, so this ONE call
		// moves all of them together - the codec's dispatch domain, the proxy, the result, the
		// Color and Output rects. It is also what makes DLSS-NR's guide extent equal DLSS-SR's BY
		// CONSTRUCTION rather than by luck: leaving nr_ensure_output on taa_out would give a
		// 3840x2160 guide against a 1920x1080 one, a silent 2x per-axis error.
		if (!nr_ensure_output(dev, st, colour.w, colour.h, colour.fmt))
		{
			chain_run = false;
			if (!st.logged_chain_out_fail)
			{
				st.logged_chain_out_fail = true;
				LOGW("DLSS-CHAIN: the DLSS-NR textures could not be prepared at the RENDER extent "
				     "%ux%u %s, so this frame is DLSS-SR ALONE - a correct upscaled frame with no "
				     "denoise, i.e. exactly dlss_sr=1. The message above says why: a geometry move "
				     "queues a teardown and self-heals on the next present, an allocation failure "
				     "is latched for this extent. This message is printed once.",
				     colour.w, colour.h, probe::format_name(colour.fmt));
			}
		}
	}

	if (chain_run)
	{
		// ONE value, computed ONCE, handed to BOTH codec dispatches - a disagreement between the
		// encode's s and the decode's is a correctness failure, not a tuning difference. Same
		// expressions as the DLSS-NR path's, and the clamps are Remix's.
		chain_proxy_scale = 1.0f / (g_cfg.paper_white_scale > 0.01f ? g_cfg.paper_white_scale : 0.01f);
		chain_graft_mode  = (st.codec_graft_ok && g_cfg.hdr_graft != 0u) ? 1u : 0u;
		chain_transfer = (g_cfg.transfer_strength < 0.0f) ? 0.0f
			: (g_cfg.transfer_strength > 1.0f ? 1.0f : g_cfg.transfer_strength);
		chain_colour_strength = (g_cfg.color_strength < 0.0f) ? 0.0f
			: (g_cfg.color_strength > 1.0f ? 1.0f : g_cfg.color_strength);

		// st.orig_ok is deliberately NOT in this predicate, unlike the DLSS-NR path's want_codec:
		// chain mode takes no pristine copy. `original` is the game's own t5 descriptor, which is
		// what colour_view carries, so its presence is the precondition instead.
		chain_codec = g_cfg.hdr_codec && !st.codec_failed && st.codec.ok &&
		              st.codec_textures_ok && colour_view.handle != 0;

		if (!chain_codec && !st.logged_chain_codec_off)
		{
			st.logged_chain_codec_off = true;
			LOGE("DLSS-CHAIN: the HDR codec is NOT running (%s), so DLSS-SR's COLOUR INPUT will be "
			     "the network's RAW DISPLAY-REFERRED answer bound as if it were linear HDR. That is "
			     "README gap 1 MAGNIFIED by the upscaler rather than merely present in the frame: "
			     "DLSS-SR accumulates and resolves an image whose transfer function is wrong. Set "
			     "hdr_codec=1. The chain still runs. This message is printed once.",
			     !g_cfg.hdr_codec        ? "hdr_codec=0"
			     : st.codec_failed       ? "its shaders or pipelines could not be built - see above"
			     : colour_view.handle == 0
			                             ? "the game's own colour SRV was not recovered at this "
			                               "dispatch, and chain mode reads the decode's `original` "
			                               "through it rather than through a copy"
			                             : "its textures could not be allocated at this extent");
		}
	}

	if (chain_run && !st.logged_chain_banner)
	{
		st.logged_chain_banner = true;
		LOGI("==================================================================");
		LOGI("DLSS-CHAIN ARMED. One accepted TAA dispatch, TWO networks, in this order:");
		LOGI("  [1] %s DLSS-NR (feature 18) at the RENDER extent %ux%u -> st.out_tex",
		     chain_codec ? "codec encode ->" : "(no codec) ->", colour.w, colour.h);
		LOGI("  [2] %s DLSS-SR (feature 1) COLOUR = %s -> u%u %ux%u",
		     chain_codec ? "codec decode ->" : "->",
		     chain_codec ? "result_tex, LINEAR HDR (the untouched original plus the network's "
		                   "residual)"
		                 : "out_tex, the network's DISPLAY-REFERRED answer (hdr_codec is off)",
		     taa_out_reg, want_out_w, want_out_h);
		LOGI("  DENOISE FIRST, THEN UPSCALE. DLSS-NR alone in this configuration would denoise "
		     "the 4K output; chained it denoises what DLSS-SR is about to resolve.");
		if (g_cfg.copy_back)
			LOGW("  copy_back=1 is IGNORED in chain mode, and it has to be: DLSS-NR's result is "
			     "%ux%u and u%u is %ux%u, and a full-subresource CopyTextureRegion between "
			     "mismatched extents is INVALID USAGE, not an error return. The only write to u%u "
			     "is DLSS-SR's.", colour.w, colour.h, taa_out_reg, taa_out.w, taa_out.h, taa_out_reg);
		if (g_cfg.history_restore)
			LOGW("  history_restore=1 is INERT in chain mode. It exists to undo the DLSS-NR "
			     "copy-back writing a denoised image into a buffer UE 4.27 extracts as TAA "
			     "history; chain mode performs no such write, so there is nothing to undo and no "
			     "pristine copy is taken.");
		LOGI("  THE CHEAPEST ON-HARDWARE CHECK: transfer_strength=0 with the codec on must be "
		     "PIXEL-IDENTICAL to dlss_chain=0/dlss_sr=1 at the same ini. At 0 the decode is "
		     "result = lerp(original, graded, 0) = original exactly, so DLSS-SR is handed the same "
		     "t5 bits it is handed today - which validates the encode, the evaluate, the decode, "
		     "the shared guide and the extra barrier independently of image quality.");
		LOGI("==================================================================");
	}

	// ================================================================ FROM HERE WE MAY OWN THE DISPATCH
	if (!suppress)
	{
		// The DLSS-NR contract, byte for byte: issue the game's dispatch exactly where it would
		// have run, and take ownership on the VERY NEXT LINE, before anything that can throw.
		SR_STAGE("about to issue the game dispatch (not suppressing)");
		cmd->dispatch(gx, gy, gz);
		issued = true;

		// The game's TAA just wrote taa_out as a UAV and, with sr_direct_output=1, NGX is about to
		// write the same resource as a UAV. Both states are unordered_access, which ReShade turns
		// into a real UAV barrier rather than a transition - which is exactly what is needed to
		// order the two writes.
		if (direct)
			cmd->barrier(taa_out.res, resource_usage::unordered_access, resource_usage::unordered_access);
	}
	else if (!st.logged_sr_suppress)
	{
		st.logged_sr_suppress = true;
		LOGW("DLSS-SR: sr_suppress_taa=1. The game's TAA Dispatch is NOT being issued - DLSS "
		     "replaces it. Ownership is reported to ReShade only AFTER a successful EvaluateFeature "
		     "AND after the D3D12 state restore, so any failure on this path leaves ReShade to "
		     "issue the game's own TAAU, which unconditionally writes every pixel of the output "
		     "view rect and therefore produces a CORRECT frame. NOTE: ReShade's event dispatch "
		     "does not short-circuit, so this suppression applies to every co-loaded add-on too "
		     "and they have no way to learn it. This message is printed once.");
	}

	bool evaluated   = false;
	bool mvec_in_srv = false;
	bool mvec_used   = false;
	ngx::Result eval_result = ngx::Result_Fail;

	// THE THROWING WINDOW, FENCED OFF - same shape and same reason as the DLSS-NR path's. An escape
	// past this point would skip probe::restore_state, which is a corrupt command list regardless
	// of what on_dispatch reports.
	try
	{
	// ---- stage 1 of 2: the motion-vector decode ------------------------------------------------
	if (run_mvec != sr_mvec_mode::raw)
	{
		// The cache sync. ReShade's command_list_impl caches _current_descriptor_heaps and
		// _current_root_signature and skips a redundant SetDescriptorHeaps or
		// SetComputeRootSignature; NGX writes the RAW list, which ReShade never sees, so the cache
		// goes stale across an evaluate. count == 0 is ReShade's own escape hatch and FORCES both.
		cmd->bind_descriptor_tables(shader_stage::all_compute, st.mvec.layout, 0, 0, nullptr);
		cmd->bind_pipeline(pipeline_stage::all_compute, st.mvec.pso);

		const resource_view mvec_srvs[2] = { vel_view, depth_view };
		descriptor_table_update mv_srv = {};
		mv_srv.binding = 0; mv_srv.array_offset = 0; mv_srv.count = 2;
		mv_srv.type = descriptor_type::shader_resource_view;
		mv_srv.descriptors = mvec_srvs;
		cmd->push_descriptors(shader_stage::compute, st.mvec.layout, mvec_decode::kParamSrvTable, mv_srv);

		descriptor_table_update mv_uav = {};
		mv_uav.binding = 0; mv_uav.array_offset = 0; mv_uav.count = 1;
		mv_uav.type = descriptor_type::unordered_access_view;
		mv_uav.descriptors = &st.sr_res.mvec_uav;
		cmd->push_descriptors(shader_stage::compute, st.mvec.layout, mvec_decode::kParamUavTable, mv_uav);

		mvec_decode::mvec_args ma;
		// The dispatch domain is the RENDER grid, which is the grid DLSS reads MotionVectors on.
		ma.out_w   = st.sr_res.mvec_w;  ma.out_h   = st.sr_res.mvec_h;
		ma.vel_w   = velocity.w;        ma.vel_h   = velocity.h;
		ma.depth_w = depth.w;           ma.depth_h = depth.h;
		// [ASSUMED] ViewRectMin == (0,0). SetupViewRect forces OutputViewRect.Min = (0,0) for
		// upsampling configs and SceneRendering.cpp shifts every view rect to the top-left, so
		// this is the same assumption the DLSS-NR path makes and it is stated there too.
		ma.view_min_x = 0.0f; ma.view_min_y = 0.0f;
		ma.view_size_x = static_cast<float>(render_w);
		ma.view_size_y = static_cast<float>(render_h);
		ma.inv_view_x  = (ma.view_size_x != 0.0f) ? 1.0f / ma.view_size_x : 0.0f;
		ma.inv_view_y  = (ma.view_size_y != 0.0f) ? 1.0f / ma.view_size_y : 0.0f;
		ma.flags = (run_mvec == sr_mvec_mode::full ? mvec_decode::kFlagReconstruct : 0u)
		         | (g_cfg.mvec_dilate              ? mvec_decode::kFlagDilate      : 0u);
		ma.pad0 = ma.pad1 = ma.pad2 = 0;
		std::memcpy(ma.clip, st.clip_to_prev, sizeof(ma.clip));

		cmd->push_constants(shader_stage::compute, st.mvec.layout, mvec_decode::kParamConstants,
		                    0, mvec_decode::kMvecConstantCount, &ma);

		cmd->dispatch(hdr_codec::group_count(st.sr_res.mvec_w), hdr_codec::group_count(st.sr_res.mvec_h), 1);

		cmd->barrier(st.sr_res.mvec_tex, resource_usage::unordered_access,
		             resource_usage::shader_resource_non_pixel);
		mvec_in_srv = true;
		mvec_used   = true;
		st.sr_mvec_frames.fetch_add(1, std::memory_order_relaxed);
	}

	// ---- CHAIN MODE, between the two stages: DENOISE, THEN UPSCALE -----------------------------
	//
	// ONE motion guide serves both networks - st.sr_res.mvec_tex, decoded just above at the render
	// extent, which the geometry move has made equal to DLSS-NR's output extent - and it is
	// already in SHADER_RESOURCE_NON_PIXEL, which is the state NGX reads a guide in. So the second
	// decode a naive chain would need does not exist, and neither does an extra barrier for it.
	if (chain_run)
	{
		if (chain_codec)
		{
			// `original` is the GAME's own t5 descriptor. It is not transitioned: it was bound as
			// an SRV to the dispatch this window replaced, so it already carries
			// NON_PIXEL_SHADER_RESOURCE, and nothing inside this window writes it. Exactly the
			// rule that leaves the velocity and depth SRVs untransitioned above.
			nr_codec_encode(cmd, st, colour_view, chain_proxy_scale, chain_proxy_in_srv);
			chain_encoded = true;
		}

		if (nr_ensure_feature(st, d3d12_cmd, st.out_w, st.out_h))
		{
			nr_eval_args na;
			na.color   = reinterpret_cast<ID3D12Resource *>(
				chain_encoded ? st.proxy_tex.handle : colour.res.handle);
			na.color_w = st.out_w;  na.color_h = st.out_h;
			na.depth   = reinterpret_cast<ID3D12Resource *>(depth.res.handle);
			na.depth_w = depth.w;   na.depth_h = depth.h;
			na.mvec    = reinterpret_cast<ID3D12Resource *>(
				mvec_used ? st.sr_res.mvec_tex.handle : velocity.res.handle);
			na.mvec_w  = mvec_used ? st.sr_res.mvec_w : velocity.w;
			na.mvec_h  = mvec_used ? st.sr_res.mvec_h : velocity.h;
			na.output  = reinterpret_cast<ID3D12Resource *>(st.out_tex.handle);
			na.out_w   = st.out_w;  na.out_h  = st.out_h;

			// FORCED to 1.0 with the decode on, for the reason the DLSS-NR path forces it: the
			// guide is already absolute pixels on this grid, so the grid ratio would double-apply.
			// Without the decode it is the ratio between the two grids, which corrects the grid
			// and can never correct UE4's ENCODING. mvec_scale_x/y still override both.
			const float cd_x = mvec_used ? 1.0f
				: (velocity.w != 0 ? static_cast<float>(st.out_w) / static_cast<float>(velocity.w) : 1.0f);
			const float cd_y = mvec_used ? 1.0f
				: (velocity.h != 0 ? static_cast<float>(st.out_h) / static_cast<float>(velocity.h) : 1.0f);
			na.scale_x = (g_cfg.mvec_scale_x != 0.0f) ? g_cfg.mvec_scale_x : cd_x;
			na.scale_y = (g_cfg.mvec_scale_y != 0.0f) ? g_cfg.mvec_scale_y : cd_y;
			chain_nr_scale_x = na.scale_x;   // kept for the proof-of-life line below
			chain_nr_scale_y = na.scale_y;

			const ngx::Result nr_r = nr_evaluate(st, d3d12_cmd, na, chain_nr_reset);

			// ---- BEGIN overlay_ui hook ----
			// The overlay's status line is the only place a player can see DLSS-NR is alive, and
			// in chain mode this is the ONLY site that publishes to it - the DLSS-NR pass's own
			// publish is on a branch chain mode never reaches.
			overlay_ui::publish_evaluate(
				static_cast<uint32_t>(nr_r), ngx::result_to_string(nr_r), !ngx::failed(nr_r),
				st.out_w, st.out_h, probe::format_name(st.out_fmt), probe::format_name(st.neural_fmt),
				velocity.w, velocity.h, na.scale_x, na.scale_y, chain_encoded,
				st.hist_restored.load(std::memory_order_relaxed),
				st.hist_dropped.load(std::memory_order_relaxed));
			// ---- END overlay_ui hook ----

			if (ngx::failed(nr_r))
			{
				if (!st.logged_chain_nr_fail)
				{
					st.logged_chain_nr_fail = true;
					LOGE("DLSS-CHAIN: the DLSS-NR EvaluateFeature FAILED: 0x%08x %s. DLSS-SR is "
					     "still run, on the game's RAW colour - so the frame is a correct upscaled "
					     "frame with no denoise, exactly dlss_sr=1. This message is printed once.",
					     (unsigned)nr_r, ngx::result_to_string(nr_r));
				}
				if (++st.chain_nr_fail_streak >= 8)
				{
					st.chain_nr_off = true;
					LOGE("DLSS-CHAIN: the DLSS-NR half has failed 8 frames running. It is latched "
					     "OFF for the rest of this run - paying for an encode, an evaluate into a "
					     "166 MB DLL and a decode every frame for an answer nothing can use is not "
					     "something to do silently. DLSS-SR keeps running alone, which is exactly "
					     "dlss_sr=1. Read the FIRST error above: it names the check that refused.");
				}
			}
			else
			{
				chain_nr_ok = true;
				st.chain_nr_fail_streak = 0;
				st.need_reset = false;
				st.evaluate_count++;
			}
		}

		// The denoised image DLSS-SR is about to read has to be FINISHED and READABLE.
		if (chain_nr_ok && chain_encoded)
		{
			nr_codec_decode(cmd, st, colour_view, chain_proxy_scale, chain_transfer,
			                chain_colour_strength, chain_graft_mode, chain_out_in_srv);
			chain_decoded = true;
			// THE ONE BARRIER CHAIN MODE ADDS. It is the decode's write-completion AND the state
			// NGX reads Color in, in one transition - the same shape as the proxy's.
			cmd->barrier(st.result_tex, resource_usage::unordered_access,
			             resource_usage::shader_resource_non_pixel);
			chain_result_in_srv = true;
		}
		else if (chain_nr_ok)
		{
			// With the codec off DLSS-SR's Color is the network's own target, so it needs the
			// transition the decode would otherwise have made for it.
			cmd->barrier(st.out_tex, resource_usage::unordered_access,
			             resource_usage::shader_resource_non_pixel);
			chain_out_in_srv = true;
		}
	}

	// ---- stage 2 of 2: CreateFeature + EvaluateFeature ------------------------------------------
	// `cd` was built ABOVE, beside the geometry latch, and the create-param latch there has already
	// refused this pass if it does not match a live feature. So by the time control reaches here
	// create_feature can only CREATE - never release - and the contract in its header holds.
	SR_STAGE("about to CreateFeature / EvaluateFeature");
	if (dlss_sr::create_feature(g_sr_snippet, st.sr_feat, d3d12_cmd, cd, &sr_log))
	{
		st.sr_render_w = render_w; st.sr_render_h = render_h;
		st.sr_out_w    = want_out_w; st.sr_out_h  = want_out_h;

		dlss_sr::evaluate_desc ed;
		// The colour INPUT is the TAA pass's own input at t5, NOT its output. That is the single
		// biggest semantic difference from DLSS-NR, which binds the RESOLVED output.
		// CHAIN MODE substitutes the DENOISED image here, and this one line is the join between
		// the two features. With the codec on it is result_tex - the untouched linear HDR original
		// plus the network's residual, in colour.fmt, at the render extent - NOT the
		// display-referred proxy, which never leaves the DLSS-NR sub-window. With the codec off it
		// is the network's raw answer (warned about, once, above). If the DLSS-NR half did not run
		// or failed, this falls back to the game's own t5 and the frame is exactly dlss_sr=1.
		ed.color  = reinterpret_cast<ID3D12Resource *>(
			chain_decoded ? st.result_tex.handle
			              : (chain_nr_ok ? st.out_tex.handle : colour.res.handle));
		ed.depth  = reinterpret_cast<ID3D12Resource *>(depth.res.handle);
		ed.mvec   = reinterpret_cast<ID3D12Resource *>(
			mvec_used ? st.sr_res.mvec_tex.handle : velocity.res.handle);
		ed.output = reinterpret_cast<ID3D12Resource *>(
			direct ? taa_out.res.handle : st.sr_res.out_tex.handle);

		ed.render_w = render_w;
		ed.render_h = render_h;
		ed.jitter_x = jitter_x;
		ed.jitter_y = jitter_y;

		// The reset signal is DERIVED, never read from View.CameraCut - that float carries only
		// View.bCameraCut and misses three of the four conditions the engine actually resets on.
		ed.reset = st.sr_jitter.reset_signalled;

		// With the decode on the guide is already absolute pixels on the RENDER grid, so the grid
		// correction has been applied inside the shader and the scale MUST be exactly 1.0 - not
		// "happens to be 1.0". Without it, the ratio between the two grids, which can correct the
		// grid but never UE4's encoding.
		const float derived_x = mvec_used ? 1.0f
			: (velocity.w != 0 ? static_cast<float>(render_w) / static_cast<float>(velocity.w) : 1.0f);
		const float derived_y = mvec_used ? 1.0f
			: (velocity.h != 0 ? static_cast<float>(render_h) / static_cast<float>(velocity.h) : 1.0f);
		ed.mv_scale_x = (g_cfg.sr_mv_scale_x != 0.0f) ? g_cfg.sr_mv_scale_x : derived_x;
		ed.mv_scale_y = (g_cfg.sr_mv_scale_y != 0.0f) ? g_cfg.sr_mv_scale_y : derived_y;

		eval_result = dlss_sr::evaluate_feature(g_sr_snippet, st.sr_feat, d3d12_cmd, ed);
		if (ngx::failed(eval_result))
		{
			if (!st.sr_feat.logged_eval_fail)
			{
				st.sr_feat.logged_eval_fail = true;
				LOGE("DLSS-SR: EvaluateFeature FAILED: 0x%08x %s. %s",
				     (unsigned)eval_result, ngx::result_to_string(eval_result),
				     dlss_sr::explain_result(eval_result));
				LOGE("DLSS-SR: the frame is still correct - %s. This message is printed once.",
				     suppress
				        ? "ownership was never reported, so ReShade issues the game's own TAAU and "
				          "that shader writes every pixel of the output view rect"
				        : "the game's TAA already ran and only the upscale was skipped");
			}

			// THE GUIDE RUNG, before the run-latch. Our decoded r16g16_float texture is the one
			// input this build changed, and a rejected guide format is the documented failure mode
			// for it. Give it back first and let the run self-heal.
			if (mvec_used && !st.sr_mvec_rejected && st.sr_eval_fail_streak == 3)
			{
				st.sr_mvec_rejected  = true;
				st.logged_sr_mvec_off = false;
				// The GUIDE'S UNITS are about to change - from absolute render-grid pixels to
				// UE4's encoded unorm - at a CONSTANT extent. NGX's temporal history was
				// accumulated against the old one and nothing else would notice, so force one
				// reset frame. This is the same latch the DLSS-NR path keys on mvec_bound_res.
				st.sr_feat.need_reset = true;
				LOGE("DLSS-SR: EvaluateFeature has failed 4 frames running with our decoded "
				     "r16g16_float motion guide bound as MotionVectors. Reverting to the game's raw "
				     "encoded velocity buffer, which is the binding NGX has already accepted on this "
				     "hardware for DLSS-NR. If the evaluate starts succeeding, the guide FORMAT is "
				     "what it would not take.");
			}
			if (++st.sr_eval_fail_streak >= 8)
			{
				st.sr_latched_off = true;
				LOGE("DLSS-SR: EvaluateFeature has failed 8 frames running. DLSS-SR is latched OFF "
				     "for the rest of this run and the game renders exactly as it does with the "
				     "add-on unloaded. Read the FIRST EvaluateFeature error above - it names the "
				     "specific check - and walk STAGING-sr.md from the rung below the one you are "
				     "on.");
			}
		}
		else
		{
			evaluated = true;
			st.sr_eval_fail_streak = 0;
			st.sr_feat.need_reset  = false;
			const uint64_t n = st.sr_evaluates.fetch_add(1, std::memory_order_relaxed) + 1;

			// THE CHAIN'S PROOF OF LIFE. This is inside DLSS-SR's success branch AND guarded on
			// the DLSS-NR half having returned Success on this same dispatch, so it cannot be
			// reached by code that merely compiled and linked - which is exactly how a feature in
			// this tree once shipped as dead code. chain_evaluates is incremented here and nowhere
			// else, and the periodic census prints it: if it is zero, the chain did not run,
			// whatever else the log says.
			if (chain_run && chain_nr_ok)
			{
				const uint64_t cn = st.chain_evaluates.fetch_add(1, std::memory_order_relaxed) + 1;
				if (cn == 1 || cn == 100)
				{
					LOGI("DLSS-CHAIN: CHAINED EVALUATE #%llu OK - BOTH networks ran on ONE accepted "
					     "dispatch.", (unsigned long long)cn);
					LOGI("  [1] DLSS-NR  feature 18, Color=%s 0x%llx %ux%u%s, MVec=0x%llx %ux%u "
					     "scale %.4f/%.4f, Output=out_tex 0x%llx, Reset=%d",
					     chain_encoded ? "the display-referred PROXY" : "the game's RAW t5",
					     (unsigned long long)(chain_encoded ? st.proxy_tex.handle : colour.res.handle),
					     st.out_w, st.out_h,
					     chain_encoded ? " (s applied)" : "",
					     (unsigned long long)(mvec_used ? st.sr_res.mvec_tex.handle : velocity.res.handle),
					     mvec_used ? st.sr_res.mvec_w : velocity.w,
					     mvec_used ? st.sr_res.mvec_h : velocity.h,
					     (double)chain_nr_scale_x, (double)chain_nr_scale_y,
					     (unsigned long long)st.out_tex.handle, (int)chain_nr_reset);
					LOGI("  [2] %s", chain_decoded
					     ? "codec decode -> result_tex, LINEAR HDR (original + residual)"
					     : "NO codec decode - DLSS-SR is reading the network's display-referred answer");
					LOGI("  [3] DLSS-SR  feature 1, Color=0x%llx (view rect %ux%u), "
					     "MotionVectors=THE SAME 0x%llx, Output=%s %ux%u, Jitter=(%.6f, %.6f), Reset=%d",
					     (unsigned long long)(chain_decoded ? st.result_tex.handle : st.out_tex.handle),
					     render_w, render_h,
					     (unsigned long long)(mvec_used ? st.sr_res.mvec_tex.handle : velocity.res.handle),
					     direct ? "the game's u0 DIRECTLY" : "the add-on's own texture",
					     want_out_w, want_out_h, (double)jitter_x, (double)jitter_y, (int)ed.reset);
					LOGI("  The DLSS-NR copy-back does not run in chain mode; the only write to u%u "
					     "is DLSS-SR's.", taa_out_reg);
				}
			}
			else if (chain_run && !chain_nr_ok && !st.logged_chain_sr_only)
			{
				st.logged_chain_sr_only = true;
				LOGW("DLSS-CHAIN: DLSS-SR evaluated but the DLSS-NR half did not, so this frame is "
				     "UPSCALED BUT NOT DENOISED - a complete, correct frame, and exactly what "
				     "dlss_sr=1 produces. The reason is above. This message is printed once; the "
				     "census's chained= counter is the running number that matters.");
			}
			st.sr_census_render_w.store(render_w, std::memory_order_relaxed);
			st.sr_census_render_h.store(render_h, std::memory_order_relaxed);
			st.sr_census_out_w.store(want_out_w, std::memory_order_relaxed);
			st.sr_census_out_h.store(want_out_h, std::memory_order_relaxed);

			// THE PROOF-OF-EXECUTION LINE. It is on the branch AFTER EvaluateFeature returned
			// Success, so it cannot be reached by a feature that merely compiled and linked - which
			// is exactly how an earlier pass in this tree shipped as dead code. The periodic census
			// in on_present prints sr_evaluates, which is incremented on this same line.
			if (n == 1 || n == 100)
			{
				LOGI("DLSS-SR: EVALUATE #%llu OK. Color=t%u res=0x%llx %s %ux%u (view rect %ux%u), "
				     "Depth=t%u %s %ux%u, MotionVectors %ux%u (%s), Output=%s %ux%u, "
				     "Jitter.Offset=(%.6f, %.6f)%s, Reset=%d, MV.Scale=(%.4f, %.4f), "
				     "suppress=%d direct=%d copy_back=%d.",
				     (unsigned long long)n, g_cfg.srv_colour,
				     (unsigned long long)colour.res.handle, probe::format_name(colour.fmt),
				     colour.w, colour.h, render_w, render_h,
				     g_cfg.srv_depth, probe::format_name(depth.fmt), depth.w, depth.h,
				     mvec_used ? st.sr_res.mvec_w : velocity.w,
				     mvec_used ? st.sr_res.mvec_h : velocity.h,
				     mvec_used ? "decoded, absolute render-grid pixels, r16g16_float"
				               : "the game's RAW encoded velocity",
				     direct ? "the game's u0 DIRECTLY" : "the add-on's own texture",
				     want_out_w, want_out_h, (double)jitter_x, (double)jitter_y,
				     (g_cfg.sr_jitter_scale_x != 1.0f || g_cfg.sr_jitter_scale_y != 1.0f)
				        ? " (sr_jitter_scale_* APPLIED)" : "",
				     (int)ed.reset, (double)ed.mv_scale_x, (double)ed.mv_scale_y,
				     (int)suppress, (int)direct, (int)g_cfg.sr_copy_back);
			}
		}
	}
	}   // end of the throwing window
	catch (const std::exception &e)
	{
		evaluated = false;
		if (!st.logged_sr_owned_throw)
		{
			st.logged_sr_owned_throw = true;
			LOGE("DLSS-SR: exception inside the owned window: %s. The state restore below still "
			     "runs. Under sr_suppress_taa=1 ownership is NOT reported, so ReShade issues the "
			     "game's own TAAU and the frame is correct. This message is printed once.", e.what());
		}
	}
	catch (...)
	{
		evaluated = false;
		if (!st.logged_sr_owned_throw)
		{
			st.logged_sr_owned_throw = true;
			LOGE("DLSS-SR: unknown exception inside the owned window. The state restore below still "
			     "runs. This message is printed once.");
		}
	}

	// Return the guide to its resting UNORDERED_ACCESS state, WHATEVER happened above - including
	// the exception path. Leaving it in NON_PIXEL_SHADER_RESOURCE would make every subsequent
	// frame's opening barrier declare a StateBefore D3D12 disagrees with.
	if (mvec_in_srv)
		cmd->barrier(st.sr_res.mvec_tex, resource_usage::shader_resource_non_pixel,
		             resource_usage::unordered_access);

	// CHAIN MODE's three, on the same UNCONDITIONAL rule and for the same reason: leaving any of
	// them in NON_PIXEL_SHADER_RESOURCE would make the next frame's opening barrier declare a
	// StateBefore D3D12 disagrees with - a validation error under the debug layer and a silently
	// wrong transition under vkd3d.
	if (chain_result_in_srv)
		cmd->barrier(st.result_tex, resource_usage::shader_resource_non_pixel,
		             resource_usage::unordered_access);
	if (chain_out_in_srv)
		cmd->barrier(st.out_tex, resource_usage::shader_resource_non_pixel,
		             resource_usage::unordered_access);
	if (chain_proxy_in_srv)
		cmd->barrier(st.proxy_tex, resource_usage::shader_resource_non_pixel,
		             resource_usage::unordered_access);

	// The last cache sync, and only when push_descriptors was actually used. It leaves ReShade's
	// cache naming the APPLICATION's heaps, which is what restore_state is about to put back.
	// Whichever of our layouts was LAST serves - the call exists for its side effect on the cache,
	// not for the binding - so chain mode's codec dispatches take priority over the mvec decode's.
	if (chain_decoded)
		cmd->bind_descriptor_tables(shader_stage::all_compute, st.codec.decode_layout, 0, 0, nullptr);
	else if (chain_encoded)
		cmd->bind_descriptor_tables(shader_stage::all_compute, st.codec.encode_layout, 0, 0, nullptr);
	else if (mvec_used)
		cmd->bind_descriptor_tables(shader_stage::all_compute, st.mvec.layout, 0, 0, nullptr);

	// Put the command list back the way NGX found it. UNCONDITIONAL: CreateFeature clobbers state
	// too, so this runs even when the evaluate was skipped or threw.
	SR_STAGE("about to restore D3D12 state");
	probe::restore_state(d3d12_cmd, plan, g_cfg.restore_graphics_root);

	// ---- the copy-back, when the add-on owns the output texture ---------------------------------
	if (evaluated && !direct && g_cfg.sr_copy_back)
	{
		if (st.sr_res.out_fmt != taa_out.fmt)
		{
			if (!st.logged_sr_copy_fmt)
			{
				st.logged_sr_copy_fmt = true;
				LOGE("DLSS-SR: the copy-back was SKIPPED - the SR output texture is %s but the "
				     "game's TAA output is %s, and CopyTextureRegion requires identical or "
				     "same-family formats. DLSS ran and the frame is unchanged.",
				     probe::format_name(st.sr_res.out_fmt), probe::format_name(taa_out.fmt));
			}
		}
		else
		{
			const resource       pair[2] = { st.sr_res.out_tex, taa_out.res };
			const resource_usage from[2] = { resource_usage::unordered_access, resource_usage::unordered_access };
			const resource_usage to[2]   = { resource_usage::copy_source,      resource_usage::copy_dest };

			cmd->barrier(2, pair, from, to);
			// The SR output is want_out_w x want_out_h and the destination texture is at least
			// that; a full-subresource copy between DIFFERENT extents is invalid usage rather than
			// an error return, so the source box is stated explicitly whenever they differ.
			if (taa_out.w == st.sr_res.out_w && taa_out.h == st.sr_res.out_h)
			{
				cmd->copy_texture_region(st.sr_res.out_tex, 0, nullptr, taa_out.res, 0, nullptr,
				                         filter_mode::min_mag_mip_point);
			}
			else
			{
				const subresource_box box = { 0u, 0u, 0u, st.sr_res.out_w, st.sr_res.out_h, 1u };
				cmd->copy_texture_region(st.sr_res.out_tex, 0, &box, taa_out.res, 0, &box,
				                         filter_mode::min_mag_mip_point);
			}
			cmd->barrier(2, pair, to, from);
		}
	}

	// ---- OWNERSHIP, under suppression, and only now ---------------------------------------------
	// Everything that can fail has run. `evaluated` is true only on the line after EvaluateFeature
	// returned Success, and restore_state has completed. Anything short of that leaves `issued`
	// false and ReShade issues the game's own TAAU over the top - a correct frame.
	if (suppress && evaluated)
	{
		issued = true;
		st.sr_suppressed.fetch_add(1, std::memory_order_relaxed);
	}
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
	// ---- BEGIN overlay_ui hook ----
	// shader_hash IS LIVE, and this is the one render-path read that cannot come through the
	// per-pass g_cfg snapshot: this block runs BEFORE st->mutex is taken (below) and BEFORE
	// overlay_ui::begin_pass, so a snapshot value could never reach it. Snapshotting it anyway
	// would have shipped a control that does nothing on the one path it names - the exact shape
	// the kParam CI gate exists to catch. So the value comes through read_ident(), which is two
	// relaxed-ish atomic loads and nothing else.
	//
	// cs->nr_epoch is what makes the change land on EVERY command list rather than on the next
	// one. The memo is cleared unconditionally on every SetPipelineState (:912-913) and by
	// cmd_shadow::reset, so the staleness window was already bounded by one command list - but it
	// was non-deterministic across concurrently recording threads, and "takes effect on some
	// command lists and not others" is worse than not being editable. An epoch is a single atomic
	// each recording thread reads for itself.
	//
	// Cost on the hot path: one acquire load, one relaxed load and a 32-bit compare, next to a
	// pointer compare that already ran here on every dispatch of every command list. This block
	// is not skippable - if it did not run, no dispatch would ever be identified and the add-on
	// would do nothing at all - so there is no question about whether the new code executes.
	const overlay_ui::ident_view nr_ident = overlay_ui::read_ident();
	if (cs->nr_checked != cs->pso || cs->nr_epoch != nr_ident.epoch)
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
				// THE ONE-LINE RE-PIN, AND IT IS LIVE. TAA_PASS_CONFIG and
				// TAA_SCREEN_PERCENTAGE_RANGE are #defines, so flipping r.TemporalAA.Upsampling or
				// dropping r.ScreenPercentage below 100 gives different DXBC and a different
				// fnv1a64 - and the ONLY symptom is NR_BAIL("this dispatch is not the target
				// shader"). sr_shader_hash carries the MainUpsampling permutation's hash without
				// disturbing the DLSS-NR pin, so the two features can be A/B'd on one install; 0
				// falls back to shader_hash, which is what dlss_sr=0 uses unconditionally.
				//
				// ALL THREE INPUTS COME OUT OF nr_ident, NOT OUT OF g_cfg, and that is the whole of
				// this hunk's merge. This block runs BEFORE st->mutex is taken and BEFORE
				// overlay_ui::begin_pass, so a g_cfg value could never be the live one here - it
				// would be whatever the ini said, for ever, while the panel showed the user's edit.
				// read_ident() takes shader_hash, dlss_sr, dlss_chain and sr_shader_hash under ONE
				// acquire, so a user who retypes both hashes and flips the feature cannot be
				// observed half way, and cs->nr_epoch invalidates the per-PSO memo on every
				// command list.
				//
				// dlss_chain IS IN THAT VIEW AND want_hash TREATS IT EXACTLY LIKE dlss_sr, because
				// chain mode only upscales in the MainUpsampling permutation - a different #define
				// set, therefore different DXBC and a different fnv1a64. Leaving it out would have
				// left chain mode pinned to the DLSS-NR hash and it would never have seen an
				// accepted dispatch, with NR_BAIL("this dispatch is not the target shader") as the
				// only symptom.
				const uint64_t want_hash = overlay_ui::want_hash(nr_ident);
				is_target = sr.is_compute && sr.dxbc_valid &&
					(want_hash != 0 ? (sr.hash == want_hash) : sr.passed_all_gates);
			}
		}
		cs->nr_checked   = cs->pso;
		cs->nr_epoch     = nr_ident.epoch;
		cs->nr_is_target = is_target;
	}
	// ---- END overlay_ui hook ----
	if (!cs->nr_is_target)
		NR_BAIL("this dispatch is not the target shader");

	device *const dev = cmd->get_device();
	if (dev == nullptr)
		NR_BAIL("command_list::get_device() returned null");

	auto *sh = probe::pd_get<probe::device_shadow>(dev, probe::kDeviceShadowGuid);
	auto *st = probe::pd_get<nr_state>(dev, kNrStateGuid);
	// With dlss_nr=0 the DLSS-NR parameter block is deliberately never allocated, so the test is
	// "at least one of the two features has a block", not "the NR block exists". At the shipping
	// defaults (dlss_nr=1, dlss_sr=0) st->params is non-null and this is the same test it was.
	if (sh == nullptr || !sh->is_d3d12 || st == nullptr ||
	    (st->params == nullptr && st->sr_feat.params == nullptr))
		NR_BAIL("device shadow or nr_state missing (params not allocated?)");
	// Any outstanding work at all, not just a teardown: the service may be about to release the
	// feature, rebuild a pipeline or allocate a fresh parameter block, and running a pass against
	// state that is one present away from being destroyed is exactly what the deferral is for.
	// Relaxed is right - a request seen one frame late costs one frame, and the acquire that
	// matters happens in the service, on the thread that acts on it.
	if (st->pending_work.load(std::memory_order_relaxed) != 0u)
		NR_BAIL("a reconfigure is pending - waiting for present");

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
	//
	// It no longer takes pending_teardown or feature_failed. Both of those were the "Reset NR
	// feature" button's route into the render path, and that button now goes through
	// nr_service_reconfigure instead - which runs from on_present UNCONDITIONALLY, so it works in
	// the one case the button exists for and the old routing could not reach: the pass has wedged
	// and begin_pass is no longer being called at all.
	//
	// st->sr_feat.need_reset RIDES ALONG as a second out-param, and it has to: begin_pass's reset
	// consequence is the ONLY thing that turns a k_reset control into a Reset frame, and DLSS-SR's
	// Reset is (ed.reset || st.sr_feat.need_reset) - a flag nothing else in this file raises except
	// a feature release, the camera-cut path and the mvec-guide rejection. Five SR controls
	// (sr_jitter_scale_x/y, sr_jitter_projection_only, sr_mv_scale_x/y) are wired at k_reset and
	// none of them moves the output geometry, so sr_try_run's own key_moved/geometry_moved seams
	// cannot stand in. Passing the flag here is what makes their "Live, one Reset frame" tooltip
	// true - and it is deliberately a raise on the EDGE, not a mirror of st->need_reset's level;
	// begin_pass's own header says why the level would be wrong in an SR-alone run.
	if (!overlay_ui::begin_pass(g_cfg, st->cfg_scratch, st->seen_pass, st->need_reset,
	                            st->sr_feat.need_reset, st->pending_res, st->codec.ok,
	                            st->codec_failed, st->orig_ok))
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

	// DLSS-NR ADDITION - the GAME's OWN descriptors for velocity and depth, kept alongside the
	// resolved descriptions so the motion-vector decode can read exactly what the game's TAA
	// reads, through the game's own view formats (the depth SRV is r32_float_x8_uint over an
	// r32_g8_typeless resource, and .x is DeviceZ - TAAStandalone.usf:1315 reads .r).
	//
	// These are CONSUMED INSIDE THIS EVENT and never stored, which is what keeps the standing
	// "NOTHING HERE EVER CREATES A VIEW ON A RESOURCE THE GAME OWNS" rule intact: that rule is
	// about caching a descriptor across frames, and about creating one per frame and leaking a
	// pool slot. This does neither.
	resource_view mvec_vel_view   = { 0 };
	resource_view mvec_depth_view = { 0 };
	// CHAIN MODE keeps the game's own COLOUR descriptor for the same reason and under the same
	// rule: the HDR codec's `original` is the render-resolution scene colour there, and reading it
	// through the game's own SRV removes a full-extent copy per frame AND the state-inference that
	// copy would need (the existing StateBefore derivation is stated for a UAV-and-SRV, NON-RTV
	// texture, and render-resolution SceneColor is RenderTargetable, so it does not transfer).
	// CONSUMED INSIDE THIS EVENT AND NEVER STORED. Caching it across frames would be a dangling
	// read with no diagnostic.
	resource_view mvec_colour_view = { 0 };

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

		if (r.dx_register_index == g_cfg.srv_depth    && bc == buffer_class::depth)    { depth    = vi; mvec_depth_view = r.view; }
		if (r.dx_register_index == g_cfg.srv_velocity && bc == buffer_class::velocity) { velocity = vi; mvec_vel_view   = r.view; }
		if (r.dx_register_index == g_cfg.srv_colour   && bc == buffer_class::colour)   { colour   = vi; mvec_colour_view = r.view; }
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

	// The OUTPUT extent, needed before the UAV can be judged. Group counts are the primary source
	// (see nr_pick_output_uav); the ini can pin it, and 0/0 selects the DLSS-NR rule.
	const uint32_t sr_tile  = (g_cfg.sr_group_tile != 0) ? g_cfg.sr_group_tile : 8u;
	const uint32_t cand_out_w = (g_cfg.sr_out_width  != 0) ? g_cfg.sr_out_width  : sr_tile * gx;
	const uint32_t cand_out_h = (g_cfg.sr_out_height != 0) ? g_cfg.sr_out_height : sr_tile * gy;

	// ---------------------------------------------------------------- CHAIN MODE: is it actually reachable?
	//
	// BOTH features must be armed - not "the add-on is armed". g_nr_armed is set when EITHER
	// snippet initialised (nr_lazy_ngx_init returns sr_ok on the DLSS-NR failure path), so it
	// cannot answer this question; the two parameter blocks can, because each is allocated only
	// after its own Init_Ext succeeded.
	bool chain_ok = g_cfg.dlss_chain &&
	                st->params != nullptr &&
	                g_sr_armed.load(std::memory_order_relaxed) && st->sr_feat.params != nullptr;

	if (g_cfg.dlss_chain && !chain_ok && !st->logged_chain_unavailable)
	{
		st->logged_chain_unavailable = true;
		LOGE("DLSS-CHAIN: dlss_chain=1 but the chain cannot run - DLSS-NR is %s and DLSS-SR is %s. "
		     "Chain mode needs BOTH snippets loaded and initialised; it never half-runs. This run "
		     "falls back to %s. The messages above name the specific failure.",
		     st->params != nullptr ? "ARMED" : "NOT armed (dlss_nr=0, nvngx_dlssnr.dll missing, or Init_Ext failed)",
		     (g_sr_armed.load(std::memory_order_relaxed) && st->sr_feat.params != nullptr)
		        ? "ARMED" : "NOT armed (nvngx_dlss.dll missing, or Init_Ext failed)",
		     (g_sr_armed.load(std::memory_order_relaxed) && st->sr_feat.params != nullptr)
		        ? (g_cfg.dlss_sr ? "DLSS-SR alone" : "the game's own TAA - set dlss_sr=1 to run DLSS-SR alone")
		        : (st->params != nullptr ? "DLSS-NR alone" : "the game's own TAA, untouched"));
	}

	// THE ENGINE.INI DETECTOR, and it is a positive, named refusal rather than an absence of
	// output. Chain mode has nothing to upscale into unless UE4 is in MainUpsampling, and the
	// symptom of getting that wrong would otherwise be a chain that runs and produces a frame
	// identical to DLSS-NR's.
	if (chain_ok && cand_out_w == colour.w && cand_out_h == colour.h)
	{
		chain_ok = false;
		if (!st->logged_chain_not_upsampling)
		{
			st->logged_chain_not_upsampling = true;
			LOGE("DLSS-CHAIN: dlss_chain=1 but this dispatch is NOT an upsampling one - the colour "
			     "input t%u is %ux%u and the output extent derived from the group counts is ALSO "
			     "%ux%u, so DLSS-SR has nothing to upscale into. Chain mode requires UE4's "
			     "MainUpsampling permutation. Put this in Engine.ini [SystemSettings]: "
			     "r.TemporalAA.Upsampling=1 / r.SecondaryScreenPercentage=100 / "
			     "r.ScreenPercentage=50 - and then RE-PIN sr_shader_hash from the probe, because "
			     "flipping Upsampling changes TAA_PASS_CONFIG, the DXBC and therefore the hash. "
			     "Falling back to %s for this run.",
			     g_cfg.srv_colour, colour.w, colour.h, cand_out_w, cand_out_h,
			     g_cfg.dlss_sr ? "DLSS-SR alone" : "DLSS-NR alone");
		}
	}

	// Chain mode needs the DLSS-SR rule (output extent >= colour extent), not the DLSS-NR one
	// (exact equality against the colour SRV) - otherwise the 4K u0 is rejected outright and the
	// pass never runs. DLSS-NR does not also try to claim that UAV: in chain mode its Output is
	// its own st.out_tex at the render extent, and taa_out appears nowhere in its half.
	const uint32_t sr_out_w = (g_cfg.dlss_sr || chain_ok) ? cand_out_w : 0u;
	const uint32_t sr_out_h = (g_cfg.dlss_sr || chain_ok) ? cand_out_h : 0u;

	nr_view_info taa_out;
	uint32_t taa_out_reg = 0;
	if (!nr_pick_output_uav(dev, *st, uavs, colour, inputs.data(), inputs.size(),
	                        sr_out_w, sr_out_h, taa_out, taa_out_reg))
		return;

	// ---------------------------------------------------------------- our own output texture
	NR_STAGE("about to create/validate the output texture");
	// DLSS-SR owns its own output texture at the OUTPUT extent (dlss_sr::ensure_output), or binds
	// u0 directly, so this DLSS-NR allocation is skipped entirely. With dlss_sr=0 the guard
	// short-circuits and the call is made exactly as before.
	// Chain mode calls nr_ensure_output itself, inside sr_try_run, at the RENDER extent - see the
	// geometry move there. Calling it here with taa_out's extent would allocate DLSS-NR's textures
	// at 4K and then immediately queue a teardown when the chain asked for 1080p.
	if (!(g_cfg.dlss_sr || chain_ok) && !nr_ensure_output(dev, *st, taa_out.w, taa_out.h, taa_out.fmt))
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
	//
	// THIS FIRES ON THE SHARED PATH, above the DLSS-SR branch, so it deliberately does NOT claim
	// the conversion is running - only the per-dispatch GAP 3 line below can say that, and only
	// for the DLSS-NR path. What it names instead is WHICH paths still bind the game's resource
	// under the current configuration, which is true in every one of them.
	if (!st->logged_depth_format &&
	    (depth.fmt == format::r32_g8_typeless || depth.fmt == format::r24_g8_typeless))
	{
		st->logged_depth_format = true;
		LOGW("DLSS-NR: the game's depth is a TYPELESS PLANAR resource (%s). NGX reads the format "
		     "from the D3D12_RESOURCE_DESC and cannot be told the view format on D3D12, so binding "
		     "it directly may be rejected with FAIL_UnsupportedInputFormat / FAIL_UnsupportedFormat "
		     "- and, short of that, hands the network something that is not a depth value at all. "
		     "%s CHAIN MODE AND DLSS-SR BIND IT DIRECTLY EITHER WAY: DLSS-SR has DLSS.Use.HW.Depth "
		     "(sr_hw_depth), a create-time channel through which the SR snippet CAN be told its "
		     "input is a hardware depth-stencil, and the DLSS-NR snippet has no equivalent - which "
		     "is the whole of README gap 3.", probe::format_name(depth.fmt),
		     g_cfg.depth_convert
		        ? "The DLSS-NR path is configured to CONVERT it into an r32_float texture of ours; "
		          "the GAP 3 line on the first accepted dispatch reports whether that actually "
		          "happened, and depth_convert=0 is the A/B against this binding."
		        : "depth_convert=0, so the DLSS-NR path binds it directly too - that is the "
		          "pre-conversion behaviour and README gap 3 stands unmitigated.");
	}

	// ---------------------------------------------------------------- DLSS SUPER RESOLUTION
	//
	// The branch point. Everything above this line is shared and unchanged: identification, the
	// SRV class quorum, the output UAV, the descriptor-table and heap identity proofs, the
	// complete restore plan, and the raw command list. Everything below it is DLSS-NR's and is
	// untouched. With dlss_sr=0 this is one predictable branch and nothing else.
	//
	// sr_try_run owns the window from here to probe::restore_state, including the decision about
	// whether the game's Dispatch gets issued at all - which is why it takes `issued` by reference
	// on exactly the same contract this function documents.
	// CHAIN MODE enters the SAME function with chain=true, which splices DLSS-NR in ahead of
	// DLSS-SR rather than duplicating any of this. If both keys are set, the chain wins: it is a
	// superset of what dlss_sr=1 does. With dlss_chain=0 and dlss_sr=0 this is one predictable
	// branch and nothing else, exactly as it is today.
	if (g_cfg.dlss_sr || chain_ok)
	{
		sr_try_run(cmd, dev, *sh, *cs, *st, shader, colour, depth, velocity,
		           mvec_vel_view, mvec_depth_view, mvec_colour_view,
		           taa_out, taa_out_reg, plan, d3d12_cmd,
		           gx, gy, gz, sr_out_w, sr_out_h, chain_ok, issued);
		return;
	}

	// ---------------------------------------------------------------- THE MOTION-VECTOR DECODE
	//
	// CPU-SIDE DECISION ONLY. Everything here can fail loudly and take a rung on the fallback
	// ladder with no GPU consequence: nothing has been issued yet, and "FROM HERE WE OWN THE
	// DISPATCH" is still below. The dispatch itself is the first stage inside the fenced window.
	//
	// THE LADDER. Every rung lands on TODAY'S BEHAVIOUR - the game's raw encoded velocity bound as
	// DLSSNR.MVec with the derived grid scale - except the one the user explicitly asked for:
	//   mvec_decode=0                     -> raw            (bit-for-bit today, gap-2 warning and all)
	//   DXBC/PSO could not be built       -> raw            (mvec_failed, run-latched)
	//   mvec_tex could not be allocated   -> raw            (mvec_tex_failed, per-resolution)
	//   no View CB / discovery failed /
	//     the two clip rows disagree      -> raw            when mvec_reconstruct=1
	//                                     -> decode only    when mvec_reconstruct=0 (asked for)
	//   EvaluateFeature fails 8 frames
	//     running with OUR guide bound     -> raw, RUN-LATCHED (mvec_eval_rejected). The one rung
	//                                        that would otherwise land on NO DENOISE AT ALL.
	//   mvec_reconstruct=0                -> decode only, invalid texels EXACTLY zero
	//
	// STALENESS IS BOUNDED EVERYWHERE. The "keep the last good matrix" behaviour belongs ONLY to
	// the two TRANSIENT per-frame paths inside nr_update_clip_to_prev_clip - the 64-byte read and
	// the plausibility test - and both give up after 30 CONSECUTIVE failures. A PERMANENT latch
	// (view_layout_failed) clears clip_ok with it, so it lands on raw instead of reprojecting the
	// static world through a frozen matrix forever: that failure is coherent and camera-INDEPENDENT,
	// i.e. strictly worse than mvec_decode=0, and the census would still call the run healthy.
	//
	// The velocity-SRV-missing rung needs nothing: the class quorum above already refuses the whole
	// pass in that case.
	//
	// WHY A MISSING ClipToPrevClip FALLS BACK TO *RAW* AND NOT TO DECODE-ONLY. Decode-only hands
	// DLSS zero motion for the entire static world, which is the failure this feature exists to
	// prevent and is strictly worse than a uniformly-wrong guide. Today's raw guide is at least
	// wrong everywhere rather than confidently wrong in one region.
	enum class mvec_mode { raw = 0, decode_only = 1, full = 2 };
	mvec_mode run_mvec = mvec_mode::raw;

	if (g_cfg.mvec_decode && st->mvec.ok && !st->mvec_failed && !st->mvec_eval_rejected && st->mvec_ok)
	{
		const bool clip = nr_update_clip_to_prev_clip(dev, *sh, *cs, *st, shader, taa_out.w, taa_out.h);

		if (!g_cfg.mvec_reconstruct)
		{
			run_mvec = mvec_mode::decode_only;
			if (!st->logged_mvec_decode_only)
			{
				st->logged_mvec_decode_only = true;
				LOGW("DLSS-NR: mvec_reconstruct=0. The velocity texture is decoded correctly, but "
				     "every INVALID texel - which under r.BasePassOutputsVelocity=1 is still the "
				     "whole static world, the sky, translucency and every movable that did not "
				     "move - is written as EXACTLY ZERO. That is a bring-up A/B for isolating the "
				     "decode from the camera reconstruction, and it is WORSE than mvec_decode=0 "
				     "for actual play. This message is printed once.");
			}
		}
		else if (clip && st->clip_ok)
		{
			run_mvec = mvec_mode::full;
		}
		// else: raw. nr_update_clip_to_prev_clip already logged exactly why, once.
	}

	st->census_mvec_mode.store(static_cast<uint32_t>(run_mvec), std::memory_order_relaxed);

	if (g_cfg.mvec_decode && run_mvec == mvec_mode::raw && !st->logged_mvec_off)
	{
		st->logged_mvec_off = true;
		LOGW("DLSS-NR: mvec_decode=1 but the decode pass is NOT running (%s). DLSSNR.MVec falls "
		     "back to the game's raw encoded velocity buffer with the derived grid scale, which is "
		     "EXACTLY the pre-decode behaviour - README gap 2 stands unmitigated for this run. "
		     "Everything else is unaffected. This message is printed once.",
		     st->mvec_failed          ? "its shader could not be compiled or its pipeline created"
		     : st->mvec_eval_rejected ? "NGX REJECTED the decoded guide, so the binding was "
		                                "REVERTED to the game's raw velocity - see the error above"
		     : !st->mvec_ok           ? "its r16g16_float target could not be allocated"
		                              : "View.ClipToPrevClip could not be located or validated, and "
		                                "reconstructing camera motion is the half that matters most");
	}

	// GAP 2, restated against the MEASURED resource - and now it says whether it is MITIGATED.
	// Leaving the old unconditional "the motion guide is meaningless" line firing while the guide
	// is in fact being decoded would be exactly as bad as the reverse.
	if (!st->logged_mvec_format &&
	    (velocity.fmt == format::r16g16b16a16_unorm || velocity.fmt == format::r16g16_unorm ||
	     velocity.fmt == format::r8g8b8a8_unorm     || velocity.fmt == format::r16g16b16a16_snorm))
	{
		st->logged_mvec_format = true;
		if (run_mvec == mvec_mode::full)
		{
			LOGI("DLSS-NR: GAP 2 ADDRESSED. The game's velocity buffer is a NORMALISED-INTEGER "
			     "resource (%s) carrying UE4's encoding scale AND bias, and it is SPARSE - but it "
			     "is no longer what DLSSNR.MVec points at. A compute pass decodes it, reconstructs "
			     "camera motion from depth through View.ClipToPrevClip wherever the texel is "
			     "invalid, and writes absolute colour-grid pixels into our own r16g16_float "
			     "texture. MVecScaleX/Y are FORCED to 1.0 so the old grid ratio (%.4f/%.4f) cannot "
			     "double-apply. Set mvec_decode=0 to A/B against the old behaviour.",
			     probe::format_name(velocity.fmt),
			     (velocity.w != 0 ? static_cast<float>(taa_out.w) / static_cast<float>(velocity.w) : 1.0f),
			     (velocity.h != 0 ? static_cast<float>(taa_out.h) / static_cast<float>(velocity.h) : 1.0f));
		}
		else if (run_mvec == mvec_mode::decode_only)
		{
			LOGW("DLSS-NR: GAP 2 PARTLY ADDRESSED - mvec_reconstruct=0. The encoding is decoded "
			     "correctly out of the %s buffer, but the SPARSITY is not handled: every texel UE "
			     "did not write becomes zero motion, which is most of the frame. Set "
			     "mvec_reconstruct=1.", probe::format_name(velocity.fmt));
		}
		else
		{
			LOGW("DLSS-NR: KNOWN GAP 2 - DLSSNR.MVec is bound to a NORMALISED-INTEGER buffer (%s). "
			     "Values in it are in [0,1] (or [-1,1]) and carry UE4's encoding scale and bias, "
			     "not absolute pixels, which is what the snippet expects. MVecScale %.4f/%.4f "
			     "corrects the GRID only. The denoise will run and report success while the motion "
			     "guide is meaningless - see README \"Known gaps\", gap 2. Set mvec_decode=1 (and "
			     "read the message above saying why the decode is not running).",
			     probe::format_name(velocity.fmt),
			     (velocity.w != 0 ? static_cast<float>(taa_out.w) / static_cast<float>(velocity.w) : 1.0f),
			     (velocity.h != 0 ? static_cast<float>(taa_out.h) / static_cast<float>(velocity.h) : 1.0f));
		}
	}

	// The game's own SRV handles for velocity and depth, kept from the resolve loop above. These
	// are pushed straight back to our own compute shader: NOTHING IS CREATED, so the standing
	// "never create a view on a game resource" rule holds, and no barrier is needed because both
	// are bound as SRVs to the compute shader that is about to run.
	if (run_mvec != mvec_mode::raw && (mvec_vel_view.handle == 0 || mvec_depth_view.handle == 0))
	{
		run_mvec = mvec_mode::raw;
		st->census_mvec_mode.store(0, std::memory_order_relaxed);
		if (!st->logged_mvec_off)
		{
			st->logged_mvec_off = true;
			LOGW("DLSS-NR: the game's own velocity/depth SRV handles were not recovered for this "
			     "dispatch, so the motion-vector decode has nothing to read. DLSSNR.MVec falls "
			     "back to the raw encoded velocity, i.e. today's behaviour. This message is "
			     "printed once.");
		}
	}

	// ---------------------------------------------------------------- THE DEPTH CONVERSION
	//
	// CPU-SIDE DECISION ONLY, exactly like the motion-vector ladder above and in the same window:
	// nothing has been issued yet, so every rung here is free.
	//
	// THE LADDER. Every rung lands on TODAY'S BEHAVIOUR - the game's own r32_g8_typeless depth
	// resource bound as DLSSNR.Depth - and none of them is fatal:
	//   depth_convert=0                    -> the game's depth   (bit-for-bit today, gap-3 warning and all)
	//   DXBC/PSO/stats could not be built  -> the game's depth   (depth_failed, run-latched)
	//   depth_tex could not be allocated   -> the game's depth   (depth_tex_failed, per-resolution)
	//   the game's depth SRV was not
	//     recovered at this dispatch       -> the game's depth   (nothing to read)
	//   EvaluateFeature fails 8 frames
	//     running with OUR depth bound     -> the game's depth, RUN-LATCHED (depth_eval_rejected)
	//
	// THE MEASUREMENT IS POLLED FIRST AND UNCONDITIONALLY, because its readback is keyed on a copy
	// that was recorded several dispatches ago and nothing else advances it. Leaving it inside the
	// run predicate would deadlock the configuration depth_convert=0/depth_detect=1: measuring()
	// is false while a readback is outstanding, so the pass would stop running, so the poll would
	// never be reached, so the readback would stay outstanding for ever.
	if (st->depth_conv.ok && st->depth_det.armed && !st->depth_det.done &&
	    depth_convert::poll(dev, st->depth_conv, st->depth_det, &nr_pipeline_log) &&
	    !st->logged_depth_det_result)
	{
		st->logged_depth_det_result = true;
		// The verdict itself was already logged by poll(). THIS line is the consequence: which
		// value DLSSNR.DepthInverted is actually going to carry, and why. Saying only one of the
		// two would leave a log that reports a measurement and a behaviour that ignores it.
		if (st->depth_det.latched != depth_convert::verdict::undecided)
		{
			const bool measured = (st->depth_det.latched == depth_convert::verdict::reversed);
			if (g_cfg.depth_inverted_pinned)
			{
				logf(measured == g_cfg.depth_inverted ? reshade::log::level::info
				                                      : reshade::log::level::warning,
				     "DLSS-NR: depth_inverted=%d is PINNED in stray_dlssnr.ini, so the measurement "
				     "is REPORTED and NOT applied. The measurement says %d. %s",
				     (int)g_cfg.depth_inverted, (int)measured,
				     measured == g_cfg.depth_inverted
				         ? "They agree - README gap 4 is now a measurement rather than an inference."
				         : "THEY DISAGREE. One of the two is wrong and the image cannot tell you "
				           "which without an A/B: remove the depth_inverted line from the ini to "
				           "let the measurement win, or leave it to keep the pin.");
			}
			else
			{
				LOGI("DLSS-NR: GAP 4 ADDRESSED. DLSSNR.DepthInverted=%d comes from the MEASUREMENT "
				     "above, not from the inference that UE 4.27 renders reversed-Z. The built-in "
				     "default would have sent %d. Put depth_inverted in stray_dlssnr.ini, or move "
				     "the DepthInverted control in the overlay, to override it for the run.",
				     (int)measured, (int)g_cfg.depth_inverted);
			}
		}
	}

	// THE STAND-DOWN. A live control that silently does nothing is the exact defect this tree has
	// already caught in itself twice, and depth_inverted IS live (overlay R1). If the user moves it
	// at ANY point - before the verdict lands or after - the control wins for the rest of the run.
	//
	// The comparison is against the LOAD-TIME value and not against a latch-time snapshot, which is
	// what makes "before the verdict lands" work: see g_depth_inverted_at_load. It is checked on
	// every accepted dispatch rather than on an edge, because the overlay's snapshot is what puts
	// the new value in g_cfg and this function has no other notification that it moved.
	if (!st->depth_det_stood_down && st->depth_det.armed && !g_cfg.depth_inverted_pinned &&
	    g_cfg.depth_inverted != g_depth_inverted_at_load)
	{
		st->depth_det_stood_down = true;
		if (!st->logged_depth_det_stand)
		{
			st->logged_depth_det_stand = true;
			LOGW("DLSS-NR: DepthInverted was changed to %d, so depth_detect STANDS DOWN for the "
			     "rest of the run and the control wins (%s). That is deliberate: a live control "
			     "the measurement quietly overrode would be a control wired to nothing. Restart to "
			     "hand the decision back to the measurement.",
			     (int)g_cfg.depth_inverted,
			     st->depth_det.latched == depth_convert::verdict::undecided
			         ? "nothing had been measured yet"
			         : (st->depth_det.latched == depth_convert::verdict::reversed
			              ? "the measurement had said reversed-Z"
			              : "the measurement had said standard-Z"));
		}
	}

	// WANTED FOR THE BINDING and WANTED FOR THE MEASUREMENT are separate questions with separate
	// answers, and the pass runs if EITHER says so. depth_convert=0 with depth_detect=1 still
	// measures - it just does not bind - and once the verdict is settled it stops dispatching
	// altogether. That decoupling is deliberate: the measurement is the more valuable of the two
	// and must not be hostage to the binding A/B.
	const bool depth_bind_wanted    = g_cfg.depth_convert && !st->depth_eval_rejected;
	const bool depth_measure_wanted = depth_convert::measuring(st->depth_conv, st->depth_det);
	const bool run_depth = st->depth_conv.ok && !st->depth_failed && st->depth_ok &&
	                       mvec_depth_view.handle != 0 &&
	                       (depth_bind_wanted || depth_measure_wanted);

	if (g_cfg.depth_convert && !depth_bind_wanted && !st->logged_depth_off)
	{
		st->logged_depth_off = true;
		LOGW("DLSS-NR: depth_convert=1 but NGX REJECTED our r32_float depth, so the binding was "
		     "REVERTED to the game's own r32_g8_typeless resource - see the error above. README "
		     "gap 3 stands unmitigated for this run. This message is printed once.");
	}
	else if (g_cfg.depth_convert && !run_depth && !st->logged_depth_off)
	{
		st->logged_depth_off = true;
		LOGW("DLSS-NR: depth_convert=1 but the conversion pass is NOT running (%s). DLSSNR.Depth "
		     "falls back to the game's own r32_g8_typeless depth resource, which is EXACTLY the "
		     "pre-conversion behaviour - README gap 3 stands unmitigated for this run. Everything "
		     "else is unaffected. This message is printed once.",
		     st->depth_failed             ? "its shader could not be compiled, or its pipeline or "
		                                    "statistics buffers could not be created"
		     : !st->depth_ok              ? "its r32_float target could not be allocated"
		                                  : "the game's own depth SRV handle was not recovered for "
		                                    "this dispatch, so there is nothing to read");
	}

	// Published for the periodic census, which cannot take this state's mutex (see hist_restored).
	// All three are written HERE, before the fenced window, so an exception inside it cannot leave
	// the census reporting a binding that never happened.
	st->census_depth_bound.store(run_depth && depth_bind_wanted, std::memory_order_relaxed);
	st->census_depth_verdict.store(static_cast<uint32_t>(st->depth_det.latched), std::memory_order_relaxed);
	st->census_depth_inverted.store(nr_depth_inverted_value(*st), std::memory_order_relaxed);

	// GAP 3, restated against the MEASURED resource and only once it is actually being converted.
	// The arm-time banner in on_present can say the pipeline exists; only here is it known that the
	// pass ran against a real depth SRV and that its output is what NGX will be handed.
	if (run_depth && depth_bind_wanted && !st->logged_depth_active)
	{
		st->logged_depth_active = true;
		LOGI("DLSS-NR: GAP 3 ADDRESSED. DLSSNR.Depth is no longer the game's %s resource: a compute "
		     "pass reads it through the game's OWN typed r32_float_x8_uint SRV and writes DeviceZ "
		     "VERBATIM into our r32_float texture at %ux%u, which is what NGX is handed. Nothing is "
		     "linearised and nothing is flipped - DLSSNR.DepthInverted still carries the reversed-Z "
		     "convention and nothing else does. Set depth_convert=0 to A/B against the old binding.",
		     probe::format_name(depth.fmt), st->out_w, st->out_h);
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
	// Which graft-back the decode uses. 0 = our additive scene-linear residual (default, and the
	// only mode whose transfer_strength=0 identity is bit-exact), 1 = the reference add-on's
	// UpgradeToneMap. This is a root constant, so changing it costs nothing but the next dispatch.
	//
	// NORMALISED TO {0, 1} HERE, unlike DLSSNR.Style. Style's unlisted values are PRESERVED
	// because they genuinely reach NGX and may mean something there, so clamping them would make
	// the overlay lie about what was sent. hdr_graft has no third behaviour - the shader branch is
	// `g_hdrGraft == 0u ? ours : theirs` - and leaving a 2 in the live block only produced a row
	// the combo refuses to draw ("2  (not a listed value - sent as-is)"), stranding the user with
	// no way back to either mode without editing the ini and restarting. The stored value now says
	// what the shader will actually do.
	//
	// And if the decode in hand was built WITHOUT the reference graft, the mode is 0 whatever was
	// asked for: dispatching 1 into the survival build's stub would return the original at every
	// colour strength, which is a silent wrong image rather than an honestly missing feature.
	const uint32_t graft_mode = (st->codec_graft_ok && g_cfg.hdr_graft != 0u) ? 1u : 0u;

	// Resources this pass moves OUT of their resting state inside the fenced window below. They
	// are put back unconditionally after the fence, so an escape cannot leave D3D12's view of a
	// resource disagreeing with ours and poison the next frame's barriers.
	bool orig_in_srv = false, proxy_in_srv = false, out_in_srv = false, mvec_in_srv = false;
	bool depth_in_srv = false;
	// True once mvec_tex actually holds THIS frame's decoded guide. Only then may it be bound as
	// DLSSNR.MVec; the intent to run the pass is not enough, because a throw could land between.
	bool mvec_used = false;
	// The same rule for depth_tex and DLSSNR.Depth: the intent to convert is not the conversion.
	// It is ALSO false whenever the pass ran purely to feed the gap-4 measurement, because in that
	// configuration (depth_convert=0, depth_detect=1) the texture is written and deliberately not
	// bound - so this must not be derived from "did the dispatch happen".
	bool depth_used = false;
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
	// ---- stage 0 of 4: THE MOTION-VECTOR DECODE ------------------------------------------------
	//
	// FIRST, and it MUST be after the game's Dispatch above rather than before it. capture_state
	// was taken far earlier and restore_state runs far later, so a compute dispatch of ours issued
	// before the game's would execute the game's TAA against OUR root signature, PSO and heaps.
	// That is a device removal, not an artefact. The pre-dispatch region is deliberately limited to
	// copies and barriers for exactly this reason.
	//
	// The game's velocity and depth are read through THEIR OWN descriptors, and are NOT
	// transitioned: they are bound as SRVs to the compute shader that just executed, so they
	// already include NON_PIXEL_SHADER_RESOURCE, and this dispatch is a second READER of the same
	// state at the same point in the list. The same rule the barrier above states.
	if (run_mvec != mvec_mode::raw)
	{
		// THE CACHE SYNC, for the same reason as the encode's below. This is now the FIRST
		// push_descriptors of the frame, so it takes over that duty; the encode's own sync stays,
		// and is idempotent (count == 0 force-issues).
		cmd->bind_descriptor_tables(shader_stage::all_compute, st->mvec.layout, 0, 0, nullptr);
		cmd->bind_pipeline(pipeline_stage::all_compute, st->mvec.pso);

		// t0 velocity, t1 depth - one contiguous table, in declaration order.
		const resource_view mvec_srvs[2] = { mvec_vel_view, mvec_depth_view };
		descriptor_table_update mv_srv = {};
		mv_srv.binding = 0; mv_srv.array_offset = 0; mv_srv.count = 2;
		mv_srv.type = descriptor_type::shader_resource_view;
		mv_srv.descriptors = mvec_srvs;
		cmd->push_descriptors(shader_stage::compute, st->mvec.layout, mvec_decode::kParamSrvTable, mv_srv);

		descriptor_table_update mv_uav = {};
		mv_uav.binding = 0; mv_uav.array_offset = 0; mv_uav.count = 1;
		mv_uav.type = descriptor_type::unordered_access_view;
		mv_uav.descriptors = &st->mvec_uav;
		cmd->push_descriptors(shader_stage::compute, st->mvec.layout, mvec_decode::kParamUavTable, mv_uav);

		mvec_decode::mvec_args ma;
		ma.out_w   = st->out_w;    ma.out_h   = st->out_h;
		ma.vel_w   = velocity.w;   ma.vel_h   = velocity.h;
		ma.depth_w = depth.w;      ma.depth_h = depth.h;
		// [ASSUMED] ViewRectMin == (0,0). ue4_jitter.hpp defines no constant for that row and
		// validates nothing there, so reading an unvalidated row and USING it would be strictly
		// worse than assuming the value every measured extent in STRAY is consistent with: colour,
		// depth and velocity are all 1920x1080 and the render fills the view. A non-zero
		// ViewRectMin would show up as a uniform smear that does not vary with camera motion.
		ma.view_min_x = 0.0f; ma.view_min_y = 0.0f;
		ma.view_size_x = (st->view_size[0] > 0.0f) ? st->view_size[0] : static_cast<float>(st->out_w);
		ma.view_size_y = (st->view_size[1] > 0.0f) ? st->view_size[1] : static_cast<float>(st->out_h);
		ma.inv_view_x  = (ma.view_size_x != 0.0f) ? 1.0f / ma.view_size_x : 0.0f;
		ma.inv_view_y  = (ma.view_size_y != 0.0f) ? 1.0f / ma.view_size_y : 0.0f;
		ma.flags = (run_mvec == mvec_mode::full ? mvec_decode::kFlagReconstruct : 0u)
		         | (g_cfg.mvec_dilate           ? mvec_decode::kFlagDilate      : 0u);
		ma.pad0 = ma.pad1 = ma.pad2 = 0;
		std::memcpy(ma.clip, st->clip_to_prev, sizeof(ma.clip));

		cmd->push_constants(shader_stage::compute, st->mvec.layout, mvec_decode::kParamConstants,
		                    0, mvec_decode::kMvecConstantCount, &ma);

		cmd->dispatch(hdr_codec::group_count(st->out_w), hdr_codec::group_count(st->out_h), 1);

		// The write-completion barrier AND the state NGX reads a guide in, in one transition -
		// the same shape as the proxy's below.
		cmd->barrier(st->mvec_tex, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel);
		mvec_in_srv = true;
		mvec_used   = true;
		st->mvec_frames.fetch_add(1, std::memory_order_relaxed);
	}

	// ---- stage 0b of 4: THE DEPTH CONVERSION ---------------------------------------------------
	//
	// Immediately after the motion-vector decode and under every one of its rules, because it is
	// the same kind of pass reading the same kind of borrowed descriptor:
	//   * it MUST be after the game's Dispatch, for the reason stage 0 states - a compute dispatch
	//     of ours issued before the game's would execute the game's TAA against OUR root signature,
	//     PSO and heaps, which is a device removal and not an artefact;
	//   * the game's depth SRV is read through ITS OWN descriptor and is NOT transitioned. It was
	//     bound as an SRV to the compute shader that just executed, so it already carries
	//     NON_PIXEL_SHADER_RESOURCE, and this dispatch is a second READER of the same state at the
	//     same point in the list. mvec_decode read the very same view three lines above.
	//
	// THE CACHE SYNC IS ISSUED HERE TOO AND IT IS NOT REDUNDANT. ReShade's command_list_impl caches
	// _current_descriptor_heaps and _current_root_signature and skips a redundant SetDescriptorHeaps
	// or SetComputeRootSignature; NGX writes the RAW list, which ReShade never sees, so the cache
	// goes stale across an evaluate. count == 0 is ReShade's own escape hatch and FORCES both. With
	// mvec_decode=0 THIS is the frame's first push_descriptors and the duty is entirely ours; with
	// it on, the call is idempotent and costs one redundant SetDescriptorHeaps.
	if (run_depth)
	{
		cmd->bind_descriptor_tables(shader_stage::all_compute, st->depth_conv.layout, 0, 0, nullptr);

		// The window sequencing (clear / accumulate / copy to readback) lives inside dispatch()
		// because getting its ORDER wrong would let half a window settle the depth convention for
		// the run - see depth_convert.hpp. The barrier below and the cache sync above are the
		// caller's, and are the only two things it does not do.
		depth_convert::dispatch(cmd, st->depth_conv, st->depth_det,
		                        mvec_depth_view, st->depth_uav,
		                        st->out_w, st->out_h, depth.w, depth.h);

		// The write-completion barrier AND the state NGX reads depth in, in one transition - the
		// same shape as the guide's above. It is issued even when the result is not going to be
		// bound (depth_convert=0, measuring only): the texture was still written as a UAV, and the
		// restore below unconditionally names SHADER_RESOURCE_NON_PIXEL as its StateBefore.
		cmd->barrier(st->depth_tex, resource_usage::unordered_access, resource_usage::shader_resource_non_pixel);
		depth_in_srv = true;
		depth_used   = depth_bind_wanted;
		st->depth_frames.fetch_add(1, std::memory_order_relaxed);
	}

	// ---- stage 1 of 4: the PRE-DENOISE ORIGINAL ------------------------------------------------
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
		nr_codec_encode(cmd, *st, st->orig_srv, proxy_scale, proxy_in_srv);
		codec_encoded = true;
	}

	// ---- stage 2 of 3: the NGX evaluate ---------------------------------------------------------
	if (nr_ensure_feature(*st, d3d12_cmd, st->out_w, st->out_h))
	{
		// The snippet is fed the PROXY, not the raw TAA output: it is a display-referred image
		// network and the TAA output is unbounded linear radiance at this point in the frame.
		// (rtx_neural_rendering.cpp:289-292 makes exactly this substitution.) The proxy is at the
		// colour extent, so the Color/Output rect equality the snippet enforces still holds.
		auto *const colour_res  = reinterpret_cast<ID3D12Resource *>(
			(codec_encoded && g_cfg.codec_bind_proxy != 0) ? st->proxy_tex.handle
			                                               : taa_out.res.handle);
		// THE DEPTH INPUT. When the conversion pass ran AND its result is being bound, this is OUR
		// r32_float texture holding DeviceZ verbatim at the colour extent; otherwise it is the
		// game's own r32_g8_typeless resource, exactly as before this feature existed. depth_used -
		// not run_depth - is the condition, for the two reasons stated at its declaration: a throw
		// could have landed between the decision and the dispatch, and the pass also runs in a
		// configuration where it deliberately does not bind (depth_convert=0, depth_detect=1).
		//
		// THE EXTENT GOES WITH THE RESOURCE. Our texture is at the COLOUR extent, which is not
		// necessarily depth.w/h - in STRAY they are equal today (both 1920x1080), so this is a
		// provable no-op here, and it is not one the moment either grid moves.
		auto *const depth_res   = reinterpret_cast<ID3D12Resource *>(
			depth_used ? st->depth_tex.handle : depth.res.handle);
		const uint32_t depth_w  = depth_used ? st->out_w : depth.w;
		const uint32_t depth_h  = depth_used ? st->out_h : depth.h;
		// THE MOTION GUIDE. When the decode pass ran this frame it is OUR r16g16_float texture on
		// the COLOUR grid, already in absolute pixels; otherwise it is the game's raw encoded
		// velocity on the velocity grid, exactly as before this feature existed. mvec_used - not
		// run_mvec - is the condition: the intent to run the pass is not enough, because a throw
		// could have landed between the decision and the dispatch.
		auto *const mvec_res    = reinterpret_cast<ID3D12Resource *>(
			mvec_used ? st->mvec_tex.handle : velocity.res.handle);
		const uint32_t mvec_w   = mvec_used ? st->out_w : velocity.w;
		const uint32_t mvec_h   = mvec_used ? st->out_h : velocity.h;
		auto *const output_res  = reinterpret_cast<ID3D12Resource *>(st->out_tex.handle);

		// MVecScaleX/Y.
		//
		// WITH THE DECODE PASS ON THE DERIVED GRID RATIO MUST NOT SURVIVE. The pass emits
		// ABSOLUTE PIXELS ON THE COLOUR GRID, so the grid correction has already been applied
		// inside the shader (it is the 0.5*ViewSize factor in the output contract) and letting
		// the old ratio through would DOUBLE-APPLY it. It is FORCED to exactly 1.0 rather than
		// left to coincide: in STRAY the ratio happens to be 1.0 today because colour and
		// velocity are both 1920x1080, so a stale ratio would be invisible here and would come
		// back as a silent 2x error the moment either grid moved.
		//
		// Without the pass this is the old behaviour untouched: the ratio between the guide grid
		// and the colour grid, which CANNOT correct UE4's velocity ENCODING - only the grid.
		//
		// mvec_scale_x/y still override BOTH paths. With the decode on, a single one of them set
		// to -1 tests a PER-AXIS sign error and nothing else. It does NOT settle the one [WEB]-only
		// link in the chain - the DLSS DIRECTION convention (previous-minus-current, which is what
		// the shader emits, versus current-minus-previous) - because getting that wrong negates
		// BOTH axes at once, and neither single-axis flip produces the doubly-negated field. Both
		// keys must be set to -1 TOGETHER for that test; if that is better than the shipped
		// binding, the output contract in mvec_decode.hpp (mvec = backN * (-0.5W, +0.5H)) must be
		// negated at source. README "Hardware A/B" spells out both rows.
		const float derived_x = mvec_used ? 1.0f
			: (velocity.w != 0 ? static_cast<float>(taa_out.w) / static_cast<float>(velocity.w) : 1.0f);
		const float derived_y = mvec_used ? 1.0f
			: (velocity.h != 0 ? static_cast<float>(taa_out.h) / static_cast<float>(velocity.h) : 1.0f);
		const float scale_x = (g_cfg.mvec_scale_x != 0.0f) ? g_cfg.mvec_scale_x : derived_x;
		const float scale_y = (g_cfg.mvec_scale_y != 0.0f) ? g_cfg.mvec_scale_y : derived_y;


		nr_eval_args ea_nr;
		ea_nr.color  = colour_res;  ea_nr.color_w = st->out_w;  ea_nr.color_h = st->out_h;
		ea_nr.depth  = depth_res;   ea_nr.depth_w = depth_w;    ea_nr.depth_h = depth_h;
		ea_nr.mvec   = mvec_res;    ea_nr.mvec_w  = mvec_w;     ea_nr.mvec_h  = mvec_h;
		ea_nr.output = output_res;  ea_nr.out_w   = st->out_w;  ea_nr.out_h   = st->out_h;
		ea_nr.scale_x = scale_x;    ea_nr.scale_y = scale_y;

		// ---- BEGIN ngx getter trace ----------------------------------------------------------
		// WHAT THIS ANSWERS, AND WHAT IT DELIBERATELY DOES NOT.
		//
		// The evaluate log line below prints the values the ADD-ON wrote. That is our side of the
		// call and it proves nothing about the snippet: a parameter can be written perfectly and
		// still never be read, or be read through a vtable slot our hand-laid table maps somewhere
		// else, in which case the snippet silently substitutes its own fallback and the image does
		// not move. Those two failures and "read correctly, but the network did not act on it"
		// produce byte-identical add-on logs.
		//
		// The trace records the CALLEE's reads: for every Get the snippet issues during this one
		// EvaluateFeature, the key it asked for, the slot it came through, whether our map had it,
		// and the number we handed back. That separates the three:
		//
		//   key absent from the trace     -> the snippet never reads it; the control cannot be live
		//   key present, MISS             -> our block did not serve it; the bug is ours
		//   key present, HIT, right value -> the value reached the snippet, and anything still
		//                                    wrong is downstream of the parameter block
		//
		// It is emphatically NOT proof that the IMAGE changed. Nothing on this side of the call
		// can be - the only evidence for that is a pixel difference between two captures at
		// different settings, which needs hardware.
		const bool tuning_moved =
			st->traced_intensity       != g_cfg.intensity              ||
			st->traced_local_tone      != g_cfg.local_tone_strength    ||
			st->traced_local_structure != g_cfg.local_structure_strength ||
			st->traced_skin_structure  != g_cfg.skin_structure_strength ||
			st->traced_use_auto_mask   != (g_cfg.use_auto_mask ? 1u : 0u) ||
			st->traced_style           != g_cfg.style;
		//
		// st->params IS the block nr_evaluate writes into and hands to the snippet, so arming it
		// here still traces the callee's reads even though the Set calls themselves now live
		// inside nr_evaluate. It is re-read rather than captured because nr_evaluate is entitled
		// to refuse the evaluate before touching it.
		ngx::parameter_block *const p = st->params;
		const bool trace_this_evaluate =
			p != nullptr && tuning_moved &&
			(st->evaluate_count == 0 ||
			 st->evaluate_count - st->last_trace_evaluate >= 30);
		if (trace_this_evaluate)
			p->get_trace.arm();
		// ---- END ngx getter trace ------------------------------------------------------------

		// THE COLOUR RECT IS st->out_w/out_h, NOT taa_out.w/h. On this path the two are equal by
		// construction - nr_ensure_output was called with taa_out.w/h a few lines above and returns
		// false unless they match what out_tex was created at - so this is a provable no-op here.
		// It is not a no-op in chain mode, where out_tex is at the RENDER extent while taa_out is
		// the 4K output UAV, and getting it wrong there is either a rejected evaluate (the snippet
		// checks Color against Output) or a silent 2x-per-axis scale error.
		bool nr_reset_sent = false;
		const ngx::Result r = nr_evaluate(*st, d3d12_cmd, ea_nr, nr_reset_sent);

		// ---- BEGIN ngx getter trace ----------------------------------------------------------
		if (trace_this_evaluate)
		{
			p->get_trace.disarm();
			st->traced_intensity       = g_cfg.intensity;
			st->traced_local_tone      = g_cfg.local_tone_strength;
			st->traced_local_structure = g_cfg.local_structure_strength;
			st->traced_skin_structure  = g_cfg.skin_structure_strength;
			st->traced_use_auto_mask   = g_cfg.use_auto_mask ? 1u : 0u;
			st->traced_style           = g_cfg.style;
			st->last_trace_evaluate    = st->evaluate_count;
			nr_log_get_trace(*st, *p);
		}
		// ---- END ngx getter trace ------------------------------------------------------------

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

			// THE MISSING RUNG. Every other rung of the fallback ladder lands on today's
			// behaviour; this one landed on NO DENOISE AT ALL, and it is reachable from the
			// shipping default (mvec_decode=1). If the evaluate keeps failing while OUR decoded
			// r16g16_float texture is the bound guide, the guide is the first thing to suspect -
			// it is the only input this build changed - so give it back and let the run self-heal
			// to the binding that was verified on this hardware. Eight frames, not one: a single
			// failure during a device-state hiccup must not throw the feature away.
			//
			// If the evaluate still fails on the raw guide, nothing has been lost - it was already
			// failing - and the log now says which binding it reverted to.
			//
			// THERE ARE NOW TWO CHANGED INPUTS AND THE LADDER TAKES THEM ONE AT A TIME. The guide
			// goes back first and the depth second, never together: reverting both at once would
			// recover the denoise while telling us NOTHING about which of the two NGX would not
			// take, which is the whole point of having the rung. mvec_rung_pending is captured
			// BEFORE the guide's own counter is touched, so the frame on which the guide is
			// reverted does not also count against the depth.
			const bool mvec_rung_pending = mvec_used && !st->mvec_eval_rejected;
			if (mvec_used && !st->mvec_eval_rejected && ++st->mvec_eval_fail_streak >= 8)
			{
				st->mvec_eval_rejected = true;
				st->logged_mvec_off    = false;   // let the ladder say why, once, for this rung
				st->logged_mvec_format = false;   // and re-state the GAP 2 verdict for the RAW guide
				LOGE("DLSS-NR: EvaluateFeature has FAILED 8 frames running with our decoded "
				     "r16g16_float motion guide bound as DLSSNR.MVec. That is the one input this "
				     "build changed, and a rejected guide format is the documented failure mode "
				     "for it, so the decode pass is being TURNED OFF FOR THIS RUN and DLSSNR.MVec "
				     "REVERTED to the game's raw encoded velocity buffer with the derived grid "
				     "scale - exactly the pre-decode binding. The guide-reset latch will issue one "
				     "NGX Reset frame on the next dispatch because the bound resource changed. If "
				     "the denoise comes back, the decoded guide is what NGX would not take: report "
				     "it, and run with mvec_decode=0 meanwhile. If it does not come back, the "
				     "motion guide was never the cause - look at the depth resource (README gap 3, "
				     "FAIL_UnsupportedInputFormat on the typeless r32_g8 depth).");
			}

			// THE SECOND RUNG, and it only starts counting once the guide's is settled. Same eight
			// frames, same reasoning, same landing place: the binding that was verified on this
			// hardware. Note that "verified" here means only "the evaluate accepted it" - the
			// typeless resource is exactly what README gap 3 says NGX cannot read the format of,
			// so falling back to it recovers the denoise and not the depth signal.
			if (!mvec_rung_pending && depth_used && !st->depth_eval_rejected &&
			    ++st->depth_eval_fail_streak >= 8)
			{
				st->depth_eval_rejected = true;
				st->logged_depth_off    = false;   // let the ladder say why, once, for this rung
				st->logged_depth_active = false;   // and re-state the GAP 3 verdict for the raw resource
				LOGE("DLSS-NR: EvaluateFeature has FAILED 8 frames running with our converted "
				     "r32_float depth bound as DLSSNR.Depth. The conversion is being TURNED OFF "
				     "FOR THIS RUN and DLSSNR.Depth REVERTED to the game's own r32_g8_typeless "
				     "resource - exactly the pre-conversion binding. If the denoise comes back, "
				     "NGX would not take a plain r32_float depth on this snippet build, which "
				     "would be a genuinely surprising result worth reporting: report it, and run "
				     "with depth_convert=0 meanwhile. Note that the depth-convention measurement "
				     "(depth_detect) is unaffected - it reads the same texels either way.");
			}
		}
		else
		{
			evaluated = true;
			st->mvec_eval_fail_streak  = 0;   // consecutive, so any success clears it
			st->depth_eval_fail_streak = 0;
			st->need_reset = false;
			st->evaluate_count++;

			// Without this the only positive evidence that the feature ran is the ABSENCE of an
			// error, and "running correctly" then looks exactly like "silently doing nothing".
			if (st->evaluate_count == 1 || st->evaluate_count == 100)
			{
				LOGI("DLSS-NR: evaluate #%llu OK. colour/output %ux%u, depth %ux%u (%s), "
				     "mvec %ux%u (%s, %s), MVecScale %.4f/%.4f, DepthInverted=%d, UseAutoMask=%d, "
				     "Intensity=%.3f LocalTone=%.3f LocalStructure=%.3f SkinStructure=%.3f "
				     "Style=%u, copy_back=%d, hdr_codec=%d, history_restore=%d.",
				     (unsigned long long)st->evaluate_count, taa_out.w, taa_out.h,
				     // THE RESOURCE ACTUALLY HANDED TO NGX, for the same reason the guide below
				     // names its own: naming the game's depth here while the conversion is running
				     // would directly contradict the GAP 3 line and misdirect the hardware A/B.
				     depth_w, depth_h,
				     depth_used ? "r32_float, CONVERTED" : probe::format_name(depth.fmt),
				     // THE RESOURCE ACTUALLY HANDED TO NGX, not the game's velocity buffer. In
				     // STRAY both are 1920x1080, so the format and the tag are the only fields
				     // that discriminate - and naming the raw buffer here directly contradicted
				     // the "GAP 2 ADDRESSED" line and misdirected the hardware A/B.
				     mvec_w, mvec_h,
				     probe::format_name(mvec_used ? format::r16g16_float : velocity.fmt),
				     mvec_used ? "decoded, absolute colour-grid pixels"
				               : "the game's RAW encoded velocity",
				     // NOT g_cfg.depth_inverted: with depth_detect=1 the value SENT can be the
				     // measured one, and a log line that printed the ini's value instead would be
				     // the precise kind of silent divergence nr_depth_inverted_value exists to
				     // prevent. See README gap 4.
				     scale_x, scale_y, (int)nr_depth_inverted_value(*st), (int)g_cfg.use_auto_mask,
				     g_cfg.intensity, g_cfg.local_tone_strength, g_cfg.local_structure_strength,
				     g_cfg.skin_structure_strength, g_cfg.style, (int)g_cfg.copy_back,
				     (int)codec_encoded, (int)(g_cfg.history_restore && g_cfg.copy_back));

				// THESE NOTES NAME A VALUE THE SNIPPET CANNOT DISTINGUISH FROM ITS NEIGHBOUR,
				// at the moment the value itself is printed. They exist because the log line
				// above faithfully printed Intensity=2.000 LocalTone=1.550 every time, which
				// reads as proof the values did something - they arrived, but 2.0 and 1.0 drive
				// identical code.
				//
				// THE INTENSITY NOTE FIRES ONLY ABOVE 1.0, NOT AT IT. An earlier revision tested
				// `>= 1.0f` and called the result "INERT ... a range regression has landed". The
				// shipped default IS 1.0, so that note fired on every stock install and told the
				// user their denoiser was doing nothing. It is the opposite: 1.0 is full-strength
				// denoise, and all the >= 1.0 branch turns off is the optional attenuation pass
				// that has nothing left to attenuate [see addon_config.hpp; the false at
				// 0x18001f51a is stored to a flag at 0x1800191bd, not returned as an abort].
				//
				// THE TEST FOR "SHOULD THIS NOTE EXIST" IS NOT "is the claim true" - it is "does
				// it fire at a value the user did not choose, to report a fault that is not
				// there". A note that fires at a default to report a REAL inertness (Style=0
				// below) passes that test; one that fires at a default to report a fault it
				// invented does not.
				if (g_cfg.intensity > 1.0f)
					LOGI("DLSS-NR:   NOTE Intensity=%.3f behaves exactly as 1.0. Above 1.0 the "
					     "snippet takes the same branch [comiss/ja at 0x18001d50a], which is "
					     "FULL denoise with no attenuation pass - not 'off'. Lower it below 1.0 "
					     "to attenuate: that moves the selector from mode 0 to mode 1 (the "
					     "ControlMask at [rcx+0x60] is null here, so the cmovne at 0x18001d53d "
					     "cannot force mode 3). Whether mode 1 then RUNS is a backend capability "
					     "bit we cannot read [bt eax,0 at 0x1800295ff]; judge it by eye.",
					     g_cfg.intensity);
				if (g_cfg.local_tone_strength > 1.0f)
					LOGI("DLSS-NR:   NOTE LocalTone=%.3f behaves exactly as 1.0. The snippet "
					     "clamps it to [0,1] itself at 0x18001d603, so this is byte-identical to "
					     "the 1.0 default.", g_cfg.local_tone_strength);
				// THE ONE NOTE THAT FIRES AT A SHIPPED DEFAULT ON PURPOSE, because at that default
				// the control genuinely is inert and the user is entitled to know BEFORE they spend
				// an evening dragging it. This is the opposite of the old ">= 1.0 INERT" note, which
				// fired at the default to report a problem that was not there.
				if (g_cfg.style == 0)
					LOGI("DLSS-NR:   NOTE Style=0: LocalTone=%.3f MOVES NOTHING. Each of the 14 "
					     "lerps at 0x18001d617.. is gated by the per-style bitmask loaded at "
					     "0x18001d606, and this build's default mask [0x1800b0da8] is 0x00000000, "
					     "so every gate is taken and no parameter is written. The only enabled "
					     "sub-entries are key 1 (mask 0x34, 3 of 14) and key 2 (mask 0x20, 1 of "
					     "14). Try Style 1 or 2 if you want this slider to do anything.",
					     g_cfg.local_tone_strength);
				if (!g_cfg.use_auto_mask)
					LOGI("DLSS-NR:   NOTE UseAutoMask=0, so the snippet substitutes -1.0f for "
					     "BOTH structure strengths at 0x18001aa84 and neither knob can matter.");
				// THE GATE THAT IS NOT OURS TO CHECK. The effective structure pair is consumed
				// at exactly one site, behind two dynamic_cast null tests (an HNetCpp::CCNetwork
				// at 0x180021cc8 tested 0x18002253f, a CCTinlayoutFusedPreBlockSwin1HLayer at
				// 0x18003f5e8 tested 0x18003f5f3). Nothing on this side of the call can observe
				// whether they succeed. Say so once, so a hardware report of "the structure
				// knobs do nothing" is read as evidence about the MODEL rather than as a bug in
				// the parameter plumbing - which the getter trace has already cleared.
				if (g_cfg.use_auto_mask)
					LOGI("DLSS-NR:   NOTE LocalStructure/SkinStructure are CONDITIONAL. The "
					     "snippet consumes them only if the loaded network dynamic_casts to "
					     "HNetCpp::CCNetwork and the layer to CCTinlayoutFusedPreBlockSwin1HLayer "
					     "[0x18002253f, 0x18003f5f3]. If either fails, both knobs and Automatic "
					     "Mask are inert TOGETHER, and no value of any of them will change the "
					     "image. This add-on cannot observe the cast; an A/B at 0.0 vs 1.0 with "
					     "Automatic Mask ON is the measurement that settles it.");
			}

			// THE GRAFT IN EFFECT, RE-STATED WHENEVER IT CHANGES - deliberately NOT folded into
			// the one-shot below. hdr_graft is a tier-0 root constant that exists to be flipped
			// mid-session from the overlay; logged once, ReShade.log would spend the rest of the
			// session ASSERTING a graft that is no longer running, and this log is the primary
			// evidence channel when the user reports what they saw during an A/B.
			if (codec_encoded && graft_mode != st->logged_graft)
			{
				st->logged_graft = graft_mode;
				LOGI("DLSS-NR: GRAFT-BACK MODE hdr_graft=%u - %s. The ENCODE is the same either "
				     "way, so the network sees the same proxy and returns the same answer; only "
				     "the way that answer is carried back differs. Their headroom term "
				     "max(0, oY - pY) is algebraically our additive residual, so the two agree on "
				     "LUMINANCE; the difference is CHROMA, in highlights at color_strength=1 and "
				     "in shadows at color_strength=0. This is a root constant: flip it in the "
				     "overlay and the next frame uses the other graft, with no feature recreate - "
				     "and this line is printed again each time it moves.",
				     (unsigned)graft_mode,
				     graft_mode == 0u
				         ? "ADDITIVE residual, result = original + (SrgbDecode(neural) - "
				           "SrgbDecode(proxy)) / s (ours). Scales RGB uniformly, so hue cannot "
				           "drift; has a chroma floor below Y = 0.001/s; and transfer_strength=0 "
				           "is a BIT-EXACT no-op at every scale"
				         : "renodx UpgradeToneMap, result = lerp(original, HueOkLab(neural * "
				           "ratio, neural), transfer_strength). Rebuilds the pixel from the "
				           "network's answer, hue-locked in OkLab with an AP1 negative clamp - a "
				           "clipped highlight is pulled toward the white point and a dark coloured "
				           "pixel keeps its chromaticity with no floor at all. transfer_strength=0 "
				           "is exact here only when paper_white_scale is a power of two");
				if (st->codec_overridden)
					LOGW("DLSS-NR: ...but a user-supplied stray_dlssnr_decode.dxbc is in use. If it "
					     "was not built from this add-on's shader source it never reads the "
					     "hdr_graft constant, and the line above describes what was REQUESTED, not "
					     "necessarily what the shader is doing. Delete the .dxbc to be sure.");
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
				     "%.6f from paper_white_scale=%.4f. The network's answer is then carried back "
				     "onto the UNTOUCHED original by the graft named on the GRAFT-BACK MODE line - "
				     "which one is a LIVE setting, so the formula is stated there and not here, "
				     "and that line is re-emitted every time the mode changes. Alpha is taken from "
				     "the ORIGINAL and never from the network, in both grafts.",
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
		nr_codec_decode(cmd, *st, st->orig_srv, proxy_scale, transfer_strength,
		                color_strength, graft_mode, out_in_srv);

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
	// mvec_tex rests in UNORDERED_ACCESS - that is the state it was created in and the StateBefore
	// the next frame's decode dispatch will name. Without this it would stay in
	// NON_PIXEL_SHADER_RESOURCE, and every subsequent frame's opening barrier would declare a
	// StateBefore that D3D12 disagrees with: a validation error under the debug layer and a
	// silently wrong transition under vkd3d. Exactly the hazard the comment above describes.
	if (mvec_in_srv)
		cmd->barrier(st->mvec_tex, resource_usage::shader_resource_non_pixel, resource_usage::unordered_access);
	// depth_tex rests in UNORDERED_ACCESS for exactly the reason mvec_tex does, and the hazard of
	// leaving it out is identical: every subsequent frame's opening barrier would declare a
	// StateBefore that D3D12 disagrees with. Note this is keyed on depth_in_srv and NOT on
	// depth_used - the pass also runs to feed the gap-4 measurement without binding its output,
	// and that run transitions the texture just the same.
	if (depth_in_srv)
		cmd->barrier(st->depth_tex, resource_usage::shader_resource_non_pixel, resource_usage::unordered_access);

	// 2c. THE LAST CACHE SYNC, and only when we actually used push_descriptors. It forces
	//     SetDescriptorHeaps and SetComputeRootSignature onto the real list one more time, and -
	//     via the _previous_descriptor_heaps path at d3d12_impl_command_list.cpp:528-535 - leaves
	//     ReShade's cache naming the APPLICATION's heaps, which is what restore_state is about to
	//     put back. Without it the cache would end this window claiming ReShade's transient heap
	//     while the raw list holds UE's, and the next push_descriptors on this command list -
	//     ours next frame, or any other add-on's - would skip a SetDescriptorHeaps it needed.
	//
	//     If NEITHER the codec NOR the motion-vector decode ran we never touched that cache, so
	//     there is nothing to re-sync and issuing this would be pure risk.
	//
	//     mvec_used is part of the condition because the decode pass calls push_descriptors too -
	//     and it is the FIRST such call of the frame. A run with hdr_codec=0 and mvec_decode=1
	//     dirties the cache and, keyed on codec_encoded alone, would never clean it: the cache
	//     would end this window naming ReShade's transient heap while the raw list holds UE's, and
	//     the next push_descriptors on this list would skip a SetDescriptorHeaps it needed. Either
	//     layout serves - the call exists for its side effect on the cache, not for the binding.
	//
	//     depth_in_srv joins the condition on exactly the same argument, and it is the one that
	//     covers hdr_codec=0 + mvec_decode=0 + depth_convert=1 - a configuration reachable from the
	//     ini alone, in which the depth pass would be the ONLY push_descriptors of the frame and,
	//     keyed on the other two, would dirty the cache and never clean it. It is depth_in_srv and
	//     not depth_used for the reason the barrier above gives: a measurement-only run pushes
	//     descriptors just the same.
	if (codec_encoded)
		cmd->bind_descriptor_tables(shader_stage::all_compute, st->codec.decode_layout, 0, 0, nullptr);
	else if (mvec_used)
		cmd->bind_descriptor_tables(shader_stage::all_compute, st->mvec.layout, 0, 0, nullptr);
	else if (depth_in_srv)
		cmd->bind_descriptor_tables(shader_stage::all_compute, st->depth_conv.layout, 0, 0, nullptr);

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
		// IMMEDIATELY after the load and nowhere else - see g_depth_inverted_at_load. Anything
		// later than this line has had the chance to be an overlay edit rather than the ini.
		g_depth_inverted_at_load = g_cfg.depth_inverted;

		// Armed HERE, above the `enabled` check below, and deliberately so: the RT census is
		// read-only instrumentation with no render-path effect, and measuring what ray tracing
		// the title runs is useful whether or not the DLSS-NR pass itself is turned on. It is
		// gated only by its own key.
		rt_census::arm(g_cfg.rt_census, g_cfg.rt_census_frames, &rt_census_log);
		// Both keys are LIVE now (overlay Diagnostics section -> a_apply_census), which is why
		// rt_census::set_live exists beside arm(): this call keeps the start-up banner, and the
		// overlay's later changes are two relaxed stores with a line of their own.
		// ---- BEGIN overlay_ui hook ----
		// Copy the live half of the freshly parsed ini into the overlay's atomics. Main thread,
		// before any dispatch and before any overlay draw, so nothing can observe a half-seeded
		// state. Also records the directory the Save button rewrites, and the baseline that the
		// "Revert to stray_dlssnr.ini" button and the dirty test compare against.
		overlay_ui::seed_from_config(g_cfg, dir);
		// ---- END overlay_ui hook ----
	}

	// overlay_ui::live_enabled(), not g_cfg.enabled, and it is the SAME VALUE here: seed_from_config
	// ran three lines above and copied the parsed key into that atomic. Reading it through the
	// overlay gives the setting exactly ONE reader in the whole add-on, which is what lets the
	// overlay's copy be authoritative for both the UI and this arm decision - and it is what makes
	// the checkbox real, because ticking it later runs nr_arm_snippet, which is this same path.
	if (!overlay_ui::live_enabled())
	{
		LOGI("DLSS-NR is DISABLED (enabled=0). The add-on is a strict no-op on the render path: "
		     "no snippet is loaded, no resource is created, and the game's dispatches are issued "
		     "by ReShade exactly as they would be with no add-on present. This is NO LONGER a "
		     "restart-only state: ticking \"Load the snippet and arm NGX\" in the overlay runs "
		     "exactly this path on the next present.");
		return;
	}

	if (dev->get_api() != device_api::d3d12)
		return;

	// The snippet is process-wide and is loaded exactly once per successful attempt.
	//
	// dlss_nr defaults to 1, so at the shipping settings this is byte for byte what it always was.
	// The predicate exists so that a run with dlss_sr=1 and dlss_nr=0 does not pay a 166 MB
	// LoadLibraryW for a feature it will not use - SR takes the accepted dispatch either way.
	// live_dlss_nr(), not g_cfg.dlss_nr, for exactly the reason live_enabled() is read above:
	// seed_from_config ran a few lines up and copied the parsed key into that atomic, so it is the
	// SAME value, and reading it through the overlay leaves the setting with one reader.
	//
	// AND A FAILURE MUST NOT RETURN FROM HERE. The DLSS-SR block below loads a SECOND, INDEPENDENT
	// snippet: a return here would mean a missing or unloadable nvngx_dlssnr.dll silently made
	// dlss_sr=1 unreachable - no LoadLibraryW of nvngx_dlss.dll, slot B never claimed,
	// g_nr_pending_init never set, so nr_lazy_ngx_init never runs, g_sr_armed stays false, and
	// nr_try_run bails on !g_nr_armed before sr_try_run is even entered. Not one of the SR
	// diagnostics would fire, because every one of them lives inside sr_try_run. nr_arm_snippet
	// already returns rather than aborting, and names the reason in the log, so the sequencing is
	// all this needs. The both-absent case is handled below, AFTER both attempts.
	//
	// THE NGX HALF IS DELIBERATELY NOT DONE HERE. NVSDK_NGX_D3D12_Init_Ext hangs when called from
	// init_device. Measured in STRAY: the log stops between "loaded nvngx_dlssnr.dll" and the
	// Init_Ext result, the process sits at ~2% CPU, and the title never reaches its menu.
	// init_device fires while the game is still inside CreateDXGIFactory1 with a half-built
	// device, and the snippet's D3D12 entry point does not tolerate that. Note the Vulkan backend
	// in our Remix build has no such problem - it is initialised from a live render path, which is
	// what this now imitates.
	//
	// So: load the module (cheap, and it is the expensive-but-safe part) and defer every call INTO
	// it to the first dispatch, on the render thread, with a device the game has finished
	// building. BOTH of those steps now live in nr_arm_snippet, so the overlay's `enabled`
	// checkbox and the require_trampoline 1 -> 0 direction run the identical code rather than a
	// second copy of it - which is the difference between a live setting and a live setting that
	// drifts out of step with start-up.
	if (overlay_ui::live_dlss_nr() && (!g_snippet_tried || !g_snippet.available))
	{
		nr_arm_snippet(dir, "enabled=1 at load");
	}
	else if (!overlay_ui::live_dlss_nr())
	{
		LOGI("DLSS-NR: dlss_nr=0, so nvngx_dlssnr.dll is neither loaded nor initialised and the "
		     "DLSS-NR evaluate never runs. This is the pure-DLSS-SR configuration - see the "
		     "\"Load and initialise DLSS-NR at all\" checkbox in the overlay's DLSS Super "
		     "Resolution section, which says why the key is launch-time.");
	}

	// ---- DLSS SUPER RESOLUTION -----------------------------------------------------------------
	// A SECOND snippet, through the trampoline's SLOT B. Loaded here on the MAIN THREAD for exactly
	// the reason the DLSS-NR one is: LoadLibraryW of a 59 MB DLL must never happen on a
	// command-list recording thread. Every call INTO it is still deferred to the first dispatch,
	// which is what nr_arm_sr_snippet's g_nr_pending_init store arranges.
	//
	// dlss_chain NEEDS THIS MODULE JUST AS MUCH AS dlss_sr DOES - it is the SECOND network in the
	// chain - so the load gate is the OR of the two. With both 0, nothing here runs. Both come
	// through the overlay's atomics rather than g_cfg for the reason live_dlss_nr() does above:
	// seed_from_config ran a few lines up and copied the parsed keys into them, so they are the
	// SAME values, and reading them here leaves each setting with one reader.
	if (overlay_ui::live_dlss_sr() || overlay_ui::live_dlss_chain())
		nr_arm_sr_snippet(dir, overlay_ui::live_dlss_chain()
		                       ? "dlss_chain=1 at load (the chain needs BOTH snippets)"
		                       : "dlss_sr=1 at load",
		                       overlay_ui::live_dlss_chain()
		                       ? "dlss_chain=1 (which needs BOTH snippets)"
		                       : "dlss_sr=1");

	// Neither snippet is present. This is the EXPECTED state for a stock install and is NOT an
	// error - and it is stated rather than returned on, because there is nothing left to return
	// from. It matters because both arms above log a per-feature reason and neither says what the
	// combination means: g_nr_pending_init was never set, so nr_lazy_ngx_init never runs, nothing
	// is ever armed, and every dispatch in the process is issued by ReShade exactly as it would be
	// with no add-on loaded.
	if (!g_snippet.available && !g_sr_snippet.available)
		LOGI("DLSS-NR/DLSS-SR: neither snippet is present, so nothing is armed and the add-on is a "
		     "strict no-op on the render path. This is the expected state for a stock install.");
}

// Runs once, on a command-list recording thread, from nr_try_run. Everything here needs a fully
// constructed device, which is exactly what init_device does not give us.
static bool nr_lazy_ngx_init(device *dev)
{
	// FIRST STATEMENT, so it is destroyed LAST - after the init lock below has been released and
	// after init_complete has been stored. Every exit of this function, present or future,
	// therefore publishes "lazy init is no longer in flight", which is the single question
	// nr_service_reconfigure's in-flight branch asks. See g_nr_init_settled for the two ways the
	// old g_nr_init_failed proxy for this got it wrong once DLSS-SR could arm independently.
	struct settled_guard
	{
		~settled_guard() { g_nr_init_settled.store(true, std::memory_order_release); }
	} settled;

	const std::wstring dir = ngx::module_directory_of(reinterpret_cast<const void *>(&nr_init_device));

	auto *st = probe::pd_create<nr_state>(dev, kNrStateGuid);
	if (st == nullptr)
		return false;

	// EVERYTHING BELOW IS UNDER st->mutex, AND NOTHING BELOW IS SAFE WITHOUT IT.
	//
	// pd_create published `st` to the whole process on the line above (set_private_data, which is
	// the first thing it does), and this function then writes ~25 fields of it over hundreds of
	// milliseconds. nr_service_reconfigure runs on the present thread from on_present, takes this
	// same mutex, and calls nr_release_feature_and_output and the pipeline builders over exactly
	// those fields. Before the reconfigure ladder the servicer could only act on a flag raised
	// from inside an accepted pass - i.e. never during this function - so the exclusion existed
	// without a lock. It no longer does: the overlay is what the user is touching while a
	// from-the-panel arm initialises. See nr_state::init_complete.
	//
	// No recursion risk: nr_try_run takes this mutex only AFTER this function has returned, and
	// nothing called from here takes it.
	std::lock_guard<std::mutex> init_lock(st->mutex);

	// Belt to init_complete's braces: a failure leaves the flag false for the life of the process,
	// which is also what the panel and the service key off. `r` is 0 for a failure that happened
	// BEFORE Init_Ext was reached, and the panel prints a result code only when there is one -
	// printing "Init_Ext returned 0x00000000" for an allocation failure would be the small kind of
	// lie this log exists not to tell.
	const auto record_init_failure = [](uint32_t r) {
		g_nr_init_result.store(r, std::memory_order_relaxed);
		g_nr_init_failed.store(true, std::memory_order_relaxed);
	};

	// The service's seen-epochs, seeded HERE rather than lazily on the first present after this
	// returns. This function is long, the user is in the overlay while it runs (that is how a
	// from-the-panel arm gets here at all), and take_reconfigure's first-call adoption would
	// otherwise swallow any rung raised in that window.
	overlay_ui::adopt_epochs(st->seen_service);

	st->d3d12 = reinterpret_cast<ID3D12Device *>(dev->get_native());
	if (st->d3d12 == nullptr)
	{
		LOGE("DLSS-NR: device::get_native() returned null; cannot initialise NGX.");
		record_init_failure(0u);
		return false;
	}

	// ---- DLSS-NR ------------------------------------------------------------------------------
	// Wrapped in a predicate that is TRUE at the shipping defaults, so this is the same sequence
	// it always was. dlss_nr=0 skips it so a pure-SR run does not initialise a feature it will not
	// evaluate; a FAILURE here no longer aborts the whole function, because DLSS-SR may still arm.
	bool nr_ok = false;
	if (overlay_ui::live_dlss_nr() && g_snippet.available)
	{
		// The snippet resolves its weights out of its own embedded WEIGHTS_HT resource, so this
		// path is only used for the log file it writes. It must be WRITABLE, or Init_Ext fails
		// with FAIL_UnableToWriteToAppDataPath.
		const ngx::Result r = g_snippet.init_ext(g_cfg.app_id, dir.c_str(), st->d3d12, ngx::kVersionApi,
		                                         nr_ngx_common_info());
		if (ngx::failed(r))
		{
			nr_log_ngx(reshade::log::level::error, "NVSDK_NGX_D3D12_Init_Ext", r);
			if (r == ngx::Result_FAIL_PlatformError)
				LOGE("DLSS-NR: Init_Ext is a GATED export. FAIL_PlatformError here almost certainly "
				     "means the snippet's caller check rejected the call. remix_nvngx.dll must be "
				     "present beside the add-on, and its forwarders must make REAL calls - a tail "
				     "jump reuses this add-on's return address and defeats the whole point.");
			if (r == ngx::Result_FAIL_UnableToWriteToAppDataPath)
				LOGE("DLSS-NR: the add-on's own directory is not writable, which is where the "
				     "snippet wants to put its log.");
			LOGE("DLSS-NR stays OFF, and it stays off for THIS PROCESS: the deferred initialiser is a "
			     "one-shot and is not cleared on failure, deliberately - the only measured fact about "
			     "Init_Ext's fragility is that it can HANG, and a hang is not a failure that degrades. "
			     "The overlay says so in the status block and reports any re-tick of `enabled` as "
			     "FAILED rather than APPLIED. Fix the cause and relaunch. The game is untouched.");
			// RECORDED, NOT RETURNED FROM, and the difference is DLSS-SR. Before SR existed this was
			// a `return false` on the spot; now the SR half below may still arm, so the failure is
			// recorded and the function carries on. g_nr_init_failed keeps exactly the meaning it
			// always had - "Init_Ext ran, failed, and cannot be retried in-process" - which is what
			// the status block and nr_service_reconfigure both key off, so re-ticking `enabled`
			// still reports FAILED rather than APPLIED. The pre-SR early exit below is what
			// preserves the old CONTROL FLOW for a dlss_sr=0 run.
			record_init_failure(static_cast<uint32_t>(r));
		}
		else
		{
			// Our own NVSDK_NGX_Parameter. The snippet exports no AllocateParameters on any
			// backend, and the SDK fallback would require the DRIVER's NGX runtime to have been
			// initialised - which is exactly the dependency this whole design exists to avoid.
			// See ngx_interop.hpp for why the vtable is laid out by hand.
			st->params = new (std::nothrow) ngx::parameter_block();
			if (st->params == nullptr)
			{
				LOGE("DLSS-NR: out of memory allocating the NGX parameter block. The pass stays off "
				     "for this process; the deferred initialiser does not retry.");
				record_init_failure(0u);
			}
			else
			{
				nr_ok = true;
			}
		}
	}
	else if (!overlay_ui::live_dlss_nr())
	{
		LOGI("DLSS-NR: dlss_nr=0, so nvngx_dlssnr.dll is neither loaded nor initialised and the "
		     "DLSS-NR evaluate never runs. This is the pure-DLSS-SR configuration.");
	}

	// THE PRE-SR EARLY EXIT, restored.
	//
	// Before DLSS-SR existed, a failed Init_Ext or a failed parameter-block allocation returned
	// from here IMMEDIATELY and nothing below - the HDR codec pipelines, the mvec pipeline - was
	// ever built. Replacing those two returns with nr_ok moved the exit past both blocks, so on
	// that failure path dlss_sr=0 would have started building pipelines a pre-SR build never
	// touched. It costs nothing at runtime and changes no pixel, but "dlss_sr=0 is byte for byte
	// the build before SR existed" has to be true as stated, not nearly true. The exit is taken
	// whenever DLSS-SR is not going to arm either; the mvec block below must stay reachable when
	// it IS, because the two features share that one pipeline.
	// The OR of the two, for the reason the load gate above is: chain mode arms the SAME SR
	// feature, so a chain run must reach everything below this exit exactly as an SR run does.
	const bool sr_will_try = (overlay_ui::live_dlss_sr() || overlay_ui::live_dlss_chain()) &&
	                         g_sr_snippet.available;
	if (!nr_ok && !sr_will_try)
	{
		if (overlay_ui::live_dlss_sr() || overlay_ui::live_dlss_chain())
			LOGE("DLSS-SR: %s but nvngx_dlss.dll never loaded (the reason is above), so "
			     "there is nothing to initialise. Neither feature is armed and the game renders "
			     "exactly as it does with the add-on unloaded.",
			     overlay_ui::live_dlss_chain() ? "dlss_chain=1" : "dlss_sr=1");
		// NO record_init_failure HERE, and that is deliberate. Reaching this line with nr_ok false
		// means one of two things: a REAL failure above has already recorded itself, or dlss_nr=0
		// and dlss_sr is not going to arm - which is a CONFIGURATION, not a failure. Recording one
		// for the configuration case would have the status block say "NGX initialisation already
		// failed in this session and cannot be retried" about a session in which nothing was ever
		// attempted, which inverts the one thing g_nr_init_failed exists to report.
		//
		// init_complete is not stored on this path either, so nr_service_reconfigure treats this
		// half-built nr_state exactly as it treats an absent one - which is correct, because there
		// is nothing in it to service.
		return false;
	}

	// ---- the HDR codec -----------------------------------------------------------------------
	// Built HERE, on the render thread, once, next to the LoadLibraryW of the snippet - never on a
	// command-list recording thread. A failure is survivable by construction: codec_failed latches,
	// the pass runs exactly as it did before the codec existed, and the reason is in the log.
	//
	// AND NOTE WHAT IS NOT HERE ANY MORE. The `else` branch of this used to read
	//
	//     st->codec_failed = true;   // not a failure, but the same "do not use it" state
	//
	// which set a RUN-LATCHED SHADER-BUILD FAILURE flag to mean "the user configured it off" - and
	// nr_release_feature_and_output deliberately never clears codec_failed, because a resolution
	// change cannot undo a failed D3DCompile. That single assignment was the entire reason
	// hdr_codec could not be turned on at runtime. It is deleted rather than cleared at
	// reconfigure time, and the difference matters: CLEARING the latch to service a config change
	// would also erase a real build failure and make the add-on retry a broken compile for ever.
	// With the assignment gone, codec_failed means only what its own comment says it means, and
	// g_cfg.hdr_codec alone gates the three sites that consult it (:1816, :2043, :3307).
	if (overlay_ui::live_hdr_codec())
	{
		nr_build_codec_pipelines(dev, *st, dir);
	}
	else
	{
		LOGW("DLSS-NR: hdr_codec=0. DLSSNR.Color is bound to the RAW TAA output: linear, "
		     "unbounded, upstream of bloom, eye adaptation and the film tone curve. That is "
		     "out-of-distribution for a display-referred network - README gap 1. The codec's "
		     "pipelines are simply not built yet; ticking HDR codec in the overlay builds them "
		     "on the next present and rebuilds the feature around them.");
	}

	// ---- the motion-vector decode --------------------------------------------------------------
	// Built HERE for exactly the same reasons as the codec above: render thread, once, never on a
	// recording thread. This is rung L1/L2 of the fallback ladder - a failure latches mvec_failed
	// for the run and DLSSNR.MVec stays on the game's raw encoded velocity, which is bit-for-bit
	// the behaviour before this feature existed (README gap 2). mvec_failed is deliberately never
	// cleared, the same rule as codec_failed.
	// The `else` branch here carried the IDENTICAL defect to the codec's, one line and the same
	// comment: `st->mvec_failed = true;` because the key was zero. mvec_failed is the run-latched
	// "the shader could not be compiled or its pipeline created" flag and is never cleared, so
	// that assignment was the whole of mvec_decode's off-to-on impossibility. Deleted, for the
	// same reason and with the same consequence: g_cfg.mvec_decode alone now gates :1830 and
	// :3040, and mvec_failed means only what it says.
	//
	// Off-to-on then needs no teardown of its own either: once st.mvec.ok is true, nr_ensure_aux
	// allocates mvec_tex on the very next pass, and the guide-reset latch keyed on mvec_bound_res
	// forces the one Reset frame by itself.
	//
	// THE PIPELINE IS SHARED WITH DLSS-SR. DLSS-NR writes into its own colour-grid target and
	// DLSS-SR into its own render-grid one, but the root signature, the PSO and the DXBC are ONE
	// set, so it is built when EITHER feature wants it. The predicate is the LIVE pair rather than
	// g_cfg's, for the same reason every other predicate in this function is: begin_pass has not
	// run yet on this first dispatch, so g_cfg still holds what the ini said even when the user has
	// already changed it and re-armed. nr_build_mvec_pipeline returns early on st.mvec.ok, so the
	// service's own reconcile is a no-op when this call already did the work.
	//
	// CHAIN MODE COUNTS AS DLSS-SR HERE, and it has to: the chained frame runs the SR half with the
	// SAME sr_mvec_decode guide, on the same shared pipeline. Leaving dlss_chain out would have
	// meant a chain run with dlss_sr=0 and mvec_decode=0 never building the pipeline that the SR
	// half of its own dispatch then asks for.
	if (overlay_ui::live_mvec_decode() ||
	    ((overlay_ui::live_dlss_sr() || overlay_ui::live_dlss_chain()) &&
	     overlay_ui::live_sr_mvec_decode()))
		nr_build_mvec_pipeline(dev, *st, dir);

	// ---- the depth conversion --------------------------------------------------------------
	// Built HERE for exactly the same reasons: render thread, once, never on a recording thread.
	// It is NOT a live control - unlike mvec_decode it has no overlay checkbox and no a_reconcile
	// arm - so g_cfg is the right thing to read and there is no live/g_cfg divergence to reason
	// about. It is built when EITHER key wants it, because depth_detect measures through the same
	// shader and must not be hostage to depth_convert's binding A/B (addon_config.hpp says why).
	if (g_cfg.depth_convert || g_cfg.depth_detect)
		nr_build_depth_pipeline(dev, *st, dir);

	// Diagnostic; returns immediately unless nr_probe=1.
	nr_build_probe_pipeline(dev, *st, dir);

	// st->params CAN BE NULL HERE NOW, which it never could before DLSS-SR: with dlss_nr=0 the
	// DLSS-NR parameter block is deliberately never allocated, and PopulateParameters_Impl takes
	// the block as its only argument. The test is not defensive tidiness - without it a pure-SR
	// run would hand the snippet a null pointer through a gated export.
	if (overlay_ui::live_populate_parameters() && g_snippet.populate_params != nullptr &&
	    st->params != nullptr)
	{
		const ngx::Result pr = g_snippet.populate_params(st->params);
		nr_log_ngx(ngx::failed(pr) ? reshade::log::level::warning : reshade::log::level::info,
		           "PopulateParameters_Impl (populate_parameters=1)", pr);
		st->serviced_populate_parameters = true;
	}
	else
	{
		st->serviced_populate_parameters = false;
	}
	// Mirrored to the panel so the checkbox can say "checked but NOT APPLIED" when the two
	// disagree. The checkbox is deliberately inert on its own click; a ticked box standing for
	// something that has not happened is a control that lies unless the panel says which.
	overlay_ui::publish_populate(st->serviced_populate_parameters);

	// =========================================================================================
	// DLSS SUPER RESOLUTION - Init_Ext and the parameter block.
	//
	// Same shape and the same reason as the DLSS-NR half above: the module was LoadLibraryW'd on
	// the main thread in nr_init_device, and every call INTO it happens here, on a render thread,
	// with a device the game has finished building. Init_Ext is a GATED export, so this call goes
	// through the trampoline's slot B - which is what nvngx_dlss.dll's caller check sees.
	// =========================================================================================
	bool sr_ok = false;
	// The OR again, and for the last time in this function: chain mode arms this same feature.
	if ((overlay_ui::live_dlss_sr() || overlay_ui::live_dlss_chain()) && g_sr_snippet.available)
	{
		const ngx::Result r = g_sr_snippet.init_ext(g_cfg.app_id, dir.c_str(), st->d3d12,
		                                            ngx::kVersionApi, nr_ngx_common_info());
		if (ngx::failed(r))
		{
			LOGE("DLSS-SR: NVSDK_NGX_D3D12_Init_Ext FAILED: 0x%08x %s. %s",
			     (unsigned)r, ngx::result_to_string(r), dlss_sr::explain_result(r));
			LOGE("DLSS-SR stays OFF. The game is untouched.");
		}
		else
		{
			st->sr_feat.params = new (std::nothrow) ngx::parameter_block();
			if (st->sr_feat.params == nullptr)
			{
				LOGE("DLSS-SR: out of memory allocating the SR NGX parameter block. The pass stays off.");
			}
			else
			{
				sr_ok = true;

				// The optimal-settings query. DIAGNOSTIC ONLY and off by default: it returns a
				// RECOMMENDED render resolution and has no power whatsoever to make UE render at
				// it - the only lever is r.ScreenPercentage. It runs on a SCRATCH block because
				// Width/Height/OutWidth/OutHeight have INVERTED meaning there.
				// live_sr_optimal_settings(), not g_cfg: this function runs BEFORE begin_pass has
				// ever written g_cfg for this device, and the key is deliberately absent from the
				// snapshot anyway, so g_cfg can only ever hold what the ini said. The neighbouring
				// arm-time predicates - live_dlss_nr(), live_hdr_codec(), live_dlss_sr() - all go
				// through the accessor for exactly this reason, and `enabled` 0 -> 1 from the panel
				// re-runs this whole function in-session, which is the case it was written for.
				if (overlay_ui::live_sr_optimal_settings() && g_sr_snippet.populate_params != nullptr)
				{
					ngx::parameter_block scratch;
					const ngx::Result pr = g_sr_snippet.populate_params(&scratch);
					if (ngx::failed(pr))
					{
						LOGW("DLSS-SR: PopulateParameters_Impl on the scratch block returned "
						     "0x%08x %s, so the optimal-settings query cannot run. %s",
						     (unsigned)pr, ngx::result_to_string(pr), dlss_sr::explain_result(pr));
					}
					else
					{
						// The callback the snippet just installed. Its own signature is
						// NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter*) - one argument, the block.
						void *cb = nullptr;
						typedef ngx::Result (__cdecl *PFN_Optimal)(void *);
						if (ngx::detail::get_voidptr(&scratch, dlss_sr::kParamOptimalCallback, &cb)
						        == ngx::Result_Success && cb != nullptr)
						{
							// The DISPLAY dims go in as Width/Height here - the opposite of
							// CreateFeature. sr_out_width/height, or 3840x2160 if unpinned.
							// The overlay's atomics for the same reason the gate above uses them:
							// g_cfg still holds the ini's numbers at arm time.
							const overlay_ui::live_block &olb = overlay_ui::live();
							const uint32_t pin_w = olb.sr_out_width.load(std::memory_order_relaxed);
							const uint32_t pin_h = olb.sr_out_height.load(std::memory_order_relaxed);
							const uint32_t disp_w = (pin_w != 0) ? pin_w : 3840u;
							const uint32_t disp_h = (pin_h != 0) ? pin_h : 2160u;
							ngx::set_u32(&scratch, dlss_sr::kParamWidth,  disp_w);
							ngx::set_u32(&scratch, dlss_sr::kParamHeight, disp_h);
							ngx::set_u32(&scratch, dlss_sr::kParamPerfQuality,
							             overlay_ui::live_sr_perf_quality());

							const ngx::Result orr = reinterpret_cast<PFN_Optimal>(cb)(&scratch);
							unsigned int ow = 0, oh = 0, mxw = 0, mxh = 0, mnw = 0, mnh = 0;
							ngx::detail::get_uint(&scratch, dlss_sr::kParamOutWidth,     &ow);
							ngx::detail::get_uint(&scratch, dlss_sr::kParamOutHeight,    &oh);
							ngx::detail::get_uint(&scratch, dlss_sr::kParamDynMaxRenderW, &mxw);
							ngx::detail::get_uint(&scratch, dlss_sr::kParamDynMaxRenderH, &mxh);
							ngx::detail::get_uint(&scratch, dlss_sr::kParamDynMinRenderW, &mnw);
							ngx::detail::get_uint(&scratch, dlss_sr::kParamDynMinRenderH, &mnh);
							LOGI("DLSS-SR: DLSS_GetOptimalSettings(display %ux%u, PerfQuality=%s) "
							     "returned 0x%08x %s - optimal RENDER %ux%u, dynamic range "
							     "%ux%u .. %ux%u. THIS IS ADVISORY ONLY: nothing here can make UE "
							     "render at that resolution, r.ScreenPercentage is the only lever. "
							     "Use it to choose the number you put in Engine.ini.",
							     disp_w, disp_h,
							     dlss_sr::perf_quality_name(overlay_ui::live_sr_perf_quality()),
							     (unsigned)orr, ngx::result_to_string(orr), ow, oh, mxw, mxh, mnw, mnh);
						}
						else
						{
							LOGW("DLSS-SR: PopulateParameters_Impl succeeded but "
							     "DLSSOptimalSettingsCallback was not populated in the scratch "
							     "block, so the optimal-settings query cannot run. This is exactly "
							     "the disagreement recorded in STAGING-sr.md: the disassembly says "
							     "the callback IS written into whatever block is handed in, and "
							     "this run says otherwise. Nothing depends on it.");
						}
					}
				}
			}
		}
	}
	g_sr_armed.store(sr_ok, std::memory_order_release);

	if (sr_ok)
	{
		LOGI("==================================================================");
		LOGI("DLSS-SR ARMED. NGX feature %u (SuperSampling), nvngx_dlss.dll, through "
		     "remix_nvngx.dll slot B.", dlss_sr::kFeatureSuperSampling);
		// The keys this pass made LIVE are printed from the OVERLAY'S atomics, not from g_cfg, for
		// exactly the reason the DLSS-NR banner below does the same: begin_pass has not run yet on
		// this first dispatch, so g_cfg still holds what the ini said even when the user has
		// already changed it - and reporting the ini's value for a setting the user has edited is
		// the sort of small lie this log exists to not tell. The keys that are NOT live yet are
		// still read from g_cfg, because for those g_cfg IS the only value there is.
		// THROUGH read_ident/want_hash, not g_cfg, and this line is why the rule above is stated
		// rather than assumed: shader_hash and sr_shader_hash are LIVE, but they are deliberately
		// absent from the per-pass snapshot (their read site, read_ident(), runs before begin_pass),
		// so g_cfg still holds whatever the ini said. A user who pastes the MainUpsampling hash in,
		// presses Apply SR hash and then arms would have read the OLD value here - from the one
		// line they look at to confirm the re-pin took, while the render path at nr_try_run was
		// already matching against the new one. ONE ident_view feeds both, so the banner and the
		// identification path cannot disagree.
		const overlay_ui::ident_view banner_ident = overlay_ui::read_ident();
		LOGI("  target shader   0x%016llx%s",
		     (unsigned long long)overlay_ui::want_hash(banner_ident),
		     (banner_ident.sr_shader_hash != 0) ? "  (sr_shader_hash)" : "  (shader_hash - re-pin with sr_shader_hash after flipping r.TemporalAA.Upsampling)");
		// ONE reference to the live block for the whole banner, exactly as the DLSS-NR banner below
		// takes one. Seventeen keys on the lines that follow - the output geometry, the ladder, all
		// six create flags, sr_hw_depth, the jitter pair, sr_jitter_projection_only and the mvec
		// scale pair - were moved into OVERLAY_OWNED_FIELDS and the per-pass snapshot by the same
		// pass that wrote the rule at the top of this block, and every one of them was still being
		// printed from g_cfg. g_cfg has exactly two writers, cfg::load and begin_pass, and this
		// function runs from nr_try_run's one-shot ABOVE the begin_pass call - so at banner time
		// g_cfg holds the ini's values and never the user's. The banner is the bring-up instrument
		// for the very A/B rungs these keys exist to drive, and it was reporting the numbers the
		// file shipped with while the next dispatch used the ones on screen.
		const overlay_ui::live_block &sb = overlay_ui::live();
		const uint32_t ban_out_w = sb.sr_out_width.load(std::memory_order_relaxed);
		const uint32_t ban_out_h = sb.sr_out_height.load(std::memory_order_relaxed);
		const float    ban_jit_x = sb.sr_jitter_scale_x.load(std::memory_order_relaxed);
		const float    ban_jit_y = sb.sr_jitter_scale_y.load(std::memory_order_relaxed);
		const bool     ban_hdr   = sb.sr_hdr.load(std::memory_order_relaxed);
		const bool     ban_mvlr  = sb.sr_mv_lowres.load(std::memory_order_relaxed);
		const bool     ban_mvj   = sb.sr_mv_jittered.load(std::memory_order_relaxed);
		const bool     ban_dinv  = sb.sr_depth_inverted.load(std::memory_order_relaxed);
		const bool     ban_ae    = sb.sr_auto_exposure.load(std::memory_order_relaxed);
		const bool     ban_alpha = sb.sr_alpha_upscaling.load(std::memory_order_relaxed);
		LOGI("  geometry        output %s, view rect %s, tile %u",
		     (ban_out_w != 0 || ban_out_h != 0)
		        ? "PINNED by sr_out_width/sr_out_height" : "derived from the dispatch group counts",
		     sb.sr_use_view_rect.load(std::memory_order_relaxed)
		        ? "from ViewSizeAndInvSize" : "the colour TEXTURE extent",
		     (unsigned)sb.sr_group_tile.load(std::memory_order_relaxed));
		LOGI("  ladder          sr_suppress_taa=%d sr_direct_output=%d sr_copy_back=%d",
		     (int)overlay_ui::live_sr_suppress_taa(), (int)overlay_ui::live_sr_direct_output(),
		     (int)overlay_ui::live_sr_copy_back());
		if (overlay_ui::live_dlss_chain())
			LOGI("  CHAIN MODE      dlss_chain=1: DLSS-NR runs FIRST, at the render extent, and its "
			     "denoised result becomes this feature's COLOUR INPUT. DLSS-NR is %s. The chain is "
			     "only entered when BOTH are armed; the line that proves it ran is "
			     "\"DLSS-CHAIN: CHAINED EVALUATE #1 OK\" and the census's chained= counter.",
			     (st->params != nullptr) ? "ARMED" : "NOT ARMED, so the chain will NOT run");
		LOGI("  create          PerfQualityValue=%s DLSS.Use.HW.Depth=%d Create.Flags=0x%02x "
		     "[IsHDR=%d MVLowRes=%d MVJittered=%d DepthInverted=%d AutoExposure=%d AlphaUpscaling=%d]",
		     dlss_sr::perf_quality_name(overlay_ui::live_sr_perf_quality()),
		     (int)sb.sr_hw_depth.load(std::memory_order_relaxed),
		     (unsigned)((ban_hdr   ? dlss_sr::kFlagIsHDR          : 0u)
		              | (ban_mvlr  ? dlss_sr::kFlagMVLowRes       : 0u)
		              | (ban_mvj   ? dlss_sr::kFlagMVJittered     : 0u)
		              | (ban_dinv  ? dlss_sr::kFlagDepthInverted  : 0u)
		              | (ban_ae    ? dlss_sr::kFlagAutoExposure   : 0u)
		              | (ban_alpha ? dlss_sr::kFlagAlphaUpscaling : 0u)),
		     (int)ban_hdr, (int)ban_mvlr, (int)ban_mvj,
		     (int)ban_dinv, (int)ban_ae, (int)ban_alpha);
		LOGI("  jitter          scale=(%.3f, %.3f)%s tier=%s",
		     (double)ban_jit_x, (double)ban_jit_y,
		     (ban_jit_x != 1.0f || ban_jit_y != 1.0f)
		        ? "  <-- OVERRIDDEN, this is the sign A/B" : "",
		     sb.sr_jitter_projection_only.load(std::memory_order_relaxed)
		        ? "projection_only permitted" : "strict (full/no_params)");
		LOGI("  motion guide    sr_mvec_decode=%d sr_mvec_reconstruct=%d (pipeline %s) scale=%s/%s",
		     (int)overlay_ui::live_sr_mvec_decode(), (int)overlay_ui::live_sr_mvec_reconstruct(),
		     st->mvec.ok ? "built" : "NOT BUILT",
		     sb.sr_mv_scale_x.load(std::memory_order_relaxed) != 0.0f ? "OVERRIDDEN" : "auto",
		     sb.sr_mv_scale_y.load(std::memory_order_relaxed) != 0.0f ? "OVERRIDDEN" : "auto");
		LOGW("DLSS-SR: the confirmation that the feature actually RAN is the line "
		     "\"DLSS-SR: EVALUATE #1 OK\", printed from the branch immediately after "
		     "EvaluateFeature returned Success, and the periodic \"--- DLSS-SR @ frame N\" census "
		     "line, whose evaluates= counter is incremented on that same branch. If neither "
		     "appears, the pass did not run and a one-shot \"DLSS-SR: pass did not run - <reason>\" "
		     "line names the stage that refused. Nothing here reports success from the absence of "
		     "an error.");
		LOGI("==================================================================");
	}
	else if (overlay_ui::live_dlss_sr() || overlay_ui::live_dlss_chain())
	{
		LOGE("DLSS-SR: %s but the feature could NOT be armed (see above). The SR pass will not run "
		     "and the game's own TAA is untouched.",
		     overlay_ui::live_dlss_chain() ? "dlss_chain=1" : "dlss_sr=1");
	}

	// DLSS-NR DID NOT ARM, SO ITS BANNER MUST NOT PRINT - BUT THE FUNCTION MUST NOT RETURN HERE.
	//
	// The pre-merge code read `if (!nr_ok) return sr_ok;`, which returned WITHOUT storing
	// init_complete. On the dlss_nr=0, dlss_sr=1 configuration that is a live nr_state - holding
	// the SR feature, the SR parameter block and the SR textures - that nr_service_reconfigure
	// treats as ABSENT for the rest of the process, because absent is exactly how it must treat a
	// state whose init flag is false. Every SR reconfigure, including the teardown that the
	// resolution change and the overlay's own controls depend on, would have been dropped in
	// silence, and the "no state at all" branch of the service would have re-queued the work on
	// every present. So the banner is skipped and the common tail below is reached instead.
	if (nr_ok)
	{
		LOGI("==================================================================");
		LOGI("DLSS-NR ARMED. feature id %u, preset %u (the only network in this snippet build).",
		     ngx::kFeatureDLSSNR, ngx::kOnlyPreset);
		// The identification block reads the OVERLAY'S atomics, not g_cfg. shader_hash never passes
		// through the g_cfg snapshot at all (it is read at its own site, before begin_pass runs), and
		// the four register pins only reach g_cfg on the first accepted dispatch - which is after this
		// banner. Printing g_cfg here would report the ini's values even when the user had already
		// changed them and re-armed, which is the sort of small lie this log exists to not tell.
		{
			const overlay_ui::live_block &lb = overlay_ui::live();
			const unsigned long long hash = (unsigned long long)lb.shader_hash.load(std::memory_order_relaxed);
			LOGI("  target shader   0x%016llx%s", hash,
			     hash == 0 ? "  (0 = any shader passing all census gates - NOT recommended)" : "");
			LOGI("  registers       depth=t%u velocity=t%u colour=t%u output=u%u",
			     (unsigned)lb.srv_depth.load(std::memory_order_relaxed),
			     (unsigned)lb.srv_velocity.load(std::memory_order_relaxed),
			     (unsigned)lb.srv_colour.load(std::memory_order_relaxed),
			     (unsigned)lb.uav_output.load(std::memory_order_relaxed));
		}
		LOGI("  tuning          Intensity=%.3f LocalTone=%.3f LocalStructure=%.3f "
		     "SkinStructure=%.3f Style=%u UseAutoMask=%d",
		     g_cfg.intensity, g_cfg.local_tone_strength, g_cfg.local_structure_strength,
		     g_cfg.skin_structure_strength, g_cfg.style, (int)g_cfg.use_auto_mask);
		LOGI("  behaviour       copy_back=%d depth_inverted=%d (%s) restore_graphics_root=%d",
		     (int)g_cfg.copy_back, (int)nr_depth_inverted_value(*st),
		     st->depth_det.latched == depth_convert::verdict::undecided
		         ? (g_cfg.depth_detect ? "not measured yet - inferred from UE 4.27 reversed-Z"
		                               : "depth_detect=0, inferred from UE 4.27 reversed-Z")
		     : (g_cfg.depth_inverted_pinned || st->depth_det_stood_down)
		         ? "MEASURED, but overridden by the ini or the overlay"
		         : "MEASURED from the depth buffer",
		     (int)g_cfg.restore_graphics_root);
		LOGI("  depth convert   depth_convert=%d depth_detect=%d (%s)",
		     (int)(g_cfg.depth_convert && !st->depth_failed && !st->depth_eval_rejected),
		     (int)g_cfg.depth_detect,
		     st->depth_failed
		        ? "FAILED: the shader or its pipeline could not be built, so DLSSNR.Depth stays on "
		          "the game's typeless resource - README gap 3"
		        : (st->depth_eval_rejected
		             ? "REVERTED: NGX refused our r32_float depth, see the error above"
		             : (g_cfg.depth_convert
		                  ? "DLSSNR.Depth will be our own r32_float texture holding DeviceZ verbatim"
		                  : "OFF: the game's own r32_g8_typeless depth resource is bound, which is "
		                    "the binding README gap 3 is about")));
		LOGI("  hdr codec       hdr_codec=%d paper_white_scale=%.4f (UNCALIBRATED) "
		     "transfer_strength=%.3f color_strength=%.3f hdr_graft=%u (%s)",
		     (int)(overlay_ui::live_hdr_codec() && !st->codec_failed), g_cfg.paper_white_scale,
		     g_cfg.transfer_strength, g_cfg.color_strength,
		     (unsigned)overlay_ui::live_hdr_graft(),
		     !st->codec_graft_ok     ? "PINNED TO 0 - this decode was built without the reference graft"
		     : overlay_ui::live_hdr_graft() == 0u
		                             ? "additive residual - ours, bit-exact identity"
		                             : "renodx UpgradeToneMap - OkLab hue lock, AP1 clamp");
		LOGI("  mvec decode     mvec_decode=%d mvec_reconstruct=%d mvec_dilate=%d clip_row=%s "
		     "transpose=%d scale=%s/%s",
		     (int)(overlay_ui::live_mvec_decode() && !st->mvec_failed),
		     (int)overlay_ui::live_mvec_reconstruct(),
		     (int)g_cfg.mvec_dilate,
		     g_cfg.mvec_clip_row != 0 ? "PINNED in the ini" : "discovered + cross-checked",
		     (int)g_cfg.mvec_clip_transpose,
		     g_cfg.mvec_scale_x != 0.0f ? "OVERRIDDEN" : "auto",
		     g_cfg.mvec_scale_y != 0.0f ? "OVERRIDDEN" : "auto");
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
		// GATED ON THE CODEC ACTUALLY BEING ON, not merely on the absence of a build failure.
		//
		// This used to read `if (!st->codec_failed)`, which was correct only while hdr_codec=0 latched
		// codec_failed as a side effect. Making hdr_codec live deleted that assignment - it had to, it
		// was the whole reason the codec could not be turned on at runtime - and left this line
		// printing "GAP 1 ADDRESSED - the HDR codec is ON" for a user who ships hdr_codec=0 and never
		// opens the panel, contradicting the configuration line printed two entries above it and
		// inverting the meaning of this add-on's primary diagnostic for the darkening symptom.
		// live_hdr_codec(), not g_cfg.hdr_codec, for the same reason the identification block above
		// reads the overlay's atomics: begin_pass has not run yet on this first dispatch, so g_cfg
		// still holds what the ini said even when the user has already changed it and re-armed.
		if (overlay_ui::live_hdr_codec() && !st->codec_failed && st->codec.ok)
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
		// GAP 2 is now fixed when the decode pass is armed, and only then. The message says which.
		// This is an ARM-TIME statement: it can report that the pipeline exists, but not that the pass
		// actually ran - locating View.ClipToPrevClip needs a real dispatch. The per-dispatch line
		// (\"GAP 2 ADDRESSED\" / \"KNOWN GAP 2\") is the one that reports the outcome, and it is
		// printed once from nr_try_run.
		if (overlay_ui::live_mvec_decode() && !st->mvec_failed && st->mvec.ok)
			LOGI("DLSS-NR: GAP 2 ADDRESSED - the motion-vector decode is ARMED. DLSSNR.MVec will be "
			     "OUR r16g16_float texture on the colour grid, not the game's encoded velocity: a "
			     "compute pass applies UE 4.27's DecodeVelocityFromTexture (scale 4.00801611f AND the "
			     "bias 32767/65535, which MVecScaleX/Y alone can never remove) and, wherever the "
			     "velocity texel is the cleared sentinel - the static world, the sky, translucency and "
			     "every movable that did not move - reconstructs camera motion by reprojecting depth "
			     "through View.ClipToPrevClip. MVecScaleX/Y are FORCED to 1.0 so the grid correction "
			     "cannot double-apply. Confirmation that it actually ran, or the reason it did not, "
			     "follows on the first accepted dispatch.");
		else
			LOGW("DLSS-NR: KNOWN GAP - MOTION VECTOR ENCODING IS NOT CONVERTED (%s). The snippet "
			     "expects absolute pixels on the colour grid, y-down. UE4 writes screen-space velocity "
			     "packed into the texture with a scale AND A BIAS, so a unorm velocity buffer is not "
			     "in those units at all. DLSSNR.MVecScaleX/Y can rescale a grid but cannot remove a "
			     "bias, so it cannot fix this. Symptom: ghosting or smearing that does not track "
			     "camera motion. The fix is the decode compute pass this build ships - see README "
			     "\"Known gaps\", gap 2.",
			     !overlay_ui::live_mvec_decode() ? "mvec_decode=0, so it is disabled by configuration"
			                                     : "its shader or pipeline could not be built - see above");
		// GAP 3, on the same terms as gap 2: this is an ARM-TIME statement about the PIPELINE. The
		// per-dispatch "GAP 3 ADDRESSED" line in nr_try_run is the one that reports the outcome,
		// because only there is the game's depth SRV known to have been recovered.
		if (g_cfg.depth_convert && !st->depth_failed && st->depth_conv.ok)
			LOGI("DLSS-NR: GAP 3 ADDRESSED - the depth conversion is ARMED. DLSSNR.Depth will be OUR "
			     "r32_float texture and not the game's r32_g8_typeless depth-stencil. On D3D12 the "
			     "snippet is handed a bare ID3D12Resource* and reads the format straight off the "
			     "D3D12_RESOURCE_DESC - there is no view-format channel in the ABI - so what it sees "
			     "today is a TYPELESS PLANAR format rather than a depth value. The pass reads the "
			     "game's depth through the game's OWN typed r32_float_x8_uint SRV and writes DeviceZ "
			     "VERBATIM. Confirmation that it actually ran follows on the first accepted dispatch.");
		else
			LOGW("DLSS-NR: KNOWN GAP - THE DEPTH RESOURCE IS TYPELESS AND PLANAR (%s). NGX reads the "
			     "format off the D3D12_RESOURCE_DESC and cannot be told the view format on D3D12, so "
			     "DLSSNR.Depth is being handed something that is not a depth value to anything reading "
			     "it that way. The one known-working DLSS-NR deployment renders its own R32F target and "
			     "hard-rejects any other format. The fix is the conversion pass this build ships - see "
			     "README \"Known gaps\", gap 3.",
			     !g_cfg.depth_convert ? "depth_convert=0, so it is disabled by configuration"
			                          : "its shader or pipeline could not be built - see above");
		// GAP 4. Arm-time only says whether the MEASUREMENT will be attempted; the verdict needs
		// real frames and is printed from nr_try_run when it lands.
		if (g_cfg.depth_detect && !st->depth_failed && st->depth_conv.ok)
			LOGI("DLSS-NR: GAP 4 - depth_detect is ARMED. DLSSNR.DepthInverted currently sends %d, "
			     "which is the INFERENCE that UE 4.27 renders reversed-Z; the conversion pass is "
			     "accumulating a distribution statistic over the depth it already reads and will "
			     "replace that inference with a MEASUREMENT, or say why it declines to. %s",
			     (int)g_cfg.depth_inverted,
			     g_cfg.depth_inverted_pinned
			        ? "depth_inverted is PINNED in stray_dlssnr.ini, so the measurement will be "
			          "REPORTED and NOT applied - a wrong pin will be visible rather than obeyed."
			        : "It has NOT been confirmed against STRAY before this build.");
		else
			LOGW("DLSS-NR: KNOWN GAP - depth_inverted=%d IS INFERRED, NOT MEASURED (%s). UE 4.27 "
			     "renders reversed-Z, which is the OPPOSITE of the working Remix deployment's value, "
			     "and getting it wrong produces no diagnostic anywhere - the evaluate still succeeds "
			     "and the image is merely wrong. If the denoise ghosts or smears in exactly the wrong "
			     "direction, flip it first. See README \"Known gaps\", gap 4.",
			     (int)g_cfg.depth_inverted,
			     !g_cfg.depth_detect ? "depth_detect=0, so it is disabled by configuration"
			                         : "the depth conversion pass it measures through could not be "
			                           "built - see above");
		LOGI("==================================================================");
	}   // if (nr_ok)

	// NOTHING ARMED. Neither half has anything the service could act on, init_complete stays false
	// for the life of the process, and nr_try_run's one-shot has already been cleared - so this is
	// a state that must never be serviced, which is precisely what a false init_complete means.
	if (!nr_ok && !sr_ok)
		return false;

	// THE LAST STATEMENT, AND IT MUST STAY THE LAST STATEMENT. RELEASE, paired with the ACQUIRE in
	// nr_service_reconfigure: until this is stored, the present thread treats this nr_state as if
	// it did not exist at all. Every `return false` above leaves it false for the life of the
	// process, which is correct - a state that failed to initialise must never be serviced.
	//
	// "A FULLY SUCCESSFUL INIT" NOW MEANS "EVERY PART THAT WAS ASKED FOR". Reaching this line means
	// at least one feature armed and every step it needed completed: for DLSS-NR, Init_Ext plus the
	// parameter block; for DLSS-SR, Init_Ext through slot B plus its own parameter block, both of
	// which are behind sr_ok. A half of the pair that was never asked for (dlss_nr=0, dlss_sr=0)
	// leaves nothing to service, and a half that FAILED took the early exit above or recorded
	// itself in g_nr_init_failed. Nothing published here is under construction.
	st->init_complete.store(true, std::memory_order_release);
	return true;
}

static void nr_destroy_device(device *dev)
{
	auto *st = probe::pd_get<nr_state>(dev, kNrStateGuid);
	if (st == nullptr)
		return;

	g_nr_armed.store(false, std::memory_order_release);
	// Cleared BEFORE the feature is released, so a dispatch racing this teardown bails at the top
	// of sr_try_run instead of reaching a handle that is about to become invalid.
	const bool sr_was_armed = g_sr_armed.exchange(false, std::memory_order_acq_rel);

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

		// The SR block, same rule, same reason. nr_release_feature_and_output above has already
		// idled the queue and released the SR feature and its textures.
		if (st->sr_feat.params != nullptr)
		{
			delete st->sr_feat.params;
			st->sr_feat.params = nullptr;
		}

		// The root signatures and PSOs outlive the per-resolution textures, so they are released
		// here rather than in nr_release_feature_and_output. nr_release_feature_and_output has
		// already idled the queue.
		hdr_codec::destroy(dev, st->codec);
		// Same lifetime, same rule, same place. mvec_decode::destroy guards a null device and zero
		// handles and resets the struct, so it is correct to call unconditionally - including when
		// mvec_decode=0 or the build failed and both handles are already 0. Without it the
		// ID3D12RootSignature and ID3D12PipelineState created by mvec_decode::create were dropped
		// by pd_destroy<nr_state> below with no Release, and each of them holds a reference on the
		// ID3D12Device, so the device itself was never destroyed either.
		mvec_decode::destroy(dev, st->mvec);
		// Same lifetime, same rule, same place again. depth_convert::destroy guards a null device
		// and zero handles and resets the struct, so it is correct to call unconditionally -
		// including when depth_convert=0 and depth_detect=0 and every handle is already 0. It owns
		// a root signature, a PSO, TWO BUFFERS and a view, and every one of them holds a reference
		// on the ID3D12Device; dropping them here is what lets the device itself be destroyed.
		depth_convert::destroy(dev, st->depth_conv);
		// Same rule, same reason: the probe owns a root signature, a PSO and two buffers, and
		// each holds a reference on the device.
		nr_probe::destroy(dev, st->probe);

		if (g_snippet.shutdown1 != nullptr && st->d3d12 != nullptr)
		{
			const ngx::Result r = g_snippet.shutdown1(st->d3d12);
			if (ngx::failed(r))
				nr_log_ngx(reshade::log::level::warning, "NVSDK_NGX_D3D12_Shutdown1", r);
		}
		if (g_sr_snippet.shutdown1 != nullptr && st->d3d12 != nullptr && sr_was_armed)
		{
			const ngx::Result r = g_sr_snippet.shutdown1(st->d3d12);
			if (ngx::failed(r))
				LOGW("DLSS-SR: NVSDK_NGX_D3D12_Shutdown1 returned 0x%08x %s.",
				     (unsigned)r, ngx::result_to_string(r));
		}
		st->d3d12 = nullptr;
	}

	probe::pd_destroy<nr_state>(dev, kNrStateGuid);

	// Deliberately NOT FreeLibrary'ing the snippet here. It is process-wide, the process is on
	// its way out, and unloading a 166 MB module that may still hold worker threads during device
	// teardown buys nothing.
}

// =============================================================================================
// THE UNIFIED DEFERRED RECONFIGURE. R3, R4 and R5 of the ladder in src/overlay_ui.hpp.
//
// Serviced from on_present, on the MAIN thread, where idling the queue and destroying a resource
// are safe. This is the generalisation of nr_service_pending_teardown, not a parallel mechanism:
// the seam is the same one the resolution change has always used - a bit raised on a recording
// thread, consumed here, handed to nr_release_feature_and_output.
//
// LOCK ORDER, and it is the one thing here that could deadlock the game. nr_try_run takes
// st->mutex (:2744) and then g.mutex (:2771). on_present takes g.mutex further down, AFTER this
// function has returned. So this function takes st->mutex ONLY and must never take g.mutex - the
// same AB/BA argument the atomics on nr_state exist to satisfy. It is called from exactly the
// same statement region nr_service_pending_teardown was.
//
// IT RUNS EVEN WHEN THE PASS DOES NOT. That is deliberate and it is why the overlay's epochs are
// read here rather than only in begin_pass: with enabled=0, with the master bypass on, or with
// the feature wedged, begin_pass is never called - and those are precisely the situations in
// which a user reaches for a reconfigure.
//
// FAILURE DEGRADES. Every step either completes or leaves the previous working state alone, says
// why in the log once, and publishes the reason to the status block, which draws it in red above
// everything else. There is no path here that leaves a half-applied configuration.
// =============================================================================================
static void nr_service_reconfigure(device *dev)
{
	if (dev == nullptr)
		return;
	// Everything below either loads a D3D12 snippet or touches D3D12 resources, and nr_init_device
	// makes the same check before it does any of it. Without this, a present on a D3D11 swapchain
	// would LoadLibraryW the snippet for a device that can never use it.
	//
	// [ASSUMED] one D3D12 device. take_reconfigure DRAINS the overlay's action word, so with two
	// D3D12 devices the first one to present would consume a request the second never sees. That
	// is the same single-device assumption the whole identification path already makes (and which
	// moving the seen-epochs into nr_state removed for the EPOCH half of this); it is recorded
	// here rather than glossed over. STRAY is single-device.
	if (dev->get_api() != device_api::d3d12)
		return;

	// ACQUIRE ON init_complete, AND A HALF-BUILT STATE IS TREATED AS NO STATE AT ALL.
	//
	// pd_get can hand back an nr_state that nr_lazy_ngx_init published on its first statement and
	// is still filling in, on a recording thread, for hundreds of milliseconds - which is exactly
	// the window a user who just ticked "Load the snippet and arm NGX" is sitting in the overlay
	// for. Acting on it there would run nr_release_feature_and_output over fields another thread
	// is writing and reach the pipeline builders concurrently with the same call. See
	// nr_state::init_complete. The ACQUIRE pairs with the RELEASE on the last line of
	// nr_lazy_ngx_init, so everything that function wrote is visible once this reads true.
	nr_state *const st_raw = probe::pd_get<nr_state>(dev, kNrStateGuid);
	nr_state *const st = (st_raw != nullptr && st_raw->init_complete.load(std::memory_order_acquire))
		? st_raw : nullptr;

	// Before nr_lazy_ngx_init has ever run there is no nr_state to keep the seen-epochs in - that
	// is exactly the enabled=0 case, where the whole point is to be able to arm from the overlay.
	// A process-wide static is correct HERE and only here: the thing it is tracking, "has the
	// snippet been asked for yet", is itself process-wide.
	static overlay_ui::seen_epochs s_bootstrap_seen;
	overlay_ui::seen_epochs &seen = (st != nullptr) ? st->seen_service : s_bootstrap_seen;

	const overlay_ui::reconfig_request req = overlay_ui::take_reconfigure(seen);

	uint32_t work = req.bits;
	if (st != nullptr)
		work |= st->pending_work.fetch_and(0u, std::memory_order_acquire);

	if (work == 0u && !req.ident_changed)
	{
		overlay_ui::publish_reconfig_pending(false);
		return;
	}

	// Which key asked. A teardown with no overlay request behind it is nr_ensure_output's own
	// resolution change, which is not a user reconfigure and must not be reported as one.
	const bool from_overlay = (req.bits != 0u) || req.ident_changed;
	const char *const why = from_overlay
		? (req.why != nullptr ? req.why : "an overlay setting")
		: "the TAA output resolution or format changed";

	overlay_ui::publish_reconfig_pending(true);

	// Hoisted ABOVE the early returns rather than declared next to the work they describe: two of
	// those returns can now carry a real failure - a snippet that will not load, and an NGX
	// initialisation that already failed and cannot be retried - and a failure reported as
	// "APPLIED" is the exact defect this ladder exists to remove.
	bool ok = true;
	const char *fail_reason = nullptr;

	// ---- the DXR census: FIRST, because it is the one action that needs NOTHING ----------------
	//
	// Two relaxed stores into rt_census's own atomics. NOT rt_census::arm(), which prints a
	// start-up banner describing the whole gate - correct once at init, wrong every time a
	// checkbox moves.
	//
	// IT IS FIRST BECAUSE IT USED TO BE LAST, AND THAT MADE IT A DEAD CONTROL. take_reconfigure
	// DRAINS the overlay's action word with fetch_and(0), and both early returns below used to
	// discard the drained bits and then publish the reconfigure as a SUCCESS. rt_census::set_live
	// is the only route by which the two census keys ever reach the census, and the census
	// checkboxes are deliberately drawn OUTSIDE the panel's BeginDisabled(!usable) block on the
	// stated grounds that "the case where NGX did NOT arm is exactly when a user wants the shader
	// census turned on" - which is precisely the state where nr_state does not exist. So with
	// enabled=0 in the ini the box ticked, Save lit up, the status block said the reconfigure was
	// fine, and the census never turned on for the rest of the session. A control that lies with a
	// green status beside it.
	//
	// The other bits are NOT re-queued on those paths, and that is a decision rather than an
	// oversight: a_teardown, a_clear_failed, a_clear_clip, a_reconcile and a_apply_populate all
	// describe work on an nr_state, and a fresh nr_state has nothing to tear down, no latched
	// failure, clean clip latches, and is built by nr_lazy_ngx_init directly from the live values
	// (live_hdr_codec / live_mvec_decode / live_populate_parameters, all read there). They are
	// satisfied by construction. Re-queueing them instead would leave reconfig_pending true for
	// ever in a session that never arms - REBUILDING on screen, permanently, for work that has
	// nothing to do.
	if ((work & overlay_ui::a_apply_census) != 0u)
	{
		const bool     on   = overlay_ui::live_rt_census();
		const uint32_t every = overlay_ui::live_rt_census_frames();
		rt_census::set_live(on, every);
		LOGI("DLSS-NR reconfigure: RT census %s (summary every %u presents). Its counters are "
		     "CUMULATIVE from the first time it was armed and are not reset here, so read the "
		     "deltas between summaries rather than the totals.",
		     on ? "ON" : "OFF", (unsigned)(every != 0 ? every : 600));
	}

	// ---- the snippet arm, which is the one action that does not need nr_state -----------------
	// Done BEFORE the lock, because it LoadLibraryW's a 166 MB module and there is no reason to
	// hold st->mutex across that. Nothing it touches is under that mutex.
	// live_dlss_nr() is in the predicate for a reason that is easy to miss: with dlss_nr=0 the
	// DLSS-NR snippet was deliberately never loaded, so g_snippet.available is false and WITHOUT
	// this test any control that raises a_reconcile - ticking HDR codec, say - would LoadLibraryW
	// 166 MB of denoiser mid-session for a feature the user has turned off. dlss_nr is launch-time
	// (see live_block::dlss_nr), so reading it here is reading a value that cannot have changed.
	if ((work & overlay_ui::a_reconcile) != 0u &&
	    overlay_ui::live_enabled() && overlay_ui::live_dlss_nr() && !g_snippet.available)
	{
		if (!nr_arm_snippet(nr_addon_dir(), why))
		{
			// The gate is opened even though this failed, and deliberately. Nothing was released
			// and nothing needs rebuilding - the snippet simply is not there - so leaving it shut
			// would make begin_pass skip every pass for the rest of the session on top of a failure
			// that has already stopped the pass by other means. A shut gate must only ever mean
			// "a release is still owed", which is the one case the refusal below leaves it shut for.
			overlay_ui::publish_serviced_rebuild(req.rebuild_epoch);
			overlay_ui::publish_reconfigure(false,
				"the snippet could not be loaded - see ReShade.log for the exact reason");
			overlay_ui::publish_reconfig_pending(false);
			return;
		}
	}
	// ---- THE ARM THAT CANNOT WORK, SAID OUT LOUD -----------------------------------------------
	// The branch above only re-arms when the snippet is not loaded. The other way to be un-armed
	// is that the snippet loaded fine and Init_Ext FAILED - and that cannot be retried: nr_try_run's
	// deferred initialiser is a one-shot that is set before the attempt and never cleared, so
	// nothing here can make it run again. Without this the service logged "reconfigure APPLIED:
	// \"enabled\" -> tier 1" for a request that reached nothing at all, under a status block still
	// promising STANDBY would clear itself. See g_nr_init_failed for why the one-shot stays.
	else if ((work & overlay_ui::a_reconcile) != 0u &&
	         overlay_ui::live_enabled() && g_snippet.available &&
	         !g_nr_armed.load(std::memory_order_acquire) &&
	         g_nr_init_failed.load(std::memory_order_relaxed))
	{
		ok = false;
		fail_reason = "NGX initialisation already failed in this session and cannot be retried "
		              "in-process - a relaunch is required; see the status block for the result code";
		static bool s_said_init_dead = false;
		if (!s_said_init_dead)
		{
			s_said_init_dead = true;
			LOGW("DLSS-NR reconfigure: \"%s\" cannot arm NGX. Init_Ext already ran and failed "
			     "(0x%08X) and the deferred initialiser is a one-shot that is not cleared on "
			     "failure - deliberately, because the one measured fact about its fragility is "
			     "that it can HANG. Fix the cause and relaunch. This message is printed once.",
			     why, (unsigned)g_nr_init_result.load(std::memory_order_relaxed));
		}
	}

	// ---- INITIALISATION IS IN FLIGHT: KEEP THE WORK, DO NOT DISCARD IT -------------------------
	//
	// st_raw exists but has not published init_complete, so nr_lazy_ngx_init is running RIGHT NOW
	// on a recording thread. That is not the same situation as "no state at all", and the
	// difference is a lost setting: lazy init reads the live values ONCE, near its start, so a
	// change the user makes during its several hundred milliseconds has ALREADY been missed by it.
	// Dropping the bits here - which is what treating this as the no-state case would do, since
	// take_reconfigure has already drained them - would leave the feature rebuilt (the rebuild
	// epoch is re-detected afterwards, because adopt_epochs seeded seen_service at the START of
	// init) but the codec or mvec pipelines never built. Tick HDR codec while the arm is running
	// and it would silently do nothing.
	//
	// pending_work is safe to touch here even though nothing else about st_raw is: it is a
	// std::atomic<uint32_t> that pd_create value-initialised BEFORE it published the pointer, and
	// nr_lazy_ngx_init never writes it.
	//
	// The rebuild gate is NOT opened and reconfig_pending is NOT cleared, both deliberately: the
	// work really is still owed, the pass must stay skipped until it is done, and REBUILDING is
	// exactly what is happening.
	//
	// THE ESCAPE HATCH IS g_nr_init_settled, NOT g_nr_init_failed. The question this branch asks
	// is "is nr_lazy_ngx_init still running", and g_nr_init_failed stopped answering it the moment
	// the DLSS-NR half learned to record a failure and CARRY ON so the SR half could still arm.
	// Asking the failure flag was wrong in both directions - it skipped this branch during the
	// whole SR tail of a run whose NR Init_Ext had failed (dropping the drained bits and opening
	// the gate early), and it matched for ever on a pure-SR run whose SR init failed without
	// recording anything (holding the panel at REBUILDING and queueing every later request into a
	// pending_work that only a true init_complete can drain). g_nr_init_settled is set from a
	// scope guard on every exit of nr_lazy_ngx_init, so it means exactly what is being asked.
	if (st == nullptr && st_raw != nullptr &&
	    !g_nr_init_settled.load(std::memory_order_acquire))
	{
		// The census is already applied, above; re-queuing it would just re-log it.
		st_raw->pending_work.fetch_or(work & ~overlay_ui::a_apply_census,
		                              std::memory_order_relaxed);
		return;
	}

	// ---- THE OTHER ARM THAT CANNOT WORK, ALSO SAID OUT LOUD -------------------------------------
	//
	// dlss_sr 0 -> 1 with nvngx_dlss.dll never loaded. The BRANCH is live and has already taken
	// effect - the snapshot carries dlss_sr, so the next accepted dispatch really will go to
	// sr_try_run - but sr_try_run will bail on its own second line with "not armed", because
	// nr_arm_sr_snippet only ever runs at launch and its header says why: honouring this here would
	// mean a SECOND NVSDK_NGX_D3D12_Init_Ext in the session, and the one measured fact about
	// Init_Ext's fragility is that it can HANG.
	//
	// IT SITS ABOVE THE `st == nullptr` RETURN, and that placement is load-bearing: on a pure-SR
	// run (dlss_nr=0) whose SR init failed there IS no serviceable nr_state, so the return below
	// is the one this request takes - and without the diagnostic in front of it that return would
	// publish the reconfigure as APPLIED for a change that reaches a bail. It is deliberately
	// BELOW the in-flight branch above, because g_sr_armed is stored near the END of
	// nr_lazy_ngx_init: firing this while init is still in flight would burn the one-shot on a
	// feature that is about to arm perfectly well.
	//
	// It is reported rather than silently half-done. Without this the banner would say
	// "reconfigure APPLIED: \"dlss_sr\" -> tier 1" for a change that reaches a bail, which is the
	// exact shape of control this ladder exists to remove. `ok` is set false, so the report block
	// at the end draws the red banner and the log line names the reason; the teardown and the
	// reconcile below still run, because releasing DLSS-NR's feature is real work that the branch
	// change genuinely needs doing.
	if (overlay_ui::live_dlss_sr() && !g_sr_armed.load(std::memory_order_acquire))
	{
		// THE BANNER CLAIMS AUTHORSHIP ONLY WHEN dlss_sr IS THE KEY THAT ASKED. The action bits are
		// OR-merged into one word, so the service structurally cannot tell which control raised
		// a_reconcile - and marking an unrelated change (ticking HDR codec, say) as FAILED with an
		// SR reason would credit the wrong key, which this file already refuses to do at
		// overlay_ui::request. `why` is the string literal the control passed, so comparing it is
		// the one thing here that IS specific to the key. The one-shot log line below is NOT gated
		// on it: the state is worth stating once however it was reached, and the panel's own SR
		// status line is the permanent, unmissable version of it.
		if (from_overlay && why != nullptr && std::strcmp(why, "dlss_sr") == 0)
		{
			ok = false;
			fail_reason = g_sr_snippet.available
				? "DLSS-SR's nvngx_dlss.dll loaded but its Init_Ext failed earlier in this session, so "
				  "the SR pass cannot run - see ReShade.log for the result code; a relaunch is required"
				: "RELAUNCH REQUIRED for dlss_sr=1: nvngx_dlss.dll is not loaded, because dlss_sr was 0 "
				  "in stray_dlssnr.ini at launch. Press Save and relaunch. Turning it back OFF is live";
		}
		static bool s_said_sr_relaunch = false;
		if (!s_said_sr_relaunch)
		{
			s_said_sr_relaunch = true;
			LOGW("DLSS-SR: dlss_sr=1 is selected but the feature is NOT ARMED (%s). The accepted "
			     "dispatch is being sent to sr_try_run, which bails immediately and leaves ReShade to "
			     "issue the game's own TAA - a correct frame, and a strict no-op. The ON direction "
			     "needs a relaunch; the OFF direction is live. This message is printed once.",
			     g_sr_snippet.available ? "nvngx_dlss.dll loaded, Init_Ext through slot B failed"
			                            : "nvngx_dlss.dll was never loaded - dlss_sr=0 in the ini at launch");
		}
	}

	if (st == nullptr)
	{
		// Nothing else can be done on this device yet: nr_lazy_ngx_init creates nr_state, and it
		// runs on the next dispatch now that the arm above has set g_nr_pending_init. The gate is
		// opened here too: with no serviceable nr_state there is nothing to release, so the
		// rebuild IS done, and leaving it shut would wedge the pass before it had ever run.
		overlay_ui::publish_serviced_rebuild(req.rebuild_epoch);
		overlay_ui::publish_reconfigure(ok, ok ? why : fail_reason);
		overlay_ui::publish_reconfig_pending(false);
		return;
	}

	// ---- REFUSE RATHER THAN RUN A RELEASE WITH NO IDLE -----------------------------------------
	// nr_release_feature_and_output idles the GPU through g_queue, and g_queue is cleared at
	// on_destroy_command_queue. With it null the release would destroy resources the GPU may
	// still be reading, with no diagnostic. Its only callers used to be present, resize and
	// destroy, where that is unlikely; a user-triggered reconfigure widens the window
	// arbitrarily, so it is checked rather than assumed. The work is put BACK, not dropped, so
	// the reconfigure lands as soon as a queue exists again.
	const bool want_teardown = (work & overlay_ui::a_teardown) != 0u;
	if (want_teardown && g_queue.load(std::memory_order_relaxed) == nullptr)
	{
		// a_apply_census is MASKED OUT, exactly as the in-flight branch above masks it and for the
		// same reason: the census was applied at the top of this function, above every early
		// return, and re-queuing the bit only makes the next present re-run rt_census::set_live and
		// re-print its LOGI. Revert raises a_teardown and a_apply_census in one word, so a Revert
		// pressed in the window between on_destroy_command_queue and on_init_command_queue is a
		// reachable way to get the census line logged twice for one click.
		st->pending_work.fetch_or(work & ~overlay_ui::a_apply_census, std::memory_order_relaxed);
		overlay_ui::publish_reconfigure(false,
			"there is no graphics queue to idle the GPU with, so nothing was released - the "
			"reconfigure is still queued and will be applied as soon as one exists");
		static bool s_said_no_queue = false;
		if (!s_said_no_queue)
		{
			s_said_no_queue = true;
			LOGW("DLSS-NR reconfigure: DEFERRED (\"%s\") - g_queue is null, so the feature and "
			     "textures cannot be released without leaving in-flight GPU work referencing "
			     "them. The request is kept and retried on every present. This message is "
			     "printed once.", why);
		}
		return;
	}

	std::lock_guard<std::mutex> lock(st->mutex);

	// ---- R4: the teardown, through the EXISTING seam -------------------------------------------
	// nr_release_feature_and_output is unchanged. It idles the queue, releases the NGX feature,
	// destroys every view and every texture, drops st->pending_res, and clears every
	// per-resolution latch. Crucially for hdr_codec it zeroes out_w/out_h and out_tex, so the
	// next accepted dispatch re-enters nr_ensure_output on the CREATE branch and re-decides the
	// neural format against the new value - which is the whole of the on-to-off fix, with no new
	// mechanism at all.
	if (want_teardown)
	{
		nr_release_feature_and_output(dev, *st, why);
		// Keeps the status block's REBUILDING rung on screen for the three seconds its own comment
		// describes. publish_evaluate clears the stamp on the first successful evaluate, so it can
		// never become the stale positive that a latch would be.
		overlay_ui::publish_teardown();
	}

	if ((work & overlay_ui::a_clear_failed) != 0u)
		st->feature_failed = false;

	// ---- R5: make what EXISTS match what is WANTED ---------------------------------------------
	// The overlay cannot make these decisions - whether the codec needs building depends on
	// st->codec.ok and st->codec_failed, which live behind this mutex - so it asks for a
	// reconcile and the answer is worked out here, once, with the state to work it out from.
	if ((work & overlay_ui::a_reconcile) != 0u)
	{
		const std::wstring dir = nr_addon_dir();

		// The DLSS-SR ladder's REFUSAL latch, re-armed here for exactly the reason a_clear_clip
		// exists. sr_try_run prints a one-shot ERROR when sr_suppress_taa=1 while sr_direct_output
		// and sr_copy_back are both 0 - a combination it refuses rather than obeys, because it
		// would leave the frame holding whatever was last in u0. sr_suppress_taa is a live control
		// now, so that combination can be entered, read about, left and entered again within one
		// session; without this the second refusal would be SILENT while the checkbox sat there
		// ticked and doing nothing. Re-arming it on any reconcile can at worst print the line
		// twice, which is the right side of that trade.
		st->logged_sr_suppress = false;
		// AND THE MOTION-GUIDE LATCH, for the identical reason and as of the same pass.
		// sr_mvec_decode is a live control raising a_reconcile now, so the guide can fall back to
		// the game's raw encoded velocity, be read about, be fixed and fall back again inside one
		// session - and this one-shot would make every fallback after the first SILENT while the
		// checkbox sat there ticked. It stays in the teardown arm too: it is also a statement
		// about resources a release destroys. Worst case it prints twice.
		st->logged_sr_mvec_off = false;

		if (overlay_ui::live_hdr_codec() && !st->codec.ok && !st->codec_failed)
		{
			if (!nr_build_codec_pipelines(dev, *st, dir))
			{
				ok = false;
				fail_reason = "the HDR codec's shaders or pipelines could not be built; the "
				              "denoise still runs, undecoded";
			}
		}

		// The SAME shared-pipeline predicate nr_lazy_ngx_init uses - and it is now actually the
		// same. One root signature, one PSO, one DXBC, two targets, so ticking sr_mvec_decode with
		// mvec_decode off has to build it here too, or the SR guide would silently fall back to the
		// game's raw encoded velocity with no diagnostic that names the reason.
		//
		// dlss_chain IS PART OF THE PREDICATE, exactly as it is in nr_lazy_ngx_init's copy, and it
		// was missing here while the comment above already claimed the two were identical. The
		// configuration it stranded is the one the chain is meant to be tested on: dlss_chain=1,
		// dlss_sr=0, both decode keys off at launch, so the pipeline is never built - then the
		// player ticks sr_mvec_decode, sr_mvec_decode's a_reconcile reaches this arm, live_dlss_sr()
		// is 0, the predicate is false, the pipeline is never built, mvec_wanted stays false for the
		// rest of the run, and the banner reports the reconfigure APPLIED over a chained frame still
		// being guided by the game's raw encoded velocity.
		if ((overlay_ui::live_mvec_decode() ||
		     ((overlay_ui::live_dlss_sr() || overlay_ui::live_dlss_chain()) &&
		      overlay_ui::live_sr_mvec_decode())) &&
		    !st->mvec.ok && !st->mvec_failed)
		{
			if (!nr_build_mvec_pipeline(dev, *st, dir))
			{
				ok = false;
				fail_reason = "the motion-vector decode's shader could not be built; the guide "
				              "falls back to the game's raw encoded velocity";
			}
		}
	}

	// ---- populate_parameters: ITS OWN BIT, AND ONLY ITS OWN BIT --------------------------------
	//
	// This block used to live inside a_reconcile above and to key purely on
	// live_populate_parameters() vs st->serviced_populate_parameters. That meant EVERY control
	// that raised a_reconcile - enabled, hdr_codec, mvec_decode, require_trampoline, Reset,
	// Revert - applied whatever the PopulateParameters checkbox happened to say, including a value
	// the user had ticked and deliberately NOT Applied. The checkbox's tooltip promises the exact
	// opposite ("only when you press Apply beside it - deliberately not on the click"), and the
	// reason it does is that PopulateParameters_Impl is a gated export whose signature is
	// unverified against this snippet build. Worse, require_trampoline raises a_reconcile at
	// k_plain - no teardown - so it reached the ON->OFF branch with st->feature still alive and
	// deleted the parameter block CreateFeature had been handed.
	//
	// a_apply_populate is raised by the Apply button and by Revert, both with a_teardown, and by
	// nothing else. The bits are OR-merged into one word, so the service structurally cannot tell
	// which key asked - which is exactly why the work must have its own bit rather than be derived
	// from a value.
	if ((work & overlay_ui::a_apply_populate) != 0u)
	{
		// OFF->ON is one call on the block we already have. ON->OFF cannot be un-called, so it
		// needs a FRESH block - which needs the feature released FIRST, because CreateFeature was
		// handed the old pointer.
		const bool want_populate = overlay_ui::live_populate_parameters();
		if (want_populate && !st->serviced_populate_parameters && st->params != nullptr)
		{
			if (g_snippet.populate_params != nullptr)
			{
				const ngx::Result pr = g_snippet.populate_params(st->params);
				nr_log_ngx(ngx::failed(pr) ? reshade::log::level::warning : reshade::log::level::info,
				           "PopulateParameters_Impl (live: populate_parameters 0 -> 1)", pr);
				st->serviced_populate_parameters = true;
			}
			else
			{
				ok = false;
				fail_reason = "this snippet build exports no PopulateParameters_Impl, so there is "
				              "nothing to call";
			}
		}
		else if (!want_populate && st->serviced_populate_parameters)
		{
			// CHECKED, NOT ASSUMED. The precondition is "the feature that was created against this
			// block is gone", and the honest way to know that is to look at st->feature rather
			// than to trust that the caller raised the right rung. want_teardown alone is not
			// enough either: the teardown may have been refused earlier for want of a queue, and
			// take_reconfigure ADOPTS the rebuild epoch on its first call for a given seen-state,
			// so a request that relied on the epoch to imply the release can arrive without one.
			// Refusing leaves the previous working state exactly as it was, which is the rule.
			if (st->feature != nullptr)
			{
				ok = false;
				fail_reason = "the NGX feature is still live, and the parameter block it was "
				              "created against cannot be replaced underneath it - press \"Reset NR "
				              "feature\" and then Apply again";
			}
			else
			{
				// ALLOCATE FIRST, THEN DELETE. If the allocation fails we keep the block we have
				// and report it: a null st->params is a bail on every subsequent dispatch, which
				// would turn a settings change into "the add-on stopped working".
				ngx::parameter_block *fresh = new (std::nothrow) ngx::parameter_block();
				if (fresh == nullptr)
				{
					ok = false;
					fail_reason = "out of memory allocating a fresh NGX parameter block; the "
					              "previous one is still in use and populate_parameters is unchanged";
				}
				else
				{
					delete st->params;
					st->params = fresh;
					st->serviced_populate_parameters = false;
					LOGI("DLSS-NR reconfigure: allocated a fresh NGX parameter block "
					     "(populate_parameters 1 -> 0). PopulateParameters_Impl cannot be un-called "
					     "on the block it was made against, so the block is replaced instead.");
				}
			}
		}
		// Mirrored to the panel either way, including on the refusal: the checkbox then keeps
		// showing "checked but NOT APPLIED" beside a red RECONFIGURE FAILED banner, which together
		// say precisely what happened.
		overlay_ui::publish_populate(st->serviced_populate_parameters);
	}

	// ---- R3: the one-shot log latches --------------------------------------------------------
	// Without this a WRONG new pin would be rejected IN SILENCE - the one-shot log lines have
	// already fired for the run - and the status block would still say WAITING FOR GAME DLSS with
	// nothing in the log to say why. It is the same re-arm the resolution change has always done
	// (this function absorbed nr_service_pending_teardown, and its list is preserved verbatim),
	// now reached by a user reconfigure as well.
	if (want_teardown)
	{
		// Let every one-shot diagnostic speak again for the new configuration.
		st->logged_taa_found      = false;
		st->logged_srv_reject     = false;
		st->logged_uav_reject     = false;
		st->logged_uav_ambiguous  = false;
		st->logged_restore_reject = false;
		st->logged_create_fail    = false;
		st->logged_eval_fail      = false;
		st->logged_codec_off      = false;
		st->logged_codec_tex_fail = false;
		st->logged_copy_fmt       = false;
		// The mvec pass's per-resolution state is cleared in nr_release_feature_and_output, so its
		// one-shot diagnostics must be re-armed here for exactly the same reason the codec's are: a
		// target that allocates at one extent and not at the next, or a View-CB discovery that
		// validates at one render size and not at the next, would otherwise re-latch in SILENCE and
		// leave the log's last word on the subject a stale "GAP 2 ADDRESSED" from the old
		// configuration while the census printed mode=RAW with no reason anywhere.
		st->logged_mvec_format      = false;
		st->logged_mvec_active      = false;
		st->logged_mvec_off         = false;
		st->logged_mvec_tex_fail    = false;
		st->logged_mvec_no_viewcb   = false;
		st->logged_mvec_clip_bad    = false;
		st->logged_mvec_clip_row    = false;
		st->logged_mvec_decode_only = false;
		// The depth pass's per-resolution one-shots, re-armed on exactly the same argument as the
		// mvec ones above: an r32_float target that allocates at one extent and not at the next
		// would otherwise re-latch in SILENCE, leaving the log's last word on the subject a stale
		// "GAP 3 ADDRESSED" from the old configuration.
		//
		// logged_depth_det_result and logged_depth_det_stand are deliberately NOT in this list.
		// They are statements about a MEASUREMENT that survives a resolution change - see the
		// note in nr_release_feature_and_output on why the verdict is not re-taken - and re-arming
		// them would promise a second announcement that can never come.
		st->logged_depth_tex_fail = false;
		st->logged_depth_active   = false;
		st->logged_depth_off      = false;
		// logged_mvec_pinned_row is deliberately NOT reset by a teardown: it is a statement about
		// the ini file, not about the resolution. It IS reset by a_clear_clip below, because there
		// the ini value is precisely what changed.
		st->logged_hist_active   = false;
		st->logged_hist_dropped  = false;
		st->logged_hist_odd_reg  = false;
		st->logged_hist_tex_fail = false;
		st->logged_hist_double_arm = false;
		// The DLSS-SR one-shots, re-armed for exactly the same reason the mvec ones are: a resource
		// that allocated at one extent and not at the next, or a View-CB discovery that validated at
		// one render size and not at the next, would otherwise re-latch in SILENCE. They belong in
		// the TEARDOWN arm and not in the ident-only arm below, because every one of them is a
		// statement about resources nr_release_feature_and_output has just destroyed.
		st->logged_sr_banner      = false;
		st->logged_sr_no_jitter   = false;
		st->logged_sr_out_extent  = false;
		st->logged_sr_mvec_off    = false;
		st->logged_sr_copy_fmt    = false;
		st->sr_feat.logged_create_fail = false;
		st->sr_feat.logged_eval_fail   = false;
		st->sr_feat.logged_preset      = false;
		// logged_sr_direct is a statement about the CONFIGURATION, not the resolution, and has
		// already been said once - the same rule as logged_mvec_pinned_row. logged_sr_suppress is
		// NOT in that group any more: sr_suppress_taa became a live control in this pass, so its
		// latch is re-armed by the a_reconcile block above, on every reconfigure rather than only
		// on a teardown. See the comment there.
		// The identity statement is deliberately NOT reset: it is a property of the codec, not of
		// the resolution, and it has already been said once.
	}
	else if (req.ident_changed)
	{
		// An identification change with no teardown behind it. Only the lines that speak about
		// identification are re-armed; the resource-allocation ones have nothing new to say.
		st->logged_taa_found       = false;
		st->logged_srv_reject      = false;
		st->logged_uav_reject      = false;
		st->logged_uav_ambiguous   = false;
		st->logged_hist_odd_reg    = false;
		st->logged_hist_dropped    = false;
		st->logged_hist_double_arm = false;
	}

	if (req.ident_changed || want_teardown)
	{
		// The armed pristine copy names a resource chosen under the OLD identification. begin_pass
		// drops it too, on the render thread, but only if the pass is actually being reached - and
		// the whole point of servicing here is that it may not be. (nr_release_feature_and_output
		// already did this on the teardown path; doing it twice is free and the alternative is a
		// branch that has to stay in step with that function.)
		st->pending_res = 0;
		st->pending_w = st->pending_h = 0;
		st->pending_fmt = format::unknown;
		// The ring the copy-back's one-shot feedback warning reads holds handles from the OLD
		// configuration, whose resources may be on their way out; a recycled address could
		// otherwise false-match. Clear it with the latch it feeds.
		for (uint64_t &h : st->copied_into) h = 0;
		st->copied_into_next = 0;
		st->logged_feedback_loop = false;
	}

	// ---- R1's latch clear: mvec_clip_row / mvec_clip_transpose ---------------------------------
	// These two can set view_layout_failed and clip_ok=false PERMANENTLY for the resolution after
	// thirty consecutive failures, and until this ladder existed only a full feature release
	// cleared them. Without this the knob is dead after the first bad value and every later change
	// does nothing: a control that lies, which is the one outcome the overlay exists to prevent.
	if ((work & overlay_ui::a_clear_clip) != 0u)
	{
		st->view_layout_failed = false;
		st->view_discover_tries = 0;
		st->clip_fail_streak = 0;
		st->clip_ok = false;            // re-derive it rather than trust a matrix from the old row
		st->logged_mvec_clip_bad  = false;
		st->logged_mvec_clip_row  = false;
		st->logged_mvec_pinned_row = false;
		st->logged_mvec_no_viewcb = false;
		st->logged_mvec_off       = false;
		st->logged_mvec_decode_only = false;
	}

	// (The DXR census is applied at the TOP of this function, above every early return - it needs
	// neither nr_state nor a lock, and it is the control a user reaches for precisely when NGX
	// never armed. See the comment there.)

	// ---- report ---------------------------------------------------------------------------------
	if (ok)
	{
		overlay_ui::publish_reconfigure(true, why);
		// THE LINE THAT FIRES ONLY ON A REAL RECONFIGURE. Not once-per-run, not per frame: the
		// service returns early when there is no work, so reaching here means a rung was actually
		// climbed. It names the key and the tier, which is what makes "did my change land?"
		// answerable from the log alone.
		LOGI("DLSS-NR reconfigure APPLIED: \"%s\" -> tier %s%s%s%s%s%s. "
		     "codec=%s mvec=%s feature=%s",
		     why,
		     want_teardown ? "1 (feature recreate)" : (req.ident_changed ? "2 (re-identify)" : "0/1 (live)"),
		     req.ident_changed        ? " +ident-epoch" : "",
		     (work & overlay_ui::a_reconcile)    ? " +reconcile" : "",
		     (work & overlay_ui::a_clear_clip)   ? " +clip-latches" : "",
		     (work & overlay_ui::a_apply_census) ? " +census" : "",
		     (work & overlay_ui::a_apply_populate) ? " +populate" : "",
		     st->codec_failed ? "FAILED" : (st->codec.ok ? "built" : "not built"),
		     st->mvec_failed  ? "FAILED" : (st->mvec.ok  ? "built" : "not built"),
		     st->feature != nullptr ? "kept" : "released");
	}
	else
	{
		overlay_ui::publish_reconfigure(false,
			fail_reason != nullptr ? fail_reason : "see ReShade.log");
		LOGE("DLSS-NR reconfigure FAILED: \"%s\" - %s. The PREVIOUS working state is still "
		     "running and nothing is half-applied.", why,
		     fail_reason != nullptr ? fail_reason : "see above");
	}

	// LAST, and only on a path that actually completed. This is the render thread's rebuild gate:
	// until it moves, begin_pass skips the pass entirely, so no frame ever runs with the new
	// settings against the old textures. The refusal path above returns WITHOUT publishing, which
	// is correct - the pass must stay skipped while the release is still owed.
	//
	// Published even when `ok` is false: a reconfigure that failed has still finished, the previous
	// working state is what is running, and leaving the gate shut would turn one failed pipeline
	// build into "the add-on stopped rendering". The red banner is how the user learns about it,
	// not a black screen.
	overlay_ui::publish_serviced_rebuild(req.rebuild_epoch);
	overlay_ui::publish_reconfig_pending(false);
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

// ---- BEGIN overlay_ui hook ----
// live_diagnostics(), NOT g_cfg.diagnostics, at all three of the sites below.
//
// This is the one setting that could never have been made live through the per-pass g_cfg
// snapshot, and the reason is the shape of these three functions rather than anything about the
// value: they run on EVERY draw and EVERY dispatch in the process, on arbitrary recording
// threads, with no lock, and entirely outside the accepted-TAA-pass window that begin_pass
// covers. Writing it into g_cfg from the snapshot and reading it here would be a plain data race
// on every draw call in the game.
//
// One relaxed atomic load at each site makes it live at zero risk. The old ledger's reason for
// greying the control out - "read from threads this overlay must not race with, for the sake of a
// log knob" - was an argument for using an atomic, not for having no control.
static bool on_draw(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t)
{
	PROBE_GUARD_FALSE({ if (overlay_ui::live_diagnostics()) dump_bindings(cmd, false); })
}

static bool on_draw_indexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
{
	PROBE_GUARD_FALSE({ if (overlay_ui::live_diagnostics()) dump_bindings(cmd, false); })
}
// ---- END overlay_ui hook ----

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
		// ---- BEGIN overlay_ui hook ---- (see on_draw above for why this is not g_cfg)
		if (overlay_ui::live_diagnostics())
			dump_bindings(cmd, true);
		// ---- END overlay_ui hook ----
		nr_try_run(cmd, group_count_x, group_count_y, group_count_z, handled);
	})
}

// =============================================================================================
// DXR. Read-only, and the ONLY handler in this file that can never do anything but observe.
// =============================================================================================
//
// MUST RETURN FALSE, ALWAYS. Returning true SUPPRESSES the game's DispatchRays - ReShade's hook
// reads `if (invoke_addon_event<dispatch_rays>(...)) return;` (d3d12_command_list.cpp:1147) and
// then never calls the original. A census that deleted the frame's ray tracing would be a
// spectacular way to answer "does ray tracing run".
//
// On D3D12 all four resource handles arrive ZERO and the shader binding tables are identified
// only by their GPU virtual addresses in the *_offset arguments. rt_census keys its buckets on
// (raygen_offset - miss_offset), which is address-independent; see rt_census.hpp.
//
// The bound state object comes from the command-list shadow: UE emits SetPipelineState1 and
// DispatchRays back to back (D3D12RayTracing.cpp:3936-3937), so cs->state_object is this
// dispatch's RTPSO.
static bool on_dispatch_rays(command_list *cmd,
                             resource /*raygen*/, uint64_t raygen_offset, uint64_t raygen_size,
                             resource /*miss*/, uint64_t miss_offset, uint64_t miss_size, uint64_t miss_stride,
                             resource /*hit_group*/, uint64_t hit_offset, uint64_t hit_size, uint64_t hit_stride,
                             resource /*callable*/, uint64_t callable_offset, uint64_t callable_size, uint64_t callable_stride,
                             uint32_t width, uint32_t height, uint32_t depth)
{
	PROBE_GUARD_FALSE({
		// Strict no-op when rt_census=0: note_dispatch's first statement is a relaxed atomic load
		// followed by a return, and this function then returns false, so ReShade issues the
		// game's DispatchRays exactly as it would with no add-on present.
		uint64_t state_object = 0;
		if (rt_census::on())
		{
			auto *cs = probe::pd_get<probe::cmd_shadow>(cmd, probe::kCmdShadowGuid);
			if (cs != nullptr)
				state_object = cs->state_object.handle;
		}

		rt_census::note_dispatch(state_object,
			raygen_offset, raygen_size,
			miss_offset, miss_size, miss_stride,
			hit_offset, hit_size, hit_stride,
			callable_offset, callable_size, callable_stride,
			width, height, depth, &rt_census_log);
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
			nr_service_reconfigure(sc->get_device());

		// Read the DLSS-NR counters BEFORE g.mutex is taken and WITHOUT st->mutex. nr_try_run
		// holds st->mutex and then takes g.mutex; acquiring them in the other order here would
		// deadlock. They are atomics for exactly this reason.
		nr_state *nst = (sc != nullptr) ? probe::pd_get<nr_state>(sc->get_device(), kNrStateGuid) : nullptr;
		const uint64_t nr_hist_applied = (nst != nullptr) ? nst->hist_restored.load(std::memory_order_relaxed) : 0;
		const uint64_t nr_hist_dropped = (nst != nullptr) ? nst->hist_dropped.load(std::memory_order_relaxed) : 0;
		const bool     nr_codec_on     = (nst != nullptr) && nst->census_codec_on.load(std::memory_order_relaxed);
		const bool     nr_orig_on      = (nst != nullptr) && nst->census_orig_on.load(std::memory_order_relaxed);
		const uint64_t nr_mvec_frames  = (nst != nullptr) ? nst->mvec_frames.load(std::memory_order_relaxed) : 0;
		const uint64_t nr_mvec_reuse   = (nst != nullptr) ? nst->mvec_cb_reuse.load(std::memory_order_relaxed) : 0;
		const uint32_t nr_mvec_mode    = (nst != nullptr) ? nst->census_mvec_mode.load(std::memory_order_relaxed) : 0;
		const uint64_t nr_depth_frames = (nst != nullptr) ? nst->depth_frames.load(std::memory_order_relaxed) : 0;
		const bool     nr_depth_bound  = (nst != nullptr) && nst->census_depth_bound.load(std::memory_order_relaxed);
		const uint32_t nr_depth_verdict= (nst != nullptr) ? nst->census_depth_verdict.load(std::memory_order_relaxed) : 0;
		const bool     nr_depth_inv    = (nst != nullptr) ? nst->census_depth_inverted.load(std::memory_order_relaxed)
		                                                  : g_cfg.depth_inverted;
		const uint64_t sr_evals        = (nst != nullptr) ? nst->sr_evaluates.load(std::memory_order_relaxed) : 0;
		const uint64_t chain_evals     = (nst != nullptr) ? nst->chain_evaluates.load(std::memory_order_relaxed) : 0;
		const uint64_t sr_suppressed   = (nst != nullptr) ? nst->sr_suppressed.load(std::memory_order_relaxed) : 0;
		const uint64_t sr_mvec         = (nst != nullptr) ? nst->sr_mvec_frames.load(std::memory_order_relaxed) : 0;
		const uint32_t sr_rw           = (nst != nullptr) ? nst->sr_census_render_w.load(std::memory_order_relaxed) : 0;
		const uint32_t sr_rh           = (nst != nullptr) ? nst->sr_census_render_h.load(std::memory_order_relaxed) : 0;
		const uint32_t sr_ow           = (nst != nullptr) ? nst->sr_census_out_w.load(std::memory_order_relaxed) : 0;
		const uint32_t sr_oh           = (nst != nullptr) ? nst->sr_census_out_h.load(std::memory_order_relaxed) : 0;
		const uint64_t nr_camera_cuts  = (nst != nullptr) ? nst->camera_cuts.load(std::memory_order_relaxed) : 0;
		const bool     nr_jitter_cut_ok= (nst != nullptr) ? nst->jitter_cut_ok.load(std::memory_order_relaxed) : false;

		// The RT census keeps its own frame counter and its own reporting interval, and takes its
		// own two mutexes - never g.mutex - so it cannot deadlock against nr_try_run. Strict
		// no-op when rt_census=0: one relaxed atomic load.
		rt_census::on_frame(&rt_census_log);

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
		// ---- BEGIN overlay_ui hook ----
		// history_restore and copy_back are LIVE now, and the value in g_cfg is written by the
		// overlay's snapshot on a RECORDING thread; on_present runs on the main thread. So the
		// GATE and the values it prints both read the overlay's atomics - the gate included,
		// because a racy gate is the same data race as a racy argument AND can additionally admit
		// a line that then reports the opposite state. This is the only place outside nr_try_run
		// that would otherwise read a snapshot-written field.
		//
		// Three tokens differ from the original statement, which read:
		//     ... || g_cfg.history_restore))            in the condition
		//     (int)g_cfg.history_restore, (int)g_cfg.copy_back,   in the argument list
		if (nst != nullptr && (nr_hist_applied != 0 || nr_hist_dropped != 0 || overlay_ui::live_history_restore()))
			LOGI("--- DLSS-NR history restore @ frame %llu: applied=%llu dropped=%llu "
			     "(history_restore=%d copy_back=%d hdr_codec_running=%d pristine=%s)",
			     (unsigned long long)g.frame,
			     (unsigned long long)nr_hist_applied, (unsigned long long)nr_hist_dropped,
			     (int)overlay_ui::live_history_restore(), (int)overlay_ui::live_copy_back(),
			     (int)nr_codec_on, nr_orig_on ? "allocated" : "MISSING");
		// ---- END overlay_ui hook ----

		// The motion-vector decode's own accounting. 'decoded' climbing at one per accepted
		// dispatch with mode=full is the success signature. 'cb_reuse' climbing means the
		// per-frame 64-byte read of View.ClipToPrevClip out of UE's upload pool is failing or
		// returning implausible rows and the last good matrix is standing in - a one-frame-stale
		// reprojection each time, and at 30 consecutive the reconstruction latches off.
		// ---- BEGIN overlay_ui hook ----
		// mvec_decode and mvec_reconstruct are LIVE now, and g_cfg carries them only because
		// begin_pass wrote them there on a RECORDING thread; on_present runs on the main thread. So
		// the gate and the two values it prints all read the overlay's atomics - the GATE included,
		// because a racy gate is the same data race as a racy argument AND can additionally admit a
		// line that then reports the opposite state. Exactly the fix the history_restore/copy_back
		// line above already carries, for exactly the same reason.
		if (nst != nullptr && (nr_mvec_frames != 0 || nr_mvec_reuse != 0 || overlay_ui::live_mvec_decode()))
			LOGI("--- DLSS-NR mvec @ frame %llu: decoded=%llu cb_reuse=%llu mode=%s "
			     "(mvec_decode=%d mvec_reconstruct=%d)",
			     (unsigned long long)g.frame,
			     (unsigned long long)nr_mvec_frames, (unsigned long long)nr_mvec_reuse,
			     nr_mvec_mode == 2 ? "FULL (decode + camera reconstruction)"
			   : nr_mvec_mode == 1 ? "DECODE ONLY (invalid texels are zero - bring-up A/B)"
			                       : "RAW (the game's encoded velocity - pre-decode behaviour)",
			     (int)overlay_ui::live_mvec_decode(), (int)overlay_ui::live_mvec_reconstruct());
		// ---- END overlay_ui hook ----

		// The depth conversion's own accounting, and the depth-convention verdict beside it.
		// 'converted' climbing at one per accepted dispatch with bound=yes is the gap-3 success
		// signature; converted climbing with bound=no is the measurement-only configuration
		// (depth_convert=0, depth_detect=1) and is not a fault. A flat counter with
		// depth_convert=1 means the pass is not running and a one-shot line above says which rung
		// it fell to.
		//
		// NEITHER KEY IS LIVE - neither has an overlay control and neither is in
		// OVERLAY_OWNED_FIELDS - so g_cfg is read directly here. That is the same main-thread read
		// of a main-thread-written value the mvec line above USED to be, and it is only correct
		// because these two keys are ini-only. If either ever becomes a live control it joins the
		// overlay atomics IN THIS LINE, exactly as sr_direct_output eventually had to.
		if (nst != nullptr && (nr_depth_frames != 0 || g_cfg.depth_convert || g_cfg.depth_detect))
			LOGI("--- DLSS-NR depth @ frame %llu: converted=%llu bound=%s DepthInverted=%d (%s) "
			     "(depth_convert=%d depth_detect=%d)",
			     (unsigned long long)g.frame, (unsigned long long)nr_depth_frames,
			     nr_depth_bound ? "yes, our r32_float" : "no, the game's typeless resource",
			     (int)nr_depth_inv,
			     nr_depth_verdict == 0 ? "not measured"
			   : nr_depth_verdict == 1 ? "MEASURED reversed-Z"
			                           : "MEASURED standard-Z",
			     (int)g_cfg.depth_convert, (int)g_cfg.depth_detect);

		// THE DLSS-SR PROOF-OF-LIFE LINE. evaluates= is incremented on the branch immediately
		// after EvaluateFeature returned Success and nowhere else, so a non-zero value here is
		// evidence the feature RAN. A zero with dlss_sr=1 means the pass never reached the
		// evaluate - and the one-shot "DLSS-SR: pass did not run - <reason>" line above names the
		// stage that refused.
		// ---- BEGIN overlay_ui hook ----
		// dlss_sr AND sr_suppress_taa ARE LIVE NOW, so the value of each in g_cfg is written by
		// the overlay's snapshot on a RECORDING thread while on_present runs on the MAIN thread.
		// That is the identical data race this hook removed for history_restore and copy_back a
		// few lines above, and it applies to the GATE as much as to the printed values: a racy
		// gate is the same race AND can additionally admit a line that then reports the opposite
		// state. Both read the overlay's atomics instead, which are authoritative anyway - they
		// are what g_cfg is written FROM.
		//
		// sr_direct_output and sr_copy_back ARE LIVE TOO, and this line used to carry a comment
		// saying the opposite - that neither was in OVERLAY_OWNED_FIELDS or the snapshot, so
		// reading them from g_cfg here was a main-thread read of a main-thread-written value, and
		// that if either were made live it would join the others IN THIS LINE. Both were made live
		// in the same pass that wrote it (OVERLAY_OWNED_FIELDS and the tier-0 snapshot block both
		// carry them) and this line was not updated with them, leaving a C++ data race on two
		// non-atomic bools that could report the pre-edit state for the rest of the run. They read
		// the atomics now, like every other value on this line.
		//
		// dlss_chain joins the gate for the same reason it joins every other one: with dlss_sr=0
		// and dlss_chain=1 the SR feature is armed and evaluating, and a census that stayed silent
		// about it would report nothing at all for the configuration under test.
		if (overlay_ui::live_dlss_sr() || overlay_ui::live_dlss_chain())
			LOGI("--- DLSS-SR @ frame %llu: evaluates=%llu suppressed_dispatches=%llu "
			     "mvec_decodes=%llu geometry=%ux%u -> %ux%u (armed=%d suppress=%d direct=%d "
			     "copy_back=%d)",
			     (unsigned long long)g.frame, (unsigned long long)sr_evals,
			     (unsigned long long)sr_suppressed, (unsigned long long)sr_mvec,
			     sr_rw, sr_rh, sr_ow, sr_oh,
			     (int)g_sr_armed.load(std::memory_order_relaxed),
			     (int)overlay_ui::live_sr_suppress_taa(),
			     (int)overlay_ui::live_sr_direct_output(),
			     (int)overlay_ui::live_sr_copy_back());
		// THE CHAIN'S PROOF-OF-LIFE LINE. chained= is incremented on the ONE branch where BOTH
		// EvaluateFeature calls returned Success on the same dispatch, so a non-zero value is
		// evidence the chain RAN. Zero with dlss_chain=1 means it did not, and one of the
		// one-shot "DLSS-CHAIN: ..." lines above names the stage that refused.
		//
		// live_dlss_chain(), not g_cfg.dlss_chain, for the reason the two lines above it read the
		// overlay's atomics: dlss_chain is LIVE now, so g_cfg's copy is written by begin_pass on a
		// RECORDING thread while on_present runs on the MAIN thread. That is the identical data
		// race this hook exists to remove, and it applies to a gate as much as to a printed value.
		if (overlay_ui::live_dlss_chain())
			LOGI("--- DLSS-CHAIN @ frame %llu: chained=%llu (of %llu DLSS-SR evaluates) sr_armed=%d",
			     (unsigned long long)g.frame, (unsigned long long)chain_evals,
			     (unsigned long long)sr_evals,
			     (int)g_sr_armed.load(std::memory_order_relaxed));
		// ---- END overlay_ui hook ----
		// `dxil=` COUNTS PIXEL AND COMPUTE SHADERS ONLY. on_init_pipeline's loop skips every
		// sub-object that is not a PS or a CS, so a DXIL ray tracing library can never reach it
		// and dxil=0 has never meant "no ray tracing". The rt= field says which it is, so the
		// number can no longer be misread.
		// Camera cuts: `cuts` climbing at scene transitions is the success signature. `ok=0` means
		// the jitter row never validated, so cut detection is simply unavailable this run and the
		// behaviour is exactly what it was before it existed.
		if (nst != nullptr)
			LOGI("--- DLSS-NR camera cuts @ frame %llu: cuts=%llu detector=%s",
			     (unsigned long long)g.frame, (unsigned long long)nr_camera_cuts,
			     nr_jitter_cut_ok ? "LIVE (View.TemporalAAJitter.zw==.xy)"
			                      : "UNAVAILABLE (jitter row did not validate)");

		LOGI("--- probe census @ frame %llu: shaders=%llu (not_dxbc=%llu dxil=%llu, PS/CS ONLY - "
		     "says NOTHING about DXR) | census fail=%llu pass=%llu | vel_const fail=%llu pass=%llu | "
		     "loops_rej=%llu conf_rej=%llu | PASSED_ALL=%llu | srv_dumps=%u/%u | rt=%s",
			(unsigned long long)g.frame,
			(unsigned long long)g.n_shaders_seen, (unsigned long long)g.n_not_dxbc, (unsigned long long)g.n_dxil,
			(unsigned long long)g.n_fail_census, (unsigned long long)g.n_pass_census,
			(unsigned long long)g.n_fail_velocity_const, (unsigned long long)g.n_pass_velocity_const,
			(unsigned long long)g.n_fail_loops, (unsigned long long)g.n_fail_confidence,
			(unsigned long long)g.n_pass_all, g.srv_dumps, kMaxSrvDumps,
			rt_census::on() ? "MEASURED - see the RT CENSUS block" : "NOT MEASURED (set rt_census=1)");

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
	// DXR. Registered UNCONDITIONALLY even though rt_census defaults to 0, because the config is
	// not read until the first init_device and ReShade's DispatchRays hook calls
	// invoke_addon_event<dispatch_rays> with no has_addon_event() guard - a late
	// ReShadeRegisterEvent would push_back into addon_event_list[90] while a recording thread is
	// iterating it. The handler's cost with the census off is one relaxed atomic load and
	// `return false`. See the gate section at the top of src/rt_census.hpp.
	//
	// build_acceleration_structure is deliberately NOT registered: ReShade allocates a
	// std::vector and converts every geometry desc on EVERY AS build as soon as that event has
	// any listener at all (d3d12_command_list.cpp:1053-1077), empty handler or not.
	register_event<addon_event::dispatch_rays>(&on_dispatch_rays);

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
		//
		// ELEVEN g_cfg READS WERE DELETED FROM THIS LAMBDA BY THE RECONFIGURE LADDER, AND THAT IS
		// PART OF THE LADDER RATHER THAN TIDYING. enabled, diagnostics, hdr_codec, shader_hash,
		// srv_depth/velocity/colour, uav_output, populate_parameters and require_trampoline are all
		// LIVE now, which means overlay_ui::begin_pass writes them into g_cfg on a RECORDING thread
		// - while this lambda runs on the PRESENT thread, from overlay_ui::read_facts(). Leaving
		// them here would have reintroduced, on eleven fields at once, the exact race the on_present
		// hook below was written to remove for history_restore and copy_back.
		//
		// The overlay reads its own atomics for all eleven instead. Those are authoritative anyway:
		// they are what g_cfg is written FROM. What is left below is the load-only remainder, and
		// every one of them is written once, on the main thread, before any dispatch: app_id has no
		// live control at all (see draw_load_only), ini_found is a statement about a file read once,
		// and the rest are facts about the snippet rather than settings.
		overlay_ui::facts_hook() = [](overlay_ui::host_facts &f) {
			f.addon_name          = NAME;
			f.app_id              = g_cfg.app_id;
			f.ini_found           = g_cfg.ini_found;
			f.snippet_loaded      = g_snippet.available;
			f.trampoline          = g_snippet.trampoline_module != nullptr;
			f.armed               = g_nr_armed.load(std::memory_order_relaxed);
			// DLSS-SR's half of the same two facts. The panel's SR section uses BOTH to say which of
			// the two "not armed" cases applies - the snippet was never asked for (dlss_sr=0 in the
			// ini at launch) or it loaded and Init_Ext through slot B failed - because those two
			// have different fixes and only one of them is "press Save and relaunch".
			f.sr_snippet_loaded   = g_sr_snippet.available;
			f.sr_armed            = g_sr_armed.load(std::memory_order_relaxed);
			f.abi_thunks_active   = probe::msvc_abi_thunks_active();
			// The one state the panel cannot fix, reported so it can say so. `armed` false has two
			// causes and only one of them clears itself; see host_facts::ngx_init_failed.
			f.ngx_init_failed     = g_nr_init_failed.load(std::memory_order_relaxed);
			f.ngx_init_result     = g_nr_init_result.load(std::memory_order_relaxed);
			f.ngx_init_result_name = f.ngx_init_result != 0
				? ngx::result_to_string(f.ngx_init_result) : "";
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
