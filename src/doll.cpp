// Doll (11-23): TTT2 shows both team members at a tag (assault: the main finishes its move while the partner jumps
// in). Tekken 7's fight logic has room for 2 fighters only, so the outgoing member becomes a look-only "doll": the
// player struct is copied at the tag and the engine's own pose code keeps playing that copy's move on a spare
// motion block, shown by a second character actor. No hitboxes, no input, no game state: just the picture.
//
// Engine facts (measured 11-23, TekkenGame-Win64-Shipping.exe):
//   AnimNode_MotHead (every body part's anim graph) -> GetPlayer(PlayerNumber) 0x759120 (48-entry table; slots 2/3 =
//   empty TTT2 partner structs) -> 0x758890(player) = the player's motion block + 0x820 (bone matrices at +0x28).
//   Motion blocks are static objects fixed to player index 0/1 (0x791100 / 0x791090). A third block of the same
//   class (vtable 0x1428C9068) sits unused at 0x14350E940: its player index (+0x4520) is 2, which the block maps to
//   a hidden 7th player struct at 0x1434FEE70 (0x759140), and +0x5220 is the player the bone pass reads.
//   The battle frame ends with 0x6E0CA0: per block 0x7662D0(block, 0, 0xFF) evaluates the player's move animation
//   (move +0x10 anim, frame = player +0x1FC) into the bones, then 0x6E7020 / 0x724C90 finish them.
//   Move +0x68 = animation length in frames.
// Doll: hidden player = copy of the outgoing fighter, spare block linked to it (+ its attachment nodes copied from
// the fighter's own block), 0x758890 redirected for table slot 2 (the doll actor's PlayerNumber), slot 2 = the same
// copy (the node reads a few player fields too). Each frame the copy's move frame advances and the block is
// evaluated. At the end both structs get their original bytes back.
#include "common.h"
#include <cstring>
#include <algorithm>

namespace {
constexpr uintptr_t kSpareBlock = 0x14350E940, kHidden = 0x1434FEE70, kSpareVt = 0x1428C9068;
constexpr uintptr_t kBlockA[2] = {0x1435037E0, 0x143509090};
constexpr uintptr_t kMotionOf = 0x140758890, kBlockIdxA = 0x140791100, kBlockIdxB = 0x140791090;
constexpr uintptr_t kEval = 0x1407662D0, kApply = 0x1406E7020, kPost = 0x140724C90;
constexpr uintptr_t kSetSkeleton = 0x140785960;   // (block, skeleton +0xC20, hierarchy +0xC28, swing +0xC30): match load
constexpr size_t kPlayer = 0x3670;

using FnIdx = uintptr_t(__fastcall*)(int);
using FnEval = void(__fastcall*)(uintptr_t block, uint8_t a, int b);
using FnApply = void(__fastcall*)(uintptr_t bones, uintptr_t src, int n);
using FnPost = void(__fastcall*)(uintptr_t ctl, uintptr_t bones);
using FnSkel = void(__fastcall*)(uintptr_t block, uintptr_t skel, uintptr_t hier, uintptr_t swing);

uintptr_t Va(uintptr_t va) { return GameBase() + va - 0x140000000; }

volatile uintptr_t g_slot2 = 0;           // table slot 2: the doll actor's player; lookups always go to the spare block
uintptr_t g_spare = 0;
int g_side = -1, g_member = -1, g_left = 0, g_frame = 0;
bool g_hold = false;
int g_holdAfter = 0;                       // frames of the move left before the doll switches to standing                       // assault: after its move the doll stands (idle move, looped) until DollEnd
uintptr_t g_idleMove = 0;                  // that idle move (move struct in the leaving member's moveset)
uint8_t g_saveSlot2[kPlayer], g_saveHidden[kPlayer];
uintptr_t g_saveLink = 0;
bool g_saved = false;

volatile int g_idleSide = 0;              // whose own block slot 2 shows while no doll runs (never the stale spare)

uintptr_t __fastcall HkMotion(uintptr_t p) {
    // 11-23 crash (stage reload, exe+0x249DC7A in MotHead): a doll actor still evaluating slot 2 read the spare block,
    // whose skeleton came from the unloaded match. The spare is only used while a doll plays.
    if (p && p == g_slot2) return (g_left > 0 ? g_spare : ((FnIdx)Va(kBlockIdxA))(g_idleSide)) + 0x820;
    if (*(int*)(p + 0x151C) == 0) return ((FnIdx)Va(kBlockIdxA))(*(int*)(p + 4)) + 0x820;
    uintptr_t o = *(uintptr_t*)(p + 0x1550);
    return ((FnIdx)Va(kBlockIdxB))(*(int*)(o + 4)) + 0x820;
}

bool HookMotion() {
    static int state = 0;                  // 0 not tried, 1 hooked, -1 failed
    if (state) return state > 0;
    state = -1;
    uintptr_t at = Va(kMotionOf);
    static const uint8_t expect[] = {0x48, 0x83, 0xEC, 0x28, 0x83, 0xB9, 0x1C, 0x15, 0x00, 0x00, 0x00};
    if (memcmp((void*)at, expect, sizeof(expect))) { Log("doll: motion lookup has unexpected bytes, doll off"); return false; }
    g_spare = Va(kSpareBlock);
    g_slot2 = GamePlayer(2);
    uint8_t code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};   // mov rax, imm64 ; jmp rax
    void* t = (void*)&HkMotion; memcpy(code + 2, &t, 8);
    DWORD old;
    if (!VirtualProtect((void*)at, 12, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)at, code, 12);
    VirtualProtect((void*)at, 12, old, &old); FlushInstructionCache(GetCurrentProcess(), (void*)at, 12);
    state = 1;
    Log("doll: motion lookup redirected for slot 2 (spare block %p)", (void*)g_spare);
    return true;
}

int BoneCount(uintptr_t block) {
    uintptr_t sk = 0, data = 0; int n = -1;
    if (Rd(block + 0x820 + 0x3D8, sk) && sk && Rd(sk, data) && data) Rd(data + 8, n);
    return n;
}

void Restore() {
    if (!g_saved) return;
    uintptr_t hidden = Va(kHidden);
    memcpy((void*)g_slot2, g_saveSlot2, kPlayer);
    memcpy((void*)hidden, g_saveHidden, kPlayer);
    *(uintptr_t*)(g_spare + 0x5220) = g_saveLink;
    g_saved = false;
}
}  // namespace

bool DollActive() { return g_left > 0; }

void DollEnd() {
    if (g_side < 0) return;
    int side = g_side, member = g_member;
    g_left = 0; g_side = -1; g_member = -1; g_hold = false; g_idleMove = 0;
    Restore();
    ModelDoll(side, member, false);
}

uintptr_t DollSource() { return g_left > 0 ? Va(kHidden) : 0; }   // the doll's player copy (its position) while it runs
int DollSide() { return g_left > 0 ? g_side : -1; }

// Game thread, before the tag changes anything: the outgoing member keeps playing its move as a doll.
// idleMove != 0: after the move the doll stands in that move (looped) until DollEnd (TTT2 assault: the point
// character stays on screen while the partner attacks).
bool DollBegin(int side, int member, int frames, uintptr_t idleMove, uintptr_t startMove) {
    if (!g_cfg.doll || side < 0 || side > 1) return false;
    if (g_side >= 0) DollEnd();
    uintptr_t p = GamePlayer(side), hidden = Va(kHidden), spare = Va(kSpareBlock), own = Va(kBlockA[side]);
    uintptr_t vt = 0; Rd(spare, vt);
    if (!p || vt != Va(kSpareVt)) { Log("doll: no spare motion block (vtable %p)", (void*)vt); return false; }
    // the spare block keeps whatever skeleton it got at boot (Heihachi's): give it the fighter's, the way the match
    // load does for the fighters' own blocks (0x785960)
    uintptr_t skel = 0, hier = 0, swing = 0, cur = 0;
    Rd(own + 0xC20, skel); Rd(own + 0xC28, hier); Rd(own + 0xC30, swing); Rd(spare + 0xC20, cur);
    if (skel && cur != skel) {
        ((FnSkel)Va(kSetSkeleton))(spare, skel, hier, swing);
        Log("doll: spare block gets P%d's skeleton (%d bones)", side + 1, BoneCount(spare));
    }
    int bs = BoneCount(spare), bo = BoneCount(own);
    if (bs <= 0 || bs != bo) { Log("doll: spare block skeleton (%d bones) is not P%d's (%d bones): no doll", bs, side + 1, bo); return false; }
    if (!HookMotion()) return false;
    uintptr_t slot2 = g_slot2;
    if (!slot2) return false;
    // length: what is left of the move (move +0x68 = animation frames), at least a few frames
    uintptr_t mv = 0; Rd(p + Off::CurMove, mv);
    int len = 0, now = 0; if (mv) Rd(mv + 0x68, len); Rd(p + Off::MoveTimer, now);
    if (startMove) { mv = startMove; now = 0; len = 0; Rd(mv + 0x68, len); if (frames < 0) frames = len; }
    uintptr_t np0 = 0; if (mv) Rd(mv, np0);
    if (!idleMove && !startMove && frames < 0 && ReadCStr(np0, 40).find("sKAM") != std::string::npos) return false;   // just standing: no doll
    if (frames < 0) frames = len > now ? len - now : 0;
    frames = (std::min)((std::max)(frames, g_cfg.dollMinFrames), g_cfg.dollMaxFrames);
    if (!ModelDoll(side, member, true)) { Log("doll: no doll actor"); return false; }

    memcpy(g_saveSlot2, (void*)slot2, kPlayer); memcpy(g_saveHidden, (void*)hidden, kPlayer);
    g_saveLink = *(uintptr_t*)(spare + 0x5220); g_saved = true;
    // the copies keep their own first bytes (index / flags) so the game still sees inactive slots; slot 2's index
    // points at the fighter so lookups that bypass the hook never meet -1 (0x791100 writes to address 0 on that)
    memcpy((void*)(hidden + 0x10), (void*)(p + 0x10), kPlayer - 0x10);
    memcpy((void*)(slot2 + 0x10), (void*)(p + 0x10), kPlayer - 0x10);
    *(int*)(slot2 + 4) = side;
    *(int*)(slot2 + 0xD4) = *(int*)(g_saveSlot2 + 0xD4);
    *(int*)(hidden + 0x151C) = 0; *(int*)(slot2 + 0x151C) = 0;
    *(uintptr_t*)(spare + 0x5220) = hidden;
    if (startMove)                                           // a different move from its first frame (raw tag: back dash out)
        for (uintptr_t q : {hidden, slot2}) {
            *(uintptr_t*)(q + 0x218) = startMove; *(uintptr_t*)(q + Off::CurMove) = startMove;
            for (uintptr_t o : {0x1D4, 0x1D8, 0x1DC, 0x1E0, 0x1E4, 0x1E8, 0x1F0, 0x1F4}) *(int*)(q + o) = 0;
            for (uintptr_t o : {0x1F8, 0x1FC, 0x200}) *(float*)(q + o) = 0.0f;
        }
    // the spare block's attachment nodes (+0x5460..+0x58B0) were never set up: the fighter's block part, moved over
    uintptr_t buf0 = 0, buf = 0; Rd(own + 0x848, buf0); Rd(spare + 0x848, buf);
    if (buf0 && buf) {
        memcpy((void*)(spare + 0x5460), (void*)(own + 0x5460), 0x58B0 - 0x5460);
        for (uintptr_t o = 0x5460; o + 8 <= 0x58B0; o += 8) {
            uintptr_t v = *(uintptr_t*)(spare + o);
            if (v >= buf0 && v < buf0 + 0x20000) *(uintptr_t*)(spare + o) = buf + (v - buf0);
            else if (v >= own && v < own + 0x58B0) *(uintptr_t*)(spare + o) = spare + (v - own);
        }
    }
    g_side = side; g_member = member; g_frame = 0; g_idleSide = side;
    g_hold = idleMove != 0; g_idleMove = idleMove;
    g_left = g_hold ? 0x7FFFFFFF : frames;
    g_holdAfter = frames;
    uintptr_t np = 0; if (mv) Rd(mv, np);
    Log("doll: P%d member %d leaves as a doll: %s frame %d/%d, %d frames%s", side + 1, member, np ? ReadCStr(np, 40).c_str() : "?",
        now, len, frames, g_hold ? ", then stands until the return" : "");
    DollTick();                                   // this frame's pose already (the actor shows it right away)
    return true;
}

// Game thread, every battle frame after the game's own pose pass.
void DollTick() {
    if (g_left <= 0 || g_side < 0) return;
    uintptr_t hidden = Va(kHidden), slot2 = g_slot2;
    // hold: the move is over -> the idle move from its first frame, looped (+0x218 = the move whose animation plays)
    bool restart = false;
    if (g_hold && g_frame == g_holdAfter && g_idleMove) restart = true;
    if (g_hold && g_frame > g_holdAfter && g_idleMove) {
        int len = 0, now = 0; Rd(g_idleMove + 0x68, len); Rd(hidden + Off::MoveTimer, now);
        if (len > 2 && now >= len - 1) restart = true;
    }
    for (uintptr_t q : {hidden, slot2}) {
        if (restart) {
            *(uintptr_t*)(q + 0x218) = g_idleMove; *(uintptr_t*)(q + Off::CurMove) = g_idleMove;
            for (uintptr_t o : {0x1D4, 0x1D8, 0x1DC, 0x1E0, 0x1E4, 0x1E8, 0x1F0, 0x1F4}) *(int*)(q + o) = 0;
            for (uintptr_t o : {0x1F8, 0x1FC, 0x200}) *(float*)(q + o) = 0.0f;
        }
        for (uintptr_t o : {0x1D4, 0x1D8, 0x1DC, 0x1E0, 0x1E4, 0x1E8, 0x1F0, 0x1F4}) *(int*)(q + o) += 1;
        for (uintptr_t o : {0x1F8, 0x1FC, 0x200}) *(float*)(q + o) += 1.0f;
    }
    ((FnEval)Va(kEval))(g_spare, 0, 0xFF);
    uintptr_t bones = 0; Rd(g_spare + 0x820 + 0x28, bones);
    if (bones) {
        ((FnApply)Va(kApply))(bones, hidden + 0x130, 0x16);
        ((FnPost)Va(kPost))(g_spare + 0x4AC0, bones);
    }
    ++g_frame;
    if (g_hold && g_frame > 60 * 12) { Log("doll: P%d held too long, ended", g_side + 1); DollEnd(); return; }
    if (!g_hold && --g_left <= 0) { Log("doll: P%d done after %d frames", g_side + 1, g_frame); DollEnd(); }
}
