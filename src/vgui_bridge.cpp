#include "vgui_bridge.h"
#include <string.h>

typedef void(__thiscall *SetPosFn)(void *self, int x, int y);
typedef void(__thiscall *GetPosFn)(void *self, int *x, int *y);
typedef void(__thiscall *SetSizeFn)(void *self, int wide, int tall);
typedef void(__thiscall *GetSizeFn)(void *self, int *wide, int *tall);
typedef void *(__thiscall *FindChildByNameFn)(void *self, const char *name, int recurse);
typedef int (__thiscall *GetChildCountFn)(void *self);
typedef void *(__thiscall *GetChildFn)(void *self, int index);
typedef void(__thiscall *SetVisibleFn)(void *self, unsigned char visible);
typedef void(__thiscall *SetEnabledFn)(void *self, unsigned char enabled);
typedef void(__thiscall *AddActionSignalTargetFn)(void *self, void *target);

#define RVA_FINDCHILDBYNAME 0x00044100u
#define RVA_GETCHILDCOUNT   0x00046260u
#define RVA_GETCHILD        0x00046280u
#define RVA_SETPOS          0x000436f0u
#define RVA_GETPOS          0x00043720u
#define RVA_SETSIZE         0x00043750u
#define RVA_GETSIZE         0x00043780u
#define OFF_PANEL_NAME      0x44
#define OFF_SETVISIBLE_VT   0x74
#define OFF_SETENABLED_VT   0xBC
#define OFF_ADDACTIONTARGET_VT 0xA4

static BYTE *g_base = NULL;
static FindChildByNameFn g_FindChildByName = NULL;
static GetChildCountFn g_GetChildCount = NULL;
static GetChildFn g_GetChild = NULL;
static SetPosFn g_SetPos = NULL;
static GetPosFn g_GetPos = NULL;
static SetSizeFn g_SetSize = NULL;
static GetSizeFn g_GetSize = NULL;

void VguiBridge_Init(HMODULE hGameUI)
{
    BYTE *base = (BYTE *)hGameUI;
    static const BYTE kFindChildPrologue[6] = { 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9 };
    static const BYTE kChildCountPrologue[4] = { 0x53, 0x56, 0x57, 0x8B };
    static const BYTE kGetChildPrologue[6] = { 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF1 };

    g_base = base;
    g_FindChildByName = NULL;
    g_GetChildCount = NULL;
    g_GetChild = NULL;
    if (base == NULL) {
        g_SetPos = NULL;
        g_GetPos = NULL;
        g_SetSize = NULL;
        g_GetSize = NULL;
        return;
    }
    g_SetPos = (SetPosFn)(base + RVA_SETPOS);
    g_GetPos = (GetPosFn)(base + RVA_GETPOS);
    g_SetSize = (SetSizeFn)(base + RVA_SETSIZE);
    g_GetSize = (GetSizeFn)(base + RVA_GETSIZE);
    if (memcmp(base + RVA_FINDCHILDBYNAME, kFindChildPrologue, 6) == 0) {
        g_FindChildByName = (FindChildByNameFn)(base + RVA_FINDCHILDBYNAME);
    }
    if (memcmp(base + RVA_GETCHILDCOUNT, kChildCountPrologue, 4) == 0) {
        g_GetChildCount = (GetChildCountFn)(base + RVA_GETCHILDCOUNT);
    }
    if (memcmp(base + RVA_GETCHILD, kGetChildPrologue, 6) == 0) {
        g_GetChild = (GetChildFn)(base + RVA_GETCHILD);
    }
}

int VguiBridge_Ready(void)
{
    return g_base != NULL && g_SetPos != NULL && g_SetSize != NULL;
}

BYTE *VguiBridge_GameUiBase(void)
{
    return g_base;
}

const char *VguiBridge_PanelName(void *panel)
{
    const char *n;
    if (panel == NULL || IsBadReadPtr((char *)panel + OFF_PANEL_NAME, sizeof(void *))) {
        return "";
    }
    n = *(const char **)((char *)panel + OFF_PANEL_NAME);
    if (n == NULL || IsBadStringPtrA(n, 128)) {
        return "";
    }
    return n;
}

void *VguiBridge_FindChild(void *parent, const char *name)
{
    int n;
    int i;
    if (parent == NULL || name == NULL) {
        return NULL;
    }
    if (g_FindChildByName != NULL) {
        void *found = g_FindChildByName(parent, name, 1);
        if (found != NULL) {
            return found;
        }
    }
    if (g_GetChildCount == NULL || g_GetChild == NULL) {
        return NULL;
    }
    n = g_GetChildCount(parent);
    if (n < 0 || n > 128) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        void *child = g_GetChild(parent, i);
        if (child == NULL) {
            continue;
        }
        if (lstrcmpiA(VguiBridge_PanelName(child), name) == 0) {
            return child;
        }
    }
    return NULL;
}

void VguiBridge_SetPos(void *panel, int x, int y)
{
    if (panel == NULL || g_SetPos == NULL) {
        return;
    }
    g_SetPos(panel, x, y);
}

void VguiBridge_GetPos(void *panel, int *x, int *y)
{
    if (x != NULL) {
        *x = 0;
    }
    if (y != NULL) {
        *y = 0;
    }
    if (panel == NULL || g_GetPos == NULL) {
        return;
    }
    g_GetPos(panel, x, y);
}

void VguiBridge_SetSize(void *panel, int w, int h)
{
    if (panel == NULL || g_SetSize == NULL) {
        return;
    }
    g_SetSize(panel, w, h);
}

void VguiBridge_GetSize(void *panel, int *w, int *h)
{
    if (w != NULL) {
        *w = 0;
    }
    if (h != NULL) {
        *h = 0;
    }
    if (panel == NULL || g_GetSize == NULL) {
        return;
    }
    g_GetSize(panel, w, h);
}

void VguiBridge_SetVisible(void *panel, int visible)
{
    void **vt;
    SetVisibleFn vis;
    if (panel == NULL || IsBadReadPtr(panel, sizeof(void *))) {
        return;
    }
    vt = *(void ***)panel;
    if (vt == NULL) {
        return;
    }
    vis = (SetVisibleFn)vt[OFF_SETVISIBLE_VT / sizeof(void *)];
    if (vis == NULL) {
        return;
    }
    __try {
        vis(panel, visible ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void VguiBridge_SetEnabled(void *panel, int enabled)
{
    void **vt;
    SetEnabledFn en;
    if (panel == NULL || IsBadReadPtr(panel, sizeof(void *))) {
        return;
    }
    vt = *(void ***)panel;
    if (vt == NULL) {
        return;
    }
    en = (SetEnabledFn)vt[OFF_SETENABLED_VT / sizeof(void *)];
    if (en == NULL) {
        return;
    }
    __try {
        en(panel, enabled ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void VguiBridge_AddActionSignalTarget(void *panel, void *target)
{
    void **vt;
    AddActionSignalTargetFn add;
    if (panel == NULL || target == NULL || IsBadReadPtr(panel, sizeof(void *))) {
        return;
    }
    vt = *(void ***)panel;
    if (vt == NULL) {
        return;
    }
    add = (AddActionSignalTargetFn)vt[OFF_ADDACTIONTARGET_VT / sizeof(void *)];
    if (add == NULL) {
        return;
    }
    __try {
        add(panel, target);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void VguiBridge_Park(void *panel)
{
    if (panel == NULL) {
        return;
    }
    VguiBridge_SetPos(panel, -4000, -4000);
    VguiBridge_SetSize(panel, 1, 1);
}
