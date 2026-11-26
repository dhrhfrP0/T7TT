// Offline test of src/tag.cpp + select flow: fake P1/P2 player structures holding real movesets (v4 .t7p blobs),
// scripted keys through g_tagInput, a scripted "fighting" flag, assertions on what the tag code writes.
#include "../src/common.h"
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <cstring>

Config g_cfg;
HMODULE g_selfModule = nullptr;
extern bool (*g_tagInput)(int side);
extern bool (*g_fightingOverride)();

static std::vector<uint8_t> P[2];
static bool g_press[2];
static bool g_fight = false;
static int g_fail = 0;

void Log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); printf("  log: "); vprintf(fmt, ap); va_end(ap); printf("\n"); }
std::wstring ModuleDir(HMODULE) { return L"bin\\"; }
void ConfigLoad(HMODULE) {}
bool SafeRead(uintptr_t a, void* o, size_t n) { if (a < 0x10000) return false; __try { memcpy(o, (void*)a, n); return true; } __except (1) { return false; } }
bool SafeWrite(uintptr_t a, const void* i, size_t n) { if (a < 0x10000) return false; __try { memcpy((void*)a, i, n); return true; } __except (1) { return false; } }
std::string ReadCStr(uintptr_t a, size_t max) { char b[128]{}; if (max > 127) max = 127; if (!a || !SafeRead(a, b, max)) return {}; return b; }
static uint8_t g_selObjMem[0x1000];
static uintptr_t g_selObjSlot = 0;                // the game's [base+0x34D65C0] (select-screen object)
uintptr_t GameBase() { return (uintptr_t)&g_selObjSlot - 0x34D65C0; }
bool GameIsOnline() { return false; }
uintptr_t GamePlayer(int s) { return (uintptr_t)P[s].data(); }
HWND GameWindow() { return nullptr; }
bool GameWindowInFront() { return true; }
bool InputBlocked() { return false; }
bool ModelAvailable() { return false; }
void ModelPrepare(int, int, int, int) {}
void ModelShow(int, int) {}
void ModelHudIcon(int, int) {}
bool ModelHudIconShows(int, int) { return false; }
void ModelHudIconRestore(int, int) {}
void ModelForget(int) {}
void ModelBossSolo(int, int) {}
uintptr_t BossSet(int) { return 0; }
void UiSound(const char*) {}
void BattleSound(int, const std::string&) {}
void OverlayToast(const wchar_t*, int) {}
void SlotLabTick() {}
void ModelSpawnDouble(int, int) {}
uintptr_t g_doubleActor = 0;
bool ModelDoll(int, int, bool) { return false; }
void CostumePick(int, bool, int, int, uintptr_t) {}
void CostumeClear(int) {}
int g_testLoads = 0;
int CostumeLoads(int) { return g_testLoads; }
void RecorderFrame(int, bool, bool) {}
bool g_testPadTag = false; int g_testPadAtk = 0;
bool PadTagDown(int) { return g_testPadTag; }
int PadAttack(int) { return g_testPadAtk; }
void PadSynthPress(int, int) {}
bool DollBegin(int, int, int, uintptr_t, uintptr_t) { return false; }
uintptr_t DollSource() { return 0; }
int DollSide() { return -1; }
void DollTick() {}
void DollEnd() {}
bool DollActive() { return false; }
namespace ue { bool FrameHookReady() { return false; } }
void InputSynthPress(int) {}

#define CHECK(c, msg) do { if (c) printf("PASS %s\n", msg); else { printf("FAIL %s\n", msg); ++g_fail; } } while (0)

static std::vector<uint8_t> g_commonFile;
// Load a v2 blob as if the game had loaded that character.
uintptr_t LoadAsGame(const char* code) {
    auto readAll = [](const char* path) {
        FILE* f; fopen_s(&f, path, "rb"); if (!f) { printf("missing %s\n", path); exit(2); }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> v(n); fread(v.data(), 1, n, f); fclose(f); return v;
    };
    if (g_commonFile.empty()) g_commonFile = readAll("bin\\T7Tag_partners\\common.bin");
    char path[128]; sprintf_s(path, "bin\\T7Tag_partners\\%s.t7p", code);
    auto raw = readAll(path);
    uint32_t nrel; uint64_t isz, comOff; memcpy(&nrel, &raw[12], 4); memcpy(&isz, &raw[16], 8); memcpy(&comOff, &raw[0x50], 8);
    size_t off = (0x70 + 4ull * nrel + 15) / 16 * 16;
    uint8_t* mem = (uint8_t*)VirtualAlloc(nullptr, isz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    memcpy(mem, &raw[off], comOff);
    memcpy(mem + comOff, g_commonFile.data() + 16, isz - comOff);
    for (uint32_t i = 0; i < nrel; ++i) { uint32_t r; memcpy(&r, &raw[0x70 + 4 * i], 4); *(uint64_t*)(mem + r) += (uintptr_t)mem; }
    return (uintptr_t)mem;
}
uintptr_t Moves(uintptr_t info) { return *(uintptr_t*)(info + Mi::Table + Mi::T_Move * 16); }
uint64_t MoveCount(uintptr_t info) { return *(uint64_t*)(info + Mi::Table + Mi::T_Move * 16 + 8); }
uintptr_t ByName(uintptr_t info, const char* n) {
    for (uint64_t i = 0; i < MoveCount(info); ++i) { uintptr_t m = Moves(info) + i * 0xB0; if (!strcmp(*(char**)m, n)) return m; }
    return 0;
}
uintptr_t Alias(uintptr_t info, int a) { return Moves(info) + *(uint16_t*)(info + Mi::CurAliases + 2 * (a - 0x8000)) * 0xB0; }
std::string MsName(uintptr_t info) { return ReadCStr(*(uintptr_t*)(info + Mi::Name)); }
template <class T> T& F(int s, uintptr_t off) { return *(T*)(P[s].data() + off); }
void SetMoveset(int s, uintptr_t info) { for (int k = 0; k < 5; ++k) F<uintptr_t>(s, Off::Motbin + 8 * k) = info; }
void Tick(int n = 1) { for (int i = 0; i < n; ++i) { TagTick(); Sleep(10); } }
bool Press(int side) { bool p = g_press[side]; g_press[side] = false; return p; }
bool Fight() { return g_fight; }
// fake select screen: cursor characters (255 = none), frame counter moves on SelTick
void SelOpen(int h1, int h2) {
    g_selObjSlot = (uintptr_t)g_selObjMem;
    *(int*)(g_selObjMem + 0x508) = h1; *(int*)(g_selObjMem + 0x948) = h2;
    g_selObjMem[0x503] = 0; g_selObjMem[0x503 + 0x440] = 0;   // costume lists closed
}
void SelTick(int n = 1) { for (int i = 0; i < n; ++i) { ++*(int*)(g_selObjMem + 0x60); Tick(); } }
bool Key(int vk) { bool r = PickFilterKey(vk); ++*(int*)(g_selObjMem + 0x60); Tick(); return r; }   // tick = key released
void Confirm(int side, int id) { g_sel.visited[side] = true; PickConfirmed(side, id); }

// health as the game keeps it: an encrypted u32 (faked here as 0x5EC00000 | pct) plus the percent
void SetHp(int s, int pct) { F<uint32_t>(s, Off::HealthEnc) = 0x5EC00000u | (uint32_t)pct; F<int32_t>(s, Off::HealthPct) = pct; }
int Hp(int s) { return F<int32_t>(s, Off::HealthPct); }
bool HpConsistent(int s) { return (F<uint32_t>(s, Off::HealthEnc) & 0xFF) == (uint32_t)Hp(s); }
uint32_t WallSwitch(uintptr_t info) {      // the wall-screw requirement row: the one hit-condition row whose list is [614|901, 881]
    uintptr_t hc = *(uintptr_t*)(info + Mi::Table + 2 * 16); uint64_t n = *(uint64_t*)(info + Mi::Table + 2 * 16 + 8);
    uintptr_t rq = *(uintptr_t*)(info + Mi::Table + 1 * 16); uint64_t rn = *(uint64_t*)(info + Mi::Table + 1 * 16 + 8);
    for (uint64_t i = 1; i < n; ++i) {
        uintptr_t r = *(uintptr_t*)(hc + i * 0x18), prev = *(uintptr_t*)(hc + (i - 1) * 0x18);
        bool outside = r < rq || r >= rq + rn * 8;
        if (outside && *(uint32_t*)prev == 615) return *(uint32_t*)r;
    }
    return 0;
}
std::string Cur(int s) { return ReadCStr(*(uintptr_t*)F<uintptr_t>(s, Off::CurMove)); }
void Play(int s, uintptr_t mv) { F<uintptr_t>(s, Off::CurMove) = mv; }
uintptr_t Ms(int s) { return F<uintptr_t>(s, Off::Motbin); }
// an attack with active frames, played from frame 0 (attack not over yet); Done() = past its last active frame
uintptr_t Atk(uintptr_t info) {
    for (uint64_t i = 100; i < MoveCount(info); ++i) {
        uintptr_t m = Moves(info) + i * 0xB0;
        if (*(uint32_t*)(m + 0xA4) > 5 && *(uint32_t*)(m + 0xA4) < 40) return m;
    }
    return Moves(info) + 900 * 0xB0;
}
void PlayAtk(int s, uintptr_t info) { Play(s, Atk(info)); F<int32_t>(s, Off::MoveTimer) = 0; }
void Done(int s) { F<int32_t>(s, Off::MoveTimer) = 999; }
uintptr_t Dash(uintptr_t info) {
    for (uint64_t i = 0; i < MoveCount(info); ++i) {
        const char* n = *(char**)(Moves(info) + i * 0xB0); size_t L = strlen(n);
        if (L > 7 && !strcmp(n + L - 7, "_sDASHF")) return Moves(info) + i * 0xB0;
    }
    return 0;
}

int main() {
    g_cfg.mode = Mode::Tag; g_cfg.p2Tag = P2TagMode::Key;
    g_cfg.recoverPerSec = 35.0; g_cfg.redPercent = 40; g_cfg.maxHealth = 175;   // fast recovery for the test
    g_cfg.assaultEffects = "0x8229:0, 0x84C4:0x0100002B, 0x828C:0x004B001E";
    g_cfg.assaultBufferMs = 400; g_cfg.returnIdleMs = 300; g_cfg.assaultWindowMs = 300; g_cfg.wallScrew = true;
    g_tagInput = Press; g_fightingOverride = Fight;
    P[0].assign(0x3670, 0); P[1].assign(0x3670, 0);
    TagInit();

    // ---- select screen (TTT2 flow, measured 11-21): TAG/SOLO first, first Enter = MAIN (hidden), second = PARTNER
    SelTick();
    CHECK(!g_sel.open && !Key(VK_RETURN), "no select screen (menus): nothing is ever hidden");
    SelOpen(6, 255); SelTick(2);
    CHECK(g_sel.open && g_sel.phase[0] == PH_MODE && PickSide() == 0, "select screen opens: P1 chooses TAG/SOLO first");
    OverlaySide ov[2]; TagOverlaySnapshot(ov);
    CHECK(ov[0].select && ov[0].phase == PH_MODE && !ov[1].select, "overlay: TAG/SOLO icons for P1 right away");
    CHECK(Key('D') && !g_sel.tagMode[0], "D = SOLO (hidden from the game)");
    CHECK(Key('A') && g_sel.tagMode[0], "A = TAG");
    CHECK(Key('S'), "cursor keys wait while TAG/SOLO is shown");
    CHECK(!Key(VK_ESCAPE) && g_sel.phase[0] == PH_MODE, "Esc at TAG/SOLO is the game's own (back dialog)");
    CHECK(Key(VK_RETURN) && g_sel.phase[0] == PH_MAIN, "Enter: TAG, now the main");
    CHECK(!Key('D'), "cursor moves reach the game");
    CHECK(Key(VK_RETURN) && g_sel.armed[0] && g_sel.firstId[0] == 6 && g_sel.phase[0] == PH_PARTNER,
          "Enter on Jin: MAIN = Jin, the press never reaches the game");
    CHECK(Key(VK_ESCAPE) && !g_sel.armed[0] && g_sel.phase[0] == PH_MAIN, "Esc: main taken back (hidden)");
    // 11-23 costume slots: U opens the game's list for the main, Enter on it = main + its slot, Esc closes it
    CHECK(!Key('U') && g_sel.phase[0] == PH_MAIN, "U at the main pick reaches the game (opens its costume list)");
    g_selObjMem[0x503] = 1; *(int*)(g_selObjMem + 0x4CC) = 5;
    CHECK(!Key('D') && !Key(VK_ESCAPE) && g_sel.phase[0] == PH_MAIN, "list open: moves and Esc are the list's");
    CHECK(Key(VK_RETURN) && g_sel.firstId[0] == 6 && g_sel.mainSlot[0] == 5 && g_sel.phase[0] == PH_PARTNER,
          "Enter on the list: MAIN = Jin in slot 5");
    CHECK(!Key(VK_ESCAPE) && g_sel.phase[0] == PH_PARTNER, "list still open at the partner pick: Esc only closes it");
    g_selObjMem[0x503] = 0;
    CHECK(Key(VK_ESCAPE) && g_sel.mainSlot[0] == -1 && g_sel.phase[0] == PH_MAIN, "list closed: Esc takes the main back");
    CHECK(Key(VK_ESCAPE) && g_sel.phase[0] == PH_MODE, "Esc again: back to TAG/SOLO (hidden)");
    Key(VK_RETURN); Key(VK_RETURN);
    CHECK(g_sel.firstId[0] == 6 && g_sel.phase[0] == PH_PARTNER, "TAG, MAIN Jin again");
    TagOverlaySnapshot(ov);
    CHECK(ov[0].select && ov[0].armed && ov[0].phase == PH_PARTNER && !strcmp(ov[0].firstName, u8"진"), "overlay: MAIN 진, picking the partner");
    SelOpen(2, 255); SelTick();
    CHECK(!Key(VK_RETURN), "partner Enter reaches the game");
    Confirm(0, 2); SelTick();
    CHECK(g_sel.phase[0] == PH_DONE && g_sel.partnerId[0] == 2, "the game's confirm: PARTNER = King");
    SelOpen(2, 8); SelTick();
    CHECK(g_sel.phase[1] == PH_MODE && PickSide() == 1, "practice: P2's turn, TAG/SOLO first");
    CHECK(!Key(VK_ESCAPE), "Esc at P2's TAG/SOLO is the game's (takes P1's confirm back)");
    SelOpen(2, 255); SelTick();
    CHECK(g_sel.phase[0] == PH_PARTNER && g_sel.armed[0] && g_sel.firstId[0] == 6 && g_sel.partnerId[0] < 0 &&
          g_sel.phase[1] == PH_NONE, "P1's confirm taken back: P1 picks the partner again, main kept");
    Confirm(0, 2); SelOpen(2, 1); SelTick();
    CHECK(Key('A') && Key(VK_RETURN) && g_sel.phase[1] == PH_MAIN, "P2: TAG");
    CHECK(Key(VK_RETURN) && g_sel.firstId[1] == 1 && g_sel.phase[1] == PH_PARTNER, "P2 MAIN = Law (hidden)");
    Confirm(1, 0); SelTick();
    CHECK(g_sel.phase[1] == PH_DONE && g_sel.partnerId[1] == 0, "P2 PARTNER = Paul");
    Confirm(0, 2); Confirm(1, 0);                  // the final commit repeats the confirms: ignored
    g_sel.done = true; SelOpen(255, 255); SelTick();
    CHECK(g_sel.armed[0] && g_sel.partnerId[0] == 2 && g_sel.armed[1] && g_sel.partnerId[1] == 0 && !g_sel.open,
          "committed: the picks stay for the match");
    CHECK(!Key(VK_RETURN), "loading / other screens: nothing hidden");

    // ---- fight: the game loaded the first picks ----
    uintptr_t jin = LoadAsGame("jin"), law = LoadAsGame("law");
    SetMoveset(0, jin); SetMoveset(1, law);
    Play(0, Alias(jin, 0x8001)); Play(1, Alias(law, 0x8001));
    SetHp(0, 100); SetHp(1, 100);
    g_fight = true;
    Tick();
    uintptr_t jinCopy = Ms(0);
    Tick();
    CHECK(jinCopy != jin && MsName(jinCopy) == "[JIN]", "match setup slots our Jin copy (assault moves + wall screw)");
    CHECK(WallSwitch(jinCopy) == 901, "wall-screw row off while the opponent is not in a combo");
    Play(1, ByName(law, "sDw_AIR00_")); Tick();
    CHECK(WallSwitch(jinCopy) == 881, "opponent in a combo, no screw yet: wall-screw row on (always true)");
    Play(1, Alias(law, 0x8001)); Sleep(200); Tick();
    TagOverlaySnapshot(ov);
    CHECK(ov[0].show && !strcmp(ov[0].waitingName, u8"킹") && ov[1].show && !strcmp(ov[1].waitingName, u8"폴"),
          "fight overlay: P1 waiting 킹, P2 waiting 폴");
    CHECK(ov[0].modelId == 6 && ov[0].activeId == 6 && ov[1].modelId == 1, "overlay ids: model / in control");

    // Jin takes damage (the game writes new encrypted values), then a neutral tag -> King at full health
    SetHp(0, 85); Tick(); SetHp(0, 70); Tick();
    g_press[0] = true; Tick();
    uintptr_t king = Ms(0);
    CHECK(MsName(king) == "[KING]", "neutral tag installs King");
    uintptr_t entry = ByName(king, "T7Tag_EntryRaw");
    CHECK(entry && F<uintptr_t>(0, Off::NextMove) == entry, "standing tag: King comes in (T7Tag_EntryRaw)");
    {
        // v6 (TTT2 measured): raw entry 45 locked frames, combo entry 20, then standing
        uintptr_t c = *(uintptr_t*)(entry + 0x20);
        CHECK(*(uint64_t*)c == 0x8000 && *(uint32_t*)(c + 0x20) == 60 && *(uint16_t*)(c + 0x24) == 0x8001,
              "T7Tag_EntryRaw: locked 60 frames (ini RawTagFrames), then standing");
        uintptr_t ce = *(uintptr_t*)(ByName(king, "T7Tag_Entry") + 0x20);
        CHECK(*(uint64_t*)ce == 0x8000 && *(uint32_t*)(ce + 0x20) == 20 && *(uint16_t*)(ce + 0x24) == 0x8001,
              "T7Tag_Entry (tag combo): locked 20 frames, then standing");
        uintptr_t a = ByName(king, "T7Tag_Assault"), ca = *(uintptr_t*)(a + 0x20);
        CHECK(*(uint64_t*)ca == 0x8000 && *(uint32_t*)(ca + 0x20) == 18 && *(uint16_t*)(ca + 0x24) == 0x8001,
              "T7Tag_Assault: locked 18 frames, then standing");
    }
    CHECK(Hp(0) == 100 && HpConsistent(0), "King has his own (full) health: raw value restored");
    TagOverlaySnapshot(ov);
    CHECK(ov[0].waitingPct == 70 && ov[0].waitingRed == 12, "waiting Jin: 70%, red 12% (40% of 30)");
    CHECK(ov[0].activeId == 2, "overlay: King in control (portrait swaps)");
    {
        uintptr_t a = ByName(king, "T7Tag_Assault");
        uint32_t* e = a ? *(uint32_t**)(a + 0x80) : nullptr;
        CHECK(e && e[1] == 0x8229 && e[4] == 0x84C4 && e[5] == 0x0100002B && e[7] == 0x828C && e[8] == 0x004B001E,
              "King's T7Tag_Assault carries the ini effects (white flash, sound, light slow motion)");
        uintptr_t dash = ByName(king, "Ki_sDASHF");
        if (!dash) for (uint64_t i = 0; i < MoveCount(king); ++i) {
            const char* n = *(char**)(Moves(king) + i * 0xB0); size_t L = strlen(n);
            if (L > 7 && !strcmp(n + L - 7, "_sDASHF")) { dash = Moves(king) + i * 0xB0; break; }
        }
        CHECK(dash && *(uintptr_t*)(a + 0x10) == *(uintptr_t*)(dash + 0x10), "T7Tag_Assault is the forward dash (partner chases)");
    }
    Sleep(900); Tick();                            // red life comes back while Jin waits
    TagOverlaySnapshot(ov);
    CHECK(ov[0].waitingPct == 85 && ov[0].waitingRed < 12, "waiting Jin recovers to a state the game produced (85%)");
    printf("     (Jin waiting %d%% red %d)\n", ov[0].waitingPct, ov[0].waitingRed);

    // ---- buffered assault: tag pressed during King's attack, fires the moment Law is screwed
    Sleep(800);
    PlayAtk(0, king); Play(1, ByName(law, "sDw_AIR00_"));
    g_press[0] = true; Tick();
    CHECK(Ms(0) == king, "tag during an attack: waits, nothing yet");
    Play(1, ByName(law, "sDw_AIR_SPINL")); Tick();
    CHECK(Ms(0) == king, "the screw lands but King's attack is not over: the attack is not cut");
    Done(0); Tick();
    CHECK(Ms(0) == jinCopy, "attack over on a fresh screw -> assault (Jin in)");
    CHECK(F<uintptr_t>(0, Off::NextMove) == ByName(jinCopy, "T7Tag_Assault"), "Jin plays T7Tag_Assault");
    CHECK(Hp(0) == 85 && HpConsistent(0), "Jin comes in with his recovered health");
    Tick();
    CHECK(WallSwitch(jinCopy) == 901 && WallSwitch(king) == 901, "screw used this combo: wall-screw switch off in both");
    TagOverlaySnapshot(ov);
    CHECK(ov[0].assault, "overlay knows an assault is on");

    // ---- partner runs after the far screw, the return timer starts when the opponent lands
    Play(0, ByName(jinCopy, "T7Tag_Assault")); Tick();
    Play(0, Alias(jinCopy, 0x8001)); Sleep(450); Tick();       // Jin stands while Law still spins
    CHECK(Ms(0) == jinCopy, "no return while the screwed opponent is still in the air");
    Play(1, ByName(law, "sDw_AIR00_")); Tick();                 // out of the spin, still airborne
    Play(0, Moves(jinCopy) + 900 * 0xB0); Sleep(400); Tick();   // Jin's follow-up
    CHECK(Ms(0) == jinCopy, "Jin keeps control while attacking");
    Play(0, Alias(jinCopy, 0x8001)); Tick(); Sleep(400); Tick();
    CHECK(Ms(0) == jinCopy, "Jin's move is over but the opponent is still airborne: Jin keeps control (AssaultReturn=Landing)");
    Play(1, Alias(law, 0x8006)); Tick(); Sleep(150); Tick();    // Law on the floor
    CHECK(Ms(0) == jinCopy, "opponent just touched the floor: not yet");
    Sleep(200); Tick();
    uintptr_t back = Ms(0);
    CHECK(back == king, "Jin free ReturnIdleMs after the floor -> King takes control back");
    CHECK(F<uintptr_t>(0, Off::NextMove) == ByName(king, "T7Tag_Return"), "King plays T7Tag_Return (white flash)");
    {
        uint32_t* e = *(uint32_t**)(ByName(king, "T7Tag_Return") + 0x80);
        CHECK(e[1] == 0x8229 && e[4] == 0x828D && e[5] == 0, "T7Tag_Return: white flash + slow-down immunity off");
    }
    Play(0, Alias(king, 0x8001)); Play(1, Alias(law, 0x8001)); Sleep(200); Tick(); Sleep(200); Tick();
    Play(1, ByName(law, "sDw_AIR00_")); Tick();
    CHECK(WallSwitch(king) == 881, "next combo: wall screw available again");
    Play(1, Alias(law, 0x8001)); Sleep(200); Tick();

    // ---- combo over (opponent down) also returns, but not before ReturnIdleMs after the landing
    Sleep(800);
    PlayAtk(0, king); Done(0); Play(1, ByName(law, "sDw_AIR_SPINR"));
    g_press[0] = true; Tick();
    CHECK(Ms(0) == jinCopy, "assault again: Jin in");
    Play(0, ByName(jinCopy, "T7Tag_Assault")); Play(1, Alias(law, 0x8006)); Tick();   // Law lands and lies down, Jin dashes
    Sleep(400); Tick();
    CHECK(Ms(0) == jinCopy, "opponent down but Jin still in his entry: no return");
    Play(0, Alias(jinCopy, 0x8001)); Tick(); Sleep(150); Tick();
    CHECK(Ms(0) == jinCopy, "entry over 0.15 s ago: not yet");
    Sleep(200); Tick();
    CHECK(Ms(0) == king, "opponent on the floor, Jin free 0.35 s -> King back");
    Play(0, Alias(king, 0x8001)); Play(1, Alias(law, 0x8001)); Sleep(200); Tick();

    // ---- manual return: tag key during the assault brings the main back at once
    Sleep(800);
    PlayAtk(0, king); Done(0); Play(1, ByName(law, "sDw_AIR_SPINR"));
    g_press[0] = true; Tick();
    CHECK(Ms(0) == jinCopy, "third assault: Jin in");
    Play(0, Moves(jinCopy) + 900 * 0xB0); Sleep(300); Tick();
    g_press[0] = true; Tick();
    CHECK(Ms(0) == king, "tag key during the assault -> King back immediately");
    Play(0, Alias(king, 0x8001)); Play(1, Alias(law, 0x8001)); Sleep(200); Tick();

    // ---- AssaultReturn=Move (TTT2 as measured): back after the partner's one move, opponent still airborne
    g_cfg.assaultReturnMove = true;
    Sleep(800);
    PlayAtk(0, king); Done(0); Play(1, ByName(law, "sDw_AIR_SPINR"));
    g_press[0] = true; Tick();
    CHECK(Ms(0) == jinCopy, "Move mode: assault, Jin in");
    Play(0, ByName(jinCopy, "T7Tag_Assault")); Tick();
    Play(0, Moves(jinCopy) + 900 * 0xB0); Play(1, ByName(law, "sDw_AIR00_")); Sleep(200); Tick();
    CHECK(Ms(0) == jinCopy, "Move mode: Jin attacking");
    Play(0, Alias(jinCopy, 0x8001)); Tick();
    CHECK(Ms(0) == king, "Move mode: Jin's one move over -> King back while the opponent is airborne");
    g_cfg.assaultReturnMove = false;
    Play(0, Alias(king, 0x8001)); Play(1, Alias(law, 0x8001)); Sleep(200); Tick();

    // ---- TTT2 tag combo: a screw that landed long ago is no assault; the opponent is juggled -> partner stays
    Sleep(800);
    PlayAtk(0, king); Done(0); Play(1, ByName(law, "sDw_AIR_SPINL")); Tick();
    Sleep(400);
    g_press[0] = true; Tick(); TagOverlaySnapshot(ov);
    CHECK(Ms(0) == jinCopy && !ov[0].assault,
          "tag 0.4 s after the screw (window over), attack out, opponent juggled: tag combo (TTT2 tag buffer launcher)");
    CHECK(F<uintptr_t>(0, Off::NextMove) == ByName(jinCopy, "T7Tag_Entry"), "tag combo: Jin runs in (locked entry)");
    Play(1, Alias(law, 0x8006)); Play(0, Alias(jinCopy, 0x8001)); Sleep(1200); Tick();
    CHECK(Ms(0) == jinCopy, "tag combo: Jin stays (no automatic return)");
    Play(1, Alias(law, 0x8001)); g_press[0] = true; Tick();
    CHECK(Ms(0) == king, "standing tag: King back");
    Sleep(800); Tick();

    // ---- tag during a move that did not launch (blocked / whiffed): TTT2 drops the buffered tag (no raw tag)
    PlayAtk(0, king); Play(1, Alias(law, 0x8001));
    g_press[0] = true; Tick(); Sleep(100); Tick();
    CHECK(Ms(0) == king, "attack still active: no tag yet");
    Done(0); Tick();
    CHECK(Ms(0) == king, "attack over, opponent not juggled: no tag (no recovery cancel)");
    Play(0, Alias(king, 0x8001)); Tick();
    CHECK(Ms(0) == king, "recovery over: the buffered tag is dropped (the move did not launch)");
    Sleep(800); g_press[0] = true; Tick();
    CHECK(Ms(0) == jinCopy && F<uintptr_t>(0, Off::NextMove) == ByName(jinCopy, "T7Tag_EntryRaw"), "raw tag from neutral: Jin comes in (fixed pose)");
    Play(0, Alias(jinCopy, 0x8001)); Sleep(800); g_press[0] = true; Tick();
    CHECK(Ms(0) == king, "standing tag: King back");
    Sleep(800); Tick();

    // ---- tag from the floor (TTT2: get up and swap) and tag throw (safe tag after a connected throw)
    {
        uintptr_t me = Ms(0);
        uintptr_t lying = ByName(me, "dwDWN_01U");
        if (lying) {
            Play(0, lying); Play(1, Alias(law, 0x8001)); Sleep(800); Tick();
            g_press[0] = true; Tick();
            CHECK(Ms(0) != me && F<uintptr_t>(0, Off::NextMove) == ByName(Ms(0), "T7Tag_EntryRaw"), "tag while down: partner comes in (floor tag)");
            Play(0, Alias(Ms(0), 0x8001)); Sleep(800); Tick();
        } else puts("     (no dwDWN_01U in this moveset: floor tag not tested)");
        uintptr_t a = Ms(0);
        uintptr_t thrY = 0;
        uintptr_t lm = Moves(law); uint64_t lc = *(uint64_t*)(law + Mi::Table + Mi::T_Move * 16 + 8);
        for (uint64_t i = 0; i < lc && !thrY; ++i) {
            const char* nm = *(const char**)(lm + i * 0xB0);
            std::string sn = nm ? nm : "";
            if (sn.find("nage") != std::string::npos && sn.size() > 2 && sn.compare(sn.size() - 2, 2, "_y") == 0 && sn.rfind("Esc_", 0) != 0) thrY = lm + i * 0xB0;
        }
        CHECK(thrY != 0, "Law's moveset has a thrown reaction (..nage_y)");
        PlayAtk(0, a); Play(1, thrY); g_press[0] = true; Tick(); Sleep(100); Tick();
        CHECK(Ms(0) == a, "tag during a connected throw: waits for the throw to end");
        Play(1, Alias(law, 0x8006)); Play(0, Alias(a, 0x8001)); Tick();
        CHECK(Ms(0) != a && F<uintptr_t>(0, Off::NextMove) == ByName(Ms(0), "T7Tag_Entry"), "throw over: tag throw, partner comes in quickly");
        Play(0, Alias(Ms(0), 0x8001)); Play(1, Alias(law, 0x8001)); Sleep(800); Tick();
    }

    // P2 tag key works for P2 (Key mode)
    Sleep(800);
    Play(1, Alias(law, 0x8001));
    uintptr_t lawCopy = Ms(1);
    g_press[1] = true; Tick();
    CHECK(MsName(Ms(1)) == "[Paul]" && lawCopy != law, "P2 tags Law -> Paul with the P key");

    // ---- rematch without the select screen keeps the teams
    g_fight = false; Tick();
    {
        uintptr_t jinB = LoadAsGame("jin"), lawB = LoadAsGame("law");
        SetMoveset(0, jinB); SetMoveset(1, lawB);
        Play(0, Alias(jinB, 0x8001)); Play(1, Alias(lawB, 0x8001));
        SetHp(0, 100); SetHp(1, 100);
        Sleep(1600); Tick();                       // the picks are reset after a fight; the team is remembered
        g_fight = true; Tick();
        TagOverlaySnapshot(ov);
        CHECK(ov[0].show && !strcmp(ov[0].waitingName, u8"킹") && ov[1].show, "rematch: both teams kept");
    }

    // ---- new select screen: P2 SOLO, P1 Hwoarang + King ----
    g_fight = false; Tick();
    SelOpen(4, 255); SelTick(2);
    CHECK(!g_sel.armed[0] && !g_sel.armed[1] && g_sel.phase[0] == PH_MODE, "a new select screen starts fresh");
    Key(VK_RETURN); Key(VK_RETURN);                // TAG (remembered), MAIN Hwoarang
    SelOpen(2, 255); SelTick(); Key(VK_RETURN); Confirm(0, 2); SelOpen(2, 1); SelTick();
    Key('D'); Key(VK_RETURN);                      // P2 SOLO
    CHECK(!g_sel.tagMode[1] && g_sel.phase[1] == PH_SOLO && !g_sel.armed[1], "P2 SOLO at the select screen");
    Confirm(1, 1); g_sel.done = true; SelOpen(255, 255); SelTick();
    uintptr_t hwo = LoadAsGame("hwo"), law2 = LoadAsGame("law");
    SetMoveset(0, hwo); SetMoveset(1, law2);
    Play(0, Alias(hwo, 0x8001)); Play(1, Alias(law2, 0x8001));
    SetHp(0, 100); SetHp(1, 100);
    g_fight = true; Tick(); Tick();
    TagOverlaySnapshot(ov);
    CHECK(ov[0].show && !ov[1].show, "new match: P1 tag, P2 solo");
    CHECK(Ms(1) != law2 && MsName(Ms(1)) == "[LAW]" && WallSwitch(Ms(1)) == 901, "solo P2 gets its own copy for the wall screw");
    Sleep(800);
    uintptr_t solo = Ms(1);
    g_press[1] = true; Tick();
    CHECK(Ms(1) == solo, "solo P2 cannot tag");
    g_press[0] = true; Tick();
    uintptr_t k2 = Ms(0);
    CHECK(MsName(k2) == "[KING]", "P1 Hwoarang -> King");
    {
        uintptr_t req = *(uintptr_t*)(k2 + Mi::Table + 16); uint64_t n = *(uint64_t*)(k2 + Mi::Table + 24);
        int own = 0; for (uint64_t i = 0; i < n; ++i) { uint32_t* r = (uint32_t*)(req + 8 * i); if (r[0] == 217 && r[1] == 4) ++own; }
        CHECK(own > 0, "King's id checks retargeted to Hwoarang (4)");
    }
    // practice-mode refill: health jumps back to 100 -> team healed
    SetHp(0, 40); Tick(); SetHp(0, 100); Tick();
    TagOverlaySnapshot(ov);
    CHECK(ov[0].waitingPct == 100 && ov[0].activePct == 100, "refill to 100% heals the whole team");
    printf(g_fail ? "\n%d FAILED\n" : "\nALL PASSED\n", g_fail);
    return g_fail;
}
