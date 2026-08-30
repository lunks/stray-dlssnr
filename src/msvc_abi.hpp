// msvc_abi.hpp - call ReShade's by-value-returning device virtuals correctly from a
// non-MSVC-ABI build.
//
// THE PROBLEM
//   ReShade is built with MSVC. This add-on can be built either with clang targeting
//   x86_64-pc-windows-msvc (the CI path) or with mingw-w64 g++ (the local cross-compile path in
//   build.sh). Those two use DIFFERENT C++ ABIs, and they disagree about how a non-static member
//   function returns a class type by value.
//
//   Three device virtuals return a class type by value:
//       reshade_api_device.hpp:374  virtual resource_desc      get_resource_desc(resource) const
//       reshade_api_device.hpp:396  virtual resource           get_resource_from_view(resource_view) const
//       reshade_api_device.hpp:400  virtual resource_view_desc get_resource_view_desc(resource_view) const
//
//   MEASURED, not assumed. The same translation unit against the vendored headers in include/,
//   compiled both ways, disassembles to:
//
//     clang++ -target x86_64-pc-windows-msvc (Microsoft C++ ABI)
//         get_resource_desc:       RCX = this, RDX = &return, R8 = arg   call *80(vtbl)
//         get_resource_from_view:  RCX = this, RDX = &return, R8 = arg   call *104(vtbl)
//         get_resource_view_desc:  RCX = this, RDX = &return, R8 = arg   call *112(vtbl)
//
//     x86_64-w64-mingw32-g++ 12.2.0 (Itanium/GNU C++ ABI)
//         get_resource_desc:       RCX = &return, RDX = this, R8 = arg   call *80(vtbl)
//         get_resource_from_view:  RCX = this,    RDX = arg,             call *104(vtbl)
//                                  ... result returned in RAX, NO hidden pointer at all
//         get_resource_view_desc:  RCX = &return, RDX = this, R8 = arg   call *112(vtbl)
//
//   The vtable slot offsets AGREE (80 / 104 / 112). Only the return convention diverges, and it
//   diverges in the worst possible way:
//     * slots 80 and 112: mingw puts the hidden return pointer where MSVC reads `this`, so
//       ReShade's device_impl loads a vtable pointer out of OUR STACK FRAME and calls through it.
//     * slot 104: MSVC uses a hidden pointer even for the 8-byte `resource` handle (the MS x64
//       ABI returns a UDT in RAX only from global and STATIC member functions), while mingw
//       returns it in RAX. The add-on would read the ADDRESS of ReShade's return buffer as
//       res.handle. That address is non-zero, so the `res.handle == 0` guard does not fire and
//       the garbage propagates into get_resource_desc.
//
//   Every one of these is on the SRV-dump path, i.e. reached the moment a shader passes the
//   gates. A crash there breaks the read-only / do-not-destabilise contract.
//
// THE FIX
//   Under a non-MSVC ABI, call these three through the vtable by hand with an explicit
//   out-parameter signature. A free function taking (void*, T*, arg) is passed RCX / RDX / R8 by
//   BOTH ABIs, which is exactly the register order MSVC uses for a member function with a hidden
//   return pointer. Parameter passing needs no fixing anywhere else: every by-value aggregate in
//   the ReShade event signatures is an 8-byte RESHADE_DEFINE_HANDLE POD, which both ABIs pass in
//   a register.
//
//   The slot numbers are NOT hard-coded. They are derived from the header itself through the
//   Itanium representation of a pointer-to-virtual-member-function (ptr = vtable byte offset + 1,
//   adj = 0), so editing or updating reshade_api_device.hpp moves them automatically. They are
//   then cross-checked at load against the offsets measured above, and the probe refuses to use
//   the thunks if the two ever disagree - see probe::msvc_abi_self_check().
//
// Under MSVC / clang-cl (_MSC_VER defined) none of this is compiled in: the calls go direct.

#pragma once

#include "reshade_compat.hpp"

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace probe {

using namespace reshade::api;

#if defined(_MSC_VER)

// Native Microsoft C++ ABI: the compiler already emits the right thing.
inline resource           abi_get_resource_from_view(device *dev, resource_view view) { return dev->get_resource_from_view(view); }
inline resource_view_desc abi_get_resource_view_desc(device *dev, resource_view view) { return dev->get_resource_view_desc(view); }
inline resource_desc      abi_get_resource_desc     (device *dev, resource res)       { return dev->get_resource_desc(res); }

inline bool msvc_abi_self_check(size_t out_slots[3]) { out_slots[0] = out_slots[1] = out_slots[2] = 0; return true; }
inline bool msvc_abi_thunks_active() { return false; }

#else

// The offsets measured from clang -target x86_64-pc-windows-msvc on the vendored headers.
// Only used to CHECK the derived value, never as the value itself.
static constexpr size_t kExpectedSlotResourceDesc     = 80;
static constexpr size_t kExpectedSlotResourceFromView = 104;
static constexpr size_t kExpectedSlotResourceViewDesc = 112;

// Itanium C++ ABI, section 2.3: a pointer to member function is { fnptr_or_vtable_offset_plus_1,
// this_adjustment }. For a virtual function the first word is the BYTE offset into the vtable
// plus 1, with the low bit as the "is virtual" tag. Returns SIZE_MAX if the function turns out
// not to be virtual, or if a non-zero this-adjustment says the class is not the single-inheritance
// shape this technique assumes.
template <typename F>
inline size_t itanium_vtable_byte_offset(F pmf)
{
	struct raw_pmf { uintptr_t ptr; ptrdiff_t adj; };
	static_assert(sizeof(F) == sizeof(raw_pmf), "unexpected pointer-to-member-function layout");

	raw_pmf raw = {};
	std::memcpy(&raw, &pmf, sizeof(raw));
	if ((raw.ptr & 1u) == 0 || raw.adj != 0)
		return static_cast<size_t>(-1);
	return static_cast<size_t>(raw.ptr - 1);
}

inline size_t slot_get_resource_desc()      { return itanium_vtable_byte_offset(&device::get_resource_desc); }
inline size_t slot_get_resource_from_view() { return itanium_vtable_byte_offset(&device::get_resource_from_view); }
inline size_t slot_get_resource_view_desc() { return itanium_vtable_byte_offset(&device::get_resource_view_desc); }

inline bool msvc_abi_thunks_active() { return true; }

// Set by msvc_abi_self_check(). Until it has run AND passed, every thunk below is a no-op that
// returns a zeroed value, so a header whose vtable shape stopped matching produces "unknown"
// rather than a wild indirect call. C++17 inline variable: one instance across the add-on.
inline bool g_msvc_abi_verified = false;

// True only when every derived slot matches the measured MSVC one. Fills out_slots for the log
// line either way, so a mismatch is diagnosable rather than merely fatal.
inline bool msvc_abi_self_check(size_t out_slots[3])
{
	out_slots[0] = slot_get_resource_desc();
	out_slots[1] = slot_get_resource_from_view();
	out_slots[2] = slot_get_resource_view_desc();

	g_msvc_abi_verified = out_slots[0] == kExpectedSlotResourceDesc &&
	                      out_slots[1] == kExpectedSlotResourceFromView &&
	                      out_slots[2] == kExpectedSlotResourceViewDesc;
	return g_msvc_abi_verified;
}

// Reads the vtable pointer without a type-punned lvalue, so -O2 strict aliasing cannot reorder it.
inline void *const *vtable_of(const void *obj)
{
	void *const *vt = nullptr;
	std::memcpy(&vt, obj, sizeof(vt));
	return vt;
}

inline resource abi_get_resource_from_view(device *dev, resource_view view)
{
	resource out = { 0 };
	if (dev == nullptr || !g_msvc_abi_verified)
		return out;
	// MSVC member-function-with-sret order: RCX = this, RDX = hidden return, R8 = argument.
	using fn_t = void (*)(void *, resource *, resource_view);
	void *const *vt = vtable_of(dev);
	reinterpret_cast<fn_t>(vt[slot_get_resource_from_view() / sizeof(void *)])(dev, &out, view);
	return out;
}

inline resource_view_desc abi_get_resource_view_desc(device *dev, resource_view view)
{
	resource_view_desc out = {};
	if (dev == nullptr || !g_msvc_abi_verified)
		return out;
	using fn_t = void (*)(void *, resource_view_desc *, resource_view);
	void *const *vt = vtable_of(dev);
	reinterpret_cast<fn_t>(vt[slot_get_resource_view_desc() / sizeof(void *)])(dev, &out, view);
	return out;
}

inline resource_desc abi_get_resource_desc(device *dev, resource res)
{
	resource_desc out = {};
	if (dev == nullptr || !g_msvc_abi_verified)
		return out;
	using fn_t = void (*)(void *, resource_desc *, resource);
	void *const *vt = vtable_of(dev);
	reinterpret_cast<fn_t>(vt[slot_get_resource_desc() / sizeof(void *)])(dev, &out, res);
	return out;
}

#endif // _MSC_VER

} // namespace probe
