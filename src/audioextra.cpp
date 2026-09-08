#include "audioextra.h"
#include "roundframe.h"
#include "log.h"
#include "bgswitch.h"
#include <stdio.h>
#include <string.h>

typedef void(__thiscall *SetPosFn)(void *self, int x, int y);
typedef void *(__thiscall *FindChildByNameFn)(void *self, const char *name, int recurse);
typedef void(__thiscall *SetVisibleFn)(void *self, unsigned char visible);
typedef char(__thiscall *IsSelectedFn)(void *self);
typedef void *(*GetCvarPointerFn)(const char *name);
typedef float (*GetCvarFloatFn)(const char *name);
typedef void (*CvarSetValueFn)(const char *name, float value);
typedef void(__thiscall *ComboActivateFn)(void *self, int index);
typedef void(__thiscall *PaintFn)(void *self);

#define RVA_FINDCHILDBYNAME 0x00044100u
#define RVA_SETPOS          0x000436f0u
#define RVA_ENGINE          0x000C3C9Cu /* cl_enginefunc_t *engine; CCvarSlider::Paint mov ecx,[imm] */
#define RVA_COMBO_ACTIVATE  0x00031310u /* CLabeledCommandComboBox::ActivateItem(int) */
#define RVA_CCVARSLIDER_VT  0x00097a3cu
#define VT_PAINT_INDEX      107
#define OFF_SETVISIBLE_VT   0x74 /* Panel::SetVisible; CCvarSlider slot 0x70 is the deleting dtor */
#define OFF_BUTTON_ISSELECTED_VT 0x2b8
#define OFF_PANEL_NAME      0x44
#define OFF_CCVAR_NAME      0xC8 /* CCvarSlider::m_szCvarName; ctor lea edx,[esi+0xC8] */
#define CCVAR_NAME_SIZE     64
#define ENG_GETCVARFLOAT    15
#define ENG_CVAR_SETVALUE   37
#define ENG_GETCVARPOINTER  72
#define OFF_SLIDER_MIN      0x8C
#define OFF_SLIDER_MAX      0x90
#define OFF_SLIDER_VALUE    0x94
#define OFF_SLIDER_DRAGGING 0x71
#define DOPPLER_SCALE       100.0f
#define DOPPLER_MAX         2.0f

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
    { "al_xfi_workaround", "al_xfi_workaround", 1.0f },
    { "al_clamping_mode", "al_clamping_mode", 1.0f },
};

#define TOGGLE_COUNT (int)(sizeof(kToggles) / sizeof(kToggles[0]))

static BYTE *g_gameUiBase = NULL;
static FindChildByNameFn g_FindChild = NULL;
static SetPosFn g_SetPos = NULL;
static ComboActivateFn g_ComboActivate = NULL;
static PaintFn g_origCvarSliderPaint = NULL;
static void *g_audioPage = NULL;
static unsigned char g_seeded[TOGGLE_COUNT];
static unsigned char g_lastUi[TOGGLE_COUNT];
static void *g_toggleBtn[TOGGLE_COUNT];
static int g_dopplerSeeded = 0;
static int g_lastDoppler = -1;
static int g_dopplerDirty = 0;
static void *g_dopplerSlider = NULL;

/* Mixer-backed Voice controls (transmit volume + MicBoost) are not cvars.
 * GameUI rereads Windows mixer on each Options open and gets 1.0 / on.
 * Keep last user values in a sidecar file and push them onto the widgets. */
static const char kVoiceTweakFile[] = "voice_tweak.cfg";
static int g_voiceFileOk = 0;
static float g_micVol = 1.0f;
static int g_micBoost = 1;
static int g_micVolDirty = 0;
static void *g_voicePage = NULL;
static void *g_micSlider = NULL;
static void *g_micBoostBtn = NULL;

static void VoiceTweakPath(char *out, int outSize)
{
    const char *root = BgSwitch_GetGameRoot();
    if (root != NULL && root[0] != '\0') {
        _snprintf(out, outSize, "%s\\voice_tweak.cfg", root);
    } else {
        lstrcpynA(out, kVoiceTweakFile, outSize);
    }
    out[outSize - 1] = '\0';
}

static void LoadVoiceTweakFile(void)
{
    FILE *f;
    char line[128];
    char path[MAX_PATH];
    VoiceTweakPath(path, sizeof(path));
    f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        float vol;
        int boost;
        if (sscanf(line, "micvol %f", &vol) == 1) {
            if (vol < 0.0f) {
                vol = 0.0f;
            }
            if (vol > 1.0f) {
                vol = 1.0f;
            }
            g_micVol = vol;
            g_voiceFileOk = 1;
        } else if (sscanf(line, "boost %d", &boost) == 1) {
            g_micBoost = boost ? 1 : 0;
            g_voiceFileOk = 1;
        }
    }
    fclose(f);
}

static void SaveVoiceTweakFile(void)
{
    char path[MAX_PATH];
    FILE *f;
    VoiceTweakPath(path, sizeof(path));
    f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "micvol %.4f\nboost %d\n", g_micVol, g_micBoost);
    fclose(f);
    g_voiceFileOk = 1;
}

static int IsMicVolumeSlider(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    return lstrcmpiA(name, "#GameUI_MicrophoneVolume") == 0
        || lstrcmpiA(name, "Microphone Volume") == 0;
}

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

static void __fastcall CvarSliderPaint_Hook(void *self)
{
    const char *name = PanelName(self);
    /* Stock CCvarSlider::Paint reads the cvar every frame and writes the
     * knob. For the HEV slider we retargeted to al_doppler that fight the
     * mouse (0–1 suit scale vs 0–2), so skip it and only draw our track. */
    if (lstrcmpiA(name, "Suit Slider") == 0 || lstrcmpiA(name, "al_doppler") == 0) {
        RoundFrame_PaintOptionsSlider(self);
        return;
    }
    if (g_origCvarSliderPaint != NULL) {
        g_origCvarSliderPaint(self);
    }
}

void AudioExtra_OnSliderPaint(void *slider)
{
    const char *name;
    int minv;
    int maxv;
    int cur;
    int want;
    int dragging;
    if (slider == NULL || IsBadReadPtr((char *)slider + OFF_SLIDER_VALUE, 4)) {
        return;
    }
    name = PanelName(slider);
    if (IsMicVolumeSlider(name)) {
        if (!g_voiceFileOk) {
            LoadVoiceTweakFile();
        }
        dragging = 0;
        if (!IsBadReadPtr((char *)slider + OFF_SLIDER_DRAGGING, 1)) {
            dragging = *((unsigned char *)slider + OFF_SLIDER_DRAGGING) != 0;
        }
        cur = *(int *)((char *)slider + OFF_SLIDER_VALUE);
        if (cur < 0) {
            cur = 0;
        }
        if (cur > 100) {
            cur = 100;
        }
        if (g_micSlider != slider) {
            g_micSlider = slider;
            if (g_voiceFileOk) {
                want = (int)(g_micVol * 100.0f + 0.5f);
                if (want < 0) {
                    want = 0;
                }
                if (want > 100) {
                    want = 100;
                }
                *(int *)((char *)slider + OFF_SLIDER_VALUE) = want;
            }
            g_micVolDirty = 0;
            return;
        }
        /* Track clicks jump the value without _dragging. Do not write the
         * saved value back every frame — that eats those clicks. */
        if (cur != (int)(g_micVol * 100.0f + 0.5f)) {
            g_micVol = (float)cur / 100.0f;
            g_micVolDirty = 1;
        }
        if (g_micVolDirty && !dragging) {
            SaveVoiceTweakFile();
            g_micVolDirty = 0;
        }
        return;
    }
    if (lstrcmpiA(name, "al_doppler") != 0 && lstrcmpiA(name, "Suit Slider") != 0) {
        return;
    }
    if (!CvarExists("al_doppler")) {
        return;
    }
    minv = 0;
    maxv = (int)(DOPPLER_MAX * DOPPLER_SCALE + 0.5f);
    *(int *)((char *)slider + OFF_SLIDER_MIN) = minv;
    *(int *)((char *)slider + OFF_SLIDER_MAX) = maxv;
    want = (int)(CvarGet("al_doppler") * DOPPLER_SCALE + 0.5f);
    if (want < minv) {
        want = minv;
    }
    if (want > maxv) {
        want = maxv;
    }
    if (g_dopplerSlider != slider) {
        *(int *)((char *)slider + OFF_SLIDER_VALUE) = want;
        g_lastDoppler = want;
        g_dopplerDirty = 0;
        g_dopplerSeeded = 1;
        g_dopplerSlider = slider;
        return;
    }
    dragging = 0;
    if (!IsBadReadPtr((char *)slider + OFF_SLIDER_DRAGGING, 1)) {
        dragging = *((unsigned char *)slider + OFF_SLIDER_DRAGGING) != 0;
    }
    cur = *(int *)((char *)slider + OFF_SLIDER_VALUE);
    if (cur < minv) {
        cur = minv;
    }
    if (cur > maxv) {
        cur = maxv;
    }
    if (dragging) {
        g_lastDoppler = cur;
        g_dopplerDirty = 1;
        return;
    }
    if (g_dopplerDirty) {
        CvarSet("al_doppler", (float)cur / DOPPLER_SCALE);
        g_lastDoppler = cur;
        g_dopplerDirty = 0;
        return;
    }
    if (cur != want) {
        *(int *)((char *)slider + OFF_SLIDER_VALUE) = want;
        g_lastDoppler = want;
    }
}

void AudioExtra_Init(HMODULE hGameUI)
{
    memset(g_seeded, 0, sizeof(g_seeded));
    memset(g_lastUi, 0, sizeof(g_lastUi));
    memset(g_toggleBtn, 0, sizeof(g_toggleBtn));
    g_dopplerSeeded = 0;
    g_lastDoppler = -1;
    g_dopplerDirty = 0;
    g_dopplerSlider = NULL;
    g_audioPage = NULL;
    g_voicePage = NULL;
    g_micSlider = NULL;
    g_micBoostBtn = NULL;
    g_micVolDirty = 0;
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
    {
        void **vt;
        DWORD oldProtect;
        vt = (void **)(g_gameUiBase + RVA_CCVARSLIDER_VT);
        if (!IsBadReadPtr(vt, (VT_PAINT_INDEX + 1) * sizeof(void *))) {
            if (VirtualProtect(&vt[VT_PAINT_INDEX], sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                g_origCvarSliderPaint = (PaintFn)vt[VT_PAINT_INDEX];
                vt[VT_PAINT_INDEX] = (void *)CvarSliderPaint_Hook;
                VirtualProtect(&vt[VT_PAINT_INDEX], sizeof(void *), oldProtect, &oldProtect);
            }
        }
    }
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
    if (lstrcmpiA(name, "MicBoost") == 0) {
        if (!g_voiceFileOk) {
            LoadVoiceTweakFile();
        }
        ui = ReadSelected(btn);
        if (g_micBoostBtn != btn) {
            g_micBoostBtn = btn;
            if (g_voiceFileOk) {
                WriteSelected(btn, g_micBoost);
            } else {
                g_micBoost = ui ? 1 : 0;
            }
            return;
        }
        if (ui != g_micBoost) {
            g_micBoost = ui ? 1 : 0;
            SaveVoiceTweakFile();
        } else if (g_voiceFileOk) {
            WriteSelected(btn, g_micBoost);
        }
        return;
    }
    for (i = 0; i < TOGGLE_COUNT; i++) {
        if (lstrcmpiA(name, kToggles[i].field) != 0) {
            continue;
        }
        if (!CvarExists(kToggles[i].cvar)) {
            return;
        }
        engOn = CvarGet(kToggles[i].cvar) > 0.01f;
        /* A new Options dialog allocates new check buttons (all off). If we
         * still think the last dialog's "on" was the UI, paint would write
         * hisound 0 the moment you reopen. Seed from the engine instead. */
        if (!g_seeded[i] || g_toggleBtn[i] != btn) {
            WriteSelected(btn, engOn);
            g_lastUi[i] = (unsigned char)engOn;
            g_seeded[i] = 1;
            g_toggleBtn[i] = btn;
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

void AudioExtra_BindVoicePage(void *voicePage)
{
    if (voicePage == NULL) {
        return;
    }
    if (!g_voiceFileOk) {
        LoadVoiceTweakFile();
    }
    if (g_voicePage != voicePage) {
        g_voicePage = voicePage;
        g_micSlider = NULL;
        g_micBoostBtn = NULL;
        g_micVolDirty = 0;
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
            HideNamed(audioPage, "al_doppler_label");
            HideNamed(audioPage, "al_xfi_workaround");
            HideNamed(audioPage, "al_clamping_mode");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void AudioExtra_EnsureDopplerSlider(void *audioPage)
{
    void *sl;
    char *cvar;
    void **vt;
    SetVisibleFn vis;
    if (audioPage == NULL) {
        return;
    }
    /* CS hides the HEV suit slider; it is already a CCvarSlider 0–2.
     * Retarget it at al_doppler instead of constructing a new widget. */
    sl = FindChild(audioPage, "Suit Slider");
    if (sl == NULL) {
        return;
    }
    cvar = (char *)sl + OFF_CCVAR_NAME;
    if (IsBadWritePtr(cvar, CCVAR_NAME_SIZE)) {
        return;
    }
    lstrcpynA(cvar, "al_doppler", CCVAR_NAME_SIZE);
    vt = *(void ***)sl;
    if (vt != NULL) {
        vis = (SetVisibleFn)vt[OFF_SETVISIBLE_VT / sizeof(void *)];
        if (vis != NULL) {
            __try {
                vis(sl, 1);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
}

int AudioExtra_HasMetaAudio(void)
{
    return CvarExists("al_occlusion");
}

void AudioExtra_Tick(void)
{
}
