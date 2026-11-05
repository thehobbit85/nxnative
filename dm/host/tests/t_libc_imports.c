// Call the real import records with guest-facing AAPCS64 signatures. These are
// boundary checks for bionic/Darwin forwarding, not a replacement libc suite.
#include "../hle_table.c"
#include <assert.h>
#define FN(name, type) ((type)hle_lib_##name.fn)
int main(void) {
    unsigned long bytes; const hle_ent *e = DM_SECTION_DATA(swhle, &bytes);
    assert(bytes / sizeof *e == 63);
    for (size_t i = 0; i < bytes / sizeof *e; i++) {
        assert(e[i].name && e[i].fn);
        for (size_t j = 0; j < i; j++) assert(strcmp(e[i].name, e[j].name));
    }
    uint8_t b[40]; memset(b, 0xa5, sizeof b);
    assert(FN(memset, void *(*)(void *, int, size_t))(b + 1, 0x1234, 20) == b + 1);
    assert(b[0] == 0xa5 && b[21] == 0xa5); for (unsigned i = 1; i < 21; i++) assert(b[i] == 0x34);
    assert(FN(memcpy, void *(*)(void *, const void *, size_t))(b + 1, "abcdef", 7) == b + 1);
    assert(FN(memmove, void *(*)(void *, const void *, size_t))(b + 2, b + 1, 7) == b + 2);
    assert(!memcmp(b + 1, "aabcdef", 8) && b[0] == 0xa5 && b[21] == 0xa5);
    assert(FN(memchr, void *(*)(const void *, int, size_t))(b + 1, 'd', 7) == b + 5);
    assert(FN(memcmp, int (*)(const void *, const void *, size_t))(b + 2, "abcdef", 7) == 0);
    char text[32] = "part";
    assert(FN(strcat, char *(*)(char *, const char *))(text, "-two") == text && !strcmp(text, "part-two"));
    assert(FN(strlen, size_t (*)(const char *))(text) == 8 && FN(strnlen, size_t (*)(const char *, size_t))(text, 3) == 3);
    assert(FN(strstr, char *(*)(const char *, const char *))(text, "two") == text + 5);
    assert(!FN(strcasecmp, int (*)(const char *, const char *))(text, "PART-TWO"));
    const char number[] = "-4294967297tail"; char *end = NULL;
    assert(FN(strtol, long (*)(const char *, char **, int))(number, &end, 10) == -4294967297l && end == number + 11);
    assert(FN(strtoull, unsigned long long (*)(const char *, char **, int))("ffffffffffffffff", &end, 16) == UINT64_MAX && !*end);
    assert(FN(strtod, double (*)(const char *, char **))("-1.25tail", &end) == -1.25 && !strcmp(end, "tail"));
    assert(FN(sqrtf, float (*)(float))(81.0f) == 9.0f && FN(pow, double (*)(double, double))(2.0, 40.0) == 1099511627776.0);
    int exponent[3] = {123,99,456}; assert(FN(frexp, double (*)(double, int *))(12.0, exponent + 1) == 0.75);
    assert(exponent[0] == 123 && exponent[1] == 4 && exponent[2] == 456);
    float integer[3] = {123,99,456}; assert(FN(modff, float (*)(float, float *))(-3.25f, integer + 1) == -0.25f);
    assert(integer[0] == 123 && integer[1] == -3.0f && integer[2] == 456);
    assert(FN(lroundf, long (*)(float))(-1.5f) == -2);
    // The calendar forwarding uses the same 64-bit scalar and tm ABI on both
    // hosts; the time-service fixture separately covers guest timezone rules.
    _Static_assert(sizeof(time_t) == 8 && sizeof(long) == 8, "guest scalar ABI");
    _Static_assert(offsetof(struct tm, tm_sec) == 0 && offsetof(struct tm, tm_isdst) == 32 &&
                   offsetof(struct tm, tm_gmtoff) == 40 && offsetof(struct tm, tm_zone) == 48 && sizeof(struct tm) == 56, "guest tm ABI");
    struct { uint64_t pre; struct tm tm; uint64_t post; } g; memset(&g, 0xa5, sizeof g);
    time_t epoch = 1709251198;
    assert(FN(gmtime_r, struct tm *(*)(const time_t *, struct tm *))(&epoch, &g.tm) == &g.tm);
    assert(g.pre == 0xa5a5a5a5a5a5a5a5ull && g.post == g.pre && g.tm.tm_year == 124 && g.tm.tm_mon == 1 && g.tm.tm_mday == 29);
    assert(g.tm.tm_hour == 23 && g.tm.tm_min == 59 && g.tm.tm_sec == 58 && !g.tm.tm_gmtoff);
    assert(FN(timegm, time_t (*)(struct tm *))(&g.tm) == epoch);
    char date[24]; memset(date, 0xa5, sizeof date);
    assert(FN(strftime, size_t (*)(char *, size_t, const char *, const struct tm *))(date + 1, 20, "%Y-%m-%d %H:%M:%S", &g.tm) == 19);
    assert((uint8_t)date[0] == 0xa5 && (uint8_t)date[21] == 0xa5 && !strcmp(date + 1, "2024-02-29 23:59:58"));
    puts("PASS: 63 unique libc imports; guarded memory/string, 64-bit conversion, FP registers/output pointers and calendar ABI forwarding");
    return 0;
}
