/* CHANGELOG
 * fixed a bug where the first-person weeball throwing animation would not play in demos
 * fixed a bug where the weeball cooldown would not show in demos
*/

/* weeball_throw_demo - restore the local player's weeball throw animation +
 * utility cooldown during CLIENT (POV) demo playback.
 *
 * Bug (see BUGREPORT_weeball_throw_demo.md): when you play back a *client* (POV)
 * demo, the recording player's own weeball throw has no first-person throw
 * animation and the utility cooldown bar never starts on the HUD.  The projectile
 * itself renders fine.  Live play is unaffected.
 *
 *   Root cause: the server never echoes a player its OWN animation/state events.
 *   Live, the throw animation + cooldown are produced by the input-driven local
 *   fire/throw path; on playback there is no input, so nothing drives them.  Held
 *   weapons survive because their fire is also driven by received packets; the
 *   thrown weeble's first-person viewmodel + utility cooldown have no driver.
 *
 * Trigger: the local player's own throw IS in the demo as a 0xDE entity_event
 * with event_key 0x99 (the weeble skill-cast), object_id == local player.  This
 * fires at THROW time -- confirmed via demo analysis: the throw cluster
 * (0x99/0x9a, arg = weeble id) lands ~0.25 s before the impact 0xDF (a separate
 * weapon_event with the damage-applied flag bit set).  An earlier version keyed
 * off that impact 0xDF, which made the animation + cooldown start on impact
 * instead of on the throw -- fixed by keying off the throw 0xDE here.
 *
 * We detour the entry of client_game_packet_handler and, for that 0xDE -- only
 * during a client replay, only for the local player, only with a weeble equipped
 * -- reproduce the two missing effects the live throw produces:
 *
 *   1) First-person animation.  The first-person view is a VIEWMODEL render
 *      object, not the player body model.  The weeble's own 1p viewmodel lives in
 *      a per-weapon map (FUN_14009b0b0) keyed by the equipped weeble id
 *      DAT_140adb440 (which equals the thrown projectile type).  We fetch it, make
 *      it visible (+0x6a1 = 1) and play the one-shot "fire" clip on it
 *      (FUN_14008b6f0) -- exactly what the weeble equip handler FUN_140094790 does
 *      to show/animate that object.  (The equipped-weapon viewmodel DAT_140e355f8
 *      is null at throw time, which is why the throw shows nothing otherwise.)
 *
 *   2) Utility cooldown.  The HUD builder calculate_item_cooldown (0x1401d42b0)
 *      reads a per-utility-slot record array at DAT_140e4df18+0x5b1d0/+0x5b1d8
 *      (stride 0x360) and displays the slot s = (*(int*)begin == -1) ? 1 : 0,
 *      computing the bar from g_physics_accumulated_time - record[s]+0x8 (a double
 *      activation anchor).  We stamp record[s]+0x8 = g_physics_accumulated_time
 *      ("now"), so the bar starts full and counts down.
 *
 * is_client_replay() (g_replay_active && g_replay_mode == 1) gates everything, so
 * live gameplay never enters this code.  The equipped item is confirmed a weeble
 * via FUN_1400a2ea0(DAT_140adb440)->+0x47 (same flag/handler the equip path uses).
 *
 * NOT fixed here: the flying weeble PROJECTILE.  Demo analysis shows it is purely
 * client-predicted from local input at throw time -- there is no networked
 * projectile entity, and a POV (client) demo never records the recorder's own
 * throw trajectory (the only packet carrying it, the impact 0xDF, arrives ~0.25 s
 * late, at the landing).  So the own flying ball cannot be reconstructed from the
 * recorded stream without re-simulating the throw; left as a separate problem.
 *
 * Provenance (Nov-2023 build, image base 0x140000000; all values are RVAs):
 *   client_game_packet_handler 0x22a390 (detoured at 0x22a40a, resume 0x22a410)
 *   0xDE entity_event layout: [0]=type, +3=object_id(u4), [7]=event_key, +8=arg(u4)
 *   throw event_key 0x99 (weeble skill-cast; player_movement_tick case -0x67)
 *   is_client_replay 0x0ba210  char(void)
 *   FUN_140199fb0 local_id     0x199fb0  int(void)
 *   FUN_1400a2ea0 ent_desc     0x0a2ea0  void*(id,0,0); +0x47 = weeble flag
 *   FUN_14009b0b0 vm_lookup    0x09b0b0  void**(unused, int* id) -> &slot (1p vm obj)
 *   FUN_14008b6f0 vm_play      0x08b6f0  void(obj, char 0, std::string* clip)
 *   std_string_init 0x080f40 ; std_string_clear 0x081b20
 *   DAT_140adb440 equipped weeble id 0xadb440 (int, -1 = none)
 *   render obj +0x6a1 = visible flag
 *   DAT_140e4df18 state entity 0xe4df18 ; g_physics_accumulated_time 0xbd1118
 *   utility records: entity+0x5b1d0(begin)/+0x5b1d8(end), stride 0x360, anchor @+0x8
 */
#include "patchkit.h"
#include <string.h>

#define SITE_RVA   0x22a40au   /* mov r15,rcx ; movzx eax,[rcx]  (6 bytes) */
#define RESUME_RVA 0x22a410u   /* add eax,0xffffff24 (next instruction)    */

#define RVA_IS_REPLAY 0x0ba210u
#define RVA_LOCAL_ID  0x199fb0u
#define RVA_WDESC     0x0a2ea0u
#define RVA_VMMAP     0x09b0b0u   /* FUN_14009b0b0(_, int* id) -> &slot */
#define RVA_VM_PLAY   0x08b6f0u   /* FUN_14008b6f0(obj, 0, std::string*) */
#define RVA_STRINIT   0x080f40u
#define RVA_STRCLR    0x081b20u
#define RVA_WEEBLE_ID 0x0adb440u  /* &DAT_140adb440 (equipped weeble id) */
#define RVA_CD_ENT    0x0e4df18u  /* &DAT_140e4df18 (state entity)       */
#define RVA_PHYS_TIME 0x0bd1118u  /* &g_physics_accumulated_time (double) */

#define OFF_VISIBLE   0x6a1       /* render obj -> visible flag (byte) */
#define OFF_REC_BEGIN 0x5b1d0
#define OFF_REC_END   0x5b1d8
#define REC_STRIDE    0x360
#define REC_ANCHOR    0x8

#define EK_OFF        7           /* 0xDE entity_event: event_key byte offset      */
#define OID_OFF       3           /* 0xDE entity_event: object_id (u4) offset      */
#define EK_WEEBLE_THROW 0x99      /* event_key for the local weeble-throw skill-cast */

typedef char   (*fn_is_replay)(void);
typedef int    (*fn_local_id)(void);
typedef void*  (*fn_wdesc)(int, int, int);
typedef void** (*fn_vmmap)(void*, int*);
typedef void   (*fn_vm_play)(void*, char, void*);
typedef void*  (*fn_strinit)(void*, const char*);
typedef void   (*fn_strclr)(void*);

static fn_is_replay p_is_replay;
static fn_local_id  p_local_id;
static fn_wdesc     p_wdesc;
static fn_vmmap     p_vmmap;
static fn_vm_play   p_vm_play;
static fn_strinit   p_strinit;
static fn_strclr    p_strclr;
static int*         p_weeble_id;      /* &DAT_140adb440 */
static void**       pp_cd_entity;     /* &DAT_140e4df18 */
static double*      p_phys_time;      /* &g_physics_accumulated_time */

/* Runs on the game's client/packet thread, once per received game packet.  Bails
 * immediately unless this is the local player's own weeble THROW event during a
 * client replay -- so the common path is a few cheap checks.
 *
 * The throw is the 0xDE entity_event with event_key 0x99 (the weeble skill-cast),
 * object_id == local player, fired at THROW time (confirmed via demo: the throw
 * cluster 0x99/0x9a is ~0.25 s before the impact 0xDF).  Triggering off the throw
 * 0xDE -- not the impact 0xDF -- makes the animation + cooldown start when the
 * weeble leaves the hand, not when it lands. */
static void weeball_fix(unsigned char* pkt, unsigned long long size)
{
    unsigned char *desc, *cdent;
    void **slot, *wobj;
    int wid;
    void *clip[4];   /* std::string SSO scratch (32 bytes, 8-aligned) */

    if (pkt[0] != 0xDE || size != 0x18)         return;   /* entity_event only        */
    if (pkt[EK_OFF] != EK_WEEBLE_THROW)         return;   /* weeble-throw skill-cast   */
    if (!p_is_replay())                         return;   /* client demo only          */
    if (*(int*)(pkt + OID_OFF) != p_local_id()) return;   /* local player's own throw  */

    wid = *p_weeble_id;                                    /* equipped weeble id        */
    if (wid == -1)                              return;   /* no weeble equipped        */
    desc = (unsigned char*)p_wdesc(wid, 0, 0);
    if (!desc || desc[0x47] == 0)               return;   /* equipped item not a weeble */

    /* 1) first-person throw animation: play "fire" on the weeble's own 1p viewmodel
     *    object (fetched by equipped weeble id), making it visible first. */
    slot = p_vmmap(0, &wid);
    if (slot && *slot) {
        wobj = *slot;
        *((unsigned char*)wobj + OFF_VISIBLE) = 1;
        p_strinit(clip, "fire");
        p_vm_play(wobj, 0, clip);
        p_strclr(clip);
    }

    /* 2) start the utility cooldown bar: stamp the HUD's displayed slot anchor. */
    cdent = (unsigned char*)*pp_cd_entity;
    if (cdent) {
        unsigned char* begin = *(unsigned char**)(cdent + OFF_REC_BEGIN);
        unsigned char* end   = *(unsigned char**)(cdent + OFF_REC_END);
        if (begin && end > begin) {
            long n = (long)((end - begin) / REC_STRIDE);
            int  s = (*(int*)begin == -1) ? 1 : 0;       /* same selection the HUD uses */
            if (s < n)
                *(double*)(begin + (long)s * REC_STRIDE + REC_ANCHOR) = *p_phys_time;
        }
    }
}

PK_PLUGIN(weeball_throw_demo)
{
    /* stolen prologue bytes: mov r15,rcx (4C 8B F9) ; movzx eax,byte[rcx] (0F B6 01) */
    static const uint8_t orig[6] = { 0x4C,0x8B,0xF9, 0x0F,0xB6,0x01 };

    if (pk_verify(SITE_RVA, "\xE9", 1)) { pk_logf("weeball_throw_demo: already detoured"); return PK_OK; }
    if (!pk_verify(SITE_RVA, orig, sizeof orig)) {
        pk_logf("weeball_throw_demo: site 0x%x mismatch; aborting", SITE_RVA);
        return PK_FAIL;
    }

    p_is_replay  = (fn_is_replay)pk_ptr(RVA_IS_REPLAY);
    p_local_id   = (fn_local_id) pk_ptr(RVA_LOCAL_ID);
    p_wdesc      = (fn_wdesc)    pk_ptr(RVA_WDESC);
    p_vmmap      = (fn_vmmap)    pk_ptr(RVA_VMMAP);
    p_vm_play    = (fn_vm_play)  pk_ptr(RVA_VM_PLAY);
    p_strinit    = (fn_strinit)  pk_ptr(RVA_STRINIT);
    p_strclr     = (fn_strclr)   pk_ptr(RVA_STRCLR);
    p_weeble_id  = (int*)        pk_ptr(RVA_WEEBLE_ID);
    pp_cd_entity = (void**)      pk_ptr(RVA_CD_ENT);
    p_phys_time  = (double*)     pk_ptr(RVA_PHYS_TIME);

    /* Trampoline: save volatiles, call weeball_fix(pkt=rcx, size=r8->rdx), restore,
     * re-exec the two stolen instructions, jump back.  RSP is 16-aligned at the
     * site; the helper address is loaded as an absolute imm64 (the DLL may be >2GB
     * from the cave, out of rel32 reach). */
    uint8_t code[] = {
        /* 0x00 */ 0x51,                            /* push rcx          ; save pkt        */
        /* 0x01 */ 0x52,                            /* push rdx                            */
        /* 0x02 */ 0x41,0x50,                       /* push r8           ; save size       */
        /* 0x04 */ 0x41,0x51,                       /* push r9                             */
        /* 0x06 */ 0x4C,0x89,0xC2,                  /* mov rdx,r8        ; arg2 = size      */
        /* 0x09 */ 0x48,0x83,0xEC,0x20,             /* sub rsp,0x20      ; shadow space     */
        /* 0x0D */ 0x48,0xB8,0,0,0,0,0,0,0,0,       /* mov rax,imm64     ; &weeball_fix @0x0F*/
        /* 0x17 */ 0xFF,0xD0,                       /* call rax          ; weeball_fix()    */
        /* 0x19 */ 0x48,0x83,0xC4,0x20,             /* add rsp,0x20                         */
        /* 0x1D */ 0x41,0x59,                       /* pop r9                              */
        /* 0x1F */ 0x41,0x58,                       /* pop r8                              */
        /* 0x21 */ 0x5A,                            /* pop rdx                             */
        /* 0x22 */ 0x59,                            /* pop rcx                             */
        /* 0x23 */ 0x4C,0x8B,0xF9,                  /* mov r15,rcx       ; re-exec stolen #1*/
        /* 0x26 */ 0x0F,0xB6,0x01,                  /* movzx eax,[rcx]   ; re-exec stolen #2*/
        /* 0x29 */ 0xE9,0,0,0,0,                    /* jmp RESUME        ; rel32 @0x2A      */
    };

    uintptr_t fn = (uintptr_t)&weeball_fix;
    memcpy(&code[0x0F], &fn, sizeof fn);

    pk_cave c = pk_cave_open(sizeof code);
    if (!c.base) { pk_logf("weeball_throw_demo: cave alloc failed"); return PK_FAIL; }
    pk_emit(&c, code, sizeof code);
    pk_fix_rel32_rva(&c, 0x2A, RESUME_RVA);

    if (!pk_detour(&c, SITE_RVA, sizeof orig)) return PK_FAIL;
    pk_logf("weeball_throw_demo: installed (weeble throw anim + utility cooldown, on throw, during demo playback)");
    return PK_OK;
}
