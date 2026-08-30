// descriptor_shadow.hpp - read-only D3D12 descriptor resolution: "which resource is bound at
// SRV register tN for this draw/dispatch?"
//
// Merged from two reference implementations, BOTH OF WHICH CARRY THE SAME DEFECT:
//   reshade/examples/utils/descriptor_tracking.{hpp,cpp}   (BSD-3/MIT)
//   renodx/src/utils/{descriptor,pipeline_layout,state}.hpp (MIT)
//
// THE DEFECT. With root signature version 1.1/1.2 ReShade emits
// pipeline_layout_param_type::descriptor_table_with_flags, whose ranges live in a FUNCTION-LOCAL
// std::vector inside D3D12Device::invoke_create_and_init_pipeline_layout_event. Both references
// deep-copy only the plain 'descriptor_table' variant, so for the flags variant they retain a
// pointer into a dead stack frame. Additionally descriptor_range_with_flags (40 bytes on x64)
// has a larger stride than descriptor_range (28 bytes), and the derived-to-base pointer
// conversion is implicit and silent, so indexing element k lands 12*k bytes early.
//
// Both were measured on the host, not assumed:
//   sizeof(descriptor_range)=28  sizeof(descriptor_range_with_flags)=40
//   reading {1000,2000,3000} through a descriptor_range* yields {1000, 0, 1}
//
// It presents as RANDOM CORRUPTION, not a clean failure. This implementation handles both
// variants, normalising everything to the WIDE type so the stride is unambiguous by
// construction downstream, and logs which variant was seen on the first pipeline layout.
//
// STRAY DLSS-NR ADDITIONS (2026-08)
//   The probe version of this file was strictly read-only. This one is still read-only in the
//   sense that nothing here issues a graphics command, but it now records two extra things the
//   add-on needs in order to put the command list back the way NGX found it:
//
//     * root ARGUMENTS, not just root tables: root CBV / SRV / UAV GPU virtual addresses and
//       root 32-bit constants, per root parameter, per pipe (graphics and compute).
//     * the ray-tracing state object alongside the pipeline state, because
//       SetPipelineState and SetPipelineState1 are mutually exclusive on D3D12.
//
//   and it can resolve UAVs as well as SRVs (resolve_bound_uavs), which is what answers
//   "which UAV is the TAA output?".
//
//   The RS 1.1 / descriptor_table_with_flags handling below is UNCHANGED and must stay that way:
//   the whole restore is keyed off layout_param::is_table and layout_param::ranges, which are
//   only trustworthy because of the 40-byte-stride deep copy.

#pragma once

#include "reshade_compat.hpp"

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <new>

namespace probe {

using namespace reshade::api;

// Guard rails. UE4's global CBV_SRV_UAV heap is 500k descriptors (~8 MB of shadow at 16 B/slot),
// which is fine, but a bogus offset must never be allowed to trigger a multi-GB resize.
static constexpr uint32_t kMaxHeapSlots   = 1u << 20; // 1M descriptors
static constexpr uint32_t kMaxRootParams  = 256;
static constexpr uint32_t kMaxRangesParam = 4096;
// UE4 declares SRV ranges of MAX_SRVS (64) regardless of real usage on ResourceBindingTier 3
// (which vkd3d-proton always reports). Never walk more than this.
static constexpr uint32_t kMaxSrvWalk     = 64;
// UE 4.27 declares UAV ranges as MAX_UAVS (8) for the same ResourceBindingTier 3 reason.
static constexpr uint32_t kMaxUavWalk     = 16;

// -------------------------------------------------------------------------------------------
// Device-level shadow
// -------------------------------------------------------------------------------------------

struct heap_slot
{
	descriptor_type type = descriptor_type::sampler;
	resource_view   view = { 0 };
};

struct layout_param
{
	// The type AS REPORTED by ReShade, kept verbatim for the diagnostic log line.
	pipeline_layout_param_type reported_type = pipeline_layout_param_type::push_constants;
	bool is_table = false;
	// ALWAYS the wide variant, whatever the source was, so the stride is unambiguous.
	std::vector<descriptor_range_with_flags> ranges;
};

// GCC/mingw does not implement __declspec(uuid) or MSVC's __uuidof, so the templated
// create/get/destroy_private_data helpers in reshade_api_device.hpp are unusable here. We use
// the underlying virtual API, which takes an explicit 16-byte key, via the pd_* helpers at the
// bottom of this file. Those key bytes are arbitrary but must not collide with another add-on
// (RenoDX, Luma) loaded into the same process.
struct device_shadow
{
	std::shared_mutex mutex;
	std::unordered_map<uint64_t, std::vector<heap_slot>>    heaps;
	std::unordered_map<uint64_t, std::vector<layout_param>> layouts;

	bool     is_d3d12 = false;
	bool     logged_rs_variant = false;
	bool     saw_table_plain = false;
	bool     saw_table_with_flags = false;
	// Atomic because they are now bumped from the PREPARE phase, which deliberately runs with no
	// lock held (see prepare_descriptor_copy). Relaxed ordering: they are pure diagnostics.
	std::atomic<uint64_t> dropped_heap_growth{ 0 }; // offsets rejected by kMaxHeapSlots
	std::atomic<uint64_t> copies_missing_src { 0 }; // copy from a heap we never observed
};

// -------------------------------------------------------------------------------------------
// Command-list shadow
// -------------------------------------------------------------------------------------------

struct root_cbv
{
	bool         valid  = false;
	buffer_range range  = {};
};

// What is bound at one root parameter, for the purpose of REPLAYING it after NGX has thrown the
// command list's root state away. Deliberately a flat enum rather than the descriptor_type it
// came from: the only thing the replay needs to know is which of the five
// SetComputeRoot* entry points to call.
enum class root_arg_kind : uint8_t
{
	none = 0,
	table,     // SetComputeRootDescriptorTable(param, { arg_gpu })
	cbv,       // SetComputeRootConstantBufferView(param, va)   - va derived from root_cbvs[param]
	srv,       // SetComputeRootShaderResourceView(param, arg_gpu)
	uav,       // SetComputeRootUnorderedAccessView(param, arg_gpu)
	constants, // SetComputeRoot32BitConstants(param, count, values, first)
};

// Streamline caps this at 64 dwords per parameter (sl_d3d12CommandList.h:38,
// kMaxComputeRoot32BitConstCount) and logs on overflow rather than truncating silently. Same here.
static constexpr uint32_t kMaxRootConstDwords = 64;
// How many DISTINCT root parameters may carry 32-bit constants at once. UE 4.27's D3D12 root
// signatures use root CBVs, not root constants, so the expected number is zero; this exists so a
// title that does use them is restored rather than silently dropped, without paying
// 256 * 64 * 4 bytes of shadow per pipe per command list.
static constexpr uint32_t kMaxRootConstParams = 8;

// One root-constant PARAMETER, shadowed as a dense dword array plus a validity bitmask.
//
// NOT as a single (first, count) window. ReShade emits one push_constants event per call, and
// ID3D12GraphicsCommandList::SetComputeRoot32BitConstant (the SINGULAR entry point) produces
// count == 1 - so an application that fills a 4-dword root parameter with four
// SetComputeRoot32BitConstant calls emits four events, each describing a different dword. A
// window that is overwritten per event would end up holding only the LAST dword, and the replay
// would leave the other three undefined after the root signature reset. The mask makes each dword
// independently remembered, and the replay below re-emits one call per contiguous run of set
// dwords.
//
// kMaxRootConstDwords is 64, which is exactly the width of set_mask. Keep them in step.
struct root_constants
{
	uint32_t param = 0;
	uint64_t set_mask = 0;                    // bit d set == values[d] was written
	uint32_t values[kMaxRootConstDwords] = {};
};

struct pipe_bindings
{
	pipeline_layout               layout = { 0 };
	std::vector<descriptor_table> tables;
	std::vector<bool>             is_root_descriptor;
	std::vector<root_cbv>         root_cbvs;

	// Parallel to 'tables'. arg_gpu holds a raw D3D12_GPU_DESCRIPTOR_HANDLE::ptr for
	// root_arg_kind::table and a raw D3D12_GPU_VIRTUAL_ADDRESS for srv/uav. For cbv the address
	// is NOT stored here: ReShade hands us a (resource, offset) pair and turning that into a VA
	// costs an ID3D12Resource::GetGPUVirtualAddress call, which would be paid on every draw. It is
	// done once, at restore time, from root_cbvs[param].
	std::vector<root_arg_kind>    arg_kind;
	std::vector<uint64_t>         arg_gpu;

	root_constants consts[kMaxRootConstParams];
	uint32_t       const_count = 0;
	// Set when a push_constants event could not be recorded (too many parameters, or more than
	// kMaxRootConstDwords dwords). A restore built on a truncated shadow is worse than no restore,
	// so the add-on refuses to run the pass at all while this is set.
	bool           consts_overflowed = false;

	void reset()
	{
		layout = { 0 };
		tables.clear();
		is_root_descriptor.clear();
		root_cbvs.clear();
		arg_kind.clear();
		arg_gpu.clear();
		const_count = 0;
		consts_overflowed = false;
	}

	void ensure(uint32_t n)
	{
		if (n > kMaxRootParams)
			return;
		if (tables.size() < n)
		{
			tables.resize(n);
			is_root_descriptor.resize(n, false);
			root_cbvs.resize(n);
			arg_kind.resize(n, root_arg_kind::none);
			arg_gpu.resize(n, 0);
		}
	}

	// Records one push_constants event. Returns false if it could not be recorded faithfully.
	//
	// MERGES into the parameter's dword array rather than replacing a window: see root_constants.
	// Any write that would land outside the tracked array latches consts_overflowed, and the pass
	// then refuses to run rather than replay a root state it knows is missing dwords.
	bool record_constants(uint32_t param, uint32_t first, uint32_t count, const void *values)
	{
		if (values == nullptr || count == 0)
			return true; // nothing to remember
		// The window must fit the tracked array at the offset it is written to - 'first' is
		// DestOffsetIn32BitValues, so a late window can overflow even when count alone does not.
		if (count > kMaxRootConstDwords ||
		    static_cast<uint64_t>(first) + count > kMaxRootConstDwords)
		{
			consts_overflowed = true;
			return false;
		}

		root_constants *rc = nullptr;
		for (uint32_t i = 0; i < const_count; ++i)
		{
			if (consts[i].param == param) { rc = &consts[i]; break; }
		}
		if (rc == nullptr)
		{
			if (const_count >= kMaxRootConstParams)
			{
				consts_overflowed = true;
				return false;
			}
			rc = &consts[const_count++];
			rc->param    = param;
			rc->set_mask = 0;
		}

		std::memcpy(rc->values + first, values, static_cast<size_t>(count) * 4);
		// count <= 64 and first + count <= 64, so neither shift is UB.
		const uint64_t window = (count >= 64) ? ~0ull : (((1ull << count) - 1ull) << first);
		rc->set_mask |= window;
		return true;
	}
};

struct cmd_shadow
{
	pipe_bindings gfx;
	pipe_bindings cmp;
	// D3D12 has a single pipeline-state slot shared by Draw and Dispatch, and ReShade reports
	// SetPipelineState with pipeline_stage::all, so one PSO per command list is correct here.
	pipeline      pso = { 0 };
	// The ray-tracing state object, if one was bound. SetPipelineState and SetPipelineState1 are
	// mutually exclusive on D3D12 (Streamline's shadow makes the same split), so exactly one of
	// these two is replayed.
	pipeline      state_object = { 0 };

	// Memoised answer to "does the global pipeline table consider cs->pso interesting?", so the
	// per-DRAW path never has to take the process-wide mutex. A D3D12 command list is recorded by
	// exactly one thread at a time (that is the API contract, and it is what makes the whole
	// pipe_bindings shadow sound), so this needs no synchronisation of its own. pso_checked is
	// the PSO the cached answer refers to; a mismatch means "ask again".
	pipeline      pso_checked     = { 0 };
	bool          pso_interesting = false;

	// The same memo, for the DLSS-NR path: "is cs->pso the shader the ini pinned?". Kept separate
	// from pso_interesting because that one is retired once the probe has dumped a shader, and the
	// NR path must keep firing forever.
	pipeline      nr_checked      = { 0 };
	bool          nr_is_target    = false;
	// The overlay's IDENTIFICATION EPOCH at the moment that answer was cached. shader_hash is a
	// live setting now, and the memo above is per-command-list: clearing it on the next
	// SetPipelineState would take effect on some command lists and not others, which is
	// non-deterministic and therefore worse than not being editable at all. An epoch is a single
	// atomic that EVERY concurrently recording thread reads for itself, so one bump invalidates
	// every cached answer in the process at once. Zero means "cached before any epoch was read",
	// which never matches a live epoch that starts at zero only until the first bump - and a
	// spurious re-check costs one map lookup, so the conservative direction is the cheap one.
	uint32_t      nr_epoch        = 0;

	// Render targets recorded at OMSetRenderTargets, so a draw can report them.
	resource_view rtvs[8] = {};
	uint32_t      rtv_count = 0;
	resource_view dsv = { 0 };

	void reset()
	{
		gfx.reset();
		cmp.reset();
		pso = { 0 };
		state_object = { 0 };
		pso_checked = { 0 };
		pso_interesting = false;
		nr_checked = { 0 };
		nr_is_target = false;
		nr_epoch = 0;
		for (auto &rtv : rtvs) rtv = { 0 };
		rtv_count = 0;
		dsv = { 0 };
	}
};

// -------------------------------------------------------------------------------------------
// Pipeline layout deep copy - the corrected version of the defect described at the top.
// -------------------------------------------------------------------------------------------

// Returns true if this parameter type carries a range array out-of-line (and therefore needs a
// deep copy), false for the inline-in-the-union kinds.
inline bool param_has_out_of_line_ranges(pipeline_layout_param_type t)
{
	return t == pipeline_layout_param_type::descriptor_table ||
	       t == pipeline_layout_param_type::descriptor_table_with_flags ||
	       t == pipeline_layout_param_type::push_descriptors_with_ranges ||
	       t == pipeline_layout_param_type::push_descriptors_with_ranges_and_flags;
}

inline bool param_uses_wide_ranges(pipeline_layout_param_type t)
{
	// descriptor_table_with_flags == descriptor_table_with_static_samplers (deprecated alias)
	// push_descriptors_with_ranges_and_flags == push_descriptors_with_static_samplers
	return t == pipeline_layout_param_type::descriptor_table_with_flags ||
	       t == pipeline_layout_param_type::push_descriptors_with_ranges_and_flags;
}

// Only a real descriptor TABLE is addressable through bind_descriptor_tables. push_descriptors*
// params are root descriptors and never reference a heap.
inline bool param_is_bindable_table(pipeline_layout_param_type t)
{
	return t == pipeline_layout_param_type::descriptor_table ||
	       t == pipeline_layout_param_type::descriptor_table_with_flags;
}

inline void copy_layout_params(uint32_t count, const pipeline_layout_param *params, std::vector<layout_param> &out)
{
	if (count > kMaxRootParams)
		count = kMaxRootParams;

	out.clear();
	out.resize(count);

	for (uint32_t i = 0; i < count; ++i)
	{
		const pipeline_layout_param &p = params[i];
		out[i].reported_type = p.type;
		out[i].is_table = param_is_bindable_table(p.type);

		if (param_has_out_of_line_ranges(p.type))
		{
			if (param_uses_wide_ranges(p.type))
			{
				// The pointer's STATIC TYPE is const descriptor_range_with_flags*, so
				// .ranges[k] and the iterator arithmetic below both step by 40 bytes.
				const uint32_t n = p.descriptor_table_with_flags.count;
				const descriptor_range_with_flags *src = p.descriptor_table_with_flags.ranges;
				if (n != 0 && n <= kMaxRangesParam && src != nullptr)
					out[i].ranges.assign(src, src + n);
			}
			else
			{
				// Source elements are 28 bytes apart. Widen one element at a time: read
				// through the correctly-typed pointer, store into the wide type.
				const uint32_t n = p.descriptor_table.count;
				const descriptor_range *src = p.descriptor_table.ranges;
				if (n != 0 && n <= kMaxRangesParam && src != nullptr)
				{
					out[i].ranges.resize(n);
					for (uint32_t k = 0; k < n; ++k)
					{
						static_cast<descriptor_range &>(out[i].ranges[k]) = src[k];
						out[i].ranges[k].flags = descriptor_range_flags::none;
					}
				}
			}
		}
		else if (p.type == pipeline_layout_param_type::push_descriptors)
		{
			// A single descriptor_range stored inline in the union - already by value.
			out[i].ranges.resize(1);
			static_cast<descriptor_range &>(out[i].ranges[0]) = p.push_descriptors;
			out[i].ranges[0].flags = descriptor_range_flags::none;
		}

		// descriptor_range_with_flags::static_samplers points at ReShade's function-local
		// std::vector<sampler_desc>, which dies with the callback. We never read it; null it so
		// a future edit cannot dereference a dangling pointer.
		for (auto &r : out[i].ranges)
			r.static_samplers = nullptr;
	}
}

// -------------------------------------------------------------------------------------------
// Heap shadow
// -------------------------------------------------------------------------------------------

inline bool descriptor_type_is_view(descriptor_type t)
{
	// NOTE: shader_resource_view == texture_shader_resource_view == 2, and
	// unordered_access_view == texture_unordered_access_view == 3. They are ALIASES, so a
	// switch cannot list both.
	return t == descriptor_type::texture_shader_resource_view ||
	       t == descriptor_type::texture_unordered_access_view ||
	       t == descriptor_type::buffer_shader_resource_view ||
	       t == descriptor_type::buffer_unordered_access_view ||
	       t == descriptor_type::acceleration_structure;
}

// Descriptor bookkeeping is split into a PREPARE phase and an APPLY phase.
//
// WHY. Everything that touches sh.heaps needs the device-wide exclusive lock, but
// get_descriptor_heap_offset does NOT: it is a virtual call back into ReShade, which does its own
// locking, and it is the expensive half. UE4 drives these events from every parallel command-list
// recording thread, so holding our exclusive lock across a ReShade call serialises the whole
// renderer on this add-on. PREPARE runs lock-free and produces plain integers; APPLY takes the
// lock and does nothing but map writes.
//
// DELIBERATELY NO DEFAULT MEMBER INITIALISERS. These are used as fixed-size stack arrays in the
// event handlers (probe::prepared_update staged[64]), and a default member initialiser would make
// GCC zero the whole array on every event - a `rep stosq` of a couple of kilobytes on every
// UE4 descriptor update, of which there are thousands per frame. prepare_descriptor_update()
// writes EVERY field before it returns true, and no element is read unless it returned true, so
// the indeterminate initial value is never observed. The static_assert holds that property.
struct prepared_update
{
	uint64_t        heap;
	uint32_t        offset;
	uint32_t        count;
	descriptor_type type;
	const void     *descriptors;
};
static_assert(std::is_trivially_default_constructible<prepared_update>::value,
	"prepared_update must stay trivially default constructible - see the note above");

// No lock held. Returns false if this update is to be skipped entirely.
inline bool prepare_descriptor_update(device_shadow &sh, device *dev, const descriptor_table_update &u, prepared_update &out)
{
	// push_descriptors payloads arrive with table == 0; get_descriptor_heap_offset asserts on
	// that and falls through to a bogus lookup in release.
	if (u.table.handle == 0 || u.descriptors == nullptr || u.count == 0)
		return false;

	uint32_t offset = 0;
	descriptor_heap heap = { 0 };
	dev->get_descriptor_heap_offset(u.table, u.binding, u.array_offset, &heap, &offset);
	if (heap.handle == 0)
		return false;

	if (static_cast<uint64_t>(offset) + u.count > kMaxHeapSlots)
	{
		sh.dropped_heap_growth.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	out.heap        = heap.handle;
	out.offset      = offset;
	out.count       = u.count;
	out.type        = u.type;
	out.descriptors = u.descriptors;
	return true;
}

// sh.mutex held EXCLUSIVELY. 'descriptors' still points into ReShade's callback-lifetime memory,
// which is valid for the whole event, and the event has not returned yet.
inline void apply_prepared_update(device_shadow &sh, const prepared_update &u)
{
	const size_t end = static_cast<size_t>(u.offset) + u.count;

	std::vector<heap_slot> &slots = sh.heaps[u.heap];
	if (slots.size() < end)
		slots.resize(end);

	for (uint32_t k = 0; k < u.count; ++k)
	{
		heap_slot &s = slots[u.offset + k];
		s.type = u.type;
		s.view = { 0 };

		if (u.type == descriptor_type::sampler_with_resource_view)
			s.view = static_cast<const sampler_with_resource_view *>(u.descriptors)[k].view;
		else if (descriptor_type_is_view(u.type))
			s.view = static_cast<const resource_view *>(u.descriptors)[k];
		// samplers and CBVs carry no resource_view; the type alone is recorded.
	}
}

// MANDATORY. UE4 stages descriptors into non-shader-visible CPU heaps and CopyDescriptors them
// into the shader-visible ring every frame. Without this the shadow of the GPU heap is empty and
// the probe reports nothing.
//
// VOLUME. ReShade's D3D12Device::CopyDescriptors emits one descriptor_table_copy per DESCRIPTOR
// whenever pSrcDescriptorRangeSizes is null - which is exactly how UE4's FD3D12DescriptorCache
// calls it (an array of size-1 source ranges). So a 64-SRV table arrives as 64 entries of
// count == 1, on every draw, from every recording thread. Anything per-entry that allocates, or
// that calls back into ReShade under our exclusive lock, is paid thousands of times a frame.
//
// No default member initialisers, for the same reason as prepared_update above; here it matters
// more, because copy_descriptor_tables is the highest-frequency event of the two.
struct prepared_copy
{
	uint64_t src_heap;
	uint64_t dst_heap;
	uint32_t src_off;
	uint32_t dst_off;
	uint32_t count;
};
static_assert(std::is_trivially_default_constructible<prepared_copy>::value,
	"prepared_copy must stay trivially default constructible - see the note above");

// No lock held.
inline bool prepare_descriptor_copy(device_shadow &sh, device *dev, const descriptor_table_copy &c, prepared_copy &out)
{
	if (c.source_table.handle == 0 || c.dest_table.handle == 0 || c.count == 0)
		return false;

	uint32_t src_offset = 0, dst_offset = 0;
	descriptor_heap src_heap = { 0 }, dst_heap = { 0 };
	dev->get_descriptor_heap_offset(c.source_table, c.source_binding, c.source_array_offset, &src_heap, &src_offset);
	dev->get_descriptor_heap_offset(c.dest_table,   c.dest_binding,   c.dest_array_offset,   &dst_heap, &dst_offset);
	if (src_heap.handle == 0 || dst_heap.handle == 0)
		return false;

	if (static_cast<uint64_t>(dst_offset) + c.count > kMaxHeapSlots)
	{
		sh.dropped_heap_growth.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	out.src_heap = src_heap.handle;
	out.dst_heap = dst_heap.handle;
	out.src_off  = src_offset;
	out.dst_off  = dst_offset;
	out.count    = c.count;
	return true;
}

// sh.mutex held EXCLUSIVELY.
//
// NO STAGING BUFFER. An earlier revision copied through a `heap_slot inline_stage[64]` local.
// heap_slot has default member initialisers, so that array is default-constructed on EVERY call -
// GCC emits a 1 KB `rep stosq` per call - and this function is called once per DESCRIPTOR (see the
// volume note above), under the device-wide exclusive lock. At UE4's rate (a 64-entry batch per
// draw) that is tens of megabytes of stack zeroing per frame inside the lock, which is exactly the
// cost the prepare/apply split exists to remove.
//
// Staging is not needed at all, for a reason that has to be stated precisely because both
// reference implementations get it wrong in the other direction:
//   * std::unordered_map NEVER invalidates references or pointers to its ELEMENTS on insert or
//     rehash ([unord.req]) - only ITERATORS. So binding `src` as a reference before the
//     sh.heaps[dst] insert keeps it valid across that insert; binding src_it and using it after
//     would not.
//   * The one case that genuinely aliases is src_heap == dst_heap, where dst.resize() can
//     reallocate the very buffer src points into, and where D3D12 permits the ranges to overlap.
//     Re-reading .data() after the resize handles the reallocation, and std::memmove handles the
//     overlap. src_off + count <= src.size() was checked before the resize, and resize only grows,
//     so it still holds after.
inline void apply_prepared_copy(device_shadow &sh, const prepared_copy &c)
{
	static_assert(std::is_trivially_copyable<heap_slot>::value,
		"heap_slot must be trivially copyable for the memmove below");

	const auto src_it = sh.heaps.find(c.src_heap);
	if (src_it == sh.heaps.end())
	{
		// Never observed this staging heap. ReShade's own example reads out of range here;
		// renodx skips, which is what we do.
		sh.copies_missing_src.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	// A REFERENCE, deliberately - see the note above. src_it must not be touched past the insert.
	std::vector<heap_slot> &src = src_it->second;
	if (static_cast<uint64_t>(c.src_off) + c.count > src.size())
	{
		sh.copies_missing_src.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	std::vector<heap_slot> &dst = sh.heaps[c.dst_heap]; // may insert and rehash; `src` survives it
	const size_t end = static_cast<size_t>(c.dst_off) + c.count;
	if (dst.size() < end)
		dst.resize(end); // may reallocate dst's own buffer - and dst may BE src

	std::memmove(dst.data() + c.dst_off, src.data() + c.src_off,
		static_cast<size_t>(c.count) * sizeof(heap_slot));
}

// -------------------------------------------------------------------------------------------
// The join: resolve every SRV visible to the given pipeline class at this point.
//
// sink(dx_register_index, dx_register_space, slot_type, resource_view, root_param, range_index)
// -------------------------------------------------------------------------------------------
struct resolved_srv
{
	uint32_t        dx_register_index = 0;
	uint32_t        dx_register_space = 0;
	uint32_t        root_param        = 0;
	uint32_t        heap_offset       = 0;
	descriptor_type slot_type         = descriptor_type::sampler;
	resource_view   view              = { 0 };
	// False unless the shader's own dcl_resource census declared THIS register in space 0.
	// Everything else holds STALE descriptors (see the TIER_3 note below) whose resource may
	// already be destroyed, and device_impl::get_resource_desc dereferences the ID3D12Resource*
	// directly. We report such slots but never resolve them.
	bool            safe_to_resolve   = true;
};

// Collects into 'out'. Takes the device shared lock once for the whole join.
//
// 'declared_srv_mask' is the EXACT set of t-registers the shader's dcl_resource census found
// (bit N == t<N>). 'count_limit' is how far to look for occupied slots at all.
//
// Why the two differ: vkd3d-proton always reports D3D12_RESOURCE_BINDING_TIER_3, so UE4 declares
// every SRV range as MAX_SRVS (64) regardless of use (D3D12Util.cpp InitShaderRegisterCounts),
// while only CurrentShaderSRVCounts[Stage] descriptors are actually copied in. Everything else is
// left over from unrelated earlier draws that shared the heap block.
//
// A MASK, NOT A CEILING. Gating on `dx_register_index < census_max + 1` still resolves every hole
// in a sparse declaration - a shader declaring t0,t1,t2,t14 would have t3..t13 dereferenced - and
// there is no destroy_resource_view handler in this add-on, while ReShade keys its view table on
// the RECYCLED D3D12 CPU descriptor handle. A stale slot therefore resolves to a live but
// unrelated resource and is printed with a definite class=COLOUR/DEPTH/VELOCITY label: a
// confidently wrong answer, which is the one outcome this probe must never produce. Pass 0 to
// resolve nothing, which is the correct behaviour when the census itself is untrustworthy.
//
// Space is part of the identity: SM 5.0 dcl_resource carries no register space, so a declared
// register only ever means space 0. A range in any other space was not declared by this shader.
inline void resolve_bound_srvs(device *dev, device_shadow &sh, const pipe_bindings &b,
                               uint64_t declared_srv_mask, uint32_t count_limit,
                               std::vector<resolved_srv> &out)
{
	if (b.layout.handle == 0)
		return;

	std::shared_lock<std::shared_mutex> lock(sh.mutex);

	const auto lit = sh.layouts.find(b.layout.handle);
	if (lit == sh.layouts.end())
		return;
	const std::vector<layout_param> &params = lit->second;

	const size_t n = (b.tables.size() < params.size()) ? b.tables.size() : params.size();
	for (size_t p = 0; p < n; ++p)
	{
		// Defensive: these three vectors are only ever grown together by pipe_bindings::ensure,
		// but a probe must not index on an invariant it merely believes.
		if (p >= b.is_root_descriptor.size())
			break;
		if (b.tables[p].handle == 0 || b.is_root_descriptor[p])
			continue;
		if (!params[p].is_table)
			continue;

		// params[p].ranges is ALWAYS descriptor_range_with_flags, so the stride here is correct
		// by construction - there is no way to accidentally index it at 28 bytes.
		for (const descriptor_range_with_flags &range : params[p].ranges)
		{
			// An unbounded range would loop 4 billion times inside a draw callback.
			if (range.count == 0 || range.count == UINT32_MAX)
				continue;
			// Filter RANGES on the generic SRV enumerator; slots are discriminated separately.
			if (range.type != descriptor_type::shader_resource_view &&
			    range.type != descriptor_type::sampler_with_resource_view)
				continue;

			uint32_t base = 0;
			descriptor_heap heap = { 0 };
			dev->get_descriptor_heap_offset(b.tables[p], range.binding, 0, &heap, &base);
			if (heap.handle == 0)
				continue;

			const auto hit = sh.heaps.find(heap.handle);
			if (hit == sh.heaps.end())
				continue;
			const std::vector<heap_slot> &slots = hit->second;

			uint32_t limit = range.count;
			if (limit > count_limit) limit = count_limit;
			if (limit > kMaxSrvWalk) limit = kMaxSrvWalk;

			for (uint32_t j = 0; j < limit; ++j)
			{
				const size_t idx = static_cast<size_t>(base) + j;
				if (idx >= slots.size())
					break;

				const heap_slot &s = slots[idx];
				if (s.view.handle == 0)
					continue;
				// Discriminate at the SLOT: buffer SRVs and acceleration structures live in the
				// same D3D12_DESCRIPTOR_RANGE_TYPE_SRV range as texture SRVs.
				if (s.type != descriptor_type::texture_shader_resource_view &&
				    s.type != descriptor_type::sampler_with_resource_view)
					continue;

				resolved_srv r;
				r.dx_register_index = range.dx_register_index + j;
				r.dx_register_space = range.dx_register_space;
				r.root_param        = static_cast<uint32_t>(p);
				r.heap_offset       = base + j;
				r.slot_type         = s.type;
				r.view              = s.view;
				r.safe_to_resolve   = (r.dx_register_space == 0) &&
				                      (r.dx_register_index < 64u) &&
				                      (((declared_srv_mask >> r.dx_register_index) & 1ull) != 0);
				out.push_back(r);

				if (out.size() >= kMaxSrvWalk * 2)
					return; // hard cap on log volume
			}
		}
	}
}

// -------------------------------------------------------------------------------------------
// The UAV join. Identical in shape to resolve_bound_srvs, and identical in its safety rule: a
// u-register the shader did not declare holds a STALE descriptor, and resolving it dereferences
// an ID3D12Resource that may already be destroyed.
//
// WHY THIS EXISTS. The probe deliberately did not track UAVs ("outputs are UAVs, not tracked by
// this probe"), so "which UAV is the TAA output?" was left open. It is answered here by
// resolution plus format/extent agreement, never by position alone - see pick_taa_output_uav in
// stray_dlssnr.cpp, which also logs every candidate and refuses to guess when two of them match.
// -------------------------------------------------------------------------------------------
struct resolved_uav
{
	uint32_t        dx_register_index = 0;
	uint32_t        dx_register_space = 0;
	uint32_t        root_param        = 0;
	uint32_t        heap_offset       = 0;
	descriptor_type slot_type         = descriptor_type::sampler;
	resource_view   view              = { 0 };
	bool            safe_to_resolve   = true;
};

inline void resolve_bound_uavs(device *dev, device_shadow &sh, const pipe_bindings &b,
                               uint64_t declared_uav_mask, uint32_t count_limit,
                               std::vector<resolved_uav> &out)
{
	if (b.layout.handle == 0)
		return;

	std::shared_lock<std::shared_mutex> lock(sh.mutex);

	const auto lit = sh.layouts.find(b.layout.handle);
	if (lit == sh.layouts.end())
		return;
	const std::vector<layout_param> &params = lit->second;

	const size_t n = (b.tables.size() < params.size()) ? b.tables.size() : params.size();
	for (size_t p = 0; p < n; ++p)
	{
		if (p >= b.is_root_descriptor.size())
			break;
		if (b.tables[p].handle == 0 || b.is_root_descriptor[p])
			continue;
		if (!params[p].is_table)
			continue;

		for (const descriptor_range_with_flags &range : params[p].ranges)
		{
			if (range.count == 0 || range.count == UINT32_MAX)
				continue;
			// The generic UAV range enumerator. unordered_access_view ==
			// texture_unordered_access_view (3); buffer UAVs live in the same D3D12 range type
			// and are discriminated at the SLOT below.
			if (range.type != descriptor_type::unordered_access_view)
				continue;

			uint32_t base = 0;
			descriptor_heap heap = { 0 };
			dev->get_descriptor_heap_offset(b.tables[p], range.binding, 0, &heap, &base);
			if (heap.handle == 0)
				continue;

			const auto hit = sh.heaps.find(heap.handle);
			if (hit == sh.heaps.end())
				continue;
			const std::vector<heap_slot> &slots = hit->second;

			uint32_t limit = range.count;
			if (limit > count_limit) limit = count_limit;
			if (limit > kMaxUavWalk) limit = kMaxUavWalk;

			for (uint32_t j = 0; j < limit; ++j)
			{
				const size_t idx = static_cast<size_t>(base) + j;
				if (idx >= slots.size())
					break;

				const heap_slot &s = slots[idx];
				if (s.view.handle == 0)
					continue;
				// Only TEXTURE UAVs can be the TAA output. A buffer UAV is reported (so the log
				// shows the whole picture) but flagged unsafe so it is never resolved.
				const bool is_texture_uav = (s.type == descriptor_type::texture_unordered_access_view);
				if (s.type != descriptor_type::texture_unordered_access_view &&
				    s.type != descriptor_type::buffer_unordered_access_view)
					continue;

				resolved_uav r;
				r.dx_register_index = range.dx_register_index + j;
				r.dx_register_space = range.dx_register_space;
				r.root_param        = static_cast<uint32_t>(p);
				r.heap_offset       = base + j;
				r.slot_type         = s.type;
				r.view              = s.view;
				r.safe_to_resolve   = is_texture_uav &&
				                      (r.dx_register_space == 0) &&
				                      (r.dx_register_index < 64u) &&
				                      (((declared_uav_mask >> r.dx_register_index) & 1ull) != 0);
				out.push_back(r);

				if (out.size() >= kMaxUavWalk * 2)
					return;
			}
		}
	}
}

// -------------------------------------------------------------------------------------------
// Bound-table census, used ONLY by the state restore.
//
// D3D12 exposes no way to read the descriptor heaps back off a command list, and ReShade emits no
// event for SetDescriptorHeaps. The heaps are therefore recovered from the tables that are bound:
// device::get_descriptor_heap_offset maps a descriptor_table back to the ID3D12DescriptorHeap it
// lives in. The heap's TYPE is taken from the pipeline layout range that addresses it, never from
// ID3D12DescriptorHeap::GetDesc - that method returns a struct BY VALUE, which is precisely the
// Microsoft-vs-Itanium ABI hazard msvc_abi.hpp exists to avoid, and there is no reason to walk
// into it a second time.
// -------------------------------------------------------------------------------------------
struct bound_table_info
{
	uint32_t param        = 0;
	uint64_t table_handle = 0;   // raw D3D12_GPU_DESCRIPTOR_HANDLE::ptr (see the note in
	                             // d3d12_state.hpp about why that identity is checked at runtime)
	uint64_t heap         = 0;   // ID3D12DescriptorHeap *
	uint32_t heap_offset  = 0;   // descriptor index of this table's first descriptor
	bool     is_sampler   = false;
};

inline void collect_bound_tables(device *dev, device_shadow &sh, const pipe_bindings &b,
                                 std::vector<bound_table_info> &out)
{
	if (b.layout.handle == 0)
		return;

	std::shared_lock<std::shared_mutex> lock(sh.mutex);

	const auto lit = sh.layouts.find(b.layout.handle);
	if (lit == sh.layouts.end())
		return;
	const std::vector<layout_param> &params = lit->second;

	const size_t n = (b.tables.size() < params.size()) ? b.tables.size() : params.size();
	for (size_t p = 0; p < n; ++p)
	{
		if (p >= b.is_root_descriptor.size())
			break;
		if (b.tables[p].handle == 0 || b.is_root_descriptor[p] || !params[p].is_table)
			continue;
		if (params[p].ranges.empty())
			continue;

		bound_table_info info;
		info.param        = static_cast<uint32_t>(p);
		info.table_handle = b.tables[p].handle;
		info.is_sampler   = (params[p].ranges[0].type == descriptor_type::sampler);

		uint32_t offset = 0;
		descriptor_heap heap = { 0 };
		// binding 0, array_offset 0: the FIRST descriptor of the table, which is what the raw
		// D3D12_GPU_DESCRIPTOR_HANDLE stored in descriptor_table refers to.
		dev->get_descriptor_heap_offset(b.tables[p], 0, 0, &heap, &offset);
		info.heap        = heap.handle;
		info.heap_offset = offset;

		out.push_back(info);
	}
}

// Private-data keys. 16 arbitrary but unique bytes each.
static const uint8_t kDeviceShadowGuid[16] = {
	0x7d,0x3c,0x9e,0x14, 0x6a,0x52, 0x4b,0xb8, 0x9c,0x71, 0x2f,0x0e,0x5a,0x84,0xd6,0x11 };
static const uint8_t kCmdShadowGuid[16] = {
	0xbd,0x41,0xf0,0x82, 0x3e,0x97, 0x4a,0x26, 0x8d,0x5b, 0xc1,0x90,0x7e,0xf2,0xa4,0xc3 };

// Typed wrappers over api_object::{get,set}_private_data(const uint8_t[16], ...).
template <typename T, typename Obj>
inline T *pd_create(Obj *o, const uint8_t (&guid)[16])
{
	T *p = new (std::nothrow) T();
	if (p == nullptr)
		return nullptr;
	o->set_private_data(guid, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)));
	return p;
}

template <typename T, typename Obj>
inline T *pd_get(Obj *o, const uint8_t (&guid)[16])
{
	uint64_t v = 0;
	o->get_private_data(guid, &v);
	return reinterpret_cast<T *>(static_cast<uintptr_t>(v));
}

template <typename T, typename Obj>
inline void pd_destroy(Obj *o, const uint8_t (&guid)[16])
{
	T *p = pd_get<T>(o, guid);
	o->set_private_data(guid, 0);
	delete p;
}

} // namespace probe
