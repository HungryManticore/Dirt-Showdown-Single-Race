/*
 * DiRT Showdown - Split Screen -> Single Race  (SplitToSingle.asi)
 *
 * Loaded by Ultimate ASI Loader (dinput8.dll). Supports both executables
 * shipped with the Steam build (showdown.exe and showdown_avx.exe, both
 * built 10 Oct 2012).
 *
 * What it does (each step can be toggled in SplitToSingle.ini):
 *
 *  1. SkipP2PressStart : the front-end flow link
 *         node "2m0" (frame_delay_waypoint_1) --next--> "2m1" (player_two_press_start)
 *     is redirected to "2m2" (set_menu_area_splitscreen), so the
 *     "Player 2 press START" overlay never appears and player 2 is never
 *     bound to a controller (i.e. P2 exists only as a controller-less slot).
 *
 *  2. SkipP2CarSelect  : after player 1 picks a car/livery the flow would go to
 *     node "I4" (player 2's vehicle_select). Links
 *         "I3" (livery_select_setup) --skip_livery_select--> "I4"
 *         "Ls" (vehicle_livery_select) --next--> "I4"
 *     are redirected to "I6" (fade_to_loading_delay), i.e. straight to loading.
 *
 *  3. RemoveP2         : the split-screen career builds its human entrants by
 *     looping over every PlayerManager player (main + secondary). The loop
 *     count is forced to 1 and the player fetched is always the MainPlayer, so
 *     the race is built with a single human; player 2's car does not exist.
 *     (AI entrants are added by the game as usual.)
 *
 *  4. SingleScreen     : while the race runs, the current event's "is split
 *     screen" flag (event+0xA4) is cleared (and restored afterwards), the
 *     StateSplitscreenDecision flow state always answers "false", and any
 *     request for the SplitScreen rendering mode is turned into MainGame.
 *     Net effect: one full-screen viewport, normal single-player HUD/flow.
 *
 * Everything is logged to SplitToSingle.log next to the .asi.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            BOOL;
typedef void*          HANDLE;

#define WINAPI __stdcall
#define IMP __declspec(dllimport)

typedef struct { u16 wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;

IMP void*  WINAPI GetModuleHandleA(const char*);
IMP BOOL   WINAPI VirtualProtect(void*, u32, u32, u32*);
IMP BOOL   WINAPI FlushInstructionCache(HANDLE, const void*, u32);
IMP HANDLE WINAPI GetCurrentProcess(void);
IMP u32    WINAPI GetModuleFileNameA(void*, char*, u32);
IMP HANDLE WINAPI CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
IMP BOOL   WINAPI WriteFile(HANDLE, const void*, u32, u32*, void*);
IMP BOOL   WINAPI FlushFileBuffers(HANDLE);
IMP BOOL   WINAPI CloseHandle(HANDLE);
IMP u32    WINAPI GetPrivateProfileIntA(const char*, const char*, int, const char*);
IMP BOOL   WINAPI WritePrivateProfileStringA(const char*, const char*, const char*, const char*);
IMP u32    WINAPI GetFileAttributesA(const char*);
IMP BOOL   WINAPI DisableThreadLibraryCalls(void*);
IMP void   WINAPI GetLocalTime(SYSTEMTIME*);
IMP void   WINAPI OutputDebugStringA(const char*);
IMP long   WINAPI InterlockedDecrement(long volatile*);
IMP void   WINAPI InitializeCriticalSection(void*);
IMP void   WINAPI EnterCriticalSection(void*);
IMP void   WINAPI LeaveCriticalSection(void*);
IMP void*  WINAPI VirtualAlloc(void*, u32, u32, u32);

#define PAGE_EXECUTE_READWRITE 0x40
#define GENERIC_WRITE          0x40000000
#define FILE_SHARE_READ        1
#define CREATE_ALWAYS          2
#define FILE_ATTRIBUTE_NORMAL  0x80
#define INVALID_HANDLE_VALUE   ((HANDLE)(long)-1)
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* tiny libc replacements (we link without a CRT)                      */
/* ------------------------------------------------------------------ */
void* memset(void* d, int c, u32 n) { u8* p = (u8*)d; while (n--) *p++ = (u8)c; return d; }
void* memcpy(void* d, const void* s, u32 n) { u8* p = (u8*)d; const u8* q = (const u8*)s; while (n--) *p++ = *q++; return d; }
static u32 s_len(const char* s) { u32 n = 0; while (s[n]) n++; return n; }
static int s_eq(const char* a, const char* b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void s_cpy(char* d, const char* s, u32 cap) { u32 i = 0; while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; } d[i] = 0; }

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */
static HANDLE g_log = 0;
static u32    g_logLines = 0;
static char   g_lastLine[512];
static u32    g_repeat = 0;
static u8     g_cs[64];          /* CRITICAL_SECTION is 24 bytes on x86; 64 to be safe */
static int    g_csInit = 0;

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, t)   __builtin_va_arg(v, t)

static u32 fmt_u(char* o, u32 v, u32 base, int width, char pad)
{
    char t[16]; u32 n = 0, i = 0;
    do { u32 d = v % base; t[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10); v /= base; } while (v);
    while ((int)n < width) t[n++] = pad;
    while (n) o[i++] = t[--n];
    return i;
}
static u32 vfmt(char* out, u32 cap, const char* f, va_list ap)
{
    u32 n = 0;
    while (*f && n + 24 < cap) {
        if (*f != '%') { out[n++] = *f++; continue; }
        f++;
        int width = 0; char pad = ' ';
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        switch (*f) {
        case 's': { const char* s = va_arg(ap, const char*); if (!s) s = "(null)";
                    while (*s && n + 2 < cap) out[n++] = *s++; break; }
        case 'd': { int v = va_arg(ap, int); if (v < 0) { out[n++] = '-'; v = -v; } n += fmt_u(out + n, (u32)v, 10, width, pad); break; }
        case 'u': n += fmt_u(out + n, va_arg(ap, u32), 10, width, pad); break;
        case 'x': n += fmt_u(out + n, va_arg(ap, u32), 16, width, pad); break;
        case 'p': out[n++] = '0'; out[n++] = 'x'; n += fmt_u(out + n, va_arg(ap, u32), 16, 8, '0'); break;
        case '%': out[n++] = '%'; break;
        default: out[n++] = *f; break;
        }
        if (*f) f++;
    }
    out[n] = 0;
    return n;
}
static void log_raw(const char* s, u32 n)
{
    u32 w;
    if (g_log && g_log != INVALID_HANDLE_VALUE) { WriteFile(g_log, s, n, &w, 0); FlushFileBuffers(g_log); }
}
void logf(const char* f, ...)
{
    char body[480], line[512];
    va_list ap; va_start(ap, f); vfmt(body, sizeof(body), f, ap); va_end(ap);
    if (g_csInit) EnterCriticalSection(g_cs);
    if (s_eq(body, g_lastLine)) { g_repeat++; if (g_csInit) LeaveCriticalSection(g_cs); return; }
    if (g_repeat) {
        char r[64]; u32 rn = 0; const char* p = "    (previous line repeated ";
        while (*p) r[rn++] = *p++;
        rn += fmt_u(r + rn, g_repeat, 10, 0, ' ');
        p = " more times)\r\n"; while (*p) r[rn++] = *p++;
        log_raw(r, rn); g_repeat = 0;
    }
    s_cpy(g_lastLine, body, sizeof(g_lastLine));
    if (g_logLines < 200000) {
        SYSTEMTIME t; GetLocalTime(&t);
        u32 n = 0;
        n += fmt_u(line + n, t.wHour, 10, 2, '0');   line[n++] = ':';
        n += fmt_u(line + n, t.wMinute, 10, 2, '0'); line[n++] = ':';
        n += fmt_u(line + n, t.wSecond, 10, 2, '0'); line[n++] = '.';
        n += fmt_u(line + n, t.wMilliseconds, 10, 3, '0'); line[n++] = ' ';
        u32 bl = s_len(body); if (n + bl + 3 > sizeof(line)) bl = sizeof(line) - n - 3;
        memcpy(line + n, body, bl); n += bl; line[n++] = '\r'; line[n++] = '\n';
        log_raw(line, n);
        g_logLines++;
    }
    if (g_csInit) LeaveCriticalSection(g_cs);
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */
static struct {
    int enabled, skipPressStart, skipCarSelect, removeP2, singleScreen, logFlow, classSelect;
} cfg;

static char g_dir[300], g_ini[320], g_logPath[320];
static void cycle_class(void);
void overlay_install(void);
void overlay_show(const char* text, u32 ms);
static void hook_language(void* state);

static void load_config(void* hMod)
{
    char path[300];
    u32 n = GetModuleFileNameA(hMod, path, sizeof(path));
    u32 slash = 0; for (u32 i = 0; i < n; i++) if (path[i] == '\\' || path[i] == '/') slash = i + 1;
    memcpy(g_dir, path, slash); g_dir[slash] = 0;
    /* ini/log = same base name as the asi */
    u32 dot = n; for (u32 i = slash; i < n; i++) if (path[i] == '.') dot = i;
    memcpy(g_ini, path, dot); s_cpy(g_ini + dot, ".ini", 8);
    memcpy(g_logPath, path, dot); s_cpy(g_logPath + dot, ".log", 8);

    if (GetFileAttributesA(g_ini) == INVALID_FILE_ATTRIBUTES) {
        WritePrivateProfileStringA("SplitToSingle", "Enabled", "1", g_ini);
        WritePrivateProfileStringA("SplitToSingle", "SkipP2PressStart", "1", g_ini);
        WritePrivateProfileStringA("SplitToSingle", "SkipP2CarSelect", "1", g_ini);
        WritePrivateProfileStringA("SplitToSingle", "RemoveP2", "1", g_ini);
        WritePrivateProfileStringA("SplitToSingle", "SingleScreen", "1", g_ini);
        WritePrivateProfileStringA("SplitToSingle", "LogFlow", "1", g_ini);
        WritePrivateProfileStringA("SplitToSingle", "ClassSelect", "1", g_ini);
    }
    cfg.enabled        = GetPrivateProfileIntA("SplitToSingle", "Enabled", 1, g_ini);
    cfg.skipPressStart = GetPrivateProfileIntA("SplitToSingle", "SkipP2PressStart", 1, g_ini);
    cfg.skipCarSelect  = GetPrivateProfileIntA("SplitToSingle", "SkipP2CarSelect", 1, g_ini);
    cfg.removeP2       = GetPrivateProfileIntA("SplitToSingle", "RemoveP2", 1, g_ini);
    cfg.singleScreen   = GetPrivateProfileIntA("SplitToSingle", "SingleScreen", 1, g_ini);
    cfg.logFlow        = GetPrivateProfileIntA("SplitToSingle", "LogFlow", 1, g_ini);
    cfg.classSelect    = GetPrivateProfileIntA("SplitToSingle", "ClassSelect", 1, g_ini);
}

/* ------------------------------------------------------------------ */
/* per-executable addresses (absolute, image base 0x400000)            */
/* ------------------------------------------------------------------ */
typedef struct {
    const char* name;
    u32 timestamp, sizeOfImage;
    /* flow node vtables, slot [1] = FindLink(const char* linkId) */
    u32 vtFindLink1Slot, fnFindLink1;
    u32 vtFindLinkNSlot, fnFindLinkN;
    /* split-screen career: human entrant loop */
    u32 entrantCountCall, entrantCountTarget;   /* call PlayerManager::GetCount   */
    u32 entrantGetCall,   entrantGetTarget;     /* call PlayerManager::GetPlayer(i) */
    u32 fnGetMainPlayer;                        /* PlayerManager::GetMainPlayer() */
    u32 fnGetCurrentEvent;                      /* CareerManager::GetCurrentEvent(out) */
    u32 ssDecisionJcc;                          /* "je false" inside StateSplitscreenDecision */
    u32 vtBeginCurSlot, fnBeginCur;             /* StateCareerBeginCurrent::OnEnter */
    u32 beginCurCmIdxInsn;                      /* mov edx, ds:[CareerManager index] */
    u32 vtEndCurSlot, fnEndCur;                 /* StateCareerEndCurrent::OnEnter */
    u32 vtInitSSSlot, fnInitSS;                 /* StateInitialiseSplitscreenEvent::OnEnter */
    u32 vtSelRenderSlot, fnSelRender;           /* StateSelectRenderingMode::OnEnter */
    /* "is the current career the split screen one" helpers (career id == 7) */
    u32 fnIsSSCareerCdecl;                      /* bool __cdecl (ctx)           - HUD, cameras, ... */
    u32 fnIsSSCareerThis;                       /* bool __thiscall (ctxholder)  */
    u32 vtGameIfaceIsSSSlot, fnGameIfaceIsSS;   /* game interface virtual       */
    u32 vtIface2IsSSSlot, fnIface2IsSS;         /* another interface virtual    */
    u32 vtAudioIsSSSlot, fnAudioIsSS;           /* audio parameter "IsSplitscreen" */
    /* car selection */
    u32 vtSetMenuAreaSlot, fnSetMenuArea;       /* StateSetMenuArea::OnEnter (area at this+0x3C, splitscreen = 7) */
    u32 fnClassFromType;                        /* NetDatabaseInterface: vehicle class id from class type (0 hoonigan, 1 demolition, 2 race) */
    u32 textKeyCall, textKeyTarget;             /* IBTextStatic activate: call string::c_str on the lng key (widget = string - 0x14) */
} Build;

static const Build g_builds[2] = {
    { "showdown.exe", 0x50758975, 0x00fc2000,
      0x0104131c, 0x00532430,  0x01041324, 0x00b61a50,
      0x00a99736, 0x00b8c1d0,  0x00a99756, 0x005d93e0,
      0x004c8940, 0x0044be00,  0x00b5eb14,
      0x010100e4, 0x0074b270,  0x0074b2d4,
      0x0101011c, 0x00413e30,  0x010107ac, 0x0065ac50,  0x010169c0, 0x00764d70,
      0x006e4640, 0x005a7130,  0x01003274, 0x00aa5c60,  0x01020a6c, 0x009dfb30,  0x0101ae14, 0x00583d00,
      0x010121dc, 0x007a1340,  
      0x00b5aa20, 0x00dafd73, 0x00d9d4a0 },
    { "showdown_avx.exe", 0x50758c5b, 0x00fd7000,
      0x0105724c, 0x004fd1d0,  0x01057254, 0x00802340,
      0x00963126, 0x004a7620,  0x00963146, 0x00bd8320,
      0x008f1040, 0x00611540,  0x00aaf384,
      0x01026214, 0x00646340,  0x006463a4,
      0x0102624c, 0x007334e0,  0x010268dc, 0x007aebe0,  0x0102cb00, 0x00434700,
      0x006b1240, 0x009583b0,  0x010193d4, 0x006baee0,  0x01036b0c, 0x005bee70,  0x01030ef4, 0x007da210,
      0x0102830c, 0x0074bbd0,  
      0x006cc680, 0x00dc2433, 0x00daf920 },
};

static const Build* B = 0;
static u32 g_base = 0;               /* actual module base */
#define VA(x) ((x) - 0x400000u + g_base)

/* ------------------------------------------------------------------ */
/* memory helpers                                                      */
/* ------------------------------------------------------------------ */
static int write_mem(u32 addr, const void* data, u32 n)
{
    u32 old;
    if (!VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void*)addr, data, n);
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    return 1;
}
static int write_u32(u32 addr, u32 v) { return write_mem(addr, &v, 4); }
static u32 call_target(u32 at) { return at + 5 + *(u32*)(at + 1); }
static int patch_call(u32 at, void* fn)
{
    u8 b[5]; u32 rel = (u32)fn - (at + 5);
    b[0] = 0xE8; memcpy(b + 1, &rel, 4);
    return write_mem(at, b, 5);
}

/* ------------------------------------------------------------------ */
/* game function types                                                 */
/* ------------------------------------------------------------------ */
typedef const char** (__thiscall *FindLink_t)(void* node, const char* linkId);
typedef void*        (__thiscall *GetMainPlayer_t)(void* playerMgr);
typedef void**       (__thiscall *GetCurrentEvent_t)(void* careerMgr, void** out);
typedef void         (__thiscall *OnEnter_t)(void* state, void* arg);
typedef void         (__thiscall *Release_t)(void* obj);

static FindLink_t        oFindLink1, oFindLinkN;
static GetMainPlayer_t   oGetMainPlayer;
static GetCurrentEvent_t oGetCurrentEvent;
static OnEnter_t         oBeginCur, oEndCur, oInitSS, oSelRender;
static u32*              g_cmIndexVar;   /* address of the global holding the CareerManager component index */

/* ------------------------------------------------------------------ */
/* 1 + 2 : flow link redirection                                        */
/* ------------------------------------------------------------------ */
typedef struct { const char* node; const char* link; const char* oldTarget; const char* newTarget; int* enable; } Rule;
static Rule g_rules[] = {
    { "2m0", "next",               "2m1", "2m2", &cfg.skipPressStart },  /* skip P2 "press START" overlay */
    { "I3",  "skip_livery_select", "I4",  "I6",  &cfg.skipCarSelect  },  /* P1 car chosen (no livery screen) -> loading */
    { "Ls",  "next",               "I4",  "I6",  &cfg.skipCarSelect  },  /* P1 livery chosen -> loading */
};
#define NRULES (sizeof(g_rules) / sizeof(g_rules[0]))
static const char* g_redirLink[NRULES][2];   /* fake {target, type} records we hand back */

static const char** redirect(void* node, const char* linkId, const char** r)
{
    const char* nodeId = node ? *(const char**)((u8*)node + 4) : 0;
    if (r && cfg.logFlow)
        logf("flow: node '%s' link '%s' -> '%s'", nodeId, linkId, r[0]);
    if (!r || !nodeId || !linkId) return r;
    if (cfg.classSelect && s_eq(nodeId, "G9") && s_eq(linkId, "to_p2_options")) {
        static const char* lobbyLink[2];
        cycle_class();
        lobbyLink[0] = "G9";          /* re-enter the lobby so the button label refreshes */
        lobbyLink[1] = r[1];
        logf("flow: REDIRECT node 'G9' link 'to_p2_options' -> 'G9' (class picker)");
        return lobbyLink;
    }
    for (u32 i = 0; i < NRULES; i++) {
        Rule* ru = &g_rules[i];
        if (*ru->enable && s_eq(nodeId, ru->node) && s_eq(linkId, ru->link) && s_eq(r[0], ru->oldTarget)) {
            g_redirLink[i][0] = ru->newTarget;
            g_redirLink[i][1] = r[1];
            logf("flow: REDIRECT node '%s' link '%s': '%s' -> '%s'", nodeId, linkId, r[0], ru->newTarget);
            return g_redirLink[i];
        }
    }
    return r;
}
static const char** __fastcall hkFindLink1(void* node, void* edx, const char* linkId)
{ (void)edx; return redirect(node, linkId, oFindLink1(node, linkId)); }
static const char** __fastcall hkFindLinkN(void* node, void* edx, const char* linkId)
{ (void)edx; return redirect(node, linkId, oFindLinkN(node, linkId)); }

/* ------------------------------------------------------------------ */
/* 3 : only the main player becomes a human entrant                     */
/* ------------------------------------------------------------------ */
static void* __fastcall hkGetEntrantPlayer(void* playerMgr, void* edx, u32 index)
{
    (void)edx;
    void* p = oGetMainPlayer(playerMgr);
    u32 count = playerMgr ? *(u32*)((u8*)playerMgr + 0x28) : 0;
    logf("entrants: split-screen career asked for human #%u (PlayerManager has %u players) -> MainPlayer %p",
         index, count, (u32)p);
    return p;
}

/* ------------------------------------------------------------------ */
/* 4 : single screen - event "split screen" flag                       */
/* ------------------------------------------------------------------ */
static u8* g_heldEvent = 0;
static u8  g_heldFlag  = 0;

static void release_obj(void* o)
{
    if (!o) return;
    if (InterlockedDecrement((long volatile*)((u8*)o + 4)) == 0) {
        Release_t rel = (Release_t)((*(void***)o)[1]);
        rel(o);
    }
}
static void restore_event_flag(const char* why)
{
    if (!g_heldEvent) return;
    g_heldEvent[0xA4] = g_heldFlag;
    logf("singlescreen: restored split flag (%u) on event %p (%s)", g_heldFlag, (u32)g_heldEvent, why);
    release_obj(g_heldEvent);
    g_heldEvent = 0;
}
static void* career_manager_from_state(void* state)
{
    u8* ctx = *(u8**)((u8*)state + 0x28);
    if (!ctx || !g_cmIndexVar) return 0;
    void** arr = *(void***)(ctx + 4);
    if (!arr) return 0;
    return arr[*g_cmIndexVar];
}
static void __fastcall hkBeginCur(void* state, void* edx, void* arg)
{
    (void)edx;
    oBeginCur(state, arg);
    if (!cfg.singleScreen) return;
    u8* cm = (u8*)career_manager_from_state(state);
    if (!cm) { logf("singlescreen: no CareerManager"); return; }
    int career = *(int*)(cm + 0xA88);
    if (career != 7) { logf("singlescreen: career_begin_current (career %d, not split screen) - untouched", career); return; }
    void* ev = 0;
    oGetCurrentEvent(cm, &ev);          /* returns an AddRef'd pointer which we keep */
    if (!ev) { logf("singlescreen: split screen career has no current event"); return; }
    restore_event_flag("new event");
    g_heldEvent = (u8*)ev;
    g_heldFlag  = g_heldEvent[0xA4];
    g_heldEvent[0xA4] = 0;
    logf("singlescreen: career_begin_current on split screen career - cleared split flag on event %p (was %u)",
         (u32)ev, g_heldFlag);
}
static void __fastcall hkEndCur(void* state, void* edx, void* arg)
{
    (void)edx;
    restore_event_flag("career_end_current");
    oEndCur(state, arg);
}
static void __fastcall hkInitSS(void* state, void* edx, void* arg)
{
    (void)edx;
    restore_event_flag("initialise_splitscreen_event");
    logf("flow: entering split screen mode (initialise_splitscreen_event)");
    oInitSS(state, arg);
}
static void __fastcall hkSelRender(void* state, void* edx, void* arg)
{
    (void)edx;
    int* mode = (int*)((u8*)state + 0x2C);   /* 0 MainGame, 1 FrontEnd, 2 LoadingScreen, 3 SplitScreen */
    if (cfg.singleScreen && *mode == 3) { *mode = 0; logf("singlescreen: SplitScreen rendering request -> MainGame"); }
    else logf("render: select rendering mode %d", *mode);
    oSelRender(state, arg);
}

/* ------------------------------------------------------------------ */
/* 4b: while racing, the split screen career is not "split screen"     */
/* ------------------------------------------------------------------ */
#define RACE_ACTIVE (g_heldEvent != 0)
typedef u8 (__cdecl    *IsSSCdecl_t)(void* ctx);
typedef u8 (__thiscall *IsSSThis_t)(void* self);
static IsSSCdecl_t oIsSSCdecl;
static IsSSThis_t  oIsSSThis, oGameIfaceIsSS, oIface2IsSS, oAudioIsSS;
static u32 g_ssDenied = 0;
static void denied(const char* who, u32 ret)
{
    if (g_ssDenied < 40) { g_ssDenied++; logf("singlescreen: %s 'is split screen career?' -> false (caller %p)", who, ret); }
}
static u8 __cdecl hkIsSSCdecl(void* ctx)
{
    if (RACE_ACTIVE) { denied("helper A", (u32)__builtin_return_address(0)); return 0; }
    return oIsSSCdecl(ctx);
}
static u8 __fastcall hkIsSSThis(void* self, void* edx)
{
    (void)edx;
    if (RACE_ACTIVE) { denied("helper B", (u32)__builtin_return_address(0)); return 0; }
    return oIsSSThis(self);
}
static u8 __fastcall hkGameIfaceIsSS(void* self, void* edx)
{
    (void)edx;
    if (RACE_ACTIVE) { denied("game interface", (u32)__builtin_return_address(0)); return 0; }
    return oGameIfaceIsSS(self);
}
static u8 __fastcall hkIface2IsSS(void* self, void* edx)
{
    (void)edx;
    if (RACE_ACTIVE) { denied("interface 2", (u32)__builtin_return_address(0)); return 0; }
    return oIface2IsSS(self);
}
static u8 __fastcall hkAudioIsSS(void* self, void* edx)
{
    (void)edx;
    /* original: return (expectedValue == isSplitCareer) */
    if (RACE_ACTIVE) return *((u8*)self + 0x2C) == 0;
    return oAudioIsSS(self);
}
/* ------------------------------------------------------------------ */
/* 5 : menu area tracking                                               */
/* ------------------------------------------------------------------ */
static OnEnter_t  oSetMenuArea;
static int g_menuArea = -1;
static void __fastcall hkSetMenuArea(void* state, void* edx, void* arg)
{
    (void)edx;
    g_menuArea = *(int*)((u8*)state + 0x3C);
    logf("menu area -> %d%s", g_menuArea, g_menuArea == 7 ? " (split screen)" : "");
    if (g_menuArea == 7) hook_language(state);
    oSetMenuArea(state, arg);
}
/* ------------------------------------------------------------------ */
/* 6 : car class picker on the split screen lobby                       */
/*     (the "Player 2 options" button cycles the class)                 */
/* ------------------------------------------------------------------ */
typedef u32 (__thiscall *ClassFromType_t)(void* self, int type);
static ClassFromType_t oClassFromType;
static int  g_classType = -1;        /* -1 = event default, else 0 hoonigan, 1 demolition, 2 race */
static int  g_eventType = -1;        /* last type the game asked for (the event's own class) */
static const char* const g_classNames[3] = { "HOONIGAN", "DEMOLITION", "RACE" };

static u32 __fastcall hkClassFromType(void* self, void* edx, int type)
{
    (void)edx;
    if (g_menuArea == 7) {
        g_eventType = type;
        if (cfg.classSelect && g_classType >= 0 && g_classType <= 2 && type != g_classType) {
            logf("class: event wants class type %d -> using %d (%s)", type, g_classType, g_classNames[g_classType]);
            type = g_classType;
        }
    }
    return oClassFromType(self, type);
}
/* Button text: IBTextStatic sends its lng key to the UI renderer, which resolves it.
   We swap the key of the two "Player 2 options" labels for existing keys whose
   English text is RACE / DEMOLITION / HOONIGAN / DEFAULT (line 1) and empty (line 2). */
static void* g_line1Widget = 0;
static void* g_line2Widget = 0;
static const char* __fastcall hkTextKey(void* strobj, void* edx)
{
    (void)edx;
    u8* p = *(u8**)strobj;
    const char* key = p ? (const char*)(p + 8) : 0;      /* same as the game's string::c_str */
    if (!key || !cfg.classSelect) return key;
    void* widget = (u8*)strobj - 0x14;
    if (!g_line1Widget && s_eq(key, "lng_p2_options_button_1")) { g_line1Widget = widget; logf("class: found lobby button label 1 (%p)", (u32)widget); }
    if (!g_line2Widget && s_eq(key, "lng_p2_options_button_2")) { g_line2Widget = widget; logf("class: found lobby button label 2 (%p)", (u32)widget); }
    if (g_menuArea != 7) return key;
    if (widget == g_line1Widget) return "lng_net_vehicle_class";                 /* VEHICLE CLASS */
    if (widget == g_line2Widget) return "lng_autosave_created_subtitle";         /* empty text */
    return key;
}
static void hook_language(void* state) { (void)state; }
static void cycle_class(void)
{
    /* order: event default -> race -> demolition -> hoonigan -> event default */
    if (g_classType < 0) g_classType = 2;
    else if (g_classType == 0) g_classType = -1;
    else g_classType--;
    logf("class: lobby class set to %s", g_classType < 0 ? "event default" : g_classNames[g_classType]);
    {
        char m[64]; const char* a = "Vehicle class set to "; u32 n = 0;
        while (*a) m[n++] = *a++;
        a = g_classType < 0 ? "EVENT DEFAULT" : g_classNames[g_classType];
        while (*a) m[n++] = *a++;
        m[n] = 0;
        overlay_show(m, 2500);
    }
}

/* inline hook: overwrite n (>=5) position-independent bytes at addr with a jmp, return trampoline */
static void* inline_hook(u32 addr, const u8* expect, u32 n, void* fn)
{
    for (u32 i = 0; i < n; i++) if (((u8*)addr)[i] != expect[i]) return 0;
    u8* t = (u8*)VirtualAlloc(0, 32, 0x3000 /* MEM_COMMIT|MEM_RESERVE */, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    memcpy(t, (void*)addr, n);
    t[n] = 0xE9; *(u32*)(t + n + 1) = (addr + n) - ((u32)t + n + 5);
    u8 j[16]; j[0] = 0xE9; *(u32*)(j + 1) = (u32)fn - (addr + 5);
    for (u32 i = 5; i < n; i++) j[i] = 0x90;
    write_mem(addr, j, n);
    return t;
}

/* ------------------------------------------------------------------ */
/* install                                                             */
/* ------------------------------------------------------------------ */
static int check_slot(const char* what, u32 slot, u32 expect)
{
    u32 v = *(u32*)VA(slot);
    if (v != VA(expect)) { logf("ERROR: %s vtable slot %p holds %p, expected %p - skipped", what, VA(slot), v, VA(expect)); return 0; }
    return 1;
}
static int hook_slot(const char* what, u32 slot, u32 expect, void* fn, void** orig)
{
    if (!check_slot(what, slot, expect)) return 0;
    *orig = (void*)VA(expect);
    if (!write_u32(VA(slot), (u32)fn)) { logf("ERROR: VirtualProtect failed for %s", what); return 0; }
    logf("hooked %s (slot %p, original %p)", what, VA(slot), VA(expect));
    return 1;
}

static void install(void)
{
    u8* mod = (u8*)GetModuleHandleA(0);
    g_base = (u32)mod;
    u32 pe = *(u32*)(mod + 0x3C);
    u32 ts = *(u32*)(mod + pe + 8);
    u32 soi = *(u32*)(mod + pe + 24 + 56);
    for (int i = 0; i < 2; i++)
        if (g_builds[i].timestamp == ts && g_builds[i].sizeOfImage == soi) B = &g_builds[i];
    if (!B) { logf("ERROR: unknown executable (timestamp %x, size %x) - mod disabled", ts, soi); return; }
    logf("detected %s, loaded at %p", B->name, g_base);
    logf("config: Enabled=%d SkipP2PressStart=%d SkipP2CarSelect=%d RemoveP2=%d SingleScreen=%d LogFlow=%d",
         cfg.enabled, cfg.skipPressStart, cfg.skipCarSelect, cfg.removeP2, cfg.singleScreen, cfg.logFlow);
    if (!cfg.enabled) { logf("disabled in ini"); return; }

    /* 1+2: flow links */
    if (cfg.skipPressStart || cfg.skipCarSelect || cfg.logFlow) {
        hook_slot("FlowNode1::FindLink", B->vtFindLink1Slot, B->fnFindLink1, (void*)hkFindLink1, (void**)&oFindLink1);
        hook_slot("FlowNodeN::FindLink", B->vtFindLinkNSlot, B->fnFindLinkN, (void*)hkFindLinkN, (void**)&oFindLinkN);
    }

    /* 3: entrants */
    if (cfg.removeP2) {
        u32 cc = VA(B->entrantCountCall), gc = VA(B->entrantGetCall);
        if (*(u8*)cc == 0xE8 && call_target(cc) == VA(B->entrantCountTarget) &&
            *(u8*)gc == 0xE8 && call_target(gc) == VA(B->entrantGetTarget)) {
            static const u8 movEax1[5] = { 0xB8, 0x01, 0x00, 0x00, 0x00 };  /* mov eax, 1 */
            oGetMainPlayer = (GetMainPlayer_t)VA(B->fnGetMainPlayer);
            write_mem(cc, movEax1, 5);
            patch_call(gc, (void*)hkGetEntrantPlayer);
            logf("patched split screen entrant builder (%p, %p): one human entrant (MainPlayer)", cc, gc);
        } else logf("ERROR: entrant builder bytes differ - RemoveP2 skipped");
    }

    /* 4: single screen */
    if (cfg.singleScreen) {
        u32 j = VA(B->ssDecisionJcc);
        if (*(u8*)j == 0x74 && *(u8*)(j + 1) == 0x07 && *(u8*)(j + 2) == 0x68) {
            static const u8 jmp = 0xEB;
            write_mem(j, &jmp, 1);
            logf("patched StateSplitscreenDecision (%p): always 'false'", j);
        } else logf("ERROR: StateSplitscreenDecision bytes differ - skipped");

        u32 mi = VA(B->beginCurCmIdxInsn);
        if (*(u8*)mi == 0x8B && *(u8*)(mi + 1) == 0x15) {
            g_cmIndexVar = (u32*)*(u32*)(mi + 2);      /* absolute address, already relocated by the loader */
            oGetCurrentEvent = (GetCurrentEvent_t)VA(B->fnGetCurrentEvent);
            hook_slot("StateCareerBeginCurrent::OnEnter", B->vtBeginCurSlot, B->fnBeginCur, (void*)hkBeginCur, (void**)&oBeginCur);
            hook_slot("StateCareerEndCurrent::OnEnter", B->vtEndCurSlot, B->fnEndCur, (void*)hkEndCur, (void**)&oEndCur);
            hook_slot("StateInitialiseSplitscreenEvent::OnEnter", B->vtInitSSSlot, B->fnInitSS, (void*)hkInitSS, (void**)&oInitSS);
        } else logf("ERROR: CareerManager index instruction differs - event flag handling skipped");

        hook_slot("StateSelectRenderingMode::OnEnter", B->vtSelRenderSlot, B->fnSelRender, (void*)hkSelRender, (void**)&oSelRender);

        static const u8 pA[7] = { 0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x45, 0x08 };
        static const u8 pB[5] = { 0x8B, 0x01, 0x8B, 0x48, 0x04 };
        oIsSSCdecl = (IsSSCdecl_t)inline_hook(VA(B->fnIsSSCareerCdecl), pA, 7, (void*)hkIsSSCdecl);
        logf(oIsSSCdecl ? "hooked IsSplitscreenCareer helper A (%p)" : "ERROR: helper A bytes differ (%p) - skipped", VA(B->fnIsSSCareerCdecl));
        oIsSSThis = (IsSSThis_t)inline_hook(VA(B->fnIsSSCareerThis), pB, 5, (void*)hkIsSSThis);
        logf(oIsSSThis ? "hooked IsSplitscreenCareer helper B (%p)" : "ERROR: helper B bytes differ (%p) - skipped", VA(B->fnIsSSCareerThis));
        hook_slot("GameInterface::IsSplitscreen", B->vtGameIfaceIsSSSlot, B->fnGameIfaceIsSS, (void*)hkGameIfaceIsSS, (void**)&oGameIfaceIsSS);
        hook_slot("Interface2::IsSplitscreen", B->vtIface2IsSSSlot, B->fnIface2IsSS, (void*)hkIface2IsSS, (void**)&oIface2IsSS);
        hook_slot("Audio IsSplitscreen param", B->vtAudioIsSSSlot, B->fnAudioIsSS, (void*)hkAudioIsSS, (void**)&oAudioIsSS);
    }

    /* 6: class picker */
    if (cfg.classSelect) {
        overlay_install();
        static const u8 pC[9] = { 0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0 };
        oClassFromType = (ClassFromType_t)inline_hook(VA(B->fnClassFromType), pC, 9, (void*)hkClassFromType);
        logf(oClassFromType ? "hooked vehicle class lookup (%p)" : "ERROR: vehicle class lookup bytes differ (%p) - ClassSelect off", VA(B->fnClassFromType));
        if (!oClassFromType) cfg.classSelect = 0;
        u32 tk = VA(B->textKeyCall);
        if (*(u8*)tk == 0xE8 && call_target(tk) == VA(B->textKeyTarget)) {
            patch_call(tk, (void*)hkTextKey);
            logf("patched text label key lookup (%p)", tk);
        } else logf("ERROR: text label call differs - button label will not change");
        if (!oSetMenuArea)
            hook_slot("StateSetMenuArea::OnEnter", B->vtSetMenuAreaSlot, B->fnSetMenuArea, (void*)hkSetMenuArea, (void**)&oSetMenuArea);
    }

    logf("init done");
}

/* ------------------------------------------------------------------ */
/* crash reporter (logs only, never handles the exception)             */
/* ------------------------------------------------------------------ */
typedef struct { u32 code, flags; void* rec; u32 addr; u32 nparams; u32 info[15]; } ExcRecord;
typedef struct { ExcRecord* rec; u8* ctx; } ExcPointers;
IMP void* WINAPI AddVectoredExceptionHandler(u32 first, void* handler);
static long g_crashLogs = 0;

static void log_addr(const char* label, u32 a)
{
    u32 imgEnd = g_base + (B ? B->sizeOfImage : 0);
    if (a >= g_base && a < imgEnd) logf("  %s %p  (exe va %p)", label, a, a - g_base + 0x400000);
    else logf("  %s %p", label, a);
}
static long WINAPI crash_handler(ExcPointers* ep)
{
    u32 c = ep->rec->code;
    if (c != 0xC0000005 && c != 0xC000001D && c != 0xC0000094 && c != 0xC00000FD && c != 0xC0000096 && c != 0xC0000409)
        return 0;
    if (g_crashLogs++ >= 6) return 0;
    u8* x = ep->ctx;
    #define R(o) (*(u32*)(x + (o)))
    logf("=== EXCEPTION %x at %p (access %u to %p) ===", c, ep->rec->addr, ep->rec->info[0], ep->rec->info[1]);
    log_addr("eip", R(0xB8));
    logf("  eax=%p ebx=%p ecx=%p edx=%p", R(0xB0), R(0xA4), R(0xAC), R(0xA8));
    logf("  esi=%p edi=%p ebp=%p esp=%p", R(0xA0), R(0x9C), R(0xB4), R(0xC4));
    u32* sp = (u32*)R(0xC4);
    u32 textLo = g_base + 0x1000, textHi = g_base + (B ? B->sizeOfImage : 0);
    for (int i = 0, n = 0; i < 256 && n < 24; i++) {
        u32 v = sp[i];
        if (v > textLo && v < textHi && v < g_base + 0xAC3000) { logf("  stack[+%x] %p  (exe va %p)", i * 4, v, v - g_base + 0x400000); n++; }
    }
    #undef R
    return 0; /* EXCEPTION_CONTINUE_SEARCH */
}

BOOL WINAPI DllMain(void* hinst, u32 reason, void* reserved)
{
    (void)reserved;
    if (reason == 1) {
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(g_cs); g_csInit = 1;
        load_config(hinst);
        g_log = CreateFileA(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        logf("SplitToSingle.asi - DiRT Showdown split screen -> single race");
        install();
        AddVectoredExceptionHandler(1, (void*)crash_handler);
    }
    return 1;
}
