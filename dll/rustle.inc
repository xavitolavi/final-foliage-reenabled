// ---------------------------------------------------------------- procedural foliage rustles played through XAudio2
// A rustle is a cloud of tiny band-passed noise grains (single leaves/blades rubbing) over a soft filtered-noise swish,
// shaped by a swell-and-fade envelope. Size changes length, grain density, grain length and the frequency band.
#include <vector>
#include <xaudio2.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "xaudio2.lib")

static constexpr int RUSTLE_RATE = 48000, RUSTLE_SIZES = 3, RUSTLE_VARIANTS = 16, RUSTLE_VOICES = 12;
struct RustleClip { std::vector<float> pcm; };
static RustleClip g_rustle[RUSTLE_SIZES][RUSTLE_VARIANTS], g_swish[RUSTLE_SIZES][RUSTLE_VARIANTS];
static IXAudio2* g_xa = nullptr; static IXAudio2MasteringVoice* g_xaMaster = nullptr;
static IXAudio2SourceVoice* g_xaVoices[RUSTLE_VOICES] = {};

struct Rng { uint32_t s; float f() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFFFF) / 16777216.f; } float n() { return f() * 2.f - 1.f; } };
struct Biquad {   // RBJ band-pass (constant 0 dB peak gain)
    float b0, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void bp(float f, float q) { float w = 6.2831853f * f / RUSTLE_RATE, al = sinf(w) / (2 * q), a0 = 1 + al; b0 = al / a0; b2 = -al / a0; a1 = -2 * cosf(w) / a0; a2 = (1 - al) / a0; }
    float run(float x) { float y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y; return y; }
};

// Rustle "types": 0 small (flowers), 1 medium (grass / leafy patches), 2 and 3 the two bush rustles (the earlier
// medium and large sounds). Size 2 (large) alternates between types 2 and 3.
struct RustleType { float len, dens, flo, fhi, glen, bed, bedf, peak; };
static const RustleType RTYPES[4] = {
    { 0.22f, 200.f, 3500.f, 9500.f, 0.0020f, 0.16f, 5500.f, 0.30f },
    { 0.32f, 320.f, 2600.f, 8500.f, 0.0028f, 0.24f, 4500.f, 0.40f },
    { 0.52f, 650.f, 1600.f, 6000.f, 0.0045f, 0.45f, 3000.f, 0.55f },
    { 0.78f, 950.f,  650.f, 4200.f, 0.0070f, 0.45f, 1800.f, 0.55f },
};
static void SynthRustle(RustleClip& c, int type, uint32_t seed) {
    // A brush-past, not an impact: the sound swells in (the body pushing into the plant), peaks and fades as the
    // leaves slide off. Grains have soft onsets and even loudness, so there is no click/hit. Every variant jitters
    // length, density, band, grain size, swish level and swell so repeats do not sound alike.
    const RustleType& T = RTYPES[type];
    Rng r{ seed * 2654435761u + 12345u };
    float len = T.len * (0.8f + 0.4f * r.f());
    float dens = T.dens * (0.75f + 0.55f * r.f());
    float band = 0.8f + 0.4f * r.f(); float flo = T.flo * band, fhi = T.fhi * band;
    float glen = T.glen * (0.8f + 0.45f * r.f());
    float bedl = T.bed * (0.8f + 0.4f * r.f()), bedf = T.bedf * (0.85f + 0.3f * r.f());
    float rise = len * (0.18f + 0.2f * r.f());
    float wobF = 15.f + 20.f * r.f();
    int n = (int)(len * RUSTLE_RATE); c.pcm.assign(n, 0.f);
    auto env = [&](float t) {
        if (t < rise) { float x = t / rise; return 0.5f - 0.5f * cosf(3.14159265f * x); }        // raised-cosine swell
        float x = (t - rise) / (len - rise); return powf(1.f - x, 1.6f);                          // slide-off
    };
    int grains = (int)(dens * len);
    for (int g = 0; g < grains; ++g) {
        float t0; do { t0 = r.f() * len; } while (r.f() > env(t0));
        float amp = env(t0) * (0.55f + 0.45f * r.f());
        float fc = flo * powf(fhi / flo, r.f());
        float gl = glen * (0.6f + 0.8f * r.f());
        Biquad bq; bq.bp(fc, 1.2f + 1.5f * r.f());
        int s0 = (int)(t0 * RUSTLE_RATE), gn = (int)(gl * RUSTLE_RATE * 4), atk = (int)(0.0015f * RUSTLE_RATE);
        for (int i = 0; i < gn && s0 + i < n; ++i) {
            float t = i / (float)RUSTLE_RATE, e = expf(-t / gl) * (i < atk ? (float)i / atk : 1.f);
            c.pcm[s0 + i] += amp * bq.run(r.n() * e) * 2.2f;
        }
    }
    Biquad bed, bed2; bed.bp(bedf, 0.6f); bed2.bp(bedf * 1.7f, 0.8f);
    for (int i = 0; i < n; ++i) {
        float t = i / (float)RUSTLE_RATE; float wob = 0.75f + 0.25f * sinf(t * wobF + seed);
        float x = r.n(); c.pcm[i] += bedl * env(t) * wob * (bed.run(x) + 0.5f * bed2.run(x));
    }
    float pk = 1e-6f; for (float v : c.pcm) pk = fmaxf(pk, fabsf(v));
    float g = T.peak * (0.85f + 0.15f * r.f()) / pk; for (float& v : c.pcm) v *= g;
}

static void SynthSwish(RustleClip& c, int size, uint32_t seed) {
    static const float LEN[3] = { 0.16f, 0.24f, 0.42f }, F0[3] = { 2200.f, 1500.f, 700.f }, F1[3] = { 6500.f, 5000.f, 2600.f };
    static const float SLAPS[3] = { 0.f, 1.5f, 4.f }, PEAK[3] = { 0.22f, 0.30f, 0.42f };
    Rng r{ seed * 747796405u + 2891336453u };
    float len = LEN[size] * (0.75f + 0.5f * r.f());
    int n = (int)(len * RUSTLE_RATE); c.pcm.assign(n, 0.f);
    bool up = r.f() < 0.6f;                              // most sweeps rise (stem flicking past), some fall
    float fa = F0[size] * (0.8f + 0.4f * r.f()), fb = F1[size] * (0.8f + 0.4f * r.f());
    float q = 1.5f + 2.5f * r.f(), peakAt = 0.3f + 0.3f * r.f();
    Biquad bq; float prevF = -1.f;
    for (int i = 0; i < n; ++i) {
        float x = i / (float)n;
        float e = x < peakAt ? sinf(1.5707963f * x / peakAt) : powf(1.f - (x - peakAt) / (1.f - peakAt), 2.f);
        float f = up ? fa * powf(fb / fa, x) : fb * powf(fa / fb, x);
        if (fabsf(f - prevF) > 20.f) { float y1 = bq.y1, y2 = bq.y2, x1 = bq.x1, x2 = bq.x2; bq.bp(f, q); bq.y1 = y1; bq.y2 = y2; bq.x1 = x1; bq.x2 = x2; prevF = f; }
        c.pcm[i] += e * bq.run(r.n());
    }
    // soft leaf slaps: short low-passed bursts
    int slaps = (int)(SLAPS[size] * (0.5f + r.f()));
    for (int k = 0; k < slaps; ++k) {
        int s0 = (int)(r.f() * 0.7f * n), sl = (int)(0.012f * RUSTLE_RATE);
        Biquad lp; lp.bp(250.f + 450.f * r.f(), 0.9f); float amp = 2.5f + 2.f * r.f();
        for (int i = 0; i < sl * 3 && s0 + i < n; ++i) { float t = i / (float)sl; c.pcm[s0 + i] += amp * expf(-t * 2.f) * (i < 48 ? i / 48.f : 1.f) * lp.run(r.n()); }
    }
    float pk = 1e-6f; for (float v : c.pcm) pk = fmaxf(pk, fabsf(v));
    float g = PEAK[size] * (0.8f + 0.2f * r.f()) / pk; for (float& v : c.pcm) v *= g;
}

static bool RustleInit() {
    for (int s = 0; s < RUSTLE_SIZES; ++s) for (int v = 0; v < RUSTLE_VARIANTS; ++v) { SynthRustle(g_rustle[s][v], s < 2 ? s : 2 + (v & 1), 1 + s * 37 + v * 7); SynthSwish(g_swish[s][v], s, 101 + s * 53 + v * 11); }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) { Log("XAudio2Create failed"); return false; }
    if (FAILED(g_xa->CreateMasteringVoice(&g_xaMaster))) { Log("mastering voice failed"); return false; }
    WAVEFORMATEX wf = {}; wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; wf.nChannels = 2; wf.nSamplesPerSec = RUSTLE_RATE; wf.wBitsPerSample = 32;
    wf.nBlockAlign = 8; wf.nAvgBytesPerSec = RUSTLE_RATE * 8;
    for (int i = 0; i < RUSTLE_VOICES; ++i) if (FAILED(g_xa->CreateSourceVoice(&g_xaVoices[i], &wf))) { Log("source voice failed"); return false; }
    Log("rustles synthesised (%d sizes x %d variants), XAudio2 ready", RUSTLE_SIZES, RUSTLE_VARIANTS);
    return true;
}

// stereo copies with pan, kept alive while voices play
static std::vector<float> g_rustleBuf[RUSTLE_VOICES];
static void PlayRustle(int size, float volume, float pan /* -1 left .. 1 right */, float pitch = 1.f) {
    if (!g_xa || size < 0 || size >= RUSTLE_SIZES) return;
    // random variant, never one of the last three played for this size
    static int recent[RUSTLE_SIZES][3] = { { -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 } }; static uint32_t rs = 0x1234567u;
    int v;
    for (int tries = 0;; ++tries) {
        rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; v = (int)(rs % RUSTLE_VARIANTS);
        if (tries > 8 || (v != recent[size][0] && v != recent[size][1] && v != recent[size][2])) break;
    }
    recent[size][2] = recent[size][1]; recent[size][1] = recent[size][0]; recent[size][0] = v;
    const RustleClip& c = g_rustle[size][v];
    int vi = -1;
    for (int i = 0; i < RUSTLE_VOICES; ++i) { XAUDIO2_VOICE_STATE st; g_xaVoices[i]->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED); if (st.BuffersQueued == 0) { vi = i; break; } }
    if (vi < 0) return;   // all busy: drop it
    // mix in a swish layer: random variant, level and offset, so every touch is a new combination
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    const RustleClip& w = g_swish[size][rs % RUSTLE_VARIANTS];
    float wg = 0.35f + 0.65f * ((rs >> 8) & 1023) / 1023.f;
    size_t woff = (size_t)(((rs >> 20) & 1023) / 1023.f * 0.06f * RUSTLE_RATE);
    size_t len = c.pcm.size() > w.pcm.size() + woff ? c.pcm.size() : w.pcm.size() + woff;
    float l = volume * sqrtf(0.5f * (1.f - pan)), r = volume * sqrtf(0.5f * (1.f + pan));
    std::vector<float>& b = g_rustleBuf[vi]; b.assign(len * 2, 0.f);
    for (size_t i = 0; i < len; ++i) {
        float x = (i < c.pcm.size() ? c.pcm[i] : 0.f) + (i >= woff && i - woff < w.pcm.size() ? wg * w.pcm[i - woff] : 0.f);
        b[2 * i] = x * l; b[2 * i + 1] = x * r;
    }
    XAUDIO2_BUFFER xb = {}; xb.AudioBytes = (UINT32)(b.size() * 4); xb.pAudioData = (const BYTE*)b.data(); xb.Flags = XAUDIO2_END_OF_STREAM;
    g_xaVoices[vi]->SetFrequencyRatio(pitch);
    g_xaVoices[vi]->SubmitSourceBuffer(&xb);
    g_xaVoices[vi]->Start();
}
