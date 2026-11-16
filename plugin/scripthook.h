// The ScriptHookRDR2.dll entry points the plugin uses (same signatures as the official SDK's main.h).
//
// They are looked up at load time with GetProcAddress, by their exported (MSVC-decorated) names.
// No import library is needed, so the plugin builds the same way with any compiler and linker.
//
// This header deliberately does not include <windows.h>: the native layer (natives.h,
// rdr2_game.cpp) only needs the table below, so the tests can build it against a simulated
// Script Hook and count every native call. The types are ABI-identical to the SDK's on x64:
// HMODULE is HINSTANCE__*, DWORD is a 32-bit unsigned long, UINT64 is a 64-bit unsigned.
#pragma once
#include <cstdint>

struct HINSTANCE__;  // what <windows.h> calls HMODULE (STRICT)

namespace scripthook {

struct Api {
    void (*scriptRegister)(HINSTANCE__* module, void (*scriptMain)()) = nullptr;
    void (*scriptUnregister)(HINSTANCE__* module) = nullptr;
    void (*scriptWait)(unsigned long timeMs) = nullptr;
    void (*nativeInit)(uint64_t hash) = nullptr;
    void (*nativePush64)(uint64_t value) = nullptr;
    uint64_t* (*nativeCall)() = nullptr;
    int (*worldGetAllPeds)(int* handles, int capacity) = nullptr;
    int (*worldGetAllVehicles)(int* handles, int capacity) = nullptr;
};

extern Api g_api;

// Resolves every entry point; false if ScriptHookRDR2.dll is not loaded or lacks one of them.
bool load();

}  // namespace scripthook

inline void scriptRegister(HINSTANCE__* module, void (*scriptMain)()) { scripthook::g_api.scriptRegister(module, scriptMain); }
inline void scriptUnregister(HINSTANCE__* module) { scripthook::g_api.scriptUnregister(module); }
inline void scriptWait(unsigned long timeMs) { scripthook::g_api.scriptWait(timeMs); }
inline void nativeInit(uint64_t hash) { scripthook::g_api.nativeInit(hash); }
inline void nativePush64(uint64_t value) { scripthook::g_api.nativePush64(value); }
inline uint64_t* nativeCall() { return scripthook::g_api.nativeCall(); }
inline int worldGetAllPeds(int* handles, int capacity) { return scripthook::g_api.worldGetAllPeds(handles, capacity); }
inline int worldGetAllVehicles(int* handles, int capacity) { return scripthook::g_api.worldGetAllVehicles(handles, capacity); }
