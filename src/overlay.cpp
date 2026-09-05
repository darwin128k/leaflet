#include "overlay.h"

#ifndef LEAFLET_LVGL

void Overlay_Start(void)
{
}

#else

#include "prefetch.h"
#include "log.h"

#include <lvgl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define BAR_H 36
#define CLASS_NAME L"LeafletLvglOverlay"

static volatile LONG g_started = 0;

static HWND g_hwnd = NULL;
static HWND g_game = NULL;
static HDC g_memDc = NULL;
static HBITMAP g_dib = NULL;
static void *g_bits = NULL;
static int g_w = 0;
static lv_display_t *g_disp = NULL;
static void *g_lvBuf = NULL;
static lv_obj_t *g_label = NULL;
static lv_obj_t *g_bar = NULL;

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

static int CreateSurface(int width)
{
    BITMAPINFO bmi;
    HDC screen;

    DestroySurface();
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -BAR_H;
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
    g_w = width;
    return 1;
}

static void BlitLayered(void)
{
    POINT dst;
    SIZE size;
    POINT src;
    BLENDFUNCTION blend;
    RECT client;
    POINT topLeft;

    if (g_hwnd == NULL || g_bits == NULL || g_game == NULL || !IsWindow(g_game)) {
        return;
    }
    GetClientRect(g_game, &client);
    topLeft.x = 0;
    topLeft.y = 0;
    ClientToScreen(g_game, &topLeft);
    dst.x = topLeft.x;
    dst.y = topLeft.y;
    SetWindowPos(g_hwnd, HWND_TOPMOST, dst.x, dst.y, g_w, BAR_H,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    size.cx = g_w;
    size.cy = BAR_H;
    src.x = 0;
    src.y = 0;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_hwnd, NULL, &dst, &size, g_memDc, &src, 0, &blend, ULW_ALPHA);
    (void)client;
}

static void FlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t y;
    int32_t w = lv_area_get_width(area);
    uint32_t *src = (uint32_t *)px_map;
    uint32_t *dst = (uint32_t *)g_bits;

    if (dst != NULL) {
        for (y = area->y1; y <= area->y2; y++) {
            memcpy(dst + (size_t)y * (size_t)g_w + (size_t)area->x1, src, (size_t)w * 4u);
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

static int CreateOverlayWindow(HINSTANCE inst, int width)
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
        0, 0, width, BAR_H,
        NULL, NULL, inst, NULL);
    return g_hwnd != NULL;
}

static void BuildUi(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x141414), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);

    g_label = lv_label_create(scr);
    lv_label_set_long_mode(g_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(g_label, g_w - 16);
    lv_obj_align(g_label, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_obj_set_style_text_color(g_label, lv_color_hex(0xe8e8e8), 0);
    lv_label_set_text(g_label, "");

    g_bar = lv_bar_create(scr);
    lv_obj_set_size(g_bar, g_w, 4);
    lv_obj_align(g_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(g_bar, 0, 1000);
    lv_obj_set_style_bg_color(g_bar, lv_color_hex(0x2a2a2a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bar, lv_color_hex(0x3d8bfd), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_bar, 0, LV_PART_INDICATOR);
}

static int InitLvgl(int width)
{
    size_t bufBytes;

    lv_init();
    lv_tick_set_cb(TickMs);
    if (!CreateSurface(width)) {
        return 0;
    }
    bufBytes = (size_t)width * (size_t)BAR_H * 4u;
    g_lvBuf = malloc(bufBytes);
    if (g_lvBuf == NULL) {
        return 0;
    }
    g_disp = lv_display_create(width, BAR_H);
    if (g_disp == NULL) {
        return 0;
    }
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(g_disp, g_lvBuf, NULL, (uint32_t)bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(g_disp, FlushCb);
    BuildUi();
    return 1;
}

static void SyncUi(void)
{
    int active = 0;
    int permille = 0;
    char label[260];

    Prefetch_GetUi(&active, &permille, label, sizeof(label));
    if (g_label != NULL) {
        lv_label_set_text(g_label, label);
    }
    if (g_bar != NULL) {
        lv_bar_set_value(g_bar, permille, LV_ANIM_OFF);
    }
    if (g_hwnd != NULL) {
        ShowWindow(g_hwnd, active ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
}

static DWORD WINAPI OverlayThread(LPVOID unused)
{
    HINSTANCE inst;
    RECT client;
    MSG msg;
    int idleMs = 0;
    int width;
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
    if (width < 320) {
        width = 320;
    }
    if (!CreateOverlayWindow(inst, width) || !InitLvgl(width)) {
        HookLog("Overlay: LVGL init failed");
        InterlockedExchange(&g_started, 0);
        return 0;
    }
    HookLog("Overlay: LVGL banner %dx%d", width, BAR_H);
    idleMs = 0;

    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        SyncUi();
        lv_timer_handler();
        BlitLayered();
        if (Prefetch_IsActive() == 0) {
            idleMs += 16;
            if (idleMs > 600) {
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
