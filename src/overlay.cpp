#include "overlay.h"

#ifndef LEAFLET_LVGL

void Overlay_Start(void)
{
}

#else

#include "prefetch.h"
#include "scheme.h"
#include "log.h"

#include <lvgl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define HOLD_MS 450
#define CLASS_NAME L"LeafletLvglOverlay"

/* macOS dark (Big Sur+):
 *  window / HUD fill  #2C2C2E
 *  elevated control   #3A3A3C
 *  label              #F5F5F7
 *  secondary          #98989D
 *  separator          #48484A
 *  accent             #0A84FF  (progress fill; close to our #3d8bfd) */
#define COL_DIM        0x000000
#define COL_WINDOW     0x1C1C1E
#define COL_PILL       0x3A3A3C
#define COL_ACCENT     0x0A84FF
#define COL_TEXT       0xF5F5F7
#define COL_MUTED      0x98989D
#define COL_STROKE     0x3A3A3C
#define DIM_OPA        110
#define WIN_W_MAX      420
#define WIN_PAD        16
#define WIN_GAP        8
#define BTN_H          12

static volatile LONG g_started = 0;

static HWND g_hwnd = NULL;
static HWND g_game = NULL;
static HDC g_memDc = NULL;
static HBITMAP g_dib = NULL;
static void *g_bits = NULL;
static int g_w = 0;
static int g_h = 0;
static lv_display_t *g_disp = NULL;
static void *g_lvBuf = NULL;
static lv_obj_t *g_caption = NULL;
static lv_obj_t *g_label = NULL;
static lv_obj_t *g_bar = NULL;
static char g_shownLabel[260];
static int g_holdLeft = 0;
static int g_bannerOn = 0;

static uint32_t TickMs(void)
{
    return GetTickCount();
}

typedef struct FindGameCtx {
    HWND best;
    LONG area;
} FindGameCtx;

static BOOL CALLBACK FindGameEnum(HWND w, LPARAM lp)
{
    FindGameCtx *ctx = (FindGameCtx *)lp;
    DWORD pid = 0;
    wchar_t cls[64];
    RECT r;
    LONG area;

    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) {
        return TRUE;
    }
    if (GetClassNameW(w, cls, 64) > 0 && wcscmp(cls, CLASS_NAME) == 0) {
        return TRUE;
    }
    GetClientRect(w, &r);
    area = (r.right - r.left) * (r.bottom - r.top);
    if (area > ctx->area) {
        ctx->area = area;
        ctx->best = w;
    }
    return TRUE;
}

static HWND FindGameWindow(void)
{
    HWND w;
    DWORD pid;
    DWORD self = GetCurrentProcessId();
    FindGameCtx ctx;

    w = FindWindowA("Valve001", NULL);
    if (w != NULL && IsWindowVisible(w)) {
        GetWindowThreadProcessId(w, &pid);
        if (pid == self) {
            return w;
        }
    }
    ctx.best = NULL;
    ctx.area = 0;
    EnumWindows(FindGameEnum, (LPARAM)&ctx);
    return ctx.best;
}

static void DestroySurface(void)
{
    if (g_memDc != NULL && g_dib != NULL) {
        SelectObject(g_memDc, (HGDIOBJ)NULL);
    }
    if (g_dib != NULL) {
        DeleteObject(g_dib);
        g_dib = NULL;
    }
    if (g_memDc != NULL) {
        DeleteDC(g_memDc);
        g_memDc = NULL;
    }
    g_bits = NULL;
}

static int CreateSurface(int width, int height)
{
    BITMAPINFO bmi;
    HDC screen;

    DestroySurface();
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    screen = GetDC(NULL);
    g_memDc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (g_memDc == NULL) {
        return 0;
    }
    g_dib = CreateDIBSection(g_memDc, &bmi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    if (g_dib == NULL || g_bits == NULL) {
        DestroySurface();
        return 0;
    }
    SelectObject(g_memDc, g_dib);
    memset(g_bits, 0, (size_t)width * (size_t)height * 4u);
    g_w = width;
    g_h = height;
    return 1;
}

static void BlitLayered(int show)
{
    POINT dst;
    SIZE size;
    POINT src;
    BLENDFUNCTION blend;
    POINT topLeft;

    if (g_hwnd == NULL || g_bits == NULL || g_game == NULL || !IsWindow(g_game)) {
        return;
    }
    if (!show) {
        ShowWindow(g_hwnd, SW_HIDE);
        return;
    }
    topLeft.x = 0;
    topLeft.y = 0;
    ClientToScreen(g_game, &topLeft);
    dst.x = topLeft.x;
    dst.y = topLeft.y;
    SetWindowPos(g_hwnd, HWND_TOPMOST, dst.x, dst.y, g_w, g_h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    size.cx = g_w;
    size.cy = g_h;
    src.x = 0;
    src.y = 0;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_hwnd, NULL, &dst, &size, g_memDc, &src, 0, &blend, ULW_ALPHA);
}

static void FlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t y;
    int32_t w = lv_area_get_width(area);
    uint32_t *src = (uint32_t *)px_map;
    uint32_t *dst = (uint32_t *)g_bits;

    if (dst != NULL) {
        for (y = area->y1; y <= area->y2; y++) {
            uint32_t *row = dst + (size_t)y * (size_t)g_w + (size_t)area->x1;
            int32_t x;
            for (x = 0; x < w; x++) {
                uint32_t px = src[x];
                uint32_t a = (px >> 24) & 0xFFu;
                uint32_t r = (px >> 16) & 0xFFu;
                uint32_t g = (px >> 8) & 0xFFu;
                uint32_t b = px & 0xFFu;
                if (a == 0) {
                    row[x] = 0;
                } else {
                    row[x] = (a << 24)
                        | (((r * a) / 255u) << 16)
                        | (((g * a) / 255u) << 8)
                        | ((b * a) / 255u);
                }
            }
            src += w;
        }
    }
    lv_display_flush_ready(disp);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int CreateOverlayWindow(HINSTANCE inst, int width, int height)
{
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CLASS_NAME,
        L"",
        WS_POPUP,
        0, 0, width, height,
        NULL, NULL, inst, NULL);
    return g_hwnd != NULL;
}

static int WinWidth(void)
{
    int w = g_w - 96;
    if (w > WIN_W_MAX) {
        w = WIN_W_MAX;
    }
    if (w < 300) {
        w = 300;
    }
    return w;
}

static void StyleMatte(lv_obj_t *obj, uint32_t color, int radius)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(COL_STROKE), 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void BuildUi(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *win;
    int winW = WinWidth();
    OverlayTheme theme;

    OverlayTheme_Load(&theme);

    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_bg_opa(scr, DIM_OPA, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    win = lv_obj_create(scr);
    StyleMatte(win, theme.windowRgb, 12);
    lv_obj_set_style_border_color(win, lv_color_hex(theme.borderRgb), 0);
    lv_obj_set_style_border_width(win, theme.borderWidth, 0);
    lv_obj_set_width(win, winW);
    lv_obj_set_height(win, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(win, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(win, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(win, WIN_PAD, 0);
    lv_obj_set_style_pad_row(win, WIN_GAP, 0);

    g_caption = lv_label_create(win);
    lv_label_set_text(g_caption, "Downloading");
    lv_obj_set_style_text_color(g_caption, lv_color_hex(theme.textRgb), 0);
    lv_obj_set_style_text_opa(g_caption, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(g_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_caption, lv_pct(100));

    g_label = lv_label_create(win);
    lv_label_set_long_mode(g_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_label, lv_pct(100));
    lv_obj_set_style_text_color(g_label, lv_color_hex(theme.mutedRgb), 0);
    lv_obj_set_style_text_opa(g_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(g_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(g_label, "");

    g_bar = lv_bar_create(win);
    lv_obj_set_width(g_bar, lv_pct(100));
    lv_obj_set_height(g_bar, BTN_H);
    lv_bar_set_range(g_bar, 0, 1000);
    lv_obj_remove_flag(g_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(g_bar, lv_color_hex(theme.trackRgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bar, lv_color_hex(theme.accentRgb), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    lv_obj_update_layout(win);
    lv_obj_center(win);
}

static int InitLvgl(int width, int height)
{
    size_t bufBytes;

    lv_init();
    lv_tick_set_cb(TickMs);
    if (!CreateSurface(width, height)) {
        return 0;
    }
    bufBytes = (size_t)width * 64u * 4u;
    g_lvBuf = malloc(bufBytes);
    if (g_lvBuf == NULL) {
        return 0;
    }
    g_disp = lv_display_create(width, height);
    if (g_disp == NULL) {
        return 0;
    }
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(g_disp, g_lvBuf, NULL, (uint32_t)bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(g_disp, FlushCb);
    BuildUi();
    return 1;
}

static int SyncUi(void)
{
    int active = 0;
    int permille = 0;
    char label[260];
    const char *shown;

    Prefetch_GetUi(&active, &permille, label, sizeof(label));
    if (label[0] != '\0') {
        lstrcpynA(g_shownLabel, label, sizeof(g_shownLabel));
    }
    if (active) {
        g_holdLeft = HOLD_MS;
        g_bannerOn = 1;
    } else if (g_holdLeft > 0 && g_shownLabel[0] != '\0') {
        g_holdLeft -= 16;
        g_bannerOn = 1;
    } else {
        g_holdLeft = 0;
        g_bannerOn = 0;
        g_shownLabel[0] = '\0';
    }

    shown = g_shownLabel;
    if (g_label != NULL) {
        lv_label_set_text(g_label, shown);
    }
    if (g_bar != NULL) {
        lv_bar_set_value(g_bar, permille, LV_ANIM_OFF);
    }
    return g_bannerOn;
}

static DWORD WINAPI OverlayThread(LPVOID unused)
{
    HINSTANCE inst;
    RECT client;
    MSG msg;
    int idleMs = 0;
    int width;
    int height;
    DWORD last = GetTickCount();

    (void)unused;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)&OverlayThread, (HMODULE *)&inst);

    while (g_game == NULL || !IsWindow(g_game)) {
        g_game = FindGameWindow();
        if (Prefetch_IsActive() == 0) {
            idleMs += 50;
            if (idleMs > 8000) {
                InterlockedExchange(&g_started, 0);
                return 0;
            }
        }
        Sleep(50);
    }

    GetClientRect(g_game, &client);
    width = client.right - client.left;
    height = client.bottom - client.top;
    if (width < 320) {
        width = 320;
    }
    if (height < 240) {
        height = 240;
    }
    if (!CreateOverlayWindow(inst, width, height) || !InitLvgl(width, height)) {
        HookLog("Overlay: LVGL init failed");
        InterlockedExchange(&g_started, 0);
        return 0;
    }
    HookLog("Overlay: LVGL sheet %dx%d", width, height);
    idleMs = 0;
    g_shownLabel[0] = '\0';
    g_holdLeft = 0;
    g_bannerOn = 0;

    for (;;) {
        int show;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        show = SyncUi();
        lv_timer_handler();
        BlitLayered(show);
        if (!show && Prefetch_IsActive() == 0) {
            idleMs += 16;
            if (idleMs > 50) {
                break;
            }
        } else {
            idleMs = 0;
        }
        Sleep(16);
        (void)last;
    }

    if (g_hwnd != NULL) {
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
    }
    DestroySurface();
    free(g_lvBuf);
    g_lvBuf = NULL;
    g_disp = NULL;
    InterlockedExchange(&g_started, 0);
    return 0;
}

void Overlay_Start(void)
{
    HANDLE thread;

    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        return;
    }
    thread = CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL);
    if (thread == NULL) {
        InterlockedExchange(&g_started, 0);
        return;
    }
    CloseHandle(thread);
}

#endif
