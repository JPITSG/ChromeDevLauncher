// Harmless doubles used only by test_updater.py's extracted-function harness.
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

typedef int BOOL;
typedef uint32_t DWORD;
typedef unsigned long long ULONGLONG;
typedef const wchar_t* LPCWSTR;
typedef wchar_t* LPWSTR;
typedef void* HANDLE;
typedef void* LPVOID;
typedef struct { unsigned cb; } STARTUPINFOW;
typedef struct { HANDLE hProcess, hThread; } PROCESS_INFORMATION;
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_INVALID_PARAMETER 87
#define CREATE_UNICODE_ENVIRONMENT 1024
#define ZeroMemory(p, size) memset(p, 0, size)

static int testArgc, applyCalls, cleanupCalls, applyReopen;
static wchar_t** testArgv;
static BOOL validCleanup;
static wchar_t capturedCommand[2048];

static void SetLastError(DWORD error) { (void)error; }
static void CloseHandle(HANDLE handle) { (void)handle; }
static void DestroyEnvironmentBlock(LPVOID env) { (void)env; }
static BOOL CreateEnvironmentBlock(LPVOID* env, HANDLE token, BOOL inherit) {
    (void)env; (void)token; (void)inherit;
    return FALSE;
}
static int wcscpy_s(wchar_t* dst, size_t size, LPCWSTR src) {
    if (wcslen(src) >= size) return 1;
    wcscpy(dst, src);
    return 0;
}
// Windows wide printf uses %s for wide strings; POSIX requires %ls.
static int swprintf_s(wchar_t* dst, size_t size, LPCWSTR format, ...) {
    wchar_t portable[1024];
    size_t j = 0;
    for (size_t i = 0; format[i]; ++i) {
        portable[j++] = format[i];
        if (format[i] == L'%' && format[i + 1] == L's') portable[j++] = L'l';
    }
    portable[j] = 0;
    va_list args;
    va_start(args, format);
    int result = vswprintf(dst, size, portable, args);
    va_end(args);
    return result;
}
static BOOL CreateProcessW(LPCWSTR path, LPWSTR command, void* a, void* b,
                          BOOL inherit, DWORD flags, void* env, void* cwd,
                          STARTUPINFOW* startup, PROCESS_INFORMATION* info) {
    (void)path; (void)a; (void)b; (void)inherit; (void)flags;
    (void)env; (void)cwd; (void)startup; (void)info;
    wcscpy(capturedCommand, command);
    return TRUE;
}
static BOOL CreateProcessWithTokenW(HANDLE token, DWORD logon, LPCWSTR path,
                                   LPWSTR command, DWORD flags, void* env,
                                   void* cwd, STARTUPINFOW* startup,
                                   PROCESS_INFORMATION* info) {
    (void)token; (void)logon;
    return CreateProcessW(path, command, NULL, NULL, FALSE, flags,
                          env, cwd, startup, info);
}
static LPCWSTR GetCommandLineW(void) { return L""; }
static LPWSTR* CommandLineToArgvW(LPCWSTR command, int* count) {
    (void)command;
    *count = testArgc;
    return testArgv;
}
static void LocalFree(void* memory) { (void)memory; }
static int RunUpdateApplyHelper(DWORD pid, LPCWSTR event, LPCWSTR target,
                                LPCWSTR staged, BOOL reopen) {
    (void)pid; (void)event; (void)target; (void)staged;
    ++applyCalls;
    applyReopen = reopen;
    return 0;
}
static BOOL FinishUpdateCleanup(DWORD helper, DWORD old, LPCWSTR staged,
                                LPCWSTR path) {
    (void)helper; (void)old; (void)staged; (void)path;
    ++cleanupCalls;
    return validCleanup;
}
