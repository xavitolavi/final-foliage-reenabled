// ---------------------------------------------------------------- per-material foliage classes (milestone 4)
// Each MassiveEnvironment render data (a "block") lists one coverage (alpha-mask) texture per material. Materials are
// classified by that texture's name using FinalFoliage.ini (flowers / grass patches / leafy patches / bushes / ...).
// Blocks near the camera get a slot in a 96-dword table (4 bits per material) that FillView copies into View regs
// 221..244, with per-class gains in View regs 103..106. The groups uniform buffers (RenderDataPositions[i].w, unused
// padding) receive slot+1 for each block, matched by bounds centre. Patched mesh shaders read slot + material id
// (meshlet word.y >> 20) and scale the bend by the class gain; class 0 (rigid) never bends.
// Only unused float4x4 View matrices are safe (FogContext arrays at 221+ are indexed dynamically -> broke sun light).
static constexpr int VIEW_REG_GAINS = 123, VIEW_REG_TABLE_A = 103, VIEW_REG_TABLE_B = 146, TABLE_A_DWORDS = 48, TABLE_DWORDS = 80, MAX_CLASSES = 16;
static constexpr int MAX_RD = 2048, MAX_MATS = 128, MAX_SNAP = 256;
static constexpr float SLOT_RANGE = 9000.f, SLOT_RANGE_OUT = 10500.f;   // cm from camera to block bounds
static constexpr ULONGLONG SLOT_QUARANTINE_MS = 3000;

// ---- configuration (FinalFoliage.ini, reloaded when it changes)
struct ClassCfg {
    char name[48]; float gain; bool part; char match[1024];
    // touch effects
    int sound; float soundVol, radius; char vfx[200]; float vfxScale, vfxHeight, vfxChance;
    bool hasColor; float colMin[4], colMax[4];      // optional leaf colour range for the effect
    float leafSize, leafRate, leafLife, leafSpread; // optional effect user parameters (negative = untouched)
};
// defaults for the touch effects of the stock classes (used when the ini has no such keys)
static const struct { int sound; float vol, radius; const char* vfx; float scale, height, chance; } CLASS_FX_DEFAULT[6] = {
    { 0, 0.f, 0.f, "", 1.f, 0.f, 0.f },
    { 1, 0.385f, 35.f, "/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Grass_01", 0.9f, 15.f, 0.7f },     // flowers
    { 2, 0.08f, 90.f, "/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Grass_01", 0.4f, 10.f, 0.6f }, // grass patches
    { 2, 0.35f, 70.f, "/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Grass_01", 0.48f, 10.f, 0.6f },      // leafy patches
    { 3, 1.15f, 120.f, "/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Grass_01", 1.15f, 40.f, 0.8f },      // bushes
    { 1, 0.5f, 60.f, "/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Grass_01", 0.4f, 10.f, 0.5f }, // other
};
static int g_fxSounds = 1, g_fxVfx = 1;
static char g_dryMatch[256] = "Dry,Dead,Dusty,Buffel";
static char g_dryVfx[200] = "/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Sand_01"; static float g_fxVolume = 0.7f, g_fxMaxVfxPerSec = 6.f, g_fxRange = 2500.f, g_fxMinSpeed = 40.f;
static ClassCfg g_cls[MAX_CLASSES]; static int g_nCls = 0;          // index = class id (0 = rigid)
static char g_exclude[512] = "";
static float g_recoveryMs = 1000.f, g_charStrength = 0.85f, g_trailStrength = 1.f;
static float g_windStrength = 0.15f, g_windSpeed = 1.f, g_windDirDeg = 45.f, g_windFadeStart = 6500.f, g_windFadeEnd = 8000.f;
static FILETIME g_iniTime = {};
static std::atomic<int> g_cfgGen{0};

static const char* DEFAULT_INI =
"; Final Foliage Reenabled - settings. Save this file while the game runs to apply changes.\n"
"; Every foliage material is classified by the name of its coverage (alpha mask) texture, e.g. T_Flower_01A_A.\n"
"; The first class whose 'match' list contains part of that name wins (case-insensitive). Names that contain an\n"
"; 'exclude' entry, opaque materials (rocks, trunks) and unmatched names stay rigid.\n"
"; gain scales how far that class bends (1.0 = full, 0 = off). Save the file while the game runs to apply it.\n"
"; mode=lean tilts each plant as a whole around its centre (good for single flowers and bushes); mode=shear leans the\n"
"; plant the same way but bends it from the ground by height, so wide patches don't rock like a plate.\n"
"; recovery_ms: how long plants take to spring back after a character passes (0 = instantly).\n"
"; character_strength: how hard characters push plants when they walk into them (0..1).\n"
"; trail_strength: how bent plants stay right behind a character while they recover (0..1).\n"
"; debug_log=1 writes periodic statistics to FinalFoliage.log (for troubleshooting).\n"
"\n"
"[global]\n"
"exclude=Wall,Pot,Vase,Deco_,Decal,Bouquet,Wreath\n"
"recovery_ms=1000\n"
"character_strength=0.85\n"
"trail_strength=1.0\n"
"debug_log=0\n"
"\n"
"; Touch effects: a rustle sound and a small particle burst when a character walks into a plant.\n"
"; sounds / vfx: 1 = on, 0 = off. volume 0..1. range_m: no effects further than this from the camera.\n"
"; max_vfx_per_second limits particle bursts when running through dense patches. min_speed: cm/s a character must move.\n"
"; Per class: sound = small | medium | large | none, sound_volume 0..1, touch_radius_cm = how close counts as a touch,\n"
"; vfx = game effect path (or none), vfx_scale, vfx_height_cm = spawn height above the plant base, vfx_chance 0..1.\n"
"[effects]\n"
"sounds=1\n"
"vfx=1\n"
"volume=0.7\n"
"range_m=25\n"
"max_vfx_per_second=6\n"
"min_speed=40\n"
"; Dry grass and dead plants (texture name contains a dry_match word) use dry_vfx instead of their class's vfx.\n"
"dry_match=Dry,Dead,Dusty,Buffel\n"
"dry_vfx=/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Sand_01\n"
"\n"
"; Gentle procedural wind for plants that have no wind animation of their own (scaled by each class's gain).\n"
"; strength 0..1 (0 = off), speed = how fast it gusts, direction in degrees, faded out between fade_start_m and fade_end_m.\n"
"[wind]\n"
"strength=0.15\n"
"speed=1.0\n"
"direction=45\n"
"fade_start_m=65\n"
"fade_end_m=80\n"
"\n"
"[class1]\n"
"name=flowers\n"
"gain=0.93\n"
"mode=lean\n"
"match=T_Flower_,Dandelion,ButterCup,Viola,WildFlower,Harujion,Allamanda,Hydrangea,Gentiana,BerryFlower,Jasmine,Lysimachia,Calystegia,Akatsumekusa\n"
"sound=small\n"
"sound_volume=0.385\n"
"touch_radius_cm=35\n"
"vfx=/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Grass_01\n"
"vfx_scale=0.9\n"
"vfx_height_cm=15\n"
"vfx_chance=0.7\n"
"\n"
"[class2]\n"
"name=grass patches\n"
"gain=0.7\n"
"mode=shear\n"
"match=GroundCover_Grass,T_Grass_,Reed,Wheat,Susuki,DryGrass,BuffelGrass,DryPlant,DryDead,DustyDead\n"
"sound=medium\n"
"sound_volume=0.08\n"
"touch_radius_cm=90\n"
"vfx=/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Grass_01\n"
"vfx_scale=0.4\n"
"vfx_height_cm=10\n"
"vfx_chance=0.6\n"
"\n"
"[class3]\n"
"name=leafy ground-cover patches\n"
"gain=2\n"
"mode=shear\n"
"match=BlackBerry,VariousGreenLeaf,TropicalWideLeaf,Fern,BladeLeaf,Dichondra,Clover,Hosta,Ginger,MugWort,Heliotropium,AcaciaYoung\n"
"sound=medium\n"
"sound_volume=0.35\n"
"touch_radius_cm=70\n"
"vfx=/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Grass_01\n"
"vfx_scale=0.48\n"
"vfx_height_cm=10\n"
"vfx_chance=0.6\n"
"\n"
"[class4]\n"
"name=bushes\n"
"gain=0.75\n"
"mode=lean\n"
"match=T_Shrub_,T_Bush_\n"
"sound=large\n"
"sound_volume=1.15\n"
"touch_radius_cm=120\n"
"vfx=/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Grass_01\n"
"vfx_scale=1.15\n"
"vfx_height_cm=40\n"
"vfx_chance=0.8\n"
"\n"
"[class5]\n"
"name=other ground cover\n"
"gain=0.8\n"
"mode=shear\n"
"match=T_GroundCover_\n"
"sound=small\n"
"sound_volume=0.5\n"
"touch_radius_cm=60\n"
"vfx=/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Grass_01\n"
"vfx_scale=0.4\n"
"vfx_height_cm=10\n"
"vfx_chance=0.5\n";

static void IniPath(wchar_t* out, const wchar_t* file) {
    GetModuleFileNameW(g_module, out, MAX_PATH);
    wchar_t* slash = wcsrchr(out, L'\\'); if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - out), file);
}

static void Trim(char* s) {
    char* b = s; while (*b == ' ' || *b == '\t') ++b;
    memmove(s, b, strlen(b) + 1);
    size_t n = strlen(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

// returns true when the configuration was (re)loaded
static bool LoadConfig() {
    wchar_t path[MAX_PATH]; IniPath(path, L"FinalFoliage.ini");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        FILE* f = _wfopen(path, L"w"); if (f) { fputs(DEFAULT_INI, f); fclose(f); }
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return false;
    }
    if (CompareFileTime(&fa.ftLastWriteTime, &g_iniTime) == 0 && g_nCls) return false;
    FILE* f = _wfopen(path, L"r"); if (!f) return false;
    g_iniTime = fa.ftLastWriteTime;
    ClassCfg cls[MAX_CLASSES] = {}; int ncls = 1; char excl[512] = ""; float rec = 1000.f, chs = 0.85f, trs = 1.f;
    strcpy(cls[0].name, "rigid");
    float wst = 0.15f, wsp = 1.f, wdir = 45.f, wf0 = 65.f, wf1 = 80.f;
    for (int k = 0; k < MAX_CLASSES; ++k) {   // touch-effect defaults per stock class
        cls[k].hasColor = false; for (int j = 0; j < 4; ++j) cls[k].colMin[j] = cls[k].colMax[j] = 1.f;
        cls[k].leafSize = cls[k].leafRate = cls[k].leafLife = cls[k].leafSpread = -1.f;
        const auto& d = CLASS_FX_DEFAULT[k < 6 ? k : 0];
        cls[k].sound = d.sound; cls[k].soundVol = d.vol; cls[k].radius = d.radius > 0.f ? d.radius : 60.f;
        strncpy(cls[k].vfx, d.vfx, sizeof(cls[k].vfx) - 1); cls[k].vfxScale = d.scale; cls[k].vfxHeight = d.height; cls[k].vfxChance = d.chance;
    }
    int esnd = 1, evfx = 1; char dryM[256] = "Dry,Dead,Dusty,Buffel", dryV[200] = "/Game/Effect/Character/Property/VE0009_Chocobo/Particle/FX_VE0009_FootStep_Sand_01"; float evol = 0.7f, erange = 25.f, emax = 6.f, emin = 40.f;
    char line[1200]; int sec = -1;   // -1 none, 0 global, n class n, 100 wind, 101 effects
    while (fgets(line, sizeof(line), f)) {
        Trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            if (_strnicmp(line, "[global]", 8) == 0) sec = 0;
            else if (_strnicmp(line, "[wind]", 6) == 0) sec = 100;
            else if (_strnicmp(line, "[effects]", 9) == 0) sec = 101;
            else if (_strnicmp(line, "[class", 6) == 0) { int k = atoi(line + 6); sec = (k >= 1 && k < MAX_CLASSES) ? k : -1; if (sec > 0) { cls[sec].gain = 1.f; if (sec >= ncls) ncls = sec + 1; } }
            else sec = -1;
            continue;
        }
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char* key = line; char* val = eq + 1; Trim(key); Trim(val);
        if (sec == 0) {
            if (!_stricmp(key, "exclude")) strncpy(excl, val, sizeof(excl) - 1);
            else if (!_stricmp(key, "recovery_ms")) rec = (float)atof(val);
            else if (!_stricmp(key, "character_strength")) chs = (float)atof(val);
            else if (!_stricmp(key, "trail_strength")) trs = (float)atof(val);
            else if (!_stricmp(key, "debug_log")) g_debugLog = atoi(val);
        } else if (sec == 100) {
            if (!_stricmp(key, "strength")) wst = (float)atof(val);
            else if (!_stricmp(key, "speed")) wsp = (float)atof(val);
            else if (!_stricmp(key, "direction")) wdir = (float)atof(val);
            else if (!_stricmp(key, "fade_start_m")) wf0 = (float)atof(val);
            else if (!_stricmp(key, "fade_end_m")) wf1 = (float)atof(val);
        } else if (sec == 101) {
            if (!_stricmp(key, "sounds")) esnd = atoi(val);
            else if (!_stricmp(key, "vfx")) evfx = atoi(val);
            else if (!_stricmp(key, "volume")) evol = (float)atof(val);
            else if (!_stricmp(key, "range_m")) erange = (float)atof(val);
            else if (!_stricmp(key, "max_vfx_per_second")) emax = (float)atof(val);
            else if (!_stricmp(key, "min_speed")) emin = (float)atof(val);
            else if (!_stricmp(key, "dry_match")) { strncpy(dryM, val, sizeof(dryM) - 1); dryM[sizeof(dryM) - 1] = 0; }
            else if (!_stricmp(key, "dry_vfx")) { if (!_stricmp(val, "none")) dryV[0] = 0; else { strncpy(dryV, val, sizeof(dryV) - 1); dryV[sizeof(dryV) - 1] = 0; } }
        } else if (sec > 0 && sec < MAX_CLASSES) {
            if (!_stricmp(key, "name")) strncpy(cls[sec].name, val, sizeof(cls[sec].name) - 1);
            else if (!_stricmp(key, "gain")) cls[sec].gain = (float)atof(val);
            else if (!_stricmp(key, "mode")) cls[sec].part = !_stricmp(val, "shear") || !_stricmp(val, "part");
            else if (!_stricmp(key, "match")) strncpy(cls[sec].match, val, sizeof(cls[sec].match) - 1);
            else if (!_stricmp(key, "sound")) cls[sec].sound = !_stricmp(val, "small") ? 1 : !_stricmp(val, "medium") ? 2 : !_stricmp(val, "large") ? 3 : 0;
            else if (!_stricmp(key, "sound_volume")) cls[sec].soundVol = (float)atof(val);
            else if (!_stricmp(key, "touch_radius_cm")) cls[sec].radius = (float)atof(val);
            else if (!_stricmp(key, "vfx")) { if (!_stricmp(val, "none")) cls[sec].vfx[0] = 0; else strncpy(cls[sec].vfx, val, sizeof(cls[sec].vfx) - 1); }
            else if (!_stricmp(key, "vfx_scale")) cls[sec].vfxScale = (float)atof(val);
            else if (!_stricmp(key, "vfx_height_cm")) cls[sec].vfxHeight = (float)atof(val);
            else if (!_stricmp(key, "vfx_chance")) cls[sec].vfxChance = (float)atof(val);
            else if (!_stricmp(key, "vfx_leaf_size")) cls[sec].leafSize = (float)atof(val);
            else if (!_stricmp(key, "vfx_leaf_count")) cls[sec].leafRate = (float)atof(val);
            else if (!_stricmp(key, "vfx_leaf_life")) cls[sec].leafLife = (float)atof(val);
            else if (!_stricmp(key, "vfx_spread")) cls[sec].leafSpread = (float)atof(val);
            else if (!_stricmp(key, "vfx_color_min") || !_stricmp(key, "vfx_color_max")) {
                float c[3] = { 1, 1, 1 }; int got = sscanf(val, "%f , %f , %f", &c[0], &c[1], &c[2]);
                if (got == 3) { float* d = !_stricmp(key, "vfx_color_min") ? cls[sec].colMin : cls[sec].colMax; d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = 1.f; cls[sec].hasColor = true; }
            }
        }
    }
    fclose(f);
    memcpy(g_cls, cls, sizeof(cls)); g_nCls = ncls; strcpy(g_exclude, excl);
    g_recoveryMs = rec < 0.f ? 0.f : (rec > 5000.f ? 5000.f : rec);
    g_charStrength = chs < 0.f ? 0.f : (chs > 1.f ? 1.f : chs);
    g_windStrength = wst < 0.f ? 0.f : (wst > 1.f ? 1.f : wst);
    g_windSpeed = wsp < 0.f ? 0.f : (wsp > 10.f ? 10.f : wsp);
    g_windDirDeg = wdir;
    g_windFadeStart = (wf0 < 0.f ? 0.f : wf0) * 100.f;
    g_windFadeEnd = (wf1 * 100.f > g_windFadeStart + 100.f) ? wf1 * 100.f : g_windFadeStart + 100.f;
    g_trailStrength = trs < 0.f ? 0.f : (trs > 1.f ? 1.f : trs);
    strcpy(g_dryMatch, dryM); strcpy(g_dryVfx, dryV);
    g_fxSounds = esnd; g_fxVfx = evfx; g_fxVolume = evol < 0.f ? 0.f : (evol > 1.f ? 1.f : evol);
    g_fxRange = (erange < 1.f ? 1.f : erange) * 100.f; g_fxMaxVfxPerSec = emax < 0.f ? 0.f : emax; g_fxMinSpeed = emin < 0.f ? 0.f : emin;
    g_cfgGen.fetch_add(1);
    Log("config loaded: %d classes, recovery %.0f ms, character strength %.2f, trail strength %.2f; wind %.2f speed %.2f dir %.0f fade %.0f-%.0f m",
        ncls - 1, g_recoveryMs, g_charStrength, g_trailStrength, g_windStrength, g_windSpeed, g_windDirDeg, g_windFadeStart / 100.f, g_windFadeEnd / 100.f);
    Log("touch effects: sounds %d vfx %d volume %.2f range %.0f m, max %.0f bursts/s, min speed %.0f cm/s", g_fxSounds, g_fxVfx, g_fxVolume, g_fxRange / 100.f, g_fxMaxVfxPerSec, g_fxMinSpeed);
    static const char* SN[4] = { "none", "small", "medium", "large" };
    for (int k = 1; k < ncls; ++k) if (cls[k].match[0]) {
        const char* vn = cls[k].vfx[0] ? strrchr(cls[k].vfx, '/') : nullptr;
        Log("  class %d '%s' gain %.2f mode %s | sound %s x%.2f radius %.0f cm | vfx %s x%.2f at %.0f cm, chance %.2f", k, cls[k].name, cls[k].gain,
            cls[k].part ? "shear" : "lean", SN[cls[k].sound & 3], cls[k].soundVol, cls[k].radius, vn ? vn + 1 : "none", cls[k].vfxScale, cls[k].vfxHeight, cls[k].vfxChance);
    }
    return true;
}

static bool ListHas(const char* list, const char* name) {   // comma-separated substrings, case-insensitive
    char lname[256]; size_t n = strlen(name); if (n >= sizeof(lname)) n = sizeof(lname) - 1;
    for (size_t i = 0; i < n; ++i) lname[i] = (char)tolower((unsigned char)name[i]); lname[n] = 0;
    const char* p = list;
    while (*p) {
        const char* e = strchr(p, ','); size_t len = e ? (size_t)(e - p) : strlen(p);
        char tok[128]; size_t k = 0;
        for (size_t i = 0; i < len && k < sizeof(tok) - 1; ++i) if (p[i] != ' ') tok[k++] = (char)tolower((unsigned char)p[i]);
        tok[k] = 0;
        if (k && strstr(lname, tok)) return true;
        if (!e) break; p = e + 1;
    }
    return false;
}

static int ClassifyName(const char* name) {
    if (!name[0] || strstr(name, "FFFFFFFF") || strstr(name, "000000FF")) return 0;
    if (g_exclude[0] && ListHas(g_exclude, name)) return 0;
    for (int k = 1; k < g_nCls; ++k) if (g_cls[k].match[0] && ListHas(g_cls[k].match, name)) return k;
    return 0;
}

// ---- render-data cache and slot allocation (scanner thread)
struct RdInfo {
    uintptr_t rd; float c[3]; int nMat; uint8_t cls[MAX_MATS]; uint8_t dry[MAX_MATS]; bool plant;
    int slot, len;            // table dword range, slot = -1 when none
    ULONGLONG seen; int gen;
};
static RdInfo g_rds[MAX_RD]; static int g_nRds = 0;
static ULONGLONG g_dwFreedAt[TABLE_DWORDS];      // 0 = never used; quarantine after free
static bool g_dwUsed[TABLE_DWORDS];
static uint32_t g_table[TABLE_DWORDS];

struct FoliageSnap { uint32_t table[TABLE_DWORDS]; float gains[MAX_CLASSES]; int n; float c[MAX_SNAP][3]; uint32_t slot1[MAX_SNAP]; };
static FoliageSnap g_fsnap[2]; static std::atomic<int> g_fsnapFront{0};
static std::atomic<int> g_fsnapReady{0};
static float g_camPos[3]; static SRWLOCK g_camLock = SRWLOCK_INIT;
static uintptr_t g_meCompClass = 0;
static std::atomic<uint32_t> g_groupsTagged{0}, g_groupsEntries{0};

static void ClassifyRd(RdInfo& r) {
    uintptr_t covArr = 0; int32_t nCov = 0;
    Rd(r.rd + 0x38, covArr); Rd(r.rd + 0x40, nCov);
    if (nCov < 0 || nCov > MAX_MATS || !covArr) nCov = 0;
    r.nMat = nCov; r.plant = false; memset(r.cls, 0, sizeof(r.cls));
    char nm[160];
    for (int m = 0; m < nCov; ++m) {
        uintptr_t tex = 0; nm[0] = 0;
        if (Rd(covArr + 8 * m, tex) && tex) ObjName(tex, nm, sizeof(nm));
        r.cls[m] = (uint8_t)ClassifyName(nm);
        r.dry[m] = (nm[0] && g_dryMatch[0] && ListHas(g_dryMatch, nm)) ? 1 : 0;
        if (r.cls[m]) r.plant = true;
    }
    r.gen = g_cfgGen.load();
}

static bool RdBounds(uintptr_t rd, float c[3], float mn[3], float mx[3]) {
    float b[6]; if (!RdBytes(rd + 0x60, b, sizeof(b))) return false;
    for (int i = 0; i < 6; ++i) if (!std::isfinite(b[i]) || fabsf(b[i]) > 1e8f) return false;
    for (int i = 0; i < 3; ++i) { mn[i] = b[i]; mx[i] = b[i + 3]; c[i] = (b[i] + b[i + 3]) * 0.5f; }
    return true;
}

static void FreeSlot(RdInfo& r, ULONGLONG now) {
    if (r.slot < 0) return;
    for (int d = r.slot; d < r.slot + r.len; ++d) { g_dwUsed[d] = false; g_dwFreedAt[d] = now; }
    r.slot = -1; r.len = 0;
}

static bool AllocSlot(RdInfo& r, ULONGLONG now) {
    int len = (r.nMat + 7) / 8; if (len <= 0 || len > TABLE_DWORDS) return false;
    for (int s = 0; s + len <= TABLE_DWORDS; ++s) {
        bool ok = true;
        for (int d = s; d < s + len; ++d) if (g_dwUsed[d] || (g_dwFreedAt[d] && now - g_dwFreedAt[d] < SLOT_QUARANTINE_MS)) { ok = false; s = d; break; }
        if (!ok) continue;
        for (int d = s; d < s + len; ++d) {
            uint32_t w = 0;
            for (int k = 0; k < 8; ++k) { int m = (d - s) * 8 + k; if (m < r.nMat) w |= (uint32_t)(r.cls[m] & 15) << (4 * k); }
            g_table[d] = w; g_dwUsed[d] = true;
        }
        r.slot = s; r.len = len;
        return true;
    }
    return false;
}

static void WriteClassDump() {
    wchar_t path[MAX_PATH]; IniPath(path, L"FinalFoliage_classes.txt");
    FILE* f = _wfopen(path, L"w"); if (!f) return;
    fprintf(f, "Coverage textures of loaded foliage and the class each one got (see FinalFoliage.ini)\n\n");
    // distinct names, grouped by class
    static char names[4096][96]; static uint8_t ncls[4096]; int nn = 0;
    char nm[160];
    for (int i = 0; i < g_nRds; ++i) {
        uintptr_t covArr = 0; int32_t nCov = 0;
        Rd(g_rds[i].rd + 0x38, covArr); Rd(g_rds[i].rd + 0x40, nCov);
        for (int m = 0; m < nCov && m < MAX_MATS; ++m) {
            uintptr_t tex = 0; nm[0] = 0;
            if (!Rd(covArr + 8 * m, tex) || !tex || !ObjName(tex, nm, sizeof(nm))) continue;
            bool dup = false; for (int k = 0; k < nn; ++k) if (!strcmp(names[k], nm)) { dup = true; break; }
            if (dup || nn >= 4096) continue;
            strncpy(names[nn], nm, 95); names[nn][95] = 0; ncls[nn] = (uint8_t)ClassifyName(nm); ++nn;
        }
    }
    for (int k = 0; k < g_nCls; ++k) {
        int cnt = 0; for (int j = 0; j < nn; ++j) if (ncls[j] == k) ++cnt;
        fprintf(f, "[%d] %s (gain %.2f): %d textures\n", k, g_cls[k].name, k ? g_cls[k].gain : 0.f, cnt);
        for (int j = 0; j < nn; ++j) if (ncls[j] == k) fprintf(f, "    %s\n", names[j]);
        fputc('\n', f);
    }
    fclose(f);
}

static void FindMeComponentClass() {
    if (g_meCompClass || !g_nameBlocks) return;
    uintptr_t arr = g_base + RVA_GUObjectArray + 0x10, chunks; int32_t num;
    if (!Rd(arr, chunks) || !Rd(arr + 0x14, num) || !chunks) return;
    char cn[128];
    for (int32_t i = 0; i < num; ++i) {
        uintptr_t chunk, obj, cls;
        if (!Rd(chunks + 8 * (i / 65536), chunk) || !chunk) continue;
        if (!Rd(chunk + 24 * (i % 65536), obj) || !obj || !Rd(obj + OFF_UObject_Class, cls) || !cls) continue;
        if (ObjName(cls, cn, sizeof(cn)) && !strcmp(cn, "MassiveEnvironmentComponent")) { g_meCompClass = cls; Log("MassiveEnvironmentComponent class %p", (void*)cls); return; }
    }
}

static ULONGLONG g_lastClassDump = 0;
static void RefreshFoliage(bool verbose) {
    if (!g_nameBlocks && !FindNamePool()) return;
    bool reloaded = LoadConfig();
    FindMeComponentClass();
    if (!g_meCompClass) return;
    ULONGLONG now = GetTickCount64();
    if (reloaded) {      // reclassify everything, drop all slots
        for (int i = 0; i < g_nRds; ++i) FreeSlot(g_rds[i], now);
        g_nRds = 0;
    }
    float cam[3]; AcquireSRWLockShared(&g_camLock); memcpy(cam, g_camPos, 12); ReleaseSRWLockShared(&g_camLock);
    // walk components -> render datas
    uintptr_t arr = g_base + RVA_GUObjectArray + 0x10, chunks; int32_t num;
    if (!Rd(arr, chunks) || !Rd(arr + 0x14, num) || !chunks) return;
    int comps = 0, newRds = 0;
    for (int32_t i = 0; i < num; ++i) {
        uintptr_t chunk, obj, cls; int32_t flags;
        if (!Rd(chunks + 8 * (i / 65536), chunk) || !chunk) continue;
        if (!Rd(chunk + 24 * (i % 65536), obj) || !obj || !Rd(obj + OFF_UObject_Class, cls) || cls != g_meCompClass) continue;
        if (!Rd(obj + OFF_UObject_Flags, flags) || (flags & (RF_ClassDefaultObject | RF_ArchetypeObject))) continue;
        ++comps;
        uintptr_t rdArr; int32_t nrd;
        if (!Rd(obj + 0x288, rdArr) || !Rd(obj + 0x290, nrd) || nrd <= 0 || nrd > 4096) continue;
        for (int k = 0; k < nrd; ++k) {
            uintptr_t rd; if (!Rd(rdArr + 8 * k, rd) || !rd) continue;
            float c[3], mn[3], mx[3]; if (!RdBounds(rd, c, mn, mx)) continue;
            int j = 0; while (j < g_nRds && g_rds[j].rd != rd) ++j;
            if (j < g_nRds && memcmp(g_rds[j].c, c, 12) != 0) { FreeSlot(g_rds[j], now); g_rds[j] = g_rds[--g_nRds]; j = g_nRds; }   // memory reused
            if (j == g_nRds) {
                if (g_nRds >= MAX_RD) continue;
                RdInfo& r = g_rds[g_nRds++]; memset(&r, 0, sizeof(r));
                r.rd = rd; memcpy(r.c, c, 12); r.slot = -1; ClassifyRd(r); ++newRds;
            }
            RdInfo& r = g_rds[j];
            r.seen = now;
            // distance from the camera to the block's bounds
            float d2 = 0.f; for (int a = 0; a < 3; ++a) { float e = cam[a] < mn[a] ? mn[a] - cam[a] : (cam[a] > mx[a] ? cam[a] - mx[a] : 0.f); d2 += e * e; }
            float d = sqrtf(d2);
            if (!r.plant) continue;
            if (r.slot < 0 && d < SLOT_RANGE) AllocSlot(r, now);
            else if (r.slot >= 0 && d > SLOT_RANGE_OUT) FreeSlot(r, now);
        }
    }
    // forget render datas that disappeared
    for (int i = 0; i < g_nRds; ) {
        if (g_rds[i].seen != now) { FreeSlot(g_rds[i], now); g_rds[i] = g_rds[--g_nRds]; } else ++i;
    }
    // publish
    int back = 1 - g_fsnapFront.load();
    FoliageSnap& s = g_fsnap[back];
    memcpy(s.table, g_table, sizeof(s.table));
    for (int k = 0; k < MAX_CLASSES; ++k) {     // the shader reads |gain|; a negative value selects part mode
        float g = (k > 0 && k < g_nCls) ? fabsf(g_cls[k].gain) : 0.f;
        s.gains[k] = (k > 0 && k < g_nCls && g_cls[k].part && g > 0.f) ? -g : g;
    }
    s.n = 0; int slots = 0, used = 0;
    for (int i = 0; i < g_nRds && s.n < MAX_SNAP; ++i) {
        if (g_rds[i].slot < 0) continue;
        memcpy(s.c[s.n], g_rds[i].c, 12); s.slot1[s.n] = (uint32_t)g_rds[i].slot + 1; ++s.n; ++slots; used += g_rds[i].len;
    }
    g_fsnapFront.store(back); g_fsnapReady.store(1);
    if (verbose || reloaded || (newRds && g_debugLog)) {
        int plants = 0; for (int i = 0; i < g_nRds; ++i) plants += g_rds[i].plant;
        Log("foliage: %d components, %d blocks (%d with plants, %d new), %d in table using %d/%d dwords; groups entries tagged %u/%u",
            comps, g_nRds, plants, newRds, slots, used, TABLE_DWORDS, g_groupsTagged.load(), g_groupsEntries.load());
    }
    if (reloaded || (newRds && now - g_lastClassDump > 5000)) { WriteClassDump(); g_lastClassDump = now; }
}

// ---- render-thread side
static void WriteFoliageView(uint8_t* v) {
    {   // wind for no-sway plants: View regs 8, 9 (unused TranslatedWorldToOrthographicClip)
        float rad = g_windDirDeg * 3.14159265f / 180.f;
        float w[8] = { cosf(rad), sinf(rad), g_windStrength, g_windSpeed, g_windFadeStart, 1.f / (g_windFadeEnd - g_windFadeStart), 0.f, 0.f };
        memcpy(v + 8 * 16, w, sizeof(w));
    }
    if (!g_fsnapReady.load()) { memset(v + VIEW_REG_GAINS * 16, 0, 64); return; }
    const FoliageSnap& s = g_fsnap[g_fsnapFront.load()];
    memcpy(v + VIEW_REG_GAINS * 16, s.gains, sizeof(s.gains));
    memcpy(v + VIEW_REG_TABLE_A * 16, s.table, TABLE_A_DWORDS * 4);
    memcpy(v + VIEW_REG_TABLE_B * 16, s.table + TABLE_A_DWORDS, (TABLE_DWORDS - TABLE_A_DWORDS) * 4);
}

static void OnGroupsUB(int kind, const uint8_t* cc) {
    uint8_t* c = const_cast<uint8_t*>(cc);
    float* pos = (float*)(c + (kind == 0 ? 0 : 4096));       // float3 + padding, 16-byte stride
    const FoliageSnap* s = g_fsnapReady.load() ? &g_fsnap[g_fsnapFront.load()] : nullptr;
    uint32_t tagged = 0;
    for (int i = 0; i < 128; ++i) {
        float* p = pos + 4 * i; uint32_t w = 0;
        if (s && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
            for (int k = 0; k < s->n; ++k) {
                if (fabsf(s->c[k][0] - p[0]) < 2.f && fabsf(s->c[k][1] - p[1]) < 2.f && fabsf(s->c[k][2] - p[2]) < 2.f) { w = s->slot1[k]; ++tagged; break; }
            }
        }
        memcpy(p + 3, &w, 4);
    }
    g_groupsTagged.fetch_add(tagged, std::memory_order_relaxed);
    g_groupsEntries.fetch_add(1, std::memory_order_relaxed);
}
static void OnGroupsCaller(uintptr_t, uintptr_t) {}

// ---- slower recovery: a "wake" behind moving characters
// Each nearby character keeps a short position history. The wake is a polyline through the positions the character
// had at fixed ages (fractions of recovery_ms); the shader bends plants by their distance to it, with strength
// trail_strength * (1 - (age/recovery)^2). An endpoint that coincides with the character's current position is faded
// out, so a character standing still gets no extra bend and the wake shrinks back smoothly after it stops.
static constexpr int HIST = 128;

struct Trail { uintptr_t obj; int n, head; float p[HIST][3]; double t[HIST]; double lastUse; };
static Trail g_trails[24];
static SRWLOCK g_trailLock = SRWLOCK_INIT;

static double NowMs() {
    static LARGE_INTEGER f = {}; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static Trail* TrailFor(uintptr_t obj) {
    Trail* freeT = nullptr; Trail* oldest = &g_trails[0];
    for (auto& t : g_trails) {
        if (t.obj == obj) return &t;
        if (!t.obj && !freeT) freeT = &t;
        if (t.lastUse < oldest->lastUse) oldest = &t;
    }
    Trail* t = freeT ? freeT : oldest;
    memset(t, 0, sizeof(*t)); t->obj = obj;
    return t;
}

// position the character had `age` ms ago (linear between samples; oldest sample if the history is shorter)
static void TrailAt(const Trail* t, double now, double age, float out[3]) {
    double want = now - age;
    for (int k = 0; k < t->n; ++k) {
        int i = (t->head - 1 - k + 2 * HIST) % HIST;
        if (t->t[i] <= want) {
            if (k == 0) { memcpy(out, t->p[i], 12); return; }
            int j = (i + 1) % HIST;                                  // newer neighbour
            double span = t->t[j] - t->t[i]; float f = span > 0 ? (float)((want - t->t[i]) / span) : 0.f;
            for (int a = 0; a < 3; ++a) out[a] = t->p[i][a] + (t->p[j][a] - t->p[i][a]) * f;
            return;
        }
    }
    memcpy(out, t->p[(t->head - t->n + 2 * HIST) % HIST], 12);
}

// segs[k] = { A.xyz, A.strength, B.xyz, B.strength }; returns the number of segments written (max maxSegs)
// now = engine game time of the frame being rendered (ms): the wake ages in step with what is drawn and pauses with the game
static int WakeSegments(const uintptr_t* objs, const float (*pos)[3], int n, float segs[][8], int maxSegs, double now) {
    float rec = g_recoveryMs; if (rec < 50.f || maxSegs <= 0 || g_trailStrength <= 0.f) return 0;
    int ns = 0;
    AcquireSRWLockExclusive(&g_trailLock);
    // Several views are built per frame; some (scene captures etc.) can carry a different game time. Ignore views whose
    // clock is behind the newest one seen; only a clock that stays behind for ~1 s (load / restart) resets the trails.
    static double lastNow = 0; static int behind = 0;
    if (now < lastNow - 50.0) {
        if (++behind < 120) { ReleaseSRWLockExclusive(&g_trailLock); g_wakeSkipped.fetch_add(1, std::memory_order_relaxed); return 0; }
        for (auto& t : g_trails) t.n = 0;
        g_wakeResets.fetch_add(1, std::memory_order_relaxed);
    }
    behind = 0;
    if (now > lastNow) lastNow = now;
    for (int i = 0; i < n; ++i) {                       // record history for every near character
        Trail* t = TrailFor(objs[i]); t->lastUse = now;
        int last = (t->head - 1 + HIST) % HIST;
        if (t->n) {
            float dx = pos[i][0] - t->p[last][0], dy = pos[i][1] - t->p[last][1], dz = pos[i][2] - t->p[last][2];
            if (dx * dx + dy * dy > 1500.f * 1500.f || fabsf(dz) > 500.f) t->n = 0;     // teleport / cut
        }
        if (t->n == 0 || now - t->t[last] >= 8.0) {                // one sample per rendered frame, time only moves forward
            memcpy(t->p[t->head], pos[i], 12); t->t[t->head] = now; t->head = (t->head + 1) % HIST; if (t->n < HIST) ++t->n;
        }
    }
    // every wake holder gets the same 2-segment wake, so a change of order never changes the result
    static const float ages[] = { 0.0f, 0.4f, 1.0f };
    const int nPts = 3;
    for (int i = 0; i < n && ns < maxSegs; ++i) {
        Trail* t = TrailFor(objs[i]);
        float pts[3][4];
        for (int k = 0; k < nPts; ++k) {
            TrailAt(t, now, ages[k] * rec, pts[k]);
            float a = ages[k];
            pts[k][3] = g_charStrength * g_trailStrength * (1.f - a * a);   // starts equal to the character's own bend; shader shows only the excess
        }
        for (int k = 0; k + 1 < nPts && ns < maxSegs; ++k) {
            memcpy(segs[ns], pts[k], 16); memcpy(segs[ns] + 4, pts[k + 1], 16); ++ns;
        }
    }
    ReleaseSRWLockExclusive(&g_trailLock);
    return ns;
}

static float CharStrength() { return g_charStrength; }
