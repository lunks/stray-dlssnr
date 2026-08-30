#!/usr/bin/env bash
# Cross-build the STRAY DLSS-NR add-on and its NGX trampoline to Windows x64 from macOS/Linux.
#
# Requires mingw-w64:   brew install mingw-w64     (macOS)
#                       apt install g++-mingw-w64  (Debian/Ubuntu)
#
# Outputs:
#   stray_dlssnr.addon64   the ReShade add-on
#   remix_nvngx.dll        the NGX trampoline (see trampoline/remix_nvngx.cpp for why)
set -euo pipefail

cd "$(dirname "$0")"

CXX="${CXX:-x86_64-w64-mingw32-g++}"
OBJDUMP="${OBJDUMP:-x86_64-w64-mingw32-objdump}"
OUT="${OUT:-stray_dlssnr.addon64}"
TRAMPOLINE="${TRAMPOLINE:-remix_nvngx.dll}"

if ! command -v "$CXX" >/dev/null 2>&1; then
	echo "error: $CXX not found. Install mingw-w64 (brew install mingw-w64)." >&2
	exit 1
fi

echo "toolchain: $("$CXX" --version | head -1)"

# -static* so neither binary has a libgcc/libstdc++ DLL dependency inside the Proton prefix.
# -municode is NOT used: the entry point is DllMain, not wmain.
#
# C++ ABI WARNING. mingw-w64 g++ uses the Itanium/GNU C++ ABI; ReShade is built with MSVC. The two
# disagree about how a non-static member function returns a class BY VALUE, and three ReShade
# device virtuals do exactly that (get_resource_desc / get_resource_from_view /
# get_resource_view_desc). src/msvc_abi.hpp handles this with explicit out-parameter thunks and
# verifies the vtable offsets at load. Do not add further calls to by-value-returning ReShade
# virtuals without routing them through there.
#
# The SAME hazard exists on the NGX side and is handled in src/ngx_interop.hpp, differently: MSVC
# numbers an OVERLOAD SET in reverse declaration order and the Itanium ABI does not, so
# NVSDK_NGX_Parameter's 17-slot vtable is laid out by hand there rather than written as C++
# virtuals. Read the header comment before touching it.
#
# D3D12 COM methods that return an aggregate are safe: mingw defines
# WIDL_EXPLICIT_AGGREGATE_RETURNS for GCC C++ (_mingw_mac.h), which turns
# GetGPUDescriptorHandleForHeapStart / GetDesc into the explicit out-parameter form that matches
# MSVC's member-function convention exactly. Do not #undef it.
COMMON_FLAGS=(
	-std=gnu++17
	-O2
	-s
	-shared
	-static -static-libgcc -static-libstdc++
	-DUNICODE -D_UNICODE
	-DWIN32_LEAN_AND_MEAN
	-DNOMINMAX
	-DNDEBUG
	-Wall -Wextra
	-Wno-unknown-pragmas
	-Wno-cast-function-type
	-Wno-unused-parameter
	-Wno-attributes
)

echo
echo "=== building the add-on: $OUT"
"$CXX" \
	"${COMMON_FLAGS[@]}" \
	-I include \
	src/*.cpp \
	-o "$OUT"

echo "built: $(pwd)/$OUT"
ls -l "$OUT"

echo
echo "=== building the trampoline: $TRAMPOLINE"
# -fno-optimize-sibling-calls is belt and braces on top of the volatile counter store in each
# forwarder. A TAIL JUMP would reuse the add-on's return address, which is exactly what the
# snippet's caller check looks at, and every gated export would then return 0xbad00002.
"$CXX" \
	"${COMMON_FLAGS[@]}" \
	-fno-optimize-sibling-calls \
	trampoline/remix_nvngx.cpp \
	-o "$TRAMPOLINE"

echo "built: $(pwd)/$TRAMPOLINE"
ls -l "$TRAMPOLINE"

# ---------------------------------------------------------------------------------------------
# Verify the property the whole trampoline exists for: every forwarder must CALL the snippet and
# then return, never tail-jump to it. This is the "call=1, tailjmp=0" check from the shipped
# Vulkan build, mechanised.
# ---------------------------------------------------------------------------------------------
echo
echo "=== verifying the trampoline forwarders make REAL calls (no tail jumps)"
if command -v "$OBJDUMP" >/dev/null 2>&1; then
	# -s strips symbols, so disassemble an unstripped object instead.
	#
	# PORTABILITY, and it is load-bearing. `mktemp -t <name>` with no X's is BSD-only; GNU
	# coreutils rejects it ("too few X's in template") and exits 1, which under `set -euo pipefail`
	# aborts the script HERE - after both DLLs are already on disk, and before a single OK/FAIL
	# line is printed. A Linux builder (the platform this script's own header documents:
	# `apt install g++-mingw-w64`) would be left with a complete-looking set of artifacts whose
	# call-vs-tailjmp property was never checked, and a tail jump is exactly what makes every gated
	# export return 0xbad00002. `mktemp -d` with no argument is portable across BSD and GNU.
	#
	# A directory, not a file, because `$(mktemp ...).o` appends the suffix to the NAME - so the
	# file mktemp actually created is a different path and the later `rm -f` never removed it.
	TMPDIR_CHECK="$(mktemp -d)"
	trap 'rm -rf "$TMPDIR_CHECK"' EXIT
	TMPOBJ="$TMPDIR_CHECK/remix_nvngx_check.o"
	"$CXX" -std=gnu++17 -O2 -c -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DNDEBUG \
		-fno-optimize-sibling-calls trampoline/remix_nvngx.cpp -o "$TMPOBJ"

	fail=0
	for fn in Init_Ext Shutdown1 CreateFeature ReleaseFeature EvaluateFeature \
	          PopulateParameters_Impl AllocateParameters DestroyParameters GetFeatureRequirements; do
		body="$("$OBJDUMP" -d "$TMPOBJ" \
			| awk -v f="NVSDK_NGX_D3D12_${fn}>:" '$0 ~ f {grab=1; next} grab && /^$/ {exit} grab {print}')"
		if [ -z "$body" ]; then
			echo "  ?? NVSDK_NGX_D3D12_${fn}: not found in the disassembly"
			fail=1
			continue
		fi
		calls=$(printf '%s\n' "$body" | grep -cE '\bcall(q)?\s+\*' || true)
		tails=$(printf '%s\n' "$body" | grep -cE '\bjmp(q)?\s+\*' || true)
		if [ "$calls" -ge 1 ] && [ "$tails" -eq 0 ]; then
			echo "  OK NVSDK_NGX_D3D12_${fn}: indirect call=${calls} tailjmp=${tails}"
		else
			echo "  FAIL NVSDK_NGX_D3D12_${fn}: indirect call=${calls} tailjmp=${tails}"
			fail=1
		fi
	done
	rm -rf "$TMPDIR_CHECK"
	trap - EXIT

	if [ "$fail" -ne 0 ]; then
		echo
		echo "error: at least one forwarder is a TAIL JUMP or has no indirect call." >&2
		echo "       A tail jump preserves the caller's return address, so the snippet's" >&2
		echo "       \"called from nvngx.dll\" check would see the ReShade add-on and every" >&2
		echo "       gated export would return 0xbad00002. Do not ship this build." >&2
		exit 1
	fi
else
	echo "  (skipped: $OBJDUMP not found - verify the disassembly by hand before shipping)"
fi

cat <<'EOF'

Next:
  1. Copy ALL of these next to the game executable, alongside the ReShade DLL:
       S:\common\Stray\Hk_project\Binaries\Win64\
         stray_dlssnr.addon64      (this build)
         remix_nvngx.dll           (this build)
         nvngx_dlssnr.dll          (the patched snippet)
         stray_dlssnr.ini          (optional; see README.md)
  2. Launch STRAY with -dx12.
  3. Read S:\common\Stray\Hk_project\Binaries\Win64\ReShade.log
     Grep for:  DLSS-NR
EOF
