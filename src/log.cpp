#include "log.h"

/* Logging disabled: was opening/writing/closing gameui_hook.log on every
 * single call (hundreds of times per menu layout), which is a lot of
 * runtime file I/O to a resource-folder-adjacent path -- suspected of
 * interacting badly with something in the launcher/anti-tamper layer
 * around a video-mode restart. Kept as a no-op (rather than deleting the
 * function and its many call sites throughout layout.cpp/bgswitch.cpp/
 * main.cpp) so logging can be turned back on in one place if needed. */
void HookLog(const char *fmt, ...)
{
    (void)fmt;
}
