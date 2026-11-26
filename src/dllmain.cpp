// T7Tag: TEKKEN 7 mod loaded as a dinput8.dll proxy (the game imports DINPUT8.dll statically).
#include "common.h"
#include <unknwn.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

HMODULE g_selfModule = nullptr;
static HMODULE g_real = nullptr;

static FARPROC Real(const char* name) {
    if (!g_real) {
        wchar_t sys[MAX_PATH]{};
        GetSystemDirectoryW(sys, MAX_PATH);
        g_real = LoadLibraryW((std::wstring(sys) + L"\\dinput8.dll").c_str());
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

extern "C" {
HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto f = (Fn)Real("DirectInput8Create");
    HRESULT hr = f ? f(inst, ver, riid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) DInputHook(*out);
    return hr;
}
HRESULT WINAPI DllCanUnloadNow() { auto f = (HRESULT(WINAPI*)())Real("DllCanUnloadNow"); return f ? f() : S_FALSE; }
HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, LPVOID* o) {
    auto f = (HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*))Real("DllGetClassObject");
    return f ? f(c, r, o) : E_FAIL;
}
HRESULT WINAPI DllRegisterServer() { auto f = (HRESULT(WINAPI*)())Real("DllRegisterServer"); return f ? f() : E_FAIL; }
HRESULT WINAPI DllUnregisterServer() { auto f = (HRESULT(WINAPI*)())Real("DllUnregisterServer"); return f ? f() : E_FAIL; }
}

static DWORD WINAPI Worker(LPVOID) {
    Log("T7Tag start (build %s %s)", __DATE__, __TIME__);
    FocusInstall();
    TagInit();
    OverlayStart();
    timeBeginPeriod(1);
    for (unsigned n = 0;; ++n) {
        if (n % 60 == 0) FocusTick();
        if (n == 1500) ModelWarmup();    // ~12 s after start: the engine's classes are all registered
        if (n % 30 == 0) NoRageTick();   // both modes; does nothing when NoRageArts=0 or online
        TagTick();                       // tag key needs fast polling
        Sleep(8);
    }
}

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = mod;
        DisableThreadLibraryCalls(mod);
        LogInit(mod);
        ConfigLoad(mod);
        // model swap: detour ProcessEvent now, while the game's main thread is still in the loader
        if (g_cfg.mode == Mode::Tag && g_cfg.modelSwap)
            Log("model: ProcessEvent hook %s", ue::InstallHook() ? "installed (game thread calls ready)" : "NOT installed (unexpected code), model swap off");
        // 11-22 rebuild: the tag logic runs inside the battle's own frame (no writes from our worker thread in a fight)
        if (g_cfg.mode == Mode::Tag && !GameIsOnline()) CostumeInstall();
        if (g_cfg.mode == Mode::Tag)
            Log("frame: battle frame hook %s", ue::InstallFrameHook(TagFrame) ? "installed (tag logic on the game thread)" :
                                                                               "NOT installed (unexpected code): worker-thread fallback");
        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
