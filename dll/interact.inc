// ---------------------------------------------------------------- touch effects: rustle sounds + particle bursts
// Plants are read from each foliage block's control-point array (rd+0x6D0, 32 bytes each: u32 context (mesh index in
// the low 31 bits, which is also the coverage-material index), float3 position relative to the block's bounds centre,
// float4 rotation). Party characters walking into a plant's touch radius trigger a procedural rustle (rustle.inc) and,
// on the game thread (UObject::ProcessEvent hook), a Niagara effect from the game's own assets.
#include <tlhelp32.h>
#include <vector>
#include <unordered_map>
#pragma comment(lib, "user32.lib")

static constexpr uintptr_t RVA_ProcessEvent = 0x19981F0;   // UObject::ProcessEvent (vtable slot 69)
static const uint8_t PE_PROLOGUE[17] = { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0xB8, 0xF0, 0x00, 0x00, 0x00 };
static constexpr int OFF_RD_CP = 0x6D0, OFF_RD_CPCOUNT = 0x88, OFF_RD_HASH = 0x80;
static constexpr int OFF_View_ViewRight = 1120;
static constexpr int EFI_RootSet = 1 << 30;                 // EInternalObjectFlags::RootSet (FUObjectItem +8)

typedef void(__fastcall* ProcessEvent_t)(uintptr_t obj, uintptr_t fn, void* parms);
static ProcessEvent_t g_peOrig = nullptr;
static DWORD g_gameTid = 0;
static bool g_inTick = false;
static ULONGLONG g_lastTick = 0;

// camera / clock published by the render thread (FillView)
static float g_camRight[3] = { 1, 0, 0 };
static std::atomic<float> g_viewGameTime{0.f};
static std::atomic<ULONGLONG> g_viewGameTimeChanged{0};
static void SetCamRight(const uint8_t* v) {
    AcquireSRWLockExclusive(&g_camLock); memcpy(g_camRight, v + OFF_View_ViewRight, 12); ReleaseSRWLockExclusive(&g_camLock);
    float t; memcpy(&t, v + OFF_View_GameTime, 4);
    if (t != g_viewGameTime.load(std::memory_order_relaxed)) { g_viewGameTime.store(t, std::memory_order_relaxed); g_viewGameTimeChanged.store(GetTickCount64(), std::memory_order_relaxed); }
}

// ---------------------------------------------------------------- reflection helpers (worker threads)
static uintptr_t FindObj(const char* name, const char* cls, const char* outer) {
    uintptr_t arr = g_base + RVA_GUObjectArray + 0x10, chunks; int32_t num;
    if (!Rd(arr, chunks) || !Rd(arr + 0x14, num) || !chunks) return 0;
    char nm[256], cn[128], on[256];
    for (int32_t i = 0; i < num; ++i) {
        uintptr_t chunk, obj;
        if (!Rd(chunks + 8 * (i / 65536), chunk) || !chunk || !Rd(chunk + 24 * (i % 65536), obj) || !obj) continue;
        if (!ObjName(obj, nm, sizeof(nm)) || strcmp(nm, name)) continue;
        if (cls) { ClassName(obj, cn, sizeof(cn)); if (strcmp(cn, cls)) continue; }
        if (outer) { uintptr_t o = 0; on[0] = 0; if (Rd(obj + 0x20, o) && o) ObjName(o, on, sizeof(on)); if (strcmp(on, outer)) continue; }
        return obj;
    }
    return 0;
}
static bool Param(uintptr_t fn, const char* pname, int& off, int& size) {
    uintptr_t f = 0; Rd(fn + 0x50, f);
    for (int k = 0; f && k < 40; ++k) {
        uint32_t ni = 0; char pn[128] = "";
        Rd(f + 0x28, ni); NameStr(ni, pn, sizeof(pn));
        if (!strcmp(pn, pname)) { Rd(f + 0x4C, off); Rd(f + 0x3C, size); return true; }
        if (!Rd(f + 0x20, f)) break;
    }
    return false;
}
struct PInfo { int off = -1, size = 0; };
struct Fn { uintptr_t fn = 0, cdo = 0; int size = 0; };
static Fn g_fnMakePath, g_fnConvPath, g_fnLoad, g_fnSpawnFx; static uintptr_t g_fnSetColor = 0; static PInfo g_pColName, g_pColVal;
static uintptr_t g_fnSetFloat = 0, g_fnSetVec3 = 0, g_fnActivate = 0, g_fnDeactivate = 0; static PInfo g_pFName, g_pFVal, g_pVName, g_pVVal, g_pReset;
static struct { PInfo path, pathRet, convIn, convRet, asset, loadRet, world, sys, loc, rot, scale, autoDestroy, autoActivate, preCull, fxRet; } g_p;
static bool ResolveFn(Fn& f, const char* outer, const char* name) {
    f.fn = FindObj(name, "Function", outer);
    char cdoName[128]; snprintf(cdoName, sizeof(cdoName), "Default__%s", outer);
    f.cdo = FindObj(cdoName, nullptr, nullptr);
    uint16_t s = 0; if (f.fn) Rd(f.fn + 0xB6, s); f.size = s;
    return f.fn && f.cdo && f.size > 0 && f.size <= 512;
}
static bool P(uintptr_t fn, const char* n, PInfo& p) { return Param(fn, n, p.off, p.size); }
static bool g_fnsReady = false;
static bool ResolveAll() {
    bool ok = ResolveFn(g_fnMakePath, "KismetSystemLibrary", "MakeSoftObjectPath")
            & ResolveFn(g_fnConvPath, "KismetSystemLibrary", "Conv_SoftObjPathToSoftObjRef")
            & ResolveFn(g_fnLoad, "KismetSystemLibrary", "LoadAsset_Blocking")
            & ResolveFn(g_fnSpawnFx, "NiagaraFunctionLibrary", "SpawnSystemAtLocation");
    static bool warned = false;
    if (!ok) { if (warned) return false; warned = true; Log("touch effects: engine functions not found (yet) (make path %p conv %p load %p spawn %p)", (void*)g_fnMakePath.fn, (void*)g_fnConvPath.fn, (void*)g_fnLoad.fn, (void*)g_fnSpawnFx.fn); return false; }
    ok = P(g_fnMakePath.fn, "PathString", g_p.path) & P(g_fnMakePath.fn, "ReturnValue", g_p.pathRet)
       & P(g_fnConvPath.fn, "SoftObjectPath", g_p.convIn) & P(g_fnConvPath.fn, "ReturnValue", g_p.convRet)
       & P(g_fnLoad.fn, "Asset", g_p.asset) & P(g_fnLoad.fn, "ReturnValue", g_p.loadRet)
       & P(g_fnSpawnFx.fn, "WorldContextObject", g_p.world) & P(g_fnSpawnFx.fn, "SystemTemplate", g_p.sys)
       & P(g_fnSpawnFx.fn, "Location", g_p.loc) & P(g_fnSpawnFx.fn, "Rotation", g_p.rot) & P(g_fnSpawnFx.fn, "Scale", g_p.scale)
       & P(g_fnSpawnFx.fn, "bAutoDestroy", g_p.autoDestroy) & P(g_fnSpawnFx.fn, "bAutoActivate", g_p.autoActivate)
       & P(g_fnSpawnFx.fn, "bPreCullCheck", g_p.preCull) & P(g_fnSpawnFx.fn, "ReturnValue", g_p.fxRet);
    if (!ok) Log("touch effects: a function parameter was not found");
    g_fnSetColor = FindObj("SetNiagaraVariableLinearColor", "Function", "NiagaraComponent");
    if (g_fnSetColor && !(P(g_fnSetColor, "InVariableName", g_pColName) & P(g_fnSetColor, "InValue", g_pColVal))) g_fnSetColor = 0;
    g_fnSetFloat = FindObj("SetNiagaraVariableFloat", "Function", "NiagaraComponent");
    if (g_fnSetFloat && !(P(g_fnSetFloat, "InVariableName", g_pFName) & P(g_fnSetFloat, "InValue", g_pFVal))) g_fnSetFloat = 0;
    g_fnSetVec3 = FindObj("SetNiagaraVariableVec3", "Function", "NiagaraComponent");
    if (g_fnSetVec3 && !(P(g_fnSetVec3, "InVariableName", g_pVName) & P(g_fnSetVec3, "InValue", g_pVVal))) g_fnSetVec3 = 0;
    g_fnDeactivate = FindObj("Deactivate", "Function", "ActorComponent");
    g_fnActivate = FindObj("Activate", "Function", "ActorComponent");
    if (g_fnActivate && !P(g_fnActivate, "bReset", g_pReset)) g_fnActivate = 0;
    Log("touch effects: setters colour %p float %p vec3 %p, activate %p", (void*)g_fnSetColor, (void*)g_fnSetFloat, (void*)g_fnSetVec3, (void*)g_fnActivate);
    return ok;
}

static bool ObjAlive(uintptr_t obj) { int32_t idx = -1; return obj && Rd(obj + 0xC, idx) && ObjectSlotHolds(idx, obj); }
static uintptr_t ObjItem(uintptr_t obj) {
    int32_t idx; if (!Rd(obj + 0xC, idx) || idx < 0) return 0;
    uintptr_t arr = g_base + RVA_GUObjectArray + 0x10, chunks, chunk;
    if (!Rd(arr, chunks) || !Rd(chunks + 8 * (idx / 65536), chunk) || !chunk) return 0;
    return chunk + 24 * (idx % 65536);
}

// ---------------------------------------------------------------- game thread
static uintptr_t GtLoad(const char* path) {
    // "/Game/A/B" -> "/Game/A/B.B"
    wchar_t w[300]; int n = 0; for (const char* c = path; *c && n < 280; ++c) w[n++] = (wchar_t)*c; w[n] = 0;
    const char* slash = strrchr(path, '/');
    if (slash && !strchr(slash, '.')) { w[n++] = L'.'; for (const char* c = slash + 1; *c && n < 298; ++c) w[n++] = (wchar_t)*c; w[n] = 0; }
    uint8_t a[512] = {}, b[512] = {}, c[512] = {};
    struct { const wchar_t* d; int32_t n, m; } str = { w, n + 1, n + 1 };
    memcpy(a + g_p.path.off, &str, sizeof(str));
    g_peOrig(g_fnMakePath.cdo, g_fnMakePath.fn, a);
    int cp = g_p.pathRet.size < g_p.convIn.size ? g_p.pathRet.size : g_p.convIn.size;
    memcpy(b + g_p.convIn.off, a + g_p.pathRet.off, cp);
    g_peOrig(g_fnConvPath.cdo, g_fnConvPath.fn, b);
    cp = g_p.convRet.size < g_p.asset.size ? g_p.convRet.size : g_p.asset.size;
    memcpy(c + g_p.asset.off, b + g_p.convRet.off, cp);
    g_peOrig(g_fnLoad.cdo, g_fnLoad.fn, c);
    uintptr_t obj = *(uintptr_t*)(c + g_p.loadRet.off);
    char cn[96] = ""; if (obj) ClassName(obj, cn, sizeof(cn));
    if (obj && strcmp(cn, "NiagaraSystem")) { Log("touch effects: %s is a %s, not a Niagara system", path, cn); return 0; }
    if (obj) {   // keep it loaded: add to the root set so garbage collection never frees it
        uintptr_t item = ObjItem(obj);
        if (item) _InterlockedOr((volatile long*)(item + 8), EFI_RootSet);
    }
    Log("touch effects: loaded %s -> %p", path, (void*)obj);
    return obj;
}
static void GtSetColor(uintptr_t comp, const wchar_t* name, const float c[4]) {
    if (!g_fnSetColor || !comp) return;
    uint8_t a[128] = {};
    struct { const wchar_t* d; int32_t n, m; } str = { name, (int32_t)wcslen(name) + 1, (int32_t)wcslen(name) + 1 };
    memcpy(a + g_pColName.off, &str, sizeof(str)); memcpy(a + g_pColVal.off, c, 16);
    g_peOrig(comp, g_fnSetColor, a);
}
static void GtSetFloat(uintptr_t comp, const wchar_t* name, float v) {
    if (!g_fnSetFloat || !comp) return;
    uint8_t a[128] = {}; struct { const wchar_t* d; int32_t n, m; } str = { name, (int32_t)wcslen(name) + 1, (int32_t)wcslen(name) + 1 };
    memcpy(a + g_pFName.off, &str, sizeof(str)); memcpy(a + g_pFVal.off, &v, 4); g_peOrig(comp, g_fnSetFloat, a);
}
static void GtSetVec3(uintptr_t comp, const wchar_t* name, const float v[3]) {
    if (!g_fnSetVec3 || !comp) return;
    uint8_t a[128] = {}; struct { const wchar_t* d; int32_t n, m; } str = { name, (int32_t)wcslen(name) + 1, (int32_t)wcslen(name) + 1 };
    memcpy(a + g_pVName.off, &str, sizeof(str)); memcpy(a + g_pVVal.off, v, 12); g_peOrig(comp, g_fnSetVec3, a);
}
// optional per-spawn overrides of the grass-step effect's user parameters (negative = leave as is)
struct FxParams { bool hasColor; float colMin[4], colMax[4]; float leafSize, rate, life, spread; };
static std::atomic<uint32_t> g_fxColoured{0};
static uintptr_t GtSpawn(uintptr_t world, uintptr_t sys, const float p[3], float scale, const FxParams* fp = nullptr) {
    bool any = fp && (fp->hasColor || fp->leafSize >= 0.f || fp->rate >= 0.f || fp->life >= 0.f || fp->spread >= 0.f);
    bool deferred = any && g_fnActivate;          // set parameters before the first particle spawns
    uint8_t a[512] = {};
    float sc[3] = { scale, scale, scale };
    *(uintptr_t*)(a + g_p.world.off) = world; *(uintptr_t*)(a + g_p.sys.off) = sys;
    memcpy(a + g_p.loc.off, p, 12); memcpy(a + g_p.scale.off, sc, 12);
    a[g_p.autoDestroy.off] = 1; a[g_p.autoActivate.off] = deferred ? 0 : 1; a[g_p.preCull.off] = 0;
    g_peOrig(g_fnSpawnFx.cdo, g_fnSpawnFx.fn, a);
    uintptr_t comp = *(uintptr_t*)(a + g_p.fxRet.off);
    if (comp && any) {
        if (fp->hasColor) { GtSetColor(comp, L"User.Leaf001_LitColor_Min", fp->colMin); GtSetColor(comp, L"User.Leaf001_LitColor_Max", fp->colMax); g_fxColoured.fetch_add(1); }
        if (fp->leafSize >= 0.f) { float v[3] = { fp->leafSize, fp->leafSize, fp->leafSize }; GtSetVec3(comp, L"User.Leaf001_Mesh Scale_Max", v); }
        if (fp->rate >= 0.f) GtSetFloat(comp, L"User.Leaf001_SpawnRate", fp->rate);
        if (fp->life >= 0.f) GtSetFloat(comp, L"User.Leaf001_Lifetime_Max", fp->life);
        if (fp->spread >= 0.f) { float v[3] = { fp->spread, fp->spread, fp->spread }; GtSetVec3(comp, L"User.Location_Box Size", v); }
        if (deferred) { uint8_t b[16] = {}; b[g_pReset.off] = 1; g_peOrig(comp, g_fnActivate, b); }
    }
    return comp;
}

// effect assets, by path (loaded lazily on the game thread)
struct FxAsset { char path[200]; uintptr_t obj; int state; ULONGLONG failedAt; uint32_t spawned; };   // state 0 = not loaded, 1 = loaded, 2 = failed
static FxAsset g_assets[16]; static int g_nAssets = 0; static SRWLOCK g_assetLock = SRWLOCK_INIT;
static int AssetIndex(const char* path) {   // worker thread
    AcquireSRWLockExclusive(&g_assetLock);
    int i = 0; while (i < g_nAssets && strcmp(g_assets[i].path, path)) ++i;
    if (i == g_nAssets) { if (g_nAssets >= 16) { ReleaseSRWLockExclusive(&g_assetLock); return -1; } strncpy(g_assets[i].path, path, 199); g_assets[i].obj = 0; g_assets[i].state = 0; ++g_nAssets; }
    ReleaseSRWLockExclusive(&g_assetLock);
    return i;
}

// spawn requests: single producer (touch thread), single consumer (game thread)
struct FxReq { int asset; float p[3]; float scale; uintptr_t world; int32_t worldIdx; FxParams fp; bool test; };
static FxReq g_fxq[64]; static std::atomic<uint32_t> g_fxqHead{0}, g_fxqTail{0};
static std::atomic<int> g_wantPreload{0};
static std::atomic<uint32_t> g_fxSpawned{0}, g_fxDropped{0};

static void GameThreadTick() {
    if (!g_fnsReady) return;
    if (g_wantPreload.exchange(0)) {      // load every configured effect once, so the first touch does not hitch
        int n; AcquireSRWLockShared(&g_assetLock); n = g_nAssets; ReleaseSRWLockShared(&g_assetLock);
        for (int i = 0; i < n; ++i) if (g_assets[i].state == 0) { uintptr_t o = GtLoad(g_assets[i].path); g_assets[i].obj = o; g_assets[i].state = o ? 1 : 2; g_assets[i].failedAt = GetTickCount64(); }
    }
    int done = 0;
    while (g_fxqTail.load() != g_fxqHead.load() && done < 4) {
        FxReq r = g_fxq[g_fxqTail.load() % 64]; g_fxqTail.fetch_add(1); ++done;
        if (r.asset < 0 || r.asset >= g_nAssets) continue;
        FxAsset& a = g_assets[r.asset];
        if (a.state == 0 || (a.state == 2 && GetTickCount64() - a.failedAt > 30000)) { a.obj = GtLoad(a.path); a.state = a.obj ? 1 : 2; a.failedAt = GetTickCount64(); }
        if (a.state != 1 || !ObjAlive(a.obj)) { a.state = 0; g_fxDropped.fetch_add(1); continue; }
        if (!ObjectSlotHolds(r.worldIdx, r.world)) { g_fxDropped.fetch_add(1); continue; }
        GtSpawn(r.world, a.obj, r.p, r.scale, &r.fp); g_fxSpawned.fetch_add(1); ++a.spawned;
    }
}

static void __fastcall HookProcessEvent(uintptr_t obj, uintptr_t fn, void* parms) {
    if (!g_inTick && GetCurrentThreadId() == g_gameTid) {
        ULONGLONG now = GetTickCount64();
        if (now - g_lastTick >= 15) {
            g_lastTick = now; g_inTick = true;
            __try { GameThreadTick(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("touch effects: game-thread fault"); g_fxqTail.store(g_fxqHead.load()); }
            g_inTick = false;
        }
    }
    g_peOrig(obj, fn, parms);
}

static DWORD MainThreadId() {
    DWORD pid = GetCurrentProcessId(), best = 0; ULONGLONG bestT = ~0ull;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te = { sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID); if (!h) continue;
        FILETIME c, e, k, u; if (GetThreadTimes(h, &c, &e, &k, &u)) { ULONGLONG t = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime; if (t < bestT) { bestT = t; best = te.th32ThreadID; } }
        CloseHandle(h);
    }
    CloseHandle(snap); return best;
}

static bool InstallPEHook() {
    uint8_t* target = (uint8_t*)(g_base + RVA_ProcessEvent);
    uint8_t cur[17]; if (!RdBytes((uintptr_t)target, cur, 17) || memcmp(cur, PE_PROLOGUE, 17)) { Log("touch effects: ProcessEvent prologue mismatch, particle bursts disabled"); return false; }
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); if (!tramp) return false;
    memcpy(tramp, PE_PROLOGUE, 17);
    tramp[17] = 0xFF; tramp[18] = 0x25; *(uint32_t*)(tramp + 19) = 0; *(uint64_t*)(tramp + 23) = (uint64_t)(target + 17);
    g_peOrig = (ProcessEvent_t)tramp;
    uint8_t patch[17]; patch[0] = 0xFF; patch[1] = 0x25; *(uint32_t*)(patch + 2) = 0; *(uint64_t*)(patch + 6) = (uint64_t)&HookProcessEvent;
    patch[14] = patch[15] = patch[16] = 0x90;
    DWORD old; if (!VirtualProtect(target, 17, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(target, patch, 17);
    VirtualProtect(target, 17, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 17);
    return true;
}

// ---------------------------------------------------------------- plant cache
static bool PlausibleCP(const uint8_t* e, const float ext[3]) {
    const float* p = (const float*)(e + 4); const float* q = (const float*)(e + 16);
    for (int i = 0; i < 3; ++i) if (!std::isfinite(p[i]) || fabsf(p[i]) > ext[i] * 1.2f + 200.f) return false;
    float ql = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return fabsf(ql - 1.f) < 0.02f;
}
struct Plant { float x, y, z; uint8_t cls; uint8_t dry; uint32_t idx; };
static constexpr float CELL = 200.f;
static inline uint64_t CellKey(int cx, int cy) { return ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cy; }
struct PlantSet {
    uintptr_t rd; float c[3], mn[3], mx[3]; uint32_t hash; int header, valid; ULONGLONG built, used;
    std::vector<Plant> plants; std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
};
static std::vector<PlantSet*> g_sets;
static SRWLOCK g_rdsLock = SRWLOCK_INIT;     // guards g_rds against RefreshFoliage (main loop)

struct RdSnap { uintptr_t rd; float c[3]; int nMat; uint8_t cls[MAX_MATS]; uint8_t dry[MAX_MATS]; };

static void BuildSet(PlantSet& s, const RdSnap& r) {
    s.plants.clear(); s.cells.clear();
    float b[6]; if (!RdBytes(r.rd + 0x60, b, sizeof(b))) return;
    float ext[3] = { (b[3] - b[0]) * 0.5f, (b[4] - b[1]) * 0.5f, (b[5] - b[2]) * 0.5f };
    for (int a = 0; a < 3; ++a) { s.mn[a] = b[a]; s.mx[a] = b[a + 3]; }
    uintptr_t cp = 0; int32_t n = 0; Rd(r.rd + OFF_RD_CP, cp); Rd(r.rd + OFF_RD_CPCOUNT, n); Rd(r.rd + OFF_RD_HASH, s.hash);
    s.header = n; s.valid = 0;
    if (!cp || n <= 0 || n > 4000000) return;
    std::vector<uint8_t> buf((size_t)n * 32);
    if (!RdBytes(cp, buf.data(), buf.size())) {     // read what we can, entry by entry
        for (int i = 0; i < n; ++i) if (!RdBytes(cp + 32ull * i, buf.data() + 32ull * i, 32)) memset(buf.data() + 32ull * i, 0, 32);
    }
    for (int i = 0; i < n; ++i) {
        const uint8_t* e = buf.data() + 32ull * i;
        if (!PlausibleCP(e, ext)) continue;
        ++s.valid;
        uint32_t m = *(const uint32_t*)e & 0x7FFFFFFF;
        uint8_t cl = (m < (uint32_t)r.nMat && m < MAX_MATS) ? r.cls[m] : 0;
        if (!cl) continue;
        const float* f = (const float*)(e + 4);
        Plant p = { f[0] + r.c[0], f[1] + r.c[1], f[2] + r.c[2], cl, r.dry[m], (uint32_t)i };
        s.cells[CellKey((int)floorf(p.x / CELL), (int)floorf(p.y / CELL))].push_back((uint32_t)s.plants.size());
        s.plants.push_back(p);
    }
}

// ---------------------------------------------------------------- touch detection thread
struct CharTouch { uintptr_t obj; float last[3]; double lastT; std::vector<uint64_t> inside; ULONGLONG lastSound[16]; bool has; };
static CharTouch g_ct[8];
static std::unordered_map<uint64_t, ULONGLONG> g_plantCooldown;
static uint32_t g_rng = 0x9E3779B9u;
static float Rand01() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return (g_rng & 0xFFFFFF) / 16777216.f; }
static std::atomic<uint32_t> g_touches{0}, g_soundsPlayed{0};
static std::atomic<uint32_t> g_touchByCls[16], g_burstByCls[16], g_soundBySize[4];

static void QueueFx(int cls, const Plant& p, uintptr_t world) {
    const ClassCfg& c = g_cls[cls];
    if (!c.vfx[0] || Rand01() >= c.vfxChance) return;
    static double tokens = 0; static ULONGLONG lastRefill = 0; ULONGLONG now = GetTickCount64();
    tokens += (now - lastRefill) * g_fxMaxVfxPerSec / 1000.0; lastRefill = now;
    if (tokens > g_fxMaxVfxPerSec) tokens = g_fxMaxVfxPerSec;
    if (tokens < 1.0) return;
    const char* path = (p.dry && g_dryVfx[0]) ? g_dryVfx : c.vfx;   // dry grass / dead plants: dust instead of leaves
    int a = AssetIndex(path); if (a < 0) return;
    uint32_t h = g_fxqHead.load(); if (h - g_fxqTail.load() >= 64) return;
    int32_t idx = -1; Rd(world + 0xC, idx);
    FxReq& r = g_fxq[h % 64]; r.test = false; r.asset = a; r.p[0] = p.x; r.p[1] = p.y; r.p[2] = p.z + c.vfxHeight; r.scale = c.vfxScale; r.world = world; r.worldIdx = idx;
    FxParams& f = r.fp; f.hasColor = c.hasColor; memcpy(f.colMin, c.colMin, 16); memcpy(f.colMax, c.colMax, 16);
    f.leafSize = c.leafSize; f.rate = c.leafRate; f.life = c.leafLife; f.spread = c.leafSpread;
    g_burstByCls[cls & 15].fetch_add(1, std::memory_order_relaxed);
    g_fxqHead.store(h + 1); tokens -= 1.0;
}

static bool BuildSetSafe(PlantSet* s, const RdSnap* r) { __try { BuildSet(*s, *r); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool ResolveAllSafe() { __try { return ResolveAll(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool g_hooked = false;
static ULONGLONG g_lastCache = 0, g_lastLog = 0, g_lastResolve = 0; static int g_lastCfgGen = -1;
static RdSnap g_snaps[256];

static void InteractTick() {
    bool hooked = g_hooked; ULONGLONG& lastCache = g_lastCache; ULONGLONG& lastLog = g_lastLog; ULONGLONG& lastResolve = g_lastResolve;
    int& lastCfgGen = g_lastCfgGen; RdSnap* snaps = g_snaps;
    {
        ULONGLONG now = GetTickCount64(); double tNow = NowMs();
        if (hooked && !g_fnsReady && g_nameBlocks && now - lastResolve > 5000) {
            lastResolve = now;
            g_fnsReady = ResolveAllSafe();
            if (g_fnsReady) Log("touch effects: engine functions ready");
        }
        if (g_cfgGen.load() != lastCfgGen) {     // (re)register the configured effect assets and preload them
            lastCfgGen = g_cfgGen.load();
            for (int k = 1; k < g_nCls; ++k) if (g_cls[k].vfx[0]) AssetIndex(g_cls[k].vfx);
            if (g_dryVfx[0]) AssetIndex(g_dryVfx);
            g_wantPreload.store(1);
        }
        if (!g_fxSounds && !g_fxVfx) return;
        // paused (game clock frozen), alt-tabbed or in a loading screen: nothing
        bool paused = now - g_viewGameTimeChanged.load(std::memory_order_relaxed) > 200;
        DWORD fgPid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
        bool focused = fgPid == GetCurrentProcessId();
        float cam[3], right[3]; AcquireSRWLockShared(&g_camLock); memcpy(cam, g_camPos, 12); memcpy(right, g_camRight, 12); ReleaseSRWLockShared(&g_camLock);

        // party characters near the camera
        uintptr_t objs[8]; float pos[8][3]; int nc = 0;
        {
            const CacheBuf& c = g_cache[g_cacheFront.load()];
            for (int i = 0; i < c.n && nc < 8; ++i) {
                if (!c.e[i].party || !ObjectSlotHolds(c.e[i].index, c.e[i].obj)) continue;
                float p[3]; if (!ActorPos(c.e[i].obj, p)) continue;
                float d2 = 0; for (int a = 0; a < 3; ++a) d2 += (p[a] - cam[a]) * (p[a] - cam[a]);
                if (d2 > g_fxRange * g_fxRange) continue;
                objs[nc] = c.e[i].obj; memcpy(pos[nc], p, 12); ++nc;
            }
        }
        // plant cache: blocks around the characters (rebuilt when they change or finish streaming)
        if (now - lastCache > 250) {
            lastCache = now;
            int ns = 0;
            AcquireSRWLockShared(&g_rdsLock);
            for (int i = 0; i < g_nRds && ns < 256; ++i) {
                const RdInfo& r = g_rds[i]; if (!r.plant) continue;
                float c[3], mn[3], mx[3]; if (!RdBounds(r.rd, c, mn, mx)) continue;
                bool near_ = false;
                for (int k = 0; k < nc && !near_; ++k)
                    near_ = pos[k][0] > mn[0] - 400 && pos[k][0] < mx[0] + 400 && pos[k][1] > mn[1] - 400 && pos[k][1] < mx[1] + 400 && pos[k][2] > mn[2] - 600 && pos[k][2] < mx[2] + 600;
                if (!near_) continue;
                RdSnap& s = snaps[ns++]; s.rd = r.rd; memcpy(s.c, r.c, 12); s.nMat = r.nMat; memcpy(s.cls, r.cls, sizeof(s.cls)); memcpy(s.dry, r.dry, sizeof(s.dry));
            }
            ReleaseSRWLockShared(&g_rdsLock);
            for (int i = 0; i < ns; ++i) {
                PlantSet* ps = nullptr;
                for (auto* s : g_sets) if (s->rd == snaps[i].rd) { ps = s; break; }
                uint32_t hash = 0; int32_t hdr = 0; Rd(snaps[i].rd + OFF_RD_HASH, hash); Rd(snaps[i].rd + OFF_RD_CPCOUNT, hdr);
                bool stale = !ps || ps->hash != hash || memcmp(ps->c, snaps[i].c, 12) || ps->header != hdr || (ps->valid < ps->header && now - ps->built > 2000);
                if (!ps) { ps = new PlantSet(); g_sets.push_back(ps); }
                if (stale) { ps->rd = snaps[i].rd; memcpy(ps->c, snaps[i].c, 12); if (!BuildSetSafe(ps, &snaps[i])) { ps->plants.clear(); ps->cells.clear(); } ps->built = now; }
                ps->used = now;
            }
            for (size_t i = 0; i < g_sets.size(); ) { if (now - g_sets[i]->used > 5000) { delete g_sets[i]; g_sets[i] = g_sets.back(); g_sets.pop_back(); } else ++i; }
            if (g_plantCooldown.size() > 4096) for (auto it = g_plantCooldown.begin(); it != g_plantCooldown.end(); ) { if (now - it->second > 3000) it = g_plantCooldown.erase(it); else ++it; }
        }
        // touches
        for (int k = 0; k < nc; ++k) {
            CharTouch* ct = nullptr;
            for (auto& t : g_ct) if (t.obj == objs[k]) { ct = &t; break; }
            if (!ct) { for (auto& t : g_ct) if (!t.obj || tNow - t.lastT > 3000) { ct = &t; t.obj = objs[k]; t.has = false; t.inside.clear(); break; } }
            if (!ct) continue;
            float speed = 0.f;
            if (ct->has && tNow > ct->lastT) speed = sqrtf((pos[k][0] - ct->last[0]) * (pos[k][0] - ct->last[0]) + (pos[k][1] - ct->last[1]) * (pos[k][1] - ct->last[1])) / (float)((tNow - ct->lastT) / 1000.0);
            if (speed > 3000.f) { ct->inside.clear(); speed = 0.f; }     // teleport
            memcpy(ct->last, pos[k], 12); ct->lastT = tNow; ct->has = true;
            std::vector<uint64_t> inside;
            const Plant* bestP[16] = {}; float bestD[16]; for (int j = 0; j < 16; ++j) bestD[j] = 1e9f;
            for (auto* s : g_sets) {
                if (pos[k][0] < s->mn[0] - 300 || pos[k][0] > s->mx[0] + 300 || pos[k][1] < s->mn[1] - 300 || pos[k][1] > s->mx[1] + 300) continue;
                int cx = (int)floorf(pos[k][0] / CELL), cy = (int)floorf(pos[k][1] / CELL);
                for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) {
                    auto it = s->cells.find(CellKey(cx + dx, cy + dy)); if (it == s->cells.end()) continue;
                    for (uint32_t pi : it->second) {
                        const Plant& p = s->plants[pi];
                        float dz = pos[k][2] - p.z; if (dz < -60.f || dz > 250.f) continue;
                        float d = sqrtf((p.x - pos[k][0]) * (p.x - pos[k][0]) + (p.y - pos[k][1]) * (p.y - pos[k][1]));
                        const ClassCfg& c = g_cls[p.cls < g_nCls ? p.cls : 0];
                        if (d > c.radius) continue;
                        uint64_t key = ((uint64_t)(s->rd & 0xFFFFFFFFFFull) << 22) ^ p.idx;
                        inside.push_back(key);
                        if (std::find(ct->inside.begin(), ct->inside.end(), key) != ct->inside.end()) continue;   // already inside
                        if (speed < g_fxMinSpeed || paused || !focused) continue;
                        auto cd = g_plantCooldown.find(key); if (cd != g_plantCooldown.end() && now - cd->second < 1200) continue;
                        g_plantCooldown[key] = now; g_touches.fetch_add(1, std::memory_order_relaxed); g_touchByCls[p.cls & 15].fetch_add(1, std::memory_order_relaxed);
                        if (d < bestD[p.cls & 15]) { bestD[p.cls & 15] = d; bestP[p.cls & 15] = &p; }
                        if (g_fxVfx && g_fnsReady) QueueFx(p.cls, p, objs[k]);
                    }
                }
            }
            ct->inside.swap(inside);
            // one rustle per plant type touched (each type has its own sound size, volume and pacing)
            static const ULONGLONG GAP[4] = { 0, 380, 300, 180 };   // ms between rustles of one character, by size
            for (int cl = 1; cl < 16 && cl < g_nCls; ++cl) {
                const Plant* bp = bestP[cl]; const ClassCfg& c = g_cls[cl];
                if (!bp || !g_fxSounds || c.sound <= 0 || now - ct->lastSound[cl] <= GAP[c.sound & 3]) continue;
                float fresh = (float)(now - ct->lastSound[cl]) / 900.f; if (fresh > 1.f) fresh = 1.f; fresh = 0.45f + 0.55f * fresh;
                ct->lastSound[cl] = now;
                float vx = bp->x - cam[0], vy = bp->y - cam[1], vz = bp->z - cam[2];
                float dist = sqrtf(vx * vx + vy * vy + vz * vz);
                float att = dist < 600.f ? 1.f : 1.f - (dist - 600.f) / (g_fxRange - 600.f > 1.f ? g_fxRange - 600.f : 1.f);
                if (att <= 0.f) continue;
                float pace = 0.45f + speed / 700.f; if (pace > 1.f) pace = 1.f;
                float hl = sqrtf(vx * vx + vy * vy); float pan = hl > 1.f ? (vx * right[0] + vy * right[1]) / hl * 0.7f : 0.f;
                PlayRustle(c.sound - 1, g_fxVolume * c.soundVol * pace * att * fresh * (0.8f + 0.2f * Rand01()), pan, 0.9f + 0.2f * Rand01());
                g_soundsPlayed.fetch_add(1, std::memory_order_relaxed); g_soundBySize[c.sound & 3].fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (g_debugLog && now - lastLog > 10000) {
            lastLog = now;
            size_t pl = 0; for (auto* s : g_sets) pl += s->plants.size();
            char per[400] = ""; int L = 0;
            for (int i = 0; i < g_nAssets && L < 360; ++i) { const char* nm = strrchr(g_assets[i].path, '/'); L += snprintf(per + L, sizeof(per) - L, " %s:%u", nm ? nm + 1 : g_assets[i].path, g_assets[i].spawned); }
            Log("touch effects: %d characters, %zu blocks cached (%zu plants), touches %u, rustles %u, bursts %u (dropped %u, coloured %u)%s |%s",
                nc, g_sets.size(), pl, g_touches.load(), g_soundsPlayed.load(), g_fxSpawned.load(), g_fxDropped.load(), g_fxColoured.load(), paused ? ", paused" : "", per);
            char cs[300]; int C = 0;
            for (int k = 1; k < g_nCls && C < 280; ++k) C += snprintf(cs + C, sizeof(cs) - C, " %d:%u/%u", k, g_touchByCls[k].load(), g_burstByCls[k].load());
            Log("   per class touches/bursts:%s | rustles small %u medium %u large %u", cs, g_soundBySize[1].load(), g_soundBySize[2].load(), g_soundBySize[3].load());
        }
    }
}

static void InteractTickSafe() { __try { InteractTick(); } __except (EXCEPTION_EXECUTE_HANDLER) { static int n = 0; if (n++ < 5) Log("touch effects: fault in touch detection"); } }
static DWORD WINAPI InteractThread(LPVOID) {
    g_gameTid = MainThreadId();
    __try { RustleInit(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("touch effects: audio init fault"); }
    g_hooked = InstallPEHook();
    Log("touch effects: game thread %lu, ProcessEvent hook %s", g_gameTid, g_hooked ? "installed" : "missing");
    for (;;) { Sleep(33); InteractTickSafe(); }
    return 0;
}
