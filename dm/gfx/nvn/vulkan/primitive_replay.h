// Convert NVN restart-delimited primitives to one draw. Keeping one primitive
// sequence also preserves primitive IDs across the restart markers.
#pragma once
#include "../nvn_primitive.h"

static uint32_t source_index(const uint8_t *src, unsigned bytes, uint32_t i) {
    if (!src) return i;
    uint32_t value=0;
    memcpy(&value,src+(size_t)i*bytes,bytes);
    return value;
}
static uint64_t run_count(int prim, uint32_t n) {
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_LINES: return n-n%2;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: return n-n%3;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return n>1 ? 2ull*(n-1) : 0;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN:
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return n>2 ? 3ull*(n-2) : 0;
    case NVN_DRAW_PRIMITIVE_QUADS: return 6ull*(n/4);
    default: return n;
    }
}
static int replay_topology(int prim, int restart) {
    if (prim==NVN_DRAW_PRIMITIVE_QUADS || prim==NVN_DRAW_PRIMITIVE_TRIANGLE_FAN ||
        (restart && prim==NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP)) return NVN_DRAW_PRIMITIVE_TRIANGLES;
    if (restart && prim==NVN_DRAW_PRIMITIVE_LINE_STRIP) return NVN_DRAW_PRIMITIVE_LINES;
    return prim;
}
static uint64_t emit_run(uint32_t *out, const uint8_t *src, unsigned bytes, uint32_t n, int prim) {
    uint64_t count=run_count(prim,n);
    if (!out || !count) return count;
    if (src && (prim==NVN_DRAW_PRIMITIVE_TRIANGLE_FAN || prim==NVN_DRAW_PRIMITIVE_QUADS))
        return nvn_expand_primitive(out,src,n,bytes,(unsigned)prim,0,0);
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: // generated indices for a nonindexed fan
        for (uint32_t i=2;i<n;i++) { *out++=0; *out++=i-1; *out++=i; }
        break;
    case NVN_DRAW_PRIMITIVE_QUADS:
        for (uint32_t q=0;q<n/4;q++) {
            uint32_t i=4*q;
            *out++=i; *out++=i+1; *out++=i+2; *out++=i; *out++=i+2; *out++=i+3;
        }
        break;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP:
        for (uint32_t i=0;i<n-2;i++) {
            // Vulkan's first provoking vertex is i for both strip windings.
            *out++=source_index(src,bytes,i);
            *out++=source_index(src,bytes,i+1+(i&1));
            *out++=source_index(src,bytes,i+2-(i&1));
        }
        break;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP:
        for (uint32_t i=1;i<n;i++) { *out++=source_index(src,bytes,i-1); *out++=source_index(src,bytes,i); }
        break;
    default:
        for (uint32_t i=0;i<count;i++) *out++=source_index(src,bytes,i);
        break;
    }
    return count;
}
static uint64_t replay_indices(uint32_t *out, const uint8_t *src, unsigned bytes, uint32_t n,
                               int prim, int restart, uint32_t marker) {
    if (!restart) return emit_run(out,src,bytes,n,prim);
    uint64_t written=0;
    uint32_t start=0;
    for (uint32_t i=0;i<n;i++) if (source_index(src,bytes,i)==marker) {
        written+=emit_run(out ? out+written : NULL,src+(size_t)start*bytes,bytes,i-start,prim);
        start=i+1;
    }
    return written+emit_run(out ? out+written : NULL,src+(size_t)start*bytes,bytes,n-start,prim);
}
