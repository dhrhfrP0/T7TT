// Minimal UE4 (4.14) reflection + game-thread calls for this TEKKEN 7 build, all found live on 11-21 with the read-only
// tools/ue_dump.py:
//   GUObjectArray 0x143693550 (+0x10: FUObjectItem* (24 bytes each: object, flags), max, num)
//   GNames        [0x1434CDF70] -> chunks of 16384 FNameEntry* (name text at +0x10, Index&1 = wide)
//   UObject       +0x10 class, +0x18 FName, +0x20 outer, +0xC internal index
//   UStruct       +0x30 super, +0x38 children (UField::Next +0x28), +0x40 properties size
//   UProperty     +0x34 element size, +0x50 offset;  UFunction +0x88 flags
//   ProcessEvent  0x140936B80 (UObject vtable slot 55): detoured so our calls run on the game thread
// Every call into the engine happens from that detour, never from our worker thread.
#include "common.h"
#include <mutex>
#include <vector>
#include <functional>
#include <cstring>
#include <unordered_map>

namespace {
constexpr uintptr_t kGObjects = 0x143693550, kGNames = 0x1434CDF70, kProcessEvent = 0x140936B80;
constexpr uint8_t kPEPrologue[12] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

uintptr_t Va(uintptr_t va) { return GameBase() + va - 0x140000000; }

using FnPE = void(__fastcall*)(uintptr_t self, uintptr_t fn, void* parms);
FnPE g_origPE = nullptr;          // trampoline
DWORD g_gameThread = 0;
std::mutex g_qmx;
std::vector<std::function<void()>> g_queue;
volatile LONG g_pending = 0;
thread_local bool t_inHook = false;

void RunQueue() {
    std::vector<std::function<void()>> q;
    { std::lock_guard<std::mutex> lk(g_qmx); q.swap(g_queue); InterlockedExchange(&g_pending, 0); }
    for (auto& f : q) f();
}

// research (11-23): for a few seconds, log the costume related events that go through ProcessEvent
volatile ULONGLONG g_peTraceUntil = 0;
void PeTrace(uintptr_t self, uintptr_t fn, void* parms);
void __fastcall HkProcessEvent(uintptr_t self, uintptr_t fn, void* parms) {
    if (g_peTraceUntil && GetTickCount64() < g_peTraceUntil && !t_inHook) { t_inHook = true; PeTrace(self, fn, parms); t_inHook = false; }
    if (g_pending && !t_inHook && GetCurrentThreadId() == g_gameThread) {
        t_inHook = true;
        RunQueue();
        t_inHook = false;
    }
    g_origPE(self, fn, parms);
}

// ---- battle frame hook (11-22 rebuild): 0x1406DDBD0 = the battle's per-frame update, called on the game thread with
// (cl = run flag, xmm1 = frame time). Its first 12 bytes: sub rsp,58h / test cl,cl / je rel32 -> the trampoline
// replays them with the je made absolute. After every original call our frame callback runs on the same thread,
// between two battle updates: tag logic writes the player structures when nothing else touches them.
constexpr uintptr_t kBattleFrame = 0x1406DDBD0;
using FnFrame = void(__fastcall*)(uint64_t flag, float dt);
FnFrame g_origFrame = nullptr;
void (*g_frameCb)() = nullptr;
volatile LONG g_frameFaults = 0;

int FrameFault(EXCEPTION_POINTERS* ep) {
    if (InterlockedIncrement(&g_frameFaults) <= 5) {
        uintptr_t base = GameBase(), rip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress, rsp = ep->ContextRecord->Rsp;
        char st[200]; int len = 0; st[0] = 0;
        for (int i = 0, k = 0; i < 300 && k < 10; ++i) {          // exe return addresses on the stack (RVA)
            uintptr_t v = 0;
            if (!Rd(rsp + 8 * i, v)) break;
            if (v > base + 0x1000 && v < base + 0x3000000) { len += snprintf(st + len, sizeof(st) - len, " %llX", (unsigned long long)(v - base)); ++k; }
        }
        Log("frame: exception %08lX at %p (exe+%llX) in the tag frame callback (skipped), stack%s", ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress, (unsigned long long)(rip - base), st);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
void RunFrameCb() { __try { g_frameCb(); } __except (FrameFault(GetExceptionInformation())) {} }

void __fastcall HkFrame(uint64_t flag, float dt) {
    g_origFrame(flag, dt);
    if (g_frameCb && (flag & 0xFF) && GetCurrentThreadId() == g_gameThread) RunFrameCb();
}

bool CallGuarded(uintptr_t obj, uintptr_t fn, void* parms) {
    __try { g_origPE(obj, fn, parms); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}  // namespace

namespace ue {

// called from DllMain: the main thread (= UE4 game thread) is still in the loader, so patching is safe
bool InstallHook() {
    g_gameThread = GetCurrentThreadId();
    auto at = (uint8_t*)Va(kProcessEvent);
    if (memcmp(at, kPEPrologue, sizeof(kPEPrologue)) != 0) return false;
    auto tr = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    memcpy(tr, at, 12);
    uint8_t back[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};   // mov rax, at+12 ; jmp rax
    uintptr_t cont = (uintptr_t)at + 12;
    memcpy(back + 2, &cont, 8);
    memcpy(tr + 12, back, 12);
    g_origPE = (FnPE)tr;
    uint8_t jmp[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    void* target = (void*)HkProcessEvent;
    memcpy(jmp + 2, &target, 8);
    DWORD old;
    if (!VirtualProtect(at, 12, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, jmp, 12);
    VirtualProtect(at, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 12);
    return true;
}

bool InstallFrameHook(void (*cb)()) {
    auto at = (uint8_t*)Va(kBattleFrame);
    static const uint8_t pro[8] = {0x48, 0x83, 0xEC, 0x58, 0x84, 0xC9, 0x0F, 0x84};
    if (memcmp(at, pro, sizeof(pro)) != 0) return false;
    int32_t rel = 0; memcpy(&rel, at + 8, 4);
    uintptr_t jeTarget = (uintptr_t)at + 12 + rel, cont = (uintptr_t)at + 12;
    auto tr = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    uint8_t code[] = {0x48, 0x83, 0xEC, 0x58,                   // sub rsp, 58h
                      0x84, 0xC9,                               // test cl, cl
                      0x74, 0x0E,                               // je +14 (to the second jump)
                      0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   // jmp [rip] -> cont
                      0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // jmp [rip] -> je target
    memcpy(code + 14, &cont, 8);
    memcpy(code + 28, &jeTarget, 8);
    memcpy(tr, code, sizeof(code));
    g_origFrame = (FnFrame)tr;
    g_frameCb = cb;
    uint8_t jmp[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    void* target = (void*)HkFrame;
    memcpy(jmp + 2, &target, 8);
    DWORD old;
    if (!VirtualProtect(at, 12, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, jmp, 12);
    VirtualProtect(at, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 12);
    return true;
}
bool FrameHookReady() { return g_origFrame != nullptr; }

bool HookReady() { return g_origPE != nullptr; }
bool IsGameThread() { return GetCurrentThreadId() == g_gameThread; }

void RunOnGameThread(std::function<void()> f) {
    if (GetCurrentThreadId() == g_gameThread && g_origPE && !t_inHook) { f(); return; }   // already there (frame hook)
    std::lock_guard<std::mutex> lk(g_qmx);
    g_queue.push_back(std::move(f));
    InterlockedExchange(&g_pending, 1);
}

// ---- objects and names
uintptr_t ObjectAt(int i);
int NumObjects() { int n = 0; Rd(Va(kGObjects) + 0x10 + 0xC, n); return n; }
uintptr_t ObjectAtIndex(int i) { return i >= 0 && i < NumObjects() ? ObjectAt(i) : 0; }
uintptr_t ObjectAt(int i) {
    uintptr_t arr = 0, o = 0;
    if (!Rd(Va(kGObjects) + 0x10, arr) || !arr) return 0;
    Rd(arr + (uintptr_t)i * 24, o);
    return o;
}
int ItemFlags(uintptr_t obj) {
    int idx = -1, fl = 0; uintptr_t arr = 0;
    if (!Rd(obj + 0xC, idx) || idx < 0 || !Rd(Va(kGObjects) + 0x10, arr)) return 0;
    Rd(arr + (uintptr_t)idx * 24 + 8, fl);
    return fl;
}
void SetRoot(uintptr_t obj, bool on) {
    if (!obj) return;
    int idx = -1; uintptr_t arr = 0;
    if (!Rd(obj + 0xC, idx) || idx < 0 || !Rd(Va(kGObjects) + 0x10, arr)) return;
    uintptr_t at = arr + (uintptr_t)idx * 24 + 8;
    int fl = 0; Rd(at, fl);
    fl = on ? (fl | (1 << 30)) : (fl & ~(1 << 30));      // EInternalObjectFlags::RootSet
    Wr(at, fl);
}
bool PendingKill(uintptr_t obj) { return (ItemFlags(obj) & ((1 << 29) | (1 << 28))) != 0; }

std::string NameOfIndexRaw(int idx);
std::string NameOfIndex(int idx) {          // names never change: cached (game thread only)
    static std::unordered_map<int, std::string> cache;
    auto it = cache.find(idx);
    if (it != cache.end()) return it->second;
    std::string s = NameOfIndexRaw(idx);
    if (!s.empty()) cache.emplace(idx, s);
    return s;
}
std::string NameOfIndexRaw(int idx) {
    uintptr_t names = 0, chunk = 0, e = 0;
    if (idx < 0 || !Rd(Va(kGNames), names) || !names) return {};
    if (!Rd(names + 8 * (uintptr_t)(idx / 16384), chunk) || !chunk) return {};
    if (!Rd(chunk + 8 * (uintptr_t)(idx % 16384), e) || !e) return {};
    int hdr = 0; Rd(e, hdr);
    if (hdr & 1) {                                     // wide: convert roughly
        wchar_t w[64]{}; SafeRead(e + 0x10, w, sizeof(w) - 2);
        char a[64]{}; WideCharToMultiByte(CP_UTF8, 0, w, -1, a, 63, nullptr, nullptr);
        return a;
    }
    return ReadCStr(e + 0x10, 63);
}
std::string Name(uintptr_t obj) {
    int idx = -1, num = 0;
    if (!obj || !Rd(obj + 0x18, idx) || !Rd(obj + 0x1C, num)) return {};
    std::string s = NameOfIndex(idx);
    if (num) s += "_" + std::to_string(num - 1);
    return s;
}
uintptr_t Class(uintptr_t obj) { uintptr_t c = 0; Rd(obj + 0x10, c); return c; }
uintptr_t Outer(uintptr_t obj) { uintptr_t c = 0; Rd(obj + 0x20, c); return c; }
std::string ClassName(uintptr_t obj) { return Name(Class(obj)); }

// first object named `name` whose class is `cls` and (if given) whose outer is named `outer`
uintptr_t Find(const char* cls, const char* outer, const char* name) {
    int n = NumObjects();
    for (int i = 0; i < n; ++i) {
        uintptr_t o = ObjectAt(i);
        if (!o || Name(o) != name) continue;
        if (cls && ClassName(o) != cls) continue;
        if (outer && Name(Outer(o)) != outer) continue;
        return o;
    }
    return 0;
}
uintptr_t Function(const char* cls, const char* fn) { return Find("Function", cls, fn); }

// a live instance of a class (not the class default object, not being destroyed)
uintptr_t Instance(const char* cls) {
    int n = NumObjects();
    uintptr_t best = 0;
    for (int i = 0; i < n; ++i) {
        uintptr_t o = ObjectAt(i);
        if (!o || ClassName(o) != cls) continue;
        std::string nm = Name(o);
        if (nm.rfind("Default__", 0) == 0 || PendingKill(o)) continue;
        best = o;                                       // the newest one wins (later in the array)
    }
    return best;
}

// offset of a reflected property (searches the class and its supers)
int PropOffset(uintptr_t st, const char* prop) {
    for (int depth = 0; st && depth < 16; ++depth) {
        uintptr_t c = 0; Rd(st + 0x38, c);
        for (int k = 0; c && k < 1000; ++k) {
            if (Name(c) == prop && ClassName(c) != "Function") { int off = -1; Rd(c + 0x50, off); return off; }
            uintptr_t nx = 0; Rd(c + 0x28, nx); c = nx;
        }
        uintptr_t sup = 0; Rd(st + 0x30, sup); st = sup;
    }
    return -1;
}
int ParmsSize(uintptr_t fn) { int n = 0; Rd(fn + 0x40, n); return n; }

// call a UFunction on obj with a parameter buffer (game thread only)
bool Call(uintptr_t obj, uintptr_t fn, void* parms) {
    if (!obj || !fn || !g_origPE || !IsGameThread()) return false;
    return CallGuarded(obj, fn, parms);
}
}  // namespace ue

void PeTraceStart(int ms) { g_peTraceUntil = GetTickCount64() + ms; }
namespace {
void PeTrace(uintptr_t self, uintptr_t fn, void* parms) {
    std::string n = ue::Name(fn);
    static const char* keys[] = {"Item", "Costume", "Custom", "Equip", "Load", "Set"};
    bool hit = false;
    for (const char* k : keys) if (n.find(k) != std::string::npos) hit = true;
    if (!hit || n.find("Tick") != std::string::npos) return;
    static std::unordered_map<std::string, int> seen;
    if (++seen[n] > 6) return;
    int sz = ue::ParmsSize(fn); std::string ps;
    for (int o = 0; o + 8 <= sz && o < 64; o += 8) {
        uintptr_t v = 0; Rd((uintptr_t)parms + o, v); char b[40];
        snprintf(b, sizeof b, " %llX", (unsigned long long)v); ps += b;
    }
    if (n == "SetAsyncLoadItemSet") {
        void* st[24]{}; int k = CaptureStackBackTrace(0, 24, st, nullptr); uintptr_t base = GameBase(); ps += " | stack";
        for (int i = 0; i < k; ++i) { char b[24]; uintptr_t v = (uintptr_t)st[i]; if (v > base && v < base + 0x4000000) { snprintf(b, sizeof b, " %llX", (unsigned long long)(v - base)); ps += b; } }
    }
    Log("petrace: %s.%s (%d bytes)%s", ue::ClassName(self).c_str(), n.c_str(), sz, ps.c_str());
}
}
