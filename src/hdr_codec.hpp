// hdr_codec.hpp - the DLSS-NR HDR colour codec: two compute shaders, ported verbatim from the
// WORKING dxvk-remix deployment, plus the runtime compile and the D3D12 pipeline objects.
//
// WHY THIS EXISTS
//   DLSS-NR is a DISPLAY-REFERRED image network. What STRAY hands us as the TAA output is UE4
//   SceneColor: linear, unbounded, upstream of bloom, eye adaptation and the film tone curve.
//   Feeding unbounded linear radiance to a display-referred network is out-of-distribution, and
//   darkening is the expected response - the same defect that produced blue/red frames in the
//   Remix deployment, where it was fixed and verified on hardware.
//
// THE DESIGN, PORTED - NOT INVENTED
//   proxy  = SrgbEncode(SoftClip(original * s))     display-referred FP16 proxy
//   neural = DLSS-NR(proxy, depth, mvec)            the network runs on the PROXY
//   result = original + (neural - proxy) / s        additive residual onto the ORIGINAL
//
//   Source of truth (read in full, quoted constant for constant):
//     dxvk-remix/src/dxvk/shaders/rtx/pass/neural_rendering/neural_rendering_codec.slangh
//     dxvk-remix/src/dxvk/shaders/rtx/pass/neural_rendering/neural_rendering_encode.comp.slang
//     dxvk-remix/src/dxvk/shaders/rtx/pass/neural_rendering/neural_rendering_decode.comp.slang
//     dxvk-remix/src/dxvk/rtx_render/rtx_neural_rendering.cpp                (how they dispatch)
//
//   THE IDENTITY PROPERTY, ALGEBRAICALLY. Let o be the original, s the scale, and let the network
//   return exactly what it was given, so the two surfaces InProxy and InNeural hold IDENTICAL BIT
//   PATTERNS at this texel. Then:
//
//   THAT PREMISE HAS A HARDWARE PRECONDITION, AND IT IS THE CALLER'S JOB. "Identical bit
//   patterns" is only reachable if the two surfaces are the SAME FORMAT. InProxy is always
//   r16g16b16a16_float; InNeural is the add-on's DLSSNR.Output, whose format would otherwise be
//   the game's TAA output format - possibly r11g11b10_float. Against an FP16 proxy that leaves an
//   identity network yielding a (neural - proxy) of up to ~2^-7 relative in R/G and ~2^-6 in B,
//   which the decode divides by s and adds to every pixel: a channel-asymmetric colour cast and a
//   per-pixel noise floor, not an identity. stray_dlssnr.cpp's nr_ensure_output therefore forces
//   the neural target to r16g16b16a16_float whenever the codec is on (the format coupling to the
//   TAA output only ever existed for the copy-back, and with the codec on the copy source is
//   result_tex instead), which is what Remix does for both surfaces -
//   rtx_neural_rendering.cpp:108 and :115, VK_FORMAT_R16G16B16A16_SFLOAT. If that is ever not
//   true the add-on says so at startup and the word "exact" below does not apply.
//
//   Given the premise:
//
//       p = SrgbDecode(x)                        for that bit pattern x
//       n = SrgbDecode(x)                        the SAME function of the SAME bits
//       n - p                = +0.0              exactly, for every finite x, IEEE-754
//       delta = (n - p) / s  = +0.0              s is clamped to [1e-6, 1e6], so no 0/0
//       transferred = max(o + 0.0, 0)      = o   for o >= 0, which max(source.rgb, 0) guarantees
//       Lt = dot(transferred, w) = dot(o, w) = Lo   same bits in, same bits out
//       luminanceRatio = Lt / Lo = 1.0          exactly (or 1.0 by the Lo > 0 guard)
//       luminanceOnly  = lerp(o, o * 1.0, cw) = o     lerp(a, a, t) = a + t*(a - a) = a
//       graded         = lerp(o, o, colorStrength)    = o
//       result         = lerp(o, o, transferStrength) = o
//       clamp(o, 0, 65504) = o                  o came out of an FP16 / R11G11B10 surface
//
//   so the pixel is returned BIT FOR BIT, for every value of s, every colorStrength and every
//   transferStrength. This is why the transfer is written as an additive residual and never as a
//   ratio or a reconstruction: an unchanged pixel must be unchanged, not nearly unchanged.
//
//   The same algebra makes transfer_strength = 0 an EXACT BYPASS: result = lerp(o, graded, 0) = o.
//
//   BE PRECISE ABOUT WHAT IT BYPASSES. It bypasses THE DENOISE, not the codec. With
//   transfer_strength=0 the decode still runs, still reads the proxy and the network's answer,
//   and still writes result_tex - but result_tex ends up holding the untouched pre-denoise TAA
//   output, and that is what the copy-back puts back over the frame. So:
//
//       transfer_strength=0   ==  pixel-identical to the game with the add-on UNLOADED
//                             ==  pixel-identical to copy_back=0
//       transfer_strength=0   !=  hdr_codec=0
//
//   hdr_codec=0 is a DIFFERENT image: it binds the raw linear TAA output as DLSSNR.Color and
//   copies the network's raw display-referred answer straight back, which is the darkened frame
//   this codec exists to fix. Comparing against it would compare "no denoise at all" with
//   "maximally out-of-distribution denoise" and could only ever differ.
//
//   transfer_strength=0 vs copy_back=0 is still the cheapest on-hardware check of the whole
//   path, and it is a real one: it exercises the encode, the evaluate, the decode, the state
//   save/restore and the copy-back end to end, and any plumbing error in any of them shows up as
//   a frame that is NOT identical.
//
// WHY THE DECODE RE-READS THE PROXY FROM THE TEXTURE INSTEAD OF RECOMPUTING IT
//   The identity above depends only on InProxy and InNeural holding the same BITS - not on
//   SrgbDecode being the exact inverse of SrgbEncode. If the decode recomputed p analytically from
//   the original, FP16 quantisation of the stored proxy would break the identity and every
//   unchanged pixel would pick up a small error. So the proxy is written to a texture, handed to
//   the network, and read BACK OUT of that same texture by the decode.
//
// WHAT WAS DELIBERATELY DROPPED FROM THE REMIX ORIGINAL
//   * RWTexture1D<float> exposureTexture / enableAutoExposure. Remix folds its own auto-exposure
//     into s. STRAY exposes no equivalent to us, so s is a plain ini constant. neuralRenderingProxyScale
//     collapses to the clamp, which is kept - the decode divides by this value and derives its
//     chroma floor from it.
//   * calcUserEVBias / userBrightness. No equivalent.
//   Nothing else. Every constant, every threshold, every guard is the Remix value.
//
// SHIPPING THE BYTECODE
//   The source is a string literal here and is compiled at runtime with D3DCompile to cs_5_0
//   DXBC. That is a deliberate choice, not a shortcut: this add-on cross-builds from macOS with
//   mingw-w64, where there is no fxc and no dxc, so a precompiled byte array could only ever be
//   produced on a different machine and nobody on this toolchain could rebuild or verify the
//   shader they were shipping. DXBC rather than DXIL because DXIL additionally needs the
//   Windows-only dxil.dll signer, and because DXBC is demonstrably accepted in this exact
//   Proton/vkd3d stack - the add-on identifies STRAY's own D3D12 compute shaders by parsing them
//   as DXBC (dxbc_tokens.hpp).
//
//   d3dcompiler_47.dll is loaded with LoadLibraryW, NEVER linked. A load-time import would make
//   the whole .addon64 fail to load when the DLL is absent, taking the working NGX path down with
//   it. If it cannot be loaded, or cannot compile, the codec latches OFF and the add-on runs
//   exactly as it does today.

#pragma once

#include "reshade_compat.hpp"

#include <d3dcompiler.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace hdr_codec {

using namespace reshade::api;

// Log levels handed to the caller's log functor: 0 = info, 1 = warning, 2 = error.
enum { log_info = 0, log_warn = 1, log_error = 2 };

// =============================================================================================
// The shader source. Slang -> HLSL is a no-op for this maths (float3, pow, exp, lerp, saturate
// and the per-channel ternaries all carry over unchanged), so these are the Remix bodies with the
// bindings rewritten and the exposure texture removed.
// =============================================================================================

// The codec itself, shared by both entry points.
//
// DO NOT substitute an approximate sRGB curve here. neural_rendering_codec.slangh:30-33 records
// why: colour.slangh's own linearToGamma/gammaToLinear approximate sRGB with x^2.2 and say so in
// their comment; the network was trained on true sRGB imagery and RenoDX - the only known working
// DLSS-NR deployment - feeds it the exact piecewise curve. Match that. Equally, do NOT reach for
// a DXGI_FORMAT_*_SRGB view: the proxy has to hold the encoded values as data.
//
// The per-channel scalar ternaries rather than a vector select are also from the original, so the
// source stays valid regardless of which vector-conditional rules the front end applies.
static const char *const kCodecPrelude = R"HLSL(
// ---- exact piecewise sRGB -------------------------------------------------------------------
float3 nrSrgbEncode(float3 color)
{
	color = saturate(color);

	const float3 toe = color * 12.92f;
	// Note: color is already saturated, so the max() only guards pow(0, 1/2.4) on hardware that
	// is unhappy with an exact zero base.
	const float3 shoulder = 1.055f * pow(max(color, 0.00000001f), float3(1.0f / 2.4f, 1.0f / 2.4f, 1.0f / 2.4f)) - 0.055f;

	return float3(
		color.r <= 0.0031308f ? toe.r : shoulder.r,
		color.g <= 0.0031308f ? toe.g : shoulder.g,
		color.b <= 0.0031308f ? toe.b : shoulder.b);
}

float3 nrSrgbDecode(float3 color)
{
	// This saturate is load-bearing: the network's output is not guaranteed to be in [0,1], and
	// clamping it here is what bounds the delta the decode adds.
	color = saturate(color);

	const float3 toe = color / 12.92f;
	const float3 shoulder = pow(max((color + 0.055f) / 1.055f, 0.0f), float3(2.4f, 2.4f, 2.4f));

	return float3(
		color.r <= 0.04045f ? toe.r : shoulder.r,
		color.g <= 0.04045f ? toe.g : shoulder.g,
		color.b <= 0.04045f ? toe.b : shoulder.b);
}

// ---- soft clip shoulder, verbatim from the RenoDX DLSS5 encoder ------------------------------
// Continuous in value at the knee, asymptotic to exactly 1.0, so no amount of input radiance can
// push the proxy out of the [0,1] domain sRGB encoding expects.
//
// Note: 0.25 * 5.770780 = 1.442695 = 1/ln(2), so the slope jumps from 1.0 to ~1.44 at the knee -
// this curve is C0 but not C1. That is what the known working deployment ships, so it is
// reproduced exactly rather than "corrected".
static const float kNrSoftClipKnee     = 0.75f;
static const float kNrSoftClipShoulder = 5.770780f;

float nrSoftClipChannel(float value)
{
	if (value <= kNrSoftClipKnee)
	{
		return value;
	}

	const float headroom = 1.0f - kNrSoftClipKnee;

	return kNrSoftClipKnee
	     + headroom * (1.0f - exp(-kNrSoftClipShoulder * (value - kNrSoftClipKnee)));
}

float3 nrSoftClip(float3 color)
{
	return float3(
		nrSoftClipChannel(color.r),
		nrSoftClipChannel(color.g),
		nrSoftClipChannel(color.b));
}

// ---- the scale -------------------------------------------------------------------------------
// Remix's neuralRenderingProxyScale with the auto-exposure texture removed. The clamp is KEPT:
// the decode's single division is by this value, and the decode's chroma floor derives from it.
// Both passes MUST compute this from the identical constant in the same frame; the host writes
// one CPU-side float into both root-constant blocks for exactly that reason.
float nrProxyScale(float staticScale)
{
	return clamp(staticScale, 0.000001f, 1000000.0f);
}

// ---- non-finite test -------------------------------------------------------------------------
// Written as a bit-pattern test rather than isnan()/isinf() so it cannot be folded away by any
// optimisation setting: exponent all-ones is inf when the mantissa is zero and NaN when it is
// not, and every guard in this codec wants "either of those".
bool nrAnyNotFinite(float3 v)
{
	const uint3 e = asuint(v) & 0x7F800000u;
	return any(e == 0x7F800000u);
}
)HLSL";

// ---- encode ---------------------------------------------------------------------------------
// neural_rendering_encode.comp.slang, bindings rewritten for D3D12.
//
// InColor is an SRV, not a UAV: Remix declares it RWTexture2D in Slang but RW_TEXTURE2D_READONLY
// on the C++ side (a DXVK-ism), and binding it as a real SRV here sidesteps
// TypedUAVLoadAdditionalFormats entirely. Only the proxy needs a UAV.
static const char *const kEncodeSource = R"HLSL(
Texture2D<float4>   InColor  : register(t0);
RWTexture2D<float4> OutProxy : register(u0);

cbuffer NrEncodeArgs : register(b0)
{
	uint2 g_imageSize;
	float g_proxyScale;
	float g_encodePad0;
};

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	const uint2 threadId = tid.xy;
	if (any(threadId >= g_imageSize))
	{
		return;
	}

	const float4 source = InColor.Load(int3(int2(threadId), 0));

	// Note: a broken pixel must not reach the network, and it must not reach the decode's
	// subtraction either, so it is flushed to black here. The decode leaves the corresponding
	// original pixel completely alone.
	const bool sourceIsFinite = !nrAnyNotFinite(source.rgb);
	const float3 sceneLinear = sourceIsFinite ? max(source.rgb, 0.0f) : float3(0.0f, 0.0f, 0.0f);

	const float scale = nrProxyScale(g_proxyScale);

	const float3 displayReferred = sceneLinear * scale;
	const float3 proxy = nrSrgbEncode(nrSoftClip(displayReferred));

	// Note: the proxy's own alpha is never read back - the decode takes the alpha it writes from
	// the untouched original - and DLSS-NR is an RGB network, so a constant is both sufficient and
	// safer here than forwarding the source alpha, which is unbounded and could carry a NaN into
	// the snippet's input texture.
	OutProxy[threadId] = float4(proxy, 1.0f);
}
)HLSL";

// ---- decode ---------------------------------------------------------------------------------
// neural_rendering_decode.comp.slang. ONE DELIBERATE DIFFERENCE from the Remix original, and it
// matters: Remix decodes IN PLACE (InOutColorBuffer is read at line 141 and written at line 211),
// because nothing downstream in Remix re-consumes that resource. Here the destination is a
// SEPARATE texture - the game's TAA output is simultaneously its own history in UE 4.27, so the
// original has to survive - and that makes the original's three early `return`s into bugs: they
// would leave the destination texel unwritten, i.e. holding an earlier frame's image.
//
// So every early-out except the genuine out-of-bounds guard COPIES THE SOURCE THROUGH and then
// returns. That is behaviourally identical to Remix's in-place "leave the pixel alone".
static const char *const kDecodeSource = R"HLSL(
Texture2D<float4>   InOriginal : register(t0);
Texture2D<float4>   InProxy    : register(t1);
Texture2D<float4>   InNeural   : register(t2);
RWTexture2D<float4> OutResult  : register(u0);

cbuffer NrDecodeArgs : register(b0)
{
	uint2 g_imageSize;
	float g_proxyScale;
	float g_transferStrength;
	float g_colorStrength;
	float g_decodePad0;
	float g_decodePad1;
	float g_decodePad2;
};

// Note: the destination is R16G16B16A16_FLOAT or R11G11B10_FLOAT. Writing a value the format
// cannot hold turns the pixel into an infinity, which then poisons the bloom downsample and the
// exposure histogram.
static const float kNrMaxHalf = 65504.0f;
static const float kNrMinChromaLuminance = 0.001f;

// color.slangh's calcBt709Luminance, inlined by hand.
float nrBt709Luminance(float3 linearColor)
{
	return dot(linearColor, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	const uint2 threadId = tid.xy;
	if (any(threadId >= g_imageSize))
	{
		// Genuinely out of bounds: there is no destination texel, so a bare return is correct.
		return;
	}

	const float4 source = InOriginal.Load(int3(int2(threadId), 0));

	// A pixel the game already broke is left exactly as it was - the encode fed the network black
	// for it, so its neural answer means nothing here. PASS THROUGH, do not just return.
	if (nrAnyNotFinite(source.rgb))
	{
		OutResult[threadId] = source;
		return;
	}

	// Note: the max() is what makes the identity argument hold only for a non-negative original.
	// The TAA output is a sum of radiances and is non-negative by construction, so in practice
	// this is the identity; a negative channel would be clamped to zero, which is the right thing
	// to do to it anyway.
	const float3 original = max(source.rgb, 0.0f);

	const float scale = nrProxyScale(g_proxyScale);

	// Both read back out of the FP16 surfaces, through the SAME decode. That is what makes the
	// identity bit-exact; see the header comment.
	const float3 proxy  = nrSrgbDecode(InProxy.Load(int3(int2(threadId), 0)).rgb);
	const float3 neural = nrSrgbDecode(InNeural.Load(int3(int2(threadId), 0)).rgb);

	// (*) the scene-linear change the network asked for. Exactly +0.0 when it asked for none.
	const float3 neuralDelta = (neural - proxy) / scale;

	// A non-finite delta can only come from a resource the snippet left in an unexpected state.
	// Never let one reach the frame: a NaN there survives every downstream pass and poisons the
	// exposure histogram and the bloom chain for the rest of the frame.
	if (nrAnyNotFinite(neuralDelta))
	{
		OutResult[threadId] = source;
		return;
	}

	const float3 transferred = max(original + neuralDelta, 0.0f);

	// Chroma safety valve. Both luminances are finite: the source came out of an FP16 or
	// R11G11B10 surface (<= 65504 per channel) and the delta is bounded by 1/scale <= 1e6, so
	// neither dot() can overflow to infinity.
	const float originalLuminance    = nrBt709Luminance(original);
	const float transferredLuminance = nrBt709Luminance(transferred);

	// Note: scale is clamped to [1e-6, 1e6] by nrProxyScale, so chromaFloor lands in [1e-9, 1e3]
	// and can never be zero.
	const float chromaFloor  = kNrMinChromaLuminance / scale;
	const float chromaWeight = saturate(originalLuminance / chromaFloor);
	// Note: the guard is still needed even though chromaWeight is zero when it trips - 0/0 is a
	// NaN and lerp() would propagate it through the zero weight.
	const float luminanceRatio = originalLuminance > 0.0f
		? transferredLuminance / originalLuminance
		: 1.0f;
	const float3 luminanceOnly = lerp(transferred, original * luminanceRatio, chromaWeight);

	const float3 graded = lerp(luminanceOnly, transferred, g_colorStrength);
	const float3 result = lerp(original, graded, g_transferStrength);

	OutResult[threadId] = float4(
		clamp(result, 0.0f, kNrMaxHalf),
		// Note: DLSS-NR is an RGB network and whatever it leaves in alpha is not meaningful. The
		// full RGBA copy this pass replaces overwrote Remix's alpha with it, which is what
		// destroyed portal particle transparency. Carry the ORIGINAL's alpha through instead.
		source.a);
}
)HLSL";

// =============================================================================================
// Root-constant blocks. Laid out so the HLSL cbuffer packing is a straight dword-for-dword copy;
// SetComputeRoot32BitConstants writes them linearly.
// =============================================================================================
struct encode_args
{
	uint32_t width = 0;
	uint32_t height = 0;
	float    proxy_scale = 1.0f;
	uint32_t pad0 = 0;
};
static_assert(sizeof(encode_args) == 16, "encode_args must be exactly 4 root constants");

struct decode_args
{
	uint32_t width = 0;
	uint32_t height = 0;
	float    proxy_scale = 1.0f;
	float    transfer_strength = 1.0f;
	float    color_strength = 1.0f;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
	uint32_t pad2 = 0;
};
static_assert(sizeof(decode_args) == 32, "decode_args must be exactly 8 root constants");

static constexpr uint32_t kEncodeConstantCount = 4;
static constexpr uint32_t kDecodeConstantCount = 8;

// Root parameter indices, shared by both layouts.
static constexpr uint32_t kParamSrvTable  = 0;
static constexpr uint32_t kParamUavTable  = 1;
static constexpr uint32_t kParamConstants = 2;

// [numthreads(16,16,1)], which is what both Remix passes use, dispatched with the plain ceil-div
// util::computeBlockCount does (rtx_neural_rendering.cpp:495 and :528). At 1920x1080 that is
// Dispatch(120, 68, 1).
static constexpr uint32_t kThreadGroupSize = 16;
inline uint32_t group_count(uint32_t extent) { return (extent + kThreadGroupSize - 1u) / kThreadGroupSize; }

// =============================================================================================
// Runtime compile
// =============================================================================================

// FNV-1a over the exact source text that is fed to the compiler. Names the on-disk cache so a
// stale blob from an older revision of this header can never be picked up silently.
inline uint64_t source_hash(const std::string &src)
{
	uint64_t h = 1469598103934665603ull;
	for (unsigned char c : src)
	{
		h ^= static_cast<uint64_t>(c);
		h *= 1099511628211ull;
	}
	return h;
}

inline std::string full_source(const char *entry_source)
{
	std::string s;
	s.reserve(std::strlen(kCodecPrelude) + std::strlen(entry_source) + 2);
	s += kCodecPrelude;
	s += "\n";
	s += entry_source;
	return s;
}

// DELIBERATELY NOT -ld3dcompiler_47: a load-time import would make the whole add-on fail to load
// when the DLL is absent, taking the working NGX path down with it. Under Proton this may be
// Wine's builtin (vkd3d-shader's HLSL compiler), whose SM5 compute coverage varies by version -
// which is exactly why a compile failure has to be survivable.
template <typename LogFn>
inline pD3DCompile resolve_compiler(LogFn log)
{
	static bool         s_tried = false;
	static pD3DCompile  s_fn = nullptr;
	if (s_tried)
		return s_fn;
	s_tried = true;

	HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
	if (m == nullptr)
		m = LoadLibraryW(L"d3dcompiler_43.dll");
	if (m != nullptr)
		s_fn = reinterpret_cast<pD3DCompile>(reinterpret_cast<void *>(GetProcAddress(m, "D3DCompile")));

	if (s_fn == nullptr)
	{
		char buf[512];
		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: d3dcompiler_47.dll / D3DCompile is not available (LastError=%lu). The HDR "
			"codec cannot be built here. Drop a precompiled stray_dlssnr_encode.dxbc and "
			"stray_dlssnr_decode.dxbc beside stray_dlssnr.ini to use it anyway, or leave "
			"hdr_codec off - the add-on runs exactly as it does without the codec.",
			(unsigned long)GetLastError());
		log(log_error, buf);
	}
	return s_fn;
}

inline bool read_file(const std::wstring &path, std::vector<uint8_t> &out)
{
	FILE *f = _wfopen(path.c_str(), L"rb");
	if (f == nullptr)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long n = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	bool ok = false;
	if (n > 0 && n < (16 * 1024 * 1024))
	{
		out.resize(static_cast<size_t>(n));
		ok = (std::fread(out.data(), 1, out.size(), f) == out.size());
	}
	std::fclose(f);
	if (!ok)
		out.clear();
	return ok;
}

inline bool write_file(const std::wstring &path, const void *data, size_t size)
{
	FILE *f = _wfopen(path.c_str(), L"wb");
	if (f == nullptr)
		return false;
	const bool ok = (std::fwrite(data, 1, size, f) == size);
	std::fclose(f);
	return ok;
}

// Built by hand rather than with swprintf: mingw's wide printf family is a minefield of
// __USE_MINGW_ANSI_STDIO variants, and this is sixteen nibbles.
inline std::wstring widen_hex(uint64_t v)
{
	static const wchar_t *digits = L"0123456789abcdef";
	std::wstring s(16, L'0');
	for (int i = 15; i >= 0; --i, v >>= 4)
		s[static_cast<size_t>(i)] = digits[v & 0xfull];
	return s;
}

// Produces the DXBC for one entry point. In order:
//   1. <dir><name>.dxbc                  - a user-supplied override, always preferred, logged loudly
//   2. <dir><name>.<source hash>.dxbc    - this build's own cache, produced by a previous launch
//   3. D3DCompile(cs_5_0)                - and the result is written to (2) on success
// Any failure returns false with a logged reason; the caller then leaves the codec off.
template <typename LogFn>
inline bool build_one(const std::wstring &dir, const wchar_t *wname, const char *name,
                      const char *entry_source, std::vector<uint8_t> &out, LogFn log)
{
	const std::string  src  = full_source(entry_source);
	const uint64_t     hash = source_hash(src);
	const std::wstring override_path = dir + wname + L".dxbc";
	const std::wstring cache_path    = dir + wname + L"." + widen_hex(hash) + L".dxbc";

	char buf[1024];

	if (read_file(override_path, out) && !out.empty())
	{
		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: HDR codec %s - using the USER-SUPPLIED bytecode %ls.dxbc (%zu bytes). This "
			"file OVERRIDES the shader source compiled into the add-on (source hash 0x%016llx); "
			"delete it to go back to the built-in shader.",
			name, wname, out.size(), (unsigned long long)hash);
		log(log_warn, buf);
		return true;
	}

	if (read_file(cache_path, out) && !out.empty())
	{
		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: HDR codec %s - reusing the cached bytecode %ls.%016llx.dxbc (%zu bytes), "
			"which was compiled from exactly this shader source.",
			name, wname, (unsigned long long)hash, out.size());
		log(log_info, buf);
		return true;
	}

	const pD3DCompile compile = resolve_compiler(log);
	if (compile == nullptr)
		return false;   // resolve_compiler already said why

	ID3DBlob *code = nullptr;
	ID3DBlob *errors = nullptr;
	// ENABLE_STRICTNESS rejects the legacy relaxations; IEEE_STRICTNESS keeps the compiler from
	// assuming operands are finite. The non-finite guards are additionally written as bit-pattern
	// tests in the source, so they survive even if a given d3dcompiler ignores the flag.
	const HRESULT hr = compile(src.data(), src.size(), name, nullptr, nullptr, "main", "cs_5_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS,
		0, &code, &errors);

	if (FAILED(hr) || code == nullptr)
	{
		// The error blob is logged VERBATIM. A silent skip is exactly what this codebase refuses
		// to ship, and an HRESULT alone does not say which line failed.
		const char *msg = (errors != nullptr) ? static_cast<const char *>(errors->GetBufferPointer())
		                                      : "(no error blob)";
		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: D3DCompile(cs_5_0) FAILED for the HDR codec %s shader, hr=0x%08lx: %s",
			name, (unsigned long)hr, msg);
		log(log_error, buf);
		if (errors != nullptr) errors->Release();
		if (code != nullptr)   code->Release();
		return false;
	}
	if (errors != nullptr)
	{
		// Warnings. Compiled anyway, but say so.
		std::snprintf(buf, sizeof(buf), "DLSS-NR: D3DCompile warnings for the HDR codec %s shader: %s",
			name, static_cast<const char *>(errors->GetBufferPointer()));
		log(log_warn, buf);
		errors->Release();
	}

	out.assign(static_cast<const uint8_t *>(code->GetBufferPointer()),
	           static_cast<const uint8_t *>(code->GetBufferPointer()) + code->GetBufferSize());
	code->Release();

	if (write_file(cache_path, out.data(), out.size()))
	{
		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: HDR codec %s compiled to %zu bytes of cs_5_0 DXBC and cached as "
			"%ls.%016llx.dxbc. Copy that file to a machine whose d3dcompiler cannot compile it "
			"and rename it to %ls.dxbc to use it there.",
			name, out.size(), wname, (unsigned long long)hash, wname);
		log(log_info, buf);
	}
	else
	{
		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: HDR codec %s compiled to %zu bytes of cs_5_0 DXBC. The blob could NOT be "
			"cached to disk (the add-on's directory is not writable); it will be recompiled on "
			"every launch, which is harmless.", name, out.size());
		log(log_info, buf);
	}
	return true;
}

struct blobs
{
	std::vector<uint8_t> encode;
	std::vector<uint8_t> decode;
	bool ok = false;
};

template <typename LogFn>
inline bool build(const std::wstring &dir, blobs &out, LogFn log)
{
	out.ok = build_one(dir, L"stray_dlssnr_encode", "encode", kEncodeSource, out.encode, log)
	      && build_one(dir, L"stray_dlssnr_decode", "decode", kDecodeSource, out.decode, log);
	return out.ok;
}

// =============================================================================================
// Pipeline layout and PSO
//
// SRVs and UAVs MUST be separate layout parameters. One push_descriptors call fills exactly ONE
// root parameter and carries a single descriptor_type; two ranges in one param would be clobbered
// by the second call into it.
//
// The single-descriptor_range constructor (push_descriptors) is used rather than descriptor_table
// because ReShade's D3D12 backend skips its root-descriptor fast path for
// texture_shader_resource_view / texture_unordered_access_view, so both become real descriptor
// tables with DESCRIPTORS_VOLATILE | DATA_VOLATILE - which is exactly what the transient-heap path
// in command_list_impl::push_descriptors requires.
//
// constant_range::binding MUST stay 0: create_pipeline_layout returns false outright otherwise.
// =============================================================================================
inline bool make_layout(device *dev, uint32_t srv_count, uint32_t constant_count, pipeline_layout &out)
{
	descriptor_range srvs = {};
	srvs.binding           = 0;
	srvs.dx_register_index = 0;      // register(t0)
	srvs.dx_register_space = 0;
	srvs.count             = srv_count;
	srvs.array_size        = 1;
	srvs.visibility        = shader_stage::compute;
	srvs.type              = descriptor_type::shader_resource_view;

	descriptor_range uavs = {};
	uavs.binding           = 0;
	uavs.dx_register_index = 0;      // register(u0)
	uavs.dx_register_space = 0;
	uavs.count             = 1;
	uavs.array_size        = 1;
	uavs.visibility        = shader_stage::compute;
	uavs.type              = descriptor_type::unordered_access_view;

	constant_range consts = {};
	consts.binding           = 0;    // MUST be 0 on D3D12
	consts.dx_register_index = 0;    // register(b0)
	consts.dx_register_space = 0;
	consts.count             = constant_count;   // in 32-BIT VALUES, not bytes
	consts.visibility        = shader_stage::compute;

	const pipeline_layout_param params[3] = {
		pipeline_layout_param(srvs),
		pipeline_layout_param(uavs),
		pipeline_layout_param(consts),
	};

	out = { 0 };
	return dev->create_pipeline_layout(3, params, &out) && out.handle != 0;
}

inline bool make_pipeline(device *dev, pipeline_layout layout, const std::vector<uint8_t> &dxbc, pipeline &out)
{
	shader_desc cs = {};
	cs.code        = dxbc.data();
	cs.code_size   = dxbc.size();
	// MUST be nullptr on D3D12: convert_shader_desc asserts it, and the D3D12 backend has no
	// compiler to resolve a name against. The entry point is baked in at compile time.
	cs.entry_point = nullptr;

	pipeline_subobject sub[1] = {
		{ pipeline_subobject_type::compute_shader, 1, &cs }
	};

	out = { 0 };
	return dev->create_pipeline(layout, 1, sub, &out) && out.handle != 0;
}

struct pipelines
{
	pipeline_layout encode_layout = { 0 };
	pipeline_layout decode_layout = { 0 };
	pipeline        encode_pso    = { 0 };
	pipeline        decode_pso    = { 0 };
	bool            ok            = false;
};

inline void destroy(device *dev, pipelines &p)
{
	if (dev == nullptr)
	{
		p = pipelines();
		return;
	}
	if (p.encode_pso.handle    != 0) dev->destroy_pipeline(p.encode_pso);
	if (p.decode_pso.handle    != 0) dev->destroy_pipeline(p.decode_pso);
	if (p.encode_layout.handle != 0) dev->destroy_pipeline_layout(p.encode_layout);
	if (p.decode_layout.handle != 0) dev->destroy_pipeline_layout(p.decode_layout);
	p = pipelines();
}

template <typename LogFn>
inline bool create(device *dev, const blobs &b, pipelines &p, LogFn log)
{
	p = pipelines();
	if (dev == nullptr || !b.ok)
		return false;

	const char *stage = nullptr;
	if      (!make_layout(dev, 1, kEncodeConstantCount, p.encode_layout))       stage = "create_pipeline_layout(encode)";
	else if (!make_layout(dev, 3, kDecodeConstantCount, p.decode_layout))       stage = "create_pipeline_layout(decode)";
	else if (!make_pipeline(dev, p.encode_layout, b.encode, p.encode_pso))      stage = "create_pipeline(encode)";
	else if (!make_pipeline(dev, p.decode_layout, b.decode, p.decode_pso))      stage = "create_pipeline(decode)";

	if (stage != nullptr)
	{
		char buf[512];
		std::snprintf(buf, sizeof(buf),
			"DLSS-NR: the HDR codec could not be built - %s failed. The codec stays OFF and the "
			"add-on runs exactly as it does without it (the network is fed the raw linear TAA "
			"output, which is the darkening described in README gap 1).", stage);
		log(log_error, buf);
		destroy(dev, p);
		return false;
	}

	p.ok = true;
	return true;
}

} // namespace hdr_codec
