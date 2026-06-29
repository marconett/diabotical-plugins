/* patchkit.c - implementation of the runtime-patching helpers. */
#include "patchkit.h"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ module */

static uintptr_t g_base;

uintptr_t pk_base(void)
{
    if (!g_base)
        g_base = (uintptr_t)GetModuleHandleW(NULL);   /* the EXE image */
    return g_base;
}

void* pk_ptr(uint32_t rva) { return (void*)(pk_base() + rva); }

/* ----------------------------------------------------------------- logging */

static wchar_t g_logpath[MAX_PATH] = L"patchkit.log";

void pk_set_self(void* hmodule)
{
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW((HMODULE)hmodule, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    while (n > 0 && p[n - 1] != L'\\') n--;     /* strip filename, keep dir + '\' */
    if (n + 13 >= MAX_PATH) return;             /* len("patchkit.log")+1 */
    wcsncpy(g_logpath, p, n);
    wcscpy(g_logpath + n, L"patchkit.log");
}

void pk_logf(const char* fmt, ...)
{
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(buf, sizeof buf - 2, fmt, a);
    va_end(a);
    if (k < 0) k = 0;
    if (k > (int)sizeof buf - 2) k = (int)sizeof buf - 2;
    buf[k++] = '\r';
    buf[k++] = '\n';

    OutputDebugStringA(buf);

    HANDLE h = CreateFileW(g_logpath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        SetFilePointer(h, 0, NULL, FILE_END);
        WriteFile(h, buf, (DWORD)k, &w, NULL);
        CloseHandle(h);
    }
}

/* ----------------------------------------------------------- raw patching */

int pk_verify(uint32_t rva, const void* expect, size_t n)
{
    return memcmp(pk_ptr(rva), expect, n) == 0;
}

int pk_write(uint32_t rva, const void* bytes, size_t n)
{
    void* dst = pk_ptr(rva);
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old))
        return PK_FAIL;
    memcpy(dst, bytes, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return PK_OK;
}

int pk_apply(uint32_t rva, const void* expect, size_t en,
             const void* repl, size_t rn, const char* label)
{
    const uint8_t* p = (const uint8_t*)pk_ptr(rva);
    if (memcmp(p, repl, rn) == 0) {
        pk_logf("%s: already applied @0x%x", label, rva);
        return PK_OK;
    }
    if (memcmp(p, expect, en) != 0) {
        pk_logf("%s: mismatch @0x%x; aborting", label, rva);
        return PK_FAIL;
    }
    if (!pk_write(rva, repl, rn)) {
        pk_logf("%s: write failed @0x%x", label, rva);
        return PK_FAIL;
    }
    pk_logf("%s: applied @0x%x (%zu bytes)", label, rva, rn);
    return PK_OK;
}

/* ------------------------------------------------------------- code caves */

/* Allocate executable memory within +-2GB of `target` so a rel32 branch can
 * reach it.  Scans the free address space downward then upward from target. */
static void* alloc_near(uintptr_t target, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t span = 0x7FFF0000;                 /* stay comfortably under 2GB */
    uintptr_t lo   = target > span ? target - span : 0x10000;
    uintptr_t hi   = target + span;
    MEMORY_BASIC_INFORMATION mbi;

    for (uintptr_t a = target & ~(gran - 1); a > lo; a -= gran) {
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_FREE) {
            void* p = VirtualAlloc((void*)a, size, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    for (uintptr_t a = (target & ~(gran - 1)) + gran; a < hi; a += gran) {
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
        if (mbi.State == MEM_FREE) {
            void* p = VirtualAlloc((void*)a, size, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return NULL;
}

pk_cave pk_cave_open(size_t cap)
{
    pk_cave c = { NULL, 0, 0 };
    void* m = alloc_near(pk_base(), cap ? cap : 1);
    if (m) { c.base = (uint8_t*)m; c.cap = cap; }
    return c;
}

void pk_emit(pk_cave* c, const void* bytes, size_t n)
{
    if (!c->base || c->len + n > c->cap) {
        pk_logf("pk_emit: overflow (have %zu cap %zu need %zu)", c->len, c->cap, n);
        return;
    }
    memcpy(c->base + c->len, bytes, n);
    c->len += n;
}

void pk_fix_rel32(pk_cave* c, size_t at, const void* target)
{
    uint8_t* field = c->base + at;
    int32_t  rel   = (int32_t)((intptr_t)target - (intptr_t)(field + 4));
    memcpy(field, &rel, 4);
}

void pk_fix_rel32_rva(pk_cave* c, size_t at, uint32_t rva)
{
    pk_fix_rel32(c, at, pk_ptr(rva));
}

int pk_detour(pk_cave* c, uint32_t site_rva, size_t stolen)
{
    if (!c->base || stolen < 5 || stolen > 32) return PK_FAIL;

    /* lock the finished cave down to RX */
    DWORD old;
    VirtualProtect(c->base, c->len, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), c->base, c->len);

    uint8_t  patch[32];
    uint8_t* site = (uint8_t*)pk_ptr(site_rva);
    int32_t  rel  = (int32_t)((intptr_t)c->base - (intptr_t)(site + 5));
    patch[0] = 0xE9;
    memcpy(patch + 1, &rel, 4);
    for (size_t i = 5; i < stolen; i++) patch[i] = 0x90;   /* NOP pad */

    return pk_write(site_rva, patch, stolen);
}
