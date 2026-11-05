// The production codec adapters and object map, with SDK functions as argument /
// result oracles. This qualifies host forwarding, not the SDK's Opus algorithm.
#include "../hle_os.c"
#include "../hle_codec.c"
#include "../hle_sdkinit.c"
#include <assert.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

module_t *g_mod; int g_nmod;
__thread _Atomic int dm_host_work;
int dm_sdk_ounce(void) { return 0; }
void crash_watch_frame(void) {}
void dm_guest_clock_advanced(void) {}
static unsigned calls, fini_calls, reset_calls, ctor_calls;
static int result;
static const void *expected_self, *expected_work, *expected_mapping;
static size_t expected_size;
static int expected_rate, expected_ch, expected_streams, expected_stereo;
typedef struct { int rate, ch, streams, stereo; unsigned resets; } soft_state;
static void ctor(void *self) { assert(self == expected_self); ctor_calls++; }
static size_t work_size(int rate, int ch) {
    assert(rate == expected_rate && ch == expected_ch); calls++; return (size_t)0x123456789ull;
}
static size_t ms_size(int rate, int ch, int streams, int stereo) {
    assert(streams == expected_streams && stereo == expected_stereo); return work_size(rate, ch);
}
static int init(void *p, int rate, int ch, void *buf, size_t size) {
    assert(p && p != expected_self && buf == expected_work && size == expected_size);
    assert(rate == expected_rate && ch == expected_ch); calls++;
    *(soft_state *)p = (soft_state){rate, ch, 0, 0, 0}; return result;
}
static int ms_init(void *p, int rate, int ch, int streams, int stereo, const uint8_t *map, void *buf, size_t size) {
    assert(streams == expected_streams && stereo == expected_stereo && map == expected_mapping);
    int r = init(p, rate, ch, buf, size); soft_state *s = p; s->streams = streams; s->stereo = stereo; return r;
}
static void fini(void *p) { assert(p); fini_calls++; }
static int rate(const void *p) { return ((const soft_state *)p)->rate; }
static int channels(const void *p) { return ((const soft_state *)p)->ch; }
static int streams(const void *p) { return ((const soft_state *)p)->streams; }
static int stereo(const void *p) { return ((const soft_state *)p)->stereo; }
static void reset(void *p) { ((soft_state *)p)->resets++; reset_calls++; }
static size_t *expected_consumed; static int *expected_samples;
static int16_t *expected_output; static const void *expected_input;
static int decode(void *p, size_t *consumed, int *samples, int16_t *out, size_t out_size, const void *in, size_t in_size) {
    assert(p == expected_self && consumed == expected_consumed && samples == expected_samples);
    assert(out == expected_output && in == expected_input && out_size == 47 && in_size == 23);
    *consumed = 19; *samples = 5; out[0] = -1234; calls++; return result;
}
static unsigned env_calls, allocator_calls, aligned_calls;
static const char *env_name; static char env_value[] = "sdk-value";
static char *sdk_getenv(const char *name) { env_calls++; env_name = name; return env_value; }
static void *allocate(void *self, size_t size) { assert(self == expected_self && size == expected_size); allocator_calls++; return (void *)expected_work; }
static void *aligned(void *self, size_t size, size_t alignment) { assert(alignment == 0x10000); aligned_calls++; return allocate(self, size); }
static const struct { const char *name; void *fn; } symbols[] = {
    {"_ZN2nn5codec11OpusDecoderC1Ev", ctor},
    {"_ZN2nn5codec22OpusMultiStreamDecoderC1Ev", ctor},
    {"_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii", work_size},
    {"_ZN2nn5codec39GetOpusMultiStreamDecoderWorkBufferSizeEiiii", ms_size},
    {"_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm", init},
    {"_ZN2nn5codec32InitializeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeEiiiiPKhPvm", ms_init},
    {"_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE", fini},
    {"_ZN2nn5codec30FinalizeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE", fini},
    {"_ZN2nn5codec24GetOpusDecoderSampleRateEPKNS0_15OpusDecoderTypeE", rate},
    {"_ZN2nn5codec35GetOpusMultiStreamDecoderSampleRateEPKNS0_26OpusMultiStreamDecoderTypeE", rate},
    {"_ZN2nn5codec26GetOpusDecoderChannelCountEPKNS0_15OpusDecoderTypeE", channels},
    {"_ZN2nn5codec37GetOpusMultiStreamDecoderChannelCountEPKNS0_26OpusMultiStreamDecoderTypeE", channels},
    {"_ZN2nn5codec41GetOpusMultiStreamDecoderTotalStreamCountEPKNS0_26OpusMultiStreamDecoderTypeE", streams},
    {"_ZN2nn5codec42GetOpusMultiStreamDecoderStereoStreamCountEPKNS0_26OpusMultiStreamDecoderTypeE", stereo},
    {"_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE", reset},
    {"_ZN2nn5codec27ResetOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE", reset},
    {"_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm", decode},
    {"_ZN2nn5codec32DecodeOpusMultiStreamInterleavedEPNS0_26OpusMultiStreamDecoderTypeEPmPiPsmPKvm", decode},
    {"getenv", sdk_getenv},
};
void *guest_sym(const char *name) {
    for (unsigned i = 0; i < sizeof symbols / sizeof *symbols; i++) if (!strcmp(name, symbols[i].name)) return symbols[i].fn;
    fprintf(stderr, "unexpected SDK lookup: %s\n", name); abort();
}
void *module_sym(module_t *m, const char *name) {
    assert(m == g_mod);
    if (!strcmp(name, "_ZN2nn3mem17StandardAllocator8AllocateEm")) return allocate;
    assert(!strcmp(name, "_ZN2nn3mem17StandardAllocator8AllocateEmm")); return aligned;
}
static void *seen[32]; static unsigned nseen;
static void mark(void *p) { for (unsigned i = 0; i < nseen; i++) if (seen[i] == p) return; assert(nseen < 32); seen[nseen++] = p; }
#define RUN(fn, ...) (mark((void *)fn), fn(__VA_ARGS__))
static void unchanged(const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) assert(b[i] == 0xa5); }
static void codec(void) {
    uint8_t hw[4][72], work[97], map[8] = {0,1,2,3,4,5,6,7}; memset(hw, 0xa5, sizeof hw);
    expected_self = hw[0]; RUN(h_codec_HwOpusCtor, hw[0], -7); RUN(h_codec_HwOpusMsCtor, hw[0], 99); assert(ctor_calls == 2);
    expected_rate = 48000; expected_ch = 6; expected_streams = 4; expected_stereo = 2;
    assert(RUN(h_codec_HwOpusWorkSize, 48000, 6, -1) == 0x123456789ull);
    assert(RUN(h_codec_HwOpusWorkSize2, 48000, 6) == 0x123456789ull);
    assert(RUN(h_codec_HwOpusMsWorkSize, 48000, 6, 4, 2) == 0x123456789ull);
    assert(RUN(h_codec_HwOpusMsWorkSize5, 48000, 6, 4, 2, -1) == 0x123456789ull);
    expected_work = work; expected_size = sizeof work; expected_mapping = map;
    const int results[] = {0, 403, (int)0x80000001u};
    for (unsigned cycle = 0; cycle < 64; cycle++) {
        void *states[4] = {0};
        for (unsigned i = 0; i < 4; i++) {
            expected_self = hw[i]; expected_rate = 24000 + 8000 * i; expected_ch = i + 1;
            for (unsigned r = 0; r < 3; r++) {
                result = results[r];
                int value = i & 1 ? RUN(h_codec_HwOpusMsInit, hw[i], expected_rate, expected_ch, 4, 2, map, work, sizeof work, -7)
                                 : RUN(h_codec_HwOpusInit, hw[i], expected_rate, expected_ch, work, sizeof work, 99);
                assert(value == result); // The SDK owns validation, including failed initialization.
                void *p = objmap_get(hw[i]); assert(p && (!states[i] || p == states[i])); states[i] = p;
                for (unsigned j = 0; j < i; j++) assert(p != states[j]);
            }
            if (i & 1) {
                assert(RUN(h_codec_HwOpusMsRate, hw[i]) == expected_rate && RUN(h_codec_HwOpusMsCh, hw[i]) == expected_ch);
                assert(RUN(h_codec_HwOpusMsTotal, hw[i]) == 4 && RUN(h_codec_HwOpusMsStereo, hw[i]) == 2);
                RUN(h_codec_HwOpusMsReset, hw[i]); assert(!RUN(h_codec_HwOpusMsTime, hw[i]));
            } else {
                assert(RUN(h_codec_HwOpusRate, hw[i]) == expected_rate && RUN(h_codec_HwOpusCh, hw[i]) == expected_ch);
                RUN(h_codec_HwOpusReset, hw[i]);
            }
            assert(((soft_state *)states[i])->resets == 1);
            size_t consumed[3] = {123,99,456}; int samples[3] = {123,99,456}; int16_t output[32]; uint8_t input[32];
            memset(output, 0xa5, sizeof output); memset(input, 0xa5, sizeof input);
            expected_self = states[i]; expected_consumed = consumed + 1; expected_samples = samples + 1;
            expected_output = output + 1; expected_input = input + 1;
            for (unsigned r = 0; r < 3; r++) {
                result = results[r];
                int value = i & 1 ? RUN(h_codec_HwOpusMsDecode, hw[i], consumed + 1, samples + 1, output + 1, 47, input + 1, 23)
                                 : RUN(h_codec_HwOpusDecode, hw[i], consumed + 1, samples + 1, output + 1, 47, input + 1, 23);
                assert(value == result && consumed[1] == 19 && samples[1] == 5 && output[1] == -1234);
                assert(consumed[0] == 123 && consumed[2] == 456 && samples[0] == 123 && samples[2] == 456);
                unchanged(output, 2); unchanged(output + 2, sizeof output - 4); unchanged(input, sizeof input);
            }
        }
        for (unsigned i = 0; i < 4; i++) {
            unsigned before = fini_calls;
            if (i & 1) { RUN(h_codec_HwOpusMsFini, hw[i]); RUN(h_codec_HwOpusMsFini, hw[i]); }
            else { RUN(h_codec_HwOpusFini, hw[i]); RUN(h_codec_HwOpusFini, hw[i]); }
            assert(fini_calls == before + 1 && !objmap_get(hw[i]));
        }
        unchanged(hw, sizeof hw);
    }
    assert(reset_calls == 256 && fini_calls == 256);
    for (unsigned i = 0; i < OM_SIZE; i++) assert(!g_om[i].k || (g_om[i].k == (void *)1 && !g_om[i].v));
    fflush(NULL); pid_t child = fork(); assert(child >= 0);
    if (!child) { struct rlimit r = {0,0}; setrlimit(RLIMIT_CORE, &r); h_codec_HwOpusRate(hw[0]); _exit(1); }
    int status; assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    unsigned long bytes; const hle_force_ent *e = DM_SECTION_DATA(swforce, &bytes); unsigned count = 0;
    for (size_t i = 0; i < bytes / sizeof *e; i++) if (!strncmp(e[i].name, "_ZN2nn5codec", 11)) {
        unsigned j = 0; while (j < nseen && seen[j] != e[i].fn) j++; assert(j < nseen && e[i].why && e[i].why[0]); count++;
    }
    assert(count == 21 && nseen == count);
    printf("PASS: %u codec registrations, independent states, SDK arguments/results, 256 lifetimes and failure after finalization\n", count);
}
static void environment(void) {
    setenv("DM_GAME_ENV", "NX_ALPHA=one NX_ALPHABET=two NX_EMPTY= invalid NX_EQ=a=b", 1);
    setenv("NX_HOST_ONLY", "must-not-leak", 1);
    assert(!strcmp(g_getenv("NX_ALPHA"), "one") && !strcmp(g_getenv("NX_ALPHABET"), "two"));
    assert(!strcmp(g_getenv("NX_EMPTY"), "") && !strcmp(g_getenv("NX_EQ"), "a=b") && !env_calls);
    const char *names[] = {"NX_ALPH", "NX_HOST_ONLY", "", NULL};
    for (unsigned i = 0; i < 4; i++) { assert(g_getenv(names[i]) == env_value && env_name == names[i]); }
    assert(env_calls == 4); setenv("DM_GAME_ENV", "NX_ALPHA=changed", 1); assert(!strcmp(g_getenv("NX_ALPHA"), "one"));
    module_t sdk = {.name = "sdk"}; g_mod = &sdk; g_nmod = 1; int object, value; expected_self = &object; expected_work = &value;
    const size_t sizes[] = {0, 65535, 65536, 65537, 0x123456789ull};
    for (unsigned i = 0; i < 5; i++) { expected_size = sizes[i]; assert(h_mem_SAAllocate(&object, sizes[i]) == &value); }
    assert(allocator_calls == 5 && aligned_calls == (DM_GAME_ALLOC_BIG_ALIGN ? 3u : 0u));
    h_nndiagStartup(); h_oe_Initialize(); h_aoc_Initialize(); h_err_InitCtx(); h_fs_InitApp(); h_pctl_Initialize(); h_diag_AbortObs();
    puts("PASS: profile environment isolation, empty/prefix/equal values, SDK fallback, allocator forwarding and seven service initializers");
}
static uint64_t reference_rng(uint64_t *state) {
    *state += UINT64_C(0x9e3779b97f4a7c15); uint64_t v = *state;
    v = (v ^ (v >> 30)) * UINT64_C(0xbf58476d1ce4e5b9); v = (v ^ (v >> 27)) * UINT64_C(0x94d049bb133111eb); return v ^ (v >> 31);
}
static void entropy(int deterministic) {
    if (deterministic) setenv("DM_DETCLOCK", "1000", 1); else unsetenv("DM_DETCLOCK");
    uint64_t state = UINT64_C(0x5357444554524e47), before = 0; uint8_t output[34], expected[32];
    for (size_t n = 0; n <= 32; n++) {
        memset(output, 0xa5, sizeof output);
        for (size_t i = 0; i < n; i += 8) { uint64_t v = reference_rng(&state); memcpy(expected + i, &v, n - i < 8 ? n - i : 8); }
        if (n & 1) g_csrng_bytes(output + 1, n); else assert(!g_getentropy(output + 1, n));
        assert(output[0] == 0xa5); unchanged(output + 1 + n, sizeof output - 1 - n);
        if (deterministic) assert(!memcmp(output + 1, expected, n));
        uint64_t v = dm_entropy64(); if (deterministic) assert(v == reference_rng(&state)); else if (n) assert(v != before);
        before = v;
    }
    puts("PASS: entropy entry points share the selected stream and preserve output bounds for all partial-word sizes");
}
int main(int argc, char **argv) {
    assert(argc == 2); entropy(!strcmp(argv[1], "det")); environment(); codec(); return 0;
}
