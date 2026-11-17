// proxy_wind.inc - loading: winmm.dll proxy + in-memory wind-distance patch.
//
// The game imports WINMM.dll, so Windows loads this DLL (shipped as winmm.dll next to ff7rebirth_.exe) before the
// game's entry point runs. Every winmm export is a one-instruction stub (winmm_proxy.asm) that jumps through
// g_winmmFn into the real System32\winmm.dll. The game exe on disk is never modified.
#include "winmm_proxy.inc"
#include "windpatch_data.inc"

extern "C" void* g_winmmFn[WINMM_EXPORTS] = {};
static uint32_t WinmmUnresolved() { return 0; }   // for an export this Windows version lacks (never called by the game)

static void InitWinmmProxy() {
    wchar_t path[MAX_PATH]; UINT n = GetSystemDirectoryW(path, MAX_PATH);
    HMODULE real = nullptr;
    if (n && n < MAX_PATH - 16) { wcscat_s(path, L"\\winmm.dll"); real = LoadLibraryW(path); }
    for (int i = 0; i < WINMM_EXPORTS; ++i) {
        FARPROC p = real ? GetProcAddress(real, WINMM_TABLE[i].name ? WINMM_TABLE[i].name : MAKEINTRESOURCEA(WINMM_TABLE[i].ordinal)) : nullptr;
        g_winmmFn[i] = p ? reinterpret_cast<void*>(p) : reinterpret_cast<void*>(&WinmmUnresolved);
    }
}

// ---------------------------------------------------------------- game wind distance
// The cave (built by patch_wind.py, see tools/gen_windpatch.py) runs where plant records are uploaded to the GPU and
// clamps each record's AnimationDecayRatio to 1 / (meters * 100), so the game's own wind animation keeps playing out
// to at least `meters` from the camera. 0 = never fades.
static uint8_t* g_windCave = nullptr;
static float g_windReachApplied = -1.f;

static bool WriteCode(uint8_t* dst, const void* src, size_t n) {
    DWORD old;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

static uint32_t WindLimitBits(float meters) {
    float lim = meters <= 0.f ? 0.f : 1.f / (meters * 100.f);
    uint32_t b; memcpy(&b, &lim, 4); return b;
}

static void LogWindReach(float m) {
    if (m <= 0.f) Log("game wind reach: never fades");
    else Log("game wind reach: %.0f m", m);
}

// Live change from the ini (affects plants as their data is uploaded; an area reload applies it everywhere).
static void SetWindReach(float meters) {
    if (meters < 0.f) meters = 0.f;
    if (!g_windCave || meters == g_windReachApplied) return;
    uint32_t b = WindLimitBits(meters);
    for (uint32_t o : WP_LIMIT_OFF) WriteCode(g_windCave + o, &b, 4);
    g_windReachApplied = meters;
    LogWindReach(meters);
}

// Called from DllMain, before the game's entry point. `base` is the game exe image.
static void ApplyWindPatch(uintptr_t base, float meters) {
    if (meters < 0.f) meters = 0.f;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    uint8_t* hook = reinterpret_cast<uint8_t*>(base + WP_HOOK_RVA);
    uint8_t* cave = reinterpret_cast<uint8_t*>(base + WP_CAVE_RVA);
    uint8_t jmp[sizeof(WP_HOOK_ORIG)];
    jmp[0] = 0xE9;
    int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(cave) - (reinterpret_cast<intptr_t>(hook) + 5));
    memcpy(jmp + 1, &rel, 4); memset(jmp + 5, 0x90, sizeof(jmp) - 5);

    // An earlier test build used an installer that patched the exe on disk: its entry point runs a stub that loads
    // FinalFoliage.dll. Switch that stub off for this session so the mod can't run twice (it becomes a plain jump to
    // the real entry point).
    if (nt->OptionalHeader.AddressOfEntryPoint == WP_V10_STUB_RVA) {
        uint8_t* stub = reinterpret_cast<uint8_t*>(base + WP_V10_STUB_RVA);
        uint8_t j[5] = { 0xE9 };
        int32_t r = static_cast<int32_t>(static_cast<intptr_t>(base + WP_ENTRY_RVA) - (reinterpret_cast<intptr_t>(stub) + 5));
        memcpy(j + 1, &r, 4);
        if (WriteCode(stub, j, sizeof(j)))
            Log("note: ff7rebirth_.exe still has the patch from an earlier Final Foliage installer. It is switched off for this "
                "session; to restore the original exe, use Steam > Properties > Installed Files > Verify integrity.");
    }

    if (memcmp(hook, WP_HOOK_ORIG, sizeof(WP_HOOK_ORIG)) == 0) {          // unmodified exe: the cave must be empty padding
        for (size_t i = 0; i < sizeof(WP_CAVE); ++i)
            if (cave[i]) { Log("wind distance: code cave in use by something else - not changed"); return; }
    } else if (memcmp(hook, jmp, sizeof(jmp)) != 0) {                      // neither original nor the old installer's patch
        Log("wind distance: unexpected code at the hook (another mod?) - not changed");
        return;
    }
    uint8_t buf[sizeof(WP_CAVE)];
    memcpy(buf, WP_CAVE, sizeof(buf));
    uint32_t b = WindLimitBits(meters);
    for (uint32_t o : WP_LIMIT_OFF) memcpy(buf + o, &b, 4);
    if (!WriteCode(cave, buf, sizeof(buf)) || !WriteCode(hook, jmp, sizeof(jmp))) { Log("wind distance: VirtualProtect failed - not changed"); return; }
    g_windCave = cave; g_windReachApplied = meters;
    LogWindReach(meters);
}
