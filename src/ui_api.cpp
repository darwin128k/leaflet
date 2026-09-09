#include "ui_api.h"
#include "vgui_bridge.h"
#include "ui_caps.h"

#include <string.h>

typedef void *(__cdecl *GameUiNewFn)(unsigned int);
typedef void (__thiscall *SliderCtorFn)(void *self, void *par, const char *panelName);
typedef void (__thiscall *CCvarSliderCtorFn)(void *self, void *par, const char *panelName,
                                             const char *labelText, float minVal, float maxVal,
                                             const char *cvarName, int extra);
typedef void (__thiscall *LabelCtorFn)(void *self, void *parent, const char *name,
                                      const char *text);
typedef void (__thiscall *ButtonCtorFn)(void *self, void *parent, const char *name,
                                       const char *text);
typedef void (__thiscall *ImagePanelCtorFn)(void *self, void *parent, const char *name);
typedef void (__thiscall *FrameCtorFn)(void *self, void *parent, const char *name,
                                      int showTaskbarIcon);
typedef void (__thiscall *SetTextFn)(void *self, const char *text);
typedef void (__thiscall *SetCommandFn)(void *self, const char *command);
typedef void (__thiscall *SetImageFn)(void *self, void *image);
typedef void (__thiscall *SetTitleFn)(void *self, const char *title, int surfaceTitle);
typedef void *(__cdecl *GetSchemeFn)(void);
typedef void *(__thiscall *SchemeGetImageFn)(void *scheme, const char *path,
                                             int hardwareFiltered);

#define RVA_GAMEUI_NEW       0x0007A483u
#define RVA_SLIDER_CTOR      0x000668C0u
#define RVA_CCVARSLIDER_CTOR 0x000301C0u
#define RVA_LABEL_CTOR       0x00040A80u
#define RVA_LABEL_SETTEXT    0x00041200u
#define RVA_BUTTON_CTOR      0x0003F2D0u
#define RVA_BUTTON_SETCOMMAND 0x000401B0u
#define RVA_IMAGEPANEL_CTOR  0x00072580u
#define RVA_IMAGEPANEL_SETIMAGE 0x000726C0u
#define RVA_FRAME_CTOR       0x0004A610u
#define RVA_FRAME_SETTITLE   0x0004C690u
#define RVA_GETSCHEME        0x0003F030u
#define SLIDER_SIZE          0xB4
#define CCVARSLIDER_SIZE     0x108
#define LABEL_SIZE           0xBC
#define BUTTON_SIZE          0x108
#define IMAGEPANEL_SIZE      0x88
#define FRAME_SIZE           0x110
#define OFF_SLIDER_MIN       0x8C
#define OFF_SLIDER_MAX       0x90
#define OFF_SLIDER_VALUE     0x94
#define OFF_IMAGEPANEL_SCALE 0x80
#define SCHEME_GETIMAGE_VT   0x14

static int g_controlApiReady = 0;

static int MatchCode(BYTE *base, unsigned int rva,
                     const BYTE *expected, unsigned int expectedSize)
{
    return base != NULL
        && !IsBadReadPtr(base + rva, expectedSize)
        && memcmp(base + rva, expected, expectedSize) == 0;
}

void UiApi_Init(HMODULE hGameUI)
{
    static const BYTE kLabelCtor[] = {
        0x8B, 0x44, 0x24, 0x08, 0x53, 0x56
    };
    static const BYTE kButtonCtor[] = {
        0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x54, 0x24, 0x04
    };
    static const BYTE kImageCtor[] = {
        0x8B, 0x44, 0x24, 0x08, 0x53, 0x56
    };
    static const BYTE kFrameCtor[] = {
        0x51, 0x8B, 0x44, 0x24, 0x0C, 0x53
    };
    BYTE *base = (BYTE *)hGameUI;
    VguiBridge_Init(hGameUI);
    UiCaps_Init(hGameUI);
    g_controlApiReady =
        MatchCode(base, RVA_LABEL_CTOR, kLabelCtor, sizeof(kLabelCtor))
        && MatchCode(base, RVA_BUTTON_CTOR, kButtonCtor, sizeof(kButtonCtor))
        && MatchCode(base, RVA_IMAGEPANEL_CTOR, kImageCtor, sizeof(kImageCtor))
        && MatchCode(base, RVA_FRAME_CTOR, kFrameCtor, sizeof(kFrameCtor));
}

void *Ui_Find(void *parent, const char *name)
{
    return VguiBridge_FindChild(parent, name);
}

void Ui_Place(void *panel, int x, int y, int w, int h)
{
    if (panel == NULL) {
        return;
    }
    VguiBridge_SetPos(panel, x, y);
    if (w > 0 && h > 0) {
        VguiBridge_SetSize(panel, w, h);
    }
    VguiBridge_SetVisible(panel, 1);
}

void Ui_Hide(void *parent, const char *name)
{
    void *c = VguiBridge_FindChild(parent, name);
    if (c != NULL) {
        VguiBridge_Park(c);
    }
}

void Ui_Show(void *panel)
{
    VguiBridge_SetVisible(panel, 1);
}

void *Ui_EnsureLabel(void *parent, const char *name)
{
    return Leaflet_CreateLabel(parent, name, "");
}

void *Ui_EnsureSlider(void *parent, const char *name, int minv, int maxv, int value)
{
    GameUiNewFn opnew;
    SliderCtorFn ctor;
    void *sl;
    BYTE *base = VguiBridge_GameUiBase();

    sl = VguiBridge_FindChild(parent, name);
    if (sl != NULL) {
        if (!IsBadReadPtr((char *)sl + OFF_SLIDER_MAX, 4)) {
            *(int *)((char *)sl + OFF_SLIDER_MIN) = minv;
            *(int *)((char *)sl + OFF_SLIDER_MAX) = maxv;
            *(int *)((char *)sl + OFF_SLIDER_VALUE) = value;
        }
        return sl;
    }
    if (parent == NULL || base == NULL || name == NULL || !VguiBridge_Ready()) {
        return NULL;
    }
    opnew = (GameUiNewFn)(base + RVA_GAMEUI_NEW);
    ctor = (SliderCtorFn)(base + RVA_SLIDER_CTOR);
    sl = NULL;
    __try {
        sl = opnew(SLIDER_SIZE);
        if (sl == NULL) {
            return NULL;
        }
        ctor(sl, parent, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    if (IsBadReadPtr((char *)sl + OFF_SLIDER_MAX, 4)) {
        return NULL;
    }
    *(int *)((char *)sl + OFF_SLIDER_MIN) = minv;
    *(int *)((char *)sl + OFF_SLIDER_MAX) = maxv;
    *(int *)((char *)sl + OFF_SLIDER_VALUE) = value;
    VguiBridge_SetSize(sl, 160, 28);
    VguiBridge_SetVisible(sl, 1);
    return sl;
}

static void *AllocateGameUiControl(unsigned int size)
{
    BYTE *base = VguiBridge_GameUiBase();
    GameUiNewFn opnew;
    if (base == NULL || !VguiBridge_Ready() || !g_controlApiReady) {
        return NULL;
    }
    opnew = (GameUiNewFn)(base + RVA_GAMEUI_NEW);
    return opnew(size);
}

void *Leaflet_CreateLabel(void *parent, const char *name, const char *text)
{
    BYTE *base = VguiBridge_GameUiBase();
    SetTextFn setText;
    void *label;
    if (parent == NULL || name == NULL || base == NULL || !g_controlApiReady) {
        return NULL;
    }
    label = VguiBridge_FindChild(parent, name);
    setText = (SetTextFn)(base + RVA_LABEL_SETTEXT);
    if (label != NULL) {
        if (text != NULL) {
            __try {
                setText(label, text);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        return label;
    }
    label = AllocateGameUiControl(LABEL_SIZE);
    if (label == NULL) {
        return NULL;
    }
    __try {
        ((LabelCtorFn)(base + RVA_LABEL_CTOR))(
            label, parent, name, text != NULL ? text : "");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    VguiBridge_SetSize(label, 120, 24);
    VguiBridge_SetVisible(label, 1);
    return label;
}

void *Leaflet_CreateButton(void *parent, const char *name, const char *text,
                           const char *command)
{
    BYTE *base = VguiBridge_GameUiBase();
    void *button;
    if (parent == NULL || name == NULL || base == NULL || !g_controlApiReady) {
        return NULL;
    }
    button = VguiBridge_FindChild(parent, name);
    if (button == NULL) {
        button = AllocateGameUiControl(BUTTON_SIZE);
        if (button == NULL) {
            return NULL;
        }
        __try {
            ((ButtonCtorFn)(base + RVA_BUTTON_CTOR))(
                button, parent, name, text != NULL ? text : "");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return NULL;
        }
        VguiBridge_SetSize(button, 120, 24);
    } else if (text != NULL) {
        __try {
            ((SetTextFn)(base + RVA_LABEL_SETTEXT))(button, text);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    VguiBridge_AddActionSignalTarget(button, parent);
    if (command != NULL && command[0] != '\0') {
        __try {
            ((SetCommandFn)(base + RVA_BUTTON_SETCOMMAND))(button, command);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    VguiBridge_SetVisible(button, 1);
    return button;
}

void *Leaflet_CreateImage(void *parent, const char *name,
                          const char *imagePath, int scaleToFit)
{
    BYTE *base = VguiBridge_GameUiBase();
    void *panel;
    void *scheme;
    void *image;
    void **schemeVtable;
    if (parent == NULL || name == NULL || base == NULL || !g_controlApiReady) {
        return NULL;
    }
    panel = VguiBridge_FindChild(parent, name);
    if (panel == NULL) {
        panel = AllocateGameUiControl(IMAGEPANEL_SIZE);
        if (panel == NULL) {
            return NULL;
        }
        __try {
            ((ImagePanelCtorFn)(base + RVA_IMAGEPANEL_CTOR))(panel, parent, name);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return NULL;
        }
        VguiBridge_SetSize(panel, 64, 64);
    }
    if (!IsBadWritePtr((char *)panel + OFF_IMAGEPANEL_SCALE, 1)) {
        *(unsigned char *)((char *)panel + OFF_IMAGEPANEL_SCALE) =
            scaleToFit ? 1 : 0;
    }
    if (imagePath != NULL && imagePath[0] != '\0') {
        image = NULL;
        __try {
            scheme = ((GetSchemeFn)(base + RVA_GETSCHEME))();
            if (scheme != NULL) {
                schemeVtable = *(void ***)scheme;
                image = ((SchemeGetImageFn)
                    schemeVtable[SCHEME_GETIMAGE_VT / sizeof(void *)])(
                        scheme, imagePath, 1);
            }
            if (image != NULL) {
                ((SetImageFn)(base + RVA_IMAGEPANEL_SETIMAGE))(panel, image);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            image = NULL;
        }
    }
    VguiBridge_SetVisible(panel, 1);
    return panel;
}

void *Leaflet_CreateDialog(void *parent, const char *name, const char *title,
                           int x, int y, int w, int h)
{
    BYTE *base = VguiBridge_GameUiBase();
    void *dialog;
    if (parent == NULL || name == NULL || base == NULL || !g_controlApiReady) {
        return NULL;
    }
    dialog = VguiBridge_FindChild(parent, name);
    if (dialog == NULL) {
        dialog = AllocateGameUiControl(FRAME_SIZE);
        if (dialog == NULL) {
            return NULL;
        }
        __try {
            /* Child dialogs stay inside GameUI and do not get a taskbar icon. */
            ((FrameCtorFn)(base + RVA_FRAME_CTOR))(dialog, parent, name, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return NULL;
        }
    }
    if (title != NULL) {
        __try {
            ((SetTitleFn)(base + RVA_FRAME_SETTITLE))(dialog, title, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (w <= 0) {
        w = 320;
    }
    if (h <= 0) {
        h = 200;
    }
    VguiBridge_SetPos(dialog, x, y);
    VguiBridge_SetSize(dialog, w, h);
    VguiBridge_SetVisible(dialog, 1);
    return dialog;
}

void *Ui_EnsureCvarSlider(void *parent, const char *name, const char *label,
                          float minv, float maxv, const char *cvar)
{
    GameUiNewFn opnew;
    CCvarSliderCtorFn ctor;
    void *sl;
    BYTE *base = VguiBridge_GameUiBase();

    sl = VguiBridge_FindChild(parent, name);
    if (sl != NULL) {
        return sl;
    }
    if (parent == NULL || base == NULL || name == NULL || cvar == NULL || !VguiBridge_Ready()) {
        return NULL;
    }
    opnew = (GameUiNewFn)(base + RVA_GAMEUI_NEW);
    ctor = (CCvarSliderCtorFn)(base + RVA_CCVARSLIDER_CTOR);
    sl = NULL;
    __try {
        sl = opnew(CCVARSLIDER_SIZE);
        if (sl == NULL) {
            return NULL;
        }
        ctor(sl, parent, name, label != NULL ? label : "", minv, maxv, cvar, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    VguiBridge_SetSize(sl, 160, 48);
    VguiBridge_SetVisible(sl, 1);
    return sl;
}
