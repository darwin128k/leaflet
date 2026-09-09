#include "scheme.h"
#include "bgswitch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int ParseRgba(const char *s, int *r, int *g, int *b, int *a)
{
    int n;
    n = sscanf(s, "%d %d %d %d", r, g, b, a);
    if (n == 3) {
        *a = 255;
        return 1;
    }
    return n == 4;
}

static uint32_t Rgb(int r, int g, int b)
{
    if (r < 0) {
        r = 0;
    }
    if (r > 255) {
        r = 255;
    }
    if (g < 0) {
        g = 0;
    }
    if (g > 255) {
        g = 255;
    }
    if (b < 0) {
        b = 0;
    }
    if (b > 255) {
        b = 255;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static char *LoadFile(const char *path, size_t *outLen)
{
    FILE *f;
    char *buf;
    long sz;

    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    *outLen = (size_t)sz;
    return buf;
}

static const char *FindColorsBlock(const char *text)
{
    const char *p = strstr(text, "Colors");
    if (p == NULL) {
        return NULL;
    }
    p = strchr(p, '{');
    return p;
}

static int LookupColor(const char *colors, const char *name, uint32_t *rgb)
{
    char key[96];
    const char *p;
    const char *q;
    int r = 0, g = 0, b = 0, a = 255;
    size_t nameLen = strlen(name);

    _snprintf(key, sizeof(key), "\"%s\"", name);
    key[sizeof(key) - 1] = '\0';
    p = colors;
    for (;;) {
        p = strstr(p, key);
        if (p == NULL) {
            return 0;
        }
        if (p[nameLen + 2] == '\0' || (p[nameLen + 2] != '"' && p[nameLen + 2] > 32)) {
            /* keep searching; require the closing quote was the end of the key */
        }
        q = p + strlen(key);
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (*q == '"') {
            q++;
            if (ParseRgba(q, &r, &g, &b, &a)) {
                *rgb = Rgb(r, g, b);
                return 1;
            }
        }
        p += 1;
    }
}

static int LookupFrameBorderWidth(const char *text)
{
    const char *p = strstr(text, "FrameBorder");
    const char *end;
    int a = 0, b = 0, c = 0, d = 0;
    int w;

    if (p == NULL) {
        return 1;
    }
    end = strstr(p, "TabBorder");
    if (end == NULL) {
        end = p + 800;
    }
    p = strstr(p, "\"inset\"");
    if (p == NULL || p >= end) {
        return 1;
    }
    p = strchr(p + 7, '"');
    if (p == NULL) {
        return 1;
    }
    if (sscanf(p + 1, "%d %d %d %d", &a, &b, &c, &d) < 1) {
        return 1;
    }
    w = a;
    if (b > w) {
        w = b;
    }
    if (c > w) {
        w = c;
    }
    if (d > w) {
        w = d;
    }
    if (w < 1) {
        w = 1;
    }
    if (w > 4) {
        w = 4;
    }
    return w;
}

static void ApplyFile(const char *path, OverlayTheme *out)
{
    size_t len = 0;
    char *text;
    const char *colors;
    uint32_t rgb;

    text = LoadFile(path, &len);
    if (text == NULL) {
        return;
    }
    colors = FindColorsBlock(text);
    if (colors != NULL) {
        if (LookupColor(colors, "CareerBoxBG", &rgb) || LookupColor(colors, "WindowBG", &rgb)
            || LookupColor(colors, "Menu/BgColor", &rgb)) {
            out->windowRgb = rgb;
        }
        if (LookupColor(colors, "BorderBright", &rgb) || LookupColor(colors, "BorderDark", &rgb)) {
            out->borderRgb = rgb;
        }
        if (LookupColor(colors, "BrightBaseText", &rgb) || LookupColor(colors, "BaseText", &rgb)) {
            out->textRgb = rgb;
        }
        if (LookupColor(colors, "DimBaseText", &rgb) || LookupColor(colors, "LabelDimText", &rgb)) {
            out->mutedRgb = rgb;
        }
        if (LookupColor(colors, "SelectionBG", &rgb) || LookupColor(colors, "BrightControlText", &rgb)) {
            out->accentRgb = rgb;
        }
        if (LookupColor(colors, "ControlDarkBG", &rgb) || LookupColor(colors, "SliderTrackColor", &rgb)) {
            out->trackRgb = rgb;
        }
    }
    out->borderWidth = LookupFrameBorderWidth(text);
    if (out->borderWidth < 2) {
        out->borderWidth = 2;
    }
    free(text);
}

void OverlayTheme_Load(OverlayTheme *out)
{
    const char *root;
    char path[MAX_PATH];

    out->windowRgb = 0x1C1C1E;
    out->borderRgb = 0x3A3A3C;
    out->textRgb = 0xF5F5F7;
    out->mutedRgb = 0x98989D;
    out->accentRgb = 0x0A84FF;
    out->trackRgb = 0x3A3A3C;
    out->borderWidth = 2;

    root = BgSwitch_GetGameRoot();
    if (root == NULL || root[0] == '\0') {
        return;
    }
    /* GameUI / overlay: TrackerScheme. ClientScheme is the in-game VGUI
     * (MOTD, team menu) and stays the stock CS gold palette. */
    _snprintf(path, sizeof(path), "%s\\platform\\resource\\TrackerScheme.res", root);
    path[sizeof(path) - 1] = '\0';
    ApplyFile(path, out);
}

static OverlayTheme g_uiTheme;
static int g_uiThemeLoaded = 0;

void UiTheme_Reload(void)
{
    OverlayTheme_Load(&g_uiTheme);
    g_uiThemeLoaded = 1;
}

const OverlayTheme *UiTheme_Current(void)
{
    if (!g_uiThemeLoaded) {
        UiTheme_Reload();
    }
    return &g_uiTheme;
}
