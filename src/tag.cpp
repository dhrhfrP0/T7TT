// Tag mode (plan A): each side may have a point character (first pick, the visible model) and a partner, both
// picked on the game's select screen (select.cpp). A tag swaps the moveset, health and rage of the player object,
// then forces a move:
//   neutral tag  -> partner's Co_ChgInJmpBalS (TTT2 leftover jump-in)
//   tag assault  -> partner's T7Tag_Assault (copy of its forward dash + white flash, impact sound, light slow
//                   motion on the opponent): the partner chases the screwed opponent and continues the combo.
//   return       -> T7Tag_Return (standing copy + white flash): the character who started the assault takes over.
// Assault flow (11-20/21 feedback): the tag key may be pressed during your own attack; the assault fires the moment
// the opponent is screwed, and only within AssaultWindowMs of it. Control comes back automatically after the screwed
// opponent has landed: when the partner stands idle ReturnIdleMs after the landing, or the combo is over (never
// earlier than ReturnIdleMs after the landing, so the partner can run after a far screw), or at once on the tag key.
// Health is encrypted by the game: the raw value is saved and restored, never computed (Off::HealthEnc).
// Wall screw: the tag movesets carry an extra hit-condition row "-> screw" behind every [615] row. Its first
// requirement is switched here: 881 (end of list = always) while the opponent is in a combo and has not been screwed
// in it, else 901 (never). 615 alone is false near/at a wall; the old [614 juggled] switch was not enough at the wall.
#include "common.h"
#include <vector>
#include <map>
#include <mutex>
#include <cstring>
#include <cmath>

bool (*g_tagInput)(int side) = nullptr;   // test hook; null = keyboard / AI
bool (*g_fightingOverride)() = nullptr;   // test hook

namespace {
struct Blob {
    bool ok = false;
    std::vector<uint8_t> image;            // image without common.arc, pointers as offsets
    std::vector<uint32_t> relocs;
    uint16_t charId = 0;
    char code[9]{}, name[33]{};
    uint32_t assault = 0, effOff = 0, effRows = 0, standing = 0, ret = 0, retEffOff = 0, assaultBase = 0, wallReq = 0;
    uint64_t comOff = 0, comSize = 0, imageSize = 0;
};

constexpr uint32_t kVersion = 6;   // v5: T7Tag_Entry (= return + 1), entries locked; v6: T7Tag_EntryRaw (= return + 2)

struct HpState { uint32_t enc = 0; int pct = 100; };

struct Member {
    uintptr_t moveset = 0;
    int assault = -1;                      // T7Tag_Assault move id in this moveset (-1: none)
    int ret = -1;                          // T7Tag_Return move id
    int entry = -1;                        // T7Tag_Entry move id (dash, 20 locked frames: tag combo)
    int entryRaw = -1;                     // T7Tag_EntryRaw move id (standing, 45 locked frames: raw / standing tag)
    HpState hp;                            // raw health while waiting
    double red = 0;                        // recoverable percent
    double recovered = 0;                  // percent recovered so far (fractional accumulator)
    std::vector<HpState> history;          // health states the game produced for this member (red life restores these)
    uint32_t rage = 0;
    int id = -1;                           // character id
    char name[40]{};
};

struct Side {
    bool tag = false;                      // this match has a partner on this side
    bool seen = false;                     // a match was set up (tag or solo)
    uintptr_t game = 0;                    // the moveset the game loaded
    uintptr_t alloc[2] = {0, 0};           // our copies: point, partner
    uint32_t wallReq[2] = {0, 0};          // offset of the wall-screw switch in each copy (0 = none)
    int blobId[2] = {-1, -1};              // character id of the blob in each copy
    Member m[2];
    int active = 0;
    bool hpOk = false, rageOk = true;
    int lastPct = 100;
    ULONGLONG cooldownUntil = 0, aiNextCheck = 0;
    bool keyWasDown = false;
    // assault
    bool assault = false;
    ULONGLONG assaultT0 = 0, neutralSince = 0, oppFreeSince = 0;
    bool attacked = false;
    bool entered = false;                  // the partner's T7Tag_Assault has started (moves before it are the main's)
    ULONGLONG pendingUntil = 0;            // tag pressed during an attack: fire the assault on the screw until then
    bool throwSeen = false;                // a throw connected while the tag was pending (tag throw)
    bool entryCut = false;                 // an attack key was pressed during the partner's locked entry
    bool entryDone = false;                // ... and the lock was ended for it
    int32_t entryBtnAt = 0;
    int entryKey = 0;
    // wall screw (this side attacking)
    bool screwUsed = false;
    ULONGLONG comboOverSince = 0;
    uint32_t wallState = 0;
    // screw timing (assault window) and the return after the landing
    bool prevScrew = false;
    ULONGLONG screwSince = 0, landedAt = 0, idleSince = 0;
    int modelId = -1;
    std::string lastOpp;                   // combo trace (log) for wall-screw checks
    int traceLeft = 0;
    ULONGLONG posUntil = 0, posNext = 0;   // position log after a tag (teleport checks)
    float tagPos[3]{};                     // position when the tag was pressed (teleport fix)
    std::string tagFrom;                   // move playing when the tag forced the new one
    ULONGLONG fixUntil = 0;                // hold-on-the-tag-spot window (entry move)
    bool fixHeld = false;
    int loads = -1;                        // CostumeLoads at the match setup
    bool entryPad = false;                 // the entry's button came from a controller
    bool returnPending = false;            // tag key pressed during the partner's attack (assault): return after it
};

Side g_side[2];
std::mutex g_mx;
ULONGLONG g_lastTick = 0;
// 11-22 rebuild step 2: fight timing in game frames. With the battle frame hook, "now" = battle frames run so far
// (60 per second): stops while the game is paused, and both machines of an online match would count the same.
// Without the hook (and in the offline test harness) it is the wall clock as before.
ULONGLONG g_frames = 0;
ULONGLONG NowMs() { return ue::FrameHookReady() ? g_frames * 1000 / 60 : GetTickCount64(); }
bool g_rageConfirmed = false;              // rage offset proven (became 1 at low health)
std::vector<uint8_t> g_common;
std::map<int, std::wstring> g_files;       // character id -> .t7p path
std::map<int, Blob> g_blobs;
int32_t g_lastTimer = 0;
ULONGLONG g_timerMoved = 0;
bool g_f7WasDown = false;

std::wstring PartnerDir() { return ModuleDir(g_selfModule) + L"T7Tag_partners\\"; }

bool ReadFileAll(const std::wstring& path, std::vector<uint8_t>& out, size_t maxBytes = SIZE_MAX) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    size_t n = (size_t)(std::min)((uint64_t)sz.QuadPart, (uint64_t)maxBytes);
    out.resize(n);
    DWORD got = 0; bool ok = ReadFile(f, out.data(), (DWORD)n, &got, nullptr) && got == n;
    CloseHandle(f);
    return ok;
}

template <class T> T Get(const std::vector<uint8_t>& b, size_t o) { T v{}; memcpy(&v, &b[o], sizeof(T)); return v; }

bool ParseBlob(const std::vector<uint8_t>& raw, Blob& b, bool headerOnly) {
    if (raw.size() < 0x70 || memcmp(raw.data(), "T7TP", 4) || Get<uint32_t>(raw, 4) != kVersion) return false;
    b.charId = (uint16_t)Get<uint32_t>(raw, 8);
    uint32_t nrel = Get<uint32_t>(raw, 12);
    b.imageSize = Get<uint64_t>(raw, 16);
    memcpy(b.code, &raw[0x18], 8); memcpy(b.name, &raw[0x20], 32);
    b.assault = Get<uint32_t>(raw, 0x40); b.effOff = Get<uint32_t>(raw, 0x44);
    b.effRows = Get<uint32_t>(raw, 0x48); b.standing = Get<uint32_t>(raw, 0x4C);
    b.comOff = Get<uint64_t>(raw, 0x50); b.comSize = Get<uint64_t>(raw, 0x58);
    b.ret = Get<uint32_t>(raw, 0x60); b.retEffOff = Get<uint32_t>(raw, 0x64);
    b.assaultBase = Get<uint32_t>(raw, 0x68); b.wallReq = Get<uint32_t>(raw, 0x6C);
    if (headerOnly) return true;
    size_t imgOff = (0x70 + 4ull * nrel + 15) / 16 * 16;
    if (imgOff + b.comOff > raw.size() || b.comOff + b.comSize != b.imageSize || g_common.size() != b.comSize) return false;
    if (b.wallReq && b.wallReq + 16 > b.comOff) return false;
    b.relocs.resize(nrel);
    memcpy(b.relocs.data(), &raw[0x70], 4ull * nrel);
    for (uint32_t r : b.relocs) if (r + 8 > b.imageSize) return false;
    b.image.assign(raw.begin() + imgOff, raw.begin() + imgOff + (size_t)b.comOff);
    b.ok = true;
    return true;
}

}  // namespace

const wchar_t* BossName(int charId) {
    if (!g_files.count(charId + kBossKey)) return nullptr;
    switch (charId) {
    case 8: return L"최종전 헤이하치";
    case 9: return L"트루 데빌 카즈야";
    case 32: return L"스토리 고우키";
    case 6: return L"트레저 진";
    case 26: return L"보스 카즈미";
    default: return L"보스판";
    }
}

namespace {
Blob* GetBlob(int charId) {
    auto it = g_blobs.find(charId);
    if (it != g_blobs.end()) return it->second.ok ? &it->second : nullptr;
    Blob& b = g_blobs[charId];
    auto f = g_files.find(charId);
    std::vector<uint8_t> raw;
    if (f == g_files.end() || !ReadFileAll(f->second, raw) || !ParseBlob(raw, b, false)) {
        Log("tag: no tag moveset for character id %d", charId);
        return nullptr;
    }
    return &b;
}

int MovesetCharId(uintptr_t info) {
    uintptr_t moves = 0; uint64_t count = 0;
    if (!Rd(info + Mi::Table + Mi::T_Move * 16, moves) || !Rd(info + Mi::Table + Mi::T_Move * 16 + 8, count)) return -1;
    int hist[256]{};
    for (uint64_t i = 0; i < count && i < 400; ++i) {
        uint16_t v = 0;
        if (Rd(moves + i * Mi::MoveStride + 0x5A, v) && v < 256) ++hist[v];
    }
    int best = -1, bestN = 0;
    for (int i = 0; i < 256; ++i) if (hist[i] > bestN) { bestN = hist[i]; best = i; }
    return best;
}

uintptr_t TableAddr(uintptr_t info, int t) { return *(uint64_t*)(info + Mi::Table + t * 16); }
uint64_t TableCount(uintptr_t info, int t) { return *(uint64_t*)(info + Mi::Table + t * 16 + 8); }

// "0x8229:0, 0x84C4:0x100002B, 12@0x828B:100" -> rows (frame defaults to instant 0x8001)
std::vector<uint32_t> ParseEffects(const std::string& s) {
    std::vector<uint32_t> rows;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
        std::string item = s.substr(i, j - i);
        i = j + 1;
        uint32_t frame = 0x8001;
        size_t at = item.find('@');
        if (at != std::string::npos) { frame = strtoul(item.c_str(), nullptr, 0); item = item.substr(at + 1); }
        size_t c = item.find(':');
        if (c == std::string::npos) continue;
        uint32_t id = strtoul(item.substr(0, c).c_str(), nullptr, 0), v = strtoul(item.substr(c + 1).c_str(), nullptr, 0);
        if (!id) continue;
        rows.insert(rows.end(), {frame, id, v});
    }
    return rows;
}

// Rebuild an effect block: [ini effects][base move's own properties][end].
void WriteBlock(uintptr_t info, const Blob& b, const std::string& spec, uint32_t off, uint32_t baseMove) {
    if (spec.empty()) return;                        // keep the defaults written by the builder
    std::vector<uint32_t> rows = ParseEffects(spec);
    uintptr_t moves = TableAddr(info, Mi::T_Move);
    uintptr_t own = *(uintptr_t*)(moves + baseMove * Mi::MoveStride + 0x80);
    for (int k = 0; own && k < 64; ++k) {
        uint32_t* r = (uint32_t*)(own + k * 12);
        if (r[1] == 0) break;
        rows.insert(rows.end(), {r[0], r[1], r[2]});
    }
    rows.insert(rows.end(), {0, 0, 0});
    if (rows.size() / 3 > b.effRows) { Log("tag: too many effects (%zu rows, room %u)", rows.size() / 3, b.effRows); return; }
    memcpy((void*)(info + off), rows.data(), rows.size() * 4);
}

void WriteEffects(uintptr_t info, const Blob& b) {
    WriteBlock(info, b, g_cfg.assaultEffects, b.effOff, b.assaultBase);
    WriteBlock(info, b, g_cfg.returnEffects, b.retEffOff, b.standing);
    // entry locks from the ini (F7 reapplies): the entry moves' only cancel row = their end, at that frame
    uintptr_t moves = TableAddr(info, Mi::T_Move);
    const int frames[2] = {g_cfg.comboTagFrames, g_cfg.rawTagFrames};
    for (int k = 0; k < 2; ++k) {
        uintptr_t c = *(uintptr_t*)(moves + (uint64_t)(b.ret + 1 + k) * Mi::MoveStride + 0x20);
        if (c && *(uint64_t*)c == 0x8000) { *(uint32_t*)(c + 0x18) = (uint32_t)frames[k]; *(uint32_t*)(c + 0x20) = (uint32_t)frames[k]; }
        else Log("tag: %s entry %d has no lock row", b.code, k);
    }
    // 11-26: the end row only fires when the move is over, and the raw entry is a copy of the standing move: it lasted
    // the whole idle animation (Kazuya 1.4 s, Akuma 643 frames = 10 s, measured). Its length = the raw lock.
    *(int32_t*)(moves + (uint64_t)(b.ret + 2) * Mi::MoveStride + 0x68) = frames[1];
}

// Copy a blob into a fresh game allocation, relocate, retarget "character id is X" checks to the player's id.
uintptr_t Install(const Blob& b, int targetCharId, uintptr_t& slot) {
    if (slot) VirtualFree((void*)slot, 0, MEM_RELEASE);
    slot = (uintptr_t)VirtualAlloc(nullptr, (size_t)b.imageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!slot) return 0;
    uint8_t* p = (uint8_t*)slot;
    memcpy(p, b.image.data(), b.image.size());
    memcpy(p + b.comOff, g_common.data(), g_common.size());
    for (uint32_t r : b.relocs) *(uint64_t*)(p + r) += slot;
    int fixes = 0;
    uintptr_t a = TableAddr(slot, Mi::T_Requirement); uint64_t n = TableCount(slot, Mi::T_Requirement);
    for (uint64_t i = 0; i < n; ++i) {
        uint32_t* row = (uint32_t*)(a + i * 8);
        if (row[0] == 217) { row[1] = row[1] == b.charId ? (uint32_t)targetCharId : (uint32_t)targetCharId + 10; ++fixes; }
    }
    for (int t : {Mi::T_StartProp, Mi::T_EndProp}) {
        a = TableAddr(slot, t); n = TableCount(slot, t);
        for (uint64_t i = 0; i < n; ++i) {
            uint32_t* row = (uint32_t*)(a + i * 16 + 8);
            if (row[0] == 217) { row[1] = row[1] == b.charId ? (uint32_t)targetCharId : (uint32_t)targetCharId + 10; ++fixes; }
        }
    }
    if (b.wallReq) *(uint32_t*)(slot + b.wallReq) = g_cfg.wallScrew ? 614 : 901;
    WriteEffects(slot, b);
    Log("tag: %s installed at %p for char id %d (%d id checks retargeted, wall screw %s, entry locks combo %df raw %df)", b.code,
        (void*)slot, targetCharId, fixes, b.wallReq ? "ready" : "none", g_cfg.comboTagFrames, g_cfg.rawTagFrames);
    NoRagePatchMoveset(slot);
    return slot;
}

// ---------------------------------------------------------------- move helpers
uintptr_t MoveAddr(uintptr_t info, uint32_t id) {
    if (id >= 0x8000) { uint16_t real = 0; if (!Rd(info + Mi::CurAliases + (id - 0x8000) * 2, real)) return 0; id = real; }
    uintptr_t moves = 0; uint64_t count = 0;
    Rd(info + Mi::Table + Mi::T_Move * 16, moves); Rd(info + Mi::Table + Mi::T_Move * 16 + 8, count);
    return id < count ? moves + id * Mi::MoveStride : 0;
}

int FindMoveByName(uintptr_t info, const char* want) {
    uintptr_t moves = 0; uint64_t count = 0;
    Rd(info + Mi::Table + Mi::T_Move * 16, moves); Rd(info + Mi::Table + Mi::T_Move * 16 + 8, count);
    for (uint64_t i = 0; i < count; ++i) {
        uintptr_t np = 0; Rd(moves + i * Mi::MoveStride, np);
        if (ReadCStr(np, 48) == want) return (int)i;
    }
    return -1;
}

// first move whose name ends with `suffix` (e.g. "_sDASHF", the forward dash), -1 if none
int FindMoveBySuffix(uintptr_t info, const char* suffix) {
    uintptr_t moves = 0; uint64_t count = 0;
    Rd(info + Mi::Table + Mi::T_Move * 16, moves); Rd(info + Mi::Table + Mi::T_Move * 16 + 8, count);
    size_t L = strlen(suffix);
    for (uint64_t i = 0; i < count; ++i) {
        uintptr_t np = 0; Rd(moves + i * Mi::MoveStride, np);
        std::string n = ReadCStr(np, 48);
        if (n.size() > L && n.compare(n.size() - L, L, suffix) == 0) return (int)i;
    }
    return -1;
}

// the current move's attack is over (its last active frame has passed): from here a tag does not cut the attack
bool AttackDone(uintptr_t player) {
    uintptr_t mv = 0; int32_t frame = 0; uint32_t last = 0;
    if (!Rd(player + Off::CurMove, mv) || !mv || !Rd(player + Off::MoveTimer, frame) || !Rd(mv + 0xA4, last)) return true;
    return last == 0 || frame > (int32_t)last;
}

std::string CurMoveName(uintptr_t player) {
    uintptr_t mv = 0, np = 0;
    if (!Rd(player + Off::CurMove, mv) || !mv || !Rd(mv, np)) return {};
    return ReadCStr(np, 48);
}

bool Contains(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }
bool Starts(const std::string& s, const char* k) { return s.rfind(k, 0) == 0; }

bool IsNeutral(uintptr_t player, uintptr_t info) {
    std::string n = CurMoveName(player);
    if (n.empty()) return false;
    size_t L = n.size();
    if (L >= 8 && n.compare(L - 8, 8, "_sKAM00_") == 0) return true;
    if (Contains(n, "WALK") || Contains(n, "DASH") || Contains(n, "STEP") || Contains(n, "sRUN__") || Contains(n, "YOKE_00")) return true;
    if (n == "T7Tag_Return") return true;
    uintptr_t cur = 0; Rd(player + Off::CurMove, cur);
    return cur && (cur == MoveAddr(info, 0x8001) || cur == MoveAddr(info, 0x8002));
}

// standing still (not attacking, not moving): the partner is done
bool IsStanding(uintptr_t player, uintptr_t info) {
    std::string n = CurMoveName(player);
    size_t L = n.size();
    if (L >= 8 && n.compare(L - 8, 8, "_sKAM00_") == 0) return true;
    if (n == "T7Tag_Return") return true;
    uintptr_t cur = 0; Rd(player + Off::CurMove, cur);
    return cur && (cur == MoveAddr(info, 0x8001) || cur == MoveAddr(info, 0x8002));
}

bool IsScrewed(uintptr_t player) {
    std::string n = CurMoveName(player);
    return Starts(n, "sDw_AIR_SPIN") || Starts(n, "sDw_screw");
}

// being thrown: the attacker's throw reaction "<code>_..nage_y" (the thrower plays "..nage_n"; "Esc_" = broken)
bool OppThrown(uintptr_t opp) {
    std::string n = CurMoveName(opp);
    return Contains(n, "nage") && n.size() > 2 && n.compare(n.size() - 2, 2, "_y") == 0 && !Starts(n, "Esc_");
}

// lying on the floor / tech rolling (not airborne in a juggle: those are sDw_ / aDw_)
bool OwnDown(uintptr_t player) {
    std::string n = CurMoveName(player);
    return Starts(n, "dw") || Contains(n, "DWN") || Starts(n, "Co_AO") || Starts(n, "dDw_");
}

// Opponent still in a combo (airborne, spinning, wall hit)? Lying on the floor or standing again = combo over.
bool InCombo(uintptr_t player) {
    std::string n = CurMoveName(player);
    return Starts(n, "sDw_") || Starts(n, "aDw_") || Starts(n, "wDm_") || Contains(n, "AIR") || Contains(n, "Air") ||
           Contains(n, "SPIN") || Contains(n, "screw") || Contains(n, "Bound") || Contains(n, "bound");
}

void ForceMove(uintptr_t player, uintptr_t info, uint32_t id) {
    uintptr_t mv = MoveAddr(info, id);
    if (!mv) return;
    uint16_t real = (uint16_t)((mv - MoveAddr(info, 0)) / Mi::MoveStride);
    Wr(player + Off::MoveTimer, (int32_t)99999);
    Wr(player + Off::NextMove, mv);
    Wr(player + Off::MoveId, real);
}

void SetMoveset(uintptr_t player, uintptr_t info) {
    for (int k = 0; k < 5; ++k) Wr(player + Off::Motbin + k * 8, info);
    uintptr_t cam1 = 0, cam2 = 0;
    Rd(info + Mi::Motas + 8 * 8, cam1); Rd(info + Mi::Motas + 9 * 8, cam2);
    Wr(player + Off::CamMota, cam1); Wr(player + Off::CamMota + 8, cam2);
}

void StripName(char* dst, size_t n, uintptr_t info) {
    uintptr_t np = 0; Rd(info + Mi::Name, np);
    std::string s = ReadCStr(np, 30);
    if (!s.empty() && s.front() == '[') s.erase(0, 1);
    if (!s.empty() && s.back() == ']') s.pop_back();
    strncpy_s(dst, n, s.c_str(), _TRUNCATE);
}

void SetMemberName(Member& m, int charId, uintptr_t info) {
    const char* k = CharName(charId);
    if (k) strncpy_s(m.name, k, _TRUNCATE); else StripName(m.name, sizeof(m.name), info);
}

// ---------------------------------------------------------------- health (raw encrypted value + percent)
bool ReadHp(uintptr_t player, HpState& h) {
    int32_t pct = -1; uint32_t enc = 0;
    if (!Rd(player + Off::HealthEnc, enc) || !Rd(player + Off::HealthPct, pct)) return false;
    h.enc = enc; h.pct = pct;
    return enc != 0 && pct >= 0 && pct <= 100;
}
void WriteHp(uintptr_t player, const HpState& h) { Wr(player + Off::HealthEnc, h.enc); Wr(player + Off::HealthPct, (int32_t)h.pct); }

void HealTeam(Side& s, const HpState& full) {
    for (auto& m : s.m) { m.hp = full; m.red = 0; m.recovered = 0; m.history.assign(1, full); m.rage = 0; }
}

// ---------------------------------------------------------------- tag logic
// Neutral: standing tag. Assault: partner in on a screw, comes back out. Return: the assault ends.
// Combo (TTT2 tag combo): tag right after an attack while the opponent is juggled -> the partner dashes in and stays.
// Raw: tag right after an attack otherwise (TTT1-style tag at the move's recovery) -> partner dashes in and stays.
// Throw (TTT2 tag throw): tag during a throw that connected -> the partner comes in when the throw is over (a safe tag),
// the thrown side loses its red life. Crash (TTT2 tag crash): down + tag + right punch while the partner has rage ->
// the partner drops in (TTT2 leftover jump-in Co_ChgInJmpBalS), rage and red life used up. Down: tag while down = get
// up and swap (TTT2 raw tag from the floor).
enum class TagKind { Neutral, Assault, Return, Combo, Raw, Throw, Crash, Down };

void DoTag(int side, uintptr_t player, TagKind kind) {
    Side& s = g_side[side];
    Member& out = s.m[s.active];
    Member& in = s.m[1 - s.active];
    // 11-23 doll: the leaving member stays on screen and finishes its move (assault: the main's launcher / screw,
    // return: the partner's attack, tag combo: the main's launcher) - copied before anything below changes the player
    // 11-23 (2): the main comes back where the partner is, i.e. near the opponent. Sending it back to where it stood at
    // the assault (one build) left it 2200-12000 units away: the opponent flies on during the assault, Tekken 7 has no
    // run-in, and writing the position across a forced move made a second jump of 1900-4200 units (user's play log).
    if (kind == TagKind::Assault || kind == TagKind::Return || kind == TagKind::Combo) DollBegin(side, s.active, -1);
    else if (kind == TagKind::Neutral || kind == TagKind::Raw) {   // TTT2 raw tag: the outgoing one backs out
        int bi = FindMoveBySuffix(out.moveset, "_sDASHB");
        uintptr_t moves = 0; Rd(out.moveset + Mi::Table + Mi::T_Move * 16, moves);
        if (bi >= 0 && moves) DollBegin(side, s.active, 24, 0, moves + (uintptr_t)bi * Mi::MoveStride);
    }
    HpState now;
    bool hp = s.hpOk && ReadHp(player, now);
    uint32_t rage = 0; Rd(player + Off::Rage, rage);
    if (rage > 1) s.rageOk = false;
    if (hp) out.hp = now;
    out.rage = rage;

    SetMoveset(player, in.moveset);
    if (hp) { WriteHp(player, in.hp); s.lastPct = in.hp.pct; }
    if (s.rageOk && g_rageConfirmed) Wr(player + Off::Rage, in.rage);

    uint32_t target = 0x8001;
    if (kind == TagKind::Assault && in.assault >= 0) target = (uint32_t)in.assault;
    else if (kind == TagKind::Return && in.ret >= 0) target = (uint32_t)in.ret;
    else if (kind == TagKind::Combo && in.entry >= 0)
        target = (uint32_t)in.entry;                           // runs in; can attack after 20 frames (TTT2 ~0.45 s)
    else if ((kind == TagKind::Raw || kind == TagKind::Neutral || kind == TagKind::Down) && in.entryRaw >= 0)
        target = (uint32_t)in.entryRaw;                        // fixed pose (TTT2 raw tag: buttons 0.75 s, guard ~1 s)
    else if (kind == TagKind::Throw && in.entry >= 0)
        target = (uint32_t)in.entry;                           // quick entry: the throw covered the swap
    else if (kind == TagKind::Crash) {
        int jm = FindMoveByName(in.moveset, "Co_ChgInJmpBalS");
        target = jm >= 0 ? (uint32_t)jm : (in.entryRaw >= 0 ? (uint32_t)in.entryRaw : 0x8001);
    }
    ForceMove(player, in.moveset, target);
    s.active ^= 1;
    ModelShow(side, s.active);
    ModelHudIcon(side, in.id);
    ULONGLONG t = NowMs();
    s.cooldownUntil = t + (kind == TagKind::Neutral ? 700 : 250);
    s.pendingUntil = 0; s.throwSeen = false;
    s.assault = kind == TagKind::Assault;
    if (kind == TagKind::Crash) {                              // costs: the incoming partner's rage, both red lives
        if (s.rageOk && g_rageConfirmed) Wr(player + Off::Rage, (uint32_t)0);
        in.rage = 0; s.m[0].red = s.m[1].red = 0; s.m[0].recovered = s.m[1].recovered = 0;
    }
    if (kind == TagKind::Throw || kind == TagKind::Combo) {   // TTT2: tag throws and tag combos remove the red life
        Side& o = g_side[1 - side];
        if (o.tag) { o.m[o.active].red = 0; o.m[o.active].recovered = 0; }
    }
    if (kind == TagKind::Assault) BattleSound(side, g_cfg.assaultSound);
    else if (kind == TagKind::Return) BattleSound(side, g_cfg.returnSound);
    if (kind == TagKind::Return) {
        float me[3]{}, op[3]{}; uintptr_t opp = GamePlayer(1 - side);
        Rd(player + 0xF4, me); if (opp) Rd(opp + 0xF4, op);
        Log("pos: P%d return swap at %.0f %.0f, opponent %.0f %.0f, distance %.0f", side + 1, me[0], me[2], op[0], op[2],
            sqrtf((me[0] - op[0]) * (me[0] - op[0]) + (me[2] - op[2]) * (me[2] - op[2])));
    }
    s.posUntil = t + 450; s.posNext = 0;
    // teleport fix for the tag combo / raw tag only: the assault partner must chase the screwed opponent (11-22 test:
    // undoing the screw move's 977-unit jump left the partner far behind)
    Rd(player + 0xF4, s.tagPos); s.tagFrom = CurMoveName(player);   // +0xF4 = where the fighter really is
    s.fixUntil = (kind == TagKind::Combo || kind == TagKind::Raw || kind == TagKind::Neutral || kind == TagKind::Throw ||
                  kind == TagKind::Down) ? t + 2000 : 0;
    s.fixHeld = false;
    if (s.assault) { s.assaultT0 = t; s.attacked = false; s.entered = false; s.returnPending = false; s.neutralSince = 0; s.oppFreeSince = 0; s.landedAt = 0; s.idleSince = 0; }
    Log("tag: P%d %s -> %s (%s) hp %d%% -> %d%% hpSwap=%d rageSwap=%d move=%u", side + 1, out.name, in.name,
        kind == TagKind::Assault ? "assault" : kind == TagKind::Return ? "return" : kind == TagKind::Combo ? "tag combo" :
        kind == TagKind::Raw ? "tag after the move" : kind == TagKind::Throw ? "tag throw" : kind == TagKind::Crash ? "tag crash" :
        kind == TagKind::Down ? "tag from the floor" : "neutral",
        hp ? now.pct : -1, in.hp.pct, hp, s.rageOk && g_rageConfirmed, target);
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
int SideKey(int side) { return side == 0 ? g_cfg.tagKey : g_cfg.p2TagKey; }

// key edge for a side (keyboard only when the game window is really in front)
bool KeyEdge(int side) {
    Side& s = g_side[side];
    bool down = GameWindowInFront() && (KeyDown(SideKey(side)) || PadTagDown(side));
    bool edge = down && !s.keyWasDown;
    s.keyWasDown = down;
    return edge;
}

bool g_testTagPulse = false;      // debug (WallTest third item "tag"): P1 presses tag once at the screw

bool WantsTag(int side, Side& s, uintptr_t player, uintptr_t opp) {
    if (side == 0 && g_testTagPulse) { g_testTagPulse = false; Log("walltest: tag pressed (opp %s)", CurMoveName(opp).c_str()); return true; }
    if (g_tagInput) return g_tagInput(side);
    if (side == 0 || g_cfg.p2Tag == P2TagMode::Key) return KeyEdge(side);
    if (g_cfg.p2Tag != P2TagMode::Auto) return false;
    // CPU AI: assault on its own screws now and then; tag out when clearly more hurt than the partner.
    ULONGLONG now = NowMs();
    if (s.assault) return false;
    if (IsScrewed(opp)) return (now / 400) % 2 == 0;
    if (now < s.aiNextCheck) return false;
    s.aiNextCheck = now + 500;
    HpState h;
    if (!s.hpOk || !ReadHp(player, h)) return false;
    const Member& in = s.m[1 - s.active];
    return h.pct + 20 <= in.hp.pct && h.pct <= 60;
}

// Wall-screw switch in both copies: on while this side has not screwed the opponent in the current combo.
void WallTick(Side& s, uintptr_t opp) {
    ULONGLONG now = NowMs();
    bool combo = InCombo(opp);
    if (IsScrewed(opp)) s.screwUsed = true;
    if (combo) s.comboOverSince = 0;
    else if (!s.comboOverSince) s.comboOverSince = now;
    if (s.screwUsed && s.comboOverSince && now - s.comboOverSince > 150) s.screwUsed = false;
    uint32_t want = g_cfg.wallScrew && !s.screwUsed && combo ? 881 : 901;
    if (want == s.wallState) return;
    s.wallState = want;
    for (int k = 0; k < 2; ++k)
        if (s.alloc[k] && s.wallReq[k]) Wr(s.alloc[k] + s.wallReq[k], want);
}

// A new match on this side: decide solo/tag from the select-screen picks and install the movesets.
void SetupMatch(int side, uintptr_t player, uintptr_t cur) {
    Side& s = g_side[side];
    s.seen = true;
    s.tag = false;
    s.game = cur;
    s.active = 0;
    s.assault = false; s.pendingUntil = 0; s.screwUsed = false; s.wallState = 0;
    s.wallReq[0] = s.wallReq[1] = 0;
    s.prevScrew = false; s.screwSince = 0; s.traceLeft = 60; s.lastOpp.clear();
    int cid = MovesetCharId(cur);
    s.modelId = cid;
    ModelHudIconRestore(side, cid);
    bool armed = g_sel.armed[side];
    int pid = armed ? g_sel.partnerId[side] : -1;
    // rematch without the select screen: same team as last time
    static int lastFirst[2] = {-1, -1}, lastPartner[2] = {-1, -1};
    static bool lastBoss[2][2] = {};
    bool boss[2] = {g_sel.mainBoss[side] == cid, armed && g_sel.partnerBoss[side]};
    bool reuse = !armed && !g_sel.visited[side] && lastFirst[side] == cid && lastPartner[side] >= 0;
    if (reuse) { armed = true; pid = lastPartner[side]; boss[0] = lastBoss[side][0]; boss[1] = lastBoss[side][1]; }
    g_sel.visited[side] = false;
    lastFirst[side] = cid; lastPartner[side] = armed ? pid : -1;
    lastBoss[side][0] = boss[0]; lastBoss[side][1] = boss[1];
    HpState h;
    s.hpOk = ReadHp(player, h);
    s.lastPct = h.pct;
    HealTeam(s, h);
    s.rageOk = true;
    s.m[0].moveset = cur; s.m[0].assault = -1; s.m[0].ret = -1; s.m[0].entry = -1; s.m[0].entryRaw = -1; s.m[0].id = cid; s.m[1].id = -1;
    SetMemberName(s.m[0], cid, cur);
    bool tag = g_cfg.mode == Mode::Tag && armed && pid >= 0 && pid != cid;
    // own copy of the point character: assault/return moves and the wall-screw switch (solo sides too)
    // 11-26 boss version of the point character: its own blob even when playing solo
    int okey = cid + (boss[0] && g_files.count(cid + kBossKey) ? kBossKey : 0);
    Blob* ob = (tag || g_cfg.wallScrew || okey != cid) ? GetBlob(okey) : nullptr;
    if (ob && Install(*ob, cid, s.alloc[0])) {
        s.m[0].moveset = s.alloc[0]; s.m[0].assault = (int)ob->assault; s.m[0].ret = (int)ob->ret; s.m[0].entry = (int)ob->ret + 1; s.m[0].entryRaw = (int)ob->ret + 2;
        s.blobId[0] = okey; s.wallReq[0] = ob->wallReq;
        if (okey != cid) strncpy_s(s.m[0].name, ob->name, _TRUNCATE);
        SetMoveset(player, s.alloc[0]);
    }
    if (!tag) {
        ModelForget(side);
        if (okey != cid) ModelBossSolo(side, cid);
        Log("tag: P%d match setup: SOLO %s (id %d)%s hp=%d%% enc=%08X%s", side + 1, s.m[0].name, cid,
            armed ? " (tag armed but no partner picked)" : "", h.pct, h.enc,
            s.hpOk ? "" : " (health not readable)");
        return;
    }
    if (reuse) Log("tag: P%d rematch: same team as last time", side + 1);
    else if (g_sel.firstId[side] != cid) Log("tag: P%d warning: loaded id %d differs from first pick %d", side + 1, cid, g_sel.firstId[side]);
    int pkey = pid + (boss[1] && g_files.count(pid + kBossKey) ? kBossKey : 0);
    Blob* pb = GetBlob(pkey);
    if (!pb || !Install(*pb, cid, s.alloc[1])) return;
    s.m[1].moveset = s.alloc[1]; s.m[1].assault = (int)pb->assault; s.m[1].ret = (int)pb->ret; s.m[1].entry = (int)pb->ret + 1; s.m[1].entryRaw = (int)pb->ret + 2;
    s.blobId[1] = pkey; s.wallReq[1] = pb->wallReq; s.m[1].id = pid;
    SetMemberName(s.m[1], pid, s.alloc[1]);
    if (pkey != pid) strncpy_s(s.m[1].name, pb->name, _TRUNCATE);
    s.tag = true;
    uintptr_t oppMs = 0; Rd(GamePlayer(1 - side) + Off::Motbin, oppMs);
    ModelPrepare(side, cid, pid, oppMs ? MovesetCharId(oppMs) : -1);
    Log("tag: P%d match setup: TAG %s(id %d) + %s(id %d) hp=%d%% enc=%08X%s move=%s", side + 1, s.m[0].name, cid,
        s.m[1].name, pid, h.pct, h.enc, s.hpOk ? "" : " (health not readable: no health swap)", CurMoveName(player).c_str());
}

void HealthTick(int side, Side& s, uintptr_t player, double dt) {
    HpState h;
    if (!s.hpOk) return;
    if (!ReadHp(player, h)) { Log("tag: P%d health unreadable, health swap disabled", side + 1); s.hpOk = false; return; }
    Member& act = s.m[s.active];
    if (h.pct > s.lastPct) {
        if (h.pct == 100) { HealTeam(s, h); Log("tag: P%d health back to full (new round or refill), team healed", side + 1); }
        else act.history.push_back(h);
    } else if (h.pct < s.lastPct) {
        act.red = (std::min)((double)(100 - h.pct), act.red + (s.lastPct - h.pct) * g_cfg.redPercent / 100.0);
        if (act.history.empty() || act.history.back().enc != h.enc) act.history.push_back(h);
    }
    act.hp = h;
    s.lastPct = h.pct;
    // TTT2 red life: the waiting member slowly gets back up to its red share, by restoring states the game made
    Member& w = s.m[1 - s.active];
    if (w.red > 0 && !w.history.empty()) {
        double step = (std::min)(w.red, g_cfg.recoverPerSec * 100.0 / g_cfg.maxHealth * dt);
        w.red -= step; w.recovered += step;
        // highest earlier state within what was recovered; once the red is used up, the state nearest the target
        double target = w.hp.pct + w.recovered;
        const HpState* best = nullptr;
        for (const HpState& e : w.history)
            if (e.pct > w.hp.pct && e.pct <= target && (!best || e.pct > best->pct)) best = &e;
        if (!best && w.red <= 0)
            for (const HpState& e : w.history)
                if (e.pct > w.hp.pct && e.pct - target < target - w.hp.pct && (!best || e.pct < best->pct)) best = &e;
        if (best) { w.recovered -= best->pct - w.hp.pct; if (w.recovered < 0 || w.red <= 0) w.recovered = 0; w.hp = *best; }
        else if (w.red <= 0) w.recovered = 0;
    }
}

// The opponent really touched the floor (not just out of the spin): lying / ground-bounce / tech roll, or out of
// the combo. 11-22: "landed" used to be "not spinning any more" (still airborne in sDw_AIR00_) -> returns too early.
bool OppGrounded(uintptr_t opp) {
    std::string n = CurMoveName(opp);
    if (Starts(n, "aDw_") || Contains(n, "Png") || Starts(n, "dw") || Starts(n, "Co_AO") || Starts(n, "dDw_")) return true;
    return !InCombo(opp);
}

void AssaultTick(int side, Side& s, uintptr_t player, uintptr_t opp, uintptr_t cur, bool key) {
    ULONGLONG now = NowMs();
    std::string n = CurMoveName(player);
    bool entering = n == "T7Tag_Assault";
    bool neutral = IsNeutral(player, cur) || entering;                   // not attacking (may be running)
    bool free = neutral && !entering;                                     // the partner's entry and moves are over
    if (entering) s.entered = true;
    if (s.entered && !neutral) s.attacked = true;
    if (free) { if (!s.idleSince) s.idleSince = now; } else s.idleSince = 0;
    if (InCombo(opp)) s.oppFreeSince = 0; else if (!s.oppFreeSince) s.oppFreeSince = now;
    if (!s.landedAt && OppGrounded(opp)) {
        s.landedAt = now;
        Log("tag: P%d opponent touched the floor %llu ms after the assault (%s)", side + 1, now - s.assaultT0,
            CurMoveName(opp).c_str());
    }
    const ULONGLONG wait = (ULONGLONG)g_cfg.returnIdleMs;
    const char* why = nullptr;
    // 11-23 (user: "moves get swallowed"): the tag key during the partner's attack used to swap at once, so the attack
    // vanished before its hit (play log: returns at Dj_dsrp00n frame 3/112, Dj_co02 16/50). TTT2: control comes back
    // after the filler. The key is kept and the return fires once the attack's hit frames are over (or it is neutral).
    if (key && now >= s.cooldownUntil && !entering && !neutral && !AttackDone(player)) {
        if (!s.returnPending) Log("tag: P%d return waits for the partner's attack (%s)", side + 1, n.c_str());
        s.returnPending = true;
        key = false;
    }
    if (s.returnPending && !entering && (neutral || AttackDone(player))) { s.returnPending = false; why = "tag key, after the attack"; }
    else if (key && now >= s.cooldownUntil) why = "tag key";
    else if (key) Log("tag: P%d tag key ignored (cooldown %llu ms left)", side + 1, s.cooldownUntil - now);
    // AssaultReturn=Move (TTT2 as measured: official #4, Ramza, PhiDX): the partner gets one move (a string, a stance
    // and its follow-ups count as one); when it is over the main is back, the opponent usually still airborne.
    else if (g_cfg.assaultReturnMove && s.attacked && free) why = "the partner's move is over";
    // AssaultReturn=Landing (default, user 11-21/22: "until the opponent touches the floor"): the partner keeps
    // control until the floor, then ReturnIdleMs after the floor once the partner is not in a move
    else if (s.landedAt && free && now - (std::max)(s.idleSince, s.landedAt) >= wait) why = "the opponent is on the floor";
    else if (neutral && now - s.assaultT0 > 8000) why = "time";
    if (!why) return;
    s.returnPending = false;
    Log("tag: P%d assault over (%s)", side + 1, why);
    DoTag(side, player, TagKind::Return);
}

void ScrewTrace(int side, uintptr_t player, uintptr_t opp);

// 11-22 teleport check: positions (+0xE0 x, +0xE4 y, +0xE8 z, candidates from a live dump) of both players for
// 450 ms after a tag, every 50 ms
// 11-22 teleport fix (measured: tag combo after He_m_p00 moved P1 +742 units in one frame): forcing the new move
// ends the old one at frame 99999, which applies the rest of its root motion at once. When the new move has
// started, the jump is undone on every copy of the position (pos +0xE0, root +0xF4, the two transform translations
// +0x160 / +0x1A0; x at +0, z at +8; same set as TKMovesets' TagT8 moves for the incoming fighter).
// 11-23 (user: "after a tag the partner must stand exactly where the other one stood, not a bit further forward"):
// while the partner's entry move runs (T7Tag_Entry / T7Tag_EntryRaw, the throw / floor entries too) its ground position
// is held on the spot of the tag every frame. Before: one correction of jumps over 60 units only, so the entry's own
// small forward slide stayed. Height is left alone. The assault is not held (the partner chases the screwed opponent).
void TeleportFix(int side, Side& s, uintptr_t player) {
    if (!s.fixUntil) return;
    ULONGLONG now = NowMs();
    if (now > s.fixUntil) { s.fixUntil = 0; return; }
    std::string n = CurMoveName(player);
    if (n == s.tagFrom) return;                          // the forced move has not started yet
    if (n.rfind("T7Tag_Entry", 0) != 0) {                // entry over (or replaced by an attack): stop holding
        if (s.fixHeld) Log("pos: P%d held on the tag spot until %s", side + 1, n.c_str());
        s.fixUntil = 0; s.fixHeld = false;
        return;
    }
    // 11-23: +0xF4 is the real position; +0xE0 is where the current move started (measured frame by frame: during the
    // assault dash +0xE0 went back 1500 units while +0xF4 stayed). Comparing +0xE0 "corrected" the real position by up
    // to 1663 units (play log) - out through walls.
    float p[3]{};
    if (!Rd(player + 0xF4, p)) return;
    float dx = p[0] - s.tagPos[0], dz = p[2] - s.tagPos[2];
    if (dx * dx + dz * dz > 3000.0f * 3000.0f) { s.fixUntil = 0; return; }   // something else moved it (a new round)
    if (dx * dx + dz * dz < 0.25f) return;
    for (uintptr_t o : {0xE0, 0xF4, 0x160, 0x1A0}) {
        float v[3]{};
        if (!Rd(player + o, v)) continue;
        v[0] -= dx; v[2] -= dz;
        Wr(player + o, v[0]); Wr(player + o + 8, v[2]);
    }
    if (!s.fixHeld || dx * dx + dz * dz > 60.0f * 60.0f)
        Log("pos: P%d moved %.0f units off the tag spot (%s -> %s): put back", side + 1, sqrtf(dx * dx + dz * dz), s.tagFrom.c_str(), n.c_str());
    s.fixHeld = true;
}

void PosLog(int side, Side& s, uintptr_t player, uintptr_t opp) {
    ULONGLONG now = NowMs();
    if (!s.posUntil || now > s.posUntil) { s.posUntil = 0; return; }
    if (now < s.posNext) return;
    s.posNext = now + 50;
    float a[3]{}, b[3]{};
    Rd(player + 0xF4, a); Rd(opp + 0xF4, b);
    Log("pos: P%d %-18s %8.1f %8.1f %8.1f | opp %-18s %8.1f %8.1f %8.1f | dx %7.1f", side + 1, CurMoveName(player).c_str(),
        a[0], a[1], a[2], CurMoveName(opp).c_str(), b[0], b[1], b[2], b[0] - a[0]);
}

void SideTick(int side, double dt) {
    Side& s = g_side[side];
    uintptr_t player = GamePlayer(side), opp = GamePlayer(1 - side);
    if (!player || !opp) return;
    uintptr_t cur = 0;
    if (!Rd(player + Off::Motbin, cur) || !cur) return;
    uint8_t init = 0;
    if (!Rd(cur + Mi::Init, init) || init != 1) return;

    // 11-24: "back to stage select" loads the fighters again at the same moveset address; taking that for a new round kept
    // the old level's actor and parts (HUD showed the partner, the body stayed the main's; freed objects were used)
    bool reloaded = s.seen && s.loads != CostumeLoads(side) && cur == s.game;
    if (reloaded) Log("tag: P%d was loaded again (same moveset address): new match setup", side + 1);
    if (!s.seen || reloaded || (cur != s.game && cur != s.alloc[0] && cur != s.alloc[1])) {
        DollEnd();
        SetupMatch(side, player, cur);
        g_side[side].loads = CostumeLoads(side);
        return;
    }
    if (cur == s.game && s.m[0].moveset != s.game) {      // the game put its own moveset back (new round?)
        s.active = 0; s.assault = false;
        SetMoveset(player, s.m[0].moveset);
        if (s.tag) { DollEnd(); ModelShow(side, 0); ModelHudIcon(side, s.m[0].id); }
        Log("tag: P%d game restored its moveset, point copy re-applied", side + 1);
    }
    WallTick(s, opp);
    ScrewTrace(side, player, opp);
    TeleportFix(side, s, player);
    PosLog(side, s, player, opp);
    ULONGLONG now = NowMs();
    bool screwNow = IsScrewed(opp);
    if (screwNow && !s.prevScrew) s.screwSince = now;
    s.prevScrew = screwNow;
    // combo trace: what the opponent goes through (wall screw checks in the log)
    if (s.traceLeft > 0) {
        std::string on = CurMoveName(opp);
        if (on != s.lastOpp) {
            const std::string& o = s.lastOpp;
            bool was = !o.empty() && (Starts(o, "sDw_") || Starts(o, "aDw_") || Starts(o, "wDm_") || Contains(o, "AIR") ||
                                      Contains(o, "SPIN") || Contains(o, "ound"));
            if (InCombo(opp) || was) {
                --s.traceLeft;
                Log("trace: P%d combo: opp %s -> %s (by %s, wall switch %u)", side + 1, o.c_str(), on.c_str(),
                    CurMoveName(player).c_str(), s.wallState);
            }
            s.lastOpp = on;
        }
    }
    if (!s.tag) return;

    if (!g_rageConfirmed && s.hpOk) {
        uint32_t r = 0; Rd(player + Off::Rage, r);
        int32_t pct = 100; Rd(player + Off::HealthPct, pct);
        if (r > 1) s.rageOk = false;
        else if (r == 1 && pct <= 35) {
            g_rageConfirmed = true;
            Log("tag: rage flag confirmed at +0x99C (P%d health %d%%), rage swap enabled", side + 1, pct);
        }
    }
    HealthTick(side, s, player, dt);

    // TTT2 raw tag (Avoiding The Puddle "Raw Tag Safety", DevicJin): the incoming character cannot guard or move for a
    // long time, but a button move (attack, parry) can cut the entry earlier. P1 keyboard: from RawTagButtonFrames on,
    // an attack key ends the fixed pose; the game then reads the held button from standing.
    // 11-23: the same for every locked entry (user: "moves get cut / swallowed during the assault"): an attack key
    // pressed during the partner's entry ends the lock once the entry's minimum is reached, so the move comes out.
    //   assault entry: from frame 4 (TTT2: the filler follows the jump-in at once), tag combo: 12, raw tag: 45
    std::string en = CurMoveName(player);
    int cutFrom = -1, lockEnd = 0;
    if (en == "T7Tag_EntryRaw") { cutFrom = g_cfg.rawTagButtonFrames; lockEnd = g_cfg.rawTagFrames; }
    else if (en == "T7Tag_Entry") { cutFrom = 12; lockEnd = g_cfg.comboTagFrames; }
    else if (en == "T7Tag_Assault") { cutFrom = 4; lockEnd = 18; }
    if (side == 0 && cutFrom >= 0) {
        int32_t f = 0; Rd(player + Off::MoveTimer, f);
        int pressed = 0;
        if (GameWindowInFront()) for (int vk : {'U', 'I', 'J', 'K'}) if (KeyDown(vk)) { pressed = vk; break; }
        int padBtn = GameWindowInFront() ? PadAttack(0) : 0;
        if (!pressed && padBtn) { pressed = padBtn; s.entryPad = true; } else if (pressed && !s.entryCut) s.entryPad = false;
        if (pressed && !s.entryCut) { s.entryCut = true; s.entryBtnAt = f; s.entryKey = pressed; }   // held or tapped
        if (s.entryCut && !s.entryDone && f >= cutFrom && f < lockEnd) {
            Wr(player + Off::MoveTimer, (int32_t)lockEnd);
            s.entryDone = true;
            if (s.entryPad) PadSynthPress(0, s.entryKey); else InputSynthPress(s.entryKey);   // the press itself fell into the lock: press it again
            Log("tag: P%d %s cut by a button (pressed at frame %d, cut at %d, lock was %d)", side + 1, en.c_str(), s.entryBtnAt, f, lockEnd);
        }
    } else { s.entryCut = false; s.entryDone = false; }
    bool key = WantsTag(side, s, player, opp);
    if (s.assault) { AssaultTick(side, s, player, opp, cur, key); return; }

    // assault only right after the screw lands (TTT2: during the bound), not for the whole spin
    bool screw = screwNow && now - s.screwSince <= (ULONGLONG)g_cfg.assaultWindowMs;
    // a tag pressed during an attack waits until the attack is over (11-21: the attack must not be cut), then:
    // screw -> assault, opponent juggled -> tag combo, otherwise -> tag at the recovery
    // 11-21 v5: only a hit that juggled / screwed the opponent lets the partner in early (after the attack's active
    // frames, like TTT2's tag combo); any other tag waits for the whole recovery: no recovery cancel by tagging.
    bool done = AttackDone(player), neutral = IsNeutral(player, cur);
    bool juggled = InCombo(opp);
    // 11-21 (2): the tag combo too waits for the full recovery (TTT2: the tag is a move like any other, it does not
    // cancel recovery); only the assault comes in once the bound/screw hit is out, the outgoing member's recovery is
    // replaced by the partner's locked jump-in.
    // 11-21 (3), official TTT2 tutorials (#1, #4): "tag buffer launcher": tag pressed with a launcher -> on hit the
    // partner comes running in; "even if the move is on block you're not going to tag out" (no raw tag from a
    // buffered press); "all tags are punishable, there is no tag cancelling". So a tag pressed during a move fires
    // only when that move juggled/screwed the opponent, right after its active frames (the partner's locked 15-frame
    // run-in replaces the rest of the recovery); otherwise it is dropped. A raw tag needs neutral.
    if (s.pendingUntil) {
        if (done && (screw || juggled)) {
            s.pendingUntil = 0;
            if (screw) { Log("tag: P%d buffered tag: assault on the screw", side + 1); DoTag(side, player, TagKind::Assault); }
            else DoTag(side, player, TagKind::Combo);
            return;
        }
        if (OppThrown(opp)) { s.throwSeen = true; s.pendingUntil = (std::max)(s.pendingUntil, now + 1500); }   // throws are long
        if (s.throwSeen && neutral) { s.pendingUntil = 0; DoTag(side, player, TagKind::Throw); return; }
        if (neutral) { s.pendingUntil = 0; Log("tag: P%d buffered tag dropped: the move did not launch", side + 1); }
        else if (now > s.pendingUntil) { s.pendingUntil = 0; Log("tag: P%d buffered tag expired: %s", side + 1, CurMoveName(player).c_str()); }
    }
    if (key && now < s.cooldownUntil) Log("tag: P%d tag key ignored (cooldown %llu ms left)", side + 1, s.cooldownUntil - now);
    if (!key || now < s.cooldownUntil) return;
    if (OwnDown(player)) {                                // on the floor: tag crash (tag + right punch, partner rage) or get up and swap
        bool punch = side == 0 && GameWindowInFront() && (KeyDown(g_cfg.crashKey) || PadAttack(0) == 'I');
        const Member& in = s.m[1 - s.active];
        HpState h; bool low = s.hpOk && ReadHp(player, h) && h.pct <= g_cfg.crashHealthPct;
        bool rage = in.rage > 0 || low;
        if (punch && rage) DoTag(side, player, TagKind::Crash);
        else {
            if (punch) Log("tag: P%d tag crash needs the partner's rage (partner rage %u, health %d%% > %d%%): plain tag", side + 1,
                           in.rage, s.hpOk ? h.pct : -1, g_cfg.crashHealthPct);
            DoTag(side, player, TagKind::Down);
        }
        return;
    }
    if (neutral) DoTag(side, player, screw ? TagKind::Assault : juggled ? TagKind::Combo : TagKind::Neutral);
    else if (done && screw) DoTag(side, player, TagKind::Assault);
    else if (done && juggled) DoTag(side, player, TagKind::Combo);
    else {
        s.pendingUntil = now + 2000; s.throwSeen = OppThrown(opp);
        Log("tag: P%d tag buffered during %s (fires only if it launches or throws)", side + 1, CurMoveName(player).c_str());
    }
}

// the move can screw: its first hit-condition row asks for 615 ("screw allowed")
bool ScrewMove(uintptr_t mv) {
    uintptr_t hc = 0, req = 0; uint32_t id = 0;
    return mv && Rd(mv + 0x60, hc) && hc && Rd(hc, req) && req && Rd(req, id) && id == 615;
}

// every hit of a screw-capable move: what the opponent went into (wall screw checks), no limit
void ScrewTrace(int side, uintptr_t player, uintptr_t opp) {
    static std::string last[2];
    uintptr_t mv = 0; Rd(player + Off::CurMove, mv);
    std::string on = CurMoveName(opp);
    if (on != last[side] && ScrewMove(mv))
        Log("screwtrace: P%d %s hits: opp %s -> %s (wall switch %u)", side + 1, CurMoveName(player).c_str(),
            last[side].c_str(), on.c_str(), g_side[side].wallState);
    last[side] = on;
}

// 11-21 (3) wall screw without wall damage (user: "like Tekken 8's tornado"): when a spinning (screwed) player hits
// the wall, the health it loses in the first WallHitWindow ms of the wall reaction is given back. The value restored
// is the game's own raw health from the last tick of the spin (never computed); later wall reactions of the same
// chain too, while the attacker is not attacking. Runs for both sides before SideTick.
void WallDamageTick(int victim) {
    struct W { std::string prev; HpState hp; HpState saved; ULONGLONG at = 0; bool fixed = false; };
    static W w[2];
    W& x = w[victim];
    uintptr_t v = GamePlayer(victim);
    if (!v) return;
    std::string n = CurMoveName(v);
    HpState h;
    if (!ReadHp(v, h)) { x.prev = n; return; }
    ULONGLONG now = NowMs();
    bool spin = Starts(x.prev, "sDw_AIR_SPIN") || Starts(x.prev, "sDw_screw");
    if (g_cfg.wallScrew && spin && Starts(n, "wDm_")) { x.at = now; x.saved = x.hp; x.fixed = false; }
    if (x.at) {
        // the whole wall reaction chain (wDm_*, at most 1.5 s): the first 120 ms always (the impact), later only while
        // the attacker is not attacking (11-22: the wall damage came in two parts 35 ms apart; a second wall reaction
        // follows ~0.4 s later)
        std::string an = CurMoveName(GamePlayer(1 - victim));
        bool attackerIdle = an.size() >= 8 && an.compare(an.size() - 8, 8, "_sKAM00_") == 0;
        attackerIdle = attackerIdle || Starts(an, "T7Tag_") || Contains(an, "WALK") || Contains(an, "DASH") ||
                       Contains(an, "STEP") || Contains(an, "sRUN__");
        if (Starts(n, "wDm_") && now - x.at <= 1500) {
            if (h.enc != x.saved.enc && h.pct <= x.saved.pct && (now - x.at <= 120 || attackerIdle)) {
                Log("walldmg: P%d hit the wall out of the screw (%s, attacker %s): health %d%% -> %d%%, given back",
                    victim + 1, n.c_str(), an.c_str(), x.saved.pct, h.pct);
                WriteHp(v, x.saved);
                h = x.saved;
                x.fixed = true;
            } else if (h.enc != x.saved.enc) {
                x.saved = h;                       // the attacker's own hit: keep it, compare from here on
            }
        } else {
            if (!x.fixed) Log("walldmg: P%d wall hit out of the screw: no wall damage seen (%s)", victim + 1, n.c_str());
            x.at = 0;
        }
    }
    x.prev = n; x.hp = h;
}

// debug (ini [Debug] WallTest=move1,move2): F9 arms; move1 is forced on P1 when P2 is in a jump, move2 when P2 is
// against the wall (wDm_*). Makes the wall screw test repeatable without hand timing.
// Sound picker (11-22, user: "I want to listen and choose the counter sound myself"), in a fight:
//   F10 = next candidate, Shift+F10 = previous (played on P1, name shown on screen), F11 = use it as the assault sound
//   (saved to T7Tag.ini [Effects] AssaultSound). Candidates: [Debug] SoundTest=a,b,c or the built-in list below.
const char* kSoundPicks =
    "se_e3_set_counter,Hit01,FX_HIT_M01,FX_HIT_M02,FX_HIT_M03,FX_HIT_M04,FX_HIT_M05,FX_HIT_M06,FX_HIT_L01,FX_HIT_L02,"
    "FX_HIT_L03,FX_HIT_L04,FX_HIT_L05,SE_battle_rageattack_hit,SE_Finish_Shougekiha,SE_battle_spcombo_start,"
    "SE_battle_rageattack_start,SE_Change_Assist,GuardBreakHit_on,se_e3_mrx_counter,se_e3_hei_counter,criticak_re_11";

void SoundTestTick() {
    static bool wasNext = false, wasSave = false;
    static int idx = -1;
    static std::string lastList;
    std::string src = g_cfg.soundTest.empty() ? kSoundPicks : g_cfg.soundTest;
    if (src != lastList) { lastList = src; idx = -1; }
    std::vector<std::string> list;
    for (size_t i = 0; i <= src.size();) {
        size_t j = src.find(',', i); if (j == std::string::npos) j = src.size();
        if (j > i) list.push_back(src.substr(i, j - i));
        i = j + 1;
    }
    if (list.empty()) return;
    bool front = GameWindowInFront();
    bool next = front && KeyDown(VK_F10), save = front && KeyDown(VK_F11);
    if (next && !wasNext) {
        int n = (int)list.size();
        idx = KeyDown(VK_SHIFT) ? (idx <= 0 ? n - 1 : idx - 1) : (idx + 1) % n;
        const std::string& e = list[idx];
        Log("soundtest: %d/%d %s", idx + 1, n, e.c_str());
        BattleSound(0, e);
        wchar_t msg[200];
        swprintf_s(msg, L"소리 %d/%d: %S   (F10 다음, Shift+F10 이전, F11 = 어썰트 소리로 저장)", idx + 1, n, e.c_str());
        OverlayToast(msg, 4000);
    }
    if (save && !wasSave && idx >= 0 && idx < (int)list.size()) {
        g_cfg.assaultSound = list[idx];
        std::wstring ini = ModuleDir(g_selfModule) + L"T7Tag.ini";
        wchar_t w[128]; MultiByteToWideChar(CP_UTF8, 0, list[idx].c_str(), -1, w, 128);
        bool ok = WritePrivateProfileStringW(L"Effects", L"AssaultSound", w, ini.c_str()) != 0;
        Log("soundtest: assault sound = %s (%s)", list[idx].c_str(), ok ? "saved to the ini" : "ini write FAILED");
        wchar_t msg[200];
        swprintf_s(msg, L"어썰트 소리 = %S %s", list[idx].c_str(), ok ? L"(저장됨)" : L"(저장 실패)");
        OverlayToast(msg, 4000);
        BattleSound(0, list[idx]);
    }
    wasNext = next; wasSave = save;
}

void WallTestTick() {
    static int stage = 0;
    static bool f9was = false;
    static ULONGLONG t0 = 0;
    if (g_cfg.wallTest.empty()) return;
    bool f9 = GameWindowInFront() && KeyDown(VK_F9);
    ULONGLONG now = NowMs();
    if (f9 && !f9was) { stage = 1; t0 = now; Log("walltest: armed (%s)", g_cfg.wallTest.c_str()); }
    f9was = f9;
    if (!stage) return;
    if (now - t0 > 8000) { Log("walltest: gave up at stage %d", stage); stage = 0; return; }
    uintptr_t p = GamePlayer(0), o = GamePlayer(1), cur = 0;
    if (!p || !o || !Rd(p + Off::Motbin, cur)) return;
    size_t comma = g_cfg.wallTest.find(',');
    std::string m1 = g_cfg.wallTest.substr(0, comma), m2 = comma == std::string::npos ? "" : g_cfg.wallTest.substr(comma + 1);
    size_t c2 = m2.find(',');
    bool pressTag = c2 != std::string::npos && m2.substr(c2 + 1) == "tag";
    if (c2 != std::string::npos) m2 = m2.substr(0, c2);
    std::string on = CurMoveName(o);
    if (stage == 3) {                                       // tag on the screw (P1 buffers it during the screw move)
        if (IsScrewed(o)) { g_testTagPulse = true; stage = 0; }
        return;
    }
    int32_t ot = 0; Rd(o + Off::MoveTimer, ot);
    if (stage == 1 && (Starts(on, "sJUMP") && ot > 8 || Starts(m1, "now:"))) {
        if (Starts(m1, "now:")) m1 = m1.substr(4);           // "now:move": no jump needed (launcher on a standing dummy)
        int id = FindMoveByName(cur, m1.c_str());
        Log("walltest: opp %s frame %d -> force %s (id %d)", on.c_str(), ot, m1.c_str(), id);
        if (id >= 0) ForceMove(p, cur, (uint32_t)id);
        stage = 2;
    } else if (stage == 2 && !m2.empty() && (Starts(on, "wDm_") ||
               (Starts(m2, "air:") && Starts(on, "sDw_") && !IsScrewed(o) &&
                ot > (m2.find('@') != std::string::npos ? atoi(m2.c_str() + m2.find('@') + 1) : 12)))) {
        if (Starts(m2, "air:")) m2 = m2.substr(4);           // "air:move[@frame]": follow up in the air (no wall needed)
        if (m2.find('@') != std::string::npos) m2 = m2.substr(0, m2.find('@'));
        int id = FindMoveByName(cur, m2.c_str());
        Log("walltest: opp %s frame %d (wall switch %u) -> force %s (id %d)", on.c_str(), ot, g_side[0].wallState, m2.c_str(), id);
        if (id >= 0) ForceMove(p, cur, (uint32_t)id);
        stage = pressTag ? 3 : 0;
    }
}

// Is a fight running? (P1's move frame counter moved in the last 300 ms)
volatile bool g_inFight = false;            // for the controller poll (any thread)
bool Fighting() {
    if (g_fightingOverride) return g_fightingOverride();
    uintptr_t p = GamePlayer(0);
    int32_t t = 0;
    if (!p || !Rd(p + Off::MoveTimer, t)) return false;
    ULONGLONG now = GetTickCount64();
    if (t != g_lastTimer) { g_lastTimer = t; g_timerMoved = now; }
    return now - g_timerMoved < 300;
}

// Select screen (TTT2 flow, select.cpp): the tag key also switches TAG/SOLO while that choice is shown.
void SelectTick() {
    SelectTick2();
    for (int k = 0; k < 2; ++k) {
        bool edge = g_tagInput ? g_tagInput(k) : KeyEdge(k);
        if (!edge) continue;
        int side = PickSide();
        if (side < 0 || g_sel.phase[side] != PH_MODE || !g_sel.active) continue;
        g_sel.tagMode[side] = !g_sel.tagMode[side];
        Log("select: P%d %s", side + 1, g_sel.tagMode[side] ? "TAG" : "SOLO");
    }
}

void ReloadEffects() {
    ConfigLoad(g_selfModule);
    for (auto& sd : g_side) {
        sd.wallState = 0;                          // WallTick rewrites the switch with the new setting
        for (int k = 0; k < 2; ++k) {
            auto it = g_blobs.find(sd.blobId[k]);
            if (sd.alloc[k] && it != g_blobs.end() && it->second.ok) WriteEffects(sd.alloc[k], it->second);
        }
    }
    Log("tag: effects reloaded: %s", g_cfg.assaultEffects.c_str());
}
}  // namespace

void TagInit() {
    if (g_cfg.mode != Mode::Tag) return;
    std::wstring dir = PartnerDir();
    std::vector<uint8_t> raw;
    if (ReadFileAll(dir + L"common.bin", raw) && raw.size() > 16 && !memcmp(raw.data(), "T7TC", 4)) {
        if (Get<uint32_t>(raw, 4) == kVersion) g_common.assign(raw.begin() + 16, raw.end());
        else Log("tag: common.bin is from an older version -> run the launcher install");
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.t7p").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::vector<uint8_t> head;
            Blob b;
            // boss versions (hei_fb ...) sit next to the character's own file under id + kBossKey
            if (ReadFileAll(dir + fd.cFileName, head, 0x70) && ParseBlob(head, b, true))
                g_files[b.charId + (memchr(b.code, '_', sizeof(b.code)) ? kBossKey : 0)] = dir + fd.cFileName;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    Log("tag: %zu tag movesets indexed, common %zu bytes%s", g_files.size(), g_common.size(),
        g_files.empty() || g_common.empty() ? " -> run the launcher install" : "");
    SelectInstall();
}

void FightTick(double dt) {
    DollTick();                                   // first: a doll started below this frame gets its own first tick
    for (int s = 0; s < 2; ++s) WallDamageTick(s);
    for (int s = 0; s < 2; ++s) SideTick(s, dt);
    WallTestTick();
    SoundTestTick();
    SlotLabTick();
}

void TagTick() {
    if (g_cfg.mode != Mode::Tag) return;
    ULONGLONG now = GetTickCount64();
    double dt = g_lastTick ? (now - g_lastTick) / 1000.0 : 0;
    g_lastTick = now;
    if (GameIsOnline()) return;
    std::lock_guard<std::mutex> lk(g_mx);
    bool f7 = GameWindowInFront() && KeyDown(VK_F7);
    if (f7 && !g_f7WasDown) ReloadEffects();
    g_f7WasDown = f7;
    g_inFight = Fighting();
    if (!g_inFight) { SelectTick(); return; }
    SelectPause();                         // never hide keys in a fight; the next select screen is a new session
    if (ue::FrameHookReady()) return;      // 11-22 rebuild: the fight logic runs in TagFrame (game thread, per frame)
    FightTick(dt);
}

// 11-22 rebuild: called by the battle frame hook (engine.cpp) on the game thread after every battle update. The
// player structures, moves, health and positions are only written here, between two updates of the game itself.
bool TagInFight() { return g_inFight; }
bool TagSide(int side) { return side >= 0 && side < 2 && g_side[side].tag; }
void TagFrame() {
    if (g_cfg.mode != Mode::Tag || GameIsOnline()) return;
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    double dt = last ? (now - last) / 1000.0 : 0;
    last = now;
    std::lock_guard<std::mutex> lk(g_mx);
    ++g_frames;
    if (!Fighting()) { if (DollActive()) DollEnd(); return; }
    FightTick(dt);
    RecorderFrame(g_side[0].active, g_side[0].assault, DollActive());
}

void TagOverlaySnapshot(OverlaySide out[2]) {
    std::lock_guard<std::mutex> lk(g_mx);
    bool fighting = g_fightingOverride ? g_fightingOverride() : (GetTickCount64() - g_timerMoved < 300);
    for (int s = 0; s < 2; ++s) {
        const Side& sd = g_side[s];
        out[s] = OverlaySide{};
        if (!fighting) {
            int hover = SelectHover(s);
            out[s].select = g_sel.open && g_sel.phase[s] != PH_NONE;
            out[s].armed = g_sel.armed[s];
            out[s].tagMode = g_sel.tagMode[s];
            out[s].picking = PickSide() == s;
            out[s].phase = g_sel.phase[s];
            SelectCell(s, out[s].curCol, out[s].curRow);
            out[s].mainCol = g_sel.mainCol[s]; out[s].mainRow = g_sel.mainRow[s];
            const char* f = g_sel.firstId[s] >= 0 ? CharName(g_sel.firstId[s]) : nullptr;
            const char* hv = CharName(hover);
            if (f) strncpy_s(out[s].firstName, f, _TRUNCATE);
            if (hv) strncpy_s(out[s].hoverName, hv, _TRUNCATE);
            const char* pn = g_sel.partnerId[s] >= 0 ? CharName(g_sel.partnerId[s]) : nullptr;
            if (pn) strncpy_s(out[s].partnerName, pn, _TRUNCATE);
            continue;
        }
        out[s].fighting = sd.seen;
        out[s].modelId = sd.modelId;
        int32_t pct = -1;
        uintptr_t pl = GamePlayer(s);
        out[s].curPct = (pl && Rd(pl + Off::HealthPct, pct) && pct >= 0 && pct <= 100) ? pct : -1;
        out[s].show = sd.tag;
        if (!sd.tag) continue;
        out[s].activeId = sd.m[sd.active].id;
        out[s].hudIcon = ModelHudIconShows(s, out[s].activeId);
        const Member& a = sd.m[sd.active];
        const Member& w = sd.m[1 - sd.active];
        strncpy_s(out[s].activeName, a.name, _TRUNCATE);
        strncpy_s(out[s].waitingName, w.name, _TRUNCATE);
        out[s].activePct = a.hp.pct; out[s].activeRed = (int)(a.red + 0.5);
        out[s].waitingPct = w.hp.pct; out[s].waitingRed = (int)(w.red + 0.5);
        out[s].assault = sd.assault;
    }
}
