/* CHANGELOG
 * fixed a bug that could lead to the client lagging for a short time when client demo recording is enabled
*/

/* replay_reserve - eliminate client replay-recording reallocation hitches
 *
 * Bug (see replay system analysis):
 *   When client replay recording is enabled (cvar `replays_client_recording`),
 *   every received network packet, fire event, camera frame, kill-feed entry,
 *   etc. is appended to a single in-memory std::ostringstream
 *   (g_replay_recording_stream, RVA 0xe35898).  Its backing buffer grows by
 *   geometric (~2x) reallocation on the main game thread; once it is hundreds
 *   of MB a single realloc memcpy stalls the frame.  Never reserve()d, capped,
 *   or streamed to disk -- only compressed+written once at match end.
 *
 * Fix (capacity bump in the growth path):
 *   All growth funnels through basic_stringbuf::overflow (FUN_140083690).  It
 *   computes a new geometric capacity in R14 (max(0x20, 2*size)), then calls
 *   std_string_allocate(R14) (0x1400830d0), memcpy's the old buffer in, and
 *   frees the old one.  We detour the instruction right before that call
 *   (RVA 0x83742, "MOV RCX,R14; MOV [RSP+0x40],R15", 8 bytes) and, *only for
 *   the replay stream*, force R14 up to RESERVE_BYTES on the first grow.  The
 *   stream then jumps straight to a big buffer and overflow is not called again
 *   until RESERVE_BYTES of demo is recorded -- so the per-frame reallocation
 *   hitch is gone.  Every other stringstream in the game hits the `this !=
 *   replay stream` early-out and is untouched.
 *
 *   This deliberately writes NO stringbuf fields.  The earlier attempt assumed
 *   an inline std::string at stream+0x50 and corrupted the streambuf's indirect
 *   pointer/count slots (it crashed at 0x836ce: MOV RCX,[this+0x58]; deref
 *   NULL).  This build's basic_streambuf stores its get/put areas *indirectly*
 *   (this+0x18/0x20/0x38/0x40 are pointers-to-slots, +0x50/+0x58 pointers-to-
 *   counts), so the only safe lever is the capacity the engine itself passes to
 *   the allocator -- i.e. R14 here.
 *
 *   Cost: one ~RESERVE_BYTES commit per recording session (demand-zero paged).
 *   Nothing happens when recording is disabled (no replay stream is created).
 */
#include "patchkit.h"
#include <string.h>

#define SITE_RVA   0x83742u    /* mov rcx,r14 ; mov [rsp+0x40],r15  (8 bytes)   */
#define RESUME_RVA 0x8374au    /* call std_string_allocate                      */
#define GRECSTREAM 0xe35898u   /* qword g_replay_recording_stream (object base) */

/* Bytes to reserve for the in-memory demo buffer.  MUST be < 0x80000000: the
 * value is a sign-extended imm32 here AND overflow tracks the put count as a
 * 32-bit int.  256 MiB covers a long match; if a match exceeds it the stream
 * just takes one more (geometric) realloc at the limit -- a single late hitch,
 * never a crash. */
#define RESERVE_BYTES 0x10000000u   /* 256 MiB */

PK_PLUGIN(replay_reserve)
{
    static const uint8_t orig[8] = { 0x49,0x8B,0xCE, 0x4C,0x89,0x7C,0x24,0x40 };
    if (pk_verify(SITE_RVA, "\xE9", 1)) { pk_logf("replay_reserve: already detoured"); return PK_OK; }
    if (!pk_verify(SITE_RVA, orig, sizeof orig)) {
        pk_logf("replay_reserve: site 0x%x mismatch; aborting", SITE_RVA);
        return PK_FAIL;
    }

    /* cave: only the replay stream's stringbuf gets its grow capacity forced up.
     * On entry RBX = this (stringbuf = stream+8); R14 = computed new capacity;
     * RAX is dead (about to be set by the std_string_allocate call).  No engine
     * calls, no stack use -> no alignment concerns. */
    uint8_t code[] = {
        /* 0x00 */ 0x48,0x8B,0x05,0x00,0x00,0x00,0x00,  /* mov rax,[rip+d] ; rax=*g_rec_stream (d@0x03) */
        /* 0x07 */ 0x48,0x83,0xC0,0x08,                 /* add rax,8       ; rax = stringbuf base       */
        /* 0x0B */ 0x48,0x39,0xC3,                      /* cmp rbx,rax     ; this == replay stringbuf?  */
        /* 0x0E */ 0x75,0x0F,                           /* jne 0x1F        ; other stream -> normal     */
        /* 0x10 */ 0x48,0xC7,0xC0,0x00,0x00,0x00,0x00,  /* mov rax,imm32   ; RESERVE_BYTES (imm@0x13)    */
        /* 0x17 */ 0x49,0x39,0xC6,                      /* cmp r14,rax                                   */
        /* 0x1A */ 0x73,0x03,                           /* jae 0x1F        ; already big enough          */
        /* 0x1C */ 0x49,0x89,0xC6,                      /* mov r14,rax     ; force capacity = reserve    */
        /* 0x1F */ 0x49,0x8B,0xCE,                      /* mov rcx,r14     ; re-exec stolen #1           */
        /* 0x22 */ 0x4C,0x89,0x7C,0x24,0x40,            /* mov [rsp+0x40],r15 ; re-exec stolen #2        */
        /* 0x27 */ 0xE9,0x00,0x00,0x00,0x00,            /* jmp RESUME (rel@0x28)                         */
    };

    uint32_t reserve = RESERVE_BYTES;
    memcpy(&code[0x13], &reserve, 4);

    pk_cave c = pk_cave_open(sizeof code);
    if (!c.base) { pk_logf("replay_reserve: cave alloc failed"); return PK_FAIL; }
    pk_emit(&c, code, sizeof code);
    pk_fix_rel32_rva(&c, 0x03, GRECSTREAM);   /* mov rax,[rip+d] -> g_replay_recording_stream */
    pk_fix_rel32_rva(&c, 0x28, RESUME_RVA);   /* jmp back to the std_string_allocate call     */

    if (!pk_detour(&c, SITE_RVA, sizeof orig)) return PK_FAIL;
    pk_logf("replay_reserve: demo buffer grows straight to %u MiB (no realloc hitch)",
            RESERVE_BYTES >> 20);
    return PK_OK;
}
