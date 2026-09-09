#include "ui_caps.h"

typedef void *(*GetCvarPointerFn)(const char *name);

#define RVA_ENGINE 0x000C3C9Cu
#define ENG_GETCVARPOINTER 72

static UiCaps g_caps;
static BYTE *g_gameUiBase = NULL;

static void **EngineFns(void)
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

static int EngCvarExists(const char *name)
{
    void **eng = EngineFns();
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

void UiCaps_Init(HMODULE hGameUI)
{
    g_gameUiBase = (BYTE *)hGameUI;
    UiCaps_Refresh();
}

void UiCaps_Refresh(void)
{
    g_caps.metaAudio = EngCvarExists("al_occlusion");
    g_caps.metaVoice = EngCvarExists("mv_gain");
}

const UiCaps *UiCaps_Get(void)
{
    return &g_caps;
}
