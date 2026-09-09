#ifndef LEAFLET_UI_CAPS_H
#define LEAFLET_UI_CAPS_H

#include <windows.h>

/* Soft feature flags from live engine cvars — not DLL handles.
 * Unloading MetaAudio/MetaVoice drops their cvars; Options should hide
 * or inert those rows instead of crashing. */

typedef struct UiCaps {
    int metaAudio; /* al_occlusion present */
    int metaVoice; /* mv_gain present */
} UiCaps;

void UiCaps_Init(HMODULE hGameUI);
void UiCaps_Refresh(void);
const UiCaps *UiCaps_Get(void);

#endif
