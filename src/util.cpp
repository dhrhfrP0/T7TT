#include "common.h"
#include <cstdio>
#include <cstdarg>
#include <mutex>

Config g_cfg;
static FILE* g_log = nullptr;
static std::mutex g_logMx;

std::wstring ModuleDir(HMODULE self) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring s(path);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

void LogInit(HMODULE self) {
    std::wstring p = ModuleDir(self) + L"T7Tag.log";
    // keep the previous session (it may hold the user's own play) next to the new one
    MoveFileExW(p.c_str(), (ModuleDir(self) + L"T7Tag.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_log = _wfsopen(p.c_str(), L"w", _SH_DENYWR);
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    std::lock_guard<std::mutex> lk(g_logMx);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// "Space", "O", "F", "VK_xx"/"0x20" -> virtual key
static int KeyFromName(const wchar_t* s, int def) {
    if (!s[0]) return def;
    if (!_wcsicmp(s, L"Space")) return VK_SPACE;
    if (!_wcsicmp(s, L"Shift")) return VK_SHIFT;
    if (!_wcsicmp(s, L"Ctrl")) return VK_CONTROL;
    if (!_wcsicmp(s, L"Tab")) return VK_TAB;
    if (!_wcsicmp(s, L"Enter")) return VK_RETURN;
    if (!s[1]) return towupper(s[0]);
    if (!_wcsnicmp(s, L"0x", 2)) return (int)wcstol(s, nullptr, 16);
    return def;
}

static int ReadInt(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(sec, key, def, ini.c_str());
}

void ConfigLoad(HMODULE self) {
    std::wstring ini = ModuleDir(self) + L"T7Tag.ini";
    wchar_t buf[64]{};
    GetPrivateProfileStringW(L"General", L"Mode", L"Solo", buf, 64, ini.c_str());
    g_cfg.mode = (_wcsicmp(buf, L"Tag") == 0) ? Mode::Tag : Mode::Solo;
    g_cfg.focusFix = ReadInt(ini, L"Fixes", L"FocusFix", 1) != 0;
    g_cfg.noRageArts = ReadInt(ini, L"Fixes", L"NoRageArts", 1) != 0;
    GetPrivateProfileStringW(L"Tag", L"TagKey", L"Space", buf, 64, ini.c_str());
    g_cfg.tagKey = KeyFromName(buf, VK_SPACE);
    GetPrivateProfileStringW(L"Tag", L"PadTag", L"LB", buf, 64, ini.c_str());
    { char pn[64]; WideCharToMultiByte(CP_UTF8, 0, buf, -1, pn, 64, nullptr, nullptr); PadConfig(pn); }
    g_cfg.padTest = ReadInt(ini, L"Debug", L"PadTest", 0) != 0;
    GetPrivateProfileStringW(L"Tag", L"P2TagKey", L"P", buf, 64, ini.c_str());
    g_cfg.p2TagKey = KeyFromName(buf, 'P');
    GetPrivateProfileStringW(L"Tag", L"P2Tag", L"Auto", buf, 64, ini.c_str());
    g_cfg.p2Tag = !_wcsicmp(buf, L"Key") ? P2TagMode::Key : !_wcsicmp(buf, L"Off") ? P2TagMode::Off : P2TagMode::Auto;
    {
        wchar_t eff[512]{};
        GetPrivateProfileStringW(L"Effects", L"Assault", L"", eff, 512, ini.c_str());
        char a[512]{};
        WideCharToMultiByte(CP_ACP, 0, eff, -1, a, sizeof(a) - 1, nullptr, nullptr);
        g_cfg.assaultEffects = a;
        GetPrivateProfileStringW(L"Effects", L"Return", L"", eff, 512, ini.c_str());
        a[0] = 0;
        WideCharToMultiByte(CP_ACP, 0, eff, -1, a, sizeof(a) - 1, nullptr, nullptr);
        g_cfg.returnEffects = a;
    }
    g_cfg.assaultBufferMs = ReadInt(ini, L"Tag", L"AssaultBufferMs", 700);
    g_cfg.returnIdleMs = ReadInt(ini, L"Tag", L"ReturnIdleMs", 700);
    g_cfg.wallScrew = ReadInt(ini, L"Tag", L"WallScrew", 0) != 0;   // 11-22 user: off by default ("a bit odd")
    g_cfg.assaultWindowMs = ReadInt(ini, L"Tag", L"AssaultWindowMs", 500);
    {
        wchar_t v[512]{}; char a[512]{};
        GetPrivateProfileStringW(L"Tag", L"AssaultReturn", L"Landing", v, 64, ini.c_str());
        g_cfg.assaultReturnMove = _wcsicmp(v, L"Move") == 0;
        g_cfg.modelEquip = GetPrivateProfileIntW(L"Tag", L"ModelEquip", 1, ini.c_str()) != 0;
        GetPrivateProfileStringW(L"Tag", L"PartnerCostume", L"Custom", v, 64, ini.c_str());
        g_cfg.partnerCustom = _wcsicmp(v, L"Default") != 0;
        g_cfg.partnerCostumeSlot = GetPrivateProfileIntW(L"Tag", L"PartnerCostumeSlot", 0, ini.c_str());   // partner colours (SetAllCustomizeInfo)
        GetPrivateProfileStringW(L"Effects", L"AssaultSound", L"", v, 512, ini.c_str());
        WideCharToMultiByte(CP_UTF8, 0, v, -1, a, 512, nullptr, nullptr); g_cfg.assaultSound = a;
        GetPrivateProfileStringW(L"Effects", L"ReturnSound", L"", v, 512, ini.c_str());
        WideCharToMultiByte(CP_UTF8, 0, v, -1, a, 512, nullptr, nullptr); g_cfg.returnSound = a;
        GetPrivateProfileStringW(L"Debug", L"SoundTest", L"", v, 512, ini.c_str());
        WideCharToMultiByte(CP_UTF8, 0, v, -1, a, 512, nullptr, nullptr); g_cfg.soundTest = a;
        GetPrivateProfileStringW(L"Debug", L"SlotOps", L"", v, 512, ini.c_str());
        WideCharToMultiByte(CP_UTF8, 0, v, -1, a, 512, nullptr, nullptr); g_cfg.slotOps = a;
        GetPrivateProfileStringW(L"Debug", L"SlotWatch", L"", v, 512, ini.c_str());
        WideCharToMultiByte(CP_UTF8, 0, v, -1, a, 512, nullptr, nullptr); g_cfg.slotWatch = a;
        GetPrivateProfileStringW(L"Debug", L"SlotBp", L"", v, 512, ini.c_str());
        WideCharToMultiByte(CP_UTF8, 0, v, -1, a, 512, nullptr, nullptr); g_cfg.slotBp = a;
    }
    g_cfg.comboTagFrames = (std::max)(1, (std::min)(300, ReadInt(ini, L"Tag", L"ComboTagFrames", 20)));
    g_cfg.rawTagFrames = (std::max)(1, (std::min)(300, ReadInt(ini, L"Tag", L"RawTagFrames", 60)));
    g_cfg.rawTagButtonFrames = (std::max)(1, (std::min)(300, ReadInt(ini, L"Tag", L"RawTagButtonFrames", 45)));
    g_cfg.crashHealthPct = (std::max)(0, (std::min)(100, ReadInt(ini, L"Tag", L"CrashHealthPct", 40)));
    g_cfg.doll = ReadInt(ini, L"Tag", L"Doll", 1) != 0;
    g_cfg.dollMinFrames = (std::max)(0, (std::min)(300, ReadInt(ini, L"Tag", L"DollMinFrames", 10)));
    g_cfg.dollMaxFrames = (std::max)(g_cfg.dollMinFrames, (std::min)(600, ReadInt(ini, L"Tag", L"DollMaxFrames", 90)));
    auto frac = [&](const wchar_t* key, double def) {
        wchar_t v[32]{}; GetPrivateProfileStringW(L"HUD", key, L"", v, 32, ini.c_str());
        return v[0] ? _wtof(v) : def;
    };
    g_cfg.hudY = frac(L"Y", 0.0840); g_cfg.hudH = frac(L"H", 0.0319);
    g_cfg.hudX0 = frac(L"X0", 0.1273); g_cfg.hudX1 = frac(L"X1", 0.4281); g_cfg.hudSlant = frac(L"Slant", 0.0148);
    {
        wchar_t v[128]{}; GetPrivateProfileStringW(L"HUD", L"Frame", L"", v, 128, ini.c_str());
        double f[8]; int n = swscanf_s(v, L"%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7]);
        if (n == 8) for (int i = 0; i < 8; ++i) g_cfg.hudFrame[i] = f[i];
    }
    g_cfg.hudBackH = frac(L"BackH", 0.6); g_cfg.hudBackShift = frac(L"BackShift", 0.006);
    g_cfg.hudPortX = frac(L"PortraitX", 0.064); g_cfg.hudPortY = frac(L"PortraitY", 0.031);
    g_cfg.hudPortW = frac(L"PortraitW", 0.053); g_cfg.hudPortH = frac(L"PortraitH", 0.132);
    GetPrivateProfileStringW(L"Tag", L"TagIn", L"Jump", buf, 64, ini.c_str());
    g_cfg.tagInJump = _wcsicmp(buf, L"None") != 0;
    g_cfg.maxHealth = ReadInt(ini, L"Tag", L"MaxHealth", 175);
    g_cfg.redPercent = ReadInt(ini, L"Tag", L"RedPercent", 40);
    g_cfg.recoverPerSec = ReadInt(ini, L"Tag", L"RecoverPerSec10", 40) / 10.0;
    g_cfg.overlay = ReadInt(ini, L"Tag", L"Overlay", 1) != 0;
    g_cfg.modelSwap = ReadInt(ini, L"Tag", L"ModelSwap", 1) != 0;
    g_cfg.overlayInCapture = ReadInt(ini, L"HUD", L"ShowInCapture", 0) != 0;
    {
        wchar_t v[128]{}; GetPrivateProfileStringW(L"Debug", L"WallTest", L"", v, 128, ini.c_str());
        char a[128]{}; WideCharToMultiByte(CP_ACP, 0, v, -1, a, 127, nullptr, nullptr);
        g_cfg.wallTest = a;
    }
    Log("config: mode=%s focusFix=%d noRageArts=%d tagKey=0x%02X p2Tag=%d p2Key=0x%02X tagIn=%s "
        "model=%d maxHealth=%d red=%d%% recover=%.1f/s overlay=%d wallScrew=%d buffer=%dms window=%dms assault=[%s]",
        g_cfg.mode == Mode::Tag ? "Tag" : "Solo", g_cfg.focusFix, g_cfg.noRageArts, g_cfg.tagKey,
        (int)g_cfg.p2Tag, g_cfg.p2TagKey, g_cfg.tagInJump ? "Jump" : "None", g_cfg.modelSwap, g_cfg.maxHealth,
        g_cfg.redPercent, g_cfg.recoverPerSec, g_cfg.overlay, g_cfg.wallScrew, g_cfg.assaultBufferMs, g_cfg.assaultWindowMs,
        g_cfg.assaultEffects.c_str());
}

std::string ReadCStr(uintptr_t addr, size_t max) {
    char buf[128]{};
    if (max > sizeof(buf) - 1) max = sizeof(buf) - 1;
    if (!addr || !SafeRead(addr, buf, max)) return {};
    buf[max] = 0;
    return buf;
}

// SEH-guarded copies: game memory can be freed between loading screens.
bool SafeRead(uintptr_t addr, void* out, size_t size) {
    if (addr < 0x10000) return false;
    __try { memcpy(out, (const void*)addr, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeWrite(uintptr_t addr, const void* in, size_t size) {
    if (addr < 0x10000) return false;
    __try { memcpy((void*)addr, in, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
