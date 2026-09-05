#ifndef LAYOUT_H
#define LAYOUT_H

#include <windows.h>

/* Resolves GameUI.dll helpers relative to its loaded base and installs
 * the Menu::PaintBackground trampoline (row backdrop image). Must be
 * called once after GameUI.dll is mapped, before the PerformLayout JMP. */
void LayoutHook_Init(HMODULE hOriginalGameUI);

/* Replacement for CGameMenu's internal layout routine (was FUN_1006afb0 at
 * RVA 0x6afb0 in the Nov-2020 GameUI.dll build). Arranges menu items in a
 * left-anchored vertical list. Called via the same register convention
 * the original used (this in ECX, no other args). */
void __fastcall LayoutHook_ReplacementEntry(void *thisPtr);

#endif
