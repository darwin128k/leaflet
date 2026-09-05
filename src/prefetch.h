#ifndef LEAFLET_PREFETCH_H
#define LEAFLET_PREFETCH_H

#include <stddef.h>
#include <windows.h>

void Prefetch_Bind(HMODULE hGameUI);
void Prefetch_Pump(void);
void Prefetch_OnConnect(void);
int Prefetch_IsActive(void);
void Prefetch_GetUi(int *active, int *permille, char *label, size_t labelSize);

#endif
