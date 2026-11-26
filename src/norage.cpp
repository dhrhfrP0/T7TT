// Removes Rage Arts while keeping Rage Drives.
//
// Rage Art moves are found two ways (verified offline on all 56 movesets in tkdata.bin, tools/audit_rage2.py):
//   * extra/start/end property 0x81BE "Rage Art FX" with a non-zero value, or
//   * a move name containing "RageArts" (the Xx_RageArts00 startup move has no FX property).
// Rage Drives spend rage (0x8272) but never carry 0x81BE nor that name, so they stay untouched.
//
// Every cancel that goes from a normal move into a Rage Art move gets its requirement list swapped for our own
// never-true list. The original pointer and option bits are remembered so the change can be undone (online).
#include "common.h"
#include <vector>
#include <unordered_map>
#include <cstring>

namespace {
// MovesetInfo layout (t7-structures.md)
constexpr uintptr_t kMotbinOffset   = 0x1520;  // 5 moveset qwords in the player: 0x1520..0x1540
constexpr uintptr_t kInfoInit       = 0x002;
constexpr uintptr_t kInfoCurAliases = 0x098;
constexpr uintptr_t kInfoTable      = 0x150;
enum { T_Cancel = 6, T_GroupCancel = 7, T_Move = 12 };
constexpr uintptr_t kMoveStride = 0xB0, kCancelStride = 0x28;

constexpr uint32_t kReqEnd = 881;
constexpr uint32_t kReqNeverTrue = 901;     // unknown condition in T7's evaluator -> false (TKMovesets uses it)
constexpr uint32_t kPropRageArtFx = 0x81BE;

// Our never-true requirement list. Lives in the dll for its whole life.
struct Req { uint32_t cond, param; };
alignas(8) Req g_neverTrue[2] = {{kReqNeverTrue, 0}, {kReqEnd, 0}};

struct Patched { uintptr_t origReq; uint16_t origOption; };
std::unordered_map<uintptr_t, Patched> g_patched;   // cancel address -> original values
bool g_wasOnline = false;

struct Table { uintptr_t addr = 0; uint64_t count = 0; };

Table GetTable(uintptr_t info, int idx) {
    Table t;
    Rd(info + kInfoTable + idx * 16, t.addr);
    Rd(info + kInfoTable + idx * 16 + 8, t.count);
    return t;
}

bool HasProp(uintptr_t list, uintptr_t stride, uintptr_t idOff, uintptr_t valOff, uint32_t endId, uint32_t want) {
    if (!list) return false;
    for (int i = 0; i < 512; ++i) {
        uint32_t id = 0, val = 0;
        if (!Rd(list + i * stride + idOff, id)) return false;
        if (id == endId) return false;
        if (id == want) { Rd(list + i * stride + valOff, val); if (val) return true; }
    }
    return false;
}

bool NameHasRageArts(uintptr_t move) {
    uintptr_t name = 0;
    if (!Rd(move, name) || !name) return false;
    char buf[64]{};
    if (!SafeRead(name, buf, sizeof(buf) - 1)) return false;
    buf[sizeof(buf) - 1] = 0;
    return strstr(buf, "RageArts") != nullptr;
}

bool IsRageArtMove(uintptr_t move) {
    uintptr_t extra = 0, start = 0, end = 0;
    Rd(move + 0x80, extra); Rd(move + 0x88, start); Rd(move + 0x90, end);
    return NameHasRageArts(move)
        || HasProp(extra, 0x0C, 0x4, 0x8, 0, kPropRageArtFx)
        || HasProp(start, 0x10, 0x8, 0xC, kReqEnd, kPropRageArtFx)
        || HasProp(end, 0x10, 0x8, 0xC, kReqEnd, kPropRageArtFx);
}

bool PatchCancel(uintptr_t c) {
    uintptr_t req = 0; uint16_t opt = 0;
    if (!Rd(c + 0x08, req) || !Rd(c + 0x26, opt)) return false;
    if (req == (uintptr_t)g_neverTrue) return false;          // already ours
    // Option bits 2 and 4 = "has a requirement id >= 0x32C / >= 0x349"; 901 is both.
    uint16_t newOpt = opt | 0x2 | 0x4;
    if (Wr(c + 0x08, (uintptr_t)g_neverTrue) && Wr(c + 0x26, newOpt)) {
        g_patched[c] = {req, opt};
        return true;
    }
    return false;
}

// Returns number of cancels patched in this moveset.
int PatchMoveset(uintptr_t info) {
    uint8_t init = 0;
    if (!Rd(info + kInfoInit, init) || init != 1) return 0;
    Table moves = GetTable(info, T_Move), groups = GetTable(info, T_GroupCancel);
    if (!moves.addr || moves.count == 0 || moves.count > 20000) return 0;

    uint16_t aliases[56]{};
    SafeRead(info + kInfoCurAliases, aliases, sizeof(aliases));

    std::vector<uint8_t> isRA((size_t)moves.count, 0);
    for (uint64_t i = 0; i < moves.count; ++i) isRA[i] = IsRageArtMove(moves.addr + i * kMoveStride);

    auto resolve = [&](uint16_t id) -> int64_t {
        if (id >= 0x8000) { uint32_t a = id - 0x8000; if (a >= 56) return -1; id = aliases[a]; }
        return id < moves.count ? id : -1;
    };
    auto targetsRA = [&](uintptr_t c) {
        uint16_t mid = 0; Rd(c + 0x24, mid);
        int64_t t = resolve(mid);
        return t >= 0 && isRA[(size_t)t];
    };

    int patched = 0;
    auto walk = [&](uintptr_t list) {
        if (!list) return;
        for (int i = 0; i < 4096; ++i) {
            uintptr_t c = list + i * kCancelStride;
            uint64_t cmd = 0;
            if (!Rd(c, cmd)) return;
            if (cmd == 0x800B) {                       // jump into a grouped-cancel list (row index in move_id)
                uint16_t row = 0; Rd(c + 0x24, row);
                if (groups.addr && row < groups.count) {
                    for (int g = 0; g < 4096; ++g) {
                        uintptr_t gc = groups.addr + (row + g) * kCancelStride;
                        uint64_t gcmd = 0;
                        if (!Rd(gc, gcmd) || gcmd == 0x800C) break;
                        if (targetsRA(gc) && PatchCancel(gc)) ++patched;
                    }
                }
            } else if (cmd != 0x8000 && targetsRA(c) && PatchCancel(c)) {
                ++patched;
            }
            if (cmd == 0x8000) return;
        }
    };

    for (uint64_t i = 0; i < moves.count; ++i) {
        if (isRA[i]) continue;                         // cancels inside the Rage Art chain are unreachable anyway
        uintptr_t m = moves.addr + i * kMoveStride;
        uintptr_t main = 0; Rd(m + 0x20, main); walk(main);
        // secondary cancel lists, present when their gate is >= 1
        const uintptr_t sec[3][2] = {{0x28, 0x30}, {0x38, 0x40}, {0x48, 0x50}};
        for (auto& s : sec) {
            int32_t gate = 0; uintptr_t lst = 0;
            Rd(m + s[1], gate); Rd(m + s[0], lst);
            if (gate >= 1) walk(lst);
        }
    }
    return patched;
}
}  // namespace

// Patch one moveset right away (tag partners are installed before they are ever slotted on a player).
void NoRagePatchMoveset(uintptr_t info) {
    if (!g_cfg.noRageArts || GameIsOnline()) return;
    int n = PatchMoveset(info);
    Log("norage: partner moveset %p: %d rage art entries blocked", (void*)info, n);
}

void NoRageRestoreAll() {
    int n = 0;
    for (auto& [c, p] : g_patched) {
        uintptr_t cur = 0;
        if (Rd(c + 0x08, cur) && cur == (uintptr_t)g_neverTrue) {   // only undo what is still ours
            Wr(c + 0x08, p.origReq); Wr(c + 0x26, p.origOption); ++n;
        }
    }
    if (!g_patched.empty()) Log("norage: restored %d cancels", n);
    g_patched.clear();
}

void NoRageTick() {
    bool online = GameIsOnline();
    if (online != g_wasOnline) {
        Log("norage: online=%d", online);
        g_wasOnline = online;
    }
    if (online || !g_cfg.noRageArts) {                 // online: never desync the opponent
        if (!g_patched.empty()) NoRageRestoreAll();
        return;
    }
    // Re-scanned every tick: a new match can load a new moveset at the same address. Already-patched
    // cancels are skipped, so only fresh patches are counted and logged.
    uintptr_t done[10]{}; int nDone = 0;
    for (int side = 0; side < 2; ++side) {
        uintptr_t pl = GamePlayer(side);
        if (!pl) continue;
        for (int k = 0; k < 5; ++k) {
            uintptr_t info = 0;
            if (!Rd(pl + kMotbinOffset + k * 8, info) || !info) continue;
            bool dup = false;
            for (int d = 0; d < nDone; ++d) dup |= done[d] == info;
            if (dup) continue;
            done[nDone++] = info;
            int n = PatchMoveset(info);
            if (n > 0) {
                char name[40]{}; uintptr_t np = 0;
                if (Rd(info + 0x8, np)) SafeRead(np, name, sizeof(name) - 1);
                Log("norage: P%d moveset %p (%s): %d rage art entries blocked", side + 1, (void*)info, name, n);
            }
        }
    }
}
