// hdr_codec_selftest.cpp - native replay of the HDR codec's decode, BOTH graft modes.
//
// WHAT THIS IS FOR, IN ONE LINE: hdr_graft added a branch to a shader that ships and that the
// user plays on. This proves mode 0 came through it unchanged, and it measures what mode 1
// actually does instead of describing it.
//
// It is deliberately NOT in src/: build.sh compiles src/*.cpp into the add-on, and this is a
// host-side test with a main(). It builds and runs natively on the build machine:
//
//     c++ -std=c++17 -O2 -Wall -o /tmp/hdr_codec_selftest tools/hdr_codec_selftest.cpp
//     /tmp/hdr_codec_selftest
//
// It shares no code with the shader - it RESTATES the shader's arithmetic, so a disagreement is a
// real disagreement. Crucially it carries TWO restatements of mode 0:
//
//   decode_shipping()  transcribed from the kDecodeSource that SHIPS today (git HEAD before the
//                      hdr_graft commit), with no branch and no graft mode in it at all.
//   decode_current()   transcribed from the kDecodeSource in src/hdr_codec.hpp AS IT IS NOW,
//                      branch included.
//
// and compares them bit for bit. That is the regression test: "mode 0 is unchanged" is not an
// argument about where a brace went, it is 1,080,000 comparisons of IEEE-754 bit patterns.
//
// WHAT IT CANNOT SETTLE. It replays FP32 arithmetic on the host; the shader runs on the GPU with
// fxc-generated DXBC. Both are IEEE-754 single precision and the codec is compiled with
// D3DCOMPILE_IEEE_STRICTNESS, which forbids the reassociation that could make them differ, but a
// driver that contracts a multiply-add would still be outside what this measures. What IS settled
// here is that the SOURCE says the same thing it said before, which is the thing a human edit can
// break and the thing this commit could plausibly have broken.
//
// Expected: 0 failed.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

static int g_fail = 0, g_pass = 0;
static void ck(bool c, const char *what, const char *detail = "")
{
    if (c) { ++g_pass; std::printf("  ok   %s %s\n", what, detail); }
    else   { ++g_fail; std::printf("  FAIL %s %s\n", what, detail); }
}
static uint32_t bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

// =============================================================================================
// float3, written out rather than pulled from a library so every operation below is visible.
// =============================================================================================
struct F3 { float x, y, z; };
static F3 mk(float a, float b, float c) { return F3{a, b, c}; }
static F3 operator+(F3 a, F3 b) { return mk(a.x+b.x, a.y+b.y, a.z+b.z); }
static F3 operator-(F3 a, F3 b) { return mk(a.x-b.x, a.y-b.y, a.z-b.z); }
static F3 operator*(F3 a, F3 b) { return mk(a.x*b.x, a.y*b.y, a.z*b.z); }
static F3 operator*(F3 a, float s) { return mk(a.x*s, a.y*s, a.z*s); }
static F3 operator/(F3 a, float s) { return mk(a.x/s, a.y/s, a.z/s); }
// HLSL lerp(a, b, t) is a + t*(b - a): the form that makes lerp(a, a, t) == a exactly.
static float  lerpf(float a, float b, float t) { return a + t * (b - a); }
static F3     lerp3(F3 a, F3 b, float t) { return mk(lerpf(a.x,b.x,t), lerpf(a.y,b.y,t), lerpf(a.z,b.z,t)); }
static float  satf(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static F3     max3(F3 a, float s) { return mk(std::fmax(a.x,s), std::fmax(a.y,s), std::fmax(a.z,s)); }

// =============================================================================================
// The shared prelude, restated. src/hdr_codec.hpp kCodecPrelude.
// =============================================================================================
static float srgb_encode_ch(float c)
{
    c = satf(c);
    const float toe = c * 12.92f;
    const float shoulder = 1.055f * std::pow(std::fmax(c, 0.00000001f), 1.0f / 2.4f) - 0.055f;
    return c <= 0.0031308f ? toe : shoulder;
}
static F3 srgb_encode(F3 c) { return mk(srgb_encode_ch(c.x), srgb_encode_ch(c.y), srgb_encode_ch(c.z)); }

static float srgb_decode_ch(float c)
{
    c = satf(c);
    const float toe = c / 12.92f;
    const float shoulder = std::pow(std::fmax((c + 0.055f) / 1.055f, 0.0f), 2.4f);
    return c <= 0.04045f ? toe : shoulder;
}
static F3 srgb_decode(F3 c) { return mk(srgb_decode_ch(c.x), srgb_decode_ch(c.y), srgb_decode_ch(c.z)); }

static const float kKnee = 0.75f, kShoulder = 5.770780f;
static float soft_clip_ch(float v)
{
    if (v <= kKnee) return v;
    const float headroom = 1.0f - kKnee;
    return kKnee + headroom * (1.0f - std::exp(-kShoulder * (v - kKnee)));
}
static F3 soft_clip(F3 c) { return mk(soft_clip_ch(c.x), soft_clip_ch(c.y), soft_clip_ch(c.z)); }

static float proxy_scale_clamp(float s) { return std::fmin(std::fmax(s, 0.000001f), 1000000.0f); }

static bool any_not_finite(F3 v)
{
    return ((bits(v.x) & 0x7F800000u) == 0x7F800000u)
        || ((bits(v.y) & 0x7F800000u) == 0x7F800000u)
        || ((bits(v.z) & 0x7F800000u) == 0x7F800000u);
}

static const float kMaxHalf = 65504.0f;
static const float kMinChromaLuminance = 0.001f;
static float bt709_lum(F3 c) { return c.x*0.2126f + c.y*0.7152f + c.z*0.0722f; }

// =============================================================================================
// FP16 storage round trip. The proxy and the network's target are BOTH r16g16b16a16_float, so
// every value the decode reads has been through this. It is what makes the identity premise
// ("identical bit patterns") reachable rather than theoretical, and it is why proxy_y can come out
// larger than original_y and light up renodx's asymmetric branch.
// =============================================================================================
static uint16_t f32_to_f16(float f)
{
    const uint32_t x = bits(f);
    const uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t mant = x & 0x007FFFFFu;
    const int32_t exp = int32_t((x >> 23) & 0xFFu);

    if (exp == 255)   // inf / nan, preserved as a quiet nan so the firewall still sees one
        return uint16_t(sign | 0x7C00u | (mant ? (0x0200u | (mant >> 13)) : 0u));

    int32_t e = exp - 127 + 15;
    if (e >= 31) return uint16_t(sign | 0x7C00u);       // overflows the half range
    if (e <= 0)
    {
        if (e < -10) return uint16_t(sign);             // underflows to signed zero
        mant |= 0x00800000u;                            // restore the implicit bit
        const uint32_t shift = uint32_t(14 - e);        // 14..24
        uint32_t m = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u);
        const uint32_t halfbit = 1u << (shift - 1);
        if (rem > halfbit || (rem == halfbit && (m & 1u))) ++m;   // round to nearest even
        return uint16_t(sign | m);                      // a carry into 0x400 IS the smallest normal
    }
    uint32_t m = mant >> 13;
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (m & 1u)))
    {
        ++m;
        if (m == 0x400u) { m = 0; ++e; if (e >= 31) return uint16_t(sign | 0x7C00u); }
    }
    return uint16_t(sign | (uint32_t(e) << 10) | m);
}

static float f16_to_f32(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const uint32_t e = uint32_t(h >> 10) & 0x1Fu;
    uint32_t m = uint32_t(h) & 0x3FFu;
    uint32_t o;
    if (e == 0)
    {
        if (m == 0) o = sign;
        else
        {
            int sh = 0;
            while ((m & 0x400u) == 0u) { m <<= 1; ++sh; }
            m &= 0x3FFu;
            o = sign | (uint32_t(113 - sh) << 23) | (m << 13);
        }
    }
    else if (e == 31) o = sign | 0x7F800000u | (m << 13);
    else              o = sign | ((e + 112u) << 23) | (m << 13);
    float f; std::memcpy(&f, &o, 4); return f;
}

static float fp16_rt(float f) { return f16_to_f32(f32_to_f16(f)); }

// =============================================================================================
// THE ENCODE. src/hdr_codec.hpp kEncodeSource, unchanged by this commit.
// =============================================================================================
static F3 encode(F3 source_rgb, float proxy_scale_arg)
{
    const bool finite = !any_not_finite(source_rgb);
    const F3 scene = finite ? max3(source_rgb, 0.0f) : mk(0.0f, 0.0f, 0.0f);
    const float scale = proxy_scale_clamp(proxy_scale_arg);
    return srgb_encode(soft_clip(scene * scale));
}

// =============================================================================================
// MODE 0, AS IT SHIPS TODAY. Transcribed from kDecodeSource at the commit BEFORE hdr_graft.
// Do not "tidy" this: its entire job is to be the old thing.
// =============================================================================================
struct Out { F3 rgb; float a; bool wrote_source; };

static Out decode_shipping(F3 src_rgb, float src_a, F3 proxy_raw, F3 neural_raw,
                           float proxy_scale_arg, float transfer_strength, float color_strength)
{
    if (any_not_finite(src_rgb)) return Out{src_rgb, src_a, true};

    const F3 original = max3(src_rgb, 0.0f);
    const float scale = proxy_scale_clamp(proxy_scale_arg);

    const F3 proxy  = srgb_decode(proxy_raw);
    const F3 neural = srgb_decode(neural_raw);

    const F3 neuralDelta = (neural - proxy) / scale;
    if (any_not_finite(neuralDelta)) return Out{src_rgb, src_a, true};

    const F3 transferred = max3(original + neuralDelta, 0.0f);

    const float originalLuminance    = bt709_lum(original);
    const float transferredLuminance = bt709_lum(transferred);

    const float chromaFloor  = kMinChromaLuminance / scale;
    const float chromaWeight = satf(originalLuminance / chromaFloor);
    const float luminanceRatio = originalLuminance > 0.0f
        ? transferredLuminance / originalLuminance : 1.0f;
    const F3 luminanceOnly = lerp3(transferred, original * luminanceRatio, chromaWeight);

    const F3 graded = lerp3(luminanceOnly, transferred, color_strength);
    const F3 result = lerp3(original, graded, transfer_strength);

    return Out{ mk(std::fmin(std::fmax(result.x,0.0f),kMaxHalf),
                   std::fmin(std::fmax(result.y,0.0f),kMaxHalf),
                   std::fmin(std::fmax(result.z,0.0f),kMaxHalf)), src_a, false };
}

// =============================================================================================
// renodx's maths, transcribed from the plaintext HLSL in renodx-reference.addon64
// (.rdata RVA 0x42f90..0x440bd) and mirrored by src/hdr_codec.hpp's nrRdx* functions.
// =============================================================================================
static float rdx_lum(F3 c) { return c.x*0.212639f + c.y*0.715169f + c.z*0.072192f; }
static float sgn(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }
static F3 rdx_cbrt_signed(F3 v)
{
    return mk(sgn(v.x) * std::pow(std::fabs(v.x), 1.0f/3.0f),
              sgn(v.y) * std::pow(std::fabs(v.y), 1.0f/3.0f),
              sgn(v.z) * std::pow(std::fabs(v.z), 1.0f/3.0f));
}
// mul(M, v) with M given row by row, exactly as HLSL reads a float3x3 initialiser list.
static F3 m3(const float m[9], F3 v)
{
    return mk(m[0]*v.x + m[1]*v.y + m[2]*v.z,
              m[3]*v.x + m[4]*v.y + m[5]*v.z,
              m[6]*v.x + m[7]*v.y + m[8]*v.z);
}
static F3 rdx_to_oklab(F3 c)
{
    static const float rgb_to_lms[9] = {
        0.4122214708f, 0.5363325363f, 0.0514459929f,
        0.2119034982f, 0.6806995451f, 0.1073969566f,
        0.0883024619f, 0.2817188376f, 0.6299787005f };
    static const float lms_to_lab[9] = {
        0.2104542553f, 0.7936177850f, -0.0040720468f,
        1.9779984951f, -2.4285922050f, 0.4505937099f,
        0.0259040371f, 0.7827717662f, -0.8086757660f };
    return m3(lms_to_lab, rdx_cbrt_signed(m3(rgb_to_lms, c)));
}
static F3 rdx_from_oklab(F3 lab)
{
    static const float lab_to_lms[9] = {
        1.0f, 0.3963377774f, 0.2158037573f,
        1.0f, -0.1055613458f, -0.0638541728f,
        1.0f, -0.0894841775f, -1.2914855480f };
    static const float lms_to_rgb[9] = {
        4.0767416621f, -3.3077115913f, 0.2309699292f,
        -1.2684380046f, 2.6097574011f, -0.3413193965f,
        -0.0041960863f, -0.7034186147f, 1.7076147010f };
    const F3 lms = m3(lab_to_lms, lab);
    return m3(lms_to_rgb, lms * lms * lms);
}
static F3 rdx_clamp_ap1(F3 c)
{
    static const float bt709_to_ap1[9] = {
        0.613097f, 0.339523f, 0.047379f,
        0.070194f, 0.916354f, 0.013452f,
        0.020616f, 0.109570f, 0.869815f };
    static const float ap1_to_bt709[9] = {
        1.705051f, -0.621792f, -0.083259f,
        -0.130256f, 1.140805f, -0.010548f,
        -0.024003f, -0.128969f, 1.152972f };
    return m3(ap1_to_bt709, max3(m3(bt709_to_ap1, c), 0.0f));
}
static F3 rdx_hue_oklab(F3 incorrect, F3 correct)
{
    F3 il = rdx_to_oklab(incorrect);
    const F3 cl = rdx_to_oklab(correct);
    const float ic = std::sqrt(il.y*il.y + il.z*il.z);
    const float cc = std::sqrt(cl.y*cl.y + cl.z*cl.z);
    const float k = (cc == 0.0f) ? 1.0f : ic / cc;
    il.y = cl.y * k;
    il.z = cl.z * k;
    return rdx_clamp_ap1(rdx_from_oklab(il));
}
// Exposed so the luminance-equivalence test can call it directly.
static float rdx_new_y(float original_y, float proxy_y, float neural_y, bool &took_lt_branch)
{
    if (original_y < proxy_y) { took_lt_branch = true; return original_y; }   // caller divides
    took_lt_branch = false;
    return neural_y + std::fmax(0.0f, original_y - proxy_y);
}
static F3 rdx_upgrade(F3 original, F3 proxy, F3 neural, float transfer_strength)
{
    const float oy = rdx_lum(original), py = rdx_lum(proxy), ny = rdx_lum(neural);
    float ratio;
    if (oy < py) ratio = oy / py;
    else {
        const float new_y = ny + std::fmax(0.0f, oy - py);
        ratio = ny > 0.0f ? new_y / ny : 0.0f;
    }
    const F3 scaled = rdx_hue_oklab(neural * ratio, neural);
    return lerp3(original, scaled, transfer_strength);
}

// =============================================================================================
// THE DECODE AS IT IS NOW. Transcribed from src/hdr_codec.hpp kDecodeSource with hdr_graft in it.
// =============================================================================================
static Out decode_current(F3 src_rgb, float src_a, F3 proxy_raw, F3 neural_raw,
                          float proxy_scale_arg, float transfer_strength, float color_strength,
                          uint32_t graft)
{
    if (any_not_finite(src_rgb)) return Out{src_rgb, src_a, true};

    const F3 original = max3(src_rgb, 0.0f);
    const float scale = proxy_scale_clamp(proxy_scale_arg);

    const F3 proxy  = srgb_decode(proxy_raw);
    const F3 neural = srgb_decode(neural_raw);

    const F3 neuralDelta = (neural - proxy) / scale;
    if (any_not_finite(neuralDelta)) return Out{src_rgb, src_a, true};

    const F3 transferred = max3(original + neuralDelta, 0.0f);

    F3 result;
    if (graft == 0u)
    {
        const float originalLuminance    = bt709_lum(original);
        const float transferredLuminance = bt709_lum(transferred);

        const float chromaFloor  = kMinChromaLuminance / scale;
        const float chromaWeight = satf(originalLuminance / chromaFloor);
        const float luminanceRatio = originalLuminance > 0.0f
            ? transferredLuminance / originalLuminance : 1.0f;
        const F3 luminanceOnly = lerp3(transferred, original * luminanceRatio, chromaWeight);

        const F3 graded = lerp3(luminanceOnly, transferred, color_strength);
        result = lerp3(original, graded, transfer_strength);
    }
    else
    {
        const F3 originalDisplay = original * scale;
        const F3 upgraded = rdx_upgrade(originalDisplay, proxy, neural, transfer_strength);

        const float originalY = rdx_lum(originalDisplay);
        const float upgradedY = rdx_lum(upgraded);
        const float ratioY = originalY == 0.0f ? 1.0f : upgradedY / originalY;
        const F3 luminanceOnlyRdx = originalDisplay * ratioY;

        const F3 gradedDisplay = lerp3(luminanceOnlyRdx, upgraded, color_strength);
        const F3 sceneLinear = gradedDisplay / scale;

        if (any_not_finite(sceneLinear)) return Out{src_rgb, src_a, true};
        result = sceneLinear;
    }

    return Out{ mk(std::fmin(std::fmax(result.x,0.0f),kMaxHalf),
                   std::fmin(std::fmax(result.y,0.0f),kMaxHalf),
                   std::fmin(std::fmax(result.z,0.0f),kMaxHalf)), src_a, false };
}

// =============================================================================================
// A deterministic PRNG. No <random>: its distributions are not specified to produce the same
// sequence across implementations, and this test's numbers must be reproducible on any host.
// =============================================================================================
struct Rng { uint64_t s; uint32_t u32() { s ^= s<<13; s ^= s>>7; s ^= s<<17; return uint32_t(s>>32); }
             float unit() { return float(u32() >> 8) * (1.0f / 16777216.0f); } };

// A plausible STRAY pixel: mostly dim, a long tail into neon-sign territory.
static F3 random_pixel(Rng &r)
{
    const float mag = std::pow(r.unit(), 3.0f) * 12.0f;
    return mk(mag * (0.05f + 0.95f*r.unit()), mag * (0.05f + 0.95f*r.unit()), mag * (0.05f + 0.95f*r.unit()));
}

// A plausible network answer: the proxy, nudged in display-referred space, then re-encoded and
// stored through FP16 exactly as the real neural target is.
static F3 network_answer(F3 proxy_enc, Rng &r, float gain)
{
    const F3 lin = srgb_decode(proxy_enc);
    const F3 nudged = mk(satf(lin.x * gain * (0.97f + 0.06f*r.unit())),
                         satf(lin.y * gain * (0.97f + 0.06f*r.unit())),
                         satf(lin.z * gain * (0.97f + 0.06f*r.unit())));
    const F3 enc = srgb_encode(nudged);
    return mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
}

// A DARK, STRONGLY CHROMATIC pixel: a shadow with a colour in it. random_pixel above draws all
// three channels as mag*(0.05 + 0.95*u), which bounds chromaticity at about 20:1 and never gets
// near black - so it cannot reach the region where mode 0's chroma valve engages, which is
// precisely the region the two grafts disagree in. Section 5 measures both.
static F3 shadow_pixel(Rng &r)
{
    const float mag = std::pow(r.unit(), 2.0f) * 0.01f;   // scene-linear, at or below 0.01
    const int dominant = int(r.u32() % 3u);
    float ch[3] = { 0.02f*r.unit(), 0.02f*r.unit(), 0.02f*r.unit() };
    ch[dominant] = 1.0f;
    return mk(mag*ch[0], mag*ch[1], mag*ch[2]);
}

// A DENOISER's answer rather than a nudge: gain 0.3x..6x and a 0.6 pull toward the pixel's own
// mean, i.e. chroma averaged away with the noise. network_answer's 0.6x..1.8x with a +-3% per
// channel wobble is a fine model of a network that agrees with the proxy; it is not a model of
// what a denoiser does to a dark, noisy, coloured pixel, which is the whole reason it exists.
static F3 denoised_answer(F3 proxy_enc, Rng &r)
{
    const F3 lin = srgb_decode(proxy_enc);
    const float mean = (lin.x + lin.y + lin.z) / 3.0f;
    const float gain = 0.3f + 5.7f * r.unit();
    const F3 pulled = mk(lerpf(lin.x, mean, 0.6f), lerpf(lin.y, mean, 0.6f), lerpf(lin.z, mean, 0.6f));
    const F3 nudged = mk(satf(pulled.x*gain), satf(pulled.y*gain), satf(pulled.z*gain));
    const F3 enc = srgb_encode(nudged);
    return mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
}

// MODE 0 WITH THE CHROMA VALVE FORCED OPEN (chromaWeight == 1). Not a shader path - a DIAGNOSTIC,
// and the only way to say WHICH term the shadow divergence in section 5 comes from. Everything
// else is decode_current's mode 0, character for character.
static Out decode_mode0_no_valve(F3 src_rgb, F3 proxy_raw, F3 neural_raw,
                                 float proxy_scale_arg, float transfer_strength, float color_strength)
{
    if (any_not_finite(src_rgb)) return Out{src_rgb, 1.0f, true};
    const F3 original = max3(src_rgb, 0.0f);
    const float scale = proxy_scale_clamp(proxy_scale_arg);
    const F3 proxy = srgb_decode(proxy_raw), neural = srgb_decode(neural_raw);
    const F3 neuralDelta = (neural - proxy) / scale;
    if (any_not_finite(neuralDelta)) return Out{src_rgb, 1.0f, true};
    const F3 transferred = max3(original + neuralDelta, 0.0f);
    const float oy = bt709_lum(original), ty = bt709_lum(transferred);
    const float luminanceRatio = oy > 0.0f ? ty / oy : 1.0f;
    const F3 luminanceOnly = original * luminanceRatio;          // chromaWeight pinned to 1
    const F3 graded = lerp3(luminanceOnly, transferred, color_strength);
    const F3 result = lerp3(original, graded, transfer_strength);
    return Out{ mk(std::fmin(std::fmax(result.x,0.0f),kMaxHalf),
                   std::fmin(std::fmax(result.y,0.0f),kMaxHalf),
                   std::fmin(std::fmax(result.z,0.0f),kMaxHalf)), 1.0f, false };
}

// The honest unit for "can you see it": an 8-bit sRGB display code value at paper white. A
// relative-to-pixel-magnitude figure flatters bright pixels and exaggerates dark ones, and both
// errors were made in the numbers this file used to print.
static float srgb_code_value(float scene_linear)
{
    return 255.0f * srgb_encode_ch(satf(scene_linear));
}
static void worst_of(F3 a, F3 b, double &worst_rel, double &worst_cv)
{
    const float ca[3] = { a.x, a.y, a.z }, cb[3] = { b.x, b.y, b.z };
    double mag = 1e-6;
    for (int k = 0; k < 3; ++k)
        mag = std::fmax(mag, std::fmax(std::fabs(double(ca[k])), std::fabs(double(cb[k]))));
    for (int k = 0; k < 3; ++k)
    {
        worst_rel = std::fmax(worst_rel, std::fabs(double(ca[k]) - double(cb[k])) / mag);
        worst_cv  = std::fmax(worst_cv,  std::fabs(double(srgb_code_value(ca[k])) - double(srgb_code_value(cb[k]))));
    }
}

static bool same_bits(F3 a, F3 b)
{
    return bits(a.x)==bits(b.x) && bits(a.y)==bits(b.y) && bits(a.z)==bits(b.z);
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("hdr_codec_selftest - replaying src/hdr_codec.hpp's decode, both graft modes\n");

    // ---------------------------------------------------------------------------------------
    // 0. The FP16 model has to be right or every number below is meaningless.
    // ---------------------------------------------------------------------------------------
    std::printf("\n0. the FP16 storage model\n");
    ck(fp16_rt(1.0f) == 1.0f, "fp16_rt(1.0) == 1.0");
    ck(fp16_rt(0.0f) == 0.0f, "fp16_rt(0.0) == 0.0");
    ck(fp16_rt(0.5f) == 0.5f, "fp16_rt(0.5) == 0.5");
    ck(std::fabs(fp16_rt(0.1f) - 0.0999755859375f) < 1e-9f, "fp16_rt(0.1) == 0.0999755859375");
    ck(fp16_rt(65504.0f) == 65504.0f, "fp16_rt(65504) == 65504 (largest finite half)");
    {
        // Every value it produces must be exactly representable as a half, i.e. idempotent.
        Rng r{0x9E3779B97F4A7C15ull};
        bool idem = true;
        for (int i = 0; i < 200000; ++i) { const float v = r.unit() * 4.0f; if (fp16_rt(fp16_rt(v)) != fp16_rt(v)) idem = false; }
        ck(idem, "fp16_rt is idempotent over 200,000 values in [0,4)");
    }

    // ---------------------------------------------------------------------------------------
    // 1. THE IDENTITY PROPERTY, MODE 0. 1,080,000 cases.
    //
    //    12 scales x 9 color_strengths x 10,000 pixels = 1,080,000, transfer_strength = 0.
    //    The premise is the one the header states: InProxy and InNeural hold IDENTICAL BITS,
    //    which is what the forced r16g16b16a16_float neural target buys. Under it the result must
    //    be the original BIT FOR BIT, and the alpha must be the source's.
    // ---------------------------------------------------------------------------------------
    std::printf("\n1. transfer_strength = 0 is an EXACT no-op in mode 0 (1,080,000 cases)\n");
    {
        static const float scales[12] = { 1.0f, 2.0f, 0.5f, 1.5f, 2.2f, 0.75f, 2.5375f, 4.0f,
                                          0.05f, 16.0f, 3.3333333f, 0.6180339f };
        static const float css[9] = { 0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f };
        Rng r{0x243F6A8885A308D3ull};
        long long cases = 0, bad = 0, bad_alpha = 0;
        double worst = 0.0;
        for (int si = 0; si < 12; ++si)
        {
            const float pw = scales[si];
            const float s  = 1.0f / (pw > 0.01f ? pw : 0.01f);
            for (int ci = 0; ci < 9; ++ci)
            {
                for (int p = 0; p < 10000; ++p)
                {
                    const F3 src = random_pixel(r);
                    const float a = r.unit() * 2.0f;
                    // The encode writes the proxy into an FP16 surface; the network returns it
                    // unchanged, so the neural surface holds THE SAME BITS.
                    const F3 enc = encode(src, s);
                    const F3 stored = mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
                    const Out o = decode_current(src, a, stored, stored, s, 0.0f, css[ci], 0u);
                    const F3 want = max3(src, 0.0f);
                    ++cases;
                    if (!same_bits(o.rgb, want))
                    {
                        ++bad;
                        const double d = std::fabs(double(o.rgb.x) - double(want.x))
                                       + std::fabs(double(o.rgb.y) - double(want.y))
                                       + std::fabs(double(o.rgb.z) - double(want.z));
                        if (d > worst) worst = d;
                    }
                    if (bits(o.a) != bits(a)) ++bad_alpha;
                }
            }
        }
        char d[192];
        std::snprintf(d, sizeof(d), "(%lld cases, %lld not bit-exact, worst abs deviation %.17g)", cases, bad, worst);
        ck(cases == 1080000, "the sweep really ran 1,080,000 cases");
        ck(bad == 0, "every case returned the original BIT FOR BIT", d);
        ck(bad_alpha == 0, "alpha came from the ORIGINAL in every case");
    }

    // ---------------------------------------------------------------------------------------
    // 2. MODE 0 IS UNCHANGED BY THIS COMMIT. 1,080,000 cases, shipping vs current, bit for bit,
    //    with the network actually CHANGING the image (so the whole expression tree is live).
    //    6 scales x 6 transfer_strengths x 5 color_strengths x 6,000 pixels = 1,080,000.
    // ---------------------------------------------------------------------------------------
    std::printf("\n2. mode 0 == the SHIPPING decode, bit for bit (1,080,000 cases)\n");
    {
        static const float scales[6] = { 1.0f, 2.0f, 0.5f, 1.5f, 2.2f, 0.05f };
        static const float tss[6] = { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f };
        static const float css[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
        Rng r{0x13198A2E03707344ull};
        long long cases = 0, bad = 0;
        for (int si = 0; si < 6; ++si)
        {
            const float pw = scales[si];
            const float s  = 1.0f / (pw > 0.01f ? pw : 0.01f);
            for (int ti = 0; ti < 6; ++ti)
            for (int ci = 0; ci < 5; ++ci)
            for (int p = 0; p < 6000; ++p)
            {
                const F3 src = random_pixel(r);
                const float a = r.unit();
                const F3 enc = encode(src, s);
                const F3 stored = mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
                const float gain = 0.6f + 1.2f * r.unit();
                const F3 neural = network_answer(stored, r, gain);
                const Out a0 = decode_shipping(src, a, stored, neural, s, tss[ti], css[ci]);
                const Out a1 = decode_current (src, a, stored, neural, s, tss[ti], css[ci], 0u);
                ++cases;
                if (!same_bits(a0.rgb, a1.rgb) || bits(a0.a) != bits(a1.a)
                    || a0.wrote_source != a1.wrote_source) ++bad;
            }
        }
        char d[128];
        std::snprintf(d, sizeof(d), "(%lld cases, %lld differed)", cases, bad);
        ck(cases == 1080000, "the sweep really ran 1,080,000 cases");
        ck(bad == 0, "mode 0 and the shipping decode agree on every bit", d);
    }

    // ---------------------------------------------------------------------------------------
    // 3. The NaN firewall and the alpha carry-through, in BOTH modes.
    // ---------------------------------------------------------------------------------------
    std::printf("\n3. the NaN firewall and alpha, in BOTH modes\n");
    {
        const float qnan = std::nanf("");
        const float inf  = INFINITY;
        const F3 broken[4] = { mk(qnan,0.2f,0.3f), mk(0.1f,inf,0.3f), mk(0.1f,0.2f,-inf), mk(qnan,qnan,qnan) };
        bool ok0 = true, ok1 = true;
        for (int i = 0; i < 4; ++i)
        {
            const F3 stored = mk(0.5f, 0.5f, 0.5f);
            for (uint32_t g = 0; g < 2; ++g)
            {
                const Out o = decode_current(broken[i], 0.375f, stored, stored, 1.0f, 1.0f, 1.0f, g);
                const bool good = o.wrote_source && bits(o.a) == bits(0.375f)
                               && bits(o.rgb.x) == bits(broken[i].x)
                               && bits(o.rgb.y) == bits(broken[i].y)
                               && bits(o.rgb.z) == bits(broken[i].z);
                if (g == 0) ok0 = ok0 && good; else ok1 = ok1 && good;
            }
        }
        ck(ok0, "mode 0 passes a broken source through untouched, alpha included");
        ck(ok1, "mode 1 passes a broken source through untouched, alpha included");

        // The graft's own output must be finite even where renodx has no guard: neural exactly
        // black (their ratio cliff) and an extreme original.
        bool finite_ok = true;
        const F3 black = mk(0.0f, 0.0f, 0.0f);
        const Out z = decode_current(mk(6.0f,5.2f,3.0f), 1.0f, black, black, 1.0f, 1.0f, 1.0f, 1u);
        finite_ok = finite_ok && !any_not_finite(z.rgb);
        const Out big = decode_current(mk(60000.0f,60000.0f,60000.0f), 1.0f, mk(1.0f,1.0f,1.0f), mk(1.0f,1.0f,1.0f),
                                       1.0f, 1.0f, 1.0f, 1u);
        finite_ok = finite_ok && !any_not_finite(big.rgb)
                    && big.rgb.x <= kMaxHalf && big.rgb.y <= kMaxHalf && big.rgb.z <= kMaxHalf;
        ck(finite_ok, "mode 1 never emits a non-finite or out-of-FP16-range value in these cases");

        // Their documented cliff, reproduced deliberately: an EXACTLY zero network answer forces
        // lerp(original, 0, ts). Assert it so nobody "fixes" it by accident.
        const Out cliff = decode_current(mk(1.0f,0.8f,0.6f), 1.0f, black, black, 1.0f, 1.0f, 1.0f, 1u);
        ck(cliff.rgb.x == 0.0f && cliff.rgb.y == 0.0f && cliff.rgb.z == 0.0f,
           "renodx's neural_y == 0 cliff is reproduced, not smoothed (result is black)");
    }

    // ---------------------------------------------------------------------------------------
    // 4. The claim the whole feature rests on: their HEADROOM TERM is our additive residual.
    //    new_y = neural_y + max(0, original_y - proxy_y)  ==  Y(original + (neural - proxy))
    //    computed with the SAME weights so the comparison is about the algebra, not the weights.
    // ---------------------------------------------------------------------------------------
    std::printf("\n4. their headroom term IS our additive residual, in luminance\n");
    {
        Rng r{0x082EFA98EC4E6C89ull};
        double worst_rel = 0.0;
        long long n = 0, lt_branch = 0, clamped = 0;
        for (int i = 0; i < 200000; ++i)
        {
            const F3 src = random_pixel(r);
            const float s = 1.0f;
            const F3 enc = encode(src, s);
            const F3 stored = mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
            const F3 neural_raw = network_answer(stored, r, 0.6f + 1.2f*r.unit());
            const F3 proxy = srgb_decode(stored);
            const F3 neural = srgb_decode(neural_raw);
            const F3 original = max3(src, 0.0f) * s;

            const float oy = rdx_lum(original), py = rdx_lum(proxy), ny = rdx_lum(neural);
            bool lt = false;
            const float theirs = rdx_new_y(oy, py, ny, lt);
            if (lt) { ++lt_branch; continue; }
            // ours, with THEIR weights, unclamped: Y(original + (neural - proxy))
            const F3 ours_lin = original + (neural - proxy);
            if (ours_lin.x < 0.0f || ours_lin.y < 0.0f || ours_lin.z < 0.0f) ++clamped;
            const float ours = rdx_lum(ours_lin);
            const double denom = std::fmax(std::fabs(double(theirs)), 1e-12);
            const double rel = std::fabs(double(theirs) - double(ours)) / denom;
            if (rel > worst_rel) worst_rel = rel;
            ++n;
        }
        char d[192];
        std::snprintf(d, sizeof(d), "(%lld samples in the else branch, %lld took original_y < proxy_y, worst relative difference %.3g)",
                      n, lt_branch, worst_rel);
        ck(worst_rel < 1e-5, "the two luminance formulations agree to float precision", d);
        ck(lt_branch > 0, "renodx's asymmetric original_y < proxy_y branch really fires on FP16 data");
        std::printf("       note: the max(...,0) scene-linear clamp bound %lld times.\n", clamped);
    }

    // ---------------------------------------------------------------------------------------
    // 5. WHERE THE TWO GRAFTS DIVERGE - and the answer is NOT "only as color_strength -> 1".
    //
    //    This section used to measure the color_strength = 0 case with random_pixel() and
    //    network_answer(), and reported 4.74 % - from which the docs concluded the two modes are
    //    nearly the same image there and told the user to A/B at 1.0. That conclusion was an
    //    artefact of the sampler: random_pixel never draws a dark, strongly chromatic pixel and
    //    network_answer never departs from the proxy by more than 0.6x..1.8x, so neither can
    //    reach the region where the modes actually part company.
    //
    //    THE MECHANISM. Mode 0 has a CHROMA VALVE: chromaWeight = saturate(originalLuminance /
    //    (kNrMinChromaLuminance/scale)) crossfades luminanceOnly toward the network's own colour
    //    once the original's luminance falls below 0.001/s. Mode 1's luminanceOnlyRdx =
    //    originalDisplay * ratioY has NO floor - faithfully to renodx - so it keeps the original's
    //    chromaticity all the way down and rescales it by an unbounded ratio. A denoiser's whole
    //    job is to move dark pixels a lot, so that region is not a curiosity.
    //
    //    Three measurements, in 8-bit sRGB code values at paper white, which is the unit a user
    //    can actually see: bright pixels agree, shadows do not, and forcing mode 0's valve open
    //    collapses the shadow difference to nothing - which pins the cause on the valve and not
    //    on anything else in either graft.
    // ---------------------------------------------------------------------------------------
    std::printf("\n5. where the two grafts diverge\n");
    {
        // 5a - the general sampler, color_strength = 0. Bright pixels: they DO agree here.
        Rng r{0xA409382229F31D00ull};
        double g_rel = 0.0, g_cv = 0.0;
        for (int i = 0; i < 60000; ++i)
        {
            const F3 src = random_pixel(r);
            const F3 enc = encode(src, 1.0f);
            const F3 stored = mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
            const F3 neural = network_answer(stored, r, 0.6f + 1.2f*r.unit());
            const Out a = decode_current(src, 1.0f, stored, neural, 1.0f, 1.0f, 0.0f, 0u);
            const Out b = decode_current(src, 1.0f, stored, neural, 1.0f, 1.0f, 0.0f, 1u);
            worst_of(a.rgb, b.rgb, g_rel, g_cv);
        }
        char d[192];
        std::snprintf(d, sizeof(d), "(60,000 pixels: worst %.1f 8-bit code values, worst %.4g of the pixel's magnitude)", g_cv, g_rel);
        ck(g_cv < 2.0, "cs=0, ORDINARY pixels: the two grafts agree to under 2 code values", d);

        // 5b - shadows, color_strength = 0. They do NOT agree here, and this is the claim the
        //      shipped docs got wrong. Asserted as a LOWER bound so nobody can quietly narrow the
        //      sampler until the divergence disappears again.
        Rng rs{0x5DEECE66Dull};
        double s_rel = 0.0, s_cv = 0.0; long long ge2 = 0, n = 0;
        F3 ws{}, wa{}, wb{}; double at_cv = 0.0;
        for (int i = 0; i < 400000; ++i)
        {
            const F3 src = shadow_pixel(rs);
            const F3 enc = encode(src, 1.0f);
            const F3 stored = mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
            const F3 neural = denoised_answer(stored, rs);
            const Out a = decode_current(src, 1.0f, stored, neural, 1.0f, 1.0f, 0.0f, 0u);
            const Out b = decode_current(src, 1.0f, stored, neural, 1.0f, 1.0f, 0.0f, 1u);
            const double before = s_cv;
            worst_of(a.rgb, b.rgb, s_rel, s_cv);
            if (s_cv > before) { ws = src; wa = a.rgb; wb = b.rgb; at_cv = s_cv; }
            double one_rel = 0.0, one_cv = 0.0;
            worst_of(a.rgb, b.rgb, one_rel, one_cv);
            if (one_cv >= 2.0) ++ge2;
            ++n;
        }
        char d2[256];
        std::snprintf(d2, sizeof(d2),
            "(%lld dark chromatic pixels: worst %.1f code values, %.1f%% differ by >= 2; worst at src [%.3g %.3g %.3g] mode0 [%.5f %.5f %.5f] mode1 [%.5f %.5f %.5f])",
            n, at_cv, 100.0*double(ge2)/double(n), ws.x, ws.y, ws.z, wa.x, wa.y, wa.z, wb.x, wb.y, wb.z);
        ck(s_cv > 10.0, "cs=0, SHADOWS: the two grafts do NOT agree - mode 1 has no chroma floor", d2);

        // 5c - the mechanism. Same shadow sampler, but mode 0's chroma valve forced open. If the
        //      difference collapses, the valve is the whole cause; if it does not, something else
        //      in one of the two grafts is also moving and the story above is incomplete.
        Rng rm{0x5DEECE66Dull};
        double m_rel = 0.0, m_cv = 0.0;
        for (int i = 0; i < 400000; ++i)
        {
            const F3 src = shadow_pixel(rm);
            const F3 enc = encode(src, 1.0f);
            const F3 stored = mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
            const F3 neural = denoised_answer(stored, rm);
            const Out a = decode_mode0_no_valve(src, stored, neural, 1.0f, 1.0f, 0.0f);
            const Out b = decode_current(src, 1.0f, stored, neural, 1.0f, 1.0f, 0.0f, 1u);
            worst_of(a.rgb, b.rgb, m_rel, m_cv);
        }
        char d3[192];
        std::snprintf(d3, sizeof(d3), "(same 400,000 pixels: worst %.1f code values, worst %.4g of the pixel's magnitude)", m_cv, m_rel);
        ck(m_cv < 1.0, "and it is ENTIRELY mode 0's chroma valve: force it open and the shadows agree", d3);

        // 5d - color_strength = 1, the headline divergence. Unchanged sampler, unchanged claim.
        Rng r1{0xA409382229F31D00ull};
        double one_rel = 0.0, one_cv = 0.0;
        F3 w1s{}, w1a{}, w1b{}; double best = 0.0;
        for (int i = 0; i < 60000; ++i)
        {
            const F3 src = random_pixel(r1);
            const F3 enc = encode(src, 1.0f);
            const F3 stored = mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
            const F3 neural = network_answer(stored, r1, 0.6f + 1.2f*r1.unit());
            const Out a = decode_current(src, 1.0f, stored, neural, 1.0f, 1.0f, 1.0f, 0u);
            const Out b = decode_current(src, 1.0f, stored, neural, 1.0f, 1.0f, 1.0f, 1u);
            const double before = one_rel;
            worst_of(a.rgb, b.rgb, one_rel, one_cv);
            if (one_rel > before) { w1s = src; w1a = a.rgb; w1b = b.rgb; best = one_rel; }
        }
        char d4[256];
        std::snprintf(d4, sizeof(d4),
            "(worst %.4g of the pixel's magnitude, %.1f code values, at src [%.4f %.4f %.4f]: mode0 [%.4f %.4f %.4f] mode1 [%.4f %.4f %.4f])",
            best, one_cv, w1s.x, w1s.y, w1s.z, w1a.x, w1a.y, w1a.z, w1b.x, w1b.y, w1b.z);
        ck(one_rel > 0.30, "cs=1: they genuinely differ on BRIGHT saturated pixels - this is the A/B", d4);

        // 5e - the case the review supplied, printed verbatim: a dim red shadow the network
        //      denoises to a neutral 0.2. It is the shortest statement of what 5b measures.
        {
            const F3 src = mk(1e-5f, 0.0f, 0.0f);
            const F3 enc = encode(src, 1.0f);
            const F3 stored = mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
            const F3 ne = srgb_encode(mk(0.2f, 0.2f, 0.2f));
            const F3 neural = mk(fp16_rt(ne.x), fp16_rt(ne.y), fp16_rt(ne.z));
            const Out a = decode_current(src, 1.0f, stored, neural, 1.0f, 1.0f, 0.0f, 0u);
            const Out b = decode_current(src, 1.0f, stored, neural, 1.0f, 1.0f, 0.0f, 1u);
            std::printf("       worked example, cs=0, src [1e-5 0 0], network returns a neutral 0.2:\n"
                        "         mode0 [%.5f %.5f %.5f]  (the valve handed the pixel to the network's grey)\n"
                        "         mode1 [%.5f %.5f %.5f]  (no valve: the original's red, rescaled)\n",
                        a.rgb.x, a.rgb.y, a.rgb.z, b.rgb.x, b.rgb.y, b.rgb.z);
        }
    }

    // ---------------------------------------------------------------------------------------
    // 6. Mode 1's transfer_strength = 0 is NOT exact, and it is exact only for a power-of-two
    //    paper_white_scale. Asserted so the README's claim stays true, and so the default never
    //    quietly moves to the mode that lacks the identity.
    //
    //    4/3 IS THE SHIPPING DEFAULT (cfg::paper_white_scale, derived in hdr_codec.hpp's "THE
    //    SCALE, s" section). It is in this list so that the bit-exact mode 0 identity is asserted
    //    AT THE VALUE THE USER ACTUALLY PLAYS ON rather than only at values nobody ships - and so
    //    that the cost of the new default, mode 1 losing its exact round trip because 4/3 is not a
    //    power of two, is a measured line in the CI log instead of a claim in a comment.
    // ---------------------------------------------------------------------------------------
    std::printf("\n6. mode 1 at transfer_strength = 0: near-identity, not identity\n");
    {
        static const float pws[7] = { 1.0f, 2.0f, 0.5f, 1.5f, 2.2f, 0.75f, 4.0f / 3.0f };
        for (int i = 0; i < 7; ++i)
        {
            const float pw = pws[i];
            const float s = 1.0f / (pw > 0.01f ? pw : 0.01f);
            Rng r{0xC0FFEE0000000001ull + uint64_t(i)};
            long long bad0 = 0, bad1 = 0; double worst = 0.0;
            for (int p = 0; p < 20000; ++p)
            {
                const F3 src = random_pixel(r);
                const F3 enc = encode(src, s);
                const F3 stored = mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
                const F3 neural = network_answer(stored, r, 0.6f + 1.2f*r.unit());
                const F3 want = max3(src, 0.0f);
                const Out m0 = decode_current(src, 1.0f, stored, neural, s, 0.0f, 1.0f, 0u);
                const Out m1 = decode_current(src, 1.0f, stored, neural, s, 0.0f, 1.0f, 1u);
                if (!same_bits(m0.rgb, want)) ++bad0;
                if (!same_bits(m1.rgb, want))
                {
                    ++bad1;
                    const float w[3] = { want.x, want.y, want.z };
                    const float g[3] = { m1.rgb.x, m1.rgb.y, m1.rgb.z };
                    for (int k = 0; k < 3; ++k)
                    {
                        const double m = std::fmax(std::fabs(double(w[k])), 1e-9);
                        worst = std::fmax(worst, std::fabs(double(g[k]) - double(w[k])) / m);
                    }
                }
            }
            std::printf("       paper_white_scale=%-7.4g  mode0 non-exact %5lld/20000   mode1 non-exact %5lld/20000  (worst rel %.3g)\n",
                        pw, bad0, bad1, worst);
            char d[96]; std::snprintf(d, sizeof(d), "(paper_white_scale=%g)", pw);
            ck(bad0 == 0, "mode 0 is exact", d);
            if (pw == 1.0f || pw == 2.0f || pw == 0.5f)
                ck(bad1 == 0, "mode 1 is exact at a power-of-two scale", d);
            else
                ck(bad1 > 0 && worst < 1e-6, "mode 1 is inexact but only at ~1e-7 relative", d);
        }
    }

    // ---------------------------------------------------------------------------------------
    // 7. The magnitude sweep the README quotes. Not a pass/fail - it is the table.
    // ---------------------------------------------------------------------------------------
    std::printf("\n7. luminance gain vs source magnitude (warm neon hue, network asking +30%%)\n");
    std::printf("       %-8s %-10s %-10s %-10s %-10s %-10s\n", "mag", "srcY", "proxyY", "gain m0", "gain m1", "|dChroma|");
    {
        static const float mags[7] = { 0.10f, 0.30f, 0.90f, 1.60f, 2.30f, 3.00f, 8.00f };
        for (int i = 0; i < 7; ++i)
        {
            const float m = mags[i];
            const F3 src = mk(1.00f*m, 0.72f*m, 0.42f*m);
            const float s = 1.0f;
            const F3 enc = encode(src, s);
            const F3 stored = mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
            // The network returns a proportional +30% in DISPLAY-referred space.
            const F3 lin = srgb_decode(stored);
            const F3 nudged = mk(satf(lin.x*1.30f), satf(lin.y*1.30f), satf(lin.z*1.30f));
            const F3 ne = srgb_encode(nudged);
            const F3 neural = mk(fp16_rt(ne.x), fp16_rt(ne.y), fp16_rt(ne.z));

            const Out a = decode_current(src, 1.0f, stored, neural, s, 1.0f, 1.0f, 0u);
            const Out b = decode_current(src, 1.0f, stored, neural, s, 1.0f, 1.0f, 1u);
            const float sy = rdx_lum(src);
            const float py = rdx_lum(srgb_decode(stored));
            const float ga = sy > 0.0f ? rdx_lum(a.rgb) / sy : 1.0f;
            const float gb = sy > 0.0f ? rdx_lum(b.rgb) / sy : 1.0f;
            // rg chromaticity distance between the two results.
            auto rg = [](F3 c, float &rr, float &gg) {
                const float t = c.x + c.y + c.z; if (t <= 0.0f) { rr = gg = 1.0f/3.0f; return; }
                rr = c.x / t; gg = c.y / t; };
            float ar, ag2, br, bg; rg(a.rgb, ar, ag2); rg(b.rgb, br, bg);
            const float dc = std::sqrt((ar-br)*(ar-br) + (ag2-bg)*(ag2-bg));
            std::printf("       %-8.2f %-10.4f %-10.4f %-10.4f %-10.4f %-10.4f\n", m, sy, py, ga, gb, dc);
        }
        std::printf("       (gains equal at every magnitude; the divergence is entirely chroma)\n");
    }

    // ---------------------------------------------------------------------------------------
    // 8. THE TRANSFER'S CEILING, IN THE PRECISION IT IS ACTUALLY STORED IN.
    //
    //    The docs used to say "neither mode recovers a highlight above about 3.5x paper white",
    //    derived from FP32: 0.25*exp(-5.77*(v-0.75)) drops below 2^-25, so SoftClip(v) rounds to
    //    exactly 1.0f, at v > 3.47. That number is real and it is the WRONG ONE, because the
    //    proxy is not kept in FP32 - it is written to an r16g16b16a16_float texture and read back
    //    out of it (stray_dlssnr.cpp's proxy_desc; hdr_codec.hpp's "WHY THE DECODE RE-READS THE
    //    PROXY" section). In half precision the encoded proxy quantises to exactly 1.0 far
    //    earlier, and the usable transfer is nearly gone well before even that.
    //
    //    Both thresholds are measured here so the documented numbers cannot drift from the code.
    // ---------------------------------------------------------------------------------------
    std::printf("\n8. where the proxy saturates, FP32 vs the FP16 surface it is stored in\n");
    {
        float first_fp32 = -1.0f, first_fp16 = -1.0f;
        for (double v = 0.5; v < 8.0 && (first_fp32 < 0.0f || first_fp16 < 0.0f); v += 0.0001)
        {
            const float f = float(v);
            if (first_fp32 < 0.0f && soft_clip_ch(f) == 1.0f) first_fp32 = f;
            if (first_fp16 < 0.0f && fp16_rt(srgb_encode_ch(soft_clip_ch(f))) == 1.0f) first_fp16 = f;
        }
        char d[192];
        std::snprintf(d, sizeof(d), "(SoftClip(v) == 1.0f in FP32 at v >= %.4f; fp16(SrgbEncode(SoftClip(v))) == 1.0f at v >= %.4f)",
                      first_fp32, first_fp16);
        ck(first_fp16 > 1.75f && first_fp16 < 1.90f,
           "the FP16-stored proxy saturates at ~1.81x paper white, NOT the FP32 figure", d);
        ck(first_fp32 > 3.40f && first_fp32 < 3.55f,
           "the FP32 soft clip saturates at ~3.47x - the number the docs used to quote", d);

        // How much of a requested gain actually survives, as a function of v = magnitude * s.
        // These figures are in units of PAPER WHITE and are invariant in paper_white_scale; what
        // moves with paper_white_scale is the SCENE-LINEAR magnitude they land at.
        std::printf("       delivered fraction of a requested +30%% display gain, by paper_white_scale:\n");
        float inv95 = -1.0f;
        for (float pw : { 1.0f, 2.0f, 4.0f })
        {
            const float s = 1.0f / pw;
            float last95 = -1.0f, last50 = -1.0f, last05 = -1.0f;
            for (float v = 0.05f; v <= 4.0f; v += 0.0005f)
            {
                const float m = v / s;
                const F3 src = mk(1.00f*m, 0.72f*m, 0.42f*m);
                const F3 enc = encode(src, s);
                const F3 stored = mk(fp16_rt(enc.x), fp16_rt(enc.y), fp16_rt(enc.z));
                const F3 lin = srgb_decode(stored);
                const F3 nud = mk(satf(lin.x*1.30f), satf(lin.y*1.30f), satf(lin.z*1.30f));
                const F3 ne = srgb_encode(nud);
                const F3 neural = mk(fp16_rt(ne.x), fp16_rt(ne.y), fp16_rt(ne.z));
                const Out a = decode_current(src, 1.0f, stored, neural, s, 1.0f, 1.0f, 0u);
                const float sy = rdx_lum(src);
                const float frac = ((rdx_lum(a.rgb) / sy) - 1.0f) / 0.30f;
                if (frac >= 0.95f) last95 = v;
                if (frac >= 0.50f) last50 = v;
                if (frac >= 0.05f) last05 = v;
            }
            std::printf("         paper_white_scale=%-4.1f  >=95%% to %.2fx paper white (magnitude %.2f)"
                        "   >=50%% to %.2fx (%.2f)   >=5%% to %.2fx (%.2f)\n",
                        pw, last95, last95/s, last50, last50/s, last05, last05/s);
            if (pw == 1.0f) inv95 = last95;
            else
            {
                char dd[128]; std::snprintf(dd, sizeof(dd), "(pw=%.1f gives %.2fx, pw=1.0 gives %.2fx)", pw, last95, inv95);
                ck(std::fabs(last95 - inv95) < 0.02f,
                   "the ceiling is INVARIANT in units of paper white - pw moves the magnitude, not the ratio", dd);
            }
        }
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
