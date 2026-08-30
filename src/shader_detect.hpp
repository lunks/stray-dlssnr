// shader_detect.hpp - UE4 TAA shader identification by pure DXBC token analysis.
//
// Lifted from Luma-Framework:
//   Source/Games/Unreal Engine/includes/shader_detect.hpp
// (IsUE4TAACandidate, GetTAAShaderConfidence, FindLargestCBufferDeclaration, FindShaderInfo,
//  ScanMemoryForPattern, word_t, TAAShaderInfo)
//
// Luma runs this against D3D11. It ports to D3D12 UNCHANGED because it makes no API calls at
// all - it only walks the DXBC instruction-token stream. UE 4.27 sets
// GShaderPlatformForFeatureLevel[SM5] = SP_PCD3D_SM5 under both the D3D11 and D3D12 RHIs, and
// PCD3D_SM5 is DXBC (DXIL requires SM6, which is UE5), so the bytecode is byte-identical.
//
// DELIBERATE DIVERGENCES FROM UPSTREAM, each marked FIX-n at its site:
//   FIX-1  Upstream assumes code[0] is the first instruction token. ReShade hands us the whole
//          DXBC container, so StripDxbcContainer() below finds the SHEX/SHDR chunk first.
//   FIX-2  FindShaderInfo's backwards scan is `for (size_t i = n-1; i - 3 >= 0; i--)`. `i` is
//          unsigned, so the condition is always true; when no window of four consecutive indices
//          exists the loop reads index_array[SIZE_MAX] and never terminates. Rewritten with a
//          bounded loop. Upstream has not hit this because it only ever runs on real TAA
//          shaders; this probe runs it on every candidate in a game that ships no TAA
//          replacement path, so the non-matching case is the COMMON case here.
//   FIX-3  GetTAAShaderConfidence walks D3D10_SB_OPCODE_CUSTOMDATA as if it were an instruction.
//          CUSTOMDATA encodes length 0 in the token and the real length in the NEXT dword, so
//          upstream desynchronises and scores the payload as opcodes. Handled explicitly.
//   FIX-4  Upstream's `int32_t best_start = UINT32_MAX;` relies on implementation-defined
//          narrowing to produce the -1 sentinel. Written as -1.
//   FIX-5  Upstream trusts fixed token offsets that are only correct for SM 5.0 operand
//          encodings (1D-indexed dcl_resource, 2D-indexed dcl_constant_buffer). SM 5.1 makes
//          both 3D-indexed and every offset shifts. We verify the index dimension before
//          trusting the offsets, and report the shader model so the assumption is settled by
//          observation rather than by faith.
//   FIX-6  StripDxbcContainer's chunk bounds are computed in size_t and phrased as subtractions.
//          Written as additions they are evaluated in 32-bit unsigned arithmetic on untrusted
//          header fields and wrap straight through the guard.
//   FIX-7  The SHEX byte-code length is bounded by chunk_size - 8, not chunk_size: out.code sits
//          8 bytes past the point at which the chunk bound was established.
//   FIX-8  Upstream's `instruction_count > 16` bail-out in IsUE4TAACandidate is DEAD CODE - the
//          counter is never incremented there. An increment added during the port activated it
//          and made the census reject shaders upstream accepts. Removed, matching upstream.
//   FIX-9  TAAShaderInfo carries declared_srv_register_mask, the exact set of declared
//          t-registers. Upstream has no equivalent because on D3D11 it never dereferences a
//          slot the shader did not declare; the D3D12 join does, so it needs the exact set.
//
// Everything else is upstream logic, including its quirks - see the notes at Gate C and
// GOTCHA-5 in the README. Do not "tighten" the unaligned byte scans; that looseness is what
// lets pattern A match l(0, 4.008016, 4.008016, 0) starting at the second word.

#pragma once

#include "dxbc_tokens.hpp"

#include <cstdint>
#include <cstring>
#include <cstddef>
#include <vector>
#include <set>
#include <algorithm>

namespace probe {

// -------------------------------------------------------------------------------------------
// Luma's word_t (shader_detect.hpp:6-12)
// -------------------------------------------------------------------------------------------
union word_t
{
	float    f;
	int32_t  i;
	uint32_t u;
	uint8_t  b[4];
};

// -------------------------------------------------------------------------------------------
// Luma's System::ScanMemoryForPattern (Source/Core/utils/system.cpp:178-195)
//
// Naive byte-granular memcmp sweep. There is deliberately NO dword-alignment requirement: a
// 4.008016 immediate pair can begin at any dword within an instruction, and the operand-token
// scan in FindShaderInfo relies on matching at arbitrary byte offsets too.
// -------------------------------------------------------------------------------------------
inline size_t CountPattern(const uint8_t *base, size_t size, const uint8_t *pattern, size_t pattern_size, bool stop_at_first = false)
{
	if (base == nullptr || pattern == nullptr || pattern_size == 0 || pattern_size > size)
		return 0;

	size_t matches = 0;
	for (size_t i = 0; i <= size - pattern_size; ++i)
	{
		if (std::memcmp(base + i, pattern, pattern_size) == 0)
		{
			++matches;
			if (stop_at_first)
				break;
		}
	}
	return matches;
}

inline void CollectPattern(const uint8_t *base, size_t size, const uint8_t *pattern, size_t pattern_size, std::vector<size_t> &out_offsets)
{
	if (base == nullptr || pattern == nullptr || pattern_size == 0 || pattern_size > size)
		return;

	for (size_t i = 0; i <= size - pattern_size; ++i)
	{
		if (std::memcmp(base + i, pattern, pattern_size) == 0)
			out_offsets.push_back(i);
	}
}

// -------------------------------------------------------------------------------------------
// FIX-1: DXBC container walk.
//
// Luma's core.hpp does this before calling into shader_detect. ReShade's shader_desc::code is
// the raw application DXBC container, so we must strip it ourselves. Every offset in the
// functions below is relative to the FIRST INSTRUCTION TOKEN, not the container.
// -------------------------------------------------------------------------------------------
struct DxbcInfo
{
	const uint8_t *code = nullptr;   // first instruction token
	size_t         size = 0;         // bytes of instruction stream
	uint8_t        sm_major = 0;
	uint8_t        sm_minor = 0;
	uint16_t       program_type = 0xFFFF; // 0 = PS, 1 = VS, 2 = GS, 3 = HS, 4 = DS, 5 = CS
	bool           valid = false;
	bool           is_dxil = false;  // SM6 container - token analysis does not apply
};

inline uint32_t ReadU32(const uint8_t *p)
{
	uint32_t v;
	std::memcpy(&v, p, sizeof(v));
	return v;
}

inline DxbcInfo StripDxbcContainer(const void *blob, size_t blob_size)
{
	DxbcInfo out;
	if (blob == nullptr || blob_size < 32)
		return out;

	const uint8_t *base = static_cast<const uint8_t *>(blob);
	if (std::memcmp(base, "DXBC", 4) != 0)
		return out;

	// DXBCHeader: 'DXBC'(4) hash(16) version(4) file_size(4) chunk_count(4) chunk_offsets[]
	const uint32_t file_size   = ReadU32(base + 24);
	const uint32_t chunk_count = ReadU32(base + 28);

	// Trust the smaller of the declared size and what the caller gave us.
	const size_t   limit = (file_size != 0 && file_size <= blob_size) ? file_size : blob_size;
	if (chunk_count == 0 || chunk_count > 64)
		return out;
	if (32 + static_cast<size_t>(chunk_count) * 4 > limit)
		return out;

	for (uint32_t i = 0; i < chunk_count; ++i)
	{
		// FIX-6: every bound below is computed in size_t and phrased as a SUBTRACTION from
		// 'limit'. Written the obvious way (chunk_off + 8 > limit) the addition happens in
		// 32-bit UNSIGNED arithmetic - chunk_off is uint32_t and the literal is int - and WRAPS
		// before the widening comparison, so a container claiming chunk_off = 0xFFFFFFFF passes
		// the guard (0xFFFFFFFF + 8 == 7) and 'chunk' lands ~4 GB past the blob. ReShade hands
		// us whatever the application passed to CreatePipelineState; a probe must not trust it.
		const size_t chunk_off = ReadU32(base + 32 + i * 4);
		if (chunk_off > limit || limit - chunk_off < 8)
			continue;

		const uint8_t *chunk = base + chunk_off;

		const bool is_dxil = (std::memcmp(chunk, "DXIL", 4) == 0);
		if (is_dxil)
			out.is_dxil = true;

		// SHEX = SM5, SHDR = SM4. DXIL = SM6 and is NOT a token stream - we never parse it.
		if (std::memcmp(chunk, "SHEX", 4) != 0 && std::memcmp(chunk, "SHDR", 4) != 0)
			continue;

		// Same wrap hazard as above, same size_t + subtraction form. limit - chunk_off >= 8
		// was established by the guard above, so the inner subtraction cannot wrap either.
		const size_t chunk_size = ReadU32(chunk + 4);
		if (chunk_size < 8 || chunk_size > (limit - chunk_off) - 8)
			continue;

		// DXBCByteCodeChunk: version_major_and_minor(1) reserved(1) program_type(2)
		//                    chunk_size_dword(4) byte_code[]
		const uint8_t *body = chunk + 8;
		const uint8_t  ver  = body[0];
		uint16_t       ptype;
		std::memcpy(&ptype, body + 2, sizeof(ptype));
		const uint32_t size_dword = ReadU32(body + 4);

		// size_dword counts itself and the version/type dword, so subtract those 8 bytes.
		if (size_dword < 2)
			continue;
		const uint64_t byte_code_size = static_cast<uint64_t>(size_dword) * 4ull - 8ull;
		// FIX-7: the in-range guarantee established above is chunk_off + 8 + chunk_size <= limit,
		// while out.code is base + chunk_off + 16, so the space that actually remains after the
		// 8-byte version/length prologue is chunk_size - 8, NOT chunk_size. Comparing against
		// chunk_size lets a truncated container yield an out.size 8 bytes too large, which every
		// downstream byte scan then walks off the end of. chunk_size >= 8 is guaranteed above,
		// so this subtraction is safe.
		if (byte_code_size == 0 || byte_code_size > static_cast<uint64_t>(chunk_size) - 8ull)
			continue;

		out.code         = body + 8;
		out.size         = static_cast<size_t>(byte_code_size);
		out.sm_major     = static_cast<uint8_t>((ver >> 4) & 0xF);
		out.sm_minor     = static_cast<uint8_t>(ver & 0xF);
		out.program_type = ptype;
		out.valid        = true;
		return out;
	}

	return out;
}

// -------------------------------------------------------------------------------------------
// Luma's TAAShaderInfo (shader_detect.hpp:23-34).
//
// Upstream's source/depth/velocity_texture_register fields are NEVER written by any function in
// shader_detect.hpp - they are filled from a live D3D11 PSGetShaderResources enumeration in
// main.cpp. On D3D12 there is no such API; the descriptor shadow produces that mapping instead,
// so those fields are dropped here and the live half lives in the probe's draw handler.
// -------------------------------------------------------------------------------------------
struct TAAShaderInfo
{
	// Bytecode-derived (IsUE4TAACandidate)
	int32_t  detected_2d_texture_float_count = 0;
	int32_t  detected_3d_texture_float_count = 0;
	int32_t  output_count                    = 0;
	int32_t  max_texture_register            = -1;
	uint64_t declared_srv_register_mask      = 0;  // bit N set == the shader declares t<N>
	// DLSS-NR ADDITION. Bit N set == the shader declares u<N> through dcl_unordered_access_view_typed.
	// Needed for exactly the same reason as declared_srv_register_mask: vkd3d-proton reports
	// D3D12_RESOURCE_BINDING_TIER_3, so UE 4.27 declares every UAV range as MAX_UAVS (8) whatever
	// the shader really uses, and the slots the shader never declared hold STALE descriptors whose
	// ID3D12Resource may already be destroyed. get_resource_desc dereferences that pointer.
	// Only dcl_unordered_access_view_TYPED is counted; raw and structured UAVs are deliberately
	// left out of the mask (their opcode ordinals were not verified against the WDK header on this
	// host, and a UE 4.27 TAA output is a typed RWTexture2D). Omitting them can only ever make the
	// mask NARROWER, i.e. resolve less, which is the safe direction.
	uint64_t declared_uav_register_mask      = 0;  // bit N set == the shader declares u<N>
	int32_t  max_uav_register                = -1;
	bool     dcl_uav_index_dim_ok            = true;  // every dcl_uav_typed operand was 1D
	bool     has_multiple_render_targets     = false;
	bool     found_velocity_constant         = false;
	int32_t  velocity_constant_pattern       = -1; // which of the three byte patterns hit
	// DLSS-NR ADDITION - the DECODE BIAS, scanned and reported but NEVER gated on. See
	// dxbc_tokens.hpp kVelocityDecodeBiasBits for why this exists and what its absence would mean.
	// +1 == the negated form (a mad), 0 == the positive form, -1 == not found.
	int32_t  velocity_bias_form              = -1;
	bool     found_velocity_bias             = false;
	bool     loops_balanced_nonzero          = false;
	float    confidence                      = 0.0f;

	// Bytecode-derived (FindShaderInfo)
	int32_t  global_buffer_register_index    = -1; // the bN slot of the largest cbuffer
	int32_t  clip_to_prev_clip_start_index   = -1; // float4 row of ClipToPrevClip in that cbuffer
	uint32_t declared_cbuffer_size           = 0;  // in float4 elements
	bool     found_shader_info               = false;

	// FIX-5 diagnostics
	bool     dcl_resource_index_dim_ok       = true;  // every dcl_resource operand was 1D
	bool     dcl_cbuffer_index_dim_ok        = true;  // every dcl_constant_buffer operand was 2D
};

// -------------------------------------------------------------------------------------------
// FindLargestCBufferDeclaration (shader_detect.hpp:86-123)
//
// UE4's View Uniform Buffer is by far the largest constant buffer any shader declares, so
// "largest dcl_constant_buffer by declared float4 count" identifies it.
//
// Returns an offset (in dwords) into code_u32, or SIZE_MAX for not-found. The declaration
// indexes as p[0]=opcode, p[1]=operand token, p[2]=cb slot (bN), p[3]=size in float4s.
// -------------------------------------------------------------------------------------------
inline size_t FindLargestCBufferDeclaration(const uint32_t *code_u32, size_t size_u32, bool *out_index_dim_ok)
{
	size_t offset = 0;
	uint32_t max_cbuffer_size = 0;
	size_t instruction_count = 0;
	bool found_first_dcl_cbuffer = false;
	size_t max_cbuffer_declaration = SIZE_MAX;

	while (offset < size_u32)
	{
		if (instruction_count > 16 && !found_first_dcl_cbuffer)
			break; // bail out if we reached too far without finding any cbuffer declarations

		const uint32_t token = code_u32[offset];
		const uint32_t opcode = DECODE_D3D10_SB_OPCODE_TYPE(token);
		uint32_t len = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);
		len = (len == 0) ? 1 : len;

		if (offset + len > size_u32)
			break;

		if (opcode == D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER)
		{
			// Needs operand token + 2 index dwords.
			if (offset + 4 > size_u32)
				break;

			found_first_dcl_cbuffer = true;

			// FIX-5: the [2]=slot / [3]=size layout is only valid for a 2D-indexed operand
			// (SM 5.0). SM 5.1 makes this 3D-indexed and [2] becomes an upper bound.
			if (DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(code_u32[offset + 1]) != D3D10_SB_OPERAND_INDEX_2D)
			{
				if (out_index_dim_ok != nullptr)
					*out_index_dim_ok = false;
			}
			else
			{
				const uint32_t buffer_size = code_u32[offset + 3];
				if (buffer_size > max_cbuffer_size)
				{
					max_cbuffer_size = buffer_size;
					max_cbuffer_declaration = offset;
				}
			}
		}
		else
		{
			if (found_first_dcl_cbuffer)
				break; // we scanned all cbuffer declarations (fxc groups them)
		}

		instruction_count++;
		offset += len;
	}

	return max_cbuffer_declaration;
}

// -------------------------------------------------------------------------------------------
// GetTAAShaderConfidence (shader_detect.hpp:125-266)
//
// Five weighted terms, max 170, threshold 60:
//   min_max_count > 15   +50   neighbourhood clamp
//   gather4 family       +20
//   LDS / structured     +20
//   has 1.5 AND 2.5      +40   Catmull-Rom / filter weights
//   YCoCg matrix row     +40   4-comp immediate (1,1,2,*) or (-1,-1,2,*)
// -------------------------------------------------------------------------------------------
inline float GetTAAShaderConfidence(const uint8_t *code, size_t size)
{
	const uint32_t *code_u32 = reinterpret_cast<const uint32_t *>(code);
	const size_t size_u32 = size / sizeof(uint32_t);

	size_t min_max_count = 0;
	bool has_gather4 = false;
	bool has_lds_op  = false;
	bool has_1_5     = false;
	bool has_2_5     = false;
	bool has_ycocg   = false;

	size_t offset = 0;
	while (offset < size_u32)
	{
		const uint32_t token = code_u32[offset];
		const uint32_t opcode = DECODE_D3D10_SB_OPCODE_TYPE(token);
		uint32_t len = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);

		// FIX-3: CUSTOMDATA carries length 0 in the token and the real dword length in the
		// following dword. Upstream treats it as length 1 and then decodes the payload as
		// instructions, which corrupts every subsequent opcode and immediate in the shader.
		if (opcode == D3D10_SB_OPCODE_CUSTOMDATA)
		{
			if (offset + 1 >= size_u32)
				break;
			const uint32_t custom_len = code_u32[offset + 1];
			if (custom_len < 2)
				break; // malformed; refuse to spin
			offset += custom_len;
			continue;
		}

		len = (len == 0) ? 1 : len;
		if (offset + len > size_u32)
			break;

		switch (opcode)
		{
		case D3D10_SB_OPCODE_MIN:
		case D3D10_SB_OPCODE_MAX:
			min_max_count++;
			break;
		case D3D10_1_SB_OPCODE_GATHER4:
		case D3D11_SB_OPCODE_GATHER4_PO_C:
		case D3D11_SB_OPCODE_GATHER4_PO:
		case D3D11_SB_OPCODE_GATHER4_C:
			has_gather4 = true;
			break;
		case D3D11_SB_OPCODE_STORE_STRUCTURED:
		case D3D11_SB_OPCODE_LD_STRUCTURED:
			has_lds_op = true;
			break;
		default:
			break;
		}

		// Scan for immediate constants in this instruction, starting after the opcode token(s).
		size_t operand_offset = DECODE_IS_D3D10_SB_OPCODE_EXTENDED(token) ? 2 : 1;

		for (size_t k = operand_offset; k < len; ++k)
		{
			const uint32_t operand_token = code_u32[offset + k];

			if (DECODE_D3D10_SB_OPERAND_TYPE(operand_token) != D3D10_SB_OPERAND_TYPE_IMMEDIATE32)
				continue;

			// Skip any extended operand tokens.
			size_t val_idx = k + 1;
			uint32_t curr_op_tok = operand_token;
			while (DECODE_IS_D3D10_SB_OPERAND_EXTENDED(curr_op_tok) && (val_idx < len))
			{
				curr_op_tok = code_u32[offset + val_idx];
				val_idx++;
			}

			const uint32_t num_comps_enum = DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(operand_token);
			int num_comps = 0;
			if (num_comps_enum == D3D10_SB_OPERAND_1_COMPONENT)
				num_comps = 1;
			else if (num_comps_enum == D3D10_SB_OPERAND_4_COMPONENT)
				num_comps = 4;

			if (num_comps > 0 && (val_idx + static_cast<size_t>(num_comps) <= len))
			{
				float values[4] = {};
				std::memcpy(values, &code_u32[offset + val_idx], sizeof(float) * static_cast<size_t>(num_comps));

				for (int i = 0; i < num_comps; ++i)
				{
					if (values[i] == 1.5f)
						has_1_5 = true;
					if (values[i] == 2.5f)
						has_2_5 = true;
				}

				if (num_comps == 4)
				{
					// YCoCg matrix rows
					if (values[0] == 1.0f && values[1] == 1.0f && values[2] == 2.0f)
						has_ycocg = true;
					if (values[0] == -1.0f && values[1] == -1.0f && values[2] == 2.0f)
						has_ycocg = true;
				}

				k = val_idx + static_cast<size_t>(num_comps) - 1;
			}
		}

		offset += len;
	}

	float score = 0.0f;
	if (min_max_count > 15) score += 50.0f;
	if (has_gather4)        score += 20.0f;
	if (has_lds_op)         score += 20.0f;
	if (has_1_5 && has_2_5) score += 40.0f;
	if (has_ycocg)          score += 40.0f;
	return score;
}

// -------------------------------------------------------------------------------------------
// IsUE4TAACandidate (shader_detect.hpp:310-495)
//
// Gate A  census of >= 4 float-returning Texture2D declarations, <= 1 Texture3D
// Gate B  the 4.00801611f immediate (1/(0.499*0.5), DecodeVelocityFromTexture in Common.ush)
// Gate C  reject well-formed loop pairs
// Gate D  confidence >= 60
// -------------------------------------------------------------------------------------------
inline bool IsUE4TAACandidate(const uint8_t *code, size_t size, TAAShaderInfo &info)
{
	const uint32_t *code_u32 = reinterpret_cast<const uint32_t *>(code);
	const size_t size_u32 = size / sizeof(uint32_t);

	// ------------------------------------------------------------------ Gate A: texture census
	size_t offset = 0;
	size_t instruction_count = 0;
	bool found_non_texture_declaration = false;
	int32_t detected_2d = 0, detected_3d = 0, output_count = 0;
	int32_t max_texture_register = -1;
	uint64_t declared_srv_mask = 0;
	int32_t max_uav_register = -1;
	uint64_t declared_uav_mask = 0;

	while (offset < size_u32 && !found_non_texture_declaration)
	{
		if (instruction_count > 16 && detected_2d == 0 && detected_3d == 0)
			return false; // too far without any texture declarations

		const uint32_t token = code_u32[offset];
		const uint32_t opcode = DECODE_D3D10_SB_OPCODE_TYPE(token);
		uint32_t len = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);
		const uint32_t resource_type = DECODE_D3D10_SB_RESOURCE_DIMENSION(token);
		len = (len == 0) ? 1 : len;

		if (offset + len > size_u32)
			break;

		if (opcode == D3D10_SB_OPCODE_DCL_RESOURCE)
		{
			// code_u32[offset+1] operand token
			// code_u32[offset+2] register index (assumed immediate32)
			// code_u32[offset+3] resource return type token
			if (offset + 4 > size_u32)
				break;

			// FIX-5: that layout is only valid for a 1D-indexed resource operand (SM 5.0).
			// SM 5.1 makes it 3D-indexed ([lo][hi][space]) and the return type moves to +5.
			if (DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(code_u32[offset + 1]) != D3D10_SB_OPERAND_INDEX_1D)
			{
				info.dcl_resource_index_dim_ok = false;
			}
			else
			{
				const uint32_t resource_return_type_token = code_u32[offset + 3];
				const uint32_t register_index = code_u32[offset + 2];

				const bool all_float_return =
					DECODE_D3D10_SB_RESOURCE_RETURN_TYPE(resource_return_type_token, D3D10_SB_4_COMPONENT_X) == D3D10_SB_RETURN_TYPE_FLOAT &&
					DECODE_D3D10_SB_RESOURCE_RETURN_TYPE(resource_return_type_token, D3D10_SB_4_COMPONENT_Y) == D3D10_SB_RETURN_TYPE_FLOAT &&
					DECODE_D3D10_SB_RESOURCE_RETURN_TYPE(resource_return_type_token, D3D10_SB_4_COMPONENT_Z) == D3D10_SB_RETURN_TYPE_FLOAT &&
					DECODE_D3D10_SB_RESOURCE_RETURN_TYPE(resource_return_type_token, D3D10_SB_4_COMPONENT_W) == D3D10_SB_RETURN_TYPE_FLOAT;

				// Sanity bound: UE4 caps at MAX_SRVS (64). Anything wilder means we mis-parsed.
				if (register_index < 1024u)
					max_texture_register = std::max<int32_t>(max_texture_register, static_cast<int32_t>(register_index));

				// FIX-9: the EXACT set of declared t-registers, not just the maximum. The live
				// join uses this to decide which heap slots were written for this draw and are
				// therefore safe to dereference. A shader declaring t0,t1,t2,t14 leaves t3..t13
				// holding stale descriptors from unrelated earlier draws that shared the heap
				// block; those must be reported but never resolved. See resolve_bound_srvs.
				if (register_index < 64u)
					declared_srv_mask |= (1ull << register_index);

				// NOTE: the return-type token encodes the HLSL return type, NOT the DXGI view
				// format. Texture2D<float2> Velocity compiles to (float,float,float,float)
				// exactly like a colour texture, so the census can never identify velocity by
				// itself. That is precisely why the register->resource mapping has to come from
				// the live descriptor shadow.
				if (resource_type == D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D && all_float_return)
					detected_2d++;
				else if (resource_type == D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D && all_float_return)
					detected_3d++;
			}

			offset += len;
		}
		else if (opcode == D3D10_SB_OPCODE_DCL_OUTPUT || opcode == D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED)
		{
			output_count++;

			// DLSS-NR ADDITION - the u-register census.
			//
			// dcl_unordered_access_view_typed has the SAME token layout as dcl_resource:
			//   [0] opcode token (resource dimension in bits 11..15, UAV flags above that)
			//   [1] operand token   (1D-indexed on SM 5.0)
			//   [2] register index  (immediate32)
			//   [3] resource return type token
			// so the SM 5.1 index-dimension guard from dcl_resource applies verbatim: on SM 5.1
			// the operand is 3D-indexed and [2] becomes a LOWER BOUND, not the register.
			//
			// This does not change any existing gate. output_count is incremented exactly as
			// before, whether or not the operand parses.
			if (opcode == D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED && offset + 4 <= size_u32)
			{
				if (DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(code_u32[offset + 1]) != D3D10_SB_OPERAND_INDEX_1D)
				{
					info.dcl_uav_index_dim_ok = false;
				}
				else
				{
					const uint32_t uav_register_index = code_u32[offset + 2];
					if (uav_register_index < 1024u)
						max_uav_register = std::max<int32_t>(max_uav_register, static_cast<int32_t>(uav_register_index));
					if (uav_register_index < 64u)
						declared_uav_mask |= (1ull << uav_register_index);
				}
			}

			offset += len;
		}
		else if ((opcode >= D3D11_SB_OPCODE_DCL_STREAM && opcode <= D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED) ||
		         (opcode >= D3D10_SB_OPCODE_DCL_RESOURCE && opcode <= D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS) ||
		         opcode == D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT)
		{
			offset += len;
		}
		else
		{
			found_non_texture_declaration = true;
		}

		// FIX-8: upstream has NO increment here, so its `instruction_count > 16` bail-out above
		// is unreachable and the census always runs to the first non-declaration opcode. An
		// increment was added during the port, which ACTIVATED that bail-out and made the port
		// stricter than the algorithm it claims to reproduce: fxc emits dcl_globalFlags, then
		// every dcl_constantbuffer, then every dcl_sampler BEFORE the first dcl_resource, so a
		// UE4 TAA shader with 17 or more leading declarations would be rejected with a census of
		// zero and binned as an ordinary census failure - silently losing the one shader this
		// diagnostic exists to find. Removed: the counter stays declared and permanently zero
		// exactly as upstream leaves it, so the guard is inert here too. The loop is still
		// bounded - offset advances by at least 1 on every iteration.
	}

	info.detected_2d_texture_float_count = detected_2d;
	info.detected_3d_texture_float_count = detected_3d;
	info.output_count = output_count;
	info.max_texture_register = max_texture_register;
	info.declared_srv_register_mask = declared_srv_mask;
	info.declared_uav_register_mask = declared_uav_mask;
	info.max_uav_register = max_uav_register;
	info.has_multiple_render_targets = (output_count > 1);

	if (detected_2d < 4 || detected_3d > 1)
		return false;

	// -------------------------------------------------- Gate B: the 4.00801611f immediate
	//
	// float2 DecodeVelocityFromTexture(float2 In)
	// { return (In - (32767.0f / 65535.0f)) / (0.499f * 0.5f); }
	//
	// compiles to e.g.
	//   mul r5.yz, r5.yyzy, l(0.000000, 4.008016, 4.008016, 0.000000)
	//   mad r1.yz, r1.zzyz, l(0.000000, 4.008016, 4.008016, 0.000000), l(...)
	//
	// Three swizzle placements are matched: adjacent (.xy/.yz), one gap (.xz), two gaps (.xw).
	{
		word_t mul_1; mul_1.u = kVelocityDecodeScaleBits;   // 4.00801611f == 0x408041AB
		word_t mul_2; mul_2.f = 0.000000f;

		const uint8_t *a = mul_1.b;
		const uint8_t *z = mul_2.b;

		const uint8_t pat_a[8]  = { a[0],a[1],a[2],a[3], a[0],a[1],a[2],a[3] };
		const uint8_t pat_b[12] = { a[0],a[1],a[2],a[3], z[0],z[1],z[2],z[3], a[0],a[1],a[2],a[3] };
		const uint8_t pat_c[16] = { a[0],a[1],a[2],a[3], z[0],z[1],z[2],z[3], z[0],z[1],z[2],z[3], a[0],a[1],a[2],a[3] };

		if (CountPattern(code, size, pat_a, sizeof(pat_a), true) != 0)
		{
			info.found_velocity_constant = true;
			info.velocity_constant_pattern = 0;
		}
		else if (CountPattern(code, size, pat_b, sizeof(pat_b), true) != 0)
		{
			info.found_velocity_constant = true;
			info.velocity_constant_pattern = 1;
		}
		else if (CountPattern(code, size, pat_c, sizeof(pat_c), true) != 0)
		{
			info.found_velocity_constant = true;
			info.velocity_constant_pattern = 2;
		}

		if (!info.found_velocity_constant)
			return false;
	}

	// ------------------------------------- Gate B', the DECODE BIAS: MEASURED, NEVER GATED ON
	//
	// Gate B above matches only the SCALE 4.00801611f. The other half of
	// DecodeVelocityFromTexture is the bias, and it is NOT 0.5 - it is 32767/65535, folded by the
	// compiler into a MAD immediate (32767/65535) * InvDiv = 2.00397754f (0x4000412B), which shows
	// up NEGATED (0xC000412B) in the `mad` form UE's decode compiles to.
	//
	// This is what turns "STRAY's velocity decode is stock UE 4.27" from an inference into a
	// measurement, and mvec_decode.hpp's entire constant set hangs on it. It is scanned here and
	// printed on the existing Gate B detail line; it deliberately does NOT reject anything, so
	// this block cannot change which shader is identified. Absence is a loud warning, not a
	// refusal - the decode could legitimately have been spelled another way by a different
	// compiler version, and Gate B plus the SRV class quorum are what actually identify the pass.
	{
		word_t neg_bias; neg_bias.u = kVelocityDecodeNegBiasBits;   // 0xC000412B, bytes 2B 41 00 C0
		word_t pos_bias; pos_bias.u = kVelocityDecodeBiasBits;      // 0x4000412B

		if (CountPattern(code, size, neg_bias.b, 4, true) != 0)
		{
			info.found_velocity_bias = true;
			info.velocity_bias_form  = 1;
		}
		else if (CountPattern(code, size, pos_bias.b, 4, true) != 0)
		{
			info.found_velocity_bias = true;
			info.velocity_bias_form  = 0;
		}
	}

	// ------------------------------------------------------------------ Gate C: loop rejection
	//
	// Upstream semantics preserved exactly: reject ONLY when both counts are equal AND non-zero,
	// i.e. well-formed loop pairs. Zero/zero falls through and is accepted. An unbalanced count
	// (a stray 0x01000030 inside immediate data) is deliberately tolerated.
	{
		word_t loop_tok;    loop_tok.u    = kTokenLoop;
		word_t endloop_tok; endloop_tok.u = kTokenEndloop;

		const size_t loop_hits    = CountPattern(code, size, loop_tok.b, 4);
		const size_t endloop_hits = CountPattern(code, size, endloop_tok.b, 4);

		if (loop_hits != 0 || endloop_hits != 0)
		{
			if (loop_hits == endloop_hits)
			{
				info.loops_balanced_nonzero = true;
				return false;
			}
		}
	}

	// -------------------------------------------------------------- Gate D: confidence threshold
	// Luma's threshold is 60, calibrated on the UE4 titles it supports. Stray's TAA compute
	// shader scores 50.0 while carrying the velocity-decode constant and an 8-texture census,
	// so 60 rejects the one shader we are actually looking for. The velocity constant is the
	// discriminating gate; the score is a tiebreak, so accept lower here and let the SRV dump
	// settle it. Raise this again if it starts admitting unrelated shaders.
	info.confidence = GetTAAShaderConfidence(code, size);
	if (info.confidence < 40.0f)
		return false;

	return true;
}

// -------------------------------------------------------------------------------------------
// FindShaderInfo (shader_detect.hpp:497-610)
//
// Locates ClipToPrevClip inside the View uniform buffer:
//   1. largest dcl_constant_buffer  -> the View UB's bN slot
//   2. byte-scan for [cb operand token][that slot] with the .xywx swizzle (fallback .xxyw) -
//      UE4's mul_float4x4 reprojection expands to four loads of consecutive cb elements all
//      carrying that unusual swizzle, which almost nothing else in the shader does
//   3. walk the sorted unique element indices BACKWARDS for the first window of four
//      consecutive rows (the matrix sits near the end of the View UB)
// -------------------------------------------------------------------------------------------
inline bool FindShaderInfo(const uint8_t *code, size_t size, TAAShaderInfo &info)
{
	const uint32_t *code_u32 = reinterpret_cast<const uint32_t *>(code);
	const size_t size_u32 = size / sizeof(uint32_t);

	const size_t decl = FindLargestCBufferDeclaration(code_u32, size_u32, &info.dcl_cbuffer_index_dim_ok);
	if (decl == SIZE_MAX || decl + 4 > size_u32)
		return false;

	word_t cbuffer_operand_register;
	cbuffer_operand_register.u = code_u32[decl + 2];

	info.global_buffer_register_index = static_cast<int32_t>(cbuffer_operand_register.u);
	info.declared_cbuffer_size = code_u32[decl + 3];

	// Search pattern is [operand token][cb slot]; the element index is the third dword at the
	// hit. That holds for both index representations - for IMMEDIATE32_PLUS_RELATIVE the
	// immediate dword still precedes the relative operand.
	std::vector<size_t> hits;

	const auto scan = [&](uint32_t operand_token) {
		word_t t; t.u = operand_token;
		uint8_t pattern[8] = {
			t.b[0], t.b[1], t.b[2], t.b[3],
			cbuffer_operand_register.b[0], cbuffer_operand_register.b[1],
			cbuffer_operand_register.b[2], cbuffer_operand_register.b[3]
		};
		CollectPattern(code, size, pattern, sizeof(pattern), hits);
	};

	scan(kTokenCbXywxImm);
	scan(kTokenCbXywxImmRel);

	if (hits.size() < 4) // try .xxyw instead of .xywx
	{
		hits.clear();
		scan(kTokenCbXxywImm);
		scan(kTokenCbXxywImmRel);

		if (hits.size() < 4)
			return false;
	}

	std::set<uint32_t> indices;
	for (const size_t hit_offset : hits)
	{
		if (hit_offset + 12 > size)
			continue; // need the third dword at the hit
		indices.insert(ReadU32(code + hit_offset + 8));
	}

	if (indices.size() < 4)
		return false;

	const std::vector<uint32_t> index_array(indices.begin(), indices.end()); // std::set is sorted

	// FIX-2/FIX-4: upstream is
	//     int32_t best_start = UINT32_MAX;
	//     for (size_t i = index_array.size() - 1; i - 3 >= 0; i--)
	// `i - 3 >= 0` is always true for size_t, so the loop only exits via the break. With no
	// matching window it walks i down to 2 and then evaluates index_array[SIZE_MAX] forever.
	int32_t best_start = -1;
	for (size_t i = index_array.size(); i-- >= 4; )
	{
		if (index_array[i] - index_array[i - 3] == 3)
		{
			best_start = static_cast<int32_t>(index_array[i - 3]);
			break;
		}
	}

	info.clip_to_prev_clip_start_index = best_start;
	info.found_shader_info = true;
	return true;
}

} // namespace probe
