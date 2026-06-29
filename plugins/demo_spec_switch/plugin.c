/* CHANGELOG
 * new feature: allow cycling through other players pov in client recorded demos
*/

/* demo_spec_switch - SPACEBAR spectate-next for CLIENT-side demos.
 *
 * Spectate POV switching in demos is server-authoritative: a server demo
 * (g_replay_mode==2) runs a local server simulation that answers the client's
 * "next spectate target" request and moves g_spectator_target_index. A client
 * demo (g_replay_mode==1) only replays recorded client snapshots - no server -
 * so the request goes unanswered and spacebar does nothing. (Verified live.)
 *
 * No byte-patch can synthesize the missing server logic, so we do the switch
 * LOCALLY. We detour the per-frame playback tick (replay_playback_tick @0x2b1820,
 * runs every frame during ANY demo) into a cave that calls a C callback on the
 * MAIN THREAD. While g_replay_mode==1 the callback implements a spacebar cycle:
 *
 *   recorded-view  ->  other player A  ->  other player B  ->  ...  ->  recorded-view
 *
 * - "recorded-view" = g_spectator_target_index == -1, applied via
 *   FUN_140199a80(0xffffffff, 1). This is the recording player's REAL first-person
 *   view (position AND mouse-driven angles come from the recorded camera stream,
 *   which the engine only plays at spec_target==-1). The recording player's own
 *   world entity is SKIPPED in the cycle: spectating it directly gives a follow-cam
 *   with frozen view angles and the spectator HUD (his angles live in the camera
 *   stream, not his world entity). He is reachable only via the -1 recorded view.
 * - other players = FUN_140199a80(id, 1) - the camera-switch routine the server
 *   path ends in (param2=1 suppresses the now-dead network send).
 * - STICKY: while watching a chosen OTHER player, each frame re-assert the choice
 *   if the recorded stream yanks the POV away (recorded client respawn, or dead
 *   recorded client spectating someone). On the recorded-view stop, sticky is
 *   released so the recording drives the camera normally. Releases if the chosen
 *   player disappears.
 *
 * Identifying the recorder: he has no stable id/pointer link to his world entity
 * (g_player_entity_ptr is even null at this hook point). The robust link is
 * POSITION - while spec_target == -1 the render camera (DAT_140e537e0 + 0x150)
 * sits on him, so the spectate-map entity nearest the camera is his. We compute
 * this only while at -1 and cache it.
 *
 * Spectate map DAT_140f2cf00 (layout read live from FUN_140199a80 @0x199ade):
 * head = *(0x140f2cf00); root = head[+0x08]; node: +0x19 is_nil(char),
 * +0x20 key(uint id), +0x00 left, +0x10 right, +0x28 entity ptr. Entity world
 * position cache is at entity+0xda0 (xy) / +0xdb0 (z), doubles (from the spectate
 * path of camera_and_entity_update @0x1404d9f1c).
 *
 * Server demos (mode 2) untouched. Spacebar read globally via GetAsyncKeyState. */
#include "patchkit.h"
#include <windows.h>

#define RVA_REPLAY_MODE  0xe358c0u   /* g_replay_mode (1=client demo)            */
#define RVA_SPEC_TARGET  0xbd10f8u   /* g_spectator_target_index (i32, -1=own)   */
#define RVA_TREE         0xf2cf00u   /* DAT_140f2cf00: ptr to map head node      */
#define RVA_SET_POV      0x199a80u   /* FUN_140199a80(uint id, char p2)          */
#define RVA_CAMERA       0xe537e0u   /* DAT_140e537e0: render camera obj ptr     */

#define SITE_RVA   0x2b1820u         /* replay_playback_tick entry               */
#define RESUME_RVA 0x2b1827u         /* SITE_RVA + 7 (after stolen bytes)        */
#define NONE       0xffffffffu

typedef void  (*set_pov_fn)(unsigned, char);
typedef SHORT (WINAPI *gaks_fn)(int);

/* In-order successor: node with the smallest key strictly greater than `cur`
 * (or, if have_cur is 0, the minimum node). NULL if the tree is empty. */
static long long tree_succ(uintptr_t base, unsigned cur, int have_cur)
{
    long long head = *(volatile long long *)(base + RVA_TREE);
    if (!head) return 0;
    long long n = *(long long *)(head + 0x08);
    long long best = 0;
    int guard = 0;
    while (*(char *)(n + 0x19) == 0) {
        unsigned k = *(unsigned *)(n + 0x20);
        if (!have_cur || k > cur) { best = n; n = *(long long *)(n + 0x00); }
        else                      {            n = *(long long *)(n + 0x10); }
        if (++guard > 8192) return 0;
    }
    return best;
}

/* Exact lookup: node with key == id, or NULL. */
static long long tree_find(uintptr_t base, unsigned id)
{
    long long head = *(volatile long long *)(base + RVA_TREE);
    if (!head) return 0;
    long long n = *(long long *)(head + 0x08);
    long long best = head;
    int guard = 0;
    while (*(char *)(n + 0x19) == 0) {
        if (*(unsigned *)(n + 0x20) >= id) { best = n; n = *(long long *)(n + 0x00); }
        else                               {           n = *(long long *)(n + 0x10); }
        if (++guard > 8192) return 0;
    }
    if (best == head || *(char *)(best + 0x19) != 0) return 0;
    if (*(unsigned *)(best + 0x20) != id) return 0;
    return best;
}

/* The recording player exists twice in a client demo: his live recorded view
 * (spec_target == -1, correct angles + own HUD) and a separate world entity in
 * the spectate map (frozen angles + spectator HUD - his look lives in the camera
 * stream, not his world entity). They share neither id nor pointer, and
 * g_player_entity_ptr is null at this hook point (set later in the frame). The
 * link that IS valid here is POSITION: while spec_target == -1 the render camera
 * (DAT_140e537e0 + 0x150) sits on the recorder, so the world entity nearest the
 * camera is his. We compute this only while at -1 and cache it (see spec_cb). */
static unsigned nearest_world_id(uintptr_t base, double ox, double oy, double oz)
{
    unsigned best_id = NONE; double best_d = 1e30;
    unsigned probe = 0; int have = 0;
    for (int i = 0; i < 256; i++) {
        long long node = tree_succ(base, probe, have);
        if (!node) break;
        unsigned  k   = *(unsigned  *)(node + 0x20);
        long long ent = *(long long *)(node + 0x28);
        if (ent) {
            double dx = *(double *)(ent + 0xda0) - ox;  /* entity pos cache (doubles) */
            double dy = *(double *)(ent + 0xda8) - oy;
            double dz = *(double *)(ent + 0xdb0) - oz;
            double d  = dx*dx + dy*dy + dz*dz;
            if (d < best_d) { best_d = d; best_id = k; }
        }
        probe = k; have = 1;
    }
    return best_id;
}

/* Next spectatable player id after `cur` that is real (entity present) and not the
 * recording player (`skip`). Returns NONE when there is no next -> caller wraps to
 * the recorded-view stop (spec_target == -1), which the engine renders with the
 * recording player's live view angles. */
static unsigned next_player(uintptr_t base, unsigned cur, int have_cur, unsigned skip)
{
    unsigned probe = cur; int have = have_cur;
    for (int i = 0; i < 256; i++) {
        long long node = tree_succ(base, probe, have);
        if (!node) return NONE;
        unsigned  k   = *(unsigned  *)(node + 0x20);
        long long ent = *(long long *)(node + 0x28);
        if (ent && k != skip) return k;
        probe = k; have = 1;
    }
    return NONE;
}

static int      g_prev_space  = 0;
static int      g_user_active = 0;   /* watching a user-chosen OTHER player */
static unsigned g_user_target = 0;
static unsigned g_recorder_id = NONE; /* recorder's world-entity id (cached at -1) */

/* Runs every frame on the main thread (via the cave). */
static void spec_cb(void)
{
    uintptr_t base = pk_base();
    if (*(volatile unsigned *)(base + RVA_REPLAY_MODE) != 1) {  /* client demos only */
        g_prev_space = 0; g_user_active = 0; return;
    }

    set_pov_fn set_pov = (set_pov_fn)(base + RVA_SET_POV);

    /* Cache the recorder's world-entity id while showing his own view: the camera
     * sits on him, so the nearest world entity to the camera is his. */
    if (*(volatile unsigned *)(base + RVA_SPEC_TARGET) == NONE) {
        long long cam = *(volatile long long *)(base + RVA_CAMERA);
        if (cam) {
            unsigned r = nearest_world_id(base, *(double *)(cam + 0x150),
                                                *(double *)(cam + 0x158),
                                                *(double *)(cam + 0x160));
            if (r != NONE) g_recorder_id = r;
        }
    }

    /* (1) spacebar press -> advance the cycle */
    static gaks_fn pGAKS = 0;
    if (!pGAKS) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) pGAKS = (gaks_fn)GetProcAddress(u, "GetAsyncKeyState");
    }
    if (pGAKS) {
        int down = (pGAKS(VK_SPACE) & 0x8000) ? 1 : 0;
        if (down && !g_prev_space) {
            unsigned skip = g_recorder_id;             /* recorder's frozen world entity */
            unsigned cur  = *(volatile unsigned *)(base + RVA_SPEC_TARGET);
            unsigned next = (cur == NONE) ? next_player(base, 0, 0, skip)
                                          : next_player(base, cur, 1, skip);
            if (next == NONE) {                 /* wrap to recorded first-person view */
                set_pov(NONE, 1);
                g_user_active = 0;              /* let the recording drive */
            } else {
                set_pov(next, 1);
                g_user_target = next;
                g_user_active = 1;
            }
        }
        g_prev_space = down;
    }

    /* (2) sticky: hold a chosen OTHER player against recorded-POV resets */
    if (g_user_active) {
        long long node = tree_find(base, g_user_target);
        if (!node || !*(long long *)(node + 0x28)) {
            set_pov(NONE, 1);                   /* chosen player gone -> recorded view */
            g_user_active = 0;
        } else if (*(volatile unsigned *)(base + RVA_SPEC_TARGET) != g_user_target) {
            set_pov(g_user_target, 1);          /* recording reset it -> re-assert */
        }
    }
}

PK_PLUGIN(demo_spec_switch)
{
    static const uint8_t orig[7] = { 0x48,0x8b,0xc4,0x48,0x89,0x58,0x08 }; /* MOV RAX,RSP; MOV [RAX+8],RBX */
    if (pk_verify(SITE_RVA, "\xE9", 1)) { pk_logf("demo_spec_switch: already detoured"); return PK_OK; }
    if (!pk_verify(SITE_RVA, orig, sizeof orig)) {
        pk_logf("demo_spec_switch: site 0x%x mismatch; aborting", SITE_RVA);
        return PK_FAIL;
    }

    uint8_t code[] = {
        0x50,                               /* 0x00 push rax              */
        0x51,                               /* 0x01 push rcx              */
        0x52,                               /* 0x02 push rdx              */
        0x41,0x50,                          /* 0x03 push r8               */
        0x41,0x51,                          /* 0x05 push r9               */
        0x41,0x52,                          /* 0x07 push r10              */
        0x41,0x53,                          /* 0x09 push r11              */
        0x48,0x83,0xEC,0x20,                /* 0x0B sub rsp,0x20 (shadow) */
        0x48,0xB8,0,0,0,0,0,0,0,0,          /* 0x0F mov rax,imm64 (@0x11) */
        0xFF,0xD0,                          /* 0x19 call rax              */
        0x48,0x83,0xC4,0x20,                /* 0x1B add rsp,0x20          */
        0x41,0x5B,                          /* 0x1F pop r11               */
        0x41,0x5A,                          /* 0x21 pop r10               */
        0x41,0x59,                          /* 0x23 pop r9                */
        0x41,0x58,                          /* 0x25 pop r8                */
        0x5A,                               /* 0x27 pop rdx               */
        0x59,                               /* 0x28 pop rcx               */
        0x58,                               /* 0x29 pop rax (RSP restored)*/
        0x48,0x8B,0xC4,0x48,0x89,0x58,0x08, /* 0x2A stolen prologue       */
        0xE9,0,0,0,0,                       /* 0x31 jmp resume (rel@0x32) */
    };
    uintptr_t cb = (uintptr_t)spec_cb;
    for (int i = 0; i < 8; i++) code[0x11 + i] = (uint8_t)(cb >> (i * 8));

    pk_cave c = pk_cave_open(sizeof code);
    if (!c.base) { pk_logf("demo_spec_switch: cave alloc failed"); return PK_FAIL; }
    pk_emit(&c, code, sizeof code);
    pk_fix_rel32_rva(&c, 0x32, RESUME_RVA);
    return pk_detour(&c, SITE_RVA, sizeof orig);
}
