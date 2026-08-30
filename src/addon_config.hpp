// addon_config.hpp - the stray_dlssnr.ini beside the add-on.
//
// Deliberately a tiny hand-rolled reader rather than ReShade's config API: ReShade's
// get_config_value keys off ReShade.ini, which the user is also editing for effects, and a
// missing key there silently yields a default with no diagnostic. Here every parse is reported.
//
// PARSED once, at the first init_device - but this struct is NO LONGER a read-once snapshot, and
// the sentence that used to stand here ("There is no hot reload") is now false. Every key below
// except app_id is a live control in the overlay.
//
// The concern that sentence recorded is real and is still honoured, just differently. A knob that
// changed halfway through a frame would produce an evaluate whose create-time and evaluate-time
// parameters disagree - a bug that is impossible to see in a screenshot. So a live change never
// touches this struct mid-pass: overlay_ui::begin_pass copies the overlay's atomics into it ONCE
// per accepted dispatch, on the render thread, under the lock that pass already holds, and every
// read inside the pass then sees one coherent set of values. Anything that cannot be applied that
// way - a different texture format, a pipeline that does not exist yet, a snippet that is not
// loaded - is deferred to nr_service_reconfigure on the next present.
//
// TWO CONSEQUENCES FOR ANYONE EDITING THIS FILE:
//   * A NEW KEY IS NOT LIVE BY DEFAULT. Adding it here and to the parser gets it parsed and
//     nothing more. It needs an atomic in overlay_ui::live_block, a line in
//     OVERLAY_OWNED_FIELDS, a line in begin_pass's snapshot, and a control - or it is a setting
//     the ini can express and the UI silently cannot.
//   * READING A FIELD OF THIS STRUCT OFF THE RENDER THREAD IS NOW A DATA RACE. begin_pass writes
//     it on a recording thread. The main-thread readers that used to exist were removed for
//     exactly this reason; use the overlay_ui::live_*() accessors instead.
//
// The full per-key ladder, with the read site of every key and the reason for its rung, is in the
// header comment of src/overlay_ui.hpp.

#pragma once

#include "reshade_compat.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cfg {

struct config
{
	// ---- master switch -------------------------------------------------------------------
	// 0 makes the add-on a strict no-op: no snippet is loaded, no resource is created, and the
	// dispatch handler returns false on its first line, so ReShade issues the game's Dispatch
	// exactly as it would with no add-on present.
	bool     enabled = true;

	// The probe's read-only diagnostics: the shader census, the root-signature variant dump and
	// the bounded SRV-table dumps. They never touch the render path - they only write to
	// ReShade.log - but they are switchable so that "strict no-op" can be made literal.
	bool     diagnostics = true;

	// ---- the DXR dispatch census (src/rt_census.hpp) ---------------------------------------
	// DEFAULTS OFF, and off means off.
	//
	// With rt_census = 0 every census entry point returns immediately after ONE relaxed atomic
	// load: on_init_pipeline's note_pipeline, on_bind_pipeline's note_state_object_bind, the
	// dispatch_rays handler (which then returns false, so ReShade issues the game's DispatchRays
	// exactly as with no add-on present), on_present's on_frame, and on_destroy_device's report.
	// Nothing is counted, named, logged, allocated or created. The census allocates NOTHING at
	// any time, on or off - every table it keeps is a fixed-size array.
	//
	// The one thing that is true either way: the dispatch_rays EVENT is registered in DllMain.
	// It has to be, because the ini is not read until the first init_device and ReShade's
	// DispatchRays hook invokes the event with no listener check, so registering it late would
	// race a recording thread. The cost with the census off is one extra indirect call inside a
	// loop that already runs.
	//
	// With rt_census = 1 the add-on reads DXR sub-objects at init_pipeline (entry-point names,
	// straight out of ReShade's DXIL RDAT reflection), counts SetPipelineState1, and records one
	// bucket per distinct DispatchRays signature - dimensions, SBT sizes and strides, and the
	// bound state object. It writes to ReShade.log and touches nothing else.
	bool     rt_census = false;
	// How many presents between RT census summary blocks. A summary is also emitted at
	// destroy_device. 600 is ten seconds at 60 fps.
	uint32_t rt_census_frames = 600;

	// ---- identification ------------------------------------------------------------------
	// The PRIMARY measured target in STRAY: compute, sm 5.0, t0 DEPTH r32_g8_typeless,
	// t2 VELOCITY r16g16b16a16_unorm, t5/t6 COLOUR r16g16b16a16_float, all 1920x1080.
	uint64_t shader_hash = 0x1708ec956099e259ull;
	// The SECOND candidate measured in STRAY is 0x52101a15e1a0c5cc (t0 DEPTH, t3 VELOCITY,
	// t7 COLOUR, t8 r16g16_float). Pin it with shader_hash=0x52101a15e1a0c5cc and
	// srv_velocity=3, srv_colour=7.
	//
	// 0 means "any shader that passes the class quorum below". NOT recommended: the measured
	// false positive 0x901e041a7cadc9db scores confidence 150 with colour=1 depth=2 velocity=0,
	// which the quorum does reject - but relying on the quorum alone gives up the one identifier
	// that is exact.
	uint32_t srv_depth    = 0;
	uint32_t srv_velocity = 2;
	uint32_t srv_colour   = 5;
	// Which u-register carries the TAA output. UE 4.27's FTAAStandaloneCS declares OutComputeTex
	// at u0 and the optional OutComputeTexDownsampled at u1.
	uint32_t uav_output   = 0;

	// ---- pipeline ------------------------------------------------------------------------
	// Copy our denoised result back over the game's TAA output. Set to 0 for the bring-up test
	// the research recommends: with this off the whole path runs - barriers, evaluate, state
	// restore - and writes to a texture nothing reads, so a frame that still renders correctly
	// is positive evidence that the state restore is faithful, independent of image quality.
	bool     copy_back = true;
	// Replay the graphics root signature and its arguments as well as the compute ones. NVIDIA's
	// own Streamline shadow does not track graphics root state, which is an empirical statement
	// that the DLSS snippets only dirty compute state; but a descriptor-heap change invalidates
	// graphics tables too, so this defaults ON.
	bool     restore_graphics_root = true;
	// Call the snippet's own PopulateParameters_Impl after allocating the parameter block. It is
	// a GATED export and its exact signature has not been verified against this snippet build, so
	// it is OFF by default. Nothing in the documented flow needs it.
	bool     populate_parameters = false;
	// Refuse to run without remix_nvngx.dll. See ngx_interop.hpp: the caller gate cannot be
	// detected at resolve time, so turning this off buys a silent 0xbad00002 instead of a clear
	// message.
	bool     require_trampoline = true;

	// ---- the HDR colour codec (rtx.neuralRendering's encode/decode, ported) ---------------
	// DLSS-NR is a DISPLAY-REFERRED image network. UE4 SceneColor is linear, unbounded and
	// upstream of bloom, eye adaptation and the film tone curve, so feeding it straight to the
	// network is out-of-distribution - the darkening this fixes. With this on, the pass builds a
	// soft-clipped exact-piecewise-sRGB proxy, hands the PROXY to the network as DLSSNR.Color, and
	// carries the network's answer back onto the untouched original as an additive residual.
	//
	// 0 restores the previous behaviour exactly: the raw TAA output is bound as DLSSNR.Color and
	// the denoised result is copied straight back. The codec also latches itself off (with a
	// logged reason) if its two compute shaders cannot be compiled or created.
	bool     hdr_codec = true;

	// The scene-linear -> display-referred scale, s, in  proxy = SrgbEncode(SoftClip(colour * s)).
	//
	// THIS VALUE IS UNCALIBRATED. Remix folds its own auto-exposure and user EV bias into s;
	// STRAY exposes no equivalent to us, so this is a plain constant and it NEEDS TUNING ON
	// HARDWARE. The default of 1.0 is a starting point, not a measurement.
	//
	// SEMANTICS ARE REMIX'S, which means this is a DIVISOR: s = 1.0 / max(paper_white_scale, 0.01).
	// So RAISING this value DARKENS the proxy. Raise it if the proxy looks blown out (highlights
	// crushed into the soft-clip shoulder); lower it if the proxy looks black. At the default of
	// 1.0 the divisor and multiplier conventions coincide exactly.
	float    paper_white_scale = 1.0f;

	// Global lerp back toward the untouched original, applied by the decode.
	//
	// 0.0 is an EXACT BYPASS OF THE DENOISE - result = lerp(original, graded, 0) = original, bit
	// for bit - and NOT a bypass of the codec: the encode, the evaluate and the decode all still
	// run, and the copy-back still writes result_tex over the frame. So a transfer_strength=0 run
	// must be pixel-identical to a run with copy_back=0, or with the add-on unloaded.
	//
	// It is NOT pixel-identical to hdr_codec=0, which is a different image entirely: that binds
	// the raw linear TAA output as DLSSNR.Color and copies the network's raw display-referred
	// answer straight back, i.e. the darkened frame the codec exists to fix.
	//
	// transfer_strength=0 vs copy_back=0 is the cheapest on-hardware check that the whole codec
	// path is wired up correctly - it exercises encode, evaluate, decode, state restore and
	// copy-back end to end.
	float    transfer_strength = 1.0f;
	// 0.0 keeps the original's chromaticity exactly and transfers only the network's luminance
	// change; 1.0 takes the network's colour as well. Lower this if the image picks up a colour
	// cast.
	float    color_strength = 1.0f;

	// ---- which GRAFT-BACK the decode uses --------------------------------------------------
	// The ENCODE is the same either way: same exact piecewise sRGB, same soft-clip knee 0.75 and
	// shoulder 5.770780, so the network is shown the same proxy and returns the same answer. Only
	// the way that answer is carried back onto the untouched HDR original differs.
	//
	//   0  ADDITIVE (ours, the DEFAULT, and the only mode whose identity is bit-exact)
	//        transferred = original + (neural - proxy) / s
	//      A scene-linear residual. Exactly +0.0 when the network asked for nothing, which is what
	//      makes transfer_strength=0 an EXACT no-op at every paper_white_scale. RGB is scaled
	//      uniformly, so the original's hue cannot drift.
	//
	//   1  RENODX UpgradeToneMap, reproduced from the reference add-on's own embedded HLSL
	//      (renodx-reference.addon64, .rdata RVA 0x42f90..0x440bd, contiguous plaintext).
	//        original_y < proxy_y : ratio = original_y / proxy_y
	//        else                 : ratio = (neural_y + max(0, original_y - proxy_y)) / neural_y
	//        result = lerp(original, HueOkLab(neural * ratio, neural), transfer_strength)
	//      It REBUILDS the pixel from the network's answer and then locks the hue to the NEURAL's
	//      hue in OkLab, with an AP1 gamut clamp.
	//
	// WHAT ACTUALLY DIFFERS, MEASURED, NOT ASSUMED (tools/hdr_codec_selftest.cpp).
	// Luminance is linear, so their "headroom term" max(0, original_y - proxy_y) is ALGEBRAICALLY
	// our additive residual:
	//     neural_y + max(0, original_y - proxy_y)  ==  Y(original + (neural - proxy))
	// The two modes therefore deliver the SAME luminance gain at every source magnitude. The whole
	// difference is CHROMA, and it has TWO halves that show at OPPOSITE ends of color_strength:
	//   * HIGHLIGHTS, at color_strength = 1. Where the soft clip has crushed the proxy to white the
	//     network's answer is neutral, so mode 1 drags a clipped highlight toward the white point
	//     while mode 0 leaves its chromaticity alone.
	//   * SHADOWS, at color_strength = 0. Mode 0 has a chroma floor - it crossfades to the
	//     network's own colour below Y = 0.001/s - and mode 1, faithfully to renodx, has none at
	//     all, so it keeps the original's chromaticity and rescales it by an unbounded ratio.
	//     Measured over 400,000 dark chromatic pixels: worst 27.6 8-bit code values, 42.5 % of them
	//     differing by 2 or more. Forcing mode 0's valve open collapses that to 0.0, which is what
	//     pins the cause on the valve. So color_strength = 0 is NOT a control that cancels the
	//     graft difference; it swaps which half of it you are looking at.
	//
	// Mode 1 is a colour experiment, NOT a highlight-recovery fix. Neither mode recovers a bright
	// highlight, and the ceiling is lower than the soft clip suggests because the proxy is stored
	// in an r16g16b16a16_float surface: the encoded proxy quantises to exactly 1.0 at 1.81x paper
	// white (3.47x is the FP32 figure and is not the one that governs), and of a requested +30 %
	// gain the decode already delivers only ~50 % at 1.15x and ~5 % at 1.86x. Those ratios are to
	// PAPER WHITE and do not move with paper_white_scale; the scene-linear magnitude they land at
	// moves in proportion. That is what paper_white_scale is for.
	//
	// Mode 1 is also NOT an exact bypass at transfer_strength=0: it works in display-referred space
	// throughout, so the result is (original * s) / s, which is exact only when s is a power of two
	// (i.e. paper_white_scale 1.0, 2.0, 0.5, ...). Mode 0 is exact at every value. That is why 0 is
	// the default.
	//
	// LIVE: this is a shader constant in the decode's root-constant block, snapshotted with the
	// rest of the pass. No feature recreate, no pipeline rebuild - flip it in the overlay and the
	// very next frame uses the other graft.
	//
	// NORMALISED TO {0, 1} AT PARSE, not left as the user typed it. The shader branch is
	// `g_hdrGraft == 0u ? ours : theirs`, so there is no third behaviour for a third value to
	// name - and an unlisted value made the overlay drop the combo entirely and print
	// "2  (not a listed value - sent as-is)", leaving no way back to either mode without editing
	// this file and restarting. Contrast DLSSNR.Style, where an unlisted value really does reach
	// NGX with a meaning of its own and is therefore preserved.
	uint32_t hdr_graft = 0;

	// ---- temporal feedback ----------------------------------------------------------------
	// Break the loop documented in README gap 5.
	//
	// UE 4.27's AddTemporalAAPass has NewHistoryTexture[0] == Outputs.SceneColor (TemporalAA.cpp
	// :696) and extracts it into OutputHistory->RT[0] (:969), so the resource copy_back writes
	// into IS the next frame's TAA history - and the history weight is 0.96 per frame
	// (r.TemporalAACurrentFrameWeight = .04), so the denoise compounds roughly 25-fold.
	//
	// With this on, the add-on keeps a private copy of the PRE-denoise TAA output and writes it
	// back over that resource at the START of the next accepted TAA dispatch, after verifying the
	// resource really is bound as a colour SRV there. The game's accumulator then only ever blends
	// its own un-denoised results, while post-processing still only ever sees denoised pixels.
	//
	// 0 restores the previous behaviour exactly, including the one-shot TEMPORAL FEEDBACK warning.
	// Inert when copy_back=0 (nothing is written back, so there is nothing to undo). This is the
	// knob to A/B on hardware.
	bool     history_restore = true;

	// ---- the motion-vector decode (README gap 2) -------------------------------------------
	// UE4 writes screen-space velocity into the texture with a SCALE AND A BIAS, and only where
	// it decided to; everywhere else the texel is EXACTLY ZERO and UE's own TAA reconstructs
	// camera motion by reprojecting depth through View.ClipToPrevClip. With this on, one compute
	// pass of ours turns that into a private r16g16_float buffer holding ABSOLUTE PIXELS ON THE
	// COLOUR GRID, y-down, in the direction DLSS wants (add the vector to a pixel's current
	// location and you get its previous location), and DLSSNR.MVecScaleX/Y are FORCED to 1.0
	// because the grid correction must not double-apply.
	//
	// 0 restores the previous behaviour EXACTLY: the game's raw encoded velocity buffer is bound
	// as DLSSNR.MVec with the derived grid ratio, and the gap-2 warning is printed unconditionally
	// as it always was. That is the A/B.
	//
	// This defaults ON for the same reason hdr_codec and history_restore do, and because the guide
	// it replaces is documented-meaningless rather than merely imperfect - there is no good
	// baseline being protected. A reviewer who wants the first hardware launch to be a pure
	// control should set this to 0 in the ini rather than change the default.
	bool     mvec_decode = true;

	// Reconstruct camera motion from depth + ClipToPrevClip wherever the velocity texel is invalid
	// (raw .x == 0). THAT IS MOST OF THE FRAME. Static geometry never writes velocity even with
	// r.BasePassOutputsVelocity=1 - that setting moves velocity output for MOVABLE primitives into
	// the base pass, it does not make static ones write.
	//
	// 0 = decode only: valid texels are decoded, invalid ones are written as EXACTLY ZERO. That is
	// strictly a bring-up A/B to isolate the two halves on hardware. Shipping it would hand DLSS
	// zero motion for the entire static world, which is WORSE than mvec_decode=0.
	bool     mvec_reconstruct = true;

	// UE's own AA_CROSS nearest-depth dilation of the velocity lookup (TAAStandalone.usf:1939-1983).
	// OFF by default: UE dilates for its own single-tap history, NVIDIA's DLSS plugin defaults to
	// the NON-dilated branch, and DLSS does its own neighbourhood work - pre-dilated vectors smear
	// object silhouettes. Here so it can be A/B'd independently of the encoding fix.
	bool     mvec_dilate = false;

	// ESCAPE HATCHES. Each one turns a SILENT failure into a 30-second experiment.

	// Pin the float4 row of View.ClipToPrevClip. 0 = discover it and VALIDATE it, which is the
	// recommended setting: the row is derived twice independently (a content signature over the
	// View constant buffer, via ue4_jitter.hpp, and this project's own DXBC instruction analysis)
	// and the two must AGREE or the reconstruction is refused. STRAY measured 122 both ways.
	// A pinned row SKIPS the content signature entirely, so it is logged as loudly as shader_hash=0.
	uint32_t mvec_clip_row = 0;

	// Transpose ClipToPrevClip before handing it to the shader. The rows are passed UNtransposed
	// because that is what FMatrix's memory layout and UE's own mul(v, M) say, and UE compiles with
	// /Zpr. If motion is roughly right at screen centre and wrong at the edges - and worse under
	// camera ROLL - this is the knob. A near-identity matrix cannot tell the two apart, which is
	// why this exists rather than a self-test.
	bool     mvec_clip_transpose = false;

	// ---- NGX evaluate parameters ---------------------------------------------------------
	// UE 4.27 renders with a REVERSED-Z depth buffer (near plane at 1.0), so this defaults to 1 -
	// which is the opposite of the working Remix deployment, whose renderer writes post-divide
	// NDC depth without inverting. If DLSS-NR ghosts or smears in exactly the wrong direction,
	// this is the first thing to flip.
	bool     depth_inverted = true;
	// 0 == derive it. With mvec_decode=0 that is the extent ratio (colour grid / mvec grid), which
	// is what the working deployment computes and which can only ever correct the GRID, never
	// UE4's velocity encoding. With mvec_decode=1 the derived value is FORCED to exactly 1.0,
	// because the decode pass already emits absolute colour-grid pixels and applying the ratio on
	// top would double-apply it.
	//
	// A non-zero value overrides either. NVIDIA documents MVecScaleX/Y as carrying SIGN, so with
	// the decode on these are the SIGN A/B - no rebuild. TWO DIFFERENT TESTS LIVE HERE:
	//   * ONE key at -1 tests a PER-AXIS sign error. X and Y are NOT symmetric (the decode negates
	//     X and not Y), so both single-axis flips have to be tried.
	//   * BOTH keys at -1 TOGETHER tests the DIRECTION CONVENTION - previous-minus-current versus
	//     current-minus-previous - which is the one [WEB]-only link in the chain and which negates
	//     both axes at once. No single-axis flip can reach that configuration, so no single-axis
	//     result can confirm or refute it.
	// See the README's A/B table.
	float    mvec_scale_x = 0.0f;
	float    mvec_scale_y = 0.0f;

	// ---- the five tuning knobs -----------------------------------------------------------
	// These defaults are the snippet's OWN fallbacks, recovered from the disassembly: each read
	// is followed by a `cmp eax,0xbad00000` test that substitutes the value below when the host
	// supplied nothing. 1.0 is therefore the fallback, NOT a calibrated neutral midpoint, and the
	// scale these values sit on is not known.
	float    intensity                = 1.0f;
	float    local_tone_strength      = 1.0f;
	float    local_structure_strength = 1.0f;
	// NEGATIVE means "inherit local_structure_strength": the snippet does an explicit comiss
	// against 0 and a jae, and copies the local structure strength on the other branch.
	// 0.0 is NOT neutral - it flattens skin structure.
	float    skin_structure_strength  = -1.0f;
	uint32_t style                    = 0;
	// ---- BEGIN overlay_ui hook ----
	// DLSSNR.UICorrection. A real parameter of THIS snippet build, unlike DLSSNR.Upscaling: the
	// exact string is in nvngx_dlssnr.dll's table (measured), and the snippet reads it with a
	// proper failure guard whose fallback is 0. Its VISUAL effect on this content has not been
	// verified, so the default here is the snippet's own. Exposed by the overlay as a live knob.
	uint32_t ui_correction            = 0;
	// ---- END overlay_ui hook ----

	// Gates BOTH structure strengths. With this at 0 the snippet internally forces both to -1 and
	// neither does anything. Binding an explicit ControlMask also forces it to 0 inside the
	// snippet; this add-on binds no ControlMask, so the two never conflict here.
	bool     use_auto_mask = true;

	// =======================================================================================
	// DLSS SUPER RESOLUTION (NGX feature 1, nvngx_dlss.dll). See STAGING-sr.md.
	// =======================================================================================
	//
	// THE MASTER SWITCH, AND WHAT IT COSTS WHEN IT IS 0.
	//
	// dlss_sr = 0 is the shipping configuration and it is BIT-IDENTICAL to the build before SR
	// existed. Exactly this much executes:
	//   * this key is parsed, and one bool is stored;
	//   * nr_init_device tests it once and does NOT LoadLibraryW nvngx_dlss.dll;
	//   * nr_lazy_ngx_init tests it once and does not call Init_Ext on the SR snippet, does not
	//     allocate the SR parameter block, and does not touch remix_nvngx.dll's slot B - and on
	//     the DLSS-NR FAILURE path it takes the same early exit it took before SR existed, so
	//     the HDR-codec and mvec pipelines are not built there either;
	//   * nr_try_run tests it once per accepted TAA dispatch, before anything else SR-related,
	//     and takes the DLSS-NR branch unchanged;
	//   * nr_pick_output_uav tests it once per accepted dispatch and applies the ORIGINAL
	//     "extent must equal the colour SRV's" rule.
	// No SR resource is created, no SR event is registered, no SR code runs on the GPU, and the
	// trampoline's slot B stays entirely null. Five predictable-branch tests per frame is the
	// whole cost.
	bool     dlss_sr = false;

	// The DLSS-NR path. 1 is today. Set to 0 alongside dlss_sr=1 to skip the 166 MB
	// nvngx_dlssnr.dll load entirely when you are only running SR - SR does not need it, and the
	// two features both want to own the same TAA dispatch.
	//
	// NOTE: when dlss_sr=1 the SR pass TAKES the accepted dispatch and the DLSS-NR evaluate does
	// not run, whether or not the NR snippet is loaded. This key only controls whether the NR
	// snippet is loaded and initialised at all.
	bool     dlss_nr = true;

	// =======================================================================================
	// CHAIN MODE - DLSS-NR *then* DLSS-SR, on ONE accepted TAA dispatch.
	// =======================================================================================
	//
	// dlss_sr=1 and dlss_nr=1 do not chain: the SR pass TAKES the accepted dispatch and the NR
	// evaluate never runs. That is an implementation choice, not a limitation - both snippets
	// already load side by side through the trampoline's two slots, DLSS-NR already evaluates
	// into its OWN texture rather than writing the frame, and DLSS-SR's colour input is a
	// parameter. This key wires the one to the other:
	//
	//     game TAA dispatch (suppressed)
	//       -> [codec encode]  render-res linear HDR -> display-referred proxy
	//       -> DLSS-NR         proxy -> out_tex, at the RENDER extent
	//       -> [codec decode]  the denoised answer grafted back onto the linear original
	//       -> DLSS-SR         COLOUR = the denoised render-res image -> u0 at 4K
	//
	// Denoise first, THEN upscale. The other order - which is what dlss_sr=0/dlss_nr=1 does
	// today - denoises at 1920x1080 and then lets a spatial filter magnify whatever noise is
	// left into 3840x2160.
	//
	// WHAT IT COSTS WHEN IT IS 0, which is the default and the shipping configuration: one bool
	// is parsed and stored, and it is read in a handful of `&&`/`||` chains that short-circuit
	// false. Nothing is loaded, nothing is allocated, no GPU work changes, and the
	// "dlss_sr=0 is BIT-IDENTICAL to the build before SR existed" contract above extends to it
	// verbatim. With dlss_chain=0 the branch at the end of nr_try_run is exactly the one that
	// ships today.
	//
	// REQUIREMENTS, and the add-on says so in the log rather than degrading silently:
	//   * BOTH snippets must load and arm - nvngx_dlssnr.dll AND nvngx_dlss.dll. If either does
	//     not, chain mode is refused once, by name, and the run falls back to whichever single
	//     feature did arm. It never half-runs.
	//   * UE4 must be in the MainUpsampling permutation (r.TemporalAA.Upsampling=1,
	//     r.SecondaryScreenPercentage=100, r.ScreenPercentage=50), because otherwise the TAA
	//     pass's output UAV is the same size as its colour input and there is nothing to upscale
	//     into. That permutation has DIFFERENT DXBC, so sr_shader_hash must be re-pinned - the
	//     want-hash selector below treats dlss_chain exactly like dlss_sr.
	//   * hdr_codec=1 is effectively mandatory. With it off, DLSS-SR is handed the network's raw
	//     DISPLAY-REFERRED answer as if it were linear HDR - README gap 1, magnified by the
	//     upscaler. The add-on warns once and runs anyway.
	//
	// The DLSS-NR copy-back does not happen in chain mode and CANNOT: its result is at the render
	// extent and u0 is at the output extent. DLSS-SR's write is the only write to u0.
	bool     dlss_chain = false;

	// ---- identification --------------------------------------------------------------------
	// THE ONE-LINE HASH RE-PIN. Flipping r.TemporalAA.Upsampling changes TAA_PASS_CONFIG, and
	// r.ScreenPercentage below 100 changes TAA_SCREEN_PERCENTAGE_RANGE; both are #defines, so the
	// DXBC and therefore the fnv1a64 change. 0 means "use shader_hash" (the DLSS-NR value). Set
	// this to the hash the probe reports for the MainUpsampling permutation and nothing else has
	// to move - in particular DLSS-NR keeps its own pin, so the two can be A/B'd on one install.
	uint64_t sr_shader_hash = 0;

	// ---- geometry ---------------------------------------------------------------------------
	// The OUTPUT extent. 0 = derive it from the dispatch's group counts, which is the only source
	// that is correct by construction: TemporalAA.cpp:958 dispatches
	// GetGroupCount(PracticableDestRect.Size(), GTemporalAATileSizeX) with tile size 8, and
	// GetGroupCount is DivideAndRoundUp - so OutW is in (8*gx - 8, 8*gx]. In every configuration
	// this add-on targets the output view rect IS the secondary resolution (3840x2160), which is a
	// multiple of 8, so 8*gx is exact. Pin these if the log says the derived value and the chosen
	// u0 candidate's texture extent disagree.
	uint32_t sr_out_width  = 0;
	uint32_t sr_out_height = 0;
	// GTemporalAATileSizeX/Y. An engine compile-time constant (TemporalAA.cpp:16-17), here so a
	// licensee edit is an ini change rather than a rebuild.
	uint32_t sr_group_tile = 8;
	// Feed DLSS the VIEW RECT recovered from ViewSizeAndInvSize rather than the colour texture's
	// extent. They differ whenever QuantizeSceneBufferSize rounded up (it rounds to a multiple of
	// 4), e.g. 1130 vs 1132 at r.ScreenPercentage=58.8. 0 uses the texture extent, which is a
	// silent ~0.2% mis-scale at those percentages - it is here only as an A/B.
	bool     sr_use_view_rect = true;

	// ---- the bring-up ladder (STAGING-sr.md walks these in order) ---------------------------
	// RUNG 6. Stop re-issuing the game's TAA dispatch. With this 0 the game's TAAU still runs and
	// DLSS writes on top of it, which wastes one dispatch and is completely safe; with it 1 the
	// dispatch is suppressed and DLSS is the only thing that writes the output. The flag that
	// tells ReShade "already issued" is set LAST under suppression - after a successful evaluate
	// AND after the state restore - so any bail or throw leaves it false and ReShade issues the
	// game's own TAAU, which unconditionally writes every pixel of the output view rect
	// (TemporalAA.usf:2268-2281). A failed frame is a correct frame with one wasted dispatch.
	bool     sr_suppress_taa = false;

	// RUNG 7. Bind the game's own TAA output UAV (u0) directly as DLSS's Output, instead of
	// evaluating into an add-on texture and copying back. Removes a full-extent 4K copy per frame
	// and both of its barriers. With this 0 the add-on owns the output texture, which is the safer
	// bring-up shape: nothing the game reads is written until the copy-back.
	bool     sr_direct_output = false;

	// Write the add-on's SR output back over u0. Inert when sr_direct_output=1 (DLSS wrote u0
	// itself). 0 is the rung-4 configuration: the whole path runs - create, jitter, decode,
	// evaluate, state restore - into a texture NOTHING READS, so a frame that still renders
	// correctly is positive evidence that the state restore is faithful, independent of image
	// quality. Exactly the role copy_back plays on the DLSS-NR path.
	bool     sr_copy_back = true;

	// ---- motion vectors ----------------------------------------------------------------------
	// Reuse mvec_decode.hpp's pass to build DLSS's MotionVectors input. The PIPELINE is shared
	// with the DLSS-NR path; only the target texture is SR's own, and it is allocated at the
	// RENDER extent rather than the output extent because SR's colour input is the TAA pass's
	// INPUT. 0 binds the game's raw encoded velocity, which is README gap 2 unmitigated - for SR
	// that is worse than for NR, because SR has no second temporal filter behind it.
	bool     sr_mvec_decode = true;
	// Reconstruct camera motion from depth through View.ClipToPrevClip wherever UE4 left the
	// velocity texel invalid. THE SAME KEY mvec_reconstruct is on the DLSS-NR path, and it exists
	// for the same reason: so that decode-only is only ever reached because someone ASKED for it.
	// 0 writes EXACTLY ZERO motion for the whole static world, the sky and translucency, which is
	// a bring-up A/B for isolating the decode from the reconstruction and is worse than
	// sr_mvec_decode=0 for actual play. A ClipToPrevClip that cannot be located or validated
	// falls back to the RAW buffer, not to this.
	bool     sr_mvec_reconstruct = true;
	// MV.Scale.X / MV.Scale.Y overrides. 0 = derive (exactly 1.0 with the decode on, because the
	// decode already emits absolute pixels on the render grid; the extent ratio otherwise).
	// A SINGLE key at -1 tests a per-axis sign error; BOTH at -1 together tests the direction
	// convention, which no single-axis flip can reach.
	float    sr_mv_scale_x = 0.0f;
	float    sr_mv_scale_y = 0.0f;

	// ---- jitter -------------------------------------------------------------------------------
	// THE SIGN A/B, and it is the most likely bug in this feature. A flipped Y sign makes DLSS run,
	// report success, and shimmer. The shipped value is the engine's own float with no arithmetic
	// applied (ue4_jitter.hpp tier `full`), so 1.0/1.0 is the value with evidence behind it -
	// these exist so the alternative can be tested without a rebuild.
	float    sr_jitter_scale_x = 1.0f;
	float    sr_jitter_scale_y = 1.0f;
	// Accept ue4_jitter's WEAKEST tier (matrix pair only, extent from the caller) when
	// TemporalAAParams does not validate. The number is still correct; it just has no second
	// opinion. Off by default - a failure should be diagnosed, not downgraded.
	bool     sr_jitter_projection_only = false;

	// ---- DLSS.Feature.Create.Flags ------------------------------------------------------------
	// Bit 0. IsHDR is NOT an exposure gate - it SELECTS A DIFFERENT TRAINED NETWORK, exactly like
	// DepthInverted and MVLowRes, and the binary says so [SRC nvngx_dlss.dll sha256
	// c85f971c..0b7e]:
	//   * the engine ships PAIRED _hdr_/_ldr_ CUDA kernels for every other combination -
	//     hiluma_engine_{input,output}_depth{inv,reg}_mv{lo,hi}_{hdr,ldr}_v{1,2}_rel and the
	//     _max_v2_ variants (44 names at file offsets 0x12f9f8-0x1301b8), plus
	//     cuda_engine_input_kernel_rel_{hdr,ldr}_{colvar,mvdiff}_mv{lo,hi} and
	//     cuda_engine_output_kernel_rel_{hdr,ldr}_gauss{3x3,5x5} at 0x12f710-0x12f970.
	//   * the loader registers each one under a descriptor word it stores at [rbp+0x38]
	//     immediately before passing the name. For the INPUT set the four bytes are, LSB first,
	//     {v2, IsHDR, MVLowRes, DepthInverted}: depthinv_mvlo_hdr_v2 = 0x01010101,
	//     depthinv_mvlo_ldr_v2 = 0x01010001, depthreg_mvhi_ldr_v2 = 0x00000001
	//     [SRC .text 0x18004f87d-0x18004fc2e]. The OUTPUT set spends the low two bytes on
	//     {max, v2} and carries DepthInverted in the separate byte at [rbp+0x3c] - 1 for every
	//     depthinv_ name, r12b (zeroed at 0x18004ee0b) for every depthreg_ one - leaving IsHDR
	//     and MVLowRes as the high two bytes: depthinv_mvlo_hdr_v2 = 0x01010100,
	//     depthinv_mvhi_hdr_v2 = 0x00010100 [SRC .text 0x18004fc64-0x18005082f].
	//   * CreateDlssInstance logs the three together: "HDR %d / Motion Vectors LowRes %d /
	//     Motion Vectors Jittered %d / Depth Inverted %d" [SRC 0x12dfe0-0x12e098].
	// Two of those three discriminators are already set from STRAY's measured properties, so the
	// third has to be too. SR binds ed.color = STRAY's t5 SceneColor: r16g16b16a16_float, linear,
	// unbounded, upstream of bloom, eye adaptation and the film tone curve, with no codec in
	// front of it. That is the HDR input, so this is 1. Clearing it would run the _ldr_ engine on
	// out-of-distribution data: EvaluateFeature returns Success, no diagnostic fires anywhere,
	// and the image comes back wrong in the same way README gap 1 documents.
	// The key stays so LDR remains A/B-able. Setting it does NOT create an exposure obligation:
	// with IsHDR set the snippet reads DLSS.Pre.Exposure and DLSS.Exposure.Scale, and this add-on
	// writes neither - the snippet's own miss-default for both is 1.0f
	// [SRC 0x18003ca9d / 0x18003cac4], and the <= 0 clamp at 0x18003cc51 is a second net under
	// that. sr_auto_exposure (bit 6) is meaningful only for HDR input, so the two now agree.
	bool     sr_hdr = true;
	// Bit 1. Velocity lives at the render resolution, which under any real upscale is LOWER than
	// the output - so this is set. At 1:1 (DLAA) it should be cleared.
	bool     sr_mv_lowres = true;
	// Bit 2. UE4's velocity buffer is not jitter-compensated. [ASSUMED]; never measured.
	bool     sr_mv_jittered = false;
	// Bit 3. UE 4.27 renders reversed-Z. Measured [HW] for STRAY, same value depth_inverted holds
	// for the DLSS-NR path - kept as its own key so the two features can be A/B'd independently.
	bool     sr_depth_inverted = true;
	// Bit 6. Set by default because the alternative is owing DLSS an ExposureTexture or a
	// calibrated DLSS.Pre.Exposure, and UE's pre-exposure is a real source of silent brightness
	// error. Note the snippet accepts (AutoExposure clear, ExposureTexture NULL) silently.
	bool     sr_auto_exposure = true;
	// Bit 7. Off; nothing in this path needs alpha upscaling.
	bool     sr_alpha_upscaling = false;

	// DLSS.Use.HW.Depth. NOT a create flag - a separate CREATE-TIME parameter, and setting it at
	// evaluate is a documented no-op. STRAY's t0 is an r32_g8_typeless hardware depth-stencil, so
	// 1 is correct; the snippet's default is 0 = Linear and getting it wrong produces NO
	// diagnostic. Set explicitly, always. Do not confuse it with sr_depth_inverted - one selects
	// the content convention, the other the sign, and they are unrelated.
	bool     sr_hw_depth = true;

	// PerfQualityValue. 0 = MaxPerf, 1 = Balanced, 2 = MaxQuality, 3 = UltraPerformance,
	// 4 = UltraQuality, 5 = DLAA. Recovered from the snippet's own name/value array. NOTE it does
	// NOT choose the render preset - the snippet picks that slot from Width/OutWidth - and that an
	// absent value defaults to 0, which is right for 1920->3840 and wrong for DLAA.
	uint32_t sr_perf_quality = 0;
	// DLSS.Hint.Render.Preset.* value, written into the ONE slot the ratio selects. 0 = auto.
	uint32_t sr_render_preset = 0;

	// ---- diagnostics -------------------------------------------------------------------------
	// Call the snippet's PopulateParameters_Impl on a SCRATCH parameter block and run
	// DLSS_GetOptimalSettings through the callback it installs, once, purely to LOG the
	// recommendation. It has no power to make UE render at that resolution - the only lever is
	// r.ScreenPercentage - so it is never on the critical path. A scratch block is mandatory:
	// Width/Height/OutWidth/OutHeight have INVERTED meaning in that call.
	//
	// This is also where the two researchers disagreed: the task brief said the query cannot work
	// because DLSSOptimalSettingsCallback is never populated in our own parameter map, and the
	// disassembly says PopulateParameters_Impl writes it INTO WHATEVER BLOCK IT IS HANDED. The
	// disassembly is the stronger evidence, so the query is implemented - but defaulted OFF and
	// kept off every path that matters, which is what the design doc asks for regardless.
	bool     sr_optimal_settings = false;

	// ---- misc ----------------------------------------------------------------------------
	// NGX application id. The snippet resolves its weights from its own embedded WEIGHTS_HT
	// resource, so this only names the log file it writes beside the add-on.
	uint64_t app_id = 0x24480451ull;

	// Whether the ini file was found at all (for the log line).
	bool     ini_found = false;
	std::string ini_path;
};

inline bool parse_bool(const char *v, bool fallback)
{
	if (v == nullptr || *v == '\0')
		return fallback;
	if (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y') return true;
	if (v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N') return false;
	return fallback;
}

inline uint64_t parse_u64(const char *v, uint64_t fallback)
{
	if (v == nullptr || *v == '\0')
		return fallback;
	char *end = nullptr;
	// base 0: "0x..." is hex, everything else decimal.
	const unsigned long long r = std::strtoull(v, &end, 0);
	if (end == v)
		return fallback;
	return static_cast<uint64_t>(r);
}

inline float parse_float(const char *v, float fallback)
{
	if (v == nullptr || *v == '\0')
		return fallback;
	char *end = nullptr;
	const double r = std::strtod(v, &end);
	if (end == v)
		return fallback;
	return static_cast<float>(r);
}

inline void trim(std::string &s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
	s = s.substr(b, e - b);
}

// Reads <directory>stray_dlssnr.ini. A missing file is NOT an error: every default above is the
// shipping default, so the add-on behaves identically with and without the file.
//
// 'log' receives one line per recognised key and one warning per unrecognised one, so a typo is
// visible instead of silently taking a default.
template <typename LogFn>
inline void load(config &c, const std::wstring &directory, LogFn log)
{
	const std::wstring path = directory + L"stray_dlssnr.ini";
	c.ini_path = std::string();

	FILE *f = _wfopen(path.c_str(), L"rb");
	if (f == nullptr)
	{
		c.ini_found = false;
		log("no stray_dlssnr.ini beside the add-on; every setting is at its built-in default.");
		return;
	}
	c.ini_found = true;

	char line[512];
	uint32_t line_no = 0;
	while (std::fgets(line, sizeof(line), f) != nullptr)
	{
		++line_no;
		std::string s(line);
		// Strip comments from the FIRST ';' or '#' anywhere on the line. No value this file
		// accepts can contain either character - they are all numbers, hex literals or booleans -
		// so a trailing comment is supported and nothing legitimate is ever truncated.
		const size_t sc = s.find_first_of(";#");
		if (sc != std::string::npos)
			s = s.substr(0, sc);
		trim(s);
		if (s.empty())
			continue;
		if (s[0] == '[')
			continue; // section headers are accepted and ignored; there is only one section

		const size_t eq = s.find('=');
		if (eq == std::string::npos)
		{
			char buf[600];
			std::snprintf(buf, sizeof(buf), "stray_dlssnr.ini line %u is not key=value and was ignored: \"%s\"", line_no, s.c_str());
			log(buf);
			continue;
		}

		std::string key = s.substr(0, eq);
		std::string val = s.substr(eq + 1);
		trim(key);
		trim(val);
		for (char &ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

		const char *v = val.c_str();
		bool known = true;

		if      (key == "enabled")                  c.enabled = parse_bool(v, c.enabled);
		else if (key == "diagnostics")              c.diagnostics = parse_bool(v, c.diagnostics);
		else if (key == "rt_census")                c.rt_census = parse_bool(v, c.rt_census);
		else if (key == "rt_census_frames")         c.rt_census_frames = static_cast<uint32_t>(parse_u64(v, c.rt_census_frames));
		else if (key == "shader_hash")              c.shader_hash = parse_u64(v, c.shader_hash);
		else if (key == "srv_depth")                c.srv_depth = static_cast<uint32_t>(parse_u64(v, c.srv_depth));
		else if (key == "srv_velocity")             c.srv_velocity = static_cast<uint32_t>(parse_u64(v, c.srv_velocity));
		else if (key == "srv_colour" || key == "srv_color")
		                                            c.srv_colour = static_cast<uint32_t>(parse_u64(v, c.srv_colour));
		else if (key == "uav_output")               c.uav_output = static_cast<uint32_t>(parse_u64(v, c.uav_output));
		else if (key == "copy_back")                c.copy_back = parse_bool(v, c.copy_back);
		else if (key == "hdr_codec")                c.hdr_codec = parse_bool(v, c.hdr_codec);
		else if (key == "paper_white_scale")        c.paper_white_scale = parse_float(v, c.paper_white_scale);
		else if (key == "transfer_strength")        c.transfer_strength = parse_float(v, c.transfer_strength);
		else if (key == "color_strength" || key == "colour_strength")
		                                            c.color_strength = parse_float(v, c.color_strength);
		else if (key == "hdr_graft")                c.hdr_graft = (parse_u64(v, c.hdr_graft) != 0ull) ? 1u : 0u;
		else if (key == "history_restore")          c.history_restore = parse_bool(v, c.history_restore);
		else if (key == "restore_graphics_root")    c.restore_graphics_root = parse_bool(v, c.restore_graphics_root);
		else if (key == "populate_parameters")      c.populate_parameters = parse_bool(v, c.populate_parameters);
		else if (key == "require_trampoline")       c.require_trampoline = parse_bool(v, c.require_trampoline);
		else if (key == "mvec_decode")              c.mvec_decode = parse_bool(v, c.mvec_decode);
		else if (key == "mvec_reconstruct")         c.mvec_reconstruct = parse_bool(v, c.mvec_reconstruct);
		else if (key == "mvec_dilate")              c.mvec_dilate = parse_bool(v, c.mvec_dilate);
		else if (key == "mvec_clip_row")            c.mvec_clip_row = static_cast<uint32_t>(parse_u64(v, c.mvec_clip_row));
		else if (key == "mvec_clip_transpose")      c.mvec_clip_transpose = parse_bool(v, c.mvec_clip_transpose);
		else if (key == "depth_inverted")           c.depth_inverted = parse_bool(v, c.depth_inverted);
		else if (key == "mvec_scale_x")             c.mvec_scale_x = parse_float(v, c.mvec_scale_x);
		else if (key == "mvec_scale_y")             c.mvec_scale_y = parse_float(v, c.mvec_scale_y);
		else if (key == "intensity")                c.intensity = parse_float(v, c.intensity);
		else if (key == "local_tone_strength")      c.local_tone_strength = parse_float(v, c.local_tone_strength);
		else if (key == "local_structure_strength") c.local_structure_strength = parse_float(v, c.local_structure_strength);
		else if (key == "skin_structure_strength")  c.skin_structure_strength = parse_float(v, c.skin_structure_strength);
		else if (key == "style")                    c.style = static_cast<uint32_t>(parse_u64(v, c.style));
		// ---- BEGIN overlay_ui hook ----
		else if (key == "ui_correction")            c.ui_correction = static_cast<uint32_t>(parse_u64(v, c.ui_correction));
		// ---- END overlay_ui hook ----
		else if (key == "use_auto_mask")            c.use_auto_mask = parse_bool(v, c.use_auto_mask);
		else if (key == "app_id")                   c.app_id = parse_u64(v, c.app_id);
		else if (key == "dlss_sr")                  c.dlss_sr = parse_bool(v, c.dlss_sr);
		else if (key == "dlss_nr")                  c.dlss_nr = parse_bool(v, c.dlss_nr);
		else if (key == "dlss_chain")               c.dlss_chain = parse_bool(v, c.dlss_chain);
		else if (key == "sr_shader_hash")           c.sr_shader_hash = parse_u64(v, c.sr_shader_hash);
		else if (key == "sr_out_width")             c.sr_out_width = static_cast<uint32_t>(parse_u64(v, c.sr_out_width));
		else if (key == "sr_out_height")            c.sr_out_height = static_cast<uint32_t>(parse_u64(v, c.sr_out_height));
		else if (key == "sr_group_tile")            c.sr_group_tile = static_cast<uint32_t>(parse_u64(v, c.sr_group_tile));
		else if (key == "sr_use_view_rect")         c.sr_use_view_rect = parse_bool(v, c.sr_use_view_rect);
		else if (key == "sr_suppress_taa")          c.sr_suppress_taa = parse_bool(v, c.sr_suppress_taa);
		else if (key == "sr_direct_output")         c.sr_direct_output = parse_bool(v, c.sr_direct_output);
		else if (key == "sr_copy_back")             c.sr_copy_back = parse_bool(v, c.sr_copy_back);
		else if (key == "sr_mvec_decode")           c.sr_mvec_decode = parse_bool(v, c.sr_mvec_decode);
		else if (key == "sr_mvec_reconstruct")      c.sr_mvec_reconstruct = parse_bool(v, c.sr_mvec_reconstruct);
		else if (key == "sr_mv_scale_x")            c.sr_mv_scale_x = parse_float(v, c.sr_mv_scale_x);
		else if (key == "sr_mv_scale_y")            c.sr_mv_scale_y = parse_float(v, c.sr_mv_scale_y);
		else if (key == "sr_jitter_scale_x")        c.sr_jitter_scale_x = parse_float(v, c.sr_jitter_scale_x);
		else if (key == "sr_jitter_scale_y")        c.sr_jitter_scale_y = parse_float(v, c.sr_jitter_scale_y);
		else if (key == "sr_jitter_projection_only") c.sr_jitter_projection_only = parse_bool(v, c.sr_jitter_projection_only);
		else if (key == "sr_hdr")                   c.sr_hdr = parse_bool(v, c.sr_hdr);
		else if (key == "sr_mv_lowres")             c.sr_mv_lowres = parse_bool(v, c.sr_mv_lowres);
		else if (key == "sr_mv_jittered")           c.sr_mv_jittered = parse_bool(v, c.sr_mv_jittered);
		else if (key == "sr_depth_inverted")        c.sr_depth_inverted = parse_bool(v, c.sr_depth_inverted);
		else if (key == "sr_auto_exposure")         c.sr_auto_exposure = parse_bool(v, c.sr_auto_exposure);
		else if (key == "sr_alpha_upscaling")       c.sr_alpha_upscaling = parse_bool(v, c.sr_alpha_upscaling);
		else if (key == "sr_hw_depth")              c.sr_hw_depth = parse_bool(v, c.sr_hw_depth);
		else if (key == "sr_perf_quality")          c.sr_perf_quality = static_cast<uint32_t>(parse_u64(v, c.sr_perf_quality));
		else if (key == "sr_render_preset")         c.sr_render_preset = static_cast<uint32_t>(parse_u64(v, c.sr_render_preset));
		else if (key == "sr_optimal_settings")      c.sr_optimal_settings = parse_bool(v, c.sr_optimal_settings);
		else                                        known = false;

		char buf[700];
		if (known)
			std::snprintf(buf, sizeof(buf), "  ini: %s = %s", key.c_str(), val.c_str());
		else
			std::snprintf(buf, sizeof(buf), "  ini: UNRECOGNISED key \"%s\" on line %u was IGNORED (typo?)", key.c_str(), line_no);
		log(buf);
	}

	std::fclose(f);
}

} // namespace cfg
