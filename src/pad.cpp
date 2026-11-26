// Controllers (11-24): pads and sticks that Windows shows as XInput (Xbox pads, most arcade sticks, PS pads through Steam
// Input). The game imports XInputGetState by ordinal 2; every poll passes here (focus.cpp).
//   fight : the tag button (PadTag, default LB) is the tag key and is taken out of what the game sees; X / Y / A / B count
//           as the attack buttons for the early end of an entry, Y as the right punch of the tag crash.
//   select: A = Enter, B = Esc, X = the costume list key, D-pad / left stick = arrows, through the same steps as the
//           keyboard; a press that belongs to us is hidden from the game until it is released.
// First connected controller = P1, the second = P2.
#include "common.h"
#include <Xinput.h>
#include <cstring>

namespace {
struct Pad {
    WORD now = 0, hidden = 0, synthMask = 0;      // buttons (stick directions included as D-pad bits)
    int synth = 0;                                // polls left: 4 = released once, 3..1 = pressed
    bool stickHidden = false;
};
Pad g_pad[2];
int g_user[2] = {-1, -1};
WORD g_tagMask = XINPUT_GAMEPAD_LEFT_SHOULDER;
volatile LONG g_tag[2] = {0, 0};
constexpr WORD kDirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;

struct Map { WORD bit; int vk; };
const Map kSelect[] = {{XINPUT_GAMEPAD_A, VK_RETURN}, {XINPUT_GAMEPAD_B, VK_ESCAPE}, {XINPUT_GAMEPAD_X, 'U'}, {XINPUT_GAMEPAD_Y, 'O'},
                       {XINPUT_GAMEPAD_DPAD_UP, VK_UP}, {XINPUT_GAMEPAD_DPAD_DOWN, VK_DOWN},
                       {XINPUT_GAMEPAD_DPAD_LEFT, VK_LEFT}, {XINPUT_GAMEPAD_DPAD_RIGHT, VK_RIGHT}};

int SideOf(DWORD user) {
    for (int s = 0; s < 2; ++s) if (g_user[s] == (int)user) return s;
    for (int s = 0; s < 2; ++s) if (g_user[s] < 0) { g_user[s] = (int)user; Log("pad: controller %lu is P%d", user, s + 1); return s; }
    return -1;
}

// test rig only ([Debug] PadTest=1): a shared word of buttons stands in for controller 0
bool TestState(DWORD user, XINPUT_STATE* st) {
    if (!g_cfg.padTest || user != 0) return false;
    static volatile DWORD* mem = nullptr;
    if (!mem) {
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64, L"T7TTPadTest");
        mem = h ? (volatile DWORD*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 64) : nullptr;
        if (!mem) return false;
    }
    static DWORD packet = 0;
    memset(st, 0, sizeof *st);
    st->dwPacketNumber = ++packet;
    st->Gamepad.wButtons = (WORD)mem[0];
    return true;
}
}  // namespace

WORD PadMaskFromName(const char* n) {
    struct { const char* n; WORD m; } t[] = {{"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {"LS", XINPUT_GAMEPAD_LEFT_THUMB}, {"RS", XINPUT_GAMEPAD_RIGHT_THUMB}, {"BACK", XINPUT_GAMEPAD_BACK}};
    for (auto& e : t) if (!_stricmp(n, e.n)) return e.m;
    return 0;
}
void PadConfig(const char* tagName) { g_tagMask = PadMaskFromName(tagName); }
bool PadTagDown(int side) { return side >= 0 && side < 2 && g_tag[side]; }
// the attack button held: 'U' 'I' 'J' 'K' like the keyboard (X Y A B), 0 = none
int PadAttack(int side) {
    if (side < 0 || side > 1) return 0;
    WORD b = g_pad[side].now;
    return b & XINPUT_GAMEPAD_X ? 'U' : b & XINPUT_GAMEPAD_Y ? 'I' : b & XINPUT_GAMEPAD_A ? 'J' : b & XINPUT_GAMEPAD_B ? 'K' : 0;
}
WORD PadButtons(int side) { return side >= 0 && side < 2 ? g_pad[side].now : 0; }
void PadSynthPress(int side, int vk) {
    if (side < 0 || side > 1) return;
    g_pad[side].synthMask = vk == 'U' ? XINPUT_GAMEPAD_X : vk == 'I' ? XINPUT_GAMEPAD_Y : vk == 'J' ? XINPUT_GAMEPAD_A : XINPUT_GAMEPAD_B;
    g_pad[side].synth = 4;
}

DWORD PadPoll(DWORD user, XINPUT_STATE* st, DWORD r) {
    if (!st) return r;
    if (TestState(user, st)) r = ERROR_SUCCESS;
    if (r != ERROR_SUCCESS) {
        for (int s = 0; s < 2; ++s) if (g_user[s] == (int)user) { g_user[s] = -1; g_pad[s] = Pad{}; g_tag[s] = 0; }
        return r;
    }
    int s = SideOf(user);
    if (s < 0) return r;
    Pad& p = g_pad[s];
    XINPUT_GAMEPAD& g = st->Gamepad;
    WORD b = g.wButtons;
    constexpr int dead = 16000;
    WORD stick = (g.sThumbLY > dead ? XINPUT_GAMEPAD_DPAD_UP : 0) | (g.sThumbLY < -dead ? XINPUT_GAMEPAD_DPAD_DOWN : 0) |
                 (g.sThumbLX < -dead ? XINPUT_GAMEPAD_DPAD_LEFT : 0) | (g.sThumbLX > dead ? XINPUT_GAMEPAD_DPAD_RIGHT : 0);
    WORD all = b | stick, down = all & ~p.now;
    p.now = all;
    p.hidden &= all;                                           // released: the game sees the next press again
    if (g_cfg.mode != Mode::Tag || GameIsOnline()) { g_tag[s] = 0; return r; }
    if (TagInFight()) {
        bool t = (b & g_tagMask) != 0;
        g_tag[s] = t;
        if (g_tagMask && TagSide(s)) g.wButtons &= ~g_tagMask;
        p.hidden = 0;
        if (p.synth > 0) {
            if (p.synth == 4) g.wButtons &= ~p.synthMask; else g.wButtons |= p.synthMask;
            --p.synth;
        }
    } else {
        g_tag[s] = 0;
        for (const Map& m : kSelect)
            if ((down & m.bit) && PickPadKey(m.vk)) p.hidden |= m.bit;
        g.wButtons &= ~p.hidden;
        if (p.hidden & kDirs & stick) { g.sThumbLX = 0; g.sThumbLY = 0; }
    }
    return r;
}
