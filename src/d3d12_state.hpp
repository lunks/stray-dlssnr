// d3d12_state.hpp - save and restore the D3D12 command-list binding state that
// NVSDK_NGX_D3D12_CreateFeature / EvaluateFeature destroy.
//
// WHY THIS IS THE HIGHEST-RISK FILE IN THE ADD-ON
//
//   NVIDIA states the problem twice, in its own words:
//
//     DLSS Programming Guide 310.6.0, section 5.4 "Feature Evaluation", page 52:
//       "IMPORTANT: NGX modifies the Vulkan and D3D12 command list states. The calling process
//        must save and restore its own Vulkan or D3D12 state before and after making the NGX
//        evaluate feature calls."
//
//     NGX Programming Guide, section 3.2 "Initializing NGX":
//       "For features that use DirectX, the NGX API preserves the state of the immediate D3D11
//        context, however, that is not the case with D3D12 command lists."
//
//   and UE 4.27 will NOT repair the damage. FD3D12StateCacheBase::ApplyState is purely
//   dirty-flag driven (D3D12StateCache.cpp:353-407): after the evaluate,
//   Compute.bNeedSetRootSignature is still false, CurrentPipelineStateObject still equals the PSO
//   UE believes is bound, and FD3D12DescriptorCache::SetDescriptorHeaps compares against its own
//   pPreviousViewHeap - which NGX did not touch - so it issues no SetDescriptorHeaps and never
//   calls DirtyViewDescriptorTables(). The next compute dispatch therefore runs UE's root
//   arguments against NGX's root signature, out of a heap that is no longer bound, quite possibly
//   with NGX's PSO still selected. That is a device removal or a silently wrong shader, and it
//   persists until UE opens a fresh command list.
//
//   There is no way to ask D3D12 what is bound: ID3D12GraphicsCommandList has 51 methods and not
//   one getter. So the only options are "shadow every state-setting call and replay it" or
//   "do not restore". This file is the first.
//
// THE MODEL IS NVIDIA'S OWN
//   Streamline's ProgrammingGuideManualHooking.md, section 7.1, publishes exactly this routine
//   (restorePipeline) as what the host must reimplement when it hooks manually, and the shadow it
//   replays from (sl_d3d12CommandList.h:165-180) is heaps + compute root signature + compute root
//   tables/CBV/SRV/UAV/32-bit-constants + PSO + RT state object. This file mirrors that set, with
//   two deliberate differences:
//
//     * the GRAPHICS root signature and its arguments are shadowed and replayed too. The
//       load-bearing reason is NOT "nvngx_dlssnr.dll might dirty graphics state" - that is only a
//       weak prior against Streamline's empirical one. It is that WE issue SetDescriptorHeaps
//       ourselves in step 1 of restore_state, and D3D12's rule (quoted below) is that a heap
//       change makes every descriptor table undefined on BOTH pipes. So the graphics tables have
//       to be re-set even on the assumption that NGX only ever touched compute.
//       (restore_graphics_root=0 turns it off; that leaves the graphics tables invalidated by our
//       own heap re-bind, so it is a diagnostic knob, not a safe default.)
//
// WHAT IS DELIBERATELY *NOT* RESTORED, AND WHY THAT IS NOT AN OMISSION
//   Render targets, viewports, scissor rects, primitive topology, blend factor, stencil reference
//   and the vertex/index buffer bindings are neither shadowed nor replayed. That is consistent,
//   not a gap, because the restore set is derived from exactly two things:
//     (a) what NGX is known to set - compute root signature, compute root arguments, descriptor
//         heaps, PSO (NVIDIA's own restorePipeline covers precisely this set), and
//     (b) what OUR OWN restore invalidates - a SetDescriptorHeaps invalidates descriptor tables,
//         and a SetRootSignature invalidates that pipe's root parameters. Nothing more.
//   Neither (a) nor (b) touches the raster pipeline configuration: SetDescriptorHeaps does not
//   invalidate an OMSetRenderTargets, an RSSetViewports, or an IASetVertexBuffers, and the NGX
//   evaluate records compute work only. If that ever stops being true the correct response is to
//   shadow and replay those states as well AND to mark the plan incomplete when they were never
//   observed - not to replay half of them.
//
// ORDER IS NOT NEGOTIABLE (both rules are D3D12's, and UE quotes them at itself):
//     D3D12StateCache.cpp:275-279  "Descriptor table state is undefined ... after descriptor
//                                   heaps are changed on a command list."
//     D3D12StateCache.cpp:378-386  "After setting a root signature, all root parameters are
//                                   undefined and must be set again."
//   => heaps first, then each pipe's root signature, then that pipe's arguments, then the PSO.
//
// EVERYTHING HERE TALKS TO THE RAW ID3D12GraphicsCommandList, never to ReShade's command_list.
//   ReShade's push_descriptors does NOT map back to SetComputeRootConstantBufferView - it
//   allocates a descriptor in ReShade's own heap, binds ReShade's heaps, and issues a
//   DESCRIPTOR TABLE instead, which is a root-signature mismatch and would leave the wrong heaps
//   bound. And ReShade's bind_descriptor_tables suppresses its own SetDescriptorHeaps whenever
//   its cached _current_descriptor_heaps already match - which they do, because NGX's heap change
//   went straight to the raw list and ReShade never saw it.
//
// ABI NOTE. Calling D3D12 COM methods from a mingw-w64 build is safe here, including the ones
// that return an aggregate: mingw defines WIDL_EXPLICIT_AGGREGATE_RETURNS for GCC C++
// (_mingw_mac.h:356-361), which turns GetGPUDescriptorHandleForHeapStart / GetDesc into an
// explicit out-parameter form - RCX = this, RDX = &ret - which is byte-for-byte the convention
// MSVC uses for a member function returning a UDT. That is the same fix msvc_abi.hpp applies by
// hand to ReShade's three by-value-returning virtuals, except that here the headers already do
// it. Do not #undef WIDL_EXPLICIT_AGGREGATE_RETURNS.

#pragma once

#include "reshade_compat.hpp"
#include "descriptor_shadow.hpp"

#include <d3d12.h>

#include <cstdint>
#include <vector>

namespace probe {

// The plan is built while the ReShade shadow is readable, and executed after NGX returns. It
// holds RAW D3D12 pointers, deliberately without AddRef: every object in it is bound on a command
// list that is still recording, so it is alive for the whole window, and taking references from a
// per-dispatch path would be both pointless and a source of release-ordering bugs.
struct pipe_restore
{
	ID3D12RootSignature *root_signature = nullptr;

	struct arg
	{
		uint32_t      param = 0;
		root_arg_kind kind  = root_arg_kind::none;
		uint64_t      gpu   = 0; // GPU descriptor handle (table) or GPU virtual address (cbv/srv/uav)
	};

	std::vector<arg> args;
	root_constants   consts[kMaxRootConstParams];
	uint32_t         const_count = 0;
};

struct restore_plan
{
	// False means "do not run the pass at all this frame". A partial restore is worse than not
	// injecting: it leaves the game running against state nobody can account for.
	bool complete = false;

	// Why not, when !complete. Logged once per distinct reason by the caller.
	const char *incomplete_reason = nullptr;

	ID3D12DescriptorHeap *heaps[2] = { nullptr, nullptr }; // [0] CBV_SRV_UAV, [1] SAMPLER
	uint32_t              heap_count = 0;

	pipe_restore cmp;
	pipe_restore gfx;

	ID3D12PipelineState *pso = nullptr;
	// A ray-tracing state object, if the application last called SetPipelineState1.
	//
	// DELIBERATELY UNTYPED AND DELIBERATELY NOT REPLAYED. Replaying it needs
	// ID3D12GraphicsCommandList4::SetPipelineState1, and the mingw-w64 d3d12.h this add-on is
	// cross-built against stops at ID3D12GraphicsCommandList2 and declares no ID3D12StateObject
	// at all. Rather than QueryInterface a GUID this build cannot even name, a command list whose
	// last pipeline binding was a state object makes the plan INCOMPLETE, and the pass then does
	// not run on that list. UE 4.27 binds no DXR state objects around the TAA pass, so this is
	// expected never to fire in STRAY - but a silently unreplayed pipeline binding is exactly the
	// failure that looks like a game bug.
	void                *state_object = nullptr;
};

// ------------------------------------------------------------------------------------------
// One-time soundness check on the single assumption the table replay rests on:
//   reshade::api::descriptor_table::handle IS the raw D3D12_GPU_DESCRIPTOR_HANDLE::ptr.
//
// If that is false, SetComputeRootDescriptorTable would be handed a number that is not a GPU
// descriptor handle at all, and the game would render garbage or hang - the exact class of
// failure that must never be guessed at silently. So it is checked against the heap the table
// actually lives in, arithmetically:
//
//     table.handle == heap->GetGPUDescriptorHandleForHeapStart().ptr
//                     + heap_offset * device->GetDescriptorHandleIncrementSize(type)
//
// Both right-hand-side calls are ABI-safe from mingw (see the header note above).
//
// Returns: 1 = verified, 0 = could not be checked (no usable table this time, try again),
//         -1 = MISMATCH, the assumption is false and the pass must stay off.
// ------------------------------------------------------------------------------------------
inline int verify_table_handle_identity(ID3D12Device *d3d12_device,
                                        const std::vector<bound_table_info> &tables,
                                        uint64_t *out_expected, uint64_t *out_actual)
{
	if (d3d12_device == nullptr)
		return 0;

	for (const bound_table_info &t : tables)
	{
		if (t.table_handle == 0 || t.heap == 0)
			continue;

		auto *heap = reinterpret_cast<ID3D12DescriptorHeap *>(t.heap);
		const D3D12_DESCRIPTOR_HEAP_TYPE type = t.is_sampler
			? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
			: D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

		const UINT inc = d3d12_device->GetDescriptorHandleIncrementSize(type);
		if (inc == 0)
			continue;

		const D3D12_GPU_DESCRIPTOR_HANDLE start = heap->GetGPUDescriptorHandleForHeapStart();
		if (start.ptr == 0)
			continue; // a non-shader-visible heap has no GPU handle; nothing to compare

		const uint64_t expected = start.ptr + static_cast<uint64_t>(t.heap_offset) * inc;
		if (out_expected != nullptr) *out_expected = expected;
		if (out_actual   != nullptr) *out_actual   = t.table_handle;

		return (expected == t.table_handle) ? 1 : -1;
	}

	return 0;
}

// ------------------------------------------------------------------------------------------
// Second one-time check, on the OTHER identity the heap replay rests on: that the
// reshade::api::descriptor_heap handle is the application's ORIGINAL ID3D12DescriptorHeap and not
// ReShade's own D3D12DescriptorHeap wrapper.
//
// It matters because SetDescriptorHeaps is issued on the RAW command list, i.e. straight into
// vkd3d-proton, which would not accept a foreign implementation of the interface. And the
// arithmetic check above does NOT cover it: a wrapper forwards GetGPUDescriptorHandleForHeapStart
// and would produce exactly the same number.
//
// ID3D12DeviceChild::GetDevice on the real heap returns the real ID3D12Device, which is what
// reshade::api::device::get_native() hands back. A wrapper would return ReShade's own device
// object instead, so a mismatch is a reliable "this is not the raw heap" signal.
//
// Returns: 1 = raw heap confirmed, 0 = could not tell, -1 = NOT the raw heap.
//
// LINKAGE NOTE. The IID is spelled out here rather than written as IID_ID3D12Device. mingw-w64's
// d3d12.h declares that name with DEFINE_GUID, which without INITGUID is a DECLARATION only, and
// no import library in the mingw-w64 10.0.0 drop actually defines it - libdxguid.a, libd3d12.a and
// libuuid.a were all checked and none carries the symbol, so referencing it fails at link time
// with "undefined reference to `IID_ID3D12Device'". __uuidof would work on both toolchains via
// __CRT_UUID_DECL, but it is the very construct reshade_compat.hpp exists to work around on this
// compiler, so a plain constant is used instead: it is header-only, needs no library, and is
// identical on MSVC and mingw. The value is from d3d12.h line 6302 and is fixed by the API.
inline int verify_heap_is_native(ID3D12Device *d3d12_device, ID3D12DescriptorHeap *heap)
{
	if (d3d12_device == nullptr || heap == nullptr)
		return 0;

	static const GUID kIidD3D12Device = {
		0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };

	ID3D12Device *owner = nullptr;
	if (FAILED(heap->GetDevice(kIidD3D12Device, reinterpret_cast<void **>(&owner))) || owner == nullptr)
		return 0;

	const bool same = (owner == d3d12_device);
	owner->Release();
	return same ? 1 : -1;
}

// ------------------------------------------------------------------------------------------
// Capture
// ------------------------------------------------------------------------------------------
// EVERY DROP IS FATAL TO THE PLAN.
//
// restore_state issues SetCompute/GraphicsRootSignature, and D3D12's rule is that this makes ALL
// root parameters undefined (the file header quotes UE quoting it at itself). So a root argument
// the shadow SAW but that this function cannot turn into a replayable value is not "one binding we
// skip" - it is a parameter left undefined for every subsequent dispatch on this command list, and
// UE 4.27's dirty-flag state cache will not re-set it because it believes it is already clean. The
// measured TAA pass has root CBVs at param 3 (the View uniform buffer) and param 4, which is
// exactly the class of parameter at risk.
//
// So a drop sets *reason and returns false, and capture_state turns that into
// plan.incomplete_reason - which makes the caller skip the pass entirely. That is the behaviour
// restore_plan::complete is documented to have ("a partial restore is worse than not injecting");
// the `continue`s this replaced quietly broke it while still reporting plan.complete == true.
//
// RESIDUAL GAP, deliberately NOT converted into a refusal. If ReShade never emitted the
// push_descriptors event at all - its SetComputeRootConstantBufferView wrapper returns early when
// resolve_gpu_address cannot find the address in its registered-buffer map - the parameter is
// invisible here: arg_kind == none and is_root_descriptor == false. That is indistinguishable from
// a parameter the application legitimately never set, which needs no replay because it was already
// undefined before we touched anything. Treating "arg_kind == none" as fatal would refuse to run
// on any root signature with an unused parameter. What IS caught is the narrower, unambiguous
// case: the event arrived, is_root_descriptor was set, and no address came out of it.
inline bool capture_pipe(device *dev, device_shadow &sh, const pipe_bindings &b,
                         const std::vector<bound_table_info> &tables, pipe_restore &out,
                         const char **reason)
{
	const auto fail = [reason](const char *why) {
		if (reason != nullptr && *reason == nullptr)
			*reason = why;
		return false;
	};

	out.root_signature = reinterpret_cast<ID3D12RootSignature *>(b.layout.handle);
	out.args.clear();
	out.const_count = 0;

	if (out.root_signature == nullptr)
		return true;   // nothing bound on this pipe; capture_state decides whether that is fatal

	for (size_t p = 0; p < b.arg_kind.size(); ++p)
	{
		const root_arg_kind k = b.arg_kind[p];

		if (k == root_arg_kind::none)
		{
			// A root descriptor WAS observed at this parameter but no address survived. See the
			// residual-gap note above for why only this shape is fatal.
			if (p < b.is_root_descriptor.size() && b.is_root_descriptor[p])
				return fail("a root descriptor was observed at a root parameter but no GPU address "
				            "could be recovered for it, so replaying the root signature would leave "
				            "that parameter undefined");
			continue;
		}
		if (k == root_arg_kind::constants)
			continue;   // replayed from the consts array below

		pipe_restore::arg a;
		a.param = static_cast<uint32_t>(p);
		a.kind  = k;

		if (k == root_arg_kind::table)
		{
			// Prefer the census entry, which was produced from the same shadow and carries the
			// heap; fall back to the raw handle. They are the same number.
			a.gpu = b.tables[p].handle;
			for (const bound_table_info &t : tables)
			{
				if (t.param == a.param) { a.gpu = t.table_handle; break; }
			}
			if (a.gpu == 0)
				return fail("a root descriptor TABLE is recorded as bound at a root parameter but its "
				            "GPU descriptor handle is 0, so it cannot be replayed");
		}
		else if (k == root_arg_kind::cbv)
		{
			// The one address computed lazily. ReShade reports a root CBV as (resource, offset),
			// so the GPU virtual address needs an ID3D12Resource::GetGPUVirtualAddress call. That
			// is paid here, once per evaluate, rather than on every push_descriptors event - and
			// UE 4.27 issues one of those per draw, on every recording thread.
			if (p >= b.root_cbvs.size() || !b.root_cbvs[p].valid)
				return fail("a root CBV is recorded at a root parameter but the shadow holds no buffer "
				            "range for it, so it cannot be replayed");
			const buffer_range &br = b.root_cbvs[p].range;
			if (br.buffer.handle == 0)
			{
				// A NULL root CBV, faithfully. ReShade's resolve_gpu_address reports success with a
				// zero resource for BufferLocation == 0, so this is the application having bound
				// address 0, which D3D12 permits. Replay it as 0 rather than dropping it: dropping
				// leaves the parameter UNDEFINED, which is not the same thing as null.
				a.gpu = 0;
			}
			else
			{
				auto *res = reinterpret_cast<ID3D12Resource *>(br.buffer.handle);
				const D3D12_GPU_VIRTUAL_ADDRESS base = res->GetGPUVirtualAddress();
				if (base == 0)
					return fail("a root CBV names a resource whose GetGPUVirtualAddress returned 0 (not "
					            "a buffer, or no GPU address), so it cannot be replayed");
				a.gpu = static_cast<uint64_t>(base) + br.offset;
			}
		}
		else // srv / uav
		{
			if (p >= b.arg_gpu.size())
				return fail("a root SRV/UAV is recorded at a root parameter that is outside the "
				            "shadowed address array");
			// 0 is a legal null root SRV/UAV address and is replayed as such, for the same reason as
			// the null root CBV above.
			a.gpu = b.arg_gpu[p];
		}

		out.args.push_back(a);
	}

	if (b.const_count > kMaxRootConstParams)
		return fail("more root 32-bit-constant parameters are recorded than the shadow can hold, so "
		            "the replay would be missing bindings");
	out.const_count = b.const_count;
	for (uint32_t i = 0; i < out.const_count; ++i)
		out.consts[i] = b.consts[i];

	(void)dev;
	(void)sh;
	return true;
}

// Builds the plan. 'restore_graphics' mirrors the ini knob.
//
// The heaps are the fragile half: D3D12 has no getter and ReShade emits no event for
// SetDescriptorHeaps, so they are recovered from the tables that happen to be bound. If the
// CBV_SRV_UAV heap cannot be recovered the plan is incomplete and the caller must skip the pass -
// restoring root tables into a heap that is not bound is strictly worse than doing nothing. A
// missing SAMPLER heap is treated the same way, because SetDescriptorHeaps replaces the whole set:
// re-binding only the view heap would UNBIND the sampler heap, and UE would never notice.
inline restore_plan capture_state(device *dev, device_shadow &sh, cmd_shadow &cs, bool restore_graphics)
{
	restore_plan plan;

	std::vector<bound_table_info> cmp_tables;
	std::vector<bound_table_info> gfx_tables;
	collect_bound_tables(dev, sh, cs.cmp, cmp_tables);
	collect_bound_tables(dev, sh, cs.gfx, gfx_tables);

	// 'from_current_pipe' distinguishes the COMPUTE tables - which were bound immediately before
	// the dispatch we are standing on, so the heap they name is provably the one bound right now -
	// from the GRAPHICS tables, which may be arbitrarily old.
	const auto absorb = [&plan](const std::vector<bound_table_info> &tables, bool from_current_pipe) {
		for (const bound_table_info &t : tables)
		{
			if (t.heap == 0)
				continue;
			const uint32_t slot = t.is_sampler ? 1u : 0u;
			auto *h = reinterpret_cast<ID3D12DescriptorHeap *>(t.heap);
			if (plan.heaps[slot] == nullptr)
				plan.heaps[slot] = h;
			else if (plan.heaps[slot] != h)
			{
				// Two different heaps of the same type cannot be bound at once on D3D12, so the two
				// records cannot both be current. Which one is stale is not knowable from here, so the
				// plan is refused either way - but say which case it is, because they have different
				// causes: a compute/graphics disagreement is a mid-command-list SetDescriptorHeaps that
				// ReShade emits no event for (see the note below), while a disagreement inside one pipe
				// really would mean the shadow is broken.
				if (plan.incomplete_reason == nullptr)
					plan.incomplete_reason = from_current_pipe
						? "two different descriptor heaps of the same type are recorded as bound at once "
						  "on the compute pipe - the descriptor shadow is inconsistent"
						: "the graphics and compute shadows name different descriptor heaps of the same "
						  "type, so the application changed heaps mid-command-list and one of the two "
						  "records is stale";
			}
		}
	};

	// HEAPS COME FROM THE COMPUTE PIPE FIRST, AND FROM THE GRAPHICS PIPE ONLY IF WE ARE GOING TO
	// REPLAY THE GRAPHICS PIPE AT ALL.
	//
	// ReShade emits NO event for SetDescriptorHeaps (its wrapper unwraps the heaps into
	// _current_descriptor_heaps and invokes no add-on event), so the shadow's table records are
	// never invalidated when the application swaps heaps mid-command-list - which UE 4.27 does when
	// a heap rolls over. The compute tables were bound in the run-up to THIS dispatch, so the heap
	// they name is the one bound now. A graphics table may predate an intervening swap, and feeding
	// its heap to SetDescriptorHeaps on the raw list would bind a heap the game is no longer using -
	// after which every graphics descriptor table the game sets resolves against a heap its handles
	// were not allocated from. Taking it only when restore_graphics is set at least confines that
	// risk to the same opt-in knob that replays the (equally possibly stale) graphics tables.
	absorb(cmp_tables, true);
	if (restore_graphics)
		absorb(gfx_tables, false);

	const char *pipe_reason = nullptr;
	capture_pipe(dev, sh, cs.cmp, cmp_tables, plan.cmp, &pipe_reason);
	if (restore_graphics)
		capture_pipe(dev, sh, cs.gfx, gfx_tables, plan.gfx, &pipe_reason);

	plan.pso          = reinterpret_cast<ID3D12PipelineState *>(cs.pso.handle);
	plan.state_object = reinterpret_cast<void *>(cs.state_object.handle);

	// SetDescriptorHeaps takes at most one heap of each type, and UE 4.27 always sets both
	// together (FD3D12DescriptorCache::SetDescriptorHeaps: ppHeaps[] = { view, sampler }).
	plan.heap_count = 0;
	if (plan.heaps[0] != nullptr) plan.heap_count++;
	if (plan.heaps[1] != nullptr) plan.heap_count++;

	if (plan.incomplete_reason == nullptr && pipe_reason != nullptr)
		plan.incomplete_reason = pipe_reason;
	if (plan.incomplete_reason == nullptr && cs.cmp.consts_overflowed)
		plan.incomplete_reason = "the compute root 32-bit-constant shadow overflowed, so the "
		                         "replay would be missing bindings";
	if (plan.incomplete_reason == nullptr && restore_graphics && cs.gfx.consts_overflowed)
		plan.incomplete_reason = "the graphics root 32-bit-constant shadow overflowed, so the "
		                         "replay would be missing bindings";
	if (plan.incomplete_reason == nullptr && plan.heaps[0] == nullptr)
		plan.incomplete_reason = "the CBV_SRV_UAV descriptor heap could not be recovered from any "
		                         "bound descriptor table (the compute pipe is the only source "
		                         "unless restore_graphics_root=1)";
	if (plan.incomplete_reason == nullptr && plan.heaps[1] == nullptr)
		plan.incomplete_reason = "the SAMPLER descriptor heap could not be recovered from any "
		                         "bound descriptor table (re-binding only the view heap would "
		                         "unbind the sampler heap; the compute pipe is the only source "
		                         "unless restore_graphics_root=1)";
	if (plan.incomplete_reason == nullptr && plan.state_object != nullptr)
		plan.incomplete_reason = "a ray-tracing state object is the command list's current "
		                         "pipeline binding (SetPipelineState1), which this build cannot "
		                         "replay - see restore_plan::state_object";
	if (plan.incomplete_reason == nullptr && plan.cmp.root_signature == nullptr)
		plan.incomplete_reason = "no compute root signature is recorded on this command list";

	plan.complete = (plan.incomplete_reason == nullptr);
	return plan;
}

// ------------------------------------------------------------------------------------------
// Replay. Raw command list only. Order per the note at the top of this file.
// ------------------------------------------------------------------------------------------
// Root 32-bit constants are shadowed as a dense dword array plus a validity mask (see
// root_constants), so the replay emits ONE SetRoot32BitConstants call per contiguous run of dwords
// that were actually written. Replaying the whole array instead would invent values for dwords the
// application never set, and replaying a single window would drop every dword outside it.
//
// 'setter' is SetComputeRoot32BitConstants or SetGraphicsRoot32BitConstants; both have the same
// signature (RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues).
template <typename Setter>
inline void replay_root_constants(const pipe_restore &p, Setter setter)
{
	for (uint32_t i = 0; i < p.const_count; ++i)
	{
		const root_constants &rc = p.consts[i];
		uint32_t d = 0;
		while (d < kMaxRootConstDwords)
		{
			if ((rc.set_mask & (1ull << d)) == 0) { ++d; continue; }
			const uint32_t run_start = d;
			while (d < kMaxRootConstDwords && (rc.set_mask & (1ull << d)) != 0)
				++d;
			setter(rc.param, d - run_start, rc.values + run_start, run_start);
		}
	}
}

inline void replay_pipe_compute(ID3D12GraphicsCommandList *cl, const pipe_restore &p)
{
	if (p.root_signature == nullptr)
		return;

	cl->SetComputeRootSignature(p.root_signature);

	for (const pipe_restore::arg &a : p.args)
	{
		switch (a.kind)
		{
		case root_arg_kind::table:
		{
			D3D12_GPU_DESCRIPTOR_HANDLE h;
			h.ptr = a.gpu;
			cl->SetComputeRootDescriptorTable(a.param, h);
			break;
		}
		case root_arg_kind::cbv: cl->SetComputeRootConstantBufferView(a.param, a.gpu);  break;
		case root_arg_kind::srv: cl->SetComputeRootShaderResourceView(a.param, a.gpu);  break;
		case root_arg_kind::uav: cl->SetComputeRootUnorderedAccessView(a.param, a.gpu); break;
		default: break;
		}
	}

	replay_root_constants(p, [cl](uint32_t param, uint32_t n, const void *src, uint32_t off) {
		cl->SetComputeRoot32BitConstants(param, n, src, off);
	});
}

inline void replay_pipe_graphics(ID3D12GraphicsCommandList *cl, const pipe_restore &p)
{
	if (p.root_signature == nullptr)
		return;

	cl->SetGraphicsRootSignature(p.root_signature);

	for (const pipe_restore::arg &a : p.args)
	{
		switch (a.kind)
		{
		case root_arg_kind::table:
		{
			D3D12_GPU_DESCRIPTOR_HANDLE h;
			h.ptr = a.gpu;
			cl->SetGraphicsRootDescriptorTable(a.param, h);
			break;
		}
		case root_arg_kind::cbv: cl->SetGraphicsRootConstantBufferView(a.param, a.gpu);  break;
		case root_arg_kind::srv: cl->SetGraphicsRootShaderResourceView(a.param, a.gpu);  break;
		case root_arg_kind::uav: cl->SetGraphicsRootUnorderedAccessView(a.param, a.gpu); break;
		default: break;
		}
	}

	replay_root_constants(p, [cl](uint32_t param, uint32_t n, const void *src, uint32_t off) {
		cl->SetGraphicsRoot32BitConstants(param, n, src, off);
	});
}

inline void restore_state(ID3D12GraphicsCommandList *cl, const restore_plan &plan, bool restore_graphics)
{
	if (cl == nullptr || !plan.complete)
		return;

	// 1. Heaps. Unconditionally, even if we believe them unchanged: NGX definitely changed them,
	//    and this is the step every later table binding depends on.
	if (plan.heap_count != 0)
	{
		ID3D12DescriptorHeap *heaps[2] = {};
		UINT n = 0;
		if (plan.heaps[0] != nullptr) heaps[n++] = plan.heaps[0];
		if (plan.heaps[1] != nullptr) heaps[n++] = plan.heaps[1];
		cl->SetDescriptorHeaps(n, heaps);
	}

	// 2/3. Compute root signature, then every compute root argument.
	replay_pipe_compute(cl, plan.cmp);

	// 4. Graphics. The heap change above invalidated its tables too, so this is not optional
	//    just because NGX only dirties compute state.
	if (restore_graphics)
		replay_pipe_graphics(cl, plan.gfx);

	// 5. Pipeline state last. SetPipelineState and SetPipelineState1 are mutually exclusive; a
	//    plan whose binding was a state object never reaches here, because capture_state marked
	//    it incomplete (see restore_plan::state_object).
	if (plan.pso != nullptr)
		cl->SetPipelineState(plan.pso);
}

} // namespace probe
