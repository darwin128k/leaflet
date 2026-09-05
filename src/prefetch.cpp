#include <winsock2.h>
#include <ws2tcpip.h>

#include "prefetch.h"
#include "overlay.h"
#include "bgswitch.h"
#include "log.h"

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

#pragma comment(lib, "ws2_32.lib")

static HMODULE g_gameUI = NULL;
static HANDLE g_thread = NULL;
static volatile LONG g_stop = 0;
static volatile LONG g_workerStarted = 0;
static volatile LONG g_uiActive = 0;
static volatile LONG g_totalFiles = 0;
static volatile LONG g_doneFiles = 0;
static volatile LONG g_filePermille = 0;
static char g_status[260];
static char g_currentFile[260];
static char g_fastDl[512];
static char g_queryHost[128];
static int g_queryPort = 27015;

static int ComputePermille(void)
{
    LONG total = InterlockedCompareExchange(&g_totalFiles, 0, 0);
    LONG done = InterlockedCompareExchange(&g_doneFiles, 0, 0);
    LONG frac = InterlockedCompareExchange(&g_filePermille, 0, 0);

    if (total <= 0) {
        return 0;
    }
    if (done >= total) {
        return 1000;
    }
    return (int)((done * 1000 + frac) / total);
}

static void SetStatus(const char *text)
{
    lstrcpynA(g_status, text != NULL ? text : "", sizeof(g_status));
}

static void SetCurrentFile(const char *text)
{
    lstrcpynA(g_currentFile, text != NULL ? text : "", sizeof(g_currentFile));
}

static int FileExistsNonEmpty(const char *path)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;

    h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    FindClose(h);
    if (fd.nFileSizeHigh == 0 && fd.nFileSizeLow == 0) {
        return 0;
    }
    return 1;
}

static void ToNativePath(char *path)
{
    char *p;
    for (p = path; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\\';
        }
    }
}

static void ToUrlPath(char *path)
{
    char *p;
    for (p = path; *p != '\0'; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
}

static void EnsureDirTree(const char *dir)
{
    char buf[MAX_PATH];
    char *p;

    lstrcpynA(buf, dir, MAX_PATH);
    p = buf;
    if (p[0] != '\0' && p[1] == ':') {
        p += 2;
        if (*p == '\\' || *p == '/') {
            p++;
        }
    }
    for (; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/') {
            char saved = *p;
            *p = '\0';
            CreateDirectoryA(buf, NULL);
            *p = saved;
        }
    }
    if (buf[0] != '\0') {
        CreateDirectoryA(buf, NULL);
    }
}

static void EnsureParentDirs(const char *path)
{
    char buf[MAX_PATH];
    char *slash;

    lstrcpynA(buf, path, MAX_PATH);
    slash = strrchr(buf, '\\');
    if (slash == NULL) {
        slash = strrchr(buf, '/');
    }
    if (slash == NULL || slash == buf) {
        return;
    }
    *slash = '\0';
    EnsureDirTree(buf);
}

static int XferInfo(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                    curl_off_t ultotal, curl_off_t ulnow)
{
    (void)clientp;
    (void)ultotal;
    (void)ulnow;
    if (InterlockedCompareExchange(&g_stop, 0, 0) != 0) {
        return 1;
    }
    if (dltotal > 0) {
        InterlockedExchange(&g_filePermille, (LONG)((dlnow * 1000) / dltotal));
    }
    return 0;
}

static int CurlGet(CURL *curl, const char *url, const char *destPath)
{
    FILE *out;
    CURLcode rc;
    long http = 0;
    char tmp[MAX_PATH];

    EnsureParentDirs(destPath);
    _snprintf(tmp, sizeof(tmp), "%s.part", destPath);
    tmp[sizeof(tmp) - 1] = '\0';
    out = fopen(tmp, "wb");
    if (out == NULL) {
        return 0;
    }
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "leaflet/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferInfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);
    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    fclose(out);
    if (rc != CURLE_OK) {
        DeleteFileA(tmp);
        return 0;
    }
    DeleteFileA(destPath);
    if (!MoveFileA(tmp, destPath)) {
        DeleteFileA(tmp);
        return 0;
    }
    (void)http;
    return 1;
}

static void JoinUnder(char *out, size_t outSize, const char *gameRoot,
                      const char *gamedir, const char *rel)
{
    char native[MAX_PATH];

    lstrcpynA(native, rel, MAX_PATH);
    ToNativePath(native);
    _snprintf(out, outSize, "%s\\%s\\%s", gameRoot, gamedir, native);
    out[outSize - 1] = '\0';
}

static void JoinDownloadFile(char *out, size_t outSize, const char *gameRoot, const char *rel)
{
    JoinUnder(out, outSize, gameRoot, "cstrike_downloads", rel);
}

static int FileAlreadyPresent(const char *gameRoot, const char *rel)
{
    char path[MAX_PATH];

    JoinUnder(path, sizeof(path), gameRoot, "cstrike", rel);
    if (FileExistsNonEmpty(path)) {
        return 1;
    }
    JoinDownloadFile(path, sizeof(path), gameRoot, rel);
    return FileExistsNonEmpty(path);
}

static int OpenLocalCopy(const char *gameRoot, const char *rel, char *out, size_t outSize)
{
    JoinUnder(out, outSize, gameRoot, "cstrike", rel);
    if (FileExistsNonEmpty(out)) {
        return 1;
    }
    JoinDownloadFile(out, outSize, gameRoot, rel);
    return FileExistsNonEmpty(out);
}

static void BuildUrl(char *out, size_t outSize, const char *rel)
{
    char urlRel[MAX_PATH];
    size_t n;

    lstrcpynA(urlRel, rel, MAX_PATH);
    ToUrlPath(urlRel);
    n = strlen(g_fastDl);
    if (n > 0 && g_fastDl[n - 1] != '/') {
        _snprintf(out, outSize, "%s/%s", g_fastDl, urlRel);
    } else {
        _snprintf(out, outSize, "%s%s", g_fastDl, urlRel);
    }
    out[outSize - 1] = '\0';
}

static void AddUnique(std::vector<std::string> *list, const char *rel)
{
    size_t i;
    if (rel == NULL || rel[0] == '\0') {
        return;
    }
    for (i = 0; i < list->size(); i++) {
        if (_stricmp((*list)[i].c_str(), rel) == 0) {
            return;
        }
    }
    list->push_back(rel);
}

static int EndsWithI(const char *s, const char *suffix)
{
    size_t n;
    size_t m;
    if (s == NULL || suffix == NULL) {
        return 0;
    }
    n = strlen(s);
    m = strlen(suffix);
    if (m > n) {
        return 0;
    }
    return _stricmp(s + (n - m), suffix) == 0;
}

/* .res often lists itself (Calou-style). Downloading that again is a loop. */
static int IsOptionalResEntry(const char *rel, const char *resPath)
{
    (void)resPath;
    if (rel == NULL || rel[0] == '\0') {
        return 1;
    }
    return EndsWithI(rel, ".res");
}

static void ParseResFile(const char *path, std::vector<std::string> *list)
{
    FILE *f;
    char line[512];

    f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = line;
        char token[MAX_PATH];
        size_t n = 0;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '/' || *p == '{' || *p == '}' || *p == '\r' || *p == '\n') {
            continue;
        }
        if (*p == '"') {
            p++;
            while (*p != '\0' && *p != '"' && n + 1 < sizeof(token)) {
                token[n++] = *p++;
            }
        } else {
            while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && n + 1 < sizeof(token)) {
                token[n++] = *p++;
            }
        }
        token[n] = '\0';
        if (strchr(token, '.') == NULL && strchr(token, '/') == NULL) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p == '"') {
                p++;
                n = 0;
                while (*p != '\0' && *p != '"' && n + 1 < sizeof(token)) {
                    token[n++] = *p++;
                }
                token[n] = '\0';
            }
        }
        if (strchr(token, '.') != NULL && strchr(token, '\\') == NULL) {
            /* skip keys like "file" without a path */
            if (strchr(token, '/') != NULL || strstr(token, ".wav") || strstr(token, ".mp3")
                || strstr(token, ".bsp") || strstr(token, ".mdl") || strstr(token, ".spr")
                || strstr(token, ".wad") || strstr(token, ".tga") || strstr(token, ".res")
                || strstr(token, ".txt")) {
                if (IsOptionalResEntry(token, path)) {
                    continue;
                }
                AddUnique(list, token);
            }
        }
    }
    fclose(f);
}

static const char *ReadA2SString(const char **p, const char *end)
{
    const char *start = *p;
    while (*p < end && **p != '\0') {
        (*p)++;
    }
    if (*p < end) {
        (*p)++;
    }
    return start;
}

static int QueryMapName(char *mapOut, size_t mapOutSize)
{
    SOCKET sock;
    struct sockaddr_in addr;
    char buf[2048];
    char req[64];
    int n;
    DWORD timeout = 2000;
    const char *p;
    const char *end;
    unsigned char type;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return 0;
    }
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_queryPort);
    if (inet_pton(AF_INET, g_queryHost, &addr.sin_addr) != 1) {
        closesocket(sock);
        return 0;
    }
    memcpy(req, "\xff\xff\xff\xffTSource Engine Query", 24);
    req[24] = '\0';
    sendto(sock, req, 25, 0, (struct sockaddr *)&addr, sizeof(addr));
    n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
    if (n >= 9 && (unsigned char)buf[4] == 0x41) {
        memcpy(req, buf, 9);
        req[4] = 'T';
        memcpy(req + 5, "Source Engine Query", 19);
        req[24] = '\0';
        memcpy(req + 25, buf + 5, 4);
        sendto(sock, req, 29, 0, (struct sockaddr *)&addr, sizeof(addr));
        n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
    }
    closesocket(sock);
    if (n < 6) {
        return 0;
    }
    buf[n] = '\0';
    p = buf + 4;
    end = buf + n;
    type = (unsigned char)*p++;
    if (type == 'I' || type == 0x49) {
        if (p >= end) {
            return 0;
        }
        p++; /* protocol */
        ReadA2SString(&p, end); /* name */
        lstrcpynA(mapOut, ReadA2SString(&p, end), (int)mapOutSize);
        return mapOut[0] != '\0';
    }
    if (type == 'm' || type == 0x6D) {
        ReadA2SString(&p, end); /* address */
        ReadA2SString(&p, end); /* name */
        lstrcpynA(mapOut, ReadA2SString(&p, end), (int)mapOutSize);
        return mapOut[0] != '\0';
    }
    return 0;
}

static void BeginDownloadUi(const char *label)
{
    SetStatus(label);
    if (InterlockedCompareExchange(&g_uiActive, 1, 0) == 0) {
        Overlay_Start();
    }
}

static DWORD WINAPI PrefetchThread(LPVOID unused)
{
    const char *gameRoot;
    char map[64];
    char rel[MAX_PATH];
    char dest[MAX_PATH];
    char url[768];
    std::vector<std::string> files;
    CURL *curl;
    WSADATA wsa;
    size_t i;

    (void)unused;
    InterlockedExchange(&g_uiActive, 0);
    InterlockedExchange(&g_totalFiles, 0);
    InterlockedExchange(&g_doneFiles, 0);
    InterlockedExchange(&g_filePermille, 0);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    gameRoot = BgSwitch_GetGameRoot();
    map[0] = '\0';
    SetStatus("");
    if (InterlockedCompareExchange(&g_stop, 0, 0) != 0) {
        goto done;
    }
    if (gameRoot != NULL && gameRoot[0] != '\0' && curl != NULL && QueryMapName(map, sizeof(map))) {
        _snprintf(rel, sizeof(rel), "maps/%s.bsp", map);
        AddUnique(&files, rel);
        _snprintf(rel, sizeof(rel), "maps/%s.res", map);
        AddUnique(&files, rel);
        if (!FileAlreadyPresent(gameRoot, rel)) {
            InterlockedExchange(&g_totalFiles, 1);
            InterlockedExchange(&g_doneFiles, 0);
            InterlockedExchange(&g_filePermille, 0);
            SetCurrentFile(rel);
            BeginDownloadUi(rel);
            BuildUrl(url, sizeof(url), rel);
            JoinDownloadFile(dest, sizeof(dest), gameRoot, rel);
            if (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
                CurlGet(curl, url, dest);
            }
            InterlockedExchange(&g_doneFiles, 1);
        }
        if (InterlockedCompareExchange(&g_stop, 0, 0) == 0
            && OpenLocalCopy(gameRoot, rel, dest, sizeof(dest))) {
            ParseResFile(dest, &files);
        }
    }

    {
        std::vector<std::string> missing;
        for (i = 0; i < files.size(); i++) {
            if (!FileAlreadyPresent(gameRoot, files[i].c_str())) {
                missing.push_back(files[i]);
            }
        }
        files.swap(missing);
    }

    if (curl != NULL && gameRoot != NULL && !files.empty()) {
        InterlockedExchange(&g_totalFiles, (LONG)files.size());
        InterlockedExchange(&g_doneFiles, 0);
        InterlockedExchange(&g_filePermille, 0);
        BeginDownloadUi(files[0].c_str());
        for (i = 0; i < files.size(); i++) {
            if (InterlockedCompareExchange(&g_stop, 0, 0) != 0) {
                break;
            }
            InterlockedExchange(&g_filePermille, 0);
            SetCurrentFile(files[i].c_str());
            BeginDownloadUi(files[i].c_str());
            JoinDownloadFile(dest, sizeof(dest), gameRoot, files[i].c_str());
            BuildUrl(url, sizeof(url), files[i].c_str());
            CurlGet(curl, url, dest);
            InterlockedExchange(&g_doneFiles, (LONG)(i + 1));
        }
    }

done:
    if (curl != NULL) {
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();
    WSACleanup();
    SetStatus("");
    SetCurrentFile("");
    InterlockedExchange(&g_uiActive, 0);
    return 0;
}

static void ReadIni(void)
{
    char ini[MAX_PATH];
    const char *root = BgSwitch_GetGameRoot();
    char port[32];

    g_fastDl[0] = '\0';
    g_queryHost[0] = '\0';
    if (root == NULL || root[0] == '\0') {
        return;
    }
    _snprintf(ini, sizeof(ini), "%s\\rev.ini", root);
    ini[sizeof(ini) - 1] = '\0';
    GetPrivateProfileStringA("Prefetch", "FastDL", "", g_fastDl, sizeof(g_fastDl), ini);
    GetPrivateProfileStringA("Prefetch", "Host", "", g_queryHost, sizeof(g_queryHost), ini);
    GetPrivateProfileStringA("Prefetch", "Port", "27015", port, sizeof(port), ini);
    g_queryPort = atoi(port);
    if (g_queryPort <= 0) {
        g_queryPort = 27015;
    }
}

void Prefetch_Bind(HMODULE hGameUI)
{
    g_gameUI = hGameUI;
}

void Prefetch_Pump(void)
{
    if (g_gameUI == NULL) {
        return;
    }
    if (InterlockedCompareExchange(&g_workerStarted, 1, 0) == 0) {
        ReadIni();
        if (g_fastDl[0] == '\0' || g_queryHost[0] == '\0') {
            return;
        }
        g_thread = CreateThread(NULL, 0, PrefetchThread, NULL, 0, NULL);
        if (g_thread != NULL) {
            HookLog("Prefetch: worker started");
        }
    }
}

void Prefetch_OnConnect(void)
{
    InterlockedExchange(&g_stop, 1);
}

int Prefetch_IsActive(void)
{
    return InterlockedCompareExchange(&g_uiActive, 0, 0) != 0;
}

void Prefetch_GetUi(int *active, int *permille, char *label, size_t labelSize)
{
    if (active != NULL) {
        *active = Prefetch_IsActive();
    }
    if (permille != NULL) {
        *permille = ComputePermille();
    }
    if (label != NULL && labelSize > 0) {
        lstrcpynA(label, g_status, (int)labelSize);
    }
}
