// addon_config.hpp - the stray_dlssnr.ini beside the add-on.
//
// Deliberately a tiny hand-rolled reader rather than ReShade's config API: ReShade's
// get_config_value keys off ReShade.ini, which the user is also editing for effects, and a
// missing key there silently yields a default with no diagnostic. Here every parse is reported.
//
// Read ONCE, at the first init_device. There is no hot reload: a knob that changed halfway
// through a frame would produce an evaluate whose create-time and evaluate-time parameters
// disagree, which is exactly the class of bug that is impossible to see in a screenshot.

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

	// ---- NGX evaluate parameters ---------------------------------------------------------
	// UE 4.27 renders with a REVERSED-Z depth buffer (near plane at 1.0), so this defaults to 1 -
	// which is the opposite of the working Remix deployment, whose renderer writes post-divide
	// NDC depth without inverting. If DLSS-NR ghosts or smears in exactly the wrong direction,
	// this is the first thing to flip.
	bool     depth_inverted = true;
	// 0 == derive from the extents (colour grid / mvec grid), which is what the working
	// deployment computes. A non-zero value overrides it. See the README's motion-vector
	// caveat: the scale cannot correct UE4's velocity ENCODING, only its grid.
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
