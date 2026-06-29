/* proxy_gfsdk.c - masquerade as GFSDK_SSAO_D3D11.win64.dll (cross-platform).
 *
 * Why this target instead of iphlpapi:  iphlpapi is a *system* DLL.  On Wine /
 * Proton the loader prefers Wine's own builtin iphlpapi and ignores a native
 * copy dropped in the game directory unless you set WINEDLLOVERRIDES -- so the
 * proxy never loads and no patch is applied.  GFSDK_SSAO_D3D11.win64.dll is an
 * *application* DLL that ships next to diabotical.exe; Wine has no builtin for
 * it, so BOTH Windows and Wine load our app-dir copy with no launch options.
 *
 * diabotical.exe load-time imports exactly ONE symbol from it, by name:
 *   GFSDK_SSAO_CreateContext_D3D11   (NVIDIA HBAO+ ambient-occlusion context).
 * Being a load-time import it maps during process init -- before the EXE entry
 * point -- the earliest point to install hooks.
 *
 * Forwarding without recursion:  the iphlpapi proxy forwarded by re-loading a
 * same-named DLL, which the loader keys by BASE NAME -- so on both Windows and
 * Wine it would hand back *this* module and the forwarder would call itself.
 * We avoid that entirely: the real DLL is RENAMED to a different base name
 * (gfsdk_ssao_orig.dll) at deploy time, and we load THAT.  Different base name
 * => no self-reference, on either platform.
 *
 * DEPLOY (both Windows and Linux/Proton, no launch options):
 *   1. rename  GFSDK_SSAO_D3D11.win64.dll  ->  gfsdk_ssao_orig.dll
 *   2. drop our build output as GFSDK_SSAO_D3D11.win64.dll next to diabotical.exe
 *
 * Build metadata read by build.sh:
 *   PK-DEPLOY-AS: GFSDK_SSAO_D3D11.win64.dll
 *   PK-RENAME-ORIG: GFSDK_SSAO_D3D11.win64.dll -> gfsdk_ssao_orig.dll
 *
 * The forwarded function is typed with opaque pointer params: we forward the x64
 * register arguments verbatim, so the exact NVIDIA struct layouts are irrelevant.
 * It takes FOUR args, not three -- the real signature is
 *   GFSDK_SSAO_CreateContext_D3D11(ID3D11Device*, GFSDK_SSAO_Context_D3D11**,
 *                                  const GFSDK_SSAO_CustomHeap* = NULL,
 *                                  GFSDK_SSAO_Version HeaderVersion = {})
 * The 4th param is a 16-byte version struct; on the x64 ABI structs >8 bytes are
 * passed BY REFERENCE, so the game hands a pointer to it in R9.  An earlier
 * 3-arg forwarder dropped R9, the real function dereferenced the stale register,
 * and the game crashed.  We must pass all four register args through. */
#include "patchkit.h"
#include <windows.h>

#define ORIG_DLL_NAME L"gfsdk_ssao_orig.dll"

typedef unsigned int (*fn_CreateContext)(void*, void*, void*, void*);
static fn_CreateContext p_CreateContext;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

/* Resolve THIS proxy's own HMODULE from the address of a function inside it, so
 * we can find the renamed original sitting beside us regardless of the process
 * working directory -- no DllMain cooperation needed (loader.c owns DllMain). */
static HMODULE self_module(void)
{
    HMODULE h = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void*)&self_module, &h);
    return h;
}

static BOOL CALLBACK resolve_real(PINIT_ONCE once, PVOID param, PVOID* ctx)
{
    (void)once; (void)param; (void)ctx;

    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self_module(), path, MAX_PATH); /* ...\GFSDK_SSAO_D3D11.win64.dll */
    if (n == 0 || n >= MAX_PATH) return TRUE;
    while (n > 0 && path[n - 1] != L'\\') n--;               /* strip to dir + trailing '\' */
    if (n + (DWORD)(sizeof(ORIG_DLL_NAME) / sizeof(wchar_t)) >= MAX_PATH) return TRUE;
    wcscpy(path + n, ORIG_DLL_NAME);

    HMODULE m = LoadLibraryW(path);                          /* distinct base name => no recursion */
    if (m) {
        p_CreateContext = (fn_CreateContext)GetProcAddress(m, "GFSDK_SSAO_CreateContext_D3D11");
        pk_logf("proxy_gfsdk: loaded %ls, CreateContext=%p", ORIG_DLL_NAME, (void*)p_CreateContext);
    } else {
        pk_logf("proxy_gfsdk: FAILED to load %ls (err %lu) - AO will be disabled",
                ORIG_DLL_NAME, GetLastError());
    }
    return TRUE;
}

static void ensure_real(void)
{
    InitOnceExecuteOnce(&g_once, resolve_real, NULL, NULL);
}

/* GFSDK_SSAO_Status GFSDK_SSAO_CreateContext_D3D11(...); 0 == GFSDK_SSAO_OK,
 * non-zero == error.  If forwarding is unavailable we return a non-zero status
 * so the game cleanly disables AO instead of dereferencing a null context. */
__declspec(dllexport) unsigned int GFSDK_SSAO_CreateContext_D3D11(void* dev, void* ctx,
                                                                  void* heap, void* version)
{
    ensure_real();
    return p_CreateContext ? p_CreateContext(dev, ctx, heap, version) : 1u;
}
