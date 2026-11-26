// Offline test: loads a relocated moveset image at its fixed base, fakes a player that points at it, runs the
// real NoRageTick() (src/norage.cpp), then writes the image back for tools/check_norage.py.
// usage: norage_harness <in.img> <out.img> <base-hex>
#include "../src/common.h"
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

Config g_cfg;
static uintptr_t g_player = 0;
void Log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }
uintptr_t GameBase() { return 0; }
bool GameIsOnline() { return false; }
uintptr_t GamePlayer(int side) { return side == 0 ? g_player : 0; }
bool SafeRead(uintptr_t a, void* o, size_t n) { __try { memcpy(o, (void*)a, n); return true; } __except (1) { return false; } }
bool SafeWrite(uintptr_t a, const void* i, size_t n) { __try { memcpy((void*)a, i, n); return true; } __except (1) { return false; } }

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    uintptr_t base = _strtoui64(argv[3], nullptr, 16);
    FILE* f = fopen(argv[1], "rb"); if (!f) return 3;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    void* mem = VirtualAlloc((void*)base, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem != (void*)base) { printf("alloc failed\n"); return 4; }
    fread(mem, 1, size, f); fclose(f);
    std::vector<uint8_t> player(0x3670, 0);
    for (int k = 0; k < 5; ++k) *(uintptr_t*)(player.data() + 0x1520 + k * 8) = base;
    g_player = (uintptr_t)player.data();

    NoRageTick();
    NoRageTick();   // second tick must patch nothing new
    FILE* o = fopen(argv[2], "wb"); fwrite(mem, 1, size, o); fclose(o);
    // test restore path too: write a second image after restore
    NoRageRestoreAll();
    std::string r = std::string(argv[2]) + ".restored";
    o = fopen(r.c_str(), "wb"); fwrite(mem, 1, size, o); fclose(o);
    return 0;
}
