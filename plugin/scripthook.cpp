#include "scripthook.h"

namespace scripthook {

Api g_api;

namespace {
template <typename T>
bool resolve(HMODULE dll, const char* name, T& out) {
    out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
    return out != nullptr;
}
}  // namespace

bool load() {
    HMODULE dll = GetModuleHandleA("ScriptHookRDR2.dll");
    if (!dll) return false;
    Api api;
    bool ok = resolve(dll, "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z", api.scriptRegister) &&
              resolve(dll, "?scriptUnregister@@YAXPEAUHINSTANCE__@@@Z", api.scriptUnregister) &&
              resolve(dll, "?scriptWait@@YAXK@Z", api.scriptWait) &&
              resolve(dll, "?nativeInit@@YAX_K@Z", api.nativeInit) &&
              resolve(dll, "?nativePush64@@YAX_K@Z", api.nativePush64) &&
              resolve(dll, "?nativeCall@@YAPEA_KXZ", api.nativeCall) &&
              resolve(dll, "?worldGetAllPeds@@YAHPEAHH@Z", api.worldGetAllPeds) &&
              resolve(dll, "?worldGetAllVehicles@@YAHPEAHH@Z", api.worldGetAllVehicles);
    if (ok) g_api = api;
    return ok;
}

}  // namespace scripthook
