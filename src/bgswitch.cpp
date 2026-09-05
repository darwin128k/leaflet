#include "bgswitch.h"
#include "log.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* Every tile set actually present under cstrike/resource/background, each
 * cut into <=256px blocks (the hard texture-size ceiling this engine's
 * background loader respects -- confirmed empirically: anything bigger
 * silently fails to draw at all). Two families: 4:3-ish (wide=0) and
 * 16:9 (wide=1). Kept sorted ascending by width within each family so the
 * "smallest that still covers the screen" scan below can just take the
 * first match. */
typedef struct {
    const char *prefix;
    int w;
    int h;
    int wide;
} BgBucket;

static const BgBucket kBuckets[] = {
    {"640",       640,  480,  0},
    {"800",       800,  600,  0},
    {"1024",      1024, 768,  0},
    {"1280",      1280, 1024, 0},
    {"1600x1200", 1600, 1200, 0},
    {"1280x720",  1280, 720,  1},
    {"1366x768",  1366, 768,  1},
    {"1600x900",  1600, 900,  1},
    {"1920",      1920, 1080, 1},
    {"2560x1440", 2560, 1440, 1},
};
#define NUM_BUCKETS (sizeof(kBuckets) / sizeof(kBuckets[0]))

#define WIDESCREEN_ASPECT_THRESHOLD 1.5999f

/* Picks the smallest same-family bucket whose native width still covers
 * the actual screen (so the tiles get downscaled, never upscaled --
 * downscaling a sharp source always looks better). Falls back to the
 * largest same-family bucket if the screen is wider than anything we
 * have baked. */
static const BgBucket *PickBucket(int screenW, int isWidescreen)
{
    const BgBucket *smallestCovering = NULL;
    const BgBucket *largestOverall = NULL;

    for (size_t i = 0; i < NUM_BUCKETS; i++) {
        const BgBucket *b = &kBuckets[i];
        if (b->wide != isWidescreen) {
            continue;
        }
        if (largestOverall == NULL || b->w > largestOverall->w) {
            largestOverall = b;
        }
        if (b->w >= screenW && (smallestCovering == NULL || b->w < smallestCovering->w)) {
            smallestCovering = b;
        }
    }

    return (smallestCovering != NULL) ? smallestCovering : largestOverall;
}

static const char kColLetters[] = "abcdefghijklmnop";

static int BlockSizes(int total, int block, int *outSizes, int maxCount)
{
    int count = 0;
    int remaining = total;
    while (remaining > 0 && count < maxCount) {
        int s = (remaining < block) ? remaining : block;
        outSizes[count++] = s;
        remaining -= s;
    }
    return count;
}

static int WriteOneLayoutFile(const char *path, const char *prefix, int baseW, int baseH, const char *suffix)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        HookLog("BgSwitch: failed to open '%s' for writing, errno-ish GetLastError=%lu", path, GetLastError());
        return 0;
    }

    int colSizes[16];
    int rowSizes[16];
    int colCount = BlockSizes(baseW, 256, colSizes, 16);
    int rowCount = BlockSizes(baseH, 256, rowSizes, 16);

    fprintf(f, "resolution\t%d\t%d\r\n\r\n", baseW, baseH);

    int y = 0;
    for (int ri = 0; ri < rowCount; ri++) {
        int x = 0;
        for (int ci = 0; ci < colCount; ci++) {
            fprintf(f, "resource/background/%s_%d_%c%s.tga\t\tscaled\t\t%d\t%d\r\n",
                    prefix, ri + 1, kColLetters[ci], suffix, x, y);
            x += colSizes[ci];
        }
        fprintf(f, "\r\n");
        y += rowSizes[ri];
    }

    fclose(f);
    return 1;
}

/* GetSystemMetrics reports the DESKTOP's current display mode, which does
 * NOT change when the player picks a different in-game resolution on this
 * build (confirmed empirically: it read the same 2560x1440 across many
 * launches at different selected resolutions -- this engine build doesn't
 * do a real exclusive-fullscreen mode switch, it just renders internally
 * at the chosen size). The actual chosen resolution is instead persisted
 * by the engine itself in the registry (has to be, for it to survive the
 * "apply video settings" restart) under the classic HL1 settings key. */
static int ReadRegistryResolution(int *outW, int *outH)
{
    HKEY hKey;
    DWORD w = 0, h = 0, sz, type;
    LONG rc;

    rc = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Half-Life\\Settings", 0, KEY_QUERY_VALUE, &hKey);
    if (rc != ERROR_SUCCESS) {
        HookLog("BgSwitch: RegOpenKeyExA failed, rc=%ld", rc);
        return 0;
    }

    sz = sizeof(DWORD);
    rc = RegQueryValueExA(hKey, "ScreenWidth", NULL, &type, (LPBYTE)&w, &sz);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) {
        HookLog("BgSwitch: ScreenWidth read failed, rc=%ld type=%lu", rc, type);
        RegCloseKey(hKey);
        return 0;
    }

    sz = sizeof(DWORD);
    rc = RegQueryValueExA(hKey, "ScreenHeight", NULL, &type, (LPBYTE)&h, &sz);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_DWORD) {
        HookLog("BgSwitch: ScreenHeight read failed, rc=%ld type=%lu", rc, type);
        return 0;
    }

    if (w == 0 || h == 0) {
        return 0;
    }

    *outW = (int)w;
    *outH = (int)h;
    return 1;
}

static char g_gameRoot[MAX_PATH];
static int g_gameRootSet = 0;
static int g_ranOnce = 0;

void BgSwitch_SetGameRoot(const char *gameRootDir)
{
    lstrcpynA(g_gameRoot, gameRootDir, MAX_PATH);
    g_gameRootSet = 1;
}

const char *BgSwitch_GetGameRoot(void)
{
    return g_gameRootSet ? g_gameRoot : NULL;
}

void BgSwitch_Reset(void)
{
    g_ranOnce = 0;
}

void BgSwitch_RunOnceIfNeeded(void)
{
    if (g_ranOnce || !g_gameRootSet) {
        return;
    }
    g_ranOnce = 1;

    const char *gameRootDir = g_gameRoot;

    int screenW, screenH;
    const char *source;
    if (ReadRegistryResolution(&screenW, &screenH)) {
        source = "registry";
    } else {
        screenW = GetSystemMetrics(SM_CXSCREEN);
        screenH = GetSystemMetrics(SM_CYSCREEN);
        source = "GetSystemMetrics-fallback";
    }
    if (screenW <= 0 || screenH <= 0) {
        HookLog("BgSwitch: no usable resolution (%s gave %dx%d), skipping", source, screenW, screenH);
        return;
    }

    float aspect = (float)screenW / (float)screenH;
    int isWidescreen = aspect >= WIDESCREEN_ASPECT_THRESHOLD;

    const BgBucket *bucket = PickBucket(screenW, isWidescreen);
    if (bucket == NULL) {
        HookLog("BgSwitch: no bucket available for wide=%d, skipping", isWidescreen);
        return;
    }
    const char *prefix = bucket->prefix;
    int baseW = bucket->w;
    int baseH = bucket->h;

    HookLog("BgSwitch: resolution(%s)=%dx%d aspect=%.3f -> %s bucket '%s' (%dx%d)",
            source, screenW, screenH, aspect, isWidescreen ? "widescreen" : "4:3", prefix, baseW, baseH);

    char pathMenu[MAX_PATH];
    char pathLoading[MAX_PATH];
    wsprintfA(pathMenu, "%s\\cstrike\\resource\\BackgroundLayout.txt", gameRootDir);
    wsprintfA(pathLoading, "%s\\cstrike\\resource\\BackgroundLoadingLayout.txt", gameRootDir);

    int ok1 = WriteOneLayoutFile(pathMenu, prefix, baseW, baseH, "");
    int ok2 = WriteOneLayoutFile(pathLoading, prefix, baseW, baseH, "_loading");
    HookLog("BgSwitch: wrote menu=%d loading=%d", ok1, ok2);
}
