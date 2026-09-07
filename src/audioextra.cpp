#include "audioextra.h"
#include "log.h"
#include <string.h>

typedef void(__thiscall *SetPosFn)(void *self, int x, int y);
typedef void *(__thiscall *FindChildByNameFn)(void *self, const char *name, int recurse);
typedef void(__thiscall *SetVisibleFn)(void *self, unsigned char visible);
typedef char(__thiscall *IsSelectedFn)(void *self);
typedef void *(*GetCvarPointerFn)(const char *name);
typedef float (*GetCvarFloatFn)(const char *name);
typedef void (*CvarSetValueFn)(const char *name, float value);
typedef void(__thiscall *ComboActivateFn)(void *self, int index);

#define RVA_FINDCHILDBYNAME 0x00044100u
#define RVA_SETPOS          0x000436f0u
#define RVA_ENGINE          0x000C3C9Cu /* cl_enginefunc_t *engine; CCvarSlider::Paint mov ecx,[imm] */
#define RVA_COMBO_ACTIVATE  0x00031310u /* CLabeledCommandComboBox::ActivateItem(int) */
#define OFF_SETVISIBLE_VT   0x70
#define OFF_BUTTON_ISSELECTED_VT 0x2b8
#define OFF_PANEL_NAME      0x44
#define ENG_GETCVARFLOAT    15
#define ENG_CVAR_SETVALUE   37
#define ENG_GETCVARPOINTER  72

typedef struct {
    const char *field;
    const char *cvar;
    float onValue;
} AudioToggle;

static const AudioToggle kToggles[] = {
    { "hisound", "hisound", 1.0f },
    { "al_occlusion", "al_occlusion", 1.0f },
    { "al_occlusion_fade", "al_occlusion_fade", 1.0f },
    { "al_resample_all", "al_resample_all", 1.0f },
    { "al_doppler", "al_doppler", 0.3f },
    { "al_xfi_workaround", "al_xfi_workaround", 1.0f },
    { "al_clamping_mode", "al_clamping_mode", 1.0f },
};

#define TOGGLE_COUNT (int)(sizeof(kToggles) / sizeof(kToggles[0]))

static BYTE *g_gameUiBase = NULL;
static FindChildByNameFn g_FindChild = NULL;
static SetPosFn g_SetPos = NULL;
static ComboActivateFn g_ComboActivate = NULL;
static void *g_audioPage = NULL;
static unsigned char g_seeded[TOGGLE_COUNT];
static unsigned char g_lastUi[TOGGLE_COUNT];

static void **EngineTable(void)
{
    void **eng;
    if (g_gameUiBase == NULL || IsBadReadPtr(g_gameUiBase + RVA_ENGINE, sizeof(void *))) {
        return NULL;
    }
    eng = *(void ***)(g_gameUiBase + RVA_ENGINE);
    if (eng == NULL || IsBadReadPtr(eng, (ENG_GETCVARPOINTER + 1) * sizeof(void *))) {
        return NULL;
    }
    return eng;
}

static int CvarExists(const char *name)
{
    void **eng = EngineTable();
    GetCvarPointerFn getPtr;
    if (eng == NULL || name == NULL) {
        return 0;
    }
    getPtr = (GetCvarPointerFn)eng[ENG_GETCVARPOINTER];
    if (getPtr == NULL) {
        return 0;
    }
    __try {
        return getPtr(name) != NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static float CvarGet(const char *name)
{
    void **eng = EngineTable();
    GetCvarFloatFn getf;
    if (eng == NULL) {
        return 0.0f;
    }
    getf = (GetCvarFloatFn)eng[ENG_GETCVARFLOAT];
    if (getf == NULL) {
        return 0.0f;
    }
    __try {
        return getf(name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
}

static void CvarSet(const char *name, float value)
{
    void **eng = EngineTable();
    CvarSetValueFn setv;
    if (eng == NULL) {
        return;
    }
    setv = (CvarSetValueFn)eng[ENG_CVAR_SETVALUE];
    if (setv == NULL) {
        return;
    }
    __try {
        setv(name, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static const char *PanelName(void *panel)
{
    const char *name;
    if (panel == NULL || IsBadReadPtr(panel, OFF_PANEL_NAME + sizeof(void *))) {
        return "";
    }
    name = *(const char **)((char *)panel + OFF_PANEL_NAME);
    if (name == NULL || IsBadReadPtr(name, 1)) {
        return "";
    }
    return name;
}

static int ReadSelected(void *btn)
{
    void **vt;
    IsSelectedFn fn;
    if (btn == NULL) {
        return 0;
    }
    vt = *(void ***)btn;
    if (vt == NULL) {
        return 0;
    }
    fn = (IsSelectedFn)vt[OFF_BUTTON_ISSELECTED_VT / sizeof(void *)];
    if (fn == NULL) {
        return 0;
    }
    __try {
        return fn(btn) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void WriteSelected(void *btn, int on)
{
    void **vt;
    unsigned char *fn;
    int off = -1;
    if (btn == NULL) {
        return;
    }
    vt = *(void ***)btn;
    if (vt == NULL) {
        return;
    }
    fn = (unsigned char *)vt[OFF_BUTTON_ISSELECTED_VT / sizeof(void *)];
    if (fn == NULL || IsBadReadPtr(fn, 6)) {
        return;
    }
    if (fn[0] == 0x8A && fn[1] == 0x41) {
        off = fn[2];
    } else if (fn[0] == 0x8A && fn[1] == 0x81) {
        off = *(int *)(fn + 2);
    }
    if (off < 0 || off > 0x200) {
        return;
    }
    if (IsBadWritePtr((char *)btn + off, 1)) {
        return;
    }
    *((unsigned char *)btn + off) = on ? 1 : 0;
}

static void *FindChild(void *page, const char *name)
{
    if (page == NULL || name == NULL || g_FindChild == NULL) {
        return NULL;
    }
    __try {
        return g_FindChild(page, name, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}

static void HideNamed(void *page, const char *name)
{
    void *c;
    void **vt;
    SetVisibleFn vis;
    c = FindChild(page, name);
    if (c == NULL) {
        return;
    }
    if (!IsBadReadPtr(c, sizeof(void *))) {
        vt = *(void ***)c;
        if (vt != NULL) {
            vis = (SetVisibleFn)vt[OFF_SETVISIBLE_VT / sizeof(void *)];
            if (vis != NULL) {
                __try {
                    vis(c, 0);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
    }
    if (g_SetPos != NULL) {
        __try {
            g_SetPos(c, -4000, -4000);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

static void SyncHiddenQualityCombo(int highOn)
{
    void *combo;
    static const BYTE kPrologue[8] = { 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7C, 0x24, 0x0C };
    if (g_audioPage == NULL || g_ComboActivate == NULL) {
        return;
    }
    if (memcmp((const BYTE *)g_ComboActivate, kPrologue, sizeof(kPrologue)) != 0) {
        return;
    }
    combo = FindChild(g_audioPage, "Sound Quality");
    if (combo == NULL) {
        return;
    }
    __try {
        g_ComboActivate(combo, highOn ? 0 : 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void AudioExtra_Init(HMODULE hGameUI)
{
    memset(g_seeded, 0, sizeof(g_seeded));
    memset(g_lastUi, 0, sizeof(g_lastUi));
    g_audioPage = NULL;
    g_gameUiBase = (BYTE *)hGameUI;
    g_FindChild = NULL;
    g_SetPos = NULL;
    g_ComboActivate = NULL;
    if (hGameUI == NULL) {
        return;
    }
    g_FindChild = (FindChildByNameFn)(g_gameUiBase + RVA_FINDCHILDBYNAME);
    g_SetPos = (SetPosFn)(g_gameUiBase + RVA_SETPOS);
    g_ComboActivate = (ComboActivateFn)(g_gameUiBase + RVA_COMBO_ACTIVATE);
}

void AudioExtra_SyncToggle(void *btn)
{
    const char *name;
    int i;
    int ui;
    int engOn;
    if (btn == NULL) {
        return;
    }
    name = PanelName(btn);
    for (i = 0; i < TOGGLE_COUNT; i++) {
        if (lstrcmpiA(name, kToggles[i].field) != 0) {
            continue;
        }
        if (!CvarExists(kToggles[i].cvar)) {
            return;
        }
        engOn = CvarGet(kToggles[i].cvar) > 0.01f;
        if (!g_seeded[i]) {
            WriteSelected(btn, engOn);
            g_lastUi[i] = (unsigned char)engOn;
            g_seeded[i] = 1;
            if (i == 0) {
                SyncHiddenQualityCombo(engOn);
            }
            return;
        }
        ui = ReadSelected(btn);
        if (ui != (int)g_lastUi[i]) {
            CvarSet(kToggles[i].cvar, ui ? kToggles[i].onValue : 0.0f);
            g_lastUi[i] = (unsigned char)ui;
            if (lstrcmpiA(kToggles[i].field, "hisound") == 0) {
                SyncHiddenQualityCombo(ui);
            }
        } else if (engOn != (int)g_lastUi[i]) {
            WriteSelected(btn, engOn);
            g_lastUi[i] = (unsigned char)engOn;
            if (i == 0) {
                SyncHiddenQualityCombo(engOn);
            }
        }
        return;
    }
}

void AudioExtra_BindPage(void *audioPage)
{
    if (audioPage == NULL) {
        return;
    }
    __try {
        HideNamed(audioPage, "MilesAudioLabel");
        HideNamed(audioPage, "EAX");
        HideNamed(audioPage, "A3D");
        HideNamed(audioPage, "Sound Quality");
        HideNamed(audioPage, "Label1");
        HideNamed(audioPage, "OpenAL Label");
        g_audioPage = audioPage;
        if (!CvarExists("al_occlusion")) {
            HideNamed(audioPage, "al_occlusion");
            HideNamed(audioPage, "al_occlusion_fade");
            HideNamed(audioPage, "al_resample_all");
            HideNamed(audioPage, "al_doppler");
            HideNamed(audioPage, "al_xfi_workaround");
            HideNamed(audioPage, "al_clamping_mode");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int AudioExtra_HasMetaAudio(void)
{
    return CvarExists("al_occlusion");
}

void AudioExtra_Tick(void)
{
}
