#include "roundframe.h"
#include "scheme.h"
#include "log.h"
#include "audioextra.h"
#include "bgswitch.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef void(__thiscall *SetPosFn)(void *self, int x, int y);
typedef void(__thiscall *SetSizeFn)(void *self, int wide, int tall);
typedef void(__thiscall *GetPosFn)(void *self, int *outX, int *outY);
typedef void(__thiscall *GetSizeFn)(void *self, int *outWide, int *outTall);
typedef void(__thiscall *PaintFn)(void *self);
typedef void *(__cdecl *GetSurfaceFn)(void);
typedef void(__thiscall *SurfDrawSetColorFn)(void *surf, unsigned int packedRgba);
typedef void(__thiscall *SurfDrawFilledRectFn)(void *surf, int x0, int y0, int x1, int y1);
typedef void(__thiscall *SurfGetScreenSizeFn)(void *surf, int *outWide, int *outTall);
typedef void(__thiscall *SurfSetCursorFn)(void *surf, unsigned long cursor);
typedef char(__thiscall *ByteGetterFn)(void *self);
typedef void(__thiscall *SetPackedColorFn)(void *self, unsigned int packedRgba);
typedef void(__thiscall *SetTwoColorsFn)(void *self, unsigned int packedFg, unsigned int packedBg);
typedef void(__thiscall *SetIntFn)(void *self, int value);
typedef void(__thiscall *SetTextInsetFn)(void *self, int xInset, int yInset);
typedef void(__thiscall *SetImageAtIndexFn)(void *self, int index, void *image, int preOffset);
typedef void(__thiscall *SetBoolFn)(void *self, unsigned char value);
typedef void(__thiscall *IImagePaintFn)(void *image);
typedef void(__thiscall *IImageSetPosFn)(void *image, int x, int y);
typedef void(__thiscall *IImageGetSizeFn)(void *image, int *outWide, int *outTall);
typedef void(__thiscall *IImageSetSizeFn)(void *image, int wide, int tall);
typedef void(__thiscall *ResizeToContentFn)(void *image);
typedef void(__thiscall *SetDrawWidthFn)(void *image, int width);

#define RVA_SETPOS                0x000436f0u
#define RVA_GETPOS                0x00043720u
#define RVA_SETSIZE               0x00043750u
#define RVA_GETSIZE               0x00043780u
#define RVA_GETSURFACE            0x0003f040u
#define RVA_PANEL_PAINTBACKGROUND 0x00043d60u
#define RVA_BUTTON_DRAWFOCUS      0x000407e0u /* Button::DrawFocusBox — dashed keyboard-focus rect */
#define RVA_BUTTON_PAINT          0x0003fa30u /* vgui2::Button::Paint (also PageTab/ToggleButton) */
#define RVA_BUTTON_VTABLE         0x0009caccu /* vgui2::Button vtable; Label/CheckButton/PageTab differ */
#define RVA_CCVARTOGGLE_VTABLE    0x00097f9cu /* CCvarToggleCheckButton */
#define RVA_DESCCHECKBUTTON_VTABLE 0x000a12f4u /* Advanced BOOL: CheckButton named DescCheckButton */
#define RVA_FRAMEBUTTON_VTABLE    0x0009dd24u /* vgui2::FrameButton — caption close/min/max, not Button vt */
#define RVA_FRAMESYSTEMBUTTON_VTABLE 0x0009e04cu /* FrameSystemButton (title Steam/menu) */
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
#define OFF_LABEL_SETIMAGEATINDEX_VT      0x25c
#define OFF_LABEL_TEXTIMAGE               0x78 /* TextImage* on Label; CheckButton keeps this */
#define LABEL_ALIGN_CENTER 4 /* a_northwest=0 ... a_west=3, a_center=4 */
#define LABEL_ALIGN_WEST   3
#define OFF_SETFGCOLOR_VT         0xD0 /* Button/Label::SetFgColor — also updates TextImage */
#define OFF_PAGETAB_ACTIVE        0x108
#define OFF_PAGETAB_ACTIVE_FG     0x109
#define COLOR_FG_WHITE            0xFFF7F5F5u /* r,g,b,a little-endian */
#define COLOR_BG_TRANSPARENT      0x00000000u
#define RVA_FRAME_PAINTBACKGROUND 0x0004cb60u /* Frame/PropertyDialog/MessageBox/COptionsDialog */
#define VT_PAINTBACKGROUND_INDEX  106 /* PerformLayout is 111; five slots earlier */
#define RVA_FRAME_TITLE_PLACE     0x0004cdb9u /* stock _title SetPos(0x1C,9) .. Paint */
#define RVA_FRAME_TITLE_CONT      0x0004cde8u /* epilogue after title Paint */
#define RVA_TEXTIMAGE_RESIZE      0x0004986fu /* TextImage::ResizeImageToContent */
#define RVA_TEXTIMAGE_SETDRAWWIDTH 0x00049c64u /* TextImage::SetDrawWidth */
#define RVA_CAREER_PAINTBACKGROUND 0x00002070u
#define RVA_PAINTBACKGROUND_18530 0x00018530u
#define RVA_PAINTBORDER           0x00043d40u
#define RVA_FRAME_PAINTBG_ALT     0x00023970u
#define RVA_PROGRESSBAR_PAINTBG   0x000696b0u /* vgui2::ProgressBar::PaintBackground — cube segments */
#define RVA_IMAGEPANEL_PAINTBG    0x00072740u /* ImagePanel::PaintBackground — TGA / scaleImage */
#define RVA_PROGRESSBAR_VTABLE    0x000a1dccu
#define RVA_SLIDER_VTABLE         0x000a194cu /* vgui2::Slider */
#define RVA_CCVARSLIDER_VTABLE    0x00097a3cu /* CCvarSlider : Slider */
#define RVA_SLIDER_PAINT          0x00066e00u /* Slider::Paint — 4px nob in SliderFgColor */
#define RVA_SLIDER_PAINTBG        0x000673d0u /* Slider::PaintBackground — Panel fill + groove + ticks */
#define RVA_SLIDER_RECOMPUTENOB   0x00066a80u /* Slider::RecomputeNobPosFromValue */
#define RVA_CCVARSLIDER_APPLY     0x00030450u /* CCvarSlider::ApplyChanges — this GameUI uses Cvar_SetValue */
#define RVA_ENGINE                0x000C3C9Cu
#define ENG_CLIENTCMD             20
#define OFF_SLIDER_NOB0           0x74
#define OFF_SLIDER_NOB1           0x78
#define OFF_SLIDER_DRAGGING       0x71
#define OFF_SLIDER_MIN            0x8C
#define OFF_SLIDER_MAX            0x90
#define OFF_SLIDER_VALUE          0x94
#define OFF_SLIDER_NOBSIZE        0xA4 /* float _nobSize; stock 8, our knob is 16 */
#define OFF_CCVAR_MODIFIED        0xB7
#define OFF_CCVAR_STARTF          0xB8
#define OFF_CCVAR_STARTI          0xBC
#define OFF_CCVAR_LASTI           0xC0
#define OFF_CCVAR_CURF            0xC4
#define OFF_CCVAR_NAME            0xC8
#define RVA_CROSSHAIRIMAGE_PAINT  0x0003b010u /* CrosshairImagePanel::Paint — engine FillRGBA, no VGUI clip */
#define RVA_TEXTENTRY_PAINTBG     0x0005b2a0u /* TextEntry::PaintBackground — fill + glyphs; ComboBox shares this */
#define RVA_COMBOBOX_VTABLE       0x0009fbbcu /* vgui2::ComboBox */
#define RVA_COMBOBOXBUTTON_VTABLE 0x0009f894u /* ComboBoxButton — Marlett arrow child */
#define RVA_CLABELEDCOMBO_VTABLE  0x000985fcu /* CLabeledCommandComboBox : ComboBox */
#define RVA_CCVARTEXTENTRY_VTABLE 0x00097c94u /* CCvarTextEntry — NameEntry */
#define RVA_TEXTENTRY_VTABLE      0x0009ff34u /* vgui2::TextEntry */
#define OFF_PROGRESS              0x78 /* float 0..1; confirmed via fmul [esi+0x78] in PaintBackground */

#define OFF_FRAME_TITLEIMAGE      0xBC /* TextImage* _title, painted in Frame::PaintBackground */
#define OFF_FRAME_CAPTION_BTNS    0xE8 /* first of five caption Button* (menu/min/max/tray/close) */
#define FRAME_CAPTION_BTN_COUNT   5
#define OFF_SETVISIBLE_VT         0x70 /* Panel::SetVisible(bool); IsVisible is 0x78 */
#define OFF_SETENABLED_VT         0xBC /* Panel::SetEnabled(bool); GameUI mic-test uses this */
#define OFF_ISENABLED_VT          0xC0
#define IIMAGE_VT_PAINT           0
#define IIMAGE_VT_SETPOS          1
#define IIMAGE_VT_GETCONTENTSIZE  2
#define IIMAGE_VT_GETSIZE         3
#define IIMAGE_VT_SETSIZE         4
#define OFF_PANEL_NAME   0x44
#define OFF_PANEL_BORDER 0x2C /* IBorder* loaded by Panel::PaintBorder */
#define SURF_VT_DRAWSETCOLOR       0x1C
#define SURF_VT_DRAWFILLEDRECT     0x24
#define SURF_VT_GETSCREENSIZE      0x80 /* ISurface::GetScreenSize(int&,int&) — CrosshairImagePanel::UpdateCrosshair */
#define SURF_VT_SETCURSOR          0xB4 /* ISurface::SetCursor; dc_none=1 dc_arrow=2 */
#define VGUI_DC_NONE               1
#define VGUI_DC_ARROW              2
#define SLIDER_KNOB_RGB            0xF5F5F7u
#define SLIDER_VALUE_INK_RGB       0x141416u
#define SLIDER_KNOB_DOT            6
#define SLIDER_VALUE_CAPSULE_W     44 /* fits "20.0" / "0.00" without resizing */
#define SLIDER_VALUE_CAPSULE_H     16

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
static PaintFn g_origImagePanelPaintBg = NULL;
static PaintFn g_origSliderPaint = NULL;
static PaintFn g_origSliderPaintBg = NULL;
static PaintFn g_origCvarSliderApply = NULL;
static PaintFn g_origCrosshairPaint = NULL;
static PaintFn g_origTextEntryPaintBg = NULL;
static BYTE g_panelPaintBgTramp[32];
static BYTE g_buttonPaintTramp[32];
static BYTE g_framePaintBgTramp[32];
static BYTE g_careerPaintBgTramp[32];
static BYTE g_paint18530Tramp[32];
static BYTE g_paintBorderTramp[32];
static BYTE g_framePaintBgAltTramp[32];
static BYTE g_progressPaintBgTramp[32];
static BYTE g_imagePanelPaintBgTramp[32];
static BYTE g_sliderPaintTramp[32];
static BYTE g_sliderPaintBgTramp[32];
static BYTE g_cvarSliderApplyTramp[32];
static BYTE g_crosshairPaintTramp[32];
static BYTE g_textEntryPaintBgTramp[32];
static BYTE g_titlePlaceTramp[32];

static SurfDrawSetColorFn g_origDrawSetColor = NULL;
static SurfDrawFilledRectFn g_origDrawFilledRect = NULL;
static SurfSetCursorFn g_origSetCursor = NULL;
static int g_surfaceHooked = 0;
static void *g_sliderCursorOwner = NULL;
static int g_sliderCursorClipped = 0;
static int g_sliderLockY = 0;
static int g_sliderDragAbandoned = 0;
static HWND g_sliderClipHwnd = NULL;

static volatile LONG g_roundDisabled = 0;
static volatile LONG g_inOurDraw = 0;
static int g_roundActive = 0;
static int g_roundW = 0;
static int g_roundH = 0;
static int g_roundR = 0;
static int g_roundIsButton = 0;
static int g_roundHot = 0;
static int g_fieldSkipFill = 0;
static unsigned int g_curColor = 0xE0101410u;
static OverlayTheme g_theme;
static void *g_dragValueLabel = NULL;
static int g_dragValueRestX = 0;
static int g_dragValueRestY = 0;
static int g_vuLiveW = 0;
static int g_vuLiveHold = 0;
static int g_voiceTrackW = 0;

#define VU_FACE_MAX_W 256
#define VU_FACE_MAX_H 128
static unsigned char g_vuFaceBgra[VU_FACE_MAX_W * VU_FACE_MAX_H * 4];
static int g_vuFaceW = 0;
static int g_vuFaceH = 0;
static int g_vuFaceLoaded = 0;
static int g_vuFaceTried = 0;

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
static void DrawAaDisk(int x0, int y0, int d, uint32_t rgb, uint32_t bgRgb);
static void DrawAaDiskOnTrack(int x0, int y0, int d, uint32_t rgb, int trackY, int trackH,
                              int splitX, uint32_t accentRgb, uint32_t trackRgb, uint32_t windowRgb);
static void DrawAaPillAt(int x0, int y0, int w, int h, uint32_t rgb, uint32_t bgRgb);
static void DrawAaRoundedFillAt(int x0, int y0, int w, int h, int r, uint32_t rgb, uint32_t bgRgb,
                                int roundTop, int roundBottom);
static void DrawPillAt(int x0, int y0, int w, int h, uint32_t rgb);
static int PillInset(int y, int h);
static void SurfaceFill(int x0, int y0, int x1, int y1, unsigned int packedRgba);
static int PanelIsEnabled(void *thisPtr);

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
    /* 1px on a r=12 corner is a stair. Floor at 2. */
    if (t < 2) {
        t = 2;
    }
    if (t > 4) {
        t = 4;
    }
    return t;
}

static unsigned int MixRgbPair(uint32_t rgb, uint32_t bgRgb, float a)
{
    int sr, sg, sb, br, bg, bb;
    int or_, og, ob;
    if (a <= 0.0f) {
        return ThemeRgbPacked(bgRgb);
    }
    if (a >= 1.0f) {
        return ThemeRgbPacked(rgb);
    }
    sr = (int)((rgb >> 16) & 0xFFu);
    sg = (int)((rgb >> 8) & 0xFFu);
    sb = (int)(rgb & 0xFFu);
    br = (int)((bgRgb >> 16) & 0xFFu);
    bg = (int)((bgRgb >> 8) & 0xFFu);
    bb = (int)(bgRgb & 0xFFu);
    or_ = (int)((float)sr * a + (float)br * (1.0f - a) + 0.5f);
    og = (int)((float)sg * a + (float)bg * (1.0f - a) + 0.5f);
    ob = (int)((float)sb * a + (float)bb * (1.0f - a) + 0.5f);
    return ThemeRgbPacked(((unsigned)or_ << 16) | ((unsigned)og << 8) | (unsigned)ob);
}

static unsigned int MixRgbToWindow(uint32_t rgb, float a)
{
    return MixRgbPair(rgb, g_theme.windowRgb, a);
}

/* Subpixel inset of a circular corner; same circle as the integer fill. */
static float CornerInsetF(int y, int h, int r)
{
    float rf;
    float py;
    float dy;
    float inside;
    if (r <= 0 || h < r * 2) {
        return 0.0f;
    }
    rf = (float)r;
    py = (float)y + 0.5f;
    if (py < rf) {
        dy = rf - py;
    } else if (py > (float)h - rf) {
        dy = py - ((float)h - rf);
    } else {
        return 0.0f;
    }
    inside = rf * rf - dy * dy;
    if (inside <= 0.0f) {
        return rf;
    }
    return rf - sqrtf(inside);
}

static void FillSpanSoftEnds(int y, float x0, float x1, uint32_t rgb, uint32_t bgL, uint32_t bgR)
{
    int s;
    int e;
    float a0;
    float a1;
    if (x1 - x0 < 0.02f) {
        return;
    }
    s = (int)x0;
    e = (int)x1;
    if (x0 < 0.0f) {
        s = (int)x0 - 1;
    }
    if (s == e) {
        SurfaceFill(s, y, s + 1, y + 1, MixRgbPair(rgb, bgL, x1 - x0));
        return;
    }
    a0 = (float)(s + 1) - x0;
    if (a0 < 0.0f) {
        a0 = 0.0f;
    }
    if (a0 > 1.0f) {
        a0 = 1.0f;
    }
    SurfaceFill(s, y, s + 1, y + 1, MixRgbPair(rgb, bgL, a0));
    if (e > s + 1) {
        SurfaceFill(s + 1, y, e, y + 1, ThemeRgbPacked(rgb));
    }
    a1 = x1 - (float)e;
    if (a1 > 0.02f) {
        if (a1 > 1.0f) {
            a1 = 1.0f;
        }
        SurfaceFill(e, y, e + 1, y + 1, MixRgbPair(rgb, bgR, a1));
    }
}

static void FillSpanSoft(int y, float x0, float x1, uint32_t rgb)
{
    FillSpanSoftEnds(y, x0, x1, rgb, g_theme.windowRgb, g_theme.windowRgb);
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

static int CapsuleRadius(int w, int h)
{
    int r = h / 2;
    if (w / 2 < r) {
        r = w / 2;
    }
    if (r < 2) {
        r = 2;
    }
    return r;
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
    if (lstrcmpiA(name, "Multiplayer") == 0 || lstrcmpiA(name, "Advanced") == 0
        || lstrcmpiA(name, "Keyboard") == 0
        || lstrcmpiA(name, "Mouse") == 0 || lstrcmpiA(name, "Audio") == 0
        || lstrcmpiA(name, "Video") == 0 || lstrcmpiA(name, "Voice") == 0
        || lstrcmpiA(name, "Lock") == 0) {
        return 1;
    }
    return 0;
}

static int NameContainsI(const char *hay, const char *needle);
static int NameIsMenuChrome(const char *name);

static int IsVguiFrame(void *thisPtr)
{
    void **vt;
    if (thisPtr == NULL || g_gameUiBase == NULL) {
        return 0;
    }
    vt = *(void ***)thisPtr;
    if (vt == NULL) {
        return 0;
    }
    /* QueryBox/MessageBox keep Frame::PaintBackground. Vtable still points
     * at the hooked RVA after InstallNearHook. */
    return vt[VT_PAINTBACKGROUND_INDEX] == (void *)(g_gameUiBase + RVA_FRAME_PAINTBACKGROUND);
}

static int IsOptionsInnerChrome(void *thisPtr)
{
    const char *name;
    int w = 0;
    int h = 0;
    if (thisPtr == NULL) {
        return 0;
    }
    name = PanelName(thisPtr);
    if (lstrcmpiA(name, "Sheet") == 0 || NameContainsI(name, "listpanel")) {
        return 1;
    }
    if (NameContainsI(name, "OptionsSub") || NameContainsI(name, "MultiplayerAdvanced")) {
        return 1;
    }
    /* Some pages are named like the tab ("Voice"), not OptionsSubVoice. */
    if (g_GetSize != NULL && NameIsOptionsTab(name)) {
        g_GetSize(thisPtr, &w, &h);
        if (h >= 100) {
            return 1;
        }
    }
    return 0;
}

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

static int IsComboBoxButton(void *thisPtr)
{
    void *vt;
    if (thisPtr == NULL || g_gameUiBase == NULL) {
        return 0;
    }
    vt = *(void **)thisPtr;
    return vt == (void *)(g_gameUiBase + RVA_COMBOBOXBUTTON_VTABLE);
}

static int IsComboField(void *thisPtr)
{
    void *vt;
    if (thisPtr == NULL || g_gameUiBase == NULL) {
        return 0;
    }
    vt = *(void **)thisPtr;
    return vt == (void *)(g_gameUiBase + RVA_COMBOBOX_VTABLE)
        || vt == (void *)(g_gameUiBase + RVA_CLABELEDCOMBO_VTABLE);
}

static int IsStyledTextField(void *thisPtr)
{
    void *vt;
    int w = 0, h = 0;
    if (thisPtr == NULL || g_gameUiBase == NULL || g_GetSize == NULL) {
        return 0;
    }
    vt = *(void **)thisPtr;
    if (vt != (void *)(g_gameUiBase + RVA_COMBOBOX_VTABLE)
        && vt != (void *)(g_gameUiBase + RVA_CLABELEDCOMBO_VTABLE)
        && vt != (void *)(g_gameUiBase + RVA_CCVARTEXTENTRY_VTABLE)
        && vt != (void *)(g_gameUiBase + RVA_TEXTENTRY_VTABLE)) {
        return 0;
    }
    g_GetSize(thisPtr, &w, &h);
    return w >= 32 && h >= 14 && h <= 48;
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

static int IsOptionsSlider(void *thisPtr)
{
    void *vt;
    if (thisPtr == NULL || g_gameUiBase == NULL) {
        return 0;
    }
    vt = *(void **)thisPtr;
    return vt == (void *)(g_gameUiBase + RVA_SLIDER_VTABLE)
        || vt == (void *)(g_gameUiBase + RVA_CCVARSLIDER_VTABLE);
}

static int IsCvarSlider(void *thisPtr)
{
    void *vt;
    if (thisPtr == NULL || g_gameUiBase == NULL) {
        return 0;
    }
    vt = *(void **)thisPtr;
    return vt == (void *)(g_gameUiBase + RVA_CCVARSLIDER_VTABLE);
}

static const char *CvarSliderCvarName(void *thisPtr)
{
    const char *cvar;
    if (thisPtr == NULL || IsBadReadPtr((char *)thisPtr + OFF_CCVAR_NAME, 2)) {
        return "";
    }
    cvar = (const char *)thisPtr + OFF_CCVAR_NAME;
    if (cvar[0] == '\0') {
        return "";
    }
    return cvar;
}

/* Only the Options float sliders Valve created in C++ (scale 100, "%.2f"
 * except mouse which the stock UI prints as "%.1f"). Color sliders on
 * Multiplayer and any factory leftover keep the original ApplyChanges. */
static int IsStockFloatCvarSlider(void *thisPtr)
{
    const char *cvar;
    if (!IsCvarSlider(thisPtr)) {
        return 0;
    }
    cvar = CvarSliderCvarName(thisPtr);
    return lstrcmpiA(cvar, "volume") == 0
        || lstrcmpiA(cvar, "mp3volume") == 0
        || lstrcmpiA(cvar, "suitvolume") == 0
        || lstrcmpiA(cvar, "al_doppler") == 0
        || lstrcmpiA(cvar, "brightness") == 0
        || lstrcmpiA(cvar, "gamma") == 0
        || lstrcmpiA(cvar, "sensitivity") == 0
        || lstrcmpiA(cvar, "voice_scale") == 0;
}

static int SliderStepFor(void *thisPtr)
{
    if (lstrcmpiA(CvarSliderCvarName(thisPtr), "sensitivity") == 0) {
        return 10;
    }
    return 1;
}

static void SnapCvarSlider(void *thisPtr)
{
    int minv;
    int maxv;
    int val;
    int step;
    int q;
    if (!IsStockFloatCvarSlider(thisPtr)
        || IsBadWritePtr((char *)thisPtr + OFF_SLIDER_VALUE, 4)) {
        return;
    }
    minv = *(int *)((char *)thisPtr + OFF_SLIDER_MIN);
    maxv = *(int *)((char *)thisPtr + OFF_SLIDER_MAX);
    val = *(int *)((char *)thisPtr + OFF_SLIDER_VALUE);
    step = SliderStepFor(thisPtr);
    if (step < 1) {
        step = 1;
    }
    q = val;
    if (q >= 0) {
        q = (q + step / 2) / step * step;
    } else {
        q = -(((-q) + step / 2) / step * step);
    }
    if (q < minv) {
        q = minv;
    }
    if (q > maxv) {
        q = maxv;
    }
    *(int *)((char *)thisPtr + OFF_SLIDER_VALUE) = q;
}

static void EngineClientCmd(const char *cmd)
{
    void **eng;
    typedef void (*ClientCmdFn)(const char *c);
    ClientCmdFn fn;
    if (g_gameUiBase == NULL || cmd == NULL || cmd[0] == '\0') {
        return;
    }
    if (IsBadReadPtr(g_gameUiBase + RVA_ENGINE, sizeof(void *))) {
        return;
    }
    eng = *(void ***)(g_gameUiBase + RVA_ENGINE);
    if (eng == NULL || IsBadReadPtr(eng, (ENG_CLIENTCMD + 1) * sizeof(void *))) {
        return;
    }
    fn = (ClientCmdFn)eng[ENG_CLIENTCMD];
    if (fn == NULL) {
        return;
    }
    __try {
        fn(cmd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void __fastcall CvarSliderApply_Hook(void *thisPtr)
{
    int ival;
    int step;
    float f;
    char num[32];
    char cmd[96];
    const char *cvar;
    if (!IsStockFloatCvarSlider(thisPtr)) {
        if (g_origCvarSliderApply != NULL) {
            g_origCvarSliderApply(thisPtr);
        }
        return;
    }
    if (thisPtr == NULL || IsBadReadPtr((char *)thisPtr + OFF_CCVAR_MODIFIED, 1)) {
        return;
    }
    if (*((unsigned char *)thisPtr + OFF_CCVAR_MODIFIED) == 0) {
        return;
    }
    SnapCvarSlider(thisPtr);
    /* Stock Apply writes the cvar immediately (Cvar_SetValue) and clears
     * the dirty flag. Skipping it left Apply armed and Paint read the old
     * cvar for one frame, so the knob jumped and needed a second click. */
    if (g_origCvarSliderApply != NULL) {
        g_origCvarSliderApply(thisPtr);
    }
    if (IsBadReadPtr((char *)thisPtr + OFF_SLIDER_VALUE, 4)) {
        return;
    }
    ival = *(int *)((char *)thisPtr + OFF_SLIDER_VALUE);
    f = (float)ival / 100.0f;
    cvar = CvarSliderCvarName(thisPtr);
    if (cvar[0] == '\0') {
        return;
    }
    step = SliderStepFor(thisPtr);
    if (step >= 10) {
        _snprintf(num, sizeof(num), "%.1f", f);
    } else {
        _snprintf(num, sizeof(num), "%.2f", f);
    }
    _snprintf(cmd, sizeof(cmd), "%s %s", cvar, num);
    EngineClientCmd(cmd);
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

static int IsFrameSystemButton(void *thisPtr)
{
    void *vt;
    if (thisPtr == NULL || g_gameUiBase == NULL) {
        return 0;
    }
    if (((unsigned)(size_t)thisPtr & 3u) != 0) {
        return 0;
    }
    if (IsBadReadPtr(thisPtr, sizeof(void *))) {
        return 0;
    }
    vt = *(void **)thisPtr;
    return vt == (void *)(g_gameUiBase + RVA_FRAMESYSTEMBUTTON_VTABLE);
}

static int FrameHasSysMenuButton(void *frame)
{
    int i;
    if (frame == NULL || g_gameUiBase == NULL) {
        return 0;
    }
    for (i = 0; i < FRAME_CAPTION_BTN_COUNT; i++) {
        void *btn = *(void **)((char *)frame + OFF_FRAME_CAPTION_BTNS + i * 4);
        if (IsFrameSystemButton(btn)) {
            return 1;
        }
    }
    return 0;
}

static void HideFrameSystemButtons(void *frame)
{
    int i;
    if (frame == NULL || g_SetPos == NULL || !FrameHasSysMenuButton(frame)) {
        return;
    }
    for (i = 0; i < FRAME_CAPTION_BTN_COUNT; i++) {
        void *btn = *(void **)((char *)frame + OFF_FRAME_CAPTION_BTNS + i * 4);
        if (!IsFrameSystemButton(btn)) {
            continue;
        }
        g_SetPos(btn, -4000, -4000);
        if (g_SetSize != NULL) {
            g_SetSize(btn, 1, 1);
        }
    }
}

/* Replaces stock SetPos(28,9)+SetSize(wide-72)+Paint so the caption
 * is drawn once, centered. GetSize returns the stretched draw box;
 * GetContentSize / ResizeImageToContent is the glyph width. */
static void __cdecl PlaceFrameTitle(void *frame)
{
    void *title;
    void **imageVt;
    IImageGetSizeFn getContent;
    IImageGetSizeFn getSize;
    IImageSetPosFn setPos;
    IImageSetSizeFn setSize;
    IImagePaintFn paint;
    ResizeToContentFn resize;
    SetDrawWidthFn setDrawWidth;
    int fw = 0, fh = 0, tw = 0, th = 0;
    int x;
    int closePad = 22;
    BYTE *fn;

    if (frame == NULL || g_gameUiBase == NULL || g_GetSize == NULL) {
        return;
    }
    title = *(void **)((char *)frame + OFF_FRAME_TITLEIMAGE);
    if (title == NULL) {
        return;
    }
    imageVt = *(void ***)title;
    if (imageVt == NULL) {
        return;
    }
    fn = (BYTE *)imageVt[IIMAGE_VT_PAINT];
    if (fn == NULL || fn < g_gameUiBase || fn > g_gameUiBase + 0x000B0000u) {
        return;
    }
    if (fn == g_gameUiBase + 0x00002860u) {
        return;
    }
    getContent = (IImageGetSizeFn)imageVt[IIMAGE_VT_GETCONTENTSIZE];
    getSize = (IImageGetSizeFn)imageVt[IIMAGE_VT_GETSIZE];
    setPos = (IImageSetPosFn)imageVt[IIMAGE_VT_SETPOS];
    setSize = (IImageSetSizeFn)imageVt[IIMAGE_VT_SETSIZE];
    paint = (IImagePaintFn)imageVt[IIMAGE_VT_PAINT];
    resize = (ResizeToContentFn)(g_gameUiBase + RVA_TEXTIMAGE_RESIZE);
    setDrawWidth = (SetDrawWidthFn)(g_gameUiBase + RVA_TEXTIMAGE_SETDRAWWIDTH);
    if (setPos == NULL || paint == NULL) {
        return;
    }
    g_GetSize(frame, &fw, &fh);
    __try {
        /* PropertyDialog titles wrap to (frame-72), so GetContentSize is the
         * layout box and the glyphs stay left. Captions are one line. */
        *((unsigned char *)title + 0x34) &= (unsigned char)~1u;
        setDrawWidth(title, 0);
        resize(title);
        tw = 0;
        th = 0;
        if (getContent != NULL) {
            getContent(title, &tw, &th);
        }
        if ((tw <= 4 || th < 8 || (fw >= 80 && tw > fw / 3)) && getSize != NULL) {
            int sw = 0, sh = 0;
            getSize(title, &sw, &sh);
            if (tw <= 4 || (sw > 0 && sw < tw)) {
                tw = sw;
                th = sh;
            }
        }
        if (tw <= 4 || th < 8 || th > 32) {
            setPos(title, 0x1C, 9);
            paint(title);
            return;
        }
        if (fw >= 80 && tw > fw / 3) {
            /* Still a stretched box: keep left-aligned stock pos. */
            setPos(title, 0x1C, 9);
            paint(title);
            return;
        }
        x = 0x1C;
        if (fw >= 80) {
            x = (fw - tw) / 2;
            if (x < 8) {
                x = 8;
            }
            if (x + tw > fw - closePad) {
                x = fw - closePad - tw;
            }
        }
        setPos(title, x, 9);
        if (setSize != NULL) {
            setSize(title, tw, th);
        }
        paint(title);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
}

static void InstallTitlePlaceHook(BYTE *base)
{
    BYTE *target = base + RVA_FRAME_TITLE_PLACE;
    BYTE *cont = base + RVA_FRAME_TITLE_CONT;
    unsigned stolen = (unsigned)(cont - target);
    DWORD oldProtect;
    DWORD trampProtect;
    INT32 relCall;
    INT32 relJmp;
    INT32 relHook;
    unsigned i;
    static const BYTE expected[10] = {
        0x8B, 0x8E, 0xBC, 0x00, 0x00, 0x00, 0x6A, 0x09, 0x6A, 0x1C
    };

    if (stolen < 16 || stolen > 64) {
        return;
    }
    if (memcmp(target, expected, sizeof(expected)) != 0) {
        HookLog("RoundFrame: title place mismatch at %p, skip", (void *)target);
        return;
    }

    g_titlePlaceTramp[0] = 0x56; /* push esi  (frame) */
    g_titlePlaceTramp[1] = 0xE8;
    relCall = (INT32)((BYTE *)PlaceFrameTitle - (g_titlePlaceTramp + 6));
    memcpy(g_titlePlaceTramp + 2, &relCall, sizeof(relCall));
    g_titlePlaceTramp[6] = 0x83;
    g_titlePlaceTramp[7] = 0xC4;
    g_titlePlaceTramp[8] = 0x04;
    g_titlePlaceTramp[9] = 0xE9;
    relJmp = (INT32)(cont - (g_titlePlaceTramp + 14));
    memcpy(g_titlePlaceTramp + 10, &relJmp, sizeof(relJmp));

    if (!VirtualProtect(g_titlePlaceTramp, sizeof(g_titlePlaceTramp),
                        PAGE_EXECUTE_READWRITE, &trampProtect)) {
        return;
    }
    if (!VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }
    relHook = (INT32)(g_titlePlaceTramp - (target + 5));
    target[0] = 0xE9;
    memcpy(target + 1, &relHook, sizeof(relHook));
    for (i = 5; i < stolen; i++) {
        target[i] = 0x90;
    }
    VirtualProtect(target, stolen, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    FlushInstructionCache(GetCurrentProcess(), g_titlePlaceTramp, sizeof(g_titlePlaceTramp));
    HookLog("RoundFrame: title place hooked %p", (void *)target);
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
    uint32_t rgb;
    if (g_GetSize == NULL) {
        return;
    }
    g_GetSize(thisPtr, &w, &h);
    if (w < 8 || h < 8) {
        return;
    }
    EnsureSurfaceHooks();
    r = CapsuleRadius(w, h);
    rgb = ControlIsHot(thisPtr) ? g_theme.accentRgb : g_theme.trackRgb;
    DrawAaRoundedFillAt(0, 0, w, h, r, rgb, g_theme.windowRgb, 1, 1);
}

static void PaintFieldPlate(void *thisPtr)
{
    int w = 0, h = 0;
    int r;
    uint32_t rgb;
    if (g_GetSize == NULL) {
        return;
    }
    g_GetSize(thisPtr, &w, &h);
    if (w < 8 || h < 8) {
        return;
    }
    EnsureSurfaceHooks();
    r = CapsuleRadius(w, h);
    if (!PanelIsEnabled(thisPtr)) {
        rgb = g_theme.mutedRgb;
    } else {
        rgb = g_theme.trackRgb;
    }
    DrawAaRoundedFillAt(0, 0, w, h, r, rgb, g_theme.windowRgb, 1, 1);
}

static int IsMouseToggleName(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    return lstrcmpiA(name, "ReverseMouse") == 0
        || lstrcmpiA(name, "MouseLook") == 0
        || lstrcmpiA(name, "MouseFilter") == 0
        || lstrcmpiA(name, "Joystick") == 0
        || lstrcmpiA(name, "JoystickLook") == 0
        || lstrcmpiA(name, "Auto-Aim") == 0
        || lstrcmpiA(name, "RawInput") == 0;
}

static int IsVideoToggleName(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    return lstrcmpiA(name, "Windowed") == 0
        || lstrcmpiA(name, "VSync") == 0
        || lstrcmpiA(name, "HDModels") == 0
        || lstrcmpiA(name, "AddonsFolder") == 0
        || lstrcmpiA(name, "LowVideoDetail") == 0
        || lstrcmpiA(name, "DetailTextures") == 0;
}

static int IsVoiceToggleName(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    return lstrcmpiA(name, "voice_modenable") == 0
        || lstrcmpiA(name, "MicBoost") == 0;
}

static int IsAudioToggleName(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    return lstrcmpiA(name, "hisound") == 0
        || lstrcmpiA(name, "al_occlusion") == 0
        || lstrcmpiA(name, "al_occlusion_fade") == 0
        || lstrcmpiA(name, "al_resample_all") == 0
        || lstrcmpiA(name, "al_xfi_workaround") == 0
        || lstrcmpiA(name, "al_clamping_mode") == 0;
}

static int IsSettingsToggle(void *thisPtr)
{
    return lstrcmpiA(PanelName(thisPtr), "CrosshairTranslucencyCheckbox") == 0;
}

static int IsCvarToggleRow(void *thisPtr)
{
    void *vt;
    int w = 0, h = 0;
    const char *name;
    if (thisPtr == NULL || g_gameUiBase == NULL || g_GetSize == NULL) {
        return 0;
    }
    vt = *(void **)thisPtr;
    name = PanelName(thisPtr);
    if (vt != (void *)(g_gameUiBase + RVA_CCVARTOGGLE_VTABLE)
        && vt != (void *)(g_gameUiBase + RVA_DESCCHECKBUTTON_VTABLE)
        && lstrcmpiA(name, "DescCheckButton") != 0
        && !IsMouseToggleName(name)
        && !IsVideoToggleName(name)
        && !IsVoiceToggleName(name)
        && !IsAudioToggleName(name)
        && !IsSettingsToggle(thisPtr)) {
        return 0;
    }
    g_GetSize(thisPtr, &w, &h);
    return w >= 20 && h >= 12 && h <= 56;
}

static int IsAdvancedSettingsRow(void *thisPtr)
{
    int w = 0, h = 0;
    if (thisPtr == NULL || g_GetSize == NULL) {
        return 0;
    }
    if (lstrcmpiA(PanelName(thisPtr), "Advanced") != 0) {
        return 0;
    }
    g_GetSize(thisPtr, &w, &h);
    return w >= 200 && h >= 28;
}

static void DrawToggleSwitch(void *thisPtr)
{
    int w = 0, h = 0;
    int on;
    int trackH;
    int trackW;
    int x;
    int y;
    int knob;
    uint32_t trackRgb;
    if (g_GetSize == NULL) {
        return;
    }
    g_GetSize(thisPtr, &w, &h);
    if (w < 20 || h < 12) {
        return;
    }
    EnsureSurfaceHooks();
    on = VtableFlag(thisPtr, OFF_BUTTON_ISSELECTED_VT);
    trackH = OPTIONS_TOGGLE_TRACK_H;
    trackW = OPTIONS_TOGGLE_TRACK_W;
    knob = OPTIONS_TOGGLE_KNOB;
    x = w - trackW;
    if (x < 0) {
        x = 0;
    }
    y = (h - trackH) / 2;
    if (y < 0) {
        y = 0;
    }
    trackRgb = (!PanelIsEnabled(thisPtr) || !on) ? g_theme.trackRgb : g_theme.accentRgb;
    if (!PanelIsEnabled(thisPtr) && on) {
        trackRgb = g_theme.mutedRgb;
    }
    DrawAaPillAt(x, y, trackW, trackH, trackRgb, g_theme.windowRgb);
    /* Knob stays the light disk. Using trackRgb here made AA mix into the
     * window color and left a dotted halo. */
    DrawAaDiskOnTrack(on ? (x + trackW - knob - 3) : (x + 3), y + (trackH - knob) / 2, knob,
                      SLIDER_KNOB_RGB, y, trackH, on ? (x + trackW) : x,
                      trackRgb, g_theme.trackRgb, g_theme.windowRgb);
}

/* Same SurfaceFill path as the track — no ISurface text (that crashed).
 * 3x5 glyphs, 2px cells. */
static const unsigned char kDigit5[10][5] = {
    { 0x7, 0x5, 0x5, 0x5, 0x7 },
    { 0x2, 0x6, 0x2, 0x2, 0x7 },
    { 0x7, 0x1, 0x7, 0x4, 0x7 },
    { 0x7, 0x1, 0x7, 0x1, 0x7 },
    { 0x5, 0x5, 0x7, 0x1, 0x1 },
    { 0x7, 0x4, 0x7, 0x1, 0x7 },
    { 0x7, 0x4, 0x7, 0x5, 0x7 },
    { 0x7, 0x1, 0x1, 0x1, 0x1 },
    { 0x7, 0x5, 0x7, 0x5, 0x7 },
    { 0x7, 0x5, 0x7, 0x1, 0x7 }
};

static void DrawGlyphRow(int x, int y, unsigned char bits, unsigned int packed)
{
    int col;
    for (col = 0; col < 3; col++) {
        if ((bits & (1 << (2 - col))) != 0) {
            SurfaceFill(x + col * 2, y, x + col * 2 + 2, y + 2, packed);
        }
    }
}

static int DrawValueGlyphs(int x, int y, const char *text, unsigned int packed)
{
    int cx = x;
    int i;
    for (i = 0; text[i] != '\0'; i++) {
        if (text[i] == '.') {
            SurfaceFill(cx + 1, y + 8, cx + 3, y + 10, packed);
            cx += 4;
        } else if (text[i] >= '0' && text[i] <= '9') {
            int row;
            const unsigned char *g = kDigit5[text[i] - '0'];
            for (row = 0; row < 5; row++) {
                DrawGlyphRow(cx, y + row * 2, g[row], packed);
            }
            cx += 8;
        }
    }
    return cx - x;
}

static void SurfaceApplyCursor(unsigned long cursor)
{
    void *surf;
    if (g_GetSurface == NULL) {
        return;
    }
    surf = g_GetSurface();
    if (surf == NULL || IsBadReadPtr(surf, sizeof(void *))) {
        return;
    }
    if (g_origSetCursor != NULL) {
        g_origSetCursor(surf, cursor);
        return;
    }
    {
        void **vt = *(void ***)surf;
        SurfSetCursorFn setCursor;
        if (vt == NULL) {
            return;
        }
        setCursor = (SurfSetCursorFn)vt[SURF_VT_SETCURSOR / sizeof(void *)];
        if (setCursor != NULL) {
            setCursor(surf, cursor);
        }
    }
}

static void ReleaseSliderCursorClip(void)
{
    if (g_sliderCursorClipped) {
        ClipCursor(NULL);
        g_sliderCursorClipped = 0;
    }
}

static int SliderDragLostFocus(void)
{
    HWND fg;
    if (g_sliderClipHwnd == NULL) {
        return 0;
    }
    fg = GetForegroundWindow();
    if (fg != g_sliderClipHwnd) {
        return 1;
    }
    if (IsIconic(g_sliderClipHwnd) || !IsWindowVisible(g_sliderClipHwnd)) {
        return 1;
    }
    return 0;
}

static void AbandonSliderCursorLock(void)
{
    g_sliderDragAbandoned = 1;
    g_sliderCursorOwner = NULL;
    g_sliderClipHwnd = NULL;
    ReleaseSliderCursorClip();
    SurfaceApplyCursor(VGUI_DC_ARROW);
}

static void UpdateSliderDragCursor(void *slider, int trackX0, int trackX1, int knobX)
{
    int dragging;
    POINT pt;
    RECT clip;
    (void)trackX0;
    (void)trackX1;
    (void)knobX;
    if (slider == NULL || IsBadReadPtr((char *)slider + OFF_SLIDER_DRAGGING, 1)) {
        return;
    }
    dragging = *((unsigned char *)slider + OFF_SLIDER_DRAGGING) != 0;
    if (!dragging) {
        if (g_sliderCursorOwner == slider) {
            g_sliderCursorOwner = NULL;
            g_sliderClipHwnd = NULL;
            g_sliderDragAbandoned = 0;
            ReleaseSliderCursorClip();
            SurfaceApplyCursor(VGUI_DC_ARROW);
        }
        return;
    }
    if (g_sliderDragAbandoned) {
        ReleaseSliderCursorClip();
        return;
    }
    if (g_sliderCursorOwner == slider && SliderDragLostFocus()) {
        AbandonSliderCursorLock();
        return;
    }
    if (!GetCursorPos(&pt)) {
        g_sliderCursorOwner = slider;
        SurfaceApplyCursor(VGUI_DC_NONE);
        return;
    }
    if (g_sliderCursorOwner != slider) {
        g_sliderLockY = pt.y;
        g_sliderClipHwnd = GetForegroundWindow();
        g_sliderCursorOwner = slider;
        g_sliderDragAbandoned = 0;
    }
    if (pt.y != g_sliderLockY) {
        SetCursorPos(pt.x, g_sliderLockY);
    }
    clip.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    clip.top = g_sliderLockY;
    clip.right = clip.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    clip.bottom = g_sliderLockY + 1;
    if (clip.right <= clip.left) {
        clip.left = 0;
        clip.right = GetSystemMetrics(SM_CXSCREEN);
    }
    ClipCursor(&clip);
    g_sliderCursorClipped = 1;
    SurfaceApplyCursor(VGUI_DC_NONE);
}

static int FormatSliderDragText(void *thisPtr, char *buf, int bufSize)
{
    int minv;
    int maxv;
    int val;
    float f;
    if (thisPtr == NULL || buf == NULL || bufSize < 4) {
        return 0;
    }
    minv = *(int *)((char *)thisPtr + OFF_SLIDER_MIN);
    maxv = *(int *)((char *)thisPtr + OFF_SLIDER_MAX);
    val = *(int *)((char *)thisPtr + OFF_SLIDER_VALUE);
    if (IsCvarSlider(thisPtr)) {
        f = (float)val / 100.0f;
        if (SliderStepFor(thisPtr) >= 10) {
            _snprintf(buf, bufSize, "%.1f", f);
        } else {
            _snprintf(buf, bufSize, "%.2f", f);
        }
    } else if (minv == 0 && maxv == 100) {
        _snprintf(buf, bufSize, "%.2f", (float)val / 100.0f);
    } else {
        _snprintf(buf, bufSize, "%d", val);
    }
    buf[bufSize - 1] = '\0';
    return 1;
}

static int GlyphTextWidth(const char *text)
{
    int tw = 0;
    int i;
    if (text == NULL) {
        return 0;
    }
    for (i = 0; text[i] != '\0'; i++) {
        tw += (text[i] == '.') ? 4 : 8;
    }
    return tw;
}

static int SliderIsDragging(void *thisPtr)
{
    if (thisPtr == NULL || IsBadReadPtr((char *)thisPtr + OFF_SLIDER_DRAGGING, 1)) {
        return 0;
    }
    return *((unsigned char *)thisPtr + OFF_SLIDER_DRAGGING) != 0;
}

static int PanelIsEnabled(void *thisPtr)
{
    void **vtable;
    ByteGetterFn isEnabled;
    char on;

    if (thisPtr == NULL) {
        return 1;
    }
    vtable = *(void ***)thisPtr;
    if (vtable == NULL) {
        return 1;
    }
    isEnabled = (ByteGetterFn)vtable[OFF_ISENABLED_VT / sizeof(void *)];
    if (isEnabled == NULL) {
        return 1;
    }
    on = 1;
    __try {
        on = isEnabled(thisPtr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        on = 1;
    }
    return on != 0;
}

static void DrawValueSlider(void *thisPtr)
{
    int w = 0;
    int h = 0;
    int n0;
    int n1;
    int minv;
    int maxv;
    int val;
    int trackH;
    int knob;
    int padX;
    int trackY;
    int cx;
    int row;
    int knobX;
    int knobY;
    int dragging;
    char buf[16];
    int tw;
    int capW;
    int capH;

    if (g_GetSize == NULL || thisPtr == NULL) {
        return;
    }
    if (!IsBadWritePtr((char *)thisPtr + OFF_SLIDER_NOBSIZE, 4)) {
        *(float *)((char *)thisPtr + OFF_SLIDER_NOBSIZE) = 16.0f;
    }
    g_GetSize(thisPtr, &w, &h);
    if (w < 32 || h < 12) {
        return;
    }
    EnsureSurfaceHooks();
    {
        typedef void(__thiscall *RecomputeFn)(void *self);
        ((RecomputeFn)(g_gameUiBase + RVA_SLIDER_RECOMPUTENOB))(thisPtr);
    }
    n0 = *(int *)((char *)thisPtr + OFF_SLIDER_NOB0);
    n1 = *(int *)((char *)thisPtr + OFF_SLIDER_NOB1);
    minv = *(int *)((char *)thisPtr + OFF_SLIDER_MIN);
    maxv = *(int *)((char *)thisPtr + OFF_SLIDER_MAX);
    val = *(int *)((char *)thisPtr + OFF_SLIDER_VALUE);
    if (n1 < n0) {
        int tmp = n0;
        n0 = n1;
        n1 = tmp;
    }
    trackH = 10;
    knob = 16;
    if (trackH > h - 4) {
        trackH = (h - 4) & ~1;
    }
    if (trackH < 8) {
        trackH = h < 10 ? (h & ~1) : 8;
    }
    if (knob > h - 2) {
        knob = h - 2;
        if (knob < trackH) {
            knob = trackH;
        }
    }
    if ((knob & 1) != 0) {
        knob -= 1;
    }
    dragging = SliderIsDragging(thisPtr);
    buf[0] = '\0';
    tw = 0;
    capW = SLIDER_VALUE_CAPSULE_W;
    capH = SLIDER_VALUE_CAPSULE_H;
    {
        int enabled = PanelIsEnabled(thisPtr);
        uint32_t fillRgb = enabled ? g_theme.accentRgb : g_theme.mutedRgb;
        uint32_t knobRgb = SLIDER_KNOB_RGB;
        uint32_t dotRgb = enabled ? g_theme.accentRgb : g_theme.mutedRgb;

        if (dragging && enabled) {
            FormatSliderDragText(thisPtr, buf, (int)sizeof(buf));
            tw = GlyphTextWidth(buf);
        }
        padX = knob / 2;
        if (padX < 8) {
            padX = 8;
        }
        if (w - padX * 2 < 16) {
            padX = 4;
        }
        trackY = (h - trackH) / 2;
        if (trackY < 0) {
            trackY = 0;
        }
        if (trackY + ((knob > trackH) ? knob : trackH) > h) {
            trackY = h - ((knob > trackH) ? knob : trackH);
            if (trackY < 0) {
                trackY = 0;
            }
        }
        cx = (n0 + n1) / 2;
        if (cx < padX || cx > w - padX || (n0 == 0 && n1 == 0)) {
            float t = 0.0f;
            if (maxv > minv) {
                t = (float)(val - minv) / (float)(maxv - minv);
            }
            if (t < 0.0f) {
                t = 0.0f;
            }
            if (t > 1.0f) {
                t = 1.0f;
            }
            cx = padX + (int)(t * (float)(w - padX * 2) + 0.5f);
        }
        if (cx < padX) {
            cx = padX;
        }
        if (cx > w - padX) {
            cx = w - padX;
        }
        DrawPillAt(padX, trackY, w - padX * 2, trackH, g_theme.trackRgb);
        if (cx > padX) {
            int tr = trackH / 2;
            for (row = 0; row < trackH; row++) {
                float inset = CornerInsetF(row, trackH, tr);
                FillSpanSoftEnds(trackY + row, (float)padX + inset, (float)cx,
                                 fillRgb, g_theme.windowRgb, fillRgb);
            }
        }
        if (dragging && enabled) {
            knobX = cx - capW / 2;
            knobY = trackY + (trackH - capH) / 2;
            if (knobX < 0) {
                knobX = 0;
            }
            if (knobX + capW > w) {
                knobX = w - capW;
                if (knobX < 0) {
                    knobX = 0;
                }
            }
            if (knobY < 0) {
                knobY = 0;
            }
            if (knobY + capH > h) {
                knobY = h - capH;
                if (knobY < 0) {
                    knobY = 0;
                }
            }
            {
                int cr = capH / 2;
                for (row = 0; row < capH; row++) {
                    float inset = CornerInsetF(row, capH, cr);
                    int py = knobY + row;
                    uint32_t bgL = g_theme.windowRgb;
                    uint32_t bgR = g_theme.windowRgb;
                    if (py >= trackY && py < trackY + trackH) {
                        bgL = fillRgb;
                        bgR = g_theme.trackRgb;
                    }
                    FillSpanSoftEnds(py, (float)knobX + inset, (float)(knobX + capW) - inset,
                                     fillRgb, bgL, bgR);
                }
            }
            if (buf[0] != '\0') {
                int tx = knobX + (capW - tw) / 2;
                int ty = knobY + (capH - 10) / 2;
                DrawValueGlyphs(tx, ty, buf, ThemeRgbPacked(SLIDER_KNOB_RGB));
            }
        } else {
            knobX = cx - knob / 2;
            knobY = trackY + (trackH - knob) / 2;
            if (knobX < 0) {
                knobX = 0;
            }
            if (knobY < 0) {
                knobY = 0;
            }
            DrawAaDiskOnTrack(knobX, knobY, knob, knobRgb, trackY, trackH, cx,
                              fillRgb, g_theme.trackRgb, g_theme.windowRgb);
            DrawAaDisk(knobX + (knob - SLIDER_KNOB_DOT) / 2,
                       knobY + (knob - SLIDER_KNOB_DOT) / 2,
                       SLIDER_KNOB_DOT, dotRgb, knobRgb);
        }
        if (enabled) {
            UpdateSliderDragCursor(thisPtr, padX, w - padX, cx);
        }
    }
}

void RoundFrame_SetDragValueLabel(void *label, int restX, int restY)
{
    g_dragValueLabel = label;
    g_dragValueRestX = restX;
    g_dragValueRestY = restY;
}

static void PaintCvarToggleRow(void *thisPtr)
{
    void **vtable;
    SetIntFn setAlign;
    SetTextInsetFn setInset;
    SetImageAtIndexFn setImage;
    int w = 0, h = 0;
    void *textImg;

    if (IsAudioToggleName(PanelName(thisPtr))
        || lstrcmpiA(PanelName(thisPtr), "MicBoost") == 0) {
        AudioExtra_SyncToggle(thisPtr);
    }

    vtable = *(void ***)thisPtr;
    setAlign = (SetIntFn)vtable[OFF_BUTTON_SETCONTENTALIGNMENT_VT / sizeof(void *)];
    setInset = (SetTextInsetFn)vtable[OFF_BUTTON_SETTEXTINSET_VT / sizeof(void *)];
    setImage = (SetImageAtIndexFn)vtable[OFF_LABEL_SETIMAGEATINDEX_VT / sizeof(void *)];
    g_GetSize(thisPtr, &w, &h);
    if (w < 80) {
        DrawToggleSwitch(thisPtr);
        return;
    }
    if (setImage != NULL) {
        /* Index 0 is the check bitmap. Index 1 is the label — do not clear it. */
        setImage(thisPtr, 0, NULL, 0);
    }
    if (setInset != NULL) {
        setInset(thisPtr, 0, 0);
    }
    if (setAlign != NULL) {
        setAlign(thisPtr, LABEL_ALIGN_WEST);
    }
    ForceWhiteOnTransparent(thisPtr);
    SetFgColorWhite(thisPtr);
    if (!IsBadReadPtr((char *)thisPtr + OFF_LABEL_TEXTIMAGE, sizeof(void *))) {
        textImg = *(void **)((char *)thisPtr + OFF_LABEL_TEXTIMAGE);
        if (textImg != NULL && g_gameUiBase != NULL && w > 56) {
            SetDrawWidthFn setDrawWidth =
                (SetDrawWidthFn)(g_gameUiBase + RVA_TEXTIMAGE_SETDRAWWIDTH);
            if (IsMouseToggleName(PanelName(thisPtr))) {
                setDrawWidth(textImg, 150);
            }
        }
    }
    if (g_origButtonPaint != NULL) {
        g_origButtonPaint(thisPtr);
    }
    EnsureSurfaceHooks();
    if (w >= 80) {
        const int switchW = OPTIONS_TOGGLE_TRACK_W;
        const int gap = OPTIONS_TOGGLE_GAP;
        int coverL = w - switchW - gap;
        if (coverL < 0) {
            coverL = 0;
        }
        SurfaceFill(coverL, 0, w - switchW, h, ThemeRgbPacked(g_theme.windowRgb));
    }
    DrawToggleSwitch(thisPtr);
}

static void DrawRowChevron(int w, int h)
{
    int cx;
    int cy;
    int i;
    unsigned int col;
    if (w < 24 || h < 12) {
        return;
    }
    EnsureSurfaceHooks();
    col = ThemeRgbPacked(g_theme.mutedRgb);
    cx = w - 18;
    cy = h / 2;
    for (i = 0; i < 5; i++) {
        SurfaceFill(cx + i, cy - 4 + i, cx + i + 2, cy - 3 + i, col);
        SurfaceFill(cx + i, cy + 3 - i, cx + i + 2, cy + 4 - i, col);
    }
}

static void DrawComboCaret(int w, int h)
{
    int cx;
    int cy;
    int i;
    unsigned int col;
    if (w < 24 || h < 12) {
        return;
    }
    EnsureSurfaceHooks();
    col = ThemeRgbPacked(g_theme.mutedRgb);
    cx = w - 14;
    cy = h / 2;
    for (i = 0; i < 5; i++) {
        SurfaceFill(cx - 4 + i, cy - 2 + i, cx + 5 - i, cy - 1 + i, col);
    }
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
    if (lstrcmpiA(name, "Advanced") == 0) {
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
    /* Options PropertySheet is already inside the dialog chrome; rounding it
     * plus the active page draws two nested boxes. Same for list panels
     * sitting inside a rounded page (Advanced). */
    if (lstrcmpiA(name, "Sheet") == 0 || NameContainsI(name, "listpanel")) {
        return 0;
    }
    if (IsOptionsInnerChrome(thisPtr)) {
        return 0;
    }
    /* Nested settings pages are Panels, not Frames. Rounding them draws
     * the inner Voice/Mouse frame. Frames stay rounded even when unnamed:
     * QueryBox is Frame(parent, NULL) so PanelName is empty. */
    if (!IsVguiFrame(thisPtr)
        && !NameContainsI(name, "Dialog") && !NameContainsI(name, "MessageBox")
        && !NameContainsI(name, "QueryBox") && lstrcmpiA(name, "BaseQuestionPanel") != 0) {
        if (w >= 160 && h >= 100) {
            return 0;
        }
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
    int innerRad;
    uint32_t rgb;
    (void)packedRgba;
    if (w <= 0 || h <= 0) {
        return;
    }
    if (thickness < 2) {
        thickness = 2;
    }
    rgb = g_theme.borderRgb;
    topR = roundTop ? r : 0;
    botR = roundBottom ? r : 0;
    if (topR * 2 + 4 > w) {
        topR = ClampInt(w / 4, 0, topR);
    }
    if (botR * 2 + 4 > w) {
        botR = ClampInt(w / 4, 0, botR);
    }
    innerRad = r - thickness;
    if (innerRad < 0) {
        innerRad = 0;
    }
    if (!roundTop && !roundBottom) {
        innerRad = 0;
    }
    for (y = 0; y < h; y++) {
        int cr = 0;
        float inset;
        float left;
        float right;
        if (y < topR) {
            cr = topR;
        } else if (y >= h - botR) {
            cr = botR;
        }
        inset = cr > 0 ? CornerInsetF(y, h, cr) : 0.0f;
        left = (float)x0 + inset;
        right = (float)(x0 + w) - inset;
        if (right - left < 0.5f) {
            continue;
        }
        if (y < thickness || y >= h - thickness) {
            FillSpanSoft(y0 + y, left, right, rgb);
        } else {
            float innerInset = 0.0f;
            float innerL;
            float innerRgt;
            int iy = y - thickness;
            int ih = h - 2 * thickness;
            if (ih > 0 && innerRad > 0) {
                if (iy < innerRad || iy >= ih - innerRad) {
                    innerInset = CornerInsetF(iy, ih, innerRad);
                }
            }
            innerL = (float)(x0 + thickness) + innerInset;
            innerRgt = (float)(x0 + w - thickness) - innerInset;
            if (innerL > left) {
                FillSpanSoft(y0 + y, left, innerL, rgb);
            }
            if (right > innerRgt) {
                FillSpanSoft(y0 + y, innerRgt, right, rgb);
            }
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

static void DrawPillAt(int x0, int y0, int w, int h, uint32_t rgb)
{
    int y;
    int r;
    if (w <= 0 || h <= 0) {
        return;
    }
    if ((h & 1) != 0) {
        h -= 1;
    }
    if (h < 2) {
        SurfaceFill(x0, y0, x0 + w, y0 + h, ThemeRgbPacked(rgb));
        return;
    }
    r = h / 2;
    for (y = 0; y < h; y++) {
        float inset = CornerInsetF(y, h, r);
        FillSpanSoft(y0 + y, (float)x0 + inset, (float)(x0 + w) - inset, rgb);
    }
}

static void __fastcall SetCursor_Hook(void *surf, void *edx, unsigned long cursor)
{
    (void)edx;
    if (g_sliderCursorOwner != NULL) {
        cursor = VGUI_DC_NONE;
    }
    if (g_origSetCursor != NULL) {
        g_origSetCursor(surf, cursor);
    }
}

static void HookSurfaceSetCursor(void **vt)
{
    DWORD oldProtect;
    unsigned int idx = SURF_VT_SETCURSOR / sizeof(void *);
    if (vt == NULL || vt[idx] == NULL || vt[idx] == (void *)SetCursor_Hook
        || IsBadCodePtr((FARPROC)vt[idx])) {
        return;
    }
    if (!VirtualProtect(&vt[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }
    g_origSetCursor = (SurfSetCursorFn)vt[idx];
    vt[idx] = (void *)SetCursor_Hook;
    VirtualProtect(&vt[idx], sizeof(void *), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), &vt[idx], sizeof(void *));
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
    if (g_fieldSkipFill && x0 <= 1 && y0 <= 1 && g_roundW > 0 && g_roundH > 0) {
        rw = x1 - x0;
        rh = y1 - y0;
        if (rw >= g_roundW - 2 && rh >= g_roundH - 2) {
            return;
        }
    }
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
    /* ClientScheme ControlBG is 0-alpha (MOTD). Frame::PaintBackground still
     * issues that fill; writing it would punch through our plate. */
    if (((g_curColor >> 24) & 0xFFu) < 8) {
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
            g_edgeCaptured = 1;
            g_edgeX = x0;
            g_edgeY = y0;
            g_edgeRoundTop = 1;
            g_edgeRoundBottom = 1;
            DrawAaRoundedFillAt(x0, y0, rw, rh, r,
                                g_roundHot ? g_theme.accentRgb : g_theme.trackRgb,
                                g_theme.windowRgb, 1, 1);
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
        if (g_roundW > 0 && g_roundH > 0 &&
            rw >= g_roundW - 4 && rh >= g_roundH - 4) {
            g_edgeCaptured = 1;
            g_edgeX = x0;
            g_edgeY = y0;
            g_edgeRoundTop = roundTop;
            g_edgeRoundBottom = roundBottom;
            g_edgeBodyColor = g_curColor;
            DrawRoundedFillAt(x0, y0, rw, rh, r, g_curColor, roundTop, roundBottom);
            return;
        }
        /* Inset client/sheet fill inside the dialog — the extra rounded
         * box on Voice/Mouse/etc. Drop it; Advanced already hid it. */
        if (!g_roundIsButton) {
            return;
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
    if (g_GetSurface == NULL) {
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
    HookSurfaceSetCursor(vt);
    if (g_surfaceHooked) {
        return;
    }
    /* Video restart re-inits GameUI but vgui2.dll can stay mapped. Do not
     * take our own hooks as "original" — that recurses until the process dies. */
    if (vt[SURF_VT_DRAWSETCOLOR / sizeof(void *)] == (void *)DrawSetColor_Hook
        || vt[SURF_VT_DRAWFILLEDRECT / sizeof(void *)] == (void *)DrawFilledRect_Hook) {
        g_surfaceHooked = 1;
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
            g_roundR = isBtn ? CapsuleRadius(w, h) : RadiusForSize(w, h);
            g_roundActive = 1;
            g_edgeCaptured = 0;
            if (!isBtn) {
                /* QueryBox / MessageBox paint the client under the caption, not
                 * a full-size rect. DrawFilledRect_Hook treats that as an inset
                 * sheet and drops it — Quit had title+buttons and no plate.
                 * Fill first so orig can still paint the title on top. */
                DrawRoundedFillAt(0, 0, w, h, g_roundR,
                                  ThemeRgbPacked(g_theme.windowRgb), 1, 1);
                g_edgeCaptured = 1;
                g_edgeX = 0;
                g_edgeY = 0;
                g_edgeRoundTop = 1;
                g_edgeRoundBottom = 1;
            }
            orig(thisPtr);
            g_roundActive = 0;
            g_roundIsButton = 0;
            g_roundHot = 0;

            if (isBtn) {
                int px = g_edgeCaptured ? g_edgeX : 0;
                int py = g_edgeCaptured ? g_edgeY : 0;
                uint32_t rgb = ControlIsHot(thisPtr) ? g_theme.accentRgb
                                                     : g_theme.trackRgb;
                DrawAaRoundedFillAt(px, py, w, h, CapsuleRadius(w, h), rgb, g_theme.windowRgb, 1, 1);
            } else {
                /* Do not refill after orig: that covered the Frame title. */
                DrawRoundedStrokeAt(0, 0, w, h, g_roundR,
                                    ThemeStrokePacked(), ThemeStrokeThickness(),
                                    1, 1);
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

static void __fastcall TextEntryPaintBg_Hook(void *thisPtr)
{
    int combo;
    int w = 0, h = 0;

    __try {
        combo = IsComboField(thisPtr);
        if (combo || IsStyledTextField(thisPtr)) {
            EnsureSurfaceHooks();
            *(void **)((char *)thisPtr + OFF_PANEL_BORDER) = NULL;
            SetFgColorWhite(thisPtr);
            PaintFieldPlate(thisPtr);
            if (g_GetSize != NULL) {
                g_GetSize(thisPtr, &w, &h);
            }
            g_roundW = w;
            g_roundH = h;
            g_fieldSkipFill = 1;
            if (g_origTextEntryPaintBg != NULL) {
                g_origTextEntryPaintBg(thisPtr);
            }
            g_fieldSkipFill = 0;
            if (combo) {
                DrawComboCaret(w, h);
            }
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fieldSkipFill = 0;
    }
    if (g_origTextEntryPaintBg != NULL) {
        g_origTextEntryPaintBg(thisPtr);
    }
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
    if (IsComboBoxButton(thisPtr)) {
        return;
    }
    /* Frame::PaintBackground calls this directly. Don't nest another
     * rounded pass — the Frame hook already owns the plate. */
    if (g_roundActive) {
        if (g_origPanelPaintBg != NULL) {
            g_origPanelPaintBg(thisPtr);
        }
        return;
    }
    /* Labels keep scheme LabelBgColor (ControlBG, alpha 242) which reads
     * as a second grey box on the inner sheet. Skip the fill so static
     * text sits on the parent. */
    if (IsStaticTextPanel(thisPtr)) {
        return;
    }
    if (IsOptionsInnerChrome(thisPtr)) {
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
        if (w >= 160 && h >= 100) {
            const char *nm = PanelName(thisPtr);
            int gameW = 0, gameH = 0;
            GameClientSize(&gameW, &gameH);
            if (!(gameW > 0 && w >= gameW - 8)
                && !IsVguiFrame(thisPtr)
                && !NameContainsI(nm, "Dialog")
                && !NameContainsI(nm, "MessageBox")
                && !NameContainsI(nm, "QueryBox")
                && lstrcmpiA(nm, "BaseQuestionPanel") != 0
                && !NameIsMenuChrome(nm)) {
                return;
            }
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
    DrawAaDisk(ox, oy, d, rgb, g_theme.windowRgb);
}

static void DrawAaDiskOnTrack(int x0, int y0, int d, uint32_t rgb, int trackY, int trackH,
                              int splitX, uint32_t accentRgb, uint32_t trackRgb, uint32_t windowRgb)
{
    float cx;
    float cy;
    float rad;
    int px;
    int py;
    int sr;
    int sg;
    int sb;

    if (d < 4) {
        return;
    }
    cx = (float)x0 + (float)d * 0.5f;
    cy = (float)y0 + (float)d * 0.5f;
    rad = (float)d * 0.5f - 0.35f;
    sr = (int)((rgb >> 16) & 0xFFu);
    sg = (int)((rgb >> 8) & 0xFFu);
    sb = (int)(rgb & 0xFFu);
    for (py = y0; py < y0 + d; py++) {
        for (px = x0; px < x0 + d; px++) {
            float dx = ((float)px + 0.5f) - cx;
            float dy = ((float)py + 0.5f) - cy;
            float dist = (float)sqrt(dx * dx + dy * dy);
            float a = (rad + 1.15f) - dist;
            uint32_t bgRgb;
            int br, bg, bb, or_, og, ob;
            if (a <= 0.0f) {
                continue;
            }
            if (a > 1.0f) {
                a = 1.0f;
            }
            if (py >= trackY && py < trackY + trackH) {
                bgRgb = (px < splitX) ? accentRgb : trackRgb;
            } else {
                bgRgb = windowRgb;
            }
            br = (int)((bgRgb >> 16) & 0xFFu);
            bg = (int)((bgRgb >> 8) & 0xFFu);
            bb = (int)(bgRgb & 0xFFu);
            or_ = (int)((float)sr * a + (float)br * (1.0f - a) + 0.5f);
            og = (int)((float)sg * a + (float)bg * (1.0f - a) + 0.5f);
            ob = (int)((float)sb * a + (float)bb * (1.0f - a) + 0.5f);
            SurfaceFill(px, py, px + 1, py + 1, ThemeRgbPacked(
                ((unsigned)or_ << 16) | ((unsigned)og << 8) | (unsigned)ob));
        }
    }
}

static void DrawAaPillAt(int x0, int y0, int w, int h, uint32_t rgb, uint32_t bgRgb)
{
    float r;
    float rad;
    float cxL;
    float cxR;
    float cy;
    int px;
    int py;

    if (w < 4 || h < 4) {
        SurfaceFill(x0, y0, x0 + w, y0 + h, ThemeRgbPacked(rgb));
        return;
    }
    if ((h & 1) != 0) {
        h -= 1;
    }
    r = (float)h * 0.5f;
    rad = r - 0.35f;
    cxL = (float)x0 + r;
    cxR = (float)x0 + (float)w - r;
    cy = (float)y0 + r;
    for (py = y0; py < y0 + h; py++) {
        for (px = x0; px < x0 + w; px++) {
            float qx = (float)px + 0.5f;
            float qy = (float)py + 0.5f;
            float dx;
            float dy;
            float dist;
            float a;
            if (qx < cxL) {
                dx = qx - cxL;
                dy = qy - cy;
                dist = (float)sqrt(dx * dx + dy * dy);
            } else if (qx > cxR) {
                dx = qx - cxR;
                dy = qy - cy;
                dist = (float)sqrt(dx * dx + dy * dy);
            } else {
                dist = (float)fabs(qy - cy);
            }
            a = (rad + 1.15f) - dist;
            if (a <= 0.0f) {
                continue;
            }
            if (a > 1.0f) {
                a = 1.0f;
            }
            SurfaceFill(px, py, px + 1, py + 1, MixRgbPair(rgb, bgRgb, a));
        }
    }
}

static void DrawAaRoundedFillAt(int x0, int y0, int w, int h, int r, uint32_t rgb, uint32_t bgRgb,
                                int roundTop, int roundBottom)
{
    int topR;
    int botR;
    int px;
    int py;

    if (w <= 0 || h <= 0) {
        return;
    }
    topR = roundTop ? r : 0;
    botR = roundBottom ? r : 0;
    if (topR * 2 > w) {
        topR = w / 2;
    }
    if (botR * 2 > w) {
        botR = w / 2;
    }
    if (topR + botR > h) {
        int cap = h / 2;
        if (topR > cap) {
            topR = cap;
        }
        if (botR > cap) {
            botR = cap;
        }
    }
    if (topR < 2 && botR < 2) {
        SurfaceFill(x0, y0, x0 + w, y0 + h, ThemeRgbPacked(rgb));
        return;
    }
    for (py = y0; py < y0 + h; py++) {
        for (px = x0; px < x0 + w; px++) {
            float qx = (float)px + 0.5f;
            float qy = (float)py + 0.5f;
            float xL = (float)x0;
            float yT = (float)y0;
            float xR = (float)(x0 + w);
            float yB = (float)(y0 + h);
            float cr = 0.0f;
            float cx = 0.0f;
            float cy = 0.0f;
            int corner = 0;
            float a;
            if (topR > 0 && qy < yT + (float)topR) {
                if (qx < xL + (float)topR) {
                    corner = 1;
                    cr = (float)topR;
                    cx = xL + cr;
                    cy = yT + cr;
                } else if (qx >= xR - (float)topR) {
                    corner = 1;
                    cr = (float)topR;
                    cx = xR - cr;
                    cy = yT + cr;
                }
            } else if (botR > 0 && qy >= yB - (float)botR) {
                if (qx < xL + (float)botR) {
                    corner = 1;
                    cr = (float)botR;
                    cx = xL + cr;
                    cy = yB - cr;
                } else if (qx >= xR - (float)botR) {
                    corner = 1;
                    cr = (float)botR;
                    cx = xR - cr;
                    cy = yB - cr;
                }
            }
            if (corner) {
                float dx = qx - cx;
                float dy = qy - cy;
                float dist = (float)sqrt(dx * dx + dy * dy);
                float rad = cr - 0.35f;
                a = (rad + 1.15f) - dist;
                if (a <= 0.0f) {
                    continue;
                }
                if (a > 1.0f) {
                    a = 1.0f;
                }
            } else {
                a = 1.0f;
            }
            SurfaceFill(px, py, px + 1, py + 1, MixRgbPair(rgb, bgRgb, a));
        }
    }
}

static void DrawAaDisk(int x0, int y0, int d, uint32_t rgb, uint32_t bgRgb)
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
    rad = (float)d * 0.5f - 0.35f;
    sr = (int)((rgb >> 16) & 0xFFu);
    sg = (int)((rgb >> 8) & 0xFFu);
    sb = (int)(rgb & 0xFFu);
    br = (int)((bgRgb >> 16) & 0xFFu);
    bg = (int)((bgRgb >> 8) & 0xFFu);
    bb = (int)(bgRgb & 0xFFu);
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

    if (thisPtr != NULL && g_voiceTrackW > 40
        && lstrcmpiA(PanelName(thisPtr), "TestMicrophone") == 0
        && g_SetSize != NULL && g_GetSize != NULL) {
        int tw = 0, th = 0;
        g_GetSize(thisPtr, &tw, &th);
        if (tw != g_voiceTrackW || th != OPTIONS_TOGGLE_ROW_H) {
            g_SetSize(thisPtr, g_voiceTrackW, OPTIONS_TOGGLE_ROW_H);
        }
    }

    if (IsTitleCloseButton(thisPtr)) {
        PaintMacCloseDot(thisPtr);
        return;
    }
    if (IsComboBoxButton(thisPtr)) {
        return;
    }
    if (IsFrameSystemButton(thisPtr)) {
        return;
    }
    if (IsCvarToggleRow(thisPtr)) {
        PaintCvarToggleRow(thisPtr);
        return;
    }
    if (IsAdvancedSettingsRow(thisPtr)) {
        void **vtable = *(void ***)thisPtr;
        SetIntFn setAlign = (SetIntFn)vtable[OFF_BUTTON_SETCONTENTALIGNMENT_VT / sizeof(void *)];
        SetTextInsetFn setInset = (SetTextInsetFn)vtable[OFF_BUTTON_SETTEXTINSET_VT / sizeof(void *)];
        int w = 0, h = 0;
        setInset(thisPtr, 16, 0);
        setAlign(thisPtr, LABEL_ALIGN_WEST);
        ForceWhiteOnTransparent(thisPtr);
        SetFgColorWhite(thisPtr);
        if (g_origButtonPaint != NULL) {
            g_origButtonPaint(thisPtr);
        }
        g_GetSize(thisPtr, &w, &h);
        DrawRowChevron(w, h);
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
    HideFrameSystemButtons(thisPtr);
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

static void __fastcall SliderPaintBg_Hook(void *thisPtr)
{
    (void)thisPtr;
}

void RoundFrame_NoteVoiceTrackW(int wide)
{
    if (wide > 40 && wide < 400) {
        g_voiceTrackW = wide;
    }
}

void RoundFrame_PaintOptionsSlider(void *slider)
{
    AudioExtra_OnSliderPaint(slider);
    SnapCvarSlider(slider);
    DrawValueSlider(slider);
}

static void __fastcall SliderPaint_Hook(void *thisPtr)
{
    RoundFrame_PaintOptionsSlider(thisPtr);
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
    DrawPillAt(0, y, w, barH, g_theme.trackRgb);
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

#define OFF_CROSSHAIR_BAR 0x94
#define OFF_CROSSHAIR_GAP 0x98

static void __fastcall CrosshairPaint_Hook(void *thisPtr)
{
    int w = 0, h = 0;
    int *bar;
    int *gap;
    int oldBar;
    int oldGap;
    int maxR;
    int largeNeed;
    int sw = 0;
    int sh = 0;

    if (g_origCrosshairPaint == NULL) {
        return;
    }
    if (thisPtr == NULL || g_GetSize == NULL) {
        g_origCrosshairPaint(thisPtr);
        return;
    }
    g_GetSize(thisPtr, &w, &h);
    bar = (int *)((char *)thisPtr + OFF_CROSSHAIR_BAR);
    gap = (int *)((char *)thisPtr + OFF_CROSSHAIR_GAP);
    oldBar = *bar;
    oldGap = *gap;
    maxR = ((w < h) ? w : h) / 2 - 2;
    if (maxR < 4) {
        maxR = 4;
    }
    /* Stock Large is (9+5)*screenWide/640. Clamping each size to maxR
     * separately made Medium and Large identical on 4:3. Scale everything
     * by the same factor so Small < Medium < Large still reads. */
    largeNeed = oldBar + oldGap;
    if (g_GetSurface != NULL) {
        void *surf = g_GetSurface();
        if (surf != NULL) {
            void **vt = *(void ***)surf;
            SurfGetScreenSizeFn getScreen = (SurfGetScreenSizeFn)vt[SURF_VT_GETSCREENSIZE / sizeof(void *)];
            if (getScreen != NULL) {
                getScreen(surf, &sw, &sh);
            }
        }
    }
    if (sw > 0) {
        largeNeed = (14 * sw) / 640;
        if (largeNeed < oldBar + oldGap) {
            largeNeed = oldBar + oldGap;
        }
    }
    if (largeNeed > maxR && largeNeed > 0) {
        *bar = oldBar * maxR / largeNeed;
        *gap = oldGap * maxR / largeNeed;
        if (*bar < 1) {
            *bar = 1;
        }
        if (*gap < 0) {
            *gap = 0;
        }
    }
    g_origCrosshairPaint(thisPtr);
    *bar = oldBar;
    *gap = oldGap;
}

static float DistPointToSeg(float px, float py, float x0, float y0, float x1, float y1, float *outT)
{
    float vx = x1 - x0;
    float vy = y1 - y0;
    float wx = px - x0;
    float wy = py - y0;
    float c2 = vx * vx + vy * vy;
    float t;
    float dx;
    float dy;

    if (c2 < 0.0001f) {
        if (outT != NULL) {
            *outT = 0.0f;
        }
        return sqrtf(wx * wx + wy * wy);
    }
    t = (vx * wx + vy * wy) / c2;
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    if (outT != NULL) {
        *outT = t;
    }
    dx = px - (x0 + t * vx);
    dy = py - (y0 + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

static int LoadVuFaceTga(void)
{
    const char *root;
    char path[MAX_PATH];
    FILE *f;
    unsigned char hdr[18];
    int w;
    int h;
    int y0;
    int y1;
    size_t nbytes;
    unsigned char row[VU_FACE_MAX_W * 4];

    if (g_vuFaceTried) {
        return g_vuFaceLoaded;
    }
    g_vuFaceTried = 1;
    root = BgSwitch_GetGameRoot();
    if (root == NULL || root[0] == '\0') {
        return 0;
    }
    _snprintf(path, sizeof(path), "%s\\cstrike\\resource\\mic_meter_dead.tga", root);
    f = fopen(path, "rb");
    if (f == NULL) {
        _snprintf(path, sizeof(path), "%s\\valve\\resource\\mic_meter_dead.tga", root);
        f = fopen(path, "rb");
    }
    if (f == NULL) {
        HookLog("VU face: tga not found");
        return 0;
    }
    if (fread(hdr, 1, 18, f) != 18 || hdr[2] != 2 || hdr[16] != 32) {
        fclose(f);
        return 0;
    }
    w = (int)hdr[12] | ((int)hdr[13] << 8);
    h = (int)hdr[14] | ((int)hdr[15] << 8);
    if (w < 8 || h < 4 || w > VU_FACE_MAX_W || h > VU_FACE_MAX_H) {
        fclose(f);
        return 0;
    }
    nbytes = (size_t)w * (size_t)h * 4u;
    if (fread(g_vuFaceBgra, 1, nbytes, f) != nbytes) {
        fclose(f);
        return 0;
    }
    fclose(f);
    /* File is bottom-up (descriptor bit 5 clear). Flip to top-down for blit. */
    if ((hdr[17] & 0x20) == 0) {
        for (y0 = 0, y1 = h - 1; y0 < y1; y0++, y1--) {
            memcpy(row, g_vuFaceBgra + (size_t)y0 * (size_t)w * 4u, (size_t)w * 4u);
            memcpy(g_vuFaceBgra + (size_t)y0 * (size_t)w * 4u,
                   g_vuFaceBgra + (size_t)y1 * (size_t)w * 4u, (size_t)w * 4u);
            memcpy(g_vuFaceBgra + (size_t)y1 * (size_t)w * 4u, row, (size_t)w * 4u);
        }
    }
    g_vuFaceW = w;
    g_vuFaceH = h;
    g_vuFaceLoaded = 1;
    HookLog("VU face: loaded %dx%d with alpha from %s", w, h, path);
    return 1;
}

static void DrawVuFace(int panelW, int panelH)
{
    int x;
    int y;

    if (!LoadVuFaceTga() || panelW <= 0 || panelH <= 0) {
        return;
    }
    for (y = 0; y < panelH; y++) {
        int sy = (g_vuFaceH * y) / panelH;
        for (x = 0; x < panelW; x++) {
            unsigned char *p;
            int a;
            uint32_t rgb;
            int sx = (g_vuFaceW * x) / panelW;
            p = g_vuFaceBgra + ((size_t)sy * (size_t)g_vuFaceW + (size_t)sx) * 4u;
            a = (int)p[3];
            if (a < 12) {
                continue;
            }
            rgb = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
            if (a >= 248) {
                SurfaceFill(x, y, x + 1, y + 1, ThemeRgbPacked(rgb));
            } else {
                SurfaceFill(x, y, x + 1, y + 1,
                            MixRgbPair(rgb, g_theme.windowRgb, (float)a / 255.0f));
            }
        }
    }
}

static void DrawAaNeedle(float x0, float y0, float x1, float y1, uint32_t rgb)
{
    /* Opaque pixels only. AA used to mix into a dark face color, which drew a
     * halo on the red hub instead of merging with it. */
    int minx;
    int maxx;
    int miny;
    int maxy;
    int px;
    int py;

    minx = (int)(x0 < x1 ? x0 : x1) - 4;
    maxx = (int)(x0 > x1 ? x0 : x1) + 4;
    miny = (int)(y0 < y1 ? y0 : y1) - 4;
    maxy = (int)(y0 > y1 ? y0 : y1) + 4;
    if (minx < 0) {
        minx = 0;
    }
    if (miny < 0) {
        miny = 0;
    }
    for (py = miny; py <= maxy; py++) {
        for (px = minx; px <= maxx; px++) {
            float t = 0.0f;
            float dist = DistPointToSeg((float)px + 0.5f, (float)py + 0.5f, x0, y0, x1, y1, &t);
            float half = 1.55f * (1.0f - t) + 0.62f * t;
            if (dist <= half) {
                SurfaceFill(px, py, px + 1, py + 1, ThemeRgbPacked(rgb));
            }
        }
    }
}

static void DrawVuNeedle(int w, int h, float level)
{
    const float a0 = 2.6179938f;
    const float a1 = 0.5235988f;
    const uint32_t peakRgb = g_theme.accentRgb;
    /* Pivot and length match the analog VU TGA (2:1 face, hub below the scale). */
    float cx = (float)(w / 2 - 1) + 0.5f;
    float cy = (float)h * 0.821f;
    float rad = (float)h * 0.547f;
    float a;
    float nx;
    float ny;

    if (level < 0.0f) {
        level = 0.0f;
    }
    if (level > 1.0f) {
        level = 1.0f;
    }
    if (rad < 18.0f) {
        rad = 18.0f;
    }
    a = a0 + level * (a1 - a0);
    nx = cx + cosf(a) * (rad - 4.0f);
    ny = cy - sinf(a) * (rad - 4.0f);
    DrawAaNeedle(cx, cy, nx, ny, peakRgb);
}

static void __fastcall ImagePanelPaintBg_Hook(void *thisPtr)
{
    int w = 0, h = 0;
    float level;

    if (thisPtr == NULL) {
        if (g_origImagePanelPaintBg != NULL) {
            g_origImagePanelPaintBg(thisPtr);
        }
        return;
    }
    {
        const char *nm = PanelName(thisPtr);
        int isL = lstrcmpiA(nm, "MicMeterL") == 0;
        int isR = lstrcmpiA(nm, "MicMeterR") == 0;
        if (!isL && !isR && lstrcmpiA(nm, "MicMeter") != 0) {
            if (g_origImagePanelPaintBg != NULL) {
                g_origImagePanelPaintBg(thisPtr);
            }
            return;
        }
        if (g_GetSize == NULL) {
            return;
        }
        g_GetSize(thisPtr, &w, &h);
        if (w < 8 || h < 4) {
            return;
        }
        EnsureSurfaceHooks();
        if (!isL && !isR) {
            /* Stock live overlay: GameUI sets wide to 0..160 from speaking volume. */
            if (w <= 160) {
                g_vuLiveW = w;
                g_vuLiveHold = 3;
            }
            return;
        }
        level = AudioExtra_VuLevel(isR);
        if (level < 0.0f) {
            level = (float)g_vuLiveW / 160.0f;
        }
        /* Stock ImagePanel treats the TGA as opaque RGB (black corners).
         * Blit 32-bit alpha ourselves so A=0 shows the page behind. */
        if (LoadVuFaceTga()) {
            DrawVuFace(w, h);
        } else if (g_origImagePanelPaintBg != NULL) {
            g_origImagePanelPaintBg(thisPtr);
        }
        DrawVuNeedle(w, h, level);
        return;
    }
}

static void __fastcall PaintBorder_Hook(void *thisPtr)
{
    if (IsComboBoxButton(thisPtr) || IsStyledTextField(thisPtr)) {
        return;
    }
    if (IsSettingsToggle(thisPtr)) {
        return;
    }
    if (IsCvarToggleRow(thisPtr)) {
        return;
    }
    if (IsOptionsInnerChrome(thisPtr)) {
        return;
    }
    if (g_GetSize != NULL && thisPtr != NULL && !ShouldRoundButton(thisPtr)) {
        int bw = 0, bh = 0;
        g_GetSize(thisPtr, &bw, &bh);
        if (bw >= 160 && bh >= 100 && !ShouldRoundPanel(thisPtr)) {
            return;
        }
    }
    if (IsProgressBar(thisPtr) || IsOptionsSlider(thisPtr) || IsTitleCloseButton(thisPtr)) {
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
    g_sliderCursorOwner = NULL;
    g_sliderDragAbandoned = 0;
    g_sliderClipHwnd = NULL;
    ReleaseSliderCursorClip();
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
    InstallTitlePlaceHook(base);
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
    {
        static const BYTE kImageBgPrologue[6] = { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1 };
        InstallNearHook(base + RVA_IMAGEPANEL_PAINTBG, 6, kImageBgPrologue,
                        g_imagePanelPaintBgTramp, sizeof(g_imagePanelPaintBgTramp),
                        (void *)ImagePanelPaintBg_Hook, &g_origImagePanelPaintBg,
                        "ImagePanelPaintBackground");
    }
    {
        static const BYTE kSliderPaintPrologue[8] = { 0x56, 0x8B, 0xF1, 0xE8, 0x18, 0x00, 0x00, 0x00 };
        InstallNearHook(base + RVA_SLIDER_PAINT, 8, kSliderPaintPrologue,
                        g_sliderPaintTramp, sizeof(g_sliderPaintTramp),
                        (void *)SliderPaint_Hook, &g_origSliderPaint, "SliderPaint");
    }
    {
        static const BYTE kCvarApplyPrologue[10] = {
            0x51, 0x56, 0x8B, 0xF1, 0x8A, 0x86, 0xB7, 0x00, 0x00, 0x00
        };
        InstallNearHook(base + RVA_CCVARSLIDER_APPLY, 10, kCvarApplyPrologue,
                        g_cvarSliderApplyTramp, sizeof(g_cvarSliderApplyTramp),
                        (void *)CvarSliderApply_Hook, &g_origCvarSliderApply, "CCvarSliderApplyChanges");
    }
    {
        static const BYTE kSliderBgPrologue[6] = { 0x83, 0xEC, 0x14, 0x53, 0x56, 0x57 };
        InstallNearHook(base + RVA_SLIDER_PAINTBG, 6, kSliderBgPrologue,
                        g_sliderPaintBgTramp, sizeof(g_sliderPaintBgTramp),
                        (void *)SliderPaintBg_Hook, &g_origSliderPaintBg, "SliderPaintBackground");
    }
    {
        static const BYTE kXhPaintPrologue[6] = { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1 };
        InstallNearHook(base + RVA_CROSSHAIRIMAGE_PAINT, 6, kXhPaintPrologue,
                        g_crosshairPaintTramp, sizeof(g_crosshairPaintTramp),
                        (void *)CrosshairPaint_Hook, &g_origCrosshairPaint, "CrosshairImagePaint");
    }
    {
        /* sub esp,20; push esi; mov esi,ecx; push edi — do not steal the
         * following 8B 06 (mov eax,[esi]); an 8-byte patch split it. */
        static const BYTE kTextEntryBgPrologue[7] = {
            0x83, 0xEC, 0x20, 0x56, 0x8B, 0xF1, 0x57
        };
        InstallNearHook(base + RVA_TEXTENTRY_PAINTBG, 7, kTextEntryBgPrologue,
                        g_textEntryPaintBgTramp, sizeof(g_textEntryPaintBgTramp),
                        (void *)TextEntryPaintBg_Hook, &g_origTextEntryPaintBg,
                        "TextEntryPaintBackground");
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
