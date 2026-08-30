// Host shim for tools/ngx_paramblock_selftest.cpp. NOT used by the add-on build.
//
// src/ngx_interop.hpp includes "reshade_compat.hpp", which pulls <windows.h> and <reshade.hpp>.
// The selftest exercises the PARAMETER BLOCK ONLY - the half of that header that has no Windows
// dependency at all - so the runner stages a copy of src/ngx_interop.hpp into a temp directory
// next to this file, and a quote-include then resolves here instead. Everything below is the
// minimum surface the loader half needs to COMPILE; none of it is called by the test.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

typedef void *HMODULE;
typedef void *FARPROC;
typedef unsigned long DWORD;
typedef const wchar_t *LPCWSTR;
typedef int BOOL;

#define MAX_PATH 260
#define CP_UTF8 65001
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
#ifndef __cdecl
#define __cdecl
#endif

inline HMODULE LoadLibraryW(const wchar_t *) { return nullptr; }
inline FARPROC GetProcAddress(HMODULE, const char *) { return nullptr; }
inline BOOL    FreeLibrary(HMODULE) { return 1; }
inline BOOL    GetModuleHandleExW(DWORD, LPCWSTR, HMODULE *m) { *m = nullptr; return 0; }
inline DWORD   GetModuleFileNameW(HMODULE, wchar_t *, DWORD) { return 0; }
inline int     WideCharToMultiByte(unsigned, DWORD, const wchar_t *, int, char *, int,
                                   const char *, BOOL *) { return 0; }

// ---- added for the tuning-range checks -------------------------------------------------------
// src/addon_config.hpp is staged into the same temp directory so the selftest can exercise the
// REAL clamp_unit()/clamp_skin() rather than a transcription of them - a copy could drift from
// the shipping code, which is exactly the failure this test exists to catch. The only Windows
// call in that header's parse path is _wfopen; the test never opens a file, so a stub that
// always fails is enough to compile and cannot change behaviour.
//
// Guarded for _WIN32: on a Windows host the CRT already declares _wfopen and this would be a
// conflicting redefinition. The shim is only ever needed on the non-Windows build machines this
// script runs on.
#include <cstdio>
#if !defined(_WIN32)
inline std::FILE *_wfopen(const wchar_t *, const wchar_t *) { return nullptr; }
#endif
