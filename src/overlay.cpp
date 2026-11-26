// Transparent click-through window over the game (borderless/windowed modes).
//  fight : TTT2 HUD: the partner bar (a copy of the game's own bar) above the real one, partner portrait (DrawFight).
//  select: TTT2 TAG / SOLO icons per side, MAIN / PARTNER slots (DrawSelect).
#include "common.h"
#include <mutex>

namespace {
constexpr COLORREF kKey = RGB(255, 0, 255);   // colour-keyed = fully transparent
HWND g_ov = nullptr;

void Fill(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }

std::wstring W(const char* s) {
    wchar_t w[64]{};
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 63);   // names are UTF-8 (Korean table) or ASCII
    return w;
}

void Text(HDC dc, const std::wstring& t, RECT r, UINT align, COLORREF c) {
    SetBkMode(dc, TRANSPARENT);
    // 1px dark shadow keeps the text readable on bright stages
    RECT s = r; OffsetRect(&s, 1, 1);
    SetTextColor(dc, RGB(10, 10, 10));
    DrawTextW(dc, t.c_str(), -1, &s, align | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, c);
    DrawTextW(dc, t.c_str(), -1, &r, align | DT_SINGLELINE | DT_NOPREFIX);
}

// ---------------------------------------------------------------- fight HUD (TTT2 layout, 11-21)
// TTT2 shows two copies of the same bar per side: the character in control on the normal bar, the waiting partner on
// a thinner copy right above it, and the portrait of the character in control at the outer end.
//  - the game's own bar keeps showing the character in control (its health is swapped on every tag);
//  - the partner's bar is the game's real bar image (grabbed from the screen) drawn above it at BackH of its height
//    with the same slant, emptied to the partner's health;
//  - the portrait: when the partner is in control, its portrait (cached from earlier fights where that character
//    was on screen) is drawn over the game's one.
// The grab is only accepted when the picture really is the HUD: both bars full, P2's bar is the mirror image of
// P1's, gold/teal colours, and unchanged over two grabs (11-21: a grab during the intro had copied the stage).
struct Quad { POINT p[4]; };   // top-outer, top-inner, bottom-inner, bottom-outer

// the game's bar in P1 space (x from the left), as doubles
struct BarGeo { double xo, xi, y0, h, sl; };
BarGeo Geo(const RECT& rc) {
    return BarGeo{rc.right * g_cfg.hudX0, rc.right * g_cfg.hudX1, rc.bottom * g_cfg.hudY, rc.bottom * g_cfg.hudH,
                  rc.right * g_cfg.hudSlant};
}
POINT Pt(const RECT& rc, int side, double x, double y) {
    if (side == 1) x = rc.right - x;
    return POINT{(LONG)(x + 0.5), (LONG)(y + 0.5)};
}
Quad BarQuad(const RECT& rc, int side) {
    BarGeo g = Geo(rc);
    return Quad{{Pt(rc, side, g.xo, g.y0), Pt(rc, side, g.xi, g.y0), Pt(rc, side, g.xi + g.sl, g.y0 + g.h),
                 Pt(rc, side, g.xo + g.sl, g.y0 + g.h)}};
}
// the bar with its copper frame (P1 space): top-left, top-right, bottom-right, bottom-left (the two ends are not
// parallel: measured 11-21 at 2560x1440)
Quad FrameQuad(const RECT& rc, int side) {
    const double* f = g_cfg.hudFrame;
    Quad q;
    for (int i = 0; i < 4; ++i) q.p[i] = Pt(rc, side, rc.right * f[2 * i], rc.bottom * f[2 * i + 1]);
    return q;
}
// the partner's copy: the same framed bar scaled to BackH height (same slant), right above, shifted outward
struct Affine { double k, dx, Y, y0, slope; };
Affine BackMap(const RECT& rc) {
    BarGeo g = Geo(rc);
    double k = g_cfg.hudBackH;
    double top = rc.bottom * g_cfg.hudFrame[1], bot = rc.bottom * g_cfg.hudFrame[5];
    double gap = (std::max)(1.0, rc.bottom * 0.0012);
    double Y = (std::max)(0.0, top - k * (bot - top) - gap);
    return Affine{k, -rc.right * g_cfg.hudBackShift, Y, top, g.sl / g.h};
}
// map a point of the real bar (P1 space) onto the copy
void MapP1(const Affine& a, double x, double y, double& ox, double& oy) {
    double v = y - a.y0;
    ox = x + a.dx - v * a.slope * (1 - a.k);
    oy = a.Y + v * a.k;
}
POINT MapScreen(const RECT& rc, int side, const Affine& a, double x, double y) {
    if (side == 1) x = rc.right - x;
    double ox, oy; MapP1(a, x, y, ox, oy);
    return Pt(rc, side, ox, oy);
}
Quad BackQuad(const RECT& rc, int side) {
    BarGeo g = Geo(rc); Affine a = BackMap(rc);
    auto M = [&](double x, double y) { double ox, oy; MapP1(a, x, y, ox, oy); return Pt(rc, side, ox, oy); };
    return Quad{{M(g.xo, g.y0), M(g.xi, g.y0), M(g.xi + g.sl, g.y0 + g.h), M(g.xo + g.sl, g.y0 + g.h)}};
}
Quad BackFrame(const RECT& rc, int side) {
    Affine a = BackMap(rc);
    const double* f = g_cfg.hudFrame;
    Quad q;
    for (int i = 0; i < 4; ++i) {
        double ox, oy; MapP1(a, rc.right * f[2 * i], rc.bottom * f[2 * i + 1], ox, oy);
        q.p[i] = Pt(rc, side, ox, oy);
    }
    return q;
}

POINT Lerp(POINT a, POINT b, double t) { return POINT{(LONG)(a.x + (b.x - a.x) * t), (LONG)(a.y + (b.y - a.y) * t)}; }

// part of a quad: t0..t1 along its length (0 = outer end), v0..v1 across its height (0 = top)
Quad Part(const Quad& q, double t0, double t1, double v0, double v1) {
    POINT a0 = Lerp(q.p[0], q.p[3], v0), a1 = Lerp(q.p[1], q.p[2], v0);
    POINT b0 = Lerp(q.p[0], q.p[3], v1), b1 = Lerp(q.p[1], q.p[2], v1);
    return Quad{{Lerp(a0, a1, t0), Lerp(a0, a1, t1), Lerp(b0, b1, t1), Lerp(b0, b1, t0)}};
}

void FillQuad(HDC dc, const Quad& q, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN pn = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pn);
    Polygon(dc, q.p, 4);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(pn);
}

RECT Bounds(const Quad& q) {
    RECT r{q.p[0].x, q.p[0].y, q.p[0].x, q.p[0].y};
    for (auto& p : q.p) {
        r.left = (std::min)(r.left, p.x); r.right = (std::max)(r.right, p.x);
        r.top = (std::min)(r.top, p.y); r.bottom = (std::max)(r.bottom, p.y);
    }
    InflateRect(&r, 2, 2);
    return r;
}

// ---- screen grabs
struct Shot { HBITMAP bmp = nullptr; uint32_t* px = nullptr; RECT at{}; int w = 0, h = 0; };
void Free(Shot& s) { if (s.bmp) DeleteObject(s.bmp); s = Shot{}; }

bool GrabRect(HWND game, RECT b, Shot& out) {
    out = Shot{};
    POINT tl{b.left, b.top};
    ClientToScreen(game, &tl);
    int w = b.right - b.left, h = b.bottom - b.top;
    if (w <= 4 || h <= 4) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(mdc); ReleaseDC(nullptr, sdc); return false; }
    HGDIOBJ o = SelectObject(mdc, bmp);
    BOOL ok = BitBlt(mdc, 0, 0, w, h, sdc, tl.x, tl.y, SRCCOPY | CAPTUREBLT);
    SelectObject(mdc, o); DeleteDC(mdc); ReleaseDC(nullptr, sdc);
    if (!ok) { DeleteObject(bmp); return false; }
    out.bmp = bmp; out.px = (uint32_t*)bits; out.at = b; out.w = w; out.h = h;
    // never the colour key (magenta) when drawn back on the overlay
    for (int i = 0; i < w * h; ++i) if ((out.px[i] & 0xFFFFFF) == 0xFF00FF) out.px[i] = 0xFE00FE;
    return true;
}

uint32_t PixelAt(const Shot& s, POINT p) {
    int x = p.x - s.at.left, y = p.y - s.at.top;
    if (x < 0 || y < 0 || x >= s.w || y >= s.h) return 0;
    return s.px[y * s.w + x];
}
int Diff(uint32_t a, uint32_t b) {
    return abs((int)((a >> 16) & 255) - (int)((b >> 16) & 255)) + abs((int)((a >> 8) & 255) - (int)((b >> 8) & 255)) +
           abs((int)(a & 255) - (int)(b & 255));
}

struct Grabs {
    bool fromCache = false;      // bar[] came from the disk copy of an earlier grab (still re-checked on screen)
    Shot bar[2];                 // accepted bars
    bool ok = false;
    Shot cand[2];                // last candidate pair (stability check)
    ULONGLONG candAt = 0;
    int tries = 0;
    ULONGLONG nextTry = 0;
} g_gr;
ULONGLONG g_fightSince = 0;
int g_lastW = 0, g_lastH = 0;

// samples inside the bar (P1 space parameters), the same spot on both sides: 24 positions along it, 3 heights
// (j = 0 top band, 1 middle, 2 lower part)
template <class F> void ForSamples(const RECT& rc, F f) {
    Quad q0 = BarQuad(rc, 0), q1 = BarQuad(rc, 1);
    for (int i = 0; i < 24; ++i)
        for (int j = 0; j < 3; ++j) {
            double t = 0.04 + 0.92 * i / 23.0, v = 0.3 + 0.2 * j;
            f(i, j, Lerp(Lerp(q0.p[0], q0.p[3], v), Lerp(q0.p[1], q0.p[2], v), t),
              Lerp(Lerp(q1.p[0], q1.p[3], v), Lerp(q1.p[1], q1.p[2], v), t));
        }
}
int Bright(uint32_t c) { return ((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255); }

// Is this the full T7 bar? Checked on 720p frames (11-21): the full bar is gold (teal at the inner end) with a
// bright top band over a dark middle band, and P2's bar is P1's mirror image. Stage pictures fail all three.
bool SaveBmp(const std::wstring& path, const Shot& s);
std::wstring BarFile(const RECT& rc, int side);

void TryGrab(HWND game, const RECT& rc) {
    Shot s[2];
    for (int k = 0; k < 2; ++k)
        if (!GrabRect(game, Bounds(FrameQuad(rc, k)), s[k])) { Free(s[0]); Free(s[1]); return; }
    int n = 0, colour = 0, band = 0, mirror = 0, stable = 0;
    int top[24]{};
    bool haveCand = g_gr.cand[0].bmp && GetTickCount64() - g_gr.candAt < 800;
    ForSamples(rc, [&](int i, int j, POINT a, POINT b) {
        uint32_t c = PixelAt(s[0], a), d = PixelAt(s[1], b);
        int r = (c >> 16) & 255, g = (c >> 8) & 255, bl = c & 255;
        ++n;
        bool gold = r > 60 && g > 50 && bl + 30 < r && abs(r - g) < 70;       // older HUD
        bool teal = g > 45 && bl > 40 && r + 30 < g;                           // current HUD: teal ...
        bool orange = r > 140 && r > bl + 80 && r > g;                         // ... to orange
        if (gold || teal || orange) ++colour;
        if (j == 0) top[i] = Bright(c);
        if (j == 1 && top[i] > Bright(c) + 120) ++band;
        mirror += Diff(c, d);
        if (haveCand) stable += Diff(c, PixelAt(g_gr.cand[0], a));
    });
    mirror /= n; if (haveCand) stable /= n;
    bool good = colour * 10 >= n * 8 && band * 10 >= 24 * 7 && mirror < 45 && haveCand && stable < 30;
    ++g_gr.tries;
    if (good) {
        for (int k = 0; k < 2; ++k) { Free(g_gr.bar[k]); g_gr.bar[k] = s[k]; SaveBmp(BarFile(rc, k), s[k]); }
        g_gr.ok = true; g_gr.fromCache = false;
        Log("overlay: health bars copied (%dx%d): colour %d/%d band %d/24 mirror diff %d change %d, try %d",
            s[0].w, s[0].h, colour, n, band, mirror, stable, g_gr.tries);
    } else {
        if (g_gr.tries <= 3 || g_gr.tries % 10 == 0)
            Log("overlay: bar grab is not the full HUD yet (colour %d/%d band %d/24 mirror diff %d change %d%s), try %d",
                colour, n, band, mirror, stable, haveCand ? "" : " -", g_gr.tries);
        for (int k = 0; k < 2; ++k) { Free(g_gr.cand[k]); g_gr.cand[k] = s[k]; }
        g_gr.candAt = GetTickCount64();
    }
}

// ---- portraits: cached per character and side, beside the dll (T7Tag_portraits\<id>_<side>.bmp)
std::wstring PortDir() { return ModuleDir(g_selfModule) + L"T7Tag_portraits\\"; }
std::wstring PortFile(int id, int side) { wchar_t b[32]; swprintf_s(b, L"%d_%d.bmp", id, side); return PortDir() + b; }
RECT PortRect(const RECT& rc, int side) {
    int x = (int)(rc.right * g_cfg.hudPortX), y = (int)(rc.bottom * g_cfg.hudPortY);
    int w = (int)(rc.right * g_cfg.hudPortW), h = (int)(rc.bottom * g_cfg.hudPortH);
    return side == 0 ? RECT{x, y, x + w, y + h} : RECT{rc.right - x - w, y, rc.right - x, y + h};
}
struct Port { HBITMAP bmp = nullptr; int w = 0, h = 0; bool tried = false; };
Port g_port[64][2];

bool SaveBmp(const std::wstring& path, const Shot& s) {
    CreateDirectoryW(PortDir().c_str(), nullptr);
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih); ih.biWidth = s.w; ih.biHeight = s.h; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    DWORD body = (DWORD)(s.w * s.h * 4);
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + body;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr;
    WriteFile(f, &fh, sizeof(fh), &wr, nullptr); WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
    for (int y = s.h - 1; y >= 0; --y) WriteFile(f, s.px + y * s.w, s.w * 4, &wr, nullptr);   // bottom-up
    CloseHandle(f);
    return true;
}

// the bar picture is the same every match: kept per resolution so the copy is right from the first frame
std::wstring BarFile(const RECT& rc, int side) {
    wchar_t b[64]; swprintf_s(b, L"bar_%dx%d_%d.bmp", (int)rc.right, (int)rc.bottom, side);
    return PortDir() + b;
}
bool LoadBars(const RECT& rc) {
    Shot t[2];
    for (int k = 0; k < 2; ++k) {
        HBITMAP h = (HBITMAP)LoadImageW(nullptr, BarFile(rc, k).c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
        if (!h) { Free(t[0]); return false; }
        BITMAP bm; GetObject(h, sizeof(bm), &bm);
        RECT at = Bounds(FrameQuad(rc, k));
        if (bm.bmWidth != at.right - at.left || bm.bmHeight != at.bottom - at.top) { DeleteObject(h); Free(t[0]); return false; }
        t[k].bmp = h; t[k].px = nullptr; t[k].at = at; t[k].w = bm.bmWidth; t[k].h = bm.bmHeight;
    }
    for (int k = 0; k < 2; ++k) { Free(g_gr.bar[k]); g_gr.bar[k] = t[k]; }
    g_gr.fromCache = true;
    Log("overlay: health bar copy loaded from the last grab (%dx%d)", t[0].w, t[0].h);
    return true;
}

Port* GetPort(int id, int side) {
    if (id < 0 || id >= 64) return nullptr;
    Port& p = g_port[id][side];
    if (!p.bmp && !p.tried) {
        p.tried = true;
        p.bmp = (HBITMAP)LoadImageW(nullptr, PortFile(id, side).c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
        if (p.bmp) { BITMAP bm; GetObject(p.bmp, sizeof(bm), &bm); p.w = bm.bmWidth; p.h = bm.bmHeight; }
    }
    return p.bmp ? &p : nullptr;
}

// right after an accepted bar grab the HUD is surely on screen: keep the portraits of the characters shown
void CachePortraits(HWND game, const RECT& rc, const OverlaySide* sides) {
    for (int k = 0; k < 2; ++k) {
        int id = sides[k].modelId;
        if (id < 0 || id >= 64 || GetFileAttributesW(PortFile(id, k).c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        // only the game's own portrait: never while we draw the partner's over it (11-21: a cached file had our tile)
        if (sides[k].show && sides[k].activeId >= 0 && sides[k].activeId != sides[k].modelId) continue;
        Shot s;
        if (!GrabRect(game, PortRect(rc, k), s)) continue;
        if (SaveBmp(PortFile(id, k), s)) {
            Log("overlay: portrait of character %d (P%d side) saved", id, k + 1);
            g_port[id][k] = Port{};
        }
        Free(s);
    }
}

void DrawPortrait(HDC dc, const RECT& rc, int side, const OverlaySide& o) {
    RECT r = PortRect(rc, side);
    int w = r.right - r.left, h = r.bottom - r.top;
    Port* p = GetPort(o.activeId, side);
    bool mirror = false;
    if (!p) { p = GetPort(o.activeId, 1 - side); mirror = p != nullptr; }
    if (p) {
        HDC m = CreateCompatibleDC(dc);
        HGDIOBJ ob = SelectObject(m, p->bmp);
        SetStretchBltMode(dc, HALFTONE);
        if (mirror) StretchBlt(dc, r.right - 1, r.top, -w, h, m, 0, 0, p->w, p->h, SRCCOPY);
        else StretchBlt(dc, r.left, r.top, w, h, m, 0, 0, p->w, p->h, SRCCOPY);
        SelectObject(m, ob); DeleteDC(m);
        return;
    }
    // not seen yet: a plain tile with the name (the real portrait is saved the first time the character fights)
    RECT t = r; InflateRect(&t, -w / 10, -h / 10);
    HBRUSH b = CreateSolidBrush(RGB(24, 26, 32)); FillRect(dc, &t, b); DeleteObject(b);
    Text(dc, W(o.activeName), t, DT_CENTER | DT_VCENTER, RGB(240, 240, 240));
}

// drawn imitation of the T7 bar when no grab is available
void DrawImitation(HDC dc, const Quad& q) {
    FillQuad(dc, q, RGB(14, 60, 74));
    const COLORREF band[6] = {RGB(40, 170, 170), RGB(50, 190, 180), RGB(80, 170, 150), RGB(200, 130, 50), RGB(220, 100, 20), RGB(190, 60, 10)};
    for (int i = 0; i < 6; ++i) FillQuad(dc, Part(q, i / 6.0, (i + 1) / 6.0, 0.08, 0.45), band[i]);
    for (int i = 0; i < 6; ++i) FillQuad(dc, Part(q, i / 6.0, (i + 1) / 6.0, 0.62, 0.92), band[i]);
}

void DrawFight(HDC dc, const RECT& rc, int side, const OverlaySide& o) {
    Quad back = BackQuad(rc, side), frame = BackFrame(rc, side);
    HRGN rg = CreatePolygonRgn(frame.p, 4, WINDING);
    SelectClipRgn(dc, rg);
    const Shot& g = g_gr.bar[side];
    if ((g_gr.ok || g_gr.fromCache) && g.bmp) {
        Affine a = BackMap(rc);
        // the grabbed rectangle's upper-left, upper-right, lower-left corners on the copy
        POINT dst[3];
        auto M = [&](LONG x, LONG y) { return MapScreen(rc, side, a, (double)x, (double)y); };
        if (side == 0) { dst[0] = M(g.at.left, g.at.top); dst[1] = M(g.at.right, g.at.top); dst[2] = M(g.at.left, g.at.bottom); }
        else {
            // MapScreen works in P1 space: mirror x of the corners first (P2's grab is a real P2 rectangle)
            auto MS = [&](LONG x, LONG y) { double ox, oy; MapP1(a, rc.right - (double)x, (double)y, ox, oy); return Pt(rc, 1, ox, oy); };
            dst[0] = MS(g.at.left, g.at.top); dst[1] = MS(g.at.right, g.at.top); dst[2] = MS(g.at.left, g.at.bottom);
        }
        HDC m = CreateCompatibleDC(dc);
        HGDIOBJ ob = SelectObject(m, g.bmp);
        SetStretchBltMode(dc, HALFTONE);
        PlgBlt(dc, dst, m, 0, 0, g.w, g.h, nullptr, 0, 0);
        SelectObject(m, ob); DeleteDC(m);
    } else {
        FillQuad(dc, frame, RGB(120, 62, 28));            // copper frame like the game's
        DrawImitation(dc, back);
    }
    // the partner's lost health, from the outer end: empty, then the recoverable (red) part next to the health
    double lost = (100 - o.waitingPct) / 100.0, red = (std::min)(lost, o.waitingRed / 100.0);
    if (lost - red > 0.001) FillQuad(dc, Part(back, 0, lost - red, 0.1, 0.9), RGB(18, 22, 28));
    if (red > 0.001) FillQuad(dc, Part(back, lost - red, lost, 0.1, 0.9), RGB(196, 30, 30));
    SelectClipRgn(dc, nullptr);
    DeleteObject(rg);
    if (o.assault) {                                      // partner in on an assault: white edge on its bar
        HPEN pn = CreatePen(PS_SOLID, 2, RGB(250, 250, 250));
        HGDIOBJ op = SelectObject(dc, pn), obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Polygon(dc, frame.p, 4);
        SelectObject(dc, op); SelectObject(dc, obr); DeleteObject(pn);
    }
    if (o.activeId >= 0 && o.activeId != o.modelId && !o.hudIcon) DrawPortrait(dc, rc, side, o);
}

// ---------------------------------------------------------------- select screen (TTT2: TAG / SOLO icons first)
// one stick figure: head + body, in a box
void Figure(HDC dc, int cx, int top, int h, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN pn = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pn);
    int hr = h / 7;
    Ellipse(dc, cx - hr, top, cx + hr, top + 2 * hr);
    RoundRect(dc, cx - hr * 3 / 2, top + 2 * hr + h / 20, cx + hr * 3 / 2, top + h * 7 / 10, hr, hr);   // body
    Rectangle(dc, cx - hr, top + h * 6 / 10, cx - hr / 4, top + h);                                      // legs
    Rectangle(dc, cx + hr / 4, top + h * 6 / 10, cx + hr, top + h);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(pn);
}

void ModeIcon(HDC dc, RECT r, bool two, bool on) {
    HBRUSH b = CreateSolidBrush(on ? RGB(70, 150, 30) : RGB(30, 30, 34));
    FillRect(dc, &r, b); DeleteObject(b);
    HBRUSH e = CreateSolidBrush(on ? RGB(190, 255, 90) : RGB(90, 90, 96));
    FrameRect(dc, &r, e); DeleteObject(e);
    int w = r.right - r.left, h = r.bottom - r.top, top = r.top + h / 8, fh = h * 3 / 4;
    COLORREF c = on ? RGB(225, 255, 160) : RGB(200, 200, 205);
    if (two) { Figure(dc, r.left + w * 34 / 100, top, fh, c); Figure(dc, r.left + w * 66 / 100, top, fh, c); }
    else Figure(dc, r.left + w / 2, top, fh, c);
}

// a cell of the game's character grid (measured 11-21 on 720p captures: 19 columns 61 px apart from x 61, rows
// 83.5 px apart, row 1 = top at y 403)
RECT CellRect(const RECT& rc, int col, int row) {
    double x = rc.right * (0.04297 + col * 0.04766), y = rc.bottom * (0.5597 + (row - 1) * 0.1160);   // row 1 = top
    return RECT{(LONG)x, (LONG)y, (LONG)(x + rc.right * 0.04375), (LONG)(y + rc.bottom * 0.1139)};
}

// "MAIN" / "PARTNER" plate on the top edge of a cell, with a coloured frame
void CellLabel(HDC dc, const RECT& rc, int col, int row, const wchar_t* text, COLORREF c) {
    if (col < 0 || row < 1 || row > 3) return;
    RECT cell = CellRect(rc, col, row);
    HBRUSH fr = CreateSolidBrush(c);
    for (int i = 0; i < 3; ++i) { RECT r = cell; InflateRect(&r, 2 + i, 2 + i); FrameRect(dc, &r, fr); }
    int ph = (cell.bottom - cell.top) / 5;
    RECT plate{cell.left - 6, cell.top - ph - 4, cell.right + 6, cell.top - 2};
    HBRUSH bg = CreateSolidBrush(RGB(12, 12, 16));
    FillRect(dc, &plate, bg); FrameRect(dc, &plate, fr);
    DeleteObject(bg); DeleteObject(fr);
    Text(dc, text, plate, DT_CENTER | DT_VCENTER, c);
}

// TTT2 select (11-21): first the TAG / SOLO icons (A/Left = TAG, D/Right = SOLO, Enter = OK), then the slots.
void DrawSelect(HDC dc, const RECT& rc, int side, const OverlaySide& o) {
    const int W_ = rc.right, H = rc.bottom;
    bool mode = o.phase == PH_MODE;
    int ic = mode && o.picking ? H * 11 / 100 : H * 6 / 100;      // big while choosing, small afterwards
    int gap = ic / 6;
    int y0 = H * 25 / 100;                                       // below the game's player card
    int x0 = side == 0 ? W_ * 2 / 100 : W_ * 98 / 100 - (2 * ic + gap);
    RECT tagR{x0, y0, x0 + ic, y0 + ic}, soloR{x0 + ic + gap, y0, x0 + 2 * ic + gap, y0 + ic};
    ModeIcon(dc, tagR, true, o.tagMode);
    ModeIcon(dc, soloR, false, !o.tagMode);
    int lh = H / 30;
    UINT al = side == 0 ? DT_LEFT : DT_RIGHT;
    RECT lab{x0, y0 + ic + 2, soloR.right, y0 + ic + 2 + lh};
    Text(dc, o.tagMode ? L"TAG" : L"SOLO", lab, DT_CENTER, o.tagMode ? RGB(200, 255, 120) : RGB(230, 230, 230));
    RECT box = side == 0 ? RECT{W_ * 2 / 100, lab.bottom + 6, W_ * 45 / 100, lab.bottom + 6 + 4 * (lh + 6)}
                         : RECT{W_ * 55 / 100, lab.bottom + 6, W_ * 98 / 100, lab.bottom + 6 + 4 * (lh + 6)};
    RECT r1 = box; r1.bottom = r1.top + lh + 4;
    RECT r2 = r1; OffsetRect(&r2, 0, lh + 6);
    RECT r3 = r2; OffsetRect(&r3, 0, lh + 6);
    std::wstring hint;
    wchar_t l1[96] = L"", l2[96] = L"";
    switch (o.phase) {
    case PH_MODE:
        hint = o.picking ? L"A / D (← →) = TAG / SOLO     Enter = 결정" : L"";
        break;
    case PH_MAIN:
        swprintf_s(l1, L"P%d  MAIN      ▶ 고르는 중", side + 1);
        swprintf_s(l2, L"P%d  PARTNER  -", side + 1);
        hint = L"Enter = MAIN 결정     Esc = TAG/SOLO 다시";
        break;
    case PH_PARTNER:
        swprintf_s(l1, L"P%d  MAIN      %s", side + 1, W(o.firstName).c_str());
        swprintf_s(l2, L"P%d  PARTNER  ▶ 고르는 중", side + 1);
        hint = L"Enter = PARTNER 결정     Esc = MAIN 다시";
        break;
    case PH_SOLO:
        swprintf_s(l1, L"P%d  SOLO", side + 1);
        hint = L"Enter = 결정     Esc = TAG/SOLO 다시";
        break;
    case PH_DONE:
        if (o.armed) {
            swprintf_s(l1, L"P%d  MAIN      %s", side + 1, W(o.firstName).c_str());
            swprintf_s(l2, L"P%d  PARTNER  %s", side + 1, o.partnerName[0] ? W(o.partnerName).c_str() : L"-");
        } else {
            swprintf_s(l1, L"P%d  SOLO", side + 1);
        }
        break;
    }
    if (l1[0]) Text(dc, l1, r1, al, o.phase == PH_MAIN ? RGB(255, 225, 90) : RGB(245, 245, 245));
    if (l2[0]) Text(dc, l2, r2, al, o.phase == PH_PARTNER ? RGB(120, 220, 255) : RGB(200, 200, 200));
    if (!hint.empty() && o.picking) Text(dc, hint, r3, al, RGB(215, 215, 215));
    // labels on the grid: the main's cell, and what the cursor is picking now
    HFONT lf = CreateFontW(-(H / 50), 0, 0, 0, FW_HEAVY, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Arial");
    HGDIOBJ of = SelectObject(dc, lf);
    const COLORREF mainC = RGB(255, 205, 60), partC = RGB(90, 210, 255);
    if (o.phase == PH_PARTNER || (o.phase == PH_DONE && o.armed)) CellLabel(dc, rc, o.mainCol, o.mainRow, L"MAIN", mainC);
    if (o.picking && o.phase == PH_MAIN) CellLabel(dc, rc, o.curCol, o.curRow, L"MAIN", mainC);
    if (o.picking && o.phase == PH_PARTNER && !(o.curCol == o.mainCol && o.curRow == o.mainRow))
        CellLabel(dc, rc, o.curCol, o.curRow, L"PARTNER", partC);
    SelectObject(dc, of); DeleteObject(lf);
}

LRESULT CALLBACK OvProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

std::mutex g_toastMx;
std::wstring g_toast;
ULONGLONG g_toastUntil = 0;

void Paint() {
    HWND game = GameWindow();
    OverlaySide sides[2];
    TagOverlaySnapshot(sides);
    bool fight = sides[0].fighting || sides[1].fighting;
    std::wstring toast;
    { std::lock_guard<std::mutex> lk(g_toastMx); if (GetTickCount64() < g_toastUntil) toast = g_toast; }
    bool any = sides[0].show || sides[1].show || sides[0].select || sides[1].select || !toast.empty();
    RECT cr{}; if (game) GetClientRect(game, &cr);
    // the bar grab: a new fight or a resized window starts over; tries while both bars read full
    ULONGLONG now = GetTickCount64();
    if (!fight) g_fightSince = 0; else if (!g_fightSince) g_fightSince = now;
    static bool cacheTried = false;
    if (!fight || cr.right != g_lastW || cr.bottom != g_lastH) {
        if (g_gr.ok || g_gr.tries) { g_gr.ok = false; g_gr.tries = 0; }
        if (cr.right != g_lastW || cr.bottom != g_lastH) { g_gr.fromCache = false; cacheTried = false; }
        g_lastW = cr.right; g_lastH = cr.bottom;
    }
    if (fight && !g_gr.ok && !g_gr.fromCache && !cacheTried && cr.right > 0) { cacheTried = true; LoadBars(cr); }
    bool wantGrab = fight && (sides[0].show || sides[1].show || g_cfg.overlay);
    if (game && GameWindowInFront() && !IsIconic(game) && wantGrab && !g_gr.ok && g_gr.tries < 200 &&
        now - g_fightSince > 1000 && now >= g_gr.nextTry && sides[0].curPct == 100 && sides[1].curPct == 100) {
        g_gr.nextTry = now + 300;
        // our overlay is excluded from capture; the grab sees the game only
        TryGrab(game, cr);
        if (g_gr.ok) CachePortraits(game, cr, sides);
    }
    if (!game || !GameWindowInFront() || !any || IsIconic(game)) { ShowWindow(g_ov, SW_HIDE); return; }
    POINT tl{0, 0}; ClientToScreen(game, &tl);
    SetWindowPos(g_ov, HWND_TOPMOST, tl.x, tl.y, cr.right, cr.bottom, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    HDC wdc = GetDC(g_ov);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, cr.right, cr.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    Fill(dc, cr, kKey);
    HFONT f = CreateFontW(-(cr.bottom / 54), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Malgun Gothic");
    HFONT fs = CreateFontW(-(cr.bottom / 38), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Malgun Gothic");
    HGDIOBJ oldF = SelectObject(dc, f);
    for (int s = 0; s < 2; ++s) {
        if (sides[s].show) { SelectObject(dc, f); DrawFight(dc, cr, s, sides[s]); }
        else if (sides[s].select) { SelectObject(dc, fs); DrawSelect(dc, cr, s, sides[s]); }
    }
    if (!toast.empty()) {                               // sound picker etc.: a line near the top centre
        SelectObject(dc, fs);
        RECT t{cr.right / 8, cr.bottom * 16 / 100, cr.right * 7 / 8, cr.bottom * 22 / 100};
        Fill(dc, t, RGB(16, 16, 24));
        Text(dc, toast, t, DT_CENTER | DT_VCENTER, RGB(255, 230, 120));
    }
    BitBlt(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldF); SelectObject(dc, oldBmp);
    DeleteObject(bmp); DeleteObject(f); DeleteObject(fs);
    DeleteDC(dc);
    ReleaseDC(g_ov, wdc);
}

DWORD WINAPI OverlayThread(LPVOID) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = OvProc;
    wc.hInstance = g_selfModule;
    wc.lpszClassName = L"T7TagOverlay";
    RegisterClassW(&wc);
    g_ov = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_selfModule, nullptr);
    if (!g_ov) { Log("overlay: window creation failed (%lu)", GetLastError()); return 0; }
    SetLayeredWindowAttributes(g_ov, kKey, 0, LWA_COLORKEY);
    // keep the overlay out of screen grabs (our own bar copy, and recordings) - Windows 10 2004+
    if (!g_cfg.overlayInCapture && !SetWindowDisplayAffinity(g_ov, 0x11 /*WDA_EXCLUDEFROMCAPTURE*/))
        Log("overlay: capture exclusion unavailable (%lu)", GetLastError());
    Log("overlay: ready");
    SetTimer(g_ov, 1, 50, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER) { Paint(); continue; }
        DispatchMessageW(&msg);
    }
    return 0;
}
}  // namespace

void OverlayStart() {
    if (g_cfg.mode != Mode::Tag || !g_cfg.overlay) return;
    HANDLE t = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void OverlayToast(const wchar_t* text, int ms) {
    std::lock_guard<std::mutex> lk(g_toastMx);
    g_toast = text ? text : L"";
    g_toastUntil = GetTickCount64() + (ULONGLONG)ms;
}
