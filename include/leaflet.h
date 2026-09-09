#ifndef LEAFLET_PUBLIC_H
#define LEAFLET_PUBLIC_H

#include <windows.h>

#if defined(LEAFLET_BUILD)
#define LEAFLET_PUBLIC __declspec(dllexport)
#else
#define LEAFLET_PUBLIC __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

LEAFLET_PUBLIC void *Leaflet_CreateLabel(void *parent, const char *name,
                                         const char *text);
LEAFLET_PUBLIC void *Leaflet_CreateButton(void *parent, const char *name,
                                          const char *text,
                                          const char *command);
LEAFLET_PUBLIC void *Leaflet_CreateImage(void *parent, const char *name,
                                         const char *imagePath,
                                         int scaleToFit);
LEAFLET_PUBLIC void *Leaflet_CreateDialog(void *parent, const char *name,
                                          const char *title,
                                          int x, int y, int w, int h);

#ifdef __cplusplus
}

namespace leaflet {
inline void *Label(void *parent, const char *name, const char *text)
{
    return Leaflet_CreateLabel(parent, name, text);
}

inline void *Button(void *parent, const char *name, const char *text,
                    const char *command = NULL)
{
    return Leaflet_CreateButton(parent, name, text, command);
}

inline void *Image(void *parent, const char *name, const char *imagePath,
                   bool scaleToFit = true)
{
    return Leaflet_CreateImage(parent, name, imagePath, scaleToFit ? 1 : 0);
}

inline void *Dialog(void *parent, const char *name, const char *title,
                    int x, int y, int w, int h)
{
    return Leaflet_CreateDialog(parent, name, title, x, y, w, h);
}
}
#endif

#endif
