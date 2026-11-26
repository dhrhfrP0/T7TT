#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <string>

// ---- logging (T7Tag.log beside the dll) ----
void LogInit(HMODULE self);
void Log(const char* fmt, ...);

// ---- config (T7Tag.ini beside the dll) ----
enum class Mode { Solo, Tag };
enum class P2TagMode { Auto, Key, Off };
struct Config {
    Mode mode = Mode::Solo;
    bool focusFix = true;        // keep running when alt-tabbed
    bool noRageArts = true;      // remove Rage Arts (Rage Drives stay)
    // tag
    int  tagKey = VK_SPACE;      // P1 tag key (virtual key)
    int  p2TagKey = 'P';         // P2 tag key when P2Tag=Key
    P2TagMode p2Tag = P2TagMode::Auto;
    bool tagInJump = true;       // play Co_ChgInJmpBalS on a neutral tag
    std::string assaultEffects;  // "id:value,id:value" rows for T7Tag_Assault (empty = blob defaults)
    std::string returnEffects;   // rows for T7Tag_Return (point takes control back)
    int assaultBufferMs = 700;   // tag pressed during your own attack: assault fires the moment it screws (within this)
    int returnIdleMs = 700;      // after the screwed opponent lands: partner standing idle this long -> the point comes back
    int comboTagFrames = 20;     // tag combo entry: frames before the partner can act (TTT2 ~0.45 s to the first attack)
    int rawTagFrames = 60;       // raw / standing tag entry: fixed-pose frames (TTT2: buttons 0.75 s, guard 1.0-1.2 s)
    int rawTagButtonFrames = 45; // ... an attack button may cut it from here (TTT2: button moves before guard)
    bool padTest = false;        // [Debug] PadTest: the test rig's virtual controller
    bool doll = true;            // (11-23) the outgoing member stays on screen and finishes its move (look only)
    int dollMinFrames = 10, dollMaxFrames = 90;
    int crashHealthPct = 40;     // tag crash: the partner counts as raging when the active one is this low
    int crashKey = 'I';          // tag crash = tag + this key while down (TTT2: TAG + 2 = right punch)
    bool partnerCustom = true;   // PartnerCostume=Custom: the player's customized costume for the partner (else default)
    int partnerCostumeSlot = 0;  // PartnerCostumeSlot: which saved customization slot
    bool modelEquip = false;     // ModelEquip=1: swap the look with the game's EquipCharacterItemSet (experimental)
    bool assaultReturnMove = false; // AssaultReturn=Move: main back after the partner's one move (TTT2); Landing: at the floor
    std::string assaultSound;    // Wwise events posted at the assault ('+' = several), [Effects] AssaultSound
    std::string returnSound;     // ... when the main comes back
    std::string soundTest;       // debug: F10 posts the next event of this comma list
    std::string slotOps, slotWatch, slotBp;   // debug slot lab (slotlab.cpp): F12 ops on the player structures, watched slots
    int assaultWindowMs = 500;   // a tag assault is possible only this long after the screw lands on the opponent
    bool wallScrew = false;      // (11-22: off by default) screws also work near a wall (once per combo, like everywhere else)
    // TTT2-style HUD drawn over the game's health bars (fractions of the game window)
    // the game's P1 health bar is a parallelogram: top edge hudX0..hudX1 at hudY, bottom edge shifted by hudSlant,
    // height hudH (fractions of the 16:9 game window, measured on 720p frames). P2 mirrors it.
    // measured 11-21 on the user's game (2560x1440, current HUD: teal/orange bar); the 720p video HUD was older
    double hudY = 0.0840, hudH = 0.0319, hudX0 = 0.1273, hudX1 = 0.4281, hudSlant = 0.0148;
    // the bar with its copper frame (P1 side): top-left, top-right, bottom-right, bottom-left (x, y fractions)
    double hudFrame[8] = {0.1156, 0.0781, 0.4297, 0.0781, 0.4539, 0.1215, 0.1348, 0.1215};
    double hudBackH = 0.6;       // TTT2 layout: the partner's bar = the same bar, this share of the height, right above
    double hudBackShift = 0.006; // ... and shifted outward by this fraction of the width
    double hudPortX = 0.064, hudPortY = 0.031, hudPortW = 0.053, hudPortH = 0.132;   // the game's portrait (P1 side)
    int  maxHealth = 175;         // T7 health (all characters), only used to convert RecoverPerSec
    int  redPercent = 40;        // share of damage that is recoverable
    double recoverPerSec = 4.0;  // red life recovered per second while waiting
    bool overlay = true;
    bool modelSwap = true;
    bool overlayInCapture = false;
    std::string wallTest;        // debug: "move1,move2" -> F9: move1 on a jumping opponent, move2 once it hits the wall // debug: let screenshots see the overlay (the bar grab then must not overlap it)       // the partner's look too (engine calls on the game thread, see model.cpp)
};
extern Config g_cfg;
void ConfigLoad(HMODULE self);
std::wstring ModuleDir(HMODULE self);
extern HMODULE g_selfModule;

// ---- safe memory access (SEH guarded) ----
bool SafeRead(uintptr_t addr, void* out, size_t size);
bool SafeWrite(uintptr_t addr, const void* in, size_t size);
template <class T> inline bool Rd(uintptr_t a, T& v) { return SafeRead(a, &v, sizeof(T)); }
template <class T> inline bool Wr(uintptr_t a, const T& v) { return SafeWrite(a, &v, sizeof(T)); }
std::string ReadCStr(uintptr_t addr, size_t max = 64);

// ---- game ----
uintptr_t GameBase();
bool GameIsOnline();                 // in a Steam lobby: all moveset changes must be off
uintptr_t GamePlayer(int side);      // 0 = P1, 1 = P2; 0 if not in a match

// Player structure offsets (latest Steam build). Sources: TKMovesets game_addresses.ini, TekkenBot memory_address.ini.
namespace Off {
constexpr uintptr_t CurMove   = 0x220;   // Move* playing now
constexpr uintptr_t MoveTimer = 0x1D4;   // frame of the current move (99999 ends it)
constexpr uintptr_t MoveId    = 0x350;   // u16 current move id
constexpr uintptr_t LastHit   = 0x360;   // i32 damage of the last hit taken (not the total; verified 11-20)
constexpr uintptr_t HealthEnc = 0x14E8;  // u32 health, encrypted by the game: copied as-is, never computed
constexpr uintptr_t HealthPct = 0x1508;  // i32 health percent 0..100 (verified 11-20: 100 -> 85 after 26 of 175)
constexpr uintptr_t Rage      = 0x99C;   // rage flag                                 [TekkenBot, unverified]
constexpr uintptr_t NextMove  = 0xDA0;   // Move* to play next
constexpr uintptr_t CamMota   = 0x14A0;  // 2 MOTA pointers (throw cameras)
constexpr uintptr_t Motbin    = 0x1520;  // 5 moveset pointers
}
// MovesetInfo
namespace Mi {
constexpr uintptr_t Init = 0x2, Name = 0x8, CurAliases = 0x98, Table = 0x150, Motas = 0x280;
constexpr int T_Requirement = 1, T_StartProp = 10, T_EndProp = 11, T_Move = 12;
constexpr uintptr_t MoveStride = 0xB0;
}

// ---- features ----
void FocusInstall();                 // IAT hooks + window subclass (call once window exists or from thread)
void FocusTick();                    // find & subclass the game window when it appears
HWND GameWindow();                   // the subclassed game window (or null)
bool GameWindowInFront();            // real foreground check (not the faked one)
bool InputBlocked();                 // focus fix on and another window really in front: game input must read as idle
void DInputHook(void* directInput8); // filter DirectInput devices created from this interface
void InputSynthPress(int vk);         // keyboard: the game sees this key released for one poll, then pressed (3 polls)
void NoRageTick();                   // patch / restore Rage Art entry cancels
void NoRageRestoreAll();
void NoRagePatchMoveset(uintptr_t info);

enum PickPhase { PH_NONE, PH_MODE, PH_MAIN, PH_PARTNER, PH_SOLO, PH_DONE };   // TTT2 pick steps (select.cpp)
// tag
void TagInit();                      // index the tag movesets
void TagTick();                      // call every few ms
struct OverlaySide {
    bool show = false;               // in a fight with a partner
    char activeName[40]{}, waitingName[40]{};
    int activePct = 100, activeRed = 0;    // health percent, recoverable (red) percent
    int waitingPct = 100, waitingRed = 0;
    bool assault = false;                  // the partner is in on an assault (both bars flash)
    // every fighting side (tag or not): for the bar grab check and the portrait cache
    bool fighting = false;
    int curPct = 100;                      // live health percent of whoever is in control
    int modelId = -1;                      // character the game loaded (its portrait is on the HUD)
    int activeId = -1;                     // character in control (tag sides)
    bool hudIcon = false;                  // the game's own HUD portrait shows activeId (no drawn portrait needed)
    // character select
    bool select = false, armed = false, tagMode = false, picking = false, needMove = false;
    int phase = 0;                         // PickPhase
    int curCol = -1, curRow = -1, mainCol = -1, mainRow = -1;   // grid cells for the MAIN / PARTNER labels
    char firstName[40]{}, hoverName[40]{}, partnerName[40]{};
};
void TagOverlaySnapshot(OverlaySide out[2]);
// model swap (model.cpp / engine.cpp)
namespace ue { bool InstallHook(); bool InstallFrameHook(void (*cb)()); bool FrameHookReady(); }
void TagFrame();                     // game thread, once per battle frame (frame hook): the fight logic
bool ModelAvailable();
void ModelPrepare(int side, int mainId, int partnerId, int oppId);   // match setup: the partner's costume meshes
void ModelShow(int side, int member);                     // 0 = main's look, 1 = partner's
void ModelHudIcon(int side, int charId);                   // the game's HUD portrait of this side -> that character
bool ModelHudIconShows(int side, int charId);              // the HUD portrait was switched to charId
void ModelHudIconRestore(int side, int charId);            // new match: the game's own portrait back if we changed it
void ModelForget(int side);
void ModelSpawnDouble(int side, int playerNumber = -1);   // research: a look-only second character actor
extern uintptr_t g_doubleActor;
bool ModelDoll(int side, int member, bool show);   // the look-only second actor of that side, dressed as that member
bool DollBegin(int side, int member, int frames, uintptr_t idleMove = 0, uintptr_t startMove = 0);  // doll.cpp: the outgoing member keeps playing its move (-1 = rest of it); idleMove: then stands until DollEnd
uintptr_t DollSource();                            // the running doll's player copy (0 if none)
int DollSide();
void DollTick();                                   // every battle frame (game thread)
void DollEnd();
bool DollActive();
void ModelWarmup();
void PeTraceStart(int ms);
void RecorderFrame(int active, bool assault, bool doll);   // recorder.cpp: black box, F8
// controllers (pad.cpp)
bool PadTagDown(int side);
int PadAttack(int side);                           // attack button held as 'U' 'I' 'J' 'K', 0 = none
unsigned short PadButtons(int side);
void PadSynthPress(int side, int vk);
void PadConfig(const char* tagName);
bool PickPadKey(int vk);                           // select.cpp: one new press; true = hide it from the game
bool TagInFight();                                 // tag.cpp
bool TagSide(int side);
void CostumeInstall();                             // costume.cpp
void CostumePick(int s, bool partner, int id, int pos, uintptr_t sel);   // select screen (the list position)
void CostumeClear(int s);
int CostumeLoads(int side);                        // how often the game has loaded that side's fighter
uintptr_t CostumeSet(int side, int partnerId);      // the partner's finished costume set (0 = none)
void OverlayStart();

// character select (select.cpp): first pick + partner through the game's own select screen
struct SelectState {
    volatile int phase[2] = {PH_NONE, PH_NONE};  // TTT2 pick steps per side (select.cpp)
    volatile bool open = false;               // the game's select screen is up (a cursor exists)
    volatile bool active = false;             // ... and takes input (no dialog): only then keys may be hidden
    volatile int mainCol[2] = {-1, -1}, mainRow[2] = {-1, -1};   // grid cell of the main pick (for its label)
    volatile bool tagMode[2] = {true, false}; // TTT2: TAG or SOLO chosen first on the select screen
    volatile bool armed[2] = {false, false};  // TAG and the main (first pick) is chosen
    volatile int firstId[2] = {-1, -1};
    volatile int partnerId[2] = {-1, -1};
    volatile int hoverId[2] = {-1, -1};      // last id seen by the hooks for this side
    volatile ULONGLONG hoverAt[2] = {0, 0};   // last cursor move on this side in this select session (0 = none yet)
    volatile bool visited[2] = {false, false};// the select screen was used since the last match setup
    volatile bool done = false;               // the game committed the picks (setB): no more swallowing
    volatile int mainSlot[2] = {-1, -1};      // 11-23 costume slots from the game's list (0-3 Default 1-4, then custom)
    volatile int partnerSlot[2] = {-1, -1};
    volatile bool slotDone[2] = {false, false};
    volatile int mainTotal[2] = {0, 0}, partnerTotal[2] = {0, 0};   // list sizes (custom = the last 10)
    volatile int bossHover[2] = {-1, -1};     // 11-26 boss version switched on (O / pad Y) for this character id
    volatile int mainBoss[2] = {-1, -1};      // character id of the main / solo pick played as its boss version
    volatile bool partnerBoss[2] = {false, false};
};
// 11-26 boss versions (story moves, versus damage): character id -> its .t7p key and shown name, nullptr = none
constexpr int kBossKey = 1000;
const wchar_t* BossName(int charId);
uintptr_t BossSet(int charId);
void ModelBossSolo(int side, int mainId);  // model.cpp: a solo boss main's extra parts (wings / hair)       // model.cpp: the boss version's story costume set (game thread)
extern SelectState g_sel;
bool SelectInstall();                // patch the select/load character setters
int SelectCursorId(int side);        // hovered character id at the select screen (-1 if unknown)
int SelectHover(int side);           // the select screen's cursor character (255 = none), read from the game
bool SelectCell(int side, int& col, int& row);   // the cursor's grid cell (col 0..18, row 1 top .. 3 bottom)
void OverlayToast(const wchar_t* text, int ms);          // a short message on the game screen (overlay)
void SlotLabTick();                  // debug: player-slot experiments (slotlab.cpp)
void BattleSound(int side, const std::string& names);   // Wwise events ('+' list) on that player's actor, game thread
void UiSound(const char* wwiseEvent);           // play a game UI sound (Wwise event by name), game thread
uintptr_t PlayerFromList(int side);  // the game's own player list (GetPlayerFromID)
const char* CharName(int id);        // Korean display name by character id (null if unknown)
// TTT2 pick flow: in TAG mode the first confirm press picks the main and never reaches the game; the second one
// is the game's normal confirm (= partner). Every input path calls this for each key the game would see as down.
bool PickFilterKey(int vk);          // true = hide this key from the game
void SelectTick2();                  // select-screen logic (tag thread)
void SelectReset();                  // a fight ended: next select screen starts fresh
void SelectPause();                  // a fight is running: no key is ever hidden
int PickSide();                      // side currently picking (-1 none)
void PickConfirmed(int side, int id); // the game's confirm (setA hook)
