// renodx-codec-shaders.hlsl - the RenoDX DLSS5 add-on's HDR codec, RECOVERED IN FULL.
//
// PROVENANCE. Everything below is plaintext HLSL that ships inside
// renodx-reference.addon64 (a copy of renodx-dlss5-v2.5.addon64, PE32+ DLL, imagebase
// 0x180000000). The add-on compiles it at runtime with D3DCompile: the two log strings
// "DLSS5 Generic proxy encode" [BIN file 0x3fcf8] and "DLSS5 Generic proxy decode"
// [BIN file 0x3fd18] name the two compiles.
//
//   COMMON PRELUDE - contiguous, verbatim, 4397 bytes.
//     file 0x41590..0x426bd, .rdata RVA 0x42f90..0x440bd, referenced by exactly one
//     RIP-relative lea at .text RVA 0xc047 [BIN]. Recovered by a straight read; no
//     inference of any kind.
//
//   THE TWO main() BODIES - file 0x427b0..0x42e90, 1760 bytes.
//     This region is NOT the source in reading order. It is 110 records of exactly 16
//     bytes, DEDUPLICATED (110 stored / 110 distinct) and stored SORTED BY THE REVERSED
//     BYTE STRING - verified exactly: sorted(chunks, key=lambda c: c[::-1]) == chunks.
//     Nothing in .text references it (zero RIP-relative leas into the range, zero
//     absolute qword pointers into it), so the reading order is not recoverable from the
//     file and the two bodies were reassembled as a jigsaw.
//
//     THE REASSEMBLY IS NOT A GUESS. It is closed: the encode body below tiles into 44
//     full 16-byte records, the decode body into 76, they share 9 records (the identical
//     prologue) plus one record the decode itself uses twice (";\n  const float3"), and
//     44 + 76 - 9 - 1 = 110 = every stored record, each consumed EXACTLY ONCE, with ZERO
//     records left over and ZERO records unmatched. The only 16 bytes not proven by the
//     file are the encode's final partial record, "ource.a);\n}\n" (12 chars, which in
//     the binary would carry NUL padding and therefore falls outside the printable run) -
//     and those characters are forced by the record that precedes them, " float4(proxy, s".
//
//     Both bodies below were then re-tiled and re-checked against the binary, and both
//     compile clean through glslang's HLSL front end
//     (glslangValidator -D -e main -S comp --target-env vulkan1.1).
//
// This file is a REFERENCE DUMP of someone else's shipped shader, kept for comparison
// against src/hdr_codec.hpp. It is not compiled by this add-on's build.
//
// To compile a single entry point:  -D RENODX_ENCODE   (otherwise the decode is built).

Texture2D<float4> Original : register(t0);
Texture2D<float4> Proxy : register(t1);
Texture2D<float4> Neural : register(t2);
Texture2D<float4> OutputOriginal : register(t3);
RWTexture2D<float4> Output : register(u0);

cbuffer CodecConstants : register(b0) {
  uint2 Size;
  uint2 SourceSize;
  uint2 SourceBase;
  uint2 ProxySize;
  float PaperWhiteScale;
  float TransferStrength;
  float ColorStrength;
  uint HdrMode;
  float4 Padding;
};

float Luminance(float3 color) {
  return dot(color, float3(0.212639, 0.715169, 0.072192));
}

float3 SrgbEncode(float3 color) {
  color = saturate(color);
  return color <= 0.0031308
      ? color * 12.92
      : 1.055 * pow(color, 1.0 / 2.4) - 0.055;
}

float3 SrgbDecode(float3 color) {
  color = saturate(color);
  return color <= 0.04045
      ? color / 12.92
      : pow((color + 0.055) / 1.055, 2.4);
}

float3 CbrtSigned(float3 value) {
  return sign(value) * pow(abs(value), 1.0 / 3.0);
}

float3 ToOkLab(float3 color) {
  const float3x3 rgb_to_lms = {
    0.4122214708, 0.5363325363, 0.0514459929,
    0.2119034982, 0.6806995451, 0.1073969566,
    0.0883024619, 0.2817188376, 0.6299787005
  };
  const float3x3 lms_to_lab = {
    0.2104542553, 0.7936177850, -0.0040720468,
    1.9779984951, -2.4285922050, 0.4505937099,
    0.0259040371, 0.7827717662, -0.8086757660
  };
  return mul(lms_to_lab, CbrtSigned(mul(rgb_to_lms, color)));
}

float3 FromOkLab(float3 lab) {
  const float3x3 lab_to_lms = {
    1.0, 0.3963377774, 0.2158037573,
    1.0, -0.1055613458, -0.0638541728,
    1.0, -0.0894841775, -1.2914855480
  };
  const float3x3 lms_to_rgb = {
    4.0767416621, -3.3077115913, 0.2309699292,
    -1.2684380046, 2.6097574011, -0.3413193965,
    -0.0041960863, -0.7034186147, 1.7076147010
  };
  float3 lms = mul(lab_to_lms, lab);
  return mul(lms_to_rgb, lms * lms * lms);
}

float3 ClampAp1(float3 color) {
  const float3x3 bt709_to_ap1 = {
    0.613097, 0.339523, 0.047379,
    0.070194, 0.916354, 0.013452,
    0.020616, 0.109570, 0.869815
  };
  const float3x3 ap1_to_bt709 = {
    1.705051, -0.621792, -0.083259,
    -0.130256, 1.140805, -0.010548,
    -0.024003, -0.128969, 1.152972
  };
  return mul(ap1_to_bt709, max(0.0, mul(bt709_to_ap1, color)));
}

float3 HueOkLab(float3 incorrect, float3 correct) {
  float3 incorrect_lab = ToOkLab(incorrect);
  const float3 correct_lab = ToOkLab(correct);
  const float incorrect_chroma = length(incorrect_lab.yz);
  const float correct_chroma = length(correct_lab.yz);
  incorrect_lab.yz = correct_lab.yz
      * (correct_chroma == 0.0 ? 1.0 : incorrect_chroma / correct_chroma);
  return ClampAp1(FromOkLab(incorrect_lab));
}

float3 UpgradeToneMap(float3 original, float3 proxy, float3 neural) {
  const float original_y = Luminance(original);
  const float proxy_y = Luminance(proxy);
  const float neural_y = Luminance(neural);
  float ratio;
  if (original_y < proxy_y) {
    ratio = original_y / proxy_y;
  } else {
    const float new_y = neural_y + max(0.0, original_y - proxy_y);
    ratio = neural_y > 0.0 ? new_y / neural_y : 0.0;
  }
  const float3 scaled = HueOkLab(neural * ratio, neural);
  return lerp(original, scaled, TransferStrength);
}

float3 NeuralLinear(uint2 pixel) {
  const float3 value = Neural.Load(int3(pixel, 0)).rgb;
  return HdrMode != 0 ? SrgbDecode(value) : value;
}

// DLSSNR 310.8 writes the neural answer at its active network resolution even
// when the Output resource is larger (the signed runtime reports success but
// leaves the remainder untouched).  Sample that populated region explicitly
// so the final display target is always fully covered.
float3 SampleNeural(uint2 pixel) {
  const uint2 neural_size = max(ProxySize, uint2(1, 1));
  const float2 position =
      ((float2(pixel) + 0.5) * float2(neural_size)) / float2(Size) - 0.5;
  const int2 base = int2(floor(position));
  const float2 fraction = saturate(position - floor(position));
  const int2 max_pixel = int2(neural_size) - 1;
  const uint2 p00 = uint2(clamp(base, int2(0, 0), max_pixel));
  const uint2 p10 = uint2(clamp(base + int2(1, 0), int2(0, 0), max_pixel));
  const uint2 p01 = uint2(clamp(base + int2(0, 1), int2(0, 0), max_pixel));
  const uint2 p11 = uint2(clamp(base + int2(1, 1), int2(0, 0), max_pixel));
  return lerp(
      lerp(NeuralLinear(p00), NeuralLinear(p10), fraction.x),
      lerp(NeuralLinear(p01), NeuralLinear(p11), fraction.x),
      fraction.y);
}

#ifdef RENODX_ENCODE
[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;
  const uint2 source_size = max(SourceSize, uint2(1, 1));
  const uint2 source_pixel = SourceBase + min(
      uint2(((float2(pixel) + 0.5) * float2(source_size)) / float2(Size)),
      source_size - 1);
  const float4 source = Original.Load(int3(source_pixel, 0));
  float3 proxy = max(source.rgb, 0.0);
  if (HdrMode != 0) {
    proxy /= PaperWhiteScale;
    const float3 shoulder = 0.75 + 0.25 * (1.0 - exp(-5.770780 * (proxy - 0.75)));
    proxy = proxy <= 0.75 ? proxy : shoulder;
    proxy = SrgbEncode(proxy);
  }
  Output[pixel] = float4(proxy, source.a);
}

#else
[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (any(pixel >= Size)) return;
  const float4 source = OutputOriginal.Load(int3(pixel, 0));
  const uint2 proxy_size = max(ProxySize, uint2(1, 1));
  const uint2 proxy_pixel = min(
      uint2(((float2(pixel) + 0.5) * float2(proxy_size)) / float2(Size)),
      proxy_size - 1);
  const float3 original = HdrMode != 0
      ? max(source.rgb, 0.0) / PaperWhiteScale
      : source.rgb;
  const float3 proxy = HdrMode != 0
      ? SrgbDecode(Proxy.Load(int3(proxy_pixel, 0)).rgb)
      : Proxy.Load(int3(proxy_pixel, 0)).rgb;
  const float3 neural = SampleNeural(pixel);
  const float3 upgraded = HdrMode != 0
      ? UpgradeToneMap(original, proxy, neural)
      : lerp(original, neural, TransferStrength);

  const float original_y = Luminance(original);
  const float upgraded_y = Luminance(upgraded);
  const float ratio = original_y == 0.0 ? 1.0 : upgraded_y / original_y;
  const float3 luminance_only = original * ratio;
  const float3 result = lerp(luminance_only, upgraded, ColorStrength);
  Output[pixel] = float4(
      HdrMode != 0 ? result * PaperWhiteScale : result,
      source.a);
}

#endif
