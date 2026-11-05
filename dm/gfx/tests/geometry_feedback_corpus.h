#ifndef DM_GEOMETRY_FEEDBACK_CORPUS_H
#define DM_GEOMETRY_FEEDBACK_CORPUS_H
#include "geometry_corpus.h"
_Static_assert(GC_MAX_VERTICES == 9, "geometry feedback corpus needs explicit-emission variant");
enum { GF_CAPTURE_PREFIX = 16, GF_ORDER_PREFIX = 7, GF_PREFIX = 13,
       GF_CAPTURE_WORDS = GF_CAPTURE_PREFIX + GC_RECORDS * 144 + 16,
       GF_ORDER_WORDS = GF_ORDER_PREFIX + GC_RECORDS + 16,
       GF_OUTPUT_WORDS = GF_PREFIX + GC_RECORDS * 4 + 16 };
static const unsigned gf_corners[] = {0, 1, 2, 3, 4, 5, 5, 4, 6};
static unsigned gf_records(geometry_case c) { return c.primitives * c.instances * c.invocations * 9; }
static void gf_fill(geometry_case c, uint32_t *order, uint32_t *output) {
    memset(order, 0xcc, GF_ORDER_WORDS * 4); memset(output, 0xcc, GF_OUTPUT_WORDS * 4);
    for (unsigned i = 0; i < gf_records(c); i++) order[GF_ORDER_PREFIX + i] = i / 9 * 9 + gf_corners[i % 9];
}
static int gf_check(geometry_case c, const uint32_t *actual, FILE *output) {
    unsigned records = gf_records(c);
    for (unsigned at = 0; at < GF_OUTPUT_WORDS; at++) {
        uint32_t expected = 0xcccccccc;
        if (at >= GF_PREFIX && at - GF_PREFIX < records * 4) {
            unsigned linear = (at - GF_PREFIX) / 4, component = (at - GF_PREFIX) % 4, group = linear / 9;
            uint32_t value[] = {group / c.invocations % c.primitives + c.base, group % c.invocations, gf_corners[linear % 9], 1};
            expected = value[component];
        }
        if (actual[at] != expected) {
            fprintf(stderr, "feedback topology%u inv%u inst%u n%u base%u word%u: %08x != %08x\n",
                    c.topology, c.invocations, c.instances, c.primitives, c.base, at, actual[at], expected);
            return 1;
        }
    }
    unsigned words = GF_PREFIX + records * 4 + 16;
    return fwrite(actual, 4, words, output) != words;
}
#endif
