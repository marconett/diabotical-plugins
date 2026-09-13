/* CHANGELOG
 * fixed a bug where the game froze for several seconds at the end of a match when client demo recording was enabled
*/

/* replay_async_finalize - moves client demo (.rbr) finalization off the main thread.
 *
 * replay_recording_finalize (RVA 0x1a6d30) gzips the whole in-memory recording
 * synchronously on the main thread when a match ends, which stalls the game for
 * 1-4 s on long matches.  This plugin replaces the function entirely: the main
 * thread only snapshots the header fields and copies the recording buffer, then a
 * worker thread writes the header, gzips the body through zlib1.dll and closes
 * the file.  The worker terminates the gzip stream with Z_FINISH, so the demo
 * tail is complete.
 *
 * The original function reads its inputs from globals that leave_game tears down
 * a few steps later (replay_cleanup clears the path, rewrites the map string and
 * deletes the stream), so everything the worker needs is copied up front.
 *
 * Shutdown: end_frame calls finalize and then TerminateProcess, which would kill a
 * worker mid-write.  That call site is recognised (return address / shutdown
 * flag) and handled synchronously, after joining any still-running workers. */
#include "patchkit.h"
#include <windows.h>
#include <stdint.h>
#include <string.h>

#define FINALIZE_RVA            0x1a6d30u   /* replay_recording_finalize entry            */
#define STRINGBUF_STR_RVA       0x0840b0u   /* basic_stringbuf::str(out) -> copy of body  */
#define STD_STRING_CLEAR_RVA    0x081b20u   /* frees a std::string's heap buffer          */
#define STD_STRING_ASSIGN_RVA   0x081ba0u   /* std::string assign(buf, len)               */

#define G_REC_PATH_RVA          0xc28318u   /* std::string output path; size != 0 == recording */
#define G_REC_STREAM_RVA        0xe35898u   /* ostringstream*; stringbuf at +8            */
#define G_APP_VERSION_RVA       0xa0b538u   /* "0.20.471s", 9 bytes                       */
#define G_HDR_STR1_RVA          0xc90778u   /* std::string, 2nd header string             */
#define G_HDR_STR2_RVA          0xf2cc50u   /* std::string, 3rd header string             */
#define G_DURATION_RVA          0xf2d2e0u   /* 8 raw bytes                                */
#define G_EXTRA_DURATION_RVA    0xe358a0u   /* 8 raw bytes (double)                       */
#define G_PACKETS_BEGIN_RVA     0xf05970u   /* vector<{std::string; u64}> begin (0x28/el) */
#define G_PACKETS_END_RVA       0xf05978u
#define G_SHUTDOWN_FLAG_RVA     0xe4886fu   /* end_frame: finalize + TerminateProcess     */
#define SHUTDOWN_CALLSITE_RVA   0x4dd478u   /* the finalize call inside end_frame         */

#define PACKET_ELEM_SIZE        0x28u
#define MAX_WORKERS             8
#define OUT_CHUNK               (256u * 1024u)
#define IN_CHUNK                (1u << 20)

/* MSVC std::string (SSO): [buf/ptr 16][size 8][cap 8]; cap > 0xf means heap. */
typedef struct { union { char buf[16]; char* ptr; } u; uint64_t size; uint64_t cap; } stdstr;
static const char* ss_data(const stdstr* s) { return s->cap > 0xf ? s->u.ptr : s->u.buf; }

typedef stdstr* (*stringbuf_str_fn)(void* stringbuf, stdstr* out);
typedef void    (*std_string_clear_fn)(stdstr* s);
typedef stdstr* (*std_string_assign_fn)(stdstr* s, const char* buf, size_t len);

/* --- zlib1.dll ----------------------------------------------------------- */
/* Win64 z_stream: uLong is 4 bytes, so the struct is 88 bytes. */
typedef struct {
    const uint8_t* next_in;  uint32_t avail_in;  uint32_t total_in;
    uint8_t*       next_out; uint32_t avail_out; uint32_t total_out;
    const char*    msg;      void*    state;
    void* zalloc; void* zfree; void* opaque;
    int data_type; uint32_t adler; uint32_t reserved;
} z_stream;
_Static_assert(sizeof(z_stream) == 88, "z_stream must match the Win64 zlib layout");

#define Z_NO_FLUSH   0
#define Z_FINISH     4
#define Z_OK         0
#define Z_STREAM_END 1
#define Z_DEFLATED   8

typedef int (*deflateInit2_fn)(z_stream*, int level, int method, int windowBits,
                               int memLevel, int strategy, const char* version, int size);
typedef int (*deflate_fn)(z_stream*, int flush);
typedef int (*deflateEnd_fn)(z_stream*);

static deflateInit2_fn p_deflateInit2;
static deflate_fn      p_deflate;
static deflateEnd_fn   p_deflateEnd;

/* --- job ------------------------------------------------------------------ */
typedef struct {
    char     path[1024];
    uint8_t* hdr;      /* plaintext header, exactly as the original writes it */
    size_t   hdr_len;
    stdstr   body;     /* copy of the recording, owned by the game allocator  */
} job_t;

static CRITICAL_SECTION g_lock;
static HANDLE g_workers[MAX_WORKERS];

static void* xalloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void  xfree(void* p)   { if (p) HeapFree(GetProcessHeap(), 0, p); }

static int write_all(HANDLE h, const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    while (n) {
        DWORD chunk = n > 0x40000000u ? 0x40000000u : (DWORD)n, w = 0;
        if (!WriteFile(h, b, chunk, &w, NULL) || w == 0) return 0;
        b += w; n -= w;
    }
    return 1;
}

/* Header + gzip body -> file.  Runs on the worker (or inline at shutdown). */
static void run_job(job_t* j)
{
    std_string_clear_fn str_clear = (std_string_clear_fn)pk_ptr(STD_STRING_CLEAR_RVA);
    const uint8_t* body = (const uint8_t*)ss_data(&j->body);
    uint64_t body_len = j->body.size;
    DWORD t0 = GetTickCount();

    HANDLE h = CreateFileA(j->path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        pk_logf("replay_async_finalize: cannot open '%s' (err %lu)", j->path, GetLastError());
        goto done;
    }
    if (!write_all(h, j->hdr, j->hdr_len)) {
        pk_logf("replay_async_finalize: header write failed (err %lu)", GetLastError());
        goto close;
    }

    {
        z_stream zs; memset(&zs, 0, sizeof zs);
        /* Same parameters as the original: level 6, gzip wrapper, memLevel 9. */
        int rc = p_deflateInit2(&zs, 6, Z_DEFLATED, 31, 9, 0, "1.2.11", (int)sizeof zs);
        if (rc != Z_OK) {
            pk_logf("replay_async_finalize: deflateInit2 failed (%d)", rc);
            goto close;
        }
        uint8_t* out = (uint8_t*)xalloc(OUT_CHUNK);
        uint64_t pos = 0;
        int ok = 1;
        for (;;) {
            uint32_t take = body_len - pos > IN_CHUNK ? IN_CHUNK : (uint32_t)(body_len - pos);
            int flush = (pos + take == body_len) ? Z_FINISH : Z_NO_FLUSH;
            zs.next_in = body + pos; zs.avail_in = take;
            do {
                zs.next_out = out; zs.avail_out = OUT_CHUNK;
                rc = p_deflate(&zs, flush);
                if (rc < 0) { pk_logf("replay_async_finalize: deflate error %d", rc); ok = 0; break; }
                if (!write_all(h, out, OUT_CHUNK - zs.avail_out)) {
                    pk_logf("replay_async_finalize: body write failed (err %lu)", GetLastError());
                    ok = 0; break;
                }
            } while (zs.avail_out == 0);
            if (!ok) break;
            pos += take;
            if (flush == Z_FINISH) {
                if (rc != Z_STREAM_END) { pk_logf("replay_async_finalize: stream not finished"); }
                break;
            }
        }
        p_deflateEnd(&zs);
        xfree(out);
        if (ok)
            pk_logf("replay_async_finalize: wrote '%s' (%llu bytes body, %lu ms)",
                    j->path, (unsigned long long)body_len, (unsigned long)(GetTickCount() - t0));
    }
close:
    CloseHandle(h);
done:
    str_clear(&j->body);
    xfree(j->hdr);
    xfree(j);
}

static DWORD WINAPI worker_main(LPVOID p)
{
    run_job((job_t*)p);
    return 0;
}

/* Reap finished workers; with `wait` block until all are done. */
static void reap_workers(int wait)
{
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (!g_workers[i]) continue;
        if (WaitForSingleObject(g_workers[i], wait ? INFINITE : 0) == WAIT_OBJECT_0) {
            CloseHandle(g_workers[i]);
            g_workers[i] = NULL;
        }
    }
}

/* --- header snapshot ------------------------------------------------------ */
static void put(uint8_t** p, const void* src, size_t n) { memcpy(*p, src, n); *p += n; }
static void put_u32(uint8_t** p, uint32_t v) { put(p, &v, 4); }

static uint8_t* build_header(size_t* out_len)
{
    const stdstr* s1 = (const stdstr*)pk_ptr(G_HDR_STR1_RVA);
    const stdstr* s2 = (const stdstr*)pk_ptr(G_HDR_STR2_RVA);
    const uint8_t* pb = *(const uint8_t**)pk_ptr(G_PACKETS_BEGIN_RVA);
    const uint8_t* pe = *(const uint8_t**)pk_ptr(G_PACKETS_END_RVA);
    uint32_t count = (uint32_t)((pe - pb) / PACKET_ELEM_SIZE);
    uint32_t len1 = (uint32_t)s1->size, len2 = (uint32_t)s2->size;

    size_t n = 4 + 4 + (4 + 9) + (4 + len1) + (4 + len2) + 8 + 8 + 4;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* el = pb + (size_t)i * PACKET_ELEM_SIZE;
        n += 8 + 4 + *(const uint32_t*)(el + 0x10);
    }

    uint8_t* hdr = (uint8_t*)xalloc(n);
    if (!hdr) return NULL;
    uint8_t* p = hdr;
    put(&p, "EVGR", 4);
    put_u32(&p, 6);
    put_u32(&p, 9);            put(&p, pk_ptr(G_APP_VERSION_RVA), 9);
    put_u32(&p, len1);         put(&p, ss_data(s1), len1);
    put_u32(&p, len2);         put(&p, ss_data(s2), len2);
    put(&p, pk_ptr(G_DURATION_RVA), 8);
    put(&p, pk_ptr(G_EXTRA_DURATION_RVA), 8);
    put_u32(&p, count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* el = pb + (size_t)i * PACKET_ELEM_SIZE;
        uint32_t len = *(const uint32_t*)(el + 0x10);
        put(&p, el + 0x20, 8);
        put_u32(&p, len);
        put(&p, ss_data((const stdstr*)el), len);
    }
    *out_len = n;
    return hdr;
}

/* --- the replacement for replay_recording_finalize ------------------------ */
void pk_replay_finalize(void)
{
    stdstr*  path   = (stdstr*)pk_ptr(G_REC_PATH_RVA);
    uint8_t* stream = *(uint8_t**)pk_ptr(G_REC_STREAM_RVA);
    int shutting_down = *(volatile uint8_t*)pk_ptr(G_SHUTDOWN_FLAG_RVA) != 0 ||
                        __builtin_return_address(0) == pk_ptr(SHUTDOWN_CALLSITE_RVA + 5);

    EnterCriticalSection(&g_lock);
    reap_workers(shutting_down);

    if (path->size == 0 || stream == NULL) { LeaveCriticalSection(&g_lock); return; }

    job_t* j = (job_t*)xalloc(sizeof *j);
    if (!j) { LeaveCriticalSection(&g_lock); return; }
    size_t plen = path->size < sizeof j->path - 1 ? path->size : sizeof j->path - 1;
    memcpy(j->path, ss_data(path), plen);
    j->path[plen] = 0;
    j->hdr = build_header(&j->hdr_len);

    ((stringbuf_str_fn)pk_ptr(STRINGBUF_STR_RVA))(stream + 8, &j->body);

    /* Same as the original: an empty path stops the recorders and turns any
     * later finalize call into a no-op. */
    ((std_string_assign_fn)pk_ptr(STD_STRING_ASSIGN_RVA))(path, "", 0);

    if (!j->hdr) {
        pk_logf("replay_async_finalize: header alloc failed; demo dropped");
        ((std_string_clear_fn)pk_ptr(STD_STRING_CLEAR_RVA))(&j->body);
        xfree(j);
        LeaveCriticalSection(&g_lock);
        return;
    }

    if (shutting_down) {
        pk_logf("replay_async_finalize: shutdown path, finalizing synchronously");
        run_job(j);
        LeaveCriticalSection(&g_lock);
        return;
    }

    HANDLE t = CreateThread(NULL, 0, worker_main, j, 0, NULL);
    if (!t) {
        pk_logf("replay_async_finalize: CreateThread failed; finalizing synchronously");
        run_job(j);
    } else {
        int slot = -1;
        for (int i = 0; i < MAX_WORKERS && slot < 0; i++) if (!g_workers[i]) slot = i;
        if (slot < 0) { reap_workers(1); slot = 0; }
        g_workers[slot] = t;
        pk_logf("replay_async_finalize: %llu bytes handed to worker",
                (unsigned long long)j->body.size);
    }
    LeaveCriticalSection(&g_lock);
}

PK_PLUGIN(replay_async_finalize)
{
    static const uint8_t entry_orig[5] = { 0x48,0x89,0x5C,0x24,0x08 };   /* mov [rsp+8],rbx */
    static const uint8_t shutdown_call[5] = { 0xE8,0xB3,0x98,0xCC,0xFF }; /* call finalize   */

    if (pk_verify(FINALIZE_RVA, "\xE9", 1)) {
        pk_logf("replay_async_finalize: already detoured");
        return PK_OK;
    }
    if (!pk_verify(FINALIZE_RVA, entry_orig, sizeof entry_orig)) {
        pk_logf("replay_async_finalize: finalize prologue mismatch; aborting");
        return PK_FAIL;
    }
    if (!pk_verify(SHUTDOWN_CALLSITE_RVA, shutdown_call, sizeof shutdown_call))
        pk_logf("replay_async_finalize: warning: end_frame call site mismatch; relying on shutdown flag only");

    HMODULE z = LoadLibraryA("zlib1.dll");
    if (!z) {
        pk_logf("replay_async_finalize: zlib1.dll not found (err %lu); aborting", GetLastError());
        return PK_FAIL;
    }
    p_deflateInit2 = (deflateInit2_fn)GetProcAddress(z, "deflateInit2_");
    p_deflate      = (deflate_fn)     GetProcAddress(z, "deflate");
    p_deflateEnd   = (deflateEnd_fn)  GetProcAddress(z, "deflateEnd");
    if (!p_deflateInit2 || !p_deflate || !p_deflateEnd) {
        pk_logf("replay_async_finalize: zlib1.dll exports missing; aborting");
        return PK_FAIL;
    }

    InitializeCriticalSection(&g_lock);

    /* The whole function is replaced, so nothing is re-executed: the cave is just
     * an absolute jump into the C replacement.  Register state at entry equals a
     * normal call, so the replacement returns straight to the original caller. */
    static const uint8_t code[] = {
        0x48,0xB8,0,0,0,0,0,0,0,0,   /* 0x00 mov rax, pk_replay_finalize  imm@0x02 */
        0xFF,0xE0,                   /* 0x0A jmp rax                                */
    };
    pk_cave c = pk_cave_open(sizeof code);
    if (!c.base) { pk_logf("replay_async_finalize: cave alloc failed"); return PK_FAIL; }
    pk_emit(&c, code, sizeof code);
    { void* fn = (void*)&pk_replay_finalize; memcpy(c.base + 0x02, &fn, sizeof fn); }

    if (!pk_detour(&c, FINALIZE_RVA, sizeof entry_orig)) {
        pk_logf("replay_async_finalize: detour failed");
        return PK_FAIL;
    }
    pk_logf("replay_async_finalize: demo finalization moved to a worker thread");
    return PK_OK;
}
