// In-memory Windows registry doubles for test_startup.py. No real startup
// entries or processes are created by these tests.
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

typedef int BOOL;
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef unsigned char BYTE;
typedef void* HKEY;
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define HKEY_CURRENT_USER ((HKEY)1)
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED 5
#define ERROR_BAD_PATHNAME 161
#define ERROR_MORE_DATA 234
#define REG_SZ 1
#define RRF_RT_REG_SZ 2
#define RRF_RT_REG_BINARY 8
#define REG_OPTION_NON_VOLATILE 0
#define KEY_SET_VALUE 2
#define _wcsicmp wcscasecmp

static wchar_t modulePath[1024], runCommand[1024];
static BOOL hasRun;
static int approvedMarker, failOperation;

static DWORD GetModuleFileNameW(void* module, wchar_t* path, DWORD capacity) {
    (void)module;
    size_t length = wcslen(modulePath);
    if (length >= capacity) return capacity;
    wcscpy(path, modulePath);
    return (DWORD)length;
}

// The one Windows wide printf format used by GetStartupCommand.
static int swprintf_s(wchar_t* out, size_t size, const wchar_t* format,
                       const wchar_t* path) {
    assert(wcscmp(format, L"\"%s\"") == 0);
    return swprintf(out, size, L"\"%ls\"", path);
}

static LONG RegGetValueW(HKEY root, const wchar_t* key, const wchar_t* name,
                         DWORD flags, DWORD* type, void* out, DWORD* size) {
    (void)type;
    assert(root == HKEY_CURRENT_USER && wcscmp(name, REG_APPNAME) == 0);
    if (wcscmp(key, STARTUP_RUN_KEY_W) == 0) {
        assert(flags == RRF_RT_REG_SZ);
        if (!hasRun) return ERROR_FILE_NOT_FOUND;
        DWORD bytes = (wcslen(runCommand) + 1) * sizeof(wchar_t);
        if (*size < bytes) return ERROR_MORE_DATA;
        memcpy(out, runCommand, bytes);
        *size = bytes;
    } else {
        assert(wcscmp(key, STARTUP_APPROVED_RUN_KEY_W) == 0);
        assert(flags == RRF_RT_REG_BINARY);
        if (approvedMarker < 0) return ERROR_FILE_NOT_FOUND;
        assert(*size >= 12);
        memset(out, 0, 12);
        *(BYTE*)out = (BYTE)approvedMarker;
        *size = 12;
    }
    return ERROR_SUCCESS;
}

static LONG RegCreateKeyExW(HKEY root, const wchar_t* key, DWORD reserved,
                             void* cls, DWORD options, DWORD access,
                             void* security, HKEY* out, DWORD* disposition) {
    (void)reserved; (void)cls; (void)options; (void)security; (void)disposition;
    assert(root == HKEY_CURRENT_USER && wcscmp(key, STARTUP_RUN_KEY_W) == 0);
    assert(access == KEY_SET_VALUE);
    if (failOperation == 1) return ERROR_ACCESS_DENIED;
    *out = (HKEY)2;
    return ERROR_SUCCESS;
}

static LONG RegSetValueExW(HKEY key, const wchar_t* name, DWORD reserved,
                            DWORD type, const BYTE* data, DWORD size) {
    (void)reserved;
    assert(key == (HKEY)2 && wcscmp(name, REG_APPNAME) == 0 && type == REG_SZ);
    assert(size == (wcslen((const wchar_t*)data) + 1) * sizeof(wchar_t));
    assert(size <= sizeof(runCommand));
    if (failOperation == 2) return ERROR_ACCESS_DENIED;
    memcpy(runCommand, data, size);
    hasRun = TRUE;
    return ERROR_SUCCESS;
}

static LONG RegCloseKey(HKEY key) {
    assert(key == (HKEY)2);
    return ERROR_SUCCESS;
}

static LONG RegDeleteKeyValueW(HKEY root, const wchar_t* key, const wchar_t* name) {
    assert(root == HKEY_CURRENT_USER && wcscmp(name, REG_APPNAME) == 0);
    if (wcscmp(key, STARTUP_RUN_KEY_W) == 0) {
        if (failOperation == 3) return ERROR_ACCESS_DENIED;
        if (!hasRun) return ERROR_FILE_NOT_FOUND;
        hasRun = FALSE;
    } else {
        assert(wcscmp(key, STARTUP_APPROVED_RUN_KEY_W) == 0);
        if (failOperation == 4) return ERROR_ACCESS_DENIED;
        if (approvedMarker < 0) return ERROR_FILE_NOT_FOUND;
        approvedMarker = -1;
    }
    return ERROR_SUCCESS;
}
