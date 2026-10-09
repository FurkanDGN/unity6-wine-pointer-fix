/*
 * version.dll sidecar: bridges Win32 pointer input (WM_POINTER / EnableMouseInPointer)
 * for Unity 6 games running under Wine, where NtUserEnableMouseInPointer is a stub.
 *
 * Approach (mirrors the wine-staging style patch for bug #53847):
 *  - EnableMouseInPointer / IsMouseInPointerEnabled report success.
 *  - Legacy WM_MOUSE* messages retrieved by the game's message loop are mirrored
 *    as synthesized WM_POINTER* messages sent directly to the window proc.
 *  - GetPointerType / GetPointerInfo / frame variants serve the last mouse frame.
 *
 * Interception is done only via IAT patching of UnityPlayer.dll and the game
 * executable (user32 imports + kernel32!GetProcAddress), so nothing depends on
 * Wine's builtin PE layout. No Direct3D hooking of any kind.
 */

#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>

/* ---------- logging (opt-in: UNITY6_PTRFIX_LOG=1) ---------- */

static void ptrfix_log(const char *fmt, ...)
{
    static int state; /* 0 unknown, 1 on, 2 off */
    static CRITICAL_SECTION cs;
    static LONG cs_init;
    char buf[16], line[512];
    va_list ap;
    HANDLE h;
    DWORD n, wrote;

    if (!state)
        state = GetEnvironmentVariableA("UNITY6_PTRFIX_LOG", buf, sizeof(buf)) > 0 ? 1 : 2;
    if (state != 1) return;

    if (!InterlockedCompareExchange(&cs_init, 1, 0)) InitializeCriticalSection(&cs);

    va_start(ap, fmt);
    n = (DWORD)vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n >= sizeof(line) - 2) n = sizeof(line) - 3;
    line[n++] = '\r'; line[n++] = '\n';

    EnterCriticalSection(&cs);
    h = CreateFileA("unity6_ptrfix.log", FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        WriteFile(h, line, n, &wrote, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&cs);
}

/* ---------- pointer frame state ---------- */

static CRITICAL_SECTION g_frame_cs;
static POINTER_INFO g_frame;        /* last synthesized mouse frame */
static DWORD g_buttons;             /* currently held buttons as POINTER_MESSAGE_FLAG_* bits */
static LONG g_emip_called;          /* EnableMouseInPointer seen (informational only) */

#ifndef POINTER_MESSAGE_FLAG_NEW
#define POINTER_MESSAGE_FLAG_NEW          0x00000001
#define POINTER_MESSAGE_FLAG_INRANGE      0x00000002
#define POINTER_MESSAGE_FLAG_INCONTACT    0x00000004
#define POINTER_MESSAGE_FLAG_FIRSTBUTTON  0x00000010
#define POINTER_MESSAGE_FLAG_SECONDBUTTON 0x00000020
#define POINTER_MESSAGE_FLAG_THIRDBUTTON  0x00000040
#define POINTER_MESSAGE_FLAG_FOURTHBUTTON 0x00000080
#define POINTER_MESSAGE_FLAG_FIFTHBUTTON  0x00000100
#define POINTER_MESSAGE_FLAG_PRIMARY      0x00002000
#define POINTER_MESSAGE_FLAG_CONFIDENCE   0x00004000
#endif

#define PTRFIX_POINTER_ID 1

static void synth_pointer_message(const MSG *msg)
{
    LARGE_INTEGER counter;
    POINTER_INFO info;
    DWORD message = 0;
    DWORD flags = 0x20000 /* POINTER_FLAG_UPDATE */ | POINTER_MESSAGE_FLAG_PRIMARY
                  | POINTER_MESSAGE_FLAG_CONFIDENCE;
    POINTER_BUTTON_CHANGE_TYPE change = POINTER_CHANGE_NONE;
    WPARAM wparam;

    switch (msg->message)
    {
    case WM_MOUSEMOVE:
        message = WM_POINTERUPDATE;
        flags |= POINTER_MESSAGE_FLAG_INRANGE | g_buttons;
        if (g_buttons) flags |= POINTER_MESSAGE_FLAG_INCONTACT;
        break;
    case WM_LBUTTONDOWN:
        message = WM_POINTERDOWN;
        g_buttons |= POINTER_MESSAGE_FLAG_FIRSTBUTTON;
        change = POINTER_CHANGE_FIRSTBUTTON_DOWN;
        break;
    case WM_RBUTTONDOWN:
        message = WM_POINTERDOWN;
        g_buttons |= POINTER_MESSAGE_FLAG_SECONDBUTTON;
        change = POINTER_CHANGE_SECONDBUTTON_DOWN;
        break;
    case WM_MBUTTONDOWN:
        message = WM_POINTERDOWN;
        g_buttons |= POINTER_MESSAGE_FLAG_THIRDBUTTON;
        change = POINTER_CHANGE_THIRDBUTTON_DOWN;
        break;
    case WM_XBUTTONDOWN:
        message = WM_POINTERDOWN;
        if (HIWORD(msg->wParam) == XBUTTON1)
        {
            g_buttons |= POINTER_MESSAGE_FLAG_FOURTHBUTTON;
            change = POINTER_CHANGE_FOURTHBUTTON_DOWN;
        }
        else
        {
            g_buttons |= POINTER_MESSAGE_FLAG_FIFTHBUTTON;
            change = POINTER_CHANGE_FIFTHBUTTON_DOWN;
        }
        break;
    case WM_LBUTTONUP:
        message = WM_POINTERUP;
        g_buttons &= ~POINTER_MESSAGE_FLAG_FIRSTBUTTON;
        change = POINTER_CHANGE_FIRSTBUTTON_UP;
        break;
    case WM_RBUTTONUP:
        message = WM_POINTERUP;
        g_buttons &= ~POINTER_MESSAGE_FLAG_SECONDBUTTON;
        change = POINTER_CHANGE_SECONDBUTTON_UP;
        break;
    case WM_MBUTTONUP:
        message = WM_POINTERUP;
        g_buttons &= ~POINTER_MESSAGE_FLAG_THIRDBUTTON;
        change = POINTER_CHANGE_THIRDBUTTON_UP;
        break;
    case WM_XBUTTONUP:
        message = WM_POINTERUP;
        if (HIWORD(msg->wParam) == XBUTTON1)
        {
            g_buttons &= ~POINTER_MESSAGE_FLAG_FOURTHBUTTON;
            change = POINTER_CHANGE_FOURTHBUTTON_UP;
        }
        else
        {
            g_buttons &= ~POINTER_MESSAGE_FLAG_FIFTHBUTTON;
            change = POINTER_CHANGE_FIFTHBUTTON_UP;
        }
        break;
    case WM_MOUSEWHEEL:
        message = WM_POINTERWHEEL;
        break;
    case WM_MOUSEHWHEEL:
        message = WM_POINTERHWHEEL;
        break;
    default:
        return;
    }

    if (message == WM_POINTERDOWN)
        flags |= POINTER_MESSAGE_FLAG_INRANGE | POINTER_MESSAGE_FLAG_INCONTACT | g_buttons;
    if (message == WM_POINTERUP)
        flags |= POINTER_MESSAGE_FLAG_INRANGE | g_buttons;

    QueryPerformanceCounter(&counter);

    memset(&info, 0, sizeof(info));
    info.pointerType = PT_MOUSE;
    info.pointerId = PTRFIX_POINTER_ID;
    info.sourceDevice = INVALID_HANDLE_VALUE;
    info.historyCount = 1;
    info.pointerFlags = flags;
    info.hwndTarget = msg->hwnd;
    info.ptPixelLocation = msg->pt;
    info.ptHimetricLocation = msg->pt;
    info.ptPixelLocationRaw = msg->pt;
    info.ptHimetricLocationRaw = msg->pt;
    info.dwTime = msg->time;
    info.PerformanceCount = counter.QuadPart;
    info.ButtonChangeType = change;

    EnterCriticalSection(&g_frame_cs);
    info.frameId = g_frame.frameId + 1;
    g_frame = info;
    LeaveCriticalSection(&g_frame_cs);

    if (message == WM_POINTERWHEEL || message == WM_POINTERHWHEEL)
        wparam = MAKELONG(PTRFIX_POINTER_ID, HIWORD(msg->wParam)); /* wheel delta in high word */
    else
        wparam = MAKELONG(PTRFIX_POINTER_ID, LOWORD(flags));

    SendMessageW(msg->hwnd, message, wparam, MAKELONG(msg->pt.x, msg->pt.y));
}

static void maybe_synth(const MSG *msg, BOOL removed)
{
    if (!removed || !msg) return;
    if (msg->message < WM_MOUSEFIRST || msg->message > WM_MOUSELAST) return;
    if (!msg->hwnd) return;
    synth_pointer_message(msg);
}

/* ---------- replacement pointer APIs ---------- */

static BOOL WINAPI Hook_EnableMouseInPointer(BOOL enable)
{
    InterlockedExchange(&g_emip_called, 1);
    ptrfix_log("[ptr] EnableMouseInPointer(%d) -> TRUE", enable);
    return TRUE;
}

static BOOL WINAPI Hook_IsMouseInPointerEnabled(void)
{
    return TRUE;
}

static BOOL WINAPI Hook_GetPointerType(UINT32 id, POINTER_INPUT_TYPE *type)
{
    if (!type) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *type = PT_MOUSE;
    return TRUE;
}

static BOOL get_frame_copy(UINT32 id, POINTER_INFO *out)
{
    BOOL ok;
    EnterCriticalSection(&g_frame_cs);
    ok = g_frame.frameId && id == PTRFIX_POINTER_ID;
    if (ok) *out = g_frame;
    LeaveCriticalSection(&g_frame_cs);
    return ok;
}

static BOOL WINAPI Hook_GetPointerInfo(UINT32 id, POINTER_INFO *info)
{
    if (!info || !get_frame_copy(id, info))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI Hook_GetPointerFrameInfo(UINT32 id, UINT32 *count, POINTER_INFO *info)
{
    if (!count) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!info) { *count = 1; return TRUE; }
    if (!*count || !get_frame_copy(id, info))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *count = 1;
    return TRUE;
}

static BOOL WINAPI Hook_GetPointerInfoHistory(UINT32 id, UINT32 *count, POINTER_INFO *info)
{
    return Hook_GetPointerFrameInfo(id, count, info);
}

static BOOL WINAPI Hook_GetPointerFrameInfoHistory(UINT32 id, UINT32 *row_count, UINT32 *col_count,
                                                   POINTER_INFO *info)
{
    if (!row_count || !col_count) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *row_count = 1;
    return Hook_GetPointerFrameInfo(id, col_count, info);
}

static BOOL WINAPI Hook_SkipPointerFrameMessages(UINT32 id)
{
    return TRUE;
}

static BOOL WINAPI Hook_GetPointerCursorId(UINT32 id, UINT32 *cursor_id)
{
    if (!cursor_id) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *cursor_id = 0;
    return TRUE;
}

/* ---------- message loop wrappers ---------- */

typedef BOOL (WINAPI *PFN_PeekMessageW)(MSG *, HWND, UINT, UINT, UINT);
typedef BOOL (WINAPI *PFN_PeekMessageA)(MSG *, HWND, UINT, UINT, UINT);
typedef BOOL (WINAPI *PFN_GetMessageW)(MSG *, HWND, UINT, UINT);
typedef BOOL (WINAPI *PFN_GetMessageA)(MSG *, HWND, UINT, UINT);

static PFN_PeekMessageW g_real_PeekMessageW;
static PFN_PeekMessageA g_real_PeekMessageA;
static PFN_GetMessageW g_real_GetMessageW;
static PFN_GetMessageA g_real_GetMessageA;

static BOOL WINAPI Hook_PeekMessageW(MSG *msg, HWND hwnd, UINT min, UINT max, UINT remove)
{
    BOOL ret = g_real_PeekMessageW(msg, hwnd, min, max, remove);
    if (ret && (remove & PM_REMOVE)) maybe_synth(msg, TRUE);
    return ret;
}

static BOOL WINAPI Hook_PeekMessageA(MSG *msg, HWND hwnd, UINT min, UINT max, UINT remove)
{
    BOOL ret = g_real_PeekMessageA(msg, hwnd, min, max, remove);
    if (ret && (remove & PM_REMOVE)) maybe_synth(msg, TRUE);
    return ret;
}

static BOOL WINAPI Hook_GetMessageW(MSG *msg, HWND hwnd, UINT min, UINT max)
{
    BOOL ret = g_real_GetMessageW(msg, hwnd, min, max);
    if (ret > 0) maybe_synth(msg, TRUE);
    return ret;
}

static BOOL WINAPI Hook_GetMessageA(MSG *msg, HWND hwnd, UINT min, UINT max)
{
    BOOL ret = g_real_GetMessageA(msg, hwnd, min, max);
    if (ret > 0) maybe_synth(msg, TRUE);
    return ret;
}

/* ---------- replacement table ---------- */

struct hook_entry
{
    const char *name;
    void *hook;
    void **real_slot; /* filled with the original pointer when first seen, may be NULL */
};

static struct hook_entry g_user32_hooks[] =
{
    { "EnableMouseInPointer",       (void *)Hook_EnableMouseInPointer,       NULL },
    { "IsMouseInPointerEnabled",    (void *)Hook_IsMouseInPointerEnabled,    NULL },
    { "GetPointerType",             (void *)Hook_GetPointerType,             NULL },
    { "GetPointerInfo",             (void *)Hook_GetPointerInfo,             NULL },
    { "GetPointerFrameInfo",        (void *)Hook_GetPointerFrameInfo,        NULL },
    { "GetPointerInfoHistory",      (void *)Hook_GetPointerInfoHistory,      NULL },
    { "GetPointerFrameInfoHistory", (void *)Hook_GetPointerFrameInfoHistory, NULL },
    { "SkipPointerFrameMessages",   (void *)Hook_SkipPointerFrameMessages,   NULL },
    { "GetPointerCursorId",         (void *)Hook_GetPointerCursorId,         NULL },
    { "PeekMessageW",               (void *)Hook_PeekMessageW,               (void **)&g_real_PeekMessageW },
    { "PeekMessageA",               (void *)Hook_PeekMessageA,               (void **)&g_real_PeekMessageA },
    { "GetMessageW",                (void *)Hook_GetMessageW,                (void **)&g_real_GetMessageW },
    { "GetMessageA",                (void *)Hook_GetMessageA,                (void **)&g_real_GetMessageA },
};

static void *lookup_user32_hook(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof(g_user32_hooks) / sizeof(g_user32_hooks[0]); i++)
        if (!lstrcmpA(name, g_user32_hooks[i].name)) return g_user32_hooks[i].hook;
    return NULL;
}

/* ---------- GetProcAddress wrapper (Unity resolves pointer APIs dynamically) ---------- */

typedef FARPROC (WINAPI *PFN_GetProcAddress)(HMODULE, LPCSTR);
static PFN_GetProcAddress g_real_GetProcAddress;

static FARPROC WINAPI Hook_GetProcAddress(HMODULE module, LPCSTR name)
{
    if (name && HIWORD((ULONG_PTR)name) && module == GetModuleHandleA("user32.dll"))
    {
        void *hook = lookup_user32_hook(name);
        if (hook)
        {
            ptrfix_log("[ptr] GetProcAddress(user32, %s) -> hook", name);
            return (FARPROC)hook;
        }
    }
    return g_real_GetProcAddress(module, name);
}

/* ---------- IAT patching ---------- */

static int patch_module_iat(HMODULE module, const char *tag)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)module;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD imp_rva, old;
    BYTE *base = (BYTE *)module;
    int patched = 0;

    if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    imp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva) return 0;

    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + imp_rva); imp->Name; imp++)
    {
        const char *dll = (const char *)(base + imp->Name);
        IMAGE_THUNK_DATA *thunk, *orig;
        int is_user32 = !lstrcmpiA(dll, "user32.dll");
        int is_kernel32 = !lstrcmpiA(dll, "kernel32.dll") || !lstrcmpiA(dll, "kernelbase.dll");

        if (!is_user32 && !is_kernel32) continue;
        if (!imp->FirstThunk || !imp->OriginalFirstThunk) continue;

        thunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        orig = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);

        for (; orig->u1.AddressOfData; thunk++, orig++)
        {
            IMAGE_IMPORT_BY_NAME *ibn;
            void *hook = NULL;
            void **real_slot = NULL;
            const char *name;
            size_t i;

            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + orig->u1.AddressOfData);
            name = (const char *)ibn->Name;

            if (is_user32)
            {
                for (i = 0; i < sizeof(g_user32_hooks) / sizeof(g_user32_hooks[0]); i++)
                {
                    if (!lstrcmpA(name, g_user32_hooks[i].name))
                    {
                        hook = g_user32_hooks[i].hook;
                        real_slot = g_user32_hooks[i].real_slot;
                        break;
                    }
                }
            }
            else if (!lstrcmpA(name, "GetProcAddress"))
            {
                hook = (void *)Hook_GetProcAddress;
            }

            if (!hook || (void *)thunk->u1.Function == hook) continue;

            if (real_slot && !*real_slot) *real_slot = (void *)thunk->u1.Function;
            if (hook == (void *)Hook_GetProcAddress && !g_real_GetProcAddress)
                g_real_GetProcAddress = (PFN_GetProcAddress)thunk->u1.Function;

            if (VirtualProtect(&thunk->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
            {
                thunk->u1.Function = (ULONG_PTR)hook;
                VirtualProtect(&thunk->u1.Function, sizeof(void *), old, &old);
                patched++;
                ptrfix_log("[ptr] %s: patched %s!%s", tag, dll, name);
            }
        }
    }
    return patched;
}

static DWORD WINAPI patch_thread(void *arg)
{
    HMODULE unity = NULL;
    int tries;

    /* The exe can be patched right away. */
    patch_module_iat(GetModuleHandleA(NULL), "exe");

    for (tries = 0; tries < 1200; tries++) /* up to ~2 minutes */
    {
        unity = GetModuleHandleA("UnityPlayer.dll");
        if (unity) break;
        Sleep(100);
    }

    if (!unity)
    {
        ptrfix_log("[ptr] UnityPlayer.dll never appeared");
        return 0;
    }

    /* Fallbacks for the message wrappers in case the import is absent. */
    if (!g_real_PeekMessageW) g_real_PeekMessageW = PeekMessageW;
    if (!g_real_PeekMessageA) g_real_PeekMessageA = PeekMessageA;
    if (!g_real_GetMessageW) g_real_GetMessageW = GetMessageW;
    if (!g_real_GetMessageA) g_real_GetMessageA = GetMessageA;
    if (!g_real_GetProcAddress) g_real_GetProcAddress = GetProcAddress;

    ptrfix_log("[ptr] UnityPlayer.dll found, patching IAT");
    patch_module_iat(unity, "UnityPlayer");
    return 0;
}

/* ---------- version.dll forwards ---------- */

static HMODULE g_ver_real;

static FARPROC ver_real(const char *name)
{
    if (!g_ver_real)
    {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n && n < MAX_PATH - 16)
        {
            lstrcatA(path, "\\version.dll");
            g_ver_real = LoadLibraryA(path);
        }
        if (!g_ver_real) ptrfix_log("[ptr] FATAL: cannot load real version.dll");
    }
    return g_ver_real ? GetProcAddress(g_ver_real, name) : NULL;
}

#define FWD(ret, name, args, call)                        \
    ret WINAPI name args                                  \
    {                                                     \
        typedef ret (WINAPI *fn) args;                    \
        static fn real;                                   \
        if (!real) real = (fn)ver_real(#name);            \
        if (!real) return (ret)0;                         \
        return real call;                                 \
    }

FWD(BOOL, GetFileVersionInfoA, (LPCSTR a, DWORD b, DWORD c, LPVOID d), (a, b, c, d))
FWD(BOOL, GetFileVersionInfoW, (LPCWSTR a, DWORD b, DWORD c, LPVOID d), (a, b, c, d))
FWD(DWORD, GetFileVersionInfoSizeA, (LPCSTR a, LPDWORD b), (a, b))
FWD(DWORD, GetFileVersionInfoSizeW, (LPCWSTR a, LPDWORD b), (a, b))
FWD(BOOL, GetFileVersionInfoExA, (DWORD f, LPCSTR a, DWORD b, DWORD c, LPVOID d), (f, a, b, c, d))
FWD(BOOL, GetFileVersionInfoExW, (DWORD f, LPCWSTR a, DWORD b, DWORD c, LPVOID d), (f, a, b, c, d))
FWD(DWORD, GetFileVersionInfoSizeExA, (DWORD f, LPCSTR a, LPDWORD b), (f, a, b))
FWD(DWORD, GetFileVersionInfoSizeExW, (DWORD f, LPCWSTR a, LPDWORD b), (f, a, b))
FWD(BOOL, VerQueryValueA, (LPCVOID a, LPCSTR b, LPVOID *c, PUINT d), (a, b, c, d))
FWD(BOOL, VerQueryValueW, (LPCVOID a, LPCWSTR b, LPVOID *c, PUINT d), (a, b, c, d))

/* ---------- entry point ---------- */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        HMODULE unity;
        HANDLE thread;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_frame_cs);

        /* Static-import fallbacks so the wrappers work even when a module
         * resolves these dynamically before its IAT entry is seen. */
        g_real_PeekMessageW = PeekMessageW;
        g_real_PeekMessageA = PeekMessageA;
        g_real_GetMessageW = GetMessageW;
        g_real_GetMessageA = GetMessageA;
        g_real_GetProcAddress = GetProcAddress;

        ptrfix_log("[ptr] attached");

        /* If this dll was pulled in as a dependency of UnityPlayer.dll, the
         * loader has already resolved UnityPlayer's imports, so patch its IAT
         * before any of its code runs. Only VirtualProtect and memory writes
         * happen here, which is safe under the loader lock. */
        patch_module_iat(GetModuleHandleA(NULL), "exe");
        if ((unity = GetModuleHandleA("UnityPlayer.dll")))
            patch_module_iat(unity, "UnityPlayer(early)");

        thread = CreateThread(NULL, 0, patch_thread, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
