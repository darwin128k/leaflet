#include "roundframe.h"
#include "scheme.h"
#include "log.h"
#include <math.h>
#include <string.h>

typedef void(__thiscall *SetPosFn)(void *self, int x, int y);
typedef void(__thiscall *SetSizeFn)(void *self, int wide, int tall);
typedef void(__thiscall *GetPosFn)(void *self, int *outX, int *outY);
typedef void(__thiscall *GetSizeFn)(void *self, int *outWide, int *outTall);
typedef void(__thiscall *PaintFn)(void *self);
typedef void *(__cdecl *GetSurfaceFn)(void);
typedef void(__thiscall *SurfDrawSetColorFn)(void *surf, unsigned int packedRgba);
typedef void(__thiscall *SurfDrawFilledRectFn)(void *surf, int x0, int y0, int x1, int y1);
typedef char(__thiscall *ByteGetterFn)(void *self);
typedef void(__thiscall *SetPackedColorFn)(void *self, unsigned int packedRgba);
typedef void(__thiscall *SetTwoColorsFn)(void *self, unsigned int packedFg, unsigned int packedBg);
typedef void(__thiscall *SetIntFn)(void *self, int value);
typedef void(__thiscall *SetTextInsetFn)(void *self, int xInset, int yInset);

#define RVA_SETPOS                0x000436f0u
#define RVA_GETPOS                0x00043720u
#define RVA_SETSIZE               0x00043750u
#define RVA_GETSIZE               0x00043780u
#define RVA_GETSURFACE            0x0003f040u
#define RVA_PANEL_PAINTBACKGROUND 0x00043d60u
#define RVA_BUTTON_DRAWFOCUS      0x000407e0u /* Button::DrawFocusBox — dashed keyboard-focus rect */
#define RVA_BUTTON_PAINT          0x0003fa30u /* vgui2::Button::Paint (also PageTab/ToggleButton) */
#define RVA_BUTTON_VTABLE         0x0009caccu /* vgui2::Button vtable; Label/CheckButton/PageTab differ */
#define RVA_FRAMEBUTTON_VTABLE    0x0009dd24u /* vgui2::FrameButton — caption close/min/max, not Button vt */
#define RVA_LABEL_VTABLE          0x0009cdf4u
#define RVA_URLLABEL_VTABLE       0x000a023cu
#define RVA_PAGETAB_VTABLE        0x000a482cu
#define OFF_BUTTON_ISARMED_VT     0x2a4
#define OFF_BUTTON_ISDEPRESSED_VT 0x2a8
#define OFF_BUTTON_ISSELECTED_VT  0x2b8
#define OFF_BUTTON_SETDEFAULTCOLOR_VT 0x2ec
#define OFF_BUTTON_SETARMEDCOLOR_VT   0x2f0
#define OFF_BUTTON_SETSELECTEDCOLOR_VT 0x2f4
#define OFF_BUTTON_SETCONTENTALIGNMENT_VT 0x22c /* Label::SetContentAlignment, same fn as MenuItem */
#define OFF_BUTTON_SETTEXTINSET_VT        0x230 /* Label::SetTextInset(x,y) — leftover 6px west inset from scheme */
#define LABEL_ALIGN_CENTER 4 /* a_northwest=0 ... a_west=3, a_center=4 */
#define OFF_SETFGCOLOR_VT         0xD0 /* Button/Label::SetFgColor — also updates TextImage */
#define OFF_PAGETAB_ACTIVE        0x108
#define OFF_PAGETAB_ACTIVE_FG     0x109
#define COLOR_FG_WHITE            0xFFF7F5F5u /* r,g,b,a little-endian */
#define COLOR_BG_TRANSPARENT      0x00000000u
#define RVA_FRAME_PAINTBACKGROUND 0x0004cb60u /* Frame/PropertyDialog/MessageBox/COptionsDialog */
#define RVA_CAREER_PAINTBACKGROUND 0x00002070u
#define RVA_PAINTBACKGROUND_18530 0x00018530u
#define RVA_PAINTBORDER           0x00043d40u
#define RVA_FRAME_PAINTBG_ALT     0x00023970u
#define RVA_PROGRESSBAR_PAINTBG   0x000696b0u /* vgui2::ProgressBar::PaintBackground — cube segments */
#define RVA_PROGRESSBAR_VTABLE    0x000a1dccu
#define OFF_PROGRESS              0x78 /* float 0..1; confirmed via fmul [esi+0x78] in PaintBackground */

#define OFF_PANEL_NAME   0x44
#define OFF_PANEL_BORDER 0x2C /* IBorder* loaded by Panel::PaintBorder */
#define SURF_VT_DRAWSETCOLOR       0x1C
#define SURF_VT_DRAWFILLEDRECT     0x24

static BYTE *g_gameUiBase = NULL;
static SetPosFn g_SetPos = NULL;
static GetPosFn g_GetPos = NULL;
static SetSizeFn g_SetSize = NULL;
static GetSizeFn g_GetSize = NULL;
static GetSurfaceFn g_GetSurface = NULL;

static PaintFn g_origPanelPaintBg = NULL;
static PaintFn g_origButtonPaint = NULL;
static PaintFn g_origFramePaintBg = NULL;
static PaintFn g_origCareerPaintBg = NULL;
static PaintFn g_origPaint18530 = NULL;
static PaintFn g_origPaintBorder = NULL;
static PaintFn g_origFramePaintBgAlt = NULL;
static PaintFn g_origProgressPaintBg = NULL;
static BYTE g_panelPaintBgTramp[32];
static BYTE g_buttonPaintTramp[32];
static BYTE g_framePaintBgTramp[32];
static BYTE g_careerPaintBgTramp[32];
static BYTE g_paint18530Tramp[32];
static BYTE g_paintBorderTramp[32];
static BYTE g_framePaintBgAltTramp[32];
static BYTE g_progressPaintBgTramp[32];

static SurfDrawSetColorFn g_origDrawSetColor = NULL;
static SurfDrawFilledRectFn g_origDrawFilledRect = NULL;
static int g_surfaceHooked = 0;

static volatile LONG g_roundDisabled = 0;
static volatile LONG g_inOurDraw = 0;
static int g_roundActive = 0;
static int g_roundW = 0;
static int g_roundH = 0;
static int g_roundR = 0;
static int g_roundIsButton = 0;
static int g_roundHot = 0;
static unsigned int g_curColor = 0xE0101410u;
static OverlayTheme g_theme;

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

static void EnsureSurfaceHooks(void);
static void DrawRoundedFillAt(int x0, int y0, int w, int h, int r, unsigned int packedRgba,
                              int roundTop, int roundBottom);
static void DrawAaDisk(int x0, int y0, int d, uint32_t rgb);

static unsigned int ThemeRgbPacked(uint32_t rgb)
{
    unsigned int r = (rgb >> 16) & 0xFFu;
    unsigned int g = (rgb >> 8) & 0xFFu;
    unsigned int b = rgb & 0xFFu;
    return (0xFFu << 24) | (b << 16) | (g << 8) | r;
}

static unsigned int ThemeStrokePacked(void)
{
    return ThemeRgbPacked(g_theme.borderRgb);
}

static int ThemeStrokeThickness(void)
{
    int t = g_theme.borderWidth;
    if (t < 1) {
        t = 1;
    }
    if (t > 4) {
        t = 4;
    }
    return t;
}

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

/* Same radius as the LVGL prefetch card (overlay.cpp WIN radius 12). */
static int RadiusForSize(int w, int h)
{
    (void)w;
    (void)h;
    return 12;
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

static int NameIsOptionsTab(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (lstrcmpiA(name, "Multiplayer") == 0 || lstrcmpiA(name, "Keyboard") == 0
        || lstrcmpiA(name, "Mouse") == 0 || lstrcmpiA(name, "Audio") == 0
        || lstrcmpiA(name, "Video") == 0 || lstrcmpiA(name, "Voice") == 0
        || lstrcmpiA(name, "Lock") == 0) {
        return 1;
    }
    return 0;
}

static int NameIsMenuChrome(const char *name);

static int IsVguiButton(void *thisPtr)
{
    void *vt;
    if (thisPtr == NULL || g_gameUiBase == NULL) {
        return 0;
    }
    vt = *(void **)thisPtr;
    return vt == (void *)(g_gameUiBase + RVA_BUTTON_VTABLE)
        || vt == (void *)(g_gameUiBase + RVA_FRAMEBUTTON_VTABLE);
}

static int IsStaticTextPanel(void *thisPtr)
{
    void *vt;
    if (thisPtr == NULL || g_gameUiBase == NULL) {
        return 0;
    }
    vt = *(void **)thisPtr;
    return vt == (void *)(g_gameUiBase + RVA_LABEL_VTABLE)
        || vt == (void *)(g_gameUiBase + RVA_URLLABEL_VTABLE);
}

static int IsPageTab(void *thisPtr)
{
    void *vt;
    if (thisPtr == NULL || g_gameUiBase == NULL) {
        return 0;
    }
    vt = *(void **)thisPtr;
    return vt == (void *)(g_gameUiBase + RVA_PAGETAB_VTABLE);
}

static int IsProgressBar(void *thisPtr)
{
    void *vt;
    if (thisPtr == NULL || g_gameUiBase == NULL) {
        return 0;
    }
    vt = *(void **)thisPtr;
    return vt == (void *)(g_gameUiBase + RVA_PROGRESSBAR_VTABLE);
}

static int IsTitleCloseButton(void *thisPtr)
{
    int w = 0, h = 0;
    const char *name;
    void *vt;
    if (thisPtr == NULL || g_GetSize == NULL || g_gameUiBase == NULL) {
        return 0;
    }
    g_GetSize(thisPtr, &w, &h);
    if (w < 10 || h < 10 || w > 32 || h > 32) {
        return 0;
    }
    if (g_GetPos != NULL) {
        int x = 0;
        int y = 0;
        g_GetPos(thisPtr, &x, &y);
        if (y > 36) {
            return 0;
        }
    }
    vt = *(void **)thisPtr;
    if (vt == (void *)(g_gameUiBase + RVA_FRAMEBUTTON_VTABLE)) {
        return 1;
    }
    name = PanelName(thisPtr);
    if (lstrcmpiA(name, "close") == 0 || lstrcmpiA(name, "CloseButton") == 0) {
        return 1;
    }
    return 0;
}

static int VtableFlag(void *thisPtr, unsigned vtOff)
{
    void **vtable;
    ByteGetterFn fn;
    if (thisPtr == NULL) {
        return 0;
    }
    vtable = *(void ***)thisPtr;
    if (vtable == NULL) {
        return 0;
    }
    fn = (ByteGetterFn)vtable[vtOff / sizeof(void *)];
    if (fn == NULL) {
        return 0;
    }
    return fn(thisPtr) != 0;
}

static int ControlIsHot(void *thisPtr)
{
    if (IsPageTab(thisPtr)) {
        if (*((unsigned char *)thisPtr + OFF_PAGETAB_ACTIVE) != 0) {
            return 1;
        }
    }
    return VtableFlag(thisPtr, OFF_BUTTON_ISARMED_VT)
        || VtableFlag(thisPtr, OFF_BUTTON_ISDEPRESSED_VT)
        || VtableFlag(thisPtr, OFF_BUTTON_ISSELECTED_VT);
}

static void ForceWhiteOnTransparent(void *thisPtr)
{
    void **vtable;
    SetTwoColorsFn setDefaultColor;
    SetTwoColorsFn setArmedColor;
    SetTwoColorsFn setSelectedColor;
    if (thisPtr == NULL) {
        return;
    }
    vtable = *(void ***)thisPtr;
    setDefaultColor = (SetTwoColorsFn)vtable[OFF_BUTTON_SETDEFAULTCOLOR_VT / sizeof(void *)];
    setArmedColor = (SetTwoColorsFn)vtable[OFF_BUTTON_SETARMEDCOLOR_VT / sizeof(void *)];
    setSelectedColor = (SetTwoColorsFn)vtable[OFF_BUTTON_SETSELECTEDCOLOR_VT / sizeof(void *)];
    setDefaultColor(thisPtr, COLOR_FG_WHITE, COLOR_BG_TRANSPARENT);
    setArmedColor(thisPtr, COLOR_FG_WHITE, COLOR_BG_TRANSPARENT);
    setSelectedColor(thisPtr, COLOR_FG_WHITE, COLOR_BG_TRANSPARENT);
}

static void SetFgColorWhite(void *thisPtr)
{
    void **vtable;
    SetPackedColorFn setFg;
    if (thisPtr == NULL) {
        return;
    }
    vtable = *(void ***)thisPtr;
    setFg = (SetPackedColorFn)vtable[OFF_SETFGCOLOR_VT / sizeof(void *)];
    if (setFg != NULL) {
        setFg(thisPtr, COLOR_FG_WHITE);
    }
}

static void PaintControlPlate(void *thisPtr)
{
    int w = 0, h = 0;
    int r;
    unsigned int fill;
    if (g_GetSize == NULL) {
        return;
    }
    g_GetSize(thisPtr, &w, &h);
    if (w < 8 || h < 8) {
        return;
    }
    EnsureSurfaceHooks();
    r = (h < 20) ? 4 : 8;
    if (r * 2 > h) {
        r = h / 2;
    }
    fill = ControlIsHot(thisPtr) ? ThemeRgbPacked(g_theme.accentRgb)
                                 : ThemeRgbPacked(g_theme.trackRgb);
    DrawRoundedFillAt(0, 0, w, h, r, fill, 1, 1);
}

static int ShouldRoundButton(void *thisPtr)
{
    int w = 0, h = 0;
    const char *name;
    if (g_GetSize == NULL || thisPtr == NULL) {
        return 0;
    }
    /* Exact vgui2::Button only. Labels, checkboxes, combo arrows and
     * page tabs share similar sizes / Paint but have other vtables. */
    if (!IsVguiButton(thisPtr)) {
        return 0;
    }
    g_GetSize(thisPtr, &w, &h);
    /* Title-bar close / icon are ~20px. A plate here paints over the
     * first letters of the Frame title (Options → tions, Quit → uit).
     * After disconnect ClientScheme can size those chrome buttons a bit
     * wider, so skip anything sitting in the caption strip. */
    if (h < 14 || h > 48 || w < 40 || w > 400) {
        return 0;
    }
    if (g_GetPos != NULL) {
        int x = 0;
        int y = 0;
        g_GetPos(thisPtr, &x, &y);
        if (y < 36 && h <= 32) {
            return 0;
        }
    }
    name = PanelName(thisPtr);
    if (NameIsMenuChrome(name) || NameIsOptionsTab(name)) {
        return 0;
    }
    return 1;
}

static void CenterRoundedButtonText(void *thisPtr)
{
    void **vtable;
    SetIntFn setAlign;
    SetTextInsetFn setInset;
    if (thisPtr == NULL) {
        return;
    }
    vtable = *(void ***)thisPtr;
    setAlign = (SetIntFn)vtable[OFF_BUTTON_SETCONTENTALIGNMENT_VT / sizeof(void *)];
    setInset = (SetTextInsetFn)vtable[OFF_BUTTON_SETTEXTINSET_VT / sizeof(void *)];
    /* Scheme/ApplySchemeSettings leaves TextInset=6 even after a_center.
     * GoldSrc Label still adds that to x, so OK/Cancel look left of the plate. */
    setInset(thisPtr, 0, 0);
    setAlign(thisPtr, LABEL_ALIGN_CENTER);
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
    if (NameContainsI(name, "MOTD") || NameContainsI(name, "TeamMenu")
        || NameContainsI(name, "ClassMenu") || NameContainsI(name, "MapInfo")
        || lstrcmpiA(name, "Message") == 0
        || lstrcmpiA(name, "ViewPortBackGround") == 0) {
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

static void DrawRoundedStrokeAt(int x0, int y0, int w, int h, int r, unsigned int packedRgba,
                                int thickness, int roundTop, int roundBottom)
{
    int y;
    int topR;
    int botR;
    if (w <= 0 || h <= 0) {
        return;
    }
    if (thickness < 1) {
        thickness = 1;
    }
    topR = roundTop ? r : 0;
    botR = roundBottom ? r : 0;
    if (topR * 2 + 4 > w) {
        topR = ClampInt(w / 4, 0, topR);
    }
    if (botR * 2 + 4 > w) {
        botR = ClampInt(w / 4, 0, botR);
    }
    for (y = 0; y < h; y++) {
        int inset = 0;
        int xL;
        int xR;
        if (y < topR) {
            inset = CornerInset(y, topR * 2, topR);
        } else if (y >= h - botR) {
            inset = CornerInset(botR + (y - (h - botR)), botR * 2, botR);
        }
        xL = x0 + inset;
        xR = x0 + w - inset;
        if (xR <= xL) {
            continue;
        }
        if (y < thickness || y >= h - thickness) {
            SurfaceFill(xL, y0 + y, xR, y0 + y + 1, packedRgba);
        } else {
            int tw = thickness;
            if (xL + tw > xR) {
                tw = xR - xL;
            }
            SurfaceFill(xL, y0 + y, xL + tw, y0 + y + 1, packedRgba);
            SurfaceFill(xR - tw, y0 + y, xR, y0 + y + 1, packedRgba);
        }
    }
}

/* Capsule of height h (even): semicircle caps of radius h/2 on both ends.
 * CornerInset() is for large dialog radii and returns 0 on a 6px bar. */
static int PillInset(int y, int h)
{
    int dy;
    int inner;
    int half;
    int r;
    if (h < 2) {
        return 0;
    }
    r = h / 2;
    dy = y * 2 + 1 - h;
    inner = h * h - dy * dy;
    if (inner <= 0) {
        return r;
    }
    half = (ISqrt(inner) + 1) / 2;
    if (half > r) {
        half = r;
    }
    return r - half;
}

static void DrawPillAt(int x0, int y0, int w, int h, unsigned int packedRgba)
{
    int y;
    if (w <= 0 || h <= 0) {
        return;
    }
    if ((h & 1) != 0) {
        h -= 1;
    }
    if (h < 2) {
        SurfaceFill(x0, y0, x0 + w, y0 + h, packedRgba);
        return;
    }
    for (y = 0; y < h; y++) {
        int inset = PillInset(y, h);
        int xL;
        int xR;
        if (inset < 0) {
            inset = 0;
        }
        if (inset * 2 >= w) {
            inset = (w - 1) / 2;
            if (inset < 0) {
                continue;
            }
        }
        xL = x0 + inset;
        xR = x0 + w - inset;
        if (xR > xL) {
            SurfaceFill(xL, y0 + y, xR, y0 + y + 1, packedRgba);
        }
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
    if (rw <= 2 || rh <= 2) {
        return;
    }
    r = g_roundR;
    /* Dialog buttons: OK / Cancel / Apply / Advanced — same rounded plate
     * language as the prefetch card, not the square VGUI fill. */
    if (g_roundIsButton && rh >= 14 && rh <= 40 && rw >= 24) {
        if (r * 2 > rh) {
            r = rh / 2;
        }
        if (r < 3) {
            r = 3;
        }
        {
            unsigned int fill = g_roundHot ? ThemeRgbPacked(g_theme.accentRgb)
                                           : ThemeRgbPacked(g_theme.trackRgb);
            g_edgeCaptured = 1;
            g_edgeX = x0;
            g_edgeY = y0;
            g_edgeRoundTop = 1;
            g_edgeRoundBottom = 1;
            DrawRoundedFillAt(x0, y0, rw, rh, r, fill, 1, 1);
        }
        return;
    }
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
        if (ShouldRoundButton(thisPtr) || ShouldRoundPanel(thisPtr)) {
            int w = 0, h = 0;
            int isBtn;
            g_GetSize(thisPtr, &w, &h);
            EnsureSurfaceHooks();
            *(void **)((char *)thisPtr + OFF_PANEL_BORDER) = NULL;
            isBtn = ShouldRoundButton(thisPtr);
            g_roundW = w;
            g_roundH = h;
            g_roundIsButton = isBtn;
            g_roundHot = isBtn && ControlIsHot(thisPtr);
            g_roundR = isBtn ? 8 : RadiusForSize(w, h);
            g_roundActive = 1;
            g_edgeCaptured = 0;
            orig(thisPtr);
            g_roundActive = 0;
            g_roundIsButton = 0;
            g_roundHot = 0;

            if (isBtn) {
                int px = g_edgeCaptured ? g_edgeX : 0;
                int py = g_edgeCaptured ? g_edgeY : 0;
                unsigned int fill = ControlIsHot(thisPtr) ? ThemeRgbPacked(g_theme.accentRgb)
                                                          : ThemeRgbPacked(g_theme.trackRgb);
                DrawRoundedFillAt(px, py, w, h, 8, fill, 1, 1);
            } else if (g_edgeCaptured) {
                /* Orig already drew the rounded body and then the Frame
                 * title. Refilling the interior here ate "Op" / "Q" after
                 * disconnect, when ClientScheme made the body fill match
                 * the panel and g_edgeCaptured flipped on. Stroke only. */
                DrawRoundedStrokeAt(g_edgeX, g_edgeY, w, h, g_roundR,
                                    ThemeStrokePacked(), ThemeStrokeThickness(),
                                    g_edgeRoundTop, g_edgeRoundBottom);
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
    /* Labels keep scheme LabelBgColor (ControlBG, alpha 242) which reads
     * as a second grey box on the inner sheet. Skip the fill so static
     * text sits on the parent. */
    if (IsStaticTextPanel(thisPtr)) {
        return;
    }
    /* 0x43d60 is shared by lots of controls. Only round inner sheets
     * that actually fill a dialog; never the 64px logo strip. */
    if (g_GetSize != NULL && thisPtr != NULL) {
        g_GetSize(thisPtr, &w, &h);
        if (ShouldRoundButton(thisPtr)) {
            RunRoundedBackground(thisPtr, g_origPanelPaintBg);
            return;
        }
        if (h < 100) {
            if (g_origPanelPaintBg != NULL) {
                g_origPanelPaintBg(thisPtr);
            }
            return;
        }
        if (ShouldRoundPanel(thisPtr)) {
            RunRoundedBackground(thisPtr, g_origPanelPaintBg);
            return;
        }
    }
    if (g_origPanelPaintBg != NULL) {
        g_origPanelPaintBg(thisPtr);
    }
}

static void PaintMacCloseDot(void *thisPtr)
{
    int w = 0, h = 0;
    int x = 0;
    int y = 0;
    int d;
    int ox;
    int oy;
    unsigned int rgb;
    if (g_GetSize == NULL) {
        return;
    }
    g_GetSize(thisPtr, &w, &h);
    if (g_GetPos != NULL) {
        g_GetPos(thisPtr, &x, &y);
    }
    /* Stock Frame parks this on the top-right corner; our 12px window
     * radius clips it into a wedge. Pull it in once (stock is ~18–20px). */
    if (g_SetSize != NULL && g_SetPos != NULL && w >= 18) {
        g_SetSize(thisPtr, 16, 16);
        g_SetPos(thisPtr, x - 2, 10);
        w = 16;
        h = 16;
    }
    d = 10;
    ox = (w - d) / 2;
    oy = (h - d) / 2;
    if (ox < 0) {
        ox = 0;
    }
    if (oy < 0) {
        oy = 0;
    }
    EnsureSurfaceHooks();
    if (VtableFlag(thisPtr, OFF_BUTTON_ISDEPRESSED_VT)) {
        rgb = 0xBF4942u;
    } else if (ControlIsHot(thisPtr)) {
        rgb = 0xFF8B86u;
    } else {
        rgb = 0xFF5F57u;
    }
    DrawAaDisk(ox, oy, d, rgb);
}

static void DrawAaDisk(int x0, int y0, int d, uint32_t rgb)
{
    float cx;
    float cy;
    float rad;
    int px;
    int py;
    int sr;
    int sg;
    int sb;
    int br;
    int bg;
    int bb;

    if (d < 4) {
        return;
    }
    cx = (float)x0 + (float)d * 0.5f;
    cy = (float)y0 + (float)d * 0.5f;
    rad = (float)d * 0.5f - 0.4f;
    sr = (int)((rgb >> 16) & 0xFFu);
    sg = (int)((rgb >> 8) & 0xFFu);
    sb = (int)(rgb & 0xFFu);
    br = (int)((g_theme.windowRgb >> 16) & 0xFFu);
    bg = (int)((g_theme.windowRgb >> 8) & 0xFFu);
    bb = (int)(g_theme.windowRgb & 0xFFu);
    for (py = y0; py < y0 + d; py++) {
        for (px = x0; px < x0 + d; px++) {
            float dx = ((float)px + 0.5f) - cx;
            float dy = ((float)py + 0.5f) - cy;
            float dist = (float)sqrt(dx * dx + dy * dy);
            float a = (rad + 1.15f) - dist;
            int or_;
            int og;
            int ob;
            if (a <= 0.0f) {
                continue;
            }
            if (a > 1.0f) {
                a = 1.0f;
            }
            or_ = (int)((float)sr * a + (float)br * (1.0f - a) + 0.5f);
            og = (int)((float)sg * a + (float)bg * (1.0f - a) + 0.5f);
            ob = (int)((float)sb * a + (float)bb * (1.0f - a) + 0.5f);
            SurfaceFill(px, py, px + 1, py + 1, ThemeRgbPacked(
                ((unsigned)or_ << 16) | ((unsigned)og << 8) | (unsigned)ob));
        }
    }
}

static void __fastcall ButtonPaint_Hook(void *thisPtr)
{
    int roundBtn;
    int tab;
    int tabHot;

    if (IsTitleCloseButton(thisPtr)) {
        PaintMacCloseDot(thisPtr);
        return;
    }

    roundBtn = ShouldRoundButton(thisPtr);
    tab = IsPageTab(thisPtr);
    tabHot = tab && ControlIsHot(thisPtr);

    if (roundBtn) {
        ForceWhiteOnTransparent(thisPtr);
        SetFgColorWhite(thisPtr);
        CenterRoundedButtonText(thisPtr);
        PaintControlPlate(thisPtr);
    } else if (tab) {
        CenterRoundedButtonText(thisPtr);
        if (tabHot) {
        unsigned char *fg = (unsigned char *)thisPtr + OFF_PAGETAB_ACTIVE_FG;
        /* Selected colour at +0x109, idle/hover colour at +0x10D. */
        fg[0] = 245;
        fg[1] = 245;
        fg[2] = 247;
        fg[3] = 255;
        fg[4] = 245;
        fg[5] = 245;
        fg[6] = 247;
        fg[7] = 255;
        ForceWhiteOnTransparent(thisPtr);
        SetFgColorWhite(thisPtr);
        PaintControlPlate(thisPtr);
        }
    }
    if (g_origButtonPaint != NULL) {
        g_origButtonPaint(thisPtr);
    }
    /* PageTab::Paint can re-apply scheme (blue selected text) after our
     * SetFgColor. Hover fixed it because SetArmed runs SetFgColor again.
     * Repeat the same on the first frame so the TextImage is white. */
    if (tabHot) {
        SetFgColorWhite(thisPtr);
        if (g_origButtonPaint != NULL) {
            g_origButtonPaint(thisPtr);
        }
    }
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

static void __fastcall ProgressPaintBg_Hook(void *thisPtr)
{
    int w = 0, h = 0;
    int barH;
    int y;
    int fillW;
    float p;

    if (g_GetSize == NULL || thisPtr == NULL) {
        if (g_origProgressPaintBg != NULL) {
            g_origProgressPaintBg(thisPtr);
        }
        return;
    }
    g_GetSize(thisPtr, &w, &h);
    if (w < 16 || h < 4) {
        if (g_origProgressPaintBg != NULL) {
            g_origProgressPaintBg(thisPtr);
        }
        return;
    }

    EnsureSurfaceHooks();
    p = *(float *)((char *)thisPtr + OFF_PROGRESS);
    if (p < 0.0f) {
        p = 0.0f;
    }
    if (p > 1.0f) {
        p = 1.0f;
    }
    /* Same 6px stadium as overlay.cpp prefetch lv_bar (BTN_H). */
    barH = 6;
    if (barH > h - 2) {
        barH = h - 2;
    }
    if (barH < 4) {
        barH = h;
    }
    if ((barH & 1) != 0) {
        barH -= 1;
    }
    y = (h - barH) / 2;
    DrawPillAt(0, y, w, barH, ThemeRgbPacked(g_theme.trackRgb));
    fillW = (int)(p * (float)w + 0.5f);
    if (fillW > 0) {
        int row;
        if (fillW < barH) {
            fillW = barH;
        }
        if (fillW > w) {
            fillW = w;
        }
        for (row = 0; row < barH; row++) {
            int inset = PillInset(row, barH);
            int xL = inset;
            int xTrackR = w - inset;
            int xFillR = fillW - inset;
            if (xL < 0) {
                xL = 0;
            }
            if (xFillR > xTrackR) {
                xFillR = xTrackR;
            }
            if (xFillR > xL) {
                SurfaceFill(xL, y + row, xFillR, y + row + 1, ThemeRgbPacked(g_theme.accentRgb));
            }
        }
    }
}

static void __fastcall PaintBorder_Hook(void *thisPtr)
{
    if (IsProgressBar(thisPtr) || IsTitleCloseButton(thisPtr)) {
        return;
    }
    if (InterlockedCompareExchange(&g_roundDisabled, 0, 0) != 0) {
        if (g_origPaintBorder != NULL) {
            g_origPaintBorder(thisPtr);
        }
        return;
    }

    __try {
        if (ShouldRoundPanel(thisPtr) || ShouldRoundButton(thisPtr) ||
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
    g_SetPos = (SetPosFn)(base + RVA_SETPOS);
    g_GetPos = (GetPosFn)(base + RVA_GETPOS);
    g_SetSize = (SetSizeFn)(base + RVA_SETSIZE);
    g_GetSize = (GetSizeFn)(base + RVA_GETSIZE);
    g_GetSurface = (GetSurfaceFn)(base + RVA_GETSURFACE);

    OverlayTheme_Load(&g_theme);
    HookLog("RoundFrame: stroke rgb=%06X width=%d", g_theme.borderRgb, g_theme.borderWidth);

    InstallNearHook(base + RVA_FRAME_PAINTBACKGROUND, 6, kFrameBgPrologue,
                    g_framePaintBgTramp, sizeof(g_framePaintBgTramp),
                    (void *)FramePaintBg_Hook, &g_origFramePaintBg, "FramePaintBackground");
    InstallNearHook(base + RVA_PANEL_PAINTBACKGROUND, 8, kPanelBgPrologue,
                    g_panelPaintBgTramp, sizeof(g_panelPaintBgTramp),
                    (void *)PanelPaintBg_Hook, &g_origPanelPaintBg, "PanelPaintBackground");
    {
        static const BYTE kButtonPaintPrologue[6] = { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1 };
        InstallNearHook(base + RVA_BUTTON_PAINT, 6, kButtonPaintPrologue,
                        g_buttonPaintTramp, sizeof(g_buttonPaintTramp),
                        (void *)ButtonPaint_Hook, &g_origButtonPaint, "ButtonPaint");
    }
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
    {
        static const BYTE kProgressBgPrologue[6] = { 0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56 };
        InstallNearHook(base + RVA_PROGRESSBAR_PAINTBG, 6, kProgressBgPrologue,
                        g_progressPaintBgTramp, sizeof(g_progressPaintBgTramp),
                        (void *)ProgressPaintBg_Hook, &g_origProgressPaintBg, "ProgressBarPaintBackground");
    }

    /* Default/OK buttons and tabs draw a dotted inset rect on focus.
     * Scheme ButtonKeyFocusBorder is already empty; this is a code path. */
    {
        BYTE *focus = base + RVA_BUTTON_DRAWFOCUS;
        DWORD oldProtect;
        if (focus[0] == 0x53 && focus[1] == 0x55 && focus[2] == 0x56) {
            if (VirtualProtect(focus, 3, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                /* thiscall + 4 stack args; a bare ret left 16 bytes and
                 * crashed on the next tab click. */
                focus[0] = 0xC2;
                focus[1] = 0x10;
                focus[2] = 0x00;
                VirtualProtect(focus, 3, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), focus, 3);
                HookLog("RoundFrame: DrawFocusBox disabled");
            }
        }
    }
}
