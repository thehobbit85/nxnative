// Reduce the actual contributing texels with ordinary point samplers. This
// keeps min/max filtering independent of optional native reduction support.
#pragma once
#include "spv_texel_bias.h"

typedef struct {
    uint32_t binding, minimum, min_linear, mag_linear;
    float min_lod, max_lod, bias;
} sample_reduction;
typedef struct { uint32_t type, scalar, lanes, image, binding, dimension, array; unsigned is_type:1; } reduction_id;
typedef struct {
    texel_module code;
    reduction_id *ids;
    uint32_t bound, glsl, floating, float2, float3, float4, sint, int2, int3, bool2;
    int failed;
} reduction_module;

static void reduction_append(reduction_module *m, texel_words *out, uint32_t op, const uint32_t *args, size_t count) {
    uint32_t words[16];
    if (count >= sizeof words / sizeof *words) { m->failed = 1; return; }
    words[0] = (uint32_t)((count + 1) << 16) | op;
    memcpy(words + 1, args, count * sizeof *args);
    if (!texel_append(out, words, count + 1)) m->failed = 1;
}
#define REDUCTION_GLOBAL(m, op, ...) reduction_append(m, &(m)->code.globals, op, (uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__})/4)
static uint32_t reduction_value(reduction_module *m, uint32_t op, uint32_t type, const uint32_t *args, size_t count) {
    uint32_t operands[15], result = m->code.next++;
    if (count > 13) { m->failed = 1; return 0; }
    operands[0] = type; operands[1] = result;
    memcpy(operands + 2, args, count * sizeof *args);
    reduction_append(m, &m->code.body, op, operands, count + 2);
    return result;
}
#define REDUCTION_VALUE(m, op, type, ...) reduction_value(m, op, type, (uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__})/4)
#define REDUCTION_EXT(m, type, op, ...) REDUCTION_VALUE(m, 12, type, (m)->glsl, op, __VA_ARGS__)

static uint32_t reduction_float(reduction_module *m, float value) {
    uint32_t bits; memcpy(&bits, &value, sizeof bits);
    uint32_t id = texel_constant(&m->code, m->floating, bits);
    if (!id) m->failed = 1;
    return id;
}
static uint32_t reduction_vector(reduction_module *m, uint32_t scalar, unsigned lanes) {
    for (uint32_t id = 1; id < m->bound; id++)
        if (m->ids[id].scalar == scalar && m->ids[id].lanes == lanes) return id;
    uint32_t id = m->code.next++;
    REDUCTION_GLOBAL(m, 23, id, scalar, lanes);
    return id;
}
static uint32_t reduction_mask(reduction_module *m, uint32_t condition, unsigned lanes) {
    if (lanes == 2) return REDUCTION_VALUE(m, 80, m->bool2, condition, condition);
    return REDUCTION_VALUE(m, 80, m->code.bool4, condition, condition, condition, condition);
}
static uint32_t reduction_component(reduction_module *m, uint32_t value, unsigned lane) {
    return REDUCTION_VALUE(m, 81, m->floating, value, lane);
}

static uint32_t reduction_level(reduction_module *m, uint32_t sampled, uint32_t image, uint32_t coordinate,
                                uint32_t level, uint32_t linear, unsigned array, unsigned minimum) {
    uint32_t integer_level = REDUCTION_VALUE(m, 110, m->sint, level); // OpConvertFToS
    uint32_t size = REDUCTION_VALUE(m, 103, array ? m->int3 : m->int2, image, integer_level);
    if (array) size = REDUCTION_VALUE(m, 79, m->int2, size, size, 0, 1);
    size = REDUCTION_VALUE(m, 111, m->float2, size); // OpConvertSToF
    uint32_t xy = array ? REDUCTION_VALUE(m, 79, m->float2, coordinate, coordinate, 0, 1) : coordinate;
    uint32_t layer = array ? reduction_component(m, coordinate, 2) : 0;
    uint32_t zero = reduction_float(m, 0), half = reduction_float(m, .5f), one = reduction_float(m, 1);
    uint32_t half2 = REDUCTION_VALUE(m, 80, m->float2, half, half);
    uint32_t zero2 = REDUCTION_VALUE(m, 80, m->float2, zero, zero);
    uint32_t scaled = REDUCTION_VALUE(m, 133, m->float2, xy, size);
    uint32_t centred = REDUCTION_VALUE(m, 131, m->float2, scaled, half2);
    uint32_t base = REDUCTION_EXT(m, m->float2, 8, centred); // GLSL Floor
    uint32_t fraction = REDUCTION_VALUE(m, 131, m->float2, centred, base);
    uint32_t nearest = REDUCTION_EXT(m, m->float2, 8, scaled);
    uint32_t mask = reduction_mask(m, linear, 2);
    base = REDUCTION_VALUE(m, 169, m->float2, mask, base, nearest);
    fraction = REDUCTION_VALUE(m, 169, m->float2, mask, fraction, zero2);
    uint32_t fx = reduction_component(m, fraction, 0), fy = reduction_component(m, fraction, 1);
    uint32_t x_weight = REDUCTION_VALUE(m, 186, m->code.boolean, fx, zero);
    uint32_t y_weight = REDUCTION_VALUE(m, 186, m->code.boolean, fy, zero);
    base = REDUCTION_VALUE(m, 129, m->float2, base, half2);
    uint32_t result = 0;
    for (unsigned tap = 0; tap < 4; tap++) {
        uint32_t offset = REDUCTION_VALUE(m, 80, m->float2, tap & 1 ? one : zero, tap & 2 ? one : zero);
        uint32_t point = REDUCTION_VALUE(m, 129, m->float2, base, offset);
        point = REDUCTION_VALUE(m, 136, m->float2, point, size);
        if (array) {
            uint32_t x = reduction_component(m, point, 0), y = reduction_component(m, point, 1);
            point = REDUCTION_VALUE(m, 80, m->float3, x, y, layer);
        }
        uint32_t value = REDUCTION_VALUE(m, 88, m->float4, sampled, point, 2, level);
        if (!tap) { result = value; continue; }
        uint32_t weight = tap == 1 ? x_weight : tap == 2 ? y_weight :
            REDUCTION_VALUE(m, 167, m->code.boolean, x_weight, y_weight); // OpLogicalAnd
        uint32_t reduced = REDUCTION_EXT(m, m->float4, minimum ? 37 : 40, result, value);
        result = REDUCTION_VALUE(m, 169, m->float4, reduction_mask(m, weight, 4), reduced, result);
    }
    return result;
}

static int reduction_sample(reduction_module *m, const uint32_t *p, unsigned len, const sample_reduction *filter) {
    // Other sample forms are diagnosed before their sampler can become point
    // filtered. Additional shapes and derivative-driven samples are separate.
    if (len != 7 || p[5] != 2 || p[1] != m->float4 || p[3] >= m->bound || p[4] >= m->bound || p[6] >= m->bound) return 0;
    uint32_t sampled_type = m->ids[p[3]].type;
    uint32_t image_type = m->ids[sampled_type].image;
    if (!image_type || m->ids[image_type].dimension != 1 || m->ids[image_type].array > 1 ||
        m->ids[p[6]].type != m->floating) return 0;
    unsigned array = m->ids[image_type].array;
    if (m->ids[p[4]].type != (array ? m->float3 : m->float2)) return 0;
    uint32_t image = REDUCTION_VALUE(m, 100, image_type, p[3]);
    uint32_t levels = REDUCTION_VALUE(m, 106, m->sint, image);
    uint32_t last = REDUCTION_VALUE(m, 111, m->floating, levels);
    uint32_t zero = reduction_float(m, 0), one = reduction_float(m, 1);
    last = REDUCTION_VALUE(m, 131, m->floating, last, one);
    uint32_t lower = reduction_float(m, filter->min_lod), upper = reduction_float(m, filter->max_lod);
    uint32_t lod = REDUCTION_VALUE(m, 129, m->floating, p[6], reduction_float(m, filter->bias));
    lod = REDUCTION_EXT(m, m->floating, 43, lod, lower, upper);
    uint32_t minify = REDUCTION_VALUE(m, 186, m->code.boolean, lod, zero);
    uint32_t min_filter = reduction_float(m, filter->min_linear), mag_filter = reduction_float(m, filter->mag_linear);
    uint32_t selected = REDUCTION_VALUE(m, 169, m->floating, minify, min_filter, mag_filter);
    uint32_t linear = REDUCTION_VALUE(m, 186, m->code.boolean, selected, zero);
    lod = REDUCTION_EXT(m, m->floating, 43, lod, zero, last);
    uint32_t first = REDUCTION_EXT(m, m->floating, 8, lod), second = REDUCTION_EXT(m, m->floating, 9, lod);
    uint32_t a = reduction_level(m, p[3], image, p[4], first, linear, array, filter->minimum);
    uint32_t b = reduction_level(m, p[3], image, p[4], second, linear, array, filter->minimum);
    uint32_t value = REDUCTION_EXT(m, m->float4, filter->minimum ? 37 : 40, a, b);
    uint32_t copy[] = {m->float4, p[2], value};
    reduction_append(m, &m->code.body, 83, copy, 3);
    return !m->failed;
}

static uint32_t *spv_sample_reduction(const uint32_t *words, size_t count, const sample_reduction *filters,
                                     size_t nf, size_t *out_count, const char **reason) {
    *reason = "invalid sampler reduction module";
    if (count < 5 || words[0] != 0x07230203 || !words[3] || words[3] > 0x3fffff) return NULL;
    reduction_module m = {.bound=words[3], .code={.next=words[3]}};
    texel_words out = {0}; size_t functions = count;
    int has_queries = 0, needs_glsl = 0;
    m.ids = calloc(m.bound, sizeof *m.ids);
    if (!m.ids) goto fail;
    for (size_t i = 5; i < count;) {
        const uint32_t *p = words + i; unsigned len = p[0] >> 16, op = p[0] & 0xffff;
        if (!len || len > count - i) goto fail;
        if (op == 17 && len == 2 && p[1] == 50) has_queries = 1;
        if (op == 11 && len == 6 && !memcmp(p + 2, "GLSL.std.450", 12)) m.glsl = p[1];
        if (op == 71 && len == 4 && p[1] < m.bound && p[2] == 33)
            for (size_t f = 0; f < nf; f++) if (filters[f].binding == p[3]) m.ids[p[1]].binding = (uint32_t)f + 1;
        if (op >= 19 && op <= 33 && len >= 2 && p[1] < m.bound) {
            reduction_id *id = m.ids + p[1]; id->is_type = 1;
            if (op == 20) m.code.boolean = p[1];
            if (op == 21 && len == 4 && p[2] == 32 && p[3] == 1) m.sint = p[1];
            if (op == 22 && len == 3 && p[2] == 32) m.floating = p[1];
            if (op == 23 && len == 4) { id->scalar = p[2]; id->lanes = p[3]; }
            if (op == 25 && len >= 9) { id->dimension = p[3]; id->array = p[5]; }
            if (op == 27 && len == 3 && p[2] < m.bound) id->image = p[2];
        } else if (len >= 3 && p[1] < m.bound && m.ids[p[1]].is_type && p[2] < m.bound) m.ids[p[2]].type = p[1];
        if (op == 54 && functions == count) functions = i;
        if ((op == 61 || op == 83 || op == 100) && len >= 4 && p[2] < m.bound && p[3] < m.bound)
            m.ids[p[2]].binding = m.ids[p[3]].binding;
        if (op == 86 && len == 5 && p[2] < m.bound && p[4] < m.bound) m.ids[p[2]].binding = m.ids[p[4]].binding;
        i += len;
    }
    if (!m.floating || functions == count) goto fail;
    if (!m.code.boolean) { m.code.boolean = m.code.next++; REDUCTION_GLOBAL(&m, 20, m.code.boolean); }
    if (!m.sint) { m.sint = m.code.next++; REDUCTION_GLOBAL(&m, 21, m.sint, 32, 1); }
    if (!m.glsl) { m.glsl = m.code.next++; needs_glsl = 1; }
    m.float2 = reduction_vector(&m, m.floating, 2); m.float3 = reduction_vector(&m, m.floating, 3); m.float4 = reduction_vector(&m, m.floating, 4);
    m.int2 = reduction_vector(&m, m.sint, 2); m.int3 = reduction_vector(&m, m.sint, 3);
    m.bool2 = reduction_vector(&m, m.code.boolean, 2); m.code.bool4 = reduction_vector(&m, m.code.boolean, 4);
    for (size_t i = functions; i < count;) {
        const uint32_t *p = words + i; unsigned len = p[0] >> 16, op = p[0] & 0xffff;
        unsigned from = len, end = len;
        switch (op) {
            case 57: case 169: from = 4; break; // function call or select
            case 245: from = 3; break; // phi, including back-edge inputs
            case 62: case 63: case 64: from = 1; end = 3; break;
            case 65: case 66: case 67: case 70: from = 3; end = 4; break;
            case 254: from = 1; break;
        }
        if (end > len) goto fail;
        for (unsigned arg = from; arg < end; arg++) if (p[arg] < m.bound && m.ids[p[arg]].binding) {
            *reason = "reduction sampler escapes its direct binding"; goto fail;
        }
        uint32_t binding = len >= 4 && p[3] < m.bound ? m.ids[p[3]].binding : 0;
        if (binding && ((op >= 87 && op <= 94) || op == 96 || op == 97 || op == 105 || (op >= 305 && op <= 315))) {
            *reason = "reduction currently needs a direct 2D float sample with explicit LOD";
            if (op != 88 || !reduction_sample(&m, p, len, filters + binding - 1)) goto fail;
        } else if (!texel_append(&m.code.body, p, len)) goto fail;
        i += len;
    }
    if (m.failed || m.code.next > 0x3fffff || !texel_append(&out, words, 5)) goto fail;
    if (!has_queries && !texel_append(&out, (uint32_t[]){2u<<16|17, 50}, 2)) goto fail;
    for (size_t i = 5; i < functions;) {
        unsigned len = words[i] >> 16;
        if (needs_glsl && (words[i] & 0xffff) == 14) {
            uint32_t import[] = {6u<<16|11, m.glsl, 0, 0, 0, 0};
            memcpy(import + 2, "GLSL.std.450", 12);
            if (!texel_append(&out, import, 6)) goto fail;
        }
        if (!texel_append(&out, words + i, len)) goto fail;
        i += len;
    }
    if (!texel_append(&out, m.code.globals.w, m.code.globals.n) || !texel_append(&out, m.code.body.w, m.code.body.n)) goto fail;
    out.w[3] = m.code.next; *out_count = out.n; *reason = NULL;
    free(m.ids); free(m.code.globals.w); free(m.code.body.w); return out.w;
fail:
    free(m.ids); free(m.code.globals.w); free(m.code.body.w); free(out.w); return NULL;
}
#undef REDUCTION_VALUE
#undef REDUCTION_GLOBAL
#undef REDUCTION_EXT
