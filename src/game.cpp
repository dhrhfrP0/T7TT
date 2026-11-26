// Addresses for the latest Steam build of TEKKEN 7 (TekkenGame-Win64-Shipping.exe).
// Source: TKMovesets2 game_addresses.ini (t7 section), verified against the installed exe.
#include "common.h"

namespace {
constexpr uintptr_t kP1Ptr        = 0x34DF630;  // [base + this] = player 1 structure
constexpr uintptr_t kSteamLobbyId = 0x34CEEF8;  // non-zero while in an online lobby
constexpr uintptr_t kPlayerSize   = 0x3670;     // player 2 = player 1 + this
}

uintptr_t GameBase() {
    static uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    return base;
}

bool GameIsOnline() {
    uint64_t lobby = 0;
    if (!Rd(GameBase() + kSteamLobbyId, lobby)) return true;  // unknown -> be safe
    return lobby != 0;
}

uintptr_t GamePlayer(int side) {
    uintptr_t p1 = 0;
    if (!Rd(GameBase() + kP1Ptr, p1) || !p1) return 0;
    return p1 + side * kPlayerSize;
}
