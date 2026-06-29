/* loader.c - DllMain + plugin runner.
 *
 * This file is proxy-agnostic.  The proxy_*.c that ships alongside it provides
 * the exported forwarders the game imports; this file only wires up the patch
 * pass.  We run plugins on a worker thread so DllMain returns immediately and
 * we never touch the loader lock while patching. */
#include "patchkit.h"
#include <windows.h>
#include <stdio.h>

/* console_banner.c: prints a "patches loaded: ..." line into the in-game
 * console at startup (see that file for the hook mechanism). */
void pk_banner_set(const char* text);
int  pk_banner_install(void);

static DWORD WINAPI pk_thread(LPVOID arg)
{
    (void)arg;
    pk_logf("patchkit: image base=%p, %d plugin(s) selected",
            (void*)pk_base(), pk_plugin_count);

    /* Run every selected plugin, recording the names that applied so we can show
     * them in-game. */
    char list[400];
    int  loff = 0;
    int  ok   = 0;
    list[0] = '\0';
    for (int i = 0; i < pk_plugin_count; i++) {
        int r = pk_plugins[i].init();
        pk_logf("  [%s] %s", pk_plugins[i].name, r ? "applied" : "FAILED");
        if (r == PK_OK) {
            if (loff < (int)sizeof list - 1) {
                int w = snprintf(list + loff, sizeof list - loff, "%s%s",
                                 ok ? ", " : "", pk_plugins[i].name);
                if (w > 0) {
                    loff += w;
                    if (loff > (int)sizeof list - 1) loff = (int)sizeof list - 1;
                }
            }
            ok++;
        }
    }
    pk_logf("patchkit: %d/%d plugin(s) applied", ok, pk_plugin_count);

    /* In-game console banner (pink), shown at startup next to the engine version
     * line.  Hooks the version-print function, so it is timing-safe regardless of
     * how early this worker thread runs. */
    char banner[480];
    if (ok > 0)
        snprintf(banner, sizeof banner, "^ff66cc[patchkit] patches loaded: %s", list);
    else
        snprintf(banner, sizeof banner, "^ff66cc[patchkit] no patches loaded");
    pk_banner_set(banner);
    if (pk_banner_install() != PK_OK)
        pk_logf("patchkit: console banner hook failed (banner not shown in-game)");

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        pk_set_self(hinst);
        HANDLE t = CreateThread(NULL, 0, pk_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
