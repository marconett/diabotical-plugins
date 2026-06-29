/* console_banner.c - prints a "patches loaded: ..." line into the in-game
 * console at startup.
 *
 * We piggyback on the engine's version-print function FUN_1404a1750 (RVA
 * 0x4a1750), which builds the "^00ff00Product version: ..." line and calls
 * console_print().  That function runs exactly ONCE during startup, on the main
 * thread, and is itself defined by a console_print() call -- so by the time it
 * executes the console subsystem is provably live and on the right thread.  No
 * polling, no readiness heuristics, no thread/ordering races.
 *
 * Mechanism: detour its 5-byte entry instruction (MOV [RSP+8],RBX) into a cave
 * that calls our C emitter, re-executes the stolen instruction, and resumes.
 * Our line therefore prints immediately above the engine version line.
 *
 *   console_print(std::string*) (RVA 0x3a1bc0) only READS the string -- it
 *   copies the text into the log + the visible scrollback ring (FUN_1401b6f40)
 *   and never frees it -- so we can hand it a static buffer with a hand-built
 *   MSVC std::string header. */
#include "patchkit.h"
#include <windows.h>
#include <string.h>

#define RVA_VERSION_PRINT 0x4a1750u   /* FUN_1404a1750: prints "Product version:" */
#define RVA_CONSOLE_PRINT 0x3a1bc0u   /* console_print(std::string*)               */

typedef void (*console_print_fn)(void*);

/* MSVC std::string (x64), 32 bytes: union{char buf[16]; char* ptr;}; size_t size;
 * size_t cap.  cap > 15 selects heap mode, so the callee reads *obj as the char*.
 * Read-only on the callee side -> a static buffer with a fake heap header is safe. */
typedef struct { void* ptr; uint64_t _pad; uint64_t size; uint64_t cap; } pk_mstr;

static char g_banner[480];

void pk_banner_set(const char* text)
{
    size_t n = strlen(text);
    if (n >= sizeof g_banner) n = sizeof g_banner - 1;
    memcpy(g_banner, text, n);
    g_banner[n] = '\0';
}

/* Cave callee, runs on the main thread from inside the version printer.  The
 * version printer runs once, but guard anyway so a re-entry can't double-print. */
static void pk_banner_emit(void)
{
    static int done = 0;
    if (done || g_banner[0] == '\0') return;
    done = 1;

    pk_mstr s;
    s.ptr  = g_banner;
    s._pad = 0;
    s.size = strlen(g_banner);
    s.cap  = sizeof g_banner - 1;          /* > 15 -> heap mode, ptr read from s.ptr */

    ((console_print_fn)pk_ptr(RVA_CONSOLE_PRINT))(&s);
    pk_logf("console_banner: printed \"%s\"", g_banner);
}

int pk_banner_install(void)
{
    /* steal the 5-byte entry instruction: MOV [RSP+8],RBX = 48 89 5C 24 08 */
    static const uint8_t orig[5] = { 0x48,0x89,0x5C,0x24,0x08 };
    if (pk_verify(RVA_VERSION_PRINT, "\xE9", 1)) {
        pk_logf("console_banner: already detoured");
        return PK_OK;
    }
    if (!pk_verify(RVA_VERSION_PRINT, orig, sizeof orig)) {
        pk_logf("console_banner: site 0x%x mismatch; aborting", RVA_VERSION_PRINT);
        return PK_FAIL;
    }

    /* cave (RSP%16==8 on entry; sub 0x28 -> 16-aligned + shadow space):
     *   0x00 sub  rsp,0x28              48 83 EC 28
     *   0x04 mov  rax,<&pk_banner_emit> 48 B8 <imm64>      imm @ 0x06
     *   0x0E call rax                   FF D0
     *   0x10 add  rsp,0x28              48 83 C4 28
     *   0x14 mov  [rsp+8],rbx           48 89 5C 24 08      ; re-exec stolen instr
     *   0x19 jmp  <site+5>              E9 <rel32>          rel32 @ 0x1A
     * RBX is nonvolatile, so pk_banner_emit() preserves it across the call; the
     * re-executed MOV then stores the original RBX into the caller's home slot. */
    static const uint8_t code[] = {
        0x48,0x83,0xEC,0x28,                                /* sub  rsp,0x28        */
        0x48,0xB8,0,0,0,0,0,0,0,0,                          /* mov  rax,imm64       */
        0xFF,0xD0,                                          /* call rax             */
        0x48,0x83,0xC4,0x28,                                /* add  rsp,0x28        */
        0x48,0x89,0x5C,0x24,0x08,                           /* mov  [rsp+8],rbx     */
        0xE9,0,0,0,0,                                       /* jmp  site+5          */
    };

    pk_cave c = pk_cave_open(sizeof code);
    if (!c.base) { pk_logf("console_banner: cave alloc failed"); return PK_FAIL; }
    pk_emit(&c, code, sizeof code);

    uint64_t fn = (uint64_t)(uintptr_t)&pk_banner_emit;
    memcpy(c.base + 0x06, &fn, 8);                          /* mov rax,imm64 operand */
    pk_fix_rel32_rva(&c, 0x1A, RVA_VERSION_PRINT + 5);     /* jmp back to entry+5    */

    return pk_detour(&c, RVA_VERSION_PRINT, sizeof orig);
}
