// Costumes (11-23): both team members wear what was picked in the game's own costume list. All facts below were read
// from the game's code and checked against its own calls (the earlier ways - list position overwrite, calling
// LoadItemSetFromCustomizeData - were wrong: the main always got the saved outfit, the partner none).
//   Select element p (the setter hooks' struct): +0x10 character, +0x68 player, +0xC0A8 costume type, +0xC298 custom slot,
//     +0x70 = record: per character an entry at record+0x130 + id * 0x28C (data at +0xD8, 0x1AC bytes; current id +0xC034).
//   Confirm (0x485560): list position -> type / slot:  < nDefault [+0x4E0]: type = position;  next nPreset [+0x4DC]: type =
//     7 + n;  then custom: type 6, slot = the id list at +0x528 (8 bytes each).   (select object, P2 +0x440)
//   0x40BE90(a, saveContext): for every player element with type 6: saved costume of (character, slot) -> entry data.
//   Match load: 0x292E30 copies the entry data to the per-player block (0x460 each, +4 type, +0x1B4 data: type first; the
//     character ids are at manager +0xC + 4 * player, manager = block0 - 0xB8). Then, inside the loading task, 0x40B5D0(obj,
//     task, player) turns the block into a finished CharacterItemSet (defaults, presets, customs alike, waits for assets)
//     and 0x5736B0(manager actor, set, character, player) hands it over (SetAsyncLoadItemSet).
// Here: at 0x40BE90 the entries of the partner AND the main are filled with their own choice; at 0x40B5D0 the block is the
// partner's for one extra call first, and the set handed over is kept for the partner's look instead.
#include "common.h"
#include <cstring>
#include <string>
#include <functional>
#include <intrin.h>

namespace ue { std::string Name(uintptr_t obj); void SetRoot(uintptr_t obj, bool on); }
namespace {
uintptr_t Va(uintptr_t va) { return GameBase() + va - 0x140000000; }
constexpr size_t kData = 0x1AC, kBlock = 0x460;
struct Pick { int id = -1, type = 0, slot = -1; };
Pick g_main[2], g_partner[2];
struct PartnerData { int id = -1, type = 0; bool have = false; uint8_t data[kData]{}; };
PartnerData g_data[2];
uintptr_t g_block0 = 0;                       // player 0's block (learned from the game's own copy 0x292E30)
struct Ready { int id = -1; uintptr_t set = 0; };
Ready g_ready[2];
volatile int g_building = -1;                 // the partner of this side is being built now (hand-over is ours)
using FnApply = void(__fastcall*)(uintptr_t a, uintptr_t save);
using FnBuild = void(__fastcall*)(uintptr_t self, uintptr_t task, int player);
using FnHand = void(__fastcall*)(uintptr_t cm, uintptr_t set, int character, int player);
using FnCopy = void*(__fastcall*)(uint8_t*, const uint8_t*);
FnApply g_origApply = nullptr;
FnBuild g_origBuild = nullptr;
FnHand g_origHand = nullptr;
FnCopy g_origCopy = nullptr;

uint32_t Sum(const uint8_t* p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u; return h; }

// a 12-byte jump over `n` whole instructions (no rip-relative ones among them); returns the trampoline
void* Detour(uintptr_t at, const uint8_t* expect, size_t n, void* to) {
    if (n < 12 || memcmp((void*)at, expect, n)) return nullptr;
    auto t = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    memcpy(t, expect, n);
    uint8_t j[14] = {0xFF, 0x25, 0, 0, 0, 0};                 // jmp [rip+0]
    uintptr_t back = at + n;
    memcpy(j + 6, &back, 8); memcpy(t + n, j, 14);
    uint8_t code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    memcpy(code + 2, &to, 8);
    DWORD old;
    if (!VirtualProtect((void*)at, 12, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    memcpy((void*)at, code, 12);
    VirtualProtect((void*)at, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 12);
    return t;
}

uintptr_t Element(int player) {               // 0x3D66E0: [0x34D55A0] -> vector of elements, +0x68 = player
    uintptr_t m = 0, a = 0, e = 0;
    Rd(GameBase() + 0x34D55A0, m);
    if (!m || !Rd(m, a) || !Rd(m + 8, e)) return 0;
    for (; a && a < e; a += 8) {
        uintptr_t p = 0; int pl = -1;
        if (Rd(a, p) && p && Rd(p + 0x68, pl) && pl == player) return p;
    }
    return 0;
}
uintptr_t EntryData(uintptr_t p, int id) { return p + 0x70 + 0x130 + (uintptr_t)id * 0x28C + 0xD8; }

void __fastcall HkApply(uintptr_t a, uintptr_t save) {
    for (int s = 0; s < 2; ++s) {
        uintptr_t p = g_sel.armed[s] && g_cfg.mode == Mode::Tag && !GameIsOnline() ? Element(s) : 0;
        if (!p || g_main[s].id < 0 || g_partner[s].id < 0) continue;
        int id0 = 0, cur0 = 0;
        Rd(p + 0x10, id0); Rd(p + 0x70 + 0xC034, cur0);
        auto pass = [&](const Pick& k) {
            Wr(p + 0x10, k.id); Wr(p + 0x70 + 0xC034, k.id); Wr(p + 0xC0A8, k.type); Wr(p + 0xC298, k.slot);
            g_origApply(a, save);
        };
        pass(g_partner[s]);
        PartnerData& d = g_data[s];
        d.id = g_partner[s].id; d.type = g_partner[s].type;
        d.have = SafeRead(EntryData(p, d.id), d.data, kData);
        pass(g_main[s]);
        uint8_t md[kData]{};
        SafeRead(EntryData(p, g_main[s].id), md, kData);
        Wr(p + 0x10, id0); Wr(p + 0x70 + 0xC034, cur0);      // type and slot stay the main's: that is the fighter that loads
        Log("costume: P%d partner %d type %d slot %d data %08X | main %d type %d slot %d data %08X (element char %d, entry %d)", s + 1,
            d.id, d.type, g_partner[s].slot, Sum(d.data, kData), g_main[s].id, g_main[s].type, g_main[s].slot, Sum(md, kData), id0, cur0);
    }
    g_origApply(a, save);
}

void* __fastcall HkCopy(uint8_t* block, const uint8_t* src) {
    uintptr_t b = (uintptr_t)block;
    if (!g_block0 || b < g_block0 || b > g_block0 + kBlock) g_block0 = b;
    Log("costume: match data -> block %p hash %08X (exe+%llX)", block, Sum(src, kData), (unsigned long long)((uintptr_t)_ReturnAddress() - GameBase()));
    return g_origCopy(block, src);
}

volatile int g_loads[2] = {0, 0};             // fighters loaded by the game (a new match, also "back to stage select")
void __fastcall HkBuild(uintptr_t self, uintptr_t task, int player) {
    if (player >= 0 && player < 2) ++g_loads[player];
    if (player >= 0 && player < 2 && g_block0 && g_building < 0 && g_data[player].have && g_sel.armed[player] &&
        g_cfg.mode == Mode::Tag && !GameIsOnline()) {
        PartnerData& d = g_data[player];
        uintptr_t b = g_block0 + kBlock * player, ids = g_block0 - 0xB8 + 0xC + 4 * player;
        uint8_t saved[kBlock]; int id0 = -1;
        if (SafeRead(b, saved, kBlock) && Rd(ids, id0)) {
            Log("costume: P%d building the partner's set (character %d, type %d, data %08X; main character %d)", player + 1, d.id,
                *(int*)d.data, Sum(d.data, kData), id0);
            ULONGLONG t = GetTickCount64();
            Wr(b + 4, *(int*)d.data); SafeWrite(b + 0x1B4, d.data, kData); Wr(ids, d.id);
            g_building = player;
            g_origBuild(self, task, player);
            g_building = -1;
            SafeWrite(b, saved, kBlock); Wr(ids, id0);
            Log("costume: P%d partner set: %s (%llu ms)", player + 1, g_ready[player].set ? ue::Name(g_ready[player].set).c_str() : "none", GetTickCount64() - t);
        }
    }
    g_origBuild(self, task, player);
}
void __fastcall HkHand(uintptr_t cm, uintptr_t set, int character, int player) {
    if (g_building >= 0 && player == g_building) {
        Ready& r = g_ready[player];
        if (r.set && r.set != set) ue::SetRoot(r.set, false);
        r.id = g_data[player].id; r.set = set;
        if (set) ue::SetRoot(set, true);
        return;
    }
    // 11-26 boss version of the main: the story costume instead of the picked one
    if (player >= 0 && player < 2 && g_sel.mainBoss[player] == character) {
        uintptr_t boss = BossSet(character);
        if (boss) { ue::SetRoot(boss, true); set = boss; }
    }
    g_origHand(cm, set, character, player);
}
}  // namespace

// select screen: the list position `pos` of side s as the game's confirm would read it (0x485560)
void CostumePick(int s, bool partner, int id, int pos, uintptr_t sel) {
    Pick k;
    k.id = id;
    if (pos >= 0 && sel) {
        uintptr_t o = sel + 0x440 * (uintptr_t)s;
        int nd = 0, np = 0;
        Rd(o + 0x4E0, nd); Rd(o + 0x4DC, np);
        if (nd <= 0 || pos < nd) k.type = nd > 0 ? pos : 0;   // 11-24: an unopened list has no counts (it became Custom 1)
        else if (pos < nd + np) k.type = 7 + pos - nd;
        else { k.type = 6; Rd(o + 0x528 + 8 * (uintptr_t)(pos - nd - np), k.slot); }
        Log("costume: P%d %s %d: list %d (defaults %d, presets %d) -> type %d, slot %d", s + 1, partner ? "partner" : "main", id, pos, nd, np, k.type, k.slot);
    }
    (partner ? g_partner : g_main)[s] = k;
    if (partner) g_data[s] = PartnerData{};
}
void CostumeClear(int s) { g_main[s] = Pick{}; g_partner[s] = Pick{}; g_data[s] = PartnerData{}; }

// match setup: the partner's finished set (0 = none)
uintptr_t CostumeSet(int side, int partnerId) { return g_ready[side].id == partnerId ? g_ready[side].set : 0; }

int CostumeLoads(int side) { return side >= 0 && side < 2 ? g_loads[side] : 0; }

void CostumeInstall() {
    static const uint8_t ea[] = {0x4C, 0x8B, 0xDC, 0x55, 0x49, 0x8D, 0xAB, 0xC8, 0xFE, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x30, 0x02, 0x00, 0x00};
    static const uint8_t eb[] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
    static const uint8_t eh[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x19};
    static const uint8_t ec[] = {0x48, 0x8D, 0x81, 0xB4, 0x01, 0x00, 0x00, 0xB9, 0x03, 0x00, 0x00, 0x00};
    g_origApply = (FnApply)Detour(Va(0x14040BE90), ea, sizeof ea, (void*)&HkApply);
    g_origBuild = (FnBuild)Detour(Va(0x14040B5D0), eb, sizeof eb, (void*)&HkBuild);
    g_origHand = (FnHand)Detour(Va(0x1405736B0), eh, sizeof eh, (void*)&HkHand);
    g_origCopy = (FnCopy)Detour(Va(0x140292E30), ec, sizeof ec, (void*)&HkCopy);
    Log("costume: hooks apply %d build %d hand-over %d copy %d", !!g_origApply, !!g_origBuild, !!g_origHand, !!g_origCopy);
}
