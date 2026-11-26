// Alt-tab fix: the game must never learn that its window lost focus.
//  1. Window subclass: WM_ACTIVATEAPP(FALSE), WM_ACTIVATE(WA_INACTIVE) and WM_KILLFOCUS never reach the game
//     (UE4/Slate pauses or idles on these). Windows still gets them through DefWindowProc where needed.
//  2. IAT hooks on the game exe: GetForegroundWindow / GetActiveWindow / GetFocus report the game window,
//     so UE4's "is this application foreground" polling keeps saying yes.
//  3. ClipCursor is ignored while another window is really in front, so the mouse is free in other apps.
//  4. Because the game now thinks it is always in front, it would also read keys typed into other windows (chat).
//     Every input path it imports is filtered while another window is really in front: GetAsyncKeyState /
//     GetKeyState / GetRawInputData / XInputGetState (IAT) and DirectInput devices (vtable, see dllmain.cpp).
#include "common.h"
#include <xinput.h>

DWORD PadPoll(DWORD user, XINPUT_STATE* st, DWORD r);   // pad.cpp
namespace {
HWND g_hwnd = nullptr;
WNDPROC g_origProc = nullptr;

using FnGetWnd = HWND(WINAPI*)();
using FnClip = BOOL(WINAPI*)(const RECT*);
FnGetWnd oGetForegroundWindow = GetForegroundWindow;
FnGetWnd oGetActiveWindow = GetActiveWindow;
FnGetWnd oGetFocus = GetFocus;
FnClip oClipCursor = ClipCursor;

bool Active() { return g_cfg.focusFix && g_hwnd && IsWindow(g_hwnd); }
bool ReallyInFront() { return oGetForegroundWindow() == g_hwnd; }

HWND WINAPI hkGetForegroundWindow() { HWND r = oGetForegroundWindow(); return (Active() && r != g_hwnd) ? g_hwnd : r; }
HWND WINAPI hkGetActiveWindow() { HWND r = oGetActiveWindow(); return (Active() && !r) ? g_hwnd : r; }
HWND WINAPI hkGetFocus() { HWND r = oGetFocus(); return (Active() && !r) ? g_hwnd : r; }
using FnKey = SHORT(WINAPI*)(int);
using FnRaw = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using FnXState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
FnKey oGetAsyncKeyState = GetAsyncKeyState;
FnKey oGetKeyState = GetKeyState;
FnRaw oGetRawInputData = GetRawInputData;
FnXState oXInputGetState = nullptr;

SHORT WINAPI hkGetAsyncKeyState(int vk) {
    if (InputBlocked()) return 0;
    SHORT r = oGetAsyncKeyState(vk);
    return ((r & 0x8000) && PickFilterKey(vk)) ? 0 : r;
}
SHORT WINAPI hkGetKeyState(int vk) {
    if (InputBlocked()) return 0;
    SHORT r = oGetKeyState(vk);
    return ((r & 0x8000) && PickFilterKey(vk)) ? (SHORT)(r & 1) : r;
}
UINT WINAPI hkGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    UINT r = oGetRawInputData(h, cmd, data, size, hdr);
    if (data && cmd == RID_INPUT && r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER) && InputBlocked()) {
        auto ri = (RAWINPUT*)data;
        if (ri->header.dwType == RIM_TYPEKEYBOARD) { ri->data.keyboard.Flags |= RI_KEY_BREAK; ri->data.keyboard.Message = WM_KEYUP; }
        else if (ri->header.dwType == RIM_TYPEMOUSE) { ri->data.mouse.usButtonFlags = 0; ri->data.mouse.lLastX = ri->data.mouse.lLastY = 0; }
    } else if (data && cmd == RID_INPUT && r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER)) {
        auto ri = (RAWINPUT*)data;
        if (ri->header.dwType == RIM_TYPEKEYBOARD && !(ri->data.keyboard.Flags & RI_KEY_BREAK) &&
            PickFilterKey(ri->data.keyboard.VKey)) {
            ri->data.keyboard.Flags |= RI_KEY_BREAK; ri->data.keyboard.Message = WM_KEYUP;
        }
    }
    return r;
}
DWORD WINAPI hkXInputGetState(DWORD user, XINPUT_STATE* st) {
    DWORD r = oXInputGetState ? oXInputGetState(user, st) : ERROR_DEVICE_NOT_CONNECTED;
    r = PadPoll(user, st, r);
    if (r == ERROR_SUCCESS && st && InputBlocked()) memset(&st->Gamepad, 0, sizeof(st->Gamepad));
    return r;
}

BOOL WINAPI hkClipCursor(const RECT* rc) {
    if (Active() && rc && !ReallyInFront()) return oClipCursor(nullptr);
    return oClipCursor(rc);
}

LRESULT CALLBACK HookProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // select-screen main pick: the press never reaches the game (UE4 reads keys from these messages too)
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && PickFilterKey((int)wp)) return 0;
    if (g_cfg.focusFix) {
        switch (msg) {
        case WM_ACTIVATEAPP:
            if (!wp) { oClipCursor(nullptr); return 0; }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) { oClipCursor(nullptr); return DefWindowProcW(h, msg, wp, lp); }
            break;
        case WM_KILLFOCUS:
            return 0;
        case WM_NCACTIVATE:
            if (!wp) return DefWindowProcW(h, msg, wp, lp);
            break;
        }
    }
    return CallWindowProcW(g_origProc, h, msg, wp, lp);
}

// Replace one import of the main exe. Returns the previous pointer (or null if the import is absent).
void* PatchIat(HMODULE mod, const char* dll, const char* fn, void* hook, WORD ordinal = 0) {
    auto base = (uint8_t*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto oft = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) {       // 11-24: XInput comes by ordinal (2 = XInputGetState)
                if (!ordinal || IMAGE_ORDINAL(oft->u1.Ordinal) != ordinal) continue;
            } else {
                auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
                if (strcmp((char*)ibn->Name, fn) != 0) continue;
            }
            DWORD old;
            VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            void* prev = (void*)ft->u1.Function;
            ft->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            return prev;
        }
    }
    return nullptr;
}

BOOL CALLBACK FindGameWindow(HWND h, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    wchar_t cls[64]{};
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
    *(HWND*)out = h;
    return FALSE;
}
}  // namespace

void FocusInstall() {
    HMODULE exe = GetModuleHandleW(nullptr);
    struct { const char* fn; void* hook; void** orig; } list[] = {
        {"GetForegroundWindow", (void*)hkGetForegroundWindow, (void**)&oGetForegroundWindow},
        {"GetActiveWindow", (void*)hkGetActiveWindow, (void**)&oGetActiveWindow},
        {"GetFocus", (void*)hkGetFocus, (void**)&oGetFocus},
        {"ClipCursor", (void*)hkClipCursor, (void**)&oClipCursor},
        {"GetAsyncKeyState", (void*)hkGetAsyncKeyState, (void**)&oGetAsyncKeyState},
        {"GetKeyState", (void*)hkGetKeyState, (void**)&oGetKeyState},
        {"GetRawInputData", (void*)hkGetRawInputData, (void**)&oGetRawInputData},
    };
    for (auto& e : list) {
        void* prev = PatchIat(exe, "USER32.dll", e.fn, e.hook);
        if (prev) *e.orig = prev;
        Log("focus: IAT %s %s", e.fn, prev ? "hooked" : "not imported");
    }
    for (const char* dll : {"XINPUT1_3.dll", "XINPUT1_4.dll", "XINPUT9_1_0.dll"}) {
        void* prev = PatchIat(exe, dll, "XInputGetState", (void*)hkXInputGetState, 2);
        if (prev) { oXInputGetState = (FnXState)prev; Log("focus: IAT %s!XInputGetState hooked", dll); }
    }
}

// keys/pads typed while another window is in front must not reach the game (focus fix makes it think it is in front)
bool InputBlocked() { return Active() && oGetForegroundWindow() != g_hwnd; }

HWND GameWindow() { return (g_hwnd && IsWindow(g_hwnd)) ? g_hwnd : nullptr; }
bool GameWindowInFront() { return g_hwnd && oGetForegroundWindow() == g_hwnd; }

void FocusTick() {
    if (g_hwnd && IsWindow(g_hwnd)) return;
    HWND h = nullptr;
    EnumWindows(FindGameWindow, (LPARAM)&h);
    if (!h) return;
    g_hwnd = h;
    g_origProc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)HookProc);
    Log("focus: game window %p subclassed (orig proc %p)", (void*)h, (void*)g_origProc);
}
