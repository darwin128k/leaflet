#include <windows.h>
#include <string.h>
#include "layout.h"
#include "log.h"
#include "bgswitch.h"

/* Sidecar DLL. It is NOT a GameUI.dll replacement -- the original
 * valve/cl_dlls/GameUI.dll stays byte-identical on disk.
 *
 * Loaded by the GoldSrc launcher via -dll (rev.ini [Loader] Dlls=).
 * Export Launcher_Init. A watcher
 * thread keeps an eye on GameUI.dll for the life of the process:
 * GoldSrc's video-mode change unloads and reloads GameUI (same process
 * or a fresh one), and a one-shot hook would leave the second instance
 * vanilla. */

#define HOOK_TARGET_RVA 0x0006afb0u
#define HOOK_STUB_SIZE  5u /* E9 rel32 */

static HMODULE g_hookedModule = NULL;

static int PathEndsWith(const char *path, const char *suffix)
{
    size_t pathLen = strlen(path);
    size_t suffixLen = strlen(suffix);
    if (pathLen < suffixLen) {
        return 0;
    }
    return lstrcmpiA(path + pathLen - suffixLen, suffix) == 0;
}

static void ResolveGameRootFromSelf(HMODULE hSelf)
{
    char selfPath[MAX_PATH];
    char gameRoot[MAX_PATH];
    char *s;

    GetModuleFileNameA(hSelf, selfPath, MAX_PATH);
    s = strrchr(selfPath, '\\');
    if (s != NULL) {
        *s = '\0';
    } else {
        selfPath[0] = '\0';
    }

    lstrcpynA(gameRoot, selfPath, MAX_PATH);

    /* Next to hl.exe already is the game root. If we were still being
     * loaded from valve/cl_dlls or cstrike/cl_dlls, climb two levels. */
    if (PathEndsWith(gameRoot, "\\cl_dlls") || PathEndsWith(gameRoot, "/cl_dlls")) {
        s = strrchr(gameRoot, '\\');
        if (s != NULL) {
            *s = '\0';
        }
        s = strrchr(gameRoot, '\\');
        if (s != NULL) {
            *s = '\0';
        }
    }

    HookLog("Launcher_Init: gameRoot='%s'", gameRoot);
    BgSwitch_SetGameRoot(gameRoot);
    BgSwitch_Reset();
    BgSwitch_RunOnceIfNeeded();
}

static int HookBytesPointAtUs(HMODULE hGameUI)
{
    BYTE *target = (BYTE *)hGameUI + HOOK_TARGET_RVA;
    INT32 rel;
    BYTE *dest;

    __try {
        if (target[0] != 0xE9) {
            return 0;
        }
        rel = *(INT32 *)(target + 1);
        dest = target + HOOK_STUB_SIZE + rel;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return dest == (BYTE *)LayoutHook_ReplacementEntry;
}

static int InstallLayoutHook(HMODULE hGameUI)
{
    BYTE *target = (BYTE *)hGameUI + HOOK_TARGET_RVA;
    DWORD oldProtect;
    INT_PTR rel;

    HookLog("InstallLayoutHook: hGameUI=%p target=%p", (void *)hGameUI, (void *)target);

    if (!VirtualProtect(target, HOOK_STUB_SIZE, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HookLog("InstallLayoutHook: VirtualProtect FAILED, GetLastError=%lu", GetLastError());
        return 0;
    }

    LayoutHook_Init(hGameUI);

    rel = (INT_PTR)LayoutHook_ReplacementEntry - (INT_PTR)(target + HOOK_STUB_SIZE);
    target[0] = 0xE9;
    *(INT32 *)(target + 1) = (INT32)rel;

    VirtualProtect(target, HOOK_STUB_SIZE, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, HOOK_STUB_SIZE);
    HookLog("InstallLayoutHook: patch written, ReplacementEntry=%p rel=%ld",
            (void *)LayoutHook_ReplacementEntry, (long)rel);
    return 1;
}

static void InstallOn(HMODULE hGameUI)
{
    HMODULE hSelf = NULL;

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)&InstallOn, &hSelf);
    if (hSelf != NULL) {
        ResolveGameRootFromSelf(hSelf);
    }

    if (InstallLayoutHook(hGameUI)) {
        g_hookedModule = hGameUI;
    }
}

extern "C" __declspec(dllexport) void Launcher_Init(void)
{
    HMODULE hGameUI = GetModuleHandleA("GameUI.dll");
    if (hGameUI != NULL) {
        InstallOn(hGameUI);
    }
}

static DWORD WINAPI WatchGameUI(LPVOID unused)
{
    (void)unused;
    for (;;) {
        HMODULE h = GetModuleHandleA("GameUI.dll");
        if (h == NULL) {
            g_hookedModule = NULL;
        } else if (h != g_hookedModule || !HookBytesPointAtUs(h)) {
            /* DllMain of a freshly mapped GameUI.dll may still be on the
             * stack. Give it a moment; if the module vanished in that
             * window, the next loop retries. */
            Sleep(50);
            if (GetModuleHandleA("GameUI.dll") == h) {
                InstallOn(h);
            }
        }
        Sleep(100);
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        {
            HANDLE thread = CreateThread(NULL, 0, WatchGameUI, NULL, 0, NULL);
            if (thread != NULL) {
                CloseHandle(thread);
            }
        }
        break;
    default:
        break;
    }
    return TRUE;
}
