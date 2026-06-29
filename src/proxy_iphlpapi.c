/* proxy_iphlpapi.c - masquerade as iphlpapi.dll.
 *
 * Why iphlpapi:  diabotical.exe load-time imports exactly four symbols from it
 * (IcmpCreateFile, IcmpSendEcho, IcmpCloseHandle, GetAdaptersAddresses -- used
 * for server pings and adapter enumeration).  It is NOT a KnownDLL, so a copy
 * named iphlpapi.dll dropped in the game directory is loaded in preference to
 * the System32 one (default DLL search order looks at the app dir first).
 * Being a load-time import, it is mapped during process init -- before the EXE
 * entry point runs -- which is the earliest possible point to install hooks.
 *
 * We must re-export those four names (or the EXE fails to load) AND forward
 * them functionally (or pings break), so each export tail-calls the real
 * System32 iphlpapi.dll, resolved lazily on first use.
 *
 * To swap the proxy target, write a sibling proxy_<name>.c that re-exports
 * whatever the game imports from <name>, and build with PROXY=<name>.
 *
 * Aggregate pointer params are typed as void* on purpose: we forward bytes
 * verbatim, so the exact struct layouts are irrelevant and we avoid pulling in
 * (and colliding with) the Windows networking headers' own declarations. */
#include <windows.h>

typedef HANDLE (WINAPI *fn_IcmpCreateFile)(void);
typedef BOOL   (WINAPI *fn_IcmpCloseHandle)(HANDLE);
typedef DWORD  (WINAPI *fn_IcmpSendEcho)(HANDLE, ULONG, LPVOID, WORD,
                                         LPVOID, LPVOID, DWORD, DWORD);
typedef ULONG  (WINAPI *fn_GetAdaptersAddresses)(ULONG, ULONG, PVOID,
                                                 PVOID, PULONG);

static fn_IcmpCreateFile       p_IcmpCreateFile;
static fn_IcmpCloseHandle      p_IcmpCloseHandle;
static fn_IcmpSendEcho         p_IcmpSendEcho;
static fn_GetAdaptersAddresses p_GetAdaptersAddresses;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK resolve_real(PINIT_ONCE once, PVOID param, PVOID* ctx)
{
    (void)once; (void)param; (void)ctx;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);          /* no trailing slash */
    if (n == 0 || n + 14 >= MAX_PATH) return TRUE;
    wcscpy(path + n, L"\\iphlpapi.dll");
    HMODULE m = LoadLibraryW(path);
    if (m) {
        p_IcmpCreateFile       = (fn_IcmpCreateFile)      GetProcAddress(m, "IcmpCreateFile");
        p_IcmpCloseHandle      = (fn_IcmpCloseHandle)     GetProcAddress(m, "IcmpCloseHandle");
        p_IcmpSendEcho         = (fn_IcmpSendEcho)        GetProcAddress(m, "IcmpSendEcho");
        p_GetAdaptersAddresses = (fn_GetAdaptersAddresses)GetProcAddress(m, "GetAdaptersAddresses");
    }
    return TRUE;
}

static void ensure_real(void)
{
    InitOnceExecuteOnce(&g_once, resolve_real, NULL, NULL);
}

__declspec(dllexport) HANDLE WINAPI IcmpCreateFile(void)
{
    ensure_real();
    return p_IcmpCreateFile ? p_IcmpCreateFile() : INVALID_HANDLE_VALUE;
}

__declspec(dllexport) BOOL WINAPI IcmpCloseHandle(HANDLE h)
{
    ensure_real();
    return p_IcmpCloseHandle ? p_IcmpCloseHandle(h) : FALSE;
}

__declspec(dllexport) DWORD WINAPI IcmpSendEcho(HANDLE h, ULONG dst, LPVOID req,
                                                WORD reqsize, LPVOID opt,
                                                LPVOID reply, DWORD replysize,
                                                DWORD timeout)
{
    ensure_real();
    return p_IcmpSendEcho
         ? p_IcmpSendEcho(h, dst, req, reqsize, opt, reply, replysize, timeout)
         : 0;
}

__declspec(dllexport) ULONG WINAPI GetAdaptersAddresses(ULONG family, ULONG flags,
                                                        PVOID reserved, PVOID addrs,
                                                        PULONG size)
{
    ensure_real();
    return p_GetAdaptersAddresses
         ? p_GetAdaptersAddresses(family, flags, reserved, addrs, size)
         : ERROR_NOT_SUPPORTED;
}
