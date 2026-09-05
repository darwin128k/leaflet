#include "roundframe.h"
#include "log.h"
#include <string.h>

typedef void(__thiscall *GetSizeFn)(void *self, int *outWide, int *outTall);
typedef void(__thiscall *PaintFn)(void *self);
typedef void *(__cdecl *GetSurfaceFn)(void);
typedef void(__thiscall *SurfDrawSetColorFn)(void *surf, unsigned int packedRgba);
typedef void(__thiscall *SurfDrawFilledRectFn)(void *surf, int x0, int y0, int x1, int y1);

#define RVA_GETSIZE               0x00043780u
#define RVA_GETSURFACE            0x0003f040u
#define RVA_PANEL_PAINTBACKGROUND 0x00043d60u
#define RVA_FRAME_PAINTBACKGROUND 0x0004cb60u /* Frame/PropertyDialog/MessageBox/COptionsDialog */
#define RVA_CAREER_PAINTBACKGROUND 0x00002070u
#define RVA_PAINTBACKGROUND_18530 0x00018530u
#define RVA_PAINTBORDER           0x00043d40u
#define RVA_FRAME_PAINTBG_ALT     0x00023970u

#define OFF_PANEL_NAME   0x44
#define OFF_PANEL_BORDER 0x2C /* IBorder* loaded by Panel::PaintBorder */
#define SURF_VT_DRAWSETCOLOR       0x1C
#define SURF_VT_DRAWFILLEDRECT     0x24

static BYTE *g_gameUiBase = NULL;
static GetSizeFn g_GetSize = NULL;
static GetSurfaceFn g_GetSurface = NULL;

static PaintFn g_origPanelPaintBg = NULL;
static PaintFn g_origFramePaintBg = NULL;
static PaintFn g_origCareerPaintBg = NULL;
static PaintFn g_origPaint18530 = NULL;
static PaintFn g_origPaintBorder = NULL;
static PaintFn g_origFramePaintBgAlt = NULL;
static BYTE g_panelPaintBgTramp[32];
static BYTE g_framePaintBgTramp[32];
static BYTE g_careerPaintBgTramp[32];
static BYTE g_paint18530Tramp[32];
static BYTE g_paintBorderTramp[32];
static BYTE g_framePaintBgAltTramp[32];

static SurfDrawSetColorFn g_origDrawSetColor = NULL;
static SurfDrawFilledRectFn g_origDrawFilledRect = NULL;
static int g_surfaceHooked = 0;

static volatile LONG g_roundDisabled = 0;
static volatile LONG g_inOurDraw = 0;
static int g_roundActive = 0;
static int g_roundW = 0;
static int g_roundH = 0;
static int g_roundR = 0;
static unsigned int g_curColor = 0xE0101410u;

/* Captured from the main-body fill inside DrawFilledRect_Hook so
 * RunRoundedBackground can trace a stroke around the exact same rounded
 * rect afterwards -- children haven't painted yet at that point, so a
 * ring drawn there sits under their text/icons instead of over them. */
static int g_edgeCaptured = 0;
static int g_edgeX = 0;
static int g_edgeY = 0;
static int g_edgeRoundTop = 0;
static int g_edgeRoundBottom = 0;
static unsigned int g_edgeBodyColor = 0;

#define BORDER_STROKE_THICKNESS 2
#define BORDER_STROKE_LIGHTEN   55

static int ISqrt(int n)
{
    int x;
    int y;
    if (n <= 0) {
        return 0;
    }
    x = n;
    for (;;) {
        y = (x + n / x) / 2;
        if (y >= x) {
            return x;
        }
        x = y;
    }
}

static int ClampInt(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* Valve Color is [r,g,b,a] little-endian bytes, i.e. r is the low byte.
 * Lighten each channel for a subtle stroke tint and force full alpha so
 * the ring reads crisply regardless of the body panel's own alpha. */
static unsigned int ShadeLighter(unsigned int packedRgba, int delta)
{
    unsigned int r = packedRgba & 0xFFu;
    unsigned int g = (packedRgba >> 8) & 0xFFu;
    unsigned int b = (packedRgba >> 16) & 0xFFu;
    r = (unsigned int)ClampInt((int)r + delta, 0, 255);
    g = (unsigned int)ClampInt((int)g + delta, 0, 255);
    b = (unsigned int)ClampInt((int)b + delta, 0, 255);
    return (0xFFu << 24) | (b << 16) | (g << 8) | r;
}

/* Bigger windows get a bigger radius; tiny query boxes stay tight. */
static int RadiusForSize(int w, int h)
{
    int m = (w < h) ? w : h;
    return ClampInt(m / 12, 8, 24);
}

static int CornerInset(int y, int h, int r)
{
    int dy;
    int inside;
    if (r <= 0 || h < r * 2) {
        return 0;
    }
    if (y < r) {
        dy = r - 1 - y;
    } else if (y >= h - r) {
        dy = y - (h - r);
    } else {
        return 0;
    }
    inside = r * r - dy * dy;
    if (inside < 0) {
        return r;
    }
    return r - 1 - ISqrt(inside);
}

static const char *PanelName(void *panel)
{
    const char *name;
    if (panel == NULL) {
        return "";
    }
    name = *(const char **)((char *)panel + OFF_PANEL_NAME);
    return name != NULL ? name : "";
}

static int NameContainsI(const char *hay, const char *needle)
{
    int i;
    int j;
    if (hay == NULL || needle == NULL || needle[0] == '\0') {
        return 0;
    }
    for (i = 0; hay[i] != '\0'; i++) {
        for (j = 0;; j++) {
            char a;
            char b;
            if (needle[j] == '\0') {
                return 1;
            }
            if (hay[i + j] == '\0') {
                return 0;
            }
            a = hay[i + j];
            b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
        }
    }
    return 0;
}

static int NameIsMenuChrome(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (lstrcmpiA(name, "GameMenu") == 0 || lstrcmpiA(name, "GameMenuButton") == 0) {
        return 1;
    }
    if (NameContainsI(name, "BackgroundMenu") || NameContainsI(name, "GameMenu")) {
        return 1;
    }
    return 0;
}

static int IsBannerPanel(int w, int h)
{
    /* Logo strip / leftover 64px CBasePanel row — wide and short. */
    return (h <= 72 && w >= h * 3);
}

static void GameClientSize(int *outW, int *outH)
{
    HWND hwnd;
    RECT rc;
    *outW = GetSystemMetrics(SM_CXSCREEN);
    *outH = GetSystemMetrics(SM_CYSCREEN);
    hwnd = FindWindowA("Valve001", NULL);
    if (hwnd != NULL && GetClientRect(hwnd, &rc) && rc.right > 64 && rc.bottom > 64) {
        *outW = rc.right;
        *outH = rc.bottom;
    }
}

static int ShouldRoundPanel(void *thisPtr)
{
    int w = 0, h = 0;
    int gameW = 0, gameH = 0;
    const char *name;
    if (g_GetSize == NULL || thisPtr == NULL) {
        return 0;
    }
    g_GetSize(thisPtr, &w, &h);
    /* Any real popup; skip buttons, tabs, 1px hairlines. */
    if (w < 96 || h < 56) {
        return 0;
    }
    name = PanelName(thisPtr);
    if (NameIsMenuChrome(name) || IsBannerPanel(w, h)) {
        return 0;
    }
    GameClientSize(&gameW, &gameH);
    if (gameW > 0 && gameH > 0 && w >= gameW - 8 && h >= gameH - 8) {
        return 0;
    }
    return 1;
}

static void SurfaceFill(int x0, int y0, int x1, int y1, unsigned int packedRgba)
{
    void *surf;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    if (g_GetSurface == NULL) {
        return;
    }
    surf = g_GetSurface();
    if (surf == NULL) {
        return;
    }
    InterlockedIncrement(&g_inOurDraw);
    if (g_origDrawSetColor != NULL && g_origDrawFilledRect != NULL) {
        g_origDrawSetColor(surf, packedRgba);
        g_origDrawFilledRect(surf, x0, y0, x1, y1);
    } else {
        void **vt = *(void ***)surf;
        SurfDrawSetColorFn setColor = (SurfDrawSetColorFn)vt[SURF_VT_DRAWSETCOLOR / sizeof(void *)];
        SurfDrawFilledRectFn fill = (SurfDrawFilledRectFn)vt[SURF_VT_DRAWFILLEDRECT / sizeof(void *)];
        setColor(surf, packedRgba);
        fill(surf, x0, y0, x1, y1);
    }
    InterlockedDecrement(&g_inOurDraw);
}

static void DrawRoundedFillAt(int x0, int y0, int w, int h, int r, unsigned int packedRgba,
                              int roundTop, int roundBottom)
{
    int y;
    int topR;
    int botR;
    if (w <= 0 || h <= 0) {
        return;
    }
    topR = roundTop ? r : 0;
    botR = roundBottom ? r : 0;
    if (topR * 2 + 4 > w) {
        topR = ClampInt(w / 4, 0, topR);
    }
    if (botR * 2 + 4 > w) {
        botR = ClampInt(w / 4, 0, botR);
    }
    if (topR + botR + 2 > h) {
        int cap = ClampInt((h - 2) / 2, 0, r);
        if (topR > cap) {
            topR = cap;
        }
        if (botR > cap) {
            botR = cap;
        }
    }
    if (topR < 3 && botR < 3) {
        SurfaceFill(x0, y0, x0 + w, y0 + h, packedRgba);
        return;
    }
    if (h > topR + botR) {
        SurfaceFill(x0, y0 + topR, x0 + w, y0 + h - botR, packedRgba);
    }
    for (y = 0; y < topR; y++) {
        int inset = CornerInset(y, topR * 2, topR);
        SurfaceFill(x0 + inset, y0 + y, x0 + w - inset, y0 + y + 1, packedRgba);
    }
    for (y = 0; y < botR; y++) {
        int inset = CornerInset(botR + y, botR * 2, botR);
        int py = y0 + h - botR + y;
        SurfaceFill(x0 + inset, py, x0 + w - inset, py + 1, packedRgba);
    }
}

static void __fastcall DrawSetColor_Hook(void *surf, void *edx, unsigned int packedRgba)
{
    (void)edx;
    g_curColor = packedRgba;
    if (g_origDrawSetColor != NULL) {
        g_origDrawSetColor(surf, packedRgba);
    }
}

static void __fastcall DrawFilledRect_Hook(void *surf, void *edx, int x0, int y0, int x1, int y1)
{
    int rw;
    int rh;
    int r;
    int roundTop;
    int roundBottom;
    (void)edx;
    if (InterlockedCompareExchange(&g_inOurDraw, 0, 0) != 0 || !g_roundActive ||
        g_origDrawFilledRect == NULL) {
        if (g_origDrawFilledRect != NULL) {
            g_origDrawFilledRect(surf, x0, y0, x1, y1);
        }
        return;
    }
    rw = x1 - x0;
    rh = y1 - y0;
    /* Stock FrameBorder/RaisedBorder is 1px hairlines on the square AABB.
     * Drop them while we are painting a rounded window. */
    if (rw <= 2 || rh <= 2) {
        return;
    }
    r = g_roundR;
    /* Main chrome: body of a dialog / sheet. */
    if (rw >= 80 && rh >= 48) {
        if (r * 2 + 4 > rw || r * 2 + 4 > rh) {
            r = ClampInt((rw < rh ? rw : rh) / 4, 3, r);
        }
        roundTop = (y0 <= 8);
        roundBottom = (g_roundH <= 0) || (y1 >= g_roundH - 8);
        if (!roundTop && !roundBottom) {
            roundTop = 1;
            roundBottom = 1;
        }
            /* This is the panel's own full body, not some smaller inner
             * sheet -- remember where it landed on screen so the caller
             * can trace a stroke around the same rounded rect once we're
             * back out of the engine's PaintBackground call. */
            if (g_roundW > 0 && g_roundH > 0 &&
                rw >= g_roundW - 4 && rh >= g_roundH - 4) {
                g_edgeCaptured = 1;
                g_edgeX = x0;
                g_edgeY = y0;
                g_edgeRoundTop = roundTop;
                g_edgeRoundBottom = roundBottom;
                g_edgeBodyColor = g_curColor;
            }
            DrawRoundedFillAt(x0, y0, rw, rh, r, g_curColor, roundTop, roundBottom);
            return;
    }
    /* Title bar / status strip sitting on the window edge. */
    if (rw >= 80 && rh >= 14 && rh < 48) {
        roundTop = (y0 <= 8);
        roundBottom = (g_roundH > 0) && (y1 >= g_roundH - 8);
        if (roundTop || roundBottom) {
            r = ClampInt(r, 3, rh - 1);
            DrawRoundedFillAt(x0, y0, rw, rh, r, g_curColor, roundTop, roundBottom);
            return;
        }
    }
    g_origDrawFilledRect(surf, x0, y0, x1, y1);
}

static void EnsureSurfaceHooks(void)
{
    void *surf;
    void **vt;
    DWORD oldProtect;
    if (g_surfaceHooked || g_GetSurface == NULL) {
        return;
    }
    surf = g_GetSurface();
    if (surf == NULL) {
        return;
    }
    vt = *(void ***)surf;
    if (vt == NULL) {
        return;
    }
    g_origDrawSetColor = (SurfDrawSetColorFn)vt[SURF_VT_DRAWSETCOLOR / sizeof(void *)];
    g_origDrawFilledRect = (SurfDrawFilledRectFn)vt[SURF_VT_DRAWFILLEDRECT / sizeof(void *)];
    if (g_origDrawSetColor == NULL || g_origDrawFilledRect == NULL) {
        return;
    }
    if (!VirtualProtect(&vt[SURF_VT_DRAWSETCOLOR / sizeof(void *)], 16, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }
    vt[SURF_VT_DRAWSETCOLOR / sizeof(void *)] = (void *)DrawSetColor_Hook;
    vt[SURF_VT_DRAWFILLEDRECT / sizeof(void *)] = (void *)DrawFilledRect_Hook;
    VirtualProtect(&vt[SURF_VT_DRAWSETCOLOR / sizeof(void *)], 16, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), &vt[SURF_VT_DRAWSETCOLOR / sizeof(void *)], 16);
    g_surfaceHooked = 1;
    HookLog("RoundFrame: ISurface DrawFilledRect hooked vt=%p", (void *)vt);
}

static void RunRoundedBackground(void *thisPtr, PaintFn orig)
{
    if (orig == NULL) {
        return;
    }
    if (InterlockedCompareExchange(&g_roundDisabled, 0, 0) != 0) {
        orig(thisPtr);
        return;
    }

    __try {
        if (ShouldRoundPanel(thisPtr)) {
            int w = 0, h = 0;
            g_GetSize(thisPtr, &w, &h);
            EnsureSurfaceHooks();
            /* Kill the scheme IBorder (FrameBorder/RaisedBorder) so the
             * square AABB stroke cannot sit outside the rounded fill --
             * we draw our own rounded stroke below instead of just
             * dropping it. */
            *(void **)((char *)thisPtr + OFF_PANEL_BORDER) = NULL;
            g_roundW = w;
            g_roundH = h;
            g_roundR = RadiusForSize(w, h);
            g_roundActive = 1;
            g_edgeCaptured = 0;
            orig(thisPtr);
            g_roundActive = 0;

            /* Trace a thin rounded stroke around the body we just painted,
             * while children still haven't drawn -- outer ring in a
             * lightened tint of the body color, then the same body color
             * inset by the stroke thickness to leave just the ring. */
            if (g_edgeCaptured) {
                int thickness = BORDER_STROKE_THICKNESS;
                unsigned int strokeColor = ShadeLighter(g_edgeBodyColor, BORDER_STROKE_LIGHTEN);
                int innerW = w - thickness * 2;
                int innerH = h - thickness * 2;
                int innerR = g_roundR - thickness;
                if (innerR < 0) {
                    innerR = 0;
                }
                DrawRoundedFillAt(g_edgeX, g_edgeY, w, h, g_roundR, strokeColor,
                                   g_edgeRoundTop, g_edgeRoundBottom);
                if (innerW > 0 && innerH > 0) {
                    DrawRoundedFillAt(g_edgeX + thickness, g_edgeY + thickness, innerW, innerH,
                                       innerR, g_edgeBodyColor, g_edgeRoundTop, g_edgeRoundBottom);
                }
            }
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_roundActive = 0;
        InterlockedExchange(&g_roundDisabled, 1);
        HookLog("RoundFrame: exception in rounded paint, disabling");
    }

    orig(thisPtr);
}

static void InstallNearHook(BYTE *target, unsigned stolen, const BYTE *expected,
                            BYTE *tramp, unsigned trampSize, void *hookFn, PaintFn *outOrig, const char *tag)
{
    DWORD oldProtect;
    DWORD trampProtect;
    INT32 relBack;
    INT32 relHook;
    unsigned i;

    if (memcmp(target, expected, stolen) != 0) {
        HookLog("RoundFrame: %s prologue mismatch at %p, skip", tag, (void *)target);
        return;
    }

    memcpy(tramp, target, stolen);
    tramp[stolen] = 0xE9;
    relBack = (INT32)((target + stolen) - (tramp + stolen + 5));
    memcpy(tramp + stolen + 1, &relBack, sizeof(relBack));

    if (!VirtualProtect(tramp, trampSize, PAGE_EXECUTE_READWRITE, &trampProtect)) {
        return;
    }
    *outOrig = (PaintFn)(void *)tramp;

    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }
    relHook = (INT32)((BYTE *)hookFn - (target + 5));
    target[0] = 0xE9;
    memcpy(target + 1, &relHook, sizeof(relHook));
    for (i = 5; i < stolen; i++) {
        target[i] = 0x90;
    }
    VirtualProtect(target, stolen, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    FlushInstructionCache(GetCurrentProcess(), tramp, trampSize);
    HookLog("RoundFrame: %s hooked %p -> %p", tag, (void *)target, hookFn);
}

static void __fastcall PanelPaintBg_Hook(void *thisPtr)
{
    int w = 0, h = 0;
    /* 0x43d60 is shared by lots of controls. Only round inner sheets
     * that actually fill a dialog; never the 64px logo strip. */
    if (g_GetSize != NULL && thisPtr != NULL) {
        g_GetSize(thisPtr, &w, &h);
        if (h < 100) {
            if (g_origPanelPaintBg != NULL) {
                g_origPanelPaintBg(thisPtr);
            }
            return;
        }
    }
    RunRoundedBackground(thisPtr, g_origPanelPaintBg);
}

static void __fastcall FramePaintBg_Hook(void *thisPtr)
{
    RunRoundedBackground(thisPtr, g_origFramePaintBg);
}

static void __fastcall CareerPaintBg_Hook(void *thisPtr)
{
    RunRoundedBackground(thisPtr, g_origCareerPaintBg);
}

static void __fastcall Paint18530_Hook(void *thisPtr)
{
    RunRoundedBackground(thisPtr, g_origPaint18530);
}

static void __fastcall FramePaintBgAlt_Hook(void *thisPtr)
{
    RunRoundedBackground(thisPtr, g_origFramePaintBgAlt);
}

static void __fastcall PaintBorder_Hook(void *thisPtr)
{
    if (InterlockedCompareExchange(&g_roundDisabled, 0, 0) != 0) {
        if (g_origPaintBorder != NULL) {
            g_origPaintBorder(thisPtr);
        }
        return;
    }

    __try {
        if (ShouldRoundPanel(thisPtr) ||
            *(void **)((char *)thisPtr + OFF_PANEL_BORDER) == NULL) {
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_roundDisabled, 1);
    }

    if (g_origPaintBorder != NULL) {
        g_origPaintBorder(thisPtr);
    }
}

void RoundFrame_Init(HMODULE hOriginalGameUI)
{
    BYTE *base = (BYTE *)hOriginalGameUI;
    static const BYTE kPanelBgPrologue[8] = { 0x83, 0xEC, 0x0C, 0x8D, 0x44, 0x24, 0x04, 0x56 };
    static const BYTE kFrameBgPrologue[6] = { 0x83, 0xEC, 0x20, 0x53, 0x55, 0x56 };
    static const BYTE kCareerBgPrologue[5] = { 0x83, 0xEC, 0x1C, 0x53, 0x56 };
    static const BYTE k18530Prologue[6] = { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1 };
    static const BYTE kBorderPrologue[7] = { 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x46, 0x2C };
    static const BYTE kAltPrologue[8] = { 0x83, 0xEC, 0x18, 0x8D, 0x44, 0x24, 0x08, 0x56 };

    InterlockedExchange(&g_roundDisabled, 0);
    g_surfaceHooked = 0;
    g_roundActive = 0;
    g_gameUiBase = base;
    g_GetSize = (GetSizeFn)(base + RVA_GETSIZE);
    g_GetSurface = (GetSurfaceFn)(base + RVA_GETSURFACE);

    InstallNearHook(base + RVA_FRAME_PAINTBACKGROUND, 6, kFrameBgPrologue,
                    g_framePaintBgTramp, sizeof(g_framePaintBgTramp),
                    (void *)FramePaintBg_Hook, &g_origFramePaintBg, "FramePaintBackground");
    InstallNearHook(base + RVA_PANEL_PAINTBACKGROUND, 8, kPanelBgPrologue,
                    g_panelPaintBgTramp, sizeof(g_panelPaintBgTramp),
                    (void *)PanelPaintBg_Hook, &g_origPanelPaintBg, "PanelPaintBackground");
    InstallNearHook(base + RVA_CAREER_PAINTBACKGROUND, 5, kCareerBgPrologue,
                    g_careerPaintBgTramp, sizeof(g_careerPaintBgTramp),
                    (void *)CareerPaintBg_Hook, &g_origCareerPaintBg, "CareerPaintBackground");
    InstallNearHook(base + RVA_PAINTBACKGROUND_18530, 6, k18530Prologue,
                    g_paint18530Tramp, sizeof(g_paint18530Tramp),
                    (void *)Paint18530_Hook, &g_origPaint18530, "PaintBackground18530");
    InstallNearHook(base + RVA_FRAME_PAINTBG_ALT, 8, kAltPrologue,
                    g_framePaintBgAltTramp, sizeof(g_framePaintBgAltTramp),
                    (void *)FramePaintBgAlt_Hook, &g_origFramePaintBgAlt, "FramePaintBgAlt");
    InstallNearHook(base + RVA_PAINTBORDER, 7, kBorderPrologue,
                    g_paintBorderTramp, sizeof(g_paintBorderTramp),
                    (void *)PaintBorder_Hook, &g_origPaintBorder, "PaintBorder");
}
