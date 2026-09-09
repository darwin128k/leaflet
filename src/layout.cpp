#include "layout.h"
#include "log.h"
#include "bgswitch.h"
#include "roundframe.h"
#include "prefetch.h"
#include "audioextra.h"
#include "ui_api.h"
#include "ui_layout.h"
#include "ui_caps.h"
#include "vgui_bridge.h"
#include "scheme.h"
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
typedef void(__thiscall *AddPageFn)(void *dialog, void *page, const char *title);
typedef void *(__cdecl *GameUiNewFn)(unsigned int size);
typedef void(__thiscall *PageCtorFn)(void *self, void *parent);
typedef void *(__thiscall *FindChildByNameFn)(void *self, const char *name, int recurse);
typedef int (__thiscall *GetChildCountFn)(void *self);
typedef void *(__thiscall *GetChildFn)(void *self, int index);

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
static AddPageFn g_origPropDialogAddPage = NULL;
static BYTE g_propDialogAddPageTrampoline[32];
static PerformLayoutFn g_origPanelListLayout = NULL;
static BYTE g_panelListLayoutTrampoline[32];
static PerformLayoutFn g_origVideoPageLayout = NULL;
static GameUiNewFn g_gameUiNew = NULL;
static PageCtorFn g_multiAdvPageCtor = NULL;
static FindChildByNameFn g_FindChildByName = NULL;
static GetChildCountFn g_GetChildCount = NULL;
static GetChildFn g_GetChild = NULL;
static const char *g_titleMultiplayer = NULL;
static const char *g_titleAdvancedTab = NULL;
static int g_addingAdvancedTab = 0;
static volatile LONG g_disabledAfterCrash = 0;
static volatile LONG g_paintDisabled = 0;
static volatile LONG g_propSheetLayoutDisabled = 0;
static int g_tabColumnMaxWide = 0; /* high-water mark across layout passes -- see PropertySheetLayout_Hook */
static int g_tabColumnMaxTall = 0;
static int g_lastMainVisibleCount = 4;
static volatile LONG g_hideGameMenuForConnect = 0;
static volatile DWORD g_lastGameMenuLayoutTick = 0;

#define MENU_ITEM_BG_PATH "gfx/vgui/menu_item_bg"
#define MENU_ITEM_BG_ARMED_PATH "gfx/vgui/menu_item_bg_armed"
#define MENU_ITEM_BG_DEPRESSED_PATH "gfx/vgui/menu_item_bg_depressed"

static void InstallPaintBackgroundHook(BYTE *base);
static void InstallBasePanelLayoutHook(BYTE *base);
static void InstallPropertySheetLayoutHook(BYTE *base);
static void InstallAdvancedOptionsTab(BYTE *base);
static void InstallPanelListPaddingHook(BYTE *base);
static void __fastcall PanelListLayout_Hook(void *thisPtr);
static void ForceSchemeLogoSize(const char *path, int wide, int tall);

#define RVA_COPTIONSSUBVIDEO_VTABLE 0x0009c58cu
#define VT_PERFORMLAYOUT_INDEX 111
#define RVA_FINDCHILDBYNAME 0x00044100u /* vgui2::Panel::FindChildByName(const char*, bool); ret 8 */
#define RVA_GETCHILDCOUNT   0x00046260u /* Panel::GetChildCount(); eax count */
#define RVA_GETCHILD        0x00046280u /* Panel::GetChild(int); ret 4 */
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
#define OPTIONS_SHEET_TAB_COUNT_MIN 7 /* Multiplayer…Lock */
#define OPTIONS_SHEET_TAB_COUNT_MAX 8 /* + Advanced tab after Multiplayer */
#define RVA_PROPERTYDIALOG_ADDPAGE 0x00065f00u /* PropertyDialog::AddPage — thunk to Sheet, ecx+0x110 */
#define ADDPAGE_STOLEN 6u /* 8B 89 10 01 00 00 */
#define RVA_GAMEUI_NEW 0x0007a483u
#define RVA_CMULTIADV_PAGE_CTOR 0x000353b0u /* CMultiplayerAdvancedPage::CMultiplayerAdvancedPage(Panel*) */
#define OFF_PROPERTYDIALOG_SHEET 0x110
#define RVA_SKIP_STOCK_ADV_PAGE 0x00037995u /* jz that adds CMultiplayerAdvancedPage after Voice; we insert after MP */
#define RVA_STR_GAMEUI_MULTIPLAYER 0x000b1ee8u
#define RVA_STR_GAMEUI_ADV_NOELLIPSIS 0x000b1e64u /* #GameUI_AdvancedNoEllipsis */
#define OFF_MULTIADV_LISTPANEL 0xBC /* CPanelListPanel* stored in CMultiplayerAdvancedPage ctor */
#define RVA_CPANELLIST_PERFORMLAYOUT 0x00031d40u
#define PLIST_LAYOUT_STOLEN 6u /* 83 EC 0C 53 55 56 */
#define OFF_PLIST_ITEM_COUNT 0x74
#define OFF_PLIST_ITEM_SLOTS 0x7c
#define OPTIONS_INNER_PAD 16
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
#define ITEM_VTABLE_ISSELECTED_OFFSET        0x2b8
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
#define OFF_FIXED_WIDTH    0x1f /* Menu::m_iFixedWidth at +0x7C; ComboBox SetFixedWidth(GetWide()) */
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

static const char *LayoutPanelName(void *panel)
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

static void LayoutMoveFooterButton(void *child, int x, int y)
{
    int w = 0, h = 0;
    if (child == NULL || g_SetPos == NULL) {
        return;
    }
    if (g_GetSize != NULL) {
        g_GetSize(child, &w, &h);
    }
    if (h <= 0) {
        h = 24;
    }
    g_SetPos(child, x, y);
}

static void *LayoutFindChild(void *page, const char *name)
{
    int n;
    int i;
    if (page == NULL || name == NULL) {
        return NULL;
    }
    if (g_FindChildByName != NULL) {
        void *found = g_FindChildByName(page, name, 1);
        if (found != NULL) {
            return found;
        }
    }
    if (g_GetChildCount == NULL || g_GetChild == NULL) {
        return NULL;
    }
    n = g_GetChildCount(page);
    if (n < 0 || n > 64) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        void *child = g_GetChild(page, i);
        const char *cn;
        if (child == NULL) {
            continue;
        }
        cn = LayoutPanelName(child);
        if (lstrcmpiA(cn, name) == 0) {
            return child;
        }
    }
    return NULL;
}

static void *LayoutFooterButtonHeuristic(void *page, void *change, void *clear)
{
    int n;
    int i;
    if (page == NULL || g_GetChildCount == NULL || g_GetChild == NULL || g_GetSize == NULL) {
        return NULL;
    }
    n = g_GetChildCount(page);
    if (n < 0 || n > 64) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        void *child = g_GetChild(page, i);
        const char *cn;
        int w = 0, h = 0, x = 0, y = 0;
        if (child == NULL || child == change || child == clear) {
            continue;
        }
        cn = LayoutPanelName(child);
        if (lstrcmpiA(cn, "listpanel_keybindlist") == 0 || lstrcmpiA(cn, "PanelListPanel") == 0) {
            continue;
        }
        g_GetSize(child, &w, &h);
        if (g_GetPos != NULL) {
            g_GetPos(child, &x, &y);
        }
        if (h < 20 || h > 28 || w < 70 || w > 160) {
            continue;
        }
        if (lstrcmpiA(cn, "Defaults") == 0 || lstrcmpiA(cn, "UseDefaults") == 0 || x < 80) {
            return child;
        }
    }
    return NULL;
}

static void PlaceKeyboardFooterButtons(void *page, int pageW, int pageH)
{
    const int pad = OPTIONS_INNER_PAD;
    void *defaults;
    void *change;
    void *clear;
    int changeW = 84, changeH = 24;
    int clearW = 84, clearH = 24;
    int defW = 105, defH = 24;
    int y;
    if (page == NULL || pageW < 80 || pageH < 80) {
        return;
    }
    defaults = LayoutFindChild(page, "Defaults");
    change = LayoutFindChild(page, "ChangeKeyButton");
    clear = LayoutFindChild(page, "ClearKeyButton");
    if (change == NULL) {
        change = *(void **)((char *)page + 0xBC);
        if (change == NULL || IsBadReadPtr(change, 8) ||
            lstrcmpiA(LayoutPanelName(change), "ChangeKeyButton") != 0) {
            change = NULL;
        }
    }
    if (clear == NULL) {
        clear = *(void **)((char *)page + 0xC0);
        if (clear == NULL || IsBadReadPtr(clear, 8) ||
            lstrcmpiA(LayoutPanelName(clear), "ClearKeyButton") != 0) {
            clear = NULL;
        }
    }
    if (defaults == NULL) {
        defaults = LayoutFooterButtonHeuristic(page, change, clear);
    }
    if (defaults == NULL && change == NULL && clear == NULL) {
        return;
    }
    if (change != NULL && g_GetSize != NULL) {
        g_GetSize(change, &changeW, &changeH);
        if (changeH <= 0) {
            changeH = 24;
        }
    }
    if (clear != NULL && g_GetSize != NULL) {
        g_GetSize(clear, &clearW, &clearH);
        if (clearH <= 0) {
            clearH = 24;
        }
    }
    if (defaults != NULL && g_GetSize != NULL) {
        g_GetSize(defaults, &defW, &defH);
        if (defH <= 0) {
            defH = 24;
        }
    }
    y = pageH - pad - changeH;
    if (clear != NULL) {
        LayoutMoveFooterButton(clear, pageW - pad - clearW, y);
    }
    if (change != NULL) {
        LayoutMoveFooterButton(change, pageW - pad - clearW - 8 - changeW, y);
    }
    if (defaults != NULL) {
        LayoutMoveFooterButton(defaults, pad, y);
    }
}

static void LayoutNamed(void *page, const char *name, int x, int y, int w, int h)
{
    void *c = LayoutFindChild(page, name);
    if (c == NULL) {
        return;
    }
    g_SetPos(c, x, y);
    if (w > 0 && h > 0) {
        g_SetSize(c, w, h);
    }
}

/* Volume captions share one left edge; scheme Label inset is 6px and
 * associate/hot-key images shift some tokens (SFX vs MP3) off that line. */
#define LABEL_SETTEXTINSET_VT 0x230
#define LABEL_SETCONTENTALIGNMENT_VT 0x22c
#define LABEL_ALIGN_WEST 3

static void StyleCaption(void *c)
{
    void **vt;
    SetTextInsetFn setInset;
    SetIntFn setAlign;
    if (c == NULL) {
        return;
    }
    vt = *(void ***)c;
    if (vt == NULL) {
        return;
    }
    setInset = (SetTextInsetFn)vt[LABEL_SETTEXTINSET_VT / sizeof(void *)];
    if (setInset != NULL) {
        setInset(c, 0, 0);
    }
    setAlign = (SetIntFn)vt[LABEL_SETCONTENTALIGNMENT_VT / sizeof(void *)];
    if (setAlign != NULL) {
        setAlign(c, LABEL_ALIGN_WEST);
    }
}

static void LayoutCaption(void *page, const char *name, int x, int y, int w, int h)
{
    void *c;
    c = LayoutFindChild(page, name);
    if (c == NULL) {
        return;
    }
    g_SetPos(c, x, y);
    if (w > 0 && h > 0) {
        g_SetSize(c, w, h);
    }
    StyleCaption(c);
}

#define OFF_IMAGEPANEL_SCALEIMAGE 0x80 /* ImagePanel::m_bScaleImage; ApplySettings writes [esi+0x80] */

static void LayoutNamedAll(void *page, const char *name, int x, int y, int w, int h)
{
    int n;
    int i;
    if (page == NULL || g_GetChildCount == NULL || g_GetChild == NULL) {
        LayoutNamed(page, name, x, y, w, h);
        return;
    }
    n = g_GetChildCount(page);
    if (n < 0 || n > 64) {
        return;
    }
    for (i = 0; i < n; i++) {
        void *child = g_GetChild(page, i);
        if (child == NULL || lstrcmpiA(LayoutPanelName(child), name) != 0) {
            continue;
        }
        g_SetPos(child, x, y);
        if (w > 0 && h > 0) {
            g_SetSize(child, w, h);
        }
    }
}

static void LayoutVoiceMeter(void *page, const char *name, int x, int y, int w, int h)
{
    int n;
    int i;
    if (page == NULL || name == NULL) {
        return;
    }
    if (g_GetChildCount == NULL || g_GetChild == NULL) {
        LayoutNamed(page, name, x, y, w, h);
        return;
    }
    n = g_GetChildCount(page);
    if (n < 0 || n > 64) {
        return;
    }
    for (i = 0; i < n; i++) {
        void *child = g_GetChild(page, i);
        if (child == NULL || lstrcmpiA(LayoutPanelName(child), name) != 0) {
            continue;
        }
        if (!IsBadWritePtr((char *)child + OFF_IMAGEPANEL_SCALEIMAGE, 1)) {
            *((unsigned char *)child + OFF_IMAGEPANEL_SCALEIMAGE) = 0;
        }
        g_SetPos(child, x, y);
        if (w > 0 && h > 0) {
            g_SetSize(child, w, h);
        }
    }
}

static void LayoutMicMeter(void *page, int x, int y, int w, int h)
{
    int n;
    int i;
    if (page == NULL) {
        return;
    }
    if (g_GetChildCount == NULL || g_GetChild == NULL) {
        LayoutNamed(page, "MicMeter", x, y, w, h);
        return;
    }
    n = g_GetChildCount(page);
    if (n < 0 || n > 64) {
        return;
    }
    for (i = 0; i < n; i++) {
        void *child = g_GetChild(page, i);
        if (child == NULL || lstrcmpiA(LayoutPanelName(child), "MicMeter") != 0) {
            continue;
        }
        if (!IsBadWritePtr((char *)child + OFF_IMAGEPANEL_SCALEIMAGE, 1)) {
            *((unsigned char *)child + OFF_IMAGEPANEL_SCALEIMAGE) = 0;
        }
        g_SetPos(child, x, y);
        if (w > 0 && h > 0) {
            int cw = 0;
            int ch = 0;
            if (g_GetSize != NULL) {
                g_GetSize(child, &cw, &ch);
            }
            /* Live overlay is shrunk to 0..160 by GameUI OnThink — don't
             * stomp that with the full track width every layout pass. */
            if (cw > 0 && cw < w - 8) {
                g_SetSize(child, cw, h);
            } else {
                g_SetSize(child, w, h);
            }
        }
    }
}

static void FitAudioPage(void *page, int pageW, int pageH)
{
    const int pad = OPTIONS_INNER_PAD;
    const int rowH = OPTIONS_TOGGLE_ROW_H;
    const int sliderH = 48;
    const int labelH = 20;
    const int hasMetaAudio = AudioExtra_HasMetaAudio();
    const UiGridColumn pageColumns[] = {
        { 5000, 160, 0 },
        { 5000, 160, 0 }
    };
    const UiGridColumn oneColumn[] = {
        { 10000, 0, 0 }
    };
    UiGrid pageGrid;
    UiGrid leftGrid;
    UiGrid rightGrid;
    UiRect leftRect;
    UiRect rightRect;
    UiTableCell leftCells[6];
    UiTableRow leftRows[6];
    UiTableCell rightCells[6];
    UiTableRow rightRows[6];
    int leftCount = 0;
    int rightCount = 0;
    int i;

    if (page == NULL || LayoutFindChild(page, "SFX Slider") == NULL) {
        return;
    }

    UiGrid_Init(&pageGrid, pad, pad, pageW - pad * 2, pageH - pad * 2,
                pad, pageColumns, 2);
    leftRect = UiGrid_Cell(&pageGrid, 0, 1);
    rightRect = UiGrid_Cell(&pageGrid, 1, 1);
    UiGrid_Init(&leftGrid, leftRect.x, leftRect.y, leftRect.w, leftRect.h,
                0, oneColumn, 1);
    UiGrid_Init(&rightGrid, rightRect.x, rightRect.y, rightRect.w, rightRect.h,
                0, oneColumn, 1);

#define AUDIO_LEFT_ROW(controlName, controlH, after)                         \
    do {                                                                     \
        leftCells[leftCount] = { controlName, NULL, 0, 1, controlH,          \
                                 UI_ALIGN_START };                            \
        leftRows[leftCount] = { &leftCells[leftCount], 1, controlH, after }; \
        leftCount++;                                                         \
    } while (0)

#define AUDIO_RIGHT_ROW(controlName)                                         \
    do {                                                                     \
        rightCells[rightCount] = { controlName, NULL, 0, 1, rowH,            \
                                   UI_ALIGN_START };                          \
        rightRows[rightCount] = { &rightCells[rightCount], 1, rowH, 6 };     \
        rightCount++;                                                        \
    } while (0)

    AUDIO_LEFT_ROW("sfx label", labelH, 2);
    AUDIO_LEFT_ROW("SFX Slider", sliderH, 8);
    AUDIO_LEFT_ROW("mp3 label", labelH, 2);
    AUDIO_LEFT_ROW("MP3 Volume", sliderH, hasMetaAudio ? 8 : 0);
    if (hasMetaAudio) {
        AudioExtra_EnsureDopplerSlider(page);
        AUDIO_LEFT_ROW("al_doppler_label", labelH, 2);
        AUDIO_LEFT_ROW("Suit Slider", sliderH, 0);
    } else {
        Ui_Hide(page, "al_doppler_label");
        Ui_Hide(page, "Suit Slider");
    }

    AUDIO_RIGHT_ROW("hisound");
    if (hasMetaAudio) {
        AUDIO_RIGHT_ROW("al_occlusion");
        AUDIO_RIGHT_ROW("al_occlusion_fade");
        AUDIO_RIGHT_ROW("al_resample_all");
        AUDIO_RIGHT_ROW("al_xfi_workaround");
        AUDIO_RIGHT_ROW("al_clamping_mode");
    } else {
        Ui_Hide(page, "al_occlusion");
        Ui_Hide(page, "al_occlusion_fade");
        Ui_Hide(page, "al_resample_all");
        Ui_Hide(page, "al_xfi_workaround");
        Ui_Hide(page, "al_clamping_mode");
    }

    UiTable_Apply(page, &leftGrid, leftRect.y, leftRows, leftCount);
    UiTable_Apply(page, &rightGrid, rightRect.y, rightRows, rightCount);
    StyleCaption(Ui_Find(page, "sfx label"));
    StyleCaption(Ui_Find(page, "mp3 label"));
    if (hasMetaAudio) {
        StyleCaption(Ui_Find(page, "al_doppler_label"));
    }

    {
        static const char *kHide[] = {
            "OpenAL Label", "MilesAudioLabel", "Sound Quality",
            "Label1", "suit label"
        };
        for (i = 0; i < (int)(sizeof(kHide) / sizeof(kHide[0])); i++) {
            Ui_Hide(page, kHide[i]);
        }
    }

#undef AUDIO_LEFT_ROW
#undef AUDIO_RIGHT_ROW

    AudioExtra_BindPage(page);
}

static void FitMultiplayerPage(void *page, int pageW, int pageH)
{
    const int pad = OPTIONS_INNER_PAD;
    const int rowH = OPTIONS_TOGGLE_ROW_H;
    const int preview = 32;
    const int gap = 8;
    const UiGridColumn columns[] = {
        { 2800, 100, 180 },
        {    0, preview, preview },
        { 3600, 72, 0 },
        { 3600, 72, 0 }
    };
    UiGrid grid;
    UiTableCell cells[4][4];
    UiTableRow rows[4];
    static const char *kHide[] = {
        "ModelImage", "URLLabel1", "Advanced", "High Quality Models",
        "Player model", "Primary Color Slider", "Secondary Color Slider",
        "Colors", "Label1", "Label3", "topHorizLeft", "topVertLeft",
        "bottomHorizRight", "bottomVertRight"
    };
    int i;

    if (page == NULL || LayoutFindChild(page, "NameEntry") == NULL) {
        return;
    }
    UiGrid_Init(&grid, pad, pad, pageW - pad * 2, pageH - pad * 2,
                gap, columns, 4);

    cells[0][0] = { "NameLabel", NULL, 0, 1, 24, UI_ALIGN_CENTER };
    cells[0][1] = { "NameEntry", NULL, 1, 3, 28, UI_ALIGN_START };
    rows[0] = { cells[0], 2, 28, 8 };

    cells[1][0] = { "Label2", NULL, 0, 1, 24, UI_ALIGN_CENTER };
    cells[1][1] = { "LogoImage", NULL, 1, 1, preview, UI_ALIGN_START };
    cells[1][2] = { "SpraypaintList", NULL, 2, 1, 24, UI_ALIGN_CENTER };
    cells[1][3] = { "SpraypaintColor", NULL, 3, 1, 24, UI_ALIGN_CENTER };
    rows[1] = { cells[1], 4, preview, 8 };

    cells[2][0] = { "CrosshairLabel", NULL, 0, 1, 24, UI_ALIGN_CENTER };
    cells[2][1] = { "CrosshairImage", NULL, 1, 1, preview, UI_ALIGN_START };
    cells[2][2] = { "CrosshairSizeComboBox", NULL, 2, 1, 24, UI_ALIGN_CENTER };
    cells[2][3] = { "CrosshairColorComboBox", NULL, 3, 1, 24, UI_ALIGN_CENTER };
    rows[2] = { cells[2], 4, preview, 8 };

    cells[3][0] = { "TranslucentLabel", NULL, 0, 1, rowH, UI_ALIGN_START };
    cells[3][1] = { "CrosshairTranslucencyCheckbox", NULL, 0, 4,
                    rowH, UI_ALIGN_START };
    rows[3] = { cells[3], 2, rowH, 0 };

    UiTable_Apply(page, &grid, pad, rows, 4);
    StyleCaption(Ui_Find(page, "NameLabel"));
    StyleCaption(Ui_Find(page, "Label2"));
    StyleCaption(Ui_Find(page, "CrosshairLabel"));
    StyleCaption(Ui_Find(page, "TranslucentLabel"));

    for (i = 0; i < (int)(sizeof(kHide) / sizeof(kHide[0])); i++) {
        Ui_Hide(page, kHide[i]);
    }
}

static void FitVoicePage(void *page, int pageW, int pageH)
{
    const int pad = OPTIONS_INNER_PAD;
    const int rowH = OPTIONS_TOGGLE_ROW_H;
    const int labelH = 20;
    const int sliderH = 28;
    const UiGridColumn columns[] = {
        { 4500, 80, 0 },
        { 5500, 160, 240 }
    };
    UiGrid grid;
    UiTableCell cells[6][2];
    UiTableRow rows[6];
    int innerW;
    int y;
    int rowCount = 0;
    int i;
    void *tx;
    void *rx;
    void *gate;
    void *monitor;

    if (page == NULL || LayoutFindChild(page, "voice_modenable") == NULL) {
        return;
    }
    if (pageH < 80) {
        pageH = 280;
    }
    innerW = pageW - pad * 2;
    UiGrid_Init(&grid, pad, pad, innerW, pageH - pad * 2, 10,
                columns, 2);
    AudioExtra_BindVoicePage(page);

    tx = LayoutFindChild(page, "#GameUI_MicrophoneVolume");
    if (tx == NULL) {
        tx = LayoutFindChild(page, "Microphone Volume");
    }
    rx = LayoutFindChild(page, "VoiceReceive");
    gate = LayoutFindChild(page, "NoiseGate");
    monitor = Ui_Find(page, "SidTone");
    if (monitor == NULL) {
        monitor = AudioExtra_VoiceMonitorPanel();
    }

#define VOICE_FULL_ROW(controlName, after)                                  \
    do {                                                                     \
        cells[rowCount][0] = { controlName, NULL, 0, 2, rowH,                \
                               UI_ALIGN_START };                              \
        rows[rowCount] = { cells[rowCount], 1, rowH, after };                \
        rowCount++;                                                          \
    } while (0)

#define VOICE_SLIDER_ROW(labelName, sliderPanel, after)                     \
    do {                                                                     \
        cells[rowCount][0] = { labelName, NULL, 0, 1, labelH,                \
                               UI_ALIGN_CENTER };                             \
        cells[rowCount][1] = { NULL, sliderPanel, 1, 1, sliderH,             \
                               UI_ALIGN_START };                              \
        rows[rowCount] = { cells[rowCount], 2, sliderH, after };             \
        rowCount++;                                                          \
    } while (0)

    VOICE_FULL_ROW("voice_modenable", 4);
    VOICE_FULL_ROW("MicBoost", 6);
    VOICE_SLIDER_ROW("Transmit label", tx, 4);
    VOICE_SLIDER_ROW("Label1", rx, 4);
    if (UiCaps_Get()->metaVoice) {
        VOICE_SLIDER_ROW("NoiseGateLabel", gate, 4);
        VOICE_SLIDER_ROW("SidToneLabel", monitor, 8);
    } else {
        Ui_Hide(page, "NoiseGate");
        Ui_Hide(page, "NoiseGateLabel");
        Ui_Hide(page, "SidTone");
        Ui_Hide(page, "SidToneLabel");
    }

    y = UiTable_Apply(page, &grid, pad, rows, rowCount);
    StyleCaption(Ui_Find(page, "Transmit label"));
    StyleCaption(Ui_Find(page, "Label1"));
    if (UiCaps_Get()->metaVoice) {
        StyleCaption(Ui_Find(page, "NoiseGateLabel"));
        StyleCaption(Ui_Find(page, "SidToneLabel"));
    }

    {
        static const char *kOldMonitorNames[] = {
            "MvMonitor", "MvMonitorLabel", "VoiceMonitor",
            "VoiceMonitorLabel", "Monitor", "MonitorLabel"
        };
        for (i = 0; i < (int)(sizeof(kOldMonitorNames) / sizeof(kOldMonitorNames[0])); i++) {
            Ui_Hide(page, kOldMonitorNames[i]);
        }
    }

#undef VOICE_FULL_ROW
#undef VOICE_SLIDER_ROW

    {
        int testY = pageH - pad - rowH;
        int vuW = 192;
        int vuH = 86;
        int vuGap = 8;
        int pairW;
        int vuX;
        int vuY;
        int btnW;
        int btnX;
        if (testY < y + vuH + 20) {
            testY = y + vuH + 20;
        }
        pairW = vuW * 2 + vuGap;
        /* Sit the meters just above the button, not flush under the sliders. */
        vuY = testY - 10 - vuH;
        if (vuY < y + 10) {
            vuY = y + 10;
        }
        vuX = pad + (innerW - pairW) / 2;
        if (vuX < pad) {
            vuX = pad;
        }
        btnW = 204;
        if (btnW > innerW) {
            btnW = innerW;
        }
        btnX = pad + (innerW - btnW) / 2;
        LayoutNamed(page, "MicMeter", -4000, -4000, 1, 1);
        LayoutVoiceMeter(page, "MicMeterL", vuX, vuY, vuW, vuH);
        LayoutVoiceMeter(page, "MicMeterR", vuX + vuW + vuGap, vuY, vuW, vuH);
        RoundFrame_NoteVoiceTrackW(btnW);
        LayoutNamed(page, "TestMicrophone", btnX, testY, btnW, rowH);
    }

    LayoutNamed(page, "MilesVoiceLabel", -4000, -4000, 1, 1);
}

static void FitVideoPage(void *page, int pageW, int pageH)
{
    const int pad = OPTIONS_INNER_PAD;
    const int rowH = OPTIONS_TOGGLE_ROW_H;
    const int comboH = 24;
    const int labelH = 20;
    const int gap = 5;
    const UiGridColumn columns[] = {
        { 5000, 156, 0 },
        { 5000, 156, 0 }
    };
    const UiGridColumn oneColumn[] = {
        { 10000, 0, 0 }
    };
    UiGrid pageGrid;
    UiGrid leftGrid;
    UiGrid rightGrid;
    UiTableCell leftCells[6];
    UiTableRow leftRows[6];
    UiTableCell rightCells[6];
    UiTableRow rightRows[6];
    UiRect leftRect;
    UiRect rightRect;
    int leftEnd;
    int rightEnd;
    int slidersY;
    int i;

    if (page == NULL || pageW < 80 || pageH < 80) {
        return;
    }
    if (LayoutFindChild(page, "Windowed") == NULL
        && LayoutFindChild(page, "Renderer") == NULL) {
        return;
    }
    UiGrid_Init(&pageGrid, pad, pad, pageW - pad * 2, pageH - pad * 2,
                pad, columns, 2);
    leftRect = UiGrid_Cell(&pageGrid, 0, 1);
    rightRect = UiGrid_Cell(&pageGrid, 1, 1);
    UiGrid_Init(&leftGrid, leftRect.x, leftRect.y, leftRect.w, leftRect.h,
                0, oneColumn, 1);
    UiGrid_Init(&rightGrid, rightRect.x, rightRect.y, rightRect.w, rightRect.h,
                0, oneColumn, 1);

    {
        static const char *kLeft[] = {
            "Label2", "Renderer", "Label1",
            "Resolution", "Label4", "AspectRatio"
        };
        const int comboMaxW = 180;
        int comboInset = 0;
        if (leftRect.w > comboMaxW) {
            comboInset = leftRect.w - comboMaxW;
        }
        for (i = 0; i < 6; i++) {
            int isLabel = (i % 2) == 0;
            int h = isLabel ? labelH : comboH;
            int after = isLabel ? 0 : (i == 5 ? 0 : gap);
            leftCells[i] = { kLeft[i], NULL, 0, 1, h, UI_ALIGN_START,
                             0, isLabel ? 0 : comboInset };
            leftRows[i] = { &leftCells[i], 1, h, after };
        }
    }
    {
        static const char *kRight[] = {
            "Windowed", "VSync", "HDModels", "AddonsFolder",
            "LowVideoDetail", "DetailTextures"
        };
        for (i = 0; i < 6; i++) {
            rightCells[i] = { kRight[i], NULL, 0, 1, rowH, UI_ALIGN_START };
            rightRows[i] = { &rightCells[i], 1, rowH, gap };
        }
    }
    leftEnd = UiTable_Apply(page, &leftGrid, pad, leftRows, 6);
    rightEnd = UiTable_Apply(page, &rightGrid, pad, rightRows, 6);
    slidersY = leftEnd + 16;
    if (rightEnd + 8 > slidersY) {
        slidersY = rightEnd + 8;
    }

    {
        int sliderW = leftRect.w;
        if (rightRect.w < sliderW) {
            sliderW = rightRect.w;
        }
        Ui_Place(Ui_Find(page, "brightness label"), leftRect.x, slidersY, sliderW, 24);
        Ui_Place(Ui_Find(page, "Gamma label"), rightRect.x, slidersY, sliderW, 24);
        Ui_Place(Ui_Find(page, "Brightness"), leftRect.x, slidersY + 22, sliderW, 50);
        Ui_Place(Ui_Find(page, "Gamma"), rightRect.x, slidersY + 22, sliderW, 50);
    }
    UiGrid_Place(&pageGrid, Ui_Find(page, "Label5"), 0, 2,
                 slidersY + 74, 40, 40, UI_ALIGN_START);

    StyleCaption(Ui_Find(page, "Label2"));
    StyleCaption(Ui_Find(page, "Label1"));
    StyleCaption(Ui_Find(page, "Label4"));
    StyleCaption(Ui_Find(page, "brightness label"));
    StyleCaption(Ui_Find(page, "Gamma label"));
}

static void __fastcall VideoPageLayout_Hook(void *thisPtr)
{
    int w = 0, h = 0;
    if (g_origVideoPageLayout != NULL) {
        g_origVideoPageLayout(thisPtr);
    }
    if (g_GetSize == NULL || thisPtr == NULL) {
        return;
    }
    g_GetSize(thisPtr, &w, &h);
    FitVideoPage(thisPtr, w, h);
}

static void InstallVideoPageLayoutHook(BYTE *base)
{
    void **vt;
    DWORD oldProtect;
    vt = (void **)(base + RVA_COPTIONSSUBVIDEO_VTABLE);
    if (IsBadReadPtr(vt, (VT_PERFORMLAYOUT_INDEX + 1) * sizeof(void *))) {
        return;
    }
    g_origVideoPageLayout = (PerformLayoutFn)vt[VT_PERFORMLAYOUT_INDEX];
    if (g_origVideoPageLayout == NULL) {
        return;
    }
    if (!VirtualProtect(&vt[VT_PERFORMLAYOUT_INDEX], sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }
    vt[VT_PERFORMLAYOUT_INDEX] = (void *)VideoPageLayout_Hook;
    VirtualProtect(&vt[VT_PERFORMLAYOUT_INDEX], sizeof(void *), oldProtect, &oldProtect);
    HookLog("InstallVideoPageLayoutHook: vt[%d] %p -> %p", VT_PERFORMLAYOUT_INDEX,
            (void *)g_origVideoPageLayout, (void *)VideoPageLayout_Hook);
}

static void FitMousePage(void *page, int pageW, int pageH)
{
    const int pad = OPTIONS_INNER_PAD;
    const int rowH = OPTIONS_TOGGLE_ROW_H;
    const int rowGap = 5;
    const int toggleReserve = OPTIONS_TOGGLE_TRACK_W + 12;
    const UiGridColumn columns[] = {
        { 0, 150, 150 },
        { 10000, 40, 0 }
    };
    static const char *kChecks[] = {
        "ReverseMouse", "MouseLook", "MouseFilter", "Joystick",
        "JoystickLook", "Auto-Aim", "RawInput"
    };
    static const char *kDescs[] = {
        "Reverse Mouse label", "Label1", "Mouse filter", "Joystick label",
        "Label2", "AutoaimLabel", "RawInputLabel"
    };
    UiGrid grid;
    UiTableCell cells[7][2];
    UiTableRow rows[7];
    void *slider;
    void *sensitivity;
    int sliderH = 50;
    int y;
    int row;

    if (page == NULL || Ui_Find(page, "ReverseMouse") == NULL) {
        return;
    }
    UiGrid_Init(&grid, pad, pad + 4, pageW - pad * 2, pageH - pad * 2,
                8, columns, 2);
    for (row = 0; row < 7; row++) {
        cells[row][0] = { kChecks[row], NULL, 0, 2, rowH, UI_ALIGN_START };
        cells[row][1] = { kDescs[row], NULL, 1, 1, 24, UI_ALIGN_START,
                          0, toggleReserve, 2 };
        rows[row] = { cells[row], 2, rowH, rowGap };
    }
    y = UiTable_Apply(page, &grid, pad + 4, rows, 7) + 8;

    slider = Ui_Find(page, "Slider");
    if (slider != NULL) {
        int oldW = 0;
        VguiBridge_GetSize(slider, &oldW, &sliderH);
        if (sliderH < 50) {
            sliderH = 50;
        }
    }
    UiGrid_Place(&grid, Ui_Find(page, "Label3"), 0, 2,
                 y, 24, 24, UI_ALIGN_START);
    UiGrid_Place(&grid, slider, 0, 2,
                 y + 24, sliderH, sliderH, UI_ALIGN_START);

    sensitivity = Ui_Find(page, "SensitivityLabel");
    if (sensitivity != NULL) {
        VguiBridge_Park(sensitivity);
        RoundFrame_SetDragValueLabel(sensitivity, -1000, -1000);
    }
}

static void FitOptionsPageLikeAdvanced(void *page, int pageW, int pageH)
{
    const int pad = OPTIONS_INNER_PAD;
    const int btnH = 24;
    const int reserve = pad + btnH + 4;
    void *advList;
    void *keyList;
    if (page == NULL || pageW < 80 || pageH < 80) {
        return;
    }
    advList = LayoutFindChild(page, "PanelListPanel");
    if (advList == NULL) {
        void *maybe = *(void **)((char *)page + OFF_MULTIADV_LISTPANEL);
        if (maybe != NULL && !IsBadReadPtr(maybe, 8) &&
            lstrcmpiA(LayoutPanelName(maybe), "PanelListPanel") == 0) {
            advList = maybe;
        }
    }
    if (advList != NULL) {
        /* Same as Multiplayer: the page is the rounded frame. The list
         * fills it so we don't get a second inset box, and stock scroll
         * range still reaches Radar type. */
        g_SetPos(advList, 0, 0);
        g_SetSize(advList, pageW, pageH);
        /* Stock list PerformLayout already ran for the pre-resize bounds.
         * Without a second pass the rows stay at 0-tall / old coords and
         * Advanced looks empty (scrollbar chrome only). */
        PanelListLayout_Hook(advList);
    }
    keyList = LayoutFindChild(page, "listpanel_keybindlist");
    if (keyList == NULL) {
        void *maybe = *(void **)((char *)page + 0xB8);
        if (maybe != NULL && !IsBadReadPtr(maybe, 8) &&
            lstrcmpiA(LayoutPanelName(maybe), "listpanel_keybindlist") == 0) {
            keyList = maybe;
        }
    }
    if (keyList != NULL) {
        int listH = pageH - pad * 2 - reserve;
        if (listH < 80) {
            listH = 80;
        }
        g_SetPos(keyList, pad, pad);
        g_SetSize(keyList, pageW - pad * 2, listH);
        PlaceKeyboardFooterButtons(page, pageW, pageH);
    }
    FitMousePage(page, pageW, pageH);
    if (LayoutFindChild(page, "voice_modenable") != NULL) {
        FitVoicePage(page, pageW, pageH);
    }
    FitMultiplayerPage(page, pageW, pageH);
    FitVideoPage(page, pageW, pageH);
    FitAudioPage(page, pageW, pageH);
}

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
#define RVA_OPTIONSDIALOG_TALL_PUSH  0x000377d2u /* PUSH 0x196 (tall) */
#define RVA_OPTIONSDIALOG_WIDTH_PUSH 0x000377d7u /* PUSH 0x200 (wide); args right-to-left for SetBounds(this,x=0,y=0,wide,tall) */
#define OPTIONSDIALOG_STOCK_WIDE 0x200u /* 512 */
#define OPTIONSDIALOG_NEW_WIDE   0x280u /* 640 -- absorbs the left tab column */
#define OPTIONSDIALOG_STOCK_TALL 0x196u /* 406 */
#define OPTIONSDIALOG_NEW_TALL   0x1BEu /* 446 -- +40px so OK/Cancel/Apply and the video-restart note aren't flush with the bottom edge */

static int PatchPushImm32(BYTE *target, DWORD expected, DWORD replacement, const char *tag)
{
    static const BYTE kPush = 0x68;
    DWORD oldProtect;

    if (target[0] != kPush || memcmp(target + 1, &expected, 4) != 0) {
        HookLog("%s: prologue mismatch at %p (already patched or wrong build), skip", tag, (void *)target);
        return 0;
    }
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HookLog("%s: VirtualProtect FAILED, GetLastError=%lu", tag, GetLastError());
        return 0;
    }
    memcpy(target + 1, &replacement, 4);
    VirtualProtect(target, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    HookLog("%s: patched %p, %u -> %u", tag, (void *)target, expected, replacement);
    return 1;
}

static void PatchOptionsDialogSize(BYTE *base)
{
    PatchPushImm32(base + RVA_OPTIONSDIALOG_WIDTH_PUSH, OPTIONSDIALOG_STOCK_WIDE,
                   OPTIONSDIALOG_NEW_WIDE, "PatchOptionsDialogSize/wide");
    PatchPushImm32(base + RVA_OPTIONSDIALOG_TALL_PUSH, OPTIONSDIALOG_STOCK_TALL,
                   OPTIONSDIALOG_NEW_TALL, "PatchOptionsDialogSize/tall");
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
    {
        static const BYTE kFindChildPrologue[6] = { 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9 };
        static const BYTE kChildCountPrologue[4] = { 0x53, 0x56, 0x57, 0x8B };
        static const BYTE kGetChildPrologue[6] = { 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF1 };
        if (memcmp(base + RVA_FINDCHILDBYNAME, kFindChildPrologue, 6) == 0) {
            g_FindChildByName = (FindChildByNameFn)(base + RVA_FINDCHILDBYNAME);
        }
        if (memcmp(base + RVA_GETCHILDCOUNT, kChildCountPrologue, 4) == 0) {
            g_GetChildCount = (GetChildCountFn)(base + RVA_GETCHILDCOUNT);
        }
        if (memcmp(base + RVA_GETCHILD, kGetChildPrologue, 6) == 0) {
            g_GetChild = (GetChildFn)(base + RVA_GETCHILD);
        }
        HookLog("LayoutHook_Init: FindChild=%p GetChildCount=%p GetChild=%p",
                (void *)g_FindChildByName, (void *)g_GetChildCount, (void *)g_GetChild);
    }
    HookLog("LayoutHook_Init: base=%p g_SetPos=%p g_GetPos=%p g_SetSize=%p g_GetSize=%p g_SetBgColor=%p g_SetBackgroundTypeCandidate=%p flags=%p/%p/%p g_GetScheme=%p",
            (void *)base, (void *)g_SetPos, (void *)g_GetPos, (void *)g_SetSize, (void *)g_GetSize, (void *)g_SetBgColor, (void *)g_SetBackgroundTypeCandidate,
            (void *)g_SetFlag40, (void *)g_SetFlag41, (void *)g_SetFlag42, (void *)g_GetScheme);
    InstallPaintBackgroundHook(base);
    InstallBasePanelLayoutHook(base);
    InstallPropertySheetLayoutHook(base);
    InstallAdvancedOptionsTab(base);
    InstallPanelListPaddingHook(base);
    InstallVideoPageLayoutHook(base);
    PatchOptionsDialogSize(base);
    UiApi_Init(hOriginalGameUI);
    UiTheme_Reload();
    RoundFrame_Init(hOriginalGameUI);
    Prefetch_Bind(hOriginalGameUI);
    AudioExtra_Init(hOriginalGameUI);
}

void LayoutHook_Tick(void)
{
    DWORD now = GetTickCount();
    DWORD last = (DWORD)InterlockedCompareExchange((volatile LONG *)&g_lastGameMenuLayoutTick, 0, 0);
    if (InterlockedCompareExchange(&g_hideGameMenuForConnect, 0, 0) != 0
        && last != 0 && now - last > 400) {
        InterlockedExchange(&g_hideGameMenuForConnect, 0);
    }
    UiCaps_Refresh();
    AudioExtra_Tick();
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

static void GetItemContentSizeQuiet(void *item, int *outWide, int *outTall)
{
    *outWide = 0;
    *outTall = 0;
    if (item == NULL) {
        return;
    }
    {
        void **vtable = *(void ***)item;
        GetContentSizeFn fn = (GetContentSizeFn)vtable[ITEM_VTABLE_GETCONTENTSIZE_OFFSET / sizeof(void *)];
        fn(item, outWide, outTall);
    }
}

static void GetItemContentSize(void *item, int *outWide, int *outTall)
{
    GetItemContentSizeQuiet(item, outWide, outTall);
    HookLog("  GetItemContentSize: item=%p wide=%d tall=%d", item, *outWide, *outTall);
}

/* GameMenu.res spacer rows are an empty label+command. Stock still marks
 * them visible in-game, which used to draw a blank plate between Player
 * list and New Game. Skip anything with no text (and no icon yet). */
static int ItemIsBlankRow(void *item)
{
    int cw = 0;
    int ct = 0;
    if (item == NULL) {
        return 1;
    }
    GetItemContentSizeQuiet(item, &cw, &ct);
    return cw < 8;
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

static void MakeItemFillTransparent(void *item)
{
    void **vtable;
    SetTwoColorsFn setDefaultColor;
    SetTwoColorsFn setArmedColor;
    SetTwoColorsFn setSelectedColor;
    if (item == NULL) {
        return;
    }
    vtable = *(void ***)item;
    setDefaultColor = (SetTwoColorsFn)vtable[ITEM_VTABLE_SETDEFAULTCOLOR_OFFSET / sizeof(void *)];
    setArmedColor = (SetTwoColorsFn)vtable[ITEM_VTABLE_SETARMEDCOLOR_OFFSET / sizeof(void *)];
    setSelectedColor = (SetTwoColorsFn)vtable[ITEM_VTABLE_SETSELECTEDCOLOR_OFFSET / sizeof(void *)];
    setDefaultColor(item, COLOR_WHITE_OPAQUE, COLOR_TRANSPARENT);
    setArmedColor(item, COLOR_WHITE_OPAQUE, COLOR_TRANSPARENT);
    setSelectedColor(item, COLOR_WHITE_OPAQUE, COLOR_TRANSPARENT);
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

static int ItemIsSelectedQuiet(void *item)
{
    void **vtable;
    IsArmedFn fn;
    if (item == NULL) {
        return 0;
    }
    vtable = *(void ***)item;
    fn = (IsArmedFn)vtable[ITEM_VTABLE_ISSELECTED_OFFSET / sizeof(void *)];
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
        if (item == NULL || !ItemIsVisibleQuiet(item) || ItemIsBlankRow(item)) {
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

static void DrawComboMenuHighlights(void *thisPtr)
{
    void *visibleItems[64];
    int visibleCount;
    int i;
    const OverlayTheme *theme;

    if (g_GetPos == NULL || g_GetSize == NULL) {
        return;
    }
    theme = UiTheme_Current();
    visibleCount = CollectVisibleItems(thisPtr, visibleItems, 64);
    for (i = 0; i < visibleCount; i++) {
        int x = 0, y = 0, w = 0, h = 0;
        int hot;
        hot = ItemIsArmedQuiet(visibleItems[i])
            || ItemIsDepressedQuiet(visibleItems[i])
            || ItemIsSelectedQuiet(visibleItems[i]);
        if (!hot) {
            continue;
        }
        g_GetPos(visibleItems[i], &x, &y);
        g_GetSize(visibleItems[i], &w, &h);
        if (w < 12 || h < 10) {
            continue;
        }
        RoundFrame_FillCapsule(x, y, w, h, theme->accentRgb);
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
        __try {
            DrawComboMenuHighlights(thisPtr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
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
        const int pad = OPTIONS_INNER_PAD;
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
        if (!showTabs || count < OPTIONS_SHEET_TAB_COUNT_MIN || count > OPTIONS_SHEET_TAB_COUNT_MAX) {
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
        y = pad;
        for (i = 0; i < count; i++) {
            void *tab = tabs[i];
            if (tab == NULL) {
                continue;
            }
            g_SetPos(tab, pad, y);
            g_SetSize(tab, columnWide, maxTall);
            y += maxTall + rowSpacing;
        }

        activePage = *(void **)((char *)thisPtr + OFF_SHEET_ACTIVE_PAGE);
        if (activePage != NULL && g_GetPos != NULL) {
            int stockX = 0, stockY = 0;
            int stockWide = 0, stockTall = 0;
            int sheetWide = 0, sheetTall = 0;
            int contentX = pad + columnWide + contentGap;
            g_GetPos(activePage, &stockX, &stockY);
            g_GetSize(activePage, &stockWide, &stockTall);
            g_GetSize(thisPtr, &sheetWide, &sheetTall);

            if (sheetWide - contentX - pad > 0 && sheetTall - pad * 2 > 0) {
                int pageWide = sheetWide - contentX - pad;
                int pageTall = sheetTall - pad * 2;
                if (stockTall > 0) {
                    int stockBottom = stockY + stockTall;
                    if (pad + pageTall > stockBottom && stockBottom > pad + 32) {
                        pageTall = stockBottom - pad;
                    }
                }
                if (pageTall < 32) {
                    pageTall = 32;
                }
                g_SetPos(activePage, contentX, pad);
                g_SetSize(activePage, pageWide, pageTall);
                FitOptionsPageLikeAdvanced(activePage, pageWide, pageTall);
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

static void __fastcall PropertyDialogAddPage_Hook(void *dlg, void *edx, void *page, const char *title)
{
    (void)edx;
    if (g_origPropDialogAddPage != NULL) {
        g_origPropDialogAddPage(dlg, page, title);
    }
    if (g_addingAdvancedTab || dlg == NULL || title == NULL || g_titleMultiplayer == NULL) {
        return;
    }
    if (lstrcmpA(title, g_titleMultiplayer) != 0) {
        return;
    }
    if (g_gameUiNew == NULL || g_multiAdvPageCtor == NULL || g_origPropDialogAddPage == NULL) {
        return;
    }
    __try {
        void *sheet = *(void **)((char *)dlg + OFF_PROPERTYDIALOG_SHEET);
        int count;
        void *adv;
        if (sheet == NULL) {
            return;
        }
        count = *(int *)((char *)sheet + OFF_SHEET_PAGETAB_COUNT);
        if (count != 1) {
            return;
        }
        adv = g_gameUiNew(0xc0u);
        if (adv == NULL) {
            return;
        }
        g_addingAdvancedTab = 1;
        g_multiAdvPageCtor(adv, dlg);
        g_origPropDialogAddPage(dlg, adv, g_titleAdvancedTab);
        g_addingAdvancedTab = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_addingAdvancedTab = 0;
        HookLog("PropertyDialogAddPage_Hook: failed to insert Advanced tab");
    }
}

static void InstallAdvancedOptionsTab(BYTE *base)
{
    static const BYTE kAddPagePrologue[6] = { 0x8B, 0x89, 0x10, 0x01, 0x00, 0x00 };
    BYTE *target = base + RVA_PROPERTYDIALOG_ADDPAGE;
    BYTE *skip = base + RVA_SKIP_STOCK_ADV_PAGE;
    DWORD oldProtect;
    INT32 relBack;
    INT32 relHook;
    DWORD trampProtect;

    g_gameUiNew = (GameUiNewFn)(base + RVA_GAMEUI_NEW);
    g_multiAdvPageCtor = (PageCtorFn)(base + RVA_CMULTIADV_PAGE_CTOR);
    g_titleMultiplayer = (const char *)(base + RVA_STR_GAMEUI_MULTIPLAYER);
    g_titleAdvancedTab = (const char *)(base + RVA_STR_GAMEUI_ADV_NOELLIPSIS);

    if (skip[0] == 0x74 && skip[1] == 0x48) {
        if (VirtualProtect(skip, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            skip[0] = 0xEB;
            VirtualProtect(skip, 1, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), skip, 1);
        }
    }

    if (memcmp(target, kAddPagePrologue, ADDPAGE_STOLEN) != 0) {
        HookLog("InstallAdvancedOptionsTab: AddPage prologue mismatch at %p, skip", (void *)target);
        return;
    }

    memcpy(g_propDialogAddPageTrampoline, target, ADDPAGE_STOLEN);
    g_propDialogAddPageTrampoline[ADDPAGE_STOLEN] = 0xE9;
    relBack = (INT32)((target + ADDPAGE_STOLEN) - (g_propDialogAddPageTrampoline + ADDPAGE_STOLEN + 5));
    memcpy(g_propDialogAddPageTrampoline + ADDPAGE_STOLEN + 1, &relBack, sizeof(relBack));

    if (!VirtualProtect(g_propDialogAddPageTrampoline, sizeof(g_propDialogAddPageTrampoline),
                        PAGE_EXECUTE_READWRITE, &trampProtect)) {
        return;
    }
    g_origPropDialogAddPage = (AddPageFn)(void *)g_propDialogAddPageTrampoline;

    if (!VirtualProtect(target, ADDPAGE_STOLEN, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }
    relHook = (INT32)((BYTE *)PropertyDialogAddPage_Hook - (target + 5));
    target[0] = 0xE9;
    memcpy(target + 1, &relHook, sizeof(relHook));
    target[5] = 0x90;
    VirtualProtect(target, ADDPAGE_STOLEN, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, ADDPAGE_STOLEN);
    FlushInstructionCache(GetCurrentProcess(), g_propDialogAddPageTrampoline,
                          sizeof(g_propDialogAddPageTrampoline));
    HookLog("InstallAdvancedOptionsTab: AddPage hooked %p", (void *)target);
}

static void __fastcall PanelListLayout_Hook(void *thisPtr)
{
    int n;
    int i;
    int listW = 0, listH = 0;
    void **slots;
    const int pad = OPTIONS_INNER_PAD;
    const int scrollW = 24;
    const int rowMinH = 28;

    if (g_origPanelListLayout != NULL) {
        g_origPanelListLayout(thisPtr);
    }
    if (thisPtr == NULL || g_GetPos == NULL || g_SetPos == NULL || g_GetSize == NULL || g_SetSize == NULL) {
        return;
    }
    __try {
        n = *(int *)((char *)thisPtr + OFF_PLIST_ITEM_COUNT);
        slots = *(void ***)((char *)thisPtr + OFF_PLIST_ITEM_SLOTS);
        if (n <= 0 || n > 256 || slots == NULL) {
            return;
        }
        g_GetSize(thisPtr, &listW, &listH);
        for (i = 0; i < n; i++) {
            void *slot = slots[i];
            void *child;
            int x = 0, y = 0, w = 0, h = 0;
            int innerW;
            if (slot == NULL || IsBadReadPtr(slot, sizeof(void *))) {
                continue;
            }
            /* This GameUI DATAITEM holds a single Panel* (the row). */
            child = *(void **)slot;
            if (child == NULL || IsBadReadPtr(child, sizeof(void *))) {
                continue;
            }
            g_GetPos(child, &x, &y);
            g_GetSize(child, &w, &h);
            if (h < rowMinH) {
                h = rowMinH;
            }
            innerW = listW - pad * 2 - scrollW;
            if (innerW < 36) {
                innerW = 36;
            }
            g_SetPos(child, pad, y);
            g_SetSize(child, innerW, h);
            if (g_GetChildCount != NULL && g_GetChild != NULL) {
                int ci;
                int cn = g_GetChildCount(child);
                if (cn > 0 && cn <= 16) {
                    for (ci = 0; ci < cn; ci++) {
                        void *sub = g_GetChild(child, ci);
                        int sx = 0, sy = 0, sw = 0, sh = 0;
                        const char *sn;
                        if (sub == NULL) {
                            continue;
                        }
                        sn = LayoutPanelName(sub);
                        if (lstrcmpiA(sn, "DescCheckButton") != 0) {
                            continue;
                        }
                        g_GetPos(sub, &sx, &sy);
                        g_GetSize(sub, &sw, &sh);
                        if (sh < rowMinH) {
                            sh = rowMinH;
                        }
                        g_SetPos(sub, 0, sy);
                        g_SetSize(sub, innerW, sh);
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        HookLog("PanelListLayout_Hook: exception, leaving stock list layout");
    }
}

static void InstallPanelListPaddingHook(BYTE *base)
{
    static const BYTE kPrologue[6] = { 0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56 };
    BYTE *target = base + RVA_CPANELLIST_PERFORMLAYOUT;
    DWORD oldProtect;
    INT32 relBack;
    INT32 relHook;
    DWORD trampProtect;

    if (memcmp(target, kPrologue, PLIST_LAYOUT_STOLEN) != 0) {
        HookLog("InstallPanelListPaddingHook: prologue mismatch at %p, skip", (void *)target);
        return;
    }
    memcpy(g_panelListLayoutTrampoline, target, PLIST_LAYOUT_STOLEN);
    g_panelListLayoutTrampoline[PLIST_LAYOUT_STOLEN] = 0xE9;
    relBack = (INT32)((target + PLIST_LAYOUT_STOLEN) - (g_panelListLayoutTrampoline + PLIST_LAYOUT_STOLEN + 5));
    memcpy(g_panelListLayoutTrampoline + PLIST_LAYOUT_STOLEN + 1, &relBack, sizeof(relBack));
    if (!VirtualProtect(g_panelListLayoutTrampoline, sizeof(g_panelListLayoutTrampoline),
                        PAGE_EXECUTE_READWRITE, &trampProtect)) {
        return;
    }
    g_origPanelListLayout = (PerformLayoutFn)(void *)g_panelListLayoutTrampoline;
    if (!VirtualProtect(target, PLIST_LAYOUT_STOLEN, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }
    relHook = (INT32)((BYTE *)PanelListLayout_Hook - (target + 5));
    target[0] = 0xE9;
    memcpy(target + 1, &relHook, sizeof(relHook));
    target[5] = 0x90;
    VirtualProtect(target, PLIST_LAYOUT_STOLEN, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, PLIST_LAYOUT_STOLEN);
    FlushInstructionCache(GetCurrentProcess(), g_panelListLayoutTrampoline,
                          sizeof(g_panelListLayoutTrampoline));
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
    HookLog("LayoutHook_ReplacementEntry: panelName=%s isMainMenu=%d visibleCount=%d",
            panelName != NULL ? panelName : "(null)", isMainMenu, visibleCount);

    if (isMainMenu) {
        Prefetch_Pump();
        InterlockedExchange((volatile LONG *)&g_lastGameMenuLayoutTick, (LONG)GetTickCount());
        BgSwitch_RunOnceIfNeeded();
        if (visibleCount == 4) {
            g_lastMainVisibleCount = 4;
            InterlockedExchange(&g_hideGameMenuForConnect, 0);
        } else {
            if (g_lastMainVisibleCount == 4) {
                InterlockedExchange(&g_hideGameMenuForConnect, 1);
                Prefetch_OnConnect();
            }
            g_lastMainVisibleCount = visibleCount;
            if (InterlockedCompareExchange(&g_hideGameMenuForConnect, 0, 0) != 0
                && g_SetPos != NULL) {
                /* Connecting: engine shows LoadingDialog. Our sidebar would
                 * pin Resume/Disconnect to the left and cover that window. */
                g_SetPos(thisPtr, -4000, -4000);
                if (g_SetSize != NULL) {
                    g_SetSize(thisPtr, 1, 1);
                }
                HookLog("LayoutHook_ReplacementEntry: hide GameMenu during connect");
                return;
            }
        }
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
        int genericItemWide;
        int fixedW = self[OFF_FIXED_WIDTH];
        /* ComboBox sets m_iFixedWidth to its own GetWide() before layout.
         * Without that cap the list grows to the longest item / leftover
         * MenuItem size and hangs past the rounded field. */
        if (fixedW >= 32) {
            genericItemWide = fixedW - genericMarginX * 2;
            if (genericItemWide < 16) {
                genericItemWide = 16;
            }
        } else {
            genericItemWide = (maxContentWide > 0) ? (maxContentWide + 8) : 0;
        }

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
            MakeItemFillTransparent(visibleItems[gv]);
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

    /* Main menu icons follow labeled GameMenu.res rows (empty spacer skipped). */
    static const char *kOutOfGameIcons[4] = {
        "gfx/vgui/icon_newgame",
        "gfx/vgui/icon_find",
        "gfx/vgui/icon_options",
        "gfx/vgui/icon_quit",
    };
    static const char *kInGameIcons[7] = {
        "gfx/vgui/icon_resume",
        "gfx/vgui/icon_disconnect",
        "gfx/vgui/icon_players",
        "gfx/vgui/icon_newgame",
        "gfx/vgui/icon_find",
        "gfx/vgui/icon_options",
        "gfx/vgui/icon_quit",
    };

    HookLog("LayoutHook_ReplacementEntry: visibleCount=%d (vertical list mode)", visibleCount);

    {
        int labeledCount = 0;
        const char **icons = NULL;
        int iconCount = 0;
        int iconSlot = 0;
        for (v = 0; v < visibleCount; v++) {
            if (!ItemIsBlankRow(visibleItems[v])) {
                labeledCount++;
            }
        }
        if (labeledCount == 4) {
            icons = kOutOfGameIcons;
            iconCount = 4;
        } else if (labeledCount == 7) {
            icons = kInGameIcons;
            iconCount = 7;
        }
        for (v = 0; v < visibleCount; v++) {
            if (ItemIsBlankRow(visibleItems[v])) {
                continue;
            }
            if (iconSlot < iconCount) {
                HookLog("LayoutHook_ReplacementEntry: v=%d attaching icon %s", v, icons[iconSlot]);
                AttachIcon(visibleItems[v], icons[iconSlot]);
                iconSlot++;
            }
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
        if (ItemIsBlankRow(visibleItems[v])) {
            continue;
        }
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
        int blank = ItemIsBlankRow(visibleItems[v]);

        if (blank) {
            /* Keep the row of air between in-game and always-on items,
             * but park the empty panel off-screen so stock/our plates
             * never paint a substrate in the gap. */
            HookLog("LayoutHook_ReplacementEntry: v=%d spacer, skipping plate", v);
            g_SetPos(visibleItems[v], -4000, y);
            if (g_SetSize != NULL) {
                g_SetSize(visibleItems[v], 1, 1);
            }
            y += rowHeight + rowSpacing;
            continue;
        }

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
