#ifndef LEAFLET_VGUI_BRIDGE_H
#define LEAFLET_VGUI_BRIDGE_H

#include <windows.h>

/* Single owner of GameUI Panel helpers (FindChild / SetPos / …).
 * Fit* pages and ui_api go through here instead of duplicating RVAs. */

void VguiBridge_Init(HMODULE hGameUI);
int VguiBridge_Ready(void);

void *VguiBridge_FindChild(void *parent, const char *name);
const char *VguiBridge_PanelName(void *panel);

void VguiBridge_SetPos(void *panel, int x, int y);
void VguiBridge_GetPos(void *panel, int *x, int *y);
void VguiBridge_SetSize(void *panel, int w, int h);
void VguiBridge_GetSize(void *panel, int *w, int *h);
void VguiBridge_SetVisible(void *panel, int visible);
void VguiBridge_SetEnabled(void *panel, int enabled);
void VguiBridge_AddActionSignalTarget(void *panel, void *target);

/* Park off-screen (leaflet hide convention). */
void VguiBridge_Park(void *panel);

BYTE *VguiBridge_GameUiBase(void);

#endif
