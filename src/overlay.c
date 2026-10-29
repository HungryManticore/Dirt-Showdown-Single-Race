/*
 * Minimal on-screen message overlay for D3D11 (DiRT Showdown / DXVK friendly).
 *  - hooks the exe's import of d3d11!D3D11CreateDevice
 *  - with the game's device, creates a throw-away swap chain to find the swap chain
 *    vtable and replaces Present
 *  - in Present: GDI renders the text into a DIB, which becomes a texture drawn as a
 *    quad (shaders compiled at runtime with d3dcompiler_43!D3DCompile).
 *    All pipeline state touched is saved and restored.
 */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            BOOL;
typedef void*          HANDLE;
#define WINAPI __stdcall
#define IMP __declspec(dllimport)

IMP void*  WINAPI GetModuleHandleA(const char*);
IMP void*  WINAPI LoadLibraryA(const char*);
IMP void*  WINAPI GetProcAddress(void*, const char*);
IMP BOOL   WINAPI VirtualProtect(void*, u32, u32, u32*);
IMP u32    WINAPI GetTickCount(void);
IMP void*  WINAPI CreateWindowExA(u32, const char*, const char*, u32, int, int, int, int, void*, void*, void*, void*);
IMP BOOL   WINAPI DestroyWindow(void*);
IMP int    WINAPI DrawTextA(void*, const char*, int, int*, u32);
IMP void*  WINAPI CreateCompatibleDC(void*);
IMP BOOL   WINAPI DeleteDC(void*);
IMP void*  WINAPI CreateDIBSection(void*, const void*, u32, void**, void*, u32);
IMP void*  WINAPI SelectObject(void*, void*);
IMP BOOL   WINAPI DeleteObject(void*);
IMP void*  WINAPI CreateFontA(int, int, int, int, int, u32, u32, u32, u32, u32, u32, u32, u32, const char*);
IMP u32    WINAPI SetTextColor(void*, u32);
IMP int    WINAPI SetBkMode(void*, int);
IMP BOOL   WINAPI GdiFlush(void);

void logf(const char* f, ...);
void* memset(void* d, int c, u32 n);
void* memcpy(void* d, const void* s, u32 n);

typedef struct { u32 a; u16 b, c; u8 d[8]; } GUID;
static const GUID IID_IDXGIDevice  = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
static const GUID IID_IDXGIFactory = { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
static const GUID IID_ID3D11Device  = { 0xdb6f6ddb, 0xac77, 0x4e88, { 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40 } };
static const GUID IID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

#define VT(o) (*(void***)(o))
typedef long  (WINAPI *QI_t)(void*, const GUID*, void**);
typedef u32   (WINAPI *Rel_t)(void*);
static void rel(void* o) { if (o) ((Rel_t)VT(o)[2])(o); }

/* ---------------- state ---------------- */
static void* g_dev;            /* game's ID3D11Device (not AddRef'd) */
static void* g_ctx;
static long (WINAPI *oPresent)(void* sc, u32 sync, u32 flags);
static long (WINAPI *oCreateDevice)(void*, u32, void*, u32, const u32*, u32, u32, void**, u32*, void**);
static void *g_vs, *g_ps, *g_cb, *g_blend, *g_rs, *g_dss, *g_samp;
static void *g_tex, *g_srv;
static int   g_texW, g_texH, g_texForH;
static volatile u32 g_msgSerial, g_builtSerial;
static char  g_msg[96];
static volatile u32 g_msgUntil;
static int   g_failed;

void overlay_show(const char* text, u32 ms)
{
    u32 i = 0; while (text[i] && i < sizeof(g_msg) - 1) { g_msg[i] = text[i]; i++; } g_msg[i] = 0;
    g_msgUntil = GetTickCount() + ms;
    g_msgSerial++;
}

/* ---------------- resources ---------------- */
static const char g_hlsl[] =
"cbuffer C : register(b0) { float4 rect; float4 mul; };\n"
"struct V { float4 p : SV_Position; float2 t : TEXCOORD0; };\n"
"V vs(uint id : SV_VertexID) { V o; float2 t = float2(id & 1, id >> 1);\n"
"  o.p = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0, 1); o.t = t; return o; }\n"
"Texture2D T : register(t0); SamplerState S : register(s0);\n"
"float4 ps(V i) : SV_Target { return T.Sample(S, i.t) * mul.x; }\n";

typedef long (WINAPI *D3DCompile_t)(const void*, u32, const char*, const void*, void*, const char*, const char*, u32, u32, void**, void**);

static int create_pipeline(void)
{
    void* lib = GetModuleHandleA("d3dcompiler_43.dll");
    if (!lib) lib = LoadLibraryA("d3dcompiler_43.dll");
    if (!lib) lib = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompile_t comp = lib ? (D3DCompile_t)GetProcAddress(lib, "D3DCompile") : 0;
    if (!comp) { logf("overlay: D3DCompile not available"); return 0; }
    void *bv = 0, *bp = 0, *err = 0;
    if (comp(g_hlsl, sizeof(g_hlsl) - 1, "ovl", 0, 0, "vs", "vs_4_0", 0, 0, &bv, &err) < 0) {
        logf("overlay: VS compile failed: %s", err ? (const char*)((void*(WINAPI*)(void*))VT(err)[3])(err) : "?"); return 0; }
    if (comp(g_hlsl, sizeof(g_hlsl) - 1, "ovl", 0, 0, "ps", "ps_4_0", 0, 0, &bp, &err) < 0) {
        logf("overlay: PS compile failed: %s", err ? (const char*)((void*(WINAPI*)(void*))VT(err)[3])(err) : "?"); return 0; }
    typedef void* (WINAPI *Ptr_t)(void*); typedef u32 (WINAPI *Sz_t)(void*);
    void** d = VT(g_dev);
    long hr;
    hr = ((long (WINAPI*)(void*, const void*, u32, void*, void**))d[12])(g_dev, ((Ptr_t)VT(bv)[3])(bv), ((Sz_t)VT(bv)[4])(bv), 0, &g_vs);
    if (hr < 0) { logf("overlay: CreateVertexShader %x", hr); return 0; }
    hr = ((long (WINAPI*)(void*, const void*, u32, void*, void**))d[15])(g_dev, ((Ptr_t)VT(bp)[3])(bp), ((Sz_t)VT(bp)[4])(bp), 0, &g_ps);
    if (hr < 0) { logf("overlay: CreatePixelShader %x", hr); return 0; }
    rel(bv); rel(bp);

    /* constant buffer: 32 bytes, default usage */
    u32 bd[6] = { 32, 0 /*DEFAULT*/, 4 /*CONSTANT_BUFFER*/, 0, 0, 0 };
    if (((long (WINAPI*)(void*, const void*, const void*, void**))d[3])(g_dev, bd, 0, &g_cb) < 0) return 0;

    /* blend: premultiplied alpha */
    u32 bl[2 + 8 * 8]; memset(bl, 0, sizeof(bl));
    bl[0] = 0; bl[1] = 0;                       /* AlphaToCoverage, IndependentBlend */
    for (int r = 0; r < 8; r++) {
        u32* t = bl + 2 + r * 8;
        t[0] = 1;                               /* BlendEnable */
        t[1] = 2; t[2] = 6; t[3] = 1;           /* ONE, INV_SRC_ALPHA, ADD */
        t[4] = 2; t[5] = 6; t[6] = 1;
        t[7] = 0x0F;                            /* write mask */
    }
    if (((long (WINAPI*)(void*, const void*, void**))d[20])(g_dev, bl, &g_blend) < 0) return 0;

    /* rasterizer: solid, cull none, depth clip on */
    u32 rs[10] = { 3 /*SOLID*/, 1 /*CULL_NONE*/, 0, 0, 0, 0, 1, 0, 0, 0 };
    if (((long (WINAPI*)(void*, const void*, void**))d[22])(g_dev, rs, &g_rs) < 0) return 0;

    /* depth stencil: everything off */
    u32 ds[13]; memset(ds, 0, sizeof(ds)); ds[2] = 8 /*ALWAYS*/;
    ds[4] = 0xFF; /* read/write masks packed in one dword area (UINT8, UINT8) */
    ds[5] = 1; ds[6] = 1; ds[7] = 1; ds[8] = 8; ds[9] = 1; ds[10] = 1; ds[11] = 1; ds[12] = 8;
    if (((long (WINAPI*)(void*, const void*, void**))d[21])(g_dev, ds, &g_dss) < 0) return 0;

    /* sampler: linear clamp */
    u32 sm[13]; memset(sm, 0, sizeof(sm));
    sm[0] = 0x15; /* MIN_MAG_MIP_LINEAR */ sm[1] = 3; sm[2] = 3; sm[3] = 3; /* CLAMP */
    sm[5] = 1; sm[6] = 1; /* MaxAnisotropy, ComparisonFunc NEVER */
    ((float*)sm)[12] = 3.4e38f;  /* MaxLOD */
    if (((long (WINAPI*)(void*, const void*, void**))d[23])(g_dev, sm, &g_samp) < 0) return 0;
    logf("overlay: pipeline ready");
    return 1;
}

typedef struct { u32 size; int w, h; u16 planes, bpp; u32 comp, sizeImg; int xppm, yppm; u32 used, important; } BMIH;

static int build_texture(int screenH)
{
    rel(g_srv); rel(g_tex); g_srv = g_tex = 0;
    int fontH = screenH / 16; if (fontH < 18) fontH = 18;
    int w = fontH * 22, h = fontH * 2;
    BMIH bi; memset(&bi, 0, sizeof(bi));
    bi.size = sizeof(bi); bi.w = w; bi.h = -h; bi.planes = 1; bi.bpp = 32;
    void* bits = 0;
    void* dc = CreateCompatibleDC(0);
    void* bmp = CreateDIBSection(dc, &bi, 0, &bits, 0, 0);
    if (!dc || !bmp || !bits) { logf("overlay: GDI surface failed"); if (dc) DeleteDC(dc); return 0; }
    void* oldb = SelectObject(dc, bmp);
    void* font = CreateFontA(-fontH, 0, 0, 0, 700, 0, 0, 0, 0, 0, 0, 4 /*ANTIALIASED*/, 0, "Arial");
    void* oldf = font ? SelectObject(dc, font) : 0;
    SetBkMode(dc, 1 /*TRANSPARENT*/);
    SetTextColor(dc, 0x00FFFFFF);
    int rc[4] = { 0, 0, w, h };
    DrawTextA(dc, g_msg, -1, rc, 0x1 | 0x4 | 0x20 /*CENTER|VCENTER|SINGLELINE*/);
    GdiFlush();
    /* white text on black -> premultiplied BGRA over a translucent dark panel */
    u8* p = (u8*)bits;
    for (int i = 0; i < w * h; i++, p += 4) {
        u32 l = p[0] > p[1] ? p[0] : p[1]; if (p[2] > l) l = p[2];
        u32 a = 170 + ((255 - 170) * l) / 255;
        p[0] = p[1] = p[2] = (u8)l; p[3] = (u8)a;
    }
    /* thin border */
    for (int x = 0; x < w; x++) { u8* q = (u8*)bits + x * 4; q[0]=q[1]=q[2]=200; q[3]=255; q = (u8*)bits + ((h - 1) * w + x) * 4; q[0]=q[1]=q[2]=200; q[3]=255; }

    u32 td[11] = { (u32)w, (u32)h, 1, 1, 87 /*B8G8R8A8_UNORM*/, 1, 0, 0 /*DEFAULT*/, 8 /*SHADER_RESOURCE*/, 0, 0 };
    struct { const void* p; u32 pitch, slice; } init = { bits, (u32)w * 4, 0 };
    void** d = VT(g_dev);
    long hr = ((long (WINAPI*)(void*, const void*, const void*, void**))d[5])(g_dev, td, &init, &g_tex);
    if (hr >= 0) hr = ((long (WINAPI*)(void*, void*, const void*, void**))d[7])(g_dev, g_tex, 0, &g_srv);
    if (oldf) SelectObject(dc, oldf);
    if (font) DeleteObject(font);
    SelectObject(dc, oldb); DeleteObject(bmp); DeleteDC(dc);
    if (hr < 0) { logf("overlay: texture creation failed %x", hr); return 0; }
    g_texW = w; g_texH = h; g_texForH = screenH;
    return 1;
}

/* ---------------- drawing ---------------- */
static void draw(void* sc)
{
    void** c = VT(g_ctx);
    void* bb = 0;
    if (((long (WINAPI*)(void*, u32, const GUID*, void**))VT(sc)[9])(sc, 0, &IID_ID3D11Texture2D, &bb) < 0 || !bb) return;
    u32 desc[11]; ((void (WINAPI*)(void*, u32*))VT(bb)[10])(bb, desc);   /* ID3D11Texture2D::GetDesc */
    int sw = (int)desc[0], sh = (int)desc[1];
    void* rtv = 0;
    long hr = ((long (WINAPI*)(void*, void*, const void*, void**))VT(g_dev)[9])(g_dev, bb, 0, &rtv);
    rel(bb);
    if (hr < 0 || !rtv) return;
    if (g_builtSerial != g_msgSerial || g_texForH != sh) {
        if (!build_texture(sh)) { rel(rtv); return; }
        g_builtSerial = g_msgSerial;
    }

    /* ---- save state ---- */
    void *sRTV[8] = {0}, *sDSV = 0, *sBlend = 0, *sDSS = 0, *sRS = 0, *sVS = 0, *sPS = 0, *sGS = 0, *sIL = 0, *sCB = 0, *sSRV = 0, *sSamp = 0;
    float sBF[4]; u32 sMask = 0, sRef = 0, sTopo = 0, nVP = 16;
    float sVP[16 * 6];
    ((void (WINAPI*)(void*, u32, void**, void**))c[89])(g_ctx, 8, sRTV, &sDSV);
    ((void (WINAPI*)(void*, void**, float*, u32*))c[91])(g_ctx, &sBlend, sBF, &sMask);
    ((void (WINAPI*)(void*, void**, u32*))c[92])(g_ctx, &sDSS, &sRef);
    ((void (WINAPI*)(void*, void**))c[94])(g_ctx, &sRS);
    ((void (WINAPI*)(void*, u32*, float*))c[95])(g_ctx, &nVP, sVP);
    ((void (WINAPI*)(void*, void**, void**, u32*))c[76])(g_ctx, &sVS, 0, 0);
    ((void (WINAPI*)(void*, void**, void**, u32*))c[74])(g_ctx, &sPS, 0, 0);
    ((void (WINAPI*)(void*, void**, void**, u32*))c[82])(g_ctx, &sGS, 0, 0);
    ((void (WINAPI*)(void*, void**))c[78])(g_ctx, &sIL);
    ((void (WINAPI*)(void*, u32*))c[83])(g_ctx, &sTopo);
    ((void (WINAPI*)(void*, u32, u32, void**))c[72])(g_ctx, 0, 1, &sCB);
    ((void (WINAPI*)(void*, u32, u32, void**))c[73])(g_ctx, 0, 1, &sSRV);
    ((void (WINAPI*)(void*, u32, u32, void**))c[75])(g_ctx, 0, 1, &sSamp);

    /* ---- draw ---- */
    u32 now = GetTickCount();
    u32 left = g_msgUntil - now;
    float fade = left < 400 ? (float)left / 400.0f : 1.0f;
    float pw = 2.0f * g_texW / sw, ph = 2.0f * g_texH / sh;
    float cy = -0.55f;
    float cbd[8] = { -pw / 2, cy + ph / 2, pw / 2, cy - ph / 2, fade, 0, 0, 0 };
    ((void (WINAPI*)(void*, void*, u32, const void*, const void*, u32, u32))c[48])(g_ctx, g_cb, 0, 0, cbd, 0, 0);
    float vp[6] = { 0, 0, (float)sw, (float)sh, 0, 1 };
    float bf[4] = { 0, 0, 0, 0 };
    ((void (WINAPI*)(void*, u32, void**, void*))c[33])(g_ctx, 1, &rtv, 0);
    ((void (WINAPI*)(void*, void*, const float*, u32))c[35])(g_ctx, g_blend, bf, 0xFFFFFFFF);
    ((void (WINAPI*)(void*, void*, u32))c[36])(g_ctx, g_dss, 0);
    ((void (WINAPI*)(void*, void*))c[43])(g_ctx, g_rs);
    ((void (WINAPI*)(void*, u32, const float*))c[44])(g_ctx, 1, vp);
    ((void (WINAPI*)(void*, void*))c[17])(g_ctx, 0);
    ((void (WINAPI*)(void*, u32))c[24])(g_ctx, 5 /*TRIANGLESTRIP*/);
    ((void (WINAPI*)(void*, void*, void**, u32))c[11])(g_ctx, g_vs, 0, 0);
    ((void (WINAPI*)(void*, void*, void**, u32))c[23])(g_ctx, 0, 0, 0);
    ((void (WINAPI*)(void*, void*, void**, u32))c[9])(g_ctx, g_ps, 0, 0);
    ((void (WINAPI*)(void*, u32, u32, void**))c[7])(g_ctx, 0, 1, &g_cb);
    ((void (WINAPI*)(void*, u32, u32, void**))c[16])(g_ctx, 0, 1, &g_cb);
    ((void (WINAPI*)(void*, u32, u32, void**))c[8])(g_ctx, 0, 1, &g_srv);
    ((void (WINAPI*)(void*, u32, u32, void**))c[10])(g_ctx, 0, 1, &g_samp);
    ((void (WINAPI*)(void*, u32, u32))c[13])(g_ctx, 4, 0);

    /* ---- restore ---- */
    void* sPSCB = 0; (void)sPSCB;
    ((void (WINAPI*)(void*, u32, void**, void*))c[33])(g_ctx, 8, sRTV, sDSV);
    ((void (WINAPI*)(void*, void*, const float*, u32))c[35])(g_ctx, sBlend, sBF, sMask);
    ((void (WINAPI*)(void*, void*, u32))c[36])(g_ctx, sDSS, sRef);
    ((void (WINAPI*)(void*, void*))c[43])(g_ctx, sRS);
    if (nVP) ((void (WINAPI*)(void*, u32, const float*))c[44])(g_ctx, nVP, sVP);
    ((void (WINAPI*)(void*, void*))c[17])(g_ctx, sIL);
    ((void (WINAPI*)(void*, u32))c[24])(g_ctx, sTopo);
    ((void (WINAPI*)(void*, void*, void**, u32))c[11])(g_ctx, sVS, 0, 0);
    ((void (WINAPI*)(void*, void*, void**, u32))c[23])(g_ctx, sGS, 0, 0);
    ((void (WINAPI*)(void*, void*, void**, u32))c[9])(g_ctx, sPS, 0, 0);
    ((void (WINAPI*)(void*, u32, u32, void**))c[7])(g_ctx, 0, 1, &sCB);
    ((void (WINAPI*)(void*, u32, u32, void**))c[8])(g_ctx, 0, 1, &sSRV);
    ((void (WINAPI*)(void*, u32, u32, void**))c[10])(g_ctx, 0, 1, &sSamp);
    for (int i = 0; i < 8; i++) rel(sRTV[i]);
    rel(sDSV); rel(sBlend); rel(sDSS); rel(sRS); rel(sVS); rel(sPS); rel(sGS); rel(sIL); rel(sCB); rel(sSRV); rel(sSamp);
    rel(rtv);
}
/* we set PS constant buffer slot 0 too; save/restore it separately */
static long WINAPI hkPresent(void* sc, u32 sync, u32 flags)
{
    if (!g_failed && g_msgSerial && (int)(g_msgUntil - GetTickCount()) > 0) {
        /* use the device that owns this swap chain - the one seen in
           D3D11CreateDevice may have been a throw-away probe device */
        void* dev = 0;
        if (((long (WINAPI*)(void*, const GUID*, void**))VT(sc)[7])(sc, &IID_ID3D11Device, &dev) < 0 || !dev)
            return oPresent(sc, sync, flags);
        rel(dev);                               /* the swap chain keeps it alive */
        if (dev != g_dev) {
            if (g_ctx) logf("overlay: swap chain device changed (%p -> %p)", (u32)g_dev, (u32)dev);
            g_dev = dev; g_ctx = 0;             /* old objects belong to the old device: drop them */
            g_vs = g_ps = g_cb = g_blend = g_rs = g_dss = g_samp = g_tex = g_srv = 0;
            g_texForH = 0;
        }
        if (!g_ctx) {
            ((void (WINAPI*)(void*, void**))VT(g_dev)[40])(g_dev, &g_ctx);
            if (!create_pipeline()) { g_failed = 1; logf("overlay: disabled"); }
        }
        if (!g_failed) {
            void* sPSCB = 0;
            ((void (WINAPI*)(void*, u32, u32, void**))VT(g_ctx)[77])(g_ctx, 0, 1, &sPSCB);
            draw(sc);
            ((void (WINAPI*)(void*, u32, u32, void**))VT(g_ctx)[16])(g_ctx, 0, 1, &sPSCB);
            rel(sPSCB);
        }
    }
    return oPresent(sc, sync, flags);
}

/* ---------------- hooking ---------------- */
static int patch_ptr(void** slot, void* fn, void** orig)
{
    u32 old;
    if (!VirtualProtect(slot, 4, 0x40, &old)) return 0;
    if (orig) *orig = *slot;
    *slot = fn;
    VirtualProtect(slot, 4, old, &old);
    return 1;
}
static void hook_present_from_device(void* dev)
{
    void *dxgiDev = 0, *adapter = 0, *factory = 0, *sc = 0;
    if (((QI_t)VT(dev)[0])(dev, &IID_IDXGIDevice, &dxgiDev) < 0) { logf("overlay: no IDXGIDevice"); return; }
    ((long (WINAPI*)(void*, void**))VT(dxgiDev)[7])(dxgiDev, &adapter);
    if (adapter) ((long (WINAPI*)(void*, const GUID*, void**))VT(adapter)[6])(adapter, &IID_IDXGIFactory, &factory);
    void* wnd = CreateWindowExA(0, "STATIC", "ovl", 0, 0, 0, 64, 64, 0, 0, 0, 0);
    if (factory && wnd) {
        u32 d[15]; memset(d, 0, sizeof(d));
        d[0] = 64; d[1] = 64; d[4] = 28 /*R8G8B8A8_UNORM*/;
        d[7] = 1;               /* SampleDesc.Count */
        d[9] = 0x20;            /* BufferUsage RENDER_TARGET_OUTPUT */
        d[10] = 1;              /* BufferCount */
        d[11] = (u32)wnd;       /* OutputWindow */
        d[12] = 1;              /* Windowed */
        long hr = ((long (WINAPI*)(void*, void*, void*, void**))VT(factory)[10])(factory, dev, d, &sc);
        if (hr >= 0 && sc) {
            void** vt = VT(sc);
            if (vt[8] != (void*)hkPresent && patch_ptr(&vt[8], (void*)hkPresent, (void**)&oPresent))
                logf("overlay: hooked IDXGISwapChain::Present (vtable %p, original %p)", (u32)vt, (u32)oPresent);
        } else logf("overlay: dummy swap chain failed %x", hr);
    } else logf("overlay: no DXGI factory/window");
    rel(sc); rel(factory); rel(adapter); rel(dxgiDev);
    if (wnd) DestroyWindow(wnd);
}
static long WINAPI hkCreateDevice(void* ad, u32 dt, void* sw, u32 fl, const u32* lv, u32 nl, u32 sdk, void** ppDev, u32* pLv, void** ppCtx)
{
    long hr = oCreateDevice(ad, dt, sw, fl, lv, nl, sdk, ppDev, pLv, ppCtx);
    if (hr >= 0 && ppDev && *ppDev && !oPresent) {
        logf("overlay: game created D3D11 device %p", (u32)*ppDev);
        hook_present_from_device(*ppDev);
    }
    return hr;
}
void overlay_install(void)
{
    u8* mod = (u8*)GetModuleHandleA(0);
    u32 pe = *(u32*)(mod + 0x3C);
    u32 impRva = *(u32*)(mod + pe + 24 + 96 + 8);
    if (!impRva) return;
    for (u32* imp = (u32*)(mod + impRva); imp[3]; imp += 5) {
        const char* dll = (const char*)(mod + imp[3]);
        char low[16]; u32 i = 0; while (dll[i] && i < 15) { char ch = dll[i]; low[i++] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; } low[i] = 0;
        if (!(low[0]=='d'&&low[1]=='3'&&low[2]=='d'&&low[3]=='1'&&low[4]=='1'&&low[5]=='.')) continue;
        u32* names = (u32*)(mod + (imp[0] ? imp[0] : imp[4]));
        void** iat = (void**)(mod + imp[4]);
        for (u32 k = 0; names[k]; k++) {
            if (names[k] & 0x80000000) continue;
            const char* nm = (const char*)(mod + names[k] + 2);
            const char* want = "D3D11CreateDevice"; u32 j = 0;
            while (want[j] && nm[j] == want[j]) j++;
            if (!want[j] && !nm[j]) {
                patch_ptr(&iat[k], (void*)hkCreateDevice, (void**)&oCreateDevice);
                logf("overlay: hooked D3D11CreateDevice import");
                return;
            }
        }
    }
    logf("overlay: D3D11CreateDevice import not found");
}
int _fltused = 0;
