// DirectInput filter: while another window is really in front (InputBlocked), keyboard state reads as all keys up,
// buffered key events are turned into releases, and other devices' state is left alone except buffered data.
// Done by patching the shared COM vtables of the real dinput8.dll objects (IDirectInput8::CreateDevice -> device
// GetDeviceState / GetDeviceData).
#include "common.h"

namespace {
using FnCreateDevice = HRESULT(WINAPI*)(void*, const GUID&, void**, void*);
using FnGetState = HRESULT(WINAPI*)(void*, DWORD, void*);
using FnGetData = HRESULT(WINAPI*)(void*, DWORD, void*, DWORD*, DWORD);
FnCreateDevice oCreateDevice = nullptr;
FnGetState oGetDeviceState = nullptr;
FnGetData oGetDeviceData = nullptr;

struct ObjData { DWORD ofs, data, time, seq; UINT_PTR app; };   // DIDEVICEOBJECTDATA (dinput 8)

// keyboard devices (GUID_SysKeyboard) and DIK -> virtual key, for the select-screen main pick (PickFilterKey)
const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
void* volatile g_kbd[4] = {};
int g_dikVk[256];
bool IsKeyboard(void* dev) { for (void* k : g_kbd) if (k == dev) return true; return false; }
void InitDik() {
    for (int i = 0; i < 256; ++i)
        g_dikVk[i] = (int)MapVirtualKeyW((i & 0x7F) | (i & 0x80 ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
}

bool PatchSlot(void** vtbl, int idx, void* hook, void** orig) {
    if (vtbl[idx] == hook) return false;
    DWORD old;
    if (!VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    *orig = vtbl[idx];
    vtbl[idx] = hook;
    VirtualProtect(&vtbl[idx], sizeof(void*), old, &old);
    return true;
}

// 11-23 synthetic press (tag entries cut by a button): the key reads up for one poll, then down for 3 polls, so the
// game sees a fresh press after our locked entry ended (the real press fell into the lock and was ignored)
volatile LONG g_synthVk = 0, g_synthStep = 0;
HRESULT WINAPI hkGetDeviceState(void* self, DWORD cb, void* data) {
    HRESULT hr = oGetDeviceState(self, cb, data);
    if (SUCCEEDED(hr) && data && cb == 256) {
        if (InputBlocked()) memset(data, 0, cb);   // keyboard: all keys up
        else {
            auto k = (uint8_t*)data;
            for (int i = 0; i < 256; ++i)
                if ((k[i] & 0x80) && PickFilterKey(g_dikVk[i])) k[i] = 0;
            LONG vk = g_synthVk, step = g_synthStep;
            if (vk && step > 0) {
                for (int i = 0; i < 256; ++i)
                    if (g_dikVk[i] == vk) k[i] = step == 4 ? 0 : 0x80;
                if (InterlockedDecrement(&g_synthStep) <= 0) g_synthVk = 0;
            }
        }
    }
    return hr;
}

HRESULT WINAPI hkGetDeviceData(void* self, DWORD cbObj, void* data, DWORD* inOut, DWORD flags) {
    HRESULT hr = oGetDeviceData(self, cbObj, data, inOut, flags);
    if (SUCCEEDED(hr) && data && inOut && cbObj >= sizeof(ObjData)) {
        bool blocked = InputBlocked(), kbd = IsKeyboard(self);
        for (DWORD i = 0; i < *inOut; ++i) {
            auto e = (ObjData*)((uint8_t*)data + i * cbObj);
            if (blocked) e->data = 0;                                  // every event = release
            else if (kbd && (e->data & 0x80) && e->ofs < 256 && PickFilterKey(g_dikVk[e->ofs])) e->data = 0;
        }
    }
    return hr;
}

HRESULT WINAPI hkCreateDevice(void* self, const GUID& g, void** dev, void* outer) {
    HRESULT hr = oCreateDevice(self, g, dev, outer);
    if (SUCCEEDED(hr) && dev && *dev) {
        if (IsEqualGUID(g, kSysKeyboard))
            for (auto& k : g_kbd) if (!k) { k = *dev; break; }
        void** vt = *(void***)*dev;
        bool a = PatchSlot(vt, 9, (void*)hkGetDeviceState, (void**)&oGetDeviceState);
        bool b = PatchSlot(vt, 10, (void*)hkGetDeviceData, (void**)&oGetDeviceData);
        if (a || b) Log("focus: DirectInput device filtered");
    }
    return hr;
}
}  // namespace

void InputSynthPress(int vk) { g_synthStep = 0; g_synthVk = vk; g_synthStep = 4; }

void DInputHook(void* di) {
    static bool once = false;
    if (!once) { once = true; InitDik(); }
    void** vt = *(void***)di;
    if (PatchSlot(vt, 3, (void*)hkCreateDevice, (void**)&oCreateDevice)) Log("focus: DirectInput8 CreateDevice hooked");
}
