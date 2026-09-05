#ifndef LEAFLET_SCHEME_H
#define LEAFLET_SCHEME_H

#include <stdint.h>

typedef struct OverlayTheme {
    uint32_t windowRgb;
    uint32_t borderRgb;
    uint32_t textRgb;
    uint32_t mutedRgb;
    uint32_t accentRgb;
    uint32_t trackRgb;
    int borderWidth;
} OverlayTheme;

/* Reads cstrike/resource/ClientScheme.res, then platform TrackerScheme.
 * Missing keys keep the matte preloader defaults. */
void OverlayTheme_Load(OverlayTheme *out);

#endif
