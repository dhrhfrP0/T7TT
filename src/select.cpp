// Character select integration, TTT2 flow (11-21): TAG or SOLO is chosen first (tag key toggles, shown as TTT2's
// two-person / one-person icons). In TAG the first confirm press picks the MAIN (point, also the model) and is hidden
// from the game, so the cursor stays; the second confirm is the game's own = the PARTNER. The game's character
// setters are replaced so the confirmed id becomes the partner and the main is what gets loaded.
//
// Patched functions (TekkenGame-Win64-Shipping.exe, same targets T7Hook uses for its character swap):
//   SetChar A   89 51 10 89 91 A4 C0 00 00 C3   mov [rcx+10h],edx ; mov [rcx+0C0A4h],edx ; ret   (+6 int3)
//   SetChar B   89 51 14 C3                     mov [rcx+14h],edx ; ret                          (+12 int3, = A+0x10)
//   CharInfoSetID 48 8B 81 60 15 00 00 89 91 DC 00 00 00 ...   (43 bytes) per-player id incl. two sub-objects
// Each one is replaced by a 12-byte absolute jump to a function that does the same stores with the id rewritten.
#include "common.h"
#include <vector>
#include <cstring>

SelectState g_sel;

namespace {
uintptr_t g_playerList = 0;      // from GetPlayerFromID: lea rcx,[rip+X]
int g_logCalls = 0;

struct Section { uintptr_t start, size; };
Section TextSection() {
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (!memcmp(sec->Name, ".text", 5)) return {(uintptr_t)base + sec->VirtualAddress, sec->Misc.VirtualSize};
    return {0, 0};
}

// pattern with -1 wildcards; returns all matches (max 4)
std::vector<uintptr_t> Find(std::initializer_list<int> pat) {
    std::vector<int> p(pat);
    std::vector<uintptr_t> out;
    Section t = TextSection();
    auto mem = (const uint8_t*)t.start;
    for (uintptr_t i = 0; i + p.size() <= t.size && out.size() < 4; ++i) {
        size_t k = 0;
        for (; k < p.size(); ++k) if (p[k] >= 0 && mem[i + k] != p[k]) break;
        if (k == p.size()) out.push_back(t.start + i);
    }
    return out;
}

bool PatchJump(uintptr_t at, void* target, size_t room) {
    if (room < 12) return false;
    uint8_t code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};   // mov rax, imm64 ; jmp rax
    memcpy(code + 2, &target, 8);
    DWORD old;
    if (!VirtualProtect((void*)at, 12, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)at, code, 12);
    VirtualProtect((void*)at, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 12);
    return true;
}

volatile uintptr_t g_selPtr[2] = {0, 0};   // per-side select struct seen by the setters (+0x10 = cursor)

// T7Hook's mapping for the two setters: the int at +8 tells the side (1 = P1, 0 = P2).
int SideFromFlag(int flag) { return flag == 1 ? 0 : flag == 0 ? 1 : -1; }

void Trace(const char* who, int side, int flag, int id) {
    if (g_logCalls < 80) {
        ++g_logCalls;
        Log("select: %s side=%d flag=%d id=%d armed=%d first=%d partner=%d", who, side, flag, id,
            side >= 0 ? g_sel.armed[side] : -1, side >= 0 ? g_sel.firstId[side] : -1, side >= 0 ? g_sel.partnerId[side] : -1);
    }
}

bool Active(int side) { return side >= 0 && side <= 1 && g_sel.armed[side] && !GameIsOnline() && g_cfg.mode == Mode::Tag; }

// setA runs at each confirm (Enter) and once more at the final commit (11-21: not on cursor moves). Never rewritten.
void HkSetCharA(uint8_t* p, int id) {
    int flag = *(int*)(p + 8), side = SideFromFlag(flag);
    Trace("setA", side, flag, id);
    if (side >= 0 && id >= 0 && id <= 60) {
        g_selPtr[side] = (uintptr_t)p;
        g_sel.hoverId[side] = id; g_sel.hoverAt[side] = GetTickCount64(); g_sel.visited[side] = true;
        PickConfirmed(side, id);
    }
    *(int*)(p + 0x10) = id;
    *(int*)(p + 0xC0A4) = id;
}

// setB only runs at the final commit: the confirmed character is the partner, the first pick is what loads.
void HkSetCharB(uint8_t* p, int id) {
    int flag = *(int*)(p + 8), side = SideFromFlag(flag);
    Trace("setB", side, flag, id);
    g_sel.done = true;
    if (Active(side)) {
        if (g_sel.partnerId[side] < 0 && id >= 0 && id <= 60) g_sel.partnerId[side] = id;
        id = g_sel.firstId[side];
        *(int*)(p + 0x10) = id;
        *(int*)(p + 0xC0A4) = id;
    }
    *(int*)(p + 0x14) = id;
}

void HkCharInfoSetID(uint8_t* ci, int id) {
    int side = (uintptr_t)ci == PlayerFromList(0) ? 0 : (uintptr_t)ci == PlayerFromList(1) ? 1 : -1;
    Trace("charInfo", side, -1, id);
    if (Active(side) && id >= 0 && id <= 60) id = g_sel.firstId[side];
    uint8_t* a = *(uint8_t**)(ci + 0x1560);
    *(int*)(ci + 0xDC) = id;
    if (a) *(int*)(a + 0xDC) = id;
    uint8_t* b = *(uint8_t**)(ci + 0x1558);
    if (b) *(int*)(b + 0xDC) = id;
}

// code -> Korean name, id -> code is filled by the tag module from the .t7p headers
struct Named { int id; const char* name; };
const Named kNames[] = {
    {0, "폴"}, {1, "로우"}, {2, "킹"}, {3, "요시미츠"}, {4, "화랑"}, {5, "샤오유"}, {6, "진"}, {7, "브라이언"},
    {8, "헤이하치"}, {9, "카즈야"}, {10, "스티브"}, {11, "잭-7"}, {12, "아스카"}, {13, "데빌 진"}, {14, "펭"},
    {15, "릴리"}, {16, "드라구노프"}, {17, "레오"}, {18, "라스"}, {19, "알리사"}, {20, "클라우디오"},
    {21, "카타리나"}, {22, "럭키 클로이"}, {23, "샤힌"}, {24, "조시"}, {25, "기가스"}, {26, "카즈미"},
    {27, "데빌 카즈미"}, {28, "니나"}, {29, "마스터 레이븐"}, {30, "리"}, {31, "밥"}, {32, "고우키"},
    {33, "쿠마"}, {34, "판다"}, {35, "에디"}, {36, "엘리자"}, {37, "미겔"}, {39, "영 카즈야"}, {40, "잭-4"},
    {41, "영 헤이하치"}, {43, "기스"}, {44, "녹티스"}, {45, "안나"}, {46, "레이"}, {47, "아머킹"}, {48, "마두크"},
    {49, "줄리아"}, {50, "니건"}, {51, "자피나"}, {52, "간류"}, {53, "르로이"}, {54, "파쿰람"}, {55, "쿠니미츠"},
    {56, "리디아"},
};
}  // namespace

const char* CharName(int id) {
    for (auto& n : kNames) if (n.id == id) return n.name;
    return nullptr;
}

uintptr_t PlayerFromList(int side) {
    if (!g_playerList) return 0;
    uintptr_t p = 0;
    Rd(g_playerList + side * 8, p);
    return p;
}

// Hovered character at the select screen. Prefer what the setters reported recently; fall back to the select
// structure TekkenBot reads ([base+0x33B4E68]+0x80, P1 +0x3CC / P2 +0x584).
int SelectCursorId(int side) {
    int id = -1;
    if (g_selPtr[side] && Rd(g_selPtr[side] + 0x10, id) && id >= 0 && id <= 60) return id;
    if (g_sel.hoverId[side] >= 0 && GetTickCount64() - g_sel.hoverAt[side] < 3000) return g_sel.hoverId[side];
    uintptr_t a = 0, b = 0;
    if (Rd(GameBase() + 0x33B4E68, a) && a && Rd(a + 0x80, b) && b && Rd(b + (side ? 0x584 : 0x3CC), id) && id >= 0 && id <= 60)
        return id;
    return g_sel.hoverId[side];
}

// ---------------------------------------------------------------- TTT2 pick flow (11-21 rewrite, measured live)
// Game facts (read-only probes with tools/selstate.py, keyboard WASD / Enter / Esc):
//   [0x1434D65C0] = select-screen object: +0x60 frame counter (moves only while the select screen takes input; a
//   dialog stops it), +0x508 P1 cursor character, +0x948 P2 cursor character (255 = no cursor: other screens).
//   Practice: P2's cursor appears only after P1 confirmed; Esc while P2 picks takes P1's confirm back (P2 -> 255).
//   Esc with nothing confirmed opens "back to side select?" (counter stops). setA runs at each confirm (Enter).
// Per side: MODE (TTT2 TAG/SOLO icons: A/Left = TAG, D/Right = SOLO, Enter = OK) -> TAG: MAIN (Enter = main, the
// press never reaches the game) -> PARTNER (Enter = the game's confirm) -> DONE.   SOLO: SOLO (game confirm) -> DONE.
// Esc goes one step back inside our steps (never reaching the game); from MODE / DONE it is the game's own Esc.
namespace {
volatile uint8_t g_held[256];              // 0 = free, 1 = hidden until released, 2 = passed until released
SRWLOCK g_lk = SRWLOCK_INIT;
volatile ULONGLONG g_cntMoved = 0;
int g_lastCnt = -1;
volatile int g_hover[2] = {255, 255};
volatile bool g_active = false;            // select screen takes input (counter moving, a cursor exists)
bool g_live = false;                       // a select session is running

uintptr_t SelObj() { uintptr_t o = 0; Rd(GameBase() + 0x34D65C0, o); return o; }

bool IsDir(int vk) {
    switch (vk) {
    case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case 'W': case 'A': case 'S': case 'D': return true;
    }
    return false;
}
// keys that confirm on the select screen (anything but movement / Esc / system / our tag keys)
bool Confirmish(int vk) {
    if (vk < 8 || vk > 0xFE || IsDir(vk)) return false;
    switch (vk) {
    case VK_ESCAPE: case VK_BACK: case VK_TAB: case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: case VK_CONTROL:
    case VK_LCONTROL: case VK_RCONTROL: case VK_MENU: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL: case VK_SNAPSHOT: case VK_PAUSE: case VK_APPS:
        return false;
    }
    if (vk >= VK_F1 && vk <= VK_F24) return false;
    if (vk == g_cfg.tagKey || vk == g_cfg.p2TagKey) return false;
    return true;
}

void ToMode(int s) {
    g_sel.phase[s] = PH_MODE; g_sel.armed[s] = false; g_sel.firstId[s] = -1; g_sel.partnerId[s] = -1; g_sel.mainSlot[s] = -1;
    g_sel.mainBoss[s] = -1; g_sel.partnerBoss[s] = false; g_sel.bossHover[s] = -1;
}

// 11-23 costume slots (user: "the skin key just moves on"). The game's costume list (Default 1-4, Preset 1.., then the
// player's own), measured on the select object (P2 +0x440): byte +0x503 = 1 while the list is open, +0x4CC = its
// position (0-3 Default 1-4, 4.. Preset 1..; it stays after the list closes). Our main pick hides its Enter from the
// game, so the main's list choice is kept here; the partner's is read at the game's own confirm.
constexpr uintptr_t kListOpen = 0x503, kListPos = 0x4CC, kListTotal = 0x4D8, kSideStride = 0x440;   // total: the last 10 = Custom 1-10
bool ListOpen(int s) { uintptr_t o = SelObj(); uint8_t v = 0; if (o) Rd(o + kListOpen + kSideStride * s, v); return o && v != 0; }
int ListCursor(int s) { uintptr_t o = SelObj(); int v = -1; if (o) Rd(o + kListPos + kSideStride * s, v); return v; }
bool CostumeKey(int vk) { return vk == 'U' || vk == 'I'; }   // the game's "customize slot" button on keyboards

// 11-26 O (pad Y) on a character that has a boss version: switch it on / off for this pick
bool BossToggle(int s) {
    uintptr_t o = SelObj();
    int id = 255;
    if (o) Rd(o + 0x508 + 0x440 * s, id);
    const wchar_t* boss = id >= 0 && id <= 60 ? BossName(id) : nullptr;
    if (!boss) { OverlayToast(L"이 캐릭터는 보스판이 없습니다", 1500); return true; }
    bool on = g_sel.bossHover[s] != id;
    g_sel.bossHover[s] = on ? id : -1;
    wchar_t msg[96];
    swprintf_s(msg, L"%s %s", boss, on ? L"켜짐 (O로 끄기)" : L"꺼짐");
    OverlayToast(msg, 2500);
    UiSound("SE_cmn_Mainmenu_Cursor|SE_Cursor|Cursor_Menu_01");
    Log("select: P%d boss version of %d %s", s + 1, id, on ? "ON" : "off");
    return true;
}

// one key going down on the select screen: returns true = hide it from the game (state already changed)
bool OnKey(int vk) {
    int s = PickSide();
    if (s < 0) return false;
    int ph = g_sel.phase[s];
    bool esc = vk == VK_ESCAPE;
    switch (ph) {
    case PH_MODE:
        if (vk == VK_LEFT || vk == 'A' || vk == VK_RIGHT || vk == 'D') {
            bool tag = vk == VK_LEFT || vk == 'A';
            if (g_sel.tagMode[s] != tag) { g_sel.tagMode[s] = tag; Log("select: P%d %s", s + 1, tag ? "TAG" : "SOLO"); }
            return true;
        }
        if (IsDir(vk)) return true;                       // the cursor waits until TAG / SOLO is chosen
        if (Confirmish(vk)) {
            g_sel.phase[s] = g_sel.tagMode[s] ? PH_MAIN : PH_SOLO;
            Log("select: P%d %s chosen", s + 1, g_sel.tagMode[s] ? "TAG" : "SOLO");
            return true;
        }
        return false;                                      // Esc: the game's own (dialog / take P1's confirm back)
    case PH_MAIN: {
        bool list = ListOpen(s);
        if (list && (esc || IsDir(vk))) return false;          // the game's slot list: move / close it
        if (list && vk != VK_RETURN) return false;             // its other buttons (pages ...) too
        if (!list && CostumeKey(vk)) return false;             // opens the slot list for the main's costume
        if (!list && vk == 'O') return BossToggle(s);
        if (esc) { ToMode(s); Log("select: P%d back to TAG/SOLO", s + 1); return true; }
        if (Confirmish(vk)) {
            // the cursor's cell: column +0x4B0, row +0x4B4 (P2 +0x440). Row 1 = top = locked characters here (row 2
            // middle, 3 bottom; measured 11-21): the press goes to the game (it refuses) and no main is taken.
            // "?" (column 9, rows 2-3) shows a cycling random character: whoever is shown at the press is the main.
            int col = -1, row = -1;
            uintptr_t o = SelObj();
            if (o) { Rd(o + 0x4B0 + 0x440 * s, col); Rd(o + 0x4B4 + 0x440 * s, row); }
            int id = 255;
            if (o) Rd(o + 0x508 + 0x440 * s, id);
            if (row == 1) { Log("select: P%d main pick on a locked cell (col %d): ignored", s + 1, col); return false; }
            if (id < 0 || id > 60) { Log("select: P%d main pick: cursor unknown (%d)", s + 1, id); return true; }
            g_sel.firstId[s] = id; g_sel.partnerId[s] = -1; g_sel.armed[s] = true; g_sel.phase[s] = PH_PARTNER;
            g_sel.mainBoss[s] = g_sel.bossHover[s] == id ? id : -1; g_sel.bossHover[s] = -1; g_sel.partnerBoss[s] = false;
            g_sel.mainCol[s] = col; g_sel.mainRow[s] = row;
            UiSound("SE_cmn_Mainmenu_Decide|SE_Decide|SE_Decide2|Decide_Menu_01");   // the game's confirm sound (Wwise)
            Log("select: P%d MAIN = %d (%s), the press stays hidden; now the partner", s + 1, id, CharName(id) ? CharName(id) : "?");
            {   // the main's costume: the list position (the game's own rule turns it into type / custom slot), else Default 1
                int pos = list ? ListCursor(s) : -1;               // no list: Default 1
                g_sel.mainSlot[s] = list ? pos : -1;
                CostumePick(s, false, id, pos, o);
                if (list) {
                    InputSynthPress(VK_ESCAPE);                // close the list (the game's own back key)
                    Wr(o + kListPos + kSideStride * s, (int)0); // the partner's pick starts from its own first costume
                    Log("select: P%d main costume = list position %d (list closed)", s + 1, pos);
                }
            }
            return true;
        }
        return false;
    }
    case PH_PARTNER:
        if (esc && ListOpen(s)) return false;                  // closes the slot list, keeps the main pick
        if (vk == 'O' && !ListOpen(s)) return BossToggle(s);
        if (vk == VK_RETURN) {                                 // the game's confirm: the partner's costume choice
            int id = 255;
            uintptr_t o = SelObj();
            if (o) Rd(o + 0x508 + 0x440 * s, id);
            g_sel.partnerBoss[s] = g_sel.bossHover[s] == id;
            int pos = ListOpen(s) ? ListCursor(s) : -1;         // no list: Default 1
            g_sel.partnerSlot[s] = pos;
            if (id >= 0 && id <= 60) CostumePick(s, true, id, pos, o);
            return false;
        }
        if (esc) {
            g_sel.armed[s] = false; g_sel.firstId[s] = -1; g_sel.partnerId[s] = -1; g_sel.phase[s] = PH_MAIN;
            g_sel.mainSlot[s] = -1; g_sel.mainBoss[s] = -1; g_sel.partnerBoss[s] = false; g_sel.bossHover[s] = -1;
            Log("select: P%d main pick taken back", s + 1);
            return true;
        }
        return false;                                      // Enter: the game's confirm -> setA -> DONE
    case PH_SOLO:
        if (vk == 'O' && !ListOpen(s)) return BossToggle(s);
        if (esc) { ToMode(s); Log("select: P%d back to TAG/SOLO", s + 1); return true; }
        return false;
    }
    return false;
}
}  // namespace

// side whose turn it is (P1 until P1 is done, then P2 when its cursor exists), -1 if none
int PickSide() {
    if (!g_live) return -1;
    if (g_sel.phase[0] != PH_DONE) return 0;
    if (g_sel.phase[1] != PH_NONE && g_sel.phase[1] != PH_DONE) return 1;
    return -1;
}

bool PickFilterKey(int vk) {
    if (vk <= 0 || vk > 255) return false;
    uint8_t h = g_held[vk];
    if (h) return h == 1;
    if (!g_active || g_cfg.mode != Mode::Tag || GameIsOnline()) return false;
    AcquireSRWLockExclusive(&g_lk);
    h = g_held[vk];
    if (!h) { h = OnKey(vk) ? 1 : 2; g_held[vk] = h; }
    ReleaseSRWLockExclusive(&g_lk);
    return h == 1;
}

// one new controller press (pad.cpp keeps the held / released state itself)
bool PickPadKey(int vk) {
    if (!g_active || g_cfg.mode != Mode::Tag || GameIsOnline()) return false;
    AcquireSRWLockExclusive(&g_lk);
    bool hide = OnKey(vk);
    ReleaseSRWLockExclusive(&g_lk);
    return hide;
}

// confirm seen by the setA hook (Enter reached the game)
void PickConfirmed(int s, int id) {
    if (!g_live || s < 0 || s > 1) return;
    AcquireSRWLockExclusive(&g_lk);
    int ph = g_sel.phase[s];
    if (ph == PH_PARTNER) {
        g_sel.partnerId[s] = id; g_sel.phase[s] = PH_DONE;
        Log("select: P%d PARTNER = %d (%s)%s", s + 1, id, CharName(id) ? CharName(id) : "?",
            id == g_sel.firstId[s] ? " - same as the main: plays SOLO" : "");
    } else if (ph == PH_SOLO || ph == PH_MODE || ph == PH_MAIN) {
        if (ph != PH_SOLO) Log("select: P%d confirmed by the game before TAG/SOLO was set: SOLO", s + 1);
        g_sel.mainBoss[s] = ph == PH_SOLO && g_sel.bossHover[s] == id ? id : -1;   // 11-26 solo boss version
        g_sel.bossHover[s] = -1;
        g_sel.armed[s] = false; g_sel.firstId[s] = -1; g_sel.phase[s] = PH_DONE;
    }
    ReleaseSRWLockExclusive(&g_lk);
}

void SelectPause() { g_active = false; g_live = false; }   // fighting: never hide keys; next select = new session

void SelectReset() {
    for (int s = 0; s < 2; ++s) {
        g_sel.phase[s] = PH_NONE; g_sel.armed[s] = false; g_sel.firstId[s] = -1; g_sel.partnerId[s] = -1;
        g_sel.mainSlot[s] = -1; g_sel.partnerSlot[s] = -1; g_sel.slotDone[s] = false; CostumeClear(s);
        g_sel.mainBoss[s] = -1; g_sel.partnerBoss[s] = false; g_sel.bossHover[s] = -1;
    }
    g_sel.done = false;
}

// tag thread, while not fighting: follow the game's select screen
void SelectTick2() {
    ULONGLONG now = GetTickCount64();
    for (int vk = 1; vk < 256; ++vk)
        if (g_held[vk] && !(GetAsyncKeyState(vk) & 0x8000)) g_held[vk] = 0;
    uintptr_t o = SelObj();
    int cnt = -1, h1 = 255, h2 = 255;
    if (o) { Rd(o + 0x60, cnt); Rd(o + 0x508, h1); Rd(o + 0x948, h2); }
    if (cnt != g_lastCnt) { g_lastCnt = cnt; g_cntMoved = now; }
    bool open = o && (h1 != 255 || h2 != 255);
    bool running = now - g_cntMoved < 250;
    g_hover[0] = h1; g_hover[1] = h2;
    g_sel.open = open;
    AcquireSRWLockExclusive(&g_lk);
    if (!open) {
        // left without the final commit (Esc out): clear; after the commit the picks are what the match loads
        if (g_live && !g_sel.done) { Log("select: left the select screen, picks cleared"); SelectReset(); }
        else if (g_live) Log("select: picks committed (P1 %s, P2 %s)", g_sel.armed[0] ? "TAG" : "SOLO", g_sel.armed[1] ? "TAG" : "SOLO");
        g_live = false;
    } else if (!g_live && running) {
        SelectReset();
        g_sel.phase[0] = PH_MODE;
        g_live = true;
        Log("select: select screen open (P1 %s by default)", g_sel.tagMode[0] ? "TAG" : "SOLO");
    }
    if (g_live) {
        // practice: P2's cursor appears after P1's confirm / disappears when Esc takes it back
        if (g_sel.phase[0] == PH_DONE && h2 != 255 && g_sel.phase[1] == PH_NONE) {
            g_sel.phase[1] = PH_MODE; g_sel.armed[1] = false; g_sel.firstId[1] = -1; g_sel.partnerId[1] = -1;
            Log("select: P2's turn");
        }
        if (g_sel.phase[0] == PH_DONE && h2 == 255 && g_sel.phase[1] != PH_NONE) {
            g_sel.phase[1] = PH_NONE;
            g_sel.partnerId[0] = -1;
            g_sel.phase[0] = g_sel.armed[0] ? PH_PARTNER : PH_SOLO;
            g_sel.slotDone[0] = false; g_sel.partnerSlot[0] = -1;
            Log("select: P1's confirm taken back by the game -> P1 %s again", g_sel.armed[0] ? "picks the partner" : "picks");
        }
    }
    g_active = g_live && running && g_cfg.mode == Mode::Tag && !GameIsOnline() && GameWindowInFront();
    g_sel.active = g_active;
    ReleaseSRWLockExclusive(&g_lk);
}

int SelectHover(int side) { return side >= 0 && side < 2 ? g_hover[side] : 255; }

// row 1 = top, 2 = middle, 3 = bottom
bool SelectCell(int side, int& col, int& row) {
    uintptr_t o = SelObj();
    col = row = -1;
    if (!o || side < 0 || side > 1) return false;
    Rd(o + 0x4B0 + 0x440 * side, col); Rd(o + 0x4B4 + 0x440 * side, row);
    return col >= 0 && col <= 18 && row >= 1 && row <= 3;
}

bool SelectInstall() {
    auto gp = Find({0x83, 0xF9, 0x2F, 0x77, 0x0F, 0x48, 0x63, 0xC1, 0x48, 0x8D, 0x0D, -1, -1, -1, -1, 0x48, 0x8B, 0x04, 0xC1, 0xC3});
    if (gp.size() == 1) { int32_t rel = *(int32_t*)(gp[0] + 11); g_playerList = gp[0] + 15 + rel; }
    auto a = Find({0x89, 0x51, 0x10, 0x89, 0x91, 0xA4, 0xC0, 0x00, 0x00, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
                   0x89, 0x51, 0x14, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC});
    auto c = Find({0x48, 0x8B, 0x81, 0x60, 0x15, 0x00, 0x00, 0x89, 0x91, 0xDC, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0});
    Log("select: playerList=%p setA=%zu charInfo=%zu", (void*)g_playerList, a.size(), c.size());
    if (a.size() != 1 || c.size() != 1 || !g_playerList) { Log("select: patterns not found, select-screen tag disabled"); return false; }
    bool ok = PatchJump(a[0], (void*)HkSetCharA, 16) && PatchJump(a[0] + 0x10, (void*)HkSetCharB, 16) &&
              PatchJump(c[0], (void*)HkCharInfoSetID, 43);
    Log("select: hooks %s", ok ? "installed" : "FAILED");
    return ok;
}
