/* CHANGELOG
 * fixed a bug where client side demos always ended prematurely, preventing the end of the game to be recorded
*/

/* demo_gzip_tail - runtime port of patches/demo_gzip_tail_patch.py
 *
 * Fixes truncated client demo (.rbr) files: replay_recording_finalize gzips the
 * recording but never writes the Z_FINISH output (gzip CRC32+ISIZE footer and
 * the last buffered window), so the stream is unterminated and the final moments
 * of the match are lost.  We detour the one instruction at RVA 0x1a751c
 * (LEA RCX,[RBP+0x1b0], 7 bytes) into a cave that writes the finish-output
 * std::string to the ofstream, re-executes the clobbered LEA, and resumes at the
 * existing teardown (RVA 0x1a7523).  See demo_truncation_patch_report.md. */
#include "patchkit.h"

#define SITE_RVA   0x1a751cu     /* LEA RCX,[RBP+0x1b0]  (7 bytes) */
#define RESUME_RVA 0x1a7523u     /* teardown (std::string clear)   */
#define WRITE_IAT  0x88f7a8u     /* IAT slot: basic_ostream::write  */

PK_PLUGIN(demo_gzip_tail)
{
    static const uint8_t orig[7] = { 0x48,0x8D,0x8D,0xB0,0x01,0x00,0x00 };
    if (pk_verify(SITE_RVA, "\xE9", 1)) { pk_logf("gzfix: already detoured"); return PK_OK; }
    if (!pk_verify(SITE_RVA, orig, sizeof orig)) {
        pk_logf("gzfix: site 0x%x mismatch; aborting", SITE_RVA);
        return PK_FAIL;
    }

    /* cave: write finish-output std::string [rbp+0x1b0] to ofstream [rsp+0x40] */
    static const uint8_t code[] = {
        0x48,0x8D,0x95,0xB0,0x01,0x00,0x00,            /* 0x00 lea    rdx,[rbp+0x1b0]  ; base   */
        0x48,0x83,0xBD,0xC8,0x01,0x00,0x00,0x10,       /* 0x07 cmp    [rbp+0x1c8],0x10 ; cap    */
        0x48,0x0F,0x43,0x95,0xB0,0x01,0x00,0x00,       /* 0x0F cmovnc rdx,[rbp+0x1b0]   ; dataptr*/
        0x4C,0x8B,0x85,0xC0,0x01,0x00,0x00,            /* 0x17 mov    r8,[rbp+0x1c0]   ; size   */
        0x48,0x8D,0x4C,0x24,0x40,                      /* 0x1E lea    rcx,[rsp+0x40]   ; ofstream*/
        0xFF,0x15,0x00,0x00,0x00,0x00,                 /* 0x23 call   [rip+WRITE_IAT]  rel@0x25 */
        0x48,0x8D,0x8D,0xB0,0x01,0x00,0x00,            /* 0x29 lea    rcx,[rbp+0x1b0]  ; re-exec*/
        0xE9,0x00,0x00,0x00,0x00,                      /* 0x30 jmp    RESUME           rel@0x31 */
    };

    pk_cave c = pk_cave_open(sizeof code);
    if (!c.base) { pk_logf("gzfix: cave alloc failed"); return PK_FAIL; }
    pk_emit(&c, code, sizeof code);
    pk_fix_rel32_rva(&c, 0x25, WRITE_IAT);   /* call [rip+disp] -> IAT slot          */
    pk_fix_rel32_rva(&c, 0x31, RESUME_RVA);  /* jmp back to teardown                 */

    return pk_detour(&c, SITE_RVA, sizeof orig);
}
