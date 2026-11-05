// Independent raw-word oracle for the translated PrimitiveID geometry corpus.
#ifndef DM_TEST_GEOMETRY_CORPUS_H
#define DM_TEST_GEOMETRY_CORPUS_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { GC_PRIMS = 97, GC_INPUT = 17 + GC_PRIMS * 12, GC_RECORDS = GC_PRIMS * 2 * 3 * 3,
       GC_VWORDS = GC_RECORDS * 134 + 16, GC_IWORDS = GC_RECORDS * 2 + 16, GC_INFO = 264,
       GC_MAP = GC_PRIMS * 6, GC_INPUT_WORDS = GC_INPUT * 2 * 144 };
typedef struct { unsigned topology, invocations, instances, primitives, base; } geometry_case;
static const unsigned gc_width[] = {1, 2, 4, 3, 6}, gc_vertices[] = {1, 2, 2, 3, 3};
static const unsigned gc_corner[5][3] = {{0}, {0, 1}, {1, 2}, {0, 1, 2}, {0, 2, 4}};
static uint32_t gc_value(unsigned vertex, unsigned instance, unsigned word) {
    if (word < 5) { const uint32_t position[] = {0x3e800000, 0xbe800000, 0x3f000000, 0x3f800000, 0x3f800000}; return position[word]; }
    if (word == 16) return vertex;
    if (word == 17) return 7 + instance;
    if (word == 18) return vertex ^ (7 + instance);
    if (word == 19) return 1;
    // Includes raw quiet-NaN payloads: geometry copy must preserve their bits.
    return 0x7fc00000u | ((vertex * 128 + word + instance * 1024) & 0x3fffff);
}
static void gc_fill(geometry_case c, uint32_t *buffers[5]) {
    for (unsigned i = 0; i < 2; i++) for (unsigned v = 0; v < GC_INPUT; v++)
        for (unsigned w = 0; w < 144; w++) buffers[0][(i * GC_INPUT + v) * 144 + w] = gc_value(v, i, w);
    memset(buffers[1], 0xcc, GC_VWORDS * 4); memset(buffers[2], 0xcc, GC_IWORDS * 4);
    memset(buffers[3], 0, GC_INFO * 4); memset(buffers[4], 0xcc, GC_MAP * 4);
    uint32_t info[] = {GC_INPUT, c.instances, 0, 0, c.primitives, c.base, 0, 0};
    memcpy(buffers[3], info, sizeof info);
    for (unsigned p = 0; p < c.primitives; p++) for (unsigned k = 0; k < gc_width[c.topology]; k++)
        buffers[4][p * gc_width[c.topology] + k] = 17 + (p * gc_width[c.topology] + k) * 2;
}
static int gc_check(geometry_case c, const uint32_t *vertices, const uint32_t *indices, FILE *output) {
    unsigned emitted = gc_vertices[c.topology], records = c.primitives * c.instances * c.invocations * emitted;
    for (unsigned at = 0; at < GC_VWORDS; at++) {
        uint32_t want = 0xcccccccc;
        if (at < records * 134) {
            unsigned record = at / 134, word = at % 134;
            unsigned corner = record % emitted, primitive = record / emitted / c.invocations % c.primitives;
            unsigned instance = record / emitted / c.invocations / c.primitives;
            unsigned vertex = 17 + (primitive * gc_width[c.topology] + gc_corner[c.topology][corner]) * 2;
            want = word == 5 ? primitive + c.base + 1 : gc_value(vertex, instance, word < 5 ? word : word + 10);
        }
        if (vertices[at] != want) {
            fprintf(stderr, "topology%u inv%u inst%u n%u base%u vertex word%u: %08x != %08x\n",
                c.topology, c.invocations, c.instances, c.primitives, c.base, at, vertices[at], want); return 1;
        }
    }
    for (unsigned at = 0; at < GC_IWORDS; at++) {
        uint32_t want = 0xcccccccc;
        if (at < records * 2) {
            unsigned corner = at % (emitted * 2);
            want = corner < emitted ? at / (emitted * 2) * emitted + corner : UINT32_MAX;
        }
        if (indices[at] != want) { fprintf(stderr, "topology%u index%u: %08x != %08x\n", c.topology, at, indices[at], want); return 1; }
    }
    if (output && (fwrite(vertices, 4, records * 134 + 16, output) != records * 134 + 16 ||
                   fwrite(indices, 4, records * 2 + 16, output) != records * 2 + 16)) return 1;
    return 0;
}
#endif
