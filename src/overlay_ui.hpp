// overlay_ui.hpp - the ReShade overlay for the STRAY DLSS-NR add-on.
//
// SELF-CONTAINED BY DESIGN. Everything that can live here does; what is left in
// src/stray_dlssnr.cpp is a set of small hooks, each wrapped in
// `// ---- BEGIN overlay_ui hook ----` / `// ---- END overlay_ui hook ----`:
//   the #include
//   read_ident + the per-PSO memo - the ONE new render-path touch the ladder needed, and the only
//     way shader_hash can be live at all (its read site runs before st->mutex and before
//     begin_pass, so no snapshot can reach it)
//   begin_pass - the per-pass snapshot, the rebuild gate and rungs R0-R3
//   the DLSSNR.UICorrection write (see "NR UI Correction" below)
//   publish_evaluate - the status block
//   seed_from_config - the parsed ini into the overlay atomics
//   the two census lines' gates and their live values
//   on_draw / on_draw_indexed / on_dispatch - live_diagnostics() at each of the three sites
//   DllMain: install the status hook and the load-only host_facts snapshot
// Plus nr_service_reconfigure, which is not a hook but the other half of this file's design: the
// present-thread half of the ladder, in stray_dlssnr.cpp because the CI gate below forbids this
// header from taking a lock or touching nr_state. Two more hooks live in src/addon_config.hpp and
// one in src/ngx_interop.hpp.
//
// LINE NUMBERS ARE DELIBERATELY NOT GIVEN FOR THE HOOK LIST. They were, and every one of them was
// wrong by the time this ladder landed - a hook list that names stale lines is worse than one
// that names none, because it sends the reader to the wrong place with confidence. Grep for the
// marker; it is there for exactly this.
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
//    stray_dlssnr.cpp:2201 takes st->mutex and then g.mutex at :2225, and on_present reads the
//    DLSS-NR counters as ATOMICS specifically to avoid inverting that order (the comment at
//    :1513-1516 spells out the AB/BA deadlock). The overlay callback runs on the present thread.
//    A lock here would join that ordering graph and the whole argument would have to be
//    re-derived. Worse, nr_try_run holds st->mutex across CreateFeature (which uploads the
//    network weights) and EvaluateFeature, so blocking Present behind it is a visible hitch.
//    Every value crossing the boundary is therefore a lock-free atomic, checked below.
//
// 2. THE OVERLAY NEVER TOUCHES nr_state, AND NEVER CALLS ngx::set_*.
//    ngx::store mutates a std::unordered_map with a mutex the snippet also calls into
//    (ngx_interop.hpp:243,:266); pushing a slider value straight at NGX from the present thread
//    is heap corruption. All parameter writes stay where they are, on the render thread.
//    Instead the overlay writes ATOMICS, and ONE hook - begin_pass() - copies them into g_cfg
//    once per pass, on the render thread, under the lock that pass already holds.
//
// 3. ONE SNAPSHOT PER PASS, WHICH IS A CORRECTNESS REQUIREMENT AND NOT TIDINESS.
//    Several settings are read MORE THAN ONCE inside a single pass, and the two reads must agree:
//
//      restore_graphics_root   :2333 (capture_state) and :3033 (restore_state). A true->false
//                              tear between them is CORRUPTING: d3d12_state.hpp:429-435 absorbs
//                              the graphics heap only if(restore_graphics), :569-576 re-binds the
//                              heaps UNCONDITIONALLY, replay_pipe_compute issues
//                              SetComputeRootSignature (:507) which per that file's note at :243
//                              invalidates ALL root arguments including graphics, and :583-584
//                              then skips the graphics replay. That is precisely the corruption
//                              this knob's default-ON exists to prevent. (false->true is benign:
//                              plan.gfx.root_signature is null and :534-535 early-returns.)
//      paper_white_scale       read TWICE inside one expression at :2646, feeding ea.proxy_scale
//                              (:2747) AND da.proxy_scale (:2967). :2638-2640 calls a mismatch
//                              between them "a correctness failure, not a tuning difference".
//      transfer/color_strength read up to 3x each in their clamp expressions, :2647-2650.
//      mvec_scale_x/y          read twice each, :2805-2808 (a !=0.0f test, then the value).
//      copy_back               SIX sites in one pass: :1663 :2499 :2636 :2882 :3050 :3069.
//      history_restore         SIX as well: :1663 :2499 :2636 :2883 :3093 :3127.
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
// THE RECONFIGURE LADDER - HOW EVERY SETTING IS MADE LIVE, AT THE RIGHT GRANULARITY
//
// An earlier version of this file listed ten keys as LOAD-ONLY and greyed them out. That
// allowance is withdrawn: eight of those ten are reachable, and this is the mechanism. Six rungs,
// strictly nesting - each implies every rung below it - and they generalise the three epochs this
// file already had rather than adding a parallel scheme.
//
//   R0  SNAPSHOT   begin_pass copies these atomics into g_cfg once per pass, on the render
//                  thread, under the lock the pass already holds. Rule 3 below is why.
//   R1  RESET      + st->need_reset: one DLSSNR.Reset frame, because the accumulated temporal
//                  history was built under the other geometry.        bump(k_reset)
//   R2  FLUSH      + st->pending_res = 0. The armed pristine copy names a RAW ID3D12Resource
//                  address held across a frame and UE 4.27's pool can recycle it.  bump(k_flush)
//   R3  IDENT      + a bump of ident_epoch, which (a) invalidates the per-PSO memo on EVERY
//                  command list at once, (b) re-arms the identification one-shot log latches, and
//                  (c) clears the copied_into ring.                    bump(k_ident)
//   R4  REBUILD    + a deferred teardown through the EXISTING seam - nr_state::pending_work's
//                  kTeardown bit, serviced on the next present by nr_service_reconfigure, which
//                  calls nr_release_feature_and_output. The feature, every view and every texture
//                  go; the next accepted dispatch rebuilds them and RE-DECIDES out_tex's format
//                  against the new hdr_codec.                          bump(k_rebuild)
//   R5  REARM      + an explicit action mask, OR-merged into the same pending_work word and
//                  serviced in the same present-thread pass: build the codec or mvec pipelines,
//                  rebuild the NGX parameter block, load the snippet, clear the clip latches.
//                                                                      request(a_*)
//
// WHY THE SPLIT BETWEEN THIS FILE AND stray_dlssnr.cpp IS NOT STYLISTIC. The CI gate at
// .github/workflows/build.yml:421-448 bans std::mutex / std::lock_guard / ngx::set_ / ngx::store /
// pd_get / kNrStateGuid from this header. Values and epochs live HERE, as lock-free atomics; every
// ACTION lives in stray_dlssnr.cpp, on a thread allowed to take st->mutex. That is the invariant
// that keeps this file lock-free on the present thread, and the ladder is built to respect it.
//
// WHERE THE RUNGS ARE CONSUMED, AND WHY THERE ARE TWO CONSUMERS. begin_pass consumes R0-R3 on the
// RENDER thread (they are per-pass consequences). nr_service_reconfigure consumes R3-R5 on the
// PRESENT thread (they idle the queue, destroy resources and LoadLibrary). Each keeps its own
// seen-epoch state, both of them in nr_state - NOT in a function-local static, which would be
// process-wide rather than per-device and would silently cost a second D3D12 device its whole
// rebuild. The service reading the epochs FOR ITSELF is load-bearing rather than tidy: with
// enabled=0, or with the feature wedged, begin_pass never runs at all, and those are exactly the
// cases where the user most needs a reconfigure to land.
//
//   PER KEY (62 keys; the render-path read site for each is cited in stray_dlssnr.cpp):
//     R0        intensity, local_tone_strength, local_structure_strength, skin_structure_strength,
//               style, use_auto_mask, ui_correction, paper_white_scale, transfer_strength,
//               color_strength, restore_graphics_root, hdr_graft,
//               sr_direct_output, sr_copy_back
//               hdr_graft is R0 and nothing more because it is a ROOT CONSTANT in the decode's own
//               constant block - it replaced a pad word, so the constant count, the root signature,
//               the pipeline layout and both PSOs are unchanged and there is nothing to rebuild
//               when it moves. Its two read sites - nr_codec_decode on the NR-alone path and on the
//               chained one - are both downstream of the snapshot.
//     R1        sr_use_view_rect, sr_jitter_scale_x, sr_jitter_scale_y,
//               sr_jitter_projection_only, sr_mv_scale_x, sr_mv_scale_y
//     R1+auto   sr_out_width, sr_out_height, sr_group_tile. These three feed want_out_w/h, and
//               sr_try_run compares that against sr_seen_out_w/h on EVERY pass - so a change makes
//               key_moved true and queues kTeardown through the same seam the recreate button uses,
//               with no action bit from the control at all. Raising one here as well would cost a
//               teardown per pixel of slider drag for no extra effect.
//     CREATE    sr_hdr, sr_mv_lowres, sr_mv_jittered, sr_depth_inverted, sr_auto_exposure,
//               sr_alpha_upscaling, sr_hw_depth, sr_perf_quality, sr_render_preset. Snapshotted for
//               COHERENCE - the create-params block reads six of them in one expression - but
//               latched at CreateFeature with no evaluate-time equivalent, and nothing in the
//               render path compares them against what the live feature was created with. The
//               "Recreate the SR feature" button is the mechanism and every tooltip says so.
//     R4+R5     dlss_chain - a_teardown | a_reconcile. 1 -> 0 is fully live; 0 -> 1 needs the SR
//               snippet, which is a launch-time LoadLibraryW, on exactly the terms dlss_sr states.
//               It has TWO read sites and therefore two mechanisms: chain_ok in nr_try_run is
//               downstream of begin_pass and rides the snapshot, while want_hash runs BEFORE it and
//               is served by read_ident() - chain only upscales in the MainUpsampling permutation,
//               so it re-pins the hash exactly as dlss_sr does.
//     ARM-ONLY  sr_optimal_settings, dlss_nr. Owned, saved and reverted; read once each in
//               nr_lazy_ngx_init / nr_init_device, so there is no second read a live value could
//               reach. Both controls say exactly that.
//     R1        depth_inverted, mvec_scale_x, mvec_scale_y, mvec_dilate, mvec_reconstruct
//     R1+latch  mvec_clip_row, mvec_clip_transpose - these two can PERMANENTLY latch
//               view_layout_failed / clip_ok=false (stray_dlssnr.cpp:2589-2593, :2604-2612,
//               :2640-2645), cleared only by nr_release_feature_and_output. Without a_clear_clip
//               the knob is dead after the first bad value: a control that lies.
//     R2        copy_back, history_restore, and the master bypass
//     R3        shader_hash, srv_depth, srv_velocity, srv_colour, uav_output
//     R4        hdr_codec (BOTH directions), mvec_decode off, enabled off
//     R4+R5     mvec_decode on, enabled on - both a_teardown | a_reconcile; the service works out
//               what actually needs building from st->codec.ok / st->mvec.ok / g_snippet.available
//     R4+R5     populate_parameters - a_teardown | a_apply_populate, raised by its Apply button and
//               by Revert and by NOTHING ELSE. It is the one action with a dedicated bit rather
//               than a reconcile, because it is the one whose work cannot be derived from "make
//               reality match the live values": the user's decision to APPLY it is itself part of
//               the input. See the a_apply_populate declaration.
//     ATOMIC AT ITS OWN SITE, never through the g_cfg snapshot:
//               shader_hash  - read at :2722, BEFORE st->mutex (:2744) and BEFORE begin_pass
//                              (:2760). A snapshot cannot reach it, so it goes through
//                              read_ident() at its own site. Snapshotting it would have shipped a
//                              control that does nothing on the one path it names - the exact
//                              shape the kParam CI gate exists to catch.
//               diagnostics  - read at :4425 / :4430 / :4449, i.e. on EVERY draw and EVERY
//                              dispatch in the process, on arbitrary recording threads, outside
//                              the accepted-pass snapshot. live_diagnostics() at each site.
//               mvec_decode / mvec_reconstruct
//                            - ALSO read on the MAIN thread by the census line at :4564 / :4572.
//                              live_mvec_decode() / live_mvec_reconstruct(), for the same reason
//                              live_copy_back() / live_history_restore() already exist.
//               rt_census / rt_census_frames
//                            - live in rt_census's own atomics; the service stores them there.
//
//   RELAUNCH-ONLY, AND ONLY THESE TWO. Both say so in their own tooltip, with the evidence:
//     require_trampoline 0->1  Honouring it means UNLOADING an already-initialised snippet, and
//                              this tree has no in-process unload path: stray_dlssnr.cpp:4339-4341
//                              declines to FreeLibrary even at device teardown ("a 166 MB module
//                              that may still hold worker threads"). 1->0 IS live - on that path
//                              load_snippet already called s.unload() (ngx_interop.hpp:686), so
//                              nothing is loaded and re-running it is clean.
//     app_id                   The MECHANISM exists (Shutdown1 is resolved and is already called
//                              in-process at :4328-4333, so Shutdown1 + Init_Ext is one more
//                              action bit). What is NOT proven is that Init_Ext survives a SECOND
//                              call, and the tree's only measurement of its fragility is that it
//                              HANGS when called at the wrong moment (:4049-4056: "the log stops
//                              between loaded nvngx_dlssnr.dll and the Init_Ext result, the
//                              process sits at ~2% CPU, and the title never reaches its menu"). A
//                              hang is not a failure that degrades. Since app_id has NO
//                              render-path effect whatsoever - addon_config.hpp:265-267 and
//                              stray_dlssnr.cpp:4083-4085 both record that it only names the log
//                              file the snippet writes beside the add-on - shipping an unverified
//                              path that can hang the user's game to rename a log file is the
//                              wrong trade. It is stated as exactly that, and not as "load-only".
//
// =============================================================================================
// FOUR renodx CONTROLS ARE DELIBERATELY ABSENT. "Never add a control that does nothing."
//
//   Enable Upscaling    ABSENT. `DLSSNR.Upscaling` does not exist anywhere in the snippet's
//                       string table (measured: 0 exact matches in nvngx_dlssnr.dll, while
//                       DLSSNR.UICorrection / .Style / .Intensity / .ScalingRatio each return 1),
//                       and ScalingRatio is dead - ngx_interop.hpp:180-183 and
//                       stray_dlssnr.cpp:1910-1911 both record that three sites read it and then
//                       unconditionally store 1.0f over the result. This add-on does not upscale.
//   NR Preset           SHOWN, DISABLED. It maps to DLSSNR.Hint.Render.Preset, which is written
//                       at CreateFeature (:1909), and :1907-1908 records that only preset 1
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
//                       IS in this snippet's string table (measured: exactly one exact-line match
//                       in nvngx_dlssnr.dll, against ZERO for `DLSSNR.Upscaling`), and it is read
//                       as Get(const char*, int*) with a proper 0xbad00000 guard and a fallback
//                       of 0. AND IT IS ACTUALLY SENT: stray_dlssnr.cpp:2834 writes it into the
//                       parameter block on every evaluate, in its own overlay_ui hook next to
//                       DLSSNR.Style. That write is the difference between this control and a
//                       control that does nothing, and it is the one thing that was missing when
//                       this checkbox first shipped - every other layer (ini key, parser, live
//                       atomic, snapshot, ini writer) was in place, so nothing looked wrong. A CI
//                       step now fails the build if any declared kParam* has no write site.
//                       Its visual effect on STRAY's content is UNVERIFIED, and the UI says so.
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

	// The literal is the DERIVED default - see cfg::paper_white_scale and hdr_codec.hpp's
	// "THE SCALE, s" section. It is only a pre-seed: seed_from_config overwrites it from the
	// parsed ini before any dispatch or any draw. Kept in step anyway, so a grep for
	// `4.0f / 3.0f` finds all three copies of this number.
	std::atomic<float>    paper_white_scale{ 4.0f / 3.0f };
	std::atomic<float>    transfer_strength{ 1.0f };
	std::atomic<float>    color_strength{ 1.0f };
	// cfg::hdr_graft. Live at tier 0: it is a root constant in the decode's own constant block,
	// snapshotted with the rest of the pass, so nothing is rebuilt when it changes.
	std::atomic<uint32_t> hdr_graft{ 0 };

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

	// ---- R3: IDENTIFICATION. shader_hash is read by the render path through read_ident(), NOT
	// through the g_cfg snapshot: its one functional read site (stray_dlssnr.cpp:2722) executes
	// BEFORE st->mutex is taken (:2744) and BEFORE begin_pass (:2760), so a snapshot value can
	// never reach it. The four register pins DO ride the snapshot - every read of them is after
	// begin_pass (:2833-2835, :2270) - and need only the arm drop and the latch re-arm that the
	// ident rung already carries. Saying they need the memo too would be wrong.
	std::atomic<uint64_t> shader_hash{ 0 };
	std::atomic<uint32_t> srv_depth{ 0 };
	std::atomic<uint32_t> srv_velocity{ 2 };
	std::atomic<uint32_t> srv_colour{ 5 };
	std::atomic<uint32_t> uav_output{ 0 };

	// ---- R4/R5: the keys that need a feature recreate or a pipeline build.
	std::atomic<bool>     hdr_codec{ true };
	std::atomic<bool>     mvec_decode{ true };
	std::atomic<bool>     populate_parameters{ false };
	std::atomic<bool>     require_trampoline{ true };
	// The ini's master switch, and it is NOT the same control as `bypass` above. Both are real:
	// `bypass` is the per-dispatch no-op, this one is whether the 166 MB snippet is loaded and
	// NGX is armed at all. Off gives the VRAM back; on runs the shipping startup path.
	std::atomic<bool>     enabled{ true };

	// ---- DLSS SUPER RESOLUTION. WHAT IS LIVE AND WHAT IS NOT, SAID HERE RATHER THAN ONLY IN A
	// TOOLTIP.
	//
	// THE BRANCH IS LIVE, BOTH WAYS. The one decision dlss_sr makes on the render path -
	// `if (g_cfg.dlss_sr) { sr_try_run(...); return; }` - is taken AFTER begin_pass has run, as is
	// every g_cfg.sr_* read inside sr_try_run and the output-extent derivation above it. So dlss_sr
	// rides the per-pass snapshot exactly like copy_back does: unticking it hands the accepted
	// dispatch back to DLSS-NR on the very next frame, and ticking it sends the dispatch to
	// sr_try_run on the very next frame.
	//
	// THE ARM IS NOT LIVE, AND THE CONTROL SAYS SO IN AS MANY WORDS. Reaching sr_try_run is not the
	// same as DLSS-SR running. Arming it means a 59 MB LoadLibraryW of nvngx_dlss.dll claiming the
	// trampoline's SLOT B, and then NVSDK_NGX_D3D12_Init_Ext through that slot from a render thread
	// with a fully built device - which happens exactly once per process, inside nr_lazy_ngx_init,
	// on the first accepted dispatch. With dlss_sr=0 in the ini at launch that call was never made,
	// g_sr_armed is false, and sr_try_run bails on its own second line with "not armed" in the log.
	// A second Init_Ext later in the session is precisely the unverified action this tree refuses
	// elsewhere (see require_trampoline and app_id in draw_load_only), and the one measured fact
	// about Init_Ext's fragility is that it can HANG. So the ON direction is reported as
	// RELAUNCH REQUIRED by the service, with the reason, instead of being claimed and not done.
	// The OFF direction is fully live, and it is the direction a user actually reaches for.
	std::atomic<bool>     dlss_sr{ false };
	// LAUNCH-TIME BY CONSTRUCTION, and owned anyway. Its only two read sites are the DLSS-NR snippet
	// load in nr_init_device and the Init_Ext gate in nr_lazy_ngx_init, both of which have already
	// happened before any checkbox can be clicked. It is in OVERLAY_OWNED_FIELDS so that Save,
	// Revert and the dirty test all cover it - a setting that applies next launch is honest, a
	// setting the Save button silently drops is not - and it is deliberately ABSENT from the
	// per-pass snapshot below, because writing it into g_cfg would put a value in front of a reader
	// that does not exist.
	std::atomic<bool>     dlss_nr{ true };
	// Read at its own site in the identification block, BEFORE begin_pass, for exactly the reason
	// shader_hash is. read_ident() carries it, and carries dlss_sr with it, because the hash this
	// dispatch must match is a function of all three. See want_hash() below.
	std::atomic<uint64_t> sr_shader_hash{ 0 };
	// Tier 0. Every read site is inside sr_try_run, downstream of the snapshot.
	std::atomic<bool>     sr_suppress_taa{ false };
	std::atomic<bool>     sr_mvec_decode{ true };
	std::atomic<bool>     sr_mvec_reconstruct{ true };
	// Tier 1. Both are latched into the DLSS create-params at CreateFeature and there is no
	// evaluate-time equivalent, so a change needs the SR feature releasing before it can be read
	// again. The controls raise a_teardown for that reason and nothing more: sr_try_run re-creates
	// on the next accepted dispatch by itself, from the values the snapshot has already committed.
	std::atomic<uint32_t> sr_perf_quality{ 0 };
	std::atomic<uint32_t> sr_render_preset{ 0 };

	// ---- dlss_chain. The SECOND network on ONE accepted dispatch: DLSS-NR denoises at the render
	// extent and its result becomes DLSS-SR's colour input. It is snapshotted because its ONE
	// functional read site - chain_ok, where nr_try_run decides whether to enter the chain - is
	// downstream of begin_pass, and it is ALSO in ident_view because want_hash() has to treat it
	// exactly like dlss_sr: chain only upscales in the MainUpsampling permutation, which is a
	// different #define set, therefore different DXBC and a different fnv1a64. Two read sites, two
	// mechanisms, one value - the same shape dlss_sr already has.
	std::atomic<bool>     dlss_chain{ false };

	// ---- DLSS-SR, TIER 0. Every read site is inside sr_try_run or nr_try_run's geometry block,
	// both downstream of begin_pass, so the snapshot is the whole mechanism.
	std::atomic<bool>     sr_copy_back{ true };
	std::atomic<bool>     sr_direct_output{ false };
	std::atomic<bool>     sr_use_view_rect{ true };
	std::atomic<bool>     sr_jitter_projection_only{ false };
	std::atomic<float>    sr_jitter_scale_x{ 1.0f };
	std::atomic<float>    sr_jitter_scale_y{ 1.0f };
	std::atomic<float>    sr_mv_scale_x{ 0.0f };
	std::atomic<float>    sr_mv_scale_y{ 0.0f };

	// ---- DLSS-SR, TIER 1. All seven are latched into the DLSS create-params at CreateFeature -
	// cd.flags and cd.hw_depth - and there is no evaluate-time equivalent, so the snapshot makes
	// the value COHERENT but only a feature release makes it TAKE. Their controls say exactly that
	// and point at the recreate button, which is the same contract sr_perf_quality already ships.
	std::atomic<bool>     sr_hdr{ true };
	std::atomic<bool>     sr_mv_lowres{ true };
	std::atomic<bool>     sr_mv_jittered{ false };
	std::atomic<bool>     sr_depth_inverted{ true };
	std::atomic<bool>     sr_auto_exposure{ true };
	std::atomic<bool>     sr_alpha_upscaling{ false };
	std::atomic<bool>     sr_hw_depth{ true };

	// ---- DLSS-SR, TIER 2. These three decide the OUTPUT EXTENT, which picks the output UAV and
	// then goes into CreateFeature, so a change is a geometry change: their controls raise
	// a_teardown | a_reconcile at k_rebuild rather than asking the user to press anything.
	std::atomic<uint32_t> sr_group_tile{ 8 };
	std::atomic<uint32_t> sr_out_width{ 0 };
	std::atomic<uint32_t> sr_out_height{ 0 };

	// ---- ARM-TIME, and its control says so. sr_optimal_settings has exactly ONE read site, in
	// nr_lazy_ngx_init, which runs once per process; it is a DIAGNOSTIC query that returns a
	// recommended render resolution and has no power to make UE render at it. Owned so that a Save
	// keeps it, wired to no action bit because there is no second read for one to reach.
	std::atomic<bool>     sr_optimal_settings{ false };

	// ---- R1, and two of them also need the clip latches cleared.
	std::atomic<bool>     mvec_reconstruct{ true };
	std::atomic<bool>     mvec_dilate{ false };
	std::atomic<bool>     mvec_clip_transpose{ false };
	std::atomic<uint32_t> mvec_clip_row{ 0 };

	// ---- read at their own sites, on threads the per-pass snapshot does not cover.
	std::atomic<bool>     diagnostics{ true };
	std::atomic<bool>     rt_census{ false };
	std::atomic<uint32_t> rt_census_frames{ 600 };

	// ---- THE WRITE SEQUENCE. THIS, NOT THE EPOCH, IS WHAT MAKES A SNAPSHOT ATOMIC. --------------
	//
	// The epochs below are CONSEQUENCE counters, and an earlier revision of this header claimed
	// they were also the tearing guard: "a user who retypes shader_hash and srv_colour together
	// can never be observed half way, because the reader acquires the epoch before it reads
	// either value." THAT WAS NOT TRUE, and the reason it was not is worth keeping, because it is
	// the same mistake anyone would make twice.
	//
	// Every widget STORES ITS VALUE AND THEN BUMPS. An acquire load of `epoch` orders a reader
	// against a release that has ALREADY HAPPENED; it says nothing about a store whose release is
	// still to come. So a render thread that ran its snapshot between the value store and the
	// bump read the NEW value with the OLD epoch - i.e. applied a change without its consequence.
	// For srv_colour that is precisely the "last frame's image on this frame's scene-colour input"
	// hazard the ident rung exists to prevent; for hdr_codec it is one frame where the copy-back's
	// format guard fires and the frame silently gets no denoise. And revert_to_baseline widens the
	// hole from one store to thirty, because config_to_live writes every field before the single
	// bump at the end.
	//
	// ui_seq is a plain seqlock counter and it closes both. draw_controls - the ONLY function that
	// writes this block after seeding - brackets itself with ui_edit_guard: ODD while it may be
	// mutating anything, EVEN otherwise. begin_pass reads it before and after its snapshot and
	// keeps the snapshot only if it was even and did not move, so a torn read is DISCARDED rather
	// than applied, and the multi-field guarantee is one the code actually provides.
	//
	// It is deliberately separate from `epoch` rather than folded into it: `epoch` is bumped by
	// bump() in the MIDDLE of a draw, which would destroy the odd/even parity, and the consumers
	// that compare epochs need a value that counts changes rather than one that counts entries
	// into the panel.
	std::atomic<uint32_t> ui_seq{ 0 };

	// The consequence epochs. The overlay stores the value relaxed and then bumps one of these
	// with RELEASE; every reader loads one with ACQUIRE before reading any value, which is what
	// makes the values stored BEFORE that bump visible. Each field is a single scalar whose stale
	// value is at most one frame old and, because of the snapshot, self-consistent within the pass.
	std::atomic<uint32_t> epoch{ 0 };           // R0: any change at all
	std::atomic<uint32_t> reset_epoch{ 0 };     // R1: needs one DLSSNR.Reset frame
	std::atomic<uint32_t> flush_epoch{ 0 };     // R2: needs st->pending_res dropped as well
	std::atomic<uint32_t> ident_epoch{ 0 };     // R3: the pass must be re-identified
	std::atomic<uint32_t> rebuild_epoch{ 0 };   // R4: the feature and our textures must go

	// R5: OR-MERGED, NEVER ASSIGNED, and consumed with fetch_and(0) by the present-thread
	// service. An OR-merge is required rather than a plain store because two independent requests
	// - a codec rebuild and a parameter-block rebuild, say, or a user reconfigure landing in the
	// same frame as nr_ensure_output's own resolution-change teardown - can arrive together, and
	// a store would silently drop one of them for ever.
	std::atomic<uint32_t> action_bits{ 0 };
	// The key that most recently asked for a reconfigure, for the log line and the status block.
	// A STRING LITERAL, like every other pointer that crosses this boundary, so it outlives any
	// read. Stored BEFORE the action bits are ORed in, so a reader that sees the bits sees this.
	std::atomic<const char *> action_why{ nullptr };
};

// R5 action bits. Every one of these is performed by nr_service_reconfigure in stray_dlssnr.cpp,
// on the present thread, under st->mutex ONLY - never g.mutex, which would invert nr_try_run's
// st->mutex-then-g.mutex order into a textbook AB/BA against the recording thread.
//
// THERE IS DELIBERATELY NO PER-PIPELINE BIT. An earlier shape of this had a_rebuild_codec,
// a_rebuild_mvec, a_rebuild_params and a_arm_snippet, and every control had to work out which of
// them its own change implied - a checkbox deciding, from the UI thread, whether a pipeline needs
// building. That is a decision the UI cannot make correctly: whether the codec needs a build
// depends on st->codec.ok and st->codec_failed, which live behind st->mutex and which the overlay
// is forbidden to reach for. So the UI raises a_reconcile and the SERVICE compares the live values
// against what actually exists, in one place, with the state to decide it. A control that raises
// the wrong bit then cannot exist.
enum : uint32_t {
	a_teardown     = 1u << 0,   // release the feature, every view and every texture
	a_clear_failed = 1u << 1,   // clear the latched create-failure (the Reset button)
	a_clear_clip   = 1u << 2,   // clear view_layout_failed / clip_ok, re-arm their latches
	a_apply_census = 1u << 3,   // push rt_census / rt_census_frames into rt_census's own atomics
	a_reconcile    = 1u << 4,   // make the codec / mvec / snippet match the live values,
	                            // building or arming whatever is now wanted
	// populate_parameters, AND IT IS THE ONE ACTION THAT MUST NOT RIDE a_reconcile.
	//
	// It used to. The service derived the work from live_populate_parameters() inside the
	// a_reconcile block, so EVERY control that raised a_reconcile - enabled, hdr_codec,
	// mvec_decode, require_trampoline, Reset, Revert - applied whatever the PopulateParameters
	// checkbox happened to say, including a value the user had ticked and deliberately not
	// Applied. The checkbox's own tooltip promises the opposite in as many words ("only when you
	// press Apply beside it - deliberately not on the click"), and one of those six
	// (require_trampoline) raises a_reconcile at k_plain - no teardown - which reached the
	// ON->OFF branch with the NGX feature still alive and freed the parameter block CreateFeature
	// had been handed.
	//
	// So it is its own bit, raised only by the Apply button and by Revert, both of which raise
	// a_teardown with it. The service additionally REFUSES the ON->OFF direction unless the
	// feature really is released, so the invariant is enforced where it can be checked rather
	// than assumed from the caller's rung.
	a_apply_populate = 1u << 5,
};

// The per-consumer, PER-DEVICE seen-epoch state. Deliberately NOT function-local statics: those
// are process-wide, so with two D3D12 devices the edge for a given change would be consumed by
// whichever device's pass ran first and the other would miss it entirely. At three rungs that
// cost one reset frame; at five it costs a whole rebuild. One of these lives in nr_state per
// consumer - begin_pass has one, the present-thread service has another.
struct seen_epochs
{
	uint32_t reset = 0, flush = 0, ident = 0, rebuild = 0;
	bool     first = true;
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
// read by the overlay, exactly as st->hist_restored / census_codec_on already do (:1517-1522) for
// the reason given at :1513-1516. Relaxed everywhere; nothing here is used to order anything.
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

	// Whether the DECODE IN HAND can actually do hdr_graft = 1. Two separate ways it cannot, and
	// in both the combo would otherwise be a control wired to nothing while every status line
	// reported the mode as active - the exact defect this panel exists to prevent.
	//   codec_graft_available   false when the decode was built from the survival source, i.e.
	//                           the compile WITH the reference graft failed on this machine and
	//                           hdr_codec::build fell back so the default would survive.
	//   codec_decode_overridden true when the decode came from a user-supplied
	//                           stray_dlssnr_decode.dxbc. That blob is whatever the user built;
	//                           it may predate g_hdrGraft entirely and read the constant not at
	//                           all. We cannot know, so we say we cannot know.
	std::atomic<bool>         codec_graft_available{ true };
	std::atomic<bool>         codec_decode_overridden{ false };

	std::atomic<float>        auto_scale_x{ 0.0f }, auto_scale_y{ 0.0f };
	std::atomic<uint64_t>     hist_applied{ 0 }, hist_dropped{ 0 };

	// A TIMESTAMP, not a latch. As a latch, a feature that never came back would leave the status
	// reading "REBUILDING" indefinitely - which is exactly the kind of stale-positive this panel
	// exists to avoid, and the same defect the reference add-on's "ACTIVE" has.
	std::atomic<uint64_t>     teardown_ms{ 0 };

	// ---- THE RECONFIGURE, reported the same way and for the same reason -----------------------
	// reconfig_ms is a TIMESTAMP, not a latch, on exactly the argument teardown_ms's comment above
	// makes: as a latch, a reconfigure that never came back would leave the panel reading
	// RECONFIGURING for ever, which is the stale-positive this whole block exists to avoid.
	std::atomic<uint64_t>     reconfig_ms{ 0 };
	// FALSE means the last reconfigure FAILED and the previous working state is still running.
	// The UI shows that in red, above the REBUILDING rung, rather than silently degrading.
	std::atomic<bool>         reconfig_ok{ true };
	// A string literal, like every other pointer here: which key asked, or why it failed.
	std::atomic<const char *> reconfig_what{ nullptr };
	// Set by the service while work is outstanding and cleared when it has all been done. This is
	// what drives the REBUILDING rung now - a real fact published by the thread doing the work,
	// not a three-second guess off a timestamp.
	std::atomic<bool>         reconfig_pending{ false };
	// THE RENDER THREAD'S GATE, and it is what makes a rebuild ATOMIC with respect to the pass.
	//
	// Without it there is a window: the user clicks, the overlay bumps rebuild_epoch, and the next
	// dispatch runs BEFORE the present-thread service has released anything - so it runs with the
	// new g_cfg against the old textures. For hdr_codec ON->OFF that window is not cosmetic: the
	// pass would take the codec-off branch while out_tex was still the r16g16b16a16_float the codec
	// needed, the copy-back's format guard would fire, and the frame would silently get no denoise.
	// A half-applied frame is exactly what requirement "never a half-applied state" rules out.
	//
	// So begin_pass compares the live rebuild epoch against this one and SKIPS the pass while they
	// differ - ReShade then issues the game's own dispatch, a strict no-op - and the service
	// publishes the epoch it actually completed. The skip is one or two frames and is the hitch the
	// tooltips already promise. If the service refuses (no graphics queue to idle), this is not
	// published and the pass correctly stays skipped rather than running against state that is one
	// present away from being destroyed.
	std::atomic<uint32_t>     serviced_rebuild_epoch{ 0 };

	// What the SERVICE has actually done about populate_parameters, so the panel can say when the
	// checkbox and reality disagree. The checkbox alone changes nothing - by design, because
	// PopulateParameters_Impl is a gated export whose signature is unverified against this snippet
	// build - and a ticked box beside an un-applied value is a control that lies unless the panel
	// says so. Published by nr_lazy_ngx_init and by nr_service_reconfigure.
	std::atomic<bool>         populate_applied{ false };
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

	// ELEVEN FIELDS WERE DELETED FROM HERE BY THE RECONFIGURE LADDER, AND THAT IS A FIX.
	//
	// enabled, diagnostics, hdr_codec, shader_hash, srv_depth/velocity/colour, uav_output,
	// populate_parameters and require_trampoline used to be copied out of g_cfg by the DllMain
	// hook. Every one of them is now LIVE, which means begin_pass writes it on a RECORDING thread
	// while this hook runs on the PRESENT thread - the exact data race the on_present hook at
	// stray_dlssnr.cpp:4539-4555 was written to remove for history_restore and copy_back. The
	// overlay reads its own atomics for all eleven instead, which are authoritative anyway: they
	// are what g_cfg is written FROM.
	//
	// What is left is the load-only remainder, and each one is load-only for a reason that is
	// still true: app_id has no live control at all (see the header), ini_found is a statement
	// about a file read once, and the rest are facts about the snippet rather than settings.
	uint64_t app_id = 0;
	bool     ini_found = false;

	bool     snippet_loaded = false;
	bool     trampoline = false;
	bool     armed = false;
	// DLSS-SR's half of the same two facts. The SR section refuses to claim a live toggle it cannot
	// honour, and these are what let it say WHICH of the two reasons applies: the snippet was never
	// loaded (dlss_sr=0 in the ini at launch, so nvngx_dlss.dll was never asked for), or it loaded
	// and Init_Ext through slot B failed. Both are facts about the snippet rather than settings,
	// which is why they live here beside `armed` and not in live_block.
	bool     sr_snippet_loaded = false;
	bool     sr_armed = false;
	bool     abi_thunks_active = false;
	char     snippet_reason[256] = {};

	// DID NVSDK_NGX_D3D12_Init_Ext ALREADY FAIL IN THIS SESSION, and with what.
	//
	// Without this the STANDBY rung tells a provable lie. `armed` false has TWO causes: the
	// deferred initialiser has not run yet (which clears itself on the next dispatch, which is
	// what STANDBY says), or it ran and FAILED - and in that case it can never run again, because
	// nr_try_run's one-shot latch is per-process (stray_dlssnr.cpp: s_init_running is set and
	// never cleared). Re-ticking `enabled` in that state reaches nothing, so the panel must say
	// so rather than promise that STANDBY "clears itself as soon as the game renders".
	bool        ngx_init_failed = false;
	uint32_t    ngx_init_result = 0;
	const char *ngx_init_result_name = nullptr;   // a string literal from ngx::result_to_string
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

// ---------------------------------------------------------------------------------------------
// THE OWNED-FIELD LIST, WRITTEN ONCE.
//
// Four callers need it - seeding from the ini, the Save button's baseline update, dirty(), and
// "Revert to stray_dlssnr.ini" - and before the reconfigure ladder each of them carried its own
// copy of a sixteen-line block. A key added to three of the four is a Save button that never
// lights up for it, or a Revert that leaves it behind: a silent, per-key failure with no
// diagnostic. With the list at forty-one keys that stopped being a theoretical risk, so it is
// spelled out in exactly one place and the four callers share it.
//
// app_id is deliberately absent from all of this. It is the one setting with no live control -
// see the header - so the overlay neither owns it nor writes it, and the ini keeps whatever the
// user put there.
// ---------------------------------------------------------------------------------------------
#define OVERLAY_OWNED_FIELDS(X) \
	X(copy_back) X(history_restore) X(restore_graphics_root) \
	X(paper_white_scale) X(transfer_strength) X(color_strength) \
	X(depth_inverted) X(mvec_scale_x) X(mvec_scale_y) \
	X(intensity) X(local_tone_strength) X(local_structure_strength) \
	X(skin_structure_strength) X(style) X(use_auto_mask) X(ui_correction) \
	X(enabled) X(diagnostics) X(hdr_codec) \
	X(shader_hash) X(srv_depth) X(srv_velocity) X(srv_colour) X(uav_output) \
	X(mvec_decode) X(mvec_reconstruct) X(mvec_dilate) \
	X(mvec_clip_row) X(mvec_clip_transpose) \
	X(populate_parameters) X(require_trampoline) \
	X(rt_census) X(rt_census_frames) \
	X(hdr_graft) \
	X(dlss_sr) X(dlss_nr) X(dlss_chain) X(sr_shader_hash) X(sr_suppress_taa) \
	X(sr_mvec_decode) X(sr_mvec_reconstruct) \
	X(sr_perf_quality) X(sr_render_preset) \
	X(sr_copy_back) X(sr_direct_output) X(sr_use_view_rect) \
	X(sr_jitter_projection_only) X(sr_jitter_scale_x) X(sr_jitter_scale_y) \
	X(sr_mv_scale_x) X(sr_mv_scale_y) \
	X(sr_hdr) X(sr_mv_lowres) X(sr_mv_jittered) X(sr_depth_inverted) \
	X(sr_auto_exposure) X(sr_alpha_upscaling) X(sr_hw_depth) \
	X(sr_group_tile) X(sr_out_width) X(sr_out_height) \
	X(sr_optimal_settings)

inline void live_to_config(const live_block &l, cfg::config &c)
{
#define X(f) c.f = l.f.load(std::memory_order_relaxed);
	OVERLAY_OWNED_FIELDS(X)
#undef X
}

inline void config_to_live(const cfg::config &c, live_block &l)
{
#define X(f) l.f.store(c.f, std::memory_order_relaxed);
	OVERLAY_OWNED_FIELDS(X)
#undef X
}

inline bool same_owned(const cfg::config &a, const cfg::config &b)
{
	bool eq = true;
#define X(f) eq = eq && (a.f == b.f);
	OVERLAY_OWNED_FIELDS(X)
#undef X
	return eq;
}

/// Copy the live half of a freshly loaded config into the atomics. Called ONCE, on the main
/// thread, from nr_init_device immediately after cfg::load - before any dispatch and before any
/// overlay draw, so no reader can observe the half-seeded state.
inline void seed_from_config(const cfg::config &c, const std::wstring &directory)
{
	live_block &l = live();
	// The seqlock's other writer. draw_controls has ui_edit_guard; this is the only other place
	// that stores into live_block, and it stores SIXTY-TWO fields, so it is bracketed the same
	// way. On a single device it is provably uncontended - it runs from nr_init_device, before any
	// dispatch on that device - but that is an argument about a device, not about the process, and
	// this block is process-wide.
	l.ui_seq.fetch_add(1, std::memory_order_release);
	config_to_live(c, l);
	l.bypass.store(false, std::memory_order_relaxed);

	// Seeding is NOT a user edit, so no action bit is raised here: the shipping startup path
	// already builds the codec and mvec pipelines from these same values, and asking the service
	// to redo it would tear down a feature that has only just been created.
	l.action_bits.store(0u, std::memory_order_relaxed);
	l.action_why.store(nullptr, std::memory_order_relaxed);

	baseline() = c;
	ini_dir() = directory;
	seeded().store(true, std::memory_order_release);
	l.epoch.fetch_add(1, std::memory_order_release);
	// Back to even, and AFTER the epoch bump, so a reader that accepts the snapshot has seen both.
	l.ui_seq.fetch_add(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------
// THE RENDER-THREAD HOOK. R0 through R3.
//
// Called from nr_try_run immediately after `std::lock_guard<std::mutex> lock(st->mutex)`
// (stray_dlssnr.cpp:2744) - so it runs on the recording thread, under the lock that pass already
// holds, with nothing else able to observe g_cfg mid-write on that device.
//
// Returns FALSE to skip the pass. Every early return in nr_try_run leaves 'issued' false, which
// leaves ReShade to issue the game's own Dispatch - i.e. a strict no-op. The caller must use a
// plain `return`, NOT NR_BAIL: NR_BAIL's one-shot latch would burn itself on the first toggle and
// never speak again.
//
// WHY WRITING g_cfg IS THE RIGHT SHAPE HERE. The alternative - replacing ~100 g_cfg reads across
// nr_try_run with atomic loads - would make each of the multi-read sites in rule 3 a fresh chance
// to get the snapshot wrong. One assignment block at the top of the pass fixes every one of them
// at once and leaves the reads alone. The four keys that CANNOT come through here - shader_hash,
// diagnostics, and the two mvec flags the main-thread census reads - are listed in the header and
// each has its own accessor below.
//
// THE ACTIONS ARE NOT HERE. R4 and R5 idle the GPU queue, destroy resources and LoadLibrary a
// 166 MB module; none of that may happen on a recording thread. They are serviced from
// on_present by nr_service_reconfigure, which reads take_reconfigure() below. This function only
// ever writes memory it was handed.
// ---------------------------------------------------------------------------------------------
// Published ONCE, from the codec build site in nr_lazy_ngx_init, rather than through begin_pass:
// it is settled before the first dispatch and never changes for the run, and threading it through
// begin_pass would widen a signature that other work is editing.
inline void publish_codec_build(bool graft_available, bool decode_overridden)
{
	status_block &s = status();
	s.codec_graft_available.store(graft_available, std::memory_order_relaxed);
	s.codec_decode_overridden.store(decode_overridden, std::memory_order_relaxed);
}

// need_reset and sr_need_reset are BOTH out-params, and there are TWO rather than one because
// there are two features with two independent Reset flags and no relationship between them that
// the call site could derive. DLSS-NR's st.need_reset is cleared by nr_evaluate; DLSS-SR's
// st.sr_feat.need_reset is cleared by dlss_sr::evaluate_feature. Mirroring the LEVEL of one onto
// the other at the call site would be wrong, and dangerously so in one direction: in a dlss_nr=0,
// dlss_sr=1 run nr_evaluate never runs, so st.need_reset never falls back to false and DLSS-SR
// would be handed Reset=1 on EVERY frame - not "a reset frame" but no temporal accumulation at
// all. Raising both on the same EDGE, here, is the only shape that gives each feature exactly one
// reset frame per user action.
//
// Every rung that raises one raises the other: the two rebuild-gate skips (the resources under
// the feature are about to be replaced), the ident change, the flush, and k_reset itself. k_reset
// is the one this was added for - sr_jitter_scale_x/y, sr_jitter_projection_only and
// sr_mv_scale_x/y are all wired at k_reset with a "Live, one Reset frame" tooltip, and none of
// them moves the output geometry, so sr_try_run's key_moved/geometry_moved seams do not cover
// them either. Before this parameter existed the rung raised only DLSS-NR's flag and those five
// controls changed the jitter convention mid-history with Reset=0 on every subsequent evaluate.
inline bool begin_pass(cfg::config &c,
                       cfg::config &scratch,
                       seen_epochs &seen,
                       bool &need_reset,
                       bool &sr_need_reset,
                       uint64_t &pending_res,
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
	// stored by the overlay before that bump is visible to the snapshot below.
	(void)l.epoch.load(std::memory_order_acquire);
	uint32_t reset_e = 0, flush_e = 0, ident_e = 0, rebuild_e = 0;

	// ---- THE REBUILD GATE, EARLY-OUT HALF ------------------------------------------------------
	// While a rebuild is outstanding this pass must not run at all: the values are new and the
	// textures are still the old ones, which for hdr_codec is the difference between "a hitch"
	// and "a silently skipped copy-back". See status_block::serviced_rebuild_epoch.
	//
	// THIS TEST ALONE IS NOT THE GATE, and the second half below is not belt and braces. Read
	// here, the epoch is read BEFORE the snapshot - so a user who clicks HDR codec in the window
	// between this load and the snapshot would pass the gate on the OLD epoch and then be handed
	// the NEW value, which is precisely the frame this gate exists to prevent. The authoritative
	// test therefore takes rebuild_epoch from INSIDE the seqlock window, next to the values it
	// governs. This one stays because it is free and it skips the snapshot work in the common
	// case where a rebuild really is outstanding.
	//
	// pending_res is dropped on the way out for the same reason every other skip drops it: the arm
	// names a raw resource address, and the resource it names is about to be destroyed.
	if (l.rebuild_epoch.load(std::memory_order_relaxed) !=
	    s.serviced_rebuild_epoch.load(std::memory_order_acquire))
	{
		pending_res = 0;
		need_reset  = true;
		sr_need_reset = true;
		return false;
	}

	// ---- THE SNAPSHOT, UNDER THE SEQLOCK -------------------------------------------------------
	// What makes every read inside this pass coherent - and, now, what makes a MULTI-FIELD edit
	// atomic rather than merely claimed to be. See live_block::ui_seq for why the epoch alone
	// never provided that: the widgets store the value and bump AFTERWARDS, so an acquire on the
	// epoch orders a reader against a release that has already happened and says nothing about the
	// one still to come.
	//
	// The snapshot is built into a scratch config and only COMMITTED when ui_seq was even
	// throughout, so a torn read is discarded instead of applied. The scratch is OWNED BY THE
	// CALLER - it lives in nr_state, beside seen_pass, for two reasons. cfg::config holds a
	// std::string (ini_path), so a fresh local per pass would be a heap allocation on the render
	// thread every frame; assigning into a long-lived object reuses its capacity and allocates
	// nothing after the first. And per-DEVICE rather than per-thread or process-wide puts it on
	// exactly the same footing as seen_pass, under the same mutex, and needs no TLS.
	//
	// IF IT NEVER SETTLES WE KEEP THE PREVIOUS SNAPSHOT AND RUN ANYWAY - we do NOT skip the pass.
	// The previous snapshot is a fully coherent one, and the overlay is drawn on the present
	// thread once a frame while this runs on a recording thread, so a persistent collision is not
	// a state that occurs; skipping would trade a theoretical tear for a real dropped denoise
	// frame every time the user has the panel open, which is exactly when they are looking at the
	// image. The epochs are read inside the verified window too, so a rejected snapshot leaves
	// `seen` untouched and the change is picked up whole on the next pass.
	cfg::config &tmp = scratch;
	bool snapshot_ok = false;
	for (int attempt = 0; attempt < 8 && !snapshot_ok; ++attempt)
	{
		const uint32_t seq0 = l.ui_seq.load(std::memory_order_acquire);
		if ((seq0 & 1u) != 0u)
			continue;   // draw_controls is mid-edit; look again

		tmp = c;
		tmp.copy_back                = l.copy_back.load(std::memory_order_relaxed);
		tmp.history_restore          = l.history_restore.load(std::memory_order_relaxed);
		tmp.restore_graphics_root    = l.restore_graphics_root.load(std::memory_order_relaxed);
		tmp.paper_white_scale        = l.paper_white_scale.load(std::memory_order_relaxed);
		tmp.transfer_strength        = l.transfer_strength.load(std::memory_order_relaxed);
		tmp.color_strength           = l.color_strength.load(std::memory_order_relaxed);
		tmp.depth_inverted           = l.depth_inverted.load(std::memory_order_relaxed);
		tmp.mvec_scale_x             = l.mvec_scale_x.load(std::memory_order_relaxed);
		tmp.mvec_scale_y             = l.mvec_scale_y.load(std::memory_order_relaxed);
		tmp.intensity                = l.intensity.load(std::memory_order_relaxed);
		tmp.local_tone_strength      = l.local_tone_strength.load(std::memory_order_relaxed);
		tmp.local_structure_strength = l.local_structure_strength.load(std::memory_order_relaxed);
		tmp.skin_structure_strength  = l.skin_structure_strength.load(std::memory_order_relaxed);
		tmp.style                    = l.style.load(std::memory_order_relaxed);
		tmp.use_auto_mask            = l.use_auto_mask.load(std::memory_order_relaxed);
		tmp.ui_correction            = l.ui_correction.load(std::memory_order_relaxed);
		// ---- newly live. Every read site of each of these is downstream of this call.
		tmp.srv_depth                = l.srv_depth.load(std::memory_order_relaxed);
		tmp.srv_velocity             = l.srv_velocity.load(std::memory_order_relaxed);
		tmp.srv_colour               = l.srv_colour.load(std::memory_order_relaxed);
		tmp.uav_output               = l.uav_output.load(std::memory_order_relaxed);
		tmp.hdr_codec                = l.hdr_codec.load(std::memory_order_relaxed);
		tmp.mvec_decode              = l.mvec_decode.load(std::memory_order_relaxed);
		tmp.mvec_reconstruct         = l.mvec_reconstruct.load(std::memory_order_relaxed);
		tmp.mvec_dilate              = l.mvec_dilate.load(std::memory_order_relaxed);
		tmp.mvec_clip_transpose      = l.mvec_clip_transpose.load(std::memory_order_relaxed);
		tmp.mvec_clip_row            = l.mvec_clip_row.load(std::memory_order_relaxed);
		// ---- DLSS-SR. dlss_sr itself belongs here because its ONE functional read site - the
		// branch into sr_try_run - is downstream of this call, as is the output-extent derivation
		// just above it and every sr_* read inside sr_try_run. sr_perf_quality and sr_render_preset
		// are snapshotted for the same reason even though CreateFeature latches them: the read is
		// still downstream, and the teardown their controls raise is what makes the latched value
		// re-taken rather than what makes the value visible.
		//
		// sr_shader_hash is DELIBERATELY ABSENT, exactly like shader_hash: its read site runs before
		// this function does, so a value written here would look correct to a reader and reach
		// nothing. read_ident() serves it. dlss_nr is absent because it has NO read site downstream
		// of this call at all - see live_block::dlss_nr.
		tmp.dlss_sr                  = l.dlss_sr.load(std::memory_order_relaxed);
		tmp.sr_suppress_taa          = l.sr_suppress_taa.load(std::memory_order_relaxed);
		tmp.sr_mvec_decode           = l.sr_mvec_decode.load(std::memory_order_relaxed);
		tmp.sr_mvec_reconstruct      = l.sr_mvec_reconstruct.load(std::memory_order_relaxed);
		tmp.sr_perf_quality          = l.sr_perf_quality.load(std::memory_order_relaxed);
		tmp.sr_render_preset         = l.sr_render_preset.load(std::memory_order_relaxed);
		// dlss_chain: the chain_ok test in nr_try_run is downstream of this call. Its OTHER read
		// site - want_hash - is not, and is served by read_ident() instead, exactly as
		// sr_shader_hash is.
		tmp.dlss_chain               = l.dlss_chain.load(std::memory_order_relaxed);
		// ---- the rest of DLSS-SR, tier 0. Read inside sr_try_run, downstream of this call.
		tmp.sr_copy_back             = l.sr_copy_back.load(std::memory_order_relaxed);
		tmp.sr_direct_output         = l.sr_direct_output.load(std::memory_order_relaxed);
		tmp.sr_use_view_rect         = l.sr_use_view_rect.load(std::memory_order_relaxed);
		tmp.sr_jitter_projection_only = l.sr_jitter_projection_only.load(std::memory_order_relaxed);
		tmp.sr_jitter_scale_x        = l.sr_jitter_scale_x.load(std::memory_order_relaxed);
		tmp.sr_jitter_scale_y        = l.sr_jitter_scale_y.load(std::memory_order_relaxed);
		tmp.sr_mv_scale_x            = l.sr_mv_scale_x.load(std::memory_order_relaxed);
		tmp.sr_mv_scale_y            = l.sr_mv_scale_y.load(std::memory_order_relaxed);
		// ---- tier 1. Snapshotted for COHERENCE - the create-params block reads six of them in one
		// expression - and made to TAKE by the feature release their controls ask for.
		tmp.sr_hdr                   = l.sr_hdr.load(std::memory_order_relaxed);
		tmp.sr_mv_lowres             = l.sr_mv_lowres.load(std::memory_order_relaxed);
		tmp.sr_mv_jittered           = l.sr_mv_jittered.load(std::memory_order_relaxed);
		tmp.sr_depth_inverted        = l.sr_depth_inverted.load(std::memory_order_relaxed);
		tmp.sr_auto_exposure         = l.sr_auto_exposure.load(std::memory_order_relaxed);
		tmp.sr_alpha_upscaling       = l.sr_alpha_upscaling.load(std::memory_order_relaxed);
		tmp.sr_hw_depth              = l.sr_hw_depth.load(std::memory_order_relaxed);
		// ---- tier 2. The output extent, read in nr_try_run AFTER this call (the group-count
		// derivation and the output-UAV pick) and then again inside sr_try_run.
		tmp.sr_group_tile            = l.sr_group_tile.load(std::memory_order_relaxed);
		tmp.sr_out_width             = l.sr_out_width.load(std::memory_order_relaxed);
		tmp.sr_out_height            = l.sr_out_height.load(std::memory_order_relaxed);
		// sr_optimal_settings is DELIBERATELY ABSENT for the same reason dlss_nr is: its only read
		// site is in nr_lazy_ngx_init, which has already run by the time this does. Writing it here
		// would look correct to a reader and reach nothing.
		// hdr_graft rides the snapshot too - it is a ROOT CONSTANT in the decode's own constant
		// block, read by nr_codec_decode on both the NR-alone and the chained path, both of which
		// are downstream of this call.
		tmp.hdr_graft                = l.hdr_graft.load(std::memory_order_relaxed);
		// shader_hash is DELIBERATELY ABSENT from this list. Its read site runs before this
		// function does; read_ident() serves it instead. Writing it here would be worse than
		// useless - it would look correct to a reader and reach nothing.

		// The consequence epochs belong INSIDE the window: a value and the rung that carries its
		// consequence must be taken from the same instant, which is the whole point.
		reset_e   = l.reset_epoch.load(std::memory_order_relaxed);
		flush_e   = l.flush_epoch.load(std::memory_order_relaxed);
		ident_e   = l.ident_epoch.load(std::memory_order_relaxed);
		rebuild_e = l.rebuild_epoch.load(std::memory_order_relaxed);

		if (l.ui_seq.load(std::memory_order_acquire) == seq0)
		{
			c = tmp;
			snapshot_ok = true;
		}
	}

	if (!snapshot_ok)
	{
		// The previous g_cfg is untouched and coherent. Run the pass on it; the edit lands next
		// frame. Nothing below may run, because reset_e / flush_e / ident_e were never taken - so
		// the two switches are re-tested here, verbatim, rather than fallen through to.
		if (!l.enabled.load(std::memory_order_relaxed) || l.bypass.load(std::memory_order_relaxed))
		{
			pending_res = 0;
			return false;
		}
		return true;
	}

	// ---- THE REBUILD GATE, AUTHORITATIVE HALF --------------------------------------------------
	// rebuild_e came from inside the verified window, so it belongs to the SAME instant as the
	// values just committed. serviced_rebuild_epoch is loaded here, after it: if the service has
	// caught up in the meantime the pass runs with new values against new textures, which is
	// correct; if it has not, the pass is skipped. Either way there is no frame in which a value
	// is applied ahead of the resources it needs.
	//
	// `seen` is deliberately not touched before this: a skipped pass must leave every rung
	// unconsumed, or the consequence would be lost with the frame.
	if (rebuild_e != s.serviced_rebuild_epoch.load(std::memory_order_acquire))
	{
		pending_res = 0;
		need_reset  = true;
		sr_need_reset = true;
		return false;
	}

	if (seen.first)
	{
		// Adopt the current epochs on the very first pass rather than treating "the overlay has
		// been seeded" as a user edit; a reset frame on the first evaluate is initialisation
		// anyway, and pending_res is 0 there.
		seen.reset = reset_e; seen.flush = flush_e; seen.ident = ident_e;
		seen.first = false;
	}

	// ---- consequences ------------------------------------------------------------------------
	// Strictly nesting, strongest first. R3 implies R2 implies R1, so one `else if` chain is
	// exactly right and a change that bumps several of them is serviced once.
	if (ident_e != seen.ident)
	{
		seen.ident = ident_e;
		seen.flush = flush_e;
		seen.reset = reset_e;
		// The identification changed, so an arm made under the OLD srv_colour must not be
		// consumed under the new one. That is precisely the "last frame's image on this frame's
		// scene-colour input" hazard the restore path already refuses by name at :3249-3260.
		pending_res = 0;
		need_reset  = true;
		sr_need_reset = true;
	}
	else if (flush_e != seen.flush)
	{
		seen.flush = flush_e;
		seen.reset = reset_e;
		// See the header: pending_res is a raw resource ADDRESS held across a frame, and
		// :3206-3210 documents that UE 4.27 can recycle it. Never let one survive a toggle of the
		// thing that armed it.
		pending_res = 0;
		need_reset  = true;
		sr_need_reset = true;
	}
	else if (reset_e != seen.reset)
	{
		seen.reset = reset_e;
		need_reset = true;
		sr_need_reset = true;
	}

	// ---- the two switches ---------------------------------------------------------------------
	// AFTER the snapshot, so g_cfg is coherent whether or not we run, and after the flush, so
	// every edge of either toggle drops any pending pristine copy.
	//
	// `enabled` IS TESTED HERE RATHER THAN BY TEARING NGX DOWN, and that is a deliberate,
	// evidence-driven choice. Turning it off could instead have cleared g_nr_armed - but then
	// turning it back on would re-run nr_lazy_ngx_init and call NVSDK_NGX_D3D12_Init_Ext A SECOND
	// TIME in the session, and the only thing this project has measured about Init_Ext's
	// fragility is that it HANGS when called at a moment the snippet does not tolerate
	// (stray_dlssnr.cpp:4049-4056). A hang is not a failure that degrades. So OFF releases the
	// feature and every texture - which is where the VRAM actually is - and stops the pass here,
	// while NGX itself stays initialised; ON simply lets the pass through again and the existing
	// nr_ensure_* path rebuilds everything on the next dispatch. Both directions are live, and no
	// second Init_Ext is ever needed. The tooltip says exactly this.
	if (!l.enabled.load(std::memory_order_relaxed) || l.bypass.load(std::memory_order_relaxed))
	{
		pending_res = 0;
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------------------------
// THE IDENTIFICATION READ. R3's render-path half.
//
// stray_dlssnr.cpp memoises "is cs->pso the shader the ini pinned?" per pipeline state object
// (:2708-2727). That memo is cleared unconditionally on every SetPipelineState (:912-913) and by
// cmd_shadow::reset, so its staleness window is bounded by the dispatches left in one command
// list after its last SetPipelineState - narrower than the old ledger claimed, but real, and
// NON-DETERMINISTIC across concurrently recording threads. An epoch beside the memo is what
// invalidates it on EVERY command list rather than on the next one: a single atomic that each
// recording thread reads for itself.
//
// Cost on the hot path: one relaxed load and one 32-bit compare, next to a pointer compare that
// already runs on every dispatch of every command list.
// ---------------------------------------------------------------------------------------------
struct ident_view
{
	uint32_t epoch = 0;
	uint64_t shader_hash = 0;
	// DLSS-SR RE-PINS THE SAME DISPATCH UNDER A DIFFERENT PERMUTATION, so the identification read
	// has to carry both keys and the switch between them. TAA_PASS_CONFIG and
	// TAA_SCREEN_PERCENTAGE_RANGE are #defines: flipping r.TemporalAA.Upsampling gives different
	// DXBC and a different fnv1a64, so the MainUpsampling permutation DLSS-SR targets is a
	// different hash from the one DLSS-NR pins - and both have to be live, because both are edited
	// from the same panel while the game runs. Putting the choice in the view rather than at the
	// call site is what keeps it a single coherent read: all three come from inside one acquire.
	bool     dlss_sr = false;
	// CHAIN MODE IS TREATED EXACTLY LIKE dlss_sr HERE, and it has to be: the chain only upscales in
	// the MainUpsampling permutation, which is a different #define set and therefore a different
	// DXBC and a different fnv1a64. It rides the SAME acquire as the other three, so a user who
	// retypes the hash and flips the mode cannot be observed half way.
	bool     dlss_chain = false;
	uint64_t sr_shader_hash = 0;
};

inline ident_view read_ident()
{
	const live_block &l = live();
	ident_view v;
	// ACQUIRE first, then the values, exactly as begin_pass does: bump(k_ident) stores the hashes
	// relaxed and then releases the epoch, so acquiring here is what makes the group visible.
	v.epoch          = l.ident_epoch.load(std::memory_order_acquire);
	v.shader_hash    = l.shader_hash.load(std::memory_order_relaxed);
	v.dlss_sr        = l.dlss_sr.load(std::memory_order_relaxed);
	v.dlss_chain     = l.dlss_chain.load(std::memory_order_relaxed);
	v.sr_shader_hash = l.sr_shader_hash.load(std::memory_order_relaxed);
	return v;
}

/// THE HASH THIS DISPATCH MUST MATCH. One place, so the render path and the arm banner cannot
/// drift apart on it.
///
/// sr_shader_hash carries the MainUpsampling permutation's hash without disturbing the DLSS-NR
/// pin, so the two features can be A/B'd on one install. 0 falls back to shader_hash, which is
/// also what dlss_sr=0 uses unconditionally. Everything here comes out of ONE ident_view, i.e.
/// out of one acquire, so a user who retypes both hashes cannot be observed half way.
inline uint64_t want_hash(const ident_view &v)
{
	return ((v.dlss_sr || v.dlss_chain) && v.sr_shader_hash != 0) ? v.sr_shader_hash : v.shader_hash;
}

// ---------------------------------------------------------------------------------------------
// THE SERVICE-SIDE READ. R3 through R5, consumed on the PRESENT thread.
//
// Called from nr_service_reconfigure, which holds st->mutex and nothing else. It is a separate
// consumer from begin_pass with its own seen-epoch state, and that is load-bearing rather than
// tidy: with enabled=0, or with the feature wedged, or with the master bypass on, begin_pass may
// never run at all - and those are exactly the cases where a reconfigure most needs to land.
// ---------------------------------------------------------------------------------------------
struct reconfig_request
{
	uint32_t    bits = 0;          // a_* above; zero means "nothing to do"
	const char *why  = nullptr;    // a string literal naming the key that asked
	bool        ident_changed = false;
	// The rebuild epoch this request was taken AT. Published back through
	// publish_serviced_rebuild once the work is done, so the render thread's gate opens on the
	// value that was actually serviced rather than on a re-read that may have moved again while
	// the service was releasing textures.
	uint32_t    rebuild_epoch = 0;
};

/// Adopt the CURRENT epochs into a seen-state without treating them as an edit.
///
/// Called once from nr_lazy_ngx_init, on the state it has just created, BEFORE it does any of its
/// work. Without it, nr_state::seen_service adopts lazily on the first present AFTER init - and
/// nr_lazy_ngx_init is hundreds of milliseconds long (Init_Ext plus up to two D3DCompile calls),
/// so a rebuild the user asked for DURING that window was adopted rather than serviced: the one
/// control that depends on the epoch alone to imply its teardown would then run its ON->OFF branch
/// against a live feature. Seeding at the start of init closes the window; the cost of the values
/// having been read after the bump is one redundant teardown, which is the harmless direction.
inline void adopt_epochs(seen_epochs &seen)
{
	const live_block &l = live();
	seen.reset   = l.reset_epoch.load(std::memory_order_acquire);
	seen.flush   = l.flush_epoch.load(std::memory_order_relaxed);
	seen.ident   = l.ident_epoch.load(std::memory_order_relaxed);
	seen.rebuild = l.rebuild_epoch.load(std::memory_order_relaxed);
	seen.first   = false;
}

inline reconfig_request take_reconfigure(seen_epochs &seen)
{
	live_block &l = live();
	reconfig_request r;

	// ACQUIRE, and first. Pairs with the RELEASE in bump()/request().
	const uint32_t ident_e   = l.ident_epoch.load(std::memory_order_acquire);
	const uint32_t rebuild_e = l.rebuild_epoch.load(std::memory_order_relaxed);

	if (seen.first)
	{
		seen.ident = ident_e; seen.rebuild = rebuild_e;
		seen.reset = l.reset_epoch.load(std::memory_order_relaxed);
		seen.flush = l.flush_epoch.load(std::memory_order_relaxed);
		seen.first = false;
		// Still drain any bits that were requested before the first present, so a reconfigure
		// asked for during start-up is not silently swallowed by the adoption above.
	}

	if (ident_e != seen.ident)
	{
		seen.ident = ident_e;
		r.ident_changed = true;
	}
	r.rebuild_epoch = rebuild_e;
	if (rebuild_e != seen.rebuild)
	{
		seen.rebuild = rebuild_e;
		r.bits |= a_teardown;
	}

	// WHY FIRST, THEN THE BITS, and the order is the opposite of the writer's on purpose.
	// request() stores the name and then ORs the bits; reading in the same order could hand back a
	// name whose bits have not arrived yet. Reading the name first can only hand back the name of
	// an EARLIER request - one whose bits the fetch_and below therefore definitely includes. So
	// the name always belongs to something that is actually being applied.
	r.why = l.action_why.load(std::memory_order_relaxed);
	// fetch_and(0) with ACQUIRE: takes every bit that has been ORed in since the last service
	// pass and leaves the word empty for the next batch. Nothing else ever clears it.
	r.bits |= l.action_bits.fetch_and(0u, std::memory_order_acquire);
	return r;
}

/// Published by the service when a reconfigure finishes, succeeds or fails. `what` must be a
/// string literal. ok=false means the previous working state is still running.
inline void publish_reconfigure(bool ok, const char *what)
{
	status_block &s = status();
	s.reconfig_what.store(what, std::memory_order_relaxed);
	s.reconfig_ok.store(ok, std::memory_order_relaxed);
	s.reconfig_ms.store(static_cast<uint64_t>(GetTickCount64()), std::memory_order_relaxed);
}

/// True while the service still has outstanding work. Drives the REBUILDING rung.
inline void publish_reconfig_pending(bool pending)
{
	status().reconfig_pending.store(pending, std::memory_order_relaxed);
}

/// Stamped by the service when it has actually released the feature and the textures. Keeps
/// teardown_ms meaning what its own comment says it means now that the Reset button no longer
/// goes through begin_pass - and it is what keeps REBUILDING on screen long enough to read.
/// reconfig_pending alone would flash for a single frame, because the service finishes the
/// release in one present and the rebuild then happens on the next dispatch; the three-second
/// window off this timestamp is what covers that gap, and publish_evaluate clears it the moment
/// an evaluate succeeds, so it can never be a stale positive.
inline void publish_teardown()
{
	status().teardown_ms.store(static_cast<uint64_t>(GetTickCount64()), std::memory_order_relaxed);
}

/// Mirrors nr_state::serviced_populate_parameters out to the panel. Written by the render thread
/// at arm time and by the service on every change; read only by the overlay.
inline void publish_populate(bool applied)
{
	status().populate_applied.store(applied, std::memory_order_relaxed);
}

/// Opens the render thread's rebuild gate. RELEASE, and it must be the LAST thing the service
/// does with that reconfigure: it is what tells the recording thread the textures it is about to
/// create can be created against the new settings.
inline void publish_serviced_rebuild(uint32_t epoch)
{
	status().serviced_rebuild_epoch.store(epoch, std::memory_order_release);
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
		// THE ONE FIELD THE renodx STATUS BLOCK HAS NO EQUIVALENT OF. Its "ACTIVE" means "a
		// feature object currently exists" and its frame counter only ever increments, so an
		// add-on that stopped evaluating ten minutes ago still reads ACTIVE with a large count.
		// A timestamp is what turns that into an answerable question.
		//
		// STAMPED BEFORE THE COUNTER IS INCREMENTED, and the order is load-bearing. The overlay
		// treats a nonzero count as "eval_ms is meaningful": on the other order, a draw landing
		// between the two stores reads evaluates==1 with eval_ms==0, skips the evals==0 rung and
		// prints "last evaluate -1.0 s ago". This way the only visible intermediate is
		// eval_ms set with evaluates still 0, which shows the evals==0 rung for one frame at the
		// very first successful evaluate - a true statement one frame late, not a false one.
		s.eval_ms.store(static_cast<uint64_t>(GetTickCount64()), std::memory_order_relaxed);
		s.teardown_ms.store(0, std::memory_order_relaxed);
		s.evaluates.fetch_add(1, std::memory_order_relaxed);
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
/// Same rule, same reason, for the mvec census line at stray_dlssnr.cpp:4564/:4572. :4564 is the
/// GATE for that line, so a racy read there is worse than a racy argument: it can admit a line
/// that then reports the opposite state.
inline bool live_mvec_decode()      { return live().mvec_decode.load(std::memory_order_relaxed); }
inline bool live_mvec_reconstruct() { return live().mvec_reconstruct.load(std::memory_order_relaxed); }
/// THE ONE THAT CAN NEVER GO THROUGH g_cfg AT ALL. g_cfg.diagnostics was read at :4425 (on_draw),
/// :4430 (on_draw_indexed) and :4449 (on_dispatch) - every draw and every dispatch in the
/// process, on arbitrary recording threads, with no lock and outside the accepted-pass snapshot.
/// One relaxed atomic load at each of those three sites makes it live at zero risk, which is why
/// the old ledger's "threads this overlay must not race with" was a reason to use an atomic and
/// not a reason to grey the control out.
inline bool live_diagnostics()      { return live().diagnostics.load(std::memory_order_relaxed); }
/// Read by nr_init_device in place of g_cfg.enabled, so that the ini's master switch has exactly
/// ONE reader and the overlay's copy is authoritative for both the UI and the arm decision.
inline bool live_enabled()          { return live().enabled.load(std::memory_order_relaxed); }
inline bool live_hdr_codec()        { return live().hdr_codec.load(std::memory_order_relaxed); }
// hdr_graft rides the per-pass snapshot at its two render-path read sites; this accessor exists
// for the ARM BANNER, which is printed from nr_lazy_ngx_init BEFORE begin_pass has ever run on
// this device - so g_cfg there still holds whatever the ini said even when the user has already
// changed it and re-armed from the panel. Exactly why live_hdr_codec() above exists.
inline uint32_t live_hdr_graft()    { return live().hdr_graft.load(std::memory_order_relaxed); }
inline bool live_require_trampoline() { return live().require_trampoline.load(std::memory_order_relaxed); }
inline bool live_populate_parameters() { return live().populate_parameters.load(std::memory_order_relaxed); }
inline bool     live_rt_census()        { return live().rt_census.load(std::memory_order_relaxed); }
inline uint32_t live_rt_census_frames() { return live().rt_census_frames.load(std::memory_order_relaxed); }
// ---- DLSS-SR. Read at their own sites, on threads or at moments the per-pass snapshot does not
// cover: nr_init_device (main thread, before any dispatch), nr_lazy_ngx_init (recording thread,
// before begin_pass has ever run for this device) and nr_service_reconfigure (present thread).
// Every one of them is the same relaxed load the accessors above are.
inline bool     live_dlss_sr()             { return live().dlss_sr.load(std::memory_order_relaxed); }
inline bool     live_dlss_nr()             { return live().dlss_nr.load(std::memory_order_relaxed); }
inline bool     live_sr_mvec_decode()      { return live().sr_mvec_decode.load(std::memory_order_relaxed); }
inline bool     live_sr_mvec_reconstruct() { return live().sr_mvec_reconstruct.load(std::memory_order_relaxed); }
inline bool     live_sr_suppress_taa()     { return live().sr_suppress_taa.load(std::memory_order_relaxed); }
inline uint32_t live_sr_perf_quality()     { return live().sr_perf_quality.load(std::memory_order_relaxed); }
inline uint32_t live_sr_render_preset()    { return live().sr_render_preset.load(std::memory_order_relaxed); }
inline uint64_t live_sr_shader_hash()      { return live().sr_shader_hash.load(std::memory_order_relaxed); }
// THESE TWO EXIST FOR ONE READ SITE EACH, AND IT IS NOT A SNAPSHOT SITE. Both keys are in
// OVERLAY_OWNED_FIELDS and in begin_pass's per-pass snapshot, so g_cfg's copies are written on a
// RECORDING thread - and on_present's DLSS-SR census line runs on the MAIN thread, holding neither
// st->mutex nor any overlay lock. Reading g_cfg there would be a data race on two non-atomic bools
// and could report the pre-edit state indefinitely. That is the identical race the census's
// history_restore/copy_back and mvec_decode/mvec_reconstruct lines already read atomics to avoid.
inline bool     live_sr_direct_output()    { return live().sr_direct_output.load(std::memory_order_relaxed); }
inline bool     live_sr_copy_back()        { return live().sr_copy_back.load(std::memory_order_relaxed); }
// dlss_chain has read sites on BOTH sides of begin_pass, so like dlss_sr it needs an accessor as
// well as a snapshot line: nr_init_device and nr_lazy_ngx_init decide whether to LoadLibraryW
// nvngx_dlss.dll and whether to Init_Ext through slot B, and both of those run before any pass.
inline bool     live_dlss_chain()           { return live().dlss_chain.load(std::memory_order_relaxed); }
// Read at arm time only - nr_lazy_ngx_init's optimal-settings query - which is exactly why it has
// an accessor and no snapshot line.
inline bool     live_sr_optimal_settings()  { return live().sr_optimal_settings.load(std::memory_order_relaxed); }

// =============================================================================================
// PERSISTENCE
//
// stray_dlssnr.ini stays the one source of truth, and we write it ourselves.
//
// NOT ReShade's config API, for four reasons, the first two of which are already written down in
// this tree:
//   1. reshade.hpp:202 - set_config_value "Sets AND SAVES". That is one ReShade.ini write per
//      frame of a slider drag.
//   2. addon_config.hpp:3-5 already argues the case: ReShade's get_config_value keys off
//      ReShade.ini, "which the user is also editing for effects, and a missing key there silently
//      yields a default with no diagnostic. Here every parse is reported."
//   3. ReShade rewrites ReShade.ini itself - that is how DisabledAddons got written in the first
//      place. Two writers, one file.
//   4. It would split the source of truth: the ini beside the add-on is still read at :3169.
//
// WRITE POLICY
//   * Only on the explicit Save button. Never per-frame, never on a drag.
//   * REWRITE IN PLACE. The shipped stray_dlssnr.ini is 200-odd commented lines and is the
//     documentation the user actually reads; a naive regenerate destroys it. Every comment, every
//     unrecognised line, every blank line and the original key order and spelling survive.
//   * TEMP FILE + MoveFileExW(REPLACE_EXISTING). A half-written ini is worse than none: per
//     addon_config.hpp:226-230 every key after the cut silently takes its built-in default.
//   * EVERY KEY THE PANEL OWNS IS ROUND-TRIPPED, AND THAT IS NOW ALL 33 OF THEM - the
//     identification pins (shader_hash, srv_depth/velocity/colour, uav_output) included, along
//     with enabled, diagnostics, hdr_codec, populate_parameters and require_trampoline. This
//     bullet used to say the exact opposite, and it was left behind when the reconfigure ladder
//     made those keys live: a control that applies live but silently forgets on relaunch is still
//     a control that lies, just more slowly, so owned_value() and owned_keys() below write them
//     all. The single source of truth is OVERLAY_OWNED_FIELDS; if a key is in that list, Save
//     writes it.
//   * app_id IS THE ONE EXCEPTION, and it is the only one. It has no live control at all (see the
//     header and draw_load_only), so the overlay neither owns it nor writes it and the ini keeps
//     whatever the user put there. Unrecognised keys, comments, blank lines and key order are
//     likewise untouched.
//   * WHAT THAT MEANS FOR A HAND-MEASURED CONFIG: the pins are safe from Save only in the sense
//     that Save writes what the PANEL currently shows. Drag srv_velocity while chasing an artefact
//     and then press Save, and the dragged value is what lands on disk. Use "Revert to
//     stray_dlssnr.ini" to get the measured values back before saving; the Save tooltip says this
//     too, because that is where it is read.
// =============================================================================================

inline void fmt_float(char *buf, size_t n, float v)
{
	// %.9g round-trips a float exactly and still prints 1.0f as "1" and -1.0f as "-1".
	std::snprintf(buf, n, "%.9g", static_cast<double>(v));
}

/// The 62 keys the overlay owns - the OVERLAY_OWNED_FIELDS list, spelling aliases included.
/// Returns false for anything else, which is what leaves app_id and unrecognised keys alone.
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
	if (key_lower == "hdr_graft")                { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.hdr_graft.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "style")                    { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.style.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "ui_correction")            { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.ui_correction.load(std::memory_order_relaxed)); out = buf; return true; }
	// ---- the keys that became live controls with the reconfigure ladder. They are round-tripped
	// now for the reason the ladder exists at all: a control that applies live but silently
	// forgets on relaunch is still a control that lies, just more slowly. app_id is the one
	// setting deliberately still NOT owned - see the header, and draw_load_only.
	if (key_lower == "enabled")                  { out = l.enabled.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "diagnostics")              { out = l.diagnostics.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "hdr_codec")                { out = l.hdr_codec.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "mvec_decode")              { out = l.mvec_decode.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "mvec_reconstruct")         { out = l.mvec_reconstruct.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "mvec_dilate")              { out = l.mvec_dilate.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "mvec_clip_transpose")      { out = l.mvec_clip_transpose.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "populate_parameters")      { out = l.populate_parameters.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "require_trampoline")       { out = l.require_trampoline.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "rt_census")                { out = l.rt_census.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "rt_census_frames")         { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.rt_census_frames.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "mvec_clip_row")            { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.mvec_clip_row.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "srv_depth")                { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.srv_depth.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "srv_velocity")             { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.srv_velocity.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "srv_colour" ||
	    key_lower == "srv_color")                { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.srv_colour.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "uav_output")               { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.uav_output.load(std::memory_order_relaxed)); out = buf; return true; }
	// Hex, zero-padded to 16, which is how every shader hash in this tree's logs, its README and
	// its shipped ini is written. A decimal here would round-trip correctly and be unreadable.
	if (key_lower == "shader_hash")              { std::snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)l.shader_hash.load(std::memory_order_relaxed)); out = buf; return true; }
	// ---- DLSS-SR and the two feature master switches. Owned for the reason every other key here
	// is owned: a control that applies and then forgets on relaunch is still a control that lies.
	// dlss_nr is in this list even though it is launch-time - being launch-time is exactly why it
	// has to survive a Save.
	if (key_lower == "dlss_sr")                  { out = l.dlss_sr.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "dlss_nr")                  { out = l.dlss_nr.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_suppress_taa")          { out = l.sr_suppress_taa.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_mvec_decode")           { out = l.sr_mvec_decode.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_mvec_reconstruct")      { out = l.sr_mvec_reconstruct.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_perf_quality")          { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.sr_perf_quality.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "sr_render_preset")         { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.sr_render_preset.load(std::memory_order_relaxed)); out = buf; return true; }
	// Hex for the same reason shader_hash is hex: it is copied out of ReShade.log by eye.
	if (key_lower == "sr_shader_hash")           { std::snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)l.sr_shader_hash.load(std::memory_order_relaxed)); out = buf; return true; }
	// ---- chain mode, and the rest of DLSS-SR. Every key in OVERLAY_OWNED_FIELDS has to appear
	// here and in owned_keys() below, or Save writes the panel's other edits and silently drops
	// this one - which is the failure the ini round-trip test in abi/ exists to catch.
	if (key_lower == "dlss_chain")               { out = l.dlss_chain.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_copy_back")             { out = l.sr_copy_back.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_direct_output")         { out = l.sr_direct_output.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_use_view_rect")         { out = l.sr_use_view_rect.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_jitter_projection_only"){ out = l.sr_jitter_projection_only.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_hdr")                   { out = l.sr_hdr.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_mv_lowres")             { out = l.sr_mv_lowres.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_mv_jittered")           { out = l.sr_mv_jittered.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_depth_inverted")        { out = l.sr_depth_inverted.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_auto_exposure")         { out = l.sr_auto_exposure.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_alpha_upscaling")       { out = l.sr_alpha_upscaling.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_hw_depth")              { out = l.sr_hw_depth.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_optimal_settings")      { out = l.sr_optimal_settings.load(std::memory_order_relaxed) ? "1" : "0"; return true; }
	if (key_lower == "sr_jitter_scale_x")        { fmt_float(buf, sizeof(buf), l.sr_jitter_scale_x.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "sr_jitter_scale_y")        { fmt_float(buf, sizeof(buf), l.sr_jitter_scale_y.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "sr_mv_scale_x")            { fmt_float(buf, sizeof(buf), l.sr_mv_scale_x.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "sr_mv_scale_y")            { fmt_float(buf, sizeof(buf), l.sr_mv_scale_y.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "sr_group_tile")            { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.sr_group_tile.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "sr_out_width")             { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.sr_out_width.load(std::memory_order_relaxed)); out = buf; return true; }
	if (key_lower == "sr_out_height")            { std::snprintf(buf, sizeof(buf), "%u", (unsigned)l.sr_out_height.load(std::memory_order_relaxed)); out = buf; return true; }
	return false;
}

// The canonical spellings, in the order they are appended when absent from the file.
inline const char *const *owned_keys(size_t &n)
{
	static const char *const keys[] = {
		"copy_back", "history_restore", "restore_graphics_root",
		"paper_white_scale", "transfer_strength", "color_strength", "hdr_graft",
		"depth_inverted", "mvec_scale_x", "mvec_scale_y",
		"intensity", "local_tone_strength", "local_structure_strength",
		"skin_structure_strength", "style", "use_auto_mask", "ui_correction",
		// The reconfigure ladder's keys.
		"enabled", "diagnostics", "hdr_codec",
		"shader_hash", "srv_depth", "srv_velocity", "srv_colour", "uav_output",
		"mvec_decode", "mvec_reconstruct", "mvec_dilate",
		"mvec_clip_row", "mvec_clip_transpose",
		"populate_parameters", "require_trampoline",
		"rt_census", "rt_census_frames",
		// DLSS-SR and the two feature master switches.
		"dlss_nr", "dlss_sr", "dlss_chain", "sr_shader_hash", "sr_suppress_taa",
		"sr_mvec_decode", "sr_mvec_reconstruct",
		"sr_perf_quality", "sr_render_preset",
		"sr_copy_back", "sr_direct_output", "sr_use_view_rect",
		"sr_jitter_projection_only", "sr_jitter_scale_x", "sr_jitter_scale_y",
		"sr_mv_scale_x", "sr_mv_scale_y",
		"sr_hdr", "sr_mv_lowres", "sr_mv_jittered", "sr_depth_inverted",
		"sr_auto_exposure", "sr_alpha_upscaling", "sr_hw_depth",
		"sr_group_tile", "sr_out_width", "sr_out_height",
		"sr_optimal_settings",
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
			bool   oversize = false;
			while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
			{
				all.append(chunk, got);
				if (all.size() > 4u * 1024u * 1024u)   // a stray_dlssnr.ini is ~12 KB
				{
					oversize = true;
					break;
				}
			}
			std::fclose(f);

			// ABORT, do not continue with the prefix. save_ini rewrites the file ATOMICALLY from
			// `lines`, so parsing a truncated read and saving it would permanently discard
			// everything past the cutoff - comments, the identification pins (shader_hash, srv_*,
			// app_id), every key we do not own. Refusing to save is always recoverable; a
			// truncating save is not. Reported via `err` so the overlay says why.
			if (oversize)
			{
				err = "stray_dlssnr.ini is larger than 4 MiB, which it should never be. "
				      "Refusing to save rather than rewrite the file from a truncated read - "
				      "nothing has been changed on disk. Check the file.";
				return false;
			}

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
				// The two alias pairs cfg::load accepts. Either spelling in the file counts as
				// the canonical key having been written; without this the append block below would
				// add a SECOND line in the other spelling, and cfg::load takes the last one - so the
				// user's own line would silently stop being the one that counts.
				const std::string canon =
					  (kl == "colour_strength") ? std::string("color_strength")
					: (kl == "srv_color")       ? std::string("srv_colour")
					: kl;
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
	live_to_config(l, baseline());

	logf(reshade::log::level::info, "DLSS-NR overlay: saved %zu setting(s) to stray_dlssnr.ini "
	     "(rewritten in place; comments, ordering and every key the overlay does not own were preserved).", n_keys);
	return true;
}

inline bool dirty()
{
	// Start from the baseline so the fields the overlay does NOT own - app_id, ini_found, ini_path
	// - are equal by construction and only the owned ones can make this true.
	cfg::config now = baseline();
	live_to_config(live(), now);
	return !same_owned(now, baseline());
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
// [WEB] crosire/reshade source/addon_manager.cpp:155 and :505 both read the list as
// `config.get("ADDON", "DisabledAddons", disabled_addons)` off global_config(), so the section
// and key are confirmed and the FIRST probe below (runtime == nullptr, which selects
// global_config in ReShadeGetConfigValue) is the one that matches ReShade's own read. The other
// two probes are kept as harmless fallbacks; an unknown section returns false and the banner
// simply never draws.
//
// [WEB] AND THE VALUE IS NUL-SEPARATED, NOT COMMA-SEPARATED. source/addon.cpp:52-56 builds the
// returned buffer as `for (element : elements) { value_string += element; value_string += '\0'; }`.
// With two disabled add-ons the buffer is "Generic Depth@generic_depth.addon64\0STRAY
// DLSS-NR@stray_dlssnr.addon64\0". config_string() below keeps those embedded NULs (it strips
// only the TRAILING ones), which is what makes the find() correct at any position - but it also
// means the string must be flattened before it is ever handed to a %s, or the text stops at the
// first entry and the message names somebody else's add-on.
//
// [WEB] One honest limit on the detection: ReShade matches PER ELEMENT and, when an element
// contains '@', it requires the file to match too (addon_manager.cpp:505-512). A substring find
// cannot express that, so this can in principle warn on `STRAY DLSS-NR@something-else.addon64`,
// which ReShade would not treat as disabling us. That direction is the safe one for a warning,
// and it is the only direction it errs in.
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
	{
		// The value is a NUL-SEPARATED element list (see the header note). Printing it through a
		// %s as-is stops at the first entry, so with `Generic Depth@...` disabled first the one
		// warning this add-on ever prints about its own disablement would quote a DIFFERENT
		// add-on's name next to the words "this add-on" - and OVERLAY_LOG_ONCE latches, so the
		// corrected text never gets a second chance. Flatten first, and name ourselves outright.
		std::string shown = value;
		for (char &c : shown)
			if (c == '\0')
				c = ',';
		OVERLAY_LOG_ONCE(reshade::log::level::warning,
			"DLSS-NR overlay: ReShade's config lists this add-on (\"%s\") in DisabledAddons. The "
			"full list is \"%s\". It is still running for the REST OF THIS SESSION, but it will NOT "
			"load next launch and the game will run with no denoise and no warning. Re-tick it in "
			"the Add-ons tab, or remove the entry from ReShade.ini. This message is printed once.",
			addon_name, shown.c_str());
	}
	return s_answer;
}

// =============================================================================================
// DRAWING
// =============================================================================================

// Reset kinds for bump().
//
// k_reset IS NOT FREE, AND IT IS NOT THE SAFE DEFAULT. slider_f bumps on every frame
// ImGui::SliderFloat returns true, i.e. every frame of a drag - so a k_reset slider sends
// DLSSNR.Reset=1 at ~60 Hz for the whole drag and the denoiser throws its temporal accumulation
// away on every one of those frames. The user is then tuning against an image that never occurs
// in normal play: the un-accumulated first frame. Whatever value looks right there is the wrong
// value, and it changes again the instant they let go of the mouse.
//
// So k_reset is reserved for the settings whose change actually INVALIDATES the accumulated
// history - the depth convention and the motion-vector grid, where the history really was built
// under the other geometry, and where a reset per drag frame is not a distortion but the truth.
// Every plain per-evaluate scalar (intensity, the three strengths, style, use_auto_mask,
// ui_correction) is k_plain: nr_try_run rewrites all of them from g_cfg on EVERY accepted
// dispatch, so they need no machinery at all. That is exactly the split this file's header
// comment describes under "LIVE, FREE" and "LIVE + ONE RESET", and the code now matches it.
//
// THE RUNGS NEST. k_rebuild implies k_ident implies k_flush implies k_reset, and bump() raises
// every epoch at or below the rung it is given. That is what lets each consumer test exactly one
// thing: begin_pass takes the strongest rung it sees and does its consequence once, and the
// service sees the rebuild edge without having to know that a rebuild also invalidates the
// identification and the armed pristine copy.
enum : uint32_t { k_plain = 0, k_reset = 1, k_flush = 2, k_ident = 3, k_rebuild = 4 };

inline void bump(uint32_t kind)
{
	live_block &l = live();
	if (kind >= k_rebuild) l.rebuild_epoch.fetch_add(1, std::memory_order_relaxed);
	if (kind >= k_ident)   l.ident_epoch.fetch_add(1, std::memory_order_relaxed);
	if (kind >= k_flush)   l.flush_epoch.fetch_add(1, std::memory_order_relaxed);
	if (kind >= k_reset)   l.reset_epoch.fetch_add(1, std::memory_order_relaxed);
	// RELEASE last, and it is the only ordering edge in the design: every consumer loads an epoch
	// with ACQUIRE before reading any value, so everything stored above is visible to it.
	l.epoch.fetch_add(1, std::memory_order_release);
}

/// Ask the present-thread service for one or more R5 actions, naming the key that asked.
/// `why` MUST be a string literal - it crosses the thread boundary as a bare pointer and is read
/// later by the log line and the status block.
///
/// The bits are OR-MERGED, never assigned: two requests landing in the same frame must both be
/// serviced, and nr_ensure_output raises a_teardown of its own accord on a resolution change.
inline void request(uint32_t bits, const char *why, uint32_t rung = k_plain)
{
	live_block &l = live();
	// Only claim authorship when this change actually needs the service or a rung above R0. Some
	// tier-0 controls route through here for uniformity - `diagnostics` is one - and letting one
	// of those overwrite the name of a heavier change that is still queued would make the
	// reconfigure log line credit the wrong key, which is the sort of small lie that costs an
	// afternoon. Two HEAVY changes in one frame still name the later one; both are applied, and
	// the log says so by listing the tiers it climbed.
	if (bits != 0u || rung != k_plain)
		l.action_why.store(why, std::memory_order_relaxed);
	l.action_bits.fetch_or(bits, std::memory_order_release);
	bump(rung);
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
	const uint32_t raw = a.load(std::memory_order_relaxed);
	int v = static_cast<int>(raw);

	// A value outside the listed choices is PRESERVED and sent to NGX by the render path, so
	// clamping it here (the old behaviour) made the panel claim the last item while the network
	// received something else. This overlay exists to say what is actually being sent; a combo
	// that lies about it is worse than no combo. Show the real number instead, and only replace
	// it when the user deliberately picks from the list.
	if (v < 0 || v >= count)
	{
		char unknown[64];
		std::snprintf(unknown, sizeof(unknown), "%u  (not a listed value - sent as-is)", (unsigned)raw);
		ImGui::TextUnformatted(label);
		ImGui::SameLine();
		overlay_imgui::textf_colored(col::amber, "%s", unknown);
		if (help != nullptr)
			ImGui::SetItemTooltip("%s", help);
		return;
	}

	if (ImGui::Combo(label, &v, items, count))
	{
		a.store(static_cast<uint32_t>(v < 0 ? 0 : v), std::memory_order_relaxed);
		bump(kind);
	}
	if (help != nullptr)
		ImGui::SetItemTooltip("%s", help);
}

// ---------------------------------------------------------------------------------------------
// THE RECONFIGURE WIDGETS.
//
// Each of these is a normal control that additionally asks the present-thread service to make
// reality match the new value. The `why` string is a LITERAL and crosses the thread boundary as a
// bare pointer - it ends up in the reconfigure log line and in the status block, so it must name
// the KEY, in the ini's own spelling, and nothing else.
// ---------------------------------------------------------------------------------------------

/// A checkbox whose change needs the service: it raises action bits as well as a rung.
inline void checkbox_action(const char *label, std::atomic<bool> &a, uint32_t bits,
                            const char *why, uint32_t rung, const char *help)
{
	bool v = a.load(std::memory_order_relaxed);
	if (ImGui::Checkbox(label, &v))
	{
		a.store(v, std::memory_order_relaxed);
		request(bits, why, rung);
	}
	if (help != nullptr)
		ImGui::SetItemTooltip("%s", help);
}

/// An integer pin, as a slider because ImGui::InputInt is NOT on this project's CI-verified safe
/// list and SliderInt is. Every register this add-on can resolve is inside the range it is given
/// (descriptor_shadow.hpp: kMaxSrvWalk 64, kMaxUavWalk 16).
inline void slider_u32(const char *label, std::atomic<uint32_t> &a, int lo, int hi,
                       uint32_t bits, const char *why, uint32_t rung, const char *help)
{
	int v = static_cast<int>(a.load(std::memory_order_relaxed));
	// A value outside the range is SHOWN rather than clamped away, on the same argument
	// combo_u32 makes above: the render path uses whatever is in the ini, and a widget that
	// silently rewrote it would make the panel disagree with what is actually being matched.
	if (v < lo) v = lo;
	if (v > hi) v = hi;
	if (ImGui::SliderInt(label, &v, lo, hi))
	{
		a.store(static_cast<uint32_t>(v < 0 ? 0 : v), std::memory_order_relaxed);
		if (bits != 0 || rung != k_plain)
			request(bits, why, rung);
		else
			bump(k_plain);
	}
	if (help != nullptr)
		ImGui::SetItemTooltip("%s", help);
}

/// The shader hash, as text. It is a 64-bit hex identifier the user copies out of ReShade.log, so
/// a slider is the wrong shape and ImGui::InputScalar is not on the safe list either; InputText
/// plus strtoull is, and it accepts both "0x…" and decimal exactly as the ini parser does.
///
/// THERE ARE TWO OF THESE NOW - shader_hash and sr_shader_hash - so the edit buffer and the
/// "typed but not applied" flag are the CALLER'S, not function-local statics. Two controls
/// sharing one static buffer would have made typing in either one overwrite the other's display,
/// which is a control that lies about its own value. Both callers are in draw_controls, which is
/// drawn from the present thread only, so a plain static at each call site is correct.
inline void input_hash(const char *label, const char *button_label, std::atomic<uint64_t> &a,
                       const char *why, char *buf, size_t buf_n, bool &editing, const char *unapplied)
{
	const uint64_t cur = a.load(std::memory_order_relaxed);
	if (!editing)
		std::snprintf(buf, buf_n, "0x%016llx", (unsigned long long)cur);

	ImGui::SetNextItemWidth(200.0f);
	if (ImGui::InputText(label, buf, buf_n, 0, nullptr, nullptr))
		editing = true;

	ImGui::SameLine();
	if (ImGui::Button(button_label))
	{
		char *end = nullptr;
		const unsigned long long parsed = std::strtoull(buf, &end, 0);
		if (end != buf)
		{
			a.store(static_cast<uint64_t>(parsed), std::memory_order_relaxed);
			// k_ident: the per-PSO memo has to be invalidated on EVERY command list, and the
			// armed pristine copy has to be dropped, before the next dispatch is identified
			// against the new hash.
			request(0u, why, k_ident);
		}
		editing = false;
	}
	if (editing && unapplied != nullptr)
		overlay_imgui::textf_colored(col::amber, "%s", unapplied);
}

/// A read-only line for a setting that exists but cannot be changed at runtime. Greyed rather
/// than hidden, per the brief: a user must be able to see what the ini said without leaving the
/// game, and must be able to see WHY it is not editable.
inline void load_only(const char *label, const char *value, const char *why)
{
	ImGui::BeginDisabled(true);
	ImGui::TextUnformatted(label);
	ImGui::EndDisabled();
	// THE TOOLTIP IS ON THE DISABLED LABEL DELIBERATELY, and it is the same text as the indented
	// line below rather than a shorter version of it. A reason that only exists in a header
	// comment is a reason the user never reads, and the whole point of these two remaining entries
	// is that the user can tell "this needs a relaunch, and here is the specific line that says
	// why" from "this add-on is broken", at the moment they reach for the control.
	if (why != nullptr)
		ImGui::SetItemTooltip("%s", why);
	ImGui::SameLine();
	overlay_imgui::textf_colored(col::dim, "%s", value);
	if (why != nullptr)
	{
		ImGui::Indent();
		overlay_imgui::textf_colored(col::dim, "%s", why);
		ImGui::Unindent();
	}
}

inline void revert_to_baseline()
{
	config_to_live(baseline(), live());
	// k_rebuild, not k_flush: a Revert can put hdr_codec, mvec_decode, enabled or an
	// identification pin back, and each of those needs the full ladder to actually take effect.
	// Raising the strongest rung is correct here even when nothing at that rung moved - one
	// rebuild costs a couple of frames, and the alternative is a Revert button that silently
	// restores some settings and not others.
	// a_apply_populate is in the list for the same reason the rest of it is: Revert promises to
	// put EVERY control back, and populate_parameters is the one key whose value alone changes
	// nothing. A Revert that restored the checkbox and not the state it stands for would be the
	// "restores some settings and not others" the comment above rules out. It is a deliberate
	// press, like Apply, and it carries a_teardown, which is what the ON->OFF direction needs.
	request(a_teardown | a_clear_failed | a_clear_clip | a_apply_census | a_reconcile |
	        a_apply_populate,
	        "Revert to stray_dlssnr.ini", k_rebuild);
}

// ---------------------------------------------------------------------------------------------
// THE STATUS BLOCK. Drawn first, always, and never disabled.
//
// The ladder is strictest-first, and the fourth rung is the one renodx does not have:
//   the add-on is listed in DisabledAddons ....... red banner, above everything
//   enabled=0 in the ini ......................... DISABLED (this session)
//   the snippet did not load ..................... WAITING FOR NGX + the reason
//   NGX not initialised yet ...................... STANDBY
//   Init_Ext ran and failed ...................... NGX INITIALISATION FAILED + the result code
//                                                  and the one thing this panel cannot fix
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

	// EVERY age below goes through this, and the clamp is not defensive tidiness. `now` is
	// sampled above; the render thread stamps eval_ms / pass_seen_ms with its OWN GetTickCount64
	// and can do so AFTER that sample. One 15.6 ms tick landing in that window makes the stamp
	// LARGER than `now`, the uint64 subtraction wraps, and an age of 1.8e16 seconds falls out of
	// the bottom of the status ladder as "NOT EVALUATING RIGHT NOW - last evaluate
	// 18446744073709551.6 s ago" while the denoiser is in fact evaluating every frame. That is
	// the precise opposite of what this panel exists to say, so it is clamped at the source
	// rather than at each of the four call sites.
	const auto age_s = [now](uint64_t stamp) -> double {
		return (stamp == 0 || now <= stamp) ? 0.0 : (double)(now - stamp) / 1000.0;
	};
	// -1.0 keeps its meaning: "no evaluate has ever been stamped". Only the wrap is removed.
	const double   eval_age = (eval_ms == 0) ? -1.0 : age_s(eval_ms);

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

	// A FAILED RECONFIGURE IS SAID FIRST, AND IN RED, ABOVE EVERY OTHER RUNG.
	//
	// Requirement: a reconfigure that fails leaves the previous working state, logs why, and the
	// UI shows it - never a half-applied state. The first two happen in nr_service_reconfigure;
	// this is the third. It is above the ladder rather than in it because the add-on may well be
	// EVALUATING perfectly happily on the old settings, and "EVALUATING" on its own would then be
	// a true headline that answers the wrong question.
	if (!s.reconfig_ok.load(std::memory_order_relaxed))
	{
		const char *what = s.reconfig_what.load(std::memory_order_relaxed);
		ImGui::PushStyleColor(ImGuiCol_Text, col::red);
		ImGui::TextWrapped("RECONFIGURE FAILED - %s. The PREVIOUS settings are still running and "
		                   "nothing is half-applied; ReShade.log has the reason. Change the setting "
		                   "back, or press \"Reset NR feature\" to try the whole rebuild again.",
		                   what != nullptr ? what : "see ReShade.log");
		ImGui::PopStyleColor(1);
		ImGui::Separator();
	}

	if (!live_enabled())
	{
		// TWO DIFFERENT SITUATIONS WEAR THIS RUNG, and telling the user the wrong one wastes their
		// time. Before `enabled` was live there was only the first, so the old text described only
		// that; with the checkbox real, the second is now the common case and its remedy is
		// different (nothing to load - it is already loaded and merely switched off).
		if (f.snippet_loaded)
		{
			overlay_imgui::textf_colored(col::amber, "OFF - \"Load the snippet and arm NGX\" is unticked in this panel");
			ImGui::TextWrapped("The NGX feature and every texture this add-on owns have been released, "
			                   "so the VRAM is back, and the game's dispatches are issued untouched. "
			                   "The snippet itself is still loaded and NGX is still initialised - "
			                   "deliberately, so that turning this back on does not have to call "
			                   "Init_Ext a second time. Re-tick it below and everything is rebuilt on "
			                   "the next dispatch.");
		}
		else
		{
			overlay_imgui::textf_colored(col::red, "DISABLED - enabled=0 in stray_dlssnr.ini");
			ImGui::TextWrapped("The add-on is a strict no-op: no snippet was loaded and no resource "
			                   "was created. This is NO LONGER a restart-only state - tick \"Load the "
			                   "snippet and arm NGX\" below to load it now. That runs exactly the "
			                   "startup path a normal launch runs, on the present thread, so expect "
			                   "one stalled frame while a 166 MB module is loaded.");
		}
		return;
	}
	if (!f.snippet_loaded)
	{
		overlay_imgui::textf_colored(col::red, "WAITING FOR NGX - the snippet is not loaded");
		ImGui::TextWrapped("%s", f.snippet_reason[0] != '\0' ? f.snippet_reason
		                        : "nvngx_dlssnr.dll could not be loaded (no reason was recorded).");
		if (live_require_trampoline() && !f.trampoline)
			ImGui::TextWrapped("remix_nvngx.dll is required (require_trampoline=1) and was not found "
			                   "beside the add-on. Every GATED snippet export would return 0xbad00002 "
			                   "without it.");
		return;
	}
	if (!f.armed)
	{
		// TWO SITUATIONS WEAR THIS RUNG TOO, and the old text described only the recoverable one.
		//
		// If Init_Ext has already run and FAILED, it can never run again in this process: the
		// deferred initialiser is behind a one-shot latch that is set before the attempt and never
		// cleared. So "this clears itself as soon as the game renders" was a promise the code
		// could not keep, and re-ticking `enabled` reaches nothing. THE HONEST ANSWER IS A
		// RELAUNCH, and the reason and the NGX result code are given with it. This is the one
		// place in the panel where "you must restart" is the truth, and it is said plainly rather
		// than left as a spinner that never resolves.
		if (f.ngx_init_failed)
		{
			overlay_imgui::textf_colored(col::red,
				"NGX INITIALISATION FAILED - A RELAUNCH IS REQUIRED");
			// A result of 0 means the initialiser died BEFORE Init_Ext was reached (a null native
			// device, or the parameter block allocation). Printing "returned 0x00000000" there
			// would name a call that never happened.
			if (f.ngx_init_result == 0)
				ImGui::TextWrapped("NGX initialisation failed BEFORE NVSDK_NGX_D3D12_Init_Ext was "
				                   "reached - the device or the parameter block could not be "
				                   "prepared. ReShade.log has the line.");
			else
				ImGui::TextWrapped("NVSDK_NGX_D3D12_Init_Ext returned 0x%08X %s. It is attempted ONCE "
				                   "per process, on the first render-thread dispatch, behind a one-shot "
				                   "latch that is not cleared on failure - so nothing in this panel can "
				                   "retry it, and unticking and re-ticking \"Load the snippet and arm "
				                   "NGX\" will not either. That latch is deliberate: the only thing this "
				                   "project has measured about Init_Ext's fragility is that it HANGS "
				                   "when called at a moment the snippet does not tolerate, and a hang is "
				                   "not a failure that degrades.\n\n"
				                   "Fix the cause and restart the game. The two causes seen here are a "
				                   "missing or tail-jumping remix_nvngx.dll (FAIL_PlatformError - the "
				                   "gated export rejected the caller) and an add-on directory the "
				                   "process cannot write to (FAIL_UnableToWriteToAppDataPath - the "
				                   "snippet wants to put its log there). ReShade.log has the full line.",
				                   (unsigned)f.ngx_init_result,
				                   f.ngx_init_result_name != nullptr ? f.ngx_init_result_name : "");
			return;
		}
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
	else if (s.reconfig_pending.load(std::memory_order_relaxed))
	{
		// Driven by the SERVICE, not by a timestamp: this is set while nr_service_reconfigure still
		// has outstanding work and cleared when it has none. The old three-second window off
		// teardown_ms could only ever be a guess, and it guessed wrong in both directions - it
		// stayed on for three seconds after a rebuild that took one frame, and it went away while a
		// slow one was still running.
		const char *what = s.reconfig_what.load(std::memory_order_relaxed);
		overlay_imgui::textf_colored(col::amber,
			"REBUILDING - applying \"%s\": the NGX feature and our textures are being released and "
			"will be recreated on the next dispatch", what != nullptr ? what : "a setting change");
	}
	else if (tear_ms != 0 && age_s(tear_ms) < 3.0)
	{
		overlay_imgui::textf_colored(col::amber, "REBUILDING - the NGX feature is being released and recreated");
	}
	else if (pass_ms == 0)
	{
		overlay_imgui::textf_colored(col::amber, "WAITING FOR GAME DLSS - the target TAA dispatch has not been seen");
		ImGui::TextWrapped("NGX is up but no dispatch has matched shader_hash 0x%016llx with the "
		                   "configured SRV registers. Check the shader identification below, and "
		                   "ReShade.log for the one-shot \"pass did not run\" line that names the "
		                   "exact reason.", (unsigned long long)live().shader_hash.load(std::memory_order_relaxed));
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
		age_s(pass_ms));

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
	const bool codec_fresh   = (eval_ms != 0) && age_s(eval_ms) <= 0.25;
	if (!live_hdr_codec())
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
// THE SEQLOCK'S WRITE SIDE. Held for the whole of draw_controls, which is the only function that
// mutates live_block after seeding.
//
// ODD means "a value in the block may be mid-change"; even means the block is settled.
// begin_pass reads the counter before and after its snapshot and discards a snapshot taken across
// an edit, which is what makes a multi-field change - a Revert, or a retyped shader_hash and
// srv_colour together - impossible to observe half way. See live_block::ui_seq for the argument,
// and for why the epoch alone never gave that guarantee.
//
// RAII, because draw() catches exceptions from ImGui and the counter must return to even on that
// path too - an odd counter left behind would make every later snapshot spin its eight attempts
// and fall back to the previous configuration, i.e. it would freeze the whole panel silently.
// RELEASE on both edges: the entry so the reader cannot hoist its loads above it, the exit so
// every value stored inside is visible to a reader that sees the even count.
// ---------------------------------------------------------------------------------------------
struct ui_edit_guard
{
	ui_edit_guard()  { live().ui_seq.fetch_add(1, std::memory_order_release); }
	~ui_edit_guard() { live().ui_seq.fetch_add(1, std::memory_order_release); }
	ui_edit_guard(const ui_edit_guard &) = delete;
	ui_edit_guard &operator=(const ui_edit_guard &) = delete;
};

// ---------------------------------------------------------------------------------------------
// THE CONTROLS. Layout and labels mimic renodx's "DLSS 5 Neural Rendering" panel; the section
// headings are its own strings where they still describe what we do.
//
// EVERY STORE INTO live_block BELOW IS INSIDE THE SEQLOCK. The guard is the first statement, so a
// future control added anywhere in this function is covered without its author having to know.
// ---------------------------------------------------------------------------------------------
inline void draw_controls(const host_facts &f)
{
	ui_edit_guard edit_guard;

	live_block &l = live();
	const status_block &s = status();

	// renodx disables NOTHING, so its sliders stay interactive when NGX never loaded and a user
	// can spend a while tuning something that reaches nothing. That is a defect, not a style
	// choice, and it is not copied.
	const bool usable = f.valid && live_enabled() && f.snippet_loaded && f.armed;

	// THREE SCOPES, NOT ONE, AND THE SPLIT IS A FIX RATHER THAN A TIDY-UP.
	//
	// `usable` above is a statement about DLSS-NR and only about DLSS-NR: f.snippet_loaded is
	// g_snippet.available, i.e. nvngx_dlssnr.dll. One BeginDisabled(!usable) used to run from the
	// "DLSS 5 Neural Rendering" separator all the way past the identification block, which put the
	// whole DLSS-SR section inside it - and ImGui's disabled state is a stack that a nested
	// BeginDisabled(false) does not cancel, so on the merge's own headline configuration
	// (dlss_nr=0, dlss_sr=1) every SR control was inert. That configuration never loads the NR
	// snippet by design, so `usable` is false however perfectly DLSS-SR armed. The user got a
	// greyed-out dlss_sr checkbox - the control whose OFF direction is the fully live one and the
	// one an A/B actually reaches for - with no explanation, because the hint line above is gated
	// on snippet_reason and no load was attempted, and no way back to dlss_nr=1 short of editing
	// the ini by hand.
	//
	// sr_section_live is deliberately NOT "SR is armed". Ticking dlss_sr while nvngx_dlss.dll was
	// never loaded is the documented save-and-relaunch path, and the amber status line right under
	// the box tells the user to do exactly that - so the box has to be clickable in precisely the
	// state where SR is not running. What must NOT be clickable in that state is everything
	// downstream of a live SR feature, and that is the inner BeginDisabled the section already has.
	const bool sr_usable       = f.valid && live_enabled() && f.sr_snippet_loaded && f.sr_armed;
	const bool sr_section_live = f.valid && live_enabled();
	// Identification is SHARED and always was: want_hash() folds shader_hash, dlss_sr and
	// sr_shader_hash into one value read at nr_try_run BEFORE the NR/SR branch is taken, and the
	// srv_/uav_ registers resolve the same descriptors for both features. Gating it on the NR
	// snippet meant a pure-SR run could not re-pin the shader it was failing to identify, which is
	// the one control that state needs. Either feature being armed is enough.
	const bool ident_usable    = usable || sr_usable;

	// ---- THE ADD-ON ITSELF ---------------------------------------------------------------------
	// OUTSIDE the BeginDisabled below, and that is the whole point of it. Everything else greys out
	// when NGX is not armed; this is the control that ARMS it, so greying it out with the rest
	// would leave exactly one situation - the one where the ini said enabled=0 - in which the panel
	// offers no way out and tells the user to restart the game. That was the old behaviour and it
	// is what this ladder exists to remove.
	ImGui::SeparatorText("Add-on");
	{
		bool on = l.enabled.load(std::memory_order_relaxed);
		if (ImGui::Checkbox("Load the snippet and arm NGX  (ini: enabled)", &on))
		{
			l.enabled.store(on, std::memory_order_relaxed);
			// a_reconcile does BOTH directions: on, the service runs the shipping startup path -
			// ngx::load_snippet then g_nr_pending_init, after which the deferred lazy init on the
			// next dispatch does Init_Ext exactly as it does at every normal launch. Off, it tears
			// the feature and every texture down so the VRAM goes back.
			request(a_teardown | a_reconcile, "enabled", k_rebuild);
		}
		ImGui::SetItemTooltip(
			"Live in BOTH directions, and it is the ini's `enabled` key - not the per-dispatch "
			"bypass below.\n\n"
			"ON runs the SHIPPING STARTUP PATH, not a new one: ngx::load_snippet, then "
			"g_nr_pending_init, which the render thread's existing deferred initialiser consumes on "
			"the next dispatch to call Init_Ext. That is the same two steps every normal launch "
			"takes. It happens on the PRESENT thread because it LoadLibraryW's a 166 MB module, "
			"which has no business on a command-list recording thread - so EXPECT ONE STALLED "
			"FRAME when you tick this.\n\n"
			"OFF releases the NGX feature, every view and every texture on the next present, so the "
			"VRAM goes back. The snippet module itself stays loaded: this tree has no in-process "
			"unload path (stray_dlssnr.cpp:4339-4341 declines to FreeLibrary even at device "
			"teardown), and unloading a module that may still hold worker threads to save address "
			"space would be a bad trade. Nothing on the render path reads it once it is off.\n\n"
			"THE ONE CASE WHERE ON REACHES NOTHING, and the panel says so above rather than "
			"pretending: if Init_Ext has already been attempted and FAILED in this session, the "
			"deferred initialiser is behind a one-shot latch that is not cleared on failure, so it "
			"cannot run again. The status block shows NGX INITIALISATION FAILED with the result "
			"code, the reconfigure is reported as FAILED rather than APPLIED, and a relaunch is "
			"the only fix.");

		if (!f.snippet_loaded && f.snippet_reason[0] != '\0')
		{
			ImGui::Indent();
			overlay_imgui::textf_colored(col::dim, "the last load attempt said: %s", f.snippet_reason);
			ImGui::Unindent();
		}
	}

	if (ImGui::Button("Reset NR feature"))
	{
		// Through the SERVICE now, not through begin_pass. begin_pass only runs when the pass is
		// actually being reached - so the old routing meant this button, whose entire purpose is
		// "something has wedged", did nothing in precisely the case it exists for. The service
		// runs from on_present unconditionally.
		request(a_teardown | a_clear_failed | a_clear_clip | a_reconcile,
		        "Reset NR feature", k_rebuild);
	}
	ImGui::SetItemTooltip(
		"Releases the NGX feature, every view and every texture on the next present (on the main "
		"thread, after the queue is idle), clears the latched create-failure AND the latched "
		"View-CB / ClipToPrevClip failures, and rebuilds everything on the following dispatch. "
		"This is the control to reach for when something has wedged - it is the single most useful "
		"button in the reference add-on too, and unlike the reference it works even when the pass "
		"is no longer being reached at all.");

	ImGui::SameLine();
	ImGui::BeginDisabled(!dirty());
	if (ImGui::Button("Revert to stray_dlssnr.ini"))
		revert_to_baseline();
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Puts every control below back to the value that was on disk at load, or "
	                      "at the last Save, and runs one full rebuild so that the settings which "
	                      "need one actually take effect. Live, like any other change.");

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
			// recycle it (stray_dlssnr.cpp:3206-3210).
			bump(k_flush);
		}
		ImGui::SetItemTooltip(
			"Live, per dispatch, and it is the CHEAP switch: the add-on still identifies the TAA "
			"pass and then lets ReShade issue the game's own dispatch untouched. A strict no-op, "
			"reversible in one click, with nothing released and nothing rebuilt.\n\n"
			"The `enabled` checkbox above is the expensive one - it unloads NGX and gives the VRAM "
			"back. Both are real and both are live; this is the one to use for an A/B.");
	}

	// ---- network tuning ----------------------------------------------------------------------
	ImGui::SeparatorText("Network");

	{
		// The three NAMES are genuinely the reference add-on's - all three are in
		// renodx-reference.addon64's string table. What is NOT established is that styles 1 and 2
		// exist in THIS snippet build: nothing in this tree has ever MEASURED 1 or 2. The binary
		// does now show two enabled keyed sub-entries carrying keys 1 and 2 with non-zero local-tone
		// masks (0x1800b0de4 / 0x1800b0e28), which is a reason to try them and NOT a measurement of
		// them - the labels say exactly that much and no more. They are not hidden, because DLSSNR.Style itself is real, live and
		// written every evaluate - the INDEX is what is unverified, not the parameter - but a user
		// who picks one and sees nothing must be able to tell "this build has no such style" from
		// "the add-on is broken", and only the label can tell them that at the moment they click.
		static const char *const style_items[] = {
			"0 - Default (local tone mask 0x00000000 - moves nothing)",
			"1 - Natural (a keyed sub-entry with mask 0x34 exists; UNMEASURED)",
			"2 - Cinematic (a keyed sub-entry with mask 0x20 exists; UNMEASURED)",
		};
		combo_u32("NR Style", l.style, style_items, 3, k_plain,
			"DLSSNR.Style is written on every evaluate and is NOT baked at CreateFeature, so the "
			"parameter itself is real. Only the INDEX reaches the snippet, and nobody here has "
			"MEASURED 1 or 2 on hardware.\n"
			"WHAT THE BINARY SAYS. There is exactly one style RECORD: 0x180023bb0 is cmp rcx,1 / jae "
			"-> xor eax,eax, so an index of 1 or 2 into that table returns NULL. But inside the one "
			"record there are eight KEYED SUB-ENTRIES, and two of them are enabled: key 1 at "
			"0x1800b0de4 (local-tone mask 0x34) and key 2 at 0x1800b0e28 (mask 0x20). The record's "
			"own default mask, at 0x1800b0da8, is 0x00000000.\n"
			"THIS IS WHY LOCAL TONE STRENGTH DOES NOTHING AT STYLE 0 - read its tooltip. If any "
			"index makes that slider bite, it is 1 or 2. NOT ESTABLISHED: that DLSSNR.Style is the "
			"value compared against those sub-entry keys rather than the separate record-id compare "
			"at 0x18001d8b3. Trying one is safe - none of the paths can crash - but do not read "
			"\"no visible change\" as a measurement of that style.");
	}

	// THE SLIDER RANGE IS [0,1], AND IT IS NOT A FIX - IT IS HONESTY ABOUT THE DOMAIN.
	// A previous revision narrowed these from 0..2 and called the range "the bug". It was not:
	// narrowing a slider removes reachable values and adds none, so it cannot make a dead control
	// live. What [0,1] buys is that no position on these sliders is indistinguishable from its
	// neighbour. The ini is NOT clamped, so any value remains reachable by hand.
	//
	// NOT ONE OF THE FIVE CONTROLS IN THIS SECTION IS LABELLED "Live", AND THAT IS DELIBERATE.
	// Three rounds of analysis each proved REACHABILITY and shipped it as LIVENESS, and each was
	// caught by the next round's verifier. What is actually proven for each is spelled out in its
	// own tooltip, in the text the user reads, with the disassembly it rests on. Where a gate
	// cannot be observed from this side of the NGX call, the tooltip says so and tells the user to
	// trust the image instead. "Conditional" is the correct label for a control whose consumer we
	// have located but never watched execute. addon_config.hpp carries the long-form derivation.
	slider_f("NR Intensity", l.intensity, 0.0f, 1.0f, "%.2f", k_plain,
		"An ATTENUATION, not a gain - and 1.0 (the default, the top of this slider) is FULL "
		"denoise, not 'off'. DRAG DOWN to reduce denoising.\n"
		"PROVEN: every value AT OR ABOVE 1.0 drives byte-identical code, which is the only reason "
		"this slider stops at 1.0. fn 0x18001d4d0 is a MODE SELECTOR returning 0, 1 or 3 - NOT a "
		"boolean - and it reads the value at [rcx+0xe0]:\n"
		"    0001d50a  comiss xmm0, dword [rbx+0xe0]   ; 1.0 vs Intensity\n"
		"    0001d511  ja     0x18001d521              ; taken iff Intensity < 1.0\n"
		"    0001d513  add    rbx, 0x60\n"
		"    0001d517  cmp    qword [rbx], 0           ; the ControlMask pointer\n"
		"    0001d51b  jne    0x18001d525\n"
		"    0001d51d  xor    al, al                   ; >=1.0 AND no ControlMask -> mode 0\n"
		"    0001d521  add    rbx, 0x60                ; <1.0 -> al = 1 unconditionally\n"
		"    0001d525  mov    al, 1\n"
		"    0001d52f  cmp    qword [rbx], 0\n"
		"    0001d53d  cmovne eax, ecx                 ; ControlMask bound -> 3 for EVERY value\n"
		"PROVEN HERE: qword [rcx+0x60] is the ControlMask pointer - 0x18001aa4b tests the same "
		"slot to force UseAutoMask off - and it is NULL on this deployment, because this add-on "
		"writes DLSSNR.ControlMask as an explicit NULL and a null simply lands. So the cmovne's "
		"non-null branch cannot be taken here, and dragging below 1.0 really does move the mode "
		"from 0 to 1.\n"
		"NOT CONFIRMED: that mode 1 changes the image. 0x18001f500 puts the mode through a BACKEND "
		"CAPABILITY QUERY before anything runs - 0x18001f522 call 0x1800295e0, which does "
		"lea ecx,[rbx-1] / bt eax, mode-1 against a bitmask fetched through a vtable at "
		"0x1800295ef. If the backend does not advertise bit 0, the query returns false, the flag "
		"at 0x1800191bd stays clear, and the one extra dispatch it gates [0x18001978c je, skipping "
		"call 0x180023080 at 0x1800197d9] never issues. That false is NOT an abort - an earlier "
		"revision read it as one and called this control dead. Nothing on this side of the NGX "
		"call can read that capability bit, so drag down and TRUST YOUR EYES.");

	ImGui::SeparatorText("Local tone - style-gated, inert at the default style");

	slider_f("Local Tone Strength", l.local_tone_strength, 0.0f, 1.0f, "%.2f", k_plain,
		"INERT AT THE SHIPPED DEFAULT STYLE. Expect no image change from this slider until Style "
		"selects a keyed sub-entry, and even then it moves at most 3 of its 14 parameters.\n"
		"The snippet does clamp the value to [0,1] itself [0x18001d603 comiss / 0x18001d60a "
		"movaps / 0x18001d614 xorps], so anything at or above 1.0 is byte-identical to 1.0. But "
		"THE CLAMP IS NOT THE POINT. Every one of the 14 lerps it feeds is gated by a per-style "
		"BITMASK, loaded by the instruction an earlier revision quoted around:\n"
		"    0001d603  comiss xmm2, xmm3\n"
		"    0001d606  mov    eax, dword [rdx]   ; <- THE MASK\n"
		"    0001d608  jbe    0x18001d60f\n"
		"and then tested bit by bit - test al,1 / test al,2 / test al,4 ... bt eax,0xd - before "
		"each store into [rcx+0x124..0x158]. A ZERO MASK STORES NOTHING.\n"
		"There is exactly ONE style record. 0x180023bb0 is cmp rcx,1 / jae -> xor eax,eax, so "
		"every index above 0 returns NULL, and 0x1800239a0 scans 0x1800b0d80..0x1800b1008 at "
		"stride 0x288 - one iteration. Its DEFAULT mask block, used whenever no keyed sub-entry "
		"matches at 0x18001d8d6-0x18001d8e9, is dword [0x1800b0da8] = 0x00000000.\n"
		"Of its eight sub-entries only TWO are enabled (flag byte non-zero at 0x18001d8db): key 1 "
		"at 0x1800b0de4 carries mask 0x34 (bits 2,4,5 -> [rcx+0x12c], [rcx+0x134], [rcx+0x138] - "
		"three of the fourteen) and key 2 at 0x1800b0e28 carries mask 0x20 (bit 5 -> [rcx+0x138] "
		"- one of the fourteen). The other six carry flag byte 0 and are skipped.\n"
		"NOT ESTABLISHED: that DLSSNR.Style is the value compared against those sub-entry keys. "
		"The key is r9d at the call site 0x18001d8f2 and the record id is a separate compare "
		"against [record+8] at 0x18001d8b3; which of the two DLSSNR.Style drives has not been "
		"traced. What IS established is that the default path stores 0 of 14.\n"
		"A previous revision called this range 'PROVEN' and the control 'Live'. It had proven the "
		"CLAMP and reported it as LIVENESS.");

	const bool mask_on = l.use_auto_mask.load(std::memory_order_relaxed);

	// THE THREE CONTROLS BELOW SHARE ONE UNRESOLVED GATE. See addon_config.hpp: the only code
	// that consumes the effective structure pair sits behind two dynamic_cast null tests. Both
	// were re-read instruction by instruction against the deployed binary:
	//
	//   GATE 1  0021cb4  lea  r9, [0x1811412d8]     ; dst .?AVCCNetwork@HNetCpp@@
	//           0021cbb  lea  r8, [0x1811412b0]     ; src .?AVNetwork@HNetCpp@@
	//           0021cc2  xor  edx, edx
	//           0021cc4  mov  rcx, qword [rdi+0x48]
	//           0021cc8  call 0x18007f5cc           ; __RTDynamicCast
	//           0021ccd  mov  rbx, rax
	//           0021cd0  mov  qword [rsp+0xe0], rax
	//           0022537  mov  rcx, qword [rsp+0xe0]
	//           002253f  test rcx, rcx
	//           0022542  je   0x18002263b           ; skips the whole consumer block
	//           0022548  movss xmm5, dword [r14+0xfc]
	//
	//   GATE 2  003f5d0  lea  r9, [0x1811414b0]     ; dst .?AVCCTinlayoutFusedPreBlockSwin1HLayer
	//           003f5df  lea  r8, [0x181141300]     ; src .?AVLayer@HNetCpp@@
	//           003f5e8  call 0x18007f5cc           ; __RTDynamicCast
	//           003f5ed  mov  rsi, rax
	//           003f5f0  test rax, rax
	//           003f5f3  je   0x18003f68e           ; skips the call to the store at 0x180061700
	//
	// If either cast returns null, all three controls are inert TOGETHER and no value of any of
	// them does anything. Whether the shipped model satisfies them is not settled from the binary
	// alone, so these stay "conditional". Mechanism identified; firing NOT confirmed.
	ImGui::SeparatorText("Structure - conditional, see tooltips");

	ImGui::BeginDisabled(!mask_on);
	slider_f("Local Structure Strength", l.local_structure_strength, 0.0f, 1.0f, "%.2f", k_plain,
		"CONDITIONAL - MECHANISM IDENTIFIED, FIRING NOT CONFIRMED. Two things must hold for this "
		"to do anything, and only the first is observable from here.\n"
		"(1) Automatic Mask must be on: with it off the snippet substitutes -1.0f for BOTH "
		"structure strengths [0x18001aa60 je -> 0x18001aa84] and neither knob matters.\n"
		"(2) THE LOADED MODEL MUST MATCH. The only site that reads the effective value is behind "
		"two dynamic_cast null tests - dst .?AVCCNetwork@HNetCpp@@ at 0x180021cc8, tested at "
		"0x18002253f, and dst .?AVCCTinlayoutFusedPreBlockSwin1HLayer@HNetCpp@@ at 0x18003f5e8, "
		"tested at 0x18003f5f3 (both type names read out of the descriptors at 0x1811412d8 and "
		"0x1811414b0). If either returns null, the je skips the store outright. That is the "
		"leading explanation for this control being reported dead from hardware, and it is NOT "
		"something a slider can fix. THE EXACT DESTINATION SLOT IS UNCONFIRMED - do not trust any "
		"claim about which of skin/local lands where until an A/B says so.\n"
		"RANGE: [0,1] is convention, not a measured clamp - 0x180061710 stores the value raw. "
		"-1.0f is the snippet's own disabled sentinel and out-of-range conditioning of a trained "
		"network is undefined rather than 'more'. The ini is unclamped if you want to test that.");

	{
		bool inherit = l.skin_structure_strength.load(std::memory_order_relaxed) < 0.0f;
		if (ImGui::Checkbox("Skin Structure: inherit Local Structure", &inherit))
		{
			l.skin_structure_strength.store(inherit ? -1.0f : 1.0f, std::memory_order_relaxed);
			bump(k_plain);
		}
		ImGui::SetItemTooltip(
			"A NEGATIVE skin structure strength means \"use the local structure strength\": the "
			"snippet does an explicit comiss against 0 [0x18001aa6d] and a jae, and copies the "
			"local value on the other branch [0x18001aa72]. This checkbox is why there is no "
			"slider position that means it - 0.0 is NOT neutral, it FLATTENS skin structure, and "
			"a bare 0..1 slider would put that trap one drag from the left edge. Any negative "
			"value behaves identically; the checkbox writes -1.0.");

		ImGui::BeginDisabled(inherit);
		slider_f("Skin Structure Strength", l.skin_structure_strength, 0.0f, 1.0f, "%.2f", k_plain,
			"CONDITIONAL - MECHANISM IDENTIFIED, FIRING NOT CONFIRMED, on exactly the same two "
			"gates as Local Structure Strength - read that "
			"tooltip first. 0.0 flattens skin structure; it is not a bypass. Untick \"inherit\" to "
			"reach it. Same range note: [0,1] by convention, not by a clamp measured in the "
			"snippet.");
		ImGui::EndDisabled();
	}
	ImGui::EndDisabled();

	checkbox_b("Automatic Mask", l.use_auto_mask, k_plain,
		"CONDITIONAL - MECHANISM IDENTIFIED, FIRING NOT CONFIRMED. On its own this only CHOOSES "
		"what the two structure strengths are set to: "
		"off substitutes -1.0f for both [0x18001aa59 cmp / 0x18001aa60 je -> 0x18001aa84], on "
		"passes them through with the negative-skin inherit rule. Both outcomes are written to "
		"the same two slots, so if the model gates described in the Local Structure tooltip do "
		"not hold, this checkbox has nothing to change either - which is why all three were "
		"reported dead together. This add-on binds no ControlMask, so the snippet's other route "
		"to forcing the mask off [0x18001aa4b cmp / 0x18001aa52] never fires here.");


	{
		// PRESENT, and unlike the other three renodx extras this one is real. `DLSSNR.UICorrection`
		// IS in this snippet's string table (measured with an exact-line strings match against
		// nvngx_dlssnr.dll: one hit, against zero for `DLSSNR.Upscaling`), and it is read as
		// Get(const char*, int*) with a proper 0xbad00000 guard and a fallback of 0. Its VISUAL
		// EFFECT on STRAY's content is unverified - hence the wording of the tooltip. Stored as a
		// u32 because the parameter block converts between the numeric Set/Get overloads
		// (ngx_interop.hpp), so a u32 Set is readable by the snippet's int Get.
		//
		// THE WRITE SITE IS stray_dlssnr.cpp:2834. Do not delete it: without it this checkbox is a
		// control that does nothing, which is the failure this whole panel is built to avoid.
		bool on = l.ui_correction.load(std::memory_order_relaxed) != 0u;
		if (ImGui::Checkbox("NR UI Correction", &on))
		{
			l.ui_correction.store(on ? 1u : 0u, std::memory_order_relaxed);
			bump(k_plain);
		}
		ImGui::SetItemTooltip(
			"DELIVERED - which is not the same as live. It is written into the parameter block on "
			"every evaluate, alongside Style, and DLSSNR.UICorrection is a genuine parameter of THIS "
			"snippet build: it is in the string table and the snippet reads it with a failure guard, "
			"defaulting to 0. Both of those are MEASURED.\n"
			"What has NOT been traced is the consumer - nobody here has followed the value from that "
			"read to anything that touches the image, and nobody has verified what it looks like on "
			"STRAY. Treat it as a diagnostic knob rather than a tuning one, and do not read \"no "
			"visible change\" as proof it is broken. It is nothing to do with this settings panel, "
			"despite the name.");
	}

	// ---- colour transfer ---------------------------------------------------------------------
	// renodx's own section heading, kept because it still describes exactly what these do.
	ImGui::SeparatorText("Control-compatible color transfer");

	{
		const bool codec_live = s.codec_running.load(std::memory_order_relaxed);
		if (!live_hdr_codec())
			overlay_imgui::textf_colored(col::amber,
				"The HDR codec is off, so these three do nothing until it is turned back on.");
		else if (!codec_live)
			overlay_imgui::textf_colored(col::amber,
				"The codec is not running at the moment, so these three are not reaching the image.");

		// Greyed when the codec cannot run at all, not merely when it is not running THIS frame:
		// all three are consumed only by the encode and the decode, so with the codec latched off
		// they reach nothing. renodx leaves its equivalents interactive in the same situation.
		ImGui::BeginDisabled(!live_hdr_codec() || s.codec_failed.load(std::memory_order_relaxed));

		slider_f("Scene Paper-White Scale", l.paper_white_scale, 0.05f, 16.0f, "%.3f", k_plain,
			"Live. The 1.333 default is DERIVED, NOT MEASURED ON HARDWARE: it puts UE4's diffuse white "
			"(SceneColor 1.0) exactly on the soft-clip knee, which is also 1/0.79 - the measured point "
			"out to which at least 95 per cent of the network's requested change survives the FP16 round "
			"trip - and it moves the clip above which the transfer can only DARKEN from SceneColor 1.81 "
			"to 2.41. Remix "
			"folds its own auto-exposure and EV bias into this and STRAY exposes no equivalent, so it is "
			"still a constant and still worth a sweep here. "
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
			"colour cast.\n\n"
			"NOT ORTHOGONAL TO THE GRAFT BELOW, BUT NOT A CONTROL FOR IT EITHER. At 0.0 both grafts "
			"reduce to \"rescale the original to the new luminance\" and their new luminances are the "
			"same number, so on ORDINARY pixels the two modes agree to well under one 8-bit code "
			"value. They do NOT agree in SHADOWS: mode 0 has a chroma floor that hands a near-black "
			"pixel over to the network's own colour below Y = 0.001/s, and mode 1 has no such floor "
			"at all, so it keeps the original's chromaticity and rescales it by an unbounded ratio. "
			"Measured over 400,000 dark chromatic pixels at 0.0: worst 27.6 code values, 42.5%% of "
			"them differing by 2 or more.\n\n"
			"So A/B the grafts at 1.0 for the HIGHLIGHT difference and at 0.0 on a DARK, COLOURED "
			"area - shadowed alley walls, unlit interiors - for the shadow difference. Neither "
			"setting shows both.");

		{
			// The graft-back selector. Both items are REAL and both are exercised by the same
			// dispatch - this is a root constant the decode reads, not a second code path that
			// could be left uncalled.
			//
			// TWO WAYS THE DECODE IN HAND CANNOT HONOUR IT, both said out loud rather than left
			// for the user to discover by seeing nothing change. Neither is hypothetical: the
			// first is what keeps a compiler that cannot build the OkLab matrices from taking the
			// DEFAULT graft down with it, and the second is the documented workflow for a machine
			// with no working D3DCompile at all.
			const bool graft_avail   = s.codec_graft_available.load(std::memory_order_relaxed);
			const bool decode_overri = s.codec_decode_overridden.load(std::memory_order_relaxed);
			if (!graft_avail)
				overlay_imgui::textf_colored(col::amber,
					"The decode was built WITHOUT the reference graft (it did not compile here), so "
					"hdr_graft is pinned to 0 for this run. Mode 0 is unaffected - see ReShade.log.");
			else if (decode_overri)
				overlay_imgui::textf_colored(col::amber,
					"A user-supplied stray_dlssnr_decode.dxbc is in use. If it was not built from "
					"this add-on's shader source it does not read hdr_graft at all, and this control "
					"will do nothing however it reads. Delete the .dxbc to be sure.");

			static const char *const graft_items[] = {
				"0 - Additive residual (ours, default)",
				"1 - renodx UpgradeToneMap (OkLab hue lock)",
			};
			ImGui::BeginDisabled(!graft_avail);
			combo_u32("HDR Graft", l.hdr_graft, graft_items, 2, k_plain,
				"Live, and free: it is one root constant in the decode's constant block, so the very "
				"next frame uses the other graft. Nothing is recreated.\n\n"
				"THE ENCODE IS THE SAME EITHER WAY. Same exact piecewise sRGB, same soft-clip knee "
				"0.75 and shoulder 5.770780, so the network is shown the same proxy and returns the "
				"same answer. Only the graft-back differs.\n\n"
				"0 - ADDITIVE. result = original + (neural - proxy) / s. A scene-linear residual, "
				"exactly +0.0 when the network asked for nothing - which is what makes "
				"transfer_strength=0 a BIT-EXACT no-op at every paper_white_scale. RGB is scaled "
				"uniformly, so the original's hue cannot drift.\n\n"
				"1 - RENODX. Rebuilds the pixel from the network's answer: ratio = (neural_y + "
				"max(0, original_y - proxy_y)) / neural_y, then HueOkLab(neural * ratio, neural) "
				"with an AP1 negative clamp. Recovered verbatim from renodx-reference.addon64's own "
				"embedded HLSL.\n\n"
				"WHAT THE TRADE ACTUALLY IS - measured, not assumed. Luminance is linear, so their "
				"headroom term max(0, original_y - proxy_y) is ALGEBRAICALLY our additive residual: "
				"both modes deliver the SAME luminance gain at every source magnitude. The entire "
				"difference is CHROMA. Where the soft clip has crushed the proxy to white the "
				"network's answer is neutral, so mode 1 hue-locks to that neutral and drags a "
				"clipped highlight toward the white point; mode 0 leaves its chromaticity exactly "
				"alone. On a bright neon sign that is the difference between keeping its colour and "
				"washing it out.\n\n"
				"THE OTHER HALF OF THE DIFFERENCE IS IN THE SHADOWS, and it shows at Color Strength "
				"0.0 rather than 1.0. Mode 0's chroma floor hands a near-black pixel over to the "
				"network's colour below Y = 0.001/s; mode 1 has no floor and rescales the "
				"original's chromaticity by an unbounded ratio. 400,000 dark chromatic pixels at "
				"0.0: worst 27.6 code values, 42.5%% of them 2 or more.\n\n"
				"SO THIS IS A COLOUR EXPERIMENT, NOT A HIGHLIGHT FIX. Neither mode recovers a "
				"bright highlight, and the ceiling is LOWER than the soft clip suggests because "
				"the proxy is stored in an FP16 surface: the encoded proxy quantises to exactly "
				"1.0 at 1.81x paper white (the 3.47x figure is FP32 and is not the one that "
				"governs), and of a requested +30%% gain the decode already delivers only ~50%% at "
				"1.15x and ~5%% at 1.86x. Those are ratios TO paper white and do not move with "
				"Scene Paper-White Scale - what moves is the scene-linear magnitude they land at, "
				"in proportion (at 4.0, full gain reaches magnitude 3.17 instead of 0.79). If "
				"highlights are being lost, the knee is in the wrong place, and that slider above "
				"is exactly the right thing to raise.\n\n"
				"Mode 1 is also NOT an exact bypass at transfer_strength=0: it works display-"
				"referred throughout, so the result is (original * s) / s, exact only when s is a "
				"power of two. Mode 0 is exact at every value, which is why it is the default and "
				"why the identity A/B should be run on it.");
			ImGui::EndDisabled();
		}

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

		// These two keep k_reset even though slider_f bumps on every frame of a drag - the only
		// two sliders that do. It is not the oversight the tuning sliders were: each intermediate
		// value really IS a different motion-vector grid from the one the history accumulated
		// against, so a reset on each of those frames is the correct answer rather than a
		// distortion of what the user is looking at. The tooltip says so plainly instead of
		// promising "one" reset, which is only true if you click rather than drag.
		ImGui::BeginDisabled(automatic);
		slider_f("Motion Scale X", l.mvec_scale_x, 0.05f, 4.0f, "%.3f", k_reset,
			"Live, and it forces a DLSSNR.Reset frame on EVERY frame the value moves - so a drag "
			"resets continuously and the image will look un-accumulated until you let go. That is "
			"correct here: each value in between is a different grid from the one the history was "
			"built on. Overrides the derived colour/mvec grid ratio.");
		slider_f("Motion Scale Y", l.mvec_scale_y, 0.05f, 4.0f, "%.3f", k_reset,
			"Live, and it forces a DLSSNR.Reset frame on EVERY frame the value moves - so a drag "
			"resets continuously and the image will look un-accumulated until you let go. That is "
			"correct here: each value in between is a different grid from the one the history was "
			"built on. Overrides the derived colour/mvec grid ratio.");
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

	// ---- the HDR codec, live in BOTH directions ------------------------------------------------
	//
	// THE HARD ONE, and the reason the rebuild rung exists. Two things used to block it, one per
	// direction, and both are gone:
	//
	//   OFF -> ON  was blocked by ONE LINE. nr_lazy_ngx_init's else branch set st->codec_failed =
	//              true "not a failure, but the same do-not-use-it state", and
	//              nr_release_feature_and_output deliberately never clears codec_failed. So
	//              hdr_codec=0 at load latched a RUN-LATCHED SHADER-BUILD FAILURE flag to mean
	//              "the user configured it off", and nothing could ever undo it. That assignment
	//              is deleted; codec_failed now means only what its own comment says it means,
	//              and the service builds the pipelines lazily on the first ON.
	//   ON -> OFF  was blocked by the FORMAT. out_tex is forced to r16g16b16a16_float for its
	//              LIFETIME whenever the codec is on, so with the codec off it becomes the
	//              copy-back source and its format no longer matches an r11g11b10_float TAA
	//              output - and the copy-back guard then SILENTLY skips, which reads as "no
	//              denoise on screen" while every other indicator stays healthy. The teardown
	//              fixes it with no new code at all: releasing out_tex zeroes out_w/out_h, so the
	//              next accepted dispatch re-enters nr_ensure_output on the create branch and
	//              re-decides the format against the new value.
	checkbox_action("HDR codec (feed the network a display-referred proxy)", l.hdr_codec,
		a_teardown | a_reconcile, "hdr_codec", k_rebuild,
		"Live in BOTH directions, and it costs one full rebuild each way - the NGX feature and "
		"every texture are released on the next present and recreated on the following dispatch, "
		"because the network's target texture has to be re-created in a DIFFERENT FORMAT. Expect a "
		"visible hitch and one un-accumulated frame.\n\n"
		"ON: the pass builds a soft-clipped exact-piecewise-sRGB proxy, hands the PROXY to the "
		"network as DLSSNR.Color, and carries the answer back onto the untouched original as an "
		"additive residual. DLSS-NR is a display-referred network and UE4 SceneColor is linear and "
		"unbounded, so this is what fixes README gap 1 - the darkening.\n\n"
		"OFF: the raw TAA output is bound as DLSSNR.Color and the network's raw answer is copied "
		"straight back. That is the A/B, and it is a genuinely different image - it is NOT the same "
		"as HDR Transfer Strength = 0, which is an exact bypass of the DENOISE with the codec still "
		"running.");

	checkbox_b("Restore the graphics root signature too", l.restore_graphics_root, k_plain,
		"Live, and safe to change mid-scene ONLY because of the snapshot at the top of each pass. "
		"This value is read TWICE per pass - once by capture_state and once by restore_state - and "
		"a true-to-false tear between them would leave the descriptor heaps re-bound, the compute "
		"root signature set (which invalidates every graphics root argument) and the graphics tables "
		"never replayed: exactly the corruption this defaulting ON exists to prevent. NVIDIA's own "
		"Streamline shadow does not track graphics root state, but a descriptor-heap change "
		"invalidates graphics tables too, so leave it on unless you are measuring.");

	// ---- motion vectors --------------------------------------------------------------------------
	ImGui::SeparatorText("Motion vectors (README gap 2)");

	checkbox_action("Decode UE4's velocity encoding", l.mvec_decode,
		a_teardown | a_reconcile, "mvec_decode", k_rebuild,
		"Live in BOTH directions. ON is the one that costs something: the decode's compute pipeline "
		"has to be built, which compiles DXBC at runtime, so it happens on the present thread and "
		"will stall a frame the first time. After that the guide-reset latch forces one Reset frame "
		"by itself, because the resource bound as DLSSNR.MVec has changed.\n\n"
		"ON binds OUR r16g16_float texture on the colour grid: one compute pass applies UE 4.27's "
		"DecodeVelocityFromTexture - scale AND bias, and MVecScaleX/Y can rescale a grid but can "
		"never remove a bias - and MVecScaleX/Y are then FORCED to 1.0 so the grid correction "
		"cannot double-apply. OFF binds the game's raw encoded velocity with the derived grid "
		"ratio, which is bit-for-bit the behaviour before this feature existed.\n\n"
		"Off-to-on used to be impossible for the same one-line reason the HDR codec's was: "
		"mvec_decode=0 at load latched mvec_failed, the run-latched \"the shader could not be "
		"built\" flag, which nothing ever cleared. That assignment is deleted.");

	ImGui::BeginDisabled(!l.mvec_decode.load(std::memory_order_relaxed));

	checkbox_b("Reconstruct camera motion from depth", l.mvec_reconstruct, k_reset,
		"Live, one Reset frame. OFF is a BRING-UP A/B ONLY and is WORSE than turning the decode "
		"off entirely: valid velocity texels are decoded and every invalid one is written as "
		"EXACTLY ZERO, which under UE 4.27 is still the whole static world, the sky, translucency "
		"and every movable that did not move. Shipping that hands DLSS zero motion for most of the "
		"frame. It exists so the two halves can be isolated on hardware, nothing else.");

	checkbox_b("Dilate the velocity lookup (UE's AA_CROSS nearest-depth)", l.mvec_dilate, k_reset,
		"Live, one Reset frame - it changes the CONTENT of the guide under an accumulated history. "
		"OFF by default: UE dilates for its own single-tap history, NVIDIA's DLSS plugin defaults "
		"to the NON-dilated branch, and DLSS does its own neighbourhood work, so pre-dilated "
		"vectors smear object silhouettes. Here so it can be A/B'd independently of the encoding "
		"fix.");

	checkbox_action("Transpose ClipToPrevClip", l.mvec_clip_transpose,
		a_clear_clip, "mvec_clip_transpose", k_reset,
		"Live, and it ALSO clears the plausibility latch, which is what makes it a real control "
		"rather than a one-shot.\n\n"
		"A wrong transpose fails the plausibility check; thirty consecutive failures set "
		"view_layout_failed and clip_ok=false PERMANENTLY for the resolution, and until this "
		"ladder existed those were cleared only by a full feature release. So the FIRST bad value "
		"killed the knob for the rest of the session and every later change to it did nothing - a "
		"control that lies, which is the one outcome this panel exists to prevent. Now each change "
		"clears the latch and re-arms its one-shot log line, so you can try both settings and read "
		"the reason for each in ReShade.log.\n\n"
		"If motion is roughly right at screen centre and wrong at the edges - and worse under "
		"camera ROLL - this is the knob. A near-identity matrix cannot tell the two apart, which "
		"is why this exists rather than a self-test.");

	slider_u32("Pin the ClipToPrevClip row (0 = discover it)", l.mvec_clip_row, 0, 511,
		a_clear_clip, "mvec_clip_row", k_reset,
		"Live, and it clears the same latches as Transpose above and for the same reason: an "
		"out-of-range pin sets view_layout_failed immediately and permanently.\n\n"
		"0 is the RECOMMENDED setting and means \"discover it and VALIDATE it\": the row is derived "
		"twice independently - a 26-constraint content signature over the View constant buffer, and "
		"this project's own DXBC instruction analysis - and the two must AGREE or the "
		"reconstruction is refused. STRAY measured 122 both ways. A pinned row SKIPS the content "
		"signature entirely, so it is as loud in the log as shader_hash=0 is.");

	ImGui::EndDisabled();

	// THE DLSS-NR SCOPE ENDS HERE. Everything from here down is either DLSS-SR's, shared between
	// the two, or launch-time - and none of it may be gated on the DLSS-NR snippet. See the three
	// predicates at the top of this function.
	ImGui::EndDisabled();   // !usable

	// ---- DLSS Super Resolution --------------------------------------------------------------------
	ImGui::SeparatorText("DLSS Super Resolution (NGX feature 1, nvngx_dlss.dll)");

	ImGui::BeginDisabled(!sr_section_live);
	{
		const bool want_sr    = l.dlss_sr.load(std::memory_order_relaxed);
		// f.valid is false when the DllMain hook has not run, which is the ABI probe's case. Treat
		// an unknown arm state as NOT armed rather than as armed: the panel's whole contract is
		// that it never reports a success it has not been told about.
		const bool sr_armed   = f.valid && f.sr_armed;
		const bool sr_loaded  = f.valid && f.sr_snippet_loaded;

		checkbox_action("Use DLSS Super Resolution for the accepted dispatch (dlss_sr)", l.dlss_sr,
			a_teardown | a_reconcile, "dlss_sr", k_rebuild,
			"HALF LIVE, AND THE UI SAYS WHICH HALF - the same shape as require_trampoline below.\n\n"
			"THE BRANCH IS LIVE IN BOTH DIRECTIONS. The decision that sends the accepted TAA dispatch "
			"to DLSS-SR instead of DLSS-NR is taken after the per-pass snapshot, so unticking this "
			"hands the dispatch straight back to DLSS-NR on the next frame with the SR feature and its "
			"textures released. That direction is complete and is the one to use for an A/B.\n\n"
			"1 -> 0 IS FULLY LIVE. 0 -> 1 NEEDS A RELAUNCH WHEN THE SNIPPET WAS NOT LOADED AT LAUNCH, "
			"and here is the specific reason: arming DLSS-SR means a 59 MB LoadLibraryW of "
			"nvngx_dlss.dll claiming the trampoline's SLOT B, and then NVSDK_NGX_D3D12_Init_Ext "
			"through that slot from a render thread with a fully built device. That call happens "
			"exactly once per process, on the first accepted dispatch, and this tree's one measured "
			"fact about Init_Ext's fragility is that it can HANG - so a second one is not offered, on "
			"exactly the argument app_id is refused for. The value is saved and takes effect next "
			"launch, the status line beside this box says so, and the reconfigure banner reports "
			"RELAUNCH REQUIRED rather than APPLIED.\n\n"
			"NOTE: DLSS-SR TAKES the dispatch. With this on, the DLSS-NR evaluate does not run.");

		if (want_sr && sr_armed)
			overlay_imgui::textf_colored(col::green, "    DLSS-SR is ARMED - the branch is live now.");
		else if (want_sr && sr_loaded)
			overlay_imgui::textf_colored(col::red,
				"    nvngx_dlss.dll LOADED but Init_Ext through slot B FAILED - see ReShade.log. "
				"sr_try_run bails on \"not armed\" and the game's own TAA is untouched. This cannot "
				"be retried in-process; fix the cause and relaunch.");
		else if (want_sr)
			overlay_imgui::textf_colored(col::amber,
				"    NOT ARMED: nvngx_dlss.dll was never loaded, because dlss_sr was 0 in "
				"stray_dlssnr.ini at launch. Press Save below and relaunch. Until then this box is a "
				"saved preference, not a running state, and DLSS-SR does NOT run.");
		else if (sr_armed)
			overlay_imgui::textf_colored(col::dim,
				"    armed but not selected - the dispatch is going to DLSS-NR. Re-ticking is live.");

		// ---- CHAIN MODE ------------------------------------------------------------------------
		// Its own control, and NOT inside the "SR is taking the dispatch" scope below: chain mode is
		// the configuration in which dlss_sr is 0 and DLSS-SR still runs, so gating it on want_sr
		// would grey out the one box that turns it on.
		const bool want_chain = l.dlss_chain.load(std::memory_order_relaxed);
		const bool nr_armed   = f.valid && f.armed;
		checkbox_action("Chain DLSS-NR into DLSS-SR on one dispatch (dlss_chain)", l.dlss_chain,
			a_teardown | a_reconcile, "dlss_chain", k_rebuild,
			"BOTH NETWORKS, ONE ACCEPTED DISPATCH. DLSS-NR denoises at the RENDER extent and its "
			"denoised result becomes DLSS-SR's colour input; DLSS-SR then upscales that. It is not "
			"dlss_nr and dlss_sr both being 1 - that configuration hands the dispatch to SR and the "
			"NR evaluate never runs at all.\n\n"
			"IT NEEDS BOTH SNIPPETS ARMED. nvngx_dlssnr.dll (dlss_nr=1) AND nvngx_dlss.dll, and the "
			"second is a 59 MB LoadLibraryW that happens at LAUNCH - so 0 -> 1 is save-and-relaunch "
			"on exactly the terms dlss_sr is, for exactly the same measured reason (Init_Ext is "
			"called once per process and its one recorded failure mode is a hang). 1 -> 0 is fully "
			"live: the next frame runs whichever single feature is selected.\n\n"
			"IT ALSO RE-PINS THE SHADER. The chain upscales, so it targets the MainUpsampling "
			"permutation - a different #define set, therefore different DXBC and a different "
			"fnv1a64. sr_shader_hash below is used for chain exactly as it is for dlss_sr; with it "
			"at 0 the DLSS-NR pin is used and the chain will simply never see an accepted dispatch.\n\n"
			"The proof it RAN is \"DLSS-CHAIN: CHAINED EVALUATE #1 OK\" in ReShade.log and the "
			"chained= counter on the periodic DLSS-CHAIN census line. Nothing here infers success "
			"from the absence of an error.");

		if (want_chain && nr_armed && sr_armed)
			overlay_imgui::textf_colored(col::green,
				"    BOTH features are armed - the chain is reachable now.");
		else if (want_chain && !sr_armed)
			overlay_imgui::textf_colored(col::amber,
				"    DLSS-SR is NOT armed, so the chain will NOT run. Save and relaunch (see the "
				"dlss_sr line above for which of the two reasons applies).");
		else if (want_chain && !nr_armed)
			overlay_imgui::textf_colored(col::amber,
				"    DLSS-NR is NOT armed, so the chain will NOT run - it is the FIRST network in "
				"the chain. Set dlss_nr=1 below and relaunch.");

		// Everything below only means anything while DLSS-SR is actually going to run, and CHAIN
		// MODE IS ONE OF THE TWO WAYS IT DOES. Gating this on want_sr alone would have greyed out
		// every create flag, the jitter scales and the output geometry on the dlss_sr=0,
		// dlss_chain=1 configuration - in which sr_try_run reads every one of them.
		ImGui::BeginDisabled(!((want_sr || want_chain) && sr_armed));

		// a_reconcile, not 0u, and it is not decoration: the render path prints a one-shot ERROR
		// when this is 1 while sr_direct_output and sr_copy_back are both 0, because that
		// combination is REFUSED rather than obeyed. A one-shot latch plus a live key is the
		// "control that lies" shape - turn it on, read the refusal, turn it off, turn it on again
		// and the second refusal is silent. The service re-arms the latch on every reconcile.
		checkbox_action("Suppress the game's own TAA dispatch (sr_suppress_taa)", l.sr_suppress_taa,
			a_reconcile, "sr_suppress_taa", k_plain,
			"Live, per dispatch, free. OFF is the SAFE shape and the bring-up default: the game's "
			"TAAU still runs and DLSS writes on top - one wasted dispatch and a correct frame on every "
			"failure path, because the game's TAAU unconditionally writes every pixel of the output "
			"view rect. ON stops that re-issue.\n\n"
			"ONE COMBINATION IS REFUSED RATHER THAN OBEYED, and the render path says so in the log: "
			"suppressing while sr_direct_output=0 AND sr_copy_back=0 would stop the game's TAA and "
			"then evaluate into a texture nothing reads, leaving the frame holding whatever was last "
			"in u0. That is not a rung on any ladder. Turn sr_copy_back on first.\n\n"
			"NOTE: ReShade's event dispatch does not short-circuit, so this suppression applies to "
			"EVERY co-loaded add-on and they have no way to learn it.");

		static const char *const perf_items[] = {
			"0  MaxPerf", "1  Balanced", "2  MaxQuality",
			"3  UltraPerformance", "4  UltraQuality", "5  DLAA",
		};
		combo_u32("PerfQualityValue (sr_perf_quality)", l.sr_perf_quality, perf_items, 6, k_plain,
			"Live, at the cost of ONE feature recreate, and the render path arranges that itself. It "
			"is latched into the DLSS create-params at CreateFeature and there is no evaluate-time "
			"equivalent, so the value cannot be re-read without releasing the feature - and a release "
			"needs an idled queue, which only the present thread can do. Changing it here queues that "
			"release; the next accepted dispatch creates the feature with the new value.\n\n"
			"It does NOT choose the render preset - the snippet picks that slot from Width/OutWidth. "
			"0 is right for 1920 -> 3840; use 5 (DLAA) when you run r.ScreenPercentage=100.");

		slider_u32("DLSS.Hint.Render.Preset (sr_render_preset, 0 = auto)", l.sr_render_preset, 0, 15,
			0u, "sr_render_preset", k_plain,
			"Live on the same terms as PerfQualityValue above: create-time, so it needs the recreate "
			"button to take. The value goes into the ONE Hint.Render.Preset slot the render/display "
			"ratio selects, which is why a preset set for the wrong ratio silently does nothing. "
			"0 = auto, i.e. let the snippet choose.");

		// a_reconcile, NOT a bare bump. This tooltip promises that ticking the box asks the service
		// to BUILD the shared mvec pipeline, and checkbox_b cannot ask the service anything: it
		// stores the atomic and bumps an epoch, so action_bits stays empty, k_reset never moves the
		// rebuild epoch, take_reconfigure returns bits==0 with ident_changed false, and
		// nr_service_reconfigure leaves at its `work == 0u && !req.ident_changed` early exit -
		// above the a_reconcile block that was written for exactly this case and even names it.
		// The DLSS-NR twin above is wired with checkbox_action for the same reason; this was an
		// asymmetry rather than a decision. nr_build_mvec_pipeline returns early on st.mvec.ok, so
		// the common case where the pipeline already exists still costs nothing.
		checkbox_action("Decode UE4's velocity encoding for SR (sr_mvec_decode)", l.sr_mvec_decode,
			a_reconcile, "sr_mvec_decode", k_reset,
			"Live, one Reset frame. Independent of the DLSS-NR mvec_decode above in VALUE, but the "
			"two share ONE compiled pipeline: the root signature, the PSO and the DXBC are a single "
			"set, built when EITHER feature asks for it, and each feature writes into its own target "
			"- DLSS-NR on the colour grid, DLSS-SR on the render grid.\n\n"
			"If neither feature wanted it at launch the pipeline was never built; ticking this asks "
			"the present-thread service to build it, which compiles DXBC and will stall a frame once.");

		ImGui::BeginDisabled(!l.sr_mvec_decode.load(std::memory_order_relaxed));
		checkbox_b("Reconstruct camera motion from depth for SR (sr_mvec_reconstruct)",
			l.sr_mvec_reconstruct, k_reset,
			"Live, one Reset frame. Same argument as the DLSS-NR control above: OFF writes EXACTLY "
			"ZERO wherever the velocity texel is the cleared sentinel, which under UE 4.27 is the "
			"static world, the sky, translucency and every movable that did not move. It is a "
			"bring-up A/B, not a setting to ship.");
		ImGui::EndDisabled();

		// ---- THE LADDER: what the frame actually does with the evaluate's result ----------------
		checkbox_b("Evaluate straight into the game's TAA output (sr_direct_output)",
			l.sr_direct_output, k_plain,
			"Live, per dispatch, free - the branch that reads it is inside sr_try_run, downstream of "
			"the per-pass snapshot.\n\n"
			"ON binds the game's own u0 as DLSS's output and no add-on texture is allocated at all. "
			"OFF evaluates into our own texture and then copies it back if sr_copy_back is on. OFF + "
			"sr_copy_back is the bring-up shape: a failed evaluate leaves the game's own TAAU result "
			"in place instead of whatever was last in u0.");

		checkbox_b("Copy our result back over the TAA output (sr_copy_back)", l.sr_copy_back, k_plain,
			"Live, per dispatch. Only consulted when sr_direct_output is OFF - with direct output "
			"there is nothing to copy, because DLSS wrote into u0 itself.\n\n"
			"WITH BOTH OFF the SR pass evaluates into a texture NOTHING READS and the frame looks "
			"exactly like stock TAA. That is a legitimate bring-up rung - a correct frame is then "
			"positive evidence the state restore is faithful - but it is also the combination that "
			"makes sr_suppress_taa above be REFUSED rather than obeyed.");

		checkbox_b("Take the render extent from ViewSizeAndInvSize (sr_use_view_rect)",
			l.sr_use_view_rect, k_reset,
			"Live, one Reset frame - and it needs no button, because changing it moves the RENDER "
			"extent and sr_try_run's own geometry test notices that and queues the feature rebuild "
			"itself, exactly as a resolution change does.\n\n"
			"ON uses the view rect the View uniform buffer reports, which is the sub-texture region "
			"UE actually rendered. OFF uses the whole colour TEXTURE extent, which with a pooled "
			"render target can be larger than the frame - and then DLSS is told to upscale from an "
			"extent containing pixels the game never wrote.");

		// ---- jitter ------------------------------------------------------------------------------
		slider_f("Jitter scale X (sr_jitter_scale_x)", l.sr_jitter_scale_x, -4.0f, 4.0f, "%.3f",
			k_reset,
			"Live, one Reset frame. 1.0 is the shipping value and means \"send the jitter exactly as "
			"UE reports it\". This is the SIGN A/B: DLSS and UE disagree about the sign convention on "
			"some titles, and -1.0 here is how that is tested without rebuilding anything. Anything "
			"other than 1.0 is flagged as OVERRIDDEN on the arm banner.");
		slider_f("Jitter scale Y (sr_jitter_scale_y)", l.sr_jitter_scale_y, -4.0f, 4.0f, "%.3f",
			k_reset, "Live, one Reset frame. See Jitter scale X - the two are independent so the "
			"sign A/B can be run one axis at a time.");

		checkbox_b("Accept a projection-only jitter read (sr_jitter_projection_only)",
			l.sr_jitter_projection_only, k_reset,
			"Live, one Reset frame. The jitter read has three tiers of confidence; OFF is STRICT and "
			"accepts only the full or no-params layouts. ON additionally accepts a match found from "
			"the projection matrix alone.\n\n"
			"THERE IS NO \"RUN WITHOUT JITTER\" CONFIGURATION - sending (0,0) would be worse than "
			"refusing, because zero is a legitimate value (r.TemporalAASamples=1) the snippet cannot "
			"tell apart from a failed read, and the result is a silent shimmer. This widens what "
			"counts as a read, never what happens when there is none.");

		// ---- the motion guide's grid --------------------------------------------------------------
		slider_f("MVec scale X for SR (sr_mv_scale_x, 0 = auto)", l.sr_mv_scale_x, -4096.0f, 4096.0f,
			"%.3f", k_reset,
			"Live, one Reset frame. 0 means DERIVED - render extent over velocity extent, which is "
			"right whenever the guide is on its own grid. A non-zero value overrides that derivation "
			"outright and is flagged OVERRIDDEN on the arm banner.\n\n"
			"Note it cannot fix UE4's velocity ENCODING, which carries a bias as well as a scale: "
			"that is what sr_mvec_decode is for. A scale alone can rescale a grid and can never "
			"remove a bias.");
		slider_f("MVec scale Y for SR (sr_mv_scale_y, 0 = auto)", l.sr_mv_scale_y, -4096.0f, 4096.0f,
			"%.3f", k_reset, "Live, one Reset frame. See MVec scale X.");

		// ---- the output geometry ------------------------------------------------------------------
		// k_reset and no action bit, and that is not an oversight. All three feed want_out_w/h, and
		// sr_try_run compares that against sr_seen_out_w/h on every pass: a change makes key_moved
		// true, which queues kTeardown through the SAME seam the recreate button uses. Raising the
		// bits here as well would mean a teardown per pixel of slider drag for no extra effect.
		slider_u32("Output width (sr_out_width, 0 = from the group counts)", l.sr_out_width,
			0, 7680, 0u, "sr_out_width", k_reset,
			"Live, and it rebuilds the feature BY ITSELF: sr_try_run compares the output extent it "
			"derives against the one it used last pass, and a move queues the same release the "
			"recreate button below asks for. Dragging this is therefore safe but not free - each "
			"landing value the pass sees costs one CreateFeature.\n\n"
			"0 means \"derive it from the dispatch group counts\", which is what a normal run wants. "
			"Pin it only when the derivation is provably wrong for this title.");
		slider_u32("Output height (sr_out_height, 0 = from the group counts)", l.sr_out_height,
			0, 4320, 0u, "sr_out_height", k_reset,
			"Live on the same terms as Output width above - the geometry test rebuilds the feature "
			"itself. 0 derives it from the dispatch group counts.");
		slider_u32("Group tile size (sr_group_tile)", l.sr_group_tile, 1, 32,
			0u, "sr_group_tile", k_reset,
			"Live on the same terms as the two above. This is the shader's [numthreads] tile edge, "
			"and it is what turns the dispatch's GROUP counts into an output extent: out = tile * "
			"groups. 8 matches UE 4.27's TAA pass. Getting it wrong does not crash anything - it "
			"derives the wrong output extent, the output-UAV pick then rejects every candidate, and "
			"the pass simply stops running.");

		// ---- THE CREATE FLAGS. Tier 1: latched at CreateFeature, no evaluate-time equivalent. -----
		// These DO auto-rebuild, exactly as the geometry above does, and by the same mechanism:
		// dlss_sr::feature_matches compares create_flags, perf_quality and hw_depth as well as the
		// four extents, and sr_try_run's create-param latch tests the descriptor it is about to
		// build against the live feature and queues kTeardown when they differ.
		//
		// An earlier revision of this comment claimed the opposite - "nothing in the render path
		// compares them against what the live feature was created with" - and that was false in
		// this tree even then. What was missing was not the COMPARISON but a SAFE PLACE TO ACT ON
		// IT: create_feature's answer to a mismatch is release_feature, whose header requires an
		// idled queue, and sr_try_run runs on a command-list recording thread that cannot idle
		// one. The latch routes the release to the present thread instead, which is the only seam
		// that can. The recreate button below still works and reaches that same seam.
		//
		// k_plain and no action bit is therefore right: the render path notices the change by
		// itself, and raising the bits here as well would queue a redundant second teardown.
		ImGui::SeparatorText("DLSS-SR create flags - applied on the next accepted dispatch");

		checkbox_b("IsHDR (sr_hdr)", l.sr_hdr, k_plain,
			"CREATE-TIME, and LIVE: it is latched into the feature at CreateFeature, so the render "
			"path queues the feature's release on the next present and the dispatch after that "
			"creates it with the new value. One rebuilt feature per change, no button needed.\n\n"
			"It selects the HDR-TRAINED KERNEL. It is NOT an exposure gate and it does not change "
			"what is bound - the colour input is whatever the pass already had.");
		checkbox_b("MVLowRes - the guide is on the render grid (sr_mv_lowres)", l.sr_mv_lowres, k_plain,
			"CREATE-TIME, and LIVE - the render path rebuilds the feature itself, once per change.\n\n"
			"ON tells DLSS the motion vectors are at the RENDER extent, which is what UE 4.27 "
			"produces and what our own decode writes. OFF claims they are at the display extent; "
			"with a render-grid guide that is a mis-declaration, not an option.");
		checkbox_b("MVJittered - the guide carries the jitter (sr_mv_jittered)", l.sr_mv_jittered,
			k_plain,
			"CREATE-TIME, and LIVE - the render path rebuilds the feature itself. OFF is correct for "
			"UE 4.27: the "
			"velocity pass writes unjittered motion and the jitter is delivered separately through "
			"Jitter.Offset.X/Y.");
		checkbox_b("DepthInverted (sr_depth_inverted)", l.sr_depth_inverted, k_plain,
			"CREATE-TIME, and LIVE - the render path rebuilds the feature itself. ON is correct for "
			"UE 4.27, which "
			"uses reversed-Z. This is the SR feature's own flag and is independent of the DLSS-NR "
			"depth_inverted above; they are two features being told about the same buffer.");
		checkbox_b("AutoExposure (sr_auto_exposure)", l.sr_auto_exposure, k_plain,
			"CREATE-TIME, and LIVE - the render path rebuilds the feature itself. ON lets the snippet "
			"compute "
			"exposure itself, which is what a run that does not bind an exposure texture wants - and "
			"this add-on does not bind one.");
		checkbox_b("AlphaUpscaling (sr_alpha_upscaling)", l.sr_alpha_upscaling, k_plain,
			"CREATE-TIME, and LIVE - the render path rebuilds the feature itself. OFF is the default "
			"and is right "
			"unless the colour input's alpha is meaningful coverage rather than whatever the render "
			"target happened to leave there.");
		checkbox_b("DLSS.Use.HW.Depth (sr_hw_depth)", l.sr_hw_depth, k_plain,
			"CREATE-TIME, and LIVE - the render path rebuilds the feature itself. It tells the snippet "
			"the depth "
			"SRV is a hardware depth buffer rather than a linearised copy. ON is correct here: the "
			"resource bound is the game's own depth target.");

		checkbox_b("Run the optimal-settings query at arm (sr_optimal_settings)",
			l.sr_optimal_settings, k_plain,
			"ARM-TIME ONLY, AND THAT IS THE WHOLE OF IT - the same shape dlss_nr has. Its one read "
			"site is in the deferred initialiser, which runs once per process on the first accepted "
			"dispatch, so there is no second read a live value could reach. It is saved and reverted "
			"like every other key, and it takes on the NEXT ARM - which is next launch in an ordinary "
			"run, but is also this session if the deferred initialiser has not run yet, because "
			"turning `enabled` on from 0 runs it.\n\n"
			"It is a DIAGNOSTIC. It asks the snippet for a RECOMMENDED render resolution and prints "
			"it; it has no power whatsoever to make UE render at it - the only lever for that is "
			"r.ScreenPercentage. Off by default because it runs on a scratch parameter block whose "
			"Width/Height have inverted meaning, which is a footgun worth not arming by default.");

		if (ImGui::Button("Recreate the SR feature (apply create-time settings)"))
			request(a_teardown | a_reconcile, "sr create-time settings", k_rebuild);
		ImGui::SetItemTooltip(
			"Releases the DLSS-SR feature handle and its textures on the next present, after idling "
			"the queue. sr_try_run creates a fresh feature on the next accepted dispatch, reading "
			"PerfQualityValue and the render preset as they now stand.\n\n"
			"This is the SAME action a resolution change already takes, on the same seam, so it is "
			"not a new mechanism. It costs the frames one CreateFeature costs and nothing else; the "
			"DLSS-NR half is released and rebuilt with it, because they share one teardown.");

		ImGui::EndDisabled();
	}
	ImGui::EndDisabled();   // !sr_section_live

	// dlss_nr is OUTSIDE EVERY DISABLED SCOPE, which is what its comment always claimed and what it
	// now actually is: it used to sit outside the SR section's inner block but inside the DLSS-NR
	// `usable` block, so on a dlss_nr=0 run - the only run in which anyone wants this control - it
	// was greyed out and there was no way back to 1 without hand-editing the ini. It is the key a
	// user reaches for precisely when nothing is armed.
	{
		checkbox_action("Load and initialise DLSS-NR at all (dlss_nr)", l.dlss_nr,
			0u, "dlss_nr", k_plain,
			"LAUNCH-TIME, AND THIS IS THE ONE PLACE THAT SAYS SO. Unlike every other control on this "
			"panel, changing it does NOTHING to the running session - not because it was not worth "
			"wiring, but because both of its read sites have already happened: the 166 MB "
			"LoadLibraryW of nvngx_dlssnr.dll in init_device, and the Init_Ext gate on the first "
			"dispatch. There is no third site for a live value to reach.\n\n"
			"It is saved and reverted like everything else, and it takes effect next launch. Set it "
			"to 0 alongside dlss_sr=1 when you want DLSS-SR alone and would rather not pay 166 MB for "
			"a denoiser that will not be evaluated - with dlss_sr=1 the SR pass TAKES the accepted "
			"dispatch and the DLSS-NR evaluate does not run either way.\n\n"
			"To turn the DLSS-NR pass off for THIS session, use \"Enable DLSS Neural Rendering\" or "
			"`enabled` above. Both are live; this one is not.");
	}

	// ---- identification --------------------------------------------------------------------------
	// Shared by both features - see ident_usable at the top of this function.
	ImGui::BeginDisabled(!ident_usable);

	ImGui::SeparatorText("Identification - which dispatch this add-on hooks");

	overlay_imgui::textf_colored(col::amber,
		"These five decide WHICH dispatch is treated as the game's TAA pass. Getting them wrong "
		"does not crash anything - the pass simply stops running, and the status line above says "
		"WAITING FOR GAME DLSS - but writing the denoised image over the wrong render target would "
		"look like a game bug rather than an add-on bug, so nothing here is guessed.");

	{
		static char s_buf[32] = {};
		static bool s_editing = false;
		input_hash("shader_hash", "Apply hash", l.shader_hash, "shader_hash",
		           s_buf, sizeof(s_buf), s_editing,
		           "    typed but not applied - press Apply hash. 0 means \"any shader passing every "
		           "census gate\", which is not recommended.");
	}
	ImGui::SetItemTooltip(
		"Live. The exact DXBC hash of the target compute shader, as ReShade.log prints it.\n\n"
		"It is the one identification input that is MEMOIZED per pipeline state object, so a live "
		"change has to invalidate that memo on EVERY command list rather than on the next one - "
		"which is what the identification epoch does: one atomic that each recording thread reads "
		"for itself, next to a pointer compare that already runs on every dispatch. The armed "
		"pristine copy is dropped in the same step, because an arm made under the old "
		"identification must never be consumed under the new one.\n\n"
		"0 means \"any shader that passes every census gate\", and it is NOT recommended: the "
		"measured false positive 0x901e041a7cadc9db scores confidence 150 and would pass any "
		"score-based test. The class quorum does reject it, but relying on the quorum alone gives "
		"up the one identifier that is exact.");

	slider_u32("srv_depth  (t)", l.srv_depth, 0, 63, 0u, "srv_depth", k_ident,
		"Live. The t-register the TAA pass binds its DEPTH texture at. Changing it drops the armed "
		"pristine copy and forces one Reset frame; the identification one-shot log lines are "
		"re-armed too, so a wrong new value is REPORTED rather than rejected in silence.");
	slider_u32("srv_velocity  (t)", l.srv_velocity, 0, 63, 0u, "srv_velocity", k_ident,
		"Live. The t-register the TAA pass binds its VELOCITY texture at. The second measured "
		"candidate in STRAY (0x52101a15e1a0c5cc) uses t3 here and t7 for colour.");
	slider_u32("srv_colour  (t)", l.srv_colour, 0, 63, 0u, "srv_colour", k_ident,
		"Live, and this is the one that MUST drop the armed pristine copy - which the ident rung "
		"does, in the same begin_pass that applies the change.\n\n"
		"srv_colour is not only an identification pin: it is ALSO the history-restore refusal "
		"test. The restore arms on one frame and consumes on the next, so a change between the arm "
		"and the consume could land last frame's image on this frame's scene-colour input - the "
		"frozen, ghosted frame the restore path already refuses by name. It is the only setting in "
		"the add-on that tears ACROSS frames, which no per-pass snapshot could fix.");
	slider_u32("uav_output  (u)", l.uav_output, 0, 15, a_clear_failed, "uav_output", k_ident,
		"Live. The u-register carrying the TAA output - UE 4.27's FTAAStandaloneCS declares "
		"OutComputeTex at u0 and the optional OutComputeTexDownsampled at u1.\n\n"
		"Changing it changes which resource the denoised image is written INTO, so the armed "
		"pristine copy is dropped, the \"ambiguous output UAV\" and \"not a usable TAA output\" "
		"one-shots are re-armed, and the ring of resources the copy-back has written into is "
		"cleared - otherwise a recycled address from the old configuration could false-match the "
		"temporal-feedback detector.");

	// The DLSS-SR re-pin sits HERE, beside the key it overrides, rather than in the DLSS-SR section
	// above: it is an identification input, it goes through the identification epoch, and a reader
	// looking for "which dispatch" must find both in one place. It is also why this whole section
	// cannot be gated on the DLSS-NR snippet - it is an SR control.
	{
		static char s_buf[32] = {};
		static bool s_editing = false;
		input_hash("sr_shader_hash", "Apply SR hash", l.sr_shader_hash, "sr_shader_hash",
		           s_buf, sizeof(s_buf), s_editing,
		           "    typed but not applied - press Apply SR hash.");
	}
	ImGui::SetItemTooltip(
		"Live, through the same identification epoch shader_hash uses, and it is consulted ONLY "
		"while dlss_sr=1. 0 means \"use shader_hash\", which is what dlss_sr=0 does "
		"unconditionally.\n\n"
		"WHY IT EXISTS AT ALL. TAA_PASS_CONFIG and TAA_SCREEN_PERCENTAGE_RANGE are #defines, so "
		"flipping r.TemporalAA.Upsampling or dropping r.ScreenPercentage below 100 produces "
		"different DXBC and therefore a different fnv1a64 - and the ONLY symptom is that the pass "
		"silently stops being identified. DLSS-SR wants the MainUpsampling permutation, which is a "
		"different shader from the one DLSS-NR is pinned to, so this carries its hash without "
		"disturbing that pin and the two features can be A/B'd on one install.");

	ImGui::EndDisabled();   // !ident_usable

	// ---- NGX bring-up ------------------------------------------------------------------------------
	// BACK UNDER `usable`, and this one genuinely is DLSS-NR-only: PopulateParameters_Impl is called
	// on st->params, the DLSS-NR parameter block, which a dlss_nr=0 run never allocates at all
	// (stray_dlssnr.cpp guards the call on st->params != nullptr for exactly that reason).
	ImGui::BeginDisabled(!usable);

	ImGui::SeparatorText("NGX bring-up");

	{
		// AN APPLY BUTTON, NEVER A BARE CHECKBOX, and the reason is in the tooltip verbatim.
		bool on = l.populate_parameters.load(std::memory_order_relaxed);
		if (ImGui::Checkbox("Call the snippet's PopulateParameters_Impl", &on))
			l.populate_parameters.store(on, std::memory_order_relaxed);
		ImGui::SetItemTooltip(
			"Live, but only when you press Apply beside it - deliberately not on the click.\n\n"
			"PopulateParameters_Impl is a GATED export whose EXACT SIGNATURE HAS NOT BEEN VERIFIED "
			"AGAINST THIS SNIPPET BUILD (addon_config.hpp:92-95). Nothing in the documented flow "
			"needs it, which is why it is off by default. A checkbox that fired on click would let "
			"a stray tick call an unverified gated export mid-frame; an Apply button makes it a "
			"decision.\n\n"
			"OFF-to-ON is one call on the existing parameter block: no NGX re-init, no teardown. "
			"ON-to-OFF cannot be un-called, so it needs a FRESH parameter block - which needs the "
			"feature released first, because CreateFeature was handed the old one. That is the "
			"full rebuild, and it is what Apply does.");
		ImGui::SameLine();
		if (ImGui::Button("Apply PopulateParameters"))
			// a_apply_populate, NOT a_reconcile, and a_teardown SPELLED OUT rather than left to
			// the rebuild epoch.
			//
			// Its own bit, because deriving the work from the checkbox inside a_reconcile meant
			// every other control that raises a_reconcile applied a value the user had not
			// Applied - see the a_apply_populate declaration for what that reached.
			//
			// a_teardown explicitly, because the ON->OFF direction needs the feature released
			// before the parameter block can be replaced, and relying on the k_rebuild epoch to
			// imply it has a hole: take_reconfigure ADOPTS the epochs on its first call for a
			// given seen-state, so on that one present the edge is swallowed while the drained
			// action bits still arrive. Every other k_rebuild control already raises a_teardown
			// itself; this was the only one that did not.
			request(a_teardown | a_apply_populate, "populate_parameters", k_rebuild);
		ImGui::SetItemTooltip(
			"Applies the checkbox to the left, and nothing else applies it - no other control in "
			"this panel can call PopulateParameters_Impl or swap the parameter block.\n\n"
			"Off-to-on calls PopulateParameters_Impl once. On-to-off releases the NGX feature and "
			"allocates a fresh parameter block, because the call cannot be undone on the block it "
			"was made against. If the release does not happen the swap is REFUSED rather than "
			"done anyway, and the panel says so in red.");

		// The checkbox and reality can disagree, by design - so say when they do, at the moment
		// the user is looking at the control, rather than leaving a ticked box standing for
		// something that has not happened.
		if (f.armed && on != s.populate_applied.load(std::memory_order_relaxed))
		{
			ImGui::Indent();
			overlay_imgui::textf_colored(col::amber,
				"checked but NOT APPLIED - the snippet is still %s. Press Apply.",
				s.populate_applied.load(std::memory_order_relaxed)
					? "running against a populated parameter block"
					: "running against the plain parameter block");
			ImGui::Unindent();
		}
	}

	ImGui::EndDisabled();   // !usable

	// ---- diagnostics ---------------------------------------------------------------------------
	// OUTSIDE the !usable block on purpose. Every control here is read-only instrumentation that
	// works whether or not NGX ever armed - and the case where NGX did NOT arm is exactly when a
	// user wants the shader census turned on.
	ImGui::SeparatorText("Diagnostics (read-only instrumentation; never touches the render path)");

	checkbox_action("Shader census, root-signature and SRV-table dumps", l.diagnostics,
		0u, "diagnostics", k_plain,
		"Live, and it is the ONE setting that could never have come through the per-pass snapshot: "
		"it is read on EVERY draw and EVERY dispatch in the process, on arbitrary recording "
		"threads, outside the accepted-pass lock. So it is a relaxed atomic load at each of those "
		"three sites instead - which costs a load per draw and makes the control real. The old "
		"reason for greying it out (\"threads this overlay must not race with\") was an argument "
		"for using an atomic, not for having no control.\n\n"
		"It writes to ReShade.log and touches nothing else, in either position.");

	checkbox_action("DXR dispatch census (rt_census)", l.rt_census,
		a_apply_census, "rt_census", k_plain,
		"Live. Off means off: every census entry point returns after ONE relaxed atomic load, "
		"nothing is counted, named, logged or allocated, and the dispatch_rays handler returns "
		"false so ReShade issues the game's own DispatchRays exactly as with no add-on present. "
		"The census allocates NOTHING at any time, on or off - every table it keeps is a "
		"fixed-size array.\n\n"
		"ONE HONEST LIMIT: the counters are CUMULATIVE FROM THE FIRST TIME IT WAS ARMED and are "
		"not reset when you toggle it off and on. A summary block after a re-tick therefore "
		"includes dispatches counted before it. Read the deltas between summaries, not the "
		"totals.");

	ImGui::BeginDisabled(!l.rt_census.load(std::memory_order_relaxed));
	slider_u32("Presents between RT census summaries", l.rt_census_frames, 60, 6000,
		a_apply_census, "rt_census_frames", k_plain,
		"Live. 600 is ten seconds at 60 fps. A summary is also emitted at destroy_device "
		"regardless of this.");
	ImGui::EndDisabled();

	checkbox_action("Refuse to run without remix_nvngx.dll (require_trampoline)", l.require_trampoline,
		a_reconcile, "require_trampoline", k_plain,
		"HALF LIVE, AND THE UI SAYS WHICH HALF.\n\n"
		"1 -> 0 IS LIVE, and only matters in the session where the snippet FAILED to load for "
		"exactly this reason: load_snippet already called unload() on that path, so nothing is "
		"loaded and re-running it is clean. Unticking this and letting the service re-arm is the "
		"same action as ticking `enabled` above.\n\n"
		"0 -> 1 NEEDS A RELAUNCH when the snippet is already loaded, and here is the specific "
		"reason: honouring it would mean UNLOADING an initialised snippet, and this tree has no "
		"in-process unload path anywhere. stray_dlssnr.cpp:4339-4341 declines to FreeLibrary even "
		"at device teardown - \"a 166 MB module that may still hold worker threads\". So the value "
		"is remembered and saved, and it takes effect next launch. Nothing about the current "
		"session changes, and the panel does not pretend otherwise.\n\n"
		"What it is FOR: every gated snippet export resolves its caller's module from the return "
		"address and rejects anything whose path does not contain \"nvngx.dll\" with 0xbad00002, "
		"and Init_Ext and CreateFeature are both gated. A resolve-time probe cannot detect that, "
		"because GetProcAddress succeeds and only the calls fail - so turning this off buys a "
		"silent 0xbad00002 instead of a clear message.");

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
			"EVERY setting this panel can change is written, INCLUDING the identification pins "
			"(shader_hash, srv_depth, srv_velocity, srv_colour, uav_output) - they became live "
			"controls, and a control that applies now but forgets on relaunch is still a control "
			"that lies. So if you have dragged a pin while chasing an artefact, press \"Revert to "
			"stray_dlssnr.ini\" before saving, or the dragged value is what lands on disk.\n\n"
			"app_id is the ONE key never written, along with every comment, blank line and key this "
			"panel does not know about.\n\n"
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
// WHAT IS LEFT THAT A RELAUNCH IS STILL NEEDED FOR - AND IT IS TWO THINGS, NOT TEN.
//
// This section used to list ten keys. Eight of them are now real controls above, reached through
// the reconfigure ladder. These two remain, and each one states the SPECIFIC line of this add-on
// that makes it so, in the control's own tooltip as well as on screen - a reason that lives only
// in a header comment is a reason the user never reads.
//
// The bar for being in this list is proof, not convenience. "A lazy classification is not
// acceptable" is the standing rule, and the reasoning for each of these is written out in full in
// the ladder section at the top of this file.
// ---------------------------------------------------------------------------------------------
inline void draw_load_only(const host_facts &f)
{
	if (!ImGui::CollapsingHeader("Settings a relaunch is still needed for (4)"))
		return;

	char buf[160];

	ImGui::TextWrapped("Everything else in stray_dlssnr.ini is a live control above. These four are "
	                   "not, for the reasons given - and the reason is specific in each case, not a "
	                   "general caution. Two of them have a real control above as well, because "
	                   "one of their two directions IS live; this section is where the other "
	                   "direction is spelled out.");
	ImGui::Spacing();

	std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)f.app_id);
	load_only("app_id", buf,
		"NGX application id. It has NO render-path effect whatsoever: the snippet resolves its "
		"weights from its own embedded WEIGHTS_HT resource, so this only names the log file the "
		"snippet writes beside the add-on.\n\n"
		"The MECHANISM to change it in place does exist - Shutdown1 is resolved, is required at "
		"load, and is already called in-process at device teardown, so Shutdown1 followed by "
		"Init_Ext would be one more action on the reconfigure service. What is NOT proven is that "
		"Init_Ext survives a SECOND call, and the only measurement this project has of its "
		"fragility is that it HANGS when called at a moment the snippet does not tolerate: the log "
		"stops between \"loaded nvngx_dlssnr.dll\" and the Init_Ext result, the process sits at "
		"~2% CPU, and the title never reaches its menu.\n\n"
		"A hang is not a failure that degrades, and the standing rule for this ladder is that a "
		"reconfigure which fails must leave the previous working state. So an unverified in-process "
		"NGX re-init is not offered, to rename a log file. Edit app_id in stray_dlssnr.ini and "
		"relaunch. If this is ever measured on hardware, the action bit is a few lines.");

	load_only("require_trampoline: 0 -> 1 only", live_require_trampoline() ? "1" : "0",
		"The 1 -> 0 direction IS live and has a real checkbox under Diagnostics above. Only 0 -> 1 "
		"needs a relaunch, and only when the snippet is already loaded: honouring it then would "
		"mean UNLOADING an initialised snippet, and there is no in-process unload path anywhere in "
		"this tree - stray_dlssnr.cpp:4339-4341 declines to FreeLibrary even at device teardown, "
		"because a 166 MB module may still hold worker threads. The new value is saved and takes "
		"effect next launch.");

	load_only("dlss_nr", live_dlss_nr() ? "1" : "0",
		"Launch-time in BOTH directions, and it is the only key on this panel of which that is "
		"true. Its two read sites are the 166 MB LoadLibraryW of nvngx_dlssnr.dll in init_device "
		"and the Init_Ext gate on the first accepted dispatch; both have already happened by the "
		"time this panel can be drawn, and there is no third site a live value could reach. "
		"Wiring it into the per-pass snapshot would have put a value in front of a reader that "
		"does not exist - a control that looks right in every layer and reaches nothing, which is "
		"the exact failure the kParam CI gate was added for.\n\n"
		"It IS saved, reverted and dirty-tracked like every other key, and the checkbox for it "
		"lives in the DLSS Super Resolution section above. To turn the DLSS-NR pass off for THIS "
		"session use `enabled` or \"Enable DLSS Neural Rendering\"; both are live.");

	load_only("dlss_sr: 0 -> 1 only", live_dlss_sr() ? "1" : "0",
		"The 1 -> 0 direction IS live and has a real checkbox above: the branch that sends the "
		"accepted dispatch to DLSS-SR is taken after the per-pass snapshot, so unticking hands it "
		"back to DLSS-NR on the next frame with the SR feature and its textures released.\n\n"
		"Only 0 -> 1 needs a relaunch, and only when nvngx_dlss.dll was not loaded at launch. "
		"Arming DLSS-SR means a 59 MB LoadLibraryW claiming the trampoline's SLOT B and then "
		"NVSDK_NGX_D3D12_Init_Ext through that slot, from a render thread, with a fully built "
		"device - a call this process makes exactly once, on the first accepted dispatch. Doing "
		"it a second time is the same unverified action app_id is refused for above, and the only "
		"measurement this project has of Init_Ext's fragility is that it HANGS when called at a "
		"moment the snippet does not tolerate. A hang is not a failure that degrades, and the "
		"standing rule for this ladder is that a reconfigure which fails leaves the previous "
		"working state. So the new value is saved and takes effect next launch, and the "
		"reconfigure banner says RELAUNCH REQUIRED rather than APPLIED.");

	load_only("stray_dlssnr.ini", f.ini_found ? "found" : "NOT FOUND (every setting is at its built-in default)",
		"Not a setting. A missing file is not an error - every default in addon_config.hpp is the "
		"shipping default, so the add-on behaves identically with and without it. Pressing Save "
		"above will create one.");
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
