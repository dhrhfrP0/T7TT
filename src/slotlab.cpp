// Slot lab (11-22, research for a real second fighter on screen): Tekken 7 keeps 6 player structures 0x3670 apart
// (0 = P1, 1 = P2, 2 / 3 = empty partner slots of team 1 / 2 left from TTT2, 4 / 5 = copies of P1 / P2). The game
// does not update 2 / 3. Debug only, offline, driven from T7Tag.ini [Debug] (F7 reloads, F12 applies):
//   SlotOps=w:slot:off:size:hexvalue;c:src:dst:off:len;...   writes / copies inside the player structures
//   SlotWatch=2,3                                            every 500 ms: how many words of that slot changed
#include "common.h"
#include <vector>
#include <string>
#include <cstring>
#include <tlhelp32.h>

namespace {
// ---- hardware write watch (SlotBp=slot:off): DR0 on every thread of the game; the vectored handler notes who wrote
volatile uintptr_t g_bpAddr = 0;
volatile LONG g_bpHits = 0;
uintptr_t g_bpRip[64], g_bpStack[64][12];

LONG CALLBACK BpHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_bpAddr) return EXCEPTION_CONTINUE_SEARCH;
    if (!(ep->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    LONG n = InterlockedIncrement(&g_bpHits) - 1;
    if (n < 64) {
        g_bpRip[n] = (uintptr_t)ep->ContextRecord->Rip;
        uintptr_t base = GameBase(), rsp = ep->ContextRecord->Rsp;
        int k = 0;
        for (int i = 0; i < 400 && k < 12; ++i) {             // return addresses into the exe found on the stack
            uintptr_t v = 0;
            if (!Rd(rsp + 8 * i, v)) break;
            if (v > base + 0x1000 && v < base + 0x3000000) g_bpStack[n][k++] = v - base;
        }
        for (; k < 12; ++k) g_bpStack[n][k] = 0;
    }
    ep->ContextRecord->Dr6 = 0;
    if (n >= 63) ep->ContextRecord->Dr7 &= ~1ull;             // enough samples: disarm on this thread
    return EXCEPTION_CONTINUE_EXECUTION;
}

void ArmAllThreadsFrom(uintptr_t addr);
// called on the game thread (11-22 frame hook): a thread cannot set its own debug registers while running, so a
// helper thread arms every thread including the caller (which just waits)
void ArmAllThreads(uintptr_t addr) {
    static PVOID veh = nullptr;
    if (!veh) veh = AddVectoredExceptionHandler(1, BpHandler);
    g_bpAddr = addr; g_bpHits = 0;
    HANDLE t = CreateThread(nullptr, 0, [](LPVOID a) -> DWORD { ArmAllThreadsFrom((uintptr_t)a); return 0; }, (LPVOID)addr, 0, nullptr);
    if (t) { WaitForSingleObject(t, 5000); CloseHandle(t); }
}
void ArmAllThreadsFrom(uintptr_t addr) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int armed = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        SuspendThread(t);
        CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            c.Dr0 = addr;
            c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1 | (1ull << 16) | (3ull << 18);   // L0, write, 4 bytes
            if (SetThreadContext(t, &c)) ++armed;
        }
        ResumeThread(t); CloseHandle(t);
    }
    CloseHandle(snap);
    Log("slotlab: write watch on %p armed on %d threads", (void*)addr, armed);
}

void BpReport() {
    static LONG reported = 0;
    LONG n = g_bpHits; if (n > 64) n = 64;
    for (; reported < n; ++reported) {
        char buf[400]; int len = snprintf(buf, sizeof(buf), "slotlab: write #%ld at rip +0x%llX stack", reported,
                                          (unsigned long long)(g_bpRip[reported] - GameBase()));
        for (int k = 0; k < 12 && g_bpStack[reported][k]; ++k)
            len += snprintf(buf + len, sizeof(buf) - len, " %llX", (unsigned long long)g_bpStack[reported][k]);
        Log("%s", buf);
    }
}

std::vector<std::string> Split(const std::string& s, char c) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(c, i); if (j == std::string::npos) j = s.size();
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}
uint64_t Num(const std::string& s) { return strtoull(s.c_str(), nullptr, 0); }

void Apply() {
    for (const std::string& op : Split(g_cfg.slotOps, ';')) {
        std::vector<std::string> f = Split(op, ':');
        if (f.size() >= 5 && f[0] == "w") {
            uintptr_t p = GamePlayer((int)Num(f[1]));
            uint64_t off = Num(f[2]), sz = Num(f[3]), v = Num(f[4]);
            if (!p || off + sz > 0x3670 || !(sz == 1 || sz == 2 || sz == 4 || sz == 8)) { Log("slotlab: bad op %s", op.c_str()); continue; }
            uint64_t old = 0; memcpy(&old, (void*)(p + off), (size_t)sz);
            memcpy((void*)(p + off), &v, (size_t)sz);
            Log("slotlab: slot %s +0x%llX (%llu bytes) 0x%llX -> 0x%llX", f[1].c_str(), off, sz, old, v);
        } else if (f.size() >= 5 && f[0] == "c") {
            uintptr_t a = GamePlayer((int)Num(f[1])), b = GamePlayer((int)Num(f[2]));
            uint64_t off = Num(f[3]), len = Num(f[4]);
            if (!a || !b || off + len > 0x3670) { Log("slotlab: bad op %s", op.c_str()); continue; }
            memcpy((void*)(b + off), (void*)(a + off), (size_t)len);
            Log("slotlab: copied slot %s -> %s +0x%llX..+0x%llX", f[1].c_str(), f[2].c_str(), off, off + len);
        } else if (f.size() >= 2 && f[0] == "spawn") {
            ModelSpawnDouble((int)Num(f[1]), f.size() >= 3 ? (int)Num(f[2]) : -1);
        } else if (f.size() >= 2 && f[0] == "doll") {      // doll:<frames>  P1's main leaves a doll now (doll.cpp)
            DollBegin(0, 0, (int)Num(f[1]));
        } else if (f[0] == "dollstop") {
            DollEnd();
        } else Log("slotlab: unknown op %s", op.c_str());
    }
}

void Watch() {
    static std::vector<uint8_t> last[6];
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (g_cfg.slotWatch.empty() || now < next) return;
    next = now + 500;
    for (const std::string& t : Split(g_cfg.slotWatch, ',')) {
        int k = (int)Num(t);
        if (k < 0 || k > 5) continue;
        uintptr_t p = GamePlayer(k);
        if (!p) continue;
        std::vector<uint8_t> cur((uint8_t*)p, (uint8_t*)p + 0x3670);
        int changed = 0;
        if (last[k].size() == cur.size())
            for (size_t o = 0; o < cur.size(); o += 4) changed += memcmp(&cur[o], &last[k][o], 4) != 0;
        uintptr_t mv = 0, np = 0; Rd(p + Off::CurMove, mv); if (mv) Rd(mv, np);
        char name[48]{}; if (np) { for (int i = 0; i < 47; ++i) { char c = 0; if (!Rd(np + i, c) || !c) break; name[i] = c; } }
        float pos[3]{}; Rd(p + 0xE0, pos);
        int32_t timer = 0; Rd(p + Off::MoveTimer, timer);
        uintptr_t ms = 0; Rd(p + Off::Motbin, ms);
        Log("slotlab: slot %d: %d words changed, move %s frame %d, moveset %p, pos %.0f %.0f %.0f", k, changed, name, timer,
            (void*)ms, pos[0], pos[1], pos[2]);
        last[k].swap(cur);
    }
}
}  // namespace

void SlotLabTick() {
    if (g_cfg.slotOps.empty() && g_cfg.slotWatch.empty() && g_cfg.slotBp.empty()) return;
    static bool was = false;
    bool f12 = GameWindowInFront() && (GetAsyncKeyState(VK_F12) & 0x8000);
    if (f12 && !was && !g_cfg.slotOps.empty()) { Log("slotlab: F12 -> %s", g_cfg.slotOps.c_str()); Apply(); }
    if (f12 && !was && !g_cfg.slotBp.empty()) {             // "slot:off", or "@hexaddr" (any address, research)
        if (g_cfg.slotBp[0] == '@') ArmAllThreads((uintptr_t)Num(g_cfg.slotBp.substr(1)));
        else {
            std::vector<std::string> f = Split(g_cfg.slotBp, ':');
            uintptr_t p = f.size() == 2 ? GamePlayer((int)Num(f[0])) : 0;
            if (p) ArmAllThreads(p + Num(f[1]));
        }
    }
    BpReport();
    was = f12;
    Watch();
}
