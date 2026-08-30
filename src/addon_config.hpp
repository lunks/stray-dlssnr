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
