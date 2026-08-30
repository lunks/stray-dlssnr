// reshade_compat.hpp - include this instead of <reshade.hpp>.
//
// WHY THIS EXISTS
//   reshade_api_device.hpp uses __uuidof(T) inside api_object's templated private-data helpers.
//   On MSVC __uuidof is a compiler built-in, so it parses anywhere. On GCC/mingw-w64 it is a
//   macro from <guiddef.h>:
//       #define __uuidof(type) __mingw_uuidof<__typeof(type)>()
//   and reshade.hpp includes reshade_events.hpp (-> reshade_api_device.hpp) at line 8, BEFORE it
//   includes <Windows.h>. So at the point the templates are parsed neither the macro nor the
//   __mingw_uuidof declaration exists, and the build fails with
//       error: '__mingw_uuidof' was not declared in this scope
//       error: expected ')' before '__typeof'
//   Pulling <windows.h> in first fixes it.
//
//   Note __typeof (single underscore suffix) is a GNU extension, so the build also needs
//   -std=gnu++17 rather than -std=c++17. See build.sh.
//
//   We never instantiate those templates anyway - GCC ignores __declspec(uuid), so
//   __mingw_uuidof<T>() would have no definition for our types. The probe uses the explicit
//   16-byte-key virtual API through probe::pd_* in descriptor_shadow.hpp instead.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <reshade.hpp>
