#ifndef ROUNDFRAME_H
#define ROUNDFRAME_H

#include <windows.h>

/* Round every GameUI popup on the startup screen (Options, Create Server,
 * New Game, MessageBox, ...). Radius is computed from the panel size so
 * the same code fits a tiny query box and a wide browser. Must run after
 * GameUI.dll is mapped. */
void RoundFrame_Init(HMODULE hOriginalGameUI);
void RoundFrame_SetDragValueLabel(void *label, int restX, int restY);

/* One pill for every Options toggle (Advanced is the source). */
#define OPTIONS_TOGGLE_TRACK_W 36
#define OPTIONS_TOGGLE_TRACK_H 18
#define OPTIONS_TOGGLE_KNOB    14
#define OPTIONS_TOGGLE_ROW_H   24
#define OPTIONS_TOGGLE_GAP     12

#endif
