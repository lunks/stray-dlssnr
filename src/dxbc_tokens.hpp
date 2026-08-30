// dxbc_tokens.hpp - the subset of the D3D11 Tokenized Program Format needed by the probe.
//
// These are transcribed from the WDK header 'd3d11TokenizedProgramFormat.hpp' (the same one
// Luma-Framework vendors at Source/External/WDK/includes/). Only the decoders and enumerators
// actually used by shader_detect.hpp are reproduced, so the build stays self-contained without
// vendoring a 3000-line Microsoft header.
//
// EVERY numeric value below was verified by compiling the real WDK header on the host and
// printing the enumerator, rather than being copied from documentation. See README "Verification".
#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------------------------
// Opcode token 0 layout
// ---------------------------------------------------------------------------------------------
// WDK:416,422-423,509-510
#define D3D10_SB_OPCODE_TYPE_MASK                   0x000007ff
#define D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH_MASK  0x7f000000
#define D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH_SHIFT 24
#define D3D10_SB_OPCODE_EXTENDED_MASK               0x80000000u
#define D3D10_SB_OPCODE_EXTENDED_SHIFT              31

// WDK:418,427,514
#define DECODE_D3D10_SB_OPCODE_TYPE(t)                  ((t) & D3D10_SB_OPCODE_TYPE_MASK)
#define DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(t) (((t) & D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH_MASK) >> D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH_SHIFT)
#define DECODE_IS_D3D10_SB_OPCODE_EXTENDED(t)           (((t) & D3D10_SB_OPCODE_EXTENDED_MASK) >> D3D10_SB_OPCODE_EXTENDED_SHIFT)

// ---------------------------------------------------------------------------------------------
// dcl_resource token layout - WDK:1215-1216, 1221
// ---------------------------------------------------------------------------------------------
#define D3D10_SB_RESOURCE_DIMENSION_MASK   0x0000F800
#define D3D10_SB_RESOURCE_DIMENSION_SHIFT  11
#define DECODE_D3D10_SB_RESOURCE_DIMENSION(t) (((t) & D3D10_SB_RESOURCE_DIMENSION_MASK) >> D3D10_SB_RESOURCE_DIMENSION_SHIFT)

// WDK:636-637, 1276
#define D3D10_SB_RESOURCE_RETURN_TYPE_MASK    0x0000000f
#define D3D10_SB_RESOURCE_RETURN_TYPE_NUMBITS 4
#define DECODE_D3D10_SB_RESOURCE_RETURN_TYPE(tok, comp) \
	(((tok) >> ((comp) * D3D10_SB_RESOURCE_RETURN_TYPE_NUMBITS)) & D3D10_SB_RESOURCE_RETURN_TYPE_MASK)

// ---------------------------------------------------------------------------------------------
// Operand token 0 layout - WDK:751,756,956-957,960,972-973,976,1007-1008,1012
// ---------------------------------------------------------------------------------------------
#define D3D10_SB_OPERAND_NUM_COMPONENTS_MASK   0x00000003
#define DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(t) ((t) & D3D10_SB_OPERAND_NUM_COMPONENTS_MASK)

#define D3D10_SB_OPERAND_TYPE_MASK   0x000ff000
#define D3D10_SB_OPERAND_TYPE_SHIFT  12
#define DECODE_D3D10_SB_OPERAND_TYPE(t) (((t) & D3D10_SB_OPERAND_TYPE_MASK) >> D3D10_SB_OPERAND_TYPE_SHIFT)

#define D3D10_SB_OPERAND_INDEX_DIMENSION_MASK  0x00300000
#define D3D10_SB_OPERAND_INDEX_DIMENSION_SHIFT 20
#define DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(t) (((t) & D3D10_SB_OPERAND_INDEX_DIMENSION_MASK) >> D3D10_SB_OPERAND_INDEX_DIMENSION_SHIFT)

#define D3D10_SB_OPERAND_EXTENDED_MASK  0x80000000u
#define D3D10_SB_OPERAND_EXTENDED_SHIFT 31
#define DECODE_IS_D3D10_SB_OPERAND_EXTENDED(t) (((t) & D3D10_SB_OPERAND_EXTENDED_MASK) >> D3D10_SB_OPERAND_EXTENDED_SHIFT)

// ---------------------------------------------------------------------------------------------
// Enumerator values. Verified by compiling the real WDK header, not from documentation.
// ---------------------------------------------------------------------------------------------
enum : uint32_t
{
	// Opcodes
	D3D10_SB_OPCODE_ENDLOOP                         = 22,
	D3D10_SB_OPCODE_LOOP                            = 48,
	D3D10_SB_OPCODE_MIN                             = 51,
	D3D10_SB_OPCODE_MAX                             = 52,
	D3D10_SB_OPCODE_CUSTOMDATA                      = 53,
	D3D10_SB_OPCODE_DCL_RESOURCE                    = 88,
	D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER             = 89,
	D3D10_SB_OPCODE_DCL_OUTPUT                      = 101,
	D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS                = 106,
	D3D10_1_SB_OPCODE_GATHER4                       = 109,
	D3D11_SB_OPCODE_GATHER4_C                       = 126,
	D3D11_SB_OPCODE_GATHER4_PO                      = 127,
	D3D11_SB_OPCODE_GATHER4_PO_C                    = 128,
	D3D11_SB_OPCODE_DCL_STREAM                      = 143,
	D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED = 156,
	D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED         = 162,
	D3D11_SB_OPCODE_LD_STRUCTURED                   = 167,
	D3D11_SB_OPCODE_STORE_STRUCTURED                = 168,
	D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT           = 206,

	// Resource dimensions
	D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D = 3,
	D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D = 5,

	// Resource return types
	D3D10_SB_RETURN_TYPE_FLOAT = 5,

	// 4-component selectors used by DECODE_D3D10_SB_RESOURCE_RETURN_TYPE
	D3D10_SB_4_COMPONENT_X = 0,
	D3D10_SB_4_COMPONENT_Y = 1,
	D3D10_SB_4_COMPONENT_Z = 2,
	D3D10_SB_4_COMPONENT_W = 3,

	// Operand types
	D3D10_SB_OPERAND_TYPE_IMMEDIATE32     = 4,
	D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER = 8,

	// Operand component counts
	D3D10_SB_OPERAND_0_COMPONENT = 0,
	D3D10_SB_OPERAND_1_COMPONENT = 1,
	D3D10_SB_OPERAND_4_COMPONENT = 2,

	// Operand index dimensions
	D3D10_SB_OPERAND_INDEX_0D = 0,
	D3D10_SB_OPERAND_INDEX_1D = 1,
	D3D10_SB_OPERAND_INDEX_2D = 2,
	D3D10_SB_OPERAND_INDEX_3D = 3,
};

// ---------------------------------------------------------------------------------------------
// Pre-computed literal tokens.
//
// Luma builds these at runtime through the ENCODE_* macro chain. They are compile-time constants,
// so they are computed once here and the derivation is recorded. All six were verified by running
// the real ENCODE_* macros from the WDK header on the host.
// ---------------------------------------------------------------------------------------------

// ENCODE_D3D10_SB_OPCODE_TYPE(LOOP) | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1)
static constexpr uint32_t kTokenLoop    = 0x01000030u;
// ENCODE_D3D10_SB_OPCODE_TYPE(ENDLOOP) | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(1)
static constexpr uint32_t kTokenEndloop = 0x01000016u;

// NUM_COMPONENTS(4) | SELECTION_MODE(SWIZZLE) | <swizzle> | OPERAND_TYPE(CONSTANT_BUFFER)
//   | INDEX_DIMENSION(2D) | INDEX_REPRESENTATION(0, IMMEDIATE32) | INDEX_REPRESENTATION(1, <rep>)
// swizzle .xywx == (0,1,3,0), swizzle .xxyw == (0,0,1,3)
static constexpr uint32_t kTokenCbXywxImm    = 0x00208346u;
static constexpr uint32_t kTokenCbXywxImmRel = 0x06208346u;
static constexpr uint32_t kTokenCbXxywImm    = 0x00208D06u;
static constexpr uint32_t kTokenCbXxywImmRel = 0x06208D06u;

// 4.00801611f == 1.0f / (0.499f * 0.5f), the DecodeVelocityFromTexture scale in Common.ush.
// Bit pattern verified on host: 0x408041AB, little-endian bytes AB 41 80 40.
static constexpr uint32_t kVelocityDecodeScaleBits = 0x408041ABu;

// DLSS-NR ADDITION - the OTHER half of DecodeVelocityFromTexture, and it is not 0.5.
//
//   V.xy = EncodedV.xy * InvDiv - 32767.0f/65535.0f * InvDiv          Common.ush:1561
//
// The compiler folds the second term into a MAD immediate: (32767/65535) * InvDiv
// = 2.00397754f == 0x4000412B, which appears NEGATED in a mad as 0xC000412B (bytes 2B 41 00 C0).
//
// This is scanned for and LOGGED, and is deliberately NOT a gate. Gate B has only ever matched
// the SCALE, so "STRAY's decode is stock" was an inference; this turns it into a measurement on
// the user's next normal run, at zero behavioural risk. If it does NOT appear alongside the
// scale in the pinned TAA shader, STRAY customised DecodeVelocityFromTexture and stock UE source
// is not authoritative for the bias - which would mean mvec_decode must be re-derived from the
// game's own DXBC before it is trusted.
//
// Bit patterns recomputed on this host: 32767/65535 = 0.49999237f = 0x3EFFFF00, and
// 0.49999237f * 4.00801611f = 2.00397754f = 0x4000412B.
static constexpr uint32_t kVelocityDecodeBiasBits    = 0x4000412Bu;
static constexpr uint32_t kVelocityDecodeNegBiasBits = 0xC000412Bu;
