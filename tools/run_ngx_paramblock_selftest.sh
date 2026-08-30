#!/usr/bin/env bash
# Build and run tools/ngx_paramblock_selftest.cpp natively on the build machine.
#
# The test includes the REAL src/ngx_interop.hpp, so it checks the shipping vtable rather than a
# copy of it. That header's first include is "reshade_compat.hpp", and a quote-include always
# resolves against the directory of the file containing the directive - src/ - which would drag in
# <windows.h> and <reshade.hpp>. So the header is STAGED into a temp directory alongside the two
# host shims, where the same quote-include finds the shims instead.
#
# The staged copy is made fresh from src/ on every run and deleted afterwards, so it cannot drift.
set -euo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-c++}"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

cp src/ngx_interop.hpp             "$stage/"
cp tools/ngx_paramblock_shim/*.hpp "$stage/"
cp tools/ngx_paramblock_shim/*.h   "$stage/"

"$CXX" -std=gnu++17 -O1 -Wall -I "$stage" -o "$stage/selftest" tools/ngx_paramblock_selftest.cpp
exec "$stage/selftest"
