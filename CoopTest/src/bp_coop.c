/* bp_coop.c - Boiling Point co-op, phase 0 connection test.
 *
 * Built as dinput.dll and placed next to Xenus.exe. vital.dll imports DINPUT.dll, so Windows loads
 * this file at game start. DirectInputCreateA is passed through to the real system dinput.dll.
 *
 * What it does:
 *  - Hooks stGetCmdLine (vital.dll) as imported by game.dll. gameLaunch() reads its options through
 *    it, and New Game always resets the command line to "map\world.map". We append
 *    "-CTFSERVER ..." (host) or "-CONNECT <ip>" (join) from bp_coop.ini.
 *  - Optional: allow_two_instances=1 renames the "XenusMutex" so two copies can run on one PC.
 *  - Logs everything to CoopTest\bp_coop.log.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <tlhelp32.h>

static char g_dir[MAX_PATH];      /* game folder, with trailing backslash */
static char g_ini[MAX_PATH];
static FILE *g_log;
static char g_mode[16];           /* off / host / join */
static char g_ip[64];
static char g_name[32];
static int  g_maxplayers;
static int  g_two;
static int  g_hooked_cmd;
static int  g_hooked_mutex;

static void logf(const char *fmt, ...)
{
    va_list ap; SYSTEMTIME st;
    static CRITICAL_SECTION cs; static LONG init;
    if (!g_log) return;
    if (!InterlockedExchange(&init, 1)) InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&cs);
}

/* ---- pass-through of the real dinput.dll ---- */
typedef HRESULT (WINAPI *fn_DICreateA)(HINSTANCE, DWORD, LPVOID *, LPUNKNOWN);
static HMODULE g_real;
static fn_DICreateA g_real_create;

static void load_real(void)
{
    char p[MAX_PATH];
    if (g_real) return;
    GetSystemDirectoryA(p, MAX_PATH);      /* SysWOW64 for a 32-bit process */
    strcat(p, "\\dinput.dll");
    g_real = LoadLibraryA(p);
    if (g_real) g_real_create = (fn_DICreateA)GetProcAddress(g_real, "DirectInputCreateA");
    logf("real dinput: %s -> %p / %p", p, (void *)g_real, (void *)g_real_create);
}

/* ---- IAT patching ---- */
static void **find_iat(HMODULE mod, const char *dll, const char *func)
{
    BYTE *b = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *d;
    DWORD rva;
    if (!b) return NULL;
    nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return NULL;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(b + rva); d->Name; d++) {
        IMAGE_THUNK_DATA *names, *addrs;
        if (_stricmp((char *)(b + d->Name), dll)) continue;
        names = (IMAGE_THUNK_DATA *)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        addrs = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            IMAGE_IMPORT_BY_NAME *n;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            n = (IMAGE_IMPORT_BY_NAME *)(b + names->u1.AddressOfData);
            if (!strcmp((char *)n->Name, func)) return (void **)&addrs->u1.Function;
        }
    }
    return NULL;
}

static void **find_iat_ord(HMODULE mod, const char *dll, WORD ord)
{
    BYTE *b = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *d;
    DWORD rva;
    if (!b) return NULL;
    nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return NULL;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(b + rva); d->Name; d++) {
        IMAGE_THUNK_DATA *names, *addrs;
        if (_stricmp((char *)(b + d->Name), dll)) continue;
        names = (IMAGE_THUNK_DATA *)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        addrs = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++)
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) && IMAGE_ORDINAL(names->u1.Ordinal) == ord)
                return (void **)&addrs->u1.Function;
    }
    return NULL;
}

static void *patch_iat(void **slot, void *fn)
{
    DWORD old; void *prev;
    if (!slot) return NULL;
    VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old);
    prev = *slot; *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return prev;
}

/* ---- stGetCmdLine hook ---- */
typedef void (__cdecl *fn_getcmd)(char *);
static fn_getcmd o_getcmd;

static void __cdecl hook_getcmd(char *out)
{
    char extra[256] = "";
    o_getcmd(out);
    logf("gameLaunch: original command line = \"%s\"", out);
    if (!_stricmp(g_mode, "host")) {
        sprintf(extra, " -CTFSERVER -MAXPLAYERS %d -SERVERNAME COOP -NAME %s", g_maxplayers, g_name);
    } else if (!_stricmp(g_mode, "join")) {
        /* -CONNECT takes everything after it, so it must be last */
        sprintf(extra, " -NAME %s -CONNECT %s", g_name, g_ip);
        /* "Load Game" passes an empty command line, and without a map the client never connects */
        if (!out[0]) { strcpy(out, "map\\world.map"); logf("gameLaunch: client had no map (Load Game), using map\\world.map"); }
    }
    if (extra[0] && !strstr(out, "-CTFSERVER") && !strstr(out, "-CONNECT") &&
        !strstr(out, "-ctfserver") && !strstr(out, "-connect")) {
        strcat(out, extra);   /* gameLaunch's buffer is 1024 bytes */
    }
    logf("gameLaunch: command line used    = \"%s\"", out);
}

/* ---- stStartGame hook ----
 * gameLaunch sets game.dll's mode byte (RVA 0x11F21C): 0 = single player, 1 = (deathmatch?), 2 = CTF.
 * -CTFSERVER / -CONNECT set it to 2, which enables the network but also makes the game look for CTF
 * spawn points (type 2/3), which the campaign world does not have. By the time stStartGame is called
 * the network is already switched on, so we put the byte back to 0: campaign spawns and rules. */
#define GAME_MODE_RVA 0x11F21C
typedef int (__cdecl *fn_startgame)(void);
static fn_startgame o_startgame;
static int g_keep_ctf;

static int __cdecl hook_startgame(void)
{
    BYTE *mode = (BYTE *)GetModuleHandleA("game.dll") + GAME_MODE_RVA;
    int r;
    logf("stStartGame: game mode byte = %d", *mode);
    /* only the host: a joining client keeps mode 2 so it takes all entities from the server
     * instead of loading the campaign's objects itself (that clashed and crashed clNetworkFrame) */
    if (!g_keep_ctf && !_stricmp(g_mode, "host") && *mode == 2) {
        *mode = 0;
        logf("stStartGame: game mode byte set to 0 (campaign rules, network stays on)");
    }
    /* client: entity data is written and read with code that branches on this byte, so the client should use
     * the same value as the host (0). bp_coop.ini client_mode_byte=2 restores the old behaviour. */
    if (!_stricmp(g_mode, "join")) {
        int want = GetPrivateProfileIntA("coop", "client_mode_byte", 0, g_ini);
        if (*mode != want) { logf("stStartGame: client game mode byte %d -> %d (same as host)", *mode, want); *mode = (BYTE)want; }
    }
    /* gameNewFrame runs the campaign systems (missions, scenario, game time, AI, ...) only when
     * stMode is 1 (single player) or 2. A host runs as stMode 3 (server), so the world never
     * starts streaming and the loading screen hangs. Make the "== 2" test read "== 3" on the host. */
    if (!_stricmp(g_mode, "host")) {
        BYTE *p = (BYTE *)GetModuleHandleA("game.dll") + 0x13F14;
        static const BYTE want[] = { 0x83, 0xF8, 0x01, 0x74, 0x09, 0x83, 0xF8, 0x02, 0x0F, 0x85 };
        if (!memcmp(p, want, sizeof want)) {
            DWORD old;
            VirtualProtect(p + 7, 1, PAGE_EXECUTE_READWRITE, &old);
            p[7] = 0x03;
            VirtualProtect(p + 7, 1, old, &old);
            logf("gameNewFrame: campaign systems enabled for server mode (stMode 3)");
        } else logf("gameNewFrame patch: bytes differ, not patched");
    } else if (!_stricmp(g_mode, "join") && !GetPrivateProfileIntA("coop", "client_campaign", 0, g_ini)) {
        /* the host runs the story (missions, dialogs, intro cutscene, AI); the client must not run its own copy.
         * cmp eax,2 -> cmp eax,0x7F so stMode 2 skips the campaign block. */
        BYTE *p = (BYTE *)GetModuleHandleA("game.dll") + 0x13F14;
        static const BYTE want[] = { 0x83, 0xF8, 0x01, 0x74, 0x09, 0x83, 0xF8, 0x02, 0x0F, 0x85 };
        if (!memcmp(p, want, sizeof want)) {
            DWORD old;
            VirtualProtect(p + 7, 1, PAGE_EXECUTE_READWRITE, &old);
            p[7] = 0x7F;
            VirtualProtect(p + 7, 1, old, &old);
            logf("gameNewFrame: campaign systems off on the client (host runs the story)");
        } else logf("gameNewFrame client patch: bytes differ, not patched");
    }
    r = o_startgame();
    logf("stStartGame returned %d", r);
    return r;
}

/* ---- CreateMutexA hook (two copies on one PC) ---- */
typedef HANDLE (WINAPI *fn_mutex)(LPSECURITY_ATTRIBUTES, BOOL, LPCSTR);
static fn_mutex o_mutex;

static HANDLE WINAPI hook_mutex(LPSECURITY_ATTRIBUTES sa, BOOL own, LPCSTR name)
{
    char n[64];
    if (name && !strcmp(name, "XenusMutex")) {
        sprintf(n, "XenusMutex_%lu", GetCurrentProcessId());
        logf("XenusMutex renamed to %s (two instances allowed)", n);
        return o_mutex(sa, own, n);
    }
    return o_mutex(sa, own, name);
}

/* ---- bind hook (join mode): the engine always binds UDP 15011. On one PC the host already has it,
 * so the joining copy uses 15012 (or port from bp_coop.ini client_port). The server answers to the
 * packet's source address, so the client's own port does not need to be 15011. ---- */
typedef int (WINAPI *fn_bind)(UINT_PTR, const struct sockaddr *, int);
static fn_bind o_bind;
static int g_client_port;

static int WINAPI hook_bind(UINT_PTR s, const struct sockaddr *a, int len)
{
    if (a && len >= 16 && a->sa_family == 2 /* AF_INET */) {
        struct sockaddr copy = *a;
        BYTE *pp = (BYTE *)copy.sa_data;
        int port = (pp[0] << 8) | pp[1];
        if (port == 15011 && g_client_port) {
            pp[0] = (BYTE)(g_client_port >> 8); pp[1] = (BYTE)g_client_port;
            logf("bind: UDP port 15011 -> %d (join mode)", g_client_port);
            return o_bind(s, &copy, len);
        }
    }
    return o_bind(s, a, len);
}

/* ---- CreateFileA hook (two copies on one PC): the engine opens resource.qrc etc. without read
 * sharing, so a second copy cannot open them. Add FILE_SHARE_READ to read-only opens. ---- */
typedef HANDLE (WINAPI *fn_createfile)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static fn_createfile o_createfile;

static HANDLE WINAPI hook_createfile(LPCSTR n, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                     DWORD disp, DWORD flags, HANDLE tmpl)
{
    HANDLE h;
    share |= FILE_SHARE_READ | FILE_SHARE_WRITE;
    h = o_createfile(n, acc, share, sa, disp, flags, tmpl);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)
            logf("CreateFileA failed: %s acc=%08lX share=%lX disp=%lu err=%lu", n ? n : "?", acc, share, disp, e);
        SetLastError(e);
    }
    return h;
}

static void hook_createfile_all(void)
{
    /* every module in the game folder that imports CreateFileA */
    HMODULE self = NULL; MODULEENTRY32 me; HANDLE snap;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)hook_createfile, &self);
    if (!o_createfile) o_createfile = (fn_createfile)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateFileA");
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    me.dwSize = sizeof me;
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        void **slot;
        if (me.hModule == self || _strnicmp(me.szExePath, g_dir, strlen(g_dir))) continue;
        slot = find_iat(me.hModule, "kernel32.dll", "CreateFileA");
        if (slot && *slot != (void *)hook_createfile) {
            patch_iat(slot, (void *)hook_createfile);
            logf("CreateFileA read-sharing hook: %s", me.szModule);
        }
    }
    CloseHandle(snap);
}

/* ---- host loading a save: LoadGameChecked (Xenus.exe) calls gameLaunch (server start, stMode 3) and then
 * lsmLoadFile, which restores the save and leaves the engine in single player (stMode 1). Put it back. ---- */
typedef int (__cdecl *fn_lsmload)(char *);
static fn_lsmload o_lsmload;
static int pid_sweep(const char *why);

static int __cdecl hook_lsmload(char *file)
{
    int r, *mode = (int *)GetProcAddress(GetModuleHandleA("vital.dll"), "?stMode@@3HA"), before;
    before = mode ? *mode : -1;
    if (!_stricmp(g_mode, "join")) {
        /* a client takes the world from the host; loading its own save would make it a separate game
         * (and it hung before). Skip it, so "Load Game" on the client acts like New Game. */
        logf("lsmLoadFile(%s) skipped on the client: the world comes from the host", file ? file : "(null)");
        return 1;
    }
    r = o_lsmload(file);
    logf("lsmLoadFile(%s) = %d, stMode %d -> %d", file ? file : "(null)", r, before, mode ? *mode : -1);
    if (mode && *mode == 1) {
        *mode = 3;
        logf("save loaded: stMode set back to 3 (server) so players can join");
    }
    if (mode && *mode == 3) pid_sweep("after save load");
    return r;
}

/* ---- guest spawn fallback: gameSvAttach (game.dll, thunk +0x16250 -> body +0xE030) spawns a joining
 * player at a start point of the "birth" map part. After a save load that part is not loaded, the list is
 * empty and the guest is refused. Then we create the guest's player next to the host's character, using the
 * same steps the game does (new pid, NETMSG 's' create-player, svSetFocus). ---- */
#define ATTACH_THUNK 0x16250
#define ATTACH_BODY  0x0E030
typedef int  (__cdecl *fn_attach)(int);
typedef int  (__cdecl *fn_i_i)(int);
typedef void (__cdecl *fn_bpinit)(void *, void *);
typedef void (__cdecl *fn_bpwrite)(void *, void *, int);
typedef void (__cdecl *fn_bporigin)(void *, float *);
typedef void (__cdecl *fn_bpangle)(void *, float);
typedef void (__cdecl *fn_svsend)(unsigned char, unsigned char, void *);
typedef void (__cdecl *fn_svfocus)(unsigned short, int, int);
static fn_attach o_attach;

static int g_attach_logged[32];
static int g_spawn_near_host;   /* ini spawn_near_host: 1 = old behaviour, guest appears next to the host */

/* vital.dll helpers, looked up once */
static struct {
    int ok, *mode; BYTE *machine; char *cur_wclient; void **sv_ent, **smptr;
    fn_i_i getpid, clfocus; fn_bpinit bpinit; fn_bpwrite bpwrite; fn_bporigin bporigin; fn_bpangle bpangle;
    fn_svsend svsend; fn_svfocus svfocus;
    BYTE *sv_bits; void **cl_ent;     /* sv_bits[pid][31]: per machine (column = machine-1) */
} V;
static int vital_load(void);

/* EN_GetFreePID(2) skips a global pid (1..0xC00) when any active machine still has bit 2 set for it in
 * sv_bits. After a save load these bits are left over from the entities that were replaced, so every
 * global pid looks taken ("Error, too many entities!!!"), the guest got pid 0 and the host crashed.
 * Clear the bits of pids that have no server entity. Pass 1 only touches pids the local client has no
 * entity for either; pass 2 (only if pass 1 freed too little) clears the rest. Returns the free count. */
static int pid_sweep(const char *why)
{
    int pid, c, pass, cleared[31], freed = 0, still = 0;
    if (!vital_load()) return -1;
    for (pass = 1; pass <= 2; pass++) {
        memset(cleared, 0, sizeof cleared);
        for (pid = 1; pid <= 0xC00; pid++) {
            BYTE *row = V.sv_bits + pid * 31, any = 0;
            if (V.sv_ent[pid]) continue;
            if (pass == 1 && V.cl_ent[pid]) continue;
            for (c = 0; c < 30; c++) {
                if (!(row[c] & 2)) continue;
                any = 1; cleared[c]++;
                row[c] = 0;
            }
            (void)any;
        }
        freed = 0; still = 0;
        for (pid = 1; pid <= 0xC00; pid++) {
            BYTE b = 0;
            if (V.sv_ent[pid]) continue;
            for (c = 0; c < 30; c++) if (*(int *)(V.machine + (c + 1) * 0xd8c + 4) == 2) b |= V.sv_bits[pid * 31 + c];
            if (b & 2) still++; else freed++;
        }
        {
            char s[256]; int n = 0;
            s[0] = 0;
            for (c = 0; c < 30; c++) if (cleared[c] && n < 230) n += wsprintfA(s + n, " m%d:%d", c + 1, cleared[c]);
            logf("pid sweep (%s) pass %d: cleared stale bits%s; free global pids now %d, still blocked %d",
                 why, pass, n ? s : " (none)", freed, still);
        }
        if (freed >= 64) break;
    }
    return freed;
}

static int vital_load(void)
{
    HMODULE v = GetModuleHandleA("vital.dll");
    if (V.ok) return 1;
    if (!v) return 0;
#define GP(dst, name) if (!(*(void **)&(dst) = (void *)GetProcAddress(v, name))) { logf("vital export missing: %s", name); return 0; }
    GP(V.mode, "?stMode@@3HA");
    GP(V.machine, "?machine@@3PAUt_machine@@A");
    GP(V.cur_wclient, "?cur_wclient@@3DA");
    GP(V.sv_ent, "?sv_ent@@3PAPAVt_entity@@A");
    GP(V.smptr, "?SmPtr@@3PAPAVt_staticmesh@@A");
    GP(V.getpid, "EN_GetFreePID");               /* exported undecorated */
    GP(V.clfocus, "clGetFocus");                 /* exported undecorated */
    GP(V.bpinit, "?bpInit@@YAXPAUt_bitptr@@PAX@Z");
    GP(V.bpwrite, "?bpWrite@@YAXPAUt_bitptr@@PAXH@Z");
    GP(V.bporigin, "?bpWriteOrigin@@YAXPAUt_bitptr@@QAM@Z");
    GP(V.bpangle, "?bpWriteAngle@@YAXPAUt_bitptr@@M@Z");
    GP(V.svsend, "?svSendMessage@@YAXEEPAUt_bitptr@@@Z");
    GP(V.svfocus, "?svSetFocus@@YAXGHH@Z");
    GP(V.sv_bits, "?sv_bits@@3PAY0BP@EA");
    GP(V.cl_ent, "?cl_ent@@3PAPAVt_entity@@A");
#undef GP
    V.ok = 1;
    return 1;
}

/* the host's own character (server-side entity), or NULL */
static BYTE *host_player(void)
{
    int hp = V.clfocus(10);
    BYTE *e = (hp > 0 && hp < 4096) ? (BYTE *)V.sv_ent[hp] : NULL;
    return (e && *(DWORD *)(e + 8) == 0x706c6179) ? e : NULL;     /* 'play' */
}

static BYTE *player_info(int mid)  /* game.dll table: team byte, name at +1, player pid (short) at +0x100 */
{
    return (BYTE *)GetModuleHandleA("game.dll") + 0x11D12C + mid * 0x110;
}

static int __cdecl hook_attach(int mid)
{
    BYTE *game = (BYTE *)GetModuleHandleA("game.dll"), *info = player_info(mid), *host;
    int r, birth = *(int *)(game + 0x116CE4), pid, i, quiet;   /* +0x116CE4 = mapGetBirthSMID() */
    float pos[3]; BYTE bp[64], buf[256], nl, zero = 0;

    if (!vital_load()) return o_attach(mid);
    quiet = mid >= 0 && mid < 32 && g_attach_logged[mid] > 3;   /* the engine retries every frame */
    if (mid >= 2 && mid < 32 && !g_attach_logged[mid] && *V.mode == 3) pid_sweep("guest attach");
    if (mid >= 2 && *V.mode == 3 && game[0x11F21C] == 0 && birth >= 0 && !V.smptr[birth] && !g_spawn_near_host) {
        /* load the New Game start area on the host (the client loads it before it connects), so the
         * game's own spawn code can place the guest there. Spawning in an area the client has not
         * loaded yet crashed the host's renderer ("Invalid object pId ... rooms objects list"). */
        typedef void (__cdecl *fn_loadsm)(unsigned long);
        fn_loadsm loadsm = (fn_loadsm)GetProcAddress((HMODULE)game, "?mapLoadSM@@YAXK@Z");
        if (loadsm) {
            loadsm((unsigned long)birth);
            if (!quiet) logf("gameSvAttach(machine %d): loaded start area %d on the host: %s", mid, birth,
                             V.smptr[birth] ? "ok" : "still not loaded");
        } else if (!quiet) logf("mapLoadSM export missing");
    }
    if (mid >= 2 && *V.mode == 3 && game[0x11F21C] == 0 && birth >= 0 && !V.smptr[birth]) {
        /* the game would "succeed" but never create the player (SmPtr[birth]=NULL); do it ourselves */
        if (!quiet) logf("gameSvAttach(machine %d): birth map part %d not loaded, using spawn fallback", mid, birth);
        r = 0;
    } else {
        r = o_attach(mid);
        if (!quiet) logf("gameSvAttach(machine %d) = %d", mid, r);
    }
    if (mid >= 0 && mid < 32) g_attach_logged[mid]++;
    if (r || mid < 2 || mid > 30 || *V.mode != 3 || game[0x11F21C] != 0) return r;
    host = host_player();
    if (!host) { if (!quiet) logf("spawn fallback: host character not found"); return r; }
    memcpy(pos, host + 0x20, sizeof pos);
    pos[0] += 150.0f;                                  /* a step to the side of the host */

    pid = V.getpid(2);
    if (pid <= 0 || pid > 0xC00) {
        pid_sweep("guest spawn");
        pid = V.getpid(2);
    }
    if (pid <= 0 || pid > 0xC00) {
        /* never hand out pid 0: that crashed the host (t_car::svRcvMsg). Refuse; the engine retries. */
        if (!quiet) logf("spawn fallback: no free entity id for %s (machine %d), not spawning yet", (char *)info + 1, mid);
        return 0;
    }

    /* tell the other players "<name> joined" (message 0x81, same layout as the game) */
    for (nl = 0; info[1 + nl] && nl < 200; nl++) {}
    memset(bp, 0, sizeof bp); V.bpinit(bp, buf);
    V.bpwrite(bp, &zero, 5); V.bpwrite(bp, info, 8); V.bpwrite(bp, &nl, 8); V.bpwrite(bp, info + 1, nl * 8);
    for (i = 1; i < 31; i++)
        if (i != mid && *(int *)(V.machine + i * 0xd8c + 4) == 2) V.svsend((unsigned char)i, 0x81, bp);

    *(int *)(V.machine + mid * 0xd8c + 4) = 2;
    *V.cur_wclient = (char)(mid - 1);
    *(short *)(info + 0x100) = (short)pid;
    memset(bp, 0, sizeof bp); V.bpinit(bp, buf);
    V.bpwrite(bp, &pid, 16); V.bporigin(bp, pos); V.bpangle(bp, 0.0f);
    V.svsend((unsigned char)mid, 's', bp);             /* NETMSG_CREATEPLAYER */
    V.svfocus((unsigned short)mid, 4, pid); V.svfocus((unsigned short)mid, 1, pid);
    V.svfocus((unsigned short)mid, 0, pid); V.svfocus((unsigned short)mid, 10, pid);
    logf("spawn fallback: created player pid %d for %s (machine %d) next to the host at %.0f %.0f %.0f",
         pid, (char *)info + 1, mid, pos[0], pos[1], pos[2]);
    g_attach_logged[mid] = 0;
    return 1;
}

/* host hotkey F8: move every guest's character next to the host (main thread, from net_check) */
static volatile LONG g_bring_guests;
static int g_admin_on, g_inited15;
static void admin_push(const char *s);

static void bring_guests(void)
{
    BYTE *host, *g; int mid, pid, n = 0; float pos[3];
    if (!vital_load() || !(host = host_player())) { logf("F8: host character not found"); return; }
    for (mid = 2; mid < 31; mid++) {
        if (*(int *)(V.machine + mid * 0xd8c + 4) != 2) continue;
        pid = *(short *)(player_info(mid) + 0x100);
        g = (pid > 0 && pid < 4096) ? (BYTE *)V.sv_ent[pid] : NULL;
        if (!g || *(DWORD *)(g + 8) != 0x706c6179) continue;
        memcpy(pos, host + 0x20, sizeof pos); pos[0] += 150.0f + 100.0f * n;
        memcpy(g + 0x20, pos, sizeof pos);                         /* same fields the game's own teleport sets */
        if (*(BYTE **)(g + 0x928) && !IsBadWritePtr(*(BYTE **)(g + 0x928) + 0xfc, 12))
            memcpy(*(BYTE **)(g + 0x928) + 0xfc, pos, sizeof pos);
        logf("F8: moved %s (pid %d) to the host", (char *)player_info(mid) + 1, pid);
        n++;
    }
    if (!n) logf("F8: no guest characters to move");
}

static DWORD WINAPI hotkey_thread(LPVOID u)
{
    int was = 0;
    for (;;) {
        DWORD fgpid = 0; int down;
        Sleep(50);
        GetWindowThreadProcessId(GetForegroundWindow(), &fgpid);
        down = fgpid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F8) & 0x8000);
        if (down && !was) InterlockedExchange(&g_bring_guests, 1);
        was = down;
    }
}

static void hook_attach_install(void)
{
    BYTE *game = (BYTE *)GetModuleHandleA("game.dll"), *p; DWORD old;
    static const BYTE want[] = { 0xE9, 0xDB, 0x7D, 0xFF, 0xFF };
    if (!game) return;
    p = game + ATTACH_THUNK;
    if (memcmp(p, want, sizeof want)) { logf("gameSvAttach thunk bytes differ, spawn fallback off"); return; }
    o_attach = (fn_attach)(game + ATTACH_BODY);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)(p + 1) = (DWORD)((BYTE *)hook_attach - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logf("gameSvAttach hooked (spawn fallback)");
}

/* ---- client: do not create world entities locally. When the client loads a map area, its NPCs,
 * bodies etc. were created on the client, sent to the host as the guest's own entities, and clashed
 * with the host's entities (same pids, different things) -> desync and crashes. The host already sends
 * the client its own copies. Only the guest's own player ('play') is still created locally. ---- */
typedef void (__cdecl *fn_create)(int, unsigned char *);
typedef long (__cdecl *fn_buftype)(unsigned char *);
static fn_create o_svcreate, o_stcreate;
static fn_buftype g_buftype;
static int g_blocked_creates;

static int create_allowed(unsigned char *buf, const char *who)
{
    int *mode = (int *)GetProcAddress(GetModuleHandleA("vital.dll"), "?stMode@@3HA");
    long t;
    if (!mode || *mode == 1 || *mode == 3 || !g_buftype || !buf) return 1;   /* not connected as a client */
    t = g_buftype(buf);
    if (t == 0x706c6179) return 1;                                            /* 'play' */
    g_blocked_creates++;
    if (g_blocked_creates <= 20 || g_blocked_creates % 500 == 0)
        logf("client: blocked %s of a '%.4s' entity (stMode %d, %d blocked so far)", who, (char *)&t, *mode, g_blocked_creates);
    return 0;
}
static void __cdecl hook_svcreate(int a, unsigned char *b) { if (create_allowed(b, "svCreateEntity")) o_svcreate(a, b); }
static void __cdecl hook_stcreate(int a, unsigned char *b) { if (create_allowed(b, "stCreateEntity")) o_stcreate(a, b); }

static void hook_client_creates(HMODULE game)
{
    void **s1 = find_iat(game, "vital.dll", "?svCreateEntity@@YAXHPAE@Z");
    void **s2 = find_iat(game, "vital.dll", "?stCreateEntity@@YAXHPAE@Z");
    g_buftype = (fn_buftype)GetProcAddress(GetModuleHandleA("vital.dll"), "?pwGetBufferType@@YAJPAE@Z");
    if (s1) o_svcreate = (fn_create)patch_iat(s1, (void *)hook_svcreate);
    if (s2) o_stcreate = (fn_create)patch_iat(s2, (void *)hook_stcreate);
    logf("client: entity creation filter %s (sv %s, st %s)", g_buftype ? "on" : "OFF (pwGetBufferType missing)",
         s1 ? "hooked" : "not found", s2 ? "hooked" : "not found");
}

/* ---- client send filter: on a client (stMode 4) the engine sends every networked entity it has to the
 * host (svNetworkSndFrame, vital+0x406F0, called from svNewFrame at vital+0x420E4). Entities the client
 * made itself from map areas (NPCs, bodies, breakables...) have pids that mean other things on the host.
 * Before each send, clear the "to server" bits (sv_bits column 0) of everything except the entities the
 * guest controls (its focus pids: player, or a vehicle it drives). They still exist locally. ---- */
#define SND_CALL 0x420E4
#define SND_FUNC 0x406F0
typedef void (__cdecl *fn_void)(void);
static fn_void o_sndframe;
static int g_snd_cleared_log;

/* ---- client "world ready": Xenus draws the LOADING screen instead of the HUD while byte Xenus+0xDE3928 is 0
 * (VCB_DrawHUD). It is set to 1 by pipeline message 0x80FC (VCB_PipeLine), which game.dll sends only from
 * gameLoad (a real save load) or scenaryNewFrame (New Game intro). A client runs neither, so it loaded forever.
 * Once the client has its player, send 0x80FC ourselves (repeat every 2 s until the flag is set). ---- */
typedef int (__cdecl *fn_msgout)(int, int, void *, void *);
typedef void (__cdecl *fn_kmen)(int);
static int g_world_ready_on = 1;
static void client_world_ready(void)
{
    static DWORD last; static int tries; static fn_msgout msgout; static fn_kmen kmen;
    BYTE *flag = (BYTE *)GetModuleHandleA(NULL) + 0xDE3928;
    int f; DWORD now;
    if (!g_world_ready_on || *flag) return;
    f = V.clfocus(10);
    if (f <= 0 || f >= 4096 || !V.sv_ent[f]) return;
    now = GetTickCount();
    if (last && now - last < 2000) return;
    last = now;
    if (!msgout) {
        HMODULE v = GetModuleHandleA("vital.dll");
        msgout = (fn_msgout)GetProcAddress(v, "msgSendOutside");
        if (!msgout) msgout = (fn_msgout)GetProcAddress(v, (LPCSTR)669);
        kmen = (fn_kmen)GetProcAddress(v, "kmEnable");
        if (!msgout) { logf("client world ready: msgSendOutside not found"); g_world_ready_on = 0; return; }
    }
    if (kmen) kmen(1);
    msgout(3, 0x80FC, 0, 0);
    tries++;
    logf("client world ready: sent 0x80FC (player pid %d), loading flag now %d, try %d", f, *flag, tries);
}

static void hook_streaming(HMODULE game);

/* ---- host bandwidth: svNetworkSndFrame gives each client a per-frame byte budget
 *   rate(machine+0x54) * dt  -  netGetRcvSpeed(machine) * dt     (debt at machine+0x50)
 * and skips every queued entity while the budget is < 0 (vital 0x55040A69). The guest uploads its player
 * ~120x/s, more than its default rate, so the budget was always negative: the host's 300 entities (its
 * player, NPCs, cars) stayed queued (sv_bits 05) and never reached the guest. Raise the rate. ---- */
static int g_host_rate = 400000;
static void host_rate_boost(void)
{
    int c; static int logged;
    for (c = 1; c < 31; c++) {
        BYTE *m = V.machine + (c + 1) * 0xd8c;
        if (*(int *)(m + 4) != 2) continue;
        if (*(int *)(m + 0x54) < g_host_rate) {
            if (logged++ < 4) logf("host: machine %d send rate %d -> %d bytes/s (debt %d)", c + 1, *(int *)(m + 0x54), g_host_rate, *(int *)(m + 0x50));
            *(int *)(m + 0x54) = g_host_rate;
        }
    }
}

static int g_send_filter_on = 1;
static void world_sync_tick(void);
static void __cdecl client_send_frame(void)
{
    if (vital_load() && (*V.mode == 3 || *V.mode == 4)) world_sync_tick();
    if (vital_load() && *V.mode == 3) { host_rate_boost(); o_sndframe(); return; }
    if (vital_load() && *V.mode == 4) client_world_ready();
    if (g_send_filter_on && vital_load() && *V.mode == 4) {
        int keep[4], k, p, n = 0;
        static const int slot[4] = { 10, 0, 1, 4 };
        for (k = 0; k < 4; k++) keep[k] = V.clfocus(slot[k]);
        int remote[31], c;
        /* the send loop runs once per machine (cur_wclient = column c, machine c+1). Clear every REMOTE
         * machine's column (state 2), never a local one (state 3) - the client needs its own objects. */
        /* 0.13.1: column 0 (machine 1) is the guest's own loopback to its client side. 0.10-0.13 cleared it too,
         * so nothing the guest made after joining (its weapon model, rain, ...) reached its own client side, and
         * t_wpnmodel::svChange (needs cl_ent[weapon]) refused every weapon. Only clear machines 2+ (the host). */
        for (c = 0; c < 31; c++) remote[c] = c >= 1 && *(int *)(V.machine + (c + 1) * 0xd8c + 4) == 2;
        for (p = 1; p < 4096; p++) {
            int any = 0;
            if (!V.sv_ent[p]) continue;
            if (p == keep[0] || p == keep[1] || p == keep[2] || p == keep[3]) continue;
            /* 0.14.3: always send our OWN character. When the host creates it with msg 0x73 the focus is not on it
             * yet in its first frame, the filter ate its "create" bit and the host never saw the guest. */
            if (*(DWORD *)((BYTE *)V.sv_ent[p] + 8) == 0x706c6179 && *(int *)((BYTE *)V.sv_ent[p] + 0x1a4) == 0) continue;
            for (c = 0; c < 31; c++) if (remote[c] && V.sv_bits[p * 31 + c]) { V.sv_bits[p * 31 + c] = 0; any = 1; }
            n += any;
        }
        if (n && g_snd_cleared_log < 5) {
            g_snd_cleared_log++;
            logf("client send filter: kept only pids %d/%d/%d/%d, held back %d local entities; machine states m0..m4 = %d %d %d %d %d",
                 keep[0], keep[1], keep[2], keep[3], n,
                 *(int *)(V.machine + 4), *(int *)(V.machine + 0xd8c + 4), *(int *)(V.machine + 2 * 0xd8c + 4),
                 *(int *)(V.machine + 3 * 0xd8c + 4), *(int *)(V.machine + 4 * 0xd8c + 4));
        }
    }
    o_sndframe();
}

static void hook_client_send(void)
{
    BYTE *v = (BYTE *)GetModuleHandleA("vital.dll"), *p; DWORD old;
    if (!v) return;
    p = v + SND_CALL;
    if (p[0] != 0xE8 || p + 5 + *(int *)(p + 1) != v + SND_FUNC) { logf("client send filter: bytes differ, not installed"); return; }
    o_sndframe = (fn_void)(v + SND_FUNC);
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int *)(p + 1) = (int)((BYTE *)client_send_frame - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logf("client send filter on (only the guest's own player/vehicle is sent to the host)");
}

/* ---- client: Xenus LoadGameChecked (export, +0x4EB00) runs the save-load sequence (Ordinal_611(3) ...
 * lsmLoadFile ... Ordinal_11/Ordinal_611(4)). With lsmLoadFile skipped on the client that left it on the
 * LOADING screen forever. New Game (NewGameChecked, +0x4E7E0) got in-game on the client before, so on the
 * client Load Game jumps straight to New Game. Both are cdecl, the caller pops the 2 args. ---- */
static void hook_loadgame_to_newgame(void)
{
    BYTE *x = (BYTE *)GetModuleHandleA(NULL), *lg, *ng; DWORD old;
    lg = (BYTE *)GetProcAddress((HMODULE)x, "?LoadGameChecked@@YAXPBD0@Z");
    ng = (BYTE *)GetProcAddress((HMODULE)x, "?NewGameChecked@@YAXXZ");
    if (!lg) lg = x + 0x4EB00;
    if (!ng) ng = x + 0x4E7E0;
    if (lg != x + 0x4EB00 || ng != x + 0x4E7E0) logf("note: LoadGameChecked %p NewGameChecked %p (base %p)", lg, ng, x);
    VirtualProtect(lg, 5, PAGE_EXECUTE_READWRITE, &old);
    lg[0] = 0xE9; *(int *)(lg + 1) = (int)(ng - (lg + 5));
    VirtualProtect(lg, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), lg, 5);
    logf("client: Load Game now runs New Game (the world comes from the host)");
}

static void install_hooks(void)
{
    static int bind_done, lsm_done;
    if (!lsm_done && (!_stricmp(g_mode, "host") || !_stricmp(g_mode, "join"))) {
        void **slot = find_iat(GetModuleHandleA(NULL), "vital.dll", "?lsmLoadFile@@YAHPAD@Z");
        if (slot) { o_lsmload = (fn_lsmload)patch_iat(slot, (void *)hook_lsmload); logf("hooked lsmLoadFile in Xenus.exe"); }
        else logf("lsmLoadFile import not found in Xenus.exe");
        if (!_stricmp(g_mode, "join") && GetPrivateProfileIntA("coop", "client_load_as_new", 1, g_ini))
            hook_loadgame_to_newgame();
        lsm_done = 1;
    }
    if (g_two) hook_createfile_all();
    if (!bind_done && !_stricmp(g_mode, "join")) {
        void **slot = find_iat_ord(GetModuleHandleA("vital.dll"), "WSOCK32.dll", 2);
        if (slot) {
            g_client_port = GetPrivateProfileIntA("coop", "client_port", 15012, g_ini);
            o_bind = (fn_bind)patch_iat(slot, (void *)hook_bind);
            logf("hooked bind in vital.dll");
        } else logf("bind import not found in vital.dll");
        bind_done = 1;
    }
    if (!g_hooked_cmd) {
        HMODULE game = GetModuleHandleA("game.dll");
        void **slot = find_iat(game, "vital.dll", "?stGetCmdLine@@YAXPAD@Z");
        void **slot2 = find_iat(game, "vital.dll", "?stStartGame@@YAHXZ");
        if (slot && slot2) {
            o_getcmd = (fn_getcmd)patch_iat(slot, (void *)hook_getcmd);
            o_startgame = (fn_startgame)patch_iat(slot2, (void *)hook_startgame);
            g_hooked_cmd = 1;
            logf("hooked stGetCmdLine and stStartGame in game.dll");
            if (GetPrivateProfileIntA("coop", "stream_all_players", 1, g_ini)) hook_streaming(game);
            if (!_stricmp(g_mode, "host")) {
                hook_attach_install();
                g_host_rate = GetPrivateProfileIntA("coop", "host_send_rate", 400000, g_ini);
                if (g_host_rate > 0) hook_client_send();   /* same svNewFrame hook; host path raises send rates */
            }
            if (!_stricmp(g_mode, "join") && GetPrivateProfileIntA("coop", "client_block_creates", 0, g_ini))
                hook_client_creates(game);
            if (!_stricmp(g_mode, "join")) {
                g_send_filter_on = GetPrivateProfileIntA("coop", "client_send_filter", 1, g_ini);
                g_world_ready_on = GetPrivateProfileIntA("coop", "client_world_ready", 1, g_ini);
                hook_client_send();
            }
        } else {
            logf("game.dll %p: stGetCmdLine import not found yet", (void *)game);
        }
    }
    if (g_two && !g_hooked_mutex) {
        void **slot = find_iat(GetModuleHandleA(NULL), "kernel32.dll", "CreateMutexA");
        if (slot) {
            o_mutex = (fn_mutex)patch_iat(slot, (void *)hook_mutex);
            g_hooked_mutex = 1;
            logf("hooked CreateMutexA in Xenus.exe");
        } else {
            logf("CreateMutexA import not found in Xenus.exe");
        }
    }
}

/* ---- engine console capture: ConPrintf / DebugPrintf in vital.dll are redirected into our log ---- */
static void __cdecl my_conprintf(char *fmt, ...)
{
    char b[1024]; va_list ap; size_t n;
    va_start(ap, fmt); _vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap); b[sizeof b - 1] = 0;
    n = strlen(b); while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    logf("con: %s", b);
}

static void jmp_hook(void *at, void *to)
{
    DWORD old; BYTE *p = (BYTE *)at;
    if (!p) return;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xE9; *(DWORD *)(p + 1) = (DWORD)((BYTE *)to - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

static void hook_console(void)
{
    HMODULE v = GetModuleHandleA("vital.dll");
    if (!v) { logf("console capture: vital.dll not loaded"); return; }
    jmp_hook(GetProcAddress(v, "?ConPrintf@@YAXPADZZ"), (void *)my_conprintf);
    jmp_hook(GetProcAddress(v, "?DebugPrintf@@YAXPADZZ"), (void *)my_conprintf);
    logf("console capture on");
}

/* ---- net guard: inside clNetworkFrame (vital 0x5503b6a0), right after an entity message's 3 flag bits
 * are read. A pid outside cl_ent[4096], or an update for an entity we do not have, means the bit stream
 * lost sync (an earlier message read a different number of bits than the server wrote). We log the
 * messages of that packet and skip the rest of it instead of crashing. ---- */
#define NG_SITE  0x3B962   /* add esp,0xc ; cmp byte [esp+0xf],5 */
#define NG_CONT  0x3B96A
#define NG_DROP  0x3BCAC   /* receive next packet */
typedef int (__cdecl *fn_bplen)(void *);
static DWORD g_ng_cont, g_ng_drop;
static void ***g_cl_ent;
static fn_bplen g_bplen;
static struct { int pid, flags, bit; DWORD type; } g_ng[64];
static int g_ng_n, g_ng_last, g_ng_drops, g_ng_msgs;

static DWORD ent_type(int pid)
{
    void **e = (pid >= 0 && pid < 4096) ? g_cl_ent[pid] : NULL;
    return (e && !IsBadReadPtr(e, 12)) ? *(DWORD *)((BYTE *)e + 8) : 0;
}

static int ent_owner(int pid)   /* machine that created a network entity (t_entity +0x1a4), -1 if none */
{
    void **e = (pid >= 0 && pid < 4096) ? g_cl_ent[pid] : NULL;
    return (e && !IsBadReadPtr((BYTE *)e + 0x1a4, 4)) ? *(int *)((BYTE *)e + 0x1a4) : -1;
}

static int *g_stmode;

static int is_play(void *e) { return e && !IsBadReadPtr(e, 12) && *(DWORD *)((BYTE *)e + 8) == 0x706c6179; }

/* host, main thread, every 2 s: if a guest's character exists under a different pid than the one the
 * server gave it (player table +0x100), point that guest's camera/controls at the real one. */
static void guest_focus_fix(void)
{
    static DWORD last; DWORD now = GetTickCount(); int m, p;
    if (now - last < 2000) return;
    last = now;
    if (!vital_load() || *V.mode != 3) return;
    for (m = 2; m < 31; m++) {
        short *ip; int cur, found = 0;
        if (*(int *)(V.machine + m * 0xd8c + 4) != 2) continue;
        ip = (short *)(player_info(m) + 0x100); cur = *ip;
        if (cur > 0 && cur < 4096 && (is_play(g_cl_ent[cur]) || is_play(V.sv_ent[cur]))) continue;
        for (p = 1; p < 4096 && !found; p++)
            if (is_play(g_cl_ent[p]) && ent_owner(p) == m) found = p;
        if (!found) continue;
        logf("guest focus fix: %s (machine %d) has pid %d, server had %d; re-pointing camera and controls",
             (char *)player_info(m) + 1, m, found, cur);
        *ip = (short)found;
        V.svfocus((unsigned short)m, 4, found); V.svfocus((unsigned short)m, 1, found);
        V.svfocus((unsigned short)m, 0, found); V.svfocus((unsigned short)m, 10, found);
    }
}

static int __cdecl net_check(BYTE *pkt, int pid, int flags, int from)
{
    int bit = g_bplen(pkt + 0x408), end = g_bplen(pkt + 0x414), bad, i, foreign = 0;
    if (g_stmode && *g_stmode == 3) guest_focus_fix();
    if (g_bring_guests && InterlockedExchange(&g_bring_guests, 0)) { if (g_inited15 && g_admin_on) admin_push("bring all"); else bring_guests(); }   /* F8 on the host */
    if (bit < g_ng_last) g_ng_n = 0;             /* new packet */
    g_ng_last = bit;
    if (g_ng_n < 64) { g_ng[g_ng_n].pid = pid; g_ng[g_ng_n].flags = flags; g_ng[g_ng_n].bit = bit; g_ng[g_ng_n].type = ent_type(pid); g_ng_n++; }
    g_ng_msgs++;
    if (g_stmode && *g_stmode == 4) {   /* client diagnostics: what the server sends us */
        static int creates, logged, upd, per_from[32]; static DWORD last;
        DWORD now = GetTickCount();
        if (from >= 0 && from < 32) per_from[from]++;
        if (flags & 1) {
            creates++;
            if (logged++ < 40) {
                DWORD t = pid < 4096 && g_cl_ent[pid] ? ent_type(pid) : 0;
                logf("client: create from machine %d for pid %d (slot %s %.4s)", from, pid, t ? "TAKEN by" : "free", t ? (char *)&t : "");
            }
        } else if (flags & 4) upd++;
        if (now - last > 5000) {
            last = now;
            logf("client: entity msgs so far: creates %d, updates %d, from m0 %d m1 %d m2 %d m3 %d", creates, upd,
                 per_from[0], per_from[1], per_from[2], per_from[3]);
        }
    }
    bad = pid >= 4096 || (!(flags & 1) && (flags & 4) && !g_cl_ent[pid]);
    /* host: a guest (machine >= 2) may only update entities it created itself. Its other entity numbers
     * come from its own world and mean different things here (e.g. pid 101 = the host's player);
     * reading them with the wrong layout desyncs the packet. An update has no length, so the rest of
     * the packet has to be skipped. */
    if (!bad && g_stmode && *g_stmode == 3 && from >= 2 && from < 31 && (flags & 4) && g_cl_ent[pid] &&
        ent_owner(pid) != from) { bad = 1; foreign = 1; }
    if (!bad) return 0;
    g_ng_drops++;
    if (g_ng_drops <= 30) {
        char line[4096], one[80]; DWORD t;
        sprintf(line, "netguard: drop #%d (msgs so far %d) from machine %d, pid=%d flags=%d owner=%d at bit %d/%d%s. packet:",
                g_ng_drops, g_ng_msgs, from, pid, flags, pid < 4096 ? ent_owner(pid) : -1, bit, end,
                foreign ? " (guest updated an entity it does not own)" : "");
        for (i = 0; i < g_ng_n && strlen(line) < sizeof line - 80; i++) {
            t = g_ng[i].type;
            sprintf(one, " [%d f%d @%d %.4s o%d]", g_ng[i].pid, g_ng[i].flags, g_ng[i].bit, t ? (char *)&t : "----",
                    ent_owner(g_ng[i].pid));
            strcat(line, one);
        }
        logf("%s", line);
    } else if (g_ng_drops % 100 == 0) logf("netguard: %d drops", g_ng_drops);
    g_ng_n = 0; g_ng_last = 0;
    return 1;
}

__declspec(naked) static void net_stub(void)
{
    __asm {
        add esp, 0x0c
        pushad
        pushfd
        movzx eax, byte ptr [esp + 36 + 0x0f]
        mov ecx, dword ptr [esp + 36 + 0x14]
        push ecx
        push eax
        push ebx
        lea eax, [esp + 12 + 36 + 0x20]
        push eax
        call net_check
        add esp, 16
        test eax, eax
        jnz drop
        popfd
        popad
        cmp byte ptr [esp + 0x0f], 5
        jmp dword ptr [g_ng_cont]
    drop:
        popfd
        popad
        jmp dword ptr [g_ng_drop]
    }
}

static void hook_netguard(void)
{
    static const BYTE want[] = { 0x83,0xC4,0x0C,0x80,0x7C,0x24,0x0F,0x05 };
    BYTE *v = (BYTE *)GetModuleHandleA("vital.dll"), *p; DWORD old;
    if (!v) { logf("netguard: vital.dll not loaded"); return; }
    p = v + NG_SITE;
    if (memcmp(p, want, sizeof want)) { logf("netguard: bytes differ, not installed"); return; }
    g_cl_ent = (void ***)GetProcAddress((HMODULE)v, "?cl_ent@@3PAPAVt_entity@@A");
    g_bplen = (fn_bplen)GetProcAddress((HMODULE)v, "?bpGetLen@@YAHPAUt_bitptr@@@Z");
    g_stmode = (int *)GetProcAddress((HMODULE)v, "?stMode@@3HA");
    if (!g_cl_ent || !g_bplen) { logf("netguard: exports missing"); return; }
    g_ng_cont = (DWORD)(v + NG_CONT); g_ng_drop = (DWORD)(v + NG_DROP);
    jmp_hook(p, (void *)net_stub);
    VirtualProtect(p + 5, 3, PAGE_EXECUTE_READWRITE, &old); memset(p + 5, 0x90, 3); VirtualProtect(p + 5, 3, old, &old);
    logf("netguard on");
}

/* ==== 0.13 world sync ====================================================================================
 * 1) game clock: game.dll keeps the time of day in four floats (gtimeLoad order): +0x7F5728 elapsed (x10 =
 *    game seconds), +0x19102C, +0x191028, +0x191030 (start time). Xenus DrawSky picks sky/rain from the time and
 *    the player's area, so copying the host's clock to the guest also lines up day/night and weather.
 *    The clock goes over a small side channel (UDP sync_port, default 15013): the guest says hello every
 *    second, the host answers each guest twice a second.
 * 2) duplicates: the guest loads the map itself (local pids), and the host's NPCs/cars arrive as well, so the
 *    guest saw everything twice. The guest removes its own networked copies of the types the host sends.
 * 3) weapon in hand: t_player::CreateWeapon (game +0x6C430) makes the 'wpnm' entity (local pid, never sent).
 *    The host only learns the guest's weapon through the guest player's net table, so a guest player without
 *    its own wpnm on the host shows no gun. The host creates one for it. Logged to see what each side has. */
#pragma comment(lib, "ws2_32.lib")
#define GT_ELAPSED 0x7F5728
#define GT_A       0x19102C
#define GT_B       0x191028
#define GT_START   0x191030
#define SYNC_MAGIC 0x31535042   /* "BPS1" */
#pragma pack(push, 1)
struct sync_pkt { DWORD magic, kind, seq; DWORD gt[4]; DWORD wx[6]; };
#pragma pack(pop)
/* Xenus DrawSky rolls the weather with rand() once per game hour, so a shared clock is not enough:
 * +0x25941B4 current sky, +0x25941B8 next, +0x25941C8 hour already rolled, +0x25941B0 (word) counter,
 * +0x25941BC/CC/CD/CE flags (CC = raining; game.dll rainNewFrame reads it through pipeline 0x807F). */
#define XW_BASE 0x25941B0
static SOCKET g_sync = INVALID_SOCKET;
static int g_sync_weather = 1, g_click_map = 1;
static volatile int g_dead_report_pid;   /* host: a guest reported this player pid dead */
static int g_sync_on = -1, g_sync_port = 15013, g_dedupe_on = 1, g_dedupe_veh = 0, g_weapon_fix = 1;
static struct sockaddr_in g_sync_host, g_guest_addr[8];
static DWORD g_guest_seen[8];
typedef void (__fastcall *fn_this0)(void *self, void *edx);
typedef void (__cdecl *fn_enremove)(void *e);
static fn_this0 g_createweapon;
static fn_enremove g_enremove;

static void sync_open(int host)
{
    WSADATA wd; struct sockaddr_in a; u_long nb = 1;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) { logf("sync: WSAStartup failed"); g_sync_on = 0; return; }
    g_sync = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sync == INVALID_SOCKET) { logf("sync: socket failed %d", WSAGetLastError()); g_sync_on = 0; return; }
    memset(&a, 0, sizeof a); a.sin_family = AF_INET;
    a.sin_port = htons((u_short)(host ? g_sync_port : 0));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sync, (struct sockaddr *)&a, sizeof a)) { logf("sync: bind port %d failed %d", host ? g_sync_port : 0, WSAGetLastError()); closesocket(g_sync); g_sync = INVALID_SOCKET; g_sync_on = 0; return; }
    ioctlsocket(g_sync, FIONBIO, &nb);
    if (!host) {
        memset(&g_sync_host, 0, sizeof g_sync_host); g_sync_host.sin_family = AF_INET;
        g_sync_host.sin_port = htons((u_short)g_sync_port);
        g_sync_host.sin_addr.s_addr = inet_addr(g_ip[0] ? g_ip : "127.0.0.1");
    }
    logf("sync: side channel open (%s, UDP %d)", host ? "host listens" : "guest talks to host", g_sync_port);
}

static void ext_recv(int host, BYTE *b, int n);
static void sync_host_tick(BYTE *g)
{
    static DWORD last, seq; struct sync_pkt pk; struct sockaddr_in from; int fl = sizeof from, n, i;
    DWORD now = GetTickCount();
    static BYTE rb[1600];
    while ((n = recvfrom(g_sync, (char *)rb, sizeof rb, 0, (struct sockaddr *)&from, &fl)) >= 12) {
        fl = sizeof from;
        if (((struct sync_pkt *)rb)->kind >= 4) { ext_recv(1, rb, n); continue; }
        memset(&pk, 0, sizeof pk); memcpy(&pk, rb, n < (int)sizeof pk ? n : (int)sizeof pk);
        if (pk.magic == SYNC_MAGIC && pk.kind == 3) {
            if (g_dead_report_pid != (int)pk.seq) logf("sync: guest says its player %lu died", pk.seq);
            g_dead_report_pid = (int)pk.seq;
            continue;
        }
        if (pk.magic != SYNC_MAGIC || pk.kind != 1) continue;
        for (i = 0; i < 8; i++) if (g_guest_seen[i] && g_guest_addr[i].sin_addr.s_addr == from.sin_addr.s_addr && g_guest_addr[i].sin_port == from.sin_port) break;
        if (i == 8) {
            for (i = 0; i < 8; i++) if (!g_guest_seen[i] || now - g_guest_seen[i] > 15000) break;
            if (i == 8) continue;
            g_guest_addr[i] = from;
            logf("sync: guest %s:%d joined the side channel", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
        }
        g_guest_seen[i] = now;
    }
    if (now - last < 500) return;
    last = now;
    pk.magic = SYNC_MAGIC; pk.kind = 2; pk.seq = ++seq;
    pk.gt[0] = *(DWORD *)(g + GT_ELAPSED); pk.gt[1] = *(DWORD *)(g + GT_A);
    pk.gt[2] = *(DWORD *)(g + GT_B);       pk.gt[3] = *(DWORD *)(g + GT_START);
    {   BYTE *x = (BYTE *)GetModuleHandleA(NULL) + XW_BASE;
        pk.wx[0] = *(DWORD *)(x + 0x00); pk.wx[1] = *(DWORD *)(x + 0x04); pk.wx[2] = *(DWORD *)(x + 0x08);
        pk.wx[3] = *(DWORD *)(x + 0x0C); pk.wx[4] = *(DWORD *)(x + 0x18); pk.wx[5] = *(DWORD *)(x + 0x1C); }
    for (i = 0; i < 8; i++)
        if (g_guest_seen[i] && now - g_guest_seen[i] < 15000)
            sendto(g_sync, (char *)&pk, sizeof pk, 0, (struct sockaddr *)&g_guest_addr[i], sizeof g_guest_addr[i]);
}

static void sync_guest_tick(BYTE *g, int ready)
{
    static DWORD last, got; struct sync_pkt pk; struct sockaddr_in from; int fl = sizeof from, n;
    DWORD now = GetTickCount();
    if (now - last >= 1000) {
        struct sync_pkt h; last = now;
        h.magic = SYNC_MAGIC; h.kind = 1; h.seq = 0; memset(h.gt, 0, sizeof h.gt);
        sendto(g_sync, (char *)&h, sizeof h, 0, (struct sockaddr *)&g_sync_host, sizeof g_sync_host);
    }
    static BYTE rb[1600];
    while ((n = recvfrom(g_sync, (char *)rb, sizeof rb, 0, (struct sockaddr *)&from, &fl)) >= 12) {
        fl = sizeof from;
        if (((struct sync_pkt *)rb)->kind >= 4) { ext_recv(0, rb, n); continue; }
        if (n < (int)sizeof pk) continue;
        memcpy(&pk, rb, sizeof pk);
        if (pk.magic != SYNC_MAGIC || pk.kind != 2 || !ready) continue;
        if (got++ % 120 == 0) {
            float he = *(float *)&pk.gt[0], hs = *(float *)&pk.gt[3], me = *(float *)(g + GT_ELAPSED), ms = *(float *)(g + GT_START);
            logf("sync: clock from host: game seconds %.0f (ours was %.0f), packet %lu", he * 10 + hs, me * 10 + ms, pk.seq);
        }
        *(DWORD *)(g + GT_ELAPSED) = pk.gt[0]; *(DWORD *)(g + GT_A) = pk.gt[1];
        *(DWORD *)(g + GT_B) = pk.gt[2];       *(DWORD *)(g + GT_START) = pk.gt[3];
        if (g_sync_weather) {
            BYTE *x = (BYTE *)GetModuleHandleA(NULL) + XW_BASE;
            static DWORD lastsky = 0xFFFFFFFF, lastrain = 0xFFFFFFFF;
            *(WORD *)(x + 0x00) = (WORD)pk.wx[0];               /* counter (word only) */
            *(DWORD *)(x + 0x04) = pk.wx[1]; *(DWORD *)(x + 0x08) = pk.wx[2];
            x[0x0C] = (BYTE)pk.wx[3];                            /* +0x25941BC flag */
            *(DWORD *)(x + 0x18) = pk.wx[4];                     /* hour already rolled */
            x[0x1C] = (BYTE)pk.wx[5]; x[0x1D] = (BYTE)(pk.wx[5] >> 8); x[0x1E] = (BYTE)(pk.wx[5] >> 16);   /* CC CD CE */
            if (pk.wx[1] != lastsky || (pk.wx[5] & 0xFF) != lastrain) {
                lastsky = pk.wx[1]; lastrain = pk.wx[5] & 0xFF;
                logf("sync: weather from host: sky %lu (next %lu), rain %lu", pk.wx[1], pk.wx[2], lastrain);
            }
        }
    }
}

typedef int (__cdecl *fn_ischild)(int, int);
static fn_ischild g_ischild; static int g_ischild_tried;
static int dedupe_type(DWORD t)
{
    /* 'andr' is a DOOR (ParseReliable 0x9E), not an android: 0.13 removed the guest's doors. Never remove them. */
    static const DWORD actors[] = { 'enpc','mhtm','robo','mdog','mcow','mjgr','mare','mlbd','body','bird','fish','bfly','parr','esnk' };
    static const DWORD vehicles[] = { 'ecar','ehel','epln','eshp','etnk','ntnk','ecrt','taxi' };
    int i;
    for (i = 0; i < (int)(sizeof actors / sizeof *actors); i++) if (t == actors[i]) return 1;
    /* 0.15.1: any other walking unit (NPC kinds not in the list above) too; 'wunt' = t_walkunit */
    if (!g_ischild_tried) { g_ischild_tried = 1; g_ischild = (fn_ischild)GetProcAddress(GetModuleHandleA("vital.dll"), "?pwIsTypeChildOf@@YAHHH@Z"); }
    if (g_ischild && t != 0x706C6179 && g_ischild((int)t, 0x77756E74)) return 1;
    if (g_dedupe_veh) for (i = 0; i < (int)(sizeof vehicles / sizeof *vehicles); i++) if (t == vehicles[i]) return 1;
    return 0;
}

static void tname(DWORD t, char *o) { o[0] = (char)(t >> 24); o[1] = (char)(t >> 16); o[2] = (char)(t >> 8); o[3] = (char)t; o[4] = 0; }

/* guest, every 2 s once in-game: remove our own networked copies of NPCs/animals/bodies/vehicles when the host
 * sends that type too (so we only see the host's ones, which the host moves and runs) */
static void guest_dedupe(DWORD since_ready)
{
    static DWORD srv_types[64]; int nsrv = 0, p, k, removed = 0, keep[4];
    static const int fs[4] = { 10, 0, 1, 4 };
    static int total, logged;
    static DWORD rtype[32]; static int rcount[32]; int nr = 0;
    if (!g_dedupe_on || since_ready < 10000 || !g_enremove) return;
    for (k = 0; k < 4; k++) keep[k] = V.clfocus(fs[k]);
    for (p = 1; p <= 0xC00 && nsrv < 64; p++) {          /* types the host has sent us */
        BYTE *e = (BYTE *)g_cl_ent[p]; DWORD t;
        if (!e || V.sv_ent[p] || IsBadReadPtr(e, 12)) continue;
        t = *(DWORD *)(e + 8);
        if (!dedupe_type(t)) continue;
        for (k = 0; k < nsrv; k++) if (srv_types[k] == t) break;
        if (k == nsrv) srv_types[nsrv++] = t;
    }
    if (!nsrv) return;
    for (p = 1; p < 4096 && removed < 40; p++) {
        BYTE *e = (BYTE *)V.sv_ent[p]; DWORD t;
        if (!e || IsBadReadPtr(e, 0x1a8)) continue;
        if (p == keep[0] || p == keep[1] || p == keep[2] || p == keep[3]) continue;
        t = *(DWORD *)(e + 8);
        if (!dedupe_type(t) || !(e[0x39] & 0x80) || (e[0x3A] & 0x80)) continue;
        for (k = 0; k < nsrv; k++) if (srv_types[k] == t) break;
        if (k == nsrv) continue;
        g_enremove(e);
        removed++;
        for (k = 0; k < nr; k++) if (rtype[k] == t) break;
        if (k == nr && nr < 32) { rtype[nr] = t; rcount[nr++] = 0; }
        if (k < 32) rcount[k]++;
    }
    total += removed;
    if (removed && logged++ < 12) {
        char line[512], nm[5]; int o = 0;
        for (k = 0; k < nr; k++) { tname(rtype[k], nm); o += _snprintf(line + o, sizeof line - o - 1, " %s:%d", nm, rcount[k]); }
        line[o] = 0;
        logf("guest: removed %d own copies of host-run things (total %d):%s", removed, total, line);
    }
}

/* both sides, every 2 s: what weapon each player has; on the host also give a guest's player a wpnm */
/* 0.14.5 host: NPC AI picks targets from the side tables (sideinfo, game +0x4C8290: 16 sides x 0x290 bytes,
 * sorted DWORD pids, count at +0x200; sideGetEnemyUnit reads them). t_player::AdjustNewParams puts a player in
 * with sideAdd(pid, byte +0xAFC), but the guest's player made from net message 0x73 never got there, so NPCs
 * only ever saw the host. Put every guest player on the host player's side. */
typedef void (__cdecl *fn_sideadd)(short pid, int side);
static fn_sideadd g_sideadd;
static int g_side_on = -1, g_host_side = -1, g_side_logs;
static int side_of(BYTE *g, int pid)
{
    int s, i;
    for (s = 0; s < 16; s++) {
        int *list = (int *)(g + 0x4C8290 + s * 0x290), n = list[0x80];
        if (n < 0 || n > 0x80) continue;
        for (i = 0; i < n; i++) if (list[i] == pid) return s;
    }
    return -1;
}
static void side_check(int p, BYTE *sv)
{
    BYTE *g = (BYTE *)GetModuleHandleA("game.dll"); int owner, cur, want;
    if (!g) return;
    if (g_side_on < 0) {
        g_side_on = GetPrivateProfileIntA("coop", "npcs_target_guest", 1, g_ini);
        g_sideadd = (fn_sideadd)GetProcAddress((HMODULE)g, "?sideAdd@@YAXFH@Z");
        if (!g_sideadd && g[0xC5BA0] == 0x53 && g[0xC5BA1] == 0x8B) g_sideadd = (fn_sideadd)(g + 0xC5BA0);
        logf("host: npcs_target_guest=%d sideAdd %p", g_side_on, (void *)g_sideadd);
    }
    if (!g_side_on || !g_sideadd || !is_play(sv)) return;
    owner = *(int *)(sv + 0x1a4);
    cur = side_of(g, p);
    if (owner == 0) {
        if (cur >= 0 && cur != g_host_side) { g_host_side = cur; logf("host: our player %d is on side %d (byte %d)", p, cur, sv[0xAFC]); }
        return;
    }
    if (cur >= 0) return;
    want = g_host_side >= 0 ? g_host_side : (g_side_logs >= 5 && sv[0xAFC] < 16 ? sv[0xAFC] : -1);
    if (want < 0) { if (g_side_logs++ < 5) logf("host: guest player %d has no side yet (host side unknown)", p); return; }
    g_sideadd((short)p, want);
    logf("host: guest player %d added to side %d (own byte %d) -> now side %d, NPCs can target it", p, want, sv[0xAFC], side_of(g, p));
}

static void weapon_check(int host)
{
    static DWORD last; static int logged, fixes;
    DWORD now = GetTickCount(); int p;
    if (now - last < 2000) return;
    last = now;
    for (p = 1; p < 4096; p++) {
        BYTE *sv = (BYTE *)V.sv_ent[p], *cl = (BYTE *)g_cl_ent[p], *w = NULL, *cw = NULL;
        char nm[5] = "-";
        if (!is_play(sv) && !is_play(cl)) continue;
        if (sv && (!is_play(sv) || IsBadReadPtr(sv, 0x1A2E4))) sv = NULL;
        if (cl && (!is_play(cl) || IsBadReadPtr(cl, 0x1A2E4))) cl = NULL;
        if (!sv && !cl) continue;
        if (sv) w = *(BYTE **)(sv + 0x1A098);
        if (cl) cw = *(BYTE **)(cl + 0x1A098);
        if (w && !IsBadReadPtr(w, 12)) tname(*(DWORD *)(w + 8), nm);
        if (logged < 40 && (logged < 6 || now % 10000 < 2000)) {
            logged++;
            logf("weapon: player %d sv=%p cl=%p%s owner=%d | sv weapon %p (%s, pid %d) cl weapon %p | kind %d code %d",
                 p, sv, cl, sv == cl ? " (same)" : "", sv ? *(int *)(sv + 0x1a4) : -1, w, nm,
                 sv ? *(short *)(sv + 0x1A1D6) : -1, cw, (sv ? sv : cl)[0x100E4], *(short *)((sv ? sv : cl) + 0x100B8));
        }
        if (host && sv) side_check(p, sv);
        if (host && g_weapon_fix && g_createweapon && sv && is_play(sv) && *(int *)(sv + 0x1a4) != 0 && !w && fixes < 20) {
            fixes++;
            g_createweapon(sv, NULL);
            logf("weapon: host made a weapon model for guest player %d -> %p (pid %d)", p, *(void **)(sv + 0x1A098), *(short *)(sv + 0x1A1D6));
        }
    }
}

/* ---- guest "use" on doors/buttons. StateManual calls doSendPlayerClick(target pid, a, b) (game +0x8700, call
 * sites +0x73D98 door, +0x73DEF button). In stMode 4 that only sends msg 0x9E to the host, whose ParseReliable
 * opens sv_ent[pid] - but the guest's doors have LOCAL pids, so nothing (or a random host entity) was clicked,
 * and the guest's own door never opened. Now: open the guest's own door like ParseReliable does
 * (scenaryClickOnDoor(e+0x8EC) -> vcall +0x8C(2,a,b); buttons scenaryClickOnButton(e+0x8EC, pid) -> vcall
 * +0x90(2,a)), and send the host the pid of ITS matching door (same type, same place). ---- */
typedef void (__cdecl *fn_click)(int pid, int a, int b);
typedef int (__cdecl *fn_cdoor)(int id);
typedef int (__cdecl *fn_cbtn)(int id, int pid);
typedef void (__fastcall *fn_this3)(void *self, void *edx, int x, int y, int z);
typedef void (__fastcall *fn_this2)(void *self, void *edx, int x, int y);
static fn_click o_click;
static fn_cdoor g_cdoor;
static fn_cbtn g_cbtn;
static int patch_call(BYTE *at, BYTE *expect_target, void *to);

static int match_host_entity(BYTE *e)
{
    DWORD t = *(DWORD *)(e + 8); float *o = (float *)(e + 0x20), best = 300.0f * 300.0f; int p, hit = 0;
    for (p = 1; p <= 0xC00; p++) {
        BYTE *c = (BYTE *)g_cl_ent[p]; float dx, dy, dz, d;
        if (!c || V.sv_ent[p] || IsBadReadPtr(c, 0x2c) || *(DWORD *)(c + 8) != t) continue;
        dx = ((float *)(c + 0x20))[0] - o[0]; dy = ((float *)(c + 0x20))[1] - o[1]; dz = ((float *)(c + 0x20))[2] - o[2];
        d = dx * dx + dy * dy + dz * dz;
        if (d < best) { best = d; hit = p; }
    }
    return hit;
}

static DWORD g_door_clicked[4096];
static short g_door_host[4096];      /* guest door pid -> host door pid (0 unknown, -1 none) */
static BYTE g_door_flips[4096];

/* 0.14.4 guest, every 0.5 s: a door the HOST opened only opened the host's copy here; the guest's own copy
 * (same place, the one that blocks the guest) stayed shut. Put our nearby doors into the host door's state
 * (open flag t_animdoor +0x97C bit 4, sent in its net table). Gives up on a door after 3 tries. */
static void guest_door_mirror(void)
{
    static DWORD last; static int logged; DWORD now = GetTickCount(); int p, f10, n = 0; BYTE *me; float *mp;
    if (now - last < 500) return;
    last = now;
    f10 = V.clfocus(10);
    me = (f10 > 0 && f10 < 4096) ? (BYTE *)V.sv_ent[f10] : NULL;
    if (!me || IsBadReadPtr(me, 0x2c)) return;
    mp = (float *)(me + 0x20);
    for (p = 1; p < 4096 && n < 64; p++) {
        BYTE *e = (BYTE *)V.sv_ent[p], *h; float dx, dz; int hp;
        if (!e || IsBadReadPtr(e, 0x980) || *(DWORD *)(e + 8) != 'andr') continue;
        dx = ((float *)(e + 0x20))[0] - mp[0]; dz = ((float *)(e + 0x20))[2] - mp[2];
        if (dx * dx + dz * dz > 4000.0f * 4000.0f) continue;
        n++;
        if (g_door_host[p] < 0 || g_door_flips[p] >= 3) continue;
        hp = g_door_host[p];
        if (!hp || !g_cl_ent[hp] || *(DWORD *)((BYTE *)g_cl_ent[hp] + 8) != 'andr') {
            hp = match_host_entity(e);
            g_door_host[p] = (short)(hp ? hp : -1);
            if (!hp) continue;
        }
        h = (BYTE *)g_cl_ent[hp];
        if (!h || IsBadReadPtr(h, 0x980)) continue;
        if (((h[0x97C] ^ e[0x97C]) & 4) == 0) { g_door_flips[p] = 0; continue; }
        if (now - g_door_clicked[p] < 3000) continue;       /* our own click is still on its way */
        ((fn_this3)(*(void ***)e)[0x8C / 4])(e, NULL, 2, 1, 0);
        g_door_clicked[p] = now;
        g_door_flips[p]++;
        if (logged++ < 20) logf("guest: door %d follows the host's door %d (now %s)", p, hp, (e[0x97C] & 4) ? "flag set" : "flag clear");
    }
}

static void __cdecl my_click(int pid32, int a, int b)
{
    int pid = (short)pid32; static int logged;
    BYTE *e = (pid > 0 && pid < 4096) ? (BYTE *)V.sv_ent[pid] : NULL;
    if (*V.mode != 4 || !e || IsBadReadPtr(e, 0x8F0)) { o_click(pid32, a, b); return; }
    {   /* 0.14.4: one key press sends the click ~4 times (0.2 s apart) and t_animdoor::svSignal TOGGLES the
         * door (xor +0x97C,4), so it ended closed again here and on the host. One click per door per 1.5 s. */
        static int last_pid; static DWORD last_t; DWORD now = GetTickCount();
        if (pid == last_pid && now - last_t < 1500) return;
        last_pid = pid; last_t = now;
        g_door_clicked[pid] = now;
    }
    {
        DWORD t = *(DWORD *)(e + 8); int host_pid = match_host_entity(e), opened = 0; char nm[5];
        if (t == 'andr' && g_cdoor && g_cdoor(*(short *)(e + 0x8EC))) {
            ((fn_this3)(*(void ***)e)[0x8C / 4])(e, NULL, 2, a, b); opened = 1;
        } else if (t == 'abtn' && g_cbtn && g_cbtn(*(short *)(e + 0x8EC), *(short *)(e + 0x14))) {
            ((fn_this2)(*(void ***)e)[0x90 / 4])(e, NULL, 2, a); opened = 1;
        }
        if (host_pid) o_click(host_pid, a, b);
        tname(t, nm);
        if (logged++ < 30) logf("guest: used %s pid %d (opened here: %d) -> host's pid %d%s", nm, pid, opened, host_pid, host_pid ? "" : " (no match, host not told)");
    }
}

/* ==== 0.14 ================================================================================================
 * a) incoming game messages (gameParseReliable +0x15000 -> ParseReliable +0x18810, msg id in EAX): logged for
 *    0x73 respawn, 0x83 bullet hit, 0x99 vehicle use, 0x9E door click, to see what really arrives.
 * b) bullets: bmSendShotObject (+0xA61D0) reports a hit to the host (msg 0x83, only that in stMode 4) - but
 *    only if sv_ent[hit pid] exists (+0xA47DB, +0xA6191). The host's NPCs exist only in the guest's cl_ent, so
 *    no hit was ever reported. Guest: those two checks look at cl_ent, and a hit on one of the guest's own
 *    objects is translated to the host's matching object (or dropped).
 * c) vehicles: VehicleUse (+0xCD8C0) sends msg 0x99 only to the host in stMode 4, so the guest's own cars
 *    never reacted. For a guest-local vehicle the call runs as stMode 1 (machine 1 = the guest itself).
 * d) respawn: nothing respawns a dead player in this mode. The host watches every player (host: focus 10,
 *    guests: player table +0x100, or a "dead" note from the guest over the side channel) and 8 s after a death
 *    creates a new player next to another living player, like gamecodeNewFrame's MP respawn: doCreatePlayer
 *    (+0x8230, pid in AX) for the host, msg 0x73 for a guest, then svSetFocus 4/1/0/10. ---- */
static BYTE *g_parse_rel, *g_shotobj, *g_vehuse_tramp;
static int g_respawn_on = 1;
#define T_DCAM 0x6463616D

static int __cdecl my_gpr(void *pkt, int msg, int from)
{
    static int cnt[4]; int r, k = msg == 0x73 ? 0 : msg == 0x83 ? 1 : msg == 0x99 ? 2 : msg == 0x9E ? 3 : -1;
    BYTE *fn = g_parse_rel;
    if (k >= 0 && cnt[k]++ < 15) logf("net in: game message 0x%X (arg %d)", msg, from);
    __asm {
        mov eax, msg
        push from
        push pkt
        call fn
        add esp, 8
        mov r, eax
    }
    return r;
}

static void __cdecl my_shotobj(int pid32, void *bullet, void *hp)
{
    static int logged; int pid = (short)pid32, orig = pid; char nm[5] = "-";
    BYTE *fn = g_shotobj;
    if (*V.mode == 4 && pid > 0 && pid < 4096) {
        BYTE *c = (BYTE *)g_cl_ent[pid];
        if (V.sv_ent[pid]) {                        /* one of the guest's own objects */
            pid = match_host_entity((BYTE *)V.sv_ent[pid]);
            if (!pid) { if (logged++ < 30) logf("guest: hit own object %d, no host match, dropped", orig); return; }
            c = (BYTE *)g_cl_ent[pid];
        }
        if (c && !IsBadReadPtr(c, 12)) tname(*(DWORD *)(c + 8), nm);
        {   /* the hit message carries the shooter (bullet +2). Make sure it is our player, a pid the host knows */
            short *shooter = (short *)((BYTE *)bullet + 2); int me = V.clfocus(10), was = -1;
            if (bullet && !IsBadWritePtr(shooter, 2)) {
                was = *shooter;
                if (me > 0 && me <= 0xC00 && (was <= 0 || was > 0xC00 || !is_play(V.sv_ent[was]))) *shooter = (short)me;
            }
            if (logged++ < 30) logf("guest: bullet hit %s pid %d (was %d) -> host, shooter %d (was %d)", nm, pid, orig, bullet ? *shooter : -1, was);
        }
    }
    __asm {
        push hp
        push bullet
        push pid
        call fn
        add esp, 12
    }
}

/* ---- 0.14.3 host: a guest's hit (msg 0x83) ends in bmShotObject(shooter, target, origin, ...) (+0xA5430, called
 * at +0x18F4D). It treats a shooter whose owner (+0x1A4) is not 0 as "not a player". For the call the guest's
 * player counts as a local player, and target health (+0x1F4) is logged before/after. ---- */
typedef void (__cdecl *fn_bmshot)(int, int, void *, int, int, int, int, int);
static fn_bmshot o_bmshot;
static void __cdecl my_bmshot(int shooter, int target, void *org, int a3, int a4, int a5, int a6, int a7)
{
    static int logged; int s = (short)shooter, t = (short)target, own = -1;
    BYTE *se = (s > 0 && s < 4096) ? (BYTE *)V.sv_ent[s] : NULL, *te = (t > 0 && t < 4096) ? (BYTE *)V.sv_ent[t] : NULL;
    float h0 = -1, h1 = -1; char sn[5] = "-", tn[5] = "-";
    if (se && !IsBadReadPtr(se, 0x1a8)) { tname(*(DWORD *)(se + 8), sn); own = *(int *)(se + 0x1a4); }
    if (te && !IsBadReadPtr(te, 0x1f8)) { tname(*(DWORD *)(te + 8), tn); h0 = *(float *)(te + 0x1f4); }
    if (se && own > 0 && is_play(se)) *(int *)(se + 0x1a4) = 0;
    o_bmshot(shooter, target, org, a3, a4, a5, a6, a7);
    if (se && own > 0 && is_play(se)) *(int *)(se + 0x1a4) = own;
    te = (t > 0 && t < 4096) ? (BYTE *)V.sv_ent[t] : NULL;
    if (te && !IsBadReadPtr(te, 0x1f8)) h1 = *(float *)(te + 0x1f4);
    if (logged++ < 30) logf("host: guest hit: shooter %d %s (owner %d) -> target %d %s, health %.1f -> %.1f, weapon %d", s, sn, own, t, tn, h0, h1, a4);
}

typedef void (__cdecl *fn_vehuse)(int veh, DWORD a, int b, int c);
static void __cdecl my_vehuse(int veh32, DWORD a, int b, int c)
{
    static int logged; int veh = (short)veh32;
    fn_vehuse o = (fn_vehuse)g_vehuse_tramp;
    if (*V.mode == 4 && veh > 0 && veh < 4096 && V.sv_ent[veh]) {
        *V.mode = 1; o(veh32, a, b, c); *V.mode = 4;
        if (logged++ < 20) logf("guest: vehicle %d use (%lu,%d,%d) handled here", veh, a, b, c);
        return;
    }
    if (*V.mode == 4 && logged++ < 20) logf("guest: vehicle %d use (%lu,%d,%d) sent to host", veh, a, b, c);
    o(veh32, a, b, c);
}

static void call_create_player(BYTE *g, int pid, float *pos, int flag)
{
    BYTE *fn = g + 0x8230; float ang = 0.0f;
    __asm {
        push flag
        push ang
        push pos
        mov eax, pid
        call fn
        add esp, 12
    }
}

static int other_player_pos(int dead_pid, float *out)
{
    int p;
    for (p = 1; p < 4096; p++) {
        BYTE *e = (BYTE *)V.sv_ent[p];
        if (p == dead_pid || !is_play(e) || IsBadReadPtr(e, 0x2c)) continue;
        memcpy(out, e + 0x20, 12); out[0] += 150.0f;
        return p;
    }
    return 0;
}

static void do_respawn(BYTE *g, int m, int old_pid, float *lastpos)
{
    BYTE *info = player_info(m), bp[64], buf[256]; float pos[3]; int np, near_pid, f0 = V.clfocus(0);
    near_pid = other_player_pos(old_pid, pos);
    if (!near_pid) memcpy(pos, lastpos, sizeof pos);
    np = V.getpid(2);
    if (np <= 0 || np > 0xC00) { pid_sweep("respawn"); np = V.getpid(2); }
    if (np <= 0 || np > 0xC00) { logf("respawn: no free entity id for machine %d", m); return; }
    *(short *)(info + 0x100) = (short)np;
    if (m == 1) call_create_player(g, np, pos, *(int *)(g + 0x11F4B4));
    else {
        memset(bp, 0, sizeof bp); V.bpinit(bp, buf);
        V.bpwrite(bp, &np, 16); V.bporigin(bp, pos); V.bpangle(bp, 0.0f);
        V.svsend((unsigned char)m, 0x73, bp);
    }
    V.svfocus((unsigned short)m, 4, np); V.svfocus((unsigned short)m, 1, np);
    V.svfocus((unsigned short)m, 0, np); V.svfocus((unsigned short)m, 10, np);
    if (m == 1 && f0 > 0 && f0 < 4096 && V.sv_ent[f0] && *(DWORD *)((BYTE *)V.sv_ent[f0] + 8) == T_DCAM && g_enremove)
        g_enremove(V.sv_ent[f0]);
    logf("respawn: machine %d (%s) back as pid %d at %.0f %.0f %.0f (%s)", m, m == 1 ? "host" : (char *)info + 1, np,
         pos[0], pos[1], pos[2], near_pid ? "next to another player" : "where it died");
}

static void respawn_tick(BYTE *g)
{
    static DWORD last, dead_since[32]; static int seen_alive[32], lastpid[32]; static float lastpos[32][3];
    DWORD now = GetTickCount(); int m;
    if (!g_respawn_on || now - last < 1000) return;
    last = now;
    for (m = 1; m < 31; m++) {
        int pid, alive; BYTE *e;
        if (m >= 2 && *(int *)(V.machine + m * 0xd8c + 4) != 2) { dead_since[m] = 0; seen_alive[m] = 0; continue; }
        pid = m == 1 ? V.clfocus(10) : *(short *)(player_info(m) + 0x100);
        if (pid <= 0 || pid >= 4096) continue;
        e = (BYTE *)V.sv_ent[pid];
        alive = is_play(e);
        if (m == 1 && alive) {                      /* the host's death camera takes focus 0 */
            int f0 = V.clfocus(0); BYTE *c = (f0 > 0 && f0 < 4096) ? (BYTE *)V.sv_ent[f0] : NULL;
            if (c && !IsBadReadPtr(c, 12) && *(DWORD *)(c + 8) == T_DCAM) alive = 0;
        }
        if (m >= 2 && g_dead_report_pid == pid) alive = 0;
        if (alive) { memcpy(lastpos[m], e + 0x20, 12); seen_alive[m] = 1; lastpid[m] = pid; dead_since[m] = 0; continue; }
        if (!seen_alive[m]) continue;
        if (!dead_since[m]) { dead_since[m] = now; logf("respawn: machine %d player %d died, respawn in 8 s", m, pid); continue; }
        if (now - dead_since[m] < 8000) continue;
        do_respawn(g, m, pid, lastpos[m]);
        if (g_dead_report_pid == pid) g_dead_report_pid = 0;
        dead_since[m] = 0; seen_alive[m] = 0;
    }
}

static void guest_death_tick(void)
{
    static DWORD since, last; static int lastpid;
    DWORD now = GetTickCount(); int f10 = V.clfocus(10), f0 = V.clfocus(0), dead; BYTE *c;
    c = (f0 > 0 && f0 < 4096) ? (BYTE *)(V.sv_ent[f0] ? V.sv_ent[f0] : g_cl_ent[f0]) : NULL;
    dead = !(f10 > 0 && f10 < 4096 && is_play(V.sv_ent[f10])) || (c && !IsBadReadPtr(c, 12) && *(DWORD *)(c + 8) == T_DCAM);
    if (!dead) { lastpid = f10; since = 0; return; }
    if (!lastpid) return;
    if (!since) { since = now; logf("guest: our player %d died, asking the host for a respawn", lastpid); }
    if (now - since > 3000 && now - last > 2000 && g_sync != INVALID_SOCKET) {
        struct sync_pkt h; last = now;
        memset(&h, 0, sizeof h); h.magic = SYNC_MAGIC; h.kind = 3; h.seq = (DWORD)lastpid;
        sendto(g_sync, (char *)&h, sizeof h, 0, (struct sockaddr *)&g_sync_host, sizeof g_sync_host);
    }
}

/* =====================================================================================================
 * 0.15: saving, quests, reputation, guest profile, admin console.
 *  - SAVE: vital lsmSaveFile returns 0 at once unless stMode == 1 (single player), so in co-op the menu
 *    wrote the slot's ini/screenshot but no game file. Host: run the save body anyway, with stMode 1 for
 *    the call, and with the guests' players (+ their weapon models) taken out of sv_ent/cl_ent so the
 *    save is a clean single-player world. Guest: its world is a copy, so it saves its CHARACTER instead
 *    (coop_profile.ini next to bp_coop.ini: money +0x1014C, 14 experience floats +0x1A208, inventory).
 *  - QUESTS: game.dll missionSave (+0xBA670, buffer in ECX) / missionLoad (+0xBA7A0, buffer in EAX) are
 *    pure data (128 mission slots of 0x41D0 at +0x292BF0 + a few globals). The host sends that blob over
 *    the side channel when it changes; the guest loads it, so its PDA / target marker follow the host.
 *  - REPUTATION: sideinfo[16] rank floats at game +0x4C851C + side*0x290 (sideGetRank/sideSetRank). Host
 *    sends all 16 every 2 s, the guest applies changed ones with sideSetRank (runs sideUpdatePlayer).
 *  - ADMIN: a console window (admin_console=1); commands run on the game thread. Teleport = the same
 *    fields the game's own teleport sets (entity +0x20, rigid body (+0x928) +0xFC); a remote player is
 *    moved by ITS machine (side channel kind 6), since each machine owns its own player's movement.
 * ===================================================================================================== */
typedef void (__cdecl *fn_v15)(void);
static float *g_worigin;
#define K_RANKS 4
#define K_MISSION 5
#define K_TP 6
#define K_CMD 7
#define MS_CHUNK 1200
#define MS_MAX (1 << 20)
#pragma pack(push, 1)
struct ext_hdr { DWORD magic, kind, seq; };
struct ext_ranks { struct ext_hdr h; float r[16]; };
struct ext_ms { struct ext_hdr h; DWORD ver, total, off; WORD len; BYTE d[MS_CHUNK]; };
struct ext_tp { struct ext_hdr h; int pid; float pos[3]; };
struct ext_cmd { struct ext_hdr h; char text[200]; };
#pragma pack(pop)
typedef void (__cdecl *fn_setrank)(int side, float v);
typedef void (__cdecl *fn_invitem)(short pid, short idx, short *sid, short *cnt);
typedef int (__cdecl *fn_invcount)(short pid, short sid);
typedef void (__cdecl *fn_invadd)(short pid, short sid, unsigned short cnt, int flag);
typedef int (__cdecl *fn_lsmsave)(char *name);
typedef int (__cdecl *fn_gcon)(char *cmd);
typedef void (__cdecl *fn_ccon)(char *cmd);
typedef void (__fastcall *fn_relink)(void *self, void *edx);
static int g_admin_on, g_ms_on, g_rank_on, g_prof_on, g_save_on, g_inited15;
static BYTE *g_ms_save, *g_ms_load; static fn_v15 g_ms_target;
static fn_setrank g_setrank; static fn_invitem g_invitem; static fn_invcount g_invcount; static fn_invadd g_invadd;
typedef int (__cdecl *fn_msgout)(int, int, void *, void *);
static fn_msgout g_msgout; static float *g_frametime;
static void install_taxi(BYTE *g);
static fn_lsmsave g_lsm_body; static fn_gcon g_gamecon; static fn_ccon g_vitalcon; static fn_relink g_relink;
static BYTE g_msbuf[MS_MAX], g_msrx[MS_MAX], g_msgot[MS_MAX / MS_CHUNK + 1];
static DWORD g_ms_len, g_ms_ver, g_ms_sendoff, g_ms_rxver, g_ms_rxlen, g_ms_applied;
static CRITICAL_SECTION g_aq_cs; static char g_aq[32][256]; static volatile LONG g_aq_h, g_aq_t; static HANDLE g_acon_out;
static void admin_exec(char *line, int depth);

static void aprint(const char *fmt, ...)
{
    char b[1024]; va_list ap; DWORD w;
    va_start(ap, fmt); _vsnprintf(b, sizeof b - 2, fmt, ap); va_end(ap); b[sizeof b - 2] = 0;
    logf("admin: %s", b);
    if (g_acon_out) { strcat(b, "\n"); WriteConsoleA(g_acon_out, b, (DWORD)strlen(b), &w, NULL); }
}

static void admin_push(const char *s)
{
    if (!g_inited15) return;
    EnterCriticalSection(&g_aq_cs);
    if (g_aq_h - g_aq_t < 32) { strncpy(g_aq[g_aq_h % 32], s, 255); g_aq[g_aq_h % 32][255] = 0; g_aq_h++; }
    LeaveCriticalSection(&g_aq_cs);
}

static DWORD WINAPI admin_console_thread(LPVOID u)
{
    HANDLE in; char line[256]; DWORD n;
    AllocConsole();
    SetConsoleTitleA(*V.mode == 4 ? "Boiling Point co-op admin (guest)" : "Boiling Point co-op admin (host)");
    g_acon_out = GetStdHandle(STD_OUTPUT_HANDLE); in = GetStdHandle(STD_INPUT_HANDLE);
    aprint("co-op admin console. Type 'help'.");
    for (;;) {
        if (!ReadConsoleA(in, line, sizeof line - 1, &n, NULL)) { Sleep(200); continue; }
        line[n] = 0;
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n) admin_push(line);
    }
}

static void sync_send(int host, void *p, int len)
{
    int i; DWORD now = GetTickCount();
    if (g_sync == INVALID_SOCKET) return;
    if (!host) { sendto(g_sync, (char *)p, len, 0, (struct sockaddr *)&g_sync_host, sizeof g_sync_host); return; }
    for (i = 0; i < 8; i++)
        if (g_guest_seen[i] && now - g_guest_seen[i] < 15000)
            sendto(g_sync, (char *)p, len, 0, (struct sockaddr *)&g_guest_addr[i], sizeof g_guest_addr[i]);
}

static BYTE *local_player(void)
{
    int p = V.clfocus(10); BYTE *e;
    if (p <= 0 || p >= 4096) return NULL;
    e = (BYTE *)V.sv_ent[p]; if (!is_play(e)) e = (BYTE *)g_cl_ent[p];
    return is_play(e) ? e : NULL;
}

static void abs_pos(BYTE *e, float *o)
{
    o[0] = ((float *)(e + 0x20))[0] + (g_worigin ? g_worigin[0] : 0);
    o[1] = ((float *)(e + 0x20))[1] + (g_worigin ? g_worigin[1] : 0);
    o[2] = ((float *)(e + 0x20))[2] + (g_worigin ? g_worigin[2] : 0);
}

static void move_ent(BYTE *e, const float *d)
{
    BYTE *rb; int k;
    if (!e || IsBadWritePtr(e + 0x20, 12)) return;
    for (k = 0; k < 3; k++) ((float *)(e + 0x20))[k] += d[k];
    rb = *(BYTE **)(e + 0x928);
    if (rb && !IsBadWritePtr(rb + 0xFC, 0x34)) {
        for (k = 0; k < 3; k++) ((float *)(rb + 0xFC))[k] += d[k];
        memset(rb + 0x118, 0, 12);                         /* no momentum carried over */
        if (g_relink) g_relink(rb, NULL);
    }
}

static int local_teleport(const float *absp)
{
    int p = V.clfocus(10), f4 = V.clfocus(4); BYTE *sv, *cl, *e; float cur[3], d[3];
    if (p <= 0 || p >= 4096 || !(e = local_player())) { aprint("teleport: no local player"); return 0; }
    if (f4 != p) { aprint("teleport: leave the vehicle first (focus %d, player %d)", f4, p); return 0; }
    abs_pos(e, cur);
    d[0] = absp[0] - cur[0]; d[1] = absp[1] - cur[1]; d[2] = absp[2] - cur[2];
    sv = (BYTE *)V.sv_ent[p]; cl = (BYTE *)g_cl_ent[p];
    if (is_play(sv)) move_ent(sv, d);
    if (is_play(cl) && cl != sv) move_ent(cl, d);
    aprint("teleport: player %d moved %.0f %.0f %.0f -> %.0f %.0f %.0f", p, cur[0], cur[1], cur[2], absp[0], absp[1], absp[2]);
    return 1;
}

static void send_tp(int host, int pid, const float *absp)
{
    struct ext_tp t; t.h.magic = SYNC_MAGIC; t.h.kind = K_TP; t.h.seq = GetTickCount();
    t.pid = pid; memcpy(t.pos, absp, 12);
    sync_send(host, &t, sizeof t);
}

/* ---- guest character profile ---- */
static void profile_path(char *out)
{
    char *s; strcpy(out, g_ini); s = strrchr(out, '\\'); strcpy(s ? s + 1 : out, "coop_profile.ini");
}

static void profile_save(const char *why)
{
    char path[MAX_PATH], k[32], v[64], items[4096]; BYTE *e = local_player(); int p = V.clfocus(10), i, n = 0;
    if (!e) { aprint("profile: no local player, not saved"); return; }
    profile_path(path);
    sprintf(v, "%d", *(int *)(e + 0x1014C)); WritePrivateProfileStringA("profile", "money", v, path);
    for (i = 0; i < 14; i++) {
        sprintf(k, "exp%d", i); sprintf(v, "%.3f", ((float *)(e + 0x1A208))[i]);
        WritePrivateProfileStringA("profile", k, v, path);
    }
    items[0] = 0;
    if (g_invitem)
        for (i = 0; i < 0x190 && strlen(items) < sizeof items - 32; i++) {
            short sid = 0, cnt = 0; g_invitem((short)p, (short)i, &sid, &cnt);
            if (sid > 0 && cnt > 0) { sprintf(v, "%s%d:%d", n ? "," : "", sid, cnt); strcat(items, v); n++; }
        }
    WritePrivateProfileStringA("profile", "items", items, path);
    aprint("profile: saved (%s): money %d, %d inventory entries -> %s", why, *(int *)(e + 0x1014C), n, path);
}

static void profile_load(void)
{
    char path[MAX_PATH], k[32], v[64], items[4096], *t; BYTE *e = local_player(); int p = V.clfocus(10), i, added = 0;
    if (!e) { aprint("profile: no local player"); return; }
    profile_path(path);
    if (GetPrivateProfileStringA("profile", "money", "", v, sizeof v, path) == 0) { aprint("profile: none saved yet (%s)", path); return; }
    *(int *)(e + 0x1014C) = atoi(v);
    for (i = 0; i < 14; i++) {
        sprintf(k, "exp%d", i);
        if (GetPrivateProfileStringA("profile", k, "", v, sizeof v, path)) ((float *)(e + 0x1A208))[i] = (float)atof(v);
    }
    GetPrivateProfileStringA("profile", "items", "", items, sizeof items, path);
    if (g_invadd && g_invcount)
        for (t = strtok(items, ","); t; t = strtok(NULL, ",")) {
            int sid = atoi(t), cnt = strchr(t, ':') ? atoi(strchr(t, ':') + 1) : 0, have;
            if (sid <= 0 || cnt <= 0) continue;
            have = g_invcount((short)p, (short)sid);
            if (have < cnt) { g_invadd((short)p, (short)sid, (unsigned short)(cnt - have), 0); added++; }
        }
    aprint("profile: loaded: money %d, experience restored, %d item kinds topped up", *(int *)(e + 0x1014C), added);
}

/* ---- save hook (vital lsmSaveFile) ---- */
static int __cdecl my_lsmsave(char *name)
{
    static short hid[64]; static void *hsv[64], *hcl[64]; int n = 0, p, i, r, mode = *V.mode;
    if (mode == 4) {
        if (g_prof_on) profile_save("guest pressed save");
        logf("save: guest save '%s' -> character profile only (the world belongs to the host)", name ? name : "?");
        return 0;
    }
    if (mode != 3 || !g_save_on) return mode == 1 ? g_lsm_body(name) : 0;
    for (p = 1; p < 4096 && n < 60; p++) {
        BYTE *e = (BYTE *)V.sv_ent[p]; int w;
        if (!is_play(e) || *(int *)(e + 0x1a4) == 0) continue;
        hid[n] = (short)p; n++;
        w = *(short *)(e + 0x1A1D6);
        if (w > 0 && w < 4096 && n < 60) { hid[n] = (short)w; n++; }
    }
    for (i = 0; i < n; i++) { hsv[i] = V.sv_ent[hid[i]]; hcl[i] = g_cl_ent[hid[i]]; V.sv_ent[hid[i]] = NULL; g_cl_ent[hid[i]] = NULL; }
    *V.mode = 1;
    r = g_lsm_body(name);
    *V.mode = mode;
    for (i = 0; i < n; i++) { V.sv_ent[hid[i]] = hsv[i]; g_cl_ent[hid[i]] = hcl[i]; }
    logf("save: host saved '%s' in co-op (result %d), %d guest entities left out", name ? name : "?", r, n);
    if (g_prof_on) { struct ext_cmd c; memset(&c, 0, sizeof c); c.h.magic = SYNC_MAGIC; c.h.kind = K_CMD; strcpy(c.text, "profile save"); sync_send(1, &c, sizeof c); }
    return r;
}

/* ---- missions ---- */
static DWORD ms_serialize(void)
{
    BYTE *f = g_ms_save, *buf = g_msbuf, *end = NULL;
    __asm { mov ecx, buf
            call f
            mov end, eax }
    return end > buf && end < buf + MS_MAX ? (DWORD)(end - buf) : 0;
}

static void ms_host_tick(void)
{
    static DWORD last, lastfull, hash, sent_logs; DWORD now = GetTickCount(), h = 2166136261u, i;
    if (now - last >= 10000) {   /* the blob has timers in it, so it differs every time: check every 10 s */
        DWORD len; last = now;
        len = ms_serialize();
        for (i = 0; i < len; i++) h = (h ^ g_msbuf[i]) * 16777619u;
        if (len && (h != hash || now - lastfull > 20000)) {
            if (h != hash && sent_logs++ < 30) logf("quests: host mission state changed (%lu bytes), sending to guests", len);
            hash = h; lastfull = now; g_ms_len = len; g_ms_ver++; g_ms_sendoff = 0;
        }
    }
    for (i = 0; i < 24 && g_ms_len && g_ms_sendoff < g_ms_len; i++) {
        static struct ext_ms m; DWORD n = g_ms_len - g_ms_sendoff; if (n > MS_CHUNK) n = MS_CHUNK;
        m.h.magic = SYNC_MAGIC; m.h.kind = K_MISSION; m.h.seq = now;
        m.ver = g_ms_ver; m.total = g_ms_len; m.off = g_ms_sendoff; m.len = (WORD)n;
        memcpy(m.d, g_msbuf + g_ms_sendoff, n);
        sync_send(1, &m, (int)(sizeof m - MS_CHUNK + n));
        g_ms_sendoff += n;
    }
}

static void ms_guest_rx(struct ext_ms *m, int n)
{
    DWORD i, chunks; BYTE *f = g_ms_load, *buf = g_msrx;
    if (n < (int)(sizeof *m - MS_CHUNK) || m->total > MS_MAX || m->off + m->len > m->total || m->off % MS_CHUNK) return;
    if (m->ver != g_ms_rxver || m->total != g_ms_rxlen) { g_ms_rxver = m->ver; g_ms_rxlen = m->total; memset(g_msgot, 0, sizeof g_msgot); }
    memcpy(g_msrx + m->off, m->d, m->len); g_msgot[m->off / MS_CHUNK] = 1;
    chunks = (g_ms_rxlen + MS_CHUNK - 1) / MS_CHUNK;
    for (i = 0; i < chunks; i++) if (!g_msgot[i]) return;
    if (g_ms_applied == g_ms_rxver) return;
    {   static DWORD lasthash; DWORD h = 2166136261u;
        for (i = 0; i < g_ms_rxlen; i++) h = (h ^ g_msrx[i]) * 16777619u;
        g_ms_applied = g_ms_rxver;
        if (h == lasthash) return;           /* periodic resend, nothing new */
        lasthash = h; }
    __asm { mov eax, buf
            call f }
    if (g_ms_target) g_ms_target();
    logf("quests: loaded the host's mission state (%lu bytes, version %lu)", g_ms_rxlen, g_ms_rxver);
}

/* ---- reputation ---- */
static void rank_host_tick(BYTE *g)
{
    static DWORD last; struct ext_ranks r; int s; DWORD now = GetTickCount();
    if (now - last < 2000) return;
    last = now;
    r.h.magic = SYNC_MAGIC; r.h.kind = K_RANKS; r.h.seq = now;
    for (s = 0; s < 16; s++) r.r[s] = *(float *)(g + 0x4C851C + s * 0x290);
    sync_send(1, &r, sizeof r);
}

/* sideAddRank also lights the relation arrow on the left of the HUD: table game +0x7F4BAC + side*0x30
 * (+0 until = DrawFrameTime + 3, +0xC start time, +0x10 direction +1/-1) and msgSendOutside(3, 0x8080,
 * side, dir) to Xenus. sideSetRank does not, so the guest saw no arrow; do the same here. */
static void rank_guest_rx(BYTE *g, struct ext_ranks *r)
{
    int s; static int logs, first = 1;
    for (s = 0; s < 16; s++) {
        float cur = *(float *)(g + 0x4C851C + s * 0x290);
        if (r->r[s] != r->r[s] || cur == r->r[s]) continue;
        if (logs++ < 40) logf("reputation: side %d %.2f -> %.2f (from host)", s, cur, r->r[s]);
        if (g_setrank) g_setrank(s, r->r[s]); else *(float *)(g + 0x4C851C + s * 0x290) = r->r[s];
        if (!first && g_frametime && (r->r[s] - cur > 0.001f || cur - r->r[s] > 0.001f)) {
            BYTE *hud = g + 0x7F4BAC + s * 0x30; int dir = r->r[s] > cur ? 1 : -1;
            *(float *)hud = *g_frametime + 3.0f; *(float *)(hud + 0xC) = *g_frametime; hud[0x10] = (BYTE)(char)dir;
            if (g_msgout) g_msgout(3, 0x8080, (void *)(INT_PTR)s, (void *)(INT_PTR)dir);
        }
    }
    first = 0;   /* no arrows for the values copied right after joining */
}

/* called from both side-channel receive loops for kinds >= 4 */
static void ext_recv(int host, BYTE *b, int n)
{
    struct ext_hdr *h = (struct ext_hdr *)b; BYTE *g = (BYTE *)GetModuleHandleA("game.dll");
    if (n < (int)sizeof *h || h->magic != SYNC_MAGIC || !g) return;
    if (h->kind == K_RANKS && !host && g_rank_on && n >= (int)sizeof(struct ext_ranks)) rank_guest_rx(g, (struct ext_ranks *)b);
    else if (h->kind == K_MISSION && !host && g_ms_on && g_ms_load) ms_guest_rx((struct ext_ms *)b, n);
    else if (h->kind == K_TP && n >= (int)sizeof(struct ext_tp)) {
        struct ext_tp *t = (struct ext_tp *)b;
        if (t->pid == -1 || t->pid == V.clfocus(10)) { aprint("teleport requested by the other machine"); local_teleport(t->pos); }
    } else if (h->kind == K_CMD && n >= (int)sizeof(struct ext_cmd)) {
        struct ext_cmd *c = (struct ext_cmd *)b; c->text[sizeof c->text - 1] = 0;
        if (!strcmp(c->text, "profile save") && g_prof_on) profile_save("host saved the game");
    }
}

/* ---- admin commands ---- */
static int find_other_player(int skip_local, BYTE **out)
{
    int me = V.clfocus(10), p;
    for (p = 1; p < 4096; p++) {
        BYTE *e = (BYTE *)g_cl_ent[p]; if (!is_play(e)) e = (BYTE *)V.sv_ent[p];
        if (!is_play(e) || (skip_local && p == me)) continue;
        if (IsBadReadPtr(e, 0x30)) continue;
        if (out) *out = e;
        return p;
    }
    return 0;
}

static void admin_exec(char *line, int depth)
{
    char cmd[64] = "", a1[128] = "", a2[64] = "", a3[64] = ""; int host = *V.mode == 3, me = V.clfocus(10);
    BYTE *g = (BYTE *)GetModuleHandleA("game.dll"), *e = local_player(); static float mark[3]; static int marked;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#' || (line[0] == '/' && line[1] == '/')) return;
    sscanf(line, "%63s %127s %63s %63s", cmd, a1, a2, a3); _strlwr(cmd);
    if (!strcmp(cmd, "help")) {
        aprint("players | pos | tp X Y Z | goto PID | bring PID|all | mark | back | up [units]");
        aprint("rank | rank SIDE VALUE (host) | money AMOUNT | exp INDEX VALUE | time +HOURS (host)");
        aprint("quests (print active missions to the log) | cmd TEXT (game console command, e.g. cmd GET TIME)");
        aprint("profile save | profile load (guest character) | exec FILE (one command per line, 'wait MS')");
    } else if (!strcmp(cmd, "players")) {
        int p; for (p = 1; p < 4096; p++) {
            BYTE *x = (BYTE *)V.sv_ent[p], *c = (BYTE *)g_cl_ent[p]; float q[3];
            if (!is_play(x)) x = c; if (!is_play(x) || IsBadReadPtr(x, 0x1A240)) continue;
            abs_pos(x, q);
            aprint("pid %d%s owner %d at %.0f %.0f %.0f money %d side %d%s", p, p == me ? " (me)" : "", *(int *)(x + 0x1a4),
                   q[0], q[1], q[2], *(int *)(x + 0x1014C), side_of(g, p), V.sv_ent[p] ? "" : " (remote copy)");
        }
    } else if (!strcmp(cmd, "pos")) {
        float q[3]; if (!e) { aprint("no local player"); return; }
        abs_pos(e, q); aprint("me (pid %d) at %.1f %.1f %.1f", me, q[0], q[1], q[2]);
    } else if (!strcmp(cmd, "tp")) {
        float q[3]; if (sscanf(line, "%*s %f %f %f", &q[0], &q[1], &q[2]) != 3) { aprint("tp X Y Z"); return; }
        local_teleport(q);
    } else if (!strcmp(cmd, "up")) {
        float q[3]; if (!e) return; abs_pos(e, q); q[1] += a1[0] ? (float)atof(a1) : 300.0f; local_teleport(q);
    } else if (!strcmp(cmd, "mark")) {
        if (!e) return; abs_pos(e, mark); marked = 1; aprint("marked %.0f %.0f %.0f", mark[0], mark[1], mark[2]);
    } else if (!strcmp(cmd, "back")) {
        if (!marked) aprint("no mark yet"); else local_teleport(mark);
    } else if (!strcmp(cmd, "goto")) {
        int p = a1[0] ? atoi(a1) : find_other_player(1, NULL); BYTE *x; float q[3];
        x = (p > 0 && p < 4096) ? (BYTE *)g_cl_ent[p] : NULL; if (!is_play(x)) x = (p > 0 && p < 4096) ? (BYTE *)V.sv_ent[p] : NULL;
        if (!is_play(x) || p == me) { aprint("goto: no other player %d", p); return; }
        abs_pos(x, q); q[0] += 120.0f; local_teleport(q);
    } else if (!strcmp(cmd, "bring")) {
        float q[3]; int p = !_stricmp(a1, "all") || !a1[0] ? -1 : atoi(a1);
        if (!e) { aprint("no local player"); return; }
        abs_pos(e, q); q[0] += 120.0f;
        send_tp(host, p, q);
        aprint("bring: asked %s to move next to me", p == -1 ? "every other player" : "that player");
    } else if (!strcmp(cmd, "rank")) {
        int s;
        if (a1[0] && a2[0]) {
            s = atoi(a1); if (s < 0 || s > 15) return;
            if (g_setrank) g_setrank(s, (float)atof(a2));
            aprint("side %d rank set to %.2f%s", s, (float)atof(a2), host ? "" : " (the host will overwrite it)");
        } else for (s = 0; s < 16; s++) {
            int *list = (int *)(g + 0x4C8290 + s * 0x290), cnt = list[0x80];
            if (cnt) aprint("side %d: rank %.2f, %d units, enemies listed %d", s, *(float *)(g + 0x4C851C + s * 0x290), cnt, *(int *)(g + 0x4C8290 + s * 0x290 + 0x288));
        }
    } else if (!strcmp(cmd, "money")) {
        if (!e) return; *(int *)(e + 0x1014C) += atoi(a1); aprint("money now %d", *(int *)(e + 0x1014C));
    } else if (!strcmp(cmd, "exp")) {
        int i = atoi(a1); if (!e || i < 0 || i > 13) { aprint("exp INDEX(0-13) VALUE"); return; }
        ((float *)(e + 0x1A208))[i] = (float)atof(a2); aprint("experience %d = %.2f", i, ((float *)(e + 0x1A208))[i]);
    } else if (!strcmp(cmd, "time")) {
        float h = (float)atof(a1);
        if (!host) { aprint("time: host only (the guest's clock follows the host)"); return; }
        *(float *)(g + GT_START) += h * 3600.0f; aprint("time moved by %.1f hours", h);
    } else if (!strcmp(cmd, "quests")) {
        fn_v15 pr = (fn_v15)GetProcAddress((HMODULE)g, "?missionPrintActive@@YAXXZ");
        if (pr) pr(); aprint("active missions printed to the log (lines 'con:')");
    } else if (!strcmp(cmd, "cmd")) {
        char *t = line + 3; int r = 0; while (*t == ' ') t++;
        if (g_gamecon) r = g_gamecon(t);
        if (!r && g_vitalcon) g_vitalcon(t);
        aprint("cmd '%s' sent (game parser %s)", t, r ? "handled it" : "did not, passed to the engine");
    } else if (!strcmp(cmd, "profile")) {
        if (!_stricmp(a1, "save")) profile_save("admin command"); else if (!_stricmp(a1, "load")) profile_load(); else aprint("profile save|load");
    } else if (!strcmp(cmd, "exec")) {
        char path[MAX_PATH], l[256]; FILE *f;
        if (depth > 3) return;
        if (strchr(a1, ':')) strcpy(path, a1); else { char *s; strcpy(path, g_ini); s = strrchr(path, '\\'); strcpy(s ? s + 1 : path, a1); }
        if (!(f = fopen(path, "r"))) { aprint("exec: cannot open %s", path); return; }
        aprint("exec %s", path);
        while (fgets(l, sizeof l, f)) {
            size_t k = strlen(l); while (k && (l[k - 1] == '\n' || l[k - 1] == '\r')) l[--k] = 0;
            if (!_strnicmp(l, "wait ", 5)) { admin_push(l); continue; }   /* waits are queued in order */
            admin_push(l);
        }
        fclose(f);
    } else aprint("unknown command '%s' (help)", cmd);
}

static void admin_tick(int ready)
{
    static DWORD wait_until; static int autoexec_done;
    char line[256];
    if (ready && !autoexec_done) {
        char path[MAX_PATH], *s; autoexec_done = 1;
        strcpy(path, g_ini); s = strrchr(path, '\\'); strcpy(s ? s + 1 : path, "coop_autoexec.txt");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) admin_push("exec coop_autoexec.txt");
    }
    while (GetTickCount() >= wait_until) {
        int got = 0;
        EnterCriticalSection(&g_aq_cs);
        if (g_aq_t != g_aq_h) { strcpy(line, g_aq[g_aq_t % 32]); g_aq_t++; got = 1; }
        LeaveCriticalSection(&g_aq_cs);
        if (!got) break;
        if (!_strnicmp(line, "wait ", 5)) { wait_until = GetTickCount() + (DWORD)atoi(line + 5); break; }
        aprint("> %s", line);
        admin_exec(line, 0);
    }
}

static void install_015(BYTE *g, int host)
{
    HMODULE v = GetModuleHandleA("vital.dll"); BYTE *ls;
    InitializeCriticalSection(&g_aq_cs); g_inited15 = 1;
    g_admin_on = GetPrivateProfileIntA("coop", "admin_console", 1, g_ini);
    g_ms_on = GetPrivateProfileIntA("coop", "sync_quests", 1, g_ini);
    g_rank_on = GetPrivateProfileIntA("coop", "sync_reputation", 1, g_ini);
    g_prof_on = GetPrivateProfileIntA("coop", "guest_profile", 1, g_ini);
    g_save_on = GetPrivateProfileIntA("coop", "host_coop_save", 1, g_ini);
    g_setrank = (fn_setrank)GetProcAddress((HMODULE)g, "?sideSetRank@@YAXHM@Z");
    g_ms_target = (fn_v15)GetProcAddress((HMODULE)g, "?missionUpdateTargetPointer@@YAXXZ");
    g_gamecon = (fn_gcon)GetProcAddress((HMODULE)g, "?gameConParseCmd@@YAHPAD@Z");
    /* vital exports some functions under their plain names only (0.15 looked for the decorated ones) */
    g_vitalcon = (fn_ccon)GetProcAddress(v, "conParseCmd");
    if (!g_vitalcon) g_vitalcon = (fn_ccon)GetProcAddress(v, "?conParseCmd@@YAXPAD@Z");
    g_invitem = (fn_invitem)GetProcAddress(v, "stInventoryGetItem");
    if (!g_invitem) g_invitem = (fn_invitem)GetProcAddress(v, "?stInventoryGetItem@@YAXFFPAF0@Z");
    g_invcount = (fn_invcount)GetProcAddress(v, "stInventoryItemCount");
    if (!g_invcount) g_invcount = (fn_invcount)GetProcAddress(v, "?stInventoryItemCount@@YAHFF@Z");
    g_invadd = (fn_invadd)GetProcAddress(v, "stInventoryAdd");
    if (!g_invadd) g_invadd = (fn_invadd)GetProcAddress(v, "?stInventoryAdd@@YAXFFGH@Z");
    g_msgout = (fn_msgout)GetProcAddress(v, "msgSendOutside");
    if (!g_msgout) g_msgout = (fn_msgout)GetProcAddress(v, "?msgSendOutside@@YAHHHPAX0@Z");
    g_frametime = (float *)GetProcAddress(v, "?DrawFrameTime@@3MA");
    if (!host) install_taxi(g);
    g_relink = (fn_relink)GetProcAddress(v, "?Relink@t_rigidbody@@QAEXXZ");
    if (!g_worigin) g_worigin = (float *)GetProcAddress(v, "?WindowOrigin@@3PAMA");
    /* missionSave: mov eax,[62292BE4] ; push ebx ; push ebp.  missionLoad: push ebx ; push ebp ; push esi ; mov esi,[get_i] */
    if (g[0xBA670] == 0xA1 && *(DWORD *)(g + 0xBA671) == (DWORD)(g + 0x292BE4) && g[0xBA675] == 0x53) g_ms_save = g + 0xBA670;
    if (g[0xBA7A0] == 0x53 && g[0xBA7A1] == 0x55 && g[0xBA7A2] == 0x56 && g[0xBA7A3] == 0x8B && g[0xBA7AA] == 0x68) g_ms_load = g + 0xBA7A0;
    if (!g_ms_save || !g_ms_load) { logf("0.15: missionSave/Load bytes differ (%p %p), quest sync off", (void *)g_ms_save, (void *)g_ms_load); g_ms_on = 0; }
    ls = (BYTE *)GetProcAddress(v, "?lsmSaveFile@@YAHPAD@Z");
    if (ls && ls[0] == 0x83 && ls[1] == 0x3D && ls[6] == 1 && ls[7] == 0x74 && ls[8] == 0x03 && ls[12] == 0x53) {
        g_lsm_body = (fn_lsmsave)(ls + 12);
        jmp_hook(ls, (void *)my_lsmsave);
    } else logf("0.15: lsmSaveFile bytes differ, co-op save not installed");
    logf("0.15: admin_console=%d sync_quests=%d sync_reputation=%d guest_profile=%d host_coop_save=%d | save hook %s, mission %p/%p, rank %p, inv %p/%p/%p",
         g_admin_on, g_ms_on, g_rank_on, g_prof_on, g_save_on, g_lsm_body ? "on" : "off", (void *)g_ms_save, (void *)g_ms_load,
         (void *)g_setrank, (void *)g_invitem, (void *)g_invcount, (void *)g_invadd);
    if (g_admin_on) CreateThread(NULL, 0, admin_console_thread, NULL, 0, NULL);
}

static void tick_015(BYTE *g, int host, int ready)
{
    static DWORD ready_at, last_prof; static int restored; DWORD now = GetTickCount();
    if (!g_inited15) return;
    if (ready && !ready_at) ready_at = now;
    if (host && ready && g_sync != INVALID_SOCKET) {
        if (g_rank_on) rank_host_tick(g);
        if (g_ms_on) ms_host_tick();
    }
    if (!host && ready && g_prof_on && local_player()) {
        if (!restored && now - ready_at > 8000) { restored = 1; profile_load(); last_prof = now; }
        if (restored && now - last_prof > 120000) { last_prof = now; profile_save("every 2 minutes"); }
    }
    admin_tick(ready);
}

/* ---- 0.15.1 guest: taxi and other "use" actions on host-run objects ----
 * t_player::StateManual (+0x73B30..) takes the pointed pid (+0x1A138), needs sv_ent[pid] (je +0x73B51 ->
 * +0x74075 when NULL), then switches on the object's vcall +0x7C (0..15; 15 = taxi stand ->
 * msgOpenMapTaxi +0xDEE0). On the guest, host-run objects are cl-only, so pressing use did nothing. The
 * NULL branch now goes through guest_use_nosv first: taxi -> open the taxi map here. Picking a destination
 * sends 0x808B to gameMsgHandler -> taxiMoveTo(smid) (+0xC87A0, call at +0x15334): it creates a taxi car,
 * charges money, loads the destination and moves the player. That is a single-player routine, so the
 * guest runs it with stMode 1 for the call (like guest-local vehicle use). */
static BYTE *g_sm_exit; static fn_v15 g_opentaxi; static BYTE *g_taxi_fn;
static void __cdecl guest_use_nosv(int pid)
{
    BYTE *e; int code; static int logs; char nm[5];
    if (*V.mode != 4 || pid <= 0 || pid >= 4096) return;
    e = (BYTE *)g_cl_ent[pid];
    if (!e || IsBadReadPtr(e, 0x40) || !(*(DWORD *)(e + 0x38) & 0x8000000)) return;
    code = (BYTE)((int (__fastcall *)(void *, void *))(*(void ***)e)[0x7C / 4])(e, NULL);
    tname(*(DWORD *)(e + 8), nm);
    if (logs++ < 30) logf("guest: use on host object %d (%s), action %d", pid, nm, code);
    if (code == 15 && g_opentaxi) { logf("guest: taxi stand -> opening the taxi map here"); g_opentaxi(); }
}
static __declspec(naked) void sm_nosv_stub(void)
{
    __asm {
        pushad
        push eax
        call guest_use_nosv
        add esp, 4
        popad
        jmp dword ptr [g_sm_exit]
    }
}
static void __cdecl my_taxi(DWORD smid)
{
    int m = *V.mode;
    logf("guest: taxi to map part %lu (run as single player for the trip)", smid);
    if (m == 4) *V.mode = 1;
    ((void (__cdecl *)(DWORD))g_taxi_fn)(smid);
    *V.mode = m;
    logf("guest: taxi trip done");
}
static void install_taxi(BYTE *g)
{
    BYTE *j = g + 0x73B51; DWORD old;
    if (!GetPrivateProfileIntA("coop", "client_use_taxi", 1, g_ini)) return;
    if (g[0xDEE0] == 0x83 && g[0xDEE1] == 0xEC && g[0xDEE2] == 0x38) g_opentaxi = (fn_v15)(g + 0xDEE0);
    if (j[0] == 0x0F && j[1] == 0x84 && j + 6 + *(int *)(j + 2) == g + 0x74075 && g_opentaxi) {
        g_sm_exit = g + 0x74075;
        VirtualProtect(j + 2, 4, PAGE_EXECUTE_READWRITE, &old);
        *(int *)(j + 2) = (int)((BYTE *)sm_nosv_stub - (j + 6));
        VirtualProtect(j + 2, 4, old, &old);
    } else logf("guest: StateManual bytes differ, taxi stand not hooked");
    if (g[0xC87A0] == 0x81 && g[0xC87A1] == 0xEC) {
        g_taxi_fn = g + 0xC87A0;
        if (!patch_call(g + 0x15334, g_taxi_fn, (void *)my_taxi)) { g_taxi_fn = NULL; logf("guest: taxi call site differs"); }
    }
    logf("guest: taxi support %s / %s", g_sm_exit ? "stand on" : "stand off", g_taxi_fn ? "trip on" : "trip off");
}

static int jmp_patch(BYTE *at, const BYTE *expect, int n, void *to, BYTE **tramp)
{
    DWORD old; BYTE *t;
    if (memcmp(at, expect, n)) return 0;
    if (tramp) {
        t = (BYTE *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(t, at, n); t[n] = 0xE9; *(int *)(t + n + 1) = (int)((at + n) - (t + n + 5));
        *tramp = t;
    }
    VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9; *(int *)(at + 1) = (int)((BYTE *)to - (at + 5));
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    return 1;
}

/* ---- 0.14.1 bullets: the bullet manager keeps a list of pids it can hit (game +0x7F56A4, bmEntityCreated
 * +0xA62B0), filled when SERVER entities are created, and every frame bmNewFrame takes each entry's position
 * from sv_ent (and drops the entry if sv_ent is empty). On the guest the host's NPCs exist only in cl_ent, so
 * they were never in the list and bullets went through them. Guest: those lookups read cl_ent, and the
 * host's NPCs/animals/vehicles are registered. */
typedef void (__cdecl *fn_bmcreated)(void *e);
static fn_bmcreated g_bmcreated;
static void *g_bmreg[4096];
static int g_bm_players;

static int patch_svent_read(BYTE *at, void **sv, void **cl)
{
    DWORD old; BYTE *imm;
    if (at[0] == 0xA1) imm = at + 1;
    else if (at[0] == 0x8B && (at[1] & 0xC7) == 0x05) imm = at + 2;
    else return 0;
    if (*(void ***)imm != sv) return 0;
    VirtualProtect(imm, 4, PAGE_EXECUTE_READWRITE, &old); *(void ***)imm = cl; VirtualProtect(imm, 4, old, &old);
    return 1;
}

static void guest_bm_register(void)
{
    static DWORD last; static int logged; DWORD now = GetTickCount(); int p, n = 0;
    if (!g_bmcreated || now - last < 1000) return;
    last = now;
    for (p = 1; p <= 0xC00; p++) {
        BYTE *c = (BYTE *)g_cl_ent[p]; DWORD t;
        if (!c || V.sv_ent[p]) { g_bmreg[p] = NULL; continue; }
        if (g_bmreg[p] == c || IsBadReadPtr(c, 0x2c)) continue;
        t = *(DWORD *)(c + 8);
        if (t == 'play' && !g_bm_players) continue;
        if (t != 'play' && t != 'mhtm' && t != 'ecar' && t != 'bird' && t != 'mdog' && t != 'fish' && t != 'mjgr' && t != 'esnk' && t != 'enpc') continue;
        g_bmcreated(c);
        g_bmreg[p] = c; n++;
    }
    if (n && logged++ < 10) logf("guest: %d host NPCs/animals/vehicles can now be hit by our bullets", n);
}

/* ---- 0.14.2: t_player::SetInfocusPID (the "what am I looking at" code, +0x6E28C and +0x6E2E3) uses the same
 * bullet trace, got the host's NPCs/corpses from it and read them from sv_ent (empty on the guest): crash at
 * SetInfocusPID+0x509 when walking up to a corpse. There the trace now ignores host-only entities. */
typedef int (__cdecl *fn_trace)(int pid, void *a, void *b, DWORD d);
static fn_trace o_trace;
static int __cdecl my_trace(int pid, void *a, void *b, DWORD d)
{
    int r = (short)o_trace(pid, a, b, d);
    if (*V.mode == 4 && r > 0 && r < 4096 && !V.sv_ent[r]) return 0;
    return r;
}

static int patch_call_keep(BYTE *at, void *to, void **orig)
{
    BYTE *t;
    if (at[0] != 0xE8) return 0;
    t = at + 5 + *(int *)(at + 1);
    if (*orig && *orig != t) return 0;
    *orig = t;
    return patch_call(at, t, to);
}

/* ---- 0.14.2: the guest never runs gtimeNewFrame (+0xAFF00), which hands the clock to Xenus (pipeline 0x80D2)
 * and sets the daylight level, so the guest's sky stayed at its start hour (14:00) while the game clock was
 * the host's. The guest now runs it every frame. ---- */
typedef void (__cdecl *fn_v)(void);
static fn_v g_gtime_frame;

/* ---- 0.14.1 host: does the game accept a guest's door click? (ParseReliable +0x198FB -> scenaryClickOnDoor) */
static fn_cdoor g_cdoor_host;
static int __cdecl host_cdoor(int id)
{
    static int logged; int r = g_cdoor_host(id);
    if (logged++ < 20) logf("host: door click (door id %d) %s", (short)id, r ? "accepted, the door opens" : "REFUSED by the game (locked or story door?)");
    return r;
}

/* ---- 0.14.1 both: the time/sky values Xenus really uses, every 30 s, to compare host and guest ---- */
static void time_log(BYTE *g)
{
    static DWORD last; DWORD now = GetTickCount(); BYTE *x = (BYTE *)GetModuleHandleA(NULL);
    if (now - last < 30000) return;
    last = now;
    logf("time: game seconds %.0f | Xenus AA=%u AC=%u AE=%u | sky %lu next %lu rain %u | light %.2f",
         *(float *)(g + GT_ELAPSED) * 10 + *(float *)(g + GT_START),
         *(WORD *)(x + 0x25941AA), *(WORD *)(x + 0x25941AC), *(WORD *)(x + 0x25941AE),
         *(DWORD *)(x + 0x25941B4), *(DWORD *)(x + 0x25941B8), x[0x25941CC], *(float *)(g + 0x116CDC));
}

static void install_014(BYTE *g, int host)
{
    static const BYTE gpr[] = { 0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x4C, 0x24, 0x04 };
    static const BYTE vu[] = { 0x83, 0xEC, 0x50, 0x56, 0x8D, 0x44, 0x24, 0x14 };
    g_parse_rel = g + 0x18810;
    logf("0.14: message log %s", jmp_patch(g + 0x15000, gpr, sizeof gpr, (void *)my_gpr, NULL) ? "on" : "NOT installed (bytes differ)");
    g_respawn_on = GetPrivateProfileIntA("coop", "respawn", 1, g_ini);
    if (host) {
        g_cdoor_host = (fn_cdoor)(g + 0xC3EF0);
        if (g[0xC3EF0] == 0x66) logf("host: door click log %s", patch_call(g + 0x198FB, g + 0xC3EF0, (void *)host_cdoor) ? "on" : "not installed");
        o_bmshot = (fn_bmshot)(g + 0xA5430);
        logf("host: guest hits %s", patch_call(g + 0x18F4D, g + 0xA5430, (void *)my_bmshot) ? "logged, guest player counts as a player" : "hook not installed");
        return;
    }
    if (GetPrivateProfileIntA("coop", "client_shoot_host_npcs", 1, g_ini)) {
        void **sv = find_iat((HMODULE)g, "vital.dll", "?sv_ent@@3PAPAVt_entity@@A");
        void **cl = find_iat((HMODULE)g, "vital.dll", "?cl_ent@@3PAPAVt_entity@@A");
        BYTE *a = g + 0xA47DB, *b = g + 0xA6191; DWORD old; int ok = 0;
        g_shotobj = g + 0xA61D0;
        if (sv && cl && a[0] == 0x8B && a[1] == 0x0D && *(void ***)(a + 2) == sv && b[0] == 0x8B && b[1] == 0x15 && *(void ***)(b + 2) == sv) {
            VirtualProtect(a, 6, PAGE_EXECUTE_READWRITE, &old); *(void ***)(a + 2) = cl; VirtualProtect(a, 6, old, &old);
            VirtualProtect(b, 6, PAGE_EXECUTE_READWRITE, &old); *(void ***)(b + 2) = cl; VirtualProtect(b, 6, old, &old);
            ok = patch_call(g + 0x1E495, g_shotobj, (void *)my_shotobj) + patch_call(g + 0xA47F8, g_shotobj, (void *)my_shotobj)
               + patch_call(g + 0xA61A7, g_shotobj, (void *)my_shotobj);
        }
        logf("guest: bullet hits on the host's NPCs are reported to the host (%d of 3 call sites)", ok);
        if (sv && cl && ok) {
            static const DWORD sites[] = { 0xA4E5D, 0xA5FDC, 0xA60C8, 0xA686E };
            int i, n = 0;
            for (i = 0; i < 4; i++) n += patch_svent_read(g + sites[i], sv, cl);
            g_bmcreated = (fn_bmcreated)(g + 0xA62B0);
            g_bm_players = GetPrivateProfileIntA("coop", "friendly_fire", 0, g_ini);
            logf("guest: bullet traces use the host's entities (%d of 4 lookups patched)", n);
            n = patch_call_keep(g + 0x6E28C, (void *)my_trace, (void **)&o_trace) + patch_call_keep(g + 0x6E2E3, (void *)my_trace, (void **)&o_trace);
            logf("guest: look-at trace guarded (%d of 2)", n);
            if (n != 2) { g_bmcreated = NULL; logf("guest: look-at guard missing, host NPCs NOT made hittable (would crash)"); }
        }
    }
    if (GetPrivateProfileIntA("coop", "client_use_vehicles", 1, g_ini))
        logf("guest: own vehicles usable %s", jmp_patch(g + 0xCD8C0, vu, sizeof vu, (void *)my_vehuse, &g_vehuse_tramp) ? "(VehicleUse hooked)" : "- NOT installed (bytes differ)");
}

/* 0.15.1: which actors are NOT shared. Every 30 s (10 times): per type, how many this machine runs itself
 * (sv), with and without the "networked" bit (+0x39 & 0x80 - without it the host never sends them), and how
 * many it only has as the other side's copy (cl-only). */
static void npc_census(int host)
{
    static DWORD last; static int runs; DWORD now = GetTickCount();
    DWORD ty[48]; int own[48], ownloc[48], remote[48], n = 0, p, k; char line[1500], nm[5]; int o = 0;
    if (runs >= 10 || now - last < 30000) return;
    last = now; runs++;
    for (p = 1; p < 4096; p++) {
        BYTE *sv = (BYTE *)V.sv_ent[p], *cl = (BYTE *)g_cl_ent[p], *e = sv ? sv : cl; DWORD t;
        if (!e || IsBadReadPtr(e, 0x40)) continue;
        t = *(DWORD *)(e + 8);
        if (!dedupe_type(t) && t != 0x706C6179) continue;
        for (k = 0; k < n; k++) if (ty[k] == t) break;
        if (k == n) { if (n == 48) continue; ty[n] = t; own[n] = ownloc[n] = remote[n] = 0; n++; }
        if (sv) { if (sv[0x39] & 0x80) own[k]++; else ownloc[k]++; } else remote[k]++;
    }
    for (k = 0; k < n && o < (int)sizeof line - 40; k++) { tname(ty[k], nm); o += _snprintf(line + o, sizeof line - o - 1, " %s:%d/%d/%d", nm, own[k], ownloc[k], remote[k]); }
    line[o] = 0;
    logf("npcs: %s census (type: own networked / own not networked / other side's copy):%s", host ? "host" : "guest", line);
}

static void world_sync_tick(void)
{
    static DWORD ready_at; BYTE *g = (BYTE *)GetModuleHandleA("game.dll");
    int host = *V.mode == 3, ready;
    if (!g) return;
    if (g_sync_on < 0) {
        HMODULE v = GetModuleHandleA("vital.dll");
        BYTE *cw = (BYTE *)GetProcAddress((HMODULE)g, "?CreateWeapon@t_player@@QAEXXZ");
        g_sync_on = GetPrivateProfileIntA("coop", "sync_clock", 1, g_ini);
        g_sync_port = GetPrivateProfileIntA("coop", "sync_port", 15013, g_ini);
        g_dedupe_on = GetPrivateProfileIntA("coop", "client_remove_duplicates", 1, g_ini);
        g_dedupe_veh = GetPrivateProfileIntA("coop", "client_remove_duplicate_vehicles", 0, g_ini);
        g_sync_weather = GetPrivateProfileIntA("coop", "sync_weather", 1, g_ini);
        g_click_map = GetPrivateProfileIntA("coop", "client_use_doors", 1, g_ini);
        if (!host && g_click_map) {
            g_cdoor = (fn_cdoor)GetProcAddress((HMODULE)g, "?scenaryClickOnDoor@@YAHF@Z");
            g_cbtn = (fn_cbtn)GetProcAddress((HMODULE)g, "?scenaryClickOnButton@@YAHFF@Z");
            if (!g_cdoor && g[0xC3EF0] == 0x66) g_cdoor = (fn_cdoor)(g + 0xC3EF0);
            if (!g_cbtn && g[0xC3F20] == 0x83) g_cbtn = (fn_cbtn)(g + 0xC3F20);
            o_click = (fn_click)(g + 0x8700);
            if (patch_call(g + 0x73D98, g + 0x8700, (void *)my_click) && patch_call(g + 0x73DEF, g + 0x8700, (void *)my_click))
                logf("guest: doors/buttons open here and on the host (door %p button %p)", (void *)g_cdoor, (void *)g_cbtn);
            else logf("guest: click call sites differ, door sync not installed");
        }
        g_weapon_fix = GetPrivateProfileIntA("coop", "host_guest_weapon", 1, g_ini);
        g_enremove = (fn_enremove)GetProcAddress(v, "EN_RemoveFromScene");
        if (!g_enremove) g_enremove = (fn_enremove)GetProcAddress(v, "?EN_RemoveFromScene@@YAXPAVt_entity@@@Z");
        if (!cw) cw = g + 0x6C430;
        if (cw[0] == 0x53 && cw[1] == 0x55 && cw[2] == 0x56 && cw[3] == 0x57 && cw[4] == 0x8B && cw[5] == 0xF9) g_createweapon = (fn_this0)cw;
        logf("sync: clock=%d port=%d remove_duplicates=%d (vehicles %d) guest_weapon=%d | EN_RemoveFromScene %p CreateWeapon %p",
             g_sync_on, g_sync_port, g_dedupe_on, g_dedupe_veh, g_weapon_fix, (void *)g_enremove, (void *)g_createweapon);
        if (g_sync_on) sync_open(host);
        install_014(g, host);
        install_015(g, host);
    }
    ready = host || *((BYTE *)GetModuleHandleA(NULL) + 0xDE3928);
    if (ready && !ready_at) ready_at = GetTickCount();
    if (g_sync_on > 0 && g_sync != INVALID_SOCKET) { if (host) sync_host_tick(g); else sync_guest_tick(g, ready); }
    if (!g_cl_ent) return;
    if (ready) weapon_check(host);
    if (host) respawn_tick(g);
    else if (ready && g_respawn_on) guest_death_tick();
    if (!host && ready && g_sync_on > 0) {
        if (!g_gtime_frame && g[0xAFF00] == 0x8B && g[0xAFF01] == 0x0D) { g_gtime_frame = (fn_v)(g + 0xAFF00); logf("guest: runs gtimeNewFrame (time of day to Xenus)"); }
        if (g_gtime_frame) g_gtime_frame();
    }
    if (ready) time_log(g);
    if (!host && ready) guest_bm_register();
    if (!host && ready && g_click_map && g_cdoor) guest_door_mirror();
    tick_015(g, host, ready);
    if (ready) npc_census(host);
    if (!host && ready) {
        static DWORD last; DWORD now = GetTickCount();
        if (now - last >= 2000) { last = now; guest_dedupe(now - ready_at); }
    }
}
/* ---- map streaming for every player. game.dll mapNewFrame (+0xB48E0) loads the map parts (static meshes)
 * in a 40000-unit box around clGetFocus(4) - the LOCAL player only - and unloads parts too far from it
 * (mapUnloadSMID, SM id in EAX, single call at +0xB4E12). A remote player outside that area stands in
 * unloaded geometry: the host crashed in clUpdateRoomLinkFor/GetPortalList, the guest stalled in the
 * renderer once the host's NPCs arrived. Fix: after the normal pass, run mapNewFrame again with the
 * focus pointed at each remote player, and never unload a part that is near ANY player. ---- */
#define MNF_FUNC  0xB48E0
#define MNF_CALL1 0x1364C
#define MNF_CALL2 0xB36FF
#define UNL_CALL  0xB4E12
#define UNL_FUNC  0xB7230
#define MAP_THROTTLE 0x7F5794   /* float: last time the pass ran (every 0.1 s) */
#define MAP_LASTPOS  0x28EFA8   /* float[3] (0.12.1 had 0x228EFA8 = outside game.dll -> host crash) */
#define MAP_LASTDIR  0x2927B8   /* float[3] */
#define MAP_MISSION  0x7F57A8   /* active mission SM id, written by the pass */
typedef int (__cdecl *fn_getfocus)(int);
typedef void (__cdecl *fn_wldbbox)(int, float *, float *);
static fn_void o_mapnewframe;
static DWORD o_unload;
static fn_getfocus o_clgetfocus;
static fn_wldbbox g_smbbox;
static float *g_worigin;
static int g_focus_override, g_pl_n, g_remote[3], g_remote_n, g_unload_vetoes;
static float g_pl_pos[4][3];

static int __cdecl my_clgetfocus(int slot)
{
    if (slot == 4 && g_focus_override) return g_focus_override;
    return o_clgetfocus(slot);
}

static void stream_players_update(void)
{
    /* local = any of our own focus pids (player, camera, vehicle): in a car clGetFocus(4) is the car, and
     * 0.12.1 then took the host's own player for a remote one */
    static const int fs[4] = { 10, 0, 1, 4 };
    int local[4], p, k, mine;
    g_pl_n = 0; g_remote_n = 0;
    if (!g_cl_ent || !o_clgetfocus) return;
    for (k = 0; k < 4; k++) local[k] = o_clgetfocus(fs[k]);
    for (p = 1; p < 4096 && g_pl_n < 4; p++) {
        BYTE *e = (BYTE *)g_cl_ent[p];
        if (!e || IsBadReadPtr(e, 0x2c) || !is_play(e)) continue;
        for (mine = 0, k = 0; k < 4; k++) if (local[k] == p) mine = 1;
        g_pl_pos[g_pl_n][0] = *(float *)(e + 0x20) + g_worigin[0];
        g_pl_pos[g_pl_n][1] = *(float *)(e + 0x24) + g_worigin[1];
        g_pl_pos[g_pl_n][2] = *(float *)(e + 0x28) + g_worigin[2];
        g_pl_n++;
        if (!mine && g_remote_n < 3) g_remote[g_remote_n++] = p;
    }
}

static void __cdecl map_frame_wrap(void)
{
    BYTE *g = (BYTE *)GetModuleHandleA("game.dll");
    int i; static int logged; static DWORD last_list, last_pass; DWORD now = GetTickCount();
    /* 0.14.5: the player scan (4096 entries) and the extra full streaming pass per remote player ran EVERY
     * frame (throttle forced to -1e9) - the guest's framerate drop. Now: list every 0.5 s, pass every 0.3 s. */
    if (now - last_list >= 500) { last_list = now; stream_players_update(); }
    o_mapnewframe();
    if (!g_remote_n || now - last_pass < 300) return;
    last_pass = now;
    {
        float thr = *(float *)(g + MAP_THROTTLE), pos[3], dir[3];
        DWORD mis = *(DWORD *)(g + MAP_MISSION);
        memcpy(pos, g + MAP_LASTPOS, 12); memcpy(dir, g + MAP_LASTDIR, 12);
        for (i = 0; i < g_remote_n; i++) {
            BYTE *e = (BYTE *)g_cl_ent[g_remote[i]];
            if (!e || IsBadReadPtr(e, 0x2c) || !is_play(e)) continue;
            *(float *)(g + MAP_THROTTLE) = -1e9f;   /* let the pass run now */
            g_focus_override = g_remote[i];
            o_mapnewframe();
            g_focus_override = 0;
            if (logged++ < 3) logf("streaming: also loading map parts around remote player pid %d", g_remote[i]);
        }
        *(float *)(g + MAP_THROTTLE) = thr;
        memcpy(g + MAP_LASTPOS, pos, 12); memcpy(g + MAP_LASTDIR, dir, 12);
        *(DWORD *)(g + MAP_MISSION) = mis;
        o_mapnewframe();   /* throttled, so this only puts the sky back for the local player */
        memcpy(g + MAP_LASTPOS, pos, 12); memcpy(g + MAP_LASTDIR, dir, 12);
    }
}

static int __cdecl unload_veto(int smid)
{
    float mn[3], mx[3], d, dx, dz; int i;
    if (g_pl_n < 2 || !g_smbbox) return 0;
    g_smbbox(smid, mn, mx);
    for (i = 0; i < g_pl_n; i++) {
        dx = g_pl_pos[i][0] < mn[0] ? mn[0] - g_pl_pos[i][0] : (g_pl_pos[i][0] > mx[0] ? g_pl_pos[i][0] - mx[0] : 0);
        dz = g_pl_pos[i][2] < mn[2] ? mn[2] - g_pl_pos[i][2] : (g_pl_pos[i][2] > mx[2] ? g_pl_pos[i][2] - mx[2] : 0);
        d = dx * dx + dz * dz;
        if (d < 40000.0f * 40000.0f) {
            if (g_unload_vetoes++ < 10) logf("streaming: kept map part %d loaded (player %d is near it)", smid, i);
            return 1;
        }
    }
    return 0;
}

__declspec(naked) static void unload_stub(void)
{
    __asm {
        pushad
        push eax
        call unload_veto
        add esp, 4
        test eax, eax
        popad
        jnz keep
        jmp dword ptr [o_unload]
    keep:
        ret
    }
}

static int patch_call(BYTE *at, BYTE *expect_target, void *to)
{
    DWORD old;
    if (at[0] != 0xE8 || at + 5 + *(int *)(at + 1) != expect_target) return 0;
    VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int *)(at + 1) = (int)((BYTE *)to - (at + 5));
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    return 1;
}

static void hook_streaming(HMODULE game)
{
    BYTE *g = (BYTE *)game; HMODULE v = GetModuleHandleA("vital.dll");
    void **slot = find_iat(game, "vital.dll", "?clGetFocus@@YAHH@Z");
    if (!slot) slot = find_iat(game, "vital.dll", "clGetFocus");
    if (!slot) slot = find_iat_ord(game, "vital.dll", 15);   /* imported by ordinal 15 */
    g_smbbox = (fn_wldbbox)GetProcAddress(v, "?wldGetSmBBox@@YAXHQAM0@Z");
    g_worigin = (float *)GetProcAddress(v, "?WindowOrigin@@3PAMA");
    if (!g_cl_ent) g_cl_ent = (void ***)GetProcAddress(v, "?cl_ent@@3PAPAVt_entity@@A");
    if (!slot || !g_smbbox || !g_worigin || !g_cl_ent) { logf("streaming: missing slot=%p bbox=%p origin=%p cl_ent=%p, not installed", (void *)slot, (void *)g_smbbox, (void *)g_worigin, (void *)g_cl_ent); return; }
    o_mapnewframe = (fn_void)(g + MNF_FUNC);
    o_unload = (DWORD)(g + UNL_FUNC);
    if (!patch_call(g + UNL_CALL, g + UNL_FUNC, (void *)unload_stub)) { logf("streaming: bytes differ, not installed"); return; }
    if (!vital_load() || !V.clfocus) { logf("streaming: vital exports not ready, not installed"); return; }
    if (*slot != (void *)V.clfocus) { logf("streaming: import slot %p holds %p, not clGetFocus %p, not installed", (void *)slot, *slot, (void *)V.clfocus); return; }
    o_clgetfocus = (fn_getfocus)patch_iat(slot, (void *)my_clgetfocus);
    if (!patch_call(g + MNF_CALL1, g + MNF_FUNC, (void *)map_frame_wrap)) logf("streaming: call site 1 differs");
    if (!patch_call(g + MNF_CALL2, g + MNF_FUNC, (void *)map_frame_wrap)) logf("streaming: call site 2 differs");
    logf("streaming: map parts are loaded around every player, not only the local one");
}

/* ---- host: reject entity creates from guests. clNetworkFrame (vital) at +0x3BA00 does
 * "cmp cl_ent[pid],0 / je create" and the server (stMode 3) then also forwards a guest's create to the other
 * machines. Guests' own entities come from the guest's local world (cars, NPCs, breakables) and became
 * ghost entities on the host (and crashed it). The guest's player is created by the server itself, so a
 * guest never needs to create anything here. The engine's own "already exists" path (+0x3BA0A) cannot be
 * used: after skipping the 11-bit create block it reads more data through the EXISTING entity (vcall +0x4C)
 * and crashed on NULL (0.10). A create also carries unsized class data, so the rest of that packet is
 * dropped (netguard drop path +0x3BCAC, same stack depth). ---- */
#define CC_SITE 0x3BA00
#define CC_SKIP 0x3BA0A
#define CC_NEW  0x3BA22
static DWORD g_cc_skip, g_cc_new;
static int g_cc_rejected;

static int __cdecl create_check(int pid, int from)
{
    static int allowed;
    if (!g_stmode || *g_stmode != 3 || from < 2 || from >= 31) return 0;
    /* the guest's OWN character: gameSvAttach only sends 's' with a pid; the GUEST creates the character
     * with that pid and owns it, so the host has no sv_ent for it. Match the pid in game.dll's player table
     * (+0x100). Without this the host never sees the guest (0.10.1/0.10.2). */
    if (pid > 0 && pid < 4096 && vital_load() &&
        (*(short *)(player_info(from) + 0x100) == pid || is_play(V.sv_ent[pid]))) {
        if (allowed++ < 5) logf("host: allowed guest machine %d to create its own player (pid %d)", from, pid);
        return 0;
    }
    g_cc_rejected++;
    if (g_cc_rejected <= 20 || g_cc_rejected % 500 == 0)
        logf("host: rejected entity create from guest machine %d (pid %d), total %d", from, pid, g_cc_rejected);
    return 1;
}

__declspec(naked) static void create_stub(void)
{
    __asm {
        pushad
        pushfd
        mov eax, dword ptr [esp + 36 + 0x14]
        push eax
        push ebx
        call create_check
        add esp, 8
        test eax, eax
        jnz skip
        popfd
        popad
        push eax
        mov eax, g_cl_ent
        cmp dword ptr [eax + ebx*4], 0
        pop eax
        je make
        jmp dword ptr [g_cc_skip]
    make:
        jmp dword ptr [g_cc_new]
    skip:
        popfd
        popad
        jmp dword ptr [g_ng_drop]
    }
}

static void hook_create_check(void)
{
    BYTE *v = (BYTE *)GetModuleHandleA("vital.dll"), *p; DWORD old;
    if (!v || !g_cl_ent || !g_ng_drop) { logf("create check: netguard not installed, not installed either"); return; }
    p = v + CC_SITE;
    if (p[0] != 0x83 || p[1] != 0x3C || p[2] != 0x9D || *(DWORD *)(p + 3) != (DWORD)g_cl_ent || p[7] != 0 ||
        p[8] != 0x74 || p + 10 + (signed char)p[9] != v + CC_NEW) { logf("create check: bytes differ, not installed"); return; }
    g_cc_skip = (DWORD)(v + CC_SKIP); g_cc_new = (DWORD)(v + CC_NEW);
    jmp_hook(p, (void *)create_stub);
    VirtualProtect(p + 5, 5, PAGE_EXECUTE_READWRITE, &old); memset(p + 5, 0x90, 5); VirtualProtect(p + 5, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 10);
    logf("host: guest entity creates are rejected (no ghost entities)");
}

/* ---- client: entities the client creates itself (map objects via t_entity::read, NPC spawners, sun...)
 * call EN_GetFreePID(2) = a GLOBAL pid (1..0xC00). Global pids belong to the server; when the server's create
 * for the same pid arrives, clNetworkFrame skips it because the slot is taken. That is why the guest never
 * saw the host's player or the host's NPCs. In stMode 4 we hand out LOCAL pids (0xC01..0xFFE) instead; if
 * the local range is full we fall back to the original. Entry bytes: 8B 44 24 04 85 C0 75 0D. ---- */
typedef long (__cdecl *fn_freepid)(int);
static DWORD g_fp_back;
static int g_fp_local, g_fp_fallback;

__declspec(naked) static long __cdecl freepid_orig(int type)
{
    (void)type;
    __asm {
        mov eax, dword ptr [esp + 4]
        test eax, eax
        jmp dword ptr [g_fp_back]
    }
}

static long __cdecl freepid_hook(int type)
{
    if (type == 2 && g_stmode && *g_stmode == 4) {
        long r = freepid_orig(1);
        if (r) { g_fp_local++; return r; }
        if (g_fp_fallback++ < 5) logf("client: local pid range full, using a global pid");
    }
    return freepid_orig(type);
}

static void hook_freepid(void)
{
    static const BYTE want[] = { 0x8B,0x44,0x24,0x04,0x85,0xC0,0x75,0x0D };
    HMODULE vm = GetModuleHandleA("vital.dll");
    BYTE *p = vm ? (BYTE *)GetProcAddress(vm, "EN_GetFreePID") : NULL;
    if (!g_stmode && vm) g_stmode = (int *)GetProcAddress(vm, "?stMode@@3HA");
    if (!p || !g_stmode) { logf("client local pids: EN_GetFreePID not found, not installed"); return; }
    if (memcmp(p, want, sizeof want)) { logf("client local pids: bytes differ, not installed"); return; }
    g_fp_back = (DWORD)(p + 6);
    jmp_hook(p, (void *)freepid_hook);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    logf("client: its own entities get local pids (server entities can now be created on the client)");
}

/* ---- probe: samples the main thread every 2 s to see where a stuck load is waiting ---- */
static DWORD g_main_tid;

static void addr_name(DWORD a, char *out)
{
    HMODULE m = NULL; char p[MAX_PATH], *s;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, p, MAX_PATH); s = strrchr(p, 92);
        sprintf(out, "%s+%lX", s ? s + 1 : p, a - (DWORD)m);
    } else sprintf(out, "%08lX", a);
}

static DWORD WINAPI probe_thread(LPVOID u)
{
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_main_tid);
    for (;;) {
        CONTEXT c; char line[4096], nm[128]; DWORD *ebp; int i;
        BYTE *vit = (BYTE *)GetModuleHandleA("vital.dll");
        Sleep(2000);
        if (!th) { logf("probe: cannot open main thread"); return 0; }
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        SuspendThread(th);
        if (!GetThreadContext(th, &c)) { ResumeThread(th); continue; }
        addr_name(c.Eip, nm); sprintf(line, "probe: eip=%s", nm);
        ebp = (DWORD *)c.Ebp;
        for (i = 0; i < 14 && ebp; i++) {
            DWORD ret, *next;
            if (IsBadReadPtr(ebp, 8)) break;
            next = (DWORD *)ebp[0]; ret = ebp[1];
            addr_name(ret, nm); strcat(line, " < "); strcat(line, nm);
            if (next <= ebp) break;
            ebp = next;
        }
        ResumeThread(th);
        if (vit) {
            char st[96];
            sprintf(st, " | attached=%d v8988=%d", *(int *)(vit + 0xAB977C), *(int *)(vit + 0xAB8988));
            strcat(line, st);
        }
        if (vital_load()) {
            /* world state: what the local camera/controls point at, and which entities exist */
            static const int slot[4] = { 10, 0, 1, 4 };
            char st[160]; int k, p, n = 0; DWORD t;
            if (!g_cl_ent) { logf("%s", line); continue; }
            sprintf(st, " | stMode=%d", *V.mode); strcat(line, st);
            for (k = 0; k < 4; k++) {
                int f = V.clfocus(slot[k]);
                BYTE *ce = (f > 0 && f < 4096) ? (BYTE *)g_cl_ent[f] : NULL, *se = (f > 0 && f < 4096) ? (BYTE *)V.sv_ent[f] : NULL;
                DWORD ct = ce && !IsBadReadPtr(ce, 12) ? *(DWORD *)(ce + 8) : 0, stt = se && !IsBadReadPtr(se, 12) ? *(DWORD *)(se + 8) : 0;
                sprintf(st, " | focus%d=%d cl:%.4s sv:%.4s", slot[k], f, ct ? (char *)&ct : "-", stt ? (char *)&stt : "-");
                strcat(line, st);
            }
            strcat(line, " | players:");
            for (p = 1; p < 4096 && n < 8; p++) {
                BYTE *e = (BYTE *)g_cl_ent[p];
                if (!e || IsBadReadPtr(e, 0x1a8)) continue;
                t = *(DWORD *)(e + 8);
                if (t == 0x706c6179 || (*(int *)(e + 0x1a4) >= 2 && *(int *)(e + 0x1a4) < 31)) {
                    sprintf(st, " cl[%d]=%.4s/o%d", p, (char *)&t, *(int *)(e + 0x1a4)); strcat(line, st); n++;
                }
            }
            for (p = 1, n = 0; p < 4096 && n < 4; p++) {
                BYTE *e = (BYTE *)V.sv_ent[p];
                if (!e || IsBadReadPtr(e, 12)) continue;
                t = *(DWORD *)(e + 8);
                if (t == 0x706c6179) { sprintf(st, " sv[%d]=play", p); strcat(line, st); n++; }
            }
            {   /* sync diagnostics: entity counts per range; on the host, how many networked entities are
                 * queued for machine 2 (column 1): bits==0 none, bit2 blocked, else pending/sent */
                int cg = 0, clc = 0, sg = 0, sl = 0, net = 0, b0 = 0, b2 = 0, bok = 0;
                for (p = 1; p < 4096; p++) {
                    BYTE *e = (BYTE *)V.sv_ent[p];
                    if (g_cl_ent[p]) { if (p <= 0xC00) cg++; else clc++; }
                    if (!e) continue;
                    if (p <= 0xC00) sg++; else sl++;
                    if (!IsBadReadPtr(e, 0x1a8) && (e[0x39] & 0x80) && *(int *)(e + 0x1a4) == 0 && p <= 0xC00) {
                        BYTE b = V.sv_bits[p * 31 + 1];
                        net++; if (!b) b0++; else if (b & 2) b2++; else bok++;
                    }
                }
                sprintf(st, " | ents cl:%d+%d sv:%d+%d | net:%d m2col[none %d, blocked %d, ok %d]", cg, clc, sg, sl, net, b0, b2, bok);
                strcat(line, st);
                if (V.sv_ent[101] && !IsBadReadPtr(V.sv_ent[101], 0x1a8)) {
                    BYTE *e = (BYTE *)V.sv_ent[101];
                    sprintf(st, " | p101 f39=%02x own=%d bits=%02x %02x %02x", e[0x39], *(int *)(e + 0x1a4),
                            V.sv_bits[101 * 31], V.sv_bits[101 * 31 + 1], V.sv_bits[101 * 31 + 2]);
                    strcat(line, st);
                }
            }
        }
        logf("%s", line);
    }
}

HRESULT WINAPI DirectInputCreateA(HINSTANCE h, DWORD v, LPVOID *o, LPUNKNOWN u)
{
    load_real();
    install_hooks();
    if (!g_real_create) return E_FAIL;
    return g_real_create(h, v, o, u);
}

static void read_config(void)
{
    char *s;
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    s = strrchr(g_dir, 92); if (s) s[1] = 0;
    sprintf(g_ini, "%sbp_coop.ini", g_dir);
    GetPrivateProfileStringA("coop", "mode", "off", g_mode, sizeof g_mode, g_ini);
    GetPrivateProfileStringA("coop", "host_ip", "127.0.0.1", g_ip, sizeof g_ip, g_ini);
    GetPrivateProfileStringA("coop", "name", "PLAYER", g_name, sizeof g_name, g_ini);
    g_maxplayers = GetPrivateProfileIntA("coop", "max_players", 4, g_ini);
    g_two = GetPrivateProfileIntA("coop", "allow_two_instances", 0, g_ini);
    g_keep_ctf = GetPrivateProfileIntA("coop", "keep_ctf_mode", 0, g_ini);
    g_spawn_near_host = GetPrivateProfileIntA("coop", "spawn_near_host", 0, g_ini);
    /* mode=auto (testing on one PC): the first copy hosts, the second joins 127.0.0.1 */
    if (!_stricmp(g_mode, "auto")) {
        HANDLE m = CreateMutexA(NULL, FALSE, "BPCoopHostRunning");
        if (m && GetLastError() == ERROR_ALREADY_EXISTS) {
            strcpy(g_mode, "join"); strcpy(g_ip, "127.0.0.1"); strcpy(g_name, "GUEST");
        } else {
            strcpy(g_mode, "host"); strcpy(g_name, "HOST");
        }
        g_two = 1;
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r)
{
    if (why == DLL_PROCESS_ATTACH) {
        char lp[MAX_PATH];
        DisableThreadLibraryCalls(h);
        read_config();
        sprintf(lp, "%sCoopTest", g_dir); CreateDirectoryA(lp, NULL);
        sprintf(lp, "%sCoopTest\\bp_coop_%lu.log", g_dir, GetCurrentProcessId());
        g_log = fopen(lp, "w");
        logf("bp_coop 0.15.1 loaded, pid %lu. mode=%s host_ip=%s name=%s max_players=%d two_instances=%d",
             GetCurrentProcessId(), g_mode, g_ip, g_name, g_maxplayers, g_two);
        install_hooks();
        g_main_tid = GetCurrentThreadId();
        if (GetPrivateProfileIntA("coop", "log_console", 0, g_ini)) hook_console();
        if (_stricmp(g_mode, "off") && GetPrivateProfileIntA("coop", "net_guard", 1, g_ini)) hook_netguard();
        if (!_stricmp(g_mode, "host") && GetPrivateProfileIntA("coop", "host_reject_guest_creates", 1, g_ini)) hook_create_check();
        if (!_stricmp(g_mode, "join") && GetPrivateProfileIntA("coop", "client_local_pids", 1, g_ini)) hook_freepid();
        if (!_stricmp(g_mode, "host")) CreateThread(NULL, 0, hotkey_thread, NULL, 0, NULL);
        if (GetPrivateProfileIntA("coop", "probe", 0, g_ini))
            CreateThread(NULL, 0, probe_thread, NULL, 0, NULL);
    } else if (why == DLL_PROCESS_DETACH) {
        /* who is exiting: scan our stack for return addresses into game modules */
        DWORD *sp = (DWORD *)&h, a; int i, n = 0; char line[2048], nm[128];
        sprintf(line, "process exit (%s), stack:", r ? "ExitProcess" : "FreeLibrary");
        for (i = 0; i < 8192 && n < 24; i++) {
            if (IsBadReadPtr(sp + i, 4)) break;
            a = sp[i];
            {   HMODULE m = NULL; char p[MAX_PATH], *b;
                if (a < 0x10000 || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) || !m) continue;
                GetModuleFileNameA(m, p, MAX_PATH); b = strrchr(p, 92); b = b ? b + 1 : p;
                if (_stricmp(b, "Xenus.exe") && _stricmp(b, "game.dll") && _stricmp(b, "vital.dll")) continue;
                addr_name(a, nm); strcat(line, " "); strcat(line, nm); n++;
            }
        }
        logf("%s", line);
    }
    return TRUE;
}
