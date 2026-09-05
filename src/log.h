#ifndef LOG_H
#define LOG_H

/* Writes one line to gameui_hook.log next to this DLL, opening/flushing/
 * closing the file on every call so a crash right after a log line still
 * leaves that line on disk. Diagnostic-only -- not meant to stay forever. */
void HookLog(const char *fmt, ...);

#endif
