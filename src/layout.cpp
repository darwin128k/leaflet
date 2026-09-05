#include "layout.h"
#include "log.h"
#include "bgswitch.h"
#include "roundframe.h"
#include <math.h>
#include <string.h>

/* All addresses below are RVAs into GameUI.dll (ImageBase 0x10000000),
 * recovered via Ghidra static analysis of the Nov-2020 build shipped with
 * this specific game install. They WILL be wrong for a different build --
 * this is a hand-tuned hack, not a portable technique. */

typedef void(__thiscall *SetPosFn)(void *item, int x, int y);
typedef void(__thiscall *SetSizeFn)(void *self, int wide, int tall);
typedef void(__thiscall *GetSizeFn)(void *self, int *outWide, int *outTall);
typedef char(__thiscall *IsVisibleFn)(void *item);
typedef void(__thiscall *SetColorFn)(void *self, unsigned int packedRgba);
typedef void(__thiscall *SetIntFn)(void *self, int value);
typedef void(__thiscall *SetBoolFn)(void *self, unsigned char value);
typedef void *(*GetSchemeFn)(void); /* FUN_1003f030 -- plain function, not virtual */
typedef void *(__thiscall *SchemeGetImageFn)(void *scheme, const char *path, int hardwareFiltered);
typedef void(__thiscall *SetImageAtIndexFn)(void *item, int index, void *image, int preOffset);
typedef void(__thiscall *SetTextInsetFn)(void *item, int xInset, int yInset);
typedef void(__thiscall *SetTextImageIndexFn)(void *item, int newIndex);
typedef void(__thiscall *GetContentSizeFn)(void *item, int *outWide, int *outTall);
typedef void(__thiscall *GetPosFn)(void *self, int *outX, int *outY);
typedef void(__thiscall *PaintBgFn)(void *self);
typedef void(__thiscall *PerformLayoutFn)(void *self);
typedef void(__thiscall *IImageSetPosFn)(void *image, int x, int y);
typedef void(__thiscall *IImageSetSizeFn)(void *image, int wide, int tall);
typedef void(__thiscall *IImagePaintFn)(void *image);
typedef char(__thiscall *IsArmedFn)(void *item);
typedef void(__thiscall *SetTwoColorsFn)(void *item, unsigned int packedFg, unsigned int packedBg);

static SetPosFn g_SetPos = NULL;
static SetSizeFn g_SetSize = NULL;
static GetSizeFn g_GetSize = NULL;
static GetPosFn g_GetPos = NULL;
static SetColorFn g_SetBgColor = NULL;
static SetIntFn g_SetBackgroundTypeCandidate = NULL;
static SetBoolFn g_SetFlag40 = NULL;
static SetBoolFn g_SetFlag41 = NULL;
static SetBoolFn g_SetFlag42 = NULL;
static GetSchemeFn g_GetScheme = NULL;
static PaintBgFn g_origPaintBackground = NULL;
static BYTE g_paintTrampoline[32];
static PerformLayoutFn g_origBasePanelLayout = NULL;
static BYTE g_basePanelLayoutTrampoline[32];
static PerformLayoutFn g_origPropSheetLayout = NULL;
static BYTE g_propSheetLayoutTrampoline[32];
static volatile LONG g_disabledAfterCrash = 0;
static volatile LONG g_paintDisabled = 0;
static volatile LONG g_propSheetLayoutDisabled = 0;
static int g_tabColumnMaxWide = 0; /* high-water mark across layout passes -- see PropertySheetLayout_Hook */
static int g_tabColumnMaxTall = 0;

#define MENU_ITEM_BG_PATH "gfx/vgui/menu_item_bg"
#define MENU_ITEM_BG_ARMED_PATH "gfx/vgui/menu_item_bg_armed"
#define MENU_ITEM_BG_DEPRESSED_PATH "gfx/vgui/menu_item_bg_depressed"

static void InstallPaintBackgroundHook(BYTE *base);
static void InstallBasePanelLayoutHook(BYTE *base);
static void InstallPropertySheetLayoutHook(BYTE *base);

#define RVA_SETPOS     0x000436f0u
#define RVA_GETPOS     0x00043720u /* Panel::GetPos(int&,int&); sits between SetPos and SetSize, same two-stack-arg thunk shape */
#define RVA_SETSIZE    0x00043750u
#define RVA_GETSIZE    0x00043780u /* Panel::GetSize(int&,int&); sits immediately after SetSize in Panel.cpp's own method order, confirmed by matching thiscall(this,int*,int*) shape */
#define RVA_PAINTBACKGROUND 0x0006b5d0u /* vgui2::Menu::PaintBackground -- vtable slot immediately before Paint/PaintBorder/PaintBuildOverlay/PerformLayout */
#define PAINTBG_STOLEN 6u /* 83 EC 08 56 8B F1 */
#define RVA_SETBGCOLOR 0x0006bcc0u /* vgui2::Menu::SetBgColor, confirmed via "Menu/BgColor" xref in ApplySchemeSettings */
#define RVA_SETBGTYPE_CANDIDATE 0x00046130u /* unconfirmed guess: setter/getter pair at offset 0x24, testing as PaintBackgroundType */
#define RVA_SETFLAG40  0x000467f0u /* vtable idx 62, stores 1 byte at this+0x40 -- unconfirmed */
#define RVA_SETFLAG41  0x00046800u /* vtable idx 63, stores 1 byte at this+0x41 -- unconfirmed */
#define RVA_SETFLAG42  0x00046810u /* vtable idx 64, stores 1 byte at this+0x42 -- unconfirmed */
#define RVA_GETSCHEME  0x0003f030u /* returns IScheme*-like singleton; confirmed via icon-loading pattern in a Career-mode button ctor */
#define RVA_BASEPANEL_PERFORMLAYOUT 0x0002bd10u /* CBasePanel vtable slot 111 (vt+0x1BC). Sets this to (0, screenH-64) size (screenW, 64) and lays out GameMenuButton inside that bottom strip. */
#define RVA_PROPERTYSHEET_PERFORMLAYOUT 0x00078370u /* vgui2::PropertySheet::PerformLayout -- found via RTTI: the Complete Object
                                                     * Locator for ".?AVPropertySheet@vgui2@@" leads to this vtable, whose
                                                     * slot 111 (5 slots after PaintBackground, matching Panel.h's
                                                     * PaintBackground/Paint/PaintBorder/PaintBuildOverlay/PostChildPaint/
                                                     * PerformLayout declaration order) decompiles to exactly
                                                     * PropertySheet::PerformLayout from the real vgui_controls source:
                                                     * calls BaseClass::PerformLayout() first, then the default-28px
                                                     * tabHeight branch, GetSize/SetBounds(xtab,2/4,width,tabHeight)
                                                     * accumulation loop, and the this+0x94==_activeTab comparison. */
#define PROPSHEET_LAYOUT_STOLEN 6u /* 83 EC 18 57 8B F9 */
#define OFF_SHEET_PAGETAB_COUNT 0x84 /* m_PageTabs.Count(), read directly off the decompiled body above */
#define OFF_SHEET_PAGETAB_ARRAY 0x8c /* m_PageTabs backing array -- plain PageTab* pointers, 4 bytes/slot (unlike
                                      * CGameMenu's 12-byte item slots) */
#define OFF_SHEET_ACTIVE_PAGE   0x90 /* _activePage (Panel*) */
#define OFF_SHEET_ACTIVE_TAB    0x94 /* _activeTab (PageTab*), unused here but confirms the offset block */
#define OFF_SHEET_SHOW_TABS     0xa0 /* _showTabs (bool) */
#define OPTIONS_SHEET_PANEL_NAME "Sheet" /* vgui_controls::PropertyDialog's constructor always builds its child as
                                          * new PropertySheet(this, "Sheet") -- confirmed by decompiling the function
                                          * that references the "Sheet" string literal. That base ctor is shared by
                                          * every PropertyDialog subclass in the process (Options, Multiplayer
                                          * Advanced, Create Game...), so the name alone doesn't uniquely pick out
                                          * the Options dialog. */
#define OPTIONS_SHEET_TAB_COUNT 7 /* Multiplayer/Keyboard/Mouse/Audio/Video/Voice/Lock -- narrows the shared "Sheet"
                                  * name down to specifically the Options dialog, same style of count-based guard
                                  * as the visibleCount==4 check already used below for the main menu icons. */
#define BANNER_Y 24 /* extra top inset so the CS logo isn't flush with the title bar */
#define LOGO_MENU_GAP 16
#define OFF_GAMEMENU_BUTTON   0xA8 /* CGameMenuButton*; stock PerformLayout SetPos/SetSize this */
#define OFF_GAMEMENU_BUTTON2  0xAC /* sibling the same function also SetSize to the hardcoded 240 */
#define LOGO_IMAGE_PATH "resource/game_menu"
#define LOGO_IMAGE_ARMED_PATH "resource/game_menu_mouseover"
#define IIMAGE_VTABLE_SETSIZE 4
#define FALLBACK_LOGO_TALL 64

/* vgui2::Label virtuals on CGameMenuItem's own vtable (base 0x1009681c),
 * confirmed by decompiling the item's ApplySchemeSettings (references
 * "Marlett") which calls SetImageAtIndex(0, m_pCheck/m_pBlankCheck, 6)
 * at exactly this offset. */
#define ITEM_VTABLE_SCHEME_GETIMAGE_OFFSET   0x14  /* on the scheme object, not the item */
#define ITEM_VTABLE_SETIMAGEATINDEX_OFFSET   0x25c
#define ITEM_VTABLE_SETTEXTINSET_OFFSET      0x230 /* same ApplySchemeSettings, reads "Menu/TextInset" */
#define ITEM_VTABLE_SETTEXTIMAGEINDEX_OFFSET 0x278 /* confirmed against real vgui2::Label::SetTextImageIndex source: moves
                                                     * the text image pointer (this+0x78) into the images-at-index array
                                                     * at the given slot, freeing index 0 for our icon */
#define ITEM_VTABLE_GETCONTENTSIZE_OFFSET    0x228 /* vgui2::Label::GetContentSize(int&,int&) -- confirmed by decompiling
                                                     * the ORIGINAL, unpatched Menu::PerformLayout at RVA 0x6afb0: its own
                                                     * auto-width pass (helper FUN_1006b1b0) calls exactly this vtable
                                                     * slot on each item with two int* out-params and takes the max
                                                     * (+8 padding) -- the real engine's own localization-aware content
                                                     * measurement, not something we're inventing. */
#define ITEM_VTABLE_ISARMED_OFFSET           0x2a4 /* Button::IsArmed -- `mov al,[ecx+0xC2]; ret`. The setter immediately
                                                     * before it (vt+0x2a0, RVA 0x3f930) is SetArmed: it writes this+0xC2
                                                     * then plays the word-at-this+0x100 armed sound if the name isn't -1. */
#define ITEM_VTABLE_ISDEPRESSED_OFFSET       0x2a8 /* Button::IsDepressed -- `mov al,[ecx+0xC3]; ret`, same getter family */
#define ITEM_VTABLE_SETDEFAULTCOLOR_OFFSET   0x2ec /* Button::SetDefaultColor(Color fg, Color bg) -- two dwords, stores +0xE4/+0xE8 */
#define ITEM_VTABLE_SETARMEDCOLOR_OFFSET     0x2f0 /* Button::SetArmedColor(Color fg, Color bg) -- same shape, stores +0xEC/+0xF0 */
#define ITEM_VTABLE_SETSELECTEDCOLOR_OFFSET  0x2f4 /* stores +0xF4/+0xF8 -- GetButtonBgColor uses +0xF8 while depressed */
#define ITEM_VTABLE_SETUSECAPTUREMOUSE_OFFSET 0x2bc /* Button::SetUseCaptureMouse(bool) -- `mov [ecx+0xC7], al`. MenuItem
                                                     * Init turns this off; without it ACTIVATE_ONPRESSEDANDRELEASED never
                                                     * SetSelected, so IsDepressed stays false and release never DoClick. */
#define ITEM_VTABLE_SETACTIVATIONTYPE_OFFSET 0x2d4 /* Button::SetButtonActivationType -- stores int at this+0xD4.
                                                     * 0 = PRESSEDANDRELEASED, 1 = ONPRESSED (stock CGameMenuItem, fires
                                                     * DoClick in OnMousePressed and returns before depressed is set). */
#define BUTTON_ACTIVATE_ONPRESSEDANDRELEASED 0

/* field offsets in DWORDs from 'this', read off the decompiled
 * FUN_1006afb0 body */
#define OFF_ITEM_COUNT     0x2f /* param_1[0x2f]: number of menu items */
#define OFF_ITEM_HEIGHT    0x1e /* param_1[0x1e]: per-item row height   */
#define OFF_ITEM_PTR_BASE  0x23 /* param_1[0x23]: base of item slot table (12 bytes/slot) */
#define OFF_ITEM_INDEX_MAP 0x2c /* param_1[0x2c]: loop-index -> slot-index remap array */

#define ITEM_SLOT_STRIDE   0xc
#define ITEM_VTABLE_IS_VISIBLE_OFFSET 0x78

/* Panel::_panelName. Found by decompiling FUN_10043660 (RVA 0x43660):
 * it frees any existing string at this+0x44 then heap-copies the new one
 * there -- the classic free+strdup shape of a SetName(const char*) setter.
 * The main menu panel is always constructed as
 * new CGameMenu(parent, datafile->GetName()) where datafile is loaded from
 * GameMenu.res, whose root key is literally "GameMenu" -- so reading this
 * field directly lets us tell the real main menu apart from every other
 * vgui2::Menu instance (combo box dropdowns, right-click menus, etc.) that
 * also runs through this same hooked layout routine. */
#define OFF_PANEL_NAME 0x44
#define MAIN_MENU_PANEL_NAME "GameMenu"

/* COptionsDialog::COptionsDialog (RVA 0x377c0) -- found via RTTI/xref to the
 * "OptionsDialog" panelName string literal it passes to PropertyDialog's
 * base ctor. It hardcodes the dialog's default size via
 * SetBounds(this, 0, 0, 0x200, 0x196) i.e. 512x406, decompiled+disassembled
 * with Ghidra (project cs16_gameui). Real vgui_controls::PropertyDialog::
 * PerformLayout (see public vgui_controls source) sizes the PropertySheet
 * to the dialog's own client area and re-anchors OK/Cancel/Apply to the
 * dialog's right edge every layout pass -- so growing the dialog here is
 * sufficient; nothing else needs patching for those to follow along.
 * That 512px width was already snug for the widest sub-page content
 * (Multiplayer's Crosshair combo box + label reach x=462, only 50px of
 * spare margin) even before the left-anchored tab column added by
 * PropertySheetLayout_Hook eats another ~90-110px on the left -- hence the
 * clipped "Crosshair appearance"/"Translucent" text once that hook shipped.
 * Bumping the hardcoded width closes that gap; the two PUSH immediates for
 * (tall, wide) sit back-to-back right after the base-ctor call, verified by
 * byte match before patching, same as every other binary patch in this
 * file. */
#define RVA_OPTIONSDIALOG_WIDTH_PUSH 0x000377d7u /* PUSH 0x200 (wide); PUSH 0x196 (tall) is the instruction just before, at 0x377d2 -- args pushed right-to-left for SetBounds(this,x=0,y=0,wide,tall) */
#define OPTIONSDIALOG_STOCK_WIDE 0x200u /* 512, matches the decompiled SetBounds call */
#define OPTIONSDIALOG_NEW_WIDE   0x280u /* 640 -- +128px, enough to absorb the left tab column and keep the original ~50px margin the widest sub-page content needs */

static void PatchOptionsDialogWidth(BYTE *base)
{
    static const BYTE kExpectedPush[5] = { 0x68, 0x00, 0x02, 0x00, 0x00 }; /* PUSH 0x200 */
    BYTE *target = base + RVA_OPTIONSDIALOG_WIDTH_PUSH;
    DWORD oldProtect;
    DWORD newWide = OPTIONSDIALOG_NEW_WIDE;

    if (memcmp(target, kExpectedPush, sizeof(kExpectedPush)) != 0) {
        HookLog("PatchOptionsDialogWidth: prologue mismatch at %p (already patched or wrong build), skip",
                (void *)target);
        return;
    }

    if (!VirtualProtect(target, sizeof(kExpectedPush), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HookLog("PatchOptionsDialogWidth: VirtualProtect FAILED, GetLastError=%lu", GetLastError());
        return;
    }

    memcpy(target + 1, &newWide, sizeof(newWide));

    VirtualProtect(target, sizeof(kExpectedPush), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(kExpectedPush));
    HookLog("PatchOptionsDialogWidth: patched %p, wide %u -> %u", (void *)target, OPTIONSDIALOG_STOCK_WIDE, newWide);
}

void LayoutHook_Init(HMODULE hOriginalGameUI)
{
    BYTE *base = (BYTE *)hOriginalGameUI;
    InterlockedExchange(&g_disabledAfterCrash, 0);
    InterlockedExchange(&g_paintDisabled, 0);
    g_SetPos = (SetPosFn)(base + RVA_SETPOS);
    g_GetPos = (GetPosFn)(base + RVA_GETPOS);
    g_SetSize = (SetSizeFn)(base + RVA_SETSIZE);
    g_GetSize = (GetSizeFn)(base + RVA_GETSIZE);
    g_SetBgColor = (SetColorFn)(base + RVA_SETBGCOLOR);
    g_SetBackgroundTypeCandidate = (SetIntFn)(base + RVA_SETBGTYPE_CANDIDATE);
    g_SetFlag40 = (SetBoolFn)(base + RVA_SETFLAG40);
    g_SetFlag41 = (SetBoolFn)(base + RVA_SETFLAG41);
    g_SetFlag42 = (SetBoolFn)(base + RVA_SETFLAG42);
    g_GetScheme = (GetSchemeFn)(base + RVA_GETSCHEME);
    HookLog("LayoutHook_Init: base=%p g_SetPos=%p g_GetPos=%p g_SetSize=%p g_GetSize=%p g_SetBgColor=%p g_SetBackgroundTypeCandidate=%p flags=%p/%p/%p g_GetScheme=%p",
            (void *)base, (void *)g_SetPos, (void *)g_GetPos, (void *)g_SetSize, (void *)g_GetSize, (void *)g_SetBgColor, (void *)g_SetBackgroundTypeCandidate,
            (void *)g_SetFlag40, (void *)g_SetFlag41, (void *)g_SetFlag42, (void *)g_GetScheme);
    InstallPaintBackgroundHook(base);
    InstallBasePanelLayoutHook(base);
    InstallPropertySheetLayoutHook(base);
    PatchOptionsDialogWidth(base);
    RoundFrame_Init(hOriginalGameUI);
}

static int ItemIsVisible(void *item)
{
    HookLog("  ItemIsVisible: item=%p", item);
    void **vtable = *(void ***)item;
    HookLog("  ItemIsVisible: vtable=%p", (void *)vtable);
    IsVisibleFn fn = (IsVisibleFn)vtable[ITEM_VTABLE_IS_VISIBLE_OFFSET / sizeof(void *)];
    HookLog("  ItemIsVisible: fn=%p, calling it", (void *)fn);
    int result = fn(item) != 0;
    HookLog("  ItemIsVisible: result=%d", result);
    return result;
}

static void GetItemContentSize(void *item, int *outWide, int *outTall)
{
    *outWide = 0;
    *outTall = 0;
    if (item == NULL) {
        return;
    }
    void **vtable = *(void ***)item;
    GetContentSizeFn fn = (GetContentSizeFn)vtable[ITEM_VTABLE_GETCONTENTSIZE_OFFSET / sizeof(void *)];
    fn(item, outWide, outTall);
    HookLog("  GetItemContentSize: item=%p wide=%d tall=%d", item, *outWide, *outTall);
}

static void AttachIcon(void *item, const char *iconPath)
{
    if (g_GetScheme == NULL) {
        return;
    }

    void *scheme = g_GetScheme();
    HookLog("  AttachIcon: scheme=%p path=%s", scheme, iconPath);
    if (scheme == NULL) {
        return;
    }

    void **schemeVtable = *(void ***)scheme;
    SchemeGetImageFn getImage = (SchemeGetImageFn)schemeVtable[ITEM_VTABLE_SCHEME_GETIMAGE_OFFSET / sizeof(void *)];
    void *image = getImage(scheme, iconPath, 1);
    HookLog("  AttachIcon: image=%p", image);
    if (image == NULL) {
        return;
    }

    void **itemVtable = *(void ***)item;

    /* Confirmed against the real vgui2::Label/MenuItem source: a plain
     * (non-checkable) MenuItem never moves its own text off image index 0,
     * so index 0 is "occupied" by the text by default. MenuItem itself
     * only calls SetTextImageIndex(1) for checkable items to make room for
     * the check glyph -- we do the same thing here to make room for our
     * icon instead. */
    SetTextImageIndexFn setTextImageIndex =
        (SetTextImageIndexFn)itemVtable[ITEM_VTABLE_SETTEXTIMAGEINDEX_OFFSET / sizeof(void *)];
    setTextImageIndex(item, 1);
    HookLog("  AttachIcon: SetTextImageIndex(1) done");

    /* Confirmed via disassembly: RET 0xc, i.e. genuinely 3 explicit stack
     * args -- our (index, image, preOffset) call was correct all along. */
    SetImageAtIndexFn setImg = (SetImageAtIndexFn)itemVtable[ITEM_VTABLE_SETIMAGEATINDEX_OFFSET / sizeof(void *)];
    setImg(item, 0, image, 4);
    HookLog("  AttachIcon: SetImageAtIndex done");
}

/* Valve Color is four bytes [r,g,b,a] in little-endian, so white is
 * 0xFFFFFFFF and fully-transparent black is 0. Passing a transparent
 * background stops MenuItem::PaintBackground from covering our plate. */
#define COLOR_WHITE_OPAQUE 0xFFFFFFFFu
#define COLOR_TRANSPARENT  0x00000000u

static void ClearStockItemFill(void *item)
{
    void **vtable;
    SetTwoColorsFn setDefaultColor;
    SetTwoColorsFn setArmedColor;
    SetTwoColorsFn setSelectedColor;
    SetBoolFn setUseCaptureMouse;
    SetIntFn setActivationType;
    if (item == NULL) {
        return;
    }
    vtable = *(void ***)item;
    setDefaultColor = (SetTwoColorsFn)vtable[ITEM_VTABLE_SETDEFAULTCOLOR_OFFSET / sizeof(void *)];
    setArmedColor = (SetTwoColorsFn)vtable[ITEM_VTABLE_SETARMEDCOLOR_OFFSET / sizeof(void *)];
    setSelectedColor = (SetTwoColorsFn)vtable[ITEM_VTABLE_SETSELECTEDCOLOR_OFFSET / sizeof(void *)];
    setUseCaptureMouse = (SetBoolFn)vtable[ITEM_VTABLE_SETUSECAPTUREMOUSE_OFFSET / sizeof(void *)];
    setActivationType = (SetIntFn)vtable[ITEM_VTABLE_SETACTIVATIONTYPE_OFFSET / sizeof(void *)];
    setDefaultColor(item, COLOR_WHITE_OPAQUE, COLOR_TRANSPARENT);
    setArmedColor(item, COLOR_WHITE_OPAQUE, COLOR_TRANSPARENT);
    setSelectedColor(item, COLOR_WHITE_OPAQUE, COLOR_TRANSPARENT);
    /* Stock CGameMenuItem uses ONPRESSED, so the dialog opens on the same
     * frame as the click and depressed never paints. Switch to click-on-
     * release so the pressed plate is actually visible while held. */
    setUseCaptureMouse(item, 1);
    setActivationType(item, BUTTON_ACTIVATE_ONPRESSEDANDRELEASED);
}

static int ItemIsArmedQuiet(void *item)
{
    void **vtable;
    IsArmedFn fn;
    if (item == NULL) {
        return 0;
    }
    vtable = *(void ***)item;
    fn = (IsArmedFn)vtable[ITEM_VTABLE_ISARMED_OFFSET / sizeof(void *)];
    return fn(item) != 0;
}

static int ItemIsDepressedQuiet(void *item)
{
    void **vtable;
    IsArmedFn fn;
    if (item == NULL) {
        return 0;
    }
    vtable = *(void ***)item;
    fn = (IsArmedFn)vtable[ITEM_VTABLE_ISDEPRESSED_OFFSET / sizeof(void *)];
    return fn(item) != 0;
}

static void PaintOneImage(void *image, int x, int y, int w, int h)
{
    void **imageVtable;
    IImageSetPosFn setPos;
    IImageSetSizeFn setSize;
    IImagePaintFn paint;
    if (image == NULL || w <= 0 || h <= 0) {
        return;
    }
    imageVtable = *(void ***)image;
    paint = (IImagePaintFn)imageVtable[0];
    setPos = (IImageSetPosFn)imageVtable[1];
    setSize = (IImageSetSizeFn)imageVtable[4];
    setPos(image, x, y);
    setSize(image, w, h);
    paint(image);
}

static int ItemIsVisibleQuiet(void *item)
{
    void **vtable;
    IsVisibleFn fn;
    if (item == NULL) {
        return 0;
    }
    vtable = *(void ***)item;
    fn = (IsVisibleFn)vtable[ITEM_VTABLE_IS_VISIBLE_OFFSET / sizeof(void *)];
    return fn(item) != 0;
}

static int CollectVisibleItems(void *thisPtr, void **outItems, int maxItems)
{
    int *self = (int *)thisPtr;
    int itemCount = self[OFF_ITEM_COUNT];
    BYTE *itemSlotBase = (BYTE *)self[OFF_ITEM_PTR_BASE];
    int *indexMap = (int *)self[OFF_ITEM_INDEX_MAP];
    int visibleCount = 0;
    int i;

    if (itemCount <= 0 || itemCount > 64 || itemSlotBase == NULL || indexMap == NULL) {
        return 0;
    }

    for (i = 0; i < itemCount && visibleCount < maxItems; i++) {
        int slotIndex = indexMap[i];
        void *item;
        if (slotIndex < 0 || slotIndex >= itemCount) {
            continue;
        }
        item = *(void **)(itemSlotBase + slotIndex * ITEM_SLOT_STRIDE);
        if (item == NULL || !ItemIsVisibleQuiet(item)) {
            continue;
        }
        outItems[visibleCount++] = item;
    }
    return visibleCount;
}

static int IsMainMenuPanel(void *thisPtr)
{
    const char *panelName = *(const char **)((char *)thisPtr + OFF_PANEL_NAME);
    return panelName != NULL && strcmp(panelName, MAIN_MENU_PANEL_NAME) == 0;
}

static int ReadLogoTgaSize(int *outWide, int *outTall)
{
    static int cachedW = 0;
    static int cachedH = 0;
    const char *root;
    char path[MAX_PATH];
    HANDLE file;
    BYTE header[18];
    DWORD nread;
    unsigned wide;
    unsigned tall;

    if (cachedW > 0 && cachedH > 0) {
        *outWide = cachedW;
        *outTall = cachedH;
        return 1;
    }

    root = BgSwitch_GetGameRoot();
    if (root == NULL || root[0] == '\0') {
        return 0;
    }

    wsprintfA(path, "%s\\cstrike\\resource\\game_menu.tga", root);
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        wsprintfA(path, "%s\\valve\\resource\\game_menu.tga", root);
        file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (!ReadFile(file, header, 18, &nread, NULL) || nread < 18) {
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);

    wide = (unsigned)header[12] | ((unsigned)header[13] << 8);
    tall = (unsigned)header[14] | ((unsigned)header[15] << 8);
    if (wide == 0 || tall == 0 || wide > 2048 || tall > 1024) {
        return 0;
    }

    cachedW = (int)wide;
    cachedH = (int)tall;
    *outWide = cachedW;
    *outTall = cachedH;
    HookLog("ReadLogoTgaSize: %dx%d from %s", cachedW, cachedH, path);
    return 1;
}

static int MenuBelowBannerY(void)
{
    int wide = 0;
    int tall = 0;
    if (ReadLogoTgaSize(&wide, &tall) && tall > 0) {
        return BANNER_Y + tall + LOGO_MENU_GAP;
    }
    return BANNER_Y + FALLBACK_LOGO_TALL + LOGO_MENU_GAP;
}

static void ForceImageDrawSize(void *image, int wide, int tall)
{
    void **vtable;
    IImageSetSizeFn setSize;
    if (image == NULL || wide <= 0 || tall <= 0) {
        return;
    }
    vtable = *(void ***)image;
    setSize = (IImageSetSizeFn)vtable[IIMAGE_VTABLE_SETSIZE];
    setSize(image, wide, tall);
}

static void ForceSchemeLogoSize(const char *path, int wide, int tall)
{
    void *scheme;
    void **schemeVtable;
    SchemeGetImageFn getImage;
    void *image;
    if (g_GetScheme == NULL) {
        return;
    }
    scheme = g_GetScheme();
    if (scheme == NULL) {
        return;
    }
    schemeVtable = *(void ***)scheme;
    getImage = (SchemeGetImageFn)schemeVtable[ITEM_VTABLE_SCHEME_GETIMAGE_OFFSET / sizeof(void *)];
    image = getImage(scheme, path, 0);
    if (image == NULL) {
        image = getImage(scheme, path, 1);
    }
    ForceImageDrawSize(image, wide, tall);
}

static void SizeLogoButton(void *basePanel, int logoWide, int logoTall)
{
    void *button;
    if (g_SetSize == NULL || logoWide <= 0 || logoTall <= 0) {
        return;
    }

    ForceSchemeLogoSize(LOGO_IMAGE_PATH, logoWide, logoTall);
    ForceSchemeLogoSize(LOGO_IMAGE_ARMED_PATH, logoWide, logoTall);

    /* Stock CBasePanel::PerformLayout SetSize's these two to a hardcoded
     * 240x (push 0xF0) -- that was enough for the old 207px inscription
     * and clips anything wider. Override from the TGA header. */
    button = *(void **)((char *)basePanel + OFF_GAMEMENU_BUTTON);
    if (button != NULL) {
        if (g_SetPos != NULL) {
            g_SetPos(button, 0, 0);
        }
        g_SetSize(button, logoWide, logoTall);
    }
    button = *(void **)((char *)basePanel + OFF_GAMEMENU_BUTTON2);
    if (button != NULL && !IsMainMenuPanel(button)) {
        if (g_SetPos != NULL) {
            g_SetPos(button, 0, 0);
        }
        g_SetSize(button, logoWide, logoTall);
    }
}

static void DrawItemBackdrops(void *thisPtr)
{
    void *scheme;
    void **schemeVtable;
    SchemeGetImageFn getImage;
    void *imageIdle;
    void *imageArmed;
    void *imageDepressed;
    void *visibleItems[64];
    int visibleCount;
    int i;

    if (g_GetScheme == NULL || g_GetPos == NULL || g_GetSize == NULL) {
        return;
    }

    scheme = g_GetScheme();
    if (scheme == NULL) {
        return;
    }

    schemeVtable = *(void ***)scheme;
    getImage = (SchemeGetImageFn)schemeVtable[ITEM_VTABLE_SCHEME_GETIMAGE_OFFSET / sizeof(void *)];
    imageIdle = getImage(scheme, MENU_ITEM_BG_PATH, 1);
    imageArmed = getImage(scheme, MENU_ITEM_BG_ARMED_PATH, 1);
    imageDepressed = getImage(scheme, MENU_ITEM_BG_DEPRESSED_PATH, 1);
    if (imageIdle == NULL) {
        return;
    }
    if (imageArmed == NULL) {
        imageArmed = imageIdle;
    }
    if (imageDepressed == NULL) {
        imageDepressed = imageArmed;
    }

    visibleCount = CollectVisibleItems(thisPtr, visibleItems, 64);
    for (i = 0; i < visibleCount; i++) {
        int x = 0, y = 0, w = 0, h = 0;
        void *plate;
        int pressed;
        g_GetPos(visibleItems[i], &x, &y);
        g_GetSize(visibleItems[i], &w, &h);
        pressed = ItemIsDepressedQuiet(visibleItems[i]);
        if (pressed) {
            plate = imageDepressed;
            /* 1px inset so the plate looks pushed in; icon+text stay put. */
            PaintOneImage(plate, x + 1, y + 1, w - 1, h - 1);
        } else {
            plate = ItemIsArmedQuiet(visibleItems[i]) ? imageArmed : imageIdle;
            PaintOneImage(plate, x, y, w, h);
        }
    }
}

static void __fastcall PaintBackground_Hook(void *thisPtr)
{
    if (InterlockedCompareExchange(&g_paintDisabled, 0, 0) != 0) {
        if (g_origPaintBackground != NULL) {
            g_origPaintBackground(thisPtr);
        }
        return;
    }

    if (!IsMainMenuPanel(thisPtr)) {
        if (g_origPaintBackground != NULL) {
            g_origPaintBackground(thisPtr);
        }
        return;
    }

    /* Skip the stock Menu fill so the CS background art shows between
     * rows; draw a scheme TGA under each visible item (under icon+text,
     * because children paint after this). */
    __try {
        DrawItemBackdrops(thisPtr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_paintDisabled, 1);
        HookLog("PaintBackground_Hook: exception drawing item backdrops, falling back to original");
        if (g_origPaintBackground != NULL) {
            g_origPaintBackground(thisPtr);
        }
    }
}

static void InstallPaintBackgroundHook(BYTE *base)
{
    static const BYTE kExpectedPrologue[6] = { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1 };
    BYTE *target = base + RVA_PAINTBACKGROUND;
    DWORD oldProtect;
    INT32 relBack;
    INT32 relHook;
    DWORD trampProtect;

    if (memcmp(target, kExpectedPrologue, PAINTBG_STOLEN) != 0) {
        HookLog("InstallPaintBackgroundHook: prologue mismatch at %p (already hooked or wrong build), skip",
                (void *)target);
        return;
    }

    memcpy(g_paintTrampoline, target, PAINTBG_STOLEN);
    g_paintTrampoline[PAINTBG_STOLEN] = 0xE9;
    relBack = (INT32)((target + PAINTBG_STOLEN) - (g_paintTrampoline + PAINTBG_STOLEN + 5));
    memcpy(g_paintTrampoline + PAINTBG_STOLEN + 1, &relBack, sizeof(relBack));

    if (!VirtualProtect(g_paintTrampoline, sizeof(g_paintTrampoline), PAGE_EXECUTE_READWRITE, &trampProtect)) {
        HookLog("InstallPaintBackgroundHook: trampoline VirtualProtect FAILED, GetLastError=%lu", GetLastError());
        return;
    }
    g_origPaintBackground = (PaintBgFn)(void *)g_paintTrampoline;

    if (!VirtualProtect(target, PAINTBG_STOLEN, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HookLog("InstallPaintBackgroundHook: target VirtualProtect FAILED, GetLastError=%lu", GetLastError());
        return;
    }

    relHook = (INT32)((BYTE *)PaintBackground_Hook - (target + 5));
    target[0] = 0xE9;
    memcpy(target + 1, &relHook, sizeof(relHook));
    target[5] = 0x90;

    VirtualProtect(target, PAINTBG_STOLEN, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, PAINTBG_STOLEN);
    FlushInstructionCache(GetCurrentProcess(), g_paintTrampoline, sizeof(g_paintTrampoline));
    HookLog("InstallPaintBackgroundHook: hooked %p -> %p trampoline=%p",
            (void *)target, (void *)PaintBackground_Hook, (void *)g_paintTrampoline);
}

static void __fastcall BasePanelLayout_Hook(void *thisPtr)
{
    if (g_origBasePanelLayout != NULL) {
        g_origBasePanelLayout(thisPtr);
    }
    /* Stock layout parks this 64px strip at (0, screenH-64) with the CS
     * logo button inside it, and hardcodes the button to 240px wide.
     * Move the strip near the top, then size the button from the TGA so
     * a wider inscription isn't clipped. BANNER_Y keeps a little air
     * under the title bar. The helper at the end of the original function
     * also shoves GameMenu to the bottom -- pull it back under the strip
     * if this object owns it. */
    __try {
        void *menu;
        int wide = 0, tall = 0;
        int logoW = 0, logoH = 0;
        int stripTall;
        int menuY;

        if (!ReadLogoTgaSize(&logoW, &logoH) || logoH <= 0) {
            logoH = FALLBACK_LOGO_TALL;
        }
        stripTall = logoH;
        menuY = BANNER_Y + stripTall + LOGO_MENU_GAP;

        if (g_SetPos != NULL) {
            g_SetPos(thisPtr, 0, BANNER_Y);
        }
        if (g_SetSize != NULL) {
            if (g_GetSize != NULL) {
                g_GetSize(thisPtr, &wide, &tall);
            }
            if (wide <= 0) {
                wide = 640;
            }
            if (wide < logoW) {
                wide = logoW;
            }
            g_SetSize(thisPtr, wide, stripTall);
        }
        SizeLogoButton(thisPtr, logoW, logoH);
        menu = *(void **)((char *)thisPtr + 0xB4);
        if (menu != NULL && IsMainMenuPanel(menu) && g_SetPos != NULL) {
            g_SetPos(menu, 0, menuY);
        }
        menu = *(void **)((char *)thisPtr + 0xB0);
        if (menu != NULL && IsMainMenuPanel(menu) && g_SetPos != NULL) {
            g_SetPos(menu, 0, menuY);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void InstallBasePanelLayoutHook(BYTE *base)
{
    static const BYTE kExpectedPrologue[6] = { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1 };
    BYTE *target = base + RVA_BASEPANEL_PERFORMLAYOUT;
    DWORD oldProtect;
    INT32 relBack;
    INT32 relHook;
    DWORD trampProtect;

    if (memcmp(target, kExpectedPrologue, PAINTBG_STOLEN) != 0) {
        HookLog("InstallBasePanelLayoutHook: prologue mismatch at %p, skip", (void *)target);
        return;
    }

    memcpy(g_basePanelLayoutTrampoline, target, PAINTBG_STOLEN);
    g_basePanelLayoutTrampoline[PAINTBG_STOLEN] = 0xE9;
    relBack = (INT32)((target + PAINTBG_STOLEN) - (g_basePanelLayoutTrampoline + PAINTBG_STOLEN + 5));
    memcpy(g_basePanelLayoutTrampoline + PAINTBG_STOLEN + 1, &relBack, sizeof(relBack));

    if (!VirtualProtect(g_basePanelLayoutTrampoline, sizeof(g_basePanelLayoutTrampoline), PAGE_EXECUTE_READWRITE, &trampProtect)) {
        return;
    }
    g_origBasePanelLayout = (PerformLayoutFn)(void *)g_basePanelLayoutTrampoline;

    if (!VirtualProtect(target, PAINTBG_STOLEN, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }

    relHook = (INT32)((BYTE *)BasePanelLayout_Hook - (target + 5));
    target[0] = 0xE9;
    memcpy(target + 1, &relHook, sizeof(relHook));
    target[5] = 0x90;

    VirtualProtect(target, PAINTBG_STOLEN, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, PAINTBG_STOLEN);
    FlushInstructionCache(GetCurrentProcess(), g_basePanelLayoutTrampoline, sizeof(g_basePanelLayoutTrampoline));
    HookLog("InstallBasePanelLayoutHook: hooked %p -> %p",
            (void *)target, (void *)BasePanelLayout_Hook);
}

/* The Options dialog's tab strip (Multiplayer/Keyboard/Mouse/Audio/Video/
 * Voice/Lock) is a stock vgui_controls::PropertySheet running its own
 * unmodified PerformLayout -- we let that run first (it still auto-sizes
 * every PageTab to its own label width and keeps the show/hide-tabs and
 * active-page bookkeeping correct), then re-stack the same tab buttons
 * into a left-anchored vertical column and shrink the active page into
 * the remaining space on the right, instead of leaving them in the
 * stock left-to-right row across the top. */
static void __fastcall PropertySheetLayout_Hook(void *thisPtr)
{
    if (g_origPropSheetLayout != NULL) {
        g_origPropSheetLayout(thisPtr);
    }

    if (InterlockedCompareExchange(&g_propSheetLayoutDisabled, 0, 0) != 0) {
        return;
    }

    __try {
        const char *panelName = *(const char **)((char *)thisPtr + OFF_PANEL_NAME);
        char showTabs;
        int count;
        void **tabs;
        int maxWide = 0;
        int maxTall = 0;
        int i;
        const int marginX = 4;
        const int startY = 4;
        const int rowSpacing = 2;
        const int contentGap = 6;
        int columnWide;
        int y;
        void *activePage;

        if (panelName == NULL || strcmp(panelName, OPTIONS_SHEET_PANEL_NAME) != 0) {
            return;
        }
        if (g_SetPos == NULL || g_SetSize == NULL || g_GetSize == NULL) {
            return;
        }

        showTabs = *(char *)((char *)thisPtr + OFF_SHEET_SHOW_TABS);
        count = *(int *)((char *)thisPtr + OFF_SHEET_PAGETAB_COUNT);
        if (!showTabs || count != OPTIONS_SHEET_TAB_COUNT) {
            return;
        }

        tabs = *(void ***)((char *)thisPtr + OFF_SHEET_PAGETAB_ARRAY);
        if (tabs == NULL) {
            return;
        }

        /* The stock pass above already auto-sized every tab to its own
         * (localization-aware) label width -- read that back instead of
         * hardcoding pixel widths, same GetSize the original uses. */
        for (i = 0; i < count; i++) {
            void *tab = tabs[i];
            int w = 0, h = 0;
            if (tab == NULL) {
                continue;
            }
            g_GetSize(tab, &w, &h);
            if (w > maxWide) maxWide = w;
            if (h > maxTall) maxTall = h;
        }
        if (maxWide <= 0 || maxTall <= 0) {
            return;
        }

        /* This build's stock PropertySheet::PerformLayout doesn't refit
         * each PageTab to its label on every pass -- it leaves tabs at
         * whatever size they currently are. That means our own GetSize()
         * read above sees back whatever WE set columnWide to on the
         * previous call, not the tab's natural label width. Feeding that
         * into "the new maxWide" made columnWide grow by +12 on literally
         * every layout pass (confirmed via a debug dump: 84, 96, 108, 120,
         * ... one +12 step per PerformLayout call, i.e. once per tab
         * click), pushing the content area further right forever. Freeze
         * the very first measurement instead -- that one still reflects
         * each tab's real, untouched label width, since it's taken before
         * this hook has ever resized anything -- and never recompute it. */
        if (g_tabColumnMaxWide == 0) {
            g_tabColumnMaxWide = maxWide;
            g_tabColumnMaxTall = maxTall;
        }
        maxWide = g_tabColumnMaxWide;
        maxTall = g_tabColumnMaxTall;

        columnWide = maxWide + 12;
        y = startY;
        for (i = 0; i < count; i++) {
            void *tab = tabs[i];
            if (tab == NULL) {
                continue;
            }
            g_SetPos(tab, marginX, y);
            g_SetSize(tab, columnWide, maxTall);
            y += maxTall + rowSpacing;
        }

        activePage = *(void **)((char *)thisPtr + OFF_SHEET_ACTIVE_PAGE);
        if (activePage != NULL && g_GetPos != NULL) {
            /* Reuse the stock pass's own Y/height for the active page --
             * that's what already keeps it clear of the OK/Cancel/Apply row
             * that PropertyDialog positions below the sheet. Overriding Y=0
             * and height=Sheet's full GetSize() (the original approach here)
             * stretched the page down over that button row. Only X/width
             * need to change, to make room for the left-anchored tab column. */
            int stockX = 0, stockY = 0;
            int stockWide = 0, stockTall = 0;
            int sheetWide = 0, sheetTall = 0;
            int contentX = marginX + columnWide + contentGap;
            g_GetPos(activePage, &stockX, &stockY);
            g_GetSize(activePage, &stockWide, &stockTall);
            g_GetSize(thisPtr, &sheetWide, &sheetTall);

            if (sheetWide - contentX > 0 && stockTall > 0) {
                g_SetPos(activePage, contentX, stockY);
                g_SetSize(activePage, sheetWide - contentX, stockTall);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_propSheetLayoutDisabled, 1);
        HookLog("PropertySheetLayout_Hook: exception, disabling further attempts and leaving stock layout in place");
    }
}

static void InstallPropertySheetLayoutHook(BYTE *base)
{
    static const BYTE kExpectedPrologue[6] = { 0x83, 0xEC, 0x18, 0x57, 0x8B, 0xF9 };
    BYTE *target = base + RVA_PROPERTYSHEET_PERFORMLAYOUT;
    DWORD oldProtect;
    INT32 relBack;
    INT32 relHook;
    DWORD trampProtect;

    if (memcmp(target, kExpectedPrologue, PROPSHEET_LAYOUT_STOLEN) != 0) {
        HookLog("InstallPropertySheetLayoutHook: prologue mismatch at %p (already hooked or wrong build), skip",
                (void *)target);
        return;
    }

    memcpy(g_propSheetLayoutTrampoline, target, PROPSHEET_LAYOUT_STOLEN);
    g_propSheetLayoutTrampoline[PROPSHEET_LAYOUT_STOLEN] = 0xE9;
    relBack = (INT32)((target + PROPSHEET_LAYOUT_STOLEN) - (g_propSheetLayoutTrampoline + PROPSHEET_LAYOUT_STOLEN + 5));
    memcpy(g_propSheetLayoutTrampoline + PROPSHEET_LAYOUT_STOLEN + 1, &relBack, sizeof(relBack));

    if (!VirtualProtect(g_propSheetLayoutTrampoline, sizeof(g_propSheetLayoutTrampoline), PAGE_EXECUTE_READWRITE, &trampProtect)) {
        HookLog("InstallPropertySheetLayoutHook: trampoline VirtualProtect FAILED, GetLastError=%lu", GetLastError());
        return;
    }
    g_origPropSheetLayout = (PerformLayoutFn)(void *)g_propSheetLayoutTrampoline;

    if (!VirtualProtect(target, PROPSHEET_LAYOUT_STOLEN, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HookLog("InstallPropertySheetLayoutHook: target VirtualProtect FAILED, GetLastError=%lu", GetLastError());
        return;
    }

    relHook = (INT32)((BYTE *)PropertySheetLayout_Hook - (target + 5));
    target[0] = 0xE9;
    memcpy(target + 1, &relHook, sizeof(relHook));
    target[5] = 0x90;

    VirtualProtect(target, PROPSHEET_LAYOUT_STOLEN, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, PROPSHEET_LAYOUT_STOLEN);
    FlushInstructionCache(GetCurrentProcess(), g_propSheetLayoutTrampoline, sizeof(g_propSheetLayoutTrampoline));
    HookLog("InstallPropertySheetLayoutHook: hooked %p -> %p trampoline=%p",
            (void *)target, (void *)PropertySheetLayout_Hook, (void *)g_propSheetLayoutTrampoline);
}

static void LayoutHook_Inner(void *thisPtr)
{
    HookLog("LayoutHook_ReplacementEntry: ENTER thisPtr=%p", thisPtr);

    int *self = (int *)thisPtr;
    int itemCount = self[OFF_ITEM_COUNT];
    int itemHeight = self[OFF_ITEM_HEIGHT];
    BYTE *itemSlotBase = (BYTE *)self[OFF_ITEM_PTR_BASE];
    int *indexMap = (int *)self[OFF_ITEM_INDEX_MAP];

    HookLog("LayoutHook_ReplacementEntry: itemCount=%d itemHeight=%d itemSlotBase=%p indexMap=%p",
            itemCount, itemHeight, (void *)itemSlotBase, (void *)indexMap);

    if (itemCount <= 0 || itemCount > 64 || g_SetPos == NULL) {
        HookLog("LayoutHook_ReplacementEntry: bailing out (itemCount out of sane range or g_SetPos unset)");
        return;
    }

    /* First pass: resolve slots and keep only the items that are actually
     * visible (GameMenu.res hides e.g. ResumeGame/Disconnect outside a
     * game) -- the circle must be divided among THESE, not the raw total,
     * or a menu with lots of hidden entries only fills a slice of it. */
    void *visibleItems[64];
    int visibleCount = 0;
    int i;
    for (i = 0; i < itemCount; i++) {
        int slotIndex = indexMap[i];
        HookLog("LayoutHook_ReplacementEntry: i=%d slotIndex=%d", i, slotIndex);

        if (slotIndex < 0 || slotIndex >= itemCount) {
            HookLog("LayoutHook_ReplacementEntry: i=%d slotIndex out of [0,%d) range, skipping", i, itemCount);
            continue;
        }

        void *item = *(void **)(itemSlotBase + slotIndex * ITEM_SLOT_STRIDE);
        HookLog("LayoutHook_ReplacementEntry: i=%d item=%p", i, item);

        if (item == NULL || !ItemIsVisible(item)) {
            continue;
        }

        visibleItems[visibleCount++] = item;
    }

    if (visibleCount <= 0) {
        HookLog("LayoutHook_ReplacementEntry: no visible items, nothing to lay out");
        return;
    }

    /* This same layout routine is the ONLY implementation left in the
     * process for vgui2::Menu::PerformLayout -- it runs for every Menu
     * instance, not just the main menu (combo box dropdowns, right-click
     * context menus, etc). Only the real main menu panel should get the
     * custom sidebar/icon treatment; everything else falls through to a
     * plain generic vertical stack that leaves size/position/icons alone
     * as much as possible. */
    const char *panelName = *(const char **)((char *)thisPtr + OFF_PANEL_NAME);
    int isMainMenu = (panelName != NULL && strcmp(panelName, MAIN_MENU_PANEL_NAME) == 0);
    HookLog("LayoutHook_ReplacementEntry: panelName=%s isMainMenu=%d", panelName != NULL ? panelName : "(null)", isMainMenu);

    if (isMainMenu) {
        /* By now the engine has fully applied whatever video mode the
         * player picked (unlike DLL-load time, when the desktop hadn't
         * necessarily switched into it yet), so this is the right moment
         * to pick the matching background tile set. No-ops after the
         * first call this process. */
        BgSwitch_RunOnceIfNeeded();
    }

    if (!isMainMenu) {
        /* Measure each item's own natural (localization-aware) content
         * size -- same real vgui2::Label::GetContentSize the stock engine
         * itself uses -- instead of trusting whatever width the panel
         * happened to have. (Briefly removed while chasing an unrelated
         * "closes right after reopening with a new resolution" report;
         * turned out not to be the cause, so it's back.) */
        int itemHeight = self[OFF_ITEM_HEIGHT];
        if (itemHeight <= 0) {
            itemHeight = 20;
        }

        int maxContentWide = 0;
        int maxContentTall = 0;
        int gv;
        for (gv = 0; gv < visibleCount; gv++) {
            int cw = 0, ct = 0;
            GetItemContentSize(visibleItems[gv], &cw, &ct);
            if (cw > maxContentWide) maxContentWide = cw;
            if (ct > maxContentTall) maxContentTall = ct;
        }
        if (maxContentTall + 4 > itemHeight) {
            itemHeight = maxContentTall + 4;
        }

        const int genericMarginX = 2;
        int genericItemWide = (maxContentWide > 0) ? (maxContentWide + 8) : 0;

        if (genericItemWide <= 0 && g_GetSize != NULL) {
            /* Measurement failed for some reason -- fall back to whatever
             * width the panel already has rather than collapsing it to 0. */
            int curWide = 0, curTall = 0;
            g_GetSize(thisPtr, &curWide, &curTall);
            genericItemWide = (curWide > genericMarginX * 2) ? (curWide - genericMarginX * 2) : 0;
        }

        HookLog("LayoutHook_ReplacementEntry: generic menu, itemHeight=%d maxContentWide=%d genericItemWide=%d",
                itemHeight, maxContentWide, genericItemWide);

        int gy = 0;
        for (gv = 0; gv < visibleCount; gv++) {
            g_SetPos(visibleItems[gv], genericMarginX, gy);
            if (g_SetSize != NULL && genericItemWide > 0) {
                g_SetSize(visibleItems[gv], genericItemWide, itemHeight);
            }
            gy += itemHeight;
        }

        if (g_SetSize != NULL && genericItemWide > 0) {
            g_SetSize(thisPtr, genericItemWide + genericMarginX * 2, gy);
        }

        HookLog("LayoutHook_ReplacementEntry: EXIT (generic menu path)");
        return;
    }

    /* Plain vertical list, sidebar-style: icon+label pairs stacked top to
     * bottom, not a circle -- anchored to the LEFT side of the screen.
     * The panel's own origin is fixed relative to the screen's top-left
     * corner regardless of resolution; a fixed offset from the right edge
     * would drift as the window is resized/the resolution changes, since
     * only the right/bottom edges move. Left-anchoring is resolution-safe. */
    const int marginX = 8;
    const int startY = 20; /* local coords are clipped below 0 by the parent (confirmed: negative startY just cut the first rows off) -- move the PANEL itself instead, see below */
    const int bottomPadding = 20;
    const int rowSpacing = 10;
    const int minRowHeight = 36; /* icon art is 32x32 -- never go below that plus a little headroom */
    int v;

    /* Only the main menu (4 visible items, in GameMenu.res order: New Game,
     * Find Servers, Options, Quit) gets real per-item icons -- other menu
     * variants (e.g. the 3-item in-game pause menu) would mismatch this
     * list, so they just don't get icons attached. */
    static const char *kMainMenuIcons[4] = {
        "gfx/vgui/icon_newgame",
        "gfx/vgui/icon_find",
        "gfx/vgui/icon_options",
        "gfx/vgui/icon_quit",
    };

    HookLog("LayoutHook_ReplacementEntry: visibleCount=%d (vertical list mode)", visibleCount);

    /* Attach icons FIRST -- they add to each item's own image list, which
     * its content-size measurement below has to see, or the measurement
     * would be taken before the icon exists. */
    if (visibleCount == 4) {
        for (v = 0; v < visibleCount; v++) {
            HookLog("LayoutHook_ReplacementEntry: v=%d attaching icon %s", v, kMainMenuIcons[v]);
            AttachIcon(visibleItems[v], kMainMenuIcons[v]);
        }
    }

    for (v = 0; v < visibleCount; v++) {
        ClearStockItemFill(visibleItems[v]);
    }

    /* Measure the real, localization-aware content width/height of each
     * item (same GetContentSize the stock engine itself uses to auto-size
     * menus) instead of a hardcoded pixel width -- a longer translated
     * label just gets more room automatically. iconAllowance covers the
     * icon + gap that GetContentSize's own bookkeeping doesn't fully
     * attribute back to the label's reported width. */
    const int iconAllowance = 40;
    const int trailingPadding = 24;
    int maxContentWide = 0;
    int maxContentTall = 0;
    for (v = 0; v < visibleCount; v++) {
        int cw = 0, ct = 0;
        GetItemContentSize(visibleItems[v], &cw, &ct);
        HookLog("LayoutHook_ReplacementEntry: v=%d contentWide=%d contentTall=%d", v, cw, ct);
        if (cw > maxContentWide) maxContentWide = cw;
        if (ct > maxContentTall) maxContentTall = ct;
    }

    int itemWidth = (maxContentWide > 0) ? (maxContentWide + iconAllowance + trailingPadding) : 210;
    int rowHeight = (maxContentTall + 8 > minRowHeight) ? (maxContentTall + 8) : minRowHeight;
    HookLog("LayoutHook_ReplacementEntry: computed itemWidth=%d rowHeight=%d", itemWidth, rowHeight);

    /* The panel's own on-screen position (wherever the engine originally
     * put the stock vertical menu) is what actually needs to move to push
     * the list higher -- local child coordinates below 0 just get clipped
     * by this same panel, so that's a dead end. */
    HookLog("LayoutHook_ReplacementEntry: calling panel SetPos");
    g_SetPos(thisPtr, 0, MenuBelowBannerY());

    /* Per-row plate is drawn in Menu::PaintBackground from
     * gfx/vgui/menu_item_bg (TGA next to the icons). */

    int y = startY;
    for (v = 0; v < visibleCount; v++) {
        int x = marginX;

        HookLog("LayoutHook_ReplacementEntry: v=%d x=%d y=%d, calling SetPos", v, x, y);
        g_SetPos(visibleItems[v], x, y);
        HookLog("LayoutHook_ReplacementEntry: SetPos returned");

        if (g_SetSize != NULL) {
            HookLog("LayoutHook_ReplacementEntry: v=%d calling item SetSize %dx%d", v, itemWidth, rowHeight);
            g_SetSize(visibleItems[v], itemWidth, rowHeight);
            HookLog("LayoutHook_ReplacementEntry: item SetSize returned");
        }

        y += rowHeight + rowSpacing;
    }

    if (g_SetSize != NULL) {
        int totalHeight = y + bottomPadding;
        int totalWidth = marginX + itemWidth + bottomPadding;
        HookLog("LayoutHook_ReplacementEntry: calling SetSize %dx%d", totalWidth, totalHeight);
        g_SetSize(thisPtr, totalWidth, totalHeight);
        HookLog("LayoutHook_ReplacementEntry: SetSize returned");
    }

    HookLog("LayoutHook_ReplacementEntry: EXIT normally");
}

static int CrashFilter(unsigned int code, EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *rec = (ep != NULL) ? ep->ExceptionRecord : NULL;
    void *addr = (rec != NULL) ? rec->ExceptionAddress : NULL;

    if (rec != NULL && code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        ULONG_PTR accessType = rec->ExceptionInformation[0]; /* 0=read, 1=write, 8=DEP */
        ULONG_PTR badVA = rec->ExceptionInformation[1];
        HookLog("LayoutHook_ReplacementEntry: *** SEH CAUGHT *** code=0x%08X instrAddr=%p accessType=%lu badAddr=0x%p",
                code, addr, (unsigned long)accessType, (void *)badVA);
    } else {
        HookLog("LayoutHook_ReplacementEntry: *** SEH CAUGHT *** code=0x%08X instrAddr=%p (no extra info)", code, addr);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

void __fastcall LayoutHook_ReplacementEntry(void *thisPtr)
{
    if (InterlockedCompareExchange(&g_disabledAfterCrash, 0, 0) != 0) {
        return; /* stay quiet after the first crash so the log doesn't explode from per-frame retries */
    }

    __try {
        LayoutHook_Inner(thisPtr);
    } __except (CrashFilter(GetExceptionCode(), GetExceptionInformation())) {
        HookLog("LayoutHook_ReplacementEntry: exception suppressed, disabling further attempts this run");
        InterlockedExchange(&g_disabledAfterCrash, 1);
    }
}
