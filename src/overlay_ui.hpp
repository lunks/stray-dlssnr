// overlay_ui.hpp - the ReShade overlay for the STRAY DLSS-NR add-on.
//
// SELF-CONTAINED BY DESIGN. src/stray_dlssnr.cpp is being edited concurrently, so everything that
// can live here does. What is left there is six small hooks, each wrapped in
// `// ---- BEGIN overlay_ui hook ----` / `// ---- END overlay_ui hook ----`.
//
// =============================================================================================
// WHAT THIS FILE IS FOR, IN ONE PARAGRAPH
//
//   A user unticked this add-on in ReShade's overlay, ReShade wrote
//   `DisabledAddons=STRAY DLSS-NR@stray_dlssnr.addon64` into ReShade.ini, and the add-on then
//   silently never loaded again. A whole play session ran with nothing running and nobody knew.
//   So the FIRST thing this overlay draws, above every control, is a status line that answers
//   "is DLSS-NR evaluating RIGHT NOW", with an AGE - not a cumulative counter, which cannot tell
//   "14203 evaluates, still climbing" from "14203 evaluates, stopped four minutes ago". The
//   renodx add-on this UI mimics has no such line; that is the one place we deliberately do more
//   than it does.
//
// =============================================================================================
// THE THREE RULES THIS FILE OBEYS
//
// 1. THE OVERLAY TAKES NO MUTEX. Not nr_state::mutex, not the probe's g.mutex.
//    stray_dlssnr.cpp:2193 takes st->mutex and then g.mutex at :2198, and on_present reads the
//    DLSS-NR counters as ATOMICS specifically to avoid inverting that order (the comment at
//    :1505-1508 spells out the AB/BA deadlock). The overlay callback runs on the present thread.
//    A lock here would join that ordering graph and the whole argument would have to be
//    re-derived. Worse, nr_try_run holds st->mutex across CreateFeature (which uploads the
//    network weights) and EvaluateFeature, so blocking Present behind it is a visible hitch.
//    Every value crossing the boundary is therefore a lock-free atomic, checked below.
//
// 2. THE OVERLAY NEVER TOUCHES nr_state, AND NEVER CALLS ngx::set_*.
//    ngx::store mutates a std::unordered_map with a mutex the snippet also calls into
//    (ngx_interop.hpp:235,:258); pushing a slider value straight at NGX from the present thread
//    is heap corruption. All parameter writes stay where they are, on the render thread.
//    Instead the overlay writes ATOMICS, and ONE hook - begin_pass() - copies them into g_cfg
//    once per pass, on the render thread, under the lock that pass already holds.
//
// 3. ONE SNAPSHOT PER PASS, WHICH IS A CORRECTNESS REQUIREMENT AND NOT TIDINESS.
//    Several settings are read MORE THAN ONCE inside a single pass, and the two reads must agree:
//
//      restore_graphics_root   :2306 (capture_state) and :2980 (restore_state). A true->false
//                              tear between them is CORRUPTING: d3d12_state.hpp:429-435 absorbs
//                              the graphics heap only if(restore_graphics), :569-576 re-binds the
//                              heaps UNCONDITIONALLY, replay_pipe_compute issues
//                              SetComputeRootSignature (:507) which per that file's note at :243
//                              invalidates ALL root arguments including graphics, and :583-584
//                              then skips the graphics replay. That is precisely the corruption
//                              this knob's default-ON exists to prevent. (false->true is benign:
//                              plan.gfx.root_signature is null and :534-535 early-returns.)
//      paper_white_scale       read TWICE inside one expression at :2619, feeding ea.proxy_scale
//                              (:2720) AND da.proxy_scale (:2914). :2611-2613 calls a mismatch
//                              between them "a correctness failure, not a tuning difference".
//      transfer/color_strength read up to 3x each in their clamp expressions, :2620-2623.
//      mvec_scale_x/y          read twice each, :2778-2781 (a !=0.0f test, then the value).
//      copy_back               SIX sites in one pass: :1655 :2472 :2609 :2997 :3016 :3074.
//      history_restore         :1655 :2472 :2609 :3040 :3074.
//
//    Be precise about WHY rather than overclaiming: on x86-64 an aligned 4-byte load is atomic in
//    hardware, so a physically torn float is not reachable. The two real problems are (i) a
//    non-atomic concurrent read/write is a data race, and the compiler is then free to reload,
//    hoist or duplicate the read - which is exactly what makes the multi-read sites above bugs -
//    and (ii) INCOHERENCE between two reads of the same variable, which is compiler-legal and is
//    the restore_graphics_root corruption. Snapshotting once into g_cfg removes both, and costs
//    one relaxed load per field per frame.
//
// =============================================================================================
// WHICH SETTINGS ARE LIVE, AND WHICH CANNOT BE - EVERY ONE OF THESE IS SAID IN THE UI TOO
//
//   LIVE, FREE            intensity, local_tone_strength, local_structure_strength,
//                         skin_structure_strength, style, use_auto_mask, ui_correction.
//                         nr_try_run writes every NGX tuning parameter from g_cfg on EVERY
//                         accepted dispatch (:2783-2797), and nr_ensure_feature - the
//                         CreateFeature site - writes only Width/Height/InputWidth/InputHeight/
//                         Enabled/RenderPreset/ScalingRatio/node-masks/FreeMemOnRelease
//                         (:1891-1910). Not one tuning knob is baked at create time, so these
//                         sliders are honest with no extra machinery.
//   LIVE + SNAPSHOT       paper_white_scale, transfer_strength, color_strength,
//                         restore_graphics_root (see rule 3).
//   LIVE + ONE RESET      depth_inverted, mvec_scale_x, mvec_scale_y. Changing the depth
//                         convention or the motion-vector grid invalidates the accumulated
//                         temporal history; :2767-2773 already forces a reset frame when the
//                         guide GRID moves, for the same reason.
//   LIVE + CLEAR pending_res
//                         copy_back, history_restore, and the overlay's own master bypass.
//                         st->pending_res is a RAW ID3D12Resource address held across a frame,
//                         and :2504-2507 warns that UE 4.27's render-target pool can free that
//                         element and hand the address back for a differently sized colour
//                         texture. A stale arm surviving a toggle could write a seconds-old frame
//                         over a recycled resource that passes the shape check.
//   LOAD-ONLY, SHOWN GREYED, WITH THE REASON
//                         enabled ...... read once at :3119; nothing on the render path reads it.
//                                        A checkbox bound to it would do nothing until relaunch,
//                                        so the overlay's master switch is a SEPARATE per-dispatch
//                                        bypass and the ini key is displayed read-only.
//                         hdr_codec .... cannot be flipped in place IN EITHER DIRECTION. OFF->ON
//                                        is impossible: hdr_codec=0 at load sets
//                                        st->codec_failed (:3252-3254) and
//                                        nr_release_feature_and_output deliberately never clears
//                                        it (:1617-1621). ON->OFF in place silently kills the
//                                        copy-back: out_tex was forced to r16g16b16a16_float for
//                                        its lifetime (:1836-1837), so src_fmt (:2995) would
//                                        mismatch an r11g11b10_float TAA output and :2997-3014
//                                        fires. Doing it properly needs a deferred teardown plus
//                                        a one-shot pipeline build, which is a change to
//                                        stray_dlssnr.cpp far larger than this overlay is allowed
//                                        to make.
//                         shader_hash .. MEMOIZED PER-PSO: the lookup is guarded by
//                                        `cs->nr_checked != cs->pso` (:2157) and cached in
//                                        cs->nr_is_target (:2174-2175). A live change would take
//                                        effect on some command lists and not others.
//                         srv_* / uav_output
//                                        identification pins, and srv_colour is ALSO the
//                                        history-restore refusal test (:2492, :2558). Changing it
//                                        between the arm (:3086) and the consume (:2472) can land
//                                        last frame's image on this frame's scene-colour input.
//                         diagnostics .. read on the draw/dispatch path (:3432 :3437 :3456) from
//                                        threads this overlay must not race with for the sake of
//                                        a log knob.
//                         populate_parameters / require_trampoline / app_id
//                                        consumed once each at :3260 / :3135 / :3188.
//
// =============================================================================================
// FOUR renodx CONTROLS ARE DELIBERATELY ABSENT. "Never add a control that does nothing."
//
//   Enable Upscaling    ABSENT. `DLSSNR.Upscaling` does not exist anywhere in the snippet's
//                       string table (measured: 0 exact matches in nvngx_dlssnr.dll, while
//                       DLSSNR.UICorrection / .Style / .Intensity / .ScalingRatio each return 1),
//                       and ScalingRatio is dead - ngx_interop.hpp:180-183 and
//                       stray_dlssnr.cpp:1902-1903 both record that three sites read it and then
//                       unconditionally store 1.0f over the result. This add-on does not upscale.
//   NR Preset           SHOWN, DISABLED. It maps to DLSSNR.Hint.Render.Preset, which is written
//                       at CreateFeature (:1901), and :1899-1900 records that only preset 1
//                       exists in this snippet build - anything else logs "preset %d is not
//                       available in this DLL build" and loads the same weights anyway. It is a
//                       DIFFERENT parameter from `style`, which IS live and real; renodx lists
//                       both and only the latter does anything here.
//   Use game NGX flag   ABSENT AS A CONTROL, and it never was one: in the reference binary it is
//                       item 0 of the three-item "Depth Convention" combo, and it means "take the
//                       DepthInverted bit from the flags the GAME passed to its own DLSS
//                       CreateFeature". This add-on never sees the game's NGX create flags - it
//                       hooks a compute dispatch, not NGX - so that item has nothing to read.
//                       Our combo therefore has the other two items and says so.
//   NGX core            ABSENT AS A CONTROL, and it never was one either: in the reference it is
//                       a backend NAME substituted into a status line ("NGX core" vs "signed
//                       runtime"). Our status block reports the equivalent facts directly.
//   NR UI Correction    PRESENT, because unlike the other three it is real: `DLSSNR.UICorrection`
//                       IS in this snippet's string table (measured), and it is read as
//                       Get(const char*, int*) with a proper 0xbad00000 guard and a fallback of
//                       0. Its visual effect on STRAY's content is UNVERIFIED, and the UI says so.
//
// =============================================================================================
// EVERY ImGui CALL BELOW IS ON THE CI-VERIFIED SAFE LIST
//
//   The table is raw function pointers (include/reshade_overlay.hpp) and fifteen entries return
//   ImVec2/ImVec4 BY VALUE, where MSVC and GCC disagree - measured as an ACCESS_VIOLATION from a
//   mingw caller. See src/overlay_imgui.hpp for the full account. This file calls NONE of the
//   fourteen ImVec2 ones; a gating CI grep over src/ keeps it that way, and abi/
//   overlay_calls_probe.cpp compiles and LINKS the whole permitted set under both toolchains.
//   Everything here takes scalars or const ImVec4& (a pointer, so convention-free).
//
//   That matters because the binary running on the user's machine today is a MINGW build. This UI
//   is drawable, unchanged, under both toolchains right now; nothing in it waits on the MSVC
//   switch.

#pragma once

// overlay_imgui.hpp MUST come first, and its own header comment explains why in full: include/
// reshade_overlay.hpp has NO include guard and its whole body is `#if defined(IMGUI_VERSION_NUM)`,
// so anything that drags in reshade.hpp before <imgui.h> deletes namespace ImGui from the
// translation unit permanently. There is a compile-time guard in there and a gating CI step that
// fails the build if the wrong order ever compiles.
#include "overlay_imgui.hpp"

#include "addon_config.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace overlay_ui {

// ---------------------------------------------------------------------------------------------
// Logging. Same shape as the rest of the tree: one-shot latches, and nothing that can be printed
// once a frame.
// ---------------------------------------------------------------------------------------------
// NOT reshade::log::message(). That inline resolves ReShadeLogMessage and then CALLS THE RESULT
// WITH NO NULL CHECK - and GetProcAddress(nullptr, ...) returns nullptr whenever
// get_reshade_module_handle() found no ReShade module in the process. So in any host that does
// not export it, the first log line here would be a call through a null pointer, on the present
// thread. Measured, not theorised: it crashed abi/ini_rewrite_test.cpp outright the first time
// that test ran in CI. Resolve it once, with a guard, and stay silent if it is absent.
inline void logf(reshade::log::level lvl, const char *fmt, ...)
{
	using log_fn = void (*)(void *, int, const char *);
	static const log_fn fn = []() -> log_fn {
		HMODULE m = reshade::internal::get_reshade_module_handle();
		return (m == nullptr) ? nullptr : reinterpret_cast<log_fn>(GetProcAddress(m, "ReShadeLogMessage"));
	}();
	if (fn == nullptr)
		return;

	char buf[1024];
	va_list args;
	va_start(args, fmt);
	const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (n < 0)
		return;
	buf[sizeof(buf) - 1] = '\0';
	fn(reshade::internal::get_current_module_handle(), static_cast<int>(lvl), buf);
}

#define OVERLAY_LOG_ONCE(lvl, ...) do { \
	static bool s_said_ = false; if (!s_said_) { s_said_ = true; overlay_ui::logf(lvl, __VA_ARGS__); } } while (0)

// ---------------------------------------------------------------------------------------------
// THE LIVE BLOCK. Overlay writes, render thread reads exactly once per pass.
//
// std::atomic<float> and std::atomic<bool> compile to a plain mov on both toolchains; the
// static_asserts below refuse to build if that ever stops being true, because a lock-BASED atomic
// would take a hidden mutex on the render thread inside the fenced window and quietly reintroduce
// rule 1's problem.
// ---------------------------------------------------------------------------------------------
struct live_block
{
	// The overlay's OWN master switch. Deliberately NOT g_cfg.enabled - see the header comment.
	// True means "bypass the pass this frame".
	std::atomic<bool>     bypass{ false };

	std::atomic<bool>     copy_back{ true };
	std::atomic<bool>     history_restore{ true };
	std::atomic<bool>     restore_graphics_root{ true };

	std::atomic<float>    paper_white_scale{ 1.0f };
	std::atomic<float>    transfer_strength{ 1.0f };
	std::atomic<float>    color_strength{ 1.0f };

	std::atomic<bool>     depth_inverted{ true };
	std::atomic<float>    mvec_scale_x{ 0.0f };
	std::atomic<float>    mvec_scale_y{ 0.0f };

	std::atomic<float>    intensity{ 1.0f };
	std::atomic<float>    local_tone_strength{ 1.0f };
	std::atomic<float>    local_structure_strength{ 1.0f };
	std::atomic<float>    skin_structure_strength{ -1.0f };
	std::atomic<uint32_t> style{ 0 };
	std::atomic<bool>     use_auto_mask{ true };
	std::atomic<uint32_t> ui_correction{ 0 };

	// MULTI-FIELD CHANGES GO THROUGH AN EPOCH, NOT THROUGH THE VALUES. The overlay stores the
	// value relaxed and then bumps an epoch with RELEASE; begin_pass loads the epochs with
	// ACQUIRE before reading any value. That pair is the only ordering anything depends on -
	// every field is a single scalar whose stale value is at most one frame old and, because of
	// the snapshot, always self-consistent within the pass.
	std::atomic<uint32_t> epoch{ 0 };           // any change at all
	std::atomic<uint32_t> reset_epoch{ 0 };     // needs one DLSSNR.Reset frame
	std::atomic<uint32_t> flush_epoch{ 0 };     // needs st->pending_res dropped as well
	std::atomic<uint32_t> teardown_epoch{ 0 };  // the "Reset NR feature" button
};

static_assert(std::atomic<bool>::is_always_lock_free,     "std::atomic<bool> must be lock-free: the render thread reads it inside the fenced NGX window.");
static_assert(std::atomic<float>::is_always_lock_free,    "std::atomic<float> must be lock-free.");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "std::atomic<uint32_t> must be lock-free.");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "std::atomic<uint64_t> must be lock-free.");

inline live_block &live()
{
	static live_block b;
	return b;
}

// ---------------------------------------------------------------------------------------------
// THE STATUS BLOCK. Flows the other way and ONLY the other way: written by the render thread,
// read by the overlay, exactly as st->hist_restored / census_codec_on already do (:1509-1514) for
// the reason given at :1505-1508. Relaxed everywhere; nothing here is used to order anything.
//
// Every pointer stored here is a STRING LITERAL from a switch (probe::format_name,
// ngx::result_to_string), so it outlives any read.
// ---------------------------------------------------------------------------------------------
struct status_block
{
	std::atomic<uint64_t>     pass_seen_ms{ 0 };   // GetTickCount64 at the last accepted TAA dispatch
	std::atomic<uint64_t>     eval_ms{ 0 };        // ... at the last SUCCESSFUL EvaluateFeature
	std::atomic<uint64_t>     evaluates{ 0 };
	std::atomic<uint64_t>     eval_failures{ 0 };
	std::atomic<uint32_t>     last_result{ 0 };
	std::atomic<bool>         have_result{ false };
	std::atomic<const char *> last_result_name{ nullptr };

	std::atomic<uint32_t>     out_w{ 0 }, out_h{ 0 };
	std::atomic<uint32_t>     guide_w{ 0 }, guide_h{ 0 };
	std::atomic<const char *> out_fmt{ nullptr };
	std::atomic<const char *> neural_fmt{ nullptr };

	std::atomic<bool>         codec_running{ false };
	std::atomic<bool>         codec_failed{ false };
	std::atomic<bool>         codec_pipelines_ok{ false };
	std::atomic<bool>         orig_ok{ false };

	std::atomic<float>        auto_scale_x{ 0.0f }, auto_scale_y{ 0.0f };
	std::atomic<uint64_t>     hist_applied{ 0 }, hist_dropped{ 0 };

	// A TIMESTAMP, not a latch. As a latch, a feature that never came back would leave the status
	// reading "REBUILDING" indefinitely - which is exactly the kind of stale-positive this panel
	// exists to avoid, and the same defect the reference add-on's "ACTIVE" has.
	std::atomic<uint64_t>     teardown_ms{ 0 };
};

inline status_block &status()
{
	static status_block s;
	return s;
}

// ---------------------------------------------------------------------------------------------
// HOST FACTS. The load-only half of the picture: globals that live in stray_dlssnr.cpp and are
// written once, on the main thread, before anything here can run.
//
// Filled through a hook installed from DllMain, which is the one place in that file that sits
// BELOW every global it needs. Deliberately BY VALUE rather than by pointer: g_cfg's LIVE fields
// are written by begin_pass on the render thread, and copying only the load-only fields makes it
// structurally impossible for the overlay to read one of them by accident.
// ---------------------------------------------------------------------------------------------
struct host_facts
{
	bool     valid = false;

	const char *addon_name = "";        // the NAME export, for the DisabledAddons check
	bool     enabled_at_load = true;    // cfg.enabled
	bool     diagnostics = true;
	bool     hdr_codec_at_load = true;
	uint64_t shader_hash = 0;
	uint32_t srv_depth = 0, srv_velocity = 0, srv_colour = 0, uav_output = 0;
	bool     populate_parameters = false;
	bool     require_trampoline = true;
	uint64_t app_id = 0;
	bool     ini_found = false;

	bool     snippet_loaded = false;
	bool     trampoline = false;
	bool     armed = false;
	bool     abi_thunks_active = false;
	char     snippet_reason[256] = {};
};

using facts_fn = void (*)(host_facts &);

inline facts_fn &facts_hook()
{
	static facts_fn fn = nullptr;
	return fn;
}

inline host_facts read_facts()
{
	host_facts f;
	const facts_fn fn = facts_hook();
	if (fn != nullptr)
	{
		try { fn(f); f.valid = true; } catch (...) { f.valid = false; }
	}
	return f;
}

// ---------------------------------------------------------------------------------------------
// Where stray_dlssnr.ini lives, and what it said at load. Both written once, from
// seed_from_config, on the main thread inside nr_init_device.
// ---------------------------------------------------------------------------------------------
inline std::wstring &ini_dir()
{
	static std::wstring d;
	return d;
}

// The values as they came off disk. Two jobs: the "Revert to stray_dlssnr.ini" button, and the
// dirty test that gates the Save button.
inline cfg::config &baseline()
{
	static cfg::config c;
	return c;
}

inline std::atomic<bool> &seeded()
{
	static std::atomic<bool> s{ false };
	return s;
}

inline void bump(uint32_t kind);   // fwd

/// Copy the live half of a freshly loaded config into the atomics. Called ONCE, on the main
/// thread, from nr_init_device immediately after cfg::load - before any dispatch and before any
/// overlay draw, so no reader can observe the half-seeded state.
inline void seed_from_config(const cfg::config &c, const std::wstring &directory)
{
	live_block &l = live();
	l.copy_back.store(c.copy_back, std::memory_order_relaxed);
	l.history_restore.store(c.history_restore, std::memory_order_relaxed);
	l.restore_graphics_root.store(c.restore_graphics_root, std::memory_order_relaxed);
	l.paper_white_scale.store(c.paper_white_scale, std::memory_order_relaxed);
	l.transfer_strength.store(c.transfer_strength, std::memory_order_relaxed);
	l.color_strength.store(c.color_strength, std::memory_order_relaxed);
	l.depth_inverted.store(c.depth_inverted, std::memory_order_relaxed);
	l.mvec_scale_x.store(c.mvec_scale_x, std::memory_order_relaxed);
	l.mvec_scale_y.store(c.mvec_scale_y, std::memory_order_relaxed);
	l.intensity.store(c.intensity, std::memory_order_relaxed);
	l.local_tone_strength.store(c.local_tone_strength, std::memory_order_relaxed);
	l.local_structure_strength.store(c.local_structure_strength, std::memory_order_relaxed);
	l.skin_structure_strength.store(c.skin_structure_strength, std::memory_order_relaxed);
	l.style.store(c.style, std::memory_order_relaxed);
	l.use_auto_mask.store(c.use_auto_mask, std::memory_order_relaxed);
	l.ui_correction.store(c.ui_correction, std::memory_order_relaxed);
	l.bypass.store(false, std::memory_order_relaxed);

	baseline() = c;
	ini_dir() = directory;
	seeded().store(true, std::memory_order_release);
	l.epoch.fetch_add(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------
// THE ONE RENDER-THREAD HOOK.
//
// Called from nr_try_run immediately after `std::lock_guard<std::mutex> lock(st->mutex)`
// (stray_dlssnr.cpp:2193) - so it runs on the recording thread, under the lock that pass already
// holds, with nothing else able to observe g_cfg mid-write on that device.
//
// Returns FALSE to skip the pass. Every early return in nr_try_run leaves 'issued' false, which
// leaves ReShade to issue the game's own Dispatch - i.e. a strict no-op (:2127-2128). The caller
// must use a plain `return`, NOT NR_BAIL: NR_BAIL's one-shot latch (:1368-1372) would burn itself
// on the first toggle and never speak again.
//
// WHY WRITING g_cfg IS THE RIGHT SHAPE HERE. The alternative - replacing ~30 g_cfg reads across
// nr_try_run with atomic loads - is a far larger edit to a file another agent is editing, and it
// would make each of the multi-read sites in rule 3 a fresh chance to get the snapshot wrong. One
// assignment block at the top of the pass fixes every one of them at once and leaves the reads
// alone. The cost is one documented writer of g_cfg; see the note on the census line below.
// ---------------------------------------------------------------------------------------------
inline bool begin_pass(cfg::config &c,
                       bool &need_reset,
                       uint64_t &pending_res,
                       bool &pending_teardown,
                       bool &feature_failed,
                       bool codec_pipelines_ok,
                       bool codec_failed,
                       bool orig_ok)
{
	live_block  &l = live();
	status_block &s = status();

	// The TAA pass was reached and identified. This is what separates "WAITING FOR GAME DLSS"
	// from "the pass runs but the evaluate is failing", and it is published even when we are
	// about to bypass, so the status stays informative while the master switch is off.
	s.pass_seen_ms.store(static_cast<uint64_t>(GetTickCount64()), std::memory_order_relaxed);
	s.codec_pipelines_ok.store(codec_pipelines_ok, std::memory_order_relaxed);
	s.codec_failed.store(codec_failed, std::memory_order_relaxed);
	s.orig_ok.store(orig_ok, std::memory_order_relaxed);

	// ACQUIRE, and it is the FIRST thing read. Pairs with the RELEASE in bump(), so every value
	// stored by the overlay before that bump is visible to the snapshot below. Its numeric value
	// is not otherwise used - the snapshot is unconditional - so it is discarded deliberately
	// rather than kept in a variable nothing compares.
	(void)l.epoch.load(std::memory_order_acquire);
	const uint32_t reset_e  = l.reset_epoch.load(std::memory_order_relaxed);
	const uint32_t flush_e  = l.flush_epoch.load(std::memory_order_relaxed);
	const uint32_t tear_e   = l.teardown_epoch.load(std::memory_order_relaxed);

	// FUNCTION-LOCAL, therefore PROCESS-WIDE rather than per-device. With two D3D12 devices the
	// edge for a given change would be consumed by whichever device's pass ran first, and the
	// other would miss its reset frame. Making it per-device means putting these in nr_state, i.e.
	// more edits to a file this work is deliberately keeping out of. STRAY is single-device and
	// this add-on's whole identification path assumes one device already; recorded here rather
	// than glossed over. The SNAPSHOT above is unconditional and so is unaffected either way.
	static uint32_t s_seen_reset = 0, s_seen_flush = 0, s_seen_tear = 0;
	static bool     s_first = true;
	if (s_first)
	{
		// Adopt the current epochs on the very first pass rather than treating "the overlay has
		// been seeded" as a user edit; a reset frame on the first evaluate is initialisation
		// anyway, and pending_res is 0 there.
		s_seen_reset = reset_e; s_seen_flush = flush_e; s_seen_tear = tear_e;
		s_first = false;
	}

	// ---- the Reset NR feature button ---------------------------------------------------------
	// Deferred, exactly like a resolution change: pending_teardown is serviced from on_present ->
	// nr_service_pending_teardown (:3368-3404), which takes st->mutex on the MAIN thread, idles
	// the queue (:1578-1580) and releases the feature, every view and every texture (:1598-1607).
	// A recording thread must not do any of that itself. feature_failed is cleared here because a
	// latched failure is the main reason to press the button at all.
	if (tear_e != s_seen_tear)
	{
		s_seen_tear = tear_e;
		s_seen_reset = reset_e; s_seen_flush = flush_e;
		pending_teardown = true;
		feature_failed   = false;
		pending_res      = 0;
		need_reset       = true;
		s.teardown_ms.store(static_cast<uint64_t>(GetTickCount64()), std::memory_order_relaxed);
		OVERLAY_LOG_ONCE(reshade::log::level::info,
			"DLSS-NR overlay: feature reset requested. The NGX feature and every texture will be "
			"released on the next present and rebuilt on the following dispatch. This message is "
			"printed once.");
		return false;
	}

	// ---- THE SNAPSHOT ------------------------------------------------------------------------
	// Unconditional, not gated on the epoch: it is what makes every read inside this pass
	// coherent, and it is a dozen relaxed loads and stores once a frame.
	c.copy_back                = l.copy_back.load(std::memory_order_relaxed);
	c.history_restore          = l.history_restore.load(std::memory_order_relaxed);
	c.restore_graphics_root    = l.restore_graphics_root.load(std::memory_order_relaxed);
	c.paper_white_scale        = l.paper_white_scale.load(std::memory_order_relaxed);
	c.transfer_strength        = l.transfer_strength.load(std::memory_order_relaxed);
	c.color_strength           = l.color_strength.load(std::memory_order_relaxed);
	c.depth_inverted           = l.depth_inverted.load(std::memory_order_relaxed);
	c.mvec_scale_x             = l.mvec_scale_x.load(std::memory_order_relaxed);
	c.mvec_scale_y             = l.mvec_scale_y.load(std::memory_order_relaxed);
	c.intensity                = l.intensity.load(std::memory_order_relaxed);
	c.local_tone_strength      = l.local_tone_strength.load(std::memory_order_relaxed);
	c.local_structure_strength = l.local_structure_strength.load(std::memory_order_relaxed);
	c.skin_structure_strength  = l.skin_structure_strength.load(std::memory_order_relaxed);
	c.style                    = l.style.load(std::memory_order_relaxed);
	c.use_auto_mask            = l.use_auto_mask.load(std::memory_order_relaxed);
	c.ui_correction            = l.ui_correction.load(std::memory_order_relaxed);

	// ---- consequences ------------------------------------------------------------------------
	if (flush_e != s_seen_flush)
	{
		s_seen_flush = flush_e;
		s_seen_reset = reset_e;
		// See the header: pending_res is a raw resource ADDRESS held across a frame, and
		// :2504-2507 documents that UE 4.27 can recycle it. Never let one survive a toggle of the
		// thing that armed it.
		pending_res = 0;
		need_reset  = true;
	}
	else if (reset_e != s_seen_reset)
	{
		s_seen_reset = reset_e;
		need_reset   = true;
	}

	// ---- the master bypass -------------------------------------------------------------------
	// AFTER the snapshot, so g_cfg is coherent whether or not we run, and after the flush, so
	// both edges of the toggle drop any pending pristine copy.
	if (l.bypass.load(std::memory_order_relaxed))
	{
		pending_res = 0;
		return false;
	}

	return true;
}

/// Called from nr_try_run immediately after EvaluateFeature. Publishes everything the status
/// block needs; keeps its OWN evaluate counter rather than mirroring st->evaluate_count, which is
/// a plain uint64 under st->mutex and which the overlay must not reach for.
inline void publish_evaluate(uint32_t ngx_result, const char *ngx_result_name, bool ok,
                             uint32_t out_w, uint32_t out_h,
                             const char *out_fmt_name, const char *neural_fmt_name,
                             uint32_t guide_w, uint32_t guide_h,
                             float scale_x, float scale_y, bool codec_running,
                             uint64_t hist_applied, uint64_t hist_dropped)
{
	status_block &s = status();
	s.last_result.store(ngx_result, std::memory_order_relaxed);
	s.last_result_name.store(ngx_result_name, std::memory_order_relaxed);
	s.have_result.store(true, std::memory_order_relaxed);
	s.out_w.store(out_w, std::memory_order_relaxed);
	s.out_h.store(out_h, std::memory_order_relaxed);
	s.out_fmt.store(out_fmt_name, std::memory_order_relaxed);
	s.neural_fmt.store(neural_fmt_name, std::memory_order_relaxed);
	s.guide_w.store(guide_w, std::memory_order_relaxed);
	s.guide_h.store(guide_h, std::memory_order_relaxed);
	s.auto_scale_x.store(scale_x, std::memory_order_relaxed);
	s.auto_scale_y.store(scale_y, std::memory_order_relaxed);
	s.codec_running.store(codec_running, std::memory_order_relaxed);
	s.hist_applied.store(hist_applied, std::memory_order_relaxed);
	s.hist_dropped.store(hist_dropped, std::memory_order_relaxed);
	if (ok)
	{
		s.evaluates.fetch_add(1, std::memory_order_relaxed);
		// THE ONE FIELD THE renodx STATUS BLOCK HAS NO EQUIVALENT OF. Its "ACTIVE" means "a
		// feature object currently exists" and its frame counter only ever increments, so an
		// add-on that stopped evaluating ten minutes ago still reads ACTIVE with a large count.
		// A timestamp is what turns that into an answerable question.
		s.eval_ms.store(static_cast<uint64_t>(GetTickCount64()), std::memory_order_relaxed);
		s.teardown_ms.store(0, std::memory_order_relaxed);
	}
	else
	{
		s.eval_failures.fetch_add(1, std::memory_order_relaxed);
	}
}

/// The live values of the two settings the periodic census in on_present prints. Exists so that
/// census line does not read g_cfg fields that begin_pass writes on the render thread - the only
/// cross-thread reader of a snapshot-written field anywhere in the add-on.
inline bool live_copy_back()       { return live().copy_back.load(std::memory_order_relaxed); }
inline bool live_history_restore() { return live().history_restore.load(std::memory_order_relaxed); }

// =============================================================================================
// PERSISTENCE
//
// stray_dlssnr.ini stays the one source of truth, and we write it ourselves.
//
// NOT ReShade's config API, for four reasons, the first two of which are already written down in
// this tree:
//   1. reshade.hpp:198-199 - set_config_value "Sets AND SAVES". That is one ReShade.ini write per
//      frame of a slider drag.
//   2. addon_config.hpp:3-5 already argues the case: ReShade's get_config_value keys off
//      ReShade.ini, "which the user is also editing for effects, and a missing key there silently
//      yields a default with no diagnostic. Here every parse is reported."
//   3. ReShade rewrites ReShade.ini itself - that is how DisabledAddons got written in the first
//      place. Two writers, one file.
//   4. It would split the source of truth: the ini beside the add-on is still read at :3116.
//
// WRITE POLICY
//   * Only on the explicit Save button. Never per-frame, never on a drag.
//   * REWRITE IN PLACE. The shipped stray_dlssnr.ini is 200-odd commented lines and is the
//     documentation the user actually reads; a naive regenerate destroys it. Every comment, every
//     unrecognised line, every blank line and the original key order and spelling survive.
//   * TEMP FILE + MoveFileExW(REPLACE_EXISTING). A half-written ini is worse than none: per
//     addon_config.hpp:219-223 every key after the cut silently takes its built-in default.
//   * NEVER round-trip a key the UI does not own. shader_hash, srv_*, uav_output, app_id,
//     require_trampoline, populate_parameters, enabled, diagnostics and hdr_codec are read-only
//     here and are not touched by the writer - clobbering a hand-measured identification pin is
//     how a working config gets lost.
// =============================================================================================

inline void fmt_float(char *buf, size_t n, float v)
{
	// %.9g round-trips a float exactly and still prints 1.0f as "1" and -1.0f as "-1".
	std::snprintf(buf, n, "%.9g", static_cast<double>(v));
}

/// The 16 keys the overlay owns. Returns nullptr for anything else.
inline bool owned_value(const std::string &key_lower, const live_block &l, std::string &out)
{
	char buf[64];
	if (key_lower == "copy_back")                { out = l.copy_back.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "history_restore")          { out = l.history_restore.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "restore_graphics_root")    { out = l.restore_graphics_root.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "depth_inverted")           { out = l.depth_inverted.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "use_auto_mask")            { out = l.use_auto_mask.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "paper_white_scale")        { fmt_float(buf, sizeof(buf), l.paper_white_scale.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "transfer_strength")        { fmt_float(buf, sizeof(buf), l.transfer_strength.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "color_strength" ||
	    key_lower == "colour_strength")          { fmt_float(buf, sizeof(buf), l.color_strength.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "mvec_scale_x")             { fmt_float(buf, sizeof(buf), l.mvec_scale_x.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "mvec_scale_y")             { fmt_float(buf, sizeof(buf), l.mvec_scale_y.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "intensity")                { fmt_float(buf, sizeof(buf), l.intensity.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "local_tone_strength")      { fmt_float(buf, sizeof(buf), l.local_tone_strength.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "local_structure_strength") { fmt_float(buf, sizeof(buf), l.local_structure_strength.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "skin_structure_strength")  { fmt_float(buf, sizeof(buf), l.skin_structure_strength.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "style")                    { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.style.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "ui_correction")            { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.ui_correction.load(std::memory_order_relaxed)); out = buf; return true; }
	return false;
}

// The canonical spellings, in the order they are appended when absent from the file.
inline const char *const *owned_keys(size_t &n)
{
	static const char *const keys[] = {
		"copy_back", "history_restore", "restore_graphics_root",
		"paper_white_scale", "transfer_strength", "color_strength",
		"depth_inverted", "mvec_scale_x", "mvec_scale_y",
		"intensity", "local_tone_strength", "local_structure_strength",
		"skin_structure_strength", "style", "use_auto_mask", "ui_correction",
	};
	n = sizeof(keys) / sizeof(keys[0]);
	return keys;
}

inline std::string lower_copy(const std::string &s)
{
	std::string r = s;
	for (char &ch : r) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return r;
}

/// Rewrite stray_dlssnr.ini in place. Returns false and fills 'err' on any failure; never throws.
inline bool save_ini(std::string &err)
{
	err.clear();
	if (ini_dir().empty())
	{
		err = "the add-on directory is not known yet (nr_init_device has not run).";
		return false;
	}

	const std::wstring path = ini_dir() + L"stray_dlssnr.ini";
	const std::wstring tmp  = ini_dir() + L"stray_dlssnr.ini.tmp";
	const live_block &l = live();

	std::vector<std::string> lines;
	bool had_file = false;
	try
	{
		FILE *f = _wfopen(path.c_str(), L"rb");
		if (f != nullptr)
		{
			had_file = true;
			std::string all;
			char chunk[4096];
			size_t got;
			while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
			{
				all.append(chunk, got);
				if (all.size() > 4u * 1024u * 1024u)   // a stray_dlssnr.ini is ~12 KB
					break;
			}
			std::fclose(f);

			size_t start = 0;
			while (start <= all.size())
			{
				const size_t nl = all.find('\n', start);
				if (nl == std::string::npos) { if (start < all.size()) lines.push_back(all.substr(start)); break; }
				lines.push_back(all.substr(start, nl - start));   // '\r' is kept and re-emitted
				start = nl + 1;
			}
		}
	}
	catch (...)
	{
		err = "reading the existing stray_dlssnr.ini threw.";
		return false;
	}

	size_t n_keys = 0;
	const char *const *keys = owned_keys(n_keys);
	std::vector<bool> written(n_keys, false);

	try
	{
		for (std::string &line : lines)
		{
			// Peel the line apart into  <head '='> <pad> <value> <gap> <comment> <eol>  and put back
			// every one of those pieces byte for byte except the value. Anything less exact reflows
			// a file the user reads as documentation: the shipped stray_dlssnr.ini pads its tuning
			// block into columns and puts trailing comments after several values, and a writer that
			// normalised either would rewrite two hundred lines the first time Save was pressed.
			// (Both of those were real defects here, caught by abi/ini_rewrite_test.cpp.)
			std::string content = line;
			std::string eol;
			if (!content.empty() && content.back() == '\r') { eol = "\r"; content.pop_back(); }

			// Comment from the first ';' or '#', exactly as cfg::load does.
			std::string comment;
			const size_t sc = content.find_first_of(";#");
			if (sc != std::string::npos) { comment = content.substr(sc); content = content.substr(0, sc); }

			const size_t eq = content.find('=');
			if (eq == std::string::npos)
				continue;

			std::string key_trim = content.substr(0, eq); cfg::trim(key_trim);
			if (key_trim.empty() || key_trim[0] == '[')
				continue;

			const std::string kl = lower_copy(key_trim);
			std::string value;
			if (!owned_value(kl, l, value))
				continue;   // not ours: left exactly as it was

			const std::string head  = content.substr(0, eq + 1);
			const std::string vpart = content.substr(eq + 1);
			const size_t vs = vpart.find_first_not_of(" \t");
			const size_t ve = vpart.find_last_not_of(" \t");
			// Whitespace before the value keeps the column alignment; whitespace after it keeps the
			// distance to a trailing comment.
			const std::string vpad = (vs == std::string::npos) ? std::string(" ") : vpart.substr(0, vs);
			const std::string gap  = (vs == std::string::npos || ve == std::string::npos)
				? std::string() : vpart.substr(ve + 1);

			line = head + vpad + value + gap + comment + eol;

			for (size_t i = 0; i < n_keys; ++i)
			{
				// color_strength / colour_strength are the same setting; the canonical key is
				// "color_strength" and either spelling in the file counts as written.
				const std::string canon = (kl == "colour_strength") ? std::string("color_strength") : kl;
				if (canon == keys[i]) { written[i] = true; break; }
			}
		}

		// Anything the file never mentioned gets appended in one clearly labelled block.
		std::string extra;
		for (size_t i = 0; i < n_keys; ++i)
		{
			if (written[i])
				continue;
			std::string v;
			if (!owned_value(keys[i], l, v))
				continue;
			extra += std::string(keys[i]) + " = " + v + "\n";
		}
		if (!extra.empty())
		{
			if (!lines.empty() && !lines.back().empty())
				lines.push_back(std::string());
			lines.push_back("; ---------------------------------------------------------------------------------------------");
			lines.push_back("; Written by the STRAY DLSS-NR overlay. Everything above is untouched.");
			lines.push_back("; ---------------------------------------------------------------------------------------------");
			size_t start = 0;
			while (start < extra.size())
			{
				const size_t nl = extra.find('\n', start);
				lines.push_back(extra.substr(start, nl - start));
				start = nl + 1;
			}
		}
		if (!had_file)
		{
			lines.insert(lines.begin(), "; stray_dlssnr.ini - created by the STRAY DLSS-NR overlay.");
			lines.insert(lines.begin() + 1, "; Every key not listed here keeps its built-in default; see the shipped ini for the full set.");
			lines.insert(lines.begin() + 2, "[stray_dlssnr]");
		}
	}
	catch (...)
	{
		err = "building the new stray_dlssnr.ini threw.";
		return false;
	}

	// TEMP FILE + ATOMIC-ISH REPLACE. If the write dies half way, the original is still whole.
	FILE *out = _wfopen(tmp.c_str(), L"wb");
	if (out == nullptr)
	{
		err = "could not open stray_dlssnr.ini.tmp for writing (is the add-on's directory writable?).";
		return false;
	}
	bool write_ok = true;
	for (const std::string &line : lines)
	{
		if (std::fwrite(line.data(), 1, line.size(), out) != line.size()) { write_ok = false; break; }
		if (std::fputc('\n', out) == EOF) { write_ok = false; break; }
	}
	if (std::fflush(out) != 0) write_ok = false;
	std::fclose(out);
	if (!write_ok)
	{
		DeleteFileW(tmp.c_str());
		err = "writing stray_dlssnr.ini.tmp failed (disk full, or read-only install?).";
		return false;
	}

	if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		const DWORD e = GetLastError();
		DeleteFileW(tmp.c_str());
		char buf[160];
		std::snprintf(buf, sizeof(buf), "could not replace stray_dlssnr.ini (Win32 error %lu). The original is unchanged.", (unsigned long)e);
		err = buf;
		return false;
	}

	// The file now IS the live state, so nothing is dirty any more.
	cfg::config &b = baseline();
	b.copy_back                = l.copy_back.load(std::memory_order_relaxed);
	b.history_restore          = l.history_restore.load(std::memory_order_relaxed);
	b.restore_graphics_root    = l.restore_graphics_root.load(std::memory_order_relaxed);
	b.paper_white_scale        = l.paper_white_scale.load(std::memory_order_relaxed);
	b.transfer_strength        = l.transfer_strength.load(std::memory_order_relaxed);
	b.color_strength           = l.color_strength.load(std::memory_order_relaxed);
	b.depth_inverted           = l.depth_inverted.load(std::memory_order_relaxed);
	b.mvec_scale_x             = l.mvec_scale_x.load(std::memory_order_relaxed);
	b.mvec_scale_y             = l.mvec_scale_y.load(std::memory_order_relaxed);
	b.intensity                = l.intensity.load(std::memory_order_relaxed);
	b.local_tone_strength      = l.local_tone_strength.load(std::memory_order_relaxed);
	b.local_structure_strength = l.local_structure_strength.load(std::memory_order_relaxed);
	b.skin_structure_strength  = l.skin_structure_strength.load(std::memory_order_relaxed);
	b.style                    = l.style.load(std::memory_order_relaxed);
	b.use_auto_mask            = l.use_auto_mask.load(std::memory_order_relaxed);
	b.ui_correction            = l.ui_correction.load(std::memory_order_relaxed);

	logf(reshade::log::level::info, "DLSS-NR overlay: saved %zu setting(s) to stray_dlssnr.ini "
	     "(rewritten in place; comments, ordering and every key the overlay does not own were preserved).", n_keys);
	return true;
}

inline bool dirty()
{
	const live_block &l = live();
	const cfg::config &b = baseline();
	return l.copy_back.load(std::memory_order_relaxed)                != b.copy_back
	    || l.history_restore.load(std::memory_order_relaxed)          != b.history_restore
	    || l.restore_graphics_root.load(std::memory_order_relaxed)    != b.restore_graphics_root
	    || l.paper_white_scale.load(std::memory_order_relaxed)        != b.paper_white_scale
	    || l.transfer_strength.load(std::memory_order_relaxed)        != b.transfer_strength
	    || l.color_strength.load(std::memory_order_relaxed)           != b.color_strength
	    || l.depth_inverted.load(std::memory_order_relaxed)           != b.depth_inverted
	    || l.mvec_scale_x.load(std::memory_order_relaxed)             != b.mvec_scale_x
	    || l.mvec_scale_y.load(std::memory_order_relaxed)             != b.mvec_scale_y
	    || l.intensity.load(std::memory_order_relaxed)                != b.intensity
	    || l.local_tone_strength.load(std::memory_order_relaxed)      != b.local_tone_strength
	    || l.local_structure_strength.load(std::memory_order_relaxed) != b.local_structure_strength
	    || l.skin_structure_strength.load(std::memory_order_relaxed)  != b.skin_structure_strength
	    || l.style.load(std::memory_order_relaxed)                    != b.style
	    || l.use_auto_mask.load(std::memory_order_relaxed)            != b.use_auto_mask
	    || l.ui_correction.load(std::memory_order_relaxed)            != b.ui_correction;
}

// =============================================================================================
// THE DisabledAddons WATCHDOG
//
// The failure this whole overlay exists to make visible is the add-on NOT LOADING, in which case
// no overlay of ours draws at all and no status line can report anything. The mitigation is
// therefore structural first and cosmetic second:
//
//   * We register with title = nullptr, i.e. as a SETTINGS overlay (reshade.hpp:342). ReShade
//     then draws this panel in the Add-ons tab DIRECTLY UNDER the same checkbox that disables us.
//     Its ABSENCE is then sitting exactly where the user is already looking.
//   * And, in the session where the box is actually unticked, we can still catch it: ReShade
//     writes the DisabledAddons entry immediately while the add-on keeps running to the end of
//     the session, so polling the value once a second lets us shout before the damage is done.
//
// [ASSUMED] the section/key spelling "ADDON" / "DisabledAddons". It could not be verified from
// this tree - no ReShade source is vendored and neither string appears in include/ - so the
// failure mode was chosen to be safe: an unknown section or key returns false and the banner
// simply never draws. Two candidate sections are tried. A reviewer with the game running should
// untick the add-on once and confirm the banner appears.
// =============================================================================================
inline bool config_string(reshade::api::effect_runtime *rt, const char *section, const char *key, std::string &out)
{
	// Resolved by hand rather than through reshade::get_config_value: that inline calls the
	// resolved pointer with NO null check, so a ReShade build without the export would be a call
	// through nullptr on the present thread.
	using fn_t = bool (*)(void *, reshade::api::effect_runtime *, const char *, const char *, char *, size_t *);
	static const fn_t fn = []() -> fn_t {
		HMODULE m = reshade::internal::get_reshade_module_handle();
		return (m == nullptr) ? nullptr : reinterpret_cast<fn_t>(GetProcAddress(m, "ReShadeGetConfigValue"));
	}();
	if (fn == nullptr)
		return false;

	void *const self = reshade::internal::get_current_module_handle();
	size_t need = 0;
	if (!fn(self, rt, section, key, nullptr, &need) || need == 0 || need > 256u * 1024u)
		return false;

	std::string buf(need + 1u, '\0');
	size_t have = need + 1u;
	if (!fn(self, rt, section, key, &buf[0], &have))
		return false;
	if (have > buf.size())
		have = buf.size();
	buf.resize(have);
	while (!buf.empty() && buf.back() == '\0')
		buf.pop_back();
	out.swap(buf);
	return true;
}

/// True when ReShade's config currently lists this add-on as disabled. Polled at most once a
/// second; the answer is cached between polls.
inline bool listed_as_disabled(reshade::api::effect_runtime *rt, const char *addon_name)
{
	static uint64_t s_next_poll_ms = 0;
	static bool     s_answer = false;
	const uint64_t now = static_cast<uint64_t>(GetTickCount64());
	if (now < s_next_poll_ms)
		return s_answer;
	s_next_poll_ms = now + 1000u;

	if (addon_name == nullptr || *addon_name == '\0')
		return (s_answer = false);

	std::string value;
	bool got = config_string(nullptr, "ADDON", "DisabledAddons", value);
	if (!got) got = config_string(rt, "ADDON", "DisabledAddons", value);
	if (!got) got = config_string(nullptr, "GENERAL", "DisabledAddons", value);
	if (!got)
		return (s_answer = false);

	s_answer = value.find(addon_name) != std::string::npos;
	if (s_answer)
		OVERLAY_LOG_ONCE(reshade::log::level::warning,
			"DLSS-NR overlay: ReShade's config lists this add-on in DisabledAddons (\"%s\"). It is "
			"still running for the REST OF THIS SESSION, but it will NOT load next launch and the "
			"game will run with no denoise and no warning. Re-tick it in the Add-ons tab, or "
			"remove the entry from ReShade.ini. This message is printed once.", value.c_str());
	return s_answer;
}

// =============================================================================================
// DRAWING
// =============================================================================================

// Reset kinds for bump().
enum : uint32_t { k_plain = 0, k_reset = 1, k_flush = 2 };

inline void bump(uint32_t kind)
{
	live_block &l = live();
	if (kind == k_flush)      l.flush_epoch.fetch_add(1, std::memory_order_relaxed);
	else if (kind == k_reset) l.reset_epoch.fetch_add(1, std::memory_order_relaxed);
	// RELEASE last, and it is the only ordering edge in the design: begin_pass loads this with
	// ACQUIRE before reading any value, so everything stored above is visible to it.
	l.epoch.fetch_add(1, std::memory_order_release);
}

namespace col {
inline const ImVec4 green { 0.36f, 0.85f, 0.40f, 1.0f };
inline const ImVec4 amber { 1.00f, 0.75f, 0.20f, 1.0f };
inline const ImVec4 red   { 1.00f, 0.38f, 0.34f, 1.0f };
inline const ImVec4 dim   { 0.62f, 0.62f, 0.64f, 1.0f };
}

/// A slider bound to a live atomic. Clamps on our side too: ImGui lets a ctrl-click type any
/// value, and none of these knobs has a documented behaviour outside its range.
inline void slider_f(const char *label, std::atomic<float> &a, float lo, float hi,
                     const char *fmt, uint32_t kind, const char *help)
{
	float v = a.load(std::memory_order_relaxed);
	if (ImGui::SliderFloat(label, &v, lo, hi, fmt))
	{
		if (v < lo) v = lo;
		if (v > hi) v = hi;
		a.store(v, std::memory_order_relaxed);
		bump(kind);
	}
	if (help != nullptr)
		ImGui::SetItemTooltip("%s", help);
}

inline void checkbox_b(const char *label, std::atomic<bool> &a, uint32_t kind, const char *help)
{
	bool v = a.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(label, &v))
	{
		a.store(v, std::memory_order_relaxed);
		bump(kind);
	}
	if (help != nullptr)
		ImGui::SetItemTooltip("%s", help);
}

inline void combo_u32(const char *label, std::atomic<uint32_t> &a, const char *const *items,
                      int count, uint32_t kind, const char *help)
{
	int v = static_cast<int>(a.load(std::memory_order_relaxed));
	if (v < 0) v = 0;
	if (v >= count) v = count - 1;
	if (ImGui::Combo(label, &v, items, count))
	{
		a.store(static_cast<uint32_t>(v < 0 ? 0 : v), std::memory_order_relaxed);
		bump(kind);
	}
	if (help != nullptr)
		ImGui::SetItemTooltip("%s", help);
}

/// A read-only line for a setting that exists but cannot be changed at runtime. Greyed rather
/// than hidden, per the brief: a user must be able to see what the ini said without leaving the
/// game, and must be able to see WHY it is not editable.
inline void load_only(const char *label, const char *value, const char *why)
{
	ImGui::BeginDisabled(true);
	ImGui::TextUnformatted(label);
	ImGui::EndDisabled();
	ImGui::SameLine();
	overlay_imgui::textf_colored(col::dim, "%s", value);
	if (why != nullptr)
	{
		ImGui::Indent();
		overlay_imgui::textf_colored(col::dim, "load-only: %s", why);
		ImGui::Unindent();
	}
}

inline void revert_to_baseline()
{
	live_block &l = live();
	const cfg::config &b = baseline();
	l.copy_back.store(b.copy_back, std::memory_order_relaxed);
	l.history_restore.store(b.history_restore, std::memory_order_relaxed);
	l.restore_graphics_root.store(b.restore_graphics_root, std::memory_order_relaxed);
	l.paper_white_scale.store(b.paper_white_scale, std::memory_order_relaxed);
	l.transfer_strength.store(b.transfer_strength, std::memory_order_relaxed);
	l.color_strength.store(b.color_strength, std::memory_order_relaxed);
	l.depth_inverted.store(b.depth_inverted, std::memory_order_relaxed);
	l.mvec_scale_x.store(b.mvec_scale_x, std::memory_order_relaxed);
	l.mvec_scale_y.store(b.mvec_scale_y, std::memory_order_relaxed);
	l.intensity.store(b.intensity, std::memory_order_relaxed);
	l.local_tone_strength.store(b.local_tone_strength, std::memory_order_relaxed);
	l.local_structure_strength.store(b.local_structure_strength, std::memory_order_relaxed);
	l.skin_structure_strength.store(b.skin_structure_strength, std::memory_order_relaxed);
	l.style.store(b.style, std::memory_order_relaxed);
	l.use_auto_mask.store(b.use_auto_mask, std::memory_order_relaxed);
	l.ui_correction.store(b.ui_correction, std::memory_order_relaxed);
	bump(k_flush);
}

// ---------------------------------------------------------------------------------------------
// THE STATUS BLOCK. Drawn first, always, and never disabled.
//
// The ladder is strictest-first, and the fourth rung is the one renodx does not have:
//   the add-on is listed in DisabledAddons ....... red banner, above everything
//   enabled=0 in the ini ......................... DISABLED (this session)
//   the snippet did not load ..................... WAITING FOR NGX + the reason
//   NGX not initialised yet ...................... STANDBY
//   the TAA pass has never been reached .......... WAITING FOR GAME DLSS
//   the overlay's own master switch is off ....... BYPASSED
//   a feature reset is pending ................... REBUILDING
//   an evaluate happened, but not recently ....... NOT EVALUATING - <n> s ago   <-- the new rung
//   otherwise .................................... EVALUATING
// ---------------------------------------------------------------------------------------------
inline void draw_status(reshade::api::effect_runtime *rt, const host_facts &f)
{
	const status_block &s = status();
	const uint64_t now      = static_cast<uint64_t>(GetTickCount64());
	const uint64_t eval_ms  = s.eval_ms.load(std::memory_order_relaxed);
	const uint64_t pass_ms  = s.pass_seen_ms.load(std::memory_order_relaxed);
	const uint64_t evals    = s.evaluates.load(std::memory_order_relaxed);
	const uint64_t fails    = s.eval_failures.load(std::memory_order_relaxed);
	const uint64_t tear_ms  = s.teardown_ms.load(std::memory_order_relaxed);
	const double   eval_age = (eval_ms == 0) ? -1.0 : (double)(now - eval_ms) / 1000.0;

	if (listed_as_disabled(rt, f.addon_name))
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col::red);
		ImGui::TextWrapped("YOU HAVE DISABLED THIS ADD-ON. ReShade's config lists \"%s\" in "
		                   "DisabledAddons. It is still running for the rest of THIS session, but "
		                   "it will NOT load next launch - and when that happens there is no "
		                   "overlay, no log line and no visible difference except that the "
		                   "denoise is gone. Re-tick it in the Add-ons tab, or remove the entry "
		                   "from ReShade.ini.", f.addon_name);
		ImGui::PopStyleColor(1);
		ImGui::Separator();
	}

	if (!f.valid)
	{
		overlay_imgui::textf_colored(col::red, "STATUS UNAVAILABLE - the add-on did not install its status hook.");
		return;
	}

	if (!f.enabled_at_load)
	{
		overlay_imgui::textf_colored(col::red, "DISABLED - enabled=0 in stray_dlssnr.ini");
		ImGui::TextWrapped("The add-on is a strict no-op this session: no snippet was loaded and no "
		                   "resource was created. Set enabled=1 in stray_dlssnr.ini and restart the "
		                   "game. Nothing in this panel can turn it on now, because there is nothing "
		                   "loaded to turn on.");
		return;
	}
	if (!f.snippet_loaded)
	{
		overlay_imgui::textf_colored(col::red, "WAITING FOR NGX - the snippet is not loaded");
		ImGui::TextWrapped("%s", f.snippet_reason[0] != '\0' ? f.snippet_reason
		                        : "nvngx_dlssnr.dll could not be loaded (no reason was recorded).");
		if (f.require_trampoline && !f.trampoline)
			ImGui::TextWrapped("remix_nvngx.dll is required (require_trampoline=1) and was not found "
			                   "beside the add-on. Every GATED snippet export would return 0xbad00002 "
			                   "without it.");
		return;
	}
	if (!f.armed)
	{
		overlay_imgui::textf_colored(col::amber, "STANDBY - NGX has not been initialised yet");
		ImGui::TextWrapped("The snippet is loaded. NVSDK_NGX_D3D12_Init_Ext is deliberately deferred "
		                   "to the first render-thread dispatch (calling it from init_device hangs "
		                   "this title), so this clears itself as soon as the game renders. If it "
		                   "persists, ReShade.log has the Init_Ext result.");
		return;
	}

	// Read from the LIVE block, not from a mirror the render thread publishes. The overlay owns
	// this value, so it is authoritative here - and a mirror would report the wrong thing whenever
	// the pass is not being reached at all (nothing to echo it), which is precisely the case where
	// the user most needs to know the switch is off.
	if (live().bypass.load(std::memory_order_relaxed))
	{
		overlay_imgui::textf_colored(col::amber, "BYPASSED - \"Enable DLSS Neural Rendering\" is off in this panel");
		ImGui::TextWrapped("The TAA pass is still being identified and the game's own dispatch is "
		                   "issued untouched. Nothing is denoised and nothing is written back.");
	}
	else if (tear_ms != 0 && (now - tear_ms) < 3000u)
	{
		overlay_imgui::textf_colored(col::amber, "REBUILDING - the NGX feature is being released and recreated");
	}
	else if (pass_ms == 0)
	{
		overlay_imgui::textf_colored(col::amber, "WAITING FOR GAME DLSS - the target TAA dispatch has not been seen");
		ImGui::TextWrapped("NGX is up but no dispatch has matched shader_hash 0x%016llx with the "
		                   "configured SRV registers. Check the shader identification below, and "
		                   "ReShade.log for the one-shot \"pass did not run\" line that names the "
		                   "exact reason.", (unsigned long long)f.shader_hash);
	}
	else if (evals == 0)
	{
		overlay_imgui::textf_colored(col::red, "NOT EVALUATING - the TAA pass is reached but no evaluate has succeeded");
		if (s.have_result.load(std::memory_order_relaxed))
		{
			const char *nm = s.last_result_name.load(std::memory_order_relaxed);
			overlay_imgui::textf_colored(col::red, "last NGX result: 0x%08X  %s",
				(unsigned)s.last_result.load(std::memory_order_relaxed), nm != nullptr ? nm : "");
		}
	}
	else if (eval_age >= 0.0 && eval_age <= 0.25)
	{
		overlay_imgui::textf_colored(col::green, "EVALUATING - last evaluate %.2f s ago", eval_age);
	}
	else
	{
		// The line that would have caught the lost session. A cumulative count cannot distinguish
		// "14203 and climbing" from "14203 and stopped"; an age can.
		overlay_imgui::textf_colored(col::amber, "NOT EVALUATING RIGHT NOW - last evaluate %.1f s ago", eval_age);
		ImGui::TextWrapped("The counter below is cumulative and will keep showing the old total. If "
		                   "this figure keeps growing while you play, the denoise has stopped.");
	}

	// ---- the detail lines --------------------------------------------------------------------
	const char *ofmt = s.out_fmt.load(std::memory_order_relaxed);
	const char *nfmt = s.neural_fmt.load(std::memory_order_relaxed);
	const char *rnm  = s.last_result_name.load(std::memory_order_relaxed);

	overlay_imgui::textf("evaluates %llu   failed %llu   last TAA dispatch %.1f s ago",
		(unsigned long long)evals, (unsigned long long)fails,
		pass_ms == 0 ? 0.0 : (double)(now - pass_ms) / 1000.0);

	if (s.have_result.load(std::memory_order_relaxed))
		overlay_imgui::textf("last NGX result  0x%08X  %s",
			(unsigned)s.last_result.load(std::memory_order_relaxed), rnm != nullptr ? rnm : "");
	else
		overlay_imgui::textf_colored(col::dim, "last NGX result  (no evaluate has been attempted yet)");

	overlay_imgui::textf("output %ux%u %s   network target %s   guides %ux%u",
		(unsigned)s.out_w.load(std::memory_order_relaxed), (unsigned)s.out_h.load(std::memory_order_relaxed),
		ofmt != nullptr ? ofmt : "?", nfmt != nullptr ? nfmt : "?",
		(unsigned)s.guide_w.load(std::memory_order_relaxed), (unsigned)s.guide_h.load(std::memory_order_relaxed));

	const bool codec_running = s.codec_running.load(std::memory_order_relaxed);
	const bool codec_failed  = s.codec_failed.load(std::memory_order_relaxed);
	// This flag is refreshed only when an evaluate happens, so once the pass stops it would keep
	// claiming RUNNING underneath a NOT EVALUATING headline. Say so instead of contradicting it.
	const bool codec_fresh   = (eval_ms != 0) && (now - eval_ms) <= 250u;
	if (!f.hdr_codec_at_load)
		overlay_imgui::textf_colored(col::amber, "HDR codec  OFF (hdr_codec=0) - the network is fed the RAW linear TAA output (README gap 1: the darkening)");
	else if (codec_running)
		overlay_imgui::textf_colored(codec_fresh ? col::green : col::dim,
			"HDR codec  %s - the network sees the display-referred proxy",
			codec_fresh ? "RUNNING" : "was running at the last evaluate");
	else if (codec_failed)
		overlay_imgui::textf_colored(col::red, "HDR codec  FAILED - its shaders or pipelines could not be created (see ReShade.log). The denoise still runs, undecoded.");
	else
		overlay_imgui::textf_colored(col::amber, "HDR codec  NOT RUNNING YET - %s",
			s.orig_ok.load(std::memory_order_relaxed) ? "waiting for its textures" : "the pre-denoise copy is not allocated");

	overlay_imgui::textf("history restore  applied %llu  dropped %llu%s",
		(unsigned long long)s.hist_applied.load(std::memory_order_relaxed),
		(unsigned long long)s.hist_dropped.load(std::memory_order_relaxed),
		live_history_restore() ? "" : "   (off)");

	overlay_imgui::textf_colored(col::dim, "trampoline %s   C++ ABI %s   ImGui table bound",
		f.trampoline ? "remix_nvngx.dll" : "ABSENT",
		f.abi_thunks_active ? "Itanium/GNU + out-param thunks" : "Microsoft (direct)");
}

// ---------------------------------------------------------------------------------------------
// THE CONTROLS. Layout and labels mimic renodx's "DLSS 5 Neural Rendering" panel; the section
// headings are its own strings where they still describe what we do.
// ---------------------------------------------------------------------------------------------
inline void draw_controls(const host_facts &f)
{
	live_block &l = live();
	const status_block &s = status();

	// renodx disables NOTHING, so its sliders stay interactive when NGX never loaded and a user
	// can spend a while tuning something that reaches nothing. That is a defect, not a style
	// choice, and it is not copied.
	const bool usable = f.valid && f.enabled_at_load && f.snippet_loaded && f.armed;

	ImGui::BeginDisabled(!usable);

	ImGui::SeparatorText("DLSS 5 Neural Rendering");

	// ---- master ------------------------------------------------------------------------------
	{
		bool on = !l.bypass.load(std::memory_order_relaxed);
		if (ImGui::Checkbox("Enable DLSS Neural Rendering", &on))
		{
			l.bypass.store(!on, std::memory_order_relaxed);
			// k_flush, both directions: the toggle must not leave a pending pristine copy armed.
			// st->pending_res is a raw resource address held across a frame and UE 4.27 can
			// recycle it (stray_dlssnr.cpp:2504-2507).
			bump(k_flush);
		}
		ImGui::SetItemTooltip(
			"Live, per dispatch. This is NOT the ini's `enabled` key - that one is read once at load "
			"(stray_dlssnr.cpp:3119) and a checkbox bound to it would silently do nothing until the "
			"game was restarted. Off means the add-on identifies the TAA pass and then lets ReShade "
			"issue the game's own dispatch untouched: a strict no-op, reversible in one click.");
	}

	if (ImGui::Button("Reset NR feature"))
	{
		l.teardown_epoch.fetch_add(1, std::memory_order_relaxed);
		l.epoch.fetch_add(1, std::memory_order_release);
	}
	ImGui::SetItemTooltip(
		"Releases the NGX feature, every view and every texture on the next present (on the main "
		"thread, after the queue is idle), clears the latched create-failure, and rebuilds "
		"everything on the following dispatch. This is the control to reach for when something has "
		"wedged - it is the single most useful button in the reference add-on too.");

	ImGui::SameLine();
	ImGui::BeginDisabled(!dirty());
	if (ImGui::Button("Revert to stray_dlssnr.ini"))
		revert_to_baseline();
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Puts every control below back to the value that was on disk at load, or "
	                      "at the last Save. Live, like any other change.");

	// ---- network tuning ----------------------------------------------------------------------
	ImGui::SeparatorText("Network");

	{
		static const char *const style_items[] = { "Default", "Natural", "Cinematic" };
		combo_u32("NR Style", l.style, style_items, 3, k_reset,
			"Live: DLSSNR.Style is written on every evaluate (stray_dlssnr.cpp:2797) and is NOT baked "
			"at CreateFeature. The names are the reference add-on's; only the INDEX reaches the "
			"snippet, which clamps it to the range the network actually exposes, so a value beyond "
			"that aliases onto the last real style rather than failing.");
	}

	slider_f("NR Intensity", l.intensity, 0.0f, 2.0f, "%.2f", k_reset,
		"Live. 1.0 is the snippet's OWN fallback recovered from its disassembly, not a calibrated "
		"neutral midpoint, and the scale these values sit on is not known.");

	slider_f("Local Tone Strength", l.local_tone_strength, 0.0f, 2.0f, "%.2f", k_reset,
		"Live. Same caveat as Intensity: 1.0 is the snippet's fallback, not a measured neutral.");

	const bool mask_on = l.use_auto_mask.load(std::memory_order_relaxed);

	ImGui::BeginDisabled(!mask_on);
	slider_f("Local Structure Strength", l.local_structure_strength, 0.0f, 2.0f, "%.2f", k_reset,
		"Live. Requires Automatic Mask: with the mask off the snippet internally forces BOTH "
		"structure strengths to -1 and neither does anything.");

	{
		bool inherit = l.skin_structure_strength.load(std::memory_order_relaxed) < 0.0f;
		if (ImGui::Checkbox("Skin Structure: inherit Local Structure", &inherit))
		{
			l.skin_structure_strength.store(inherit ? -1.0f : 1.0f, std::memory_order_relaxed);
			bump(k_reset);
		}
		ImGui::SetItemTooltip(
			"A NEGATIVE skin structure strength means \"use the local structure strength\": the "
			"snippet does an explicit comiss against 0 and copies the local value on the other "
			"branch. This checkbox is why there is no slider position that means it - 0.0 is NOT "
			"neutral, it FLATTENS skin structure, and a bare 0..1 slider would put that trap one "
			"drag from the left edge.");

		ImGui::BeginDisabled(inherit);
		slider_f("Skin Structure Strength", l.skin_structure_strength, 0.0f, 2.0f, "%.2f", k_reset,
			"Live. 0.0 flattens skin structure; it is not a bypass. Untick \"inherit\" to reach it.");
		ImGui::EndDisabled();
	}
	ImGui::EndDisabled();

	checkbox_b("Automatic Mask", l.use_auto_mask, k_reset,
		"Live. Gates BOTH structure strengths - with this off the snippet forces both to -1 "
		"internally, which is why those two sliders grey out. This add-on binds no ControlMask, so "
		"the snippet's other route to forcing the mask off never fires here.");


	{
		// PRESENT, and unlike the other three renodx extras this one is real. `DLSSNR.UICorrection`
		// IS in this snippet's string table (measured with an exact-line strings match against
		// nvngx_dlssnr.dll), and it is read as Get(const char*, int*) with a proper 0xbad00000
		// guard and a fallback of 0. Its VISUAL EFFECT on STRAY's content is unverified - hence the
		// wording of the tooltip. Stored as a u32 because the parameter block converts between the
		// numeric Set/Get overloads (ngx_interop.hpp), so a u32 Set is readable by the snippet's
		// int Get.
		bool on = l.ui_correction.load(std::memory_order_relaxed) != 0u;
		if (ImGui::Checkbox("NR UI Correction", &on))
		{
			l.ui_correction.store(on ? 1u : 0u, std::memory_order_relaxed);
			bump(k_reset);
		}
		ImGui::SetItemTooltip(
			"Live. DLSSNR.UICorrection is a genuine parameter of THIS snippet build - it is in its "
			"string table and it is read with a failure guard, defaulting to 0. What it looks like on "
			"STRAY has NOT been verified, so treat it as a diagnostic knob rather than a tuning one. "
			"It is nothing to do with this settings panel, despite the name.");
	}

	// ---- colour transfer ---------------------------------------------------------------------
	// renodx's own section heading, kept because it still describes exactly what these do.
	ImGui::SeparatorText("Control-compatible color transfer");

	{
		const bool codec_live = s.codec_running.load(std::memory_order_relaxed);
		if (!f.hdr_codec_at_load)
			overlay_imgui::textf_colored(col::amber,
				"hdr_codec=0 in the ini: these three do nothing this session.");
		else if (!codec_live)
			overlay_imgui::textf_colored(col::amber,
				"The codec is not running at the moment, so these three are not reaching the image.");

		// Greyed when the codec cannot run at all, not merely when it is not running THIS frame:
		// all three are consumed only by the encode and the decode, so with the codec latched off
		// they reach nothing. renodx leaves its equivalents interactive in the same situation.
		ImGui::BeginDisabled(!f.hdr_codec_at_load || s.codec_failed.load(std::memory_order_relaxed));

		slider_f("Scene Paper-White Scale", l.paper_white_scale, 0.05f, 16.0f, "%.3f", k_plain,
			"Live. THIS VALUE IS UNCALIBRATED - Remix folds its own auto-exposure and EV bias into it "
			"and STRAY exposes no equivalent, so it is a plain constant that needs tuning on hardware. "
			"It is a DIVISOR: s = 1 / max(value, 0.01), so RAISING it DARKENS the proxy the network is "
			"shown. Raise it if the proxy looks blown out, lower it if it looks black. One value feeds "
			"BOTH the encode and the decode; the snapshot taken at the top of each pass is what "
			"guarantees they cannot disagree mid-frame, which would be a correctness failure rather "
			"than a tuning difference.");

		slider_f("HDR Transfer Strength", l.transfer_strength, 0.0f, 1.0f, "%.2f", k_plain,
			"Live. 0.0 is an EXACT bypass of the denoise: result = lerp(original, graded, 0) = "
			"original, bit for bit - so a run at 0.0 must be pixel-identical to copy_back=0 and to the "
			"add-on being UNLOADED, while still exercising encode, evaluate, decode, state restore and "
			"copy-back end to end. It is NOT the same as hdr_codec=0, which is a different image "
			"entirely (that binds the raw linear TAA output and copies the network's raw answer "
			"straight back). This is the cheapest A/B that proves the whole path is wired up.");

		slider_f("Color Strength", l.color_strength, 0.0f, 1.0f, "%.2f", k_plain,
			"Live. 0.0 keeps the original's chromaticity exactly and transfers only the network's "
			"luminance change; 1.0 takes the network's colour too. Lower it if the image picks up a "
			"colour cast.");

		ImGui::EndDisabled();
	}

	// ---- guides ------------------------------------------------------------------------------
	ImGui::SeparatorText("Guide overrides (leave at defaults unless diagnostics require them)");

	{
		// renodx's combo has THREE items; ours has two, and the missing one is not an oversight.
		// Its item 0, "Use game NGX flag", means "take DepthInverted from the flags the GAME passed
		// to its own DLSS CreateFeature". This add-on hooks a COMPUTE DISPATCH, not NGX, so it never
		// sees those flags and has nothing to read.
		static const char *const depth_items[] = { "Force normal depth", "Force inverted depth" };
		int v = l.depth_inverted.load(std::memory_order_relaxed) ? 1 : 0;
		if (ImGui::Combo("Depth Convention", &v, depth_items, 2))
		{
			l.depth_inverted.store(v != 0, std::memory_order_relaxed);
			bump(k_reset);   // the accumulated temporal history was built on the other convention
		}
		ImGui::SetItemTooltip(
			"Live, and it forces one DLSSNR.Reset frame because the accumulated history was built "
			"under the other convention. UE 4.27 renders with a REVERSED-Z depth buffer (near plane "
			"at 1.0), so \"Force inverted depth\" is the default here - the opposite of the working "
			"Remix deployment, whose renderer writes post-divide NDC depth without inverting. If "
			"DLSS-NR ghosts or smears in exactly the wrong direction, this is the first thing to "
			"flip.\n\n"
			"The reference add-on offers a third option, \"Use game NGX flag\". It is absent here "
			"because this add-on hooks a compute dispatch rather than NGX and never sees the game's "
			"DLSS create flags, so there would be nothing for it to read.");
	}

	{
		// OUR motion scale is NOT renodx's. Theirs is a MULTIPLIER on a derived base, default 1.0,
		// range -2..2. Ours is an OVERRIDE with 0 meaning "derive from the extents". Dropping
		// renodx's range onto our key would put our auto sentinel exactly at the slider's midpoint,
		// so a user dragging past centre would silently flip to auto. Instead the sentinel is not
		// reachable by dragging at all: it lives behind this checkbox, and the sliders start above
		// zero.
		const float mx = l.mvec_scale_x.load(std::memory_order_relaxed);
		const float my = l.mvec_scale_y.load(std::memory_order_relaxed);
		bool automatic = (mx == 0.0f && my == 0.0f);
		if (ImGui::Checkbox("Motion Scale: derive from the extents (auto)", &automatic))
		{
			if (automatic)
			{
				l.mvec_scale_x.store(0.0f, std::memory_order_relaxed);
				l.mvec_scale_y.store(0.0f, std::memory_order_relaxed);
			}
			else
			{
				// Seed the manual sliders with whatever auto most recently computed, so unticking
				// this does not jump the image.
				const float ax = s.auto_scale_x.load(std::memory_order_relaxed);
				const float ay = s.auto_scale_y.load(std::memory_order_relaxed);
				l.mvec_scale_x.store(ax > 0.05f ? ax : 1.0f, std::memory_order_relaxed);
				l.mvec_scale_y.store(ay > 0.05f ? ay : 1.0f, std::memory_order_relaxed);
			}
			bump(k_reset);
		}
		ImGui::SetItemTooltip(
			"0 in the ini means \"derive from the extents\" - the colour grid divided by the motion "
			"vector grid, which is what the working deployment computes. The reference add-on's "
			"equivalent is a MULTIPLIER over that base with a -2..2 range; ours is an OVERRIDE, so "
			"copying that range would put our auto sentinel dead centre and a drag past the middle "
			"would silently switch to auto. It lives here instead, where it cannot be reached by "
			"accident.\n\n"
			"Neither form can correct UE4's velocity ENCODING - only its grid. See the README.");

		overlay_imgui::textf_colored(col::dim, "    auto is currently computing %.4f / %.4f",
			(double)s.auto_scale_x.load(std::memory_order_relaxed),
			(double)s.auto_scale_y.load(std::memory_order_relaxed));

		ImGui::BeginDisabled(automatic);
		slider_f("Motion Scale X", l.mvec_scale_x, 0.05f, 4.0f, "%.3f", k_reset,
			"Live, and it forces one reset frame. Overrides the derived colour/mvec grid ratio.");
		slider_f("Motion Scale Y", l.mvec_scale_y, 0.05f, 4.0f, "%.3f", k_reset,
			"Live, and it forces one reset frame. Overrides the derived colour/mvec grid ratio.");
		ImGui::EndDisabled();
	}

	// ---- pipeline ----------------------------------------------------------------------------
	ImGui::SeparatorText("Pipeline");

	checkbox_b("Copy the result back over the frame", l.copy_back, k_flush,
		"Live. Off runs the WHOLE path - barriers, encode, evaluate, decode, state restore - and "
		"writes to a texture nothing reads, so a frame that still renders correctly is positive "
		"evidence that the state restore is faithful, independently of image quality. That is the "
		"bring-up A/B this project was built around. Toggling it drops any armed pristine copy, "
		"because the resource address it names can be recycled by UE 4.27's render-target pool.");

	ImGui::BeginDisabled(!l.copy_back.load(std::memory_order_relaxed));
	checkbox_b("History restore (break the temporal feedback loop)", l.history_restore, k_flush,
		"Live. UE 4.27 extracts the TAA compute pass's output as the NEXT frame's history, so "
		"without this the denoised image re-enters the game's own accumulator at a 0.96 per-frame "
		"weight and compounds roughly 25-fold. With it on, the add-on keeps a private copy of the "
		"PRE-denoise output and writes it back over that resource at the start of the next accepted "
		"dispatch, after verifying the resource really is bound as a colour SRV there. Inert when "
		"the copy-back is off, which is why it greys out.");
	ImGui::EndDisabled();

	checkbox_b("Restore the graphics root signature too", l.restore_graphics_root, k_plain,
		"Live, and safe to change mid-scene ONLY because of the snapshot at the top of each pass. "
		"This value is read TWICE per pass - once by capture_state and once by restore_state - and "
		"a true-to-false tear between them would leave the descriptor heaps re-bound, the compute "
		"root signature set (which invalidates every graphics root argument) and the graphics tables "
		"never replayed: exactly the corruption this defaulting ON exists to prevent. NVIDIA's own "
		"Streamline shadow does not track graphics root state, but a descriptor-heap change "
		"invalidates graphics tables too, so leave it on unless you are measuring.");

	ImGui::EndDisabled();   // !usable

	// ---- persistence -------------------------------------------------------------------------
	ImGui::Separator();
	{
		static std::string s_err;
		static uint64_t    s_err_until = 0;
		const bool is_dirty = dirty();

		ImGui::BeginDisabled(!is_dirty);
		if (ImGui::Button("Save to stray_dlssnr.ini"))
		{
			std::string err;
			if (save_ini(err))
			{
				s_err.clear();
				s_err_until = 0;
			}
			else
			{
				s_err = err;
				s_err_until = static_cast<uint64_t>(GetTickCount64()) + 15000u;
				logf(reshade::log::level::error, "DLSS-NR overlay: could not save stray_dlssnr.ini - %s", err.c_str());
			}
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip(
			"Rewrites stray_dlssnr.ini IN PLACE: every comment, every blank line, the key order and "
			"your own spelling all survive, and only the values of the settings this panel owns are "
			"replaced. Written to a temp file and renamed, because a half-written ini is worse than "
			"none - every key after the cut would silently take its built-in default.\n\n"
			"The identification pins (shader_hash, srv_*, uav_output, app_id) and the load-only keys "
			"are NEVER round-tripped, so a hand-measured config cannot be clobbered from here.\n\n"
			"Saving is a file write on the present thread. It happens only when you press this, never "
			"during a drag.");

		ImGui::SameLine();
		if (is_dirty)
			overlay_imgui::textf_colored(col::amber, "unsaved changes - they are LIVE now but will be lost on restart");
		else
			overlay_imgui::textf_colored(col::dim, "in sync with stray_dlssnr.ini");

		if (!s_err.empty() && static_cast<uint64_t>(GetTickCount64()) < s_err_until)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, col::red);
			ImGui::TextWrapped("save failed: %s", s_err.c_str());
			ImGui::PopStyleColor(1);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// The settings that exist but cannot be changed while the game is running. Shown GREYED rather
// than hidden: a user must be able to read what the ini said without alt-tabbing, and must be able
// to see WHY each one is not a control. Every reason here is a specific line of the add-on, not a
// general caution.
// ---------------------------------------------------------------------------------------------
inline void draw_load_only(const host_facts &f)
{
	if (!ImGui::CollapsingHeader("Load-only settings (edit stray_dlssnr.ini and restart)"))
		return;

	char buf[128];

	ImGui::TextWrapped("These are read once, at load. Nothing in this panel can change them, and a "
	                   "control that looked like it could would be worse than none.");
	ImGui::Spacing();

	load_only("enabled", f.enabled_at_load ? "1" : "0",
		"read once in nr_init_device and never again; nothing on the render path consults it. The "
		"master switch above is a separate per-dispatch bypass, which is why it works and this "
		"would not.");

	load_only("hdr_codec", f.hdr_codec_at_load ? "1" : "0",
		"cannot be flipped in place in EITHER direction. Off-to-on is impossible - hdr_codec=0 at "
		"load latches codec_failed, which is deliberately never cleared - and on-to-off would "
		"silently kill the copy-back, because out_tex was created r16g16b16a16_float for the codec "
		"and keeps that format for its lifetime, so it would no longer match the frame it is copied "
		"into. Use HDR Transfer Strength = 0 for a live, exact bypass of the denoise instead.");

	std::snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)f.shader_hash);
	load_only("shader_hash", buf,
		"memoized per pipeline state object: the lookup is guarded by nr_checked != pso and cached "
		"in nr_is_target. Changing it at runtime would take effect on some command lists and not "
		"others - non-deterministic, which is the one thing worse than not editable.");

	std::snprintf(buf, sizeof(buf), "t%u / t%u / t%u -> u%u",
		(unsigned)f.srv_depth, (unsigned)f.srv_velocity, (unsigned)f.srv_colour, (unsigned)f.uav_output);
	load_only("srv_depth / srv_velocity / srv_colour / uav_output", buf,
		"identification pins. srv_colour is ALSO the history-restore refusal test, so changing it "
		"between the arm and the consume could land last frame's image on this frame's scene-colour "
		"input - the frozen, ghosted frame the restore path refuses by name.");

	load_only("diagnostics", f.diagnostics ? "1" : "0",
		"read on the draw and dispatch paths from threads this overlay must not race with, for the "
		"sake of a log knob. It never touches the render path either way.");

	load_only("require_trampoline", f.require_trampoline ? "1" : "0",
		"consumed once, at the LoadLibrary of the snippet.");
	load_only("populate_parameters", f.populate_parameters ? "1" : "0",
		"consumed once, right after the parameter block is allocated.");

	std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)f.app_id);
	load_only("app_id", buf, "consumed once, at NVSDK_NGX_D3D12_Init_Ext.");

	load_only("stray_dlssnr.ini", f.ini_found ? "found" : "NOT FOUND (every setting is at its built-in default)",
		"a missing file is not an error - every default in addon_config.hpp is the shipping default, "
		"so the add-on behaves identically with and without it.");
}

// ---------------------------------------------------------------------------------------------
// The reference add-on's controls we deliberately do not have, and why. Kept in the UI rather than
// only in a comment so that nobody - including a future version of this add-on - re-adds one
// because it "should" be there. A control that does nothing is worse than a missing control:
// it makes the user believe they have tested something they have not.
// ---------------------------------------------------------------------------------------------
inline void draw_absent()
{
	if (!ImGui::CollapsingHeader("Controls the reference add-on has that this one does not"))
		return;

	ImGui::BeginDisabled(true);
	{
		static bool dummy_upscale = false;
		static int  dummy_preset = 0;
		static const char *const preset_items[] = { "Preset 1 (the only network in this build)" };
		ImGui::Checkbox("Enable Upscaling", &dummy_upscale);
		ImGui::Combo("NR Preset", &dummy_preset, preset_items, 1);
	}
	ImGui::EndDisabled();

	ImGui::Spacing();
	ImGui::TextWrapped(
		"Enable Upscaling - ABSENT. DLSSNR.Upscaling does not exist anywhere in this snippet's "
		"string table (measured: zero exact matches, while DLSSNR.UICorrection, .Style, .Intensity "
		"and .ScalingRatio each return one), and DLSSNR.ScalingRatio is dead code - three sites read "
		"it and then unconditionally store 1.0f over the result, with no failure guard, so it can "
		"never change anything. This add-on does not upscale.");
	ImGui::Spacing();
	ImGui::TextWrapped(
		"NR Preset - INERT. It maps to DLSSNR.Hint.Render.Preset, which is written at CreateFeature "
		"rather than per evaluate, and only preset 1 exists in this snippet build: anything else "
		"logs \"preset %%d is not available in this DLL build\" and loads the same weights anyway. "
		"Note it is a DIFFERENT parameter from NR Style, which IS live and real.");
	ImGui::Spacing();
	ImGui::TextWrapped(
		"Use game NGX flag - NEVER A CONTROL. In the reference binary it is item 0 of the "
		"three-item Depth Convention combo, and it means \"take DepthInverted from the flags the "
		"game passed to its own DLSS CreateFeature\". This add-on hooks a compute dispatch, not NGX, "
		"so it never sees those flags.");
	ImGui::Spacing();
	ImGui::TextWrapped(
		"NGX core - NEVER A CONTROL EITHER. In the reference it is a backend NAME substituted into a "
		"status line (\"NGX core\" versus \"signed runtime\"). The status block above reports the "
		"equivalent facts directly.");
	ImGui::Spacing();
	ImGui::TextWrapped(
		"Also not copied: the reference's \"ratio %%.2f\" status line, which is fed a hardcoded 0.5 "
		"from its constant pool rather than a measured value even though it has real extents on "
		"hand one line above. A status line that displays a constant is worse than no status line.");
}

// =============================================================================================
// THE ENTRY POINT
//
// Registered with title = nullptr, i.e. as the add-on's SETTINGS overlay, so ReShade draws it in
// the Add-ons tab immediately under the same checkbox that would disable the add-on. That is
// deliberate: the failure this panel exists to prevent is the add-on not loading at all, and the
// best available signal for that is this panel's ABSENCE from exactly where the user is looking.
//
// NOTHING MAY ESCAPE INTO ReShade. Same contract as every other callback in this add-on: an
// exception crossing back into the host terminates the process, and if this add-on destabilises
// STRAY we lose the ability to test anything at all.
// =============================================================================================
inline void draw(reshade::api::effect_runtime *runtime)
{
	try
	{
		// bind_table() must have succeeded. If ReShade could not supply an ImGui function table we
		// run headless rather than dereferencing a null table pointer inside the present path.
		if (!overlay_imgui::available())
			return;

		const host_facts f = read_facts();

		draw_status(runtime, f);
		ImGui::Spacing();
		draw_controls(f);
		ImGui::Spacing();
		draw_load_only(f);
		draw_absent();
	}
	catch (const std::exception &e)
	{
		OVERLAY_LOG_ONCE(reshade::log::level::error,
			"exception in the DLSS-NR overlay: %s. The overlay keeps drawing; the render path is "
			"unaffected. This message is printed once.", e.what());
	}
	catch (...)
	{
		OVERLAY_LOG_ONCE(reshade::log::level::error,
			"unknown exception in the DLSS-NR overlay. The overlay keeps drawing; the render path is "
			"unaffected. This message is printed once.");
	}
}

/// Bind the ImGui table and register the overlay. Returns false when this ReShade build cannot
/// supply an ImGui function table, in which case the add-on runs headless - DLSS-NR is unaffected.
/// Never fatal: this project has already lost a play session to an add-on that silently did not
/// load, and failing to get the table must cost the user the overlay, never the denoise.
inline bool install()
{
	if (!overlay_imgui::bind_table(reshade::internal::get_reshade_module_handle()))
	{
		logf(reshade::log::level::info,
			"DLSS-NR: no ImGui function table from this ReShade build - running headless. The "
			"denoise is unaffected; there will simply be no settings panel.");
		return false;
	}
	// nullptr = the add-on's own settings page, NOT a floating window. See the header comment.
	reshade::register_overlay(nullptr, &draw);
	logf(reshade::log::level::info,
		"DLSS-NR: overlay registered on the add-on's settings page (ImGui %s, table %u).",
		IMGUI_VERSION, (unsigned)IMGUI_VERSION_NUM);
	return true;
}

} // namespace overlay_ui
