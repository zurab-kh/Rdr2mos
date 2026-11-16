// The ScriptHookRDR2.dll entry points the plugin uses (same signatures as the official SDK's main.h).
//
// They are looked up at load time with GetProcAddress, by their exported (MSVC-decorated) names.
// No import library is needed, so the plugin builds the same way with any compiler and linker.
#pragma once
#include <windows.h>
#include <cstdint>

namespace scripthook {

struct Api {
    void (*scriptRegister)(HMODULE module, void (*scriptMain)()) = nullptr;
    void (*scriptUnregister)(HMODULE module) = nullptr;
    void (*scriptWait)(DWORD timeMs) = nullptr;
    void (*nativeInit)(UINT64 hash) = nullptr;
    void (*nativePush64)(UINT64 value) = nullptr;
    PUINT64 (*nativeCall)() = nullptr;
    int (*worldGetAllPeds)(int* handles, int capacity) = nullptr;
    int (*worldGetAllVehicles)(int* handles, int capacity) = nullptr;
};

extern Api g_api;

// Resolves every entry point; false if ScriptHookRDR2.dll is not loaded or lacks one of them.
bool load();

}  // namespace scripthook

inline void scriptRegister(HMODULE module, void (*scriptMain)()) { scripthook::g_api.scriptRegister(module, scriptMain); }
inline void scriptUnregister(HMODULE module) { scripthook::g_api.scriptUnregister(module); }
inline void scriptWait(DWORD timeMs) { scripthook::g_api.scriptWait(timeMs); }
inline void nativeInit(UINT64 hash) { scripthook::g_api.nativeInit(hash); }
inline void nativePush64(UINT64 value) { scripthook::g_api.nativePush64(value); }
inline PUINT64 nativeCall() { return scripthook::g_api.nativeCall(); }
inline int worldGetAllPeds(int* handles, int capacity) { return scripthook::g_api.worldGetAllPeds(handles, capacity); }
inline int worldGetAllVehicles(int* handles, int capacity) { return scripthook::g_api.worldGetAllVehicles(handles, capacity); }
