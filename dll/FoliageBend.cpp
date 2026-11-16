// Final Foliage Reenabled (FinalFoliage.dll) - feeds character positions to FF7 Rebirth's foliage shaders.
//
// How it works
//  * A background thread scans GUObjectArray every 250 ms and caches every live object that derives from
//    ACharacter (party, enemies, NPCs, chocobos).
//  * The GDynamicRHI vtable slot RHICreateUniformBuffer is replaced. When the engine creates a "View" uniform
//    buffer (layout == FViewUniformShaderParameters layout), the nearest characters to the camera are written into
//    fields no PC shader reads (verified: 0 of 34,323 shaders): View_MobileSkyIrradianceEnvironmentMap[0..6] and
//    View_XRPassthroughCameraUVs[0] carry (x, y, z world, 1); View_XRPassthroughCameraUVs[1] carries a magic header
//    so the patched foliage shaders ignore the fields when this DLL isn't running.
//  * Every memory read of game objects is guarded with SEH; a bad pointer is skipped, never a crash.
//
// Addresses are for ff7rebirth_.exe build sha256 752807180c4e... (verified at startup; otherwise the DLL idles).
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <intrin.h>
#include <cctype>
#include <algorithm>

// ---------------------------------------------------------------- build-specific RVAs
static constexpr uintptr_t RVA_GDynamicRHI      = 0x8F49BC0;  // FDynamicRHI* GDynamicRHI
static constexpr uintptr_t RVA_ViewLayout       = 0x90BCAF0;  // FViewUniformShaderParameters layout (meta+0x30)
static constexpr uintptr_t RVA_GUObjectArray    = 0x871EB28;  // FUObjectArray
static constexpr uintptr_t RVA_ACharacterClass  = 0x9176520;  // UClass* cache for ACharacter
static constexpr uintptr_t RVA_PlayerCharClass  = 0x90F7700;  // UClass* cache for AEndPlayerCharacter
static constexpr int       VT_CreateUniformBuffer = 0x118 / 8;
// Milestone 3 diagnostics: attack (damage source) and weapon classes, world/level access
static constexpr uintptr_t RVA_DamageSrcClass   = 0x90F0DB8;  // AEndBattleDamageSourceActor
static constexpr uintptr_t RVA_WeaponClass      = 0x90F8328;  // AEndWeaponSkeletalMeshActor
static constexpr uintptr_t RVA_WorldClass       = 0x9179EE0;  // UWorld
static constexpr uintptr_t RVA_SphereClass      = 0x9179A38;  // USphereComponent
static constexpr uintptr_t RVA_CapsuleClass     = 0x9176538;  // UCapsuleComponent
static constexpr uintptr_t RVA_ShapeClass       = 0x91795E0;  // UShapeComponent
static constexpr int OFF_World_PersistentLevel = 0x30;
static constexpr int OFF_Level_Actors          = 0x98;     // TArray<AActor*> (UE 4.26 layout, verified in log)
static constexpr int OFF_Shape_A               = 0x498;    // SphereRadius / CapsuleHalfHeight
static constexpr int OFF_Shape_B               = 0x49C;    // CapsuleRadius
// Signature check: bytes of the create-uniform-buffer call site that loads the View layout.
static constexpr uintptr_t RVA_SigSite = 0x10BE2DC;           // lea r9,[rip+ViewLayout]; mov [rsp+28],ebx
static const uint8_t SIG_BYTES[] = { 0x4C, 0x8D, 0x0D, 0x0D, 0xE8, 0xFF, 0x07, 0x89, 0x5C, 0x24, 0x28 };

// ---------------------------------------------------------------- engine layout (UE 4.26)
static constexpr int OFF_UObject_Flags = 0x08;     // EObjectFlags
static constexpr int OFF_UObject_Class = 0x10;
static constexpr int OFF_UStruct_Super = 0x40;     // verified at runtime, see log
static constexpr int OFF_Actor_Root    = 0x138;    // AActor::RootComponent
static constexpr int OFF_Scene_RelLoc  = 0x11C;    // USceneComponent::RelativeLocation
static constexpr int OFF_Scene_C2W     = 0x1C0;    // USceneComponent::ComponentToWorld (FTransform)
static constexpr int RF_ClassDefaultObject = 0x10, RF_ArchetypeObject = 0x20;

// ---------------------------------------------------------------- View struct fields we write
static constexpr int OFF_View_CameraOrigin = 1200;  // View_WorldCameraOrigin (float3)
static constexpr int OFF_View_SlotsA       = 4016;  // View_MobileSkyIrradianceEnvironmentMap[7] (float4)
static constexpr int OFF_View_SlotsB       = 4640;  // View_XRPassthroughCameraUVs[2] (float4)
static constexpr float MAGIC_X = 12345.5f, MAGIC_Y = -6789.25f, MAGIC_Z = 31.75f, MAGIC_W = 7.0f;
static constexpr int   MAX_SLOTS = 8;
static constexpr float MAX_DIST = 6000.f;           // only characters within 60 m of the camera

static uintptr_t g_base = 0;
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logLock;

static HMODULE g_module = nullptr;   // this DLL (its folder holds the ini and log)
static int g_debugLog = 0;   // [global] debug_log=1 in FinalFoliage.ini: periodic statistics in the log
static void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

// ---------------------------------------------------------------- guarded reads
template <class T> static bool Rd(uintptr_t addr, T& out) {
    __try { out = *reinterpret_cast<const T*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RdBytes(uintptr_t addr, void* out, size_t n) {
    __try { memcpy(out, reinterpret_cast<const void*>(addr), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------- character cache (written by scanner)
struct Cached { uintptr_t obj; int32_t index; bool party; };
static constexpr int MAX_CACHE = 512;
struct CacheBuf { Cached e[MAX_CACHE]; int n; };
static CacheBuf g_cache[2];
static std::atomic<int> g_cacheFront{0};
static int g_posMode = 0;   // 0 = ComponentToWorld translation, 1 = RelativeLocation

static bool ObjectSlotHolds(int32_t index, uintptr_t obj) {
    uintptr_t arr = g_base + RVA_GUObjectArray + 0x10, chunks; int32_t num;
    if (!Rd(arr + 0x00, chunks) || !Rd(arr + 0x14, num) || index < 0 || index >= num) return false;
    uintptr_t chunk, item;
    if (!Rd(chunks + 8 * (index / 65536), chunk) || !chunk) return false;
    if (!Rd(chunk + 24 * (index % 65536), item)) return false;
    return item == obj;
}

static bool IsChildOf(uintptr_t cls, uintptr_t target) {
    for (int i = 0; i < 64 && cls; ++i) {
        if (cls == target) return true;
        if (!Rd(cls + OFF_UStruct_Super, cls)) return false;
    }
    return false;
}

static bool ActorPos(uintptr_t obj, float out[3]) {
    uintptr_t root;
    if (!Rd(obj + OFF_Actor_Root, root) || !root) return false;
    int off = g_posMode == 0 ? OFF_Scene_C2W + 0x10 : OFF_Scene_RelLoc;
    float p[3];
    if (!Rd(root + off, p[0]) || !Rd(root + off + 4, p[1]) || !Rd(root + off + 8, p[2])) return false;
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    if (fabsf(p[0]) > 1e7f || fabsf(p[1]) > 1e7f || fabsf(p[2]) > 1e7f) return false;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
    return true;
}

static void ScanObjects(bool verbose) {
    uintptr_t charCls = 0, playerCls = 0;
    Rd(g_base + RVA_ACharacterClass, charCls);
    Rd(g_base + RVA_PlayerCharClass, playerCls);
    if (!charCls) return;
    uintptr_t arr = g_base + RVA_GUObjectArray + 0x10, chunks; int32_t num;
    if (!Rd(arr, chunks) || !Rd(arr + 0x14, num) || !chunks || num <= 0) return;
    int back = 1 - g_cacheFront.load();
    CacheBuf& b = g_cache[back]; b.n = 0;
    int players = 0, total = 0;
    for (int32_t i = 0; i < num && b.n < MAX_CACHE; ++i) {
        uintptr_t chunk, obj, cls; int32_t flags;
        if ((i % 65536) == 0 || i == 0) { if (!Rd(chunks + 8 * (i / 65536), chunk)) break; }
        if (!Rd(chunks + 8 * (i / 65536), chunk) || !chunk) continue;
        if (!Rd(chunk + 24 * (i % 65536), obj) || !obj) continue;
        ++total;
        if (!Rd(obj + OFF_UObject_Class, cls) || !cls) continue;
        if (!Rd(obj + OFF_UObject_Flags, flags) || (flags & (RF_ClassDefaultObject | RF_ArchetypeObject))) continue;
        if (!IsChildOf(cls, charCls)) continue;
        bool party = playerCls && IsChildOf(cls, playerCls);
        b.e[b.n++] = { obj, i, party };
        if (party) ++players;
    }
    g_cacheFront.store(back);
    if (verbose) Log("scan: %d objects, %d characters (%d party-class)", total, b.n, players);
}

// Pick which world-position source is valid, by checking the FTransform of the first cached character.
static void ChoosePositionMode() {
    const CacheBuf& c = g_cache[g_cacheFront.load()];
    for (int k = 0; k < c.n && k < 8; ++k) {
        uintptr_t root; if (!Rd(c.e[k].obj + OFF_Actor_Root, root) || !root) continue;
        float q[4], t[3], s[3], r[3];
        bool ok = true;
        for (int j = 0; j < 4; ++j) ok &= Rd(root + OFF_Scene_C2W + 4 * j, q[j]);
        for (int j = 0; j < 3; ++j) ok &= Rd(root + OFF_Scene_C2W + 0x10 + 4 * j, t[j]);
        for (int j = 0; j < 3; ++j) ok &= Rd(root + OFF_Scene_C2W + 0x20 + 4 * j, s[j]);
        for (int j = 0; j < 3; ++j) ok &= Rd(root + OFF_Scene_RelLoc + 4 * j, r[j]);
        if (!ok) continue;
        float qn = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
        Log("char %d: C2W quat=(%.3f %.3f %.3f %.3f |%.3f|) trans=(%.1f %.1f %.1f) scale=(%.2f %.2f %.2f) relLoc=(%.1f %.1f %.1f)",
            k, q[0], q[1], q[2], q[3], qn, t[0], t[1], t[2], s[0], s[1], s[2], r[0], r[1], r[2]);
        bool c2wValid = fabsf(qn - 1.f) < 0.05f && fabsf(s[0] - 1.f) < 0.5f && fabsf(s[2] - 1.f) < 0.5f;
        g_posMode = c2wValid ? 0 : 1;
        Log("position source: %s", c2wValid ? "ComponentToWorld" : "RelativeLocation (fallback)");
        return;
    }
}

// ---------------------------------------------------------------- milestone 3: weapon swing gusts
// Weapons (AEndWeaponSkeletalMeshActor) are tracked at ~120 Hz. While a weapon moves faster than GUST_MIN_SPEED,
// short-lived "gust" points are dropped along its path (blade centre from the component's world Bounds). Each gust
// carries the horizontal swing direction (or none = radial push for mostly vertical swings) and a strength that fades
// out over GUST_LIFE_MS, so grass leans with the swing and springs back.
static constexpr uintptr_t RVA_DSCompClass = 0x90F0D28;   // UEndBattleDamageSourceComponent (diagnostics)
static constexpr int   ATTACK_SLOTS = 10;
static constexpr int   OFF_View_Attack = 79 * 16;            // View regs 79..98 (Prev* matrices, unused by all shaders)
static constexpr float GUST_LIFE_MS = 450.f, GUST_MIN_SPEED = 600.f, GUST_FULL_SPEED = 3000.f, GUST_RADIUS = 160.f;
static constexpr float GUST_MAX_SPEED = 6000.f;              // faster = teleport/attach jump, ignored
static constexpr int   GUST_EMIT_MS = 40;

static uintptr_t g_weapons[32]; static std::atomic<int> g_nWeapons{0};
static std::atomic<int> g_boundsOff{-2};                     // -2 = not probed yet, -1 = not found
struct Gust { float p[3]; float dir[2]; float s0; ULONGLONG t; };
static Gust g_gusts[128]; static int g_nGusts = 0;
static SRWLOCK g_gustLock = SRWLOCK_INIT;
static std::atomic<uint32_t> g_gustsEmitted{0};

static void ScanWeaponsAndDamageSources(bool verbose) {
    uintptr_t weaponCls = 0, dsCls = 0;
    Rd(g_base + RVA_WeaponClass, weaponCls); Rd(g_base + RVA_DSCompClass, dsCls);
    uintptr_t arr = g_base + RVA_GUObjectArray + 0x10, chunks; int32_t num;
    if (!Rd(arr, chunks) || !Rd(arr + 0x14, num) || !chunks) return;
    uintptr_t curDs[256]; int nCur = 0; int nw = 0;
    uintptr_t ismCls = 0, hismCls = 0, folCls = 0, meCls = 0;
    Rd(g_base + 0x9176DA8, ismCls); Rd(g_base + 0x9176CF8, hismCls); Rd(g_base + 0x9166CE0, folCls); Rd(g_base + 0x91774B8, meCls);
    int nIsm = 0, nHism = 0, nFol = 0, nIsmNear = 0, nMe = 0;
    float player[3] = {}; bool havePlayer = false;
    {   // first party-class character position = player reference
        uintptr_t playerCls = 0; Rd(g_base + RVA_PlayerCharClass, playerCls);
        const CacheBuf& c = g_cache[g_cacheFront.load()];
        for (int k = 0; k < c.n && !havePlayer; ++k) {
            uintptr_t cls; if (Rd(c.e[k].obj + OFF_UObject_Class, cls) && IsChildOf(cls, playerCls)) havePlayer = ActorPos(c.e[k].obj, player);
        }
    }
    for (int32_t i = 0; i < num; ++i) {
        uintptr_t chunk, obj, cls; int32_t flags;
        if (!Rd(chunks + 8 * (i / 65536), chunk) || !chunk) continue;
        if (!Rd(chunk + 24 * (i % 65536), obj) || !obj) continue;
        if (!Rd(obj + OFF_UObject_Class, cls) || !cls) continue;
        if (!Rd(obj + OFF_UObject_Flags, flags) || (flags & (RF_ClassDefaultObject | RF_ArchetypeObject))) continue;
        if (weaponCls && nw < 32 && IsChildOf(cls, weaponCls)) { g_weapons[nw++] = obj; continue; }
        if (dsCls && nCur < 256 && IsChildOf(cls, dsCls)) { curDs[nCur++] = obj; continue; }
        if (verbose) {   // renderer census for locating non-swaying foliage
            if (ismCls && IsChildOf(cls, ismCls)) {
                ++nIsm; if (hismCls && IsChildOf(cls, hismCls)) ++nHism; if (folCls && IsChildOf(cls, folCls)) ++nFol;
                float t[3];
                if (Rd(obj + OFF_Scene_C2W + 0x10, t[0]) && Rd(obj + OFF_Scene_C2W + 0x14, t[1]) && Rd(obj + OFF_Scene_C2W + 0x18, t[2])) {
                    float dx = t[0] - player[0], dy = t[1] - player[1];
                    if (havePlayer && dx*dx + dy*dy < 3000.f * 3000.f) ++nIsmNear;
                }
            } else if (meCls && IsChildOf(cls, meCls)) ++nMe;
        }
    }
    if (verbose) Log("renderer census: ISM %d (HISM %d, foliage-ISM %d, within 30 m of player %d), MassiveEnvironment components %d",
                     nIsm, nHism, nFol, nIsmNear, nMe);
    g_nWeapons.store(nw);
    if (verbose) Log("weapons tracked: %d, damage-source components alive: %d, gusts emitted: %u", nw, nCur, g_gustsEmitted.load());
}

// Find USceneComponent::Bounds (FBoxSphereBounds: origin, extent, radius) by checking candidate offsets on a weapon.
static void ProbeBoundsOffset(uintptr_t comp) {
    float t[3];
    for (int j = 0; j < 3; ++j) if (!Rd(comp + OFF_Scene_C2W + 0x10 + 4 * j, t[j])) return;
    for (int off = 0x80; off <= 0x1B0; off += 4) {
        float b[7]; bool ok = true;
        for (int j = 0; j < 7 && ok; ++j) ok = Rd(comp + off + 4 * j, b[j]) && std::isfinite(b[j]);
        if (!ok) continue;
        float dx = b[0] - t[0], dy = b[1] - t[1], dz = b[2] - t[2];
        float el = sqrtf(b[3]*b[3] + b[4]*b[4] + b[5]*b[5]);
        if (dx*dx + dy*dy + dz*dz < 400.f*400.f && b[3] > 0.5f && b[4] > 0.5f && b[5] > 0.5f && el < 1500.f &&
            fabsf(b[6] - el) < 0.5f + 0.01f * el) {
            g_boundsOff.store(off);
            Log("weapon Bounds offset %#x: origin (%.0f %.0f %.0f) extent (%.0f %.0f %.0f) radius %.0f, root at (%.0f %.0f %.0f)",
                off, b[0], b[1], b[2], b[3], b[4], b[5], b[6], t[0], t[1], t[2]);
            return;
        }
    }
    g_boundsOff.store(-1);
    Log("weapon Bounds offset not found; using weapon root position");
}

static bool WeaponPoint(uintptr_t weapon, float p[3]) {
    uintptr_t root;
    if (!Rd(weapon + OFF_Actor_Root, root) || !root) return false;
    int off = g_boundsOff.load();
    if (off == -2) { ProbeBoundsOffset(root); off = g_boundsOff.load(); }
    if (off >= 0) {
        float b[3];
        if (Rd(root + off, b[0]) && Rd(root + off + 4, b[1]) && Rd(root + off + 8, b[2]) &&
            std::isfinite(b[0]) && std::isfinite(b[1]) && std::isfinite(b[2])) { memcpy(p, b, 12); return true; }
    }
    return ActorPos(weapon, p);
}

static DWORD WINAPI GustThread(LPVOID) {
    float prev[32][3] = {}; bool hasPrev[32] = {}; ULONGLONG lastEmit[32] = {}; uintptr_t prevObj[32] = {};
    ULONGLONG prevT = GetTickCount64();
    for (;;) {
        Sleep(8);
        ULONGLONG now = GetTickCount64(); float dt = (now - prevT) / 1000.f; prevT = now;
        if (dt <= 0.f) continue;
        int nw = g_nWeapons.load();
        for (int w = 0; w < nw; ++w) {
            uintptr_t obj = g_weapons[w];
            if (prevObj[w] != obj) { prevObj[w] = obj; hasPrev[w] = false; }
            float p[3]; if (!WeaponPoint(obj, p)) { hasPrev[w] = false; continue; }
            if (hasPrev[w]) {
                float vx = (p[0] - prev[w][0]) / dt, vy = (p[1] - prev[w][1]) / dt, vz = (p[2] - prev[w][2]) / dt;
                float sp = sqrtf(vx*vx + vy*vy + vz*vz), hs = sqrtf(vx*vx + vy*vy);
                if (sp > GUST_MIN_SPEED && sp < GUST_MAX_SPEED && now - lastEmit[w] >= (ULONGLONG)GUST_EMIT_MS) {
                    Gust g; memcpy(g.p, p, 12); g.t = now;
                    g.s0 = fminf(1.f, (sp - GUST_MIN_SPEED) / (GUST_FULL_SPEED - GUST_MIN_SPEED));
                    if (hs > 0.3f * sp) { g.dir[0] = vx / hs; g.dir[1] = vy / hs; } else { g.dir[0] = g.dir[1] = 0.f; }
                    AcquireSRWLockExclusive(&g_gustLock);
                    int slot = g_nGusts < 128 ? g_nGusts++ : 0;
                    if (slot == 0 && g_nGusts == 128) { ULONGLONG oldest = ~0ull; for (int k = 0; k < 128; ++k) if (g_gusts[k].t < oldest) { oldest = g_gusts[k].t; slot = k; } }
                    g_gusts[slot] = g;
                    ReleaseSRWLockExclusive(&g_gustLock);
                    lastEmit[w] = now; g_gustsEmitted.fetch_add(1);
                }
            }
            memcpy(prev[w], p, 12); hasPrev[w] = true;
        }
        // prune expired gusts
        AcquireSRWLockExclusive(&g_gustLock);
        for (int k = 0; k < g_nGusts; ) {
            if (now - g_gusts[k].t > (ULONGLONG)GUST_LIFE_MS) g_gusts[k] = g_gusts[--g_nGusts]; else ++k;
        }
        ReleaseSRWLockExclusive(&g_gustLock);
    }
}

// Called from FillView: write the strongest live gusts near the camera into View regs 79..98.
static void FillAttackSlots(uint8_t* v, const float cam[3]) {
    float slots[ATTACK_SLOTS][2][4] = {};
    float strength[ATTACK_SLOTS] = {}; int n = 0;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_gustLock);
    for (int k = 0; k < g_nGusts; ++k) {
        const Gust& g = g_gusts[k];
        float age = (float)(now - g.t) / GUST_LIFE_MS; if (age >= 1.f) continue;
        float s = g.s0 * (1.f - age) * (1.f - age);
        float dx = g.p[0] - cam[0], dy = g.p[1] - cam[1];
        if (dx*dx + dy*dy > 4000.f * 4000.f || s <= 0.01f) continue;
        int slot = n < ATTACK_SLOTS ? n++ : -1;
        if (slot < 0) { int w = 0; for (int j = 1; j < ATTACK_SLOTS; ++j) if (strength[j] < strength[w]) w = j; if (s <= strength[w]) continue; slot = w; }
        strength[slot] = s;
        slots[slot][0][0] = g.p[0]; slots[slot][0][1] = g.p[1]; slots[slot][0][2] = g.p[2]; slots[slot][0][3] = GUST_RADIUS;
        slots[slot][1][0] = g.dir[0]; slots[slot][1][1] = g.dir[1]; slots[slot][1][2] = s; slots[slot][1][3] = 0.f;
    }
    ReleaseSRWLockShared(&g_gustLock);
    memcpy(v + OFF_View_Attack, slots, sizeof(slots));
}

// ---------------------------------------------------------------- the hook
using CreateUB_t = void* (__fastcall*)(void* self, void* ret, const void* contents, const void* layout, uint32_t usage, uint32_t validation);
static CreateUB_t g_origCreateUB = nullptr;
static std::atomic<uint32_t> g_viewWrites{0}, g_lastActive{0};

static void WriteFoliageView(uint8_t* v);
static void SetCamPos(const float* cam);
static void SetCamRight(const uint8_t* v);
static int WakeSegments(const uintptr_t* objs, const float (*pos)[3], int n, float segs[][8], int maxSegs, double nowMs);
static constexpr int OFF_View_GameTime = 2760;
static std::atomic<uint32_t> g_wakeSkipped{0}, g_wakeResets{0};
static float CharStrength();
static constexpr int OFF_View_WakeA = 40 * 16, OFF_View_WakeB = 64 * 16;   // ViewToClipNoAA, MobileMultiviewShadowTransform (unused)

static void FillView(uint8_t* v) {
    float cam[3];
    memcpy(cam, v + OFF_View_CameraOrigin, 12);
    SetCamPos(cam);
    SetCamRight(v);
    struct Hit { float p[3]; float d2; uintptr_t obj; bool party; } best[MAX_SLOTS]; int nb = 0;
    const CacheBuf& c = g_cache[g_cacheFront.load()];
    for (int k = 0; k < c.n; ++k) {
        if (!ObjectSlotHolds(c.e[k].index, c.e[k].obj)) continue;
        float p[3];
        if (!ActorPos(c.e[k].obj, p)) continue;
        float dx = p[0] - cam[0], dy = p[1] - cam[1], dz = p[2] - cam[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > MAX_DIST * MAX_DIST) continue;
        if (nb < MAX_SLOTS) { best[nb++] = { {p[0], p[1], p[2]}, d2, c.e[k].obj, c.e[k].party }; continue; }
        int w = 0; for (int j = 1; j < nb; ++j) if (best[j].d2 > best[w].d2) w = j;
        if (d2 < best[w].d2) best[w] = { {p[0], p[1], p[2]}, d2, c.e[k].obj, c.e[k].party };
    }
    // nearest first, so trail ghosts go to the characters closest to the camera
    for (int a = 1; a < nb; ++a) for (int b = a; b > 0 && best[b].d2 < best[b - 1].d2; --b) { Hit t = best[b]; best[b] = best[b - 1]; best[b - 1] = t; }
    float slots[MAX_SLOTS][4] = {};
    float chs = CharStrength();
    for (int j = 0; j < nb; ++j) { memcpy(slots[j], best[j].p, 12); slots[j][3] = chs; }
    {   // wake behind the 2 party members nearest the camera; a holder keeps its wake while it is among the
        // 3 nearest party members (no flip-flopping). Without party members nearby, the nearest characters.
        static uintptr_t holders[2] = {};
        int cand[MAX_SLOTS], nc = 0;
        for (int j = 0; j < nb; ++j) if (best[j].party) cand[nc++] = j;
        if (!nc) for (int j = 0; j < nb; ++j) cand[nc++] = j;
        bool mainView = nb && best[0].d2 < 2500.f * 2500.f;          // only the gameplay camera may change holders
        if (mainView) {
            int lim = nc < 3 ? nc : 3;
            for (int h = 0; h < 2; ++h) {
                bool keep = false; for (int j = 0; j < lim; ++j) if (best[cand[j]].obj == holders[h]) keep = true;
                if (!keep) holders[h] = 0;
            }
            for (int h = 0; h < 2; ++h) if (!holders[h])
                for (int j = 0; j < lim; ++j) { uintptr_t o = best[cand[j]].obj; if (o != holders[0] && o != holders[1]) { holders[h] = o; break; } }
        }
        uintptr_t objs[2]; float ps[2][3]; int nw = 0;
        for (int h = 0; h < 2; ++h) for (int j = 0; j < nb; ++j) if (holders[h] && best[j].obj == holders[h]) { objs[nw] = holders[h]; memcpy(ps[nw], best[j].p, 12); ++nw; break; }
        float segs[4][8] = {};
        float gameTime = 0.f; memcpy(&gameTime, v + OFF_View_GameTime, 4);
        int nsg = WakeSegments(objs, ps, nw, segs, 4, (double)gameTime * 1000.0);
        memcpy(v + OFF_View_WakeA, segs[0], 64);
        memcpy(v + OFF_View_WakeB, segs[2], 64);
        static ULONGLONG lastLog = 0; ULONGLONG t = GetTickCount64();
        if (g_debugLog && t - lastLog > 2000 && nb) {
            lastLog = t;
            char b2[600]; int L = snprintf(b2, sizeof(b2), "wake: chars %d segments %d (views skipped %u, resets %u, game time %.2f s):", nb, nsg, g_wakeSkipped.load(), g_wakeResets.load(), gameTime);
            for (int j = 0; j < nsg && L < 540; ++j) {
                float l = sqrtf((segs[j][4] - segs[j][0]) * (segs[j][4] - segs[j][0]) + (segs[j][5] - segs[j][1]) * (segs[j][5] - segs[j][1]));
                L += snprintf(b2 + L, sizeof(b2) - L, " [len %.0fcm s %.2f->%.2f]", l, segs[j][3], segs[j][7]);
            }
            Log("%s", b2);
        }
    }
    memcpy(v + OFF_View_SlotsA, slots, 7 * 16);
    memcpy(v + OFF_View_SlotsB, slots[7], 16);
    const float magic[4] = { MAGIC_X, MAGIC_Y, MAGIC_Z, MAGIC_W };
    memcpy(v + OFF_View_SlotsB + 16, magic, 16);
    FillAttackSlots(v, cam);
    WriteFoliageView(v);
    g_lastActive.store(nb);
}

static void OnGroupsUB(int kind, const uint8_t* c);
static void OnGroupsCaller(uintptr_t ret, uintptr_t callerRsp);
static constexpr uintptr_t RVA_RenderingGroupsLayoutFwd = 0x9094360 + 0x30, RVA_CullingGroupsLayoutFwd = 0x9094180 + 0x30;

static void* __fastcall HookCreateUB(void* self, void* ret, const void* contents, const void* layout, uint32_t usage, uint32_t validation) {
    uintptr_t lay = reinterpret_cast<uintptr_t>(layout);
    if (contents && lay == g_base + RVA_ViewLayout) {
        __try { FillView((uint8_t*)const_cast<void*>(contents)); g_viewWrites.fetch_add(1); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    } else if (contents && (lay == g_base + RVA_RenderingGroupsLayoutFwd || lay == g_base + RVA_CullingGroupsLayoutFwd)) {
        __try {
            OnGroupsUB(lay == g_base + RVA_RenderingGroupsLayoutFwd ? 0 : 1, (uint8_t*)const_cast<void*>(contents));
            uintptr_t* ra = (uintptr_t*)_AddressOfReturnAddress();
            OnGroupsCaller(*ra, (uintptr_t)(ra + 1));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return g_origCreateUB(self, ret, contents, layout, usage, validation);
}

static bool InstallHook() {
    uintptr_t rhi, vtbl;
    if (!Rd(g_base + RVA_GDynamicRHI, rhi) || !rhi || !Rd(rhi, vtbl) || !vtbl) return false;
    uintptr_t* slot = reinterpret_cast<uintptr_t*>(vtbl) + VT_CreateUniformBuffer;
    uintptr_t orig; if (!Rd((uintptr_t)slot, orig) || !orig) return false;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    g_origCreateUB = reinterpret_cast<CreateUB_t>(orig);
    *slot = reinterpret_cast<uintptr_t>(&HookCreateUB);
    VirtualProtect(slot, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("hook installed: GDynamicRHI=%p vtable=%p slot[%d] %p -> %p", (void*)rhi, (void*)vtbl, VT_CreateUniformBuffer, (void*)orig, (void*)&HookCreateUB);
    return true;
}

// ---------------------------------------------------------------- FName decoding + material-group diagnostics
// FNamePool (UE 4.26): Blocks[] of entries; entry = uint16 header (bIsWide:1, probe:5, Len:10) + chars, stride 2.
static uintptr_t g_nameBlocks = 0;   // address of Blocks[0]

static bool FindNamePool() {
    // FNamePool::Blocks at a fixed RVA for this build; entry = [u32][u16 header: len = h>>6, wide = h&1][chars], stride 4
    uintptr_t b0; if (!Rd(g_base + 0x90D3C10, b0) || !b0) { Log("name pool not ready"); return false; }
    g_nameBlocks = g_base + 0x90D3C10;
    return true;
}

static bool NameStr(uint32_t idx, char* out, size_t n) {
    out[0] = 0;
    if (!g_nameBlocks) return false;
    uintptr_t block; if (!Rd(g_nameBlocks + 8 * (idx >> 16), block) || !block) return false;
    uintptr_t e = block + 4 * (idx & 0xFFFF) + 4;
    uint16_t h; if (!Rd(e, h)) return false;
    size_t len = h >> 6; bool wide = h & 1;
    if (len == 0 || len >= n) return false;
    if (wide) { for (size_t i = 0; i < len; ++i) { uint16_t c; if (!Rd(e + 2 + 2 * i, c)) return false; out[i] = c < 128 ? (char)c : '?'; } }
    else if (!RdBytes(e + 2, out, len)) return false;
    out[len] = 0;
    return true;
}

static bool ObjName(uintptr_t obj, char* out, size_t n) {
    uint32_t idx; int32_t num;
    if (!Rd(obj + 0x18, idx) || !Rd(obj + 0x1C, num) || !NameStr(idx, out, n)) return false;
    if (num > 0) { size_t l = strlen(out); snprintf(out + l, n - l, "_%d", num - 1); }
    return true;
}

static void ObjPath(uintptr_t obj, char* out, size_t n) {
    char parts[8][128]; int np = 0; uintptr_t o = obj;
    while (o && np < 8) { if (!ObjName(o, parts[np], 128)) break; ++np; if (!Rd(o + 0x20, o)) break; }
    out[0] = 0;
    for (int i = np - 1; i >= 0; --i) { size_t l = strlen(out); snprintf(out + l, n - l, i == np - 1 ? "%s" : (i == 0 ? ".%s" : "/%s"), parts[i]); }
}

static void ClassName(uintptr_t obj, char* out, size_t n) {
    uintptr_t cls = 0; out[0] = 0;
    if (Rd(obj + OFF_UObject_Class, cls) && cls) ObjName(cls, out, n);
}

static DWORD WINAPI NamesDiagThread(LPVOID) {
    // wait until the world is loaded (characters present), then a little longer for streaming
    for (int i = 0; i < 600 && g_cache[g_cacheFront.load()].n == 0; ++i) Sleep(500);
    Sleep(10000);
    FindNamePool();
    return 0;
}

#include "foliage.inc"
#include "rustle.inc"
#include "interact.inc"
static void SetCamPos(const float* cam) { AcquireSRWLockExclusive(&g_camLock); memcpy(g_camPos, cam, 12); ReleaseSRWLockExclusive(&g_camLock); }

static DWORD WINAPI MainThread(LPVOID) {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uint8_t sig[sizeof(SIG_BYTES)];
    if (!RdBytes(g_base + RVA_SigSite, sig, sizeof(sig)) || memcmp(sig, SIG_BYTES, sizeof(sig)) != 0) {
        Log("game build mismatch at RVA %#llx - Final Foliage Reenabled disabled (game update?)", (unsigned long long)RVA_SigSite);
        return 0;
    }
    Log("Final Foliage Reenabled v1.0 loaded, base=%p", (void*)g_base);
    while (!InstallHook()) Sleep(200);
    // log the class chain once so the SuperStruct offset can be verified
    for (int tries = 0; tries < 600; ++tries) {
        uintptr_t player = 0, chr = 0;
        Rd(g_base + RVA_PlayerCharClass, player); Rd(g_base + RVA_ACharacterClass, chr);
        if (player && chr) {
            uintptr_t c = player; int depth = -1;
            for (int i = 0; i < 64 && c; ++i) { if (c == chr) { depth = i; break; } if (!Rd(c + OFF_UStruct_Super, c)) break; }
            Log("class check: AEndPlayerCharacter=%p ACharacter=%p, ACharacter is ancestor at depth %d", (void*)player, (void*)chr, depth);
            break;
        }
        Sleep(500);
    }
    bool modeChosen = false;
    HANDLE diag = CreateThread(nullptr, 0, GustThread, nullptr, 0, nullptr);
    HANDLE nd = CreateThread(nullptr, 0, NamesDiagThread, nullptr, 0, nullptr);
    if (nd) CloseHandle(nd);
    { HANDLE sp = CreateThread(nullptr, 0, InteractThread, nullptr, 0, nullptr); if (sp) CloseHandle(sp); }
    if (diag) CloseHandle(diag);
    for (unsigned n = 0;; ++n) {
        ScanObjects(g_debugLog && n % 40 == 0);
        ScanWeaponsAndDamageSources(g_debugLog && n % 40 == 0);
        if (n % 2 == 0) { AcquireSRWLockExclusive(&g_rdsLock); __try { RefreshFoliage(g_debugLog && n % 40 == 0); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("foliage refresh fault"); } ReleaseSRWLockExclusive(&g_rdsLock); }
        if (!modeChosen && g_cache[g_cacheFront.load()].n > 0) { ChoosePositionMode(); modeChosen = true; }
        if (g_debugLog && n % 40 == 0) Log("view buffers filled: %u, characters in slots last frame: %u", g_viewWrites.load(), g_lastActive.load());
        Sleep(250);
    }
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = mod;
        DisableThreadLibraryCalls(mod);
        InitializeCriticalSection(&g_logLock);
        wchar_t path[MAX_PATH]; GetModuleFileNameW(mod, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\'); if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"FinalFoliage.log");
        g_log = _wfopen(path, L"w");
        HANDLE h = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
