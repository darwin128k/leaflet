#ifndef LEAFLET_UI_API_H
#define LEAFLET_UI_API_H

#include <windows.h>
#include "leaflet.h"

/* Small Options / overlay control façade over stock VGUI.
 * Prefer .res FindChild; create only when LoadControlSettings skipped the
 * control (known for extra Slider rows on OptionsSubVoice). */

void UiApi_Init(HMODULE hGameUI);

void *Ui_Find(void *parent, const char *name);
void Ui_Place(void *panel, int x, int y, int w, int h);
void Ui_Hide(void *parent, const char *name);
void Ui_Show(void *panel);

/* Find name under parent, else construct a plain vgui2::Slider. */
void *Ui_EnsureSlider(void *parent, const char *name, int minv, int maxv, int value);

/* Find name, else construct CCvarSlider bound to cvar (min/max float range). */
void *Ui_EnsureCvarSlider(void *parent, const char *name, const char *label,
                          float minv, float maxv, const char *cvar);

/* Label: find only for now (stock Label from .res). Creating Labels via
 * factory comes next — keep captions in OptionsSub*.res until then. */
void *Ui_EnsureLabel(void *parent, const char *name);

#endif
