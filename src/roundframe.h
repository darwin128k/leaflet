#ifndef ROUNDFRAME_H
#define ROUNDFRAME_H

#include <windows.h>

/* Round every GameUI popup on the startup screen (Options, Create Server,
 * New Game, MessageBox, ...). Radius is computed from the panel size so
 * the same code fits a tiny query box and a wide browser. Must run after
 * GameUI.dll is mapped. */
void RoundFrame_Init(HMODULE hOriginalGameUI);

#endif
