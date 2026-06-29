/* patchkit.h - tiny runtime-patching SDK for diabotical.exe
 *
 * The DLL is loaded into the game by masquerading as one of its imported DLLs
 * (see src/proxy_*.c).  After the loader resolves the game image base it runs
 * every selected plugin's init function.  A plugin uses the helpers below to
 * apply the SAME edits the standalone .py binary-patchers apply -- flat byte
 * overwrites and code-cave detours -- except at runtime, so the on-disk
 * diabotical.exe is never modified.
 *
 * All addresses are passed as IMAGE-RELATIVE RVAs (VA - 0x140000000).  The
 * helpers translate to the real, ASLR-correct runtime address via pk_base().
 * This is strictly more robust than the .py patchers, which hardcode absolute
 * VAs and only work when the image loads at its preferred base.
 */
#ifndef PATCHKIT_H
#define PATCHKIT_H

#include <stdint.h>
#include <stddef.h>

#define PK_OK   1
#define PK_FAIL 0

#ifdef __cplusplus
extern "C" {
#endif

/* --- module / addressing ------------------------------------------------- */
uintptr_t pk_base(void);          /* base of diabotical.exe (GetModuleHandle(NULL)) */
void*     pk_ptr(uint32_t rva);   /* pk_base() + rva                                */

/* --- raw memory patching ------------------------------------------------- */
int  pk_verify(uint32_t rva, const void* expect, size_t n); /* 1 if bytes match    */
int  pk_write (uint32_t rva, const void* bytes,  size_t n); /* VirtualProtect+copy  */

/* Flat-patch convenience: the verify / already-applied / write / log idiom every
 * in-place patcher under patches/ shares.  Checks `expect_n` bytes at rva; if
 * they already equal `repl` (first repl_n bytes) it's a no-op success, if they
 * equal `expect` it writes `repl_n` bytes of `repl`, otherwise it logs a mismatch
 * and fails.  `expect` is a verification signature and may be shorter than `repl`
 * (e.g. 16-byte function prologue sig vs a longer replacement body). */
int  pk_apply(uint32_t rva, const void* expect, size_t expect_n,
              const void* repl, size_t repl_n, const char* label);

/* --- code-cave builder ---------------------------------------------------
 * A "cave" is a freshly allocated executable buffer placed within +-2GB of the
 * game image so a 5-byte E9 jump can reach it.  Build it by emitting the same
 * machine code the .py cave emits, fix up its rel32 fields to runtime targets,
 * then install the detour.
 */
typedef struct {
    uint8_t* base;   /* start of the allocated cave (NULL on failure) */
    size_t   len;    /* bytes emitted so far                          */
    size_t   cap;    /* capacity                                      */
} pk_cave;

pk_cave pk_cave_open(size_t cap);                       /* alloc RX-capable mem near image */
void    pk_emit(pk_cave* c, const void* bytes, size_t n);

/* Patch a little-endian rel32 displacement field at cave offset `at` so that,
 * at runtime, the instruction it belongs to references `target`/`rva`.
 * Used for E8 call / E9 jmp / FF15 call-[rip+disp] operands alike: the value
 * written is  target - (field_addr + 4). */
void    pk_fix_rel32    (pk_cave* c, size_t at, const void* target);
void    pk_fix_rel32_rva(pk_cave* c, size_t at, uint32_t rva);

/* Overwrite `stolen` bytes at site_rva with `E9 rel32` -> cave start, NOP-pad
 * the remainder.  Marks the cave executable.  Returns PK_OK/PK_FAIL. */
int     pk_detour(pk_cave* c, uint32_t site_rva, size_t stolen);

/* --- logging -------------------------------------------------------------
 * Appends to patchkit.log next to the loaded DLL and mirrors to the debugger
 * (OutputDebugString). */
void pk_logf(const char* fmt, ...);
void pk_set_self(void* hmodule);  /* called by the loader to locate the log */

/* --- plugin registration -------------------------------------------------
 * Each plugin defines exactly one:   PK_PLUGIN(my_name) { ...; return PK_OK; }
 * build.sh generates pk_registry.c listing the selected plugins. */
typedef int (*pk_init_fn)(void);
typedef struct { const char* name; pk_init_fn init; } pk_plugin;

extern const pk_plugin pk_plugins[];
extern const int       pk_plugin_count;

#define PK_PLUGIN(NAME) int pk_init_##NAME(void)

#ifdef __cplusplus
}
#endif
#endif /* PATCHKIT_H */
