// Black box (11-23): the last 15 s of the fight, frame by frame. F8 (game window in front) writes them to
// T7Tag_mark_<time>.txt next to the log, with the log's last lines: the player marks "that was wrong" in one key.
#include "common.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Frame { uint32_t n; char mv[2][24]; int timer[2], hit[2]; float pos[2][3]; uint8_t keys, active, assault, doll; };
constexpr int kMax = 900;
std::vector<Frame> g_ring(kMax);
uint32_t g_n = 0;

void Save() {
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe); dir = dir.substr(0, dir.find_last_of((wchar_t)92) + 1);
    wchar_t name[80]; swprintf_s(name, L"T7Tag_mark_%02d%02d_%02d%02d%02d.txt", t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + name).c_str(), L"wb") || !f) { Log("mark: cannot write the file"); return; }
    fprintf(f, "frame keys active assault doll | P1 move timer x y z | P2 move timer x y z | distance  (keys: W A S D U I J K Space)\r\n");
    uint32_t from = g_n > (uint32_t)kMax ? g_n - kMax : 0;
    for (uint32_t i = from; i < g_n; ++i) {
        const Frame& r = g_ring[i % kMax];
        float dx = r.pos[0][0] - r.pos[1][0], dz = r.pos[0][2] - r.pos[1][2];
        char k[10]; const char* names = "WASDUIJK_";
        for (int b = 0; b < 8; ++b) k[b] = (r.keys >> b) & 1 ? names[b] : '.';
        k[8] = r.active & 0x80 ? '_' : '.'; k[9] = 0;
        fprintf(f, "%u %s %d %d %d | %-22s %5d %8.0f %6.0f %8.0f | %-22s %5d %8.0f %6.0f %8.0f | %6.0f hit %d %d\r\n", r.n, k, r.active & 0x7F, r.assault, r.doll,
                r.mv[0], r.timer[0], r.pos[0][0], r.pos[0][1], r.pos[0][2], r.mv[1], r.timer[1], r.pos[1][0], r.pos[1][1], r.pos[1][2],
                sqrtf(dx * dx + dz * dz), r.hit[0], r.hit[1]);
    }
    fclose(f);
    Log("mark: ======== F8: the player marked this moment (last %u frames in the mark file) ========", g_n - from);
    OverlayToast(L"기록됨 (F8)", 1500);
}
}  // namespace

// game thread, every battle frame
void RecorderFrame(int active, bool assault, bool doll) {
    Frame& r = g_ring[g_n % kMax];
    r.n = g_n++;
    for (int k = 0; k < 2; ++k) {
        uintptr_t p = GamePlayer(k), mv = 0, np = 0;
        r.mv[k][0] = 0; r.timer[k] = 0;
        if (!p) continue;
        Rd(p + Off::CurMove, mv); if (mv) Rd(mv, np);
        std::string s = np ? ReadCStr(np, 23) : "";
        strncpy_s(r.mv[k], s.c_str(), _TRUNCATE);
        Rd(p + Off::MoveTimer, r.timer[k]); Rd(p + 0xF4, r.pos[k]); Rd(p + Off::LastHit, r.hit[k]);
    }
    r.keys = 0;
    bool front = GameWindowInFront();
    static const int vks[8] = {'W', 'A', 'S', 'D', 'U', 'I', 'J', 'K'};
    for (int b = 0; front && b < 8; ++b) if (GetAsyncKeyState(vks[b]) & 0x8000) r.keys |= (uint8_t)(1 << b);
    unsigned short pb = PadButtons(0);                          // controller: D-pad / stick up left down right, X Y A B
    static const unsigned short pm[8] = {0x0001, 0x0004, 0x0002, 0x0008, 0x4000, 0x8000, 0x1000, 0x2000};
    for (int b = 0; b < 8; ++b) if (pb & pm[b]) r.keys |= (uint8_t)(1 << b);
    r.active = (uint8_t)((active & 0x7F) | (front && ((GetAsyncKeyState(g_cfg.tagKey) & 0x8000) || PadTagDown(0)) ? 0x80 : 0));
    r.assault = assault; r.doll = doll;
    static bool was = false;
    bool f8 = front && (GetAsyncKeyState(VK_F8) & 0x8000);
    if (f8 && !was) Save();
    was = f8;
}
