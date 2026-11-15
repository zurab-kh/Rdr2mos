// Declarations of the ScriptHookRDR2.dll entry points the plugin uses (same signatures as the
// official SDK's main.h).
//
// MSVC derives the DLL's decorated export names straight from the C++ declarations. GCC mangles
// C++ names differently, so for MinGW the functions are imported under plain C aliases that
// ScriptHookRDR2.mingw.def maps onto the real export names.
#pragma once
#include <windows.h>
#include <cstdint>

#ifdef _MSC_VER

__declspec(dllimport) void scriptRegister(HMODULE module, void (*scriptMain)());
__declspec(dllimport) void scriptUnregister(HMODULE module);
__declspec(dllimport) void scriptWait(DWORD timeMs);
__declspec(dllimport) void nativeInit(UINT64 hash);
__declspec(dllimport) void nativePush64(UINT64 value);
__declspec(dllimport) PUINT64 nativeCall();
__declspec(dllimport) int worldGetAllPeds(int* handles, int capacity);

#else

extern "C" {
__declspec(dllimport) void sh_scriptRegister(HMODULE module, void (*scriptMain)());
__declspec(dllimport) void sh_scriptUnregister(HMODULE module);
__declspec(dllimport) void sh_scriptWait(DWORD timeMs);
__declspec(dllimport) void sh_nativeInit(UINT64 hash);
__declspec(dllimport) void sh_nativePush64(UINT64 value);
__declspec(dllimport) PUINT64 sh_nativeCall();
__declspec(dllimport) int sh_worldGetAllPeds(int* handles, int capacity);
}
inline void scriptRegister(HMODULE module, void (*scriptMain)()) { sh_scriptRegister(module, scriptMain); }
inline void scriptUnregister(HMODULE module) { sh_scriptUnregister(module); }
inline void scriptWait(DWORD timeMs) { sh_scriptWait(timeMs); }
inline void nativeInit(UINT64 hash) { sh_nativeInit(hash); }
inline void nativePush64(UINT64 value) { sh_nativePush64(value); }
inline PUINT64 nativeCall() { return sh_nativeCall(); }
inline int worldGetAllPeds(int* handles, int capacity) { return sh_worldGetAllPeds(handles, capacity); }

#endif
